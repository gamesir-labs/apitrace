#include "d3d11_capture_internal.hpp"

#include "apitrace/capture_runtime.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace apitrace::d3d11::internal {

namespace {

constexpr std::size_t kDeviceVtblSize = 43;
constexpr std::size_t kContextVtblSize = 115;
constexpr std::size_t kSwapChainVtblSize = 18;

constexpr std::size_t kDeviceCreateBufferIndex = 3;
constexpr std::size_t kDeviceCreateRenderTargetViewIndex = 9;
constexpr std::size_t kDeviceCreateInputLayoutIndex = 11;
constexpr std::size_t kDeviceCreateVertexShaderIndex = 12;
constexpr std::size_t kDeviceCreatePixelShaderIndex = 15;
constexpr std::size_t kDeviceGetImmediateContextIndex = 40;

constexpr std::size_t kContextVSSetConstantBuffersIndex = 7;
constexpr std::size_t kContextPSSetShaderIndex = 9;
constexpr std::size_t kContextVSSetShaderIndex = 11;
constexpr std::size_t kContextDrawIndex = 13;
constexpr std::size_t kContextMapIndex = 14;
constexpr std::size_t kContextUnmapIndex = 15;
constexpr std::size_t kContextPSSetConstantBuffersIndex = 16;
constexpr std::size_t kContextIASetInputLayoutIndex = 17;
constexpr std::size_t kContextIASetVertexBuffersIndex = 18;
constexpr std::size_t kContextIASetPrimitiveTopologyIndex = 24;
constexpr std::size_t kContextOMSetRenderTargetsIndex = 33;
constexpr std::size_t kContextRSSetViewportsIndex = 44;
constexpr std::size_t kContextClearRenderTargetViewIndex = 50;

constexpr std::size_t kSwapChainPresentIndex = 8;
constexpr std::size_t kSwapChainGetBufferIndex = 9;

using CreateDeviceFn = HRESULT(WINAPI *)(
    IDXGIAdapter *,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL *,
    UINT,
    UINT,
    ID3D11Device **,
    D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);

using CreateDeviceAndSwapChainFn = HRESULT(WINAPI *)(
    IDXGIAdapter *,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL *,
    UINT,
    UINT,
    const DXGI_SWAP_CHAIN_DESC *,
    IDXGISwapChain **,
    ID3D11Device **,
    D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);

using CoreCreateDeviceFn = HRESULT(WINAPI *)(
    IDXGIFactory *,
    IDXGIAdapter *,
    UINT,
    const D3D_FEATURE_LEVEL *,
    UINT,
    ID3D11Device **);

using On12CreateDeviceFn = HRESULT(WINAPI *)(
    IUnknown *,
    UINT,
    const D3D_FEATURE_LEVEL *,
    UINT,
    IUnknown *const *,
    UINT,
    UINT,
    ID3D11Device **,
    ID3D11DeviceContext **,
    D3D_FEATURE_LEVEL *);

using DeviceCreateBufferFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_BUFFER_DESC *,
    const D3D11_SUBRESOURCE_DATA *,
    ID3D11Buffer **);
using DeviceCreateRenderTargetViewFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    ID3D11Resource *,
    const D3D11_RENDER_TARGET_VIEW_DESC *,
    ID3D11RenderTargetView **);
using DeviceCreateInputLayoutFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_INPUT_ELEMENT_DESC *,
    UINT,
    const void *,
    SIZE_T,
    ID3D11InputLayout **);
using DeviceCreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const void *,
    SIZE_T,
    ID3D11ClassLinkage *,
    ID3D11VertexShader **);
using DeviceCreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const void *,
    SIZE_T,
    ID3D11ClassLinkage *,
    ID3D11PixelShader **);
using DeviceGetImmediateContextFn = void(STDMETHODCALLTYPE *)(ID3D11Device *, ID3D11DeviceContext **);

using ContextVSSetConstantBuffersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using ContextPSSetShaderFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
using ContextVSSetShaderFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
using ContextDrawFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using ContextMapFn =
    HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
using ContextUnmapFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using ContextPSSetConstantBuffersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using ContextIASetInputLayoutFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11InputLayout *);
using ContextIASetVertexBuffersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *, const UINT *, const UINT *);
using ContextIASetPrimitiveTopologyFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, D3D11_PRIMITIVE_TOPOLOGY);
using ContextOMSetRenderTargetsFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
using ContextRSSetViewportsFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
using ContextClearRenderTargetViewFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);

using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using SwapChainGetBufferFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, REFIID, void **);

struct DownstreamModule {
  HMODULE module = nullptr;
  CoreCreateDeviceFn core_create_device = nullptr;
  CreateDeviceFn create_device = nullptr;
  CreateDeviceAndSwapChainFn create_device_and_swap_chain = nullptr;
  On12CreateDeviceFn on12_create_device = nullptr;
};

struct DeviceHookState {
  void **vtable = nullptr;
  DeviceCreateBufferFn create_buffer = nullptr;
  DeviceCreateRenderTargetViewFn create_render_target_view = nullptr;
  DeviceCreateInputLayoutFn create_input_layout = nullptr;
  DeviceCreateVertexShaderFn create_vertex_shader = nullptr;
  DeviceCreatePixelShaderFn create_pixel_shader = nullptr;
  DeviceGetImmediateContextFn get_immediate_context = nullptr;
};

struct ContextHookState {
  void **vtable = nullptr;
  ContextVSSetConstantBuffersFn vs_set_constant_buffers = nullptr;
  ContextPSSetShaderFn ps_set_shader = nullptr;
  ContextVSSetShaderFn vs_set_shader = nullptr;
  ContextDrawFn draw = nullptr;
  ContextMapFn map = nullptr;
  ContextUnmapFn unmap = nullptr;
  ContextPSSetConstantBuffersFn ps_set_constant_buffers = nullptr;
  ContextIASetInputLayoutFn ia_set_input_layout = nullptr;
  ContextIASetVertexBuffersFn ia_set_vertex_buffers = nullptr;
  ContextIASetPrimitiveTopologyFn ia_set_primitive_topology = nullptr;
  ContextOMSetRenderTargetsFn om_set_render_targets = nullptr;
  ContextRSSetViewportsFn rs_set_viewports = nullptr;
  ContextClearRenderTargetViewFn clear_render_target_view = nullptr;
};

struct SwapChainHookState {
  void **vtable = nullptr;
  SwapChainPresentFn present = nullptr;
  SwapChainGetBufferFn get_buffer = nullptr;
};

