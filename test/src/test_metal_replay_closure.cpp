#include "apitrace/asset_index.hpp"
#include "trace/src/payload_object_refs.hpp"
#include "apitrace/replay_options.hpp"
#include "apitrace/replay_session.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

apitrace::trace::MetalEventRecord metal_event(
    apitrace::trace::MetalCallKind kind,
    std::uint64_t sequence,
    std::uint64_t object_id,
    const char *function_name,
    std::string payload,
    std::vector<apitrace::trace::BlobId> blob_refs = {})
{
  apitrace::trace::MetalEventRecord event;
  event.call_kind = kind;
  event.metal_sequence = sequence;
  event.object_id = object_id;
  event.function_name = function_name ? function_name : "";
  event.payload = std::move(payload);
  event.blob_refs = std::move(blob_refs);
  apitrace::trace::append_payload_text_object_refs(event.payload, event.object_refs);
  return event;
}

bool expect_contains(const std::string &text, const std::string &needle)
{
  return text.find(needle) != std::string::npos;
}

int run_bundle_check(
    const std::filesystem::path &bundle_check,
    const std::filesystem::path &bundle,
    const std::string &options)
{
  if (bundle_check.empty()) {
    return 0;
  }
  std::ostringstream command;
  command << '"' << bundle_check.string() << "\" " << options << " \"" << bundle.string() << '"';
  return std::system(command.str().c_str());
}

bool validate_metal_bundle(const std::filesystem::path &bundle, std::string &error)
{
  apitrace::replay::ReplayOptions options;
  options.bundle_root = bundle;
  options.enable_metal_retrace = true;
  options.validate_only = true;

  apitrace::replay::ReplaySession session(std::move(options));
  if (session.run()) {
    error.clear();
    return true;
  }
  error = session.last_error();
  return false;
}

bool validate_metal_bundle(
    const std::filesystem::path &bundle,
    apitrace::replay::ReplayStatistics &statistics,
    std::string &error)
{
  apitrace::replay::ReplayOptions options;
  options.bundle_root = bundle;
  options.enable_metal_retrace = true;
  options.validate_only = true;

  apitrace::replay::ReplaySession session(std::move(options));
  if (!session.run()) {
    error = session.last_error();
    return false;
  }
  statistics = session.statistics();
  error.clear();
  return true;
}

std::string texture_descriptor_payload()
{
  return "{\"descriptor\":{\"type\":2,\"width\":2,\"height\":2,\"depth\":1,"
         "\"array_length\":1,\"pixel_format\":\"bgra8unorm\",\"usage\":5,"
         "\"mipmap_level_count\":1,\"sample_count\":1}}";
}

std::string render_pass_payload(std::uint64_t color_texture_id)
{
  return "{\"command_buffer_id\":3,\"render_pass_info\":{\"color_texture_id\":" +
         std::to_string(color_texture_id) +
         ",\"color_pixel_format\":\"bgra8unorm\",\"width\":2,\"height\":2,"
         "\"load_action\":\"clear\",\"store_action\":\"store\",\"clear_color\":[0.0,0.0,0.0,1.0],"
         "\"slot\":0,\"level\":0,\"slice\":0,\"depth_plane\":0}}";
}

void append_explicit_render_state(
    apitrace::trace::TraceBundleWriter &writer,
    std::uint64_t start_sequence,
    std::uint64_t encoder_id)
{
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      start_sequence,
      encoder_id,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_viewports\",\"viewports\":[[0.0,0.0,2.0,2.0,0.0,1.0]]}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      start_sequence + 1,
      encoder_id,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_scissor_rects\",\"rects\":[[0,0,2,2]]}"));
}

void append_minimal_render_work(
    apitrace::trace::TraceBundleWriter &writer,
    std::uint64_t start_sequence,
    const std::string &extra_event_payload,
    apitrace::trace::MetalCallKind extra_event_kind,
    const char *extra_event_function)
{
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      start_sequence,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      start_sequence + 1,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      start_sequence + 2,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      start_sequence + 3,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, start_sequence + 4, 4);
  writer.append_metal_event(metal_event(
      extra_event_kind,
      start_sequence + 6,
      4,
      extra_event_function,
      extra_event_payload));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      start_sequence + 7,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      start_sequence + 8,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      start_sequence + 9,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_id\":5,\"frame_index\":0,\"width\":2,\"height\":2,\"sync_interval\":1,\"flags\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferCommit,
      start_sequence + 10,
      3,
      "MTLCommandBuffer.commit",
      "{}"));
}

apitrace::trace::AssetRecord write_metal_asset(
    apitrace::trace::TraceBundleWriter &writer,
    apitrace::trace::MetalAssetKind metal_kind,
    apitrace::trace::BlobId blob_id,
    const char *debug_name,
    const std::string &payload)
{
  apitrace::trace::AssetRecord asset;
  asset.blob_id = blob_id;
  asset.kind = apitrace::trace::AssetKind::Unknown;
  asset.debug_name = debug_name ? debug_name : "";
  asset.payload_bytes.assign(payload.begin(), payload.end());
  return writer.register_metal_asset(metal_kind, std::move(asset));
}

bool write_missing_pipeline_bind_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_missing_pipeline_bind";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      200,
      "missing-bind.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      201,
      "missing-bind.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      6,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_invalid_viewport_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_invalid_viewport";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      220,
      "invalid-viewport.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      221,
      "invalid-viewport.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  append_minimal_render_work(
      writer,
      3,
      "{\"payload\":{\"viewports\":[[0.0,0.0,2.0,2.0,0.0,1.0,42.0]]}}",
      apitrace::trace::MetalCallKind::SetViewport,
      "MTLRenderCommandEncoder.setViewport");
  writer.close();
  return true;
}

bool write_missing_color_texture_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_missing_color_texture";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      180,
      "missing-color-texture.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      181,
      "missing-color-texture.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      3,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      4,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      5,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      6,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_render_pass_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_render_pass_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      190,
      "render-pass-missing-payload.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      191,
      "render-pass-missing-payload.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      "{\"command_buffer_id\":3,\"render_pass_info\":{\"color_texture_id\":5,"
      "\"color_pixel_format\":\"bgra8unorm\",\"width\":2,\"height\":2,"
      "\"load_action\":2,\"clear_color\":[0.0,0.0,0.0,1.0]}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 7, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_render_pass_missing_subresource_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_render_pass_missing_subresource";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      194,
      "render-pass-missing-subresource.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      195,
      "render-pass-missing-subresource.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      "{\"command_buffer_id\":3,\"render_pass_info\":{\"color_texture_id\":5,"
      "\"color_pixel_format\":\"bgra8unorm\",\"width\":2,\"height\":2,"
      "\"load_action\":2,\"store_action\":1,\"clear_color\":[0.0,0.0,0.0,1.0],"
      "\"slot\":0,\"slice\":0,\"depth_plane\":0}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      7,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_render_pass_invalid_clear_color_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_render_pass_invalid_clear_color";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      190,
      "render-pass-invalid-clear-color.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      191,
      "render-pass-invalid-clear-color.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      "{\"command_buffer_id\":3,\"render_pass_info\":{\"color_texture_id\":5,"
      "\"color_pixel_format\":\"bgra8unorm\",\"width\":2,\"height\":2,"
      "\"load_action\":\"clear\",\"store_action\":\"store\","
      "\"clear_color\":[0.0,0.0,0.0,1.0,0.5],"
      "\"slot\":0,\"level\":0,\"slice\":0,\"depth_plane\":0}}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      7,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_missing_pipeline_blob_ref_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_missing_pipeline_blob_ref";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      210,
      "missing-blob-ref.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      211,
      "missing-blob-ref.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {}));
  writer.close();
  return true;
}

bool write_wrong_pipeline_blob_ref_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_wrong_pipeline_blob_ref";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      230,
      "wrong-pipeline-blob-ref.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      231,
      "wrong-pipeline-blob-ref.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {library.blob_id}));
  writer.close();
  return true;
}

bool write_incomplete_pipeline_descriptor_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_incomplete_pipeline_descriptor";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      220,
      "incomplete-descriptor.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      221,
      "incomplete-descriptor.pipeline",
      "{\"library_id\":1,\"fragment_function\":\"fs_main\",\"colors\":[]}");
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.close();
  return true;
}

bool write_pipeline_color_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_pipeline_color_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      224,
      "pipeline-color-missing.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      225,
      "pipeline-color-missing.pipeline",
      "{\"library_id\":1,\"vertex_library_id\":1,\"fragment_library_id\":1,"
      "\"vertex_function\":\"vs_main\",\"fragment_function\":\"fs_main\","
      "\"colors\":[{\"pixel_format\":\"bgra8unorm\",\"write_mask\":15}],"
      "\"rasterization_enabled\":true,\"raster_sample_count\":1}");
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.close();
  return true;
}

