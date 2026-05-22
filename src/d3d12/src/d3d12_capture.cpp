#include "apitrace/d3d12_capture.hpp"

namespace apitrace::d3d12 {

D3D12CaptureHooks::D3D12CaptureHooks() = default;

void D3D12CaptureHooks::install_proxy_hooks(runtime::CaptureRuntime &runtime)
{
  // TODO: attach D3D12 proxy-surface hooks through runtime bootstrap.
  (void)runtime;
}

void D3D12CaptureHooks::install_device_hooks(runtime::CaptureRuntime &runtime)
{
  // TODO: intercept D3D12CreateDevice and device-owned object creation surfaces.
  (void)runtime;
}

void D3D12CaptureHooks::install_submission_hooks(runtime::CaptureRuntime &runtime)
{
  // TODO: intercept queue submission, command list close, fence, and present surfaces.
  (void)runtime;
}

} // namespace apitrace::d3d12
