#include "apitrace/d3d12_replay.hpp"
#include "apitrace/asset_index.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using apitrace::trace::EventKind;
using apitrace::trace::EventRecord;
using apitrace::trace::ObjectId;
using apitrace::trace::ObjectKind;

} // namespace

namespace apitrace::d3d12 {

struct D3D12ReplayBackendTestHook {
  static const D3D12ReplayBackend::ResourceSemanticState *resource_state(
      const D3D12ReplayBackend &backend,
      apitrace::trace::ObjectId resource_object_id)
  {
    const auto it = backend.resources_.find(resource_object_id);
    if (it == backend.resources_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  static void seed_unbound_root_descriptor_table(D3D12ReplayBackend &backend)
  {
    using Backend = D3D12ReplayBackend;

    Backend::DescriptorHeapSemanticState heap;
    heap.heap_object_id = 2;
    heap.create_sequence = 1;
    heap.type = 0;
    heap.num_descriptors = 1;
    heap.flags = 1;
    heap.descriptor_size = 32;
    heap.cpu_start = 8192;
    heap.gpu_start = 40960;
    backend.descriptor_heaps_[heap.heap_object_id] = heap;

    Backend::CommandListSemanticState command_list;
    command_list.graphics_root_tables[0].descriptor = 40960;
    command_list.graphics_root_tables[0].heap_object_id = heap.heap_object_id;
    command_list.graphics_root_tables[0].heap_type = heap.type;
    command_list.graphics_root_tables[0].descriptor_index = 0;
    backend.command_lists_[15] = std::move(command_list);
  }
};

} // namespace apitrace::d3d12

namespace {

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
    std::vector<apitrace::trace::BlobId> blob_refs = {})
{
  EventRecord event;
  event.kind = EventKind::Call;
  event.callsite.sequence = sequence;
  event.callsite.function_name = function_name;
  event.object_refs = std::move(object_refs);
  event.blob_refs = std::move(blob_refs);
  event.payload = std::move(payload);
  return event;
}

EventRecord boundary(
    std::uint64_t sequence,
    apitrace::trace::BoundaryKind kind,
    std::string payload)
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
    std::vector<apitrace::trace::BlobId> blob_refs,
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

std::string texture2d_desc_json(std::uint64_t width, std::uint32_t height, std::uint32_t array_size, std::uint32_t format)
{
  return std::string("{")
      + "\"dimension\":3,"
      + "\"alignment\":0,"
      + "\"width\":" + std::to_string(width) + ","
      + "\"height\":" + std::to_string(height) + ","
      + "\"depth_or_array_size\":" + std::to_string(array_size) + ","
      + "\"mip_levels\":4,"
      + "\"format\":" + std::to_string(format) + ","
      + "\"sample_count\":1,"
      + "\"sample_quality\":0,"
      + "\"layout\":0,"
      + "\"flags\":3"
      + "}";
}

std::string texture3d_desc_json(std::uint64_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t format)
{
  return std::string("{")
      + "\"dimension\":4,"
      + "\"alignment\":0,"
      + "\"width\":" + std::to_string(width) + ","
      + "\"height\":" + std::to_string(height) + ","
      + "\"depth_or_array_size\":" + std::to_string(depth) + ","
      + "\"mip_levels\":4,"
      + "\"format\":" + std::to_string(format) + ","
      + "\"sample_count\":1,"
      + "\"sample_quality\":0,"
      + "\"layout\":0,"
      + "\"flags\":3"
      + "}";
}

std::string native_ready_graphics_pipeline_json(
    ObjectId root_signature_object_id,
    const apitrace::trace::AssetRecord &vertex_shader_asset);

bool append(apitrace::trace::TraceBundleWriter &writer, const EventRecord &event)
{
  writer.append_call_event(event);
  return true;
}

bool write_asset(
    apitrace::trace::TraceBundleWriter &writer,
    apitrace::trace::AssetKind kind,
    const char *debug_name,
    const std::string &text,
    apitrace::trace::AssetRecord &out)
{
  static apitrace::trace::BlobId next_blob_id = 1000;
  apitrace::trace::AssetRecord asset;
  asset.blob_id = next_blob_id++;
  asset.kind = kind;
  asset.debug_name = debug_name ? debug_name : "";
  asset.payload_bytes.assign(text.begin(), text.end());
  out = writer.register_asset(std::move(asset));
  return !out.relative_path.empty();
}

std::string asset_kind_name(apitrace::trace::AssetKind kind)
{
  switch (kind) {
  case apitrace::trace::AssetKind::ShaderDxbc:
    return "ShaderDxbc";
  case apitrace::trace::AssetKind::ShaderDxil:
    return "ShaderDxil";
  case apitrace::trace::AssetKind::RootSignature:
    return "RootSignature";
  case apitrace::trace::AssetKind::Texture:
    return "Texture";
  case apitrace::trace::AssetKind::Buffer:
    return "Buffer";
  case apitrace::trace::AssetKind::Pipeline:
    return "Pipeline";
  case apitrace::trace::AssetKind::ObjectIndex:
    return "ObjectIndex";
  case apitrace::trace::AssetKind::Analysis:
    return "Analysis";
  case apitrace::trace::AssetKind::Unknown:
    break;
  }
  return "Unknown";
}

bool write_asset_index_file(
    const std::filesystem::path &bundle,
    const std::vector<apitrace::trace::AssetRecord> &assets)
{
  std::ofstream output(bundle / apitrace::trace::kAssetIndexFileName, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  output << "{\n  \"assets\": [\n";
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto &asset = assets[index];
    output << "    {\"blob_id\":" << asset.blob_id
           << ",\"path\":\"" << asset.relative_path.generic_string()
           << "\",\"kind\":\"" << asset_kind_name(asset.kind)
           << "\",\"metal\":false,\"binary_payload\":true,\"byte_size\":" << asset.byte_size
           << "}";
    if (index + 1 != assets.size()) {
      output << ",";
    }
    output << "\n";
  }
  output << "  ]\n}\n";
  return true;
}

bool replace_file_text(
    const std::filesystem::path &path,
    const std::string &from,
    const std::string &to)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  auto text = buffer.str();
  const auto position = text.find(from);
  if (position == std::string::npos) {
    return false;
  }
  text.replace(position, from.size(), to);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  output << text;
  return true;
}

bool replay_all_events(apitrace::trace::TraceBundleReader &reader, apitrace::d3d12::D3D12ReplayBackend &backend)
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

std::filesystem::path g_bundle_finalize;
std::string g_open_bundle_error;

bool finalize_bundle(const std::filesystem::path &bundle)
{
  if (g_bundle_finalize.empty()) {
    g_open_bundle_error = "missing bundle-finalize path";
    return false;
  }
  const auto command = shell_quote_path(g_bundle_finalize) + " --jobs 1 " + shell_quote_path(bundle);
  const auto result = std::system(command.c_str());
  if (result != 0) {
    g_open_bundle_error = "bundle-finalize failed for " + bundle.string();
    return false;
  }
  return true;
}

bool open_finalized_bundle(apitrace::trace::TraceBundleReader &reader, const std::filesystem::path &bundle)
{
  if (!finalize_bundle(bundle)) {
    return false;
  }
  if (!reader.open(bundle)) {
    g_open_bundle_error = reader.last_error();
    return false;
  }
  g_open_bundle_error.clear();
  return true;
}

bool expect_d3d12_native_readiness_failure(
    const std::filesystem::path &bundle,
    const std::string &error_substring,
    std::string &error)
{
  apitrace::trace::TraceBundleReader reader;
  if (!open_finalized_bundle(reader, bundle)) {
    error = "reader failed to reopen bundle: " + g_open_bundle_error;
    return false;
  }
  apitrace::d3d12::D3D12ReplayBackend backend;
  if (!replay_all_events(reader, backend)) {
    error = "replay_event failed: " + backend.last_error();
    return false;
  }
  if (backend.validate_native_replay_readiness()) {
    error = "native readiness unexpectedly accepted bundle";
    return false;
  }
  if (backend.last_error().find(error_substring) == std::string::npos) {
    error = "native readiness failed with unexpected error: " + backend.last_error();
    return false;
  }
  return true;
}

bool expect_d3d12_replay_failure(
    const std::filesystem::path &bundle,
    const std::string &expected_error)
{
  apitrace::trace::TraceBundleReader reader;
  if (!open_finalized_bundle(reader, bundle)) {
    if (g_open_bundle_error.find(expected_error) != std::string::npos) {
      return true;
    }
    std::cerr << "reader failed with unexpected error: " << g_open_bundle_error << "\n";
    return false;
  }
  apitrace::d3d12::D3D12ReplayBackend backend;
  if (replay_all_events(reader, backend)) {
    std::cerr << "D3D12 replay accepted expected-failure bundle\n";
    return false;
  }
  if (backend.last_error().find(expected_error) == std::string::npos) {
    std::cerr << "D3D12 replay failed with unexpected error: " << backend.last_error() << "\n";
    return false;
  }
  return true;
}

bool error_contains_any(const std::string &error, std::initializer_list<const char *> needles)
{
  for (const char *needle : needles) {
    if (error.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

int run_bundle_check(
    const std::filesystem::path &bundle_check,
    const std::filesystem::path &bundle,
    const std::string &options)
{
  if (!finalize_bundle(bundle)) {
    std::cerr << g_open_bundle_error << "\n";
    return 1;
  }
  const auto command = shell_quote_path(bundle_check) + " " + options + " " + shell_quote_path(bundle);
  return std::system(command.c_str());
}

bool write_temporal_upscale_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_temporal_upscale_semantics";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 9, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 9},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":32768,"
      "\"resource_desc\":" + texture2d_desc_json(128, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandQueue",
      {1, 13},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandAllocator",
      {1, 14},
      "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandListExt::TemporalUpscale",
      {15, 9, 9, 9, 9, 0},
      "{\"input_content_width\":128,\"input_content_height\":64,"
      "\"auto_exposure\":true,\"in_reset\":false,\"depth_reversed\":true,"
      "\"motion_vector_in_display_res\":false,\"color_object_id\":9,"
      "\"depth_object_id\":9,\"motion_vector_object_id\":9,\"output_object_id\":9,"
      "\"motion_vector_scale_x\":0.5,\"motion_vector_scale_y\":-0.5,"
      "\"pre_exposure\":1.25,\"exposure_texture_object_id\":0,"
      "\"jitter_offset_x\":0.125,\"jitter_offset_y\":-0.25}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::Close",
      {15},
      "{}"));
  append(writer, call(
      sequence++,
      "ID3D12CommandQueue::ExecuteCommandLists",
      {13, 15},
      "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_dispatch_rays_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_dispatch_rays_semantics";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  apitrace::trace::AssetRecord root_signature_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::RootSignature,
          "rootsig",
          "rootsig",
          root_signature_asset)) {
    return false;
  }
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  apitrace::trace::AssetRecord compute_shader_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::ShaderDxil,
          "cs",
          "cs",
          compute_shader_asset)) {
    return false;
  }
  apitrace::trace::AssetRecord compute_pipeline_asset;
  const std::string compute_pipeline_json =
      "{\"type\":\"compute\",\"root_signature_object_id\":10,\"node_mask\":0,\"flags\":0,"
      "\"cs\":{\"bytecode_size\":2,\"cs_path\":\"" +
      compute_shader_asset.relative_path.generic_string() + "\"}}";
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "compute-pipeline",
          compute_pipeline_json,
          compute_pipeline_asset)) {
    return false;
  }
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateComputePipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + compute_pipeline_asset.relative_path.generic_string() + "\"}",
      {compute_pipeline_asset.blob_id, compute_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
      "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandQueue",
      {1, 13},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandAllocator",
      {1, 14},
      "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList4::DispatchRays",
      {15, 4},
      "{\"width\":16,\"height\":8,\"depth\":1,"
      "\"ray_generation_shader_record\":{\"start_address\":8192,\"size_in_bytes\":64,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":4,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096},"
      "\"miss_shader_table\":{\"start_address\":8256,\"size_in_bytes\":128,\"stride_in_bytes\":32,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":4,"
      "\"resolved_resource_offset\":64,\"resolved_resource_width\":4096},"
      "\"hit_group_table\":{\"start_address\":8384,\"size_in_bytes\":128,\"stride_in_bytes\":32,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":4,"
      "\"resolved_resource_offset\":192,\"resolved_resource_width\":4096},"
      "\"callable_shader_table\":{\"start_address\":0,\"size_in_bytes\":0,\"stride_in_bytes\":0,"
      "\"gpuva_resolve_status\":\"null\",\"resolved_resource_object_id\":0,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":0}}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12CommandQueue::ExecuteCommandLists",
      {13, 15},
      "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_resolve_region_requires_list1_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_resolve_region_requires_list1";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(128, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(128, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandQueue",
      {1, 13},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ResolveSubresourceRegion",
      {15, 4, 5},
      "{\"dst_resource_object_id\":4,\"dst_subresource\":0,\"dst_x\":4,\"dst_y\":0,"
      "\"src_resource_object_id\":5,\"src_subresource\":0,"
      "\"src_rect\":{\"left\":0,\"top\":0,\"right\":124,\"bottom\":64},"
      "\"format\":28,\"mode\":3}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12CommandQueue::ExecuteCommandLists",
      {13, 15},
      "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_create_command_list1_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_create_command_list1_semantics";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList1",
      {1, 15},
      "{\"node_mask\":2,\"type\":0,\"flags\":1}"));

  writer.close();
  return true;
}

