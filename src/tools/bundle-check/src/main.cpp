#include "apitrace/d3d12_replay.hpp"
#include "trace/src/payload_object_refs.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kMetalTextureSwizzleAlpha = 5;

struct AssetJsonCache {
  std::unordered_map<std::string, json> parsed;
};

std::string read_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void print_usage(const char *argv0)
{
  std::cerr << "usage: " << (argv0 ? argv0 : "bundle-check")
            << " [--require-asset-index]"
            << " [--require-d3d] [--require-metal] [--require-translation-links]"
            << " [--require-shared-resources] [--require-d3d-replay-closure]"
            << " [--require-d3d-native-readiness]"
            << " [--require-metal-replay-closure]"
            << " [--require-d3d-present-frames] [--require-metal-present-frames]"
            << " [--strict-cross-api]"
            << " <trace-bundle>\n";
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

bool is_generic_buffer_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  return part != path.end() && *part == "buffers" && path.extension() == ".buffer";
}

bool is_generic_texture_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  return part != path.end() && *part == "textures" && path.extension() == ".texture";
}

enum class GenericResourcePathKind {
  None,
  Buffer,
  Texture,
};

GenericResourcePathKind generic_resource_path_kind(const std::filesystem::path &path)
{
  if (is_generic_buffer_asset_path(path)) {
    return GenericResourcePathKind::Buffer;
  }
  if (is_generic_texture_asset_path(path)) {
    return GenericResourcePathKind::Texture;
  }
  return GenericResourcePathKind::None;
}

bool is_d3d_pipeline_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  return part != path.end() && *part == "pipelines" && path.extension() == ".json" &&
         path.filename().generic_string().find(".pipeline.") != std::string::npos;
}

bool is_d3d_shader_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  if (part == path.end() || *part != "shaders") {
    return false;
  }
  const auto extension = path.extension().generic_string();
  return extension == ".dxbc" || extension == ".dxil";
}

bool is_d3d_root_signature_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  return part != path.end() && *part == "shaders" && path.extension() == ".rootsig";
}

bool is_metal_library_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  if (part == path.end() || *part != "metal") {
    return false;
  }
  ++part;
  return part != path.end() && *part == "libraries" && path.extension() == ".metallib";
}

bool is_metal_pipeline_asset_path(const std::filesystem::path &path)
{
  auto part = path.begin();
  if (part == path.end() || *part != "metal") {
    return false;
  }
  ++part;
  return part != path.end() && *part == "pipelines" && path.extension() == ".json" &&
         path.filename().generic_string().find(".pipeline.") != std::string::npos;
}

struct CheckOptions {
  bool require_asset_index = false;
  bool require_d3d = false;
  bool require_metal = false;
  bool require_translation_links = false;
  bool require_shared_resources = false;
  bool require_d3d_replay_closure = false;
  bool require_d3d_native_readiness = false;
  bool require_metal_replay_closure = false;
  bool require_d3d_present_frames = false;
  bool require_metal_present_frames = false;
};

void enable_strict_cross_api(CheckOptions &options)
{
  options.require_asset_index = true;
  options.require_d3d = true;
  options.require_metal = true;
  options.require_translation_links = true;
  options.require_shared_resources = true;
  options.require_d3d_replay_closure = true;
  options.require_d3d_native_readiness = true;
  options.require_metal_replay_closure = true;
  options.require_d3d_present_frames = true;
  options.require_metal_present_frames = true;
}

struct SharedResourceStats {
  std::size_t generic_buffer_assets = 0;
  std::size_t generic_texture_assets = 0;
  std::size_t metal_buffer_assets = 0;
  std::size_t metal_texture_assets = 0;
  std::size_t paths_referenced_by_d3d = 0;
  std::size_t paths_referenced_by_metal = 0;
  std::size_t shared_resource_paths = 0;
  std::size_t d3d_buffer_paths = 0;
  std::size_t d3d_texture_paths = 0;
  std::size_t metal_buffer_paths = 0;
  std::size_t metal_texture_paths = 0;
  std::size_t shared_buffer_paths = 0;
  std::size_t shared_texture_paths = 0;
  std::size_t duplicated_cross_api_resource_hashes = 0;
  std::size_t d3d_pipeline_paths = 0;
  std::size_t d3d_shader_paths = 0;
  std::size_t d3d_root_signature_paths = 0;
  std::size_t d3d_pipeline_dependent_calls = 0;
  std::size_t metal_library_paths = 0;
  std::size_t metal_pipeline_paths = 0;
  std::size_t metal_pipeline_bind_calls = 0;
  std::size_t metal_draw_or_dispatch_calls = 0;
};

struct PresentFrameStats {
  std::size_t d3d_present_calls = 0;
  std::size_t d3d_present_boundaries = 0;
  std::size_t d3d_present_frame_assets = 0;
  std::size_t metal_present_drawables = 0;
  std::size_t metal_present_frame_assets = 0;
};

struct TranslationLinkStats {
  std::size_t total = 0;
  std::size_t draw_scope_links = 0;
  std::size_t draw_scope_links_with_metal_work = 0;
  std::size_t draw_scope_links_to_d3d_pipeline_work = 0;
};

bool is_d3d12_pipeline_dependent_function(std::string_view function_name)
{
  return function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
         function_name == "ID3D12Device::CreateComputePipelineState" ||
         function_name == "ID3D12Device2::CreatePipelineState" ||
         function_name == "ID3D12GraphicsCommandList::SetPipelineState" ||
         function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
         function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced" ||
         function_name == "ID3D12GraphicsCommandList::Dispatch" ||
         function_name == "ID3D12GraphicsCommandList4::DispatchRays" ||
         function_name == "ID3D12GraphicsCommandList6::DispatchMesh";
}

