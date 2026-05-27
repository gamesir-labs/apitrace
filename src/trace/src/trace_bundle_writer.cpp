#include "apitrace/trace_bundle_io.hpp"
#include "metal_callstream_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace apitrace::trace {

namespace {

constexpr const char *kObjectsDirectoryName = "objects";
constexpr const char *kObjectIndexFileName = "objects.json";
constexpr const char *kShadersDirectoryName = "shaders";
constexpr const char *kTexturesDirectoryName = "textures";
constexpr const char *kBuffersDirectoryName = "buffers";
constexpr const char *kPipelinesDirectoryName = "pipelines";

class Sha256 {
public:
  void update(const std::uint8_t *data, std::size_t size)
  {
    for (std::size_t index = 0; index < size; ++index) {
      buffer_[buffer_size_++] = data[index];
      bit_count_ += 8;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_.data());
        buffer_size_ = 0;
      }
    }
  }

  void update(const std::vector<std::uint8_t> &data)
  {
    update(data.data(), data.size());
  }

  void update(std::string_view text)
  {
    update(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  }

  [[nodiscard]] std::string final_hex()
  {
    auto local_buffer = buffer_;
    const auto local_buffer_size = buffer_size_;
    const auto local_bit_count = bit_count_;

    local_buffer[local_buffer_size] = 0x80;
    for (std::size_t index = local_buffer_size + 1; index < local_buffer.size(); ++index) {
      local_buffer[index] = 0;
    }

    if (local_buffer_size >= 56) {
      transform(local_buffer.data());
      local_buffer.fill(0);
    }

    const std::uint64_t bit_count_be = byteswap64(local_bit_count);
    std::memcpy(local_buffer.data() + 56, &bit_count_be, sizeof(bit_count_be));
    transform(local_buffer.data());

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::uint32_t value : state_) {
      output << std::setw(8) << value;
    }
    return output.str();
  }

private:
  static constexpr std::array<std::uint32_t, 64> kTable = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  };

  static std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count)
  {
    return (value >> count) | (value << (32 - count));
  }

  static std::uint32_t byteswap32(std::uint32_t value)
  {
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
  }

  static std::uint64_t byteswap64(std::uint64_t value)
  {
    return ((value & 0x00000000000000ffull) << 56) |
           ((value & 0x000000000000ff00ull) << 40) |
           ((value & 0x0000000000ff0000ull) << 24) |
           ((value & 0x00000000ff000000ull) << 8) |
           ((value & 0x000000ff00000000ull) >> 8) |
           ((value & 0x0000ff0000000000ull) >> 24) |
           ((value & 0x00ff000000000000ull) >> 40) |
           ((value & 0xff00000000000000ull) >> 56);
  }

  void transform(const std::uint8_t *chunk)
  {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
      std::uint32_t word = 0;
      std::memcpy(&word, chunk + (index * 4), sizeof(word));
      schedule[index] = byteswap32(word);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
      const auto s0 = rotate_right(schedule[index - 15], 7) ^ rotate_right(schedule[index - 15], 18) ^
                      (schedule[index - 15] >> 3);
      const auto s1 = rotate_right(schedule[index - 2], 17) ^ rotate_right(schedule[index - 2], 19) ^
                      (schedule[index - 2] >> 10);
      schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < schedule.size(); ++index) {
      const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temp1 = h + s1 + choose + kTable[index] + schedule[index];
      const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667,
      0xbb67ae85,
      0x3c6ef372,
      0xa54ff53a,
      0x510e527f,
      0x9b05688c,
      0x1f83d9ab,
      0x5be0cd19,
  };
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_ = 0;
  std::uint64_t bit_count_ = 0;
};

std::string sha256_bytes(const std::vector<std::uint8_t> &bytes)
{
  Sha256 sha256;
  sha256.update(bytes);
  return sha256.final_hex();
}

std::string sha256_text(std::string_view text)
{
  Sha256 sha256;
  sha256.update(text);
  return sha256.final_hex();
}

std::string sha256_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  Sha256 sha256;
  std::array<std::uint8_t, 4096> buffer{};
  while (input.good()) {
    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = static_cast<std::size_t>(input.gcount());
    if (read_count != 0) {
      sha256.update(buffer.data(), read_count);
    }
  }
  return sha256.final_hex();
}

