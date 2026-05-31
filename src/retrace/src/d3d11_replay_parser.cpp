#include "retrace/src/d3d11_replay_parser.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <unordered_set>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace apitrace::replay::internal {

namespace {

using json = nlohmann::json;

struct D3D11ReplayParseContext {
  const trace::TraceBundleReader &reader;
  std::unordered_map<trace::BlobId, std::filesystem::path> asset_paths_by_blob;
};

using ParseCallHandler = bool (*)(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error);

std::unordered_map<trace::BlobId, std::filesystem::path> build_asset_path_index(
    const trace::TraceBundleReader &reader)
{
  std::unordered_map<trace::BlobId, std::filesystem::path> asset_paths;
  asset_paths.reserve(reader.assets().size() + reader.metal_assets().size());
  for (const auto &asset : reader.assets()) {
    if (asset.blob_id != 0 && !asset.relative_path.empty()) {
      asset_paths[asset.blob_id] = asset.relative_path;
    }
  }
  for (const auto &asset : reader.metal_assets()) {
    if (asset.blob_id != 0 && !asset.relative_path.empty()) {
      asset_paths[asset.blob_id] = asset.relative_path;
    }
  }
  return asset_paths;
}

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
    const D3D11ReplayParseContext &context,
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
  const std::filesystem::path relative_path(it->get<std::string>());
  if (relative_path.is_absolute()) {
    error = record_prefix(event) + ": " + std::string(key) + " references an absolute asset path";
    return {};
  }
  for (const auto &part : relative_path) {
    if (part == "..") {
      error = record_prefix(event) + ": " + std::string(key) + " references an unsafe asset path";
      return {};
    }
  }
  if (event.blob_refs.empty()) {
    error = record_prefix(event) + ": " + std::string(key) + " is missing blob_refs";
    return {};
  }

  std::unordered_set<std::string> event_asset_paths;
  event_asset_paths.reserve(event.blob_refs.size());
  for (const auto blob_id : event.blob_refs) {
    if (blob_id == 0) {
      error = record_prefix(event) + ": " + std::string(key) + " has zero blob_ref";
      return {};
    }

    const auto path_it = context.asset_paths_by_blob.find(blob_id);
    if (path_it == context.asset_paths_by_blob.end()) {
      error = record_prefix(event) + ": " + std::string(key) + " blob_ref does not resolve";
      return {};
    }
    event_asset_paths.insert(path_it->second.generic_string());
  }

