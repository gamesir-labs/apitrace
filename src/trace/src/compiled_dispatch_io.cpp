#include "apitrace/compiled_dispatch_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <ostream>
#include <type_traits>

namespace apitrace::trace {
namespace {

using json = nlohmann::json;

constexpr std::array<std::uint8_t, 8> kMagic = {
    'A', 'P', 'I', 'D', 'S', 'P', '6', '\0'};
constexpr std::uint32_t kHeaderBytes = kCompiledDispatchHeaderBytes;
constexpr std::uint32_t kRecordFixedBytes = 72;
constexpr std::uint32_t kCompiledNodeMaxDepth = 64;

enum class CompiledNodeTag : std::uint8_t {
  Null = 0,
  False = 1,
  True = 2,
  Unsigned = 3,
  Signed = 4,
  Float64 = 5,
  String = 6,
  Array = 7,
  Object = 8,
};

std::uint64_t checksum64_mix(std::uint64_t value)
{
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

std::uint64_t checksum64_update(
    std::uint64_t hash,
    const std::uint8_t *data,
    std::size_t size)
{
  while (size >= sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, data, sizeof(word));
    hash ^= checksum64_mix(word + 0x9e3779b97f4a7c15ull);
    hash = (hash << 27u) | (hash >> 37u);
    hash = hash * 5u + 0x52dce729ull;
    data += sizeof(word);
    size -= sizeof(word);
  }
  if (size != 0) {
    std::uint64_t tail = 0;
    std::memcpy(&tail, data, size);
    hash ^= checksum64_mix(tail ^ (static_cast<std::uint64_t>(size) << 56u));
  }
  return hash;
}

std::uint64_t record_checksum64(
    const std::uint8_t *size_bytes,
    std::size_t size_byte_count,
    const std::uint8_t *body,
    std::size_t body_size)
{
  auto hash = checksum64_update(
      0x243f6a8885a308d3ull ^ static_cast<std::uint64_t>(body_size),
      size_bytes,
      size_byte_count);
  hash = checksum64_update(hash, body, body_size);
  return checksum64_mix(hash ^ 0x13198a2e03707344ull);
}

bool valid_sha256(std::string_view digest)
{
  return digest.size() == 64 &&
         std::all_of(digest.begin(), digest.end(), [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool contains(std::string_view text, std::string_view needle)
{
  return text.find(needle) != std::string_view::npos;
}

CompiledCommandKind compile_command_kind(std::string_view function)
{
  if (function == "ID3D12Device::CreateCommandList") {
    return CompiledCommandKind::BeginCommandList;
  }
  if (function.rfind("ID3D12GraphicsCommandList", 0) != 0) {
    if (function == "ID3D12Resource::Map") {
      return CompiledCommandKind::MapResource;
    }
    if (function == "ID3D12Resource::Unmap") {
      return CompiledCommandKind::UnmapResource;
    }
    return CompiledCommandKind::Unknown;
  }
  const auto separator = function.find("::");
  if (separator == std::string_view::npos) {
    return CompiledCommandKind::Unknown;
  }
  const auto method = function.substr(separator + 2);
  if (method == "Reset") return CompiledCommandKind::BeginCommandList;
  if (method == "Close") return CompiledCommandKind::EndCommandList;
  if (method == "SetPipelineState") return CompiledCommandKind::SetPipelineState;
  if (method == "SetGraphicsRootSignature" || method == "SetComputeRootSignature")
    return CompiledCommandKind::SetRootSignature;
  if (method == "SetDescriptorHeaps") return CompiledCommandKind::SetDescriptorHeaps;
  if (method == "SetGraphicsRootDescriptorTable" ||
      method == "SetComputeRootDescriptorTable")
    return CompiledCommandKind::SetRootDescriptorTable;
  if (method == "SetGraphicsRoot32BitConstant" ||
      method == "SetComputeRoot32BitConstant" ||
      method == "SetGraphicsRoot32BitConstants" ||
      method == "SetComputeRoot32BitConstants")
    return CompiledCommandKind::SetRootConstants;
  if (method == "SetGraphicsRootConstantBufferView" ||
      method == "SetComputeRootConstantBufferView" ||
      method == "SetGraphicsRootShaderResourceView" ||
      method == "SetComputeRootShaderResourceView" ||
      method == "SetGraphicsRootUnorderedAccessView" ||
      method == "SetComputeRootUnorderedAccessView")
    return CompiledCommandKind::SetRootConstantBufferView;
  if (method == "RSSetViewports") return CompiledCommandKind::SetViewports;
  if (method == "RSSetScissorRects") return CompiledCommandKind::SetScissorRects;
  if (method == "OMSetRenderTargets") return CompiledCommandKind::SetRenderTargets;
  if (method == "ClearRenderTargetView") return CompiledCommandKind::ClearRenderTarget;
  if (method == "ClearDepthStencilView") return CompiledCommandKind::ClearDepthStencil;
  if (method == "ClearUnorderedAccessViewUint" || method == "ClearUnorderedAccessViewFloat")
    return CompiledCommandKind::ClearUnorderedAccess;
  if (method == "DiscardResource") return CompiledCommandKind::DiscardResource;
  if (method == "IASetPrimitiveTopology") return CompiledCommandKind::SetPrimitiveTopology;
  if (method == "IASetVertexBuffers") return CompiledCommandKind::SetVertexBuffers;
  if (method == "IASetIndexBuffer") return CompiledCommandKind::SetIndexBuffer;
  if (method == "ResourceBarrier" || method == "ResourceBarrierBatch")
    return CompiledCommandKind::ResourceBarrier;
  if (method == "ClearState" || method == "OMSetBlendFactor" || method == "OMSetStencilRef")
    return CompiledCommandKind::SetDynamicState;
  if (method == "BeginRenderPass" || method == "EndRenderPass")
    return CompiledCommandKind::RenderPass;
  if (method == "BeginQuery" || method == "EndQuery" || method == "ResolveQueryData")
    return CompiledCommandKind::Query;
  if (method == "SetPredication") return CompiledCommandKind::Predication;
  if (method == "WriteBufferImmediate") return CompiledCommandKind::WriteBufferImmediate;
  if (method == "TemporalUpscale") return CompiledCommandKind::TemporalUpscale;
  if (method == "DrawInstanced" || method == "DrawIndexedInstanced")
    return CompiledCommandKind::Draw;
  if (method == "Dispatch" || method == "DispatchRays" || method == "DispatchMesh")
    return CompiledCommandKind::Dispatch;
  if (method == "ExecuteIndirect") return CompiledCommandKind::ExecuteIndirect;
  if (method == "ExecuteBundle") return CompiledCommandKind::ExecuteBundle;
  if (method == "CopyBufferRegion" || method == "CopyBufferRegionBatch" ||
      method == "CopyTextureRegion" || method == "CopyTextureRegionBatch" ||
      method == "CopyResource")
    return CompiledCommandKind::Copy;
  if (method == "ResolveSubresource" || method == "ResolveSubresourceRegion")
    return CompiledCommandKind::Resolve;
  return CompiledCommandKind::Unknown;
}

bool is_record_command(std::string_view function)
{
  if (function == "ID3D12CommandAllocator::Reset") {
    return true;
  }
  const auto kind = compile_command_kind(function);
  return kind != CompiledCommandKind::Unknown &&
         kind != CompiledCommandKind::MapResource &&
         kind != CompiledCommandKind::UnmapResource;
}

CompiledDispatchRoute compile_dispatch_route(const EventRecord &event, const json &payload)
{
  const std::string_view function(event.callsite.function_name);
  if (event.kind == EventKind::Call && event.object_refs.empty() && event.blob_refs.empty() &&
      function.rfind("ID3D12", 0) == 0 && payload.empty()) {
    return CompiledDispatchRoute::NoOp;
  }
  if (event.kind == EventKind::ObjectCreate) {
    return CompiledDispatchRoute::NoOp;
  }
  if (event.kind == EventKind::ObjectDestroy) {
    return CompiledDispatchRoute::ObjectDestroy;
  }
  if (function == "D3D12CreateDevice") {
    return CompiledDispatchRoute::CreateDevice;
  }
  if (function.rfind("DXMT::", 0) == 0) {
    return CompiledDispatchRoute::NoOp;
  }
  if (event.kind == EventKind::Boundary) {
    return event.boundary == BoundaryKind::Present
               ? CompiledDispatchRoute::PresentBoundary
               : CompiledDispatchRoute::NoOp;
  }
  if (function == "IDXGISwapChain::Present") {
    return CompiledDispatchRoute::PresentCall;
  }
  if (contains(function, "CreateHeap") || contains(function, "OpenExistingHeap") ||
      contains(function, "CreateQueryHeap")) {
    return CompiledDispatchRoute::CreateMemory;
  }
  if (contains(function, "CreateCommittedResource") ||
      contains(function, "CreatePlacedResource") ||
      contains(function, "CreateReservedResource")) {
    return CompiledDispatchRoute::CreateResource;
  }
  if (contains(function, "CreateDescriptorHeap") ||
      function == "ID3D12Device::CreateDescriptorViewBatch" ||
      function == "ID3D12Device::CreateConstantBufferView" ||
      function == "ID3D12Device::CreateShaderResourceView" ||
      function == "ID3D12Device::CreateUnorderedAccessView" ||
      function == "ID3D12Device::CreateRenderTargetView" ||
      function == "ID3D12Device::CreateDepthStencilView" ||
      function == "ID3D12Device::CreateSampler") {
    return CompiledDispatchRoute::CreateDescriptor;
  }
  if (function == "ID3D12Device::CreateGraphicsPipelineState" ||
      function == "ID3D12Device::CreateComputePipelineState" ||
      function == "ID3D12Device2::CreatePipelineState" ||
      contains(function, "CreateRootSignature") ||
      contains(function, "CreateCommandSignature")) {
    return CompiledDispatchRoute::CreatePipeline;
  }
  if (contains(function, "CreateCommandQueue") ||
      contains(function, "CreateCommandAllocator") ||
      contains(function, "CreateCommandList") || contains(function, "CreateFence")) {
    return CompiledDispatchRoute::CreateCommandObject;
  }
  if (is_record_command(function)) {
    return CompiledDispatchRoute::RecordCommand;
  }
  if (contains(function, "CopyDescriptors")) {
    return CompiledDispatchRoute::CopyDescriptors;
  }
  if (function == "ID3D12CommandQueue::ExecuteCommandLists") {
    return CompiledDispatchRoute::ExecuteCommandLists;
  }
  if (function == "ID3D12CommandQueue::UpdateTileMappings") {
    return CompiledDispatchRoute::UpdateTileMappings;
  }
  if (function == "ID3D12CommandQueue::Signal") {
    return CompiledDispatchRoute::QueueSignal;
  }
  if (function == "ID3D12CommandQueue::Wait") {
    return CompiledDispatchRoute::QueueWait;
  }
  if (function == "ID3D12Fence::Signal") {
    return CompiledDispatchRoute::FenceSignal;
  }
  if (function == "ID3D12Fence::SetEventOnCompletion") {
    return CompiledDispatchRoute::FenceSetEventOnCompletion;
  }
  if (function == "ID3D12Resource::Map") {
    return CompiledDispatchRoute::ResourceMap;
  }
  if (function == "ID3D12Resource::Unmap") {
    return CompiledDispatchRoute::ResourceUnmap;
  }
  if (function == "apitrace::D3D12ResourceDataUpdate") {
    return CompiledDispatchRoute::ResourceDataUpdate;
  }
  return CompiledDispatchRoute::DiagnosticStub;
}

template <typename T>
void append_le(std::vector<std::uint8_t> &output, T value)
{
  using U = std::make_unsigned_t<T>;
  const U bits = static_cast<U>(value);
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    output.push_back(static_cast<std::uint8_t>((bits >> (index * 8u)) & 0xffu));
  }
}

template <typename T>
bool read_le(const std::uint8_t *&cursor, const std::uint8_t *end, T &value)
{
  if (static_cast<std::size_t>(end - cursor) < sizeof(T)) {
    return false;
  }
  using U = std::make_unsigned_t<T>;
  U bits = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bits |= static_cast<U>(cursor[index]) << (index * 8u);
  }
  cursor += sizeof(T);
  value = static_cast<T>(bits);
  return true;
}

template <typename T>
bool write_le(std::ostream &output, T value)
{
  std::array<std::uint8_t, sizeof(T)> bytes{};
  using U = std::make_unsigned_t<T>;
  const U bits = static_cast<U>(value);
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes[index] = static_cast<std::uint8_t>((bits >> (index * 8u)) & 0xffu);
  }
  output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  return static_cast<bool>(output);
}

bool read_exact(std::istream &input, void *data, std::size_t size)
{
  input.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
  return input.gcount() == static_cast<std::streamsize>(size);
}

bool checked_u32(std::size_t value, const char *label, std::uint32_t &result, std::string &error)
{
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    error = std::string("compiled dispatch ") + label + " exceeds uint32 range";
    return false;
  }
  result = static_cast<std::uint32_t>(value);
  return true;
}

bool json_u32(const json &value, std::uint32_t &result)
{
  std::uint64_t parsed = 0;
  if (value.is_number_unsigned()) {
    parsed = value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
      return false;
    }
    parsed = static_cast<std::uint64_t>(signed_value);
  } else {
    return false;
  }
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  result = static_cast<std::uint32_t>(parsed);
  return true;
}

