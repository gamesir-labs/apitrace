#include "apitrace/replay_session.hpp"

#include "apitrace/d3d12_replay.hpp"
#include "apitrace/metal_replay_backend_factory.hpp"

#include "d3d11/src/d3d11_replay_internal.hpp"
#include "retrace/src/d3d11_replay_parser.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace apitrace::replay {

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kMetalTextureSwizzleAlpha = 5;

const char *backend_name(BackendKind backend)
{
  switch (backend) {
  case BackendKind::NativeD3D11:
    return "NativeD3D11";
  case BackendKind::NativeD3D12:
    return "NativeD3D12";
  case BackendKind::TranslationLayer:
    return "TranslationLayer";
  case BackendKind::MetalTranslation:
    return "MetalTranslation";
  }
  return "UnknownBackend";
}

bool ends_with(std::string_view text, std::string_view suffix)
{
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool path_is_safe(const std::filesystem::path &path)
{
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  for (const auto &part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

bool is_api_specific_resource_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  if (part == path.end() || *part != "metal") {
    return false;
  }
  ++part;
  return part != path.end() && (*part == "buffers" || *part == "textures");
}

std::string read_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::uint64_t json_u64(const json &value, std::uint64_t fallback = 0)
{
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    return signed_value < 0 ? fallback : static_cast<std::uint64_t>(signed_value);
  }
  return fallback;
}

std::string string_from_json(const json &value)
{
  return value.is_string() ? value.get<std::string>() : std::string();
}

void collect_asset_paths(const json &value, std::vector<std::string> &paths)
{
  if (value.is_object()) {
    for (const auto &[key, child] : value.items()) {
      if ((ends_with(key, "_path") || key == "path") && child.is_string()) {
        const auto path = child.get<std::string>();
        if (!path.empty())
          paths.push_back(path);
      }
      collect_asset_paths(child, paths);
    }
    return;
  }

  if (value.is_array()) {
    for (const auto &child : value) {
      collect_asset_paths(child, paths);
    }
  }
}

bool is_metal_library_path(const std::string &path)
{
  return path.rfind("metal/libraries/", 0) == 0 && ends_with(path, ".metallib");
}

bool is_metal_pipeline_path(const std::string &path)
{
  return path.rfind("metal/pipelines/", 0) == 0 && ends_with(path, ".pipeline.json");
}

bool read_asset_json(
    const trace::TraceBundleReader &reader,
    const std::string &relative_path_text,
    json &asset_json,
    std::string &error)
{
  const std::filesystem::path relative_path = relative_path_text;
  if (!path_is_safe(relative_path)) {
    error = "unsafe asset path reference: " + relative_path_text;
    return false;
  }
  const auto content = read_file(reader.layout().root_path / relative_path);
  asset_json = json::parse(content, nullptr, false);
  if (asset_json.is_discarded()) {
    error = "referenced asset JSON is invalid: " + relative_path_text;
    return false;
  }
  return true;
}

std::unordered_set<std::string> checksum_paths(const trace::TraceBundleReader &reader)
{
  std::unordered_set<std::string> paths;
  for (const auto &record : reader.checksums().files) {
    paths.insert(record.relative_path.generic_string());
  }
  return paths;
}

std::unordered_map<std::uint64_t, std::string> blob_paths(const trace::TraceBundleReader &reader)
{
  std::unordered_map<std::uint64_t, std::string> paths;
  for (const auto &asset : reader.assets()) {
    if (asset.blob_id != 0 && !asset.relative_path.empty()) {
      paths[asset.blob_id] = asset.relative_path.generic_string();
    }
  }
  for (const auto &asset : reader.metal_assets()) {
    if (asset.blob_id != 0 && !asset.relative_path.empty()) {
      paths[asset.blob_id] = asset.relative_path.generic_string();
    }
  }
  return paths;
}

bool verify_asset_path(
    const trace::TraceBundleReader &reader,
    const std::unordered_set<std::string> &known_checksum_paths,
    const std::string &relative_path_text,
    std::string &error)
{
  const std::filesystem::path relative_path = relative_path_text;
  if (!path_is_safe(relative_path)) {
    error = "unsafe asset path reference: " + relative_path_text;
    return false;
  }
  if (is_api_specific_resource_path(relative_path)) {
    error = "API-specific buffer/texture asset path is not allowed: " + relative_path_text;
    return false;
  }
  const auto key = relative_path.generic_string();
  if (known_checksum_paths.find(key) == known_checksum_paths.end()) {
    error = "asset path missing from checksums: " + key;
    return false;
  }
  if (!std::filesystem::is_regular_file(reader.layout().root_path / relative_path)) {
    error = "asset path missing file: " + key;
    return false;
  }
  return true;
}

bool verify_blob_refs_cover_asset_paths(
    std::string_view label,
    const std::vector<trace::BlobId> &refs,
    const std::vector<std::string> &paths,
    const std::unordered_map<std::uint64_t, std::string> &known_blob_paths,
    std::string &error)
{
  if (refs.size() < paths.size()) {
    error = std::string(label) + ": asset path references are missing blob_refs";
    return false;
  }
  std::unordered_set<std::string> ref_paths;
  ref_paths.reserve(refs.size());
  for (const auto blob_id : refs) {
    if (blob_id == 0) {
      error = std::string(label) + ": asset path has zero blob_ref";
      return false;
    }
    const auto blob_path_it = known_blob_paths.find(blob_id);
    if (blob_path_it == known_blob_paths.end()) {
      error = std::string(label) + ": asset path blob_ref does not resolve";
      return false;
    }
    ref_paths.insert(blob_path_it->second);
  }
  for (const auto &path : paths) {
    if (ref_paths.find(path) == ref_paths.end()) {
      error = std::string(label) + ": asset path blob_ref does not match " + path;
      return false;
    }
  }
  return true;
}

bool verify_metal_render_pipeline_descriptor(
    const std::string &label,
    const json &descriptor,
    const std::unordered_set<std::uint64_t> &library_ids,
    std::string &error)
{
  if (!descriptor.is_object()) {
    error = label + ": render pipeline descriptor must be a JSON object";
    return false;
  }
  const auto vertex_function = string_from_json(descriptor.value("vertex_function", json(nullptr)));
  if (vertex_function.empty()) {
    error = label + ": render pipeline descriptor missing vertex_function";
    return false;
  }
  const auto vertex_library_id = json_u64(descriptor.value("vertex_library_id", descriptor.value("library_id", json(nullptr))));
  if (vertex_library_id == 0 || library_ids.find(vertex_library_id) == library_ids.end()) {
    error = label + ": render pipeline descriptor references an unknown vertex library";
    return false;
  }
  const auto fragment_function = string_from_json(descriptor.value("fragment_function", json(nullptr)));
  if (!fragment_function.empty()) {
    const auto fragment_library_id =
        json_u64(descriptor.value("fragment_library_id", descriptor.value("library_id", json(nullptr))));
    if (fragment_library_id == 0 || library_ids.find(fragment_library_id) == library_ids.end()) {
      error = label + ": render pipeline descriptor references an unknown fragment library";
      return false;
    }
  }
  const auto colors = descriptor.find("colors");
  if (colors == descriptor.end() || !colors->is_array() || colors->empty()) {
    error = label + ": render pipeline descriptor missing color attachment metadata";
    return false;
  }
  const auto require_integer_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_boolean_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_boolean()) {
      error = label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  std::size_t color_index = 0;
  for (const auto &color : *colors) {
    if (!color.is_object()) {
      error = label + ": render pipeline color attachment must be a JSON object";
      return false;
    }
    const auto description = "render pipeline color attachment " + std::to_string(color_index);
    const auto pixel_format = color.find("pixel_format");
    if (pixel_format == color.end() ||
        (!pixel_format->is_string() && !pixel_format->is_number_unsigned() && !pixel_format->is_number_integer())) {
      error = label + ": " + description + " is missing pixel_format";
      return false;
    }
    if (!require_boolean_field(color, "blending_enabled", description.c_str()) ||
        !require_integer_field(color, "write_mask", description.c_str()) ||
        !require_integer_field(color, "rgb_blend_operation", description.c_str()) ||
        !require_integer_field(color, "alpha_blend_operation", description.c_str()) ||
        !require_integer_field(color, "src_rgb_blend_factor", description.c_str()) ||
        !require_integer_field(color, "dst_rgb_blend_factor", description.c_str()) ||
        !require_integer_field(color, "src_alpha_blend_factor", description.c_str()) ||
        !require_integer_field(color, "dst_alpha_blend_factor", description.c_str())) {
      return false;
    }
    ++color_index;
  }
  if (!require_boolean_field(descriptor, "rasterization_enabled", "render pipeline descriptor") ||
      !require_integer_field(descriptor, "raster_sample_count", "render pipeline descriptor")) {
    return false;
  }
  const auto sample_count = json_u64(descriptor.value("raster_sample_count", json(1)), 1);
  if (sample_count == 0) {
    error = label + ": render pipeline descriptor has invalid raster_sample_count";
    return false;
  }
  return true;
}

bool verify_metal_compute_pipeline_descriptor(
    const std::string &label,
    const json &descriptor,
    const std::unordered_set<std::uint64_t> &library_ids,
    std::string &error)
{
  if (!descriptor.is_object()) {
    error = label + ": compute pipeline descriptor must be a JSON object";
    return false;
  }
  const auto function_name = string_from_json(descriptor.value("function", json(nullptr)));
  if (function_name.empty()) {
    error = label + ": compute pipeline descriptor missing function";
    return false;
  }
  const auto library_id = json_u64(descriptor.value("library_id", json(nullptr)));
  if (library_id == 0 || library_ids.find(library_id) == library_ids.end()) {
    error = label + ": compute pipeline descriptor references an unknown library";
    return false;
  }
  return true;
}

bool is_metal_draw_or_dispatch(trace::MetalCallKind kind)
{
  switch (kind) {
  case trace::MetalCallKind::DrawPrimitives:
  case trace::MetalCallKind::DrawIndexedPrimitives:
  case trace::MetalCallKind::DrawPrimitivesIndirect:
  case trace::MetalCallKind::DrawIndexedPrimitivesIndirect:
  case trace::MetalCallKind::DispatchThreadgroups:
  case trace::MetalCallKind::DispatchThreadgroupsIndirect:
  case trace::MetalCallKind::DispatchThreads:
  case trace::MetalCallKind::DispatchThreadsPerTile:
    return true;
  default:
    return false;
  }
}

json nested_json(const json &parent, const char *field_name)
{
  const auto it = parent.find(field_name);
  if (it == parent.end()) {
    return json::object();
  }
  if (it->is_object() || it->is_array()) {
    return *it;
  }
  if (it->is_string()) {
    auto parsed = json::parse(it->get_ref<const std::string &>(), nullptr, false);
    return parsed.is_discarded() ? json::object() : parsed;
  }
  return json::object();
}

std::uint64_t object_id_field(const json &payload, const char *key)
{
  const auto it = payload.find(key);
  return it == payload.end() ? 0 : json_u64(*it);
}

bool require_object_id_field(
    const json &payload,
    const char *key,
    const std::string &event_label,
    const char *description,
    std::uint64_t &object_id,
    std::string &error)
{
  const auto it = payload.find(key);
  if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
    error = event_label + ": " + description + " is missing " + key;
    return false;
  }
  object_id = json_u64(*it);
  return true;
}

