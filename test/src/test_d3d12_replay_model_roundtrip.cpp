// Round-trip self-test for D3D12 replay-model serialization.
//
// Populates a backend so that EVERY field of EVERY container holds a distinct non-default value,
// saves it via save_replay_model, loads it into a second backend via load_replay_model, then
// asserts field-by-field equality across all containers. A field that is never serialized would
// keep its default in the loaded backend and fail the equality check; a field that is written but
// not read would change the deterministic re-serialization and fail the byte-identity check.

#include "apitrace/d3d12_replay.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace apitrace::d3d12 {

// Memberwise-equality helpers. F(x) compares a scalar/struct/container member by member; AEQ(x)
// compares a fixed C array element by element. These compose: vector/unordered_map operator== call
// the element operator== found via ADL in this namespace.
namespace {
template <typename T, std::size_t N>
bool array_equal(const T (&a)[N], const T (&b)[N])
{
  for (std::size_t i = 0; i < N; ++i) {
    if (!(a[i] == b[i])) {
      return false;
    }
  }
  return true;
}
} // namespace

#define F(x) (a.x == b.x)
#define AEQ(x) (array_equal(a.x, b.x))

using B = D3D12ReplayBackend;

inline bool operator==(const B::GpuVirtualAddressBinding &a, const B::GpuVirtualAddressBinding &b)
{ return F(gpu_virtual_address) && F(resource_object_id) && F(offset) && F(resolved); }

inline bool operator==(const B::DescriptorBinding &a, const B::DescriptorBinding &b)
{ return F(descriptor) && F(heap_object_id) && F(heap_type) && F(descriptor_index); }

inline bool operator==(const B::ViewportSemanticState &a, const B::ViewportSemanticState &b)
{ return F(x) && F(y) && F(width) && F(height) && F(min_depth) && F(max_depth); }

inline bool operator==(const B::ScissorRectSemanticState &a, const B::ScissorRectSemanticState &b)
{ return F(left) && F(top) && F(right) && F(bottom); }

inline bool operator==(const B::Root32BitConstantsBinding &a, const B::Root32BitConstantsBinding &b)
{ return F(root_parameter_index) && F(dst_offset) && F(values); }

inline bool operator==(const B::VertexBufferBinding &a, const B::VertexBufferBinding &b)
{ return F(sequence) && F(slot) && F(address) && F(size_in_bytes) && F(stride_in_bytes); }

inline bool operator==(const B::IndexBufferBinding &a, const B::IndexBufferBinding &b)
{ return F(sequence) && F(address) && F(size_in_bytes) && F(format); }

inline bool operator==(const B::ResourceTransition &a, const B::ResourceTransition &b)
{ return F(sequence) && F(command_list_object_id) && F(flags) && F(before) && F(after) && F(subresource); }

inline bool operator==(const B::ResourceBarrierSemanticState &a, const B::ResourceBarrierSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(type) && F(flags) && F(resource_object_id)
      && F(resource_before_object_id) && F(resource_after_object_id) && F(before) && F(after) && F(subresource); }

inline bool operator==(const B::TextureCopyLocation &a, const B::TextureCopyLocation &b)
{ return F(resource_object_id) && F(type) && F(subresource_index) && F(footprint_offset) && F(footprint_format)
      && F(footprint_width) && F(footprint_height) && F(footprint_depth) && F(footprint_row_pitch); }

inline bool operator==(const B::TextureCopySemanticState &a, const B::TextureCopySemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(dst) && F(src) && F(dst_x) && F(dst_y) && F(dst_z)
      && F(has_src_box) && F(src_box_left) && F(src_box_top) && F(src_box_front) && F(src_box_right)
      && F(src_box_bottom) && F(src_box_back); }

inline bool operator==(const B::ResourceCopySemanticState &a, const B::ResourceCopySemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(dst_resource_object_id) && F(src_resource_object_id); }

inline bool operator==(const B::BufferCopySemanticState &a, const B::BufferCopySemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(dst_buffer_object_id) && F(src_buffer_object_id)
      && F(dst_offset) && F(src_offset) && F(byte_count); }

inline bool operator==(const B::ResolveSubresourceSemanticState &a, const B::ResolveSubresourceSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(dst_resource_object_id) && F(src_resource_object_id)
      && F(dst_subresource) && F(dst_x) && F(dst_y) && F(src_subresource) && F(has_src_rect) && F(src_rect_left)
      && F(src_rect_top) && F(src_rect_right) && F(src_rect_bottom) && F(format) && F(mode); }

inline bool operator==(const B::QueryCommandSemanticState &a, const B::QueryCommandSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(query_heap_object_id) && F(type) && F(index)
      && F(start_index) && F(query_count) && F(dst_buffer_object_id) && F(aligned_dst_buffer_offset)
      && F(resolve) && F(end); }

inline bool operator==(const B::PredicationSemanticState &a, const B::PredicationSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(buffer_object_id) && F(aligned_buffer_offset) && F(operation); }

inline bool operator==(const B::WriteBufferImmediateEntry &a, const B::WriteBufferImmediateEntry &b)
{ return F(dest) && F(value) && F(mode); }

inline bool operator==(const B::WriteBufferImmediateSemanticState &a, const B::WriteBufferImmediateSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(writes); }

inline bool operator==(const B::RenderTargetClearSemanticState &a, const B::RenderTargetClearSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(render_target) && AEQ(color) && F(rect_count)
      && F(has_color) && F(rects); }

inline bool operator==(const B::DepthStencilClearSemanticState &a, const B::DepthStencilClearSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(depth_stencil) && F(depth) && F(clear_flags)
      && F(stencil) && F(rect_count) && F(rects); }

inline bool operator==(const B::UnorderedAccessClearSemanticState &a, const B::UnorderedAccessClearSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(gpu_descriptor) && F(cpu_descriptor) && F(resource_object_id)
      && AEQ(uint_values) && AEQ(float_values) && F(integer) && F(rect_count) && F(rects); }

inline bool operator==(const B::DiscardResourceSemanticState &a, const B::DiscardResourceSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(resource_object_id) && F(first_subresource)
      && F(subresource_count) && F(rect_count) && F(has_region) && F(rects); }

inline bool operator==(const B::CommandSignatureArgument &a, const B::CommandSignatureArgument &b)
{ return F(type) && F(slot) && F(root_parameter_index) && F(dest_offset_in32bit_values) && F(num32bit_values_to_set); }

inline bool operator==(const B::CommandSignatureSemanticState &a, const B::CommandSignatureSemanticState &b)
{ return F(command_signature_object_id) && F(root_signature_object_id) && F(create_sequence) && F(byte_stride)
      && F(argument_count) && F(node_mask) && F(arguments); }

inline bool operator==(const B::ExecuteIndirectSemanticState &a, const B::ExecuteIndirectSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(command_signature_object_id) && F(arg_buffer_object_id)
      && F(count_buffer_object_id) && F(pipeline_state_object_id) && F(graphics_root_signature_object_id)
      && F(compute_root_signature_object_id) && F(graphics_root_tables) && F(compute_root_tables)
      && F(render_targets) && F(depth_stencil) && F(viewports) && F(scissor_rects) && F(primitive_topology)
      && F(max_command_count) && F(arg_buffer_offset) && F(count_buffer_offset); }

inline bool operator==(const B::ExecuteBundleSemanticState &a, const B::ExecuteBundleSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(bundle_command_list_object_id); }