struct ObjectInfo {
  trace::ObjectId object_id = 0;
  trace::ObjectKind kind = trace::ObjectKind::Unknown;
  trace::ObjectId parent_object_id = 0;
  std::string debug_name;
};

struct BufferInfo {
  trace::ObjectId object_id = 0;
  UINT byte_width = 0;
  UINT bind_flags = 0;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
  UINT cpu_access_flags = 0;
  const void *mapped_ptr = nullptr;
  UINT mapped_subresource = 0;
  D3D11_MAP mapped_type = D3D11_MAP_READ;
};

struct CaptureState {
  std::recursive_mutex mutex;
  trace::ObjectId next_object_id = 1000;
  trace::BlobId next_blob_id = 5000;
  std::uint64_t next_sequence = 1;
  std::uint64_t frame_index = 0;
  bool frame_begin_pending = true;
  std::unordered_map<const void *, ObjectInfo> objects;
  std::unordered_map<const void *, BufferInfo> buffers;
  std::unordered_map<void **, DeviceHookState> device_hooks;
  std::unordered_map<void **, ContextHookState> context_hooks;
  std::unordered_map<void **, SwapChainHookState> swapchain_hooks;
  DownstreamModule downstream;
  std::once_flag downstream_once;
};

CaptureState &capture_state()
{
  static CaptureState state;
  return state;
}

void proxy_debug_log(const char *message)
{
  const char *path = std::getenv("APITRACE_D3D11_PROXY_LOG");
  if (!path || !*path || !message) {
    return;
  }

  if (std::FILE *stream = std::fopen(path, "ab")) {
    std::fputs(message, stream);
    std::fputc('\n', stream);
    std::fclose(stream);
  }
}

void proxy_debug_logf(const char *format, ...)
{
  const char *path = std::getenv("APITRACE_D3D11_PROXY_LOG");
  if (!path || !*path || !format) {
    return;
  }

  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  proxy_debug_log(buffer);
}

trace::AssetRecord register_asset_bytes(
    trace::AssetKind kind,
    std::string debug_name,
    const void *data,
    std::size_t size)
{
  trace::AssetRecord asset;
  auto &state = capture_state();
  asset.blob_id = ++state.next_blob_id;
  asset.kind = kind;
  asset.debug_name = std::move(debug_name);
  asset.payload_bytes.resize(size);
  if (size != 0) {
    std::memcpy(asset.payload_bytes.data(), data, size);
  }

  if (auto *session = runtime::ensure_process_trace_session(trace::ApiKind::D3D11)) {
    return session->register_asset(asset);
  }
  return asset;
}

std::string json_escape(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (const unsigned char ch : text) {
    switch (ch) {
    case '\"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (ch < 0x20) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(ch));
        escaped += buffer;
      } else {
        escaped.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  return escaped;
}

std::string bool_json(bool value)
{
  return value ? "true" : "false";
}

bool env_flag_enabled(const char *name, bool default_value = true)
{
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return default_value;
  }

  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0) {
    return false;
  }
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0) {
    return true;
  }
  return default_value;
}

std::string driver_type_name(D3D_DRIVER_TYPE driver_type)
{
  switch (driver_type) {
  case D3D_DRIVER_TYPE_HARDWARE:
    return "HARDWARE";
  case D3D_DRIVER_TYPE_WARP:
    return "WARP";
  case D3D_DRIVER_TYPE_REFERENCE:
    return "REFERENCE";
  case D3D_DRIVER_TYPE_NULL:
    return "NULL";
  case D3D_DRIVER_TYPE_SOFTWARE:
    return "SOFTWARE";
  case D3D_DRIVER_TYPE_UNKNOWN:
  default:
    return "UNKNOWN";
  }
}

std::string feature_level_name(D3D_FEATURE_LEVEL level)
{
  switch (level) {
  case D3D_FEATURE_LEVEL_11_1:
    return "11_1";
  case D3D_FEATURE_LEVEL_11_0:
    return "11_0";
  case D3D_FEATURE_LEVEL_10_1:
    return "10_1";
  case D3D_FEATURE_LEVEL_10_0:
    return "10_0";
  case D3D_FEATURE_LEVEL_9_3:
    return "9_3";
  case D3D_FEATURE_LEVEL_9_2:
    return "9_2";
  case D3D_FEATURE_LEVEL_9_1:
    return "9_1";
  default:
    return "unknown";
  }
}

std::string topology_name(D3D11_PRIMITIVE_TOPOLOGY topology)
{
  switch (topology) {
  case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    return "TRIANGLELIST";
  case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return "TRIANGLESTRIP";
  default:
    return "OTHER";
  }
}

std::string map_type_name(D3D11_MAP map_type)
{
  switch (map_type) {
  case D3D11_MAP_WRITE_DISCARD:
    return "WRITE_DISCARD";
  case D3D11_MAP_WRITE:
    return "WRITE";
  case D3D11_MAP_READ:
    return "READ";
  case D3D11_MAP_READ_WRITE:
    return "READ_WRITE";
  case D3D11_MAP_WRITE_NO_OVERWRITE:
    return "WRITE_NO_OVERWRITE";
  default:
    return "OTHER";
  }
}

template <typename Interface>
trace::ObjectId lookup_object_id_locked(Interface *object)
{
  const auto &state = capture_state();
  const auto it = state.objects.find(object);
  if (it == state.objects.end()) {
    return 0;
  }
  return it->second.object_id;
}

trace::ObjectId register_object_locked(
    const void *object,
    trace::ObjectKind kind,
    std::string debug_name,
    trace::ObjectId parent_object_id = 0)
{
  auto &state = capture_state();
  const auto existing = state.objects.find(object);
  if (existing != state.objects.end()) {
    return existing->second.object_id;
  }

  trace::ObjectRecord record;
  record.object_id = ++state.next_object_id;
  record.kind = kind;
  record.parent_object_id = parent_object_id;
  record.debug_name = std::move(debug_name);

  state.objects.emplace(
      object,
      ObjectInfo{
          record.object_id,
          record.kind,
          record.parent_object_id,
          record.debug_name,
      });

  if (auto *session = runtime::ensure_process_trace_session(trace::ApiKind::D3D11)) {
    session->record_object(record);
  }
  return record.object_id;
}

std::vector<trace::ObjectId> collect_object_ids_locked(const std::vector<const void *> &objects)
{
  std::vector<trace::ObjectId> object_ids;
  object_ids.reserve(objects.size());
  for (const void *object : objects) {
    if (!object) {
      continue;
    }
    const auto it = capture_state().objects.find(object);
    if (it != capture_state().objects.end()) {
      object_ids.push_back(it->second.object_id);
    }
  }
  return object_ids;
}

