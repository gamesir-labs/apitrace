#include "apitrace/metal_replay_backend_factory.hpp"

#include <nlohmann/json.hpp>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace apitrace::replay {

namespace {

using json = nlohmann::json;

json parse_json(std::string_view text)
{
  if (text.empty()) {
    return json::object();
  }

  json parsed = json::parse(text, nullptr, false);
  return parsed.is_discarded() ? json::object() : parsed;
}

json parse_nested_json(const json &parent, const char *field_name)
{
  const auto it = parent.find(field_name);
  if (it == parent.end()) {
    return json::object();
  }
  if (it->is_object() || it->is_array()) {
    return *it;
  }
  if (it->is_string()) {
    return parse_json(it->get_ref<const std::string &>());
  }
  return json::object();
}

NSNumber *object_key(std::uint64_t object_id)
{
  return [NSNumber numberWithUnsignedLongLong:object_id];
}

MTLPixelFormat pixel_format_from_string(std::string_view name)
{
  if (name == "bgra8unorm") {
    return MTLPixelFormatBGRA8Unorm;
  }
  if (name == "rgba8unorm") {
    return MTLPixelFormatRGBA8Unorm;
  }
  if (name == "r32uint") {
    return MTLPixelFormatR32Uint;
  }
  return MTLPixelFormatBGRA8Unorm;
}

MTLPixelFormat pixel_format_from_json_field_or(const json &descriptor, const char *field_name, MTLPixelFormat fallback)
{
  const auto it = descriptor.find(field_name);
  if (it == descriptor.end()) {
    return fallback;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    return static_cast<MTLPixelFormat>(it->get<std::uint32_t>());
  }
  if (it->is_string()) {
    return pixel_format_from_string(it->get_ref<const std::string &>());
  }
  return fallback;
}

MTLPixelFormat pixel_format_from_json_field(const json &descriptor, const char *field_name)
{
  return pixel_format_from_json_field_or(descriptor, field_name, MTLPixelFormatBGRA8Unorm);
}

MTLPrimitiveType primitive_type_from_integer(std::uint32_t value)
{
  switch (value) {
  case 1:
    return MTLPrimitiveTypeLine;
  case 2:
    return MTLPrimitiveTypeLineStrip;
  case 3:
    return MTLPrimitiveTypeTriangle;
  case 4:
    return MTLPrimitiveTypeTriangleStrip;
  default:
    return MTLPrimitiveTypePoint;
  }
}

MTLIndexType index_type_from_integer(std::uint32_t value)
{
  return value == 0 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

void apply_stencil_descriptor(MTLStencilDescriptor *descriptor, const json &payload)
{
  if (descriptor == nil || !payload.value("enabled", false)) {
    return;
  }
  descriptor.depthStencilPassOperation =
      static_cast<MTLStencilOperation>(payload.value("depth_stencil_pass_op", 0u));
  descriptor.stencilFailureOperation =
      static_cast<MTLStencilOperation>(payload.value("stencil_fail_op", 0u));
  descriptor.depthFailureOperation =
      static_cast<MTLStencilOperation>(payload.value("depth_fail_op", 0u));
  descriptor.stencilCompareFunction =
      static_cast<MTLCompareFunction>(payload.value("stencil_compare_function", 0u));
  descriptor.writeMask = static_cast<std::uint32_t>(payload.value("write_mask", 0u));
  descriptor.readMask = static_cast<std::uint32_t>(payload.value("read_mask", 0u));
}

MTLLoadAction load_action_from_string(std::string_view name)
{
  if (name == "load") {
    return MTLLoadActionLoad;
  }
  if (name == "dontcare") {
    return MTLLoadActionDontCare;
  }
  return MTLLoadActionClear;
}

MTLLoadAction load_action_from_json_field(const json &payload, const char *field_name)
{
  const auto it = payload.find(field_name);
  if (it == payload.end()) {
    return MTLLoadActionClear;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    return static_cast<MTLLoadAction>(it->get<std::uint32_t>());
  }
  if (it->is_string()) {
    return load_action_from_string(it->get_ref<const std::string &>());
  }
  return MTLLoadActionClear;
}

MTLStoreAction store_action_from_string(std::string_view name)
{
  if (name == "dontcare") {
    return MTLStoreActionDontCare;
  }
  return MTLStoreActionStore;
}

MTLStoreAction store_action_from_json_field(const json &payload, const char *field_name)
{
  const auto it = payload.find(field_name);
  if (it == payload.end()) {
    return MTLStoreActionStore;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    return static_cast<MTLStoreAction>(it->get<std::uint32_t>());
  }
  if (it->is_string()) {
    return store_action_from_string(it->get_ref<const std::string &>());
  }
  return MTLStoreActionStore;
}

std::uint64_t optional_object_id_from_json_field(const json &payload, const char *field_name)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || it->is_null()) {
    return 0;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    return it->get<std::uint64_t>();
  }
  return 0;
}

MTLOrigin origin_from_json_array(const json &payload, const char *field_name)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || !it->is_array() || it->size() < 3) {
    return MTLOriginMake(0, 0, 0);
  }
  return MTLOriginMake(
      static_cast<NSUInteger>((*it)[0].get<std::uint64_t>()),
      static_cast<NSUInteger>((*it)[1].get<std::uint64_t>()),
      static_cast<NSUInteger>((*it)[2].get<std::uint64_t>()));
}

MTLSize size_from_json_array(const json &payload, const char *field_name, id<MTLTexture> fallback_texture)
{
  const auto it = payload.find(field_name);
  if (it != payload.end() && it->is_array() && it->size() >= 3) {
    return MTLSizeMake(
        static_cast<NSUInteger>((*it)[0].get<std::uint64_t>()),
        static_cast<NSUInteger>((*it)[1].get<std::uint64_t>()),
        static_cast<NSUInteger>((*it)[2].get<std::uint64_t>()));
  }
  if (fallback_texture != nil) {
    return MTLSizeMake(fallback_texture.width, fallback_texture.height, 1);
  }
  return MTLSizeMake(1, 1, 1);
}

MTLViewport viewport_from_json_array(const json &value)
{
  if (!value.is_array() || value.size() < 6) {
    return {0.0, 0.0, 1.0, 1.0, 0.0, 1.0};
  }
  return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>(),
          value[3].get<double>(), value[4].get<double>(), value[5].get<double>()};
}

MTLScissorRect scissor_from_json_array(const json &value)
{
  if (!value.is_array() || value.size() < 4) {
    return MTLScissorRect{0, 0, 1, 1};
  }
  return MTLScissorRect{static_cast<NSUInteger>(value[0].get<std::uint64_t>()),
                        static_cast<NSUInteger>(value[1].get<std::uint64_t>()),
                        static_cast<NSUInteger>(value[2].get<std::uint64_t>()),
                        static_cast<NSUInteger>(value[3].get<std::uint64_t>())};
}

MTLTextureSwizzleChannels swizzle_channels_from_json_array(const json &value)
{
  if (!value.is_array() || value.size() < 4) {
    return MTLTextureSwizzleChannelsMake(
        MTLTextureSwizzleRed,
        MTLTextureSwizzleGreen,
        MTLTextureSwizzleBlue,
        MTLTextureSwizzleAlpha);
  }
  return MTLTextureSwizzleChannelsMake(
      static_cast<MTLTextureSwizzle>(value[0].get<std::uint32_t>()),
      static_cast<MTLTextureSwizzle>(value[1].get<std::uint32_t>()),
      static_cast<MTLTextureSwizzle>(value[2].get<std::uint32_t>()),
      static_cast<MTLTextureSwizzle>(value[3].get<std::uint32_t>()));
}

const char *present_frame_format_for_texture(id<MTLTexture> texture)
{
  if (texture == nil) {
    return "bgra8";
  }
  switch (texture.pixelFormat) {
  case MTLPixelFormatRGBA8Unorm:
  case MTLPixelFormatRGBA8Unorm_sRGB:
    return "rgba8";
  case MTLPixelFormatBGRA8Unorm:
  case MTLPixelFormatBGRA8Unorm_sRGB:
  default:
    return "bgra8";
  }
}

std::vector<std::uint8_t> bytes_from_json_array(const json &payload, const char *field_name)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || !it->is_array()) {
    return {};
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(it->size());
  for (const auto &value : *it) {
    bytes.push_back(static_cast<std::uint8_t>(value.get<int>()));
  }
  return bytes;
}

MTLTextureType texture_type_from_json_field(const json &descriptor, const char *field_name)
{
  const auto it = descriptor.find(field_name);
  if (it == descriptor.end() || !(it->is_number_unsigned() || it->is_number_integer())) {
    return MTLTextureType2D;
  }
  return static_cast<MTLTextureType>(it->get<std::uint32_t>());
}