std::string json_escape(std::string_view text)
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
        std::ostringstream codepoint;
        codepoint << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
        escaped += codepoint.str();
      } else {
        escaped.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  return escaped;
}

std::string api_name(ApiKind api)
{
  switch (api) {
  case ApiKind::D3D11:
    return "D3D11";
  case ApiKind::D3D12:
    return "D3D12";
  default:
    return "Unknown";
  }
}

std::string boundary_name(BoundaryKind boundary)
{
  switch (boundary) {
  case BoundaryKind::Frame:
    return "Frame";
  case BoundaryKind::CommandList:
    return "CommandList";
  case BoundaryKind::Submit:
    return "Submit";
  case BoundaryKind::Present:
    return "Present";
  case BoundaryKind::Fence:
    return "Fence";
  case BoundaryKind::Barrier:
    return "Barrier";
  case BoundaryKind::DebugMarker:
    return "DebugMarker";
  }
  return "Unknown";
}

std::string object_kind_name(ObjectKind kind)
{
  switch (kind) {
  case ObjectKind::Device:
    return "Device";
  case ObjectKind::Context:
    return "Context";
  case ObjectKind::CommandQueue:
    return "CommandQueue";
  case ObjectKind::CommandAllocator:
    return "CommandAllocator";
  case ObjectKind::CommandList:
    return "CommandList";
  case ObjectKind::CommandSignature:
    return "CommandSignature";
  case ObjectKind::Fence:
    return "Fence";
  case ObjectKind::SwapChain:
    return "SwapChain";
  case ObjectKind::Resource:
    return "Resource";
  case ObjectKind::View:
    return "View";
  case ObjectKind::Shader:
    return "Shader";
  case ObjectKind::PipelineState:
    return "PipelineState";
  case ObjectKind::RootSignature:
    return "RootSignature";
  case ObjectKind::DescriptorHeap:
    return "DescriptorHeap";
  case ObjectKind::Unknown:
  default:
    return "Unknown";
  }
}

std::string asset_extension(AssetKind kind)
{
  switch (kind) {
  case AssetKind::ShaderDxbc:
    return ".dxbc";
  case AssetKind::ShaderDxil:
    return ".dxil";
  case AssetKind::RootSignature:
    return ".rootsig";
  case AssetKind::Texture:
    return ".texture";
  case AssetKind::Buffer:
    return ".buffer";
  case AssetKind::Pipeline:
    return ".pipeline.json";
  case AssetKind::ObjectIndex:
    return ".json";
  case AssetKind::Analysis:
    return ".jsonl";
  case AssetKind::Unknown:
  default:
    return ".bin";
  }
}

std::filesystem::path asset_directory(const BundleLayout &layout, AssetKind kind)
{
  switch (kind) {
  case AssetKind::ShaderDxbc:
  case AssetKind::ShaderDxil:
  case AssetKind::RootSignature:
    return layout.shaders_directory_path;
  case AssetKind::Texture:
    return layout.textures_directory_path;
  case AssetKind::Buffer:
    return layout.buffers_directory_path;
  case AssetKind::Pipeline:
    return layout.pipelines_directory_path;
  case AssetKind::ObjectIndex:
    return layout.objects_directory_path;
  case AssetKind::Analysis:
    return layout.analysis_directory_path;
  case AssetKind::Unknown:
  default:
    return layout.root_path;
  }
}

std::string asset_directory_name(AssetKind kind)
{
  switch (kind) {
  case AssetKind::ShaderDxbc:
  case AssetKind::ShaderDxil:
  case AssetKind::RootSignature:
    return kShadersDirectoryName;
  case AssetKind::Texture:
    return kTexturesDirectoryName;
  case AssetKind::Buffer:
    return kBuffersDirectoryName;
  case AssetKind::Pipeline:
    return kPipelinesDirectoryName;
  case AssetKind::ObjectIndex:
    return kObjectsDirectoryName;
  case AssetKind::Analysis:
    return kAnalysisDirectoryName;
  case AssetKind::Unknown:
  default:
    return ".";
  }
}

