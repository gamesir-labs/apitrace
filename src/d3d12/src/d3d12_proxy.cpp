#include "apitrace/d3d12_proxy.hpp"

#include "apitrace/capture_runtime.hpp"
#include "apitrace/trace_session.hpp"

#ifndef CINTERFACE
#define CINTERFACE
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

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
void STDMETHODCALLTYPE hook_command_list_set_pipeline_state(
    ID3D12GraphicsCommandList *self,
    ID3D12PipelineState *pipeline_state);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_signature(
    ID3D12GraphicsCommandList *self,
    ID3D12RootSignature *root_signature);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_signature(
    ID3D12GraphicsCommandList *self,
    ID3D12RootSignature *root_signature);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_descriptor_table(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_descriptor_table(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_32bit_constant(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT data,
    UINT dst_offset);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_32bit_constant(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT data,
    UINT dst_offset);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_32bit_constants(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_32bit_constants(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_constant_buffer_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_constant_buffer_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_shader_resource_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_shader_resource_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location);
void STDMETHODCALLTYPE hook_command_list_set_graphics_root_unordered_access_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location);
void STDMETHODCALLTYPE hook_command_list_set_compute_root_unordered_access_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location);
void STDMETHODCALLTYPE hook_command_list_rs_set_viewports(
    ID3D12GraphicsCommandList *self,
    UINT viewport_count,
    const D3D12_VIEWPORT *viewports);
void STDMETHODCALLTYPE hook_command_list_rs_set_scissor_rects(
    ID3D12GraphicsCommandList *self,
    UINT rect_count,
    const D3D12_RECT *rects);
void STDMETHODCALLTYPE hook_command_list_om_set_render_targets(
    ID3D12GraphicsCommandList *self,
    UINT render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    BOOL single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor);
void STDMETHODCALLTYPE hook_command_list_clear_render_target_view(
    ID3D12GraphicsCommandList *self,
    D3D12_CPU_DESCRIPTOR_HANDLE render_target_view,
    const FLOAT color_rgba[4],
    UINT rect_count,
    const D3D12_RECT *rects);
void STDMETHODCALLTYPE hook_command_list_clear_depth_stencil_view(
    ID3D12GraphicsCommandList *self,
    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_view,
    D3D12_CLEAR_FLAGS clear_flags,
    FLOAT depth,
    UINT8 stencil,
    UINT rect_count,
    const D3D12_RECT *rects);
void STDMETHODCALLTYPE hook_command_list_ia_set_primitive_topology(
    ID3D12GraphicsCommandList *self,
    D3D12_PRIMITIVE_TOPOLOGY primitive_topology);
void STDMETHODCALLTYPE hook_command_list_ia_set_vertex_buffers(
    ID3D12GraphicsCommandList *self,
    UINT start_slot,
    UINT view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views);
void STDMETHODCALLTYPE hook_command_list_ia_set_index_buffer(
    ID3D12GraphicsCommandList *self,
    const D3D12_INDEX_BUFFER_VIEW *view);
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
void STDMETHODCALLTYPE hook_command_list_copy_texture_region(
    ID3D12GraphicsCommandList *self,
    const D3D12_TEXTURE_COPY_LOCATION *dst,
    UINT dst_x,
    UINT dst_y,
    UINT dst_z,
    const D3D12_TEXTURE_COPY_LOCATION *src,
    const D3D12_BOX *src_box);
void STDMETHODCALLTYPE hook_command_list_copy_resource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    ID3D12Resource *src);
void STDMETHODCALLTYPE hook_command_list_resolve_subresource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    UINT dst_subresource,
    ID3D12Resource *src,
    UINT src_subresource,
    DXGI_FORMAT format);
HRESULT STDMETHODCALLTYPE hook_resource_map(
    ID3D12Resource *self,
    UINT subresource,
    const D3D12_RANGE *read_range,
    void **data);
void STDMETHODCALLTYPE hook_resource_unmap(
    ID3D12Resource *self,
    UINT subresource,
    const D3D12_RANGE *written_range);
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
UINT64 STDMETHODCALLTYPE hook_fence_get_completed_value(ID3D12Fence *self);

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
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetPipelineState) set_pipeline_state = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRootSignature) set_graphics_root_signature = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRootSignature) set_compute_root_signature = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRootDescriptorTable) set_graphics_root_descriptor_table = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRootDescriptorTable) set_compute_root_descriptor_table = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRoot32BitConstant) set_graphics_root_32bit_constant = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRoot32BitConstant) set_compute_root_32bit_constant = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRoot32BitConstants) set_graphics_root_32bit_constants = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRoot32BitConstants) set_compute_root_32bit_constants = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRootConstantBufferView) set_graphics_root_constant_buffer_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRootConstantBufferView) set_compute_root_constant_buffer_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRootShaderResourceView) set_graphics_root_shader_resource_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRootShaderResourceView) set_compute_root_shader_resource_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetGraphicsRootUnorderedAccessView) set_graphics_root_unordered_access_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetComputeRootUnorderedAccessView) set_compute_root_unordered_access_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().RSSetViewports) rs_set_viewports = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().RSSetScissorRects) rs_set_scissor_rects = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().OMSetRenderTargets) om_set_render_targets = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ClearRenderTargetView) clear_render_target_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ClearDepthStencilView) clear_depth_stencil_view = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().IASetPrimitiveTopology) ia_set_primitive_topology = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().IASetVertexBuffers) ia_set_vertex_buffers = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().IASetIndexBuffer) ia_set_index_buffer = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ResourceBarrier) resource_barrier = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetDescriptorHeaps) set_descriptor_heaps = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DrawInstanced) draw_instanced = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DrawIndexedInstanced) draw_indexed_instanced = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().Dispatch) dispatch = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ExecuteIndirect) execute_indirect = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().CopyTextureRegion) copy_texture_region = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().CopyResource) copy_resource = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ResolveSubresource) resolve_subresource = nullptr;
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
  decltype(std::declval<ID3D12FenceVtbl>().GetCompletedValue) get_completed_value = nullptr;
};

struct ResourceHookState {
  ID3D12ResourceVtbl *vtable = nullptr;
  decltype(std::declval<ID3D12ResourceVtbl>().Map) map = nullptr;
  decltype(std::declval<ID3D12ResourceVtbl>().Unmap) unmap = nullptr;
};

struct MappedResourceState {
  void *data = nullptr;
  UINT subresource = 0;
};

struct CaptureState {
  std::once_flag downstream_once;
  DownstreamModule downstream;
  apitrace::trace::ObjectId next_object_id = 1000;
  apitrace::trace::BlobId next_blob_id = 5000;
  std::uint64_t next_sequence = 1;
  std::uint64_t frame_index = 0;
  std::uint64_t last_present_frame_index = 0;
  bool has_last_present_frame = false;
  bool frame_begin_pending = true;
  std::mutex mutex;
  std::unordered_map<const void *, ObjectInfo> objects;
  std::unordered_map<ID3D12DeviceVtbl *, DeviceHookState> device_hooks;
  std::unordered_map<ID3D12CommandQueueVtbl *, CommandQueueHookState> queue_hooks;
  std::unordered_map<ID3D12CommandAllocatorVtbl *, CommandAllocatorHookState> allocator_hooks;
  std::unordered_map<ID3D12GraphicsCommandListVtbl *, CommandListHookState> command_list_hooks;
  std::unordered_map<ID3D12GraphicsCommandList4Vtbl *, CommandList4HookState> command_list4_hooks;
  std::unordered_map<ID3D12GraphicsCommandList6Vtbl *, CommandList6HookState> command_list6_hooks;
  std::unordered_map<ID3D12FenceVtbl *, FenceHookState> fence_hooks;
  std::unordered_map<ID3D12ResourceVtbl *, ResourceHookState> resource_hooks;
  std::unordered_map<ID3D12Resource *, MappedResourceState> mapped_resources;
  std::unordered_map<ID3D12GraphicsCommandList *, std::vector<ID3D12Resource *>> command_list_resources;
  std::vector<const void *> bridge_command_objects;
};

thread_local unsigned int g_capture_suppression_depth = 0;

CaptureState &capture_state()
{
  static CaptureState state;
  return state;
}

bool capture_recording_suppressed()
{
  return g_capture_suppression_depth != 0;
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
ResourceHookState resource_hook_for(ID3D12Resource *resource);

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

class ScopedResourceOriginalVTable {
public:
  explicit ScopedResourceOriginalVTable(ID3D12Resource *resource)
      : resource_(resource), patched_vtable_(resource ? resource->lpVtbl : nullptr)
  {
    const auto hook = resource_hook_for(resource);
    if (resource_ && hook.vtable) {
      resource_->lpVtbl = hook.vtable;
    }
  }

  ~ScopedResourceOriginalVTable()
  {
    if (resource_ && patched_vtable_) {
      resource_->lpVtbl = patched_vtable_;
    }
  }

private:
  ID3D12Resource *resource_ = nullptr;
  ID3D12ResourceVtbl *patched_vtable_ = nullptr;
};

class ScopedResourceArrayOriginalVTables {
public:
  explicit ScopedResourceArrayOriginalVTables(std::vector<ID3D12Resource *> resources)
  {
    entries_.reserve(resources.size());
    for (auto *resource : resources) {
      if (!resource || !resource->lpVtbl) {
        continue;
      }
      const auto hook = resource_hook_for(resource);
      if (!hook.vtable) {
        continue;
      }
      entries_.push_back({resource, resource->lpVtbl});
      resource->lpVtbl = hook.vtable;
    }
  }

  ~ScopedResourceArrayOriginalVTables()
  {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      it->object->lpVtbl = it->patched_vtable;
    }
  }

private:
  struct Entry {
    ID3D12Resource *object = nullptr;
    ID3D12ResourceVtbl *patched_vtable = nullptr;
  };

  std::vector<Entry> entries_;
};