bool write_wrong_vertex_buffer_type_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_wrong_vertex_buffer_type";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      240,
      "wrong-buffer-type.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      241,
      "wrong-buffer-type.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetVertexBuffer,
      7,
      4,
      "MTLRenderCommandEncoder.setVertexBuffer",
      "{\"buffer_id\":2,\"offset\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_missing_vertex_buffer_offset_bind_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_missing_vertex_buffer_offset_bind";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      250,
      "missing-offset-bind.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      251,
      "missing-offset-bind.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetVertexBufferOffset,
      7,
      4,
      "MTLRenderCommandEncoder.setVertexBufferOffset",
      "{\"offset\":16,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_missing_fragment_bytes_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_missing_fragment_bytes";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      255,
      "missing-fragment-bytes.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      256,
      "missing-fragment-bytes.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetFragmentBytes,
      7,
      4,
      "MTLRenderCommandEncoder.setFragmentBytes",
      "{\"index\":0,\"payload\":\"{\\\"length\\\":4,\\\"bytes\\\":[]}\"}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_texture_bind_missing_index_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_bind_missing_index";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      265,
      "texture-bind-missing-index.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      266,
      "texture-bind-missing-index.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  append_minimal_render_work(
      writer,
      3,
      "{\"texture_id\":5}",
      apitrace::trace::MetalCallKind::SetFragmentTexture,
      "MTLRenderCommandEncoder.setFragmentTexture");
  writer.close();
  return true;
}

bool write_inline_bytes_missing_length_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_inline_bytes_missing_length";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      267,
      "inline-bytes-missing-length.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      268,
      "inline-bytes-missing-length.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  append_minimal_render_work(
      writer,
      3,
      "{\"index\":0,\"payload\":\"{\\\"bytes\\\":[1,2,3,4]}\"}",
      apitrace::trace::MetalCallKind::SetFragmentBytes,
      "MTLRenderCommandEncoder.setFragmentBytes");
  writer.close();
  return true;
}

bool write_buffer_missing_length_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_buffer_missing_length";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      269,
      "buffer-missing-length.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      270,
      "buffer-missing-length.pipeline",
      pipeline_payload);
  apitrace::trace::AssetRecord buffer_asset;
  buffer_asset.blob_id = 271;
  buffer_asset.kind = apitrace::trace::AssetKind::Buffer;
  buffer_asset.debug_name = "buffer-missing-length";
  buffer_asset.payload_bytes.assign(16, 0);
  buffer_asset = writer.register_asset(std::move(buffer_asset));
  if (library.relative_path.empty() || pipeline.relative_path.empty() || buffer_asset.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      6,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + buffer_asset.relative_path.generic_string() + "\"}",
      {buffer_asset.blob_id}));
  append_minimal_render_work(
      writer,
      4,
      "{\"index\":0,\"length\":4,\"bytes\":[1,2,3,4]}",
      apitrace::trace::MetalCallKind::SetFragmentBytes,
      "MTLRenderCommandEncoder.setFragmentBytes");
  writer.close();
  return true;
}

std::string sampler_descriptor_payload(bool include_support_argument_buffers)
{
  std::string payload =
      "{\"kind\":\"dxmt_sampler_gpu_resource_id\","
      "\"sampler_id\":8,"
      "\"gpu_resource_id\":9001,"
      "\"border_color\":0,"
      "\"r_address_mode\":1,"
      "\"s_address_mode\":1,"
      "\"t_address_mode\":1,"
      "\"mag_filter\":1,"
      "\"min_filter\":1,"
      "\"mip_filter\":1,"
      "\"compare_function\":0,"
      "\"lod_max_clamp\":1000,"
      "\"lod_min_clamp\":0,"
      "\"max_anisotropy\":1,"
      "\"lod_average\":false,"
      "\"normalized_coordinates\":true";
  if (include_support_argument_buffers) {
    payload += ",\"support_argument_buffers\":true";
  }
  payload += "}";
  return payload;
}

bool write_sampler_descriptor_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload,
    bool include_support_argument_buffers)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = include_support_argument_buffers
                          ? "test_metal_sampler_descriptor"
                          : "test_metal_incomplete_sampler_descriptor";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      270,
      "sampler-descriptor.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      271,
      "sampler-descriptor.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      4,
      8,
      "MTLObject.metadata",
      sampler_descriptor_payload(include_support_argument_buffers)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      5,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      6,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      7,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 8, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetFragmentSamplerState,
      10,
      4,
      "MTLRenderCommandEncoder.setFragmentSamplerState",
      "{\"sampler_state_id\":8,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      11,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      12,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      13,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_id\":5,\"frame_index\":0,\"width\":2,\"height\":2,\"sync_interval\":1,\"flags\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferCommit,
      14,
      3,
      "MTLCommandBuffer.commit",
      "{}"));
  writer.close();
  return true;
}

bool write_nullable_resource_bind_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_nullable_resource_binds";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      280,
      "nullable-resource-binds.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      281,
      "nullable-resource-binds.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 7, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetVertexBuffer,
      9,
      4,
      "MTLRenderCommandEncoder.setVertexBuffer",
      "{\"buffer_id\":0,\"offset\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetVertexTexture,
      10,
      4,
      "MTLRenderCommandEncoder.setVertexTexture",
      "{\"texture_id\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetVertexSamplerState,
      11,
      4,
      "MTLRenderCommandEncoder.setVertexSamplerState",
      "{\"sampler_state_id\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetFragmentTexture,
      12,
      4,
      "MTLRenderCommandEncoder.setFragmentTexture",
      "{\"texture_id\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetFragmentBuffer,
      13,
      4,
      "MTLRenderCommandEncoder.setFragmentBuffer",
      "{\"buffer_id\":0,\"offset\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetFragmentSamplerState,
      14,
      4,
      "MTLRenderCommandEncoder.setFragmentSamplerState",
      "{\"sampler_state_id\":0,\"index\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      15,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      16,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      17,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_id\":5,\"frame_index\":0,\"width\":2,\"height\":2,\"sync_interval\":1,\"flags\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferCommit,
      18,
      3,
      "MTLCommandBuffer.commit",
      "{}"));
  writer.close();
  return true;
}

bool write_argument_buffer_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_argument_buffer";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      260,
      "argument-buffer.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      261,
      "argument-buffer.pipeline",
      pipeline_payload);
  apitrace::trace::AssetRecord buffer_asset;
  buffer_asset.blob_id = 262;
  buffer_asset.kind = apitrace::trace::AssetKind::Buffer;
  buffer_asset.debug_name = "argument-buffer";
  buffer_asset.payload_bytes.assign(64, 0);
  buffer_asset = writer.register_asset(std::move(buffer_asset));
  if (library.relative_path.empty() || pipeline.relative_path.empty() || buffer_asset.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      6,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + buffer_asset.relative_path.generic_string() + "\",\"length\":64}",
      {buffer_asset.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      4,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      5,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      6,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      7,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetArgumentBuffer,
      8,
      4,
      "MTLCommandEncoder.setArgumentBuffer",
      "{\"index\":0,\"buffer_id\":6,\"offset\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_wrong_use_resource_type_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_wrong_use_resource_type";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      270,
      "wrong-use-resource.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      271,
      "wrong-use-resource.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::UseResource,
      7,
      4,
      "MTLRenderCommandEncoder.useResource",
      "{\"resource_id\":2,\"usage\":1,\"stages\":1}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_use_resource_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_use_resource_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      276,
      "use-resource-missing.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      277,
      "use-resource-missing.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  append_minimal_render_work(
      writer,
      3,
      "{\"resource_id\":5,\"usage\":1}",
      apitrace::trace::MetalCallKind::UseResource,
      "MTLRenderCommandEncoder.useResource");
  writer.close();
  return true;
}

