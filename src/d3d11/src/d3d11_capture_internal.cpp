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
constexpr std::size_t kDeviceCreateTexture2DIndex = 5;
constexpr std::size_t kDeviceCreateShaderResourceViewIndex = 7;
constexpr std::size_t kDeviceCreateRenderTargetViewIndex = 9;
constexpr std::size_t kDeviceCreateDepthStencilViewIndex = 10;
constexpr std::size_t kDeviceCreateInputLayoutIndex = 11;
constexpr std::size_t kDeviceCreateVertexShaderIndex = 12;
constexpr std::size_t kDeviceCreatePixelShaderIndex = 15;
constexpr std::size_t kDeviceCreateBlendStateIndex = 20;
constexpr std::size_t kDeviceCreateDepthStencilStateIndex = 21;
constexpr std::size_t kDeviceCreateRasterizerStateIndex = 22;
constexpr std::size_t kDeviceCreateSamplerStateIndex = 23;
constexpr std::size_t kDeviceGetImmediateContextIndex = 40;

constexpr std::size_t kContextVSSetConstantBuffersIndex = 7;
constexpr std::size_t kContextPSSetShaderResourcesIndex = 8;
constexpr std::size_t kContextPSSetShaderIndex = 9;
constexpr std::size_t kContextPSSetSamplersIndex = 10;
constexpr std::size_t kContextVSSetShaderIndex = 11;
constexpr std::size_t kContextDrawIndexedIndex = 12;
constexpr std::size_t kContextDrawIndex = 13;
constexpr std::size_t kContextMapIndex = 14;
constexpr std::size_t kContextUnmapIndex = 15;
constexpr std::size_t kContextPSSetConstantBuffersIndex = 16;
constexpr std::size_t kContextIASetInputLayoutIndex = 17;
constexpr std::size_t kContextIASetVertexBuffersIndex = 18;
constexpr std::size_t kContextIASetIndexBufferIndex = 19;
constexpr std::size_t kContextDrawIndexedInstancedIndex = 20;
constexpr std::size_t kContextIASetPrimitiveTopologyIndex = 24;
constexpr std::size_t kContextOMSetRenderTargetsIndex = 33;
constexpr std::size_t kContextOMSetBlendStateIndex = 35;
constexpr std::size_t kContextOMSetDepthStencilStateIndex = 36;
constexpr std::size_t kContextRSSetStateIndex = 43;
constexpr std::size_t kContextRSSetViewportsIndex = 44;
constexpr std::size_t kContextRSSetScissorRectsIndex = 45;
constexpr std::size_t kContextCopyResourceIndex = 47;
constexpr std::size_t kContextUpdateSubresourceIndex = 48;
constexpr std::size_t kContextClearRenderTargetViewIndex = 50;
constexpr std::size_t kContextClearDepthStencilViewIndex = 53;

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
using DeviceCreateTexture2DFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_TEXTURE2D_DESC *,
    const D3D11_SUBRESOURCE_DATA *,
    ID3D11Texture2D **);
using DeviceCreateShaderResourceViewFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    ID3D11Resource *,
    const D3D11_SHADER_RESOURCE_VIEW_DESC *,
    ID3D11ShaderResourceView **);
using DeviceCreateRenderTargetViewFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    ID3D11Resource *,
    const D3D11_RENDER_TARGET_VIEW_DESC *,
    ID3D11RenderTargetView **);
using DeviceCreateDepthStencilViewFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    ID3D11Resource *,
    const D3D11_DEPTH_STENCIL_VIEW_DESC *,
    ID3D11DepthStencilView **);
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
using DeviceCreateBlendStateFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_BLEND_DESC *,
    ID3D11BlendState **);
using DeviceCreateDepthStencilStateFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_DEPTH_STENCIL_DESC *,
    ID3D11DepthStencilState **);
using DeviceCreateRasterizerStateFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_RASTERIZER_DESC *,
    ID3D11RasterizerState **);
using DeviceCreateSamplerStateFn = HRESULT(STDMETHODCALLTYPE *)(
    ID3D11Device *,
    const D3D11_SAMPLER_DESC *,
    ID3D11SamplerState **);
using DeviceGetImmediateContextFn = void(STDMETHODCALLTYPE *)(ID3D11Device *, ID3D11DeviceContext **);

using ContextVSSetConstantBuffersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using ContextPSSetShaderResourcesFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
using ContextPSSetShaderFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
using ContextPSSetSamplersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
using ContextVSSetShaderFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
using ContextDrawIndexedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using ContextDrawFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using ContextMapFn =
    HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
using ContextUnmapFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using ContextPSSetConstantBuffersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
using ContextIASetInputLayoutFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11InputLayout *);
using ContextIASetVertexBuffersFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *, const UINT *, const UINT *);
using ContextIASetIndexBufferFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Buffer *, DXGI_FORMAT, UINT);
using ContextDrawIndexedInstancedFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
using ContextIASetPrimitiveTopologyFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, D3D11_PRIMITIVE_TOPOLOGY);
using ContextOMSetRenderTargetsFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
using ContextOMSetBlendStateFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11BlendState *, const FLOAT[4], UINT);
using ContextOMSetDepthStencilStateFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
using ContextRSSetStateFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11RasterizerState *);
using ContextRSSetViewportsFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
using ContextRSSetScissorRectsFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, const D3D11_RECT *);
using ContextCopyResourceFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
using ContextUpdateSubresourceFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
using ContextClearRenderTargetViewFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
using ContextClearDepthStencilViewFn =
    void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);

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
  DeviceCreateTexture2DFn create_texture2d = nullptr;
  DeviceCreateShaderResourceViewFn create_shader_resource_view = nullptr;
  DeviceCreateRenderTargetViewFn create_render_target_view = nullptr;
  DeviceCreateDepthStencilViewFn create_depth_stencil_view = nullptr;
  DeviceCreateInputLayoutFn create_input_layout = nullptr;
  DeviceCreateVertexShaderFn create_vertex_shader = nullptr;
  DeviceCreatePixelShaderFn create_pixel_shader = nullptr;
  DeviceCreateBlendStateFn create_blend_state = nullptr;
  DeviceCreateDepthStencilStateFn create_depth_stencil_state = nullptr;
  DeviceCreateRasterizerStateFn create_rasterizer_state = nullptr;
  DeviceCreateSamplerStateFn create_sampler_state = nullptr;
  DeviceGetImmediateContextFn get_immediate_context = nullptr;
};

