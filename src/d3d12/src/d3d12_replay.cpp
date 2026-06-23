#include "apitrace/d3d12_replay.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <limits>
#include <sstream>
#include <string_view>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(APITRACE_HAS_D3D_NATIVE)
#include <d3d12.h>
#include <dxgi1_4.h>
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(APITRACE_HAS_D3D_NATIVE) && defined(__APPLE__)
#include "apitrace/platform/macos_window.hpp"
#endif

namespace apitrace::d3d12 {

namespace {

using json = nlohmann::json;

std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point begin)
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - begin)
          .count());
}

std::uint64_t duration_ms(std::chrono::steady_clock::duration duration)
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

class ScopedMsAccumulator {
public:
  explicit ScopedMsAccumulator(std::uint64_t &target, bool enabled = true)
      : target_(target), enabled_(enabled)
  {
    if (enabled_) {
      begin_ = std::chrono::steady_clock::now();
    }
  }

  ~ScopedMsAccumulator()
  {
    stop();
  }

  void stop()
  {
    if (!active_ || !enabled_) {
      return;
    }
    target_ += elapsed_ms(begin_);
    active_ = false;
  }

private:
  std::uint64_t &target_;
  std::chrono::steady_clock::time_point begin_;
  bool enabled_ = true;
  bool active_ = true;
};

class ScopedDurationAccumulator {
public:
  explicit ScopedDurationAccumulator(
      std::chrono::steady_clock::duration &target,
      bool enabled = true,
      std::chrono::steady_clock::duration *secondary_target = nullptr)
      : target_(target),
        secondary_target_(secondary_target),
        enabled_(enabled)
  {
    if (enabled_) {
      begin_ = std::chrono::steady_clock::now();
    }
  }

  ~ScopedDurationAccumulator()
  {
    stop();
  }

  void stop()
  {
    if (!active_ || !enabled_) {
      return;
    }
    const auto duration = std::chrono::steady_clock::now() - begin_;
    target_ += duration;
    if (secondary_target_) {
      *secondary_target_ += duration;
    }
    active_ = false;
  }

private:
  std::chrono::steady_clock::duration &target_;
  std::chrono::steady_clock::duration *secondary_target_ = nullptr;
  std::chrono::steady_clock::time_point begin_;
  bool enabled_ = true;
  bool active_ = true;
};

bool env_enabled(const char *name, bool fallback)
{
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  return std::string_view(value) != "0" &&
         std::string_view(value) != "false" &&
         std::string_view(value) != "FALSE";
}

std::uint32_t env_u32(const char *name, std::uint32_t fallback = 0)
{
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed > std::numeric_limits<std::uint32_t>::max()) {
    return fallback;
  }
  return static_cast<std::uint32_t>(parsed);
}

std::uint64_t env_u64(const char *name, std::uint64_t fallback = 0)
{
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || parsed > std::numeric_limits<std::uint64_t>::max()) {
    return fallback;
  }
  return static_cast<std::uint64_t>(parsed);
}

#if defined(APITRACE_HAS_D3D_NATIVE)

template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(const ComPtr &) = delete;
  ComPtr &operator=(const ComPtr &) = delete;

  ComPtr(ComPtr &&other) noexcept : ptr_(other.detach()) {}
  ComPtr &operator=(ComPtr &&other) noexcept
  {
    if (this != &other) {
      reset(other.detach());
    }
    return *this;
  }

  ~ComPtr()
  {
    reset();
  }

  T *get() const noexcept { return ptr_; }
  T **put() noexcept
  {
    reset();
    return &ptr_;
  }
  T *const *get_address_of() const noexcept { return &ptr_; }
  T **get_address_of() noexcept { return &ptr_; }
  T *operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  void reset(T *ptr = nullptr) noexcept
  {
    if (ptr_) {
      ptr_->Release();
    }
    ptr_ = ptr;
  }

  T *detach() noexcept
  {
    T *tmp = ptr_;
    ptr_ = nullptr;
    return tmp;
  }

private:
  T *ptr_ = nullptr;
};

#ifdef _WIN32
LRESULT CALLBACK replay_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  switch (message) {
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcA(hwnd, message, wparam, lparam);
  }
}

bool pump_messages()
{
  MSG msg{};
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      return false;
    }
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return true;
}
#else
bool pump_messages()
{
  return true;
}
#endif

class ScopedFenceEvent {
public:
  enum class WaitResult {
    Signaled,
    Timeout,
    Failed,
  };

#ifdef _WIN32
  ScopedFenceEvent() : handle_(CreateEventA(nullptr, FALSE, FALSE, nullptr)) {}
  ~ScopedFenceEvent()
  {
    if (handle_) {
      CloseHandle(handle_);
    }
  }

  bool valid() const noexcept { return handle_ != nullptr; }
  HANDLE get() const noexcept { return handle_; }

  WaitResult wait(std::uint32_t timeout_ms) const
  {
    const DWORD result = WaitForSingleObject(handle_, timeout_ms);
    if (result == WAIT_OBJECT_0) {
      return WaitResult::Signaled;
    }
    if (result == WAIT_TIMEOUT) {
      return WaitResult::Timeout;
    }
    return WaitResult::Failed;
  }

private:
  HANDLE handle_ = nullptr;
#else
  ScopedFenceEvent() = default;
  bool valid() const noexcept { return true; }
  HANDLE get() const noexcept { return nullptr; }

