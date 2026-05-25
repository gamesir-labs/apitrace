#include "apitrace/d3d12_replay.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace apitrace::d3d12 {

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

bool validate_object_refs(const trace::EventRecord &event, const D3D12ObjectRegistry &objects, std::string &error)
{
  for (trace::ObjectId object_id : event.object_refs) {
    if (object_id != 0 && !objects.contains(object_id)) {
      std::ostringstream message;
      message << record_prefix(event) << ": unknown object id " << object_id;
      error = message.str();
      return false;
    }
  }
  return true;
}

bool starts_with(std::string_view text, std::string_view prefix)
{
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool is_pipeline_asset_path(const std::filesystem::path &relative_path)
{
  const auto path = relative_path.generic_string();
  return starts_with(path, "pipelines/") && path.find(".pipeline.json") != std::string::npos;
}

void collect_referenced_asset_paths(const json &value, std::vector<std::filesystem::path> &paths)
{
  if (value.is_object()) {
    for (const auto &[key, child] : value.items()) {
      if (key.size() >= 5 && key.compare(key.size() - 5, 5, "_path") == 0 && child.is_string()) {
        paths.emplace_back(child.get<std::string>());
      }
      collect_referenced_asset_paths(child, paths);
    }
    return;
  }

  if (value.is_array()) {
    for (const auto &child : value) {
      collect_referenced_asset_paths(child, paths);
    }
  }
}

bool read_pipeline_asset_json(
    const std::filesystem::path &bundle_root,
    const std::filesystem::path &relative_path,
    std::string &error)
{
  const auto path = bundle_root / relative_path;
  std::ifstream input(path);
  if (!input) {
    error = "missing D3D12 pipeline asset: " + path.generic_string();
    return false;
  }

  json pipeline = json::parse(input, nullptr, false);
  if (pipeline.is_discarded() || !pipeline.is_object()) {
    error = "invalid D3D12 pipeline asset JSON: " + path.generic_string();
    return false;
  }

  std::vector<std::filesystem::path> referenced_paths;
  collect_referenced_asset_paths(pipeline, referenced_paths);
  for (const auto &referenced_path : referenced_paths) {
    const auto absolute_referenced_path = bundle_root / referenced_path;
    if (!std::filesystem::is_regular_file(absolute_referenced_path)) {
      error = "D3D12 pipeline asset references missing file: " + referenced_path.generic_string();
      return false;
    }
  }
  return true;
}

bool is_supported_d3d12_call(std::string_view function_name)
{
  return !function_name.empty();
}

std::uint64_t payload_u64(const json &payload, std::string_view key, std::uint64_t fallback = 0)
{
  const auto it = payload.find(std::string(key));
  if (it == payload.end()) {
    return fallback;
  }
  if (it->is_number_unsigned()) {
    return it->get<std::uint64_t>();
  }
  if (it->is_number_integer()) {
    const auto value = it->get<std::int64_t>();
    return value < 0 ? fallback : static_cast<std::uint64_t>(value);
  }
  return fallback;
}

} // namespace

D3D12ReplayBackend::D3D12ReplayBackend() = default;

bool D3D12ReplayBackend::initialize(const trace::TraceBundleReader &reader)
{
  last_error_.clear();
  objects_.clear();
  submissions_.clear();
  initialized_ = false;
  commands_replayed_ = 0;
  frames_seen_ = 0;
  presents_seen_ = 0;
  pipeline_assets_read_ = 0;
  last_sequence_ = 0;

  if (reader.metadata().api != trace::ApiKind::D3D12) {
    last_error_ = "trace bundle is not a D3D12 bundle";
    return false;
  }

  for (const auto &object : reader.objects()) {
    D3D12TrackedObject tracked;
    tracked.object_id = object.object_id;
    tracked.kind = object.kind;
    tracked.parent_object_id = object.parent_object_id;
    tracked.debug_name = object.debug_name;
    objects_.track(tracked);
  }

  for (const auto &asset : reader.assets()) {
    if (!is_pipeline_asset_path(asset.relative_path)) {
      continue;
    }
    if (!read_pipeline_asset_json(reader.layout().root_path, asset.relative_path, last_error_)) {
      return false;
    }
    ++pipeline_assets_read_;
  }

  initialized_ = true;
  return true;
}