bool write_descriptor_table_without_bound_heap_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_descriptor_table_without_bound_heap";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 2, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 2},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      {15},
      "{\"root_parameter_index\":0,\"base_descriptor\":40960}"));

  writer.close();
  return true;
}

bool write_descriptor_table_rebind_clears_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_descriptor_table_rebind_clears";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 2, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 2},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, object_create(sequence++, 3, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 3},
      "{\"type\":1,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":45056}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetDescriptorHeaps",
      {15, 2},
      "{\"heap_count\":1}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      {15},
      "{\"root_parameter_index\":0,\"base_descriptor\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetDescriptorHeaps",
      {15, 3},
      "{\"heap_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_with_late_descriptor_table_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_with_late_descriptor_table";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Texture"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetDescriptorHeaps", {15, 6}, "{\"heap_count\":1}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      {15},
      "{\"root_parameter_index\":0,\"base_descriptor\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":28,\"view_dimension\":4,"
      "\"shader_4_component_mapping\":5768,\"view\":{\"most_detailed_mip\":0,"
      "\"mip_levels\":1,\"plane_slice\":0,\"resource_min_lod_clamp\":0}}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_with_incomplete_descriptor_table_range_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_with_incomplete_descriptor_table_range";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Texture"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "CBV_SRV_UAV"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":2,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, object_create(sequence++, 8, ObjectKind::DescriptorHeap, 1, "RTV"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() +
          "\",\"descriptor_tables\":[{\"root_parameter_index\":0,\"shader_visibility\":0,"
          "\"ranges\":[{\"type\":0,\"descriptor_count\":2,\"base_shader_register\":0,"
          "\"register_space\":0,\"offset_from_table_start\":0,\"flags\":0}]}]}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":28,\"view_dimension\":4,"
      "\"shader_4_component_mapping\":5768,\"view\":{\"most_detailed_mip\":0,"
      "\"mip_levels\":1,\"plane_slice\":0,\"resource_min_lod_clamp\":0}}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 4},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetDescriptorHeaps", {15, 6}, "{\"heap_count\":1}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      "{\"render_target_count\":1,\"single_handle_to_descriptor_range\":false,"
      "\"render_targets\":[12288],\"dsv\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      {15},
      "{\"root_parameter_index\":0,\"base_descriptor\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_with_late_render_target_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_with_late_render_target";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 8, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      "{\"render_target_count\":1,\"single_handle_to_descriptor_range\":false,"
      "\"render_targets\":[12288],\"dsv\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 4},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_missing_raster_state_bundle(
    const std::filesystem::path &bundle,
    bool include_viewport,
    bool include_scissor,
    bool include_topology)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_missing_raster_state";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 8, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 4},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      "{\"render_target_count\":1,\"single_handle_to_descriptor_range\":false,"
      "\"render_targets\":[12288],\"dsv\":0}"));
  if (include_viewport) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::RSSetViewports",
        {15},
        "{\"viewport_count\":1,\"viewports\":[{\"x\":0,\"y\":0,\"width\":64,"
        "\"height\":64,\"min_depth\":0,\"max_depth\":1}]}"));
  }
  if (include_scissor) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::RSSetScissorRects",
        {15},
        "{\"rect_count\":1,\"rects\":[{\"left\":0,\"top\":0,\"right\":64,\"bottom\":64}]}"));
  }
  if (include_topology) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::IASetPrimitiveTopology",
        {15},
        "{\"primitive_topology\":4}"));
  }
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_render_pass_with_late_render_target_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_render_pass_with_late_render_target";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 8, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList4::BeginRenderPass",
      {15},
      "{\"render_targets_count\":1,"
      "\"render_targets\":[{\"cpu_descriptor\":12288,"
      "\"beginning_access\":{\"type\":1},"
      "\"ending_access\":{\"type\":1,\"src_resource_object_id\":0,\"dst_resource_object_id\":0,"
      "\"subresource_count\":0,\"subresources\":[],\"format\":0,\"resolve_mode\":0,"
      "\"preserve_resolve_source\":false}}],"
      "\"has_depth_stencil\":false,\"depth_stencil\":null,\"flags\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 4},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList4::EndRenderPass", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_write_buffer_immediate_bundle(const std::filesystem::path &bundle, std::uint64_t resolved_offset = 16)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_write_buffer_immediate";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Buffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList2::WriteBufferImmediate",
      {15, 5},
      "{\"count\":1,\"writes\":[{\"dest\":" + std::to_string(16384 + resolved_offset) + ","
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":5,"
      "\"resolved_resource_offset\":" + std::to_string(resolved_offset) +
      ",\"resolved_resource_width\":4096,"
      "\"value\":3735928559,\"mode\":null}]}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_copy_buffer_range_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_copy_buffer_range_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(256) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":20480,"
      "\"resource_desc\":" + buffer_desc_json(256) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::CopyBufferRegion",
      {15, 5, 6},
      "{\"dst_buffer_object_id\":5,\"dst_offset\":128,"
      "\"src_buffer_object_id\":6,\"src_offset\":64,\"byte_count\":512}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_execute_bundle_copy_buffer_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_execute_bundle_copy_buffer";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(256) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":20480,"
      "\"resource_desc\":" + buffer_desc_json(256) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 16, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 16}, "{\"type\":1}"));
  append(writer, object_create(sequence++, 17, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 16, 0, 17},
      "{\"node_mask\":0,\"type\":1,\"command_allocator_object_id\":16,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::CopyBufferRegion",
      {17, 5, 6},
      "{\"dst_buffer_object_id\":5,\"dst_offset\":128,"
      "\"src_buffer_object_id\":6,\"src_offset\":64,\"byte_count\":32}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {17}, "{}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::ExecuteBundle", {15, 17}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_copy_texture_source_box_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_copy_texture_source_box_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      {15, 5, 6},
      "{\"dst\":{\"resource_object_id\":5,\"type\":0,\"subresource_index\":0},"
      "\"dst_x\":0,\"dst_y\":0,\"dst_z\":0,"
      "\"src\":{\"resource_object_id\":6,\"type\":0,\"subresource_index\":0},"
      "\"src_box\":{\"left\":0,\"top\":0,\"front\":0,\"right\":128,\"bottom\":64,\"back\":1}}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_set_pipeline_state_missing_ref_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_set_pipeline_state_missing_ref";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetPipelineState",
      {15},
      "{}"));
  writer.close();
  return true;
}