struct ContextHookState {
  void **vtable = nullptr;
  ContextVSSetConstantBuffersFn vs_set_constant_buffers = nullptr;
  ContextPSSetShaderResourcesFn ps_set_shader_resources = nullptr;
  ContextPSSetShaderFn ps_set_shader = nullptr;
  ContextPSSetSamplersFn ps_set_samplers = nullptr;
  ContextVSSetShaderFn vs_set_shader = nullptr;
  ContextDrawIndexedFn draw_indexed = nullptr;
  ContextDrawFn draw = nullptr;
  ContextMapFn map = nullptr;
  ContextUnmapFn unmap = nullptr;
  ContextPSSetConstantBuffersFn ps_set_constant_buffers = nullptr;
  ContextIASetInputLayoutFn ia_set_input_layout = nullptr;
  ContextIASetVertexBuffersFn ia_set_vertex_buffers = nullptr;
  ContextIASetIndexBufferFn ia_set_index_buffer = nullptr;
  ContextDrawIndexedInstancedFn draw_indexed_instanced = nullptr;
  ContextIASetPrimitiveTopologyFn ia_set_primitive_topology = nullptr;
  ContextOMSetRenderTargetsFn om_set_render_targets = nullptr;
  ContextOMSetBlendStateFn om_set_blend_state = nullptr;
  ContextOMSetDepthStencilStateFn om_set_depth_stencil_state = nullptr;
  ContextRSSetStateFn rs_set_state = nullptr;
  ContextRSSetViewportsFn rs_set_viewports = nullptr;
  ContextRSSetScissorRectsFn rs_set_scissor_rects = nullptr;
  ContextCopyResourceFn copy_resource = nullptr;
  ContextUpdateSubresourceFn update_subresource = nullptr;
  ContextClearRenderTargetViewFn clear_render_target_view = nullptr;
  ContextClearDepthStencilViewFn clear_depth_stencil_view = nullptr;
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

enum class ResourceClass {
  Buffer,
  Texture2D,
};

struct ResourceInfo {
  trace::ObjectId object_id = 0;
  ResourceClass resource_class = ResourceClass::Buffer;
  trace::ObjectId parent_object_id = 0;
  UINT byte_width = 0;
  UINT width = 0;
  UINT height = 0;
  UINT mip_levels = 0;
  UINT array_size = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT sample_count = 0;
  UINT sample_quality = 0;
  UINT bind_flags = 0;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
  UINT cpu_access_flags = 0;
  UINT misc_flags = 0;
  UINT structure_byte_stride = 0;
  const void *mapped_ptr = nullptr;
  UINT mapped_subresource = 0;
  D3D11_MAP mapped_type = D3D11_MAP_READ;
  UINT mapped_row_pitch = 0;
  UINT mapped_depth_pitch = 0;
};

struct CaptureState {
  std::recursive_mutex mutex;
  trace::ObjectId next_object_id = 1000;
  trace::BlobId next_blob_id = 5000;
  std::uint64_t next_sequence = 1;
  std::uint64_t frame_index = 0;
  bool frame_begin_pending = true;
  std::unordered_map<const void *, ObjectInfo> objects;
  std::unordered_map<const void *, ResourceInfo> resources;
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

std::string resource_class_name(ResourceClass resource_class)
{
  switch (resource_class) {
  case ResourceClass::Buffer:
    return "buffer";
  case ResourceClass::Texture2D:
    return "texture2d";
  default:
    return "unknown";
  }
}

std::uint32_t format_bytes_per_pixel(DXGI_FORMAT format)
{
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_D32_FLOAT:
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

UINT subresource_mip_level(const ResourceInfo &resource, UINT subresource)
{
  if (resource.mip_levels == 0) {
    return 0;
  }
  return subresource % resource.mip_levels;
}

UINT texture_subresource_width(const ResourceInfo &resource, UINT subresource)
{
  return mip_extent(resource.width, subresource_mip_level(resource, subresource));
}

UINT texture_subresource_height(const ResourceInfo &resource, UINT subresource)
{
  return mip_extent(resource.height, subresource_mip_level(resource, subresource));
}

std::size_t mapped_texture_snapshot_size(const ResourceInfo &resource)
{
  if (resource.resource_class != ResourceClass::Texture2D || resource.mapped_row_pitch == 0) {
    return 0;
  }
  return static_cast<std::size_t>(resource.mapped_row_pitch) *
         static_cast<std::size_t>(texture_subresource_height(resource, resource.mapped_subresource));
}

std::size_t texture_upload_size(
    const ResourceInfo &resource,
    UINT subresource,
    const D3D11_BOX *dst_box,
    UINT src_row_pitch,
    UINT src_depth_pitch)
{
  if (resource.resource_class != ResourceClass::Texture2D) {
    return 0;
  }

  UINT height = texture_subresource_height(resource, subresource);
  if (dst_box) {
    height = dst_box->bottom > dst_box->top ? (dst_box->bottom - dst_box->top) : 0U;
  }
  if (height == 0) {
    return 0;
  }

  if (src_depth_pitch != 0) {
    return src_depth_pitch;
  }
  if (src_row_pitch != 0) {
    return static_cast<std::size_t>(src_row_pitch) * static_cast<std::size_t>(height);
  }

  const auto bytes_per_pixel = format_bytes_per_pixel(resource.format);
  if (bytes_per_pixel == 0) {
    return 0;
  }

  const UINT width = dst_box && dst_box->right > dst_box->left ? (dst_box->right - dst_box->left)
                                                               : texture_subresource_width(resource, subresource);
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytes_per_pixel;
}

std::string texture2d_desc_json(const D3D11_TEXTURE2D_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"width\":" << desc.Width << ","
          << "\"height\":" << desc.Height << ","
          << "\"mip_levels\":" << desc.MipLevels << ","
          << "\"array_size\":" << desc.ArraySize << ","
          << "\"format\":" << static_cast<unsigned int>(desc.Format) << ","
          << "\"sample_count\":" << desc.SampleDesc.Count << ","
          << "\"sample_quality\":" << desc.SampleDesc.Quality << ","
          << "\"usage\":" << static_cast<unsigned int>(desc.Usage) << ","
          << "\"bind_flags\":" << desc.BindFlags << ","
          << "\"cpu_access_flags\":" << desc.CPUAccessFlags << ","
          << "\"misc_flags\":" << desc.MiscFlags
          << "}";
  return payload.str();
}

std::string sampler_desc_json(const D3D11_SAMPLER_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"filter\":" << static_cast<unsigned int>(desc.Filter) << ","
          << "\"address_u\":" << static_cast<unsigned int>(desc.AddressU) << ","
          << "\"address_v\":" << static_cast<unsigned int>(desc.AddressV) << ","
          << "\"address_w\":" << static_cast<unsigned int>(desc.AddressW) << ","
          << "\"mip_lod_bias\":" << desc.MipLODBias << ","
          << "\"max_anisotropy\":" << desc.MaxAnisotropy << ","
          << "\"comparison_func\":" << static_cast<unsigned int>(desc.ComparisonFunc) << ","
          << "\"border_color\":[" << desc.BorderColor[0] << "," << desc.BorderColor[1] << "," << desc.BorderColor[2] << ","
          << desc.BorderColor[3] << "],"
          << "\"min_lod\":" << desc.MinLOD << ","
          << "\"max_lod\":" << desc.MaxLOD
          << "}";
  return payload.str();
}

std::string shader_resource_view_desc_json(const D3D11_SHADER_RESOURCE_VIEW_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"format\":" << static_cast<unsigned int>(desc.Format) << ","
          << "\"view_dimension\":" << static_cast<unsigned int>(desc.ViewDimension);
  if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
    payload << ",\"texture2d\":{\"most_detailed_mip\":" << desc.Texture2D.MostDetailedMip
            << ",\"mip_levels\":" << desc.Texture2D.MipLevels << "}";
  }
  payload << "}";
  return payload.str();
}

std::string render_target_view_desc_json(const D3D11_RENDER_TARGET_VIEW_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"format\":" << static_cast<unsigned int>(desc.Format) << ","
          << "\"view_dimension\":" << static_cast<unsigned int>(desc.ViewDimension);
  if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
    payload << ",\"texture2d\":{\"mip_slice\":" << desc.Texture2D.MipSlice << "}";
  }
  payload << "}";
  return payload.str();
}

std::string depth_stencil_view_desc_json(const D3D11_DEPTH_STENCIL_VIEW_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"format\":" << static_cast<unsigned int>(desc.Format) << ","
          << "\"view_dimension\":" << static_cast<unsigned int>(desc.ViewDimension) << ","
          << "\"flags\":" << desc.Flags;
  if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) {
    payload << ",\"texture2d\":{\"mip_slice\":" << desc.Texture2D.MipSlice << "}";
  }
  payload << "}";
  return payload.str();
}

