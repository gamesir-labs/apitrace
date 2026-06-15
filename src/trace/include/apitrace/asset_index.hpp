#pragma once

#include "apitrace/object_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

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
  bool force_synchronous_write = false;
  std::vector<std::uint8_t> payload_bytes;

  // TODO: split raw-asset identity from derived-analysis identity if analysis outputs grow.
};

class ContentHasher {
public:
  ContentHasher();

  void update(const std::uint8_t *data, std::size_t size);
  void update(const std::vector<std::uint8_t> &data);
  void update(std::string_view text);

  [[nodiscard]] std::string final_hex();

private:
  static const std::array<std::uint32_t, 64> kTable;

  static std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count);
  static std::uint32_t byteswap32(std::uint32_t value);
  static std::uint64_t byteswap64(std::uint64_t value);

  void transform(const std::uint8_t *chunk);

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
#ifdef __APPLE__
  CC_SHA256_CTX platform_context_{};
#endif
};

std::string content_hash_bytes(const void *data, std::size_t size);
std::string content_hash_file(const std::filesystem::path &path);
std::string content_hash_file_prefix(const std::filesystem::path &path, std::uint64_t byte_size);
std::string fast_fingerprint_bytes(const void *data, std::size_t size);

} // namespace apitrace::trace
