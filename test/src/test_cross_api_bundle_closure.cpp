#include "apitrace/asset_index.hpp"
#include "apitrace/d3d12_replay.hpp"
#include "trace/src/payload_object_refs.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/translation_link_writer.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using apitrace::trace::AssetKind;
using apitrace::trace::BlobId;
using apitrace::trace::BoundaryKind;
using apitrace::trace::EventKind;
using apitrace::trace::EventRecord;
using apitrace::trace::MetalAssetKind;
using apitrace::trace::MetalCallKind;
using apitrace::trace::ObjectId;
using apitrace::trace::ObjectKind;
using apitrace::trace::TraceBundleWriter;

EventRecord object_create(
    std::uint64_t sequence,
    ObjectId object_id,
    ObjectKind kind,
    ObjectId parent,
    const char *debug_name)
{
  EventRecord event;
  event.kind = EventKind::ObjectCreate;
  event.callsite.sequence = sequence;
  event.callsite.function_name = "D3DObjectCreate";
  event.object_id = object_id;
  event.object_kind = kind;
  event.parent_object_id = parent;
  event.object_debug_name = debug_name ? debug_name : "";
  event.payload = "{}";
  return event;
}

EventRecord call(
    std::uint64_t sequence,
    const char *function_name,
    std::vector<ObjectId> object_refs,
    std::string payload,
    std::vector<BlobId> blob_refs = {})
{
  EventRecord event;
  event.kind = EventKind::Call;
  event.callsite.sequence = sequence;
  event.callsite.function_name = function_name ? function_name : "";
  event.object_refs = std::move(object_refs);
  event.payload = std::move(payload);
  event.blob_refs = std::move(blob_refs);
  return event;
}

EventRecord boundary(std::uint64_t sequence, BoundaryKind kind, std::string payload)
{
  EventRecord event;
  event.kind = EventKind::Boundary;
  event.callsite.sequence = sequence;
  event.boundary = kind;
  event.payload = std::move(payload);
  return event;
}

EventRecord resource_blob(
    std::uint64_t sequence,
    const char *debug_name,
    std::vector<BlobId> blob_refs,
    std::string payload)
{
  EventRecord event;
  event.kind = EventKind::ResourceBlob;
  event.callsite.sequence = sequence;
  event.callsite.function_name = "resource_blob";
  event.object_debug_name = debug_name ? debug_name : "";
  event.blob_refs = std::move(blob_refs);
  event.payload = std::move(payload);
  return event;
}

std::string texture_descriptor_json()
{
  return "\"descriptor\":{\"type\":2,\"width\":2,\"height\":2,\"depth\":1,"
         "\"array_length\":1,\"pixel_format\":\"bgra8unorm\",\"usage\":5,"
         "\"mipmap_level_count\":1,\"sample_count\":1";
}

std::string render_pass_payload(std::uint64_t color_texture_id)
{
  return "{\"command_buffer_id\":3,\"render_pass_info\":{\"color_texture_id\":" +
         std::to_string(color_texture_id) +
         ",\"color_pixel_format\":\"bgra8unorm\",\"width\":2,\"height\":2,"
         "\"load_action\":2,\"store_action\":1,\"clear_color\":[0.0,0.0,0.0,1.0],"
         "\"slot\":0,\"level\":0,\"slice\":0,\"depth_plane\":0}}";
}

apitrace::trace::MetalEventRecord metal_event(
    MetalCallKind kind,
    std::uint64_t sequence,
    std::uint64_t object_id,
    const char *function_name,
    std::string payload,
    std::vector<BlobId> blob_refs = {},
    std::vector<ObjectId> object_refs = {})
{
  apitrace::trace::MetalEventRecord event;
  event.call_kind = kind;
  event.metal_sequence = sequence;
  event.object_id = object_id;
  event.function_name = function_name ? function_name : "";
  event.payload = std::move(payload);
  event.blob_refs = std::move(blob_refs);
  event.object_refs = std::move(object_refs);
  apitrace::trace::append_payload_text_object_refs(event.payload, event.object_refs);
  return event;
}

