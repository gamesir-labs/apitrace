#pragma once

#include "apitrace/object_types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace apitrace::trace {

enum class AssetKind {
  Unknown,
  ShaderDxbc,
  ShaderDxil,
  RootSignature,
  Texture,
  Buffer,
  Pipeline,
  ObjectIndex,
  Analysis,
};

struct AssetRecord {
  BlobId blob_id = 0;
  AssetKind kind = AssetKind::Unknown;
  std::filesystem::path relative_path;
  std::string debug_name;
  std::string content_hash;
  std::string fast_fingerprint;
  std::uint64_t byte_size = 0;
  bool binary_payload = true;
  std::vector<std::uint8_t> payload_bytes;

  // TODO: split raw-asset identity from derived-analysis identity if analysis outputs grow.
};

std::string content_hash_bytes(const void *data, std::size_t size);
std::string content_hash_file(const std::filesystem::path &path);
std::string fast_fingerprint_bytes(const void *data, std::size_t size);

} // namespace apitrace::trace
