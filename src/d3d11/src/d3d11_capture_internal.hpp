#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace apitrace::d3d11::internal {

void process_attach();
void process_detach() noexcept;

HRESULT WINAPI core_create_device(
    IDXGIFactory *factory,
    IDXGIAdapter *adapter,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    ID3D11Device **device);

HRESULT WINAPI create_device(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **immediate_context);

HRESULT WINAPI create_device_and_swap_chain(
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
    ID3D11DeviceContext **immediate_context);

HRESULT WINAPI on12_create_device(
    IUnknown *device,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    IUnknown *const *command_queues,
    UINT num_command_queues,
    UINT node_mask,
    ID3D11Device **d3d11_device,
    ID3D11DeviceContext **immediate_context,
    D3D_FEATURE_LEVEL *chosen_feature_level);

} // namespace apitrace::d3d11::internal