std::string encode_integer_array(const std::vector<std::uint64_t> &values)
{
  std::ostringstream output;
  output << "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ",";
    }
    output << values[index];
  }
  output << "]";
  return output.str();
}

std::string normalize_payload(std::string_view payload)
{
  if (payload.empty()) {
    return "{}";
  }
  return std::string(payload);
}

std::vector<std::filesystem::path> collect_bundle_relative_paths(const BundleLayout &layout)
{
  std::vector<std::filesystem::path> relative_paths;
  if (!std::filesystem::exists(layout.root_path)) {
    return relative_paths;
  }

  for (const auto &entry : std::filesystem::recursive_directory_iterator(layout.root_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    const auto relative_path = std::filesystem::relative(entry.path(), layout.root_path);
    if (relative_path == std::filesystem::path(kChecksumsFileName)) {
      continue;
    }
    relative_paths.push_back(relative_path);
  }

  std::sort(relative_paths.begin(), relative_paths.end());
  relative_paths.erase(std::unique(relative_paths.begin(), relative_paths.end()), relative_paths.end());
  return relative_paths;
}

std::string event_record_json(const EventRecord &event)
{
  std::ostringstream output;
  if (event.kind == EventKind::Boundary) {
    output << "{\"record_kind\":\"boundary\""
           << ",\"sequence\":" << event.callsite.sequence
           << ",\"boundary\":\"" << boundary_name(event.boundary) << "\""
           << ",\"payload\":" << normalize_payload(event.payload)
           << "}";
    return output.str();
  }

  if (event.kind == EventKind::ObjectCreate || event.kind == EventKind::ObjectDestroy || event.kind == EventKind::ResourceBlob) {
    output << "{\"record_kind\":\""
           << (event.kind == EventKind::ObjectCreate ? "object_create" :
               event.kind == EventKind::ObjectDestroy ? "object_destroy" : "resource_blob")
           << "\""
           << ",\"sequence\":" << event.callsite.sequence
           << ",\"object_id\":" << event.object_id
           << ",\"object_kind\":\"" << object_kind_name(event.object_kind) << "\""
           << ",\"parent_object_id\":" << event.parent_object_id
           << ",\"debug_name\":\"" << json_escape(event.object_debug_name) << "\""
           << ",\"object_refs\":" << encode_integer_array(
                  std::vector<std::uint64_t>(event.object_refs.begin(), event.object_refs.end()))
           << ",\"blob_refs\":" << encode_integer_array(
                  std::vector<std::uint64_t>(event.blob_refs.begin(), event.blob_refs.end()));
    if (!event.payload.empty() && event.payload != "{}") {
      output << ",\"payload\":" << normalize_payload(event.payload);
    }
    output << "}";
    return output.str();
  }

  output << "{\"record_kind\":\"call\""
         << ",\"sequence\":" << event.callsite.sequence
         << ",\"function\":\"" << json_escape(event.callsite.function_name) << "\""
         << ",\"result_code\":" << event.callsite.result_code
         << ",\"object_refs\":" << encode_integer_array(
                std::vector<std::uint64_t>(event.object_refs.begin(), event.object_refs.end()))
         << ",\"blob_refs\":" << encode_integer_array(std::vector<std::uint64_t>(event.blob_refs.begin(), event.blob_refs.end()))
         << ",\"payload\":" << normalize_payload(event.payload)
         << "}";
  return output.str();
}

std::string checksum_index_json(const ChecksumIndex &checksums)
{
  std::ostringstream output;
  output << "{\n"
         << "  \"format_version\": " << checksums.format_version << ",\n"
         << "  \"bundle_hash\": \"" << json_escape(checksums.bundle_hash) << "\",\n"
         << "  \"files\": {\n";
  for (std::size_t index = 0; index < checksums.files.size(); ++index) {
    const auto &record = checksums.files[index];
    output << "    \"" << json_escape(record.relative_path.generic_string()) << "\": \""
           << json_escape(record.algorithm + ":" + record.digest) << "\"";
    if (index + 1 != checksums.files.size()) {
      output << ",";
    }
    output << "\n";
  }
  output << "  }\n"
         << "}\n";
  return output.str();
}

std::string object_index_json(const std::vector<ObjectRecord> &objects)
{
  std::ostringstream output;
  output << "{\n"
         << "  \"objects\": [\n";
  for (std::size_t index = 0; index < objects.size(); ++index) {
    const auto &object = objects[index];
    output << "    {\n"
           << "      \"object_id\": " << object.object_id << ",\n"
           << "      \"object_kind\": \"" << object_kind_name(object.kind) << "\",\n"
           << "      \"parent_object_id\": " << object.parent_object_id << ",\n"
           << "      \"debug_name\": \"" << json_escape(object.debug_name) << "\"\n"
           << "    }";
    if (index + 1 != objects.size()) {
      output << ",";
    }
    output << "\n";
  }
  output << "  ]\n"
         << "}\n";
  return output.str();
}

std::filesystem::path analysis_path_for_stream(const BundleLayout &layout, std::string_view stream_name)
{
  std::filesystem::path name(stream_name);
  if (!name.has_extension()) {
    name += ".jsonl";
  }
  return layout.analysis_directory_path / name;
}

} // namespace

