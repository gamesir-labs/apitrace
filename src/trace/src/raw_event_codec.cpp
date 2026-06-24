#include "apitrace/raw_event_codec.hpp"

#include <array>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace apitrace::trace::raw {

namespace {

constexpr std::uint32_t kRawPayloadFlagsRequiresDxmtBackend = 1u << 0;

void put_u8(std::vector<std::uint8_t> &bytes, std::uint8_t value)
{
  bytes.push_back(value);
}

void put_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void put_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void put_string(std::vector<std::uint8_t> &bytes, const std::string &value)
{
  put_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

std::string json_escape(std::string_view value)
{
  std::ostringstream output;
  for (const char ch : value) {
    switch (ch) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        output << "\\u00";
        constexpr char digits[] = "0123456789abcdef";
        output << digits[(static_cast<unsigned char>(ch) >> 4) & 0x0f]
               << digits[static_cast<unsigned char>(ch) & 0x0f];
      } else {
        output << ch;
      }
      break;
    }
  }
  return output.str();
}

class PayloadCursor {
public:
  explicit PayloadCursor(const std::vector<std::uint8_t> &bytes)
      : bytes_(bytes)
  {
  }

  bool u8(std::uint8_t &value)
  {
    if (remaining() < 1) {
      return false;
    }
    value = bytes_[offset_++];
    return true;
  }

  bool u16(std::uint16_t &value)
  {
    if (remaining() < 2) {
      return false;
    }
    value = static_cast<std::uint16_t>(bytes_[offset_]) |
            static_cast<std::uint16_t>(bytes_[offset_ + 1] << 8);
    offset_ += 2;
    return true;
  }

  bool u32(std::uint32_t &value)
  {
    if (remaining() < 4) {
      return false;
    }
    value = static_cast<std::uint32_t>(bytes_[offset_]) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24);
    offset_ += 4;
    return true;
  }

  bool u64(std::uint64_t &value)
  {
    if (remaining() < 8) {
      return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8);
    }
    offset_ += 8;
    return true;
  }

  bool string(std::string &value)
  {
    std::uint32_t size = 0;
    if (!u32(size) || remaining() < size) {
      return false;
    }
    value.assign(
        reinterpret_cast<const char *>(bytes_.data() + offset_),
        reinterpret_cast<const char *>(bytes_.data() + offset_ + size));
    offset_ += size;
    return true;
  }

  [[nodiscard]] bool done() const noexcept
  {
    return offset_ == bytes_.size();
  }

private:
  [[nodiscard]] std::size_t remaining() const noexcept
  {
    return bytes_.size() - offset_;
  }

  const std::vector<std::uint8_t> &bytes_;
  std::size_t offset_ = 0;
};

AssetKind asset_kind_from_raw_blob_kind(std::uint32_t kind)
{
  switch (static_cast<RawBlobKind>(kind)) {
  case RawBlobKind::Buffer:
    return AssetKind::Buffer;
  case RawBlobKind::ShaderDxbc:
    return AssetKind::ShaderDxbc;
  case RawBlobKind::ShaderDxil:
    return AssetKind::ShaderDxil;
  case RawBlobKind::RootSignature:
    return AssetKind::RootSignature;
  case RawBlobKind::Unknown:
  default:
    return AssetKind::Unknown;
  }
}

std::string generated_asset_relative_path(AssetKind kind, BlobId blob_id)
{
  const char *directory = ".";
  const char *extension = ".bin";
  switch (kind) {
  case AssetKind::Buffer:
    directory = "buffers";
    extension = ".buffer";
    break;
  case AssetKind::ShaderDxbc:
    directory = "shaders";
    extension = ".dxbc";
    break;
  case AssetKind::ShaderDxil:
    directory = "shaders";
    extension = ".dxil";
    break;
  case AssetKind::RootSignature:
    directory = "shaders";
    extension = ".rootsig";
    break;
  case AssetKind::Texture:
    directory = "textures";
    extension = ".texture";
    break;
  case AssetKind::Pipeline:
    directory = "pipelines";
    extension = ".pipeline.json";
    break;
  case AssetKind::ObjectIndex:
    directory = "objects";
    extension = ".json";
    break;
  case AssetKind::Analysis:
    directory = "analysis";
    extension = ".jsonl";
    break;
  case AssetKind::Unknown:
  default:
    break;
  }
  std::ostringstream path;
  if (std::string_view(directory) != ".") {
    path << directory << "/";
  }
  path << "asset-" << blob_id << extension;
  return path.str();
}

