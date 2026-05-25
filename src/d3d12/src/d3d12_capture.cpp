#include "apitrace/d3d12_capture.hpp"

namespace apitrace::d3d12 {

D3D12CaptureHooks::D3D12CaptureHooks() = default;

void D3D12CaptureHooks::install_proxy_hooks(runtime::CaptureRuntime &runtime)
{
  runtime.install_hooks();
  runtime.extend_hooks_for_module("d3d12.dll");
}

void D3D12CaptureHooks::install_device_hooks(runtime::CaptureRuntime &runtime)
{
  runtime.extend_hooks_for_module("d3d12.dll");
}

void D3D12CaptureHooks::install_submission_hooks(runtime::CaptureRuntime &runtime)
{
  runtime.extend_hooks_for_module("d3d12.dll");
}

} // namespace apitrace::d3d12
