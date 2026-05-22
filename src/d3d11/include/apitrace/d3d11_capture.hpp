#pragma once

#include "apitrace/capture_runtime.hpp"

namespace apitrace::d3d11 {

class D3D11CaptureHooks {
public:
  D3D11CaptureHooks();

  void install_proxy_hooks(runtime::CaptureRuntime &runtime);
  void install_device_hooks(runtime::CaptureRuntime &runtime);
  void install_context_hooks(runtime::CaptureRuntime &runtime);

private:
  // TODO: split device hooks from immediate/deferred context hooks once interception surfaces become concrete.
  // TODO: add hook capability reporting so capture planning can see what D3D11 surfaces were actually armed.
};

} // namespace apitrace::d3d11
