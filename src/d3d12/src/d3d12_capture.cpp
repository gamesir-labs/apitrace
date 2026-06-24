#include "apitrace/d3d12_capture.hpp"

#include "apitrace/asset_index.hpp"
#include "apitrace/capture_runtime.hpp"
#include "apitrace/d3d12_raw_sink.hpp"
#include "apitrace/event_types.hpp"
#include "apitrace/raw_event_codec.hpp"
#include "apitrace/trace_session.hpp"

#ifndef CINTERFACE
#define CINTERFACE
#endif
#include <d3d12.h>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace apitrace::d3d12 {
namespace {

enum class PipelineStateSubobjectType : std::uint32_t {
  RootSignature = 0x0,
  VS = 0x1,
  PS = 0x2,
  DS = 0x3,
  HS = 0x4,
  GS = 0x5,
  CS = 0x6,
  StreamOutput = 0x7,
  Blend = 0x8,
  SampleMask = 0x9,
  Rasterizer = 0xa,
  DepthStencil = 0xb,
  InputLayout = 0xc,
  IbStripCutValue = 0xd,
  PrimitiveTopology = 0xe,
  RenderTargetFormats = 0xf,
  DepthStencilFormat = 0x10,
  SampleDesc = 0x11,
  NodeMask = 0x12,
  CachedPso = 0x13,
  Flags = 0x14,
  DepthStencil1 = 0x15,
  ViewInstancing = 0x16,
  AS = 0x18,
  MS = 0x19,
};

struct DepthStencilDesc1 {
  WINBOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  WINBOOL StencilEnable;
  UINT8 StencilReadMask;
  UINT8 StencilWriteMask;
  D3D12_DEPTH_STENCILOP_DESC FrontFace;
  D3D12_DEPTH_STENCILOP_DESC BackFace;
  WINBOOL DepthBoundsTestEnable;
};

struct RtFormatArray {
  DXGI_FORMAT RTFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
  UINT NumRenderTargets;
};

struct ViewInstancingDesc {
  UINT ViewInstanceCount;
  const void *pViewInstanceLocations;
  UINT Flags;
};

std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::uint64_t> g_blob_id{0};
std::mutex g_object_mutex;
std::mutex g_event_order_mutex;
std::mutex g_command_batch_mutex;
std::mutex g_present_mutex;
std::mutex g_shader_asset_memo_mutex;
std::unordered_map<const void *, trace::ObjectId> g_object_ids;
std::unordered_map<const void *, trace::ObjectKind> g_object_kinds;
constexpr std::uint64_t kRootConstantBufferSnapshotBytes =
    std::uint64_t{D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT} * 4u * sizeof(std::uint32_t);
constexpr std::uint64_t kMappedUseSnapshotChunkBytes = 4u * 1024u;
constexpr std::uint64_t kRawUnmapCommitCadenceBytes = 4ull * 1024ull * 1024ull;

struct ResourceGpuVirtualAddressState {
  trace::ObjectId object_id = 0;
  std::uint64_t base = 0;
  std::uint64_t width = 0;
  std::uint64_t create_sequence = 0;
};

struct GpuVirtualAddressResolve {
  trace::ObjectId object_id = 0;
  std::uint64_t offset = 0;
  std::uint64_t width = 0;
  const char *status = "unmapped";
};

std::unordered_map<const void *, ResourceGpuVirtualAddressState> g_resource_gpu_virtual_addresses;

struct ShaderAssetMemoKey {
  const void *data = nullptr;
  std::size_t size = 0;