void record_boundary_locked(trace::BoundaryKind boundary, std::string payload_json)
{
  if (auto *session = runtime::ensure_process_trace_session(trace::ApiKind::D3D11)) {
    trace::EventRecord event;
    event.kind = trace::EventKind::Boundary;
    event.boundary = boundary;
    event.callsite.sequence = capture_state().next_sequence++;
    event.callsite.function_name = boundary == trace::BoundaryKind::Present ? "Present" : "Frame";
    event.payload = std::move(payload_json);
    session->append_call_event(event);
  }
}

void record_call_locked(
    std::string function_name,
    HRESULT result_code,
    const std::vector<const void *> &objects,
    const std::vector<trace::BlobId> &blob_refs,
    std::string payload_json)
{
  if (auto *session = runtime::ensure_process_trace_session(trace::ApiKind::D3D11)) {
    trace::EventRecord event;
    event.kind = trace::EventKind::Call;
    event.callsite.sequence = capture_state().next_sequence++;
    event.callsite.function_name = std::move(function_name);
    event.callsite.result_code = static_cast<std::int32_t>(result_code);
    event.object_refs = collect_object_ids_locked(objects);
    event.blob_refs = blob_refs;
    event.payload = std::move(payload_json);
    session->append_call_event(event);
  }
}

void ensure_frame_begin_locked()
{
  auto &state = capture_state();
  if (!state.frame_begin_pending) {
    return;
  }
  std::ostringstream payload;
  payload << "{\"label\":\"FrameBegin\",\"frame_index\":" << state.frame_index << "}";
  record_boundary_locked(trace::BoundaryKind::Frame, payload.str());
  state.frame_begin_pending = false;
}

std::string downstream_path()
{
  if (const char *explicit_path = std::getenv("APITRACE_DOWNSTREAM_D3D11")) {
    if (*explicit_path != '\0') {
      return explicit_path;
    }
  }

  char system_directory[MAX_PATH] = {};
  const auto length = GetSystemDirectoryA(system_directory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return "C:\\windows\\system32\\d3d11.dll";
  }

  std::string path(system_directory, length);
  path += "\\d3d11.dll";
  return path;
}

DownstreamModule &downstream_module()
{
  auto &state = capture_state();
  std::call_once(state.downstream_once, [&state]() {
    const auto path = downstream_path();
    proxy_debug_logf("downstream path=%s", path.c_str());
    state.downstream.module = LoadLibraryA(path.c_str());
    proxy_debug_logf("LoadLibraryA module=%p", state.downstream.module);
    if (!state.downstream.module) {
      return;
    }
    state.downstream.create_device = reinterpret_cast<CreateDeviceFn>(
        GetProcAddress(state.downstream.module, "D3D11CreateDevice"));
    state.downstream.core_create_device = reinterpret_cast<CoreCreateDeviceFn>(
        GetProcAddress(state.downstream.module, "D3D11CoreCreateDevice"));
    state.downstream.create_device_and_swap_chain = reinterpret_cast<CreateDeviceAndSwapChainFn>(
        GetProcAddress(state.downstream.module, "D3D11CreateDeviceAndSwapChain"));
    state.downstream.on12_create_device = reinterpret_cast<On12CreateDeviceFn>(
        GetProcAddress(state.downstream.module, "D3D11On12CreateDevice"));
    proxy_debug_logf(
        "exports core_create_device=%p create_device=%p create_device_and_swap_chain=%p on12_create_device=%p",
        reinterpret_cast<void *>(state.downstream.core_create_device),
        reinterpret_cast<void *>(state.downstream.create_device),
        reinterpret_cast<void *>(state.downstream.create_device_and_swap_chain),
        reinterpret_cast<void *>(state.downstream.on12_create_device));
  });
  return state.downstream;
}

bool patch_vtable_entry(void **vtable, std::size_t index, void *replacement)
{
  DWORD old_protect = 0;
  if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) {
    return false;
  }

  vtable[index] = replacement;

  DWORD ignored = 0;
  VirtualProtect(&vtable[index], sizeof(void *), old_protect, &ignored);
  return true;
}

DeviceHookState &device_hook_locked(ID3D11Device *device)
{
  auto &state = capture_state();
  return state.device_hooks.at(*reinterpret_cast<void ***>(device));
}

ContextHookState &context_hook_locked(ID3D11DeviceContext *context)
{
  auto &state = capture_state();
  return state.context_hooks.at(*reinterpret_cast<void ***>(context));
}

SwapChainHookState &swapchain_hook_locked(IDXGISwapChain *swapchain)
{
  auto &state = capture_state();
  return state.swapchain_hooks.at(*reinterpret_cast<void ***>(swapchain));
}

void patch_context(ID3D11DeviceContext *context);
void patch_swapchain(IDXGISwapChain *swapchain);

