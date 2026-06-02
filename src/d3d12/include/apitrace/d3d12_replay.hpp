#pragma once

#include "apitrace/event_types.hpp"
#include "apitrace/d3d12_state.hpp"
#include "apitrace/d3d12_submission.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace apitrace::d3d12 {

class D3D12NativeReplayer;

class D3D12ReplayBackend {
public:
  struct DescriptorSemanticState;

  D3D12ReplayBackend();
  ~D3D12ReplayBackend();

  bool initialize(const trace::TraceBundleReader &reader);
  bool replay_event(const trace::EventRecord &event);
  bool finalize_replay();
  bool validate_only();
  void shutdown();

  const std::string &last_error() const noexcept;
  const std::vector<DescriptorSemanticState> &descriptors() const noexcept;
  enum class ReplayCommandKind {
    Unknown,
    BeginCommandList,
    EndCommandList,
    SetPipelineState,
    SetRootSignature,
    SetDescriptorHeaps,
    SetRootDescriptorTable,
    SetRootConstants,
    SetRootConstantBufferView,
    SetViewports,
    SetScissorRects,
    SetRenderTargets,
    ClearRenderTarget,
    ClearDepthStencil,
    ClearUnorderedAccess,
    DiscardResource,
    SetPrimitiveTopology,
    SetVertexBuffers,
    SetIndexBuffer,
    ResourceBarrier,
    SetDynamicState,
    RenderPass,
    Query,
    Predication,
    WriteBufferImmediate,
    TemporalUpscale,
    UnsupportedNative,
    Draw,
    Dispatch,
    ExecuteIndirect,
    ExecuteBundle,
    Copy,
    Resolve,
    MapResource,
    UnmapResource,
  };

  struct ReplayCommandRecord {
    ReplayCommandKind kind = ReplayCommandKind::Unknown;
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::vector<trace::ObjectId> object_refs;
    std::string function_name;
    std::string payload;
  };

  std::string last_error_;
  bool initialized_ = false;
  std::uint64_t commands_replayed_ = 0;
  std::uint64_t frames_seen_ = 0;
  std::uint64_t presents_seen_ = 0;
  std::uint64_t pipeline_assets_read_ = 0;
  std::uint64_t semantic_calls_seen_ = 0;
  std::uint64_t draw_calls_seen_ = 0;
  std::uint64_t dispatch_calls_seen_ = 0;
  std::uint64_t last_sequence_ = 0;
  std::unordered_map<trace::BlobId, std::filesystem::path> blob_paths_;
  D3D12ObjectRegistry objects_;
  D3D12SubmissionTracker submissions_;
  std::filesystem::path bundle_root_;

  struct PresentFrame {
    std::filesystem::path relative_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::uint32_t sync_interval = 1;
    std::uint32_t flags = 0;
    std::uint64_t frame_index = 0;
  };

  struct PresentSemanticState {
    std::uint64_t frame_index = 0;
    std::uint64_t call_sequence = 0;
    std::uint64_t boundary_sequence = 0;
    std::uint32_t sync_interval = 0;
    std::uint32_t flags = 0;
    std::int32_t result_code = 0;
    bool has_call = false;
    bool has_boundary = false;
  };

  struct FrameSemanticState {
    std::uint64_t frame_index = 0;
    std::uint64_t begin_sequence = 0;
    std::uint64_t end_sequence = 0;
    bool has_begin = false;
    bool has_present = false;
    bool has_end = false;
  };