  bool operator==(const ShaderAssetMemoKey &other) const noexcept
  {
    return data == other.data && size == other.size;
  }
};

struct ShaderAssetMemoKeyHash {
  std::size_t operator()(const ShaderAssetMemoKey &key) const noexcept
  {
    auto hash = std::hash<const void *>{}(key.data);
    hash ^= std::hash<std::size_t>{}(key.size) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct ShaderAssetMemoEntry {
  trace::AssetRecord asset;
  std::string fast_fingerprint;
};

TraceSession *g_shader_asset_memo_session = nullptr;
std::filesystem::path g_shader_asset_memo_bundle_root;
std::uint64_t g_shader_asset_memo_initial_sequence = 0;
std::unordered_map<ShaderAssetMemoKey, ShaderAssetMemoEntry, ShaderAssetMemoKeyHash> g_shader_asset_memo;

void reset_shader_asset_memo_if_needed(TraceSession *session)
{
  if (!session) {
    return;
  }
  const auto initial_sequence = session->initial_call_sequence();
  const auto &bundle_root = session->options().bundle_root;
  std::lock_guard lock(g_shader_asset_memo_mutex);
  if (g_shader_asset_memo_session == session &&
      g_shader_asset_memo_initial_sequence == initial_sequence &&
      g_shader_asset_memo_bundle_root == bundle_root) {
    return;
  }
  g_shader_asset_memo_session = session;
  g_shader_asset_memo_bundle_root = bundle_root;
  g_shader_asset_memo_initial_sequence = initial_sequence;
  g_shader_asset_memo.clear();
}

struct CapturedMappedRange {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t hash = 0;
};

struct FastHash128 {
  std::uint64_t low = 0;
  std::uint64_t high = 0;

  bool operator==(const FastHash128 &other) const noexcept
  {
    return low == other.low && high == other.high;
  }
};

struct RawUnmapSignatureKey {
  const void *resource = nullptr;
  std::uint32_t subresource = 0;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;

  bool operator==(const RawUnmapSignatureKey &other) const noexcept
  {
    return resource == other.resource &&
           subresource == other.subresource &&
           begin == other.begin &&
           end == other.end;
  }
};

struct RawUnmapSignatureKeyHash {
  std::size_t operator()(const RawUnmapSignatureKey &key) const noexcept
  {
    std::uint64_t hash = reinterpret_cast<std::uintptr_t>(key.resource);
    hash ^= (static_cast<std::uint64_t>(key.subresource) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
    hash ^= (key.begin + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
    hash ^= (key.end + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
    return static_cast<std::size_t>(hash);
  }
};

struct RawUnmapCounters {
  std::uint64_t unmap_candidates = 0;
  std::uint64_t unchanged_skipped = 0;
  std::uint64_t emitted_blob_bytes = 0;
  std::uint64_t raw_write_failures = 0;
};

std::mutex g_raw_unmap_mutex;
TraceSession *g_raw_unmap_signature_session = nullptr;
std::filesystem::path g_raw_unmap_signature_bundle_root;
std::uint64_t g_raw_unmap_signature_initial_sequence = 0;
std::unordered_map<RawUnmapSignatureKey, FastHash128, RawUnmapSignatureKeyHash> g_raw_unmap_signatures;
RawUnmapCounters g_raw_unmap_counters;

void reset_raw_unmap_signatures_if_needed(TraceSession *session)
{
  if (!session) {
    return;
  }
  const auto initial_sequence = session->initial_call_sequence();
  const auto &bundle_root = session->options().bundle_root;
  std::lock_guard lock(g_raw_unmap_mutex);
  if (g_raw_unmap_signature_session == session &&
      g_raw_unmap_signature_initial_sequence == initial_sequence &&
      g_raw_unmap_signature_bundle_root == bundle_root) {
    return;
  }
  g_raw_unmap_signature_session = session;
  g_raw_unmap_signature_bundle_root = bundle_root;
  g_raw_unmap_signature_initial_sequence = initial_sequence;
  g_raw_unmap_signatures.clear();
}

struct MappedResourceState {
  const void *data = nullptr;
  std::uint32_t subresource = 0;
  std::vector<CapturedMappedRange> captured_ranges;
};

std::unordered_map<const void *, MappedResourceState> g_mapped_resources;

// --- Frame-end re-capture of THIS FRAME's mapped root-CBV ranges (env-gated, bounded) ---
// Defeats a stale submit-time snapshot of a DYNAMICALLY INDEXED mapped cbuffer (the FH4
// homepage CG composite reads a per-element transform palette via TEXCOORD-as-index; its
// palette entry settles after submit relative to the async GPU read, so the submit-time
// snapshot records zero -> composite off-screen in retrace -> CG never composites). We
// re-read only the ranges USED THIS FRAME at Present (after writes settle); bounded +
// deduped + cleared every present so this never degenerates into the unbounded
// whole-history rehash. Default OFF: zero cost / zero behavior change for normal play.
inline bool present_recapture_enabled() {
  static const bool enabled = [] {
    const char *v = std::getenv("DXMT_APITRACE_PRESENT_RECAPTURE");
    return v && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}
struct PresentRecaptureRange {
  const void *resource = nullptr;
  trace::ObjectId object_id = 0;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
};
std::vector<PresentRecaptureRange> g_present_recapture_ranges; // guarded by g_object_mutex
std::unordered_set<std::uint64_t> g_present_recapture_seen;    // guarded by g_object_mutex
constexpr std::size_t kPresentRecaptureMaxRanges = 2048;
// caller must hold g_object_mutex
inline void note_present_recapture_range(const void *resource, trace::ObjectId object_id,
                                         std::uint64_t begin, std::uint64_t end) {
  if (!present_recapture_enabled() ||
      g_present_recapture_ranges.size() >= kPresentRecaptureMaxRanges) {
    return;
  }
  const std::uint64_t key =
      (reinterpret_cast<std::uintptr_t>(resource) * 0x9e3779b97f4a7c15ull) ^ begin;
  if (!g_present_recapture_seen.insert(key).second) {
    return;
  }
  g_present_recapture_ranges.push_back({resource, object_id, begin, end});
}

struct MappedGpuvaUseRange {
  std::uint64_t address = 0;
  std::uint64_t size = 0;
  bool valid = false;
};

struct CommandListMappedUseState {
  std::unordered_map<std::uint32_t, MappedGpuvaUseRange> graphics_root_cbvs;
  std::unordered_map<std::uint32_t, MappedGpuvaUseRange> compute_root_cbvs;
  std::vector<MappedGpuvaUseRange> vertex_buffers;
  MappedGpuvaUseRange index_buffer;
};

std::mutex g_command_list_mapped_use_mutex;
std::unordered_map<const void *, CommandListMappedUseState> g_command_list_mapped_uses;

struct CopyBufferBatchOp {
  std::uint64_t sequence = 0;
  std::string function_name;
  const void *dst_buffer = nullptr;
  std::uint64_t dst_offset = 0;
  const void *src_buffer = nullptr;
  std::uint64_t src_offset = 0;
  std::uint64_t byte_count = 0;
};

struct PendingCopyBufferBatch {
  std::vector<CopyBufferBatchOp> ops;
};

struct CopyBufferBatchFlush {
  const void *command_list = nullptr;
  PendingCopyBufferBatch batch;
};

struct ResourceBarrierBatchOp {
  std::uint64_t sequence = 0;
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  trace::ObjectId resource_object_id = 0;
  std::uint32_t before = 0;
  std::uint32_t after = 0;
  std::uint32_t subresource = 0;
  trace::ObjectId resource_before_object_id = 0;
  trace::ObjectId resource_after_object_id = 0;
};

struct PendingResourceBarrierBatch {
  std::vector<ResourceBarrierBatchOp> ops;
};

struct ResourceBarrierBatchFlush {
  const void *command_list = nullptr;
  PendingResourceBarrierBatch batch;
};

struct TextureCopyLocationCompact {
  trace::ObjectId resource_object_id = 0;
  std::uint32_t type = 0;
  std::uint32_t subresource_index = 0;
  std::uint64_t footprint_offset = 0;
  std::uint32_t footprint_format = 0;
  std::uint32_t footprint_width = 0;
  std::uint32_t footprint_height = 0;
  std::uint32_t footprint_depth = 0;
  std::uint32_t footprint_row_pitch = 0;
};

struct CopyTextureRegionBatchOp {
  std::uint64_t sequence = 0;
  TextureCopyLocationCompact dst;
  std::uint32_t dst_x = 0;
  std::uint32_t dst_y = 0;
  std::uint32_t dst_z = 0;
  TextureCopyLocationCompact src;
  bool has_src_box = false;
  std::uint32_t src_box_left = 0;
  std::uint32_t src_box_top = 0;
  std::uint32_t src_box_front = 0;
  std::uint32_t src_box_right = 0;
  std::uint32_t src_box_bottom = 0;
  std::uint32_t src_box_back = 0;
};

struct PendingCopyTextureRegionBatch {
  std::vector<CopyTextureRegionBatchOp> ops;
};

struct CopyTextureRegionBatchFlush {
  const void *command_list = nullptr;
  PendingCopyTextureRegionBatch batch;
};

struct FenceDependencyOp {
  std::uint64_t sequence = 0;
  std::string scope;
  std::uint64_t d3d_sequence = 0;
  std::uint64_t encoder_id = 0;
  bool implicit_pre_raster_wait = false;
  std::uint32_t strong_count = 0;
  std::uint32_t full_count = 0;
  std::uint32_t minimal_count = 0;
  std::uint32_t mask_count = 0;
};

struct PendingFenceDependencyBatch {
  std::vector<FenceDependencyOp> ops;
};

struct DescriptorViewOp {
  std::uint64_t sequence = 0;
  std::uint32_t kind = 0;
  trace::ObjectId device_object_id = 0;
  trace::ObjectId resource_object_id = 0;
  trace::ObjectId counter_resource_object_id = 0;
  std::uint64_t descriptor = 0;
  std::uint32_t format = 0;
  std::uint32_t view_dimension = 0;
  std::uint32_t shader_4_component_mapping = 0;
  std::uint32_t flags = 0;
  std::uint64_t buffer_location = 0;
  std::uint32_t size_in_bytes = 0;
  std::string gpuva_resolve_status;
  trace::ObjectId resolved_resource_object_id = 0;
  std::uint64_t resolved_resource_offset = 0;
  std::uint64_t resolved_resource_width = 0;
  std::string view_payload;
};

struct PendingDescriptorViewBatch {
  std::vector<DescriptorViewOp> ops;
};

struct CopyDescriptorRange {
  std::uint64_t dst_descriptor = 0;
  std::uint64_t src_descriptor = 0;
  std::uint32_t count = 0;
};

struct CapturedCbvDescriptor {
  std::uint64_t buffer_location = 0;
  std::uint32_t size_in_bytes = 0;
};

struct CbvDescriptorCopy {
  std::uint64_t dst_descriptor = 0;
  bool has_cbv = false;
  CapturedCbvDescriptor cbv;
};

struct CopyDescriptorOp {
  std::uint64_t sequence = 0;
  trace::ObjectId device_object_id = 0;
  std::uint32_t descriptor_heap_type = 0;
  std::uint32_t descriptor_size = 0;
  std::uint32_t dst_range_count = 0;
  std::uint32_t src_range_count = 0;
  std::vector<CopyDescriptorRange> ranges;
};

struct PendingCopyDescriptorBatch {
  std::vector<CopyDescriptorOp> ops;
};

enum class PendingBatchKind {
  CopyBuffer,
  ResourceBarrier,
  CopyTextureRegion,
  FenceDependency,
  DescriptorView,
  CopyDescriptor,
};

struct PendingBatchFlush {
  PendingBatchKind kind = PendingBatchKind::CopyBuffer;
  std::uint64_t sequence = UINT64_MAX;
  const void *command_list = nullptr;
  PendingCopyBufferBatch copy_buffer;
  PendingResourceBarrierBatch resource_barrier;
  PendingCopyTextureRegionBatch copy_texture_region;
  PendingFenceDependencyBatch fence_dependency;
  PendingDescriptorViewBatch descriptor_view;
  PendingCopyDescriptorBatch copy_descriptor;
};

std::unordered_map<const void *, PendingCopyBufferBatch> g_copy_buffer_batches;
std::unordered_map<const void *, PendingResourceBarrierBatch> g_resource_barrier_batches;
std::unordered_map<const void *, PendingCopyTextureRegionBatch> g_copy_texture_region_batches;
std::mutex g_diagnostic_batch_mutex;
std::mutex g_descriptor_metadata_mutex;
std::unordered_map<std::uint64_t, CapturedCbvDescriptor> g_cbv_descriptors;
PendingFenceDependencyBatch g_fence_dependency_batch;
PendingDescriptorViewBatch g_descriptor_view_batch;
PendingCopyDescriptorBatch g_copy_descriptor_batch;
TraceSession *g_present_session = nullptr;
std::uint64_t g_present_frame_index = 0;
constexpr std::size_t kDescriptorViewBatchMaxOps = 1024;
constexpr std::size_t kCopyDescriptorBatchMaxOps = 1024;
constexpr std::size_t kCommandListBatchMaxOps = 1024;

std::uint64_t env_u64(const char *name)
{
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return 0;
  }
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value) {
    return 0;
  }
  return static_cast<std::uint64_t>(parsed);
}

struct SealCheckpointTriggers {
  std::uint64_t after_frame = 0;
  std::uint64_t every_frames = 0;
  bool has_after_frame = false;
};

const SealCheckpointTriggers &seal_checkpoint_triggers()
{
  static const SealCheckpointTriggers triggers = [] {
    SealCheckpointTriggers result;
    if (const char *after = std::getenv("APITRACE_D3D12_SEAL_CHECKPOINT_AFTER_FRAME");
        after && *after) {
      char *end = nullptr;
      const unsigned long long parsed = std::strtoull(after, &end, 10);
      if (end != after) {
        result.after_frame = static_cast<std::uint64_t>(parsed);
        result.has_after_frame = true;
      }
    }
    result.every_frames = env_u64("APITRACE_D3D12_SEAL_CHECKPOINT_EVERY_FRAMES");
    return result;
  }();
  return triggers;
}

void maybe_seal_checkpoint_after_present(std::uint64_t frame_index)
{
  if (frame_index == UINT64_MAX) {
    return;
  }

  const auto &triggers = seal_checkpoint_triggers();
  const bool after_frame_hit = triggers.has_after_frame && frame_index == triggers.after_frame;
  const bool every_frames_hit =
      triggers.every_frames != 0 &&
      (frame_index + 1) >= triggers.every_frames &&
      ((frame_index + 1) % triggers.every_frames) == 0;
  if (!after_frame_hit && !every_frames_hit) {
    return;
  }

  static std::atomic<bool> after_frame_sealed{false};
  static std::atomic<bool> seal_in_progress{false};
  if (seal_in_progress.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (after_frame_hit) {
    bool expected = false;
    if (!after_frame_sealed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      if (!every_frames_hit) {
        seal_in_progress.store(false, std::memory_order_release);
        return;
      }
    }
  }
  runtime::seal_process_trace_session_checkpoint();
  seal_in_progress.store(false, std::memory_order_release);
}

std::uint32_t fence_dependency_scope_id(std::string_view scope)
{
  if (scope == "blit") {
    return 1;
  }
  if (scope == "clear") {
    return 2;
  }
  if (scope == "render_vertex") {
    return 3;
  }
  if (scope == "render_fragment") {
    return 4;
  }
  if (scope == "compute") {
    return 5;
  }
  return 0;
}

std::uint32_t descriptor_view_kind_for_function(const char *function_name)
{
  if (!function_name) {
    return 0;
  }
  if (std::strcmp(function_name, "ID3D12Device::CreateConstantBufferView") == 0) {
    return 1;
  }
  if (std::strcmp(function_name, "ID3D12Device::CreateShaderResourceView") == 0) {
    return 2;
  }
  if (std::strcmp(function_name, "ID3D12Device::CreateUnorderedAccessView") == 0) {
    return 3;
  }
  if (std::strcmp(function_name, "ID3D12Device::CreateRenderTargetView") == 0) {
    return 4;
  }
  if (std::strcmp(function_name, "ID3D12Device::CreateDepthStencilView") == 0) {
    return 5;
  }
  return 0;
}

trace::ApiKind classify_api(const char *opname)
{
  if (!opname) {
    return trace::ApiKind::Unknown;
  }
  if (std::strncmp(opname, "ID3D11", 6) == 0 || std::strncmp(opname, "D3D11", 5) == 0) {
    return trace::ApiKind::D3D11;
  }
  if (std::strncmp(opname, "ID3D12", 6) == 0 || std::strncmp(opname, "D3D12", 5) == 0) {
    return trace::ApiKind::D3D12;
  }
  if (std::strncmp(opname, "IDXGI", 5) == 0 || std::strncmp(opname, "DXGI", 4) == 0) {
    return trace::ApiKind::D3D12;
  }
  return trace::ApiKind::Unknown;
}

std::string escape_json_string(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (const unsigned char ch : text) {
    switch (ch) {
    case '\"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (ch < 0x20) {
        escaped += "?";
      } else {
        escaped.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  return escaped;
}

std::string root_signature_descriptor_tables_json(const D3D12_ROOT_SIGNATURE_DESC *desc)
{
  std::ostringstream payload;
  payload << "[";
  bool first_table = true;
  if (desc && desc->pParameters) {
    for (UINT parameter_index = 0; parameter_index < desc->NumParameters; ++parameter_index) {
      const auto &parameter = desc->pParameters[parameter_index];
      if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        continue;
      }
      if (!first_table) {
        payload << ",";
      }
      first_table = false;
      payload << "{\"root_parameter_index\":" << parameter_index
              << ",\"shader_visibility\":" << static_cast<unsigned int>(parameter.ShaderVisibility)
              << ",\"ranges\":[";
      UINT next_offset = 0;
      for (UINT range_index = 0; range_index < parameter.DescriptorTable.NumDescriptorRanges; ++range_index) {
        const auto &range = parameter.DescriptorTable.pDescriptorRanges[range_index];
        if (range_index) {
          payload << ",";
        }
        const UINT offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                ? next_offset
                                : range.OffsetInDescriptorsFromTableStart;
        payload << "{\"type\":" << static_cast<unsigned int>(range.RangeType)
                << ",\"descriptor_count\":" << range.NumDescriptors
                << ",\"base_shader_register\":" << range.BaseShaderRegister
                << ",\"register_space\":" << range.RegisterSpace
                << ",\"offset_from_table_start\":" << offset
                << ",\"flags\":0}";
        if (range.NumDescriptors != UINT_MAX &&
            offset <= UINT_MAX - range.NumDescriptors) {
          next_offset = offset + range.NumDescriptors;
        }
      }
      payload << "]}";
    }
  }
  payload << "]";
  return payload.str();
}

std::string root_signature_parameters_json(const D3D12_ROOT_SIGNATURE_DESC *desc)
{
  std::ostringstream payload;
  payload << "[";
  if (desc && desc->pParameters) {
    for (UINT parameter_index = 0; parameter_index < desc->NumParameters; ++parameter_index) {
      const auto &parameter = desc->pParameters[parameter_index];
      if (parameter_index) {
        payload << ",";
      }
      payload << "{\"root_parameter_index\":" << parameter_index
              << ",\"parameter_type\":" << static_cast<unsigned int>(parameter.ParameterType)
              << ",\"shader_visibility\":" << static_cast<unsigned int>(parameter.ShaderVisibility);
      if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        payload << ",\"range_count\":" << parameter.DescriptorTable.NumDescriptorRanges;
      } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        payload << ",\"shader_register\":" << parameter.Constants.ShaderRegister
                << ",\"register_space\":" << parameter.Constants.RegisterSpace
                << ",\"num_32bit_values\":" << parameter.Constants.Num32BitValues;
      } else {
        payload << ",\"shader_register\":" << parameter.Descriptor.ShaderRegister
                << ",\"register_space\":" << parameter.Descriptor.RegisterSpace;
      }
      payload << "}";
    }
  }
  payload << "]";
  return payload.str();
}

trace::ObjectKind to_trace_object_kind(CaptureObjectKind kind)
{
  switch (kind) {
  case CaptureObjectKind::Device:
    return trace::ObjectKind::Device;
  case CaptureObjectKind::CommandQueue:
    return trace::ObjectKind::CommandQueue;
  case CaptureObjectKind::CommandAllocator:
    return trace::ObjectKind::CommandAllocator;
  case CaptureObjectKind::CommandList:
    return trace::ObjectKind::CommandList;
  case CaptureObjectKind::CommandSignature:
    return trace::ObjectKind::CommandSignature;
  case CaptureObjectKind::Fence:
    return trace::ObjectKind::Fence;
  case CaptureObjectKind::SwapChain:
    return trace::ObjectKind::SwapChain;
  case CaptureObjectKind::Heap:
    return trace::ObjectKind::Heap;
  case CaptureObjectKind::Resource:
    return trace::ObjectKind::Resource;
  case CaptureObjectKind::View:
    return trace::ObjectKind::View;
  case CaptureObjectKind::Shader:
    return trace::ObjectKind::Shader;
  case CaptureObjectKind::PipelineState:
    return trace::ObjectKind::PipelineState;
  case CaptureObjectKind::RootSignature:
    return trace::ObjectKind::RootSignature;
  case CaptureObjectKind::DescriptorHeap:
    return trace::ObjectKind::DescriptorHeap;
  case CaptureObjectKind::QueryHeap:
    return trace::ObjectKind::QueryHeap;
  case CaptureObjectKind::Unknown:
  default:
    return trace::ObjectKind::Unknown;
  }
}

TraceSession *session_for(trace::ApiKind api)
{
  auto *session = runtime::ensure_process_trace_session(api);
  if (session && api == trace::ApiKind::D3D12) {
    const auto initial_sequence = session->initial_call_sequence();
    reset_raw_unmap_signatures_if_needed(session);
    reset_shader_asset_memo_if_needed(session);
    auto current = g_sequence.load(std::memory_order_relaxed);
    while (current < initial_sequence &&
           !g_sequence.compare_exchange_weak(current, initial_sequence, std::memory_order_relaxed)) {
    }
  }
  return session;
}

std::uint64_t next_present_frame_index()
{
  auto *session = session_for(trace::ApiKind::D3D12);
  std::lock_guard lock(g_present_mutex);
  if (g_present_session != session) {
    g_present_session = session;
    g_present_frame_index = 0;
  }
  return g_present_frame_index++;
}

bool is_present_test(std::uint32_t flags)
{
  return (flags & DXGI_PRESENT_TEST) != 0;
}

trace::ObjectId lookup_object_id_locked(const void *object)
{
  if (!object) {
    return 0;
  }
  auto found = g_object_ids.find(object);
  if (found != g_object_ids.end()) {
    return found->second;
  }
  const auto id = static_cast<trace::ObjectId>(reinterpret_cast<std::uintptr_t>(object));
  g_object_ids.emplace(object, id);
  return id;
}

bool object_kind_known_locked(const void *object, trace::ObjectKind kind)
{
  if (!object) {
    return true;
  }
  const auto found = g_object_kinds.find(object);
  return found != g_object_kinds.end() && found->second == kind;
}

trace::ObjectId existing_object_id_locked(const void *object)
{
  if (!object) {
    return 0;
  }
  const auto found = g_object_ids.find(object);
  return found == g_object_ids.end() ? 0 : found->second;
}

GpuVirtualAddressResolve resolve_gpu_virtual_address_locked(std::uint64_t address)
{
  GpuVirtualAddressResolve resolve;
  if (address == 0) {
    resolve.status = "null";
    return resolve;
  }

  const ResourceGpuVirtualAddressState *best = nullptr;
  for (const auto &[resource, state] : g_resource_gpu_virtual_addresses) {
    (void)resource;
    if (state.base == 0 || address < state.base) {
      continue;
    }
    const auto offset = address - state.base;
    if (offset >= state.width) {
      continue;
    }
    if (!best || state.create_sequence > best->create_sequence) {
      best = &state;
    }
  }

  if (!best) {
    return resolve;
  }

  resolve.object_id = best->object_id;
  resolve.offset = address - best->base;
  resolve.width = best->width;
  resolve.status = "mapped";
  return resolve;
}

void append_gpu_virtual_address_resolve_json(std::ostringstream &payload, const GpuVirtualAddressResolve &resolve)
{
  payload << ",\"gpuva_resolve_status\":\"" << resolve.status << "\""
          << ",\"resolved_resource_object_id\":" << resolve.object_id
          << ",\"resolved_resource_offset\":" << resolve.offset
          << ",\"resolved_resource_width\":" << resolve.width;
}

void append_gpu_virtual_address_binding_json(std::ostringstream &payload, const char *key, std::uint64_t address)
{
  std::lock_guard lock(g_object_mutex);
  payload << "\"" << key << "\":" << address;
  append_gpu_virtual_address_resolve_json(payload, resolve_gpu_virtual_address_locked(address));
}

trace::AssetRecord register_asset_bytes(
    trace::AssetKind kind,
    const char *debug_name,
    const void *data,
    std::size_t size);

void record_call_event_unbatched(
    std::uint64_t sequence,
    const char *opname,
    const char *payload_json,
    const void *const *object_refs,
    std::uint32_t object_ref_count,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code);

void record_call_event_unbatched_with_object_ids(
    std::uint64_t sequence,
    const char *opname,
    const char *payload_json,
    std::vector<trace::ObjectId> object_refs,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code);

void flush_diagnostic_batches();
void flush_command_list_batches(const void *command_list = nullptr);

std::uint64_t fnv1a64_bytes(const void *data, std::size_t size)
{
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kPrime;
  }
  return hash;
}

std::uint64_t rotl64(std::uint64_t value, int bits) noexcept
{
  return (value << bits) | (value >> (64 - bits));
}

std::uint64_t read_le64_unaligned(const std::uint8_t *bytes) noexcept
{
  std::uint64_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  value = ((value & 0x00000000000000ffull) << 56) |
          ((value & 0x000000000000ff00ull) << 40) |
          ((value & 0x0000000000ff0000ull) << 24) |
          ((value & 0x00000000ff000000ull) << 8) |
          ((value & 0x000000ff00000000ull) >> 8) |
          ((value & 0x0000ff0000000000ull) >> 24) |
          ((value & 0x00ff000000000000ull) >> 40) |
          ((value & 0xff00000000000000ull) >> 56);
#endif
  return value;
}

std::uint64_t fmix64(std::uint64_t value) noexcept
{
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33;
  return value;
}

FastHash128 fast_hash128_bytes(const void *data, std::size_t size) noexcept
{
  constexpr std::uint64_t kC1 = 0x87c37b91114253d5ull;
  constexpr std::uint64_t kC2 = 0x4cf5ad432745937full;
  auto h1 = 0x9368e53c2f6af274ull ^ static_cast<std::uint64_t>(size);
  auto h2 = 0x586dcd208f7cd3fdull ^ (static_cast<std::uint64_t>(size) << 1);
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  const auto block_count = size / 16;
  for (std::size_t block = 0; block < block_count; ++block) {
    auto k1 = read_le64_unaligned(bytes + block * 16);
    auto k2 = read_le64_unaligned(bytes + block * 16 + 8);
    k1 *= kC1;
    k1 = rotl64(k1, 31);
    k1 *= kC2;
    h1 ^= k1;
    h1 = rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;

    k2 *= kC2;
    k2 = rotl64(k2, 33);
    k2 *= kC1;
    h2 ^= k2;
    h2 = rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  auto k1 = std::uint64_t{0};
  auto k2 = std::uint64_t{0};
  const auto *tail = bytes + block_count * 16;
  switch (size & 15u) {
  case 15:
    k2 ^= static_cast<std::uint64_t>(tail[14]) << 48;
    [[fallthrough]];
  case 14:
    k2 ^= static_cast<std::uint64_t>(tail[13]) << 40;
    [[fallthrough]];
  case 13:
    k2 ^= static_cast<std::uint64_t>(tail[12]) << 32;
    [[fallthrough]];
  case 12:
    k2 ^= static_cast<std::uint64_t>(tail[11]) << 24;
    [[fallthrough]];
  case 11:
    k2 ^= static_cast<std::uint64_t>(tail[10]) << 16;
    [[fallthrough]];
  case 10:
    k2 ^= static_cast<std::uint64_t>(tail[9]) << 8;
    [[fallthrough]];
  case 9:
    k2 ^= static_cast<std::uint64_t>(tail[8]);
    k2 *= kC2;
    k2 = rotl64(k2, 33);
    k2 *= kC1;
    h2 ^= k2;
    [[fallthrough]];
  case 8:
    k1 ^= static_cast<std::uint64_t>(tail[7]) << 56;
    [[fallthrough]];
  case 7:
    k1 ^= static_cast<std::uint64_t>(tail[6]) << 48;
    [[fallthrough]];
  case 6:
    k1 ^= static_cast<std::uint64_t>(tail[5]) << 40;
    [[fallthrough]];
  case 5:
    k1 ^= static_cast<std::uint64_t>(tail[4]) << 32;
    [[fallthrough]];
  case 4:
    k1 ^= static_cast<std::uint64_t>(tail[3]) << 24;
    [[fallthrough]];
  case 3:
    k1 ^= static_cast<std::uint64_t>(tail[2]) << 16;
    [[fallthrough]];
  case 2:
    k1 ^= static_cast<std::uint64_t>(tail[1]) << 8;
    [[fallthrough]];
  case 1:
    k1 ^= static_cast<std::uint64_t>(tail[0]);
    k1 *= kC1;
    k1 = rotl64(k1, 31);
    k1 *= kC2;
    h1 ^= k1;
    break;
  case 0:
  default:
    break;
  }

  h1 ^= static_cast<std::uint64_t>(size);
  h2 ^= static_cast<std::uint64_t>(size);
  h1 += h2;
  h2 += h1;
  h1 = fmix64(h1);
  h2 = fmix64(h2);
  h1 += h2;
  h2 += h1;
  return {h1, h2};
}

bool raw_unmap_signature_unchanged(
    const RawUnmapSignatureKey &key,
    const FastHash128 &signature)
{
  std::lock_guard lock(g_raw_unmap_mutex);
  ++g_raw_unmap_counters.unmap_candidates;
  const auto found = g_raw_unmap_signatures.find(key);
  if (found != g_raw_unmap_signatures.end() && found->second == signature) {
    ++g_raw_unmap_counters.unchanged_skipped;
    return true;
  }
  return false;
}

void mark_raw_unmap_signature(
    const RawUnmapSignatureKey &key,
    const FastHash128 &signature,
    std::uint64_t emitted_bytes)
{
  std::lock_guard lock(g_raw_unmap_mutex);
  g_raw_unmap_signatures[key] = signature;
  g_raw_unmap_counters.emitted_blob_bytes += emitted_bytes;
}

void note_raw_unmap_write_failure()
{
  std::lock_guard lock(g_raw_unmap_mutex);
  ++g_raw_unmap_counters.raw_write_failures;
}

bool record_rawonly_resource_unmap(
    TraceSession *session,
    const void *resource,
    trace::ObjectId resource_object_id,
    std::uint32_t subresource,
    std::uint64_t written_begin,
    std::uint64_t written_end,
    const void *written_data,
    std::size_t written_size)
{
  if (!session ||
      session->capture_raw_mode() != runtime::CaptureOptions::CaptureRawMode::RawOnly ||
      !session->raw_capture_writer() ||
      !resource ||
      resource_object_id == 0 ||
      !written_data ||
      written_size == 0 ||
      written_end <= written_begin) {
    return false;
  }

  const auto size64 = written_end - written_begin;
  if (size64 != static_cast<std::uint64_t>(written_size)) {
    return false;
  }

  const auto key = RawUnmapSignatureKey{resource, subresource, written_begin, written_end};
  const auto signature = fast_hash128_bytes(written_data, written_size);
  if (raw_unmap_signature_unchanged(key, signature)) {
    return true;
  }

  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto *source_bytes = static_cast<const std::uint8_t *>(written_data);
  std::vector<std::uint8_t> copied_bytes(source_bytes, source_bytes + written_size);
  RawSink sink(session->raw_capture_writer(), kRawUnmapCommitCadenceBytes);
  const auto raw_blob_id = sink.append_blob_for_event(
      copied_bytes.data(),
      static_cast<std::uint64_t>(copied_bytes.size()),
      trace::raw::RawBlobKind::Buffer,
      sequence);
  if (raw_blob_id == trace::raw::kInvalidRawBlobId) {
    note_raw_unmap_write_failure();
    return true;
  }

  const auto payload = trace::raw::encode_resource_unmap_payload(
      resource_object_id,
      raw_blob_id,
      written_begin,
      written_end);
  if (!sink.append_binary_event(sequence, trace::raw::RawEventOpcode::ResourceUnmap, payload)) {
    note_raw_unmap_write_failure();
    return true;
  }

  mark_raw_unmap_signature(key, signature, static_cast<std::uint64_t>(written_size));
  return true;
}

bool mapped_range_captured(const MappedResourceState &mapped, std::uint64_t begin, std::uint64_t end, std::uint64_t hash)
{
  for (const auto &range : mapped.captured_ranges) {
    if (range.begin == begin && range.end == end && range.hash == hash) {
      return true;
    }
  }
  return false;
}

void mark_mapped_range_captured(MappedResourceState &mapped, std::uint64_t begin, std::uint64_t end, std::uint64_t hash)
{
  if (end <= begin) {
    return;
  }
  for (auto &range : mapped.captured_ranges) {
    if (range.begin == begin && range.end == end) {
      range.hash = hash;
      return;
    }
  }
  mapped.captured_ranges.push_back(CapturedMappedRange{begin, end, hash});
}

const void *resource_from_object_id_locked(trace::ObjectId object_id)
{
  if (object_id == 0) {
    return nullptr;
  }
  for (const auto &[resource, state] : g_resource_gpu_virtual_addresses) {
    if (state.object_id == object_id) {
      return resource;
    }
  }
  return nullptr;
}

void record_mapped_resource_range_update_unbatched(
    const void *resource,
    MappedResourceState &mapped,
    trace::ObjectId resource_object_id,
    std::uint64_t begin,
    std::uint64_t end)
{
  if (!resource || !mapped.data || resource_object_id == 0 || end <= begin) {
    return;
  }

  const auto size64 = end - begin;
  if (size64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return;
  }

  const auto size = static_cast<std::size_t>(size64);
  const auto *bytes = static_cast<const std::uint8_t *>(mapped.data) + static_cast<std::size_t>(begin);
  if (auto *session = session_for(trace::ApiKind::D3D12);
      session && session->capture_raw_mode() == runtime::CaptureOptions::CaptureRawMode::RawOnly) {
    record_rawonly_resource_unmap(
        session,
        resource,
        resource_object_id,
        mapped.subresource,
        begin,
        end,
        bytes,
        size);
    return;
  }

  const auto hash = fnv1a64_bytes(bytes, size);
  if (mapped_range_captured(mapped, begin, end, hash)) {
    return;
  }

  const auto asset = register_asset_bytes(
      trace::AssetKind::Buffer,
      "d3d12-mapped-resource-use",
      bytes,
      size);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  const auto buffer_path = asset.relative_path.generic_string();
  std::string payload;
  payload.reserve(224 + buffer_path.size());
  payload += "{\"resource_object_id\":";
  payload += std::to_string(resource_object_id);
  payload += ",\"subresource\":";
  payload += std::to_string(mapped.subresource);
  payload += ",\"written_begin\":";
  payload += std::to_string(begin);
  payload += ",\"written_end\":";
  payload += std::to_string(end);
  payload += ",\"written_size\":";
  payload += std::to_string(size64);
  payload += ",\"buffer_path\":\"";
  payload += buffer_path;
  payload += "\",\"capture_reason\":\"mapped_resource_use\"}";
  const std::uint64_t blob_id = asset.blob_id;
  record_call_event_unbatched_with_object_ids(
      g_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
      "ID3D12Resource::Unmap",
      payload.c_str(),
      {resource_object_id},
      &blob_id,
      1,
      0);
  mark_mapped_range_captured(mapped, begin, end, hash);
}

void record_resource_bytes_snapshot_unbatched(
    trace::ObjectId resource_object_id,
    std::uint64_t begin,
    std::uint64_t end,
    const void *bytes,
    std::uint64_t sequence)
{
  if (resource_object_id == 0 || !bytes || end <= begin) {
    return;
  }

  const auto size64 = end - begin;
  if (size64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return;
  }

  const auto size = static_cast<std::size_t>(size64);
  const auto asset = register_asset_bytes(
      trace::AssetKind::Buffer,
      "d3d12-gpu-cbv-use",
      bytes,
      size);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  const auto buffer_path = asset.relative_path.generic_string();
  std::string payload;
  payload.reserve(224 + buffer_path.size());
  payload += "{\"resource_object_id\":";
  payload += std::to_string(resource_object_id);
  payload += ",\"subresource\":0";
  payload += ",\"written_begin\":";
  payload += std::to_string(begin);
  payload += ",\"written_end\":";
  payload += std::to_string(end);
  payload += ",\"written_size\":";
  payload += std::to_string(size64);
  payload += ",\"buffer_path\":\"";
  payload += buffer_path;
  payload += "\",\"apply_sequence\":";
  payload += std::to_string(sequence);
  payload += ",\"capture_reason\":\"gpu_cbv_use\"}";
  const std::uint64_t blob_id = asset.blob_id;
  record_call_event_unbatched_with_object_ids(
      g_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
      "ID3D12Resource::Unmap",
      payload.c_str(),
      {resource_object_id},
      &blob_id,
      1,
      0);
}

void capture_mapped_resource_range_before_use(const void *resource, std::uint64_t begin, std::uint64_t size)
{
  if (!resource || size == 0) {
    return;
  }

  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  flush_command_list_batches();

  std::lock_guard object_lock(g_object_mutex);
  auto mapped_it = g_mapped_resources.find(resource);
  if (mapped_it == g_mapped_resources.end()) {
    return;
  }

  auto gpuva_it = g_resource_gpu_virtual_addresses.find(resource);
  if (gpuva_it == g_resource_gpu_virtual_addresses.end() ||
      gpuva_it->second.object_id == 0 ||
      gpuva_it->second.width == 0 ||
      begin >= gpuva_it->second.width) {
    return;
  }

  const auto clamped_size = std::min(size, gpuva_it->second.width - begin);
  if (clamped_size == 0 || begin > std::numeric_limits<std::uint64_t>::max() - clamped_size) {
    return;
  }

  record_mapped_resource_range_update_unbatched(
      resource,
      mapped_it->second,
      gpuva_it->second.object_id,
      begin,
      begin + clamped_size);
}

void capture_mapped_resource_chunks_before_use(
    const void *resource,
    std::uint64_t begin,
    std::uint64_t end)
{
  if (!resource || end <= begin) {
    return;
  }

  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  flush_command_list_batches();

  std::lock_guard object_lock(g_object_mutex);
  auto mapped_it = g_mapped_resources.find(resource);
  if (mapped_it == g_mapped_resources.end()) {
    return;
  }

  auto gpuva_it = g_resource_gpu_virtual_addresses.find(resource);
  if (gpuva_it == g_resource_gpu_virtual_addresses.end() ||
      gpuva_it->second.object_id == 0 ||
      gpuva_it->second.width == 0 ||
      begin >= gpuva_it->second.width) {
    return;
  }

  const auto resource_width = gpuva_it->second.width;
  const auto clamped_end = std::min(end, resource_width);
  for (std::uint64_t chunk_begin = begin; chunk_begin < clamped_end;
       chunk_begin += kMappedUseSnapshotChunkBytes) {
    const auto chunk_size = std::min(kMappedUseSnapshotChunkBytes,
                                     clamped_end - chunk_begin);
    if (chunk_size == 0 || chunk_begin > std::numeric_limits<std::uint64_t>::max() - chunk_size) {
      return;
    }
    record_mapped_resource_range_update_unbatched(
        resource,
        mapped_it->second,
        gpuva_it->second.object_id,
        chunk_begin,
        chunk_begin + chunk_size);
    // Remember this frame's root-CBV chunk so Present can re-read the settled value
    // (no-op unless DXMT_APITRACE_PRESENT_RECAPTURE is set).
    note_present_recapture_range(resource, gpuva_it->second.object_id, chunk_begin,
                                 chunk_begin + chunk_size);
  }
}

// Re-snapshot this frame's root-CBV ranges at Present (after the frame's CPU writes
// settle), then clear. Bounded by kPresentRecaptureMaxRanges + dedup; gated OFF by
// default. Caller (record_present) holds g_event_order_mutex; record_mapped_resource_
// range_update_unbatched is "unbatched" so it requires the caller's event lock, which we
// hold transitively, and we take g_object_mutex here (same order as the capture paths).
void maybe_recapture_present_ranges()
{
  if (!present_recapture_enabled()) {
    return;
  }
  std::lock_guard object_lock(g_object_mutex);
  for (const auto &range : g_present_recapture_ranges) {
    auto mapped_it = g_mapped_resources.find(range.resource);
    if (mapped_it == g_mapped_resources.end()) {
      continue;
    }
    record_mapped_resource_range_update_unbatched(
        range.resource, mapped_it->second, range.object_id, range.begin, range.end);
  }
  g_present_recapture_ranges.clear();
  g_present_recapture_seen.clear();
}

void capture_mapped_gpuva_range_before_use(std::uint64_t gpu_virtual_address, std::uint64_t size)
{
  if (gpu_virtual_address == 0 || size == 0) {
    return;
  }

  const void *resource = nullptr;
  std::uint64_t offset = 0;
  {
    std::lock_guard lock(g_object_mutex);
    const auto resolve = resolve_gpu_virtual_address_locked(gpu_virtual_address);
    if (resolve.object_id == 0) {
      return;
    }
    resource = resource_from_object_id_locked(resolve.object_id);
    offset = resolve.offset;
  }
  capture_mapped_resource_range_before_use(resource, offset, size);
}

void capture_mapped_gpuva_range_before_use_chunked(std::uint64_t gpu_virtual_address, std::uint64_t size)
{
  if (gpu_virtual_address == 0 || size == 0) {
    return;
  }

  const void *resource = nullptr;
  std::uint64_t offset = 0;
  {
    std::lock_guard lock(g_object_mutex);
    const auto resolve = resolve_gpu_virtual_address_locked(gpu_virtual_address);
    if (resolve.object_id == 0) {
      return;
    }
    resource = resource_from_object_id_locked(resolve.object_id);
    offset = resolve.offset;
  }
  if (!resource) {
    return;
  }

  if (offset > std::numeric_limits<std::uint64_t>::max() - size) {
    return;
  }
  const auto end = offset + size;
  const auto chunk_mask = kMappedUseSnapshotChunkBytes - 1u;
  if (end > std::numeric_limits<std::uint64_t>::max() - chunk_mask) {
    return;
  }
  const auto begin = offset & ~chunk_mask;
  const auto aligned_end = (end + chunk_mask) & ~chunk_mask;

  capture_mapped_resource_chunks_before_use(resource, begin, aligned_end);
}

void capture_root_cbv_mapped_gpuva_range_before_use(const MappedGpuvaUseRange &range)
{
  if (!range.valid) {
    return;
  }
  capture_mapped_gpuva_range_before_use_chunked(range.address, range.size);
}

void remember_root_cbv_range(
    const void *command_list,
    bool compute,
    std::uint32_t root_parameter_index,
    std::uint64_t gpu_virtual_address)
{
  if (!command_list) {
    return;
  }
  std::lock_guard lock(g_command_list_mapped_use_mutex);
  auto &state = g_command_list_mapped_uses[command_list];
  auto &roots = compute ? state.compute_root_cbvs : state.graphics_root_cbvs;
  if (gpu_virtual_address == 0) {
    roots.erase(root_parameter_index);
    return;
  }
  roots[root_parameter_index] = MappedGpuvaUseRange{
      gpu_virtual_address,
      kRootConstantBufferSnapshotBytes,
      true,
  };
}

void clear_root_cbv_ranges(const void *command_list, bool compute)
{
  if (!command_list) {
    return;
  }
  std::lock_guard lock(g_command_list_mapped_use_mutex);
  auto it = g_command_list_mapped_uses.find(command_list);
  if (it == g_command_list_mapped_uses.end()) {
    return;
  }
  if (compute) {
    it->second.compute_root_cbvs.clear();
  } else {
    it->second.graphics_root_cbvs.clear();
  }
}

void remember_vertex_buffer_ranges(
    const void *command_list,
    std::uint32_t start_slot,
    std::uint32_t view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views)
{
  if (!command_list || view_count == 0) {
    return;
  }
  std::lock_guard lock(g_command_list_mapped_use_mutex);
  auto &buffers = g_command_list_mapped_uses[command_list].vertex_buffers;
  const auto required_size = static_cast<std::size_t>(start_slot) + static_cast<std::size_t>(view_count);
  if (buffers.size() < required_size) {
    buffers.resize(required_size);
  }
  for (std::uint32_t index = 0; index < view_count; ++index) {
    auto &range = buffers[static_cast<std::size_t>(start_slot) + index];
    if (!views || views[index].BufferLocation == 0 || views[index].SizeInBytes == 0) {
      range = {};
      continue;
    }
    range = MappedGpuvaUseRange{views[index].BufferLocation, views[index].SizeInBytes, true};
  }
}

void remember_index_buffer_range(const void *command_list, const D3D12_INDEX_BUFFER_VIEW *view)
{
  if (!command_list) {
    return;
  }
  std::lock_guard lock(g_command_list_mapped_use_mutex);
  auto &range = g_command_list_mapped_uses[command_list].index_buffer;
  if (!view || view->BufferLocation == 0 || view->SizeInBytes == 0) {
    range = {};
    return;
  }
  range = MappedGpuvaUseRange{view->BufferLocation, view->SizeInBytes, true};
}

void clear_command_list_mapped_uses(const void *command_list)
{
  if (!command_list) {
    return;
  }
  std::lock_guard lock(g_command_list_mapped_use_mutex);
  g_command_list_mapped_uses.erase(command_list);
}

CommandListMappedUseState command_list_mapped_use_snapshot(const void *command_list)
{
  if (!command_list) {
    return {};
  }
  std::lock_guard lock(g_command_list_mapped_use_mutex);
  const auto it = g_command_list_mapped_uses.find(command_list);
  return it == g_command_list_mapped_uses.end() ? CommandListMappedUseState{} : it->second;
}

void capture_mapped_gpuva_range_before_use(const MappedGpuvaUseRange &range)
{
  if (!range.valid) {
    return;
  }
  capture_mapped_gpuva_range_before_use(range.address, range.size);
}

void capture_graphics_mapped_inputs_before_use(const void *command_list, bool include_index_buffer)
{
  const auto state = command_list_mapped_use_snapshot(command_list);
  for (const auto &[root_parameter_index, range] : state.graphics_root_cbvs) {
    (void)root_parameter_index;
    capture_root_cbv_mapped_gpuva_range_before_use(range);
  }
  for (const auto &range : state.vertex_buffers) {
    capture_mapped_gpuva_range_before_use(range);
  }
  if (include_index_buffer) {
    capture_mapped_gpuva_range_before_use(state.index_buffer);
  }
}

void capture_compute_mapped_inputs_before_use(const void *command_list)
{
  const auto state = command_list_mapped_use_snapshot(command_list);
  for (const auto &[root_parameter_index, range] : state.compute_root_cbvs) {
    (void)root_parameter_index;
    capture_root_cbv_mapped_gpuva_range_before_use(range);
  }
}

void capture_command_list_mapped_inputs_before_submit(const void *command_list)
{
  capture_graphics_mapped_inputs_before_use(command_list, true);
  capture_compute_mapped_inputs_before_use(command_list);
}

void append_object_ref_id(std::vector<trace::ObjectId> &refs, trace::ObjectId object_id)
{
  if (object_id == 0 || std::find(refs.begin(), refs.end(), object_id) != refs.end()) {
    return;
  }
  refs.push_back(object_id);
}

GpuVirtualAddressResolve append_gpu_virtual_address_binding_json_with_ref(
    std::ostringstream &payload,
    const char *key,
    std::uint64_t address,
    std::vector<trace::ObjectId> &refs)
{
  std::lock_guard lock(g_object_mutex);
  payload << "\"" << key << "\":" << address;
  auto resolve = resolve_gpu_virtual_address_locked(address);
  append_gpu_virtual_address_resolve_json(payload, resolve);
  append_object_ref_id(refs, resolve.object_id);
  return resolve;
}

std::vector<trace::ObjectId> collect_object_refs(const void *const *objects, std::uint32_t object_count)
{
  std::vector<trace::ObjectId> refs;
  if (!objects || object_count == 0) {
    return refs;
  }
  refs.reserve(object_count);
  std::lock_guard lock(g_object_mutex);
  for (std::uint32_t index = 0; index < object_count; ++index) {
    append_object_ref_id(refs, lookup_object_id_locked(objects[index]));
  }
  return refs;
}

void deduplicate_tail_object_refs(std::vector<trace::ObjectId> &refs, std::size_t first_tail_index)
{
  if (first_tail_index >= refs.size()) {
    return;
  }
  std::sort(refs.begin() + static_cast<std::ptrdiff_t>(first_tail_index), refs.end());
  refs.erase(std::unique(refs.begin() + static_cast<std::ptrdiff_t>(first_tail_index), refs.end()), refs.end());
}

std::vector<trace::BlobId> collect_blob_refs(const std::uint64_t *blobs, std::uint32_t blob_count)
{
  std::vector<trace::BlobId> refs;
  if (!blobs || blob_count == 0) {
    return refs;
  }
  refs.reserve(blob_count);
  for (std::uint32_t index = 0; index < blob_count; ++index) {
    if (blobs[index]) {
      refs.push_back(static_cast<trace::BlobId>(blobs[index]));
    }
  }
  return refs;
}

void append_copy_buffer_batch_payload(std::ostringstream &payload, const PendingCopyBufferBatch &batch)
{
  payload << "{\"op_count\":" << batch.ops.size() << ",\"ops\":[";
  for (std::size_t index = 0; index < batch.ops.size(); ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &op = batch.ops[index];
    payload << "{\"sequence\":" << op.sequence
            << ",\"function\":\"" << escape_json_string(op.function_name) << "\""
            << ",\"dst_buffer_object_id\":" << object_id(op.dst_buffer)
            << ",\"dst_offset\":" << op.dst_offset
            << ",\"src_buffer_object_id\":" << object_id(op.src_buffer)
            << ",\"src_offset\":" << op.src_offset
            << ",\"byte_count\":" << op.byte_count
            << "}";
  }
  payload << "]}";
}

void append_resource_barrier_batch_payload(std::ostringstream &payload, const PendingResourceBarrierBatch &batch)
{
  payload << "{\"schema\":\"resource-barrier-v2\",\"barrier_count\":" << batch.ops.size()
          << ",\"columns\":[\"sequence\",\"type\",\"flags\",\"resource\",\"before\",\"after\",\"subresource\","
          << "\"resource_before\",\"resource_after\"],\"barriers\":[";
  for (std::size_t index = 0; index < batch.ops.size(); ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &op = batch.ops[index];
    payload << "[" << op.sequence
            << "," << op.type
            << "," << op.flags
            << "," << op.resource_object_id
            << "," << op.before
            << "," << op.after
            << "," << op.subresource
            << "," << op.resource_before_object_id
            << "," << op.resource_after_object_id
            << "]";
  }
  payload << "]}";
}

void append_texture_copy_location_compact_json(std::ostringstream &payload, const TextureCopyLocationCompact &location)
{
  payload << "[" << location.resource_object_id
          << "," << location.type
          << "," << location.subresource_index
          << "," << location.footprint_offset
          << "," << location.footprint_format
          << "," << location.footprint_width
          << "," << location.footprint_height
          << "," << location.footprint_depth
          << "," << location.footprint_row_pitch
          << "]";
}

void append_copy_texture_region_batch_payload(std::ostringstream &payload, const PendingCopyTextureRegionBatch &batch)
{
  payload << "{\"schema\":\"copy-texture-region-v2\",\"op_count\":" << batch.ops.size()
          << ",\"columns\":[\"sequence\",\"dst\",\"dst_x\",\"dst_y\",\"dst_z\",\"src\",\"src_box\"],"
          << "\"location_columns\":[\"resource\",\"type\",\"subresource_index\",\"footprint_offset\","
          << "\"footprint_format\",\"footprint_width\",\"footprint_height\",\"footprint_depth\","
          << "\"footprint_row_pitch\"],\"ops\":[";
  for (std::size_t index = 0; index < batch.ops.size(); ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &op = batch.ops[index];
    payload << "[" << op.sequence << ",";
    append_texture_copy_location_compact_json(payload, op.dst);
    payload << "," << op.dst_x
            << "," << op.dst_y
            << "," << op.dst_z
            << ",";
    append_texture_copy_location_compact_json(payload, op.src);
    payload << ",";
    if (op.has_src_box) {
      payload << "[" << op.src_box_left
              << "," << op.src_box_top
              << "," << op.src_box_front
              << "," << op.src_box_right
              << "," << op.src_box_bottom
              << "," << op.src_box_back
              << "]";
    } else {
      payload << "null";
    }
    payload << "]";
  }
  payload << "]}";
}

void append_fence_dependency_batch_payload(std::ostringstream &payload, const PendingFenceDependencyBatch &batch)
{
  payload << "{\"schema\":\"fence-dependency-v2\",\"op_count\":" << batch.ops.size()
          << ",\"columns\":[\"sequence\",\"scope\",\"d3d_sequence\",\"encoder_id\","
          << "\"implicit_pre_raster_wait\",\"strong_count\",\"full_count\",\"minimal_count\",\"mask_count\"],\"ops\":[";
  for (std::size_t index = 0; index < batch.ops.size(); ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &op = batch.ops[index];
    payload << "[" << op.sequence
            << "," << fence_dependency_scope_id(op.scope)
            << "," << op.d3d_sequence
            << "," << op.encoder_id
            << "," << (op.implicit_pre_raster_wait ? 1 : 0)
            << "," << op.strong_count
            << "," << op.full_count
            << "," << op.minimal_count
            << "," << op.mask_count
            << "]";
  }
  payload << "]}";
}

void append_descriptor_view_batch_payload(std::ostringstream &payload, const PendingDescriptorViewBatch &batch)
{
  payload << "{\"schema\":\"descriptor-view-v2\",\"op_count\":" << batch.ops.size()
          << ",\"columns\":[\"sequence\",\"kind\",\"descriptor\",\"resource\",\"counter_resource\","
          << "\"format\",\"dimension\",\"mapping\",\"flags\",\"buffer_location\",\"size\","
          << "\"gpuva_status\",\"resolved_resource\",\"resolved_offset\",\"resolved_width\",\"view\"],\"ops\":[";
  for (std::size_t index = 0; index < batch.ops.size(); ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &op = batch.ops[index];
    payload << "[" << op.sequence
            << "," << op.kind
            << "," << op.descriptor
            << "," << op.resource_object_id
            << "," << op.counter_resource_object_id
            << "," << op.format
            << "," << op.view_dimension
            << "," << op.shader_4_component_mapping
            << "," << op.flags
            << "," << op.buffer_location
            << "," << op.size_in_bytes
            << ",\"" << escape_json_string(op.gpuva_resolve_status) << "\""
            << "," << op.resolved_resource_object_id
            << "," << op.resolved_resource_offset
            << "," << op.resolved_resource_width
            << "," << (op.view_payload.empty() ? "null" : op.view_payload)
            << "]";
  }
  payload << "]}";
}

void append_copy_descriptor_batch_payload(std::ostringstream &payload, const PendingCopyDescriptorBatch &batch)
{
  payload << "{\"schema\":\"copy-descriptors-v2\",\"op_count\":" << batch.ops.size()
          << ",\"columns\":[\"sequence\",\"heap_type\",\"descriptor_size\",\"dst_range_count\","
          << "\"src_range_count\",\"ranges\"],\"ops\":[";
  for (std::size_t index = 0; index < batch.ops.size(); ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &op = batch.ops[index];
    payload << "[" << op.sequence
            << "," << op.descriptor_heap_type
            << "," << op.descriptor_size
            << "," << op.dst_range_count
            << "," << op.src_range_count
            << ",[";
    for (std::size_t range_index = 0; range_index < op.ranges.size(); ++range_index) {
      if (range_index != 0) {
        payload << ",";
      }
      const auto &range = op.ranges[range_index];
      payload << "[" << range.dst_descriptor
              << "," << range.src_descriptor
              << "," << range.count
              << "]";
    }
    payload << "]]";
  }
  payload << "]}";
}

void record_call_event_unbatched(
    std::uint64_t sequence,
    const char *opname,
    const char *payload_json,
    const void *const *object_refs,
    std::uint32_t object_ref_count,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code)
{
  if (auto *session = session_for(classify_api(opname))) {
    trace::EventRecord event;
    event.kind = trace::EventKind::Call;
    event.callsite.sequence = sequence;
    event.callsite.function_name = opname ? opname : "";
    event.callsite.result_code = result_code;
    event.object_refs = collect_object_refs(object_refs, object_ref_count);
    event.blob_refs = collect_blob_refs(blob_refs, blob_ref_count);
    event.payload = payload_json && *payload_json ? payload_json : "{}";
    session->append_call_event(std::move(event));
  }
}

void record_call_event_unbatched_with_object_ids(
    std::uint64_t sequence,
    const char *opname,
    const char *payload_json,
    std::vector<trace::ObjectId> object_refs,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code)
{
  if (auto *session = session_for(classify_api(opname))) {
    trace::EventRecord event;
    event.kind = trace::EventKind::Call;
    event.callsite.sequence = sequence;
    event.callsite.function_name = opname ? opname : "";
    event.callsite.result_code = result_code;
    event.object_refs = std::move(object_refs);
    event.blob_refs = collect_blob_refs(blob_refs, blob_ref_count);
    event.payload = payload_json && *payload_json ? payload_json : "{}";
    session->append_call_event(std::move(event));
  }
}

PendingFenceDependencyBatch collect_fence_dependency_batch()
{
  std::lock_guard lock(g_diagnostic_batch_mutex);
  PendingFenceDependencyBatch batch = std::move(g_fence_dependency_batch);
  g_fence_dependency_batch = PendingFenceDependencyBatch{};
  return batch;
}

void flush_fence_dependency_batch(PendingFenceDependencyBatch batch)
{
  if (batch.ops.empty()) {
    return;
  }

  std::ostringstream payload;
  append_fence_dependency_batch_payload(payload, batch);
  record_call_event_unbatched(
      batch.ops.front().sequence,
      "DXMT::FenceDependencyBatch",
      payload.str().c_str(),
      nullptr,
      0,
      nullptr,
      0,
      0);
}

void flush_fence_dependency_batch()
{
  flush_fence_dependency_batch(collect_fence_dependency_batch());
}

PendingDescriptorViewBatch collect_descriptor_view_batch()
{
  std::lock_guard lock(g_diagnostic_batch_mutex);
  PendingDescriptorViewBatch batch = std::move(g_descriptor_view_batch);
  g_descriptor_view_batch = PendingDescriptorViewBatch{};
  return batch;
}

std::uint64_t descriptor_view_batch_sequence(const PendingDescriptorViewBatch &batch)
{
  return batch.ops.empty() ? UINT64_MAX : batch.ops.front().sequence;
}

std::uint64_t copy_descriptor_batch_sequence(const PendingCopyDescriptorBatch &batch)
{
  return batch.ops.empty() ? UINT64_MAX : batch.ops.front().sequence;
}

std::uint64_t fence_dependency_batch_sequence(const PendingFenceDependencyBatch &batch)
{
  return batch.ops.empty() ? UINT64_MAX : batch.ops.front().sequence;
}

std::uint64_t copy_buffer_batch_sequence(const PendingCopyBufferBatch &batch)
{
  return batch.ops.empty() ? UINT64_MAX : batch.ops.front().sequence;
}

std::uint64_t resource_barrier_batch_sequence(const PendingResourceBarrierBatch &batch)
{
  return batch.ops.empty() ? UINT64_MAX : batch.ops.front().sequence;
}

std::uint64_t copy_texture_region_batch_sequence(const PendingCopyTextureRegionBatch &batch)
{
  return batch.ops.empty() ? UINT64_MAX : batch.ops.front().sequence;
}

void flush_descriptor_view_batch(PendingDescriptorViewBatch batch)
{
  if (batch.ops.empty()) {
    return;
  }

  std::vector<trace::ObjectId> refs;
  refs.reserve(batch.ops.size() * 3);
  for (const auto &op : batch.ops) {
    if (op.device_object_id) {
      refs.push_back(op.device_object_id);
    }
    if (op.resource_object_id) {
      refs.push_back(op.resource_object_id);
    }
    if (op.counter_resource_object_id) {
      refs.push_back(op.counter_resource_object_id);
    }
  }
  std::sort(refs.begin(), refs.end());
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

  std::ostringstream payload;
  append_descriptor_view_batch_payload(payload, batch);
  record_call_event_unbatched_with_object_ids(
      batch.ops.front().sequence,
      "ID3D12Device::CreateDescriptorViewBatch",
      payload.str().c_str(),
      refs,
      nullptr,
      0,
      0);
}

PendingCopyDescriptorBatch collect_copy_descriptor_batch()
{
  std::lock_guard lock(g_diagnostic_batch_mutex);
  PendingCopyDescriptorBatch batch = std::move(g_copy_descriptor_batch);
  g_copy_descriptor_batch = PendingCopyDescriptorBatch{};
  return batch;
}

void flush_copy_descriptor_batch(PendingCopyDescriptorBatch batch)
{
  if (batch.ops.empty()) {
    return;
  }

  std::vector<trace::ObjectId> refs;
  refs.reserve(batch.ops.size());
  for (const auto &op : batch.ops) {
    if (op.device_object_id) {
      refs.push_back(op.device_object_id);
    }
  }
  std::sort(refs.begin(), refs.end());
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

  std::ostringstream payload;
  append_copy_descriptor_batch_payload(payload, batch);
  record_call_event_unbatched_with_object_ids(
      batch.ops.front().sequence,
      "ID3D12Device::CopyDescriptorsBatch",
      payload.str().c_str(),
      refs,
      nullptr,
      0,
      0);
}

void flush_diagnostic_batches();

std::uint64_t record_descriptor_view_batched(
    const char *function_name,
    trace::ObjectId device_object_id,
    trace::ObjectId resource_object_id,
    trace::ObjectId counter_resource_object_id,
    DescriptorViewOp op)
{
  std::lock_guard event_lock(g_event_order_mutex);
  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  op.sequence = sequence;
  op.kind = descriptor_view_kind_for_function(function_name);
  op.device_object_id = device_object_id;
  op.resource_object_id = resource_object_id;
  op.counter_resource_object_id = counter_resource_object_id;
  bool should_flush = false;
  {
    std::lock_guard lock(g_diagnostic_batch_mutex);
    g_descriptor_view_batch.ops.push_back(std::move(op));
    should_flush = g_descriptor_view_batch.ops.size() >= kDescriptorViewBatchMaxOps;
  }
  if (should_flush) {
    flush_diagnostic_batches();
  }
  return sequence;
}

void record_boundary_event_unbatched(
    std::uint64_t sequence,
    trace::BoundaryKind boundary,
    const char *payload_json)
{
  if (auto *session = session_for(trace::ApiKind::D3D12)) {
    trace::EventRecord event;
    event.kind = trace::EventKind::Boundary;
    event.callsite.sequence = sequence;
    event.callsite.function_name = "D3DBoundary";
    event.boundary = boundary;
    event.payload = payload_json && *payload_json ? payload_json : "{}";
    session->append_call_event(std::move(event));
  }
}

void record_copy_buffer_batch_event(const void *command_list, const PendingCopyBufferBatch &batch)
{
  std::vector<const void *> refs;
  refs.reserve(1 + batch.ops.size() * 2);
  refs.push_back(command_list);
  for (const auto &op : batch.ops) {
    refs.push_back(op.dst_buffer);
    refs.push_back(op.src_buffer);
  }

  std::ostringstream payload;
  append_copy_buffer_batch_payload(payload, batch);
  record_call_event_unbatched(
      batch.ops.front().sequence,
      "ID3D12GraphicsCommandList::CopyBufferRegionBatch",
      payload.str().c_str(),
      refs.data(),
      static_cast<std::uint32_t>(refs.size()),
      nullptr,
      0,
      0);
}

void record_resource_barrier_batch_event(const void *command_list, const PendingResourceBarrierBatch &batch)
{
  std::vector<trace::ObjectId> refs;
  refs.reserve(1 + batch.ops.size() * 3);
  std::size_t resource_ref_begin = 0;
  if (const auto command_list_id = object_id(command_list)) {
    refs.push_back(command_list_id);
    resource_ref_begin = 1;
  }
  for (const auto &op : batch.ops) {
    if (op.resource_object_id) {
      refs.push_back(op.resource_object_id);
    }
    if (op.resource_before_object_id) {
      refs.push_back(op.resource_before_object_id);
    }
    if (op.resource_after_object_id) {
      refs.push_back(op.resource_after_object_id);
    }
  }
  deduplicate_tail_object_refs(refs, resource_ref_begin);

  std::ostringstream payload;
  append_resource_barrier_batch_payload(payload, batch);
  record_call_event_unbatched_with_object_ids(
      batch.ops.front().sequence,
      "ID3D12GraphicsCommandList::ResourceBarrierBatch",
      payload.str().c_str(),
      refs,
      nullptr,
      0,
      0);
}

void record_copy_texture_region_batch_event(const void *command_list, const PendingCopyTextureRegionBatch &batch)
{
  std::vector<trace::ObjectId> refs;
  refs.reserve(1 + batch.ops.size() * 2);
  std::size_t resource_ref_begin = 0;
  if (const auto command_list_id = object_id(command_list)) {
    refs.push_back(command_list_id);
    resource_ref_begin = 1;
  }
  for (const auto &op : batch.ops) {
    if (op.dst.resource_object_id) {
      refs.push_back(op.dst.resource_object_id);
    }
    if (op.src.resource_object_id) {
      refs.push_back(op.src.resource_object_id);
    }
  }
  deduplicate_tail_object_refs(refs, resource_ref_begin);

  std::ostringstream payload;
  append_copy_texture_region_batch_payload(payload, batch);
  record_call_event_unbatched_with_object_ids(
      batch.ops.front().sequence,
      "ID3D12GraphicsCommandList::CopyTextureRegionBatch",
      payload.str().c_str(),
      refs,
      nullptr,
      0,
      0);
}

std::vector<CopyBufferBatchFlush> collect_copy_buffer_batches(const void *command_list)
{
  std::lock_guard lock(g_command_batch_mutex);
  std::vector<CopyBufferBatchFlush> batches;
  if (command_list) {
    auto found = g_copy_buffer_batches.find(command_list);
    if (found != g_copy_buffer_batches.end() && !found->second.ops.empty()) {
      batches.push_back(CopyBufferBatchFlush{command_list, std::move(found->second)});
      g_copy_buffer_batches.erase(found);
    }
  } else {
    batches.reserve(g_copy_buffer_batches.size());
    for (auto &entry : g_copy_buffer_batches) {
      if (!entry.second.ops.empty()) {
        batches.push_back(CopyBufferBatchFlush{entry.first, std::move(entry.second)});
      }
    }
    g_copy_buffer_batches.clear();
  }
  std::sort(batches.begin(), batches.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.batch.ops.front().sequence < rhs.batch.ops.front().sequence;
  });
  return batches;
}

std::vector<ResourceBarrierBatchFlush> collect_resource_barrier_batches(const void *command_list)
{
  std::lock_guard lock(g_command_batch_mutex);
  std::vector<ResourceBarrierBatchFlush> batches;
  if (command_list) {
    auto found = g_resource_barrier_batches.find(command_list);
    if (found != g_resource_barrier_batches.end() && !found->second.ops.empty()) {
      batches.push_back(ResourceBarrierBatchFlush{command_list, std::move(found->second)});
      g_resource_barrier_batches.erase(found);
    }
  } else {
    batches.reserve(g_resource_barrier_batches.size());
    for (auto &entry : g_resource_barrier_batches) {
      if (!entry.second.ops.empty()) {
        batches.push_back(ResourceBarrierBatchFlush{entry.first, std::move(entry.second)});
      }
    }
    g_resource_barrier_batches.clear();
  }
  std::sort(batches.begin(), batches.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.batch.ops.front().sequence < rhs.batch.ops.front().sequence;
  });
  return batches;
}

std::vector<CopyTextureRegionBatchFlush> collect_copy_texture_region_batches(const void *command_list)
{
  std::lock_guard lock(g_command_batch_mutex);
  std::vector<CopyTextureRegionBatchFlush> batches;
  if (command_list) {
    auto found = g_copy_texture_region_batches.find(command_list);
    if (found != g_copy_texture_region_batches.end() && !found->second.ops.empty()) {
      batches.push_back(CopyTextureRegionBatchFlush{command_list, std::move(found->second)});
      g_copy_texture_region_batches.erase(found);
    }
  } else {
    batches.reserve(g_copy_texture_region_batches.size());
    for (auto &entry : g_copy_texture_region_batches) {
      if (!entry.second.ops.empty()) {
        batches.push_back(CopyTextureRegionBatchFlush{entry.first, std::move(entry.second)});
      }
    }
    g_copy_texture_region_batches.clear();
  }
  std::sort(batches.begin(), batches.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.batch.ops.front().sequence < rhs.batch.ops.front().sequence;
  });
  return batches;
}

void flush_copy_buffer_batches(const void *command_list)
{
  auto batches = collect_copy_buffer_batches(command_list);
  for (const auto &batch : batches) {
    record_copy_buffer_batch_event(batch.command_list, batch.batch);
  }
}

void flush_resource_barrier_batches(const void *command_list)
{
  auto batches = collect_resource_barrier_batches(command_list);
  for (const auto &batch : batches) {
    record_resource_barrier_batch_event(batch.command_list, batch.batch);
  }
}

void flush_copy_texture_region_batches(const void *command_list)
{
  auto batches = collect_copy_texture_region_batches(command_list);
  for (const auto &batch : batches) {
    record_copy_texture_region_batch_event(batch.command_list, batch.batch);
  }
}

void flush_command_list_batches(const void *command_list)
{
  flush_copy_buffer_batches(command_list);
  flush_resource_barrier_batches(command_list);
  flush_copy_texture_region_batches(command_list);
}

void append_pending_batch_flush(std::vector<PendingBatchFlush> &pending, CopyBufferBatchFlush batch)
{
  if (batch.batch.ops.empty()) {
    return;
  }
  PendingBatchFlush flush;
  flush.kind = PendingBatchKind::CopyBuffer;
  flush.sequence = copy_buffer_batch_sequence(batch.batch);
  flush.command_list = batch.command_list;
  flush.copy_buffer = std::move(batch.batch);
  pending.push_back(std::move(flush));
}

void append_pending_batch_flush(std::vector<PendingBatchFlush> &pending, ResourceBarrierBatchFlush batch)
{
  if (batch.batch.ops.empty()) {
    return;
  }
  PendingBatchFlush flush;
  flush.kind = PendingBatchKind::ResourceBarrier;
  flush.sequence = resource_barrier_batch_sequence(batch.batch);
  flush.command_list = batch.command_list;
  flush.resource_barrier = std::move(batch.batch);
  pending.push_back(std::move(flush));
}

void append_pending_batch_flush(std::vector<PendingBatchFlush> &pending, CopyTextureRegionBatchFlush batch)
{
  if (batch.batch.ops.empty()) {
    return;
  }
  PendingBatchFlush flush;
  flush.kind = PendingBatchKind::CopyTextureRegion;
  flush.sequence = copy_texture_region_batch_sequence(batch.batch);
  flush.command_list = batch.command_list;
  flush.copy_texture_region = std::move(batch.batch);
  pending.push_back(std::move(flush));
}

void append_pending_batch_flush(std::vector<PendingBatchFlush> &pending, PendingFenceDependencyBatch batch)
{
  if (batch.ops.empty()) {
    return;
  }
  PendingBatchFlush flush;
  flush.kind = PendingBatchKind::FenceDependency;
  flush.sequence = fence_dependency_batch_sequence(batch);
  flush.fence_dependency = std::move(batch);
  pending.push_back(std::move(flush));
}

void append_pending_batch_flush(std::vector<PendingBatchFlush> &pending, PendingDescriptorViewBatch batch)
{
  if (batch.ops.empty()) {
    return;
  }
  PendingBatchFlush flush;
  flush.kind = PendingBatchKind::DescriptorView;
  flush.sequence = descriptor_view_batch_sequence(batch);
  flush.descriptor_view = std::move(batch);
  pending.push_back(std::move(flush));
}

void append_pending_batch_flush(std::vector<PendingBatchFlush> &pending, PendingCopyDescriptorBatch batch)
{
  if (batch.ops.empty()) {
    return;
  }
  PendingBatchFlush flush;
  flush.kind = PendingBatchKind::CopyDescriptor;
  flush.sequence = copy_descriptor_batch_sequence(batch);
  flush.copy_descriptor = std::move(batch);
  pending.push_back(std::move(flush));
}

void emit_pending_batch_flush(PendingBatchFlush &flush)
{
  switch (flush.kind) {
  case PendingBatchKind::CopyBuffer:
    record_copy_buffer_batch_event(flush.command_list, flush.copy_buffer);
    break;
  case PendingBatchKind::ResourceBarrier:
    record_resource_barrier_batch_event(flush.command_list, flush.resource_barrier);
    break;
  case PendingBatchKind::CopyTextureRegion:
    record_copy_texture_region_batch_event(flush.command_list, flush.copy_texture_region);
    break;
  case PendingBatchKind::FenceDependency:
    flush_fence_dependency_batch(std::move(flush.fence_dependency));
    break;
  case PendingBatchKind::DescriptorView:
    flush_descriptor_view_batch(std::move(flush.descriptor_view));
    break;
  case PendingBatchKind::CopyDescriptor:
    flush_copy_descriptor_batch(std::move(flush.copy_descriptor));
    break;
  }
}

void flush_pending_batches(const void *command_list = nullptr)
{
  std::vector<PendingBatchFlush> pending;
  for (auto &batch : collect_copy_buffer_batches(command_list)) {
    append_pending_batch_flush(pending, std::move(batch));
  }
  for (auto &batch : collect_resource_barrier_batches(command_list)) {
    append_pending_batch_flush(pending, std::move(batch));
  }
  for (auto &batch : collect_copy_texture_region_batches(command_list)) {
    append_pending_batch_flush(pending, std::move(batch));
  }
  if (!command_list) {
    append_pending_batch_flush(pending, collect_fence_dependency_batch());
    append_pending_batch_flush(pending, collect_descriptor_view_batch());
    append_pending_batch_flush(pending, collect_copy_descriptor_batch());
  }
  std::sort(pending.begin(), pending.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.sequence < rhs.sequence;
  });
  for (auto &batch : pending) {
    emit_pending_batch_flush(batch);
  }
}

void flush_diagnostic_batches()
{
  flush_pending_batches();
}

// Present-frame textures are the largest per-frame asset captured (full RGBA
// backbuffer, multiple MB). Hashing them eagerly here forces the synchronous
// content-addressed dedup path inside register_asset and runs a full SHA256 on
// the capture (readback) thread for every presented frame -- the single most
// expensive synchronous computation on the live capture path. Defer it instead:
// register_asset enqueues the payload to the async asset-writer pool, which both
// writes and hashes it off the capture thread, and bundle-finalize performs the
// authoritative content dedup offline. Identical back-to-back frames that arrive
// before the first async hash lands are deduplicated at finalize time, so the
// only thing lost is a transient in-flight dedup window -- acceptable under the
// "keep only required synchronous work on the capture thread" rule.
trace::AssetRecord register_asset_bytes(
    TraceSession *session,
    trace::AssetKind kind,
    const char *debug_name,
    const void *data,
    std::size_t size);

trace::AssetRecord register_asset_bytes(
    trace::AssetKind kind,
    const char *debug_name,
    const void *data,
    std::size_t size)
{
  auto *session = session_for(trace::ApiKind::D3D12);
  if (!session) {
    return {};
  }
  return register_asset_bytes(session, kind, debug_name, data, size);
}

trace::AssetRecord register_asset_bytes(
    TraceSession *session,
    trace::AssetKind kind,
    const char *debug_name,
    const void *data,
    std::size_t size)
{
  if (!session) {
    return {};
  }

  trace::AssetRecord asset;
  asset.blob_id = g_blob_id.fetch_add(1, std::memory_order_relaxed) + 1;
  asset.kind = kind;
  asset.debug_name = debug_name ? debug_name : "";

  if (!data || size == 0) {
    return session->capture_raw_mode() == runtime::CaptureOptions::CaptureRawMode::RawOnly
        ? session->stage_raw_asset(std::move(asset))
        : session->register_asset(std::move(asset));
  }

  const auto *bytes = static_cast<const std::uint8_t *>(data);
  asset.payload_bytes.assign(bytes, bytes + size);
  return session->capture_raw_mode() == runtime::CaptureOptions::CaptureRawMode::RawOnly
      ? session->stage_raw_asset(std::move(asset))
      : session->register_asset(std::move(asset));
}

trace::AssetRecord register_shader_asset_bytes(
    const char *debug_name,
    const void *data,
    std::size_t size)
{
  auto *session = session_for(trace::ApiKind::D3D12);
  if (!session) {
    return {};
  }
  if (!data || size == 0) {
    return register_asset_bytes(session, trace::AssetKind::ShaderDxil, debug_name, data, size);
  }

  const ShaderAssetMemoKey key{data, size};
  ShaderAssetMemoEntry memo_entry;
  bool has_memo_entry = false;
  {
    std::lock_guard lock(g_shader_asset_memo_mutex);
    const auto found = g_shader_asset_memo.find(key);
    if (found != g_shader_asset_memo.end() &&
        found->second.asset.blob_id != 0 &&
        !found->second.asset.relative_path.empty()) {
      memo_entry = found->second;
      has_memo_entry = true;
    }
  }

  std::string fast_fingerprint;
  if (has_memo_entry) {
    fast_fingerprint = trace::fast_fingerprint_bytes(data, size);
    if (memo_entry.fast_fingerprint == fast_fingerprint) {
      return memo_entry.asset;
    }
  }

  auto asset = register_asset_bytes(session, trace::AssetKind::ShaderDxil, debug_name, data, size);
  if (asset.blob_id != 0 && !asset.relative_path.empty()) {
    if (fast_fingerprint.empty()) {
      fast_fingerprint = asset.fast_fingerprint.empty()
                             ? trace::fast_fingerprint_bytes(data, size)
                             : asset.fast_fingerprint;
    }
    std::lock_guard lock(g_shader_asset_memo_mutex);
    g_shader_asset_memo[key] = ShaderAssetMemoEntry{asset, fast_fingerprint};
  }
  return asset;
}

std::string hex_encode_bytes(const void *data, std::size_t size)
{
  static constexpr char kHex[] = "0123456789abcdef";
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  std::string encoded;
  encoded.resize(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    encoded[index * 2] = kHex[bytes[index] >> 4];
    encoded[index * 2 + 1] = kHex[bytes[index] & 0xf];
  }
  return encoded;
}

std::size_t align_stream_offset(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t pipeline_stream_payload_size(PipelineStateSubobjectType type)
{
  switch (type) {
  case PipelineStateSubobjectType::RootSignature:
    return sizeof(ID3D12RootSignature *);
  case PipelineStateSubobjectType::VS:
  case PipelineStateSubobjectType::PS:
  case PipelineStateSubobjectType::DS:
  case PipelineStateSubobjectType::HS:
  case PipelineStateSubobjectType::GS:
  case PipelineStateSubobjectType::CS:
  case PipelineStateSubobjectType::AS:
  case PipelineStateSubobjectType::MS:
    return sizeof(D3D12_SHADER_BYTECODE);
  case PipelineStateSubobjectType::StreamOutput:
    return sizeof(D3D12_STREAM_OUTPUT_DESC);
  case PipelineStateSubobjectType::Blend:
    return sizeof(D3D12_BLEND_DESC);
  case PipelineStateSubobjectType::SampleMask:
    return sizeof(UINT);
  case PipelineStateSubobjectType::Rasterizer:
    return sizeof(D3D12_RASTERIZER_DESC);
  case PipelineStateSubobjectType::DepthStencil:
    return sizeof(D3D12_DEPTH_STENCIL_DESC);
  case PipelineStateSubobjectType::DepthStencil1:
    return sizeof(DepthStencilDesc1);
  case PipelineStateSubobjectType::InputLayout:
    return sizeof(D3D12_INPUT_LAYOUT_DESC);
  case PipelineStateSubobjectType::IbStripCutValue:
    return sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case PipelineStateSubobjectType::PrimitiveTopology:
    return sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case PipelineStateSubobjectType::RenderTargetFormats:
    return sizeof(RtFormatArray);
  case PipelineStateSubobjectType::DepthStencilFormat:
    return sizeof(DXGI_FORMAT);
  case PipelineStateSubobjectType::SampleDesc:
    return sizeof(DXGI_SAMPLE_DESC);
  case PipelineStateSubobjectType::NodeMask:
    return sizeof(UINT);
  case PipelineStateSubobjectType::CachedPso:
    return sizeof(D3D12_CACHED_PIPELINE_STATE);
  case PipelineStateSubobjectType::Flags:
    return sizeof(D3D12_PIPELINE_STATE_FLAGS);
  case PipelineStateSubobjectType::ViewInstancing:
    return sizeof(ViewInstancingDesc);
  default:
    return 0;
  }
}

std::size_t pipeline_stream_payload_alignment(PipelineStateSubobjectType type)
{
  switch (type) {
  case PipelineStateSubobjectType::RootSignature:
    return alignof(ID3D12RootSignature *);
  case PipelineStateSubobjectType::VS:
  case PipelineStateSubobjectType::PS:
  case PipelineStateSubobjectType::DS:
  case PipelineStateSubobjectType::HS:
  case PipelineStateSubobjectType::GS:
  case PipelineStateSubobjectType::CS:
  case PipelineStateSubobjectType::AS:
  case PipelineStateSubobjectType::MS:
    return alignof(D3D12_SHADER_BYTECODE);
  case PipelineStateSubobjectType::StreamOutput:
    return alignof(D3D12_STREAM_OUTPUT_DESC);
  case PipelineStateSubobjectType::Blend:
    return alignof(D3D12_BLEND_DESC);
  case PipelineStateSubobjectType::SampleMask:
    return alignof(UINT);
  case PipelineStateSubobjectType::Rasterizer:
    return alignof(D3D12_RASTERIZER_DESC);
  case PipelineStateSubobjectType::DepthStencil:
    return alignof(D3D12_DEPTH_STENCIL_DESC);
  case PipelineStateSubobjectType::DepthStencil1:
    return alignof(DepthStencilDesc1);
  case PipelineStateSubobjectType::InputLayout:
    return alignof(D3D12_INPUT_LAYOUT_DESC);
  case PipelineStateSubobjectType::IbStripCutValue:
    return alignof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case PipelineStateSubobjectType::PrimitiveTopology:
    return alignof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case PipelineStateSubobjectType::RenderTargetFormats:
    return alignof(RtFormatArray);
  case PipelineStateSubobjectType::DepthStencilFormat:
    return alignof(DXGI_FORMAT);
  case PipelineStateSubobjectType::SampleDesc:
    return alignof(DXGI_SAMPLE_DESC);
  case PipelineStateSubobjectType::NodeMask:
    return alignof(UINT);
  case PipelineStateSubobjectType::CachedPso:
    return alignof(D3D12_CACHED_PIPELINE_STATE);
  case PipelineStateSubobjectType::Flags:
    return alignof(D3D12_PIPELINE_STATE_FLAGS);
  case PipelineStateSubobjectType::ViewInstancing:
    return alignof(ViewInstancingDesc);
  default:
    return 0;
  }
}

std::string raw_shader_asset_json(
    const char *field_name,
    const D3D12_SHADER_BYTECODE &bytecode,
    std::vector<trace::BlobId> &blob_refs)
{
  if (!bytecode.pShaderBytecode || bytecode.BytecodeLength == 0) {
    return std::string("\"") + field_name + "\":null";
  }
  const auto asset = register_shader_asset_bytes(
      (std::string("d3d12-") + field_name).c_str(),
      bytecode.pShaderBytecode,
      bytecode.BytecodeLength);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return std::string("\"") + field_name + "\":null";
  }
  blob_refs.push_back(asset.blob_id);
  std::ostringstream payload;
  payload << "\"" << field_name << "\":{"
          << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength)
          << ",\"" << field_name << "_path\":\"" << asset.relative_path.generic_string() << "\""
          << ",\"blob_id\":" << asset.blob_id
          << "}";
  return payload.str();
}