std::string blend_desc_json(const D3D11_BLEND_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"alpha_to_coverage_enable\":" << bool_json(desc.AlphaToCoverageEnable == TRUE) << ","
          << "\"independent_blend_enable\":" << bool_json(desc.IndependentBlendEnable == TRUE) << ","
          << "\"render_targets\":[";
  for (UINT index = 0; index < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &target = desc.RenderTarget[index];
    payload << "{"
            << "\"blend_enable\":" << bool_json(target.BlendEnable == TRUE) << ","
            << "\"src_blend\":" << static_cast<unsigned int>(target.SrcBlend) << ","
            << "\"dest_blend\":" << static_cast<unsigned int>(target.DestBlend) << ","
            << "\"blend_op\":" << static_cast<unsigned int>(target.BlendOp) << ","
            << "\"src_blend_alpha\":" << static_cast<unsigned int>(target.SrcBlendAlpha) << ","
            << "\"dest_blend_alpha\":" << static_cast<unsigned int>(target.DestBlendAlpha) << ","
            << "\"blend_op_alpha\":" << static_cast<unsigned int>(target.BlendOpAlpha) << ","
            << "\"write_mask\":" << static_cast<unsigned int>(target.RenderTargetWriteMask)
            << "}";
  }
  payload << "]}";
  return payload.str();
}

std::string depth_stencil_state_desc_json(const D3D11_DEPTH_STENCIL_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"depth_enable\":" << bool_json(desc.DepthEnable == TRUE) << ","
          << "\"depth_write_mask\":" << static_cast<unsigned int>(desc.DepthWriteMask) << ","
          << "\"depth_func\":" << static_cast<unsigned int>(desc.DepthFunc) << ","
          << "\"stencil_enable\":" << bool_json(desc.StencilEnable == TRUE) << ","
          << "\"stencil_read_mask\":" << static_cast<unsigned int>(desc.StencilReadMask) << ","
          << "\"stencil_write_mask\":" << static_cast<unsigned int>(desc.StencilWriteMask)
          << "}";
  return payload.str();
}

std::string rasterizer_desc_json(const D3D11_RASTERIZER_DESC &desc)
{
  std::ostringstream payload;
  payload << "{"
          << "\"fill_mode\":" << static_cast<unsigned int>(desc.FillMode) << ","
          << "\"cull_mode\":" << static_cast<unsigned int>(desc.CullMode) << ","
          << "\"front_counter_clockwise\":" << bool_json(desc.FrontCounterClockwise == TRUE) << ","
          << "\"depth_bias\":" << desc.DepthBias << ","
          << "\"depth_bias_clamp\":" << desc.DepthBiasClamp << ","
          << "\"slope_scaled_depth_bias\":" << desc.SlopeScaledDepthBias << ","
          << "\"depth_clip_enable\":" << bool_json(desc.DepthClipEnable == TRUE) << ","
          << "\"scissor_enable\":" << bool_json(desc.ScissorEnable == TRUE) << ","
          << "\"multisample_enable\":" << bool_json(desc.MultisampleEnable == TRUE) << ","
          << "\"antialiased_line_enable\":" << bool_json(desc.AntialiasedLineEnable == TRUE)
          << "}";
  return payload.str();
}

ResourceInfo make_buffer_info(
    trace::ObjectId object_id,
    trace::ObjectId parent_object_id,
    const D3D11_BUFFER_DESC *desc)
{
  ResourceInfo info;
  info.object_id = object_id;
  info.resource_class = ResourceClass::Buffer;
  info.parent_object_id = parent_object_id;
  if (desc) {
    info.byte_width = desc->ByteWidth;
    info.bind_flags = desc->BindFlags;
    info.usage = desc->Usage;
    info.cpu_access_flags = desc->CPUAccessFlags;
    info.misc_flags = desc->MiscFlags;
    info.structure_byte_stride = desc->StructureByteStride;
  }
  return info;
}