bool require_metal_object(
    const std::unordered_set<std::uint64_t> &objects,
    std::uint64_t object_id,
    const std::string &event_label,
    const char *description,
    std::string &error)
{
  if (object_id == 0 || objects.find(object_id) == objects.end()) {
    error = event_label + ": " + description;
    return false;
  }
  return true;
}

bool require_metal_object_or_null(
    const std::unordered_set<std::uint64_t> &objects,
    std::uint64_t object_id,
    const std::string &event_label,
    const char *description,
    std::string &error)
{
  if (object_id == 0) {
    return true;
  }
  return require_metal_object(objects, object_id, event_label, description, error);
}

bool validate_metal_inline_bytes(
    const std::string &event_label,
    const json &payload,
    const char *description,
    std::string &error)
{
  const auto index = payload.find("index");
  if (index == payload.end() || (!index->is_number_unsigned() && !index->is_number_integer())) {
    error = event_label + ": " + description + " is missing index";
    return false;
  }
  auto nested = nested_json(payload, "payload");
  if (!nested.is_object() || nested.find("bytes") == nested.end()) {
    nested = payload;
  }
  const auto bytes = nested.find("bytes");
  if (bytes == nested.end() || !bytes->is_array() || bytes->empty()) {
    error = event_label + ": " + description + " is missing captured bytes";
    return false;
  }
  const auto length_it = nested.find("length");
  if (length_it == nested.end() || (!length_it->is_number_unsigned() && !length_it->is_number_integer())) {
    error = event_label + ": " + description + " is missing length";
    return false;
  }
  const auto length = json_u64(*length_it);
  if (length == 0 || length > bytes->size()) {
    error = event_label + ": " + description + " has invalid byte length";
    return false;
  }
  return true;
}

bool validate_metal_bind_payload(
    const std::string &event_label,
    const json &payload,
    const char *description,
    bool require_offset,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  return require_integer_field("index") &&
         (!require_offset || require_integer_field("offset"));
}

bool validate_metal_sampler_descriptor(
    const std::string &event_label,
    const json &payload,
    std::string &error)
{
  constexpr std::array<std::string_view, 16> required_fields = {
      "sampler_id",
      "gpu_resource_id",
      "border_color",
      "r_address_mode",
      "s_address_mode",
      "t_address_mode",
      "mag_filter",
      "min_filter",
      "mip_filter",
      "compare_function",
      "lod_max_clamp",
      "lod_min_clamp",
      "max_anisotropy",
      "lod_average",
      "normalized_coordinates",
      "support_argument_buffers",
  };
  for (const auto field : required_fields) {
    const auto it = payload.find(std::string(field));
    if (it == payload.end()) {
      error = event_label + ": sampler descriptor is missing " + std::string(field);
      return false;
    }
    if (field == "lod_max_clamp" || field == "lod_min_clamp") {
      if (!it->is_number()) {
        error = event_label + ": sampler descriptor field " + std::string(field) + " must be numeric";
        return false;
      }
    } else if (field == "lod_average" || field == "normalized_coordinates" || field == "support_argument_buffers") {
      if (!it->is_boolean()) {
        error = event_label + ": sampler descriptor field " + std::string(field) + " must be boolean";
        return false;
      }
    } else if (!it->is_number_unsigned() && !it->is_number_integer()) {
      error = event_label + ": sampler descriptor field " + std::string(field) + " must be an integer";
      return false;
    }
  }
  if (object_id_field(payload, "sampler_id") == 0) {
    error = event_label + ": sampler descriptor has zero sampler_id";
    return false;
  }
  return true;
}

bool validate_metal_texture_view_descriptor(
    const std::string &event_label,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": texture view descriptor is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_swizzle = [&]() -> bool {
    const auto it = payload.find("swizzle");
    if (it == payload.end() || !it->is_array()) {
      error = event_label + ": texture view descriptor is missing swizzle";
      return false;
    }
    if (it->size() != 4) {
      error = event_label + ": texture view descriptor has invalid swizzle";
      return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (!(*it)[index].is_number_unsigned() && !(*it)[index].is_number_integer()) {
        error = event_label + ": texture view descriptor has invalid swizzle";
        return false;
      }
      if ((*it)[index].is_number_integer() && (*it)[index].get<std::int64_t>() < 0) {
        error = event_label + ": texture view descriptor has invalid swizzle";
        return false;
      }
      if ((*it)[index].get<std::uint64_t>() > kMetalTextureSwizzleAlpha) {
        error = event_label + ": texture view descriptor has invalid swizzle";
        return false;
      }
    }
    return true;
  };

  return require_integer_field("texture_id") &&
         require_integer_field("source_texture_id") &&
         require_integer_field("gpu_resource_id") &&
         require_integer_field("pixel_format") &&
         require_integer_field("texture_type") &&
         require_integer_field("level_start") &&
         require_integer_field("level_count") &&
         require_integer_field("slice_start") &&
         require_integer_field("slice_count") &&
         require_swizzle();
}

bool validate_metal_depth_stencil_descriptor(
    const std::string &event_label,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_bool_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_boolean()) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto validate_stencil = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end()) {
      return true;
    }
    if (!it->is_object()) {
      error = event_label + ": depth stencil descriptor has invalid " + field;
      return false;
    }
    if (!require_bool_field(*it, "enabled", field)) {
      return false;
    }
    if (!it->value("enabled", false)) {
      return true;
    }
    return require_integer_field(*it, "depth_stencil_pass_op", field) &&
           require_integer_field(*it, "stencil_fail_op", field) &&
           require_integer_field(*it, "depth_fail_op", field) &&
           require_integer_field(*it, "stencil_compare_function", field) &&
           require_integer_field(*it, "write_mask", field) &&
           require_integer_field(*it, "read_mask", field);
  };

  return require_integer_field(payload, "depth_stencil_state_id", "depth stencil descriptor") &&
         require_integer_field(payload, "depth_compare_function", "depth stencil descriptor") &&
         require_bool_field(payload, "depth_write_enabled", "depth stencil descriptor") &&
         validate_stencil("front_stencil") &&
         validate_stencil("back_stencil");
}

bool validate_dxmt_resource_id_metadata(
    const std::string &event_label,
    const std::string &kind,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + kind + " is missing " + field;
      return false;
    }
    return true;
  };

  if (kind == "dxmt_buffer_gpu_address") {
    return require_integer_field("buffer_id") &&
           require_integer_field("gpu_address");
  }
  if (kind == "dxmt_texture_gpu_resource_id") {
    return require_integer_field("texture_id") &&
           require_integer_field("gpu_resource_id");
  }
  return true;
}