struct ShaderAssetMetadataJson {
  std::string asset_json;
  std::string metadata_json;
};

ShaderAssetMetadataJson shader_asset_metadata_json(
    const char *field_name,
    const D3D12_SHADER_BYTECODE &bytecode,
    std::vector<trace::BlobId> &blob_refs)
{
  if (!bytecode.pShaderBytecode || bytecode.BytecodeLength == 0) {
    const auto null_field = std::string("\"") + field_name + "\":null";
    return {null_field, null_field};
  }
  const auto asset = register_shader_asset_bytes(
      (std::string("d3d12-") + field_name).c_str(),
      bytecode.pShaderBytecode,
      bytecode.BytecodeLength);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    const auto null_field = std::string("\"") + field_name + "\":null";
    return {null_field, null_field};
  }
  blob_refs.push_back(asset.blob_id);
  std::ostringstream asset_payload;
  asset_payload << "\"" << field_name << "\":{"
                << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength)
                << ",\"" << field_name << "_path\":\"" << asset.relative_path.generic_string() << "\""
                << "}";
  std::ostringstream metadata_payload;
  metadata_payload << "\"" << field_name << "\":{"
                   << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength)
                   << ",\"blob_id\":" << asset.blob_id
                   << "}";
  return {asset_payload.str(), metadata_payload.str()};
}

