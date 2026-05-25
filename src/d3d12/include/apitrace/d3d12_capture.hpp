#pragma once

#include "apitrace/capture_runtime.hpp"

namespace apitrace::d3d12 {

class D3D12CaptureHooks {
public:
  D3D12CaptureHooks();

  void install_proxy_hooks(runtime::CaptureRuntime &runtime);
  void install_device_hooks(runtime::CaptureRuntime &runtime);
  void install_submission_hooks(runtime::CaptureRuntime &runtime);

private:
  // The Wine app-local d3d12.dll proxy owns the concrete COM vtable capture surface.
  // This planner keeps shared capture runtime setup scoped to that existing entry.
};

} // namespace apitrace::d3d12
