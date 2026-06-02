#include "apitrace/metal_capi.hpp"

#include "apitrace/api_types.hpp"
#include "apitrace/asset_index.hpp"
#include "apitrace/metal_event_types.hpp"
#include "trace/src/payload_object_refs.hpp"
#include "apitrace/translation_trace_recorder.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace apitrace::metal {

namespace detail {

thread_local std::uint64_t g_current_d3d_sequence = 0;

std::uint64_t monotonic_nanoseconds()
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void update_atomic_max(std::atomic_uint64_t &target, std::uint64_t value)
{
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

struct MetalCapiStats {
  std::atomic_uint64_t lock_wait_count{0};
  std::atomic_uint64_t lock_wait_ns{0};
  std::atomic_uint64_t max_lock_wait_ns{0};
  std::atomic_uint64_t record_count{0};
  std::atomic_uint64_t record_ns{0};
  std::atomic_uint64_t max_record_ns{0};
  std::atomic_uint64_t raw_payload_count{0};
  std::atomic_uint64_t raw_payload_bytes{0};
  std::atomic_uint64_t json_payload_count{0};
  std::atomic_uint64_t json_payload_bytes{0};
};

MetalCapiStats g_capi_stats;

class TimedSessionLock {
public:
  explicit TimedSessionLock(std::mutex &mutex)
      : lock_(mutex, std::defer_lock)
  {
    const auto start_ns = monotonic_nanoseconds();
    lock_.lock();
    const auto elapsed_ns = monotonic_nanoseconds() - start_ns;
    if (elapsed_ns != 0) {
      g_capi_stats.lock_wait_count.fetch_add(1, std::memory_order_relaxed);
      g_capi_stats.lock_wait_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
      update_atomic_max(g_capi_stats.max_lock_wait_ns, elapsed_ns);
    }
  }

private:
  std::unique_lock<std::mutex> lock_;
};

std::string copy_c_string(const char *text)
{
  return text == nullptr ? std::string() : std::string(text);
}

std::string dump_json(const json &payload)
{
  return payload.empty() ? std::string("{}") : payload.dump();
}

void append_json_key(std::string &output, bool &first, const char *key)
{
  if (!first) {
    output.push_back(',');
  }
  first = false;
  output.push_back('"');
  output += key;
  output += "\":";
}

template <typename Value>
void append_json_number(std::string &output, Value value)
{
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc()) {
    output.append(buffer, result.ptr);
  }
}

void append_json_u64(std::string &output, bool &first, const char *key, std::uint64_t value)
{
  append_json_key(output, first, key);
  append_json_number(output, value);
}

void append_json_i64(std::string &output, bool &first, const char *key, std::int64_t value)
{
  append_json_key(output, first, key);
  append_json_number(output, value);
}

std::string finish_json_object(std::string output)
{
  output.push_back('}');
  return output;
}

std::string begin_json_object(std::size_t reserve = 128)
{
  std::string output;
  output.reserve(reserve);
  output.push_back('{');
  return output;
}

std::string scope_kind_name(apitrace_metal_scope_kind scope)
{
  switch (scope) {
  case APITRACE_METAL_SCOPE_COMMAND_BUFFER:
    return "command_buffer";
  case APITRACE_METAL_SCOPE_ENCODER:
    return "encoder";
  case APITRACE_METAL_SCOPE_DRAW_TO_METAL:
    return "draw_to_metal_calls";
  }
  return "opaque";
}

std::string capi_stats_json()
{
  std::ostringstream output;
  output << "{\"record_type\":\"metal_capi_stats\""
         << ",\"lock_wait_count\":" << g_capi_stats.lock_wait_count.load(std::memory_order_relaxed)
         << ",\"lock_wait_ns\":" << g_capi_stats.lock_wait_ns.load(std::memory_order_relaxed)
         << ",\"max_lock_wait_ns\":" << g_capi_stats.max_lock_wait_ns.load(std::memory_order_relaxed)
         << ",\"record_count\":" << g_capi_stats.record_count.load(std::memory_order_relaxed)
         << ",\"record_ns\":" << g_capi_stats.record_ns.load(std::memory_order_relaxed)
         << ",\"max_record_ns\":" << g_capi_stats.max_record_ns.load(std::memory_order_relaxed)
         << ",\"raw_payload_count\":" << g_capi_stats.raw_payload_count.load(std::memory_order_relaxed)
         << ",\"raw_payload_bytes\":" << g_capi_stats.raw_payload_bytes.load(std::memory_order_relaxed)
         << ",\"json_payload_count\":" << g_capi_stats.json_payload_count.load(std::memory_order_relaxed)
         << ",\"json_payload_bytes\":" << g_capi_stats.json_payload_bytes.load(std::memory_order_relaxed)
         << "}";
  return output.str();
}

void append_fence_ops_json(
    std::ostringstream &output,
    const std::uint64_t *wait_fences,
    std::uint32_t wait_fence_count,
    const std::uint64_t *update_fences,
    std::uint32_t update_fence_count)
{
  output << ",\"fence_ops\":{\"schema\":\"blit-fence-v2\",\"stages\":0,\"wait_fences\":[";
  for (std::uint32_t index = 0; index < wait_fence_count; ++index) {
    if (index != 0) {
      output << ',';
    }
    output << wait_fences[index];
  }
  output << "],\"update_fences\":[";
  for (std::uint32_t index = 0; index < update_fence_count; ++index) {
    if (index != 0) {
      output << ',';
    }
    output << update_fences[index];
  }
  output << "]}";
}

std::string blit_encoder_fence_batch_payload_json(
    std::uint64_t command_buffer_object_id,
    const std::uint64_t *wait_fences,
    std::uint32_t wait_fence_count,
    const std::uint64_t *update_fences,
    std::uint32_t update_fence_count)
{
  std::ostringstream output;
  output << "{\"command_buffer_id\":" << command_buffer_object_id
         << ",\"kind\":\"dxmt_blit_encoder_batch\",\"ops\":[],\"fence_ops\":{\"schema\":\"blit-fence-v2\",\"stages\":0"
         << ",\"wait_fences\":[";
  for (std::uint32_t index = 0; index < wait_fence_count; ++index) {
    if (index != 0) {
      output << ',';
    }
    output << wait_fences[index];
  }
  output << "],\"update_fences\":[";
  for (std::uint32_t index = 0; index < update_fence_count; ++index) {
    if (index != 0) {
      output << ',';
    }
    output << update_fences[index];
  }
  output << "]}}";
  return output.str();
}

std::string blit_fence_batch_payload_json(
    const std::uint64_t *wait_fences,
    std::uint32_t wait_fence_count,
    const std::uint64_t *update_fences,
    std::uint32_t update_fence_count)
{
  std::ostringstream output;
  output << "{\"kind\":\"dxmt_blit_batch\",\"ops\":[]";
  append_fence_ops_json(output, wait_fences, wait_fence_count, update_fences, update_fence_count);
  output << '}';
  return output.str();
}

std::string blit_encoder_copy_texture_batch_payload_json(
    std::uint64_t command_buffer_object_id,
    std::uint64_t source_texture_id,
    std::uint64_t destination_texture_id,
    std::uint64_t source_origin_x,
    std::uint64_t source_origin_y,
    std::uint64_t source_origin_z,
    std::uint64_t source_size_width,
    std::uint64_t source_size_height,
    std::uint64_t source_size_depth,
    std::uint32_t source_slice,
    std::uint32_t source_level,
    std::uint64_t destination_origin_x,
    std::uint64_t destination_origin_y,
    std::uint64_t destination_origin_z,
    std::uint32_t destination_slice,
    std::uint32_t destination_level,
    const std::uint64_t *wait_fences,
    std::uint32_t wait_fence_count,
    const std::uint64_t *update_fences,
    std::uint32_t update_fence_count)
{
  std::ostringstream output;
  output << "{\"command_buffer_id\":" << command_buffer_object_id
         << ",\"kind\":\"dxmt_blit_encoder_batch\",\"ops\":[{\"op\":\"copy_texture\""
         << ",\"source_texture_id\":" << source_texture_id
         << ",\"payload\":\"{\\\"source_texture\\\":" << source_texture_id
         << ",\\\"destination_origin\\\":[" << destination_origin_x << ',' << destination_origin_y << ',' << destination_origin_z << ']'
         << ",\\\"source_level\\\":" << source_level
         << ",\\\"source_slice\\\":" << source_slice
         << ",\\\"destination_slice\\\":" << destination_slice
         << ",\\\"destination_level\\\":" << destination_level
         << ",\\\"source_size\\\":[" << source_size_width << ',' << source_size_height << ',' << source_size_depth << ']'
         << ",\\\"source_origin\\\":[" << source_origin_x << ',' << source_origin_y << ',' << source_origin_z << ']'
         << ",\\\"destination_texture\\\":" << destination_texture_id
         << "}\",\"destination_texture_id\":" << destination_texture_id << "}]";
  if (wait_fence_count != 0 || update_fence_count != 0) {
    append_fence_ops_json(output, wait_fences, wait_fence_count, update_fences, update_fence_count);
  }
  output << '}';
  return output.str();
}

std::string blit_encoder_copy_texture_ops_batch_payload_json(
    std::uint64_t command_buffer_object_id,
    const apitrace_metal_copy_texture_op_t *copy_ops,
    std::uint32_t copy_op_count,
    const std::uint64_t *wait_fences,
    std::uint32_t wait_fence_count,
    const std::uint64_t *update_fences,
    std::uint32_t update_fence_count)
{
  std::ostringstream output;
  output << "{\"command_buffer_id\":" << command_buffer_object_id
         << ",\"kind\":\"dxmt_blit_encoder_batch\",\"schema\":\"copy-texture-v2\",\"copy_texture_ops\":[";
  for (std::uint32_t index = 0; index < copy_op_count; ++index) {
    const auto &op = copy_ops[index];
    if (index != 0) {
      output << ',';
    }
    output << '['
           << op.source_texture_id << ','
           << op.destination_texture_id << ','
           << op.source_origin_x << ','
           << op.source_origin_y << ','
           << op.source_origin_z << ','
           << op.source_size_width << ','
           << op.source_size_height << ','
           << op.source_size_depth << ','
           << op.source_slice << ','
           << op.source_level << ','
           << op.destination_origin_x << ','
           << op.destination_origin_y << ','
           << op.destination_origin_z << ','
           << op.destination_slice << ','
           << op.destination_level
           << ']';
  }
  output << ']';
  if (wait_fence_count != 0 || update_fence_count != 0) {
    append_fence_ops_json(output, wait_fences, wait_fence_count, update_fences, update_fence_count);
  }
  output << '}';
  return output.str();
}

struct SessionState {
  std::mutex mutex;
  std::mutex recorder_mutex;
  trace::TraceBundleWriter writer;
  MetalBridge bridge{MetalBridgeOptions{}};
  TranslationTraceRecorder recorder;
  MetalObjectRegistry object_registry;
  std::unordered_map<std::uint64_t, std::string> object_metadata_payloads;
  bool open = false;
};

std::vector<trace::BlobId> blob_refs_for_asset(const trace::AssetRecord &asset)
{
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return {};
  }

