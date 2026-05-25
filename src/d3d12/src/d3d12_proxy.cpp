#include "apitrace/d3d12_proxy.hpp"

#include "apitrace/capture_runtime.hpp"
#include "apitrace/trace_session.hpp"

#ifndef CINTERFACE
#define CINTERFACE
#endif

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace apitrace::d3d12 {

ProxyModuleDescriptor proxy_descriptor()
{
  ProxyModuleDescriptor descriptor;
  descriptor.api = trace::ApiKind::D3D12;
  descriptor.dll_name = "d3d12.dll";
  descriptor.bootstrap_symbol = "apitrace_bootstrap_d3d12";
  return descriptor;
}

} // namespace apitrace::d3d12

namespace {

using D3D12CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
using D3D12GetDebugInterfaceFn = HRESULT(WINAPI *)(REFIID, void **);
using D3D12SerializeRootSignatureFn =
    HRESULT(WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
using D3D12SerializeVersionedRootSignatureFn =
    HRESULT(WINAPI *)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);
using D3D12CreateRootSignatureDeserializerFn =
    HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
using D3D12CreateVersionedRootSignatureDeserializerFn =
    HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
using D3D12EnableExperimentalFeaturesFn = HRESULT(WINAPI *)(UINT, const IID *, void *, UINT *);

constexpr GUID kIidD3D12Device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
constexpr GUID kIidD3D12Device1 = {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}};
constexpr GUID kIidD3D12Device2 = {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};
constexpr GUID kIidD3D12Device3 = {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}};
constexpr GUID kIidD3D12Device4 = {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}};
constexpr GUID kIidD3D12Device5 = {0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}};
constexpr GUID kIidD3D12GraphicsCommandList4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
constexpr GUID kIidD3D12GraphicsCommandList6 = {0xc3827890, 0xe548, 0x4cfa, {0x96, 0xcf, 0x56, 0x89, 0xa9, 0x37, 0x0f, 0x80}};

struct DownstreamModule {
  HMODULE module = nullptr;
  D3D12CreateDeviceFn create_device = nullptr;
  D3D12GetDebugInterfaceFn get_debug_interface = nullptr;
  D3D12SerializeRootSignatureFn serialize_root_signature = nullptr;
  D3D12SerializeVersionedRootSignatureFn serialize_versioned_root_signature = nullptr;
  D3D12CreateRootSignatureDeserializerFn create_root_signature_deserializer = nullptr;
  D3D12CreateVersionedRootSignatureDeserializerFn create_versioned_root_signature_deserializer = nullptr;
  D3D12EnableExperimentalFeaturesFn enable_experimental_features = nullptr;
};

struct ObjectInfo {
  apitrace::trace::ObjectId object_id = 0;
  apitrace::trace::ObjectKind kind = apitrace::trace::ObjectKind::Unknown;
  apitrace::trace::ObjectId parent_object_id = 0;
  std::string debug_name;
};

HRESULT STDMETHODCALLTYPE hook_device_query_interface(ID3D12Device *self, REFIID riid, void **object);
HRESULT STDMETHODCALLTYPE hook_device_create_command_queue(
    ID3D12Device *self,
    const D3D12_COMMAND_QUEUE_DESC *desc,
    REFIID riid,
    void **command_queue);
HRESULT STDMETHODCALLTYPE hook_device_create_command_allocator(
    ID3D12Device *self,
    D3D12_COMMAND_LIST_TYPE type,
    REFIID riid,
    void **command_allocator);
HRESULT STDMETHODCALLTYPE hook_device_create_graphics_pipeline_state(
    ID3D12Device *self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    REFIID riid,
    void **pipeline_state);
HRESULT STDMETHODCALLTYPE hook_device_create_compute_pipeline_state(
    ID3D12Device *self,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    REFIID riid,
    void **pipeline_state);
HRESULT STDMETHODCALLTYPE hook_device_create_command_list(
    ID3D12Device *self,
    UINT node_mask,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *command_allocator,
    ID3D12PipelineState *initial_pipeline_state,
    REFIID riid,
    void **command_list);
HRESULT STDMETHODCALLTYPE hook_device_create_descriptor_heap(
    ID3D12Device *self,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc,
    REFIID riid,
    void **descriptor_heap);
HRESULT STDMETHODCALLTYPE hook_device_create_root_signature(
    ID3D12Device *self,
    UINT node_mask,
    const void *bytecode,
    SIZE_T bytecode_length,
    REFIID riid,
    void **root_signature);
void STDMETHODCALLTYPE hook_device_create_render_target_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
void STDMETHODCALLTYPE hook_device_create_depth_stencil_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
void STDMETHODCALLTYPE hook_device_create_shader_resource_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
void STDMETHODCALLTYPE hook_device_create_unordered_access_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    ID3D12Resource *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
void STDMETHODCALLTYPE hook_device_create_constant_buffer_view(
    ID3D12Device *self,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
HRESULT STDMETHODCALLTYPE hook_device_create_committed_resource(
    ID3D12Device *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource);
HRESULT STDMETHODCALLTYPE hook_device_create_fence(
    ID3D12Device *self,
    UINT64 initial_value,
    D3D12_FENCE_FLAGS flags,
    REFIID riid,
    void **fence);
HRESULT STDMETHODCALLTYPE hook_device_create_command_signature(
    ID3D12Device *self,
    const D3D12_COMMAND_SIGNATURE_DESC *desc,
    ID3D12RootSignature *root_signature,
    REFIID riid,
    void **command_signature);

HRESULT STDMETHODCALLTYPE hook_allocator_reset(ID3D12CommandAllocator *self);
void STDMETHODCALLTYPE hook_queue_execute_command_lists(
    ID3D12CommandQueue *self,
    UINT command_list_count,
    ID3D12CommandList *const *command_lists);
HRESULT STDMETHODCALLTYPE hook_queue_signal(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value);
HRESULT STDMETHODCALLTYPE hook_queue_wait(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value);
HRESULT STDMETHODCALLTYPE hook_command_list_query_interface(ID3D12GraphicsCommandList *self, REFIID riid, void **object);
HRESULT STDMETHODCALLTYPE hook_command_list_close(ID3D12GraphicsCommandList *self);
HRESULT STDMETHODCALLTYPE hook_command_list_reset(
    ID3D12GraphicsCommandList *self,
    ID3D12CommandAllocator *allocator,
    ID3D12PipelineState *initial_state);
void STDMETHODCALLTYPE hook_command_list_resource_barrier(
    ID3D12GraphicsCommandList *self,
    UINT barrier_count,
    const D3D12_RESOURCE_BARRIER *barriers);
void STDMETHODCALLTYPE hook_command_list_set_descriptor_heaps(
    ID3D12GraphicsCommandList *self,
    UINT heap_count,
    ID3D12DescriptorHeap *const *heaps);
void STDMETHODCALLTYPE hook_command_list_draw_instanced(
    ID3D12GraphicsCommandList *self,
    UINT vertex_count_per_instance,
    UINT instance_count,
    UINT start_vertex_location,
    UINT start_instance_location);
void STDMETHODCALLTYPE hook_command_list_draw_indexed_instanced(
    ID3D12GraphicsCommandList *self,
    UINT index_count_per_instance,
    UINT instance_count,
    UINT start_index_location,
    INT base_vertex_location,
    UINT start_instance_location);
void STDMETHODCALLTYPE hook_command_list_dispatch(ID3D12GraphicsCommandList *self, UINT x, UINT y, UINT z);
void STDMETHODCALLTYPE hook_command_list_execute_indirect(
    ID3D12GraphicsCommandList *self,
    ID3D12CommandSignature *command_signature,
    UINT max_command_count,
    ID3D12Resource *arg_buffer,
    UINT64 arg_buffer_offset,
    ID3D12Resource *count_buffer,
    UINT64 count_buffer_offset);
void STDMETHODCALLTYPE hook_command_list_dispatch_rays(
    ID3D12GraphicsCommandList4 *self,
    const D3D12_DISPATCH_RAYS_DESC *desc);
void STDMETHODCALLTYPE hook_command_list_dispatch_mesh(
    ID3D12GraphicsCommandList6 *self,
    UINT thread_group_count_x,
    UINT thread_group_count_y,
    UINT thread_group_count_z);
HRESULT STDMETHODCALLTYPE hook_fence_set_event_on_completion(ID3D12Fence *self, UINT64 value, HANDLE event);
HRESULT STDMETHODCALLTYPE hook_fence_signal(ID3D12Fence *self, UINT64 value);

struct DeviceHookState {
  ID3D12DeviceVtbl *vtable = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().QueryInterface) query_interface = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommandQueue) create_command_queue = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommandAllocator) create_command_allocator = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateGraphicsPipelineState) create_graphics_pipeline_state = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateComputePipelineState) create_compute_pipeline_state = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommandList) create_command_list = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateDescriptorHeap) create_descriptor_heap = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateRootSignature) create_root_signature = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateConstantBufferView) create_constant_buffer_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateShaderResourceView) create_shader_resource_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateUnorderedAccessView) create_unordered_access_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateRenderTargetView) create_render_target_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateDepthStencilView) create_depth_stencil_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommittedResource) create_committed_resource = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateFence) create_fence = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommandSignature) create_command_signature = nullptr;
};

