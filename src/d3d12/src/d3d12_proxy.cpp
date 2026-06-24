#include "apitrace/d3d12_proxy.hpp"

#include "apitrace/asset_index.hpp"
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
#include <limits>
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

constexpr std::uint64_t kRootConstantBufferSnapshotBytes =
    std::uint64_t{D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT} * 4u * sizeof(std::uint32_t);
constexpr std::uint64_t kMappedUseSnapshotChunkBytes = 4u * 1024u;
constexpr GUID kIidIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidD3D12Heap = {0x6b3b2502, 0x6e51, 0x45b3, {0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3}};
constexpr GUID kIidD3D12Device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
constexpr GUID kIidD3D12Device1 = {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}};
constexpr GUID kIidD3D12Device2 = {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};
constexpr GUID kIidD3D12Device3 = {0x81dadc15, 0x2bad, 0x4392, {0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98}};
constexpr GUID kIidD3D12Device4 = {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}};
constexpr GUID kIidD3D12Device5 = {0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}};
constexpr GUID kIidD3D12Device6 = {0xc70b221b, 0x40e4, 0x4a17, {0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc}};
constexpr GUID kIidD3D12Device7 = {0x5c014b53, 0x68a1, 0x4b9b, {0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b}};
constexpr GUID kIidD3D12Device8 = {0x9218e6bb, 0xf944, 0x4f7e, {0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3}};
constexpr GUID kIidD3D12Device9 = {0x4c80e962, 0xf032, 0x4f60, {0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c}};
constexpr GUID kIidD3D12RootSignatureDeserializer = {0x34ab647b, 0x3cc8, 0x46ac, {0x84, 0x1b, 0xc0, 0x96, 0x56, 0x45, 0xc0, 0x46}};
constexpr GUID kIidD3D12VersionedRootSignatureDeserializer = {0x7f91ce67, 0x090c, 0x4bb7, {0xb7, 0x8e, 0xed, 0x8f, 0xf2, 0xe3, 0x1d, 0xa0}};
constexpr GUID kIidD3D12Fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
constexpr GUID kIidD3D12GraphicsCommandList = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
constexpr GUID kIidD3D12GraphicsCommandList1 = {0x553103fb, 0x1fe7, 0x4557, {0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7}};
constexpr GUID kIidD3D12GraphicsCommandList2 = {0x38c3e585, 0xff17, 0x412c, {0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28}};
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

enum class PipelineStateSubobjectType : std::uint32_t {
  RootSignature = 0x0,
  VS = 0x1,
  PS = 0x2,
  DS = 0x3,
  HS = 0x4,
  GS = 0x5,
  CS = 0x6,
  StreamOutput = 0x7,
  Blend = 0x8,
  SampleMask = 0x9,
  Rasterizer = 0xa,
  DepthStencil = 0xb,
  InputLayout = 0xc,
  IbStripCutValue = 0xd,
  PrimitiveTopology = 0xe,
  RenderTargetFormats = 0xf,
  DepthStencilFormat = 0x10,
  SampleDesc = 0x11,
  NodeMask = 0x12,
  CachedPso = 0x13,
  Flags = 0x14,
  DepthStencil1 = 0x15,
  ViewInstancing = 0x16,
  AS = 0x18,
  MS = 0x19,
};

struct DepthStencilDesc1 {
  WINBOOL DepthEnable;
  D3D12_DEPTH_WRITE_MASK DepthWriteMask;
  D3D12_COMPARISON_FUNC DepthFunc;
  WINBOOL StencilEnable;
  UINT8 StencilReadMask;
  UINT8 StencilWriteMask;
  D3D12_DEPTH_STENCILOP_DESC FrontFace;
  D3D12_DEPTH_STENCILOP_DESC BackFace;
  WINBOOL DepthBoundsTestEnable;
};

struct RtFormatArray {
  DXGI_FORMAT RTFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
  UINT NumRenderTargets;
};

struct ViewInstancingDesc {
  UINT ViewInstanceCount;
  const void *pViewInstanceLocations;
  UINT Flags;
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
HRESULT STDMETHODCALLTYPE hook_device_create_pipeline_state(
    ID3D12Device2 *self,
    const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
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
HRESULT STDMETHODCALLTYPE hook_device_create_command_list1(
    ID3D12Device4 *self,
    UINT node_mask,
    D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid,
    void **command_list);
HRESULT STDMETHODCALLTYPE hook_device_create_heap1(
    ID3D12Device4 *self,
    const D3D12_HEAP_DESC *desc,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **heap);
HRESULT STDMETHODCALLTYPE hook_device_open_existing_heap_from_address(
    ID3D12Device3 *self,
    const void *address,
    REFIID riid,
    void **heap);
HRESULT STDMETHODCALLTYPE hook_device_set_background_processing_mode(
    ID3D12Device6 *self,
    D3D12_BACKGROUND_PROCESSING_MODE mode,
    D3D12_MEASUREMENTS_ACTION action,
    HANDLE event,
    BOOL *further_measurements_desired);
HRESULT STDMETHODCALLTYPE hook_device_add_to_state_object(
    ID3D12Device7 *self,
    const D3D12_STATE_OBJECT_DESC *addition,
    ID3D12StateObject *state_object_to_grow_from,
    REFIID riid,
    void **new_state_object);
HRESULT STDMETHODCALLTYPE hook_device_create_protected_resource_session1(
    ID3D12Device7 *self,
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
    REFIID riid,
    void **session);
HRESULT STDMETHODCALLTYPE hook_device_create_committed_resource2(
    ID3D12Device8 *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **resource);
HRESULT STDMETHODCALLTYPE hook_device_create_placed_resource1(
    ID3D12Device8 *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource);
HRESULT STDMETHODCALLTYPE hook_device_create_shader_cache_session(
    ID3D12Device9 *self,
    const D3D12_SHADER_CACHE_SESSION_DESC *desc,
    REFIID riid,
    void **session);
HRESULT STDMETHODCALLTYPE hook_device_shader_cache_control(
    ID3D12Device9 *self,
    D3D12_SHADER_CACHE_KIND_FLAGS kinds,
    D3D12_SHADER_CACHE_CONTROL_FLAGS control);
HRESULT STDMETHODCALLTYPE hook_device_create_command_queue1(
    ID3D12Device9 *self,
    const D3D12_COMMAND_QUEUE_DESC *desc,
    REFIID creator_id,
    REFIID riid,
    void **command_queue);
HRESULT STDMETHODCALLTYPE hook_device_create_descriptor_heap(
    ID3D12Device *self,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc,
    REFIID riid,
    void **descriptor_heap);
HRESULT STDMETHODCALLTYPE hook_device_create_query_heap(
    ID3D12Device *self,
    const D3D12_QUERY_HEAP_DESC *desc,
    REFIID riid,
    void **query_heap);
HRESULT STDMETHODCALLTYPE hook_device_create_root_signature(
    ID3D12Device *self,
    UINT node_mask,
    const void *bytecode,
    SIZE_T bytecode_length,
    REFIID riid,
    void **root_signature);
void STDMETHODCALLTYPE hook_device_create_sampler(
    ID3D12Device *self,
    const D3D12_SAMPLER_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
void STDMETHODCALLTYPE hook_device_copy_descriptors(
    ID3D12Device *self,
    UINT dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const UINT *dst_descriptor_range_sizes,
    UINT src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const UINT *src_descriptor_range_sizes,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);
void STDMETHODCALLTYPE hook_device_copy_descriptors_simple(
    ID3D12Device *self,
    UINT descriptor_count,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_start,
    D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_start,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type);
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
HRESULT STDMETHODCALLTYPE hook_device_create_heap(
    ID3D12Device *self,
    const D3D12_HEAP_DESC *desc,
    REFIID riid,
    void **heap);
HRESULT STDMETHODCALLTYPE hook_device_create_placed_resource(
    ID3D12Device *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
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
void STDMETHODCALLTYPE hook_command_list_clear_state(
    ID3D12GraphicsCommandList *self,
    ID3D12PipelineState *pipeline_state);
void STDMETHODCALLTYPE hook_command_list_om_set_blend_factor(
    ID3D12GraphicsCommandList *self,
    const FLOAT blend_factor[4]);
void STDMETHODCALLTYPE hook_command_list_om_set_stencil_ref(
    ID3D12GraphicsCommandList *self,
    UINT stencil_ref);
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
void STDMETHODCALLTYPE hook_command_list_clear_unordered_access_view_uint(
    ID3D12GraphicsCommandList *self,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    ID3D12Resource *resource,
    const UINT values[4],
    UINT rect_count,
    const D3D12_RECT *rects);
void STDMETHODCALLTYPE hook_command_list_clear_unordered_access_view_float(
    ID3D12GraphicsCommandList *self,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    ID3D12Resource *resource,
    const FLOAT values[4],
    UINT rect_count,
    const D3D12_RECT *rects);
void STDMETHODCALLTYPE hook_command_list_discard_resource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *resource,
    const D3D12_DISCARD_REGION *region);
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
void STDMETHODCALLTYPE hook_command_list_execute_bundle(
    ID3D12GraphicsCommandList *self,
    ID3D12GraphicsCommandList *bundle_command_list);
void STDMETHODCALLTYPE hook_command_list_copy_buffer_region(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    UINT64 dst_offset,
    ID3D12Resource *src,
    UINT64 src_offset,
    UINT64 byte_count);
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
void STDMETHODCALLTYPE hook_command_list_begin_query(
    ID3D12GraphicsCommandList *self,
    ID3D12QueryHeap *heap,
    D3D12_QUERY_TYPE type,
    UINT index);
void STDMETHODCALLTYPE hook_command_list_end_query(
    ID3D12GraphicsCommandList *self,
    ID3D12QueryHeap *heap,
    D3D12_QUERY_TYPE type,
    UINT index);
void STDMETHODCALLTYPE hook_command_list_resolve_query_data(
    ID3D12GraphicsCommandList *self,
    ID3D12QueryHeap *heap,
    D3D12_QUERY_TYPE type,
    UINT start_index,
    UINT query_count,
    ID3D12Resource *dst_buffer,
    UINT64 aligned_dst_buffer_offset);
void STDMETHODCALLTYPE hook_command_list_set_predication(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *buffer,
    UINT64 aligned_buffer_offset,
    D3D12_PREDICATION_OP operation);
void STDMETHODCALLTYPE hook_command_list_resolve_subresource_region(
    ID3D12GraphicsCommandList1 *self,
    ID3D12Resource *dst,
    UINT dst_subresource,
    UINT dst_x,
    UINT dst_y,
    ID3D12Resource *src,
    UINT src_subresource,
    D3D12_RECT *src_rect,
    DXGI_FORMAT format,
    D3D12_RESOLVE_MODE mode);
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
void STDMETHODCALLTYPE hook_command_list_write_buffer_immediate(
    ID3D12GraphicsCommandList2 *self,
    UINT count,
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);
void STDMETHODCALLTYPE hook_command_list_begin_render_pass(
    ID3D12GraphicsCommandList4 *self,
    UINT render_target_count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
    D3D12_RENDER_PASS_FLAGS flags);
void STDMETHODCALLTYPE hook_command_list_end_render_pass(ID3D12GraphicsCommandList4 *self);
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
  decltype(std::declval<ID3D12DeviceVtbl>().CreateQueryHeap) create_query_heap = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateRootSignature) create_root_signature = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateSampler) create_sampler = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CopyDescriptors) copy_descriptors = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CopyDescriptorsSimple) copy_descriptors_simple = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateConstantBufferView) create_constant_buffer_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateShaderResourceView) create_shader_resource_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateUnorderedAccessView) create_unordered_access_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateRenderTargetView) create_render_target_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateDepthStencilView) create_depth_stencil_view = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommittedResource) create_committed_resource = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateHeap) create_heap = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreatePlacedResource) create_placed_resource = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateFence) create_fence = nullptr;
  decltype(std::declval<ID3D12DeviceVtbl>().CreateCommandSignature) create_command_signature = nullptr;
};

struct Device2HookState {
  ID3D12Device2Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device2Vtbl>().CreatePipelineState) create_pipeline_state = nullptr;
};

struct Device3HookState {
  ID3D12Device3Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device3Vtbl>().OpenExistingHeapFromAddress) open_existing_heap_from_address = nullptr;
};

struct Device4HookState {
  ID3D12Device4Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device4Vtbl>().CreateCommandList1) create_command_list1 = nullptr;
  decltype(std::declval<ID3D12Device4Vtbl>().CreateHeap1) create_heap1 = nullptr;
};

struct Device6HookState {
  ID3D12Device6Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device6Vtbl>().SetBackgroundProcessingMode) set_background_processing_mode = nullptr;
};

struct Device7HookState {
  ID3D12Device7Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device7Vtbl>().AddToStateObject) add_to_state_object = nullptr;
  decltype(std::declval<ID3D12Device7Vtbl>().CreateProtectedResourceSession1) create_protected_resource_session1 = nullptr;
};

struct Device8HookState {
  ID3D12Device8Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device8Vtbl>().CreateCommittedResource2) create_committed_resource2 = nullptr;
  decltype(std::declval<ID3D12Device8Vtbl>().CreatePlacedResource1) create_placed_resource1 = nullptr;
};

struct Device9HookState {
  ID3D12Device9Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12Device9Vtbl>().CreateShaderCacheSession) create_shader_cache_session = nullptr;
  decltype(std::declval<ID3D12Device9Vtbl>().ShaderCacheControl) shader_cache_control = nullptr;
  decltype(std::declval<ID3D12Device9Vtbl>().CreateCommandQueue1) create_command_queue1 = nullptr;
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
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ClearState) clear_state = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetPipelineState) set_pipeline_state = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().OMSetBlendFactor) om_set_blend_factor = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().OMSetStencilRef) om_set_stencil_ref = nullptr;
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
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ClearUnorderedAccessViewUint) clear_unordered_access_view_uint = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ClearUnorderedAccessViewFloat) clear_unordered_access_view_float = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DiscardResource) discard_resource = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().IASetPrimitiveTopology) ia_set_primitive_topology = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().IASetVertexBuffers) ia_set_vertex_buffers = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().IASetIndexBuffer) ia_set_index_buffer = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ResourceBarrier) resource_barrier = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetDescriptorHeaps) set_descriptor_heaps = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DrawInstanced) draw_instanced = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().DrawIndexedInstanced) draw_indexed_instanced = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().Dispatch) dispatch = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ExecuteIndirect) execute_indirect = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ExecuteBundle) execute_bundle = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().CopyBufferRegion) copy_buffer_region = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().CopyTextureRegion) copy_texture_region = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().CopyResource) copy_resource = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ResolveSubresource) resolve_subresource = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().BeginQuery) begin_query = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().EndQuery) end_query = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().ResolveQueryData) resolve_query_data = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandListVtbl>().SetPredication) set_predication = nullptr;
};

struct CommandList4HookState {
  ID3D12GraphicsCommandList4Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList4Vtbl>().BeginRenderPass) begin_render_pass = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList4Vtbl>().EndRenderPass) end_render_pass = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList4Vtbl>().DispatchRays) dispatch_rays = nullptr;
};

struct CommandList1HookState {
  ID3D12GraphicsCommandList1Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList1Vtbl>().ResolveSubresourceRegion) resolve_subresource_region = nullptr;
};

