#pragma once

#include "apitrace/api_types.hpp"

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace apitrace::trace {

struct ChecksumRecord {
  std::filesystem::path relative_path;
  std::string algorithm = "sha256";
  std::string digest;
  std::uint64_t byte_size = 0;
  bool has_byte_size = false;

  // TODO: allow per-file flags once optional or generated outputs need separate handling.
};

struct ChecksumIndex {
  std::uint32_t format_version = kFormatVersion;
  std::string bundle_hash;
  std::vector<ChecksumRecord> files;

  // TODO: add manifest-level compatibility metadata once bundle sealing is implemented.
};

} // namespace apitrace::trace
