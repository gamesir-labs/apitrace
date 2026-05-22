#pragma once

#include <cstdint>
#include <string>

namespace apitrace::trace {

inline constexpr std::uint32_t kFormatVersion = 1;

enum class ApiKind {
  Unknown,
  D3D11,
  D3D12,
};

struct TraceMetadata {
  ApiKind api = ApiKind::Unknown;
  std::uint32_t format_version = kFormatVersion;
  std::string producer;

  // TODO: add bundle schema metadata once readable root indexes are versioned independently.
};

} // namespace apitrace::trace