apitrace::trace::AssetRecord asset(
    BlobId blob_id,
    AssetKind kind,
    const char *debug_name,
    std::string payload)
{
  apitrace::trace::AssetRecord record;
  record.blob_id = blob_id;
  record.kind = kind;
  record.debug_name = debug_name ? debug_name : "";
  record.payload_bytes.assign(payload.begin(), payload.end());
  return record;
}

std::string buffer_desc_json(std::uint64_t width)
{
  return std::string("{")
      + "\"dimension\":1,"
      + "\"alignment\":0,"
      + "\"width\":" + std::to_string(width) + ","
      + "\"height\":1,"
      + "\"depth_or_array_size\":1,"
      + "\"mip_levels\":1,"
      + "\"format\":0,"
      + "\"sample_count\":1,"
      + "\"sample_quality\":0,"
      + "\"layout\":1,"
      + "\"flags\":0"
      + "}";
}

std::string texture2d_desc_json(std::uint64_t width, std::uint32_t height, std::uint32_t format)
{
  return std::string("{")
      + "\"dimension\":3,"
      + "\"alignment\":0,"
      + "\"width\":" + std::to_string(width) + ","
      + "\"height\":" + std::to_string(height) + ","
      + "\"depth_or_array_size\":1,"
      + "\"mip_levels\":1,"
      + "\"format\":" + std::to_string(format) + ","
      + "\"sample_count\":1,"
      + "\"sample_quality\":0,"
      + "\"layout\":0,"
      + "\"flags\":0"
      + "}";
}

std::string shell_quote_path(const std::filesystem::path &path)
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

int run_command(const std::string &command)
{
  return std::system(command.c_str());
}

bool run_tool(
    const std::filesystem::path &tool,
    const std::string &options,
    const std::filesystem::path &bundle)
{
  return run_command(shell_quote_path(tool) + " " + options + " " + shell_quote_path(bundle)) == 0;
}

bool append_translation_link(
    apitrace::trace::TranslationLinkWriter &writer,
    const char *scope_kind,
    std::uint64_t d3d_sequence,
    std::uint64_t metal_begin,
    std::uint64_t metal_end)
{
  apitrace::trace::TranslationLinkRecord record;
  record.record_type = "scope";
  record.scope_kind = scope_kind ? scope_kind : "";
  record.d3d_sequence = d3d_sequence;
  record.metal_sequence_begin = metal_begin;
  record.metal_sequence_end = metal_end;
  record.frame_id = 0;
  record.payload = "{\"encoder\":\"render\"}";
  writer.append_record(record);
  return true;
}

struct CrossApiBundleOptions {
  bool link_draw_to_pipeline_work = true;
};