HRESULT STDMETHODCALLTYPE hook_create_buffer(
    ID3D11Device *device,
    const D3D11_BUFFER_DESC *desc,
    const D3D11_SUBRESOURCE_DATA *initial_data,
    ID3D11Buffer **buffer)
{
  proxy_debug_log("hook_create_buffer");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_buffer(device, desc, initial_data, buffer);

  std::vector<trace::BlobId> blob_refs;
  std::string initial_path_json = "null";
  if (SUCCEEDED(hr) && buffer && *buffer) {
    const auto object_id = register_object_locked(*buffer, trace::ObjectKind::Resource, "ID3D11Buffer", lookup_object_id_locked(device));
    BufferInfo buffer_info;
    buffer_info.object_id = object_id;
    if (desc) {
      buffer_info.byte_width = desc->ByteWidth;
      buffer_info.bind_flags = desc->BindFlags;
      buffer_info.usage = desc->Usage;
      buffer_info.cpu_access_flags = desc->CPUAccessFlags;
    }
    state.buffers[*buffer] = buffer_info;

    if (desc && initial_data && initial_data->pSysMem && desc->ByteWidth != 0) {
      auto asset = register_asset_bytes(trace::AssetKind::Buffer, "buffer-initial", initial_data->pSysMem, desc->ByteWidth);
      blob_refs.push_back(asset.blob_id);
      initial_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
    }
  }

  std::ostringstream payload;
  payload << "{"
          << "\"byte_width\":" << (desc ? desc->ByteWidth : 0) << ","
          << "\"usage\":" << static_cast<unsigned int>(desc ? desc->Usage : 0) << ","
          << "\"bind_flags\":" << (desc ? desc->BindFlags : 0) << ","
          << "\"cpu_access_flags\":" << (desc ? desc->CPUAccessFlags : 0) << ","
          << "\"has_initial_data\":" << bool_json(initial_data && initial_data->pSysMem != nullptr) << ","
          << "\"initial_data_path\":" << initial_path_json
          << "}";
  record_call_locked("ID3D11Device::CreateBuffer", hr, {device, buffer ? *buffer : nullptr}, blob_refs, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_render_target_view(
    ID3D11Device *device,
    ID3D11Resource *resource,
    const D3D11_RENDER_TARGET_VIEW_DESC *desc,
    ID3D11RenderTargetView **render_target_view)
{
  proxy_debug_log("hook_create_render_target_view");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_render_target_view(device, resource, desc, render_target_view);
  if (SUCCEEDED(hr) && render_target_view && *render_target_view) {
    register_object_locked(
        *render_target_view,
        trace::ObjectKind::View,
        "ID3D11RenderTargetView",
        lookup_object_id_locked(resource));
  }

  std::ostringstream payload;
  payload << "{\"desc_present\":" << bool_json(desc != nullptr) << "}";
  record_call_locked(
      "ID3D11Device::CreateRenderTargetView",
      hr,
      {device, resource, render_target_view ? *render_target_view : nullptr},
      {},
      payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_input_layout(
    ID3D11Device *device,
    const D3D11_INPUT_ELEMENT_DESC *input_element_descs,
    UINT num_elements,
    const void *shader_bytecode,
    SIZE_T bytecode_length,
    ID3D11InputLayout **input_layout)
{
  proxy_debug_log("hook_create_input_layout");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_input_layout(
      device,
      input_element_descs,
      num_elements,
      shader_bytecode,
      bytecode_length,
      input_layout);
  if (SUCCEEDED(hr) && input_layout && *input_layout) {
    register_object_locked(
        *input_layout,
        trace::ObjectKind::PipelineState,
        "ID3D11InputLayout",
        lookup_object_id_locked(device));
  }

  std::vector<trace::BlobId> blob_refs;
  std::string shader_path_json = "null";
  if (shader_bytecode && bytecode_length != 0) {
    auto asset = register_asset_bytes(trace::AssetKind::ShaderDxbc, "input-layout-shader", shader_bytecode, bytecode_length);
    blob_refs.push_back(asset.blob_id);
    shader_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
  }

  std::ostringstream payload;
  payload << "{\"num_elements\":" << num_elements
          << ",\"bytecode_length\":" << static_cast<std::uint64_t>(bytecode_length)
          << ",\"shader_path\":" << shader_path_json
          << ",\"elements\":[";
  for (UINT index = 0; index < num_elements; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &element = input_element_descs[index];
    payload << "{"
            << "\"semantic_name\":\"" << json_escape(element.SemanticName ? element.SemanticName : "") << "\","
            << "\"semantic_index\":" << element.SemanticIndex << ","
            << "\"format\":" << static_cast<unsigned int>(element.Format) << ","
            << "\"input_slot\":" << element.InputSlot << ","
            << "\"aligned_byte_offset\":" << element.AlignedByteOffset << ","
            << "\"input_slot_class\":" << static_cast<unsigned int>(element.InputSlotClass) << ","
            << "\"instance_data_step_rate\":" << element.InstanceDataStepRate
            << "}";
  }
  payload << "]}";
  record_call_locked(
      "ID3D11Device::CreateInputLayout",
      hr,
      {device, input_layout ? *input_layout : nullptr},
      blob_refs,
      payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_vertex_shader(
    ID3D11Device *device,
    const void *shader_bytecode,
    SIZE_T bytecode_length,
    ID3D11ClassLinkage *class_linkage,
    ID3D11VertexShader **vertex_shader)
{
  proxy_debug_log("hook_create_vertex_shader");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_vertex_shader(device, shader_bytecode, bytecode_length, class_linkage, vertex_shader);

  std::vector<trace::BlobId> blob_refs;
  std::string shader_path_json = "null";
  if (SUCCEEDED(hr) && vertex_shader && *vertex_shader && shader_bytecode && bytecode_length != 0) {
    const auto asset = register_asset_bytes(trace::AssetKind::ShaderDxbc, "vertex-shader", shader_bytecode, bytecode_length);
    blob_refs.push_back(asset.blob_id);
    shader_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
    register_object_locked(*vertex_shader, trace::ObjectKind::Shader, "ID3D11VertexShader", lookup_object_id_locked(device));
  }

  std::ostringstream payload;
  payload << "{\"bytecode_length\":" << static_cast<std::uint64_t>(bytecode_length)
          << ",\"shader_path\":" << shader_path_json << "}";
  record_call_locked("ID3D11Device::CreateVertexShader", hr, {device, vertex_shader ? *vertex_shader : nullptr}, blob_refs, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_pixel_shader(
    ID3D11Device *device,
    const void *shader_bytecode,
    SIZE_T bytecode_length,
    ID3D11ClassLinkage *class_linkage,
    ID3D11PixelShader **pixel_shader)
{
  proxy_debug_log("hook_create_pixel_shader");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_pixel_shader(device, shader_bytecode, bytecode_length, class_linkage, pixel_shader);

  std::vector<trace::BlobId> blob_refs;
  std::string shader_path_json = "null";
  if (SUCCEEDED(hr) && pixel_shader && *pixel_shader && shader_bytecode && bytecode_length != 0) {
    const auto asset = register_asset_bytes(trace::AssetKind::ShaderDxbc, "pixel-shader", shader_bytecode, bytecode_length);
    blob_refs.push_back(asset.blob_id);
    shader_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
    register_object_locked(*pixel_shader, trace::ObjectKind::Shader, "ID3D11PixelShader", lookup_object_id_locked(device));
  }

  std::ostringstream payload;
  payload << "{\"bytecode_length\":" << static_cast<std::uint64_t>(bytecode_length)
          << ",\"shader_path\":" << shader_path_json << "}";
  record_call_locked("ID3D11Device::CreatePixelShader", hr, {device, pixel_shader ? *pixel_shader : nullptr}, blob_refs, payload.str());
  return hr;
}

void STDMETHODCALLTYPE hook_get_immediate_context(ID3D11Device *device, ID3D11DeviceContext **immediate_context)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  hook.get_immediate_context(device, immediate_context);
  if (immediate_context && *immediate_context) {
    patch_context(*immediate_context);
    register_object_locked(*immediate_context, trace::ObjectKind::Context, "ID3D11DeviceContext", lookup_object_id_locked(device));
  }

  record_call_locked(
      "ID3D11Device::GetImmediateContext",
      S_OK,
      {device, immediate_context ? *immediate_context : nullptr},
      {},
      "{}");
}

void STDMETHODCALLTYPE hook_vs_set_constant_buffers(
    ID3D11DeviceContext *context,
    UINT start_slot,
    UINT num_buffers,
    ID3D11Buffer *const *constant_buffers)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.vs_set_constant_buffers(context, start_slot, num_buffers, constant_buffers);

  std::vector<const void *> objects = {context};
  for (UINT index = 0; index < num_buffers; ++index) {
    objects.push_back(constant_buffers ? constant_buffers[index] : nullptr);
  }

  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot << ",\"num_buffers\":" << num_buffers << "}";
  record_call_locked("ID3D11DeviceContext::VSSetConstantBuffers", S_OK, objects, {}, payload.str());
}

void STDMETHODCALLTYPE hook_ps_set_shader(
    ID3D11DeviceContext *context,
    ID3D11PixelShader *pixel_shader,
    ID3D11ClassInstance *const *class_instances,
    UINT num_class_instances)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ps_set_shader(context, pixel_shader, class_instances, num_class_instances);

  std::ostringstream payload;
  payload << "{\"class_instance_count\":" << num_class_instances << "}";
  record_call_locked("ID3D11DeviceContext::PSSetShader", S_OK, {context, pixel_shader}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_vs_set_shader(
    ID3D11DeviceContext *context,
    ID3D11VertexShader *vertex_shader,
    ID3D11ClassInstance *const *class_instances,
    UINT num_class_instances)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.vs_set_shader(context, vertex_shader, class_instances, num_class_instances);

  std::ostringstream payload;
  payload << "{\"class_instance_count\":" << num_class_instances << "}";
  record_call_locked("ID3D11DeviceContext::VSSetShader", S_OK, {context, vertex_shader}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext *context, UINT vertex_count, UINT start_vertex_location)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  ensure_frame_begin_locked();
  auto &hook = context_hook_locked(context);
  hook.draw(context, vertex_count, start_vertex_location);

  std::ostringstream payload;
  payload << "{\"vertex_count\":" << vertex_count << ",\"start_vertex_location\":" << start_vertex_location << "}";
  record_call_locked("ID3D11DeviceContext::Draw", S_OK, {context}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_map(
    ID3D11DeviceContext *context,
    ID3D11Resource *resource,
    UINT subresource,
    D3D11_MAP map_type,
    UINT map_flags,
    D3D11_MAPPED_SUBRESOURCE *mapped_resource)
{
  proxy_debug_log("hook_map");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  const HRESULT hr = hook.map(context, resource, subresource, map_type, map_flags, mapped_resource);

  if (SUCCEEDED(hr) && mapped_resource) {
    const auto buffer_it = state.buffers.find(resource);
    if (buffer_it != state.buffers.end()) {
      buffer_it->second.mapped_ptr = mapped_resource->pData;
      buffer_it->second.mapped_subresource = subresource;
      buffer_it->second.mapped_type = map_type;
    }
  }

  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource << ",\"map_type\":\"" << map_type_name(map_type)
          << "\",\"map_flags\":" << map_flags << "}";
  record_call_locked("ID3D11DeviceContext::Map", hr, {context, resource}, {}, payload.str());
  return hr;
}

void STDMETHODCALLTYPE hook_unmap(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource)
{
  proxy_debug_log("hook_unmap");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);

  std::vector<trace::BlobId> blob_refs;
  std::string snapshot_path_json = "null";
  const auto buffer_it = state.buffers.find(resource);
  if (buffer_it != state.buffers.end() && buffer_it->second.mapped_ptr && buffer_it->second.byte_width != 0) {
    const auto asset = register_asset_bytes(
        trace::AssetKind::Buffer,
        "buffer-map-snapshot",
        buffer_it->second.mapped_ptr,
        buffer_it->second.byte_width);
    blob_refs.push_back(asset.blob_id);
    snapshot_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
    buffer_it->second.mapped_ptr = nullptr;
  }

  auto &hook = context_hook_locked(context);
  hook.unmap(context, resource, subresource);

  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource << ",\"snapshot_path\":" << snapshot_path_json << "}";
  record_call_locked("ID3D11DeviceContext::Unmap", S_OK, {context, resource}, blob_refs, payload.str());
}

void STDMETHODCALLTYPE hook_ps_set_constant_buffers(
    ID3D11DeviceContext *context,
    UINT start_slot,
    UINT num_buffers,
    ID3D11Buffer *const *constant_buffers)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ps_set_constant_buffers(context, start_slot, num_buffers, constant_buffers);

  std::vector<const void *> objects = {context};
  for (UINT index = 0; index < num_buffers; ++index) {
    objects.push_back(constant_buffers ? constant_buffers[index] : nullptr);
  }

  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot << ",\"num_buffers\":" << num_buffers << "}";
  record_call_locked("ID3D11DeviceContext::PSSetConstantBuffers", S_OK, objects, {}, payload.str());
}

void STDMETHODCALLTYPE hook_ia_set_input_layout(ID3D11DeviceContext *context, ID3D11InputLayout *input_layout)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ia_set_input_layout(context, input_layout);
  record_call_locked("ID3D11DeviceContext::IASetInputLayout", S_OK, {context, input_layout}, {}, "{}");
}

void STDMETHODCALLTYPE hook_ia_set_vertex_buffers(
    ID3D11DeviceContext *context,
    UINT start_slot,
    UINT num_buffers,
    ID3D11Buffer *const *vertex_buffers,
    const UINT *strides,
    const UINT *offsets)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ia_set_vertex_buffers(context, start_slot, num_buffers, vertex_buffers, strides, offsets);

  std::vector<const void *> objects = {context};
  for (UINT index = 0; index < num_buffers; ++index) {
    objects.push_back(vertex_buffers ? vertex_buffers[index] : nullptr);
  }

  const UINT stride = (strides && num_buffers > 0) ? strides[0] : 0;
  const UINT offset = (offsets && num_buffers > 0) ? offsets[0] : 0;
  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot << ",\"num_buffers\":" << num_buffers
          << ",\"first_stride\":" << stride << ",\"first_offset\":" << offset
          << ",\"bindings\":[";
  for (UINT index = 0; index < num_buffers; ++index) {
    if (index != 0) {
      payload << ",";
    }
    payload << "{"
            << "\"object_id\":" << lookup_object_id_locked(vertex_buffers ? vertex_buffers[index] : nullptr) << ","
            << "\"stride\":" << (strides ? strides[index] : 0) << ","
            << "\"offset\":" << (offsets ? offsets[index] : 0)
            << "}";
  }
  payload << "]}";
  record_call_locked("ID3D11DeviceContext::IASetVertexBuffers", S_OK, objects, {}, payload.str());
}

void STDMETHODCALLTYPE hook_ia_set_primitive_topology(ID3D11DeviceContext *context, D3D11_PRIMITIVE_TOPOLOGY topology)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ia_set_primitive_topology(context, topology);

  std::ostringstream payload;
  payload << "{\"topology\":\"" << topology_name(topology) << "\"}";
  record_call_locked("ID3D11DeviceContext::IASetPrimitiveTopology", S_OK, {context}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_om_set_render_targets(
    ID3D11DeviceContext *context,
    UINT num_views,
    ID3D11RenderTargetView *const *render_target_views,
    ID3D11DepthStencilView *depth_stencil_view)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.om_set_render_targets(context, num_views, render_target_views, depth_stencil_view);

  std::vector<const void *> objects = {context};
  for (UINT index = 0; index < num_views; ++index) {
    objects.push_back(render_target_views ? render_target_views[index] : nullptr);
  }
  if (depth_stencil_view) {
    objects.push_back(depth_stencil_view);
  }

  std::ostringstream payload;
  payload << "{\"num_views\":" << num_views << ",\"has_depth_stencil\":" << bool_json(depth_stencil_view != nullptr) << "}";
  record_call_locked("ID3D11DeviceContext::OMSetRenderTargets", S_OK, objects, {}, payload.str());
}

void STDMETHODCALLTYPE hook_rs_set_viewports(ID3D11DeviceContext *context, UINT num_viewports, const D3D11_VIEWPORT *viewports)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.rs_set_viewports(context, num_viewports, viewports);

  const float width = (viewports && num_viewports > 0) ? viewports[0].Width : 0.0f;
  const float height = (viewports && num_viewports > 0) ? viewports[0].Height : 0.0f;
  std::ostringstream payload;
  payload << "{\"num_viewports\":" << num_viewports << ",\"first_width\":" << width
          << ",\"first_height\":" << height << ",\"viewports\":[";
  for (UINT index = 0; index < num_viewports; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &viewport = viewports[index];
    payload << "{"
            << "\"top_left_x\":" << viewport.TopLeftX << ","
            << "\"top_left_y\":" << viewport.TopLeftY << ","
            << "\"width\":" << viewport.Width << ","
            << "\"height\":" << viewport.Height << ","
            << "\"min_depth\":" << viewport.MinDepth << ","
            << "\"max_depth\":" << viewport.MaxDepth
            << "}";
  }
  payload << "]}";
  record_call_locked("ID3D11DeviceContext::RSSetViewports", S_OK, {context}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_clear_render_target_view(
    ID3D11DeviceContext *context,
    ID3D11RenderTargetView *render_target_view,
    const FLOAT color_rgba[4])
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.clear_render_target_view(context, render_target_view, color_rgba);

  std::ostringstream payload;
  payload << "{\"color\":[" << (color_rgba ? color_rgba[0] : 0.0f) << ","
          << (color_rgba ? color_rgba[1] : 0.0f) << ","
          << (color_rgba ? color_rgba[2] : 0.0f) << ","
          << (color_rgba ? color_rgba[3] : 0.0f) << "]}";
  record_call_locked("ID3D11DeviceContext::ClearRenderTargetView", S_OK, {context, render_target_view}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *swapchain, UINT sync_interval, UINT flags)
{
  proxy_debug_log("hook_present");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = swapchain_hook_locked(swapchain);
  const HRESULT hr = hook.present(swapchain, sync_interval, flags);

  ensure_frame_begin_locked();

  std::ostringstream call_payload;
  call_payload << "{\"sync_interval\":" << sync_interval << ",\"flags\":" << flags << "}";
  record_call_locked("IDXGISwapChain::Present", hr, {swapchain}, {}, call_payload.str());

  std::ostringstream present_payload;
  present_payload << "{\"label\":\"Present\",\"frame_index\":" << state.frame_index << "}";
  record_boundary_locked(trace::BoundaryKind::Present, present_payload.str());

  std::ostringstream frame_end_payload;
  frame_end_payload << "{\"label\":\"FrameEnd\",\"frame_index\":" << state.frame_index << "}";
  record_boundary_locked(trace::BoundaryKind::Frame, frame_end_payload.str());
  ++state.frame_index;
  state.frame_begin_pending = true;
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_get_buffer(IDXGISwapChain *swapchain, UINT buffer_index, REFIID riid, void **surface)
{
  proxy_debug_log("hook_get_buffer");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = swapchain_hook_locked(swapchain);
  const HRESULT hr = hook.get_buffer(swapchain, buffer_index, riid, surface);
  if (SUCCEEDED(hr) && surface && *surface) {
    register_object_locked(*surface, trace::ObjectKind::Resource, "IDXGISwapChain::Buffer", lookup_object_id_locked(swapchain));
  }

  std::ostringstream payload;
  payload << "{\"buffer_index\":" << buffer_index << "}";
  record_call_locked("IDXGISwapChain::GetBuffer", hr, {swapchain, surface ? *surface : nullptr}, {}, payload.str());
  return hr;
}

void patch_device(ID3D11Device *device)
{
  auto &state = capture_state();
  if (!device) {
    return;
  }
  if (!env_flag_enabled("APITRACE_D3D11_PATCH_DEVICE")) {
    proxy_debug_log("patch_device skipped by env");
    return;
  }

  auto **vtable = *reinterpret_cast<void ***>(device);
  if (state.device_hooks.find(vtable) != state.device_hooks.end()) {
    return;
  }
  proxy_debug_logf("patch_device object=%p vtable=%p", device, vtable);
  DeviceHookState hook;
  hook.vtable = vtable;
  hook.create_buffer = reinterpret_cast<DeviceCreateBufferFn>(vtable[kDeviceCreateBufferIndex]);
  hook.create_render_target_view =
      reinterpret_cast<DeviceCreateRenderTargetViewFn>(vtable[kDeviceCreateRenderTargetViewIndex]);
  hook.create_input_layout = reinterpret_cast<DeviceCreateInputLayoutFn>(vtable[kDeviceCreateInputLayoutIndex]);
  hook.create_vertex_shader = reinterpret_cast<DeviceCreateVertexShaderFn>(vtable[kDeviceCreateVertexShaderIndex]);
  hook.create_pixel_shader = reinterpret_cast<DeviceCreatePixelShaderFn>(vtable[kDeviceCreatePixelShaderIndex]);
  hook.get_immediate_context = reinterpret_cast<DeviceGetImmediateContextFn>(vtable[kDeviceGetImmediateContextIndex]);

  state.device_hooks.emplace(vtable, hook);
  patch_vtable_entry(vtable, kDeviceCreateBufferIndex, reinterpret_cast<void *>(hook_create_buffer));
  patch_vtable_entry(vtable, kDeviceCreateRenderTargetViewIndex, reinterpret_cast<void *>(hook_create_render_target_view));
  patch_vtable_entry(vtable, kDeviceCreateInputLayoutIndex, reinterpret_cast<void *>(hook_create_input_layout));
  patch_vtable_entry(vtable, kDeviceCreateVertexShaderIndex, reinterpret_cast<void *>(hook_create_vertex_shader));
  patch_vtable_entry(vtable, kDeviceCreatePixelShaderIndex, reinterpret_cast<void *>(hook_create_pixel_shader));
  patch_vtable_entry(vtable, kDeviceGetImmediateContextIndex, reinterpret_cast<void *>(hook_get_immediate_context));
  proxy_debug_logf("patch_device installed object=%p vtable=%p", device, vtable);
}

void patch_context(ID3D11DeviceContext *context)
{
  auto &state = capture_state();
  if (!context) {
    return;
  }
  if (!env_flag_enabled("APITRACE_D3D11_PATCH_CONTEXT")) {
    proxy_debug_log("patch_context skipped by env");
    return;
  }

  auto **vtable = *reinterpret_cast<void ***>(context);
  if (state.context_hooks.find(vtable) != state.context_hooks.end()) {
    return;
  }
  proxy_debug_logf("patch_context object=%p vtable=%p", context, vtable);
  ContextHookState hook;
  hook.vtable = vtable;
  hook.vs_set_constant_buffers =
      reinterpret_cast<ContextVSSetConstantBuffersFn>(vtable[kContextVSSetConstantBuffersIndex]);
  hook.ps_set_shader = reinterpret_cast<ContextPSSetShaderFn>(vtable[kContextPSSetShaderIndex]);
  hook.vs_set_shader = reinterpret_cast<ContextVSSetShaderFn>(vtable[kContextVSSetShaderIndex]);
  hook.draw = reinterpret_cast<ContextDrawFn>(vtable[kContextDrawIndex]);
  hook.map = reinterpret_cast<ContextMapFn>(vtable[kContextMapIndex]);
  hook.unmap = reinterpret_cast<ContextUnmapFn>(vtable[kContextUnmapIndex]);
  hook.ps_set_constant_buffers =
      reinterpret_cast<ContextPSSetConstantBuffersFn>(vtable[kContextPSSetConstantBuffersIndex]);
  hook.ia_set_input_layout = reinterpret_cast<ContextIASetInputLayoutFn>(vtable[kContextIASetInputLayoutIndex]);
  hook.ia_set_vertex_buffers =
      reinterpret_cast<ContextIASetVertexBuffersFn>(vtable[kContextIASetVertexBuffersIndex]);
  hook.ia_set_primitive_topology =
      reinterpret_cast<ContextIASetPrimitiveTopologyFn>(vtable[kContextIASetPrimitiveTopologyIndex]);
  hook.om_set_render_targets =
      reinterpret_cast<ContextOMSetRenderTargetsFn>(vtable[kContextOMSetRenderTargetsIndex]);
  hook.rs_set_viewports = reinterpret_cast<ContextRSSetViewportsFn>(vtable[kContextRSSetViewportsIndex]);
  hook.clear_render_target_view =
      reinterpret_cast<ContextClearRenderTargetViewFn>(vtable[kContextClearRenderTargetViewIndex]);

  state.context_hooks.emplace(vtable, hook);
  patch_vtable_entry(vtable, kContextVSSetConstantBuffersIndex, reinterpret_cast<void *>(hook_vs_set_constant_buffers));
  patch_vtable_entry(vtable, kContextPSSetShaderIndex, reinterpret_cast<void *>(hook_ps_set_shader));
  patch_vtable_entry(vtable, kContextVSSetShaderIndex, reinterpret_cast<void *>(hook_vs_set_shader));
  patch_vtable_entry(vtable, kContextDrawIndex, reinterpret_cast<void *>(hook_draw));
  patch_vtable_entry(vtable, kContextMapIndex, reinterpret_cast<void *>(hook_map));
  patch_vtable_entry(vtable, kContextUnmapIndex, reinterpret_cast<void *>(hook_unmap));
  patch_vtable_entry(vtable, kContextPSSetConstantBuffersIndex, reinterpret_cast<void *>(hook_ps_set_constant_buffers));
  patch_vtable_entry(vtable, kContextIASetInputLayoutIndex, reinterpret_cast<void *>(hook_ia_set_input_layout));
  patch_vtable_entry(vtable, kContextIASetVertexBuffersIndex, reinterpret_cast<void *>(hook_ia_set_vertex_buffers));
  patch_vtable_entry(vtable, kContextIASetPrimitiveTopologyIndex, reinterpret_cast<void *>(hook_ia_set_primitive_topology));
  patch_vtable_entry(vtable, kContextOMSetRenderTargetsIndex, reinterpret_cast<void *>(hook_om_set_render_targets));
  patch_vtable_entry(vtable, kContextRSSetViewportsIndex, reinterpret_cast<void *>(hook_rs_set_viewports));
  patch_vtable_entry(vtable, kContextClearRenderTargetViewIndex, reinterpret_cast<void *>(hook_clear_render_target_view));
  proxy_debug_logf("patch_context installed object=%p vtable=%p", context, vtable);
}

void patch_swapchain(IDXGISwapChain *swapchain)
{
  auto &state = capture_state();
  if (!swapchain) {
    return;
  }
  if (!env_flag_enabled("APITRACE_D3D11_PATCH_SWAPCHAIN")) {
    proxy_debug_log("patch_swapchain skipped by env");
    return;
  }

  auto **vtable = *reinterpret_cast<void ***>(swapchain);
  if (state.swapchain_hooks.find(vtable) != state.swapchain_hooks.end()) {
    return;
  }
  proxy_debug_logf("patch_swapchain object=%p vtable=%p", swapchain, vtable);
  SwapChainHookState hook;
  hook.vtable = vtable;
  hook.present = reinterpret_cast<SwapChainPresentFn>(vtable[kSwapChainPresentIndex]);
  hook.get_buffer = reinterpret_cast<SwapChainGetBufferFn>(vtable[kSwapChainGetBufferIndex]);

  state.swapchain_hooks.emplace(vtable, hook);
  patch_vtable_entry(vtable, kSwapChainPresentIndex, reinterpret_cast<void *>(hook_present));
  patch_vtable_entry(vtable, kSwapChainGetBufferIndex, reinterpret_cast<void *>(hook_get_buffer));
  proxy_debug_logf("patch_swapchain installed object=%p vtable=%p", swapchain, vtable);
}

void capture_created_interfaces(
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    IDXGISwapChain *swapchain)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);

  if (device) {
    patch_device(device);
    register_object_locked(device, trace::ObjectKind::Device, "ID3D11Device");
  }
  if (context) {
    patch_context(context);
    register_object_locked(context, trace::ObjectKind::Context, "ID3D11DeviceContext", lookup_object_id_locked(device));
  }
  if (swapchain) {
    patch_swapchain(swapchain);
    register_object_locked(swapchain, trace::ObjectKind::SwapChain, "IDXGISwapChain", lookup_object_id_locked(device));
  }
}

std::string create_device_payload_json(
    D3D_DRIVER_TYPE driver_type,
    UINT flags,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swap_chain_desc,
    D3D_FEATURE_LEVEL feature_level)
{
  std::ostringstream payload;
  payload << "{"
          << "\"driver_type\":\"" << driver_type_name(driver_type) << "\","
          << "\"flags\":" << flags << ","
          << "\"sdk_version\":" << sdk_version << ","
          << "\"feature_level\":\"" << feature_level_name(feature_level) << "\"";
  if (swap_chain_desc) {
    payload << ",\"swap_chain\":{\"width\":" << swap_chain_desc->BufferDesc.Width
            << ",\"height\":" << swap_chain_desc->BufferDesc.Height
            << ",\"format\":" << static_cast<unsigned int>(swap_chain_desc->BufferDesc.Format)
            << ",\"sample_count\":" << swap_chain_desc->SampleDesc.Count
            << ",\"sample_quality\":" << swap_chain_desc->SampleDesc.Quality
            << ",\"buffer_usage\":" << swap_chain_desc->BufferUsage
            << ",\"buffer_count\":" << swap_chain_desc->BufferCount
            << ",\"swap_effect\":" << static_cast<unsigned int>(swap_chain_desc->SwapEffect)
            << ",\"windowed\":" << bool_json(swap_chain_desc->Windowed == TRUE)
            << ",\"flags\":" << swap_chain_desc->Flags << "}";
  }
  payload << "}";
  return payload.str();
}

} // namespace

void process_attach()
{
}

void process_detach() noexcept
{
  runtime::shutdown_process_trace_session();
}

HRESULT WINAPI create_device(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **immediate_context)
{
  auto &downstream = downstream_module();
  if (!downstream.create_device) {
    return E_FAIL;
  }

  const HRESULT hr = downstream.create_device(
      adapter,
      driver_type,
      software,
      flags,
      feature_levels,
      feature_levels_count,
      sdk_version,
      device,
      feature_level,
      immediate_context);
  proxy_debug_logf(
      "create_device hr=%#lx device=%p context=%p feature_level=%u",
      static_cast<unsigned long>(hr),
      device ? *device : nullptr,
      immediate_context ? *immediate_context : nullptr,
      feature_level ? static_cast<unsigned int>(*feature_level) : 0U);

  capture_created_interfaces(device ? *device : nullptr, immediate_context ? *immediate_context : nullptr, nullptr);

  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  record_call_locked(
      "D3D11CreateDevice",
      hr,
      {device ? *device : nullptr, immediate_context ? *immediate_context : nullptr},
      {},
      create_device_payload_json(driver_type, flags, sdk_version, nullptr, feature_level ? *feature_level : D3D_FEATURE_LEVEL_11_0));
  return hr;
}

HRESULT WINAPI core_create_device(
    IDXGIFactory *factory,
    IDXGIAdapter *adapter,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    ID3D11Device **device)
{
  auto &downstream = downstream_module();
  if (!downstream.core_create_device) {
    return E_NOTIMPL;
  }

  const HRESULT hr = downstream.core_create_device(
      factory,
      adapter,
      flags,
      feature_levels,
      feature_levels_count,
      device);
  if (SUCCEEDED(hr) && device && *device) {
    capture_created_interfaces(*device, nullptr, nullptr);
  }
  return hr;
}

HRESULT WINAPI create_device_and_swap_chain(
    IDXGIAdapter *adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swap_chain_desc,
    IDXGISwapChain **swap_chain,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *feature_level,
    ID3D11DeviceContext **immediate_context)
{
  auto &downstream = downstream_module();
  if (!downstream.create_device_and_swap_chain) {
    return E_FAIL;
  }

  const HRESULT hr = downstream.create_device_and_swap_chain(
      adapter,
      driver_type,
      software,
      flags,
      feature_levels,
      feature_levels_count,
      sdk_version,
      swap_chain_desc,
      swap_chain,
      device,
      feature_level,
      immediate_context);
  proxy_debug_logf(
      "create_device_and_swap_chain hr=%#lx swapchain=%p device=%p context=%p feature_level=%u",
      static_cast<unsigned long>(hr),
      swap_chain ? *swap_chain : nullptr,
      device ? *device : nullptr,
      immediate_context ? *immediate_context : nullptr,
      feature_level ? static_cast<unsigned int>(*feature_level) : 0U);

  capture_created_interfaces(
      device ? *device : nullptr,
      immediate_context ? *immediate_context : nullptr,
      swap_chain ? *swap_chain : nullptr);

  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  record_call_locked(
      "D3D11CreateDeviceAndSwapChain",
      hr,
      {swap_chain ? *swap_chain : nullptr, device ? *device : nullptr, immediate_context ? *immediate_context : nullptr},
      {},
      create_device_payload_json(
          driver_type,
          flags,
          sdk_version,
          swap_chain_desc,
          feature_level ? *feature_level : D3D_FEATURE_LEVEL_11_0));
  return hr;
}

HRESULT WINAPI on12_create_device(
    IUnknown *device,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels,
    UINT feature_levels_count,
    IUnknown *const *command_queues,
    UINT num_command_queues,
    UINT node_mask,
    ID3D11Device **d3d11_device,
    ID3D11DeviceContext **immediate_context,
    D3D_FEATURE_LEVEL *chosen_feature_level)
{
  auto &downstream = downstream_module();
  if (!downstream.on12_create_device) {
    return E_NOTIMPL;
  }

  const HRESULT hr = downstream.on12_create_device(
      device,
      flags,
      feature_levels,
      feature_levels_count,
      command_queues,
      num_command_queues,
      node_mask,
      d3d11_device,
      immediate_context,
      chosen_feature_level);
  capture_created_interfaces(d3d11_device ? *d3d11_device : nullptr, immediate_context ? *immediate_context : nullptr, nullptr);
  return hr;
}

} // namespace apitrace::d3d11::internal