bool write_viewport_first_cannot_cover_multiple_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_viewport_first_cannot_cover_multiple";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::RSSetViewports",
      {15},
      "{\"viewport_count\":2,\"first\":{\"x\":0,\"y\":0,\"width\":128,"
      "\"height\":64,\"min_depth\":0,\"max_depth\":1}}"));
  writer.close();
  return true;
}

bool write_omset_render_targets_depth_only_bundle(const std::filesystem::path &bundle, bool include_dsv_key)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = include_dsv_key ? "test_d3d12_omset_depth_only"
                                      : "test_d3d12_omset_missing_dsv";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      include_dsv_key
          ? "{\"render_target_count\":0,\"single_handle_to_descriptor_range\":false,\"dsv\":0}"
          : "{\"render_target_count\":0,\"single_handle_to_descriptor_range\":false}"));
  writer.close();
  return true;
}

bool write_copy_texture_full_copy_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_copy_texture_full_copy_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      {15, 5, 6},
      "{\"dst\":{\"resource_object_id\":5,\"type\":0,\"subresource_index\":0},"
      "\"dst_x\":1,\"dst_y\":0,\"dst_z\":0,"
      "\"src\":{\"resource_object_id\":6,\"type\":0,\"subresource_index\":0},"
      "\"has_src_box\":false}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_copy_texture_missing_location_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_copy_texture_missing_location";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      {15, 5, 6},
      "{\"dst\":{\"resource_object_id\":5,\"type\":0,\"subresource_index\":0},"
      "\"dst_x\":0,\"dst_y\":0,\"dst_z\":0}"));
  writer.close();
  return true;
}