struct DecodeContext {
  const RawCaptureReader &reader;
  std::unordered_map<std::uint64_t, BlobId> final_blob_by_raw_blob;
  BlobId next_blob_id = 1;
};

bool materialize_raw_blob(
    DecodeContext &context,
    std::uint64_t raw_blob_id,
    AssetKind expected_kind,
    std::string debug_name,
    AssetRecord &asset,
    std::string &error)
{
  if (raw_blob_id == kInvalidRawBlobId) {
    error = "raw event referenced invalid raw blob id 0";
    return false;
  }

  RawBlobExtent extent;
  if (!context.reader.find_blob_extent(raw_blob_id, extent)) {
    error = "raw event referenced missing raw blob id " + std::to_string(raw_blob_id);
    return false;
  }

  const auto extent_kind = asset_kind_from_raw_blob_kind(extent.kind);
  const auto kind = expected_kind == AssetKind::Unknown ? extent_kind : expected_kind;
  if (kind == AssetKind::Unknown) {
    error = "raw blob " + std::to_string(raw_blob_id) + " has unknown asset kind";
    return false;
  }
  if (extent_kind != AssetKind::Unknown && expected_kind != AssetKind::Unknown && extent_kind != expected_kind) {
    error = "raw blob " + std::to_string(raw_blob_id) + " kind does not match event payload";
    return false;
  }

  std::vector<std::uint8_t> bytes;
  if (!context.reader.read_blob(raw_blob_id, bytes)) {
    error = "failed to read raw blob " + std::to_string(raw_blob_id);
    return false;
  }

  const auto [it, inserted] = context.final_blob_by_raw_blob.emplace(raw_blob_id, context.next_blob_id);
  if (inserted) {
    ++context.next_blob_id;
  }

  asset.blob_id = it->second;
  asset.kind = kind;
  asset.debug_name = std::move(debug_name);
  asset.byte_size = static_cast<std::uint64_t>(bytes.size());
  asset.binary_payload = true;
  asset.payload_bytes = std::move(bytes);
  asset.relative_path = generated_asset_relative_path(asset.kind, asset.blob_id);
  return true;
}

void stamp_common(EventRecord &event, const RawEventRecord &record)
{
  event.callsite.sequence = record.header.sequence;
  event.time_ns = record.header.timestamp_or_monotonic_counter;
  event.elapsed_ns = 0;
}

bool decode_resource_create(
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  std::uint64_t device = 0;
  std::uint64_t resource = 0;
  std::uint64_t dimension = 0;
  std::uint64_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t depth_or_array_size = 0;
  std::uint16_t mip_levels = 0;
  std::uint32_t format = 0;
  std::uint32_t flags = 0;
  std::uint32_t initial_state = 0;
  std::string debug_name;
  if (!cursor.u64(device) ||
      !cursor.u64(resource) ||
      !cursor.u64(dimension) ||
      !cursor.u64(width) ||
      !cursor.u32(height) ||
      !cursor.u16(depth_or_array_size) ||
      !cursor.u16(mip_levels) ||
      !cursor.u32(format) ||
      !cursor.u32(flags) ||
      !cursor.u32(initial_state) ||
      !cursor.string(debug_name) ||
      !cursor.done()) {
    error = "malformed ResourceCreate payload";
    return false;
  }

  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::ObjectCreate;
  event.object_id = resource;
  event.object_kind = ObjectKind::Resource;
  event.parent_object_id = device;
  event.object_debug_name = debug_name;
  event.object_refs = {device, resource};
  std::ostringstream payload;
  payload << "{\"dimension\":" << dimension
          << ",\"width\":" << width
          << ",\"height\":" << height
          << ",\"depth_or_array_size\":" << depth_or_array_size
          << ",\"mip_levels\":" << mip_levels
          << ",\"format\":" << format
          << ",\"flags\":" << flags
          << ",\"initial_state\":" << initial_state << "}";
  event.payload = payload.str();
  return true;
}