bool json_u64(const json &value, std::uint64_t &result)
{
  if (value.is_number_unsigned()) {
    result = value.get<std::uint64_t>();
    return true;
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value >= 0) {
      result = static_cast<std::uint64_t>(signed_value);
      return true;
    }
  }
  return false;
}

bool encode_compiled_node(
    const json &node,
    std::vector<std::uint8_t> &encoded,
    std::uint32_t depth,
    std::string &error)
{
  if (depth > kCompiledNodeMaxDepth) {
    error = "compiled payload exceeds the maximum node depth";
    return false;
  }
  if (node.is_null()) {
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::Null));
    return true;
  }
  if (node.is_boolean()) {
    encoded.push_back(static_cast<std::uint8_t>(
        node.get<bool>() ? CompiledNodeTag::True : CompiledNodeTag::False));
    return true;
  }
  if (node.is_number_unsigned()) {
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::Unsigned));
    append_le(encoded, node.get<std::uint64_t>());
    return true;
  }
  if (node.is_number_integer()) {
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::Signed));
    append_le(encoded, node.get<std::int64_t>());
    return true;
  }
  if (node.is_number_float()) {
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::Float64));
    const auto value = node.get<double>();
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    append_le(encoded, bits);
    return true;
  }
  if (node.is_string()) {
    const auto &value = node.get_ref<const std::string &>();
    std::uint32_t bytes = 0;
    if (!checked_u32(value.size(), "node string", bytes, error)) {
      return false;
    }
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::String));
    append_le(encoded, bytes);
    encoded.insert(encoded.end(), value.begin(), value.end());
    return true;
  }
  if (node.is_array()) {
    std::uint32_t count = 0;
    if (!checked_u32(node.size(), "node array", count, error)) {
      return false;
    }
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::Array));
    append_le(encoded, count);
    for (const auto &element : node) {
      if (!encode_compiled_node(element, encoded, depth + 1, error)) {
        return false;
      }
    }
    return true;
  }
  if (node.is_object()) {
    std::uint32_t count = 0;
    if (!checked_u32(node.size(), "node object", count, error)) {
      return false;
    }
    encoded.push_back(static_cast<std::uint8_t>(CompiledNodeTag::Object));
    append_le(encoded, count);
    for (auto it = node.begin(); it != node.end(); ++it) {
      std::uint32_t key_bytes = 0;
      if (!checked_u32(it.key().size(), "node key", key_bytes, error)) {
        return false;
      }
      append_le(encoded, key_bytes);
      encoded.insert(encoded.end(), it.key().begin(), it.key().end());
      if (!encode_compiled_node(it.value(), encoded, depth + 1, error)) {
        return false;
      }
    }
    return true;
  }
  error = "compiled payload contains an unsupported JSON node";
  return false;
}