struct CommandQueueHookState {
  ID3D12CommandQueueVtbl *vtable = nullptr;
  decltype(std::declval<ID3D12CommandQueueVtbl>().ExecuteCommandLists) execute_command_lists = nullptr;
  decltype(std::declval<ID3D12CommandQueueVtbl>().Signal) signal = nullptr;
  decltype(std::declval<ID3D12CommandQueueVtbl>().Wait) wait = nullptr;
};

struct CommandAllocatorHookState {
  ID3D12CommandAllocatorVtbl *vtable = nullptr;
  decltype(std::declval<ID3D12CommandAllocatorVtbl>().Reset) reset = nullptr;
};

struct CommandListHookState {
  ID3D12GraphicsCommandListVtbl *vtable = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().QueryInterface) query_interface = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().Close) close = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().Reset) reset = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ResourceBarrier) resource_barrier = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetDescriptorHeaps) set_descriptor_heaps = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DrawInstanced) draw_instanced = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DrawIndexedInstanced) draw_indexed_instanced = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().Dispatch) dispatch = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ExecuteIndirect) execute_indirect = nullptr;
};

struct CommandList4HookState {
  ID3D12GraphicsCommandList4Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList4Vtbl>().DispatchRays) dispatch_rays = nullptr;
};

struct CommandList6HookState {
  ID3D12GraphicsCommandList6Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList6Vtbl>().DispatchMesh) dispatch_mesh = nullptr;
};

struct FenceHookState {
  ID3D12FenceVtbl *vtable = nullptr;
  decltype(std::declval<ID3D12FenceVtbl>().SetEventOnCompletion) set_event_on_completion = nullptr;
  decltype(std::declval<ID3D12FenceVtbl>().Signal) signal = nullptr;
};

struct CaptureState {
  std::once_flag downstream_once;
  DownstreamModule downstream;
  apitrace::trace::ObjectId next_object_id = 1000;
  apitrace::trace::BlobId next_blob_id = 5000;
  std::uint64_t next_sequence = 1;
  std::uint64_t frame_index = 0;
  std::mutex mutex;
  std::unordered_map<const void *, ObjectInfo> objects;
  std::unordered_map<ID3D12DeviceVtbl *, DeviceHookState> device_hooks;
  std::unordered_map<ID3D12CommandQueueVtbl *, CommandQueueHookState> queue_hooks;
  std::unordered_map<ID3D12CommandAllocatorVtbl *, CommandAllocatorHookState> allocator_hooks;
  std::unordered_map<ID3D12GraphicsCommandListVtbl *, CommandListHookState> command_list_hooks;
  std::unordered_map<ID3D12GraphicsCommandList4Vtbl *, CommandList4HookState> command_list4_hooks;
  std::unordered_map<ID3D12GraphicsCommandList6Vtbl *, CommandList6HookState> command_list6_hooks;
  std::unordered_map<ID3D12FenceVtbl *, FenceHookState> fence_hooks;
};

CaptureState &capture_state()
{
  static CaptureState state;
  return state;
}

std::string downstream_path()
{
  if (const char *explicit_path = std::getenv("APITRACE_DOWNSTREAM_D3D12")) {
    if (*explicit_path != '\0') {
      return explicit_path;
    }
  }

  char system_directory[MAX_PATH] = {};
  const auto length = GetSystemDirectoryA(system_directory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return "C:\\windows\\system32\\d3d12.dll";
  }

  std::string path(system_directory, length);
  path += "\\d3d12.dll";
  return path;
}

void proxy_debug_log(const char *message)
{
  const char *path = std::getenv("APITRACE_D3D12_PROXY_LOG");
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
  const char *path = std::getenv("APITRACE_D3D12_PROXY_LOG");
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
  auto &state = capture_state();
  std::call_once(state.downstream_once, [&state]() {
    const auto path = downstream_path();
    state.downstream.module = LoadLibraryA(path.c_str());
    if (!state.downstream.module) {
      return;
    }

    state.downstream.create_device = reinterpret_cast<D3D12CreateDeviceFn>(
        GetProcAddress(state.downstream.module, "D3D12CreateDevice"));
    state.downstream.get_debug_interface = reinterpret_cast<D3D12GetDebugInterfaceFn>(
        GetProcAddress(state.downstream.module, "D3D12GetDebugInterface"));
    state.downstream.serialize_root_signature = reinterpret_cast<D3D12SerializeRootSignatureFn>(
        GetProcAddress(state.downstream.module, "D3D12SerializeRootSignature"));
    state.downstream.serialize_versioned_root_signature =
        reinterpret_cast<D3D12SerializeVersionedRootSignatureFn>(
            GetProcAddress(state.downstream.module, "D3D12SerializeVersionedRootSignature"));
    state.downstream.create_root_signature_deserializer =
        reinterpret_cast<D3D12CreateRootSignatureDeserializerFn>(
            GetProcAddress(state.downstream.module, "D3D12CreateRootSignatureDeserializer"));
    state.downstream.create_versioned_root_signature_deserializer =
        reinterpret_cast<D3D12CreateVersionedRootSignatureDeserializerFn>(
            GetProcAddress(state.downstream.module, "D3D12CreateVersionedRootSignatureDeserializer"));
    state.downstream.enable_experimental_features = reinterpret_cast<D3D12EnableExperimentalFeaturesFn>(
        GetProcAddress(state.downstream.module, "D3D12EnableExperimentalFeatures"));
  });
  return state.downstream;
}

template <typename VTable, typename Field>
void patch_vtable_field(VTable *vtable, Field VTable::*field, Field replacement)
{
  using NtProtectVirtualMemoryFn = LONG(WINAPI *)(HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);
  static auto *nt_protect_virtual_memory = reinterpret_cast<NtProtectVirtualMemoryFn>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtProtectVirtualMemory"));

  auto *slot = &(vtable->*field);
  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  const auto page_size = static_cast<std::uintptr_t>(system_info.dwPageSize ? system_info.dwPageSize : 4096);
  const auto address = reinterpret_cast<std::uintptr_t>(slot);
  const auto page_start = address & ~(page_size - 1);
  const auto protect_size = (address - page_start) + sizeof(*slot);

  DWORD old_protect = 0;
  if (nt_protect_virtual_memory) {
    PVOID protect_base = reinterpret_cast<void *>(page_start);
    SIZE_T nt_protect_size = protect_size;
    ULONG nt_old_protect = 0;
    if (nt_protect_virtual_memory(
            GetCurrentProcess(),
            &protect_base,
            &nt_protect_size,
            PAGE_EXECUTE_READWRITE,
            &nt_old_protect) >= 0) {
      *slot = replacement;
      ULONG ignored = 0;
      nt_protect_virtual_memory(GetCurrentProcess(), &protect_base, &nt_protect_size, nt_old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
      return;
    }
  }

  if (!VirtualProtect(reinterpret_cast<void *>(page_start), protect_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
    SIZE_T written = 0;
    WriteProcessMemory(GetCurrentProcess(), slot, &replacement, sizeof(replacement), &written);
    return;
  }
  *slot = replacement;
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void *>(page_start), protect_size, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
}

std::size_t readable_vtable_clone_size(const void *source, std::size_t minimum_size)
{
  constexpr std::size_t kExtraVTableSlots = 64;
  constexpr std::size_t kExtraVTableBytes = kExtraVTableSlots * sizeof(void *);
  if (!source) {
    return minimum_size;
  }
  MEMORY_BASIC_INFORMATION info = {};
  if (!VirtualQuery(source, &info, sizeof(info))) {
    return minimum_size;
  }
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
    return minimum_size;
  }
  const auto *region_begin = static_cast<const unsigned char *>(info.BaseAddress);
  const auto *region_end = region_begin + info.RegionSize;
  const auto *vtable_begin = static_cast<const unsigned char *>(source);
  if (vtable_begin < region_begin || vtable_begin >= region_end) {
    return minimum_size;
  }
  const auto readable_bytes = static_cast<std::size_t>(region_end - vtable_begin);
  const auto desired_size = minimum_size + kExtraVTableBytes;
  return std::min(readable_bytes, desired_size);
}

template <typename VTable>
VTable *clone_vtable_bytes(const VTable *source, std::size_t size)
{
  if (!source) {
    return nullptr;
  }
  if (size < sizeof(VTable)) {
    size = sizeof(VTable);
  }
  void *memory = std::malloc(size);
  if (!memory) {
    return nullptr;
  }
  std::memcpy(memory, source, size);
  return static_cast<VTable *>(memory);
}

template <typename VTable>
VTable *clone_vtable(const VTable *source)
{
  return clone_vtable_bytes(source, readable_vtable_clone_size(source, sizeof(VTable)));
}

CommandListHookState command_list_hook_for(ID3D12GraphicsCommandList *command_list);

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

class ScopedCommandListArrayOriginalVTables {
public:
  ScopedCommandListArrayOriginalVTables(UINT count, ID3D12CommandList *const *command_lists)
  {
    if (!command_lists) {
      return;
    }
    entries_.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      ID3D12CommandList *command_list = command_lists[index];
      if (!command_list || !command_list->lpVtbl) {
        continue;
      }
      auto *graphics_list = reinterpret_cast<ID3D12GraphicsCommandList *>(command_list);
      const auto hook = command_list_hook_for(graphics_list);
      if (!hook.vtable) {
        continue;
      }
      entries_.push_back({command_list, command_list->lpVtbl});
      command_list->lpVtbl = reinterpret_cast<ID3D12CommandListVtbl *>(hook.vtable);
    }
  }

  ~ScopedCommandListArrayOriginalVTables()
  {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      it->object->lpVtbl = it->patched_vtable;
    }
  }

