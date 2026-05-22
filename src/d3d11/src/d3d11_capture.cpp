#include "apitrace/d3d11_capture.hpp"

namespace apitrace::d3d11 {

D3D11CaptureHooks::D3D11CaptureHooks() = default;

void D3D11CaptureHooks::install_proxy_hooks(runtime::CaptureRuntime &runtime)
{
  // TODO: attach D3D11 proxy-surface hooks through runtime bootstrap.
  (void)runtime;
}

void D3D11CaptureHooks::install_device_hooks(runtime::CaptureRuntime &runtime)
{
  // TODO: intercept D3D11CreateDevice and device-owned factory paths.
  (void)runtime;
}

void D3D11CaptureHooks::install_context_hooks(runtime::CaptureRuntime &runtime)
{
  // TODO: intercept immediate and deferred context command recording surfaces.
  (void)runtime;
}

} // namespace apitrace::d3d11