bool encode_compiled_payload_nodes(
    const json &payload,
    std::vector<std::uint8_t> &encoded,
    std::string &error)
{
  encoded.clear();
  if (!payload.is_object()) {
    error = "compiled payload root must be an object";
    return false;
  }
  return encode_compiled_node(payload, encoded, 0, error);
}

bool decode_compiled_node(
    const std::uint8_t *&cursor,
    const std::uint8_t *end,
    json &node,
    std::uint32_t depth,
    std::string &error)
{
  if (depth > kCompiledNodeMaxDepth || cursor == end) {
    error = depth > kCompiledNodeMaxDepth
                ? "compiled payload exceeds the maximum node depth"
                : "compiled payload node is truncated";
    return false;
  }
  const auto raw_tag = *cursor++;
  if (raw_tag > static_cast<std::uint8_t>(CompiledNodeTag::Object)) {
    error = "compiled payload has an unknown node tag";
    return false;
  }
  const auto tag = static_cast<CompiledNodeTag>(raw_tag);
  switch (tag) {
  case CompiledNodeTag::Null:
    node = nullptr;
    return true;
  case CompiledNodeTag::False:
    node = false;
    return true;
  case CompiledNodeTag::True:
    node = true;
    return true;
  case CompiledNodeTag::Unsigned: {
    std::uint64_t value = 0;
    if (!read_le(cursor, end, value)) {
      error = "compiled unsigned node is truncated";
      return false;
    }
    node = value;
    return true;
  }
  case CompiledNodeTag::Signed: {
    std::int64_t value = 0;
    if (!read_le(cursor, end, value)) {
      error = "compiled signed node is truncated";
      return false;
    }
    node = value;
    return true;
  }
  case CompiledNodeTag::Float64: {
    std::uint64_t bits = 0;
    if (!read_le(cursor, end, bits)) {
      error = "compiled float node is truncated";
      return false;
    }
    double value = 0.0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    node = value;
    return true;
  }
  case CompiledNodeTag::String: {
    std::uint32_t bytes = 0;
    if (!read_le(cursor, end, bytes) || static_cast<std::size_t>(end - cursor) < bytes) {
      error = "compiled string node is truncated";
      return false;
    }
    node = std::string(reinterpret_cast<const char *>(cursor), bytes);
    cursor += bytes;
    return true;
  }
  case CompiledNodeTag::Array: {
    std::uint32_t count = 0;
    if (!read_le(cursor, end, count) || count > static_cast<std::uint32_t>(end - cursor)) {
      error = "compiled array node has an invalid element count";
      return false;
    }
    node = json::array();
    auto &array = node.get_ref<json::array_t &>();
    array.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      json element;
      if (!decode_compiled_node(cursor, end, element, depth + 1, error)) {
        return false;
      }
      array.push_back(std::move(element));
    }
    return true;
  }
  case CompiledNodeTag::Object: {
    std::uint32_t count = 0;
    if (!read_le(cursor, end, count) ||
        count > static_cast<std::uint32_t>((end - cursor) / 5)) {
      error = "compiled object node has an invalid member count";
      return false;
    }
    node = json::object();
    auto &object = node.get_ref<json::object_t &>();
    for (std::uint32_t index = 0; index < count; ++index) {
      std::uint32_t key_bytes = 0;
      if (!read_le(cursor, end, key_bytes) ||
          static_cast<std::size_t>(end - cursor) < key_bytes) {
        error = "compiled object key is truncated";
        return false;
      }
      std::string key(reinterpret_cast<const char *>(cursor), key_bytes);
      cursor += key_bytes;
      json value;
      if (!decode_compiled_node(cursor, end, value, depth + 1, error)) {
        return false;
      }
      if (!object.emplace(std::move(key), std::move(value)).second) {
        error = "compiled object contains a duplicate key";
        return false;
      }
    }
    return true;
  }
  }
  error = "compiled payload node is invalid";
  return false;
}

