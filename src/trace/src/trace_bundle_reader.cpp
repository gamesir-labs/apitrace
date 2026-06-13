#include "apitrace/trace_bundle_io.hpp"
#include "metal_callstream_writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
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

using json = nlohmann::json;

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

std::string sha256_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  Sha256 sha256;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  while (input.good()) {
    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = static_cast<std::size_t>(input.gcount());
    if (read_count != 0) {
      sha256.update(buffer.data(), read_count);
    }
  }
  return sha256.final_hex();
}

std::string sha256_file_prefix(const std::filesystem::path &path, std::uint64_t byte_size)
{
  std::ifstream input(path, std::ios::binary);
  Sha256 sha256;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  std::uint64_t remaining = byte_size;
  while (input.good() && remaining != 0) {
    const auto chunk_size = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
    input.read(reinterpret_cast<char *>(buffer.data()), chunk_size);
    const auto read_count = static_cast<std::size_t>(input.gcount());
    if (read_count != 0) {
      sha256.update(buffer.data(), read_count);
      remaining -= read_count;
    }
  }
  return sha256.final_hex();
}

std::uint64_t file_size_or_max(const std::filesystem::path &path)
{
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(size);
}

std::optional<ApiKind> api_kind_from_name(std::string_view name)
{
  if (name == "D3D11") {
    return ApiKind::D3D11;
  }
  if (name == "D3D12") {
    return ApiKind::D3D12;
  }
  if (name == "Unknown") {
    return ApiKind::Unknown;
  }
  return std::nullopt;
}

std::optional<ObjectKind> object_kind_from_name(std::string_view name)
{
  if (name == "Unknown") {
    return ObjectKind::Unknown;
  }
  if (name == "Device") {
    return ObjectKind::Device;
  }
  if (name == "Context") {
    return ObjectKind::Context;
  }
  if (name == "CommandQueue") {
    return ObjectKind::CommandQueue;
  }
  if (name == "CommandAllocator") {
    return ObjectKind::CommandAllocator;
  }
  if (name == "CommandList") {
    return ObjectKind::CommandList;
  }
  if (name == "CommandSignature") {
    return ObjectKind::CommandSignature;
  }
  if (name == "Fence") {
    return ObjectKind::Fence;
  }
  if (name == "SwapChain") {
    return ObjectKind::SwapChain;
  }
  if (name == "Heap") {
    return ObjectKind::Heap;
  }
  if (name == "Resource") {
    return ObjectKind::Resource;
  }
  if (name == "View") {
    return ObjectKind::View;
  }
  if (name == "Shader") {
    return ObjectKind::Shader;
  }
  if (name == "PipelineState") {
    return ObjectKind::PipelineState;
  }
  if (name == "RootSignature") {
    return ObjectKind::RootSignature;
  }
  if (name == "DescriptorHeap") {
    return ObjectKind::DescriptorHeap;
  }
  if (name == "QueryHeap") {
    return ObjectKind::QueryHeap;
  }
  return std::nullopt;
}

std::optional<BoundaryKind> boundary_kind_from_name(std::string_view name)
{
  if (name == "Frame") {
    return BoundaryKind::Frame;
  }
  if (name == "CommandList") {
    return BoundaryKind::CommandList;
  }
  if (name == "Submit") {
    return BoundaryKind::Submit;
  }
  if (name == "Present") {
    return BoundaryKind::Present;
  }
  if (name == "Fence") {
    return BoundaryKind::Fence;
  }
  if (name == "Barrier") {
    return BoundaryKind::Barrier;
  }
  if (name == "DebugMarker") {
    return BoundaryKind::DebugMarker;
  }
  return std::nullopt;
}

