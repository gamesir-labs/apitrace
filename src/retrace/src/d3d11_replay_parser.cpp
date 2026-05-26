#include "retrace/src/d3d11_replay_parser.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string_view>
#include <unordered_map>

namespace apitrace::replay::internal {

namespace {

using json = nlohmann::json;
using ParseCallHandler = bool (*)(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error);

std::string record_prefix(const trace::EventRecord &event)
{
  std::ostringstream message;
  message << "sequence " << event.callsite.sequence << " ";
  if (event.kind == trace::EventKind::Boundary) {
    message << "boundary " << event.callsite.function_name;
  } else {
    message << "function " << event.callsite.function_name;
  }
  return message.str();
}

bool payload_to_json(const trace::EventRecord &event, json &payload, std::string &error)
{
  payload = json::parse(event.payload, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    error = record_prefix(event) + ": payload must be a JSON object";
    return false;
  }
  return true;
}

template <typename CommandT>
CommandT make_command_header(const trace::EventRecord &event)
{
  CommandT command;
  command.header.sequence = event.callsite.sequence;
  command.header.label = event.callsite.function_name;
  return command;
}

bool require_object_ref_count(const trace::EventRecord &event, std::size_t expected, std::string &error)
{
  if (event.object_refs.size() < expected) {
    std::ostringstream message;
    message << record_prefix(event) << ": expected at least " << expected << " object refs";
    error = message.str();
    return false;
  }
  return true;
}

bool require_payload_key(
    const trace::EventRecord &event,
    const json &payload,
    std::string_view key,
    std::string &error)
{
  if (payload.find(std::string(key)) != payload.end()) {
    return true;
  }
  error = record_prefix(event) + ": missing payload key " + std::string(key);
  return false;
}

std::filesystem::path resolve_asset_path(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    std::string_view key,
    std::string &error)
{
  const auto it = payload.find(std::string(key));
  if (it == payload.end() || !it->is_string() || it->get<std::string>().empty()) {
    error = record_prefix(event) + ": missing " + std::string(key);
    return {};
  }
  return reader.layout().root_path / std::filesystem::path(it->get<std::string>());
}

bool require_json_object(
    const trace::EventRecord &event,
    const json &payload,
    std::string_view key,
    const json *&object,
    std::string &error)
{
  const auto it = payload.find(std::string(key));
  if (it == payload.end() || !it->is_object()) {
    error = record_prefix(event) + ": missing " + std::string(key) + " object";
    return false;
  }
  object = &(*it);
  return true;
}

bool parse_bool_field(
    const trace::EventRecord &event,
    const json &source,
    std::string_view key,
    bool &value,
    std::string &error)
{
  const auto it = source.find(std::string(key));
  if (it == source.end() || !it->is_boolean()) {
    error = record_prefix(event) + ": missing boolean field " + std::string(key);
    return false;
  }
  value = it->get<bool>();
  return true;
}

bool parse_float4_field(
    const trace::EventRecord &event,
    const json &source,
    std::string_view key,
    std::array<float, 4> &values,
    std::string &error)
{
  const auto it = source.find(std::string(key));
  if (it == source.end() || !it->is_array() || it->size() != 4) {
    error = record_prefix(event) + ": missing float4 field " + std::string(key);
    return false;
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index] = (*it)[index].get<float>();
  }
  return true;
}

ReplayResourceClass parse_resource_class_name(std::string_view resource_class)
{
  if (resource_class == "buffer") {
    return ReplayResourceClass::Buffer;
  }
  if (resource_class == "texture2d") {
    return ReplayResourceClass::Texture2D;
  }
  return ReplayResourceClass::Unknown;
}

bool parse_input_elements(
    const trace::EventRecord &event,
    const json &payload,
    std::vector<InputElementDesc> &elements,
    std::string &error)
{
  const auto elements_it = payload.find("elements");
  if (elements_it == payload.end() || !elements_it->is_array() || elements_it->empty()) {
    error = record_prefix(event) + ": missing input layout elements";
    return false;
  }

  elements.clear();
  elements.reserve(elements_it->size());
  for (const auto &entry : *elements_it) {
    if (!entry.is_object()) {
      error = record_prefix(event) + ": input layout element must be an object";
      return false;
    }

    InputElementDesc element;
    element.semantic_name = entry.value("semantic_name", "");
    element.semantic_index = entry.value("semantic_index", 0u);
    element.format = entry.value("format", 0u);
    element.input_slot = entry.value("input_slot", 0u);
    element.aligned_byte_offset = entry.value("aligned_byte_offset", 0u);
    element.input_slot_class = entry.value("input_slot_class", 0u);
    element.instance_data_step_rate = entry.value("instance_data_step_rate", 0u);
    if (element.semantic_name.empty()) {
      error = record_prefix(event) + ": input layout element missing semantic_name";
      return false;
    }
    elements.push_back(std::move(element));
  }
  return true;
}

bool parse_texture2d_desc(const trace::EventRecord &event, const json &source, Texture2DDesc &desc, std::string &error)
{
  desc.width = source.value("width", 0u);
  desc.height = source.value("height", 0u);
  desc.mip_levels = source.value("mip_levels", 0u);
  desc.array_size = source.value("array_size", 0u);
  desc.format = source.value("format", 0u);
  desc.sample_count = source.value("sample_count", 1u);
  desc.sample_quality = source.value("sample_quality", 0u);
  desc.usage = source.value("usage", 0u);
  desc.bind_flags = source.value("bind_flags", 0u);
  desc.cpu_access_flags = source.value("cpu_access_flags", 0u);
  desc.misc_flags = source.value("misc_flags", 0u);
  if (desc.width == 0 || desc.height == 0 || desc.array_size == 0) {
    error = record_prefix(event) + ": incomplete texture2d desc";
    return false;
  }
  return true;
}

bool parse_shader_resource_view_desc(
    const trace::EventRecord &event,
    const json &source,
    ShaderResourceViewDesc &desc,
    std::string &error)
{
  desc.format = source.value("format", 0u);
  desc.view_dimension = source.value("view_dimension", 0u);
  const auto texture2d = source.find("texture2d");
  if (texture2d != source.end()) {
    if (!texture2d->is_object()) {
      error = record_prefix(event) + ": shader resource view texture2d desc must be an object";
      return false;
    }
    desc.has_texture2d = true;
    desc.texture2d_most_detailed_mip = texture2d->value("most_detailed_mip", 0u);
    desc.texture2d_mip_levels = texture2d->value("mip_levels", 0u);
  }
  return true;
}

bool parse_render_target_view_desc(
    const trace::EventRecord &event,
    const json &source,
    CreateRenderTargetViewCommand &command,
    std::string &error)
{
  command.format = source.value("format", 0u);
  command.view_dimension = source.value("view_dimension", 0u);
  const auto texture2d = source.find("texture2d");
  if (texture2d != source.end()) {
    if (!texture2d->is_object()) {
      error = record_prefix(event) + ": render target view texture2d desc must be an object";
      return false;
    }
    command.texture2d_mip_slice = texture2d->value("mip_slice", 0u);
  }
  return true;
}

bool parse_depth_stencil_view_desc(
    const trace::EventRecord &event,
    const json &source,
    DepthStencilViewDesc &desc,
    std::string &error)
{
  desc.format = source.value("format", 0u);
  desc.view_dimension = source.value("view_dimension", 0u);
  desc.flags = source.value("flags", 0u);
  const auto texture2d = source.find("texture2d");
  if (texture2d != source.end()) {
    if (!texture2d->is_object()) {
      error = record_prefix(event) + ": depth stencil view texture2d desc must be an object";
      return false;
    }
    desc.has_texture2d = true;
    desc.texture2d_mip_slice = texture2d->value("mip_slice", 0u);
  }
  return true;
}

bool parse_sampler_state_desc(
    const trace::EventRecord &event,
    const json &source,
    SamplerStateDesc &desc,
    std::string &error)
{
  desc.filter = source.value("filter", 0u);
  desc.address_u = source.value("address_u", 0u);
  desc.address_v = source.value("address_v", 0u);
  desc.address_w = source.value("address_w", 0u);
  desc.mip_lod_bias = source.value("mip_lod_bias", 0.0f);
  desc.max_anisotropy = source.value("max_anisotropy", 0u);
  desc.comparison_func = source.value("comparison_func", 0u);
  if (!parse_float4_field(event, source, "border_color", desc.border_color, error)) {
    return false;
  }
  desc.min_lod = source.value("min_lod", 0.0f);
  desc.max_lod = source.value("max_lod", 0.0f);
  return true;
}

bool parse_blend_state_desc(
    const trace::EventRecord &event,
    const json &source,
    BlendStateDesc &desc,
    std::string &error)
{
  if (!parse_bool_field(event, source, "alpha_to_coverage_enable", desc.alpha_to_coverage_enable, error) ||
      !parse_bool_field(event, source, "independent_blend_enable", desc.independent_blend_enable, error)) {
    return false;
  }

  const auto render_targets = source.find("render_targets");
  if (render_targets == source.end() || !render_targets->is_array() ||
      render_targets->size() != desc.render_targets.size()) {
    error = record_prefix(event) + ": blend state render_targets must contain 8 entries";
    return false;
  }

  for (std::size_t index = 0; index < desc.render_targets.size(); ++index) {
    const auto &entry = (*render_targets)[index];
    if (!entry.is_object()) {
      error = record_prefix(event) + ": blend state render target entry must be an object";
      return false;
    }
    auto &target = desc.render_targets[index];
    if (!parse_bool_field(event, entry, "blend_enable", target.blend_enable, error)) {
      return false;
    }
    target.src_blend = entry.value("src_blend", 0u);
    target.dest_blend = entry.value("dest_blend", 0u);
    target.blend_op = entry.value("blend_op", 0u);
    target.src_blend_alpha = entry.value("src_blend_alpha", 0u);
    target.dest_blend_alpha = entry.value("dest_blend_alpha", 0u);
    target.blend_op_alpha = entry.value("blend_op_alpha", 0u);
    target.write_mask = static_cast<std::uint8_t>(entry.value("write_mask", 0u));
  }
  return true;
}

bool parse_depth_stencil_state_desc(
    const trace::EventRecord &event,
    const json &source,
    DepthStencilStateDesc &desc,
    std::string &error)
{
  if (!parse_bool_field(event, source, "depth_enable", desc.depth_enable, error) ||
      !parse_bool_field(event, source, "stencil_enable", desc.stencil_enable, error)) {
    return false;
  }
  desc.depth_write_mask = source.value("depth_write_mask", 0u);
  desc.depth_func = source.value("depth_func", 0u);
  desc.stencil_read_mask = static_cast<std::uint8_t>(source.value("stencil_read_mask", 0u));
  desc.stencil_write_mask = static_cast<std::uint8_t>(source.value("stencil_write_mask", 0u));
  return true;
}

bool parse_rasterizer_state_desc(
    const trace::EventRecord &event,
    const json &source,
    RasterizerStateDesc &desc,
    std::string &error)
{
  desc.fill_mode = source.value("fill_mode", 0u);
  desc.cull_mode = source.value("cull_mode", 0u);
  desc.depth_bias = source.value("depth_bias", 0);
  desc.depth_bias_clamp = source.value("depth_bias_clamp", 0.0f);
  desc.slope_scaled_depth_bias = source.value("slope_scaled_depth_bias", 0.0f);
  if (!parse_bool_field(event, source, "front_counter_clockwise", desc.front_counter_clockwise, error) ||
      !parse_bool_field(event, source, "depth_clip_enable", desc.depth_clip_enable, error) ||
      !parse_bool_field(event, source, "scissor_enable", desc.scissor_enable, error) ||
      !parse_bool_field(event, source, "multisample_enable", desc.multisample_enable, error) ||
      !parse_bool_field(event, source, "antialiased_line_enable", desc.antialiased_line_enable, error)) {
    return false;
  }
  return true;
}

bool parse_viewports(
    const trace::EventRecord &event,
    const json &payload,
    std::vector<ViewportDesc> &viewports,
    std::string &error)
{
  const auto viewports_it = payload.find("viewports");
  if (viewports_it != payload.end() && viewports_it->is_array() && !viewports_it->empty()) {
    viewports.clear();
    viewports.reserve(viewports_it->size());
    for (const auto &entry : *viewports_it) {
      if (!entry.is_object()) {
        error = record_prefix(event) + ": viewport must be an object";
        return false;
      }

      ViewportDesc viewport;
      viewport.top_left_x = entry.value("top_left_x", 0.0f);
      viewport.top_left_y = entry.value("top_left_y", 0.0f);
      viewport.width = entry.value("width", 0.0f);
      viewport.height = entry.value("height", 0.0f);
      viewport.min_depth = entry.value("min_depth", 0.0f);
      viewport.max_depth = entry.value("max_depth", 1.0f);
      viewports.push_back(viewport);
    }
    return true;
  }

  if (payload.value("num_viewports", 0u) == 1) {
    ViewportDesc viewport;
    viewport.width = payload.value("first_width", 0.0f);
    viewport.height = payload.value("first_height", 0.0f);
    viewports = {viewport};
    return true;
  }

  error = record_prefix(event) + ": missing viewport array";
  return false;
}

bool parse_rects(
    const trace::EventRecord &event,
    const json &payload,
    std::vector<RectDesc> &rects,
    std::string &error)
{
  const auto rects_it = payload.find("rects");
  if (rects_it == payload.end() || !rects_it->is_array()) {
    error = record_prefix(event) + ": missing scissor rects";
    return false;
  }

  rects.clear();
  rects.reserve(rects_it->size());
  for (const auto &entry : *rects_it) {
    if (!entry.is_object()) {
      error = record_prefix(event) + ": scissor rect must be an object";
      return false;
    }
    RectDesc rect;
    rect.left = entry.value("left", 0);
    rect.top = entry.value("top", 0);
    rect.right = entry.value("right", 0);
    rect.bottom = entry.value("bottom", 0);
    rects.push_back(rect);
  }
  return true;
}

bool parse_vertex_buffer_bindings(
    const trace::EventRecord &event,
    const json &payload,
    std::vector<VertexBufferBinding> &bindings,
    std::string &error)
{
  const auto bindings_it = payload.find("bindings");
  if (bindings_it != payload.end() && bindings_it->is_array()) {
    bindings.clear();
    bindings.reserve(bindings_it->size());
    for (const auto &entry : *bindings_it) {
      if (!entry.is_object()) {
        error = record_prefix(event) + ": vertex buffer binding must be an object";
        return false;
      }

      VertexBufferBinding binding;
      binding.buffer_id = entry.value("object_id", 0ull);
      binding.stride = entry.value("stride", 0u);
      binding.offset = entry.value("offset", 0u);
      bindings.push_back(binding);
    }
    return true;
  }

  if (payload.value("num_buffers", 0u) == 1 && event.object_refs.size() >= 2) {
    VertexBufferBinding binding;
    binding.buffer_id = event.object_refs[1];
    binding.stride = payload.value("first_stride", 0u);
    binding.offset = payload.value("first_offset", 0u);
    bindings = {binding};
    return true;
  }

  error = record_prefix(event) + ": missing vertex buffer bindings";
  return false;
}

std::optional<std::uint64_t> parse_frame_index(const json &payload)
{
  const auto it = payload.find("frame_index");
  if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
    return std::nullopt;
  }
  return it->get<std::uint64_t>();
}