bool json_object_u32(
    const json &object,
    const char *key,
    std::uint32_t &result,
    std::string &error)
{
  const auto it = object.find(key);
  if (it == object.end() || !json_u32(*it, result)) {
    error = std::string("UpdateTileMappings field '") + key + "' must be a uint32";
    return false;
  }
  return true;
}

bool encode_compiled_tile_mappings(
    const json &payload,
    std::vector<std::uint8_t> &encoded,
    std::string &error)
{
  std::uint32_t flags = 0;
  std::uint32_t region_count = 0;
  std::uint32_t range_count = 0;
  if (!json_object_u32(payload, "flags", flags, error) ||
      !json_object_u32(payload, "region_count", region_count, error) ||
      !json_object_u32(payload, "range_count", range_count, error)) {
    return false;
  }
  const auto regions = payload.find("regions");
  const auto region_sizes = payload.find("region_sizes");
  const auto range_flags = payload.find("range_flags");
  const auto heap_offsets = payload.find("heap_range_offsets");
  const auto tile_counts = payload.find("range_tile_counts");
  if (regions == payload.end() || !regions->is_array() || regions->size() != region_count ||
      region_sizes == payload.end() || !region_sizes->is_array() ||
      region_sizes->size() != region_count ||
      range_flags == payload.end() || !range_flags->is_array() ||
      range_flags->size() != range_count ||
      heap_offsets == payload.end() || !heap_offsets->is_array() ||
      heap_offsets->size() != range_count ||
      tile_counts == payload.end() || !tile_counts->is_array() ||
      tile_counts->size() != range_count) {
    error = "UpdateTileMappings array sizes do not match region_count/range_count";
    return false;
  }

  const std::uint64_t encoded_size =
      3u * sizeof(std::uint32_t) +
      static_cast<std::uint64_t>(region_count) * 9u * sizeof(std::uint32_t) +
      static_cast<std::uint64_t>(range_count) * 3u * sizeof(std::uint32_t);
  if (encoded_size > std::numeric_limits<std::uint32_t>::max()) {
    error = "UpdateTileMappings compiled payload exceeds uint32 range";
    return false;
  }
  encoded.clear();
  encoded.reserve(static_cast<std::size_t>(encoded_size));
  append_le(encoded, flags);
  append_le(encoded, region_count);
  append_le(encoded, range_count);
  for (const auto &region : *regions) {
    if (!region.is_object()) {
      error = "UpdateTileMappings region must be an object";
      return false;
    }
    for (const char *key : {"subresource", "x", "y", "z"}) {
      std::uint32_t value = 0;
      if (!json_object_u32(region, key, value, error)) {
        return false;
      }
      append_le(encoded, value);
    }
  }
  for (const auto &size : *region_sizes) {
    if (!size.is_object()) {
      error = "UpdateTileMappings region size must be an object";
      return false;
    }
    std::uint32_t num_tiles = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    const auto use_box = size.find("use_box");
    if (!json_object_u32(size, "num_tiles", num_tiles, error) ||
        use_box == size.end() || !use_box->is_boolean() ||
        !json_object_u32(size, "width", width, error) ||
        !json_object_u32(size, "height", height, error) ||
        !json_object_u32(size, "depth", depth, error)) {
      if (error.empty()) {
        error = "UpdateTileMappings field 'use_box' must be boolean";
      }
      return false;
    }
    append_le(encoded, num_tiles);
    append_le(encoded, static_cast<std::uint32_t>(use_box->get<bool>()));
    append_le(encoded, width);
    append_le(encoded, height);
    append_le(encoded, depth);
  }
  for (const auto *array : {&*range_flags, &*heap_offsets, &*tile_counts}) {
    for (const auto &element : *array) {
      std::uint32_t value = 0;
      if (!json_u32(element, value)) {
        error = "UpdateTileMappings range arrays must contain uint32 values";
        return false;
      }
      append_le(encoded, value);
    }
  }
  return encoded.size() == encoded_size;
}

