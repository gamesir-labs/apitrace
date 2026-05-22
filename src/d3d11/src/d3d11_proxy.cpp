#include "apitrace/d3d11_proxy.hpp"

#include "d3d11_capture_internal.hpp"

namespace apitrace::d3d11 {

ProxyModuleDescriptor proxy_descriptor()
{
  ProxyModuleDescriptor descriptor;
  descriptor.api = trace::ApiKind::D3D11;
  descriptor.dll_name = "d3d11.dll";
  descriptor.bootstrap_symbol = "apitrace_bootstrap_d3d11";
  return descriptor;
}

} // namespace apitrace::d3d11

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **immediate_context)
{
  return apitrace::d3d11::internal::create_device(
      adapter,
      driver_type,
      software,
      flags,
      feature_levels,
      feature_levels_count,
      sdk_version,
      device,
      feature_level,
      immediate_context);
}

extern "C" HRESULT WINAPI D3D11CoreCreateDevice(
    IDXGIFactory *factory,
    IDXGIAdapter *adapter,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    ID3D11Device **device)
{
  return apitrace::d3d11::internal::core_create_device(
      factory,
      adapter,
      flags,
      feature_levels,
      feature_levels_count,
      device);
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swap_chain_desc,
    IDXGISwapChain **swap_chain,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **immediate_context)
{
  return apitrace::d3d11::internal::create_device_and_swap_chain(
      adapter,
      driver_type,
      software,
      flags,
      feature_levels,
      feature_levels_count,
      sdk_version,
      swap_chain_desc,
      swap_chain,
      device,
      feature_level,
      immediate_context);
}

extern "C" HRESULT WINAPI D3D11On12CreateDevice(
    IUnknown *device,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    IUnknown *const *command_queues,
    UINT num_command_queues,
    UINT node_mask,
    ID3D11Device **d3d11_device,
    ID3D11DeviceContext **immediate_context,
    D3D_FEATURE_LEVEL *chosen_feature_level)
{
  return apitrace::d3d11::internal::on12_create_device(
      device,
      flags,
      feature_levels,
      feature_levels_count,
      command_queues,
      num_command_queues,
      node_mask,
      d3d11_device,
      immediate_context,
      chosen_feature_level);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
  (void)instance;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    apitrace::d3d11::internal::process_attach();
  } else if (reason == DLL_PROCESS_DETACH) {
    apitrace::d3d11::internal::process_detach();
  }
  return TRUE;
}
