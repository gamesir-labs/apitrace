#pragma once

#include "apitrace/capture_runtime.hpp"
#include "apitrace/object_types.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;
struct D3D12_COMMAND_QUEUE_DESC;
struct D3D12_DESCRIPTOR_HEAP_DESC;
struct D3D12_HEAP_PROPERTIES;
struct D3D12_RESOURCE_DESC;
struct D3D12_CLEAR_VALUE;
struct D3D12_GRAPHICS_PIPELINE_STATE_DESC;
struct D3D12_COMPUTE_PIPELINE_STATE_DESC;
struct D3D12_HEAP_DESC;
struct D3D12_CONSTANT_BUFFER_VIEW_DESC;
struct D3D12_SHADER_RESOURCE_VIEW_DESC;
struct D3D12_UNORDERED_ACCESS_VIEW_DESC;
struct D3D12_RENDER_TARGET_VIEW_DESC;
struct D3D12_DEPTH_STENCIL_VIEW_DESC;
struct D3D12_SAMPLER_DESC;
struct D3D12_RESOURCE_BARRIER;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct D3D12_GPU_DESCRIPTOR_HANDLE;
struct D3D12_TEXTURE_COPY_LOCATION;
struct D3D12_BOX;
struct D3D12_VIEWPORT;
struct D3D12_VERTEX_BUFFER_VIEW;
struct D3D12_INDEX_BUFFER_VIEW;
struct D3D12_COMMAND_SIGNATURE_DESC;
struct D3D12_WRITEBUFFERIMMEDIATE_PARAMETER;
struct D3D12_QUERY_HEAP_DESC;
struct D3D12_ROOT_SIGNATURE_DESC;

namespace apitrace::d3d12 {

struct RenderPassClearValue {
  std::uint32_t format = 0;
  float color[4] = {};
  float depth = 0.0f;
  std::uint8_t stencil = 0;
};

struct RenderPassBeginningAccessDesc {
  std::uint32_t type = 0;
  RenderPassClearValue clear = {};
};

struct RenderPassResolveSubresourceDesc {
  std::uint32_t src_subresource = 0;
  std::uint32_t dst_subresource = 0;
  std::uint32_t dst_x = 0;
  std::uint32_t dst_y = 0;
  bool has_src_rect = false;
  std::int32_t src_left = 0;
  std::int32_t src_top = 0;
  std::int32_t src_right = 0;
  std::int32_t src_bottom = 0;
};

struct RenderPassEndingAccessDesc {
  std::uint32_t type = 0;
  const void *src_resource = nullptr;
  const void *dst_resource = nullptr;
  std::uint32_t subresource_count = 0;
  const RenderPassResolveSubresourceDesc *subresources = nullptr;
  std::uint32_t format = 0;
  std::uint32_t resolve_mode = 0;
  bool preserve_resolve_source = false;
};

struct RenderPassRenderTargetDesc {
  std::uint64_t cpu_descriptor = 0;
  RenderPassBeginningAccessDesc beginning_access = {};
  RenderPassEndingAccessDesc ending_access = {};
};

struct RenderPassDepthStencilDesc {
  std::uint64_t cpu_descriptor = 0;
  RenderPassBeginningAccessDesc depth_beginning_access = {};
  RenderPassBeginningAccessDesc stencil_beginning_access = {};
  RenderPassEndingAccessDesc depth_ending_access = {};
  RenderPassEndingAccessDesc stencil_ending_access = {};
};

enum class CaptureObjectKind {
  Unknown,
  Device,
  CommandQueue,
  CommandAllocator,
  CommandList,
  CommandSignature,
  Fence,
  SwapChain,
  Heap,
  Resource,
  View,
  Shader,
  PipelineState,
  RootSignature,
  DescriptorHeap,
  QueryHeap,
};

class D3D12CaptureHooks {
public:
  D3D12CaptureHooks();

