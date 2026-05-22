#pragma once

#include "apitrace/object_types.hpp"

#include <filesystem>
#include <string>

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
  bool binary_payload = true;

  // TODO: split raw-asset identity from derived-analysis identity if analysis outputs grow.
  // TODO: attach stable content hashes here once checksum generation becomes real.
};

} // namespace apitrace::trace