bool write_cross_api_bundle(
    const std::filesystem::path &bundle,
    const CrossApiBundleOptions &options,
    std::string &error)
{
  std::filesystem::remove_all(bundle);

  TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    error = "failed to open bundle";
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_cross_api_bundle_closure";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({
      {300, ObjectKind::Resource, 0, "metal-drawable"},
  });

  auto shared_buffer = asset(1000, AssetKind::Buffer, "shared-buffer", std::string(256, '\x2a'));
  shared_buffer = writer.register_asset(std::move(shared_buffer));
  auto shared_buffer_alias = asset(1001, AssetKind::Buffer, "shared-buffer-metal-alias", std::string(256, '\x2a'));
  shared_buffer_alias = writer.register_metal_asset(MetalAssetKind::Buffer, std::move(shared_buffer_alias));
  if (shared_buffer.relative_path != shared_buffer_alias.relative_path) {
    error = "shared buffer was not stored under a single API-independent path";
    return false;
  }

  auto shared_texture = asset(1002, AssetKind::Texture, "shared-texture", std::string(16, '\x7f'));
  shared_texture = writer.register_asset(std::move(shared_texture));
  auto shared_texture_alias = asset(1003, AssetKind::Texture, "shared-texture-metal-alias", std::string(16, '\x7f'));
  shared_texture_alias = writer.register_metal_asset(MetalAssetKind::Texture, std::move(shared_texture_alias));
  if (shared_texture.relative_path != shared_texture_alias.relative_path) {
    error = "shared texture was not stored under a single API-independent path";
    return false;
  }

  const std::string present_pixels("\x10\x20\x30\xff\x40\x50\x60\xff\x70\x80\x90\xff\xa0\xb0\xc0\xff", 16);
  auto d3d_present = asset(1004, AssetKind::Texture, "d3d-present-frame", present_pixels);
  d3d_present = writer.register_asset(std::move(d3d_present));
  auto metal_present = asset(1005, AssetKind::Texture, "metal-present-frame", present_pixels);
  metal_present = writer.register_asset(std::move(metal_present));

  auto root_signature = asset(1006, AssetKind::RootSignature, "rootsig", "rootsig");
  root_signature = writer.register_asset(std::move(root_signature));
  auto compute_shader = asset(1007, AssetKind::ShaderDxil, "cs", "cs");
  compute_shader = writer.register_asset(std::move(compute_shader));
  const std::string compute_pipeline_json =
      "{\"type\":\"compute\",\"root_signature_object_id\":10,\"node_mask\":0,\"flags\":0,"
      "\"cs\":{\"bytecode_size\":2,\"cs_path\":\"" +
      compute_shader.relative_path.generic_string() + "\"}}";
  auto compute_pipeline = asset(1008, AssetKind::Pipeline, "compute-pipeline", compute_pipeline_json);
  compute_pipeline = writer.register_asset(std::move(compute_pipeline));

  const auto metal_library = writer.register_metal_asset(
      MetalAssetKind::Library,
      asset(1009, AssetKind::Unknown, "smoke.metallib", "fake-metallib-bytes"));
  const std::string metal_pipeline_json =
      "{\"library_id\":1,\"vertex_library_id\":1,\"fragment_library_id\":1,"
      "\"vertex_function\":\"vs_main\",\"fragment_function\":\"fs_main\","
      "\"colors\":[{\"pixel_format\":\"bgra8unorm\",\"blending_enabled\":false,"
      "\"write_mask\":15,\"rgb_blend_operation\":0,\"alpha_blend_operation\":0,"
      "\"src_rgb_blend_factor\":1,\"dst_rgb_blend_factor\":0,"
      "\"src_alpha_blend_factor\":1,\"dst_alpha_blend_factor\":0}],"
      "\"rasterization_enabled\":true,"
      "\"raster_sample_count\":1}";
  const auto metal_pipeline = writer.register_metal_asset(
      MetalAssetKind::RenderPipeline,
      asset(1010, AssetKind::Unknown, "smoke.pipeline", metal_pipeline_json));

  if (metal_library.relative_path.empty() || metal_pipeline.relative_path.empty()) {
    error = "failed to write Metal replay assets";
    return false;
  }

  std::uint64_t sequence = 1;
  writer.append_call_event(object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  writer.append_call_event(call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));

  writer.append_call_event(object_create(sequence++, 4, ObjectKind::Resource, 1, "ID3D12Resource"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
      "\"resource_desc\":" + buffer_desc_json(256) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(resource_blob(
      sequence++,
      "D3DSharedBuffer",
      {shared_buffer.blob_id},
      "{\"buffer_path\":\"" + shared_buffer.relative_path.generic_string() + "\"}"));

  writer.append_call_event(object_create(sequence++, 9, ObjectKind::Resource, 1, "ID3D12Resource"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 9},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(2, 2, 28) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(resource_blob(
      sequence++,
      "D3DSharedTexture",
      {shared_texture.blob_id},
      "{\"texture_path\":\"" + shared_texture.relative_path.generic_string() + "\"}"));

  writer.append_call_event(object_create(sequence++, 19, ObjectKind::Resource, 1, "IDXGISwapChainBackBuffer"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 19},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":4,\"gpu_virtual_address\":0,"
      "\"swapchain_back_buffer\":true,\"buffer_index\":0,"
      "\"resource_desc\":" + texture2d_desc_json(2, 2, 28) + ",\"optimized_clear_value\":null}"));

  writer.append_call_event(object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature.relative_path.generic_string() + "\"}",
      {root_signature.blob_id}));

  writer.append_call_event(object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateComputePipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + compute_pipeline.relative_path.generic_string() + "\"}",
      {compute_pipeline.blob_id, compute_shader.blob_id}));

  writer.append_call_event(object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommandQueue",
      {1, 13},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  writer.append_call_event(object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  writer.append_call_event(call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  writer.append_call_event(object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  writer.append_call_event(call(sequence++, "ID3D12GraphicsCommandList::SetComputeRootSignature", {15, 10}, "{}"));
  const auto d3d_dispatch_sequence = sequence;
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::Dispatch",
      {15},
      "{\"thread_group_count_x\":1,\"thread_group_count_y\":1,\"thread_group_count_z\":1}"));
  writer.append_call_event(call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));

  writer.append_call_event(resource_blob(
      sequence++,
      "D3D12PresentFrame",
      {d3d_present.blob_id},
      "{\"frame_index\":0,\"width\":2,\"height\":2,\"row_pitch\":8,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\",\"frame_path\":\"" +
          d3d_present.relative_path.generic_string() + "\"}"));
  writer.append_call_event(boundary(sequence++, BoundaryKind::Frame, "{\"label\":\"FrameBegin\",\"frame_index\":0}"));
  const auto d3d_execute_sequence = sequence;
  writer.append_call_event(call(
      sequence++,
      "ID3D12CommandQueue::ExecuteCommandLists",
      {13, 15},
      "{\"command_list_count\":1}"));
  writer.append_call_event(call(
      sequence++,
      "IDXGISwapChain::Present",
      {},
      "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}"));
  writer.append_call_event(boundary(
      sequence++,
      BoundaryKind::Present,
      "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}"));
  writer.append_call_event(boundary(sequence++, BoundaryKind::Frame, "{\"label\":\"FrameEnd\",\"frame_index\":0}"));

  writer.append_metal_event(metal_event(
      MetalCallKind::DeviceCreate,
      1,
      1,
      "MTLDevice.newLibrary",
      "{\"library_path\":\"" + metal_library.relative_path.generic_string() + "\",\"size\":18}",
      {metal_library.blob_id}));
  writer.append_metal_event(metal_event(
      MetalCallKind::DeviceCreate,
      2,
      2,
      "MTLDevice.newRenderPipelineState",
      "{\"descriptor_path\":\"" + metal_pipeline.relative_path.generic_string() + "\"}",
      {metal_pipeline.blob_id}));
  writer.append_metal_event(metal_event(
      MetalCallKind::DeviceCreate,
      3,
      101,
      "MTLDevice.newBuffer",
      "{\"buffer_path\":\"" + shared_buffer_alias.relative_path.generic_string() + "\",\"length\":256}",
      {shared_buffer_alias.blob_id}));
  writer.append_metal_event(metal_event(
      MetalCallKind::DeviceCreate,
      4,
      102,
      "MTLDevice.newTexture",
      "{\"texture_path\":\"" + shared_texture_alias.relative_path.generic_string() +
          "\"," + texture_descriptor_json() + ","
          "\"bytes_per_row\":8,\"initial_bytes\":[127,127,127,127,127,127,127,127,"
          "127,127,127,127,127,127,127,127]}}",
      {shared_texture_alias.blob_id}));
  writer.append_metal_event(metal_event(
      MetalCallKind::DeviceCreate,
      5,
      300,
      "MTLDevice.newTexture",
      "{" + texture_descriptor_json() + "}}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::CommandBufferBegin,
      6,
      3,
      "MTLCommandQueue.commandBuffer",
      "{\"frame_id\":0}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::RenderEncoderBegin,
      7,
      4,
      "MTLCommandBuffer.renderCommandEncoder",
      render_pass_payload(300)));
  writer.append_metal_event(metal_event(
      MetalCallKind::SetRenderPipelineState,
      8,
      4,
      "MTLRenderCommandEncoder.setRenderPipelineState",
      "{\"pipeline_state_id\":2}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::SetViewport,
      9,
      4,
      "MTLRenderCommandEncoder.setViewport",
      "{\"payload\":{\"viewports\":[[0.0,0.0,2.0,2.0,0.0,1.0]]}}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::SetScissorRect,
      10,
      4,
      "MTLRenderCommandEncoder.setScissorRect",
      "{\"payload\":{\"rects\":[[0,0,2,2]]}}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::DrawPrimitives,
      11,
      4,
      "MTLRenderCommandEncoder.drawPrimitives",
      "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3,\"instance_count\":1,\"base_instance\":0}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::RenderEncoderEnd,
      12,
      4,
      "MTLRenderCommandEncoder.endEncoding",
      "{}"));
  writer.append_metal_event(metal_event(
      MetalCallKind::PresentDrawable,
      13,
      3,
      "MTLCommandBuffer.presentDrawable",
      "{\"drawable_handle\":300,\"present_texture_id\":300,\"frame_index\":0,"
      "\"width\":2,\"height\":2,\"sync_interval\":1,\"flags\":0}",
      {},
      {300}));
  writer.append_metal_event(metal_event(
      MetalCallKind::CommandBufferCommit,
      14,
      3,
      "MTLCommandBuffer.commit",
      "{}"));
  writer.append_call_event(resource_blob(
      sequence++,
      "MetalPresentFrame",
      {metal_present.blob_id},
      "{\"frame_index\":0,\"width\":2,\"height\":2,\"row_pitch\":8,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\",\"frame_path\":\"" +
          metal_present.relative_path.generic_string() + "\"}"));

  apitrace::trace::TranslationLinkWriter link_writer;
  apitrace::trace::TranslationLinkStreamOptions link_options;
  link_options.stream_name = "translation-links";
  link_options.producer_name = "test_cross_api_bundle_closure";
  if (!link_writer.open(writer, link_options)) {
    error = "failed to open translation link writer";
    return false;
  }
  const auto linked_d3d_sequence =
      options.link_draw_to_pipeline_work ? d3d_dispatch_sequence : d3d_execute_sequence;
  append_translation_link(link_writer, "encoder", d3d_dispatch_sequence, 7, 12);
  append_translation_link(link_writer, "draw_to_metal_calls", linked_d3d_sequence, 8, 11);
  link_writer.close();

  writer.close();
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2 && argc != 3 && argc != 4) {
    std::cerr << "usage: test_cross_api_bundle_closure <bundle> [bundle-check] [retrace]\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  const std::filesystem::path bundle_check = argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path();
  const std::filesystem::path retrace = argc >= 4 ? std::filesystem::path(argv[3]) : std::filesystem::path();
  std::string error;
  if (!write_cross_api_bundle(bundle, CrossApiBundleOptions{}, error)) {
    std::cerr << error << "\n";
    return 1;
  }

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    std::cerr << "reader failed to reopen bundle: " << reader.last_error() << "\n";
    return 1;
  }

  apitrace::d3d12::D3D12ReplayBackend d3d_backend;
  if (!d3d_backend.initialize(reader)) {
    std::cerr << "D3D backend initialize failed: " << d3d_backend.last_error() << "\n";
    return 1;
  }
  for (const auto &event : reader.events()) {
    if (!d3d_backend.replay_event(event)) {
      std::cerr << "D3D replay_event failed: " << d3d_backend.last_error() << "\n";
      return 1;
    }
  }
  if (!d3d_backend.validate_only()) {
    std::cerr << "D3D validate-only failed: " << d3d_backend.last_error() << "\n";
    return 1;
  }

  if (!bundle_check.empty() &&
      !run_tool(
          bundle_check,
          "--strict-cross-api",
          bundle)) {
    std::cerr << "bundle-check rejected the cross-api closure fixture\n";
    return 1;
  }

  if (!bundle_check.empty()) {
    const auto broken_link_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-broken-d3d-draw-link");
    CrossApiBundleOptions broken_options;
    broken_options.link_draw_to_pipeline_work = false;
    if (!write_cross_api_bundle(broken_link_bundle, broken_options, error)) {
      std::cerr << error << "\n";
      return 1;
    }
    if (run_tool(
            bundle_check,
            "--require-d3d --require-metal --require-translation-links --require-shared-resources "
            "--require-d3d-replay-closure --require-d3d-present-frames "
            "--require-metal-replay-closure --require-metal-present-frames",
            broken_link_bundle)) {
      std::cerr << "bundle-check accepted Metal draw work linked to non-pipeline D3D metadata\n";
      return 1;
    }
  }

  if (!retrace.empty()) {
    if (!run_tool(retrace, "--validate-only", bundle)) {
      std::cerr << "D3D retrace validate-only rejected the cross-api closure fixture\n";
      return 1;
    }
    if (!run_tool(retrace, "--metal --validate-only", bundle)) {
      std::cerr << "Metal retrace validate-only rejected the cross-api closure fixture\n";
      return 1;
    }
  }

  return 0;
}
