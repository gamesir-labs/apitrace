#include "apitrace/d3d11_replay.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include "d3d11_replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(APITRACE_HAS_D3D_NATIVE)
#include <d3d11.h>
#include <dxgi.h>
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(APITRACE_HAS_D3D_NATIVE) && defined(__APPLE__)
#include "apitrace/platform/macos_window.hpp"
#endif

namespace apitrace::d3d11 {

D3D11ReplayBackend::D3D11ReplayBackend() = default;

bool D3D11ReplayBackend::initialize()
{
#if defined(APITRACE_HAS_D3D_NATIVE)
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

#if defined(APITRACE_HAS_D3D_NATIVE)

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
#endif

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

constexpr UINT texture_bytes_per_pixel(DXGI_FORMAT format)
{
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_D32_FLOAT:
    return 4;
  case DXGI_FORMAT_R16_UINT:
    return 2;
  case DXGI_FORMAT_R32_UINT:
    return 4;
  default:
    return 0;
  }
}

UINT mip_extent(UINT base_extent, UINT mip_level)
{
  const UINT shifted = base_extent >> mip_level;
  return shifted == 0 ? 1U : shifted;
}

struct PendingMapState {
  UINT subresource = 0;
  D3D11_MAP map_type = D3D11_MAP_WRITE_DISCARD;
  UINT map_flags = 0;
};

struct ReplayResourceState {
  ID3D11Resource *resource = nullptr;
  ID3D11Buffer *buffer = nullptr;
  ID3D11Texture2D *texture2d = nullptr;
  replay::internal::ReplayResourceClass resource_class = replay::internal::ReplayResourceClass::Unknown;
  UINT byte_width = 0;
  UINT width = 0;
  UINT height = 0;
  UINT mip_levels = 0;
  UINT array_size = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT sample_count = 1;
  UINT sample_quality = 0;
  UINT bind_flags = 0;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
  UINT cpu_access_flags = 0;
  UINT misc_flags = 0;
  std::optional<PendingMapState> pending_map;
};

struct BoundIndexBufferState {
  trace::ObjectId buffer_id = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT offset = 0;
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
          !std::holds_alternative<replay::internal::PresentBoundaryCommand>(command) &&
          !std::holds_alternative<replay::internal::DebugMarkerCommand>(command)) {
        ++statistics.calls_replayed;
      }
    }

    if (!present_calls_.empty() || !open_frames_.empty() ||
        next_present_call_frame_index_ != next_present_boundary_frame_index_ ||
        next_frame_begin_index_ != next_frame_end_index_) {
      error = "D3D11 present boundary count does not match replayed IDXGISwapChain::Present calls";
      return false;
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
    devices_.clear();
    contexts_.clear();
    swapchains_.clear();
    present_calls_.clear();
    open_frames_.clear();
    resource_states_.clear();
    render_target_views_.clear();
    shader_resource_views_.clear();
    depth_stencil_views_.clear();
    input_layouts_.clear();
    vertex_shaders_.clear();
    pixel_shaders_.clear();
    sampler_states_.clear();
    blend_states_.clear();
    depth_stencil_states_.clear();
    rasterizer_states_.clear();
    next_frame_begin_index_ = 0;
    next_frame_end_index_ = 0;
    next_present_call_frame_index_ = 0;
    next_present_boundary_frame_index_ = 0;

#ifdef _WIN32
    if (window_) {
      DestroyWindow(window_);
      window_ = nullptr;
    }
    if (window_class_registered_) {
      UnregisterClassA(kWindowClassName, GetModuleHandleA(nullptr));
      window_class_registered_ = false;
    }
#elif defined(__APPLE__)
    window_ = nullptr;
    apitrace::platform::macos::destroy_window(window_handles_);
#endif
#if !defined(_WIN32)
    if (capture_writer_) {
      capture_writer_->close();
      capture_writer_.reset();
    }
#endif
  }

private:
  bool ensure_window(const replay::internal::CreateDeviceAndSwapChainCommand &command, std::string &error)
  {
    if (window_) {
      return true;
    }

#ifdef _WIN32
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
#elif defined(__APPLE__)
    show_window_ = env_flag_enabled("APITRACE_RETRACE_SHOW_WINDOW", false);
    const std::string window_title =
        env_string("APITRACE_RETRACE_WINDOW_TITLE", "APITRACE_VISUAL_WINDOW_TITLE", kDefaultWindowTitle);
    apitrace::platform::macos::WindowSpec spec;
    spec.width = command.swap_chain.width;
    spec.height = command.swap_chain.height;
    spec.title = window_title;
    spec.show = show_window_;
    if (!apitrace::platform::macos::create_window(spec, window_handles_, error)) {
      if (error.empty()) {
        error = "failed to create native macOS D3D11 replay window";
      }
      return false;
    }
    window_ = static_cast<HWND>(window_handles_.nswindow);
    if (!window_) {
      error = "native macOS D3D11 replay window did not produce an HWND token";
      return false;
    }
#else
    error = "D3D11 native replay has no window bootstrap for this platform";
    return false;
#endif
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

  bool lookup_resource_state(trace::ObjectId object_id, const char *what, ReplayResourceState *&state, std::string &error)
  {
    const auto it = resource_states_.find(object_id);
    if (it == resource_states_.end() || !it->second.resource) {
      std::ostringstream message;
      message << "missing replay resource " << what << " for object_id " << object_id;
      error = message.str();
      return false;
    }
    state = &it->second;
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
#ifdef _WIN32
    MSG message{};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageA(&message);
    }
#elif defined(__APPLE__)
    auto &handles = const_cast<apitrace::platform::macos::WindowHandles &>(window_handles_);
    apitrace::platform::macos::pump_events(handles);
#endif
  }