void remember_command_list_resource_locked(ID3D12GraphicsCommandList *command_list, ID3D12Resource *resource)
{
  if (!command_list || !resource) {
    return;
  }
  auto &resources = capture_state().command_list_resources[command_list];
  if (std::find(resources.begin(), resources.end(), resource) == resources.end()) {
    resources.push_back(resource);
  }
}

void remember_command_list_resources_locked(
    ID3D12GraphicsCommandList *command_list,
    const std::vector<ID3D12Resource *> &resources)
{
  for (auto *resource : resources) {
    remember_command_list_resource_locked(command_list, resource);
  }
}

std::vector<ID3D12Resource *> command_list_resources_snapshot(ID3D12GraphicsCommandList *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().command_list_resources.find(command_list);
  if (it == capture_state().command_list_resources.end()) {
    return {};
  }
  return it->second;
}

std::vector<ID3D12Resource *> command_list_array_resources_snapshot(UINT count, ID3D12CommandList *const *command_lists)
{
  std::vector<ID3D12Resource *> resources;
  if (!command_lists) {
    return resources;
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  for (UINT index = 0; index < count; ++index) {
    auto *command_list = reinterpret_cast<ID3D12GraphicsCommandList *>(command_lists[index]);
    const auto it = capture_state().command_list_resources.find(command_list);
    if (it == capture_state().command_list_resources.end()) {
      continue;
    }
    for (auto *resource : it->second) {
      if (resource && std::find(resources.begin(), resources.end(), resource) == resources.end()) {
        resources.push_back(resource);
      }
    }
  }
  return resources;
}

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

std::string srv_desc_detail_json(const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
    payload << "\"first_element\":" << desc->Buffer.FirstElement
            << ",\"num_elements\":" << desc->Buffer.NumElements
            << ",\"structure_byte_stride\":" << desc->Buffer.StructureByteStride
            << ",\"flags\":" << static_cast<unsigned int>(desc->Buffer.Flags);
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    payload << "\"most_detailed_mip\":" << desc->Texture2D.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture2D.MipLevels
            << ",\"plane_slice\":" << desc->Texture2D.PlaneSlice
            << ",\"resource_min_lod_clamp\":" << desc->Texture2D.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"most_detailed_mip\":" << desc->Texture2DArray.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture2DArray.MipLevels
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice
            << ",\"resource_min_lod_clamp\":" << desc->Texture2DArray.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
    payload << "\"location\":" << desc->RaytracingAccelerationStructure.Location;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string uav_desc_detail_json(const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
    payload << "\"first_element\":" << desc->Buffer.FirstElement
            << ",\"num_elements\":" << desc->Buffer.NumElements
            << ",\"structure_byte_stride\":" << desc->Buffer.StructureByteStride
            << ",\"counter_offset_in_bytes\":" << desc->Buffer.CounterOffsetInBytes
            << ",\"flags\":" << static_cast<unsigned int>(desc->Buffer.Flags);
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    payload << "\"mip_slice\":" << desc->Texture2D.MipSlice
            << ",\"plane_slice\":" << desc->Texture2D.PlaneSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    payload << "\"mip_slice\":" << desc->Texture3D.MipSlice
            << ",\"first_w_slice\":" << desc->Texture3D.FirstWSlice
            << ",\"w_size\":" << desc->Texture3D.WSize;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string rtv_desc_detail_json(const D3D12_RENDER_TARGET_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE2D:
    payload << "\"mip_slice\":" << desc->Texture2D.MipSlice
            << ",\"plane_slice\":" << desc->Texture2D.PlaneSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE3D:
    payload << "\"mip_slice\":" << desc->Texture3D.MipSlice
            << ",\"first_w_slice\":" << desc->Texture3D.FirstWSlice
            << ",\"w_size\":" << desc->Texture3D.WSize;
    break;
  case D3D12_RTV_DIMENSION_BUFFER:
    payload << "\"first_element\":" << desc->Buffer.FirstElement
            << ",\"num_elements\":" << desc->Buffer.NumElements;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
}

std::string dsv_desc_detail_json(const D3D12_DEPTH_STENCIL_VIEW_DESC *desc)
{
  if (!desc) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{";
  switch (desc->ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    payload << "\"mip_slice\":" << desc->Texture2D.MipSlice;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    payload << "\"mip_slice\":" << desc->Texture1D.MipSlice;
    break;
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"mip_slice\":" << desc->Texture1DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize;
    break;
  default:
    break;
  }
  payload << "}";
  return payload.str();
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
    state.objects[alias] = it->second;
  }
}

template <typename VTable, typename Field, typename Replacement>
void patch_vtable_field_cast(VTable *vtable, Field VTable::*field, Replacement replacement)
{
  patch_vtable_field(vtable, field, reinterpret_cast<Field>(replacement));
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

apitrace::trace::ObjectId register_fresh_object_locked(
    const void *object,
    apitrace::trace::ObjectKind kind,
    std::string debug_name,
    apitrace::trace::ObjectId parent_object_id = 0)
{
  if (!object) {
    return 0;
  }
  capture_state().objects.erase(object);
  return register_object_locked(object, kind, std::move(debug_name), parent_object_id);
}

void remember_bridge_command_object_locked(const void *object)
{
  if (!object) {
    return;
  }
  auto &objects = capture_state().bridge_command_objects;
  if (std::find(objects.begin(), objects.end(), object) == objects.end()) {
    objects.push_back(object);
  }
}

bool is_bridge_command_object_locked(const void *object)
{
  const auto &objects = capture_state().bridge_command_objects;
  return std::find(objects.begin(), objects.end(), object) != objects.end();
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
  if (capture_recording_suppressed()) {
    return;
  }
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
  if (capture_recording_suppressed()) {
    return;
  }
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

void record_resource_blob_locked(
    std::string debug_name,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    std::string payload_json)
{
  if (capture_recording_suppressed()) {
    return;
  }
  if (auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12)) {
    apitrace::trace::EventRecord event;
    event.kind = apitrace::trace::EventKind::ResourceBlob;
    event.callsite.sequence = capture_state().next_sequence++;
    event.object_debug_name = std::move(debug_name);
    event.blob_refs = blob_refs;
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

std::string gpu_descriptor_handle_json(D3D12_GPU_DESCRIPTOR_HANDLE descriptor)
{
  return std::to_string(static_cast<std::uint64_t>(descriptor.ptr));
}

std::string descriptor_heap_handle_payload(ID3D12DescriptorHeap *descriptor_heap)
{
  if (!descriptor_heap || !descriptor_heap->lpVtbl) {
    return "\"cpu_start\":0,\"gpu_start\":0";
  }
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
  descriptor_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(descriptor_heap, &cpu_start);
  std::ostringstream payload;
  payload << "\"cpu_start\":" << descriptor_handle_json(cpu_start);
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  descriptor_heap->lpVtbl->GetDesc(descriptor_heap, &desc);
  if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    descriptor_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(descriptor_heap, &gpu_start);
    payload << ",\"gpu_start\":" << gpu_descriptor_handle_json(gpu_start);
  } else {
    payload << ",\"gpu_start\":0";
  }
  return payload.str();
}

UINT descriptor_handle_increment_size(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE heap_type)
{
  if (!device || !device->lpVtbl) {
    return 0;
  }
  return device->lpVtbl->GetDescriptorHandleIncrementSize(device, heap_type);
}

std::string gpu_virtual_address_json(D3D12_GPU_VIRTUAL_ADDRESS address)
{
  return std::to_string(static_cast<std::uint64_t>(address));
}

std::uint64_t resource_gpu_virtual_address(ID3D12Resource *resource)
{
  if (!resource || !resource->lpVtbl) {
    return 0;
  }
  return static_cast<std::uint64_t>(resource->lpVtbl->GetGPUVirtualAddress(resource));
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

std::string clear_value_json(const D3D12_CLEAR_VALUE *clear_value)
{
  if (!clear_value) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{\"format\":" << static_cast<unsigned int>(clear_value->Format)
          << ",\"color\":[" << clear_value->Color[0]
          << "," << clear_value->Color[1]
          << "," << clear_value->Color[2]
          << "," << clear_value->Color[3]
          << "],\"depth\":" << clear_value->DepthStencil.Depth
          << ",\"stencil\":" << static_cast<unsigned int>(clear_value->DepthStencil.Stencil)
          << "}";
  return payload.str();
}

std::string texture_copy_location_json_locked(const D3D12_TEXTURE_COPY_LOCATION *location)
{
  if (!location) {
    return "null";
  }
  std::ostringstream payload;
  payload << "{"
          << "\"resource_object_id\":" << object_id_json(lookup_object_id_locked(location->pResource))
          << ",\"type\":" << static_cast<unsigned int>(location->Type);
  if (location->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    payload << ",\"subresource_index\":" << location->SubresourceIndex;
  } else if (location->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    const auto &footprint = location->PlacedFootprint;
    payload << ",\"placed_footprint\":{"
            << "\"offset\":" << footprint.Offset
            << ",\"format\":" << static_cast<unsigned int>(footprint.Footprint.Format)
            << ",\"width\":" << footprint.Footprint.Width
            << ",\"height\":" << footprint.Footprint.Height
            << ",\"depth\":" << footprint.Footprint.Depth
            << ",\"row_pitch\":" << footprint.Footprint.RowPitch
            << "}";
  }
  payload << "}";
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

std::string render_target_blend_desc_json(const D3D12_RENDER_TARGET_BLEND_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"blend_enable\":" << (desc.BlendEnable ? "true" : "false") << ","
          << "\"logic_op_enable\":" << (desc.LogicOpEnable ? "true" : "false") << ","
          << "\"src_blend\":" << static_cast<unsigned int>(desc.SrcBlend) << ","
          << "\"dest_blend\":" << static_cast<unsigned int>(desc.DestBlend) << ","
          << "\"blend_op\":" << static_cast<unsigned int>(desc.BlendOp) << ","
          << "\"src_blend_alpha\":" << static_cast<unsigned int>(desc.SrcBlendAlpha) << ","
          << "\"dest_blend_alpha\":" << static_cast<unsigned int>(desc.DestBlendAlpha) << ","
          << "\"blend_op_alpha\":" << static_cast<unsigned int>(desc.BlendOpAlpha) << ","
          << "\"logic_op\":" << static_cast<unsigned int>(desc.LogicOp) << ","
          << "\"render_target_write_mask\":" << static_cast<unsigned int>(desc.RenderTargetWriteMask)
          << "}";
  return payload.str();
}

std::string blend_desc_json(const D3D12_BLEND_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"alpha_to_coverage_enable\":" << (desc.AlphaToCoverageEnable ? "true" : "false") << ","
          << "\"independent_blend_enable\":" << (desc.IndependentBlendEnable ? "true" : "false") << ","
          << "\"render_targets\":[";
  for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
    if (index != 0) {
      payload << ",";
    }
    payload << render_target_blend_desc_json(desc.RenderTarget[index]);
  }
  payload << "]}";
  return payload.str();
}

std::string rasterizer_desc_json(const D3D12_RASTERIZER_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"fill_mode\":" << static_cast<unsigned int>(desc.FillMode) << ","
          << "\"cull_mode\":" << static_cast<unsigned int>(desc.CullMode) << ","
          << "\"front_counter_clockwise\":" << (desc.FrontCounterClockwise ? "true" : "false") << ","
          << "\"depth_bias\":" << desc.DepthBias << ","
          << "\"depth_bias_clamp\":" << desc.DepthBiasClamp << ","
          << "\"slope_scaled_depth_bias\":" << desc.SlopeScaledDepthBias << ","
          << "\"depth_clip_enable\":" << (desc.DepthClipEnable ? "true" : "false") << ","
          << "\"multisample_enable\":" << (desc.MultisampleEnable ? "true" : "false") << ","
          << "\"antialiased_line_enable\":" << (desc.AntialiasedLineEnable ? "true" : "false") << ","
          << "\"forced_sample_count\":" << desc.ForcedSampleCount << ","
          << "\"conservative_raster\":" << static_cast<unsigned int>(desc.ConservativeRaster)
          << "}";
  return payload.str();
}

std::string depth_stencil_op_desc_json(const D3D12_DEPTH_STENCILOP_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"stencil_fail_op\":" << static_cast<unsigned int>(desc.StencilFailOp) << ","
          << "\"stencil_depth_fail_op\":" << static_cast<unsigned int>(desc.StencilDepthFailOp) << ","
          << "\"stencil_pass_op\":" << static_cast<unsigned int>(desc.StencilPassOp) << ","
          << "\"stencil_func\":" << static_cast<unsigned int>(desc.StencilFunc)
          << "}";
  return payload.str();
}

std::string depth_stencil_desc_json(const D3D12_DEPTH_STENCIL_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"depth_enable\":" << (desc.DepthEnable ? "true" : "false") << ","
          << "\"depth_write_mask\":" << static_cast<unsigned int>(desc.DepthWriteMask) << ","
          << "\"depth_func\":" << static_cast<unsigned int>(desc.DepthFunc) << ","
          << "\"stencil_enable\":" << (desc.StencilEnable ? "true" : "false") << ","
          << "\"stencil_read_mask\":" << static_cast<unsigned int>(desc.StencilReadMask) << ","
          << "\"stencil_write_mask\":" << static_cast<unsigned int>(desc.StencilWriteMask) << ","
          << "\"front_face\":" << depth_stencil_op_desc_json(desc.FrontFace) << ","
          << "\"back_face\":" << depth_stencil_op_desc_json(desc.BackFace)
          << "}";
  return payload.str();
}

std::string input_layout_json(const D3D12_INPUT_LAYOUT_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"element_count\":" << desc.NumElements << ","
          << "\"elements\":[";
  for (UINT index = 0; index < desc.NumElements; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &element = desc.pInputElementDescs[index];
    payload << "{"
            << "\"semantic_name\":\"" << json_escape(element.SemanticName ? element.SemanticName : "") << "\","
            << "\"semantic_index\":" << element.SemanticIndex << ","
            << "\"format\":" << static_cast<unsigned int>(element.Format) << ","
            << "\"input_slot\":" << element.InputSlot << ","
            << "\"aligned_byte_offset\":" << element.AlignedByteOffset << ","
            << "\"input_slot_class\":" << static_cast<unsigned int>(element.InputSlotClass) << ","
            << "\"instance_data_step_rate\":" << element.InstanceDataStepRate
            << "}";
  }
  payload << "]}";
  return payload.str();
}

std::string stream_output_json(const D3D12_STREAM_OUTPUT_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"declaration_count\":" << desc.NumEntries << ","
          << "\"stride_count\":" << desc.NumStrides << ","
          << "\"rasterized_stream\":" << desc.RasterizedStream
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
          << "\"root_signature_object_id\":" << object_id_json(lookup_object_id_locked(desc->pRootSignature)) << ","
          << "\"node_mask\":" << desc->NodeMask << ","
          << "\"flags\":" << static_cast<unsigned int>(desc->Flags) << ","
          << "\"input_layout\":" << input_layout_json(desc->InputLayout) << ","
          << "\"blend_state\":" << blend_desc_json(desc->BlendState) << ","
          << "\"sample_mask\":" << desc->SampleMask << ","
          << "\"rasterizer_state\":" << rasterizer_desc_json(desc->RasterizerState) << ","
          << "\"depth_stencil_state\":" << depth_stencil_desc_json(desc->DepthStencilState) << ","
          << "\"stream_output\":" << stream_output_json(desc->StreamOutput) << ","
          << "\"primitive_topology_type\":" << static_cast<unsigned int>(desc->PrimitiveTopologyType) << ","
          << "\"num_render_targets\":" << desc->NumRenderTargets << ","
          << "\"rtv_formats\":[";
  for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
    if (index != 0) {
      payload << ",";
    }
    payload << static_cast<unsigned int>(desc->RTVFormats[index]);
  }
  payload << "],"
          << "\"dsv_format\":" << static_cast<unsigned int>(desc->DSVFormat) << ","
          << "\"sample_desc\":{\"count\":" << desc->SampleDesc.Count
          << ",\"quality\":" << desc->SampleDesc.Quality << "},"
          << "\"ib_strip_cut_value\":" << static_cast<unsigned int>(desc->IBStripCutValue) << ","
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
          << "\"root_signature_object_id\":" << object_id_json(lookup_object_id_locked(desc->pRootSignature)) << ","
          << "\"node_mask\":" << desc->NodeMask << ","
          << "\"flags\":" << static_cast<unsigned int>(desc->Flags) << ","
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
void patch_resource(ID3D12Resource *resource);

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

ResourceHookState resource_hook_for(ID3D12Resource *resource)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().resource_hooks.find(resource ? resource->lpVtbl : nullptr);
  return it == capture_state().resource_hooks.end() ? ResourceHookState{} : it->second;
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
  hook.set_pipeline_state = original_vtable->SetPipelineState;
  hook.set_graphics_root_signature = original_vtable->SetGraphicsRootSignature;
  hook.set_compute_root_signature = original_vtable->SetComputeRootSignature;
  hook.set_graphics_root_descriptor_table = original_vtable->SetGraphicsRootDescriptorTable;
  hook.set_compute_root_descriptor_table = original_vtable->SetComputeRootDescriptorTable;
  hook.set_graphics_root_32bit_constant = original_vtable->SetGraphicsRoot32BitConstant;
  hook.set_compute_root_32bit_constant = original_vtable->SetComputeRoot32BitConstant;
  hook.set_graphics_root_32bit_constants = original_vtable->SetGraphicsRoot32BitConstants;
  hook.set_compute_root_32bit_constants = original_vtable->SetComputeRoot32BitConstants;
  hook.set_graphics_root_constant_buffer_view = original_vtable->SetGraphicsRootConstantBufferView;
  hook.set_compute_root_constant_buffer_view = original_vtable->SetComputeRootConstantBufferView;
  hook.set_graphics_root_shader_resource_view = original_vtable->SetGraphicsRootShaderResourceView;
  hook.set_compute_root_shader_resource_view = original_vtable->SetComputeRootShaderResourceView;
  hook.set_graphics_root_unordered_access_view = original_vtable->SetGraphicsRootUnorderedAccessView;
  hook.set_compute_root_unordered_access_view = original_vtable->SetComputeRootUnorderedAccessView;
  hook.rs_set_viewports = original_vtable->RSSetViewports;
  hook.rs_set_scissor_rects = original_vtable->RSSetScissorRects;
  hook.om_set_render_targets = original_vtable->OMSetRenderTargets;
  hook.clear_render_target_view = original_vtable->ClearRenderTargetView;
  hook.clear_depth_stencil_view = original_vtable->ClearDepthStencilView;
  hook.ia_set_primitive_topology = original_vtable->IASetPrimitiveTopology;
  hook.ia_set_vertex_buffers = original_vtable->IASetVertexBuffers;
  hook.ia_set_index_buffer = original_vtable->IASetIndexBuffer;
  hook.resource_barrier = original_vtable->ResourceBarrier;
  hook.set_descriptor_heaps = original_vtable->SetDescriptorHeaps;
  hook.draw_instanced = original_vtable->DrawInstanced;
  hook.draw_indexed_instanced = original_vtable->DrawIndexedInstanced;
  hook.dispatch = original_vtable->Dispatch;
  hook.execute_indirect = original_vtable->ExecuteIndirect;
  hook.copy_texture_region = original_vtable->CopyTextureRegion;
  hook.copy_resource = original_vtable->CopyResource;
  hook.resolve_subresource = original_vtable->ResolveSubresource;
  capture_state().command_list_hooks.emplace(vtable, hook);
  command_list->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::QueryInterface, hook_command_list_query_interface);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Close, hook_command_list_close);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Reset, hook_command_list_reset);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetPipelineState, hook_command_list_set_pipeline_state);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRootSignature, hook_command_list_set_graphics_root_signature);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRootSignature, hook_command_list_set_compute_root_signature);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRootDescriptorTable, hook_command_list_set_graphics_root_descriptor_table);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRootDescriptorTable, hook_command_list_set_compute_root_descriptor_table);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRoot32BitConstant, hook_command_list_set_graphics_root_32bit_constant);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRoot32BitConstant, hook_command_list_set_compute_root_32bit_constant);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRoot32BitConstants, hook_command_list_set_graphics_root_32bit_constants);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRoot32BitConstants, hook_command_list_set_compute_root_32bit_constants);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRootConstantBufferView, hook_command_list_set_graphics_root_constant_buffer_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRootConstantBufferView, hook_command_list_set_compute_root_constant_buffer_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRootShaderResourceView, hook_command_list_set_graphics_root_shader_resource_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRootShaderResourceView, hook_command_list_set_compute_root_shader_resource_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetGraphicsRootUnorderedAccessView, hook_command_list_set_graphics_root_unordered_access_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetComputeRootUnorderedAccessView, hook_command_list_set_compute_root_unordered_access_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::RSSetViewports, hook_command_list_rs_set_viewports);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::RSSetScissorRects, hook_command_list_rs_set_scissor_rects);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::OMSetRenderTargets, hook_command_list_om_set_render_targets);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ClearRenderTargetView, hook_command_list_clear_render_target_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ClearDepthStencilView, hook_command_list_clear_depth_stencil_view);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::IASetPrimitiveTopology, hook_command_list_ia_set_primitive_topology);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::IASetVertexBuffers, hook_command_list_ia_set_vertex_buffers);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::IASetIndexBuffer, hook_command_list_ia_set_index_buffer);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ResourceBarrier, hook_command_list_resource_barrier);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetDescriptorHeaps, hook_command_list_set_descriptor_heaps);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DrawInstanced, hook_command_list_draw_instanced);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DrawIndexedInstanced, hook_command_list_draw_indexed_instanced);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Dispatch, hook_command_list_dispatch);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ExecuteIndirect, hook_command_list_execute_indirect);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::CopyTextureRegion, hook_command_list_copy_texture_region);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::CopyResource, hook_command_list_copy_resource);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ResolveSubresource, hook_command_list_resolve_subresource);
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