struct CommandList2HookState {
  ID3D12GraphicsCommandList2Vtbl *vtable = nullptr;
  decltype(std::declval<ID3D12GraphicsCommandList2Vtbl>().WriteBufferImmediate) write_buffer_immediate = nullptr;
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

struct CapturedMappedRange {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t hash = 0;
};

struct MappedResourceState {
  void *data = nullptr;
  UINT subresource = 0;
  std::vector<CapturedMappedRange> captured_ranges;
};

struct ResourceGpuVirtualAddressState {
  apitrace::trace::ObjectId object_id = 0;
  std::uint64_t base = 0;
  std::uint64_t width = 0;
  std::uint64_t create_sequence = 0;
};

struct ResourceCaptureInfo {
  apitrace::trace::ObjectId object_id = 0;
  D3D12_RESOURCE_DESC desc = {};
  bool has_desc = false;
  UINT heap_type = 0;
  bool cpu_writable = false;
};

struct GpuVirtualAddressResolve {
  bool resolved = false;
  apitrace::trace::ObjectId object_id = 0;
  std::uint64_t offset = 0;
  std::uint64_t width = 0;
  const char *status = "missing";
};

struct MappedGpuvaUseRange {
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  std::uint64_t size = 0;
  bool valid = false;
};

struct CommandListMappedUseState {
  std::unordered_map<UINT, MappedGpuvaUseRange> graphics_root_cbvs;
  std::unordered_map<UINT, MappedGpuvaUseRange> compute_root_cbvs;
  std::vector<MappedGpuvaUseRange> vertex_buffers;
  MappedGpuvaUseRange index_buffer;
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
  std::unordered_map<ID3D12Device2Vtbl *, Device2HookState> device2_hooks;
  std::unordered_map<ID3D12Device3Vtbl *, Device3HookState> device3_hooks;
  std::unordered_map<ID3D12Device4Vtbl *, Device4HookState> device4_hooks;
  std::unordered_map<ID3D12Device6Vtbl *, Device6HookState> device6_hooks;
  std::unordered_map<ID3D12Device7Vtbl *, Device7HookState> device7_hooks;
  std::unordered_map<ID3D12Device8Vtbl *, Device8HookState> device8_hooks;
  std::unordered_map<ID3D12Device9Vtbl *, Device9HookState> device9_hooks;
  std::unordered_map<ID3D12CommandQueueVtbl *, CommandQueueHookState> queue_hooks;
  std::unordered_map<ID3D12CommandAllocatorVtbl *, CommandAllocatorHookState> allocator_hooks;
  std::unordered_map<ID3D12GraphicsCommandListVtbl *, CommandListHookState> command_list_hooks;
  std::unordered_map<ID3D12GraphicsCommandList1Vtbl *, CommandList1HookState> command_list1_hooks;
  std::unordered_map<ID3D12GraphicsCommandList2Vtbl *, CommandList2HookState> command_list2_hooks;
  std::unordered_map<ID3D12GraphicsCommandList4Vtbl *, CommandList4HookState> command_list4_hooks;
  std::unordered_map<ID3D12GraphicsCommandList6Vtbl *, CommandList6HookState> command_list6_hooks;
  std::unordered_map<ID3D12FenceVtbl *, FenceHookState> fence_hooks;
  std::unordered_map<ID3D12ResourceVtbl *, ResourceHookState> resource_hooks;
  std::unordered_map<ID3D12Resource *, MappedResourceState> mapped_resources;
  std::unordered_map<ID3D12Resource *, ResourceGpuVirtualAddressState> resource_gpu_virtual_addresses;
  std::unordered_map<ID3D12Resource *, ResourceCaptureInfo> resource_capture_infos;
  std::unordered_map<ID3D12Heap *, UINT> heap_types;
  std::unordered_map<ID3D12GraphicsCommandList *, std::vector<ID3D12Resource *>> command_list_resources;
  std::unordered_map<ID3D12GraphicsCommandList *, CommandListMappedUseState> command_list_mapped_uses;
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

bool proxy_hooks_disabled()
{
  const char *value = std::getenv("APITRACE_D3D12_PROXY_DISABLE_HOOKS");
  return value && *value && std::strcmp(value, "0") != 0;
}

class ScopedCaptureSuppression {
public:
  ScopedCaptureSuppression()
  {
    ++g_capture_suppression_depth;
  }

  ~ScopedCaptureSuppression()
  {
    --g_capture_suppression_depth;
  }
};

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
    proxy_debug_logf("D3D12 proxy loading downstream %s", path.c_str());
    state.downstream.module = LoadLibraryA(path.c_str());
    if (!state.downstream.module) {
      proxy_debug_logf("D3D12 proxy failed to load downstream %s", path.c_str());
      return;
    }
    proxy_debug_logf("D3D12 proxy loaded downstream module=%p", state.downstream.module);

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
    proxy_debug_logf(
        "D3D12 proxy exports create_device=%p debug=%p serialize_root=%p serialize_versioned=%p",
        reinterpret_cast<void *>(state.downstream.create_device),
        reinterpret_cast<void *>(state.downstream.get_debug_interface),
        reinterpret_cast<void *>(state.downstream.serialize_root_signature),
        reinterpret_cast<void *>(state.downstream.serialize_versioned_root_signature));
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
const void *registered_command_list_key_locked(ID3D12CommandList *command_list);
ID3D12GraphicsCommandList *query_graphics_command_list(ID3D12CommandList *command_list);
void *query_iunknown_identity(void *object);
void release_iunknown(void *object);

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
      auto hook = command_list_hook_for(graphics_list);
      ID3D12GraphicsCommandList *queried_graphics_list = nullptr;
      if (!hook.vtable) {
        queried_graphics_list = query_graphics_command_list(command_list);
        hook = command_list_hook_for(queried_graphics_list);
      }
      if (!hook.vtable) {
        release_iunknown(queried_graphics_list);
        continue;
      }
      entries_.push_back({command_list, command_list->lpVtbl});
      command_list->lpVtbl = reinterpret_cast<ID3D12CommandListVtbl *>(hook.vtable);
      release_iunknown(queried_graphics_list);
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
    auto *command_list = static_cast<ID3D12GraphicsCommandList *>(
        const_cast<void *>(registered_command_list_key_locked(command_lists[index])));
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

std::string guid_string(REFGUID guid)
{
  char text[37] = {};
  std::snprintf(
      text,
      sizeof(text),
      "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      static_cast<unsigned long>(guid.Data1),
      static_cast<unsigned int>(guid.Data2),
      static_cast<unsigned int>(guid.Data3),
      static_cast<unsigned int>(guid.Data4[0]),
      static_cast<unsigned int>(guid.Data4[1]),
      static_cast<unsigned int>(guid.Data4[2]),
      static_cast<unsigned int>(guid.Data4[3]),
      static_cast<unsigned int>(guid.Data4[4]),
      static_cast<unsigned int>(guid.Data4[5]),
      static_cast<unsigned int>(guid.Data4[6]),
      static_cast<unsigned int>(guid.Data4[7]));
  return text;
}

std::string root_signature_descriptor_tables_json(const D3D12_ROOT_SIGNATURE_DESC *desc)
{
  std::ostringstream payload;
  payload << "[";
  bool first_table = true;
  if (desc && desc->pParameters) {
    for (UINT parameter_index = 0; parameter_index < desc->NumParameters; ++parameter_index) {
      const auto &parameter = desc->pParameters[parameter_index];
      if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        continue;
      }
      if (!first_table) {
        payload << ",";
      }
      first_table = false;
      payload << "{\"root_parameter_index\":" << parameter_index
              << ",\"shader_visibility\":" << static_cast<unsigned int>(parameter.ShaderVisibility)
              << ",\"ranges\":[";
      UINT next_offset = 0;
      for (UINT range_index = 0; range_index < parameter.DescriptorTable.NumDescriptorRanges; ++range_index) {
        const auto &range = parameter.DescriptorTable.pDescriptorRanges[range_index];
        if (range_index) {
          payload << ",";
        }
        const UINT offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                ? next_offset
                                : range.OffsetInDescriptorsFromTableStart;
        payload << "{\"type\":" << static_cast<unsigned int>(range.RangeType)
                << ",\"descriptor_count\":" << range.NumDescriptors
                << ",\"base_shader_register\":" << range.BaseShaderRegister
                << ",\"register_space\":" << range.RegisterSpace
                << ",\"offset_from_table_start\":" << offset
                << ",\"flags\":0}";
        if (range.NumDescriptors != UINT_MAX &&
            offset <= UINT_MAX - range.NumDescriptors) {
          next_offset = offset + range.NumDescriptors;
        }
      }
      payload << "]}";
    }
  }
  payload << "]";
  return payload.str();
}

std::string root_signature_parameters_json(const D3D12_ROOT_SIGNATURE_DESC *desc)
{
  std::ostringstream payload;
  payload << "[";
  if (desc && desc->pParameters) {
    for (UINT parameter_index = 0; parameter_index < desc->NumParameters; ++parameter_index) {
      const auto &parameter = desc->pParameters[parameter_index];
      if (parameter_index) {
        payload << ",";
      }
      payload << "{\"root_parameter_index\":" << parameter_index
              << ",\"parameter_type\":" << static_cast<unsigned int>(parameter.ParameterType)
              << ",\"shader_visibility\":" << static_cast<unsigned int>(parameter.ShaderVisibility);
      if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        payload << ",\"range_count\":" << parameter.DescriptorTable.NumDescriptorRanges;
      } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        payload << ",\"shader_register\":" << parameter.Constants.ShaderRegister
                << ",\"register_space\":" << parameter.Constants.RegisterSpace
                << ",\"num_32bit_values\":" << parameter.Constants.Num32BitValues;
      } else {
        payload << ",\"shader_register\":" << parameter.Descriptor.ShaderRegister
                << ",\"register_space\":" << parameter.Descriptor.RegisterSpace;
      }
      payload << "}";
    }
  }
  payload << "]";
  return payload.str();
}

std::string root_signature_descriptor_tables_json(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc)
{
  if (!desc) {
    return "[]";
  }
  if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
    return root_signature_descriptor_tables_json(&desc->Desc_1_0);
  }
  if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
    return "[]";
  }

  std::ostringstream payload;
  payload << "[";
  bool first_table = true;
  const auto &desc1 = desc->Desc_1_1;
  if (desc1.pParameters) {
    for (UINT parameter_index = 0; parameter_index < desc1.NumParameters; ++parameter_index) {
      const auto &parameter = desc1.pParameters[parameter_index];
      if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        continue;
      }
      if (!first_table) {
        payload << ",";
      }
      first_table = false;
      payload << "{\"root_parameter_index\":" << parameter_index
              << ",\"shader_visibility\":" << static_cast<unsigned int>(parameter.ShaderVisibility)
              << ",\"ranges\":[";
      UINT next_offset = 0;
      for (UINT range_index = 0; range_index < parameter.DescriptorTable.NumDescriptorRanges; ++range_index) {
        const auto &range = parameter.DescriptorTable.pDescriptorRanges[range_index];
        if (range_index) {
          payload << ",";
        }
        const UINT offset = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                ? next_offset
                                : range.OffsetInDescriptorsFromTableStart;
        payload << "{\"type\":" << static_cast<unsigned int>(range.RangeType)
                << ",\"descriptor_count\":" << range.NumDescriptors
                << ",\"base_shader_register\":" << range.BaseShaderRegister
                << ",\"register_space\":" << range.RegisterSpace
                << ",\"offset_from_table_start\":" << offset
                << ",\"flags\":" << static_cast<unsigned int>(range.Flags) << "}";
        if (range.NumDescriptors != UINT_MAX &&
            offset <= UINT_MAX - range.NumDescriptors) {
          next_offset = offset + range.NumDescriptors;
        }
      }
      payload << "]}";
    }
  }
  payload << "]";
  return payload.str();
}

std::string root_signature_parameters_json(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc)
{
  if (!desc) {
    return "[]";
  }
  if (desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
    return root_signature_parameters_json(&desc->Desc_1_0);
  }
  if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
    return "[]";
  }

  std::ostringstream payload;
  payload << "[";
  const auto &desc1 = desc->Desc_1_1;
  if (desc1.pParameters) {
    for (UINT parameter_index = 0; parameter_index < desc1.NumParameters; ++parameter_index) {
      const auto &parameter = desc1.pParameters[parameter_index];
      if (parameter_index) {
        payload << ",";
      }
      payload << "{\"root_parameter_index\":" << parameter_index
              << ",\"parameter_type\":" << static_cast<unsigned int>(parameter.ParameterType)
              << ",\"shader_visibility\":" << static_cast<unsigned int>(parameter.ShaderVisibility);
      if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        payload << ",\"range_count\":" << parameter.DescriptorTable.NumDescriptorRanges;
      } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        payload << ",\"shader_register\":" << parameter.Constants.ShaderRegister
                << ",\"register_space\":" << parameter.Constants.RegisterSpace
                << ",\"num_32bit_values\":" << parameter.Constants.Num32BitValues;
      } else {
        payload << ",\"shader_register\":" << parameter.Descriptor.ShaderRegister
                << ",\"register_space\":" << parameter.Descriptor.RegisterSpace
                << ",\"flags\":" << static_cast<unsigned int>(parameter.Descriptor.Flags);
      }
      payload << "}";
    }
  }
  payload << "]";
  return payload.str();
}