  return {asset.blob_id};
}

void append_object_ref(std::vector<std::uint64_t> &refs, std::uint64_t object_id)
{
  if (object_id == 0) {
    return;
  }
  if (std::find(refs.begin(), refs.end(), object_id) == refs.end()) {
    refs.push_back(object_id);
  }
}

std::vector<std::uint64_t> object_refs(std::initializer_list<std::uint64_t> ids)
{
  std::vector<std::uint64_t> refs;
  refs.reserve(ids.size());
  for (const auto object_id : ids) {
    append_object_ref(refs, object_id);
  }
  return refs;
}

MetalTraceRecord make_metal_trace_record(
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    std::vector<std::uint64_t> object_refs,
    std::vector<trace::BlobId> blob_refs,
    const json &payload,
    std::uint64_t frame_id = 0)
{
  MetalTraceRecord trace_record;
  trace_record.call_kind = call_kind;
  trace_record.d3d_sequence = g_current_d3d_sequence;
  trace_record.frame_id = frame_id;
  trace_record.object_id = object_id;
  trace_record.object_refs = std::move(object_refs);
  trace::append_payload_object_refs(payload, trace_record.object_refs);
  trace_record.payload_refs_scanned = true;
  trace_record.blob_refs = std::move(blob_refs);
  trace_record.translated_call_name = std::move(function_name);
  trace_record.translation_link_payload = dump_json(payload);
  detail::g_capi_stats.json_payload_count.fetch_add(1, std::memory_order_relaxed);
  detail::g_capi_stats.json_payload_bytes.fetch_add(trace_record.translation_link_payload.size(), std::memory_order_relaxed);
  return trace_record;
}

MetalTraceRecord make_metal_trace_record(
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    const json &payload)
{
  return make_metal_trace_record(
      call_kind,
      std::move(function_name),
      object_id,
      {},
      {},
      payload);
}

MetalTraceRecord make_metal_trace_record_raw_payload(
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    std::vector<std::uint64_t> object_refs,
    std::vector<trace::BlobId> blob_refs,
    const char *payload_json,
    std::uint64_t frame_id = 0,
    bool payload_refs_scanned = false)
{
  MetalTraceRecord trace_record;
  trace_record.call_kind = call_kind;
  trace_record.d3d_sequence = g_current_d3d_sequence;
  trace_record.frame_id = frame_id;
  trace_record.object_id = object_id;
  trace_record.object_refs = std::move(object_refs);
  trace_record.blob_refs = std::move(blob_refs);
  trace_record.translated_call_name = std::move(function_name);
  trace_record.translation_link_payload =
      (payload_json == nullptr || payload_json[0] == '\0') ? std::string("{}") : std::string(payload_json);
  trace_record.payload_refs_scanned = payload_refs_scanned;
  detail::g_capi_stats.raw_payload_count.fetch_add(1, std::memory_order_relaxed);
  detail::g_capi_stats.raw_payload_bytes.fetch_add(trace_record.translation_link_payload.size(), std::memory_order_relaxed);
  return trace_record;
}

std::uint64_t submit_metal_trace_record(SessionState &state, MetalTraceRecord &&trace_record)
{
  std::unique_lock<std::mutex> lock(state.recorder_mutex);
  const auto record_start_ns = monotonic_nanoseconds();
  state.recorder.record_metal_call(std::move(trace_record));
  const auto record_ns = monotonic_nanoseconds() - record_start_ns;
  detail::g_capi_stats.record_count.fetch_add(1, std::memory_order_relaxed);
  detail::g_capi_stats.record_ns.fetch_add(record_ns, std::memory_order_relaxed);
  update_atomic_max(detail::g_capi_stats.max_record_ns, record_ns);
  const auto sequence = state.recorder.current_metal_sequence();
  return sequence;
}

std::uint64_t current_metal_sequence(SessionState &state);

