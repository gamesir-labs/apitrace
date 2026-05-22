#include "capture_runtime_state.hpp"

#include <utility>

namespace apitrace::capture::internal {

CaptureRuntimeState::CaptureRuntimeState(runtime::CaptureOptions options)
    : options_(std::move(options)), hook_plan_(build_capture_hook_plan(options_))
{
}

void CaptureRuntimeState::arm()
{
  armed_ = true;

  // TODO: track which planned hook surfaces were actually installed instead of a single armed flag.
}

void CaptureRuntimeState::remember_hooked_module(std::string module_name)
{
  hooked_modules_.push_back(std::move(module_name));

  // TODO: normalize module naming once late-module hook policies distinguish paths from short module names.
}

bool CaptureRuntimeState::armed() const noexcept
{
  return armed_;
}

const runtime::CaptureOptions &CaptureRuntimeState::options() const noexcept
{
  return options_;
}

CaptureHookPlan &CaptureRuntimeState::hook_plan() noexcept
{
  return hook_plan_;
}

const CaptureHookPlan &CaptureRuntimeState::hook_plan() const noexcept
{
  return hook_plan_;
}

const std::vector<std::string> &CaptureRuntimeState::hooked_modules() const noexcept
{
  return hooked_modules_;
}

} // namespace apitrace::capture::internal