bool D3D12ReplayBackend::replay_event(const trace::EventRecord &event)
{
  if (!initialized_) {
    last_error_ = "D3D12 replay backend was not initialized";
    return false;
  }

  if (event.callsite.sequence != 0 && event.callsite.sequence < last_sequence_) {
    last_error_ = record_prefix(event) + ": sequence number regressed";
    return false;
  }
  last_sequence_ = event.callsite.sequence;

  if (event.kind == trace::EventKind::Boundary) {
    switch (event.boundary) {
    case trace::BoundaryKind::Frame:
      ++frames_seen_;
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    case trace::BoundaryKind::Present:
      ++presents_seen_;
      submissions_.mark_present();
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    case trace::BoundaryKind::Submit:
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    case trace::BoundaryKind::CommandList:
    case trace::BoundaryKind::Fence:
    case trace::BoundaryKind::Barrier:
    case trace::BoundaryKind::DebugMarker:
      return true;
    }
  }

  json payload;
  if (!payload_to_json(event, payload, last_error_)) {
    return false;
  }

  if (event.kind == trace::EventKind::ObjectCreate) {
    if (event.object_id == 0) {
      last_error_ = record_prefix(event) + ": object_create missing object_id";
      return false;
    }

    D3D12TrackedObject tracked;
    tracked.object_id = event.object_id;
    tracked.kind = event.object_kind;
    tracked.parent_object_id = event.parent_object_id;
    tracked.debug_name = event.object_debug_name;
    objects_.track(tracked);
    ++commands_replayed_;
    return true;
  }

  if (event.kind == trace::EventKind::ObjectDestroy) {
    if (event.object_id != 0) {
      objects_.forget(event.object_id);
    }
    ++commands_replayed_;
    return true;
  }

  if (event.kind == trace::EventKind::ResourceBlob) {
    ++commands_replayed_;
    return true;
  }

  if (!validate_object_refs(event, objects_, last_error_)) {
    return false;
  }

  if (!is_supported_d3d12_call(event.callsite.function_name)) {
    last_error_ = record_prefix(event) + ": unsupported D3D12 call";
    return false;
  }

  const auto &function_name = event.callsite.function_name;
  if (function_name.find("ExecuteCommandLists") != std::string::npos) {
    if (!event.object_refs.empty()) {
      submissions_.begin_batch(event.object_refs.front());
      for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
        submissions_.append_command_list(event.object_refs[index]);
      }
      submissions_.end_batch();
    }
  } else if (function_name.find("SetDescriptorHeaps") != std::string::npos) {
    for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
      submissions_.append_descriptor_heap(event.object_refs[index]);
    }
  } else if (function_name.find("Signal") != std::string::npos) {
    if (event.object_refs.size() >= 2) {
      submissions_.signal_fence(event.object_refs[1], payload_u64(payload, "fence_value", payload_u64(payload, "value", 0)));
    }
  } else if (function_name.find("Present") != std::string::npos) {
    submissions_.mark_present();
    if (submissions_.has_open_batch()) {
      submissions_.end_batch();
    }
  } else if (function_name.find("Reset") != std::string::npos && event.object_refs.size() >= 2) {
    submissions_.begin_batch(event.object_refs[0], event.object_refs[1]);
  } else if (function_name.find("Close") != std::string::npos) {
    submissions_.end_batch();
  }

  ++commands_replayed_;
  return true;
}

void D3D12ReplayBackend::shutdown()
{
  objects_.clear();
  submissions_.clear();
  initialized_ = false;
  commands_replayed_ = 0;
  frames_seen_ = 0;
  presents_seen_ = 0;
  pipeline_assets_read_ = 0;
  last_sequence_ = 0;
}

const std::string &D3D12ReplayBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::d3d12
