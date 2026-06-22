#include "apitrace/capture_runtime.hpp"
#include "apitrace/d3d12_capture.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <vector>

#include <d3d12.h>

#ifdef _WIN32
#include <stdlib.h>
#endif

enum class PipelineStateSubobjectType : std::uint32_t {
  RootSignature = 0x0,
  CS = 0x6,
  AS = 0x18,
  MS = 0x19,
};

template <typename T, PipelineStateSubobjectType TypeValue>
struct StreamSubobject {
  std::uint32_t type = static_cast<std::uint32_t>(TypeValue);
  T data;
};

namespace {

bool set_trace_bundle_env(const std::filesystem::path &bundle)
{
#ifdef _WIN32
  return _putenv_s("APITRACE_TRACE_BUNDLE", bundle.string().c_str()) == 0;
#else
  return setenv("APITRACE_TRACE_BUNDLE", bundle.string().c_str(), 1) == 0;
#endif
}

bool clear_trace_bundle_env()
{
#ifdef _WIN32
  return _putenv_s("APITRACE_TRACE_BUNDLE", "") == 0;
#else
  return unsetenv("APITRACE_TRACE_BUNDLE") == 0;
#endif
}

bool has_call(const apitrace::trace::TraceBundleReader &reader, std::string_view function_name)
{
  for (const auto &event : reader.events()) {
    if (event.kind == apitrace::trace::EventKind::Call &&
        event.callsite.function_name == function_name) {
      return true;
    }
  }
  return false;
}

const apitrace::trace::EventRecord *find_call(
    const apitrace::trace::TraceBundleReader &reader,
    std::string_view function_name)
{
  for (const auto &event : reader.events()) {
    if (event.kind == apitrace::trace::EventKind::Call &&
        event.callsite.function_name == function_name) {
      return &event;
    }
  }
  return nullptr;
}

void *fake_object(std::uintptr_t address)
{
  return reinterpret_cast<void *>(address);
}

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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

bool finalize_bundle(const char *argv0, const std::filesystem::path &bundle)
{
  std::filesystem::path finalize = "bundle-finalize";
  const auto self = std::filesystem::path(argv0 ? argv0 : "");
  if (self.has_parent_path()) {
    const auto sibling = self.parent_path() / "bundle-finalize";
    if (std::filesystem::exists(sibling)) {
      finalize = sibling;
    }
  }
  const auto command = shell_quote_path(finalize) + " --jobs 1 " + shell_quote_path(bundle);
  return std::system(command.c_str()) == 0;
}

bool verify_raw_pipeline_capture(const std::filesystem::path &bundle)
{
  std::ifstream input(bundle / apitrace::trace::kCallstreamFileName, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "raw callstream was not written\n";
    return false;
  }
  std::string line;
  std::size_t stream_pipeline_calls = 0;
  bool found_compute_stream_pipeline = false;
  bool found_mesh_stream_pipeline = false;
  while (std::getline(input, line)) {
    const auto record = nlohmann::json::parse(line, nullptr, false);
    if (record.is_discarded()) {
      continue;
    }
    if (record.value("function", std::string()) != "ID3D12Device2::CreatePipelineState") {
      continue;
    }
    ++stream_pipeline_calls;
    const auto payload = record.value("payload", nlohmann::json::object());
    const auto blob_refs = record.value("blob_refs", nlohmann::json::array());
    if (!payload.is_object() ||
        payload.value("pso_raw_version", 0) != 1 ||
        payload.value("source", "") != "stream" ||
        payload.contains("pipeline_path") ||
        !payload.contains("stream_metadata") ||
        blob_refs.size() != 1) {
      std::cerr << "raw CreatePipelineState stream payload is incomplete\n";
      return false;
    }
    const auto blob_id = blob_refs.front().get<std::uint64_t>();
    if (payload.value("requires_dxmt_backend", false)) {
      found_mesh_stream_pipeline =
          payload.contains("ms") && !payload["ms"].is_null() &&
          payload["ms"].value("blob_id", 0ull) == blob_id;
    } else {
      found_compute_stream_pipeline =
          payload.contains("cs") && !payload["cs"].is_null() &&
          payload["cs"].value("blob_id", 0ull) == blob_id;
    }
  }
  if (stream_pipeline_calls != 2 || !found_compute_stream_pipeline || !found_mesh_stream_pipeline) {
    std::cerr << "raw CreatePipelineState calls did not preserve compute and mesh variants\n";
    return false;
  }
  return true;
}

bool verify_mapped_root_cbv_capture(const std::filesystem::path &bundle)
{
  std::ifstream input(bundle / apitrace::trace::kCallstreamFileName, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "raw callstream was not written\n";
    return false;
  }

  bool found_draw = false;
  bool seen_root_cbv = false;
  bool found_use_snapshot = false;
  std::string line;
  while (std::getline(input, line)) {
    const auto record = nlohmann::json::parse(line, nullptr, false);
    if (record.is_discarded() || record.value("record_kind", std::string()) != "call") {
      continue;
    }
    const auto function = record.value("function", std::string());
    if (function == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView") {
      seen_root_cbv = true;
    }
    if (function == "ID3D12GraphicsCommandList::DrawInstanced") {
      found_draw = true;
    }
    if (function != "ID3D12Resource::Unmap") {
      continue;
    }
    const auto payload = record.value("payload", nlohmann::json::object());
    if (!payload.is_object() ||
        payload.value("capture_reason", std::string()) != "mapped_resource_use") {
      continue;
    }
    found_use_snapshot = seen_root_cbv && !found_draw &&
        payload.value("written_begin", UINT64_MAX) <= 0x200 &&
        payload.value("written_end", 0ull) >= 0x1000 &&
        payload.value("written_size", 0ull) >= 0x1000 &&
        !record.value("blob_refs", nlohmann::json::array()).empty();
  }

  if (!found_draw || !found_use_snapshot) {
    std::cerr << "mapped root CBV use was not captured before DrawInstanced\n";
    return false;
  }
  return true;
}

bool verify_mapped_descriptor_cbv_capture(const std::filesystem::path &bundle)
{
  std::ifstream input(bundle / apitrace::trace::kCallstreamFileName, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "raw callstream was not written\n";
    return false;
  }

  bool found_use_snapshot = false;
  bool found_copy_use_snapshot = false;
  bool found_descriptor_cbv = false;
  bool found_descriptor_cbv_copy = false;
  std::string line;
  while (std::getline(input, line)) {
    const auto record = nlohmann::json::parse(line, nullptr, false);
    if (record.is_discarded() || record.value("record_kind", std::string()) != "call") {
      continue;
    }
    const auto function = record.value("function", std::string());
    if (function == "ID3D12Resource::Unmap") {
      const auto payload = record.value("payload", nlohmann::json::object());
      if (payload.is_object() &&
          payload.value("capture_reason", std::string()) == "mapped_resource_use" &&
          payload.value("written_begin", UINT64_MAX) == 0x100 &&
          payload.value("written_end", 0ull) == 0x200 &&
          payload.value("written_size", 0ull) == 0x100 &&
          !record.value("blob_refs", nlohmann::json::array()).empty()) {
        if (!found_descriptor_cbv) {
          found_use_snapshot = true;
        } else if (!found_descriptor_cbv_copy) {
          found_copy_use_snapshot = true;
        }
      }
      continue;
    }
    if (function == "ID3D12Device::CopyDescriptorsBatch") {
      const auto payload = record.value("payload", nlohmann::json::object());
      if (!payload.is_object() || !payload.contains("ops")) {
        continue;
      }
      for (const auto &op : payload["ops"]) {
        if (!op.is_array() || op.size() < 6 || !op[5].is_array()) {
          continue;
        }
        for (const auto &range : op[5]) {
          if (!range.is_array() || range.size() < 3) {
            continue;
          }
          if (range[0].get<std::uint64_t>() == 0x7060 &&
              range[1].get<std::uint64_t>() == 0x7040 &&
              range[2].get<std::uint32_t>() == 1) {
            found_descriptor_cbv_copy = true;
            if (!found_copy_use_snapshot) {
              std::cerr << "mapped descriptor CBV copy was not captured before CopyDescriptors\n";
              return false;
            }
          }
        }
      }
      continue;
    }
    if (function != "ID3D12Device::CreateDescriptorViewBatch") {
      continue;
    }
    const auto payload = record.value("payload", nlohmann::json::object());
    if (!payload.is_object() || !payload.contains("ops")) {
      continue;
    }
    for (const auto &op : payload["ops"]) {
      if (!op.is_array() || op.size() < 3) {
        continue;
      }
      if (op[1].get<int>() == 1 && op[2].get<std::uint64_t>() == 0x7040) {
        found_descriptor_cbv = true;
        if (!found_use_snapshot) {
          std::cerr << "mapped descriptor CBV use was not captured before descriptor creation\n";
          return false;
        }
      }
    }
  }

  if (!found_descriptor_cbv || !found_use_snapshot || !found_descriptor_cbv_copy || !found_copy_use_snapshot) {
    std::cerr << "mapped descriptor CBV use was not captured\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    std::cerr << "usage: test_d3d12_capture_api <bundle>\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  apitrace::runtime::shutdown_process_trace_session();
  std::filesystem::remove_all(bundle);

  if (!set_trace_bundle_env(bundle)) {
    std::cerr << "failed to set APITRACE_TRACE_BUNDLE\n";
    return 1;
  }

  auto *factory = fake_object(0x1000);
  auto *device = fake_object(0x2000);
  auto *swapchain = fake_object(0x3000);
  auto *second_swapchain = fake_object(0x4000);
  auto *descriptor_heap = fake_object(0x4500);
  auto *command_list = fake_object(0x7000);
  auto *copy_src = fake_object(0x7600);
  auto *copy_dst = fake_object(0x7800);

  const auto device_sequence = apitrace::d3d12::record_d3d12_create_device(device);
  const auto swapchain_sequence =
      apitrace::d3d12::record_dxgi_create_swapchain(factory, device, swapchain);
  const auto second_swapchain_sequence =
      apitrace::d3d12::record_dxgi_create_swapchain(factory, device, second_swapchain);
  const auto test_present_frame =
      apitrace::d3d12::record_present(swapchain, 1, DXGI_PRESENT_TEST, 0, false);
  const auto first_present_frame = apitrace::d3d12::record_present(swapchain, 1, 0, 0, true);
  const auto second_present_frame = apitrace::d3d12::record_present(second_swapchain, 0, 2, 0, true);
  if (device_sequence == 0 ||
      swapchain_sequence == 0 ||
      second_swapchain_sequence == 0 ||
      test_present_frame != UINT64_MAX ||
      first_present_frame != 0 ||
      second_present_frame != 1) {
    std::cerr << "failed to record d3d12 capture api calls\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
  descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  descriptor_heap_desc.NumDescriptors = 8;
  descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (apitrace::d3d12::record_create_descriptor_heap(
          static_cast<ID3D12Device *>(device),
          &descriptor_heap_desc,
          descriptor_heap,
          32,
          0x4000,
          0x9000,
          S_OK) == 0) {
    std::cerr << "failed to record CreateDescriptorHeap capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  apitrace::d3d12::record_object_create(
      command_list,
      apitrace::d3d12::CaptureObjectKind::CommandList,
      device,
      "ID3D12GraphicsCommandList");
  D3D12_HEAP_PROPERTIES default_heap_properties = {};
  default_heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture_desc = {};
  texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture_desc.Width = 64;
  texture_desc.Height = 32;
  texture_desc.DepthOrArraySize = 1;
  texture_desc.MipLevels = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  if (apitrace::d3d12::record_create_committed_resource(
          static_cast<ID3D12Device *>(device),
          &default_heap_properties,
          D3D12_HEAP_FLAG_NONE,
          &texture_desc,
          D3D12_RESOURCE_STATE_COPY_SOURCE,
          nullptr,
          copy_src,
          0,
          S_OK) == 0 ||
      apitrace::d3d12::record_create_committed_resource(
          static_cast<ID3D12Device *>(device),
          &default_heap_properties,
          D3D12_HEAP_FLAG_NONE,
          &texture_desc,
          D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,
          copy_dst,
          0,
          S_OK) == 0) {
    std::cerr << "failed to record texture resources for batch commands\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }
  D3D12_RESOURCE_BARRIER barriers[2] = {};
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = static_cast<ID3D12Resource *>(copy_src);
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barriers[1].UAV.pResource = static_cast<ID3D12Resource *>(copy_dst);
  D3D12_TEXTURE_COPY_LOCATION copy_location_src = {};
  copy_location_src.pResource = static_cast<ID3D12Resource *>(copy_src);
  copy_location_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_location_src.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copy_location_dst = {};
  copy_location_dst.pResource = static_cast<ID3D12Resource *>(copy_dst);
  copy_location_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copy_location_dst.SubresourceIndex = 0;
  D3D12_BOX copy_box = {1, 2, 0, 16, 18, 1};
  apitrace::d3d12::record_resource_barrier(command_list, 2, barriers);
  apitrace::d3d12::record_copy_texture_region(
      command_list, &copy_location_dst, 4, 5, 0, &copy_location_src, &copy_box);

  D3D12_DESCRIPTOR_RANGE root_descriptor_range = {};
  root_descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  root_descriptor_range.NumDescriptors = 2;
  root_descriptor_range.BaseShaderRegister = 3;
  root_descriptor_range.RegisterSpace = 1;
  root_descriptor_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  D3D12_ROOT_PARAMETER root_parameter = {};
  root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameter.DescriptorTable.NumDescriptorRanges = 1;
  root_parameter.DescriptorTable.pDescriptorRanges = &root_descriptor_range;
  root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_PARAMETER root_cbv_parameter = {};
  root_cbv_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  root_cbv_parameter.Descriptor.ShaderRegister = 4;
  root_cbv_parameter.Descriptor.RegisterSpace = 2;
  root_cbv_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_ROOT_PARAMETER root_constants_parameter = {};
  root_constants_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_constants_parameter.Constants.ShaderRegister = 5;
  root_constants_parameter.Constants.RegisterSpace = 3;
  root_constants_parameter.Constants.Num32BitValues = 6;
  root_constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_PARAMETER root_parameters[] = {root_parameter, root_cbv_parameter, root_constants_parameter};
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
  root_signature_desc.NumParameters = 3;
  root_signature_desc.pParameters = root_parameters;
  const std::uint8_t root_signature_bytes[] = {0x72, 0x73, 0x69, 0x67};
  auto *captured_root_signature = fake_object(0x4700);
  if (apitrace::d3d12::record_create_root_signature(
          static_cast<ID3D12Device *>(device),
          0,
          root_signature_bytes,
          sizeof(root_signature_bytes),
          captured_root_signature,
          S_OK,
          &root_signature_desc) == 0) {
    std::cerr << "failed to record CreateRootSignature capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = {{0x4000}};
  D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = {{0x5000}, {0x6000}};
  const std::uint32_t dst_range_sizes[] = {2};
  if (apitrace::d3d12::record_copy_descriptors(
          static_cast<ID3D12Device *>(device),
          1,
          dst_ranges,
          dst_range_sizes,
          2,
          src_ranges,
          nullptr,
          0,
          32) == 0) {
    std::cerr << "failed to record CopyDescriptors capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  const auto *descriptor_resource = fake_object(0x6500);
  apitrace::d3d12::record_object_create(
      descriptor_resource,
      apitrace::d3d12::CaptureObjectKind::Resource,
      device,
      "ID3D12Resource");
  const auto *constant_buffer_resource = fake_object(0x6600);
  D3D12_HEAP_PROPERTIES upload_heap_properties = {};
  upload_heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC constant_buffer_desc = {};
  constant_buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  constant_buffer_desc.Width = 4096;
  constant_buffer_desc.Height = 1;
  constant_buffer_desc.DepthOrArraySize = 1;
  constant_buffer_desc.MipLevels = 1;
  constant_buffer_desc.SampleDesc.Count = 1;
  constant_buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (apitrace::d3d12::record_create_committed_resource(
          static_cast<ID3D12Device *>(device),
          &upload_heap_properties,
          D3D12_HEAP_FLAG_NONE,
          &constant_buffer_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr,
          constant_buffer_resource,
          0x100000,
          S_OK) == 0) {
    std::cerr << "failed to record constant buffer resource capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }
  std::uint8_t mapped_constant_data[4096] = {};
  if (apitrace::d3d12::record_resource_map(
          constant_buffer_resource,
          0,
          false,
          0,
          0,
          mapped_constant_data,
          true,
          S_OK) == 0) {
    std::cerr << "failed to record mapped constant buffer Map call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }
  for (std::size_t index = 0x300; index < 0x320; ++index) {
    mapped_constant_data[index] = static_cast<std::uint8_t>(index ^ 0xa5);
  }
  apitrace::d3d12::record_resource_unmap(
      constant_buffer_resource,
      0,
      0x300,
      0x320,
      mapped_constant_data + 0x300,
      0x20);
  for (std::size_t index = 0x100; index < 0x200; ++index) {
    mapped_constant_data[index] = static_cast<std::uint8_t>(index ^ 0x3c);
  }
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
  cbv_desc.BufferLocation = 0x100100;
  cbv_desc.SizeInBytes = 256;
  if (apitrace::d3d12::record_create_shader_resource_view(
          static_cast<ID3D12Device *>(device),
          descriptor_resource,
          nullptr,
          D3D12_CPU_DESCRIPTOR_HANDLE{0x7000}) == 0 ||
      apitrace::d3d12::record_create_unordered_access_view(
          static_cast<ID3D12Device *>(device),
          descriptor_resource,
          nullptr,
          nullptr,
          D3D12_CPU_DESCRIPTOR_HANDLE{0x7020}) == 0 ||
      apitrace::d3d12::record_create_constant_buffer_view(
          static_cast<ID3D12Device *>(device),
          &cbv_desc,
          D3D12_CPU_DESCRIPTOR_HANDLE{0x7040},
          constant_buffer_resource,
          0x100,
          4096) == 0) {
    std::cerr << "failed to record null-view descriptor capture api calls\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }
  for (std::size_t index = 0x100; index < 0x200; ++index) {
    mapped_constant_data[index] = static_cast<std::uint8_t>(index ^ 0xc3);
  }
  if (apitrace::d3d12::record_copy_descriptors_simple(
          static_cast<ID3D12Device *>(device),
          1,
          D3D12_CPU_DESCRIPTOR_HANDLE{0x7060},
          D3D12_CPU_DESCRIPTOR_HANDLE{0x7040},
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
          32) == 0) {
    std::cerr << "failed to record descriptor CBV copy capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }
  if (apitrace::d3d12::record_set_root_descriptor(
          command_list,
          false,
          D3D12_ROOT_PARAMETER_TYPE_CBV,
          1,
          0x100200) == 0) {
    std::cerr << "failed to record root CBV binding\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }
  for (std::size_t index = 0x200; index < 0x300; ++index) {
    mapped_constant_data[index] = static_cast<std::uint8_t>(index ^ 0x5a);
  }
  if (apitrace::d3d12::record_draw_instanced(command_list, 3, 1, 0, 0) == 0) {
    std::cerr << "failed to record DrawInstanced after mapped root CBV update\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  const auto *resolve_src = fake_object(0x8000);
  const auto *resolve_dst = fake_object(0x9000);
  const auto *query_heap = fake_object(0xc000);
  apitrace::d3d12::record_object_create(
      command_list,
      apitrace::d3d12::CaptureObjectKind::CommandList,
      device,
      "ID3D12GraphicsCommandList");
  apitrace::d3d12::record_object_create(
      resolve_src,
      apitrace::d3d12::CaptureObjectKind::Resource,
      device,
      "ID3D12Resource");
  apitrace::d3d12::record_object_create(
      resolve_dst,
      apitrace::d3d12::CaptureObjectKind::Resource,
      device,
      "ID3D12Resource");
  D3D12_QUERY_HEAP_DESC query_heap_desc = {};
  query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  query_heap_desc.Count = 4;
  query_heap_desc.NodeMask = 0;
  if (apitrace::d3d12::record_create_query_heap(
          static_cast<ID3D12Device *>(device),
          &query_heap_desc,
          query_heap,
          0) == 0 ||
      apitrace::d3d12::record_begin_query(command_list, query_heap, D3D12_QUERY_TYPE_OCCLUSION, 1) == 0 ||
      apitrace::d3d12::record_end_query(command_list, query_heap, D3D12_QUERY_TYPE_OCCLUSION, 1) == 0) {
    std::cerr << "failed to record query heap capture api calls\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  std::uint8_t shader_bytes[] = {0x44, 0x58, 0x49, 0x4c};
  std::uint8_t mesh_shader_bytes[] = {0x44, 0x58, 0x49, 0x4d};
  struct ComputePipelineStream {
    StreamSubobject<void *, PipelineStateSubobjectType::RootSignature> root_signature;
    StreamSubobject<D3D12_SHADER_BYTECODE, PipelineStateSubobjectType::CS> compute_shader;
  } compute_stream = {};
  compute_stream.root_signature.data = captured_root_signature;
  compute_stream.compute_shader.data = {shader_bytes, sizeof(shader_bytes)};
  if (apitrace::d3d12::record_create_pipeline_state(
          static_cast<ID3D12Device *>(device),
          &compute_stream,
          sizeof(compute_stream),
          fake_object(0xe000),
          0) == 0) {
    std::cerr << "failed to record stream CreatePipelineState capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  struct MeshPipelineStream {
    StreamSubobject<void *, PipelineStateSubobjectType::RootSignature> root_signature;
    StreamSubobject<D3D12_SHADER_BYTECODE, PipelineStateSubobjectType::MS> mesh_shader;
  } mesh_stream = {};
  mesh_stream.root_signature.data = compute_stream.root_signature.data;
  mesh_stream.mesh_shader.data = {mesh_shader_bytes, sizeof(mesh_shader_bytes)};
  if (apitrace::d3d12::record_create_pipeline_state(
          static_cast<ID3D12Device *>(device),
          &mesh_stream,
          sizeof(mesh_stream),
          fake_object(0xf000),
          0) == 0) {
    std::cerr << "failed to record mesh stream CreatePipelineState capture api call\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  apitrace::d3d12::RenderPassResolveSubresourceDesc resolve_subresources[1] = {};
  resolve_subresources[0].src_subresource = 2;
  resolve_subresources[0].dst_subresource = 3;
  resolve_subresources[0].dst_x = 4;
  resolve_subresources[0].dst_y = 5;
  resolve_subresources[0].has_src_rect = true;
  resolve_subresources[0].src_left = 6;
  resolve_subresources[0].src_top = 7;
  resolve_subresources[0].src_right = 8;
  resolve_subresources[0].src_bottom = 9;
  apitrace::d3d12::RenderPassRenderTargetDesc render_target = {};
  render_target.cpu_descriptor = 0xa000;
  render_target.beginning_access.type = 2;
  render_target.beginning_access.clear.format = 28;
  render_target.beginning_access.clear.color[0] = 1.0f;
  render_target.beginning_access.clear.color[1] = 0.5f;
  render_target.ending_access.type = 2;
  render_target.ending_access.src_resource = resolve_src;
  render_target.ending_access.dst_resource = resolve_dst;
  render_target.ending_access.subresource_count = 1;
  render_target.ending_access.subresources = resolve_subresources;
  render_target.ending_access.format = 28;
  render_target.ending_access.resolve_mode = 3;
  render_target.ending_access.preserve_resolve_source = true;
  apitrace::d3d12::RenderPassDepthStencilDesc depth_stencil = {};
  depth_stencil.cpu_descriptor = 0xb000;
  depth_stencil.depth_beginning_access.type = 2;
  depth_stencil.depth_beginning_access.clear.format = 45;
  depth_stencil.depth_beginning_access.clear.depth = 0.25f;
  depth_stencil.depth_beginning_access.clear.stencil = 12;
  if (apitrace::d3d12::record_begin_render_pass(
          command_list,
          1,
          &render_target,
          &depth_stencil,
          1) == 0 ||
      apitrace::d3d12::record_end_render_pass(command_list) == 0) {
    std::cerr << "failed to record render pass capture api calls\n";
    clear_trace_bundle_env();
    apitrace::runtime::shutdown_process_trace_session();
    return 1;
  }

  apitrace::runtime::shutdown_process_trace_session();
  clear_trace_bundle_env();

  if (!verify_raw_pipeline_capture(bundle)) {
    return 1;
  }
  if (!verify_mapped_descriptor_cbv_capture(bundle)) {
    return 1;
  }
  if (!verify_mapped_root_cbv_capture(bundle)) {
    return 1;
  }
  if (!finalize_bundle(argv[0], bundle)) {
    std::cerr << "bundle-finalize failed for capture api bundle\n";
    return 1;
  }

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    std::cerr << "failed to read bundle: " << reader.last_error() << "\n";
    return 1;
  }

  if (reader.metadata().api != apitrace::trace::ApiKind::D3D12) {
    std::cerr << "capture api did not route DXGI calls into the D3D12 bundle\n";
    return 1;
  }

  if (!has_call(reader, "D3D12CreateDevice") ||
      !has_call(reader, "IDXGIFactory::CreateSwapChain") ||
      !has_call(reader, "IDXGISwapChain::Present") ||
      !has_call(reader, "ID3D12Device::CreateDescriptorHeap") ||
      !has_call(reader, "ID3D12Device::CreateRootSignature") ||
      !has_call(reader, "ID3D12Device::CreateCommittedResource") ||
      !has_call(reader, "ID3D12Device::CopyDescriptorsBatch") ||
      !has_call(reader, "ID3D12Device::CreateDescriptorViewBatch") ||
      !has_call(reader, "ID3D12GraphicsCommandList::ResourceBarrierBatch") ||
      !has_call(reader, "ID3D12GraphicsCommandList::CopyTextureRegionBatch") ||
      !has_call(reader, "ID3D12Device::CreateQueryHeap") ||
      !has_call(reader, "ID3D12Device2::CreatePipelineState") ||
      !has_call(reader, "ID3D12GraphicsCommandList::BeginQuery") ||
      !has_call(reader, "ID3D12GraphicsCommandList::EndQuery") ||
      !has_call(reader, "ID3D12GraphicsCommandList4::BeginRenderPass") ||
      !has_call(reader, "ID3D12GraphicsCommandList4::EndRenderPass")) {
    std::cerr << "capture api bundle is missing expected D3D12/DXGI calls\n";
    return 1;
  }

  const auto *descriptor_heap_event = find_call(reader, "ID3D12Device::CreateDescriptorHeap");
  const auto descriptor_heap_payload = nlohmann::json::parse(descriptor_heap_event->payload, nullptr, false);
  if (descriptor_heap_payload.is_discarded() ||
      descriptor_heap_payload.value("type", UINT32_MAX) != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
      descriptor_heap_payload.value("num_descriptors", 0) != 8 ||
      descriptor_heap_payload.value("flags", UINT32_MAX) != D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE ||
      descriptor_heap_payload.value("descriptor_size", 0) != 32 ||
      descriptor_heap_payload.value("cpu_start", 0ull) != 0x4000 ||
      descriptor_heap_payload.value("gpu_start", 0ull) != 0x9000 ||
      descriptor_heap_event->object_refs.size() != 2) {
    std::cerr << "CreateDescriptorHeap did not preserve descriptor heap handle ranges\n";
    return 1;
  }

  const auto *root_signature_event = find_call(reader, "ID3D12Device::CreateRootSignature");
  const auto root_signature_payload = nlohmann::json::parse(root_signature_event->payload, nullptr, false);
  if (root_signature_payload.is_discarded() ||
      root_signature_payload.value("bytecode_size", 0) != sizeof(root_signature_bytes) ||
      !root_signature_payload.contains("root_signature_path") ||
      !root_signature_payload.contains("descriptor_tables") ||
      !root_signature_payload.contains("root_parameters") ||
      root_signature_payload["descriptor_tables"].size() != 1 ||
      root_signature_payload["descriptor_tables"][0].value("root_parameter_index", UINT32_MAX) != 0 ||
      root_signature_payload["descriptor_tables"][0].value("shader_visibility", UINT32_MAX) != D3D12_SHADER_VISIBILITY_PIXEL ||
      root_signature_payload["descriptor_tables"][0]["ranges"].size() != 1 ||
      root_signature_payload["descriptor_tables"][0]["ranges"][0].value("type", UINT32_MAX) != D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
      root_signature_payload["descriptor_tables"][0]["ranges"][0].value("descriptor_count", 0) != 2 ||
      root_signature_payload["descriptor_tables"][0]["ranges"][0].value("base_shader_register", UINT32_MAX) != 3 ||
      root_signature_payload["descriptor_tables"][0]["ranges"][0].value("register_space", UINT32_MAX) != 1 ||
      root_signature_payload["descriptor_tables"][0]["ranges"][0].value("offset_from_table_start", UINT32_MAX) != 0 ||
      root_signature_payload["root_parameters"].size() != 3 ||
      root_signature_payload["root_parameters"][0].value("root_parameter_index", UINT32_MAX) != 0 ||
      root_signature_payload["root_parameters"][0].value("parameter_type", UINT32_MAX) != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
      root_signature_payload["root_parameters"][0].value("range_count", 0) != 1 ||
      root_signature_payload["root_parameters"][1].value("root_parameter_index", UINT32_MAX) != 1 ||
      root_signature_payload["root_parameters"][1].value("parameter_type", UINT32_MAX) != D3D12_ROOT_PARAMETER_TYPE_CBV ||
      root_signature_payload["root_parameters"][1].value("shader_register", UINT32_MAX) != 4 ||
      root_signature_payload["root_parameters"][1].value("register_space", UINT32_MAX) != 2 ||
      root_signature_payload["root_parameters"][2].value("root_parameter_index", UINT32_MAX) != 2 ||
      root_signature_payload["root_parameters"][2].value("parameter_type", UINT32_MAX) != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
      root_signature_payload["root_parameters"][2].value("num_32bit_values", 0) != 6 ||
      root_signature_event->object_refs.size() != 2 ||
      root_signature_event->blob_refs.empty()) {
    std::cerr << "CreateRootSignature did not preserve root signature metadata\n";
    return 1;
  }

  const auto *query_heap_event = find_call(reader, "ID3D12Device::CreateQueryHeap");
  const auto query_heap_payload = nlohmann::json::parse(query_heap_event->payload, nullptr, false);
  if (query_heap_payload.is_discarded() ||
      query_heap_payload.value("type", UINT32_MAX) != D3D12_QUERY_HEAP_TYPE_OCCLUSION ||
      query_heap_payload.value("count", 0) != 4 ||
      query_heap_event->object_refs.size() != 2) {
    std::cerr << "CreateQueryHeap did not preserve query heap metadata\n";
    return 1;
  }

  std::size_t stream_pipeline_calls = 0;
  bool found_compute_stream_pipeline = false;
  bool found_mesh_stream_pipeline = false;
  std::size_t stream_shader_blob_refs = 0;
  std::size_t pipeline_assets = 0;
  for (const auto &asset : reader.assets()) {
    if (asset.kind == apitrace::trace::AssetKind::Pipeline) {
      ++pipeline_assets;
    }
  }
  if (pipeline_assets != 2) {
    std::cerr << "bundle-finalize should rebuild two D3D12 pipeline semantic assets\n";
    return 1;
  }
  for (const auto &event : reader.events()) {
    if (event.kind != apitrace::trace::EventKind::Call ||
        event.callsite.function_name != "ID3D12Device2::CreatePipelineState") {
      continue;
    }
    ++stream_pipeline_calls;
    const auto stream_payload = nlohmann::json::parse(event.payload, nullptr, false);
    if (stream_payload.is_discarded() ||
        stream_payload.value("source", "") != "stream" ||
        stream_payload.value("stream_size", 0) == 0 ||
        !stream_payload.contains("pipeline_path") ||
        !stream_payload.contains("stream_metadata")) {
      std::cerr << "CreatePipelineState stream payload is incomplete\n";
      return 1;
    }
    const std::string pipeline_path = stream_payload.value("pipeline_path", "");
    const auto asset_it = std::find_if(reader.assets().begin(), reader.assets().end(), [&](const auto &asset) {
      return asset.relative_path.generic_string() == pipeline_path;
    });
    if (asset_it == reader.assets().end() || event.blob_refs.empty() ||
        event.blob_refs.front() != asset_it->blob_id) {
      std::cerr << "CreatePipelineState stream pipeline asset was not linked by blob ref\n";
      return 1;
    }
    if (event.blob_refs.size() != 2) {
      std::cerr << "CreatePipelineState stream should link one pipeline asset and one shader asset after bundle-finalize\n";
      return 1;
    }
    ++stream_shader_blob_refs;

    const auto pipeline_json_text = read_text(bundle / asset_it->relative_path);
    const auto pipeline_json = nlohmann::json::parse(pipeline_json_text, nullptr, false);
    if (pipeline_json.is_discarded() || pipeline_json.value("source", "") != "stream") {
      std::cerr << "CreatePipelineState stream pipeline asset is not readable JSON\n";
      return 1;
    }
    if (stream_payload.value("requires_dxmt_backend", false)) {
      if (!pipeline_json.contains("ms") || pipeline_json["ms"].is_null() ||
          !pipeline_json["ms"].contains("ms_path")) {
        std::cerr << "mesh stream pipeline asset did not preserve mesh shader metadata\n";
        return 1;
      }
      found_mesh_stream_pipeline = true;
    } else {
      if (!pipeline_json.contains("cs") || pipeline_json["cs"].is_null() ||
          !pipeline_json["cs"].contains("cs_path")) {
        std::cerr << "compute stream pipeline asset did not preserve compute shader metadata\n";
        return 1;
      }
      found_compute_stream_pipeline = true;
    }
  }
  if (stream_pipeline_calls != 2 || stream_shader_blob_refs != 2 ||
      !found_compute_stream_pipeline || !found_mesh_stream_pipeline) {
    std::cerr << "CreatePipelineState stream calls did not preserve compute and mesh variants\n";
    return 1;
  }

  const auto *copy_event = find_call(reader, "ID3D12Device::CopyDescriptorsBatch");
  const auto payload = nlohmann::json::parse(copy_event->payload, nullptr, false);
  if (payload.is_discarded() ||
      payload.value("schema", "") != "copy-descriptors-v2" ||
      payload.value("op_count", 0) != 1 ||
      !payload.contains("ops") ||
      payload["ops"].size() != 1 ||
      payload["ops"][0].size() < 6 ||
      payload["ops"][0][1].get<std::uint32_t>() != 0 ||
      payload["ops"][0][2].get<std::uint32_t>() != 32 ||
      payload["ops"][0][3].get<std::uint32_t>() != 1 ||
      payload["ops"][0][4].get<std::uint32_t>() != 2 ||
      payload["ops"][0][5].size() != 2 ||
      payload["ops"][0][5][0][0].get<std::uint64_t>() != 0x4000 ||
      payload["ops"][0][5][0][1].get<std::uint64_t>() != 0x5000 ||
      payload["ops"][0][5][0][2].get<std::uint32_t>() != 1 ||
      payload["ops"][0][5][1][0].get<std::uint64_t>() != 0x4020 ||
      payload["ops"][0][5][1][1].get<std::uint64_t>() != 0x6000 ||
      payload["ops"][0][5][1][2].get<std::uint32_t>() != 1 ||
      copy_event->object_refs.size() != 1) {
    std::cerr << "CopyDescriptorsBatch null source range sizes were not preserved correctly\n";
    return 1;
  }

  const std::uint64_t descriptor_resource_id =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(descriptor_resource));
  const std::uint64_t constant_buffer_resource_id =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(constant_buffer_resource));
  const auto has_object_ref = [](const auto &refs, std::uint64_t object_id) {
    return std::find(refs.begin(), refs.end(), object_id) != refs.end();
  };
  nlohmann::json srv_op;
  nlohmann::json uav_op;
  nlohmann::json cbv_op;
  bool srv_refs_ok = false;
  bool uav_refs_ok = false;
  bool cbv_refs_ok = false;
  std::size_t view_op_count = 0;
  for (const auto &event : reader.events()) {
    if (event.kind != apitrace::trace::EventKind::Call ||
        event.callsite.function_name != "ID3D12Device::CreateDescriptorViewBatch") {
      continue;
    }
    const auto view_batch_payload = nlohmann::json::parse(event.payload, nullptr, false);
    if (view_batch_payload.is_discarded() ||
        !view_batch_payload.contains("ops") ||
        !view_batch_payload["ops"].is_array()) {
      continue;
    }
    for (const auto &op : view_batch_payload["ops"]) {
      ++view_op_count;
      if (!op.is_array() || op.size() < 3) {
        continue;
      }
      const auto kind = op[1].get<int>();
      const auto descriptor = op[2].get<std::uint64_t>();
      if (kind == 2 && descriptor == 0x7000) {
        srv_op = op;
        srv_refs_ok = has_object_ref(event.object_refs, descriptor_resource_id);
      } else if (kind == 3 && descriptor == 0x7020) {
        uav_op = op;
        uav_refs_ok = has_object_ref(event.object_refs, descriptor_resource_id);
      } else if (kind == 1 && descriptor == 0x7040) {
        cbv_op = op;
        cbv_refs_ok = has_object_ref(event.object_refs, constant_buffer_resource_id);
      }
    }
  }
  if (view_op_count < 3 ||
      srv_op.is_null() ||
      uav_op.is_null() ||
      cbv_op.is_null() ||
      !srv_refs_ok ||
      !uav_refs_ok ||
      !cbv_refs_ok) {
    std::cerr << "CreateDescriptorViewBatch did not preserve descriptor view ops\n";
    return 1;
  }
  if (srv_op.size() < 16 ||
      srv_op[1].get<int>() != 2 ||
      srv_op[2].get<std::uint64_t>() != 0x7000 ||
      !srv_op[15].is_null()) {
    std::cerr << "CreateDescriptorViewBatch did not preserve null SRV metadata\n";
    return 1;
  }
  if (uav_op.size() < 16 ||
      uav_op[1].get<int>() != 3 ||
      uav_op[2].get<std::uint64_t>() != 0x7020 ||
      uav_op[4].get<std::uint64_t>() != 0 ||
      !uav_op[15].is_null()) {
    std::cerr << "CreateDescriptorViewBatch did not preserve null UAV metadata\n";
    return 1;
  }
  if (cbv_op.size() < 16 ||
      cbv_op[1].get<int>() != 1 ||
      cbv_op[2].get<std::uint64_t>() != 0x7040 ||
      cbv_op[9].get<std::uint64_t>() != 0x100100 ||
      cbv_op[10].get<std::uint64_t>() != 256 ||
      cbv_op[11].get<std::string>() != "mapped" ||
      cbv_op[12].get<std::uint64_t>() !=
          constant_buffer_resource_id ||
      cbv_op[13].get<std::uint64_t>() != 0x100 ||
      cbv_op[14].get<std::uint64_t>() != 4096 ||
      !cbv_op[15].is_null()) {
    std::cerr << "CreateDescriptorViewBatch did not preserve CBV GPUVA resolve metadata\n";
    return 1;
  }

  const auto *barrier_batch_event = find_call(reader, "ID3D12GraphicsCommandList::ResourceBarrierBatch");
  const auto barrier_batch_payload = nlohmann::json::parse(barrier_batch_event->payload, nullptr, false);
  if (barrier_batch_payload.is_discarded() ||
      barrier_batch_payload.value("schema", "") != "resource-barrier-v2" ||
      barrier_batch_event->object_refs.empty() ||
      barrier_batch_event->object_refs.front() !=
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(command_list)) ||
      barrier_batch_payload.value("barrier_count", 0) != 2 ||
      !barrier_batch_payload.contains("barriers") ||
      barrier_batch_payload["barriers"].size() != 2 ||
      barrier_batch_payload["barriers"][0].size() < 9 ||
      barrier_batch_payload["barriers"][0][1].get<std::uint32_t>() != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
      barrier_batch_payload["barriers"][0][3].get<std::uint64_t>() !=
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(copy_src)) ||
      barrier_batch_payload["barriers"][0][4].get<std::uint32_t>() != D3D12_RESOURCE_STATE_COPY_SOURCE ||
      barrier_batch_payload["barriers"][0][5].get<std::uint32_t>() != D3D12_RESOURCE_STATE_COPY_DEST ||
      barrier_batch_payload["barriers"][1][1].get<std::uint32_t>() != D3D12_RESOURCE_BARRIER_TYPE_UAV ||
      barrier_batch_payload["barriers"][1][3].get<std::uint64_t>() !=
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(copy_dst))) {
    std::cerr << "ResourceBarrierBatch did not preserve compact barrier metadata\n";
    return 1;
  }

  const auto *copy_texture_batch_event = find_call(reader, "ID3D12GraphicsCommandList::CopyTextureRegionBatch");
  const auto copy_texture_batch_payload = nlohmann::json::parse(copy_texture_batch_event->payload, nullptr, false);
  if (copy_texture_batch_payload.is_discarded() ||
      copy_texture_batch_payload.value("schema", "") != "copy-texture-region-v2" ||
      copy_texture_batch_event->object_refs.empty() ||
      copy_texture_batch_event->object_refs.front() !=
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(command_list)) ||
      copy_texture_batch_payload.value("op_count", 0) != 1 ||
      !copy_texture_batch_payload.contains("ops") ||
      copy_texture_batch_payload["ops"].size() != 1 ||
      copy_texture_batch_payload["ops"][0].size() < 7 ||
      copy_texture_batch_payload["ops"][0][1][0].get<std::uint64_t>() !=
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(copy_dst)) ||
      copy_texture_batch_payload["ops"][0][2].get<std::uint32_t>() != 4 ||
      copy_texture_batch_payload["ops"][0][3].get<std::uint32_t>() != 5 ||
      copy_texture_batch_payload["ops"][0][5][0].get<std::uint64_t>() !=
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(copy_src)) ||
      copy_texture_batch_payload["ops"][0][6][0].get<std::uint32_t>() != 1 ||
      copy_texture_batch_payload["ops"][0][6][3].get<std::uint32_t>() != 16) {
    std::cerr << "CopyTextureRegionBatch did not preserve compact copy metadata\n";
    return 1;
  }

  const auto *render_pass_event = find_call(reader, "ID3D12GraphicsCommandList4::BeginRenderPass");
  const auto render_pass_payload = nlohmann::json::parse(render_pass_event->payload, nullptr, false);
  if (render_pass_payload.is_discarded() ||
      render_pass_payload.value("render_targets_count", 0) != 1 ||
      !render_pass_payload.value("has_depth_stencil", false) ||
      render_pass_payload["render_targets"][0].value("cpu_descriptor", 0ull) != 0xa000 ||
      render_pass_payload["render_targets"][0]["beginning_access"].value("type", 0) != 2 ||
      render_pass_payload["render_targets"][0]["beginning_access"]["clear"].value("format", 0) != 28 ||
      render_pass_payload["render_targets"][0]["ending_access"].value("src_resource_object_id", 0ull) == 0 ||
      render_pass_payload["render_targets"][0]["ending_access"].value("dst_resource_object_id", 0ull) == 0 ||
      render_pass_payload["render_targets"][0]["ending_access"]["subresources"][0].value("src_subresource", 0) != 2 ||
      render_pass_payload["render_targets"][0]["ending_access"]["subresources"][0]["src_rect"].value("right", 0) != 8 ||
      render_pass_payload["depth_stencil"].value("cpu_descriptor", 0ull) != 0xb000 ||
      render_pass_payload["depth_stencil"]["depth_beginning_access"]["clear"].value("stencil", 0) != 12) {
    std::cerr << "BeginRenderPass did not preserve full render-pass metadata\n";
    return 1;
  }
  if (render_pass_event->object_refs.size() != 3) {
    std::cerr << "BeginRenderPass did not retain resolve resource references\n";
    return 1;
  }

  std::uint64_t expected_present_frame_index = 0;
  std::uint64_t present_call_count = 0;
  std::uint64_t present_test_count = 0;
  for (const auto &event : reader.events()) {
    if (event.kind != apitrace::trace::EventKind::Call ||
        event.callsite.function_name != "IDXGISwapChain::Present") {
      continue;
    }
    ++present_call_count;
    const auto present_payload = nlohmann::json::parse(event.payload, nullptr, false);
    if (present_payload.is_discarded()) {
      std::cerr << "Present payload was not valid JSON\n";
      return 1;
    }
    if (present_payload.value("present_test", false)) {
      if (present_payload.contains("frame_index")) {
        std::cerr << "test Present should not consume a frame_index\n";
        return 1;
      }
      ++present_test_count;
      continue;
    }
    if (present_payload.value("frame_index", UINT64_MAX) != expected_present_frame_index) {
      std::cerr << "Present frame_index was not globally contiguous\n";
      return 1;
    }
    ++expected_present_frame_index;
  }
  if (present_call_count != 3 || present_test_count != 1 || expected_present_frame_index != 2) {
    std::cerr << "capture api did not preserve test Present while keeping frame indexes real-present only\n";
    return 1;
  }

  return 0;
}
