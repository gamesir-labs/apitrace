#include "apitrace/trace_bundle_io.hpp"
#include "retrace/src/d3d11_replay_parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using apitrace::trace::ApiKind;
using apitrace::trace::AssetKind;
using apitrace::trace::AssetRecord;
using apitrace::trace::BlobId;
using apitrace::trace::EventKind;
using apitrace::trace::EventRecord;
using apitrace::trace::ObjectId;
using apitrace::trace::TraceBundleReader;
using apitrace::trace::TraceBundleWriter;

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool bundle_has_timing_stamps(const std::filesystem::path &bundle)
{
  const auto callstream = read_text(bundle / apitrace::trace::kCallstreamFileName);
  if (callstream.find("\"time_origin_ns\":") == std::string::npos ||
      callstream.find("\"monotonic_origin_ns\":") == std::string::npos ||
      callstream.find("\"time_ns\":") == std::string::npos ||
      callstream.find("\"elapsed_ns\":") == std::string::npos) {
    return false;
  }

  TraceBundleReader reader;
  if (!reader.open(bundle)) {
    return false;
  }
  for (const auto &event : reader.events()) {
    if (event.time_ns != 0 && event.elapsed_ns != 0) {
      return true;
    }
  }
  return false;
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

AssetRecord asset(
    AssetKind kind,
    BlobId blob_id,
    const char *debug_name,
    std::string payload)
{
  AssetRecord record;
  record.kind = kind;
  record.blob_id = blob_id;
  record.debug_name = debug_name ? debug_name : "";
  record.payload_bytes.assign(payload.begin(), payload.end());
  return record;
}

bool write_poisoned_present_frame_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = ApiKind::D3D11;
  metadata.producer = "test_d3d11_poisoned_present_frame";
  writer.write_metadata(metadata);
  writer.write_object_index({});

  auto frame = asset(AssetKind::Texture, 200, "poisoned-d3d11-present-frame", std::string(16, '\0'));
  frame = writer.register_asset(std::move(frame));
  if (frame.relative_path.empty()) {
    return false;
  }

  writer.append_call_event(boundary(
      1,
      apitrace::trace::BoundaryKind::Frame,
      "{\"label\":\"FrameBegin\",\"frame_index\":0}"));
  writer.append_call_event(call(
      2,
      "IDXGISwapChain::Present",
      {10},
      "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}"));
  writer.append_call_event(boundary(
      3,
      apitrace::trace::BoundaryKind::Present,
      "{\"label\":\"Present\",\"frame_index\":0,\"sync_interval\":1,\"flags\":0}"));
  writer.append_call_event(boundary(
      4,
      apitrace::trace::BoundaryKind::Frame,
      "{\"label\":\"FrameEnd\",\"frame_index\":0}"));
  writer.append_call_event(resource_blob(
      5,
      "D3D11PresentFrame",
      {frame.blob_id},
      "{\"frame_index\":0,\"width\":2,\"height\":2,\"row_pitch\":8,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\","
      "\"frame_path\":\"" +
          frame.relative_path.generic_string() + "\"}"));
  writer.close();
  return true;
}

bool write_shader_blob_ref_bundle(
    const std::filesystem::path &bundle,
    bool wrong_blob_ref)
{
  std::filesystem::remove_all(bundle);
  TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = ApiKind::D3D11;
  metadata.producer = wrong_blob_ref ? "test_d3d11_wrong_shader_blob_ref" : "test_d3d11_shader_blob_ref";
  writer.write_metadata(metadata);
  writer.write_object_index({});

  auto shader = asset(AssetKind::ShaderDxbc, 100, "shader.dxbc", "shader-bytecode");
  shader = writer.register_asset(std::move(shader));
  AssetRecord other;
  if (wrong_blob_ref) {
    other = asset(AssetKind::Buffer, 101, "wrong-buffer", "not-shader");
    other = writer.register_asset(std::move(other));
  }
  if (shader.relative_path.empty() || (wrong_blob_ref && other.relative_path.empty())) {
    return false;
  }

  writer.append_call_event(call(
      1,
      "ID3D11Device::CreateVertexShader",
      {1, 2},
      "{\"shader_path\":\"" + shader.relative_path.generic_string() + "\"}",
      {wrong_blob_ref ? other.blob_id : shader.blob_id}));
  writer.close();
  return true;
}

bool write_missing_draw_metadata_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = ApiKind::D3D11;
  metadata.producer = "test_d3d11_missing_draw_metadata";
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_call_event(call(
      1,
      "ID3D11DeviceContext::Draw",
      {20},
      "{\"start_vertex_location\":0}"));
  writer.close();
  return true;
}

bool write_missing_clear_depth_metadata_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = ApiKind::D3D11;
  metadata.producer = "test_d3d11_missing_clear_depth_metadata";
  writer.write_metadata(metadata);
  writer.write_object_index({});

  writer.append_call_event(call(
      1,
      "ID3D11DeviceContext::ClearDepthStencilView",
      {20, 21},
      "{\"clear_flags\":1,\"depth\":1.0}"));
  writer.close();
  return true;
}