bool validate_metal_render_pass_resources(
    const std::string &event_label,
    const json &payload,
    std::unordered_set<std::uint64_t> &texture_ids,
    std::string &error)
{
  auto pass = nested_json(payload, "render_pass_info");
  if (pass.contains("render_pass_info")) {
    pass = nested_json(pass, "render_pass_info");
  }
  const auto require_integer_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_action_field =
      [&](const json &object, const char *field, const char *description, std::initializer_list<std::string_view> names) -> bool {
    const auto it = object.find(field);
    if (it == object.end()) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    if (it->is_number_unsigned() || it->is_number_integer()) {
      return true;
    }
    if (it->is_string()) {
      const auto &name = it->get_ref<const std::string &>();
      for (const auto expected : names) {
        if (name == expected) {
          return true;
        }
      }
    }
    error = event_label + ": " + description + " has invalid " + field;
    return false;
  };
  const auto require_clear_color = [&](const json &object, const char *description) -> bool {
    const auto it = object.find("clear_color");
    if (it == object.end() || !it->is_array()) {
      error = event_label + ": " + description + " is missing clear_color";
      return false;
    }
    if (it->size() != 4) {
      error = event_label + ": " + description + " has invalid clear_color";
      return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (!(*it)[index].is_number()) {
        error = event_label + ": " + description + " has invalid clear_color";
        return false;
      }
    }
    return true;
  };
  const auto validate_color_attachment = [&](const json &object, const char *description) -> bool {
    return require_action_field(object, "load_action", description, {"load", "clear", "dontcare"}) &&
           require_action_field(object, "store_action", description, {"store", "dontcare"}) &&
           require_clear_color(object, description) &&
           require_integer_field(object, "slot", description) &&
           require_integer_field(object, "level", description) &&
           require_integer_field(object, "slice", description) &&
           require_integer_field(object, "depth_plane", description);
  };
  const auto validate_resolve_attachment = [&](const json &object, const char *description) -> bool {
    return require_integer_field(object, "resolve_level", description) &&
           require_integer_field(object, "resolve_slice", description) &&
           require_integer_field(object, "resolve_depth_plane", description);
  };

  bool has_attachment_texture = false;
  const auto colors = pass.find("colors");
  if (colors != pass.end() && colors->is_array()) {
    for (const auto &color : *colors) {
      const auto texture_id = object_id_field(color, "texture");
      if (texture_id != 0) {
        if (texture_ids.find(texture_id) == texture_ids.end()) {
          error = event_label + ": render pass references an unknown color texture";
          return false;
        }
        has_attachment_texture = true;
      }
      if (texture_id != 0 && !validate_color_attachment(color, "render pass color attachment")) {
        return false;
      }
      const auto resolve_texture_id = object_id_field(color, "resolve_texture");
      if (resolve_texture_id != 0 &&
          texture_ids.find(resolve_texture_id) == texture_ids.end()) {
        error = event_label + ": render pass references an unknown resolve texture";
        return false;
      }
      if (resolve_texture_id != 0 &&
          !validate_resolve_attachment(color, "render pass resolve attachment")) {
        return false;
      }
    }
  } else {
    auto color_texture_id = object_id_field(pass, "color_texture_id");
    if (color_texture_id == 0) {
      color_texture_id = object_id_field(pass, "drawable_id");
    }
    if (color_texture_id != 0) {
      if (texture_ids.find(color_texture_id) == texture_ids.end()) {
        error = event_label + ": render pass references an unknown color texture";
        return false;
      }
      if (!validate_color_attachment(pass, "render pass color attachment")) {
        return false;
      }
      has_attachment_texture = true;
    }
  }

  const auto depth = pass.find("depth");
  if (depth != pass.end() && depth->is_object()) {
    const auto depth_texture_id = object_id_field(*depth, "texture");
    if (depth_texture_id != 0 && texture_ids.find(depth_texture_id) == texture_ids.end()) {
      error = event_label + ": render pass references an unknown depth texture";
      return false;
    }
    if (depth_texture_id != 0) {
      has_attachment_texture = true;
    }
    if (depth_texture_id != 0 &&
        (!require_action_field(*depth, "load_action", "render pass depth attachment", {"load", "clear", "dontcare"}) ||
         !require_action_field(*depth, "store_action", "render pass depth attachment", {"store", "dontcare"}) ||
         !require_integer_field(*depth, "level", "render pass depth attachment") ||
         !require_integer_field(*depth, "slice", "render pass depth attachment") ||
         !require_integer_field(*depth, "depth_plane", "render pass depth attachment"))) {
      return false;
    }
    const auto clear_depth = depth->find("clear_depth");
    if (depth_texture_id != 0 && (clear_depth == depth->end() || !clear_depth->is_number())) {
      error = event_label + ": render pass depth attachment is missing clear_depth";
      return false;
    }
  }
  const auto stencil = pass.find("stencil");
  if (stencil != pass.end() && stencil->is_object()) {
    const auto stencil_texture_id = object_id_field(*stencil, "texture");
    if (stencil_texture_id != 0 && texture_ids.find(stencil_texture_id) == texture_ids.end()) {
      error = event_label + ": render pass references an unknown stencil texture";
      return false;
    }
    if (stencil_texture_id != 0) {
      has_attachment_texture = true;
    }
  }
  if (!has_attachment_texture) {
    error = event_label + ": render pass is missing attachment texture metadata";
    return false;
  }
  return true;
}

bool validate_metal_blit_payload_resources(
    const std::string &event_label,
    trace::MetalCallKind kind,
    const json &payload,
    const std::unordered_set<std::uint64_t> &buffer_ids,
    const std::unordered_set<std::uint64_t> &texture_ids,
    std::string &error)
{
  const auto require_numeric_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_positive_numeric_field = [&](const json &object, const char *field, const char *description) -> bool {
    if (!require_numeric_field(object, field, description)) {
      return false;
    }
    if (json_u64(object[field]) == 0) {
      error = event_label + ": " + description + " has zero " + field;
      return false;
    }
    return true;
  };
  const auto require_size_array = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->size() != 3) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    for (const auto &component : *it) {
      if ((!component.is_number_unsigned() && !component.is_number_integer()) || json_u64(component) == 0) {
        error = event_label + ": " + description + " has invalid " + field;
        return false;
      }
    }
    return true;
  };
  const auto require_numeric_array = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->size() != 3) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    for (const auto &component : *it) {
      if (!component.is_number_unsigned() && !component.is_number_integer()) {
        error = event_label + ": " + description + " has invalid " + field;
        return false;
      }
    }
    return true;
  };

  switch (kind) {
  case trace::MetalCallKind::CopyBuffer:
    return require_positive_numeric_field(payload, "size", "copyFromBuffer") &&
           require_numeric_field(payload, "source_offset", "copyFromBuffer") &&
           require_numeric_field(payload, "destination_offset", "copyFromBuffer") &&
           require_metal_object(buffer_ids, object_id_field(payload, "source_buffer_id"), event_label,
               "copyFromBuffer references an unknown source buffer", error) &&
           require_metal_object(buffer_ids, object_id_field(payload, "destination_buffer_id"), event_label,
               "copyFromBuffer references an unknown destination buffer", error);
  case trace::MetalCallKind::CopyBufferToTexture:
    return require_numeric_field(payload, "source_offset", "copyFromBufferToTexture") &&
           require_positive_numeric_field(payload, "source_bytes_per_row", "copyFromBufferToTexture") &&
           require_positive_numeric_field(payload, "source_bytes_per_image", "copyFromBufferToTexture") &&
           require_size_array(payload, "source_size", "copyFromBufferToTexture") &&
           require_numeric_field(payload, "destination_slice", "copyFromBufferToTexture") &&
           require_numeric_field(payload, "destination_level", "copyFromBufferToTexture") &&
           require_numeric_array(payload, "destination_origin", "copyFromBufferToTexture") &&
           require_metal_object(buffer_ids, object_id_field(payload, "source_buffer"), event_label,
               "copyFromBufferToTexture references an unknown source buffer", error) &&
           require_metal_object(texture_ids, object_id_field(payload, "destination_texture"), event_label,
               "copyFromBufferToTexture references an unknown destination texture", error);
  case trace::MetalCallKind::CopyTexture:
    {
      const auto nested = nested_json(payload, "payload");
      return require_numeric_field(nested, "source_slice", "copyFromTexture") &&
             require_numeric_field(nested, "source_level", "copyFromTexture") &&
             require_numeric_array(nested, "source_origin", "copyFromTexture") &&
             require_size_array(nested, "source_size", "copyFromTexture") &&
             require_numeric_field(nested, "destination_slice", "copyFromTexture") &&
             require_numeric_field(nested, "destination_level", "copyFromTexture") &&
             require_numeric_array(nested, "destination_origin", "copyFromTexture") &&
             require_metal_object(texture_ids, object_id_field(payload, "source_texture_id"), event_label,
               "copyFromTexture references an unknown source texture", error) &&
             require_metal_object(texture_ids, object_id_field(payload, "destination_texture_id"), event_label,
               "copyFromTexture references an unknown destination texture", error);
    }
  case trace::MetalCallKind::BlitFill:
    return require_numeric_field(payload, "range_start", "fillBuffer") &&
           require_positive_numeric_field(payload, "range_length", "fillBuffer") &&
           require_numeric_field(payload, "value", "fillBuffer") &&
           require_metal_object(buffer_ids, object_id_field(payload, "buffer_id"), event_label,
        "fillBuffer references an unknown buffer", error);
  default:
    break;
  }
  return true;
}

bool validate_metal_texture_descriptor(
    const std::string &event_label,
    const json &payload,
    std::string &error)
{
  const auto descriptor = nested_json(payload, "descriptor");
  if (!descriptor.is_object() || descriptor.empty()) {
    error = event_label + ": newTexture is missing descriptor";
    return false;
  }
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = descriptor.find(field);
    if (it == descriptor.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": texture descriptor is missing " + field;
      return false;
    }
    return true;
  };
  const auto pixel_format = descriptor.find("pixel_format");
  if (pixel_format == descriptor.end() ||
      (!pixel_format->is_string() && !pixel_format->is_number_unsigned() && !pixel_format->is_number_integer())) {
    error = event_label + ": texture descriptor is missing pixel_format";
    return false;
  }
  if (pixel_format->is_string()) {
    const auto &format = pixel_format->get_ref<const std::string &>();
    if (format != "bgra8unorm" && format != "rgba8unorm" && format != "r32uint") {
      error = event_label + ": texture descriptor has invalid pixel_format";
      return false;
    }
  }

  return require_integer_field("type") &&
         require_integer_field("width") &&
         require_integer_field("height") &&
         require_integer_field("depth") &&
         require_integer_field("array_length") &&
         require_integer_field("usage") &&
         require_integer_field("mipmap_level_count") &&
         require_integer_field("sample_count");
}

bool validate_metal_resource_usage_payload(
    const std::string &event_label,
    const json &payload,
    bool render_encoder,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field, const char *description) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  return require_integer_field("usage", "useResource") &&
         (!render_encoder || require_integer_field("stages", "useResource"));
}

bool validate_metal_present_payload(
    const std::string &event_label,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": presentDrawable is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_positive_integer_field = [&](const char *field) -> bool {
    if (!require_integer_field(field)) {
      return false;
    }
    if (json_u64(payload[field]) == 0) {
      error = event_label + ": presentDrawable has zero " + std::string(field);
      return false;
    }
    return true;
  };
  return require_integer_field("frame_index") &&
         require_positive_integer_field("width") &&
         require_positive_integer_field("height") &&
         require_integer_field("sync_interval") &&
         require_integer_field("flags");
}