bool root_signature_descriptor_tables_json_from_bytecode(
    const void *bytecode,
    SIZE_T bytecode_length,
    std::string &descriptor_tables_json,
    std::string &root_parameters_json)
{
  if (!bytecode || bytecode_length == 0) {
    return false;
  }
  auto &downstream = downstream_module();
  if (downstream.create_versioned_root_signature_deserializer) {
    ID3D12VersionedRootSignatureDeserializer *deserializer = nullptr;
    if (SUCCEEDED(downstream.create_versioned_root_signature_deserializer(
            bytecode,
            bytecode_length,
            kIidD3D12VersionedRootSignatureDeserializer,
            reinterpret_cast<void **>(&deserializer))) &&
        deserializer) {
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc = nullptr;
      if (SUCCEEDED(deserializer->lpVtbl->GetRootSignatureDescAtVersion(
              deserializer,
              D3D_ROOT_SIGNATURE_VERSION_1_1,
              &desc)) &&
          desc &&
          desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        descriptor_tables_json = root_signature_descriptor_tables_json(desc);
        root_parameters_json = root_signature_parameters_json(desc);
        deserializer->lpVtbl->Release(deserializer);
        return true;
      }
      const auto *unconverted = deserializer->lpVtbl->GetUnconvertedRootSignatureDesc(deserializer);
      descriptor_tables_json = root_signature_descriptor_tables_json(unconverted);
      root_parameters_json = root_signature_parameters_json(unconverted);
      deserializer->lpVtbl->Release(deserializer);
      return unconverted != nullptr;
    }
  }
  if (downstream.create_root_signature_deserializer) {
    ID3D12RootSignatureDeserializer *deserializer = nullptr;
    if (SUCCEEDED(downstream.create_root_signature_deserializer(
            bytecode,
            bytecode_length,
            kIidD3D12RootSignatureDeserializer,
            reinterpret_cast<void **>(&deserializer))) &&
        deserializer) {
      const auto *desc = deserializer->lpVtbl->GetRootSignatureDesc(deserializer);
      descriptor_tables_json = root_signature_descriptor_tables_json(desc);
      root_parameters_json = root_signature_parameters_json(desc);
      deserializer->lpVtbl->Release(deserializer);
      return desc != nullptr;
    }
  }
  return false;
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
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    payload << "\"most_detailed_mip\":" << desc->Texture1D.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture1D.MipLevels
            << ",\"resource_min_lod_clamp\":" << desc->Texture1D.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"most_detailed_mip\":" << desc->Texture1DArray.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture1DArray.MipLevels
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize
            << ",\"resource_min_lod_clamp\":" << desc->Texture1DArray.ResourceMinLODClamp;
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
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    payload << "\"first_array_slice\":" << desc->Texture2DMSArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DMSArray.ArraySize;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    payload << "\"most_detailed_mip\":" << desc->Texture3D.MostDetailedMip
            << ",\"mip_levels\":" << desc->Texture3D.MipLevels
            << ",\"resource_min_lod_clamp\":" << desc->Texture3D.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    payload << "\"most_detailed_mip\":" << desc->TextureCube.MostDetailedMip
            << ",\"mip_levels\":" << desc->TextureCube.MipLevels
            << ",\"resource_min_lod_clamp\":" << desc->TextureCube.ResourceMinLODClamp;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    payload << "\"most_detailed_mip\":" << desc->TextureCubeArray.MostDetailedMip
            << ",\"mip_levels\":" << desc->TextureCubeArray.MipLevels
            << ",\"first_2d_array_face\":" << desc->TextureCubeArray.First2DArrayFace
            << ",\"num_cubes\":" << desc->TextureCubeArray.NumCubes
            << ",\"resource_min_lod_clamp\":" << desc->TextureCubeArray.ResourceMinLODClamp;
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
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    payload << "\"mip_slice\":" << desc->Texture1D.MipSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"mip_slice\":" << desc->Texture1DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize;
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
  case D3D12_RTV_DIMENSION_TEXTURE1D:
    payload << "\"mip_slice\":" << desc->Texture1D.MipSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    payload << "\"mip_slice\":" << desc->Texture1DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture1DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture1DArray.ArraySize;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    payload << "\"mip_slice\":" << desc->Texture2DArray.MipSlice
            << ",\"first_array_slice\":" << desc->Texture2DArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DArray.ArraySize
            << ",\"plane_slice\":" << desc->Texture2DArray.PlaneSlice;
    break;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    payload << "\"first_array_slice\":" << desc->Texture2DMSArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DMSArray.ArraySize;
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
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    payload << "\"first_array_slice\":" << desc->Texture2DMSArray.FirstArraySlice
            << ",\"array_size\":" << desc->Texture2DMSArray.ArraySize;
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

const void *registered_object_key_locked(const void *object)
{
  if (!object) {
    return nullptr;
  }
  return lookup_object_id_locked(object) == 0 ? nullptr : object;
}

const void *registered_command_list_key_locked(ID3D12CommandList *command_list)
{
  auto *graphics_list = reinterpret_cast<ID3D12GraphicsCommandList *>(command_list);
  if (const void *key = registered_object_key_locked(graphics_list)) {
    return key;
  }
  if (const void *key = registered_object_key_locked(command_list)) {
    return key;
  }
  return nullptr;
}

const void *remember_registered_identity_alias_locked(void *object, void *identity)
{
  if (!object || !identity) {
    return nullptr;
  }
  if (registered_object_key_locked(identity)) {
    remember_object_alias_locked(object, identity);
    return identity;
  }
  if (registered_object_key_locked(object)) {
    remember_object_alias_locked(identity, object);
    return object;
  }
  return nullptr;
}

template <typename VTable, typename Field, typename Replacement>
void patch_vtable_field_cast(VTable *vtable, Field VTable::*field, Replacement replacement)
{
  patch_vtable_field(vtable, field, reinterpret_cast<Field>(replacement));
}

void record_object_create_locked(const apitrace::trace::ObjectRecord &record, std::string payload_json = "{}")
{
  if (capture_recording_suppressed()) {
    return;
  }
  if (auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12)) {
    apitrace::trace::EventRecord event;
    event.kind = apitrace::trace::EventKind::ObjectCreate;
    event.callsite.sequence = capture_state().next_sequence++;
    event.callsite.function_name = "D3DObjectCreate";
    event.object_id = record.object_id;
    event.object_kind = record.kind;
    event.parent_object_id = record.parent_object_id;
    event.object_debug_name = record.debug_name;
    event.payload = std::move(payload_json);
    session->append_call_event(std::move(event));
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
    record_object_create_locked(record);

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

bool object_kind_known_locked(const void *object, apitrace::trace::ObjectKind kind)
{
  const auto &state = capture_state();
  const auto it = state.objects.find(object);
  return it != state.objects.end() && it->second.kind == kind;
}

void remember_resource_capture_info_locked(
    ID3D12Resource *resource,
    apitrace::trace::ObjectId object_id,
    const D3D12_RESOURCE_DESC *desc,
    UINT heap_type,
    bool cpu_writable)
{
  if (!resource) {
    return;
  }

  ResourceCaptureInfo info;
  info.object_id = object_id;
  if (desc) {
    info.desc = *desc;
    info.has_desc = true;
  }
  info.heap_type = heap_type;
  info.cpu_writable = cpu_writable;
  capture_state().resource_capture_infos[resource] = info;
}

void remember_resource_capture_info_locked(
    ID3D12Resource *resource,
    apitrace::trace::ObjectId object_id,
    const D3D12_RESOURCE_DESC1 *desc,
    UINT heap_type,
    bool cpu_writable)
{
  if (!desc) {
    remember_resource_capture_info_locked(resource, object_id, static_cast<const D3D12_RESOURCE_DESC *>(nullptr), heap_type, cpu_writable);
    return;
  }

  D3D12_RESOURCE_DESC legacy_desc = {};
  legacy_desc.Dimension = desc->Dimension;
  legacy_desc.Alignment = desc->Alignment;
  legacy_desc.Width = desc->Width;
  legacy_desc.Height = desc->Height;
  legacy_desc.DepthOrArraySize = desc->DepthOrArraySize;
  legacy_desc.MipLevels = desc->MipLevels;
  legacy_desc.Format = desc->Format;
  legacy_desc.SampleDesc = desc->SampleDesc;
  legacy_desc.Layout = desc->Layout;
  legacy_desc.Flags = desc->Flags;
  remember_resource_capture_info_locked(resource, object_id, &legacy_desc, heap_type, cpu_writable);
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

void append_object_ref(std::vector<apitrace::trace::ObjectId> &refs, apitrace::trace::ObjectId object_id)
{
  if (object_id == 0) {
    return;
  }
  if (std::find(refs.begin(), refs.end(), object_id) == refs.end()) {
    refs.push_back(object_id);
  }
}

void append_gpuva_object_ref(std::vector<apitrace::trace::ObjectId> &refs, const GpuVirtualAddressResolve &resolve)
{
  append_object_ref(refs, resolve.object_id);
}

std::vector<apitrace::trace::ObjectId> collect_object_refs_locked(
    const std::vector<const void *> &objects,
    const std::vector<apitrace::trace::ObjectId> &extra_refs = {})
{
  std::vector<apitrace::trace::ObjectId> refs;
  refs.reserve(objects.size() + extra_refs.size());
  for (const void *object : objects) {
    append_object_ref(refs, lookup_object_id_locked(object));
  }
  for (const auto object_id : extra_refs) {
    append_object_ref(refs, object_id);
  }
  return refs;
}

void record_call_locked(
    std::string function_name,
    HRESULT result_code,
    const std::vector<const void *> &objects,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    std::string payload_json,
    const std::vector<apitrace::trace::ObjectId> &extra_object_refs = {})
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
    event.object_refs = collect_object_refs_locked(objects, extra_object_refs);
    event.blob_refs = blob_refs;
    event.payload = std::move(payload_json);
    session->append_call_event(event);
  }
}

GpuVirtualAddressResolve resolve_gpu_virtual_address_locked(std::uint64_t address);

apitrace::trace::AssetRecord register_asset_bytes_locked(
    apitrace::trace::AssetKind kind,
    std::string debug_name,
    const void *data,
    std::size_t size);

std::uint64_t fnv1a64_bytes(const void *data, std::size_t size)
{
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kPrime;
  }
  return hash;
}

bool mapped_range_captured(const MappedResourceState &mapped, std::uint64_t begin, std::uint64_t end, std::uint64_t hash)
{
  for (const auto &range : mapped.captured_ranges) {
    if (range.begin == begin && range.end == end && range.hash == hash) {
      return true;
    }
  }
  return false;
}

void mark_mapped_range_captured(MappedResourceState &mapped, std::uint64_t begin, std::uint64_t end, std::uint64_t hash)
{
  if (end <= begin) {
    return;
  }
  auto &ranges = mapped.captured_ranges;
  for (auto &range : ranges) {
    if (range.begin == begin && range.end == end) {
      range.hash = hash;
      return;
    }
  }
  ranges.push_back(CapturedMappedRange{begin, end, hash});
}

ID3D12Resource *resource_from_object_id_locked(apitrace::trace::ObjectId object_id)
{
  if (object_id == 0) {
    return nullptr;
  }
  for (const auto &[resource, info] : capture_state().resource_capture_infos) {
    if (info.object_id == object_id) {
      return resource;
    }
  }
  return nullptr;
}

void record_mapped_resource_range_update_locked(
    ID3D12Resource *resource,
    MappedResourceState &mapped,
    const ResourceCaptureInfo &info,
    std::uint64_t begin,
    std::uint64_t end)
{
  if (capture_recording_suppressed() || !resource || !mapped.data || end <= begin) {
    return;
  }

  const auto size64 = end - begin;
  if (size64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return;
  }

  const auto size = static_cast<std::size_t>(size64);
  const auto *bytes = static_cast<const std::uint8_t *>(mapped.data) + static_cast<std::size_t>(begin);
  const auto hash = fnv1a64_bytes(bytes, size);
  if (mapped_range_captured(mapped, begin, end, hash)) {
    return;
  }

  const auto asset = register_asset_bytes_locked(
      apitrace::trace::AssetKind::Buffer,
      "d3d12-mapped-resource-use",
      bytes,
      size);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  const auto buffer_path = asset.relative_path.generic_string();
  std::ostringstream payload;
  payload << "{\"resource_object_id\":" << object_id_json(info.object_id)
          << ",\"subresource\":" << mapped.subresource
          << ",\"written_begin\":" << begin
          << ",\"written_end\":" << end
          << ",\"written_size\":" << size64
          << ",\"buffer_path\":\"" << buffer_path << "\""
          << ",\"capture_reason\":\"mapped_resource_use\"}";
  record_call_locked("ID3D12Resource::Unmap", S_OK, {resource}, {asset.blob_id}, payload.str());
  mark_mapped_range_captured(mapped, begin, end, hash);
}

void capture_mapped_resource_range_before_use_locked(
    ID3D12Resource *resource,
    std::uint64_t begin,
    std::uint64_t size)
{
  if (!resource || size == 0) {
    return;
  }

  const auto mapped_it = capture_state().mapped_resources.find(resource);
  if (mapped_it == capture_state().mapped_resources.end()) {
    return;
  }

  const auto info_it = capture_state().resource_capture_infos.find(resource);
  if (info_it == capture_state().resource_capture_infos.end() || !info_it->second.cpu_writable ||
      !info_it->second.has_desc || info_it->second.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      info_it->second.desc.Width == 0 || begin >= info_it->second.desc.Width) {
    return;
  }

  const auto max_size = info_it->second.desc.Width - begin;
  const auto clamped_size = std::min(size, max_size);
  if (clamped_size == 0) {
    return;
  }

  if (begin > std::numeric_limits<std::uint64_t>::max() - clamped_size) {
    return;
  }

  record_mapped_resource_range_update_locked(
      resource,
      mapped_it->second,
      info_it->second,
      begin,
      begin + clamped_size);
}

void capture_mapped_gpuva_range_before_use_locked(
    const GpuVirtualAddressResolve &resolve,
    std::uint64_t size)
{
  if (!resolve.resolved || resolve.object_id == 0 || size == 0) {
    return;
  }

  ID3D12Resource *resource = resource_from_object_id_locked(resolve.object_id);
  capture_mapped_resource_range_before_use_locked(resource, resolve.offset, size);
}

void capture_mapped_gpuva_range_before_use_chunked_locked(
    const GpuVirtualAddressResolve &resolve,
    std::uint64_t size)
{
  if (!resolve.resolved || resolve.object_id == 0 || size == 0) {
    return;
  }

  ID3D12Resource *resource = resource_from_object_id_locked(resolve.object_id);
  if (!resource) {
    return;
  }
  if (resolve.offset > std::numeric_limits<std::uint64_t>::max() - size) {
    return;
  }
  const auto end = resolve.offset + size;
  const auto chunk_mask = kMappedUseSnapshotChunkBytes - 1u;
  if (end > std::numeric_limits<std::uint64_t>::max() - chunk_mask) {
    return;
  }
  const auto begin = resolve.offset & ~chunk_mask;
  const auto aligned_end = (end + chunk_mask) & ~chunk_mask;

  for (std::uint64_t chunk_begin = begin; chunk_begin < aligned_end;
       chunk_begin += kMappedUseSnapshotChunkBytes) {
    const auto chunk_size = std::min(kMappedUseSnapshotChunkBytes,
                                     aligned_end - chunk_begin);
    capture_mapped_resource_range_before_use_locked(resource, chunk_begin, chunk_size);
  }
}

ID3D12GraphicsCommandList *command_list_mapped_use_key_locked(ID3D12GraphicsCommandList *command_list)
{
  if (!command_list) {
    return nullptr;
  }
  if (const void *key = registered_object_key_locked(command_list)) {
    return static_cast<ID3D12GraphicsCommandList *>(const_cast<void *>(key));
  }
  return command_list;
}

ID3D12GraphicsCommandList *command_list_mapped_use_key_locked(ID3D12CommandList *command_list)
{
  if (!command_list) {
    return nullptr;
  }
  if (const void *key = registered_command_list_key_locked(command_list)) {
    return static_cast<ID3D12GraphicsCommandList *>(const_cast<void *>(key));
  }
  return reinterpret_cast<ID3D12GraphicsCommandList *>(command_list);
}

void capture_mapped_gpuva_range_before_use_locked(const MappedGpuvaUseRange &range)
{
  if (!range.valid || range.address == 0 || range.size == 0) {
    return;
  }
  const auto resolve = resolve_gpu_virtual_address_locked(range.address);
  capture_mapped_gpuva_range_before_use_locked(resolve, range.size);
}

void capture_root_cbv_mapped_gpuva_range_before_use_locked(const MappedGpuvaUseRange &range)
{
  if (!range.valid || range.address == 0 || range.size == 0) {
    return;
  }
  const auto resolve = resolve_gpu_virtual_address_locked(range.address);
  capture_mapped_gpuva_range_before_use_chunked_locked(resolve, range.size);
}

void remember_root_cbv_range_locked(
    ID3D12GraphicsCommandList *command_list,
    bool compute,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  auto &state = capture_state().command_list_mapped_uses[key];
  auto &roots = compute ? state.compute_root_cbvs : state.graphics_root_cbvs;
  if (buffer_location == 0) {
    roots.erase(root_parameter_index);
    return;
  }
  roots[root_parameter_index] = MappedGpuvaUseRange{
      buffer_location,
      kRootConstantBufferSnapshotBytes,
      true,
  };
}

void clear_root_cbv_ranges_locked(ID3D12GraphicsCommandList *command_list, bool compute)
{
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  const auto it = capture_state().command_list_mapped_uses.find(key);
  if (it == capture_state().command_list_mapped_uses.end()) {
    return;
  }
  if (compute) {
    it->second.compute_root_cbvs.clear();
  } else {
    it->second.graphics_root_cbvs.clear();
  }
}

void remember_vertex_buffer_ranges_locked(
    ID3D12GraphicsCommandList *command_list,
    UINT start_slot,
    UINT view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views)
{
  if (view_count == 0) {
    return;
  }
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  auto &buffers = capture_state().command_list_mapped_uses[key].vertex_buffers;
  const auto required_size = static_cast<std::size_t>(start_slot) + static_cast<std::size_t>(view_count);
  if (buffers.size() < required_size) {
    buffers.resize(required_size);
  }
  for (UINT index = 0; index < view_count; ++index) {
    auto &range = buffers[static_cast<std::size_t>(start_slot) + index];
    if (!views || views[index].BufferLocation == 0 || views[index].SizeInBytes == 0) {
      range = {};
      continue;
    }
    range = MappedGpuvaUseRange{views[index].BufferLocation, views[index].SizeInBytes, true};
  }
}

void remember_index_buffer_range_locked(ID3D12GraphicsCommandList *command_list, const D3D12_INDEX_BUFFER_VIEW *view)
{
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  auto &range = capture_state().command_list_mapped_uses[key].index_buffer;
  if (!view || view->BufferLocation == 0 || view->SizeInBytes == 0) {
    range = {};
    return;
  }
  range = MappedGpuvaUseRange{view->BufferLocation, view->SizeInBytes, true};
}

void clear_command_list_mapped_uses_locked(ID3D12GraphicsCommandList *command_list)
{
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  capture_state().command_list_mapped_uses.erase(key);
}

void capture_graphics_mapped_inputs_before_use_locked(ID3D12GraphicsCommandList *command_list, bool include_index_buffer)
{
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  const auto it = capture_state().command_list_mapped_uses.find(key);
  if (it == capture_state().command_list_mapped_uses.end()) {
    return;
  }
  for (const auto &[root_parameter_index, range] : it->second.graphics_root_cbvs) {
    (void)root_parameter_index;
    capture_root_cbv_mapped_gpuva_range_before_use_locked(range);
  }
  for (const auto &range : it->second.vertex_buffers) {
    capture_mapped_gpuva_range_before_use_locked(range);
  }
  if (include_index_buffer) {
    capture_mapped_gpuva_range_before_use_locked(it->second.index_buffer);
  }
}

void capture_compute_mapped_inputs_before_use_locked(ID3D12GraphicsCommandList *command_list)
{
  auto *key = command_list_mapped_use_key_locked(command_list);
  if (!key) {
    return;
  }
  const auto it = capture_state().command_list_mapped_uses.find(key);
  if (it == capture_state().command_list_mapped_uses.end()) {
    return;
  }
  for (const auto &[root_parameter_index, range] : it->second.compute_root_cbvs) {
    (void)root_parameter_index;
    capture_root_cbv_mapped_gpuva_range_before_use_locked(range);
  }
}

void capture_command_list_mapped_inputs_before_submit_locked(ID3D12CommandList *command_list)
{
  auto *graphics_list = command_list_mapped_use_key_locked(command_list);
  if (!graphics_list) {
    return;
  }
  capture_graphics_mapped_inputs_before_use_locked(graphics_list, true);
  capture_compute_mapped_inputs_before_use_locked(graphics_list);
}

