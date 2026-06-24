#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

std::filesystem::path unique_work_dir()
{
  const auto base = std::filesystem::temp_directory_path();
  return base / ("apitrace-raw-to-final-test-" + std::to_string(static_cast<unsigned long long>(
                                               reinterpret_cast<std::uintptr_t>(&base))));
}

bool expect(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

std::string quote_arg(const std::filesystem::path &path)
{
  std::string text = path.string();
  std::string quoted = "'";
  for (const char ch : text) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

bool run_command(const std::string &command)
{
  const auto result = std::system(command.c_str());
  if (result != 0) {
    std::cerr << "command failed: " << command << "\n";
    return false;
  }
  return true;
}

std::vector<json> read_jsonl(const std::filesystem::path &path)
{
  std::vector<json> records;
  std::ifstream input(path, std::ios::binary);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    auto parsed = json::parse(line, nullptr, false);
    if (!parsed.is_discarded()) {
      records.push_back(std::move(parsed));
    }
  }
  return records;
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
}

std::vector<std::string> read_lines(const std::filesystem::path &path)
{
  std::vector<std::string> lines;
  std::ifstream input(path, std::ios::binary);
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  return lines;
}

bool append_raw_event(
    apitrace::trace::raw::RawCaptureWriter &writer,
    std::uint64_t sequence,
    apitrace::trace::raw::RawEventOpcode opcode,
    const std::vector<std::uint8_t> &payload)
{
  apitrace::trace::raw::RawEventHeader header;
  header.sequence = sequence;
  header.thread_id = 1;
  header.timestamp_or_monotonic_counter = 1000 + sequence;
  header.opcode = static_cast<std::uint32_t>(opcode);
  header.result_or_flags = 0;
  header.payload_len = payload.size();
  return writer.append_event(header, payload.data(), payload.size());
}

bool write_synthetic_raw_capture(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open synthetic raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> buffer_blob = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const std::vector<std::uint8_t> vs_blob = {'v', 's', '-', 'd', 'x', 'b', 'c'};
  const std::vector<std::uint8_t> ps_blob = {'p', 's', '-', 'd', 'x', 'b', 'c'};
  const auto buffer_raw_id = writer.append_blob(buffer_blob.data(), buffer_blob.size(), static_cast<std::uint32_t>(RawBlobKind::Buffer), 2);
  const auto vs_raw_id = writer.append_blob(vs_blob.data(), vs_blob.size(), static_cast<std::uint32_t>(RawBlobKind::ShaderDxbc), 3);
  const auto ps_raw_id = writer.append_blob(ps_blob.data(), ps_blob.size(), static_cast<std::uint32_t>(RawBlobKind::ShaderDxbc), 3);
  if (!expect(buffer_raw_id != kInvalidRawBlobId &&
                  vs_raw_id != kInvalidRawBlobId &&
                  ps_raw_id != kInvalidRawBlobId,
              "failed to append synthetic raw blobs")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!append_raw_event(writer, 1, RawEventOpcode::FrameBegin, encode_frame_boundary_payload(0)) ||
      !append_raw_event(
          writer,
          2,
          RawEventOpcode::ResourceCreate,
          encode_resource_create_payload(100, 200, 1, 4096, 1, 1, 1, 87, 0, 4, "synthetic-buffer")) ||
      !append_raw_event(writer, 3, RawEventOpcode::ResourceUnmap, encode_resource_unmap_payload(200, buffer_raw_id, 0, buffer_blob.size())) ||
      !append_raw_event(
          writer,
          4,
          RawEventOpcode::GraphicsPipelineCreate,
          encode_graphics_pipeline_create_payload(100, 300, 400, vs_raw_id, vs_blob.size(), ps_raw_id, ps_blob.size(), 0, 0)) ||
      !append_raw_event(writer, 5, RawEventOpcode::DrawInstanced, encode_draw_instanced_payload(500, 3, 1, 0, 0)) ||
      !append_raw_event(writer, 6, RawEventOpcode::Dispatch, encode_dispatch_payload(500, 2, 3, 4)) ||
      !append_raw_event(writer, 7, RawEventOpcode::PresentCall, encode_present_payload(600, 0, 1, 0)) ||
      !append_raw_event(writer, 8, RawEventOpcode::PresentBoundary, encode_present_payload(600, 0, 1, 0)) ||
      !append_raw_event(writer, 9, RawEventOpcode::FrameEnd, encode_frame_boundary_payload(0))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit synthetic raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool write_passthrough_mixed_raw_capture(
    const std::filesystem::path &bundle,
    const std::string &passthrough_before,
    const std::string &passthrough_after)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open passthrough raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!append_raw_event(
          writer,
          1,
          RawEventOpcode::Passthrough,
          encode_passthrough_final_json_payload(passthrough_before)) ||
      !append_raw_event(
          writer,
          2,
          RawEventOpcode::DrawInstanced,
          encode_draw_instanced_payload(500, 3, 1, 0, 0)) ||
      !append_raw_event(
          writer,
          3,
          RawEventOpcode::Passthrough,
          encode_passthrough_final_json_payload(passthrough_after))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit passthrough raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool write_passthrough_with_blob_raw_capture(
    const std::filesystem::path &bundle,
    const std::string &passthrough_blob_line,
    const std::string &passthrough_non_blob_line)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open passthrough-with-blob raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> buffer_blob = {0xaa, 0xbb, 0xcc, 0xdd};
  const auto buffer_raw_id = writer.append_blob(
      buffer_blob.data(),
      buffer_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      1);
  if (!expect(buffer_raw_id != kInvalidRawBlobId, "failed to append passthrough-with-blob bytes")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  PassthroughBlobDescriptor descriptor;
  descriptor.provisional_asset_path = "buffers/asset-000000000000004d.buffer";
  descriptor.final_blob_id = 77;
  descriptor.raw_blob_id = buffer_raw_id;
  descriptor.raw_blob_kind = static_cast<std::uint32_t>(RawBlobKind::Buffer);
  descriptor.debug_name = "d3d12-resource-unmap";

  if (!append_raw_event(
          writer,
          1,
          RawEventOpcode::PassthroughWithBlob,
          encode_passthrough_with_blob_payload(passthrough_blob_line, {descriptor})) ||
      !append_raw_event(
          writer,
          2,
          RawEventOpcode::Passthrough,
          encode_passthrough_final_json_payload(passthrough_non_blob_line))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit passthrough-with-blob raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool validate_final_bundle(const std::filesystem::path &bundle)
{
  const auto records = read_jsonl(bundle / "callstream.jsonl");
  if (!expect(records.size() == 10, "unexpected callstream record count")) {
    return false;
  }
  if (!expect(records[0].value("record_kind", "") == "bundle_header", "missing bundle header")) {
    return false;
  }

  std::vector<std::string> functions;
  std::vector<std::string> kinds;
  for (std::size_t index = 1; index < records.size(); ++index) {
    kinds.push_back(records[index].value("record_kind", std::string()));
    functions.push_back(records[index].value("function", std::string()));
  }
  if (!expect(kinds[0] == "boundary" &&
                  kinds[1] == "object_create" &&
                  functions[2] == "ID3D12Resource::Unmap" &&
                  functions[3] == "ID3D12Device::CreateGraphicsPipelineState" &&
                  functions[4] == "ID3D12GraphicsCommandList::DrawInstanced" &&
                  functions[5] == "ID3D12GraphicsCommandList::Dispatch" &&
                  functions[6] == "IDXGISwapChain::Present" &&
                  kinds[7] == "boundary" &&
                  kinds[8] == "boundary",
              "raw-to-final callstream shape mismatch")) {
    return false;
  }

  const auto &unmap = records[3];
  if (!expect(unmap["blob_refs"].is_array() && unmap["blob_refs"].size() == 1, "Unmap should reference one buffer blob")) {
    return false;
  }
  const auto buffer_blob_id = unmap["blob_refs"][0].get<std::uint64_t>();
  const auto buffer_path = unmap["payload"].value("buffer_path", std::string());
  if (!expect(buffer_path.rfind("buffers/", 0) == 0 && buffer_path.find("asset-") == std::string::npos,
              "Unmap buffer path was not canonicalized")) {
    return false;
  }
  if (!expect(unmap["payload"]["written_range"].value("begin", 1) == 0 &&
                  unmap["payload"]["written_range"].value("end", 0) == 6,
              "Unmap written range mismatch")) {
    return false;
  }

  const auto &pipeline = records[4];
  if (!expect(pipeline["blob_refs"].is_array() &&
                  pipeline["blob_refs"].size() >= 3 &&
                  pipeline["payload"].contains("pipeline_path") &&
                  !pipeline["payload"].contains("pso_raw_version"),
              "pipeline event was not rebuilt to final form")) {
    return false;
  }
  const auto pipeline_path = pipeline["payload"].value("pipeline_path", std::string());
  if (!expect(pipeline_path.rfind("pipelines/", 0) == 0, "pipeline path missing from rebuilt event")) {
    return false;
  }

  const auto assets_json = json::parse(std::ifstream(bundle / "assets.json"), nullptr, false);
  if (!expect(!assets_json.is_discarded() && assets_json.contains("assets"), "assets.json missing or invalid")) {
    return false;
  }
  std::filesystem::path buffer_asset_path;
  bool saw_pipeline_asset = false;
  for (const auto &asset : assets_json["assets"]) {
    if (asset.value("blob_id", 0ull) == buffer_blob_id) {
      buffer_asset_path = asset.value("path", std::string());
    }
    if (asset.value("path", std::string()) == pipeline_path && asset.value("kind", std::string()) == "Pipeline") {
      saw_pipeline_asset = true;
    }
  }
  if (!expect(!buffer_asset_path.empty() && saw_pipeline_asset, "expected assets not indexed")) {
    return false;
  }
  if (!expect(read_file_bytes(bundle / buffer_asset_path) == std::vector<std::uint8_t>({0x10, 0x20, 0x30, 0x40, 0x50, 0x60}),
              "canonical buffer asset bytes mismatch")) {
    return false;
  }
  return true;
}

bool validate_passthrough_mixed_bundle(
    const std::filesystem::path &bundle,
    const std::string &passthrough_before,
    const std::string &passthrough_after)
{
  const auto lines = read_lines(bundle / "callstream.jsonl");
  if (!expect(lines.size() == 4, "unexpected passthrough callstream line count")) {
    return false;
  }
  if (!expect(lines[1] == passthrough_before, "first passthrough line was not preserved verbatim")) {
    return false;
  }
  if (!expect(lines[3] == passthrough_after, "second passthrough line was not preserved verbatim")) {
    return false;
  }

  const auto records = read_jsonl(bundle / "callstream.jsonl");
  if (!expect(records.size() == 4, "unexpected passthrough JSONL record count")) {
    return false;
  }
  if (!expect(records[0].value("record_kind", "") == "bundle_header", "missing passthrough bundle header")) {
    return false;
  }
  if (!expect(records[1].value("sequence", 0) == 1 &&
                  records[2].value("sequence", 0) == 2 &&
                  records[3].value("sequence", 0) == 3,
              "passthrough mixed sequence order mismatch")) {
    return false;
  }
  if (!expect(records[2].value("function", "") == "ID3D12GraphicsCommandList::DrawInstanced",
              "binary event was not interleaved between passthrough events")) {
    return false;
  }
  return true;
}

bool validate_passthrough_with_blob_bundle(
    const std::filesystem::path &bundle,
    const std::string &passthrough_blob_line,
    const std::string &passthrough_non_blob_line)
{
  (void)passthrough_blob_line;
  const auto lines = read_lines(bundle / "callstream.jsonl");
  if (!expect(lines.size() == 3, "unexpected passthrough-with-blob callstream line count")) {
    return false;
  }
  if (!expect(lines[2] == passthrough_non_blob_line, "non-blob passthrough line was not preserved verbatim")) {
    return false;
  }

  const auto records = read_jsonl(bundle / "callstream.jsonl");
  if (!expect(records.size() == 3, "unexpected passthrough-with-blob JSONL record count")) {
    return false;
  }
  const auto &unmap = records[1];
  if (!expect(unmap.value("function", "") == "ID3D12Resource::Unmap", "blob passthrough function mismatch") ||
      !expect(unmap["blob_refs"].is_array() && unmap["blob_refs"].size() == 1, "blob passthrough missing blob_refs") ||
      !expect(unmap["blob_refs"][0].get<std::uint64_t>() == 77, "blob passthrough blob id changed")) {
    return false;
  }
  const auto final_payload_path = unmap["payload"].value("buffer_path", std::string());
  if (!expect(final_payload_path.rfind("buffers/", 0) == 0 &&
                  final_payload_path.find("asset-") == std::string::npos,
              "blob passthrough buffer path was not canonicalized")) {
    return false;
  }

  const auto assets_json = json::parse(std::ifstream(bundle / "assets.json"), nullptr, false);
  if (!expect(!assets_json.is_discarded() && assets_json.contains("assets"), "passthrough-with-blob assets.json missing")) {
    return false;
  }
  std::string buffer_path;
  for (const auto &asset : assets_json["assets"]) {
    if (asset.value("blob_id", 0ull) == 77) {
      buffer_path = asset.value("path", std::string());
    }
  }
  if (!expect(buffer_path.rfind("buffers/", 0) == 0, "passthrough-with-blob asset not indexed")) {
    return false;
  }
  if (!expect(read_file_bytes(bundle / buffer_path) == std::vector<std::uint8_t>({0xaa, 0xbb, 0xcc, 0xdd}),
              "passthrough-with-blob asset bytes mismatch")) {
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
#if !defined(APITRACE_ENABLE_TEST_HOOKS)
  std::cerr << "test requires APITRACE_ENABLE_TEST_HOOKS\n";
  return 77;
#else
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " <bundle-finalize> <bundle-check>\n";
    return 2;
  }

  const auto work_dir = unique_work_dir();
  const auto bundle = work_dir / "synthetic-raw.apitrace";
  const auto passthrough_bundle = work_dir / "synthetic-passthrough-raw.apitrace";
  const auto passthrough_blob_bundle = work_dir / "synthetic-passthrough-with-blob-raw.apitrace";
  const std::string passthrough_before =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12GraphicsCommandList::SetPipelineState\",\"result_code\":0,\"object_refs\":[500,700],\"payload\":{\"state\":\"passthrough-before\",\"spacing\":\"kept exactly\"}}";
  const std::string passthrough_after =
      "{\"record_kind\":\"boundary\",\"sequence\":3,\"time_ns\":1003,\"elapsed_ns\":0,\"boundary\":\"DebugMarker\",\"payload\":{\"label\":\"passthrough-after\",\"nested\":{\"value\":42}}}";
  const std::string passthrough_blob_line =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12Resource::Unmap\",\"result_code\":0,\"blob_refs\":[77],\"payload\":{\"buffer_path\":\"buffers/asset-000000000000004d.buffer\",\"written_range\":{\"begin\":0,\"end\":4}}}";
  const std::string passthrough_non_blob_line =
      "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1002,\"elapsed_ns\":0,\"function\":\"ID3D12GraphicsCommandList::DrawInstanced\",\"result_code\":0,\"object_refs\":[500],\"payload\":{\"vertex_count_per_instance\":3,\"instance_count\":1,\"start_vertex_location\":0,\"start_instance_location\":0}}";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const bool ok =
      write_synthetic_raw_capture(bundle) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress " + quote_arg(bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(bundle)) &&
      validate_final_bundle(bundle) &&
      write_passthrough_mixed_raw_capture(passthrough_bundle, passthrough_before, passthrough_after) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress " + quote_arg(passthrough_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(passthrough_bundle)) &&
      validate_passthrough_mixed_bundle(passthrough_bundle, passthrough_before, passthrough_after) &&
      write_passthrough_with_blob_raw_capture(passthrough_blob_bundle, passthrough_blob_line, passthrough_non_blob_line) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress " + quote_arg(passthrough_blob_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(passthrough_blob_bundle)) &&
      validate_passthrough_with_blob_bundle(passthrough_blob_bundle, passthrough_blob_line, passthrough_non_blob_line);

  std::filesystem::remove_all(work_dir);
  if (!ok) {
    return 1;
  }
  std::cout << "raw_to_final synthetic e2e test passed\n";
  return 0;
#endif
}