bool validate_metal_synchronization_payload(
    const std::string &event_label,
    trace::MetalCallKind kind,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };

  if (kind == trace::MetalCallKind::MemoryBarrier) {
    const auto payload_it = payload.find("payload");
    const auto barrier = payload_it != payload.end() && payload_it->is_object()
                             ? *payload_it
                             : payload;
    return require_integer_field(barrier, "scope", "memoryBarrier") &&
           require_integer_field(barrier, "stages_before", "memoryBarrier") &&
           require_integer_field(barrier, "stages_after", "memoryBarrier");
  }
  if (kind == trace::MetalCallKind::UpdateFence || kind == trace::MetalCallKind::WaitForFence) {
    const char *description = kind == trace::MetalCallKind::UpdateFence ? "updateFence" : "waitForFence";
    return require_integer_field(payload, "fence_id", description) &&
           require_integer_field(payload, "stages", description);
  }
  if (kind == trace::MetalCallKind::FenceOps) {
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array() || ops->empty()) {
      error = event_label + ": fenceOps is missing ops";
      return false;
    }
    for (const auto &op : *ops) {
      if (!op.is_array() || op.size() < 3 || !op[0].is_string() ||
          (!op[1].is_number_unsigned() && !op[1].is_number_integer()) ||
          (!op[2].is_number_unsigned() && !op[2].is_number_integer())) {
        error = event_label + ": fenceOps has invalid op";
        return false;
      }
      const auto op_name = op[0].get_ref<const std::string &>();
      if (op_name != "update" && op_name != "wait") {
        error = event_label + ": fenceOps has unsupported op " + op_name;
        return false;
      }
    }
  }
  return true;
}

bool validate_metal_blit_batch_payload(
    const std::string &event_label,
    const json &payload,
    const std::unordered_set<std::uint64_t> &buffer_ids,
    const std::unordered_set<std::uint64_t> &texture_ids,
    std::string &error)
{
  const auto ops = payload.find("ops");
  const auto fence_ops = payload.find("fence_ops");
  if ((ops == payload.end() || !ops->is_array() || ops->empty()) &&
      (fence_ops == payload.end() || !fence_ops->is_object())) {
    error = event_label + ": blitBatch is missing ops";
    return false;
  }

  if (ops != payload.end()) for (const auto &op : *ops) {
    if (!op.is_object()) {
      error = event_label + ": blitBatch op must be an object";
      return false;
    }
    const auto kind = string_from_json(op.value("op", json(nullptr)));
    if (kind == "copy_texture") {
      if (!validate_metal_blit_payload_resources(
              event_label, trace::MetalCallKind::CopyTexture, op, buffer_ids, texture_ids, error)) {
        return false;
      }
    } else if (kind == "fill_buffer") {
      if (!validate_metal_blit_payload_resources(
              event_label, trace::MetalCallKind::BlitFill, op, buffer_ids, texture_ids, error)) {
        return false;
      }
    } else if (kind == "wait_fence" || kind == "update_fence") {
      if (object_id_field(op, "fence_id") == 0) {
        error = event_label + ": " + kind + " blitBatch op is missing fence_id";
        return false;
      }
      const auto stages = op.find("stages");
      if (stages == op.end() || (!stages->is_number_unsigned() && !stages->is_number_integer())) {
        error = event_label + ": " + kind + " blitBatch op is missing stages";
        return false;
      }
    } else {
      error = event_label + ": unsupported blitBatch op " + (kind.empty() ? std::string("<missing>") : kind);
      return false;
    }
  }
  if (fence_ops != payload.end()) {
    if (string_from_json(fence_ops->value("schema", json(nullptr))) == "blit-fence-v2") {
      for (const auto *key : {"wait_fences", "update_fences"}) {
        const auto fences = fence_ops->find(key);
        if (fences == fence_ops->end() || !fences->is_array()) {
          error = event_label + ": compact blitBatch fence list is missing";
          return false;
        }
        for (const auto &fence : *fences) {
          if (json_u64(fence) == 0) {
            error = event_label + ": compact blitBatch fence op is missing fence_id";
            return false;
          }
        }
      }
    } else {
      const auto compact_ops = fence_ops->find("ops");
      if (compact_ops == fence_ops->end() || !compact_ops->is_array() || compact_ops->empty()) {
        error = event_label + ": compact blitBatch fence ops are missing ops";
        return false;
      }
      for (const auto &op : *compact_ops) {
        if (!op.is_array() || op.size() < 3) {
          error = event_label + ": compact blitBatch fence op must be a 3-column array";
          return false;
        }
        const auto kind = json_u64(op[0]);
        if (kind != 1 && kind != 2) {
          error = event_label + ": unsupported compact blitBatch fence op";
          return false;
        }
        if (json_u64(op[1]) == 0) {
          error = event_label + ": compact blitBatch fence op is missing fence_id";
          return false;
        }
      }
    }
  }
  return true;
}

bool validate_metal_work_payload(
    const std::string &event_label,
    trace::MetalCallKind kind,
    const json &payload,
    std::string &error)
{
  const auto require_numeric_field = [&](const char *field, const char *description) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_primitive_type = [&](const char *description) -> bool {
    if (!require_numeric_field("primitive_type", description)) {
      return false;
    }
    const auto primitive_type = json_u64(payload["primitive_type"]);
    if (primitive_type > 4) {
      error = event_label + ": " + description + " has invalid primitive_type";
      return false;
    }
    return true;
  };
  const auto require_index_type = [&](const char *description) -> bool {
    if (!require_numeric_field("index_type", description)) {
      return false;
    }
    const auto index_type = json_u64(payload["index_type"]);
    if (index_type > 1) {
      error = event_label + ": " + description + " has invalid index_type";
      return false;
    }
    return true;
  };

  switch (kind) {
  case trace::MetalCallKind::DrawPrimitives:
    return require_primitive_type("drawPrimitives") &&
           require_numeric_field("vertex_start", "drawPrimitives") &&
           require_numeric_field("vertex_count", "drawPrimitives") &&
           require_numeric_field("instance_count", "drawPrimitives") &&
           require_numeric_field("base_instance", "drawPrimitives");
  case trace::MetalCallKind::DrawIndexedPrimitives:
    return require_primitive_type("drawIndexedPrimitives") &&
           require_numeric_field("index_count", "drawIndexedPrimitives") &&
           require_index_type("drawIndexedPrimitives") &&
           require_numeric_field("index_buffer_offset", "drawIndexedPrimitives") &&
           require_numeric_field("instance_count", "drawIndexedPrimitives") &&
           require_numeric_field("base_vertex", "drawIndexedPrimitives") &&
           require_numeric_field("base_instance", "drawIndexedPrimitives");
  case trace::MetalCallKind::DrawPrimitivesIndirect:
    return require_primitive_type("drawPrimitivesIndirect") &&
           require_numeric_field("indirect_buffer_offset", "drawPrimitivesIndirect");
  case trace::MetalCallKind::DrawIndexedPrimitivesIndirect:
    return require_primitive_type("drawIndexedPrimitivesIndirect") &&
           require_index_type("drawIndexedPrimitivesIndirect") &&
           require_numeric_field("index_buffer_offset", "drawIndexedPrimitivesIndirect") &&
           require_numeric_field("indirect_buffer_offset", "drawIndexedPrimitivesIndirect");
  case trace::MetalCallKind::DispatchThreadgroups:
    return require_numeric_field("tgx", "dispatchThreadgroups") &&
           require_numeric_field("tgy", "dispatchThreadgroups") &&
           require_numeric_field("tgz", "dispatchThreadgroups") &&
           require_numeric_field("tx", "dispatchThreadgroups") &&
           require_numeric_field("ty", "dispatchThreadgroups") &&
           require_numeric_field("tz", "dispatchThreadgroups");
  case trace::MetalCallKind::DispatchThreadgroupsIndirect:
    return require_numeric_field("indirect_buffer_offset", "dispatchThreadgroupsIndirect") &&
           require_numeric_field("tx", "dispatchThreadgroupsIndirect") &&
           require_numeric_field("ty", "dispatchThreadgroupsIndirect") &&
           require_numeric_field("tz", "dispatchThreadgroupsIndirect");
  case trace::MetalCallKind::DispatchThreads:
    return require_numeric_field("width", "dispatchThreads") &&
           require_numeric_field("height", "dispatchThreads") &&
           require_numeric_field("depth", "dispatchThreads") &&
           require_numeric_field("threads_per_group_width", "dispatchThreads") &&
           require_numeric_field("threads_per_group_height", "dispatchThreads") &&
           require_numeric_field("threads_per_group_depth", "dispatchThreads");
  case trace::MetalCallKind::DispatchThreadsPerTile:
    return require_numeric_field("width", "dispatchThreadsPerTile") &&
           require_numeric_field("height", "dispatchThreadsPerTile");
  default:
    return true;
  }
}

bool validate_metal_render_state_payload(
    const std::string &event_label,
    trace::MetalCallKind kind,
    const json &payload,
    std::string &error)
{
  const auto require_numeric_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_number()) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_numeric_array = [&](const json &object, const char *field, const char *description, std::size_t width) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->empty()) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    for (const auto &entry : *it) {
      if (!entry.is_array() || entry.size() != width) {
        error = event_label + ": " + description + " has invalid " + field;
        return false;
      }
      for (std::size_t index = 0; index < width; ++index) {
        if (!entry[index].is_number()) {
          error = event_label + ": " + description + " has invalid " + field;
          return false;
        }
      }
    }
    return true;
  };

  if (kind == trace::MetalCallKind::SetCullMode) {
    return require_numeric_field(payload, "cull_mode", "setCullMode");
  }
  if (kind == trace::MetalCallKind::SetFrontFacingWinding) {
    return require_numeric_field(payload, "winding", "setFrontFacingWinding");
  }
  if (kind == trace::MetalCallKind::SetTriangleFillMode) {
    return require_numeric_field(payload, "fill_mode", "setTriangleFillMode");
  }
  if (kind == trace::MetalCallKind::SetViewport) {
    return require_numeric_array(nested_json(payload, "payload"), "viewports", "setViewport", 6);
  }
  if (kind == trace::MetalCallKind::SetScissorRect) {
    return require_numeric_array(nested_json(payload, "payload"), "rects", "setScissorRect", 4);
  }
  return true;
}