private:
  struct Entry {
    ID3D12CommandList *object = nullptr;
    ID3D12CommandListVtbl *patched_vtable = nullptr;
  };

  std::vector<Entry> entries_;
};

std::string json_escape(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (const unsigned char ch : text) {
    switch (ch) {
    case '\"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (ch < 0x20) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(ch));
        escaped += buffer;
      } else {
        escaped.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  return escaped;
}

std::string object_id_json(apitrace::trace::ObjectId object_id)
{
  return object_id == 0 ? "null" : std::to_string(object_id);
}

apitrace::trace::ObjectId lookup_object_id_locked(const void *object)
{
  const auto &state = capture_state();
  const auto it = state.objects.find(object);
  return it == state.objects.end() ? 0 : it->second.object_id;
}

void remember_object_alias_locked(const void *alias, const void *source)
{
  if (!alias || !source) {
    return;
  }
  auto &state = capture_state();
  const auto it = state.objects.find(source);
  if (it != state.objects.end()) {
    state.objects.emplace(alias, it->second);
  }
}

apitrace::trace::ObjectId register_object_locked(
    const void *object,
    apitrace::trace::ObjectKind kind,
    std::string debug_name,
    apitrace::trace::ObjectId parent_object_id = 0)
{
  if (!object) {
    return 0;
  }

  auto &state = capture_state();
  const auto existing = state.objects.find(object);
  if (existing != state.objects.end()) {
    return existing->second.object_id;
  }

  const auto object_id = ++state.next_object_id;
  if (auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12)) {
    apitrace::trace::ObjectRecord record;
    record.object_id = object_id;
    record.kind = kind;
    record.parent_object_id = parent_object_id;
    record.debug_name = std::move(debug_name);
    session->record_object(record);

    state.objects.emplace(
        object,
        ObjectInfo{
            record.object_id,
            record.kind,
            record.parent_object_id,
            record.debug_name,
        });
  }
  return object_id;
}

std::vector<apitrace::trace::ObjectId> collect_object_refs_locked(const std::vector<const void *> &objects)
{
  std::vector<apitrace::trace::ObjectId> refs;
  refs.reserve(objects.size());
  for (const void *object : objects) {
    const auto object_id = lookup_object_id_locked(object);
    if (object_id != 0) {
      refs.push_back(object_id);
    }
  }
  return refs;
}

void record_call_locked(
    std::string function_name,
    HRESULT result_code,
    const std::vector<const void *> &objects,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    std::string payload_json)
{
  if (auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12)) {
    apitrace::trace::EventRecord event;
    event.kind = apitrace::trace::EventKind::Call;
    event.callsite.sequence = capture_state().next_sequence++;
    event.callsite.function_name = std::move(function_name);
    event.callsite.result_code = static_cast<std::int32_t>(result_code);
    event.object_refs = collect_object_refs_locked(objects);
    event.blob_refs = blob_refs;
    event.payload = std::move(payload_json);
    session->append_call_event(event);
  }
}

void record_boundary_locked(apitrace::trace::BoundaryKind boundary, std::string payload_json)
{
  if (auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12)) {
    apitrace::trace::EventRecord event;
    event.kind = apitrace::trace::EventKind::Boundary;
    event.boundary = boundary;
    event.callsite.sequence = capture_state().next_sequence++;
    switch (boundary) {
    case apitrace::trace::BoundaryKind::Frame:
      event.callsite.function_name = "Frame";
      break;
    case apitrace::trace::BoundaryKind::Present:
      event.callsite.function_name = "Present";
      break;
    case apitrace::trace::BoundaryKind::Submit:
      event.callsite.function_name = "Submit";
      break;
    case apitrace::trace::BoundaryKind::Barrier:
      event.callsite.function_name = "ResourceBarrier";
      break;
    case apitrace::trace::BoundaryKind::Fence:
      event.callsite.function_name = "Fence";
      break;
    case apitrace::trace::BoundaryKind::CommandList:
      event.callsite.function_name = "CommandList";
      break;
    case apitrace::trace::BoundaryKind::DebugMarker:
    default:
      event.callsite.function_name = "DebugMarker";
      break;
    }
    event.payload = std::move(payload_json);
    session->append_call_event(event);
  }
}

apitrace::trace::AssetRecord register_asset_bytes_locked(
    apitrace::trace::AssetKind kind,
    std::string debug_name,
    const void *data,
    std::size_t size)
{
  apitrace::trace::AssetRecord asset;
  asset.blob_id = ++capture_state().next_blob_id;
  asset.kind = kind;
  asset.debug_name = std::move(debug_name);
  asset.payload_bytes.resize(size);
  if (size != 0 && data) {
    std::memcpy(asset.payload_bytes.data(), data, size);
  }
  if (auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12)) {
    return session->register_asset(asset);
  }
  return asset;
}

apitrace::trace::AssetRecord register_asset_text_locked(
    apitrace::trace::AssetKind kind,
    std::string debug_name,
    const std::string &text)
{
  return register_asset_bytes_locked(kind, std::move(debug_name), text.data(), text.size());
}

std::string descriptor_handle_json(D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  return std::to_string(static_cast<std::uint64_t>(descriptor.ptr));
}

std::string resource_desc_json(const D3D12_RESOURCE_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{"
          << "\"dimension\":" << static_cast<unsigned int>(desc->Dimension) << ","
          << "\"alignment\":" << desc->Alignment << ","
          << "\"width\":" << desc->Width << ","
          << "\"height\":" << desc->Height << ","
          << "\"depth_or_array_size\":" << desc->DepthOrArraySize << ","
          << "\"mip_levels\":" << desc->MipLevels << ","
          << "\"format\":" << static_cast<unsigned int>(desc->Format) << ","
          << "\"sample_count\":" << desc->SampleDesc.Count << ","
          << "\"sample_quality\":" << desc->SampleDesc.Quality << ","
          << "\"layout\":" << static_cast<unsigned int>(desc->Layout) << ","
          << "\"flags\":" << static_cast<unsigned int>(desc->Flags)
          << "}";
  return payload.str();
}

std::string shader_asset_json_locked(const char *field_name, const D3D12_SHADER_BYTECODE &bytecode, std::vector<apitrace::trace::BlobId> &blob_refs)
{
  if (!bytecode.pShaderBytecode || bytecode.BytecodeLength == 0) {
    return std::string("\"") + field_name + "\":null";
  }
  auto asset = register_asset_bytes_locked(
      apitrace::trace::AssetKind::ShaderDxil,
      std::string("d3d12-") + field_name,
      bytecode.pShaderBytecode,
      bytecode.BytecodeLength);
  blob_refs.push_back(asset.blob_id);
  std::ostringstream payload;
  payload << "\"" << field_name << "\":{"
          << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength) << ","
          << "\"" << field_name << "_path\":\"" << asset.relative_path.generic_string() << "\""
          << "}";
  return payload.str();
}

std::string graphics_pipeline_asset_json_locked(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    std::vector<apitrace::trace::BlobId> &blob_refs)
{
  if (!desc) {
    return "{}";
  }
  std::ostringstream payload;
  payload << "{"
          << "\"type\":\"graphics\","
          << "\"sample_mask\":" << desc->SampleMask << ","
          << "\"primitive_topology_type\":" << static_cast<unsigned int>(desc->PrimitiveTopologyType) << ","
          << "\"rtv_formats\":[";
  for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
    if (index != 0) {
      payload << ",";
    }
    payload << static_cast<unsigned int>(desc->RTVFormats[index]);
  }
  payload << "],"
          << "\"dsv_format\":" << static_cast<unsigned int>(desc->DSVFormat) << ","
          << shader_asset_json_locked("vs", desc->VS, blob_refs) << ","
          << shader_asset_json_locked("ps", desc->PS, blob_refs)
          << "}";
  return payload.str();
}

std::string compute_pipeline_asset_json_locked(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    std::vector<apitrace::trace::BlobId> &blob_refs)
{
  if (!desc) {
    return "{}";
  }
  std::ostringstream payload;
  payload << "{"
          << "\"type\":\"compute\","
          << shader_asset_json_locked("cs", desc->CS, blob_refs)
          << "}";
  return payload.str();
}

void patch_device(ID3D12Device *device, std::size_t vtable_size = sizeof(ID3D12DeviceVtbl));
void patch_command_queue(ID3D12CommandQueue *queue);
void patch_command_allocator(ID3D12CommandAllocator *allocator);
void patch_command_list(ID3D12GraphicsCommandList *command_list);
void patch_command_list4(ID3D12GraphicsCommandList4 *command_list);
void patch_command_list6(ID3D12GraphicsCommandList6 *command_list);
void patch_fence(ID3D12Fence *fence);

DeviceHookState device_hook_for(ID3D12Device *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device_hooks.end() ? DeviceHookState{} : it->second;
}

