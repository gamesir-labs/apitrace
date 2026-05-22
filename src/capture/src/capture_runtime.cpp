#include "apitrace/capture_runtime.hpp"

#include "capture_hook_plan.hpp"
#include "capture_runtime_state.hpp"

#include <memory>
#include <utility>

namespace apitrace::runtime {

struct CaptureRuntime::Impl {
  explicit Impl(CaptureOptions opts) : state(std::move(opts)) {}

  capture::internal::CaptureRuntimeState state;

  // TODO: add a diagnostics sink once hook planning and hook installation report separate failure modes.
};

CaptureRuntime::CaptureRuntime(CaptureOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

CaptureRuntime::~CaptureRuntime() = default;

void CaptureRuntime::install_hooks()
{
  // TODO: install planned D3D/DXGI interception surfaces based on hook plan and selected entry mode.
  // TODO: distinguish mandatory hook surfaces from optional late-module surfaces.
  impl_->state.arm();
}

void CaptureRuntime::extend_hooks_for_module(std::string_view module_name)
{
  capture::internal::register_late_module(impl_->state.hook_plan(), module_name);
  // TODO: reconnect this to the dynamic module scanner and late hook installer.
  impl_->state.remember_hooked_module(std::string(module_name));
}

bool CaptureRuntime::armed() const noexcept
{
  return impl_ && impl_->state.armed();
}

const CaptureOptions &CaptureRuntime::options() const noexcept
{
  return impl_->state.options();
}

} // namespace apitrace::runtime