bool validate_dxmt_encoder_state_payload(
    const std::string &event_label,
    const std::string &kind,
    const json &payload,
    std::string &error)
{
  const auto require_numeric_field = [&](const char *field, const std::string &description) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || !it->is_number()) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_numeric_array = [&](const char *field, const std::string &description, std::size_t width) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || !it->is_array() || it->empty()) {
      error = event_label + ": " + description + " is missing " + field;
      return false;
    }
    for (const auto &entry : *it) {
      if (!entry.is_array() || entry.size() != width) {
        error = event_label + ": " + description + " has invalid " + field;
        return false;
      }
      for (std::size_t index = 0; index < width; ++index) {
        if (!entry[index].is_number()) {
          error = event_label + ": " + description + " has invalid " + field;
          return false;
        }
      }
    }
    return true;
  };

  if (kind == "dxmt_set_rasterizer_state") {
    return require_numeric_field("fill_mode", kind) &&
           require_numeric_field("cull_mode", kind) &&
           require_numeric_field("depth_clip_mode", kind) &&
           require_numeric_field("depth_bias", kind) &&
           require_numeric_field("slope_scale", kind) &&
           require_numeric_field("depth_bias_clamp", kind) &&
           require_numeric_field("winding", kind);
  }
  if (kind == "dxmt_set_blend_factor") {
    return require_numeric_field("red", kind) &&
           require_numeric_field("green", kind) &&
           require_numeric_field("blue", kind) &&
           require_numeric_field("alpha", kind) &&
           require_numeric_field("stencil_ref", kind);
  }
  if (kind == "dxmt_set_viewports") {
    return require_numeric_array("viewports", kind, 6);
  }
  if (kind == "dxmt_set_scissor_rects") {
    return require_numeric_array("rects", kind, 4);
  }
  if (kind == "dxmt_set_depth_stencil_state") {
    return require_numeric_field("depth_stencil_state_id", kind) &&
           require_numeric_field("stencil_ref", kind);
  }
  return true;
}