  std::unordered_map<std::uint64_t, PresentFrame> present_frames_;
  std::unordered_map<std::uint64_t, PresentSemanticState> present_semantics_;
  std::unordered_map<std::uint64_t, FrameSemanticState> frame_semantics_;
  bool index_present_frame_event(const trace::EventRecord &event, bool replace_existing);
  bool index_present_frames(const trace::TraceBundleReader &reader);
  bool index_present_semantics(const trace::TraceBundleReader &reader);
  bool validate_frame_boundaries(const trace::TraceBundleReader &reader);
  bool validate_present_boundaries(const trace::TraceBundleReader &reader);
  bool validate_present_semantic_match(
      const trace::EventRecord &event,
      std::uint64_t frame_index,
      std::uint32_t sync_interval,
      std::uint32_t flags);
  bool validate_replay_closure();
  bool validate_native_replay_readiness();
  const std::vector<ReplayCommandRecord> &replay_commands() const noexcept;

  struct ResourceDataUpdate {
    std::uint64_t sequence = 0;
    std::uint32_t subresource = 0;
    std::uint64_t written_begin = 0;
    std::uint64_t written_end = 0;
    std::filesystem::path relative_path;
    std::vector<trace::BlobId> blob_refs;
    std::vector<std::uint8_t> bytes;
  };

  struct DeviceSemanticState {
    trace::ObjectId device_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t minimum_feature_level = 0;
  };

  struct ResourceTransition {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::uint32_t flags = 0;
    std::uint32_t before = 0;
    std::uint32_t after = 0;
    std::uint32_t subresource = 0;
  };