bool write_resource_barrier_subresource_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_resource_barrier_subresource_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Texture"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ResourceBarrier",
      {15, 5},
      "{\"barrier_count\":1,\"barriers\":["
      "{\"type\":0,\"flags\":0,\"resource_object_id\":5,"
      "\"before\":0,\"after\":64,\"subresource\":4}]}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_discard_resource_rect_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_discard_resource_rect_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Texture"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DiscardResource",
      {15, 5},
      "{\"first_subresource\":0,\"subresource_count\":1,\"rect_count\":1,"
      "\"rects\":[{\"left\":0,\"top\":0,\"right\":128,\"bottom\":64}]}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_resolve_subresource_out_of_bounds_bundle(const std::filesystem::path &bundle, bool region)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = region ? "test_d3d12_resolve_region_rect_out_of_bounds"
                             : "test_d3d12_resolve_subresource_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  if (region) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::ResolveSubresourceRegion",
        {15, 5, 6},
        "{\"dst_resource_object_id\":5,\"dst_subresource\":0,\"dst_x\":0,\"dst_y\":0,"
        "\"src_resource_object_id\":6,\"src_subresource\":0,"
        "\"src_rect\":{\"left\":0,\"top\":0,\"right\":128,\"bottom\":64},"
        "\"format\":28,\"mode\":3}"));
  } else {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::ResolveSubresource",
        {15, 5, 6},
        "{\"dst_resource_object_id\":5,\"dst_subresource\":4,"
        "\"src_resource_object_id\":6,\"src_subresource\":0,\"format\":28}"));
  }
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_ia_buffer_range_out_of_bounds_bundle(const std::filesystem::path &bundle, bool indexed)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = indexed ? "test_d3d12_index_buffer_range_out_of_bounds"
                              : "test_d3d12_vertex_buffer_range_out_of_bounds";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Buffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(256) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 8, ObjectKind::DescriptorHeap, 1, "RTV"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 6},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      "{\"render_target_count\":1,\"single_handle_to_descriptor_range\":false,"
      "\"render_targets\":[12288],\"dsv\":0}"));
  if (indexed) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::IASetIndexBuffer",
        {15, 5},
        "{\"buffer_location\":16512,"
        "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":5,"
        "\"resolved_resource_offset\":128,\"resolved_resource_width\":256,"
        "\"size_in_bytes\":256,\"format\":57}"));
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::DrawIndexedInstanced",
        {15},
        "{\"index_count_per_instance\":3,\"instance_count\":1,"
        "\"start_index_location\":0,\"base_vertex_location\":0,"
        "\"start_instance_location\":0}"));
  } else {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::IASetVertexBuffers",
        {15, 5},
        "{\"start_slot\":0,\"view_count\":1,\"views\":[{\"buffer_location\":16512,"
        "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":5,"
        "\"resolved_resource_offset\":128,\"resolved_resource_width\":256,"
        "\"size_in_bytes\":256,\"stride_in_bytes\":16}]}"));
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::DrawInstanced",
        {15},
        "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
        "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  }
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_copy_resource_bundle(const std::filesystem::path &bundle, bool mismatched_desc)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = mismatched_desc
                          ? "test_d3d12_copy_resource_mismatched_desc"
                          : "test_d3d12_copy_resource";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Dst"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::Resource, 1, "Src"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(mismatched_desc ? 32 : 64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::CopyResource", {15, 5, 6}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_without_pipeline_assets_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_without_pipeline_assets";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandQueue",
      {1, 13},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12CommandQueue::ExecuteCommandLists",
      {13, 15},
      "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_root_signature_wrong_blob_ref_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_root_signature_wrong_blob_ref";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord wrong_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::Buffer, "wrong", "wrong!!", wrong_asset)) {
    return false;
  }
  if (!write_asset_index_file(bundle, {root_signature_asset, wrong_asset})) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, object_create(sequence++, 2, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 2},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {wrong_asset.blob_id}));

  writer.close();
  return true;
}