struct TraceBundleWriter::Impl {
  BundleLayout layout;
  TraceMetadata metadata;
  std::vector<EventRecord> events;
  std::vector<MetalEventRecord> metal_events;
  std::vector<AssetRecord> assets;
  std::vector<AssetRecord> metal_assets;
  std::vector<ObjectRecord> objects;
  std::vector<std::string> analysis_streams;
  std::vector<AnalysisRecord> analysis_records;
  ChecksumIndex checksums;
  std::ofstream callstream_stream;
  std::ofstream metal_callstream_stream;
  std::unordered_map<std::string, std::ofstream> analysis_stream_files;
  std::unordered_set<std::string> written_files;
  bool open = false;
  bool metadata_written = false;
  TraceBundleOpenMode open_mode = TraceBundleOpenMode::Primary;

  // TODO: split buffered readable indexes from buffered raw asset writes once persistence begins.
  // TODO: add explicit writer-phase state so open/write/close sequencing can be validated.
};

TraceBundleWriter::TraceBundleWriter() : impl_(std::make_unique<Impl>()) {}

TraceBundleWriter::~TraceBundleWriter() = default;

bool TraceBundleWriter::open(const std::filesystem::path &bundle_root, TraceBundleOpenMode mode)
{
  impl_ = std::make_unique<Impl>();
  impl_->open_mode = mode;
  impl_->layout.root_path = bundle_root;
  impl_->layout.callstream_path = bundle_root / kCallstreamFileName;
  impl_->layout.metal_callstream_path = bundle_root / kMetalCallstreamFileName;
  impl_->layout.checksums_path = bundle_root / kChecksumsFileName;
  impl_->layout.analysis_directory_path = bundle_root / kAnalysisDirectoryName;
  impl_->layout.translation_links_path = impl_->layout.analysis_directory_path / kTranslationLinksFileName;
  impl_->layout.objects_directory_path = bundle_root / kObjectsDirectoryName;
  impl_->layout.object_index_path = impl_->layout.objects_directory_path / kObjectIndexFileName;
  impl_->layout.shaders_directory_path = bundle_root / kShadersDirectoryName;
  impl_->layout.textures_directory_path = bundle_root / kTexturesDirectoryName;
  impl_->layout.buffers_directory_path = bundle_root / kBuffersDirectoryName;
  impl_->layout.pipelines_directory_path = bundle_root / kPipelinesDirectoryName;
  impl_->layout.metal_directory_path = bundle_root / kMetalDirectoryName;
  impl_->layout.metal_libraries_directory_path = impl_->layout.metal_directory_path / kMetalLibrariesDirectoryName;
  impl_->layout.metal_pipelines_directory_path = impl_->layout.metal_directory_path / kMetalPipelinesDirectoryName;
  impl_->layout.metal_buffers_directory_path = impl_->layout.metal_directory_path / kMetalBuffersDirectoryName;
  impl_->layout.metal_textures_directory_path = impl_->layout.metal_directory_path / kMetalTexturesDirectoryName;

  std::filesystem::create_directories(impl_->layout.root_path);
  std::filesystem::create_directories(impl_->layout.analysis_directory_path);
  std::filesystem::create_directories(impl_->layout.objects_directory_path);
  std::filesystem::create_directories(impl_->layout.shaders_directory_path);
  std::filesystem::create_directories(impl_->layout.textures_directory_path);
  std::filesystem::create_directories(impl_->layout.buffers_directory_path);
  std::filesystem::create_directories(impl_->layout.pipelines_directory_path);
  std::filesystem::create_directories(impl_->layout.metal_libraries_directory_path);
  std::filesystem::create_directories(impl_->layout.metal_pipelines_directory_path);
  std::filesystem::create_directories(impl_->layout.metal_buffers_directory_path);
  std::filesystem::create_directories(impl_->layout.metal_textures_directory_path);

  if (mode == TraceBundleOpenMode::Primary) {
    impl_->callstream_stream.open(impl_->layout.callstream_path, std::ios::binary | std::ios::trunc);
    impl_->open = impl_->callstream_stream.is_open();
  } else {
    impl_->open = true;
  }
  if (impl_->open && mode == TraceBundleOpenMode::Primary) {
    write_object_index({});
  }
  return impl_->open;
}