  void install_proxy_hooks(runtime::CaptureRuntime &runtime);
  void install_device_hooks(runtime::CaptureRuntime &runtime);
  void install_submission_hooks(runtime::CaptureRuntime &runtime);

private:
  // The Wine app-local d3d12.dll proxy owns the concrete COM vtable capture surface.
  // This planner keeps shared capture runtime setup scoped to that existing entry.
};

bool builtin_capture_enabled();
std::uint64_t current_sequence();
std::uint64_t object_id(const void *object);
std::uint64_t register_blob(const char *debug_name, const void *data, std::size_t size);

std::uint64_t record_call(
    const char *opname,
    const char *payload_json = "{}",
    const void *const *object_refs = nullptr,
    std::uint32_t object_ref_count = 0,
    const std::uint64_t *blob_refs = nullptr,
    std::uint32_t blob_ref_count = 0,
    std::int32_t result_code = 0);

void record_call_with_sequence(
    std::uint64_t sequence,
    const char *opname,
    const char *payload_json = "{}",
    const void *const *object_refs = nullptr,
    std::uint32_t object_ref_count = 0,
    const std::uint64_t *blob_refs = nullptr,
    std::uint32_t blob_ref_count = 0,
    std::int32_t result_code = 0);

void record_object_create(
    const void *object,
    CaptureObjectKind kind,
    const void *parent_object,
    const char *debug_name,
    const char *payload_json = "{}");

void record_object_destroy(
    const void *object,
    CaptureObjectKind kind,
    const char *payload_json = "{}");

void record_resource_blob(
    const char *debug_name,
    const std::uint64_t *blob_refs,
    std::uint32_t blob_ref_count,
    const char *payload_json = "{}");

std::uint64_t record_d3d12_create_device(const void *device);
std::uint64_t record_d3d11_create_device(const void *device);
std::uint64_t record_dxgi_create_swapchain(
    const void *factory,
    const void *device,
    const void *swapchain);
void record_swapchain_back_buffer(
    const void *device,
    const void *swapchain,
    ID3D12Resource *back_buffer,
    std::uint32_t buffer_index);
std::uint64_t record_execute_command_lists(
    const void *queue,
    const void *command_list);
std::uint64_t record_present(
    const void *swapchain,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    std::int32_t result_code,
    bool frame_presented);
void record_present_frame(
    std::uint64_t frame_index,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t row_pitch,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    const void *rgba_data,
    std::size_t rgba_size);
void record_resource_unmap(
    const void *resource,
    std::uint32_t subresource,
    std::uint64_t written_begin,
    std::uint64_t written_end,
    const void *written_data,
    std::size_t written_size);
#if defined(APITRACE_ENABLE_TEST_HOOKS)
struct RawUnmapFastPathCounters {
  std::uint64_t unmap_candidates = 0;
  std::uint64_t unchanged_skipped = 0;
  std::uint64_t emitted_blob_bytes = 0;
  std::uint64_t raw_write_failures = 0;
};
void reset_raw_unmap_fast_path_for_test();
RawUnmapFastPathCounters raw_unmap_fast_path_counters_for_test();
#endif
void record_resource_bytes_snapshot(
    trace::ObjectId resource_object_id,
    std::uint64_t begin,
    std::uint64_t end,
    const void *bytes,
    std::uint64_t sequence);
std::uint64_t record_resource_map(
    const void *resource,
    std::uint32_t subresource,
    bool has_read_range,
    std::uint64_t read_begin,
    std::uint64_t read_end,
    const void *mapped_data,
    bool mapped,
    std::int32_t result_code);
void record_resolve_query_data_result(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t start_index,
    std::uint32_t query_count,
    const void *dst_buffer,
    std::uint64_t aligned_dst_buffer_offset,
    const void *resolved_data,
    std::size_t resolved_size);
void record_fence_dependency(
    const char *scope,
    std::uint64_t d3d_sequence,
    std::uint64_t encoder_id,
    bool implicit_pre_raster_wait,
    const std::uint64_t *strong_masks,
    std::uint32_t strong_count,
    const std::uint64_t *full_masks,
    std::uint32_t full_count,
    const std::uint64_t *minimal_masks,
    std::uint32_t minimal_count,
    std::uint32_t mask_count);

std::uint64_t record_create_command_queue(
    ID3D12Device *device,
    const D3D12_COMMAND_QUEUE_DESC *desc,
    const void *command_queue,
    std::int32_t result_code);

std::uint64_t record_create_command_allocator(
    ID3D12Device *device,
    std::uint32_t type,
    const void *command_allocator,
    std::int32_t result_code);

std::uint64_t record_create_command_list(
    ID3D12Device *device,
    std::uint32_t node_mask,
    std::uint32_t type,
    const void *command_allocator,
    const void *initial_pipeline_state,
    const void *command_list,
    std::int32_t result_code);

std::uint64_t record_create_command_list1(
    ID3D12Device *device,
    std::uint32_t node_mask,
    std::uint32_t type,
    std::uint32_t flags,
    const void *command_list,
    std::int32_t result_code);

std::uint64_t record_create_graphics_pipeline_state(
    ID3D12Device *device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state,
    std::int32_t result_code);

std::uint64_t record_create_compute_pipeline_state(
    ID3D12Device *device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state,
    std::int32_t result_code);

std::uint64_t record_create_pipeline_state(
    ID3D12Device *device,
    const void *stream,
    std::size_t stream_size,
    const void *pipeline_state,
    std::int32_t result_code);

std::uint64_t record_create_descriptor_heap(
    ID3D12Device *device,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc,
    const void *descriptor_heap,
    std::uint32_t descriptor_size,
    std::uint64_t cpu_start,
    std::uint64_t gpu_start,
    std::int32_t result_code);

std::uint64_t record_create_query_heap(
    ID3D12Device *device,
    const D3D12_QUERY_HEAP_DESC *desc,
    const void *query_heap,
    std::int32_t result_code);

std::uint64_t record_create_root_signature(
    ID3D12Device *device,
    std::uint32_t node_mask,
    const void *bytecode,
    std::size_t bytecode_length,
    const void *root_signature,
    std::int32_t result_code,
    const D3D12_ROOT_SIGNATURE_DESC *desc = nullptr);

std::uint64_t record_create_committed_resource(
    ID3D12Device *device,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    std::uint32_t heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    std::uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource,
    std::uint64_t gpu_virtual_address,
    std::int32_t result_code);

std::uint64_t record_create_heap(
    ID3D12Device *device,
    const D3D12_HEAP_DESC *desc,
    const void *heap,
    std::int32_t result_code,
    const char *function_name = nullptr);

std::uint64_t record_create_placed_resource(
    ID3D12Device *device,
    const void *heap,
    std::uint64_t heap_offset,
    const D3D12_RESOURCE_DESC *desc,
    std::uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource,
    std::uint64_t gpu_virtual_address,
    std::int32_t result_code);

std::uint64_t record_create_reserved_resource(
    ID3D12Device *device,
    const D3D12_RESOURCE_DESC *desc,
    std::uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource,
    std::uint64_t gpu_virtual_address,
    std::int32_t result_code);

std::uint64_t record_create_fence(
    ID3D12Device *device,
    std::uint64_t initial_value,
    std::uint32_t flags,
    const void *fence,
    std::int32_t result_code);

std::uint64_t record_create_command_signature(
    ID3D12Device *device,
    const D3D12_COMMAND_SIGNATURE_DESC *desc,
    const void *root_signature,
    const void *command_signature,
    std::int32_t result_code);

std::uint64_t record_create_constant_buffer_view(
    ID3D12Device *device,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
    const void *resolved_resource,
    std::uint64_t resolved_resource_offset,
    std::uint64_t resolved_resource_width);

std::uint64_t record_create_shader_resource_view(
    ID3D12Device *device,
    const void *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

std::uint64_t record_create_unordered_access_view(
    ID3D12Device *device,
    const void *resource,
    const void *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

std::uint64_t record_create_render_target_view(
    ID3D12Device *device,
    const void *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

std::uint64_t record_create_depth_stencil_view(
    ID3D12Device *device,
    const void *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

std::uint64_t record_create_sampler(
    ID3D12Device *device,
    const D3D12_SAMPLER_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

std::uint64_t record_copy_descriptors(
    ID3D12Device *device,
    std::uint32_t dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const std::uint32_t *dst_descriptor_range_sizes,
    std::uint32_t src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const std::uint32_t *src_descriptor_range_sizes,
    std::uint32_t descriptor_heap_type,
    std::uint32_t descriptor_size);

std::uint64_t record_copy_descriptors_simple(
    ID3D12Device *device,
    std::uint32_t descriptor_count,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_start,
    D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_start,
    std::uint32_t descriptor_heap_type,
    std::uint32_t descriptor_size);

std::uint64_t record_draw_instanced(
    const void *command_list,
    std::uint32_t vertex_count_per_instance,
    std::uint32_t instance_count,
    std::uint32_t start_vertex_location,
    std::uint32_t start_instance_location);

std::uint64_t record_draw_indexed_instanced(
    const void *command_list,
    std::uint32_t index_count_per_instance,
    std::uint32_t instance_count,
    std::uint32_t start_index_location,
    std::int32_t base_vertex_location,
    std::uint32_t start_instance_location);

std::uint64_t record_dispatch(
    const void *command_list,
    std::uint32_t thread_group_count_x,
    std::uint32_t thread_group_count_y,
    std::uint32_t thread_group_count_z);

std::uint64_t record_close_command_list(
    const void *command_list,
    std::int32_t result_code);

std::uint64_t record_reset_command_list(
    const void *command_list,
    const void *command_allocator,
    const void *initial_pipeline_state,
    std::int32_t result_code);

std::uint64_t record_execute_indirect(
    const void *command_list,
    const void *command_signature,
    std::uint32_t max_command_count,
    const void *arg_buffer,
    std::uint64_t arg_buffer_offset,
    const void *count_buffer,
    std::uint64_t count_buffer_offset);

std::uint64_t record_execute_bundle(
    const void *command_list,
    const void *bundle_command_list);

std::uint64_t record_resource_barrier(
    const void *command_list,
    std::uint32_t barrier_count,
    const D3D12_RESOURCE_BARRIER *barriers);

std::uint64_t record_copy_buffer_region(
    const char *function_name,
    const void *command_list,
    const void *dst_buffer,
    std::uint64_t dst_offset,
    const void *src_buffer,
    std::uint64_t src_offset,
    std::uint64_t byte_count);

std::uint64_t record_copy_texture_region(
    const void *command_list,
    const D3D12_TEXTURE_COPY_LOCATION *dst,
    std::uint32_t dst_x,
    std::uint32_t dst_y,
    std::uint32_t dst_z,
    const D3D12_TEXTURE_COPY_LOCATION *src,
    const D3D12_BOX *src_box);

std::uint64_t record_copy_resource(
    const void *command_list,
    const void *dst_resource,
    const void *src_resource);

std::uint64_t record_resolve_subresource(
    const void *command_list,
    const void *dst_resource,
    std::uint32_t dst_subresource,
    const void *src_resource,
    std::uint32_t src_subresource,
    std::uint32_t format);

std::uint64_t record_ia_set_primitive_topology(
    const void *command_list,
    std::uint32_t primitive_topology);

std::uint64_t record_rs_set_viewports(
    const void *command_list,
    std::uint32_t viewport_count,
    const D3D12_VIEWPORT *viewports);

std::uint64_t record_rs_set_scissor_rects(
    const void *command_list,
    std::uint32_t rect_count,
    const void *rects);

std::uint64_t record_set_pipeline_state(
    const void *command_list,
    const void *pipeline_state);

std::uint64_t record_clear_state(
    const void *command_list,
    const void *pipeline_state);

std::uint64_t record_om_set_blend_factor(
    const void *command_list,
    const float blend_factor[4]);

std::uint64_t record_om_set_stencil_ref(
    const void *command_list,
    std::uint32_t stencil_ref);

std::uint64_t record_set_descriptor_heaps(
    const void *command_list,
    std::uint32_t heap_count,
    const void *const *heaps);

std::uint64_t record_set_root_signature(
    const void *command_list,
    bool compute,
    const void *root_signature);

std::uint64_t record_set_root_descriptor_table(
    const void *command_list,
    bool compute,
    std::uint32_t root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);

std::uint64_t record_set_root_32bit_constants(
    const void *command_list,
    bool compute,
    std::uint32_t root_parameter_index,
    std::uint32_t constant_count,
    const std::uint32_t *values,
    std::uint32_t dst_offset);

std::uint64_t record_set_root_descriptor(
    const void *command_list,
    bool compute,
    std::uint32_t parameter_type,
    std::uint32_t root_parameter_index,
    std::uint64_t gpu_virtual_address);

std::uint64_t record_ia_set_index_buffer(
    const void *command_list,
    const D3D12_INDEX_BUFFER_VIEW *view);

std::uint64_t record_ia_set_vertex_buffers(
    const void *command_list,
    std::uint32_t start_slot,
    std::uint32_t view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views);

std::uint64_t record_om_set_render_targets(
    const void *command_list,
    std::uint32_t render_target_descriptor_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    bool single_descriptor_handle,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor);

std::uint64_t record_clear_depth_stencil_view(
    const void *command_list,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    std::uint32_t flags,
    float depth,
    std::uint8_t stencil,
    std::uint32_t rect_count,
    const void *rects);

std::uint64_t record_clear_render_target_view(
    const void *command_list,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    const float color[4],
    std::uint32_t rect_count,
    const void *rects);

std::uint64_t record_clear_unordered_access_view_uint(
    const void *command_list,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor,
    const void *resource,
    const std::uint32_t values[4],
    std::uint32_t rect_count,
    const void *rects);

std::uint64_t record_clear_unordered_access_view_float(
    const void *command_list,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor,
    const void *resource,
    const float values[4],
    std::uint32_t rect_count,
    const void *rects);

std::uint64_t record_discard_resource(
    const void *command_list,
    const void *resource,
    std::uint32_t first_subresource,
    std::uint32_t subresource_count,
    std::uint32_t rect_count,
    const void *rects);

std::uint64_t record_begin_query(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t index);

std::uint64_t record_end_query(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t index);

std::uint64_t record_resolve_query_data(
    const void *command_list,
    const void *query_heap,
    std::uint32_t type,
    std::uint32_t start_index,
    std::uint32_t query_count,
    const void *dst_buffer,
    std::uint64_t aligned_dst_buffer_offset);

std::uint64_t record_set_predication(
    const void *command_list,
    const void *buffer,
    std::uint64_t aligned_buffer_offset,
    std::uint32_t operation);

std::uint64_t record_resolve_subresource_region(
    const void *command_list,
    const void *dst_resource,
    std::uint32_t dst_subresource,
    std::uint32_t dst_x,
    std::uint32_t dst_y,
    const void *src_resource,
    std::uint32_t src_subresource,
    const void *src_rect,
    std::uint32_t format,
    std::uint32_t mode);

std::uint64_t record_write_buffer_immediate(
    const void *command_list,
    std::uint32_t count,
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const void *modes);

std::uint64_t record_begin_render_pass(
    const void *command_list,
    std::uint32_t render_targets_count,
    const RenderPassRenderTargetDesc *render_targets,
    const RenderPassDepthStencilDesc *depth_stencil,
    std::uint32_t flags);

std::uint64_t record_end_render_pass(const void *command_list);

std::uint64_t record_temporal_upscale(
    const void *command_list,
    std::uint32_t input_content_width,
    std::uint32_t input_content_height,
    bool auto_exposure,
    bool in_reset,
    bool depth_reversed,
    bool motion_vector_in_display_res,
    const void *color,
    const void *depth,
    const void *motion_vector,
    const void *output,
    float motion_vector_scale_x,
    float motion_vector_scale_y,
    float pre_exposure,
    const void *exposure_texture,
    float jitter_offset_x,
    float jitter_offset_y);

} // namespace apitrace::d3d12
