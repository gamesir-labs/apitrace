#include "apitrace/asset_index.hpp"
#include "apitrace/d3d12_replay.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using apitrace::trace::AssetKind;
using apitrace::trace::BlobId;
using apitrace::trace::BoundaryKind;
using apitrace::trace::EventKind;
using apitrace::trace::EventRecord;
using apitrace::trace::ObjectId;
using apitrace::trace::ObjectKind;
using apitrace::trace::TraceBundleReader;
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

apitrace::trace::AssetRecord register_buffer_asset(
    TraceBundleWriter &writer,
    BlobId blob_id,
    const char *debug_name,
    const std::vector<std::uint8_t> &bytes)
{
  apitrace::trace::AssetRecord asset;
  asset.blob_id = blob_id;
  asset.kind = AssetKind::Buffer;
  asset.debug_name = debug_name ? debug_name : "";
  asset.payload_bytes = bytes;
  return writer.register_asset(std::move(asset));
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0) {
    return {};
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
      return {};
    }
  }
  return bytes;
}

bool register_file_asset(
    TraceBundleWriter &writer,
    AssetKind kind,
    BlobId blob_id,
    const char *debug_name,
    const std::filesystem::path &path,
    apitrace::trace::AssetRecord &out,
    std::uint64_t &payload_size)
{
  apitrace::trace::AssetRecord asset;
  asset.blob_id = blob_id;
  asset.kind = kind;
  asset.debug_name = debug_name ? debug_name : "";
  asset.payload_bytes = read_binary_file(path);
  if (asset.payload_bytes.empty()) {
    return false;
  }
  payload_size = asset.payload_bytes.size();
  out = writer.register_asset(std::move(asset));
  return !out.relative_path.empty();
}

struct ShaderAssetSource {
  AssetKind kind = AssetKind::ShaderDxbc;
  std::filesystem::path path;
};

bool select_shader_asset(
    const std::filesystem::path &asset_dir,
    const char *stem,
    ShaderAssetSource &out)
{
  const auto dxil_path = asset_dir / (std::string(stem) + ".dxil");
  if (std::filesystem::exists(dxil_path)) {
    out.kind = AssetKind::ShaderDxil;
    out.path = dxil_path;
    return true;
  }

  const auto dxbc_path = asset_dir / (std::string(stem) + ".dxbc");
  if (std::filesystem::exists(dxbc_path)) {
    out.kind = AssetKind::ShaderDxbc;
    out.path = dxbc_path;
    return true;
  }

  return false;
}

std::string shader_stage_json(
    const char *stage,
    const apitrace::trace::AssetRecord &asset,
    std::uint64_t bytecode_size)
{
  return std::string("\"") + stage + "\":{\"bytecode_size\":" + std::to_string(bytecode_size) +
      ",\"" + stage + "_path\":\"" + asset.relative_path.generic_string() + "\"}";
}

std::string default_blend_state_json()
{
  std::ostringstream payload;
  payload << "{\"alpha_to_coverage_enable\":false,\"independent_blend_enable\":false,\"render_targets\":[";
  for (int index = 0; index < 8; ++index) {
    if (index) {
      payload << ",";
    }
    payload << "{\"blend_enable\":false,\"logic_op_enable\":false,"
            << "\"src_blend\":2,\"dest_blend\":1,\"blend_op\":1,"
            << "\"src_blend_alpha\":2,\"dest_blend_alpha\":1,\"blend_op_alpha\":1,"
            << "\"logic_op\":5,\"render_target_write_mask\":15}";
  }
  payload << "]}";
  return payload.str();
}

std::string default_rasterizer_state_json()
{
  return "{\"fill_mode\":3,\"cull_mode\":1,\"front_counter_clockwise\":false,"
      "\"depth_bias\":0,\"depth_bias_clamp\":0,\"slope_scaled_depth_bias\":0,"
      "\"depth_clip_enable\":true,\"multisample_enable\":false,"
      "\"antialiased_line_enable\":false,\"forced_sample_count\":0,"
      "\"conservative_raster\":0}";
}

