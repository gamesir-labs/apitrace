#include "apitrace/metal_replay_backend_factory.hpp"

#include <nlohmann/json.hpp>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

MTLPixelFormat pixel_format_from_json_field(const json &descriptor, const char *field_name)
{
  const auto it = descriptor.find(field_name);
  if (it == descriptor.end()) {
    return MTLPixelFormatBGRA8Unorm;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    return static_cast<MTLPixelFormat>(it->get<std::uint32_t>());
  }
  if (it->is_string()) {
    return pixel_format_from_string(it->get_ref<const std::string &>());
  }
  return MTLPixelFormatBGRA8Unorm;
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

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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
    libraries_ = [[NSMutableDictionary alloc] init];
    render_pipelines_ = [[NSMutableDictionary alloc] init];
    compute_pipelines_ = [[NSMutableDictionary alloc] init];
    command_buffers_ = [[NSMutableDictionary alloc] init];
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
    case trace::MetalCallKind::SetFragmentTexture:
      return set_fragment_texture(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBuffer:
      return set_fragment_buffer(event.object_id, payload);
    case trace::MetalCallKind::SetComputePipelineState:
      return set_compute_pipeline_state(event.object_id, payload);
    case trace::MetalCallKind::SetComputeBuffer:
      return set_compute_buffer(event.object_id, payload);
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
    case trace::MetalCallKind::PresentDrawable:
      return queue_present(event.object_id, payload);
    case trace::MetalCallKind::UseResource:
    case trace::MetalCallKind::UseResources:
    case trace::MetalCallKind::SetArgumentBuffer:
    case trace::MetalCallKind::UpdateFence:
    case trace::MetalCallKind::WaitForFence:
    case trace::MetalCallKind::MemoryBarrier:
    case trace::MetalCallKind::PushDebugGroup:
    case trace::MetalCallKind::PopDebugGroup:
    case trace::MetalCallKind::InsertDebugSignpost:
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

  std::filesystem::path bundle_path(std::string_view relative_path) const
  {
    return reader_->layout().root_path / std::filesystem::path(relative_path);
  }

  bool fail(std::string message)
  {
    last_error_ = std::move(message);
    return false;
  }

  bool replay_device_create(const trace::MetalEventRecord &event, const json &payload)
  {
    if (event.function_name == "MTLDevice.newBuffer") {
      const auto buffer_path = payload.value("buffer_path", std::string());
      const auto bytes = buffer_path.empty() ? std::vector<std::uint8_t>{} : read_binary_file(bundle_path(buffer_path));
      const auto length = static_cast<NSUInteger>(payload.value("length", static_cast<std::uint64_t>(bytes.size())));
      id<MTLBuffer> buffer = bytes.empty() ? [device_ newBufferWithLength:length options:MTLResourceStorageModeShared]
                                           : [device_ newBufferWithBytes:bytes.data() length:length options:MTLResourceStorageModeShared];
      if (buffer == nil) {
        return fail("failed to create MTLBuffer");
      }
      [buffers_ setObject:buffer forKey:object_key(event.object_id)];
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
    const auto width = static_cast<NSUInteger>(descriptor.value("width", 1u));
    const auto height = static_cast<NSUInteger>(descriptor.value("height", 1u));
    const auto pixel_format = pixel_format_from_json_field(descriptor, "pixel_format");
    auto *texture_descriptor = [[MTLTextureDescriptor alloc] init];
    texture_descriptor.textureType = MTLTextureType2D;
    texture_descriptor.width = std::max<NSUInteger>(width, 1);
    texture_descriptor.height = std::max<NSUInteger>(height, 1);
    texture_descriptor.depth = 1;
    texture_descriptor.arrayLength = 1;
    texture_descriptor.mipmapLevelCount = static_cast<NSUInteger>(descriptor.value("mipmap_level_count", 1u));
    texture_descriptor.pixelFormat = pixel_format;
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;

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
    const auto rasterization_enabled = descriptor.value("rasterization_enabled", true);
    id<MTLLibrary> vertex_library = library_for_id(vertex_library_id);
    id<MTLLibrary> fragment_library = fragment_function_name.empty() ? nil : library_for_id(fragment_library_id);
    if (vertex_library == nil || (!fragment_function_name.empty() && fragment_library == nil)) {
      return fail("render pipeline references missing library");
    }

    auto *pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.vertexFunction =
        [vertex_library newFunctionWithName:[NSString stringWithUTF8String:vertex_function_name.c_str()]];
    if (!fragment_function_name.empty()) {
      pipeline_descriptor.fragmentFunction =
          [fragment_library newFunctionWithName:[NSString stringWithUTF8String:fragment_function_name.c_str()]];
    }
    pipeline_descriptor.colorAttachments[0].pixelFormat = pixel_format_from_json_field(descriptor, "color_pixel_format");
    pipeline_descriptor.rasterizationEnabled = rasterization_enabled;
    pipeline_descriptor.depthAttachmentPixelFormat = pixel_format_from_json_field(descriptor, "depth_pixel_format");
    pipeline_descriptor.stencilAttachmentPixelFormat = pixel_format_from_json_field(descriptor, "stencil_pixel_format");

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

    NSDictionary *present_info = [pending_presents_ objectForKey:object_key(event.object_id)];
    if (present_info != nil) {
      if (!capture_present_frame(present_info)) {
        return false;
      }
      [pending_presents_ removeObjectForKey:object_key(event.object_id)];
    }

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

    id<MTLTexture> color_texture = texture_for_id(color_texture_id);
    if (color_texture == nil) {
      if (!create_texture(color_texture_id, pass)) {
        return false;
      }
      color_texture = texture_for_id(color_texture_id);
    }

    descriptor.colorAttachments[0].texture = color_texture;
    descriptor.colorAttachments[0].loadAction = load_action_from_json_field(pass, "load_action");
    descriptor.colorAttachments[0].storeAction = store_action_from_json_field(pass, "store_action");

    const auto clear = pass.value("clear_color", std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
    descriptor.colorAttachments[0].clearColor =
        MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (encoder == nil) {
      return fail("failed to create render command encoder");
    }
    [render_encoders_ setObject:encoder forKey:object_key(event.object_id)];
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
    return true;
  }

  bool draw_primitives(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("drawPrimitives references missing render encoder");
    }
    [encoder drawPrimitives:primitive_type_from_integer(payload.value("primitive_type", 0u))
                vertexStart:static_cast<NSUInteger>(payload.value("vertex_start", 0u))
                vertexCount:static_cast<NSUInteger>(payload.value("vertex_count", 0u))
              instanceCount:static_cast<NSUInteger>(payload.value("instance_count", 1u))
               baseInstance:static_cast<NSUInteger>(payload.value("base_instance", 0u))];
    return true;
  }

  bool draw_indexed_primitives(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLBuffer> index_buffer = buffer_for_id(payload.value("index_buffer_id", 0ull));
    if (encoder == nil || index_buffer == nil) {
      return fail("drawIndexedPrimitives references missing encoder or index buffer");
    }
    [encoder drawIndexedPrimitives:primitive_type_from_integer(payload.value("primitive_type", 0u))
                         indexCount:static_cast<NSUInteger>(payload.value("index_count", 0u))
                          indexType:index_type_from_integer(payload.value("index_type", 0u))
                        indexBuffer:index_buffer
                  indexBufferOffset:static_cast<NSUInteger>(payload.value("index_buffer_offset", 0ull))
                      instanceCount:static_cast<NSUInteger>(payload.value("instance_count", 1u))
                         baseVertex:payload.value("base_vertex", 0)
                       baseInstance:static_cast<NSUInteger>(payload.value("base_instance", 0u))];
    return true;
  }

  bool draw_primitives_indirect(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    id<MTLBuffer> indirect_buffer = buffer_for_id(payload.value("indirect_buffer_id", 0ull));
    if (encoder == nil || indirect_buffer == nil) {
      return fail("drawPrimitivesIndirect references missing encoder or indirect buffer");
    }
    [encoder drawPrimitives:primitive_type_from_integer(payload.value("primitive_type", 0u))
             indirectBuffer:indirect_buffer
       indirectBufferOffset:static_cast<NSUInteger>(payload.value("indirect_buffer_offset", 0ull))];
    return true;
  }

  bool dispatch_threadgroups(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("dispatchThreadgroups references missing compute encoder");
    }
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
    [encoder dispatchThreadgroupsWithIndirectBuffer:indirect_buffer
                               indirectBufferOffset:static_cast<NSUInteger>(payload.value("indirect_buffer_offset", 0ull))
                              threadsPerThreadgroup:MTLSizeMake(payload.value("tx", 1u), payload.value("ty", 1u), payload.value("tz", 1u))];
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

  bool queue_present(std::uint64_t command_buffer_id, const json &payload)
  {
    NSMutableDictionary *info = [[NSMutableDictionary alloc] init];
    info[@"drawable_id"] = object_key(payload.value("drawable_id", 0ull));
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

    const auto drawable_id = static_cast<std::uint64_t>([present_info[@"drawable_id"] unsignedLongLongValue]);
    id<MTLTexture> texture = texture_for_id(drawable_id);
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
                         {"format", "bgra8"},
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
  NSMutableDictionary<NSNumber *, id<MTLLibrary>> *libraries_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *render_pipelines_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLComputePipelineState>> *compute_pipelines_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLCommandBuffer>> *command_buffers_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLRenderCommandEncoder>> *render_encoders_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLComputeCommandEncoder>> *compute_encoders_ = nil;
  NSMutableDictionary<NSNumber *, id<MTLBlitCommandEncoder>> *blit_encoders_ = nil;
  NSMutableDictionary<NSNumber *, NSDictionary *> *pending_presents_ = nil;
};

} // namespace

void register_native_metal_replay_backend()
{
  register_metal_replay_backend("native", [] {
    return std::make_unique<NativeMetalReplayBackend>();
  });
}

} // namespace apitrace::replay
