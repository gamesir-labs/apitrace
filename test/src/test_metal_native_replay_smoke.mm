#include "apitrace/replay_session.hpp"
#include "apitrace/trace_bundle_io.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Vertex {
  float position[2];
  float padding[2];
  float color[4];
};

constexpr std::uint32_t kSmokeWidth = 4;
constexpr std::uint32_t kSmokeHeight = 4;
constexpr std::uint32_t kSmokeRowPitch = kSmokeWidth * 4;

std::array<Vertex, 3> smoke_vertices()
{
  return {{
      {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
      {{-1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
      {{1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
  }};
}

std::string shell_quote(const std::filesystem::path &path)
{
  std::string quoted = "'";
  for (const char ch : path.string()) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool write_text(const std::filesystem::path &path, std::string_view text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return output.good();
}

std::vector<std::uint8_t> compile_metallib(const std::filesystem::path &work_dir)
{
  static constexpr std::string_view kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct ColorVertex {
  float2 position;
  float2 padding;
  float4 color;
};

struct VSOut {
  float4 position [[position]];
  float4 color;
};

vertex VSOut vs_main(uint vid [[vertex_id]], const device ColorVertex *vertices [[buffer(0)]]) {
  VSOut out;
  ColorVertex v = vertices[vid];
  out.position = float4(v.position, 0.0, 1.0);
  out.color = v.color;
  return out;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
  return in.color;
}
)";

  const auto metal_path = work_dir / "shader.metal";
  const auto air_path = work_dir / "shader.air";
  const auto library_path = work_dir / "shader.metallib";
  if (!write_text(metal_path, kShaderSource)) {
    return {};
  }

  const auto command =
      "xcrun -sdk macosx metal -o " + shell_quote(air_path) + " -c " + shell_quote(metal_path) +
      " && xcrun -sdk macosx metallib -o " + shell_quote(library_path) + " " + shell_quote(air_path);
  if (std::system(command.c_str()) != 0) {
    return {};
  }
  return read_bytes(library_path);
}

bool render_reference_frame(const std::vector<std::uint8_t> &metallib, std::vector<std::uint8_t> &frame, std::string &error)
{
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      error = "MTLCreateSystemDefaultDevice returned nil";
      return false;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
      error = "failed to create Metal command queue";
      return false;
    }

    void *copied_bytes = std::malloc(metallib.size());
    if (copied_bytes == nullptr) {
      error = "failed to allocate metallib staging bytes";
      return false;
    }
    std::memcpy(copied_bytes, metallib.data(), metallib.size());
    dispatch_data_t library_data = dispatch_data_create(
        copied_bytes,
        metallib.size(),
        dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
        ^{
          std::free(copied_bytes);
        });
    NSError *library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    if (library == nil) {
      error = library_error ? [[library_error localizedDescription] UTF8String] : "failed to create MTLLibrary";
      return false;
    }

    auto *pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.vertexFunction = [library newFunctionWithName:@"vs_main"];
    pipeline_descriptor.fragmentFunction = [library newFunctionWithName:@"fs_main"];
    pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    NSError *pipeline_error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&pipeline_error];
    if (pipeline == nil) {
      error = pipeline_error ? [[pipeline_error localizedDescription] UTF8String] : "failed to create render pipeline";
      return false;
    }

    const auto vertices = smoke_vertices();
    id<MTLBuffer> vertex_buffer = [device newBufferWithBytes:vertices.data()
                                                      length:sizeof(Vertex) * vertices.size()
                                                     options:MTLResourceStorageModeShared];
    if (vertex_buffer == nil) {
      error = "failed to create vertex buffer";
      return false;
    }

    auto *texture_descriptor = [[MTLTextureDescriptor alloc] init];
    texture_descriptor.textureType = MTLTextureType2D;
    texture_descriptor.width = kSmokeWidth;
    texture_descriptor.height = kSmokeHeight;
    texture_descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:texture_descriptor];
    if (target == nil) {
      error = "failed to create render target";
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    if (command_buffer == nil) {
      error = "failed to create command buffer";
      return false;
    }

    auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
      error = "failed to create render encoder";
      return false;
    }
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1 baseInstance:0];
    [encoder endEncoding];

    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if (command_buffer.status == MTLCommandBufferStatusError) {
      error = command_buffer.error ? [[command_buffer.error localizedDescription] UTF8String] : "reference command buffer failed";
      return false;
    }

    frame.assign(static_cast<std::size_t>(kSmokeRowPitch) * kSmokeHeight, 0);
    [target getBytes:frame.data()
          bytesPerRow:kSmokeRowPitch
           fromRegion:MTLRegionMake2D(0, 0, kSmokeWidth, kSmokeHeight)
          mipmapLevel:0];
    return true;
  }
}