  WaitResult wait_until(ID3D12Fence *fence, std::uint64_t value, std::uint32_t timeout_ms) const
  {
    if (!fence) {
      return WaitResult::Failed;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (fence->GetCompletedValue() < value) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return WaitResult::Timeout;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return WaitResult::Signaled;
  }
#endif
};

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

std::string hresult_error(std::string_view action, HRESULT hr)
{
  std::ostringstream message;
  message << action << " failed for D3D12 native replay: 0x"
          << std::hex << static_cast<unsigned long>(hr);
  return message.str();
}

#endif

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

bool object_ref_kind_matches(
    const D3D12ObjectRegistry &objects,
    trace::ObjectId object_id,
    trace::ObjectKind expected_kind)
{
  if (object_id == 0) {
    return false;
  }
  const auto *object = objects.find(object_id);
  return object != nullptr && object->kind == expected_kind;
}

bool validate_render_pass_clear_value(const json &clear, std::string &error)
{
  if (!clear.is_object()) {
    error = "render pass clear value must be an object";
    return false;
  }
  const auto color = clear.find("color");
  if (color == clear.end() || !color->is_array() || color->size() != 4) {
    error = "render pass clear color must be an array of four numbers";
    return false;
  }
  for (const auto &entry : *color) {
    if (!entry.is_number()) {
      error = "render pass clear color entries must be numeric";
      return false;
    }
  }
  return true;
}

bool validate_render_pass_beginning_access(const json &access, std::string &error)
{
  if (!access.is_object()) {
    error = "render pass beginning access must be an object";
    return false;
  }
  if (!access.contains("type")) {
    error = "render pass beginning access missing type";
    return false;
  }
  const auto clear = access.find("clear");
  if (clear != access.end() && !validate_render_pass_clear_value(*clear, error)) {
    return false;
  }
  return true;
}

std::uint64_t json_u64_early(const json &value, std::uint64_t fallback = 0)
{
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    return number < 0 ? fallback : static_cast<std::uint64_t>(number);
  }
  return fallback;
}

std::uint64_t payload_u64_early(const json &payload, std::string_view key, std::uint64_t fallback = 0)
{
  const auto it = payload.find(std::string(key));
  return it == payload.end() ? fallback : json_u64_early(*it, fallback);
}

