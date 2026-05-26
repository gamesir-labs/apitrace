#pragma once

#include "apitrace/trace_bundle_io.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace apitrace::replay::internal {

struct ReplayCommandHeader {
  std::uint64_t sequence = 0;
  std::string label;
};

enum class ReplayResourceClass {
  Unknown,
  Buffer,
  Texture2D,
};

struct ReplaySwapChainDesc {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  std::uint32_t sample_count = 1;
  std::uint32_t sample_quality = 0;
  std::uint32_t buffer_usage = 0;
  std::uint32_t buffer_count = 0;
  std::uint32_t swap_effect = 0;
  std::uint32_t flags = 0;
  bool windowed = true;
};

struct CreateDeviceAndSwapChainCommand {
  ReplayCommandHeader header;
  trace::ObjectId swap_chain_id = 0;
  trace::ObjectId device_id = 0;
  trace::ObjectId context_id = 0;
  std::string driver_type;
  std::uint32_t flags = 0;
  std::uint32_t sdk_version = 0;
  std::string feature_level;
  ReplaySwapChainDesc swap_chain;
};

struct GetBufferCommand {
  ReplayCommandHeader header;
  trace::ObjectId swap_chain_id = 0;
  trace::ObjectId resource_id = 0;
  std::uint32_t buffer_index = 0;
};

struct CreateRenderTargetViewCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId resource_id = 0;
  trace::ObjectId view_id = 0;
  bool desc_present = false;
  std::uint32_t format = 0;
  std::uint32_t view_dimension = 0;
  std::uint32_t texture2d_mip_slice = 0;
};

struct InputElementDesc {
  std::string semantic_name;
  std::uint32_t semantic_index = 0;
  std::uint32_t format = 0;
  std::uint32_t input_slot = 0;
  std::uint32_t aligned_byte_offset = 0;
  std::uint32_t input_slot_class = 0;
  std::uint32_t instance_data_step_rate = 0;
};

struct CreateInputLayoutCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId input_layout_id = 0;
  std::filesystem::path shader_path;
  std::vector<InputElementDesc> elements;
};

struct CreateShaderCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId shader_id = 0;
  std::filesystem::path shader_path;
  bool vertex_stage = false;
};

struct CreateBufferCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId buffer_id = 0;
  std::uint32_t byte_width = 0;
  std::uint32_t usage = 0;
  std::uint32_t bind_flags = 0;
  std::uint32_t cpu_access_flags = 0;
  bool has_initial_data = false;
  std::filesystem::path initial_data_path;
};

struct Texture2DDesc {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mip_levels = 0;
  std::uint32_t array_size = 0;
  std::uint32_t format = 0;
  std::uint32_t sample_count = 1;
  std::uint32_t sample_quality = 0;
  std::uint32_t usage = 0;
  std::uint32_t bind_flags = 0;
  std::uint32_t cpu_access_flags = 0;
  std::uint32_t misc_flags = 0;
};

struct CreateTexture2DCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId texture_id = 0;
  Texture2DDesc desc;
  bool has_initial_data = false;
  std::filesystem::path initial_data_path;
};

struct ShaderResourceViewDesc {
  std::uint32_t format = 0;
  std::uint32_t view_dimension = 0;
  bool has_texture2d = false;
  std::uint32_t texture2d_most_detailed_mip = 0;
  std::uint32_t texture2d_mip_levels = 0;
};

struct CreateShaderResourceViewCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId resource_id = 0;
  trace::ObjectId view_id = 0;
  bool desc_present = false;
  ShaderResourceViewDesc desc;
};

struct SamplerStateDesc {
  std::uint32_t filter = 0;
  std::uint32_t address_u = 0;
  std::uint32_t address_v = 0;
  std::uint32_t address_w = 0;
  float mip_lod_bias = 0.0f;
  std::uint32_t max_anisotropy = 0;
  std::uint32_t comparison_func = 0;
  std::array<float, 4> border_color = {0.0f, 0.0f, 0.0f, 0.0f};
  float min_lod = 0.0f;
  float max_lod = 0.0f;
};

struct CreateSamplerStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId sampler_id = 0;
  SamplerStateDesc desc;
};

struct DepthStencilViewDesc {
  std::uint32_t format = 0;
  std::uint32_t view_dimension = 0;
  std::uint32_t flags = 0;
  bool has_texture2d = false;
  std::uint32_t texture2d_mip_slice = 0;
};