id<MTLFunction> new_function(
    id<MTLLibrary> library,
    const std::string &function_name,
    const json &function_constants,
    NSError **error)
{
  if (library == nil || function_name.empty()) {
    return nil;
  }
  NSString *name = [NSString stringWithUTF8String:function_name.c_str()];
  if (!function_constants.is_array() || function_constants.empty()) {
    return [library newFunctionWithName:name];
  }

  auto *values = [[MTLFunctionConstantValues alloc] init];
  std::vector<std::vector<std::uint8_t>> byte_storage;
  byte_storage.reserve(function_constants.size());
  for (const auto &constant : function_constants) {
    const auto index = static_cast<NSUInteger>(constant.value("index", 0u));
    const auto type = static_cast<MTLDataType>(constant.value("type", 0u));
    if (constant.contains("bool_value")) {
      const auto &bool_value = constant["bool_value"];
      const bool value = bool_value.is_boolean() ? bool_value.get<bool>() : bool_value.get<int>() != 0;
      [values setConstantValue:&value type:type atIndex:index];
      continue;
    }

    byte_storage.push_back(bytes_from_json_array(constant, "bytes"));
    if (!byte_storage.back().empty()) {
      [values setConstantValue:byte_storage.back().data() type:type atIndex:index];
    }
  }

  id<MTLFunction> function = [library newFunctionWithName:name constantValues:values error:error];
  return function;
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::uint64_t gpu_address_for_buffer(id<MTLBuffer> buffer)
{
  if (buffer == nil || ![buffer respondsToSelector:@selector(gpuAddress)]) {
    return 0;
  }
  return static_cast<std::uint64_t>([buffer gpuAddress]);
}

std::uint64_t gpu_resource_id_for_texture(id<MTLTexture> texture)
{
  if (texture == nil || ![texture respondsToSelector:@selector(gpuResourceID)]) {
    return 0;
  }
  return static_cast<std::uint64_t>([texture gpuResourceID]._impl);
}

std::uint64_t gpu_resource_id_for_sampler(id<MTLSamplerState> sampler)
{
  if (sampler == nil || ![sampler respondsToSelector:@selector(gpuResourceID)]) {
    return 0;
  }
  return static_cast<std::uint64_t>([sampler gpuResourceID]._impl);
}

class NativeMetalReplayBackend final : public MetalReplayBackend {
public:
  NativeMetalReplayBackend() = default;
  ~NativeMetalReplayBackend() override = default;

  bool initialize(const trace::TraceBundleReader &reader, const ReplayOptions &options) override
  {
    reader_ = &reader;
    options_ = options;
    options_.enable_metal_present_capture =
        std::getenv("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES") != nullptr;

    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nil) {
      last_error_ = "MTLCreateSystemDefaultDevice returned nil";
      return false;
    }

    queue_ = [device_ newCommandQueue];
    if (queue_ == nil) {
      last_error_ = "failed to create Metal command queue";
      return false;
    }

    buffers_ = [[NSMutableDictionary alloc] init];
    textures_ = [[NSMutableDictionary alloc] init];
    samplers_ = [[NSMutableDictionary alloc] init];
    libraries_ = [[NSMutableDictionary alloc] init];
    render_pipelines_ = [[NSMutableDictionary alloc] init];
    compute_pipelines_ = [[NSMutableDictionary alloc] init];
    depth_stencil_states_ = [[NSMutableDictionary alloc] init];
    command_buffers_ = [[NSMutableDictionary alloc] init];
    command_buffer_present_sizes_ = [[NSMutableDictionary alloc] init];
    command_buffer_present_textures_ = [[NSMutableDictionary alloc] init];
    render_encoder_command_buffers_ = [[NSMutableDictionary alloc] init];
    render_encoders_ = [[NSMutableDictionary alloc] init];
    compute_encoders_ = [[NSMutableDictionary alloc] init];
    blit_encoders_ = [[NSMutableDictionary alloc] init];
    pending_presents_ = [[NSMutableDictionary alloc] init];

    if (const char *bundle_root = std::getenv("APITRACE_TRACE_BUNDLE"); bundle_root && *bundle_root) {
      capture_writer_ = std::make_unique<trace::TraceBundleWriter>();
      if (!capture_writer_->open(bundle_root)) {
        last_error_ = "failed to open APITRACE_TRACE_BUNDLE for metal retrace capture";
        return false;
      }
      trace::TraceMetadata metadata;
      metadata.api = trace::ApiKind::Unknown;
      metadata.producer = "apitrace_metal_retrace";
      capture_writer_->write_metadata(metadata);
    }

    for (const auto &event : reader_->metal_events()) {
      if (event.call_kind == trace::MetalCallKind::InsertDebugSignpost) {
        const json signpost = parse_json(parse_json(event.payload).value("label", std::string()));
        const auto kind = signpost.value("kind", std::string());
        if (kind == "dxmt_buffer_gpu_address") {
          const auto buffer_id = signpost.value("buffer_id", 0ull);
          const auto gpu_address = signpost.value("gpu_address", 0ull);
          if (buffer_id != 0 && gpu_address != 0) {
            for (const auto &record : reader_->metal_events()) {
              if (record.call_kind != trace::MetalCallKind::DeviceCreate ||
                  record.function_name != "MTLDevice.newBuffer" ||
                  record.object_id != buffer_id) {
                continue;
              }
              const json buffer_payload = parse_json(record.payload);
              original_buffer_lengths_[gpu_address] =
                  buffer_payload.value("length", original_buffer_lengths_[gpu_address]);
            }
          }
        } else if (kind == "dxmt_texture_gpu_resource_id") {
          const auto texture_id = signpost.value("texture_id", 0ull);
          const auto gpu_resource_id = signpost.value("gpu_resource_id", 0ull);
          if (texture_id != 0 && gpu_resource_id != 0) {
            original_texture_gpu_resource_ids_[texture_id] = gpu_resource_id;
          }
        } else if (kind == "dxmt_sampler_gpu_resource_id") {
          const auto sampler_id = signpost.value("sampler_id", 0ull);
          const auto gpu_resource_id = signpost.value("gpu_resource_id", 0ull);
          if (sampler_id != 0 && gpu_resource_id != 0) {
            original_sampler_gpu_resource_ids_[sampler_id] = gpu_resource_id;
          }
        }
      }

      if (event.call_kind != trace::MetalCallKind::PresentDrawable) {
        continue;
      }

      const json payload = parse_json(event.payload);
      const auto width = payload.value("width", 0u);
      const auto height = payload.value("height", 0u);
      if (width == 0 || height == 0) {
        continue;
      }

      NSDictionary *size = @{
        @"width" : [NSNumber numberWithUnsignedInt:width],
        @"height" : [NSNumber numberWithUnsignedInt:height],
      };
      [command_buffer_present_sizes_ setObject:size forKey:object_key(event.object_id)];
    }

    return true;
  }

  bool replay_metal_event(const trace::MetalEventRecord &event) override
  {
    const json payload = parse_json(event.payload);

    switch (event.call_kind) {
    case trace::MetalCallKind::DeviceCreate:
      return replay_device_create(event, payload);
    case trace::MetalCallKind::CommandBufferBegin:
      return begin_command_buffer(event);
    case trace::MetalCallKind::CommandBufferCommit:
      return commit_command_buffer(event);
    case trace::MetalCallKind::RenderEncoderBegin:
      return begin_render_encoder(event, payload);
    case trace::MetalCallKind::RenderEncoderEnd:
      return end_render_encoder(event.object_id);
    case trace::MetalCallKind::ComputeEncoderBegin:
      return begin_compute_encoder(event, payload);
    case trace::MetalCallKind::ComputeEncoderEnd:
      return end_compute_encoder(event.object_id);
    case trace::MetalCallKind::BlitEncoderBegin:
      return begin_blit_encoder(event, payload);
    case trace::MetalCallKind::BlitEncoderEnd:
      return end_blit_encoder(event.object_id);
    case trace::MetalCallKind::SetRenderPipelineState:
      return set_render_pipeline_state(event.object_id, payload);
    case trace::MetalCallKind::SetVertexBuffer:
      return set_vertex_buffer(event.object_id, payload);
    case trace::MetalCallKind::SetVertexBytes:
      return set_vertex_bytes(event.object_id, payload);
    case trace::MetalCallKind::SetVertexBufferOffset:
      return set_vertex_buffer_offset(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentTexture:
      return set_fragment_texture(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBuffer:
      return set_fragment_buffer(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBytes:
      return set_fragment_bytes(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBufferOffset:
      return set_fragment_buffer_offset(event.object_id, payload);
    case trace::MetalCallKind::SetViewport:
      return set_viewport(event.object_id, payload);
    case trace::MetalCallKind::SetScissorRect:
      return set_scissor_rect(event.object_id, payload);
    case trace::MetalCallKind::SetComputePipelineState:
      return set_compute_pipeline_state(event.object_id, payload);
    case trace::MetalCallKind::SetComputeBuffer:
      return set_compute_buffer(event.object_id, payload);
    case trace::MetalCallKind::SetComputeBufferOffset:
      return set_compute_buffer_offset(event.object_id, payload);
    case trace::MetalCallKind::DrawPrimitives:
      return draw_primitives(event.object_id, payload);
    case trace::MetalCallKind::DrawIndexedPrimitives:
      return draw_indexed_primitives(event.object_id, payload);
    case trace::MetalCallKind::DrawPrimitivesIndirect:
      return draw_primitives_indirect(event.object_id, payload);
    case trace::MetalCallKind::DispatchThreadgroups:
      return dispatch_threadgroups(event.object_id, payload);
    case trace::MetalCallKind::DispatchThreadgroupsIndirect:
      return dispatch_threadgroups_indirect(event.object_id, payload);
    case trace::MetalCallKind::CopyBuffer:
      return copy_buffer(event.object_id, payload);
    case trace::MetalCallKind::CopyTexture:
      return copy_texture(event.object_id, payload);
    case trace::MetalCallKind::PresentDrawable:
      return queue_present(event.object_id, payload);
    case trace::MetalCallKind::UseResource:
      return use_resource(event.object_id, payload);
    case trace::MetalCallKind::InsertDebugSignpost:
      return handle_debug_signpost(event.object_id, payload);
    case trace::MetalCallKind::UseResources:
    case trace::MetalCallKind::SetArgumentBuffer:
    case trace::MetalCallKind::UpdateFence:
    case trace::MetalCallKind::WaitForFence:
    case trace::MetalCallKind::MemoryBarrier:
    case trace::MetalCallKind::PushDebugGroup:
    case trace::MetalCallKind::PopDebugGroup:
      return true;
    default:
      return true;
    }
  }

  bool finalize() override
  {
    if (capture_writer_) {
      capture_writer_->close();
      capture_writer_.reset();
    }
    return true;
  }

  const std::string &last_error() const override
  {
    return last_error_;
  }

private:
  id<MTLBuffer> buffer_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [buffers_ objectForKey:object_key(object_id)];
  }

  id<MTLTexture> texture_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [textures_ objectForKey:object_key(object_id)];
  }

  id<MTLSamplerState> sampler_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [samplers_ objectForKey:object_key(object_id)];
  }

  id<MTLLibrary> library_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [libraries_ objectForKey:object_key(object_id)];
  }

  id<MTLRenderPipelineState> render_pipeline_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [render_pipelines_ objectForKey:object_key(object_id)];
  }

  id<MTLComputePipelineState> compute_pipeline_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [compute_pipelines_ objectForKey:object_key(object_id)];
  }

  id<MTLDepthStencilState> depth_stencil_state_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [depth_stencil_states_ objectForKey:object_key(object_id)];
  }

  id<MTLCommandBuffer> command_buffer_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [command_buffers_ objectForKey:object_key(object_id)];
  }

  id<MTLRenderCommandEncoder> render_encoder_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [render_encoders_ objectForKey:object_key(object_id)];
  }

  id<MTLComputeCommandEncoder> compute_encoder_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [compute_encoders_ objectForKey:object_key(object_id)];
  }

  id<MTLBlitCommandEncoder> blit_encoder_for_id(std::uint64_t object_id) const
  {
    return object_id == 0 ? nil : [blit_encoders_ objectForKey:object_key(object_id)];
  }

  id<MTLResource> resource_for_id(std::uint64_t object_id) const
  {
    id<MTLBuffer> buffer = buffer_for_id(object_id);
    if (buffer != nil) {
      return buffer;
    }
    return texture_for_id(object_id);
  }

  std::filesystem::path bundle_path(std::string_view relative_path) const
  {
    return reader_->layout().root_path / std::filesystem::path(relative_path);
  }

  bool fail(std::string message)
  {
    last_error_ = std::move(message);
    return false;
  }

  struct BufferState {
    id<MTLBuffer> buffer = nil;
    std::uint64_t original_gpu_address = 0;
    std::uint64_t replay_gpu_address = 0;
    std::uint64_t length = 0;
  };

  struct TextureState {
    id<MTLTexture> texture = nil;
    std::uint64_t original_gpu_resource_id = 0;
    std::uint64_t replay_gpu_resource_id = 0;
    std::uint64_t width = 1;
    std::uint64_t height = 1;
  };

  struct SamplerState {
    id<MTLSamplerState> sampler = nil;
    std::uint64_t original_gpu_resource_id = 0;
    std::uint64_t replay_gpu_resource_id = 0;
  };

  struct BufferBinding {
    std::uint64_t buffer_id = 0;
    std::uint64_t offset = 0;
    id<MTLBuffer> buffer = nil;
  };

  using BufferBindings = std::unordered_map<std::uint32_t, BufferBinding>;

  struct ActiveResource {
    bool is_buffer = false;
    BufferState buffer;
    bool is_texture = false;
    TextureState texture;
    bool is_sampler = false;
    SamplerState sampler;
  };

  struct EncoderResourceState {
    std::vector<ActiveResource> active;
    std::vector<ActiveResource> draw_scope;
  };

  static constexpr std::size_t kArgumentResourceRecordSize = 16;

  enum class HandlePatchMode {
    GpuAddresses,
    ResourceIds,
    All,
  };

  static bool mode_allows_gpu_addresses(HandlePatchMode mode)
  {
    return mode == HandlePatchMode::GpuAddresses || mode == HandlePatchMode::All;
  }

  static bool mode_allows_resource_ids(HandlePatchMode mode)
  {
    return mode == HandlePatchMode::ResourceIds || mode == HandlePatchMode::All;
  }

  std::uint64_t replacement_for_gpu_handle(
      std::uint64_t value,
      std::uint64_t encoder_id,
      HandlePatchMode mode) const
  {
    const auto active_it = encoder_resource_states_.find(encoder_id);
    if (active_it != encoder_resource_states_.end()) {
      const auto &resources =
          mode == HandlePatchMode::ResourceIds && !active_it->second.draw_scope.empty()
              ? active_it->second.draw_scope
              : active_it->second.active;
      for (const auto &resource : resources) {
        if (mode_allows_resource_ids(mode) &&
            resource.is_texture &&
            resource.texture.original_gpu_resource_id != 0 &&
            resource.texture.replay_gpu_resource_id != 0 &&
            resource.texture.original_gpu_resource_id == value) {
          return resource.texture.replay_gpu_resource_id;
        }
        if (mode_allows_resource_ids(mode) &&
            resource.is_sampler &&
            resource.sampler.original_gpu_resource_id != 0 &&
            resource.sampler.replay_gpu_resource_id != 0 &&
            resource.sampler.original_gpu_resource_id == value) {
          return resource.sampler.replay_gpu_resource_id;
        }

        if (mode_allows_gpu_addresses(mode) && resource.is_buffer) {
          const auto &buffer = resource.buffer;
          if (buffer.original_gpu_address == 0 ||
              buffer.replay_gpu_address == 0 ||
              buffer.length == 0 ||
              value < buffer.original_gpu_address) {
            continue;
          }
          const auto delta = value - buffer.original_gpu_address;
          if (delta < buffer.length) {
            return buffer.replay_gpu_address + delta;
          }
        }
      }
    }

    if (mode_allows_resource_ids(mode)) {
      const auto texture_it = texture_gpu_resource_id_map_.find(value);
      if (texture_it != texture_gpu_resource_id_map_.end()) {
        return texture_it->second;
      }
      const auto sampler_it = sampler_gpu_resource_id_map_.find(value);
      if (sampler_it != sampler_gpu_resource_id_map_.end()) {
        return sampler_it->second;
      }
    }

    if (!mode_allows_gpu_addresses(mode)) {
      return 0;
    }

    std::uint64_t replacement = 0;
    for (const auto &[object_id, state] : buffer_states_) {
      if (state.original_gpu_address == 0 ||
          state.replay_gpu_address == 0 ||
          state.length == 0 ||
          value < state.original_gpu_address) {
        continue;
      }
      const auto delta = value - state.original_gpu_address;
      if (delta >= state.length) {
        continue;
      }
      const auto candidate = state.replay_gpu_address + delta;
      if (replacement != 0 && replacement != candidate) {
        return 0;
      }
      replacement = candidate;
    }
    return replacement;
  }

  bool debug_gpu_patch_enabled() const
  {
    const char *value = std::getenv("APITRACE_METAL_RETRACE_DEBUG_GPU_PATCH");
    return value != nullptr && value[0] != '\0';
  }

  std::size_t patch_gpu_handle_slot(
      std::uint8_t *bytes,
      std::size_t size,
      std::uint64_t encoder_id,
      HandlePatchMode mode) const
  {
    if (bytes == nullptr || size < sizeof(std::uint64_t)) {
      return 0;
    }

    std::uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    const auto replacement = replacement_for_gpu_handle(value, encoder_id, mode);
    if (replacement != 0 && replacement != value) {
      std::memcpy(bytes, &replacement, sizeof(replacement));
      return 1;
    }
    return 0;
  }

  std::size_t patch_argument_resource_records(
      std::uint8_t *bytes,
      std::size_t size,
      std::uint64_t encoder_id,
      std::size_t record_count,
      HandlePatchMode mode) const
  {
    if (bytes == nullptr || size < sizeof(std::uint64_t) || record_count == 0) {
      return 0;
    }

    std::size_t patched = 0;
    const auto table_size = std::min(size, record_count * kArgumentResourceRecordSize);
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= table_size;
         offset += kArgumentResourceRecordSize) {
      patched += patch_gpu_handle_slot(bytes + offset, sizeof(std::uint64_t), encoder_id, mode);
    }
    return patched;
  }

  std::size_t patch_resource_id_argument_slots(
      std::uint8_t *bytes,
      std::size_t size,
      std::uint64_t encoder_id) const
  {
    if (bytes == nullptr || size < sizeof(std::uint64_t)) {
      return 0;
    }

    std::size_t patched = 0;
    constexpr std::array<std::size_t, 5> kResourceIdSlots = {0, 8, 24, 40, 56};
    for (const auto offset : kResourceIdSlots) {
      if (offset + sizeof(std::uint64_t) <= size) {
        patched += patch_gpu_handle_slot(bytes + offset, sizeof(std::uint64_t), encoder_id, HandlePatchMode::ResourceIds);
      }
    }
    return patched;
  }

  void patch_bound_buffer_binding(
      std::uint32_t index,
      const BufferBinding &binding,
      std::uint64_t encoder_id) const
  {
    id<MTLBuffer> buffer = binding.buffer;
    if (buffer == nil) {
      const auto state_it = buffer_states_.find(binding.buffer_id);
      if (state_it != buffer_states_.end()) {
        buffer = state_it->second.buffer;
      }
    }
    if (buffer == nil) {
      return;
    }
    void *contents = [buffer contents];
    if (contents == nullptr) {
      return;
    }
    const auto length = static_cast<std::size_t>([buffer length]);
    if (binding.offset >= length) {
      return;
    }
    std::uint64_t first_value = 0;
    if (length - binding.offset >= sizeof(first_value)) {
      std::memcpy(&first_value, static_cast<std::uint8_t *>(contents) + binding.offset, sizeof(first_value));
    }
    const auto remaining = length - static_cast<std::size_t>(binding.offset);
    std::size_t patched = 0;
    std::size_t active_buffer_count = 1;
    const auto active_it = encoder_resource_states_.find(encoder_id);
    if (active_it != encoder_resource_states_.end()) {
      active_buffer_count = 0;
      for (const auto &resource : active_it->second.active) {
        if (resource.is_buffer) {
          ++active_buffer_count;
        }
      }
      active_buffer_count = std::max<std::size_t>(active_buffer_count, 1);
    }
    patched = patch_argument_resource_records(
        static_cast<std::uint8_t *>(contents) + binding.offset,
        remaining,
        encoder_id,
        active_buffer_count,
        HandlePatchMode::GpuAddresses);
    if (index == 30) {
      patched += patch_resource_id_argument_slots(
          static_cast<std::uint8_t *>(contents) + binding.offset,
          remaining,
          encoder_id);
    }
    if (debug_gpu_patch_enabled() && patched != 0) {
      std::uint64_t patched_first_value = 0;
      std::uint64_t patched_second_record = 0;
      if (length - binding.offset >= sizeof(patched_first_value)) {
        std::memcpy(&patched_first_value, static_cast<std::uint8_t *>(contents) + binding.offset, sizeof(patched_first_value));
      }
      if (length - binding.offset >= kArgumentResourceRecordSize + sizeof(patched_second_record)) {
        std::memcpy(
            &patched_second_record,
            static_cast<std::uint8_t *>(contents) + binding.offset + kArgumentResourceRecordSize,
            sizeof(patched_second_record));
      }
      std::fprintf(
          stderr,
          "metal replay gpu patch: encoder=%llu buffer=%llu index=%u offset=%llu patched=%zu first=0x%llx->0x%llx second=0x%llx\n",
          static_cast<unsigned long long>(encoder_id),
          static_cast<unsigned long long>(binding.buffer_id),
          index,
          static_cast<unsigned long long>(binding.offset),
          patched,
          static_cast<unsigned long long>(first_value),
          static_cast<unsigned long long>(patched_first_value),
          static_cast<unsigned long long>(patched_second_record));
      const auto active_it = encoder_resource_states_.find(encoder_id);
      if (active_it != encoder_resource_states_.end()) {
        for (const auto &resource : active_it->second.active) {
          if (resource.is_buffer) {
            std::fprintf(
                stderr,
                "metal replay gpu patch active-buffer: encoder=%llu original=0x%llx replay=0x%llx length=%llu\n",
                static_cast<unsigned long long>(encoder_id),
                static_cast<unsigned long long>(resource.buffer.original_gpu_address),
                static_cast<unsigned long long>(resource.buffer.replay_gpu_address),
                static_cast<unsigned long long>(resource.buffer.length));
          }
        }
      }
    }
    if (buffer.storageMode == MTLStorageModeManaged) {
      [buffer didModifyRange:NSMakeRange(static_cast<NSUInteger>(binding.offset),
                                         static_cast<NSUInteger>(length - binding.offset))];
    }
  }

  void patch_bound_buffers(
      std::uint64_t encoder_id,
      const BufferBindings &bindings,
      bool render_vertex_stage,
      bool render_fragment_stage) const
  {
    std::unordered_set<std::string> patched_slots;
    for (const auto &[index, binding] : bindings) {
      if (render_vertex_stage && index != 16) {
        continue;
      }
      if (render_fragment_stage && binding.offset == 0) {
        continue;
      }
      const auto key = std::to_string(binding.buffer_id) + ":" + std::to_string(binding.offset);
      if (binding.buffer_id == 0 || !patched_slots.insert(key).second) {
        continue;
      }
      patch_bound_buffer_binding(index, binding, encoder_id);
    }
  }

  void patch_render_bound_buffers(std::uint64_t encoder_id) const
  {
    if (debug_gpu_patch_enabled()) {
      const auto vertex_it = render_vertex_buffer_bindings_.find(encoder_id);
      const auto fragment_it = render_fragment_buffer_bindings_.find(encoder_id);
      const auto active_it = encoder_resource_states_.find(encoder_id);
      const auto vertex_count = vertex_it == render_vertex_buffer_bindings_.end() ? 0 : vertex_it->second.size();
      const auto fragment_count = fragment_it == render_fragment_buffer_bindings_.end() ? 0 : fragment_it->second.size();
      const auto active_count = active_it == encoder_resource_states_.end() ? 0 : active_it->second.active.size();
      std::fprintf(
          stderr,
          "metal replay gpu patch draw-state: encoder=%llu vertex_bindings=%zu fragment_bindings=%zu active_resources=%zu\n",
          static_cast<unsigned long long>(encoder_id),
          vertex_count,
          fragment_count,
          active_count);
      if (vertex_it != render_vertex_buffer_bindings_.end()) {
        for (const auto &[index, binding] : vertex_it->second) {
          id<MTLBuffer> buffer = binding.buffer;
          std::uint64_t v0 = 0;
          std::uint64_t v1 = 0;
          std::uint64_t v2 = 0;
          if (buffer != nil && [buffer contents] != nullptr && [buffer length] >= binding.offset + 72) {
            auto *raw = static_cast<const std::uint8_t *>([buffer contents]) + binding.offset;
            std::memcpy(&v0, raw, sizeof(v0));
            std::memcpy(&v1, raw + 32, sizeof(v1));
            std::memcpy(&v2, raw + 64, sizeof(v2));
          }
          std::fprintf(
              stderr,
              "metal replay gpu patch binding: encoder=%llu stage=vertex index=%u buffer=%llu offset=%llu first=[0x%llx,0x%llx,0x%llx]\n",
              static_cast<unsigned long long>(encoder_id),
              index,
              static_cast<unsigned long long>(binding.buffer_id),
              static_cast<unsigned long long>(binding.offset),
              static_cast<unsigned long long>(v0),
              static_cast<unsigned long long>(v1),
              static_cast<unsigned long long>(v2));
        }
      }
    }
    const auto vertex_it = render_vertex_buffer_bindings_.find(encoder_id);
    if (vertex_it != render_vertex_buffer_bindings_.end()) {
      patch_bound_buffers(encoder_id, vertex_it->second, true, false);
    }
    const auto fragment_it = render_fragment_buffer_bindings_.find(encoder_id);
    if (fragment_it != render_fragment_buffer_bindings_.end()) {
      patch_bound_buffers(encoder_id, fragment_it->second, false, true);
    }
  }

  void patch_compute_bound_buffers(std::uint64_t encoder_id) const
  {
    const auto bindings_it = compute_buffer_bindings_.find(encoder_id);
    if (bindings_it != compute_buffer_bindings_.end()) {
      patch_bound_buffers(encoder_id, bindings_it->second, false, false);
    }
  }

  void record_buffer_binding(BufferBindings &bindings, const json &payload)
  {
    const auto index = payload.value("index", 0u);
    const auto buffer_id = payload.value("buffer_id", 0ull);
    bindings[static_cast<std::uint32_t>(index)] =
        BufferBinding{buffer_id, payload.value("offset", 0ull), buffer_for_id(buffer_id)};
  }

  void record_buffer_offset(BufferBindings &bindings, const json &payload)
  {
    const auto index = static_cast<std::uint32_t>(payload.value("index", 0u));
    auto it = bindings.find(index);
    if (it != bindings.end()) {
      it->second.offset = payload.value("offset", 0ull);
    }
  }

  bool replay_device_create(const trace::MetalEventRecord &event, const json &payload)
  {
    if (event.function_name == "MTLDevice.newBuffer") {
      const auto buffer_path = payload.value("buffer_path", std::string());
      auto bytes = buffer_path.empty() ? std::vector<std::uint8_t>{} : read_binary_file(bundle_path(buffer_path));
      const auto length = static_cast<NSUInteger>(payload.value("length", static_cast<std::uint64_t>(bytes.size())));
      id<MTLBuffer> buffer = [device_ newBufferWithLength:length options:MTLResourceStorageModeShared];
      if (buffer == nil) {
        return fail("failed to create MTLBuffer");
      }
      const auto pending_it = pending_buffer_gpu_addresses_.find(event.object_id);
      const auto original_gpu_address = pending_it == pending_buffer_gpu_addresses_.end() ? 0 : pending_it->second;
      if (pending_it != pending_buffer_gpu_addresses_.end()) {
        pending_buffer_gpu_addresses_.erase(pending_it);
      }
      const auto replay_gpu_address = gpu_address_for_buffer(buffer);
      if (original_gpu_address != 0 && replay_gpu_address != 0) {
        buffer_gpu_address_map_[original_gpu_address] = replay_gpu_address;
      }
      if (!bytes.empty()) {
        std::memcpy([buffer contents], bytes.data(), std::min<std::size_t>(bytes.size(), length));
        if (buffer.storageMode == MTLStorageModeManaged) {
          [buffer didModifyRange:NSMakeRange(0, std::min<std::size_t>(bytes.size(), length))];
        }
      }
      [buffers_ setObject:buffer forKey:object_key(event.object_id)];
      buffer_states_[event.object_id] = BufferState{
          buffer,
          original_gpu_address,
          replay_gpu_address,
          static_cast<std::uint64_t>(length),
      };
      return true;
    }

    if (event.function_name == "MTLDevice.newTexture") {
      return create_texture(event.object_id, parse_nested_json(payload, "descriptor"));
    }

    if (event.function_name == "MTLDevice.newLibrary") {
      const auto library_path = payload.value("library_path", std::string());
      const auto bytes = read_binary_file(bundle_path(library_path));
      if (bytes.empty()) {
        return fail("failed to read metallib asset: " + library_path);
      }
      void *copied_bytes = std::malloc(bytes.size());
      if (copied_bytes == nullptr) {
        return fail("failed to allocate metallib staging bytes");
      }
      std::memcpy(copied_bytes, bytes.data(), bytes.size());
      dispatch_data_t dispatch_data = dispatch_data_create(
          copied_bytes,
          bytes.size(),
          dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
          ^{
            std::free(copied_bytes);
          });
      NSError *error = nil;
      id<MTLLibrary> library = [device_ newLibraryWithData:dispatch_data error:&error];
      if (library == nil) {
        return fail(error ? [[error localizedDescription] UTF8String] : "failed to create MTLLibrary");
      }
      [libraries_ setObject:library forKey:object_key(event.object_id)];
      return true;
    }

    if (event.function_name == "MTLDevice.newRenderPipelineState") {
      return create_render_pipeline(event.object_id, payload);
    }

    if (event.function_name == "MTLDevice.newComputePipelineState") {
      return create_compute_pipeline(event.object_id, payload);
    }

    return true;
  }

  bool create_texture(std::uint64_t object_id, const json &descriptor)
  {
    const auto width = static_cast<NSUInteger>(
        descriptor.value("width", descriptor.value("render_target_width", 1u)));
    const auto height = static_cast<NSUInteger>(
        descriptor.value("height", descriptor.value("render_target_height", 1u)));
    const auto pixel_format = pixel_format_from_json_field(descriptor, "pixel_format");
    auto *texture_descriptor = [[MTLTextureDescriptor alloc] init];
    texture_descriptor.textureType = texture_type_from_json_field(descriptor, "type");
    texture_descriptor.width = std::max<NSUInteger>(width, 1);
    texture_descriptor.height = std::max<NSUInteger>(height, 1);
    texture_descriptor.depth = static_cast<NSUInteger>(descriptor.value("depth", 1u));
    texture_descriptor.arrayLength = static_cast<NSUInteger>(descriptor.value("array_length", descriptor.value("arrayLength", 1u)));
    texture_descriptor.mipmapLevelCount = static_cast<NSUInteger>(descriptor.value("mipmap_level_count", 1u));
    texture_descriptor.sampleCount = static_cast<NSUInteger>(descriptor.value("sample_count", 1u));
    texture_descriptor.pixelFormat = pixel_format;
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = static_cast<MTLTextureUsage>(
        descriptor.value("usage", static_cast<std::uint32_t>(
                                      MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)));

    id<MTLTexture> texture = [device_ newTextureWithDescriptor:texture_descriptor];
    if (texture == nil) {
      return fail("failed to create MTLTexture");
    }

    const auto initial_bytes = descriptor.find("initial_bytes");
    if (initial_bytes != descriptor.end() && initial_bytes->is_array() && !initial_bytes->empty()) {
      std::vector<std::uint8_t> bytes;
      bytes.reserve(initial_bytes->size());
      for (const auto &value : *initial_bytes) {
        bytes.push_back(static_cast<std::uint8_t>(value.get<int>()));
      }
      const auto bytes_per_row = static_cast<NSUInteger>(descriptor.value("bytes_per_row", width * 4));
      [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                 mipmapLevel:0
                   withBytes:bytes.data()
                 bytesPerRow:bytes_per_row];
    }

    [textures_ setObject:texture forKey:object_key(object_id)];
    const auto original_gpu_resource_id = original_texture_gpu_resource_ids_[object_id];
    const auto replay_gpu_resource_id = gpu_resource_id_for_texture(texture);
    if (original_gpu_resource_id != 0 && replay_gpu_resource_id != 0) {
      texture_gpu_resource_id_map_[original_gpu_resource_id] = replay_gpu_resource_id;
    }
    texture_states_[object_id] = TextureState{
        texture,
        original_gpu_resource_id,
        replay_gpu_resource_id,
        static_cast<std::uint64_t>(texture.width),
        static_cast<std::uint64_t>(texture.height)};
    return true;
  }

  bool create_sampler(std::uint64_t object_id, const json &descriptor)
  {
    auto *sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.borderColor = static_cast<MTLSamplerBorderColor>(descriptor.value("border_color", 0u));
    sampler_descriptor.rAddressMode = static_cast<MTLSamplerAddressMode>(descriptor.value("r_address_mode", 0u));
    sampler_descriptor.sAddressMode = static_cast<MTLSamplerAddressMode>(descriptor.value("s_address_mode", 0u));
    sampler_descriptor.tAddressMode = static_cast<MTLSamplerAddressMode>(descriptor.value("t_address_mode", 0u));
    sampler_descriptor.magFilter = static_cast<MTLSamplerMinMagFilter>(descriptor.value("mag_filter", 0u));
    sampler_descriptor.minFilter = static_cast<MTLSamplerMinMagFilter>(descriptor.value("min_filter", 0u));
    sampler_descriptor.mipFilter = static_cast<MTLSamplerMipFilter>(descriptor.value("mip_filter", 0u));
    sampler_descriptor.compareFunction = static_cast<MTLCompareFunction>(descriptor.value("compare_function", 0u));
    sampler_descriptor.lodMaxClamp = descriptor.value("lod_max_clamp", 0.0);
    sampler_descriptor.lodMinClamp = descriptor.value("lod_min_clamp", 0.0);
    sampler_descriptor.maxAnisotropy = static_cast<NSUInteger>(descriptor.value("max_anisotropy", 0u));
    sampler_descriptor.lodAverage = descriptor.value("lod_average", false);
    sampler_descriptor.normalizedCoordinates = descriptor.value("normalized_coordinates", true);
    sampler_descriptor.supportArgumentBuffers = descriptor.value("support_argument_buffers", true);

    id<MTLSamplerState> sampler = [device_ newSamplerStateWithDescriptor:sampler_descriptor];
    if (sampler == nil) {
      return fail("failed to create MTLSamplerState");
    }
    [samplers_ setObject:sampler forKey:object_key(object_id)];
    const auto original_gpu_resource_id = original_sampler_gpu_resource_ids_[object_id];
    const auto replay_gpu_resource_id = gpu_resource_id_for_sampler(sampler);
    if (original_gpu_resource_id != 0 && replay_gpu_resource_id != 0) {
      sampler_gpu_resource_id_map_[original_gpu_resource_id] = replay_gpu_resource_id;
    }
    sampler_states_[object_id] = SamplerState{sampler, original_gpu_resource_id, replay_gpu_resource_id};
    return true;
  }

  bool create_texture_view(const json &descriptor)
  {
    const auto object_id = descriptor.value("texture_id", 0ull);
    const auto source_texture_id = descriptor.value("source_texture_id", 0ull);
    id<MTLTexture> source_texture = texture_for_id(source_texture_id);
    if (object_id == 0 || source_texture == nil) {
      return true;
    }

    id<MTLTexture> texture = [source_texture newTextureViewWithPixelFormat:static_cast<MTLPixelFormat>(descriptor.value("pixel_format", 0u))
                                                               textureType:static_cast<MTLTextureType>(descriptor.value("texture_type", 0u))
                                                                    levels:NSMakeRange(static_cast<NSUInteger>(descriptor.value("level_start", 0u)),
                                                                                       static_cast<NSUInteger>(descriptor.value("level_count", 1u)))
                                                                    slices:NSMakeRange(static_cast<NSUInteger>(descriptor.value("slice_start", 0u)),
                                                                                       static_cast<NSUInteger>(descriptor.value("slice_count", 1u)))
                                                                   swizzle:swizzle_channels_from_json_array(descriptor["swizzle"])];
    if (texture == nil) {
      return fail("failed to create MTLTexture view");
    }
    [textures_ setObject:texture forKey:object_key(object_id)];
    const auto original_gpu_resource_id = descriptor.value("gpu_resource_id", original_texture_gpu_resource_ids_[object_id]);
    const auto replay_gpu_resource_id = gpu_resource_id_for_texture(texture);
    if (original_gpu_resource_id != 0 && replay_gpu_resource_id != 0) {
      texture_gpu_resource_id_map_[original_gpu_resource_id] = replay_gpu_resource_id;
    }
    const auto source_state_it = texture_states_.find(source_texture_id);
    const auto width = source_state_it == texture_states_.end()
                           ? static_cast<std::uint64_t>(texture.width)
                           : source_state_it->second.width;
    const auto height = source_state_it == texture_states_.end()
                            ? static_cast<std::uint64_t>(texture.height)
                            : source_state_it->second.height;
    texture_states_[object_id] = TextureState{
        texture,
        original_gpu_resource_id,
        replay_gpu_resource_id,
        width,
        height};
    return true;
  }

  bool create_depth_stencil_state(const json &descriptor)
  {
    const auto object_id = descriptor.value("depth_stencil_state_id", 0ull);
    if (object_id == 0) {
      return true;
    }

    auto *state_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    state_descriptor.depthCompareFunction =
        static_cast<MTLCompareFunction>(descriptor.value("depth_compare_function", 0u));
    state_descriptor.depthWriteEnabled = descriptor.value("depth_write_enabled", false);
    if (descriptor.contains("front_stencil")) {
      apply_stencil_descriptor(state_descriptor.frontFaceStencil, descriptor["front_stencil"]);
    }
    if (descriptor.contains("back_stencil")) {
      apply_stencil_descriptor(state_descriptor.backFaceStencil, descriptor["back_stencil"]);
    }

    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:state_descriptor];
    if (state == nil) {
      return fail("failed to create MTLDepthStencilState");
    }
    [depth_stencil_states_ setObject:state forKey:object_key(object_id)];
    return true;
  }

  bool create_render_pipeline(std::uint64_t object_id, const json &payload)
  {
    const auto descriptor_path = payload.value("descriptor_path", std::string());
    const auto descriptor_bytes = read_binary_file(bundle_path(descriptor_path));
    const auto descriptor = parse_json(std::string(descriptor_bytes.begin(), descriptor_bytes.end()));
    const auto library_id = descriptor.value("library_id", 0ull);
    const auto vertex_library_id = descriptor.value("vertex_library_id", library_id);
    const auto fragment_library_id = descriptor.value("fragment_library_id", library_id);
    const auto vertex_function_name = descriptor.value("vertex_function", std::string());
    const auto fragment_function_name = descriptor.value("fragment_function", std::string());
    const auto fragment_function_constants = descriptor.value("fragment_function_constants", json::array());
    const auto rasterization_enabled = descriptor.value("rasterization_enabled", true);
    id<MTLLibrary> vertex_library = library_for_id(vertex_library_id);
    id<MTLLibrary> fragment_library = fragment_function_name.empty() ? nil : library_for_id(fragment_library_id);
    if (vertex_library == nil || (!fragment_function_name.empty() && fragment_library == nil)) {
      return fail("render pipeline references missing library");
    }

    auto *pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    NSError *function_error = nil;
    pipeline_descriptor.vertexFunction = new_function(vertex_library, vertex_function_name, json::array(), &function_error);
    if (pipeline_descriptor.vertexFunction == nil) {
      return fail(function_error ? [[function_error localizedDescription] UTF8String] : "failed to create vertex function");
    }
    if (!fragment_function_name.empty()) {
      function_error = nil;
      pipeline_descriptor.fragmentFunction =
          new_function(fragment_library, fragment_function_name, fragment_function_constants, &function_error);
      if (pipeline_descriptor.fragmentFunction == nil) {
        return fail(function_error ? [[function_error localizedDescription] UTF8String] : "failed to create fragment function");
      }
    }
    const auto colors = descriptor.find("colors");
    if (colors != descriptor.end() && colors->is_array()) {
      std::size_t slot = 0;
      for (const auto &color : *colors) {
        if (slot >= 8) {
          break;
        }
        auto *attachment = pipeline_descriptor.colorAttachments[slot++];
        attachment.pixelFormat = pixel_format_from_json_field_or(color, "pixel_format", MTLPixelFormatInvalid);
        attachment.blendingEnabled = color.value("blending_enabled", false);
        attachment.writeMask = static_cast<MTLColorWriteMask>(color.value("write_mask", 0u));
        attachment.rgbBlendOperation = static_cast<MTLBlendOperation>(color.value("rgb_blend_operation", 0u));
        attachment.alphaBlendOperation = static_cast<MTLBlendOperation>(color.value("alpha_blend_operation", 0u));
        attachment.sourceRGBBlendFactor = static_cast<MTLBlendFactor>(color.value("src_rgb_blend_factor", 1u));
        attachment.destinationRGBBlendFactor = static_cast<MTLBlendFactor>(color.value("dst_rgb_blend_factor", 0u));
        attachment.sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(color.value("src_alpha_blend_factor", 1u));
        attachment.destinationAlphaBlendFactor = static_cast<MTLBlendFactor>(color.value("dst_alpha_blend_factor", 0u));
      }
    } else {
      pipeline_descriptor.colorAttachments[0].pixelFormat = pixel_format_from_json_field(descriptor, "color_pixel_format");
    }
    pipeline_descriptor.rasterizationEnabled = rasterization_enabled;
    pipeline_descriptor.depthAttachmentPixelFormat =
        pixel_format_from_json_field_or(descriptor, "depth_pixel_format", MTLPixelFormatInvalid);
    pipeline_descriptor.stencilAttachmentPixelFormat =
        pixel_format_from_json_field_or(descriptor, "stencil_pixel_format", MTLPixelFormatInvalid);
    pipeline_descriptor.rasterSampleCount =
        static_cast<NSUInteger>(descriptor.value("raster_sample_count", 1u));

    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [device_ newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
    if (pipeline == nil) {
      return fail(error ? [[error localizedDescription] UTF8String] : "failed to create render pipeline");
    }
    [render_pipelines_ setObject:pipeline forKey:object_key(object_id)];
    return true;
  }

  bool create_compute_pipeline(std::uint64_t object_id, const json &payload)
  {
    const auto descriptor_path = payload.value("descriptor_path", std::string());
    const auto descriptor_bytes = read_binary_file(bundle_path(descriptor_path));
    const auto descriptor = parse_json(std::string(descriptor_bytes.begin(), descriptor_bytes.end()));
    const auto library_id = descriptor.value("library_id", 0ull);
    id<MTLLibrary> library = library_for_id(library_id);
    if (library == nil) {
      return fail("compute pipeline references missing library");
    }

    id<MTLFunction> function =
        [library newFunctionWithName:[NSString stringWithUTF8String:descriptor.value("function", std::string()).c_str()]];
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = [device_ newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) {
      return fail(error ? [[error localizedDescription] UTF8String] : "failed to create compute pipeline");
    }
    [compute_pipelines_ setObject:pipeline forKey:object_key(object_id)];
    return true;
  }

  bool begin_command_buffer(const trace::MetalEventRecord &event)
  {
    id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
    if (command_buffer == nil) {
      return fail("failed to create command buffer");
    }
    [command_buffers_ setObject:command_buffer forKey:object_key(event.object_id)];
    return true;
  }

  bool commit_command_buffer(const trace::MetalEventRecord &event)
  {
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(event.object_id);
    if (command_buffer == nil) {
      return fail("command buffer commit references missing command buffer");
    }

    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (debug_gpu_patch_enabled()) {
      if (command_buffer.status == MTLCommandBufferStatusError && command_buffer.error != nil) {
        std::fprintf(stderr, "metal replay debug command buffer error: %s\n", [[command_buffer.error localizedDescription] UTF8String]);
      }
    }

    NSDictionary *present_info = [pending_presents_ objectForKey:object_key(event.object_id)];
    if (present_info != nil) {
      if (!capture_present_frame(present_info)) {
        return false;
      }
      [pending_presents_ removeObjectForKey:object_key(event.object_id)];
    }

    [command_buffer_present_textures_ removeObjectForKey:object_key(event.object_id)];
    [command_buffers_ removeObjectForKey:object_key(event.object_id)];
    return true;
  }

  bool begin_render_encoder(const trace::MetalEventRecord &event, const json &payload)
  {
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(payload.value("command_buffer_id", 0ull));
    if (command_buffer == nil && !event.object_refs.empty()) {
      command_buffer = command_buffer_for_id(event.object_refs.front());
    }
    if (command_buffer == nil) {
      return fail("render encoder references missing command buffer");
    }

    auto pass = parse_nested_json(payload, "render_pass_info");
    if (pass.contains("render_pass_info")) {
      pass = parse_nested_json(pass, "render_pass_info");
    }
    auto *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    std::uint64_t color_texture_id = pass.value("color_texture_id", pass.value("drawable_id", 0ull));
    if (color_texture_id == 0) {
      const auto colors = pass.find("colors");
      if (colors != pass.end() && colors->is_array() && !colors->empty()) {
        const auto &first_color = (*colors)[0];
        color_texture_id = first_color.value("texture", 0ull);
        if (color_texture_id == 0) {
          color_texture_id = first_color.value("resolve_texture", 0ull);
        }
        if (color_texture_id != 0) {
          pass["color_texture_id"] = color_texture_id;
          pass["load_action"] = first_color.value("load_action", 2u);
          pass["store_action"] = first_color.value("store_action", 1u);
          pass["clear_color"] = first_color.value("clear_color", std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
        }
      }
    }
    if (color_texture_id == 0) {
      return fail("render pass is missing color_texture_id");
    }

    const auto command_buffer_id =
        payload.value("command_buffer_id", !event.object_refs.empty() ? event.object_refs.front() : 0ull);
    if (command_buffer_id != 0 &&
        pass.value("render_target_width", 0u) == 0 &&
        pass.value("render_target_height", 0u) == 0) {
      NSDictionary *present_size = [command_buffer_present_sizes_ objectForKey:object_key(command_buffer_id)];
      if (present_size != nil) {
        pass["render_target_width"] = static_cast<std::uint32_t>([present_size[@"width"] unsignedIntValue]);
        pass["render_target_height"] = static_cast<std::uint32_t>([present_size[@"height"] unsignedIntValue]);
      }
    }
    if (command_buffer_id != 0) {
      [command_buffer_present_textures_ setObject:object_key(color_texture_id)
                                           forKey:object_key(command_buffer_id)];
    }

    id<MTLTexture> first_color_texture = nil;
    const auto colors = pass.find("colors");
    if (colors != pass.end() && colors->is_array() && !colors->empty()) {
      for (const auto &color : *colors) {
        const auto slot = static_cast<NSUInteger>(color.value("slot", 0u));
        const auto texture_id = optional_object_id_from_json_field(color, "texture");
        if (texture_id == 0) {
          continue;
        }
        id<MTLTexture> color_texture = texture_for_id(texture_id);
        if (color_texture == nil) {
          auto color_descriptor = pass;
          color_descriptor["color_texture_id"] = texture_id;
          if (!create_texture(texture_id, color_descriptor)) {
            return false;
          }
          color_texture = texture_for_id(texture_id);
        }
        if (first_color_texture == nil) {
          first_color_texture = color_texture;
        }

        auto *attachment = descriptor.colorAttachments[slot];
        attachment.texture = color_texture;
        attachment.loadAction = load_action_from_json_field(color, "load_action");
        attachment.storeAction = store_action_from_json_field(color, "store_action");
        attachment.level = static_cast<NSUInteger>(color.value("level", 0u));
        attachment.slice = static_cast<NSUInteger>(color.value("slice", 0u));
        attachment.depthPlane = static_cast<NSUInteger>(color.value("depth_plane", 0u));
        const auto clear = color.value("clear_color", pass.value("clear_color", std::array<double, 4>{0.0, 0.0, 0.0, 1.0}));
        attachment.clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);

        const auto resolve_texture_id = optional_object_id_from_json_field(color, "resolve_texture");
        if (resolve_texture_id != 0) {
          id<MTLTexture> resolve_texture = texture_for_id(resolve_texture_id);
          if (resolve_texture == nil) {
            return fail("render pass references missing resolve texture");
          }
          attachment.resolveTexture = resolve_texture;
        }
      }
    } else {
      id<MTLTexture> color_texture = texture_for_id(color_texture_id);
      if (color_texture == nil) {
        if (!create_texture(color_texture_id, pass)) {
          return false;
        }
        color_texture = texture_for_id(color_texture_id);
      }
      first_color_texture = color_texture;

      descriptor.colorAttachments[0].texture = color_texture;
      descriptor.colorAttachments[0].loadAction = load_action_from_json_field(pass, "load_action");
      descriptor.colorAttachments[0].storeAction = store_action_from_json_field(pass, "store_action");

      const auto clear = pass.value("clear_color", std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
      descriptor.colorAttachments[0].clearColor =
          MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
    }

    const auto depth = pass.find("depth");
    if (depth != pass.end() && depth->is_object()) {
      const auto depth_texture_id = optional_object_id_from_json_field(*depth, "texture");
      if (depth_texture_id != 0) {
        id<MTLTexture> depth_texture = texture_for_id(depth_texture_id);
        if (depth_texture == nil) {
          return fail("render pass references missing depth texture");
        }
        descriptor.depthAttachment.texture = depth_texture;
        descriptor.depthAttachment.loadAction = load_action_from_json_field(*depth, "load_action");
        descriptor.depthAttachment.storeAction = store_action_from_json_field(*depth, "store_action");
        descriptor.depthAttachment.clearDepth = depth->value("clear_depth", 1.0);
        descriptor.depthAttachment.level = static_cast<NSUInteger>(depth->value("level", 0u));
        descriptor.depthAttachment.slice = static_cast<NSUInteger>(depth->value("slice", 0u));
        descriptor.depthAttachment.depthPlane = static_cast<NSUInteger>(depth->value("depth_plane", 0u));
      }
    }

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (encoder == nil) {
      return fail("failed to create render command encoder");
    }
    auto default_width = static_cast<std::uint64_t>(first_color_texture.width);
    auto default_height = static_cast<std::uint64_t>(first_color_texture.height);
    const auto first_color_texture_id = first_color_texture == nil ? 0 : color_texture_id;
    const auto texture_state_it = texture_states_.find(first_color_texture_id);
    if (texture_state_it != texture_states_.end()) {
      default_width = texture_state_it->second.width;
      default_height = texture_state_it->second.height;
    }
    [encoder setViewport:{0.0, 0.0, static_cast<double>(default_width), static_cast<double>(default_height), 0.0, 1.0}];
    [encoder setScissorRect:MTLScissorRect{0, 0, static_cast<NSUInteger>(default_width), static_cast<NSUInteger>(default_height)}];
    [render_encoders_ setObject:encoder forKey:object_key(event.object_id)];
    [render_encoder_command_buffers_ setObject:object_key(command_buffer_id) forKey:object_key(event.object_id)];
    encoder_resource_states_[event.object_id] = {};
    render_vertex_buffer_bindings_[event.object_id] = {};
    render_fragment_buffer_bindings_[event.object_id] = {};
    return true;
  }

  bool begin_compute_encoder(const trace::MetalEventRecord &event, const json &payload)
  {
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(payload.value("command_buffer_id", 0ull));
    if (command_buffer == nil && !event.object_refs.empty()) {
      command_buffer = command_buffer_for_id(event.object_refs.front());
    }
    if (command_buffer == nil) {
      return fail("compute encoder references missing command buffer");
    }

    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return fail("failed to create compute command encoder");
    }
    [compute_encoders_ setObject:encoder forKey:object_key(event.object_id)];
    encoder_resource_states_[event.object_id] = {};
    compute_buffer_bindings_[event.object_id] = {};
    return true;
  }

  bool begin_blit_encoder(const trace::MetalEventRecord &event, const json &payload)
  {
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(payload.value("command_buffer_id", 0ull));
    if (command_buffer == nil && !event.object_refs.empty()) {
      command_buffer = command_buffer_for_id(event.object_refs.front());
    }
    if (command_buffer == nil) {
      return fail("blit encoder references missing command buffer");
    }

    id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
    if (encoder == nil) {
      return fail("failed to create blit command encoder");
    }
    [blit_encoders_ setObject:encoder forKey:object_key(event.object_id)];
    return true;
  }

  bool end_render_encoder(std::uint64_t encoder_id)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("render encoder end references missing encoder");
    }
    [encoder endEncoding];
    [render_encoders_ removeObjectForKey:object_key(encoder_id)];
    [render_encoder_command_buffers_ removeObjectForKey:object_key(encoder_id)];
    encoder_resource_states_.erase(encoder_id);
    render_vertex_buffer_bindings_.erase(encoder_id);
    render_fragment_buffer_bindings_.erase(encoder_id);
    return true;
  }

  bool end_compute_encoder(std::uint64_t encoder_id)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("compute encoder end references missing encoder");
    }
    [encoder endEncoding];
    [compute_encoders_ removeObjectForKey:object_key(encoder_id)];
    encoder_resource_states_.erase(encoder_id);
    compute_buffer_bindings_.erase(encoder_id);
    return true;
  }

  bool end_blit_encoder(std::uint64_t encoder_id)
  {
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("blit encoder end references missing encoder");
    }
    [encoder endEncoding];
    [blit_encoders_ removeObjectForKey:object_key(encoder_id)];
    return true;
  }

  bool set_render_pipeline_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLRenderPipelineState> pipeline = render_pipeline_for_id(payload.value("pipeline_state_id", 0ull));
    if (encoder == nil || pipeline == nil) {
      return fail("setRenderPipelineState references missing encoder or pipeline");
    }
    [encoder setRenderPipelineState:pipeline];
    return true;
  }

  bool set_vertex_buffer(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLBuffer> buffer = buffer_for_id(payload.value("buffer_id", 0ull));
    if (encoder == nil || buffer == nil) {
      return fail("setVertexBuffer references missing encoder or buffer");
    }
    [encoder setVertexBuffer:buffer
                      offset:static_cast<NSUInteger>(payload.value("offset", 0ull))
                     atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    record_buffer_binding(render_vertex_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_vertex_bytes(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setVertexBytes references missing encoder");
    }

    const auto nested = parse_nested_json(payload, "payload");
    const auto bytes = bytes_from_json_array(nested, "bytes");
    if (bytes.empty()) {
      return true;
    }

    [encoder setVertexBytes:bytes.data()
                     length:static_cast<NSUInteger>(nested.value("length", static_cast<std::uint64_t>(bytes.size())))
                    atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    return true;
  }

  bool set_vertex_buffer_offset(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setVertexBufferOffset references missing encoder");
    }
    [encoder setVertexBufferOffset:static_cast<NSUInteger>(payload.value("offset", 0ull))
                           atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    record_buffer_offset(render_vertex_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_fragment_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLTexture> texture = texture_for_id(payload.value("texture_id", 0ull));
    if (encoder == nil || texture == nil) {
      return fail("setFragmentTexture references missing encoder or texture");
    }
    [encoder setFragmentTexture:texture atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    return true;
  }

  bool set_fragment_buffer(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLBuffer> buffer = buffer_for_id(payload.value("buffer_id", 0ull));
    if (encoder == nil || buffer == nil) {
      return fail("setFragmentBuffer references missing encoder or buffer");
    }
    [encoder setFragmentBuffer:buffer
                        offset:static_cast<NSUInteger>(payload.value("offset", 0ull))
                       atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    record_buffer_binding(render_fragment_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_fragment_bytes(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setFragmentBytes references missing encoder");
    }

    const auto nested = parse_nested_json(payload, "payload");
    const auto bytes = bytes_from_json_array(nested, "bytes");
    if (bytes.empty()) {
      return true;
    }

    [encoder setFragmentBytes:bytes.data()
                       length:static_cast<NSUInteger>(nested.value("length", static_cast<std::uint64_t>(bytes.size())))
                      atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    return true;
  }

  bool set_fragment_buffer_offset(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setFragmentBufferOffset references missing encoder");
    }
    [encoder setFragmentBufferOffset:static_cast<NSUInteger>(payload.value("offset", 0ull))
                             atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    record_buffer_offset(render_fragment_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool use_resource(std::uint64_t encoder_id, const json &payload)
  {
    const auto resource_id = payload.value("resource_id", 0ull);
    id<MTLResource> resource = resource_for_id(resource_id);
    ActiveResource active_resource;
    const auto buffer_state_it = buffer_states_.find(resource_id);
    if (buffer_state_it != buffer_states_.end()) {
      active_resource.is_buffer = true;
      active_resource.buffer = buffer_state_it->second;
    }
    const auto texture_state_it = texture_states_.find(resource_id);
    if (texture_state_it != texture_states_.end()) {
      active_resource.is_texture = true;
      active_resource.texture = texture_state_it->second;
    }
    if (!active_resource.is_texture) {
      const auto original_it = original_texture_gpu_resource_ids_.find(resource_id);
      if (original_it != original_texture_gpu_resource_ids_.end()) {
        active_resource.is_texture = true;
        active_resource.texture.original_gpu_resource_id = original_it->second;
        const auto mapped_it = texture_gpu_resource_id_map_.find(original_it->second);
        active_resource.texture.replay_gpu_resource_id = mapped_it == texture_gpu_resource_id_map_.end() ? 0 : mapped_it->second;
      }
    }
    if (active_resource.is_buffer || active_resource.is_texture || active_resource.is_sampler) {
      auto &state = encoder_resource_states_[encoder_id];
      state.active.push_back(active_resource);
      state.draw_scope.push_back(active_resource);
    }

    if (resource == nil) {
      return true;
    }

    const auto usage = static_cast<MTLResourceUsage>(payload.value("usage", 1u));
    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(encoder_id);
    if (render_encoder != nil) {
      [render_encoder useResource:resource
                            usage:usage
                           stages:static_cast<MTLRenderStages>(payload.value("stages", 0u))];
      return true;
    }

    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(encoder_id);
    if (compute_encoder != nil) {
      [compute_encoder useResource:resource usage:usage];
      return true;
    }

    return true;
  }

  void finish_draw_scope(std::uint64_t encoder_id)
  {
    const auto it = encoder_resource_states_.find(encoder_id);
    if (it != encoder_resource_states_.end()) {
      it->second.draw_scope.clear();
    }
  }

  bool set_viewport(std::uint64_t encoder_id, const json &payload)
  {
    return set_viewports(encoder_id, parse_nested_json(payload, "payload"));
  }

  bool set_viewports(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setViewport references missing encoder");
    }

    const auto it = payload.find("viewports");
    if (it == payload.end() || !it->is_array() || it->empty()) {
      return true;
    }

    std::vector<MTLViewport> viewports;
    viewports.reserve(it->size());
    for (const auto &viewport : *it) {
      viewports.push_back(viewport_from_json_array(viewport));
    }
    [encoder setViewports:viewports.data() count:viewports.size()];
    return true;
  }

  bool set_scissor_rect(std::uint64_t encoder_id, const json &payload)
  {
    return set_scissor_rects(encoder_id, parse_nested_json(payload, "payload"));
  }

  bool set_scissor_rects(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setScissorRect references missing encoder");
    }

    const auto it = payload.find("rects");
    if (it == payload.end() || !it->is_array() || it->empty()) {
      return true;
    }

    std::vector<MTLScissorRect> rects;
    rects.reserve(it->size());
    for (const auto &rect : *it) {
      rects.push_back(scissor_from_json_array(rect));
    }
    [encoder setScissorRects:rects.data() count:rects.size()];
    return true;
  }

  bool set_rasterizer_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("set rasterizer state references missing encoder");
    }
    [encoder setTriangleFillMode:static_cast<MTLTriangleFillMode>(payload.value("fill_mode", 0u))];
    [encoder setCullMode:static_cast<MTLCullMode>(payload.value("cull_mode", 0u))];
    [encoder setDepthClipMode:static_cast<MTLDepthClipMode>(payload.value("depth_clip_mode", 0u))];
    [encoder setDepthBias:payload.value("depth_bias", 0.0f)
               slopeScale:payload.value("slope_scale", 0.0f)
                    clamp:payload.value("depth_bias_clamp", 0.0f)];
    [encoder setFrontFacingWinding:static_cast<MTLWinding>(payload.value("winding", 0u))];
    return true;
  }

  bool set_depth_stencil_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("set depth stencil state references missing encoder");
    }
    id<MTLDepthStencilState> state =
        depth_stencil_state_for_id(payload.value("depth_stencil_state_id", 0ull));
    if (state != nil) {
      [encoder setDepthStencilState:state];
    }
    [encoder setStencilReferenceValue:static_cast<std::uint32_t>(payload.value("stencil_ref", 0u))];
    return true;
  }

  bool set_blend_factor(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("set blend factor references missing encoder");
    }
    [encoder setBlendColorRed:payload.value("red", 0.0f)
                        green:payload.value("green", 0.0f)
                         blue:payload.value("blue", 0.0f)
                        alpha:payload.value("alpha", 0.0f)];
    [encoder setStencilReferenceValue:static_cast<std::uint32_t>(payload.value("stencil_ref", 0u))];
    return true;
  }

  bool set_compute_pipeline_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    id<MTLComputePipelineState> pipeline = compute_pipeline_for_id(payload.value("pipeline_state_id", 0ull));
    if (encoder == nil || pipeline == nil) {
      return fail("setComputePipelineState references missing encoder or pipeline");
    }
    [encoder setComputePipelineState:pipeline];
    return true;
  }

  bool set_compute_buffer(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    id<MTLBuffer> buffer = buffer_for_id(payload.value("buffer_id", 0ull));
    if (encoder == nil || buffer == nil) {
      return fail("setComputeBuffer references missing encoder or buffer");
    }
    [encoder setBuffer:buffer
                offset:static_cast<NSUInteger>(payload.value("offset", 0ull))
               atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    record_buffer_binding(compute_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_compute_buffer_offset(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setComputeBufferOffset references missing compute encoder");
    }
    [encoder setBufferOffset:static_cast<NSUInteger>(payload.value("offset", 0ull))
                     atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    record_buffer_offset(compute_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool draw_primitives(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("drawPrimitives references missing render encoder");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawPrimitives:primitive_type_from_integer(payload.value("primitive_type", 0u))
                vertexStart:static_cast<NSUInteger>(payload.value("vertex_start", 0u))
                vertexCount:static_cast<NSUInteger>(payload.value("vertex_count", 0u))
              instanceCount:static_cast<NSUInteger>(payload.value("instance_count", 1u))
               baseInstance:static_cast<NSUInteger>(payload.value("base_instance", 0u))];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool draw_indexed_primitives(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLBuffer> index_buffer = buffer_for_id(payload.value("index_buffer_id", 0ull));
    if (encoder == nil || index_buffer == nil) {
      return fail("drawIndexedPrimitives references missing encoder or index buffer");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawIndexedPrimitives:primitive_type_from_integer(payload.value("primitive_type", 0u))
                         indexCount:static_cast<NSUInteger>(payload.value("index_count", 0u))
                          indexType:index_type_from_integer(payload.value("index_type", 0u))
                        indexBuffer:index_buffer
                  indexBufferOffset:static_cast<NSUInteger>(payload.value("index_buffer_offset", 0ull))
                      instanceCount:static_cast<NSUInteger>(payload.value("instance_count", 1u))
                         baseVertex:payload.value("base_vertex", 0)
                       baseInstance:static_cast<NSUInteger>(payload.value("base_instance", 0u))];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool draw_primitives_indirect(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLBuffer> indirect_buffer = buffer_for_id(payload.value("indirect_buffer_id", 0ull));
    if (encoder == nil || indirect_buffer == nil) {
      return fail("drawPrimitivesIndirect references missing encoder or indirect buffer");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawPrimitives:primitive_type_from_integer(payload.value("primitive_type", 0u))
             indirectBuffer:indirect_buffer
       indirectBufferOffset:static_cast<NSUInteger>(payload.value("indirect_buffer_offset", 0ull))];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool dispatch_threadgroups(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("dispatchThreadgroups references missing compute encoder");
    }
    patch_compute_bound_buffers(encoder_id);
    [encoder dispatchThreadgroups:MTLSizeMake(payload.value("tgx", 1u), payload.value("tgy", 1u), payload.value("tgz", 1u))
          threadsPerThreadgroup:MTLSizeMake(payload.value("tx", 1u), payload.value("ty", 1u), payload.value("tz", 1u))];
    return true;
  }

  bool dispatch_threadgroups_indirect(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    id<MTLBuffer> indirect_buffer = buffer_for_id(payload.value("indirect_buffer_id", 0ull));
    if (encoder == nil || indirect_buffer == nil) {
      return fail("dispatchThreadgroupsIndirect references missing encoder or indirect buffer");
    }
    patch_compute_bound_buffers(encoder_id);
    [encoder dispatchThreadgroupsWithIndirectBuffer:indirect_buffer
                               indirectBufferOffset:static_cast<NSUInteger>(payload.value("indirect_buffer_offset", 0ull))
                              threadsPerThreadgroup:MTLSizeMake(payload.value("tx", 1u), payload.value("ty", 1u), payload.value("tz", 1u))];
    return true;
  }

  bool dispatch_threads(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("dispatchThreads references missing compute encoder");
    }
    patch_compute_bound_buffers(encoder_id);
    [encoder dispatchThreads:MTLSizeMake(payload.value("width", 1u), payload.value("height", 1u), payload.value("depth", 1u))
        threadsPerThreadgroup:MTLSizeMake(payload.value("threads_per_group_width", 1u),
                                          payload.value("threads_per_group_height", 1u),
                                          payload.value("threads_per_group_depth", 1u))];
    return true;
  }

  bool set_compute_bytes(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setComputeBytes references missing compute encoder");
    }
    const auto bytes = bytes_from_json_array(payload, "bytes");
    if (bytes.empty()) {
      return true;
    }
    [encoder setBytes:bytes.data()
               length:static_cast<NSUInteger>(payload.value("length", static_cast<std::uint64_t>(bytes.size())))
              atIndex:static_cast<NSUInteger>(payload.value("index", 0u))];
    return true;
  }

  bool copy_buffer(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    id<MTLBuffer> source_buffer = buffer_for_id(payload.value("source_buffer_id", 0ull));
    id<MTLBuffer> destination_buffer = buffer_for_id(payload.value("destination_buffer_id", 0ull));
    if (encoder == nil || source_buffer == nil || destination_buffer == nil) {
      return fail("copyFromBuffer references missing encoder or buffers");
    }
    [encoder copyFromBuffer:source_buffer
               sourceOffset:static_cast<NSUInteger>(payload.value("source_offset", 0ull))
                   toBuffer:destination_buffer
          destinationOffset:static_cast<NSUInteger>(payload.value("destination_offset", 0ull))
                       size:static_cast<NSUInteger>(payload.value("size", 0ull))];
    return true;
  }

  bool copy_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    id<MTLTexture> source_texture = texture_for_id(payload.value("source_texture_id", 0ull));
    id<MTLTexture> destination_texture = texture_for_id(payload.value("destination_texture_id", 0ull));
    if (encoder == nil || source_texture == nil || destination_texture == nil) {
      return fail("copyFromTexture references missing encoder or textures");
    }

    const auto nested = parse_nested_json(payload, "payload");
    [encoder copyFromTexture:source_texture
                 sourceSlice:static_cast<NSUInteger>(nested.value("source_slice", 0u))
                 sourceLevel:static_cast<NSUInteger>(nested.value("source_level", 0u))
                sourceOrigin:origin_from_json_array(nested, "source_origin")
                  sourceSize:size_from_json_array(nested, "source_size", source_texture)
                   toTexture:destination_texture
            destinationSlice:static_cast<NSUInteger>(nested.value("destination_slice", 0u))
            destinationLevel:static_cast<NSUInteger>(nested.value("destination_level", 0u))
           destinationOrigin:origin_from_json_array(nested, "destination_origin")];
    return true;
  }

  bool copy_buffer_to_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    id<MTLBuffer> source_buffer = buffer_for_id(payload.value("source_buffer", 0ull));
    id<MTLTexture> destination_texture = texture_for_id(payload.value("destination_texture", 0ull));
    if (encoder == nil || source_buffer == nil || destination_texture == nil) {
      return fail("copyFromBuffer toTexture references missing encoder, buffer, or texture");
    }

    [encoder copyFromBuffer:source_buffer
               sourceOffset:static_cast<NSUInteger>(payload.value("source_offset", 0ull))
          sourceBytesPerRow:static_cast<NSUInteger>(payload.value("source_bytes_per_row", 0u))
        sourceBytesPerImage:static_cast<NSUInteger>(payload.value("source_bytes_per_image", 0u))
                 sourceSize:size_from_json_array(payload, "source_size", destination_texture)
                  toTexture:destination_texture
           destinationSlice:static_cast<NSUInteger>(payload.value("destination_slice", 0u))
           destinationLevel:static_cast<NSUInteger>(payload.value("destination_level", 0u))
          destinationOrigin:origin_from_json_array(payload, "destination_origin")];
    return true;
  }

  bool handle_debug_signpost(std::uint64_t object_id, const json &payload)
  {
    const json signpost = parse_json(payload.value("label", std::string()));
    const auto kind = signpost.value("kind", std::string());
    if (kind == "dxmt_buffer_gpu_address") {
      const auto buffer_id = signpost.value("buffer_id", object_id);
      const auto gpu_address = signpost.value("gpu_address", 0ull);
      if (buffer_id != 0 && gpu_address != 0) {
        pending_buffer_gpu_addresses_[buffer_id] = gpu_address;
      }
      return true;
    }
    if (kind == "dxmt_copy_buffer_to_texture") {
      return copy_buffer_to_texture(object_id, signpost);
    }
    if (kind == "dxmt_dispatch_threads") {
      return dispatch_threads(object_id, signpost);
    }
    if (kind == "dxmt_set_compute_bytes") {
      return set_compute_bytes(object_id, signpost);
    }
    if (kind == "dxmt_sampler_gpu_resource_id") {
      return create_sampler(signpost.value("sampler_id", object_id), signpost);
    }
    if (kind == "dxmt_texture_view") {
      return create_texture_view(signpost);
    }
    if (kind == "dxmt_depth_stencil_state") {
      return create_depth_stencil_state(signpost);
    }
    if (kind == "dxmt_set_rasterizer_state") {
      return set_rasterizer_state(object_id, signpost);
    }
    if (kind == "dxmt_set_depth_stencil_state") {
      return set_depth_stencil_state(object_id, signpost);
    }
    if (kind == "dxmt_set_blend_factor") {
      return set_blend_factor(object_id, signpost);
    }
    if (kind == "dxmt_set_viewports") {
      return set_viewports(object_id, signpost);
    }
    if (kind == "dxmt_set_scissor_rects") {
      return set_scissor_rects(object_id, signpost);
    }
    return true;
  }

  bool queue_present(std::uint64_t command_buffer_id, const json &payload)
  {
    NSMutableDictionary *info = [[NSMutableDictionary alloc] init];
    info[@"drawable_id"] = object_key(payload.value("drawable_id", 0ull));
    NSNumber *present_texture_id = [command_buffer_present_textures_ objectForKey:object_key(command_buffer_id)];
    if (present_texture_id != nil) {
      info[@"present_texture_id"] = present_texture_id;
    }
    info[@"frame_index"] = object_key(payload.value("frame_index", 0ull));
    info[@"width"] = [NSNumber numberWithUnsignedInt:payload.value("width", 0u)];
    info[@"height"] = [NSNumber numberWithUnsignedInt:payload.value("height", 0u)];
    info[@"sync_interval"] = [NSNumber numberWithUnsignedInt:payload.value("sync_interval", 0u)];
    info[@"flags"] = [NSNumber numberWithUnsignedInt:payload.value("flags", 0u)];
    [pending_presents_ setObject:info forKey:object_key(command_buffer_id)];
    return true;
  }

  bool capture_present_frame(NSDictionary *present_info)
  {
    if (!capture_writer_ || !options_.enable_metal_present_capture) {
      return true;
    }

    const auto present_texture_id =
        static_cast<std::uint64_t>([present_info[@"present_texture_id"] unsignedLongLongValue]);
    id<MTLTexture> texture = texture_for_id(present_texture_id);
    if (texture == nil) {
      const auto drawable_id = static_cast<std::uint64_t>([present_info[@"drawable_id"] unsignedLongLongValue]);
      texture = texture_for_id(drawable_id);
    }
    if (texture == nil) {
      return fail("present references missing drawable texture");
    }

    const auto width = static_cast<std::uint32_t>([present_info[@"width"] unsignedIntValue]);
    const auto height = static_cast<std::uint32_t>([present_info[@"height"] unsignedIntValue]);
    const auto row_pitch = width * 4u;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(height), 0);
    [texture getBytes:bytes.data()
          bytesPerRow:row_pitch
           fromRegion:MTLRegionMake2D(0, 0, width, height)
          mipmapLevel:0];

    trace::AssetRecord asset;
    asset.blob_id = ++capture_sequence_;
    asset.kind = trace::AssetKind::Texture;
    asset.debug_name = "metal-present-frame";
    asset.payload_bytes = bytes;
    asset = capture_writer_->register_asset(asset);

    trace::EventRecord event;
    event.kind = trace::EventKind::ResourceBlob;
    event.callsite.sequence = ++capture_sequence_;
    event.callsite.function_name = "resource_blob";
    event.object_kind = trace::ObjectKind::Unknown;
    event.object_debug_name = "MetalPresentFrame";
    event.blob_refs = {asset.blob_id};
    event.payload = json{{"frame_index", static_cast<std::uint64_t>([present_info[@"frame_index"] unsignedLongLongValue])},
                         {"width", width},
                         {"height", height},
                         {"row_pitch", row_pitch},
                         {"sync_interval", static_cast<std::uint32_t>([present_info[@"sync_interval"] unsignedIntValue])},
                         {"flags", static_cast<std::uint32_t>([present_info[@"flags"] unsignedIntValue])},
                         {"format", present_frame_format_for_texture(texture)},
                         {"frame_path", asset.relative_path.generic_string()}}
                            .dump();
    capture_writer_->append_call_event(event);
    return true;
  }

  const trace::TraceBundleReader *reader_ = nullptr;
  ReplayOptions options_;
  std::string last_error_;
  std::unique_ptr<trace::TraceBundleWriter> capture_writer_;
  std::uint64_t capture_sequence_ = 0;

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLBuffer>> *buffers_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLTexture>> *textures_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *samplers_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLLibrary>> *libraries_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *render_pipelines_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLComputePipelineState>> *compute_pipelines_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *depth_stencil_states_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLCommandBuffer>> *command_buffers_ = nil;
  NSMutableDictionary<NSNumber *, NSDictionary *> *command_buffer_present_sizes_ = nil;
  NSMutableDictionary<NSNumber *, NSNumber *> *command_buffer_present_textures_ = nil;
  NSMutableDictionary<NSNumber *, NSNumber *> *render_encoder_command_buffers_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLRenderCommandEncoder>> *render_encoders_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLComputeCommandEncoder>> *compute_encoders_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLBlitCommandEncoder>> *blit_encoders_ = nil;
  NSMutableDictionary<NSNumber *, NSDictionary *> *pending_presents_ = nil;
  std::unordered_map<std::uint64_t, std::uint64_t> pending_buffer_gpu_addresses_;
  std::unordered_map<std::uint64_t, std::uint64_t> original_texture_gpu_resource_ids_;
  std::unordered_map<std::uint64_t, std::uint64_t> original_sampler_gpu_resource_ids_;
  std::unordered_map<std::uint64_t, std::uint64_t> original_buffer_lengths_;
  std::unordered_map<std::uint64_t, std::uint64_t> buffer_gpu_address_map_;
  std::unordered_map<std::uint64_t, std::uint64_t> texture_gpu_resource_id_map_;
  std::unordered_map<std::uint64_t, std::uint64_t> sampler_gpu_resource_id_map_;
  std::unordered_map<std::uint64_t, BufferState> buffer_states_;
  std::unordered_map<std::uint64_t, TextureState> texture_states_;
  std::unordered_map<std::uint64_t, SamplerState> sampler_states_;
  std::unordered_map<std::uint64_t, EncoderResourceState> encoder_resource_states_;
  std::unordered_map<std::uint64_t, BufferBindings> render_vertex_buffer_bindings_;
  std::unordered_map<std::uint64_t, BufferBindings> render_fragment_buffer_bindings_;
  std::unordered_map<std::uint64_t, BufferBindings> compute_buffer_bindings_;
};

} // namespace

void register_native_metal_replay_backend()
{
  register_metal_replay_backend("native", [] {
    return std::make_unique<NativeMetalReplayBackend>();
  });
}

} // namespace apitrace::replay