  const auto relative_key = relative_path.generic_string();
  if (event_asset_paths.find(relative_key) == event_asset_paths.end()) {
    error = record_prefix(event) + ": " + std::string(key) + " blob_ref does not match " + relative_key;
    return {};
  }
  return context.reader.layout().root_path / relative_path;
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

bool parse_u32_field(
    const trace::EventRecord &event,
    const json &source,
    std::string_view key,
    std::uint32_t &value,
    std::string &error)
{
  const auto it = source.find(std::string(key));
  if (it == source.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
    error = record_prefix(event) + ": missing uint32 field " + std::string(key);
    return false;
  }
  std::uint64_t parsed = 0;
  if (it->is_number_unsigned()) {
    parsed = it->get<std::uint64_t>();
  } else {
    const auto signed_value = it->get<std::int64_t>();
    if (signed_value < 0) {
      error = record_prefix(event) + ": uint32 field " + std::string(key) + " is negative";
      return false;
    }
    parsed = static_cast<std::uint64_t>(signed_value);
  }
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    error = record_prefix(event) + ": uint32 field " + std::string(key) + " is out of range";
    return false;
  }
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parse_i32_field(
    const trace::EventRecord &event,
    const json &source,
    std::string_view key,
    std::int32_t &value,
    std::string &error)
{
  const auto it = source.find(std::string(key));
  if (it == source.end() || !it->is_number_integer()) {
    error = record_prefix(event) + ": missing int32 field " + std::string(key);
    return false;
  }
  const auto parsed = it->get<std::int64_t>();
  if (parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    error = record_prefix(event) + ": int32 field " + std::string(key) + " is out of range";
    return false;
  }
  value = static_cast<std::int32_t>(parsed);
  return true;
}

bool parse_float_field(
    const trace::EventRecord &event,
    const json &source,
    std::string_view key,
    float &value,
    std::string &error)
{
  const auto it = source.find(std::string(key));
  if (it == source.end() || !it->is_number()) {
    error = record_prefix(event) + ": missing float field " + std::string(key);
    return false;
  }
  value = it->get<float>();
  return true;
}

bool parse_string_field(
    const trace::EventRecord &event,
    const json &source,
    std::string_view key,
    std::string &value,
    std::string &error)
{
  const auto it = source.find(std::string(key));
  if (it == source.end() || !it->is_string()) {
    error = record_prefix(event) + ": missing string field " + std::string(key);
    return false;
  }
  value = it->get<std::string>();
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
    if (!parse_string_field(event, entry, "semantic_name", element.semantic_name, error) ||
        !parse_u32_field(event, entry, "semantic_index", element.semantic_index, error) ||
        !parse_u32_field(event, entry, "format", element.format, error) ||
        !parse_u32_field(event, entry, "input_slot", element.input_slot, error) ||
        !parse_u32_field(event, entry, "aligned_byte_offset", element.aligned_byte_offset, error) ||
        !parse_u32_field(event, entry, "input_slot_class", element.input_slot_class, error) ||
        !parse_u32_field(event, entry, "instance_data_step_rate", element.instance_data_step_rate, error)) {
      return false;
    }
    elements.push_back(std::move(element));
  }
  return true;
}

bool parse_texture2d_desc(const trace::EventRecord &event, const json &source, Texture2DDesc &desc, std::string &error)
{
  if (!parse_u32_field(event, source, "width", desc.width, error) ||
      !parse_u32_field(event, source, "height", desc.height, error) ||
      !parse_u32_field(event, source, "mip_levels", desc.mip_levels, error) ||
      !parse_u32_field(event, source, "array_size", desc.array_size, error) ||
      !parse_u32_field(event, source, "format", desc.format, error) ||
      !parse_u32_field(event, source, "sample_count", desc.sample_count, error) ||
      !parse_u32_field(event, source, "sample_quality", desc.sample_quality, error) ||
      !parse_u32_field(event, source, "usage", desc.usage, error) ||
      !parse_u32_field(event, source, "bind_flags", desc.bind_flags, error) ||
      !parse_u32_field(event, source, "cpu_access_flags", desc.cpu_access_flags, error) ||
      !parse_u32_field(event, source, "misc_flags", desc.misc_flags, error)) {
    return false;
  }
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
  if (!parse_u32_field(event, source, "format", desc.format, error) ||
      !parse_u32_field(event, source, "view_dimension", desc.view_dimension, error)) {
    return false;
  }
  const auto texture2d = source.find("texture2d");
  if (texture2d != source.end()) {
    if (!texture2d->is_object()) {
      error = record_prefix(event) + ": shader resource view texture2d desc must be an object";
      return false;
    }
    desc.has_texture2d = true;
    if (!parse_u32_field(event, *texture2d, "most_detailed_mip", desc.texture2d_most_detailed_mip, error) ||
        !parse_u32_field(event, *texture2d, "mip_levels", desc.texture2d_mip_levels, error)) {
      return false;
    }
  }
  return true;
}

bool parse_render_target_view_desc(
    const trace::EventRecord &event,
    const json &source,
    CreateRenderTargetViewCommand &command,
    std::string &error)
{
  if (!parse_u32_field(event, source, "format", command.format, error) ||
      !parse_u32_field(event, source, "view_dimension", command.view_dimension, error)) {
    return false;
  }
  const auto texture2d = source.find("texture2d");
  if (texture2d != source.end()) {
    if (!texture2d->is_object()) {
      error = record_prefix(event) + ": render target view texture2d desc must be an object";
      return false;
    }
    if (!parse_u32_field(event, *texture2d, "mip_slice", command.texture2d_mip_slice, error)) {
      return false;
    }
  }
  return true;
}

