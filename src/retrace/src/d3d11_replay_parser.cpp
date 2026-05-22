#include "retrace/src/d3d11_replay_parser.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string_view>

namespace apitrace::replay::internal {

namespace {

using json = nlohmann::json;

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

bool require_object_ref_count(
    const trace::EventRecord &event,
    std::size_t expected,
    std::string &error)
{
  if (event.object_refs.size() < expected) {
    std::ostringstream message;
    message << record_prefix(event) << ": expected at least " << expected << " object refs";
    error = message.str();
    return false;
  }
  return true;
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

} // namespace

bool build_d3d11_replay_plan(
    const trace::TraceBundleReader &reader,
    D3D11ReplayPlan &plan,
    std::string &error)
{
  plan.commands.clear();
  plan.commands.reserve(reader.events().size());

  for (const auto &event : reader.events()) {
    json payload;
    if (!payload_to_json(event, payload, error)) {
      return false;
    }

    if (event.kind == trace::EventKind::Boundary) {
      if (event.boundary == trace::BoundaryKind::Frame) {
        FrameBoundaryCommand command;
        command.header.sequence = event.callsite.sequence;
        command.header.label = "Frame";
        command.label = payload.value("label", std::string());
        command.frame_index = parse_frame_index(payload).value_or(0);
        plan.commands.emplace_back(std::move(command));
        continue;
      }
      if (event.boundary == trace::BoundaryKind::Present) {
        PresentBoundaryCommand command;
        command.header.sequence = event.callsite.sequence;
        command.header.label = "Present";
        command.label = payload.value("label", std::string());
        command.frame_index = parse_frame_index(payload).value_or(0);
        plan.commands.emplace_back(std::move(command));
        continue;
      }

      error = record_prefix(event) + ": unsupported boundary kind";
      return false;
    }

    const std::string &function = event.callsite.function_name;

    if (function == "D3D11CreateDeviceAndSwapChain") {
      if (!require_object_ref_count(event, 3, error)) {
        return false;
      }
      const auto swap_chain = payload.find("swap_chain");
      if (swap_chain == payload.end() || !swap_chain->is_object()) {
        error = record_prefix(event) + ": missing swap_chain payload";
        return false;
      }

      CreateDeviceAndSwapChainCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
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
      continue;
    }

    if (function == "IDXGISwapChain::GetBuffer") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      GetBufferCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.swap_chain_id = event.object_refs[0];
      command.resource_id = event.object_refs[1];
      command.buffer_index = payload.value("buffer_index", 0u);
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11Device::CreateRenderTargetView") {
      if (!require_object_ref_count(event, 3, error)) {
        return false;
      }
      CreateRenderTargetViewCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.device_id = event.object_refs[0];
      command.resource_id = event.object_refs[1];
      command.view_id = event.object_refs[2];
      command.desc_present = payload.value("desc_present", false);
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11Device::CreateInputLayout") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      CreateInputLayoutCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
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
      continue;
    }

    if (function == "ID3D11Device::CreateVertexShader" || function == "ID3D11Device::CreatePixelShader") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      CreateShaderCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.device_id = event.object_refs[0];
      command.shader_id = event.object_refs[1];
      command.shader_path = resolve_asset_path(reader, event, payload, "shader_path", error);
      if (!error.empty()) {
        return false;
      }
      command.vertex_stage = function == "ID3D11Device::CreateVertexShader";
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11Device::CreateBuffer") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      CreateBufferCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
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
      continue;
    }

    if (function == "ID3D11DeviceContext::Map") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      MapCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.resource_id = event.object_refs[1];
      command.subresource = payload.value("subresource", 0u);
      command.map_type = payload.value("map_type", std::string("OTHER"));
      command.map_flags = payload.value("map_flags", 0u);
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::Unmap") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      UnmapCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.resource_id = event.object_refs[1];
      command.subresource = payload.value("subresource", 0u);
      command.snapshot_path = resolve_asset_path(reader, event, payload, "snapshot_path", error);
      if (!error.empty()) {
        return false;
      }
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::OMSetRenderTargets") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      SetRenderTargetsCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.has_depth_stencil = payload.value("has_depth_stencil", false);
      command.render_target_view_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::RSSetViewports") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      SetViewportsCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      if (!parse_viewports(event, payload, command.viewports, error)) {
        return false;
      }
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::ClearRenderTargetView") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      const auto color_it = payload.find("color");
      if (color_it == payload.end() || !color_it->is_array() || color_it->size() != 4) {
        error = record_prefix(event) + ": missing clear color";
        return false;
      }

      ClearRenderTargetViewCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.render_target_view_id = event.object_refs[1];
      for (std::size_t index = 0; index < 4; ++index) {
        command.color[index] = (*color_it)[index].get<float>();
      }
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::IASetInputLayout") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      SetInputLayoutCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.input_layout_id = event.object_refs[1];
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::IASetVertexBuffers") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      SetVertexBuffersCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.start_slot = payload.value("start_slot", 0u);
      if (!parse_vertex_buffer_bindings(event, payload, command.bindings, error)) {
        return false;
      }
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::IASetPrimitiveTopology") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      SetPrimitiveTopologyCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.topology = payload.value("topology", std::string("OTHER"));
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::VSSetShader" || function == "ID3D11DeviceContext::PSSetShader") {
      if (!require_object_ref_count(event, 2, error)) {
        return false;
      }
      if (payload.value("class_instance_count", 0u) != 0u) {
        error = record_prefix(event) + ": class instances are unsupported";
        return false;
      }

      SetShaderCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.shader_id = event.object_refs[1];
      command.vertex_stage = function == "ID3D11DeviceContext::VSSetShader";
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::VSSetConstantBuffers" || function == "ID3D11DeviceContext::PSSetConstantBuffers") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      SetConstantBuffersCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.start_slot = payload.value("start_slot", 0u);
      command.vertex_stage = function == "ID3D11DeviceContext::VSSetConstantBuffers";
      command.buffer_ids.assign(event.object_refs.begin() + 1, event.object_refs.end());
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "ID3D11DeviceContext::Draw") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      DrawCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.context_id = event.object_refs[0];
      command.vertex_count = payload.value("vertex_count", 0u);
      command.start_vertex_location = payload.value("start_vertex_location", 0u);
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    if (function == "IDXGISwapChain::Present") {
      if (!require_object_ref_count(event, 1, error)) {
        return false;
      }
      PresentCommand command;
      command.header.sequence = event.callsite.sequence;
      command.header.label = function;
      command.swap_chain_id = event.object_refs[0];
      command.sync_interval = payload.value("sync_interval", 0u);
      command.flags = payload.value("flags", 0u);
      plan.commands.emplace_back(std::move(command));
      continue;
    }

    error = record_prefix(event) + ": unsupported function";
    return false;
  }

  error.clear();
  return true;
}

} // namespace apitrace::replay::internal