apitrace::trace::MetalEventRecord metal_event(
    apitrace::trace::MetalCallKind kind,
    std::uint64_t sequence,
    std::uint64_t object_id,
    const char *function_name,
    std::string payload,
    std::vector<apitrace::trace::ObjectId> object_refs = {},
    std::vector<apitrace::trace::BlobId> blob_refs = {})
{
  apitrace::trace::MetalEventRecord event;
  event.call_kind = kind;
  event.metal_sequence = sequence;
  event.object_id = object_id;
  event.function_name = function_name ? function_name : "";
  event.payload = std::move(payload);
  event.object_refs = std::move(object_refs);
  event.blob_refs = std::move(blob_refs);
  return event;
}

apitrace::trace::AssetRecord asset(
    apitrace::trace::AssetKind kind,
    apitrace::trace::BlobId blob_id,
    const char *debug_name,
    const void *data,
    std::size_t size)
{
  apitrace::trace::AssetRecord record;
  record.kind = kind;
  record.blob_id = blob_id;
  record.debug_name = debug_name ? debug_name : "";
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  record.payload_bytes.assign(bytes, bytes + size);
  return record;
}

std::string frame_path_from_callstream(const std::filesystem::path &bundle)
{
  const auto callstream = read_text(bundle / "callstream.jsonl");
  const std::string marker = "\"frame_path\":\"";
  const auto marker_pos = callstream.find(marker);
  if (marker_pos == std::string::npos) {
    return {};
  }
  const auto path_begin = marker_pos + marker.size();
  const auto path_end = callstream.find('"', path_begin);
  if (path_end == std::string::npos) {
    return {};
  }
  return callstream.substr(path_begin, path_end - path_begin);
}

enum class VertexBufferBlobRefs {
  Correct,
  Missing,
  Wrong,
};

enum class CopyRegionPayload {
  Complete,
  MissingSourceSize,
};