template <typename Resources>
bool validate_render_pass_ending_access(
    const json &access,
    const Resources &resources,
    std::string &error)
{
  if (!access.is_object()) {
    error = "render pass ending access must be an object";
    return false;
  }
  if (!access.contains("type")) {
    error = "render pass ending access missing type";
    return false;
  }
  const auto src_resource_object_id = payload_u64_early(access, "src_resource_object_id");
  const auto dst_resource_object_id = payload_u64_early(access, "dst_resource_object_id");
  if (src_resource_object_id != 0 && resources.find(src_resource_object_id) == resources.end()) {
    error = "render pass ending access references an unknown source resource";
    return false;
  }
  if (dst_resource_object_id != 0 && resources.find(dst_resource_object_id) == resources.end()) {
    error = "render pass ending access references an unknown destination resource";
    return false;
  }
  const auto subresources = access.find("subresources");
  if (subresources == access.end() || !subresources->is_array()) {
    error = "render pass ending access missing subresources array";
    return false;
  }
  const auto subresource_count = payload_u64_early(access, "subresource_count");
  if (subresources->size() != subresource_count) {
    error = "render pass ending access subresource_count does not match subresources payload";
    return false;
  }
  for (const auto &subresource : *subresources) {
    if (!subresource.is_object()) {
      error = "render pass resolve subresource must be an object";
      return false;
    }
    const auto src_rect = subresource.find("src_rect");
    if (src_rect != subresource.end() && !src_rect->is_null() && !src_rect->is_object()) {
      error = "render pass resolve src_rect must be null or an object";
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

bool is_shader_asset_path(const std::filesystem::path &relative_path)
{
  const auto path = relative_path.generic_string();
  if (!starts_with(path, "shaders/")) {
    return false;
  }
  const auto extension = relative_path.extension().generic_string();
  return extension == ".dxbc" || extension == ".dxil";
}

void collect_referenced_asset_paths(const json &value, std::vector<std::filesystem::path> &paths)
{
  if (value.is_object()) {
    for (const auto &[key, child] : value.items()) {
      if (((key.size() >= 5 && key.compare(key.size() - 5, 5, "_path") == 0) || key == "path") && child.is_string()) {
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
    json *pipeline_out,
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
  if (pipeline_out) {
    *pipeline_out = std::move(pipeline);
  }
  return true;
}

bool read_pipeline_asset_json(
    const std::filesystem::path &bundle_root,
    const std::filesystem::path &relative_path,
    std::string &error)
{
  return read_pipeline_asset_json(bundle_root, relative_path, nullptr, error);
}

std::string pipeline_shader_path(const json &shader, std::string_view stage)
{
  const auto key = std::string(stage) + "_path";
  const auto path = shader.find(key);
  return path != shader.end() && path->is_string() ? path->get<std::string>() : std::string();
}

bool collect_pipeline_shader_asset_paths(
    const json &pipeline,
    std::vector<std::filesystem::path> &shader_paths,
    std::string &error)
{
  shader_paths.clear();
  for (std::string_view stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
    const auto shader = pipeline.find(std::string(stage));
    if (shader == pipeline.end()) {
      continue;
    }
    if (shader->is_null()) {
      continue;
    }
    if (!shader->is_object()) {
      error = "pipeline shader stage must be an object: " + std::string(stage);
      return false;
    }
    const auto bytecode_size = json_u64_early(shader->value("bytecode_size", json(nullptr)));
    if (bytecode_size == 0) {
      error = "pipeline shader stage is missing bytecode_size: " + std::string(stage);
      return false;
    }
    const auto path = pipeline_shader_path(*shader, stage);
    if (path.empty()) {
      error = "pipeline shader stage is missing path: " + std::string(stage);
      return false;
    }
    const std::filesystem::path relative_path(path);
    if (!is_shader_asset_path(relative_path)) {
      error = "pipeline shader stage does not reference a D3D shader asset: " + std::string(stage);
      return false;
    }
    shader_paths.push_back(relative_path);
  }
  return true;
}

bool require_pipeline_bool_field(const json &object, std::string_view field, std::string_view description, std::string &error)
{
  const auto it = object.find(std::string(field));
  if (it == object.end() || !it->is_boolean()) {
    error = std::string("pipeline ") + std::string(description) + " is missing boolean field " + std::string(field);
    return false;
  }
  return true;
}

bool require_pipeline_numeric_field(const json &object, std::string_view field, std::string_view description, std::string &error)
{
  const auto it = object.find(std::string(field));
  if (it == object.end() || (!it->is_number_unsigned() && !it->is_number_integer() && !it->is_number_float())) {
    error = std::string("pipeline ") + std::string(description) + " is missing numeric field " + std::string(field);
    return false;
  }
  return true;
}

bool validate_graphics_pipeline_asset_metadata(const json &pipeline, std::string &error)
{
  const auto blend_state = pipeline.find("blend_state");
  const auto rasterizer_state = pipeline.find("rasterizer_state");
  const auto depth_stencil_state = pipeline.find("depth_stencil_state");
  if (blend_state == pipeline.end() || !blend_state->is_object()) {
    error = "graphics pipeline asset missing blend_state";
    return false;
  }
  if (rasterizer_state == pipeline.end() || !rasterizer_state->is_object()) {
    error = "graphics pipeline asset missing rasterizer_state";
    return false;
  }
  if (depth_stencil_state == pipeline.end() || !depth_stencil_state->is_object()) {
    error = "graphics pipeline asset missing depth_stencil_state";
    return false;
  }

  if (!require_pipeline_bool_field(*blend_state, "alpha_to_coverage_enable", "blend_state", error) ||
      !require_pipeline_bool_field(*blend_state, "independent_blend_enable", "blend_state", error)) {
    return false;
  }
  const auto render_targets = blend_state->find("render_targets");
  if (render_targets == blend_state->end() || !render_targets->is_array()) {
    error = "pipeline blend_state is missing render_targets";
    return false;
  }
  std::size_t render_target_index = 0;
  for (const auto &render_target : *render_targets) {
    if (!render_target.is_object()) {
      error = "pipeline blend_state render target must be an object";
      return false;
    }
    const auto description = "blend_state render target " + std::to_string(render_target_index);
    if (!require_pipeline_bool_field(render_target, "blend_enable", description, error) ||
        !require_pipeline_bool_field(render_target, "logic_op_enable", description, error) ||
        !require_pipeline_numeric_field(render_target, "src_blend", description, error) ||
        !require_pipeline_numeric_field(render_target, "dest_blend", description, error) ||
        !require_pipeline_numeric_field(render_target, "blend_op", description, error) ||
        !require_pipeline_numeric_field(render_target, "src_blend_alpha", description, error) ||
        !require_pipeline_numeric_field(render_target, "dest_blend_alpha", description, error) ||
        !require_pipeline_numeric_field(render_target, "blend_op_alpha", description, error) ||
        !require_pipeline_numeric_field(render_target, "logic_op", description, error) ||
        !require_pipeline_numeric_field(render_target, "render_target_write_mask", description, error)) {
      return false;
    }
    ++render_target_index;
  }

  if (!require_pipeline_numeric_field(*rasterizer_state, "fill_mode", "rasterizer_state", error) ||
      !require_pipeline_numeric_field(*rasterizer_state, "cull_mode", "rasterizer_state", error) ||
      !require_pipeline_bool_field(*rasterizer_state, "front_counter_clockwise", "rasterizer_state", error) ||
      !require_pipeline_numeric_field(*rasterizer_state, "depth_bias", "rasterizer_state", error) ||
      !require_pipeline_numeric_field(*rasterizer_state, "depth_bias_clamp", "rasterizer_state", error) ||
      !require_pipeline_numeric_field(*rasterizer_state, "slope_scaled_depth_bias", "rasterizer_state", error) ||
      !require_pipeline_bool_field(*rasterizer_state, "depth_clip_enable", "rasterizer_state", error) ||
      !require_pipeline_bool_field(*rasterizer_state, "multisample_enable", "rasterizer_state", error) ||
      !require_pipeline_bool_field(*rasterizer_state, "antialiased_line_enable", "rasterizer_state", error) ||
      !require_pipeline_numeric_field(*rasterizer_state, "forced_sample_count", "rasterizer_state", error) ||
      !require_pipeline_numeric_field(*rasterizer_state, "conservative_raster", "rasterizer_state", error)) {
    return false;
  }

  if (!require_pipeline_bool_field(*depth_stencil_state, "depth_enable", "depth_stencil_state", error) ||
      !require_pipeline_numeric_field(*depth_stencil_state, "depth_write_mask", "depth_stencil_state", error) ||
      !require_pipeline_numeric_field(*depth_stencil_state, "depth_func", "depth_stencil_state", error) ||
      !require_pipeline_bool_field(*depth_stencil_state, "stencil_enable", "depth_stencil_state", error) ||
      !require_pipeline_numeric_field(*depth_stencil_state, "stencil_read_mask", "depth_stencil_state", error) ||
      !require_pipeline_numeric_field(*depth_stencil_state, "stencil_write_mask", "depth_stencil_state", error)) {
    return false;
  }
  for (const auto *face_name : {"front_face", "back_face"}) {
    const auto face = depth_stencil_state->find(face_name);
    if (face == depth_stencil_state->end() || !face->is_object()) {
      error = std::string("pipeline depth_stencil_state is missing ") + face_name;
      return false;
    }
    if (!require_pipeline_numeric_field(*face, "stencil_fail_op", face_name, error) ||
        !require_pipeline_numeric_field(*face, "stencil_depth_fail_op", face_name, error) ||
        !require_pipeline_numeric_field(*face, "stencil_pass_op", face_name, error) ||
        !require_pipeline_numeric_field(*face, "stencil_func", face_name, error)) {
      return false;
    }
  }
  return true;
}

bool verify_event_blob_refs_cover_paths(
    const trace::EventRecord &event,
    const std::vector<std::filesystem::path> &paths,
    const std::unordered_map<trace::BlobId, std::filesystem::path> &blob_paths,
    std::string &error)
{
  std::unordered_set<std::string> event_paths;
  event_paths.reserve(event.blob_refs.size());
  for (const auto blob_id : event.blob_refs) {
    if (blob_id == 0) {
      continue;
    }
    const auto blob_path = blob_paths.find(blob_id);
    if (blob_path == blob_paths.end()) {
      error = record_prefix(event) + ": blob_refs contains unknown blob id " + std::to_string(blob_id);
      return false;
    }
    event_paths.insert(blob_path->second.generic_string());
  }

  for (const auto &path : paths) {
    if (event_paths.find(path.generic_string()) == event_paths.end()) {
      error = record_prefix(event) + ": asset path is not covered by blob_refs: " + path.generic_string();
      return false;
    }
  }
  return true;
}

bool read_exact_asset_bytes(
    const std::filesystem::path &path,
    std::uint64_t expected_size,
    std::vector<std::uint8_t> &bytes,
    std::string &error)
{
  if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    error = "asset is too large for replay memory: " + path.generic_string();
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "failed to open asset: " + path.generic_string();
    return false;
  }
  bytes.assign(static_cast<std::size_t>(expected_size), 0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  const auto read_count = static_cast<std::size_t>(input.gcount());
  if (read_count != bytes.size()) {
    // The recorder stores a buffer asset trimmed to its last non-zero byte
    // (trailing zeros are dropped; an all-zero buffer collapses to a tiny
    // representative), while the referencing event keeps the full logical
    // size. A short read therefore means the stored prefix plus the
    // zero-prefilled tail already reconstructs the logical buffer. Only a
    // read longer than expected is a genuine inconsistency.
    if (read_count > bytes.size()) {
      error = "asset is larger than expected: " + path.generic_string();
      return false;
    }
    return true;
  }
  char extra = 0;
  if (input.get(extra)) {
    error = "asset is larger than expected: " + path.generic_string();
    return false;
  }
  return true;
}

bool read_prefix_asset_bytes(
    const std::filesystem::path &path,
    std::uint64_t expected_size,
    std::vector<std::uint8_t> &bytes,
    std::string &error)
{
  if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    error = "asset is too large for replay memory: " + path.generic_string();
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "failed to open asset: " + path.generic_string();
    return false;
  }
  bytes.assign(static_cast<std::size_t>(expected_size), 0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  const auto read_count = static_cast<std::size_t>(input.gcount());
  if (read_count != bytes.size()) {
    // See read_exact_asset_bytes: the recorder trims buffer assets to their
    // last non-zero byte, so a short read is the stored prefix and the
    // zero-prefilled tail reconstructs the logical buffer.
    return true;
  }
  return true;
}

std::uint8_t hex_digit_value(char digit)
{
  if (digit >= '0' && digit <= '9') {
    return static_cast<std::uint8_t>(digit - '0');
  }
  if (digit >= 'a' && digit <= 'f') {
    return static_cast<std::uint8_t>(digit - 'a' + 10);
  }
  if (digit >= 'A' && digit <= 'F') {
    return static_cast<std::uint8_t>(digit - 'A' + 10);
  }
  return 0xff;
}

bool decode_hex_bytes(
    std::string_view encoded,
    std::uint64_t expected_size,
    std::vector<std::uint8_t> &bytes,
    std::string &error)
{
  if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    error = "inline payload is too large for replay memory";
    return false;
  }
  if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 2)) {
    error = "inline payload hex length is too large for replay memory";
    return false;
  }
  if (encoded.size() != static_cast<std::size_t>(expected_size) * 2) {
    error = "inline payload size does not match resolved_size";
    return false;
  }
  bytes.assign(static_cast<std::size_t>(expected_size), 0);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto high = hex_digit_value(encoded[index * 2]);
    const auto low = hex_digit_value(encoded[index * 2 + 1]);
    if (high > 0xf || low > 0xf) {
      error = "inline payload contains a non-hex digit";
      return false;
    }
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

bool is_supported_d3d12_call(std::string_view function_name)
{
  return function_name == "D3D12CreateDevice" ||
         function_name == "ID3D12Device::QueryInterface" ||
         function_name == "IDXGIFactory::CreateSwapChain" ||
         function_name == "ID3D12Device::CreateCommandQueue" ||
         function_name == "ID3D12Device9::CreateCommandQueue1" ||
         function_name == "ID3D12Device::CreateCommandAllocator" ||
         function_name == "ID3D12Device::CreateCommandList" ||
         function_name == "ID3D12Device::CreateCommandList1" ||
         function_name == "ID3D12Device::CreateCommandSignature" ||
         function_name == "ID3D12Device::CreateDescriptorHeap" ||
         function_name == "ID3D12Device::CreateQueryHeap" ||
         function_name == "ID3D12Device::CreateRootSignature" ||
         function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
         function_name == "ID3D12Device::CreateComputePipelineState" ||
         function_name == "ID3D12Device2::CreatePipelineState" ||
         function_name == "ID3D12Device6::SetBackgroundProcessingMode" ||
         function_name == "ID3D12Device7::AddToStateObject" ||
         function_name == "ID3D12Device7::CreateProtectedResourceSession1" ||
         function_name == "ID3D12Device::CreateCommittedResource" ||
         function_name == "ID3D12Device8::CreateCommittedResource2" ||
         function_name == "ID3D12Device::CreateReservedResource" ||
         function_name == "ID3D12Device::CreateHeap" ||
         function_name == "ID3D12Device::OpenExistingHeap" ||
         function_name == "ID3D12Device3::OpenExistingHeapFromAddress" ||
         function_name == "ID3D12Device4::CreateHeap1" ||
         function_name == "ID3D12Device::CreatePlacedResource" ||
         function_name == "ID3D12Device8::CreatePlacedResource1" ||
         function_name == "ID3D12Device9::CreateShaderCacheSession" ||
         function_name == "ID3D12Device9::ShaderCacheControl" ||
         function_name == "ID3D12Device::GetResourceTiling" ||
         function_name == "ID3D12Device::CreateFence" ||
         function_name == "ID3D12Device::CreateConstantBufferView" ||
         function_name == "ID3D12Device::CreateShaderResourceView" ||
         function_name == "ID3D12Device::CreateUnorderedAccessView" ||
	         function_name == "ID3D12Device::CreateRenderTargetView" ||
	         function_name == "ID3D12Device::CreateDepthStencilView" ||
	         function_name == "ID3D12Device::CreateDescriptorViewBatch" ||
	         function_name == "ID3D12Device::CreateSampler" ||
	         function_name == "ID3D12Device::CopyDescriptorsBatch" ||
	         function_name == "ID3D12Device::CopyDescriptors" ||
	         function_name == "ID3D12Device::CopyDescriptorsSimple" ||
	         function_name == "DXMT::FenceDependencyBatch" ||
	         function_name == "DXMT::SparseTextureMappingOps" ||
	         function_name == "ID3D12CommandAllocator::Reset" ||
         function_name == "ID3D12CommandQueue::ExecuteCommandLists" ||
         function_name == "ID3D12CommandQueue::UpdateTileMappings" ||
         function_name == "ID3D12CommandQueue::Signal" ||
         function_name == "ID3D12CommandQueue::Wait" ||
         function_name == "ID3D12GraphicsCommandList::QueryInterface" ||
         function_name == "ID3D12GraphicsCommandList::Close" ||
         function_name == "ID3D12GraphicsCommandList::Reset" ||
         function_name == "ID3D12GraphicsCommandList::SetPipelineState" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRootSignature" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRootSignature" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView" ||
         function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView" ||
         function_name == "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView" ||
         function_name == "ID3D12GraphicsCommandList::RSSetViewports" ||
         function_name == "ID3D12GraphicsCommandList::RSSetScissorRects" ||
         function_name == "ID3D12GraphicsCommandList::ClearState" ||
         function_name == "ID3D12GraphicsCommandList::OMSetBlendFactor" ||
         function_name == "ID3D12GraphicsCommandList::OMSetStencilRef" ||
         function_name == "ID3D12GraphicsCommandList::OMSetRenderTargets" ||
         function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView" ||
         function_name == "ID3D12GraphicsCommandList::ClearDepthStencilView" ||
         function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint" ||
         function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat" ||
         function_name == "ID3D12GraphicsCommandList::DiscardResource" ||
         function_name == "ID3D12GraphicsCommandList::IASetPrimitiveTopology" ||
         function_name == "ID3D12GraphicsCommandList::IASetVertexBuffers" ||
         function_name == "ID3D12GraphicsCommandList::IASetIndexBuffer" ||
         function_name == "ID3D12GraphicsCommandList::ResourceBarrier" ||
         function_name == "ID3D12GraphicsCommandList::ResourceBarrierBatch" ||
         function_name == "ID3D12GraphicsCommandList::SetDescriptorHeaps" ||
         function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
         function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced" ||
         function_name == "ID3D12GraphicsCommandList::Dispatch" ||
         function_name == "ID3D12GraphicsCommandList::ExecuteIndirect" ||
         function_name == "ID3D12GraphicsCommandList::ExecuteBundle" ||
         function_name == "ID3D12GraphicsCommandList::CopyBufferRegion" ||
         function_name == "ID3D12GraphicsCommandList::CopyBufferRegionBatch" ||
         function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" ||
         function_name == "ID3D12GraphicsCommandList::CopyTextureRegionBatch" ||
         function_name == "ID3D12GraphicsCommandList::CopyResource" ||
         function_name == "ID3D12GraphicsCommandList::ResolveSubresource" ||
         function_name == "ID3D12GraphicsCommandList::BeginQuery" ||
         function_name == "ID3D12GraphicsCommandList::EndQuery" ||
         function_name == "ID3D12GraphicsCommandList::ResolveQueryData" ||
         function_name == "ID3D12GraphicsCommandList::ResolveQueryDataResult" ||
         function_name == "ID3D12GraphicsCommandList::SetPredication" ||
         function_name == "ID3D12GraphicsCommandList::ResolveSubresourceRegion" ||
         function_name == "ID3D12GraphicsCommandList2::WriteBufferImmediate" ||
         function_name == "ID3D12GraphicsCommandList4::BeginRenderPass" ||
         function_name == "ID3D12GraphicsCommandList4::EndRenderPass" ||
         function_name == "ID3D12GraphicsCommandListExt::TemporalUpscale" ||
         function_name == "ID3D12GraphicsCommandList4::DispatchRays" ||
         function_name == "ID3D12GraphicsCommandList6::DispatchMesh" ||
         function_name == "ID3D12Resource::Map" ||
         function_name == "ID3D12Resource::Unmap" ||
         function_name == "IDXGISwapChain::Present" ||
         function_name == "ID3D12Fence::SetEventOnCompletion" ||
         function_name == "ID3D12Fence::GetCompletedValue" ||
         function_name == "ID3D12Fence::Signal";
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

std::uint64_t json_u64(const json &value, std::uint64_t fallback = 0)
{
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    return number < 0 ? fallback : static_cast<std::uint64_t>(number);
  }
  return fallback;
}

std::uint32_t payload_u32(const json &payload, std::string_view key, std::uint32_t fallback = 0)
{
  const auto value = payload_u64(payload, key, fallback);
  return value > std::numeric_limits<std::uint32_t>::max() ? fallback : static_cast<std::uint32_t>(value);
}

std::int32_t payload_i32(const json &payload, std::string_view key, std::int32_t fallback = 0)
{
  const auto it = payload.find(std::string(key));
  if (it == payload.end()) {
    return fallback;
  }
  if (it->is_number_integer()) {
    const auto value = it->get<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
      return fallback;
    }
    return static_cast<std::int32_t>(value);
  }
  if (it->is_number_unsigned()) {
    const auto value = it->get<std::uint64_t>();
    return value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
               ? fallback
               : static_cast<std::int32_t>(value);
  }
  return fallback;
}

float payload_float(const json &payload, std::string_view key, float fallback = 0.0f)
{
  const auto it = payload.find(std::string(key));
  return it != payload.end() && it->is_number() ? it->get<float>() : fallback;
}

bool payload_bool(const json &payload, std::string_view key, bool fallback = false)
{
  const auto it = payload.find(std::string(key));
  return it != payload.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

trace::ObjectId json_object_id(const json &value)
{
  if (value.is_null()) {
    return 0;
  }
  if (value.is_number_unsigned()) {
    return value.get<trace::ObjectId>();
  }
  if (value.is_number_integer()) {
    const auto object_id = value.get<std::int64_t>();
    return object_id < 0 ? trace::ObjectId{} : static_cast<trace::ObjectId>(object_id);
  }
  return 0;
}

std::string payload_string(const json &payload, std::string_view key)
{
  const auto it = payload.find(std::string(key));
  return it != payload.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool payload_has(const json &payload, std::string_view key)
{
  return payload.find(std::string(key)) != payload.end();
}

std::uint32_t dxgi_format_element_size(std::uint32_t format)
{
  switch (format) {
  case 1:
  case 2:
  case 3:
  case 4:
    return 16;
  case 5:
  case 6:
  case 7:
  case 8:
  case 9:
  case 10:
  case 11:
  case 12:
  case 13:
  case 14:
  case 15:
  case 16:
  case 17:
  case 18:
  case 19:
  case 20:
  case 21:
  case 22:
  case 23:
  case 24:
  case 25:
  case 26:
    return 8;
  case 27:
  case 28:
  case 29:
  case 30:
  case 31:
  case 32:
  case 33:
  case 34:
  case 35:
  case 36:
  case 37:
  case 38:
  case 39:
  case 40:
  case 41:
  case 42:
  case 43:
  case 44:
  case 45:
  case 46:
  case 47:
  case 48:
  case 49:
  case 50:
  case 51:
  case 52:
  case 67:
  case 68:
  case 69:
  case 85:
  case 86:
  case 87:
  case 88:
  case 89:
  case 90:
  case 91:
  case 92:
  case 93:
  case 100:
  case 101:
    return 4;
  case 53:
  case 54:
  case 55:
  case 56:
  case 57:
  case 58:
  case 59:
  case 112:
  case 113:
  case 115:
    return 2;
  case 60:
  case 61:
  case 62:
  case 63:
  case 64:
  case 65:
  case 66:
  case 111:
    return 1;
  default:
    return 0;
  }
}

template <typename RectState>
bool parse_rect_array_payload(
    const trace::EventRecord &event,
    const json &payload,
    std::string_view function_label,
    std::uint32_t rect_count,
    std::vector<RectState> &rects,
    std::string &error)
{
  rects.clear();
  const auto rect_array = payload.find("rects");
  if (rect_array != payload.end()) {
    if (!rect_array->is_array() || rect_array->size() != rect_count) {
      error = record_prefix(event) + ": " + std::string(function_label) + " rects count does not match rect_count";
      return false;
    }
    for (const auto &rect_payload : *rect_array) {
      if (!rect_payload.is_object()) {
        error = record_prefix(event) + ": " + std::string(function_label) + " rect entries must be objects";
        return false;
      }
      RectState rect;
      rect.left = payload_i32(rect_payload, "left");
      rect.top = payload_i32(rect_payload, "top");
      rect.right = payload_i32(rect_payload, "right");
      rect.bottom = payload_i32(rect_payload, "bottom");
      rects.push_back(rect);
    }
    return true;
  }

  if (rect_count == 0) {
    return true;
  }

  const auto first = payload.find("first");
  const auto first_rect = first != payload.end() ? first : payload.find("first_rect");
  if (first_rect == payload.end() || !first_rect->is_object()) {
    error = record_prefix(event) + ": " + std::string(function_label) + " missing rect payload";
    return false;
  }
  RectState rect;
  rect.left = payload_i32(*first_rect, "left");
  rect.top = payload_i32(*first_rect, "top");
  rect.right = payload_i32(*first_rect, "right");
  rect.bottom = payload_i32(*first_rect, "bottom");
  rects.push_back(rect);
  return true;
}

bool require_payload_key(
    const trace::EventRecord &event,
    const json &payload,
    std::string_view key,
    std::string &error)
{
  if (payload_has(payload, key)) {
    return true;
  }
  error = record_prefix(event) + ": missing payload key " + std::string(key);
  return false;
}

bool command_signature_argument_uses_root_parameter(std::uint32_t type)
{
  constexpr std::uint32_t indirect_argument_type_constant = 5;
  constexpr std::uint32_t indirect_argument_type_constant_buffer_view = 6;
  constexpr std::uint32_t indirect_argument_type_shader_resource_view = 7;
  constexpr std::uint32_t indirect_argument_type_unordered_access_view = 8;
  return type == indirect_argument_type_constant ||
         type == indirect_argument_type_constant_buffer_view ||
         type == indirect_argument_type_shader_resource_view ||
         type == indirect_argument_type_unordered_access_view;
}

bool command_signature_argument_executes_draw(std::uint32_t type)
{
  constexpr std::uint32_t indirect_argument_type_draw = 0;
  constexpr std::uint32_t indirect_argument_type_draw_indexed = 1;
  return type == indirect_argument_type_draw ||
         type == indirect_argument_type_draw_indexed;
}

bool command_signature_argument_executes_dispatch(std::uint32_t type)
{
  constexpr std::uint32_t indirect_argument_type_dispatch = 2;
  return type == indirect_argument_type_dispatch;
}

bool descriptor_heap_type_allows_shader_visibility(std::uint32_t heap_type)
{
  constexpr std::uint32_t cbv_srv_uav_heap = 0;
  constexpr std::uint32_t sampler_heap = 1;
  return heap_type == cbv_srv_uav_heap || heap_type == sampler_heap;
}

bool descriptor_heap_type_is_shader_visible(std::uint32_t flags)
{
  constexpr std::uint32_t shader_visible = 0x1;
  return (flags & shader_visible) != 0;
}

std::uint32_t normalized_resource_mip_levels(std::uint64_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t mip_levels)
{
  if (mip_levels != 0) {
    return mip_levels;
  }

  std::uint64_t max_dimension = std::max<std::uint64_t>(width, std::max<std::uint32_t>(height, depth));
  std::uint32_t levels = 1;
  while (max_dimension > 1) {
    max_dimension >>= 1;
    ++levels;
  }
  return levels;
}

std::uint64_t resource_subresource_count(const D3D12ReplayBackend::ResourceSemanticState &resource)
{
  return static_cast<std::uint64_t>(resource.mip_levels) *
         static_cast<std::uint64_t>(resource.depth_or_array_size);
}

const char *descriptor_kind_for_root_range_type(std::uint32_t type)
{
  switch (type) {
  case 0:
    return "ShaderResourceView";
  case 1:
    return "UnorderedAccessView";
  case 2:
    return "ConstantBufferView";
  case 3:
    return "Sampler";
  default:
    return nullptr;
  }
}

std::string descriptor_binding_key(const D3D12ReplayBackend::DescriptorBinding &binding)
{
  return std::to_string(binding.heap_object_id) + ":" + std::to_string(binding.descriptor_index);
}

D3D12ReplayBackend::ReplayCommandKind replay_command_kind_for(std::string_view function_name)
{
  using Kind = D3D12ReplayBackend::ReplayCommandKind;
  if (function_name == "ID3D12Device::CreateCommandList" ||
      function_name == "ID3D12GraphicsCommandList::Reset") {
    return Kind::BeginCommandList;
  }
  if (function_name == "ID3D12GraphicsCommandList::Close") {
    return Kind::EndCommandList;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetPipelineState") {
    return Kind::SetPipelineState;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootSignature" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRootSignature") {
    return Kind::SetRootSignature;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetDescriptorHeaps") {
    return Kind::SetDescriptorHeaps;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable") {
    return Kind::SetRootDescriptorTable;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant" ||
      function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants") {
    return Kind::SetRootConstants;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView" ||
      function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView" ||
      function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView" ||
      function_name == "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView") {
    return Kind::SetRootConstantBufferView;
  }
  if (function_name == "ID3D12GraphicsCommandList::RSSetViewports") {
    return Kind::SetViewports;
  }
  if (function_name == "ID3D12GraphicsCommandList::RSSetScissorRects") {
    return Kind::SetScissorRects;
  }
  if (function_name == "ID3D12GraphicsCommandList::OMSetRenderTargets") {
    return Kind::SetRenderTargets;
  }
  if (function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView") {
    return Kind::ClearRenderTarget;
  }
  if (function_name == "ID3D12GraphicsCommandList::ClearDepthStencilView") {
    return Kind::ClearDepthStencil;
  }
  if (function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint" ||
      function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat") {
    return Kind::ClearUnorderedAccess;
  }
  if (function_name == "ID3D12GraphicsCommandList::DiscardResource") {
    return Kind::DiscardResource;
  }
  if (function_name == "ID3D12GraphicsCommandList::IASetPrimitiveTopology") {
    return Kind::SetPrimitiveTopology;
  }
  if (function_name == "ID3D12GraphicsCommandList::IASetVertexBuffers") {
    return Kind::SetVertexBuffers;
  }
  if (function_name == "ID3D12GraphicsCommandList::IASetIndexBuffer") {
    return Kind::SetIndexBuffer;
  }
  if (function_name == "ID3D12GraphicsCommandList::ResourceBarrier" ||
      function_name == "ID3D12GraphicsCommandList::ResourceBarrierBatch") {
    return Kind::ResourceBarrier;
  }
  if (function_name == "ID3D12GraphicsCommandList::ClearState" ||
      function_name == "ID3D12GraphicsCommandList::OMSetBlendFactor" ||
      function_name == "ID3D12GraphicsCommandList::OMSetStencilRef") {
    return Kind::SetDynamicState;
  }
  if (function_name == "ID3D12GraphicsCommandList4::BeginRenderPass" ||
      function_name == "ID3D12GraphicsCommandList4::EndRenderPass") {
    return Kind::RenderPass;
  }
  if (function_name == "ID3D12GraphicsCommandList::BeginQuery" ||
      function_name == "ID3D12GraphicsCommandList::EndQuery" ||
      function_name == "ID3D12GraphicsCommandList::ResolveQueryData") {
    return Kind::Query;
  }
  if (function_name == "ID3D12GraphicsCommandList::SetPredication") {
    return Kind::Predication;
  }
  if (function_name == "ID3D12GraphicsCommandList2::WriteBufferImmediate") {
    return Kind::WriteBufferImmediate;
  }
  if (function_name == "ID3D12GraphicsCommandListExt::TemporalUpscale") {
    return Kind::TemporalUpscale;
  }
  if (function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
      function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced") {
    return Kind::Draw;
  }
  if (function_name == "ID3D12GraphicsCommandList::Dispatch" ||
      function_name == "ID3D12GraphicsCommandList4::DispatchRays" ||
      function_name == "ID3D12GraphicsCommandList6::DispatchMesh") {
    return Kind::Dispatch;
  }
  if (function_name == "ID3D12GraphicsCommandList::ExecuteIndirect") {
    return Kind::ExecuteIndirect;
  }
  if (function_name == "ID3D12GraphicsCommandList::ExecuteBundle") {
    return Kind::ExecuteBundle;
  }
  if (function_name == "ID3D12GraphicsCommandList::CopyBufferRegion" ||
      function_name == "ID3D12GraphicsCommandList::CopyBufferRegionBatch" ||
      function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" ||
      function_name == "ID3D12GraphicsCommandList::CopyTextureRegionBatch" ||
      function_name == "ID3D12GraphicsCommandList::CopyResource") {
    return Kind::Copy;
  }
  if (function_name == "ID3D12GraphicsCommandList::ResolveSubresource" ||
      function_name == "ID3D12GraphicsCommandList::ResolveSubresourceRegion") {
    return Kind::Resolve;
  }
  if (function_name == "ID3D12Resource::Map") {
    return Kind::MapResource;
  }
  if (function_name == "ID3D12Resource::Unmap") {
    return Kind::UnmapResource;
  }
  return Kind::Unknown;
}

bool replay_command_requires_submission(D3D12ReplayBackend::ReplayCommandKind kind)
{
  using Kind = D3D12ReplayBackend::ReplayCommandKind;
  switch (kind) {
  case Kind::ClearRenderTarget:
  case Kind::ClearDepthStencil:
  case Kind::ClearUnorderedAccess:
  case Kind::DiscardResource:
  case Kind::ResourceBarrier:
  case Kind::SetDynamicState:
  case Kind::RenderPass:
  case Kind::Query:
  case Kind::Predication:
  case Kind::WriteBufferImmediate:
  case Kind::TemporalUpscale:
  case Kind::UnsupportedNative:
  case Kind::Draw:
  case Kind::Dispatch:
  case Kind::ExecuteIndirect:
  case Kind::ExecuteBundle:
  case Kind::Copy:
  case Kind::Resolve:
    return true;
  default:
    return false;
  }
}

bool d3d12_native_replay_enabled()
{
  return env_enabled("APITRACE_D3D12_NATIVE_REPLAY", true);
}

struct DxgiFormatFootprintLayout {
  std::uint32_t block_width = 1;
  std::uint32_t block_height = 1;
  std::uint32_t bytes_per_block = 0;
};

DxgiFormatFootprintLayout dxgi_format_footprint_layout(std::uint32_t format)
{
  // Numeric DXGI_FORMAT values keep this helper usable in non-Windows builds.
  switch (format) {
  case 1:  // R32G32B32A32
  case 2:
  case 3:
  case 4:
    return {1, 1, 16};
  case 5:  // R32G32B32
  case 6:
  case 7:
  case 8:
    return {1, 1, 12};
  case 9:  // 64-bit four/two-channel and packed depth-stencil formats
  case 10:
  case 11:
  case 12:
  case 13:
  case 14:
  case 15:
  case 16:
  case 17:
  case 18:
  case 19:
  case 20:
  case 21:
  case 22:
  case 102:  // Y416
    return {1, 1, 8};
  case 23:  // 32-bit color/depth formats
  case 24:
  case 25:
  case 26:
  case 27:
  case 28:
  case 29:
  case 30:
  case 31:
  case 32:
  case 33:
  case 34:
  case 35:
  case 36:
  case 37:
  case 38:
  case 39:
  case 40:
  case 41:
  case 42:
  case 43:
  case 44:
  case 45:
  case 46:
  case 47:
  case 67:
  case 89:
  case 100:  // AYUV
  case 101:  // Y410
    return {1, 1, 4};
  case 48:  // 16-bit two/single-channel formats
  case 49:
  case 50:
  case 51:
  case 52:
  case 53:
  case 54:
  case 55:
  case 56:
  case 57:
  case 58:
  case 59:
  case 85:
  case 86:
    return {1, 1, 2};
  case 60:  // 8-bit single-channel formats
  case 61:
  case 62:
  case 63:
  case 64:
  case 65:
    return {1, 1, 1};
  case 66:  // R1_UNORM
    return {8, 1, 1};
  case 68:  // R8G8_B8G8/G8R8_G8B8 packed pairs
  case 69:
    return {2, 1, 4};
  case 70:  // BC1
  case 71:
  case 72:
  case 79:  // BC4
  case 80:
  case 81:
    return {4, 4, 8};
  case 73:  // BC2
  case 74:
  case 75:
  case 76:  // BC3
  case 77:
  case 78:
  case 82:  // BC5
  case 83:
  case 84:
  case 94:  // BC6H
  case 95:
  case 96:
  case 97:  // BC7
  case 98:
  case 99:
    return {4, 4, 16};
  case 87:  // B8G8R8A8/B8G8R8X8 families
  case 88:
  case 90:
  case 91:
  case 92:
  case 93:
    return {1, 1, 4};
  default:
    return {};
  }
}

std::uint64_t placed_footprint_active_bytes(
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    std::uint32_t row_pitch)
{
  if (width == 0 || height == 0 || depth == 0 || row_pitch == 0) {
    return 0;
  }

  const auto layout = dxgi_format_footprint_layout(format);
  const std::uint64_t rows = layout.bytes_per_block == 0
                                 ? static_cast<std::uint64_t>(height)
                                 : (static_cast<std::uint64_t>(height) + layout.block_height - 1) /
                                       layout.block_height;
  if (layout.bytes_per_block == 0) {
    return static_cast<std::uint64_t>(row_pitch) * rows * depth;
  }

  const std::uint64_t blocks_per_row =
      (static_cast<std::uint64_t>(width) + layout.block_width - 1) / layout.block_width;
  const std::uint64_t row_size = blocks_per_row * layout.bytes_per_block;
  if (row_size > row_pitch) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  const std::uint64_t total_rows = rows * static_cast<std::uint64_t>(depth);
  return total_rows == 0 ? 0 : static_cast<std::uint64_t>(row_pitch) * (total_rows - 1) + row_size;
}

bool d3d12_retrace_present_frame_capture_enabled()
{
  return env_enabled("APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES", false);
}

} // namespace

#include "d3d12_replay/native_replayer.inc"
#include "d3d12_replay/backend_methods.inc"
#include "d3d12_replay/replay_model_io.inc"

} // namespace apitrace::d3d12