void capture_command_lists_mapped_inputs_before_submit_locked(UINT count, ID3D12CommandList *const *command_lists)
{
  for (UINT index = 0; command_lists && index < count; ++index) {
    capture_command_list_mapped_inputs_before_submit_locked(command_lists[index]);
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
  auto *session = apitrace::runtime::ensure_process_trace_session(apitrace::trace::ApiKind::D3D12);
  if (!session) {
    return {};
  }

  apitrace::trace::AssetRecord asset;
  asset.blob_id = ++capture_state().next_blob_id;
  asset.kind = kind;
  asset.debug_name = std::move(debug_name);
  asset.payload_bytes.resize(size);
  if (size != 0 && data) {
    std::memcpy(asset.payload_bytes.data(), data, size);
  }
  return session->capture_raw_mode() == apitrace::runtime::CaptureOptions::CaptureRawMode::RawOnly
      ? session->stage_raw_asset(std::move(asset))
      : session->register_asset(std::move(asset));
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

D3D12_HEAP_DESC heap_desc(ID3D12Heap *heap)
{
  D3D12_HEAP_DESC desc{};
  if (!heap || !heap->lpVtbl || !heap->lpVtbl->GetDesc) {
    return desc;
  }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  heap->lpVtbl->GetDesc(heap, &desc);
  return desc;
#else
  return heap->lpVtbl->GetDesc(heap);
#endif
}

void ensure_placed_resource_heap_object_locked(ID3D12Device *device, ID3D12Heap *heap)
{
  if (!heap || object_kind_known_locked(heap, apitrace::trace::ObjectKind::Heap)) {
    return;
  }

  const auto parent = lookup_object_id_locked(device);
  register_fresh_object_locked(heap, apitrace::trace::ObjectKind::Heap, "ID3D12Heap", parent);
  const D3D12_HEAP_DESC desc = heap_desc(heap);
  capture_state().heap_types[heap] = static_cast<UINT>(desc.Properties.Type);
  std::ostringstream payload;
  payload << "{\"size_in_bytes\":" << desc.SizeInBytes
          << ",\"alignment\":" << desc.Alignment
          << ",\"heap_type\":" << static_cast<unsigned int>(desc.Properties.Type)
          << ",\"cpu_page_property\":" << static_cast<unsigned int>(desc.Properties.CPUPageProperty)
          << ",\"memory_pool_preference\":" << static_cast<unsigned int>(desc.Properties.MemoryPoolPreference)
          << ",\"creation_node_mask\":" << desc.Properties.CreationNodeMask
          << ",\"visible_node_mask\":" << desc.Properties.VisibleNodeMask
          << ",\"flags\":" << static_cast<unsigned int>(desc.Flags)
          << "}";
  record_call_locked("ID3D12Device::OpenExistingHeap", S_OK, {device, heap}, {}, payload.str());
}

GpuVirtualAddressResolve resolve_gpu_virtual_address_locked(std::uint64_t address)
{
  GpuVirtualAddressResolve resolve;
  if (address == 0) {
    resolve.resolved = true;
    resolve.status = "null";
    return resolve;
  }

  const ResourceGpuVirtualAddressState *best = nullptr;
  for (const auto &[resource, state] : capture_state().resource_gpu_virtual_addresses) {
    (void)resource;
    if (state.base == 0 || address < state.base)
      continue;

    const auto offset = address - state.base;
    if (offset >= state.width)
      continue;

    if (!best || state.create_sequence > best->create_sequence)
      best = &state;
  }

  if (!best) {
    resolve.status = "unmapped";
    return resolve;
  }

  resolve.resolved = true;
  resolve.object_id = best->object_id;
  resolve.offset = address - best->base;
  resolve.width = best->width;
  resolve.status = "mapped";
  return resolve;
}

void append_gpu_virtual_address_resolve_json(std::ostringstream &payload, const GpuVirtualAddressResolve &resolve)
{
  payload << ",\"gpuva_resolve_status\":\"" << resolve.status << "\""
          << ",\"resolved_resource_object_id\":" << object_id_json(resolve.object_id)
          << ",\"resolved_resource_offset\":" << resolve.offset
          << ",\"resolved_resource_width\":" << resolve.width;
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

std::string mip_region_json(const D3D12_MIP_REGION &region)
{
  std::ostringstream payload;
  payload << "{"
          << "\"width\":" << region.Width
          << ",\"height\":" << region.Height
          << ",\"depth\":" << region.Depth
          << "}";
  return payload.str();
}

std::string resource_desc1_json(const D3D12_RESOURCE_DESC1 *desc)
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
          << "\"flags\":" << static_cast<unsigned int>(desc->Flags) << ","
          << "\"sampler_feedback_mip_region\":" << mip_region_json(desc->SamplerFeedbackMipRegion)
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

void append_rect_array_json(std::ostringstream &payload, UINT rect_count, const D3D12_RECT *rects)
{
  payload << ",\"rects\":[";
  for (UINT index = 0; rects && index < rect_count; ++index) {
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

void append_render_pass_clear_value_json(std::ostringstream &payload, const D3D12_RENDER_PASS_BEGINNING_ACCESS_CLEAR_PARAMETERS &clear)
{
  payload << "{\"format\":" << static_cast<unsigned int>(clear.ClearValue.Format)
          << ",\"color\":[" << clear.ClearValue.Color[0]
          << "," << clear.ClearValue.Color[1]
          << "," << clear.ClearValue.Color[2]
          << "," << clear.ClearValue.Color[3]
          << "],\"depth\":" << clear.ClearValue.DepthStencil.Depth
          << ",\"stencil\":" << static_cast<unsigned int>(clear.ClearValue.DepthStencil.Stencil)
          << "}";
}

void append_render_pass_beginning_access_json(std::ostringstream &payload, const D3D12_RENDER_PASS_BEGINNING_ACCESS &access)
{
  payload << "{\"type\":" << static_cast<unsigned int>(access.Type)
          << ",\"clear\":";
  if (access.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
    append_render_pass_clear_value_json(payload, access.Clear);
  } else {
    payload << "{\"format\":0,\"color\":[0,0,0,0],\"depth\":0,\"stencil\":0}";
  }
  payload << "}";
}

void append_render_pass_ending_access_json_locked(
    std::ostringstream &payload,
    const D3D12_RENDER_PASS_ENDING_ACCESS &access,
    std::vector<ID3D12Resource *> &refs)
{
  payload << "{\"type\":" << static_cast<unsigned int>(access.Type);
  if (access.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
    const auto &resolve = access.Resolve;
    if (resolve.pSrcResource) {
      refs.push_back(resolve.pSrcResource);
    }
    if (resolve.pDstResource) {
      refs.push_back(resolve.pDstResource);
    }
    payload << ",\"src_resource_object_id\":" << object_id_json(lookup_object_id_locked(resolve.pSrcResource))
            << ",\"dst_resource_object_id\":" << object_id_json(lookup_object_id_locked(resolve.pDstResource))
            << ",\"subresource_count\":" << resolve.SubresourceCount
            << ",\"subresources\":[";
    for (UINT index = 0; resolve.pSubresourceParameters && index < resolve.SubresourceCount; ++index) {
      if (index != 0) {
        payload << ",";
      }
      const auto &subresource = resolve.pSubresourceParameters[index];
      payload << "{\"src_subresource\":" << subresource.SrcSubresource
              << ",\"dst_subresource\":" << subresource.DstSubresource
              << ",\"dst_x\":" << subresource.DstX
              << ",\"dst_y\":" << subresource.DstY
              << ",\"src_rect\":{\"left\":" << subresource.SrcRect.left
              << ",\"top\":" << subresource.SrcRect.top
              << ",\"right\":" << subresource.SrcRect.right
              << ",\"bottom\":" << subresource.SrcRect.bottom << "}}";
    }
    payload << "],\"format\":" << static_cast<unsigned int>(resolve.Format)
            << ",\"resolve_mode\":" << static_cast<unsigned int>(resolve.ResolveMode)
            << ",\"preserve_resolve_source\":" << (resolve.PreserveResolveSource ? "true" : "false");
  } else {
    payload << ",\"src_resource_object_id\":0"
            << ",\"dst_resource_object_id\":0"
            << ",\"subresource_count\":0"
            << ",\"subresources\":[]"
            << ",\"format\":0"
            << ",\"resolve_mode\":0"
            << ",\"preserve_resolve_source\":false";
  }
  payload << "}";
}

void append_render_pass_render_target_json_locked(
    std::ostringstream &payload,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC &render_target,
    std::vector<ID3D12Resource *> &refs)
{
  payload << "{\"cpu_descriptor\":" << descriptor_handle_json(render_target.cpuDescriptor)
          << ",\"beginning_access\":";
  append_render_pass_beginning_access_json(payload, render_target.BeginningAccess);
  payload << ",\"ending_access\":";
  append_render_pass_ending_access_json_locked(payload, render_target.EndingAccess, refs);
  payload << "}";
}

void append_render_pass_depth_stencil_json_locked(
    std::ostringstream &payload,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC &depth_stencil,
    std::vector<ID3D12Resource *> &refs)
{
  payload << "{\"cpu_descriptor\":" << descriptor_handle_json(depth_stencil.cpuDescriptor)
          << ",\"depth_beginning_access\":";
  append_render_pass_beginning_access_json(payload, depth_stencil.DepthBeginningAccess);
  payload << ",\"stencil_beginning_access\":";
  append_render_pass_beginning_access_json(payload, depth_stencil.StencilBeginningAccess);
  payload << ",\"depth_ending_access\":";
  append_render_pass_ending_access_json_locked(payload, depth_stencil.DepthEndingAccess, refs);
  payload << ",\"stencil_ending_access\":";
  append_render_pass_ending_access_json_locked(payload, depth_stencil.StencilEndingAccess, refs);
  payload << "}";
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
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return std::string("\"") + field_name + "\":null";
  }
  blob_refs.push_back(asset.blob_id);
  std::ostringstream payload;
  payload << "\"" << field_name << "\":{"
          << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength) << ","
          << "\"" << field_name << "_path\":\"" << asset.relative_path.generic_string() << "\","
          << "\"blob_id\":" << asset.blob_id
          << "}";
  return payload.str();
}

struct ShaderAssetMetadataJson {
  std::string asset_json;
  std::string metadata_json;
};

ShaderAssetMetadataJson shader_asset_metadata_json_locked(
    const char *field_name,
    const D3D12_SHADER_BYTECODE &bytecode,
    std::vector<apitrace::trace::BlobId> &blob_refs)
{
  if (!bytecode.pShaderBytecode || bytecode.BytecodeLength == 0) {
    const auto null_field = std::string("\"") + field_name + "\":null";
    return {null_field, null_field};
  }

  const auto asset = register_asset_bytes_locked(
      apitrace::trace::AssetKind::ShaderDxil,
      std::string("d3d12-") + field_name,
      bytecode.pShaderBytecode,
      bytecode.BytecodeLength);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    const auto null_field = std::string("\"") + field_name + "\":null";
    return {null_field, null_field};
  }
  blob_refs.push_back(asset.blob_id);

  std::ostringstream asset_payload;
  asset_payload << "\"" << field_name << "\":{"
                << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength)
                << ",\"" << field_name << "_path\":\"" << asset.relative_path.generic_string() << "\""
                << "}";

  std::ostringstream metadata_payload;
  metadata_payload << "\"" << field_name << "\":{"
                   << "\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode.BytecodeLength)
                   << ",\"blob_id\":" << asset.blob_id
                   << "}";
  return {asset_payload.str(), metadata_payload.str()};
}

struct StreamShaderAssetJson {
  std::string vs = "\"vs\":null";
  std::string ps = "\"ps\":null";
  std::string ds = "\"ds\":null";
  std::string hs = "\"hs\":null";
  std::string gs = "\"gs\":null";
  std::string cs = "\"cs\":null";
  std::string as = "\"as\":null";
  std::string ms = "\"ms\":null";
};

struct StreamShaderMetadataJson {
  std::string vs = "\"vs\":null";
  std::string ps = "\"ps\":null";
  std::string ds = "\"ds\":null";
  std::string hs = "\"hs\":null";
  std::string gs = "\"gs\":null";
  std::string cs = "\"cs\":null";
  std::string as = "\"as\":null";
  std::string ms = "\"ms\":null";
};

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

std::size_t align_stream_offset(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t pipeline_stream_payload_size(PipelineStateSubobjectType type)
{
  switch (type) {
  case PipelineStateSubobjectType::RootSignature:
    return sizeof(ID3D12RootSignature *);
  case PipelineStateSubobjectType::VS:
  case PipelineStateSubobjectType::PS:
  case PipelineStateSubobjectType::DS:
  case PipelineStateSubobjectType::HS:
  case PipelineStateSubobjectType::GS:
  case PipelineStateSubobjectType::CS:
  case PipelineStateSubobjectType::AS:
  case PipelineStateSubobjectType::MS:
    return sizeof(D3D12_SHADER_BYTECODE);
  case PipelineStateSubobjectType::StreamOutput:
    return sizeof(D3D12_STREAM_OUTPUT_DESC);
  case PipelineStateSubobjectType::Blend:
    return sizeof(D3D12_BLEND_DESC);
  case PipelineStateSubobjectType::SampleMask:
    return sizeof(UINT);
  case PipelineStateSubobjectType::Rasterizer:
    return sizeof(D3D12_RASTERIZER_DESC);
  case PipelineStateSubobjectType::DepthStencil:
    return sizeof(D3D12_DEPTH_STENCIL_DESC);
  case PipelineStateSubobjectType::DepthStencil1:
    return sizeof(DepthStencilDesc1);
  case PipelineStateSubobjectType::InputLayout:
    return sizeof(D3D12_INPUT_LAYOUT_DESC);
  case PipelineStateSubobjectType::IbStripCutValue:
    return sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case PipelineStateSubobjectType::PrimitiveTopology:
    return sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case PipelineStateSubobjectType::RenderTargetFormats:
    return sizeof(RtFormatArray);
  case PipelineStateSubobjectType::DepthStencilFormat:
    return sizeof(DXGI_FORMAT);
  case PipelineStateSubobjectType::SampleDesc:
    return sizeof(DXGI_SAMPLE_DESC);
  case PipelineStateSubobjectType::NodeMask:
    return sizeof(UINT);
  case PipelineStateSubobjectType::CachedPso:
    return sizeof(D3D12_CACHED_PIPELINE_STATE);
  case PipelineStateSubobjectType::Flags:
    return sizeof(D3D12_PIPELINE_STATE_FLAGS);
  case PipelineStateSubobjectType::ViewInstancing:
    return sizeof(ViewInstancingDesc);
  default:
    return 0;
  }
}

std::size_t pipeline_stream_payload_alignment(PipelineStateSubobjectType type)
{
  switch (type) {
  case PipelineStateSubobjectType::RootSignature:
    return alignof(ID3D12RootSignature *);
  case PipelineStateSubobjectType::VS:
  case PipelineStateSubobjectType::PS:
  case PipelineStateSubobjectType::DS:
  case PipelineStateSubobjectType::HS:
  case PipelineStateSubobjectType::GS:
  case PipelineStateSubobjectType::CS:
  case PipelineStateSubobjectType::AS:
  case PipelineStateSubobjectType::MS:
    return alignof(D3D12_SHADER_BYTECODE);
  case PipelineStateSubobjectType::StreamOutput:
    return alignof(D3D12_STREAM_OUTPUT_DESC);
  case PipelineStateSubobjectType::Blend:
    return alignof(D3D12_BLEND_DESC);
  case PipelineStateSubobjectType::SampleMask:
    return alignof(UINT);
  case PipelineStateSubobjectType::Rasterizer:
    return alignof(D3D12_RASTERIZER_DESC);
  case PipelineStateSubobjectType::DepthStencil:
    return alignof(D3D12_DEPTH_STENCIL_DESC);
  case PipelineStateSubobjectType::DepthStencil1:
    return alignof(DepthStencilDesc1);
  case PipelineStateSubobjectType::InputLayout:
    return alignof(D3D12_INPUT_LAYOUT_DESC);
  case PipelineStateSubobjectType::IbStripCutValue:
    return alignof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case PipelineStateSubobjectType::PrimitiveTopology:
    return alignof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case PipelineStateSubobjectType::RenderTargetFormats:
    return alignof(RtFormatArray);
  case PipelineStateSubobjectType::DepthStencilFormat:
    return alignof(DXGI_FORMAT);
  case PipelineStateSubobjectType::SampleDesc:
    return alignof(DXGI_SAMPLE_DESC);
  case PipelineStateSubobjectType::NodeMask:
    return alignof(UINT);
  case PipelineStateSubobjectType::CachedPso:
    return alignof(D3D12_CACHED_PIPELINE_STATE);
  case PipelineStateSubobjectType::Flags:
    return alignof(D3D12_PIPELINE_STATE_FLAGS);
  case PipelineStateSubobjectType::ViewInstancing:
    return alignof(ViewInstancingDesc);
  default:
    return 0;
  }
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
          << "\"pso_raw_version\":1,"
          << "\"pso_kind\":\"graphics\","
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
          << shader_asset_json_locked("ps", desc->PS, blob_refs) << ","
          << shader_asset_json_locked("ds", desc->DS, blob_refs) << ","
          << shader_asset_json_locked("hs", desc->HS, blob_refs) << ","
          << shader_asset_json_locked("gs", desc->GS, blob_refs)
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
          << "\"pso_raw_version\":1,"
          << "\"pso_kind\":\"compute\","
          << "\"root_signature_object_id\":" << object_id_json(lookup_object_id_locked(desc->pRootSignature)) << ","
          << "\"node_mask\":" << desc->NodeMask << ","
          << "\"flags\":" << static_cast<unsigned int>(desc->Flags) << ","
          << shader_asset_json_locked("cs", desc->CS, blob_refs)
          << "}";
  return payload.str();
}

struct StreamPipelineAsset {
  std::string pipeline_json = "{}";
  std::string metadata_json = "{\"source\":\"stream\",\"subobjects\":[]}";
  bool requires_dxmt_backend = false;
  bool compute = false;
};

StreamPipelineAsset stream_pipeline_asset_json_locked(
    const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
    std::vector<apitrace::trace::BlobId> &shader_blob_refs)
{
  StreamPipelineAsset result;
  if (!desc || !desc->pPipelineStateSubobjectStream || desc->SizeInBytes == 0) {
    return result;
  }

  const auto *bytes = static_cast<const std::uint8_t *>(desc->pPipelineStateSubobjectStream);
  const auto stream_size = static_cast<std::size_t>(desc->SizeInBytes);
  std::size_t offset = 0;
  bool first_subobject = true;
  bool has_cs = false;
  bool has_as = false;
  bool has_ms = false;
  StreamShaderAssetJson shader_json;
  StreamShaderMetadataJson shader_metadata_json;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
  graphics.SampleMask = UINT_MAX;
  graphics.SampleDesc.Count = 1;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};

  std::ostringstream stream;
  stream << "{\"source\":\"stream\",\"subobjects\":[";
  while (offset < stream_size) {
    if (stream_size - offset < sizeof(std::uint32_t)) {
      break;
    }

    const auto type_offset = offset;
    const auto raw_type = *reinterpret_cast<const std::uint32_t *>(bytes + offset);
    const auto type = static_cast<PipelineStateSubobjectType>(raw_type);
    offset += sizeof(std::uint32_t);
    const auto payload_size = pipeline_stream_payload_size(type);
    const auto payload_alignment = pipeline_stream_payload_alignment(type);
    if (!payload_size || !payload_alignment) {
      break;
    }
    offset = align_stream_offset(offset, payload_alignment);
    if (stream_size - offset < payload_size) {
      break;
    }

    const auto *subobject = bytes + offset;
    if (!first_subobject) {
      stream << ",";
    }
    first_subobject = false;
    stream << "{\"type\":" << raw_type
           << ",\"type_offset\":" << static_cast<std::uint64_t>(type_offset);

    switch (type) {
    case PipelineStateSubobjectType::RootSignature: {
      const auto root_signature = *reinterpret_cast<ID3D12RootSignature *const *>(subobject);
      graphics.pRootSignature = root_signature;
      compute.pRootSignature = root_signature;
      stream << ",\"root_signature_object_id\":" << object_id_json(lookup_object_id_locked(root_signature));
      break;
    }
    case PipelineStateSubobjectType::VS:
      graphics.VS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      {
        const auto shader = shader_asset_metadata_json_locked("vs", graphics.VS, shader_blob_refs);
        shader_json.vs = shader.asset_json;
        shader_metadata_json.vs = shader.metadata_json;
      }
      stream << "," << shader_metadata_json.vs;
      break;
    case PipelineStateSubobjectType::PS:
      graphics.PS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      {
        const auto shader = shader_asset_metadata_json_locked("ps", graphics.PS, shader_blob_refs);
        shader_json.ps = shader.asset_json;
        shader_metadata_json.ps = shader.metadata_json;
      }
      stream << "," << shader_metadata_json.ps;
      break;
    case PipelineStateSubobjectType::DS:
      graphics.DS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      {
        const auto shader = shader_asset_metadata_json_locked("ds", graphics.DS, shader_blob_refs);
        shader_json.ds = shader.asset_json;
        shader_metadata_json.ds = shader.metadata_json;
      }
      stream << "," << shader_metadata_json.ds;
      break;
    case PipelineStateSubobjectType::HS:
      graphics.HS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      {
        const auto shader = shader_asset_metadata_json_locked("hs", graphics.HS, shader_blob_refs);
        shader_json.hs = shader.asset_json;
        shader_metadata_json.hs = shader.metadata_json;
      }
      stream << "," << shader_metadata_json.hs;
      break;
    case PipelineStateSubobjectType::GS:
      graphics.GS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      {
        const auto shader = shader_asset_metadata_json_locked("gs", graphics.GS, shader_blob_refs);
        shader_json.gs = shader.asset_json;
        shader_metadata_json.gs = shader.metadata_json;
      }
      stream << "," << shader_metadata_json.gs;
      break;
    case PipelineStateSubobjectType::CS:
      compute.CS = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      has_cs = compute.CS.pShaderBytecode && compute.CS.BytecodeLength > 0;
      {
        const auto shader = shader_asset_metadata_json_locked("cs", compute.CS, shader_blob_refs);
        shader_json.cs = shader.asset_json;
        shader_metadata_json.cs = shader.metadata_json;
      }
      stream << "," << shader_metadata_json.cs;
      break;
    case PipelineStateSubobjectType::AS: {
      const auto shader = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      has_as = shader.pShaderBytecode && shader.BytecodeLength > 0;
      const auto shader_json_fields = shader_asset_metadata_json_locked("as", shader, shader_blob_refs);
      shader_json.as = shader_json_fields.asset_json;
      shader_metadata_json.as = shader_json_fields.metadata_json;
      stream << "," << shader_metadata_json.as;
      break;
    }
    case PipelineStateSubobjectType::MS: {
      const auto shader = *reinterpret_cast<const D3D12_SHADER_BYTECODE *>(subobject);
      has_ms = shader.pShaderBytecode && shader.BytecodeLength > 0;
      const auto shader_json_fields = shader_asset_metadata_json_locked("ms", shader, shader_blob_refs);
      shader_json.ms = shader_json_fields.asset_json;
      shader_metadata_json.ms = shader_json_fields.metadata_json;
      stream << "," << shader_metadata_json.ms;
      break;
    }
    case PipelineStateSubobjectType::StreamOutput:
      graphics.StreamOutput = *reinterpret_cast<const D3D12_STREAM_OUTPUT_DESC *>(subobject);
      stream << ",\"stream_output\":" << stream_output_json(graphics.StreamOutput);
      break;
    case PipelineStateSubobjectType::Blend:
      graphics.BlendState = *reinterpret_cast<const D3D12_BLEND_DESC *>(subobject);
      stream << ",\"blend_state\":" << blend_desc_json(graphics.BlendState);
      break;
    case PipelineStateSubobjectType::SampleMask:
      graphics.SampleMask = *reinterpret_cast<const UINT *>(subobject);
      stream << ",\"sample_mask\":" << graphics.SampleMask;
      break;
    case PipelineStateSubobjectType::Rasterizer:
      graphics.RasterizerState = *reinterpret_cast<const D3D12_RASTERIZER_DESC *>(subobject);
      stream << ",\"rasterizer_state\":" << rasterizer_desc_json(graphics.RasterizerState);
      break;
    case PipelineStateSubobjectType::DepthStencil:
      graphics.DepthStencilState = *reinterpret_cast<const D3D12_DEPTH_STENCIL_DESC *>(subobject);
      stream << ",\"depth_stencil_state\":" << depth_stencil_desc_json(graphics.DepthStencilState);
      break;
    case PipelineStateSubobjectType::DepthStencil1: {
      const auto &depth_stencil = *reinterpret_cast<const DepthStencilDesc1 *>(subobject);
      graphics.DepthStencilState.DepthEnable = depth_stencil.DepthEnable;
      graphics.DepthStencilState.DepthWriteMask = depth_stencil.DepthWriteMask;
      graphics.DepthStencilState.DepthFunc = depth_stencil.DepthFunc;
      graphics.DepthStencilState.StencilEnable = depth_stencil.StencilEnable;
      graphics.DepthStencilState.StencilReadMask = depth_stencil.StencilReadMask;
      graphics.DepthStencilState.StencilWriteMask = depth_stencil.StencilWriteMask;
      graphics.DepthStencilState.FrontFace = depth_stencil.FrontFace;
      graphics.DepthStencilState.BackFace = depth_stencil.BackFace;
      stream << ",\"depth_stencil_state\":" << depth_stencil_desc_json(graphics.DepthStencilState)
             << ",\"depth_bounds_test_enable\":" << (depth_stencil.DepthBoundsTestEnable ? "true" : "false");
      break;
    }
    case PipelineStateSubobjectType::InputLayout:
      graphics.InputLayout = *reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC *>(subobject);
      stream << ",\"input_layout\":" << input_layout_json(graphics.InputLayout);
      break;
    case PipelineStateSubobjectType::IbStripCutValue:
      graphics.IBStripCutValue = *reinterpret_cast<const D3D12_INDEX_BUFFER_STRIP_CUT_VALUE *>(subobject);
      stream << ",\"ib_strip_cut_value\":" << static_cast<unsigned int>(graphics.IBStripCutValue);
      break;
    case PipelineStateSubobjectType::PrimitiveTopology:
      graphics.PrimitiveTopologyType = *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE *>(subobject);
      stream << ",\"primitive_topology_type\":" << static_cast<unsigned int>(graphics.PrimitiveTopologyType);
      break;
    case PipelineStateSubobjectType::RenderTargetFormats: {
      const auto &formats = *reinterpret_cast<const RtFormatArray *>(subobject);
      graphics.NumRenderTargets = formats.NumRenderTargets;
      stream << ",\"num_render_targets\":" << formats.NumRenderTargets << ",\"rtv_formats\":[";
      for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
        if (index) {
          stream << ",";
        }
        const auto format = index < formats.NumRenderTargets ? formats.RTFormats[index] : DXGI_FORMAT_UNKNOWN;
        graphics.RTVFormats[index] = format;
        stream << static_cast<unsigned int>(format);
      }
      stream << "]";
      break;
    }
    case PipelineStateSubobjectType::DepthStencilFormat:
      graphics.DSVFormat = *reinterpret_cast<const DXGI_FORMAT *>(subobject);
      stream << ",\"dsv_format\":" << static_cast<unsigned int>(graphics.DSVFormat);
      break;
    case PipelineStateSubobjectType::SampleDesc:
      graphics.SampleDesc = *reinterpret_cast<const DXGI_SAMPLE_DESC *>(subobject);
      stream << ",\"sample_desc\":{\"count\":" << graphics.SampleDesc.Count
             << ",\"quality\":" << graphics.SampleDesc.Quality << "}";
      break;
    case PipelineStateSubobjectType::NodeMask:
      graphics.NodeMask = *reinterpret_cast<const UINT *>(subobject);
      compute.NodeMask = graphics.NodeMask;
      stream << ",\"node_mask\":" << graphics.NodeMask;
      break;
    case PipelineStateSubobjectType::CachedPso: {
      const auto &cached = *reinterpret_cast<const D3D12_CACHED_PIPELINE_STATE *>(subobject);
      graphics.CachedPSO = cached;
      compute.CachedPSO = cached;
      stream << ",\"cached_pso_size\":" << static_cast<std::uint64_t>(cached.CachedBlobSizeInBytes);
      break;
    }
    case PipelineStateSubobjectType::Flags:
      graphics.Flags = *reinterpret_cast<const D3D12_PIPELINE_STATE_FLAGS *>(subobject);
      compute.Flags = graphics.Flags;
      stream << ",\"flags\":" << static_cast<unsigned int>(graphics.Flags);
      break;
    case PipelineStateSubobjectType::ViewInstancing: {
      const auto &view_instancing = *reinterpret_cast<const ViewInstancingDesc *>(subobject);
      stream << ",\"view_instance_count\":" << view_instancing.ViewInstanceCount;
      break;
    }
    default:
      break;
    }
    stream << "}";
    offset = align_stream_offset(offset + payload_size, sizeof(void *));
  }
  stream << "]}";
  result.metadata_json = stream.str();
  result.compute = has_cs && !has_as && !has_ms;
  result.requires_dxmt_backend = has_ms || has_as;

  std::ostringstream pipeline;
  if (result.compute) {
    pipeline << "{\"pso_raw_version\":1"
             << ",\"pso_kind\":\"compute\""
             << ",\"source\":\"stream\""
             << ",\"stream_size\":" << static_cast<std::uint64_t>(desc->SizeInBytes)
             << ",\"root_signature_object_id\":" << object_id_json(lookup_object_id_locked(compute.pRootSignature))
             << ",\"node_mask\":" << compute.NodeMask
             << ",\"flags\":" << static_cast<unsigned int>(compute.Flags)
             << "," << shader_json.cs
             << ",\"requires_dxmt_backend\":false"
             << ",\"stream_metadata\":" << result.metadata_json
             << "}";
  } else {
    pipeline << "{\"pso_raw_version\":1"
             << ",\"pso_kind\":\"" << (has_ms ? "mesh" : "graphics") << "\""
             << ",\"source\":\"stream\""
             << ",\"stream_size\":" << static_cast<std::uint64_t>(desc->SizeInBytes)
             << ",\"root_signature_object_id\":" << object_id_json(lookup_object_id_locked(graphics.pRootSignature))
             << ",\"node_mask\":" << graphics.NodeMask
             << ",\"flags\":" << static_cast<unsigned int>(graphics.Flags)
             << ",\"input_layout\":" << input_layout_json(graphics.InputLayout)
             << ",\"blend_state\":" << blend_desc_json(graphics.BlendState)
             << ",\"sample_mask\":" << graphics.SampleMask
             << ",\"rasterizer_state\":" << rasterizer_desc_json(graphics.RasterizerState)
             << ",\"depth_stencil_state\":" << depth_stencil_desc_json(graphics.DepthStencilState)
             << ",\"stream_output\":" << stream_output_json(graphics.StreamOutput)
             << ",\"primitive_topology_type\":" << static_cast<unsigned int>(graphics.PrimitiveTopologyType)
             << ",\"num_render_targets\":" << graphics.NumRenderTargets
             << ",\"rtv_formats\":[";
    for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
      if (index) {
        pipeline << ",";
      }
      pipeline << static_cast<unsigned int>(graphics.RTVFormats[index]);
    }
    pipeline << "]"
             << ",\"dsv_format\":" << static_cast<unsigned int>(graphics.DSVFormat)
             << ",\"sample_desc\":{\"count\":" << graphics.SampleDesc.Count
             << ",\"quality\":" << graphics.SampleDesc.Quality << "}"
             << ",\"ib_strip_cut_value\":" << static_cast<unsigned int>(graphics.IBStripCutValue)
             << "," << shader_json.vs
             << "," << shader_json.ps
             << "," << shader_json.ds
             << "," << shader_json.hs
             << "," << shader_json.gs;
    if (has_as) {
      pipeline << "," << shader_json.as;
    }
    if (has_ms) {
      pipeline << "," << shader_json.ms;
    }
    pipeline << ",\"requires_dxmt_backend\":" << (result.requires_dxmt_backend ? "true" : "false")
             << ",\"stream_metadata\":" << result.metadata_json;
    pipeline << "}";
  }
  result.pipeline_json = pipeline.str();
  return result;
}