bool write_trace_bundle(
    const std::filesystem::path &bundle,
    const std::vector<std::uint8_t> &metallib,
    const std::vector<std::uint8_t> *present_frame = nullptr,
    VertexBufferBlobRefs vertex_buffer_blob_refs = VertexBufferBlobRefs::Correct,
    CopyRegionPayload copy_region_payload = CopyRegionPayload::Complete)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_native_replay_smoke";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({
      {1, apitrace::trace::ObjectKind::Device, 0, "library"},
      {2, apitrace::trace::ObjectKind::PipelineState, 0, "pipeline"},
      {3, apitrace::trace::ObjectKind::Resource, 0, "vertex-buffer"},
      {4, apitrace::trace::ObjectKind::Resource, 0, "render-target"},
      {5, apitrace::trace::ObjectKind::CommandList, 0, "command-buffer"},
      {6, apitrace::trace::ObjectKind::Context, 5, "render-encoder"},
      {7, apitrace::trace::ObjectKind::Context, 5, "blit-encoder"},
      {8, apitrace::trace::ObjectKind::Resource, 0, "upload-buffer"},
      {9, apitrace::trace::ObjectKind::Resource, 0, "copy-target"},
  });

  auto library = asset(
      apitrace::trace::AssetKind::Unknown,
      100,
      "native-smoke.metallib",
      metallib.data(),
      metallib.size());
  library = writer.register_metal_asset(apitrace::trace::MetalAssetKind::Library, std::move(library));

  const auto pipeline_json =
      std::string("{\"library_id\":1,\"vertex_library_id\":1,\"fragment_library_id\":1,") +
      "\"vertex_function\":\"vs_main\",\"fragment_function\":\"fs_main\"," +
      "\"colors\":[{\"pixel_format\":\"bgra8unorm\",\"blending_enabled\":false,"
      "\"write_mask\":15,\"rgb_blend_operation\":0,\"alpha_blend_operation\":0,"
      "\"src_rgb_blend_factor\":1,\"dst_rgb_blend_factor\":0,"
      "\"src_alpha_blend_factor\":1,\"dst_alpha_blend_factor\":0}],"
      "\"rasterization_enabled\":true,\"raster_sample_count\":1}";
  auto pipeline = asset(
      apitrace::trace::AssetKind::Unknown,
      101,
      "native-smoke.pipeline",
      pipeline_json.data(),
      pipeline_json.size());
  pipeline = writer.register_metal_asset(apitrace::trace::MetalAssetKind::RenderPipeline, std::move(pipeline));

  const auto vertices = smoke_vertices();
  auto vertex_buffer = asset(
      apitrace::trace::AssetKind::Buffer,
      102,
      "native-smoke-vertices",
      vertices.data(),
      sizeof(vertices));
  vertex_buffer = writer.register_asset(std::move(vertex_buffer));

  std::vector<std::uint8_t> upload_bytes(static_cast<std::size_t>(kSmokeRowPitch) * kSmokeHeight, 0);
  auto upload_buffer = asset(
      apitrace::trace::AssetKind::Buffer,
      104,
      "native-smoke-upload",
      upload_bytes.data(),
      upload_bytes.size());
  upload_buffer = writer.register_asset(std::move(upload_buffer));

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":" +
          std::to_string(metallib.size()) + "}",
      {},
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {1},
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      3,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + vertex_buffer.relative_path.generic_string() + "\",\"length\":" +
          std::to_string(sizeof(vertices)) + "}",
      {},
      vertex_buffer_blob_refs == VertexBufferBlobRefs::Missing
          ? std::vector<apitrace::trace::BlobId>{}
          : vertex_buffer_blob_refs == VertexBufferBlobRefs::Wrong
                ? std::vector<apitrace::trace::BlobId>{library.blob_id}
                : std::vector<apitrace::trace::BlobId>{vertex_buffer.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      4,
      4,
      "MTLDevice.newTexture",
      std::string("{\"descriptor\":{\"width\":") + std::to_string(kSmokeWidth) +
          ",\"height\":" + std::to_string(kSmokeHeight) +
          ",\"pixel_format\":\"bgra8unorm\","
          "\"type\":2,\"depth\":1,\"array_length\":1,"
          "\"usage\":5,\"mipmap_level_count\":1,\"sample_count\":1}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      5,
      9,
      "MTLDevice.newTexture",
      std::string("{\"descriptor\":{\"width\":") + std::to_string(kSmokeWidth) +
          ",\"height\":" + std::to_string(kSmokeHeight) +
          ",\"pixel_format\":\"bgra8unorm\","
          "\"type\":2,\"depth\":1,\"array_length\":1,"
          "\"usage\":1,\"mipmap_level_count\":1,\"sample_count\":1}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      6,
      8,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + upload_buffer.relative_path.generic_string() + "\",\"length\":" +
          std::to_string(upload_bytes.size()) + "}",
      {},
      {upload_buffer.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      7,
      5,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      8,
      6,
      "MTLCommandBuffer.renderCommandEncoder",
      std::string("{\"command_buffer_id\":5,\"render_pass_info\":{\"color_texture_id\":4,") +
          "\"render_target_width\":" + std::to_string(kSmokeWidth) +
          ",\"render_target_height\":" + std::to_string(kSmokeHeight) +
          ",\"color_pixel_format\":\"bgra8unorm\","
          "\"load_action\":\"clear\",\"store_action\":\"store\",\"clear_color\":[0,0,0,1],"
          "\"slot\":0,\"level\":0,\"slice\":0,\"depth_plane\":0}}",
      {5, 4}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      9,
      6,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}",
      {2}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetViewport,
      10,
      6,
      "MTLRenderCommandEncoder.setViewport",
      std::string("{\"payload\":{\"viewports\":[[0.0,0.0,") + std::to_string(kSmokeWidth) + ".0," +
          std::to_string(kSmokeHeight) + ".0,0.0,1.0]]}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetScissorRect,
      11,
      6,
      "MTLRenderCommandEncoder.setScissorRect",
      std::string("{\"payload\":{\"rects\":[[0,0,") + std::to_string(kSmokeWidth) + "," +
          std::to_string(kSmokeHeight) + "]]}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetVertexBuffer,
      12,
      6,
      "MTLRenderCommandEncoder.setVertexBuffer",
      "{\"buffer_id\":3,\"offset\":0,\"index\":0}",
      {3}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      13,
      6,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      14,
      6,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderBegin,
      15,
      7,
      "MTLCommandBuffer.blitCommandEncoder",
      "{\"command_buffer_id\":5}",
      {5}));
  std::string copy_payload =
      std::string("{\"source_buffer\":8,\"source_offset\":0,") +
      "\"source_bytes_per_row\":" + std::to_string(kSmokeRowPitch) +
      ",\"source_bytes_per_image\":" + std::to_string(upload_bytes.size()) +
      ",\"destination_texture\":9,\"destination_slice\":0,\"destination_level\":0,"
      "\"destination_origin\":[0,0,0],\"source_asset_size\":" +
      std::to_string(upload_bytes.size()) +
      ",\"source_buffer_path\":\"" + upload_buffer.relative_path.generic_string() + "\"";
  if (copy_region_payload == CopyRegionPayload::Complete) {
    copy_payload += std::string(",\"source_size\":[") + std::to_string(kSmokeWidth) +
                    "," + std::to_string(kSmokeHeight) + ",1]";
  }
  copy_payload += "}";
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CopyBufferToTexture,
      14,
      7,
      "MTLBlitCommandEncoder.copyFromBuffer",
      copy_payload,
      {7, 8, 9},
      {upload_buffer.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderEnd,
      15,
      7,
      "MTLBlitCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      16,
      5,
      "MTLCommandBuffer.presentDrawable",
      std::string("{\"drawable_id\":4,\"frame_index\":0,\"width\":") + std::to_string(kSmokeWidth) +
          ",\"height\":" + std::to_string(kSmokeHeight) +
          ",\"sync_interval\":1,\"flags\":0}",
      {4}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferCommit,
      17,
      5,
      "MTLCommandBuffer.commit",
      "{}"));
  if (present_frame != nullptr && !present_frame->empty()) {
    auto frame = asset(
        apitrace::trace::AssetKind::Texture,
        103,
        "poisoned-metal-present-frame",
        present_frame->data(),
        present_frame->size());
    frame = writer.register_asset(std::move(frame));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.callsite.function_name = "resource_blob";
    frame_event.object_debug_name = "MetalPresentFrame";
    frame_event.blob_refs = {frame.blob_id};
    frame_event.payload = std::string("{\"frame_index\":0,\"width\":") + std::to_string(kSmokeWidth) +
                          ",\"height\":" + std::to_string(kSmokeHeight) +
                          ",\"row_pitch\":" + std::to_string(kSmokeRowPitch) +
                          ",\"sync_interval\":1,\"flags\":0,\"format\":\"bgra8\","
                          "\"frame_path\":\"" + frame.relative_path.generic_string() + "\"}";
    writer.append_call_event(frame_event);
  }
  writer.close();
  return true;
}