bool decode_compiled_tile_mappings(
    const std::uint8_t *cursor,
    const std::uint8_t *end,
    CompiledTileMappingPayload &payload,
    std::string &error)
{
  std::uint32_t region_count = 0;
  std::uint32_t range_count = 0;
  if (!read_le(cursor, end, payload.flags) ||
      !read_le(cursor, end, region_count) ||
      !read_le(cursor, end, range_count)) {
    error = "compiled UpdateTileMappings payload header is truncated";
    return false;
  }
  const std::uint64_t expected_bytes =
      static_cast<std::uint64_t>(region_count) * 9u * sizeof(std::uint32_t) +
      static_cast<std::uint64_t>(range_count) * 3u * sizeof(std::uint32_t);
  if (expected_bytes != static_cast<std::uint64_t>(end - cursor)) {
    error = "compiled UpdateTileMappings payload size is invalid";
    return false;
  }
  payload.regions.resize(region_count);
  payload.region_sizes.resize(region_count);
  payload.range_flags.resize(range_count);
  payload.heap_range_offsets.resize(range_count);
  payload.range_tile_counts.resize(range_count);
  for (auto &region : payload.regions) {
    if (!read_le(cursor, end, region.subresource) ||
        !read_le(cursor, end, region.x) ||
        !read_le(cursor, end, region.y) ||
        !read_le(cursor, end, region.z)) {
      error = "compiled UpdateTileMappings region is truncated";
      return false;
    }
  }
  for (auto &size : payload.region_sizes) {
    std::uint32_t use_box = 0;
    if (!read_le(cursor, end, size.num_tiles) ||
        !read_le(cursor, end, use_box) ||
        !read_le(cursor, end, size.width) ||
        !read_le(cursor, end, size.height) ||
        !read_le(cursor, end, size.depth) ||
        use_box > 1) {
      error = "compiled UpdateTileMappings region size is invalid";
      return false;
    }
    size.use_box = use_box != 0;
  }
  for (auto *array : {&payload.range_flags, &payload.heap_range_offsets, &payload.range_tile_counts}) {
    for (auto &value : *array) {
      if (!read_le(cursor, end, value)) {
        error = "compiled UpdateTileMappings range array is truncated";
        return false;
      }
    }
  }
  return cursor == end;
}

bool encode_compiled_resource_data_update(
    const EventRecord &event,
    const json &payload,
    std::vector<std::uint8_t> &encoded,
    std::string &error)
{
  if (event.object_refs.empty() || event.object_refs.front() == 0 || event.blob_refs.empty()) {
    error = "ResourceDataUpdate requires a resource object ref and blob ref";
    return false;
  }
  std::uint64_t resource_object_id = 0;
  std::uint64_t apply_sequence = 0;
  std::uint64_t written_begin = 0;
  std::uint64_t written_end = 0;
  std::uint64_t written_size = 0;
  std::uint32_t subresource = 0;
  const auto resource = payload.find("resource_object_id");
  const auto apply = payload.find("apply_sequence");
  const auto begin = payload.find("written_begin");
  const auto end = payload.find("written_end");
  const auto size = payload.find("written_size");
  const auto subresource_it = payload.find("subresource");
  const auto path = payload.find("buffer_path");
  if (resource == payload.end() || !json_u64(*resource, resource_object_id) ||
      resource_object_id != event.object_refs.front() ||
      (apply != payload.end() && !json_u64(*apply, apply_sequence)) ||
      begin == payload.end() || !json_u64(*begin, written_begin) ||
      end == payload.end() || !json_u64(*end, written_end) ||
      written_end < written_begin ||
      (size != payload.end() &&
       (!json_u64(*size, written_size) || written_size != written_end - written_begin)) ||
      subresource_it == payload.end() || !json_u32(*subresource_it, subresource) ||
      path == payload.end() || !path->is_string() || path->get_ref<const std::string &>().empty()) {
    error = "ResourceDataUpdate fields are missing, inconsistent, or invalid";
    return false;
  }
  const auto &buffer_path = path->get_ref<const std::string &>();
  if (buffer_path.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "ResourceDataUpdate buffer_path exceeds uint32 range";
    return false;
  }
  encoded.clear();
  encoded.reserve(32 + buffer_path.size());
  append_le(encoded, apply_sequence);
  append_le(encoded, subresource);
  append_le(encoded, written_begin);
  append_le(encoded, written_end);
  append_le(encoded, static_cast<std::uint32_t>(buffer_path.size()));
  encoded.insert(encoded.end(), buffer_path.begin(), buffer_path.end());
  return true;
}

bool decode_compiled_resource_data_update(
    const std::uint8_t *cursor,
    const std::uint8_t *end,
    CompiledResourceDataUpdatePayload &payload,
    std::string &error)
{
  std::uint32_t path_bytes = 0;
  if (!read_le(cursor, end, payload.apply_sequence) ||
      !read_le(cursor, end, payload.subresource) ||
      !read_le(cursor, end, payload.written_begin) ||
      !read_le(cursor, end, payload.written_end) ||
      !read_le(cursor, end, path_bytes) ||
      payload.written_end < payload.written_begin ||
      static_cast<std::size_t>(end - cursor) != path_bytes || path_bytes == 0) {
    error = "compiled ResourceDataUpdate payload is invalid";
    return false;
  }
  payload.buffer_path.assign(reinterpret_cast<const char *>(cursor), path_bytes);
  cursor += path_bytes;
  return cursor == end;
}

bool read_compiled_dispatch_header(
    std::ifstream &input,
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    std::string_view expected_source_callstream_sha256,
    CompiledDispatchHeader &header,
    std::string &error)
{
  std::array<std::uint8_t, 8> magic{};
  if (!read_exact(input, magic.data(), magic.size()) || magic != kMagic) {
    error = "compiled dispatch stream has invalid magic";
    return false;
  }
  std::array<std::uint8_t, kHeaderBytes - 8> raw_header{};
  if (!read_exact(input, raw_header.data(), raw_header.size())) {
    error = "compiled dispatch stream has a truncated header";
    return false;
  }
  const std::uint8_t *header_cursor = raw_header.data();
  const std::uint8_t *header_end = header_cursor + raw_header.size();
  std::uint32_t version = 0;
  std::uint32_t header_bytes = 0;
  std::uint64_t reserved = 0;
  if (!read_le(header_cursor, header_end, version) ||
      !read_le(header_cursor, header_end, header_bytes) ||
      !read_le(header_cursor, header_end, header.source_callstream_bytes) ||
      !read_le(header_cursor, header_end, header.record_count) ||
      !read_le(header_cursor, header_end, header.encoded_record_bytes)) {
    error = "compiled dispatch stream has a malformed header";
    return false;
  }
  if (static_cast<std::size_t>(header_end - header_cursor) < 64) {
    error = "compiled dispatch stream has a malformed source identity";
    return false;
  }
  header.source_callstream_sha256.assign(
      reinterpret_cast<const char *>(header_cursor), 64);
  header_cursor += 64;
  if (!read_le(header_cursor, header_end, reserved) ||
      header_cursor != header_end) {
    error = "compiled dispatch stream has a malformed header";
    return false;
  }
  if (version != kCompiledDispatchVersion || header_bytes != kHeaderBytes) {
    error = "compiled dispatch stream version is unsupported";
    return false;
  }
  if (reserved != 0) {
    error = "compiled dispatch stream has non-zero reserved header fields";
    return false;
  }
  if (header.source_callstream_bytes != expected_source_callstream_bytes) {
    error = "compiled dispatch stream source size does not match callstream.jsonl";
    return false;
  }
  if (!valid_sha256(expected_source_callstream_sha256) ||
      !valid_sha256(header.source_callstream_sha256) ||
      header.source_callstream_sha256 != expected_source_callstream_sha256) {
    error = "compiled dispatch stream source checksum does not match callstream.jsonl";
    return false;
  }
  std::error_code size_error;
  const auto file_size = std::filesystem::file_size(path, size_error);
  if (size_error || file_size != kHeaderBytes + header.encoded_record_bytes) {
    error = "compiled dispatch stream byte size does not match its header";
    return false;
  }
  if (header.record_count >
      header.encoded_record_bytes /
          (sizeof(std::uint32_t) + kRecordFixedBytes + sizeof(std::uint64_t))) {
    error = "compiled dispatch stream record count is impossible for its byte size";
    return false;
  }
  return true;
}

} // namespace

