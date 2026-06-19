#include "apitrace/asset_index.hpp"
#include "trace/src/payload_object_refs.hpp"
#include "apitrace/replay_options.hpp"
#include "apitrace/replay_session.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
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

#include "metal_replay_closure/main.inc"
