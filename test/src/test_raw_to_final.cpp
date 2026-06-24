#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"
#include "apitrace/trace_session.hpp"

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

bool set_env_var(const char *name, const char *value)
{
#ifdef _WIN32
  return _putenv_s(name, value ? value : "") == 0;
#else
  return value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0;
#endif
}

bool copy_directory(const std::filesystem::path &source, const std::filesystem::path &target)
{
  std::error_code error;
  std::filesystem::remove_all(target, error);
  std::filesystem::create_directories(target, error);
  if (error) {
    std::cerr << "failed to create copy target: " << target << ": " << error.message() << "\n";
    return false;
  }
  for (std::filesystem::recursive_directory_iterator it(source, error), end; it != end; it.increment(error)) {
    if (error) {
      std::cerr << "failed to iterate copy source: " << source << ": " << error.message() << "\n";
      return false;
    }
    const auto relative = std::filesystem::relative(it->path(), source, error);
    if (error) {
      std::cerr << "failed to compute relative path: " << it->path() << ": " << error.message() << "\n";
      return false;
    }
    const auto destination = target / relative;
    if (it->is_directory(error)) {
      std::filesystem::create_directories(destination, error);
    } else if (it->is_regular_file(error)) {
      std::filesystem::create_directories(destination.parent_path(), error);
      std::filesystem::copy_file(it->path(), destination, std::filesystem::copy_options::overwrite_existing, error);
    }
    if (error) {
      std::cerr << "failed to copy " << it->path() << " to " << destination << ": " << error.message() << "\n";
      return false;
    }
  }
  return true;
}

apitrace::TraceOptions trace_options(const std::filesystem::path &bundle)
{
  apitrace::TraceOptions options;
  options.api = apitrace::trace::ApiKind::D3D12;
  options.bundle_root = bundle;
  options.capture.raw_format_reserved = true;
  return options;
}

apitrace::trace::EventRecord make_call_event(
    std::uint64_t sequence,
    const char *function_name,
    std::string payload,
    std::vector<apitrace::trace::ObjectId> object_refs = {},
    std::vector<apitrace::trace::BlobId> blob_refs = {})
{
  apitrace::trace::EventRecord event;
  event.kind = apitrace::trace::EventKind::Call;
  event.callsite.sequence = sequence;
  event.callsite.function_name = function_name;
  event.callsite.result_code = 0;
  event.object_refs = std::move(object_refs);
  event.blob_refs = std::move(blob_refs);
  event.payload = std::move(payload);
  return event;
}