struct StreamShaderAssetJson {
  std::string vs = "\"vs\":null";
  std::string ps = "\"ps\":null";
  std::string ds = "\"ds\":null";
  std::string hs = "\"hs\":null";
  std::string gs = "\"gs\":null";
  std::string cs = "\"cs\":null";
  std::string as = "\"as\":null";
  std::string ms = "\"ms\":null";
};

struct StreamShaderMetadataJson {
  std::string vs = "\"vs\":null";
  std::string ps = "\"ps\":null";
  std::string ds = "\"ds\":null";
  std::string hs = "\"hs\":null";
  std::string gs = "\"gs\":null";
  std::string cs = "\"cs\":null";
  std::string as = "\"as\":null";
  std::string ms = "\"ms\":null";
};


std::string srv_desc_detail_json(const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
    payload << "\"first_element\":" << desc->Buffer.FirstElement
            << ",\"num_elements\":" << desc->Buffer.NumElements
            << ",\"structure_byte_stride\":" << desc->Buffer.StructureByteStride
            << ",\"flags\":" << static_cast<unsigned int>(desc->Buffer.Flags);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    payload << "\"most_detailed_mip\":" << desc->Texture1D.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture1D.MipLevels
            << ",\"resource_min_lod_clamp\":" << desc->Texture1D.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"most_detailed_mip\":" << desc->Texture1DArray.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture1DArray.MipLevels
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize
            << ",\"resource_min_lod_clamp\":" << desc->Texture1DArray.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    payload << "\"most_detailed_mip\":" << desc->Texture2D.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture2D.MipLevels
            << ",\"plane_slice\":" << desc->Texture2D.PlaneSlice
            << ",\"resource_min_lod_clamp\":" << desc->Texture2D.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"most_detailed_mip\":" << desc->Texture2DArray.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture2DArray.MipLevels
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice
            << ",\"resource_min_lod_clamp\":" << desc->Texture2DArray.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    payload << "\"first_array_slice\":" << desc->Texture2DMSArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DMSArray.ArraySize;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    payload << "\"most_detailed_mip\":" << desc->Texture3D.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture3D.MipLevels
            << ",\"resource_min_lod_clamp\":" << desc->Texture3D.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    payload << "\"most_detailed_mip\":" << desc->TextureCube.MostDetailedMip
            << ",\"mip_levels\":" << desc->TextureCube.MipLevels
            << ",\"resource_min_lod_clamp\":" << desc->TextureCube.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    payload << "\"most_detailed_mip\":" << desc->TextureCubeArray.MostDetailedMip
            << ",\"mip_levels\":" << desc->TextureCubeArray.MipLevels
            << ",\"first_2d_array_face\":" << desc->TextureCubeArray.First2DArrayFace
            << ",\"num_cubes\":" << desc->TextureCubeArray.NumCubes
            << ",\"resource_min_lod_clamp\":" << desc->TextureCubeArray.ResourceMinLODClamp;
    break;
#ifdef D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE
  case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
    payload << "\"location\":" << desc->RaytracingAccelerationStructure.Location;
    break;
#endif
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

void append_json_u64_field(std::ostringstream &payload, bool &first, const char *name, std::uint64_t value)
{
  if (value == 0) {
    return;
  }
  if (!first) {
    payload << ",";
  }
  first = false;
  payload << "\"" << name << "\":" << value;
}

void append_json_u32_field(std::ostringstream &payload, bool &first, const char *name, std::uint32_t value)
{
  append_json_u64_field(payload, first, name, value);
}

void append_json_float_field(std::ostringstream &payload, bool &first, const char *name, float value)
{
  if (value == 0.0f) {
    return;
  }
  if (!first) {
    payload << ",";
  }
  first = false;
  payload << "\"" << name << "\":" << value;
}

std::string srv_desc_detail_sparse_json(const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  bool first = true;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
    append_json_u64_field(payload, first, "first_element", desc->Buffer.FirstElement);
    append_json_u32_field(payload, first, "num_elements", desc->Buffer.NumElements);
    append_json_u32_field(payload, first, "structure_byte_stride", desc->Buffer.StructureByteStride);
    append_json_u32_field(payload, first, "flags", static_cast<unsigned int>(desc->Buffer.Flags));
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->Texture1D.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->Texture1D.MipLevels);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->Texture1D.ResourceMinLODClamp);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->Texture1DArray.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->Texture1DArray.MipLevels);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture1DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture1DArray.ArraySize);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->Texture1DArray.ResourceMinLODClamp);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->Texture2D.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->Texture2D.MipLevels);
    append_json_u32_field(payload, first, "plane_slice", desc->Texture2D.PlaneSlice);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->Texture2D.ResourceMinLODClamp);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->Texture2DArray.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->Texture2DArray.MipLevels);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DArray.ArraySize);
    append_json_u32_field(payload, first, "plane_slice", desc->Texture2DArray.PlaneSlice);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->Texture2DArray.ResourceMinLODClamp);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DMSArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DMSArray.ArraySize);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->Texture3D.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->Texture3D.MipLevels);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->Texture3D.ResourceMinLODClamp);
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->TextureCube.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->TextureCube.MipLevels);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->TextureCube.ResourceMinLODClamp);
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    append_json_u32_field(payload, first, "most_detailed_mip", desc->TextureCubeArray.MostDetailedMip);
    append_json_u32_field(payload, first, "mip_levels", desc->TextureCubeArray.MipLevels);
    append_json_u32_field(payload, first, "first_2d_array_face", desc->TextureCubeArray.First2DArrayFace);
    append_json_u32_field(payload, first, "num_cubes", desc->TextureCubeArray.NumCubes);
    append_json_float_field(payload, first, "resource_min_lod_clamp", desc->TextureCubeArray.ResourceMinLODClamp);
    break;
#ifdef D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE
  case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
    append_json_u64_field(payload, first, "location", desc->RaytracingAccelerationStructure.Location);
    break;
#endif
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string uav_desc_detail_json(const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
    payload << "\"first_element\":" << desc->Buffer.FirstElement
            << ",\"num_elements\":" << desc->Buffer.NumElements
            << ",\"structure_byte_stride\":" << desc->Buffer.StructureByteStride
            << ",\"counter_offset_in_bytes\":" << desc->Buffer.CounterOffsetInBytes
            << ",\"flags\":" << static_cast<unsigned int>(desc->Buffer.Flags);
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    payload << "\"mip_slice\":" << desc->Texture1D.MipSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"mip_slice\":" << desc->Texture1DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    payload << "\"mip_slice\":" << desc->Texture2D.MipSlice
            << ",\"plane_slice\":" << desc->Texture2D.PlaneSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    payload << "\"mip_slice\":" << desc->Texture3D.MipSlice
            << ",\"first_w_slice\":" << desc->Texture3D.FirstWSlice
            << ",\"w_size\":" << desc->Texture3D.WSize;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string uav_desc_detail_sparse_json(const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  bool first = true;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
    append_json_u64_field(payload, first, "first_element", desc->Buffer.FirstElement);
    append_json_u32_field(payload, first, "num_elements", desc->Buffer.NumElements);
    append_json_u32_field(payload, first, "structure_byte_stride", desc->Buffer.StructureByteStride);
    append_json_u64_field(payload, first, "counter_offset_in_bytes", desc->Buffer.CounterOffsetInBytes);
    append_json_u32_field(payload, first, "flags", static_cast<unsigned int>(desc->Buffer.Flags));
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture1D.MipSlice);
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture1DArray.MipSlice);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture1DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture1DArray.ArraySize);
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture2D.MipSlice);
    append_json_u32_field(payload, first, "plane_slice", desc->Texture2D.PlaneSlice);
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture2DArray.MipSlice);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DArray.ArraySize);
    append_json_u32_field(payload, first, "plane_slice", desc->Texture2DArray.PlaneSlice);
    break;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture3D.MipSlice);
    append_json_u32_field(payload, first, "first_w_slice", desc->Texture3D.FirstWSlice);
    append_json_u32_field(payload, first, "w_size", desc->Texture3D.WSize);
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string rtv_desc_detail_json(const D3D12_RENDER_TARGET_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_RTV_DIMENSION_BUFFER:
    payload << "\"first_element\":" << desc->Buffer.FirstElement
            << ",\"num_elements\":" << desc->Buffer.NumElements;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE1D:
    payload << "\"mip_slice\":" << desc->Texture1D.MipSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"mip_slice\":" << desc->Texture1DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2D:
    payload << "\"mip_slice\":" << desc->Texture2D.MipSlice
            << ",\"plane_slice\":" << desc->Texture2D.PlaneSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    payload << "\"first_array_slice\":" << desc->Texture2DMSArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DMSArray.ArraySize;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE3D:
    payload << "\"mip_slice\":" << desc->Texture3D.MipSlice
            << ",\"first_w_slice\":" << desc->Texture3D.FirstWSlice
            << ",\"w_size\":" << desc->Texture3D.WSize;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string rtv_desc_detail_sparse_json(const D3D12_RENDER_TARGET_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  bool first = true;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_RTV_DIMENSION_BUFFER:
    append_json_u64_field(payload, first, "first_element", desc->Buffer.FirstElement);
    append_json_u32_field(payload, first, "num_elements", desc->Buffer.NumElements);
    break;
  case D3D12_RTV_DIMENSION_TEXTURE1D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture1D.MipSlice);
    break;
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture1DArray.MipSlice);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture1DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture1DArray.ArraySize);
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture2D.MipSlice);
    append_json_u32_field(payload, first, "plane_slice", desc->Texture2D.PlaneSlice);
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture2DArray.MipSlice);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DArray.ArraySize);
    append_json_u32_field(payload, first, "plane_slice", desc->Texture2DArray.PlaneSlice);
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DMSArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DMSArray.ArraySize);
    break;
  case D3D12_RTV_DIMENSION_TEXTURE3D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture3D.MipSlice);
    append_json_u32_field(payload, first, "first_w_slice", desc->Texture3D.FirstWSlice);
    append_json_u32_field(payload, first, "w_size", desc->Texture3D.WSize);
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string dsv_desc_detail_json(const D3D12_DEPTH_STENCIL_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    payload << "\"mip_slice\":" << desc->Texture1D.MipSlice;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"mip_slice\":" << desc->Texture1DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    payload << "\"mip_slice\":" << desc->Texture2D.MipSlice;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    payload << "\"first_array_slice\":" << desc->Texture2DMSArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DMSArray.ArraySize;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string dsv_desc_detail_sparse_json(const D3D12_DEPTH_STENCIL_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  bool first = true;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture1D.MipSlice);
    break;
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture1DArray.MipSlice);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture1DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture1DArray.ArraySize);
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture2D.MipSlice);
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    append_json_u32_field(payload, first, "mip_slice", desc->Texture2DArray.MipSlice);
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DArray.ArraySize);
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    append_json_u32_field(payload, first, "first_array_slice", desc->Texture2DMSArray.FirstArraySlice);
    append_json_u32_field(payload, first, "array_size", desc->Texture2DMSArray.ArraySize);
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string render_target_blend_desc_json(const D3D12_RENDER_TARGET_BLEND_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"blend_enable\":" << (desc.BlendEnable ? "true" : "false")
          << ",\"logic_op_enable\":" << (desc.LogicOpEnable ? "true" : "false")
          << ",\"src_blend\":" << static_cast<unsigned int>(desc.SrcBlend)
          << ",\"dest_blend\":" << static_cast<unsigned int>(desc.DestBlend)
          << ",\"blend_op\":" << static_cast<unsigned int>(desc.BlendOp)
          << ",\"src_blend_alpha\":" << static_cast<unsigned int>(desc.SrcBlendAlpha)
          << ",\"dest_blend_alpha\":" << static_cast<unsigned int>(desc.DestBlendAlpha)
          << ",\"blend_op_alpha\":" << static_cast<unsigned int>(desc.BlendOpAlpha)
          << ",\"logic_op\":" << static_cast<unsigned int>(desc.LogicOp)
          << ",\"render_target_write_mask\":" << static_cast<unsigned int>(desc.RenderTargetWriteMask)
          << "}";
  return payload.str();
}

std::string blend_desc_json(const D3D12_BLEND_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"alpha_to_coverage_enable\":" << (desc.AlphaToCoverageEnable ? "true" : "false")
          << ",\"independent_blend_enable\":" << (desc.IndependentBlendEnable ? "true" : "false")
          << ",\"render_targets\":[";
  for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
    if (index) {
      payload << ",";
    }
    payload << render_target_blend_desc_json(desc.RenderTarget[index]);
  }
  payload << "]}";
  return payload.str();
}

std::string rasterizer_desc_json(const D3D12_RASTERIZER_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"fill_mode\":" << static_cast<unsigned int>(desc.FillMode)
          << ",\"cull_mode\":" << static_cast<unsigned int>(desc.CullMode)
          << ",\"front_counter_clockwise\":" << (desc.FrontCounterClockwise ? "true" : "false")
          << ",\"depth_bias\":" << desc.DepthBias
          << ",\"depth_bias_clamp\":" << desc.DepthBiasClamp
          << ",\"slope_scaled_depth_bias\":" << desc.SlopeScaledDepthBias
          << ",\"depth_clip_enable\":" << (desc.DepthClipEnable ? "true" : "false")
          << ",\"multisample_enable\":" << (desc.MultisampleEnable ? "true" : "false")
          << ",\"antialiased_line_enable\":" << (desc.AntialiasedLineEnable ? "true" : "false")
          << ",\"forced_sample_count\":" << desc.ForcedSampleCount
          << ",\"conservative_raster\":" << static_cast<unsigned int>(desc.ConservativeRaster)
          << "}";
  return payload.str();
}

std::string depth_stencil_op_desc_json(const D3D12_DEPTH_STENCILOP_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"stencil_fail_op\":" << static_cast<unsigned int>(desc.StencilFailOp)
          << ",\"stencil_depth_fail_op\":" << static_cast<unsigned int>(desc.StencilDepthFailOp)
          << ",\"stencil_pass_op\":" << static_cast<unsigned int>(desc.StencilPassOp)
          << ",\"stencil_func\":" << static_cast<unsigned int>(desc.StencilFunc)
          << "}";
  return payload.str();
}

std::string depth_stencil_desc_json(const D3D12_DEPTH_STENCIL_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"depth_enable\":" << (desc.DepthEnable ? "true" : "false")
          << ",\"depth_write_mask\":" << static_cast<unsigned int>(desc.DepthWriteMask)
          << ",\"depth_func\":" << static_cast<unsigned int>(desc.DepthFunc)
          << ",\"stencil_enable\":" << (desc.StencilEnable ? "true" : "false")
          << ",\"stencil_read_mask\":" << static_cast<unsigned int>(desc.StencilReadMask)
          << ",\"stencil_write_mask\":" << static_cast<unsigned int>(desc.StencilWriteMask)
          << ",\"front_face\":" << depth_stencil_op_desc_json(desc.FrontFace)
          << ",\"back_face\":" << depth_stencil_op_desc_json(desc.BackFace)
          << "}";
  return payload.str();
}

std::string input_layout_json(const D3D12_INPUT_LAYOUT_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"element_count\":" << desc.NumElements << ",\"elements\":[";
  for (UINT index = 0; index < desc.NumElements; ++index) {
    if (index) {
      payload << ",";
    }
    const auto &element = desc.pInputElementDescs[index];
    payload << "{\"semantic_name\":\"";
    const char *semantic_name = element.SemanticName ? element.SemanticName : "";
    for (const char *cursor = semantic_name; *cursor; ++cursor) {
      if (*cursor == '"' || *cursor == '\\') {
        payload << '\\';
      }
      payload << *cursor;
    }
    payload << "\""
            << ",\"semantic_index\":" << element.SemanticIndex
            << ",\"format\":" << static_cast<unsigned int>(element.Format)
            << ",\"input_slot\":" << element.InputSlot
            << ",\"aligned_byte_offset\":" << element.AlignedByteOffset
            << ",\"input_slot_class\":" << static_cast<unsigned int>(element.InputSlotClass)
            << ",\"instance_data_step_rate\":" << element.InstanceDataStepRate
            << "}";
  }
  payload << "]}";
  return payload.str();
}

std::string stream_output_json(const D3D12_STREAM_OUTPUT_DESC &desc)
{
  std::ostringstream payload;
  payload << "{\"declaration_count\":" << desc.NumEntries
          << ",\"stride_count\":" << desc.NumStrides
          << ",\"rasterized_stream\":" << desc.RasterizedStream
          << "}";
  return payload.str();
}