bool write_pipeline_unknown_root_signature_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_pipeline_unknown_root_signature";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, object_create(sequence++, 2, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  apitrace::trace::AssetRecord root_signature_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset)) {
    return false;
  }
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 2},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));

  append(writer, object_create(sequence++, 3, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  apitrace::trace::AssetRecord shader_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "cs", "cs", shader_asset)) {
    return false;
  }
  apitrace::trace::AssetRecord pipeline_asset;
  const std::string pipeline_json =
      "{\"type\":\"compute\",\"root_signature_object_id\":999,\"node_mask\":0,\"flags\":0,"
      "\"cs\":{\"bytecode_size\":2,\"cs_path\":\"" +
      shader_asset.relative_path.generic_string() + "\"}}";
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "compute-pipeline",
          pipeline_json,
          pipeline_asset)) {
    return false;
  }
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateComputePipelineState",
      {1, 3},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, shader_asset.blob_id}));

  writer.close();
  return true;
}

std::string native_ready_graphics_pipeline_json(
    ObjectId root_signature_object_id,
    const apitrace::trace::AssetRecord &vertex_shader_asset)
{
  return std::string("{\"type\":\"graphics\",\"root_signature_object_id\":") +
      std::to_string(root_signature_object_id) +
      ",\"node_mask\":0,\"flags\":0,\"input_layout\":{\"element_count\":0,\"elements\":[]},"
      "\"blend_state\":{\"alpha_to_coverage_enable\":false,\"independent_blend_enable\":false,"
      "\"render_targets\":[]},\"sample_mask\":4294967295,"
      "\"rasterizer_state\":{\"fill_mode\":3,\"cull_mode\":1,\"front_counter_clockwise\":false,"
      "\"depth_bias\":0,\"depth_bias_clamp\":0.0,\"slope_scaled_depth_bias\":0.0,"
      "\"depth_clip_enable\":true,\"multisample_enable\":false,\"antialiased_line_enable\":false,"
      "\"forced_sample_count\":0,\"conservative_raster\":0},"
      "\"depth_stencil_state\":{\"depth_enable\":false,\"depth_write_mask\":0,\"depth_func\":8,"
      "\"stencil_enable\":false,\"stencil_read_mask\":255,\"stencil_write_mask\":255,"
      "\"front_face\":{\"stencil_fail_op\":1,\"stencil_depth_fail_op\":1,\"stencil_pass_op\":1,\"stencil_func\":8},"
      "\"back_face\":{\"stencil_fail_op\":1,\"stencil_depth_fail_op\":1,\"stencil_pass_op\":1,\"stencil_func\":8}},"
      "\"primitive_topology_type\":3,\"num_render_targets\":1,\"rtv_formats\":[28],"
      "\"dsv_format\":0,\"sample_desc\":{\"count\":1,\"quality\":0},"
      "\"ib_strip_cut_value\":0,"
      "\"vs\":{\"bytecode_size\":2,\"vs_path\":\"" +
      vertex_shader_asset.relative_path.generic_string() + "\"}}";
}