  struct ResourceBarrierSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    trace::ObjectId resource_object_id = 0;
    trace::ObjectId resource_before_object_id = 0;
    trace::ObjectId resource_after_object_id = 0;
    std::uint32_t before = 0;
    std::uint32_t after = 0;
    std::uint32_t subresource = 0;
  };

  struct GpuVirtualAddressBinding {
    std::uint64_t gpu_virtual_address = 0;
    trace::ObjectId resource_object_id = 0;
    std::uint64_t offset = 0;
    bool resolved = true;
  };

  struct Root32BitConstantsBinding {
    std::uint32_t root_parameter_index = 0;
    std::uint32_t dst_offset = 0;
    std::vector<std::uint32_t> values;
  };

  struct VertexBufferBinding {
    std::uint64_t sequence = 0;
    std::uint32_t slot = 0;
    GpuVirtualAddressBinding address;
    std::uint32_t size_in_bytes = 0;
    std::uint32_t stride_in_bytes = 0;
  };

  struct IndexBufferBinding {
    std::uint64_t sequence = 0;
    GpuVirtualAddressBinding address;
    std::uint32_t size_in_bytes = 0;
    std::uint32_t format = 0;
  };

  struct ViewportSemanticState {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 0.0f;
  };

  struct ScissorRectSemanticState {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
  };

  struct TextureCopyLocation {
    trace::ObjectId resource_object_id = 0;
    std::uint32_t type = 0;
    std::uint32_t subresource_index = 0;
    std::uint64_t footprint_offset = 0;
    std::uint32_t footprint_format = 0;
    std::uint32_t footprint_width = 0;
    std::uint32_t footprint_height = 0;
    std::uint32_t footprint_depth = 0;
    std::uint32_t footprint_row_pitch = 0;
  };

  struct TextureCopySemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    TextureCopyLocation dst;
    TextureCopyLocation src;
    std::uint32_t dst_x = 0;
    std::uint32_t dst_y = 0;
    std::uint32_t dst_z = 0;
    bool has_src_box = false;
    std::uint32_t src_box_left = 0;
    std::uint32_t src_box_top = 0;
    std::uint32_t src_box_front = 0;
    std::uint32_t src_box_right = 0;
    std::uint32_t src_box_bottom = 0;
    std::uint32_t src_box_back = 0;
  };

  struct ResourceCopySemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId dst_resource_object_id = 0;
    trace::ObjectId src_resource_object_id = 0;
  };

  struct BufferCopySemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId dst_buffer_object_id = 0;
    trace::ObjectId src_buffer_object_id = 0;
    std::uint64_t dst_offset = 0;
    std::uint64_t src_offset = 0;
    std::uint64_t byte_count = 0;
  };

  struct ResolveSubresourceSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId dst_resource_object_id = 0;
    trace::ObjectId src_resource_object_id = 0;
    std::uint32_t dst_subresource = 0;
    std::uint32_t dst_x = 0;
    std::uint32_t dst_y = 0;
    std::uint32_t src_subresource = 0;
    bool has_src_rect = false;
    std::int32_t src_rect_left = 0;
    std::int32_t src_rect_top = 0;
    std::int32_t src_rect_right = 0;
    std::int32_t src_rect_bottom = 0;
    std::uint32_t format = 0;
    std::uint32_t mode = 3;
  };

  struct QueryCommandSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId query_heap_object_id = 0;
    std::uint32_t type = 0;
    std::uint32_t index = 0;
    std::uint32_t start_index = 0;
    std::uint32_t query_count = 0;
    trace::ObjectId dst_buffer_object_id = 0;
    std::uint64_t aligned_dst_buffer_offset = 0;
    bool resolve = false;
    bool end = false;
  };

  struct PredicationSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId buffer_object_id = 0;
    std::uint64_t aligned_buffer_offset = 0;
    std::uint32_t operation = 0;
  };

  struct WriteBufferImmediateEntry {
    GpuVirtualAddressBinding dest;
    std::uint32_t value = 0;
    std::uint32_t mode = 0;
  };

  struct WriteBufferImmediateSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::vector<WriteBufferImmediateEntry> writes;
  };

  struct FenceSemanticState {
    trace::ObjectId fence_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint64_t initial_value = 0;
    std::uint32_t flags = 0;
    std::uint64_t current_value = 0;
    std::uint64_t completed_value = 0;
    std::uint64_t last_signal_sequence = 0;
    std::uint64_t last_wait_sequence = 0;
    std::uint64_t last_event_sequence = 0;
  };

  struct FenceOperationSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId queue_object_id = 0;
    trace::ObjectId fence_object_id = 0;
    std::uint64_t fence_value = 0;
    bool queue_operation = false;
    bool wait_operation = false;
    bool event_operation = false;
  };

  struct CommandQueueSemanticState {
    trace::ObjectId queue_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t type = 0;
    std::int32_t priority = 0;
    std::uint32_t flags = 0;
    std::uint32_t node_mask = 0;
    std::uint64_t execute_count = 0;
    std::uint64_t last_execute_sequence = 0;
  };

  struct CommandAllocatorSemanticState {
    trace::ObjectId allocator_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t type = 0;
    std::uint64_t reset_count = 0;
    std::uint64_t last_reset_sequence = 0;
    std::uint64_t last_submit_sequence = 0;
    trace::ObjectId last_submit_fence_object_id = 0;
    std::uint64_t last_submit_fence_value = 0;
  };

  struct HeapSemanticState {
    trace::ObjectId heap_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint64_t size_in_bytes = 0;
    std::uint64_t alignment = 0;
    std::uint32_t heap_type = 0;
    std::uint32_t flags = 0;
  };

  struct QueryHeapSemanticState {
    trace::ObjectId query_heap_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t type = 0;
    std::uint32_t count = 0;
    std::uint32_t node_mask = 0;
  };

  struct DescriptorBinding {
    std::uint64_t descriptor = 0;
    trace::ObjectId heap_object_id = 0;
    std::uint32_t heap_type = 0;
    std::uint32_t descriptor_index = 0;
  };

  struct CommandSignatureArgument {
    std::uint32_t type = 0;
    std::uint32_t slot = 0;
    std::uint32_t root_parameter_index = 0;
    std::uint32_t dest_offset_in32bit_values = 0;
    std::uint32_t num32bit_values_to_set = 0;
  };

  struct CommandSignatureSemanticState {
    trace::ObjectId command_signature_object_id = 0;
    trace::ObjectId root_signature_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t byte_stride = 0;
    std::uint32_t argument_count = 0;
    std::uint32_t node_mask = 0;
    std::vector<CommandSignatureArgument> arguments;
  };

  struct ExecuteIndirectSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId command_signature_object_id = 0;
    trace::ObjectId arg_buffer_object_id = 0;
    trace::ObjectId count_buffer_object_id = 0;
    trace::ObjectId pipeline_state_object_id = 0;
    trace::ObjectId graphics_root_signature_object_id = 0;
    trace::ObjectId compute_root_signature_object_id = 0;
    std::unordered_map<std::uint32_t, DescriptorBinding> graphics_root_tables;
    std::unordered_map<std::uint32_t, DescriptorBinding> compute_root_tables;
    std::vector<DescriptorBinding> render_targets;
    DescriptorBinding depth_stencil;
    std::vector<ViewportSemanticState> viewports;
    std::vector<ScissorRectSemanticState> scissor_rects;
    std::uint32_t primitive_topology = 0;
    std::uint32_t max_command_count = 0;
    std::uint64_t arg_buffer_offset = 0;
    std::uint64_t count_buffer_offset = 0;
  };

  struct ExecuteBundleSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId bundle_command_list_object_id = 0;
  };

  struct DispatchSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId pipeline_state_object_id = 0;
    trace::ObjectId graphics_root_signature_object_id = 0;
    trace::ObjectId compute_root_signature_object_id = 0;
    std::unordered_map<std::uint32_t, DescriptorBinding> graphics_root_tables;
    std::unordered_map<std::uint32_t, DescriptorBinding> compute_root_tables;
    std::uint32_t thread_group_count_x = 0;
    std::uint32_t thread_group_count_y = 0;
    std::uint32_t thread_group_count_z = 0;
    bool mesh = false;
  };

  struct DispatchRaysSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    GpuVirtualAddressBinding ray_generation_shader_record;
    std::uint64_t ray_generation_shader_record_size = 0;
    GpuVirtualAddressBinding miss_shader_table;
    std::uint64_t miss_shader_table_size = 0;
    std::uint64_t miss_shader_table_stride = 0;
    GpuVirtualAddressBinding hit_group_table;
    std::uint64_t hit_group_table_size = 0;
    std::uint64_t hit_group_table_stride = 0;
    GpuVirtualAddressBinding callable_shader_table;
    std::uint64_t callable_shader_table_size = 0;
    std::uint64_t callable_shader_table_stride = 0;
  };

  struct TemporalUpscaleSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::uint32_t input_content_width = 0;
    std::uint32_t input_content_height = 0;
    bool auto_exposure = false;
    bool in_reset = false;
    bool depth_reversed = false;
    bool motion_vector_in_display_res = false;
    trace::ObjectId color_object_id = 0;
    trace::ObjectId depth_object_id = 0;
    trace::ObjectId motion_vector_object_id = 0;
    trace::ObjectId output_object_id = 0;
    trace::ObjectId exposure_texture_object_id = 0;
    float motion_vector_scale_x = 0.0f;
    float motion_vector_scale_y = 0.0f;
    float pre_exposure = 0.0f;
    float jitter_offset_x = 0.0f;
    float jitter_offset_y = 0.0f;
  };

  struct PipelineSemanticState {
    trace::ObjectId pipeline_state_object_id = 0;
    trace::ObjectId root_signature_object_id = 0;
    std::uint64_t create_sequence = 0;
    bool graphics = false;
    std::filesystem::path relative_path;
    std::vector<trace::BlobId> blob_refs;
    std::uint32_t node_mask = 0;
    std::uint32_t flags = 0;
    std::uint32_t sample_mask = 0;
    std::uint32_t primitive_topology_type = 0;
    std::uint32_t num_render_targets = 0;
    std::uint32_t dsv_format = 0;
    std::uint32_t sample_count = 1;
    std::uint32_t sample_quality = 0;
    std::uint32_t ib_strip_cut_value = 0;
    std::uint32_t input_element_count = 0;
    bool has_input_layout = false;
    bool has_blend_state = false;
    bool has_rasterizer_state = false;
    bool has_depth_stencil_state = false;
    std::vector<std::uint32_t> rtv_formats;
    bool has_vertex_shader = false;
    bool has_pixel_shader = false;
    bool has_domain_shader = false;
    bool has_hull_shader = false;
    bool has_geometry_shader = false;
    bool has_compute_shader = false;
    bool requires_dxmt_backend = false;
  };

  struct RootDescriptorRangeSemanticState {
    std::uint32_t type = 0;
    std::uint32_t descriptor_count = 0;
    std::uint32_t base_shader_register = 0;
    std::uint32_t register_space = 0;
    std::uint32_t offset_from_table_start = 0;
    std::uint32_t flags = 0;
  };

  struct RootDescriptorTableSemanticState {
    std::uint32_t root_parameter_index = 0;
    std::uint32_t shader_visibility = 0;
    std::vector<RootDescriptorRangeSemanticState> ranges;
  };

  struct RootParameterSemanticState {
    std::uint32_t root_parameter_index = 0;
    std::uint32_t parameter_type = 0;
    std::uint32_t shader_visibility = 0;
    std::uint32_t shader_register = 0;
    std::uint32_t register_space = 0;
    std::uint32_t num_32bit_values = 0;
    std::uint32_t range_count = 0;
    std::uint32_t flags = 0;
  };

  struct RootSignatureSemanticState {
    trace::ObjectId root_signature_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t node_mask = 0;
    std::uint64_t bytecode_size = 0;
    std::filesystem::path relative_path;
    std::vector<trace::BlobId> blob_refs;
    std::vector<std::uint8_t> bytes;
    std::vector<RootDescriptorTableSemanticState> descriptor_tables;
    bool has_descriptor_table_metadata = false;
    std::vector<RootParameterSemanticState> root_parameters;
    bool has_root_parameter_metadata = false;
  };

  struct DrawSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId pipeline_state_object_id = 0;
    trace::ObjectId graphics_root_signature_object_id = 0;
    std::unordered_map<std::uint32_t, DescriptorBinding> graphics_root_tables;
    std::vector<DescriptorBinding> render_targets;
    DescriptorBinding depth_stencil;
    std::vector<ViewportSemanticState> viewports;
    std::vector<ScissorRectSemanticState> scissor_rects;
    std::uint32_t primitive_topology = 0;
    std::uint32_t vertex_count_per_instance = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t start_vertex_location = 0;
    std::uint32_t start_instance_location = 0;
  };

  struct DrawIndexedSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId pipeline_state_object_id = 0;
    trace::ObjectId graphics_root_signature_object_id = 0;
    std::unordered_map<std::uint32_t, DescriptorBinding> graphics_root_tables;
    std::vector<DescriptorBinding> render_targets;
    DescriptorBinding depth_stencil;
    std::vector<ViewportSemanticState> viewports;
    std::vector<ScissorRectSemanticState> scissor_rects;
    std::uint32_t primitive_topology = 0;
    std::uint32_t index_count_per_instance = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t start_index_location = 0;
    std::int32_t base_vertex_location = 0;
    std::uint32_t start_instance_location = 0;
  };

  struct ResourceSemanticState {
    trace::ObjectId resource_object_id = 0;
    trace::ObjectId heap_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t heap_type = 0;
    std::uint32_t heap_flags = 0;
    std::uint32_t initial_state = 0;
    std::uint32_t current_state = 0;
    std::uint64_t heap_offset = 0;
    std::uint64_t last_transition_sequence = 0;
    std::uint64_t gpu_virtual_address = 0;
    std::uint64_t width = 0;
    std::uint64_t alignment = 0;
    std::uint32_t dimension = 0;
    std::uint32_t height = 0;
    std::uint32_t depth_or_array_size = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t format = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t sample_quality = 0;
    std::uint32_t layout = 0;
    std::uint32_t flags = 0;
    bool has_optimized_clear_value = false;
    bool reserved_resource = false;
    bool swapchain_back_buffer = false;
    std::uint32_t swapchain_buffer_index = 0;
    std::uint32_t optimized_clear_format = 0;
    float optimized_clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float optimized_clear_depth = 0.0f;
    std::uint32_t optimized_clear_stencil = 0;
    bool mapped = false;
    std::uint32_t mapped_subresource = 0;
    std::uint64_t map_sequence = 0;
    std::vector<ResourceTransition> transitions;
    std::unordered_map<std::uint32_t, std::uint32_t> subresource_states;
    std::vector<ResourceDataUpdate> data_updates;
  };

  struct DescriptorHeapSemanticState {
    trace::ObjectId heap_object_id = 0;
    std::uint64_t create_sequence = 0;
    std::uint32_t type = 0;
    std::uint32_t num_descriptors = 0;
    std::uint32_t flags = 0;
    std::uint32_t node_mask = 0;
    std::uint32_t descriptor_size = 0;
    std::uint64_t cpu_start = 0;
    std::uint64_t gpu_start = 0;
  };

  struct RenderTargetClearSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    DescriptorBinding render_target;
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t rect_count = 0;
    bool has_color = false;
    std::vector<ScissorRectSemanticState> rects;
  };

  struct DepthStencilClearSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    DescriptorBinding depth_stencil;
    float depth = 0.0f;
    std::uint32_t clear_flags = 0;
    std::uint32_t stencil = 0;
    std::uint32_t rect_count = 0;
    std::vector<ScissorRectSemanticState> rects;
  };

  struct UnorderedAccessClearSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    DescriptorBinding gpu_descriptor;
    DescriptorBinding cpu_descriptor;
    trace::ObjectId resource_object_id = 0;
    std::uint32_t uint_values[4] = {0, 0, 0, 0};
    float float_values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool integer = false;
    std::uint32_t rect_count = 0;
    std::vector<ScissorRectSemanticState> rects;
  };

  struct DiscardResourceSemanticState {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    trace::ObjectId resource_object_id = 0;
    std::uint32_t first_subresource = 0;
    std::uint32_t subresource_count = 0;
    std::uint32_t rect_count = 0;
    bool has_region = false;
    std::vector<ScissorRectSemanticState> rects;
  };

  struct DescriptorSemanticState {
    std::string kind;
    std::uint64_t create_sequence = 0;
    trace::ObjectId resource_object_id = 0;
    trace::ObjectId counter_resource_object_id = 0;
    std::uint64_t descriptor = 0;
    DescriptorBinding binding;
    std::uint32_t format = 0;
    std::uint32_t view_dimension = 0;
    std::uint32_t flags = 0;
    std::uint32_t shader_4_component_mapping = 0;
    std::uint64_t buffer_location = 0;
    std::uint32_t size_in_bytes = 0;
    std::uint64_t first_element = 0;
    std::uint32_t num_elements = 0;
    std::uint32_t structure_byte_stride = 0;
    std::uint64_t counter_offset_in_bytes = 0;
    bool has_view_desc = false;
    std::uint32_t most_detailed_mip = 0;
    std::uint32_t mip_levels = 0;
    std::uint32_t mip_slice = 0;
    std::uint32_t plane_slice = 0;
    std::uint32_t first_array_slice = 0;
    std::uint32_t array_size = 0;
    std::uint32_t first_2d_array_face = 0;
    std::uint32_t num_cubes = 0;
    std::uint32_t first_w_slice = 0;
    std::uint32_t w_size = 0;
    float resource_min_lod_clamp = 0.0f;
    GpuVirtualAddressBinding raytracing_acceleration_structure;
    GpuVirtualAddressBinding buffer;
  };

  struct SamplerSemanticState {
    std::uint64_t create_sequence = 0;
    std::uint64_t descriptor = 0;
    DescriptorBinding binding;
    std::uint32_t filter = 0;
    std::uint32_t address_u = 0;
    std::uint32_t address_v = 0;
    std::uint32_t address_w = 0;
    float mip_lod_bias = 0.0f;
    std::uint32_t max_anisotropy = 0;
    std::uint32_t comparison_func = 0;
    float border_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float min_lod = 0.0f;
    float max_lod = 0.0f;
    bool has_desc = false;
  };

  struct CopyDescriptorPair {
    std::uint64_t dst_descriptor = 0;
    std::uint64_t src_descriptor = 0;
  };

  struct CopyDescriptorSemanticState {
    std::uint64_t create_sequence = 0;
    std::uint32_t descriptor_heap_type = 0;
    std::vector<CopyDescriptorPair> descriptors;
  };

  struct CommandListSemanticState {
    bool recording = false;
    std::uint32_t type = 0;
    std::uint32_t node_mask = 0;
    trace::ObjectId allocator_object_id = 0;
    trace::ObjectId pipeline_state_object_id = 0;
    trace::ObjectId graphics_root_signature_object_id = 0;
    trace::ObjectId compute_root_signature_object_id = 0;
    std::uint64_t descriptor_heap_count = 0;
    std::uint64_t render_target_count = 0;
    std::uint64_t barrier_count = 0;
    std::uint64_t clear_count = 0;
    std::uint64_t copy_count = 0;
    std::uint64_t draw_count = 0;
    std::uint64_t dispatch_count = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t close_sequence = 0;
    std::uint32_t primitive_topology = 0;
    std::vector<trace::ObjectId> descriptor_heap_object_ids;
    std::vector<ViewportSemanticState> viewports;
    std::vector<ScissorRectSemanticState> scissor_rects;
    std::vector<DescriptorBinding> render_targets;
    DescriptorBinding depth_stencil;
    std::unordered_map<std::uint32_t, DescriptorBinding> graphics_root_tables;
    std::unordered_map<std::uint32_t, DescriptorBinding> compute_root_tables;
    std::unordered_map<std::uint32_t, Root32BitConstantsBinding> graphics_root_constants;
    std::unordered_map<std::uint32_t, Root32BitConstantsBinding> compute_root_constants;
    std::unordered_map<std::uint32_t, GpuVirtualAddressBinding> graphics_root_constant_buffers;
    std::unordered_map<std::uint32_t, GpuVirtualAddressBinding> compute_root_constant_buffers;
    std::unordered_map<std::uint32_t, GpuVirtualAddressBinding> graphics_root_shader_resources;
    std::unordered_map<std::uint32_t, GpuVirtualAddressBinding> compute_root_shader_resources;
    std::unordered_map<std::uint32_t, GpuVirtualAddressBinding> graphics_root_unordered_accesses;
    std::unordered_map<std::uint32_t, GpuVirtualAddressBinding> compute_root_unordered_accesses;
    std::vector<VertexBufferBinding> vertex_buffers;
    IndexBufferBinding index_buffer;
    std::vector<BufferCopySemanticState> buffer_copies;
    std::vector<TextureCopySemanticState> texture_copies;
    std::vector<ResourceCopySemanticState> resource_copies;
    std::vector<ResourceBarrierSemanticState> barriers;
    std::vector<ResolveSubresourceSemanticState> resolves;
    std::vector<QueryCommandSemanticState> queries;
    std::vector<PredicationSemanticState> predications;
    std::vector<WriteBufferImmediateSemanticState> write_buffer_immediates;
    std::vector<RenderTargetClearSemanticState> render_target_clears;
    std::vector<DepthStencilClearSemanticState> depth_stencil_clears;
    std::vector<UnorderedAccessClearSemanticState> unordered_access_clears;
    std::vector<DiscardResourceSemanticState> discards;
    std::vector<ExecuteIndirectSemanticState> indirect_executes;
    std::vector<ExecuteBundleSemanticState> bundle_executes;
    std::vector<DrawSemanticState> draws;
    std::vector<DrawIndexedSemanticState> indexed_draws;
    std::vector<DispatchSemanticState> dispatches;
    std::vector<DispatchRaysSemanticState> ray_dispatches;
    std::vector<TemporalUpscaleSemanticState> temporal_upscales;
    std::vector<std::uint64_t> command_indices;
  };

  const CommandListSemanticState *command_list_state(trace::ObjectId object_id) const noexcept;
  const std::vector<CopyDescriptorSemanticState> &descriptor_copies() const noexcept;