inline bool operator==(const B::DispatchSemanticState &a, const B::DispatchSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(pipeline_state_object_id) && F(graphics_root_signature_object_id)
      && F(compute_root_signature_object_id) && F(graphics_root_tables) && F(compute_root_tables)
      && F(thread_group_count_x) && F(thread_group_count_y) && F(thread_group_count_z) && F(mesh); }

inline bool operator==(const B::DispatchRaysSemanticState &a, const B::DispatchRaysSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(width) && F(height) && F(depth)
      && F(ray_generation_shader_record) && F(ray_generation_shader_record_size) && F(miss_shader_table)
      && F(miss_shader_table_size) && F(miss_shader_table_stride) && F(hit_group_table) && F(hit_group_table_size)
      && F(hit_group_table_stride) && F(callable_shader_table) && F(callable_shader_table_size)
      && F(callable_shader_table_stride); }

inline bool operator==(const B::TemporalUpscaleSemanticState &a, const B::TemporalUpscaleSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(input_content_width) && F(input_content_height)
      && F(auto_exposure) && F(in_reset) && F(depth_reversed) && F(motion_vector_in_display_res) && F(color_object_id)
      && F(depth_object_id) && F(motion_vector_object_id) && F(output_object_id) && F(exposure_texture_object_id)
      && F(motion_vector_scale_x) && F(motion_vector_scale_y) && F(pre_exposure) && F(jitter_offset_x)
      && F(jitter_offset_y); }

inline bool operator==(const B::DrawSemanticState &a, const B::DrawSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(pipeline_state_object_id) && F(graphics_root_signature_object_id)
      && F(graphics_root_tables) && F(render_targets) && F(depth_stencil) && F(viewports) && F(scissor_rects)
      && F(primitive_topology) && F(vertex_count_per_instance) && F(instance_count) && F(start_vertex_location)
      && F(start_instance_location); }

inline bool operator==(const B::DrawIndexedSemanticState &a, const B::DrawIndexedSemanticState &b)
{ return F(sequence) && F(command_list_object_id) && F(pipeline_state_object_id) && F(graphics_root_signature_object_id)
      && F(graphics_root_tables) && F(render_targets) && F(depth_stencil) && F(viewports) && F(scissor_rects)
      && F(primitive_topology) && F(index_count_per_instance) && F(instance_count) && F(start_index_location)
      && F(base_vertex_location) && F(start_instance_location); }

inline bool operator==(const B::RootDescriptorRangeSemanticState &a, const B::RootDescriptorRangeSemanticState &b)
{ return F(type) && F(descriptor_count) && F(base_shader_register) && F(register_space)
      && F(offset_from_table_start) && F(flags); }

inline bool operator==(const B::RootDescriptorTableSemanticState &a, const B::RootDescriptorTableSemanticState &b)
{ return F(root_parameter_index) && F(shader_visibility) && F(ranges); }

inline bool operator==(const B::RootParameterSemanticState &a, const B::RootParameterSemanticState &b)
{ return F(root_parameter_index) && F(parameter_type) && F(shader_visibility) && F(shader_register)
      && F(register_space) && F(num_32bit_values) && F(range_count) && F(flags); }

inline bool operator==(const B::DescriptorSemanticState &a, const B::DescriptorSemanticState &b)
{ return F(kind) && F(create_sequence) && F(resource_object_id) && F(counter_resource_object_id) && F(descriptor)
      && F(binding) && F(format) && F(view_dimension) && F(flags) && F(shader_4_component_mapping)
      && F(buffer_location) && F(size_in_bytes) && F(first_element) && F(num_elements) && F(structure_byte_stride)
      && F(counter_offset_in_bytes) && F(has_view_desc) && F(most_detailed_mip) && F(mip_levels) && F(mip_slice)
      && F(plane_slice) && F(first_array_slice) && F(array_size) && F(first_2d_array_face) && F(num_cubes)
      && F(first_w_slice) && F(w_size) && F(resource_min_lod_clamp) && F(raytracing_acceleration_structure)
      && F(buffer); }

inline bool operator==(const B::SamplerSemanticState &a, const B::SamplerSemanticState &b)
{ return F(create_sequence) && F(descriptor) && F(binding) && F(filter) && F(address_u) && F(address_v)
      && F(address_w) && F(mip_lod_bias) && F(max_anisotropy) && F(comparison_func) && AEQ(border_color)
      && F(min_lod) && F(max_lod) && F(has_desc); }

inline bool operator==(const B::CopyDescriptorPair &a, const B::CopyDescriptorPair &b)
{ return F(dst_descriptor) && F(src_descriptor); }

inline bool operator==(const B::CopyDescriptorSemanticState &a, const B::CopyDescriptorSemanticState &b)
{ return F(create_sequence) && F(descriptor_heap_type) && F(descriptors); }

inline bool operator==(const B::DeviceSemanticState &a, const B::DeviceSemanticState &b)
{ return F(device_object_id) && F(create_sequence) && F(minimum_feature_level); }

inline bool operator==(const B::CommandQueueSemanticState &a, const B::CommandQueueSemanticState &b)
{ return F(queue_object_id) && F(create_sequence) && F(type) && F(priority) && F(flags) && F(node_mask)
      && F(execute_count) && F(last_execute_sequence); }

inline bool operator==(const B::CommandAllocatorSemanticState &a, const B::CommandAllocatorSemanticState &b)
{ return F(allocator_object_id) && F(create_sequence) && F(type) && F(reset_count) && F(last_reset_sequence)
      && F(last_submit_sequence) && F(last_submit_fence_object_id) && F(last_submit_fence_value); }

inline bool operator==(const B::HeapSemanticState &a, const B::HeapSemanticState &b)
{ return F(heap_object_id) && F(create_sequence) && F(size_in_bytes) && F(alignment) && F(heap_type)
      && F(cpu_page_property) && F(memory_pool_preference) && F(creation_node_mask) && F(visible_node_mask) && F(flags); }

inline bool operator==(const B::QueryHeapSemanticState &a, const B::QueryHeapSemanticState &b)
{ return F(query_heap_object_id) && F(create_sequence) && F(type) && F(count) && F(node_mask); }

inline bool operator==(const B::DescriptorHeapSemanticState &a, const B::DescriptorHeapSemanticState &b)
{ return F(heap_object_id) && F(create_sequence) && F(type) && F(num_descriptors) && F(flags) && F(node_mask)
      && F(descriptor_size) && F(cpu_start) && F(gpu_start); }

inline bool operator==(const B::FenceSemanticState &a, const B::FenceSemanticState &b)
{ return F(fence_object_id) && F(create_sequence) && F(initial_value) && F(flags) && F(current_value)
      && F(completed_value) && F(last_signal_sequence) && F(last_wait_sequence) && F(last_event_sequence); }

inline bool operator==(const B::FenceOperationSemanticState &a, const B::FenceOperationSemanticState &b)
{ return F(sequence) && F(queue_object_id) && F(fence_object_id) && F(fence_value) && F(queue_operation)
      && F(wait_operation) && F(event_operation); }

inline bool operator==(const B::PipelineSemanticState &a, const B::PipelineSemanticState &b)
{ return F(pipeline_state_object_id) && F(root_signature_object_id) && F(create_sequence) && F(graphics)
      && F(relative_path) && F(blob_refs) && F(uses_embedded_root_signature) && F(node_mask) && F(flags)
      && F(sample_mask) && F(primitive_topology_type) && F(num_render_targets) && F(dsv_format) && F(sample_count)
      && F(sample_quality) && F(ib_strip_cut_value) && F(input_element_count) && F(has_input_layout)
      && F(has_blend_state) && F(has_rasterizer_state) && F(has_depth_stencil_state) && F(rtv_formats)
      && F(has_vertex_shader) && F(has_pixel_shader) && F(has_domain_shader) && F(has_hull_shader)
      && F(has_geometry_shader) && F(has_compute_shader) && F(requires_dxmt_backend); }