template <typename VTable>
CommandListHookState make_command_list_base_hook_state(VTable *original_vtable)
{
  CommandListHookState hook;
  hook.vtable = reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(original_vtable);
  hook.query_interface = reinterpret_cast<decltype(hook.query_interface)>(original_vtable->QueryInterface);
  hook.close = reinterpret_cast<decltype(hook.close)>(original_vtable->Close);
  hook.reset = reinterpret_cast<decltype(hook.reset)>(original_vtable->Reset);
  hook.set_pipeline_state = reinterpret_cast<decltype(hook.set_pipeline_state)>(original_vtable->SetPipelineState);
  hook.set_graphics_root_signature =
      reinterpret_cast<decltype(hook.set_graphics_root_signature)>(original_vtable->SetGraphicsRootSignature);
  hook.set_compute_root_signature =
      reinterpret_cast<decltype(hook.set_compute_root_signature)>(original_vtable->SetComputeRootSignature);
  hook.set_graphics_root_descriptor_table =
      reinterpret_cast<decltype(hook.set_graphics_root_descriptor_table)>(original_vtable->SetGraphicsRootDescriptorTable);
  hook.set_compute_root_descriptor_table =
      reinterpret_cast<decltype(hook.set_compute_root_descriptor_table)>(original_vtable->SetComputeRootDescriptorTable);
  hook.set_graphics_root_32bit_constant =
      reinterpret_cast<decltype(hook.set_graphics_root_32bit_constant)>(original_vtable->SetGraphicsRoot32BitConstant);
  hook.set_compute_root_32bit_constant =
      reinterpret_cast<decltype(hook.set_compute_root_32bit_constant)>(original_vtable->SetComputeRoot32BitConstant);
  hook.set_graphics_root_32bit_constants =
      reinterpret_cast<decltype(hook.set_graphics_root_32bit_constants)>(original_vtable->SetGraphicsRoot32BitConstants);
  hook.set_compute_root_32bit_constants =
      reinterpret_cast<decltype(hook.set_compute_root_32bit_constants)>(original_vtable->SetComputeRoot32BitConstants);
  hook.set_graphics_root_constant_buffer_view =
      reinterpret_cast<decltype(hook.set_graphics_root_constant_buffer_view)>(original_vtable->SetGraphicsRootConstantBufferView);
  hook.set_compute_root_constant_buffer_view =
      reinterpret_cast<decltype(hook.set_compute_root_constant_buffer_view)>(original_vtable->SetComputeRootConstantBufferView);
  hook.set_graphics_root_shader_resource_view =
      reinterpret_cast<decltype(hook.set_graphics_root_shader_resource_view)>(original_vtable->SetGraphicsRootShaderResourceView);
  hook.set_compute_root_shader_resource_view =
      reinterpret_cast<decltype(hook.set_compute_root_shader_resource_view)>(original_vtable->SetComputeRootShaderResourceView);
  hook.set_graphics_root_unordered_access_view =
      reinterpret_cast<decltype(hook.set_graphics_root_unordered_access_view)>(original_vtable->SetGraphicsRootUnorderedAccessView);
  hook.set_compute_root_unordered_access_view =
      reinterpret_cast<decltype(hook.set_compute_root_unordered_access_view)>(original_vtable->SetComputeRootUnorderedAccessView);
  hook.rs_set_viewports = reinterpret_cast<decltype(hook.rs_set_viewports)>(original_vtable->RSSetViewports);
  hook.rs_set_scissor_rects = reinterpret_cast<decltype(hook.rs_set_scissor_rects)>(original_vtable->RSSetScissorRects);
  hook.om_set_render_targets = reinterpret_cast<decltype(hook.om_set_render_targets)>(original_vtable->OMSetRenderTargets);
  hook.clear_render_target_view = reinterpret_cast<decltype(hook.clear_render_target_view)>(original_vtable->ClearRenderTargetView);
  hook.clear_depth_stencil_view = reinterpret_cast<decltype(hook.clear_depth_stencil_view)>(original_vtable->ClearDepthStencilView);
  hook.ia_set_primitive_topology = reinterpret_cast<decltype(hook.ia_set_primitive_topology)>(original_vtable->IASetPrimitiveTopology);
  hook.ia_set_vertex_buffers = reinterpret_cast<decltype(hook.ia_set_vertex_buffers)>(original_vtable->IASetVertexBuffers);
  hook.ia_set_index_buffer = reinterpret_cast<decltype(hook.ia_set_index_buffer)>(original_vtable->IASetIndexBuffer);
  hook.resource_barrier = reinterpret_cast<decltype(hook.resource_barrier)>(original_vtable->ResourceBarrier);
  hook.set_descriptor_heaps = reinterpret_cast<decltype(hook.set_descriptor_heaps)>(original_vtable->SetDescriptorHeaps);
  hook.draw_instanced = reinterpret_cast<decltype(hook.draw_instanced)>(original_vtable->DrawInstanced);
  hook.draw_indexed_instanced = reinterpret_cast<decltype(hook.draw_indexed_instanced)>(original_vtable->DrawIndexedInstanced);
  hook.dispatch = reinterpret_cast<decltype(hook.dispatch)>(original_vtable->Dispatch);
  hook.execute_indirect = reinterpret_cast<decltype(hook.execute_indirect)>(original_vtable->ExecuteIndirect);
  hook.copy_texture_region = reinterpret_cast<decltype(hook.copy_texture_region)>(original_vtable->CopyTextureRegion);
  hook.copy_resource = reinterpret_cast<decltype(hook.copy_resource)>(original_vtable->CopyResource);
  hook.resolve_subresource = reinterpret_cast<decltype(hook.resolve_subresource)>(original_vtable->ResolveSubresource);
  return hook;
}