bool write_synthetic_dual_write_capture(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::TraceSession session(trace_options(bundle));
  session.begin();
  session.append_call_event(make_call_event(
      11,
      "ID3D12GraphicsCommandList::SetPipelineState",
      "{\"state\":\"synthetic-non-blob\",\"spacing\":\"kept exactly\"}",
      {500, 300}));

  apitrace::trace::AssetRecord buffer;
  buffer.blob_id = 77;
  buffer.kind = apitrace::trace::AssetKind::Buffer;
  buffer.debug_name = "d3d12-resource-unmap";
  buffer.payload_bytes = {0x01, 0x02, 0x03, 0x04};
  buffer = session.register_asset(std::move(buffer));
  session.append_call_event(make_call_event(
      12,
      "ID3D12Resource::Unmap",
      "{\"buffer_path\":\"" + buffer.relative_path.generic_string() +
          "\",\"written_range\":{\"begin\":0,\"end\":4}}",
      {200},
      {buffer.blob_id}));

  apitrace::trace::AssetRecord vs;
  vs.blob_id = 78;
  vs.kind = apitrace::trace::AssetKind::ShaderDxbc;
  vs.debug_name = "raw-vs-bytecode";
  vs.payload_bytes = {'v', 's', '-', 'd', 'x', 'b', 'c'};
  vs = session.register_asset(std::move(vs));
  apitrace::trace::AssetRecord ps;
  ps.blob_id = 79;
  ps.kind = apitrace::trace::AssetKind::ShaderDxbc;
  ps.debug_name = "raw-ps-bytecode";
  ps.payload_bytes = {'p', 's', '-', 'd', 'x', 'b', 'c'};
  ps = session.register_asset(std::move(ps));
  const std::string pso_payload =
      "{\"pso_raw_version\":1"
      ",\"pso_kind\":\"graphics\""
      ",\"root_signature_object_id\":400"
      ",\"node_mask\":0"
      ",\"flags\":0"
      ",\"input_layout\":[]"
      ",\"blend_state\":{}"
      ",\"sample_mask\":4294967295"
      ",\"rasterizer_state\":{}"
      ",\"depth_stencil_state\":{}"
      ",\"stream_output\":{}"
      ",\"primitive_topology_type\":3"
      ",\"num_render_targets\":1"
      ",\"rtv_formats\":[28,0,0,0,0,0,0,0]"
      ",\"dsv_format\":0"
      ",\"sample_desc\":{\"count\":1,\"quality\":0}"
      ",\"ib_strip_cut_value\":0"
      ",\"vs\":{\"bytecode_size\":" + std::to_string(vs.byte_size) + ",\"blob_id\":" + std::to_string(vs.blob_id) + "}"
      ",\"ps\":{\"bytecode_size\":" + std::to_string(ps.byte_size) + ",\"blob_id\":" + std::to_string(ps.blob_id) + "}"
      ",\"ds\":null,\"hs\":null,\"gs\":null"
      ",\"requires_dxmt_backend\":false}";
  session.append_call_event(make_call_event(
      13,
      "ID3D12Device::CreateGraphicsPipelineState",
      pso_payload,
      {100, 400, 300},
      {vs.blob_id, ps.blob_id}));

  session.append_call_event(make_call_event(
      14,
      "ID3D12GraphicsCommandList::DrawInstanced",
      "{\"vertex_count_per_instance\":3,\"instance_count\":1,\"start_vertex_location\":0,\"start_instance_location\":0}",
      {500}));
  session.end();
  return std::filesystem::exists(bundle / "raw" / "events.bin") &&
         std::filesystem::exists(bundle / "raw" / "blobs.bin");
}