inline bool operator==(const B::PresentFrame &a, const B::PresentFrame &b)
{ return F(relative_path) && F(width) && F(height) && F(row_pitch) && F(sync_interval) && F(flags) && F(frame_index); }

inline bool operator==(const B::PresentSemanticState &a, const B::PresentSemanticState &b)
{ return F(frame_index) && F(call_sequence) && F(boundary_sequence) && F(sync_interval) && F(flags)
      && F(result_code) && F(has_call) && F(has_boundary); }

inline bool operator==(const B::FrameSemanticState &a, const B::FrameSemanticState &b)
{ return F(frame_index) && F(begin_sequence) && F(end_sequence) && F(has_begin) && F(has_present) && F(has_end); }

inline bool operator==(const B::ResourceDataUpdate &a, const B::ResourceDataUpdate &b)
{ return F(sequence) && F(subresource) && F(written_begin) && F(written_end) && F(relative_path)
      && F(blob_refs) && F(bytes); }

inline bool operator==(const B::ResourceSemanticState &a, const B::ResourceSemanticState &b)
{ return F(resource_object_id) && F(heap_object_id) && F(create_sequence) && F(heap_type) && F(heap_flags)
      && F(initial_state) && F(current_state) && F(heap_offset) && F(last_transition_sequence) && F(gpu_virtual_address)
      && F(width) && F(alignment) && F(dimension) && F(height) && F(depth_or_array_size) && F(mip_levels)
      && F(recorded_mip_levels) && F(format) && F(sample_count) && F(sample_quality) && F(layout) && F(flags)
      && F(has_optimized_clear_value) && F(reserved_resource) && F(swapchain_back_buffer) && F(external_resource)
      && F(swapchain_buffer_index) && F(optimized_clear_format) && AEQ(optimized_clear_color) && F(optimized_clear_depth)
      && F(optimized_clear_stencil) && F(mapped) && F(mapped_subresource) && F(map_sequence) && F(transitions)
      && F(subresource_states) && F(data_updates); }

inline bool operator==(const B::RootSignatureSemanticState &a, const B::RootSignatureSemanticState &b)
{ return F(root_signature_object_id) && F(create_sequence) && F(node_mask) && F(bytecode_size) && F(relative_path)
      && F(blob_refs) && F(bytes) && F(descriptor_tables) && F(has_descriptor_table_metadata) && F(root_parameters)
      && F(has_root_parameter_metadata); }

inline bool operator==(const B::CommandListSemanticState &a, const B::CommandListSemanticState &b)
{ return F(recording) && F(type) && F(node_mask) && F(allocator_object_id) && F(pipeline_state_object_id)
      && F(graphics_root_signature_object_id) && F(compute_root_signature_object_id) && F(descriptor_heap_count)
      && F(render_target_count) && F(barrier_count) && F(clear_count) && F(copy_count) && F(draw_count)
      && F(dispatch_count) && F(first_sequence) && F(close_sequence) && F(primitive_topology)
      && F(descriptor_heap_object_ids) && F(viewports) && F(scissor_rects) && F(render_targets) && F(depth_stencil)
      && F(graphics_root_tables) && F(compute_root_tables) && F(graphics_root_constants) && F(compute_root_constants)
      && F(graphics_root_constant_buffers) && F(compute_root_constant_buffers) && F(graphics_root_shader_resources)
      && F(compute_root_shader_resources) && F(graphics_root_unordered_accesses) && F(compute_root_unordered_accesses)
      && F(vertex_buffers) && F(index_buffer) && F(buffer_copies) && F(texture_copies) && F(resource_copies)
      && F(barriers) && F(resolves) && F(queries) && F(predications) && F(write_buffer_immediates)
      && F(render_target_clears) && F(depth_stencil_clears) && F(unordered_access_clears) && F(discards)
      && F(indirect_executes) && F(bundle_executes) && F(draws) && F(indexed_draws) && F(dispatches)
      && F(ray_dispatches) && F(temporal_upscales) && F(command_indices); }

inline bool operator==(const B::ReplayCommandRecord &a, const B::ReplayCommandRecord &b)
{ return F(kind) && F(sequence) && F(command_list_object_id) && F(object_refs) && F(function_name) && F(payload); }

inline bool operator==(const B::TileMappingUpdateRecord &a, const B::TileMappingUpdateRecord &b)
{ return F(sequence) && F(queue_object_id) && F(resource_object_id) && F(heap_object_id) && F(payload); }

inline bool operator==(const D3D12QueueWait &a, const D3D12QueueWait &b)
{ return F(queue_object_id) && F(fence_object_id) && F(fence_value) && F(sequence); }

inline bool operator==(const D3D12SubmissionBatch &a, const D3D12SubmissionBatch &b)
{ return F(queue_object_id) && F(command_allocator_object_id) && F(command_list_ids) && F(descriptor_heap_ids)
      && F(waits_before_execute) && F(fence_object_id) && F(execute_sequence) && F(fence_sequence)
      && F(present_sequence) && F(present_frame_index) && F(fence_value) && F(presented); }

inline bool operator==(const D3D12TrackedObject &a, const D3D12TrackedObject &b)
{ return F(object_id) && F(kind) && F(parent_object_id) && F(debug_name); }