std::string default_depth_stencil_state_json()
{
  return "{\"depth_enable\":false,\"depth_write_mask\":0,\"depth_func\":8,"
      "\"stencil_enable\":false,\"stencil_read_mask\":255,\"stencil_write_mask\":255,"
      "\"front_face\":{\"stencil_fail_op\":1,\"stencil_depth_fail_op\":1,"
      "\"stencil_pass_op\":1,\"stencil_func\":1},"
      "\"back_face\":{\"stencil_fail_op\":1,\"stencil_depth_fail_op\":1,"
      "\"stencil_pass_op\":1,\"stencil_func\":1}}";
}

std::string graphics_pipeline_json(
    const apitrace::trace::AssetRecord &vs,
    std::uint64_t vs_size,
    const apitrace::trace::AssetRecord &ps,
    std::uint64_t ps_size)
{
  std::ostringstream pipeline;
  pipeline << "{\"type\":\"graphics\""
           << ",\"root_signature_object_id\":7"
           << ",\"node_mask\":0"
           << ",\"flags\":0"
           << ",\"input_layout\":{\"element_count\":0,\"elements\":[]}"
           << ",\"blend_state\":" << default_blend_state_json()
           << ",\"sample_mask\":4294967295"
           << ",\"rasterizer_state\":" << default_rasterizer_state_json()
           << ",\"depth_stencil_state\":" << default_depth_stencil_state_json()
           << ",\"stream_output\":{\"declaration_count\":0,\"stride_count\":0,\"rasterized_stream\":0}"
           << ",\"primitive_topology_type\":3"
           << ",\"num_render_targets\":1"
           << ",\"rtv_formats\":[28,0,0,0,0,0,0,0]"
           << ",\"dsv_format\":0"
           << ",\"sample_desc\":{\"count\":1,\"quality\":0}"
           << ",\"ib_strip_cut_value\":0"
           << "," << shader_stage_json("vs", vs, vs_size)
           << "," << shader_stage_json("ps", ps, ps_size)
           << "}";
  return pipeline.str();
}

bool replay_all_events(TraceBundleReader &reader, apitrace::d3d12::D3D12ReplayBackend &backend)
{
  if (!backend.initialize(reader)) {
    return false;
  }
  for (const auto &event : reader.events()) {
    if (!backend.replay_event(event)) {
      return false;
    }
  }
  return true;
}