#if !defined(_WIN32)
  bool ensure_capture_writer(std::string &error)
  {
    if (capture_writer_) {
      return true;
    }
    const char *bundle_root = std::getenv("APITRACE_TRACE_BUNDLE");
    if (bundle_root == nullptr || *bundle_root == '\0') {
      error = "D3D11 native present capture requires APITRACE_TRACE_BUNDLE";
      return false;
    }
    capture_writer_ = std::make_unique<trace::TraceBundleWriter>();
    if (!capture_writer_->open(bundle_root)) {
      capture_writer_.reset();
      error = "failed to open APITRACE_TRACE_BUNDLE for native D3D11 present capture";
      return false;
    }
    trace::TraceMetadata metadata;
    metadata.api = trace::ApiKind::D3D11;
    metadata.producer = "apitrace_d3d11_native_retrace";
    capture_writer_->write_metadata(metadata);
    return true;
  }

  bool capture_present_frame(
      IDXGISwapChain *swapchain,
      std::uint64_t frame_index,
      UINT sync_interval,
      UINT flags,
      std::string &error)
  {
    if (!env_flag_enabled("APITRACE_D3D11_RETRACE_CAPTURE_PRESENT_FRAMES")) {
      return true;
    }
    if (!ensure_capture_writer(error)) {
      return false;
    }

    ID3D11Texture2D *back_buffer = nullptr;
    HRESULT hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
    if (FAILED(hr) || back_buffer == nullptr) {
      error = format_hresult("IDXGISwapChain::GetBuffer(present-capture)", hr);
      return false;
    }

    ID3D11Device *device = nullptr;
    back_buffer->GetDevice(&device);
    if (device == nullptr) {
      back_buffer->Release();
      error = "present-frame capture could not resolve ID3D11Device";
      return false;
    }

    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr) {
      device->Release();
      back_buffer->Release();
      error = "present-frame capture could not resolve ID3D11DeviceContext";
      return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
      context->Release();
      device->Release();
      back_buffer->Release();
      error = "present-frame capture currently requires a non-empty RGBA8 swapchain";
      return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    ID3D11Texture2D *staging = nullptr;
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || staging == nullptr) {
      context->Release();
      device->Release();
      back_buffer->Release();
      error = format_hresult("ID3D11Device::CreateTexture2D(present-capture)", hr);
      return false;
    }

    context->CopyResource(staging, back_buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == nullptr) {
      staging->Release();
      context->Release();
      device->Release();
      back_buffer->Release();
      error = format_hresult("ID3D11DeviceContext::Map(present-capture)", hr);
      return false;
    }

    const UINT row_pitch = desc.Width * 4u;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(desc.Height));
    for (UINT row = 0; row < desc.Height; ++row) {
      const auto *src = static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch;
      auto *dst = rgba.data() + static_cast<std::size_t>(row) * row_pitch;
      std::memcpy(dst, src, row_pitch);
    }
    context->Unmap(staging, 0);
    staging->Release();
    context->Release();
    device->Release();
    back_buffer->Release();

    trace::AssetRecord asset;
    asset.blob_id = ++capture_sequence_;
    asset.kind = trace::AssetKind::Texture;
    asset.debug_name = "d3d11-present-frame";
    asset.payload_bytes = std::move(rgba);
    asset = capture_writer_->register_asset(std::move(asset));

    std::ostringstream payload;
    payload << "{"
            << "\"frame_index\":" << frame_index << ","
            << "\"width\":" << desc.Width << ","
            << "\"height\":" << desc.Height << ","
            << "\"row_pitch\":" << row_pitch << ","
            << "\"sync_interval\":" << sync_interval << ","
            << "\"flags\":" << flags << ","
            << "\"format\":\"rgba8\","
            << "\"frame_path\":\"" << asset.relative_path.generic_string() << "\""
            << "}";

    trace::EventRecord event;
    event.kind = trace::EventKind::ResourceBlob;
    event.callsite.sequence = ++capture_sequence_;
    event.object_debug_name = "D3D11PresentFrame";
    event.blob_refs = {asset.blob_id};
    event.payload = payload.str();
    capture_writer_->append_call_event(event);
    return true;
  }
#else
  bool capture_present_frame(IDXGISwapChain *, std::uint64_t, UINT, UINT, std::string &)
  {
    return true;
  }