std::string resource_desc_json(const D3D12_RESOURCE_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{\"dimension\":" << static_cast<unsigned int>(desc->Dimension)
          << ",\"alignment\":" << desc->Alignment
          << ",\"width\":" << desc->Width
          << ",\"height\":" << desc->Height
          << ",\"depth_or_array_size\":" << desc->DepthOrArraySize
          << ",\"mip_levels\":" << desc->MipLevels
          << ",\"format\":" << static_cast<unsigned int>(desc->Format)
          << ",\"sample_count\":" << desc->SampleDesc.Count
          << ",\"sample_quality\":" << desc->SampleDesc.Quality
          << ",\"layout\":" << static_cast<unsigned int>(desc->Layout)
          << ",\"flags\":" << static_cast<unsigned int>(desc->Flags)
          << "}";
  return payload.str();
}

std::string cpu_descriptor_json(D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  return std::to_string(static_cast<std::uint64_t>(descriptor.ptr));
}

std::string cpu_descriptor_detail_json(D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  std::ostringstream payload;
  payload << "{\"ptr\":" << static_cast<std::uint64_t>(descriptor.ptr)
          << ",\"descriptor_id\":" << object_id(reinterpret_cast<const void *>(descriptor.ptr))
          << "}";
  return payload.str();
}

std::string gpu_descriptor_json(D3D12_GPU_DESCRIPTOR_HANDLE descriptor)
{
  return std::to_string(static_cast<std::uint64_t>(descriptor.ptr));
}

std::string gpu_descriptor_detail_json(D3D12_GPU_DESCRIPTOR_HANDLE descriptor)
{
  std::ostringstream payload;
  payload << "{\"ptr\":" << static_cast<std::uint64_t>(descriptor.ptr) << "}";
  return payload.str();
}

void append_copy_descriptor_pairs_json(
    std::ostringstream &payload,
    std::uint32_t dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const std::uint32_t *dst_descriptor_range_sizes,
    std::uint32_t src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const std::uint32_t *src_descriptor_range_sizes,
    std::uint32_t descriptor_size)
{
  payload << ",\"descriptors\":[";
  bool first = true;
  std::uint32_t dst_range_index = 0;
  std::uint32_t src_range_index = 0;
  std::uint32_t dst_offset = 0;
  std::uint32_t src_offset = 0;
  while (dst_descriptor_range_starts &&
         src_descriptor_range_starts &&
         dst_range_index < dst_descriptor_range_count &&
         src_range_index < src_descriptor_range_count) {
    const std::uint32_t dst_range_size =
        dst_descriptor_range_sizes ? dst_descriptor_range_sizes[dst_range_index] : 1;
    const std::uint32_t src_range_size =
        src_descriptor_range_sizes ? src_descriptor_range_sizes[src_range_index] : 1;
    if (dst_offset >= dst_range_size) {
      ++dst_range_index;
      dst_offset = 0;
      continue;
    }
    if (src_offset >= src_range_size) {
      ++src_range_index;
      src_offset = 0;
      continue;
    }
    if (!first) {
      payload << ",";
    }
    first = false;
    payload << "{\"dst_descriptor\":"
            << (static_cast<std::uint64_t>(dst_descriptor_range_starts[dst_range_index].ptr) +
                static_cast<std::uint64_t>(dst_offset) * descriptor_size)
            << ",\"src_descriptor\":"
            << (static_cast<std::uint64_t>(src_descriptor_range_starts[src_range_index].ptr) +
                static_cast<std::uint64_t>(src_offset) * descriptor_size)
            << "}";
    ++dst_offset;
    ++src_offset;
  }
  payload << "]";
}

std::vector<CopyDescriptorRange> collect_copy_descriptor_ranges(
    std::uint32_t dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const std::uint32_t *dst_descriptor_range_sizes,
    std::uint32_t src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const std::uint32_t *src_descriptor_range_sizes,
    std::uint32_t descriptor_size)
{
  std::vector<CopyDescriptorRange> ranges;
  std::uint32_t dst_range_index = 0;
  std::uint32_t src_range_index = 0;
  std::uint32_t dst_offset = 0;
  std::uint32_t src_offset = 0;
  while (dst_descriptor_range_starts &&
         src_descriptor_range_starts &&
         dst_range_index < dst_descriptor_range_count &&
         src_range_index < src_descriptor_range_count) {
    const std::uint32_t dst_range_size =
        dst_descriptor_range_sizes ? dst_descriptor_range_sizes[dst_range_index] : 1;
    const std::uint32_t src_range_size =
        src_descriptor_range_sizes ? src_descriptor_range_sizes[src_range_index] : 1;
    if (dst_offset >= dst_range_size) {
      ++dst_range_index;
      dst_offset = 0;
      continue;
    }
    if (src_offset >= src_range_size) {
      ++src_range_index;
      src_offset = 0;
      continue;
    }

    const std::uint64_t dst_descriptor =
        static_cast<std::uint64_t>(dst_descriptor_range_starts[dst_range_index].ptr) +
        static_cast<std::uint64_t>(dst_offset) * descriptor_size;
    const std::uint64_t src_descriptor =
        static_cast<std::uint64_t>(src_descriptor_range_starts[src_range_index].ptr) +
        static_cast<std::uint64_t>(src_offset) * descriptor_size;
    if (!ranges.empty()) {
      auto &previous = ranges.back();
      const auto previous_count = static_cast<std::uint64_t>(previous.count);
      if (previous.dst_descriptor + previous_count * descriptor_size == dst_descriptor &&
          previous.src_descriptor + previous_count * descriptor_size == src_descriptor) {
        ++previous.count;
        ++dst_offset;
        ++src_offset;
        continue;
      }
    }

    ranges.push_back(CopyDescriptorRange{dst_descriptor, src_descriptor, 1});
    ++dst_offset;
    ++src_offset;
  }
  return ranges;
}

void remember_cbv_descriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc)
{
  if (descriptor.ptr == 0) {
    return;
  }
  std::lock_guard lock(g_descriptor_metadata_mutex);
  if (!desc || desc->BufferLocation == 0 || desc->SizeInBytes == 0) {
    g_cbv_descriptors.erase(static_cast<std::uint64_t>(descriptor.ptr));
    return;
  }
  g_cbv_descriptors[static_cast<std::uint64_t>(descriptor.ptr)] = {
      static_cast<std::uint64_t>(desc->BufferLocation),
      desc->SizeInBytes,
  };
}

void forget_cbv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  if (descriptor.ptr == 0) {
    return;
  }
  std::lock_guard lock(g_descriptor_metadata_mutex);
  g_cbv_descriptors.erase(static_cast<std::uint64_t>(descriptor.ptr));
}

void append_unique_cbv_capture_range(
    std::vector<MappedGpuvaUseRange> &ranges,
    const CapturedCbvDescriptor &cbv)
{
  if (cbv.buffer_location == 0 || cbv.size_in_bytes == 0) {
    return;
  }
  const auto duplicate = std::find_if(
      ranges.begin(),
      ranges.end(),
      [&cbv](const MappedGpuvaUseRange &range) {
        return range.valid &&
               range.address == cbv.buffer_location &&
               range.size == cbv.size_in_bytes;
      });
  if (duplicate == ranges.end()) {
    ranges.push_back(MappedGpuvaUseRange{cbv.buffer_location, cbv.size_in_bytes, true});
  }
}

std::vector<MappedGpuvaUseRange> update_cbv_metadata_for_descriptor_copy(
    std::uint32_t descriptor_heap_type,
    std::uint32_t descriptor_size,
    const std::vector<CopyDescriptorRange> &ranges)
{
  std::vector<MappedGpuvaUseRange> capture_ranges;
  if (descriptor_heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
      descriptor_size == 0 ||
      ranges.empty()) {
    return capture_ranges;
  }

  std::vector<CbvDescriptorCopy> copies;
  {
    std::lock_guard lock(g_descriptor_metadata_mutex);
    for (const auto &range : ranges) {
      for (std::uint32_t index = 0; index < range.count; ++index) {
        const auto offset = static_cast<std::uint64_t>(index) * descriptor_size;
        const auto dst_descriptor = range.dst_descriptor + offset;
        const auto src_descriptor = range.src_descriptor + offset;
        CbvDescriptorCopy copy;
        copy.dst_descriptor = dst_descriptor;
        if (const auto found = g_cbv_descriptors.find(src_descriptor);
            found != g_cbv_descriptors.end()) {
          copy.has_cbv = true;
          copy.cbv = found->second;
          append_unique_cbv_capture_range(capture_ranges, copy.cbv);
        }
        copies.push_back(copy);
      }
    }
    for (const auto &copy : copies) {
      if (copy.has_cbv) {
        g_cbv_descriptors[copy.dst_descriptor] = copy.cbv;
      } else {
        g_cbv_descriptors.erase(copy.dst_descriptor);
      }
    }
  }
  return capture_ranges;
}

void capture_cbv_descriptor_copy_ranges_before_use(
    std::uint32_t descriptor_heap_type,
    std::uint32_t descriptor_size,
    const std::vector<CopyDescriptorRange> &ranges)
{
  for (const auto &range :
       update_cbv_metadata_for_descriptor_copy(descriptor_heap_type, descriptor_size, ranges)) {
    capture_mapped_gpuva_range_before_use(range);
  }
}

void append_rect_json(std::ostringstream &payload, const D3D12_RECT &rect)
{
  payload << "{\"left\":" << rect.left
          << ",\"top\":" << rect.top
          << ",\"right\":" << rect.right
          << ",\"bottom\":" << rect.bottom
          << "}";
}

void append_box_json(std::ostringstream &payload, const D3D12_BOX &box)
{
  payload << "{\"left\":" << box.left
          << ",\"top\":" << box.top
          << ",\"front\":" << box.front
          << ",\"right\":" << box.right
          << ",\"bottom\":" << box.bottom
          << ",\"back\":" << box.back
          << "}";
}

std::string render_pass_clear_value_json(const RenderPassClearValue &clear)
{
  std::ostringstream payload;
  payload << "{\"format\":" << clear.format
          << ",\"color\":[";
  for (std::uint32_t index = 0; index < 4; ++index) {
    if (index) {
      payload << ",";
    }
    payload << clear.color[index];
  }
  payload << "],\"depth\":" << clear.depth
          << ",\"stencil\":" << static_cast<std::uint32_t>(clear.stencil)
          << "}";
  return payload.str();
}

std::string render_pass_beginning_access_json(const RenderPassBeginningAccessDesc &access)
{
  std::ostringstream payload;
  payload << "{\"type\":" << access.type
          << ",\"clear\":" << render_pass_clear_value_json(access.clear)
          << "}";
  return payload.str();
}

std::string render_pass_ending_access_json(
    const RenderPassEndingAccessDesc &access,
    std::vector<const void *> &refs)
{
  if (access.src_resource) {
    refs.push_back(access.src_resource);
  }
  if (access.dst_resource) {
    refs.push_back(access.dst_resource);
  }

  std::ostringstream payload;
  payload << "{\"type\":" << access.type
          << ",\"src_resource_object_id\":" << object_id(access.src_resource)
          << ",\"dst_resource_object_id\":" << object_id(access.dst_resource)
          << ",\"subresource_count\":" << access.subresource_count
          << ",\"subresources\":[";
  for (std::uint32_t index = 0; access.subresources && index < access.subresource_count; ++index) {
    if (index) {
      payload << ",";
    }
    const auto &subresource = access.subresources[index];
    payload << "{\"src_subresource\":" << subresource.src_subresource
            << ",\"dst_subresource\":" << subresource.dst_subresource
            << ",\"dst_x\":" << subresource.dst_x
            << ",\"dst_y\":" << subresource.dst_y
            << ",\"src_rect\":";
    if (subresource.has_src_rect) {
      payload << "{\"left\":" << subresource.src_left
              << ",\"top\":" << subresource.src_top
              << ",\"right\":" << subresource.src_right
              << ",\"bottom\":" << subresource.src_bottom
              << "}";
    } else {
      payload << "null";
    }
    payload << "}";
  }
  payload << "],\"format\":" << access.format
          << ",\"resolve_mode\":" << access.resolve_mode
          << ",\"preserve_resolve_source\":" << (access.preserve_resolve_source ? "true" : "false")
          << "}";
  return payload.str();
}

std::string render_pass_render_target_json(
    const RenderPassRenderTargetDesc &render_target,
    std::vector<const void *> &refs)
{
  std::ostringstream payload;
  payload << "{\"cpu_descriptor\":" << render_target.cpu_descriptor
          << ",\"beginning_access\":" << render_pass_beginning_access_json(render_target.beginning_access)
          << ",\"ending_access\":" << render_pass_ending_access_json(render_target.ending_access, refs)
          << "}";
  return payload.str();
}

std::string render_pass_depth_stencil_json(
    const RenderPassDepthStencilDesc &depth_stencil,
    std::vector<const void *> &refs)
{
  std::ostringstream payload;
  payload << "{\"cpu_descriptor\":" << depth_stencil.cpu_descriptor
          << ",\"depth_beginning_access\":" << render_pass_beginning_access_json(depth_stencil.depth_beginning_access)
          << ",\"stencil_beginning_access\":" << render_pass_beginning_access_json(depth_stencil.stencil_beginning_access)
          << ",\"depth_ending_access\":" << render_pass_ending_access_json(depth_stencil.depth_ending_access, refs)
          << ",\"stencil_ending_access\":" << render_pass_ending_access_json(depth_stencil.stencil_ending_access, refs)
          << "}";
  return payload.str();
}

void append_texture_copy_location_json(std::ostringstream &payload, const D3D12_TEXTURE_COPY_LOCATION *location)
{
  if (!location) {
    payload << "null";
    return;
  }
  payload << "{\"resource_object_id\":" << object_id(location->pResource)
          << ",\"type\":" << static_cast<unsigned int>(location->Type);
  if (location->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    payload << ",\"placed_footprint\":{\"offset\":" << location->PlacedFootprint.Offset
            << ",\"format\":" << static_cast<unsigned int>(location->PlacedFootprint.Footprint.Format)
            << ",\"width\":" << location->PlacedFootprint.Footprint.Width
            << ",\"height\":" << location->PlacedFootprint.Footprint.Height
            << ",\"depth\":" << location->PlacedFootprint.Footprint.Depth
            << ",\"row_pitch\":" << location->PlacedFootprint.Footprint.RowPitch
            << "}";
  } else {
    payload << ",\"subresource_index\":" << location->SubresourceIndex;
  }
  payload << "}";
}

TextureCopyLocationCompact compact_texture_copy_location(const D3D12_TEXTURE_COPY_LOCATION *location)
{
  TextureCopyLocationCompact compact;
  if (!location) {
    return compact;
  }
  compact.resource_object_id = object_id(location->pResource);
  compact.type = static_cast<std::uint32_t>(location->Type);
  if (location->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    compact.footprint_offset = location->PlacedFootprint.Offset;
    compact.footprint_format = static_cast<std::uint32_t>(location->PlacedFootprint.Footprint.Format);
    compact.footprint_width = location->PlacedFootprint.Footprint.Width;
    compact.footprint_height = location->PlacedFootprint.Footprint.Height;
    compact.footprint_depth = location->PlacedFootprint.Footprint.Depth;
    compact.footprint_row_pitch = location->PlacedFootprint.Footprint.RowPitch;
  } else {
    compact.subresource_index = location->SubresourceIndex;
  }
  return compact;
}

} // namespace

D3D12CaptureHooks::D3D12CaptureHooks() = default;

void D3D12CaptureHooks::install_proxy_hooks(runtime::CaptureRuntime &runtime)
{
  runtime.install_hooks();
  runtime.extend_hooks_for_module("d3d12.dll");
}

void D3D12CaptureHooks::install_device_hooks(runtime::CaptureRuntime &runtime)
{
  runtime.extend_hooks_for_module("d3d12.dll");
}

void D3D12CaptureHooks::install_submission_hooks(runtime::CaptureRuntime &runtime)
{
  runtime.extend_hooks_for_module("d3d12.dll");
}

bool builtin_capture_enabled()
{
  return runtime::current_process_trace_session() != nullptr ||
         runtime::ensure_process_trace_session(trace::ApiKind::D3D12) != nullptr;
}

std::uint64_t current_sequence()
{
  return g_sequence.load(std::memory_order_relaxed);
}

std::uint64_t object_id(const void *object)
{
  if (!object) {
    return 0;
  }
  std::lock_guard lock(g_object_mutex);
  return lookup_object_id_locked(object);
}

void record_external_object_create_if_needed(
    const void *object,
    CaptureObjectKind kind,
    const char *debug_name,
    const char *payload_json = "{}")
{
  if (!object) {
    return;
  }
  {
    std::lock_guard lock(g_object_mutex);
    if (object_kind_known_locked(object, to_trace_object_kind(kind))) {
      return;
    }
  }
  record_object_create(object, kind, nullptr, debug_name, payload_json);
}

void ensure_external_resource_object(const void *resource, const char *reason)
{
  if (!resource) {
    return;
  }
  std::ostringstream payload;
  payload << "{\"external_resource\":true";
  if (reason && *reason) {
    payload << ",\"reason\":\"" << escape_json_string(reason) << "\"";
  }
  payload << "}";
  record_external_object_create_if_needed(
      resource,
      CaptureObjectKind::Resource,
      "ExternalD3D12Resource",
      payload.str().c_str());
}

bool is_known_resource_object(const void *resource)
{
  std::lock_guard lock(g_object_mutex);
  return object_kind_known_locked(resource, trace::ObjectKind::Resource);
}

bool query_resource_desc(ID3D12Resource *resource, D3D12_RESOURCE_DESC &desc)
{
  if (!resource || !resource->lpVtbl || !resource->lpVtbl->GetDesc) {
    return false;
  }
  resource->lpVtbl->GetDesc(resource, &desc);
  return true;
}

void ensure_external_resource_object_with_desc(ID3D12Resource *resource, const char *reason)
{
  if (!resource) {
    return;
  }
  if (is_known_resource_object(resource)) {
    return;
  }
  D3D12_RESOURCE_DESC desc{};
  const bool has_desc = query_resource_desc(resource, desc);
  std::ostringstream payload;
  payload << "{\"external_resource\":true";
  if (reason && *reason) {
    payload << ",\"reason\":\"" << escape_json_string(reason) << "\"";
  }
  if (has_desc) {
    payload << ",\"resource_desc\":" << resource_desc_json(&desc);
  }
  payload << "}";
  record_external_object_create_if_needed(
      resource,
      CaptureObjectKind::Resource,
      "ExternalD3D12Resource",
      payload.str().c_str());
}

void ensure_external_swapchain_object(const void *swapchain, const char *reason)
{
  if (!swapchain) {
    return;
  }
  std::ostringstream payload;
  payload << "{\"external_swapchain\":true";
  if (reason && *reason) {
    payload << ",\"reason\":\"" << escape_json_string(reason) << "\"";
  }
  payload << "}";
  record_external_object_create_if_needed(
      swapchain,
      CaptureObjectKind::SwapChain,
      "ExternalDXGISwapChain",
      payload.str().c_str());
}

// Forward declaration: defined later in this file; needed by
// record_swapchain_back_buffer below.
std::uint64_t record_call_with_object_ids(
    const char *opname,
    const char *payload_json,
    std::vector<trace::ObjectId> object_refs,
    const std::uint64_t *blob_refs = nullptr,
    std::uint32_t blob_ref_count = 0,
    std::int32_t result_code = 0);

void record_swapchain_back_buffer(
    const void *device,
    const void *swapchain,
    ID3D12Resource *back_buffer,
    std::uint32_t buffer_index)
{
  if (!back_buffer) {
    return;
  }
  // Idempotent: the back buffer is also lazily captured as an external
  // resource the first time it is used as an RTV/barrier target. Emitting the
  // swapchain semantics here, before that first use, registers the resource
  // with the swapchain marker so native retrace can map it onto the real
  // swapchain back buffer and infer the window size. Skip if already known.
  {
    std::lock_guard lock(g_object_mutex);
    if (object_kind_known_locked(back_buffer, trace::ObjectKind::Resource)) {
      return;
    }
  }
  ensure_external_swapchain_object(swapchain, "GetBuffer");
  record_object_create(back_buffer, CaptureObjectKind::Resource, swapchain, "IDXGISwapChainBackBuffer");
  D3D12_RESOURCE_DESC desc{};
  const bool has_desc = query_resource_desc(back_buffer, desc);
  std::ostringstream payload;
  payload << "{\"heap_type\":" << static_cast<unsigned int>(D3D12_HEAP_TYPE_DEFAULT)
          << ",\"heap_flags\":" << static_cast<unsigned int>(D3D12_HEAP_FLAG_NONE)
          << ",\"initial_state\":" << static_cast<unsigned int>(D3D12_RESOURCE_STATE_PRESENT)
          << ",\"gpu_virtual_address\":0"
          << ",\"swapchain_back_buffer\":true"
          << ",\"buffer_index\":" << buffer_index
          << ",\"resource_desc\":" << (has_desc ? resource_desc_json(&desc) : std::string("null"))
          << "}";
  std::vector<trace::ObjectId> refs;
  {
    std::lock_guard lock(g_object_mutex);
    append_object_ref_id(refs, existing_object_id_locked(device));
    append_object_ref_id(refs, lookup_object_id_locked(back_buffer));
  }
  record_call_with_object_ids(
      "ID3D12Device::CreateCommittedResource",
      payload.str().c_str(),
      std::move(refs));
}


std::uint64_t register_blob(const char *debug_name, const void *data, std::size_t size)
{
  return register_asset_bytes(trace::AssetKind::Unknown, debug_name, data, size).blob_id;
}

std::uint64_t record_call(
    const char *opname,
    const char *payload_json,
    const void *const *object_refs,
    std::uint32_t object_ref_count,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code)
{
  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  flush_command_list_batches();
  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  record_call_with_sequence(
      sequence,
      opname,
      payload_json,
      object_refs,
      object_ref_count,
      blob_refs,
      blob_ref_count,
      result_code);
  return sequence;
}

std::uint64_t record_call_with_object_ids(
    const char *opname,
    const char *payload_json,
    std::vector<trace::ObjectId> object_refs,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code)
{
  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  flush_command_list_batches();
  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  record_call_event_unbatched_with_object_ids(
      sequence,
      opname,
      payload_json,
      std::move(object_refs),
      blob_refs,
      blob_ref_count,
      result_code);
  return sequence;
}

void record_call_with_sequence(
    std::uint64_t sequence,
    const char *opname,
    const char *payload_json,
    const void *const *object_refs,
    std::uint32_t object_ref_count,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    std::int32_t result_code)
{
  record_call_event_unbatched(
      sequence,
      opname,
      payload_json,
      object_refs,
      object_ref_count,
      blob_refs,
      blob_ref_count,
      result_code);
}

void record_object_create(
    const void *object,
    CaptureObjectKind kind,
    const void *parent_object,
    const char *debug_name,
    const char *payload_json)
{
  if (!object) {
    return;
  }

  trace::ObjectRecord object_record;
  {
    std::lock_guard lock(g_object_mutex);
    object_record.object_id = lookup_object_id_locked(object);
    object_record.parent_object_id = lookup_object_id_locked(parent_object);
    g_object_kinds[object] = to_trace_object_kind(kind);
  }
  object_record.kind = to_trace_object_kind(kind);
  object_record.debug_name = debug_name ? debug_name : "";

  if (auto *session = session_for(trace::ApiKind::D3D12)) {
    std::lock_guard event_lock(g_event_order_mutex);
    session->record_object(object_record);
    flush_diagnostic_batches();
    flush_command_list_batches();
    trace::EventRecord event;
    event.kind = trace::EventKind::ObjectCreate;
    event.callsite.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    event.callsite.function_name = "D3DObjectCreate";
    event.object_id = object_record.object_id;
    event.object_kind = object_record.kind;
    event.parent_object_id = object_record.parent_object_id;
    event.object_debug_name = object_record.debug_name;
    event.payload = payload_json && *payload_json ? payload_json : "{}";
    session->append_call_event(std::move(event));
  }
}

void record_object_destroy(const void *object, CaptureObjectKind kind, const char *payload_json)
{
  const auto id = object_id(object);
  if (!id) {
    return;
  }
  if (auto *session = session_for(trace::ApiKind::D3D12)) {
    std::lock_guard event_lock(g_event_order_mutex);
    flush_diagnostic_batches();
    flush_command_list_batches();
    trace::EventRecord event;
    event.kind = trace::EventKind::ObjectDestroy;
    event.callsite.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    event.callsite.function_name = "D3DObjectDestroy";
    event.object_id = id;
    event.object_kind = to_trace_object_kind(kind);
    event.payload = payload_json && *payload_json ? payload_json : "{}";
    session->append_call_event(std::move(event));
  }
  if (kind == CaptureObjectKind::Resource) {
    std::lock_guard lock(g_object_mutex);
    g_resource_gpu_virtual_addresses.erase(object);
    g_mapped_resources.erase(object);
    g_object_kinds.erase(object);
  }
}

void record_resource_blob(
    const char *debug_name,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    const char *payload_json)
{
  if (auto *session = session_for(trace::ApiKind::D3D12)) {
    std::lock_guard event_lock(g_event_order_mutex);
    flush_diagnostic_batches();
    flush_command_list_batches();
    trace::EventRecord event;
    event.kind = trace::EventKind::ResourceBlob;
    event.callsite.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    event.callsite.function_name = "D3DResourceBlob";
    event.object_debug_name = debug_name ? debug_name : "";
    event.blob_refs = collect_blob_refs(blob_refs, blob_ref_count);
    event.payload = payload_json && *payload_json ? payload_json : "{}";
    session->append_call_event(std::move(event));
  }
}

std::uint64_t record_d3d12_create_device(const void *device)
{
  if (!device) {
    return 0;
  }
  record_object_create(device, CaptureObjectKind::Device, nullptr, "ID3D12Device");
  const void *refs[] = {device};
  return record_call("D3D12CreateDevice", "{\"minimum_feature_level\":45056}", refs, 1);
}

std::uint64_t record_d3d11_create_device(const void *device)
{
  if (!device) {
    return 0;
  }
  const void *refs[] = {device};
  return record_call("D3D11CreateDevice", "{}", refs, 1);
}

std::uint64_t record_dxgi_create_swapchain(
    const void *factory,
    const void *device,
    const void *swapchain)
{
  if (!swapchain) {
    return 0;
  }
  record_object_create(swapchain, CaptureObjectKind::SwapChain, factory, "IDXGISwapChain");
  std::vector<trace::ObjectId> refs;
  {
    std::lock_guard lock(g_object_mutex);
    append_object_ref_id(refs, existing_object_id_locked(device));
    append_object_ref_id(refs, lookup_object_id_locked(swapchain));
  }
  return record_call_with_object_ids("IDXGIFactory::CreateSwapChain", "{}", std::move(refs));
}

std::uint64_t record_execute_command_lists(const void *queue, const void *command_list)
{
  if (!queue || !command_list) {
    return 0;
  }
  capture_command_list_mapped_inputs_before_submit(command_list);
  const void *refs[] = {queue, command_list};
  return record_call("ID3D12CommandQueue::ExecuteCommandLists", "{\"command_list_count\":1}", refs, 2);
}

std::uint64_t record_present(
    const void *swapchain,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    std::int32_t result_code,
    bool frame_presented)
{
  if (!swapchain) {
    return 0;
  }
  ensure_external_swapchain_object(swapchain, "Present");
  std::uint64_t frame_index = UINT64_MAX;
  {
    std::lock_guard event_lock(g_event_order_mutex);
    flush_diagnostic_batches();
    flush_command_list_batches();

    // Frame-end re-snapshot of the ranges USED THIS FRAME (BOUNDED + env-gated; default
    // OFF so normal play is untouched). BUG: the homepage CG composite VS dynamically
    // indexes a mapped cbuffer (a per-element transform palette via TEXCOORD-as-index);
    // the submit-time snapshot can record a stale ZERO for that palette entry (the value
    // settles relative to the async GPU read after submit), so retrace renders the
    // composite off-screen and the CG never composites. Re-reading the frame's used
    // ranges here, after the frame's CPU writes settle, captures the value the GPU reads;
    // the palette is stable across frames so the next frame's composite reconstructs
    // correctly. NOTE: must stay bounded - g_mapped_resources.captured_ranges accumulates
    // for the whole session, so we re-snapshot ONLY this frame's ranges (g_present_recapture
    // _ranges, cleared every present), never the full history.
    maybe_recapture_present_ranges();

    const auto present_test = is_present_test(flags);
    const auto has_frame_index = frame_presented && !present_test && result_code == 0;
    frame_index = has_frame_index ? next_present_frame_index() : UINT64_MAX;
    if (has_frame_index) {
      const auto frame_begin_sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
      std::ostringstream frame_begin_payload;
      frame_begin_payload << "{\"label\":\"FrameBegin\",\"frame_index\":" << frame_index << "}";
      record_boundary_event_unbatched(
          frame_begin_sequence,
          trace::BoundaryKind::Frame,
          frame_begin_payload.str().c_str());
    }

    std::ostringstream payload;
    payload << "{";
    if (has_frame_index) {
      payload << "\"frame_index\":" << frame_index << ",";
    }
    payload << "\"sync_interval\":" << sync_interval
            << ",\"flags\":" << flags;
    if (present_test) {
      payload << ",\"present_test\":true";
    } else if (!has_frame_index) {
      payload << ",\"frame_presented\":false";
    }
    payload << "}";
    const void *refs[] = {swapchain};
    const auto present_sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    record_call_event_unbatched(
        present_sequence,
        "IDXGISwapChain::Present",
        payload.str().c_str(),
        refs,
        1,
        nullptr,
        0,
        result_code);
    if (!has_frame_index) {
      return UINT64_MAX;
    }

    std::ostringstream present_boundary_payload;
    present_boundary_payload << "{\"label\":\"Present\",\"frame_index\":" << frame_index
                             << ",\"sync_interval\":" << sync_interval
                             << ",\"flags\":" << flags << "}";
    const auto present_boundary_sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    record_boundary_event_unbatched(
        present_boundary_sequence,
        trace::BoundaryKind::Present,
        present_boundary_payload.str().c_str());

    std::ostringstream frame_end_payload;
    frame_end_payload << "{\"label\":\"FrameEnd\",\"frame_index\":" << frame_index << "}";
    const auto frame_end_sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    record_boundary_event_unbatched(
        frame_end_sequence,
        trace::BoundaryKind::Frame,
        frame_end_payload.str().c_str());
  }
  maybe_seal_checkpoint_after_present(frame_index);
  return frame_index;
}