bool parse_create_device_and_swap_chain(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  const json *swap_chain = nullptr;
  if (!require_json_object(event, payload, "swap_chain", swap_chain, error)) {
    return false;
  }

  auto command = make_command_header<CreateDeviceAndSwapChainCommand>(event);
  command.swap_chain_id = event.object_refs[0];
  command.device_id = event.object_refs[1];
  command.context_id = event.object_refs[2];
  command.driver_type = payload.value("driver_type", std::string("UNKNOWN"));
  command.flags = payload.value("flags", 0u);
  command.sdk_version = payload.value("sdk_version", 0u);
  command.feature_level = payload.value("feature_level", std::string("11_0"));
  command.swap_chain.width = swap_chain->value("width", 0u);
  command.swap_chain.height = swap_chain->value("height", 0u);
  command.swap_chain.format = swap_chain->value("format", 0u);
  command.swap_chain.sample_count = swap_chain->value("sample_count", 1u);
  command.swap_chain.sample_quality = swap_chain->value("sample_quality", 0u);
  command.swap_chain.buffer_usage = swap_chain->value("buffer_usage", 32u);
  command.swap_chain.buffer_count = swap_chain->value("buffer_count", 0u);
  command.swap_chain.swap_effect = swap_chain->value("swap_effect", 0u);
  command.swap_chain.flags = swap_chain->value("flags", 0u);
  command.swap_chain.windowed = swap_chain->value("windowed", true);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_get_buffer(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<GetBufferCommand>(event);
  command.swap_chain_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.buffer_index = payload.value("buffer_index", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_render_target_view(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CreateRenderTargetViewCommand>(event);
  command.device_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.view_id = event.object_refs[2];
  command.desc_present = payload.value("desc_present", false);
  if (command.desc_present) {
    const json *desc = nullptr;
    if (!require_json_object(event, payload, "desc", desc, error) ||
        !parse_render_target_view_desc(event, *desc, command, error)) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_texture2d(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  const json *desc = nullptr;
  if (!require_json_object(event, payload, "desc", desc, error)) {
    return false;
  }

  auto command = make_command_header<CreateTexture2DCommand>(event);
  command.device_id = event.object_refs[0];
  command.texture_id = event.object_refs[1];
  command.has_initial_data = payload.value("has_initial_data", false);
  if (!parse_texture2d_desc(event, *desc, command.desc, error)) {
    return false;
  }
  if (command.has_initial_data) {
    command.initial_data_path = resolve_asset_path(reader, event, payload, "initial_data_path", error);
    if (!error.empty()) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_shader_resource_view(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CreateShaderResourceViewCommand>(event);
  command.device_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.view_id = event.object_refs[2];
  command.desc_present = payload.value("desc_present", false);
  if (command.desc_present) {
    const json *desc = nullptr;
    if (!require_json_object(event, payload, "desc", desc, error) ||
        !parse_shader_resource_view_desc(event, *desc, command.desc, error)) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_depth_stencil_view(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CreateDepthStencilViewCommand>(event);
  command.device_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.view_id = event.object_refs[2];
  command.desc_present = payload.value("desc_present", false);
  if (command.desc_present) {
    const json *desc = nullptr;
    if (!require_json_object(event, payload, "desc", desc, error) ||
        !parse_depth_stencil_view_desc(event, *desc, command.desc, error)) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_input_layout(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<CreateInputLayoutCommand>(event);
  command.device_id = event.object_refs[0];
  command.input_layout_id = event.object_refs[1];
  command.shader_path = resolve_asset_path(reader, event, payload, "shader_path", error);
  if (!error.empty()) {
    return false;
  }
  if (!parse_input_elements(event, payload, command.elements, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_shader(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<CreateShaderCommand>(event);
  command.device_id = event.object_refs[0];
  command.shader_id = event.object_refs[1];
  command.shader_path = resolve_asset_path(reader, event, payload, "shader_path", error);
  if (!error.empty()) {
    return false;
  }
  command.vertex_stage = event.callsite.function_name == "ID3D11Device::CreateVertexShader";
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_buffer(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<CreateBufferCommand>(event);
  command.device_id = event.object_refs[0];
  command.buffer_id = event.object_refs[1];
  command.byte_width = payload.value("byte_width", 0u);
  command.usage = payload.value("usage", 0u);
  command.bind_flags = payload.value("bind_flags", 0u);
  command.cpu_access_flags = payload.value("cpu_access_flags", 0u);
  command.has_initial_data = payload.value("has_initial_data", false);
  if (command.has_initial_data) {
    command.initial_data_path = resolve_asset_path(reader, event, payload, "initial_data_path", error);
    if (!error.empty()) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_blend_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  const json *desc = nullptr;
  if (!require_json_object(event, payload, "desc", desc, error)) {
    return false;
  }
  auto command = make_command_header<CreateBlendStateCommand>(event);
  command.device_id = event.object_refs[0];
  command.blend_state_id = event.object_refs[1];
  if (!parse_blend_state_desc(event, *desc, command.desc, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_depth_stencil_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  const json *desc = nullptr;
  if (!require_json_object(event, payload, "desc", desc, error)) {
    return false;
  }
  auto command = make_command_header<CreateDepthStencilStateCommand>(event);
  command.device_id = event.object_refs[0];
  command.depth_stencil_state_id = event.object_refs[1];
  if (!parse_depth_stencil_state_desc(event, *desc, command.desc, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_rasterizer_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  const json *desc = nullptr;
  if (!require_json_object(event, payload, "desc", desc, error)) {
    return false;
  }
  auto command = make_command_header<CreateRasterizerStateCommand>(event);
  command.device_id = event.object_refs[0];
  command.rasterizer_state_id = event.object_refs[1];
  if (!parse_rasterizer_state_desc(event, *desc, command.desc, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_sampler_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  const json *desc = nullptr;
  if (!require_json_object(event, payload, "desc", desc, error)) {
    return false;
  }
  auto command = make_command_header<CreateSamplerStateCommand>(event);
  command.device_id = event.object_refs[0];
  command.sampler_id = event.object_refs[1];
  if (!parse_sampler_state_desc(event, *desc, command.desc, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_map(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<MapCommand>(event);
  command.context_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.subresource = payload.value("subresource", 0u);
  command.map_type = payload.value("map_type", std::string("OTHER"));
  command.map_flags = payload.value("map_flags", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_unmap(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<UnmapCommand>(event);
  command.context_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.subresource = payload.value("subresource", 0u);
  command.snapshot_path = resolve_asset_path(reader, event, payload, "snapshot_path", error);
  if (!error.empty()) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_update_subresource(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<UpdateSubresourceCommand>(event);
  command.context_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.resource_class = parse_resource_class_name(payload.value("resource_class", std::string("unknown")));
  command.dst_subresource = payload.value("dst_subresource", 0u);
  command.src_row_pitch = payload.value("src_row_pitch", 0u);
  command.src_depth_pitch = payload.value("src_depth_pitch", 0u);
  command.has_dst_box = payload.value("has_dst_box", false);
  if (command.has_dst_box) {
    const json *box = nullptr;
    if (!require_json_object(event, payload, "dst_box", box, error)) {
      return false;
    }
    command.dst_box.left = box->value("left", 0u);
    command.dst_box.top = box->value("top", 0u);
    command.dst_box.front = box->value("front", 0u);
    command.dst_box.right = box->value("right", 0u);
    command.dst_box.bottom = box->value("bottom", 0u);
    command.dst_box.back = box->value("back", 0u);
  }
  command.data_path = resolve_asset_path(reader, event, payload, "data_path", error);
  if (!error.empty()) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_om_set_render_targets(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetRenderTargetsCommand>(event);
  command.context_id = event.object_refs[0];
  command.has_depth_stencil = payload.value("has_depth_stencil", false);
  command.render_target_view_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  if (command.has_depth_stencil) {
    if (command.render_target_view_ids.empty()) {
      error = record_prefix(event) + ": missing depth stencil view ref";
      return false;
    }
    command.depth_stencil_view_id = command.render_target_view_ids.back();
    command.render_target_view_ids.pop_back();
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_rs_set_viewports(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetViewportsCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_viewports(event, payload, command.viewports, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_clear_render_target_view(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<ClearRenderTargetViewCommand>(event);
  command.context_id = event.object_refs[0];
  command.render_target_view_id = event.object_refs[1];
  if (!parse_float4_field(event, payload, "color", command.color, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_clear_depth_stencil_view(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<ClearDepthStencilViewCommand>(event);
  command.context_id = event.object_refs[0];
  command.depth_stencil_view_id = event.object_refs[1];
  command.clear_flags = payload.value("clear_flags", 0u);
  command.depth = payload.value("depth", 1.0f);
  command.stencil = static_cast<std::uint8_t>(payload.value("stencil", 0u));
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_input_layout(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  (void)payload;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<SetInputLayoutCommand>(event);
  command.context_id = event.object_refs[0];
  command.input_layout_id = event.object_refs[1];
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_vertex_buffers(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetVertexBuffersCommand>(event);
  command.context_id = event.object_refs[0];
  command.start_slot = payload.value("start_slot", 0u);
  if (!parse_vertex_buffer_bindings(event, payload, command.bindings, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_index_buffer(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<SetIndexBufferCommand>(event);
  command.context_id = event.object_refs[0];
  command.buffer_id = event.object_refs[1];
  command.format = payload.value("format", 0u);
  command.offset = payload.value("offset", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_primitive_topology(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetPrimitiveTopologyCommand>(event);
  command.context_id = event.object_refs[0];
  command.topology = payload.value("topology", std::string("OTHER"));
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_set_shader(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  if (payload.value("class_instance_count", 0u) != 0u) {
    error = record_prefix(event) + ": class instances are unsupported";
    return false;
  }

  auto command = make_command_header<SetShaderCommand>(event);
  command.context_id = event.object_refs[0];
  command.shader_id = event.object_refs[1];
  command.vertex_stage = event.callsite.function_name == "ID3D11DeviceContext::VSSetShader";
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_set_constant_buffers(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetConstantBuffersCommand>(event);
  command.context_id = event.object_refs[0];
  command.start_slot = payload.value("start_slot", 0u);
  command.vertex_stage = event.callsite.function_name == "ID3D11DeviceContext::VSSetConstantBuffers";
  command.buffer_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ps_set_shader_resources(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetShaderResourcesCommand>(event);
  command.context_id = event.object_refs[0];
  command.start_slot = payload.value("start_slot", 0u);
  command.shader_resource_view_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ps_set_samplers(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetSamplersCommand>(event);
  command.context_id = event.object_refs[0];
  command.start_slot = payload.value("start_slot", 0u);
  command.sampler_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_om_set_depth_stencil_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetDepthStencilStateCommand>(event);
  command.context_id = event.object_refs[0];
  command.depth_stencil_state_id = event.object_refs.size() >= 2 ? event.object_refs[1] : 0;
  command.stencil_ref = payload.value("stencil_ref", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_om_set_blend_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetBlendStateCommand>(event);
  command.context_id = event.object_refs[0];
  command.blend_state_id = event.object_refs.size() >= 2 ? event.object_refs[1] : 0;
  command.sample_mask = payload.value("sample_mask", 0u);
  if (!parse_float4_field(event, payload, "blend_factor", command.blend_factor, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_rs_set_state(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  (void)payload;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetRasterizerStateCommand>(event);
  command.context_id = event.object_refs[0];
  command.rasterizer_state_id = event.object_refs.size() >= 2 ? event.object_refs[1] : 0;
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_rs_set_scissor_rects(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetScissorRectsCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_rects(event, payload, command.rects, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_draw(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<DrawCommand>(event);
  command.context_id = event.object_refs[0];
  command.vertex_count = payload.value("vertex_count", 0u);
  command.start_vertex_location = payload.value("start_vertex_location", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_draw_indexed(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<DrawIndexedCommand>(event);
  command.context_id = event.object_refs[0];
  command.index_count = payload.value("index_count", 0u);
  command.start_index_location = payload.value("start_index_location", 0u);
  command.base_vertex_location = payload.value("base_vertex_location", 0);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_draw_indexed_instanced(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<DrawIndexedInstancedCommand>(event);
  command.context_id = event.object_refs[0];
  command.index_count_per_instance = payload.value("index_count_per_instance", 0u);
  command.instance_count = payload.value("instance_count", 0u);
  command.start_index_location = payload.value("start_index_location", 0u);
  command.base_vertex_location = payload.value("base_vertex_location", 0);
  command.start_instance_location = payload.value("start_instance_location", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_copy_resource(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  (void)payload;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CopyResourceCommand>(event);
  command.context_id = event.object_refs[0];
  command.dst_resource_id = event.object_refs[1];
  command.src_resource_id = event.object_refs[2];
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_resolve_subresource(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<ResolveSubresourceCommand>(event);
  command.context_id = event.object_refs[0];
  command.dst_resource_id = event.object_refs[1];
  command.src_resource_id = event.object_refs[2];
  command.dst_subresource = payload.value("dst_subresource", 0u);
  command.src_subresource = payload.value("src_subresource", 0u);
  command.format = payload.value("format", 0u);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_present(
    const trace::TraceBundleReader &reader,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)reader;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  if (!require_payload_key(event, payload, "sync_interval", error) ||
      !require_payload_key(event, payload, "flags", error)) {
    return false;
  }
  auto command = make_command_header<PresentCommand>(event);
  command.swap_chain_id = event.object_refs[0];
  command.sync_interval = payload.value("sync_interval", 0u);
  command.flags = payload.value("flags", 0u);
  if (!require_payload_key(event, payload, "frame_index", error)) {
    return false;
  }
  command.frame_index = parse_frame_index(payload).value_or(0);
  if (command.frame_index != plan.present_call_count) {
    error = record_prefix(event) + ": IDXGISwapChain::Present frame_index is not contiguous";
    return false;
  }
  plan.present_sync_intervals.push_back(command.sync_interval);
  plan.present_flags.push_back(command.flags);
  ++plan.present_call_count;
  plan.commands.emplace_back(std::move(command));
  return true;
}

const std::unordered_map<std::string, ParseCallHandler> &call_handlers()
{
  static const std::unordered_map<std::string, ParseCallHandler> handlers = {
      {"D3D11CreateDeviceAndSwapChain", &parse_create_device_and_swap_chain},
      {"IDXGISwapChain::GetBuffer", &parse_get_buffer},
      {"ID3D11Device::CreateRenderTargetView", &parse_create_render_target_view},
      {"ID3D11Device::CreateTexture2D", &parse_create_texture2d},
      {"ID3D11Device::CreateShaderResourceView", &parse_create_shader_resource_view},
      {"ID3D11Device::CreateDepthStencilView", &parse_create_depth_stencil_view},
      {"ID3D11Device::CreateInputLayout", &parse_create_input_layout},
      {"ID3D11Device::CreateVertexShader", &parse_create_shader},
      {"ID3D11Device::CreatePixelShader", &parse_create_shader},
      {"ID3D11Device::CreateBuffer", &parse_create_buffer},
      {"ID3D11Device::CreateBlendState", &parse_create_blend_state},
      {"ID3D11Device::CreateDepthStencilState", &parse_create_depth_stencil_state},
      {"ID3D11Device::CreateRasterizerState", &parse_create_rasterizer_state},
      {"ID3D11Device::CreateSamplerState", &parse_create_sampler_state},
      {"ID3D11DeviceContext::Map", &parse_map},
      {"ID3D11DeviceContext::Unmap", &parse_unmap},
      {"ID3D11DeviceContext::UpdateSubresource", &parse_update_subresource},
      {"ID3D11DeviceContext::OMSetRenderTargets", &parse_om_set_render_targets},
      {"ID3D11DeviceContext::RSSetViewports", &parse_rs_set_viewports},
      {"ID3D11DeviceContext::ClearRenderTargetView", &parse_clear_render_target_view},
      {"ID3D11DeviceContext::ClearDepthStencilView", &parse_clear_depth_stencil_view},
      {"ID3D11DeviceContext::IASetInputLayout", &parse_ia_set_input_layout},
      {"ID3D11DeviceContext::IASetVertexBuffers", &parse_ia_set_vertex_buffers},
      {"ID3D11DeviceContext::IASetIndexBuffer", &parse_ia_set_index_buffer},
      {"ID3D11DeviceContext::IASetPrimitiveTopology", &parse_ia_set_primitive_topology},
      {"ID3D11DeviceContext::VSSetShader", &parse_set_shader},
      {"ID3D11DeviceContext::PSSetShader", &parse_set_shader},
      {"ID3D11DeviceContext::VSSetConstantBuffers", &parse_set_constant_buffers},
      {"ID3D11DeviceContext::PSSetConstantBuffers", &parse_set_constant_buffers},
      {"ID3D11DeviceContext::PSSetShaderResources", &parse_ps_set_shader_resources},
      {"ID3D11DeviceContext::PSSetSamplers", &parse_ps_set_samplers},
      {"ID3D11DeviceContext::OMSetDepthStencilState", &parse_om_set_depth_stencil_state},
      {"ID3D11DeviceContext::OMSetBlendState", &parse_om_set_blend_state},
      {"ID3D11DeviceContext::RSSetState", &parse_rs_set_state},
      {"ID3D11DeviceContext::RSSetScissorRects", &parse_rs_set_scissor_rects},
      {"ID3D11DeviceContext::Draw", &parse_draw},
      {"ID3D11DeviceContext::DrawIndexed", &parse_draw_indexed},
      {"ID3D11DeviceContext::DrawIndexedInstanced", &parse_draw_indexed_instanced},
      {"ID3D11DeviceContext::CopyResource", &parse_copy_resource},
      {"ID3D11DeviceContext::ResolveSubresource", &parse_resolve_subresource},
      {"IDXGISwapChain::Present", &parse_present},
  };
  return handlers;
}

bool parse_boundary_event(const trace::EventRecord &event, const json &payload, D3D11ReplayPlan &plan, std::string &error)
{
  switch (event.boundary) {
  case trace::BoundaryKind::Frame: {
    auto command = make_command_header<FrameBoundaryCommand>(event);
    command.header.label = "Frame";
    command.label = payload.value("label", std::string());
    if (!require_payload_key(event, payload, "frame_index", error)) {
      return false;
    }
    command.frame_index = parse_frame_index(payload).value_or(0);
    if (command.label == "FrameBegin") {
      if (command.frame_index != plan.frame_begin_count) {
        error = record_prefix(event) + ": FrameBegin frame_index is not contiguous";
        return false;
      }
      if (plan.open_frames.find(command.frame_index) != plan.open_frames.end()) {
        error = record_prefix(event) + ": duplicate FrameBegin for frame_index";
        return false;
      }
      plan.open_frames.emplace(command.frame_index, true);
      ++plan.frame_begin_count;
    } else if (command.label == "FrameEnd") {
      if (command.frame_index != plan.frame_end_count) {
        error = record_prefix(event) + ": FrameEnd frame_index is not contiguous";
        return false;
      }
      const auto open_frame = plan.open_frames.find(command.frame_index);
      if (open_frame == plan.open_frames.end()) {
        error = record_prefix(event) + ": FrameEnd is missing matching FrameBegin";
        return false;
      }
      if (plan.presented_frames.find(command.frame_index) == plan.presented_frames.end()) {
        error = record_prefix(event) + ": FrameEnd is missing matching Present boundary";
        return false;
      }
      plan.open_frames.erase(open_frame);
      plan.presented_frames.erase(command.frame_index);
      ++plan.frame_end_count;
    } else {
      error = record_prefix(event) + ": unsupported Frame boundary label";
      return false;
    }
    plan.commands.emplace_back(std::move(command));
    return true;
  }
  case trace::BoundaryKind::Present: {
    auto command = make_command_header<PresentBoundaryCommand>(event);
    command.header.label = "Present";
    command.label = payload.value("label", std::string());
    if (!require_payload_key(event, payload, "frame_index", error)) {
      return false;
    }
    if (!require_payload_key(event, payload, "sync_interval", error) ||
        !require_payload_key(event, payload, "flags", error)) {
      return false;
    }
    command.frame_index = parse_frame_index(payload).value_or(0);
    if (command.frame_index != plan.present_boundary_count) {
      error = record_prefix(event) + ": Present boundary frame_index is not contiguous";
      return false;
    }
    if (command.frame_index >= plan.present_call_count) {
      error = record_prefix(event) + ": Present boundary is missing matching IDXGISwapChain::Present call";
      return false;
    }
    command.sync_interval = payload.value("sync_interval", 0u);
    command.flags = payload.value("flags", 0u);
    if (plan.present_sync_intervals[command.frame_index] != command.sync_interval ||
        plan.present_flags[command.frame_index] != command.flags) {
      error = record_prefix(event) + ": Present boundary does not match captured IDXGISwapChain::Present parameters";
      return false;
    }
    if (plan.open_frames.find(command.frame_index) == plan.open_frames.end()) {
      error = record_prefix(event) + ": Present boundary is missing matching FrameBegin";
      return false;
    }
    if (plan.presented_frames.find(command.frame_index) != plan.presented_frames.end()) {
      error = record_prefix(event) + ": duplicate Present boundary for frame_index";
      return false;
    }
    plan.presented_frames.emplace(command.frame_index, true);
    ++plan.present_boundary_count;
    plan.commands.emplace_back(std::move(command));
    return true;
  }
  case trace::BoundaryKind::DebugMarker: {
    auto command = make_command_header<DebugMarkerCommand>(event);
    command.header.label = "DebugMarker";
    command.label = payload.value("label", std::string());
    command.scene_name = payload.value("scene_name", std::string());
    command.dx_mode = payload.value("dx_mode", std::string());
    command.phase = payload.value("phase", std::string());
    plan.commands.emplace_back(std::move(command));
    return true;
  }
  default:
    error = record_prefix(event) + ": unsupported boundary kind";
    return false;
  }
}

} // namespace

bool build_d3d11_replay_plan(
    const trace::TraceBundleReader &reader,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  plan.commands.clear();
  plan.present_call_count = 0;
  plan.present_boundary_count = 0;
  plan.frame_begin_count = 0;
  plan.frame_end_count = 0;
  plan.present_sync_intervals.clear();
  plan.present_flags.clear();
  plan.open_frames.clear();
  plan.presented_frames.clear();
  plan.commands.reserve(reader.events().size());

  for (const auto &event : reader.events()) {
    json payload;
    if (!payload_to_json(event, payload, error)) {
      return false;
    }

    if (event.kind == trace::EventKind::Boundary) {
      if (!parse_boundary_event(event, payload, plan, error)) {
        return false;
      }
      continue;
    }

    const auto handler = call_handlers().find(event.callsite.function_name);
    if (handler == call_handlers().end()) {
      error = record_prefix(event) + ": unsupported function";
      return false;
    }
    if (!handler->second(reader, event, payload, plan, error)) {
      return false;
    }
  }

  if (plan.present_call_count != plan.present_boundary_count) {
    error = "D3D11 present boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (plan.frame_begin_count != plan.frame_end_count) {
    error = "D3D11 frame boundary count does not match";
    return false;
  }
  if (plan.frame_begin_count != plan.present_call_count) {
    error = "D3D11 frame boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (!plan.open_frames.empty() || !plan.presented_frames.empty()) {
    error = "D3D11 frame boundaries are not fully closed";
    return false;
  }

  error.clear();
  return true;
}

} // namespace apitrace::replay::internal