bool write_use_heap_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_use_heap";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      280,
      "use-heap.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      281,
      "use-heap.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::UseHeap,
      7,
      4,
      "MTLRenderCommandEncoder.useHeap",
      "{\"heap_id\":5}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_unknown_encoder_state_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_unknown_encoder_state";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      290,
      "unknown-encoder-state.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      291,
      "unknown-encoder-state.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      7,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      8,
      4,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_rasterizer_state\",\"fill_mode\":0,\"cull_mode\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_rasterizer_state_missing_field_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_rasterizer_state_missing_field";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      380,
      "rasterizer-state-missing-field.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      381,
      "rasterizer-state-missing-field.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      7,
      4,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_rasterizer_state\",\"fill_mode\":0,\"cull_mode\":0,"
      "\"depth_clip_mode\":0,\"depth_bias\":0.0,\"slope_scale\":0.0,"
      "\"depth_bias_clamp\":0.0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_unknown_call_kind_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_unknown_call_kind";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      340,
      "unknown-call-kind.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      341,
      "unknown-call-kind.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::Unknown,
      7,
      4,
      "MTLCommandEncoder.unknownCall",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_unknown_depth_stencil_state_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_unknown_depth_stencil_state";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      300,
      "unknown-depth-stencil-state.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      301,
      "unknown-depth-stencil-state.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      7,
      4,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_depth_stencil_state\",\"depth_stencil_state_id\":2,\"stencil_ref\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_depth_stencil_state_missing_id_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_depth_stencil_state_missing_id";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      342,
      "depth-stencil-state-missing-id.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      343,
      "depth-stencil-state-missing-id.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      7,
      4,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_depth_stencil_state\",\"stencil_ref\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      8,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_texture_view_missing_payload_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_view_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      2,
      6,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_texture_view\",\"texture_id\":6,\"source_texture_id\":5,"
      "\"gpu_resource_id\":88,\"pixel_format\":80,\"texture_type\":2,"
      "\"level_start\":0,\"level_count\":1,\"slice_start\":0,\"slice_count\":1}"));
  writer.close();
  return true;
}

bool write_texture_view_invalid_swizzle_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_view_invalid_swizzle";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      2,
      6,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_texture_view\",\"texture_id\":6,\"source_texture_id\":5,"
      "\"gpu_resource_id\":88,\"pixel_format\":80,\"texture_type\":2,"
      "\"level_start\":0,\"level_count\":1,\"slice_start\":0,\"slice_count\":1,"
      "\"swizzle\":[0,1,2,3,4]}"));
  writer.close();
  return true;
}

bool write_texture_view_unknown_swizzle_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_view_unknown_swizzle";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      2,
      6,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_texture_view\",\"texture_id\":6,\"source_texture_id\":5,"
      "\"gpu_resource_id\":88,\"pixel_format\":80,\"texture_type\":2,"
      "\"level_start\":0,\"level_count\":1,\"slice_start\":0,\"slice_count\":1,"
      "\"swizzle\":[2,3,4,6]}"));
  writer.close();
  return true;
}

bool write_depth_stencil_missing_payload_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_depth_stencil_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      1,
      7,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_depth_stencil_state\",\"depth_stencil_state_id\":7,"
      "\"depth_compare_function\":0}"));
  writer.close();
  return true;
}

bool write_buffer_gpu_address_missing_payload_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_buffer_gpu_address_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      1,
      5,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_buffer_gpu_address\",\"buffer_id\":5}"));
  writer.close();
  return true;
}

bool write_buffer_gpu_address_signpost_missing_payload_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_buffer_gpu_address_signpost_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      1,
      5,
      "MTLCommandEncoder.insertDebugSignpost",
      "{\"label\":\"{\\\"kind\\\":\\\"dxmt_buffer_gpu_address\\\",\\\"gpu_address\\\":4096}\"}"));
  writer.close();
  return true;
}

bool write_texture_descriptor_missing_payload_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_descriptor_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      5,
      "MTLDevice.newTexture",
      "{\"descriptor\":{\"type\":2,\"width\":2,\"height\":2,\"depth\":1,"
      "\"array_length\":1,\"pixel_format\":\"bgra8unorm\",\"usage\":5,"
      "\"mipmap_level_count\":1}}"));
  writer.close();
  return true;
}

bool write_texture_descriptor_invalid_pixel_format_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_descriptor_invalid_pixel_format";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      5,
      "MTLDevice.newTexture",
      "{\"descriptor\":{\"type\":2,\"width\":2,\"height\":2,\"depth\":1,"
      "\"array_length\":1,\"pixel_format\":\"not-a-format\",\"usage\":5,"
      "\"mipmap_level_count\":1,\"sample_count\":1}}"));
  writer.close();
  return true;
}

bool write_texture_gpu_resource_missing_payload_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_texture_gpu_resource_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      1,
      5,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_texture_gpu_resource_id\",\"texture_id\":5}"));
  writer.close();
  return true;
}

bool write_memory_barrier_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_memory_barrier_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      360,
      "memory-barrier-missing-payload.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      361,
      "memory-barrier-missing-payload.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::MemoryBarrier,
      7,
      4,
      "MTLRenderCommandEncoder.memoryBarrier",
      "{\"scope\":1,\"stages_before\":1}"));
  writer.close();
  return true;
}

bool write_fence_ops_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_fence_ops_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      370,
      "fence-ops-missing-payload.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      371,
      "fence-ops-missing-payload.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::FenceOps,
      7,
      4,
      "MTLRenderCommandEncoder.fenceOps",
      "{\"ops\":[]}"));
  writer.close();
  return true;
}

bool write_dispatch_signpost_without_pipeline_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_dispatch_signpost_without_pipeline";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      310,
      "dispatch-signpost.metallib",
      library_payload);
  if (library.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      2,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ComputeEncoderBegin,
      3,
      4,
      "MTLCommandBuffer.computeCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      4,
      4,
      "MTLCommandEncoder.insertDebugSignpost",
      "{\"label\":\"{\\\"kind\\\":\\\"dxmt_dispatch_threads\\\",\\\"width\\\":1,"
      "\\\"height\\\":1,\\\"depth\\\":1,\\\"threads_per_group_width\\\":1,"
      "\\\"threads_per_group_height\\\":1,\\\"threads_per_group_depth\\\":1}\"}"));
  writer.close();
  return true;
}

bool write_dispatch_signpost_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_dispatch_signpost_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      320,
      "dispatch-signpost-missing-payload.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::ComputePipeline,
      321,
      "dispatch-signpost-missing-payload.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newComputePipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      3,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ComputeEncoderBegin,
      4,
      4,
      "MTLCommandBuffer.computeCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetComputePipelineState,
      5,
      4,
      "MTLComputeCommandEncoder.setComputePipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      6,
      4,
      "MTLCommandEncoder.insertDebugSignpost",
      "{\"label\":\"{\\\"kind\\\":\\\"dxmt_dispatch_threads\\\",\\\"width\\\":1}\"}"));
  writer.close();
  return true;
}

bool write_compute_bytes_without_encoder_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_compute_bytes_without_encoder";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      330,
      "compute-bytes.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::ComputePipeline,
      331,
      "compute-bytes.pipeline",
      "{\"library_id\":1,\"function\":\"cs_main\"}");
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newComputePipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      3,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ComputeEncoderBegin,
      4,
      4,
      "MTLCommandBuffer.computeCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetComputePipelineState,
      5,
      4,
      "MTLComputeCommandEncoder.setComputePipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ComputeEncoderEnd,
      6,
      4,
      "MTLComputeCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetComputeBytes,
      7,
      4,
      "MTLComputeCommandEncoder.setBytes",
      "{\"index\":0,\"length\":4,\"bytes\":[1,2,3,4]}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DispatchThreads,
      8,
      4,
      "MTLComputeCommandEncoder.dispatchThreads",
      "{\"width\":1,\"height\":1,\"depth\":1,\"threads_per_group_width\":1,"
      "\"threads_per_group_height\":1,\"threads_per_group_depth\":1}"));
  writer.close();
  return true;
}

bool write_compute_bytes_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_compute_bytes_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      340,
      "compute-bytes-missing-payload.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::ComputePipeline,
      341,
      "compute-bytes-missing-payload.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newComputePipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      3,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ComputeEncoderBegin,
      4,
      4,
      "MTLCommandBuffer.computeCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetComputePipelineState,
      5,
      4,
      "MTLComputeCommandEncoder.setComputePipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetComputeBytes,
      6,
      4,
      "MTLComputeCommandEncoder.setBytes",
      "{\"index\":0,\"length\":4,\"bytes\":[]}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DispatchThreads,
      7,
      4,
      "MTLComputeCommandEncoder.dispatchThreads",
      "{\"width\":1,\"height\":1,\"depth\":1,\"threads_per_group_width\":1,"
      "\"threads_per_group_height\":1,\"threads_per_group_depth\":1}"));
  writer.close();
  return true;
}

bool write_compute_bytes_signpost_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_compute_bytes_signpost_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      350,
      "compute-bytes-signpost-missing-payload.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::ComputePipeline,
      351,
      "compute-bytes-signpost-missing-payload.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newComputePipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      3,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ComputeEncoderBegin,
      4,
      4,
      "MTLCommandBuffer.computeCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetComputePipelineState,
      5,
      4,
      "MTLComputeCommandEncoder.setComputePipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      6,
      4,
      "MTLCommandEncoder.insertDebugSignpost",
      "{\"label\":\"{\\\"kind\\\":\\\"dxmt_set_compute_bytes\\\",\\\"index\\\":0,"
      "\\\"length\\\":4,\\\"bytes\\\":[]}\"}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DispatchThreads,
      7,
      4,
      "MTLComputeCommandEncoder.dispatchThreads",
      "{\"width\":1,\"height\":1,\"depth\":1,\"threads_per_group_width\":1,"
      "\"threads_per_group_height\":1,\"threads_per_group_depth\":1}"));
  writer.close();
  return true;
}