void patch_device(ID3D12Device *device, std::size_t vtable_size = sizeof(ID3D12DeviceVtbl));
std::size_t supported_device_vtable_size(ID3D12Device *device, std::size_t minimum_size);
void patch_command_queue(ID3D12CommandQueue *queue);
void patch_command_allocator(ID3D12CommandAllocator *allocator);
void patch_command_list(ID3D12GraphicsCommandList *command_list);
void patch_command_list1(ID3D12GraphicsCommandList1 *command_list);
void patch_command_list2(ID3D12GraphicsCommandList2 *command_list);
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

Device2HookState device2_hook_for(ID3D12Device2 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device2_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device2_hooks.end() ? Device2HookState{} : it->second;
}

Device3HookState device3_hook_for(ID3D12Device3 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device3_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device3_hooks.end() ? Device3HookState{} : it->second;
}

Device4HookState device4_hook_for(ID3D12Device4 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device4_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device4_hooks.end() ? Device4HookState{} : it->second;
}

Device6HookState device6_hook_for(ID3D12Device6 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device6_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device6_hooks.end() ? Device6HookState{} : it->second;
}

Device7HookState device7_hook_for(ID3D12Device7 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device7_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device7_hooks.end() ? Device7HookState{} : it->second;
}

Device8HookState device8_hook_for(ID3D12Device8 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device8_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device8_hooks.end() ? Device8HookState{} : it->second;
}