struct CreateDepthStencilViewCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId resource_id = 0;
  trace::ObjectId view_id = 0;
  bool desc_present = false;
  DepthStencilViewDesc desc;
};

struct BlendRenderTargetDesc {
  bool blend_enable = false;
  std::uint32_t src_blend = 0;
  std::uint32_t dest_blend = 0;
  std::uint32_t blend_op = 0;
  std::uint32_t src_blend_alpha = 0;
  std::uint32_t dest_blend_alpha = 0;
  std::uint32_t blend_op_alpha = 0;
  std::uint8_t write_mask = 0;
};

struct BlendStateDesc {
  bool alpha_to_coverage_enable = false;
  bool independent_blend_enable = false;
  std::array<BlendRenderTargetDesc, 8> render_targets;
};

struct CreateBlendStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId blend_state_id = 0;
  BlendStateDesc desc;
};

struct DepthStencilStateDesc {
  bool depth_enable = false;
  std::uint32_t depth_write_mask = 0;
  std::uint32_t depth_func = 0;
  bool stencil_enable = false;
  std::uint8_t stencil_read_mask = 0;
  std::uint8_t stencil_write_mask = 0;
};

struct CreateDepthStencilStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId depth_stencil_state_id = 0;
  DepthStencilStateDesc desc;
};

struct RasterizerStateDesc {
  std::uint32_t fill_mode = 0;
  std::uint32_t cull_mode = 0;
  bool front_counter_clockwise = false;
  int depth_bias = 0;
  float depth_bias_clamp = 0.0f;
  float slope_scaled_depth_bias = 0.0f;
  bool depth_clip_enable = false;
  bool scissor_enable = false;
  bool multisample_enable = false;
  bool antialiased_line_enable = false;
};

struct CreateRasterizerStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId device_id = 0;
  trace::ObjectId rasterizer_state_id = 0;
  RasterizerStateDesc desc;
};

struct MapCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId resource_id = 0;
  std::uint32_t subresource = 0;
  std::string map_type;
  std::uint32_t map_flags = 0;
};

struct UnmapCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId resource_id = 0;
  std::uint32_t subresource = 0;
  std::filesystem::path snapshot_path;
};

struct BoxDesc {
  std::uint32_t left = 0;
  std::uint32_t top = 0;
  std::uint32_t front = 0;
  std::uint32_t right = 0;
  std::uint32_t bottom = 0;
  std::uint32_t back = 0;
};

struct UpdateSubresourceCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId resource_id = 0;
  ReplayResourceClass resource_class = ReplayResourceClass::Unknown;
  std::uint32_t dst_subresource = 0;
  std::uint32_t src_row_pitch = 0;
  std::uint32_t src_depth_pitch = 0;
  bool has_dst_box = false;
  BoxDesc dst_box;
  std::filesystem::path data_path;
};

struct SetRenderTargetsCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::vector<trace::ObjectId> render_target_view_ids;
  bool has_depth_stencil = false;
  trace::ObjectId depth_stencil_view_id = 0;
};

struct ViewportDesc {
  float top_left_x = 0.0f;
  float top_left_y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float min_depth = 0.0f;
  float max_depth = 1.0f;
};

struct SetViewportsCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::vector<ViewportDesc> viewports;
};

struct ClearRenderTargetViewCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId render_target_view_id = 0;
  std::array<float, 4> color = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct SetInputLayoutCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId input_layout_id = 0;
};

struct VertexBufferBinding {
  trace::ObjectId buffer_id = 0;
  std::uint32_t stride = 0;
  std::uint32_t offset = 0;
};

struct SetVertexBuffersCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t start_slot = 0;
  std::vector<VertexBufferBinding> bindings;
};

struct SetIndexBufferCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId buffer_id = 0;
  std::uint32_t format = 0;
  std::uint32_t offset = 0;
};

struct SetPrimitiveTopologyCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::string topology;
};

struct SetShaderCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId shader_id = 0;
  bool vertex_stage = false;
};

struct SetConstantBuffersCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t start_slot = 0;
  std::vector<trace::ObjectId> buffer_ids;
  bool vertex_stage = false;
};

struct SetShaderResourcesCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t start_slot = 0;
  std::vector<trace::ObjectId> shader_resource_view_ids;
};

struct SetSamplersCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t start_slot = 0;
  std::vector<trace::ObjectId> sampler_ids;
};

struct SetDepthStencilStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId depth_stencil_state_id = 0;
  std::uint32_t stencil_ref = 0;
};