void record_present_frame(
    std::uint64_t frame_index,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t row_pitch,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    const void *rgba_data,
    std::size_t rgba_size)
{
  if (frame_index == UINT64_MAX ||
      !rgba_data || width == 0 || height == 0 || row_pitch == 0 || rgba_size == 0) {
    return;
  }

  const auto asset = register_asset_bytes(
      trace::AssetKind::Texture,
      "d3d12-present-frame",
      rgba_data,
      rgba_size);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  std::ostringstream payload;
  payload << "{\"frame_index\":" << frame_index
          << ",\"width\":" << width
          << ",\"height\":" << height
          << ",\"row_pitch\":" << row_pitch
          << ",\"sync_interval\":" << sync_interval
          << ",\"flags\":" << flags
          << ",\"format\":\"rgba8\""
          << ",\"frame_path\":\"" << asset.relative_path.generic_string() << "\""
          << "}";
  const auto blob_id = static_cast<std::uint64_t>(asset.blob_id);
  record_resource_blob("D3D12PresentFrame", &blob_id, 1, payload.str().c_str());
}

void record_resource_unmap(
    const void *resource,
    std::uint32_t subresource,
    std::uint64_t written_begin,
    std::uint64_t written_end,
    const void *written_data,
    std::size_t written_size)
{
  if (!resource || !written_data || written_size == 0 || written_end <= written_begin) {
    return;
  }

  if (auto *session = session_for(trace::ApiKind::D3D12);
      session && session->capture_raw_mode() == runtime::CaptureOptions::CaptureRawMode::RawOnly) {
    const auto resource_object_id = object_id(resource);
    std::lock_guard event_lock(g_event_order_mutex);
    record_rawonly_resource_unmap(
        session,
        resource,
        resource_object_id,
        subresource,
        written_begin,
        written_end,
        written_data,
        written_size);
    return;
  }

  const auto written_hash = fnv1a64_bytes(written_data, written_size);

  const auto asset = register_asset_bytes(
      trace::AssetKind::Buffer,
      "d3d12-resource-unmap",
      written_data,
      written_size);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  const auto buffer_path = asset.relative_path.generic_string();
  std::string payload;
  payload.reserve(192 + buffer_path.size());
  payload += "{\"resource_object_id\":";
  payload += std::to_string(object_id(resource));
  payload += ",\"subresource\":";
  payload += std::to_string(subresource);
  payload += ",\"written_begin\":";
  payload += std::to_string(written_begin);
  payload += ",\"written_end\":";
  payload += std::to_string(written_end);
  payload += ",\"written_size\":";
  payload += std::to_string(static_cast<std::uint64_t>(written_size));
  payload += ",\"buffer_path\":\"";
  payload += buffer_path;
  payload += "\"}";
  const void *refs[] = {resource};
  const auto blob_id = static_cast<std::uint64_t>(asset.blob_id);
  record_call(
      "ID3D12Resource::Unmap",
      payload.c_str(),
      refs,
      1,
      &blob_id,
      1);
  {
    std::lock_guard lock(g_object_mutex);
    auto mapped = g_mapped_resources.find(resource);
    if (mapped != g_mapped_resources.end() && mapped->second.subresource == subresource) {
      mark_mapped_range_captured(mapped->second, written_begin, written_end, written_hash);
    }
  }
}

#if defined(APITRACE_ENABLE_TEST_HOOKS)
void reset_raw_unmap_fast_path_for_test()
{
  std::lock_guard lock(g_raw_unmap_mutex);
  g_raw_unmap_signature_session = nullptr;
  g_raw_unmap_signature_bundle_root.clear();
  g_raw_unmap_signature_initial_sequence = 0;
  g_raw_unmap_signatures.clear();
  g_raw_unmap_counters = RawUnmapCounters{};
}

RawUnmapFastPathCounters raw_unmap_fast_path_counters_for_test()
{
  std::lock_guard lock(g_raw_unmap_mutex);
  return RawUnmapFastPathCounters{
      g_raw_unmap_counters.unmap_candidates,
      g_raw_unmap_counters.unchanged_skipped,
      g_raw_unmap_counters.emitted_blob_bytes,
      g_raw_unmap_counters.raw_write_failures};
}
#endif

void record_resource_bytes_snapshot(
    trace::ObjectId resource_object_id,
    std::uint64_t begin,
    std::uint64_t end,
    const void *bytes,
    std::uint64_t sequence)
{
  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  flush_command_list_batches();
  record_resource_bytes_snapshot_unbatched(resource_object_id, begin, end, bytes,
                                          sequence);
}

std::uint64_t record_resource_map(
    const void *resource,
    std::uint32_t subresource,
    bool has_read_range,
    std::uint64_t read_begin,
    std::uint64_t read_end,
    const void *mapped_data,
    bool mapped,
    std::int32_t result_code)
{
  {
    std::lock_guard lock(g_object_mutex);
    if (mapped && mapped_data) {
      g_mapped_resources[resource] = MappedResourceState{mapped_data, subresource};
    } else {
      g_mapped_resources.erase(resource);
    }
  }

  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource;
  if (has_read_range) {
    payload << ",\"read_begin\":" << read_begin
            << ",\"read_end\":" << read_end;
  } else {
    payload << ",\"read_range\":null";
  }
  payload << ",\"mapped\":" << (mapped ? "true" : "false") << "}";
  const void *refs[] = {resource};
  return record_call(
      "ID3D12Resource::Map",
      payload.str().c_str(),
      refs,
      1,
      nullptr,
      0,
      result_code);
}

void record_resolve_query_data_result(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t start_index,
    std::uint32_t query_count,
    const void *dst_buffer,
    std::uint64_t aligned_dst_buffer_offset,
    const void *resolved_data,
    std::size_t resolved_size)
{
  if (!dst_buffer || !resolved_data || resolved_size == 0) {
    return;
  }

  const auto asset = register_asset_bytes(
      trace::AssetKind::Buffer,
      "d3d12-query-resolve",
      resolved_data,
      resolved_size);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id(query_heap)
          << ",\"type\":" << type
          << ",\"start_index\":" << start_index
          << ",\"query_count\":" << query_count
          << ",\"dst_buffer_object_id\":" << object_id(dst_buffer)
          << ",\"aligned_dst_buffer_offset\":" << aligned_dst_buffer_offset
          << ",\"resolved_size\":" << static_cast<std::uint64_t>(resolved_size)
          << ",\"buffer_path\":\"" << asset.relative_path.generic_string() << "\""
          << ",\"resolved_data_hex\":\"" << hex_encode_bytes(resolved_data, resolved_size) << "\""
          << "}";
  const void *refs[] = {command_list, query_heap, dst_buffer};
  const auto blob_id = static_cast<std::uint64_t>(asset.blob_id);
  record_call(
      "ID3D12GraphicsCommandList::ResolveQueryDataResult",
      payload.str().c_str(),
      refs,
      3,
      &blob_id,
      1);
}

void record_fence_dependency(
    const char *scope,
    std::uint64_t d3d_sequence,
    std::uint64_t encoder_id,
    bool implicit_pre_raster_wait,
    const std::uint64_t *strong_masks,
    std::uint32_t strong_count,
    const std::uint64_t *full_masks,
    std::uint32_t full_count,
    const std::uint64_t *minimal_masks,
    std::uint32_t minimal_count,
    std::uint32_t mask_count)
{
  std::lock_guard event_lock(g_event_order_mutex);
  (void)strong_masks;
  (void)full_masks;
  (void)minimal_masks;
  FenceDependencyOp op;
  op.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  op.scope = scope ? scope : "unknown";
  op.d3d_sequence = d3d_sequence;
  op.encoder_id = encoder_id;
  op.implicit_pre_raster_wait = implicit_pre_raster_wait;
  op.strong_count = strong_count;
  op.full_count = full_count;
  op.minimal_count = minimal_count;
  op.mask_count = mask_count;
  std::lock_guard lock(g_diagnostic_batch_mutex);
  g_fence_dependency_batch.ops.push_back(std::move(op));
}

std::uint64_t record_create_command_queue(
    ID3D12Device *device,
    const D3D12_COMMAND_QUEUE_DESC *desc,
    const void *command_queue,
    std::int32_t result_code)
{
  if (command_queue && result_code >= 0) {
    record_object_create(command_queue, CaptureObjectKind::CommandQueue, device, "ID3D12CommandQueue");
  }
  std::ostringstream payload;
  payload << "{\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0)
          << ",\"priority\":" << (desc ? desc->Priority : 0)
          << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
          << ",\"node_mask\":" << (desc ? desc->NodeMask : 0)
          << "}";
  const void *refs[] = {device, command_queue};
  return record_call("ID3D12Device::CreateCommandQueue", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_command_allocator(
    ID3D12Device *device,
    std::uint32_t type,
    const void *command_allocator,
    std::int32_t result_code)
{
  if (command_allocator && result_code >= 0) {
    record_object_create(command_allocator, CaptureObjectKind::CommandAllocator, device, "ID3D12CommandAllocator");
  }
  std::ostringstream payload;
  payload << "{\"type\":" << type << "}";
  const void *refs[] = {device, command_allocator};
  return record_call("ID3D12Device::CreateCommandAllocator", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_command_list(
    ID3D12Device *device,
    std::uint32_t node_mask,
    std::uint32_t type,
    const void *command_allocator,
    const void *initial_pipeline_state,
    const void *command_list,
    std::int32_t result_code)
{
  if (command_list && result_code >= 0) {
    record_object_create(command_list, CaptureObjectKind::CommandList, device, "ID3D12GraphicsCommandList");
  }
  std::ostringstream payload;
  payload << "{\"node_mask\":" << node_mask
          << ",\"type\":" << type
          << ",\"command_allocator_object_id\":" << object_id(command_allocator)
          << ",\"initial_pipeline_state_object_id\":" << object_id(initial_pipeline_state)
          << "}";
  const void *refs[] = {device, command_allocator, initial_pipeline_state, command_list};
  return record_call("ID3D12Device::CreateCommandList", payload.str().c_str(), refs, 4, nullptr, 0, result_code);
}

std::uint64_t record_create_command_list1(
    ID3D12Device *device,
    std::uint32_t node_mask,
    std::uint32_t type,
    std::uint32_t flags,
    const void *command_list,
    std::int32_t result_code)
{
  if (command_list && result_code >= 0) {
    record_object_create(command_list, CaptureObjectKind::CommandList, device, "ID3D12GraphicsCommandList");
  }
  std::ostringstream payload;
  payload << "{\"node_mask\":" << node_mask
          << ",\"type\":" << type
          << ",\"flags\":" << flags
          << "}";
  const void *refs[] = {device, command_list};
  return record_call("ID3D12Device::CreateCommandList1", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_graphics_pipeline_state(
    ID3D12Device *device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state,
    std::int32_t result_code)
{
  if (pipeline_state && result_code >= 0) {
    record_object_create(pipeline_state, CaptureObjectKind::PipelineState, device, "ID3D12PipelineState");
  }
  std::vector<trace::BlobId> shader_blob_refs;
  std::vector<trace::BlobId> blob_refs;
  std::ostringstream payload;
  payload << "{";
  if (desc) {
    payload << "\"pso_raw_version\":1"
             << ",\"pso_kind\":\"graphics\""
             << ",\"root_signature_object_id\":" << object_id(desc->pRootSignature)
             << ",\"node_mask\":" << desc->NodeMask
             << ",\"flags\":" << static_cast<unsigned int>(desc->Flags)
             << ",\"input_layout\":" << input_layout_json(desc->InputLayout)
             << ",\"blend_state\":" << blend_desc_json(desc->BlendState)
             << ",\"sample_mask\":" << desc->SampleMask
             << ",\"rasterizer_state\":" << rasterizer_desc_json(desc->RasterizerState)
             << ",\"depth_stencil_state\":" << depth_stencil_desc_json(desc->DepthStencilState)
             << ",\"stream_output\":" << stream_output_json(desc->StreamOutput)
             << ",\"primitive_topology_type\":" << static_cast<unsigned int>(desc->PrimitiveTopologyType)
             << ",\"num_render_targets\":" << desc->NumRenderTargets
             << ",\"rtv_formats\":[";
    for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
      if (index) {
        payload << ",";
      }
      payload << static_cast<unsigned int>(desc->RTVFormats[index]);
    }
    payload << "]"
             << ",\"dsv_format\":" << static_cast<unsigned int>(desc->DSVFormat)
             << ",\"sample_desc\":{\"count\":" << desc->SampleDesc.Count
             << ",\"quality\":" << desc->SampleDesc.Quality << "}"
             << ",\"ib_strip_cut_value\":" << static_cast<unsigned int>(desc->IBStripCutValue)
             << "," << raw_shader_asset_json("vs", desc->VS, shader_blob_refs)
             << "," << raw_shader_asset_json("ps", desc->PS, shader_blob_refs)
             << "," << raw_shader_asset_json("ds", desc->DS, shader_blob_refs)
             << "," << raw_shader_asset_json("hs", desc->HS, shader_blob_refs)
             << "," << raw_shader_asset_json("gs", desc->GS, shader_blob_refs);
  }
  payload << "}";
  blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
  const void *refs[] = {device, desc ? desc->pRootSignature : nullptr, pipeline_state};
  return record_call(
      "ID3D12Device::CreateGraphicsPipelineState",
      payload.str().c_str(),
      refs,
      3,
      reinterpret_cast<const std::uint64_t *>(blob_refs.data()),
      blob_refs.size(),
      result_code);
}

std::uint64_t record_create_compute_pipeline_state(
    ID3D12Device *device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state,
    std::int32_t result_code)
{
  if (pipeline_state && result_code >= 0) {
    record_object_create(pipeline_state, CaptureObjectKind::PipelineState, device, "ID3D12PipelineState");
  }
  std::vector<trace::BlobId> shader_blob_refs;
  std::vector<trace::BlobId> blob_refs;
  std::ostringstream payload;
  payload << "{";
  if (desc) {
    payload << "\"pso_raw_version\":1"
             << ",\"pso_kind\":\"compute\""
             << ",\"root_signature_object_id\":" << object_id(desc->pRootSignature)
             << ",\"node_mask\":" << desc->NodeMask
             << ",\"flags\":" << static_cast<unsigned int>(desc->Flags)
             << "," << raw_shader_asset_json("cs", desc->CS, shader_blob_refs);
    blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
  }
  payload << "}";
  const void *refs[] = {device, desc ? desc->pRootSignature : nullptr, pipeline_state};
  return record_call(
      "ID3D12Device::CreateComputePipelineState",
      payload.str().c_str(),
      refs,
      3,
      reinterpret_cast<const std::uint64_t *>(blob_refs.data()),
      blob_refs.size(),
      result_code);
}

std::uint64_t record_create_pipeline_state(
    ID3D12Device *device,
    const void *stream_data,
    std::size_t stream_size,
    const void *pipeline_state,
    std::int32_t result_code)
{
  if (pipeline_state && result_code >= 0) {
    record_object_create(pipeline_state, CaptureObjectKind::PipelineState, device, "ID3D12PipelineState");
  }

  std::vector<trace::BlobId> shader_blob_refs;
  std::vector<trace::BlobId> blob_refs;
  std::vector<const void *> refs;
  refs.push_back(device);
  refs.push_back(pipeline_state);
  std::ostringstream payload;
  payload << "{";
  if (stream_data && stream_size > 0) {
    const auto *bytes = static_cast<const std::uint8_t *>(stream_data);
    std::size_t offset = 0;
    bool first_subobject = true;
    bool has_cs = false;
    bool has_as = false;
    bool has_ms = false;
    StreamShaderAssetJson shader_json;
    StreamShaderMetadataJson shader_metadata_json;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.SampleMask = UINT_MAX;
    graphics.SampleDesc.Count = 1;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};

    std::ostringstream stream;
    stream << "{\"source\":\"stream\",\"subobjects\":[";
    while (offset < stream_size) {
      if (stream_size - offset < sizeof(std::uint32_t)) {
        break;
      }

      const auto type_offset = offset;
      const auto raw_type = *reinterpret_cast<const std::uint32_t *>(bytes + offset);
      const auto type = static_cast<PipelineStateSubobjectType>(raw_type);
      offset += sizeof(std::uint32_t);
      const auto payload_size = pipeline_stream_payload_size(type);
      const auto payload_alignment = pipeline_stream_payload_alignment(type);
      if (!payload_size || !payload_alignment) {
        break;
      }
      offset = align_stream_offset(offset, payload_alignment);
      if (stream_size - offset < payload_size) {
        break;
      }

      const auto *subobject = bytes + offset;
      if (!first_subobject) {
        stream << ",";
      }
      first_subobject = false;
      stream << "{\"type\":" << raw_type
             << ",\"type_offset\":" << static_cast<std::uint64_t>(type_offset);

      switch (type) {
      case PipelineStateSubobjectType::RootSignature: {
        const auto root_signature = *reinterpret_cast<ID3D12RootSignature *const *>(subobject);
        graphics.pRootSignature = root_signature;
        compute.pRootSignature = root_signature;
        refs.push_back(root_signature);
        stream << ",\"root_signature_object_id\":" << object_id(root_signature);
        break;
      }
      case PipelineStateSubobjectType::VS:
        graphics.VS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        {
          const auto shader = shader_asset_metadata_json("vs", graphics.VS, shader_blob_refs);
          shader_json.vs = shader.asset_json;
          shader_metadata_json.vs = shader.metadata_json;
        }
        stream << "," << shader_metadata_json.vs;
        break;
      case PipelineStateSubobjectType::PS:
        graphics.PS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        {
          const auto shader = shader_asset_metadata_json("ps", graphics.PS, shader_blob_refs);
          shader_json.ps = shader.asset_json;
          shader_metadata_json.ps = shader.metadata_json;
        }
        stream << "," << shader_metadata_json.ps;
        break;
      case PipelineStateSubobjectType::DS:
        graphics.DS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        {
          const auto shader = shader_asset_metadata_json("ds", graphics.DS, shader_blob_refs);
          shader_json.ds = shader.asset_json;
          shader_metadata_json.ds = shader.metadata_json;
        }
        stream << "," << shader_metadata_json.ds;
        break;
      case PipelineStateSubobjectType::HS:
        graphics.HS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        {
          const auto shader = shader_asset_metadata_json("hs", graphics.HS, shader_blob_refs);
          shader_json.hs = shader.asset_json;
          shader_metadata_json.hs = shader.metadata_json;
        }
        stream << "," << shader_metadata_json.hs;
        break;
      case PipelineStateSubobjectType::GS:
        graphics.GS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        {
          const auto shader = shader_asset_metadata_json("gs", graphics.GS, shader_blob_refs);
          shader_json.gs = shader.asset_json;
          shader_metadata_json.gs = shader.metadata_json;
        }
        stream << "," << shader_metadata_json.gs;
        break;
      case PipelineStateSubobjectType::CS:
        compute.CS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        has_cs = compute.CS.pShaderBytecode && compute.CS.BytecodeLength > 0;
        {
          const auto shader = shader_asset_metadata_json("cs", compute.CS, shader_blob_refs);
          shader_json.cs = shader.asset_json;
          shader_metadata_json.cs = shader.metadata_json;
        }
        stream << "," << shader_metadata_json.cs;
        break;
      case PipelineStateSubobjectType::AS: {
        const auto shader = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        has_as = shader.pShaderBytecode && shader.BytecodeLength > 0;
        const auto shader_json_fields = shader_asset_metadata_json("as", shader, shader_blob_refs);
        shader_json.as = shader_json_fields.asset_json;
        shader_metadata_json.as = shader_json_fields.metadata_json;
        stream << "," << shader_metadata_json.as;
        break;
      }
      case PipelineStateSubobjectType::MS: {
        const auto shader = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
        has_ms = shader.pShaderBytecode && shader.BytecodeLength > 0;
        const auto shader_json_fields = shader_asset_metadata_json("ms", shader, shader_blob_refs);
        shader_json.ms = shader_json_fields.asset_json;
        shader_metadata_json.ms = shader_json_fields.metadata_json;
        stream << "," << shader_metadata_json.ms;
        break;
      }
      case PipelineStateSubobjectType::StreamOutput:
        graphics.StreamOutput = *reinterpret_cast<const D3D12_STREAM_OUTPUT_DESC *>(subobject);
        stream << ",\"stream_output\":" << stream_output_json(graphics.StreamOutput);
        break;
      case PipelineStateSubobjectType::Blend:
        graphics.BlendState = *reinterpret_cast<const D3D12_BLEND_DESC *>(subobject);
        stream << ",\"blend_state\":" << blend_desc_json(graphics.BlendState);
        break;
      case PipelineStateSubobjectType::SampleMask:
        graphics.SampleMask = *reinterpret_cast<const UINT *>(subobject);
        stream << ",\"sample_mask\":" << graphics.SampleMask;
        break;
      case PipelineStateSubobjectType::Rasterizer:
        graphics.RasterizerState = *reinterpret_cast<const D3D12_RASTERIZER_DESC *>(subobject);
        stream << ",\"rasterizer_state\":" << rasterizer_desc_json(graphics.RasterizerState);
        break;
      case PipelineStateSubobjectType::DepthStencil:
        graphics.DepthStencilState = *reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC *>(subobject);
        stream << ",\"depth_stencil_state\":" << depth_stencil_desc_json(graphics.DepthStencilState);
        break;
      case PipelineStateSubobjectType::DepthStencil1: {
        const auto &depth_stencil = *reinterpret_cast<const DepthStencilDesc1 *>(subobject);
        graphics.DepthStencilState.DepthEnable = depth_stencil.DepthEnable;
        graphics.DepthStencilState.DepthWriteMask = depth_stencil.DepthWriteMask;
        graphics.DepthStencilState.DepthFunc = depth_stencil.DepthFunc;
        graphics.DepthStencilState.StencilEnable = depth_stencil.StencilEnable;
        graphics.DepthStencilState.StencilReadMask = depth_stencil.StencilReadMask;
        graphics.DepthStencilState.StencilWriteMask = depth_stencil.StencilWriteMask;
        graphics.DepthStencilState.FrontFace = depth_stencil.FrontFace;
        graphics.DepthStencilState.BackFace = depth_stencil.BackFace;
        stream << ",\"depth_stencil_state\":" << depth_stencil_desc_json(graphics.DepthStencilState)
               << ",\"depth_bounds_test_enable\":" << (depth_stencil.DepthBoundsTestEnable ? "true" : "false");
        break;
      }
      case PipelineStateSubobjectType::InputLayout:
        graphics.InputLayout = *reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC *>(subobject);
        stream << ",\"input_layout\":" << input_layout_json(graphics.InputLayout);
        break;
      case PipelineStateSubobjectType::IbStripCutValue:
        graphics.IBStripCutValue = *reinterpret_cast<const D3D12_INDEX_BUFFER_STRIP_CUT_VALUE *>(subobject);
        stream << ",\"ib_strip_cut_value\":" << static_cast<unsigned int>(graphics.IBStripCutValue);
        break;
      case PipelineStateSubobjectType::PrimitiveTopology:
        graphics.PrimitiveTopologyType = *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE *>(subobject);
        stream << ",\"primitive_topology_type\":" << static_cast<unsigned int>(graphics.PrimitiveTopologyType);
        break;
      case PipelineStateSubobjectType::RenderTargetFormats: {
        const auto &formats = *reinterpret_cast<const RtFormatArray *>(subobject);
        graphics.NumRenderTargets = formats.NumRenderTargets;
        stream << ",\"num_render_targets\":" << formats.NumRenderTargets << ",\"rtv_formats\":[";
        for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
          if (index) {
            stream << ",";
          }
          const auto format = index < formats.NumRenderTargets ? formats.RTFormats[index] : DXGI_FORMAT_UNKNOWN;
          graphics.RTVFormats[index] = format;
          stream << static_cast<unsigned int>(format);
        }
        stream << "]";
        break;
      }
      case PipelineStateSubobjectType::DepthStencilFormat:
        graphics.DSVFormat = *reinterpret_cast<const DXGI_FORMAT *>(subobject);
        stream << ",\"dsv_format\":" << static_cast<unsigned int>(graphics.DSVFormat);
        break;
      case PipelineStateSubobjectType::SampleDesc:
        graphics.SampleDesc = *reinterpret_cast<const DXGI_SAMPLE_DESC *>(subobject);
        stream << ",\"sample_desc\":{\"count\":" << graphics.SampleDesc.Count
               << ",\"quality\":" << graphics.SampleDesc.Quality << "}";
        break;
      case PipelineStateSubobjectType::NodeMask:
        graphics.NodeMask = *reinterpret_cast<const UINT *>(subobject);
        compute.NodeMask = graphics.NodeMask;
        stream << ",\"node_mask\":" << graphics.NodeMask;
        break;
      case PipelineStateSubobjectType::CachedPso: {
        const auto &cached = *reinterpret_cast<const D3D12_CACHED_PIPELINE_STATE *>(subobject);
        graphics.CachedPSO = cached;
        compute.CachedPSO = cached;
        stream << ",\"cached_pso_size\":" << static_cast<std::uint64_t>(cached.CachedBlobSizeInBytes);
        break;
      }
      case PipelineStateSubobjectType::Flags:
        graphics.Flags = *reinterpret_cast<const D3D12_PIPELINE_STATE_FLAGS *>(subobject);
        compute.Flags = graphics.Flags;
        stream << ",\"flags\":" << static_cast<unsigned int>(graphics.Flags);
        break;
      case PipelineStateSubobjectType::ViewInstancing: {
        const auto &view_instancing = *reinterpret_cast<const ViewInstancingDesc *>(subobject);
        stream << ",\"view_instance_count\":" << view_instancing.ViewInstanceCount;
        break;
      }
      default:
        break;
      }
      stream << "}";
      offset = align_stream_offset(offset + payload_size, sizeof(void *));
    }
    stream << "]";

    if (has_cs && !has_as && !has_ms) {
      payload << "\"pso_raw_version\":1"
               << ",\"pso_kind\":\"compute\""
               << ",\"source\":\"stream\""
               << ",\"stream_size\":" << static_cast<std::uint64_t>(stream_size)
               << ",\"root_signature_object_id\":" << object_id(compute.pRootSignature)
               << ",\"node_mask\":" << compute.NodeMask
               << ",\"flags\":" << static_cast<unsigned int>(compute.Flags)
               << "," << shader_metadata_json.cs
               << ",\"requires_dxmt_backend\":false"
               << ",\"stream_metadata\":" << stream.str() << "}";
    } else {
      payload << "\"pso_raw_version\":1"
               << ",\"pso_kind\":\"" << (has_ms ? "mesh" : "graphics") << "\""
               << ",\"source\":\"stream\""
               << ",\"stream_size\":" << static_cast<std::uint64_t>(stream_size)
               << ",\"root_signature_object_id\":" << object_id(graphics.pRootSignature)
               << ",\"node_mask\":" << graphics.NodeMask
               << ",\"flags\":" << static_cast<unsigned int>(graphics.Flags)
               << ",\"input_layout\":" << input_layout_json(graphics.InputLayout)
               << ",\"blend_state\":" << blend_desc_json(graphics.BlendState)
               << ",\"sample_mask\":" << graphics.SampleMask
               << ",\"rasterizer_state\":" << rasterizer_desc_json(graphics.RasterizerState)
               << ",\"depth_stencil_state\":" << depth_stencil_desc_json(graphics.DepthStencilState)
               << ",\"stream_output\":" << stream_output_json(graphics.StreamOutput)
               << ",\"primitive_topology_type\":" << static_cast<unsigned int>(graphics.PrimitiveTopologyType)
               << ",\"num_render_targets\":" << graphics.NumRenderTargets
               << ",\"rtv_formats\":[";
      for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
        if (index) {
          payload << ",";
        }
        payload << static_cast<unsigned int>(graphics.RTVFormats[index]);
      }
      payload << "]"
               << ",\"dsv_format\":" << static_cast<unsigned int>(graphics.DSVFormat)
               << ",\"sample_desc\":{\"count\":" << graphics.SampleDesc.Count
               << ",\"quality\":" << graphics.SampleDesc.Quality << "}"
               << ",\"ib_strip_cut_value\":" << static_cast<unsigned int>(graphics.IBStripCutValue)
               << "," << shader_metadata_json.vs
               << "," << shader_metadata_json.ps
               << "," << shader_metadata_json.ds
               << "," << shader_metadata_json.hs
               << "," << shader_metadata_json.gs;
      if (has_as) {
        payload << "," << shader_metadata_json.as;
      }
      if (has_ms) {
        payload << "," << shader_metadata_json.ms;
      }
      payload << ",\"requires_dxmt_backend\":" << (has_ms || has_as ? "true" : "false")
              << ",\"stream_metadata\":" << stream.str() << "}";
    }
    blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
  }
  payload << "}";
  return record_call(
      "ID3D12Device2::CreatePipelineState",
      payload.str().c_str(),
      refs.data(),
      refs.size(),
      reinterpret_cast<const std::uint64_t *>(blob_refs.data()),
      blob_refs.size(),
      result_code);
}

std::uint64_t record_create_descriptor_heap(
    ID3D12Device *device,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc,
    const void *descriptor_heap,
    std::uint32_t descriptor_size,
    std::uint64_t cpu_start,
    std::uint64_t gpu_start,
    std::int32_t result_code)
{
  if (descriptor_heap && result_code >= 0) {
    record_object_create(descriptor_heap, CaptureObjectKind::DescriptorHeap, device, "ID3D12DescriptorHeap");
  }
  std::ostringstream payload;
  payload << "{\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0)
          << ",\"num_descriptors\":" << (desc ? desc->NumDescriptors : 0)
          << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
          << ",\"node_mask\":" << (desc ? desc->NodeMask : 0)
          << ",\"descriptor_size\":" << descriptor_size
          << ",\"cpu_start\":" << cpu_start
          << ",\"gpu_start\":" << gpu_start
          << "}";
  const void *refs[] = {device, descriptor_heap};
  return record_call("ID3D12Device::CreateDescriptorHeap", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_query_heap(
    ID3D12Device *device,
    const D3D12_QUERY_HEAP_DESC *desc,
    const void *query_heap,
    std::int32_t result_code)
{
  if (query_heap && result_code >= 0) {
    record_object_create(
        query_heap,
        CaptureObjectKind::QueryHeap,
        device,
        "ID3D12QueryHeap");
  }
  std::ostringstream payload;
  payload << "{\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0)
          << ",\"count\":" << (desc ? desc->Count : 0)
          << ",\"node_mask\":" << (desc ? desc->NodeMask : 0)
          << "}";
  const void *refs[] = {device, query_heap};
  return record_call("ID3D12Device::CreateQueryHeap", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_root_signature(
    ID3D12Device *device,
    std::uint32_t node_mask,
    const void *bytecode,
    std::size_t bytecode_length,
    const void *root_signature,
    std::int32_t result_code,
    const D3D12_ROOT_SIGNATURE_DESC *desc)
{
  if (root_signature && result_code >= 0) {
    record_object_create(root_signature, CaptureObjectKind::RootSignature, device, "ID3D12RootSignature");
  }
  const auto asset = bytecode && bytecode_length
                         ? register_asset_bytes(trace::AssetKind::RootSignature, "d3d12-root-signature", bytecode, bytecode_length)
                         : trace::AssetRecord{};
  std::ostringstream payload;
  payload << "{\"node_mask\":" << node_mask
          << ",\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode_length);
  if (asset.blob_id != 0 && !asset.relative_path.empty()) {
    payload << ",\"root_signature_path\":\"" << asset.relative_path.generic_string() << "\"";
  }
  if (desc) {
    payload << ",\"descriptor_tables\":" << root_signature_descriptor_tables_json(desc);
    payload << ",\"root_parameters\":" << root_signature_parameters_json(desc);
  }
  payload << "}";
  const void *refs[] = {device, root_signature};
  return record_call(
      "ID3D12Device::CreateRootSignature",
      payload.str().c_str(),
      refs,
      2,
      asset.blob_id != 0 && !asset.relative_path.empty() ? &asset.blob_id : nullptr,
      asset.blob_id != 0 && !asset.relative_path.empty() ? 1 : 0,
      result_code);
}

std::uint64_t record_create_committed_resource(
    ID3D12Device *device,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    std::uint32_t heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    std::uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource,
    std::uint64_t gpu_virtual_address,
    std::int32_t result_code)
{
  (void)optimized_clear_value;
  if (resource && result_code >= 0) {
    record_object_create(resource, CaptureObjectKind::Resource, device, "ID3D12Resource");
    std::lock_guard lock(g_object_mutex);
    g_resource_gpu_virtual_addresses[resource] = {
        lookup_object_id_locked(resource),
        gpu_virtual_address,
        desc ? desc->Width : 0,
        g_sequence.load(std::memory_order_relaxed),
    };
  }
  std::ostringstream payload;
  payload << "{\"heap_type\":" << (heap_properties ? static_cast<unsigned int>(heap_properties->Type) : 0)
          << ",\"heap_flags\":" << heap_flags
          << ",\"initial_state\":" << initial_state
          << ",\"gpu_virtual_address\":" << gpu_virtual_address
          << ",\"resource_desc\":" << resource_desc_json(desc)
          << "}";
  const void *refs[] = {device, resource};
  return record_call("ID3D12Device::CreateCommittedResource", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_heap(ID3D12Device *device, const D3D12_HEAP_DESC *desc, const void *heap, std::int32_t result_code, const char *function_name)
{
  if (heap && result_code >= 0) {
    record_object_create(heap, CaptureObjectKind::Heap, device, "ID3D12Heap");
  }
  std::ostringstream payload;
  payload << "{\"size_in_bytes\":" << (desc ? desc->SizeInBytes : 0)
          << ",\"alignment\":" << (desc ? desc->Alignment : 0)
          << ",\"heap_type\":" << (desc ? static_cast<unsigned int>(desc->Properties.Type) : 0)
          << ",\"cpu_page_property\":" << (desc ? static_cast<unsigned int>(desc->Properties.CPUPageProperty) : 0)
          << ",\"memory_pool_preference\":" << (desc ? static_cast<unsigned int>(desc->Properties.MemoryPoolPreference) : 0)
          << ",\"creation_node_mask\":" << (desc ? desc->Properties.CreationNodeMask : 0)
          << ",\"visible_node_mask\":" << (desc ? desc->Properties.VisibleNodeMask : 0)
          << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
          << "}";
  const void *refs[] = {device, heap};
  return record_call(function_name ? function_name : "ID3D12Device::CreateHeap", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

void ensure_placed_resource_heap_object(ID3D12Device *device, ID3D12Heap *heap)
{
  if (!heap) {
    return;
  }
  {
    std::lock_guard lock(g_object_mutex);
    if (object_kind_known_locked(heap, trace::ObjectKind::Heap)) {
      return;
    }
  }
  D3D12_HEAP_DESC desc{};
  if (heap->lpVtbl && heap->lpVtbl->GetDesc) {
    heap->lpVtbl->GetDesc(heap, &desc);
  }
  record_create_heap(device, &desc, heap, 0, "ID3D12Device::OpenExistingHeap");
}

std::uint64_t record_create_placed_resource(
    ID3D12Device *device,
    const void *heap,
    std::uint64_t heap_offset,
    const D3D12_RESOURCE_DESC *desc,
    std::uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource,
    std::uint64_t gpu_virtual_address,
    std::int32_t result_code)
{
  (void)optimized_clear_value;
  if (resource && result_code >= 0) {
    ensure_placed_resource_heap_object(device, static_cast<ID3D12Heap *>(const_cast<void *>(heap)));
    record_object_create(resource, CaptureObjectKind::Resource, heap, "ID3D12Resource");
    std::lock_guard lock(g_object_mutex);
    g_resource_gpu_virtual_addresses[resource] = {
        lookup_object_id_locked(resource),
        gpu_virtual_address,
        desc ? desc->Width : 0,
        g_sequence.load(std::memory_order_relaxed),
    };
  }
  std::ostringstream payload;
  payload << "{\"heap_object_id\":" << object_id(heap)
          << ",\"heap_offset\":" << heap_offset
          << ",\"initial_state\":" << initial_state
          << ",\"gpu_virtual_address\":" << gpu_virtual_address
          << ",\"resource_desc\":" << resource_desc_json(desc)
          << "}";
  const void *refs[] = {device, heap, resource};
  return record_call("ID3D12Device::CreatePlacedResource", payload.str().c_str(), refs, 3, nullptr, 0, result_code);
}

std::uint64_t record_create_reserved_resource(
    ID3D12Device *device,
    const D3D12_RESOURCE_DESC *desc,
    std::uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource,
    std::uint64_t gpu_virtual_address,
    std::int32_t result_code)
{
  (void)optimized_clear_value;
  if (resource && result_code >= 0) {
    record_object_create(resource, CaptureObjectKind::Resource, device, "ID3D12Resource");
    std::lock_guard lock(g_object_mutex);
    g_resource_gpu_virtual_addresses[resource] = {
        lookup_object_id_locked(resource),
        gpu_virtual_address,
        desc ? desc->Width : 0,
        g_sequence.load(std::memory_order_relaxed),
    };
  }
  std::ostringstream payload;
  payload << "{\"initial_state\":" << initial_state
          << ",\"gpu_virtual_address\":" << gpu_virtual_address
          << ",\"reserved_resource\":true"
          << ",\"resource_desc\":" << resource_desc_json(desc)
          << "}";
  const void *refs[] = {device, resource};
  return record_call("ID3D12Device::CreateReservedResource", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_fence(
    ID3D12Device *device,
    std::uint64_t initial_value,
    std::uint32_t flags,
    const void *fence,
    std::int32_t result_code)
{
  if (fence && result_code >= 0) {
    record_object_create(fence, CaptureObjectKind::Fence, device, "ID3D12Fence");
  }
  std::ostringstream payload;
  payload << "{\"initial_value\":" << initial_value
          << ",\"flags\":" << flags
          << "}";
  const void *refs[] = {device, fence};
  return record_call("ID3D12Device::CreateFence", payload.str().c_str(), refs, 2, nullptr, 0, result_code);
}

std::uint64_t record_create_command_signature(
    ID3D12Device *device,
    const D3D12_COMMAND_SIGNATURE_DESC *desc,
    const void *root_signature,
    const void *command_signature,
    std::int32_t result_code)
{
  if (result_code >= 0 && command_signature) {
    record_object_create(
        command_signature,
        CaptureObjectKind::CommandSignature,
        device,
        "ID3D12CommandSignature");
  }

  std::ostringstream payload;
  payload << "{\"byte_stride\":" << (desc ? desc->ByteStride : 0)
          << ",\"argument_count\":" << (desc ? desc->NumArgumentDescs : 0)
          << ",\"node_mask\":" << (desc ? desc->NodeMask : 0);
  if (desc && desc->NumArgumentDescs > 0 && desc->pArgumentDescs) {
    payload << ",\"arguments\":[";
    for (std::uint32_t index = 0; index < desc->NumArgumentDescs; ++index) {
      if (index) {
        payload << ",";
      }
      const auto &argument = desc->pArgumentDescs[index];
      payload << "{\"type\":" << static_cast<unsigned int>(argument.Type);
      switch (argument.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
        payload << ",\"slot\":" << argument.VertexBuffer.Slot;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
        payload << ",\"root_parameter_index\":" << argument.Constant.RootParameterIndex
                << ",\"dest_offset_in32bit_values\":" << argument.Constant.DestOffsetIn32BitValues
                << ",\"num32bit_values_to_set\":" << argument.Constant.Num32BitValuesToSet;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        payload << ",\"root_parameter_index\":" << argument.ConstantBufferView.RootParameterIndex;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        payload << ",\"root_parameter_index\":" << argument.ShaderResourceView.RootParameterIndex;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        payload << ",\"root_parameter_index\":" << argument.UnorderedAccessView.RootParameterIndex;
        break;
      default:
        break;
      }
      payload << "}";
    }
    payload << "]";
  }
  payload << "}";
  const void *refs[] = {device, root_signature, command_signature};
  return record_call(
      "ID3D12Device::CreateCommandSignature",
      payload.str().c_str(),
      refs,
      3,
      nullptr,
      0,
      result_code);
}

std::uint64_t record_create_constant_buffer_view(
    ID3D12Device *device,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
    const void *resolved_resource,
    std::uint64_t resolved_resource_offset,
    std::uint64_t resolved_resource_width)
{
  const bool has_buffer_location = desc && desc->BufferLocation != 0;
  const bool has_resolved_resource = resolved_resource != nullptr;
  remember_cbv_descriptor(descriptor, desc);
  if (has_resolved_resource && !is_known_resource_object(resolved_resource)) {
    ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(resolved_resource)), "CreateConstantBufferViewGpuVA");
  }
  DescriptorViewOp op;
  op.descriptor = descriptor.ptr;
  if (has_buffer_location) {
    op.buffer_location = desc->BufferLocation;
    op.size_in_bytes = desc->SizeInBytes;
    op.gpuva_resolve_status = has_resolved_resource ? "mapped" : "unmapped";
    if (has_resolved_resource) {
      op.resolved_resource_object_id = object_id(resolved_resource);
      op.resolved_resource_offset = resolved_resource_offset;
      op.resolved_resource_width = resolved_resource_width;
    }
  } else if (desc && desc->SizeInBytes != 0) {
    op.size_in_bytes = desc->SizeInBytes;
  }
  if (has_buffer_location && has_resolved_resource && desc->SizeInBytes != 0) {
    capture_mapped_resource_range_before_use(
        resolved_resource,
        resolved_resource_offset,
        desc->SizeInBytes);
  }
  return record_descriptor_view_batched(
      "ID3D12Device::CreateConstantBufferView",
      object_id(device),
      object_id(resolved_resource),
      0,
      std::move(op));
}

std::uint64_t record_create_shader_resource_view(
    ID3D12Device *device,
    const void *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  forget_cbv_descriptor(descriptor);
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(resource)), "CreateShaderResourceView");
  DescriptorViewOp op;
  op.descriptor = descriptor.ptr;
  op.format = desc ? static_cast<unsigned int>(desc->Format) : 0;
  op.view_dimension = desc ? static_cast<unsigned int>(desc->ViewDimension) : 0;
  op.shader_4_component_mapping = desc ? desc->Shader4ComponentMapping : 0;
  op.view_payload = srv_desc_detail_sparse_json(desc);
  return record_descriptor_view_batched(
      "ID3D12Device::CreateShaderResourceView",
      object_id(device),
      object_id(resource),
      0,
      std::move(op));
}

std::uint64_t record_create_unordered_access_view(
    ID3D12Device *device,
    const void *resource,
    const void *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  forget_cbv_descriptor(descriptor);
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(resource)), "CreateUnorderedAccessView");
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(counter_resource)), "CreateUnorderedAccessViewCounter");
  DescriptorViewOp op;
  op.descriptor = descriptor.ptr;
  op.format = desc ? static_cast<unsigned int>(desc->Format) : 0;
  op.view_dimension = desc ? static_cast<unsigned int>(desc->ViewDimension) : 0;
  op.view_payload = uav_desc_detail_sparse_json(desc);
  return record_descriptor_view_batched(
      "ID3D12Device::CreateUnorderedAccessView",
      object_id(device),
      object_id(resource),
      object_id(counter_resource),
      std::move(op));
}

std::uint64_t record_create_render_target_view(
    ID3D12Device *device,
    const void *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(resource)), "CreateRenderTargetView");
  DescriptorViewOp op;
  op.descriptor = descriptor.ptr;
  op.format = desc ? static_cast<unsigned int>(desc->Format) : 0;
  op.view_dimension = desc ? static_cast<unsigned int>(desc->ViewDimension) : 0;
  op.view_payload = rtv_desc_detail_sparse_json(desc);
  return record_descriptor_view_batched(
      "ID3D12Device::CreateRenderTargetView",
      object_id(device),
      object_id(resource),
      0,
      std::move(op));
}

std::uint64_t record_create_depth_stencil_view(
    ID3D12Device *device,
    const void *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(resource)), "CreateDepthStencilView");
  DescriptorViewOp op;
  op.descriptor = descriptor.ptr;
  op.format = desc ? static_cast<unsigned int>(desc->Format) : 0;
  op.view_dimension = desc ? static_cast<unsigned int>(desc->ViewDimension) : 0;
  op.flags = desc ? static_cast<unsigned int>(desc->Flags) : 0;
  op.view_payload = dsv_desc_detail_sparse_json(desc);
  return record_descriptor_view_batched(
      "ID3D12Device::CreateDepthStencilView",
      object_id(device),
      object_id(resource),
      0,
      std::move(op));
}

