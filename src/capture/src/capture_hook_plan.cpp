#include "capture_hook_plan.hpp"

namespace apitrace::capture::internal {

CaptureHookPlan build_capture_hook_plan(const runtime::CaptureOptions &options)
{
  CaptureHookPlan plan;
  plan.mode = options.mode;
  plan.surfaces = {HookSurface::D3D11, HookSurface::D3D12, HookSurface::Dxgi};

  if (options.hook_dynamic_modules) {
    plan.surfaces.push_back(HookSurface::DynamicModuleScan);
  }

  if (options.follow_child_processes) {
    plan.surfaces.push_back(HookSurface::ChildProcessPropagation);
  }

  // TODO: specialize the plan per entry mode instead of using one shared default surface set.
  // TODO: derive plan contents from target executable and API expectations once preflight inspection exists.
  return plan;
}

void register_late_module(CaptureHookPlan &plan, std::string_view module_name)
{
  plan.late_modules.emplace_back(module_name);

  // TODO: classify late modules by hook surface so D3D11, D3D12, and DXGI rescans can be separated.
}

} // namespace apitrace::capture::internal