bool capture_trace_frame(const std::filesystem::path &source_bundle, const std::filesystem::path &capture_bundle)
{
  std::filesystem::remove_all(capture_bundle);
  const char *old_capture_env = std::getenv("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES");
  const char *old_bundle_env = std::getenv("APITRACE_TRACE_BUNDLE");
  const std::string old_capture = old_capture_env ? old_capture_env : "";
  const std::string old_bundle = old_bundle_env ? old_bundle_env : "";
  setenv("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES", "1", 1);
  setenv("APITRACE_TRACE_BUNDLE", capture_bundle.c_str(), 1);

  apitrace::replay::ReplayOptions options;
  options.bundle_root = source_bundle;
  options.enable_metal_retrace = true;
  options.backend = apitrace::replay::BackendKind::MetalTranslation;
  apitrace::replay::ReplaySession session(options);
  const bool ok = session.run();

  if (old_capture_env) {
    setenv("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES", old_capture.c_str(), 1);
  } else {
    unsetenv("APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES");
  }
  if (old_bundle_env) {
    setenv("APITRACE_TRACE_BUNDLE", old_bundle.c_str(), 1);
  } else {
    unsetenv("APITRACE_TRACE_BUNDLE");
  }

  if (!ok) {
    std::cerr << "metal native replay failed: " << session.last_error() << "\n";
    return false;
  }
  const auto &stats = session.statistics();
  if (stats.backend_name != "metal-native" || stats.metal_calls_replayed != 19 || stats.metal_presents_seen != 1) {
    std::cerr << "unexpected metal native replay stats\n";
    return false;
  }
  return true;
}