std::uint64_t record_create_sampler(
    ID3D12Device *device,
    const D3D12_SAMPLER_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  std::ostringstream payload;
  payload << "{\"descriptor\":" << cpu_descriptor_json(descriptor);
  payload << ",\"descriptor_detail\":" << cpu_descriptor_detail_json(descriptor);
  if (desc) {
    payload << ",\"filter\":" << static_cast<unsigned int>(desc->Filter)
            << ",\"address_u\":" << static_cast<unsigned int>(desc->AddressU)
            << ",\"address_v\":" << static_cast<unsigned int>(desc->AddressV)
            << ",\"address_w\":" << static_cast<unsigned int>(desc->AddressW)
            << ",\"mip_lod_bias\":" << desc->MipLODBias
            << ",\"max_anisotropy\":" << desc->MaxAnisotropy
            << ",\"comparison_func\":" << static_cast<unsigned int>(desc->ComparisonFunc)
            << ",\"border_color\":["
            << desc->BorderColor[0] << ","
            << desc->BorderColor[1] << ","
            << desc->BorderColor[2] << ","
            << desc->BorderColor[3] << "]"
            << ",\"min_lod\":" << desc->MinLOD
            << ",\"max_lod\":" << desc->MaxLOD;
  }
  payload << "}";
  const void *refs[] = {device};
  return record_call("ID3D12Device::CreateSampler", payload.str().c_str(), refs, 1);
}

std::uint64_t record_copy_descriptors(
    ID3D12Device *device,
    std::uint32_t dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const std::uint32_t *dst_descriptor_range_sizes,
    std::uint32_t src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const std::uint32_t *src_descriptor_range_sizes,
    std::uint32_t descriptor_heap_type,
    std::uint32_t descriptor_size)
{
  auto ranges = collect_copy_descriptor_ranges(
      dst_descriptor_range_count,
      dst_descriptor_range_starts,
      dst_descriptor_range_sizes,
      src_descriptor_range_count,
      src_descriptor_range_starts,
      src_descriptor_range_sizes,
      descriptor_size);
  capture_cbv_descriptor_copy_ranges_before_use(
      descriptor_heap_type,
      descriptor_size,
      ranges);

  std::lock_guard event_lock(g_event_order_mutex);
  CopyDescriptorOp op;
  op.device_object_id = object_id(device);
  op.descriptor_heap_type = descriptor_heap_type;
  op.descriptor_size = descriptor_size;
  op.dst_range_count = dst_descriptor_range_count;
  op.src_range_count = src_descriptor_range_count;
  op.ranges = std::move(ranges);

  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  op.sequence = sequence;
  bool should_flush = false;
  {
    std::lock_guard lock(g_diagnostic_batch_mutex);
    g_copy_descriptor_batch.ops.push_back(std::move(op));
    should_flush = g_copy_descriptor_batch.ops.size() >= kCopyDescriptorBatchMaxOps;
  }
  if (should_flush) {
    flush_diagnostic_batches();
  }
  return sequence;
}

std::uint64_t record_copy_descriptors_simple(
    ID3D12Device *device,
    std::uint32_t descriptor_count,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_start,
    D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_start,
    std::uint32_t descriptor_heap_type,
    std::uint32_t descriptor_size)
{
  return record_copy_descriptors(
      device,
      1,
      &dst_descriptor_range_start,
      &descriptor_count,
      1,
      &src_descriptor_range_start,
      &descriptor_count,
      descriptor_heap_type,
      descriptor_size);
}

std::uint64_t record_draw_instanced(
    const void *command_list,
    std::uint32_t vertex_count_per_instance,
    std::uint32_t instance_count,
    std::uint32_t start_vertex_location,
    std::uint32_t start_instance_location)
{
  capture_graphics_mapped_inputs_before_use(command_list, false);
  std::ostringstream payload;
  payload << "{\"vertex_count_per_instance\":" << vertex_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_vertex_location\":" << start_vertex_location
          << ",\"start_instance_location\":" << start_instance_location
          << "}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::DrawInstanced", payload.str().c_str(), refs, 1);
}

std::uint64_t record_draw_indexed_instanced(
    const void *command_list,
    std::uint32_t index_count_per_instance,
    std::uint32_t instance_count,
    std::uint32_t start_index_location,
    std::int32_t base_vertex_location,
    std::uint32_t start_instance_location)
{
  capture_graphics_mapped_inputs_before_use(command_list, true);
  std::ostringstream payload;
  payload << "{\"index_count_per_instance\":" << index_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_index_location\":" << start_index_location
          << ",\"base_vertex_location\":" << base_vertex_location
          << ",\"start_instance_location\":" << start_instance_location
          << "}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::DrawIndexedInstanced", payload.str().c_str(), refs, 1);
}

std::uint64_t record_dispatch(const void *command_list, std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
  capture_compute_mapped_inputs_before_use(command_list);
  std::ostringstream payload;
  payload << "{\"thread_group_count_x\":" << x
          << ",\"thread_group_count_y\":" << y
          << ",\"thread_group_count_z\":" << z
          << "}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::Dispatch", payload.str().c_str(), refs, 1);
}

std::uint64_t record_close_command_list(const void *command_list, std::int32_t result_code)
{
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::Close", "{}", refs, 1, nullptr, 0, result_code);
}

std::uint64_t record_reset_command_list(
    const void *command_list,
    const void *command_allocator,
    const void *initial_pipeline_state,
    std::int32_t result_code)
{
  if (result_code >= 0) {
    clear_command_list_mapped_uses(command_list);
  }
  std::ostringstream payload;
  payload << "{\"command_allocator_object_id\":" << object_id(command_allocator)
          << ",\"initial_pipeline_state_object_id\":" << object_id(initial_pipeline_state)
          << "}";
  const void *refs[] = {command_list, command_allocator, initial_pipeline_state};
  return record_call(
      "ID3D12GraphicsCommandList::Reset",
      payload.str().c_str(),
      refs,
      3,
      nullptr,
      0,
      result_code);
}

std::uint64_t record_execute_indirect(
    const void *command_list,
    const void *command_signature,
    std::uint32_t max_command_count,
    const void *arg_buffer,
    std::uint64_t arg_buffer_offset,
    const void *count_buffer,
    std::uint64_t count_buffer_offset)
{
  std::ostringstream payload;
  payload << "{\"max_command_count\":" << max_command_count
          << ",\"arg_buffer_offset\":" << arg_buffer_offset
          << ",\"count_buffer_offset\":" << count_buffer_offset << "}";
  const void *refs[] = {command_list, command_signature, arg_buffer, count_buffer};
  return record_call(
      "ID3D12GraphicsCommandList::ExecuteIndirect",
      payload.str().c_str(),
      refs,
      4);
}

std::uint64_t record_execute_bundle(const void *command_list, const void *bundle_command_list)
{
  std::ostringstream payload;
  payload << "{\"bundle_command_list_object_id\":" << object_id(bundle_command_list) << "}";
  const void *refs[] = {command_list, bundle_command_list};
  return record_call("ID3D12GraphicsCommandList::ExecuteBundle", payload.str().c_str(), refs, 2);
}

std::uint64_t record_resource_barrier(
    const void *command_list,
    std::uint32_t barrier_count,
    const D3D12_RESOURCE_BARRIER *barriers)
{
  if (barriers && barrier_count != 0) {
    for (std::uint32_t index = 0; index < barrier_count; ++index) {
      const auto &barrier = barriers[index];
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        ensure_external_resource_object_with_desc(barrier.Transition.pResource, "ResourceBarrier");
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        ensure_external_resource_object_with_desc(barrier.Aliasing.pResourceBefore, "ResourceBarrierAliasingBefore");
        ensure_external_resource_object_with_desc(barrier.Aliasing.pResourceAfter, "ResourceBarrierAliasingAfter");
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        ensure_external_resource_object_with_desc(barrier.UAV.pResource, "ResourceBarrierUAV");
      }
    }
  }
  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  const auto first_sequence = g_sequence.fetch_add(barrier_count ? barrier_count : 1, std::memory_order_relaxed) + 1;
  if (!command_list || !barriers || barrier_count == 0) {
    return first_sequence;
  }

  flush_copy_buffer_batches(command_list);
  flush_copy_texture_region_batches(command_list);

  PendingResourceBarrierBatch flush_batch;
  {
    std::lock_guard lock(g_command_batch_mutex);
    auto &batch = g_resource_barrier_batches[command_list];
    batch.ops.reserve(batch.ops.size() + barrier_count);
    for (std::uint32_t index = 0; index < barrier_count; ++index) {
      ResourceBarrierBatchOp op;
      op.sequence = first_sequence + index;
      const auto &barrier = barriers[index];
      op.type = static_cast<std::uint32_t>(barrier.Type);
      op.flags = static_cast<std::uint32_t>(barrier.Flags);
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        op.resource_object_id = object_id(barrier.Transition.pResource);
        op.before = static_cast<std::uint32_t>(barrier.Transition.StateBefore);
        op.after = static_cast<std::uint32_t>(barrier.Transition.StateAfter);
        op.subresource = barrier.Transition.Subresource;
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        op.resource_before_object_id = object_id(barrier.Aliasing.pResourceBefore);
        op.resource_after_object_id = object_id(barrier.Aliasing.pResourceAfter);
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        op.resource_object_id = object_id(barrier.UAV.pResource);
      }
      batch.ops.push_back(op);
    }
    if (batch.ops.size() >= kCommandListBatchMaxOps) {
      flush_batch = std::move(batch);
      g_resource_barrier_batches.erase(command_list);
    }
  }
  if (!flush_batch.ops.empty()) {
    record_resource_barrier_batch_event(command_list, flush_batch);
  }
  return first_sequence;
}

