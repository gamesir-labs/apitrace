#include "apitrace/d3d11_replay.hpp"

#include "d3d11_replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#endif

namespace apitrace::d3d11 {

D3D11ReplayBackend::D3D11ReplayBackend() = default;

bool D3D11ReplayBackend::initialize()
{
#ifdef _WIN32
  last_error_.clear();
  return true;
#else
  last_error_ = "D3D11 replay backend is only implemented for Windows retrace.exe.";
  return false;
#endif
}

bool D3D11ReplayBackend::replay_event(const trace::EventRecord &event)
{
  (void)event;
  last_error_ = "D3D11 replay backend requires typed replay commands.";
  return false;
}

void D3D11ReplayBackend::shutdown() {}

const std::string &D3D11ReplayBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::d3d11

namespace apitrace::d3d11::internal {

#ifdef _WIN32

namespace {

constexpr const char *kWindowClassName = "apitrace-retrace-d3d11";
constexpr const char *kDefaultWindowTitle = "apitrace retrace d3d11";

bool env_flag_enabled(const char *name, bool fallback = false)
{
  const char *value = std::getenv(name);
  if (!value || !*value) {
    return fallback;
  }
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0) {
    return true;
  }
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0) {
    return false;
  }
  return fallback;
}

std::string env_string(const char *primary, const char *fallback, const char *default_value)
{
  if (const char *value = std::getenv(primary); value && *value) {
    return value;
  }
  if (const char *value = std::getenv(fallback); value && *value) {
    return value;
  }
  return default_value;
}

std::string format_hresult(const char *operation, HRESULT hr)
{
  std::ostringstream message;
  message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
  return message.str();
}

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

D3D_DRIVER_TYPE driver_type_from_name(const std::string &driver_type)
{
  if (driver_type == "WARP") {
    return D3D_DRIVER_TYPE_WARP;
  }
  if (driver_type == "REFERENCE") {
    return D3D_DRIVER_TYPE_REFERENCE;
  }
  if (driver_type == "NULL") {
    return D3D_DRIVER_TYPE_NULL;
  }
  if (driver_type == "SOFTWARE") {
    return D3D_DRIVER_TYPE_SOFTWARE;
  }
  return D3D_DRIVER_TYPE_HARDWARE;
}

D3D_FEATURE_LEVEL feature_level_from_name(const std::string &feature_level)
{
  if (feature_level == "11_1") {
    return D3D_FEATURE_LEVEL_11_1;
  }
  if (feature_level == "10_1") {
    return D3D_FEATURE_LEVEL_10_1;
  }
  if (feature_level == "10_0") {
    return D3D_FEATURE_LEVEL_10_0;
  }
  if (feature_level == "9_3") {
    return D3D_FEATURE_LEVEL_9_3;
  }
  if (feature_level == "9_2") {
    return D3D_FEATURE_LEVEL_9_2;
  }
  if (feature_level == "9_1") {
    return D3D_FEATURE_LEVEL_9_1;
  }
  return D3D_FEATURE_LEVEL_11_0;
}

D3D11_MAP map_type_from_name(const std::string &map_type)
{
  if (map_type == "WRITE_DISCARD") {
    return D3D11_MAP_WRITE_DISCARD;
  }
  if (map_type == "WRITE") {
    return D3D11_MAP_WRITE;
  }
  if (map_type == "READ") {
    return D3D11_MAP_READ;
  }
  if (map_type == "READ_WRITE") {
    return D3D11_MAP_READ_WRITE;
  }
  if (map_type == "WRITE_NO_OVERWRITE") {
    return D3D11_MAP_WRITE_NO_OVERWRITE;
  }
  return D3D11_MAP_WRITE_DISCARD;
}

std::optional<D3D11_PRIMITIVE_TOPOLOGY> topology_from_name(const std::string &topology)
{
  if (topology == "TRIANGLELIST") {
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  }
  if (topology == "TRIANGLESTRIP") {
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
  }
  return std::nullopt;
}

struct PendingMapState {
  UINT subresource = 0;
  D3D11_MAP map_type = D3D11_MAP_WRITE_DISCARD;
  UINT map_flags = 0;
};

struct BufferState {
  ID3D11Buffer *buffer = nullptr;
  UINT byte_width = 0;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
  UINT bind_flags = 0;
  UINT cpu_access_flags = 0;
  std::optional<PendingMapState> pending_map;
};

class TranslationLayerRuntime {
public:
  TranslationLayerRuntime() = default;
  ~TranslationLayerRuntime()
  {
    shutdown();
  }

