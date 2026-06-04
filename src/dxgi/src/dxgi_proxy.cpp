#include "apitrace/d3d12_capture.hpp"

#ifndef CINTERFACE
#define CINTERFACE
#endif

#include <windows.h>
#include <dxgi1_2.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using CreateDXGIFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
using CreateDXGIFactory1Fn = HRESULT(WINAPI *)(REFIID, void **);
using CreateDXGIFactory2Fn = HRESULT(WINAPI *)(UINT, REFIID, void **);
using DXGIGetDebugInterface1Fn = HRESULT(WINAPI *)(UINT, REFIID, void **);
using DXGIDeclareAdapterRemovalSupportFn = HRESULT(WINAPI *)();
using DXGIReportAdapterConfigurationFn = HRESULT(WINAPI *)(DWORD);
using PIXBeginCaptureFn = HRESULT(WINAPI *)(DWORD, void *);
using PIXEndCaptureFn = HRESULT(WINAPI *)();
using PIXGetCaptureStateFn = HRESULT(WINAPI *)();

using FactoryCreateSwapChainFn =
    HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
using Factory2CreateSwapChainForHwndFn =
    HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
using Factory2CreateSwapChainForCoreWindowFn =
    HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                IDXGIOutput *, IDXGISwapChain1 **);
using Factory2CreateSwapChainForCompositionFn =
    HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                IDXGIOutput *, IDXGISwapChain1 **);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using SwapChain1Present1Fn =
    HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

struct DownstreamModule {
  HMODULE module = nullptr;
  CreateDXGIFactoryFn create_factory = nullptr;
  CreateDXGIFactory1Fn create_factory1 = nullptr;
  CreateDXGIFactory2Fn create_factory2 = nullptr;
  DXGIGetDebugInterface1Fn get_debug_interface1 = nullptr;
  DXGIDeclareAdapterRemovalSupportFn declare_adapter_removal_support = nullptr;
  DXGIReportAdapterConfigurationFn report_adapter_configuration = nullptr;
  PIXBeginCaptureFn pix_begin_capture = nullptr;
  PIXEndCaptureFn pix_end_capture = nullptr;
  PIXGetCaptureStateFn pix_get_capture_state = nullptr;
};

struct FactoryHookState {
  IDXGIFactoryVtbl *vtable = nullptr;
  FactoryCreateSwapChainFn create_swap_chain = nullptr;
};

struct Factory2HookState {
  IDXGIFactory2Vtbl *vtable = nullptr;
  FactoryCreateSwapChainFn create_swap_chain = nullptr;
  Factory2CreateSwapChainForHwndFn create_swap_chain_for_hwnd = nullptr;
  Factory2CreateSwapChainForCoreWindowFn create_swap_chain_for_core_window = nullptr;
  Factory2CreateSwapChainForCompositionFn create_swap_chain_for_composition = nullptr;
};

struct SwapChainHookState {
  IDXGISwapChainVtbl *vtable = nullptr;
  SwapChainPresentFn present = nullptr;
};

struct SwapChain1HookState {
  IDXGISwapChain1Vtbl *vtable = nullptr;
  SwapChainPresentFn present = nullptr;
  SwapChain1Present1Fn present1 = nullptr;
};

struct State {
  std::once_flag downstream_once;
  DownstreamModule downstream;
  std::mutex mutex;
  std::unordered_map<IDXGIFactoryVtbl *, FactoryHookState> factory_hooks;
  std::unordered_map<IDXGIFactory2Vtbl *, Factory2HookState> factory2_hooks;
  std::unordered_map<IDXGISwapChainVtbl *, SwapChainHookState> swapchain_hooks;
  std::unordered_map<IDXGISwapChain1Vtbl *, SwapChain1HookState> swapchain1_hooks;
};

State &state()
{
  static State value;
  return value;
}