Device9HookState device9_hook_for(ID3D12Device9 *device)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().device9_hooks.find(device ? device->lpVtbl : nullptr);
  return it == capture_state().device9_hooks.end() ? Device9HookState{} : it->second;
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

CommandList1HookState command_list1_hook_for(ID3D12GraphicsCommandList1 *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().command_list1_hooks.find(command_list ? command_list->lpVtbl : nullptr);
  return it == capture_state().command_list1_hooks.end() ? CommandList1HookState{} : it->second;
}

CommandList2HookState command_list2_hook_for(ID3D12GraphicsCommandList2 *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto it = capture_state().command_list2_hooks.find(command_list ? command_list->lpVtbl : nullptr);
  return it == capture_state().command_list2_hooks.end() ? CommandList2HookState{} : it->second;
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
  hook.create_query_heap = original_vtable->CreateQueryHeap;
  hook.create_root_signature = original_vtable->CreateRootSignature;
  hook.create_sampler = original_vtable->CreateSampler;
  hook.copy_descriptors = original_vtable->CopyDescriptors;
  hook.copy_descriptors_simple = original_vtable->CopyDescriptorsSimple;
  hook.create_constant_buffer_view = original_vtable->CreateConstantBufferView;
  hook.create_shader_resource_view = original_vtable->CreateShaderResourceView;
  hook.create_unordered_access_view = original_vtable->CreateUnorderedAccessView;
  hook.create_render_target_view = original_vtable->CreateRenderTargetView;
  hook.create_depth_stencil_view = original_vtable->CreateDepthStencilView;
  hook.create_committed_resource = original_vtable->CreateCommittedResource;
  hook.create_heap = original_vtable->CreateHeap;
  hook.create_placed_resource = original_vtable->CreatePlacedResource;
  hook.create_fence = original_vtable->CreateFence;
  hook.create_command_signature = original_vtable->CreateCommandSignature;
  capture_state().device_hooks.emplace(vtable, hook);
  if (vtable_size >= sizeof(ID3D12Device2Vtbl)) {
    auto *original_vtable2 = reinterpret_cast<ID3D12Device2Vtbl *>(original_vtable);
    auto *vtable2 = reinterpret_cast<ID3D12Device2Vtbl *>(vtable);
    Device2HookState hook2;
    hook2.vtable = original_vtable2;
    hook2.create_pipeline_state = original_vtable2->CreatePipelineState;
    capture_state().device2_hooks.emplace(vtable2, hook2);
    patch_vtable_field(vtable2, &ID3D12Device2Vtbl::CreatePipelineState, hook_device_create_pipeline_state);
  }
  if (vtable_size >= sizeof(ID3D12Device3Vtbl)) {
    auto *original_vtable3 = reinterpret_cast<ID3D12Device3Vtbl *>(original_vtable);
    auto *vtable3 = reinterpret_cast<ID3D12Device3Vtbl *>(vtable);
    Device3HookState hook3;
    hook3.vtable = original_vtable3;
    hook3.open_existing_heap_from_address = original_vtable3->OpenExistingHeapFromAddress;
    capture_state().device3_hooks.emplace(vtable3, hook3);
    patch_vtable_field(vtable3, &ID3D12Device3Vtbl::OpenExistingHeapFromAddress, hook_device_open_existing_heap_from_address);
  }
  if (vtable_size >= sizeof(ID3D12Device4Vtbl)) {
    auto *original_vtable4 = reinterpret_cast<ID3D12Device4Vtbl *>(original_vtable);
    auto *vtable4 = reinterpret_cast<ID3D12Device4Vtbl *>(vtable);
    Device4HookState hook4;
    hook4.vtable = original_vtable4;
    hook4.create_command_list1 = original_vtable4->CreateCommandList1;
    hook4.create_heap1 = original_vtable4->CreateHeap1;
    capture_state().device4_hooks.emplace(vtable4, hook4);
    patch_vtable_field(vtable4, &ID3D12Device4Vtbl::CreateCommandList1, hook_device_create_command_list1);
    patch_vtable_field(vtable4, &ID3D12Device4Vtbl::CreateHeap1, hook_device_create_heap1);
  }
  if (vtable_size >= sizeof(ID3D12Device6Vtbl)) {
    auto *original_vtable6 = reinterpret_cast<ID3D12Device6Vtbl *>(original_vtable);
    auto *vtable6 = reinterpret_cast<ID3D12Device6Vtbl *>(vtable);
    Device6HookState hook6;
    hook6.vtable = original_vtable6;
    hook6.set_background_processing_mode = original_vtable6->SetBackgroundProcessingMode;
    capture_state().device6_hooks.emplace(vtable6, hook6);
    patch_vtable_field(vtable6, &ID3D12Device6Vtbl::SetBackgroundProcessingMode, hook_device_set_background_processing_mode);
  }
  if (vtable_size >= sizeof(ID3D12Device7Vtbl)) {
    auto *original_vtable7 = reinterpret_cast<ID3D12Device7Vtbl *>(original_vtable);
    auto *vtable7 = reinterpret_cast<ID3D12Device7Vtbl *>(vtable);
    Device7HookState hook7;
    hook7.vtable = original_vtable7;
    hook7.add_to_state_object = original_vtable7->AddToStateObject;
    hook7.create_protected_resource_session1 = original_vtable7->CreateProtectedResourceSession1;
    capture_state().device7_hooks.emplace(vtable7, hook7);
    patch_vtable_field(vtable7, &ID3D12Device7Vtbl::AddToStateObject, hook_device_add_to_state_object);
    patch_vtable_field(vtable7, &ID3D12Device7Vtbl::CreateProtectedResourceSession1, hook_device_create_protected_resource_session1);
  }
  if (vtable_size >= sizeof(ID3D12Device8Vtbl)) {
    auto *original_vtable8 = reinterpret_cast<ID3D12Device8Vtbl *>(original_vtable);
    auto *vtable8 = reinterpret_cast<ID3D12Device8Vtbl *>(vtable);
    Device8HookState hook8;
    hook8.vtable = original_vtable8;
    hook8.create_committed_resource2 = original_vtable8->CreateCommittedResource2;
    hook8.create_placed_resource1 = original_vtable8->CreatePlacedResource1;
    capture_state().device8_hooks.emplace(vtable8, hook8);
    patch_vtable_field(vtable8, &ID3D12Device8Vtbl::CreateCommittedResource2, hook_device_create_committed_resource2);
    patch_vtable_field(vtable8, &ID3D12Device8Vtbl::CreatePlacedResource1, hook_device_create_placed_resource1);
  }
  if (vtable_size >= sizeof(ID3D12Device9Vtbl)) {
    auto *original_vtable9 = reinterpret_cast<ID3D12Device9Vtbl *>(original_vtable);
    auto *vtable9 = reinterpret_cast<ID3D12Device9Vtbl *>(vtable);
    Device9HookState hook9;
    hook9.vtable = original_vtable9;
    hook9.create_shader_cache_session = original_vtable9->CreateShaderCacheSession;
    hook9.shader_cache_control = original_vtable9->ShaderCacheControl;
    hook9.create_command_queue1 = original_vtable9->CreateCommandQueue1;
    capture_state().device9_hooks.emplace(vtable9, hook9);
    patch_vtable_field(vtable9, &ID3D12Device9Vtbl::CreateShaderCacheSession, hook_device_create_shader_cache_session);
    patch_vtable_field(vtable9, &ID3D12Device9Vtbl::ShaderCacheControl, hook_device_shader_cache_control);
    patch_vtable_field(vtable9, &ID3D12Device9Vtbl::CreateCommandQueue1, hook_device_create_command_queue1);
  }
  device->lpVtbl = vtable;

  patch_vtable_field(vtable, &ID3D12DeviceVtbl::QueryInterface, hook_device_query_interface);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandQueue, hook_device_create_command_queue);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandAllocator, hook_device_create_command_allocator);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateGraphicsPipelineState, hook_device_create_graphics_pipeline_state);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateComputePipelineState, hook_device_create_compute_pipeline_state);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandList, hook_device_create_command_list);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateDescriptorHeap, hook_device_create_descriptor_heap);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateQueryHeap, hook_device_create_query_heap);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateRootSignature, hook_device_create_root_signature);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateSampler, hook_device_create_sampler);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CopyDescriptors, hook_device_copy_descriptors);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CopyDescriptorsSimple, hook_device_copy_descriptors_simple);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateConstantBufferView, hook_device_create_constant_buffer_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateShaderResourceView, hook_device_create_shader_resource_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateUnorderedAccessView, hook_device_create_unordered_access_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateRenderTargetView, hook_device_create_render_target_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateDepthStencilView, hook_device_create_depth_stencil_view);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommittedResource, hook_device_create_committed_resource);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateHeap, hook_device_create_heap);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreatePlacedResource, hook_device_create_placed_resource);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateFence, hook_device_create_fence);
  patch_vtable_field(vtable, &ID3D12DeviceVtbl::CreateCommandSignature, hook_device_create_command_signature);
  proxy_debug_logf(
      "patch_device vtable=%p pso=%d queue=%d allocator=%d list=%d list1=%d heap1=%d fence=%d",
      static_cast<void *>(vtable),
      vtable->CreateGraphicsPipelineState == hook_device_create_graphics_pipeline_state,
      vtable->CreateCommandQueue == hook_device_create_command_queue,
      vtable->CreateCommandAllocator == hook_device_create_command_allocator,
      vtable->CreateCommandList == hook_device_create_command_list,
      vtable_size >= sizeof(ID3D12Device2Vtbl)
          ? reinterpret_cast<ID3D12Device2Vtbl *>(vtable)->CreatePipelineState == hook_device_create_pipeline_state
          : false,
      vtable_size >= sizeof(ID3D12Device4Vtbl)
          ? reinterpret_cast<ID3D12Device4Vtbl *>(vtable)->CreateCommandList1 == hook_device_create_command_list1
          : false,
      vtable_size >= sizeof(ID3D12Device4Vtbl)
          ? reinterpret_cast<ID3D12Device4Vtbl *>(vtable)->CreateHeap1 == hook_device_create_heap1
          : false,
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
  hook.clear_state = original_vtable->ClearState;
  hook.set_pipeline_state = original_vtable->SetPipelineState;
  hook.om_set_blend_factor = original_vtable->OMSetBlendFactor;
  hook.om_set_stencil_ref = original_vtable->OMSetStencilRef;
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
  hook.clear_unordered_access_view_uint = original_vtable->ClearUnorderedAccessViewUint;
  hook.clear_unordered_access_view_float = original_vtable->ClearUnorderedAccessViewFloat;
  hook.discard_resource = original_vtable->DiscardResource;
  hook.ia_set_primitive_topology = original_vtable->IASetPrimitiveTopology;
  hook.ia_set_vertex_buffers = original_vtable->IASetVertexBuffers;
  hook.ia_set_index_buffer = original_vtable->IASetIndexBuffer;
  hook.resource_barrier = original_vtable->ResourceBarrier;
  hook.set_descriptor_heaps = original_vtable->SetDescriptorHeaps;
  hook.draw_instanced = original_vtable->DrawInstanced;
  hook.draw_indexed_instanced = original_vtable->DrawIndexedInstanced;
  hook.dispatch = original_vtable->Dispatch;
  hook.execute_indirect = original_vtable->ExecuteIndirect;
  hook.execute_bundle = original_vtable->ExecuteBundle;
  hook.copy_buffer_region = original_vtable->CopyBufferRegion;
  hook.copy_texture_region = original_vtable->CopyTextureRegion;
  hook.copy_resource = original_vtable->CopyResource;
  hook.resolve_subresource = original_vtable->ResolveSubresource;
  hook.begin_query = original_vtable->BeginQuery;
  hook.end_query = original_vtable->EndQuery;
  hook.resolve_query_data = original_vtable->ResolveQueryData;
  hook.set_predication = original_vtable->SetPredication;
  capture_state().command_list_hooks.emplace(vtable, hook);
  command_list->lpVtbl = vtable;
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::QueryInterface, hook_command_list_query_interface);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Close, hook_command_list_close);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Reset, hook_command_list_reset);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ClearState, hook_command_list_clear_state);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetPipelineState, hook_command_list_set_pipeline_state);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::OMSetBlendFactor, hook_command_list_om_set_blend_factor);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::OMSetStencilRef, hook_command_list_om_set_stencil_ref);
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
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ClearUnorderedAccessViewUint, hook_command_list_clear_unordered_access_view_uint);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ClearUnorderedAccessViewFloat, hook_command_list_clear_unordered_access_view_float);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DiscardResource, hook_command_list_discard_resource);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::IASetPrimitiveTopology, hook_command_list_ia_set_primitive_topology);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::IASetVertexBuffers, hook_command_list_ia_set_vertex_buffers);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::IASetIndexBuffer, hook_command_list_ia_set_index_buffer);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ResourceBarrier, hook_command_list_resource_barrier);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetDescriptorHeaps, hook_command_list_set_descriptor_heaps);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DrawInstanced, hook_command_list_draw_instanced);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::DrawIndexedInstanced, hook_command_list_draw_indexed_instanced);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::Dispatch, hook_command_list_dispatch);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ExecuteIndirect, hook_command_list_execute_indirect);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ExecuteBundle, hook_command_list_execute_bundle);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::CopyBufferRegion, hook_command_list_copy_buffer_region);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::CopyTextureRegion, hook_command_list_copy_texture_region);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::CopyResource, hook_command_list_copy_resource);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ResolveSubresource, hook_command_list_resolve_subresource);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::BeginQuery, hook_command_list_begin_query);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::EndQuery, hook_command_list_end_query);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::ResolveQueryData, hook_command_list_resolve_query_data);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandListVtbl::SetPredication, hook_command_list_set_predication);
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
CommandList1HookState make_command_list1_hook_state(VTable *original_vtable)
{
  CommandList1HookState hook;
  hook.vtable = reinterpret_cast<ID3D12GraphicsCommandList1Vtbl *>(original_vtable);
  hook.resolve_subresource_region =
      reinterpret_cast<decltype(hook.resolve_subresource_region)>(original_vtable->ResolveSubresourceRegion);
  return hook;
}

template <typename VTable>
CommandList2HookState make_command_list2_hook_state(VTable *original_vtable)
{
  CommandList2HookState hook;
  hook.vtable = reinterpret_cast<ID3D12GraphicsCommandList2Vtbl *>(original_vtable);
  hook.write_buffer_immediate =
      reinterpret_cast<decltype(hook.write_buffer_immediate)>(original_vtable->WriteBufferImmediate);
  return hook;
}

template <typename VTable>
CommandListHookState make_command_list_base_hook_state(VTable *original_vtable)
{
  CommandListHookState hook;
  hook.vtable = reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(original_vtable);
  hook.query_interface = reinterpret_cast<decltype(hook.query_interface)>(original_vtable->QueryInterface);
  hook.close = reinterpret_cast<decltype(hook.close)>(original_vtable->Close);
  hook.reset = reinterpret_cast<decltype(hook.reset)>(original_vtable->Reset);
  hook.clear_state = reinterpret_cast<decltype(hook.clear_state)>(original_vtable->ClearState);
  hook.set_pipeline_state = reinterpret_cast<decltype(hook.set_pipeline_state)>(original_vtable->SetPipelineState);
  hook.om_set_blend_factor = reinterpret_cast<decltype(hook.om_set_blend_factor)>(original_vtable->OMSetBlendFactor);
  hook.om_set_stencil_ref = reinterpret_cast<decltype(hook.om_set_stencil_ref)>(original_vtable->OMSetStencilRef);
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
  hook.clear_unordered_access_view_uint =
      reinterpret_cast<decltype(hook.clear_unordered_access_view_uint)>(original_vtable->ClearUnorderedAccessViewUint);
  hook.clear_unordered_access_view_float =
      reinterpret_cast<decltype(hook.clear_unordered_access_view_float)>(original_vtable->ClearUnorderedAccessViewFloat);
  hook.discard_resource = reinterpret_cast<decltype(hook.discard_resource)>(original_vtable->DiscardResource);
  hook.ia_set_primitive_topology = reinterpret_cast<decltype(hook.ia_set_primitive_topology)>(original_vtable->IASetPrimitiveTopology);
  hook.ia_set_vertex_buffers = reinterpret_cast<decltype(hook.ia_set_vertex_buffers)>(original_vtable->IASetVertexBuffers);
  hook.ia_set_index_buffer = reinterpret_cast<decltype(hook.ia_set_index_buffer)>(original_vtable->IASetIndexBuffer);
  hook.resource_barrier = reinterpret_cast<decltype(hook.resource_barrier)>(original_vtable->ResourceBarrier);
  hook.set_descriptor_heaps = reinterpret_cast<decltype(hook.set_descriptor_heaps)>(original_vtable->SetDescriptorHeaps);
  hook.draw_instanced = reinterpret_cast<decltype(hook.draw_instanced)>(original_vtable->DrawInstanced);
  hook.draw_indexed_instanced = reinterpret_cast<decltype(hook.draw_indexed_instanced)>(original_vtable->DrawIndexedInstanced);
  hook.dispatch = reinterpret_cast<decltype(hook.dispatch)>(original_vtable->Dispatch);
  hook.execute_indirect = reinterpret_cast<decltype(hook.execute_indirect)>(original_vtable->ExecuteIndirect);
  hook.execute_bundle = reinterpret_cast<decltype(hook.execute_bundle)>(original_vtable->ExecuteBundle);
  hook.copy_buffer_region = reinterpret_cast<decltype(hook.copy_buffer_region)>(original_vtable->CopyBufferRegion);
  hook.copy_texture_region = reinterpret_cast<decltype(hook.copy_texture_region)>(original_vtable->CopyTextureRegion);
  hook.copy_resource = reinterpret_cast<decltype(hook.copy_resource)>(original_vtable->CopyResource);
  hook.resolve_subresource = reinterpret_cast<decltype(hook.resolve_subresource)>(original_vtable->ResolveSubresource);
  hook.begin_query = reinterpret_cast<decltype(hook.begin_query)>(original_vtable->BeginQuery);
  hook.end_query = reinterpret_cast<decltype(hook.end_query)>(original_vtable->EndQuery);
  hook.resolve_query_data = reinterpret_cast<decltype(hook.resolve_query_data)>(original_vtable->ResolveQueryData);
  hook.set_predication = reinterpret_cast<decltype(hook.set_predication)>(original_vtable->SetPredication);
  return hook;
}