bool decode_resource_unmap(
    DecodeContext &context,
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  std::uint64_t resource = 0;
  std::uint64_t raw_blob_id = 0;
  std::uint64_t written_begin = 0;
  std::uint64_t written_end = 0;
  if (!cursor.u64(resource) ||
      !cursor.u64(raw_blob_id) ||
      !cursor.u64(written_begin) ||
      !cursor.u64(written_end) ||
      !cursor.done()) {
    error = "malformed ResourceUnmap payload";
    return false;
  }

  AssetRecord asset;
  if (!materialize_raw_blob(context, raw_blob_id, AssetKind::Buffer, "d3d12-resource-unmap", asset, error)) {
    return false;
  }

  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Call;
  event.callsite.function_name = "ID3D12Resource::Unmap";
  event.callsite.result_code = static_cast<std::int32_t>(record.header.result_or_flags);
  event.object_refs = {resource};
  event.blob_refs = {asset.blob_id};
  event.payload = "{\"written_range\":{\"begin\":" + std::to_string(written_begin) +
                  ",\"end\":" + std::to_string(written_end) +
                  "},\"buffer_path\":\"" + asset.relative_path.generic_string() + "\"}";
  decoded.assets.push_back(std::move(asset));
  return true;
}

bool decode_graphics_pipeline_create(
    DecodeContext &context,
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  std::uint64_t device = 0;
  std::uint64_t root_signature = 0;
  std::uint64_t pipeline = 0;
  std::uint64_t vs_raw_blob_id = 0;
  std::uint64_t vs_bytecode_size = 0;
  std::uint64_t ps_raw_blob_id = 0;
  std::uint64_t ps_bytecode_size = 0;
  std::uint32_t node_mask = 0;
  std::uint32_t flags = 0;
  std::uint32_t payload_flags = 0;
  if (!cursor.u64(device) ||
      !cursor.u64(root_signature) ||
      !cursor.u64(pipeline) ||
      !cursor.u64(vs_raw_blob_id) ||
      !cursor.u64(vs_bytecode_size) ||
      !cursor.u64(ps_raw_blob_id) ||
      !cursor.u64(ps_bytecode_size) ||
      !cursor.u32(node_mask) ||
      !cursor.u32(flags) ||
      !cursor.u32(payload_flags) ||
      !cursor.done()) {
    error = "malformed GraphicsPipelineCreate payload";
    return false;
  }

  AssetRecord vs;
  if (!materialize_raw_blob(context, vs_raw_blob_id, AssetKind::ShaderDxbc, "raw-vs-bytecode", vs, error)) {
    return false;
  }
  AssetRecord ps;
  if (!materialize_raw_blob(context, ps_raw_blob_id, AssetKind::ShaderDxbc, "raw-ps-bytecode", ps, error)) {
    return false;
  }

  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Call;
  event.callsite.function_name = "ID3D12Device::CreateGraphicsPipelineState";
  event.callsite.result_code = static_cast<std::int32_t>(record.header.result_or_flags);
  event.object_refs = {device, root_signature, pipeline};
  event.blob_refs = {vs.blob_id, ps.blob_id};
  std::ostringstream payload;
  payload << "{\"pso_raw_version\":1"
          << ",\"pso_kind\":\"graphics\""
          << ",\"root_signature_object_id\":" << root_signature
          << ",\"node_mask\":" << node_mask
          << ",\"flags\":" << flags
          << ",\"input_layout\":[]"
          << ",\"blend_state\":{}"
          << ",\"sample_mask\":4294967295"
          << ",\"rasterizer_state\":{}"
          << ",\"depth_stencil_state\":{}"
          << ",\"stream_output\":{}"
          << ",\"primitive_topology_type\":3"
          << ",\"num_render_targets\":1"
          << ",\"rtv_formats\":[28,0,0,0,0,0,0,0]"
          << ",\"dsv_format\":0"
          << ",\"sample_desc\":{\"count\":1,\"quality\":0}"
          << ",\"ib_strip_cut_value\":0"
          << ",\"vs\":{\"bytecode_size\":" << vs_bytecode_size << ",\"blob_id\":" << vs.blob_id << "}"
          << ",\"ps\":{\"bytecode_size\":" << ps_bytecode_size << ",\"blob_id\":" << ps.blob_id << "}"
          << ",\"ds\":null,\"hs\":null,\"gs\":null"
          << ",\"requires_dxmt_backend\":"
          << ((payload_flags & kRawPayloadFlagsRequiresDxmtBackend) ? "true" : "false")
          << "}";
  event.payload = payload.str();
  decoded.assets.push_back(std::move(vs));
  decoded.assets.push_back(std::move(ps));
  return true;
}