bool write_copy_signpost_without_blit_encoder_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_copy_signpost_without_blit_encoder";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      320,
      "copy-signpost.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      321,
      "copy-signpost.pipeline",
      pipeline_payload);
  apitrace::trace::AssetRecord buffer_asset;
  buffer_asset.blob_id = 322;
  buffer_asset.kind = apitrace::trace::AssetKind::Buffer;
  buffer_asset.debug_name = "copy-signpost-buffer";
  buffer_asset.payload_bytes.assign(16, 0);
  buffer_asset = writer.register_asset(std::move(buffer_asset));
  if (library.relative_path.empty() || pipeline.relative_path.empty() || buffer_asset.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      6,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + buffer_asset.relative_path.generic_string() + "\",\"length\":16}",
      {buffer_asset.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      4,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      5,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      6,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      7,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      8,
      4,
      "MTLCommandEncoder.insertDebugSignpost",
      "{\"label\":\"{\\\"kind\\\":\\\"dxmt_copy_buffer_to_texture\\\","
      "\\\"source_buffer\\\":6,\\\"destination_texture\\\":5,"
      "\\\"source_offset\\\":0,\\\"source_bytes_per_row\\\":8,"
      "\\\"source_bytes_per_image\\\":16}\"}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_copy_buffer_missing_size_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_copy_buffer_missing_size";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      350,
      "copy-buffer-missing-size.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      351,
      "copy-buffer-missing-size.pipeline",
      pipeline_payload);
  apitrace::trace::AssetRecord source_asset;
  source_asset.blob_id = 352;
  source_asset.kind = apitrace::trace::AssetKind::Buffer;
  source_asset.debug_name = "copy-buffer-source";
  source_asset.payload_bytes.assign(16, 1);
  source_asset = writer.register_asset(std::move(source_asset));
  apitrace::trace::AssetRecord destination_asset;
  destination_asset.blob_id = 353;
  destination_asset.kind = apitrace::trace::AssetKind::Buffer;
  destination_asset.debug_name = "copy-buffer-destination";
  destination_asset.payload_bytes.assign(16, 0);
  destination_asset = writer.register_asset(std::move(destination_asset));
  if (library.relative_path.empty() || pipeline.relative_path.empty() ||
      source_asset.relative_path.empty() || destination_asset.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      6,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + source_asset.relative_path.generic_string() + "\",\"length\":16}",
      {source_asset.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      4,
      7,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + destination_asset.relative_path.generic_string() + "\",\"length\":16}",
      {destination_asset.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      5,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderBegin,
      6,
      4,
      "MTLCommandBuffer.blitCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CopyBuffer,
      7,
      4,
      "MTLBlitCommandEncoder.copyFromBuffer",
      "{\"source_buffer_id\":6,\"source_offset\":0,"
      "\"destination_buffer_id\":7,\"destination_offset\":0}"));
  writer.close();
  return true;
}

bool write_copy_texture_missing_payload_bundle(
    const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_copy_texture_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      10,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      11,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      3,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderBegin,
      4,
      4,
      "MTLCommandBuffer.blitCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CopyTexture,
      5,
      4,
      "MTLBlitCommandEncoder.copyFromTexture",
      "{\"source_texture_id\":10,\"destination_texture_id\":11,"
      "\"payload\":{\"source_size\":[2,2,1],\"destination_slice\":0,"
      "\"destination_level\":0,\"destination_origin\":[0,0,0]}}"));
  writer.close();
  return true;
}

bool write_blit_fill_missing_value_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_blit_fill_missing_value";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      356,
      "blit-fill-missing-value.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      357,
      "blit-fill-missing-value.pipeline",
      pipeline_payload);
  apitrace::trace::AssetRecord buffer_asset;
  buffer_asset.blob_id = 358;
  buffer_asset.kind = apitrace::trace::AssetKind::Buffer;
  buffer_asset.debug_name = "blit-fill-buffer";
  buffer_asset.payload_bytes.assign(16, 0);
  buffer_asset = writer.register_asset(std::move(buffer_asset));
  if (library.relative_path.empty() || pipeline.relative_path.empty() || buffer_asset.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      6,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + buffer_asset.relative_path.generic_string() + "\",\"length\":16}",
      {buffer_asset.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.blitCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitFill,
      6,
      4,
      "MTLBlitCommandEncoder.fillBuffer",
      "{\"buffer_id\":6,\"range_start\":0,\"range_length\":16}"));
  writer.close();
  return true;
}

bool write_blit_batch_missing_ops_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_blit_batch_missing_ops";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      370,
      "blit-batch-missing-ops.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      371,
      "blit-batch-missing-ops.pipeline",
      pipeline_payload);
  apitrace::trace::AssetRecord buffer_asset;
  buffer_asset.blob_id = 372;
  buffer_asset.kind = apitrace::trace::AssetKind::Buffer;
  buffer_asset.debug_name = "blit-batch-buffer";
  buffer_asset.payload_bytes.assign(16, 0);
  buffer_asset = writer.register_asset(std::move(buffer_asset));
  if (library.relative_path.empty() || pipeline.relative_path.empty() || buffer_asset.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      6,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + buffer_asset.relative_path.generic_string() + "\",\"length\":16}",
      {buffer_asset.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      4,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      5,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderBegin,
      6,
      4,
      "MTLCommandBuffer.blitCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitBatch,
      7,
      4,
      "MTLBlitCommandEncoder.blitBatch",
      "{\"op_count\":1}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderEnd,
      8,
      4,
      "MTLBlitCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      9,
      7,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      10,
      7,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      11,
      7,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_blit_batch_fence_missing_stages_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_blit_batch_fence_missing_stages";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      374,
      "blit-batch-fence-missing-stages.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      375,
      "blit-batch-fence-missing-stages.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.blitCommandEncoder",
      "{\"command_buffer_id\":3}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::BlitBatch,
      6,
      4,
      "MTLBlitCommandEncoder.blitBatch",
      "{\"ops\":[{\"op\":\"wait_fence\",\"fence_id\":9}]}"));
  writer.close();
  return true;
}

bool write_draw_missing_vertex_count_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_draw_missing_vertex_count";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      360,
      "draw-missing-vertex-count.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      361,
      "draw-missing-vertex-count.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      7,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_draw_invalid_primitive_type_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_draw_invalid_primitive_type";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      370,
      "draw-invalid-primitive-type.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      371,
      "draw-invalid-primitive-type.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      7,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":99,\"vertex_start\":0,\"vertex_count\":3,"
      "\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_present_missing_payload_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_present_missing_payload";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      364,
      "present-missing.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      365,
      "present-missing.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 7, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      10,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      9,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_id\":5,\"frame_index\":0,\"height\":2,\"sync_interval\":1,\"flags\":0}"));
  writer.close();
  return true;
}

bool write_open_encoder_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_open_encoder";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      390,
      "open-encoder.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      391,
      "open-encoder.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 7, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.close();
  return true;
}