void TraceBundleWriter::write_metadata(const TraceMetadata &metadata)
{
  impl_->metadata = metadata;
  if (!impl_->open || impl_->metadata_written || !impl_->callstream_stream.is_open()) {
    return;
  }

  impl_->callstream_stream << "{\"record_kind\":\"bundle_header\""
                           << ",\"format_version\":" << metadata.format_version
                           << ",\"api\":\"" << api_name(metadata.api) << "\""
                           << ",\"producer\":\"" << json_escape(metadata.producer) << "\""
                           << ",\"has_metal_callstream\":" << (metadata.has_metal_callstream ? "true" : "false")
                           << ",\"entry_file\":\"" << kCallstreamFileName << "\""
                           << "}\n";
  impl_->metadata_written = true;
}

void TraceBundleWriter::append_call_event(const EventRecord &event)
{
  impl_->events.push_back(event);
  if (!impl_->open || !impl_->callstream_stream.is_open()) {
    return;
  }

  if (!impl_->metadata_written) {
    write_metadata(impl_->metadata);
  }

  impl_->callstream_stream << event_record_json(event) << "\n";
}

void TraceBundleWriter::append_metal_event(const MetalEventRecord &event)
{
  impl_->metal_events.push_back(event);
  if (!impl_->open) {
    return;
  }

  if (!impl_->metal_callstream_stream.is_open()) {
    impl_->metal_callstream_stream.open(impl_->layout.metal_callstream_path, std::ios::binary | std::ios::trunc);
  }
  impl_->metal_callstream_stream << detail::metal_event_record_json(event) << "\n";
}

AssetRecord TraceBundleWriter::register_asset(const AssetRecord &asset)
{
  AssetRecord finalized = asset;
  if (finalized.payload_bytes.empty()) {
    impl_->assets.push_back(finalized);
    return finalized;
  }

  const auto digest = sha256_bytes(finalized.payload_bytes);
  if (finalized.relative_path.empty()) {
    finalized.relative_path =
        std::filesystem::path(asset_directory_name(finalized.kind)) / (digest + asset_extension(finalized.kind));
  }

  const auto target_path = impl_->layout.root_path / finalized.relative_path;
  std::filesystem::create_directories(target_path.parent_path());
  if (!std::filesystem::exists(target_path)) {
    std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char *>(finalized.payload_bytes.data()),
        static_cast<std::streamsize>(finalized.payload_bytes.size()));
  }

  impl_->assets.push_back(finalized);
  finalized.payload_bytes.clear();
  return finalized;
}