trace::ObjectKind object_kind_for_asset(trace::MetalAssetKind kind)
{
  switch (kind) {
  case trace::MetalAssetKind::Buffer:
  case trace::MetalAssetKind::Texture:
  case trace::MetalAssetKind::Heap:
    return trace::ObjectKind::Resource;
  case trace::MetalAssetKind::RenderPipeline:
  case trace::MetalAssetKind::ComputePipeline:
  case trace::MetalAssetKind::DepthStencilState:
  case trace::MetalAssetKind::SamplerState:
  case trace::MetalAssetKind::ArgumentEncoder:
    return trace::ObjectKind::PipelineState;
  case trace::MetalAssetKind::Library:
  default:
    return trace::ObjectKind::Shader;
  }
}

void track_object(
    SessionState &state,
    std::uint64_t object_id,
    trace::MetalAssetKind asset_kind,
    const trace::AssetRecord &asset,
    std::string payload)
{
  if (object_id == 0) {
    return;
  }

  MetalTrackedObject tracked;
  tracked.object_id = object_id;
  tracked.kind = object_kind_for_asset(asset_kind);
  tracked.asset_kind = asset_kind;
  tracked.blob_refs = blob_refs_for_asset(asset);
  tracked.asset_relative_path = asset.relative_path;
  tracked.payload = std::move(payload);
  tracked.debug_label = asset.debug_name.empty() ? asset.relative_path.filename().generic_string() : asset.debug_name;
  state.object_registry.track(tracked);
}

std::uint64_t record_metal_call(
    SessionState &state,
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    std::vector<std::uint64_t> object_refs,
    std::vector<trace::BlobId> blob_refs,
    const json &payload,
    std::uint64_t frame_id = 0)
{
  return submit_metal_trace_record(
      state,
      make_metal_trace_record(
          call_kind,
          std::move(function_name),
          object_id,
          std::move(object_refs),
          std::move(blob_refs),
          payload,
          frame_id));
}

std::uint64_t record_metal_call_raw_payload(
    SessionState &state,
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    std::vector<std::uint64_t> object_refs,
    std::vector<trace::BlobId> blob_refs,
    const char *payload_json,
    std::uint64_t frame_id = 0,
    bool payload_refs_scanned = false)
{
  return submit_metal_trace_record(
      state,
      make_metal_trace_record_raw_payload(
          call_kind,
          std::move(function_name),
          object_id,
          std::move(object_refs),
          std::move(blob_refs),
          payload_json,
          frame_id,
      payload_refs_scanned));
}

std::uint64_t record_metal_call_raw_payload(
    SessionState &state,
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    std::vector<std::uint64_t> object_refs,
    std::string payload_json,
    std::uint64_t frame_id = 0)
{
  return submit_metal_trace_record(
      state,
      make_metal_trace_record_raw_payload(
          call_kind,
          std::move(function_name),
          object_id,
          std::move(object_refs),
          {},
          payload_json.c_str(),
          frame_id,
          true));
}

std::uint64_t record_metal_call(
    SessionState &state,
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    const json &payload)
{
  return record_metal_call(
      state,
      call_kind,
      std::move(function_name),
      object_id,
      {},
      {},
      payload);
}

std::vector<std::uint8_t> copy_bytes(const void *bytes, std::uint64_t size)
{
  std::vector<std::uint8_t> copied;
  if (bytes == nullptr || size == 0) {
    return copied;
  }
  copied.resize(static_cast<std::size_t>(size));
  std::memcpy(copied.data(), bytes, static_cast<std::size_t>(size));
  return copied;
}

trace::AssetRecord make_asset_record(std::uint64_t object_id, std::string debug_name, const void *bytes, std::uint64_t size)
{
  trace::AssetRecord asset;
  asset.blob_id = object_id;
  asset.debug_name = std::move(debug_name);
  asset.payload_bytes = copy_bytes(bytes, size);
  return asset;
}

trace::AssetRecord register_buffer_range_asset_locked(
    SessionState &state,
    std::uint64_t object_id,
    const char *debug_name,
    const void *bytes,
    std::uint64_t size)
{
  (void)object_id;
  const auto next_blob_id = current_metal_sequence(state) + 1;
  auto asset = make_asset_record(
      next_blob_id,
      debug_name == nullptr ? "metal-buffer-range" : debug_name,
      bytes,
      size);
  asset.kind = trace::AssetKind::Buffer;
  return state.writer.register_asset(std::move(asset));
}

void attach_buffer_range_asset(
    SessionState &state,
    json &payload,
    std::vector<trace::BlobId> &blob_refs,
    std::uint64_t object_id,
    const char *debug_name,
    const void *bytes,
    std::uint64_t size)
{
  if (bytes == nullptr || size == 0) {
    return;
  }

  const auto asset = register_buffer_range_asset_locked(state, object_id, debug_name, bytes, size);
  if (!asset.relative_path.empty()) {
    payload["source_asset_path"] = asset.relative_path.generic_string();
    payload["source_asset_size"] = size;
  }
  const auto refs = blob_refs_for_asset(asset);
  blob_refs.insert(blob_refs.end(), refs.begin(), refs.end());
}

void record_present_frame_locked(
    SessionState &state,
    std::uint64_t frame_index,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t row_pitch,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    const std::vector<std::uint8_t> &bgra_bytes)
{
  if (bgra_bytes.empty()) {
    return;
  }

  trace::AssetRecord asset;
  const auto next_sequence = current_metal_sequence(state) + 1;
  asset.blob_id = next_sequence;
  asset.kind = trace::AssetKind::Texture;
  asset.debug_name = "metal-present-frame";
  asset.payload_bytes = bgra_bytes;
  asset = state.writer.register_asset(std::move(asset));

  trace::EventRecord event;
  event.kind = trace::EventKind::ResourceBlob;
  event.callsite.sequence = next_sequence;
  event.callsite.function_name = "resource_blob";
  event.object_kind = trace::ObjectKind::Unknown;
  event.object_debug_name = "MetalPresentFrame";
  event.blob_refs = {asset.blob_id};
  event.payload = json{{"frame_index", frame_index},
                       {"width", width},
                       {"height", height},
                       {"row_pitch", row_pitch},
                       {"sync_interval", sync_interval},
                       {"flags", flags},
                       {"format", "bgra8"},
                       {"frame_path", asset.relative_path.generic_string()}}
                      .dump();
  state.writer.append_call_event(event);
}

void append_link(
    SessionState &state,
    apitrace_metal_scope_kind scope,
    std::uint64_t d3d_sequence,
    std::uint64_t metal_sequence_begin,
    std::uint64_t metal_sequence_end,
    std::uint64_t frame_id,
    const char *payload_utf8)
{
  trace::TranslationLinkRecord record;
  record.scope_kind = scope_kind_name(scope);
  record.d3d_sequence = d3d_sequence;
  record.metal_sequence_begin = metal_sequence_begin;
  record.metal_sequence_end = metal_sequence_end;
  record.frame_id = frame_id;
  record.payload = copy_c_string(payload_utf8);
  state.recorder.append_link_record(record);
}

std::uint64_t current_metal_sequence(SessionState &state)
{
  detail::TimedSessionLock lock(state.recorder_mutex);
  return state.recorder.current_metal_sequence();
}

} // namespace detail

} // namespace apitrace::metal

struct apitrace_metal_session {
  apitrace::metal::detail::SessionState state;
};

namespace apitrace::metal {

void record_present_frame(
    apitrace_metal_session_t *session,
    std::uint64_t frame_index,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t row_pitch,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    const std::vector<std::uint8_t> &bgra_bytes)
{
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  detail::record_present_frame_locked(
      session->state,
      frame_index,
      width,
      height,
      row_pitch,
      sync_interval,
      flags,
      bgra_bytes);
}

} // namespace apitrace::metal