bool ends_with(std::string_view text, std::string_view suffix)
{
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string file_label(const std::filesystem::path &path)
{
  return path.filename().generic_string();
}

void collect_asset_paths(const json &value, std::vector<std::string> &paths)
{
  if (value.is_object()) {
    for (const auto &[key, child] : value.items()) {
      if ((ends_with(key, "_path") || key == "path") && child.is_string()) {
        const auto path = child.get<std::string>();
        if (!path.empty())
          paths.push_back(path);
      }
      collect_asset_paths(child, paths);
    }
    return;
  }

  if (value.is_array()) {
    for (const auto &child : value) {
      collect_asset_paths(child, paths);
    }
  }
}

void collect_nested_asset_paths(const json &value, std::vector<std::string> &paths)
{
  if (value.is_object()) {
    if (const auto type = value.find("type"); type != value.end() && type->is_string() &&
        (*type == "graphics" || *type == "compute")) {
      for (std::string_view stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
        const auto shader = value.find(std::string(stage));
        if (shader == value.end() || !shader->is_object()) {
          continue;
        }
        const auto path_key = std::string(stage) + "_path";
        const auto path = shader->find(path_key);
        if (path != shader->end() && path->is_string()) {
          const auto path_text = path->get<std::string>();
          if (!path_text.empty())
            paths.push_back(path_text);
        }
      }
      return;
    }

    for (const auto &[key, child] : value.items()) {
      if ((ends_with(key, "_path") || key == "path") && child.is_string()) {
        const auto path = child.get<std::string>();
        if (!path.empty())
          paths.push_back(path);
      }
      collect_nested_asset_paths(child, paths);
    }
    return;
  }

  if (value.is_array()) {
    for (const auto &child : value) {
      collect_nested_asset_paths(child, paths);
    }
  }
}

AssetKind asset_kind_from_name(std::string_view name)
{
  if (name == "ShaderDxbc") {
    return AssetKind::ShaderDxbc;
  }
  if (name == "ShaderDxil") {
    return AssetKind::ShaderDxil;
  }
  if (name == "RootSignature") {
    return AssetKind::RootSignature;
  }
  if (name == "Texture") {
    return AssetKind::Texture;
  }
  if (name == "Buffer") {
    return AssetKind::Buffer;
  }
  if (name == "Pipeline") {
    return AssetKind::Pipeline;
  }
  if (name == "ObjectIndex") {
    return AssetKind::ObjectIndex;
  }
  if (name == "Analysis") {
    return AssetKind::Analysis;
  }
  return AssetKind::Unknown;
}

bool is_safe_bundle_relative_path(const std::filesystem::path &relative_path)
{
  if (relative_path.empty() || relative_path.is_absolute()) {
    return false;
  }
  for (const auto &part : relative_path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

template <typename Integer>
bool append_integer_array(
    const json &value,
    std::vector<Integer> &output,
    const std::filesystem::path &path,
    std::uint64_t sequence,
    const char *field_name,
    std::string &error)
{
  if (!value.is_array()) {
    std::ostringstream message;
    message << file_label(path) << ": sequence " << sequence << " field " << field_name << " must be an array";
    error = message.str();
    return false;
  }

  output.clear();
  output.reserve(value.size());
  for (const auto &item : value) {
    if (!item.is_number_unsigned() && !item.is_number_integer()) {
      std::ostringstream message;
      message << file_label(path) << ": sequence " << sequence << " field " << field_name << " contains a non-integer";
      error = message.str();
      return false;
    }
    output.push_back(static_cast<Integer>(item.get<std::uint64_t>()));
  }
  return true;
}

bool parse_checksums(
    const std::filesystem::path &checksums_path,
    ChecksumIndex &checksums,
    std::unordered_map<std::string, ChecksumRecord> &lookup,
    std::string &error)
{
  std::ifstream input(checksums_path);
  if (!input.is_open()) {
    error = "missing required file: " + file_label(checksums_path);
    return false;
  }

  json parsed = json::parse(input, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    error = file_label(checksums_path) + ": invalid JSON";
    return false;
  }

  checksums = ChecksumIndex{};
  checksums.format_version = parsed.value("format_version", kFormatVersion);
  checksums.bundle_hash = parsed.value("bundle_hash", "");

  const auto files_it = parsed.find("files");
  if (files_it == parsed.end() || !files_it->is_object()) {
    error = file_label(checksums_path) + ": files must be an object";
    return false;
  }

  lookup.clear();
  for (const auto &[relative_path, digest_value] : files_it->items()) {
    if (!digest_value.is_string() && !digest_value.is_object()) {
      error = file_label(checksums_path) + ": file digest must be a string";
      return false;
    }
    const std::string encoded =
        digest_value.is_string()
            ? digest_value.get<std::string>()
            : digest_value.value("digest", std::string());
    const auto separator = encoded.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= encoded.size()) {
      error = file_label(checksums_path) + ": malformed digest entry for " + relative_path;
      return false;
    }

    ChecksumRecord record;
    record.relative_path = relative_path;
    record.algorithm = encoded.substr(0, separator);
    const auto size_separator = encoded.find(':', separator + 1);
    record.digest = encoded.substr(
        separator + 1,
        size_separator == std::string::npos ? std::string::npos : size_separator - separator - 1);
    if (size_separator != std::string::npos) {
      try {
        record.byte_size = std::stoull(encoded.substr(size_separator + 1));
        record.has_byte_size = true;
      } catch (const std::exception &) {
        error = file_label(checksums_path) + ": malformed byte size for " + relative_path;
        return false;
      }
    }
    if (digest_value.is_object()) {
      if (digest_value.contains("byte_size") && digest_value["byte_size"].is_number_unsigned()) {
        record.byte_size = digest_value["byte_size"].get<std::uint64_t>();
        record.has_byte_size = true;
      }
    }
    checksums.files.push_back(record);
    lookup.emplace(record.relative_path.generic_string(), record);
  }
  return true;
}

bool parse_objects(
    const std::filesystem::path &objects_path,
    std::vector<ObjectRecord> &objects,
    std::string &error)
{
  std::ifstream input(objects_path);
  if (!input.is_open()) {
    error = "missing required file: " + file_label(objects_path);
    return false;
  }

  json parsed = json::parse(input, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    error = file_label(objects_path) + ": invalid JSON";
    return false;
  }

  const auto objects_it = parsed.find("objects");
  if (objects_it == parsed.end() || !objects_it->is_array()) {
    error = file_label(objects_path) + ": objects must be an array";
    return false;
  }

  objects.clear();
  objects.reserve(objects_it->size());
  for (const auto &entry : *objects_it) {
    if (!entry.is_object()) {
      error = file_label(objects_path) + ": object entry must be an object";
      return false;
    }

    ObjectRecord record;
    record.object_id = entry.value("object_id", 0ull);
    record.parent_object_id = entry.value("parent_object_id", 0ull);
    record.debug_name = entry.value("debug_name", "");

    const auto kind_name = entry.value("object_kind", std::string("Unknown"));
    const auto kind = object_kind_from_name(kind_name);
    if (!kind.has_value()) {
      error = file_label(objects_path) + ": unknown object_kind " + kind_name;
      return false;
    }
    record.kind = *kind;
    objects.push_back(std::move(record));
  }
  return true;
}

bool parse_asset_index(
    const std::filesystem::path &asset_index_path,
    std::vector<AssetRecord> &assets,
    std::vector<AssetRecord> &metal_assets,
    std::unordered_map<BlobId, std::filesystem::path> &indexed_blob_paths,
    std::string &error)
{
  assets.clear();
  metal_assets.clear();
  indexed_blob_paths.clear();

  if (!std::filesystem::is_regular_file(asset_index_path)) {
    return true;
  }

  std::ifstream input(asset_index_path);
  if (!input.is_open()) {
    error = "failed to open asset index: " + file_label(asset_index_path);
    return false;
  }
  json root = json::parse(input, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    error = file_label(asset_index_path) + ": invalid JSON";
    return false;
  }
  const auto list = root.find("assets");
  if (list == root.end() || !list->is_array()) {
    error = file_label(asset_index_path) + ": assets must be an array";
    return false;
  }

  for (const auto &entry : *list) {
    if (!entry.is_object()) {
      error = file_label(asset_index_path) + ": asset entry must be an object";
      return false;
    }
    AssetRecord asset;
    asset.blob_id = entry.value("blob_id", 0ull);
    asset.relative_path = entry.value("path", std::string());
    asset.kind = asset_kind_from_name(entry.value("kind", std::string("Unknown")));
    asset.debug_name = entry.value("debug_name", std::string());
    asset.content_hash = entry.value("content_hash", std::string());
    asset.fast_fingerprint = entry.value("fast_fingerprint", std::string());
    asset.byte_size = entry.value("byte_size", 0ull);
    asset.binary_payload = entry.value("binary_payload", true);
    if (asset.blob_id == 0 || asset.relative_path.empty()) {
      error = file_label(asset_index_path) + ": asset entry missing blob_id or path";
      return false;
    }
    if (!is_safe_bundle_relative_path(asset.relative_path)) {
      error = file_label(asset_index_path) + ": unsafe asset path " + asset.relative_path.generic_string();
      return false;
    }
    const auto [blob_it, inserted] = indexed_blob_paths.emplace(asset.blob_id, asset.relative_path);
    if (!inserted && blob_it->second != asset.relative_path) {
      error = file_label(asset_index_path) + ": blob_id maps to multiple paths: " + std::to_string(asset.blob_id);
      return false;
    }
    if (entry.value("metal", false)) {
      metal_assets.push_back(std::move(asset));
    } else {
      assets.push_back(std::move(asset));
    }
  }
  return true;
}

bool parse_bundle_header(
    const json &record,
    const std::filesystem::path &callstream_path,
    TraceMetadata &metadata,
    std::string &error)
{
  if (!record.is_object()) {
    error = file_label(callstream_path) + ": bundle header must be an object";
    return false;
  }

  metadata.format_version = record.value("format_version", kFormatVersion);
  metadata.producer = record.value("producer", "");
  metadata.has_metal_callstream = record.value("has_metal_callstream", false);
  const auto api_name = record.value("api", std::string("Unknown"));
  const auto api = api_kind_from_name(api_name);
  if (!api.has_value()) {
    error = file_label(callstream_path) + ": unknown bundle api " + api_name;
    return false;
  }
  metadata.api = *api;
  return true;
}

std::uint64_t json_u64_field(const json &value, const char *name, std::uint64_t fallback = 0)
{
  const auto it = value.find(name);
  if (it == value.end()) {
    return fallback;
  }
  if (it->is_number_unsigned()) {
    return it->get<std::uint64_t>();
  }
  if (it->is_number_integer()) {
    const auto signed_value = it->get<std::int64_t>();
    return signed_value < 0 ? fallback : static_cast<std::uint64_t>(signed_value);
  }
  return fallback;
}

bool parse_event_record(
    const json &record,
    const std::filesystem::path &callstream_path,
    EventRecord &event,
    std::string &error)
{
  if (!record.is_object()) {
    error = file_label(callstream_path) + ": callstream record must be an object";
    return false;
  }

  event = EventRecord{};
  const auto record_kind = record.value("record_kind", std::string());

  if (record_kind == "call") {
    event.kind = EventKind::Call;
    event.callsite.sequence = record.value("sequence", 0ull);
    event.time_ns = record.value("time_ns", 0ull);
    event.elapsed_ns = record.value("elapsed_ns", 0ull);
    event.callsite.function_name = record.value("function", std::string());
    event.callsite.result_code = record.value("result_code", 0);
    if (event.callsite.sequence == 0 || event.callsite.function_name.empty()) {
      std::ostringstream message;
      message << file_label(callstream_path) << ": call record missing sequence or function";
      error = message.str();
      return false;
    }

    if (!append_integer_array<ObjectId>(
            record.value("object_refs", json::array()),
            event.object_refs,
            callstream_path,
            event.callsite.sequence,
            "object_refs",
            error)) {
      return false;
    }
    if (!append_integer_array<BlobId>(
            record.value("blob_refs", json::array()),
            event.blob_refs,
            callstream_path,
            event.callsite.sequence,
            "blob_refs",
            error)) {
      return false;
    }

    const auto payload_it = record.find("payload");
    if (payload_it == record.end()) {
      std::ostringstream message;
      message << file_label(callstream_path) << ": sequence " << event.callsite.sequence << " missing payload";
      error = message.str();
      return false;
    }
    event.payload = payload_it->dump();
    return true;
  }

  if (record_kind == "object_create" || record_kind == "object_destroy" || record_kind == "resource_blob") {
    event.kind = record_kind == "object_create" ? EventKind::ObjectCreate :
                 record_kind == "object_destroy" ? EventKind::ObjectDestroy :
                                                   EventKind::ResourceBlob;
    event.callsite.sequence = record.value("sequence", 0ull);
    event.time_ns = record.value("time_ns", 0ull);
    event.elapsed_ns = record.value("elapsed_ns", 0ull);
    if (event.callsite.sequence == 0) {
      std::ostringstream message;
      message << file_label(callstream_path) << ": " << record_kind << " record missing sequence";
      error = message.str();
      return false;
    }
    event.callsite.function_name = record_kind;
    event.object_id = record.value("object_id", 0ull);
    event.parent_object_id = record.value("parent_object_id", 0ull);
    event.object_debug_name = record.value("debug_name", std::string());

    const auto kind_name = record.value("object_kind", std::string("Unknown"));
    const auto kind = object_kind_from_name(kind_name);
    if (!kind.has_value()) {
      std::ostringstream message;
      message << file_label(callstream_path) << ": sequence " << event.callsite.sequence
              << " has unknown object_kind " << kind_name;
      error = message.str();
      return false;
    }
    event.object_kind = *kind;

    if (event.kind != EventKind::ResourceBlob && event.object_id == 0) {
      std::ostringstream message;
      message << file_label(callstream_path) << ": sequence " << event.callsite.sequence
              << " missing object_id";
      error = message.str();
      return false;
    }

    if (!append_integer_array<ObjectId>(
            record.value("object_refs", json::array()),
            event.object_refs,
            callstream_path,
            event.callsite.sequence,
            "object_refs",
            error)) {
      return false;
    }
    if (!append_integer_array<BlobId>(
            record.value("blob_refs", json::array()),
            event.blob_refs,
            callstream_path,
            event.callsite.sequence,
            "blob_refs",
            error)) {
      return false;
    }

    const auto payload_it = record.find("payload");
    event.payload = payload_it == record.end() ? std::string("{}") : payload_it->dump();
    return true;
  }

  if (record_kind == "boundary") {
    event.kind = EventKind::Boundary;
    event.callsite.sequence = record.value("sequence", 0ull);
    event.time_ns = record.value("time_ns", 0ull);
    event.elapsed_ns = record.value("elapsed_ns", 0ull);
    const auto boundary_name = record.value("boundary", std::string());
    const auto boundary_kind = boundary_kind_from_name(boundary_name);
    if (!boundary_kind.has_value()) {
      std::ostringstream message;
      message << file_label(callstream_path) << ": sequence " << event.callsite.sequence
              << " has unknown boundary " << boundary_name;
      error = message.str();
      return false;
    }
    event.boundary = *boundary_kind;
    event.callsite.function_name = boundary_name;
    const auto payload_it = record.find("payload");
    event.payload = payload_it == record.end() ? std::string("{}") : payload_it->dump();
    return true;
  }

  std::ostringstream message;
  message << file_label(callstream_path) << ": unsupported record_kind " << record_kind;
  error = message.str();
  return false;
}

bool validate_checksum_entry(
    const std::filesystem::path &bundle_root,
    const std::filesystem::path &relative_path,
    const std::unordered_map<std::string, ChecksumRecord> &lookup,
    const std::filesystem::path &checksums_path,
    std::unordered_set<std::string> &validated_paths,
    std::string &error)
{
  const auto key = relative_path.generic_string();
  if (validated_paths.find(key) != validated_paths.end())
    return true;

  const auto checksum_it = lookup.find(key);
  if (checksum_it == lookup.end()) {
    error = file_label(checksums_path) + ": missing checksum entry for " + key;
    return false;
  }
  if (checksum_it->second.algorithm != "sha256") {
    error = file_label(checksums_path) + ": unsupported checksum algorithm for " + key;
    return false;
  }

  const auto absolute_path = bundle_root / relative_path;
  if (!std::filesystem::is_regular_file(absolute_path)) {
    error = "missing referenced file: " + key;
    return false;
  }

  const auto actual_size = file_size_or_max(absolute_path);
  const auto use_prefix =
      checksum_it->second.has_byte_size &&
      actual_size != std::numeric_limits<std::uint64_t>::max() &&
      actual_size > checksum_it->second.byte_size;
  const auto digest = use_prefix
                          ? sha256_file_prefix(absolute_path, checksum_it->second.byte_size)
                          : sha256_file(absolute_path);
  if (digest != checksum_it->second.digest) {
    error = "checksum mismatch for " + key;
    return false;
  }
  validated_paths.insert(key);
  return true;
}

bool validate_asset_index_record(
    const std::filesystem::path &bundle_root,
    const AssetRecord &asset,
    const std::unordered_map<std::string, ChecksumRecord> &lookup,
    std::unordered_map<std::string, std::uint64_t> &file_sizes,
    std::string &error)
{
  if (asset.relative_path.empty()) {
    return true;
  }
  const auto key = asset.relative_path.generic_string();
  const auto checksum_it = lookup.find(key);
  if (checksum_it == lookup.end()) {
    error = "asset index path missing checksum entry: " + key;
    return false;
  }
  if (!asset.content_hash.empty()) {
    if (checksum_it->second.algorithm != "sha256") {
      error = "asset index content_hash requires sha256 checksum: " + key;
      return false;
    }
    if (checksum_it->second.digest != asset.content_hash) {
      error = "asset index content_hash does not match checksum: " + key;
      return false;
    }
  }
  if (asset.byte_size != 0) {
    std::uint64_t actual_size = 0;
    if (const auto known_size = file_sizes.find(key); known_size != file_sizes.end()) {
      actual_size = known_size->second;
    } else {
      std::error_code stat_error;
      actual_size = std::filesystem::file_size(bundle_root / asset.relative_path, stat_error);
      if (stat_error) {
        error = "failed to stat asset index path: " + key;
        return false;
      }
      file_sizes.emplace(key, actual_size);
    }
    if (actual_size != asset.byte_size) {
      error = "asset index byte_size does not match file size: " + key;
      return false;
    }
  }
  return true;
}

bool read_referenced_asset_json(
    const std::filesystem::path &bundle_root,
    const std::filesystem::path &relative_path,
    std::unordered_map<std::string, json> &cache,
    json &asset_json,
    std::string &error)
{
  const auto key = relative_path.generic_string();
  if (const auto cached = cache.find(key); cached != cache.end()) {
    asset_json = cached->second;
    return true;
  }
  const auto absolute_path = bundle_root / relative_path;
  if (!std::filesystem::is_regular_file(absolute_path)) {
    error = "missing referenced asset file: " + key;
    return false;
  }

  std::ifstream input(absolute_path);
  if (!input.is_open()) {
    error = "failed to open referenced asset file: " + key;
    return false;
  }

  asset_json = json::parse(input, nullptr, false);
  if (asset_json.is_discarded() || !asset_json.is_object()) {
    error = "referenced asset JSON is invalid: " + key;
    return false;
  }
  cache.emplace(key, asset_json);
  return true;
}

} // namespace

struct TraceBundleReader::Impl {
  BundleLayout layout;
  TraceMetadata metadata;
  std::vector<EventRecord> events;
  std::vector<MetalEventRecord> metal_events;
  std::vector<AssetRecord> assets;
  std::vector<AssetRecord> metal_assets;
  std::vector<ObjectRecord> objects;
  ChecksumIndex checksums;
  std::unordered_set<std::string> validated_checksum_paths;
  std::string last_error;
  bool has_asset_index = false;
  bool prefix_limited = false;
  bool open = false;

  // TODO: track reader phases explicitly so validation, parsing, and asset discovery can fail independently.
  // TODO: cache parsed readable indexes separately from raw asset lookup results.
};

TraceBundleReader::TraceBundleReader() : impl_(std::make_unique<Impl>()) {}

TraceBundleReader::~TraceBundleReader() = default;

bool TraceBundleReader::open(const std::filesystem::path &bundle_root)
{
  return open(bundle_root, OpenOptions{});
}

bool TraceBundleReader::open(const std::filesystem::path &bundle_root, const OpenOptions &options)
{
  impl_ = std::make_unique<Impl>();
  impl_->layout.root_path = bundle_root;
  impl_->layout.callstream_path = bundle_root / kCallstreamFileName;
  impl_->layout.metal_callstream_path = bundle_root / kMetalCallstreamFileName;
  impl_->layout.checksums_path = bundle_root / kChecksumsFileName;
  impl_->layout.asset_index_path = bundle_root / kAssetIndexFileName;
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

  for (const auto &required_path : {impl_->layout.callstream_path, impl_->layout.checksums_path, impl_->layout.object_index_path}) {
    if (!std::filesystem::is_regular_file(required_path)) {
      impl_->last_error = "missing required file: " + file_label(required_path);
      return false;
    }
  }

  std::unordered_map<std::string, ChecksumRecord> checksum_lookup;
  if (!parse_checksums(impl_->layout.checksums_path, impl_->checksums, checksum_lookup, impl_->last_error)) {
    return false;
  }

  if (!parse_objects(impl_->layout.object_index_path, impl_->objects, impl_->last_error)) {
    return false;
  }

  std::unordered_map<BlobId, std::filesystem::path> indexed_blob_paths;
  if (!parse_asset_index(
          impl_->layout.asset_index_path,
          impl_->assets,
          impl_->metal_assets,
          indexed_blob_paths,
          impl_->last_error)) {
    return false;
  }
  impl_->has_asset_index = std::filesystem::is_regular_file(impl_->layout.asset_index_path);

  auto checksum_byte_limit = [&](const std::filesystem::path &relative_path) {
    const auto entry = checksum_lookup.find(relative_path.generic_string());
    if (entry == checksum_lookup.end() || !entry->second.has_byte_size) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return entry->second.byte_size;
  };
  const auto metal_callstream_byte_limit =
      checksum_byte_limit(std::filesystem::path(kMetalCallstreamFileName));
  const auto callstream_byte_limit =
      checksum_byte_limit(std::filesystem::path(kCallstreamFileName));
  const bool prefix_limited =
      options.stop_after_sequence != 0 ||
      options.stop_after_present_frame != 0;
  impl_->prefix_limited = prefix_limited;

  const bool has_metal_callstream_file = std::filesystem::is_regular_file(impl_->layout.metal_callstream_path);

  if (options.load_metal_sideband && has_metal_callstream_file &&
      !detail::parse_metal_callstream(
          impl_->layout.metal_callstream_path,
          impl_->metal_events,
          impl_->last_error,
          metal_callstream_byte_limit)) {
    return false;
  }

  std::ifstream callstream_input(impl_->layout.callstream_path);
  if (!callstream_input.is_open()) {
    impl_->last_error = "missing required file: " + file_label(impl_->layout.callstream_path);
    return false;
  }

  std::unordered_map<std::string, std::size_t> asset_indices_by_path;
  std::unordered_map<std::string, std::size_t> metal_asset_indices_by_path;
  std::unordered_set<std::string> asset_blob_keys;
  std::unordered_set<std::string> metal_asset_blob_keys;
  std::unordered_set<std::string> referenced_asset_paths;
  std::unordered_set<std::string> referenced_metal_asset_paths;
  for (std::size_t index = 0; index < impl_->assets.size(); ++index) {
    asset_indices_by_path.emplace(impl_->assets[index].relative_path.generic_string(), index);
    asset_blob_keys.insert(impl_->assets[index].relative_path.generic_string() + "#" + std::to_string(impl_->assets[index].blob_id));
  }
  for (std::size_t index = 0; index < impl_->metal_assets.size(); ++index) {
    metal_asset_indices_by_path.emplace(impl_->metal_assets[index].relative_path.generic_string(), index);
    metal_asset_blob_keys.insert(impl_->metal_assets[index].relative_path.generic_string() + "#" + std::to_string(impl_->metal_assets[index].blob_id));
  }
	  auto register_asset_path = [&](const std::filesystem::path &relative_path, BlobId blob_id, bool metal) -> bool {
	    if (!is_safe_bundle_relative_path(relative_path)) {
	      impl_->last_error = "unsafe asset path reference: " + relative_path.generic_string();
	      return false;
	    }
	    const bool enforce_asset_index = impl_->has_asset_index && !indexed_blob_paths.empty();
	    if (enforce_asset_index && blob_id != 0) {
	      const auto indexed_path = indexed_blob_paths.find(blob_id);
	      if (indexed_path == indexed_blob_paths.end()) {
	        impl_->last_error = "blob_ref missing from asset index: " + std::to_string(blob_id);
        return false;
      }
      if (indexed_path->second != relative_path) {
        impl_->last_error = "blob_ref path mismatch: " + std::to_string(blob_id) +
                            " -> " + indexed_path->second.generic_string() +
                            ", event references " + relative_path.generic_string();
        return false;
      }
    }

    AssetRecord asset;
    asset.blob_id = blob_id;
    asset.relative_path = relative_path;

	    auto &indices_by_path = metal ? metal_asset_indices_by_path : asset_indices_by_path;
	    auto &blob_keys = metal ? metal_asset_blob_keys : asset_blob_keys;
	    auto &asset_list = metal ? impl_->metal_assets : impl_->assets;
	    auto &referenced_paths = metal ? referenced_metal_asset_paths : referenced_asset_paths;
	    const auto key = asset.relative_path.generic_string();
	    referenced_paths.insert(key);
	    const auto blob_key = key + "#" + std::to_string(blob_id);
    if (blob_id != 0 && !blob_keys.insert(blob_key).second) {
      return true;
    }
    const auto existing = indices_by_path.find(key);
    if (existing == indices_by_path.end()) {
      indices_by_path.emplace(key, asset_list.size());
      asset_list.push_back(asset);
    } else if (asset_list[existing->second].blob_id == 0 && asset.blob_id != 0) {
      asset_list[existing->second].blob_id = asset.blob_id;
    } else if (asset.blob_id != 0 && asset_list[existing->second].blob_id != asset.blob_id) {
      asset_list.push_back(asset);
    }
    return true;
  };
  std::unordered_map<std::string, json> referenced_asset_json_cache;
  auto register_referenced_assets = [&](const json &record, const std::vector<BlobId> &blob_refs) -> bool {
    std::vector<std::string> referenced_paths;
    collect_asset_paths(record, referenced_paths);
    std::vector<std::string> nested_referenced_paths;
    for (const auto &referenced_path : referenced_paths) {
      const std::filesystem::path relative_path = referenced_path;
      if (ends_with(relative_path.generic_string(), ".json")) {
        json asset_json;
        if (!read_referenced_asset_json(
                impl_->layout.root_path,
                relative_path,
                referenced_asset_json_cache,
                asset_json,
                impl_->last_error)) {
          return false;
        }
        collect_nested_asset_paths(asset_json, nested_referenced_paths);
      }
    }

    std::vector<std::string> all_paths = referenced_paths;
    all_paths.insert(all_paths.end(), nested_referenced_paths.begin(), nested_referenced_paths.end());
    std::size_t legacy_blob_ref_index = 0;
    for (const auto &path_text : all_paths) {
      const std::filesystem::path relative_path = path_text;
      MetalAssetKind metal_kind = MetalAssetKind::Library;
      BlobId blob_id = {};
      if (impl_->has_asset_index) {
        for (const auto candidate_blob_id : blob_refs) {
          const auto indexed_path = indexed_blob_paths.find(candidate_blob_id);
          if (indexed_path != indexed_blob_paths.end() && indexed_path->second == relative_path) {
            blob_id = candidate_blob_id;
            break;
          }
        }
      }
      if (blob_id == 0 && (!impl_->has_asset_index || indexed_blob_paths.empty()) &&
          legacy_blob_ref_index < blob_refs.size()) {
        blob_id = blob_refs[legacy_blob_ref_index];
      }
      const bool metal_asset = detail::is_metal_asset_path(relative_path, &metal_kind);
      if (!register_asset_path(relative_path, blob_id, metal_asset)) {
        return false;
      }
      ++legacy_blob_ref_index;
    }
    return true;
  };

  if (options.load_metal_sideband && has_metal_callstream_file) {
    std::ifstream metal_input(impl_->layout.metal_callstream_path);
    std::string metal_line;
    std::uint64_t consumed_metal_callstream_bytes = 0;
    while (std::getline(metal_input, metal_line)) {
      const auto line_bytes = static_cast<std::uint64_t>(metal_line.size() + 1);
      if (consumed_metal_callstream_bytes + line_bytes > metal_callstream_byte_limit) {
        break;
      }
      consumed_metal_callstream_bytes += line_bytes;
      if (metal_line.empty()) {
        continue;
      }
      json record = json::parse(metal_line, nullptr, false);
      if (record.is_discarded()) {
        continue;
      }
      std::vector<BlobId> blob_refs;
      if (record.contains("blob_refs") && record["blob_refs"].is_array()) {
        blob_refs = record["blob_refs"].get<std::vector<BlobId>>();
      }
      if (!register_referenced_assets(record, blob_refs)) {
        return false;
      }
    }
  }

  bool header_seen = false;
  std::string line;
  std::size_t line_number = 0;
  std::uint64_t consumed_callstream_bytes = 0;
  while (std::getline(callstream_input, line)) {
    const auto line_bytes = static_cast<std::uint64_t>(line.size() + 1);
    if (consumed_callstream_bytes + line_bytes > callstream_byte_limit) {
      break;
    }
    consumed_callstream_bytes += line_bytes;
    ++line_number;
    if (line.empty()) {
      continue;
    }

    json record = json::parse(line, nullptr, false);
    if (record.is_discarded()) {
      std::ostringstream message;
      message << file_label(impl_->layout.callstream_path) << ": invalid JSON at line " << line_number;
      impl_->last_error = message.str();
      return false;
    }

    const auto record_kind = record.value("record_kind", std::string());
    if (!header_seen) {
      if (record_kind != "bundle_header") {
        impl_->last_error = file_label(impl_->layout.callstream_path) + ": first record must be bundle_header";
        return false;
      }
      if (!parse_bundle_header(record, impl_->layout.callstream_path, impl_->metadata, impl_->last_error)) {
        return false;
      }
      header_seen = true;
      continue;
    }

    EventRecord event;
    if (!parse_event_record(record, impl_->layout.callstream_path, event, impl_->last_error)) {
      return false;
    }
    impl_->events.push_back(event);

    json payload = json::parse(event.payload.empty() ? std::string("{}") : event.payload, nullptr, false);
    if (payload.is_discarded()) {
      std::ostringstream message;
      message << file_label(impl_->layout.callstream_path) << ": sequence " << event.callsite.sequence
              << " has invalid payload JSON";
      impl_->last_error = message.str();
      return false;
    }

    if (!register_referenced_assets(record, event.blob_refs)) {
      return false;
    }

    if (options.stop_after_sequence != 0 &&
        event.callsite.sequence >= options.stop_after_sequence) {
      break;
    }

    if (options.stop_after_present_frame != 0 &&
        event.kind == EventKind::Boundary &&
        event.boundary == BoundaryKind::Present &&
        json_u64_field(payload, "frame_index", std::numeric_limits<std::uint64_t>::max()) >=
            options.stop_after_present_frame) {
      break;
    }
  }

  if (!header_seen) {
    impl_->last_error = file_label(impl_->layout.callstream_path) + ": missing bundle_header";
    return false;
  }
  if (has_metal_callstream_file) {
    impl_->metadata.has_metal_callstream = true;
  }

  std::unordered_set<std::string> validated_checksum_paths;
  std::unordered_map<std::string, std::uint64_t> validated_asset_file_sizes;
  for (const auto &required_relative_path :
       {std::filesystem::path(kCallstreamFileName),
        std::filesystem::path(kObjectsDirectoryName) / kObjectIndexFileName,
        std::filesystem::path(kAssetIndexFileName)}) {
    if (required_relative_path == kAssetIndexFileName && !std::filesystem::is_regular_file(impl_->layout.asset_index_path)) {
      continue;
    }
    if (prefix_limited && required_relative_path == kCallstreamFileName) {
      continue;
    }
    if (!validate_checksum_entry(
            impl_->layout.root_path,
            required_relative_path,
            checksum_lookup,
            impl_->layout.checksums_path,
            validated_checksum_paths,
            impl_->last_error)) {
      return false;
    }
  }

  for (const auto &asset : impl_->assets) {
    if (prefix_limited && referenced_asset_paths.find(asset.relative_path.generic_string()) == referenced_asset_paths.end()) {
      continue;
    }
    if (!validate_asset_index_record(
            impl_->layout.root_path,
            asset,
            checksum_lookup,
            validated_asset_file_sizes,
            impl_->last_error)) {
      return false;
    }
    if (!validate_checksum_entry(
            impl_->layout.root_path,
            asset.relative_path,
            checksum_lookup,
            impl_->layout.checksums_path,
            validated_checksum_paths,
            impl_->last_error)) {
      return false;
    }
  }

  if (options.load_metal_sideband) {
    for (const auto &entry : impl_->checksums.files) {
    MetalAssetKind metal_kind = MetalAssetKind::Library;
    if (!detail::is_metal_asset_path(entry.relative_path, &metal_kind)) {
      continue;
    }

    AssetRecord asset;
    asset.relative_path = entry.relative_path;
    asset.debug_name = entry.relative_path.filename().generic_string();
    if (metal_asset_indices_by_path.find(asset.relative_path.generic_string()) == metal_asset_indices_by_path.end()) {
      metal_asset_indices_by_path.emplace(asset.relative_path.generic_string(), impl_->metal_assets.size());
      impl_->metal_assets.push_back(std::move(asset));
    }
  }
  }

  if (options.load_metal_sideband && has_metal_callstream_file &&
      !validate_checksum_entry(
          impl_->layout.root_path,
          std::filesystem::path(kMetalCallstreamFileName),
          checksum_lookup,
          impl_->layout.checksums_path,
          validated_checksum_paths,
          impl_->last_error)) {
    return false;
  }

  for (const auto &asset : impl_->metal_assets) {
    if (prefix_limited && referenced_metal_asset_paths.find(asset.relative_path.generic_string()) == referenced_metal_asset_paths.end()) {
      continue;
    }
    if (!validate_asset_index_record(
            impl_->layout.root_path,
            asset,
            checksum_lookup,
            validated_asset_file_sizes,
            impl_->last_error)) {
      return false;
    }
    if (!validate_checksum_entry(
            impl_->layout.root_path,
            asset.relative_path,
            checksum_lookup,
            impl_->layout.checksums_path,
            validated_checksum_paths,
            impl_->last_error)) {
      return false;
    }
  }

  impl_->validated_checksum_paths = std::move(validated_checksum_paths);
  impl_->last_error.clear();
  impl_->open = true;
  return true;
}

void TraceBundleReader::close()
{
  impl_->open = false;
}

bool TraceBundleReader::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const BundleLayout &TraceBundleReader::layout() const noexcept
{
  return impl_->layout;
}

const TraceMetadata &TraceBundleReader::metadata() const noexcept
{
  return impl_->metadata;
}

const std::vector<EventRecord> &TraceBundleReader::events() const noexcept
{
  return impl_->events;
}

const std::vector<MetalEventRecord> &TraceBundleReader::metal_events() const noexcept
{
  return impl_->metal_events;
}

const std::vector<AssetRecord> &TraceBundleReader::assets() const noexcept
{
  return impl_->assets;
}

const std::vector<AssetRecord> &TraceBundleReader::metal_assets() const noexcept
{
  return impl_->metal_assets;
}

const std::vector<ObjectRecord> &TraceBundleReader::objects() const noexcept
{
  return impl_->objects;
}

const ChecksumIndex &TraceBundleReader::checksums() const noexcept
{
  return impl_->checksums;
}

const std::unordered_set<std::string> &TraceBundleReader::validated_checksum_paths() const noexcept
{
  return impl_->validated_checksum_paths;
}

bool TraceBundleReader::has_asset_index() const noexcept
{
  return impl_ && impl_->has_asset_index;
}

bool TraceBundleReader::prefix_limited() const noexcept
{
  return impl_ && impl_->prefix_limited;
}

const std::string &TraceBundleReader::last_error() const noexcept
{
  return impl_->last_error;
}

} // namespace apitrace::trace