bool write_uncommitted_command_buffer_bundle(
    const std::filesystem::path &bundle,
    const std::string &library_payload,
    const std::string &pipeline_payload)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_uncommitted_command_buffer";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({});

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      400,
      "uncommitted-command-buffer.metallib",
      library_payload);
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      401,
      "uncommitted-command-buffer.pipeline",
      pipeline_payload);
  if (library.relative_path.empty() || pipeline.relative_path.empty()) {
    return false;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      3,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      4,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      5,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      6,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 7, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      9,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      10,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.close();
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: test_metal_replay_closure <bundle> [bundle-check]\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  const std::filesystem::path bundle_check = argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path();
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    std::cerr << "failed to open bundle\n";
    return 1;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::Unknown;
  metadata.producer = "test_metal_replay_closure";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({
      {5, apitrace::trace::ObjectKind::Resource, 0, "drawable-texture"},
  });

  const auto library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      100,
      "smoke.metallib",
      "fake-metallib-bytes");
  if (library.relative_path.empty()) {
    std::cerr << "failed to write Metal library asset\n";
    return 1;
  }
  const auto duplicate_library = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::Library,
      103,
      "smoke-duplicate.metallib",
      "fake-metallib-bytes");
  if (duplicate_library.blob_id == library.blob_id ||
      duplicate_library.relative_path != library.relative_path) {
    std::cerr << "duplicate Metal library did not share storage while preserving blob identity\n";
    return 1;
  }

  const std::string pipeline_descriptor =
      "{\"library_id\":1,\"vertex_library_id\":1,\"fragment_library_id\":1,"
      "\"vertex_function\":\"vs_main\",\"fragment_function\":\"fs_main\","
      "\"colors\":[{\"pixel_format\":\"bgra8unorm\",\"blending_enabled\":false,"
      "\"write_mask\":15,\"rgb_blend_operation\":0,\"alpha_blend_operation\":0,"
      "\"src_rgb_blend_factor\":1,\"dst_rgb_blend_factor\":0,"
      "\"src_alpha_blend_factor\":1,\"dst_alpha_blend_factor\":0}],"
      "\"rasterization_enabled\":true,"
      "\"raster_sample_count\":1}";
  const std::string compute_pipeline_descriptor =
      "{\"library_id\":1,\"function\":\"cs_main\"}";
  const auto pipeline = write_metal_asset(
      writer,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      101,
      "smoke.pipeline",
      pipeline_descriptor);
  if (pipeline.relative_path.empty()) {
    std::cerr << "failed to write Metal pipeline asset\n";
    return 1;
  }

  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + library.relative_path.generic_string() + "\",\"size\":18}",
      {library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      10,
      6,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + duplicate_library.relative_path.generic_string() + "\",\"size\":18}",
      {duplicate_library.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      11,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DeviceCreate,
      12,
      5,
      "MTLDevice.newTexture",
      texture_descriptor_payload()));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::ObjectMetadata,
      13,
      7,
      "MTLObject.metadata",
      "{\"kind\":\"dxmt_depth_stencil_state\",\"depth_stencil_state_id\":7,"
      "\"depth_compare_function\":0,\"depth_write_enabled\":false}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferBegin,
      14,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderBegin,
      15,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(5)));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      16,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  append_explicit_render_state(writer, 17, 4);
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::EncoderState,
      19,
      4,
      "MTLCommandEncoder.encoderState",
      "{\"kind\":\"dxmt_set_depth_stencil_state\",\"depth_stencil_state_id\":7,\"stencil_ref\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::DrawPrimitives,
      20,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::RenderEncoderEnd,
      21,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::PresentDrawable,
      22,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_id\":5,\"frame_index\":0,\"width\":2,\"height\":2,\"sync_interval\":1,\"flags\":0}"));
  writer.append_metal_event(metal_event(
      apitrace::trace::MetalCallKind::CommandBufferCommit,
      23,
      3,
      "MTLCommandBuffer.commit",
      "{}"));

  apitrace::trace::AssetRecord present_frame;
  present_frame.blob_id = 102;
  present_frame.kind = apitrace::trace::AssetKind::Texture;
  present_frame.debug_name = "poisoned-diagnostic-metal-present-frame";
  present_frame.payload_bytes.assign(2 * 2 * 4, 0x00);
  present_frame = writer.register_asset(std::move(present_frame));
  apitrace::trace::EventRecord present_frame_event;
  present_frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
  present_frame_event.callsite.sequence = 1;
  present_frame_event.callsite.function_name = "resource_blob";
  present_frame_event.object_kind = apitrace::trace::ObjectKind::Unknown;
  present_frame_event.object_debug_name = "MetalPresentFrame";
  present_frame_event.blob_refs = {present_frame.blob_id};
  present_frame_event.payload =
      "{\"frame_index\":0,\"width\":2,\"height\":2,\"row_pitch\":8,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\",\"frame_path\":\"" +
      present_frame.relative_path.generic_string() + "\"}";
  writer.append_call_event(present_frame_event);
  writer.close();

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    std::cerr << "failed to read bundle: " << reader.last_error() << "\n";
    return 1;
  }
  if (reader.metal_events().size() != 15) {
    std::cerr << "unexpected Metal event count\n";
    return 1;
  }
  if (reader.events().size() != 1 ||
      reader.events().front().object_debug_name != "MetalPresentFrame" ||
      reader.events().front().blob_refs.size() != 1 ||
      reader.events().front().blob_refs.front() != present_frame.blob_id) {
    std::cerr << "poisoned MetalPresentFrame diagnostic event was not preserved\n";
    return 1;
  }
  if (reader.events().front().payload.find(present_frame.relative_path.generic_string()) == std::string::npos) {
    std::cerr << "poisoned MetalPresentFrame diagnostic path was not preserved\n";
    return 1;
  }

  bool found_library_blob = false;
  bool found_duplicate_library_blob = false;
  bool found_pipeline_blob = false;
  for (const auto &asset : reader.metal_assets()) {
    found_library_blob = found_library_blob || asset.blob_id == library.blob_id;
    found_duplicate_library_blob = found_duplicate_library_blob || asset.blob_id == duplicate_library.blob_id;
    found_pipeline_blob = found_pipeline_blob || asset.blob_id == pipeline.blob_id;
  }
  if (!found_library_blob || !found_duplicate_library_blob || !found_pipeline_blob) {
    std::cerr << "Metal asset blob refs were not indexed by the reader\n";
    return 1;
  }

  std::string replay_error;
  apitrace::replay::ReplayStatistics replay_statistics;
  if (!validate_metal_bundle(bundle, replay_statistics, replay_error)) {
    std::cerr << "valid Metal bundle failed validate-only replay: " << replay_error << "\n";
    return 1;
  }
  if (replay_statistics.backend_name != "metal-validate-only" ||
      replay_statistics.metal_calls_replayed != reader.metal_events().size() ||
      replay_statistics.calls_replayed != 0 ||
      replay_statistics.presents_seen != 1 ||
      replay_statistics.metal_presents_seen != 1) {
    std::cerr << "poisoned MetalPresentFrame should remain diagnostic metadata, not replay input\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, bundle, "--require-metal --require-metal-replay-closure --require-metal-present-frames") != 0) {
    std::cerr << "bundle-check rejected the valid Metal replay closure fixture\n";
    return 1;
  }

  const auto sampler_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-sampler-descriptor");
  if (!write_sampler_descriptor_bundle(sampler_descriptor_bundle, "fake-metallib-bytes", pipeline_descriptor, true)) {
    std::cerr << "failed to write sampler-descriptor bundle\n";
    return 1;
  }
  if (!validate_metal_bundle(sampler_descriptor_bundle, replay_error)) {
    std::cerr << "sampler-descriptor bundle failed validate-only replay: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, sampler_descriptor_bundle, "--require-metal --require-metal-replay-closure") != 0) {
    std::cerr << "bundle-check rejected sampler-descriptor bundle\n";
    return 1;
  }

  const auto nullable_resource_bind_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-nullable-resource-binds");
  if (!write_nullable_resource_bind_bundle(nullable_resource_bind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write nullable-resource-binds bundle\n";
    return 1;
  }
  if (!validate_metal_bundle(nullable_resource_bind_bundle, replay_error)) {
    std::cerr << "nullable-resource-binds bundle failed validate-only replay: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, nullable_resource_bind_bundle, "--require-metal --require-metal-replay-closure") != 0) {
    std::cerr << "bundle-check rejected nullable-resource-binds bundle\n";
    return 1;
  }

  const auto incomplete_sampler_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-incomplete-sampler-descriptor");
  if (!write_sampler_descriptor_bundle(
          incomplete_sampler_descriptor_bundle, "fake-metallib-bytes", pipeline_descriptor, false)) {
    std::cerr << "failed to write incomplete-sampler-descriptor bundle\n";
    return 1;
  }
  if (validate_metal_bundle(incomplete_sampler_descriptor_bundle, replay_error) ||
      !expect_contains(replay_error, "sampler descriptor is missing support_argument_buffers")) {
    std::cerr << "incomplete-sampler-descriptor bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, incomplete_sampler_descriptor_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted incomplete-sampler-descriptor bundle\n";
    return 1;
  }

  const auto missing_bind_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-pipeline-bind");
  if (!write_missing_pipeline_bind_bundle(missing_bind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-pipeline-bind bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader missing_bind_reader;
  if (!missing_bind_reader.open(missing_bind_bundle)) {
    std::cerr << "failed to read missing-pipeline-bind bundle: " << missing_bind_reader.last_error() << "\n";
    return 1;
  }
  if (validate_metal_bundle(missing_bind_bundle, replay_error) ||
      !expect_contains(replay_error, "draw occurs before a valid render pipeline bind")) {
    std::cerr << "missing-pipeline-bind bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_bind_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-pipeline-bind bundle\n";
    return 1;
  }

  const auto invalid_viewport_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-invalid-viewport");
  if (!write_invalid_viewport_bundle(invalid_viewport_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write invalid-viewport bundle\n";
    return 1;
  }
  if (validate_metal_bundle(invalid_viewport_bundle, replay_error) ||
      !expect_contains(replay_error, "setViewport has invalid viewports")) {
    std::cerr << "invalid-viewport bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, invalid_viewport_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted invalid-viewport bundle\n";
    return 1;
  }

  const auto missing_color_texture_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-color-texture");
  if (!write_missing_color_texture_bundle(missing_color_texture_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-color-texture bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_color_texture_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass references an unknown color texture")) {
    std::cerr << "missing-color-texture bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_color_texture_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-color-texture bundle\n";
    return 1;
  }

  const auto render_pass_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-missing-payload");
  if (!write_render_pass_missing_payload_bundle(
          render_pass_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write render-pass-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(render_pass_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass color attachment is missing store_action")) {
    std::cerr << "render-pass-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          render_pass_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted render-pass-missing-payload bundle\n";
    return 1;
  }

  const auto render_pass_missing_subresource_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-missing-subresource");
  if (!write_render_pass_missing_subresource_bundle(
          render_pass_missing_subresource_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write render-pass-missing-subresource bundle\n";
    return 1;
  }
  if (validate_metal_bundle(render_pass_missing_subresource_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass color attachment is missing level")) {
    std::cerr << "render-pass-missing-subresource bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          render_pass_missing_subresource_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted render-pass-missing-subresource bundle\n";
    return 1;
  }

  const auto render_pass_invalid_clear_color_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-invalid-clear-color");
  if (!write_render_pass_invalid_clear_color_bundle(
          render_pass_invalid_clear_color_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write render-pass-invalid-clear-color bundle\n";
    return 1;
  }
  if (validate_metal_bundle(render_pass_invalid_clear_color_bundle, replay_error) ||
      !expect_contains(replay_error, "render pass color attachment has invalid clear_color")) {
    std::cerr << "render-pass-invalid-clear-color bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          render_pass_invalid_clear_color_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted render-pass-invalid-clear-color bundle\n";
    return 1;
  }

  const auto missing_blob_ref_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-blob-ref");
  if (!write_missing_pipeline_blob_ref_bundle(missing_blob_ref_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-blob-ref bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_blob_ref_bundle, replay_error) ||
      !expect_contains(replay_error, "asset path references are missing blob_refs")) {
    std::cerr << "missing-blob-ref bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_blob_ref_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-blob-ref bundle\n";
    return 1;
  }

  const auto wrong_blob_ref_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-wrong-blob-ref");
  if (!write_wrong_pipeline_blob_ref_bundle(wrong_blob_ref_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write wrong-blob-ref bundle\n";
    return 1;
  }
  if (validate_metal_bundle(wrong_blob_ref_bundle, replay_error) ||
      !expect_contains(replay_error, "asset path blob_ref does not match")) {
    std::cerr << "wrong-blob-ref bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, wrong_blob_ref_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted wrong-blob-ref bundle\n";
    return 1;
  }

  const auto incomplete_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-incomplete-descriptor");
  if (!write_incomplete_pipeline_descriptor_bundle(incomplete_descriptor_bundle, "fake-metallib-bytes")) {
    std::cerr << "failed to write incomplete-descriptor bundle\n";
    return 1;
  }
  if (validate_metal_bundle(incomplete_descriptor_bundle, replay_error) ||
      !expect_contains(replay_error, "render pipeline descriptor missing vertex_function")) {
    std::cerr << "incomplete-descriptor bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, incomplete_descriptor_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted incomplete-descriptor bundle\n";
    return 1;
  }

  const auto pipeline_color_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-pipeline-color-missing-payload");
  if (!write_pipeline_color_missing_payload_bundle(pipeline_color_missing_payload_bundle, "fake-metallib-bytes")) {
    std::cerr << "failed to write pipeline-color-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(pipeline_color_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "render pipeline color attachment 0 is missing blending_enabled")) {
    std::cerr << "pipeline-color-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          pipeline_color_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted pipeline-color-missing-payload bundle\n";
    return 1;
  }

  const auto wrong_buffer_type_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-wrong-vertex-buffer-type");
  if (!write_wrong_vertex_buffer_type_bundle(wrong_buffer_type_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write wrong-vertex-buffer-type bundle\n";
    return 1;
  }
  if (validate_metal_bundle(wrong_buffer_type_bundle, replay_error) ||
      !expect_contains(replay_error, "buffer bind references an unknown buffer")) {
    std::cerr << "wrong-vertex-buffer-type bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, wrong_buffer_type_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted wrong-vertex-buffer-type bundle\n";
    return 1;
  }

  const auto missing_offset_bind_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-vertex-buffer-offset-bind");
  if (!write_missing_vertex_buffer_offset_bind_bundle(missing_offset_bind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-vertex-buffer-offset-bind bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_offset_bind_bundle, replay_error) ||
      !expect_contains(replay_error, "buffer offset update occurs before a matching vertex buffer bind")) {
    std::cerr << "missing-vertex-buffer-offset-bind bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_offset_bind_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-vertex-buffer-offset-bind bundle\n";
    return 1;
  }

  const auto missing_fragment_bytes_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-fragment-bytes");
  if (!write_missing_fragment_bytes_bundle(missing_fragment_bytes_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write missing-fragment-bytes bundle\n";
    return 1;
  }
  if (validate_metal_bundle(missing_fragment_bytes_bundle, replay_error) ||
      !expect_contains(replay_error, "setFragmentBytes is missing captured bytes")) {
    std::cerr << "missing-fragment-bytes bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, missing_fragment_bytes_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted missing-fragment-bytes bundle\n";
    return 1;
  }

  const auto texture_bind_missing_index_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-bind-missing-index");
  if (!write_texture_bind_missing_index_bundle(
          texture_bind_missing_index_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write texture-bind-missing-index bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_bind_missing_index_bundle, replay_error) ||
      !expect_contains(replay_error, "texture bind is missing index")) {
    std::cerr << "texture-bind-missing-index bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_bind_missing_index_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-bind-missing-index bundle\n";
    return 1;
  }

  const auto inline_bytes_missing_length_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-inline-bytes-missing-length");
  if (!write_inline_bytes_missing_length_bundle(
          inline_bytes_missing_length_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write inline-bytes-missing-length bundle\n";
    return 1;
  }
  if (validate_metal_bundle(inline_bytes_missing_length_bundle, replay_error) ||
      !expect_contains(replay_error, "setFragmentBytes is missing length")) {
    std::cerr << "inline-bytes-missing-length bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          inline_bytes_missing_length_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted inline-bytes-missing-length bundle\n";
    return 1;
  }

  const auto buffer_missing_length_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-missing-length");
  if (!write_buffer_missing_length_bundle(
          buffer_missing_length_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write buffer-missing-length bundle\n";
    return 1;
  }
  if (validate_metal_bundle(buffer_missing_length_bundle, replay_error) ||
      !expect_contains(replay_error, "MTLDevice.newBuffer is missing length")) {
    std::cerr << "buffer-missing-length bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          buffer_missing_length_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted buffer-missing-length bundle\n";
    return 1;
  }

  const auto open_encoder_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-open-encoder");
  if (!write_open_encoder_bundle(open_encoder_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write open-encoder bundle\n";
    return 1;
  }
  if (validate_metal_bundle(open_encoder_bundle, replay_error) ||
      !expect_contains(replay_error, "ended with an open render encoder")) {
    std::cerr << "open-encoder bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, open_encoder_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted open-encoder bundle\n";
    return 1;
  }

  const auto uncommitted_command_buffer_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-uncommitted-command-buffer");
  if (!write_uncommitted_command_buffer_bundle(uncommitted_command_buffer_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write uncommitted-command-buffer bundle\n";
    return 1;
  }
  if (validate_metal_bundle(uncommitted_command_buffer_bundle, replay_error) ||
      !expect_contains(replay_error, "ended with an uncommitted command buffer")) {
    std::cerr << "uncommitted-command-buffer bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, uncommitted_command_buffer_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted uncommitted-command-buffer bundle\n";
    return 1;
  }

  const auto argument_buffer_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-argument-buffer");
  if (!write_argument_buffer_bundle(argument_buffer_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write argument-buffer bundle\n";
    return 1;
  }
  if (validate_metal_bundle(argument_buffer_bundle, replay_error) ||
      !expect_contains(replay_error, "SetArgumentBuffer requires native Metal argument-buffer replay support")) {
    std::cerr << "argument-buffer bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, argument_buffer_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted argument-buffer bundle\n";
    return 1;
  }

  const auto wrong_use_resource_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-wrong-use-resource-type");
  if (!write_wrong_use_resource_type_bundle(wrong_use_resource_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write wrong-use-resource-type bundle\n";
    return 1;
  }
  if (validate_metal_bundle(wrong_use_resource_bundle, replay_error) ||
      !expect_contains(replay_error, "useResource references an unknown replayable resource")) {
    std::cerr << "wrong-use-resource-type bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, wrong_use_resource_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted wrong-use-resource-type bundle\n";
    return 1;
  }

  const auto use_resource_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-use-resource-missing-payload");
  if (!write_use_resource_missing_payload_bundle(
          use_resource_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write use-resource-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(use_resource_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "useResource is missing stages")) {
    std::cerr << "use-resource-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          use_resource_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted use-resource-missing-payload bundle\n";
    return 1;
  }

  const auto use_heap_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-use-heap");
  if (!write_use_heap_bundle(use_heap_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write use-heap bundle\n";
    return 1;
  }
  if (validate_metal_bundle(use_heap_bundle, replay_error) ||
      !expect_contains(replay_error, "UseHeap requires native Metal heap replay support")) {
    std::cerr << "use-heap bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, use_heap_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted use-heap bundle\n";
    return 1;
  }

  const auto unknown_encoder_state_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unknown-encoder-state");
  if (!write_unknown_encoder_state_bundle(unknown_encoder_state_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write unknown-encoder-state bundle\n";
    return 1;
  }
  if (validate_metal_bundle(unknown_encoder_state_bundle, replay_error) ||
      !expect_contains(replay_error, "encoder state command references an unknown render encoder")) {
    std::cerr << "unknown-encoder-state bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, unknown_encoder_state_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted unknown-encoder-state bundle\n";
    return 1;
  }

  const auto rasterizer_state_missing_field_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-rasterizer-state-missing-field");
  if (!write_rasterizer_state_missing_field_bundle(
          rasterizer_state_missing_field_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write rasterizer-state-missing-field bundle\n";
    return 1;
  }
  if (validate_metal_bundle(rasterizer_state_missing_field_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_set_rasterizer_state is missing winding")) {
    std::cerr << "rasterizer-state-missing-field bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, rasterizer_state_missing_field_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted rasterizer-state-missing-field bundle\n";
    return 1;
  }

  const auto unknown_call_kind_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unknown-call-kind");
  if (!write_unknown_call_kind_bundle(unknown_call_kind_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write unknown-call-kind bundle\n";
    return 1;
  }
  if (validate_metal_bundle(unknown_call_kind_bundle, replay_error) ||
      !expect_contains(replay_error, "unknown Metal call kind is not replayable")) {
    std::cerr << "unknown-call-kind bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, unknown_call_kind_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted unknown-call-kind bundle\n";
    return 1;
  }

  const auto unknown_depth_stencil_state_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unknown-depth-stencil-state");
  if (!write_unknown_depth_stencil_state_bundle(
          unknown_depth_stencil_state_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write unknown-depth-stencil-state bundle\n";
    return 1;
  }
  if (validate_metal_bundle(unknown_depth_stencil_state_bundle, replay_error) ||
      !expect_contains(replay_error, "encoder state references an unknown depth stencil state")) {
    std::cerr << "unknown-depth-stencil-state bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, unknown_depth_stencil_state_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted unknown-depth-stencil-state bundle\n";
    return 1;
  }

  const auto depth_stencil_state_missing_id_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-depth-stencil-state-missing-id");
  if (!write_depth_stencil_state_missing_id_bundle(
          depth_stencil_state_missing_id_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write depth-stencil-state-missing-id bundle\n";
    return 1;
  }
  if (validate_metal_bundle(depth_stencil_state_missing_id_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_set_depth_stencil_state is missing depth_stencil_state_id")) {
    std::cerr << "depth-stencil-state-missing-id bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          depth_stencil_state_missing_id_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted depth-stencil-state-missing-id bundle\n";
    return 1;
  }

  const auto texture_view_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-view-missing-payload");
  if (!write_texture_view_missing_payload_bundle(texture_view_missing_payload_bundle)) {
    std::cerr << "failed to write texture-view-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_view_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "texture view descriptor is missing swizzle")) {
    std::cerr << "texture-view-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_view_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-view-missing-payload bundle\n";
    return 1;
  }

  const auto texture_view_invalid_swizzle_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-view-invalid-swizzle");
  if (!write_texture_view_invalid_swizzle_bundle(texture_view_invalid_swizzle_bundle)) {
    std::cerr << "failed to write texture-view-invalid-swizzle bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_view_invalid_swizzle_bundle, replay_error) ||
      !expect_contains(replay_error, "texture view descriptor has invalid swizzle")) {
    std::cerr << "texture-view-invalid-swizzle bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_view_invalid_swizzle_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-view-invalid-swizzle bundle\n";
    return 1;
  }

  const auto texture_view_unknown_swizzle_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-view-unknown-swizzle");
  if (!write_texture_view_unknown_swizzle_bundle(texture_view_unknown_swizzle_bundle)) {
    std::cerr << "failed to write texture-view-unknown-swizzle bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_view_unknown_swizzle_bundle, replay_error) ||
      !expect_contains(replay_error, "texture view descriptor has invalid swizzle")) {
    std::cerr << "texture-view-unknown-swizzle bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_view_unknown_swizzle_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-view-unknown-swizzle bundle\n";
    return 1;
  }

  const auto depth_stencil_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-depth-stencil-missing-payload");
  if (!write_depth_stencil_missing_payload_bundle(depth_stencil_missing_payload_bundle)) {
    std::cerr << "failed to write depth-stencil-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(depth_stencil_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "depth stencil descriptor is missing depth_write_enabled")) {
    std::cerr << "depth-stencil-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          depth_stencil_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted depth-stencil-missing-payload bundle\n";
    return 1;
  }

  const auto buffer_gpu_address_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-gpu-address-missing-payload");
  if (!write_buffer_gpu_address_missing_payload_bundle(buffer_gpu_address_missing_payload_bundle)) {
    std::cerr << "failed to write buffer-gpu-address-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(buffer_gpu_address_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_buffer_gpu_address is missing gpu_address")) {
    std::cerr << "buffer-gpu-address-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          buffer_gpu_address_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted buffer-gpu-address-missing-payload bundle\n";
    return 1;
  }

  const auto buffer_gpu_address_signpost_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-gpu-address-signpost-missing-payload");
  if (!write_buffer_gpu_address_signpost_missing_payload_bundle(buffer_gpu_address_signpost_missing_payload_bundle)) {
    std::cerr << "failed to write buffer-gpu-address-signpost-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(buffer_gpu_address_signpost_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_buffer_gpu_address is missing buffer_id")) {
    std::cerr << "buffer-gpu-address-signpost-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          buffer_gpu_address_signpost_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted buffer-gpu-address-signpost-missing-payload bundle\n";
    return 1;
  }

  const auto texture_descriptor_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-descriptor-missing-payload");
  if (!write_texture_descriptor_missing_payload_bundle(texture_descriptor_missing_payload_bundle)) {
    std::cerr << "failed to write texture-descriptor-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_descriptor_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "texture descriptor is missing sample_count")) {
    std::cerr << "texture-descriptor-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_descriptor_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-descriptor-missing-payload bundle\n";
    return 1;
  }

  const auto texture_descriptor_invalid_pixel_format_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-descriptor-invalid-pixel-format");
  if (!write_texture_descriptor_invalid_pixel_format_bundle(texture_descriptor_invalid_pixel_format_bundle)) {
    std::cerr << "failed to write texture-descriptor-invalid-pixel-format bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_descriptor_invalid_pixel_format_bundle, replay_error) ||
      !expect_contains(replay_error, "texture descriptor has invalid pixel_format")) {
    std::cerr << "texture-descriptor-invalid-pixel-format bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_descriptor_invalid_pixel_format_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-descriptor-invalid-pixel-format bundle\n";
    return 1;
  }

  const auto texture_gpu_resource_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-gpu-resource-missing-payload");
  if (!write_texture_gpu_resource_missing_payload_bundle(texture_gpu_resource_missing_payload_bundle)) {
    std::cerr << "failed to write texture-gpu-resource-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(texture_gpu_resource_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dxmt_texture_gpu_resource_id is missing gpu_resource_id")) {
    std::cerr << "texture-gpu-resource-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          texture_gpu_resource_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted texture-gpu-resource-missing-payload bundle\n";
    return 1;
  }

  const auto memory_barrier_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-memory-barrier-missing-payload");
  if (!write_memory_barrier_missing_payload_bundle(
          memory_barrier_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write memory-barrier-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(memory_barrier_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "memoryBarrier is missing stages_after")) {
    std::cerr << "memory-barrier-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          memory_barrier_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted memory-barrier-missing-payload bundle\n";
    return 1;
  }

  const auto fence_ops_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-fence-ops-missing-payload");
  if (!write_fence_ops_missing_payload_bundle(
          fence_ops_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write fence-ops-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(fence_ops_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "fenceOps is missing ops")) {
    std::cerr << "fence-ops-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          fence_ops_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted fence-ops-missing-payload bundle\n";
    return 1;
  }

  const auto dispatch_signpost_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-dispatch-signpost-without-pipeline");
  if (!write_dispatch_signpost_without_pipeline_bundle(dispatch_signpost_bundle, "fake-metallib-bytes")) {
    std::cerr << "failed to write dispatch-signpost-without-pipeline bundle\n";
    return 1;
  }
  if (validate_metal_bundle(dispatch_signpost_bundle, replay_error) ||
      !expect_contains(replay_error, "dispatch signpost occurs before a valid compute pipeline bind")) {
    std::cerr << "dispatch-signpost-without-pipeline bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, dispatch_signpost_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted dispatch-signpost-without-pipeline bundle\n";
    return 1;
  }

  const auto dispatch_signpost_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-dispatch-signpost-missing-payload");
  if (!write_dispatch_signpost_missing_payload_bundle(
          dispatch_signpost_missing_payload_bundle, "fake-metallib-bytes", compute_pipeline_descriptor)) {
    std::cerr << "failed to write dispatch-signpost-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(dispatch_signpost_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "dispatchThreads is missing height")) {
    std::cerr << "dispatch-signpost-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          dispatch_signpost_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted dispatch-signpost-missing-payload bundle\n";
    return 1;
  }

  const auto compute_bytes_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-compute-bytes-without-encoder");
  if (!write_compute_bytes_without_encoder_bundle(compute_bytes_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write compute-bytes-without-encoder bundle\n";
    return 1;
  }
  if (validate_metal_bundle(compute_bytes_bundle, replay_error) ||
      !expect_contains(replay_error, "setComputeBytes references an unknown compute encoder")) {
    std::cerr << "compute-bytes-without-encoder bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, compute_bytes_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted compute-bytes-without-encoder bundle\n";
    return 1;
  }

  const auto compute_bytes_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-compute-bytes-missing-payload");
  if (!write_compute_bytes_missing_payload_bundle(
          compute_bytes_missing_payload_bundle, "fake-metallib-bytes", compute_pipeline_descriptor)) {
    std::cerr << "failed to write compute-bytes-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(compute_bytes_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "setComputeBytes is missing captured bytes")) {
    std::cerr << "compute-bytes-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, compute_bytes_missing_payload_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted compute-bytes-missing-payload bundle\n";
    return 1;
  }

  const auto compute_bytes_signpost_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-compute-bytes-signpost-missing-payload");
  if (!write_compute_bytes_signpost_missing_payload_bundle(
          compute_bytes_signpost_missing_payload_bundle, "fake-metallib-bytes", compute_pipeline_descriptor)) {
    std::cerr << "failed to write compute-bytes-signpost-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(compute_bytes_signpost_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "compute bytes signpost is missing captured bytes")) {
    std::cerr << "compute-bytes-signpost-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          compute_bytes_signpost_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted compute-bytes-signpost-missing-payload bundle\n";
    return 1;
  }

  const auto copy_signpost_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-signpost-without-blit-encoder");
  if (!write_copy_signpost_without_blit_encoder_bundle(
          copy_signpost_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write copy-signpost-without-blit-encoder bundle\n";
    return 1;
  }
  if (validate_metal_bundle(copy_signpost_bundle, replay_error) ||
      !expect_contains(replay_error, "copy buffer to texture signpost references an unknown blit encoder")) {
    std::cerr << "copy-signpost-without-blit-encoder bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, copy_signpost_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted copy-signpost-without-blit-encoder bundle\n";
    return 1;
  }

  const auto copy_buffer_missing_size_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-buffer-missing-size");
  if (!write_copy_buffer_missing_size_bundle(
          copy_buffer_missing_size_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write copy-buffer-missing-size bundle\n";
    return 1;
  }
  if (validate_metal_bundle(copy_buffer_missing_size_bundle, replay_error) ||
      !expect_contains(replay_error, "copyFromBuffer is missing size")) {
    std::cerr << "copy-buffer-missing-size bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, copy_buffer_missing_size_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted copy-buffer-missing-size bundle\n";
    return 1;
  }

  const auto copy_texture_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-texture-missing-payload");
  if (!write_copy_texture_missing_payload_bundle(copy_texture_missing_payload_bundle)) {
    std::cerr << "failed to write copy-texture-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(copy_texture_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "copyFromTexture is missing source_slice")) {
    std::cerr << "copy-texture-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          copy_texture_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted copy-texture-missing-payload bundle\n";
    return 1;
  }

  const auto blit_fill_missing_value_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-blit-fill-missing-value");
  if (!write_blit_fill_missing_value_bundle(
          blit_fill_missing_value_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write blit-fill-missing-value bundle\n";
    return 1;
  }
  if (validate_metal_bundle(blit_fill_missing_value_bundle, replay_error) ||
      !expect_contains(replay_error, "fillBuffer is missing value")) {
    std::cerr << "blit-fill-missing-value bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          blit_fill_missing_value_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted blit-fill-missing-value bundle\n";
    return 1;
  }

  const auto blit_batch_missing_ops_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-blit-batch-missing-ops");
  if (!write_blit_batch_missing_ops_bundle(
          blit_batch_missing_ops_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write blit-batch-missing-ops bundle\n";
    return 1;
  }
  if (validate_metal_bundle(blit_batch_missing_ops_bundle, replay_error) ||
      !expect_contains(replay_error, "blitBatch is missing ops")) {
    std::cerr << "blit-batch-missing-ops bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, blit_batch_missing_ops_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted blit-batch-missing-ops bundle\n";
    return 1;
  }

  const auto blit_batch_fence_missing_stages_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-blit-batch-fence-missing-stages");
  if (!write_blit_batch_fence_missing_stages_bundle(
          blit_batch_fence_missing_stages_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write blit-batch-fence-missing-stages bundle\n";
    return 1;
  }
  if (validate_metal_bundle(blit_batch_fence_missing_stages_bundle, replay_error) ||
      !expect_contains(replay_error, "wait_fence blitBatch op is missing stages")) {
    std::cerr << "blit-batch-fence-missing-stages bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          blit_batch_fence_missing_stages_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted blit-batch-fence-missing-stages bundle\n";
    return 1;
  }

  const auto draw_missing_vertex_count_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-missing-vertex-count");
  if (!write_draw_missing_vertex_count_bundle(
          draw_missing_vertex_count_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write draw-missing-vertex-count bundle\n";
    return 1;
  }
  if (validate_metal_bundle(draw_missing_vertex_count_bundle, replay_error) ||
      !expect_contains(replay_error, "drawPrimitives is missing vertex_count")) {
    std::cerr << "draw-missing-vertex-count bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, draw_missing_vertex_count_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted draw-missing-vertex-count bundle\n";
    return 1;
  }

  const auto draw_invalid_primitive_type_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-invalid-primitive-type");
  if (!write_draw_invalid_primitive_type_bundle(
          draw_invalid_primitive_type_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write draw-invalid-primitive-type bundle\n";
    return 1;
  }
  if (validate_metal_bundle(draw_invalid_primitive_type_bundle, replay_error) ||
      !expect_contains(replay_error, "drawPrimitives has invalid primitive_type")) {
    std::cerr << "draw-invalid-primitive-type bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(bundle_check, draw_invalid_primitive_type_bundle, "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted draw-invalid-primitive-type bundle\n";
    return 1;
  }

  const auto present_missing_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-present-missing-payload");
  if (!write_present_missing_payload_bundle(
          present_missing_payload_bundle, "fake-metallib-bytes", pipeline_descriptor)) {
    std::cerr << "failed to write present-missing-payload bundle\n";
    return 1;
  }
  if (validate_metal_bundle(present_missing_payload_bundle, replay_error) ||
      !expect_contains(replay_error, "presentDrawable is missing width")) {
    std::cerr << "present-missing-payload bundle did not fail validate-only as expected: "
              << replay_error << "\n";
    return 1;
  }
  if (!bundle_check.empty() &&
      run_bundle_check(
          bundle_check,
          present_missing_payload_bundle,
          "--require-metal --require-metal-replay-closure") == 0) {
    std::cerr << "bundle-check accepted present-missing-payload bundle\n";
    return 1;
  }

  return 0;
}