#endif

  void store_buffer_resource(
      trace::ObjectId object_id,
      ID3D11Buffer *buffer,
      UINT byte_width,
      D3D11_USAGE usage,
      UINT bind_flags,
      UINT cpu_access_flags)
  {
    resource_states_[object_id] = ReplayResourceState{
        buffer,
        buffer,
        nullptr,
        replay::internal::ReplayResourceClass::Buffer,
        byte_width,
        0,
        0,
        0,
        0,
        DXGI_FORMAT_UNKNOWN,
        1,
        0,
        bind_flags,
        usage,
        cpu_access_flags,
        0,
        std::nullopt,
    };
  }

  void store_texture_resource(trace::ObjectId object_id, ID3D11Texture2D *texture, const D3D11_TEXTURE2D_DESC &desc)
  {
    resource_states_[object_id] = ReplayResourceState{
        texture,
        nullptr,
        texture,
        replay::internal::ReplayResourceClass::Texture2D,
        0,
        desc.Width,
        desc.Height,
        desc.MipLevels,
        desc.ArraySize,
        desc.Format,
        desc.SampleDesc.Count == 0 ? 1u : desc.SampleDesc.Count,
        desc.SampleDesc.Quality,
        desc.BindFlags,
        desc.Usage,
        desc.CPUAccessFlags,
        desc.MiscFlags,
        std::nullopt,
    };
  }

  static D3D11_TEXTURE2D_DESC texture_desc_from_command(const replay::internal::Texture2DDesc &source)
  {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source.width;
    desc.Height = source.height;
    desc.MipLevels = source.mip_levels;
    desc.ArraySize = source.array_size;
    desc.Format = static_cast<DXGI_FORMAT>(source.format);
    desc.SampleDesc.Count = source.sample_count == 0 ? 1 : source.sample_count;
    desc.SampleDesc.Quality = source.sample_quality;
    desc.Usage = static_cast<D3D11_USAGE>(source.usage);
    desc.BindFlags = source.bind_flags;
    desc.CPUAccessFlags = source.cpu_access_flags;
    desc.MiscFlags = source.misc_flags;
    return desc;
  }

  static D3D11_SHADER_RESOURCE_VIEW_DESC shader_resource_view_desc_from_command(
      const replay::internal::ShaderResourceViewDesc &source)
  {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(source.format);
    desc.ViewDimension = static_cast<D3D11_SRV_DIMENSION>(source.view_dimension);
    if (source.has_texture2d) {
      desc.Texture2D.MostDetailedMip = source.texture2d_most_detailed_mip;
      desc.Texture2D.MipLevels = source.texture2d_mip_levels;
    }
    return desc;
  }

  static D3D11_RENDER_TARGET_VIEW_DESC render_target_view_desc_from_command(
      const replay::internal::CreateRenderTargetViewCommand &source)
  {
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(source.format);
    desc.ViewDimension = static_cast<D3D11_RTV_DIMENSION>(source.view_dimension);
    desc.Texture2D.MipSlice = source.texture2d_mip_slice;
    return desc;
  }

  static D3D11_DEPTH_STENCIL_VIEW_DESC depth_stencil_view_desc_from_command(
      const replay::internal::DepthStencilViewDesc &source)
  {
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(source.format);
    desc.ViewDimension = static_cast<D3D11_DSV_DIMENSION>(source.view_dimension);
    desc.Flags = source.flags;
    if (source.has_texture2d) {
      desc.Texture2D.MipSlice = source.texture2d_mip_slice;
    }
    return desc;
  }

  static D3D11_SAMPLER_DESC sampler_desc_from_command(const replay::internal::SamplerStateDesc &source)
  {
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = static_cast<D3D11_FILTER>(source.filter);
    desc.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(source.address_u);
    desc.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(source.address_v);
    desc.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(source.address_w);
    desc.MipLODBias = source.mip_lod_bias;
    desc.MaxAnisotropy = source.max_anisotropy;
    desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(source.comparison_func);
    std::copy(source.border_color.begin(), source.border_color.end(), desc.BorderColor);
    desc.MinLOD = source.min_lod;
    desc.MaxLOD = source.max_lod;
    return desc;
  }

  static D3D11_BLEND_DESC blend_desc_from_command(const replay::internal::BlendStateDesc &source)
  {
    D3D11_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = source.alpha_to_coverage_enable ? TRUE : FALSE;
    desc.IndependentBlendEnable = source.independent_blend_enable ? TRUE : FALSE;
    for (std::size_t index = 0; index < source.render_targets.size(); ++index) {
      const auto &input = source.render_targets[index];
      auto &target = desc.RenderTarget[index];
      target.BlendEnable = input.blend_enable ? TRUE : FALSE;
      target.SrcBlend = static_cast<D3D11_BLEND>(input.src_blend);
      target.DestBlend = static_cast<D3D11_BLEND>(input.dest_blend);
      target.BlendOp = static_cast<D3D11_BLEND_OP>(input.blend_op);
      target.SrcBlendAlpha = static_cast<D3D11_BLEND>(input.src_blend_alpha);
      target.DestBlendAlpha = static_cast<D3D11_BLEND>(input.dest_blend_alpha);
      target.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(input.blend_op_alpha);
      target.RenderTargetWriteMask = input.write_mask;
    }
    return desc;
  }

  static D3D11_DEPTH_STENCIL_DESC depth_stencil_state_desc_from_command(
      const replay::internal::DepthStencilStateDesc &source)
  {
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = source.depth_enable ? TRUE : FALSE;
    desc.DepthWriteMask = static_cast<D3D11_DEPTH_WRITE_MASK>(source.depth_write_mask);
    desc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(source.depth_func);
    desc.StencilEnable = source.stencil_enable ? TRUE : FALSE;
    desc.StencilReadMask = source.stencil_read_mask;
    desc.StencilWriteMask = source.stencil_write_mask;
    return desc;
  }

  static D3D11_RASTERIZER_DESC rasterizer_desc_from_command(const replay::internal::RasterizerStateDesc &source)
  {
    D3D11_RASTERIZER_DESC desc{};
    desc.FillMode = static_cast<D3D11_FILL_MODE>(source.fill_mode);
    desc.CullMode = static_cast<D3D11_CULL_MODE>(source.cull_mode);
    desc.FrontCounterClockwise = source.front_counter_clockwise ? TRUE : FALSE;
    desc.DepthBias = source.depth_bias;
    desc.DepthBiasClamp = source.depth_bias_clamp;
    desc.SlopeScaledDepthBias = source.slope_scaled_depth_bias;
    desc.DepthClipEnable = source.depth_clip_enable ? TRUE : FALSE;
    desc.ScissorEnable = source.scissor_enable ? TRUE : FALSE;
    desc.MultisampleEnable = source.multisample_enable ? TRUE : FALSE;
    desc.AntialiasedLineEnable = source.antialiased_line_enable ? TRUE : FALSE;
    return desc;
  }

  static UINT texture_subresource_height(const ReplayResourceState &resource, UINT subresource)
  {
    const UINT mip_levels = resource.mip_levels == 0 ? 1 : resource.mip_levels;
    return mip_extent(resource.height, subresource % mip_levels);
  }

  static UINT texture_subresource_width(const ReplayResourceState &resource, UINT subresource)
  {
    const UINT mip_levels = resource.mip_levels == 0 ? 1 : resource.mip_levels;
    return mip_extent(resource.width, subresource % mip_levels);
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
      error = "multiple D3D11CreateDeviceAndSwapChain calls are unsupported";
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

  bool execute_command(const replay::internal::GetBufferCommand &command, replay::ReplayStatistics &statistics, std::string &error)
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

    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);
    own(back_buffer);
    store_texture_resource(command.resource_id, back_buffer, desc);
    return true;
  }

  bool execute_command(
      const replay::internal::CreateRenderTargetViewCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    ReplayResourceState *resource = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error) ||
        !lookup_resource_state(command.resource_id, "resource", resource, error)) {
      return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    const D3D11_RENDER_TARGET_VIEW_DESC *desc_ptr = nullptr;
    if (command.desc_present) {
      desc = render_target_view_desc_from_command(command);
      desc_ptr = &desc;
    }

    ID3D11RenderTargetView *view = nullptr;
    const HRESULT hr = device->CreateRenderTargetView(resource->resource, desc_ptr, &view);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateRenderTargetView", hr);
      return false;
    }

    own(view);
    render_target_views_[command.view_id] = view;
    return true;
  }

  bool execute_command(const replay::internal::CreateTexture2DCommand &command, replay::ReplayStatistics &statistics, std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    const D3D11_TEXTURE2D_DESC desc = texture_desc_from_command(command.desc);
    std::vector<std::uint8_t> initial_data_bytes;
    D3D11_SUBRESOURCE_DATA initial_data{};
    D3D11_SUBRESOURCE_DATA *initial_data_ptr = nullptr;
    if (command.has_initial_data) {
      if (!read_file_bytes(command.initial_data_path, initial_data_bytes, error)) {
        return false;
      }
      initial_data.pSysMem = initial_data_bytes.data();
      if (desc.Height > 0 && !initial_data_bytes.empty()) {
        initial_data.SysMemPitch = static_cast<UINT>(initial_data_bytes.size() / desc.Height);
      }
      initial_data.SysMemSlicePitch = static_cast<UINT>(initial_data_bytes.size());
      initial_data_ptr = &initial_data;
    }

    ID3D11Texture2D *texture = nullptr;
    const HRESULT hr = device->CreateTexture2D(&desc, initial_data_ptr, &texture);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateTexture2D", hr);
      return false;
    }

    own(texture);
    store_texture_resource(command.texture_id, texture, desc);
    return true;
  }

  bool execute_command(
      const replay::internal::CreateShaderResourceViewCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    ReplayResourceState *resource = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error) ||
        !lookup_resource_state(command.resource_id, "resource", resource, error)) {
      return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    const D3D11_SHADER_RESOURCE_VIEW_DESC *desc_ptr = nullptr;
    if (command.desc_present) {
      desc = shader_resource_view_desc_from_command(command.desc);
      desc_ptr = &desc;
    }

    ID3D11ShaderResourceView *view = nullptr;
    const HRESULT hr = device->CreateShaderResourceView(resource->resource, desc_ptr, &view);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateShaderResourceView", hr);
      return false;
    }

    own(view);
    shader_resource_views_[command.view_id] = view;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateSamplerStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    const D3D11_SAMPLER_DESC desc = sampler_desc_from_command(command.desc);
    ID3D11SamplerState *sampler = nullptr;
    const HRESULT hr = device->CreateSamplerState(&desc, &sampler);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateSamplerState", hr);
      return false;
    }

    own(sampler);
    sampler_states_[command.sampler_id] = sampler;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateDepthStencilViewCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    ReplayResourceState *resource = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error) ||
        !lookup_resource_state(command.resource_id, "resource", resource, error)) {
      return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    const D3D11_DEPTH_STENCIL_VIEW_DESC *desc_ptr = nullptr;
    if (command.desc_present) {
      desc = depth_stencil_view_desc_from_command(command.desc);
      desc_ptr = &desc;
    }

    ID3D11DepthStencilView *view = nullptr;
    const HRESULT hr = device->CreateDepthStencilView(resource->resource, desc_ptr, &view);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateDepthStencilView", hr);
      return false;
    }

    own(view);
    depth_stencil_views_[command.view_id] = view;
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
    store_buffer_resource(
        command.buffer_id,
        buffer,
        command.byte_width,
        static_cast<D3D11_USAGE>(command.usage),
        command.bind_flags,
        command.cpu_access_flags);
    return true;
  }

  bool execute_command(
      const replay::internal::CreateBlendStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    const D3D11_BLEND_DESC desc = blend_desc_from_command(command.desc);
    ID3D11BlendState *state = nullptr;
    const HRESULT hr = device->CreateBlendState(&desc, &state);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateBlendState", hr);
      return false;
    }

    own(state);
    blend_states_[command.blend_state_id] = state;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateDepthStencilStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    const D3D11_DEPTH_STENCIL_DESC desc = depth_stencil_state_desc_from_command(command.desc);
    ID3D11DepthStencilState *state = nullptr;
    const HRESULT hr = device->CreateDepthStencilState(&desc, &state);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateDepthStencilState", hr);
      return false;
    }

    own(state);
    depth_stencil_states_[command.depth_stencil_state_id] = state;
    return true;
  }

  bool execute_command(
      const replay::internal::CreateRasterizerStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11Device *device = nullptr;
    if (!lookup_object(devices_, command.device_id, "device", device, error)) {
      return false;
    }

    const D3D11_RASTERIZER_DESC desc = rasterizer_desc_from_command(command.desc);
    ID3D11RasterizerState *state = nullptr;
    const HRESULT hr = device->CreateRasterizerState(&desc, &state);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11Device::CreateRasterizerState", hr);
      return false;
    }

    own(state);
    rasterizer_states_[command.rasterizer_state_id] = state;
    return true;
  }

  bool execute_command(const replay::internal::MapCommand &command, replay::ReplayStatistics &statistics, std::string &error)
  {
    (void)statistics;
    ReplayResourceState *resource = nullptr;
    if (!lookup_resource_state(command.resource_id, "resource", resource, error)) {
      return false;
    }
    resource->pending_map = PendingMapState{
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
    ReplayResourceState *resource = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_resource_state(command.resource_id, "resource", resource, error)) {
      return false;
    }
    if (!resource->pending_map.has_value()) {
      error = "Unmap without a prior Map is unsupported";
      return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto pending = *resource->pending_map;
    const HRESULT hr = context->Map(resource->resource, pending.subresource, pending.map_type, pending.map_flags, &mapped);
    if (FAILED(hr)) {
      error = format_hresult("ID3D11DeviceContext::Map", hr);
      return false;
    }

    if (pending.map_type != D3D11_MAP_READ) {
      std::vector<std::uint8_t> snapshot_bytes;
      if (!read_file_bytes(command.snapshot_path, snapshot_bytes, error)) {
        context->Unmap(resource->resource, pending.subresource);
        return false;
      }

      std::size_t copy_size = snapshot_bytes.size();
      if (resource->resource_class == replay::internal::ReplayResourceClass::Buffer) {
        copy_size = std::min<std::size_t>(copy_size, resource->byte_width);
      } else if (resource->resource_class == replay::internal::ReplayResourceClass::Texture2D) {
        copy_size = std::min<std::size_t>(
            copy_size,
            static_cast<std::size_t>(mapped.RowPitch) *
                static_cast<std::size_t>(texture_subresource_height(*resource, pending.subresource)));
      }
      if (copy_size != 0) {
        std::memcpy(mapped.pData, snapshot_bytes.data(), copy_size);
      }
    }

    context->Unmap(resource->resource, pending.subresource);
    resource->pending_map.reset();
    return true;
  }

  bool execute_command(
      const replay::internal::UpdateSubresourceCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ReplayResourceState *resource = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_resource_state(command.resource_id, "resource", resource, error)) {
      return false;
    }

    std::vector<std::uint8_t> data_bytes;
    if (!read_file_bytes(command.data_path, data_bytes, error)) {
      return false;
    }

    D3D11_BOX dst_box{};
    const D3D11_BOX *dst_box_ptr = nullptr;
    if (command.has_dst_box) {
      dst_box.left = command.dst_box.left;
      dst_box.top = command.dst_box.top;
      dst_box.front = command.dst_box.front;
      dst_box.right = command.dst_box.right;
      dst_box.bottom = command.dst_box.bottom;
      dst_box.back = command.dst_box.back;
      dst_box_ptr = &dst_box;
    }

    UINT row_pitch = command.src_row_pitch;
    if (resource->resource_class == replay::internal::ReplayResourceClass::Texture2D && row_pitch == 0 && !data_bytes.empty()) {
      const UINT height = dst_box_ptr ? (dst_box.bottom - dst_box.top) : texture_subresource_height(*resource, command.dst_subresource);
      if (height != 0) {
        row_pitch = static_cast<UINT>(data_bytes.size() / height);
      }
      if (row_pitch == 0) {
        const UINT bytes_per_pixel = texture_bytes_per_pixel(resource->format);
        row_pitch = texture_subresource_width(*resource, command.dst_subresource) * bytes_per_pixel;
      }
    }
    UINT depth_pitch = command.src_depth_pitch;
    if (depth_pitch == 0) {
      depth_pitch = static_cast<UINT>(data_bytes.size());
    }

    context->UpdateSubresource(
        resource->resource,
        command.dst_subresource,
        dst_box_ptr,
        data_bytes.empty() ? nullptr : data_bytes.data(),
        row_pitch,
        depth_pitch);
    return true;
  }

  bool execute_command(
      const replay::internal::ClearStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }
    context->ClearState();
    return true;
  }

  bool execute_command(
      const replay::internal::SetRenderTargetsCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
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

    ID3D11DepthStencilView *depth_stencil_view = nullptr;
    if (command.has_depth_stencil &&
        !lookup_object(depth_stencil_views_, command.depth_stencil_view_id, "depth-stencil-view", depth_stencil_view, error)) {
      return false;
    }

    context->OMSetRenderTargets(
        static_cast<UINT>(views.size()),
        views.empty() ? nullptr : views.data(),
        depth_stencil_view);
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
      const replay::internal::ClearDepthStencilViewCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ID3D11DepthStencilView *view = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_object(depth_stencil_views_, command.depth_stencil_view_id, "depth-stencil-view", view, error)) {
      return false;
    }
    context->ClearDepthStencilView(view, command.clear_flags, command.depth, command.stencil);
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
        ReplayResourceState *resource = nullptr;
        if (!lookup_resource_state(binding.buffer_id, "vertex-buffer", resource, error) || !resource->buffer) {
          error = "missing replay vertex buffer";
          return false;
        }
        buffers.push_back(resource->buffer);
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
      const replay::internal::SetIndexBufferCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ReplayResourceState *resource = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_resource_state(command.buffer_id, "index-buffer", resource, error) || !resource->buffer) {
      error = error.empty() ? "missing replay index buffer" : error;
      return false;
    }

    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(command.format);
    context->IASetIndexBuffer(resource->buffer, format, command.offset);
    bound_index_buffer_ = BoundIndexBufferState{command.buffer_id, format, command.offset};
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
      ReplayResourceState *resource = nullptr;
      if (!lookup_resource_state(buffer_id, "constant-buffer", resource, error) || !resource->buffer) {
        error = "missing replay constant buffer";
        return false;
      }
      buffers.push_back(resource->buffer);
    }

    if (command.vertex_stage) {
      context->VSSetConstantBuffers(command.start_slot, static_cast<UINT>(buffers.size()), buffers.data());
    } else {
      context->PSSetConstantBuffers(command.start_slot, static_cast<UINT>(buffers.size()), buffers.data());
    }
    return true;
  }

  bool execute_command(
      const replay::internal::SetShaderResourcesCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<ID3D11ShaderResourceView *> views;
    views.reserve(command.shader_resource_view_ids.size());
    for (const auto view_id : command.shader_resource_view_ids) {
      if (view_id == 0) {
        views.push_back(nullptr);
        continue;
      }
      ID3D11ShaderResourceView *view = nullptr;
      if (!lookup_object(shader_resource_views_, view_id, "shader-resource-view", view, error)) {
        return false;
      }
      views.push_back(view);
    }

    context->PSSetShaderResources(command.start_slot, static_cast<UINT>(views.size()), views.data());
    for (std::size_t index = 0; index < views.size(); ++index) {
      const auto slot = static_cast<std::size_t>(command.start_slot) + index;
      if (slot < ps_shader_resources_.size()) {
        ps_shader_resources_[slot] = views[index];
      }
    }
    return true;
  }

  bool execute_command(
      const replay::internal::SetSamplersCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<ID3D11SamplerState *> samplers;
    samplers.reserve(command.sampler_ids.size());
    for (const auto sampler_id : command.sampler_ids) {
      if (sampler_id == 0) {
        samplers.push_back(nullptr);
        continue;
      }
      ID3D11SamplerState *sampler = nullptr;
      if (!lookup_object(sampler_states_, sampler_id, "sampler-state", sampler, error)) {
        return false;
      }
      samplers.push_back(sampler);
    }

    context->PSSetSamplers(command.start_slot, static_cast<UINT>(samplers.size()), samplers.data());
    for (std::size_t index = 0; index < samplers.size(); ++index) {
      const auto slot = static_cast<std::size_t>(command.start_slot) + index;
      if (slot < ps_samplers_.size()) {
        ps_samplers_[slot] = samplers[index];
      }
    }
    return true;
  }

  bool execute_command(
      const replay::internal::SetDepthStencilStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }
    ID3D11DepthStencilState *state = nullptr;
    if (command.depth_stencil_state_id != 0 &&
        !lookup_object(depth_stencil_states_, command.depth_stencil_state_id, "depth-stencil-state", state, error)) {
      return false;
    }
    context->OMSetDepthStencilState(state, command.stencil_ref);
    bound_depth_stencil_state_ = state;
    bound_stencil_ref_ = command.stencil_ref;
    return true;
  }

  bool execute_command(
      const replay::internal::SetBlendStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }
    ID3D11BlendState *state = nullptr;
    if (command.blend_state_id != 0 &&
        !lookup_object(blend_states_, command.blend_state_id, "blend-state", state, error)) {
      return false;
    }
    context->OMSetBlendState(state, command.blend_factor.data(), command.sample_mask);
    bound_blend_state_ = state;
    bound_blend_factor_ = command.blend_factor;
    bound_sample_mask_ = command.sample_mask;
    return true;
  }

  bool execute_command(
      const replay::internal::SetRasterizerStateCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }
    ID3D11RasterizerState *state = nullptr;
    if (command.rasterizer_state_id != 0 &&
        !lookup_object(rasterizer_states_, command.rasterizer_state_id, "rasterizer-state", state, error)) {
      return false;
    }
    context->RSSetState(state);
    bound_rasterizer_state_ = state;
    return true;
  }

  bool execute_command(
      const replay::internal::SetScissorRectsCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }

    std::vector<D3D11_RECT> rects;
    rects.reserve(command.rects.size());
    for (const auto &source : command.rects) {
      rects.push_back(D3D11_RECT{source.left, source.top, source.right, source.bottom});
    }
    context->RSSetScissorRects(static_cast<UINT>(rects.size()), rects.data());
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
      const replay::internal::DrawIndexedCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }
    context->DrawIndexed(command.index_count, command.start_index_location, command.base_vertex_location);
    return true;
  }

  bool execute_command(
      const replay::internal::DrawIndexedInstancedCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error)) {
      return false;
    }
    context->DrawIndexedInstanced(
        command.index_count_per_instance,
        command.instance_count,
        command.start_index_location,
        command.base_vertex_location,
        command.start_instance_location);
    return true;
  }

  bool execute_command(
      const replay::internal::CopyResourceCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ReplayResourceState *dst = nullptr;
    ReplayResourceState *src = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_resource_state(command.dst_resource_id, "copy-dst", dst, error) ||
        !lookup_resource_state(command.src_resource_id, "copy-src", src, error)) {
      return false;
    }
    context->CopyResource(dst->resource, src->resource);
    return true;
  }

  bool execute_command(
      const replay::internal::ResolveSubresourceCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)statistics;
    ID3D11DeviceContext *context = nullptr;
    ReplayResourceState *dst = nullptr;
    ReplayResourceState *src = nullptr;
    if (!lookup_object(contexts_, command.context_id, "context", context, error) ||
        !lookup_resource_state(command.dst_resource_id, "resolve-dst", dst, error) ||
        !lookup_resource_state(command.src_resource_id, "resolve-src", src, error)) {
      return false;
    }
    context->ResolveSubresource(
        dst->resource,
        command.dst_subresource,
        src->resource,
        command.src_subresource,
        static_cast<DXGI_FORMAT>(command.format));
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
    if (command.frame_index != next_present_call_frame_index_) {
      error = "IDXGISwapChain::Present frame_index is not contiguous";
      return false;
    }
    if (present_calls_.find(command.frame_index) != present_calls_.end()) {
      error = "duplicate IDXGISwapChain::Present frame_index";
      return false;
    }

    if (!capture_present_frame(swapchain, command.frame_index, command.sync_interval, command.flags, error)) {
      return false;
    }

    const HRESULT hr = swapchain->Present(command.sync_interval, command.flags);
    if (FAILED(hr)) {
      error = format_hresult("IDXGISwapChain::Present", hr);
      return false;
    }

    present_calls_.emplace(command.frame_index, std::make_pair(command.sync_interval, command.flags));
    ++next_present_call_frame_index_;
    pump_messages();
    return true;
  }

  bool execute_command(
      const replay::internal::FrameBoundaryCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    if (command.label == "FrameBegin") {
      if (command.frame_index != next_frame_begin_index_) {
        error = "FrameBegin frame_index is not contiguous";
        return false;
      }
      if (open_frames_.find(command.frame_index) != open_frames_.end()) {
        error = "duplicate FrameBegin for frame_index";
        return false;
      }
      open_frames_.emplace(command.frame_index, false);
      ++next_frame_begin_index_;
      ++statistics.frames_seen;
      return true;
    }
    if (command.label == "FrameEnd") {
      if (command.frame_index != next_frame_end_index_) {
        error = "FrameEnd frame_index is not contiguous";
        return false;
      }
      const auto frame = open_frames_.find(command.frame_index);
      if (frame == open_frames_.end()) {
        error = "FrameEnd is missing matching FrameBegin";
        return false;
      }
      if (!frame->second) {
        error = "FrameEnd is missing matching Present boundary";
        return false;
      }
      open_frames_.erase(frame);
      ++next_frame_end_index_;
      return true;
    }
    error = "unsupported Frame boundary label";
    return false;
  }

  bool execute_command(
      const replay::internal::PresentBoundaryCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    if (command.frame_index != next_present_boundary_frame_index_) {
      error = "Present boundary frame_index is not contiguous";
      return false;
    }
    const auto present = present_calls_.find(command.frame_index);
    if (present == present_calls_.end()) {
      error = "Present boundary is missing matching IDXGISwapChain::Present call";
      return false;
    }
    const auto frame = open_frames_.find(command.frame_index);
    if (frame == open_frames_.end()) {
      error = "Present boundary is missing matching FrameBegin";
      return false;
    }
    if (frame->second) {
      error = "duplicate Present boundary for frame_index";
      return false;
    }
    if (present->second.first != command.sync_interval || present->second.second != command.flags) {
      error = "Present boundary does not match replayed IDXGISwapChain::Present parameters";
      return false;
    }
    frame->second = true;
    present_calls_.erase(present);
    ++next_present_boundary_frame_index_;
    ++statistics.presents_seen;
    return true;
  }

  bool execute_command(
      const replay::internal::DebugMarkerCommand &command,
      replay::ReplayStatistics &statistics,
      std::string &error)
  {
    (void)command;
    (void)statistics;
    (void)error;
    return true;
  }

  HWND window_ = nullptr;
