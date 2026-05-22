#pragma once

#include "apitrace/trace_bundle_io.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace apitrace::replay::internal {

struct ReplayCommandHeader {
  std::uint64_t sequence = 0;
  std::string label;
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

struct SetRenderTargetsCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::vector<trace::ObjectId> render_target_view_ids;
  bool has_depth_stencil = false;
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

struct DrawCommand {
  ReplayCommandHeader header;
  trace::ObjectId context_id = 0;
  std::uint32_t vertex_count = 0;
  std::uint32_t start_vertex_location = 0;
};

struct PresentCommand {
  ReplayCommandHeader header;
  trace::ObjectId swap_chain_id = 0;
  std::uint32_t sync_interval = 0;
  std::uint32_t flags = 0;
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
};

using ReplayCommand = std::variant<
    CreateDeviceAndSwapChainCommand,
    GetBufferCommand,
    CreateRenderTargetViewCommand,
    CreateInputLayoutCommand,
    CreateShaderCommand,
    CreateBufferCommand,
    MapCommand,
    UnmapCommand,
    SetRenderTargetsCommand,
    SetViewportsCommand,
    ClearRenderTargetViewCommand,
    SetInputLayoutCommand,
    SetVertexBuffersCommand,
    SetPrimitiveTopologyCommand,
    SetShaderCommand,
    SetConstantBuffersCommand,
    DrawCommand,
    PresentCommand,
    FrameBoundaryCommand,
    PresentBoundaryCommand>;

struct D3D11ReplayPlan {
  std::vector<ReplayCommand> commands;
};

bool build_d3d11_replay_plan(
    const trace::TraceBundleReader &reader,
    D3D11ReplayPlan &plan,
    std::string &error);

} // namespace apitrace::replay::internal