std::string native_ready_compute_pipeline_json(
    ObjectId root_signature_object_id,
    const apitrace::trace::AssetRecord &compute_shader_asset)
{
  return std::string("{\"type\":\"compute\",\"root_signature_object_id\":") +
      std::to_string(root_signature_object_id) +
      ",\"node_mask\":0,\"flags\":0,"
      "\"cs\":{\"bytecode_size\":2,\"cs_path\":\"" +
      compute_shader_asset.relative_path.generic_string() + "\"}}";
}

bool write_graphics_pipeline_missing_metadata_bundle(
    const std::filesystem::path &bundle,
    const std::string &field_marker)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_graphics_pipeline_missing_metadata";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, object_create(sequence++, 2, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  apitrace::trace::AssetRecord root_signature_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset)) {
    return false;
  }
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 2},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));

  apitrace::trace::AssetRecord vertex_shader_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset)) {
    return false;
  }
  std::string pipeline_json = native_ready_graphics_pipeline_json(2, vertex_shader_asset);
  if (field_marker == "sample_desc") {
    const std::string sample_desc = "\"dsv_format\":0,\"sample_desc\":{\"count\":1,\"quality\":0},";
    const auto position = pipeline_json.find(sample_desc);
    if (position == std::string::npos) {
      return false;
    }
    pipeline_json.erase(position + std::string("\"dsv_format\":0,").size(),
                        std::string("\"sample_desc\":{\"count\":1,\"quality\":0},").size());
  } else if (field_marker == "rtv_formats") {
    const std::string rtv_formats = "\"primitive_topology_type\":3,\"num_render_targets\":1,\"rtv_formats\":[28],";
    const auto position = pipeline_json.find(rtv_formats);
    if (position == std::string::npos) {
      return false;
    }
    pipeline_json.erase(position + std::string("\"primitive_topology_type\":3,\"num_render_targets\":1,").size(),
                        std::string("\"rtv_formats\":[28],").size());
  } else if (field_marker == "depth_enable") {
    const std::string depth_enable = "\"depth_enable\":false,";
    const auto position = pipeline_json.find(depth_enable);
    if (position == std::string::npos) {
      return false;
    }
    pipeline_json.erase(position, depth_enable.size());
  } else {
    return false;
  }

  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          pipeline_json,
          pipeline_asset)) {
    return false;
  }
  append(writer, object_create(sequence++, 3, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 3},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));

  writer.close();
  return true;
}