bool decode_compiled_payload_nodes(
    std::string_view encoded,
    nlohmann::json &payload,
    std::string &error)
{
  error.clear();
  if (encoded.empty()) {
    error = "compiled payload is empty";
    payload = json();
    return false;
  }
  const auto *cursor = reinterpret_cast<const std::uint8_t *>(encoded.data());
  const auto *end = cursor + encoded.size();
  if (!decode_compiled_node(cursor, end, payload, 0, error) || cursor != end) {
    if (error.empty()) {
      error = "compiled payload has trailing bytes";
    }
    payload = json();
    return false;
  }
  if (!payload.is_object()) {
    error = "compiled payload root is not an object";
    payload = json();
    return false;
  }
  return true;
}

bool encode_compiled_dispatch_event(
    const EventRecord &event,
    std::vector<std::uint8_t> &encoded,
    std::string &error)
{
  error.clear();
  encoded.clear();

  const json payload = json::parse(
      event.payload.empty() ? std::string_view("{}") : std::string_view(event.payload),
      nullptr,
      false);
  if (payload.is_discarded() || !payload.is_object()) {
    error = "sequence " + std::to_string(event.callsite.sequence) +
            " has a non-object payload while compiling dispatch stream";
    return false;
  }
  const auto dispatch_route = compile_dispatch_route(event, payload);
  const auto command_kind = compile_command_kind(event.callsite.function_name);
  auto payload_encoding = EventPayloadEncoding::CompiledNodes;
  std::vector<std::uint8_t> compiled_payload;
  if (dispatch_route == CompiledDispatchRoute::UpdateTileMappings) {
    if (!encode_compiled_tile_mappings(payload, compiled_payload, error)) {
      error = "sequence " + std::to_string(event.callsite.sequence) + ": " + error;
      return false;
    }
    payload_encoding = EventPayloadEncoding::CompiledTileMappings;
  } else if (dispatch_route == CompiledDispatchRoute::ResourceDataUpdate) {
    if (!encode_compiled_resource_data_update(event, payload, compiled_payload, error)) {
      error = "sequence " + std::to_string(event.callsite.sequence) + ": " + error;
      return false;
    }
    payload_encoding = EventPayloadEncoding::CompiledResourceDataUpdate;
  } else {
    if (!encode_compiled_payload_nodes(payload, compiled_payload, error)) {
      error = "sequence " + std::to_string(event.callsite.sequence) + ": " + error;
      return false;
    }
  }

  std::uint32_t function_bytes = 0;
  std::uint32_t debug_name_bytes = 0;
  std::uint32_t object_ref_count = 0;
  std::uint32_t blob_ref_count = 0;
  std::uint32_t payload_bytes = 0;
  if (!checked_u32(event.callsite.function_name.size(), "function name", function_bytes, error) ||
      !checked_u32(event.object_debug_name.size(), "debug name", debug_name_bytes, error) ||
      !checked_u32(event.object_refs.size(), "object ref count", object_ref_count, error) ||
      !checked_u32(event.blob_refs.size(), "blob ref count", blob_ref_count, error) ||
      !checked_u32(compiled_payload.size(), "payload", payload_bytes, error)) {
    return false;
  }

  const std::uint64_t body_size_64 =
      kRecordFixedBytes +
      static_cast<std::uint64_t>(function_bytes) +
      static_cast<std::uint64_t>(debug_name_bytes) +
      static_cast<std::uint64_t>(object_ref_count) * sizeof(std::uint64_t) +
      static_cast<std::uint64_t>(blob_ref_count) * sizeof(std::uint64_t) +
      static_cast<std::uint64_t>(payload_bytes);
  if (body_size_64 > std::numeric_limits<std::uint32_t>::max()) {
    error = "sequence " + std::to_string(event.callsite.sequence) +
            " compiled dispatch record exceeds uint32 range";
    return false;
  }
  const auto body_size = static_cast<std::uint32_t>(body_size_64);
  encoded.reserve(sizeof(std::uint32_t) + body_size);

  append_le(encoded, body_size);
  encoded.push_back(static_cast<std::uint8_t>(event.kind));
  encoded.push_back(static_cast<std::uint8_t>(event.boundary));
  encoded.push_back(static_cast<std::uint8_t>(event.object_kind));
  encoded.push_back(static_cast<std::uint8_t>(payload_encoding));
  append_le(encoded, event.callsite.result_code);
  append_le(encoded, function_bytes);
  append_le(encoded, debug_name_bytes);
  append_le(encoded, object_ref_count);
  append_le(encoded, blob_ref_count);
  append_le(encoded, payload_bytes);
  const auto compiled_kinds =
      static_cast<std::uint32_t>(dispatch_route) |
      (static_cast<std::uint32_t>(command_kind) << 16u);
  append_le(encoded, compiled_kinds);
  append_le(encoded, event.callsite.sequence);
  append_le(encoded, event.time_ns);
  append_le(encoded, event.elapsed_ns);
  append_le(encoded, event.object_id);
  append_le(encoded, event.parent_object_id);
  encoded.insert(
      encoded.end(), event.callsite.function_name.begin(), event.callsite.function_name.end());
  encoded.insert(encoded.end(), event.object_debug_name.begin(), event.object_debug_name.end());
  for (const auto ref : event.object_refs) {
    append_le(encoded, static_cast<std::uint64_t>(ref));
  }
  for (const auto ref : event.blob_refs) {
    append_le(encoded, static_cast<std::uint64_t>(ref));
  }
  encoded.insert(encoded.end(), compiled_payload.begin(), compiled_payload.end());
  const auto checksum = record_checksum64(
      encoded.data(), sizeof(std::uint32_t),
      encoded.data() + sizeof(std::uint32_t), body_size);
  append_le(encoded, checksum);
  return encoded.size() == sizeof(std::uint32_t) + body_size + sizeof(std::uint64_t);
}