bool build_plan(
    const std::filesystem::path &bundle,
    apitrace::replay::internal::D3D11ReplayPlan &plan,
    std::string &error)
{
  error.clear();
  TraceBundleReader reader;
  if (!reader.open(bundle)) {
    error = reader.last_error();
    return false;
  }
  if (!apitrace::replay::internal::build_d3d11_replay_plan(reader, plan, error)) {
    return false;
  }
  error.clear();
  return true;
}

bool build_plan(
    const std::filesystem::path &bundle,
    std::string &error)
{
  apitrace::replay::internal::D3D11ReplayPlan plan;
  return build_plan(bundle, plan, error);
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    std::cerr << "usage: test_d3d11_replay_semantics <work-dir>\n";
    return 2;
  }

  const std::filesystem::path work_dir = argv[1];
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const auto valid_bundle = work_dir / "valid-shader.apitrace";
  if (!write_shader_blob_ref_bundle(valid_bundle, false)) {
    std::cerr << "failed to write valid D3D11 bundle\n";
    return 1;
  }
  if (!bundle_has_timing_stamps(valid_bundle)) {
    std::cerr << "D3D11 writer/reader did not preserve timing stamps\n";
    return 1;
  }
  std::string error;
  if (!build_plan(valid_bundle, error)) {
    std::cerr << "D3D11 parser rejected valid bundle: " << error << "\n";
    return 1;
  }

  const auto wrong_blob_ref_bundle = work_dir / "wrong-shader-blob-ref.apitrace";
  if (!write_shader_blob_ref_bundle(wrong_blob_ref_bundle, true)) {
    std::cerr << "failed to write wrong blob_ref D3D11 bundle\n";
    return 1;
  }
  if (build_plan(wrong_blob_ref_bundle, error) ||
      error.find("shader_path blob_ref does not match") == std::string::npos) {
    std::cerr << "D3D11 parser accepted wrong shader blob_ref: " << error << "\n";
    return 1;
  }

  const auto poisoned_present_frame_bundle = work_dir / "poisoned-present-frame.apitrace";
  if (!write_poisoned_present_frame_bundle(poisoned_present_frame_bundle)) {
    std::cerr << "failed to write poisoned PresentFrame D3D11 bundle\n";
    return 1;
  }
  apitrace::replay::internal::D3D11ReplayPlan poisoned_plan;
  if (!build_plan(poisoned_present_frame_bundle, poisoned_plan, error)) {
    std::cerr << "D3D11 parser rejected poisoned PresentFrame bundle: " << error << "\n";
    return 1;
  }
  if (poisoned_plan.present_frames.size() != 1 ||
      poisoned_plan.present_frames.front().frame_index != 0 ||
      poisoned_plan.present_frames.front().frame_path.empty()) {
    std::cerr << "poisoned D3D11PresentFrame was not preserved as diagnostic metadata\n";
    return 1;
  }
  if (poisoned_plan.commands.size() != 4) {
    std::cerr << "D3D11PresentFrame became a replay command\n";
    return 1;
  }
  const auto present_commands = std::count_if(
      poisoned_plan.commands.begin(),
      poisoned_plan.commands.end(),
      [](const auto &command) {
        return std::holds_alternative<apitrace::replay::internal::PresentCommand>(command);
      });
  if (present_commands != 1) {
    std::cerr << "D3D11 Present command was not preserved independently from diagnostic PresentFrame\n";
    return 1;
  }

  const auto missing_draw_metadata_bundle = work_dir / "missing-draw-metadata.apitrace";
  if (!write_missing_draw_metadata_bundle(missing_draw_metadata_bundle)) {
    std::cerr << "failed to write missing draw metadata D3D11 bundle\n";
    return 1;
  }
  if (build_plan(missing_draw_metadata_bundle, error) ||
      error.find("missing uint32 field vertex_count") == std::string::npos) {
    std::cerr << "D3D11 parser accepted Draw without vertex_count: " << error << "\n";
    return 1;
  }

  const auto missing_clear_depth_metadata_bundle = work_dir / "missing-clear-depth-metadata.apitrace";
  if (!write_missing_clear_depth_metadata_bundle(missing_clear_depth_metadata_bundle)) {
    std::cerr << "failed to write missing clear depth metadata D3D11 bundle\n";
    return 1;
  }
  if (build_plan(missing_clear_depth_metadata_bundle, error) ||
      error.find("missing uint32 field stencil") == std::string::npos) {
    std::cerr << "D3D11 parser accepted ClearDepthStencilView without stencil: " << error << "\n";
    return 1;
  }

  std::cout << "d3d11_replay_semantics PASS\n";
  return 0;
}