bool decode_draw_instanced(
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  std::uint64_t command_list = 0;
  std::uint32_t vertex_count_per_instance = 0;
  std::uint32_t instance_count = 0;
  std::uint32_t start_vertex_location = 0;
  std::uint32_t start_instance_location = 0;
  if (!cursor.u64(command_list) ||
      !cursor.u32(vertex_count_per_instance) ||
      !cursor.u32(instance_count) ||
      !cursor.u32(start_vertex_location) ||
      !cursor.u32(start_instance_location) ||
      !cursor.done()) {
    error = "malformed DrawInstanced payload";
    return false;
  }

  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Call;
  event.callsite.function_name = "ID3D12GraphicsCommandList::DrawInstanced";
  event.callsite.result_code = static_cast<std::int32_t>(record.header.result_or_flags);
  event.object_refs = {command_list};
  std::ostringstream payload;
  payload << "{\"vertex_count_per_instance\":" << vertex_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_vertex_location\":" << start_vertex_location
          << ",\"start_instance_location\":" << start_instance_location << "}";
  event.payload = payload.str();
  return true;
}

bool decode_dispatch(
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  std::uint64_t command_list = 0;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t z = 0;
  if (!cursor.u64(command_list) ||
      !cursor.u32(x) ||
      !cursor.u32(y) ||
      !cursor.u32(z) ||
      !cursor.done()) {
    error = "malformed Dispatch payload";
    return false;
  }

  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Call;
  event.callsite.function_name = "ID3D12GraphicsCommandList::Dispatch";
  event.callsite.result_code = static_cast<std::int32_t>(record.header.result_or_flags);
  event.object_refs = {command_list};
  std::ostringstream payload;
  payload << "{\"thread_group_count_x\":" << x
          << ",\"thread_group_count_y\":" << y
          << ",\"thread_group_count_z\":" << z << "}";
  event.payload = payload.str();
  return true;
}

bool decode_present_payload(
    const RawEventRecord &record,
    std::uint64_t &swap_chain,
    std::uint64_t &frame_index,
    std::uint32_t &sync_interval,
    std::uint32_t &flags,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  if (!cursor.u64(swap_chain) ||
      !cursor.u64(frame_index) ||
      !cursor.u32(sync_interval) ||
      !cursor.u32(flags) ||
      !cursor.done()) {
    error = "malformed Present payload";
    return false;
  }
  return true;
}

bool decode_present_call(
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  std::uint64_t swap_chain = 0;
  std::uint64_t frame_index = 0;
  std::uint32_t sync_interval = 0;
  std::uint32_t flags = 0;
  if (!decode_present_payload(record, swap_chain, frame_index, sync_interval, flags, error)) {
    return false;
  }
  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Call;
  event.callsite.function_name = "IDXGISwapChain::Present";
  event.callsite.result_code = static_cast<std::int32_t>(record.header.result_or_flags);
  event.object_refs = {swap_chain};
  event.payload = "{\"frame_index\":" + std::to_string(frame_index) +
                  ",\"sync_interval\":" + std::to_string(sync_interval) +
                  ",\"flags\":" + std::to_string(flags) + "}";
  return true;
}

bool decode_present_boundary(
    const RawEventRecord &record,
    DecodedRawEvent &decoded,
    std::string &error)
{
  std::uint64_t swap_chain = 0;
  std::uint64_t frame_index = 0;
  std::uint32_t sync_interval = 0;
  std::uint32_t flags = 0;
  if (!decode_present_payload(record, swap_chain, frame_index, sync_interval, flags, error)) {
    return false;
  }
  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Boundary;
  event.boundary = BoundaryKind::Present;
  event.object_refs = {swap_chain};
  event.payload = "{\"frame_index\":" + std::to_string(frame_index) +
                  ",\"sync_interval\":" + std::to_string(sync_interval) +
                  ",\"flags\":" + std::to_string(flags) + "}";
  return true;
}