namespace {

// Hands out distinct non-default values so every populated field differs from its zero default.
// Any field that the serializer drops would reset to its default on load and fail equality.
struct Filler {
  std::uint64_t n = 1;
  std::uint64_t next() { return n++; }
  std::uint64_t u64() { return 0x100000 + next() * 7u; }
  std::uint32_t u32() { return static_cast<std::uint32_t>(0x1000 + next() * 3u); }
  std::int32_t i32() { return -static_cast<std::int32_t>(next() * 5u + 1u); }
  float f32() { return static_cast<float>(next()) + 0.25f; }
  bool b() { return true; } // all bool fields default false; true is always non-default
  std::string str(const char *prefix) { return std::string(prefix) + std::to_string(next()); }
  std::filesystem::path path(const char *prefix) { return std::filesystem::path(str(prefix)); }
  std::vector<std::uint32_t> u32_vec()
  {
    return {u32(), u32(), u32()};
  }
};

B::GpuVirtualAddressBinding make_gvab(Filler &f)
{
  B::GpuVirtualAddressBinding v;
  v.gpu_virtual_address = f.u64();
  v.resource_object_id = f.u64();
  v.offset = f.u64();
  v.resolved = false; // default is true; flip it
  return v;
}

B::DescriptorBinding make_binding(Filler &f)
{
  B::DescriptorBinding v;
  v.descriptor = f.u64();
  v.heap_object_id = f.u64();
  v.heap_type = f.u32();
  v.descriptor_index = f.u32();
  return v;
}

B::ViewportSemanticState make_viewport(Filler &f)
{
  B::ViewportSemanticState v;
  v.x = f.f32();
  v.y = f.f32();
  v.width = f.f32();
  v.height = f.f32();
  v.min_depth = f.f32();
  v.max_depth = f.f32();
  return v;
}

B::ScissorRectSemanticState make_scissor(Filler &f)
{
  B::ScissorRectSemanticState v;
  v.left = f.i32();
  v.top = f.i32();
  v.right = f.i32();
  v.bottom = f.i32();
  return v;
}

B::Root32BitConstantsBinding make_root_constants(Filler &f)
{
  B::Root32BitConstantsBinding v;
  v.root_parameter_index = f.u32();
  v.dst_offset = f.u32();
  v.values = f.u32_vec();
  return v;
}

B::VertexBufferBinding make_vertex_buffer(Filler &f)
{
  B::VertexBufferBinding v;
  v.sequence = f.u64();
  v.slot = f.u32();
  v.address = make_gvab(f);
  v.size_in_bytes = f.u32();
  v.stride_in_bytes = f.u32();
  return v;
}

B::ResourceTransition make_transition(Filler &f)
{
  B::ResourceTransition v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.flags = f.u32();
  v.before = f.u32();
  v.after = f.u32();
  v.subresource = f.u32();
  return v;
}

B::TextureCopyLocation make_copy_location(Filler &f)
{
  B::TextureCopyLocation v;
  v.resource_object_id = f.u64();
  v.type = f.u32();
  v.subresource_index = f.u32();
  v.footprint_offset = f.u64();
  v.footprint_format = f.u32();
  v.footprint_width = f.u32();
  v.footprint_height = f.u32();
  v.footprint_depth = f.u32();
  v.footprint_row_pitch = f.u32();
  return v;
}

template <typename Map, typename ValueFn>
Map make_u32_map(Filler &f, ValueFn make_value)
{
  Map m;
  m[f.u32()] = make_value(f);
  m[f.u32()] = make_value(f);
  return m;
}

B::TextureCopySemanticState make_texture_copy(Filler &f)
{
  B::TextureCopySemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.dst = make_copy_location(f);
  v.src = make_copy_location(f);
  v.dst_x = f.u32();
  v.dst_y = f.u32();
  v.dst_z = f.u32();
  v.has_src_box = f.b();
  v.src_box_left = f.u32();
  v.src_box_top = f.u32();
  v.src_box_front = f.u32();
  v.src_box_right = f.u32();
  v.src_box_bottom = f.u32();
  v.src_box_back = f.u32();
  return v;
}

B::BufferCopySemanticState make_buffer_copy(Filler &f)
{
  B::BufferCopySemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.dst_buffer_object_id = f.u64();
  v.src_buffer_object_id = f.u64();
  v.dst_offset = f.u64();
  v.src_offset = f.u64();
  v.byte_count = f.u64();
  return v;
}

B::ResourceCopySemanticState make_resource_copy(Filler &f)
{
  B::ResourceCopySemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.dst_resource_object_id = f.u64();
  v.src_resource_object_id = f.u64();
  return v;
}

B::ResourceBarrierSemanticState make_barrier(Filler &f)
{
  B::ResourceBarrierSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.type = f.u32();
  v.flags = f.u32();
  v.resource_object_id = f.u64();
  v.resource_before_object_id = f.u64();
  v.resource_after_object_id = f.u64();
  v.before = f.u32();
  v.after = f.u32();
  v.subresource = f.u32();
  return v;
}

B::ResolveSubresourceSemanticState make_resolve(Filler &f)
{
  B::ResolveSubresourceSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.dst_resource_object_id = f.u64();
  v.src_resource_object_id = f.u64();
  v.dst_subresource = f.u32();
  v.dst_x = f.u32();
  v.dst_y = f.u32();
  v.src_subresource = f.u32();
  v.has_src_rect = f.b();
  v.src_rect_left = f.i32();
  v.src_rect_top = f.i32();
  v.src_rect_right = f.i32();
  v.src_rect_bottom = f.i32();
  v.format = f.u32();
  v.mode = f.u32();
  return v;
}

B::QueryCommandSemanticState make_query(Filler &f)
{
  B::QueryCommandSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.query_heap_object_id = f.u64();
  v.type = f.u32();
  v.index = f.u32();
  v.start_index = f.u32();
  v.query_count = f.u32();
  v.dst_buffer_object_id = f.u64();
  v.aligned_dst_buffer_offset = f.u64();
  v.resolve = f.b();
  v.end = f.b();
  return v;
}

B::PredicationSemanticState make_predication(Filler &f)
{
  B::PredicationSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.buffer_object_id = f.u64();
  v.aligned_buffer_offset = f.u64();
  v.operation = f.u32();
  return v;
}

B::WriteBufferImmediateSemanticState make_write_buffer_immediate(Filler &f)
{
  B::WriteBufferImmediateSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  B::WriteBufferImmediateEntry entry;
  entry.dest = make_gvab(f);
  entry.value = f.u32();
  entry.mode = f.u32();
  v.writes.push_back(entry);
  return v;
}

B::RenderTargetClearSemanticState make_rtv_clear(Filler &f)
{
  B::RenderTargetClearSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.render_target = make_binding(f);
  v.color[0] = f.f32();
  v.color[1] = f.f32();
  v.color[2] = f.f32();
  v.color[3] = f.f32();
  v.rect_count = f.u32();
  v.has_color = f.b();
  v.rects.push_back(make_scissor(f));
  return v;
}

B::DepthStencilClearSemanticState make_dsv_clear(Filler &f)
{
  B::DepthStencilClearSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.depth_stencil = make_binding(f);
  v.depth = f.f32();
  v.clear_flags = f.u32();
  v.stencil = f.u32();
  v.rect_count = f.u32();
  v.rects.push_back(make_scissor(f));
  return v;
}

B::UnorderedAccessClearSemanticState make_uav_clear(Filler &f)
{
  B::UnorderedAccessClearSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.gpu_descriptor = make_binding(f);
  v.cpu_descriptor = make_binding(f);
  v.resource_object_id = f.u64();
  for (auto &value : v.uint_values) value = f.u32();
  for (auto &value : v.float_values) value = f.f32();
  v.integer = f.b();
  v.rect_count = f.u32();
  v.rects.push_back(make_scissor(f));
  return v;
}

B::DiscardResourceSemanticState make_discard(Filler &f)
{
  B::DiscardResourceSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.resource_object_id = f.u64();
  v.first_subresource = f.u32();
  v.subresource_count = f.u32();
  v.rect_count = f.u32();
  v.has_region = f.b();
  v.rects.push_back(make_scissor(f));
  return v;
}

B::ExecuteIndirectSemanticState make_indirect(Filler &f)
{
  B::ExecuteIndirectSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.command_signature_object_id = f.u64();
  v.arg_buffer_object_id = f.u64();
  v.count_buffer_object_id = f.u64();
  v.pipeline_state_object_id = f.u64();
  v.graphics_root_signature_object_id = f.u64();
  v.compute_root_signature_object_id = f.u64();
  v.graphics_root_tables[f.u32()] = make_binding(f);
  v.compute_root_tables[f.u32()] = make_binding(f);
  v.render_targets.push_back(make_binding(f));
  v.depth_stencil = make_binding(f);
  v.viewports.push_back(make_viewport(f));
  v.scissor_rects.push_back(make_scissor(f));
  v.primitive_topology = f.u32();
  v.max_command_count = f.u32();
  v.arg_buffer_offset = f.u64();
  v.count_buffer_offset = f.u64();
  return v;
}

B::ExecuteBundleSemanticState make_bundle_execute(Filler &f)
{
  B::ExecuteBundleSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.bundle_command_list_object_id = f.u64();
  return v;
}

B::DispatchSemanticState make_dispatch(Filler &f)
{
  B::DispatchSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.pipeline_state_object_id = f.u64();
  v.graphics_root_signature_object_id = f.u64();
  v.compute_root_signature_object_id = f.u64();
  v.graphics_root_tables[f.u32()] = make_binding(f);
  v.compute_root_tables[f.u32()] = make_binding(f);
  v.thread_group_count_x = f.u32();
  v.thread_group_count_y = f.u32();
  v.thread_group_count_z = f.u32();
  v.mesh = f.b();
  return v;
}

B::DispatchRaysSemanticState make_dispatch_rays(Filler &f)
{
  B::DispatchRaysSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.width = f.u32();
  v.height = f.u32();
  v.depth = f.u32();
  v.ray_generation_shader_record = make_gvab(f);
  v.ray_generation_shader_record_size = f.u64();
  v.miss_shader_table = make_gvab(f);
  v.miss_shader_table_size = f.u64();
  v.miss_shader_table_stride = f.u64();
  v.hit_group_table = make_gvab(f);
  v.hit_group_table_size = f.u64();
  v.hit_group_table_stride = f.u64();
  v.callable_shader_table = make_gvab(f);
  v.callable_shader_table_size = f.u64();
  v.callable_shader_table_stride = f.u64();
  return v;
}

B::TemporalUpscaleSemanticState make_temporal_upscale(Filler &f)
{
  B::TemporalUpscaleSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.input_content_width = f.u32();
  v.input_content_height = f.u32();
  v.auto_exposure = f.b();
  v.in_reset = f.b();
  v.depth_reversed = f.b();
  v.motion_vector_in_display_res = f.b();
  v.color_object_id = f.u64();
  v.depth_object_id = f.u64();
  v.motion_vector_object_id = f.u64();
  v.output_object_id = f.u64();
  v.exposure_texture_object_id = f.u64();
  v.motion_vector_scale_x = f.f32();
  v.motion_vector_scale_y = f.f32();
  v.pre_exposure = f.f32();
  v.jitter_offset_x = f.f32();
  v.jitter_offset_y = f.f32();
  return v;
}

B::DrawSemanticState make_draw(Filler &f)
{
  B::DrawSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.pipeline_state_object_id = f.u64();
  v.graphics_root_signature_object_id = f.u64();
  v.graphics_root_tables[f.u32()] = make_binding(f);
  v.render_targets.push_back(make_binding(f));
  v.depth_stencil = make_binding(f);
  v.viewports.push_back(make_viewport(f));
  v.scissor_rects.push_back(make_scissor(f));
  v.primitive_topology = f.u32();
  v.vertex_count_per_instance = f.u32();
  v.instance_count = f.u32();
  v.start_vertex_location = f.u32();
  v.start_instance_location = f.u32();
  return v;
}

B::DrawIndexedSemanticState make_draw_indexed(Filler &f)
{
  B::DrawIndexedSemanticState v;
  v.sequence = f.u64();
  v.command_list_object_id = f.u64();
  v.pipeline_state_object_id = f.u64();
  v.graphics_root_signature_object_id = f.u64();
  v.graphics_root_tables[f.u32()] = make_binding(f);
  v.render_targets.push_back(make_binding(f));
  v.depth_stencil = make_binding(f);
  v.viewports.push_back(make_viewport(f));
  v.scissor_rects.push_back(make_scissor(f));
  v.primitive_topology = f.u32();
  v.index_count_per_instance = f.u32();
  v.instance_count = f.u32();
  v.start_index_location = f.u32();
  v.base_vertex_location = f.i32();
  v.start_instance_location = f.u32();
  return v;
}

B::DescriptorSemanticState make_descriptor(Filler &f)
{
  B::DescriptorSemanticState v;
  v.kind = f.str("srv");
  v.create_sequence = f.u64();
  v.resource_object_id = f.u64();
  v.counter_resource_object_id = f.u64();
  v.descriptor = f.u64();
  v.binding = make_binding(f);
  v.format = f.u32();
  v.view_dimension = f.u32();
  v.flags = f.u32();
  v.shader_4_component_mapping = f.u32();
  v.buffer_location = f.u64();
  v.size_in_bytes = f.u32();
  v.first_element = f.u64();
  v.num_elements = f.u32();
  v.structure_byte_stride = f.u32();
  v.counter_offset_in_bytes = f.u64();
  v.has_view_desc = f.b();
  v.most_detailed_mip = f.u32();
  v.mip_levels = f.u32();
  v.mip_slice = f.u32();
  v.plane_slice = f.u32();
  v.first_array_slice = f.u32();
  v.array_size = f.u32();
  v.first_2d_array_face = f.u32();
  v.num_cubes = f.u32();
  v.first_w_slice = f.u32();
  v.w_size = f.u32();
  v.resource_min_lod_clamp = f.f32();
  v.raytracing_acceleration_structure = make_gvab(f);
  v.buffer = make_gvab(f);
  return v;
}

B::SamplerSemanticState make_sampler(Filler &f)
{
  B::SamplerSemanticState v;
  v.create_sequence = f.u64();
  v.descriptor = f.u64();
  v.binding = make_binding(f);
  v.filter = f.u32();
  v.address_u = f.u32();
  v.address_v = f.u32();
  v.address_w = f.u32();
  v.mip_lod_bias = f.f32();
  v.max_anisotropy = f.u32();
  v.comparison_func = f.u32();
  for (auto &value : v.border_color) value = f.f32();
  v.min_lod = f.f32();
  v.max_lod = f.f32();
  v.has_desc = f.b();
  return v;
}

B::CopyDescriptorSemanticState make_copy_descriptor(Filler &f)
{
  B::CopyDescriptorSemanticState v;
  v.create_sequence = f.u64();
  v.descriptor_heap_type = f.u32();
  B::CopyDescriptorPair pair;
  pair.dst_descriptor = f.u64();
  pair.src_descriptor = f.u64();
  v.descriptors.push_back(pair);
  return v;
}

B::DeviceSemanticState make_device(Filler &f)
{
  B::DeviceSemanticState v;
  v.device_object_id = f.u64();
  v.create_sequence = f.u64();
  v.minimum_feature_level = f.u32();
  return v;
}

B::CommandQueueSemanticState make_queue(Filler &f)
{
  B::CommandQueueSemanticState v;
  v.queue_object_id = f.u64();
  v.create_sequence = f.u64();
  v.type = f.u32();
  v.priority = f.i32();
  v.flags = f.u32();
  v.node_mask = f.u32();
  v.execute_count = f.u64();
  v.last_execute_sequence = f.u64();
  return v;
}

B::CommandAllocatorSemanticState make_allocator(Filler &f)
{
  B::CommandAllocatorSemanticState v;
  v.allocator_object_id = f.u64();
  v.create_sequence = f.u64();
  v.type = f.u32();
  v.reset_count = f.u64();
  v.last_reset_sequence = f.u64();
  v.last_submit_sequence = f.u64();
  v.last_submit_fence_object_id = f.u64();
  v.last_submit_fence_value = f.u64();
  return v;
}

B::HeapSemanticState make_heap(Filler &f)
{
  B::HeapSemanticState v;
  v.heap_object_id = f.u64();
  v.create_sequence = f.u64();
  v.size_in_bytes = f.u64();
  v.alignment = f.u64();
  v.heap_type = f.u32();
  v.cpu_page_property = f.u32();
  v.memory_pool_preference = f.u32();
  v.creation_node_mask = f.u32();
  v.visible_node_mask = f.u32();
  v.flags = f.u32();
  return v;
}

B::QueryHeapSemanticState make_query_heap(Filler &f)
{
  B::QueryHeapSemanticState v;
  v.query_heap_object_id = f.u64();
  v.create_sequence = f.u64();
  v.type = f.u32();
  v.count = f.u32();
  v.node_mask = f.u32();
  return v;
}

B::DescriptorHeapSemanticState make_descriptor_heap(Filler &f)
{
  B::DescriptorHeapSemanticState v;
  v.heap_object_id = f.u64();
  v.create_sequence = f.u64();
  v.type = f.u32();
  v.num_descriptors = f.u32();
  v.flags = f.u32();
  v.node_mask = f.u32();
  v.descriptor_size = f.u32();
  v.cpu_start = f.u64();
  v.gpu_start = f.u64();
  return v;
}

B::CommandSignatureSemanticState make_command_signature(Filler &f)
{
  B::CommandSignatureSemanticState v;
  v.command_signature_object_id = f.u64();
  v.root_signature_object_id = f.u64();
  v.create_sequence = f.u64();
  v.byte_stride = f.u32();
  v.argument_count = f.u32();
  v.node_mask = f.u32();
  B::CommandSignatureArgument arg;
  arg.type = f.u32();
  arg.slot = f.u32();
  arg.root_parameter_index = f.u32();
  arg.dest_offset_in32bit_values = f.u32();
  arg.num32bit_values_to_set = f.u32();
  v.arguments.push_back(arg);
  return v;
}

B::FenceSemanticState make_fence(Filler &f)
{
  B::FenceSemanticState v;
  v.fence_object_id = f.u64();
  v.create_sequence = f.u64();
  v.initial_value = f.u64();
  v.flags = f.u32();
  v.current_value = f.u64();
  v.completed_value = f.u64();
  v.last_signal_sequence = f.u64();
  v.last_wait_sequence = f.u64();
  v.last_event_sequence = f.u64();
  return v;
}

B::FenceOperationSemanticState make_fence_op(Filler &f)
{
  B::FenceOperationSemanticState v;
  v.sequence = f.u64();
  v.queue_object_id = f.u64();
  v.fence_object_id = f.u64();
  v.fence_value = f.u64();
  v.queue_operation = f.b();
  v.wait_operation = f.b();
  v.event_operation = f.b();
  return v;
}

B::PipelineSemanticState make_pipeline(Filler &f)
{
  B::PipelineSemanticState v;
  v.pipeline_state_object_id = f.u64();
  v.root_signature_object_id = f.u64();
  v.create_sequence = f.u64();
  v.graphics = f.b();
  v.relative_path = f.path("pipelines/p");
  v.blob_refs = {f.u64(), f.u64()};
  v.uses_embedded_root_signature = f.b();
  v.node_mask = f.u32();
  v.flags = f.u32();
  v.sample_mask = f.u32();
  v.primitive_topology_type = f.u32();
  v.num_render_targets = f.u32();
  v.dsv_format = f.u32();
  v.sample_count = f.u32();
  v.sample_quality = f.u32();
  v.ib_strip_cut_value = f.u32();
  v.input_element_count = f.u32();
  v.has_input_layout = f.b();
  v.has_blend_state = f.b();
  v.has_rasterizer_state = f.b();
  v.has_depth_stencil_state = f.b();
  v.rtv_formats = {f.u32(), f.u32()};
  v.has_vertex_shader = f.b();
  v.has_pixel_shader = f.b();
  v.has_domain_shader = f.b();
  v.has_hull_shader = f.b();
  v.has_geometry_shader = f.b();
  v.has_compute_shader = f.b();
  v.requires_dxmt_backend = f.b();
  return v;
}

B::RootSignatureSemanticState make_root_signature(Filler &f)
{
  B::RootSignatureSemanticState v;
  v.root_signature_object_id = f.u64();
  v.create_sequence = f.u64();
  v.node_mask = f.u32();
  v.bytecode_size = f.u64();
  v.relative_path = f.path("rootsig/r");
  v.blob_refs = {f.u64()};
  v.bytes = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
  B::RootDescriptorTableSemanticState table;
  table.root_parameter_index = f.u32();
  table.shader_visibility = f.u32();
  B::RootDescriptorRangeSemanticState range;
  range.type = f.u32();
  range.descriptor_count = f.u32();
  range.base_shader_register = f.u32();
  range.register_space = f.u32();
  range.offset_from_table_start = f.u32();
  range.flags = f.u32();
  table.ranges.push_back(range);
  v.descriptor_tables.push_back(table);
  v.has_descriptor_table_metadata = f.b();
  B::RootParameterSemanticState param;
  param.root_parameter_index = f.u32();
  param.parameter_type = f.u32();
  param.shader_visibility = f.u32();
  param.shader_register = f.u32();
  param.register_space = f.u32();
  param.num_32bit_values = f.u32();
  param.range_count = f.u32();
  param.flags = f.u32();
  v.root_parameters.push_back(param);
  v.has_root_parameter_metadata = f.b();
  return v;
}

B::ResourceSemanticState make_resource(Filler &f)
{
  B::ResourceSemanticState v;
  v.resource_object_id = f.u64();
  v.heap_object_id = f.u64();
  v.create_sequence = f.u64();
  v.heap_type = f.u32();
  v.heap_flags = f.u32();
  v.initial_state = f.u32();
  v.current_state = f.u32();
  v.heap_offset = f.u64();
  v.last_transition_sequence = f.u64();
  v.gpu_virtual_address = f.u64();
  v.width = f.u64();
  v.alignment = f.u64();
  v.dimension = f.u32();
  v.height = f.u32();
  v.depth_or_array_size = f.u32();
  v.mip_levels = f.u32();
  v.recorded_mip_levels = f.u32();
  v.format = f.u32();
  v.sample_count = f.u32();
  v.sample_quality = f.u32();
  v.layout = f.u32();
  v.flags = f.u32();
  v.has_optimized_clear_value = f.b();
  v.reserved_resource = f.b();
  v.swapchain_back_buffer = f.b();
  v.external_resource = f.b();
  v.swapchain_buffer_index = f.u32();
  v.optimized_clear_format = f.u32();
  for (auto &value : v.optimized_clear_color) value = f.f32();
  v.optimized_clear_depth = f.f32();
  v.optimized_clear_stencil = f.u32();
  v.mapped = f.b();
  v.mapped_subresource = f.u32();
  v.map_sequence = f.u64();
  v.transitions.push_back(make_transition(f));
  v.subresource_states[f.u32()] = f.u32();
  B::ResourceDataUpdate update;
  update.sequence = f.u64();
  update.subresource = f.u32();
  update.written_begin = f.u64();
  update.written_end = f.u64();
  update.relative_path = f.path("buffers/b");
  update.blob_refs = {f.u64(), f.u64()};
  update.bytes = {0x11, 0x22, 0x33};
  v.data_updates.push_back(update);
  return v;
}

B::CommandListSemanticState make_command_list(Filler &f)
{
  B::CommandListSemanticState v;
  v.recording = f.b();
  v.type = f.u32();
  v.node_mask = f.u32();
  v.allocator_object_id = f.u64();
  v.pipeline_state_object_id = f.u64();
  v.graphics_root_signature_object_id = f.u64();
  v.compute_root_signature_object_id = f.u64();
  v.descriptor_heap_count = f.u64();
  v.render_target_count = f.u64();
  v.barrier_count = f.u64();
  v.clear_count = f.u64();
  v.copy_count = f.u64();
  v.draw_count = f.u64();
  v.dispatch_count = f.u64();
  v.first_sequence = f.u64();
  v.close_sequence = f.u64();
  v.primitive_topology = f.u32();
  v.descriptor_heap_object_ids = {f.u64(), f.u64()};
  v.viewports.push_back(make_viewport(f));
  v.scissor_rects.push_back(make_scissor(f));
  v.render_targets.push_back(make_binding(f));
  v.depth_stencil = make_binding(f);
  v.graphics_root_tables[f.u32()] = make_binding(f);
  v.compute_root_tables[f.u32()] = make_binding(f);
  v.graphics_root_constants[f.u32()] = make_root_constants(f);
  v.compute_root_constants[f.u32()] = make_root_constants(f);
  v.graphics_root_constant_buffers[f.u32()] = make_gvab(f);
  v.compute_root_constant_buffers[f.u32()] = make_gvab(f);
  v.graphics_root_shader_resources[f.u32()] = make_gvab(f);
  v.compute_root_shader_resources[f.u32()] = make_gvab(f);
  v.graphics_root_unordered_accesses[f.u32()] = make_gvab(f);
  v.compute_root_unordered_accesses[f.u32()] = make_gvab(f);
  v.vertex_buffers.push_back(make_vertex_buffer(f));
  v.index_buffer.sequence = f.u64();
  v.index_buffer.address = make_gvab(f);
  v.index_buffer.size_in_bytes = f.u32();
  v.index_buffer.format = f.u32();
  v.buffer_copies.push_back(make_buffer_copy(f));
  v.texture_copies.push_back(make_texture_copy(f));
  v.resource_copies.push_back(make_resource_copy(f));
  v.barriers.push_back(make_barrier(f));
  v.resolves.push_back(make_resolve(f));
  v.queries.push_back(make_query(f));
  v.predications.push_back(make_predication(f));
  v.write_buffer_immediates.push_back(make_write_buffer_immediate(f));
  v.render_target_clears.push_back(make_rtv_clear(f));
  v.depth_stencil_clears.push_back(make_dsv_clear(f));
  v.unordered_access_clears.push_back(make_uav_clear(f));
  v.discards.push_back(make_discard(f));
  v.indirect_executes.push_back(make_indirect(f));
  v.bundle_executes.push_back(make_bundle_execute(f));
  v.draws.push_back(make_draw(f));
  v.indexed_draws.push_back(make_draw_indexed(f));
  v.dispatches.push_back(make_dispatch(f));
  v.ray_dispatches.push_back(make_dispatch_rays(f));
  v.temporal_upscales.push_back(make_temporal_upscale(f));
  v.command_indices = {f.u64(), f.u64(), f.u64()};
  return v;
}

} // namespace

