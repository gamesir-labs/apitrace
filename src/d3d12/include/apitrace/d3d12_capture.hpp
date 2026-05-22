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
  // TODO: split descriptor, command list, and queue hook groups once interception points are enumerated.
  // TODO: report hook coverage per surface so D3D12 capture planning can be inspected independently.
};

} // namespace apitrace::d3d12