bool write_no_end_periodic_commit_capture(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  if (!expect(set_env_var("APITRACE_RAW_COMMIT_BYTES", "512"),
              "failed to set raw commit cadence test env")) {
    return false;
  }

  {
    apitrace::TraceSession session(trace_options(bundle));
    session.begin();
    for (std::uint64_t index = 0; index < 8; ++index) {
      const std::string padding(96, static_cast<char>('a' + index));
      session.append_call_event(make_call_event(
          100 + index,
          "ID3D12GraphicsCommandList::SetPipelineState",
          "{\"padding\":\"" + padding + "\"}",
          {500 + index}));
    }

    apitrace::trace::AssetRecord buffer;
    buffer.blob_id = 177;
    buffer.kind = apitrace::trace::AssetKind::Buffer;
    buffer.debug_name = "periodic-commit-buffer";
    buffer.payload_bytes.assign(768, 0x5a);
    buffer = session.register_asset(std::move(buffer));
    session.append_call_event(make_call_event(
        200,
        "ID3D12Resource::Unmap",
        "{\"buffer_path\":\"" + buffer.relative_path.generic_string() +
            "\",\"written_range\":{\"begin\":0,\"end\":" + std::to_string(buffer.byte_size) + "}}",
        {700},
        {buffer.blob_id}));

    session.append_call_event(make_call_event(
        201,
        "ID3D12GraphicsCommandList::DrawInstanced",
        "{\"vertex_count_per_instance\":3,\"instance_count\":1,\"start_vertex_location\":0,\"start_instance_location\":0}",
        {500}));
  }

  if (!expect(set_env_var("APITRACE_RAW_COMMIT_BYTES", nullptr),
              "failed to clear raw commit cadence test env")) {
    return false;
  }

  apitrace::trace::raw::RawCaptureReader reader;
  if (!expect(reader.open(bundle), "no-end raw reader could not open periodic capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }
  const auto committed = reader.committed_prefix();
  if (!expect(committed.events_committed_bytes > 16 &&
                  committed.blobs_committed_bytes > 16 &&
                  committed.blob_index_committed_bytes > 16,
              "periodic raw commit did not advance all committed prefixes before end()")) {
    return false;
  }
  const auto events = reader.read_events();
  if (!expect(events.size() >= 9, "periodic raw commit did not expose events through committed prefix")) {
    return false;
  }
  bool saw_blob_event = false;
  for (const auto &event : events) {
    if (event.header.sequence == 200) {
      saw_blob_event = true;
      break;
    }
  }
  if (!expect(saw_blob_event, "periodic raw commit did not include the blob-referencing event")) {
    return false;
  }
  if (!expect(reader.blob_extents().size() == 1, "periodic raw commit did not expose committed blob extent")) {
    return false;
  }
  std::vector<std::uint8_t> blob;
  if (!expect(reader.read_blob(reader.blob_extents().front().raw_blob_id, blob),
              "periodic raw commit exposed a torn blob reference") ||
      !expect(blob.size() == 768 && blob.front() == 0x5a && blob.back() == 0x5a,
              "periodic raw commit blob payload mismatch")) {
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

bool run_dual_write_parity_test(
    const std::filesystem::path &source_bundle,
    const std::filesystem::path &old_bundle,
    const std::filesystem::path &raw_bundle,
    const char *finalize,
    const char *check,
    const std::filesystem::path &compare_script)
{
  return write_synthetic_dual_write_capture(source_bundle) &&
         copy_directory(source_bundle, old_bundle) &&
         copy_directory(source_bundle, raw_bundle) &&
         run_command(quote_arg(finalize) + " --no-progress --jobs 1 " + quote_arg(old_bundle)) &&
         run_command(quote_arg(finalize) + " --raw-format --no-progress --jobs 1 " + quote_arg(raw_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(old_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(raw_bundle)) &&
         expect(read_file_bytes(old_bundle / "callstream.jsonl") == read_file_bytes(raw_bundle / "callstream.jsonl"),
                "dual-write old/raw finalized callstream mismatch") &&
         run_command("python3 " + quote_arg(compare_script) +
                     " --bundle-finalize " + quote_arg(finalize) +
                     " --bundle-check " + quote_arg(check) +
                     " --work-dir " + quote_arg(source_bundle.parent_path() / "compare-work") +
                     " --jobs 1 " + quote_arg(source_bundle));
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
  const auto dual_write_bundle = work_dir / "synthetic-dual-write.apitrace";
  const auto dual_write_old_bundle = work_dir / "synthetic-dual-write-old.apitrace";
  const auto dual_write_raw_bundle = work_dir / "synthetic-dual-write-raw.apitrace";
  const auto no_end_raw_bundle = work_dir / "synthetic-no-end-raw.apitrace";
  const auto compare_script = std::filesystem::current_path() / "scripts" / "compare-raw-parity.py";
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
      validate_passthrough_with_blob_bundle(passthrough_blob_bundle, passthrough_blob_line, passthrough_non_blob_line) &&
      write_no_end_periodic_commit_capture(no_end_raw_bundle) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress --jobs 1 " + quote_arg(no_end_raw_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(no_end_raw_bundle)) &&
      run_dual_write_parity_test(
          dual_write_bundle,
          dual_write_old_bundle,
          dual_write_raw_bundle,
          argv[1],
          argv[2],
          compare_script);

  std::filesystem::remove_all(work_dir);
  if (!ok) {
    return 1;
  }
  std::cout << "raw_to_final synthetic e2e test passed\n";
  return 0;
#endif
}