template <typename VTable>
void patch_command_list_base_methods(VTable *vtable)
{
  patch_vtable_field_cast(vtable, &VTable::QueryInterface, hook_command_list_query_interface);
  patch_vtable_field_cast(vtable, &VTable::Close, hook_command_list_close);
  patch_vtable_field_cast(vtable, &VTable::Reset, hook_command_list_reset);
  patch_vtable_field_cast(vtable, &VTable::SetPipelineState, hook_command_list_set_pipeline_state);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRootSignature, hook_command_list_set_graphics_root_signature);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRootSignature, hook_command_list_set_compute_root_signature);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRootDescriptorTable, hook_command_list_set_graphics_root_descriptor_table);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRootDescriptorTable, hook_command_list_set_compute_root_descriptor_table);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRoot32BitConstant, hook_command_list_set_graphics_root_32bit_constant);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRoot32BitConstant, hook_command_list_set_compute_root_32bit_constant);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRoot32BitConstants, hook_command_list_set_graphics_root_32bit_constants);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRoot32BitConstants, hook_command_list_set_compute_root_32bit_constants);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRootConstantBufferView, hook_command_list_set_graphics_root_constant_buffer_view);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRootConstantBufferView, hook_command_list_set_compute_root_constant_buffer_view);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRootShaderResourceView, hook_command_list_set_graphics_root_shader_resource_view);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRootShaderResourceView, hook_command_list_set_compute_root_shader_resource_view);
  patch_vtable_field_cast(vtable, &VTable::SetGraphicsRootUnorderedAccessView, hook_command_list_set_graphics_root_unordered_access_view);
  patch_vtable_field_cast(vtable, &VTable::SetComputeRootUnorderedAccessView, hook_command_list_set_compute_root_unordered_access_view);
  patch_vtable_field_cast(vtable, &VTable::RSSetViewports, hook_command_list_rs_set_viewports);
  patch_vtable_field_cast(vtable, &VTable::RSSetScissorRects, hook_command_list_rs_set_scissor_rects);
  patch_vtable_field_cast(vtable, &VTable::OMSetRenderTargets, hook_command_list_om_set_render_targets);
  patch_vtable_field_cast(vtable, &VTable::ClearRenderTargetView, hook_command_list_clear_render_target_view);
  patch_vtable_field_cast(vtable, &VTable::ClearDepthStencilView, hook_command_list_clear_depth_stencil_view);
  patch_vtable_field_cast(vtable, &VTable::IASetPrimitiveTopology, hook_command_list_ia_set_primitive_topology);
  patch_vtable_field_cast(vtable, &VTable::IASetVertexBuffers, hook_command_list_ia_set_vertex_buffers);
  patch_vtable_field_cast(vtable, &VTable::IASetIndexBuffer, hook_command_list_ia_set_index_buffer);
  patch_vtable_field_cast(vtable, &VTable::ResourceBarrier, hook_command_list_resource_barrier);
  patch_vtable_field_cast(vtable, &VTable::SetDescriptorHeaps, hook_command_list_set_descriptor_heaps);
  patch_vtable_field_cast(vtable, &VTable::DrawInstanced, hook_command_list_draw_instanced);
  patch_vtable_field_cast(vtable, &VTable::DrawIndexedInstanced, hook_command_list_draw_indexed_instanced);
  patch_vtable_field_cast(vtable, &VTable::Dispatch, hook_command_list_dispatch);
  patch_vtable_field_cast(vtable, &VTable::ExecuteIndirect, hook_command_list_execute_indirect);
  patch_vtable_field_cast(vtable, &VTable::CopyTextureRegion, hook_command_list_copy_texture_region);
  patch_vtable_field_cast(vtable, &VTable::CopyResource, hook_command_list_copy_resource);
  patch_vtable_field_cast(vtable, &VTable::ResolveSubresource, hook_command_list_resolve_subresource);
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
  capture_state().command_list_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(vtable),
      make_command_list_base_hook_state(original_vtable));
  command_list->lpVtbl = vtable;
  patch_command_list_base_methods(vtable);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList4Vtbl::DispatchRays, hook_command_list_dispatch_rays);
  proxy_debug_logf(
      "patch_list4 vtable=%p heaps=%d dispatch_rays=%d",
      static_cast<void *>(vtable),
      reinterpret_cast<void *>(vtable->SetDescriptorHeaps) == reinterpret_cast<void *>(hook_command_list_set_descriptor_heaps),
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
  capture_state().command_list_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(vtable),
      make_command_list_base_hook_state(original_vtable));
  command_list->lpVtbl = vtable;
  patch_command_list_base_methods(vtable);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList6Vtbl::DispatchMesh, hook_command_list_dispatch_mesh);
  proxy_debug_logf(
      "patch_list6 vtable=%p heaps=%d dispatch_mesh=%d",
      static_cast<void *>(vtable),
      reinterpret_cast<void *>(vtable->SetDescriptorHeaps) == reinterpret_cast<void *>(hook_command_list_set_descriptor_heaps),
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
  hook.get_completed_value = original_vtable->GetCompletedValue;
  capture_state().fence_hooks.emplace(vtable, hook);
  fence->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12FenceVtbl::SetEventOnCompletion, hook_fence_set_event_on_completion);
  patch_vtable_field(vtable, &ID3D12FenceVtbl::Signal, hook_fence_signal);
  patch_vtable_field(vtable, &ID3D12FenceVtbl::GetCompletedValue, hook_fence_get_completed_value);
  proxy_debug_logf(
      "patch_fence vtable=%p set_event=%d signal=%d completed=%d",
      static_cast<void *>(vtable),
      vtable->SetEventOnCompletion == hook_fence_set_event_on_completion,
      vtable->Signal == hook_fence_signal,
      vtable->GetCompletedValue == hook_fence_get_completed_value);
}