std::uint64_t record_copy_buffer_region(
    const char *function_name,
    const void *command_list,
    const void *dst_buffer,
    std::uint64_t dst_offset,
    const void *src_buffer,
    std::uint64_t src_offset,
    std::uint64_t byte_count)
{
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(dst_buffer)), "CopyBufferRegionDst");
  ensure_external_resource_object_with_desc(static_cast<ID3D12Resource *>(const_cast<void *>(src_buffer)), "CopyBufferRegionSrc");
  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  flush_resource_barrier_batches(command_list);
  flush_copy_texture_region_batches(command_list);

  std::lock_guard lock(g_command_batch_mutex);
  auto &batch = g_copy_buffer_batches[command_list];
  batch.ops.push_back(CopyBufferBatchOp{
      sequence,
      function_name && *function_name ? function_name : "ID3D12GraphicsCommandList::CopyBufferRegion",
      dst_buffer,
      dst_offset,
      src_buffer,
      src_offset,
      byte_count,
  });
  return sequence;
}

std::uint64_t record_copy_texture_region(
    const void *command_list,
    const D3D12_TEXTURE_COPY_LOCATION *dst,
    std::uint32_t dst_x,
    std::uint32_t dst_y,
    std::uint32_t dst_z,
    const D3D12_TEXTURE_COPY_LOCATION *src,
    const D3D12_BOX *src_box)
{
  ensure_external_resource_object_with_desc(dst ? dst->pResource : nullptr, "CopyTextureRegionDst");
  ensure_external_resource_object_with_desc(src ? src->pResource : nullptr, "CopyTextureRegionSrc");
  std::lock_guard event_lock(g_event_order_mutex);
  flush_diagnostic_batches();
  const auto sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!command_list) {
    return sequence;
  }

  flush_copy_buffer_batches(command_list);
  flush_resource_barrier_batches(command_list);

  CopyTextureRegionBatchOp op;
  op.sequence = sequence;
  op.dst = compact_texture_copy_location(dst);
  op.dst_x = dst_x;
  op.dst_y = dst_y;
  op.dst_z = dst_z;
  op.src = compact_texture_copy_location(src);
  if (src_box) {
    op.has_src_box = true;
    op.src_box_left = src_box->left;
    op.src_box_top = src_box->top;
    op.src_box_front = src_box->front;
    op.src_box_right = src_box->right;
    op.src_box_bottom = src_box->bottom;
    op.src_box_back = src_box->back;
  }

  PendingCopyTextureRegionBatch flush_batch;
  {
    std::lock_guard lock(g_command_batch_mutex);
    auto &batch = g_copy_texture_region_batches[command_list];
    batch.ops.push_back(op);
    if (batch.ops.size() >= kCommandListBatchMaxOps) {
      flush_batch = std::move(batch);
      g_copy_texture_region_batches.erase(command_list);
    }
  }
  if (!flush_batch.ops.empty()) {
    record_copy_texture_region_batch_event(command_list, flush_batch);
  }
  return sequence;
}

std::uint64_t record_copy_resource(const void *command_list, const void *dst_resource, const void *src_resource)
{
  std::ostringstream payload;
  payload << "{\"dst_resource_object_id\":" << object_id(dst_resource)
          << ",\"src_resource_object_id\":" << object_id(src_resource)
          << "}";
  const void *refs[] = {command_list, dst_resource, src_resource};
  return record_call("ID3D12GraphicsCommandList::CopyResource", payload.str().c_str(), refs, 3);
}

std::uint64_t record_resolve_subresource(
    const void *command_list,
    const void *dst_resource,
    std::uint32_t dst_subresource,
    const void *src_resource,
    std::uint32_t src_subresource,
    std::uint32_t format)
{
  std::ostringstream payload;
  payload << "{\"dst_resource_object_id\":" << object_id(dst_resource)
          << ",\"dst_subresource\":" << dst_subresource
          << ",\"src_resource_object_id\":" << object_id(src_resource)
          << ",\"src_subresource\":" << src_subresource
          << ",\"format\":" << format
          << "}";
  const void *refs[] = {command_list, dst_resource, src_resource};
  return record_call("ID3D12GraphicsCommandList::ResolveSubresource", payload.str().c_str(), refs, 3);
}

std::uint64_t record_ia_set_primitive_topology(const void *command_list, std::uint32_t primitive_topology)
{
  std::ostringstream payload;
  payload << "{\"primitive_topology\":" << primitive_topology << "}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::IASetPrimitiveTopology", payload.str().c_str(), refs, 1);
}

std::uint64_t record_rs_set_viewports(const void *command_list, std::uint32_t viewport_count, const D3D12_VIEWPORT *viewports)
{
  std::ostringstream payload;
  payload << "{\"viewport_count\":" << viewport_count << ",\"viewports\":[";
  for (std::uint32_t index = 0; viewports && index < viewport_count; ++index) {
    if (index) payload << ",";
    payload << "{\"x\":" << viewports[index].TopLeftX
            << ",\"y\":" << viewports[index].TopLeftY
            << ",\"top_left_x\":" << viewports[index].TopLeftX
            << ",\"top_left_y\":" << viewports[index].TopLeftY
            << ",\"width\":" << viewports[index].Width
            << ",\"height\":" << viewports[index].Height
            << ",\"min_depth\":" << viewports[index].MinDepth
            << ",\"max_depth\":" << viewports[index].MaxDepth
            << "}";
  }
  payload << "]}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::RSSetViewports", payload.str().c_str(), refs, 1);
}

std::uint64_t record_rs_set_scissor_rects(const void *command_list, std::uint32_t rect_count, const void *rects)
{
  const auto *typed_rects = static_cast<const D3D12_RECT *>(rects);
  std::ostringstream payload;
  payload << "{\"rect_count\":" << rect_count << ",\"rects\":[";
  for (std::uint32_t index = 0; typed_rects && index < rect_count; ++index) {
    if (index) payload << ",";
    append_rect_json(payload, typed_rects[index]);
  }
  payload << "]}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::RSSetScissorRects", payload.str().c_str(), refs, 1);
}

std::uint64_t record_set_pipeline_state(const void *command_list, const void *pipeline_state)
{
  std::ostringstream payload;
  payload << "{\"pipeline_state_object_id\":" << object_id(pipeline_state) << "}";
  const void *refs[] = {command_list, pipeline_state};
  return record_call("ID3D12GraphicsCommandList::SetPipelineState", payload.str().c_str(), refs, 2);
}

std::uint64_t record_clear_state(const void *command_list, const void *pipeline_state)
{
  clear_command_list_mapped_uses(command_list);
  std::ostringstream payload;
  payload << "{\"pipeline_state_object_id\":" << object_id(pipeline_state) << "}";
  const void *refs[] = {command_list, pipeline_state};
  return record_call("ID3D12GraphicsCommandList::ClearState", payload.str().c_str(), refs, 2);
}

std::uint64_t record_om_set_blend_factor(const void *command_list, const float blend_factor[4])
{
  std::ostringstream payload;
  payload << "{\"blend_factor\":[";
  for (std::uint32_t index = 0; index < 4; ++index) {
    if (index) {
      payload << ",";
    }
    payload << (blend_factor ? blend_factor[index] : 0.0f);
  }
  payload << "]}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::OMSetBlendFactor", payload.str().c_str(), refs, 1);
}

std::uint64_t record_om_set_stencil_ref(const void *command_list, std::uint32_t stencil_ref)
{
  std::ostringstream payload;
  payload << "{\"stencil_ref\":" << stencil_ref << "}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::OMSetStencilRef", payload.str().c_str(), refs, 1);
}

std::uint64_t record_set_descriptor_heaps(const void *command_list, std::uint32_t heap_count, const void *const *heaps)
{
  std::vector<const void *> refs = {command_list};
  std::ostringstream payload;
  payload << "{\"heap_count\":" << heap_count << ",\"heaps\":[";
  for (std::uint32_t index = 0; heaps && index < heap_count; ++index) {
    if (index) payload << ",";
    payload << object_id(heaps[index]);
    refs.push_back(heaps[index]);
  }
  payload << "]}";
  return record_call("ID3D12GraphicsCommandList::SetDescriptorHeaps", payload.str().c_str(), refs.data(), refs.size());
}

std::uint64_t record_set_root_signature(const void *command_list, bool compute, const void *root_signature)
{
  clear_root_cbv_ranges(command_list, compute);
  std::ostringstream payload;
  payload << "{\"compute\":" << (compute ? "true" : "false")
          << ",\"root_signature_object_id\":" << object_id(root_signature)
          << "}";
  const void *refs[] = {command_list, root_signature};
  return record_call(
      compute ? "ID3D12GraphicsCommandList::SetComputeRootSignature" : "ID3D12GraphicsCommandList::SetGraphicsRootSignature",
      payload.str().c_str(), refs, 2);
}

std::uint64_t record_set_root_descriptor_table(
    const void *command_list,
    bool compute,
    std::uint32_t root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
  std::ostringstream payload;
  payload << "{\"compute\":" << (compute ? "true" : "false")
          << ",\"root_parameter_index\":" << root_parameter_index
          << ",\"base_descriptor\":" << gpu_descriptor_json(base_descriptor)
          << ",\"base_descriptor_detail\":" << gpu_descriptor_detail_json(base_descriptor)
          << "}";
  const void *refs[] = {command_list};
  return record_call(
      compute ? "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable" : "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      payload.str().c_str(), refs, 1);
}

std::uint64_t record_set_root_32bit_constants(
    const void *command_list,
    bool compute,
    std::uint32_t root_parameter_index,
    std::uint32_t constant_count,
    const std::uint32_t *values,
    std::uint32_t dst_offset)
{
  std::ostringstream payload;
  payload << "{\"compute\":" << (compute ? "true" : "false")
          << ",\"root_parameter_index\":" << root_parameter_index
          << ",\"dst_offset\":" << dst_offset
          << ",\"values\":[";
  for (std::uint32_t index = 0; values && index < constant_count; ++index) {
    if (index) payload << ",";
    payload << values[index];
  }
  payload << "]}";
  const void *refs[] = {command_list};
  return record_call(
      compute ? "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants" : "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants",
      payload.str().c_str(), refs, 1);
}

std::uint64_t record_set_root_descriptor(
    const void *command_list,
    bool compute,
    std::uint32_t parameter_type,
    std::uint32_t root_parameter_index,
    std::uint64_t gpu_virtual_address)
{
  if (parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
    remember_root_cbv_range(command_list, compute, root_parameter_index, gpu_virtual_address);
    capture_mapped_gpuva_range_before_use_chunked(gpu_virtual_address, kRootConstantBufferSnapshotBytes);
  }

  const char *name = "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView";
  if (compute && parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) name = "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView";
  else if (compute && parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV) name = "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView";
  else if (compute && parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) name = "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView";
  else if (!compute && parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV) name = "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView";
  else if (!compute && parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) name = "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView";
  std::vector<trace::ObjectId> refs;
  append_object_ref_id(refs, object_id(command_list));
  std::ostringstream payload;
  payload << "{\"compute\":" << (compute ? "true" : "false")
          << ",\"parameter_type\":" << parameter_type
          << ",\"root_parameter_index\":" << root_parameter_index
          << ",";
  append_gpu_virtual_address_binding_json_with_ref(payload, "buffer_location", gpu_virtual_address, refs);
  payload << "}";
  return record_call_with_object_ids(
      name,
      payload.str().c_str(),
      std::move(refs));
}

std::uint64_t record_ia_set_index_buffer(const void *command_list, const D3D12_INDEX_BUFFER_VIEW *view)
{
  remember_index_buffer_range(command_list, view);
  if (view) {
    capture_mapped_gpuva_range_before_use(view->BufferLocation, view->SizeInBytes);
  }

  std::vector<trace::ObjectId> refs;
  append_object_ref_id(refs, object_id(command_list));
  std::ostringstream payload;
  payload << "{";
  if (view) {
    append_gpu_virtual_address_binding_json_with_ref(payload, "buffer_location", view->BufferLocation, refs);
    payload << ",\"size_in_bytes\":" << view->SizeInBytes
            << ",\"format\":" << static_cast<unsigned int>(view->Format);
  }
  payload << "}";
  return record_call_with_object_ids(
      "ID3D12GraphicsCommandList::IASetIndexBuffer",
      payload.str().c_str(),
      std::move(refs));
}

std::uint64_t record_ia_set_vertex_buffers(
    const void *command_list,
    std::uint32_t start_slot,
    std::uint32_t view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views)
{
  remember_vertex_buffer_ranges(command_list, start_slot, view_count, views);
  for (std::uint32_t index = 0; views && index < view_count; ++index) {
    capture_mapped_gpuva_range_before_use(views[index].BufferLocation, views[index].SizeInBytes);
  }

  std::vector<trace::ObjectId> refs;
  append_object_ref_id(refs, object_id(command_list));
  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot
          << ",\"view_count\":" << view_count
          << ",\"views\":[";
  for (std::uint32_t index = 0; views && index < view_count; ++index) {
    if (index) payload << ",";
    payload << "{";
    append_gpu_virtual_address_binding_json_with_ref(payload, "buffer_location", views[index].BufferLocation, refs);
    payload << ",\"size_in_bytes\":" << views[index].SizeInBytes
            << ",\"stride_in_bytes\":" << views[index].StrideInBytes
            << "}";
  }
  payload << "]}";
  return record_call_with_object_ids(
      "ID3D12GraphicsCommandList::IASetVertexBuffers",
      payload.str().c_str(),
      std::move(refs));
}

std::uint64_t record_om_set_render_targets(
    const void *command_list,
    std::uint32_t render_target_descriptor_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    bool single_descriptor_handle,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor)
{
  std::vector<const void *> refs = {command_list};
  std::ostringstream payload;
  payload << "{\"render_target_count\":" << render_target_descriptor_count
          << ",\"single_handle_to_descriptor_range\":" << (single_descriptor_handle ? "true" : "false")
          << ",\"single_descriptor_handle\":" << (single_descriptor_handle ? "true" : "false")
          << ",\"render_targets\":[";
  const auto descriptor_slots = single_descriptor_handle && render_target_descriptor_count > 0 ? 1 : render_target_descriptor_count;
  for (std::uint32_t index = 0; render_target_descriptors && index < descriptor_slots; ++index) {
    if (index) {
      payload << ",";
    }
    payload << cpu_descriptor_json(render_target_descriptors[index]);
  }
  payload << "],\"render_target_descriptors\":[";
  for (std::uint32_t index = 0; render_target_descriptors && index < descriptor_slots; ++index) {
    if (index) {
      payload << ",";
    }
    payload << cpu_descriptor_detail_json(render_target_descriptors[index]);
  }
  payload << "]";
  if (render_target_descriptor_count > 0 && render_target_descriptors) {
    payload << ",\"first_rtv\":" << cpu_descriptor_json(render_target_descriptors[0]);
  } else {
    payload << ",\"first_rtv\":0";
  }
  payload << ",\"dsv\":";
  if (depth_stencil_descriptor) {
    payload << cpu_descriptor_json(*depth_stencil_descriptor);
  } else {
    payload << "0";
  }
  payload << ",\"depth_stencil_descriptor\":";
  if (depth_stencil_descriptor) {
    payload << cpu_descriptor_detail_json(*depth_stencil_descriptor);
  } else {
    payload << "null";
  }
  payload << "}";
  return record_call("ID3D12GraphicsCommandList::OMSetRenderTargets", payload.str().c_str(), refs.data(), refs.size());
}

std::uint64_t record_clear_depth_stencil_view(
    const void *command_list,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    std::uint32_t flags,
    float depth,
    std::uint8_t stencil,
    std::uint32_t rect_count,
    const void *rects)
{
  const auto *typed_rects = static_cast<const D3D12_RECT *>(rects);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << static_cast<std::uint64_t>(dsv.ptr)
          << ",\"dsv\":" << cpu_descriptor_json(dsv)
          << ",\"clear_flags\":" << flags
          << ",\"depth\":" << depth
          << ",\"stencil\":" << static_cast<std::uint32_t>(stencil)
          << ",\"rect_count\":" << rect_count
          << ",\"rects\":[";
  for (std::uint32_t index = 0; typed_rects && index < rect_count; ++index) {
    if (index) {
      payload << ",";
    }
    append_rect_json(payload, typed_rects[index]);
  }
  payload << "]}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::ClearDepthStencilView", payload.str().c_str(), refs, 1);
}

std::uint64_t record_clear_render_target_view(
    const void *command_list,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    const float color[4],
    std::uint32_t rect_count,
    const void *rects)
{
  const auto *typed_rects = static_cast<const D3D12_RECT *>(rects);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << static_cast<std::uint64_t>(rtv.ptr)
          << ",\"rtv\":" << cpu_descriptor_json(rtv)
          << ",\"color\":[";
  for (std::uint32_t index = 0; index < 4; ++index) {
    if (index) {
      payload << ",";
    }
    payload << (color ? color[index] : 0.0f);
  }
  payload << "],\"rect_count\":" << rect_count
          << ",\"rects\":[";
  for (std::uint32_t index = 0; typed_rects && index < rect_count; ++index) {
    if (index) {
      payload << ",";
    }
    append_rect_json(payload, typed_rects[index]);
  }
  payload << "]}";
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList::ClearRenderTargetView", payload.str().c_str(), refs, 1);
}

std::uint64_t record_clear_unordered_access_view_uint(
    const void *command_list,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor,
    const void *resource,
    const std::uint32_t values[4],
    std::uint32_t rect_count,
    const void *rects)
{
  const auto *typed_rects = static_cast<const D3D12_RECT *>(rects);
  std::ostringstream payload;
  payload << "{\"gpu_descriptor\":" << static_cast<std::uint64_t>(gpu_descriptor.ptr)
          << ",\"cpu_descriptor\":" << static_cast<std::uint64_t>(cpu_descriptor.ptr)
          << ",\"values\":[";
  for (std::uint32_t index = 0; index < 4; ++index) {
    if (index) {
      payload << ",";
    }
    payload << (values ? values[index] : 0);
  }
  payload << "],\"rect_count\":" << rect_count
          << ",\"rects\":[";
  for (std::uint32_t index = 0; typed_rects && index < rect_count; ++index) {
    if (index) {
      payload << ",";
    }
    append_rect_json(payload, typed_rects[index]);
  }
  payload << "]}";
  const void *refs[] = {command_list, resource};
  return record_call("ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint", payload.str().c_str(), refs, 2);
}

std::uint64_t record_clear_unordered_access_view_float(
    const void *command_list,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor,
    const void *resource,
    const float values[4],
    std::uint32_t rect_count,
    const void *rects)
{
  const auto *typed_rects = static_cast<const D3D12_RECT *>(rects);
  std::ostringstream payload;
  payload << "{\"gpu_descriptor\":" << static_cast<std::uint64_t>(gpu_descriptor.ptr)
          << ",\"cpu_descriptor\":" << static_cast<std::uint64_t>(cpu_descriptor.ptr)
          << ",\"values\":[";
  for (std::uint32_t index = 0; index < 4; ++index) {
    if (index) {
      payload << ",";
    }
    payload << (values ? values[index] : 0.0f);
  }
  payload << "],\"rect_count\":" << rect_count
          << ",\"rects\":[";
  for (std::uint32_t index = 0; typed_rects && index < rect_count; ++index) {
    if (index) {
      payload << ",";
    }
    append_rect_json(payload, typed_rects[index]);
  }
  payload << "]}";
  const void *refs[] = {command_list, resource};
  return record_call("ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat", payload.str().c_str(), refs, 2);
}

std::uint64_t record_discard_resource(
    const void *command_list,
    const void *resource,
    std::uint32_t first_subresource,
    std::uint32_t subresource_count,
    std::uint32_t rect_count,
    const void *rects)
{
  const auto *typed_rects = static_cast<const D3D12_RECT *>(rects);
  std::ostringstream payload;
  payload << "{\"first_subresource\":" << first_subresource
          << ",\"subresource_count\":" << subresource_count
          << ",\"rect_count\":" << rect_count
          << ",\"rects\":[";
  for (std::uint32_t index = 0; typed_rects && index < rect_count; ++index) {
    if (index) {
      payload << ",";
    }
    append_rect_json(payload, typed_rects[index]);
  }
  payload << "]}";
  const void *refs[] = {command_list, resource};
  return record_call("ID3D12GraphicsCommandList::DiscardResource", payload.str().c_str(), refs, 2);
}

std::uint64_t record_begin_query(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t index)
{
  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id(query_heap)
          << ",\"type\":" << type
          << ",\"index\":" << index
          << "}";
  const void *refs[] = {command_list, query_heap};
  return record_call("ID3D12GraphicsCommandList::BeginQuery", payload.str().c_str(), refs, 2);
}

std::uint64_t record_end_query(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t index)
{
  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id(query_heap)
          << ",\"type\":" << type
          << ",\"index\":" << index
          << "}";
  const void *refs[] = {command_list, query_heap};
  return record_call("ID3D12GraphicsCommandList::EndQuery", payload.str().c_str(), refs, 2);
}

std::uint64_t record_resolve_query_data(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t start_index,
    std::uint32_t query_count,
    const void *dst_buffer,
    std::uint64_t aligned_dst_buffer_offset)
{
  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id(query_heap)
          << ",\"type\":" << type
          << ",\"start_index\":" << start_index
          << ",\"query_count\":" << query_count
          << ",\"dst_buffer_object_id\":" << object_id(dst_buffer)
          << ",\"aligned_dst_buffer_offset\":" << aligned_dst_buffer_offset
          << "}";
  const void *refs[] = {command_list, query_heap, dst_buffer};
  return record_call("ID3D12GraphicsCommandList::ResolveQueryData", payload.str().c_str(), refs, 3);
}

std::uint64_t record_set_predication(
    const void *command_list,
    const void *buffer,
    std::uint64_t aligned_buffer_offset,
    std::uint32_t operation)
{
  std::ostringstream payload;
  payload << "{\"buffer_object_id\":" << object_id(buffer)
          << ",\"aligned_buffer_offset\":" << aligned_buffer_offset
          << ",\"operation\":" << operation
          << "}";
  const void *refs[] = {command_list, buffer};
  return record_call("ID3D12GraphicsCommandList::SetPredication", payload.str().c_str(), refs, 2);
}

std::uint64_t record_resolve_subresource_region(
    const void *command_list,
    const void *dst_resource,
    std::uint32_t dst_subresource,
    std::uint32_t dst_x,
    std::uint32_t dst_y,
    const void *src_resource,
    std::uint32_t src_subresource,
    const void *src_rect,
    std::uint32_t format,
    std::uint32_t mode)
{
  const auto *typed_src_rect = static_cast<const D3D12_RECT *>(src_rect);
  std::ostringstream payload;
  payload << "{\"dst_resource_object_id\":" << object_id(dst_resource)
          << ",\"dst_subresource\":" << dst_subresource
          << ",\"dst_x\":" << dst_x
          << ",\"dst_y\":" << dst_y
          << ",\"src_resource_object_id\":" << object_id(src_resource)
          << ",\"src_subresource\":" << src_subresource
          << ",\"src_rect\":";
  if (typed_src_rect) {
    append_rect_json(payload, *typed_src_rect);
  } else {
    payload << "null";
  }
  payload << ",\"format\":" << format
          << ",\"mode\":" << mode
          << "}";
  const void *refs[] = {command_list, dst_resource, src_resource};
  return record_call("ID3D12GraphicsCommandList::ResolveSubresourceRegion", payload.str().c_str(), refs, 3);
}

std::uint64_t record_write_buffer_immediate(
    const void *command_list,
    std::uint32_t count,
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const void *modes)
{
  const auto *typed_modes = static_cast<const D3D12_WRITEBUFFERIMMEDIATE_MODE *>(modes);
  std::vector<trace::ObjectId> refs;
  append_object_ref_id(refs, object_id(command_list));
  std::ostringstream payload;
  payload << "{\"count\":" << count << ",\"writes\":[";
  for (std::uint32_t index = 0; parameters && index < count; ++index) {
    if (index) {
      payload << ",";
    }
    payload << "{";
    append_gpu_virtual_address_binding_json_with_ref(payload, "dest", parameters[index].Dest, refs);
    payload << ",\"value\":" << parameters[index].Value
            << ",\"mode\":";
    if (typed_modes) {
      payload << static_cast<unsigned int>(typed_modes[index]);
    } else {
      payload << "null";
    }
    payload << "}";
  }
  payload << "]}";
  return record_call_with_object_ids(
      "ID3D12GraphicsCommandList2::WriteBufferImmediate",
      payload.str().c_str(),
      std::move(refs));
}

std::uint64_t record_begin_render_pass(
    const void *command_list,
    std::uint32_t render_targets_count,
    const RenderPassRenderTargetDesc *render_targets,
    const RenderPassDepthStencilDesc *depth_stencil,
    std::uint32_t flags)
{
  std::vector<const void *> refs = {command_list};
  std::ostringstream payload;
  payload << "{\"render_targets_count\":" << render_targets_count
          << ",\"render_targets\":[";
  for (std::uint32_t index = 0; render_targets && index < render_targets_count; ++index) {
    if (index) {
      payload << ",";
    }
    payload << render_pass_render_target_json(render_targets[index], refs);
  }
  payload << "],\"has_depth_stencil\":" << (depth_stencil ? "true" : "false")
          << ",\"depth_stencil\":";
  if (depth_stencil) {
    payload << render_pass_depth_stencil_json(*depth_stencil, refs);
  } else {
    payload << "null";
  }
  payload << ",\"flags\":" << flags
          << "}";
  return record_call("ID3D12GraphicsCommandList4::BeginRenderPass", payload.str().c_str(), refs.data(), refs.size());
}

std::uint64_t record_end_render_pass(const void *command_list)
{
  const void *refs[] = {command_list};
  return record_call("ID3D12GraphicsCommandList4::EndRenderPass", "{}", refs, 1);
}

std::uint64_t record_temporal_upscale(
    const void *command_list,
    std::uint32_t input_content_width,
    std::uint32_t input_content_height,
    bool auto_exposure,
    bool in_reset,
    bool depth_reversed,
    bool motion_vector_in_display_res,
    const void *color,
    const void *depth,
    const void *motion_vector,
    const void *output,
    float motion_vector_scale_x,
    float motion_vector_scale_y,
    float pre_exposure,
    const void *exposure_texture,
    float jitter_offset_x,
    float jitter_offset_y)
{
  std::ostringstream payload;
  payload << "{\"input_content_width\":" << input_content_width
          << ",\"input_content_height\":" << input_content_height
          << ",\"auto_exposure\":" << (auto_exposure ? "true" : "false")
          << ",\"in_reset\":" << (in_reset ? "true" : "false")
          << ",\"depth_reversed\":" << (depth_reversed ? "true" : "false")
          << ",\"motion_vector_in_display_res\":" << (motion_vector_in_display_res ? "true" : "false")
          << ",\"color_object_id\":" << object_id(color)
          << ",\"depth_object_id\":" << object_id(depth)
          << ",\"motion_vector_object_id\":" << object_id(motion_vector)
          << ",\"output_object_id\":" << object_id(output)
          << ",\"motion_vector_scale_x\":" << motion_vector_scale_x
          << ",\"motion_vector_scale_y\":" << motion_vector_scale_y
          << ",\"pre_exposure\":" << pre_exposure
          << ",\"exposure_texture_object_id\":" << object_id(exposure_texture)
          << ",\"jitter_offset_x\":" << jitter_offset_x
          << ",\"jitter_offset_y\":" << jitter_offset_y
          << "}";
  const void *refs[] = {command_list, color, depth, motion_vector, output, exposure_texture};
  return record_call("ID3D12GraphicsCommandListExt::TemporalUpscale", payload.str().c_str(), refs, 6);
}

} // namespace apitrace::d3d12