#ifdef _WIN32
  bool window_class_registered_ = false;
#endif
  bool show_window_ = false;
#if defined(__APPLE__)
  apitrace::platform::macos::WindowHandles window_handles_;
#endif
  std::vector<IUnknown *> owned_objects_;
  std::unordered_map<trace::ObjectId, ID3D11Device *> devices_;
  std::unordered_map<trace::ObjectId, ID3D11DeviceContext *> contexts_;
  std::unordered_map<trace::ObjectId, IDXGISwapChain *> swapchains_;
  std::unordered_map<std::uint64_t, std::pair<UINT, UINT>> present_calls_;
  std::unordered_map<std::uint64_t, bool> open_frames_;
  std::unordered_map<trace::ObjectId, ReplayResourceState> resource_states_;
  std::unordered_map<trace::ObjectId, ID3D11RenderTargetView *> render_target_views_;
  std::unordered_map<trace::ObjectId, ID3D11ShaderResourceView *> shader_resource_views_;
  std::unordered_map<trace::ObjectId, ID3D11DepthStencilView *> depth_stencil_views_;
  std::unordered_map<trace::ObjectId, ID3D11InputLayout *> input_layouts_;
  std::unordered_map<trace::ObjectId, ID3D11VertexShader *> vertex_shaders_;
  std::unordered_map<trace::ObjectId, ID3D11PixelShader *> pixel_shaders_;
  std::unordered_map<trace::ObjectId, ID3D11SamplerState *> sampler_states_;
  std::unordered_map<trace::ObjectId, ID3D11BlendState *> blend_states_;
  std::unordered_map<trace::ObjectId, ID3D11DepthStencilState *> depth_stencil_states_;
  std::unordered_map<trace::ObjectId, ID3D11RasterizerState *> rasterizer_states_;
  std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> ps_shader_resources_{};
  std::array<ID3D11SamplerState *, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> ps_samplers_{};
  std::optional<BoundIndexBufferState> bound_index_buffer_;
  std::uint64_t next_frame_begin_index_ = 0;
  std::uint64_t next_frame_end_index_ = 0;
  std::uint64_t next_present_call_frame_index_ = 0;
  std::uint64_t next_present_boundary_frame_index_ = 0;
  ID3D11DepthStencilState *bound_depth_stencil_state_ = nullptr;
  UINT bound_stencil_ref_ = 0;
  ID3D11BlendState *bound_blend_state_ = nullptr;
  std::array<float, 4> bound_blend_factor_ = {0.0f, 0.0f, 0.0f, 0.0f};
  UINT bound_sample_mask_ = 0;
  ID3D11RasterizerState *bound_rasterizer_state_ = nullptr;
#if !defined(_WIN32)
  std::unique_ptr<trace::TraceBundleWriter> capture_writer_;
  std::uint64_t capture_sequence_ = 0;
#endif
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