bool parse_depth_stencil_view_desc(
    const trace::EventRecord &event,
    const json &source,
    DepthStencilViewDesc &desc,
    std::string &error)
{
  if (!parse_u32_field(event, source, "format", desc.format, error) ||
      !parse_u32_field(event, source, "view_dimension", desc.view_dimension, error) ||
      !parse_u32_field(event, source, "flags", desc.flags, error)) {
    return false;
  }
  const auto texture2d = source.find("texture2d");
  if (texture2d != source.end()) {
    if (!texture2d->is_object()) {
      error = record_prefix(event) + ": depth stencil view texture2d desc must be an object";
      return false;
    }
    desc.has_texture2d = true;
    if (!parse_u32_field(event, *texture2d, "mip_slice", desc.texture2d_mip_slice, error)) {
      return false;
    }
  }
  return true;
}

bool parse_sampler_state_desc(
    const trace::EventRecord &event,
    const json &source,
    SamplerStateDesc &desc,
    std::string &error)
{
  if (!parse_u32_field(event, source, "filter", desc.filter, error) ||
      !parse_u32_field(event, source, "address_u", desc.address_u, error) ||
      !parse_u32_field(event, source, "address_v", desc.address_v, error) ||
      !parse_u32_field(event, source, "address_w", desc.address_w, error) ||
      !parse_float_field(event, source, "mip_lod_bias", desc.mip_lod_bias, error) ||
      !parse_u32_field(event, source, "max_anisotropy", desc.max_anisotropy, error) ||
      !parse_u32_field(event, source, "comparison_func", desc.comparison_func, error) ||
      !parse_float4_field(event, source, "border_color", desc.border_color, error) ||
      !parse_float_field(event, source, "min_lod", desc.min_lod, error) ||
      !parse_float_field(event, source, "max_lod", desc.max_lod, error)) {
    return false;
  }
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
    std::uint32_t write_mask = 0;
    if (!parse_u32_field(event, entry, "src_blend", target.src_blend, error) ||
        !parse_u32_field(event, entry, "dest_blend", target.dest_blend, error) ||
        !parse_u32_field(event, entry, "blend_op", target.blend_op, error) ||
        !parse_u32_field(event, entry, "src_blend_alpha", target.src_blend_alpha, error) ||
        !parse_u32_field(event, entry, "dest_blend_alpha", target.dest_blend_alpha, error) ||
        !parse_u32_field(event, entry, "blend_op_alpha", target.blend_op_alpha, error) ||
        !parse_u32_field(event, entry, "write_mask", write_mask, error)) {
      return false;
    }
    if (write_mask > std::numeric_limits<std::uint8_t>::max()) {
      error = record_prefix(event) + ": uint8 field write_mask is out of range";
      return false;
    }
    target.write_mask = static_cast<std::uint8_t>(write_mask);
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
  std::uint32_t stencil_read_mask = 0;
  std::uint32_t stencil_write_mask = 0;
  if (!parse_u32_field(event, source, "depth_write_mask", desc.depth_write_mask, error) ||
      !parse_u32_field(event, source, "depth_func", desc.depth_func, error) ||
      !parse_u32_field(event, source, "stencil_read_mask", stencil_read_mask, error) ||
      !parse_u32_field(event, source, "stencil_write_mask", stencil_write_mask, error)) {
    return false;
  }
  if (stencil_read_mask > std::numeric_limits<std::uint8_t>::max() ||
      stencil_write_mask > std::numeric_limits<std::uint8_t>::max()) {
    error = record_prefix(event) + ": stencil mask is out of range";
    return false;
  }
  desc.stencil_read_mask = static_cast<std::uint8_t>(stencil_read_mask);
  desc.stencil_write_mask = static_cast<std::uint8_t>(stencil_write_mask);
  return true;
}