AssetRecord TraceBundleWriter::register_metal_asset(MetalAssetKind kind, const AssetRecord &asset)
{
  AssetRecord finalized = asset;
  if (finalized.payload_bytes.empty()) {
    impl_->metal_assets.push_back(finalized);
    return finalized;
  }

  const auto digest = sha256_bytes(finalized.payload_bytes);
  if (finalized.relative_path.empty()) {
    finalized.relative_path = std::filesystem::path(detail::metal_asset_directory_name(kind)) /
                              (digest + detail::metal_asset_extension(kind));
  }

  const auto target_path = impl_->layout.root_path / finalized.relative_path;
  std::filesystem::create_directories(target_path.parent_path());
  if (!std::filesystem::exists(target_path)) {
    std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char *>(finalized.payload_bytes.data()),
        static_cast<std::streamsize>(finalized.payload_bytes.size()));
  }

  impl_->metal_assets.push_back(finalized);
  finalized.payload_bytes.clear();
  return finalized;
}

void TraceBundleWriter::write_object_index(const std::vector<ObjectRecord> &objects)
{
  impl_->objects = objects;
  if (!impl_->open) {
    return;
  }

  std::filesystem::create_directories(impl_->layout.object_index_path.parent_path());
  std::ofstream output(impl_->layout.object_index_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return;
  }
  output << object_index_json(objects);
  output.flush();
}

void TraceBundleWriter::declare_analysis_stream(std::string_view stream_name)
{
  if (stream_name.empty()) {
    return;
  }

  const auto already_declared = std::find_if(
      impl_->analysis_streams.begin(),
      impl_->analysis_streams.end(),
      [stream_name](const std::string &existing_stream_name) {
        return existing_stream_name == stream_name;
      }) != impl_->analysis_streams.end();
  if (!already_declared) {
    impl_->analysis_streams.emplace_back(stream_name);
  }
}

void TraceBundleWriter::append_analysis_record(const AnalysisRecord &record)
{
  impl_->analysis_records.push_back(record);
  if (!impl_->open || record.stream_name.empty()) {
    return;
  }

  declare_analysis_stream(record.stream_name);
  const auto stream_key = std::string(record.stream_name);
  auto &stream = impl_->analysis_stream_files[stream_key];
  if (!stream.is_open()) {
    stream.open(analysis_path_for_stream(impl_->layout, stream_key), std::ios::binary | std::ios::app);
  }
  stream << "{\"record_type\":\"" << json_escape(record.record_type) << "\",\"payload\":"
         << normalize_payload(record.payload) << "}\n";
}

void TraceBundleWriter::write_checksum_index(const ChecksumIndex &checksums)
{
  impl_->checksums = checksums;
}

void TraceBundleWriter::close()
{
  if (!impl_->open) {
    return;
  }

  for (auto &entry : impl_->analysis_stream_files) {
    if (entry.second.is_open()) {
      entry.second.flush();
      entry.second.close();
    }
  }

  if (impl_->callstream_stream.is_open()) {
    impl_->callstream_stream.flush();
    impl_->callstream_stream.close();
  }
  if (impl_->metal_callstream_stream.is_open()) {
    impl_->metal_callstream_stream.flush();
    impl_->metal_callstream_stream.close();
  }

  ChecksumIndex checksums = impl_->checksums;
  if (checksums.files.empty()) {
    checksums.format_version = impl_->metadata.format_version;
    const auto relative_paths = collect_bundle_relative_paths(impl_->layout);

    std::string bundle_fingerprint_source;
    for (const auto &relative_path : relative_paths) {
      const auto absolute_path = impl_->layout.root_path / relative_path;
      if (!std::filesystem::exists(absolute_path) || std::filesystem::is_directory(absolute_path)) {
        continue;
      }
      ChecksumRecord file_record;
      file_record.relative_path = relative_path;
      file_record.digest = sha256_file(absolute_path);
      checksums.files.push_back(file_record);
      bundle_fingerprint_source += relative_path.generic_string();
      bundle_fingerprint_source += "=";
      bundle_fingerprint_source += file_record.digest;
      bundle_fingerprint_source += "\n";
    }
    checksums.bundle_hash = "sha256:" + sha256_text(bundle_fingerprint_source);
  }

  std::ofstream checksum_output(impl_->layout.checksums_path, std::ios::binary | std::ios::trunc);
  checksum_output << checksum_index_json(checksums);
  checksum_output.close();

  impl_->checksums = checksums;
  impl_->open = false;
}

bool TraceBundleWriter::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const BundleLayout &TraceBundleWriter::layout() const noexcept
{
  return impl_->layout;
}

} // namespace apitrace::trace