CommandQueueHookState queue_hook_for(ID3D12CommandQueue *queue)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().queue_hooks.find(queue ? queue->lpVtbl : nullptr);
  return it == capture_state().queue_hooks.end() ? CommandQueueHookState{} : it->second;
}

CommandAllocatorHookState allocator_hook_for(ID3D12CommandAllocator *allocator)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().allocator_hooks.find(allocator ? allocator->lpVtbl : nullptr);
  return it == capture_state().allocator_hooks.end() ? CommandAllocatorHookState{} : it->second;
}

CommandListHookState command_list_hook_for(ID3D12GraphicsCommandList *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().command_list_hooks.find(command_list ? command_list->lpVtbl : nullptr);
  return it == capture_state().command_list_hooks.end() ? CommandListHookState{} : it->second;
}

CommandList4HookState command_list4_hook_for(ID3D12GraphicsCommandList4 *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().command_list4_hooks.find(command_list ? command_list->lpVtbl : nullptr);
  return it == capture_state().command_list4_hooks.end() ? CommandList4HookState{} : it->second;
}

CommandList6HookState command_list6_hook_for(ID3D12GraphicsCommandList6 *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().command_list6_hooks.find(command_list ? command_list->lpVtbl : nullptr);
  return it == capture_state().command_list6_hooks.end() ? CommandList6HookState{} : it->second;
}

FenceHookState fence_hook_for(ID3D12Fence *fence)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().fence_hooks.find(fence ? fence->lpVtbl : nullptr);
  return it == capture_state().fence_hooks.end() ? FenceHookState{} : it->second;
}

void patch_device(ID3D12Device *device, std::size_t vtable_size)
{
  if (!device || !device->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = device->lpVtbl;
  if (capture_state().device_hooks.find(original_vtable) != capture_state().device_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable_bytes(original_vtable, readable_vtable_clone_size(original_vtable, vtable_size));
  if (!vtable) {
    proxy_debug_log("patch_device: failed to clone vtable");
    return;
  }

  DeviceHookState hook;
  hook.vtable = original_vtable;
  hook.query_interface = original_vtable->QueryInterface;
  hook.create_command_queue = original_vtable->CreateCommandQueue;
  hook.create_command_allocator = original_vtable->CreateCommandAllocator;
  hook.create_graphics_pipeline_state = original_vtable->CreateGraphicsPipelineState;
  hook.create_compute_pipeline_state = original_vtable->CreateComputePipelineState;
  hook.create_command_list = original_vtable->CreateCommandList;
  hook.create_descriptor_heap = original_vtable->CreateDescriptorHeap;
  hook.create_root_signature = original_vtable->CreateRootSignature;
  hook.create_constant_buffer_view = original_vtable->CreateConstantBufferView;
  hook.create_shader_resource_view = original_vtable->CreateShaderResourceView;
  hook.create_unordered_access_view = original_vtable->CreateUnorderedAccessView;
  hook.create_render_target_view = original_vtable->CreateRenderTargetView;
  hook.create_depth_stencil_view = original_vtable->CreateDepthStencilView;
  hook.create_committed_resource = original_vtable->CreateCommittedResource;
  hook.create_fence = original_vtable->CreateFence;
  hook.create_command_signature = original_vtable->CreateCommandSignature;
  capture_state().device_hooks.emplace(vtable, hook);
  device->lpVtbl = vtable;

  patch_vtable_field(vtable, &ID3D12DeviceVtbl::QueryInterface, hook_device_query_interface);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandQueue, hook_device_create_command_queue);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandAllocator, hook_device_create_command_allocator);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateGraphicsPipelineState, hook_device_create_graphics_pipeline_state);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateComputePipelineState, hook_device_create_compute_pipeline_state);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandList, hook_device_create_command_list);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateDescriptorHeap, hook_device_create_descriptor_heap);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateRootSignature, hook_device_create_root_signature);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateConstantBufferView, hook_device_create_constant_buffer_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateShaderResourceView, hook_device_create_shader_resource_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateUnorderedAccessView, hook_device_create_unordered_access_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateRenderTargetView, hook_device_create_render_target_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateDepthStencilView, hook_device_create_depth_stencil_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommittedResource, hook_device_create_committed_resource);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateFence, hook_device_create_fence);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandSignature, hook_device_create_command_signature);
  proxy_debug_logf(
      "patch_device vtable=%p pso=%d queue=%d allocator=%d list=%d fence=%d",
      static_cast<void *>(vtable),
      vtable->CreateGraphicsPipelineState == hook_device_create_graphics_pipeline_state,
      vtable->CreateCommandQueue == hook_device_create_command_queue,
      vtable->CreateCommandAllocator == hook_device_create_command_allocator,
      vtable->CreateCommandList == hook_device_create_command_list,
      vtable->CreateFence == hook_device_create_fence);
}

void patch_command_queue(ID3D12CommandQueue *queue)
{
  if (!queue || !queue->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = queue->lpVtbl;
  if (capture_state().queue_hooks.find(original_vtable) != capture_state().queue_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_queue: failed to clone vtable");
    return;
  }
  CommandQueueHookState hook;
  hook.vtable = original_vtable;
  hook.execute_command_lists = original_vtable->ExecuteCommandLists;
  hook.signal = original_vtable->Signal;
  hook.wait = original_vtable->Wait;
  capture_state().queue_hooks.emplace(vtable, hook);
  queue->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12CommandQueueVtbl::ExecuteCommandLists, hook_queue_execute_command_lists);
  patch_vtable_field(vtable, &ID3D12CommandQueueVtbl::Signal, hook_queue_signal);
  patch_vtable_field(vtable, &ID3D12CommandQueueVtbl::Wait, hook_queue_wait);
  proxy_debug_logf(
      "patch_queue vtable=%p execute=%d signal=%d wait=%d",
      static_cast<void *>(vtable),
      vtable->ExecuteCommandLists == hook_queue_execute_command_lists,
      vtable->Signal == hook_queue_signal,
      vtable->Wait == hook_queue_wait);
}

void patch_command_allocator(ID3D12CommandAllocator *allocator)
{
  if (!allocator || !allocator->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = allocator->lpVtbl;
  if (capture_state().allocator_hooks.find(original_vtable) != capture_state().allocator_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_allocator: failed to clone vtable");
    return;
  }
  CommandAllocatorHookState hook;
  hook.vtable = original_vtable;
  hook.reset = original_vtable->Reset;
  capture_state().allocator_hooks.emplace(vtable, hook);
  allocator->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12CommandAllocatorVtbl::Reset, hook_allocator_reset);
  proxy_debug_logf("patch_allocator vtable=%p reset=%d", static_cast<void *>(vtable), vtable->Reset == hook_allocator_reset);
}

void patch_command_list(ID3D12GraphicsCommandList *command_list)
{
  if (!command_list || !command_list->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = command_list->lpVtbl;
  if (capture_state().command_list_hooks.find(original_vtable) != capture_state().command_list_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_list: failed to clone vtable");
    return;
  }
  CommandListHookState hook;
  hook.vtable = original_vtable;
  hook.query_interface = original_vtable->QueryInterface;
  hook.close = original_vtable->Close;
  hook.reset = original_vtable->Reset;
  hook.resource_barrier = original_vtable->ResourceBarrier;
  hook.set_descriptor_heaps = original_vtable->SetDescriptorHeaps;
  hook.draw_instanced = original_vtable->DrawInstanced;
  hook.draw_indexed_instanced = original_vtable->DrawIndexedInstanced;
  hook.dispatch = original_vtable->Dispatch;
  hook.execute_indirect = original_vtable->ExecuteIndirect;
  capture_state().command_list_hooks.emplace(vtable, hook);
  command_list->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::QueryInterface, hook_command_list_query_interface);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Close, hook_command_list_close);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Reset, hook_command_list_reset);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ResourceBarrier, hook_command_list_resource_barrier);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetDescriptorHeaps, hook_command_list_set_descriptor_heaps);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DrawInstanced, hook_command_list_draw_instanced);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DrawIndexedInstanced, hook_command_list_draw_indexed_instanced);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Dispatch, hook_command_list_dispatch);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ExecuteIndirect, hook_command_list_execute_indirect);
  proxy_debug_logf(
      "patch_list vtable=%p qi=%d close=%d reset=%d barrier=%d heaps=%d draw=%d draw_indexed=%d dispatch=%d indirect=%d",
      static_cast<void *>(vtable),
      vtable->QueryInterface == hook_command_list_query_interface,
      vtable->Close == hook_command_list_close,
      vtable->Reset == hook_command_list_reset,
      vtable->ResourceBarrier == hook_command_list_resource_barrier,
      vtable->SetDescriptorHeaps == hook_command_list_set_descriptor_heaps,
      vtable->DrawInstanced == hook_command_list_draw_instanced,
      vtable->DrawIndexedInstanced == hook_command_list_draw_indexed_instanced,
      vtable->Dispatch == hook_command_list_dispatch,
      vtable->ExecuteIndirect == hook_command_list_execute_indirect);
}