bool validate_metal_replay_closure(
    const trace::TraceBundleReader &reader,
    ReplayStatistics &statistics,
    std::string &error)
{
  bool has_library_asset = false;
  bool has_pipeline_asset = false;
  bool has_pipeline_bind = false;
  bool has_draw_or_dispatch = false;
  std::unordered_set<std::uint64_t> render_pipeline_ids;
  std::unordered_set<std::uint64_t> compute_pipeline_ids;
  std::unordered_set<std::uint64_t> library_ids;
  std::unordered_set<std::uint64_t> buffer_ids;
  std::unordered_set<std::uint64_t> texture_ids;
  std::unordered_set<std::uint64_t> sampler_ids;
  std::unordered_set<std::uint64_t> depth_stencil_state_ids;
  std::unordered_set<std::uint64_t> command_buffer_ids;
  std::unordered_set<std::uint64_t> render_encoder_ids;
  std::unordered_set<std::uint64_t> compute_encoder_ids;
  std::unordered_set<std::uint64_t> blit_encoder_ids;
  std::unordered_map<std::uint64_t, bool> render_encoder_has_pipeline;
  std::unordered_map<std::uint64_t, bool> render_encoder_has_viewport;
  std::unordered_map<std::uint64_t, bool> render_encoder_has_scissor;
  std::unordered_map<std::uint64_t, bool> compute_encoder_has_pipeline;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> render_vertex_buffer_bindings;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> render_fragment_buffer_bindings;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> compute_buffer_bindings;
  const auto known_checksum_paths = checksum_paths(reader);
  const auto known_blob_paths = blob_paths(reader);

  for (const auto &event : reader.metal_events()) {
    const auto event_label = "metal sequence " + std::to_string(event.metal_sequence);
    json payload = json::parse(event.payload.empty() ? std::string("{}") : event.payload, nullptr, false);
    if (payload.is_discarded()) {
      error = event_label + ": invalid payload JSON";
      return false;
    }

    std::vector<std::string> paths;
    collect_asset_paths(payload, paths);
    for (const auto &path : paths) {
      has_library_asset = has_library_asset || is_metal_library_path(path);
      has_pipeline_asset = has_pipeline_asset || is_metal_pipeline_path(path);
      if (!verify_asset_path(reader, known_checksum_paths, path, error)) {
        error = event_label + ": " + error;
        return false;
      }
    }
    if (!verify_blob_refs_cover_asset_paths(event_label, event.blob_refs, paths, known_blob_paths, error)) {
      return false;
    }
    for (const auto blob_id : event.blob_refs) {
      if (blob_id == 0) {
        continue;
      }
      if (known_blob_paths.find(blob_id) == known_blob_paths.end()) {
        error = event_label + ": blob_refs contains unknown blob id " + std::to_string(blob_id);
        return false;
      }
    }
    if (event.call_kind == trace::MetalCallKind::Unknown) {
      error = event_label + ": unknown Metal call kind is not replayable";
      return false;
    }

    if (event.call_kind == trace::MetalCallKind::DeviceCreate) {
      if (event.function_name == "MTLDevice.newBuffer") {
        const auto length_it = payload.find("length");
        if (length_it == payload.end() ||
            (!length_it->is_number_unsigned() && !length_it->is_number_integer())) {
          error = event_label + ": MTLDevice.newBuffer is missing length";
          return false;
        }
        if (json_u64(*length_it) == 0) {
          error = event_label + ": MTLDevice.newBuffer has invalid length";
          return false;
        }
        buffer_ids.insert(event.object_id);
      } else if (event.function_name == "MTLDevice.newTexture" ||
                 event.function_name == "CAMetalDrawable.texture") {
        if (!validate_metal_texture_descriptor(event_label, payload, error)) {
          return false;
        }
        texture_ids.insert(event.object_id);
      } else if (event.function_name.find("newLibrary") != std::string::npos) {
        const auto library_path = string_from_json(payload.value("library_path", json(nullptr)));
        if (library_path.empty()) {
          error = event_label + ": newLibrary is missing library_path";
          return false;
        }
        if (!is_metal_library_path(library_path)) {
          error = event_label + ": newLibrary does not reference a Metal library asset";
          return false;
        }
        library_ids.insert(event.object_id);
      } else if (payload.contains("descriptor_path")) {
        const auto descriptor_path = string_from_json(payload.value("descriptor_path", json(nullptr)));
        if (descriptor_path.empty()) {
          error = event_label + ": pipeline creation is missing descriptor_path";
          return false;
        }
        if (!is_metal_pipeline_path(descriptor_path)) {
          error = event_label + ": descriptor_path does not reference a Metal pipeline asset";
          return false;
        }
        json descriptor;
        if (!read_asset_json(reader, descriptor_path, descriptor, error)) {
          error = event_label + ": " + error;
          return false;
        }
	        if (event.function_name.find("newTileRenderPipelineState") != std::string::npos) {
	          render_pipeline_ids.insert(event.object_id);
	        } else if (event.function_name.find("newRenderPipelineState") != std::string::npos) {
	          if (!verify_metal_render_pipeline_descriptor(event_label, descriptor, library_ids, error)) {
	            return false;
	          }
          render_pipeline_ids.insert(event.object_id);
        } else if (event.function_name.find("newComputePipelineState") != std::string::npos) {
          if (!verify_metal_compute_pipeline_descriptor(event_label, descriptor, library_ids, error)) {
            return false;
          }
          compute_pipeline_ids.insert(event.object_id);
        }
      }
    } else if (event.call_kind == trace::MetalCallKind::CommandBufferBegin) {
      command_buffer_ids.insert(event.object_id);
    } else if (event.call_kind == trace::MetalCallKind::RenderEncoderBegin) {
      const auto command_buffer_id = object_id_field(payload, "command_buffer_id");
      if (!require_metal_object(command_buffer_ids, command_buffer_id, event_label,
              "render encoder references an unknown command buffer", error)) {
        return false;
      }
      if (!validate_metal_render_pass_resources(event_label, payload, texture_ids, error)) {
        return false;
      }
      render_encoder_ids.insert(event.object_id);
      render_encoder_has_pipeline[event.object_id] = false;
      render_encoder_has_viewport[event.object_id] = false;
      render_encoder_has_scissor[event.object_id] = false;
    } else if (event.call_kind == trace::MetalCallKind::ComputeEncoderBegin) {
      const auto command_buffer_id = object_id_field(payload, "command_buffer_id");
      if (!require_metal_object(command_buffer_ids, command_buffer_id, event_label,
              "compute encoder references an unknown command buffer", error)) {
        return false;
      }
      compute_encoder_ids.insert(event.object_id);
      compute_encoder_has_pipeline[event.object_id] = false;
    } else if (event.call_kind == trace::MetalCallKind::BlitEncoderBegin ||
               event.call_kind == trace::MetalCallKind::BlitEncoderBatch) {
      const auto command_buffer_id = object_id_field(payload, "command_buffer_id");
      if (!require_metal_object(command_buffer_ids, command_buffer_id, event_label,
              "blit encoder references an unknown command buffer", error)) {
        return false;
      }
      if (event.call_kind == trace::MetalCallKind::BlitEncoderBatch) {
        if (!validate_metal_blit_batch_payload(event_label, payload, buffer_ids, texture_ids, error)) {
          return false;
        }
      } else {
        blit_encoder_ids.insert(event.object_id);
      }
    } else if (event.call_kind == trace::MetalCallKind::RenderEncoderEnd) {
      if (!require_metal_object(render_encoder_ids, event.object_id, event_label,
              "render encoder end references an unknown render encoder", error)) {
        return false;
      }
      render_encoder_ids.erase(event.object_id);
      render_encoder_has_pipeline.erase(event.object_id);
      render_encoder_has_viewport.erase(event.object_id);
      render_encoder_has_scissor.erase(event.object_id);
      render_vertex_buffer_bindings.erase(event.object_id);
      render_fragment_buffer_bindings.erase(event.object_id);
    } else if (event.call_kind == trace::MetalCallKind::ComputeEncoderEnd) {
      if (!require_metal_object(compute_encoder_ids, event.object_id, event_label,
              "compute encoder end references an unknown compute encoder", error)) {
        return false;
      }
      compute_encoder_ids.erase(event.object_id);
      compute_encoder_has_pipeline.erase(event.object_id);
      compute_buffer_bindings.erase(event.object_id);
    } else if (event.call_kind == trace::MetalCallKind::BlitEncoderEnd) {
      if (!require_metal_object(blit_encoder_ids, event.object_id, event_label,
              "blit encoder end references an unknown blit encoder", error)) {
        return false;
      }
      blit_encoder_ids.erase(event.object_id);
    } else if (event.call_kind == trace::MetalCallKind::CommandBufferCommit) {
      if (!require_metal_object(command_buffer_ids, event.object_id, event_label,
              "command buffer commit references an unknown command buffer", error)) {
        return false;
      }
      command_buffer_ids.erase(event.object_id);
    }

    if (event.call_kind == trace::MetalCallKind::SetRenderPipelineState ||
        event.call_kind == trace::MetalCallKind::SetComputePipelineState) {
      const auto pipeline_id = payload.value("pipeline_state_id", 0ull);
      if (event.call_kind == trace::MetalCallKind::SetRenderPipelineState) {
        if (render_pipeline_ids.find(pipeline_id) == render_pipeline_ids.end()) {
          error = event_label + ": setRenderPipelineState references an unknown render pipeline";
          return false;
        }
        const auto encoder = render_encoder_has_pipeline.find(event.object_id);
        if (encoder == render_encoder_has_pipeline.end()) {
          error = event_label + ": setRenderPipelineState references an unknown render encoder";
          return false;
        }
        encoder->second = true;
      } else {
        if (compute_pipeline_ids.find(pipeline_id) == compute_pipeline_ids.end()) {
          error = event_label + ": setComputePipelineState references an unknown compute pipeline";
          return false;
        }
        const auto encoder = compute_encoder_has_pipeline.find(event.object_id);
        if (encoder == compute_encoder_has_pipeline.end()) {
          error = event_label + ": setComputePipelineState references an unknown compute encoder";
          return false;
        }
        encoder->second = true;
      }
      has_pipeline_bind = true;
    }
    if (event.call_kind == trace::MetalCallKind::SetVertexBuffer ||
        event.call_kind == trace::MetalCallKind::SetFragmentBuffer) {
      const auto encoder = render_encoder_has_pipeline.find(event.object_id);
      if (encoder == render_encoder_has_pipeline.end()) {
        error = event_label + ": buffer bind references an unknown render encoder";
        return false;
      }
      std::uint64_t buffer_id = 0;
      if (!require_object_id_field(payload, "buffer_id", event_label, "buffer bind", buffer_id, error) ||
          !require_metal_object_or_null(buffer_ids, buffer_id, event_label,
              "buffer bind references an unknown buffer", error)) {
        return false;
      }
      if (!validate_metal_bind_payload(event_label, payload, "buffer bind", true, error)) {
        return false;
      }
      auto &bindings = event.call_kind == trace::MetalCallKind::SetVertexBuffer
                           ? render_vertex_buffer_bindings[event.object_id]
                           : render_fragment_buffer_bindings[event.object_id];
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      if (buffer_id == 0) {
        bindings.erase(index);
      } else {
        bindings.insert(index);
      }
    } else if (event.call_kind == trace::MetalCallKind::SetComputeBuffer) {
      if (compute_encoder_has_pipeline.find(event.object_id) == compute_encoder_has_pipeline.end()) {
        error = event_label + ": compute buffer bind references an unknown compute encoder";
        return false;
      }
      std::uint64_t buffer_id = 0;
      if (!require_object_id_field(payload, "buffer_id", event_label, "compute buffer bind", buffer_id, error) ||
          !require_metal_object_or_null(buffer_ids, buffer_id, event_label,
              "compute buffer bind references an unknown buffer", error)) {
        return false;
      }
      if (!validate_metal_bind_payload(event_label, payload, "compute buffer bind", true, error)) {
        return false;
      }
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      if (buffer_id == 0) {
        compute_buffer_bindings[event.object_id].erase(index);
      } else {
        compute_buffer_bindings[event.object_id].insert(index);
      }
    } else if (event.call_kind == trace::MetalCallKind::SetVertexBytes ||
               event.call_kind == trace::MetalCallKind::SetFragmentBytes) {
      if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
        error = event_label + ": inline bytes bind references an unknown render encoder";
        return false;
      }
      const auto description = event.call_kind == trace::MetalCallKind::SetVertexBytes
                                   ? "setVertexBytes"
                                   : "setFragmentBytes";
      if (!validate_metal_inline_bytes(event_label, payload, description, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetVertexBufferOffset ||
               event.call_kind == trace::MetalCallKind::SetFragmentBufferOffset) {
      if (!validate_metal_bind_payload(event_label, payload, "buffer offset update", true, error)) {
        return false;
      }
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      if (event.call_kind == trace::MetalCallKind::SetVertexBufferOffset) {
        const auto bindings_it = render_vertex_buffer_bindings.find(event.object_id);
        if (bindings_it == render_vertex_buffer_bindings.end() ||
            bindings_it->second.find(index) == bindings_it->second.end()) {
          error = event_label + ": buffer offset update occurs before a matching vertex buffer bind";
          return false;
        }
      } else {
        const auto bindings_it = render_fragment_buffer_bindings.find(event.object_id);
        if (bindings_it == render_fragment_buffer_bindings.end() ||
            bindings_it->second.find(index) == bindings_it->second.end()) {
          error = event_label + ": buffer offset update occurs before a matching fragment buffer bind";
          return false;
        }
      }
    } else if (event.call_kind == trace::MetalCallKind::SetComputeBufferOffset) {
      if (!validate_metal_bind_payload(event_label, payload, "compute buffer offset update", true, error)) {
        return false;
      }
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      const auto bindings_it = compute_buffer_bindings.find(event.object_id);
      if (bindings_it == compute_buffer_bindings.end() || bindings_it->second.find(index) == bindings_it->second.end()) {
        error = event_label + ": compute buffer offset update occurs before a matching buffer bind";
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetComputeBytes) {
      if (compute_encoder_has_pipeline.find(event.object_id) == compute_encoder_has_pipeline.end()) {
        error = event_label + ": setComputeBytes references an unknown compute encoder";
        return false;
      }
      if (!validate_metal_inline_bytes(event_label, payload, "setComputeBytes", error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetArgumentBuffer) {
      error = event_label + ": SetArgumentBuffer requires native Metal argument-buffer replay support";
      return false;
    } else if (event.call_kind == trace::MetalCallKind::UseHeap) {
      error = event_label + ": UseHeap requires native Metal heap replay support";
      return false;
    } else if (event.call_kind == trace::MetalCallKind::UseResource) {
      const bool is_render_encoder = render_encoder_has_pipeline.find(event.object_id) != render_encoder_has_pipeline.end();
      const bool is_compute_encoder = compute_encoder_has_pipeline.find(event.object_id) != compute_encoder_has_pipeline.end();
      if (!is_render_encoder && !is_compute_encoder) {
        error = event_label + ": useResource references an unknown encoder";
        return false;
      }
      const auto resource_id = object_id_field(payload, "resource_id");
      if (buffer_ids.find(resource_id) == buffer_ids.end() &&
          texture_ids.find(resource_id) == texture_ids.end()) {
        error = event_label + ": useResource references an unknown replayable resource";
        return false;
      }
      if (!validate_metal_resource_usage_payload(event_label, payload, is_render_encoder, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::UseResources) {
      const bool is_render_encoder = render_encoder_has_pipeline.find(event.object_id) != render_encoder_has_pipeline.end();
      const bool is_compute_encoder = compute_encoder_has_pipeline.find(event.object_id) != compute_encoder_has_pipeline.end();
      if (!is_render_encoder && !is_compute_encoder) {
        error = event_label + ": useResources references an unknown encoder";
        return false;
      }
      const auto resources = payload.find("resource_ids");
      if (resources == payload.end() || !resources->is_array()) {
        error = event_label + ": useResources is missing resource_ids";
        return false;
      }
      if (!validate_metal_resource_usage_payload(event_label, payload, is_render_encoder, error)) {
        return false;
      }
      for (const auto &resource : *resources) {
        const auto resource_id = json_u64(resource);
        if (buffer_ids.find(resource_id) == buffer_ids.end() &&
            texture_ids.find(resource_id) == texture_ids.end()) {
          error = event_label + ": useResources references an unknown replayable resource";
          return false;
        }
      }
    } else if (event.call_kind == trace::MetalCallKind::MemoryBarrier ||
               event.call_kind == trace::MetalCallKind::FenceOps ||
               event.call_kind == trace::MetalCallKind::UpdateFence ||
               event.call_kind == trace::MetalCallKind::WaitForFence) {
      if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end() &&
          compute_encoder_has_pipeline.find(event.object_id) == compute_encoder_has_pipeline.end() &&
          blit_encoder_ids.find(event.object_id) == blit_encoder_ids.end()) {
        error = event_label + ": synchronization command references an unknown encoder";
        return false;
      }
      if (!validate_metal_synchronization_payload(event_label, event.call_kind, payload, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetCullMode ||
               event.call_kind == trace::MetalCallKind::SetFrontFacingWinding ||
               event.call_kind == trace::MetalCallKind::SetTriangleFillMode ||
               event.call_kind == trace::MetalCallKind::SetViewport ||
               event.call_kind == trace::MetalCallKind::SetScissorRect) {
      if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
        error = event_label + ": render state command references an unknown render encoder";
        return false;
      }
      if (!validate_metal_render_state_payload(event_label, event.call_kind, payload, error)) {
        return false;
      }
      if (event.call_kind == trace::MetalCallKind::SetViewport) {
        render_encoder_has_viewport[event.object_id] = true;
      } else if (event.call_kind == trace::MetalCallKind::SetScissorRect) {
        render_encoder_has_scissor[event.object_id] = true;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetDepthStencilState) {
      if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
        error = event_label + ": depth stencil state command references an unknown render encoder";
        return false;
      }
      const auto depth_stencil_field = payload.find("depth_stencil_state_id");
      if (depth_stencil_field == payload.end() ||
          (!depth_stencil_field->is_number_unsigned() && !depth_stencil_field->is_number_integer())) {
        error = event_label + ": setDepthStencilState is missing depth_stencil_state_id";
        return false;
      }
      const auto depth_stencil_state_id = object_id_field(payload, "depth_stencil_state_id");
      if (depth_stencil_state_id != 0 &&
          depth_stencil_state_ids.find(depth_stencil_state_id) == depth_stencil_state_ids.end()) {
        error = event_label + ": depth stencil state command references an unknown depth stencil state";
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::EncoderState) {
      const auto kind = string_from_json(payload.value("kind", json(nullptr)));
      if (kind == "dxmt_set_rasterizer_state" ||
          kind == "dxmt_set_blend_factor" ||
          kind == "dxmt_set_viewports" ||
          kind == "dxmt_set_scissor_rects" ||
          kind == "dxmt_set_depth_stencil_state") {
        if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
          error = event_label + ": encoder state command references an unknown render encoder";
          return false;
        }
        if (!validate_dxmt_encoder_state_payload(event_label, kind, payload, error)) {
          return false;
        }
        if (kind == "dxmt_set_viewports") {
          render_encoder_has_viewport[event.object_id] = true;
        } else if (kind == "dxmt_set_scissor_rects") {
          render_encoder_has_scissor[event.object_id] = true;
        }
        if (kind == "dxmt_set_depth_stencil_state") {
          const auto depth_stencil_state_id = object_id_field(payload, "depth_stencil_state_id");
          if (depth_stencil_state_id != 0 &&
              depth_stencil_state_ids.find(depth_stencil_state_id) == depth_stencil_state_ids.end()) {
            error = event_label + ": encoder state references an unknown depth stencil state";
            return false;
          }
        }
      }
    } else if (event.call_kind == trace::MetalCallKind::SetVertexTexture ||
               event.call_kind == trace::MetalCallKind::SetFragmentTexture) {
      if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
        error = event_label + ": texture bind references an unknown render encoder";
        return false;
      }
      std::uint64_t texture_id = 0;
      if (!require_object_id_field(payload, "texture_id", event_label, "texture bind", texture_id, error) ||
          !require_metal_object_or_null(texture_ids, texture_id, event_label,
              "texture bind references an unknown texture", error)) {
        return false;
      }
      if (!validate_metal_bind_payload(event_label, payload, "texture bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetComputeTexture) {
      if (compute_encoder_has_pipeline.find(event.object_id) == compute_encoder_has_pipeline.end()) {
        error = event_label + ": compute texture bind references an unknown compute encoder";
        return false;
      }
      std::uint64_t texture_id = 0;
      if (!require_object_id_field(payload, "texture_id", event_label, "compute texture bind", texture_id, error) ||
          !require_metal_object_or_null(texture_ids, texture_id, event_label,
              "compute texture bind references an unknown texture", error)) {
        return false;
      }
      if (!validate_metal_bind_payload(event_label, payload, "compute texture bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetVertexSamplerState ||
               event.call_kind == trace::MetalCallKind::SetFragmentSamplerState) {
      if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
        error = event_label + ": sampler bind references an unknown render encoder";
        return false;
      }
      std::uint64_t sampler_state_id = 0;
      if (!require_object_id_field(payload, "sampler_state_id", event_label, "sampler bind", sampler_state_id, error) ||
          !require_metal_object_or_null(sampler_ids, sampler_state_id, event_label,
              "sampler bind references an unknown sampler", error)) {
        return false;
      }
      if (!validate_metal_bind_payload(event_label, payload, "sampler bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::SetComputeSamplerState) {
      if (compute_encoder_has_pipeline.find(event.object_id) == compute_encoder_has_pipeline.end()) {
        error = event_label + ": compute sampler bind references an unknown compute encoder";
        return false;
      }
      std::uint64_t sampler_state_id = 0;
      if (!require_object_id_field(payload, "sampler_state_id", event_label, "compute sampler bind", sampler_state_id, error) ||
          !require_metal_object_or_null(sampler_ids, sampler_state_id, event_label,
              "compute sampler bind references an unknown sampler", error)) {
        return false;
      }
      if (!validate_metal_bind_payload(event_label, payload, "compute sampler bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::DrawIndexedPrimitives) {
      if (!require_metal_object(buffer_ids, object_id_field(payload, "index_buffer_id"), event_label,
              "drawIndexedPrimitives references an unknown index buffer", error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::DrawPrimitivesIndirect ||
               event.call_kind == trace::MetalCallKind::DispatchThreadgroupsIndirect) {
      if (!require_metal_object(buffer_ids, object_id_field(payload, "indirect_buffer_id"), event_label,
              "indirect command references an unknown indirect buffer", error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::DrawIndexedPrimitivesIndirect) {
      if (!require_metal_object(buffer_ids, object_id_field(payload, "index_buffer_id"), event_label,
              "drawIndexedPrimitivesIndirect references an unknown index buffer", error) ||
          !require_metal_object(buffer_ids, object_id_field(payload, "indirect_buffer_id"), event_label,
              "drawIndexedPrimitivesIndirect references an unknown indirect buffer", error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::CopyBuffer ||
               event.call_kind == trace::MetalCallKind::CopyBufferToTexture ||
               event.call_kind == trace::MetalCallKind::CopyTexture ||
               event.call_kind == trace::MetalCallKind::BlitFill) {
      if (!require_metal_object(blit_encoder_ids, event.object_id, event_label,
              "blit command references an unknown blit encoder", error) ||
          !validate_metal_blit_payload_resources(event_label, event.call_kind, payload, buffer_ids, texture_ids, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::BlitBatch) {
      if (!require_metal_object(blit_encoder_ids, event.object_id, event_label,
              "blitBatch references an unknown blit encoder", error) ||
          !validate_metal_blit_batch_payload(event_label, payload, buffer_ids, texture_ids, error)) {
        return false;
      }
	    } else if (event.call_kind == trace::MetalCallKind::PresentDrawable) {
	      auto present_texture_id = object_id_field(payload, "present_texture_id");
	      if (present_texture_id == 0) {
	        present_texture_id = event.object_refs.empty() ? 0 : event.object_refs.front();
	      }
	      if (!require_metal_object(command_buffer_ids, event.object_id, event_label,
	              "presentDrawable references an unknown command buffer", error) ||
	          !require_metal_object(texture_ids, present_texture_id, event_label,
	              "presentDrawable references an unknown drawable texture", error)) {
	        return false;
	      }
      if (!validate_metal_present_payload(event_label, payload, error)) {
        return false;
      }
    } else if (event.call_kind == trace::MetalCallKind::ObjectMetadata ||
               event.call_kind == trace::MetalCallKind::InsertDebugSignpost) {
      const json metadata = event.call_kind == trace::MetalCallKind::InsertDebugSignpost
                                ? json::parse(payload.value("label", std::string()), nullptr, false)
                                : payload;
      if (!metadata.is_discarded() && metadata.is_object()) {
        const auto kind = string_from_json(metadata.value("kind", json(nullptr)));
        if (kind == "dxmt_sampler_gpu_resource_id") {
          if (!validate_metal_sampler_descriptor(event_label, metadata, error)) {
            return false;
          }
          const auto sampler_id = object_id_field(metadata, "sampler_id");
          if (sampler_id != 0) {
            sampler_ids.insert(sampler_id);
          }
        } else if (kind == "dxmt_texture_view") {
          if (!validate_metal_texture_view_descriptor(event_label, metadata, error)) {
            return false;
          }
          const auto source_texture_id = object_id_field(metadata, "source_texture_id");
          if (!require_metal_object(texture_ids, source_texture_id, event_label,
                  "texture view references an unknown source texture", error)) {
            return false;
          }
          const auto texture_id = object_id_field(metadata, "texture_id");
          if (texture_id != 0) {
            texture_ids.insert(texture_id);
          }
        } else if (kind == "dxmt_depth_stencil_state") {
          if (!validate_metal_depth_stencil_descriptor(event_label, metadata, error)) {
            return false;
          }
          const auto depth_stencil_state_id = object_id_field(metadata, "depth_stencil_state_id");
          if (depth_stencil_state_id != 0) {
            depth_stencil_state_ids.insert(depth_stencil_state_id);
          }
        } else if (kind == "dxmt_buffer_gpu_address" ||
                   kind == "dxmt_texture_gpu_resource_id") {
          if (!validate_dxmt_resource_id_metadata(event_label, kind, metadata, error)) {
            return false;
          }
        } else if (kind == "dxmt_copy_buffer_to_texture") {
          if (!require_metal_object(blit_encoder_ids, event.object_id, event_label,
                  "copy buffer to texture signpost references an unknown blit encoder", error) ||
              !validate_metal_blit_payload_resources(
                  event_label, trace::MetalCallKind::CopyBufferToTexture, metadata, buffer_ids, texture_ids, error)) {
            return false;
          }
        } else if (kind == "dxmt_dispatch_threads") {
          const auto encoder = compute_encoder_has_pipeline.find(event.object_id);
          if (encoder == compute_encoder_has_pipeline.end() || !encoder->second) {
            error = event_label + ": dispatch signpost occurs before a valid compute pipeline bind";
            return false;
          }
          if (!validate_metal_work_payload(event_label, trace::MetalCallKind::DispatchThreads, metadata, error)) {
            return false;
          }
          has_draw_or_dispatch = true;
        } else if (kind == "dxmt_set_compute_bytes") {
          if (compute_encoder_has_pipeline.find(event.object_id) == compute_encoder_has_pipeline.end()) {
            error = event_label + ": compute bytes signpost references an unknown compute encoder";
            return false;
          }
          if (!validate_metal_inline_bytes(event_label, metadata, "compute bytes signpost", error)) {
            return false;
          }
        } else if (kind == "dxmt_set_rasterizer_state" ||
                   kind == "dxmt_set_blend_factor" ||
                   kind == "dxmt_set_viewports" ||
                   kind == "dxmt_set_scissor_rects" ||
                   kind == "dxmt_set_depth_stencil_state") {
          if (render_encoder_has_pipeline.find(event.object_id) == render_encoder_has_pipeline.end()) {
            error = event_label + ": render state signpost references an unknown render encoder";
            return false;
          }
          if (!validate_dxmt_encoder_state_payload(event_label, kind, metadata, error)) {
            return false;
          }
          if (kind == "dxmt_set_viewports") {
            render_encoder_has_viewport[event.object_id] = true;
          } else if (kind == "dxmt_set_scissor_rects") {
            render_encoder_has_scissor[event.object_id] = true;
          }
          if (kind == "dxmt_set_depth_stencil_state") {
            const auto depth_stencil_state_id = object_id_field(metadata, "depth_stencil_state_id");
            if (depth_stencil_state_id != 0 &&
                depth_stencil_state_ids.find(depth_stencil_state_id) == depth_stencil_state_ids.end()) {
              error = event_label + ": depth stencil state signpost references an unknown depth stencil state";
              return false;
            }
          }
        }
      }
    }
    if (is_metal_draw_or_dispatch(event.call_kind)) {
      if (!validate_metal_work_payload(event_label, event.call_kind, payload, error)) {
        return false;
      }
      if (event.call_kind == trace::MetalCallKind::DrawPrimitives ||
          event.call_kind == trace::MetalCallKind::DrawIndexedPrimitives ||
          event.call_kind == trace::MetalCallKind::DrawPrimitivesIndirect ||
          event.call_kind == trace::MetalCallKind::DrawIndexedPrimitivesIndirect ||
          event.call_kind == trace::MetalCallKind::DispatchThreadsPerTile) {
        const auto encoder = render_encoder_has_pipeline.find(event.object_id);
        if (encoder == render_encoder_has_pipeline.end() || !encoder->second) {
          error = event_label + ": render work occurs before a valid render pipeline bind";
          return false;
        }
        if (event.call_kind != trace::MetalCallKind::DispatchThreadsPerTile) {
          const auto viewport = render_encoder_has_viewport.find(event.object_id);
	        if (viewport == render_encoder_has_viewport.end() || !viewport->second) {
	          error = event_label + ": draw occurs before a valid viewport bind";
	          return false;
	        }
	      }
      } else {
        const auto encoder = compute_encoder_has_pipeline.find(event.object_id);
        if (encoder == compute_encoder_has_pipeline.end() || !encoder->second) {
          error = event_label + ": dispatch occurs before a valid compute pipeline bind";
          return false;
        }
      }
      has_draw_or_dispatch = true;
    }

    ++statistics.metal_calls_replayed;
    if (event.call_kind == trace::MetalCallKind::PresentDrawable) {
      ++statistics.metal_presents_seen;
      ++statistics.presents_seen;
    }
  }

  if (!has_library_asset) {
    error = "metal validate-only found no referenced library assets";
    return false;
  }
  if (!has_pipeline_asset) {
    error = "metal validate-only found no referenced pipeline assets";
    return false;
  }
  if (!has_pipeline_bind) {
    error = "metal validate-only found no pipeline bind calls";
    return false;
  }
  if (!has_draw_or_dispatch) {
    error = "metal validate-only found no draw or dispatch calls";
    return false;
  }
	  return true;
	}

} // namespace

struct ReplaySession::Impl {
  explicit Impl(ReplayOptions opts) : options(std::move(opts)) {}

  ReplayOptions options;
  ReplayStatistics statistics;
  trace::TraceBundleReader reader;
  std::string last_error;
};

ReplaySession::ReplaySession(ReplayOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

ReplaySession::~ReplaySession() = default;

bool ReplaySession::run()
{
  impl_->statistics = ReplayStatistics{};
  impl_->last_error.clear();

  if (impl_->options.bundle_root.empty()) {
    impl_->last_error = "bundle root is empty";
    return false;
  }

  if (!impl_->reader.open(impl_->options.bundle_root)) {
    impl_->last_error = impl_->reader.last_error().empty() ? "failed to open trace bundle" : impl_->reader.last_error();
    return false;
  }

  if (impl_->options.enable_metal_retrace) {
    if (!impl_->reader.metadata().has_metal_callstream && impl_->reader.metal_events().empty()) {
      impl_->last_error = "metal retrace requested but bundle has no metal callstream";
      return false;
    }
    if (impl_->options.validate_only) {
      impl_->statistics.backend_name = "metal-validate-only";
      return validate_metal_replay_closure(impl_->reader, impl_->statistics, impl_->last_error);
    }

    register_builtin_metal_replay_backends();
    const auto *factory = find_metal_replay_backend(impl_->options.metal_backend_name);
    if (factory == nullptr) {
      impl_->last_error = "unknown metal replay backend: " + impl_->options.metal_backend_name;
      return false;
    }

    auto backend = (*factory)();
    if (!backend || !backend->initialize(impl_->reader, impl_->options)) {
      impl_->last_error = backend && !backend->last_error().empty() ? backend->last_error()
                                                                    : "failed to initialize metal replay backend";
      return false;
    }

    impl_->statistics.backend_name = "metal-" + impl_->options.metal_backend_name;
    for (const auto &event : impl_->reader.metal_events()) {
      if (!backend->replay_metal_event(event)) {
        if (!backend->last_error().empty()) {
          impl_->last_error = "metal sequence " + std::to_string(event.metal_sequence) + " " +
                              event.function_name + ": " + backend->last_error();
        } else {
          impl_->last_error = "metal sequence " + std::to_string(event.metal_sequence) + " " +
                              event.function_name + ": metal replay failed";
        }
        return false;
      }

      ++impl_->statistics.metal_calls_replayed;
      if (event.call_kind == trace::MetalCallKind::PresentDrawable) {
        ++impl_->statistics.metal_presents_seen;
        ++impl_->statistics.presents_seen;
      }
    }

    if (!backend->finalize()) {
      impl_->last_error = backend->last_error().empty() ? "metal replay finalization failed" : backend->last_error();
      return false;
    }
    return true;
  }

#if !defined(_WIN32) && !defined(APITRACE_HAS_D3D_NATIVE)
  if (!impl_->options.validate_only) {
    impl_->last_error = "retrace backends are only implemented for Windows retrace.exe in the current MVP.";
    return false;
  }
#endif
  if (impl_->reader.metadata().api == trace::ApiKind::D3D11) {
#if !defined(APITRACE_HAS_D3D_NATIVE)
    if (impl_->options.validate_only) {
      internal::D3D11ReplayPlan plan;
      return internal::build_d3d11_replay_plan(impl_->reader, plan, impl_->last_error);
    }
    impl_->last_error = "D3D11 replay backend is only implemented for Windows retrace.exe.";
    return false;
#else
    if (impl_->options.backend != BackendKind::TranslationLayer &&
        impl_->options.backend != BackendKind::NativeD3D11 &&
        impl_->options.backend != BackendKind::NativeD3D12) {
      impl_->last_error = std::string("backend ") + backend_name(impl_->options.backend) +
                          " is unsupported for the D3D11 retrace MVP";
      return false;
    }

    internal::D3D11ReplayPlan plan;
    if (!internal::build_d3d11_replay_plan(impl_->reader, plan, impl_->last_error)) {
      return false;
    }
    if (impl_->options.validate_only) {
      return true;
    }

    if (!d3d11::internal::replay_translation_layer_plan(plan, impl_->statistics, impl_->last_error)) {
      return false;
    }
#if !defined(_WIN32)
    impl_->statistics.backend_name = "native-d3d11";
#endif
    return true;
#endif
  }

  if (impl_->reader.metadata().api == trace::ApiKind::D3D12) {
    if (impl_->options.backend == BackendKind::NativeD3D11) {
      impl_->last_error = "backend NativeD3D11 is unsupported for D3D12 bundles";
      return false;
    }

    d3d12::D3D12ReplayBackend backend;
    if (!backend.initialize(impl_->reader)) {
      impl_->last_error = backend.last_error().empty() ? "failed to initialize D3D12 replay backend" : backend.last_error();
      return false;
    }

    impl_->statistics.backend_name =
        impl_->options.backend == BackendKind::NativeD3D12 ? "native-d3d12" : "bundle-d3d12";
    for (const auto &event : impl_->reader.events()) {
      if (!backend.replay_event(event)) {
        if (!backend.last_error().empty()) {
          impl_->last_error = "sequence " + std::to_string(event.callsite.sequence) + " " +
                              event.callsite.function_name + ": " + backend.last_error();
        } else {
          impl_->last_error = "sequence " + std::to_string(event.callsite.sequence) + " " +
                              event.callsite.function_name + ": D3D12 replay failed";
        }
        return false;
      }

      if (event.kind == trace::EventKind::Boundary) {
        switch (event.boundary) {
        case trace::BoundaryKind::Frame:
          ++impl_->statistics.frames_seen;
          break;
        case trace::BoundaryKind::Present:
          ++impl_->statistics.presents_seen;
          break;
        default:
          break;
        }
      } else {
        ++impl_->statistics.calls_replayed;
      }
    }
    if (impl_->options.validate_only) {
      if (!backend.validate_only()) {
        impl_->last_error = backend.last_error().empty() ? "D3D12 replay validation failed" : backend.last_error();
        return false;
      }
    } else {
      if (!backend.finalize_replay()) {
        impl_->last_error = backend.last_error().empty() ? "D3D12 replay finalization failed" : backend.last_error();
        return false;
      }
    }
    backend.shutdown();
    return true;
  }

  impl_->last_error = "only D3D11 and D3D12 bundles are supported by the retrace MVP";
  return false;
}

const ReplayOptions &ReplaySession::options() const noexcept
{
  return impl_->options;
}

const ReplayStatistics &ReplaySession::statistics() const noexcept
{
  return impl_->statistics;
}

const std::string &ReplaySession::last_error() const noexcept
{
  return impl_->last_error;
}

} // namespace apitrace::replay