// Friend hook: populates every private container of a backend and compares two backends field by
// field. Declared as a friend of D3D12ReplayBackend via APITRACE_ENABLE_TEST_HOOKS.
struct D3D12ReplayBackendTestHook {
  static void populate(B &backend)
  {
    Filler f;

    backend.initialized_ = true;
    backend.commands_replayed_ = f.u64();
    backend.frames_seen_ = f.u64();
    backend.presents_seen_ = f.u64();
    backend.pipeline_assets_read_ = f.u64();
    backend.semantic_calls_seen_ = f.u64();
    backend.draw_calls_seen_ = f.u64();
    backend.dispatch_calls_seen_ = f.u64();
    backend.last_sequence_ = f.u64();
    backend.bundle_root_ = f.path("bundle/root");

    backend.blob_paths_[f.u64()] = f.path("shaders/s");
    backend.blob_paths_[f.u64()] = f.path("textures/t");

    // Object registry.
    for (int i = 0; i < 2; ++i) {
      D3D12TrackedObject obj;
      obj.object_id = f.u64();
      obj.kind = trace::ObjectKind::Resource;
      obj.parent_object_id = f.u64();
      obj.debug_name = f.str("obj");
      backend.objects_.track(obj);
    }

    // Submission tracker.
    {
      std::vector<D3D12SubmissionBatch> completed;
      D3D12SubmissionBatch batch;
      batch.queue_object_id = f.u64();
      batch.command_allocator_object_id = f.u64();
      batch.command_list_ids = {f.u64(), f.u64()};
      batch.descriptor_heap_ids = {f.u64()};
      D3D12QueueWait wait;
      wait.queue_object_id = f.u64();
      wait.fence_object_id = f.u64();
      wait.fence_value = f.u64();
      wait.sequence = f.u64();
      batch.waits_before_execute.push_back(wait);
      batch.fence_object_id = f.u64();
      batch.execute_sequence = f.u64();
      batch.fence_sequence = f.u64();
      batch.present_sequence = f.u64();
      batch.present_frame_index = f.u64();
      batch.fence_value = f.u64();
      batch.presented = f.b();
      completed.push_back(batch);

      D3D12SubmissionBatch current;
      current.queue_object_id = f.u64();
      current.command_allocator_object_id = f.u64();
      current.execute_sequence = f.u64();

      std::vector<D3D12QueueWait> pending;
      D3D12QueueWait pending_wait;
      pending_wait.queue_object_id = f.u64();
      pending_wait.fence_object_id = f.u64();
      pending_wait.fence_value = f.u64();
      pending_wait.sequence = f.u64();
      pending.push_back(pending_wait);

      backend.submissions_.restore(completed, current, pending, true);
    }

    backend.present_frames_[f.u64()] = [&] {
      B::PresentFrame v;
      v.relative_path = f.path("frames/f");
      v.width = f.u32();
      v.height = f.u32();
      v.row_pitch = f.u32();
      v.sync_interval = f.u32();
      v.flags = f.u32();
      v.frame_index = f.u64();
      return v;
    }();
    backend.present_semantics_[f.u64()] = [&] {
      B::PresentSemanticState v;
      v.frame_index = f.u64();
      v.call_sequence = f.u64();
      v.boundary_sequence = f.u64();
      v.sync_interval = f.u32();
      v.flags = f.u32();
      v.result_code = f.i32();
      v.has_call = f.b();
      v.has_boundary = f.b();
      return v;
    }();
    backend.frame_semantics_[f.u64()] = [&] {
      B::FrameSemanticState v;
      v.frame_index = f.u64();
      v.begin_sequence = f.u64();
      v.end_sequence = f.u64();
      v.has_begin = f.b();
      v.has_present = f.b();
      v.has_end = f.b();
      return v;
    }();

    backend.command_lists_[f.u64()] = make_command_list(f);
    backend.command_queues_[f.u64()] = make_queue(f);
    backend.command_allocators_[f.u64()] = make_allocator(f);
    backend.devices_[f.u64()] = make_device(f);
    backend.heaps_[f.u64()] = make_heap(f);
    backend.query_heaps_[f.u64()] = make_query_heap(f);
    backend.descriptor_heaps_[f.u64()] = make_descriptor_heap(f);
    backend.command_signatures_[f.u64()] = make_command_signature(f);
    backend.fences_[f.u64()] = make_fence(f);
    backend.resources_[f.u64()] = make_resource(f);
    // Append-only resource history: include duplicate object_ids with different formats to exercise
    // the pointer-reuse case the versioned resolver depends on.
    {
      B::ResourceSemanticState ver_a = make_resource(f);
      ver_a.resource_object_id = 0x4000;
      ver_a.create_sequence = 10;
      ver_a.format = 71;  // DXGI_FORMAT_R8G8B8A8_UNORM
      backend.resource_versions_.push_back(ver_a);
      B::ResourceSemanticState ver_b = make_resource(f);
      ver_b.resource_object_id = 0x4000;  // same object_id, later lifetime
      ver_b.create_sequence = 250;
      ver_b.format = 2;  // DXGI_FORMAT_R32G32B32A32_FLOAT
      backend.resource_versions_.push_back(ver_b);
    }
    backend.pipelines_[f.u64()] = make_pipeline(f);
    backend.root_signatures_[f.u64()] = make_root_signature(f);

    backend.descriptors_.push_back(make_descriptor(f));
    backend.samplers_.push_back(make_sampler(f));
    backend.descriptor_copies_.push_back(make_copy_descriptor(f));
    backend.fence_operations_.push_back(make_fence_op(f));

    {
      B::ReplayCommandRecord record;
      record.kind = B::ReplayCommandKind::Draw;
      record.sequence = f.u64();
      record.command_list_object_id = f.u64();
      record.object_refs = {f.u64(), f.u64()};
      record.function_name = f.str("ID3D12GraphicsCommandList::DrawInstanced");
      record.payload = f.str("{\"payload\":");
      backend.replay_commands_.push_back(record);
    }
    {
      B::TileMappingUpdateRecord record;
      record.sequence = f.u64();
      record.queue_object_id = f.u64();
      record.resource_object_id = f.u64();
      record.heap_object_id = f.u64();
      record.payload = f.str("{\"tile\":");
      backend.tile_mapping_updates_.push_back(record);
    }
  }