  bool run_plan(
      const replay::internal::D3D11ReplayPlan &plan,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    statistics.backend_name = "translation-layer-d3d11-dxmt";
    for (const auto &command : plan.commands) {
      const auto header = std::visit(
          [](const auto &typed_command) {
            return typed_command.header;
          },
          command);
      const bool ok = std::visit(
          [this, &statistics, &error](const auto &typed_command) {
            return execute_command(typed_command, statistics, error);
          },
          command);
      if (!ok) {
        if (!error.empty()) {
          error = "sequence " + std::to_string(header.sequence) + " " + header.label + ": " + error;
        }
        return false;
      }

      if (!std::holds_alternative<replay::internal::FrameBoundaryCommand>(command) &&
          !std::holds_alternative<replay::internal::PresentBoundaryCommand>(command)) {
        ++statistics.calls_replayed;
      }
    }

    if (show_window_) {
      const DWORD deadline = GetTickCount() + 750;
      while (GetTickCount() < deadline) {
        pump_messages();
        Sleep(15);
      }
    }
    return true;
  }

  void shutdown()
  {
    for (auto it = owned_objects_.rbegin(); it != owned_objects_.rend(); ++it) {
      if (*it) {
        (*it)->Release();
      }
    }
    owned_objects_.clear();
    buffers_.clear();
    resources_.clear();
    render_target_views_.clear();
    input_layouts_.clear();
    vertex_shaders_.clear();
    pixel_shaders_.clear();
    swapchains_.clear();
    devices_.clear();
    contexts_.clear();

    if (window_) {
      DestroyWindow(window_);
      window_ = nullptr;
    }
    if (window_class_registered_) {
      UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));
      window_class_registered_ = false;
    }
  }

