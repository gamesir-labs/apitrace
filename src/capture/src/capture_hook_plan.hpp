#pragma once

#include "apitrace/capture_options.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace apitrace::capture::internal {

enum class HookSurface {
  D3D11,
  D3D12,
  Dxgi,
  DynamicModuleScan,
  ChildProcessPropagation,
};

struct CaptureHookPlan {
  runtime::CaptureMode mode = runtime::CaptureMode::LauncherInject;
  std::vector<HookSurface> surfaces;
  std::vector<std::string> late_modules;

  // TODO: attach launch-time and attach-time prerequisites once entry modes diverge further.
  // TODO: record per-surface validation requirements so tests can inspect intended hook coverage.
};

CaptureHookPlan build_capture_hook_plan(const runtime::CaptureOptions &options);
void register_late_module(CaptureHookPlan &plan, std::string_view module_name);

} // namespace apitrace::capture::internal