APITRACE_METAL_API apitrace_metal_session_t *apitrace_metal_session_open(const char *bundle_root)
{
  if (bundle_root == nullptr || *bundle_root == '\0') {
    return nullptr;
  }

  auto session = std::make_unique<apitrace_metal_session>();
  auto &state = session->state;
  if (!state.writer.open(bundle_root, apitrace::trace::TraceBundleOpenMode::SidebandOnly)) {
    return nullptr;
  }
  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "apitrace_metal_capi";
  metadata.has_metal_callstream = true;
  state.writer.write_metadata(metadata);

  if (!state.bridge.initialize()) {
    state.writer.close();
    return nullptr;
  }

  apitrace::metal::TranslationTraceRecorderOptions recorder_options;
  recorder_options.enable_metal_trace = true;
  recorder_options.trace_label = "metal_capi";
  apitrace::trace::TranslationLinkStreamOptions link_options;
  link_options.producer_name = "apitrace_metal_capi";
  if (!state.recorder.open(state.writer, state.bridge, std::move(recorder_options), std::move(link_options))) {
    state.bridge.shutdown();
    state.writer.close();
    return nullptr;
  }

  state.open = true;
  return session.release();
}

APITRACE_METAL_API void apitrace_metal_session_close(apitrace_metal_session_t *session)
{
  if (session == nullptr) {
    return;
  }

  auto &state = session->state;
  {
    apitrace::metal::detail::TimedSessionLock lock(state.mutex);
    if (state.open) {
      state.writer.append_analysis_line("metal-capi-stats", apitrace::metal::detail::capi_stats_json());
      state.recorder.close();
      state.bridge.shutdown();
      state.writer.close();
      state.open = false;
    }
  }
  delete session;
}

APITRACE_METAL_API void apitrace_metal_session_seal_checkpoint(apitrace_metal_session_t *session)
{
  if (session == nullptr) {
    return;
  }

  auto &state = session->state;
  apitrace::metal::detail::TimedSessionLock lock(state.mutex);
  if (state.open) {
    apitrace::metal::detail::TimedSessionLock recorder_lock(state.recorder_mutex);
    state.writer.append_analysis_line("metal-capi-stats", apitrace::metal::detail::capi_stats_json());
    state.writer.seal_checkpoint();
  }
}

APITRACE_METAL_API void apitrace_metal_set_current_d3d_sequence(
    apitrace_metal_session_t *session,
    uint64_t d3d_seq)
{
  (void)session;
  apitrace::metal::detail::g_current_d3d_sequence = d3d_seq;
}

APITRACE_METAL_API uint64_t apitrace_metal_current_metal_sequence(apitrace_metal_session_t *session)
{
  if (session == nullptr) {
    return 0;
  }

  return apitrace::metal::detail::current_metal_sequence(session->state);
}

#define APITRACE_METAL_RECORD_JSON(function_name_literal, call_kind_value, object_id_value, ...)                     \
  do {                                                                                                                \
    if (session == nullptr) {                                                                                         \
      return;                                                                                                         \
    }                                                                                                                 \
    const auto payload = (__VA_ARGS__);                                                                               \
    auto trace_record = apitrace::metal::detail::make_metal_trace_record(                                             \
        call_kind_value, function_name_literal, object_id_value, {}, {}, payload);                                     \
    apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));                            \
  } while (false)

#define APITRACE_METAL_RECORD_JSON_RET(function_name_literal, call_kind_value, object_id_value, ...)                 \
  do {                                                                                                                \
    if (session == nullptr) {                                                                                         \
      return 0;                                                                                                       \
    }                                                                                                                 \
    const auto payload = (__VA_ARGS__);                                                                               \
    auto trace_record = apitrace::metal::detail::make_metal_trace_record(                                             \
        call_kind_value, function_name_literal, object_id_value, {}, {}, payload);                                     \
    return apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));                     \
  } while (false)

APITRACE_METAL_API uint64_t apitrace_metal_command_buffer_begin(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_object_id,
    uint64_t frame_id,
    const char *label_utf8)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::TranslationCommandBufferInfo info;
  info.frame_id = frame_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.label = apitrace::metal::detail::copy_c_string(label_utf8);
  info.payload = json{{"frame_id", frame_id}, {"label", info.label}}.dump();
  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.begin_command_buffer(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_command_buffer_commit(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_object_id)
{
  (void)command_buffer_object_id;
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.end_command_buffer();
}

APITRACE_METAL_API uint64_t apitrace_metal_render_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *render_pass_info_json)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::TranslationEncoderInfo info;
  info.encoder_id = encoder_object_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.pass_kind = apitrace::metal::TranslationPassKind::Render;
  info.payload = json{{"command_buffer_id", command_buffer_object_id},
                      {"render_pass_info", apitrace::metal::detail::copy_c_string(render_pass_info_json)}}
                     .dump();
  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.begin_encoder(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_render_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id)
{
  (void)encoder_object_id;
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.end_encoder();
}

APITRACE_METAL_API uint64_t apitrace_metal_compute_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::TranslationEncoderInfo info;
  info.encoder_id = encoder_object_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.pass_kind = apitrace::metal::TranslationPassKind::Compute;
  info.payload = json{{"command_buffer_id", command_buffer_object_id},
                      {"payload", apitrace::metal::detail::copy_c_string(payload_json)}}
                     .dump();
  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.begin_encoder(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_compute_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id)
{
  (void)encoder_object_id;
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.end_encoder();
}