std::string downstream_path()
{
  if (const char *explicit_path = std::getenv("APITRACE_DOWNSTREAM_DXGI")) {
    if (*explicit_path) {
      return explicit_path;
    }
  }

  char system_directory[MAX_PATH] = {};
  const auto length = GetSystemDirectoryA(system_directory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return "C:\\windows\\system32\\dxgi.dll";
  }

  std::string path(system_directory, length);
  path += "\\dxgi.dll";
  return path;
}

void proxy_debug_log(const char *message)
{
  const char *path = std::getenv("APITRACE_DXGI_PROXY_LOG");
  if (!path || !*path || !message) {
    return;
  }
  if (std::FILE *stream = std::fopen(path, "ab")) {
    std::fputs(message, stream);
    std::fputc('\n', stream);
    std::fclose(stream);
  }
}

void proxy_debug_logf(const char *format, ...)
{
  const char *path = std::getenv("APITRACE_DXGI_PROXY_LOG");
  if (!path || !*path || !format) {
    return;
  }
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  proxy_debug_log(buffer);
}

DownstreamModule &downstream_module()
{
  auto &current = state();
  std::call_once(current.downstream_once, [&current]() {
    const auto path = downstream_path();
    current.downstream.module = LoadLibraryA(path.c_str());
    proxy_debug_logf("LoadLibraryA(%s)=%p", path.c_str(), current.downstream.module);
    if (!current.downstream.module) {
      return;
    }
    current.downstream.create_factory =
        reinterpret_cast<CreateDXGIFactoryFn>(GetProcAddress(current.downstream.module, "CreateDXGIFactory"));
    current.downstream.create_factory1 =
        reinterpret_cast<CreateDXGIFactory1Fn>(GetProcAddress(current.downstream.module, "CreateDXGIFactory1"));
    current.downstream.create_factory2 =
        reinterpret_cast<CreateDXGIFactory2Fn>(GetProcAddress(current.downstream.module, "CreateDXGIFactory2"));
    current.downstream.get_debug_interface1 =
        reinterpret_cast<DXGIGetDebugInterface1Fn>(GetProcAddress(current.downstream.module, "DXGIGetDebugInterface1"));
    current.downstream.declare_adapter_removal_support =
        reinterpret_cast<DXGIDeclareAdapterRemovalSupportFn>(
            GetProcAddress(current.downstream.module, "DXGIDeclareAdapterRemovalSupport"));
    current.downstream.report_adapter_configuration =
        reinterpret_cast<DXGIReportAdapterConfigurationFn>(
            GetProcAddress(current.downstream.module, "DXGIReportAdapterConfiguration"));
    current.downstream.pix_begin_capture =
        reinterpret_cast<PIXBeginCaptureFn>(GetProcAddress(current.downstream.module, "PIXBeginCapture"));
    current.downstream.pix_end_capture =
        reinterpret_cast<PIXEndCaptureFn>(GetProcAddress(current.downstream.module, "PIXEndCapture"));
    current.downstream.pix_get_capture_state =
        reinterpret_cast<PIXGetCaptureStateFn>(GetProcAddress(current.downstream.module, "PIXGetCaptureState"));
  });
  return current.downstream;
}

