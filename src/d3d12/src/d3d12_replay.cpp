#include "apitrace/d3d12_replay.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
    error = "asset is smaller than expected: " + path.generic_string();
    return false;
  }
  char extra = 0;
  if (input.get(extra)) {
    error = "asset is larger than expected: " + path.generic_string();
    return false;
  }
  return true;
}

bool is_supported_d3d12_call(std::string_view function_name)
{
  return function_name == "D3D12CreateDevice" ||
         function_name == "ID3D12Device::QueryInterface" ||
         function_name == "ID3D12Device::CreateCommandQueue" ||
         function_name == "ID3D12Device::CreateCommandAllocator" ||
         function_name == "ID3D12Device::CreateCommandList" ||
         function_name == "ID3D12Device::CreateCommandSignature" ||
         function_name == "ID3D12Device::CreateDescriptorHeap" ||
         function_name == "ID3D12Device::CreateRootSignature" ||
         function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
         function_name == "ID3D12Device::CreateComputePipelineState" ||
         function_name == "ID3D12Device::CreateCommittedResource" ||
         function_name == "ID3D12Device::CreateFence" ||
         function_name == "ID3D12Device::CreateConstantBufferView" ||
         function_name == "ID3D12Device::CreateShaderResourceView" ||
         function_name == "ID3D12Device::CreateUnorderedAccessView" ||
         function_name == "ID3D12Device::CreateRenderTargetView" ||
         function_name == "ID3D12Device::CreateDepthStencilView" ||
         function_name == "ID3D12CommandAllocator::Reset" ||
         function_name == "ID3D12CommandQueue::ExecuteCommandLists" ||
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
         function_name == "ID3D12GraphicsCommandList::OMSetRenderTargets" ||
         function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView" ||
         function_name == "ID3D12GraphicsCommandList::ClearDepthStencilView" ||
         function_name == "ID3D12GraphicsCommandList::IASetPrimitiveTopology" ||
         function_name == "ID3D12GraphicsCommandList::IASetVertexBuffers" ||
         function_name == "ID3D12GraphicsCommandList::IASetIndexBuffer" ||
         function_name == "ID3D12GraphicsCommandList::ResourceBarrier" ||
         function_name == "ID3D12GraphicsCommandList::SetDescriptorHeaps" ||
         function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
         function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced" ||
         function_name == "ID3D12GraphicsCommandList::Dispatch" ||
         function_name == "ID3D12GraphicsCommandList::ExecuteIndirect" ||
         function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" ||
         function_name == "ID3D12GraphicsCommandList::CopyResource" ||
         function_name == "ID3D12GraphicsCommandList::ResolveSubresource" ||
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
  if (function_name == "ID3D12GraphicsCommandList::IASetPrimitiveTopology") {
    return Kind::SetPrimitiveTopology;
  }
  if (function_name == "ID3D12GraphicsCommandList::IASetVertexBuffers") {
    return Kind::SetVertexBuffers;
  }
  if (function_name == "ID3D12GraphicsCommandList::IASetIndexBuffer") {
    return Kind::SetIndexBuffer;
  }
  if (function_name == "ID3D12GraphicsCommandList::ResourceBarrier") {
    return Kind::ResourceBarrier;
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
  if (function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" ||
      function_name == "ID3D12GraphicsCommandList::CopyResource") {
    return Kind::Copy;
  }
  if (function_name == "ID3D12GraphicsCommandList::ResolveSubresource") {
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
  case Kind::ResourceBarrier:
  case Kind::Draw:
  case Kind::Dispatch:
  case Kind::ExecuteIndirect:
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

bool d3d12_retrace_present_frame_capture_enabled()
{
  return env_enabled("APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES", false);
}

} // namespace

#if defined(APITRACE_HAS_D3D_NATIVE)

using RecordPresentFrameFn = void(WINAPI *)(UINT, UINT, UINT, UINT, UINT, const void *, SIZE_T);
using RecordPresentSemanticsFn = void(WINAPI *)(UINT, UINT, HRESULT);
using CaptureSuppressionFn = void(WINAPI *)();

#ifdef _WIN32
template <typename Fn>
Fn resolve_d3d12_export(const char *name)
{
  HMODULE module = GetModuleHandleA("d3d12.dll");
  if (!module) {
    return nullptr;
  }
  return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

class ScopedD3D12CaptureSuppression {
public:
  ScopedD3D12CaptureSuppression()
      : begin_(resolve_d3d12_export<CaptureSuppressionFn>("apitrace_d3d12_begin_capture_suppression")),
        end_(resolve_d3d12_export<CaptureSuppressionFn>("apitrace_d3d12_end_capture_suppression"))
  {
    if (begin_ && end_) {
      begin_();
      active_ = true;
    }
  }

  ScopedD3D12CaptureSuppression(const ScopedD3D12CaptureSuppression &) = delete;
  ScopedD3D12CaptureSuppression &operator=(const ScopedD3D12CaptureSuppression &) = delete;

  ~ScopedD3D12CaptureSuppression()
  {
    if (active_) {
      end_();
    }
  }

private:
  CaptureSuppressionFn begin_ = nullptr;
  CaptureSuppressionFn end_ = nullptr;
  bool active_ = false;
};
#else
template <typename Fn>
Fn resolve_d3d12_export(const char *)
{
  return nullptr;
}

class ScopedD3D12CaptureSuppression {
public:
  ScopedD3D12CaptureSuppression() = default;
};
#endif

class D3D12NativeReplayer {
public:
  explicit D3D12NativeReplayer(const D3D12ReplayBackend &backend) : backend_(backend) {}

  ~D3D12NativeReplayer()
  {
    shutdown();
  }

  bool replay(std::string &error)
  {
    if (!d3d12_native_replay_enabled()) {
      error = "D3D12 native command replay incomplete";
      return false;
    }
    if (!bootstrap(error) ||
        !create_resources(error) ||
        !create_descriptor_heaps(error) ||
        !create_root_signatures(error) ||
        !create_pipelines(error) ||
        !create_command_signatures(error) ||
        !prepare_chronological_state(error) ||
        !create_command_objects(error) ||
        !replay_submissions(error)) {
      return false;
    }
    wait_for_gpu();
    return true;
  }

private:
  struct NativeResource {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool swapchain_back_buffer = false;
  };

  struct NativeCommandList {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  };

  struct ReadbackCommandObjects {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
  };

  struct NativeDescriptorHeap {
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    UINT descriptor_size = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
  };

  bool fail(std::string &error, std::string message) const
  {
    error = "D3D12 native command replay incomplete: " + std::move(message);
    return false;
  }

  void shutdown()
  {
    wait_for_gpu();
#ifdef _WIN32
    if (fence_event_) {
      CloseHandle(fence_event_);
      fence_event_ = nullptr;
    }
#endif
    command_lists_.clear();
    command_signatures_.clear();
    pipelines_.clear();
    root_signatures_.clear();
    descriptor_heaps_.clear();
    resources_.clear();
    back_buffers_[0].reset();
    back_buffers_[1].reset();
    fence_.reset();
    swap_chain_.reset();
    queue_.reset();
    device_.reset();
#if !defined(_WIN32)
    if (capture_writer_) {
      capture_writer_->close();
      capture_writer_.reset();
    }
#endif
#ifdef _WIN32
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
#elif defined(__APPLE__)
    hwnd_ = nullptr;
    apitrace::platform::macos::destroy_window(window_handles_);
#endif
  }

  bool infer_window_size(UINT &width, UINT &height) const
  {
    for (const auto &[object_id, resource] : backend_.resources_) {
      (void)object_id;
      if (!resource.swapchain_back_buffer) {
        continue;
      }
      width = static_cast<UINT>(resource.width);
      height = resource.height;
      return width != 0 && height != 0;
    }
    return false;
  }

  bool bootstrap(std::string &error)
  {
    UINT width = 0;
    UINT height = 0;
    if (!infer_window_size(width, height)) {
      return fail(error, "trace is missing swapchain back-buffer semantics");
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateDXGIFactory1", hr);
      return false;
    }

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    for (const auto &[object_id, device] : backend_.devices_) {
      (void)object_id;
      feature_level = static_cast<D3D_FEATURE_LEVEL>(device.minimum_feature_level);
      break;
    }
    hr = D3D12CreateDevice(nullptr, feature_level, IID_PPV_ARGS(device_.put()));
    if (FAILED(hr)) {
      error = hresult_error("D3D12CreateDevice", hr);
      return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue_.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateCommandQueue", hr);
      return false;
    }

#ifdef _WIN32
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = replay_window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "apitrace_d3d12_native_retrace_window";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return fail(error, "RegisterClassA failed");
    }

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExA(
        0,
        wc.lpszClassName,
        "apitrace D3D12 native retrace",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd_) {
      return fail(error, "CreateWindowExA failed");
    }
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
#elif defined(__APPLE__)
    apitrace::platform::macos::WindowSpec spec;
    spec.width = width;
    spec.height = height;
    spec.title = "apitrace D3D12 native retrace";
    spec.show = true;
    if (!apitrace::platform::macos::create_window(spec, window_handles_, error)) {
      return fail(error, error.empty() ? "failed to create native macOS replay window" : error);
    }
    hwnd_ = static_cast<HWND>(window_handles_.nswindow);
    if (!hwnd_) {
      return fail(error, "native macOS replay window did not produce an HWND token");
    }
#else
    return fail(error, "D3D12 native replay has no window bootstrap for this platform");
#endif

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.Width = width;
    swap_chain_desc.Height = height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = kBufferCount;
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap_chain1;
    hr = factory->CreateSwapChainForHwnd(queue_.get(), hwnd_, &swap_chain_desc, nullptr, nullptr, swap_chain1.put());
    if (FAILED(hr)) {
      error = hresult_error("CreateSwapChainForHwnd", hr);
      return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    hr = swap_chain1->QueryInterface(IID_PPV_ARGS(swap_chain_.put()));
    if (FAILED(hr)) {
      error = hresult_error("QueryInterface(IDXGISwapChain3)", hr);
      return false;
    }
    for (UINT index = 0; index < kBufferCount; ++index) {
      hr = swap_chain_->GetBuffer(index, IID_PPV_ARGS(back_buffers_[index].put()));
      if (FAILED(hr)) {
        error = hresult_error("IDXGISwapChain::GetBuffer", hr);
        return false;
      }
    }

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateFence", hr);
      return false;
    }
#ifdef _WIN32
    fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      return fail(error, "CreateEventA failed");
    }
#endif
    return true;
  }

  static D3D12_HEAP_PROPERTIES heap_properties(std::uint32_t heap_type)
  {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = static_cast<D3D12_HEAP_TYPE>(heap_type);
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
  }

  static D3D12_RESOURCE_DESC resource_desc(const D3D12ReplayBackend::ResourceSemanticState &resource)
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(resource.dimension);
    desc.Alignment = resource.alignment;
    desc.Width = resource.width;
    desc.Height = resource.height;
    desc.DepthOrArraySize = static_cast<UINT16>(resource.depth_or_array_size);
    desc.MipLevels = static_cast<UINT16>(resource.mip_levels);
    desc.Format = static_cast<DXGI_FORMAT>(resource.format);
    desc.SampleDesc.Count = resource.sample_count;
    desc.SampleDesc.Quality = resource.sample_quality;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(resource.layout);
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(resource.flags);
    return desc;
  }

  static D3D12_CLEAR_VALUE clear_value(const D3D12ReplayBackend::ResourceSemanticState &resource)
  {
    D3D12_CLEAR_VALUE value{};
    value.Format = static_cast<DXGI_FORMAT>(resource.optimized_clear_format);
    value.Color[0] = resource.optimized_clear_color[0];
    value.Color[1] = resource.optimized_clear_color[1];
    value.Color[2] = resource.optimized_clear_color[2];
    value.Color[3] = resource.optimized_clear_color[3];
    value.DepthStencil.Depth = resource.optimized_clear_depth;
    value.DepthStencil.Stencil = static_cast<UINT8>(resource.optimized_clear_stencil);
    return value;
  }

  bool create_resources(std::string &error)
  {
    for (const auto &[object_id, resource] : backend_.resources_) {
      NativeResource native;
      native.state = static_cast<D3D12_RESOURCE_STATES>(resource.initial_state);
      native.swapchain_back_buffer = resource.swapchain_back_buffer;
      if (resource.swapchain_back_buffer) {
        if (resource.swapchain_buffer_index >= kBufferCount || !back_buffers_[resource.swapchain_buffer_index]) {
          return fail(error, "swapchain back-buffer index is out of range");
        }
        back_buffers_[resource.swapchain_buffer_index]->AddRef();
        native.resource.reset(back_buffers_[resource.swapchain_buffer_index].get());
        resources_.emplace(object_id, std::move(native));
        continue;
      }

      const auto heap = heap_properties(resource.heap_type);
      const auto desc = resource_desc(resource);
      const auto optimized_clear = resource.has_optimized_clear_value ? clear_value(resource) : D3D12_CLEAR_VALUE{};
      HRESULT hr = device_->CreateCommittedResource(
          &heap,
          static_cast<D3D12_HEAP_FLAGS>(resource.heap_flags),
          &desc,
          static_cast<D3D12_RESOURCE_STATES>(resource.initial_state),
          resource.has_optimized_clear_value ? &optimized_clear : nullptr,
          IID_PPV_ARGS(native.resource.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommittedResource", hr);
        return false;
      }
      resources_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool upload_resource_data(
      ID3D12Resource *resource,
      const D3D12ReplayBackend::ResourceDataUpdate &update,
      std::string &error)
  {
    void *mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    HRESULT hr = resource->Map(update.subresource, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
      error = hresult_error("Map(resource data)", hr);
      return false;
    }
    std::memcpy(
        static_cast<std::uint8_t *>(mapped) + static_cast<std::size_t>(update.written_begin),
        update.bytes.data(),
        update.bytes.size());
    D3D12_RANGE written_range{
        static_cast<SIZE_T>(update.written_begin),
        static_cast<SIZE_T>(update.written_end)};
    resource->Unmap(update.subresource, &written_range);
    return true;
  }

  bool create_descriptor_heaps(std::string &error)
  {
    for (const auto &[object_id, heap] : backend_.descriptor_heaps_) {
      NativeDescriptorHeap native;
      D3D12_DESCRIPTOR_HEAP_DESC desc{};
      desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap.type);
      desc.NumDescriptors = heap.num_descriptors;
      desc.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(heap.flags);
      desc.NodeMask = heap.node_mask;
      HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(native.heap.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateDescriptorHeap", hr);
        return false;
      }
      native.type = desc.Type;
      native.descriptor_size = device_->GetDescriptorHandleIncrementSize(desc.Type);
      native.cpu_start = native.heap->GetCPUDescriptorHandleForHeapStart();
      if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        native.gpu_start = native.heap->GetGPUDescriptorHandleForHeapStart();
      }
      descriptor_heaps_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool create_root_signatures(std::string &error)
  {
    for (const auto &[object_id, root_signature] : backend_.root_signatures_) {
      if (root_signature.bytes.empty()) {
        return fail(error, "root signature is missing bytecode");
      }
      ComPtr<ID3D12RootSignature> native;
      HRESULT hr = device_->CreateRootSignature(
          root_signature.node_mask,
          root_signature.bytes.data(),
          root_signature.bytes.size(),
          IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateRootSignature", hr);
        return false;
      }
      root_signatures_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool read_shader_bytes(const json &pipeline_asset, const char *field, std::vector<std::uint8_t> &bytes, std::string &error) const
  {
    const auto shader = pipeline_asset.find(field);
    if (shader == pipeline_asset.end() || shader->is_null()) {
      bytes.clear();
      return true;
    }
    if (!shader->is_object()) {
      return fail(error, std::string("pipeline shader field is not an object: ") + field);
    }
    const auto size = payload_u64(*shader, "bytecode_size");
    const auto path_key = std::string(field) + "_path";
    const auto relative_path = payload_string(*shader, path_key);
    if (size == 0 || relative_path.empty()) {
      return fail(error, std::string("pipeline shader field is incomplete: ") + field);
    }
    return read_exact_asset_bytes(backend_.bundle_root_ / relative_path, size, bytes, error);
  }

  static D3D12_INPUT_LAYOUT_DESC input_layout_desc(
      const json &pipeline_asset,
      std::vector<std::string> &semantic_names,
      std::vector<D3D12_INPUT_ELEMENT_DESC> &elements)
  {
    semantic_names.clear();
    elements.clear();
    const auto input_layout = pipeline_asset.find("input_layout");
    if (input_layout == pipeline_asset.end() || !input_layout->is_object()) {
      return {};
    }
    const auto input_elements = input_layout->find("elements");
    if (input_elements == input_layout->end() || !input_elements->is_array()) {
      return {};
    }
    semantic_names.reserve(input_elements->size());
    elements.reserve(input_elements->size());
    for (const auto &element_json : *input_elements) {
      semantic_names.push_back(payload_string(element_json, "semantic_name"));
      D3D12_INPUT_ELEMENT_DESC element{};
      element.SemanticName = semantic_names.back().c_str();
      element.SemanticIndex = payload_u32(element_json, "semantic_index");
      element.Format = static_cast<DXGI_FORMAT>(payload_u32(element_json, "format"));
      element.InputSlot = payload_u32(element_json, "input_slot");
      element.AlignedByteOffset = payload_u32(element_json, "aligned_byte_offset");
      element.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(payload_u32(element_json, "input_slot_class"));
      element.InstanceDataStepRate = payload_u32(element_json, "instance_data_step_rate");
      elements.push_back(element);
    }
    return D3D12_INPUT_LAYOUT_DESC{elements.data(), static_cast<UINT>(elements.size())};
  }

  template <typename Desc>
  static bool require_object(const json &value, const char *key, Desc &, std::string &error)
  {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_object()) {
      error = std::string("D3D12 native command replay incomplete: pipeline asset missing ") + key;
      return false;
    }
    return true;
  }

  static D3D12_BLEND_DESC blend_desc(const json &pipeline_asset)
  {
    D3D12_BLEND_DESC desc{};
    const auto &blend = pipeline_asset["blend_state"];
    desc.AlphaToCoverageEnable = blend.value("alpha_to_coverage_enable", false);
    desc.IndependentBlendEnable = blend.value("independent_blend_enable", false);
    const auto targets = blend.find("render_targets");
    if (targets != blend.end() && targets->is_array()) {
      const auto count = std::min<std::size_t>(targets->size(), D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
      for (std::size_t index = 0; index < count; ++index) {
        const auto &target = (*targets)[index];
        desc.RenderTarget[index].BlendEnable = target.value("blend_enable", false);
        desc.RenderTarget[index].LogicOpEnable = target.value("logic_op_enable", false);
        desc.RenderTarget[index].SrcBlend = static_cast<D3D12_BLEND>(payload_u32(target, "src_blend"));
        desc.RenderTarget[index].DestBlend = static_cast<D3D12_BLEND>(payload_u32(target, "dest_blend"));
        desc.RenderTarget[index].BlendOp = static_cast<D3D12_BLEND_OP>(payload_u32(target, "blend_op"));
        desc.RenderTarget[index].SrcBlendAlpha = static_cast<D3D12_BLEND>(payload_u32(target, "src_blend_alpha"));
        desc.RenderTarget[index].DestBlendAlpha = static_cast<D3D12_BLEND>(payload_u32(target, "dest_blend_alpha"));
        desc.RenderTarget[index].BlendOpAlpha = static_cast<D3D12_BLEND_OP>(payload_u32(target, "blend_op_alpha"));
        desc.RenderTarget[index].LogicOp = static_cast<D3D12_LOGIC_OP>(payload_u32(target, "logic_op"));
        desc.RenderTarget[index].RenderTargetWriteMask = static_cast<UINT8>(payload_u32(target, "render_target_write_mask"));
      }
    }
    return desc;
  }

  static D3D12_RASTERIZER_DESC rasterizer_desc(const json &pipeline_asset)
  {
    const auto &rasterizer = pipeline_asset["rasterizer_state"];
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = static_cast<D3D12_FILL_MODE>(payload_u32(rasterizer, "fill_mode"));
    desc.CullMode = static_cast<D3D12_CULL_MODE>(payload_u32(rasterizer, "cull_mode"));
    desc.FrontCounterClockwise = rasterizer.value("front_counter_clockwise", false);
    desc.DepthBias = payload_i32(rasterizer, "depth_bias");
    desc.DepthBiasClamp = payload_float(rasterizer, "depth_bias_clamp");
    desc.SlopeScaledDepthBias = payload_float(rasterizer, "slope_scaled_depth_bias");
    desc.DepthClipEnable = rasterizer.value("depth_clip_enable", false);
    desc.MultisampleEnable = rasterizer.value("multisample_enable", false);
    desc.AntialiasedLineEnable = rasterizer.value("antialiased_line_enable", false);
    desc.ForcedSampleCount = payload_u32(rasterizer, "forced_sample_count");
    desc.ConservativeRaster = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(payload_u32(rasterizer, "conservative_raster"));
    return desc;
  }

  static D3D12_DEPTH_STENCILOP_DESC depth_stencil_op_desc(const json &op)
  {
    D3D12_DEPTH_STENCILOP_DESC desc{};
    desc.StencilFailOp = static_cast<D3D12_STENCIL_OP>(payload_u32(op, "stencil_fail_op"));
    desc.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(payload_u32(op, "stencil_depth_fail_op"));
    desc.StencilPassOp = static_cast<D3D12_STENCIL_OP>(payload_u32(op, "stencil_pass_op"));
    desc.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(payload_u32(op, "stencil_func"));
    return desc;
  }

  static D3D12_DEPTH_STENCIL_DESC depth_stencil_desc(const json &pipeline_asset)
  {
    const auto &depth_stencil = pipeline_asset["depth_stencil_state"];
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = depth_stencil.value("depth_enable", false);
    desc.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(payload_u32(depth_stencil, "depth_write_mask"));
    desc.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(payload_u32(depth_stencil, "depth_func"));
    desc.StencilEnable = depth_stencil.value("stencil_enable", false);
    desc.StencilReadMask = static_cast<UINT8>(payload_u32(depth_stencil, "stencil_read_mask"));
    desc.StencilWriteMask = static_cast<UINT8>(payload_u32(depth_stencil, "stencil_write_mask"));
    desc.FrontFace = depth_stencil_op_desc(depth_stencil["front_face"]);
    desc.BackFace = depth_stencil_op_desc(depth_stencil["back_face"]);
    return desc;
  }

  bool create_pipelines(std::string &error)
  {
    for (const auto &[object_id, pipeline] : backend_.pipelines_) {
      const auto root_it = root_signatures_.find(pipeline.root_signature_object_id);
      if (root_it == root_signatures_.end()) {
        return fail(error, "pipeline references an unknown root signature");
      }
      json pipeline_asset;
      if (!read_pipeline_asset_json(backend_.bundle_root_, pipeline.relative_path, &pipeline_asset, error)) {
        return false;
      }
      D3D12_BLEND_DESC blend{};
      D3D12_RASTERIZER_DESC rasterizer{};
      D3D12_DEPTH_STENCIL_DESC depth_stencil{};
      if (!pipeline.graphics) {
        std::vector<std::uint8_t> cs;
        if (!read_shader_bytes(pipeline_asset, "cs", cs, error)) {
          return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_it->second.get();
        desc.CS = D3D12_SHADER_BYTECODE{cs.data(), cs.size()};
        desc.NodeMask = pipeline.node_mask;
        desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(pipeline.flags);
        ComPtr<ID3D12PipelineState> native;
        HRESULT hr = device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(native.put()));
        if (FAILED(hr)) {
          error = hresult_error("CreateComputePipelineState", hr);
          return false;
        }
        pipelines_.emplace(object_id, std::move(native));
        continue;
      }
      if (!require_object(pipeline_asset, "blend_state", blend, error) ||
          !require_object(pipeline_asset, "rasterizer_state", rasterizer, error) ||
          !require_object(pipeline_asset, "depth_stencil_state", depth_stencil, error)) {
        return false;
      }
      std::vector<std::uint8_t> vs;
      std::vector<std::uint8_t> ps;
      if (!read_shader_bytes(pipeline_asset, "vs", vs, error) ||
          !read_shader_bytes(pipeline_asset, "ps", ps, error)) {
        return false;
      }
      std::vector<std::string> semantic_names;
      std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = root_it->second.get();
      desc.VS = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
      desc.PS = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
      desc.BlendState = blend_desc(pipeline_asset);
      desc.SampleMask = pipeline.sample_mask;
      desc.RasterizerState = rasterizer_desc(pipeline_asset);
      desc.DepthStencilState = depth_stencil_desc(pipeline_asset);
      desc.InputLayout = input_layout_desc(pipeline_asset, semantic_names, input_elements);
      desc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(pipeline.primitive_topology_type);
      desc.NumRenderTargets = pipeline.num_render_targets;
      for (std::size_t index = 0; index < pipeline.rtv_formats.size() && index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
        desc.RTVFormats[index] = static_cast<DXGI_FORMAT>(pipeline.rtv_formats[index]);
      }
      desc.DSVFormat = static_cast<DXGI_FORMAT>(pipeline.dsv_format);
      desc.SampleDesc.Count = pipeline.sample_count;
      desc.SampleDesc.Quality = pipeline.sample_quality;
      desc.NodeMask = pipeline.node_mask;
      desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(pipeline.flags);
      desc.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(pipeline.ib_strip_cut_value);
      ComPtr<ID3D12PipelineState> native;
      HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateGraphicsPipelineState", hr);
        return false;
      }
      pipelines_.emplace(object_id, std::move(native));
    }
    return true;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE relocated_cpu_descriptor(const D3D12ReplayBackend::DescriptorBinding &binding) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return handle;
    }
    handle.ptr = heap_it->second.cpu_start.ptr +
                 static_cast<SIZE_T>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return handle;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE relocated_gpu_descriptor(const D3D12ReplayBackend::DescriptorBinding &binding) const
  {
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return handle;
    }
    handle.ptr = heap_it->second.gpu_start.ptr +
                 static_cast<UINT64>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return handle;
  }

  bool apply_descriptor(
      const D3D12ReplayBackend::DescriptorSemanticState &semantic,
      std::string &error)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE dst = relocated_cpu_descriptor(semantic.binding);
    if (dst.ptr == 0) {
      return fail(error, "descriptor destination cannot be relocated");
    }
    ID3D12Resource *resource = semantic.resource_object_id == 0 ? nullptr : resource_for(semantic.resource_object_id, error);
    if (semantic.resource_object_id != 0 && !resource) {
      return false;
    }
    if (semantic.kind == "RenderTargetView") {
      D3D12_RENDER_TARGET_VIEW_DESC desc{};
      D3D12_RENDER_TARGET_VIEW_DESC *desc_ptr = nullptr;
      if (semantic.view_dimension != 0 || semantic.format != 0) {
        desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
        desc.ViewDimension = static_cast<D3D12_RTV_DIMENSION>(semantic.view_dimension);
        desc.Texture2D.MipSlice = semantic.mip_slice;
        desc.Texture2D.PlaneSlice = semantic.plane_slice;
        desc_ptr = &desc;
      }
      device_->CreateRenderTargetView(resource, desc_ptr, dst);
    } else if (semantic.kind == "DepthStencilView") {
      D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
      D3D12_DEPTH_STENCIL_VIEW_DESC *desc_ptr = nullptr;
      if (semantic.view_dimension != 0 || semantic.format != 0) {
        desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
        desc.ViewDimension = static_cast<D3D12_DSV_DIMENSION>(semantic.view_dimension);
        desc.Flags = static_cast<D3D12_DSV_FLAGS>(semantic.flags);
        desc.Texture2D.MipSlice = semantic.mip_slice;
        desc_ptr = &desc;
      }
      device_->CreateDepthStencilView(resource, desc_ptr, dst);
    } else if (semantic.kind == "ConstantBufferView") {
      if (semantic.buffer.resource_object_id == 0) {
        return fail(error, "CBV source GPUVA cannot be relocated");
      }
      const auto native_it = resources_.find(semantic.buffer.resource_object_id);
      if (native_it == resources_.end()) {
        return fail(error, "CBV source resource is missing");
      }
      D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
      desc.BufferLocation = native_it->second.resource->GetGPUVirtualAddress() + semantic.buffer.offset;
      desc.SizeInBytes = semantic.size_in_bytes;
      device_->CreateConstantBufferView(&desc, dst);
    } else if (semantic.kind == "ShaderResourceView") {
      D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
      desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
      desc.ViewDimension = static_cast<D3D12_SRV_DIMENSION>(semantic.view_dimension);
      desc.Shader4ComponentMapping = semantic.shader_4_component_mapping;
      if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MostDetailedMip = semantic.most_detailed_mip;
        desc.Texture2D.MipLevels = semantic.mip_levels;
        desc.Texture2D.PlaneSlice = semantic.plane_slice;
        desc.Texture2D.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        desc.Buffer.FirstElement = semantic.first_element;
        desc.Buffer.NumElements = semantic.num_elements;
        desc.Buffer.StructureByteStride = semantic.structure_byte_stride;
        desc.Buffer.Flags = static_cast<D3D12_BUFFER_SRV_FLAGS>(semantic.flags);
      } else {
        return fail(error, "SRV dimension is not covered yet");
      }
      device_->CreateShaderResourceView(resource, &desc, dst);
    } else if (semantic.kind == "UnorderedAccessView") {
      D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
      desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
      desc.ViewDimension = static_cast<D3D12_UAV_DIMENSION>(semantic.view_dimension);
      if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MipSlice = semantic.mip_slice;
        desc.Texture2D.PlaneSlice = semantic.plane_slice;
      } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
        desc.Buffer.FirstElement = semantic.first_element;
        desc.Buffer.NumElements = semantic.num_elements;
        desc.Buffer.StructureByteStride = semantic.structure_byte_stride;
        desc.Buffer.CounterOffsetInBytes = semantic.counter_offset_in_bytes;
        desc.Buffer.Flags = static_cast<D3D12_BUFFER_UAV_FLAGS>(semantic.flags);
      } else {
        return fail(error, "UAV dimension is not covered yet");
      }
      ID3D12Resource *counter_resource = semantic.counter_resource_object_id == 0 ? nullptr : resource_for(semantic.counter_resource_object_id, error);
      if (semantic.counter_resource_object_id != 0 && !counter_resource) {
        return false;
      }
      device_->CreateUnorderedAccessView(resource, counter_resource, &desc, dst);
    } else {
      return fail(error, "descriptor kind is not covered: " + semantic.kind);
    }
    return true;
  }

  bool sync_descriptors_until(std::uint64_t sequence, std::string &error)
  {
    while (next_descriptor_index_ < descriptor_timeline_.size() &&
           descriptor_timeline_[next_descriptor_index_]->create_sequence <= sequence) {
      if (!apply_descriptor(*descriptor_timeline_[next_descriptor_index_], error)) {
        return false;
      }
      ++next_descriptor_index_;
    }
    return true;
  }

  bool sync_resource_data_until(std::uint64_t sequence, std::string &error)
  {
    while (next_resource_update_index_ < resource_update_timeline_.size() &&
           resource_update_timeline_[next_resource_update_index_].sequence <= sequence) {
      const auto &entry = resource_update_timeline_[next_resource_update_index_];
      auto native_it = resources_.find(entry.resource_object_id);
      if (native_it == resources_.end()) {
        return fail(error, "resource update references an unknown resource");
      }
      if (!upload_resource_data(native_it->second.resource.get(), *entry.update, error)) {
        return false;
      }
      ++next_resource_update_index_;
    }
    return true;
  }

  bool prepare_chronological_state(std::string &error)
  {
    descriptor_timeline_.clear();
    descriptor_timeline_.reserve(backend_.descriptors_.size());
    for (const auto &descriptor : backend_.descriptors_) {
      descriptor_timeline_.push_back(&descriptor);
    }
    std::stable_sort(
        descriptor_timeline_.begin(),
        descriptor_timeline_.end(),
        [](const auto *lhs, const auto *rhs) {
          return lhs->create_sequence < rhs->create_sequence;
        });

    resource_update_timeline_.clear();
    for (const auto &[resource_object_id, resource] : backend_.resources_) {
      for (const auto &update : resource.data_updates) {
        resource_update_timeline_.push_back(ResourceDataUpdateRef{update.sequence, resource_object_id, &update});
      }
    }
    std::stable_sort(
        resource_update_timeline_.begin(),
        resource_update_timeline_.end(),
        [](const auto &lhs, const auto &rhs) {
          return lhs.sequence < rhs.sequence;
        });

    next_descriptor_index_ = 0;
    next_resource_update_index_ = 0;
    (void)error;
    return true;
  }

  bool create_command_signatures(std::string &error)
  {
    for (const auto &[object_id, signature] : backend_.command_signatures_) {
      std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
      arguments.reserve(signature.arguments.size());
      for (const auto &argument : signature.arguments) {
        D3D12_INDIRECT_ARGUMENT_DESC desc{};
        desc.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(argument.type);
        switch (desc.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
          desc.VertexBuffer.Slot = argument.slot;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
          desc.Constant.RootParameterIndex = argument.root_parameter_index;
          desc.Constant.DestOffsetIn32BitValues = argument.dest_offset_in32bit_values;
          desc.Constant.Num32BitValuesToSet = argument.num32bit_values_to_set;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
          desc.ConstantBufferView.RootParameterIndex = argument.root_parameter_index;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
          desc.ShaderResourceView.RootParameterIndex = argument.root_parameter_index;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
          desc.UnorderedAccessView.RootParameterIndex = argument.root_parameter_index;
          break;
        default:
          break;
        }
        arguments.push_back(desc);
      }
      D3D12_COMMAND_SIGNATURE_DESC desc{};
      desc.ByteStride = signature.byte_stride;
      desc.NumArgumentDescs = static_cast<UINT>(arguments.size());
      desc.pArgumentDescs = arguments.data();
      desc.NodeMask = signature.node_mask;
      ID3D12RootSignature *root_signature = nullptr;
      if (signature.root_signature_object_id != 0) {
        const auto root_it = root_signatures_.find(signature.root_signature_object_id);
        if (root_it == root_signatures_.end()) {
          return fail(error, "command signature references an unknown root signature");
        }
        root_signature = root_it->second.get();
      }
      ComPtr<ID3D12CommandSignature> native;
      HRESULT hr = device_->CreateCommandSignature(&desc, root_signature, IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandSignature", hr);
        return false;
      }
      command_signatures_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool create_command_objects(std::string &error)
  {
    for (const auto &[object_id, command_list] : backend_.command_lists_) {
      if (command_list.type != static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT)) {
        return fail(error, "only direct command lists are currently supported");
      }
      NativeCommandList native;
      native.type = static_cast<D3D12_COMMAND_LIST_TYPE>(command_list.type);
      HRESULT hr = device_->CreateCommandAllocator(native.type, IID_PPV_ARGS(native.allocator.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandAllocator", hr);
        return false;
      }
      hr = device_->CreateCommandList(0, native.type, native.allocator.get(), nullptr, IID_PPV_ARGS(native.list.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandList", hr);
        return false;
      }
      native.list->Close();
      command_lists_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool find_submitted_interval(
      trace::ObjectId command_list_id,
      std::uint64_t execute_sequence,
      std::size_t &begin_index,
      std::size_t &end_index,
      std::string &error) const
  {
    constexpr auto invalid = std::numeric_limits<std::size_t>::max();
    auto current_begin = invalid;
    begin_index = invalid;
    end_index = invalid;
    for (std::size_t index = 0; index < backend_.replay_commands_.size(); ++index) {
      const auto &command = backend_.replay_commands_[index];
      if (command.command_list_object_id != command_list_id) {
        continue;
      }
      if (command.sequence >= execute_sequence) {
        break;
      }
      if (command.kind == D3D12ReplayBackend::ReplayCommandKind::BeginCommandList) {
        current_begin = index;
      } else if (command.kind == D3D12ReplayBackend::ReplayCommandKind::EndCommandList && current_begin != invalid) {
        begin_index = current_begin;
        end_index = index;
        current_begin = invalid;
      }
    }
    if (begin_index == invalid || end_index == invalid) {
      error = "D3D12 native command replay incomplete: submitted command list has no closed interval";
      return false;
    }
    return true;
  }

  bool replay_submissions(std::string &error)
  {
    for (const auto &batch : backend_.submissions_.completed_batches()) {
      std::vector<ID3D12CommandList *> native_lists;
      native_lists.reserve(batch.command_list_ids.size());
      for (const auto command_list_id : batch.command_list_ids) {
        auto native_it = command_lists_.find(command_list_id);
        if (native_it == command_lists_.end()) {
          return fail(error, "submission references an unknown command list");
        }
        std::size_t begin_index = 0;
        std::size_t end_index = 0;
        if (!find_submitted_interval(command_list_id, batch.execute_sequence, begin_index, end_index, error)) {
          return false;
        }
        if (!record_command_list(native_it->second, begin_index, end_index, error)) {
          return false;
        }
        native_lists.push_back(native_it->second.list.get());
      }
      if (!sync_resource_data_until(batch.execute_sequence, error) ||
          !sync_descriptors_until(batch.execute_sequence, error)) {
        return false;
      }
      if (!native_lists.empty()) {
        queue_->ExecuteCommandLists(static_cast<UINT>(native_lists.size()), native_lists.data());
        wait_for_gpu();
      }
      if (batch.presented) {
        UINT sync_interval = 1;
        UINT flags = 0;
        for (const auto &[frame_index, semantic] : backend_.present_semantics_) {
          (void)frame_index;
          if (semantic.boundary_sequence != batch.present_sequence) {
            continue;
          }
          sync_interval = static_cast<UINT>(semantic.sync_interval);
          flags = static_cast<UINT>(semantic.flags);
          break;
        }
        if (!capture_present_frame(sync_interval, flags, error)) {
          return false;
        }
        HRESULT hr = swap_chain_->Present(sync_interval, flags);
        record_present_semantics(sync_interval, flags, hr);
        if (FAILED(hr)) {
          error = hresult_error("IDXGISwapChain::Present", hr);
          return false;
        }
        if (present_delay_ms_ != 0) {
#ifdef _WIN32
          Sleep(present_delay_ms_);
#else
          std::this_thread::sleep_for(std::chrono::milliseconds(present_delay_ms_));
#endif
        }
        if (!pump_messages()) {
          return fail(error, "replay window closed");
        }
      }
    }
    return true;
  }

  void record_present_semantics(UINT sync_interval, UINT flags, HRESULT result)
  {
    if (!d3d12_retrace_present_frame_capture_enabled()) {
      return;
    }
    static RecordPresentSemanticsFn record =
        resolve_d3d12_export<RecordPresentSemanticsFn>("apitrace_d3d12_record_present_semantics");
    if (record) {
      record(sync_interval, flags, result);
    }
  }

  bool capture_present_frame(UINT sync_interval, UINT flags, std::string &error)
  {
    if (!d3d12_retrace_present_frame_capture_enabled()) {
      return true;
    }
#ifdef _WIN32
    static RecordPresentFrameFn proxy_record_present_frame =
        resolve_d3d12_export<RecordPresentFrameFn>("apitrace_d3d12_record_present_frame");
    if (!proxy_record_present_frame) {
      return fail(error, "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES is enabled but the capture export is missing");
    }
#endif

    const UINT back_buffer_index = swap_chain_->GetCurrentBackBufferIndex();
    if (back_buffer_index >= kBufferCount || !back_buffers_[back_buffer_index]) {
      return fail(error, "swapchain back-buffer is missing for present-frame capture");
    }
    ID3D12Resource *back_buffer = back_buffers_[back_buffer_index].get();
    const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
      return fail(error, "present-frame capture currently requires an RGBA8 swapchain");
    }
    if (desc.Width == 0 || desc.Height == 0 ||
        desc.Width > static_cast<UINT64>(std::numeric_limits<UINT>::max()) ||
        desc.Height > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
      return fail(error, "present-frame capture has invalid back-buffer dimensions");
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 readback_size = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &readback_size);
    if (num_rows == 0 || readback_size == 0 || footprint.Footprint.RowPitch < desc.Width * 4u) {
      return fail(error, "present-frame capture produced an invalid readback layout");
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = readback_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const auto width = static_cast<UINT>(desc.Width);
    const auto height = static_cast<UINT>(desc.Height);
    const auto row_pitch = width * 4u;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(row_pitch) * height);

    {
      ScopedD3D12CaptureSuppression suppress_capture;
      ComPtr<ID3D12Resource> readback;
      HRESULT hr = device_->CreateCommittedResource(
          &heap,
          D3D12_HEAP_FLAG_NONE,
          &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,
          IID_PPV_ARGS(readback.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommittedResource(readback)", hr);
        return false;
      }

      ReadbackCommandObjects commands;
      hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commands.allocator.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandAllocator(readback)", hr);
        return false;
      }
      hr = device_->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          commands.allocator.get(),
          nullptr,
          IID_PPV_ARGS(commands.list.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandList(readback)", hr);
        return false;
      }

      const auto to_copy = transition_barrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      commands.list->ResourceBarrier(1, &to_copy);

      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = back_buffer;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;

      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = readback.get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprint;
      commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

      const auto to_present = transition_barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      commands.list->ResourceBarrier(1, &to_present);
      hr = commands.list->Close();
      if (FAILED(hr)) {
        error = hresult_error("ID3D12GraphicsCommandList::Close(readback)", hr);
        return false;
      }

      ID3D12CommandList *lists[] = {commands.list.get()};
      queue_->ExecuteCommandLists(1, lists);
      wait_for_gpu();

      void *mapped = nullptr;
      D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
      hr = readback->Map(0, &read_range, &mapped);
      if (FAILED(hr) || !mapped) {
        error = hresult_error("Map(readback)", hr);
        return false;
      }

      for (UINT row = 0; row < height; ++row) {
        const auto *src_row = static_cast<const std::uint8_t *>(mapped) +
                              static_cast<std::size_t>(footprint.Offset) +
                              static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
        auto *dst_row = rgba.data() + static_cast<std::size_t>(row) * row_pitch;
        std::memcpy(dst_row, src_row, row_pitch);
      }
      D3D12_RANGE written_range{0, 0};
      readback->Unmap(0, &written_range);
    }

    if (!record_present_frame(
        width,
        height,
        row_pitch,
        sync_interval,
        flags,
        rgba.data(),
        static_cast<SIZE_T>(rgba.size()),
        error)) {
      return false;
    }
    return true;
  }

#ifdef _WIN32
  bool record_present_frame(
      UINT width,
      UINT height,
      UINT row_pitch,
      UINT sync_interval,
      UINT flags,
      const void *rgba_data,
      SIZE_T rgba_size,
      std::string &error)
  {
    static RecordPresentFrameFn record =
        resolve_d3d12_export<RecordPresentFrameFn>("apitrace_d3d12_record_present_frame");
    if (!record) {
      return fail(error, "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES is enabled but the capture export is missing");
    }
    record(width, height, row_pitch, sync_interval, flags, rgba_data, rgba_size);
    return true;
  }
#else
  bool ensure_capture_writer(std::string &error)
  {
    if (capture_writer_) {
      return true;
    }
    const char *bundle_root = std::getenv("APITRACE_TRACE_BUNDLE");
    if (bundle_root == nullptr || *bundle_root == '\0') {
      return fail(error, "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES requires APITRACE_TRACE_BUNDLE");
    }
    capture_writer_ = std::make_unique<trace::TraceBundleWriter>();
    if (!capture_writer_->open(bundle_root)) {
      capture_writer_.reset();
      return fail(error, "failed to open APITRACE_TRACE_BUNDLE for native D3D12 present capture");
    }
    trace::TraceMetadata metadata;
    metadata.api = trace::ApiKind::D3D12;
    metadata.producer = "apitrace_d3d12_native_retrace";
    capture_writer_->write_metadata(metadata);
    return true;
  }

  bool record_present_frame(
      UINT width,
      UINT height,
      UINT row_pitch,
      UINT sync_interval,
      UINT flags,
      const void *rgba_data,
      SIZE_T rgba_size,
      std::string &error)
  {
    if (!rgba_data || width == 0 || height == 0 || row_pitch == 0 || rgba_size == 0) {
      return true;
    }
    if (!ensure_capture_writer(error)) {
      return false;
    }

    const auto frame_index = ++capture_frame_index_;
    trace::AssetRecord asset;
    asset.blob_id = ++capture_sequence_;
    asset.kind = trace::AssetKind::Texture;
    asset.debug_name = "d3d12-present-frame";
    const auto *begin = static_cast<const std::uint8_t *>(rgba_data);
    asset.payload_bytes.assign(begin, begin + static_cast<std::size_t>(rgba_size));
    asset = capture_writer_->register_asset(asset);

    std::ostringstream payload;
    payload << "{"
            << "\"frame_index\":" << frame_index << ","
            << "\"width\":" << width << ","
            << "\"height\":" << height << ","
            << "\"row_pitch\":" << row_pitch << ","
            << "\"sync_interval\":" << sync_interval << ","
            << "\"flags\":" << flags << ","
            << "\"format\":\"rgba8\","
            << "\"frame_path\":\"" << asset.relative_path.generic_string() << "\""
            << "}";

    trace::EventRecord event;
    event.kind = trace::EventKind::ResourceBlob;
    event.callsite.sequence = ++capture_sequence_;
    event.callsite.function_name = "resource_blob";
    event.object_debug_name = "D3D12PresentFrame";
    event.blob_refs = {asset.blob_id};
    event.payload = payload.str();
    capture_writer_->append_call_event(event);
    return true;
  }
#endif

  bool record_command_list(
      NativeCommandList &native,
      std::size_t begin_index,
      std::size_t end_index,
      std::string &error)
  {
    HRESULT hr = native.allocator->Reset();
    if (FAILED(hr)) {
      error = hresult_error("ID3D12CommandAllocator::Reset", hr);
      return false;
    }
    hr = native.list->Reset(native.allocator.get(), nullptr);
    if (FAILED(hr)) {
      error = hresult_error("ID3D12GraphicsCommandList::Reset", hr);
      return false;
    }
    for (std::size_t index = begin_index + 1; index < end_index; ++index) {
      if (!record_command(native.list.get(), backend_.replay_commands_[index], error)) {
        return false;
      }
    }
    hr = native.list->Close();
    if (FAILED(hr)) {
      error = hresult_error("ID3D12GraphicsCommandList::Close", hr);
      return false;
    }
    return true;
  }

  bool descriptor_handle(
      const D3D12ReplayBackend::DescriptorBinding &binding,
      D3D12_CPU_DESCRIPTOR_HANDLE &handle,
      std::string &error) const
  {
    handle = {};
    if (binding.heap_object_id == 0) {
      return true;
    }
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return fail(error, "descriptor binding references an unknown heap");
    }
    handle.ptr = heap_it->second.cpu_start.ptr +
                 static_cast<SIZE_T>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return true;
  }

  bool descriptor_gpu_handle(
      const D3D12ReplayBackend::DescriptorBinding &binding,
      D3D12_GPU_DESCRIPTOR_HANDLE &handle,
      std::string &error) const
  {
    handle = {};
    if (binding.heap_object_id == 0) {
      return true;
    }
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return fail(error, "descriptor binding references an unknown heap");
    }
    handle.ptr = heap_it->second.gpu_start.ptr +
                 static_cast<UINT64>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return true;
  }

  ID3D12Resource *resource_for(trace::ObjectId object_id, std::string &error) const
  {
    const auto resource_it = resources_.find(object_id);
    if (resource_it == resources_.end() || !resource_it->second.resource) {
      error = "D3D12 native command replay incomplete: command references an unknown resource";
      return nullptr;
    }
    return resource_it->second.resource.get();
  }

  bool record_command(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    json payload = json::parse(command.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return fail(error, "command payload is not JSON");
    }
    if (!sync_resource_data_until(command.sequence, error) ||
        !sync_descriptors_until(command.sequence, error)) {
      return false;
    }

    switch (command.kind) {
    case D3D12ReplayBackend::ReplayCommandKind::SetPipelineState:
      return record_set_pipeline_state(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootSignature:
      return record_set_root_signature(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetDescriptorHeaps:
      return record_set_descriptor_heaps(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootDescriptorTable:
      return record_set_root_descriptor_table(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstants:
      return record_set_root_constants(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstantBufferView:
      return record_set_root_descriptor(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetViewports:
      return record_set_viewports(list, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetScissorRects:
      return record_set_scissors(list, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRenderTargets:
      return record_set_render_targets(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ClearRenderTarget:
      return record_clear_render_target(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ClearDepthStencil:
      return record_clear_depth_stencil(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ResourceBarrier:
      return record_resource_barriers(list, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetPrimitiveTopology:
      list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(payload_u32(payload, "primitive_topology")));
      return true;
    case D3D12ReplayBackend::ReplayCommandKind::SetVertexBuffers:
      return record_set_vertex_buffers(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetIndexBuffer:
      return record_set_index_buffer(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Draw:
      return record_draw(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ExecuteIndirect:
      return record_execute_indirect(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Copy:
      return record_copy(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Resolve:
      return record_resolve(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Dispatch:
      return record_dispatch(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::MapResource:
    case D3D12ReplayBackend::ReplayCommandKind::UnmapResource:
      return true;
    default:
      return fail(error, "unsupported command " + command.function_name);
    }
  }

  bool record_set_pipeline_state(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    if (command.object_refs.size() < 2) {
      return fail(error, "SetPipelineState missing PSO object");
    }
    const auto pipeline_it = pipelines_.find(command.object_refs[1]);
    if (pipeline_it == pipelines_.end()) {
      return fail(error, "SetPipelineState references an unknown PSO");
    }
    list->SetPipelineState(pipeline_it->second.get());
    return true;
  }

  bool record_set_root_signature(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    if (command.object_refs.size() < 2) {
      return fail(error, "SetRootSignature missing root signature object");
    }
    const auto root_it = root_signatures_.find(command.object_refs[1]);
    if (root_it == root_signatures_.end()) {
      return fail(error, "SetRootSignature references an unknown root signature");
    }
    if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootSignature") {
      list->SetComputeRootSignature(root_it->second.get());
    } else {
      list->SetGraphicsRootSignature(root_it->second.get());
    }
    return true;
  }

  bool record_set_descriptor_heaps(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    std::vector<ID3D12DescriptorHeap *> heaps;
    for (std::size_t index = 1; index < command.object_refs.size(); ++index) {
      const auto heap_it = descriptor_heaps_.find(command.object_refs[index]);
      if (heap_it == descriptor_heaps_.end()) {
        return fail(error, "SetDescriptorHeaps references an unknown heap");
      }
      heaps.push_back(heap_it->second.heap.get());
    }
    list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
    return true;
  }

  bool record_set_root_descriptor_table(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
    std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "base_descriptor"), true, command.sequence, binding, error)) {
      return false;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_gpu_handle(binding, handle, error)) {
      return false;
    }
    const UINT root_parameter_index = payload_u32(payload, "root_parameter_index");
    if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable") {
      list->SetComputeRootDescriptorTable(root_parameter_index, handle);
    } else {
      list->SetGraphicsRootDescriptorTable(root_parameter_index, handle);
    }
    return true;
  }

  bool record_set_root_constants(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    const auto values_json = payload.find("values");
    if (values_json == payload.end() || !values_json->is_array()) {
      return fail(error, "root constant command missing values");
    }
    std::vector<UINT> values;
    values.reserve(values_json->size());
    for (const auto &value : *values_json) {
      values.push_back(static_cast<UINT>(json_u64(value)));
    }
    const UINT root_parameter_index = payload_u32(payload, "root_parameter_index");
    const UINT dst_offset = payload_u32(payload, "dst_offset");
    if (command.function_name.find("Compute") != std::string::npos) {
      list->SetComputeRoot32BitConstants(root_parameter_index, static_cast<UINT>(values.size()), values.data(), dst_offset);
    } else {
      list->SetGraphicsRoot32BitConstants(root_parameter_index, static_cast<UINT>(values.size()), values.data(), dst_offset);
    }
    return true;
  }

  bool record_set_root_descriptor(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::GpuVirtualAddressBinding binding;
    if (!backend_.resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), command.sequence, binding, error)) {
      return false;
    }
    const auto resource_it = resources_.find(binding.resource_object_id);
    if (resource_it == resources_.end()) {
      return fail(error, "root descriptor resource cannot be relocated");
    }
    const D3D12_GPU_VIRTUAL_ADDRESS address = resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
    const UINT root_parameter_index = payload_u32(payload, "root_parameter_index");
    if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView") {
      list->SetComputeRootConstantBufferView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView") {
      list->SetGraphicsRootShaderResourceView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView") {
      list->SetComputeRootShaderResourceView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView") {
      list->SetGraphicsRootUnorderedAccessView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView") {
      list->SetComputeRootUnorderedAccessView(root_parameter_index, address);
    } else {
      list->SetGraphicsRootConstantBufferView(root_parameter_index, address);
    }
    return true;
  }

  bool record_set_viewports(ID3D12GraphicsCommandList *list, const json &payload, std::string &error)
  {
    const auto viewports_json = payload.find("viewports");
    if (viewports_json == payload.end() || !viewports_json->is_array()) {
      return fail(error, "RSSetViewports missing viewport array");
    }
    std::vector<D3D12_VIEWPORT> viewports;
    for (const auto &viewport_json : *viewports_json) {
      D3D12_VIEWPORT viewport{};
      viewport.TopLeftX = payload_float(viewport_json, "x");
      viewport.TopLeftY = payload_float(viewport_json, "y");
      viewport.Width = payload_float(viewport_json, "width");
      viewport.Height = payload_float(viewport_json, "height");
      viewport.MinDepth = payload_float(viewport_json, "min_depth");
      viewport.MaxDepth = payload_float(viewport_json, "max_depth");
      viewports.push_back(viewport);
    }
    list->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    (void)error;
    return true;
  }

  bool record_set_scissors(ID3D12GraphicsCommandList *list, const json &payload, std::string &error)
  {
    const auto rects_json = payload.find("rects");
    if (rects_json == payload.end() || !rects_json->is_array()) {
      return fail(error, "RSSetScissorRects missing rect array");
    }
    std::vector<D3D12_RECT> rects;
    for (const auto &rect_json : *rects_json) {
      D3D12_RECT rect{};
      rect.left = payload_i32(rect_json, "left");
      rect.top = payload_i32(rect_json, "top");
      rect.right = payload_i32(rect_json, "right");
      rect.bottom = payload_i32(rect_json, "bottom");
      rects.push_back(rect);
    }
    list->RSSetScissorRects(static_cast<UINT>(rects.size()), rects.data());
    return true;
  }

  bool record_set_render_targets(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    const auto render_targets = payload.find("render_targets");
    if (render_targets == payload.end() || !render_targets->is_array()) {
      return fail(error, "OMSetRenderTargets missing render target array");
    }
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
    for (const auto &descriptor : *render_targets) {
      D3D12ReplayBackend::DescriptorBinding binding;
      if (!backend_.resolve_descriptor_binding_at(json_u64(descriptor), false, command.sequence, binding, error)) {
        return false;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE handle{};
      if (!descriptor_handle(binding, handle, error)) {
        return false;
      }
      handles.push_back(handle);
    }
    list->OMSetRenderTargets(static_cast<UINT>(handles.size()), handles.data(), FALSE, nullptr);
    return true;
  }

  bool record_clear_render_target(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, command.sequence, binding, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_handle(binding, handle, error)) {
      return false;
    }
    const auto color_json = payload.find("color");
    if (color_json == payload.end() || !color_json->is_array() || color_json->size() != 4) {
      return fail(error, "ClearRenderTargetView missing color");
    }
    FLOAT color[4] = {
        (*color_json)[0].get<float>(),
        (*color_json)[1].get<float>(),
        (*color_json)[2].get<float>(),
        (*color_json)[3].get<float>()};
    list->ClearRenderTargetView(handle, color, 0, nullptr);
    return true;
  }

  bool record_clear_depth_stencil(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, command.sequence, binding, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_handle(binding, handle, error)) {
      return false;
    }
    list->ClearDepthStencilView(
        handle,
        static_cast<D3D12_CLEAR_FLAGS>(payload_u32(payload, "clear_flags")),
        payload_float(payload, "depth"),
        static_cast<UINT8>(payload_u32(payload, "stencil")),
        0,
        nullptr);
    return true;
  }

  bool record_resource_barriers(ID3D12GraphicsCommandList *list, const json &payload, std::string &error)
  {
    const auto barriers_json = payload.find("barriers");
    if (barriers_json == payload.end() || !barriers_json->is_array()) {
      return fail(error, "ResourceBarrier missing barrier array");
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const auto &barrier_json : *barriers_json) {
      if (payload_u32(barrier_json, "type") != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        return fail(error, "only transition barriers are currently supported");
      }
      ID3D12Resource *resource = resource_for(json_object_id(barrier_json["resource_object_id"]), error);
      if (!resource) {
        return false;
      }
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(payload_u32(barrier_json, "flags"));
      barrier.Transition.pResource = resource;
      barrier.Transition.Subresource = payload_u32(barrier_json, "subresource");
      barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(payload_u32(barrier_json, "before"));
      barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(payload_u32(barrier_json, "after"));
      barriers.push_back(barrier);
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    return true;
  }

  bool record_set_vertex_buffers(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    const auto views_json = payload.find("views");
    if (views_json == payload.end() || !views_json->is_array()) {
      return fail(error, "IASetVertexBuffers missing view array");
    }
    std::vector<D3D12_VERTEX_BUFFER_VIEW> views;
    views.reserve(views_json->size());
    for (const auto &view_json : *views_json) {
      D3D12ReplayBackend::GpuVirtualAddressBinding binding;
      if (!backend_.resolve_gpu_virtual_address_at(payload_u64(view_json, "buffer_location"), command.sequence, binding, error)) {
        return false;
      }
      const auto resource_it = resources_.find(binding.resource_object_id);
      if (resource_it == resources_.end()) {
        return fail(error, "vertex buffer resource cannot be relocated");
      }
      D3D12_VERTEX_BUFFER_VIEW view{};
      view.BufferLocation = resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
      view.SizeInBytes = payload_u32(view_json, "size_in_bytes");
      view.StrideInBytes = payload_u32(view_json, "stride_in_bytes");
      views.push_back(view);
    }
    list->IASetVertexBuffers(payload_u32(payload, "start_slot"), static_cast<UINT>(views.size()), views.data());
    return true;
  }

  bool record_set_index_buffer(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (payload.empty() || payload_u64(payload, "buffer_location") == 0) {
      list->IASetIndexBuffer(nullptr);
      return true;
    }
    D3D12ReplayBackend::GpuVirtualAddressBinding binding;
    if (!backend_.resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), command.sequence, binding, error)) {
      return false;
    }
    const auto resource_it = resources_.find(binding.resource_object_id);
    if (resource_it == resources_.end()) {
      return fail(error, "index buffer resource cannot be relocated");
    }
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
    view.SizeInBytes = payload_u32(payload, "size_in_bytes");
    view.Format = static_cast<DXGI_FORMAT>(payload_u32(payload, "format"));
    list->IASetIndexBuffer(&view);
    return true;
  }

  bool record_draw(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    (void)error;
    if (command.function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced") {
      list->DrawIndexedInstanced(
          payload_u32(payload, "index_count_per_instance"),
          payload_u32(payload, "instance_count"),
          payload_u32(payload, "start_index_location"),
          payload_i32(payload, "base_vertex_location"),
          payload_u32(payload, "start_instance_location"));
    } else {
      list->DrawInstanced(
          payload_u32(payload, "vertex_count_per_instance"),
          payload_u32(payload, "instance_count"),
          payload_u32(payload, "start_vertex_location"),
          payload_u32(payload, "start_instance_location"));
    }
    return true;
  }

  bool record_execute_indirect(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.object_refs.size() < 3) {
      return fail(error, "ExecuteIndirect missing refs");
    }
    const auto signature_it = command_signatures_.find(command.object_refs[1]);
    if (signature_it == command_signatures_.end()) {
      return fail(error, "ExecuteIndirect references an unknown command signature");
    }
    ID3D12Resource *arg_buffer = resource_for(command.object_refs[2], error);
    if (!arg_buffer) {
      return false;
    }
    ID3D12Resource *count_buffer = nullptr;
    if (command.object_refs.size() >= 4 && command.object_refs[3] != 0) {
      count_buffer = resource_for(command.object_refs[3], error);
      if (!count_buffer) {
        return false;
      }
    }
    list->ExecuteIndirect(
        signature_it->second.get(),
        payload_u32(payload, "max_command_count"),
        arg_buffer,
        payload_u64(payload, "arg_buffer_offset"),
        count_buffer,
        payload_u64(payload, "count_buffer_offset"));
    return true;
  }

  bool texture_copy_location(
      const json &location_json,
      D3D12_TEXTURE_COPY_LOCATION &location,
      std::string &error) const
  {
    location = {};
    ID3D12Resource *resource = resource_for(json_object_id(location_json["resource_object_id"]), error);
    if (!resource) {
      return false;
    }
    location.pResource = resource;
    location.Type = static_cast<D3D12_TEXTURE_COPY_TYPE>(payload_u32(location_json, "type"));
    if (location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      location.SubresourceIndex = payload_u32(location_json, "subresource_index");
    } else if (location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      const auto footprint = location_json.find("placed_footprint");
      if (footprint == location_json.end() || !footprint->is_object()) {
        return fail(error, "CopyTextureRegion missing placed footprint");
      }
      location.PlacedFootprint.Offset = payload_u64(*footprint, "offset");
      location.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(payload_u32(*footprint, "format"));
      location.PlacedFootprint.Footprint.Width = payload_u32(*footprint, "width");
      location.PlacedFootprint.Footprint.Height = payload_u32(*footprint, "height");
      location.PlacedFootprint.Footprint.Depth = payload_u32(*footprint, "depth");
      location.PlacedFootprint.Footprint.RowPitch = payload_u32(*footprint, "row_pitch");
    } else {
      return fail(error, "unsupported texture copy location type");
    }
    return true;
  }

  bool record_copy(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.function_name == "ID3D12GraphicsCommandList::CopyResource") {
      if (command.object_refs.size() < 3) {
        return fail(error, "CopyResource missing resource refs");
      }
      ID3D12Resource *dst = resource_for(command.object_refs[1], error);
      ID3D12Resource *src = resource_for(command.object_refs[2], error);
      if (!dst || !src) {
        return false;
      }
      list->CopyResource(dst, src);
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::CopyTextureRegion") {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      D3D12_TEXTURE_COPY_LOCATION src{};
      if (!texture_copy_location(payload["dst"], dst, error) ||
          !texture_copy_location(payload["src"], src, error)) {
        return false;
      }
      list->CopyTextureRegion(
          &dst,
          payload_u32(payload, "dst_x"),
          payload_u32(payload, "dst_y"),
          payload_u32(payload, "dst_z"),
          &src,
          nullptr);
      return true;
    }
    return fail(error, "copy command is not covered: " + command.function_name);
  }

  bool record_resolve(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.object_refs.size() < 3) {
      return fail(error, "ResolveSubresource missing resource refs");
    }
    ID3D12Resource *dst = resource_for(command.object_refs[1], error);
    ID3D12Resource *src = resource_for(command.object_refs[2], error);
    if (!dst || !src) {
      return false;
    }
    list->ResolveSubresource(
        dst,
        payload_u32(payload, "dst_subresource"),
        src,
        payload_u32(payload, "src_subresource"),
        static_cast<DXGI_FORMAT>(payload_u32(payload, "format")));
    return true;
  }

  bool record_dispatch(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.function_name != "ID3D12GraphicsCommandList::Dispatch") {
      return fail(error, "advanced dispatch command is not covered: " + command.function_name);
    }
    list->Dispatch(
        payload_u32(payload, "thread_group_count_x"),
        payload_u32(payload, "thread_group_count_y"),
        payload_u32(payload, "thread_group_count_z"));
    return true;
  }

  void wait_for_gpu()
  {
    if (!queue_ || !fence_) {
      return;
    }
    const UINT64 value = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.get(), value))) {
      return;
    }
#ifdef _WIN32
    if (fence_->GetCompletedValue() < value && fence_event_ &&
        SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
      WaitForSingleObject(fence_event_, INFINITE);
    }
#else
    while (fence_->GetCompletedValue() < value) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
  }

  static constexpr UINT kBufferCount = 2;

  struct ResourceDataUpdateRef {
    std::uint64_t sequence = 0;
    trace::ObjectId resource_object_id = 0;
    const D3D12ReplayBackend::ResourceDataUpdate *update = nullptr;
  };

  const D3D12ReplayBackend &backend_;
  HWND hwnd_ = nullptr;
  UINT present_delay_ms_ = static_cast<UINT>(env_u32("APITRACE_D3D12_RETRACE_PRESENT_DELAY_MS"));
  UINT64 fence_value_ = 0;
#ifdef _WIN32
  HANDLE fence_event_ = nullptr;
#endif
#if defined(__APPLE__)
  apitrace::platform::macos::WindowHandles window_handles_;
#endif
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swap_chain_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12Resource> back_buffers_[kBufferCount];
  std::unordered_map<trace::ObjectId, NativeDescriptorHeap> descriptor_heaps_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12RootSignature>> root_signatures_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12PipelineState>> pipelines_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12CommandSignature>> command_signatures_;
  std::unordered_map<trace::ObjectId, NativeResource> resources_;
  std::unordered_map<trace::ObjectId, NativeCommandList> command_lists_;
  std::vector<const D3D12ReplayBackend::DescriptorSemanticState *> descriptor_timeline_;
  std::vector<ResourceDataUpdateRef> resource_update_timeline_;
  std::size_t next_descriptor_index_ = 0;
  std::size_t next_resource_update_index_ = 0;
#if !defined(_WIN32)
  std::unique_ptr<trace::TraceBundleWriter> capture_writer_;
  std::uint64_t capture_sequence_ = 0;
  std::uint64_t capture_frame_index_ = 0;
#endif
};

#else

class D3D12NativeReplayer {
public:
  explicit D3D12NativeReplayer(const D3D12ReplayBackend &) {}

  bool replay(std::string &error)
  {
    error = "D3D12 native command replay incomplete";
    return false;
  }
};

#endif

D3D12ReplayBackend::D3D12ReplayBackend() = default;

D3D12ReplayBackend::~D3D12ReplayBackend() = default;

bool D3D12ReplayBackend::resolve_descriptor_binding(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    DescriptorBinding &binding,
    std::string &error) const
{
  return resolve_descriptor_binding_at(
      descriptor,
      gpu_descriptor,
      std::numeric_limits<std::uint64_t>::max(),
      binding,
      error);
}

bool D3D12ReplayBackend::resolve_descriptor_binding_at(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    std::uint64_t sequence,
    DescriptorBinding &binding,
    std::string &error) const
{
  std::vector<trace::ObjectId> heap_object_ids;
  heap_object_ids.reserve(descriptor_heaps_.size());
  for (const auto &[heap_object_id, heap] : descriptor_heaps_) {
    (void)heap;
    heap_object_ids.push_back(heap_object_id);
  }
  return resolve_descriptor_binding_in_heaps_at(descriptor, gpu_descriptor, heap_object_ids, sequence, binding, error);
}

bool D3D12ReplayBackend::resolve_descriptor_binding_in_heaps(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    const std::vector<trace::ObjectId> &heap_object_ids,
    DescriptorBinding &binding,
    std::string &error) const
{
  return resolve_descriptor_binding_in_heaps_at(
      descriptor,
      gpu_descriptor,
      heap_object_ids,
      std::numeric_limits<std::uint64_t>::max(),
      binding,
      error);
}

bool D3D12ReplayBackend::resolve_descriptor_binding_in_heaps_at(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    const std::vector<trace::ObjectId> &heap_object_ids,
    std::uint64_t sequence,
    DescriptorBinding &binding,
    std::string &error) const
{
  binding = DescriptorBinding{};
  binding.descriptor = descriptor;
  if (descriptor == 0) {
    return true;
  }
  const DescriptorHeapSemanticState *best_heap = nullptr;
  trace::ObjectId best_heap_object_id = 0;
  std::uint64_t best_descriptor_index = 0;
  for (const auto heap_object_id : heap_object_ids) {
    const auto heap_it = descriptor_heaps_.find(heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      continue;
    }
    const auto &heap = heap_it->second;
    if (heap.create_sequence > sequence) {
      continue;
    }
    const auto heap_start = gpu_descriptor ? heap.gpu_start : heap.cpu_start;
    if (heap_start == 0 || heap.descriptor_size == 0 || heap.num_descriptors == 0 || descriptor < heap_start) {
      continue;
    }
    const auto offset = descriptor - heap_start;
    if (offset % heap.descriptor_size != 0) {
      continue;
    }
    const auto descriptor_index = offset / heap.descriptor_size;
    if (descriptor_index >= heap.num_descriptors) {
      continue;
    }
    if (!best_heap || heap.create_sequence > best_heap->create_sequence) {
      best_heap = &heap;
      best_heap_object_id = heap_object_id;
      best_descriptor_index = descriptor_index;
    }
  }
  if (best_heap) {
    binding.heap_object_id = best_heap_object_id;
    binding.heap_type = best_heap->type;
    binding.descriptor_index = static_cast<std::uint32_t>(best_descriptor_index);
    return true;
  }
  error = "descriptor handle does not map to a captured descriptor heap";
  return false;
}

bool D3D12ReplayBackend::resolve_gpu_virtual_address(
    std::uint64_t gpu_virtual_address,
    GpuVirtualAddressBinding &binding,
    std::string &error) const
{
  return resolve_gpu_virtual_address_at(gpu_virtual_address, std::numeric_limits<std::uint64_t>::max(), binding, error);
}

bool D3D12ReplayBackend::resolve_gpu_virtual_address_at(
    std::uint64_t gpu_virtual_address,
    std::uint64_t sequence,
    GpuVirtualAddressBinding &binding,
    std::string &error) const
{
  binding = GpuVirtualAddressBinding{};
  binding.gpu_virtual_address = gpu_virtual_address;
  if (gpu_virtual_address == 0) {
    return true;
  }
  const ResourceSemanticState *best_resource = nullptr;
  trace::ObjectId best_resource_object_id = 0;
  for (const auto &[resource_object_id, resource] : resources_) {
    if (resource.gpu_virtual_address == 0 ||
        resource.create_sequence > sequence ||
        gpu_virtual_address < resource.gpu_virtual_address) {
      continue;
    }
    const auto offset = gpu_virtual_address - resource.gpu_virtual_address;
    if (offset >= resource.width) {
      continue;
    }
    if (!best_resource || resource.create_sequence > best_resource->create_sequence) {
      best_resource = &resource;
      best_resource_object_id = resource_object_id;
    }
  }
  if (best_resource) {
    binding.resource_object_id = best_resource_object_id;
    binding.offset = gpu_virtual_address - best_resource->gpu_virtual_address;
    return true;
  }
  error = "GPU virtual address does not map to a captured resource";
  return false;
}

bool D3D12ReplayBackend::parse_texture_copy_location_payload(
    const trace::EventRecord &event,
    const std::string &payload_name,
    TextureCopyLocation &location,
    std::string &error) const
{
  json payload;
  if (!payload_to_json(event, payload, error)) {
    return false;
  }
  const auto value_it = payload.find(payload_name);
  if (value_it == payload.end()) {
    error = record_prefix(event) + ": missing texture copy location " + payload_name;
    return false;
  }
  const auto &value = *value_it;
  location = TextureCopyLocation{};
  if (value.is_null()) {
    return true;
  }
  if (!value.is_object()) {
    error = record_prefix(event) + ": texture copy location must be an object";
    return false;
  }
  location.resource_object_id = json_object_id(value.value("resource_object_id", json(nullptr)));
  location.type = payload_u32(value, "type");
  if (location.type == 0) {
    if (!payload_has(value, "subresource_index")) {
      error = record_prefix(event) + ": texture copy subresource location missing subresource_index";
      return false;
    }
    location.subresource_index = payload_u32(value, "subresource_index");
  } else if (location.type == 1) {
    const auto footprint = value.find("placed_footprint");
    if (footprint == value.end() || !footprint->is_object()) {
      error = record_prefix(event) + ": texture copy placed location missing placed_footprint";
      return false;
    }
    location.footprint_offset = payload_u64(*footprint, "offset");
    location.footprint_format = payload_u32(*footprint, "format");
    location.footprint_width = payload_u32(*footprint, "width");
    location.footprint_height = payload_u32(*footprint, "height");
    location.footprint_depth = payload_u32(*footprint, "depth");
    location.footprint_row_pitch = payload_u32(*footprint, "row_pitch");
    if (location.footprint_width == 0 || location.footprint_height == 0 || location.footprint_depth == 0 ||
        location.footprint_row_pitch == 0) {
      error = record_prefix(event) + ": texture copy placed footprint is incomplete";
      return false;
    }
  } else {
    error = record_prefix(event) + ": unsupported texture copy location type";
    return false;
  }
  return true;
}

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
  semantic_calls_seen_ = 0;
  draw_calls_seen_ = 0;
  dispatch_calls_seen_ = 0;
  last_sequence_ = 0;
  bundle_root_ = reader.layout().root_path;
  present_frames_.clear();
  present_semantics_.clear();
  frame_semantics_.clear();
  command_lists_.clear();
  command_queues_.clear();
  command_allocators_.clear();
  devices_.clear();
  descriptor_heaps_.clear();
  descriptors_.clear();
  command_signatures_.clear();
  resources_.clear();
  replay_commands_.clear();

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
    if (object.kind == trace::ObjectKind::CommandList) {
      command_lists_.emplace(object.object_id, CommandListSemanticState{});
    }
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

  if (!index_present_frames(reader)) {
    return false;
  }
  if (!index_present_semantics(reader)) {
    return false;
  }
  if (!validate_present_boundaries(reader)) {
    return false;
  }
  if (!validate_frame_boundaries(reader)) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool D3D12ReplayBackend::index_present_frame_event(const trace::EventRecord &event, bool replace_existing)
{
  if (event.kind != trace::EventKind::ResourceBlob) {
    return true;
  }

  json payload;
  if (!payload_to_json(event, payload, last_error_)) {
    return false;
  }

  const json &frame_payload = payload.contains("payload") && payload["payload"].is_object() ? payload["payload"] : payload;

  const std::string frame_path = payload_string(frame_payload, "frame_path");
  if (frame_path.empty()) {
    return true;
  }

  if (event.object_debug_name != "D3D12PresentFrame") {
    last_error_ = record_prefix(event) + ": resource blob has frame_path but is not a D3D12PresentFrame";
    return false;
  }

  PresentFrame frame;
  frame.relative_path = frame_path;
  if (!require_payload_key(event, frame_payload, "frame_index", last_error_) ||
      !require_payload_key(event, frame_payload, "width", last_error_) ||
      !require_payload_key(event, frame_payload, "height", last_error_) ||
      !require_payload_key(event, frame_payload, "row_pitch", last_error_) ||
      !require_payload_key(event, frame_payload, "sync_interval", last_error_) ||
      !require_payload_key(event, frame_payload, "flags", last_error_)) {
    return false;
  }
  frame.frame_index = payload_u64(frame_payload, "frame_index");
  frame.width = payload_u32(frame_payload, "width");
  frame.height = payload_u32(frame_payload, "height");
  frame.row_pitch = payload_u32(frame_payload, "row_pitch");
  frame.sync_interval = payload_u32(frame_payload, "sync_interval");
  frame.flags = payload_u32(frame_payload, "flags");
  if (frame.width == 0 || frame.height == 0 || frame.row_pitch < frame.width * 4u) {
    last_error_ = record_prefix(event) + ": present frame payload is missing dimensions";
    return false;
  }
  const auto absolute_path = bundle_root_ / frame.relative_path;
  if (!std::filesystem::is_regular_file(absolute_path)) {
    last_error_ = record_prefix(event) + ": missing present frame asset " + frame.relative_path.generic_string();
    return false;
  }
  std::error_code file_size_error;
  const auto actual_size = std::filesystem::file_size(absolute_path, file_size_error);
  if (file_size_error) {
    last_error_ = record_prefix(event) + ": failed to stat present frame asset " + frame.relative_path.generic_string();
    return false;
  }
  const auto expected_size = static_cast<std::uintmax_t>(frame.row_pitch) * static_cast<std::uintmax_t>(frame.height);
  if (actual_size != expected_size) {
    last_error_ = record_prefix(event) + ": present frame asset size does not match row_pitch * height";
    return false;
  }

  const auto existing = present_frames_.find(frame.frame_index);
  if (existing != present_frames_.end() && !replace_existing) {
    const auto &existing_frame = existing->second;
    if (existing_frame.relative_path != frame.relative_path ||
        existing_frame.width != frame.width ||
        existing_frame.height != frame.height ||
        existing_frame.row_pitch != frame.row_pitch ||
        existing_frame.sync_interval != frame.sync_interval ||
        existing_frame.flags != frame.flags) {
      last_error_ = record_prefix(event) + ": duplicate present frame index has different payload";
      return false;
    }
    return true;
  }

  present_frames_[frame.frame_index] = std::move(frame);
  return true;
}

bool D3D12ReplayBackend::index_present_frames(const trace::TraceBundleReader &reader)
{
  for (const auto &event : reader.events()) {
    if (!index_present_frame_event(event, true)) {
      return false;
    }
  }
  return true;
}

bool D3D12ReplayBackend::index_present_semantics(const trace::TraceBundleReader &reader)
{
  std::uint64_t present_index = 0;
  for (const auto &event : reader.events()) {
    if (event.kind != trace::EventKind::Call || event.callsite.function_name != "IDXGISwapChain::Present") {
      continue;
    }

    json payload;
    if (!payload_to_json(event, payload, last_error_)) {
      return false;
    }
    if (!require_payload_key(event, payload, "sync_interval", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }

    if (!require_payload_key(event, payload, "frame_index", last_error_)) {
      return false;
    }
    const auto frame_index = payload_u64(payload, "frame_index");
    if (frame_index != present_index) {
      last_error_ = record_prefix(event) + ": IDXGISwapChain::Present frame_index is not contiguous";
      return false;
    }
    auto &semantic = present_semantics_[frame_index];
    if (semantic.has_call) {
      last_error_ = record_prefix(event) + ": duplicate IDXGISwapChain::Present frame_index";
      return false;
    }
    semantic.frame_index = frame_index;
    semantic.call_sequence = event.callsite.sequence;
    semantic.sync_interval = payload_u32(payload, "sync_interval");
    semantic.flags = payload_u32(payload, "flags");
    semantic.result_code = event.callsite.result_code;
    semantic.has_call = true;
    ++present_index;
  }
  if (present_index == 0 && !present_frames_.empty()) {
    last_error_ = "D3D12 present frames exist without captured IDXGISwapChain::Present calls";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::validate_present_semantic_match(
    const trace::EventRecord &event,
    std::uint64_t frame_index,
    std::uint32_t sync_interval,
    std::uint32_t flags)
{
  auto semantic_it = present_semantics_.find(frame_index);
  if (semantic_it == present_semantics_.end() || !semantic_it->second.has_call) {
    last_error_ = record_prefix(event) + ": missing IDXGISwapChain::Present call for frame_index " + std::to_string(frame_index);
    return false;
  }

  auto &semantic = semantic_it->second;
  if (semantic.sync_interval != sync_interval || semantic.flags != flags) {
    std::ostringstream message;
    message << record_prefix(event) << ": Present boundary does not match captured IDXGISwapChain::Present parameters for frame_index "
            << frame_index;
    last_error_ = message.str();
    return false;
  }
  if (semantic.result_code < 0) {
    std::ostringstream message;
    message << record_prefix(event) << ": captured IDXGISwapChain::Present failed for frame_index " << frame_index;
    last_error_ = message.str();
    return false;
  }
  if (semantic.has_boundary) {
    last_error_ = record_prefix(event) + ": duplicate Present boundary for frame_index " + std::to_string(frame_index);
    return false;
  }

  semantic.boundary_sequence = event.callsite.sequence;
  semantic.has_boundary = true;
  return true;
}

bool D3D12ReplayBackend::validate_present_boundaries(const trace::TraceBundleReader &reader)
{
  std::uint64_t present_index = 0;
  for (const auto &event : reader.events()) {
    if (event.kind != trace::EventKind::Boundary || event.boundary != trace::BoundaryKind::Present) {
      continue;
    }

    json payload;
    if (!payload_to_json(event, payload, last_error_)) {
      return false;
    }
    if (!require_payload_key(event, payload, "frame_index", last_error_)) {
      return false;
    }
    const auto frame_index = payload_u64(payload, "frame_index");
    if (frame_index != present_index) {
      last_error_ = record_prefix(event) + ": Present boundary frame_index is not contiguous";
      return false;
    }
    if (!require_payload_key(event, payload, "sync_interval", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    const auto sync_interval = payload_u32(payload, "sync_interval");
    const auto flags = payload_u32(payload, "flags");
    if (!validate_present_semantic_match(event, frame_index, sync_interval, flags)) {
      return false;
    }
    const auto frame_it = present_frames_.find(frame_index);
    if (frame_it != present_frames_.end()) {
      const auto &frame = frame_it->second;
      if (frame.sync_interval != sync_interval || frame.flags != flags) {
        std::ostringstream message;
        message << record_prefix(event) << ": D3D12PresentFrame metadata does not match captured Present parameters for frame_index "
                << frame_index;
        last_error_ = message.str();
        return false;
      }
    }
    ++present_index;
  }
  if (present_index != present_semantics_.size()) {
    last_error_ = "D3D12 present boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (!present_frames_.empty() && present_index != present_frames_.size()) {
    last_error_ = "D3D12 present frame asset count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::validate_frame_boundaries(const trace::TraceBundleReader &reader)
{
  std::uint64_t frame_begin_index = 0;
  std::uint64_t frame_end_index = 0;
  std::unordered_map<std::uint64_t, bool> open_frames;
  std::unordered_set<std::uint64_t> presented_frames;

  for (const auto &event : reader.events()) {
    if (event.kind == trace::EventKind::Boundary && event.boundary == trace::BoundaryKind::Present) {
      json payload;
      if (!payload_to_json(event, payload, last_error_)) {
        return false;
      }
      if (!require_payload_key(event, payload, "frame_index", last_error_)) {
        return false;
      }
      const auto frame_index = payload_u64(payload, "frame_index");
      if (open_frames.find(frame_index) == open_frames.end()) {
        last_error_ = record_prefix(event) + ": Present boundary is missing matching FrameBegin";
        return false;
      }
      if (presented_frames.find(frame_index) != presented_frames.end()) {
        last_error_ = record_prefix(event) + ": duplicate Present boundary for frame_index";
        return false;
      }
      presented_frames.insert(frame_index);
      auto &frame = frame_semantics_[frame_index];
      frame.frame_index = frame_index;
      frame.has_present = true;
      continue;
    }

    if (event.kind != trace::EventKind::Boundary || event.boundary != trace::BoundaryKind::Frame) {
      continue;
    }

    json payload;
    if (!payload_to_json(event, payload, last_error_)) {
      return false;
    }
    const auto label = payload_string(payload, "label");
    if (label != "FrameBegin" && label != "FrameEnd") {
      continue;
    }
    if (!require_payload_key(event, payload, "frame_index", last_error_)) {
      return false;
    }
    const auto frame_index = payload_u64(payload, "frame_index");
    auto &frame = frame_semantics_[frame_index];
    frame.frame_index = frame_index;

    if (label == "FrameBegin") {
      if (frame_index != frame_begin_index) {
        last_error_ = record_prefix(event) + ": FrameBegin frame_index is not contiguous";
        return false;
      }
      if (open_frames.find(frame_index) != open_frames.end() || frame.has_begin) {
        last_error_ = record_prefix(event) + ": duplicate FrameBegin for frame_index";
        return false;
      }
      frame.begin_sequence = event.callsite.sequence;
      frame.has_begin = true;
      open_frames.emplace(frame_index, true);
      ++frame_begin_index;
      continue;
    }

    if (frame_index != frame_end_index) {
      last_error_ = record_prefix(event) + ": FrameEnd frame_index is not contiguous";
      return false;
    }
    const auto open_frame = open_frames.find(frame_index);
    if (open_frame == open_frames.end() || !frame.has_begin) {
      last_error_ = record_prefix(event) + ": FrameEnd is missing matching FrameBegin";
      return false;
    }
    if (presented_frames.find(frame_index) == presented_frames.end() || !frame.has_present) {
      last_error_ = record_prefix(event) + ": FrameEnd is missing matching Present boundary";
      return false;
    }
    if (frame.has_end) {
      last_error_ = record_prefix(event) + ": duplicate FrameEnd for frame_index";
      return false;
    }
    frame.end_sequence = event.callsite.sequence;
    frame.has_end = true;
    open_frames.erase(open_frame);
    presented_frames.erase(frame_index);
    ++frame_end_index;
  }

  if (frame_begin_index != frame_end_index) {
    last_error_ = "D3D12 frame boundary count does not match";
    return false;
  }
  if (frame_begin_index != present_semantics_.size()) {
    last_error_ = "D3D12 frame boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (!open_frames.empty() || !presented_frames.empty()) {
    last_error_ = "D3D12 frame boundaries are not fully closed";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::validate_replay_closure()
{
  if (submissions_.has_open_batch()) {
    last_error_ = "D3D12 replay ended with an open submission batch";
    return false;
  }

  if (replay_commands_.empty()) {
    last_error_ = "D3D12 replay did not collect command records";
    return false;
  }

  std::unordered_set<std::size_t> submitted_command_indices;
  for (const auto &batch : submissions_.completed_batches()) {
    if (batch.queue_object_id == 0 || batch.execute_sequence == 0) {
      last_error_ = "D3D12 submission batch is missing queue or ExecuteCommandLists sequence";
      return false;
    }
    if (batch.command_list_ids.empty()) {
      last_error_ = "D3D12 submission batch has no command lists";
      return false;
    }

    for (const auto command_list_id : batch.command_list_ids) {
      constexpr auto invalid_index = std::numeric_limits<std::size_t>::max();
      auto current_begin_index = invalid_index;
      auto closed_begin_index = invalid_index;
      auto closed_end_index = invalid_index;

      for (std::size_t command_index = 0; command_index < replay_commands_.size(); ++command_index) {
        const auto &command = replay_commands_[command_index];
        if (command.command_list_object_id != command_list_id) {
          continue;
        }
        if (command.sequence >= batch.execute_sequence) {
          break;
        }

        if (command.kind == ReplayCommandKind::BeginCommandList) {
          current_begin_index = command_index;
          continue;
        }
        if (current_begin_index == invalid_index) {
          continue;
        }
        if (command.kind == ReplayCommandKind::EndCommandList) {
          closed_begin_index = current_begin_index;
          closed_end_index = command_index;
          current_begin_index = invalid_index;
        }
      }

      if (closed_begin_index == invalid_index || closed_end_index == invalid_index) {
        last_error_ = "D3D12 submission references a command list with no closed replay interval";
        return false;
      }

      for (std::size_t command_index = closed_begin_index; command_index <= closed_end_index; ++command_index) {
        if (replay_commands_[command_index].command_list_object_id == command_list_id) {
          submitted_command_indices.insert(command_index);
        }
      }
    }
  }

  if (submissions_.completed_batches().empty()) {
    last_error_ = "D3D12 replay did not observe any ExecuteCommandLists submissions";
    return false;
  }

  for (std::size_t index = 0; index < replay_commands_.size(); ++index) {
    const auto &command = replay_commands_[index];
    if (command.command_list_object_id == 0) {
      continue;
    }
    if (!replay_command_requires_submission(command.kind)) {
      continue;
    }
    if (submitted_command_indices.find(index) == submitted_command_indices.end()) {
      std::ostringstream message;
      message << "D3D12 replay command sequence " << command.sequence
              << " was recorded but never submitted";
      last_error_ = message.str();
      return false;
    }
  }

  return true;
}

bool D3D12ReplayBackend::finalize_replay()
{
  if (!validate_replay_closure()) {
    return false;
  }
  D3D12NativeReplayer native_replayer(*this);
  return native_replayer.replay(last_error_);
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
    {
      json boundary_payload;
      if (!payload_to_json(event, boundary_payload, last_error_)) {
        return false;
      }
      const auto frame_index = payload_u64(boundary_payload, "frame_index", presents_seen_);
      const auto semantic = present_semantics_.find(frame_index);
      (void)semantic;
      ++presents_seen_;
      submissions_.mark_present(event.callsite.sequence);
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    }
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
    if (event.object_kind == trace::ObjectKind::CommandList) {
      command_lists_.emplace(event.object_id, CommandListSemanticState{});
    }
    ++commands_replayed_;
    return true;
  }

  if (event.kind == trace::EventKind::ObjectDestroy) {
    if (event.object_id != 0) {
      objects_.forget(event.object_id);
      command_lists_.erase(event.object_id);
      resources_.erase(event.object_id);
      command_signatures_.erase(event.object_id);
      command_queues_.erase(event.object_id);
      command_allocators_.erase(event.object_id);
      devices_.erase(event.object_id);
    }
    ++commands_replayed_;
    return true;
  }

  if (event.kind == trace::EventKind::ResourceBlob) {
    if (!index_present_frame_event(event, false)) {
      return false;
    }
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
  const auto command_list_id = event.object_refs.empty() ? trace::ObjectId{} : event.object_refs.front();
  auto command_list_it = command_lists_.find(command_list_id);
  auto command_list_state = command_list_it == command_lists_.end() ? nullptr : &command_list_it->second;
  const auto replay_command_kind = replay_command_kind_for(function_name);
  const bool is_command_list_command =
      command_list_state != nullptr && replay_command_kind != ReplayCommandKind::Unknown;
  if (is_command_list_command && replay_command_kind != ReplayCommandKind::BeginCommandList &&
      !command_list_state->recording) {
    last_error_ = record_prefix(event) + ": command-list call outside a recording interval";
    return false;
  }
  if (replay_command_kind != ReplayCommandKind::Unknown) {
    ReplayCommandRecord command;
    command.kind = replay_command_kind;
    command.sequence = event.callsite.sequence;
    command.command_list_object_id = command_list_state ? command_list_id : trace::ObjectId{};
    if (function_name == "ID3D12Device::CreateCommandList" && !event.object_refs.empty()) {
      command.command_list_object_id = event.object_refs.back();
    }
    command.object_refs = event.object_refs;
    command.function_name = function_name;
    command.payload = event.payload;
    replay_commands_.push_back(std::move(command));
    if (command_list_state) {
      command_list_state->command_indices.push_back(replay_commands_.size() - 1);
    }
  }

  if (function_name == "D3D12CreateDevice") {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": D3D12CreateDevice missing device object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs.front(), trace::ObjectKind::Device)) {
      last_error_ = record_prefix(event) + ": D3D12CreateDevice output object is not a device";
      return false;
    }
    if (!require_payload_key(event, payload, "minimum_feature_level", last_error_)) {
      return false;
    }
    if (event.callsite.result_code < 0) {
      last_error_ = record_prefix(event) + ": captured D3D12CreateDevice failed";
      return false;
    }
    DeviceSemanticState device;
    device.device_object_id = event.object_refs.front();
    device.create_sequence = event.callsite.sequence;
    device.minimum_feature_level = payload_u32(payload, "minimum_feature_level");
    devices_[device.device_object_id] = device;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
      function_name == "ID3D12Device::CreateComputePipelineState") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": pipeline create missing pipeline state object id";
      return false;
    }
    const auto pipeline_path = payload_string(payload, "pipeline_path");
    if (pipeline_path.empty()) {
      last_error_ = record_prefix(event) + ": pipeline create missing pipeline_path";
      return false;
    }
    json pipeline_asset;
    if (!read_pipeline_asset_json(bundle_root_, pipeline_path, &pipeline_asset, last_error_)) {
      return false;
    }
    PipelineSemanticState pipeline;
    pipeline.pipeline_state_object_id = event.object_refs[1];
    pipeline.create_sequence = event.callsite.sequence;
    pipeline.graphics = function_name == "ID3D12Device::CreateGraphicsPipelineState";
    pipeline.relative_path = pipeline_path;
    pipeline.blob_refs = event.blob_refs;
    pipeline.root_signature_object_id = json_object_id(pipeline_asset.value("root_signature_object_id", json(nullptr)));
    if (pipeline.root_signature_object_id != 0 && root_signatures_.find(pipeline.root_signature_object_id) == root_signatures_.end()) {
      last_error_ = record_prefix(event) + ": pipeline asset references an unknown root signature";
      return false;
    }
    pipeline.node_mask = payload_u32(pipeline_asset, "node_mask");
    pipeline.flags = payload_u32(pipeline_asset, "flags");
    pipeline.sample_mask = payload_u32(pipeline_asset, "sample_mask");
    pipeline.primitive_topology_type = payload_u32(pipeline_asset, "primitive_topology_type");
    pipeline.num_render_targets = payload_u32(pipeline_asset, "num_render_targets");
    pipeline.dsv_format = payload_u32(pipeline_asset, "dsv_format");
    if (const auto sample_desc = pipeline_asset.find("sample_desc"); sample_desc != pipeline_asset.end()) {
      if (!sample_desc->is_object()) {
        last_error_ = record_prefix(event) + ": pipeline sample_desc must be an object";
        return false;
      }
      pipeline.sample_count = payload_u32(*sample_desc, "count");
      pipeline.sample_quality = payload_u32(*sample_desc, "quality");
      if (pipeline.sample_count == 0) {
        last_error_ = record_prefix(event) + ": pipeline sample_desc count must be non-zero";
        return false;
      }
    }
    pipeline.ib_strip_cut_value = payload_u32(pipeline_asset, "ib_strip_cut_value");
    if (pipeline.graphics) {
      const auto input_layout = pipeline_asset.find("input_layout");
      const auto blend_state = pipeline_asset.find("blend_state");
      const auto rasterizer_state = pipeline_asset.find("rasterizer_state");
      const auto depth_stencil_state = pipeline_asset.find("depth_stencil_state");
      if (input_layout == pipeline_asset.end() || !input_layout->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing input_layout";
        return false;
      }
      if (blend_state == pipeline_asset.end() || !blend_state->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing blend_state";
        return false;
      }
      if (rasterizer_state == pipeline_asset.end() || !rasterizer_state->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing rasterizer_state";
        return false;
      }
      if (depth_stencil_state == pipeline_asset.end() || !depth_stencil_state->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing depth_stencil_state";
        return false;
      }
      pipeline.has_input_layout = true;
      pipeline.has_blend_state = true;
      pipeline.has_rasterizer_state = true;
      pipeline.has_depth_stencil_state = true;
      pipeline.input_element_count = payload_u32(*input_layout, "element_count");
      const auto elements = input_layout->find("elements");
      if (elements == input_layout->end() || !elements->is_array() || elements->size() != pipeline.input_element_count) {
        last_error_ = record_prefix(event) + ": graphics pipeline input layout element count mismatch";
        return false;
      }
    }
    const auto rtv_formats = pipeline_asset.find("rtv_formats");
    if (rtv_formats != pipeline_asset.end()) {
      if (!rtv_formats->is_array()) {
        last_error_ = record_prefix(event) + ": pipeline rtv_formats must be an array";
        return false;
      }
      for (const auto &format : *rtv_formats) {
        pipeline.rtv_formats.push_back(static_cast<std::uint32_t>(json_u64(format)));
      }
    }
    pipeline.has_vertex_shader = pipeline_asset.contains("vs");
    pipeline.has_pixel_shader = pipeline_asset.contains("ps");
    pipeline.has_compute_shader = pipeline_asset.contains("cs");
    if (pipeline.graphics && !pipeline.has_vertex_shader) {
      last_error_ = record_prefix(event) + ": graphics pipeline asset missing vertex shader entry";
      return false;
    }
    if (pipeline.graphics && pipeline.root_signature_object_id == 0) {
      last_error_ = record_prefix(event) + ": graphics pipeline asset missing root signature object id";
      return false;
    }
    if (!pipeline.graphics && !pipeline.has_compute_shader) {
      last_error_ = record_prefix(event) + ": compute pipeline asset missing compute shader entry";
      return false;
    }
    if (!pipeline.graphics && pipeline.root_signature_object_id == 0) {
      last_error_ = record_prefix(event) + ": compute pipeline asset missing root signature object id";
      return false;
    }
    pipelines_[pipeline.pipeline_state_object_id] = std::move(pipeline);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateRootSignature") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateRootSignature missing root signature object id";
      return false;
    }
    if (!require_payload_key(event, payload, "node_mask", last_error_) ||
        !require_payload_key(event, payload, "bytecode_size", last_error_)) {
      return false;
    }
    RootSignatureSemanticState root_signature;
    root_signature.root_signature_object_id = event.object_refs[1];
    root_signature.create_sequence = event.callsite.sequence;
    root_signature.node_mask = payload_u32(payload, "node_mask");
    root_signature.bytecode_size = payload_u64(payload, "bytecode_size");
    root_signature.blob_refs = event.blob_refs;
    const auto root_signature_path = payload_string(payload, "root_signature_path");
    if (!root_signature_path.empty()) {
      root_signature.relative_path = root_signature_path;
      const auto absolute_root_signature_path = bundle_root_ / root_signature.relative_path;
      if (!std::filesystem::is_regular_file(absolute_root_signature_path)) {
        last_error_ = record_prefix(event) + ": missing root signature asset " + root_signature_path;
        return false;
      }
      if (event.blob_refs.empty()) {
        last_error_ = record_prefix(event) + ": root signature asset is missing blob_refs";
        return false;
      }
      std::string asset_error;
      if (!read_exact_asset_bytes(
              absolute_root_signature_path,
              root_signature.bytecode_size,
              root_signature.bytes,
              asset_error)) {
        last_error_ = record_prefix(event) + ": " + asset_error;
        return false;
      }
    } else if (root_signature.bytecode_size != 0) {
      last_error_ = record_prefix(event) + ": CreateRootSignature missing root_signature_path";
      return false;
    }
    root_signatures_[root_signature.root_signature_object_id] = std::move(root_signature);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandQueue") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandQueue missing queue object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs[1], trace::ObjectKind::CommandQueue)) {
      last_error_ = record_prefix(event) + ": CreateCommandQueue output object is not a command queue";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    CommandQueueSemanticState queue;
    queue.queue_object_id = event.object_refs[1];
    queue.create_sequence = event.callsite.sequence;
    queue.type = payload_u32(payload, "type");
    queue.priority = payload_i32(payload, "priority");
    queue.flags = payload_u32(payload, "flags");
    queue.node_mask = payload_u32(payload, "node_mask");
    command_queues_[queue.queue_object_id] = queue;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandAllocator") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandAllocator missing allocator object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs[1], trace::ObjectKind::CommandAllocator)) {
      last_error_ = record_prefix(event) + ": CreateCommandAllocator output object is not a command allocator";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_)) {
      return false;
    }
    CommandAllocatorSemanticState allocator;
    allocator.allocator_object_id = event.object_refs[1];
    allocator.create_sequence = event.callsite.sequence;
    allocator.type = payload_u32(payload, "type");
    command_allocators_[allocator.allocator_object_id] = allocator;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateFence") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateFence missing fence object id";
      return false;
    }
    if (!require_payload_key(event, payload, "initial_value", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    FenceSemanticState fence;
    fence.fence_object_id = event.object_refs[1];
    fence.create_sequence = event.callsite.sequence;
    fence.initial_value = payload_u64(payload, "initial_value");
    fence.current_value = fence.initial_value;
    fence.completed_value = fence.initial_value;
    fence.flags = payload_u32(payload, "flags");
    fences_[fence.fence_object_id] = fence;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommittedResource") {
    if (!require_payload_key(event, payload, "resource_desc", last_error_) ||
        !require_payload_key(event, payload, "initial_state", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateCommittedResource missing resource object id";
      return false;
    }
    const auto &resource_desc = payload["resource_desc"];
    if (!resource_desc.is_object()) {
      last_error_ = record_prefix(event) + ": resource_desc must be an object";
      return false;
    }
    static constexpr const char *required_resource_desc_keys[] = {
        "dimension",
        "alignment",
        "width",
        "height",
        "depth_or_array_size",
        "mip_levels",
        "format",
        "sample_count",
        "sample_quality",
        "layout",
        "flags",
    };
    for (const char *key : required_resource_desc_keys) {
      if (!require_payload_key(event, resource_desc, key, last_error_)) {
        return false;
      }
    }
    ResourceSemanticState resource;
    resource.resource_object_id = event.object_refs[1];
    resource.create_sequence = event.callsite.sequence;
    resource.heap_type = payload_u32(payload, "heap_type");
    resource.heap_flags = payload_u32(payload, "heap_flags");
    resource.initial_state = payload_u32(payload, "initial_state");
    resource.current_state = resource.initial_state;
    resource.gpu_virtual_address = payload_u64(payload, "gpu_virtual_address");
    resource.swapchain_back_buffer = payload.value("swapchain_back_buffer", false);
    resource.swapchain_buffer_index = payload_u32(payload, "buffer_index");
    resource.width = payload_u64(resource_desc, "width");
    resource.alignment = payload_u64(resource_desc, "alignment");
    resource.dimension = payload_u32(resource_desc, "dimension");
    resource.height = payload_u32(resource_desc, "height");
    resource.depth_or_array_size = payload_u32(resource_desc, "depth_or_array_size");
    resource.mip_levels = payload_u32(resource_desc, "mip_levels");
    resource.format = payload_u32(resource_desc, "format");
    resource.sample_count = payload_u32(resource_desc, "sample_count");
    resource.sample_quality = payload_u32(resource_desc, "sample_quality");
    resource.layout = payload_u32(resource_desc, "layout");
    resource.flags = payload_u32(resource_desc, "flags");
    if (resource.width == 0 || resource.height == 0 || resource.depth_or_array_size == 0 ||
        resource.mip_levels == 0 || resource.sample_count == 0) {
      last_error_ = record_prefix(event) + ": resource_desc has invalid dimensions or sample count";
      return false;
    }
    const auto optimized_clear_value = payload.find("optimized_clear_value");
    if (optimized_clear_value != payload.end()) {
      if (!optimized_clear_value->is_null() && !optimized_clear_value->is_object()) {
        last_error_ = record_prefix(event) + ": optimized_clear_value must be null or an object";
        return false;
      }
      if (optimized_clear_value->is_object()) {
        if (!require_payload_key(event, *optimized_clear_value, "format", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "color", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "depth", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "stencil", last_error_)) {
          return false;
        }
        const auto color = optimized_clear_value->find("color");
        if (!color->is_array() || color->size() != 4) {
          last_error_ = record_prefix(event) + ": optimized_clear_value color must be an array of four numbers";
          return false;
        }
        resource.has_optimized_clear_value = true;
        resource.optimized_clear_format = payload_u32(*optimized_clear_value, "format");
        for (std::size_t index = 0; index < 4; ++index) {
          if (!(*color)[index].is_number()) {
            last_error_ = record_prefix(event) + ": optimized_clear_value color entries must be numeric";
            return false;
          }
          resource.optimized_clear_color[index] = (*color)[index].get<float>();
        }
        resource.optimized_clear_depth = payload_float(*optimized_clear_value, "depth");
        resource.optimized_clear_stencil = payload_u32(*optimized_clear_value, "stencil");
      }
    }
    resources_[resource.resource_object_id] = std::move(resource);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateConstantBufferView" ||
             function_name == "ID3D12Device::CreateShaderResourceView" ||
             function_name == "ID3D12Device::CreateUnorderedAccessView" ||
             function_name == "ID3D12Device::CreateRenderTargetView" ||
             function_name == "ID3D12Device::CreateDepthStencilView") {
    if (!require_payload_key(event, payload, "descriptor", last_error_)) {
      return false;
    }
    DescriptorSemanticState descriptor;
    descriptor.kind = function_name.substr(function_name.find("Create") + 6);
    descriptor.create_sequence = event.callsite.sequence;
    descriptor.descriptor = payload_u64(payload, "descriptor");
    if (event.object_refs.size() >= 2) {
      descriptor.resource_object_id = event.object_refs[1];
    }
    if (event.object_refs.size() >= 3) {
      descriptor.counter_resource_object_id = event.object_refs[2];
    }
    if (!resolve_descriptor_binding_at(descriptor.descriptor, false, event.callsite.sequence, descriptor.binding, last_error_)) {
      last_error_ = record_prefix(event) + ": " + last_error_;
      return false;
    }
    descriptor.format = payload_u32(payload, "format");
    descriptor.view_dimension = payload_u32(payload, "view_dimension");
    descriptor.flags = payload_u32(payload, "flags");
    descriptor.shader_4_component_mapping = payload_u32(payload, "shader_4_component_mapping");
    descriptor.buffer_location = payload_u64(payload, "buffer_location");
    descriptor.size_in_bytes = payload_u32(payload, "size_in_bytes");
    if (function_name == "ID3D12Device::CreateConstantBufferView" && descriptor.buffer_location != 0) {
      if (!resolve_gpu_virtual_address_at(descriptor.buffer_location, event.callsite.sequence, descriptor.buffer, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
    }
    const auto view = payload.find("view");
    if (view != payload.end()) {
      if (!view->is_null() && !view->is_object()) {
        last_error_ = record_prefix(event) + ": descriptor view payload must be an object";
        return false;
      }
      if (view->is_object()) {
        descriptor.first_element = payload_u64(*view, "first_element");
        descriptor.num_elements = payload_u32(*view, "num_elements");
        descriptor.structure_byte_stride = payload_u32(*view, "structure_byte_stride");
        descriptor.counter_offset_in_bytes = payload_u64(*view, "counter_offset_in_bytes");
        descriptor.flags = payload_u32(*view, "flags", descriptor.flags);
        descriptor.most_detailed_mip = payload_u32(*view, "most_detailed_mip");
        descriptor.mip_levels = payload_u32(*view, "mip_levels");
        descriptor.mip_slice = payload_u32(*view, "mip_slice");
        descriptor.plane_slice = payload_u32(*view, "plane_slice");
        descriptor.first_array_slice = payload_u32(*view, "first_array_slice");
        descriptor.array_size = payload_u32(*view, "array_size");
        descriptor.first_w_slice = payload_u32(*view, "first_w_slice");
        descriptor.w_size = payload_u32(*view, "w_size");
        descriptor.resource_min_lod_clamp = payload_float(*view, "resource_min_lod_clamp");
        if (payload_has(*view, "location")) {
          if (!resolve_gpu_virtual_address_at(payload_u64(*view, "location"), event.callsite.sequence, descriptor.raytracing_acceleration_structure, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
        }
      }
    }
    descriptors_.push_back(std::move(descriptor));
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateDescriptorHeap") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateDescriptorHeap missing heap object id";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_) ||
        !require_payload_key(event, payload, "num_descriptors", last_error_)) {
      return false;
    }
    DescriptorHeapSemanticState heap;
    heap.heap_object_id = event.object_refs[1];
    heap.create_sequence = event.callsite.sequence;
    heap.type = payload_u32(payload, "type");
    heap.num_descriptors = payload_u32(payload, "num_descriptors");
    heap.flags = payload_u32(payload, "flags");
    heap.node_mask = payload_u32(payload, "node_mask");
    heap.descriptor_size = payload_u32(payload, "descriptor_size");
    heap.cpu_start = payload_u64(payload, "cpu_start");
    heap.gpu_start = payload_u64(payload, "gpu_start");
    if (heap.num_descriptors == 0 || heap.descriptor_size == 0 || heap.cpu_start == 0) {
      last_error_ = record_prefix(event) + ": descriptor heap relocation metadata is incomplete";
      return false;
    }
    const bool shader_visible = descriptor_heap_type_is_shader_visible(heap.flags);
    if (shader_visible && !descriptor_heap_type_allows_shader_visibility(heap.type)) {
      last_error_ = record_prefix(event) + ": descriptor heap type cannot be shader-visible";
      return false;
    }
    if (shader_visible && heap.gpu_start == 0) {
      last_error_ = record_prefix(event) + ": shader-visible descriptor heap missing GPU start handle";
      return false;
    }
    if (!shader_visible && heap.gpu_start != 0) {
      last_error_ = record_prefix(event) + ": non shader-visible descriptor heap has GPU start handle";
      return false;
    }
    descriptor_heaps_[heap.heap_object_id] = std::move(heap);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandSignature") {
    if (event.object_refs.size() < 2 || event.object_refs.back() == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandSignature missing command signature object id";
      return false;
    }
    if (!require_payload_key(event, payload, "byte_stride", last_error_) ||
        !require_payload_key(event, payload, "argument_count", last_error_)) {
      return false;
    }
    CommandSignatureSemanticState signature;
    signature.command_signature_object_id = event.object_refs.back();
    signature.root_signature_object_id = event.object_refs.size() >= 3 ? event.object_refs[1] : trace::ObjectId{};
    signature.create_sequence = event.callsite.sequence;
    signature.byte_stride = payload_u32(payload, "byte_stride");
    signature.argument_count = payload_u32(payload, "argument_count");
    signature.node_mask = payload_u32(payload, "node_mask");
    const auto arguments = payload.find("arguments");
    if (arguments != payload.end()) {
      if (!arguments->is_array()) {
        last_error_ = record_prefix(event) + ": command signature arguments must be an array";
        return false;
      }
      if (arguments->size() != signature.argument_count) {
        last_error_ = record_prefix(event) + ": command signature argument_count does not match arguments payload";
        return false;
      }
      for (const auto &argument_payload : *arguments) {
        if (!argument_payload.is_object()) {
          last_error_ = record_prefix(event) + ": command signature argument must be an object";
          return false;
        }
        if (!require_payload_key(event, argument_payload, "type", last_error_)) {
          return false;
        }
        CommandSignatureArgument argument;
        argument.type = payload_u32(argument_payload, "type");
        argument.slot = payload_u32(argument_payload, "slot");
        argument.root_parameter_index = payload_u32(argument_payload, "root_parameter_index");
        argument.dest_offset_in32bit_values = payload_u32(argument_payload, "dest_offset_in32bit_values");
        argument.num32bit_values_to_set = payload_u32(argument_payload, "num32bit_values_to_set");
        if (argument.type == 3 && !payload_has(argument_payload, "slot")) {
          last_error_ = record_prefix(event) + ": vertex-buffer indirect argument missing slot";
          return false;
        }
        if (argument.type == 5 &&
            (!payload_has(argument_payload, "root_parameter_index") ||
             !payload_has(argument_payload, "dest_offset_in32bit_values") ||
             !payload_has(argument_payload, "num32bit_values_to_set"))) {
          last_error_ = record_prefix(event) + ": constant indirect argument is incomplete";
          return false;
        }
        if (command_signature_argument_uses_root_parameter(argument.type) &&
            !payload_has(argument_payload, "root_parameter_index")) {
          last_error_ = record_prefix(event) + ": root descriptor indirect argument missing root_parameter_index";
          return false;
        }
        signature.arguments.push_back(argument);
      }
    } else if (signature.argument_count != 0) {
      last_error_ = record_prefix(event) + ": CreateCommandSignature missing arguments payload";
      return false;
    }
    command_signatures_[signature.command_signature_object_id] = std::move(signature);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandList") {
    if (event.object_refs.size() < 3 || event.object_refs.back() == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandList missing command list object id";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_)) {
      return false;
    }
    const auto allocator_object_id = event.object_refs[1];
    if (allocator_object_id == 0 || command_allocators_.find(allocator_object_id) == command_allocators_.end()) {
      last_error_ = record_prefix(event) + ": CreateCommandList references an unknown command allocator";
      return false;
    }
    const auto initial_pipeline_state_object_id =
        event.object_refs.size() >= 4 ? event.object_refs[2] : trace::ObjectId{};
    if (initial_pipeline_state_object_id != 0 &&
        pipelines_.find(initial_pipeline_state_object_id) == pipelines_.end()) {
      last_error_ = record_prefix(event) + ": CreateCommandList references an unknown initial pipeline state";
      return false;
    }
    const auto command_list_object_id = event.object_refs.back();
    if (!object_ref_kind_matches(objects_, command_list_object_id, trace::ObjectKind::CommandList)) {
      last_error_ = record_prefix(event) + ": CreateCommandList output object is not a command list";
      return false;
    }
    const auto type = payload_u32(payload, "type");
    if (command_allocators_[allocator_object_id].type != type) {
      last_error_ = record_prefix(event) + ": CreateCommandList type does not match allocator type";
      return false;
    }
    auto &state = command_lists_[command_list_object_id];
    state = CommandListSemanticState{};
    state.recording = true;
    state.first_sequence = event.callsite.sequence;
    state.type = type;
    state.node_mask = payload_u32(payload, "node_mask");
    state.allocator_object_id = allocator_object_id;
    state.pipeline_state_object_id = initial_pipeline_state_object_id;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::Reset") {
    if (!command_list_state) {
      last_error_ = record_prefix(event) + ": Reset references an unknown command list";
      return false;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": Reset missing command allocator object id";
      return false;
    }
    const auto allocator_it = command_allocators_.find(event.object_refs[1]);
    if (allocator_it == command_allocators_.end()) {
      last_error_ = record_prefix(event) + ": Reset references an unknown command allocator";
      return false;
    }
    const auto previous_type = command_list_state->type;
    if (previous_type != 0 && allocator_it->second.type != previous_type) {
      last_error_ = record_prefix(event) + ": Reset allocator type does not match command list type";
      return false;
    }
    const auto initial_pipeline_state_object_id = event.object_refs.size() >= 3 ? event.object_refs[2] : trace::ObjectId{};
    if (initial_pipeline_state_object_id != 0 &&
        pipelines_.find(initial_pipeline_state_object_id) == pipelines_.end()) {
      last_error_ = record_prefix(event) + ": Reset references an unknown initial pipeline state";
      return false;
    }
    *command_list_state = CommandListSemanticState{};
    command_list_state->recording = true;
    command_list_state->first_sequence = event.callsite.sequence;
    command_list_state->type = allocator_it->second.type;
    command_list_state->allocator_object_id = event.object_refs[1];
    command_list_state->pipeline_state_object_id = initial_pipeline_state_object_id;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::Close") {
    if (command_list_state) {
      command_list_state->recording = false;
      command_list_state->close_sequence = event.callsite.sequence;
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetPipelineState") {
    if (command_list_state && event.object_refs.size() >= 2) {
      if (pipelines_.find(event.object_refs[1]) == pipelines_.end()) {
        last_error_ = record_prefix(event) + ": SetPipelineState references an unknown pipeline state";
        return false;
      }
      command_list_state->pipeline_state_object_id = event.object_refs[1];
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootSignature") {
    if (command_list_state && event.object_refs.size() >= 2) {
      if (root_signatures_.find(event.object_refs[1]) == root_signatures_.end()) {
        last_error_ = record_prefix(event) + ": SetGraphicsRootSignature references an unknown root signature";
        return false;
      }
      command_list_state->graphics_root_signature_object_id = event.object_refs[1];
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetComputeRootSignature") {
    if (command_list_state && event.object_refs.size() >= 2) {
      if (root_signatures_.find(event.object_refs[1]) == root_signatures_.end()) {
        last_error_ = record_prefix(event) + ": SetComputeRootSignature references an unknown root signature";
        return false;
      }
      command_list_state->compute_root_signature_object_id = event.object_refs[1];
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable") {
    if (!require_payload_key(event, payload, "root_parameter_index", last_error_) ||
        !require_payload_key(event, payload, "base_descriptor", last_error_)) {
      return false;
    }
    if (command_list_state) {
      DescriptorBinding binding;
      const auto base_descriptor = payload_u64(payload, "base_descriptor");
      if (!resolve_descriptor_binding_in_heaps_at(
              base_descriptor,
              true,
              command_list_state->descriptor_heap_object_ids,
              event.callsite.sequence,
              binding,
              last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      if (binding.heap_object_id == 0) {
        last_error_ = record_prefix(event) + ": root descriptor table references a null descriptor";
        return false;
      }
      const auto root_parameter_index = payload_u32(payload, "root_parameter_index");
      if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable") {
        command_list_state->graphics_root_tables[root_parameter_index] = binding;
      } else {
        command_list_state->compute_root_tables[root_parameter_index] = binding;
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant" ||
             function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants") {
    if (!require_payload_key(event, payload, "root_parameter_index", last_error_) ||
        !require_payload_key(event, payload, "constant_count", last_error_) ||
        !require_payload_key(event, payload, "dst_offset", last_error_) ||
        !require_payload_key(event, payload, "values", last_error_)) {
      return false;
    }
    const auto &values_payload = payload["values"];
    if (!values_payload.is_array()) {
      last_error_ = record_prefix(event) + ": root 32-bit constants values must be an array";
      return false;
    }
    const auto constant_count = payload_u32(payload, "constant_count");
    if (values_payload.size() != constant_count) {
      last_error_ = record_prefix(event) + ": root 32-bit constant_count does not match values payload";
      return false;
    }
    Root32BitConstantsBinding binding;
    binding.root_parameter_index = payload_u32(payload, "root_parameter_index");
    binding.dst_offset = payload_u32(payload, "dst_offset");
    binding.values.reserve(values_payload.size());
    for (const auto &value : values_payload) {
      if (!value.is_number_unsigned() && !value.is_number_integer()) {
        last_error_ = record_prefix(event) + ": root 32-bit constant value must be an integer";
        return false;
      }
      const auto constant = json_u64(value);
      if (constant > std::numeric_limits<std::uint32_t>::max()) {
        last_error_ = record_prefix(event) + ": root 32-bit constant value exceeds uint32";
        return false;
      }
      binding.values.push_back(static_cast<std::uint32_t>(constant));
    }
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant" ||
          function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants") {
        command_list_state->graphics_root_constants[binding.root_parameter_index] = std::move(binding);
      } else {
        command_list_state->compute_root_constants[binding.root_parameter_index] = std::move(binding);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView" ||
             function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView" ||
             function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView") {
    if (!require_payload_key(event, payload, "root_parameter_index", last_error_) ||
        !require_payload_key(event, payload, "buffer_location", last_error_)) {
      return false;
    }
    if (command_list_state) {
      GpuVirtualAddressBinding binding;
      if (!resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), event.callsite.sequence, binding, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      const auto root_parameter_index = payload_u32(payload, "root_parameter_index");
      if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView") {
        command_list_state->graphics_root_constant_buffers[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView") {
        command_list_state->compute_root_constant_buffers[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView") {
        command_list_state->graphics_root_shader_resources[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView") {
        command_list_state->compute_root_shader_resources[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView") {
        command_list_state->graphics_root_unordered_accesses[root_parameter_index] = binding;
      } else {
        command_list_state->compute_root_unordered_accesses[root_parameter_index] = binding;
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetDescriptorHeaps") {
    if (!require_payload_key(event, payload, "heap_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->descriptor_heap_count = payload_u64(payload, "heap_count");
      command_list_state->descriptor_heap_object_ids.clear();
    }
    if (payload_u64(payload, "heap_count") != event.object_refs.size() - 1) {
      last_error_ = record_prefix(event) + ": SetDescriptorHeaps object refs do not match heap_count";
      return false;
    }
    for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
      const auto heap_it = descriptor_heaps_.find(event.object_refs[index]);
      if (heap_it == descriptor_heaps_.end()) {
        last_error_ = record_prefix(event) + ": SetDescriptorHeaps references an unknown descriptor heap";
        return false;
      }
      if (!descriptor_heap_type_is_shader_visible(heap_it->second.flags)) {
        last_error_ = record_prefix(event) + ": SetDescriptorHeaps references a non shader-visible descriptor heap";
        return false;
      }
      for (std::size_t prior_index = 1; prior_index < index; ++prior_index) {
        const auto prior_heap_it = descriptor_heaps_.find(event.object_refs[prior_index]);
        if (prior_heap_it != descriptor_heaps_.end() && prior_heap_it->second.type == heap_it->second.type) {
          last_error_ = record_prefix(event) + ": SetDescriptorHeaps binds duplicate heap types";
          return false;
        }
      }
      if (command_list_state) {
        command_list_state->descriptor_heap_object_ids.push_back(event.object_refs[index]);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::RSSetViewports") {
    if (!require_payload_key(event, payload, "viewport_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->viewports.clear();
      const auto viewport_count = payload_u32(payload, "viewport_count");
      const auto viewports = payload.find("viewports");
      if (viewports != payload.end()) {
        if (!viewports->is_array() || viewports->size() != viewport_count) {
          last_error_ = record_prefix(event) + ": RSSetViewports viewports count does not match viewport_count";
          return false;
        }
        for (const auto &viewport_payload : *viewports) {
          if (!viewport_payload.is_object()) {
            last_error_ = record_prefix(event) + ": RSSetViewports viewport entries must be objects";
            return false;
          }
          ViewportSemanticState viewport;
          viewport.x = payload_float(viewport_payload, "x");
          viewport.y = payload_float(viewport_payload, "y");
          viewport.width = payload_float(viewport_payload, "width");
          viewport.height = payload_float(viewport_payload, "height");
          viewport.min_depth = payload_float(viewport_payload, "min_depth");
          viewport.max_depth = payload_float(viewport_payload, "max_depth");
          command_list_state->viewports.push_back(viewport);
        }
      } else if (viewport_count > 0) {
        const auto first = payload.find("first");
        if (first == payload.end() || !first->is_object()) {
          last_error_ = record_prefix(event) + ": RSSetViewports missing viewport payload";
          return false;
        }
        ViewportSemanticState viewport;
        viewport.x = payload_float(*first, "x");
        viewport.y = payload_float(*first, "y");
        viewport.width = payload_float(*first, "width");
        viewport.height = payload_float(*first, "height");
        viewport.min_depth = payload_float(*first, "min_depth");
        viewport.max_depth = payload_float(*first, "max_depth");
        command_list_state->viewports.push_back(viewport);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::RSSetScissorRects") {
    if (!require_payload_key(event, payload, "rect_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->scissor_rects.clear();
      const auto rect_count = payload_u32(payload, "rect_count");
      const auto rects = payload.find("rects");
      if (rects != payload.end()) {
        if (!rects->is_array() || rects->size() != rect_count) {
          last_error_ = record_prefix(event) + ": RSSetScissorRects rects count does not match rect_count";
          return false;
        }
        for (const auto &rect_payload : *rects) {
          if (!rect_payload.is_object()) {
            last_error_ = record_prefix(event) + ": RSSetScissorRects rect entries must be objects";
            return false;
          }
          ScissorRectSemanticState rect;
          rect.left = payload_i32(rect_payload, "left");
          rect.top = payload_i32(rect_payload, "top");
          rect.right = payload_i32(rect_payload, "right");
          rect.bottom = payload_i32(rect_payload, "bottom");
          command_list_state->scissor_rects.push_back(rect);
        }
      } else if (rect_count > 0) {
        const auto first = payload.find("first");
        if (first == payload.end() || !first->is_object()) {
          last_error_ = record_prefix(event) + ": RSSetScissorRects missing rect payload";
          return false;
        }
        ScissorRectSemanticState rect;
        rect.left = payload_i32(*first, "left");
        rect.top = payload_i32(*first, "top");
        rect.right = payload_i32(*first, "right");
        rect.bottom = payload_i32(*first, "bottom");
        command_list_state->scissor_rects.push_back(rect);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::OMSetRenderTargets") {
    if (!require_payload_key(event, payload, "render_target_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->render_target_count = payload_u64(payload, "render_target_count");
      command_list_state->render_targets.clear();
      const auto render_targets = payload.find("render_targets");
      if (render_targets != payload.end()) {
        if (!render_targets->is_array()) {
          last_error_ = record_prefix(event) + ": render_targets must be an array";
          return false;
        }
        if (render_targets->size() != command_list_state->render_target_count) {
          last_error_ = record_prefix(event) + ": render_targets count does not match render_target_count";
          return false;
        }
        for (const auto &descriptor : *render_targets) {
          DescriptorBinding binding;
          if (!resolve_descriptor_binding_at(json_u64(descriptor), false, event.callsite.sequence, binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          command_list_state->render_targets.push_back(binding);
        }
      } else if (command_list_state->render_target_count > 0 && payload_u64(payload, "first_rtv") != 0) {
        DescriptorBinding binding;
        if (!resolve_descriptor_binding_at(payload_u64(payload, "first_rtv"), false, event.callsite.sequence, binding, last_error_)) {
          last_error_ = record_prefix(event) + ": " + last_error_;
          return false;
        }
        command_list_state->render_targets.push_back(binding);
      }
      const auto dsv = payload_u64(payload, "dsv");
      if (dsv != 0 && !resolve_descriptor_binding_at(dsv, false, event.callsite.sequence, command_list_state->depth_stencil, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView" ||
             function_name == "ID3D12GraphicsCommandList::ClearDepthStencilView") {
    if (!require_payload_key(event, payload, "descriptor", last_error_)) {
      return false;
    }
    if (!require_payload_key(event, payload, "rect_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      ++command_list_state->clear_count;
      if (function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView") {
        RenderTargetClearSemanticState clear;
        clear.sequence = event.callsite.sequence;
        clear.command_list_object_id = command_list_id;
        clear.rect_count = payload_u32(payload, "rect_count");
        if (!resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, event.callsite.sequence, clear.render_target, last_error_)) {
          last_error_ = record_prefix(event) + ": " + last_error_;
          return false;
        }
        const auto color = payload.find("color");
        if (color != payload.end()) {
          if (!color->is_array() || color->size() != 4) {
            last_error_ = record_prefix(event) + ": ClearRenderTargetView color must be an array of four numbers";
            return false;
          }
          for (std::size_t index = 0; index < 4; ++index) {
            if (!(*color)[index].is_number()) {
              last_error_ = record_prefix(event) + ": ClearRenderTargetView color entries must be numeric";
              return false;
            }
            clear.color[index] = (*color)[index].get<float>();
          }
          clear.has_color = true;
        }
        if (!parse_rect_array_payload(
                event,
                payload,
                "ClearRenderTargetView",
                clear.rect_count,
                clear.rects,
                last_error_)) {
          return false;
        }
        command_list_state->render_target_clears.push_back(clear);
      } else {
        if (!require_payload_key(event, payload, "clear_flags", last_error_) ||
            !require_payload_key(event, payload, "depth", last_error_) ||
            !require_payload_key(event, payload, "stencil", last_error_)) {
          return false;
        }
        DepthStencilClearSemanticState clear;
        clear.sequence = event.callsite.sequence;
        clear.command_list_object_id = command_list_id;
        clear.clear_flags = payload_u32(payload, "clear_flags");
        clear.depth = payload_float(payload, "depth");
        clear.stencil = payload_u32(payload, "stencil");
        clear.rect_count = payload_u32(payload, "rect_count");
        if (!resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, event.callsite.sequence, clear.depth_stencil, last_error_)) {
          last_error_ = record_prefix(event) + ": " + last_error_;
          return false;
        }
        if (!parse_rect_array_payload(
                event,
                payload,
                "ClearDepthStencilView",
                clear.rect_count,
                clear.rects,
                last_error_)) {
          return false;
        }
        command_list_state->depth_stencil_clears.push_back(clear);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::IASetPrimitiveTopology") {
    if (!require_payload_key(event, payload, "primitive_topology", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->primitive_topology = payload_u32(payload, "primitive_topology");
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::IASetVertexBuffers") {
    if (!require_payload_key(event, payload, "start_slot", last_error_) ||
        !require_payload_key(event, payload, "view_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->vertex_buffers.clear();
      const auto views = payload.find("views");
      if (views != payload.end()) {
        if (!views->is_array()) {
          last_error_ = record_prefix(event) + ": vertex buffer views must be an array";
          return false;
        }
        if (views->size() != payload_u64(payload, "view_count")) {
          last_error_ = record_prefix(event) + ": vertex buffer views count does not match view_count";
          return false;
        }
        const auto start_slot = payload_u32(payload, "start_slot");
        for (std::size_t index = 0; index < views->size(); ++index) {
          const auto &view = (*views)[index];
          if (!view.is_object()) {
            last_error_ = record_prefix(event) + ": vertex buffer view must be an object";
            return false;
          }
          if (payload_u64(view, "buffer_location") == 0) {
            continue;
          }
          VertexBufferBinding binding;
          binding.slot = start_slot + static_cast<std::uint32_t>(index);
          if (!resolve_gpu_virtual_address_at(payload_u64(view, "buffer_location"), event.callsite.sequence, binding.address, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          binding.size_in_bytes = payload_u32(view, "size_in_bytes");
          binding.stride_in_bytes = payload_u32(view, "stride_in_bytes");
          command_list_state->vertex_buffers.push_back(binding);
        }
      } else {
        const auto &first_view = payload.find("first") != payload.end() ? payload["first"] : json{};
        if (first_view.is_object() && payload_u64(first_view, "buffer_location") != 0) {
          VertexBufferBinding binding;
          binding.slot = payload_u32(payload, "start_slot");
          if (!resolve_gpu_virtual_address_at(payload_u64(first_view, "buffer_location"), event.callsite.sequence, binding.address, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          binding.size_in_bytes = payload_u32(first_view, "size_in_bytes");
          binding.stride_in_bytes = payload_u32(first_view, "stride_in_bytes");
          command_list_state->vertex_buffers.push_back(binding);
        }
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::IASetIndexBuffer") {
    if (!payload.empty() && !require_payload_key(event, payload, "buffer_location", last_error_)) {
      return false;
    }
    if (command_list_state && payload_u64(payload, "buffer_location") != 0) {
      IndexBufferBinding binding;
      if (!resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), event.callsite.sequence, binding.address, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      binding.size_in_bytes = payload_u32(payload, "size_in_bytes");
      binding.format = payload_u32(payload, "format");
      command_list_state->index_buffer = binding;
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ResourceBarrier") {
    if (!require_payload_key(event, payload, "barrier_count", last_error_) ||
        !require_payload_key(event, payload, "barriers", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->barrier_count += payload_u64(payload, "barrier_count");
    }
    const auto &barriers = payload["barriers"];
    if (!barriers.is_array()) {
      last_error_ = record_prefix(event) + ": barriers must be an array";
      return false;
    }
    const auto barrier_count = payload_u64(payload, "barrier_count");
    if (barriers.size() != barrier_count) {
      last_error_ = record_prefix(event) + ": barrier_count does not match barriers payload";
      return false;
    }
    for (const auto &barrier : barriers) {
      if (!barrier.is_object()) {
        last_error_ = record_prefix(event) + ": barrier entry must be an object";
        return false;
      }
      const auto barrier_type = payload_u32(barrier, "type");
      if (barrier_type != 0) {
        continue;
      }
      if (!require_payload_key(event, barrier, "resource_object_id", last_error_) ||
          !require_payload_key(event, barrier, "before", last_error_) ||
          !require_payload_key(event, barrier, "after", last_error_) ||
          !require_payload_key(event, barrier, "subresource", last_error_)) {
        return false;
      }
      const auto resource_object_id = json_object_id(barrier["resource_object_id"]);
      if (resource_object_id == 0) {
        continue;
      }
      auto resource_it = resources_.find(resource_object_id);
      if (resource_it == resources_.end()) {
        last_error_ = record_prefix(event) + ": ResourceBarrier references an unknown replay resource";
        return false;
      }

      constexpr std::uint32_t begin_only = 0x1;
      constexpr std::uint32_t end_only = 0x2;
      constexpr std::uint32_t all_subresources = 0xffffffffu;
      auto &resource = resource_it->second;
      const auto flags = payload_u32(barrier, "flags");
      const auto before = payload_u32(barrier, "before");
      const auto after = payload_u32(barrier, "after");
      const auto subresource = payload_u32(barrier, "subresource");
      if ((flags & end_only) == 0) {
        bool before_state_matches = resource.current_state == before;
        if (subresource == all_subresources) {
          for (const auto &[tracked_subresource, tracked_state] : resource.subresource_states) {
            (void)tracked_subresource;
            if (tracked_state != before) {
              before_state_matches = false;
              break;
            }
          }
        } else {
          const auto subresource_state = resource.subresource_states.find(subresource);
          before_state_matches = subresource_state != resource.subresource_states.end()
                                     ? subresource_state->second == before
                                     : resource.current_state == before;
        }
        if (!before_state_matches) {
          last_error_ = record_prefix(event) + ": ResourceBarrier before-state does not match tracked resource state";
          return false;
        }
      }

      ResourceTransition transition;
      transition.sequence = event.callsite.sequence;
      transition.command_list_object_id = command_list_id;
      transition.flags = flags;
      transition.before = before;
      transition.after = after;
      transition.subresource = subresource;
      resource.transitions.push_back(transition);
      resource.last_transition_sequence = event.callsite.sequence;
      if ((flags & begin_only) == 0) {
        if (subresource == all_subresources) {
          resource.current_state = after;
          resource.subresource_states.clear();
        } else {
          resource.subresource_states[subresource] = after;
        }
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
             function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced") {
    if (function_name == "ID3D12GraphicsCommandList::DrawInstanced") {
      if (!require_payload_key(event, payload, "vertex_count_per_instance", last_error_) ||
          !require_payload_key(event, payload, "instance_count", last_error_) ||
          !require_payload_key(event, payload, "start_vertex_location", last_error_) ||
          !require_payload_key(event, payload, "start_instance_location", last_error_)) {
        return false;
      }
    } else if (!require_payload_key(event, payload, "index_count_per_instance", last_error_) ||
               !require_payload_key(event, payload, "instance_count", last_error_) ||
               !require_payload_key(event, payload, "start_index_location", last_error_) ||
               !require_payload_key(event, payload, "base_vertex_location", last_error_) ||
               !require_payload_key(event, payload, "start_instance_location", last_error_)) {
      return false;
    }
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList::DrawInstanced") {
        DrawSemanticState draw;
        draw.sequence = event.callsite.sequence;
        draw.command_list_object_id = command_list_id;
        draw.vertex_count_per_instance = payload_u32(payload, "vertex_count_per_instance");
        draw.instance_count = payload_u32(payload, "instance_count");
        draw.start_vertex_location = payload_u32(payload, "start_vertex_location");
        draw.start_instance_location = payload_u32(payload, "start_instance_location");
        command_list_state->draws.push_back(draw);
      } else {
        DrawIndexedSemanticState draw;
        draw.sequence = event.callsite.sequence;
        draw.command_list_object_id = command_list_id;
        draw.index_count_per_instance = payload_u32(payload, "index_count_per_instance");
        draw.instance_count = payload_u32(payload, "instance_count");
        draw.start_index_location = payload_u32(payload, "start_index_location");
        draw.base_vertex_location = payload_i32(payload, "base_vertex_location");
        draw.start_instance_location = payload_u32(payload, "start_instance_location");
        command_list_state->indexed_draws.push_back(draw);
      }
      ++command_list_state->draw_count;
    }
    ++draw_calls_seen_;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::Dispatch" ||
             function_name == "ID3D12GraphicsCommandList4::DispatchRays" ||
             function_name == "ID3D12GraphicsCommandList6::DispatchMesh") {
    if (!require_payload_key(event, payload, function_name == "ID3D12GraphicsCommandList4::DispatchRays" ? "width" : "thread_group_count_x", last_error_) ||
        !require_payload_key(event, payload, function_name == "ID3D12GraphicsCommandList4::DispatchRays" ? "height" : "thread_group_count_y", last_error_) ||
        !require_payload_key(event, payload, function_name == "ID3D12GraphicsCommandList4::DispatchRays" ? "depth" : "thread_group_count_z", last_error_)) {
      return false;
    }
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList4::DispatchRays") {
        DispatchRaysSemanticState dispatch;
        dispatch.sequence = event.callsite.sequence;
        dispatch.command_list_object_id = command_list_id;
        dispatch.width = payload_u32(payload, "width");
        dispatch.height = payload_u32(payload, "height");
        dispatch.depth = payload_u32(payload, "depth");
        const auto parse_address_range = [&](const char *key, GpuVirtualAddressBinding &binding, std::uint64_t &size) -> bool {
          const auto range = payload.find(key);
          if (range == payload.end()) {
            return true;
          }
          if (!range->is_object()) {
            last_error_ = record_prefix(event) + ": DispatchRays shader table range must be an object";
            return false;
          }
          if (!resolve_gpu_virtual_address_at(payload_u64(*range, "start_address"), event.callsite.sequence, binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          size = payload_u64(*range, "size_in_bytes");
          return true;
        };
        const auto parse_address_range_and_stride = [&](
                                                         const char *key,
                                                         GpuVirtualAddressBinding &binding,
                                                         std::uint64_t &size,
                                                         std::uint64_t &stride) -> bool {
          const auto range = payload.find(key);
          if (range == payload.end()) {
            return true;
          }
          if (!range->is_object()) {
            last_error_ = record_prefix(event) + ": DispatchRays shader table range must be an object";
            return false;
          }
          if (!resolve_gpu_virtual_address_at(payload_u64(*range, "start_address"), event.callsite.sequence, binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          size = payload_u64(*range, "size_in_bytes");
          stride = payload_u64(*range, "stride_in_bytes");
          return true;
        };
        if (!parse_address_range(
                "ray_generation_shader_record",
                dispatch.ray_generation_shader_record,
                dispatch.ray_generation_shader_record_size) ||
            !parse_address_range_and_stride(
                "miss_shader_table",
                dispatch.miss_shader_table,
                dispatch.miss_shader_table_size,
                dispatch.miss_shader_table_stride) ||
            !parse_address_range_and_stride(
                "hit_group_table",
                dispatch.hit_group_table,
                dispatch.hit_group_table_size,
                dispatch.hit_group_table_stride) ||
            !parse_address_range_and_stride(
                "callable_shader_table",
                dispatch.callable_shader_table,
                dispatch.callable_shader_table_size,
                dispatch.callable_shader_table_stride)) {
          return false;
        }
        command_list_state->ray_dispatches.push_back(dispatch);
      } else {
        DispatchSemanticState dispatch;
        dispatch.sequence = event.callsite.sequence;
        dispatch.command_list_object_id = command_list_id;
        dispatch.thread_group_count_x = payload_u32(payload, "thread_group_count_x");
        dispatch.thread_group_count_y = payload_u32(payload, "thread_group_count_y");
        dispatch.thread_group_count_z = payload_u32(payload, "thread_group_count_z");
        command_list_state->dispatches.push_back(dispatch);
      }
      ++command_list_state->dispatch_count;
    }
    ++dispatch_calls_seen_;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ExecuteIndirect") {
    if (!require_payload_key(event, payload, "max_command_count", last_error_) ||
        !require_payload_key(event, payload, "arg_buffer_offset", last_error_) ||
        !require_payload_key(event, payload, "count_buffer_offset", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect missing command signature or argument buffer object id";
      return false;
    }
    const auto signature_it = command_signatures_.find(event.object_refs[1]);
    if (signature_it == command_signatures_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect references an unknown command signature";
      return false;
    }
    if (resources_.find(event.object_refs[2]) == resources_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect references an unknown argument buffer";
      return false;
    }
    const auto count_buffer_object_id = event.object_refs.size() >= 4 ? event.object_refs[3] : trace::ObjectId{};
    if (count_buffer_object_id != 0 && resources_.find(count_buffer_object_id) == resources_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect references an unknown count buffer";
      return false;
    }
    if (command_list_state) {
      ExecuteIndirectSemanticState execute;
      execute.sequence = event.callsite.sequence;
      execute.command_list_object_id = command_list_id;
      execute.command_signature_object_id = event.object_refs[1];
      execute.arg_buffer_object_id = event.object_refs[2];
      execute.count_buffer_object_id = count_buffer_object_id;
      execute.max_command_count = payload_u32(payload, "max_command_count");
      execute.arg_buffer_offset = payload_u64(payload, "arg_buffer_offset");
      execute.count_buffer_offset = payload_u64(payload, "count_buffer_offset");
      command_list_state->indirect_executes.push_back(execute);
      ++command_list_state->draw_count;
    }
    ++draw_calls_seen_;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" ||
             function_name == "ID3D12GraphicsCommandList::CopyResource" ||
             function_name == "ID3D12GraphicsCommandList::ResolveSubresource") {
    if (command_list_state) {
      ++command_list_state->copy_count;
      if (function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" &&
          payload_has(payload, "dst") && payload_has(payload, "src")) {
        TextureCopySemanticState copy;
        copy.sequence = event.callsite.sequence;
        copy.command_list_object_id = command_list_id;
        copy.dst_x = payload_u32(payload, "dst_x");
        copy.dst_y = payload_u32(payload, "dst_y");
        copy.dst_z = payload_u32(payload, "dst_z");
        copy.has_src_box = payload.value("has_src_box", false);
        if (!parse_texture_copy_location_payload(event, "dst", copy.dst, last_error_) ||
            !parse_texture_copy_location_payload(event, "src", copy.src, last_error_)) {
          return false;
        }
        if (copy.dst.resource_object_id != 0 && resources_.find(copy.dst.resource_object_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyTextureRegion references an unknown destination resource";
          return false;
        }
        if (copy.src.resource_object_id != 0 && resources_.find(copy.src.resource_object_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyTextureRegion references an unknown source resource";
          return false;
        }
        command_list_state->texture_copies.push_back(copy);
      } else if (function_name == "ID3D12GraphicsCommandList::CopyResource") {
        if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
          last_error_ = record_prefix(event) + ": CopyResource missing source or destination resource object id";
          return false;
        }
        if (resources_.find(event.object_refs[1]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyResource references an unknown destination resource";
          return false;
        }
        if (resources_.find(event.object_refs[2]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyResource references an unknown source resource";
          return false;
        }
        ResourceCopySemanticState copy;
        copy.sequence = event.callsite.sequence;
        copy.command_list_object_id = command_list_id;
        copy.dst_resource_object_id = event.object_refs[1];
        copy.src_resource_object_id = event.object_refs[2];
        command_list_state->resource_copies.push_back(copy);
      } else if (function_name == "ID3D12GraphicsCommandList::ResolveSubresource") {
        if (!require_payload_key(event, payload, "dst_subresource", last_error_) ||
            !require_payload_key(event, payload, "src_subresource", last_error_) ||
            !require_payload_key(event, payload, "format", last_error_)) {
          return false;
        }
        if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
          last_error_ = record_prefix(event) + ": ResolveSubresource missing source or destination resource object id";
          return false;
        }
        if (resources_.find(event.object_refs[1]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResolveSubresource references an unknown destination resource";
          return false;
        }
        if (resources_.find(event.object_refs[2]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResolveSubresource references an unknown source resource";
          return false;
        }
        ResolveSubresourceSemanticState resolve;
        resolve.sequence = event.callsite.sequence;
        resolve.command_list_object_id = command_list_id;
        resolve.dst_resource_object_id = event.object_refs[1];
        resolve.src_resource_object_id = event.object_refs[2];
        resolve.dst_subresource = payload_u32(payload, "dst_subresource");
        resolve.src_subresource = payload_u32(payload, "src_subresource");
        resolve.format = payload_u32(payload, "format");
        command_list_state->resolves.push_back(resolve);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Resource::Map") {
    if (!require_payload_key(event, payload, "subresource", last_error_)) {
      return false;
    }
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": Map missing resource object id";
      return false;
    }
    auto resource_it = resources_.find(event.object_refs.front());
    if (resource_it == resources_.end()) {
      last_error_ = record_prefix(event) + ": Map references an unknown replay resource";
      return false;
    }
    resource_it->second.mapped = payload.value("mapped", false);
    resource_it->second.mapped_subresource = payload_u32(payload, "subresource");
    resource_it->second.map_sequence = event.callsite.sequence;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Resource::Unmap") {
    if (!require_payload_key(event, payload, "subresource", last_error_)) {
      return false;
    }
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": Unmap missing resource object id";
      return false;
    }
    auto resource_it = resources_.find(event.object_refs.front());
    if (resource_it == resources_.end()) {
      last_error_ = record_prefix(event) + ": Unmap references an unknown replay resource";
      return false;
    }
    const auto subresource = payload_u32(payload, "subresource");
    if (resource_it->second.mapped && resource_it->second.mapped_subresource != subresource) {
      last_error_ = record_prefix(event) + ": Unmap subresource does not match prior Map";
      return false;
    }
    const auto buffer_path = payload_string(payload, "buffer_path");
    if (!buffer_path.empty() && !std::filesystem::is_regular_file(bundle_root_ / buffer_path)) {
      last_error_ = record_prefix(event) + ": missing mapped buffer asset " + buffer_path;
      return false;
    }
    const auto written_begin = payload_u64(payload, "written_begin");
    const auto written_end = payload_u64(payload, "written_end", written_begin);
    if (written_end < written_begin) {
      last_error_ = record_prefix(event) + ": Unmap written range is invalid";
      return false;
    }
    if (!buffer_path.empty()) {
      if (event.blob_refs.empty()) {
        last_error_ = record_prefix(event) + ": Unmap buffer asset is missing blob_refs";
        return false;
      }
      const auto absolute_buffer_path = bundle_root_ / buffer_path;
      std::vector<std::uint8_t> bytes;
      std::string asset_error;
      if (!read_exact_asset_bytes(absolute_buffer_path, written_end - written_begin, bytes, asset_error)) {
        last_error_ = record_prefix(event) + ": " + asset_error;
        return false;
      }
      ResourceDataUpdate update;
      update.sequence = event.callsite.sequence;
      update.subresource = subresource;
      update.written_begin = written_begin;
      update.written_end = written_end;
      update.relative_path = buffer_path;
      update.blob_refs = event.blob_refs;
      update.bytes = std::move(bytes);
      resource_it->second.data_updates.push_back(std::move(update));
    }
    resource_it->second.mapped = false;
    resource_it->second.map_sequence = 0;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12CommandQueue::ExecuteCommandLists") {
    if (!event.object_refs.empty()) {
      const auto queue_object_id = event.object_refs.front();
      auto queue_it = command_queues_.find(queue_object_id);
      if (queue_it == command_queues_.end()) {
        last_error_ = record_prefix(event) + ": ExecuteCommandLists references an unknown command queue";
        return false;
      }
      if (!require_payload_key(event, payload, "command_list_count", last_error_)) {
        return false;
      }
      const auto command_list_count = payload_u64(payload, "command_list_count");
      if (command_list_count != event.object_refs.size() - 1) {
        last_error_ = record_prefix(event) + ": ExecuteCommandLists object refs do not match command_list_count";
        return false;
      }
      submissions_.begin_batch(queue_object_id, 0, event.callsite.sequence);
      for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
        const auto submitted_list = command_lists_.find(event.object_refs[index]);
        if (submitted_list == command_lists_.end()) {
          last_error_ = record_prefix(event) + ": ExecuteCommandLists references an unknown command list";
          return false;
        }
        if (submitted_list->second.recording || submitted_list->second.close_sequence == 0) {
          last_error_ = record_prefix(event) + ": ExecuteCommandLists references an open command list";
          return false;
        }
        if (submitted_list->second.type != queue_it->second.type) {
          last_error_ = record_prefix(event) + ": ExecuteCommandLists command list type does not match queue type";
          return false;
        }
        if (index == 1) {
          submissions_.set_command_allocator(submitted_list->second.allocator_object_id);
        }
        submissions_.append_descriptor_heaps(submitted_list->second.descriptor_heap_object_ids);
        submissions_.append_command_list(event.object_refs[index]);
      }
      ++queue_it->second.execute_count;
      queue_it->second.last_execute_sequence = event.callsite.sequence;
      submissions_.end_batch();
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12CommandQueue::Signal" ||
             function_name == "ID3D12CommandQueue::Wait" ||
             function_name == "ID3D12Fence::Signal" ||
             function_name == "ID3D12Fence::SetEventOnCompletion" ||
             function_name == "ID3D12Fence::GetCompletedValue") {
    if (function_name == "ID3D12Fence::GetCompletedValue") {
      if (!require_payload_key(event, payload, "completed_value", last_error_)) {
        return false;
      }
      const auto fence_object_id = event.object_refs.empty() ? trace::ObjectId{} : event.object_refs.front();
      if (fence_object_id == 0) {
        last_error_ = record_prefix(event) + ": GetCompletedValue missing fence object id";
        return false;
      }
      auto fence_it = fences_.find(fence_object_id);
      if (fence_it == fences_.end()) {
        last_error_ = record_prefix(event) + ": GetCompletedValue references an unknown fence";
        return false;
      }
      const auto completed_value = payload_u64(payload, "completed_value");
      if (completed_value > fence_it->second.completed_value) {
        fence_it->second.completed_value = completed_value;
      }
      ++semantic_calls_seen_;
    } else {
      if (!require_payload_key(event, payload, "fence_value", last_error_)) {
        return false;
      }
      const bool queue_operation = function_name == "ID3D12CommandQueue::Signal" ||
                                   function_name == "ID3D12CommandQueue::Wait";
      const auto fence_object_id = queue_operation
                                       ? (event.object_refs.size() >= 2 ? event.object_refs[1] : trace::ObjectId{})
                                       : (event.object_refs.empty() ? trace::ObjectId{} : event.object_refs.front());
      if (fence_object_id == 0) {
        last_error_ = record_prefix(event) + ": fence operation missing fence object id";
        return false;
      }
      const auto queue_object_id = queue_operation && !event.object_refs.empty() ? event.object_refs.front() : trace::ObjectId{};
      if (queue_operation) {
        const auto queue_it = command_queues_.find(queue_object_id);
        if (queue_it == command_queues_.end()) {
          last_error_ = record_prefix(event) + ": fence operation references an unknown command queue";
          return false;
        }
      }
      auto fence_it = fences_.find(fence_object_id);
      if (fence_it == fences_.end()) {
        last_error_ = record_prefix(event) + ": fence operation references an unknown fence";
        return false;
      }
      FenceOperationSemanticState operation;
      operation.sequence = event.callsite.sequence;
      operation.queue_object_id = queue_object_id;
      operation.fence_object_id = fence_object_id;
      operation.fence_value = payload_u64(payload, "fence_value");
      operation.queue_operation = queue_operation;
      operation.wait_operation = function_name == "ID3D12CommandQueue::Wait";
      operation.event_operation = function_name == "ID3D12Fence::SetEventOnCompletion";
      fence_operations_.push_back(operation);
      if (function_name == "ID3D12CommandQueue::Signal" ||
          function_name == "ID3D12Fence::Signal") {
        fence_it->second.current_value = operation.fence_value;
        fence_it->second.last_signal_sequence = event.callsite.sequence;
        if (function_name == "ID3D12CommandQueue::Signal") {
          submissions_.signal_fence_for_queue(
              operation.queue_object_id,
              fence_object_id,
              operation.fence_value,
              event.callsite.sequence);
          const auto &batches = submissions_.completed_batches();
          if (!batches.empty() && batches.back().queue_object_id == operation.queue_object_id) {
            for (const auto command_list_object_id : batches.back().command_list_ids) {
              const auto command_list_it = command_lists_.find(command_list_object_id);
              if (command_list_it == command_lists_.end() || command_list_it->second.allocator_object_id == 0) {
                continue;
              }
              auto allocator_it = command_allocators_.find(command_list_it->second.allocator_object_id);
              if (allocator_it == command_allocators_.end()) {
                continue;
              }
              allocator_it->second.last_submit_sequence = batches.back().execute_sequence;
              allocator_it->second.last_submit_fence_object_id = fence_object_id;
              allocator_it->second.last_submit_fence_value = operation.fence_value;
            }
          }
        }
      } else if (function_name == "ID3D12CommandQueue::Wait") {
        fence_it->second.last_wait_sequence = event.callsite.sequence;
        submissions_.wait_on_fence_for_queue(
            operation.queue_object_id,
            fence_object_id,
            operation.fence_value,
            event.callsite.sequence);
      } else {
        fence_it->second.last_event_sequence = event.callsite.sequence;
        if (operation.fence_value > fence_it->second.completed_value) {
          fence_it->second.completed_value = operation.fence_value;
        }
      }
      ++semantic_calls_seen_;
    }
  } else if (function_name == "ID3D12CommandAllocator::Reset") {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": CommandAllocator::Reset missing allocator object id";
      return false;
    }
    auto allocator_it = command_allocators_.find(event.object_refs.front());
    if (allocator_it == command_allocators_.end()) {
      last_error_ = record_prefix(event) + ": CommandAllocator::Reset references an unknown allocator";
      return false;
    }
    if (allocator_it->second.last_submit_fence_object_id != 0) {
      const auto fence_it = fences_.find(allocator_it->second.last_submit_fence_object_id);
      if (fence_it == fences_.end()) {
        last_error_ = record_prefix(event) + ": CommandAllocator::Reset references an unknown submit fence";
        return false;
      }
      if (fence_it->second.completed_value < allocator_it->second.last_submit_fence_value) {
        last_error_ = record_prefix(event) + ": CommandAllocator::Reset before captured fence completion for prior submission";
        return false;
      }
    }
    for (const auto &[command_list_object_id, command_list] : command_lists_) {
      (void)command_list_object_id;
      if (command_list.recording && command_list.allocator_object_id == allocator_it->first) {
        last_error_ = record_prefix(event) + ": CommandAllocator::Reset while a referencing command list is recording";
        return false;
      }
    }
    ++allocator_it->second.reset_count;
    allocator_it->second.last_reset_sequence = event.callsite.sequence;
    ++semantic_calls_seen_;
  } else if (function_name == "D3D12CreateDevice" ||
             function_name == "ID3D12Device::QueryInterface" ||
             function_name == "ID3D12GraphicsCommandList::QueryInterface" ||
             function_name == "IDXGISwapChain::Present") {
    if (function_name == "IDXGISwapChain::Present" &&
        (!require_payload_key(event, payload, "sync_interval", last_error_) ||
         !require_payload_key(event, payload, "flags", last_error_))) {
      return false;
    }
    ++semantic_calls_seen_;
  }

  ++commands_replayed_;
  return true;
}

void D3D12ReplayBackend::shutdown()
{
  objects_.clear();
  submissions_.clear();
  present_frames_.clear();
  present_semantics_.clear();
  command_lists_.clear();
  command_queues_.clear();
  command_allocators_.clear();
  descriptor_heaps_.clear();
  descriptors_.clear();
  command_signatures_.clear();
  fences_.clear();
  devices_.clear();
  resources_.clear();
  pipelines_.clear();
  root_signatures_.clear();
  fence_operations_.clear();
  replay_commands_.clear();
  bundle_root_.clear();
  initialized_ = false;
  commands_replayed_ = 0;
  frames_seen_ = 0;
  presents_seen_ = 0;
  pipeline_assets_read_ = 0;
  semantic_calls_seen_ = 0;
  draw_calls_seen_ = 0;
  dispatch_calls_seen_ = 0;
  last_sequence_ = 0;
}

const std::string &D3D12ReplayBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::d3d12
