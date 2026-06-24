#pragma once

#include "apitrace/api_types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace apitrace::runtime {

enum class CaptureMode {
  ProxyDll,
  LauncherInject,
  Attach,
  ChildProcess,
};

struct CaptureTarget {
  std::string executable_path;
  std::vector<std::string> arguments;
  std::string working_directory;
};

struct CaptureOptions {
  CaptureMode mode = CaptureMode::LauncherInject;
  CaptureTarget target;
  bool follow_child_processes = true;
  bool hook_dynamic_modules = true;
  bool capture_initial_resources = true;
  // Reserved for the Phase 1a raw capture format. Parsed from
  // DXMT_CAPTURE_RAW_FORMAT, but intentionally inert until a later phase wires
  // writer selection into the live D3D capture path.
  bool raw_format_reserved = false;
};

} // namespace apitrace::runtime

namespace apitrace {

struct TraceOptions {
  trace::ApiKind api = trace::ApiKind::Unknown;
  runtime::CaptureOptions capture;
  std::filesystem::path bundle_root = "capture.apitrace";
  bool enable_object_graph = true;
  bool enable_resource_blobs = true;
};

} // namespace apitrace