bool decode_frame_boundary(
    const RawEventRecord &record,
    const char *label,
    BoundaryKind boundary,
    DecodedRawEvent &decoded,
    std::string &error)
{
  PayloadCursor cursor(record.payload);
  std::uint64_t frame_index = 0;
  if (!cursor.u64(frame_index) || !cursor.done()) {
    error = "malformed frame boundary payload";
    return false;
  }
  auto &event = decoded.event;
  stamp_common(event, record);
  event.kind = EventKind::Boundary;
  event.boundary = boundary;
  event.payload = "{\"frame_index\":" + std::to_string(frame_index) +
                  ",\"label\":\"" + std::string(label) + "\"}";
  return true;
}

} // namespace

std::string raw_event_contract_markdown()
{
  return
      "Raw event contract v1\n"
      "\n"
      "All integer fields are little-endian. RawEventHeader carries sequence, thread_id, timestamp_or_monotonic_counter, opcode, result_or_flags, and payload_len. Payloads never contain JSON, hashes, canonical paths, or dedup state.\n"
      "\n"
      "- ResourceCreate 0x0101: u64 device_object_id, u64 resource_object_id, u64 dimension, u64 width, u32 height, u16 depth_or_array_size, u16 mip_levels, u32 format, u32 flags, u32 initial_state, str debug_name.\n"
      "- ResourceUnmap 0x0102: u64 resource_object_id, u64 raw_blob_id, u64 written_begin, u64 written_end. raw_blob_id must reference RawBlobKind::Buffer.\n"
      "- GraphicsPipelineCreate 0x0201: u64 device_object_id, u64 root_signature_object_id, u64 pipeline_state_object_id, u64 vs_raw_blob_id, u64 vs_bytecode_size, u64 ps_raw_blob_id, u64 ps_bytecode_size, u32 node_mask, u32 flags, u32 payload_flags. Shader raw blobs carry bytecode bytes; finalize emits existing pso_raw_version payload and later rebuilds pipeline_path.\n"
      "- DrawInstanced 0x0301: u64 command_list_object_id, u32 vertex_count_per_instance, u32 instance_count, u32 start_vertex_location, u32 start_instance_location.\n"
      "- Dispatch 0x0302: u64 command_list_object_id, u32 thread_group_count_x, u32 thread_group_count_y, u32 thread_group_count_z.\n"
      "- PresentCall 0x0401 / PresentBoundary 0x0404: u64 swap_chain_object_id, u64 frame_index, u32 sync_interval, u32 flags.\n"
      "- FrameBegin 0x0402 / FrameEnd 0x0403: u64 frame_index. Final payloads include label=FrameBegin or label=FrameEnd for existing tail-consistency checks.\n";
}

std::vector<std::uint8_t> encode_resource_create_payload(
    ObjectId device_object_id,
    ObjectId resource_object_id,
    std::uint64_t dimension,
    std::uint64_t width,
    std::uint32_t height,
    std::uint16_t depth_or_array_size,
    std::uint16_t mip_levels,
    std::uint32_t format,
    std::uint32_t flags,
    std::uint32_t initial_state,
    std::string debug_name)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, device_object_id);
  put_u64(bytes, resource_object_id);
  put_u64(bytes, dimension);
  put_u64(bytes, width);
  put_u32(bytes, height);
  put_u16(bytes, depth_or_array_size);
  put_u16(bytes, mip_levels);
  put_u32(bytes, format);
  put_u32(bytes, flags);
  put_u32(bytes, initial_state);
  put_string(bytes, debug_name);
  return bytes;
}

std::vector<std::uint8_t> encode_resource_unmap_payload(
    ObjectId resource_object_id,
    std::uint64_t raw_blob_id,
    std::uint64_t written_begin,
    std::uint64_t written_end)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, resource_object_id);
  put_u64(bytes, raw_blob_id);
  put_u64(bytes, written_begin);
  put_u64(bytes, written_end);
  return bytes;
}