bool write_compiled_dispatch_header(
    std::ostream &output,
    const CompiledDispatchHeader &header,
    std::string &error)
{
  error.clear();
  const auto source_digest = header.source_callstream_sha256.empty()
                                 ? std::string(64, '0')
                                 : header.source_callstream_sha256;
  if (!valid_sha256(source_digest)) {
    error = "compiled dispatch source checksum is not lowercase sha256";
    return false;
  }
  output.write(reinterpret_cast<const char *>(kMagic.data()), kMagic.size());
  if (!write_le(output, kCompiledDispatchVersion) ||
      !write_le(output, kHeaderBytes) ||
      !write_le(output, header.source_callstream_bytes) ||
      !write_le(output, header.record_count) ||
      !write_le(output, header.encoded_record_bytes)) {
    error = "failed to write compiled dispatch header";
    return false;
  }
  output.write(source_digest.data(), static_cast<std::streamsize>(source_digest.size()));
  if (!output || !write_le(output, std::uint64_t{0})) {
    error = "failed to write compiled dispatch header";
    return false;
  }
  return true;
}

bool inspect_compiled_dispatch(
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    std::string_view expected_source_callstream_sha256,
    CompiledDispatchHeader &header,
    std::string &error)
{
  error.clear();
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    error = "failed to open compiled dispatch stream: " + path.string();
    return false;
  }
  return read_compiled_dispatch_header(
      input, path, expected_source_callstream_bytes,
      expected_source_callstream_sha256, header, error);
}