bool is_metal_draw_or_dispatch_call(apitrace::trace::MetalCallKind kind)
{
  switch (kind) {
  case apitrace::trace::MetalCallKind::DrawPrimitives:
  case apitrace::trace::MetalCallKind::DrawIndexedPrimitives:
  case apitrace::trace::MetalCallKind::DrawPrimitivesIndirect:
  case apitrace::trace::MetalCallKind::DrawIndexedPrimitivesIndirect:
  case apitrace::trace::MetalCallKind::DispatchThreadgroups:
  case apitrace::trace::MetalCallKind::DispatchThreadgroupsIndirect:
  case apitrace::trace::MetalCallKind::DispatchThreads:
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

std::string bundle_hash_from_records(const std::vector<apitrace::trace::ChecksumRecord> &files)
{
  auto sorted_files = files;
  std::sort(sorted_files.begin(), sorted_files.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
  });
  std::string bundle_fingerprint_source;
  for (const auto &record : sorted_files) {
    bundle_fingerprint_source += record.relative_path.generic_string();
    bundle_fingerprint_source += "=";
    bundle_fingerprint_source += record.digest;
    bundle_fingerprint_source += "\n";
  }
  return "sha256:" + apitrace::trace::content_hash_bytes(
                       bundle_fingerprint_source.data(),
                       bundle_fingerprint_source.size());
}

void collect_asset_paths(const json &value, std::vector<std::string> &paths)
{
  if (value.is_object()) {
    for (const auto &[key, child] : value.items()) {
      if ((ends_with(key, "_path") || key == "path") && child.is_string()) {
        paths.push_back(child.get<std::string>());
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

void collect_nested_asset_paths(const json &value, std::vector<std::string> &paths)
{
  if (value.is_object()) {
    if (const auto type = value.find("type"); type != value.end() && type->is_string() &&
        (*type == "graphics" || *type == "compute")) {
      for (std::string_view stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
        const auto shader = value.find(std::string(stage));
        if (shader == value.end() || !shader->is_object()) {
          continue;
        }
        const auto path_key = std::string(stage) + "_path";
        const auto path = shader->find(path_key);
        if (path != shader->end() && path->is_string()) {
          paths.push_back(path->get<std::string>());
        }
      }
      return;
    }

    for (const auto &[key, child] : value.items()) {
      if ((ends_with(key, "_path") || key == "path") && child.is_string()) {
        paths.push_back(child.get<std::string>());
      }
      collect_nested_asset_paths(child, paths);
    }
    return;
  }

  if (value.is_array()) {
    for (const auto &child : value) {
      collect_nested_asset_paths(child, paths);
    }
  }
}

bool read_asset_json(
    const apitrace::trace::TraceBundleReader &reader,
    const std::string &relative_path_text,
    AssetJsonCache &cache,
    json &asset_json,
    std::string &error)
{
  if (const auto cached = cache.parsed.find(relative_path_text); cached != cache.parsed.end()) {
    asset_json = cached->second;
    return true;
  }
  const std::filesystem::path relative_path = relative_path_text;
  const auto content = read_file(reader.layout().root_path / relative_path);
  asset_json = json::parse(content, nullptr, false);
  if (asset_json.is_discarded()) {
    error = "referenced asset JSON is invalid: " + relative_path_text;
    return false;
  }
  cache.parsed.emplace(relative_path_text, asset_json);
  return true;
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

std::uint64_t object_id_field(const json &payload, const char *key)
{
  const auto it = payload.find(key);
  return it == payload.end() ? 0 : json_u64(*it);
}

bool require_object_id_field(
    const json &payload,
    const char *key,
    const std::string &error_prefix,
    const char *description,
    std::uint64_t &object_id,
    std::string &error)
{
  const auto it = payload.find(key);
  if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
    error = error_prefix + description + " is missing " + key;
    return false;
  }
  object_id = json_u64(*it);
  return true;
}

bool require_metal_object(
    const std::unordered_set<std::uint64_t> &objects,
    std::uint64_t object_id,
    const std::string &error_prefix,
    const char *description,
    std::string &error)
{
  if (object_id == 0 || objects.find(object_id) == objects.end()) {
    error = error_prefix + description;
    return false;
  }
  return true;
}

bool require_metal_object_or_null(
    const std::unordered_set<std::uint64_t> &objects,
    std::uint64_t object_id,
    const std::string &error_prefix,
    const char *description,
    std::string &error)
{
  if (object_id == 0) {
    return true;
  }
  return require_metal_object(objects, object_id, error_prefix, description, error);
}

std::uint64_t object_id_from_pipeline_json(const json &pipeline, const char *key)
{
  const auto it = pipeline.find(key);
  return it == pipeline.end() ? 0 : json_u64(*it);
}

std::string string_from_json(const json &value)
{
  return value.is_string() ? value.get<std::string>() : std::string();
}

std::string path_from_pipeline_shader(const json &shader, const char *stage)
{
  const auto path_it = shader.find(std::string(stage) + "_path");
  return path_it == shader.end() ? std::string() : string_from_json(*path_it);
}

bool verify_pipeline_shader_metadata(
    const std::string &label,
    const json &pipeline,
    const char *stage,
    std::unordered_set<std::string> &shader_paths,
    std::string &error)
{
  const auto shader_it = pipeline.find(stage);
  if (shader_it == pipeline.end() || shader_it->is_null()) {
    return true;
  }
  if (!shader_it->is_object()) {
    error = label + ": pipeline shader stage is not an object: " + stage;
    return false;
  }
  const auto size_it = shader_it->find("bytecode_size");
  if (size_it == shader_it->end() || json_u64(*size_it) == 0) {
    error = label + ": pipeline shader stage is missing bytecode_size: " + stage;
    return false;
  }
  const auto shader_path = path_from_pipeline_shader(*shader_it, stage);
  if (shader_path.empty()) {
    error = label + ": pipeline shader stage is missing path: " + stage;
    return false;
  }
  const std::filesystem::path relative_path(shader_path);
  if (!is_d3d_shader_asset_path(relative_path)) {
    error = label + ": pipeline shader stage does not reference a D3D shader asset: " + stage;
    return false;
  }
  shader_paths.insert(shader_path);
  return true;
}

bool require_d3d_pipeline_bool_field(
    const std::string &label,
    const json &object,
    const char *field,
    const std::string &description,
    std::string &error)
{
  const auto it = object.find(field);
  if (it == object.end() || !it->is_boolean()) {
    error = label + ": pipeline " + description + " is missing boolean field " + field;
    return false;
  }
  return true;
}

bool require_d3d_pipeline_numeric_field(
    const std::string &label,
    const json &object,
    const char *field,
    const std::string &description,
    std::string &error)
{
  const auto it = object.find(field);
  if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer() && !it->is_number_float())) {
    error = label + ": pipeline " + description + " is missing numeric field " + field;
    return false;
  }
  return true;
}

bool verify_d3d_graphics_pipeline_metadata(
    const std::string &label,
    const json &pipeline,
    std::string &error)
{
  const auto blend_state = pipeline.find("blend_state");
  const auto rasterizer_state = pipeline.find("rasterizer_state");
  const auto depth_stencil_state = pipeline.find("depth_stencil_state");
  if (blend_state == pipeline.end() || !blend_state->is_object()) {
    error = label + ": graphics pipeline asset missing blend_state";
    return false;
  }
  if (rasterizer_state == pipeline.end() || !rasterizer_state->is_object()) {
    error = label + ": graphics pipeline asset missing rasterizer_state";
    return false;
  }
  if (depth_stencil_state == pipeline.end() || !depth_stencil_state->is_object()) {
    error = label + ": graphics pipeline asset missing depth_stencil_state";
    return false;
  }

  if (!require_d3d_pipeline_bool_field(label, *blend_state, "alpha_to_coverage_enable", "blend_state", error) ||
      !require_d3d_pipeline_bool_field(label, *blend_state, "independent_blend_enable", "blend_state", error)) {
    return false;
  }
  const auto render_targets = blend_state->find("render_targets");
  if (render_targets == blend_state->end() || !render_targets->is_array()) {
    error = label + ": pipeline blend_state is missing render_targets";
    return false;
  }
  std::size_t render_target_index = 0;
  for (const auto &target : *render_targets) {
    if (!target.is_object()) {
      error = label + ": pipeline blend_state render target must be an object";
      return false;
    }
    const auto description = "blend_state render target " + std::to_string(render_target_index);
    if (!require_d3d_pipeline_bool_field(label, target, "blend_enable", description, error) ||
        !require_d3d_pipeline_bool_field(label, target, "logic_op_enable", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "src_blend", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "dest_blend", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "blend_op", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "src_blend_alpha", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "dest_blend_alpha", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "blend_op_alpha", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "logic_op", description, error) ||
        !require_d3d_pipeline_numeric_field(label, target, "render_target_write_mask", description, error)) {
      return false;
    }
    ++render_target_index;
  }

  if (!require_d3d_pipeline_numeric_field(label, *rasterizer_state, "fill_mode", "rasterizer_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *rasterizer_state, "cull_mode", "rasterizer_state", error) ||
      !require_d3d_pipeline_bool_field(label, *rasterizer_state, "front_counter_clockwise", "rasterizer_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *rasterizer_state, "depth_bias", "rasterizer_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *rasterizer_state, "depth_bias_clamp", "rasterizer_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *rasterizer_state, "slope_scaled_depth_bias", "rasterizer_state", error) ||
      !require_d3d_pipeline_bool_field(label, *rasterizer_state, "depth_clip_enable", "rasterizer_state", error) ||
      !require_d3d_pipeline_bool_field(label, *rasterizer_state, "multisample_enable", "rasterizer_state", error) ||
      !require_d3d_pipeline_bool_field(label, *rasterizer_state, "antialiased_line_enable", "rasterizer_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *rasterizer_state, "forced_sample_count", "rasterizer_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *rasterizer_state, "conservative_raster", "rasterizer_state", error)) {
    return false;
  }

  if (!require_d3d_pipeline_bool_field(label, *depth_stencil_state, "depth_enable", "depth_stencil_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *depth_stencil_state, "depth_write_mask", "depth_stencil_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *depth_stencil_state, "depth_func", "depth_stencil_state", error) ||
      !require_d3d_pipeline_bool_field(label, *depth_stencil_state, "stencil_enable", "depth_stencil_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *depth_stencil_state, "stencil_read_mask", "depth_stencil_state", error) ||
      !require_d3d_pipeline_numeric_field(label, *depth_stencil_state, "stencil_write_mask", "depth_stencil_state", error)) {
    return false;
  }
  for (const auto *face_name : {"front_face", "back_face"}) {
    const auto face = depth_stencil_state->find(face_name);
    if (face == depth_stencil_state->end() || !face->is_object()) {
      error = label + ": pipeline depth_stencil_state is missing " + face_name;
      return false;
    }
    if (!require_d3d_pipeline_numeric_field(label, *face, "stencil_fail_op", face_name, error) ||
        !require_d3d_pipeline_numeric_field(label, *face, "stencil_depth_fail_op", face_name, error) ||
        !require_d3d_pipeline_numeric_field(label, *face, "stencil_pass_op", face_name, error) ||
        !require_d3d_pipeline_numeric_field(label, *face, "stencil_func", face_name, error)) {
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
  const auto vertex_library_id = json_u64(
      descriptor.value("vertex_library_id", descriptor.value("library_id", json(nullptr))));
  if (vertex_library_id == 0 || library_ids.find(vertex_library_id) == library_ids.end()) {
    error = label + ": render pipeline descriptor references an unknown vertex library";
    return false;
  }
  const auto fragment_function = string_from_json(descriptor.value("fragment_function", json(nullptr)));
  if (!fragment_function.empty()) {
    const auto fragment_library_id = json_u64(
        descriptor.value("fragment_library_id", descriptor.value("library_id", json(nullptr))));
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

bool validate_metal_render_pass_resources(
    const std::string &error_prefix,
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
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_action_field =
      [&](const json &object, const char *field, const char *description, std::initializer_list<std::string_view> names) -> bool {
    const auto it = object.find(field);
    if (it == object.end()) {
      error = error_prefix + description + " is missing " + field;
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
    error = error_prefix + description + " has invalid " + field;
    return false;
  };
  const auto require_clear_color = [&](const json &object, const char *description) -> bool {
    const auto it = object.find("clear_color");
    if (it == object.end() || !it->is_array()) {
      error = error_prefix + description + " is missing clear_color";
      return false;
    }
    if (it->size() != 4) {
      error = error_prefix + description + " has invalid clear_color";
      return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (!(*it)[index].is_number()) {
        error = error_prefix + description + " has invalid clear_color";
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

  bool has_color_texture = false;
  auto color_texture_id = object_id_field(pass, "color_texture_id");
  if (color_texture_id == 0) {
    color_texture_id = object_id_field(pass, "drawable_id");
  }
  if (color_texture_id != 0) {
    if (texture_ids.find(color_texture_id) == texture_ids.end()) {
      error = error_prefix + "render pass references an unknown color texture";
      return false;
    }
    if (!validate_color_attachment(pass, "render pass color attachment")) {
      return false;
    }
    has_color_texture = true;
  }

  const auto colors = pass.find("colors");
  if (colors != pass.end() && colors->is_array()) {
    for (const auto &color : *colors) {
      const auto texture_id = object_id_field(color, "texture");
      if (texture_id != 0) {
        if (texture_ids.find(texture_id) == texture_ids.end()) {
          error = error_prefix + "render pass references an unknown color texture";
          return false;
        }
        has_color_texture = true;
      }
      if (texture_id != 0 && !validate_color_attachment(color, "render pass color attachment")) {
        return false;
      }
      const auto resolve_texture_id = object_id_field(color, "resolve_texture");
      if (resolve_texture_id != 0 &&
          texture_ids.find(resolve_texture_id) == texture_ids.end()) {
        error = error_prefix + "render pass references an unknown resolve texture";
        return false;
      }
      if (resolve_texture_id != 0 &&
          !validate_resolve_attachment(color, "render pass resolve attachment")) {
        return false;
      }
    }
  }

  if (!has_color_texture) {
    error = error_prefix + "render pass is missing color texture metadata";
    return false;
  }

  const auto depth = pass.find("depth");
  if (depth != pass.end() && depth->is_object()) {
    const auto depth_texture_id = object_id_field(*depth, "texture");
    if (depth_texture_id != 0 && texture_ids.find(depth_texture_id) == texture_ids.end()) {
      error = error_prefix + "render pass references an unknown depth texture";
      return false;
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
      error = error_prefix + "render pass depth attachment is missing clear_depth";
      return false;
    }
  }
  return true;
}

bool validate_metal_texture_descriptor(
    const std::string &error_prefix,
    const json &payload,
    std::string &error)
{
  const auto descriptor = nested_json(payload, "descriptor");
  if (!descriptor.is_object() || descriptor.empty()) {
    error = error_prefix + "newTexture is missing descriptor";
    return false;
  }
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = descriptor.find(field);
    if (it == descriptor.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + "texture descriptor is missing " + field;
      return false;
    }
    return true;
  };
  const auto pixel_format = descriptor.find("pixel_format");
  if (pixel_format == descriptor.end() ||
      (!pixel_format->is_string() && !pixel_format->is_number_unsigned() && !pixel_format->is_number_integer())) {
    error = error_prefix + "texture descriptor is missing pixel_format";
    return false;
  }
  if (pixel_format->is_string()) {
    const auto &format = pixel_format->get_ref<const std::string &>();
    if (format != "bgra8unorm" && format != "rgba8unorm" && format != "r32uint") {
      error = error_prefix + "texture descriptor has invalid pixel_format";
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

bool validate_metal_inline_bytes(
    const std::string &error_prefix,
    const json &payload,
    const char *description,
    std::string &error)
{
  const auto index = payload.find("index");
  if (index == payload.end() || (!index->is_number_unsigned() && !index->is_number_integer())) {
    error = error_prefix + description + " is missing index";
    return false;
  }
  auto nested = nested_json(payload, "payload");
  if (!nested.is_object() || nested.find("bytes") == nested.end()) {
    nested = payload;
  }
  const auto bytes = nested.find("bytes");
  if (bytes == nested.end() || !bytes->is_array() || bytes->empty()) {
    error = error_prefix + description + " is missing captured bytes";
    return false;
  }
  const auto length_it = nested.find("length");
  if (length_it == nested.end() || (!length_it->is_number_unsigned() && !length_it->is_number_integer())) {
    error = error_prefix + description + " is missing length";
    return false;
  }
  const auto length = json_u64(*length_it);
  if (length == 0 || length > bytes->size()) {
    error = error_prefix + description + " has invalid byte length";
    return false;
  }
  return true;
}

bool validate_metal_bind_payload(
    const std::string &error_prefix,
    const json &payload,
    const char *description,
    bool require_offset,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  return require_integer_field("index") &&
         (!require_offset || require_integer_field("offset"));
}

bool validate_metal_sampler_descriptor(
    const std::string &error_prefix,
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
      error = error_prefix + "sampler descriptor is missing " + std::string(field);
      return false;
    }
    if (field == "lod_max_clamp" || field == "lod_min_clamp") {
      if (!it->is_number()) {
        error = error_prefix + "sampler descriptor field " + std::string(field) + " must be numeric";
        return false;
      }
    } else if (field == "lod_average" || field == "normalized_coordinates" || field == "support_argument_buffers") {
      if (!it->is_boolean()) {
        error = error_prefix + "sampler descriptor field " + std::string(field) + " must be boolean";
        return false;
      }
    } else if (!it->is_number_unsigned() && !it->is_number_integer()) {
      error = error_prefix + "sampler descriptor field " + std::string(field) + " must be an integer";
      return false;
    }
  }
  if (object_id_field(payload, "sampler_id") == 0) {
    error = error_prefix + "sampler descriptor has zero sampler_id";
    return false;
  }
  return true;
}

bool validate_metal_resource_usage_payload(
    const std::string &error_prefix,
    const json &payload,
    bool render_encoder,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field, const char *description) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  return require_integer_field("usage", "useResource") &&
         (!render_encoder || require_integer_field("stages", "useResource"));
}

bool validate_metal_present_payload(
    const std::string &error_prefix,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + "presentDrawable is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_positive_integer_field = [&](const char *field) -> bool {
    if (!require_integer_field(field)) {
      return false;
    }
    if (json_u64(payload[field]) == 0) {
      error = error_prefix + "presentDrawable has zero " + std::string(field);
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

bool validate_metal_texture_view_descriptor(
    const std::string &error_prefix,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + "texture view descriptor is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_swizzle = [&]() -> bool {
    const auto it = payload.find("swizzle");
    if (it == payload.end() || !it->is_array()) {
      error = error_prefix + "texture view descriptor is missing swizzle";
      return false;
    }
    if (it->size() != 4) {
      error = error_prefix + "texture view descriptor has invalid swizzle";
      return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (!(*it)[index].is_number_unsigned() && !(*it)[index].is_number_integer()) {
        error = error_prefix + "texture view descriptor has invalid swizzle";
        return false;
      }
      if ((*it)[index].is_number_integer() && (*it)[index].get<std::int64_t>() < 0) {
        error = error_prefix + "texture view descriptor has invalid swizzle";
        return false;
      }
      if ((*it)[index].get<std::uint64_t>() > kMetalTextureSwizzleAlpha) {
        error = error_prefix + "texture view descriptor has invalid swizzle";
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
    const std::string &error_prefix,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_bool_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_boolean()) {
      error = error_prefix + description + " is missing " + field;
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
      error = error_prefix + "depth stencil descriptor has invalid " + field;
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
    const std::string &error_prefix,
    const std::string &kind,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const char *field) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + kind + " is missing " + field;
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

bool validate_metal_blit_payload_resources(
    const std::string &error_prefix,
    apitrace::trace::MetalCallKind kind,
    const json &payload,
    const std::unordered_set<std::uint64_t> &buffer_ids,
    const std::unordered_set<std::uint64_t> &texture_ids,
    std::string &error)
{
  const auto require_numeric_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_positive_numeric_field = [&](const json &object, const char *field, const char *description) -> bool {
    if (!require_numeric_field(object, field, description)) {
      return false;
    }
    if (json_u64(object[field]) == 0) {
      error = error_prefix + description + " has zero " + field;
      return false;
    }
    return true;
  };
  const auto require_size_array = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->size() != 3) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    for (const auto &component : *it) {
      if ((!component.is_number_unsigned() && !component.is_number_integer()) || json_u64(component) == 0) {
        error = error_prefix + description + " has invalid " + field;
        return false;
      }
    }
    return true;
  };
  const auto require_numeric_array = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->size() != 3) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    for (const auto &component : *it) {
      if (!component.is_number_unsigned() && !component.is_number_integer()) {
        error = error_prefix + description + " has invalid " + field;
        return false;
      }
    }
    return true;
  };

  switch (kind) {
  case apitrace::trace::MetalCallKind::CopyBuffer:
    return require_positive_numeric_field(payload, "size", "copyFromBuffer") &&
           require_numeric_field(payload, "source_offset", "copyFromBuffer") &&
           require_numeric_field(payload, "destination_offset", "copyFromBuffer") &&
           require_metal_object(buffer_ids, object_id_field(payload, "source_buffer_id"), error_prefix,
               "copyFromBuffer references an unknown source buffer", error) &&
           require_metal_object(buffer_ids, object_id_field(payload, "destination_buffer_id"), error_prefix,
               "copyFromBuffer references an unknown destination buffer", error);
  case apitrace::trace::MetalCallKind::CopyBufferToTexture:
    return require_numeric_field(payload, "source_offset", "copyFromBufferToTexture") &&
           require_positive_numeric_field(payload, "source_bytes_per_row", "copyFromBufferToTexture") &&
           require_positive_numeric_field(payload, "source_bytes_per_image", "copyFromBufferToTexture") &&
           require_size_array(payload, "source_size", "copyFromBufferToTexture") &&
           require_numeric_field(payload, "destination_slice", "copyFromBufferToTexture") &&
           require_numeric_field(payload, "destination_level", "copyFromBufferToTexture") &&
           require_numeric_array(payload, "destination_origin", "copyFromBufferToTexture") &&
           require_metal_object(buffer_ids, object_id_field(payload, "source_buffer"), error_prefix,
               "copyFromBufferToTexture references an unknown source buffer", error) &&
           require_metal_object(texture_ids, object_id_field(payload, "destination_texture"), error_prefix,
               "copyFromBufferToTexture references an unknown destination texture", error);
  case apitrace::trace::MetalCallKind::CopyTexture:
    {
      const auto nested = nested_json(payload, "payload");
      return require_numeric_field(nested, "source_slice", "copyFromTexture") &&
             require_numeric_field(nested, "source_level", "copyFromTexture") &&
             require_numeric_array(nested, "source_origin", "copyFromTexture") &&
             require_size_array(nested, "source_size", "copyFromTexture") &&
             require_numeric_field(nested, "destination_slice", "copyFromTexture") &&
             require_numeric_field(nested, "destination_level", "copyFromTexture") &&
             require_numeric_array(nested, "destination_origin", "copyFromTexture") &&
             require_metal_object(texture_ids, object_id_field(payload, "source_texture_id"), error_prefix,
                 "copyFromTexture references an unknown source texture", error) &&
             require_metal_object(texture_ids, object_id_field(payload, "destination_texture_id"), error_prefix,
                 "copyFromTexture references an unknown destination texture", error);
    }
  case apitrace::trace::MetalCallKind::BlitFill:
    return require_numeric_field(payload, "range_start", "fillBuffer") &&
           require_positive_numeric_field(payload, "range_length", "fillBuffer") &&
           require_numeric_field(payload, "value", "fillBuffer") &&
           require_metal_object(buffer_ids, object_id_field(payload, "buffer_id"), error_prefix,
        "fillBuffer references an unknown buffer", error);
  default:
    break;
  }
  return true;
}

bool validate_metal_synchronization_payload(
    const std::string &error_prefix,
    apitrace::trace::MetalCallKind kind,
    const json &payload,
    std::string &error)
{
  const auto require_integer_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };

  if (kind == apitrace::trace::MetalCallKind::MemoryBarrier) {
    const auto payload_it = payload.find("payload");
    const auto barrier = payload_it != payload.end() && payload_it->is_object()
                             ? *payload_it
                             : payload;
    return require_integer_field(barrier, "scope", "memoryBarrier") &&
           require_integer_field(barrier, "stages_before", "memoryBarrier") &&
           require_integer_field(barrier, "stages_after", "memoryBarrier");
  }
  if (kind == apitrace::trace::MetalCallKind::UpdateFence ||
      kind == apitrace::trace::MetalCallKind::WaitForFence) {
    const char *description = kind == apitrace::trace::MetalCallKind::UpdateFence ? "updateFence" : "waitForFence";
    return require_integer_field(payload, "fence_id", description) &&
           require_integer_field(payload, "stages", description);
  }
  if (kind == apitrace::trace::MetalCallKind::FenceOps) {
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array() || ops->empty()) {
      error = error_prefix + "fenceOps is missing ops";
      return false;
    }
    for (const auto &op : *ops) {
      if (!op.is_array() || op.size() < 3 || !op[0].is_string() ||
          (!op[1].is_number_unsigned() && !op[1].is_number_integer()) ||
          (!op[2].is_number_unsigned() && !op[2].is_number_integer())) {
        error = error_prefix + "fenceOps has invalid op";
        return false;
      }
      const auto op_name = op[0].get_ref<const std::string &>();
      if (op_name != "update" && op_name != "wait") {
        error = error_prefix + "fenceOps has unsupported op " + op_name;
        return false;
      }
    }
  }
  return true;
}

bool validate_metal_blit_batch_payload(
    const std::string &error_prefix,
    const json &payload,
    const std::unordered_set<std::uint64_t> &buffer_ids,
    const std::unordered_set<std::uint64_t> &texture_ids,
    std::string &error)
{
  const auto ops = payload.find("ops");
  if (ops == payload.end() || !ops->is_array() || ops->empty()) {
    error = error_prefix + "blitBatch is missing ops";
    return false;
  }

  for (const auto &op : *ops) {
    if (!op.is_object()) {
      error = error_prefix + "blitBatch op must be an object";
      return false;
    }
    const auto kind = string_from_json(op.value("op", json(nullptr)));
    if (kind == "copy_texture") {
      if (!validate_metal_blit_payload_resources(
              error_prefix, apitrace::trace::MetalCallKind::CopyTexture, op, buffer_ids, texture_ids, error)) {
        return false;
      }
    } else if (kind == "fill_buffer") {
      if (!validate_metal_blit_payload_resources(
              error_prefix, apitrace::trace::MetalCallKind::BlitFill, op, buffer_ids, texture_ids, error)) {
        return false;
      }
    } else if (kind == "wait_fence" || kind == "update_fence") {
      if (object_id_field(op, "fence_id") == 0) {
        error = error_prefix + kind + " blitBatch op is missing fence_id";
        return false;
      }
      const auto stages = op.find("stages");
      if (stages == op.end() || (!stages->is_number_unsigned() && !stages->is_number_integer())) {
        error = error_prefix + kind + " blitBatch op is missing stages";
        return false;
      }
    } else {
      error = error_prefix + "unsupported blitBatch op " + (kind.empty() ? std::string("<missing>") : kind);
      return false;
    }
  }
  return true;
}

bool validate_metal_work_payload(
    const std::string &error_prefix,
    apitrace::trace::MetalCallKind kind,
    const json &payload,
    std::string &error)
{
  const auto require_numeric_field = [&](const char *field, const char *description) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
      error = error_prefix + description + " is missing " + field;
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
      error = error_prefix + description + " has invalid primitive_type";
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
      error = error_prefix + description + " has invalid index_type";
      return false;
    }
    return true;
  };

  switch (kind) {
  case apitrace::trace::MetalCallKind::DrawPrimitives:
    return require_primitive_type("drawPrimitives") &&
           require_numeric_field("vertex_start", "drawPrimitives") &&
           require_numeric_field("vertex_count", "drawPrimitives") &&
           require_numeric_field("instance_count", "drawPrimitives") &&
           require_numeric_field("base_instance", "drawPrimitives");
  case apitrace::trace::MetalCallKind::DrawIndexedPrimitives:
    return require_primitive_type("drawIndexedPrimitives") &&
           require_numeric_field("index_count", "drawIndexedPrimitives") &&
           require_index_type("drawIndexedPrimitives") &&
           require_numeric_field("index_buffer_offset", "drawIndexedPrimitives") &&
           require_numeric_field("instance_count", "drawIndexedPrimitives") &&
           require_numeric_field("base_vertex", "drawIndexedPrimitives") &&
           require_numeric_field("base_instance", "drawIndexedPrimitives");
  case apitrace::trace::MetalCallKind::DrawPrimitivesIndirect:
    return require_primitive_type("drawPrimitivesIndirect") &&
           require_numeric_field("indirect_buffer_offset", "drawPrimitivesIndirect");
  case apitrace::trace::MetalCallKind::DrawIndexedPrimitivesIndirect:
    return require_primitive_type("drawIndexedPrimitivesIndirect") &&
           require_index_type("drawIndexedPrimitivesIndirect") &&
           require_numeric_field("index_buffer_offset", "drawIndexedPrimitivesIndirect") &&
           require_numeric_field("indirect_buffer_offset", "drawIndexedPrimitivesIndirect");
  case apitrace::trace::MetalCallKind::DispatchThreadgroups:
    return require_numeric_field("tgx", "dispatchThreadgroups") &&
           require_numeric_field("tgy", "dispatchThreadgroups") &&
           require_numeric_field("tgz", "dispatchThreadgroups") &&
           require_numeric_field("tx", "dispatchThreadgroups") &&
           require_numeric_field("ty", "dispatchThreadgroups") &&
           require_numeric_field("tz", "dispatchThreadgroups");
  case apitrace::trace::MetalCallKind::DispatchThreadgroupsIndirect:
    return require_numeric_field("indirect_buffer_offset", "dispatchThreadgroupsIndirect") &&
           require_numeric_field("tx", "dispatchThreadgroupsIndirect") &&
           require_numeric_field("ty", "dispatchThreadgroupsIndirect") &&
           require_numeric_field("tz", "dispatchThreadgroupsIndirect");
  case apitrace::trace::MetalCallKind::DispatchThreads:
    return require_numeric_field("width", "dispatchThreads") &&
           require_numeric_field("height", "dispatchThreads") &&
           require_numeric_field("depth", "dispatchThreads") &&
           require_numeric_field("threads_per_group_width", "dispatchThreads") &&
           require_numeric_field("threads_per_group_height", "dispatchThreads") &&
           require_numeric_field("threads_per_group_depth", "dispatchThreads");
  default:
    return true;
  }
}

bool validate_metal_render_state_payload(
    const std::string &error_prefix,
    apitrace::trace::MetalCallKind kind,
    const json &payload,
    std::string &error)
{
  const auto require_numeric_field = [&](const json &object, const char *field, const char *description) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_number()) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_numeric_array = [&](const json &object, const char *field, const char *description, std::size_t width) -> bool {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_array() || it->empty()) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    for (const auto &entry : *it) {
      if (!entry.is_array() || entry.size() != width) {
        error = error_prefix + description + " has invalid " + field;
        return false;
      }
      for (std::size_t index = 0; index < width; ++index) {
        if (!entry[index].is_number()) {
          error = error_prefix + description + " has invalid " + field;
          return false;
        }
      }
    }
    return true;
  };

  if (kind == apitrace::trace::MetalCallKind::SetCullMode) {
    return require_numeric_field(payload, "cull_mode", "setCullMode");
  }
  if (kind == apitrace::trace::MetalCallKind::SetFrontFacingWinding) {
    return require_numeric_field(payload, "winding", "setFrontFacingWinding");
  }
  if (kind == apitrace::trace::MetalCallKind::SetTriangleFillMode) {
    return require_numeric_field(payload, "fill_mode", "setTriangleFillMode");
  }
  if (kind == apitrace::trace::MetalCallKind::SetViewport) {
    return require_numeric_array(nested_json(payload, "payload"), "viewports", "setViewport", 6);
  }
  if (kind == apitrace::trace::MetalCallKind::SetScissorRect) {
    return require_numeric_array(nested_json(payload, "payload"), "rects", "setScissorRect", 4);
  }
  return true;
}

bool validate_dxmt_encoder_state_payload(
    const std::string &error_prefix,
    const std::string &kind,
    const json &payload,
    std::string &error)
{
  const auto require_numeric_field = [&](const char *field, const std::string &description) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || !it->is_number()) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    return true;
  };
  const auto require_numeric_array = [&](const char *field, const std::string &description, std::size_t width) -> bool {
    const auto it = payload.find(field);
    if (it == payload.end() || !it->is_array() || it->empty()) {
      error = error_prefix + description + " is missing " + field;
      return false;
    }
    for (const auto &entry : *it) {
      if (!entry.is_array() || entry.size() != width) {
        error = error_prefix + description + " has invalid " + field;
        return false;
      }
      for (std::size_t index = 0; index < width; ++index) {
        if (!entry[index].is_number()) {
          error = error_prefix + description + " has invalid " + field;
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

bool verify_d3d_pipeline_payload(
    const apitrace::trace::TraceBundleReader &reader,
    const std::string &label,
    const json &payload,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    const std::unordered_map<std::uint64_t, std::string> &blob_paths,
    const std::unordered_map<std::uint64_t, std::string> &root_signature_paths_by_object,
    AssetJsonCache &asset_json_cache,
    std::unordered_set<std::string> &d3d_pipeline_paths,
    std::unordered_set<std::string> &d3d_shader_paths,
    std::unordered_set<std::string> &d3d_root_signature_paths,
    std::string &error)
{
  const auto pipeline_path_it = payload.find("pipeline_path");
  if (pipeline_path_it == payload.end()) {
    return true;
  }
  const auto pipeline_path = string_from_json(*pipeline_path_it);
  if (pipeline_path.empty()) {
    error = label + ": pipeline_path must be a non-empty string";
    return false;
  }
  const std::filesystem::path pipeline_relative_path(pipeline_path);
  if (!is_d3d_pipeline_asset_path(pipeline_relative_path)) {
    error = label + ": pipeline_path does not reference a D3D pipeline asset";
    return false;
  }

  json pipeline;
  if (!read_asset_json(reader, pipeline_path, asset_json_cache, pipeline, error)) {
    error = label + ": " + error;
    return false;
  }
  if (!pipeline.is_object()) {
    error = label + ": pipeline asset JSON must be an object";
    return false;
  }

  d3d_pipeline_paths.insert(pipeline_path);

  const auto type = string_from_json(pipeline.value("type", json(nullptr)));
  const bool requires_dxmt_backend = payload.value("requires_dxmt_backend", false) ||
                                     type == "mesh" ||
                                     pipeline.contains("as") ||
                                     pipeline.contains("ms");
  const bool compute_pipeline = type == "compute";
  const bool graphics_pipeline = !compute_pipeline;

  const auto root_signature_object_id = object_id_from_pipeline_json(pipeline, "root_signature_object_id");
  if (root_signature_object_id == 0) {
    error = label + ": pipeline asset missing root_signature_object_id";
    return false;
  }
  const auto root_signature_path_it = root_signature_paths_by_object.find(root_signature_object_id);
  if (root_signature_path_it == root_signature_paths_by_object.end()) {
    error = label + ": pipeline asset references an unknown root signature object id";
    return false;
  }
  const std::filesystem::path root_signature_relative_path(root_signature_path_it->second);
  if (!is_d3d_root_signature_asset_path(root_signature_relative_path)) {
    error = label + ": pipeline root signature object does not reference a D3D root signature asset";
    return false;
  }

  std::unordered_set<std::string> nested_shader_paths;
  for (const auto *stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
    if (!verify_pipeline_shader_metadata(label, pipeline, stage, nested_shader_paths, error)) {
      return false;
    }
  }

  if (compute_pipeline && nested_shader_paths.empty()) {
    error = label + ": compute pipeline asset has no shader bytecode";
    return false;
  }
  if (compute_pipeline && !pipeline.contains("cs")) {
    error = label + ": compute pipeline asset missing cs shader metadata";
    return false;
  }
  if (graphics_pipeline && !requires_dxmt_backend && !pipeline.contains("vs")) {
    error = label + ": graphics pipeline asset missing vs shader metadata";
    return false;
  }
  if (graphics_pipeline) {
    for (const auto *key : {
             "input_layout",
             "blend_state",
             "rasterizer_state",
             "depth_stencil_state",
             "sample_desc",
         }) {
      const auto it = pipeline.find(key);
      if (it == pipeline.end() || !it->is_object()) {
        error = label + ": graphics pipeline asset missing object metadata: " + key;
        return false;
      }
    }
    const auto rtv_formats = pipeline.find("rtv_formats");
    if (rtv_formats == pipeline.end() || !rtv_formats->is_array()) {
      error = label + ": graphics pipeline asset missing rtv_formats";
      return false;
    }
    const auto num_render_targets = object_id_from_pipeline_json(pipeline, "num_render_targets");
    if (rtv_formats->size() < num_render_targets) {
      error = label + ": graphics pipeline rtv_formats shorter than num_render_targets";
      return false;
    }
    if (!requires_dxmt_backend && !verify_d3d_graphics_pipeline_metadata(label, pipeline, error)) {
      return false;
    }
  }

  std::unordered_set<std::string> event_blob_paths;
  event_blob_paths.reserve(blob_refs.size());
  for (const auto blob_id : blob_refs) {
    const auto blob_path_it = blob_paths.find(blob_id);
    if (blob_path_it != blob_paths.end()) {
      event_blob_paths.insert(blob_path_it->second);
    }
  }
  if (event_blob_paths.find(pipeline_path) == event_blob_paths.end()) {
    error = label + ": pipeline_path is not covered by blob_refs";
    return false;
  }
  for (const auto &shader_path : nested_shader_paths) {
    if (event_blob_paths.find(shader_path) == event_blob_paths.end()) {
      error = label + ": nested pipeline shader path is not covered by blob_refs: " + shader_path;
      return false;
    }
    d3d_shader_paths.insert(shader_path);
  }

  const auto root_signature_refs = payload.find("root_signature_path");
  if (root_signature_refs != payload.end() && root_signature_refs->is_string()) {
    d3d_root_signature_paths.insert(root_signature_refs->get<std::string>());
  }
  d3d_root_signature_paths.insert(root_signature_path_it->second);

  return true;
}

bool collect_asset_paths_with_nested_json(
    const apitrace::trace::TraceBundleReader &reader,
    const json &payload,
    AssetJsonCache &asset_json_cache,
    std::vector<std::string> &paths,
    std::string &error)
{
  collect_asset_paths(payload, paths);
  const auto direct_path_count = paths.size();
  for (std::size_t index = 0; index < direct_path_count; ++index) {
    if (!ends_with(paths[index], ".json")) {
      continue;
    }
    json asset_json;
    if (!read_asset_json(reader, paths[index], asset_json_cache, asset_json, error)) {
      return false;
    }
    collect_nested_asset_paths(asset_json, paths);
  }
  return true;
}

bool bind_blob_paths(
    std::string_view label,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    const std::vector<std::string> &paths,
    const std::unordered_map<std::uint64_t, std::string> &blob_paths,
    std::unordered_map<std::uint64_t, std::string> &observed_blob_paths,
    std::string &error)
{
  std::unordered_set<std::string> path_set(paths.begin(), paths.end());
  for (const auto blob_id : blob_refs) {
    if (blob_id == 0) {
      continue;
    }
    const auto blob_path_it = blob_paths.find(blob_id);
    if (blob_path_it == blob_paths.end()) {
      continue;
    }
    if (path_set.find(blob_path_it->second) == path_set.end()) {
      continue;
    }
    const auto [it, inserted] = observed_blob_paths.emplace(blob_id, blob_path_it->second);
    if (!inserted && it->second != blob_path_it->second) {
      error = std::string(label) + ": blob_ref " + std::to_string(blob_id) +
              " is bound to both " + it->second + " and " + blob_path_it->second;
      return false;
    }
  }
  return true;
}

bool verify_blob_refs_cover_asset_paths(
    std::string_view label,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    const std::vector<std::string> &paths,
    const std::unordered_map<std::uint64_t, std::string> &blob_paths,
    std::string &error)
{
  if (blob_refs.size() < paths.size()) {
    error = std::string(label) + ": asset path references are missing blob_refs";
    return false;
  }
  std::unordered_set<std::string> ref_paths;
  ref_paths.reserve(blob_refs.size());
  for (const auto blob_id : blob_refs) {
    if (blob_id == 0) {
      error = std::string(label) + ": asset path has zero blob_ref";
      return false;
    }
    const auto blob_path_it = blob_paths.find(blob_id);
    if (blob_path_it == blob_paths.end()) {
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

std::optional<std::uint64_t> object_id_from_payload_key(const json &payload, const char *key)
{
  const auto it = payload.find(key);
  if (it == payload.end() || it->is_null()) {
    return std::nullopt;
  }
  if (!it->is_number_unsigned() && !it->is_number_integer()) {
    return std::nullopt;
  }
  return it->get<std::uint64_t>();
}

struct PresentFrameMetadata {
  std::uint64_t frame_index = 0;
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t sync_interval = 0;
  std::uint64_t flags = 0;
  std::uint64_t sequence = 0;
  bool has_call = false;
  bool has_boundary = false;
};

bool present_frame_metadata_matches(const PresentFrameMetadata &expected, const PresentFrameMetadata &actual)
{
  return expected.frame_index == actual.frame_index &&
         expected.width == actual.width &&
         expected.height == actual.height &&
         expected.sync_interval == actual.sync_interval &&
         expected.flags == actual.flags;
}

bool present_parameters_match(const PresentFrameMetadata &expected, const PresentFrameMetadata &actual)
{
  return expected.frame_index == actual.frame_index &&
         expected.sync_interval == actual.sync_interval &&
         expected.flags == actual.flags;
}

bool is_present_frame_debug_name(std::string_view debug_name)
{
  return debug_name == "D3D11PresentFrame" ||
         debug_name == "D3D12PresentFrame" ||
         debug_name == "MetalPresentFrame";
}

bool is_supported_present_frame_format(std::string_view debug_name, std::string_view format)
{
  if (format == "rgba8unorm") {
    format = "rgba8";
  } else if (format == "bgra8unorm") {
    format = "bgra8";
  }

  if (debug_name == "D3D11PresentFrame" || debug_name == "D3D12PresentFrame") {
    return format == "rgba8";
  }
  if (debug_name == "MetalPresentFrame") {
    return format == "rgba8" || format == "bgra8";
  }
  return false;
}

std::string api_name_for_present_frame_debug_name(std::string_view debug_name)
{
  if (debug_name == "D3D11PresentFrame") {
    return "D3D11";
  }
  if (debug_name == "D3D12PresentFrame") {
    return "D3D12";
  }
  if (debug_name == "MetalPresentFrame") {
    return "Metal";
  }
  return std::string(debug_name);
}

std::string present_frame_debug_name_for_present_call(std::string_view function_name)
{
  if (function_name == "IDXGISwapChain::Present" ||
      function_name == "IDXGISwapChain1::Present" ||
      function_name == "IDXGISwapChain1::Present1") {
    return "DXGIPresentFrame";
  }
  return {};
}

PresentFrameMetadata present_frame_metadata_from_payload(const json &payload, std::uint64_t sequence)
{
  PresentFrameMetadata metadata;
  metadata.frame_index = payload.value("frame_index", 0ull);
  metadata.width = payload.value("width", 0ull);
  metadata.height = payload.value("height", 0ull);
  metadata.sync_interval = payload.value("sync_interval", 0ull);
  metadata.flags = payload.value("flags", 0ull);
  metadata.sequence = sequence;
  return metadata;
}

bool payload_present_metadata(
    const json &payload,
    std::uint64_t sequence,
    PresentFrameMetadata &metadata,
    std::string &missing_key)
{
  for (const auto *key : {"frame_index", "sync_interval", "flags"}) {
    if (!payload.contains(key)) {
      missing_key = key;
      return false;
    }
  }
  metadata = present_frame_metadata_from_payload(payload, sequence);
  return true;
}

void collect_resolved_resource_object_ids(const json &payload, std::vector<std::uint64_t> &object_ids)
{
  if (payload.is_object()) {
    for (const auto &[key, child] : payload.items()) {
      if (key == "resolved_resource_object_id" && (child.is_number_unsigned() || child.is_number_integer())) {
        object_ids.push_back(child.get<std::uint64_t>());
      }
      collect_resolved_resource_object_ids(child, object_ids);
    }
    return;
  }

  if (payload.is_array()) {
    for (const auto &child : payload) {
      collect_resolved_resource_object_ids(child, object_ids);
    }
  }
}

bool object_refs_contain(const std::vector<apitrace::trace::ObjectId> &object_refs, std::uint64_t object_id)
{
  return std::find(object_refs.begin(), object_refs.end(), object_id) != object_refs.end();
}

bool parse_payload(const std::string &payload_text, json &payload, std::string &error)
{
  payload = json::parse(payload_text.empty() ? std::string("{}") : payload_text, nullptr, false);
  if (payload.is_discarded()) {
    error = "invalid event payload JSON";
    return false;
  }
  return true;
}

bool verify_asset_path(
    const apitrace::trace::TraceBundleReader &reader,
    const std::unordered_map<std::string, const apitrace::trace::ChecksumRecord *> &checksum_records,
    const std::string &relative_path_text,
    std::unordered_set<std::string> &existing_paths,
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
  const auto checksum = checksum_records.find(key);
  if (checksum == checksum_records.end()) {
    error = "asset path missing from checksums: " + key;
    return false;
  }
  if (existing_paths.find(key) == existing_paths.end()) {
    if (reader.validated_checksum_paths().find(key) == reader.validated_checksum_paths().end() &&
        !std::filesystem::is_regular_file(reader.layout().root_path / relative_path)) {
      error = "asset path missing file: " + key;
      return false;
    }
    existing_paths.insert(key);
  }
  return true;
}

bool verify_asset_index_record(
    const apitrace::trace::TraceBundleReader &reader,
    const std::unordered_map<std::string, const apitrace::trace::ChecksumRecord *> &checksum_records,
    const apitrace::trace::AssetRecord &asset,
    std::unordered_map<std::string, std::uint64_t> &file_sizes,
    std::unordered_set<std::string> &existing_paths,
    std::string &error)
{
  if (asset.relative_path.empty()) {
    return true;
  }
  if (!verify_asset_path(reader, checksum_records, asset.relative_path.generic_string(), existing_paths, error)) {
    return false;
  }

  const auto key = asset.relative_path.generic_string();
  const auto checksum = checksum_records.find(key);
  if (checksum == checksum_records.end()) {
    error = "asset path missing from checksums: " + key;
    return false;
  }
  if (!asset.content_hash.empty()) {
    if (checksum->second->algorithm != "sha256") {
      error = "asset index content_hash requires sha256 checksum: " + key;
      return false;
    }
    if (checksum->second->digest != asset.content_hash) {
      error = "asset index content_hash does not match checksum: " + key;
      return false;
    }
  }
  if (asset.byte_size != 0) {
    std::uint64_t actual_size = 0;
    if (const auto known_size = file_sizes.find(key); known_size != file_sizes.end()) {
      actual_size = known_size->second;
    } else {
      std::error_code stat_error;
      actual_size = std::filesystem::file_size(reader.layout().root_path / asset.relative_path, stat_error);
      if (stat_error) {
        error = "failed to stat asset index path: " + key;
        return false;
      }
      file_sizes.emplace(key, actual_size);
    }
    if (actual_size != asset.byte_size) {
      error = "asset index byte_size does not match file size: " + key;
      return false;
    }
  }
  return true;
}

bool add_blob_path(
    std::unordered_map<std::uint64_t, std::string> &blob_paths,
    std::uint64_t blob_id,
    const std::filesystem::path &relative_path,
    std::string_view label,
    std::string &error)
{
  if (blob_id == 0 || relative_path.empty()) {
    return true;
  }

  const auto path = relative_path.generic_string();
  const auto [it, inserted] = blob_paths.emplace(blob_id, path);
  if (!inserted && it->second != path) {
    std::ostringstream message;
    message << label << ": blob_id " << blob_id
            << " maps to multiple asset paths: " << it->second
            << " and " << path;
    error = message.str();
    return false;
  }
  return true;
}

bool verify_present_frame_payload(
    const apitrace::trace::TraceBundleReader &reader,
    const std::unordered_map<std::uint64_t, std::string> &blob_paths,
    std::string_view label,
    const json &payload,
    const std::vector<apitrace::trace::BlobId> &blob_refs,
    std::string &error)
{
  const auto frame_path_it = payload.find("frame_path");
  if (frame_path_it == payload.end()) {
    return true;
  }
  if (!frame_path_it->is_string()) {
    error = std::string(label) + ": frame_path must be a string";
    return false;
  }
  const auto debug_name = payload.value("__debug_name", std::string());
  if (debug_name != "D3D11PresentFrame" &&
      debug_name != "D3D12PresentFrame" &&
      debug_name != "MetalPresentFrame") {
    error = std::string(label) + ": frame_path is only allowed on PresentFrame resource blobs";
    return false;
  }
  const auto format_it = payload.find("format");
  if (format_it == payload.end() || !format_it->is_string() ||
      !is_supported_present_frame_format(debug_name, format_it->get<std::string>())) {
    error = std::string(label) + ": unsupported PresentFrame format";
    return false;
  }
  const auto width = payload.value("width", 0ull);
  const auto height = payload.value("height", 0ull);
  const auto row_pitch = payload.value("row_pitch", 0ull);
  if (width == 0 || height == 0 || row_pitch < width * 4ull) {
    error = std::string(label) + ": PresentFrame dimensions are invalid";
    return false;
  }
  const std::filesystem::path relative_path = frame_path_it->get<std::string>();
  if (!path_is_safe(relative_path)) {
    error = std::string(label) + ": unsafe PresentFrame path: " + relative_path.generic_string();
    return false;
  }
  if (relative_path.empty() || relative_path.begin() == relative_path.end() ||
      *relative_path.begin() != "textures") {
    error = std::string(label) + ": PresentFrame asset must live under textures/";
    return false;
  }
  const auto frame_path_key = relative_path.generic_string();
  if (blob_refs.size() != 1 || blob_refs.front() == 0) {
    error = std::string(label) + ": PresentFrame must have exactly one blob_ref";
    return false;
  }
  const auto blob_path_it = blob_paths.find(blob_refs.front());
  if (blob_path_it == blob_paths.end()) {
    error = std::string(label) + ": PresentFrame blob_ref does not resolve to an asset";
    return false;
  }
  if (blob_path_it->second != frame_path_key) {
    error = std::string(label) + ": PresentFrame blob_ref does not match frame_path";
    return false;
  }
  const auto absolute_path = reader.layout().root_path / relative_path;
  std::error_code stat_error;
  const auto actual_size = std::filesystem::file_size(absolute_path, stat_error);
  if (stat_error) {
    error = std::string(label) + ": failed to stat PresentFrame asset: " + relative_path.generic_string();
    return false;
  }
  const auto expected_size = static_cast<std::uintmax_t>(row_pitch) * static_cast<std::uintmax_t>(height);
  if (actual_size != expected_size) {
    error = std::string(label) + ": PresentFrame asset size does not match row_pitch * height";
    return false;
  }
  return true;
}

bool metal_event_creates_object(apitrace::trace::MetalCallKind kind)
{
  switch (kind) {
  case apitrace::trace::MetalCallKind::DeviceCreate:
  case apitrace::trace::MetalCallKind::CommandQueueCreate:
  case apitrace::trace::MetalCallKind::CommandBufferBegin:
  case apitrace::trace::MetalCallKind::RenderEncoderBegin:
  case apitrace::trace::MetalCallKind::ComputeEncoderBegin:
  case apitrace::trace::MetalCallKind::BlitEncoderBegin:
  case apitrace::trace::MetalCallKind::ObjectMetadata:
    return true;
  default:
    return false;
  }
}

bool verify_bundle_references(
    const apitrace::trace::TraceBundleReader &reader,
    std::size_t &d3d_object_refs,
    std::size_t &metal_object_refs,
    std::size_t &blob_refs,
    std::size_t &asset_path_refs,
    PresentFrameStats &present_stats,
    std::string &error)
{
  std::unordered_set<std::uint64_t> known_objects;
  known_objects.reserve(reader.objects().size() + reader.events().size());
  for (const auto &object : reader.objects()) {
    if (object.object_id != 0) {
      known_objects.insert(object.object_id);
    }
  }

  std::unordered_map<std::uint64_t, std::string> blob_paths;
  for (const auto &asset : reader.assets()) {
    if (!add_blob_path(blob_paths, asset.blob_id, asset.relative_path, "reader asset index", error)) {
      return false;
    }
  }
  for (const auto &asset : reader.metal_assets()) {
    if (!add_blob_path(blob_paths, asset.blob_id, asset.relative_path, "reader Metal asset index", error)) {
      return false;
    }
  }

  std::unordered_map<std::string, const apitrace::trace::ChecksumRecord *> checksum_records;
  checksum_records.reserve(reader.checksums().files.size());
  for (const auto &record : reader.checksums().files) {
    if (!path_is_safe(record.relative_path)) {
      error = "unsafe checksum path: " + record.relative_path.generic_string();
      return false;
    }
    checksum_records[record.relative_path.generic_string()] = &record;
  }

  std::unordered_map<std::string, std::uint64_t> asset_file_sizes;
  std::unordered_set<std::string> existing_asset_paths;
  for (const auto &asset : reader.assets()) {
    if (!asset.relative_path.empty()) {
      ++asset_path_refs;
      if (!verify_asset_index_record(reader, checksum_records, asset, asset_file_sizes, existing_asset_paths, error)) {
        error = "reader asset index: " + error;
        return false;
      }
    }
  }
  for (const auto &asset : reader.metal_assets()) {
    if (!asset.relative_path.empty()) {
      ++asset_path_refs;
      if (!verify_asset_index_record(reader, checksum_records, asset, asset_file_sizes, existing_asset_paths, error)) {
        error = "reader Metal asset index: " + error;
        return false;
      }
    }
  }

  std::unordered_map<std::uint64_t, PresentFrameMetadata> metal_present_frames;
  std::unordered_map<std::string, PresentFrameMetadata> previous_present_frames_by_kind;
  std::set<std::string> d3d_present_frame_kinds;
  std::unordered_map<std::uint64_t, PresentFrameMetadata> d3d_present_calls;
  std::unordered_map<std::uint64_t, PresentFrameMetadata> d3d_present_boundaries;
  std::unordered_map<std::string, std::unordered_map<std::uint64_t, PresentFrameMetadata>> d3d_present_frames_by_kind;
  std::unordered_map<std::uint64_t, std::string> observed_blob_paths;
  AssetJsonCache asset_json_cache;
  for (const auto &event : reader.events()) {
    json payload;
    if (!parse_payload(event.payload, payload, error)) {
      error = "sequence " + std::to_string(event.callsite.sequence) + ": " + error;
      return false;
    }
    payload["__debug_name"] = event.object_debug_name;
    if (!verify_present_frame_payload(
            reader,
            blob_paths,
            "sequence " + std::to_string(event.callsite.sequence),
            payload,
            event.blob_refs,
            error)) {
      return false;
    }
    if (event.kind == apitrace::trace::EventKind::ResourceBlob &&
        is_present_frame_debug_name(event.object_debug_name) &&
        payload.contains("frame_path")) {
      const auto metadata = present_frame_metadata_from_payload(payload, event.callsite.sequence);
      const auto previous_it = previous_present_frames_by_kind.find(event.object_debug_name);
      if (previous_it != previous_present_frames_by_kind.end() &&
          metadata.frame_index <= previous_it->second.frame_index) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": " + event.object_debug_name + " frame_index is not strictly increasing (" +
                std::to_string(metadata.frame_index) + " after " +
                std::to_string(previous_it->second.frame_index) + ")";
        return false;
      }
      previous_present_frames_by_kind[event.object_debug_name] = metadata;
      if (event.object_debug_name == "D3D11PresentFrame" || event.object_debug_name == "D3D12PresentFrame") {
        d3d_present_frame_kinds.insert(event.object_debug_name);
        if (!d3d_present_frames_by_kind[event.object_debug_name].emplace(metadata.frame_index, metadata).second) {
          error = "sequence " + std::to_string(event.callsite.sequence) +
                  ": duplicate " + event.object_debug_name + " frame_index " + std::to_string(metadata.frame_index);
          return false;
        }
      }
      if (event.object_debug_name != "MetalPresentFrame") {
        continue;
      }
      if (!metal_present_frames.emplace(metadata.frame_index, metadata).second) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": duplicate MetalPresentFrame frame_index " + std::to_string(metadata.frame_index);
        return false;
      }
    }
    if (event.kind == apitrace::trace::EventKind::Call &&
        present_frame_debug_name_for_present_call(event.callsite.function_name).empty() == false) {
      PresentFrameMetadata metadata;
      std::string missing_key;
      if (!payload_present_metadata(payload, event.callsite.sequence, metadata, missing_key)) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": captured Present call is missing " + missing_key;
        return false;
      }
      if (metadata.frame_index != d3d_present_calls.size()) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": captured Present call frame_index is not contiguous";
        return false;
      }
      auto &present_call = d3d_present_calls[metadata.frame_index];
      if (present_call.has_call) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": duplicate captured Present call frame_index " + std::to_string(metadata.frame_index);
        return false;
      }
      metadata.has_call = true;
      present_call = metadata;
    }
    if (event.kind == apitrace::trace::EventKind::Boundary &&
        event.boundary == apitrace::trace::BoundaryKind::Present) {
      PresentFrameMetadata metadata;
      std::string missing_key;
      if (!payload_present_metadata(payload, event.callsite.sequence, metadata, missing_key)) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": Present boundary is missing " + missing_key;
        return false;
      }
      if (metadata.frame_index != d3d_present_boundaries.size()) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": Present boundary frame_index is not contiguous";
        return false;
      }
      const auto call_it = d3d_present_calls.find(metadata.frame_index);
      if (call_it == d3d_present_calls.end()) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": Present boundary is missing matching IDXGISwapChain::Present call";
        return false;
      }
      if (!present_parameters_match(call_it->second, metadata)) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": Present boundary does not match captured Present parameters";
        return false;
      }
      auto &present_boundary = d3d_present_boundaries[metadata.frame_index];
      if (present_boundary.has_boundary) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": duplicate Present boundary frame_index " + std::to_string(metadata.frame_index);
        return false;
      }
      metadata.has_boundary = true;
      present_boundary = metadata;
    }

    if ((event.kind == apitrace::trace::EventKind::ObjectCreate ||
         event.kind == apitrace::trace::EventKind::ObjectDestroy) &&
        event.object_id != 0) {
      known_objects.insert(event.object_id);
    }

    for (const auto object_id : event.object_refs) {
      if (object_id == 0) {
        continue;
      }
      ++d3d_object_refs;
      if (known_objects.find(object_id) == known_objects.end()) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": object_refs contains unknown object id " + std::to_string(object_id);
        return false;
      }
    }

    std::vector<std::uint64_t> payload_object_ids;
    apitrace::trace::append_payload_object_refs(payload, payload_object_ids);
    for (const auto object_id : payload_object_ids) {
      if (object_id == 0) {
        continue;
      }
      ++d3d_object_refs;
      if (known_objects.find(object_id) == known_objects.end()) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": payload references unknown object id " + std::to_string(object_id);
        return false;
      }
    }
    if (event.kind == apitrace::trace::EventKind::Call) {
      for (const auto object_id : payload_object_ids) {
        if (object_id == 0) {
          continue;
        }
        if (!object_refs_contain(event.object_refs, object_id)) {
          error = "sequence " + std::to_string(event.callsite.sequence) +
                  ": payload object id is missing from object_refs " +
                  std::to_string(object_id);
          return false;
        }
      }
    }
    std::vector<std::uint64_t> resolved_resource_object_ids;
    collect_resolved_resource_object_ids(payload, resolved_resource_object_ids);
    for (const auto object_id : resolved_resource_object_ids) {
      if (object_id == 0) {
        continue;
      }
      if (!object_refs_contain(event.object_refs, object_id)) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": payload resolved_resource_object_id is missing from object_refs " +
                std::to_string(object_id);
        return false;
      }
    }

    const auto event_label = "sequence " + std::to_string(event.callsite.sequence);
    std::vector<std::string> paths;
    if (!collect_asset_paths_with_nested_json(reader, payload, asset_json_cache, paths, error)) {
      error = event_label + ": " + error;
      return false;
    }
    if (event.kind == apitrace::trace::EventKind::ResourceBlob && paths.size() != event.blob_refs.size()) {
      error = event_label + ": resource blob asset path count does not match blob_refs";
      return false;
    }
    if (!verify_blob_refs_cover_asset_paths(event_label, event.blob_refs, paths, blob_paths, error)) {
      return false;
    }
    if (!bind_blob_paths(event_label, event.blob_refs, paths, blob_paths, observed_blob_paths, error)) {
      return false;
    }
    for (const auto &path : paths) {
      ++asset_path_refs;
      if (!verify_asset_path(reader, checksum_records, path, existing_asset_paths, error)) {
        error = event_label + ": " + error;
        return false;
      }
    }
    for (const auto blob_id : event.blob_refs) {
      if (blob_id == 0) {
        continue;
      }
      ++blob_refs;
      if (blob_paths.find(blob_id) == blob_paths.end()) {
        error = "sequence " + std::to_string(event.callsite.sequence) +
                ": blob_refs contains unknown blob id " + std::to_string(blob_id);
        return false;
      }
    }
  }

  if (d3d_present_boundaries.size() != d3d_present_calls.size()) {
    error = "D3D Present boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (d3d_present_frame_kinds.size() > 1) {
    error = "bundle contains multiple D3D PresentFrame debug kinds";
    return false;
  }
  if (!d3d_present_frame_kinds.empty()) {
    const auto &debug_name = *d3d_present_frame_kinds.begin();
    const auto &frames = d3d_present_frames_by_kind[debug_name];
    if (d3d_present_calls.empty()) {
      error = api_name_for_present_frame_debug_name(debug_name) +
              " present frames exist without captured IDXGISwapChain::Present calls";
      return false;
    }
    if (frames.size() != d3d_present_calls.size()) {
      error = api_name_for_present_frame_debug_name(debug_name) +
              " present frame asset count does not match captured Present calls";
      return false;
    }
    if (d3d_present_boundaries.size() != d3d_present_calls.size()) {
      error = api_name_for_present_frame_debug_name(debug_name) +
              " Present boundary count does not match captured Present calls";
      return false;
    }
    for (const auto &[frame_index, frame] : frames) {
      const auto call_it = d3d_present_calls.find(frame_index);
      if (call_it == d3d_present_calls.end()) {
        error = "sequence " + std::to_string(frame.sequence) + ": " + debug_name +
                " has no matching IDXGISwapChain::Present call frame_index " + std::to_string(frame_index);
        return false;
      }
      const auto boundary_it = d3d_present_boundaries.find(frame_index);
      if (boundary_it == d3d_present_boundaries.end()) {
        error = "sequence " + std::to_string(frame.sequence) + ": " + debug_name +
                " has no matching Present boundary frame_index " + std::to_string(frame_index);
        return false;
      }
      if (!present_parameters_match(call_it->second, frame) ||
          !present_parameters_match(boundary_it->second, frame)) {
        error = "sequence " + std::to_string(frame.sequence) + ": " + debug_name +
                " metadata does not match captured Present parameters for frame_index " +
                std::to_string(frame_index);
        return false;
      }
    }
  }

  std::unordered_map<std::uint64_t, PresentFrameMetadata> metal_present_events;
  for (const auto &event : reader.metal_events()) {
    json payload;
    if (!parse_payload(event.payload, payload, error)) {
      error = "metal sequence " + std::to_string(event.metal_sequence) + ": " + error;
      return false;
    }
    if (event.call_kind == apitrace::trace::MetalCallKind::PresentDrawable) {
      const auto metadata = present_frame_metadata_from_payload(payload, event.metal_sequence);
      if (!metal_present_events.emplace(metadata.frame_index, metadata).second) {
        error = "metal sequence " + std::to_string(event.metal_sequence) +
                ": duplicate PresentDrawable frame_index " + std::to_string(metadata.frame_index);
        return false;
      }
    }

    if (event.object_id != 0) {
      if (metal_event_creates_object(event.call_kind)) {
        known_objects.insert(event.object_id);
      } else if (known_objects.find(event.object_id) == known_objects.end()) {
        error = "metal sequence " + std::to_string(event.metal_sequence) +
                ": object_id references unknown object id " + std::to_string(event.object_id);
        return false;
      }
    }
    for (const auto object_id : event.object_refs) {
      if (object_id == 0) {
        continue;
      }
      ++metal_object_refs;
      if (known_objects.find(object_id) == known_objects.end()) {
        error = "metal sequence " + std::to_string(event.metal_sequence) +
                ": object_refs contains unknown object id " + std::to_string(object_id);
        return false;
      }
    }

    std::vector<std::uint64_t> payload_object_ids;
    apitrace::trace::append_payload_object_refs(payload, payload_object_ids);
    for (const auto object_id : payload_object_ids) {
      if (object_id == 0) {
        continue;
      }
      ++metal_object_refs;
      if (known_objects.find(object_id) == known_objects.end()) {
        error = "metal sequence " + std::to_string(event.metal_sequence) +
                ": payload references unknown object id " + std::to_string(object_id);
        return false;
      }
    }
    for (const auto object_id : payload_object_ids) {
      if (object_id == 0) {
        continue;
      }
      if (event.object_id != object_id && !object_refs_contain(event.object_refs, object_id)) {
        error = "metal sequence " + std::to_string(event.metal_sequence) +
                ": payload object id is missing from object_refs " +
                std::to_string(object_id);
        return false;
      }
    }

    const auto event_label = "metal sequence " + std::to_string(event.metal_sequence);
    std::vector<std::string> paths;
    if (!collect_asset_paths_with_nested_json(reader, payload, asset_json_cache, paths, error)) {
      error = event_label + ": " + error;
      return false;
    }
    if (!verify_blob_refs_cover_asset_paths(event_label, event.blob_refs, paths, blob_paths, error)) {
      return false;
    }
    if (!bind_blob_paths(event_label, event.blob_refs, paths, blob_paths, observed_blob_paths, error)) {
      return false;
    }
    for (const auto &path : paths) {
      ++asset_path_refs;
      if (!verify_asset_path(reader, checksum_records, path, existing_asset_paths, error)) {
        error = event_label + ": " + error;
        return false;
      }
    }
    for (const auto blob_id : event.blob_refs) {
      if (blob_id == 0) {
        continue;
      }
      ++blob_refs;
      if (blob_paths.find(blob_id) == blob_paths.end()) {
        error = "metal sequence " + std::to_string(event.metal_sequence) +
                ": blob_refs contains unknown blob id " + std::to_string(blob_id);
        return false;
      }
    }
  }

  for (const auto &[frame_index, frame] : metal_present_frames) {
    const auto present_it = metal_present_events.find(frame_index);
    if (present_it == metal_present_events.end()) {
      error = "sequence " + std::to_string(frame.sequence) +
              ": MetalPresentFrame has no matching PresentDrawable frame_index " +
              std::to_string(frame_index);
      return false;
    }
    if (!present_frame_metadata_matches(present_it->second, frame)) {
      error = "sequence " + std::to_string(frame.sequence) +
              ": MetalPresentFrame metadata does not match PresentDrawable frame_index " +
              std::to_string(frame_index);
      return false;
    }
  }

  present_stats.d3d_present_calls = d3d_present_calls.size();
  present_stats.d3d_present_boundaries = d3d_present_boundaries.size();
  if (!d3d_present_frame_kinds.empty()) {
    const auto &debug_name = *d3d_present_frame_kinds.begin();
    present_stats.d3d_present_frame_assets = d3d_present_frames_by_kind[debug_name].size();
  }
  present_stats.metal_present_drawables = metal_present_events.size();
  present_stats.metal_present_frame_assets = metal_present_frames.size();

  return true;
}