std::vector<std::uint8_t> encode_graphics_pipeline_create_payload(
    ObjectId device_object_id,
    ObjectId root_signature_object_id,
    ObjectId pipeline_state_object_id,
    std::uint64_t vs_raw_blob_id,
    std::uint64_t vs_bytecode_size,
    std::uint64_t ps_raw_blob_id,
    std::uint64_t ps_bytecode_size,
    std::uint32_t node_mask,
    std::uint32_t flags,
    bool requires_dxmt_backend)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, device_object_id);
  put_u64(bytes, root_signature_object_id);
  put_u64(bytes, pipeline_state_object_id);
  put_u64(bytes, vs_raw_blob_id);
  put_u64(bytes, vs_bytecode_size);
  put_u64(bytes, ps_raw_blob_id);
  put_u64(bytes, ps_bytecode_size);
  put_u32(bytes, node_mask);
  put_u32(bytes, flags);
  put_u32(bytes, requires_dxmt_backend ? kRawPayloadFlagsRequiresDxmtBackend : 0);
  return bytes;
}

std::vector<std::uint8_t> encode_draw_instanced_payload(
    ObjectId command_list_object_id,
    std::uint32_t vertex_count_per_instance,
    std::uint32_t instance_count,
    std::uint32_t start_vertex_location,
    std::uint32_t start_instance_location)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, command_list_object_id);
  put_u32(bytes, vertex_count_per_instance);
  put_u32(bytes, instance_count);
  put_u32(bytes, start_vertex_location);
  put_u32(bytes, start_instance_location);
  return bytes;
}

std::vector<std::uint8_t> encode_dispatch_payload(
    ObjectId command_list_object_id,
    std::uint32_t thread_group_count_x,
    std::uint32_t thread_group_count_y,
    std::uint32_t thread_group_count_z)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, command_list_object_id);
  put_u32(bytes, thread_group_count_x);
  put_u32(bytes, thread_group_count_y);
  put_u32(bytes, thread_group_count_z);
  return bytes;
}

std::vector<std::uint8_t> encode_present_payload(
    ObjectId swap_chain_object_id,
    std::uint64_t frame_index,
    std::uint32_t sync_interval,
    std::uint32_t flags)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, swap_chain_object_id);
  put_u64(bytes, frame_index);
  put_u32(bytes, sync_interval);
  put_u32(bytes, flags);
  return bytes;
}

std::vector<std::uint8_t> encode_frame_boundary_payload(std::uint64_t frame_index)
{
  std::vector<std::uint8_t> bytes;
  put_u64(bytes, frame_index);
  return bytes;
}

RawDecodeResult decode_raw_events(
    const RawCaptureReader &reader,
    const std::vector<RawEventRecord> &records)
{
  RawDecodeResult result;
  DecodeContext context{reader};
  result.events.reserve(records.size());

  for (const auto &record : records) {
    DecodedRawEvent decoded;
    bool ok = false;
    switch (static_cast<RawEventOpcode>(record.header.opcode)) {
    case RawEventOpcode::ResourceCreate:
      ok = decode_resource_create(record, decoded, result.error);
      break;
    case RawEventOpcode::ResourceUnmap:
      ok = decode_resource_unmap(context, record, decoded, result.error);
      break;
    case RawEventOpcode::GraphicsPipelineCreate:
      ok = decode_graphics_pipeline_create(context, record, decoded, result.error);
      break;
    case RawEventOpcode::DrawInstanced:
      ok = decode_draw_instanced(record, decoded, result.error);
      break;
    case RawEventOpcode::Dispatch:
      ok = decode_dispatch(record, decoded, result.error);
      break;
    case RawEventOpcode::PresentCall:
      ok = decode_present_call(record, decoded, result.error);
      break;
    case RawEventOpcode::FrameBegin:
      ok = decode_frame_boundary(record, "FrameBegin", BoundaryKind::Frame, decoded, result.error);
      break;
    case RawEventOpcode::FrameEnd:
      ok = decode_frame_boundary(record, "FrameEnd", BoundaryKind::Frame, decoded, result.error);
      break;
    case RawEventOpcode::PresentBoundary:
      ok = decode_present_boundary(record, decoded, result.error);
      break;
    default:
      result.error = "unsupported raw event opcode " + std::to_string(record.header.opcode) +
                     " at sequence " + std::to_string(record.header.sequence);
      ok = false;
      break;
    }
    if (!ok) {
      if (result.error.empty()) {
        result.error = "failed to decode raw event sequence " + std::to_string(record.header.sequence);
      }
      return result;
    }
    result.events.push_back(std::move(decoded));
  }
  return result;
}

} // namespace apitrace::trace::raw