APITRACE_METAL_API uint64_t apitrace_metal_blit_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::TranslationEncoderInfo info;
  info.encoder_id = encoder_object_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.pass_kind = apitrace::metal::TranslationPassKind::Blit;
  info.payload = json{{"command_buffer_id", command_buffer_object_id},
                      {"payload", apitrace::metal::detail::copy_c_string(payload_json)}}
                     .dump();
  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.begin_encoder(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id)
{
  (void)encoder_object_id;
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.recorder_mutex);
  session->state.recorder.end_encoder();
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return;
  }

  auto payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    payload = json::object();
  }
  payload["command_buffer_id"] = command_buffer_object_id;

  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::BlitEncoderBatch,
      "MTLCommandBuffer.blitCommandEncoderBatch",
      encoder_object_id,
      {command_buffer_object_id},
      {},
      payload);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_batch_with_command_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return;
  }

  auto trace_record = apitrace::metal::detail::make_metal_trace_record_raw_payload(
      apitrace::trace::MetalCallKind::BlitEncoderBatch,
      "MTLCommandBuffer.blitCommandEncoderBatch",
      encoder_object_id,
      {command_buffer_object_id},
      {},
      payload_json);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_fence_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count)
{
  if (session == nullptr) {
    return;
  }

  if ((wait_fence_count != 0 && wait_fences == nullptr) ||
      (update_fence_count != 0 && update_fences == nullptr)) {
    return;
  }

  const auto payload = apitrace::metal::detail::blit_encoder_fence_batch_payload_json(
      command_buffer_object_id,
      wait_fences,
      wait_fence_count,
      update_fences,
      update_fence_count);

  auto trace_record = apitrace::metal::detail::make_metal_trace_record_raw_payload(
      apitrace::trace::MetalCallKind::BlitEncoderBatch,
      "MTLCommandBuffer.blitCommandEncoderBatch",
      encoder_object_id,
      {command_buffer_object_id},
      {},
      payload.c_str());
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_copy_texture_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    uint64_t source_texture_id,
    uint64_t destination_texture_id,
    uint64_t source_origin_x,
    uint64_t source_origin_y,
    uint64_t source_origin_z,
    uint64_t source_size_width,
    uint64_t source_size_height,
    uint64_t source_size_depth,
    uint32_t source_slice,
    uint32_t source_level,
    uint64_t destination_origin_x,
    uint64_t destination_origin_y,
    uint64_t destination_origin_z,
    uint32_t destination_slice,
    uint32_t destination_level,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count)
{
  if (session == nullptr) {
    return;
  }

  if ((wait_fence_count != 0 && wait_fences == nullptr) ||
      (update_fence_count != 0 && update_fences == nullptr)) {
    return;
  }

  const auto payload = apitrace::metal::detail::blit_encoder_copy_texture_batch_payload_json(
      command_buffer_object_id,
      source_texture_id,
      destination_texture_id,
      source_origin_x,
      source_origin_y,
      source_origin_z,
      source_size_width,
      source_size_height,
      source_size_depth,
      source_slice,
      source_level,
      destination_origin_x,
      destination_origin_y,
      destination_origin_z,
      destination_slice,
      destination_level,
      wait_fences,
      wait_fence_count,
      update_fences,
      update_fence_count);

  auto trace_record = apitrace::metal::detail::make_metal_trace_record_raw_payload(
      apitrace::trace::MetalCallKind::BlitEncoderBatch,
      "MTLCommandBuffer.blitCommandEncoderBatch",
      encoder_object_id,
      {command_buffer_object_id, source_texture_id, destination_texture_id},
      {},
      payload.c_str());
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_copy_texture_ops_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const apitrace_metal_copy_texture_op_t *copy_ops,
    uint32_t copy_op_count,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count)
{
  if (session == nullptr) {
    return;
  }

  if ((copy_op_count != 0 && copy_ops == nullptr) ||
      (wait_fence_count != 0 && wait_fences == nullptr) ||
      (update_fence_count != 0 && update_fences == nullptr)) {
    return;
  }

  const auto payload = apitrace::metal::detail::blit_encoder_copy_texture_ops_batch_payload_json(
      command_buffer_object_id,
      copy_ops,
      copy_op_count,
      wait_fences,
      wait_fence_count,
      update_fences,
      update_fence_count);

  std::vector<std::uint64_t> object_refs;
  object_refs.reserve(1 + copy_op_count * 2);
  apitrace::metal::detail::append_object_ref(object_refs, command_buffer_object_id);
  for (std::uint32_t index = 0; index < copy_op_count; ++index) {
    apitrace::metal::detail::append_object_ref(object_refs, copy_ops[index].source_texture_id);
    apitrace::metal::detail::append_object_ref(object_refs, copy_ops[index].destination_texture_id);
  }

  auto trace_record = apitrace::metal::detail::make_metal_trace_record_raw_payload(
      apitrace::trace::MetalCallKind::BlitEncoderBatch,
      "MTLCommandBuffer.blitCommandEncoderBatch",
      encoder_object_id,
      std::move(object_refs),
      {},
      payload.c_str(),
      0,
      true);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_set_render_pipeline_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t pipeline_state_object_id)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(48);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "pipeline_state_id", pipeline_state_object_id);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      encoder_id,
      apitrace::metal::detail::object_refs({pipeline_state_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_vertex_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(80);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "buffer_id", buffer_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetVertexBuffer,
      "MTLRenderCommandEncoder.setVertexBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_vertex_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index,
    const void *bytes,
    uint64_t bytes_size)
{
  if (session == nullptr) {
    return;
  }
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto payload = json{{"buffer_id", buffer_object_id}, {"offset", offset}, {"index", index}};
  std::vector<apitrace::trace::BlobId> blob_refs;
  apitrace::metal::detail::attach_buffer_range_asset(
      session->state, payload, blob_refs, buffer_object_id,
      "metal-set-vertex-buffer", bytes, bytes_size);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::SetVertexBuffer,
      "MTLRenderCommandEncoder.setVertexBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      std::move(blob_refs),
      payload);
}