template <typename VTable>
void patch_command_list_base_methods(VTable *vtable)
{
  patch_vtable_field_cast(vtable, &VTable::QueryInterface, hook_command_list_query_interface);
  patch_vtable_field_cast(vtable, &VTable::Close, hook_command_list_close);
  patch_vtable_field_cast(vtable, &VTable::Reset, hook_command_list_reset);
  patch_vtable_field_cast(vtable, &VTable::ClearState, hook_command_list_clear_state);
  patch_vtable_field_cast(vtable, &VTable::SetPipelineState, hook_command_list_set_pipeline_state);
  patch_vtable_field_cast(vtable, &VTable::OMSetBlendFactor, hook_command_list_om_set_blend_factor);
  patch_vtable_field_cast(vtable, &VTable::OMSetStencilRef, hook_command_list_om_set_stencil_ref);
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
  patch_vtable_field_cast(vtable, &VTable::ClearUnorderedAccessViewUint, hook_command_list_clear_unordered_access_view_uint);
  patch_vtable_field_cast(vtable, &VTable::ClearUnorderedAccessViewFloat, hook_command_list_clear_unordered_access_view_float);
  patch_vtable_field_cast(vtable, &VTable::DiscardResource, hook_command_list_discard_resource);
  patch_vtable_field_cast(vtable, &VTable::IASetPrimitiveTopology, hook_command_list_ia_set_primitive_topology);
  patch_vtable_field_cast(vtable, &VTable::IASetVertexBuffers, hook_command_list_ia_set_vertex_buffers);
  patch_vtable_field_cast(vtable, &VTable::IASetIndexBuffer, hook_command_list_ia_set_index_buffer);
  patch_vtable_field_cast(vtable, &VTable::ResourceBarrier, hook_command_list_resource_barrier);
  patch_vtable_field_cast(vtable, &VTable::SetDescriptorHeaps, hook_command_list_set_descriptor_heaps);
  patch_vtable_field_cast(vtable, &VTable::DrawInstanced, hook_command_list_draw_instanced);
  patch_vtable_field_cast(vtable, &VTable::DrawIndexedInstanced, hook_command_list_draw_indexed_instanced);
  patch_vtable_field_cast(vtable, &VTable::Dispatch, hook_command_list_dispatch);
  patch_vtable_field_cast(vtable, &VTable::ExecuteIndirect, hook_command_list_execute_indirect);
  patch_vtable_field_cast(vtable, &VTable::ExecuteBundle, hook_command_list_execute_bundle);
  patch_vtable_field_cast(vtable, &VTable::CopyBufferRegion, hook_command_list_copy_buffer_region);
  patch_vtable_field_cast(vtable, &VTable::CopyTextureRegion, hook_command_list_copy_texture_region);
  patch_vtable_field_cast(vtable, &VTable::CopyResource, hook_command_list_copy_resource);
  patch_vtable_field_cast(vtable, &VTable::ResolveSubresource, hook_command_list_resolve_subresource);
  patch_vtable_field_cast(vtable, &VTable::BeginQuery, hook_command_list_begin_query);
  patch_vtable_field_cast(vtable, &VTable::EndQuery, hook_command_list_end_query);
  patch_vtable_field_cast(vtable, &VTable::ResolveQueryData, hook_command_list_resolve_query_data);
  patch_vtable_field_cast(vtable, &VTable::SetPredication, hook_command_list_set_predication);
}

template <typename VTable>
void patch_command_list1_methods(VTable *vtable)
{
  patch_vtable_field_cast(vtable, &VTable::ResolveSubresourceRegion, hook_command_list_resolve_subresource_region);
}

template <typename VTable>
void patch_command_list2_methods(VTable *vtable)
{
  patch_vtable_field_cast(vtable, &VTable::WriteBufferImmediate, hook_command_list_write_buffer_immediate);
}

void patch_command_list1(ID3D12GraphicsCommandList1 *command_list)
{
  if (!command_list || !command_list->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = command_list->lpVtbl;
  if (capture_state().command_list1_hooks.find(original_vtable) != capture_state().command_list1_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_list1: failed to clone vtable");
    return;
  }
  capture_state().command_list1_hooks.emplace(vtable, make_command_list1_hook_state(original_vtable));
  capture_state().command_list_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(vtable),
      make_command_list_base_hook_state(original_vtable));
  command_list->lpVtbl = vtable;
  patch_command_list_base_methods(vtable);
  patch_command_list1_methods(vtable);
  proxy_debug_logf(
      "patch_list1 vtable=%p heaps=%d resolve_region=%d",
      static_cast<void *>(vtable),
      reinterpret_cast<void *>(vtable->SetDescriptorHeaps) == reinterpret_cast<void *>(hook_command_list_set_descriptor_heaps),
      reinterpret_cast<void *>(vtable->ResolveSubresourceRegion) == reinterpret_cast<void *>(hook_command_list_resolve_subresource_region));
}

void patch_command_list2(ID3D12GraphicsCommandList2 *command_list)
{
  if (!command_list || !command_list->lpVtbl) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  auto *original_vtable = command_list->lpVtbl;
  if (capture_state().command_list2_hooks.find(original_vtable) != capture_state().command_list2_hooks.end()) {
    return;
  }
  auto *vtable = clone_vtable(original_vtable);
  if (!vtable) {
    proxy_debug_log("patch_command_list2: failed to clone vtable");
    return;
  }
  capture_state().command_list2_hooks.emplace(vtable, make_command_list2_hook_state(original_vtable));
  capture_state().command_list1_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandList1Vtbl *>(vtable),
      make_command_list1_hook_state(original_vtable));
  capture_state().command_list_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(vtable),
      make_command_list_base_hook_state(original_vtable));
  command_list->lpVtbl = vtable;
  patch_command_list_base_methods(vtable);
  patch_command_list1_methods(vtable);
  patch_command_list2_methods(vtable);
  proxy_debug_logf(
      "patch_list2 vtable=%p heaps=%d resolve_region=%d wbi=%d",
      static_cast<void *>(vtable),
      reinterpret_cast<void *>(vtable->SetDescriptorHeaps) == reinterpret_cast<void *>(hook_command_list_set_descriptor_heaps),
      reinterpret_cast<void *>(vtable->ResolveSubresourceRegion) == reinterpret_cast<void *>(hook_command_list_resolve_subresource_region),
      reinterpret_cast<void *>(vtable->WriteBufferImmediate) == reinterpret_cast<void *>(hook_command_list_write_buffer_immediate));
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
  hook.begin_render_pass = original_vtable->BeginRenderPass;
  hook.end_render_pass = original_vtable->EndRenderPass;
  hook.dispatch_rays = original_vtable->DispatchRays;
  capture_state().command_list1_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandList1Vtbl *>(vtable),
      make_command_list1_hook_state(original_vtable));
  capture_state().command_list2_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandList2Vtbl *>(vtable),
      make_command_list2_hook_state(original_vtable));
  capture_state().command_list4_hooks.emplace(vtable, hook);
  capture_state().command_list_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(vtable),
      make_command_list_base_hook_state(original_vtable));
  command_list->lpVtbl = vtable;
  patch_command_list_base_methods(vtable);
  patch_command_list1_methods(vtable);
  patch_command_list2_methods(vtable);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList4Vtbl::BeginRenderPass, hook_command_list_begin_render_pass);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList4Vtbl::EndRenderPass, hook_command_list_end_render_pass);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList4Vtbl::DispatchRays, hook_command_list_dispatch_rays);
  proxy_debug_logf(
      "patch_list4 vtable=%p heaps=%d resolve_region=%d wbi=%d begin_rp=%d end_rp=%d dispatch_rays=%d",
      static_cast<void *>(vtable),
      reinterpret_cast<void *>(vtable->SetDescriptorHeaps) == reinterpret_cast<void *>(hook_command_list_set_descriptor_heaps),
      reinterpret_cast<void *>(vtable->ResolveSubresourceRegion) == reinterpret_cast<void *>(hook_command_list_resolve_subresource_region),
      reinterpret_cast<void *>(vtable->WriteBufferImmediate) == reinterpret_cast<void *>(hook_command_list_write_buffer_immediate),
      vtable->BeginRenderPass == hook_command_list_begin_render_pass,
      vtable->EndRenderPass == hook_command_list_end_render_pass,
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
  capture_state().command_list1_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandList1Vtbl *>(vtable),
      make_command_list1_hook_state(original_vtable));
  capture_state().command_list2_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandList2Vtbl *>(vtable),
      make_command_list2_hook_state(original_vtable));
  capture_state().command_list6_hooks.emplace(vtable, hook);
  capture_state().command_list_hooks.emplace(
      reinterpret_cast<ID3D12GraphicsCommandListVtbl *>(vtable),
      make_command_list_base_hook_state(original_vtable));
  command_list->lpVtbl = vtable;
  patch_command_list_base_methods(vtable);
  patch_command_list1_methods(vtable);
  patch_command_list2_methods(vtable);
  patch_vtable_field(vtable, &ID3D12GraphicsCommandList6Vtbl::DispatchMesh, hook_command_list_dispatch_mesh);
  proxy_debug_logf(
      "patch_list6 vtable=%p heaps=%d resolve_region=%d wbi=%d dispatch_mesh=%d",
      static_cast<void *>(vtable),
      reinterpret_cast<void *>(vtable->SetDescriptorHeaps) == reinterpret_cast<void *>(hook_command_list_set_descriptor_heaps),
      reinterpret_cast<void *>(vtable->ResolveSubresourceRegion) == reinterpret_cast<void *>(hook_command_list_resolve_subresource_region),
      reinterpret_cast<void *>(vtable->WriteBufferImmediate) == reinterpret_cast<void *>(hook_command_list_write_buffer_immediate),
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
    patch_device(device, supported_device_vtable_size(device, sizeof(ID3D12DeviceVtbl)));
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
         IsEqualGUID(riid, kIidD3D12Device5) ||
         IsEqualGUID(riid, kIidD3D12Device6) ||
         IsEqualGUID(riid, kIidD3D12Device7) ||
         IsEqualGUID(riid, kIidD3D12Device8) ||
         IsEqualGUID(riid, kIidD3D12Device9);
}

