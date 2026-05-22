#pragma once

#include "apitrace/capture_options.hpp"
#include "capture_hook_plan.hpp"

#include <string>
#include <vector>

namespace apitrace::capture::internal {

class CaptureRuntimeState {
public:
  explicit CaptureRuntimeState(runtime::CaptureOptions options);

  void arm();
  void remember_hooked_module(std::string module_name);

  bool armed() const noexcept;
  const runtime::CaptureOptions &options() const noexcept;
  CaptureHookPlan &hook_plan() noexcept;
  const CaptureHookPlan &hook_plan() const noexcept;
  const std::vector<std::string> &hooked_modules() const noexcept;

private:
  runtime::CaptureOptions options_;
  CaptureHookPlan hook_plan_;
  bool armed_ = false;
  std::vector<std::string> hooked_modules_;

  // TODO: split persistent runtime state from one-shot session state if capture reuse across sessions is needed.
  // TODO: track failure or degraded mode reasons so callers can surface hook coverage gaps.
};

} // namespace apitrace::capture::internal