  // Returns "" when the two backends match, otherwise a description of the first divergence.
  static std::string compare(const B &a, const B &b)
  {
#define CHECK(cond, label)                                                                         \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      return std::string("mismatch: ") + (label);                                                  \
    }                                                                                              \
  } while (0)

    CHECK(a.initialized_ == b.initialized_, "initialized_");
    CHECK(b.initialized_, "loaded backend not initialized");
    CHECK(a.commands_replayed_ == b.commands_replayed_, "commands_replayed_");
    CHECK(a.frames_seen_ == b.frames_seen_, "frames_seen_");
    CHECK(a.presents_seen_ == b.presents_seen_, "presents_seen_");
    CHECK(a.pipeline_assets_read_ == b.pipeline_assets_read_, "pipeline_assets_read_");
    CHECK(a.semantic_calls_seen_ == b.semantic_calls_seen_, "semantic_calls_seen_");
    CHECK(a.draw_calls_seen_ == b.draw_calls_seen_, "draw_calls_seen_");
    CHECK(a.dispatch_calls_seen_ == b.dispatch_calls_seen_, "dispatch_calls_seen_");
    CHECK(a.last_sequence_ == b.last_sequence_, "last_sequence_");
    CHECK(a.bundle_root_ == b.bundle_root_, "bundle_root_");
    CHECK(a.blob_paths_ == b.blob_paths_, "blob_paths_");

    CHECK(a.objects_.tracked_objects() == b.objects_.tracked_objects(), "objects_");

    CHECK(a.submissions_.completed_batches() == b.submissions_.completed_batches(), "submissions_.completed_batches");
    CHECK(a.submissions_.has_open_batch() == b.submissions_.has_open_batch(), "submissions_.has_open_batch");
    CHECK(a.submissions_.current_batch() == b.submissions_.current_batch(), "submissions_.current_batch");
    CHECK(a.submissions_.pending_waits() == b.submissions_.pending_waits(), "submissions_.pending_waits");

    CHECK(a.present_frames_ == b.present_frames_, "present_frames_");
    CHECK(a.present_semantics_ == b.present_semantics_, "present_semantics_");
    CHECK(a.frame_semantics_ == b.frame_semantics_, "frame_semantics_");
    CHECK(a.command_lists_ == b.command_lists_, "command_lists_");
    CHECK(a.command_queues_ == b.command_queues_, "command_queues_");
    CHECK(a.command_allocators_ == b.command_allocators_, "command_allocators_");
    CHECK(a.devices_ == b.devices_, "devices_");
    CHECK(a.heaps_ == b.heaps_, "heaps_");
    CHECK(a.query_heaps_ == b.query_heaps_, "query_heaps_");
    CHECK(a.descriptor_heaps_ == b.descriptor_heaps_, "descriptor_heaps_");
    CHECK(a.descriptors_ == b.descriptors_, "descriptors_");
    CHECK(a.samplers_ == b.samplers_, "samplers_");
    CHECK(a.descriptor_copies_ == b.descriptor_copies_, "descriptor_copies_");
    CHECK(a.command_signatures_ == b.command_signatures_, "command_signatures_");
    CHECK(a.fences_ == b.fences_, "fences_");
    CHECK(a.resources_ == b.resources_, "resources_");
    CHECK(a.resource_versions_ == b.resource_versions_, "resource_versions_");
    CHECK(a.pipelines_ == b.pipelines_, "pipelines_");
    CHECK(a.root_signatures_ == b.root_signatures_, "root_signatures_");
    CHECK(a.fence_operations_ == b.fence_operations_, "fence_operations_");
    CHECK(a.replay_commands_ == b.replay_commands_, "replay_commands_");
    CHECK(a.tile_mapping_updates_ == b.tile_mapping_updates_, "tile_mapping_updates_");