std::size_t device_vtable_size_for_iid(REFIID riid)
{
  if (IsEqualGUID(riid, kIidD3D12Device9)) {
    return sizeof(ID3D12Device9Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device8)) {
    return sizeof(ID3D12Device8Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device7)) {
    return sizeof(ID3D12Device7Vtbl);
  }
  if (IsEqualGUID(riid, kIidD3D12Device6)) {
    return sizeof(ID3D12Device6Vtbl);
  }
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

void release_iunknown(void *object)
{
  if (!object) {
    return;
  }
  auto *unknown = static_cast<IUnknown *>(object);
  if (unknown->lpVtbl && unknown->lpVtbl->Release) {
    unknown->lpVtbl->Release(unknown);
  }
}

void *query_interface_suppressed(IUnknown *object, REFIID riid)
{
  if (!object || !object->lpVtbl || !object->lpVtbl->QueryInterface) {
    return nullptr;
  }

  void *queried = nullptr;
  {
    ScopedCaptureSuppression suppress_capture;
    if (FAILED(object->lpVtbl->QueryInterface(object, riid, &queried))) {
      return nullptr;
    }
  }
  return queried;
}

ID3D12GraphicsCommandList *query_graphics_command_list(ID3D12CommandList *command_list)
{
  return static_cast<ID3D12GraphicsCommandList *>(
      query_interface_suppressed(reinterpret_cast<IUnknown *>(command_list), kIidD3D12GraphicsCommandList));
}

void *query_iunknown_identity(void *object)
{
  return query_interface_suppressed(static_cast<IUnknown *>(object), kIidIUnknown);
}

ID3D12Fence *query_fence(ID3D12Fence *fence)
{
  return static_cast<ID3D12Fence *>(query_interface_suppressed(reinterpret_cast<IUnknown *>(fence), kIidD3D12Fence));
}

std::size_t supported_device_vtable_size(ID3D12Device *device, std::size_t minimum_size)
{
  if (!device || !device->lpVtbl || !device->lpVtbl->QueryInterface) {
    return minimum_size;
  }

  const auto hook = device_hook_for(device);
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(device, hook.vtable);

  void *queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device9, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device9Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device8, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device8Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device7, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device7Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device6, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device6Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device5, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device5Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device4, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device4Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device3, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device3Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device2, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device2Vtbl));
  }

  queried = nullptr;
  if (SUCCEEDED(device->lpVtbl->QueryInterface(device, kIidD3D12Device1, &queried)) && queried) {
    release_iunknown(queried);
    return std::max(minimum_size, sizeof(ID3D12Device1Vtbl));
  }

  return minimum_size;
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
      auto *device = static_cast<ID3D12Device *>(*object);
      patch_device(device, supported_device_vtable_size(device, device_vtable_size_for_iid(riid)));
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
    const auto payload = graphics_pipeline_asset_json_locked(desc, shader_blob_refs);
    std::vector<apitrace::trace::BlobId> blob_refs;
    blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
    record_call_locked(
        "ID3D12Device::CreateGraphicsPipelineState",
        hr,
        {self, desc ? desc->pRootSignature : nullptr, *pipeline_state},
        blob_refs,
        payload);
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
    const auto payload = compute_pipeline_asset_json_locked(desc, shader_blob_refs);
    std::vector<apitrace::trace::BlobId> blob_refs;
    blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
    record_call_locked(
        "ID3D12Device::CreateComputePipelineState",
        hr,
        {self, desc ? desc->pRootSignature : nullptr, *pipeline_state},
        blob_refs,
        payload);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_pipeline_state(
    ID3D12Device2 *self,
    const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
    REFIID riid,
    void **pipeline_state)
{
  const auto hook = device2_hook_for(self);
  if (!hook.create_pipeline_state) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device2, ID3D12Device2Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_pipeline_state(self, desc, riid, pipeline_state);
  if (SUCCEEDED(hr) && pipeline_state && *pipeline_state) {
    proxy_debug_logf("hook_device_create_pipeline_state self=%p pso=%p", self, *pipeline_state);
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_fresh_object_locked(*pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
    std::vector<apitrace::trace::BlobId> shader_blob_refs;
    const auto stream_asset = stream_pipeline_asset_json_locked(desc, shader_blob_refs);
    std::vector<apitrace::trace::BlobId> blob_refs;
    blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
    record_call_locked("ID3D12Device2::CreatePipelineState", hr, {self, *pipeline_state}, blob_refs, stream_asset.pipeline_json);
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
    void *identity = query_iunknown_identity(*command_list);
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_fresh_object_locked(*command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", parent);
      remember_object_alias_locked(identity, *command_list);
      std::ostringstream payload;
      payload << "{\"node_mask\":" << node_mask << ",\"type\":" << static_cast<unsigned int>(type) << "}";
      record_call_locked("ID3D12Device::CreateCommandList", hr, {self, command_allocator, initial_pipeline_state, *command_list}, {}, payload.str());
    }
    release_iunknown(identity);
    patch_command_list(static_cast<ID3D12GraphicsCommandList *>(*command_list));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_list1(
    ID3D12Device4 *self,
    UINT node_mask,
    D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid,
    void **command_list)
{
  const auto hook = device4_hook_for(self);
  if (!hook.create_command_list1) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device4, ID3D12Device4Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_command_list1(self, node_mask, type, flags, riid, command_list);
  if (SUCCEEDED(hr) && command_list && *command_list) {
    proxy_debug_logf("hook_device_create_command_list1 self=%p list=%p", self, *command_list);
    void *identity = query_iunknown_identity(*command_list);
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_fresh_object_locked(*command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", parent);
      remember_object_alias_locked(identity, *command_list);
      std::ostringstream payload;
      payload << "{\"node_mask\":" << node_mask
              << ",\"type\":" << static_cast<unsigned int>(type)
              << ",\"flags\":" << static_cast<unsigned int>(flags) << "}";
      record_call_locked("ID3D12Device::CreateCommandList1", hr, {self, *command_list}, {}, payload.str());
    }
    release_iunknown(identity);
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

HRESULT STDMETHODCALLTYPE hook_device_create_query_heap(
    ID3D12Device *self,
    const D3D12_QUERY_HEAP_DESC *desc,
    REFIID riid,
    void **query_heap)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_query_heap) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_query_heap(self, desc, riid, query_heap);
  if (SUCCEEDED(hr) && query_heap && *query_heap) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_fresh_object_locked(*query_heap, apitrace::trace::ObjectKind::QueryHeap, "ID3D12QueryHeap", parent);
    std::ostringstream payload;
    payload << "{"
            << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
            << "\"count\":" << (desc ? desc->Count : 0) << ","
            << "\"node_mask\":" << (desc ? desc->NodeMask : 0)
            << "}";
    record_call_locked("ID3D12Device::CreateQueryHeap", hr, {self, *query_heap}, {}, payload.str());
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
      if (asset.blob_id != 0 && !asset.relative_path.empty()) {
        blob_refs.push_back(asset.blob_id);
        root_sig_path = asset.relative_path.generic_string();
      }
    }
    std::ostringstream payload;
    payload << "{\"node_mask\":" << node_mask << ",\"bytecode_size\":" << static_cast<std::uint64_t>(bytecode_length);
    if (!root_sig_path.empty()) {
      payload << ",\"root_signature_path\":\"" << root_sig_path << "\"";
    }
    std::string descriptor_tables_json;
    std::string root_parameters_json;
    if (root_signature_descriptor_tables_json_from_bytecode(bytecode, bytecode_length, descriptor_tables_json, root_parameters_json)) {
      payload << ",\"descriptor_tables\":" << descriptor_tables_json;
      payload << ",\"root_parameters\":" << root_parameters_json;
    }
    payload << "}";
    record_call_locked("ID3D12Device::CreateRootSignature", hr, {self, *root_signature}, blob_refs, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_heap1(
    ID3D12Device4 *self,
    const D3D12_HEAP_DESC *desc,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **heap)
{
  const auto hook = device4_hook_for(self);
  if (!hook.create_heap1) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device4, ID3D12Device4Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_heap1(self, desc, protected_session, riid, heap);
  if (SUCCEEDED(hr) && heap && *heap) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    const auto protected_session_object_id = lookup_object_id_locked(protected_session);
    register_fresh_object_locked(*heap, apitrace::trace::ObjectKind::Heap, "ID3D12Heap", parent);
    capture_state().heap_types[static_cast<ID3D12Heap *>(*heap)] =
        desc ? static_cast<UINT>(desc->Properties.Type) : 0;
    std::ostringstream payload;
    payload << "{\"size_in_bytes\":" << (desc ? desc->SizeInBytes : 0)
            << ",\"alignment\":" << (desc ? desc->Alignment : 0)
            << ",\"heap_type\":" << (desc ? static_cast<unsigned int>(desc->Properties.Type) : 0)
            << ",\"cpu_page_property\":" << (desc ? static_cast<unsigned int>(desc->Properties.CPUPageProperty) : 0)
            << ",\"memory_pool_preference\":" << (desc ? static_cast<unsigned int>(desc->Properties.MemoryPoolPreference) : 0)
            << ",\"creation_node_mask\":" << (desc ? desc->Properties.CreationNodeMask : 0)
            << ",\"visible_node_mask\":" << (desc ? desc->Properties.VisibleNodeMask : 0)
            << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
            << ",\"protected_session_object_id\":" << object_id_json(protected_session_object_id)
            << "}";
    record_call_locked("ID3D12Device4::CreateHeap1", hr, {self, *heap, protected_session}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_open_existing_heap_from_address(
    ID3D12Device3 *self,
    const void *address,
    REFIID riid,
    void **heap)
{
  const auto hook = device3_hook_for(self);
  if (!hook.open_existing_heap_from_address) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device3, ID3D12Device3Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.open_existing_heap_from_address(self, address, riid, heap);
  if (SUCCEEDED(hr) && heap && *heap) {
    ID3D12Heap *created_heap = nullptr;
    auto *unknown = static_cast<IUnknown *>(*heap);
    if (!unknown->lpVtbl || FAILED(unknown->lpVtbl->QueryInterface(unknown, kIidD3D12Heap, reinterpret_cast<void **>(&created_heap))) || !created_heap) {
      return hr;
    }
    const D3D12_HEAP_DESC desc = heap_desc(created_heap);
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_fresh_object_locked(created_heap, apitrace::trace::ObjectKind::Heap, "ID3D12Heap", parent);
      remember_object_alias_locked(*heap, created_heap);
      capture_state().heap_types[created_heap] = static_cast<UINT>(desc.Properties.Type);
      capture_state().heap_types[static_cast<ID3D12Heap *>(*heap)] = static_cast<UINT>(desc.Properties.Type);
      std::ostringstream payload;
      payload << "{\"size_in_bytes\":" << desc.SizeInBytes
              << ",\"alignment\":" << desc.Alignment
              << ",\"heap_type\":" << static_cast<unsigned int>(desc.Properties.Type)
              << ",\"cpu_page_property\":" << static_cast<unsigned int>(desc.Properties.CPUPageProperty)
              << ",\"memory_pool_preference\":" << static_cast<unsigned int>(desc.Properties.MemoryPoolPreference)
              << ",\"creation_node_mask\":" << desc.Properties.CreationNodeMask
              << ",\"visible_node_mask\":" << desc.Properties.VisibleNodeMask
              << ",\"flags\":" << static_cast<unsigned int>(desc.Flags)
              << "}";
      record_call_locked("ID3D12Device3::OpenExistingHeapFromAddress", hr, {self, created_heap}, {}, payload.str());
    }
    created_heap->lpVtbl->Release(created_heap);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_set_background_processing_mode(
    ID3D12Device6 *self,
    D3D12_BACKGROUND_PROCESSING_MODE mode,
    D3D12_MEASUREMENTS_ACTION action,
    HANDLE event,
    BOOL *further_measurements_desired)
{
  const auto hook = device6_hook_for(self);
  if (!hook.set_background_processing_mode) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device6, ID3D12Device6Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.set_background_processing_mode(self, mode, action, event, further_measurements_desired);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"mode\":" << static_cast<unsigned int>(mode)
          << ",\"action\":" << static_cast<unsigned int>(action)
          << ",\"further_measurements_desired\":"
          << (further_measurements_desired ? (*further_measurements_desired ? "true" : "false") : "null")
          << "}";
  record_call_locked("ID3D12Device6::SetBackgroundProcessingMode", hr, {self}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_add_to_state_object(
    ID3D12Device7 *self,
    const D3D12_STATE_OBJECT_DESC *addition,
    ID3D12StateObject *state_object_to_grow_from,
    REFIID riid,
    void **new_state_object)
{
  const auto hook = device7_hook_for(self);
  if (!hook.add_to_state_object) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device7, ID3D12Device7Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.add_to_state_object(self, addition, state_object_to_grow_from, riid, new_state_object);
  if (SUCCEEDED(hr) && new_state_object && *new_state_object) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    const auto source_id = lookup_object_id_locked(state_object_to_grow_from);
    if (source_id != 0) {
      remember_object_alias_locked(*new_state_object, state_object_to_grow_from);
    }
    if (lookup_object_id_locked(*new_state_object) == 0) {
      register_fresh_object_locked(*new_state_object, apitrace::trace::ObjectKind::Unknown, "ID3D12StateObject", parent);
    }
    std::ostringstream payload;
    payload << "{\"subobject_count\":" << (addition ? addition->NumSubobjects : 0) << "}";
    record_call_locked("ID3D12Device7::AddToStateObject", hr, {self, state_object_to_grow_from, *new_state_object}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_protected_resource_session1(
    ID3D12Device7 *self,
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
    REFIID riid,
    void **session)
{
  const auto hook = device7_hook_for(self);
  if (!hook.create_protected_resource_session1) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device7, ID3D12Device7Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_protected_resource_session1(self, desc, riid, session);
  if (SUCCEEDED(hr) && session && *session) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_fresh_object_locked(*session, apitrace::trace::ObjectKind::Unknown, "ID3D12ProtectedResourceSession", parent);
    std::ostringstream payload;
    payload << "{\"node_mask\":" << (desc ? desc->NodeMask : 0)
            << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
            << "}";
    record_call_locked("ID3D12Device7::CreateProtectedResourceSession1", hr, {self, *session}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_committed_resource2(
    ID3D12Device8 *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **resource)
{
  const auto hook = device8_hook_for(self);
  if (!hook.create_committed_resource2) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device8, ID3D12Device8Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_committed_resource2(
      self,
      heap_properties,
      heap_flags,
      desc,
      initial_state,
      optimized_clear_value,
      protected_session,
      riid,
      resource);
  if (SUCCEEDED(hr) && resource && *resource) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    const auto protected_session_object_id = lookup_object_id_locked(protected_session);
    const auto resource_object_id =
        register_fresh_object_locked(*resource, apitrace::trace::ObjectKind::Resource, "ID3D12Resource", parent);
    const auto heap_type = heap_properties ? static_cast<UINT>(heap_properties->Type) : 0;
    const auto gpu_virtual_address = resource_gpu_virtual_address(static_cast<ID3D12Resource *>(*resource));
    capture_state().resource_gpu_virtual_addresses[static_cast<ID3D12Resource *>(*resource)] = {
        resource_object_id,
        gpu_virtual_address,
        desc ? desc->Width : 0,
        capture_state().next_sequence,
    };
    remember_resource_capture_info_locked(
        static_cast<ID3D12Resource *>(*resource),
        resource_object_id,
        desc,
        heap_type,
        heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD));
    std::ostringstream payload;
    payload << "{"
            << "\"heap_type\":" << (heap_properties ? static_cast<unsigned int>(heap_properties->Type) : 0) << ","
            << "\"heap_flags\":" << static_cast<unsigned int>(heap_flags) << ","
            << "\"initial_state\":" << static_cast<unsigned int>(initial_state) << ","
            << "\"resource_desc\":" << resource_desc1_json(desc) << ","
            << "\"optimized_clear_value\":" << clear_value_json(optimized_clear_value) << ","
            << "\"protected_session_object_id\":" << object_id_json(protected_session_object_id) << ","
            << "\"gpu_virtual_address\":" << gpu_virtual_address
            << "}";
    record_call_locked("ID3D12Device8::CreateCommittedResource2", hr, {self, *resource, protected_session}, {}, payload.str());
  }
  if (SUCCEEDED(hr) && resource && *resource) {
    patch_resource(static_cast<ID3D12Resource *>(*resource));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_placed_resource1(
    ID3D12Device8 *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource)
{
  const auto hook = device8_hook_for(self);
  if (!hook.create_placed_resource1) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device8, ID3D12Device8Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_placed_resource1(self, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource);
  if (SUCCEEDED(hr) && resource && *resource) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    ensure_placed_resource_heap_object_locked(reinterpret_cast<ID3D12Device *>(self), heap);
    const auto heap_object_id = lookup_object_id_locked(heap);
    const auto resource_object_id =
        register_fresh_object_locked(*resource, apitrace::trace::ObjectKind::Resource, "ID3D12Resource", heap_object_id);
    const auto heap_type_it = capture_state().heap_types.find(heap);
    const auto heap_type = heap_type_it != capture_state().heap_types.end() ? heap_type_it->second : 0;
    const auto gpu_virtual_address = resource_gpu_virtual_address(static_cast<ID3D12Resource *>(*resource));
    capture_state().resource_gpu_virtual_addresses[static_cast<ID3D12Resource *>(*resource)] = {
        resource_object_id,
        gpu_virtual_address,
        desc ? desc->Width : 0,
        capture_state().next_sequence,
    };
    remember_resource_capture_info_locked(
        static_cast<ID3D12Resource *>(*resource),
        resource_object_id,
        desc,
        heap_type,
        heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD));
    std::ostringstream payload;
    payload << "{"
            << "\"heap_object_id\":" << object_id_json(heap_object_id) << ","
            << "\"heap_offset\":" << heap_offset << ","
            << "\"initial_state\":" << static_cast<unsigned int>(initial_state) << ","
            << "\"resource_desc\":" << resource_desc1_json(desc) << ","
            << "\"optimized_clear_value\":" << clear_value_json(optimized_clear_value) << ","
            << "\"gpu_virtual_address\":" << gpu_virtual_address
            << "}";
    record_call_locked("ID3D12Device8::CreatePlacedResource1", hr, {self, heap, *resource}, {}, payload.str());
  }
  if (SUCCEEDED(hr) && resource && *resource) {
    patch_resource(static_cast<ID3D12Resource *>(*resource));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_shader_cache_session(
    ID3D12Device9 *self,
    const D3D12_SHADER_CACHE_SESSION_DESC *desc,
    REFIID riid,
    void **session)
{
  const auto hook = device9_hook_for(self);
  if (!hook.create_shader_cache_session) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device9, ID3D12Device9Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_shader_cache_session(self, desc, riid, session);
  if (SUCCEEDED(hr) && session && *session) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_fresh_object_locked(*session, apitrace::trace::ObjectKind::Unknown, "ID3D12ShaderCacheSession", parent);
    std::ostringstream payload;
    payload << "{\"mode\":" << (desc ? static_cast<unsigned int>(desc->Mode) : 0)
            << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
            << ",\"maximum_in_memory_cache_size_bytes\":" << (desc ? desc->MaximumInMemoryCacheSizeBytes : 0)
            << ",\"maximum_in_memory_cache_entries\":" << (desc ? desc->MaximumInMemoryCacheEntries : 0)
            << ",\"maximum_value_file_size_bytes\":" << (desc ? desc->MaximumValueFileSizeBytes : 0)
            << ",\"version\":" << (desc ? desc->Version : 0)
            << "}";
    record_call_locked("ID3D12Device9::CreateShaderCacheSession", hr, {self, *session}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_shader_cache_control(
    ID3D12Device9 *self,
    D3D12_SHADER_CACHE_KIND_FLAGS kinds,
    D3D12_SHADER_CACHE_CONTROL_FLAGS control)
{
  const auto hook = device9_hook_for(self);
  if (!hook.shader_cache_control) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device9, ID3D12Device9Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.shader_cache_control(self, kinds, control);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"kinds\":" << static_cast<unsigned int>(kinds)
          << ",\"control\":" << static_cast<unsigned int>(control)
          << "}";
  record_call_locked("ID3D12Device9::ShaderCacheControl", hr, {self}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_queue1(
    ID3D12Device9 *self,
    const D3D12_COMMAND_QUEUE_DESC *desc,
    REFIID creator_id,
    REFIID riid,
    void **command_queue)
{
  const auto hook = device9_hook_for(self);
  if (!hook.create_command_queue1) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device9, ID3D12Device9Vtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_command_queue1(self, desc, creator_id, riid, command_queue);
  if (SUCCEEDED(hr) && command_queue && *command_queue) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_fresh_object_locked(*command_queue, apitrace::trace::ObjectKind::CommandQueue, "ID3D12CommandQueue", parent);
      std::ostringstream payload;
      payload << "{"
              << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
              << "\"priority\":" << (desc ? desc->Priority : 0) << ","
              << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0) << ","
              << "\"node_mask\":" << (desc ? desc->NodeMask : 0) << ","
              << "\"creator_id\":\"" << guid_string(creator_id) << "\""
              << "}";
      record_call_locked("ID3D12Device9::CreateCommandQueue1", hr, {self, *command_queue}, {}, payload.str());
    }
    patch_command_queue(static_cast<ID3D12CommandQueue *>(*command_queue));
  }
  return hr;
}

#include "d3d12_proxy/hooks.inc"

} // namespace

#include "d3d12_proxy/exports.inc"