ResourceInfo make_texture2d_info(
    trace::ObjectId object_id,
    trace::ObjectId parent_object_id,
    const D3D11_TEXTURE2D_DESC &desc)
{
  ResourceInfo info;
  info.object_id = object_id;
  info.resource_class = ResourceClass::Texture2D;
  info.parent_object_id = parent_object_id;
  info.width = desc.Width;
  info.height = desc.Height;
  info.mip_levels = desc.MipLevels;
  info.array_size = desc.ArraySize;
  info.format = desc.Format;
  info.sample_count = desc.SampleDesc.Count;
  info.sample_quality = desc.SampleDesc.Quality;
  info.bind_flags = desc.BindFlags;
  info.usage = desc.Usage;
  info.cpu_access_flags = desc.CPUAccessFlags;
  info.misc_flags = desc.MiscFlags;
  return info;
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

ResourceInfo *lookup_resource_info_locked(const void *resource)
{
  auto &state = capture_state();
  const auto it = state.resources.find(resource);
  if (it == state.resources.end()) {
    return nullptr;
  }
  return &it->second;
}

void record_boundary_locked(trace::BoundaryKind boundary, std::string payload_json)
{
  if (auto *session = runtime::ensure_process_trace_session(trace::ApiKind::D3D11)) {
    trace::EventRecord event;
    event.kind = trace::EventKind::Boundary;
    event.boundary = boundary;
    event.callsite.sequence = capture_state().next_sequence++;
    switch (boundary) {
    case trace::BoundaryKind::Present:
      event.callsite.function_name = "Present";
      break;
    case trace::BoundaryKind::DebugMarker:
      event.callsite.function_name = "DebugMarker";
      break;
    default:
      event.callsite.function_name = "Frame";
      break;
    }
    event.payload = std::move(payload_json);
    session->append_call_event(event);
  }
}

void record_scene_marker_locked(const char *scene_name, const char *dx_mode, const char *phase)
{
  std::ostringstream payload;
  payload << "{"
          << "\"scene_name\":\"" << json_escape(scene_name ? scene_name : "") << "\","
          << "\"dx_mode\":\"" << json_escape(dx_mode ? dx_mode : "") << "\","
          << "\"phase\":\"" << json_escape(phase ? phase : "") << "\""
          << "}";
  record_boundary_locked(trace::BoundaryKind::DebugMarker, payload.str());
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
    const auto parent_object_id = lookup_object_id_locked(device);
    const auto object_id = register_object_locked(*buffer, trace::ObjectKind::Resource, "ID3D11Buffer", parent_object_id);
    state.resources[*buffer] = make_buffer_info(object_id, parent_object_id, desc);

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

HRESULT STDMETHODCALLTYPE hook_create_texture2d(
    ID3D11Device *device,
    const D3D11_TEXTURE2D_DESC *desc,
    const D3D11_SUBRESOURCE_DATA *initial_data,
    ID3D11Texture2D **texture)
{
  proxy_debug_log("hook_create_texture2d");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_texture2d(device, desc, initial_data, texture);

  std::vector<trace::BlobId> blob_refs;
  std::string initial_path_json = "null";
  if (SUCCEEDED(hr) && texture && *texture && desc) {
    const auto object_id = register_object_locked(*texture, trace::ObjectKind::Resource, "ID3D11Texture2D", lookup_object_id_locked(device));
    state.resources[*texture] = make_texture2d_info(object_id, lookup_object_id_locked(device), *desc);

    if (initial_data && initial_data->pSysMem) {
      const auto upload_size = texture_upload_size(state.resources[*texture], 0, nullptr, initial_data->SysMemPitch, initial_data->SysMemSlicePitch);
      if (upload_size != 0) {
        const auto asset = register_asset_bytes(trace::AssetKind::Texture, "texture-initial", initial_data->pSysMem, upload_size);
        blob_refs.push_back(asset.blob_id);
        initial_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
      }
    }
  }

  std::ostringstream payload;
  payload << "{"
          << "\"resource_class\":\"texture2d\","
          << "\"desc\":" << (desc ? texture2d_desc_json(*desc) : "null") << ","
          << "\"has_initial_data\":" << bool_json(initial_data && initial_data->pSysMem != nullptr) << ","
          << "\"initial_data_path\":" << initial_path_json
          << "}";
  record_call_locked("ID3D11Device::CreateTexture2D", hr, {device, texture ? *texture : nullptr}, blob_refs, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_shader_resource_view(
    ID3D11Device *device,
    ID3D11Resource *resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
    ID3D11ShaderResourceView **shader_resource_view)
{
  proxy_debug_log("hook_create_shader_resource_view");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_shader_resource_view(device, resource, desc, shader_resource_view);
  if (SUCCEEDED(hr) && shader_resource_view && *shader_resource_view) {
    register_object_locked(
        *shader_resource_view,
        trace::ObjectKind::View,
        "ID3D11ShaderResourceView",
        lookup_object_id_locked(resource));
  }

  std::ostringstream payload;
  payload << "{"
          << "\"desc_present\":" << bool_json(desc != nullptr) << ","
          << "\"desc\":" << (desc ? shader_resource_view_desc_json(*desc) : "null")
          << "}";
  record_call_locked(
      "ID3D11Device::CreateShaderResourceView",
      hr,
      {device, resource, shader_resource_view ? *shader_resource_view : nullptr},
      {},
      payload.str());
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
  payload << "{"
          << "\"desc_present\":" << bool_json(desc != nullptr) << ","
          << "\"desc\":" << (desc ? render_target_view_desc_json(*desc) : "null")
          << "}";
  record_call_locked(
      "ID3D11Device::CreateRenderTargetView",
      hr,
      {device, resource, render_target_view ? *render_target_view : nullptr},
      {},
      payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_depth_stencil_view(
    ID3D11Device *device,
    ID3D11Resource *resource,
    const D3D11_DEPTH_STENCIL_VIEW_DESC *desc,
    ID3D11DepthStencilView **depth_stencil_view)
{
  proxy_debug_log("hook_create_depth_stencil_view");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_depth_stencil_view(device, resource, desc, depth_stencil_view);
  if (SUCCEEDED(hr) && depth_stencil_view && *depth_stencil_view) {
    register_object_locked(
        *depth_stencil_view,
        trace::ObjectKind::View,
        "ID3D11DepthStencilView",
        lookup_object_id_locked(resource));
  }

  std::ostringstream payload;
  payload << "{"
          << "\"desc_present\":" << bool_json(desc != nullptr) << ","
          << "\"desc\":" << (desc ? depth_stencil_view_desc_json(*desc) : "null")
          << "}";
  record_call_locked(
      "ID3D11Device::CreateDepthStencilView",
      hr,
      {device, resource, depth_stencil_view ? *depth_stencil_view : nullptr},
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

HRESULT STDMETHODCALLTYPE hook_create_blend_state(
    ID3D11Device *device,
    const D3D11_BLEND_DESC *desc,
    ID3D11BlendState **blend_state)
{
  proxy_debug_log("hook_create_blend_state");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_blend_state(device, desc, blend_state);
  if (SUCCEEDED(hr) && blend_state && *blend_state) {
    register_object_locked(*blend_state, trace::ObjectKind::PipelineState, "ID3D11BlendState", lookup_object_id_locked(device));
  }

  std::ostringstream payload;
  payload << "{\"desc\":" << (desc ? blend_desc_json(*desc) : "null") << "}";
  record_call_locked("ID3D11Device::CreateBlendState", hr, {device, blend_state ? *blend_state : nullptr}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_depth_stencil_state(
    ID3D11Device *device,
    const D3D11_DEPTH_STENCIL_DESC *desc,
    ID3D11DepthStencilState **depth_stencil_state)
{
  proxy_debug_log("hook_create_depth_stencil_state");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_depth_stencil_state(device, desc, depth_stencil_state);
  if (SUCCEEDED(hr) && depth_stencil_state && *depth_stencil_state) {
    register_object_locked(
        *depth_stencil_state,
        trace::ObjectKind::PipelineState,
        "ID3D11DepthStencilState",
        lookup_object_id_locked(device));
  }

  std::ostringstream payload;
  payload << "{\"desc\":" << (desc ? depth_stencil_state_desc_json(*desc) : "null") << "}";
  record_call_locked(
      "ID3D11Device::CreateDepthStencilState",
      hr,
      {device, depth_stencil_state ? *depth_stencil_state : nullptr},
      {},
      payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_rasterizer_state(
    ID3D11Device *device,
    const D3D11_RASTERIZER_DESC *desc,
    ID3D11RasterizerState **rasterizer_state)
{
  proxy_debug_log("hook_create_rasterizer_state");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_rasterizer_state(device, desc, rasterizer_state);
  if (SUCCEEDED(hr) && rasterizer_state && *rasterizer_state) {
    register_object_locked(
        *rasterizer_state,
        trace::ObjectKind::PipelineState,
        "ID3D11RasterizerState",
        lookup_object_id_locked(device));
  }

  std::ostringstream payload;
  payload << "{\"desc\":" << (desc ? rasterizer_desc_json(*desc) : "null") << "}";
  record_call_locked(
      "ID3D11Device::CreateRasterizerState",
      hr,
      {device, rasterizer_state ? *rasterizer_state : nullptr},
      {},
      payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_sampler_state(
    ID3D11Device *device,
    const D3D11_SAMPLER_DESC *desc,
    ID3D11SamplerState **sampler_state)
{
  proxy_debug_log("hook_create_sampler_state");
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = device_hook_locked(device);
  const HRESULT hr = hook.create_sampler_state(device, desc, sampler_state);
  if (SUCCEEDED(hr) && sampler_state && *sampler_state) {
    register_object_locked(*sampler_state, trace::ObjectKind::PipelineState, "ID3D11SamplerState", lookup_object_id_locked(device));
  }

  std::ostringstream payload;
  payload << "{\"desc\":" << (desc ? sampler_desc_json(*desc) : "null") << "}";
  record_call_locked("ID3D11Device::CreateSamplerState", hr, {device, sampler_state ? *sampler_state : nullptr}, {}, payload.str());
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

void STDMETHODCALLTYPE hook_ps_set_shader_resources(
    ID3D11DeviceContext *context,
    UINT start_slot,
    UINT num_views,
    ID3D11ShaderResourceView *const *shader_resource_views)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ps_set_shader_resources(context, start_slot, num_views, shader_resource_views);

  std::vector<const void *> objects = {context};
  for (UINT index = 0; index < num_views; ++index) {
    objects.push_back(shader_resource_views ? shader_resource_views[index] : nullptr);
  }

  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot << ",\"num_views\":" << num_views << "}";
  record_call_locked("ID3D11DeviceContext::PSSetShaderResources", S_OK, objects, {}, payload.str());
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

void STDMETHODCALLTYPE hook_ps_set_samplers(
    ID3D11DeviceContext *context,
    UINT start_slot,
    UINT num_samplers,
    ID3D11SamplerState *const *samplers)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ps_set_samplers(context, start_slot, num_samplers, samplers);

  std::vector<const void *> objects = {context};
  for (UINT index = 0; index < num_samplers; ++index) {
    objects.push_back(samplers ? samplers[index] : nullptr);
  }

  std::ostringstream payload;
  payload << "{\"start_slot\":" << start_slot << ",\"num_samplers\":" << num_samplers << "}";
  record_call_locked("ID3D11DeviceContext::PSSetSamplers", S_OK, objects, {}, payload.str());
}

void STDMETHODCALLTYPE hook_draw_indexed(
    ID3D11DeviceContext *context,
    UINT index_count,
    UINT start_index_location,
    INT base_vertex_location)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  ensure_frame_begin_locked();
  auto &hook = context_hook_locked(context);
  hook.draw_indexed(context, index_count, start_index_location, base_vertex_location);

  std::ostringstream payload;
  payload << "{\"index_count\":" << index_count << ",\"start_index_location\":" << start_index_location
          << ",\"base_vertex_location\":" << base_vertex_location << "}";
  record_call_locked("ID3D11DeviceContext::DrawIndexed", S_OK, {context}, {}, payload.str());
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
    if (auto *resource_info = lookup_resource_info_locked(resource)) {
      resource_info->mapped_ptr = mapped_resource->pData;
      resource_info->mapped_subresource = subresource;
      resource_info->mapped_type = map_type;
      resource_info->mapped_row_pitch = mapped_resource->RowPitch;
      resource_info->mapped_depth_pitch = mapped_resource->DepthPitch;
    }
  }

  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource << ",\"map_type\":\"" << map_type_name(map_type)
          << "\",\"map_flags\":" << map_flags;
  if (const auto *resource_info = lookup_resource_info_locked(resource)) {
    payload << ",\"resource_class\":\"" << resource_class_name(resource_info->resource_class) << "\""
            << ",\"mapped_row_pitch\":" << resource_info->mapped_row_pitch
            << ",\"mapped_depth_pitch\":" << resource_info->mapped_depth_pitch;
  }
  payload << "}";
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
  std::string resource_class_json = "\"unknown\"";
  if (auto *resource_info = lookup_resource_info_locked(resource)) {
    resource_class_json = "\"" + json_escape(resource_class_name(resource_info->resource_class)) + "\"";
    std::size_t snapshot_size = 0;
    trace::AssetKind asset_kind = trace::AssetKind::Unknown;
    const char *asset_name = nullptr;
    if (resource_info->resource_class == ResourceClass::Buffer && resource_info->mapped_ptr && resource_info->byte_width != 0) {
      snapshot_size = resource_info->byte_width;
      asset_kind = trace::AssetKind::Buffer;
      asset_name = "buffer-map-snapshot";
    } else if (resource_info->resource_class == ResourceClass::Texture2D && resource_info->mapped_ptr) {
      snapshot_size = mapped_texture_snapshot_size(*resource_info);
      asset_kind = trace::AssetKind::Texture;
      asset_name = "texture-map-snapshot";
    }

    if (snapshot_size != 0 && asset_kind != trace::AssetKind::Unknown && asset_name) {
      const auto asset = register_asset_bytes(asset_kind, asset_name, resource_info->mapped_ptr, snapshot_size);
      blob_refs.push_back(asset.blob_id);
      snapshot_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
    }

    resource_info->mapped_ptr = nullptr;
    resource_info->mapped_row_pitch = 0;
    resource_info->mapped_depth_pitch = 0;
  }

  auto &hook = context_hook_locked(context);
  hook.unmap(context, resource, subresource);

  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource << ",\"resource_class\":" << resource_class_json
          << ",\"snapshot_path\":" << snapshot_path_json << "}";
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

void STDMETHODCALLTYPE hook_ia_set_index_buffer(
    ID3D11DeviceContext *context,
    ID3D11Buffer *index_buffer,
    DXGI_FORMAT format,
    UINT offset)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.ia_set_index_buffer(context, index_buffer, format, offset);

  std::ostringstream payload;
  payload << "{\"format\":" << static_cast<unsigned int>(format) << ",\"offset\":" << offset << "}";
  record_call_locked("ID3D11DeviceContext::IASetIndexBuffer", S_OK, {context, index_buffer}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_draw_indexed_instanced(
    ID3D11DeviceContext *context,
    UINT index_count_per_instance,
    UINT instance_count,
    UINT start_index_location,
    INT base_vertex_location,
    UINT start_instance_location)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  ensure_frame_begin_locked();
  auto &hook = context_hook_locked(context);
  hook.draw_indexed_instanced(
      context,
      index_count_per_instance,
      instance_count,
      start_index_location,
      base_vertex_location,
      start_instance_location);

  std::ostringstream payload;
  payload << "{\"index_count_per_instance\":" << index_count_per_instance << ",\"instance_count\":" << instance_count
          << ",\"start_index_location\":" << start_index_location << ",\"base_vertex_location\":" << base_vertex_location
          << ",\"start_instance_location\":" << start_instance_location << "}";
  record_call_locked("ID3D11DeviceContext::DrawIndexedInstanced", S_OK, {context}, {}, payload.str());
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

void STDMETHODCALLTYPE hook_om_set_blend_state(
    ID3D11DeviceContext *context,
    ID3D11BlendState *blend_state,
    const FLOAT blend_factor[4],
    UINT sample_mask)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.om_set_blend_state(context, blend_state, blend_factor, sample_mask);

  std::ostringstream payload;
  payload << "{\"blend_factor\":["
          << (blend_factor ? blend_factor[0] : 0.0f) << ","
          << (blend_factor ? blend_factor[1] : 0.0f) << ","
          << (blend_factor ? blend_factor[2] : 0.0f) << ","
          << (blend_factor ? blend_factor[3] : 0.0f) << "],"
          << "\"sample_mask\":" << sample_mask << "}";
  record_call_locked("ID3D11DeviceContext::OMSetBlendState", S_OK, {context, blend_state}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_om_set_depth_stencil_state(
    ID3D11DeviceContext *context,
    ID3D11DepthStencilState *depth_stencil_state,
    UINT stencil_ref)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.om_set_depth_stencil_state(context, depth_stencil_state, stencil_ref);

  std::ostringstream payload;
  payload << "{\"stencil_ref\":" << stencil_ref << "}";
  record_call_locked(
      "ID3D11DeviceContext::OMSetDepthStencilState",
      S_OK,
      {context, depth_stencil_state},
      {},
      payload.str());
}

void STDMETHODCALLTYPE hook_rs_set_state(ID3D11DeviceContext *context, ID3D11RasterizerState *rasterizer_state)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.rs_set_state(context, rasterizer_state);
  record_call_locked("ID3D11DeviceContext::RSSetState", S_OK, {context, rasterizer_state}, {}, "{}");
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

void STDMETHODCALLTYPE hook_rs_set_scissor_rects(ID3D11DeviceContext *context, UINT num_rects, const D3D11_RECT *rects)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.rs_set_scissor_rects(context, num_rects, rects);

  std::ostringstream payload;
  payload << "{\"num_rects\":" << num_rects << ",\"rects\":[";
  for (UINT index = 0; index < num_rects; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &rect = rects[index];
    payload << "{\"left\":" << rect.left << ",\"top\":" << rect.top << ",\"right\":" << rect.right
            << ",\"bottom\":" << rect.bottom << "}";
  }
  payload << "]}";
  record_call_locked("ID3D11DeviceContext::RSSetScissorRects", S_OK, {context}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_copy_resource(
    ID3D11DeviceContext *context,
    ID3D11Resource *dst_resource,
    ID3D11Resource *src_resource)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.copy_resource(context, dst_resource, src_resource);

  std::ostringstream payload;
  if (const auto *dst_info = lookup_resource_info_locked(dst_resource)) {
    payload << "{\"dst_resource_class\":\"" << resource_class_name(dst_info->resource_class) << "\"";
  } else {
    payload << "{\"dst_resource_class\":\"unknown\"";
  }
  if (const auto *src_info = lookup_resource_info_locked(src_resource)) {
    payload << ",\"src_resource_class\":\"" << resource_class_name(src_info->resource_class) << "\"";
  } else {
    payload << ",\"src_resource_class\":\"unknown\"";
  }
  payload << "}";
  record_call_locked("ID3D11DeviceContext::CopyResource", S_OK, {context, dst_resource, src_resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_update_subresource(
    ID3D11DeviceContext *context,
    ID3D11Resource *dst_resource,
    UINT dst_subresource,
    const D3D11_BOX *dst_box,
    const void *src_data,
    UINT src_row_pitch,
    UINT src_depth_pitch)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.update_subresource(context, dst_resource, dst_subresource, dst_box, src_data, src_row_pitch, src_depth_pitch);

  std::vector<trace::BlobId> blob_refs;
  std::string data_path_json = "null";
  std::string resource_class_json = "\"unknown\"";
  if (const auto *resource_info = lookup_resource_info_locked(dst_resource)) {
    resource_class_json = "\"" + json_escape(resource_class_name(resource_info->resource_class)) + "\"";
    if (src_data) {
      const auto upload_size = resource_info->resource_class == ResourceClass::Buffer
                                   ? static_cast<std::size_t>(resource_info->byte_width)
                                   : texture_upload_size(*resource_info, dst_subresource, dst_box, src_row_pitch, src_depth_pitch);
      if (upload_size != 0) {
        const auto asset_kind =
            resource_info->resource_class == ResourceClass::Buffer ? trace::AssetKind::Buffer : trace::AssetKind::Texture;
        const auto asset_name =
            resource_info->resource_class == ResourceClass::Buffer ? "buffer-update" : "texture-update";
        const auto asset = register_asset_bytes(asset_kind, asset_name, src_data, upload_size);
        blob_refs.push_back(asset.blob_id);
        data_path_json = "\"" + json_escape(asset.relative_path.generic_string()) + "\"";
      }
    }
  }

  std::ostringstream payload;
  payload << "{\"dst_subresource\":" << dst_subresource << ",\"resource_class\":" << resource_class_json
          << ",\"src_row_pitch\":" << src_row_pitch << ",\"src_depth_pitch\":" << src_depth_pitch
          << ",\"has_dst_box\":" << bool_json(dst_box != nullptr);
  if (dst_box) {
    payload << ",\"dst_box\":{\"left\":" << dst_box->left << ",\"top\":" << dst_box->top << ",\"front\":" << dst_box->front
            << ",\"right\":" << dst_box->right << ",\"bottom\":" << dst_box->bottom << ",\"back\":" << dst_box->back << "}";
  }
  payload << ",\"data_path\":" << data_path_json << "}";
  record_call_locked("ID3D11DeviceContext::UpdateSubresource", S_OK, {context, dst_resource}, blob_refs, payload.str());
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

void STDMETHODCALLTYPE hook_clear_depth_stencil_view(
    ID3D11DeviceContext *context,
    ID3D11DepthStencilView *depth_stencil_view,
    UINT clear_flags,
    FLOAT depth,
    UINT8 stencil)
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  auto &hook = context_hook_locked(context);
  hook.clear_depth_stencil_view(context, depth_stencil_view, clear_flags, depth, stencil);

  std::ostringstream payload;
  payload << "{\"clear_flags\":" << clear_flags << ",\"depth\":" << depth << ",\"stencil\":" << static_cast<unsigned int>(stencil)
          << "}";
  record_call_locked("ID3D11DeviceContext::ClearDepthStencilView", S_OK, {context, depth_stencil_view}, {}, payload.str());
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
    const auto parent_object_id = lookup_object_id_locked(swapchain);
    const auto object_id = register_object_locked(*surface, trace::ObjectKind::Resource, "IDXGISwapChain::Buffer", parent_object_id);
    if (riid == __uuidof(ID3D11Texture2D)) {
      auto *texture = static_cast<ID3D11Texture2D *>(*surface);
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      state.resources[texture] = make_texture2d_info(object_id, parent_object_id, desc);
    }
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
  hook.create_texture2d = reinterpret_cast<DeviceCreateTexture2DFn>(vtable[kDeviceCreateTexture2DIndex]);
  hook.create_shader_resource_view =
      reinterpret_cast<DeviceCreateShaderResourceViewFn>(vtable[kDeviceCreateShaderResourceViewIndex]);
  hook.create_render_target_view =
      reinterpret_cast<DeviceCreateRenderTargetViewFn>(vtable[kDeviceCreateRenderTargetViewIndex]);
  hook.create_depth_stencil_view =
      reinterpret_cast<DeviceCreateDepthStencilViewFn>(vtable[kDeviceCreateDepthStencilViewIndex]);
  hook.create_input_layout = reinterpret_cast<DeviceCreateInputLayoutFn>(vtable[kDeviceCreateInputLayoutIndex]);
  hook.create_vertex_shader = reinterpret_cast<DeviceCreateVertexShaderFn>(vtable[kDeviceCreateVertexShaderIndex]);
  hook.create_pixel_shader = reinterpret_cast<DeviceCreatePixelShaderFn>(vtable[kDeviceCreatePixelShaderIndex]);
  hook.create_blend_state = reinterpret_cast<DeviceCreateBlendStateFn>(vtable[kDeviceCreateBlendStateIndex]);
  hook.create_depth_stencil_state =
      reinterpret_cast<DeviceCreateDepthStencilStateFn>(vtable[kDeviceCreateDepthStencilStateIndex]);
  hook.create_rasterizer_state =
      reinterpret_cast<DeviceCreateRasterizerStateFn>(vtable[kDeviceCreateRasterizerStateIndex]);
  hook.create_sampler_state = reinterpret_cast<DeviceCreateSamplerStateFn>(vtable[kDeviceCreateSamplerStateIndex]);
  hook.get_immediate_context = reinterpret_cast<DeviceGetImmediateContextFn>(vtable[kDeviceGetImmediateContextIndex]);

  state.device_hooks.emplace(vtable, hook);
  patch_vtable_entry(vtable, kDeviceCreateBufferIndex, reinterpret_cast<void *>(hook_create_buffer));
  patch_vtable_entry(vtable, kDeviceCreateTexture2DIndex, reinterpret_cast<void *>(hook_create_texture2d));
  patch_vtable_entry(
      vtable,
      kDeviceCreateShaderResourceViewIndex,
      reinterpret_cast<void *>(hook_create_shader_resource_view));
  patch_vtable_entry(vtable, kDeviceCreateRenderTargetViewIndex, reinterpret_cast<void *>(hook_create_render_target_view));
  patch_vtable_entry(vtable, kDeviceCreateDepthStencilViewIndex, reinterpret_cast<void *>(hook_create_depth_stencil_view));
  patch_vtable_entry(vtable, kDeviceCreateInputLayoutIndex, reinterpret_cast<void *>(hook_create_input_layout));
  patch_vtable_entry(vtable, kDeviceCreateVertexShaderIndex, reinterpret_cast<void *>(hook_create_vertex_shader));
  patch_vtable_entry(vtable, kDeviceCreatePixelShaderIndex, reinterpret_cast<void *>(hook_create_pixel_shader));
  patch_vtable_entry(vtable, kDeviceCreateBlendStateIndex, reinterpret_cast<void *>(hook_create_blend_state));
  patch_vtable_entry(
      vtable,
      kDeviceCreateDepthStencilStateIndex,
      reinterpret_cast<void *>(hook_create_depth_stencil_state));
  patch_vtable_entry(vtable, kDeviceCreateRasterizerStateIndex, reinterpret_cast<void *>(hook_create_rasterizer_state));
  patch_vtable_entry(vtable, kDeviceCreateSamplerStateIndex, reinterpret_cast<void *>(hook_create_sampler_state));
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
  hook.ps_set_shader_resources =
      reinterpret_cast<ContextPSSetShaderResourcesFn>(vtable[kContextPSSetShaderResourcesIndex]);
  hook.ps_set_shader = reinterpret_cast<ContextPSSetShaderFn>(vtable[kContextPSSetShaderIndex]);
  hook.ps_set_samplers = reinterpret_cast<ContextPSSetSamplersFn>(vtable[kContextPSSetSamplersIndex]);
  hook.vs_set_shader = reinterpret_cast<ContextVSSetShaderFn>(vtable[kContextVSSetShaderIndex]);
  hook.draw_indexed = reinterpret_cast<ContextDrawIndexedFn>(vtable[kContextDrawIndexedIndex]);
  hook.draw = reinterpret_cast<ContextDrawFn>(vtable[kContextDrawIndex]);
  hook.map = reinterpret_cast<ContextMapFn>(vtable[kContextMapIndex]);
  hook.unmap = reinterpret_cast<ContextUnmapFn>(vtable[kContextUnmapIndex]);
  hook.ps_set_constant_buffers =
      reinterpret_cast<ContextPSSetConstantBuffersFn>(vtable[kContextPSSetConstantBuffersIndex]);
  hook.ia_set_input_layout = reinterpret_cast<ContextIASetInputLayoutFn>(vtable[kContextIASetInputLayoutIndex]);
  hook.ia_set_vertex_buffers =
      reinterpret_cast<ContextIASetVertexBuffersFn>(vtable[kContextIASetVertexBuffersIndex]);
  hook.ia_set_index_buffer = reinterpret_cast<ContextIASetIndexBufferFn>(vtable[kContextIASetIndexBufferIndex]);
  hook.draw_indexed_instanced =
      reinterpret_cast<ContextDrawIndexedInstancedFn>(vtable[kContextDrawIndexedInstancedIndex]);
  hook.ia_set_primitive_topology =
      reinterpret_cast<ContextIASetPrimitiveTopologyFn>(vtable[kContextIASetPrimitiveTopologyIndex]);
  hook.om_set_render_targets =
      reinterpret_cast<ContextOMSetRenderTargetsFn>(vtable[kContextOMSetRenderTargetsIndex]);
  hook.om_set_blend_state = reinterpret_cast<ContextOMSetBlendStateFn>(vtable[kContextOMSetBlendStateIndex]);
  hook.om_set_depth_stencil_state =
      reinterpret_cast<ContextOMSetDepthStencilStateFn>(vtable[kContextOMSetDepthStencilStateIndex]);
  hook.rs_set_state = reinterpret_cast<ContextRSSetStateFn>(vtable[kContextRSSetStateIndex]);
  hook.rs_set_viewports = reinterpret_cast<ContextRSSetViewportsFn>(vtable[kContextRSSetViewportsIndex]);
  hook.rs_set_scissor_rects =
      reinterpret_cast<ContextRSSetScissorRectsFn>(vtable[kContextRSSetScissorRectsIndex]);
  hook.copy_resource = reinterpret_cast<ContextCopyResourceFn>(vtable[kContextCopyResourceIndex]);
  hook.update_subresource =
      reinterpret_cast<ContextUpdateSubresourceFn>(vtable[kContextUpdateSubresourceIndex]);
  hook.clear_render_target_view =
      reinterpret_cast<ContextClearRenderTargetViewFn>(vtable[kContextClearRenderTargetViewIndex]);
  hook.clear_depth_stencil_view =
      reinterpret_cast<ContextClearDepthStencilViewFn>(vtable[kContextClearDepthStencilViewIndex]);

  state.context_hooks.emplace(vtable, hook);
  patch_vtable_entry(vtable, kContextVSSetConstantBuffersIndex, reinterpret_cast<void *>(hook_vs_set_constant_buffers));
  patch_vtable_entry(
      vtable,
      kContextPSSetShaderResourcesIndex,
      reinterpret_cast<void *>(hook_ps_set_shader_resources));
  patch_vtable_entry(vtable, kContextPSSetShaderIndex, reinterpret_cast<void *>(hook_ps_set_shader));
  patch_vtable_entry(vtable, kContextPSSetSamplersIndex, reinterpret_cast<void *>(hook_ps_set_samplers));
  patch_vtable_entry(vtable, kContextVSSetShaderIndex, reinterpret_cast<void *>(hook_vs_set_shader));
  patch_vtable_entry(vtable, kContextDrawIndexedIndex, reinterpret_cast<void *>(hook_draw_indexed));
  patch_vtable_entry(vtable, kContextDrawIndex, reinterpret_cast<void *>(hook_draw));
  patch_vtable_entry(vtable, kContextMapIndex, reinterpret_cast<void *>(hook_map));
  patch_vtable_entry(vtable, kContextUnmapIndex, reinterpret_cast<void *>(hook_unmap));
  patch_vtable_entry(vtable, kContextPSSetConstantBuffersIndex, reinterpret_cast<void *>(hook_ps_set_constant_buffers));
  patch_vtable_entry(vtable, kContextIASetInputLayoutIndex, reinterpret_cast<void *>(hook_ia_set_input_layout));
  patch_vtable_entry(vtable, kContextIASetVertexBuffersIndex, reinterpret_cast<void *>(hook_ia_set_vertex_buffers));
  patch_vtable_entry(vtable, kContextIASetIndexBufferIndex, reinterpret_cast<void *>(hook_ia_set_index_buffer));
  patch_vtable_entry(
      vtable,
      kContextDrawIndexedInstancedIndex,
      reinterpret_cast<void *>(hook_draw_indexed_instanced));
  patch_vtable_entry(vtable, kContextIASetPrimitiveTopologyIndex, reinterpret_cast<void *>(hook_ia_set_primitive_topology));
  patch_vtable_entry(vtable, kContextOMSetRenderTargetsIndex, reinterpret_cast<void *>(hook_om_set_render_targets));
  patch_vtable_entry(vtable, kContextOMSetBlendStateIndex, reinterpret_cast<void *>(hook_om_set_blend_state));
  patch_vtable_entry(
      vtable,
      kContextOMSetDepthStencilStateIndex,
      reinterpret_cast<void *>(hook_om_set_depth_stencil_state));
  patch_vtable_entry(vtable, kContextRSSetStateIndex, reinterpret_cast<void *>(hook_rs_set_state));
  patch_vtable_entry(vtable, kContextRSSetViewportsIndex, reinterpret_cast<void *>(hook_rs_set_viewports));
  patch_vtable_entry(
      vtable,
      kContextRSSetScissorRectsIndex,
      reinterpret_cast<void *>(hook_rs_set_scissor_rects));
  patch_vtable_entry(vtable, kContextCopyResourceIndex, reinterpret_cast<void *>(hook_copy_resource));
  patch_vtable_entry(
      vtable,
      kContextUpdateSubresourceIndex,
      reinterpret_cast<void *>(hook_update_subresource));
  patch_vtable_entry(vtable, kContextClearRenderTargetViewIndex, reinterpret_cast<void *>(hook_clear_render_target_view));
  patch_vtable_entry(
      vtable,
      kContextClearDepthStencilViewIndex,
      reinterpret_cast<void *>(hook_clear_depth_stencil_view));
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

void emit_scene_marker(const char *scene_name, const char *dx_mode, const char *phase) noexcept
{
  auto &state = capture_state();
  std::lock_guard<std::recursive_mutex> lock(state.mutex);
  record_scene_marker_locked(scene_name, dx_mode, phase);
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