APITRACE_METAL_API void apitrace_metal_set_vertex_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t texture_object_id, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(64);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "texture_id", texture_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetVertexTexture,
      "MTLRenderCommandEncoder.setVertexTexture",
      encoder_id,
      apitrace::metal::detail::object_refs({texture_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_vertex_sampler_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t sampler_state_object_id, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(72);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "sampler_state_id", sampler_state_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetVertexSamplerState,
      "MTLRenderCommandEncoder.setVertexSamplerState",
      encoder_id,
      apitrace::metal::detail::object_refs({sampler_state_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_fragment_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t buffer_object_id, uint64_t offset, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(80);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "buffer_id", buffer_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetFragmentBuffer,
      "MTLRenderCommandEncoder.setFragmentBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_fragment_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index,
    const void *bytes,
    uint64_t bytes_size)
{
  if (session == nullptr) {
    return;
  }
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto payload = json{{"buffer_id", buffer_object_id}, {"offset", offset}, {"index", index}};
  std::vector<apitrace::trace::BlobId> blob_refs;
  apitrace::metal::detail::attach_buffer_range_asset(
      session->state, payload, blob_refs, buffer_object_id,
      "metal-set-fragment-buffer", bytes, bytes_size);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::SetFragmentBuffer,
      "MTLRenderCommandEncoder.setFragmentBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      std::move(blob_refs),
      payload);
}

APITRACE_METAL_API void apitrace_metal_set_fragment_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t texture_object_id, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(64);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "texture_id", texture_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetFragmentTexture,
      "MTLRenderCommandEncoder.setFragmentTexture",
      encoder_id,
      apitrace::metal::detail::object_refs({texture_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_fragment_sampler_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t sampler_state_object_id, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(72);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "sampler_state_id", sampler_state_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetFragmentSamplerState,
      "MTLRenderCommandEncoder.setFragmentSamplerState",
      encoder_id,
      apitrace::metal::detail::object_refs({sampler_state_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_vertex_bytes(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t index, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setVertexBytes",
      apitrace::trace::MetalCallKind::SetVertexBytes,
      encoder_id,
      json{{"index", index}, {"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_set_fragment_bytes(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t index, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setFragmentBytes",
      apitrace::trace::MetalCallKind::SetFragmentBytes,
      encoder_id,
      json{{"index", index}, {"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_set_vertex_buffer_offset(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t offset, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(56);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetVertexBufferOffset,
      "MTLRenderCommandEncoder.setVertexBufferOffset",
      encoder_id,
      {},
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_fragment_buffer_offset(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t offset, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(56);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetFragmentBufferOffset,
      "MTLRenderCommandEncoder.setFragmentBufferOffset",
      encoder_id,
      {},
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_compute_pipeline_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t pipeline_state_object_id)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(48);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "pipeline_state_id", pipeline_state_object_id);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetComputePipelineState,
      "MTLComputeCommandEncoder.setComputePipelineState",
      encoder_id,
      apitrace::metal::detail::object_refs({pipeline_state_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_compute_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t buffer_object_id, uint64_t offset, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(80);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "buffer_id", buffer_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetComputeBuffer,
      "MTLComputeCommandEncoder.setComputeBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_compute_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index,
    const void *bytes,
    uint64_t bytes_size)
{
  if (session == nullptr) {
    return;
  }
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto payload = json{{"buffer_id", buffer_object_id}, {"offset", offset}, {"index", index}};
  std::vector<apitrace::trace::BlobId> blob_refs;
  apitrace::metal::detail::attach_buffer_range_asset(
      session->state, payload, blob_refs, buffer_object_id,
      "metal-set-compute-buffer", bytes, bytes_size);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::SetComputeBuffer,
      "MTLComputeCommandEncoder.setComputeBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      std::move(blob_refs),
      payload);
}

APITRACE_METAL_API void apitrace_metal_set_compute_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t texture_object_id, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(64);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "texture_id", texture_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetComputeTexture,
      "MTLComputeCommandEncoder.setComputeTexture",
      encoder_id,
      apitrace::metal::detail::object_refs({texture_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_compute_sampler_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t sampler_state_object_id, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(72);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "sampler_state_id", sampler_state_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetComputeSamplerState,
      "MTLComputeCommandEncoder.setComputeSamplerState",
      encoder_id,
      apitrace::metal::detail::object_refs({sampler_state_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_compute_buffer_offset(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t offset, uint32_t index)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(56);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetComputeBufferOffset,
      "MTLComputeCommandEncoder.setComputeBufferOffset",
      encoder_id,
      {},
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_draw_primitives(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint32_t vertex_start, uint32_t vertex_count, uint32_t instance_count, uint32_t base_instance)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(128);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "primitive_type", primitive_type);
  apitrace::metal::detail::append_json_u64(payload, first, "vertex_start", vertex_start);
  apitrace::metal::detail::append_json_u64(payload, first, "vertex_count", vertex_count);
  apitrace::metal::detail::append_json_u64(payload, first, "instance_count", instance_count);
  apitrace::metal::detail::append_json_u64(payload, first, "base_instance", base_instance);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DrawPrimitives,
      "MTLRenderCommandEncoder.drawPrimitives",
      encoder_id,
      {},
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_draw_indexed_primitives(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint32_t index_count, uint32_t index_type, uint64_t index_buffer_id, uint64_t index_buffer_offset, uint32_t instance_count, int32_t base_vertex, uint32_t base_instance)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(192);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "primitive_type", primitive_type);
  apitrace::metal::detail::append_json_u64(payload, first, "index_count", index_count);
  apitrace::metal::detail::append_json_u64(payload, first, "index_type", index_type);
  apitrace::metal::detail::append_json_u64(payload, first, "index_buffer_id", index_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index_buffer_offset", index_buffer_offset);
  apitrace::metal::detail::append_json_u64(payload, first, "instance_count", instance_count);
  apitrace::metal::detail::append_json_i64(payload, first, "base_vertex", base_vertex);
  apitrace::metal::detail::append_json_u64(payload, first, "base_instance", base_instance);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DrawIndexedPrimitives,
      "MTLRenderCommandEncoder.drawIndexedPrimitives",
      encoder_id,
      apitrace::metal::detail::object_refs({index_buffer_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_draw_primitives_indirect(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint64_t indirect_buffer_id, uint64_t indirect_buffer_offset)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(104);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "primitive_type", primitive_type);
  apitrace::metal::detail::append_json_u64(payload, first, "indirect_buffer_id", indirect_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "indirect_buffer_offset", indirect_buffer_offset);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DrawPrimitivesIndirect,
      "MTLRenderCommandEncoder.drawPrimitivesIndirect",
      encoder_id,
      apitrace::metal::detail::object_refs({indirect_buffer_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_draw_indexed_primitives_indirect(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint32_t index_type, uint64_t index_buffer_id, uint64_t index_buffer_offset, uint64_t indirect_buffer_id, uint64_t indirect_buffer_offset)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(176);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "primitive_type", primitive_type);
  apitrace::metal::detail::append_json_u64(payload, first, "index_type", index_type);
  apitrace::metal::detail::append_json_u64(payload, first, "index_buffer_id", index_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "index_buffer_offset", index_buffer_offset);
  apitrace::metal::detail::append_json_u64(payload, first, "indirect_buffer_id", indirect_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "indirect_buffer_offset", indirect_buffer_offset);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DrawIndexedPrimitivesIndirect,
      "MTLRenderCommandEncoder.drawIndexedPrimitivesIndirect",
      encoder_id,
      apitrace::metal::detail::object_refs({index_buffer_id, indirect_buffer_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_dispatch_threadgroups(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t tgx, uint32_t tgy, uint32_t tgz, uint32_t tx, uint32_t ty, uint32_t tz)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(112);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "tgx", tgx);
  apitrace::metal::detail::append_json_u64(payload, first, "tgy", tgy);
  apitrace::metal::detail::append_json_u64(payload, first, "tgz", tgz);
  apitrace::metal::detail::append_json_u64(payload, first, "tx", tx);
  apitrace::metal::detail::append_json_u64(payload, first, "ty", ty);
  apitrace::metal::detail::append_json_u64(payload, first, "tz", tz);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DispatchThreadgroups,
      "MTLComputeCommandEncoder.dispatchThreadgroups",
      encoder_id,
      {},
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_dispatch_threadgroups_indirect(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t indirect_buffer_id, uint64_t indirect_buffer_offset, uint32_t tx, uint32_t ty, uint32_t tz)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(128);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "indirect_buffer_id", indirect_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "indirect_buffer_offset", indirect_buffer_offset);
  apitrace::metal::detail::append_json_u64(payload, first, "tx", tx);
  apitrace::metal::detail::append_json_u64(payload, first, "ty", ty);
  apitrace::metal::detail::append_json_u64(payload, first, "tz", tz);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DispatchThreadgroupsIndirect,
      "MTLComputeCommandEncoder.dispatchThreadgroupsIndirect",
      encoder_id,
      apitrace::metal::detail::object_refs({indirect_buffer_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_blit_batch(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  auto payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    payload = json::object();
  }
  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::BlitBatch,
      "MTLBlitCommandEncoder.blitBatch",
      encoder_id,
      payload);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_blit_fence_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count)
{
  if (session == nullptr) {
    return;
  }

  if ((wait_fence_count != 0 && wait_fences == nullptr) ||
      (update_fence_count != 0 && update_fences == nullptr)) {
    return;
  }

  const auto payload = apitrace::metal::detail::blit_fence_batch_payload_json(
      wait_fences,
      wait_fence_count,
      update_fences,
      update_fence_count);

  auto trace_record = apitrace::metal::detail::make_metal_trace_record_raw_payload(
      apitrace::trace::MetalCallKind::BlitBatch,
      "MTLBlitCommandEncoder.blitBatch",
      encoder_id,
      {},
      {},
      payload.c_str());
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_copy_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t source_buffer_id, uint64_t source_offset, uint64_t destination_buffer_id, uint64_t destination_offset, uint64_t size)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(160);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "source_buffer_id", source_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "source_offset", source_offset);
  apitrace::metal::detail::append_json_u64(payload, first, "destination_buffer_id", destination_buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "destination_offset", destination_offset);
  apitrace::metal::detail::append_json_u64(payload, first, "size", size);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::CopyBuffer,
      "MTLBlitCommandEncoder.copyFromBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({source_buffer_id, destination_buffer_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_copy_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t source_buffer_id,
    uint64_t source_offset,
    uint64_t destination_buffer_id,
    uint64_t destination_offset,
    uint64_t size,
    const void *source_bytes,
    uint64_t source_bytes_size)
{
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto payload = json{{"source_buffer_id", source_buffer_id},
                      {"source_offset", source_offset},
                      {"destination_buffer_id", destination_buffer_id},
                      {"destination_offset", destination_offset},
                      {"size", size}};
  std::vector<apitrace::trace::BlobId> blob_refs;
  apitrace::metal::detail::attach_buffer_range_asset(
      session->state,
      payload,
      blob_refs,
      source_buffer_id,
      "metal-copy-buffer-source",
      source_bytes,
      source_bytes_size);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::CopyBuffer,
      "MTLBlitCommandEncoder.copyFromBuffer",
      encoder_id,
      {source_buffer_id, destination_buffer_id},
      std::move(blob_refs),
      payload);
}

APITRACE_METAL_API void apitrace_metal_copy_buffer_to_texture_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json,
    const void *source_bytes,
    uint64_t source_bytes_size)
{
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (!payload.is_object()) {
    payload = json::object();
  }
  const auto source_buffer_id = payload.value("source_buffer", 0ull);
  const auto destination_texture_id = payload.value("destination_texture", 0ull);
  std::vector<apitrace::trace::BlobId> blob_refs;
  apitrace::metal::detail::attach_buffer_range_asset(
      session->state,
      payload,
      blob_refs,
      source_buffer_id,
      "metal-copy-buffer-to-texture-source",
      source_bytes,
      source_bytes_size);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::CopyBufferToTexture,
      "MTLBlitCommandEncoder.copyFromBufferToTexture",
      encoder_id,
      {source_buffer_id, destination_texture_id},
      std::move(blob_refs),
      payload);
}

APITRACE_METAL_API void apitrace_metal_copy_buffer_to_texture(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t source_buffer_id,
    uint64_t destination_texture_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::CopyBufferToTexture,
      "MTLBlitCommandEncoder.copyFromBufferToTexture",
      encoder_id,
      apitrace::metal::detail::object_refs({source_buffer_id, destination_texture_id}),
      apitrace::metal::detail::copy_c_string(payload_json));
}

APITRACE_METAL_API void apitrace_metal_copy_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t source_texture_id, uint64_t destination_texture_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::CopyTexture,
      "MTLBlitCommandEncoder.copyFromTexture",
      encoder_id,
      apitrace::metal::detail::object_refs({source_texture_id, destination_texture_id}),
      {},
      json{{"source_texture_id", source_texture_id},
           {"destination_texture_id", destination_texture_id},
           {"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_blit_fill(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t buffer_object_id, uint64_t range_start, uint64_t range_length, uint32_t value)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(112);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "buffer_id", buffer_object_id);
  apitrace::metal::detail::append_json_u64(payload, first, "range_start", range_start);
  apitrace::metal::detail::append_json_u64(payload, first, "range_length", range_length);
  apitrace::metal::detail::append_json_u64(payload, first, "value", value);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::BlitFill,
      "MTLBlitCommandEncoder.fillBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_object_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_use_resource(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t resource_id, uint32_t usage, uint32_t stages)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(80);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "resource_id", resource_id);
  apitrace::metal::detail::append_json_u64(payload, first, "usage", usage);
  apitrace::metal::detail::append_json_u64(payload, first, "stages", stages);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::UseResource,
      "MTLRenderCommandEncoder.useResource",
      encoder_id,
      apitrace::metal::detail::object_refs({resource_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_use_resources(apitrace_metal_session_t *session, uint64_t encoder_id, const uint64_t *resource_ids, uint32_t count, uint32_t usage, uint32_t stages)
{
  json resources = json::array();
  for (uint32_t index = 0; index < count; ++index) {
    resources.push_back(resource_ids[index]);
  }
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.useResources",
      apitrace::trace::MetalCallKind::UseResources,
      encoder_id,
      json{{"resource_ids", std::move(resources)}, {"usage", usage}, {"stages", stages}});
}

APITRACE_METAL_API void apitrace_metal_use_heap(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t heap_id)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(40);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "heap_id", heap_id);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::UseHeap,
      "MTLCommandEncoder.useHeap",
      encoder_id,
      apitrace::metal::detail::object_refs({heap_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_set_argument_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t index, uint64_t buffer_id, uint64_t offset)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(80);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "index", index);
  apitrace::metal::detail::append_json_u64(payload, first, "buffer_id", buffer_id);
  apitrace::metal::detail::append_json_u64(payload, first, "offset", offset);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::SetArgumentBuffer,
      "MTLCommandEncoder.setArgumentBuffer",
      encoder_id,
      apitrace::metal::detail::object_refs({buffer_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_memory_barrier(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.memoryBarrier",
      apitrace::trace::MetalCallKind::MemoryBarrier,
      encoder_id,
      json{{"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_encoder_state(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  auto payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    payload = json::object();
  }
  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::EncoderState,
      "MTLCommandEncoder.encoderState",
      encoder_id,
      payload);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_set_compute_bytes(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  auto payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    payload = json::object();
  }
  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::SetComputeBytes,
      "MTLComputeCommandEncoder.setBytes",
      encoder_id,
      payload);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_dispatch_threads(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  auto payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    payload = json::object();
  }
  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::DispatchThreads,
      "MTLComputeCommandEncoder.dispatchThreads",
      encoder_id,
      payload);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_dispatch_threads_per_tile(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t width, uint32_t height)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(48);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "width", width);
  apitrace::metal::detail::append_json_u64(payload, first, "height", height);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::DispatchThreadsPerTile,
      "MTLRenderCommandEncoder.dispatchThreadsPerTile",
      encoder_id,
      {},
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_record_fence_ops(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  json fence_payload = json::parse(apitrace::metal::detail::copy_c_string(payload_json), nullptr, false);
  if (fence_payload.is_discarded() || !fence_payload.is_object()) {
    fence_payload = json::object();
  }
  auto trace_record = apitrace::metal::detail::make_metal_trace_record(
      apitrace::trace::MetalCallKind::FenceOps,
      "MTLCommandEncoder.fenceOps",
      encoder_id,
      fence_payload);
  apitrace::metal::detail::submit_metal_trace_record(session->state, std::move(trace_record));
}

APITRACE_METAL_API void apitrace_metal_update_fence(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t fence_id, uint32_t stages)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(56);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "fence_id", fence_id);
  apitrace::metal::detail::append_json_u64(payload, first, "stages", stages);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::UpdateFence,
      "MTLCommandEncoder.updateFence",
      encoder_id,
      apitrace::metal::detail::object_refs({fence_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_wait_for_fence(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t fence_id, uint32_t stages)
{
  if (session == nullptr) {
    return;
  }
  auto payload = apitrace::metal::detail::begin_json_object(56);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "fence_id", fence_id);
  apitrace::metal::detail::append_json_u64(payload, first, "stages", stages);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::WaitForFence,
      "MTLCommandEncoder.waitForFence",
      encoder_id,
      apitrace::metal::detail::object_refs({fence_id}),
      apitrace::metal::detail::finish_json_object(std::move(payload)));
}

APITRACE_METAL_API void apitrace_metal_present_drawable(apitrace_metal_session_t *session, uint64_t command_buffer_id, uint64_t drawable_id, uint64_t texture_id, uint64_t frame_index, uint32_t width, uint32_t height, uint32_t sync_interval, uint32_t flags)
{
  if (session == nullptr) {
    return;
  }

  auto payload = apitrace::metal::detail::begin_json_object(160);
  bool first = true;
  apitrace::metal::detail::append_json_u64(payload, first, "drawable_handle", drawable_id);
  apitrace::metal::detail::append_json_u64(payload, first, "present_texture_id", texture_id);
  apitrace::metal::detail::append_json_u64(payload, first, "frame_index", frame_index);
  apitrace::metal::detail::append_json_u64(payload, first, "width", width);
  apitrace::metal::detail::append_json_u64(payload, first, "height", height);
  apitrace::metal::detail::append_json_u64(payload, first, "sync_interval", sync_interval);
  apitrace::metal::detail::append_json_u64(payload, first, "flags", flags);
  apitrace::metal::detail::record_metal_call_raw_payload(
      session->state,
      apitrace::trace::MetalCallKind::PresentDrawable,
      "MTLCommandBuffer.presentDrawable",
      command_buffer_id,
      {texture_id},
      apitrace::metal::detail::finish_json_object(std::move(payload)),
      frame_index);
}

APITRACE_METAL_API void apitrace_metal_push_debug_group(apitrace_metal_session_t *session, uint64_t encoder_id, const char *label_utf8)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.pushDebugGroup",
      apitrace::trace::MetalCallKind::PushDebugGroup,
      encoder_id,
      json{{"label", apitrace::metal::detail::copy_c_string(label_utf8)}});
}

APITRACE_METAL_API void apitrace_metal_pop_debug_group(apitrace_metal_session_t *session, uint64_t encoder_id)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.popDebugGroup",
      apitrace::trace::MetalCallKind::PopDebugGroup,
      encoder_id,
      json::object());
}

APITRACE_METAL_API void apitrace_metal_object_metadata(apitrace_metal_session_t *session, uint64_t object_id, const char *payload_json)
{
  if (session == nullptr) {
    return;
  }
  auto payload_text = apitrace::metal::detail::copy_c_string(payload_json);
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto [metadata_it, inserted] = session->state.object_metadata_payloads.try_emplace(object_id, payload_text);
  if (!inserted && metadata_it->second == payload_text) {
    return;
  }
  if (!inserted) {
    metadata_it->second = payload_text;
  }
  auto payload = json::parse(payload_text, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    payload = json::object();
  }
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::ObjectMetadata,
      "MTLObject.metadata",
      object_id,
      payload);
}

APITRACE_METAL_API void apitrace_metal_insert_debug_signpost(apitrace_metal_session_t *session, uint64_t encoder_id, const char *label_utf8)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.insertDebugSignpost",
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      encoder_id,
      json{{"label", apitrace::metal::detail::copy_c_string(label_utf8)}});
}

APITRACE_METAL_API uint64_t apitrace_metal_register_buffer(apitrace_metal_session_t *session, uint64_t object_id, uint64_t length, uint32_t storage_mode, const void *initial_bytes, uint64_t initial_bytes_size)
{
  if (session == nullptr) {
    return 0;
  }
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto asset = apitrace::metal::detail::make_asset_record(object_id, "buffer", initial_bytes, initial_bytes_size);
  asset.kind = apitrace::trace::AssetKind::Buffer;
  asset = session->state.writer.register_asset(std::move(asset));
  const auto payload = json{{"length", length},
                            {"storage_mode", storage_mode},
                            {"initial_bytes_size", initial_bytes_size},
                            {"buffer_path", asset.relative_path.generic_string()}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Buffer,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newBuffer",
      object_id,
      {},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_texture(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  const auto payload = json{{"descriptor", apitrace::metal::detail::copy_c_string(descriptor_json)}};
  apitrace::trace::AssetRecord asset;
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Texture,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newTexture",
      object_id,
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_drawable_texture(apitrace_metal_session_t *session, uint64_t object_id, uint64_t drawable_id, const char *descriptor_json)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  const auto payload = json{{"descriptor", apitrace::metal::detail::copy_c_string(descriptor_json)},
                            {"drawable_handle", drawable_id},
                            {"external", true}};
  apitrace::trace::AssetRecord asset;
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Texture,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "CAMetalDrawable.texture",
      object_id,
      {},
      {},
      payload);
}

APITRACE_METAL_API void apitrace_metal_update_buffer_contents(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    uint64_t offset,
    uint64_t length,
    uint32_t storage_mode,
    const void *bytes,
    uint64_t bytes_size)
{
  if (session == nullptr) {
    return;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto payload = json{{"buffer_id", object_id},
                      {"offset", offset},
                      {"length", length},
                      {"storage_mode", storage_mode}};
  std::vector<apitrace::trace::BlobId> blob_refs;
  apitrace::metal::detail::attach_buffer_range_asset(
      session->state,
      payload,
      blob_refs,
      object_id,
      "metal-buffer-update",
      bytes,
      bytes_size);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::BufferUpdate,
      "MTLBuffer.updateContents",
      object_id,
      apitrace::metal::detail::object_refs({object_id}),
      std::move(blob_refs),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_library(apitrace_metal_session_t *session, uint64_t object_id, const void *metallib_bytes, uint64_t size)
{
  if (session == nullptr) {
    return 0;
  }
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  auto asset = apitrace::metal::detail::make_asset_record(object_id, "library", metallib_bytes, size);
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::Library, std::move(asset));
  const auto payload = json{{"library_path", asset.relative_path.generic_string()}, {"size", size}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Library,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newLibrary",
      object_id,
      {},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_render_pipeline(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json, uint64_t vs_function_id, uint64_t fs_function_id)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  const auto descriptor = apitrace::metal::detail::copy_c_string(descriptor_json);
  auto asset = apitrace::metal::detail::make_asset_record(
      object_id,
      "render_pipeline",
      descriptor.data(),
      static_cast<std::uint64_t>(descriptor.size()));
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::RenderPipeline, std::move(asset));
  const auto payload = json{{"descriptor_path", asset.relative_path.generic_string()},
                            {"vs_function_id", vs_function_id},
                            {"fs_function_id", fs_function_id}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newRenderPipelineState",
      object_id,
      {vs_function_id, fs_function_id},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_mesh_render_pipeline(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json, uint64_t object_function_id, uint64_t mesh_function_id, uint64_t fragment_function_id)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  const auto descriptor = apitrace::metal::detail::copy_c_string(descriptor_json);
  auto asset = apitrace::metal::detail::make_asset_record(
      object_id,
      "mesh_render_pipeline",
      descriptor.data(),
      static_cast<std::uint64_t>(descriptor.size()));
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::RenderPipeline, std::move(asset));
  const auto payload = json{{"descriptor_path", asset.relative_path.generic_string()},
                            {"object_function_id", object_function_id},
                            {"mesh_function_id", mesh_function_id},
                            {"fragment_function_id", fragment_function_id}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newMeshRenderPipelineState",
      object_id,
      {object_function_id, mesh_function_id, fragment_function_id},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_tile_render_pipeline(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json, uint64_t tile_function_id)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  const auto descriptor = apitrace::metal::detail::copy_c_string(descriptor_json);
  auto asset = apitrace::metal::detail::make_asset_record(
      object_id,
      "tile_render_pipeline",
      descriptor.data(),
      static_cast<std::uint64_t>(descriptor.size()));
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::RenderPipeline, std::move(asset));
  const auto payload = json{{"descriptor_path", asset.relative_path.generic_string()},
                            {"tile_function_id", tile_function_id}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newTileRenderPipelineState",
      object_id,
      {tile_function_id},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_compute_pipeline(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json, uint64_t cs_function_id)
{
  if (session == nullptr) {
    return 0;
  }

  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  const auto descriptor = apitrace::metal::detail::copy_c_string(descriptor_json);
  auto asset = apitrace::metal::detail::make_asset_record(
      object_id,
      "compute_pipeline",
      descriptor.data(),
      static_cast<std::uint64_t>(descriptor.size()));
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::ComputePipeline, std::move(asset));
  const auto payload = json{{"descriptor_path", asset.relative_path.generic_string()}, {"cs_function_id", cs_function_id}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::ComputePipeline,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newComputePipelineState",
      object_id,
      {cs_function_id},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API void apitrace_metal_emit_link(
    apitrace_metal_session_t *session,
    apitrace_metal_scope_kind scope,
    uint64_t d3d_sequence,
    uint64_t metal_sequence_begin,
    uint64_t metal_sequence_end,
    const char *payload_utf8)
{
  if (session == nullptr) {
    return;
  }
  apitrace::metal::detail::TimedSessionLock lock(session->state.mutex);
  apitrace::metal::detail::append_link(
      session->state,
      scope,
      d3d_sequence,
      metal_sequence_begin,
      metal_sequence_end,
      0,
      payload_utf8);
}