void patch_command_list4(ID3D12GraphicsCommandList4 *command_list)
{
  if (!command_list || !command_list->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = command_list->lpVtbl;
  if (capture_state().command_list4_hooks.find(original_vtable) != capture_state().command_list4_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_list4: failed to clone vtable");
    return;
  }
  CommandList4HookState hook;
  hook.vtable = original_vtable;
  hook.dispatch_rays = original_vtable->DispatchRays;
  capture_state().command_list4_hooks.emplace(vtable, hook);
  command_list->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList4Vtbl::DispatchRays, hook_command_list_dispatch_rays);
  proxy_debug_logf(
      "patch_list4 vtable=%p dispatch_rays=%d",
      static_cast<void *>(vtable),
      vtable->DispatchRays == hook_command_list_dispatch_rays);
}

void patch_command_list6(ID3D12GraphicsCommandList6 *command_list)
{
  if (!command_list || !command_list->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = command_list->lpVtbl;
  if (capture_state().command_list6_hooks.find(original_vtable) != capture_state().command_list6_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_list6: failed to clone vtable");
    return;
  }
  CommandList6HookState hook;
  hook.vtable = original_vtable;
  hook.dispatch_mesh = original_vtable->DispatchMesh;
  capture_state().command_list6_hooks.emplace(vtable, hook);
  command_list->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList6Vtbl::DispatchMesh, hook_command_list_dispatch_mesh);
  proxy_debug_logf(
      "patch_list6 vtable=%p dispatch_mesh=%d",
      static_cast<void *>(vtable),
      vtable->DispatchMesh == hook_command_list_dispatch_mesh);
}

void patch_fence(ID3D12Fence *fence)
{
  if (!fence || !fence->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = fence->lpVtbl;
  if (capture_state().fence_hooks.find(original_vtable) != capture_state().fence_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_fence: failed to clone vtable");
    return;
  }
  FenceHookState hook;
  hook.vtable = original_vtable;
  hook.set_event_on_completion = original_vtable->SetEventOnCompletion;
  hook.signal = original_vtable->Signal;
  capture_state().fence_hooks.emplace(vtable, hook);
  fence->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12FenceVtbl::SetEventOnCompletion, hook_fence_set_event_on_completion);
  patch_vtable_field(vtable, &ID3D12FenceVtbl::Signal, hook_fence_signal);
  proxy_debug_logf(
      "patch_fence vtable=%p set_event=%d signal=%d",
      static_cast<void *>(vtable),
      vtable->SetEventOnCompletion == hook_fence_set_event_on_completion,
      vtable->Signal == hook_fence_signal);
}

void patch_known_command_queues()
{
  std::vector<ID3D12CommandQueue *> queues;
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    for (const auto &[object, info] : capture_state().objects) {
      if (info.kind == apitrace::trace::ObjectKind::CommandQueue) {
        queues.push_back(static_cast<ID3D12CommandQueue *>(const_cast<void *>(object)));
      }
    }
  }
  for (auto *queue : queues) {
    patch_command_queue(queue);
  }
}

void patch_known_devices()
{
  std::vector<ID3D12Device *> devices;
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    for (const auto &[object, info] : capture_state().objects) {
      if (info.kind == apitrace::trace::ObjectKind::Device) {
        devices.push_back(static_cast<ID3D12Device *>(const_cast<void *>(object)));
      }
    }
  }
  for (auto *device : devices) {
    patch_device(device);
  }
}