bool write_bundle(
    const std::filesystem::path &bundle,
    const std::filesystem::path &asset_dir,
    bool poison_present_frame)
{
  std::filesystem::remove_all(bundle);

  TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_native_replay_smoke";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord present_frame;
  present_frame.blob_id = 2000;
  present_frame.kind = AssetKind::Texture;
  present_frame.debug_name = "d3d12-native-smoke-present-frame";
  present_frame.payload_bytes.assign(4 * 4 * 4, poison_present_frame ? 0x00 : 0xff);
  present_frame = writer.register_asset(std::move(present_frame));

  apitrace::trace::AssetRecord root_signature;
  apitrace::trace::AssetRecord vs;
  apitrace::trace::AssetRecord ps;
  std::uint64_t root_signature_size = 0;
  std::uint64_t vs_size = 0;
  std::uint64_t ps_size = 0;
  ShaderAssetSource vs_source;
  ShaderAssetSource ps_source;
  if (!select_shader_asset(asset_dir, "fullscreen_triangle.vs", vs_source) ||
      !select_shader_asset(asset_dir, "fullscreen_triangle.ps", ps_source)) {
    return false;
  }

  if (!register_file_asset(
          writer,
          AssetKind::RootSignature,
          2001,
          "d3d12-native-smoke-root-signature",
          asset_dir / "fullscreen_triangle.rootsig",
          root_signature,
          root_signature_size) ||
      !register_file_asset(
          writer,
          vs_source.kind,
          2002,
          "d3d12-native-smoke-vs",
          vs_source.path,
          vs,
          vs_size) ||
      !register_file_asset(
          writer,
          ps_source.kind,
          2003,
          "d3d12-native-smoke-ps",
          ps_source.path,
          ps,
          ps_size)) {
    return false;
  }

  apitrace::trace::AssetRecord pipeline;
  pipeline.blob_id = 2004;
  pipeline.kind = AssetKind::Pipeline;
  pipeline.debug_name = "d3d12-native-smoke-graphics-pipeline";
  const auto pipeline_text = graphics_pipeline_json(vs, vs_size, ps, ps_size);
  pipeline.payload_bytes.assign(pipeline_text.begin(), pipeline_text.end());
  pipeline = writer.register_asset(std::move(pipeline));

  std::vector<std::uint8_t> vertex_bytes(48);
  for (std::size_t index = 0; index < vertex_bytes.size(); ++index) {
    vertex_bytes[index] = static_cast<std::uint8_t>(index * 3u);
  }
  std::vector<std::uint8_t> index_bytes = {0, 0, 1, 0, 2, 0};
  std::vector<std::uint8_t> constant_bytes(256);
  for (std::size_t index = 0; index < constant_bytes.size(); ++index) {
    constant_bytes[index] = static_cast<std::uint8_t>(0x80u + (index & 0x7fu));
  }
  std::vector<std::uint8_t> texture_upload_bytes(256 * 4);
  for (std::size_t index = 0; index < texture_upload_bytes.size(); index += 4) {
    texture_upload_bytes[index + 0] = static_cast<std::uint8_t>(index);
    texture_upload_bytes[index + 1] = static_cast<std::uint8_t>(0x40u + index);
    texture_upload_bytes[index + 2] = static_cast<std::uint8_t>(0x80u + index);
    texture_upload_bytes[index + 3] = 0xff;
  }

  auto vertex_asset = register_buffer_asset(writer, 2005, "d3d12-native-smoke-vertex-buffer", vertex_bytes);
  auto index_asset = register_buffer_asset(writer, 2006, "d3d12-native-smoke-index-buffer", index_bytes);
  auto constant_asset = register_buffer_asset(writer, 2007, "d3d12-native-smoke-constant-buffer", constant_bytes);
  auto texture_upload_asset = register_buffer_asset(writer, 2008, "d3d12-native-smoke-texture-upload", texture_upload_bytes);
  if (vertex_asset.relative_path.empty() ||
      index_asset.relative_path.empty() ||
      constant_asset.relative_path.empty() ||
      texture_upload_asset.relative_path.empty()) {
    return false;
  }

  std::uint64_t sequence = 1;
  writer.append_call_event(object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  writer.append_call_event(call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));

  writer.append_call_event(object_create(sequence++, 2, ObjectKind::Resource, 1, "IDXGISwapChainBackBuffer"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 2},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"swapchain_back_buffer\":true,\"buffer_index\":0,"
      "\"resource_desc\":" + texture2d_desc_json(4, 4, 28) + ",\"optimized_clear_value\":null}"));

  writer.append_call_event(object_create(sequence++, 9, ObjectKind::Resource, 1, "VertexBuffer"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 9},
      "{\"heap_type\":2,\"heap_flags\":0,\"initial_state\":1,\"gpu_virtual_address\":268435456,"
      "\"resource_desc\":" + buffer_desc_json(vertex_bytes.size()) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(call(sequence++, "ID3D12Resource::Map", {9}, "{\"subresource\":0,\"mapped\":true}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Resource::Unmap",
      {9},
      "{\"subresource\":0,\"written_begin\":0,\"written_end\":" + std::to_string(vertex_bytes.size()) +
          ",\"buffer_path\":\"" + vertex_asset.relative_path.generic_string() + "\"}",
      {vertex_asset.blob_id}));

  writer.append_call_event(object_create(sequence++, 10, ObjectKind::Resource, 1, "IndexBuffer"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 10},
      "{\"heap_type\":2,\"heap_flags\":0,\"initial_state\":1,\"gpu_virtual_address\":268439552,"
      "\"resource_desc\":" + buffer_desc_json(index_bytes.size()) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(call(sequence++, "ID3D12Resource::Map", {10}, "{\"subresource\":0,\"mapped\":true}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Resource::Unmap",
      {10},
      "{\"subresource\":0,\"written_begin\":0,\"written_end\":" + std::to_string(index_bytes.size()) +
          ",\"buffer_path\":\"" + index_asset.relative_path.generic_string() + "\"}",
      {index_asset.blob_id}));

  writer.append_call_event(object_create(sequence++, 11, ObjectKind::Resource, 1, "ConstantBuffer"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 11},
      "{\"heap_type\":2,\"heap_flags\":0,\"initial_state\":1,\"gpu_virtual_address\":268443648,"
      "\"resource_desc\":" + buffer_desc_json(constant_bytes.size()) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(call(sequence++, "ID3D12Resource::Map", {11}, "{\"subresource\":0,\"mapped\":true}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Resource::Unmap",
      {11},
      "{\"subresource\":0,\"written_begin\":0,\"written_end\":" + std::to_string(constant_bytes.size()) +
          ",\"buffer_path\":\"" + constant_asset.relative_path.generic_string() + "\"}",
      {constant_asset.blob_id}));

  writer.append_call_event(object_create(sequence++, 12, ObjectKind::Resource, 1, "ShaderResourceTexture"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 12},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":1,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(4, 4, 28) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(object_create(sequence++, 14, ObjectKind::Resource, 1, "TextureUploadBuffer"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 14},
      "{\"heap_type\":2,\"heap_flags\":0,\"initial_state\":1,\"gpu_virtual_address\":268447744,"
      "\"resource_desc\":" + buffer_desc_json(texture_upload_bytes.size()) + ",\"optimized_clear_value\":null}"));
  writer.append_call_event(call(sequence++, "ID3D12Resource::Map", {14}, "{\"subresource\":0,\"mapped\":true}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Resource::Unmap",
      {14},
      "{\"subresource\":0,\"written_begin\":0,\"written_end\":" + std::to_string(texture_upload_bytes.size()) +
          ",\"buffer_path\":\"" + texture_upload_asset.relative_path.generic_string() + "\"}",
      {texture_upload_asset.blob_id}));

  writer.append_call_event(object_create(sequence++, 3, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 3},
      "{\"type\":1,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":0}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 2},
      "{\"descriptor\":8192,\"format\":0,\"view_dimension\":0}"));
  writer.append_call_event(object_create(sequence++, 13, ObjectKind::DescriptorHeap, 1, "ID3D12ShaderVisibleDescriptorHeap"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 13},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":40960}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 12},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"shader_4_component_mapping\":5768,\"view\":{\"most_detailed_mip\":0,"
      "\"mip_levels\":1,\"plane_slice\":0,\"resource_min_lod_clamp\":0}}"));

  writer.append_call_event(object_create(sequence++, 7, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 7},
      "{\"node_mask\":0,\"bytecode_size\":" + std::to_string(root_signature_size) +
          ",\"root_signature_path\":\"" + root_signature.relative_path.generic_string() +
          "\",\"descriptor_tables\":[{\"root_parameter_index\":1,\"shader_visibility\":0,"
          "\"ranges\":[{\"type\":0,\"descriptor_count\":1,\"base_shader_register\":0,"
          "\"register_space\":0,\"offset_from_table_start\":0,\"flags\":0}]}],"
          "\"root_parameters\":[{\"root_parameter_index\":0,\"parameter_type\":2,"
          "\"shader_visibility\":0,\"shader_register\":0,\"register_space\":0,"
          "\"num_32bit_values\":0,\"range_count\":0,\"flags\":0},"
          "{\"root_parameter_index\":1,\"parameter_type\":0,"
          "\"shader_visibility\":0,\"shader_register\":0,\"register_space\":0,"
          "\"num_32bit_values\":0,\"range_count\":1,\"flags\":0}]}",
      {root_signature.blob_id}));

  writer.append_call_event(object_create(sequence++, 8, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 8},
      "{\"pipeline_path\":\"" + pipeline.relative_path.generic_string() + "\"}",
      {pipeline.blob_id, vs.blob_id, ps.blob_id}));

  writer.append_call_event(object_create(sequence++, 4, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommandQueue",
      {1, 4},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  writer.append_call_event(object_create(sequence++, 5, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  writer.append_call_event(call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 5}, "{\"type\":0}"));
  writer.append_call_event(object_create(sequence++, 6, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 5, 0, 6},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":5,"
      "\"initial_pipeline_state_object_id\":0}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::ResourceBarrier",
      {6, 2},
      "{\"barrier_count\":1,\"barriers\":[{\"type\":0,\"flags\":0,"
      "\"resource_object_id\":2,\"before\":0,\"after\":4,\"subresource\":4294967295}]}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::ClearRenderTargetView",
      {6},
      "{\"descriptor\":8192,\"color\":[0.0,0.0,0.0,1.0],\"rect_count\":0,\"rects\":[]}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {6},
      "{\"render_target_count\":1,\"single_handle_to_descriptor_range\":false,"
      "\"single_descriptor_handle\":false,\"render_targets\":[8192],"
      "\"render_target_descriptors\":[{\"ptr\":8192}],\"first_rtv\":8192,"
      "\"dsv\":0,\"depth_stencil_descriptor\":null}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::RSSetViewports",
      {6},
      "{\"viewport_count\":1,\"viewports\":[{\"x\":0.0,\"y\":0.0,"
      "\"top_left_x\":0.0,\"top_left_y\":0.0,\"width\":4.0,\"height\":4.0,"
      "\"min_depth\":0.0,\"max_depth\":1.0}]}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::RSSetScissorRects",
      {6},
      "{\"rect_count\":1,\"rects\":[{\"left\":0,\"top\":0,\"right\":4,\"bottom\":4}]}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootSignature",
      {6, 7},
      "{\"compute\":false,\"root_signature_object_id\":7}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::SetPipelineState",
      {6, 8},
      "{\"pipeline_state_object_id\":8}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      {6, 12, 14},
      "{\"dst\":{\"resource_object_id\":12,\"type\":0,\"subresource_index\":0},"
      "\"dst_x\":0,\"dst_y\":0,\"dst_z\":0,"
      "\"src\":{\"resource_object_id\":14,\"type\":1,"
      "\"placed_footprint\":{\"offset\":0,\"format\":28,\"width\":4,"
      "\"height\":4,\"depth\":1,\"row_pitch\":256}},"
      "\"has_src_box\":false}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::SetDescriptorHeaps",
      {6, 13},
      "{\"heap_count\":1}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      {6},
      "{\"root_parameter_index\":1,\"base_descriptor\":40960}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::IASetPrimitiveTopology",
      {6},
      "{\"primitive_topology\":4}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::IASetVertexBuffers",
      {6, 9},
      "{\"start_slot\":0,\"view_count\":1,\"views\":[{\"buffer_location\":268435456,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":9,"
      "\"resolved_resource_offset\":0,\"size_in_bytes\":" + std::to_string(vertex_bytes.size()) +
          ",\"stride_in_bytes\":16}]}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::IASetIndexBuffer",
      {6, 10},
      "{\"buffer_location\":268439552,\"gpuva_resolve_status\":\"mapped\","
      "\"resolved_resource_object_id\":10,\"resolved_resource_offset\":0,"
      "\"size_in_bytes\":" + std::to_string(index_bytes.size()) + ",\"format\":57}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
      {6, 11},
      "{\"root_parameter_index\":0,\"buffer_location\":268443648,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":11,"
      "\"resolved_resource_offset\":0}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawIndexedInstanced",
      {6},
      "{\"index_count_per_instance\":3,\"instance_count\":1,"
      "\"start_index_location\":0,\"base_vertex_location\":0,\"start_instance_location\":0}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12GraphicsCommandList::ResourceBarrier",
      {6, 2},
      "{\"barrier_count\":1,\"barriers\":[{\"type\":0,\"flags\":0,"
      "\"resource_object_id\":2,\"before\":4,\"after\":0,\"subresource\":4294967295}]}"));
  writer.append_call_event(call(sequence++, "ID3D12GraphicsCommandList::Close", {6}, "{}"));

  writer.append_call_event(resource_blob(
      sequence++,
      "D3D12PresentFrame",
      {present_frame.blob_id},
      "{\"frame_index\":0,\"width\":4,\"height\":4,\"row_pitch\":16,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\",\"frame_path\":\"" +
          present_frame.relative_path.generic_string() + "\"}"));
  writer.append_call_event(boundary(sequence++, BoundaryKind::Frame, "{\"label\":\"FrameBegin\",\"frame_index\":0}"));
  writer.append_call_event(call(
      sequence++,
      "ID3D12CommandQueue::ExecuteCommandLists",
      {4, 6},
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

  writer.close();
  return true;
}

bool require_native_ready_bundle(const std::filesystem::path &bundle, std::string &error)
{
  TraceBundleReader reader;
  if (!reader.open(bundle)) {
    error = "reader failed to reopen bundle: " + reader.last_error();
    return false;
  }
  if (!reader.has_asset_index()) {
    error = "writer did not emit assets.json";
    return false;
  }

  apitrace::d3d12::D3D12ReplayBackend backend;
  if (!replay_all_events(reader, backend)) {
    error = "D3D12 validate replay failed: " + backend.last_error();
    return false;
  }
  if (!backend.validate_native_replay_readiness()) {
    error = "D3D12 native replay readiness failed: " + backend.last_error();
    return false;
  }

  return true;
}

bool expect_missing_resource_asset_failure(const std::filesystem::path &bundle)
{
  const auto broken_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-buffer-asset");
  std::filesystem::remove_all(broken_bundle);
  std::error_code copy_error;
  std::filesystem::copy(
      bundle,
      broken_bundle,
      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
      copy_error);
  if (copy_error) {
    std::cerr << "failed to copy negative bundle: " << copy_error.message() << "\n";
    return false;
  }

  const auto constant_path =
      broken_bundle / "buffers" / "4735f13c30847f0efd0b3cf9139c6f27242640219e6464b9cb12c21ba390b902.buffer";
  std::filesystem::remove(constant_path);

  std::string error;
  if (require_native_ready_bundle(broken_bundle, error)) {
    std::cerr << "native readiness accepted bundle with missing constant-buffer asset\n";
    return false;
  }
  if (error.find("missing referenced file") == std::string::npos &&
      error.find("missing mapped buffer asset") == std::string::npos &&
      error.find("failed to stat asset index path") == std::string::npos) {
    std::cerr << "missing resource asset failed with unexpected error: " << error << "\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 3 || argc > 4) {
    std::cerr << "usage: test_d3d12_native_replay_smoke <bundle> <asset-dir> [--poison-present-frame]\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  const std::filesystem::path asset_dir = argv[2];
  const bool poison_present_frame = argc == 4 && std::string(argv[3]) == "--poison-present-frame";
  if (argc == 4 && !poison_present_frame) {
    std::cerr << "unknown option: " << argv[3] << "\n";
    return 2;
  }
  if (!write_bundle(bundle, asset_dir, poison_present_frame)) {
    std::cerr << "failed to write D3D12 native replay smoke bundle\n";
    return 1;
  }

  std::string error;
  if (!require_native_ready_bundle(bundle, error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!poison_present_frame && !expect_missing_resource_asset_failure(bundle)) {
    return 1;
  }

  return 0;
}