bool parse_rasterizer_state_desc(
    const trace::EventRecord &event,
    const json &source,
    RasterizerStateDesc &desc,
    std::string &error)
{
  if (!parse_u32_field(event, source, "fill_mode", desc.fill_mode, error) ||
      !parse_u32_field(event, source, "cull_mode", desc.cull_mode, error) ||
      !parse_i32_field(event, source, "depth_bias", desc.depth_bias, error) ||
      !parse_float_field(event, source, "depth_bias_clamp", desc.depth_bias_clamp, error) ||
      !parse_float_field(event, source, "slope_scaled_depth_bias", desc.slope_scaled_depth_bias, error) ||
      !parse_bool_field(event, source, "front_counter_clockwise", desc.front_counter_clockwise, error) ||
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
      if (!parse_float_field(event, entry, "top_left_x", viewport.top_left_x, error) ||
          !parse_float_field(event, entry, "top_left_y", viewport.top_left_y, error) ||
          !parse_float_field(event, entry, "width", viewport.width, error) ||
          !parse_float_field(event, entry, "height", viewport.height, error) ||
          !parse_float_field(event, entry, "min_depth", viewport.min_depth, error) ||
          !parse_float_field(event, entry, "max_depth", viewport.max_depth, error)) {
        return false;
      }
      viewports.push_back(viewport);
    }
    return true;
  }

  std::uint32_t num_viewports = 0;
  if (parse_u32_field(event, payload, "num_viewports", num_viewports, error) && num_viewports == 1) {
    ViewportDesc viewport;
    if (!parse_float_field(event, payload, "first_width", viewport.width, error) ||
        !parse_float_field(event, payload, "first_height", viewport.height, error)) {
      return false;
    }
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
    if (!parse_i32_field(event, entry, "left", rect.left, error) ||
        !parse_i32_field(event, entry, "top", rect.top, error) ||
        !parse_i32_field(event, entry, "right", rect.right, error) ||
        !parse_i32_field(event, entry, "bottom", rect.bottom, error)) {
      return false;
    }
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
      if (!parse_u32_field(event, entry, "stride", binding.stride, error) ||
          !parse_u32_field(event, entry, "offset", binding.offset, error)) {
        return false;
      }
      const auto object_id = entry.find("object_id");
      if (object_id == entry.end() || (!object_id->is_number_unsigned() && !object_id->is_number_integer())) {
        error = record_prefix(event) + ": missing object_id in vertex buffer binding";
        return false;
      }
      binding.buffer_id = object_id->get<trace::ObjectId>();
      bindings.push_back(binding);
    }
    return true;
  }

  std::uint32_t num_buffers = 0;
  if (parse_u32_field(event, payload, "num_buffers", num_buffers, error) && num_buffers == 1 &&
      event.object_refs.size() >= 2) {
    VertexBufferBinding binding;
    binding.buffer_id = event.object_refs[1];
    if (!parse_u32_field(event, payload, "first_stride", binding.stride, error) ||
        !parse_u32_field(event, payload, "first_offset", binding.offset, error)) {
      return false;
    }
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
  if (!parse_string_field(event, payload, "driver_type", command.driver_type, error) ||
      !parse_u32_field(event, payload, "flags", command.flags, error) ||
      !parse_u32_field(event, payload, "sdk_version", command.sdk_version, error) ||
      !parse_string_field(event, payload, "feature_level", command.feature_level, error) ||
      !parse_u32_field(event, *swap_chain, "width", command.swap_chain.width, error) ||
      !parse_u32_field(event, *swap_chain, "height", command.swap_chain.height, error) ||
      !parse_u32_field(event, *swap_chain, "format", command.swap_chain.format, error) ||
      !parse_u32_field(event, *swap_chain, "sample_count", command.swap_chain.sample_count, error) ||
      !parse_u32_field(event, *swap_chain, "sample_quality", command.swap_chain.sample_quality, error) ||
      !parse_u32_field(event, *swap_chain, "buffer_usage", command.swap_chain.buffer_usage, error) ||
      !parse_u32_field(event, *swap_chain, "buffer_count", command.swap_chain.buffer_count, error) ||
      !parse_u32_field(event, *swap_chain, "swap_effect", command.swap_chain.swap_effect, error) ||
      !parse_u32_field(event, *swap_chain, "flags", command.swap_chain.flags, error) ||
      !parse_bool_field(event, *swap_chain, "windowed", command.swap_chain.windowed, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_get_buffer(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<GetBufferCommand>(event);
  command.swap_chain_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  if (!parse_u32_field(event, payload, "buffer_index", command.buffer_index, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_render_target_view(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CreateRenderTargetViewCommand>(event);
  command.device_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.view_id = event.object_refs[2];
  if (!parse_bool_field(event, payload, "desc_present", command.desc_present, error)) {
    return false;
  }
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
    const D3D11ReplayParseContext &context,
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
  if (!parse_bool_field(event, payload, "has_initial_data", command.has_initial_data, error)) {
    return false;
  }
  if (!parse_texture2d_desc(event, *desc, command.desc, error)) {
    return false;
  }
  if (command.has_initial_data) {
    command.initial_data_path = resolve_asset_path(context, event, payload, "initial_data_path", error);
    if (!error.empty()) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_shader_resource_view(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CreateShaderResourceViewCommand>(event);
  command.device_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.view_id = event.object_refs[2];
  if (!parse_bool_field(event, payload, "desc_present", command.desc_present, error)) {
    return false;
  }
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<CreateDepthStencilViewCommand>(event);
  command.device_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  command.view_id = event.object_refs[2];
  if (!parse_bool_field(event, payload, "desc_present", command.desc_present, error)) {
    return false;
  }
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
    const D3D11ReplayParseContext &context,
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
  command.shader_path = resolve_asset_path(context, event, payload, "shader_path", error);
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
    const D3D11ReplayParseContext &context,
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
  command.shader_path = resolve_asset_path(context, event, payload, "shader_path", error);
  if (!error.empty()) {
    return false;
  }
  command.vertex_stage = event.callsite.function_name == "ID3D11Device::CreateVertexShader";
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_buffer(
    const D3D11ReplayParseContext &context,
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
  if (!parse_u32_field(event, payload, "byte_width", command.byte_width, error) ||
      !parse_u32_field(event, payload, "usage", command.usage, error) ||
      !parse_u32_field(event, payload, "bind_flags", command.bind_flags, error) ||
      !parse_u32_field(event, payload, "cpu_access_flags", command.cpu_access_flags, error) ||
      !parse_bool_field(event, payload, "has_initial_data", command.has_initial_data, error)) {
    return false;
  }
  if (command.has_initial_data) {
    command.initial_data_path = resolve_asset_path(context, event, payload, "initial_data_path", error);
    if (!error.empty()) {
      return false;
    }
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_create_blend_state(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<MapCommand>(event);
  command.context_id = event.object_refs[0];
  command.resource_id = event.object_refs[1];
  if (!parse_u32_field(event, payload, "subresource", command.subresource, error) ||
      !parse_string_field(event, payload, "map_type", command.map_type, error) ||
      !parse_u32_field(event, payload, "map_flags", command.map_flags, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_unmap(
    const D3D11ReplayParseContext &context,
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
  if (!parse_u32_field(event, payload, "subresource", command.subresource, error)) {
    return false;
  }
  command.snapshot_path = resolve_asset_path(context, event, payload, "snapshot_path", error);
  if (!error.empty()) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_update_subresource(
    const D3D11ReplayParseContext &context,
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
  std::string resource_class;
  if (!parse_string_field(event, payload, "resource_class", resource_class, error) ||
      !parse_u32_field(event, payload, "dst_subresource", command.dst_subresource, error) ||
      !parse_u32_field(event, payload, "src_row_pitch", command.src_row_pitch, error) ||
      !parse_u32_field(event, payload, "src_depth_pitch", command.src_depth_pitch, error) ||
      !parse_bool_field(event, payload, "has_dst_box", command.has_dst_box, error)) {
    return false;
  }
  command.resource_class = parse_resource_class_name(resource_class);
  if (command.resource_class == ReplayResourceClass::Unknown) {
    error = record_prefix(event) + ": unsupported resource_class " + resource_class;
    return false;
  }
  if (command.has_dst_box) {
    const json *box = nullptr;
    if (!require_json_object(event, payload, "dst_box", box, error)) {
      return false;
    }
    if (!parse_u32_field(event, *box, "left", command.dst_box.left, error) ||
        !parse_u32_field(event, *box, "top", command.dst_box.top, error) ||
        !parse_u32_field(event, *box, "front", command.dst_box.front, error) ||
        !parse_u32_field(event, *box, "right", command.dst_box.right, error) ||
        !parse_u32_field(event, *box, "bottom", command.dst_box.bottom, error) ||
        !parse_u32_field(event, *box, "back", command.dst_box.back, error)) {
      return false;
    }
  }
  command.data_path = resolve_asset_path(context, event, payload, "data_path", error);
  if (!error.empty()) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_clear_state(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  (void)payload;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<ClearStateCommand>(event);
  command.context_id = event.object_refs[0];
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_om_set_render_targets(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetRenderTargetsCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_bool_field(event, payload, "has_depth_stencil", command.has_depth_stencil, error)) {
    return false;
  }
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<ClearDepthStencilViewCommand>(event);
  command.context_id = event.object_refs[0];
  command.depth_stencil_view_id = event.object_refs[1];
  std::uint32_t stencil = 0;
  if (!parse_u32_field(event, payload, "clear_flags", command.clear_flags, error) ||
      !parse_float_field(event, payload, "depth", command.depth, error) ||
      !parse_u32_field(event, payload, "stencil", stencil, error)) {
    return false;
  }
  if (stencil > std::numeric_limits<std::uint8_t>::max()) {
    error = record_prefix(event) + ": uint8 field stencil is out of range";
    return false;
  }
  command.stencil = static_cast<std::uint8_t>(stencil);
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_input_layout(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetVertexBuffersCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "start_slot", command.start_slot, error)) {
    return false;
  }
  if (!parse_vertex_buffer_bindings(event, payload, command.bindings, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_index_buffer(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  auto command = make_command_header<SetIndexBufferCommand>(event);
  command.context_id = event.object_refs[0];
  command.buffer_id = event.object_refs[1];
  if (!parse_u32_field(event, payload, "format", command.format, error) ||
      !parse_u32_field(event, payload, "offset", command.offset, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ia_set_primitive_topology(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetPrimitiveTopologyCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_string_field(event, payload, "topology", command.topology, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_set_shader(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 2, error)) {
    return false;
  }
  std::uint32_t class_instance_count = 0;
  if (!parse_u32_field(event, payload, "class_instance_count", class_instance_count, error)) {
    return false;
  }
  if (class_instance_count != 0u) {
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetConstantBuffersCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "start_slot", command.start_slot, error)) {
    return false;
  }
  command.vertex_stage = event.callsite.function_name == "ID3D11DeviceContext::VSSetConstantBuffers";
  command.buffer_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ps_set_shader_resources(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetShaderResourcesCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "start_slot", command.start_slot, error)) {
    return false;
  }
  command.shader_resource_view_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_ps_set_samplers(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetSamplersCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "start_slot", command.start_slot, error)) {
    return false;
  }
  command.sampler_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_om_set_depth_stencil_state(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetDepthStencilStateCommand>(event);
  command.context_id = event.object_refs[0];
  command.depth_stencil_state_id = event.object_refs.size() >= 2 ? event.object_refs[1] : 0;
  if (!parse_u32_field(event, payload, "stencil_ref", command.stencil_ref, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_om_set_blend_state(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<SetBlendStateCommand>(event);
  command.context_id = event.object_refs[0];
  command.blend_state_id = event.object_refs.size() >= 2 ? event.object_refs[1] : 0;
  if (!parse_u32_field(event, payload, "sample_mask", command.sample_mask, error) ||
      !parse_float4_field(event, payload, "blend_factor", command.blend_factor, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_rs_set_state(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<DrawCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "vertex_count", command.vertex_count, error) ||
      !parse_u32_field(event, payload, "start_vertex_location", command.start_vertex_location, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_draw_indexed(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<DrawIndexedCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "index_count", command.index_count, error) ||
      !parse_u32_field(event, payload, "start_index_location", command.start_index_location, error) ||
      !parse_i32_field(event, payload, "base_vertex_location", command.base_vertex_location, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_draw_indexed_instanced(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  auto command = make_command_header<DrawIndexedInstancedCommand>(event);
  command.context_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "index_count_per_instance", command.index_count_per_instance, error) ||
      !parse_u32_field(event, payload, "instance_count", command.instance_count, error) ||
      !parse_u32_field(event, payload, "start_index_location", command.start_index_location, error) ||
      !parse_i32_field(event, payload, "base_vertex_location", command.base_vertex_location, error) ||
      !parse_u32_field(event, payload, "start_instance_location", command.start_instance_location, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_copy_resource(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
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
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 3, error)) {
    return false;
  }
  auto command = make_command_header<ResolveSubresourceCommand>(event);
  command.context_id = event.object_refs[0];
  command.dst_resource_id = event.object_refs[1];
  command.src_resource_id = event.object_refs[2];
  if (!parse_u32_field(event, payload, "dst_subresource", command.dst_subresource, error) ||
      !parse_u32_field(event, payload, "src_subresource", command.src_subresource, error) ||
      !parse_u32_field(event, payload, "format", command.format, error)) {
    return false;
  }
  plan.commands.emplace_back(std::move(command));
  return true;
}

bool parse_present(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  (void)context;
  if (!require_object_ref_count(event, 1, error)) {
    return false;
  }
  if (!require_payload_key(event, payload, "sync_interval", error) ||
      !require_payload_key(event, payload, "flags", error)) {
    return false;
  }
  auto command = make_command_header<PresentCommand>(event);
  command.swap_chain_id = event.object_refs[0];
  if (!parse_u32_field(event, payload, "sync_interval", command.sync_interval, error) ||
      !parse_u32_field(event, payload, "flags", command.flags, error)) {
    return false;
  }
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

bool parse_resource_blob(
    const D3D11ReplayParseContext &context,
    const trace::EventRecord &event,
    const json &payload,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  if (event.object_debug_name != "D3D11PresentFrame") {
    error = record_prefix(event) + ": unsupported resource blob " + event.object_debug_name;
    return false;
  }
  if (!require_payload_key(event, payload, "frame_index", error) ||
      !require_payload_key(event, payload, "width", error) ||
      !require_payload_key(event, payload, "height", error) ||
      !require_payload_key(event, payload, "row_pitch", error) ||
      !require_payload_key(event, payload, "sync_interval", error) ||
      !require_payload_key(event, payload, "flags", error) ||
      !require_payload_key(event, payload, "format", error)) {
    return false;
  }

  PresentFrameRecord frame;
  frame.frame_index = parse_frame_index(payload).value_or(0);
  if (frame.frame_index != plan.present_frames.size()) {
    error = record_prefix(event) + ": D3D11PresentFrame frame_index is not contiguous";
    return false;
  }
  std::string format;
  if (!parse_u32_field(event, payload, "width", frame.width, error) ||
      !parse_u32_field(event, payload, "height", frame.height, error) ||
      !parse_u32_field(event, payload, "row_pitch", frame.row_pitch, error) ||
      !parse_u32_field(event, payload, "sync_interval", frame.sync_interval, error) ||
      !parse_u32_field(event, payload, "flags", frame.flags, error) ||
      !parse_string_field(event, payload, "format", format, error)) {
    return false;
  }
  if (format != "rgba8") {
    error = record_prefix(event) + ": D3D11PresentFrame format must be rgba8";
    return false;
  }
  frame.frame_path = resolve_asset_path(context, event, payload, "frame_path", error);
  if (!error.empty()) {
    return false;
  }
  if (frame.width == 0 || frame.height == 0 || frame.row_pitch < frame.width * 4u) {
    error = record_prefix(event) + ": D3D11PresentFrame has invalid dimensions";
    return false;
  }
  if (!std::filesystem::is_regular_file(frame.frame_path)) {
    error = record_prefix(event) + ": missing D3D11PresentFrame asset";
    return false;
  }

  std::error_code stat_error;
  const auto frame_size = std::filesystem::file_size(frame.frame_path, stat_error);
  if (stat_error) {
    error = record_prefix(event) + ": failed to stat D3D11PresentFrame asset";
    return false;
  }
  if (frame_size != static_cast<std::uintmax_t>(frame.row_pitch) * static_cast<std::uintmax_t>(frame.height)) {
    error = record_prefix(event) + ": D3D11PresentFrame asset size does not match row_pitch * height";
    return false;
  }

  plan.present_frames.push_back(std::move(frame));
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
      {"ID3D11DeviceContext::ClearState", &parse_clear_state},
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
    if (!parse_string_field(event, payload, "label", command.label, error)) {
      return false;
    }
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
    if (!parse_string_field(event, payload, "label", command.label, error)) {
      return false;
    }
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
    if (!parse_u32_field(event, payload, "sync_interval", command.sync_interval, error) ||
        !parse_u32_field(event, payload, "flags", command.flags, error)) {
      return false;
    }
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
  const D3D11ReplayParseContext context{reader, build_asset_path_index(reader)};
  plan.commands.clear();
  plan.present_call_count = 0;
  plan.present_boundary_count = 0;
  plan.present_frames.clear();
  plan.frame_begin_count = 0;
  plan.frame_end_count = 0;
  plan.present_sync_intervals.clear();
  plan.present_flags.clear();
  plan.open_frames.clear();
  plan.presented_frames.clear();
  plan.commands.reserve(context.reader.events().size());

  for (const auto &event : context.reader.events()) {
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

    if (event.kind == trace::EventKind::ResourceBlob) {
      if (!parse_resource_blob(context, event, payload, plan, error)) {
        return false;
      }
      continue;
    }

    const auto handler = call_handlers().find(event.callsite.function_name);
    if (handler == call_handlers().end()) {
      error = record_prefix(event) + ": unsupported function";
      return false;
    }
    if (!handler->second(context, event, payload, plan, error)) {
      return false;
    }
  }

  if (plan.present_call_count != plan.present_boundary_count) {
    error = "D3D11 present boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (!plan.present_frames.empty() && plan.present_frames.size() != plan.present_call_count) {
    error = "D3D11 present frame asset count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  for (const auto &frame : plan.present_frames) {
    if (frame.frame_index >= plan.present_sync_intervals.size()) {
      error = "D3D11 present frame asset count does not match captured IDXGISwapChain::Present calls";
      return false;
    }
    if (plan.present_sync_intervals[frame.frame_index] != frame.sync_interval ||
        plan.present_flags[frame.frame_index] != frame.flags) {
      error = "D3D11 present frame metadata does not match captured IDXGISwapChain::Present parameters";
      return false;
    }
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