#undef CHECK
    return std::string();
  }
};

} // namespace apitrace::d3d12

namespace {

std::string read_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

} // namespace

int main()
{
  namespace fs = std::filesystem;
  using apitrace::d3d12::D3D12ReplayBackend;
  using apitrace::d3d12::D3D12ReplayBackendTestHook;

  const fs::path temp_dir = fs::temp_directory_path() / "apitrace_replay_model_roundtrip";
  fs::remove_all(temp_dir);
  fs::create_directories(temp_dir);
  const fs::path json_a = temp_dir / "model_a.json";
  const fs::path blob_a = temp_dir / "model_a.bin";
  const fs::path json_b = temp_dir / "model_b.json";
  const fs::path blob_b = temp_dir / "model_b.bin";
  const std::string bundle_hash = "sha256:roundtrip-test";

  // 1. Populate a backend so every container holds non-default data.
  D3D12ReplayBackend backend_a;
  D3D12ReplayBackendTestHook::populate(backend_a);

  // 2. Save it.
  std::string error;
  if (!backend_a.save_replay_model(json_a, blob_a, bundle_hash, error)) {
    std::cerr << "save_replay_model failed: " << error << "\n";
    return 1;
  }

  // 3. Load into a second backend.
  D3D12ReplayBackend backend_b;
  if (!backend_b.load_replay_model(json_a, blob_a, bundle_hash, error)) {
    std::cerr << "load_replay_model failed: " << error << "\n";
    return 1;
  }

  // 4. Field-by-field equality across all containers (catches dropped fields).
  const std::string diff = D3D12ReplayBackendTestHook::compare(backend_a, backend_b);
  if (!diff.empty()) {
    std::cerr << diff << "\n";
    return 1;
  }

  // 5. Re-save the loaded backend and require byte-identical output (catches written-but-not-read
  //    fields and confirms deterministic serialization).
  if (!backend_b.save_replay_model(json_b, blob_b, bundle_hash, error)) {
    std::cerr << "re-save failed: " << error << "\n";
    return 1;
  }
  if (read_file(json_a) != read_file(json_b)) {
    std::cerr << "json output differs after round-trip\n";
    return 1;
  }
  if (read_file(blob_a) != read_file(blob_b)) {
    std::cerr << "blob output differs after round-trip\n";
    return 1;
  }

  // 6. Staleness guard: a mismatched expected hash must fail.
  D3D12ReplayBackend backend_c;
  if (backend_c.load_replay_model(json_a, blob_a, "sha256:wrong-hash", error)) {
    std::cerr << "load_replay_model accepted a stale hash\n";
    return 1;
  }

  fs::remove_all(temp_dir);
  std::cout << "d3d12 replay-model round-trip OK\n";
  return 0;
}