bool write_draw_without_bound_pipeline_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_without_bound_pipeline";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_without_bound_root_signature_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_without_bound_root_signature";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_draw_with_mismatched_root_parameter_type_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_draw_with_mismatched_root_parameter_type";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
      "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() +
          "\",\"root_parameters\":[{\"root_parameter_index\":0,\"parameter_type\":3,"
          "\"shader_visibility\":0,\"shader_register\":0,\"register_space\":0}]}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
      {15, 4},
      "{\"root_parameter_index\":0,\"buffer_location\":8192,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":4,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::DrawInstanced",
      {15},
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,"
      "\"start_vertex_location\":0,\"start_instance_location\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_dispatch_without_bound_pipeline_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_dispatch_without_bound_pipeline";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord compute_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "cs", "cs", compute_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "compute-pipeline",
          native_ready_compute_pipeline_json(10, compute_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateComputePipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, compute_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetComputeRootSignature", {15, 10}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::Dispatch",
      {15},
      "{\"thread_group_count_x\":1,\"thread_group_count_y\":1,\"thread_group_count_z\":1}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_execute_indirect_without_bound_pipeline_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_execute_indirect_without_bound_pipeline";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ArgumentBuffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(1024) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 12, ObjectKind::CommandSignature, 1, "ID3D12CommandSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandSignature",
      {1, 10, 12},
      "{\"byte_stride\":20,\"argument_count\":1,\"node_mask\":0,"
      "\"arguments\":[{\"type\":1}]}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ExecuteIndirect",
      {15, 12, 4, 0},
      "{\"max_command_count\":1,\"arg_buffer_offset\":0,\"count_buffer_offset\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_execute_indirect_argument_buffer_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_execute_indirect_argument_buffer_out_of_bounds";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ArgumentBuffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(16) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 12, ObjectKind::CommandSignature, 1, "ID3D12CommandSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandSignature",
      {1, 10, 12},
      "{\"byte_stride\":20,\"argument_count\":1,\"node_mask\":0,"
      "\"arguments\":[{\"type\":1}]}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ExecuteIndirect",
      {15, 12, 4, 0},
      "{\"max_command_count\":1,\"arg_buffer_offset\":0,\"count_buffer_offset\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_execute_indirect_missing_raster_state_bundle(
    const std::filesystem::path &bundle,
    bool include_viewport,
    bool include_scissor,
    bool include_topology)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_execute_indirect_missing_raster_state";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord root_signature_asset;
  apitrace::trace::AssetRecord vertex_shader_asset;
  apitrace::trace::AssetRecord pipeline_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::RootSignature, "rootsig", "rootsig", root_signature_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::ShaderDxil, "vs", "vs", vertex_shader_asset) ||
      !write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          pipeline_asset)) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ArgumentBuffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(1024) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 8, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 5},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));
  append(writer, object_create(sequence++, 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + pipeline_asset.relative_path.generic_string() + "\"}",
      {pipeline_asset.blob_id, vertex_shader_asset.blob_id}));
  append(writer, object_create(sequence++, 12, ObjectKind::CommandSignature, 1, "ID3D12CommandSignature"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandSignature",
      {1, 10, 12},
      "{\"byte_stride\":20,\"argument_count\":1,\"node_mask\":0,"
      "\"arguments\":[{\"type\":1}]}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetGraphicsRootSignature", {15, 10}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::SetPipelineState", {15, 11}, "{}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      "{\"render_target_count\":1,\"single_handle_to_descriptor_range\":false,"
      "\"render_targets\":[12288],\"dsv\":0}"));
  if (include_viewport) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::RSSetViewports",
        {15},
        "{\"viewport_count\":1,\"viewports\":[{\"x\":0,\"y\":0,\"width\":64,"
        "\"height\":64,\"min_depth\":0,\"max_depth\":1}]}"));
  }
  if (include_scissor) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::RSSetScissorRects",
        {15},
        "{\"rect_count\":1,\"rects\":[{\"left\":0,\"top\":0,\"right\":64,\"bottom\":64}]}"));
  }
  if (include_topology) {
    append(writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::IASetPrimitiveTopology",
        {15},
        "{\"primitive_topology\":4}"));
  }
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ExecuteIndirect",
      {15, 12, 4, 0},
      "{\"max_command_count\":1,\"arg_buffer_offset\":0,\"count_buffer_offset\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_clear_rtv_with_srv_descriptor_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_clear_rtv_with_srv_descriptor";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":2,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":28,\"view_dimension\":4,"
      "\"shader_4_component_mapping\":5768,\"view\":{\"most_detailed_mip\":0,"
      "\"mip_levels\":1,\"plane_slice\":0,\"resource_min_lod_clamp\":0}}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ClearRenderTargetView",
      {15},
      "{\"descriptor\":8192,\"rect_count\":0,\"color\":[0,0,0,1]}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_buffer_srv_range_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_buffer_srv_range_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Buffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
      "\"resource_desc\":" + buffer_desc_json(64) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":0,\"view_dimension\":1,"
      "\"shader_4_component_mapping\":5768,"
      "\"view\":{\"first_element\":4,\"num_elements\":8,\"structure_byte_stride\":16,\"flags\":0}}"));

  writer.close();
  return true;
}

bool write_typed_buffer_srv_range_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_typed_buffer_srv_range_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Buffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
      "\"resource_desc\":" + buffer_desc_json(64) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":2,\"view_dimension\":1,"
      "\"shader_4_component_mapping\":5768,"
      "\"view\":{\"first_element\":3,\"num_elements\":2,\"structure_byte_stride\":0,\"flags\":0}}"));

  writer.close();
  return true;
}

bool write_texture_srv_mip_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_texture_srv_mip_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Texture"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":1,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateShaderResourceView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":28,\"view_dimension\":4,"
      "\"shader_4_component_mapping\":5768,"
      "\"view\":{\"most_detailed_mip\":4,\"mip_levels\":1,\"plane_slice\":0,"
      "\"resource_min_lod_clamp\":0}}"));

  writer.close();
  return true;
}