bool for_each_compiled_dispatch_event(
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    std::string_view expected_source_callstream_sha256,
    const CompiledDispatchEventCallback &callback,
    CompiledDispatchHeader *header_out,
    std::string &error)
{
  error.clear();
  if (!callback) {
    error = "compiled dispatch stream callback is empty";
    return false;
  }
  std::vector<char> input_buffer(4u * 1024u * 1024u);
  std::ifstream input;
  input.rdbuf()->pubsetbuf(input_buffer.data(), static_cast<std::streamsize>(input_buffer.size()));
  input.open(path, std::ios::binary);
  if (!input.is_open()) {
    error = "failed to open compiled dispatch stream: " + path.string();
    return false;
  }
  CompiledDispatchHeader header;
  if (!read_compiled_dispatch_header(
          input, path, expected_source_callstream_bytes,
          expected_source_callstream_sha256, header, error)) {
    return false;
  }

  std::vector<std::uint8_t> record;
  // The callback contract limits EventRecord lifetime to one invocation. Reuse its owned buffers
  // so a multi-million-record retrace does not allocate function, ref, and payload storage again
  // for every dispatch record.
  EventRecord event;
  for (std::uint64_t index = 0; index < header.record_count; ++index) {
    std::array<std::uint8_t, 4> raw_size{};
    if (!read_exact(input, raw_size.data(), raw_size.size())) {
      error = "compiled dispatch stream ended before record " + std::to_string(index);
      return false;
    }
    const std::uint8_t *size_cursor = raw_size.data();
    const std::uint8_t *size_end = size_cursor + raw_size.size();
    std::uint32_t body_size = 0;
    if (!read_le(size_cursor, size_end, body_size) || body_size < kRecordFixedBytes) {
      error = "compiled dispatch stream has an invalid record size";
      return false;
    }
    record.resize(body_size);
    if (!read_exact(input, record.data(), record.size())) {
      error = "compiled dispatch stream has a truncated record " + std::to_string(index);
      return false;
    }
    std::array<std::uint8_t, 8> raw_checksum{};
    if (!read_exact(input, raw_checksum.data(), raw_checksum.size())) {
      error = "compiled dispatch stream has a truncated record checksum";
      return false;
    }
    const std::uint8_t *checksum_cursor = raw_checksum.data();
    const std::uint8_t *checksum_end = checksum_cursor + raw_checksum.size();
    std::uint64_t stored_checksum = 0;
    if (!read_le(checksum_cursor, checksum_end, stored_checksum) ||
        stored_checksum != record_checksum64(
            raw_size.data(), raw_size.size(), record.data(), record.size())) {
      error = "compiled dispatch stream record " + std::to_string(index) +
              " failed checksum validation";
      return false;
    }

    const std::uint8_t *cursor = record.data();
    const std::uint8_t *end = cursor + record.size();
    if (end - cursor < 4) {
      error = "compiled dispatch stream has a truncated record prefix";
      return false;
    }
    const auto kind = *cursor++;
    const auto boundary = *cursor++;
    const auto object_kind = *cursor++;
    const auto payload_encoding = *cursor++;
    std::int32_t result_code = 0;
    std::uint32_t function_bytes = 0;
    std::uint32_t debug_name_bytes = 0;
    std::uint32_t object_ref_count = 0;
    std::uint32_t blob_ref_count = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t compiled_kinds = 0;
    if (!read_le(cursor, end, result_code) ||
        !read_le(cursor, end, function_bytes) ||
        !read_le(cursor, end, debug_name_bytes) ||
        !read_le(cursor, end, object_ref_count) ||
        !read_le(cursor, end, blob_ref_count) ||
        !read_le(cursor, end, payload_bytes) ||
        !read_le(cursor, end, compiled_kinds) ||
        !read_le(cursor, end, event.callsite.sequence) ||
        !read_le(cursor, end, event.time_ns) ||
        !read_le(cursor, end, event.elapsed_ns) ||
        !read_le(cursor, end, event.object_id) ||
        !read_le(cursor, end, event.parent_object_id)) {
      error = "compiled dispatch stream has a malformed record header";
      return false;
    }
    const std::uint64_t variable_bytes =
        static_cast<std::uint64_t>(function_bytes) + debug_name_bytes +
        static_cast<std::uint64_t>(object_ref_count) * sizeof(std::uint64_t) +
        static_cast<std::uint64_t>(blob_ref_count) * sizeof(std::uint64_t) + payload_bytes;
    const auto dispatch_route = compiled_kinds & 0xffffu;
    const auto command_kind = compiled_kinds >> 16u;
    if (variable_bytes != static_cast<std::uint64_t>(end - cursor) ||
        kind > static_cast<std::uint8_t>(EventKind::Boundary) ||
        boundary > static_cast<std::uint8_t>(BoundaryKind::DebugMarker) ||
        object_kind > static_cast<std::uint8_t>(ObjectKind::QueryHeap) ||
        dispatch_route == static_cast<std::uint32_t>(CompiledDispatchRoute::Unspecified) ||
        dispatch_route > static_cast<std::uint32_t>(CompiledDispatchRoute::DiagnosticStub) ||
        command_kind > static_cast<std::uint32_t>(CompiledCommandKind::UnmapResource) ||
        payload_encoding > static_cast<std::uint8_t>(EventPayloadEncoding::CompiledResourceDataUpdate) ||
        (dispatch_route == static_cast<std::uint32_t>(CompiledDispatchRoute::UpdateTileMappings)) !=
            (payload_encoding == static_cast<std::uint8_t>(EventPayloadEncoding::CompiledTileMappings)) ||
        (dispatch_route == static_cast<std::uint32_t>(CompiledDispatchRoute::ResourceDataUpdate)) !=
            (payload_encoding == static_cast<std::uint8_t>(EventPayloadEncoding::CompiledResourceDataUpdate)) ||
        (dispatch_route != static_cast<std::uint32_t>(CompiledDispatchRoute::UpdateTileMappings) &&
         dispatch_route != static_cast<std::uint32_t>(CompiledDispatchRoute::ResourceDataUpdate) &&
         payload_encoding != static_cast<std::uint8_t>(EventPayloadEncoding::CompiledNodes))) {
      error = "compiled dispatch stream has invalid record fields";
      return false;
    }
    event.kind = static_cast<EventKind>(kind);
    event.boundary = static_cast<BoundaryKind>(boundary);
    event.object_kind = static_cast<ObjectKind>(object_kind);
    event.payload_encoding = static_cast<EventPayloadEncoding>(payload_encoding);
    event.dispatch_route = static_cast<CompiledDispatchRoute>(dispatch_route);
    event.command_kind = static_cast<CompiledCommandKind>(command_kind);
    event.callsite.result_code = result_code;
    event.callsite.function_name.assign(reinterpret_cast<const char *>(cursor), function_bytes);
    cursor += function_bytes;
    event.object_debug_name.assign(reinterpret_cast<const char *>(cursor), debug_name_bytes);
    cursor += debug_name_bytes;
    event.object_refs.resize(object_ref_count);
    for (auto &ref : event.object_refs) {
      std::uint64_t value = 0;
      if (!read_le(cursor, end, value)) {
        error = "compiled dispatch stream has truncated object refs";
        return false;
      }
      ref = static_cast<ObjectId>(value);
    }
    event.blob_refs.resize(blob_ref_count);
    for (auto &ref : event.blob_refs) {
      std::uint64_t value = 0;
      if (!read_le(cursor, end, value)) {
        error = "compiled dispatch stream has truncated blob refs";
        return false;
      }
      ref = static_cast<BlobId>(value);
    }
    if (event.payload_encoding == EventPayloadEncoding::CompiledTileMappings) {
      event.payload.clear();
      if (!decode_compiled_tile_mappings(
              cursor, cursor + payload_bytes, event.compiled_tile_mapping, error)) {
        return false;
      }
      event.compiled_resource_data_update = {};
    } else if (event.payload_encoding == EventPayloadEncoding::CompiledResourceDataUpdate) {
      event.payload.clear();
      event.compiled_tile_mapping = {};
      if (!decode_compiled_resource_data_update(
              cursor, cursor + payload_bytes, event.compiled_resource_data_update, error)) {
        return false;
      }
    } else {
      event.compiled_tile_mapping.regions.clear();
      event.compiled_tile_mapping.region_sizes.clear();
      event.compiled_tile_mapping.range_flags.clear();
      event.compiled_tile_mapping.heap_range_offsets.clear();
      event.compiled_tile_mapping.range_tile_counts.clear();
      event.compiled_resource_data_update = {};
      event.payload.assign(reinterpret_cast<const char *>(cursor), payload_bytes);
    }
    cursor += payload_bytes;
    if (cursor != end || event.callsite.sequence == 0 || event.callsite.function_name.empty()) {
      error = "compiled dispatch stream has an invalid event record";
      return false;
    }
    if (!callback(event)) {
      if (header_out) {
        *header_out = header;
      }
      return true;
    }
  }
  if (input.peek() != std::char_traits<char>::eof()) {
    error = "compiled dispatch stream has trailing bytes";
    return false;
  }
  if (header_out) {
    *header_out = header;
  }
  return true;
}

bool load_compiled_dispatch_events(
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    std::string_view expected_source_callstream_sha256,
    std::vector<EventRecord> &events,
    CompiledDispatchHeader *header,
    std::string &error)
{
  events.clear();
  CompiledDispatchHeader inspected;
  if (!inspect_compiled_dispatch(
          path, expected_source_callstream_bytes,
          expected_source_callstream_sha256, inspected, error)) {
    return false;
  }
  events.reserve(static_cast<std::size_t>(inspected.record_count));
  return for_each_compiled_dispatch_event(
      path,
      expected_source_callstream_bytes,
      expected_source_callstream_sha256,
      [&](const EventRecord &event) {
        events.push_back(event);
        return true;
      },
      header,
      error);
}

} // namespace apitrace::trace