bool verify_sequences(
    const apitrace::trace::TraceBundleReader &reader,
    std::string &error)
{
  std::unordered_set<std::uint64_t> d3d_api_sequences;
  d3d_api_sequences.reserve(reader.events().size());
  std::uint64_t previous_d3d_api_sequence = 0;
  for (const auto &event : reader.events()) {
    const auto sequence = event.callsite.sequence;
    if (sequence == 0) {
      error = "D3D event has zero sequence";
      return false;
    }

    const bool api_timeline_record =
        event.kind == apitrace::trace::EventKind::Call ||
        event.kind == apitrace::trace::EventKind::Boundary ||
        event.kind == apitrace::trace::EventKind::ResourceBlob;
    if (api_timeline_record) {
      if (sequence <= previous_d3d_api_sequence) {
        std::ostringstream message;
        message << "D3D API sequence is not strictly increasing: " << sequence
                << " after " << previous_d3d_api_sequence;
        error = message.str();
        return false;
      }
      if (!d3d_api_sequences.insert(sequence).second) {
        error = "duplicate D3D API sequence: " + std::to_string(sequence);
        return false;
      }
      previous_d3d_api_sequence = sequence;
    } else if (sequence < previous_d3d_api_sequence) {
      std::ostringstream message;
      message << "D3D side-record sequence moved behind API timeline: " << sequence
              << " before " << previous_d3d_api_sequence;
      error = message.str();
      return false;
    }
  }

  std::unordered_set<std::uint64_t> metal_sequences;
  metal_sequences.reserve(reader.metal_events().size());
  std::uint64_t previous_metal_sequence = 0;
  for (const auto &event : reader.metal_events()) {
    const auto sequence = event.metal_sequence;
    if (sequence == 0) {
      error = "Metal event has zero sequence";
      return false;
    }
    if (sequence <= previous_metal_sequence) {
      std::ostringstream message;
      message << "Metal sequence is not strictly increasing: " << sequence
              << " after " << previous_metal_sequence;
      error = message.str();
      return false;
    }
    if (!metal_sequences.insert(sequence).second) {
      error = "duplicate Metal sequence: " + std::to_string(sequence);
      return false;
    }
    previous_metal_sequence = sequence;
  }

  return true;
}