void record_scene_marker(const char *scene_name, const char *dx_mode, const char *phase)
{
  if (phase && std::strcmp(phase, "start") == 0) {
    patch_known_devices();
    patch_known_command_queues();
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream marker_payload;
  marker_payload << "{"
                 << "\"scene_name\":\"" << json_escape(scene_name ? scene_name : "") << "\","
                 << "\"dx_mode\":\"" << json_escape(dx_mode ? dx_mode : "") << "\","
                 << "\"phase\":\"" << json_escape(phase ? phase : "") << "\""
                 << "}";
  record_boundary_locked(apitrace::trace::BoundaryKind::DebugMarker, marker_payload.str());

  if (phase && std::strcmp(phase, "start") == 0) {
    std::ostringstream payload;
    payload << "{\"label\":\"FrameBegin\",\"frame_index\":" << capture_state().frame_index
            << ",\"scene_name\":\"" << json_escape(scene_name ? scene_name : "") << "\"}";
    record_boundary_locked(apitrace::trace::BoundaryKind::Frame, payload.str());
  } else if (phase && std::strcmp(phase, "end") == 0) {
    std::ostringstream payload;
    payload << "{\"label\":\"Present\",\"frame_index\":" << capture_state().frame_index++
            << ",\"scene_name\":\"" << json_escape(scene_name ? scene_name : "") << "\"}";
    record_boundary_locked(apitrace::trace::BoundaryKind::Present, payload.str());
  }
}

bool is_device_iid(REFIID riid)
{
  return IsEqualGUID(riid, kIidD3D12Device) ||
         IsEqualGUID(riid, kIidD3D12Device1) ||
         IsEqualGUID(riid, kIidD3D12Device2) ||
         IsEqualGUID(riid, kIidD3D12Device3) ||
         IsEqualGUID(riid, kIidD3D12Device4) ||
         IsEqualGUID(riid, kIidD3D12Device5);
}

std::size_t device_vtable_size_for_iid(REFIID riid)
{
  if (IsEqualGUID(riid, kIidD3D12Device5)) {
    return sizeof(ID3D12Device5Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device4)) {
    return sizeof(ID3D12Device4Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device3)) {
    return sizeof(ID3D12Device3Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device2)) {
    return sizeof(ID3D12Device2Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device1)) {
    return sizeof(ID3D12Device1Vtbl);
  }
  return sizeof(ID3D12DeviceVtbl);
}

HRESULT STDMETHODCALLTYPE hook_device_query_interface(ID3D12Device *self, REFIID riid, void **object)
{
  const auto hook = device_hook_for(self);
  if (!hook.query_interface) {
    return E_NOINTERFACE;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.query_interface(self, riid, object);
  if (SUCCEEDED(hr) && object && *object) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      remember_object_alias_locked(*object, self);
      record_call_locked("ID3D12Device::QueryInterface", hr, {self, *object}, {}, "{}");
    }
    if (is_device_iid(riid)) {
      patch_device(static_cast<ID3D12Device *>(*object), device_vtable_size_for_iid(riid));
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_queue(
    ID3D12Device *self,
    const D3D12_COMMAND_QUEUE_DESC *desc,
    REFIID riid,
    void **command_queue)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_command_queue) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_command_queue(self, desc, riid, command_queue);
  if (SUCCEEDED(hr) && command_queue && *command_queue) {
    proxy_debug_logf("hook_device_create_command_queue self=%p queue=%p", self, *command_queue);
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_object_locked(*command_queue, apitrace::trace::ObjectKind::CommandQueue, "ID3D12CommandQueue", parent);
      std::ostringstream payload;
      payload << "{"
              << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
              << "\"priority\":" << (desc ? desc->Priority : 0) << ","
              << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0) << ","
              << "\"node_mask\":" << (desc ? desc->NodeMask : 0)
              << "}";
      record_call_locked("ID3D12Device::CreateCommandQueue", hr, {self, *command_queue}, {}, payload.str());
    }
    patch_command_queue(static_cast<ID3D12CommandQueue *>(*command_queue));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_allocator(
    ID3D12Device *self,
    D3D12_COMMAND_LIST_TYPE type,
    REFIID riid,
    void **command_allocator)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_command_allocator) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_command_allocator(self, type, riid, command_allocator);
  if (SUCCEEDED(hr) && command_allocator && *command_allocator) {
    proxy_debug_logf("hook_device_create_command_allocator self=%p allocator=%p", self, *command_allocator);
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_object_locked(*command_allocator, apitrace::trace::ObjectKind::CommandAllocator, "ID3D12CommandAllocator", parent);
      std::ostringstream payload;
      payload << "{\"type\":" << static_cast<unsigned int>(type) << "}";
      record_call_locked("ID3D12Device::CreateCommandAllocator", hr, {self, *command_allocator}, {}, payload.str());
    }
    patch_command_allocator(static_cast<ID3D12CommandAllocator *>(*command_allocator));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_graphics_pipeline_state(
    ID3D12Device *self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    REFIID riid,
    void **pipeline_state)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_graphics_pipeline_state) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_graphics_pipeline_state(self, desc, riid, pipeline_state);
  if (SUCCEEDED(hr) && pipeline_state && *pipeline_state) {
    proxy_debug_logf("hook_device_create_graphics_pipeline_state self=%p pso=%p", self, *pipeline_state);
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_object_locked(*pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
    std::vector<apitrace::trace::BlobId> shader_blob_refs;
    const auto pipeline_json = graphics_pipeline_asset_json_locked(desc, shader_blob_refs);
    const auto pipeline_asset = register_asset_text_locked(apitrace::trace::AssetKind::Pipeline, "d3d12-graphics-pipeline", pipeline_json);
    std::vector<apitrace::trace::BlobId> blob_refs = {pipeline_asset.blob_id};
    std::ostringstream payload;
    payload << "{\"pipeline_path\":\"" << pipeline_asset.relative_path.generic_string() << "\"}";
    record_call_locked("ID3D12Device::CreateGraphicsPipelineState", hr, {self, *pipeline_state}, blob_refs, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_compute_pipeline_state(
    ID3D12Device *self,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    REFIID riid,
    void **pipeline_state)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_compute_pipeline_state) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_compute_pipeline_state(self, desc, riid, pipeline_state);
  if (SUCCEEDED(hr) && pipeline_state && *pipeline_state) {
    proxy_debug_logf("hook_device_create_compute_pipeline_state self=%p pso=%p", self, *pipeline_state);
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_object_locked(*pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
    std::vector<apitrace::trace::BlobId> shader_blob_refs;
    const auto pipeline_json = compute_pipeline_asset_json_locked(desc, shader_blob_refs);
    const auto pipeline_asset = register_asset_text_locked(apitrace::trace::AssetKind::Pipeline, "d3d12-compute-pipeline", pipeline_json);
    std::vector<apitrace::trace::BlobId> blob_refs = {pipeline_asset.blob_id};
    std::ostringstream payload;
    payload << "{\"pipeline_path\":\"" << pipeline_asset.relative_path.generic_string() << "\"}";
    record_call_locked("ID3D12Device::CreateComputePipelineState", hr, {self, *pipeline_state}, blob_refs, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_list(
    ID3D12Device *self,
    UINT node_mask,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *command_allocator,
    ID3D12PipelineState *initial_pipeline_state,
    REFIID riid,
    void **command_list)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_command_list) {
    return E_FAIL;
  }
  const auto allocator_hook = allocator_hook_for(command_allocator);
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12CommandAllocator, ID3D12CommandAllocatorVtbl> original_allocator_vtable(
      command_allocator,
      allocator_hook.vtable);
  const HRESULT hr = hook.create_command_list(self, node_mask, type, command_allocator, initial_pipeline_state, riid, command_list);
  if (SUCCEEDED(hr) && command_list && *command_list) {
    proxy_debug_logf("hook_device_create_command_list self=%p list=%p", self, *command_list);
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_object_locked(*command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", parent);
      std::ostringstream payload;
      payload << "{\"node_mask\":" << node_mask << ",\"type\":" << static_cast<unsigned int>(type) << "}";
      record_call_locked("ID3D12Device::CreateCommandList", hr, {self, command_allocator, initial_pipeline_state, *command_list}, {}, payload.str());
    }
    patch_command_list(static_cast<ID3D12GraphicsCommandList *>(*command_list));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_descriptor_heap(
    ID3D12Device *self,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc,
    REFIID riid,
    void **descriptor_heap)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_descriptor_heap) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_descriptor_heap(self, desc, riid, descriptor_heap);
  if (SUCCEEDED(hr) && descriptor_heap && *descriptor_heap) {
    proxy_debug_logf(
        "hook_device_create_descriptor_heap self=%p heap=%p type=%u",
        self,
        *descriptor_heap,
        desc ? static_cast<unsigned int>(desc->Type) : 0U);
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_object_locked(*descriptor_heap, apitrace::trace::ObjectKind::DescriptorHeap, "ID3D12DescriptorHeap", parent);
    std::ostringstream payload;
    payload << "{"
            << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
            << "\"num_descriptors\":" << (desc ? desc->NumDescriptors : 0) << ","
            << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0) << ","
            << "\"node_mask\":" << (desc ? desc->NodeMask : 0)
            << "}";
    record_call_locked("ID3D12Device::CreateDescriptorHeap", hr, {self, *descriptor_heap}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_root_signature(
    ID3D12Device *self,
    UINT node_mask,
    const void *bytecode,
    SIZE_T bytecode_length,
    REFIID riid,
    void **root_signature)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_root_signature) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_root_signature(self, node_mask, bytecode, bytecode_length, riid, root_signature);
  if (SUCCEEDED(hr) && root_signature && *root_signature) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_object_locked(*root_signature, apitrace::trace::ObjectKind::RootSignature, "ID3D12RootSignature", parent);
    std::vector<apitrace::trace::BlobId> blob_refs;
    std::string root_sig_path;
    if (bytecode && bytecode_length != 0) {
      const auto asset = register_asset_bytes_locked(apitrace::trace::AssetKind::RootSignature, "d3d12-root-signature", bytecode, bytecode_length);
      blob_refs.push_back(asset.blob_id);
      root_sig_path = asset.relative_path.generic_string();
    }
    std::ostringstream payload;
    payload << "{\"node_mask\":" << node_mask << ",\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode_length);
    if (!root_sig_path.empty()) {
      payload << ",\"root_signature_path\":\"" << root_sig_path << "\"";
    }
    payload << "}";
    record_call_locked("ID3D12Device::CreateRootSignature", hr, {self, *root_signature}, blob_refs, payload.str());
  }
  return hr;
}

void STDMETHODCALLTYPE hook_device_create_constant_buffer_view(
    ID3D12Device *self,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_constant_buffer_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_constant_buffer_view(self, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"buffer_location\":" << (desc ? desc->BufferLocation : 0)
          << ",\"size_in_bytes\":" << (desc ? desc->SizeInBytes : 0) << "}";
  record_call_locked("ID3D12Device::CreateConstantBufferView", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_shader_resource_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_shader_resource_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_shader_resource_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0) << "}";
  record_call_locked("ID3D12Device::CreateShaderResourceView", S_OK, {self, resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_unordered_access_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    ID3D12Resource *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_unordered_access_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_unordered_access_view(self, resource, counter_resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0) << "}";
  record_call_locked("ID3D12Device::CreateUnorderedAccessView", S_OK, {self, resource, counter_resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_render_target_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_render_target_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_render_target_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0) << "}";
  record_call_locked("ID3D12Device::CreateRenderTargetView", S_OK, {self, resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_depth_stencil_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_depth_stencil_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_depth_stencil_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0) << "}";
  record_call_locked("ID3D12Device::CreateDepthStencilView", S_OK, {self, resource}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_device_create_committed_resource(
    ID3D12Device *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_committed_resource) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_committed_resource(self, heap_properties, heap_flags, desc, initial_state, optimized_clear_value, riid, resource);
  if (SUCCEEDED(hr) && resource && *resource) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_object_locked(*resource, apitrace::trace::ObjectKind::Resource, "ID3D12Resource", parent);
    std::ostringstream payload;
    payload << "{\"heap_type\":" << (heap_properties ? static_cast<unsigned int>(heap_properties->Type) : 0)
            << ",\"heap_flags\":" << static_cast<unsigned int>(heap_flags)
            << ",\"initial_state\":" << static_cast<unsigned int>(initial_state)
            << ",\"resource_desc\":" << resource_desc_json(desc) << "}";
    record_call_locked("ID3D12Device::CreateCommittedResource", hr, {self, *resource}, {}, payload.str());
  }
  (void)optimized_clear_value;
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_fence(
    ID3D12Device *self,
    UINT64 initial_value,
    D3D12_FENCE_FLAGS flags,
    REFIID riid,
    void **fence)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_fence) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_fence(self, initial_value, flags, riid, fence);
  if (SUCCEEDED(hr) && fence && *fence) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_object_locked(*fence, apitrace::trace::ObjectKind::Fence, "ID3D12Fence", parent);
      std::ostringstream payload;
      payload << "{\"initial_value\":" << initial_value << ",\"flags\":" << static_cast<unsigned int>(flags) << "}";
      record_call_locked("ID3D12Device::CreateFence", hr, {self, *fence}, {}, payload.str());
    }
    patch_fence(static_cast<ID3D12Fence *>(*fence));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_signature(
    ID3D12Device *self,
    const D3D12_COMMAND_SIGNATURE_DESC *desc,
    ID3D12RootSignature *root_signature,
    REFIID riid,
    void **command_signature)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_command_signature) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_command_signature(self, desc, root_signature, riid, command_signature);
  if (SUCCEEDED(hr) && command_signature && *command_signature) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_object_locked(*command_signature, apitrace::trace::ObjectKind::CommandSignature, "ID3D12CommandSignature", parent);
    std::ostringstream payload;
    payload << "{\"byte_stride\":" << (desc ? desc->ByteStride : 0)
            << ",\"argument_count\":" << (desc ? desc->NumArgumentDescs : 0)
            << ",\"node_mask\":" << (desc ? desc->NodeMask : 0) << "}";
    record_call_locked("ID3D12Device::CreateCommandSignature", hr, {self, root_signature, *command_signature}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_allocator_reset(ID3D12CommandAllocator *self)
{
  const auto hook = allocator_hook_for(self);
  if (!hook.reset) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12CommandAllocator, ID3D12CommandAllocatorVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.reset(self);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12CommandAllocator::Reset", hr, {self}, {}, "{}");
  return hr;
}

void STDMETHODCALLTYPE hook_queue_execute_command_lists(
    ID3D12CommandQueue *self,
    UINT command_list_count,
    ID3D12CommandList *const *command_lists)
{
  const auto hook = queue_hook_for(self);
  if (hook.execute_command_lists) {
    ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
    ScopedCommandListArrayOriginalVTables original_command_list_vtables(command_list_count, command_lists);
    hook.execute_command_lists(self, command_list_count, command_lists);
  }
  proxy_debug_logf("hook_queue_execute_command_lists self=%p count=%u", self, command_list_count);
  std::vector<const void *> refs = {self};
  for (UINT index = 0; index < command_list_count; ++index) {
    refs.push_back(command_lists ? command_lists[index] : nullptr);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"command_list_count\":" << command_list_count << "}";
  record_call_locked("ID3D12CommandQueue::ExecuteCommandLists", S_OK, refs, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Submit, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_queue_signal(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value)
{
  const auto hook = queue_hook_for(self);
  if (!hook.signal) {
    return E_FAIL;
  }
  const auto fence_hook = fence_hook_for(fence);
  ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_fence_vtable(fence, fence_hook.vtable);
  const HRESULT hr = hook.signal(self, fence, value);
  proxy_debug_logf("hook_queue_signal self=%p fence=%p value=%llu", self, fence, static_cast<unsigned long long>(value));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12CommandQueue::Signal", hr, {self, fence}, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Fence, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_queue_wait(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value)
{
  const auto hook = queue_hook_for(self);
  if (!hook.wait) {
    return E_FAIL;
  }
  const auto fence_hook = fence_hook_for(fence);
  ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_fence_vtable(fence, fence_hook.vtable);
  const HRESULT hr = hook.wait(self, fence, value);
  proxy_debug_logf("hook_queue_wait self=%p fence=%p value=%llu", self, fence, static_cast<unsigned long long>(value));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12CommandQueue::Wait", hr, {self, fence}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_command_list_query_interface(ID3D12GraphicsCommandList *self, REFIID riid, void **object)
{
  const auto hook = command_list_hook_for(self);
  if (!hook.query_interface) {
    return E_NOINTERFACE;
  }
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.query_interface(self, riid, object);
  if (SUCCEEDED(hr) && object && *object) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      remember_object_alias_locked(*object, self);
      record_call_locked("ID3D12GraphicsCommandList::QueryInterface", hr, {self, *object}, {}, "{}");
    }
    if (IsEqualGUID(riid, kIidD3D12GraphicsCommandList4)) {
      patch_command_list4(static_cast<ID3D12GraphicsCommandList4 *>(*object));
    } else if (IsEqualGUID(riid, kIidD3D12GraphicsCommandList6)) {
      patch_command_list6(static_cast<ID3D12GraphicsCommandList6 *>(*object));
    } else {
      patch_command_list(static_cast<ID3D12GraphicsCommandList *>(*object));
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_command_list_close(ID3D12GraphicsCommandList *self)
{
  const auto hook = command_list_hook_for(self);
  if (!hook.close) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.close(self);
  proxy_debug_logf("hook_command_list_close self=%p hr=0x%08lx", self, static_cast<unsigned long>(hr));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::Close", hr, {self}, {}, "{}");
  record_boundary_locked(apitrace::trace::BoundaryKind::CommandList, "{\"label\":\"Close\"}");
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_command_list_reset(
    ID3D12GraphicsCommandList *self,
    ID3D12CommandAllocator *allocator,
    ID3D12PipelineState *initial_state)
{
  const auto hook = command_list_hook_for(self);
  if (!hook.reset) {
    return E_FAIL;
  }
  const auto allocator_hook = allocator_hook_for(allocator);
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12CommandAllocator, ID3D12CommandAllocatorVtbl> original_allocator_vtable(
      allocator,
      allocator_hook.vtable);
  const HRESULT hr = hook.reset(self, allocator, initial_state);
  proxy_debug_logf("hook_command_list_reset self=%p allocator=%p hr=0x%08lx", self, allocator, static_cast<unsigned long>(hr));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::Reset", hr, {self, allocator, initial_state}, {}, "{}");
  return hr;
}

void STDMETHODCALLTYPE hook_command_list_resource_barrier(
    ID3D12GraphicsCommandList *self,
    UINT barrier_count,
    const D3D12_RESOURCE_BARRIER *barriers)
{
  const auto hook = command_list_hook_for(self);
  if (hook.resource_barrier) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.resource_barrier(self, barrier_count, barriers);
  }
  proxy_debug_logf("hook_command_list_resource_barrier self=%p count=%u", self, barrier_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::vector<const void *> refs = {self};
  std::ostringstream payload;
  payload << "{\"barrier_count\":" << barrier_count << ",\"barriers\":[";
  for (UINT index = 0; index < barrier_count; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &barrier = barriers[index];
    payload << "{\"type\":" << static_cast<unsigned int>(barrier.Type)
            << ",\"flags\":" << static_cast<unsigned int>(barrier.Flags);
    if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      refs.push_back(barrier.Transition.pResource);
      payload << ",\"resource_object_id\":" << object_id_json(lookup_object_id_locked(barrier.Transition.pResource))
              << ",\"before\":" << static_cast<unsigned int>(barrier.Transition.StateBefore)
              << ",\"after\":" << static_cast<unsigned int>(barrier.Transition.StateAfter)
              << ",\"subresource\":" << barrier.Transition.Subresource;
    }
    payload << "}";
  }
  payload << "]}";
  record_call_locked("ID3D12GraphicsCommandList::ResourceBarrier", S_OK, refs, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Barrier, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_descriptor_heaps(
    ID3D12GraphicsCommandList *self,
    UINT heap_count,
    ID3D12DescriptorHeap *const *heaps)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_descriptor_heaps) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_descriptor_heaps(self, heap_count, heaps);
  }
  proxy_debug_logf("hook_command_list_set_descriptor_heaps self=%p heap_count=%u", self, heap_count);
  std::vector<const void *> refs = {self};
  for (UINT index = 0; index < heap_count; ++index) {
    refs.push_back(heaps ? heaps[index] : nullptr);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"heap_count\":" << heap_count << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetDescriptorHeaps", S_OK, refs, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_draw_instanced(
    ID3D12GraphicsCommandList *self,
    UINT vertex_count_per_instance,
    UINT instance_count,
    UINT start_vertex_location,
    UINT start_instance_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.draw_instanced) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.draw_instanced(self, vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location);
  }
  proxy_debug_logf("hook_command_list_draw_instanced self=%p instance_count=%u", self, instance_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"vertex_count_per_instance\":" << vertex_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_vertex_location\":" << start_vertex_location
          << ",\"start_instance_location\":" << start_instance_location << "}";
  record_call_locked("ID3D12GraphicsCommandList::DrawInstanced", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_draw_indexed_instanced(
    ID3D12GraphicsCommandList *self,
    UINT index_count_per_instance,
    UINT instance_count,
    UINT start_index_location,
    INT base_vertex_location,
    UINT start_instance_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.draw_indexed_instanced) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.draw_indexed_instanced(self, index_count_per_instance, instance_count, start_index_location, base_vertex_location, start_instance_location);
  }
  proxy_debug_logf("hook_command_list_draw_indexed_instanced self=%p index_count=%u", self, index_count_per_instance);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"index_count_per_instance\":" << index_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_index_location\":" << start_index_location
          << ",\"base_vertex_location\":" << base_vertex_location
          << ",\"start_instance_location\":" << start_instance_location << "}";
  record_call_locked("ID3D12GraphicsCommandList::DrawIndexedInstanced", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_dispatch(ID3D12GraphicsCommandList *self, UINT x, UINT y, UINT z)
{
  const auto hook = command_list_hook_for(self);
  if (hook.dispatch) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.dispatch(self, x, y, z);
  }
  proxy_debug_logf("hook_command_list_dispatch self=%p groups=%u,%u,%u", self, x, y, z);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"thread_group_count_x\":" << x << ",\"thread_group_count_y\":" << y << ",\"thread_group_count_z\":" << z << "}";
  record_call_locked("ID3D12GraphicsCommandList::Dispatch", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_execute_indirect(
    ID3D12GraphicsCommandList *self,
    ID3D12CommandSignature *command_signature,
    UINT max_command_count,
    ID3D12Resource *arg_buffer,
    UINT64 arg_buffer_offset,
    ID3D12Resource *count_buffer,
    UINT64 count_buffer_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.execute_indirect) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.execute_indirect(self, command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset);
  }
  proxy_debug_logf("hook_command_list_execute_indirect self=%p max=%u", self, max_command_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"max_command_count\":" << max_command_count
          << ",\"arg_buffer_offset\":" << arg_buffer_offset
          << ",\"count_buffer_offset\":" << count_buffer_offset << "}";
  record_call_locked("ID3D12GraphicsCommandList::ExecuteIndirect", S_OK, {self, command_signature, arg_buffer, count_buffer}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_dispatch_rays(
    ID3D12GraphicsCommandList4 *self,
    const D3D12_DISPATCH_RAYS_DESC *desc)
{
  const auto hook = command_list4_hook_for(self);
  if (hook.dispatch_rays) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList4Vtbl> original_vtable(self, hook.vtable);
    hook.dispatch_rays(self, desc);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"width\":" << (desc ? desc->Width : 0)
          << ",\"height\":" << (desc ? desc->Height : 0)
          << ",\"depth\":" << (desc ? desc->Depth : 0) << "}";
  record_call_locked("ID3D12GraphicsCommandList4::DispatchRays", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_dispatch_mesh(
    ID3D12GraphicsCommandList6 *self,
    UINT thread_group_count_x,
    UINT thread_group_count_y,
    UINT thread_group_count_z)
{
  const auto hook = command_list6_hook_for(self);
  if (hook.dispatch_mesh) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList6, ID3D12GraphicsCommandList6Vtbl> original_vtable(self, hook.vtable);
    hook.dispatch_mesh(self, thread_group_count_x, thread_group_count_y, thread_group_count_z);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"thread_group_count_x\":" << thread_group_count_x
          << ",\"thread_group_count_y\":" << thread_group_count_y
          << ",\"thread_group_count_z\":" << thread_group_count_z << "}";
  record_call_locked("ID3D12GraphicsCommandList6::DispatchMesh", S_OK, {self}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_fence_set_event_on_completion(ID3D12Fence *self, UINT64 value, HANDLE event)
{
  const auto hook = fence_hook_for(self);
  if (!hook.set_event_on_completion) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.set_event_on_completion(self, value, event);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12Fence::SetEventOnCompletion", hr, {self}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_fence_signal(ID3D12Fence *self, UINT64 value)
{
  const auto hook = fence_hook_for(self);
  if (!hook.signal) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.signal(self, value);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12Fence::Signal", hr, {self}, {}, payload.str());
  return hr;
}

void process_attach()
{
}

void process_detach() noexcept
{
  apitrace::runtime::shutdown_process_trace_session();
}

} // namespace

extern "C" HRESULT WINAPI D3D12CreateDevice(
    IUnknown *adapter,
    D3D_FEATURE_LEVEL minimum_feature_level,
    REFIID iid,
    void **device)
{
  auto &downstream = downstream_module();
  if (!downstream.create_device) {
    return E_FAIL;
  }

  const HRESULT hr = downstream.create_device(adapter, minimum_feature_level, iid, device);
  proxy_debug_logf("D3D12CreateDevice hr=0x%08lx device=%p", static_cast<unsigned long>(hr), device ? *device : nullptr);
  if (SUCCEEDED(hr) && device && *device) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      register_object_locked(*device, apitrace::trace::ObjectKind::Device, "ID3D12Device");
      std::ostringstream payload;
      payload << "{\"minimum_feature_level\":" << static_cast<unsigned int>(minimum_feature_level) << "}";
      record_call_locked("D3D12CreateDevice", hr, {*device}, {}, payload.str());
    }
    if (is_device_iid(iid)) {
      patch_device(static_cast<ID3D12Device *>(*device), device_vtable_size_for_iid(iid));
    }
  } else {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    std::ostringstream payload;
    payload << "{\"minimum_feature_level\":" << static_cast<unsigned int>(minimum_feature_level) << "}";
    record_call_locked("D3D12CreateDevice", hr, {}, {}, payload.str());
  }
  return hr;
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
  auto &downstream = downstream_module();
  if (!downstream.get_debug_interface) {
    return E_FAIL;
  }
  return downstream.get_debug_interface(iid, debug);
}

extern "C" void WINAPI apitrace_d3d12_record_runtime_objects(
    ID3D12Device *device,
    ID3D12CommandQueue *queue,
    ID3D12CommandAllocator *allocator,
    ID3D12GraphicsCommandList *command_list,
    ID3D12Fence *fence)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto device_id = register_object_locked(device, apitrace::trace::ObjectKind::Device, "ID3D12Device");

  if (queue) {
    register_object_locked(queue, apitrace::trace::ObjectKind::CommandQueue, "ID3D12CommandQueue", device_id);
    record_call_locked("ID3D12Device::CreateCommandQueue", S_OK, {device, queue}, {}, "{\"type\":0,\"flags\":0}");
  }
  if (allocator) {
    register_object_locked(allocator, apitrace::trace::ObjectKind::CommandAllocator, "ID3D12CommandAllocator", device_id);
    record_call_locked("ID3D12Device::CreateCommandAllocator", S_OK, {device, allocator}, {}, "{\"type\":0}");
  }
  if (command_list) {
    register_object_locked(command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", device_id);
    record_call_locked("ID3D12Device::CreateCommandList", S_OK, {device, allocator, command_list}, {}, "{\"type\":0}");
  }
  if (fence) {
    register_object_locked(fence, apitrace::trace::ObjectKind::Fence, "ID3D12Fence", device_id);
    record_call_locked("ID3D12Device::CreateFence", S_OK, {device, fence}, {}, "{\"initial_value\":0,\"flags\":0}");
  }
}

extern "C" void WINAPI apitrace_d3d12_record_execute_command_lists(
    ID3D12CommandQueue *queue,
    ID3D12GraphicsCommandList *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12CommandQueue::ExecuteCommandLists", S_OK, {queue, command_list}, {}, "{\"command_list_count\":1}");
  record_boundary_locked(apitrace::trace::BoundaryKind::Submit, "{\"queue\":\"ID3D12CommandQueue\"}");
}

extern "C" void WINAPI apitrace_d3d12_record_descriptor_heap(
    ID3D12Device *device,
    ID3D12DescriptorHeap *descriptor_heap,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
  if (!descriptor_heap) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(descriptor_heap, apitrace::trace::ObjectKind::DescriptorHeap, "ID3D12DescriptorHeap", parent);
  std::ostringstream payload;
  payload << "{"
          << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
          << "\"num_descriptors\":" << (desc ? desc->NumDescriptors : 0) << ","
          << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
          << "}";
  record_call_locked("ID3D12Device::CreateDescriptorHeap", S_OK, {device, descriptor_heap}, {}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_resource_barrier(
    ID3D12GraphicsCommandList *command_list,
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    UINT subresource)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"barrier_count\":1,\"barriers\":[{"
          << "\"type\":" << static_cast<unsigned int>(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) << ","
          << "\"flags\":0,"
          << "\"resource_object_id\":" << object_id_json(lookup_object_id_locked(resource)) << ","
          << "\"before\":" << static_cast<unsigned int>(before) << ","
          << "\"after\":" << static_cast<unsigned int>(after) << ","
          << "\"subresource\":" << subresource
          << "}]}";
  record_call_locked("ID3D12GraphicsCommandList::ResourceBarrier", S_OK, {command_list, resource}, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Barrier, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_graphics_pipeline(
    ID3D12Device *device,
    ID3D12PipelineState *pipeline_state,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
  if (!pipeline_state || !desc) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
  std::vector<apitrace::trace::BlobId> shader_blob_refs;
  const auto pipeline_json = graphics_pipeline_asset_json_locked(desc, shader_blob_refs);
  const auto pipeline_asset = register_asset_text_locked(apitrace::trace::AssetKind::Pipeline, "d3d12-graphics-pipeline", pipeline_json);
  std::vector<apitrace::trace::BlobId> blob_refs = {pipeline_asset.blob_id};
  std::ostringstream payload;
  payload << "{\"pipeline_path\":\"" << pipeline_asset.relative_path.generic_string() << "\"}";
  record_call_locked("ID3D12Device::CreateGraphicsPipelineState", S_OK, {device, pipeline_state}, blob_refs, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_compute_pipeline(
    ID3D12Device *device,
    ID3D12PipelineState *pipeline_state,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
  if (!pipeline_state || !desc) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
  std::vector<apitrace::trace::BlobId> shader_blob_refs;
  const auto pipeline_json = compute_pipeline_asset_json_locked(desc, shader_blob_refs);
  const auto pipeline_asset = register_asset_text_locked(apitrace::trace::AssetKind::Pipeline, "d3d12-compute-pipeline", pipeline_json);
  std::vector<apitrace::trace::BlobId> blob_refs = {pipeline_asset.blob_id};
  std::ostringstream payload;
  payload << "{\"pipeline_path\":\"" << pipeline_asset.relative_path.generic_string() << "\"}";
  record_call_locked("ID3D12Device::CreateComputePipelineState", S_OK, {device, pipeline_state}, blob_refs, payload.str());
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc,
    D3D_ROOT_SIGNATURE_VERSION version,
    ID3DBlob **blob,
    ID3DBlob **error_blob)
{
  auto &downstream = downstream_module();
  if (!downstream.serialize_root_signature) {
    return E_FAIL;
  }
  return downstream.serialize_root_signature(root_signature_desc, version, blob, error_blob);
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc,
    ID3DBlob **blob,
    ID3DBlob **error_blob)
{
  auto &downstream = downstream_module();
  if (!downstream.serialize_versioned_root_signature) {
    return E_FAIL;
  }
  return downstream.serialize_versioned_root_signature(root_signature_desc, blob, error_blob);
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void *data,
    SIZE_T data_size,
    REFIID iid,
    void **deserializer)
{
  auto &downstream = downstream_module();
  if (!downstream.create_root_signature_deserializer) {
    return E_FAIL;
  }
  return downstream.create_root_signature_deserializer(data, data_size, iid, deserializer);
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void *data,
    SIZE_T data_size,
    REFIID iid,
    void **deserializer)
{
  auto &downstream = downstream_module();
  if (!downstream.create_versioned_root_signature_deserializer) {
    return E_FAIL;
  }
  return downstream.create_versioned_root_signature_deserializer(data, data_size, iid, deserializer);
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT feature_count,
    const IID *iids,
    void *configurations,
    UINT *configurations_sizes)
{
  auto &downstream = downstream_module();
  if (!downstream.enable_experimental_features) {
    return E_FAIL;
  }
  return downstream.enable_experimental_features(feature_count, iids, configurations, configurations_sizes);
}

extern "C" void WINAPI apitrace_bootstrap_d3d12()
{
  process_attach();
}

extern "C" void WINAPI apitrace_d3d12_emit_scene_marker(
    const char *scene_name,
    const char *dx_mode,
    const char *phase)
{
  record_scene_marker(scene_name, dx_mode, phase);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
  (void)instance;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    process_attach();
  } else if (reason == DLL_PROCESS_DETACH) {
    process_detach();
  }
  return TRUE;
}