void patch_resource(ID3D12Resource *resource)
{
  if (!resource || !resource->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = resource->lpVtbl;
  if (capture_state().resource_hooks.find(original_vtable) != capture_state().resource_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_resource: failed to clone vtable");
    return;
  }
  ResourceHookState hook;
  hook.vtable = original_vtable;
  hook.map = original_vtable->Map;
  hook.unmap = original_vtable->Unmap;
  capture_state().resource_hooks.emplace(vtable, hook);
  resource->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12ResourceVtbl::Map, hook_resource_map);
  patch_vtable_field(vtable, &ID3D12ResourceVtbl::Unmap, hook_resource_unmap);
  proxy_debug_logf(
      "patch_resource vtable=%p map=%d unmap=%d",
      static_cast<void *>(vtable),
      vtable->Map == hook_resource_map,
      vtable->Unmap == hook_resource_unmap);
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

}

void ensure_frame_begin_locked()
{
  if (!capture_state().frame_begin_pending) {
    return;
  }
  std::ostringstream payload;
  payload << "{\"label\":\"FrameBegin\",\"frame_index\":" << capture_state().frame_index << "}";
  record_boundary_locked(apitrace::trace::BoundaryKind::Frame, payload.str());
  capture_state().frame_begin_pending = false;
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
      register_fresh_object_locked(*command_queue, apitrace::trace::ObjectKind::CommandQueue, "ID3D12CommandQueue", parent);
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
      register_fresh_object_locked(*command_allocator, apitrace::trace::ObjectKind::CommandAllocator, "ID3D12CommandAllocator", parent);
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
    register_fresh_object_locked(*pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
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
    register_fresh_object_locked(*pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
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
      register_fresh_object_locked(*command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", parent);
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
    register_fresh_object_locked(*descriptor_heap, apitrace::trace::ObjectKind::DescriptorHeap, "ID3D12DescriptorHeap", parent);
    std::ostringstream payload;
    payload << "{"
            << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
            << "\"num_descriptors\":" << (desc ? desc->NumDescriptors : 0) << ","
            << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0) << ","
            << "\"node_mask\":" << (desc ? desc->NodeMask : 0) << ","
            << "\"descriptor_size\":"
            << (desc ? descriptor_handle_increment_size(self, desc->Type) : 0) << ","
            << descriptor_heap_handle_payload(static_cast<ID3D12DescriptorHeap *>(*descriptor_heap))
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
    register_fresh_object_locked(*root_signature, apitrace::trace::ObjectKind::RootSignature, "ID3D12RootSignature", parent);
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
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.create_shader_resource_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"shader_4_component_mapping\":" << (desc ? desc->Shader4ComponentMapping : 0)
          << ",\"view\":" << srv_desc_detail_json(desc) << "}";
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
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    ScopedResourceOriginalVTable original_counter_resource_vtable(counter_resource);
    hook.create_unordered_access_view(self, resource, counter_resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"view\":" << uav_desc_detail_json(desc) << "}";
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
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.create_render_target_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"view\":" << rtv_desc_detail_json(desc) << "}";
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
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.create_depth_stencil_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
          << ",\"view\":" << dsv_desc_detail_json(desc) << "}";
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
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_fresh_object_locked(*resource, apitrace::trace::ObjectKind::Resource, "ID3D12Resource", parent);
      std::ostringstream payload;
      payload << "{\"heap_type\":" << (heap_properties ? static_cast<unsigned int>(heap_properties->Type) : 0)
              << ",\"heap_flags\":" << static_cast<unsigned int>(heap_flags)
              << ",\"initial_state\":" << static_cast<unsigned int>(initial_state)
              << ",\"gpu_virtual_address\":"
              << resource_gpu_virtual_address(static_cast<ID3D12Resource *>(*resource))
              << ",\"resource_desc\":" << resource_desc_json(desc)
              << ",\"optimized_clear_value\":" << clear_value_json(optimized_clear_value)
              << "}";
      record_call_locked("ID3D12Device::CreateCommittedResource", hr, {self, *resource}, {}, payload.str());
    }
    if (heap_properties && heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD) {
      patch_resource(static_cast<ID3D12Resource *>(*resource));
    }
  }
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
      register_fresh_object_locked(*fence, apitrace::trace::ObjectKind::Fence, "ID3D12Fence", parent);
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
    register_fresh_object_locked(*command_signature, apitrace::trace::ObjectKind::CommandSignature, "ID3D12CommandSignature", parent);
    std::ostringstream payload;
    payload << "{\"byte_stride\":" << (desc ? desc->ByteStride : 0)
            << ",\"argument_count\":" << (desc ? desc->NumArgumentDescs : 0)
            << ",\"node_mask\":" << (desc ? desc->NodeMask : 0);
    if (desc && desc->NumArgumentDescs > 0 && desc->pArgumentDescs) {
      payload << ",\"arguments\":[";
      for (UINT index = 0; index < desc->NumArgumentDescs; ++index) {
        if (index != 0) {
          payload << ",";
        }
        const auto &argument = desc->pArgumentDescs[index];
        payload << "{\"type\":" << static_cast<unsigned int>(argument.Type);
        switch (argument.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
          payload << ",\"slot\":" << argument.VertexBuffer.Slot;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
          payload << ",\"root_parameter_index\":" << argument.Constant.RootParameterIndex
                  << ",\"dest_offset_in32bit_values\":" << argument.Constant.DestOffsetIn32BitValues
                  << ",\"num32bit_values_to_set\":" << argument.Constant.Num32BitValuesToSet;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
          payload << ",\"root_parameter_index\":" << argument.ConstantBufferView.RootParameterIndex;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
          payload << ",\"root_parameter_index\":" << argument.ShaderResourceView.RootParameterIndex;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
          payload << ",\"root_parameter_index\":" << argument.UnorderedAccessView.RootParameterIndex;
          break;
        default:
          break;
        }
        payload << "}";
      }
      payload << "]";
    }
    payload << "}";
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
    const auto referenced_resources = command_list_array_resources_snapshot(command_list_count, command_lists);
    ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
    ScopedCommandListArrayOriginalVTables original_command_list_vtables(command_list_count, command_lists);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
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
  const auto referenced_resources = command_list_resources_snapshot(self);
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
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
  if (SUCCEEDED(hr)) {
    capture_state().command_list_resources[self].clear();
  }
  record_call_locked("ID3D12GraphicsCommandList::Reset", hr, {self, allocator, initial_state}, {}, "{}");
  return hr;
}

void STDMETHODCALLTYPE hook_command_list_set_pipeline_state(
    ID3D12GraphicsCommandList *self,
    ID3D12PipelineState *pipeline_state)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_pipeline_state) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_pipeline_state(self, pipeline_state);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::SetPipelineState", S_OK, {self, pipeline_state}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_signature(
    ID3D12GraphicsCommandList *self,
    ID3D12RootSignature *root_signature)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_signature) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_signature(self, root_signature);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootSignature", S_OK, {self, root_signature}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_signature(
    ID3D12GraphicsCommandList *self,
    ID3D12RootSignature *root_signature)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_signature) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_signature(self, root_signature);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::SetComputeRootSignature", S_OK, {self, root_signature}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_descriptor_table(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_descriptor_table) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_descriptor_table(self, root_parameter_index, base_descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"base_descriptor\":" << gpu_descriptor_handle_json(base_descriptor) << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_descriptor_table(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_descriptor_table) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_descriptor_table(self, root_parameter_index, base_descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"base_descriptor\":" << gpu_descriptor_handle_json(base_descriptor) << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetComputeRootDescriptorTable", S_OK, {self}, {}, payload.str());
}