bool replay_trace_expect_failure(
    const std::filesystem::path &source_bundle,
    std::string_view expected_error)
{
  apitrace::replay::ReplayOptions options;
  options.bundle_root = source_bundle;
  options.enable_metal_retrace = true;
  options.backend = apitrace::replay::BackendKind::MetalTranslation;
  apitrace::replay::ReplaySession session(options);
  if (session.run()) {
    std::cerr << "metal native replay unexpectedly succeeded\n";
    return false;
  }
  if (session.last_error().find(expected_error) == std::string::npos) {
    std::cerr << "metal native replay failed with unexpected error: "
              << session.last_error() << "\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    std::cerr << "usage: test_metal_native_replay_smoke <work-dir>\n";
    return 2;
  }

  @autoreleasepool {
    if (MTLCreateSystemDefaultDevice() == nil) {
      std::cout << "SKIP: MTLCreateSystemDefaultDevice returned nil\n";
      return 77;
    }
  }

  const std::filesystem::path work_dir = argv[1];
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const auto metallib = compile_metallib(work_dir / "shader-build");
  if (metallib.empty()) {
    std::cerr << "failed to compile native smoke metallib\n";
    return 1;
  }

  const auto trace_bundle = work_dir / "trace.apitrace";
  const auto poisoned_trace_bundle = work_dir / "poisoned-trace.apitrace";
  const auto retrace_capture_bundle = work_dir / "retrace-capture.apitrace";
  const auto poisoned_retrace_capture_bundle = work_dir / "poisoned-retrace-capture.apitrace";
  const auto missing_buffer_blob_ref_bundle = work_dir / "missing-buffer-blob-ref.apitrace";
  const auto wrong_buffer_blob_ref_bundle = work_dir / "wrong-buffer-blob-ref.apitrace";
  const auto missing_copy_region_bundle = work_dir / "missing-copy-region.apitrace";
  std::vector<std::uint8_t> baseline;
  std::string baseline_error;
  if (!render_reference_frame(metallib, baseline, baseline_error)) {
    std::cerr << "failed to render native Metal baseline: " << baseline_error << "\n";
    return 1;
  }
  if (!write_trace_bundle(trace_bundle, metallib)) {
    std::cerr << "failed to write trace bundle\n";
    return 1;
  }
  if (!write_trace_bundle(missing_buffer_blob_ref_bundle, metallib, nullptr, VertexBufferBlobRefs::Missing)) {
    std::cerr << "failed to write missing buffer blob_ref trace bundle\n";
    return 1;
  }
  if (!replay_trace_expect_failure(missing_buffer_blob_ref_bundle, "asset path references are missing blob_refs")) {
    return 1;
  }
  if (!write_trace_bundle(wrong_buffer_blob_ref_bundle, metallib, nullptr, VertexBufferBlobRefs::Wrong)) {
    std::cerr << "failed to write wrong buffer blob_ref trace bundle\n";
    return 1;
  }
  if (!replay_trace_expect_failure(wrong_buffer_blob_ref_bundle, "asset path blob_ref does not match")) {
    return 1;
  }
  if (!write_trace_bundle(
          missing_copy_region_bundle,
          metallib,
          nullptr,
          VertexBufferBlobRefs::Correct,
          CopyRegionPayload::MissingSourceSize)) {
    std::cerr << "failed to write missing copy region trace bundle\n";
    return 1;
  }
  if (!replay_trace_expect_failure(
          missing_copy_region_bundle,
          "copyFromBuffer toTexture is missing replay region metadata")) {
    return 1;
  }
  if (!capture_trace_frame(trace_bundle, retrace_capture_bundle)) {
    return 1;
  }

  const auto candidate_frame = frame_path_from_callstream(retrace_capture_bundle);
  if (candidate_frame.empty()) {
    std::cerr << "missing captured MetalPresentFrame\n";
    return 1;
  }

  const auto candidate = read_bytes(retrace_capture_bundle / candidate_frame);
  if (baseline.empty() || baseline != candidate) {
    std::cerr << "native Metal replay frame mismatch\n";
    return 1;
  }

  std::vector<std::uint8_t> poisoned_baseline(baseline.size(), 0);
  if (!write_trace_bundle(poisoned_trace_bundle, metallib, &poisoned_baseline)) {
    std::cerr << "failed to write poisoned trace bundle\n";
    return 1;
  }
  if (!capture_trace_frame(poisoned_trace_bundle, poisoned_retrace_capture_bundle)) {
    return 1;
  }
  const auto poisoned_candidate_frame = frame_path_from_callstream(poisoned_retrace_capture_bundle);
  if (poisoned_candidate_frame.empty()) {
    std::cerr << "missing captured poisoned MetalPresentFrame\n";
    return 1;
  }
  const auto poisoned_candidate = read_bytes(poisoned_retrace_capture_bundle / poisoned_candidate_frame);
  if (poisoned_candidate == poisoned_baseline) {
    std::cerr << "poisoned MetalPresentFrame matched retrace output; replay may be reusing recorded frame pixels\n";
    return 1;
  }
  if (poisoned_candidate != baseline) {
    std::cerr << "poisoned Metal replay frame mismatch\n";
    return 1;
  }

  std::cout << "metal_native_replay_smoke PASS\n";
  return 0;
}