bool write_clear_rtv_rect_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_clear_rtv_rect_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "Color"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 4},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture2d_desc_json(64, 64, 1, 28) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":2,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateRenderTargetView",
      {1, 4},
      "{\"descriptor\":8192,\"format\":28,\"view_dimension\":4,"
      "\"view\":{\"mip_slice\":0,\"plane_slice\":0}}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ClearRenderTargetView",
      {15},
      "{\"descriptor\":8192,\"rect_count\":1,\"color\":[0,0,0,1],"
      "\"rects\":[{\"left\":0,\"top\":0,\"right\":128,\"bottom\":64}]}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_query_range_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_query_range_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 18, ObjectKind::QueryHeap, 1, "ID3D12QueryHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateQueryHeap",
      {1, 18},
      "{\"type\":0,\"count\":1,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::BeginQuery",
      {15, 18},
      "{\"query_heap_object_id\":18,\"type\":0,\"index\":1}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_clear_state_unknown_pipeline_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_clear_state_unknown_pipeline";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, object_create(sequence++, 99, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::ClearState", {15, 99}, "{}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_predication_range_out_of_bounds_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_predication_range_out_of_bounds";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "PredicateBuffer"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(8) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::SetPredication",
      {15, 5},
      "{\"buffer_object_id\":5,\"aligned_buffer_offset\":8,\"operation\":1}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));

  writer.close();
  return true;
}

bool write_fence_signal_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_fence_signal";
  writer.write_metadata(metadata);

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandQueue", {1, 13}, "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(sequence++, "ID3D12Device::CreateCommandAllocator", {1, 14}, "{\"type\":0}"));
  append(writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommandList",
      {1, 14, 0, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":0}"));
  append(writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::ExecuteCommandLists", {13, 15}, "{\"command_list_count\":1}"));
  append(writer, object_create(sequence++, 20, ObjectKind::Fence, 1, "ID3D12Fence"));
  append(writer, call(sequence++, "ID3D12Device::CreateFence", {1, 20}, "{\"initial_value\":0,\"flags\":0}"));
  append(writer, call(sequence++, "ID3D12CommandQueue::Signal", {13, 20}, "{\"fence_value\":1}"));

  writer.close();
  return true;
}

bool write_unmap_wrong_blob_ref_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_unmap_wrong_blob_ref";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord buffer_asset;
  apitrace::trace::AssetRecord wrong_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::Buffer, "mapped-buffer", "data", buffer_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::Buffer, "wrong", "bad!", wrong_asset)) {
    return false;
  }
  if (!write_asset_index_file(bundle, {buffer_asset, wrong_asset})) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 2, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 2},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":4096,"
      "\"resource_desc\":" + buffer_desc_json(16) + ",\"optimized_clear_value\":null}"));
  append(writer, call(
      sequence++,
      "ID3D12Resource::Map",
      {2},
      "{\"subresource\":0,\"mapped\":true}"));
  append(writer, call(
      sequence++,
      "ID3D12Resource::Unmap",
      {2},
      "{\"subresource\":0,\"written_begin\":0,\"written_end\":4,\"buffer_path\":\"" +
          buffer_asset.relative_path.generic_string() + "\"}",
      {wrong_asset.blob_id}));

  writer.close();
  return true;
}

bool write_resolve_query_data_result_wrong_blob_ref_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_resolve_query_data_result_wrong_blob_ref";
  writer.write_metadata(metadata);

  apitrace::trace::AssetRecord query_result_asset;
  apitrace::trace::AssetRecord wrong_asset;
  if (!write_asset(writer, apitrace::trace::AssetKind::Buffer, "query-result", "queryres", query_result_asset) ||
      !write_asset(writer, apitrace::trace::AssetKind::Buffer, "wrong", "wrongqry", wrong_asset)) {
    return false;
  }
  if (!write_asset_index_file(bundle, {query_result_asset, wrong_asset})) {
    return false;
  }

  std::uint64_t sequence = 1;
  append(writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
  append(writer, object_create(sequence++, 2, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateCommittedResource",
      {1, 2},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":4096,"
      "\"resource_desc\":" + buffer_desc_json(64) + ",\"optimized_clear_value\":null}"));
  append(writer, object_create(sequence++, 3, ObjectKind::QueryHeap, 1, "ID3D12QueryHeap"));
  append(writer, call(
      sequence++,
      "ID3D12Device::CreateQueryHeap",
      {1, 3},
      "{\"type\":0,\"count\":1,\"node_mask\":0}"));
  append(writer, object_create(sequence++, 4, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      sequence++,
      "ID3D12GraphicsCommandList::ResolveQueryDataResult",
      {4, 3, 2},
      "{\"query_heap_object_id\":3,\"type\":0,\"start_index\":0,"
      "\"query_count\":1,\"dst_buffer_object_id\":2,"
      "\"aligned_dst_buffer_offset\":16,\"resolved_size\":8,"
      "\"buffer_path\":\"" + query_result_asset.relative_path.generic_string() + "\"}",
      {wrong_asset.blob_id}));

  writer.close();
  return true;
}

} // namespace

#include "d3d12_replay_semantics/main.inc"
