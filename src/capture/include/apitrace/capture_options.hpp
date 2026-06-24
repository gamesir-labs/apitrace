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
  enum class CaptureRawMode {
    Off,
    DualWrite,
    RawOnly,
  };
  // DXMT_CAPTURE_RAW_FORMAT:
  // unset/0 -> Off, 1/dual/dual-write -> DualWrite, 2/raw-only -> RawOnly.
  CaptureRawMode raw_mode = CaptureRawMode::Off;
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