private:
  bool ensure_window(
      const replay::internal::CreateDeviceAndSwapChainCommand &command,
      std::string &error)
  {
    if (window_) {
      return true;
    }

    WNDCLASSA window_class{};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = replay_window_proc;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      error = "RegisterClassA failed for retrace window";
      return false;
    }
    window_class_registered_ = true;

    RECT rect{0, 0, static_cast<LONG>(command.swap_chain.width), static_cast<LONG>(command.swap_chain.height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    show_window_ = env_flag_enabled("APITRACE_RETRACE_SHOW_WINDOW", false);
    const std::string window_title =
        env_string("APITRACE_RETRACE_WINDOW_TITLE", "APITRACE_VISUAL_WINDOW_TITLE", kDefaultWindowTitle);

    window_ = CreateWindowExA(
        0,
        kWindowClassName,
        window_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        GetModuleHandleA(nullptr),
        nullptr);
    if (!window_) {
      error = "CreateWindowExA failed for retrace window";
      return false;
    }

    if (show_window_) {
      ShowWindow(window_, SW_SHOWDEFAULT);
      UpdateWindow(window_);
    } else {
      ShowWindow(window_, SW_HIDE);
    }
    return true;
  }

  void own(IUnknown *object)
  {
    if (object) {
      owned_objects_.push_back(object);
    }
  }

  template <typename T>
  bool lookup_object(
      const std::unordered_map<trace::ObjectId, T *> &table,
      trace::ObjectId object_id,
      const char *what,
      T *&object,
      std::string &error) const
  {
    const auto it = table.find(object_id);
    if (it == table.end() || !it->second) {
      std::ostringstream message;
      message << "missing replay object " << what << " for object_id " << object_id;
      error = message.str();
      return false;
    }
    object = it->second;
    return true;
  }

  bool read_file_bytes(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes, std::string &error) const
  {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      error = "failed to open asset " + path.generic_string();
      return false;
    }
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(input.tellg());
    input.seekg(0, std::ios::beg);
    bytes.resize(size);
    if (size != 0) {
      input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
    }
    return true;
  }

  void pump_messages() const
  {
    MSG message{};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageA(&message);
    }
  }

  bool execute_command(
      const replay::internal::CreateDeviceAndSwapChainCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    if (!ensure_window(command, error)) {
      return false;
    }
    if (!devices_.empty() || !contexts_.empty() || !swapchains_.empty()) {
      error = "multiple D3D11CreateDeviceAndSwapChain calls are unsupported in the MVP";
      return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = command.swap_chain.width;
    desc.BufferDesc.Height = command.swap_chain.height;
    desc.BufferDesc.Format = static_cast<DXGI_FORMAT>(command.swap_chain.format);
    desc.BufferUsage = command.swap_chain.buffer_usage == 0 ? DXGI_USAGE_RENDER_TARGET_OUTPUT : command.swap_chain.buffer_usage;
    desc.BufferCount = command.swap_chain.buffer_count == 0 ? 1 : command.swap_chain.buffer_count;
    desc.OutputWindow = window_;
    desc.SampleDesc.Count = command.swap_chain.sample_count == 0 ? 1 : command.swap_chain.sample_count;
    desc.SampleDesc.Quality = command.swap_chain.sample_quality;
    desc.Windowed = command.swap_chain.windowed ? TRUE : FALSE;
    desc.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(command.swap_chain.swap_effect);
    desc.Flags = command.swap_chain.flags;

    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGISwapChain *swapchain = nullptr;
    D3D_FEATURE_LEVEL requested_feature_level = feature_level_from_name(command.feature_level);
    D3D_FEATURE_LEVEL created_feature_level = requested_feature_level;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        driver_type_from_name(command.driver_type),
        nullptr,
        command.flags,
        &requested_feature_level,
        1,
        command.sdk_version,
        &desc,
        &swapchain,
        &device,
        &created_feature_level,
        &context);
    if (FAILED(hr)) {
      error = format_hresult("D3D11CreateDeviceAndSwapChain", hr);
      return false;
    }

    own(device);
    own(context);
    own(swapchain);
    devices_[command.device_id] = device;
    contexts_[command.context_id] = context;
    swapchains_[command.swap_chain_id] = swapchain;
    return true;
  }

  bool execute_command(
      const replay::internal::GetBufferCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    IDXGISwapChain *swapchain = nullptr;
    if (!lookup_object(swapchains_, command.swap_chain_id, "swapchain", swapchain, error)) {
      return false;
    }

    ID3D11Texture2D *back_buffer = nullptr;
    const HRESULT hr = swapchain->GetBuffer(command.buffer_index, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
    if (FAILED(hr)) {
      error = format_hresult("IDXGISwapChain::GetBuffer", hr);
      return false;
    }

    own(back_buffer);
    resources_[command.resource_id] = back_buffer;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateRenderTargetViewCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    if (command.desc_present) {
      error = "CreateRenderTargetView with explicit desc is unsupported in the MVP";
      return false;
    }

    ID3D11Device *device = nullptr;
    ID3D11Resource *resource = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error) ||
        !lookup_object(resources_, command.resource_id, "resource", resource, error)) {
      return false;
    }

    ID3D11RenderTargetView *view = nullptr;
    const HRESULT hr = device->CreateRenderTargetView(resource, nullptr, &view);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateRenderTargetView", hr);
      return false;
    }

    own(view);
    render_target_views_[command.view_id] = view;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateInputLayoutCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    std::vector<std::uint8_t> shader_bytes;
    if (!read_file_bytes(command.shader_path, shader_bytes, error)) {
      return false;
    }

    std::vector<std::string> semantic_names;
    semantic_names.reserve(command.elements.size());
    for (const auto &element : command.elements) {
      semantic_names.push_back(element.semantic_name);
    }

    std::vector<D3D11_INPUT_ELEMENT_DESC> descriptors;
    descriptors.reserve(command.elements.size());
    for (std::size_t index = 0; index < command.elements.size(); ++index) {
      const auto &element = command.elements[index];
      D3D11_INPUT_ELEMENT_DESC desc{};
      desc.SemanticName = semantic_names[index].c_str();
      desc.SemanticIndex = element.semantic_index;
      desc.Format = static_cast<DXGI_FORMAT>(element.format);
      desc.InputSlot = element.input_slot;
      desc.AlignedByteOffset = element.aligned_byte_offset;
      desc.InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(element.input_slot_class);
      desc.InstanceDataStepRate = element.instance_data_step_rate;
      descriptors.push_back(desc);
    }

    ID3D11InputLayout *input_layout = nullptr;
    const HRESULT hr = device->CreateInputLayout(
        descriptors.data(),
        static_cast<UINT>(descriptors.size()),
        shader_bytes.data(),
        shader_bytes.size(),
        &input_layout);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateInputLayout", hr);
      return false;
    }

    own(input_layout);
    input_layouts_[command.input_layout_id] = input_layout;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateShaderCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    std::vector<std::uint8_t> shader_bytes;
    if (!read_file_bytes(command.shader_path, shader_bytes, error)) {
      return false;
    }

    if (command.vertex_stage) {
      ID3D11VertexShader *shader = nullptr;
      const HRESULT hr = device->CreateVertexShader(shader_bytes.data(), shader_bytes.size(), nullptr, &shader);
      if (FAILED(hr)) {
        error = format_hresult("ID3D11Device::CreateVertexShader", hr);
        return false;
      }
      own(shader);
      vertex_shaders_[command.shader_id] = shader;
      return true;
    }

    ID3D11PixelShader *shader = nullptr;
    const HRESULT hr = device->CreatePixelShader(shader_bytes.data(), shader_bytes.size(), nullptr, &shader);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreatePixelShader", hr);
      return false;
    }
    own(shader);
    pixel_shaders_[command.shader_id] = shader;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateBufferCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = command.byte_width;
    desc.Usage = static_cast<D3D11_USAGE>(command.usage);
    desc.BindFlags = command.bind_flags;
    desc.CPUAccessFlags = command.cpu_access_flags;

    std::vector<std::uint8_t> initial_data_bytes;
    D3D11_SUBRESOURCE_DATA initial_data{};
    D3D11_SUBRESOURCE_DATA *initial_data_ptr = nullptr;
    if (command.has_initial_data) {
      if (!read_file_bytes(command.initial_data_path, initial_data_bytes, error)) {
        return false;
      }
      if (initial_data_bytes.size() < command.byte_width) {
        error = "initial buffer payload is smaller than ByteWidth";
        return false;
      }
      initial_data.pSysMem = initial_data_bytes.data();
      initial_data_ptr = &initial_data;
    }

    ID3D11Buffer *buffer = nullptr;
    const HRESULT hr = device->CreateBuffer(&desc, initial_data_ptr, &buffer);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateBuffer", hr);
      return false;
    }

    own(buffer);
    resources_[command.buffer_id] = buffer;
    buffers_[command.buffer_id] = BufferState{
        buffer,
        command.byte_width,
        static_cast<D3D11_USAGE>(command.usage),
        command.bind_flags,
        command.cpu_access_flags,
        std::nullopt,
    };
    return true;
  }

  bool execute_command(const replay::internal::MapCommand &command, replay::ReplayStatistics &statistics, std::string &error)
  {
    (void)statistics;
    const auto buffer_it = buffers_.find(command.resource_id);
    if (buffer_it == buffers_.end() || !buffer_it->second.buffer) {
      error = "missing replay buffer for Map";
      return false;
    }
    buffer_it->second.pending_map = PendingMapState{
        command.subresource,
        map_type_from_name(command.map_type),
        command.map_flags,
    };
    return true;
  }

  bool execute_command(
      const replay::internal::UnmapCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    const auto buffer_it = buffers_.find(command.resource_id);
    if (buffer_it == buffers_.end() || !buffer_it->second.buffer) {
      error = "missing replay buffer for Unmap";
      return false;
    }
    if (!buffer_it->second.pending_map.has_value()) {
      error = "Unmap without a prior Map is unsupported in the MVP";
      return false;
    }

    std::vector<std::uint8_t> snapshot_bytes;
    if (!read_file_bytes(command.snapshot_path, snapshot_bytes, error)) {
      return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto pending = *buffer_it->second.pending_map;
    const HRESULT hr = context->Map(buffer_it->second.buffer, pending.subresource, pending.map_type, pending.map_flags, &mapped);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11DeviceContext::Map", hr);
      return false;
    }

    const std::size_t copy_size = std::min<std::size_t>(snapshot_bytes.size(), buffer_it->second.byte_width);
    if (copy_size != 0) {
      std::memcpy(mapped.pData, snapshot_bytes.data(), copy_size);
    }
    if (copy_size < buffer_it->second.byte_width) {
      std::memset(static_cast<std::uint8_t *>(mapped.pData) + copy_size, 0, buffer_it->second.byte_width - copy_size);
    }
    context->Unmap(buffer_it->second.buffer, pending.subresource);
    buffer_it->second.pending_map.reset();
    return true;
  }

  bool execute_command(
      const replay::internal::SetRenderTargetsCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    if (command.has_depth_stencil) {
      error = "depth-stencil OMSetRenderTargets is unsupported in the MVP";
      return false;
    }

    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<ID3D11RenderTargetView *> views;
    views.reserve(command.render_target_view_ids.size());
    for (const auto view_id : command.render_target_view_ids) {
      ID3D11RenderTargetView *view = nullptr;
      if (!lookup_object(render_target_views_, view_id, "render-target-view", view, error)) {
        return false;
      }
      views.push_back(view);
    }

    context->OMSetRenderTargets(
        static_cast<UINT>(views.size()),
        views.empty() ? nullptr : views.data(),
        nullptr);
    return true;
  }

  bool execute_command(
      const replay::internal::SetViewportsCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<D3D11_VIEWPORT> viewports;
    viewports.reserve(command.viewports.size());
    for (const auto &source : command.viewports) {
      D3D11_VIEWPORT viewport{};
      viewport.TopLeftX = source.top_left_x;
      viewport.TopLeftY = source.top_left_y;
      viewport.Width = source.width;
      viewport.Height = source.height;
      viewport.MinDepth = source.min_depth;
      viewport.MaxDepth = source.max_depth;
      viewports.push_back(viewport);
    }

    context->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    return true;
  }

  bool execute_command(
      const replay::internal::ClearRenderTargetViewCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ID3D11RenderTargetView *view = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_object(render_target_views_, command.render_target_view_id, "render-target-view", view, error)) {
      return false;
    }

    context->ClearRenderTargetView(view, command.color.data());
    return true;
  }

  bool execute_command(
      const replay::internal::SetInputLayoutCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ID3D11InputLayout *input_layout = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_object(input_layouts_, command.input_layout_id, "input-layout", input_layout, error)) {
      return false;
    }

    context->IASetInputLayout(input_layout);
    return true;
  }

  bool execute_command(
      const replay::internal::SetVertexBuffersCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<ID3D11Buffer *> buffers;
    std::vector<UINT> strides;
    std::vector<UINT> offsets;
    buffers.reserve(command.bindings.size());
    strides.reserve(command.bindings.size());
    offsets.reserve(command.bindings.size());
    for (const auto &binding : command.bindings) {
      if (binding.buffer_id == 0) {
        buffers.push_back(nullptr);
      } else {
        const auto buffer_it = buffers_.find(binding.buffer_id);
        if (buffer_it == buffers_.end() || !buffer_it->second.buffer) {
          error = "missing replay vertex buffer";
          return false;
        }
        buffers.push_back(buffer_it->second.buffer);
      }
      strides.push_back(binding.stride);
      offsets.push_back(binding.offset);
    }

    context->IASetVertexBuffers(
        command.start_slot,
        static_cast<UINT>(buffers.size()),
        buffers.empty() ? nullptr : buffers.data(),
        strides.empty() ? nullptr : strides.data(),
        offsets.empty() ? nullptr : offsets.data());
    return true;
  }

  bool execute_command(
      const replay::internal::SetPrimitiveTopologyCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    const auto topology = topology_from_name(command.topology);
    if (!topology.has_value()) {
      error = "unsupported primitive topology " + command.topology;
      return false;
    }
    context->IASetPrimitiveTopology(*topology);
    return true;
  }

  bool execute_command(
      const replay::internal::SetShaderCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    if (command.vertex_stage) {
      ID3D11VertexShader *shader = nullptr;
      if (!lookup_object(vertex_shaders_, command.shader_id, "vertex-shader", shader, error)) {
        return false;
      }
      context->VSSetShader(shader, nullptr, 0);
      return true;
    }

    ID3D11PixelShader *shader = nullptr;
    if (!lookup_object(pixel_shaders_, command.shader_id, "pixel-shader", shader, error)) {
      return false;
    }
    context->PSSetShader(shader, nullptr, 0);
    return true;
  }

  bool execute_command(
      const replay::internal::SetConstantBuffersCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<ID3D11Buffer *> buffers;
    buffers.reserve(command.buffer_ids.size());
    for (const auto buffer_id : command.buffer_ids) {
      if (buffer_id == 0) {
        buffers.push_back(nullptr);
        continue;
      }
      const auto buffer_it = buffers_.find(buffer_id);
      if (buffer_it == buffers_.end() || !buffer_it->second.buffer) {
        error = "missing replay constant buffer";
        return false;
      }
      buffers.push_back(buffer_it->second.buffer);
    }

    if (command.vertex_stage) {
      context->VSSetConstantBuffers(command.start_slot, static_cast<UINT>(buffers.size()), buffers.data());
    } else {
      context->PSSetConstantBuffers(command.start_slot, static_cast<UINT>(buffers.size()), buffers.data());
    }
    return true;
  }

  bool execute_command(const replay::internal::DrawCommand &command, replay::ReplayStatistics &statistics, std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    context->Draw(command.vertex_count, command.start_vertex_location);
    return true;
  }

  bool execute_command(
      const replay::internal::PresentCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    IDXGISwapChain *swapchain = nullptr;
    if (!lookup_object(swapchains_, command.swap_chain_id, "swapchain", swapchain, error)) {
      return false;
    }

    const HRESULT hr = swapchain->Present(command.sync_interval, command.flags);
    if (FAILED(hr)) {
      error = format_hresult("IDXGISwapChain::Present", hr);
      return false;
    }

    pump_messages();
    if (show_window_) {
      Sleep(5);
    }
    return true;
  }

  bool execute_command(
      const replay::internal::FrameBoundaryCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)error;
    if (command.label == "FrameBegin") {
      ++statistics.frames_seen;
    }
    return true;
  }

  bool execute_command(
      const replay::internal::PresentBoundaryCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)command;
    (void)error;
    ++statistics.presents_seen;
    return true;
  }

  HWND window_ = nullptr;
  bool window_class_registered_ = false;
  bool show_window_ = false;
  std::vector<IUnknown *> owned_objects_;
  std::unordered_map<trace::ObjectId, ID3D11Device *> devices_;
  std::unordered_map<trace::ObjectId, ID3D11DeviceContext *> contexts_;
  std::unordered_map<trace::ObjectId, IDXGISwapChain *> swapchains_;
  std::unordered_map<trace::ObjectId, ID3D11Resource *> resources_;
  std::unordered_map<trace::ObjectId, BufferState> buffers_;
  std::unordered_map<trace::ObjectId, ID3D11RenderTargetView *> render_target_views_;
  std::unordered_map<trace::ObjectId, ID3D11InputLayout *> input_layouts_;
  std::unordered_map<trace::ObjectId, ID3D11VertexShader *> vertex_shaders_;
  std::unordered_map<trace::ObjectId, ID3D11PixelShader *> pixel_shaders_;
};

} // namespace

bool replay_translation_layer_plan(
    const replay::internal::D3D11ReplayPlan &plan,
    replay::ReplayStatistics &statistics,
    std::string &error)
{
  TranslationLayerRuntime runtime;
  return runtime.run_plan(plan, statistics, error);
}

#else

bool replay_translation_layer_plan(
    const replay::internal::D3D11ReplayPlan &plan,
    replay::ReplayStatistics &statistics,
    std::string &error)
{
  (void)plan;
  (void)statistics;
  error = "Translation-layer D3D11 replay is only implemented for Windows retrace.exe.";
  return false;
}

#endif

} // namespace apitrace::d3d11::internal