template <typename VTable, typename Field>
void patch_vtable_field(VTable *vtable, Field VTable::*field, Field replacement)
{
  auto *slot = &(vtable->*field);
  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  const auto page_size = static_cast<std::uintptr_t>(system_info.dwPageSize ? system_info.dwPageSize : 4096);
  const auto address = reinterpret_cast<std::uintptr_t>(slot);
  const auto page_start = address & ~(page_size - 1);
  const auto protect_size = (address - page_start) + sizeof(*slot);

  DWORD old_protect = 0;
  if (VirtualProtect(reinterpret_cast<void *>(page_start), protect_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void *>(page_start), protect_size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return;
  }

  SIZE_T written = 0;
  WriteProcessMemory(GetCurrentProcess(), slot, &replacement, sizeof(replacement), &written);
}

template <typename VTable>
VTable *clone_vtable(const VTable *source)
{
  if (!source) {
    return nullptr;
  }
  auto *memory = std::malloc(sizeof(VTable));
  if (!memory) {
    return nullptr;
  }
  std::memcpy(memory, source, sizeof(VTable));
  return static_cast<VTable *>(memory);
}

template <typename Interface, typename VTable>
class ScopedOriginalVTable {
public:
  ScopedOriginalVTable(Interface *object, VTable *original_vtable)
      : object_(object), patched_vtable_(object ? object->lpVtbl : nullptr)
  {
    if (object_ && original_vtable) {
      object_->lpVtbl = original_vtable;
    }
  }

  ~ScopedOriginalVTable()
  {
    if (object_ && patched_vtable_) {
      object_->lpVtbl = patched_vtable_;
    }
  }

private:
  Interface *object_ = nullptr;
  VTable *patched_vtable_ = nullptr;
};

void patch_factory(IDXGIFactory *factory);
void patch_factory2(IDXGIFactory2 *factory);
void patch_swapchain(IDXGISwapChain *swapchain);
void patch_swapchain1(IDXGISwapChain1 *swapchain);
void patch_swapchain_interfaces(IDXGISwapChain *swapchain);

FactoryHookState factory_hook_for(IDXGIFactory *factory)
{
  std::lock_guard<std::mutex> lock(state().mutex);
  const auto it = state().factory_hooks.find(factory ? factory->lpVtbl : nullptr);
  return it == state().factory_hooks.end() ? FactoryHookState{} : it->second;
}

Factory2HookState factory2_hook_for(IDXGIFactory2 *factory)
{
  std::lock_guard<std::mutex> lock(state().mutex);
  const auto it = state().factory2_hooks.find(factory ? factory->lpVtbl : nullptr);
  return it == state().factory2_hooks.end() ? Factory2HookState{} : it->second;
}

SwapChainHookState swapchain_hook_for(IDXGISwapChain *swapchain)
{
  std::lock_guard<std::mutex> lock(state().mutex);
  const auto it = state().swapchain_hooks.find(swapchain ? swapchain->lpVtbl : nullptr);
  return it == state().swapchain_hooks.end() ? SwapChainHookState{} : it->second;
}

SwapChain1HookState swapchain1_hook_for(IDXGISwapChain1 *swapchain)
{
  std::lock_guard<std::mutex> lock(state().mutex);
  const auto it = state().swapchain1_hooks.find(swapchain ? swapchain->lpVtbl : nullptr);
  return it == state().swapchain1_hooks.end() ? SwapChain1HookState{} : it->second;
}

HRESULT STDMETHODCALLTYPE hook_factory_create_swap_chain(
    IDXGIFactory *factory,
    IUnknown *device,
    DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **swapchain)
{
  const auto hook = factory_hook_for(factory);
  if (!hook.create_swap_chain) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGIFactory, IDXGIFactoryVtbl> original(factory, hook.vtable);
  const HRESULT hr = hook.create_swap_chain(factory, device, desc, swapchain);
  if (SUCCEEDED(hr) && swapchain && *swapchain) {
    patch_swapchain(*swapchain);
    apitrace::d3d12::record_dxgi_create_swapchain(factory, device, *swapchain);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_factory2_create_swap_chain(
    IDXGIFactory2 *factory,
    IUnknown *device,
    DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **swapchain)
{
  const auto hook = factory2_hook_for(factory);
  if (!hook.create_swap_chain) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGIFactory2, IDXGIFactory2Vtbl> original(factory, hook.vtable);
  const HRESULT hr = hook.create_swap_chain(reinterpret_cast<IDXGIFactory *>(factory), device, desc, swapchain);
  if (SUCCEEDED(hr) && swapchain && *swapchain) {
    patch_swapchain_interfaces(*swapchain);
    apitrace::d3d12::record_dxgi_create_swapchain(factory, device, *swapchain);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_factory2_create_swap_chain_for_hwnd(
    IDXGIFactory2 *factory,
    IUnknown *device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
    IDXGIOutput *restrict_to_output,
    IDXGISwapChain1 **swapchain)
{
  const auto hook = factory2_hook_for(factory);
  if (!hook.create_swap_chain_for_hwnd) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGIFactory2, IDXGIFactory2Vtbl> original(factory, hook.vtable);
  const HRESULT hr = hook.create_swap_chain_for_hwnd(
      factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swapchain);
  if (SUCCEEDED(hr) && swapchain && *swapchain) {
    patch_swapchain1(*swapchain);
    apitrace::d3d12::record_dxgi_create_swapchain(factory, device, *swapchain);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_factory2_create_swap_chain_for_core_window(
    IDXGIFactory2 *factory,
    IUnknown *device,
    IUnknown *window,
    const DXGI_SWAP_CHAIN_DESC1 *desc,
    IDXGIOutput *restrict_to_output,
    IDXGISwapChain1 **swapchain)
{
  const auto hook = factory2_hook_for(factory);
  if (!hook.create_swap_chain_for_core_window) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGIFactory2, IDXGIFactory2Vtbl> original(factory, hook.vtable);
  const HRESULT hr = hook.create_swap_chain_for_core_window(
      factory, device, window, desc, restrict_to_output, swapchain);
  if (SUCCEEDED(hr) && swapchain && *swapchain) {
    patch_swapchain1(*swapchain);
    apitrace::d3d12::record_dxgi_create_swapchain(factory, device, *swapchain);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_factory2_create_swap_chain_for_composition(
    IDXGIFactory2 *factory,
    IUnknown *device,
    const DXGI_SWAP_CHAIN_DESC1 *desc,
    IDXGIOutput *restrict_to_output,
    IDXGISwapChain1 **swapchain)
{
  const auto hook = factory2_hook_for(factory);
  if (!hook.create_swap_chain_for_composition) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGIFactory2, IDXGIFactory2Vtbl> original(factory, hook.vtable);
  const HRESULT hr = hook.create_swap_chain_for_composition(
      factory, device, desc, restrict_to_output, swapchain);
  if (SUCCEEDED(hr) && swapchain && *swapchain) {
    patch_swapchain1(*swapchain);
    apitrace::d3d12::record_dxgi_create_swapchain(factory, device, *swapchain);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_swapchain_present(IDXGISwapChain *swapchain, UINT sync_interval, UINT flags)
{
  const auto hook = swapchain_hook_for(swapchain);
  if (!hook.present) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGISwapChain, IDXGISwapChainVtbl> original(swapchain, hook.vtable);
  const HRESULT hr = hook.present(swapchain, sync_interval, flags);
  apitrace::d3d12::record_present(swapchain, sync_interval, flags, static_cast<std::int32_t>(hr), SUCCEEDED(hr));
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_swapchain1_present(IDXGISwapChain1 *swapchain, UINT sync_interval, UINT flags)
{
  const auto hook = swapchain1_hook_for(swapchain);
  if (!hook.present) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGISwapChain1, IDXGISwapChain1Vtbl> original(swapchain, hook.vtable);
  const HRESULT hr = hook.present(reinterpret_cast<IDXGISwapChain *>(swapchain), sync_interval, flags);
  apitrace::d3d12::record_present(swapchain, sync_interval, flags, static_cast<std::int32_t>(hr), SUCCEEDED(hr));
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_swapchain1_present1(
    IDXGISwapChain1 *swapchain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS *present_parameters)
{
  const auto hook = swapchain1_hook_for(swapchain);
  if (!hook.present1) {
    return E_FAIL;
  }
  ScopedOriginalVTable<IDXGISwapChain1, IDXGISwapChain1Vtbl> original(swapchain, hook.vtable);
  const HRESULT hr = hook.present1(swapchain, sync_interval, flags, present_parameters);
  apitrace::d3d12::record_present(swapchain, sync_interval, flags, static_cast<std::int32_t>(hr), SUCCEEDED(hr));
  return hr;
}

void patch_factory(IDXGIFactory *factory)
{
  if (!factory || !factory->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(state().mutex);
  if (state().factory_hooks.find(factory->lpVtbl) != state().factory_hooks.end()) {
    return;
  }
  auto *original_vtable = factory->lpVtbl;
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    return;
  }
  FactoryHookState hook;
  hook.vtable = original_vtable;
  hook.create_swap_chain = original_vtable->CreateSwapChain;
  state().factory_hooks.emplace(vtable, hook);
  factory->lpVtbl = vtable;
  patch_vtable_field(vtable, &IDXGIFactoryVtbl::CreateSwapChain, hook_factory_create_swap_chain);
}

void patch_factory2(IDXGIFactory2 *factory)
{
  if (!factory || !factory->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(state().mutex);
  if (state().factory2_hooks.find(factory->lpVtbl) != state().factory2_hooks.end()) {
    return;
  }
  auto *original_vtable = factory->lpVtbl;
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    return;
  }
  Factory2HookState hook;
  hook.vtable = original_vtable;
  hook.create_swap_chain = reinterpret_cast<FactoryCreateSwapChainFn>(original_vtable->CreateSwapChain);
  hook.create_swap_chain_for_hwnd = original_vtable->CreateSwapChainForHwnd;
  hook.create_swap_chain_for_core_window = original_vtable->CreateSwapChainForCoreWindow;
  hook.create_swap_chain_for_composition = original_vtable->CreateSwapChainForComposition;
  state().factory2_hooks.emplace(vtable, hook);
  factory->lpVtbl = vtable;
  patch_vtable_field(
      vtable,
      &IDXGIFactory2Vtbl::CreateSwapChain,
      reinterpret_cast<decltype(vtable->CreateSwapChain)>(hook_factory2_create_swap_chain));
  patch_vtable_field(vtable, &IDXGIFactory2Vtbl::CreateSwapChainForHwnd, hook_factory2_create_swap_chain_for_hwnd);
  patch_vtable_field(
      vtable,
      &IDXGIFactory2Vtbl::CreateSwapChainForCoreWindow,
      hook_factory2_create_swap_chain_for_core_window);
  patch_vtable_field(
      vtable,
      &IDXGIFactory2Vtbl::CreateSwapChainForComposition,
      hook_factory2_create_swap_chain_for_composition);
}

void patch_factory_interfaces(void *factory)
{
  if (!factory) {
    return;
  }
  auto *factory0 = static_cast<IDXGIFactory *>(factory);
  auto *factory0_vtable = factory0->lpVtbl;

  IDXGIFactory2 *factory2 = nullptr;
  if (SUCCEEDED(factory0->lpVtbl->QueryInterface(factory0, IID_IDXGIFactory2, reinterpret_cast<void **>(&factory2))) &&
      factory2) {
    const bool shared_vtable = factory0_vtable == reinterpret_cast<IDXGIFactoryVtbl *>(factory2->lpVtbl);
    patch_factory2(factory2);
    factory2->lpVtbl->Release(factory2);
    if (shared_vtable) {
      return;
    }
  }
  patch_factory(factory0);
}

void patch_swapchain(IDXGISwapChain *swapchain)
{
  if (!swapchain || !swapchain->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(state().mutex);
  if (state().swapchain_hooks.find(swapchain->lpVtbl) != state().swapchain_hooks.end()) {
    return;
  }
  auto *original_vtable = swapchain->lpVtbl;
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    return;
  }
  SwapChainHookState hook;
  hook.vtable = original_vtable;
  hook.present = original_vtable->Present;
  state().swapchain_hooks.emplace(vtable, hook);
  swapchain->lpVtbl = vtable;
  patch_vtable_field(vtable, &IDXGISwapChainVtbl::Present, hook_swapchain_present);
}

void patch_swapchain1(IDXGISwapChain1 *swapchain)
{
  if (!swapchain || !swapchain->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(state().mutex);
  if (state().swapchain1_hooks.find(swapchain->lpVtbl) != state().swapchain1_hooks.end()) {
    return;
  }
  auto *original_vtable = swapchain->lpVtbl;
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    return;
  }
  SwapChain1HookState hook;
  hook.vtable = original_vtable;
  hook.present = reinterpret_cast<SwapChainPresentFn>(original_vtable->Present);
  hook.present1 = original_vtable->Present1;
  state().swapchain1_hooks.emplace(vtable, hook);
  swapchain->lpVtbl = vtable;
  patch_vtable_field(
      vtable,
      &IDXGISwapChain1Vtbl::Present,
      reinterpret_cast<decltype(vtable->Present)>(hook_swapchain1_present));
  patch_vtable_field(vtable, &IDXGISwapChain1Vtbl::Present1, hook_swapchain1_present1);
}

void patch_swapchain_interfaces(IDXGISwapChain *swapchain)
{
  if (!swapchain) {
    return;
  }
  auto *swapchain_vtable = swapchain->lpVtbl;

  IDXGISwapChain1 *swapchain1 = nullptr;
  if (SUCCEEDED(swapchain->lpVtbl->QueryInterface(
          swapchain,
          IID_IDXGISwapChain1,
          reinterpret_cast<void **>(&swapchain1))) &&
      swapchain1) {
    const bool shared_vtable = swapchain_vtable == reinterpret_cast<IDXGISwapChainVtbl *>(swapchain1->lpVtbl);
    patch_swapchain1(swapchain1);
    swapchain1->lpVtbl->Release(swapchain1);
    if (shared_vtable) {
      return;
    }
  }
  patch_swapchain(swapchain);
}

HRESULT create_factory_common(HRESULT hr, void **factory)
{
  if (SUCCEEDED(hr) && factory && *factory) {
    patch_factory_interfaces(*factory);
  }
  return hr;
}

} // namespace

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **factory)
{
  auto &downstream = downstream_module();
  if (!downstream.create_factory) {
    return E_FAIL;
  }
  return create_factory_common(downstream.create_factory(riid, factory), factory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **factory)
{
  auto &downstream = downstream_module();
  if (!downstream.create_factory1) {
    return E_FAIL;
  }
  return create_factory_common(downstream.create_factory1(riid, factory), factory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
  auto &downstream = downstream_module();
  if (!downstream.create_factory2) {
    return E_FAIL;
  }
  return create_factory_common(downstream.create_factory2(flags, riid, factory), factory);
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **debug)
{
  auto &downstream = downstream_module();
  if (!downstream.get_debug_interface1) {
    return E_NOINTERFACE;
  }
  return downstream.get_debug_interface1(flags, riid, debug);
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
  auto &downstream = downstream_module();
  if (!downstream.declare_adapter_removal_support) {
    return E_NOTIMPL;
  }
  return downstream.declare_adapter_removal_support();
}

extern "C" HRESULT WINAPI DXGIReportAdapterConfiguration(DWORD flags)
{
  auto &downstream = downstream_module();
  if (!downstream.report_adapter_configuration) {
    return E_NOTIMPL;
  }
  return downstream.report_adapter_configuration(flags);
}

extern "C" HRESULT WINAPI PIXBeginCapture(DWORD flags, void *parameters)
{
  auto &downstream = downstream_module();
  if (!downstream.pix_begin_capture) {
    return E_NOTIMPL;
  }
  return downstream.pix_begin_capture(flags, parameters);
}

extern "C" HRESULT WINAPI PIXEndCapture()
{
  auto &downstream = downstream_module();
  if (!downstream.pix_end_capture) {
    return E_NOTIMPL;
  }
  return downstream.pix_end_capture();
}

extern "C" HRESULT WINAPI PIXGetCaptureState()
{
  auto &downstream = downstream_module();
  if (!downstream.pix_get_capture_state) {
    return E_NOTIMPL;
  }
  return downstream.pix_get_capture_state();
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
  (void)instance;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