bool verify_translation_links(
    const apitrace::trace::TraceBundleReader &reader,
    TranslationLinkStats &stats,
    std::string &error)
{
  stats = {};
  const auto link_path = reader.layout().translation_links_path;
  if (!std::filesystem::is_regular_file(link_path)) {
    return true;
  }

  std::unordered_set<std::uint64_t> d3d_sequences;
  std::unordered_map<std::uint64_t, std::string> d3d_functions;
  d3d_sequences.reserve(reader.events().size());
  for (const auto &event : reader.events()) {
    if (event.callsite.sequence != 0) {
      d3d_sequences.insert(event.callsite.sequence);
      d3d_functions.emplace(event.callsite.sequence, event.callsite.function_name);
    }
  }

  std::unordered_set<std::uint64_t> metal_sequences;
  std::unordered_map<std::uint64_t, apitrace::trace::MetalCallKind> metal_call_kinds;
  metal_sequences.reserve(reader.metal_events().size());
  for (const auto &event : reader.metal_events()) {
    if (event.metal_sequence != 0) {
      metal_sequences.insert(event.metal_sequence);
      metal_call_kinds.emplace(event.metal_sequence, event.call_kind);
    }
  }

  std::ifstream input(link_path);
  if (!input.is_open()) {
    error = "failed to open translation link stream";
    return false;
  }

  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) {
      continue;
    }

    json record = json::parse(line, nullptr, false);
    if (record.is_discarded() || !record.is_object()) {
      std::ostringstream message;
      message << "translation-links.jsonl line " << line_number << ": invalid JSON";
      error = message.str();
      return false;
    }

    const auto d3d_sequence = record.value("d3d_sequence", 0ull);
    const auto metal_sequence_begin = record.value("metal_sequence_begin", 0ull);
    const auto metal_sequence_end = record.value("metal_sequence_end", 0ull);
    const auto scope_kind = record.value("scope_kind", std::string());
    if (d3d_sequence == 0 || d3d_sequences.find(d3d_sequence) == d3d_sequences.end()) {
      std::ostringstream message;
      message << "translation-links.jsonl line " << line_number
              << ": unknown d3d_sequence " << d3d_sequence;
      error = message.str();
      return false;
    }
    if (metal_sequence_begin == 0 || metal_sequence_end == 0 ||
        metal_sequence_begin > metal_sequence_end) {
      std::ostringstream message;
      message << "translation-links.jsonl line " << line_number
              << ": invalid metal sequence range " << metal_sequence_begin
              << ".." << metal_sequence_end;
      error = message.str();
      return false;
    }
    if (metal_sequences.empty()) {
      error = "translation-links.jsonl references Metal sequences but bundle has no Metal callstream";
      return false;
    }
    bool range_has_metal_draw_or_dispatch = false;
    for (std::uint64_t sequence = metal_sequence_begin; sequence <= metal_sequence_end; ++sequence) {
      const auto metal_it = metal_call_kinds.find(sequence);
      if (metal_it == metal_call_kinds.end()) {
        std::ostringstream message;
        message << "translation-links.jsonl line " << line_number
                << ": missing metal_sequence " << sequence;
        error = message.str();
        return false;
      }
      range_has_metal_draw_or_dispatch =
          range_has_metal_draw_or_dispatch || is_metal_draw_or_dispatch_call(metal_it->second);
      if (sequence == UINT64_MAX) {
        break;
      }
    }

    if (scope_kind == "draw_to_metal_calls") {
      ++stats.draw_scope_links;
      if (range_has_metal_draw_or_dispatch) {
        ++stats.draw_scope_links_with_metal_work;
      }
      const auto function_it = d3d_functions.find(d3d_sequence);
      if (function_it != d3d_functions.end() &&
          is_d3d12_pipeline_dependent_function(function_it->second)) {
        ++stats.draw_scope_links_to_d3d_pipeline_work;
      }
    }

    ++stats.total;
  }
  return true;
}

