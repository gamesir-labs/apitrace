#include "apitrace/metal_replay_backend_factory.hpp"

#include <nlohmann/json.hpp>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#if defined(__APPLE__)
@interface MTLMeshRenderPipelineDescriptor ()
- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(NSUInteger)op;
@end

@interface MTLTileRenderPipelineDescriptor ()
- (void)setThreadgroupSizeMatchesTileSize:(BOOL)enable;
@end
#endif

#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
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

bool required_u64(const json &payload, const char *field_name, std::uint64_t &value)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || (!it->is_number_unsigned() && !it->is_number_integer())) {
    return false;
  }
  if (it->is_number_unsigned()) {
    value = it->get<std::uint64_t>();
    return true;
  }
  const auto signed_value = it->get<std::int64_t>();
  if (signed_value < 0) {
    return false;
  }
  value = static_cast<std::uint64_t>(signed_value);
  return true;
}

bool required_u32(const json &payload, const char *field_name, std::uint32_t &value)
{
  std::uint64_t parsed = 0;
  if (!required_u64(payload, field_name, parsed) || parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool required_i32(const json &payload, const char *field_name, std::int32_t &value)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || (!it->is_number_integer() && !it->is_number_unsigned())) {
    return false;
  }
  if (it->is_number_unsigned()) {
    const auto parsed = it->get<std::uint64_t>();
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
      return false;
    }
    value = static_cast<std::int32_t>(parsed);
    return true;
  }
  const auto parsed = it->get<std::int64_t>();
  if (parsed < std::numeric_limits<std::int32_t>::min() ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  value = static_cast<std::int32_t>(parsed);
  return true;
}

bool validate_sampler_descriptor_payload(
    const json &descriptor,
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
    const auto it = descriptor.find(std::string(field));
    if (it == descriptor.end()) {
      error = "sampler descriptor is missing " + std::string(field);
      return false;
    }
    if (field == "lod_max_clamp" || field == "lod_min_clamp") {
      if (!it->is_number()) {
        error = "sampler descriptor field " + std::string(field) + " must be numeric";
        return false;
      }
    } else if (field == "lod_average" || field == "normalized_coordinates" || field == "support_argument_buffers") {
      if (!it->is_boolean()) {
        error = "sampler descriptor field " + std::string(field) + " must be boolean";
        return false;
      }
    } else if (!it->is_number_unsigned() && !it->is_number_integer()) {
      error = "sampler descriptor field " + std::string(field) + " must be an integer";
      return false;
    }
  }
  if (json_u64(descriptor.value("sampler_id", json(nullptr))) == 0) {
    error = "sampler descriptor has zero sampler_id";
    return false;
  }
  return true;
}

NSNumber *object_key(std::uint64_t object_id)
{
  return [NSNumber numberWithUnsignedLongLong:object_id];
}

bool pixel_format_from_string(std::string_view name, MTLPixelFormat &format)
{
  if (name == "bgra8unorm") {
    format = MTLPixelFormatBGRA8Unorm;
    return true;
  }
  if (name == "rgba8unorm") {
    format = MTLPixelFormatRGBA8Unorm;
    return true;
  }
  if (name == "r32uint") {
    format = MTLPixelFormatR32Uint;
    return true;
  }
  return false;
}

bool pixel_format_from_json_field(const json &descriptor, const char *field_name, MTLPixelFormat &format)
{
  const auto it = descriptor.find(field_name);
  if (it == descriptor.end()) {
    return false;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    format = static_cast<MTLPixelFormat>(it->get<std::uint32_t>());
    return true;
  }
  if (it->is_string()) {
    return pixel_format_from_string(it->get_ref<const std::string &>(), format);
  }
  return false;
}

bool optional_pixel_format_from_json_field(
    const json &descriptor,
    const char *field_name,
    MTLPixelFormat &format)
{
  const auto it = descriptor.find(field_name);
  if (it == descriptor.end() || it->is_null()) {
    format = MTLPixelFormatInvalid;
    return true;
  }
  return pixel_format_from_json_field(descriptor, field_name, format);
}

bool primitive_type_from_integer(std::uint32_t value, MTLPrimitiveType &primitive_type)
{
  switch (value) {
  case 0:
    primitive_type = MTLPrimitiveTypePoint;
    return true;
  case 1:
    primitive_type = MTLPrimitiveTypeLine;
    return true;
  case 2:
    primitive_type = MTLPrimitiveTypeLineStrip;
    return true;
  case 3:
    primitive_type = MTLPrimitiveTypeTriangle;
    return true;
  case 4:
    primitive_type = MTLPrimitiveTypeTriangleStrip;
    return true;
  default:
    return false;
  }
}

bool index_type_from_integer(std::uint32_t value, MTLIndexType &index_type)
{
  if (value == 0) {
    index_type = MTLIndexTypeUInt16;
    return true;
  }
  if (value == 1) {
    index_type = MTLIndexTypeUInt32;
    return true;
  }
  return false;
}

bool apply_stencil_descriptor(MTLStencilDescriptor *descriptor, const json &payload, std::string &error)
{
  if (descriptor == nil) {
    error = "depth stencil descriptor references missing stencil descriptor";
    return false;
  }
  const auto enabled_it = payload.find("enabled");
  if (enabled_it == payload.end() || !enabled_it->is_boolean()) {
    error = "depth stencil descriptor is missing enabled";
    return false;
  }
  if (!enabled_it->get<bool>()) {
    return true;
  }
  std::uint32_t depth_stencil_pass_op = 0;
  std::uint32_t stencil_fail_op = 0;
  std::uint32_t depth_fail_op = 0;
  std::uint32_t stencil_compare_function = 0;
  std::uint32_t write_mask = 0;
  std::uint32_t read_mask = 0;
  if (!required_u32(payload, "depth_stencil_pass_op", depth_stencil_pass_op) ||
      !required_u32(payload, "stencil_fail_op", stencil_fail_op) ||
      !required_u32(payload, "depth_fail_op", depth_fail_op) ||
      !required_u32(payload, "stencil_compare_function", stencil_compare_function) ||
      !required_u32(payload, "write_mask", write_mask) ||
      !required_u32(payload, "read_mask", read_mask)) {
    error = "depth stencil descriptor has incomplete stencil payload";
    return false;
  }
  descriptor.depthStencilPassOperation =
      static_cast<MTLStencilOperation>(depth_stencil_pass_op);
  descriptor.stencilFailureOperation =
      static_cast<MTLStencilOperation>(stencil_fail_op);
  descriptor.depthFailureOperation =
      static_cast<MTLStencilOperation>(depth_fail_op);
  descriptor.stencilCompareFunction =
      static_cast<MTLCompareFunction>(stencil_compare_function);
  descriptor.writeMask = write_mask;
  descriptor.readMask = read_mask;
  return true;
}

bool load_action_from_string(std::string_view name, MTLLoadAction &action)
{
  if (name == "load") {
    action = MTLLoadActionLoad;
    return true;
  }
  if (name == "dontcare") {
    action = MTLLoadActionDontCare;
    return true;
  }
  if (name == "clear") {
    action = MTLLoadActionClear;
    return true;
  }
  return false;
}

bool load_action_from_json_field(const json &payload, const char *field_name, MTLLoadAction &action)
{
  const auto it = payload.find(field_name);
  if (it == payload.end()) {
    return false;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    action = static_cast<MTLLoadAction>(it->get<std::uint32_t>());
    return true;
  }
  if (it->is_string()) {
    return load_action_from_string(it->get_ref<const std::string &>(), action);
  }
  return false;
}

bool store_action_from_string(std::string_view name, MTLStoreAction &action)
{
  if (name == "dontcare") {
    action = MTLStoreActionDontCare;
    return true;
  }
  if (name == "store") {
    action = MTLStoreActionStore;
    return true;
  }
  return false;
}

bool store_action_from_json_field(const json &payload, const char *field_name, MTLStoreAction &action)
{
  const auto it = payload.find(field_name);
  if (it == payload.end()) {
    return false;
  }
  if (it->is_number_unsigned() || it->is_number_integer()) {
    action = static_cast<MTLStoreAction>(it->get<std::uint32_t>());
    return true;
  }
  if (it->is_string()) {
    return store_action_from_string(it->get_ref<const std::string &>(), action);
  }
  return false;
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

bool origin_from_json_array(const json &payload, const char *field_name, MTLOrigin &origin)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || !it->is_array() || it->size() != 3) {
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    if (!(*it)[index].is_number_unsigned() && !(*it)[index].is_number_integer()) {
      return false;
    }
    if ((*it)[index].is_number_integer() && (*it)[index].get<std::int64_t>() < 0) {
      return false;
    }
  }
  origin = MTLOriginMake(
      static_cast<NSUInteger>((*it)[0].get<std::uint64_t>()),
      static_cast<NSUInteger>((*it)[1].get<std::uint64_t>()),
      static_cast<NSUInteger>((*it)[2].get<std::uint64_t>()));
  return true;
}

bool size_from_json_array(const json &payload, const char *field_name, MTLSize &size)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || !it->is_array() || it->size() != 3) {
    return false;
  }
  for (std::size_t index = 0; index < 3; ++index) {
    if (!(*it)[index].is_number_unsigned() && !(*it)[index].is_number_integer()) {
      return false;
    }
    if ((*it)[index].is_number_integer() && (*it)[index].get<std::int64_t>() <= 0) {
      return false;
    }
    if ((*it)[index].get<std::uint64_t>() == 0) {
      return false;
    }
  }
  size = MTLSizeMake(
      static_cast<NSUInteger>((*it)[0].get<std::uint64_t>()),
      static_cast<NSUInteger>((*it)[1].get<std::uint64_t>()),
      static_cast<NSUInteger>((*it)[2].get<std::uint64_t>()));
  return true;
}

bool viewport_from_json_array(const json &value, MTLViewport &viewport)
{
  if (!value.is_array() || value.size() != 6) {
    return false;
  }
  for (std::size_t index = 0; index < 6; ++index) {
    if (!value[index].is_number()) {
      return false;
    }
  }
  viewport = {value[0].get<double>(), value[1].get<double>(), value[2].get<double>(),
              value[3].get<double>(), value[4].get<double>(), value[5].get<double>()};
  return true;
}

bool scissor_from_json_array(const json &value, MTLScissorRect &rect)
{
  if (!value.is_array() || value.size() != 4) {
    return false;
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!value[index].is_number_unsigned() && !value[index].is_number_integer()) {
      return false;
    }
    if (value[index].is_number_integer() && value[index].get<std::int64_t>() < 0) {
      return false;
    }
  }
  rect = MTLScissorRect{static_cast<NSUInteger>(value[0].get<std::uint64_t>()),
                        static_cast<NSUInteger>(value[1].get<std::uint64_t>()),
                        static_cast<NSUInteger>(value[2].get<std::uint64_t>()),
                        static_cast<NSUInteger>(value[3].get<std::uint64_t>())};
  return true;
}

bool required_clear_color(const json &payload, const char *field_name, std::array<double, 4> &value)
{
  const auto it = payload.find(field_name);
  if (it == payload.end() || !it->is_array() || it->size() != 4) {
    return false;
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!(*it)[index].is_number()) {
      return false;
    }
    value[index] = (*it)[index].get<double>();
  }
  return true;
}

