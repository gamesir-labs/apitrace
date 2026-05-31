#include "payload_object_refs.hpp"

#include <algorithm>
#include <string>

namespace apitrace::trace {

namespace {

bool ends_with(std::string_view text, std::string_view suffix)
{
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

void append_object_ref(std::vector<ObjectId> &refs, ObjectId object_id)
{
  if (object_id == 0) {
    return;
  }
  if (std::find(refs.begin(), refs.end(), object_id) == refs.end()) {
    refs.push_back(object_id);
  }
}

bool is_object_reference_key(std::string_view key)
{
  static constexpr std::string_view kExactKeys[] = {
      "buffer_id",
      "buffer_object_id",
      "color_texture_id",
      "command_buffer_id",
      "depth_stencil_state_id",
      "depth_texture_id",
      "destination_buffer_id",
      "destination_texture",
      "destination_texture_id",
      "drawable_id",
      "encoder_id",
      "fence_id",
      "heap_id",
      "indirect_buffer_id",
      "index_buffer_id",
      "pipeline_state_id",
      "pipeline_state_object_id",
      "resource_id",
      "resolved_resource_object_id",
      "sampler_id",
      "sampler_state_id",
      "sampler_state_object_id",
      "source_buffer",
      "source_buffer_id",
      "source_texture",
      "source_texture_id",
      "texture_id",
      "texture_object_id",
      "resolve_texture_id",
      "stencil_texture_id",
  };
  for (const auto exact_key : kExactKeys) {
    if (key == exact_key) {
      return true;
    }
  }
  return ends_with(key, "_object_id");
}

bool is_object_reference_array_key(std::string_view key)
{
  static constexpr std::string_view kExactKeys[] = {
      "blend_state_ids",
      "buffer_ids",
      "buffer_object_ids",
      "command_allocator_ids",
      "command_buffer_ids",
      "command_list_ids",
      "command_queue_ids",
      "constant_buffer_view_ids",
      "depth_stencil_state_ids",
      "depth_stencil_view_ids",
      "descriptor_heap_ids",
      "encoder_ids",
      "fence_ids",
      "heap_ids",
      "input_layout_ids",
      "object_ids",
      "pipeline_state_ids",
      "pipeline_state_object_ids",
      "rasterizer_state_ids",
      "render_target_view_ids",
      "resource_ids",
      "resource_object_ids",
      "root_signature_ids",
      "sampler_ids",
      "sampler_state_ids",
      "sampler_state_object_ids",
      "shader_ids",
      "shader_resource_view_ids",
      "texture_ids",
      "texture_object_ids",
      "unordered_access_view_ids",
      "view_ids",
  };
  for (const auto exact_key : kExactKeys) {
    if (key == exact_key) {
      return true;
    }
  }
  return ends_with(key, "_object_ids");
}

void collect_render_pass_texture_refs(const nlohmann::json &pass, std::vector<ObjectId> &refs)
{
  if (!pass.is_object()) {
    return;
  }

  if (const auto it = pass.find("render_pass_info"); it != pass.end()) {
    collect_render_pass_texture_refs(*it, refs);
  }
  for (const auto *key : {"color_texture_id", "drawable_id", "depth_texture_id", "stencil_texture_id"}) {
    const auto it = pass.find(key);
    if (it != pass.end() && (it->is_number_unsigned() || it->is_number_integer())) {
      append_object_ref(refs, it->get<ObjectId>());
    }
  }

  const auto colors = pass.find("colors");
  if (colors != pass.end() && colors->is_array()) {
    for (const auto &color : *colors) {
      if (!color.is_object()) {
        continue;
      }
      for (const auto *key : {"texture", "resolve_texture"}) {
        const auto it = color.find(key);
        if (it != color.end() && (it->is_number_unsigned() || it->is_number_integer())) {
          append_object_ref(refs, it->get<ObjectId>());
        }
      }
    }
  }

  for (const auto *attachment_key : {"depth", "stencil"}) {
    const auto attachment = pass.find(attachment_key);
    if (attachment == pass.end() || !attachment->is_object()) {
      continue;
    }
    const auto texture = attachment->find("texture");
    if (texture != attachment->end() && (texture->is_number_unsigned() || texture->is_number_integer())) {
      append_object_ref(refs, texture->get<ObjectId>());
    }
  }
}

void collect_numeric_array_refs(const nlohmann::json &payload, std::vector<ObjectId> &refs)
{
  if (!payload.is_array()) {
    return;
  }
  for (const auto &child : payload) {
    if (child.is_number_unsigned() || child.is_number_integer()) {
      append_object_ref(refs, child.get<ObjectId>());
    }
  }
}

} // namespace

void append_payload_object_refs(const nlohmann::json &payload, std::vector<ObjectId> &object_refs)
{
  if (payload.is_object()) {
    for (const auto &[key, child] : payload.items()) {
      if (is_object_reference_key(key) && (child.is_number_unsigned() || child.is_number_integer())) {
        append_object_ref(object_refs, child.get<ObjectId>());
      }
      if (is_object_reference_array_key(key)) {
        collect_numeric_array_refs(child, object_refs);
      }
      if (key == "render_pass_info") {
        collect_render_pass_texture_refs(child, object_refs);
      }
      if (child.is_string()) {
        const auto nested = nlohmann::json::parse(child.get_ref<const std::string &>(), nullptr, false);
        if (!nested.is_discarded()) {
          if (key == "render_pass_info") {
            collect_render_pass_texture_refs(nested, object_refs);
          }
          append_payload_object_refs(nested, object_refs);
        }
      }
      append_payload_object_refs(child, object_refs);
    }
    return;
  }

  if (payload.is_array()) {
    for (const auto &child : payload) {
      append_payload_object_refs(child, object_refs);
    }
  }
}

void append_payload_text_object_refs(std::string_view payload_text, std::vector<ObjectId> &object_refs)
{
  const auto payload = nlohmann::json::parse(
      payload_text.empty() ? std::string("{}") : std::string(payload_text),
      nullptr,
      false);
  if (!payload.is_discarded()) {
    append_payload_object_refs(payload, object_refs);
  }
}

} // namespace apitrace::trace