bool verify_strict_options(
    const apitrace::trace::TraceBundleReader &reader,
    const CheckOptions &options,
    const TranslationLinkStats &translation_stats,
    SharedResourceStats &stats,
    const PresentFrameStats &present_stats,
    std::string &error)
{
  const auto api = reader.metadata().api;
  const bool has_d3d_api =
      api == apitrace::trace::ApiKind::D3D11 ||
      api == apitrace::trace::ApiKind::D3D12 ||
      !reader.events().empty();
  const bool has_metal_api = reader.metadata().has_metal_callstream || !reader.metal_events().empty();

  if (options.require_asset_index && !reader.has_asset_index()) {
    error = "strict validation requires assets.json";
    return false;
  }
  if (options.require_d3d && !has_d3d_api) {
    error = "strict check requested a D3D callstream, but the bundle has no D3D API records";
    return false;
  }
  if (options.require_d3d && reader.events().empty()) {
    error = "strict check requested a D3D callstream, but callstream.jsonl has no replay records";
    return false;
  }
  if (options.require_metal && !has_metal_api) {
    error = "strict check requested a Metal callstream, but the bundle metadata has no Metal sideband";
    return false;
  }
  if (options.require_metal && reader.metal_events().empty()) {
    error = "strict check requested a Metal callstream, but metal-callstream.jsonl has no replay records";
    return false;
  }
  if (options.require_translation_links && translation_stats.total == 0) {
    error = "strict check requested translation links, but none were recorded";
    return false;
  }

  std::unordered_set<std::string> generic_resource_paths;
  std::unordered_set<std::string> generic_buffer_paths;
  std::unordered_set<std::string> generic_texture_paths;
  std::unordered_map<std::uint64_t, std::string> blob_paths;
  std::unordered_map<std::string, std::string> resource_hash_by_path;
  for (const auto &asset : reader.assets()) {
    if (!add_blob_path(blob_paths, asset.blob_id, asset.relative_path, "reader asset index", error)) {
      return false;
    }
    if (asset.relative_path.empty()) {
      continue;
    }
    const auto path_key = asset.relative_path.generic_string();
    const auto resource_kind = generic_resource_path_kind(asset.relative_path);
    if (resource_kind == GenericResourcePathKind::Buffer) {
      generic_resource_paths.insert(path_key);
      generic_buffer_paths.insert(path_key);
      if (!asset.content_hash.empty()) {
        resource_hash_by_path[path_key] = "buffer:" + asset.content_hash;
      }
    } else if (resource_kind == GenericResourcePathKind::Texture) {
      generic_resource_paths.insert(path_key);
      generic_texture_paths.insert(path_key);
      if (!asset.content_hash.empty()) {
        resource_hash_by_path[path_key] = "texture:" + asset.content_hash;
      }
    }
  }
  stats.generic_buffer_assets = generic_buffer_paths.size();
  stats.generic_texture_assets = generic_texture_paths.size();
  for (const auto &asset : reader.metal_assets()) {
    if (!add_blob_path(blob_paths, asset.blob_id, asset.relative_path, "reader Metal asset index", error)) {
      return false;
    }
    if (asset.kind == apitrace::trace::AssetKind::Buffer) {
      ++stats.metal_buffer_assets;
    } else if (asset.kind == apitrace::trace::AssetKind::Texture) {
      ++stats.metal_texture_assets;
    } else if (is_api_specific_resource_path(asset.relative_path)) {
      auto part = asset.relative_path.begin();
      ++part;
      if (part != asset.relative_path.end() && *part == "buffers") {
        ++stats.metal_buffer_assets;
      } else if (part != asset.relative_path.end() && *part == "textures") {
        ++stats.metal_texture_assets;
      }
    }
  }

  std::unordered_set<std::string> d3d_resource_paths;
  std::unordered_set<std::string> d3d_buffer_paths;
  std::unordered_set<std::string> d3d_texture_paths;
  std::unordered_set<std::string> d3d_pipeline_paths;
  std::unordered_set<std::string> d3d_shader_paths;
  std::unordered_set<std::string> d3d_root_signature_paths;
  std::unordered_map<std::uint64_t, std::string> root_signature_paths_by_object;
  std::unordered_set<std::string> metal_resource_paths;
  std::unordered_set<std::string> metal_buffer_paths;
  std::unordered_set<std::string> metal_texture_paths;
  std::unordered_set<std::string> metal_library_paths;
  std::unordered_set<std::string> metal_pipeline_paths;
  std::unordered_set<std::uint64_t> metal_library_ids;
  std::unordered_set<std::uint64_t> metal_render_pipeline_ids;
  std::unordered_set<std::uint64_t> metal_compute_pipeline_ids;
  std::unordered_set<std::uint64_t> metal_buffer_ids;
  std::unordered_set<std::uint64_t> metal_texture_ids;
  std::unordered_set<std::uint64_t> metal_sampler_ids;
  std::unordered_set<std::uint64_t> metal_depth_stencil_state_ids;
  std::unordered_set<std::uint64_t> metal_command_buffer_ids;
  std::unordered_set<std::uint64_t> metal_render_encoder_ids;
  std::unordered_set<std::uint64_t> metal_compute_encoder_ids;
  std::unordered_set<std::uint64_t> metal_blit_encoder_ids;
  std::unordered_map<std::uint64_t, bool> metal_render_encoder_has_pipeline;
  std::unordered_map<std::uint64_t, bool> metal_render_encoder_has_viewport;
  std::unordered_map<std::uint64_t, bool> metal_render_encoder_has_scissor;
  std::unordered_map<std::uint64_t, bool> metal_compute_encoder_has_pipeline;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> metal_render_vertex_buffer_bindings;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> metal_render_fragment_buffer_bindings;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>> metal_compute_buffer_bindings;
  std::size_t metal_pipeline_bind_calls = 0;
  std::size_t metal_draw_or_dispatch_calls = 0;
  AssetJsonCache asset_json_cache;
  for (const auto &event : reader.events()) {
    json payload;
    if (!parse_payload(event.payload, payload, error)) {
      error = "sequence " + std::to_string(event.callsite.sequence) + ": " + error;
      return false;
    }
    std::vector<std::string> paths;
    if (!collect_asset_paths_with_nested_json(reader, payload, asset_json_cache, paths, error)) {
      error = "sequence " + std::to_string(event.callsite.sequence) + ": " + error;
      return false;
    }
    if (event.kind == apitrace::trace::EventKind::Call &&
        event.callsite.function_name == "ID3D12Device::CreateRootSignature") {
      const auto root_signature_path = string_from_json(payload.value("root_signature_path", json(nullptr)));
      if (event.object_refs.size() >= 2 && event.object_refs[1] != 0 && !root_signature_path.empty()) {
        const std::filesystem::path root_signature_relative_path(root_signature_path);
        if (!is_d3d_root_signature_asset_path(root_signature_relative_path)) {
          error = "sequence " + std::to_string(event.callsite.sequence) +
                  ": CreateRootSignature root_signature_path does not reference a D3D root signature asset";
          return false;
        }
        std::unordered_set<std::string> event_blob_paths;
        event_blob_paths.reserve(event.blob_refs.size());
        for (const auto blob_id : event.blob_refs) {
          const auto blob_path_it = blob_paths.find(blob_id);
          if (blob_path_it != blob_paths.end()) {
            event_blob_paths.insert(blob_path_it->second);
          }
        }
        if (event_blob_paths.find(root_signature_path) == event_blob_paths.end()) {
          error = "sequence " + std::to_string(event.callsite.sequence) +
                  ": root_signature_path is not covered by blob_refs";
          return false;
        }
        root_signature_paths_by_object[event.object_refs[1]] = root_signature_path;
        d3d_root_signature_paths.insert(root_signature_path);
      }
    }
    if (event.kind == apitrace::trace::EventKind::Call &&
        (event.callsite.function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
         event.callsite.function_name == "ID3D12Device::CreateComputePipelineState" ||
         event.callsite.function_name == "ID3D12Device2::CreatePipelineState") &&
        !verify_d3d_pipeline_payload(
            reader,
            "sequence " + std::to_string(event.callsite.sequence),
            payload,
            event.blob_refs,
            blob_paths,
            root_signature_paths_by_object,
            asset_json_cache,
            d3d_pipeline_paths,
            d3d_shader_paths,
            d3d_root_signature_paths,
            error)) {
      return false;
    }
    const bool skip_resource_stats =
        event.kind == apitrace::trace::EventKind::ResourceBlob &&
        is_present_frame_debug_name(event.object_debug_name);
    for (const auto &path : paths) {
      const std::filesystem::path relative_path(path);
      const auto resource_kind = generic_resource_path_kind(relative_path);
      if (!skip_resource_stats && generic_resource_paths.find(path) != generic_resource_paths.end()) {
        d3d_resource_paths.insert(path);
        if (resource_kind == GenericResourcePathKind::Buffer) {
          d3d_buffer_paths.insert(path);
        } else if (resource_kind == GenericResourcePathKind::Texture) {
          d3d_texture_paths.insert(path);
        }
      }
      if (is_d3d_pipeline_asset_path(relative_path)) {
        d3d_pipeline_paths.insert(path);
      } else if (is_d3d_shader_asset_path(relative_path)) {
        d3d_shader_paths.insert(path);
      } else if (is_d3d_root_signature_asset_path(relative_path)) {
        d3d_root_signature_paths.insert(path);
      }
    }
    if (event.kind == apitrace::trace::EventKind::Call &&
        (event.callsite.function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
         event.callsite.function_name == "ID3D12Device::CreateComputePipelineState" ||
         event.callsite.function_name == "ID3D12Device2::CreatePipelineState" ||
         event.callsite.function_name == "ID3D12GraphicsCommandList::SetPipelineState" ||
         event.callsite.function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
         event.callsite.function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced" ||
         event.callsite.function_name == "ID3D12GraphicsCommandList::Dispatch" ||
         event.callsite.function_name == "ID3D12GraphicsCommandList4::DispatchRays" ||
         event.callsite.function_name == "ID3D12GraphicsCommandList6::DispatchMesh")) {
      ++stats.d3d_pipeline_dependent_calls;
    }
  }
  for (const auto &event : reader.metal_events()) {
    json payload;
    if (!parse_payload(event.payload, payload, error)) {
      error = "metal sequence " + std::to_string(event.metal_sequence) + ": " + error;
      return false;
    }
    std::vector<std::string> paths;
    if (!collect_asset_paths_with_nested_json(reader, payload, asset_json_cache, paths, error)) {
      error = "metal sequence " + std::to_string(event.metal_sequence) + ": " + error;
      return false;
    }
    if (event.call_kind == apitrace::trace::MetalCallKind::Unknown) {
      error = "metal sequence " + std::to_string(event.metal_sequence) +
              ": unknown Metal call kind is not replayable";
      return false;
    }
    for (const auto &path : paths) {
      const std::filesystem::path relative_path(path);
      const auto resource_kind = generic_resource_path_kind(relative_path);
      if (generic_resource_paths.find(path) != generic_resource_paths.end()) {
        metal_resource_paths.insert(path);
        if (resource_kind == GenericResourcePathKind::Buffer) {
          metal_buffer_paths.insert(path);
        } else if (resource_kind == GenericResourcePathKind::Texture) {
          metal_texture_paths.insert(path);
        }
      }
      if (is_metal_library_asset_path(relative_path)) {
        metal_library_paths.insert(path);
      } else if (is_metal_pipeline_asset_path(relative_path)) {
        metal_pipeline_paths.insert(path);
      }
    }

    if (event.call_kind == apitrace::trace::MetalCallKind::DeviceCreate) {
      if (event.function_name == "MTLDevice.newBuffer") {
        const auto length_it = payload.find("length");
        if (length_it == payload.end() ||
            (!length_it->is_number_unsigned() && !length_it->is_number_integer())) {
          error = "metal sequence " + std::to_string(event.metal_sequence) +
                  ": MTLDevice.newBuffer is missing length";
          return false;
        }
        if (json_u64(*length_it) == 0) {
          error = "metal sequence " + std::to_string(event.metal_sequence) +
                  ": MTLDevice.newBuffer has invalid length";
          return false;
        }
        metal_buffer_ids.insert(event.object_id);
      } else if (event.function_name == "MTLDevice.newTexture") {
        if (options.require_metal_replay_closure &&
            !validate_metal_texture_descriptor("Metal replay closure ", payload, error)) {
          return false;
        }
        metal_texture_ids.insert(event.object_id);
      } else if (event.function_name.find("newLibrary") != std::string::npos) {
        const auto library_path = string_from_json(payload.value("library_path", json(nullptr)));
        if (library_path.empty()) {
          error = "metal sequence " + std::to_string(event.metal_sequence) + ": newLibrary is missing library_path";
          return false;
        }
        const std::filesystem::path library_relative_path(library_path);
        if (!is_metal_library_asset_path(library_relative_path)) {
          error = "metal sequence " + std::to_string(event.metal_sequence) +
                  ": newLibrary does not reference a Metal library asset";
          return false;
        }
        metal_library_ids.insert(event.object_id);
      } else if (payload.contains("descriptor_path")) {
        const auto descriptor_path = string_from_json(payload.value("descriptor_path", json(nullptr)));
        if (descriptor_path.empty()) {
          error = "metal sequence " + std::to_string(event.metal_sequence) +
                  ": pipeline creation is missing descriptor_path";
          return false;
        }
        const std::filesystem::path descriptor_relative_path(descriptor_path);
        if (!is_metal_pipeline_asset_path(descriptor_relative_path)) {
          error = "metal sequence " + std::to_string(event.metal_sequence) +
                  ": descriptor_path does not reference a Metal pipeline asset";
          return false;
        }
        json descriptor;
        if (!read_asset_json(reader, descriptor_path, asset_json_cache, descriptor, error)) {
          error = "metal sequence " + std::to_string(event.metal_sequence) + ": " + error;
          return false;
        }
        if (event.function_name.find("newRenderPipelineState") != std::string::npos) {
          if (!verify_metal_render_pipeline_descriptor(
                  "metal sequence " + std::to_string(event.metal_sequence),
                  descriptor,
                  metal_library_ids,
                  error)) {
            return false;
          }
          metal_render_pipeline_ids.insert(event.object_id);
        } else if (event.function_name.find("newComputePipelineState") != std::string::npos) {
          if (!verify_metal_compute_pipeline_descriptor(
                  "metal sequence " + std::to_string(event.metal_sequence),
                  descriptor,
                  metal_library_ids,
                  error)) {
            return false;
          }
          metal_compute_pipeline_ids.insert(event.object_id);
        }
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::CommandBufferBegin) {
      metal_command_buffer_ids.insert(event.object_id);
    } else if (event.call_kind == apitrace::trace::MetalCallKind::RenderEncoderBegin) {
      const auto error_prefix = "Metal replay closure ";
      if (!require_metal_object(
              metal_command_buffer_ids,
              object_id_field(payload, "command_buffer_id"),
              error_prefix,
              "render encoder references an unknown command buffer",
              error) ||
          !validate_metal_render_pass_resources(error_prefix, payload, metal_texture_ids, error)) {
        return false;
      }
      metal_render_encoder_ids.insert(event.object_id);
      metal_render_encoder_has_pipeline[event.object_id] = false;
      metal_render_encoder_has_viewport[event.object_id] = false;
      metal_render_encoder_has_scissor[event.object_id] = false;
    } else if (event.call_kind == apitrace::trace::MetalCallKind::ComputeEncoderBegin) {
      if (!require_metal_object(
              metal_command_buffer_ids,
              object_id_field(payload, "command_buffer_id"),
              "Metal replay closure ",
              "compute encoder references an unknown command buffer",
              error)) {
        return false;
      }
      metal_compute_encoder_ids.insert(event.object_id);
      metal_compute_encoder_has_pipeline[event.object_id] = false;
    } else if (event.call_kind == apitrace::trace::MetalCallKind::BlitEncoderBegin ||
               event.call_kind == apitrace::trace::MetalCallKind::BlitEncoderBatch) {
      if (!require_metal_object(
              metal_command_buffer_ids,
              object_id_field(payload, "command_buffer_id"),
              "Metal replay closure ",
              "blit encoder references an unknown command buffer",
              error)) {
        return false;
      }
      if (event.call_kind == apitrace::trace::MetalCallKind::BlitEncoderBatch) {
        if (!validate_metal_blit_batch_payload(
                "Metal replay closure ", payload, metal_buffer_ids, metal_texture_ids, error)) {
          return false;
        }
      } else {
        metal_blit_encoder_ids.insert(event.object_id);
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::RenderEncoderEnd) {
      if (!require_metal_object(
              metal_render_encoder_ids,
              event.object_id,
              "Metal replay closure ",
              "render encoder end references an unknown render encoder",
              error)) {
        return false;
      }
      metal_render_encoder_ids.erase(event.object_id);
      metal_render_encoder_has_pipeline.erase(event.object_id);
      metal_render_encoder_has_viewport.erase(event.object_id);
      metal_render_encoder_has_scissor.erase(event.object_id);
      metal_render_vertex_buffer_bindings.erase(event.object_id);
      metal_render_fragment_buffer_bindings.erase(event.object_id);
    } else if (event.call_kind == apitrace::trace::MetalCallKind::ComputeEncoderEnd) {
      if (!require_metal_object(
              metal_compute_encoder_ids,
              event.object_id,
              "Metal replay closure ",
              "compute encoder end references an unknown compute encoder",
              error)) {
        return false;
      }
      metal_compute_encoder_ids.erase(event.object_id);
      metal_compute_encoder_has_pipeline.erase(event.object_id);
      metal_compute_buffer_bindings.erase(event.object_id);
    } else if (event.call_kind == apitrace::trace::MetalCallKind::BlitEncoderEnd) {
      if (!require_metal_object(
              metal_blit_encoder_ids,
              event.object_id,
              "Metal replay closure ",
              "blit encoder end references an unknown blit encoder",
              error)) {
        return false;
      }
      metal_blit_encoder_ids.erase(event.object_id);
    } else if (event.call_kind == apitrace::trace::MetalCallKind::CommandBufferCommit) {
      if (!require_metal_object(
              metal_command_buffer_ids,
              event.object_id,
              "Metal replay closure ",
              "command buffer commit references an unknown command buffer",
              error)) {
        return false;
      }
      metal_command_buffer_ids.erase(event.object_id);
    }

    if (event.call_kind == apitrace::trace::MetalCallKind::SetRenderPipelineState ||
        event.call_kind == apitrace::trace::MetalCallKind::SetComputePipelineState) {
      const auto pipeline_id = payload.value("pipeline_state_id", 0ull);
      if (event.call_kind == apitrace::trace::MetalCallKind::SetRenderPipelineState) {
        if (metal_render_pipeline_ids.find(pipeline_id) == metal_render_pipeline_ids.end()) {
          error = "Metal replay closure setRenderPipelineState references an unknown render pipeline";
          return false;
        }
        const auto encoder = metal_render_encoder_has_pipeline.find(event.object_id);
        if (encoder == metal_render_encoder_has_pipeline.end()) {
          error = "Metal replay closure setRenderPipelineState references an unknown render encoder";
          return false;
        }
        encoder->second = true;
      } else {
        if (metal_compute_pipeline_ids.find(pipeline_id) == metal_compute_pipeline_ids.end()) {
          error = "Metal replay closure setComputePipelineState references an unknown compute pipeline";
          return false;
        }
        const auto encoder = metal_compute_encoder_has_pipeline.find(event.object_id);
        if (encoder == metal_compute_encoder_has_pipeline.end()) {
          error = "Metal replay closure setComputePipelineState references an unknown compute encoder";
          return false;
        }
        encoder->second = true;
      }
      ++metal_pipeline_bind_calls;
    }
    if (event.call_kind == apitrace::trace::MetalCallKind::SetVertexBuffer ||
        event.call_kind == apitrace::trace::MetalCallKind::SetFragmentBuffer) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
        error = "Metal replay closure buffer bind references an unknown render encoder";
        return false;
      }
      std::uint64_t buffer_id = 0;
      if (!require_object_id_field(payload, "buffer_id", "Metal replay closure ", "buffer bind", buffer_id, error) ||
          !require_metal_object_or_null(
              metal_buffer_ids,
              buffer_id,
              "Metal replay closure ",
              "buffer bind references an unknown buffer",
              error)) {
        return false;
      }
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "buffer bind", true, error)) {
        return false;
      }
      auto &bindings = event.call_kind == apitrace::trace::MetalCallKind::SetVertexBuffer
                           ? metal_render_vertex_buffer_bindings[event.object_id]
                           : metal_render_fragment_buffer_bindings[event.object_id];
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      if (buffer_id == 0) {
        bindings.erase(index);
      } else {
        bindings.insert(index);
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetComputeBuffer) {
      if (metal_compute_encoder_has_pipeline.find(event.object_id) == metal_compute_encoder_has_pipeline.end()) {
        error = "Metal replay closure compute buffer bind references an unknown compute encoder";
        return false;
      }
      std::uint64_t buffer_id = 0;
      if (!require_object_id_field(payload, "buffer_id", "Metal replay closure ", "compute buffer bind", buffer_id, error) ||
          !require_metal_object_or_null(
              metal_buffer_ids,
              buffer_id,
              "Metal replay closure ",
              "compute buffer bind references an unknown buffer",
              error)) {
        return false;
      }
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "compute buffer bind", true, error)) {
        return false;
      }
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      if (buffer_id == 0) {
        metal_compute_buffer_bindings[event.object_id].erase(index);
      } else {
        metal_compute_buffer_bindings[event.object_id].insert(index);
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetVertexBytes ||
               event.call_kind == apitrace::trace::MetalCallKind::SetFragmentBytes) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
        error = "Metal replay closure inline bytes bind references an unknown render encoder";
        return false;
      }
      const auto description = event.call_kind == apitrace::trace::MetalCallKind::SetVertexBytes
                                   ? "setVertexBytes"
                                   : "setFragmentBytes";
      if (!validate_metal_inline_bytes("Metal replay closure ", payload, description, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetVertexBufferOffset ||
               event.call_kind == apitrace::trace::MetalCallKind::SetFragmentBufferOffset) {
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "buffer offset update", true, error)) {
        return false;
      }
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      if (event.call_kind == apitrace::trace::MetalCallKind::SetVertexBufferOffset) {
        const auto bindings_it = metal_render_vertex_buffer_bindings.find(event.object_id);
        if (bindings_it == metal_render_vertex_buffer_bindings.end() ||
            bindings_it->second.find(index) == bindings_it->second.end()) {
          error = "Metal replay closure buffer offset update occurs before a matching vertex buffer bind";
          return false;
        }
      } else {
        const auto bindings_it = metal_render_fragment_buffer_bindings.find(event.object_id);
        if (bindings_it == metal_render_fragment_buffer_bindings.end() ||
            bindings_it->second.find(index) == bindings_it->second.end()) {
          error = "Metal replay closure buffer offset update occurs before a matching fragment buffer bind";
          return false;
        }
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetComputeBufferOffset) {
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "compute buffer offset update", true, error)) {
        return false;
      }
      const auto index = static_cast<std::uint32_t>(object_id_field(payload, "index"));
      const auto bindings_it = metal_compute_buffer_bindings.find(event.object_id);
      if (bindings_it == metal_compute_buffer_bindings.end() || bindings_it->second.find(index) == bindings_it->second.end()) {
        error = "Metal replay closure compute buffer offset update occurs before a matching buffer bind";
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetComputeBytes) {
      if (metal_compute_encoder_has_pipeline.find(event.object_id) == metal_compute_encoder_has_pipeline.end()) {
        error = "Metal replay closure setComputeBytes references an unknown compute encoder";
        return false;
      }
      if (!validate_metal_inline_bytes("Metal replay closure ", payload, "setComputeBytes", error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetArgumentBuffer) {
      error = "Metal replay closure SetArgumentBuffer requires native Metal argument-buffer replay support";
      return false;
    } else if (event.call_kind == apitrace::trace::MetalCallKind::UseHeap) {
      error = "Metal replay closure UseHeap requires native Metal heap replay support";
      return false;
    } else if (event.call_kind == apitrace::trace::MetalCallKind::UseResource) {
      const bool is_render_encoder =
          metal_render_encoder_has_pipeline.find(event.object_id) != metal_render_encoder_has_pipeline.end();
      const bool is_compute_encoder =
          metal_compute_encoder_has_pipeline.find(event.object_id) != metal_compute_encoder_has_pipeline.end();
      if (!is_render_encoder && !is_compute_encoder) {
        error = "Metal replay closure useResource references an unknown encoder";
        return false;
      }
      const auto resource_id = object_id_field(payload, "resource_id");
      if (metal_buffer_ids.find(resource_id) == metal_buffer_ids.end() &&
          metal_texture_ids.find(resource_id) == metal_texture_ids.end()) {
        error = "Metal replay closure useResource references an unknown replayable resource";
        return false;
      }
      if (!validate_metal_resource_usage_payload("Metal replay closure ", payload, is_render_encoder, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::UseResources) {
      const bool is_render_encoder =
          metal_render_encoder_has_pipeline.find(event.object_id) != metal_render_encoder_has_pipeline.end();
      const bool is_compute_encoder =
          metal_compute_encoder_has_pipeline.find(event.object_id) != metal_compute_encoder_has_pipeline.end();
      if (!is_render_encoder && !is_compute_encoder) {
        error = "Metal replay closure useResources references an unknown encoder";
        return false;
      }
      const auto resources = payload.find("resource_ids");
      if (resources == payload.end() || !resources->is_array()) {
        error = "Metal replay closure useResources is missing resource_ids";
        return false;
      }
      if (!validate_metal_resource_usage_payload("Metal replay closure ", payload, is_render_encoder, error)) {
        return false;
      }
      for (const auto &resource : *resources) {
        const auto resource_id = json_u64(resource);
        if (metal_buffer_ids.find(resource_id) == metal_buffer_ids.end() &&
            metal_texture_ids.find(resource_id) == metal_texture_ids.end()) {
          error = "Metal replay closure useResources references an unknown replayable resource";
          return false;
        }
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::MemoryBarrier ||
               event.call_kind == apitrace::trace::MetalCallKind::FenceOps ||
               event.call_kind == apitrace::trace::MetalCallKind::UpdateFence ||
               event.call_kind == apitrace::trace::MetalCallKind::WaitForFence) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end() &&
          metal_compute_encoder_has_pipeline.find(event.object_id) == metal_compute_encoder_has_pipeline.end() &&
          metal_blit_encoder_ids.find(event.object_id) == metal_blit_encoder_ids.end()) {
        error = "Metal replay closure synchronization command references an unknown encoder";
        return false;
      }
      if (!validate_metal_synchronization_payload("Metal replay closure ", event.call_kind, payload, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetCullMode ||
               event.call_kind == apitrace::trace::MetalCallKind::SetFrontFacingWinding ||
               event.call_kind == apitrace::trace::MetalCallKind::SetTriangleFillMode ||
               event.call_kind == apitrace::trace::MetalCallKind::SetViewport ||
               event.call_kind == apitrace::trace::MetalCallKind::SetScissorRect) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
        error = "Metal replay closure render state command references an unknown render encoder";
        return false;
      }
      if (!validate_metal_render_state_payload("Metal replay closure ", event.call_kind, payload, error)) {
        return false;
      }
      if (event.call_kind == apitrace::trace::MetalCallKind::SetViewport) {
        metal_render_encoder_has_viewport[event.object_id] = true;
      } else if (event.call_kind == apitrace::trace::MetalCallKind::SetScissorRect) {
        metal_render_encoder_has_scissor[event.object_id] = true;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetDepthStencilState) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
        error = "Metal replay closure depth stencil state command references an unknown render encoder";
        return false;
      }
      const auto depth_stencil_field = payload.find("depth_stencil_state_id");
      if (depth_stencil_field == payload.end() ||
          (!depth_stencil_field->is_number_unsigned() && !depth_stencil_field->is_number_integer())) {
        error = "Metal replay closure setDepthStencilState is missing depth_stencil_state_id";
        return false;
      }
      const auto depth_stencil_state_id = object_id_field(payload, "depth_stencil_state_id");
      if (depth_stencil_state_id != 0 &&
          metal_depth_stencil_state_ids.find(depth_stencil_state_id) == metal_depth_stencil_state_ids.end()) {
        error = "Metal replay closure depth stencil state command references an unknown depth stencil state";
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::EncoderState) {
      const auto kind = string_from_json(payload.value("kind", json(nullptr)));
      if (kind == "dxmt_set_rasterizer_state" ||
          kind == "dxmt_set_blend_factor" ||
          kind == "dxmt_set_viewports" ||
          kind == "dxmt_set_scissor_rects" ||
          kind == "dxmt_set_depth_stencil_state") {
        if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
          error = "Metal replay closure encoder state command references an unknown render encoder";
          return false;
        }
        if (!validate_dxmt_encoder_state_payload("Metal replay closure ", kind, payload, error)) {
          return false;
        }
        if (kind == "dxmt_set_viewports") {
          metal_render_encoder_has_viewport[event.object_id] = true;
        } else if (kind == "dxmt_set_scissor_rects") {
          metal_render_encoder_has_scissor[event.object_id] = true;
        }
        if (kind == "dxmt_set_depth_stencil_state") {
          const auto depth_stencil_state_id = object_id_field(payload, "depth_stencil_state_id");
          if (depth_stencil_state_id != 0 &&
              metal_depth_stencil_state_ids.find(depth_stencil_state_id) == metal_depth_stencil_state_ids.end()) {
            error = "Metal replay closure encoder state references an unknown depth stencil state";
            return false;
          }
        }
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetVertexTexture ||
               event.call_kind == apitrace::trace::MetalCallKind::SetFragmentTexture) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
        error = "Metal replay closure texture bind references an unknown render encoder";
        return false;
      }
      std::uint64_t texture_id = 0;
      if (!require_object_id_field(payload, "texture_id", "Metal replay closure ", "texture bind", texture_id, error) ||
          !require_metal_object_or_null(
              metal_texture_ids,
              texture_id,
              "Metal replay closure ",
              "texture bind references an unknown texture",
              error)) {
        return false;
      }
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "texture bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetComputeTexture) {
      if (metal_compute_encoder_has_pipeline.find(event.object_id) == metal_compute_encoder_has_pipeline.end()) {
        error = "Metal replay closure compute texture bind references an unknown compute encoder";
        return false;
      }
      std::uint64_t texture_id = 0;
      if (!require_object_id_field(payload, "texture_id", "Metal replay closure ", "compute texture bind", texture_id, error) ||
          !require_metal_object_or_null(
              metal_texture_ids,
              texture_id,
              "Metal replay closure ",
              "compute texture bind references an unknown texture",
              error)) {
        return false;
      }
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "compute texture bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetVertexSamplerState ||
               event.call_kind == apitrace::trace::MetalCallKind::SetFragmentSamplerState) {
      if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
        error = "Metal replay closure sampler bind references an unknown render encoder";
        return false;
      }
      std::uint64_t sampler_state_id = 0;
      if (!require_object_id_field(payload, "sampler_state_id", "Metal replay closure ", "sampler bind", sampler_state_id, error) ||
          !require_metal_object_or_null(
              metal_sampler_ids,
              sampler_state_id,
              "Metal replay closure ",
              "sampler bind references an unknown sampler",
              error)) {
        return false;
      }
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "sampler bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::SetComputeSamplerState) {
      if (metal_compute_encoder_has_pipeline.find(event.object_id) == metal_compute_encoder_has_pipeline.end()) {
        error = "Metal replay closure compute sampler bind references an unknown compute encoder";
        return false;
      }
      std::uint64_t sampler_state_id = 0;
      if (!require_object_id_field(payload, "sampler_state_id", "Metal replay closure ", "compute sampler bind", sampler_state_id, error) ||
          !require_metal_object_or_null(
              metal_sampler_ids,
              sampler_state_id,
              "Metal replay closure ",
              "compute sampler bind references an unknown sampler",
              error)) {
        return false;
      }
      if (!validate_metal_bind_payload("Metal replay closure ", payload, "compute sampler bind", false, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::DrawIndexedPrimitives) {
      if (!require_metal_object(
              metal_buffer_ids,
              object_id_field(payload, "index_buffer_id"),
              "Metal replay closure ",
              "drawIndexedPrimitives references an unknown index buffer",
              error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::DrawPrimitivesIndirect ||
               event.call_kind == apitrace::trace::MetalCallKind::DispatchThreadgroupsIndirect) {
      if (!require_metal_object(
              metal_buffer_ids,
              object_id_field(payload, "indirect_buffer_id"),
              "Metal replay closure ",
              "indirect command references an unknown indirect buffer",
              error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::DrawIndexedPrimitivesIndirect) {
      if (!require_metal_object(
              metal_buffer_ids,
              object_id_field(payload, "index_buffer_id"),
              "Metal replay closure ",
              "drawIndexedPrimitivesIndirect references an unknown index buffer",
              error) ||
          !require_metal_object(
              metal_buffer_ids,
              object_id_field(payload, "indirect_buffer_id"),
              "Metal replay closure ",
              "drawIndexedPrimitivesIndirect references an unknown indirect buffer",
              error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::CopyBuffer ||
               event.call_kind == apitrace::trace::MetalCallKind::CopyBufferToTexture ||
               event.call_kind == apitrace::trace::MetalCallKind::CopyTexture ||
               event.call_kind == apitrace::trace::MetalCallKind::BlitFill) {
      if (!require_metal_object(
              metal_blit_encoder_ids,
              event.object_id,
              "Metal replay closure ",
              "blit command references an unknown blit encoder",
              error) ||
          !validate_metal_blit_payload_resources(
              "Metal replay closure ", event.call_kind, payload, metal_buffer_ids, metal_texture_ids, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::BlitBatch) {
      if (!require_metal_object(
              metal_blit_encoder_ids,
              event.object_id,
              "Metal replay closure ",
              "blitBatch references an unknown blit encoder",
              error) ||
          !validate_metal_blit_batch_payload(
              "Metal replay closure ", payload, metal_buffer_ids, metal_texture_ids, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::PresentDrawable) {
      if (!require_metal_object(
              metal_command_buffer_ids,
              event.object_id,
              "Metal replay closure ",
              "presentDrawable references an unknown command buffer",
              error) ||
          !require_metal_object(
              metal_texture_ids,
              object_id_field(payload, "drawable_id"),
              "Metal replay closure ",
              "presentDrawable references an unknown drawable texture",
              error)) {
        return false;
      }
      if (!validate_metal_present_payload("Metal replay closure ", payload, error)) {
        return false;
      }
    } else if (event.call_kind == apitrace::trace::MetalCallKind::ObjectMetadata ||
               event.call_kind == apitrace::trace::MetalCallKind::InsertDebugSignpost) {
      const json metadata = event.call_kind == apitrace::trace::MetalCallKind::InsertDebugSignpost
                                ? json::parse(payload.value("label", std::string()), nullptr, false)
                                : payload;
      if (!metadata.is_discarded() && metadata.is_object()) {
        const auto kind = string_from_json(metadata.value("kind", json(nullptr)));
        if (kind == "dxmt_sampler_gpu_resource_id") {
          if (!validate_metal_sampler_descriptor("Metal replay closure ", metadata, error)) {
            return false;
          }
          const auto sampler_id = object_id_field(metadata, "sampler_id");
          metal_sampler_ids.insert(sampler_id);
        } else if (kind == "dxmt_texture_view") {
          if (!validate_metal_texture_view_descriptor("Metal replay closure ", metadata, error)) {
            return false;
          }
          if (!require_metal_object(
                  metal_texture_ids,
                  object_id_field(metadata, "source_texture_id"),
                  "Metal replay closure ",
                  "texture view references an unknown source texture",
                  error)) {
            return false;
          }
          const auto texture_id = object_id_field(metadata, "texture_id");
          if (texture_id != 0) {
            metal_texture_ids.insert(texture_id);
          }
        } else if (kind == "dxmt_depth_stencil_state") {
          if (!validate_metal_depth_stencil_descriptor("Metal replay closure ", metadata, error)) {
            return false;
          }
          const auto depth_stencil_state_id = object_id_field(metadata, "depth_stencil_state_id");
          if (depth_stencil_state_id != 0) {
            metal_depth_stencil_state_ids.insert(depth_stencil_state_id);
          }
        } else if (kind == "dxmt_buffer_gpu_address" ||
                   kind == "dxmt_texture_gpu_resource_id") {
          if (!validate_dxmt_resource_id_metadata("Metal replay closure ", kind, metadata, error)) {
            return false;
          }
        } else if (kind == "dxmt_copy_buffer_to_texture") {
          if (!require_metal_object(
                  metal_blit_encoder_ids,
                  event.object_id,
                  "Metal replay closure ",
                  "copy buffer to texture signpost references an unknown blit encoder",
                  error) ||
              !validate_metal_blit_payload_resources(
                  "Metal replay closure ",
                  apitrace::trace::MetalCallKind::CopyBufferToTexture,
                  metadata,
                  metal_buffer_ids,
                  metal_texture_ids,
                  error)) {
            return false;
          }
        } else if (kind == "dxmt_dispatch_threads") {
          const auto encoder = metal_compute_encoder_has_pipeline.find(event.object_id);
          if (encoder == metal_compute_encoder_has_pipeline.end() || !encoder->second) {
            error = "Metal replay closure dispatch signpost occurs before a valid compute pipeline bind";
            return false;
          }
          if (!validate_metal_work_payload(
                  "Metal replay closure ", apitrace::trace::MetalCallKind::DispatchThreads, metadata, error)) {
            return false;
          }
          ++metal_draw_or_dispatch_calls;
        } else if (kind == "dxmt_set_compute_bytes") {
          if (metal_compute_encoder_has_pipeline.find(event.object_id) == metal_compute_encoder_has_pipeline.end()) {
            error = "Metal replay closure compute bytes signpost references an unknown compute encoder";
            return false;
          }
          if (!validate_metal_inline_bytes("Metal replay closure ", metadata, "compute bytes signpost", error)) {
            return false;
          }
        } else if (kind == "dxmt_set_rasterizer_state" ||
                   kind == "dxmt_set_blend_factor" ||
                   kind == "dxmt_set_viewports" ||
                   kind == "dxmt_set_scissor_rects" ||
                   kind == "dxmt_set_depth_stencil_state") {
          if (metal_render_encoder_has_pipeline.find(event.object_id) == metal_render_encoder_has_pipeline.end()) {
            error = "Metal replay closure render state signpost references an unknown render encoder";
            return false;
          }
          if (!validate_dxmt_encoder_state_payload("Metal replay closure ", kind, metadata, error)) {
            return false;
          }
          if (kind == "dxmt_set_viewports") {
            metal_render_encoder_has_viewport[event.object_id] = true;
          } else if (kind == "dxmt_set_scissor_rects") {
            metal_render_encoder_has_scissor[event.object_id] = true;
          }
          if (kind == "dxmt_set_depth_stencil_state") {
            const auto depth_stencil_state_id = object_id_field(metadata, "depth_stencil_state_id");
            if (depth_stencil_state_id != 0 &&
                metal_depth_stencil_state_ids.find(depth_stencil_state_id) == metal_depth_stencil_state_ids.end()) {
              error = "Metal replay closure depth stencil state signpost references an unknown depth stencil state";
              return false;
            }
          }
        }
      }
    }
    switch (event.call_kind) {
    case apitrace::trace::MetalCallKind::DrawPrimitives:
    case apitrace::trace::MetalCallKind::DrawIndexedPrimitives:
    case apitrace::trace::MetalCallKind::DrawPrimitivesIndirect:
    case apitrace::trace::MetalCallKind::DrawIndexedPrimitivesIndirect:
      {
        if (!validate_metal_work_payload("Metal replay closure ", event.call_kind, payload, error)) {
          return false;
        }
        const auto encoder = metal_render_encoder_has_pipeline.find(event.object_id);
        if (encoder == metal_render_encoder_has_pipeline.end() || !encoder->second) {
          error = "Metal replay closure draw occurs before a valid render pipeline bind";
          return false;
        }
        const auto viewport = metal_render_encoder_has_viewport.find(event.object_id);
        if (viewport == metal_render_encoder_has_viewport.end() || !viewport->second) {
          error = "Metal replay closure draw occurs before a valid viewport bind";
          return false;
        }
        const auto scissor = metal_render_encoder_has_scissor.find(event.object_id);
        if (scissor == metal_render_encoder_has_scissor.end() || !scissor->second) {
          error = "Metal replay closure draw occurs before a valid scissor bind";
          return false;
        }
      }
      ++metal_draw_or_dispatch_calls;
      break;
    case apitrace::trace::MetalCallKind::DispatchThreadgroups:
    case apitrace::trace::MetalCallKind::DispatchThreadgroupsIndirect:
    case apitrace::trace::MetalCallKind::DispatchThreads:
      {
        if (!validate_metal_work_payload("Metal replay closure ", event.call_kind, payload, error)) {
          return false;
        }
        const auto encoder = metal_compute_encoder_has_pipeline.find(event.object_id);
        if (encoder == metal_compute_encoder_has_pipeline.end() || !encoder->second) {
          error = "Metal replay closure dispatch occurs before a valid compute pipeline bind";
          return false;
        }
      }
      ++metal_draw_or_dispatch_calls;
      break;
    default:
      break;
    }
  }

  stats.paths_referenced_by_d3d = d3d_resource_paths.size();
  stats.paths_referenced_by_metal = metal_resource_paths.size();
  stats.d3d_buffer_paths = d3d_buffer_paths.size();
  stats.d3d_texture_paths = d3d_texture_paths.size();
  stats.metal_buffer_paths = metal_buffer_paths.size();
  stats.metal_texture_paths = metal_texture_paths.size();
  stats.d3d_pipeline_paths = d3d_pipeline_paths.size();
  stats.d3d_shader_paths = d3d_shader_paths.size();
  stats.d3d_root_signature_paths = d3d_root_signature_paths.size();
  stats.metal_library_paths = metal_library_paths.size();
  stats.metal_pipeline_paths = metal_pipeline_paths.size();
  stats.metal_pipeline_bind_calls = metal_pipeline_bind_calls;
  stats.metal_draw_or_dispatch_calls = metal_draw_or_dispatch_calls;
  for (const auto &path : d3d_resource_paths) {
    if (metal_resource_paths.find(path) != metal_resource_paths.end()) {
      ++stats.shared_resource_paths;
    }
  }
  for (const auto &path : d3d_buffer_paths) {
    if (metal_buffer_paths.find(path) != metal_buffer_paths.end()) {
      ++stats.shared_buffer_paths;
    }
  }
  for (const auto &path : d3d_texture_paths) {
    if (metal_texture_paths.find(path) != metal_texture_paths.end()) {
      ++stats.shared_texture_paths;
    }
  }
  std::unordered_map<std::string, std::string> d3d_resource_path_by_hash;
  for (const auto &path : d3d_resource_paths) {
    const auto hash_it = resource_hash_by_path.find(path);
    if (hash_it != resource_hash_by_path.end()) {
      d3d_resource_path_by_hash.emplace(hash_it->second, path);
    }
  }
  for (const auto &path : metal_resource_paths) {
    const auto hash_it = resource_hash_by_path.find(path);
    if (hash_it == resource_hash_by_path.end()) {
      continue;
    }
    const auto d3d_path_it = d3d_resource_path_by_hash.find(hash_it->second);
    if (d3d_path_it != d3d_resource_path_by_hash.end() && d3d_path_it->second != path) {
      ++stats.duplicated_cross_api_resource_hashes;
    }
  }

  if (options.require_shared_resources) {
    if (!options.require_d3d || !options.require_metal) {
      error = "strict shared resource validation requires both --require-d3d and --require-metal";
      return false;
    }
    if (stats.metal_buffer_assets != 0 || stats.metal_texture_assets != 0) {
      error = "Metal buffer/texture assets must be stored as API-independent resources";
      return false;
    }
    if (stats.generic_buffer_assets + stats.generic_texture_assets == 0) {
      error = "strict shared resource validation found no API-independent buffer or texture assets";
      return false;
    }
    if (stats.paths_referenced_by_d3d == 0 || stats.paths_referenced_by_metal == 0) {
      error = "strict shared resource validation requires both D3D and Metal to reference resource assets";
      return false;
    }
    if (stats.shared_resource_paths == 0) {
      error = "strict shared resource validation found no buffer/texture path referenced by both D3D and Metal";
      return false;
    }
    if (stats.d3d_buffer_paths != 0 && stats.metal_buffer_paths != 0 && stats.shared_buffer_paths == 0) {
      error = "strict shared resource validation found buffer assets on both sides, but no shared buffer path";
      return false;
    }
    if (stats.d3d_texture_paths != 0 && stats.metal_texture_paths != 0 && stats.shared_texture_paths == 0) {
      error = "strict shared resource validation found texture assets on both sides, but no shared texture path";
      return false;
    }
    if (stats.duplicated_cross_api_resource_hashes != 0) {
      error = "strict shared resource validation found duplicate D3D/Metal resource content under different paths";
      return false;
    }
  }

  if (options.require_d3d_replay_closure) {
    if (!options.require_d3d) {
      error = "D3D replay closure validation requires --require-d3d";
      return false;
    }
    if (api == apitrace::trace::ApiKind::D3D12) {
      if (options.require_metal_replay_closure && stats.metal_draw_or_dispatch_calls != 0) {
        if (translation_stats.draw_scope_links_with_metal_work == 0) {
          error = "D3D12 replay closure has no per-call translation links for Metal draw/dispatch work";
          return false;
        }
        if (translation_stats.draw_scope_links_to_d3d_pipeline_work !=
            translation_stats.draw_scope_links_with_metal_work) {
          error = "D3D12 replay closure has Metal draw/dispatch links that do not point to D3D pipeline work";
          return false;
        }
      }
      if (stats.d3d_pipeline_dependent_calls != 0 && stats.d3d_pipeline_paths == 0) {
        error = "D3D12 replay closure has no referenced pipeline assets";
        return false;
      }
      if (stats.d3d_pipeline_dependent_calls != 0 && stats.d3d_shader_paths == 0) {
        error = "D3D12 replay closure has no referenced shader assets";
        return false;
      }
      if (stats.d3d_pipeline_dependent_calls != 0 && stats.d3d_root_signature_paths == 0) {
        error = "D3D12 replay closure has no referenced root-signature assets";
        return false;
      }
    }
  }

  if (options.require_metal_replay_closure) {
    if (!options.require_metal) {
      error = "Metal replay closure validation requires --require-metal";
      return false;
    }
    if (stats.metal_library_paths == 0) {
      error = "Metal replay closure has no referenced library assets";
      return false;
    }
    if (stats.metal_pipeline_paths == 0) {
      error = "Metal replay closure has no referenced pipeline assets";
      return false;
    }
    if (stats.metal_pipeline_bind_calls == 0) {
      error = "Metal replay closure has no pipeline bind calls";
      return false;
    }
    if (stats.metal_draw_or_dispatch_calls == 0) {
      error = "Metal replay closure has no draw or dispatch calls";
      return false;
    }
    if (!metal_render_encoder_ids.empty()) {
      error = "Metal replay closure ended with an open render encoder";
      return false;
    }
    if (!metal_compute_encoder_ids.empty()) {
      error = "Metal replay closure ended with an open compute encoder";
      return false;
    }
    if (!metal_blit_encoder_ids.empty()) {
      error = "Metal replay closure ended with an open blit encoder";
      return false;
    }
    if (!metal_command_buffer_ids.empty()) {
      error = "Metal replay closure ended with an uncommitted command buffer";
      return false;
    }
  }

  if (options.require_d3d_present_frames) {
    if (!options.require_d3d) {
      error = "D3D PresentFrame validation requires --require-d3d";
      return false;
    }
    if (present_stats.d3d_present_calls == 0) {
      error = "D3D PresentFrame validation found no captured IDXGISwapChain::Present calls";
      return false;
    }
    if (present_stats.d3d_present_frame_assets == 0) {
      error = "D3D PresentFrame validation found no D3D PresentFrame assets";
      return false;
    }
    if (present_stats.d3d_present_frame_assets != present_stats.d3d_present_calls) {
      error = "D3D PresentFrame asset count does not match captured Present calls";
      return false;
    }
    if (present_stats.d3d_present_boundaries != present_stats.d3d_present_calls) {
      error = "D3D PresentFrame boundary count does not match captured Present calls";
      return false;
    }
  }

  if (options.require_metal_present_frames) {
    if (!options.require_metal) {
      error = "Metal PresentFrame validation requires --require-metal";
      return false;
    }
    if (present_stats.metal_present_drawables == 0) {
      error = "Metal PresentFrame validation found no PresentDrawable calls";
      return false;
    }
    if (present_stats.metal_present_frame_assets == 0) {
      error = "Metal PresentFrame validation found no MetalPresentFrame assets";
      return false;
    }
    if (present_stats.metal_present_frame_assets != present_stats.metal_present_drawables) {
      error = "Metal PresentFrame asset count does not match PresentDrawable calls";
      return false;
    }
  }

  return true;
}

bool verify_d3d_native_readiness(
    const apitrace::trace::TraceBundleReader &reader,
    std::uint64_t &d3d_native_replay_commands,
    std::string &error)
{
  if (reader.metadata().api != apitrace::trace::ApiKind::D3D12) {
    error = "D3D native readiness validation currently requires a D3D12 bundle";
    return false;
  }

  apitrace::d3d12::D3D12ReplayBackend backend;
  if (!backend.initialize(reader)) {
    error = backend.last_error().empty() ? "failed to initialize D3D12 replay backend" : backend.last_error();
    return false;
  }
  for (const auto &event : reader.events()) {
    if (!backend.replay_event(event)) {
      error = backend.last_error().empty()
                  ? "D3D12 replay backend rejected a callstream event"
                  : backend.last_error();
      return false;
    }
  }
  d3d_native_replay_commands = backend.replay_commands().size();
  if (!backend.validate_only()) {
    error = backend.last_error().empty() ? "D3D12 native readiness validation failed" : backend.last_error();
    return false;
  }
  return true;
}

bool verify_d3d_replay_closure(
    const apitrace::trace::TraceBundleReader &reader,
    std::uint64_t &d3d_replay_commands,
    std::string &error)
{
  if (reader.metadata().api != apitrace::trace::ApiKind::D3D12) {
    error = "D3D replay closure validation currently requires a D3D12 bundle";
    return false;
  }

  apitrace::d3d12::D3D12ReplayBackend backend;
  if (!backend.initialize(reader)) {
    error = backend.last_error().empty() ? "failed to initialize D3D12 replay backend" : backend.last_error();
    return false;
  }
  for (const auto &event : reader.events()) {
    if (!backend.replay_event(event)) {
      error = backend.last_error().empty()
                  ? "D3D12 replay backend rejected a callstream event"
                  : backend.last_error();
      return false;
    }
  }
  d3d_replay_commands = backend.replay_commands().size();
  if (!backend.validate_replay_closure()) {
    error = backend.last_error().empty() ? "D3D12 replay closure validation failed" : backend.last_error();
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  CheckOptions options;
  std::filesystem::path bundle;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--require-d3d") {
      options.require_d3d = true;
      continue;
    }
    if (arg == "--require-asset-index") {
      options.require_asset_index = true;
      continue;
    }
    if (arg == "--require-metal") {
      options.require_metal = true;
      continue;
    }
    if (arg == "--require-translation-links") {
      options.require_translation_links = true;
      continue;
    }
    if (arg == "--require-shared-resources") {
      options.require_shared_resources = true;
      continue;
    }
    if (arg == "--require-d3d-replay-closure") {
      options.require_d3d_replay_closure = true;
      continue;
    }
    if (arg == "--require-d3d-native-readiness") {
      options.require_d3d_native_readiness = true;
      continue;
    }
    if (arg == "--require-metal-replay-closure") {
      options.require_metal_replay_closure = true;
      continue;
    }
    if (arg == "--require-d3d-present-frames") {
      options.require_d3d_present_frames = true;
      continue;
    }
    if (arg == "--require-metal-present-frames") {
      options.require_metal_present_frames = true;
      continue;
    }
    if (arg == "--strict-cross-api") {
      enable_strict_cross_api(options);
      continue;
    }
    if (arg.empty() || arg[0] == '-' || !bundle.empty()) {
      print_usage(argc > 0 ? argv[0] : nullptr);
      return 2;
    }
    bundle = std::filesystem::path(arg);
  }

  if (bundle.empty()) {
    print_usage(argc > 0 ? argv[0] : nullptr);
    return 2;
  }
  if (options.require_d3d_replay_closure ||
      options.require_d3d_native_readiness ||
      options.require_metal_replay_closure ||
      options.require_shared_resources) {
    options.require_asset_index = true;
  }

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    std::cerr << "bundle-check failed: " << reader.last_error() << "\n";
    return 1;
  }

  for (const auto &record : reader.checksums().files) {
    if (record.algorithm != "sha256") {
      std::cerr << "bundle-check failed: unsupported checksum algorithm for "
                << record.relative_path.generic_string() << "\n";
      return 1;
    }
    if (reader.validated_checksum_paths().find(record.relative_path.generic_string()) !=
        reader.validated_checksum_paths().end()) {
      continue;
    }
    const auto absolute = reader.layout().root_path / record.relative_path;
    if (!std::filesystem::is_regular_file(absolute)) {
      std::cerr << "bundle-check failed: missing checksum file "
                << record.relative_path.generic_string() << "\n";
      return 1;
    }
    const auto digest = apitrace::trace::content_hash_file(absolute);
    if (digest != record.digest) {
      std::cerr << "bundle-check failed: checksum mismatch for "
                << record.relative_path.generic_string() << "\n";
      return 1;
    }
  }
  const auto expected_bundle_hash = bundle_hash_from_records(reader.checksums().files);
  if (!reader.checksums().bundle_hash.empty() &&
      reader.checksums().bundle_hash != expected_bundle_hash) {
    std::cerr << "bundle-check failed: bundle_hash mismatch"
              << " expected " << expected_bundle_hash
              << " got " << reader.checksums().bundle_hash << "\n";
    return 1;
  }

  std::size_t d3d_object_refs = 0;
  std::size_t metal_object_refs = 0;
  std::size_t blob_refs = 0;
  std::size_t asset_path_refs = 0;
  TranslationLinkStats translation_stats;
  PresentFrameStats present_stats;
  std::string reference_error;
  if (!verify_sequences(reader, reference_error)) {
    std::cerr << "bundle-check failed: " << reference_error << "\n";
    return 1;
  }
  if (!verify_bundle_references(
          reader,
          d3d_object_refs,
          metal_object_refs,
          blob_refs,
          asset_path_refs,
          present_stats,
          reference_error)) {
    std::cerr << "bundle-check failed: " << reference_error << "\n";
    return 1;
  }
  if (!verify_translation_links(reader, translation_stats, reference_error)) {
    std::cerr << "bundle-check failed: " << reference_error << "\n";
    return 1;
  }
  SharedResourceStats shared_stats;
  if (!verify_strict_options(reader, options, translation_stats, shared_stats, present_stats, reference_error)) {
    std::cerr << "bundle-check failed: " << reference_error << "\n";
    return 1;
  }
  std::uint64_t d3d_replay_commands = 0;
  std::uint64_t d3d_native_replay_commands = 0;
  if (options.require_d3d_replay_closure && reader.metadata().api == apitrace::trace::ApiKind::D3D12) {
    if (!verify_d3d_replay_closure(reader, d3d_replay_commands, reference_error)) {
      std::cerr << "bundle-check failed: " << reference_error << "\n";
      return 1;
    }
  }
  if (options.require_d3d_native_readiness) {
    if (!options.require_d3d) {
      std::cerr << "bundle-check failed: D3D native readiness validation requires --require-d3d\n";
      return 1;
    }
    if (!verify_d3d_native_readiness(reader, d3d_native_replay_commands, reference_error)) {
      std::cerr << "bundle-check failed: " << reference_error << "\n";
      return 1;
    }
  }

  std::cout << "bundle-check PASS\n";
  std::cout << "events=" << reader.events().size() << "\n";
  std::cout << "metal_events=" << reader.metal_events().size() << "\n";
  std::cout << "assets=" << reader.assets().size() << "\n";
  std::cout << "metal_assets=" << reader.metal_assets().size() << "\n";
  std::cout << "asset_index=" << (reader.has_asset_index() ? 1 : 0) << "\n";
  std::cout << "checksum_files=" << reader.checksums().files.size() << "\n";
  std::cout << "d3d_object_refs=" << d3d_object_refs << "\n";
  std::cout << "metal_object_refs=" << metal_object_refs << "\n";
  std::cout << "blob_refs=" << blob_refs << "\n";
  std::cout << "asset_path_refs=" << asset_path_refs << "\n";
  std::cout << "translation_links=" << translation_stats.total << "\n";
  std::cout << "translation_draw_scope_links=" << translation_stats.draw_scope_links << "\n";
  std::cout << "translation_draw_scope_links_with_metal_work="
            << translation_stats.draw_scope_links_with_metal_work << "\n";
  std::cout << "translation_draw_scope_links_to_d3d_pipeline_work="
            << translation_stats.draw_scope_links_to_d3d_pipeline_work << "\n";
  std::cout << "d3d_present_calls=" << present_stats.d3d_present_calls << "\n";
  std::cout << "d3d_present_boundaries=" << present_stats.d3d_present_boundaries << "\n";
  std::cout << "d3d_present_frame_assets=" << present_stats.d3d_present_frame_assets << "\n";
  std::cout << "metal_present_drawables=" << present_stats.metal_present_drawables << "\n";
  std::cout << "metal_present_frame_assets=" << present_stats.metal_present_frame_assets << "\n";
  std::cout << "generic_buffer_assets=" << shared_stats.generic_buffer_assets << "\n";
  std::cout << "generic_texture_assets=" << shared_stats.generic_texture_assets << "\n";
  std::cout << "metal_buffer_assets=" << shared_stats.metal_buffer_assets << "\n";
  std::cout << "metal_texture_assets=" << shared_stats.metal_texture_assets << "\n";
  std::cout << "d3d_resource_paths=" << shared_stats.paths_referenced_by_d3d << "\n";
  std::cout << "metal_resource_paths=" << shared_stats.paths_referenced_by_metal << "\n";
  std::cout << "shared_resource_paths=" << shared_stats.shared_resource_paths << "\n";
  std::cout << "d3d_buffer_paths=" << shared_stats.d3d_buffer_paths << "\n";
  std::cout << "d3d_texture_paths=" << shared_stats.d3d_texture_paths << "\n";
  std::cout << "metal_buffer_paths=" << shared_stats.metal_buffer_paths << "\n";
  std::cout << "metal_texture_paths=" << shared_stats.metal_texture_paths << "\n";
  std::cout << "shared_buffer_paths=" << shared_stats.shared_buffer_paths << "\n";
  std::cout << "shared_texture_paths=" << shared_stats.shared_texture_paths << "\n";
  std::cout << "duplicated_cross_api_resource_hashes="
            << shared_stats.duplicated_cross_api_resource_hashes << "\n";
  std::cout << "d3d_replay_commands=" << d3d_replay_commands << "\n";
  std::cout << "d3d_native_replay_commands=" << d3d_native_replay_commands << "\n";
  std::cout << "d3d_pipeline_paths=" << shared_stats.d3d_pipeline_paths << "\n";
  std::cout << "d3d_shader_paths=" << shared_stats.d3d_shader_paths << "\n";
  std::cout << "d3d_root_signature_paths=" << shared_stats.d3d_root_signature_paths << "\n";
  std::cout << "d3d_pipeline_dependent_calls=" << shared_stats.d3d_pipeline_dependent_calls << "\n";
  std::cout << "metal_library_paths=" << shared_stats.metal_library_paths << "\n";
  std::cout << "metal_pipeline_paths=" << shared_stats.metal_pipeline_paths << "\n";
  std::cout << "metal_pipeline_bind_calls=" << shared_stats.metal_pipeline_bind_calls << "\n";
  std::cout << "metal_draw_or_dispatch_calls=" << shared_stats.metal_draw_or_dispatch_calls << "\n";
  return 0;
}