bool swizzle_channels_from_json_array(const json &value, MTLTextureSwizzleChannels &channels)
{
  if (!value.is_array() || value.size() != 4) {
    return false;
  }
  for (std::size_t index = 0; index < 4; ++index) {
    if (!value[index].is_number_unsigned() && !value[index].is_number_integer()) {
      return false;
    }
    if (value[index].is_number_integer() && value[index].get<std::int64_t>() < 0) {
      return false;
    }
    if (value[index].get<std::uint64_t>() > MTLTextureSwizzleAlpha) {
      return false;
    }
  }
  channels = MTLTextureSwizzleChannelsMake(
      static_cast<MTLTextureSwizzle>(value[0].get<std::uint32_t>()),
      static_cast<MTLTextureSwizzle>(value[1].get<std::uint32_t>()),
      static_cast<MTLTextureSwizzle>(value[2].get<std::uint32_t>()),
      static_cast<MTLTextureSwizzle>(value[3].get<std::uint32_t>()));
  return true;
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

bool texture_type_from_json_field(const json &descriptor, const char *field_name, MTLTextureType &texture_type)
{
  const auto it = descriptor.find(field_name);
  if (it == descriptor.end() || !(it->is_number_unsigned() || it->is_number_integer())) {
    return false;
  }
  texture_type = static_cast<MTLTextureType>(it->get<std::uint32_t>());
  return true;
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

bool env_flag_enabled(const char *name)
{
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 &&
         std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "FALSE") != 0;
}

std::uint64_t env_u64(const char *name, std::uint64_t fallback = 0)
{
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  char *end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != value ? static_cast<std::uint64_t>(parsed) : fallback;
}

bool metal_retrace_present_capture_enabled()
{
  return env_flag_enabled("APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES") ||
         env_flag_enabled("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES");
}

bool metal_retrace_capture_frame_in_range(std::uint64_t frame_index)
{
  static const std::uint64_t capture_from_frame =
      env_u64("APITRACE_D3D12_RETRACE_CAPTURE_FROM_FRAME");
  static const std::uint64_t stop_after_present_frame =
      env_u64("APITRACE_D3D12_RETRACE_STOP_AFTER_PRESENT_FRAME");
  if (capture_from_frame != 0 && frame_index < capture_from_frame) {
    return false;
  }
  if (stop_after_present_frame != 0 && frame_index > stop_after_present_frame) {
    return false;
  }
  return true;
}

const char *pixel_format_name(MTLPixelFormat format)
{
  switch (format) {
  case MTLPixelFormatRGBA8Unorm:
    return "rgba8unorm";
  case MTLPixelFormatRGBA8Unorm_sRGB:
    return "rgba8unorm_srgb";
  case MTLPixelFormatBGRA8Unorm:
    return "bgra8unorm";
  case MTLPixelFormatBGRA8Unorm_sRGB:
    return "bgra8unorm_srgb";
  default:
    return "unsupported";
  }
}

class NativeMetalReplayBackend final : public MetalReplayBackend {
public:
  NativeMetalReplayBackend() = default;
  ~NativeMetalReplayBackend() override = default;

private:
  struct RenderPassColorTarget {
    std::uint64_t command_buffer_id = 0;
    id<MTLTexture> texture = nil;
    NSUInteger level = 0;
    NSUInteger slice = 0;
    NSUInteger depth_plane = 0;
  };

  struct RenderPassCapture {
    std::uint64_t pass_index = 0;
    id<MTLBuffer> readback = nil;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    MTLPixelFormat pixel_format = MTLPixelFormatInvalid;
  };

public:
  bool initialize(const trace::TraceBundleReader &reader, const ReplayOptions &options) override
  {
    reader_ = &reader;
    options_ = options;
    options_.enable_metal_present_capture =
        metal_retrace_present_capture_enabled();
    index_asset_paths(reader.assets());
    index_asset_paths(reader.metal_assets());

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
    fences_ = [[NSMutableDictionary alloc] init];
    pending_presents_ = [[NSMutableDictionary alloc] init];

    if (options_.enable_metal_present_capture && env_flag_enabled("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES")) {
      const char *bundle_root = std::getenv("APITRACE_TRACE_BUNDLE");
      if (bundle_root == nullptr || *bundle_root == '\0') {
        last_error_ = "APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES requires APITRACE_TRACE_BUNDLE";
        return false;
      }
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
      if (event.call_kind == trace::MetalCallKind::ObjectMetadata) {
        index_object_metadata(event.object_id, parse_json(event.payload));
      } else if (event.call_kind == trace::MetalCallKind::InsertDebugSignpost) {
        index_object_metadata(event.object_id, parse_json(parse_json(event.payload).value("label", std::string())));
      }

      if (event.call_kind != trace::MetalCallKind::PresentDrawable) {
        continue;
      }

      const json payload = parse_json(event.payload);
      const auto width = payload.value("width", 0u);
      const auto height = payload.value("height", 0u);
      command_buffer_present_frame_indices_[event.object_id] =
          json_u64(payload.value("frame_index", json(nullptr)));
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
    case trace::MetalCallKind::Unknown:
      return fail("unknown Metal call kind is not replayable");
    case trace::MetalCallKind::DeviceCreate:
      return replay_device_create(event, payload);
    case trace::MetalCallKind::CommandQueueCreate:
      return true;
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
    case trace::MetalCallKind::BlitEncoderBatch:
      return blit_encoder_batch(event, payload);
    case trace::MetalCallKind::SetRenderPipelineState:
      return set_render_pipeline_state(event.object_id, payload);
    case trace::MetalCallKind::SetVertexBuffer:
      return set_vertex_buffer(event, payload);
    case trace::MetalCallKind::SetVertexTexture:
      return set_vertex_texture(event.object_id, payload);
    case trace::MetalCallKind::SetVertexSamplerState:
      return set_vertex_sampler_state(event.object_id, payload);
    case trace::MetalCallKind::SetVertexBytes:
      return set_vertex_bytes(event.object_id, payload);
    case trace::MetalCallKind::SetVertexBufferOffset:
      return set_vertex_buffer_offset(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentTexture:
      return set_fragment_texture(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBuffer:
      return set_fragment_buffer(event, payload);
    case trace::MetalCallKind::SetFragmentSamplerState:
      return set_fragment_sampler_state(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBytes:
      return set_fragment_bytes(event.object_id, payload);
    case trace::MetalCallKind::SetFragmentBufferOffset:
      return set_fragment_buffer_offset(event.object_id, payload);
    case trace::MetalCallKind::SetCullMode:
      return set_cull_mode(event.object_id, payload);
    case trace::MetalCallKind::SetFrontFacingWinding:
      return set_front_facing_winding(event.object_id, payload);
    case trace::MetalCallKind::SetTriangleFillMode:
      return set_triangle_fill_mode(event.object_id, payload);
    case trace::MetalCallKind::SetDepthStencilState:
      return set_depth_stencil_state(event.object_id, payload);
    case trace::MetalCallKind::SetViewport:
      return set_viewport(event.object_id, payload);
    case trace::MetalCallKind::SetScissorRect:
      return set_scissor_rect(event.object_id, payload);
    case trace::MetalCallKind::SetComputePipelineState:
      return set_compute_pipeline_state(event.object_id, payload);
    case trace::MetalCallKind::SetComputeBuffer:
      return set_compute_buffer(event, payload);
    case trace::MetalCallKind::SetComputeTexture:
      return set_compute_texture(event.object_id, payload);
    case trace::MetalCallKind::SetComputeSamplerState:
      return set_compute_sampler_state(event.object_id, payload);
    case trace::MetalCallKind::SetComputeBufferOffset:
      return set_compute_buffer_offset(event.object_id, payload);
    case trace::MetalCallKind::EncoderState:
      return encoder_state(event.object_id, payload);
    case trace::MetalCallKind::SetComputeBytes:
      return set_compute_bytes(event.object_id, payload);
    case trace::MetalCallKind::DispatchThreads:
      return dispatch_threads(event.object_id, payload);
    case trace::MetalCallKind::DispatchThreadsPerTile:
      return dispatch_threads_per_tile(event.object_id, payload);
    case trace::MetalCallKind::DrawPrimitives:
      return draw_primitives(event.object_id, payload);
    case trace::MetalCallKind::DrawIndexedPrimitives:
      return draw_indexed_primitives(event.object_id, payload);
    case trace::MetalCallKind::DrawPrimitivesIndirect:
      return draw_primitives_indirect(event.object_id, payload);
    case trace::MetalCallKind::DrawIndexedPrimitivesIndirect:
      return draw_indexed_primitives_indirect(event.object_id, payload);
    case trace::MetalCallKind::DispatchThreadgroups:
      return dispatch_threadgroups(event.object_id, payload);
    case trace::MetalCallKind::DispatchThreadgroupsIndirect:
      return dispatch_threadgroups_indirect(event.object_id, payload);
    case trace::MetalCallKind::CopyBuffer:
      return copy_buffer(event, payload);
    case trace::MetalCallKind::CopyBufferToTexture:
      return copy_buffer_to_texture(event, payload);
    case trace::MetalCallKind::CopyTexture:
      return copy_texture(event.object_id, payload);
    case trace::MetalCallKind::BlitFill:
      return blit_fill(event.object_id, payload);
    case trace::MetalCallKind::BlitBatch:
      return blit_batch(event, payload);
    case trace::MetalCallKind::PresentDrawable:
      return queue_present(event.object_id, payload);
    case trace::MetalCallKind::UseResource:
      return use_resource(event.object_id, payload);
    case trace::MetalCallKind::UseResources:
      return use_resources(event.object_id, payload);
    case trace::MetalCallKind::MemoryBarrier:
      return memory_barrier(event.object_id, payload);
    case trace::MetalCallKind::FenceOps:
      return fence_ops(event.object_id, payload);
    case trace::MetalCallKind::UpdateFence:
      return update_fence(event.object_id, payload);
    case trace::MetalCallKind::WaitForFence:
      return wait_for_fence(event.object_id, payload);
    case trace::MetalCallKind::PushDebugGroup:
      return push_debug_group(event.object_id, payload);
    case trace::MetalCallKind::PopDebugGroup:
      return pop_debug_group(event.object_id);
    case trace::MetalCallKind::ObjectMetadata:
      return object_metadata(event.object_id, payload);
    case trace::MetalCallKind::InsertDebugSignpost:
      return handle_debug_signpost(event, payload);
    case trace::MetalCallKind::BufferUpdate:
      return buffer_update(event, payload);
    case trace::MetalCallKind::TextureUpdate:
      return texture_update(event, payload);
    case trace::MetalCallKind::SetArgumentBuffer:
      return true;
    default:
      return fail("unsupported Metal call kind is not replayable");
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

  id<MTLFence> fence_for_id(std::uint64_t object_id)
  {
    if (object_id == 0) {
      return nil;
    }
    id<MTLFence> fence = [fences_ objectForKey:object_key(object_id)];
    if (fence != nil) {
      return fence;
    }
    fence = [device_ newFence];
    if (fence != nil) {
      [fences_ setObject:fence forKey:object_key(object_id)];
    }
    return fence;
  }

  void index_object_metadata(std::uint64_t object_id, const json &metadata)
  {
    const auto kind = metadata.value("kind", std::string());
    if (kind == "dxmt_buffer_gpu_address") {
      std::uint64_t buffer_id = 0;
      std::uint64_t gpu_address = 0;
      if (!required_u64(metadata, "buffer_id", buffer_id) ||
          !required_u64(metadata, "gpu_address", gpu_address)) {
        return;
      }
      if (buffer_id == 0 || gpu_address == 0) {
        return;
      }
      for (const auto &record : reader_->metal_events()) {
        if (record.call_kind != trace::MetalCallKind::DeviceCreate ||
            record.function_name != "MTLDevice.newBuffer" ||
            record.object_id != buffer_id) {
          continue;
        }
        const json buffer_payload = parse_json(record.payload);
        std::uint64_t length = 0;
        if (required_u64(buffer_payload, "length", length)) {
          original_buffer_lengths_[gpu_address] = length;
        }
      }
      return;
    }
    if (kind == "dxmt_texture_gpu_resource_id") {
      std::uint64_t texture_id = 0;
      std::uint64_t gpu_resource_id = 0;
      if (!required_u64(metadata, "texture_id", texture_id) ||
          !required_u64(metadata, "gpu_resource_id", gpu_resource_id)) {
        return;
      }
      if (texture_id != 0 && gpu_resource_id != 0) {
        original_texture_gpu_resource_ids_[texture_id] = gpu_resource_id;
      }
      return;
    }
    if (kind == "dxmt_sampler_gpu_resource_id") {
      std::uint64_t sampler_id = 0;
      std::uint64_t gpu_resource_id = 0;
      if (!required_u64(metadata, "sampler_id", sampler_id) ||
          !required_u64(metadata, "gpu_resource_id", gpu_resource_id)) {
        return;
      }
      if (sampler_id != 0 && gpu_resource_id != 0) {
        original_sampler_gpu_resource_ids_[sampler_id] = gpu_resource_id;
      }
    }
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

  static bool is_safe_asset_path(const std::filesystem::path &path)
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

  void index_asset_paths(const std::vector<trace::AssetRecord> &assets)
  {
    for (const auto &asset : assets) {
      if (asset.blob_id != 0 && !asset.relative_path.empty()) {
        asset_paths_by_blob_[asset.blob_id] = asset.relative_path;
      }
    }
  }

  bool verify_event_asset_path(
      const trace::MetalEventRecord &event,
      std::string_view relative_path_text,
      std::string_view field_name)
  {
    if (relative_path_text.empty()) {
      return true;
    }

    const std::filesystem::path relative_path(relative_path_text);
    if (!is_safe_asset_path(relative_path)) {
      return fail(std::string(field_name) + " references unsafe asset path: " + std::string(relative_path_text));
    }

    if (event.blob_refs.empty()) {
      return fail("asset path references are missing blob_refs");
    }

    const auto expected_path = relative_path.generic_string();
    for (const auto blob_id : event.blob_refs) {
      if (blob_id == 0) {
        return fail("asset path has zero blob_ref");
      }
      const auto path_it = asset_paths_by_blob_.find(blob_id);
      if (path_it == asset_paths_by_blob_.end()) {
        return fail("asset path blob_ref does not resolve");
      }
      if (path_it->second.generic_string() == expected_path) {
        return true;
      }
    }

    return fail("asset path blob_ref does not match " + expected_path);
  }

  bool fail(std::string message)
  {
    last_error_ = std::move(message);
    return false;
  }

  bool restore_buffer_range_from_asset(
      const trace::MetalEventRecord &event,
      id<MTLBuffer> buffer,
      const json &payload)
  {
    const auto asset_path = payload.value("source_asset_path", std::string());
    if (asset_path.empty()) {
      return true;
    }
    if (!verify_event_asset_path(event, asset_path, "source_asset_path")) {
      return false;
    }
    if (buffer == nil) {
      return fail("buffer range asset references missing source buffer");
    }
    void *contents = [buffer contents];
    if (contents == nullptr) {
      return fail("buffer range asset requires CPU-visible source buffer");
    }

    std::uint64_t source_offset = 0;
    std::uint64_t expected_size = 0;
    if (!required_u64(payload, "source_offset", source_offset) ||
        !required_u64(payload, "source_asset_size", expected_size)) {
      return fail("buffer range asset is missing source_offset or source_asset_size");
    }
    const auto bytes = read_binary_file(bundle_path(asset_path));
    if (bytes.size() < expected_size) {
      return fail("buffer range asset is truncated: " + asset_path);
    }
    if (source_offset > [buffer length] || expected_size > [buffer length] - source_offset) {
      return fail("buffer range asset exceeds source buffer bounds: " + asset_path);
    }

    std::memcpy(static_cast<std::uint8_t *>(contents) + source_offset, bytes.data(), static_cast<std::size_t>(expected_size));
    if (buffer.storageMode == MTLStorageModeManaged) {
      [buffer didModifyRange:NSMakeRange(static_cast<NSUInteger>(source_offset), static_cast<NSUInteger>(expected_size))];
    }
    return true;
  }

  bool restore_texture_region_from_asset(
      const trace::MetalEventRecord &event,
      id<MTLTexture> texture,
      const json &payload)
  {
    const auto asset_path = payload.value("source_asset_path", std::string());
    if (asset_path.empty()) {
      return true;
    }
    if (!verify_event_asset_path(event, asset_path, "source_asset_path")) {
      return false;
    }
    if (texture == nil) {
      return fail("texture region asset references missing texture");
    }

    std::uint64_t expected_size = 0;
    std::uint64_t bytes_per_row = 0;
    std::uint64_t bytes_per_image = 0;
    std::uint32_t level = 0;
    std::uint32_t slice = 0;
    if (!required_u64(payload, "source_asset_size", expected_size) ||
        !required_u64(payload, "bytes_per_row", bytes_per_row) ||
        !required_u64(payload, "bytes_per_image", bytes_per_image) ||
        !required_u32(payload, "level", level) ||
        !required_u32(payload, "slice", slice)) {
      return fail("texture region asset is missing replay metadata");
    }

    MTLOrigin origin = {};
    MTLSize size = {};
    if (!origin_from_json_array(payload, "origin", origin) ||
        !size_from_json_array(payload, "size", size)) {
      return fail("texture region asset is missing region metadata");
    }

    const auto bytes = read_binary_file(bundle_path(asset_path));
    if (bytes.size() < expected_size) {
      return fail("texture region asset is truncated: " + asset_path);
    }
    [texture replaceRegion:MTLRegionMake3D(origin.x, origin.y, origin.z, size.width, size.height, size.depth)
               mipmapLevel:static_cast<NSUInteger>(level)
                     slice:static_cast<NSUInteger>(slice)
                 withBytes:bytes.data()
               bytesPerRow:static_cast<NSUInteger>(bytes_per_row)
             bytesPerImage:static_cast<NSUInteger>(bytes_per_image)];
    return true;
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
    if (buffer_id == 0) {
      bindings.erase(static_cast<std::uint32_t>(index));
      return;
    }
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
      if (!verify_event_asset_path(event, buffer_path, "buffer_path")) {
        return false;
      }
      auto bytes = buffer_path.empty() ? std::vector<std::uint8_t>{} : read_binary_file(bundle_path(buffer_path));
      std::uint64_t length = 0;
      if (!required_u64(payload, "length", length)) {
        return fail("MTLDevice.newBuffer is missing length");
      }
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

	    if (event.function_name == "MTLDevice.newTexture" ||
	        event.function_name == "CAMetalDrawable.texture") {
	      return create_texture(event.object_id, parse_nested_json(payload, "descriptor"));
	    }

    if (event.function_name == "MTLDevice.newLibrary") {
      const auto library_path = payload.value("library_path", std::string());
      if (!verify_event_asset_path(event, library_path, "library_path")) {
        return false;
      }
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
      return create_render_pipeline(event, payload);
    }

    if (event.function_name == "MTLDevice.newTileRenderPipelineState") {
      return create_tile_render_pipeline(event, payload);
    }

    if (event.function_name == "MTLDevice.newComputePipelineState") {
      return create_compute_pipeline(event, payload);
    }

    return true;
  }

  bool create_texture(std::uint64_t object_id, const json &descriptor)
  {
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t depth = 0;
    std::uint64_t array_length = 0;
    std::uint64_t mipmap_level_count = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t usage = 0;
    if (!required_u64(descriptor, "width", width) ||
        !required_u64(descriptor, "height", height) ||
        !required_u64(descriptor, "depth", depth) ||
        !required_u64(descriptor, "array_length", array_length) ||
        !required_u64(descriptor, "mipmap_level_count", mipmap_level_count) ||
        !required_u64(descriptor, "sample_count", sample_count) ||
        !required_u64(descriptor, "usage", usage)) {
      return fail("texture descriptor is missing replay metadata");
    }
    MTLPixelFormat pixel_format = MTLPixelFormatInvalid;
    if (!pixel_format_from_json_field(descriptor, "pixel_format", pixel_format)) {
      return fail("texture descriptor is missing pixel_format");
    }
    if (pixel_format == MTLPixelFormatInvalid) {
      return fail("texture descriptor is missing pixel_format");
    }
    MTLTextureType texture_type = MTLTextureType2D;
    if (!texture_type_from_json_field(descriptor, "type", texture_type)) {
      return fail("texture descriptor is missing type");
    }
    auto *texture_descriptor = [[MTLTextureDescriptor alloc] init];
    texture_descriptor.textureType = texture_type;
    texture_descriptor.width = std::max<NSUInteger>(static_cast<NSUInteger>(width), 1);
    texture_descriptor.height = std::max<NSUInteger>(static_cast<NSUInteger>(height), 1);
    texture_descriptor.depth = static_cast<NSUInteger>(depth);
    texture_descriptor.arrayLength = static_cast<NSUInteger>(array_length);
    texture_descriptor.mipmapLevelCount = static_cast<NSUInteger>(mipmap_level_count);
    texture_descriptor.sampleCount = static_cast<NSUInteger>(sample_count);
    texture_descriptor.pixelFormat = pixel_format;
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = static_cast<MTLTextureUsage>(usage);

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
      std::uint64_t bytes_per_row = 0;
      if (!required_u64(descriptor, "bytes_per_row", bytes_per_row)) {
        return fail("texture descriptor initial_bytes is missing bytes_per_row");
      }
      [texture replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width), static_cast<NSUInteger>(height))
                 mipmapLevel:0
                   withBytes:bytes.data()
                 bytesPerRow:static_cast<NSUInteger>(bytes_per_row)];
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

  bool create_sampler(const json &descriptor)
  {
    std::string sampler_error;
    if (!validate_sampler_descriptor_payload(descriptor, sampler_error)) {
      return fail(sampler_error);
    }
    std::uint64_t object_id = 0;
    std::uint32_t border_color = 0;
    std::uint32_t r_address_mode = 0;
    std::uint32_t s_address_mode = 0;
    std::uint32_t t_address_mode = 0;
    std::uint32_t mag_filter = 0;
    std::uint32_t min_filter = 0;
    std::uint32_t mip_filter = 0;
    std::uint32_t compare_function = 0;
    std::uint32_t max_anisotropy = 0;
    const auto lod_max_clamp_it = descriptor.find("lod_max_clamp");
    const auto lod_min_clamp_it = descriptor.find("lod_min_clamp");
    const auto lod_average_it = descriptor.find("lod_average");
    const auto normalized_coordinates_it = descriptor.find("normalized_coordinates");
    const auto support_argument_buffers_it = descriptor.find("support_argument_buffers");
    if (!required_u64(descriptor, "sampler_id", object_id) ||
        !required_u32(descriptor, "border_color", border_color) ||
        !required_u32(descriptor, "r_address_mode", r_address_mode) ||
        !required_u32(descriptor, "s_address_mode", s_address_mode) ||
        !required_u32(descriptor, "t_address_mode", t_address_mode) ||
        !required_u32(descriptor, "mag_filter", mag_filter) ||
        !required_u32(descriptor, "min_filter", min_filter) ||
        !required_u32(descriptor, "mip_filter", mip_filter) ||
        !required_u32(descriptor, "compare_function", compare_function) ||
        !required_u32(descriptor, "max_anisotropy", max_anisotropy) ||
        lod_max_clamp_it == descriptor.end() ||
        !lod_max_clamp_it->is_number() ||
        lod_min_clamp_it == descriptor.end() ||
        !lod_min_clamp_it->is_number() ||
        lod_average_it == descriptor.end() ||
        !lod_average_it->is_boolean() ||
        normalized_coordinates_it == descriptor.end() ||
        !normalized_coordinates_it->is_boolean() ||
        support_argument_buffers_it == descriptor.end() ||
        !support_argument_buffers_it->is_boolean()) {
      return fail("sampler descriptor is missing replay metadata");
    }
    auto *sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.borderColor = static_cast<MTLSamplerBorderColor>(border_color);
    sampler_descriptor.rAddressMode = static_cast<MTLSamplerAddressMode>(r_address_mode);
    sampler_descriptor.sAddressMode = static_cast<MTLSamplerAddressMode>(s_address_mode);
    sampler_descriptor.tAddressMode = static_cast<MTLSamplerAddressMode>(t_address_mode);
    sampler_descriptor.magFilter = static_cast<MTLSamplerMinMagFilter>(mag_filter);
    sampler_descriptor.minFilter = static_cast<MTLSamplerMinMagFilter>(min_filter);
    sampler_descriptor.mipFilter = static_cast<MTLSamplerMipFilter>(mip_filter);
    sampler_descriptor.compareFunction = static_cast<MTLCompareFunction>(compare_function);
    sampler_descriptor.lodMaxClamp = lod_max_clamp_it->get<double>();
    sampler_descriptor.lodMinClamp = lod_min_clamp_it->get<double>();
    sampler_descriptor.maxAnisotropy = static_cast<NSUInteger>(max_anisotropy);
    sampler_descriptor.lodAverage = lod_average_it->get<bool>();
    sampler_descriptor.normalizedCoordinates = normalized_coordinates_it->get<bool>();
    sampler_descriptor.supportArgumentBuffers = support_argument_buffers_it->get<bool>();

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
    std::uint64_t object_id = 0;
    std::uint64_t source_texture_id = 0;
    std::uint64_t gpu_resource_id = 0;
    std::uint32_t pixel_format = 0;
    std::uint32_t texture_type = 0;
    std::uint64_t level_start = 0;
    std::uint64_t level_count = 0;
    std::uint64_t slice_start = 0;
    std::uint64_t slice_count = 0;
    if (!required_u64(descriptor, "texture_id", object_id) ||
        !required_u64(descriptor, "source_texture_id", source_texture_id) ||
        !required_u64(descriptor, "gpu_resource_id", gpu_resource_id) ||
        !required_u32(descriptor, "pixel_format", pixel_format) ||
        !required_u32(descriptor, "texture_type", texture_type) ||
        !required_u64(descriptor, "level_start", level_start) ||
        !required_u64(descriptor, "level_count", level_count) ||
        !required_u64(descriptor, "slice_start", slice_start) ||
        !required_u64(descriptor, "slice_count", slice_count) ||
        descriptor.find("swizzle") == descriptor.end()) {
      return fail("texture view descriptor is missing replay metadata");
    }
    id<MTLTexture> source_texture = texture_for_id(source_texture_id);
    if (object_id == 0 || source_texture == nil) {
      return true;
    }

    MTLTextureSwizzleChannels swizzle = {};
    if (!swizzle_channels_from_json_array(descriptor["swizzle"], swizzle)) {
      return fail("texture view descriptor has invalid swizzle");
    }
    id<MTLTexture> texture = [source_texture newTextureViewWithPixelFormat:static_cast<MTLPixelFormat>(pixel_format)
                                                               textureType:static_cast<MTLTextureType>(texture_type)
                                                                    levels:NSMakeRange(static_cast<NSUInteger>(level_start),
                                                                                       static_cast<NSUInteger>(level_count))
                                                                    slices:NSMakeRange(static_cast<NSUInteger>(slice_start),
                                                                                       static_cast<NSUInteger>(slice_count))
                                                                   swizzle:swizzle];
    if (texture == nil) {
      return fail("failed to create MTLTexture view");
    }
    [textures_ setObject:texture forKey:object_key(object_id)];
    const auto original_gpu_resource_id = gpu_resource_id;
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
    std::uint64_t object_id = 0;
    std::uint32_t depth_compare_function = 0;
    const auto depth_write_enabled_it = descriptor.find("depth_write_enabled");
    if (!required_u64(descriptor, "depth_stencil_state_id", object_id) ||
        !required_u32(descriptor, "depth_compare_function", depth_compare_function) ||
        depth_write_enabled_it == descriptor.end() ||
        !depth_write_enabled_it->is_boolean()) {
      return fail("depth stencil descriptor is missing replay metadata");
    }
    if (object_id == 0) {
      return fail("depth stencil descriptor has zero depth_stencil_state_id");
    }

    auto *state_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    state_descriptor.depthCompareFunction =
        static_cast<MTLCompareFunction>(depth_compare_function);
    state_descriptor.depthWriteEnabled = depth_write_enabled_it->get<bool>();
    if (descriptor.contains("front_stencil")) {
      std::string error;
      if (!apply_stencil_descriptor(state_descriptor.frontFaceStencil, descriptor["front_stencil"], error)) {
        return fail(error);
      }
    }
    if (descriptor.contains("back_stencil")) {
      std::string error;
      if (!apply_stencil_descriptor(state_descriptor.backFaceStencil, descriptor["back_stencil"], error)) {
        return fail(error);
      }
    }

    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:state_descriptor];
    if (state == nil) {
      return fail("failed to create MTLDepthStencilState");
    }
    [depth_stencil_states_ setObject:state forKey:object_key(object_id)];
    return true;
  }

  bool create_render_pipeline(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto descriptor_path = payload.value("descriptor_path", std::string());
    if (!verify_event_asset_path(event, descriptor_path, "descriptor_path")) {
      return false;
    }
    const auto descriptor_bytes = read_binary_file(bundle_path(descriptor_path));
    const auto descriptor = parse_json(std::string(descriptor_bytes.begin(), descriptor_bytes.end()));
    if (!descriptor.is_object()) {
      return fail("render pipeline descriptor must be a JSON object");
    }
    if (descriptor.value("pipeline_kind", std::string()) == "mesh_render") {
      return create_mesh_render_pipeline(event, descriptor);
    }
    std::uint64_t vertex_library_id = 0;
    if (!required_u64(descriptor, "vertex_library_id", vertex_library_id)) {
      std::uint64_t library_id = 0;
      if (!required_u64(descriptor, "library_id", library_id)) {
        return fail("render pipeline descriptor is missing vertex_library_id");
      }
      vertex_library_id = library_id;
    }
    std::uint64_t fragment_library_id = 0;
    if (!required_u64(descriptor, "fragment_library_id", fragment_library_id)) {
      fragment_library_id = vertex_library_id;
    }
    const auto vertex_function_name = descriptor.value("vertex_function", std::string());
    if (vertex_function_name.empty()) {
      return fail("render pipeline descriptor missing vertex_function");
    }
    const auto fragment_function_name = descriptor.value("fragment_function", std::string());
    const auto fragment_function_constants = descriptor.value("fragment_function_constants", json::array());
    const auto rasterization_enabled_it = descriptor.find("rasterization_enabled");
    std::uint64_t raster_sample_count = 0;
    if (rasterization_enabled_it == descriptor.end() ||
        !rasterization_enabled_it->is_boolean() ||
        !required_u64(descriptor, "raster_sample_count", raster_sample_count)) {
      return fail("render pipeline descriptor is missing rasterization metadata");
    }
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
	    const bool rasterization_enabled = descriptor.value("rasterization_enabled", true);
	    const auto colors = descriptor.find("colors");
    if (colors == descriptor.end() || !colors->is_array() || colors->empty()) {
      return fail("render pipeline descriptor missing color attachment metadata");
    }
    std::size_t slot = 0;
    for (const auto &color : *colors) {
      if (slot >= 8) {
        break;
      }
      auto *attachment = pipeline_descriptor.colorAttachments[slot++];
      const auto blending_enabled_it = color.find("blending_enabled");
      std::uint64_t write_mask = 0;
      std::uint64_t rgb_blend_operation = 0;
      std::uint64_t alpha_blend_operation = 0;
      std::uint64_t src_rgb_blend_factor = 0;
      std::uint64_t dst_rgb_blend_factor = 0;
      std::uint64_t src_alpha_blend_factor = 0;
      std::uint64_t dst_alpha_blend_factor = 0;
      if (blending_enabled_it == color.end() ||
          !blending_enabled_it->is_boolean() ||
          !required_u64(color, "write_mask", write_mask) ||
          !required_u64(color, "rgb_blend_operation", rgb_blend_operation) ||
          !required_u64(color, "alpha_blend_operation", alpha_blend_operation) ||
          !required_u64(color, "src_rgb_blend_factor", src_rgb_blend_factor) ||
          !required_u64(color, "dst_rgb_blend_factor", dst_rgb_blend_factor) ||
          !required_u64(color, "src_alpha_blend_factor", src_alpha_blend_factor) ||
          !required_u64(color, "dst_alpha_blend_factor", dst_alpha_blend_factor)) {
        return fail("render pipeline color attachment is missing replay metadata");
      }
	      MTLPixelFormat color_pixel_format = MTLPixelFormatInvalid;
	      if (!pixel_format_from_json_field(color, "pixel_format", color_pixel_format)) {
	        return fail("render pipeline color attachment is missing pixel_format");
	      }
	      if (color_pixel_format == MTLPixelFormatInvalid && rasterization_enabled) {
	        continue;
	      }
	      attachment.pixelFormat = color_pixel_format;
      attachment.blendingEnabled = blending_enabled_it->get<bool>();
      attachment.writeMask = static_cast<MTLColorWriteMask>(write_mask);
      attachment.rgbBlendOperation = static_cast<MTLBlendOperation>(rgb_blend_operation);
      attachment.alphaBlendOperation = static_cast<MTLBlendOperation>(alpha_blend_operation);
      attachment.sourceRGBBlendFactor = static_cast<MTLBlendFactor>(src_rgb_blend_factor);
      attachment.destinationRGBBlendFactor = static_cast<MTLBlendFactor>(dst_rgb_blend_factor);
      attachment.sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(src_alpha_blend_factor);
      attachment.destinationAlphaBlendFactor = static_cast<MTLBlendFactor>(dst_alpha_blend_factor);
    }
    pipeline_descriptor.rasterizationEnabled = rasterization_enabled_it->get<bool>();
    MTLPixelFormat depth_pixel_format = MTLPixelFormatInvalid;
    MTLPixelFormat stencil_pixel_format = MTLPixelFormatInvalid;
    if (!optional_pixel_format_from_json_field(descriptor, "depth_pixel_format", depth_pixel_format) ||
        !optional_pixel_format_from_json_field(descriptor, "stencil_pixel_format", stencil_pixel_format)) {
      return fail("render pipeline descriptor has invalid depth/stencil pixel format");
    }
    pipeline_descriptor.depthAttachmentPixelFormat = depth_pixel_format;
    pipeline_descriptor.stencilAttachmentPixelFormat = stencil_pixel_format;
    pipeline_descriptor.rasterSampleCount = static_cast<NSUInteger>(raster_sample_count);

    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [device_ newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
    if (pipeline == nil) {
      return fail(error ? [[error localizedDescription] UTF8String] : "failed to create render pipeline");
    }
    [render_pipelines_ setObject:pipeline forKey:object_key(event.object_id)];
    return true;
  }

  bool create_mesh_render_pipeline(const trace::MetalEventRecord &event, const json &descriptor)
  {
    std::uint64_t object_library_id = 0;
    std::uint64_t mesh_library_id = 0;
    std::uint64_t fragment_library_id = 0;
    if (!required_u64(descriptor, "object_library_id", object_library_id) ||
        !required_u64(descriptor, "mesh_library_id", mesh_library_id)) {
      return fail("mesh render pipeline descriptor is missing object/mesh library metadata");
    }
    if (!required_u64(descriptor, "fragment_library_id", fragment_library_id)) {
      fragment_library_id = mesh_library_id;
    }
    const auto object_function_name = descriptor.value("object_function", std::string());
    const auto mesh_function_name = descriptor.value("mesh_function", std::string());
    const auto fragment_function_name = descriptor.value("fragment_function", std::string());
    if (object_function_name.empty() || mesh_function_name.empty()) {
      return fail("mesh render pipeline descriptor is missing object or mesh function");
    }
    id<MTLLibrary> object_library = library_for_id(object_library_id);
    id<MTLLibrary> mesh_library = library_for_id(mesh_library_id);
    id<MTLLibrary> fragment_library = fragment_function_name.empty() ? nil : library_for_id(fragment_library_id);
    if (object_library == nil || mesh_library == nil || (!fragment_function_name.empty() && fragment_library == nil)) {
      return fail("mesh render pipeline references missing library");
    }

    auto *pipeline_descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];
    NSError *function_error = nil;
    pipeline_descriptor.objectFunction =
        new_function(object_library, object_function_name, descriptor.value("object_function_constants", json::array()), &function_error);
    if (pipeline_descriptor.objectFunction == nil) {
      return fail(function_error ? [[function_error localizedDescription] UTF8String] : "failed to create object function");
    }
    function_error = nil;
    pipeline_descriptor.meshFunction =
        new_function(mesh_library, mesh_function_name, descriptor.value("mesh_function_constants", json::array()), &function_error);
    if (pipeline_descriptor.meshFunction == nil) {
      return fail(function_error ? [[function_error localizedDescription] UTF8String] : "failed to create mesh function");
    }
    if (!fragment_function_name.empty()) {
      function_error = nil;
      pipeline_descriptor.fragmentFunction =
          new_function(fragment_library, fragment_function_name, descriptor.value("fragment_function_constants", json::array()), &function_error);
      if (pipeline_descriptor.fragmentFunction == nil) {
        return fail(function_error ? [[function_error localizedDescription] UTF8String] : "failed to create fragment function");
      }
    }
    const auto colors = descriptor.find("colors");
    if (colors == descriptor.end() || !colors->is_array() || colors->empty()) {
      return fail("mesh render pipeline descriptor missing color attachment metadata");
    }
    std::size_t slot = 0;
    for (const auto &color : *colors) {
      if (slot >= 8) {
        break;
      }
      auto *attachment = pipeline_descriptor.colorAttachments[slot++];
      MTLPixelFormat color_pixel_format = MTLPixelFormatInvalid;
      if (!pixel_format_from_json_field(color, "pixel_format", color_pixel_format)) {
        return fail("mesh render pipeline color attachment is missing pixel_format");
      }
      attachment.pixelFormat = color_pixel_format;
      attachment.blendingEnabled = color.value("blending_enabled", false);
      attachment.writeMask = static_cast<MTLColorWriteMask>(json_u64(color.value("write_mask", json(0))));
      attachment.rgbBlendOperation = static_cast<MTLBlendOperation>(json_u64(color.value("rgb_blend_operation", json(0))));
      attachment.alphaBlendOperation = static_cast<MTLBlendOperation>(json_u64(color.value("alpha_blend_operation", json(0))));
      attachment.sourceRGBBlendFactor = static_cast<MTLBlendFactor>(json_u64(color.value("src_rgb_blend_factor", json(0))));
      attachment.destinationRGBBlendFactor = static_cast<MTLBlendFactor>(json_u64(color.value("dst_rgb_blend_factor", json(0))));
      attachment.sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(json_u64(color.value("src_alpha_blend_factor", json(0))));
      attachment.destinationAlphaBlendFactor = static_cast<MTLBlendFactor>(json_u64(color.value("dst_alpha_blend_factor", json(0))));
    }
    pipeline_descriptor.rasterizationEnabled = descriptor.value("rasterization_enabled", true);
    pipeline_descriptor.rasterSampleCount = static_cast<NSUInteger>(json_u64(descriptor.value("raster_sample_count", json(1)), 1));
    pipeline_descriptor.payloadMemoryLength = static_cast<NSUInteger>(json_u64(descriptor.value("payload_memory_length", json(0))));
    pipeline_descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth =
        descriptor.value("mesh_tgsize_is_multiple_of_sgwidth", false);
    pipeline_descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth =
        descriptor.value("object_tgsize_is_multiple_of_sgwidth", false);
    MTLPixelFormat depth_pixel_format = MTLPixelFormatInvalid;
    MTLPixelFormat stencil_pixel_format = MTLPixelFormatInvalid;
    if (!optional_pixel_format_from_json_field(descriptor, "depth_pixel_format", depth_pixel_format) ||
        !optional_pixel_format_from_json_field(descriptor, "stencil_pixel_format", stencil_pixel_format)) {
      return fail("mesh render pipeline descriptor has invalid depth/stencil pixel format");
    }
    pipeline_descriptor.depthAttachmentPixelFormat = depth_pixel_format;
    pipeline_descriptor.stencilAttachmentPixelFormat = stencil_pixel_format;
#if defined(__APPLE__)
    if ([pipeline_descriptor respondsToSelector:@selector(setLogicOperationEnabled:)]) {
      [pipeline_descriptor setLogicOperationEnabled:descriptor.value("logic_operation_enabled", false)];
      [pipeline_descriptor setLogicOperation:static_cast<NSUInteger>(json_u64(descriptor.value("logic_operation", json(0))))];
    }
#endif

    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device_ newRenderPipelineStateWithMeshDescriptor:pipeline_descriptor
                                                  options:MTLPipelineOptionNone
                                               reflection:nil
                                                    error:&error];
    if (pipeline == nil) {
      return fail(error ? [[error localizedDescription] UTF8String] : "failed to create mesh render pipeline");
    }
    [render_pipelines_ setObject:pipeline forKey:object_key(event.object_id)];
    return true;
  }

  bool create_tile_render_pipeline(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto descriptor_path = payload.value("descriptor_path", std::string());
    if (!verify_event_asset_path(event, descriptor_path, "descriptor_path")) {
      return false;
    }
    const auto descriptor_bytes = read_binary_file(bundle_path(descriptor_path));
    const auto descriptor = parse_json(std::string(descriptor_bytes.begin(), descriptor_bytes.end()));
    if (!descriptor.is_object()) {
      return fail("tile render pipeline descriptor must be a JSON object");
    }

    std::uint64_t library_id = 0;
    if (!required_u64(descriptor, "tile_library_id", library_id)) {
      return fail("tile render pipeline descriptor is missing tile_library_id");
    }
    id<MTLLibrary> library = library_for_id(library_id);
    if (library == nil) {
      return fail("tile render pipeline references missing library");
    }
    const auto function_name = descriptor.value("tile_function", std::string());
    if (function_name.empty()) {
      return fail("tile render pipeline descriptor missing tile_function");
    }
    NSError *function_error = nil;
    id<MTLFunction> function = new_function(library, function_name, json::array(), &function_error);
    if (function == nil) {
      return fail(function_error ? [[function_error localizedDescription] UTF8String] : "failed to create tile function");
    }

    auto *pipeline_descriptor = [[MTLTileRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.tileFunction = function;
    const auto colors = descriptor.find("colors");
    if (colors == descriptor.end() || !colors->is_array() || colors->empty()) {
      return fail("tile render pipeline descriptor missing color attachment metadata");
    }
    std::size_t slot = 0;
    for (const auto &color : *colors) {
      if (slot >= 8) {
        break;
      }
      MTLPixelFormat color_pixel_format = MTLPixelFormatInvalid;
      if (!pixel_format_from_json_field(color, "pixel_format", color_pixel_format)) {
        return fail("tile render pipeline color attachment is missing pixel_format");
      }
      pipeline_descriptor.colorAttachments[slot++].pixelFormat = color_pixel_format;
    }
    pipeline_descriptor.rasterSampleCount =
        static_cast<NSUInteger>(json_u64(descriptor.value("raster_sample_count", json(1)), 1));
#if defined(__APPLE__)
    if ([pipeline_descriptor respondsToSelector:@selector(setThreadgroupSizeMatchesTileSize:)]) {
      [pipeline_descriptor setThreadgroupSizeMatchesTileSize:descriptor.value("tgsize_matches_tile_size", false)];
    }
#endif

    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device_ newRenderPipelineStateWithTileDescriptor:pipeline_descriptor
                                                  options:MTLPipelineOptionNone
                                               reflection:nil
                                                    error:&error];
    if (pipeline == nil) {
      return fail(error ? [[error localizedDescription] UTF8String] : "failed to create tile render pipeline");
    }
    [render_pipelines_ setObject:pipeline forKey:object_key(event.object_id)];
    return true;
  }

  bool create_compute_pipeline(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto descriptor_path = payload.value("descriptor_path", std::string());
    if (!verify_event_asset_path(event, descriptor_path, "descriptor_path")) {
      return false;
    }
    const auto descriptor_bytes = read_binary_file(bundle_path(descriptor_path));
    const auto descriptor = parse_json(std::string(descriptor_bytes.begin(), descriptor_bytes.end()));
    std::uint64_t library_id = 0;
    if (!required_u64(descriptor, "library_id", library_id)) {
      return fail("compute pipeline descriptor is missing library_id");
    }
    id<MTLLibrary> library = library_for_id(library_id);
    if (library == nil) {
      return fail("compute pipeline references missing library");
    }

    const auto function_name = descriptor.value("function", std::string());
    if (function_name.empty()) {
      return fail("compute pipeline descriptor missing function");
    }
    id<MTLFunction> function =
        [library newFunctionWithName:[NSString stringWithUTF8String:function_name.c_str()]];
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = [device_ newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) {
      return fail(error ? [[error localizedDescription] UTF8String] : "failed to create compute pipeline");
    }
    [compute_pipelines_ setObject:pipeline forKey:object_key(event.object_id)];
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
    if (command_buffer.status == MTLCommandBufferStatusError) {
      const char *message = command_buffer.error != nil
                                ? [[command_buffer.error localizedDescription] UTF8String]
                                : "command buffer completed with error";
      if (debug_gpu_patch_enabled()) {
        std::fprintf(stderr, "metal replay debug command buffer error: %s\n", message);
      }
      return fail(message);
    }

    NSDictionary *present_info = [pending_presents_ objectForKey:object_key(event.object_id)];
    if (present_info != nil) {
      if (!flush_render_pass_captures(event.object_id, present_info)) {
        return false;
      }
      if (!capture_present_frame(present_info)) {
        return false;
      }
      [pending_presents_ removeObjectForKey:object_key(event.object_id)];
    } else {
      discard_render_pass_captures(event.object_id);
    }

    [command_buffer_present_textures_ removeObjectForKey:object_key(event.object_id)];
    [command_buffers_ removeObjectForKey:object_key(event.object_id)];
    command_buffer_present_frame_indices_.erase(event.object_id);
    return true;
  }

  bool begin_render_encoder(const trace::MetalEventRecord &event, const json &payload)
  {
    std::uint64_t command_buffer_id = 0;
    if (!required_u64(payload, "command_buffer_id", command_buffer_id)) {
      return fail("render encoder is missing command_buffer_id");
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(command_buffer_id);
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
	      }
	    }
	    const bool has_color_texture = color_texture_id != 0;

	    if (command_buffer_id != 0 &&
	        pass.value("render_target_width", 0u) == 0 &&
        pass.value("render_target_height", 0u) == 0) {
      NSDictionary *present_size = [command_buffer_present_sizes_ objectForKey:object_key(command_buffer_id)];
      if (present_size != nil) {
        pass["render_target_width"] = static_cast<std::uint32_t>([present_size[@"width"] unsignedIntValue]);
        pass["render_target_height"] = static_cast<std::uint32_t>([present_size[@"height"] unsignedIntValue]);
      }
    }
	    if (command_buffer_id != 0 && has_color_texture) {
	      [command_buffer_present_textures_ setObject:object_key(color_texture_id)
	                                           forKey:object_key(command_buffer_id)];
	    }

    RenderPassColorTarget primary_color_target{};
    const auto colors = pass.find("colors");
    if (colors != pass.end() && colors->is_array() && !colors->empty()) {
      for (const auto &color : *colors) {
        std::uint32_t slot = 0;
        std::uint64_t level = 0;
        std::uint64_t slice = 0;
        std::uint64_t depth_plane = 0;
        if (!required_u32(color, "slot", slot) ||
            !required_u64(color, "level", level) ||
            !required_u64(color, "slice", slice) ||
            !required_u64(color, "depth_plane", depth_plane)) {
          return fail("render pass color attachment is missing subresource metadata");
        }
        std::array<double, 4> clear{};
        if (!required_clear_color(color, "clear_color", clear)) {
          return fail("render pass color attachment is missing clear_color");
        }
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
        if (primary_color_target.texture == nil) {
          primary_color_target = RenderPassColorTarget{
              command_buffer_id,
              color_texture,
              static_cast<NSUInteger>(level),
              static_cast<NSUInteger>(slice),
              static_cast<NSUInteger>(depth_plane),
          };
        }

        MTLLoadAction load_action = MTLLoadActionClear;
        MTLStoreAction store_action = MTLStoreActionStore;
        if (!load_action_from_json_field(color, "load_action", load_action) ||
            !store_action_from_json_field(color, "store_action", store_action)) {
          return fail("render pass color attachment is missing load/store action metadata");
        }
        auto *attachment = descriptor.colorAttachments[static_cast<NSUInteger>(slot)];
        attachment.texture = color_texture;
        attachment.loadAction = load_action;
        attachment.storeAction = store_action;
        attachment.level = static_cast<NSUInteger>(level);
        attachment.slice = static_cast<NSUInteger>(slice);
        attachment.depthPlane = static_cast<NSUInteger>(depth_plane);
        attachment.clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);

        const auto resolve_texture_id = optional_object_id_from_json_field(color, "resolve_texture");
        if (resolve_texture_id != 0) {
          id<MTLTexture> resolve_texture = texture_for_id(resolve_texture_id);
          if (resolve_texture == nil) {
            return fail("render pass references missing resolve texture");
          }
          std::uint64_t resolve_level = 0;
          std::uint64_t resolve_slice = 0;
          std::uint64_t resolve_depth_plane = 0;
          if (!required_u64(color, "resolve_level", resolve_level) ||
              !required_u64(color, "resolve_slice", resolve_slice) ||
              !required_u64(color, "resolve_depth_plane", resolve_depth_plane)) {
            return fail("render pass resolve attachment is missing subresource metadata");
          }
          attachment.resolveTexture = resolve_texture;
          attachment.resolveLevel = static_cast<NSUInteger>(resolve_level);
          attachment.resolveSlice = static_cast<NSUInteger>(resolve_slice);
          attachment.resolveDepthPlane = static_cast<NSUInteger>(resolve_depth_plane);
        }
      }
	    } else if (has_color_texture) {
	      id<MTLTexture> color_texture = texture_for_id(color_texture_id);
      if (color_texture == nil) {
        if (!create_texture(color_texture_id, pass)) {
          return false;
        }
        color_texture = texture_for_id(color_texture_id);
      }

      std::uint32_t slot = 0;
      std::uint64_t level = 0;
      std::uint64_t slice = 0;
      std::uint64_t depth_plane = 0;
      if (!required_u32(pass, "slot", slot) ||
          !required_u64(pass, "level", level) ||
          !required_u64(pass, "slice", slice) ||
          !required_u64(pass, "depth_plane", depth_plane)) {
        return fail("render pass color attachment is missing subresource metadata");
      }
      primary_color_target = RenderPassColorTarget{
          command_buffer_id,
          color_texture,
          static_cast<NSUInteger>(level),
          static_cast<NSUInteger>(slice),
          static_cast<NSUInteger>(depth_plane),
      };
      std::array<double, 4> clear{};
      if (!required_clear_color(pass, "clear_color", clear)) {
        return fail("render pass color attachment is missing clear_color");
      }
      MTLLoadAction load_action = MTLLoadActionClear;
      MTLStoreAction store_action = MTLStoreActionStore;
      if (!load_action_from_json_field(pass, "load_action", load_action) ||
          !store_action_from_json_field(pass, "store_action", store_action)) {
        return fail("render pass color attachment is missing load/store action metadata");
      }
      auto *attachment = descriptor.colorAttachments[static_cast<NSUInteger>(slot)];
      attachment.texture = color_texture;
      attachment.loadAction = load_action;
      attachment.storeAction = store_action;
      attachment.level = static_cast<NSUInteger>(level);
      attachment.slice = static_cast<NSUInteger>(slice);
      attachment.depthPlane = static_cast<NSUInteger>(depth_plane);
      attachment.clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
    }

    const auto depth = pass.find("depth");
    if (depth != pass.end() && depth->is_object()) {
      const auto depth_texture_id = optional_object_id_from_json_field(*depth, "texture");
      if (depth_texture_id != 0) {
        id<MTLTexture> depth_texture = texture_for_id(depth_texture_id);
        if (depth_texture == nil) {
          return fail("render pass references missing depth texture");
        }
        MTLLoadAction load_action = MTLLoadActionClear;
        MTLStoreAction store_action = MTLStoreActionStore;
        if (!load_action_from_json_field(*depth, "load_action", load_action) ||
            !store_action_from_json_field(*depth, "store_action", store_action)) {
          return fail("render pass depth attachment is missing load/store action metadata");
        }
        descriptor.depthAttachment.texture = depth_texture;
        descriptor.depthAttachment.loadAction = load_action;
        descriptor.depthAttachment.storeAction = store_action;
        const auto clear_depth = depth->find("clear_depth");
        std::uint64_t level = 0;
        std::uint64_t slice = 0;
        std::uint64_t depth_plane = 0;
        if (clear_depth == depth->end() ||
            !clear_depth->is_number() ||
            !required_u64(*depth, "level", level) ||
            !required_u64(*depth, "slice", slice) ||
            !required_u64(*depth, "depth_plane", depth_plane)) {
          return fail("render pass depth attachment is missing replay metadata");
        }
        descriptor.depthAttachment.clearDepth = clear_depth->get<double>();
        descriptor.depthAttachment.level = static_cast<NSUInteger>(level);
        descriptor.depthAttachment.slice = static_cast<NSUInteger>(slice);
        descriptor.depthAttachment.depthPlane = static_cast<NSUInteger>(depth_plane);
      }
    }

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (encoder == nil) {
      return fail("failed to create render command encoder");
    }
    [render_encoders_ setObject:encoder forKey:object_key(event.object_id)];
    [render_encoder_command_buffers_ setObject:object_key(command_buffer_id) forKey:object_key(event.object_id)];
    if (primary_color_target.texture != nil) {
      render_encoder_color_targets_[event.object_id] = primary_color_target;
    }
    encoder_resource_states_[event.object_id] = {};
    render_vertex_buffer_bindings_[event.object_id] = {};
    render_fragment_buffer_bindings_[event.object_id] = {};
    return true;
  }

  bool begin_compute_encoder(const trace::MetalEventRecord &event, const json &payload)
  {
    std::uint64_t command_buffer_id = 0;
    if (!required_u64(payload, "command_buffer_id", command_buffer_id)) {
      return fail("compute encoder is missing command_buffer_id");
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(command_buffer_id);
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
    std::uint64_t command_buffer_id = 0;
    if (!required_u64(payload, "command_buffer_id", command_buffer_id)) {
      return fail("blit encoder is missing command_buffer_id");
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(command_buffer_id);
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
    capture_render_pass_color_target(encoder_id);
    [render_encoders_ removeObjectForKey:object_key(encoder_id)];
    [render_encoder_command_buffers_ removeObjectForKey:object_key(encoder_id)];
    render_encoder_color_targets_.erase(encoder_id);
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

  void capture_render_pass_color_target(std::uint64_t encoder_id)
  {
    if (!options_.enable_metal_present_capture) {
      return;
    }
    const char *sink_dir = std::getenv("APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR");
    if (sink_dir == nullptr || *sink_dir == '\0') {
      return;
    }
    const auto target_it = render_encoder_color_targets_.find(encoder_id);
    if (target_it == render_encoder_color_targets_.end()) {
      return;
    }
    const RenderPassColorTarget &target = target_it->second;
    const auto frame_it = command_buffer_present_frame_indices_.find(target.command_buffer_id);
    if (frame_it == command_buffer_present_frame_indices_.end() ||
        !metal_retrace_capture_frame_in_range(frame_it->second)) {
      return;
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(target.command_buffer_id);
    if (command_buffer == nil || target.texture == nil) {
      return;
    }
    switch (target.texture.pixelFormat) {
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
      break;
    default:
      std::fprintf(stderr,
                   "metal retrace pass capture skipped: unsupported color format %s (%lu)\n",
                   pixel_format_name(target.texture.pixelFormat),
                   static_cast<unsigned long>(target.texture.pixelFormat));
      return;
    }
    if (target.texture.sampleCount > 1) {
      std::fprintf(stderr,
                   "metal retrace pass capture skipped: multisample color target sampleCount=%lu\n",
                   static_cast<unsigned long>(target.texture.sampleCount));
      return;
    }
    if (target.texture.width == 0 || target.texture.height == 0 ||
        target.texture.width > std::numeric_limits<std::uint32_t>::max() ||
        target.texture.height > std::numeric_limits<std::uint32_t>::max()) {
      std::fprintf(stderr, "metal retrace pass capture skipped: invalid color target dimensions\n");
      return;
    }

    const auto width = static_cast<std::uint32_t>(target.texture.width);
    const auto height = static_cast<std::uint32_t>(target.texture.height);
    const auto row_pitch = width * 4u;
    const auto byte_size =
        static_cast<NSUInteger>(row_pitch) * static_cast<NSUInteger>(height);
    id<MTLBuffer> readback =
        [device_ newBufferWithLength:byte_size options:MTLResourceStorageModeShared];
    if (readback == nil) {
      std::fprintf(stderr, "metal retrace pass capture skipped: failed to allocate readback buffer\n");
      return;
    }
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) {
      std::fprintf(stderr, "metal retrace pass capture skipped: failed to create readback blit encoder\n");
      return;
    }
    [blit copyFromTexture:target.texture
              sourceSlice:target.slice
              sourceLevel:target.level
             sourceOrigin:MTLOriginMake(0, 0, target.depth_plane)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:readback
        destinationOffset:0
   destinationBytesPerRow:row_pitch
 destinationBytesPerImage:static_cast<NSUInteger>(row_pitch) * static_cast<NSUInteger>(height)];
    [blit endEncoding];

    auto &captures = render_pass_captures_[target.command_buffer_id];
    captures.push_back(RenderPassCapture{
        ++render_pass_indices_[target.command_buffer_id],
        readback,
        width,
        height,
        row_pitch,
        target.texture.pixelFormat,
    });
  }

  bool blit_encoder_batch(const trace::MetalEventRecord &event, const json &payload)
  {
    std::uint64_t command_buffer_id = 0;
    if (!required_u64(payload, "command_buffer_id", command_buffer_id)) {
      return fail("blit encoder batch is missing command_buffer_id");
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(command_buffer_id);
    if (command_buffer == nil) {
      return fail("blit encoder batch references missing command buffer");
    }

    id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
    if (encoder == nil) {
      return fail("failed to create batched blit command encoder");
    }
    [blit_encoders_ setObject:encoder forKey:object_key(event.object_id)];
    const bool ok = blit_batch(event, payload);
    [encoder endEncoding];
    [blit_encoders_ removeObjectForKey:object_key(event.object_id)];
    return ok;
  }

  bool set_render_pipeline_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t pipeline_state_id = 0;
    if (!required_u64(payload, "pipeline_state_id", pipeline_state_id)) {
      return fail("setRenderPipelineState is missing pipeline_state_id");
    }
    id<MTLRenderPipelineState> pipeline = render_pipeline_for_id(pipeline_state_id);
    if (encoder == nil || pipeline == nil) {
      return fail("setRenderPipelineState references missing encoder or pipeline");
    }
    [encoder setRenderPipelineState:pipeline];
    return true;
  }

  bool set_vertex_buffer(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto encoder_id = event.object_id;
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t buffer_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "buffer_id", buffer_id) ||
        !required_u64(payload, "offset", offset) ||
        !required_u32(payload, "index", index)) {
      return fail("setVertexBuffer is missing buffer_id, offset, or index");
    }
    id<MTLBuffer> buffer = buffer_for_id(buffer_id);
    if (encoder == nil || (buffer_id != 0 && buffer == nil)) {
      return fail("setVertexBuffer references missing encoder or buffer");
    }
    if (!restore_buffer_range_from_asset(event, buffer, payload)) {
      return false;
    }
    [encoder setVertexBuffer:buffer
                      offset:static_cast<NSUInteger>(offset)
                     atIndex:static_cast<NSUInteger>(index)];
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
      return fail("setVertexBytes is missing captured bytes");
    }
    std::uint64_t length = 0;
    if (!required_u64(nested, "length", length)) {
      return fail("setVertexBytes is missing length");
    }
    if (length == 0 || length > bytes.size()) {
      return fail("setVertexBytes has invalid byte length");
    }
    std::uint32_t index = 0;
    if (!required_u32(payload, "index", index)) {
      return fail("setVertexBytes is missing index");
    }

    [encoder setVertexBytes:bytes.data()
                     length:static_cast<NSUInteger>(length)
                    atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool set_vertex_buffer_offset(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setVertexBufferOffset references missing encoder");
    }
    std::uint64_t offset = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "offset", offset) ||
        !required_u32(payload, "index", index)) {
      return fail("setVertexBufferOffset is missing offset or index");
    }
    [encoder setVertexBufferOffset:static_cast<NSUInteger>(offset)
                           atIndex:static_cast<NSUInteger>(index)];
    record_buffer_offset(render_vertex_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_vertex_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t texture_id = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "texture_id", texture_id) ||
        !required_u32(payload, "index", index)) {
      return fail("setVertexTexture is missing texture_id or index");
    }
    id<MTLTexture> texture = texture_for_id(texture_id);
    if (encoder == nil || (texture_id != 0 && texture == nil)) {
      return fail("setVertexTexture references missing encoder or texture");
    }
    [encoder setVertexTexture:texture atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool set_vertex_sampler_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t sampler_state_id = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "sampler_state_id", sampler_state_id) ||
        !required_u32(payload, "index", index)) {
      return fail("setVertexSamplerState is missing sampler_state_id or index");
    }
    id<MTLSamplerState> sampler = sampler_for_id(sampler_state_id);
    if (encoder == nil || (sampler_state_id != 0 && sampler == nil)) {
      return fail("setVertexSamplerState references missing encoder or sampler");
    }
    [encoder setVertexSamplerState:sampler atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool set_fragment_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t texture_id = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "texture_id", texture_id) ||
        !required_u32(payload, "index", index)) {
      return fail("setFragmentTexture is missing texture_id or index");
    }
    id<MTLTexture> texture = texture_for_id(texture_id);
    if (encoder == nil || (texture_id != 0 && texture == nil)) {
      return fail("setFragmentTexture references missing encoder or texture");
    }
    [encoder setFragmentTexture:texture atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool set_fragment_buffer(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto encoder_id = event.object_id;
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t buffer_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "buffer_id", buffer_id) ||
        !required_u64(payload, "offset", offset) ||
        !required_u32(payload, "index", index)) {
      return fail("setFragmentBuffer is missing buffer_id, offset, or index");
    }
    id<MTLBuffer> buffer = buffer_for_id(buffer_id);
    if (encoder == nil || (buffer_id != 0 && buffer == nil)) {
      return fail("setFragmentBuffer references missing encoder or buffer");
    }
    if (!restore_buffer_range_from_asset(event, buffer, payload)) {
      return false;
    }
    [encoder setFragmentBuffer:buffer
                        offset:static_cast<NSUInteger>(offset)
                       atIndex:static_cast<NSUInteger>(index)];
    record_buffer_binding(render_fragment_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_fragment_sampler_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t sampler_state_id = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "sampler_state_id", sampler_state_id) ||
        !required_u32(payload, "index", index)) {
      return fail("setFragmentSamplerState is missing sampler_state_id or index");
    }
    id<MTLSamplerState> sampler = sampler_for_id(sampler_state_id);
    if (encoder == nil || (sampler_state_id != 0 && sampler == nil)) {
      return fail("setFragmentSamplerState references missing encoder or sampler");
    }
    [encoder setFragmentSamplerState:sampler atIndex:static_cast<NSUInteger>(index)];
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
      return fail("setFragmentBytes is missing captured bytes");
    }
    std::uint64_t length = 0;
    if (!required_u64(nested, "length", length)) {
      return fail("setFragmentBytes is missing length");
    }
    if (length == 0 || length > bytes.size()) {
      return fail("setFragmentBytes has invalid byte length");
    }
    std::uint32_t index = 0;
    if (!required_u32(payload, "index", index)) {
      return fail("setFragmentBytes is missing index");
    }

    [encoder setFragmentBytes:bytes.data()
                       length:static_cast<NSUInteger>(length)
                      atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool set_fragment_buffer_offset(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setFragmentBufferOffset references missing encoder");
    }
    std::uint64_t offset = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "offset", offset) ||
        !required_u32(payload, "index", index)) {
      return fail("setFragmentBufferOffset is missing offset or index");
    }
    [encoder setFragmentBufferOffset:static_cast<NSUInteger>(offset)
                             atIndex:static_cast<NSUInteger>(index)];
    record_buffer_offset(render_fragment_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_cull_mode(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setCullMode references missing encoder");
    }
    const auto it = payload.find("cull_mode");
    if (it == payload.end() || !it->is_number()) {
      return fail("setCullMode is missing cull_mode");
    }
    [encoder setCullMode:static_cast<MTLCullMode>(it->get<std::uint32_t>())];
    return true;
  }

  bool set_front_facing_winding(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setFrontFacingWinding references missing encoder");
    }
    const auto it = payload.find("winding");
    if (it == payload.end() || !it->is_number()) {
      return fail("setFrontFacingWinding is missing winding");
    }
    [encoder setFrontFacingWinding:static_cast<MTLWinding>(it->get<std::uint32_t>())];
    return true;
  }

  bool set_triangle_fill_mode(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setTriangleFillMode references missing encoder");
    }
    const auto it = payload.find("fill_mode");
    if (it == payload.end() || !it->is_number()) {
      return fail("setTriangleFillMode is missing fill_mode");
    }
    [encoder setTriangleFillMode:static_cast<MTLTriangleFillMode>(it->get<std::uint32_t>())];
    return true;
  }

  bool use_resource(std::uint64_t encoder_id, const json &payload)
  {
    std::uint64_t resource_id = 0;
    std::uint32_t usage_value = 0;
    if (!required_u64(payload, "resource_id", resource_id) ||
        !required_u32(payload, "usage", usage_value)) {
      return fail("useResource is missing resource_id or usage");
    }
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
      return fail("useResource references missing resource");
    }

    const auto usage = static_cast<MTLResourceUsage>(usage_value);
    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(encoder_id);
    if (render_encoder != nil) {
      std::uint32_t stages = 0;
      if (!required_u32(payload, "stages", stages)) {
        return fail("useResource is missing stages");
      }
      [render_encoder useResource:resource
                            usage:usage
                           stages:static_cast<MTLRenderStages>(stages)];
      return true;
    }

    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(encoder_id);
    if (compute_encoder != nil) {
      [compute_encoder useResource:resource usage:usage];
      return true;
    }

    return fail("useResource references missing encoder");
  }

  bool use_resources(std::uint64_t encoder_id, const json &payload)
  {
    const auto resources = payload.find("resource_ids");
    if (resources == payload.end() || !resources->is_array()) {
      return fail("useResources is missing resource_ids");
    }
    for (const auto &resource_id : *resources) {
      if (!resource_id.is_number_unsigned() && !resource_id.is_number_integer()) {
        return fail("useResources has invalid resource id");
      }
      json single = payload;
      single["resource_id"] = resource_id.get<std::uint64_t>();
      if (!use_resource(encoder_id, single)) {
        return false;
      }
    }
    return true;
  }

  bool memory_barrier(std::uint64_t encoder_id, const json &payload)
  {
    const json barrier = parse_nested_json(payload, "payload");
    std::uint32_t scope_value = 0;
    std::uint32_t stages_before = 0;
    std::uint32_t stages_after = 0;
    if (!required_u32(barrier, "scope", scope_value) ||
        !required_u32(barrier, "stages_before", stages_before) ||
        !required_u32(barrier, "stages_after", stages_after)) {
      return fail("memoryBarrier is missing scope, stages_before, or stages_after");
    }
    const auto scope = static_cast<MTLBarrierScope>(scope_value);

    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(encoder_id);
    if (render_encoder != nil) {
      [render_encoder memoryBarrierWithScope:scope
                                 afterStages:static_cast<MTLRenderStages>(stages_before)
                                beforeStages:static_cast<MTLRenderStages>(stages_after)];
      return true;
    }

    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(encoder_id);
    if (compute_encoder != nil) {
      [compute_encoder memoryBarrierWithScope:scope];
      return true;
    }

    return fail("memoryBarrier references missing encoder");
  }

  bool update_fence(std::uint64_t encoder_id, const json &payload)
  {
    std::uint64_t fence_id = 0;
    std::uint32_t stages = 0;
    if (!required_u64(payload, "fence_id", fence_id) ||
        !required_u32(payload, "stages", stages)) {
      return fail("updateFence is missing fence_id or stages");
    }
    id<MTLFence> fence = fence_for_id(fence_id);
    if (fence == nil) {
      return fail("updateFence references missing fence");
    }

    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(encoder_id);
    if (render_encoder != nil) {
      [render_encoder updateFence:fence
                      afterStages:static_cast<MTLRenderStages>(stages)];
      return true;
    }

    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(encoder_id);
    if (compute_encoder != nil) {
      [compute_encoder updateFence:fence];
      return true;
    }

    id<MTLBlitCommandEncoder> blit_encoder = blit_encoder_for_id(encoder_id);
    if (blit_encoder != nil) {
      [blit_encoder updateFence:fence];
      return true;
    }

    return fail("updateFence references missing encoder");
  }

  bool wait_for_fence(std::uint64_t encoder_id, const json &payload)
  {
    std::uint64_t fence_id = 0;
    std::uint32_t stages = 0;
    if (!required_u64(payload, "fence_id", fence_id) ||
        !required_u32(payload, "stages", stages)) {
      return fail("waitForFence is missing fence_id or stages");
    }
    id<MTLFence> fence = fence_for_id(fence_id);
    if (fence == nil) {
      return fail("waitForFence references missing fence");
    }

    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(encoder_id);
    if (render_encoder != nil) {
      [render_encoder waitForFence:fence
                       beforeStages:static_cast<MTLRenderStages>(stages)];
      return true;
    }

    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(encoder_id);
    if (compute_encoder != nil) {
      [compute_encoder waitForFence:fence];
      return true;
    }

    id<MTLBlitCommandEncoder> blit_encoder = blit_encoder_for_id(encoder_id);
    if (blit_encoder != nil) {
      [blit_encoder waitForFence:fence];
      return true;
    }

    return fail("waitForFence references missing encoder");
  }

  bool fence_ops(std::uint64_t encoder_id, const json &payload)
  {
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array() || ops->empty()) {
      return fail("fenceOps is missing ops");
    }

    for (const auto &op : *ops) {
      if (!op.is_array() || op.size() < 3 || !op[0].is_string() ||
          (!op[1].is_number_unsigned() && !op[1].is_number_integer()) ||
          (!op[2].is_number_unsigned() && !op[2].is_number_integer())) {
        return fail("fenceOps has invalid op");
      }
      json single{{"fence_id", op[1].get<std::uint64_t>()}, {"stages", op[2].get<std::uint32_t>()}};
      if (op[0].get_ref<const std::string &>() == "update") {
        if (!update_fence(encoder_id, single)) {
          return false;
        }
      } else if (op[0].get_ref<const std::string &>() == "wait") {
        if (!wait_for_fence(encoder_id, single)) {
          return false;
        }
      } else {
        return fail("fenceOps has unsupported op " + op[0].get<std::string>());
      }
    }
    return true;
  }

  bool push_debug_group(std::uint64_t object_id, const json &payload)
  {
    NSString *label = [NSString stringWithUTF8String:payload.value("label", std::string()).c_str()];
    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(object_id);
    if (render_encoder != nil) {
      [render_encoder pushDebugGroup:label];
      return true;
    }
    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(object_id);
    if (compute_encoder != nil) {
      [compute_encoder pushDebugGroup:label];
      return true;
    }
    id<MTLBlitCommandEncoder> blit_encoder = blit_encoder_for_id(object_id);
    if (blit_encoder != nil) {
      [blit_encoder pushDebugGroup:label];
      return true;
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(object_id);
    if (command_buffer != nil && [command_buffer respondsToSelector:@selector(pushDebugGroup:)]) {
      [command_buffer pushDebugGroup:label];
    }
    return true;
  }

  bool pop_debug_group(std::uint64_t object_id)
  {
    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(object_id);
    if (render_encoder != nil) {
      [render_encoder popDebugGroup];
      return true;
    }
    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(object_id);
    if (compute_encoder != nil) {
      [compute_encoder popDebugGroup];
      return true;
    }
    id<MTLBlitCommandEncoder> blit_encoder = blit_encoder_for_id(object_id);
    if (blit_encoder != nil) {
      [blit_encoder popDebugGroup];
      return true;
    }
    id<MTLCommandBuffer> command_buffer = command_buffer_for_id(object_id);
    if (command_buffer != nil && [command_buffer respondsToSelector:@selector(popDebugGroup)]) {
      [command_buffer popDebugGroup];
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
      return fail("setViewport is missing viewports");
    }

    std::vector<MTLViewport> viewports;
    viewports.reserve(it->size());
    for (const auto &viewport : *it) {
      MTLViewport parsed{};
      if (!viewport_from_json_array(viewport, parsed)) {
        return fail("setViewport has invalid viewports");
      }
      viewports.push_back(parsed);
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
      return fail("setScissorRect is missing rects");
    }

    std::vector<MTLScissorRect> rects;
    rects.reserve(it->size());
    for (const auto &rect : *it) {
      MTLScissorRect parsed{};
      if (!scissor_from_json_array(rect, parsed)) {
        return fail("setScissorRect has invalid rects");
      }
      rects.push_back(parsed);
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
    const auto require_number = [&](const char *field) -> bool {
      const auto it = payload.find(field);
      if (it == payload.end() || !it->is_number()) {
        return fail(std::string("dxmt_set_rasterizer_state is missing ") + field);
      }
      return true;
    };
    if (!require_number("fill_mode") ||
        !require_number("cull_mode") ||
        !require_number("depth_clip_mode") ||
        !require_number("depth_bias") ||
        !require_number("slope_scale") ||
        !require_number("depth_bias_clamp") ||
        !require_number("winding")) {
      return false;
    }
    [encoder setTriangleFillMode:static_cast<MTLTriangleFillMode>(payload["fill_mode"].get<std::uint32_t>())];
    [encoder setCullMode:static_cast<MTLCullMode>(payload["cull_mode"].get<std::uint32_t>())];
    [encoder setDepthClipMode:static_cast<MTLDepthClipMode>(payload["depth_clip_mode"].get<std::uint32_t>())];
    [encoder setDepthBias:payload["depth_bias"].get<float>()
               slopeScale:payload["slope_scale"].get<float>()
                    clamp:payload["depth_bias_clamp"].get<float>()];
    [encoder setFrontFacingWinding:static_cast<MTLWinding>(payload["winding"].get<std::uint32_t>())];
    return true;
  }

  bool set_depth_stencil_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("set depth stencil state references missing encoder");
    }
    const auto depth_stencil_field = payload.find("depth_stencil_state_id");
    if (depth_stencil_field == payload.end() ||
        (!depth_stencil_field->is_number_unsigned() && !depth_stencil_field->is_number_integer())) {
      return fail("dxmt_set_depth_stencil_state is missing depth_stencil_state_id");
    }
    id<MTLDepthStencilState> state =
        depth_stencil_state_for_id(depth_stencil_field->get<std::uint64_t>());
    if (state != nil) {
      [encoder setDepthStencilState:state];
    }
    const auto stencil_ref = payload.find("stencil_ref");
    if (stencil_ref == payload.end() || !stencil_ref->is_number()) {
      return fail("dxmt_set_depth_stencil_state is missing stencil_ref");
    }
    [encoder setStencilReferenceValue:static_cast<std::uint32_t>(stencil_ref->get<std::uint32_t>())];
    return true;
  }

  bool set_blend_factor(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("set blend factor references missing encoder");
    }
    const auto require_number = [&](const char *field) -> bool {
      const auto it = payload.find(field);
      if (it == payload.end() || !it->is_number()) {
        return fail(std::string("dxmt_set_blend_factor is missing ") + field);
      }
      return true;
    };
    if (!require_number("red") ||
        !require_number("green") ||
        !require_number("blue") ||
        !require_number("alpha") ||
        !require_number("stencil_ref")) {
      return false;
    }
    [encoder setBlendColorRed:payload["red"].get<float>()
                        green:payload["green"].get<float>()
                         blue:payload["blue"].get<float>()
                        alpha:payload["alpha"].get<float>()];
    [encoder setStencilReferenceValue:static_cast<std::uint32_t>(payload["stencil_ref"].get<std::uint32_t>())];
    return true;
  }

  bool set_compute_pipeline_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    std::uint64_t pipeline_state_id = 0;
    if (!required_u64(payload, "pipeline_state_id", pipeline_state_id)) {
      return fail("setComputePipelineState is missing pipeline_state_id");
    }
    id<MTLComputePipelineState> pipeline = compute_pipeline_for_id(pipeline_state_id);
    if (encoder == nil || pipeline == nil) {
      return fail("setComputePipelineState references missing encoder or pipeline");
    }
    [encoder setComputePipelineState:pipeline];
    return true;
  }

  bool set_compute_buffer(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto encoder_id = event.object_id;
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    std::uint64_t buffer_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "buffer_id", buffer_id) ||
        !required_u64(payload, "offset", offset) ||
        !required_u32(payload, "index", index)) {
      return fail("setComputeBuffer is missing buffer_id, offset, or index");
    }
    id<MTLBuffer> buffer = buffer_for_id(buffer_id);
    if (encoder == nil || (buffer_id != 0 && buffer == nil)) {
      return fail("setComputeBuffer references missing encoder or buffer");
    }
    if (!restore_buffer_range_from_asset(event, buffer, payload)) {
      return false;
    }
    [encoder setBuffer:buffer
                offset:static_cast<NSUInteger>(offset)
               atIndex:static_cast<NSUInteger>(index)];
    record_buffer_binding(compute_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_compute_buffer_offset(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("setComputeBufferOffset references missing compute encoder");
    }
    std::uint64_t offset = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "offset", offset) ||
        !required_u32(payload, "index", index)) {
      return fail("setComputeBufferOffset is missing offset or index");
    }
    [encoder setBufferOffset:static_cast<NSUInteger>(offset)
                     atIndex:static_cast<NSUInteger>(index)];
    record_buffer_offset(compute_buffer_bindings_[encoder_id], payload);
    return true;
  }

  bool set_compute_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    std::uint64_t texture_id = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "texture_id", texture_id) ||
        !required_u32(payload, "index", index)) {
      return fail("setComputeTexture is missing texture_id or index");
    }
    id<MTLTexture> texture = texture_for_id(texture_id);
    if (encoder == nil || (texture_id != 0 && texture == nil)) {
      return fail("setComputeTexture references missing encoder or texture");
    }
    [encoder setTexture:texture atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool set_compute_sampler_state(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    std::uint64_t sampler_state_id = 0;
    std::uint32_t index = 0;
    if (!required_u64(payload, "sampler_state_id", sampler_state_id) ||
        !required_u32(payload, "index", index)) {
      return fail("setComputeSamplerState is missing sampler_state_id or index");
    }
    id<MTLSamplerState> sampler = sampler_for_id(sampler_state_id);
    if (encoder == nil || (sampler_state_id != 0 && sampler == nil)) {
      return fail("setComputeSamplerState references missing encoder or sampler");
    }
    [encoder setSamplerState:sampler atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool encoder_state(std::uint64_t encoder_id, const json &payload)
  {
    const auto kind = payload.value("kind", std::string());
    if (kind == "dxmt_set_rasterizer_state") {
      return set_rasterizer_state(encoder_id, payload);
    }
    if (kind == "dxmt_set_depth_stencil_state") {
      return set_depth_stencil_state(encoder_id, payload);
    }
    if (kind == "dxmt_set_blend_factor") {
      return set_blend_factor(encoder_id, payload);
    }
    if (kind == "dxmt_set_viewports") {
      return set_viewports(encoder_id, payload);
    }
    if (kind == "dxmt_set_scissor_rects") {
      return set_scissor_rects(encoder_id, payload);
    }
    return true;
  }

  bool draw_primitives(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("drawPrimitives references missing render encoder");
    }
    std::uint32_t primitive_type = 0;
    std::uint32_t vertex_start = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t base_instance = 0;
    if (!required_u32(payload, "primitive_type", primitive_type) ||
        !required_u32(payload, "vertex_start", vertex_start) ||
        !required_u32(payload, "vertex_count", vertex_count) ||
        !required_u32(payload, "instance_count", instance_count) ||
        !required_u32(payload, "base_instance", base_instance)) {
      return fail("drawPrimitives is missing replay metadata");
    }
    MTLPrimitiveType mtl_primitive_type = MTLPrimitiveTypePoint;
    if (!primitive_type_from_integer(primitive_type, mtl_primitive_type)) {
      return fail("drawPrimitives has invalid primitive_type");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawPrimitives:mtl_primitive_type
                vertexStart:static_cast<NSUInteger>(vertex_start)
                vertexCount:static_cast<NSUInteger>(vertex_count)
              instanceCount:static_cast<NSUInteger>(instance_count)
               baseInstance:static_cast<NSUInteger>(base_instance)];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool draw_indexed_primitives(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t index_buffer_id = 0;
    std::uint64_t index_buffer_offset = 0;
    std::uint32_t primitive_type = 0;
    std::uint32_t index_count = 0;
    std::uint32_t index_type = 0;
    std::uint32_t instance_count = 0;
    std::int32_t base_vertex = 0;
    std::uint32_t base_instance = 0;
    if (!required_u64(payload, "index_buffer_id", index_buffer_id) ||
        !required_u64(payload, "index_buffer_offset", index_buffer_offset) ||
        !required_u32(payload, "primitive_type", primitive_type) ||
        !required_u32(payload, "index_count", index_count) ||
        !required_u32(payload, "index_type", index_type) ||
        !required_u32(payload, "instance_count", instance_count) ||
        !required_i32(payload, "base_vertex", base_vertex) ||
        !required_u32(payload, "base_instance", base_instance)) {
      return fail("drawIndexedPrimitives is missing replay metadata");
    }
    id<MTLBuffer> index_buffer = buffer_for_id(index_buffer_id);
    if (encoder == nil || index_buffer == nil) {
      return fail("drawIndexedPrimitives references missing encoder or index buffer");
    }
    MTLPrimitiveType mtl_primitive_type = MTLPrimitiveTypePoint;
    MTLIndexType mtl_index_type = MTLIndexTypeUInt16;
    if (!primitive_type_from_integer(primitive_type, mtl_primitive_type) ||
        !index_type_from_integer(index_type, mtl_index_type)) {
      return fail("drawIndexedPrimitives has invalid replay enum metadata");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawIndexedPrimitives:mtl_primitive_type
                         indexCount:static_cast<NSUInteger>(index_count)
                          indexType:mtl_index_type
                        indexBuffer:index_buffer
                  indexBufferOffset:static_cast<NSUInteger>(index_buffer_offset)
                      instanceCount:static_cast<NSUInteger>(instance_count)
                         baseVertex:base_vertex
                       baseInstance:static_cast<NSUInteger>(base_instance)];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool draw_primitives_indirect(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t indirect_buffer_id = 0;
    std::uint64_t indirect_buffer_offset = 0;
    std::uint32_t primitive_type = 0;
    if (!required_u64(payload, "indirect_buffer_id", indirect_buffer_id) ||
        !required_u64(payload, "indirect_buffer_offset", indirect_buffer_offset) ||
        !required_u32(payload, "primitive_type", primitive_type)) {
      return fail("drawPrimitivesIndirect is missing replay metadata");
    }
    id<MTLBuffer> indirect_buffer = buffer_for_id(indirect_buffer_id);
    if (encoder == nil || indirect_buffer == nil) {
      return fail("drawPrimitivesIndirect references missing encoder or indirect buffer");
    }
    MTLPrimitiveType mtl_primitive_type = MTLPrimitiveTypePoint;
    if (!primitive_type_from_integer(primitive_type, mtl_primitive_type)) {
      return fail("drawPrimitivesIndirect has invalid primitive_type");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawPrimitives:mtl_primitive_type
             indirectBuffer:indirect_buffer
       indirectBufferOffset:static_cast<NSUInteger>(indirect_buffer_offset)];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool draw_indexed_primitives_indirect(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    std::uint64_t index_buffer_id = 0;
    std::uint64_t indirect_buffer_id = 0;
    std::uint64_t index_buffer_offset = 0;
    std::uint64_t indirect_buffer_offset = 0;
    std::uint32_t primitive_type = 0;
    std::uint32_t index_type = 0;
    if (!required_u64(payload, "index_buffer_id", index_buffer_id) ||
        !required_u64(payload, "indirect_buffer_id", indirect_buffer_id) ||
        !required_u64(payload, "index_buffer_offset", index_buffer_offset) ||
        !required_u64(payload, "indirect_buffer_offset", indirect_buffer_offset) ||
        !required_u32(payload, "primitive_type", primitive_type) ||
        !required_u32(payload, "index_type", index_type)) {
      return fail("drawIndexedPrimitivesIndirect is missing replay metadata");
    }
    id<MTLBuffer> index_buffer = buffer_for_id(index_buffer_id);
    id<MTLBuffer> indirect_buffer = buffer_for_id(indirect_buffer_id);
    if (encoder == nil || index_buffer == nil || indirect_buffer == nil) {
      return fail("drawIndexedPrimitivesIndirect references missing encoder or buffers");
    }
    MTLPrimitiveType mtl_primitive_type = MTLPrimitiveTypePoint;
    MTLIndexType mtl_index_type = MTLIndexTypeUInt16;
    if (!primitive_type_from_integer(primitive_type, mtl_primitive_type) ||
        !index_type_from_integer(index_type, mtl_index_type)) {
      return fail("drawIndexedPrimitivesIndirect has invalid replay enum metadata");
    }
    patch_render_bound_buffers(encoder_id);
    [encoder drawIndexedPrimitives:mtl_primitive_type
                         indexType:mtl_index_type
                       indexBuffer:index_buffer
                 indexBufferOffset:static_cast<NSUInteger>(index_buffer_offset)
                    indirectBuffer:indirect_buffer
              indirectBufferOffset:static_cast<NSUInteger>(indirect_buffer_offset)];
    finish_draw_scope(encoder_id);
    return true;
  }

  bool dispatch_threadgroups(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("dispatchThreadgroups references missing compute encoder");
    }
    std::uint32_t tgx = 0;
    std::uint32_t tgy = 0;
    std::uint32_t tgz = 0;
    std::uint32_t tx = 0;
    std::uint32_t ty = 0;
    std::uint32_t tz = 0;
    if (!required_u32(payload, "tgx", tgx) ||
        !required_u32(payload, "tgy", tgy) ||
        !required_u32(payload, "tgz", tgz) ||
        !required_u32(payload, "tx", tx) ||
        !required_u32(payload, "ty", ty) ||
        !required_u32(payload, "tz", tz)) {
      return fail("dispatchThreadgroups is missing replay metadata");
    }
    patch_compute_bound_buffers(encoder_id);
    [encoder dispatchThreadgroups:MTLSizeMake(tgx, tgy, tgz)
          threadsPerThreadgroup:MTLSizeMake(tx, ty, tz)];
    return true;
  }

  bool dispatch_threadgroups_indirect(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    std::uint64_t indirect_buffer_id = 0;
    std::uint64_t indirect_buffer_offset = 0;
    std::uint32_t tx = 0;
    std::uint32_t ty = 0;
    std::uint32_t tz = 0;
    if (!required_u64(payload, "indirect_buffer_id", indirect_buffer_id) ||
        !required_u64(payload, "indirect_buffer_offset", indirect_buffer_offset) ||
        !required_u32(payload, "tx", tx) ||
        !required_u32(payload, "ty", ty) ||
        !required_u32(payload, "tz", tz)) {
      return fail("dispatchThreadgroupsIndirect is missing replay metadata");
    }
    id<MTLBuffer> indirect_buffer = buffer_for_id(indirect_buffer_id);
    if (encoder == nil || indirect_buffer == nil) {
      return fail("dispatchThreadgroupsIndirect references missing encoder or indirect buffer");
    }
    patch_compute_bound_buffers(encoder_id);
    [encoder dispatchThreadgroupsWithIndirectBuffer:indirect_buffer
                               indirectBufferOffset:static_cast<NSUInteger>(indirect_buffer_offset)
                              threadsPerThreadgroup:MTLSizeMake(tx, ty, tz)];
    return true;
  }

  bool dispatch_threads(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLComputeCommandEncoder> encoder = compute_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("dispatchThreads references missing compute encoder");
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t threads_per_group_width = 0;
    std::uint32_t threads_per_group_height = 0;
    std::uint32_t threads_per_group_depth = 0;
    if (!required_u32(payload, "width", width) ||
        !required_u32(payload, "height", height) ||
        !required_u32(payload, "depth", depth) ||
        !required_u32(payload, "threads_per_group_width", threads_per_group_width) ||
        !required_u32(payload, "threads_per_group_height", threads_per_group_height) ||
        !required_u32(payload, "threads_per_group_depth", threads_per_group_depth)) {
      return fail("dispatchThreads is missing replay metadata");
    }
    patch_compute_bound_buffers(encoder_id);
    [encoder dispatchThreads:MTLSizeMake(width, height, depth)
        threadsPerThreadgroup:MTLSizeMake(threads_per_group_width,
                                          threads_per_group_height,
                                          threads_per_group_depth)];
    return true;
  }

  bool dispatch_threads_per_tile(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLRenderCommandEncoder> encoder = render_encoder_for_id(encoder_id);
    if (encoder == nil) {
      return fail("dispatchThreadsPerTile references missing render encoder");
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!required_u32(payload, "width", width) ||
        !required_u32(payload, "height", height)) {
      return fail("dispatchThreadsPerTile is missing replay metadata");
    }
    [encoder dispatchThreadsPerTile:MTLSizeMake(width, height, 1)];
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
      return fail("setComputeBytes is missing captured bytes");
    }
    std::uint64_t length = 0;
    if (!required_u64(payload, "length", length)) {
      return fail("setComputeBytes is missing length");
    }
    if (length == 0 || length > bytes.size()) {
      return fail("setComputeBytes has invalid byte length");
    }
    std::uint32_t index = 0;
    if (!required_u32(payload, "index", index)) {
      return fail("setComputeBytes is missing index");
    }
    [encoder setBytes:bytes.data()
               length:static_cast<NSUInteger>(length)
              atIndex:static_cast<NSUInteger>(index)];
    return true;
  }

  bool copy_buffer(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto encoder_id = event.object_id;
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    std::uint64_t source_buffer_id = 0;
    std::uint64_t destination_buffer_id = 0;
    std::uint64_t source_offset = 0;
    std::uint64_t destination_offset = 0;
    std::uint64_t size = 0;
    if (!required_u64(payload, "source_buffer_id", source_buffer_id) ||
        !required_u64(payload, "destination_buffer_id", destination_buffer_id) ||
        !required_u64(payload, "source_offset", source_offset) ||
        !required_u64(payload, "destination_offset", destination_offset) ||
        !required_u64(payload, "size", size)) {
      return fail("copyFromBuffer is missing replay metadata");
    }
    id<MTLBuffer> source_buffer = buffer_for_id(source_buffer_id);
    id<MTLBuffer> destination_buffer = buffer_for_id(destination_buffer_id);
    if (encoder == nil || source_buffer == nil || destination_buffer == nil) {
      return fail("copyFromBuffer references missing encoder or buffers");
    }
    if (!restore_buffer_range_from_asset(event, source_buffer, payload)) {
      return false;
    }
    [encoder copyFromBuffer:source_buffer
               sourceOffset:static_cast<NSUInteger>(source_offset)
                   toBuffer:destination_buffer
          destinationOffset:static_cast<NSUInteger>(destination_offset)
                       size:static_cast<NSUInteger>(size)];
    return true;
  }

  bool copy_texture(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    std::uint64_t source_texture_id = 0;
    std::uint64_t destination_texture_id = 0;
    if (!required_u64(payload, "source_texture_id", source_texture_id) ||
        !required_u64(payload, "destination_texture_id", destination_texture_id)) {
      return fail("copyFromTexture is missing source_texture_id or destination_texture_id");
    }
    id<MTLTexture> source_texture = texture_for_id(source_texture_id);
    id<MTLTexture> destination_texture = texture_for_id(destination_texture_id);
    if (encoder == nil || source_texture == nil || destination_texture == nil) {
      return fail("copyFromTexture references missing encoder or textures");
    }

    const auto nested = parse_nested_json(payload, "payload");
    std::uint32_t source_slice = 0;
    std::uint32_t source_level = 0;
    std::uint32_t destination_slice = 0;
    std::uint32_t destination_level = 0;
    if (!required_u32(nested, "source_slice", source_slice) ||
        !required_u32(nested, "source_level", source_level) ||
        !required_u32(nested, "destination_slice", destination_slice) ||
        !required_u32(nested, "destination_level", destination_level)) {
      return fail("copyFromTexture is missing replay metadata");
    }
    MTLOrigin source_origin = {};
    MTLSize source_size = {};
    MTLOrigin destination_origin = {};
    if (!origin_from_json_array(nested, "source_origin", source_origin) ||
        !size_from_json_array(nested, "source_size", source_size) ||
        !origin_from_json_array(nested, "destination_origin", destination_origin)) {
      return fail("copyFromTexture is missing replay region metadata");
    }
    [encoder copyFromTexture:source_texture
                 sourceSlice:static_cast<NSUInteger>(source_slice)
                 sourceLevel:static_cast<NSUInteger>(source_level)
                sourceOrigin:source_origin
                  sourceSize:source_size
                   toTexture:destination_texture
            destinationSlice:static_cast<NSUInteger>(destination_slice)
            destinationLevel:static_cast<NSUInteger>(destination_level)
           destinationOrigin:destination_origin];
    return true;
  }

  bool blit_fill(std::uint64_t encoder_id, const json &payload)
  {
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    std::uint64_t buffer_id = 0;
    std::uint64_t range_start = 0;
    std::uint64_t range_length = 0;
    std::uint32_t value = 0;
    if (!required_u64(payload, "buffer_id", buffer_id) ||
        !required_u64(payload, "range_start", range_start) ||
        !required_u64(payload, "range_length", range_length) ||
        !required_u32(payload, "value", value) ||
        value > std::numeric_limits<std::uint8_t>::max()) {
      return fail("fillBuffer is missing replay metadata");
    }
    id<MTLBuffer> buffer = buffer_for_id(buffer_id);
    if (encoder == nil || buffer == nil) {
      return fail("fillBuffer references missing encoder or buffer");
    }
    [encoder fillBuffer:buffer
                  range:NSMakeRange(static_cast<NSUInteger>(range_start),
                                    static_cast<NSUInteger>(range_length))
                  value:static_cast<uint8_t>(value)];
    return true;
  }

  bool blit_batch(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto encoder_id = event.object_id;
    const auto ops = payload.find("ops");
    const auto fence_ops = payload.find("fence_ops");
    if ((ops == payload.end() || !ops->is_array() || ops->empty()) &&
        (fence_ops == payload.end() || !fence_ops->is_object())) {
      return fail("blitBatch is missing ops");
    }

    if (ops != payload.end()) for (const auto &op : *ops) {
      if (op.is_array()) {
        if (op.size() < 2) {
          return fail("blitBatch array op must include kind and fence id");
        }
        std::string kind;
        if (op[0].is_string()) {
          kind = op[0].get<std::string>();
        } else {
          const auto numeric_kind = json_u64(op[0]);
          if (numeric_kind == 1) {
            kind = "wait_fence";
          } else if (numeric_kind == 2) {
            kind = "update_fence";
          }
        }
        if (kind != "wait_fence" && kind != "update_fence") {
          return fail("unsupported blitBatch array op");
        }
        if (json_u64(op[1]) == 0) {
          return fail(kind + " blitBatch op is missing fence_id");
        }
        continue;
      }
      if (!op.is_object()) {
        return fail("blitBatch op must be an object");
      }
      const auto kind = op.value("op", std::string());
      if (kind == "copy_texture") {
        if (!copy_texture(encoder_id, op)) {
          return false;
        }
      } else if (kind == "fill_buffer") {
        if (!blit_fill(encoder_id, op)) {
          return false;
        }
      } else if (kind == "wait_fence" || kind == "update_fence") {
        if (json_u64(op.value("fence_id", json(nullptr))) == 0) {
          return fail(kind + " blitBatch op is missing fence_id");
        }
        continue;
      } else {
        return fail("unsupported blitBatch op " + (kind.empty() ? std::string("<missing>") : kind));
      }
    }
    const auto copy_texture_ops = payload.find("copy_texture_ops");
    if (copy_texture_ops != payload.end()) {
      if (!copy_texture_ops->is_array()) {
        return fail("blitBatch compact copy_texture_ops must be an array");
      }
      for (const auto &op : *copy_texture_ops) {
        if (!op.is_array() || op.size() < 15) {
          return fail("blitBatch compact copy_texture op must include 15 fields");
        }
        json expanded = {
            {"source_texture_id", json_u64(op[0])},
            {"destination_texture_id", json_u64(op[1])},
            {"payload", json{
                            {"source_texture", json_u64(op[0])},
                            {"destination_texture", json_u64(op[1])},
                            {"source_origin", json::array({json_u64(op[2]), json_u64(op[3]), json_u64(op[4])})},
                            {"source_size", json::array({json_u64(op[5]), json_u64(op[6]), json_u64(op[7])})},
                            {"source_slice", json_u64(op[8])},
                            {"source_level", json_u64(op[9])},
                            {"destination_origin", json::array({json_u64(op[10]), json_u64(op[11]), json_u64(op[12])})},
                            {"destination_slice", json_u64(op[13])},
                            {"destination_level", json_u64(op[14])},
                        }.dump()},
        };
        if (!copy_texture(encoder_id, expanded)) {
          return false;
        }
      }
    }
    if (fence_ops != payload.end()) {
      if (fence_ops->value("schema", std::string()) == "blit-fence-v2") {
        for (const auto *key : {"wait_fences", "update_fences"}) {
          const auto fences = fence_ops->find(key);
          if (fences == fence_ops->end() || !fences->is_array()) {
            return fail("blitBatch compact fence list is missing");
          }
          for (const auto &fence : *fences) {
            if (json_u64(fence) == 0) {
              return fail("compact blitBatch fence op is missing fence_id");
            }
          }
        }
      } else {
        const auto compact_ops = fence_ops->find("ops");
        if (compact_ops == fence_ops->end() || !compact_ops->is_array()) {
          return fail("blitBatch compact fence ops must include ops");
        }
        for (const auto &op : *compact_ops) {
          if (!op.is_array() || op.size() < 2) {
            return fail("blitBatch compact fence op must include kind and fence id");
          }
          const auto numeric_kind = json_u64(op[0]);
          if (numeric_kind != 1 && numeric_kind != 2) {
            return fail("unsupported blitBatch compact fence op");
          }
          if (json_u64(op[1]) == 0) {
            return fail("compact blitBatch fence op is missing fence_id");
          }
        }
      }
    }
    return true;
  }

  bool copy_buffer_to_texture(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto encoder_id = event.object_id;
    id<MTLBlitCommandEncoder> encoder = blit_encoder_for_id(encoder_id);
    std::uint64_t source_buffer_id = 0;
    std::uint64_t destination_texture_id = 0;
    std::uint64_t source_offset = 0;
    std::uint32_t source_bytes_per_row = 0;
    std::uint32_t source_bytes_per_image = 0;
    std::uint32_t destination_slice = 0;
    std::uint32_t destination_level = 0;
    if (!required_u64(payload, "source_buffer", source_buffer_id) ||
        !required_u64(payload, "destination_texture", destination_texture_id) ||
        !required_u64(payload, "source_offset", source_offset) ||
        !required_u32(payload, "source_bytes_per_row", source_bytes_per_row) ||
        !required_u32(payload, "source_bytes_per_image", source_bytes_per_image) ||
        !required_u32(payload, "destination_slice", destination_slice) ||
        !required_u32(payload, "destination_level", destination_level)) {
      return fail("copyFromBuffer toTexture is missing replay metadata");
    }
    id<MTLBuffer> source_buffer = buffer_for_id(source_buffer_id);
    id<MTLTexture> destination_texture = texture_for_id(destination_texture_id);
    if (encoder == nil || source_buffer == nil || destination_texture == nil) {
      return fail("copyFromBuffer toTexture references missing encoder, buffer, or texture");
    }
    if (!restore_buffer_range_from_asset(event, source_buffer, payload)) {
      return false;
    }

    MTLSize source_size = {};
    MTLOrigin destination_origin = {};
    if (!size_from_json_array(payload, "source_size", source_size) ||
        !origin_from_json_array(payload, "destination_origin", destination_origin)) {
      return fail("copyFromBuffer toTexture is missing replay region metadata");
    }
    [encoder copyFromBuffer:source_buffer
               sourceOffset:static_cast<NSUInteger>(source_offset)
          sourceBytesPerRow:static_cast<NSUInteger>(source_bytes_per_row)
        sourceBytesPerImage:static_cast<NSUInteger>(source_bytes_per_image)
                 sourceSize:source_size
                  toTexture:destination_texture
           destinationSlice:static_cast<NSUInteger>(destination_slice)
           destinationLevel:static_cast<NSUInteger>(destination_level)
          destinationOrigin:destination_origin];
    return true;
  }

  bool object_metadata(std::uint64_t object_id, const json &metadata)
  {
    index_object_metadata(object_id, metadata);
    const auto kind = metadata.value("kind", std::string());
    if (kind == "dxmt_sampler_gpu_resource_id") {
      return create_sampler(metadata);
    }
    if (kind == "dxmt_texture_view") {
      return create_texture_view(metadata);
    }
    if (kind == "dxmt_depth_stencil_state") {
      return create_depth_stencil_state(metadata);
    }
    return true;
  }

  bool buffer_update(const trace::MetalEventRecord &event, const json &payload)
  {
    std::uint64_t buffer_id = event.object_id;
    if (payload.contains("buffer_id")) {
      buffer_id = payload.value("buffer_id", buffer_id);
    }
    id<MTLBuffer> buffer = buffer_for_id(buffer_id);
    if (buffer == nil) {
      return fail("buffer update references missing buffer");
    }
    return restore_buffer_range_from_asset(event, buffer, payload);
  }

  bool texture_update(const trace::MetalEventRecord &event, const json &payload)
  {
    std::uint64_t texture_id = event.object_id;
    if (payload.contains("texture_id")) {
      texture_id = payload.value("texture_id", texture_id);
    }
    id<MTLTexture> texture = texture_for_id(texture_id);
    if (texture == nil) {
      return fail("texture update references missing texture");
    }
    return restore_texture_region_from_asset(event, texture, payload);
  }

  bool handle_debug_signpost(const trace::MetalEventRecord &event, const json &payload)
  {
    const auto object_id = event.object_id;
    const json signpost = parse_json(payload.value("label", std::string()));
    const auto kind = signpost.value("kind", std::string());
    if (kind == "dxmt_buffer_gpu_address") {
      std::uint64_t buffer_id = 0;
      std::uint64_t gpu_address = 0;
      if (!required_u64(signpost, "buffer_id", buffer_id) ||
          !required_u64(signpost, "gpu_address", gpu_address)) {
        return fail("dxmt_buffer_gpu_address is missing payload");
      }
      if (buffer_id == 0 || gpu_address == 0) {
        return fail("dxmt_buffer_gpu_address has zero payload");
      }
      pending_buffer_gpu_addresses_[buffer_id] = gpu_address;
      return true;
    }
    if (kind == "dxmt_copy_buffer_to_texture") {
      return copy_buffer_to_texture(event, signpost);
    }
    if (kind == "dxmt_dispatch_threads") {
      return dispatch_threads(object_id, signpost);
    }
    if (kind == "dxmt_set_compute_bytes") {
      return set_compute_bytes(object_id, signpost);
    }
    if (kind == "dxmt_sampler_gpu_resource_id") {
      return create_sampler(signpost);
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
    NSString *label = [NSString stringWithUTF8String:payload.value("label", std::string()).c_str()];
    id<MTLRenderCommandEncoder> render_encoder = render_encoder_for_id(object_id);
    if (render_encoder != nil) {
      [render_encoder insertDebugSignpost:label];
      return true;
    }
    id<MTLComputeCommandEncoder> compute_encoder = compute_encoder_for_id(object_id);
    if (compute_encoder != nil) {
      [compute_encoder insertDebugSignpost:label];
      return true;
    }
    id<MTLBlitCommandEncoder> blit_encoder = blit_encoder_for_id(object_id);
    if (blit_encoder != nil) {
      [blit_encoder insertDebugSignpost:label];
      return true;
    }
    return true;
  }

  bool queue_present(std::uint64_t command_buffer_id, const json &payload)
  {
    std::uint64_t drawable_id = 0;
    std::uint64_t present_texture_id = 0;
    std::uint64_t frame_index = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sync_interval = 0;
    std::uint32_t flags = 0;
    if (!required_u64(payload, "drawable_handle", drawable_id) ||
        !required_u64(payload, "present_texture_id", present_texture_id) ||
        !required_u64(payload, "frame_index", frame_index) ||
        !required_u32(payload, "width", width) ||
        !required_u32(payload, "height", height) ||
        !required_u32(payload, "sync_interval", sync_interval) ||
        !required_u32(payload, "flags", flags) ||
        width == 0 || height == 0) {
      return fail("presentDrawable is missing replay metadata");
    }
    NSMutableDictionary *info = [[NSMutableDictionary alloc] init];
    info[@"drawable_id"] = object_key(drawable_id);
    NSNumber *render_pass_texture_id = [command_buffer_present_textures_ objectForKey:object_key(command_buffer_id)];
    if (render_pass_texture_id != nil) {
      info[@"present_texture_id"] = render_pass_texture_id;
    } else {
      info[@"present_texture_id"] = object_key(present_texture_id);
    }
    info[@"frame_index"] = object_key(frame_index);
    info[@"width"] = [NSNumber numberWithUnsignedInt:width];
    info[@"height"] = [NSNumber numberWithUnsignedInt:height];
    info[@"sync_interval"] = [NSNumber numberWithUnsignedInt:sync_interval];
    info[@"flags"] = [NSNumber numberWithUnsignedInt:flags];
    [pending_presents_ setObject:info forKey:object_key(command_buffer_id)];
    return true;
  }

  bool capture_present_frame(NSDictionary *present_info)
  {
    if (!options_.enable_metal_present_capture) {
      return true;
    }

    const auto present_texture_id =
        static_cast<std::uint64_t>([present_info[@"present_texture_id"] unsignedLongLongValue]);
    id<MTLTexture> texture = texture_for_id(present_texture_id);
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
    if (!write_present_frame_to_dir(
            "present-frame",
            static_cast<std::uint64_t>([present_info[@"frame_index"] unsignedLongLongValue]),
            0,
            width,
            height,
            row_pitch,
            texture.pixelFormat,
            bytes.data(),
            bytes.size())) {
      return false;
    }
    if (!capture_writer_) {
      return true;
    }

    trace::AssetRecord asset;
    asset.blob_id = ++capture_sequence_;
    asset.kind = trace::AssetKind::Texture;
    asset.debug_name = "metal-present-frame";
    asset.payload_bytes = bytes;
    asset = capture_writer_->register_asset(std::move(asset));

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

  bool flush_render_pass_captures(std::uint64_t command_buffer_id, NSDictionary *present_info)
  {
    const auto frame_index =
        static_cast<std::uint64_t>([present_info[@"frame_index"] unsignedLongLongValue]);
    auto captures_it = render_pass_captures_.find(command_buffer_id);
    if (captures_it == render_pass_captures_.end()) {
      return true;
    }
    if (!metal_retrace_capture_frame_in_range(frame_index)) {
      discard_render_pass_captures(command_buffer_id);
      return true;
    }
    bool ok = true;
    for (const auto &capture : captures_it->second) {
      if (capture.readback == nil || capture.readback.contents == nullptr) {
        std::fprintf(stderr,
                     "metal retrace pass capture skipped: frame=%llu pass=%llu has no readback contents\n",
                     static_cast<unsigned long long>(frame_index),
                     static_cast<unsigned long long>(capture.pass_index));
        continue;
      }
      if (!write_present_frame_to_dir(
              "pass",
              frame_index,
              capture.pass_index,
              capture.width,
              capture.height,
              capture.row_pitch,
              capture.pixel_format,
              capture.readback.contents,
              static_cast<std::size_t>(capture.row_pitch) * static_cast<std::size_t>(capture.height))) {
        ok = false;
      }
    }
    discard_render_pass_captures(command_buffer_id);
    return ok;
  }

  void discard_render_pass_captures(std::uint64_t command_buffer_id)
  {
    render_pass_captures_.erase(command_buffer_id);
    render_pass_indices_.erase(command_buffer_id);
  }

  bool write_present_frame_to_dir(
      const char *kind,
      std::uint64_t frame_index,
      std::uint64_t pass_index,
      std::uint32_t width,
      std::uint32_t height,
      std::uint32_t row_pitch,
      MTLPixelFormat pixel_format,
      const void *rgba_data,
      std::size_t rgba_size)
  {
    const char *dir = std::getenv("APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR");
    if (dir == nullptr || *dir == '\0') {
      return true;
    }
    if (!metal_retrace_capture_frame_in_range(frame_index)) {
      return true;
    }
    if (rgba_data == nullptr || width == 0 || height == 0 || row_pitch < width * 4u ||
        rgba_size < static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(height)) {
      std::fprintf(stderr,
                   "metal retrace %s capture skipped: invalid readback layout frame=%llu pass=%llu\n",
                   kind,
                   static_cast<unsigned long long>(frame_index),
                   static_cast<unsigned long long>(pass_index));
      return true;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) {
      last_error_ = "failed to create present-frame sink dir: " + ec.message();
      return false;
    }

    char stem[96];
    if (std::strcmp(kind, "pass") == 0) {
      std::snprintf(stem, sizeof(stem), "pass-%06llu-%06llu",
                    static_cast<unsigned long long>(frame_index),
                    static_cast<unsigned long long>(pass_index));
    } else {
      std::snprintf(stem, sizeof(stem), "present-frame-%06llu",
                    static_cast<unsigned long long>(frame_index));
    }

    const auto *bytes = static_cast<const std::uint8_t *>(rgba_data);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * 4u *
                                  static_cast<std::size_t>(height));
    for (std::uint32_t row = 0; row < height; ++row) {
      const auto *src_row = bytes + static_cast<std::size_t>(row) * row_pitch;
      auto *dst_row = rgba.data() + static_cast<std::size_t>(row) *
                                      static_cast<std::size_t>(width) * 4u;
      switch (pixel_format) {
      case MTLPixelFormatRGBA8Unorm:
      case MTLPixelFormatRGBA8Unorm_sRGB:
        std::memcpy(dst_row, src_row, static_cast<std::size_t>(width) * 4u);
        break;
      case MTLPixelFormatBGRA8Unorm:
      case MTLPixelFormatBGRA8Unorm_sRGB:
        for (std::uint32_t col = 0; col < width; ++col) {
          const auto *src = src_row + static_cast<std::size_t>(col) * 4u;
          auto *dst = dst_row + static_cast<std::size_t>(col) * 4u;
          dst[0] = src[2];
          dst[1] = src[1];
          dst[2] = src[0];
          dst[3] = src[3];
        }
        break;
      default:
        std::fprintf(stderr,
                     "metal retrace %s capture skipped: unsupported readback format %s (%lu)\n",
                     kind,
                     pixel_format_name(pixel_format),
                     static_cast<unsigned long>(pixel_format));
        return true;
      }
    }

    const fs::path raw_path = fs::path(dir) / (std::string(stem) + ".rgba");
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw) {
      last_error_ = "failed to open present-frame raw file: " + raw_path.string();
      return false;
    }
    raw.write(reinterpret_cast<const char *>(rgba.data()),
              static_cast<std::streamsize>(rgba.size()));
    if (!raw) {
      last_error_ = "failed to write present-frame raw file: " + raw_path.string();
      return false;
    }
    return true;
  }

  const trace::TraceBundleReader *reader_ = nullptr;
  ReplayOptions options_;
  std::string last_error_;
  std::unique_ptr<trace::TraceBundleWriter> capture_writer_;
  std::uint64_t capture_sequence_ = 0;
  std::unordered_map<trace::BlobId, std::filesystem::path> asset_paths_by_blob_;

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
  NSMutableDictionary<NSNumber *, id<MTLFence>> *fences_ = nil;
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
  std::unordered_map<std::uint64_t, RenderPassColorTarget> render_encoder_color_targets_;
  std::unordered_map<std::uint64_t, std::vector<RenderPassCapture>> render_pass_captures_;
  std::unordered_map<std::uint64_t, std::uint64_t> render_pass_indices_;
  std::unordered_map<std::uint64_t, std::uint64_t> command_buffer_present_frame_indices_;
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