struct SetBlendStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId blend_state_id = 0;
  std::array<float, 4> blend_factor = {0.0f, 0.0f, 0.0f, 0.0f};
  std::uint32_t sample_mask = 0;
};

struct SetRasterizerStateCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId rasterizer_state_id = 0;
};

struct RectDesc {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct SetScissorRectsCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::vector<RectDesc> rects;
};

struct DrawCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t start_vertex_location = 0;
};

struct DrawIndexedCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t index_count = 0;
  std::uint32_t start_index_location = 0;
  std::int32_t base_vertex_location = 0;
};

struct DrawIndexedInstancedCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t index_count_per_instance = 0;
  std::uint32_t instance_count = 0;
  std::uint32_t start_index_location = 0;
  std::int32_t base_vertex_location = 0;
  std::uint32_t start_instance_location = 0;
};

struct CopyResourceCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId dst_resource_id = 0;
  trace::ObjectId src_resource_id = 0;
};

struct ResolveSubresourceCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId dst_resource_id = 0;
  std::uint32_t dst_subresource = 0;
  trace::ObjectId src_resource_id = 0;
  std::uint32_t src_subresource = 0;
  std::uint32_t format = 0;
};

struct ClearDepthStencilViewCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  trace::ObjectId depth_stencil_view_id = 0;
  std::uint32_t clear_flags = 0;
  float depth = 1.0f;
  std::uint8_t stencil = 0;
};

struct PresentCommand {
  ReplayCommandHeader header;
  trace::ObjectId swap_chain_id = 0;
  std::uint32_t sync_interval = 0;
  std::uint32_t flags = 0;
  std::uint64_t frame_index = 0;
};

struct FrameBoundaryCommand {
  ReplayCommandHeader header;
  std::string label;
  std::uint64_t frame_index = 0;
};

struct PresentBoundaryCommand {
  ReplayCommandHeader header;
  std::string label;
  std::uint64_t frame_index = 0;
  std::uint32_t sync_interval = 0;
  std::uint32_t flags = 0;
};

struct DebugMarkerCommand {
  ReplayCommandHeader header;
  std::string label;
  std::string scene_name;
  std::string dx_mode;
  std::string phase;
};

using ReplayCommand = std::variant<
    CreateDeviceAndSwapChainCommand,
    GetBufferCommand,
    CreateRenderTargetViewCommand,
    CreateTexture2DCommand,
    CreateShaderResourceViewCommand,
    CreateSamplerStateCommand,
    CreateDepthStencilViewCommand,
    CreateInputLayoutCommand,
    CreateShaderCommand,
    CreateBufferCommand,
    CreateBlendStateCommand,
    CreateDepthStencilStateCommand,
    CreateRasterizerStateCommand,
    MapCommand,
    UnmapCommand,
    UpdateSubresourceCommand,
    SetRenderTargetsCommand,
    SetViewportsCommand,
    ClearRenderTargetViewCommand,
    ClearDepthStencilViewCommand,
    SetInputLayoutCommand,
    SetVertexBuffersCommand,
    SetIndexBufferCommand,
    SetPrimitiveTopologyCommand,
    SetShaderCommand,
    SetConstantBuffersCommand,
    SetShaderResourcesCommand,
    SetSamplersCommand,
    SetDepthStencilStateCommand,
    SetBlendStateCommand,
    SetRasterizerStateCommand,
    SetScissorRectsCommand,
    DrawCommand,
    DrawIndexedCommand,
    DrawIndexedInstancedCommand,
    CopyResourceCommand,
    ResolveSubresourceCommand,
    PresentCommand,
    FrameBoundaryCommand,
    PresentBoundaryCommand,
    DebugMarkerCommand>;

struct D3D11ReplayPlan {
  std::vector<ReplayCommand> commands;
  std::uint64_t present_call_count = 0;
  std::uint64_t present_boundary_count = 0;
  std::uint64_t frame_begin_count = 0;
  std::uint64_t frame_end_count = 0;
  std::vector<std::uint32_t> present_sync_intervals;
  std::vector<std::uint32_t> present_flags;
  std::unordered_map<std::uint64_t, bool> open_frames;
  std::unordered_map<std::uint64_t, bool> presented_frames;
};

bool build_d3d11_replay_plan(
    const trace::TraceBundleReader &reader,
    D3D11ReplayPlan &plan,
    std::string &error);

} // namespace apitrace::replay::internal