void record_root_32bit_constants_call_locked(
    const char *function_name,
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset)
{
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"constant_count\":" << constant_count
          << ",\"dst_offset\":" << dst_offset
          << ",\"values\":[";
  const auto *values = static_cast<const UINT *>(data);
  for (UINT index = 0; index < constant_count; ++index) {
    if (index != 0) {
      payload << ",";
    }
    payload << (values ? values[index] : 0);
  }
  payload << "]}";
  record_call_locked(function_name, S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_32bit_constant(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_32bit_constant) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_32bit_constant(self, root_parameter_index, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant",
      self,
      root_parameter_index,
      1,
      &data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_32bit_constant(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_32bit_constant) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_32bit_constant(self, root_parameter_index, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant",
      self,
      root_parameter_index,
      1,
      &data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_32bit_constants(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_32bit_constants) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_32bit_constants(self, root_parameter_index, constant_count, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants",
      self,
      root_parameter_index,
      constant_count,
      data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_32bit_constants(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_32bit_constants) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_32bit_constants(self, root_parameter_index, constant_count, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants",
      self,
      root_parameter_index,
      constant_count,
      data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_constant_buffer_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_constant_buffer_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_constant_buffer_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"buffer_location\":" << gpu_virtual_address_json(buffer_location) << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView", S_OK, {self}, {}, payload.str());
}

void record_root_descriptor_call_locked(
    const char *function_name,
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"buffer_location\":" << gpu_virtual_address_json(buffer_location) << "}";
  record_call_locked(function_name, S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_constant_buffer_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_constant_buffer_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_constant_buffer_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetComputeRootConstantBufferView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_shader_resource_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_shader_resource_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_shader_resource_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_shader_resource_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_shader_resource_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_shader_resource_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetComputeRootShaderResourceView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_unordered_access_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_unordered_access_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_unordered_access_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_unordered_access_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_unordered_access_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_unordered_access_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_rs_set_viewports(
    ID3D12GraphicsCommandList *self,
    UINT viewport_count,
    const D3D12_VIEWPORT *viewports)
{
  const auto hook = command_list_hook_for(self);
  if (hook.rs_set_viewports) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.rs_set_viewports(self, viewport_count, viewports);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"viewport_count\":" << viewport_count;
  if (viewport_count > 0 && viewports) {
    payload << ",\"first\":{\"x\":" << viewports[0].TopLeftX
            << ",\"y\":" << viewports[0].TopLeftY
            << ",\"width\":" << viewports[0].Width
            << ",\"height\":" << viewports[0].Height
            << ",\"min_depth\":" << viewports[0].MinDepth
            << ",\"max_depth\":" << viewports[0].MaxDepth << "}";
    payload << ",\"viewports\":[";
    for (UINT index = 0; index < viewport_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"x\":" << viewports[index].TopLeftX
              << ",\"y\":" << viewports[index].TopLeftY
              << ",\"width\":" << viewports[index].Width
              << ",\"height\":" << viewports[index].Height
              << ",\"min_depth\":" << viewports[index].MinDepth
              << ",\"max_depth\":" << viewports[index].MaxDepth << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::RSSetViewports", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_rs_set_scissor_rects(
    ID3D12GraphicsCommandList *self,
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.rs_set_scissor_rects) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.rs_set_scissor_rects(self, rect_count, rects);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"rect_count\":" << rect_count;
  if (rect_count > 0 && rects) {
    payload << ",\"first\":{\"left\":" << rects[0].left
            << ",\"top\":" << rects[0].top
            << ",\"right\":" << rects[0].right
            << ",\"bottom\":" << rects[0].bottom << "}";
    payload << ",\"rects\":[";
    for (UINT index = 0; index < rect_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"left\":" << rects[index].left
              << ",\"top\":" << rects[index].top
              << ",\"right\":" << rects[index].right
              << ",\"bottom\":" << rects[index].bottom << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::RSSetScissorRects", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_om_set_render_targets(
    ID3D12GraphicsCommandList *self,
    UINT render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    BOOL single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor)
{
  const auto hook = command_list_hook_for(self);
  if (hook.om_set_render_targets) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.om_set_render_targets(self, render_target_count, render_target_descriptors, single_handle_to_descriptor_range, depth_stencil_descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"render_target_count\":" << render_target_count
          << ",\"single_handle_to_descriptor_range\":" << (single_handle_to_descriptor_range ? "true" : "false");
  if (render_target_count > 0 && render_target_descriptors) {
    payload << ",\"first_rtv\":" << descriptor_handle_json(render_target_descriptors[0]);
    payload << ",\"render_targets\":[";
    for (UINT index = 0; index < render_target_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << descriptor_handle_json(render_target_descriptors[index]);
    }
    payload << "]";
  }
  if (depth_stencil_descriptor) {
    payload << ",\"dsv\":" << descriptor_handle_json(*depth_stencil_descriptor);
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::OMSetRenderTargets", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_clear_render_target_view(
    ID3D12GraphicsCommandList *self,
    D3D12_CPU_DESCRIPTOR_HANDLE render_target_view,
    const FLOAT color_rgba[4],
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_render_target_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.clear_render_target_view(self, render_target_view, color_rgba, rect_count, rects);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(render_target_view)
          << ",\"rect_count\":" << rect_count;
  if (color_rgba) {
    payload << ",\"color\":[" << color_rgba[0] << "," << color_rgba[1] << "," << color_rgba[2] << "," << color_rgba[3] << "]";
  }
  if (rect_count > 0 && rects) {
    payload << ",\"first_rect\":{\"left\":" << rects[0].left
            << ",\"top\":" << rects[0].top
            << ",\"right\":" << rects[0].right
            << ",\"bottom\":" << rects[0].bottom << "}";
    payload << ",\"rects\":[";
    for (UINT index = 0; index < rect_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"left\":" << rects[index].left
              << ",\"top\":" << rects[index].top
              << ",\"right\":" << rects[index].right
              << ",\"bottom\":" << rects[index].bottom << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::ClearRenderTargetView", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_clear_depth_stencil_view(
    ID3D12GraphicsCommandList *self,
    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_view,
    D3D12_CLEAR_FLAGS clear_flags,
    FLOAT depth,
    UINT8 stencil,
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_depth_stencil_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.clear_depth_stencil_view(self, depth_stencil_view, clear_flags, depth, stencil, rect_count, rects);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(depth_stencil_view)
          << ",\"clear_flags\":" << static_cast<unsigned int>(clear_flags)
          << ",\"depth\":" << depth
          << ",\"stencil\":" << static_cast<unsigned int>(stencil)
          << ",\"rect_count\":" << rect_count;
  if (rect_count > 0 && rects) {
    payload << ",\"first_rect\":{\"left\":" << rects[0].left
            << ",\"top\":" << rects[0].top
            << ",\"right\":" << rects[0].right
            << ",\"bottom\":" << rects[0].bottom << "}";
    payload << ",\"rects\":[";
    for (UINT index = 0; index < rect_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"left\":" << rects[index].left
              << ",\"top\":" << rects[index].top
              << ",\"right\":" << rects[index].right
              << ",\"bottom\":" << rects[index].bottom << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::ClearDepthStencilView", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_ia_set_primitive_topology(
    ID3D12GraphicsCommandList *self,
    D3D12_PRIMITIVE_TOPOLOGY primitive_topology)
{
  const auto hook = command_list_hook_for(self);
  if (hook.ia_set_primitive_topology) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.ia_set_primitive_topology(self, primitive_topology);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"primitive_topology\":" << static_cast<unsigned int>(primitive_topology) << "}";
  record_call_locked("ID3D12GraphicsCommandList::IASetPrimitiveTopology", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_ia_set_vertex_buffers(
    ID3D12GraphicsCommandList *self,
    UINT start_slot,
    UINT view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views)
{
  const auto hook = command_list_hook_for(self);
  if (hook.ia_set_vertex_buffers) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.ia_set_vertex_buffers(self, start_slot, view_count, views);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot << ",\"view_count\":" << view_count;
  if (view_count > 0 && views) {
    payload << ",\"first\":{\"buffer_location\":" << gpu_virtual_address_json(views[0].BufferLocation)
            << ",\"size_in_bytes\":" << views[0].SizeInBytes
            << ",\"stride_in_bytes\":" << views[0].StrideInBytes << "}";
    payload << ",\"views\":[";
    for (UINT index = 0; index < view_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"buffer_location\":" << gpu_virtual_address_json(views[index].BufferLocation)
              << ",\"size_in_bytes\":" << views[index].SizeInBytes
              << ",\"stride_in_bytes\":" << views[index].StrideInBytes << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::IASetVertexBuffers", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_ia_set_index_buffer(
    ID3D12GraphicsCommandList *self,
    const D3D12_INDEX_BUFFER_VIEW *view)
{
  const auto hook = command_list_hook_for(self);
  if (hook.ia_set_index_buffer) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.ia_set_index_buffer(self, view);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{";
  if (view) {
    payload << "\"buffer_location\":" << gpu_virtual_address_json(view->BufferLocation)
            << ",\"size_in_bytes\":" << view->SizeInBytes
            << ",\"format\":" << static_cast<unsigned int>(view->Format);
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::IASetIndexBuffer", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_resource_barrier(
    ID3D12GraphicsCommandList *self,
    UINT barrier_count,
    const D3D12_RESOURCE_BARRIER *barriers)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> barrier_resources;
  barrier_resources.reserve(barrier_count);
  for (UINT index = 0; barriers && index < barrier_count; ++index) {
    if (barriers[index].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      barrier_resources.push_back(barriers[index].Transition.pResource);
    } else if (barriers[index].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
      barrier_resources.push_back(barriers[index].Aliasing.pResourceBefore);
      barrier_resources.push_back(barriers[index].Aliasing.pResourceAfter);
    } else if (barriers[index].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      barrier_resources.push_back(barriers[index].UAV.pResource);
    }
  }
  if (hook.resource_barrier) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(barrier_resources);
    hook.resource_barrier(self, barrier_count, barriers);
  }
  proxy_debug_logf("hook_command_list_resource_barrier self=%p count=%u", self, barrier_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, barrier_resources);
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
  std::vector<ID3D12Resource *> referenced_resources = {arg_buffer, count_buffer};
  if (hook.execute_indirect) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.execute_indirect(self, command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset);
  }
  proxy_debug_logf("hook_command_list_execute_indirect self=%p max=%u", self, max_command_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"max_command_count\":" << max_command_count
          << ",\"arg_buffer_offset\":" << arg_buffer_offset
          << ",\"count_buffer_offset\":" << count_buffer_offset << "}";
  record_call_locked("ID3D12GraphicsCommandList::ExecuteIndirect", S_OK, {self, command_signature, arg_buffer, count_buffer}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_copy_texture_region(
    ID3D12GraphicsCommandList *self,
    const D3D12_TEXTURE_COPY_LOCATION *dst,
    UINT dst_x,
    UINT dst_y,
    UINT dst_z,
    const D3D12_TEXTURE_COPY_LOCATION *src,
    const D3D12_BOX *src_box)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {
      dst ? dst->pResource : nullptr,
      src ? src->pResource : nullptr,
  };
  if (hook.copy_texture_region) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.copy_texture_region(self, dst, dst_x, dst_y, dst_z, src, src_box);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_x\":" << dst_x
          << ",\"dst_y\":" << dst_y
          << ",\"dst_z\":" << dst_z
          << ",\"dst_type\":" << (dst ? static_cast<unsigned int>(dst->Type) : 0)
          << ",\"src_type\":" << (src ? static_cast<unsigned int>(src->Type) : 0)
          << ",\"dst\":" << texture_copy_location_json_locked(dst)
          << ",\"src\":" << texture_copy_location_json_locked(src)
          << ",\"has_src_box\":" << (src_box ? "true" : "false");
  if (src_box) {
    payload << ",\"src_box\":{\"left\":" << src_box->left
            << ",\"top\":" << src_box->top
            << ",\"front\":" << src_box->front
            << ",\"right\":" << src_box->right
            << ",\"bottom\":" << src_box->bottom
            << ",\"back\":" << src_box->back << "}";
  }
  payload << "}";
  record_call_locked(
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      S_OK,
      {self, dst ? dst->pResource : nullptr, src ? src->pResource : nullptr},
      {},
      payload.str());
}

void STDMETHODCALLTYPE hook_command_list_copy_resource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    ID3D12Resource *src)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {dst, src};
  if (hook.copy_resource) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.copy_resource(self, dst, src);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_resource_object_id\":" << object_id_json(lookup_object_id_locked(dst))
          << ",\"src_resource_object_id\":" << object_id_json(lookup_object_id_locked(src)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::CopyResource", S_OK, {self, dst, src}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_resolve_subresource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    UINT dst_subresource,
    ID3D12Resource *src,
    UINT src_subresource,
    DXGI_FORMAT format)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {dst, src};
  if (hook.resolve_subresource) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.resolve_subresource(self, dst, dst_subresource, src, src_subresource, format);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_subresource\":" << dst_subresource
          << ",\"src_subresource\":" << src_subresource
          << ",\"format\":" << static_cast<unsigned int>(format)
          << ",\"dst_resource_object_id\":" << object_id_json(lookup_object_id_locked(dst))
          << ",\"src_resource_object_id\":" << object_id_json(lookup_object_id_locked(src)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::ResolveSubresource", S_OK, {self, dst, src}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_resource_map(
    ID3D12Resource *self,
    UINT subresource,
    const D3D12_RANGE *read_range,
    void **data)
{
  const auto hook = resource_hook_for(self);
  if (!hook.map) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Resource, ID3D12ResourceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.map(self, subresource, read_range, data);

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (SUCCEEDED(hr) && data && *data) {
    capture_state().mapped_resources[self] = MappedResourceState{*data, subresource};
  }
  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource;
  if (read_range) {
    payload << ",\"read_begin\":" << static_cast<std::uint64_t>(read_range->Begin)
            << ",\"read_end\":" << static_cast<std::uint64_t>(read_range->End);
  } else {
    payload << ",\"read_range\":null";
  }
  payload << ",\"mapped\":" << ((SUCCEEDED(hr) && data && *data) ? "true" : "false") << "}";
  record_call_locked("ID3D12Resource::Map", hr, {self}, {}, payload.str());
  return hr;
}

void STDMETHODCALLTYPE hook_resource_unmap(
    ID3D12Resource *self,
    UINT subresource,
    const D3D12_RANGE *written_range)
{
  const auto hook = resource_hook_for(self);

  void *mapped_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto it = capture_state().mapped_resources.find(self);
    if (it != capture_state().mapped_resources.end() && it->second.subresource == subresource) {
      mapped_data = it->second.data;
    }
  }

  std::vector<apitrace::trace::BlobId> blob_refs;
  std::string buffer_path;
  const bool has_written_range = written_range && written_range->End > written_range->Begin;
  if (mapped_data && has_written_range) {
    const auto begin = static_cast<std::size_t>(written_range->Begin);
    const auto size = static_cast<std::size_t>(written_range->End - written_range->Begin);
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto *bytes = static_cast<const std::uint8_t *>(mapped_data) + begin;
    const auto asset = register_asset_bytes_locked(apitrace::trace::AssetKind::Buffer, "d3d12-mapped-resource", bytes, size);
    blob_refs.push_back(asset.blob_id);
    buffer_path = asset.relative_path.generic_string();
  }

  if (hook.unmap) {
    ScopedOriginalVTable<ID3D12Resource, ID3D12ResourceVtbl> original_vtable(self, hook.vtable);
    hook.unmap(self, subresource, written_range);
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  capture_state().mapped_resources.erase(self);
  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource;
  if (written_range) {
    payload << ",\"written_begin\":" << static_cast<std::uint64_t>(written_range->Begin)
            << ",\"written_end\":" << static_cast<std::uint64_t>(written_range->End);
  } else {
    payload << ",\"written_range\":null";
  }
  if (!buffer_path.empty()) {
    payload << ",\"buffer_path\":\"" << buffer_path << "\"";
  }
  payload << "}";
  record_call_locked("ID3D12Resource::Unmap", S_OK, {self}, blob_refs, payload.str());
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
          << ",\"depth\":" << (desc ? desc->Depth : 0);
  if (desc) {
    payload << ",\"ray_generation_shader_record\":{"
            << "\"start_address\":" << desc->RayGenerationShaderRecord.StartAddress
            << ",\"size_in_bytes\":" << desc->RayGenerationShaderRecord.SizeInBytes
            << "},\"miss_shader_table\":{"
            << "\"start_address\":" << desc->MissShaderTable.StartAddress
            << ",\"size_in_bytes\":" << desc->MissShaderTable.SizeInBytes
            << ",\"stride_in_bytes\":" << desc->MissShaderTable.StrideInBytes
            << "},\"hit_group_table\":{"
            << "\"start_address\":" << desc->HitGroupTable.StartAddress
            << ",\"size_in_bytes\":" << desc->HitGroupTable.SizeInBytes
            << ",\"stride_in_bytes\":" << desc->HitGroupTable.StrideInBytes
            << "},\"callable_shader_table\":{"
            << "\"start_address\":" << desc->CallableShaderTable.StartAddress
            << ",\"size_in_bytes\":" << desc->CallableShaderTable.SizeInBytes
            << ",\"stride_in_bytes\":" << desc->CallableShaderTable.StrideInBytes
            << "}";
  }
  payload << "}";
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

UINT64 STDMETHODCALLTYPE hook_fence_get_completed_value(ID3D12Fence *self)
{
  const auto hook = fence_hook_for(self);
  if (!hook.get_completed_value) {
    return 0;
  }
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_vtable(self, hook.vtable);
  const UINT64 completed_value = hook.get_completed_value(self);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"completed_value\":" << completed_value << "}";
  record_call_locked("ID3D12Fence::GetCompletedValue", S_OK, {self}, {}, payload.str());
  return completed_value;
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
    const bool queue_known = lookup_object_id_locked(queue) != 0;
    if (!queue_known) {
      register_object_locked(queue, apitrace::trace::ObjectKind::CommandQueue, "ID3D12CommandQueue", device_id);
      remember_bridge_command_object_locked(queue);
      record_call_locked(
          "ID3D12Device::CreateCommandQueue",
          S_OK,
          {device, queue},
          {},
          "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}");
    }
  }
  if (allocator) {
    const bool allocator_known = lookup_object_id_locked(allocator) != 0;
    if (!allocator_known) {
      register_object_locked(allocator, apitrace::trace::ObjectKind::CommandAllocator, "ID3D12CommandAllocator", device_id);
      record_call_locked("ID3D12Device::CreateCommandAllocator", S_OK, {device, allocator}, {}, "{\"type\":0}");
    }
  }
  if (command_list && allocator) {
    const bool command_list_known = lookup_object_id_locked(command_list) != 0;
    if (!command_list_known) {
      register_object_locked(command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", device_id);
      remember_bridge_command_object_locked(command_list);
      record_call_locked("ID3D12Device::CreateCommandList", S_OK, {device, allocator, nullptr, command_list}, {}, "{\"node_mask\":0,\"type\":0}");
      record_call_locked("ID3D12GraphicsCommandList::Close", S_OK, {command_list}, {}, "{}");
    }
  }
  if (fence) {
    const bool fence_known = lookup_object_id_locked(fence) != 0;
    if (!fence_known) {
      register_object_locked(fence, apitrace::trace::ObjectKind::Fence, "ID3D12Fence", device_id);
      record_call_locked("ID3D12Device::CreateFence", S_OK, {device, fence}, {}, "{\"initial_value\":0,\"flags\":0}");
    }
  }
}

extern "C" void WINAPI apitrace_d3d12_record_execute_command_lists(
    ID3D12CommandQueue *queue,
    ID3D12GraphicsCommandList *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (!is_bridge_command_object_locked(queue) && !is_bridge_command_object_locked(command_list)) {
    return;
  }
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
  if (lookup_object_id_locked(descriptor_heap) != 0) {
    return;
  }
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(descriptor_heap, apitrace::trace::ObjectKind::DescriptorHeap, "ID3D12DescriptorHeap", parent);
  std::ostringstream payload;
  payload << "{"
          << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
          << "\"num_descriptors\":" << (desc ? desc->NumDescriptors : 0) << ","
          << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0) << ","
          << "\"node_mask\":" << (desc ? desc->NodeMask : 0) << ","
          << "\"descriptor_size\":"
          << (desc ? descriptor_handle_increment_size(device, desc->Type) : 0) << ","
          << descriptor_heap_handle_payload(descriptor_heap)
          << "}";
  record_call_locked("ID3D12Device::CreateDescriptorHeap", S_OK, {device, descriptor_heap}, {}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_swapchain_back_buffer(
    ID3D12Device *device,
    IDXGISwapChain *swap_chain,
    ID3D12Resource *back_buffer,
    UINT buffer_index,
    const D3D12_RESOURCE_DESC *desc)
{
  if (!back_buffer) {
    return;
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto device_id = register_object_locked(device, apitrace::trace::ObjectKind::Device, "ID3D12Device");
  const auto swap_chain_id = register_object_locked(swap_chain, apitrace::trace::ObjectKind::SwapChain, "IDXGISwapChain", device_id);
  register_fresh_object_locked(back_buffer, apitrace::trace::ObjectKind::Resource, "IDXGISwapChainBackBuffer", swap_chain_id);
  std::ostringstream payload;
  payload << "{\"heap_type\":" << static_cast<unsigned int>(D3D12_HEAP_TYPE_DEFAULT)
          << ",\"heap_flags\":" << static_cast<unsigned int>(D3D12_HEAP_FLAG_NONE)
          << ",\"initial_state\":" << static_cast<unsigned int>(D3D12_RESOURCE_STATE_PRESENT)
          << ",\"gpu_virtual_address\":0"
          << ",\"swapchain_back_buffer\":true"
          << ",\"buffer_index\":" << buffer_index
          << ",\"resource_desc\":" << resource_desc_json(desc)
          << ",\"optimized_clear_value\":null"
          << "}";
  record_call_locked("ID3D12Device::CreateCommittedResource", S_OK, {device, back_buffer}, {}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_resource_barrier(
    ID3D12GraphicsCommandList *command_list,
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    UINT subresource)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (!is_bridge_command_object_locked(command_list)) {
    return;
  }
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

extern "C" void WINAPI apitrace_d3d12_record_present_frame(
    UINT width,
    UINT height,
    UINT row_pitch,
    UINT sync_interval,
    UINT flags,
    const void *rgba_data,
    SIZE_T rgba_size)
{
  if (!rgba_data || width == 0 || height == 0 || row_pitch == 0 || rgba_size == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  ensure_frame_begin_locked();
  const auto frame_index = capture_state().frame_index;
  const auto asset = register_asset_bytes_locked(
      apitrace::trace::AssetKind::Texture,
      "d3d12-present-frame",
      rgba_data,
      static_cast<std::size_t>(rgba_size));

  std::ostringstream payload;
  payload << "{"
          << "\"frame_index\":" << frame_index << ","
          << "\"width\":" << width << ","
          << "\"height\":" << height << ","
          << "\"row_pitch\":" << row_pitch << ","
          << "\"sync_interval\":" << sync_interval << ","
          << "\"flags\":" << flags << ","
          << "\"format\":\"rgba8\","
          << "\"frame_path\":\"" << asset.relative_path.generic_string() << "\""
          << "}";
  record_resource_blob_locked("D3D12PresentFrame", {asset.blob_id}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_begin_capture_suppression()
{
  ++g_capture_suppression_depth;
}

extern "C" void WINAPI apitrace_d3d12_end_capture_suppression()
{
  if (g_capture_suppression_depth != 0) {
    --g_capture_suppression_depth;
  }
}

extern "C" void WINAPI apitrace_d3d12_record_present_semantics(
    UINT sync_interval,
    UINT flags,
    HRESULT result)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  ensure_frame_begin_locked();
  const auto frame_index = capture_state().frame_index++;
  capture_state().last_present_frame_index = frame_index;
  capture_state().has_last_present_frame = true;
  std::ostringstream call_payload;
  call_payload << "{\"sync_interval\":" << sync_interval
               << ",\"flags\":" << flags
               << ",\"frame_index\":" << frame_index << "}";
  record_call_locked("IDXGISwapChain::Present", result, {}, {}, call_payload.str());

  std::ostringstream boundary_payload;
  boundary_payload << "{\"label\":\"Present\",\"frame_index\":" << frame_index
                   << ",\"sync_interval\":" << sync_interval
                   << ",\"flags\":" << flags << "}";
  record_boundary_locked(apitrace::trace::BoundaryKind::Present, boundary_payload.str());

  std::ostringstream frame_end_payload;
  frame_end_payload << "{\"label\":\"FrameEnd\",\"frame_index\":" << frame_index << "}";
  record_boundary_locked(apitrace::trace::BoundaryKind::Frame, frame_end_payload.str());
  capture_state().frame_begin_pending = true;
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
  if (lookup_object_id_locked(pipeline_state) != 0) {
    return;
  }
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
  if (lookup_object_id_locked(pipeline_state) != 0) {
    return;
  }
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