private:
  friend class D3D12NativeReplayer;
#if defined(APITRACE_ENABLE_TEST_HOOKS)
  friend struct D3D12ReplayBackendTestHook;
#endif

  bool resolve_descriptor_binding(
      std::uint64_t descriptor,
      bool gpu_descriptor,
      DescriptorBinding &binding,
      std::string &error) const;
  bool resolve_descriptor_binding_at(
      std::uint64_t descriptor,
      bool gpu_descriptor,
      std::uint64_t sequence,
      DescriptorBinding &binding,
      std::string &error) const;
  bool resolve_descriptor_binding_in_heaps(
      std::uint64_t descriptor,
      bool gpu_descriptor,
      const std::vector<trace::ObjectId> &heap_object_ids,
      DescriptorBinding &binding,
      std::string &error) const;
  bool resolve_descriptor_binding_in_heaps_at(
      std::uint64_t descriptor,
      bool gpu_descriptor,
      const std::vector<trace::ObjectId> &heap_object_ids,
      std::uint64_t sequence,
      DescriptorBinding &binding,
      std::string &error) const;
  bool resolve_gpu_virtual_address(
      std::uint64_t gpu_virtual_address,
      GpuVirtualAddressBinding &binding,
      std::string &error) const;
  bool resolve_gpu_virtual_address_at(
      std::uint64_t gpu_virtual_address,
      std::uint64_t sequence,
      GpuVirtualAddressBinding &binding,
      std::string &error) const;
  bool parse_texture_copy_location_payload(
      const trace::EventRecord &event,
      const std::string &payload_name,
      TextureCopyLocation &location,
      std::string &error) const;

  std::unordered_map<trace::ObjectId, CommandListSemanticState> command_lists_;
  std::unordered_map<trace::ObjectId, CommandQueueSemanticState> command_queues_;
  std::unordered_map<trace::ObjectId, CommandAllocatorSemanticState> command_allocators_;
  std::unordered_map<trace::ObjectId, DeviceSemanticState> devices_;
  std::unordered_map<trace::ObjectId, HeapSemanticState> heaps_;
  std::unordered_map<trace::ObjectId, QueryHeapSemanticState> query_heaps_;
  std::unordered_map<trace::ObjectId, DescriptorHeapSemanticState> descriptor_heaps_;
  std::vector<DescriptorSemanticState> descriptors_;
  std::vector<SamplerSemanticState> samplers_;
  std::vector<CopyDescriptorSemanticState> descriptor_copies_;
  std::unordered_map<trace::ObjectId, CommandSignatureSemanticState> command_signatures_;
  std::unordered_map<trace::ObjectId, FenceSemanticState> fences_;
  std::unordered_map<trace::ObjectId, ResourceSemanticState> resources_;
  std::unordered_map<trace::ObjectId, PipelineSemanticState> pipelines_;
  std::unordered_map<trace::ObjectId, RootSignatureSemanticState> root_signatures_;
  std::vector<FenceOperationSemanticState> fence_operations_;
  std::vector<ReplayCommandRecord> replay_commands_;

  // TODO: split device bootstrap, descriptor reconstruction, and queue submission into explicit replay phases.
  // TODO: separate native D3D12 replay from translation-layer-backed D3D12 replay if backend expectations diverge.
};

} // namespace apitrace::d3d12
