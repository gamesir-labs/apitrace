#include "apitrace/asset_index.hpp"
#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/trace_session.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
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

bool run_command_expect_failure(const std::string &command)
{
  const auto result = std::system(command.c_str());
  if (result == 0) {
    std::cerr << "command unexpectedly succeeded: " << command << "\n";
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

struct ScopedEnvVar {
  explicit ScopedEnvVar(const char *env_name, const char *value)
      : name(env_name)
  {
#ifdef _WIN32
    char *existing = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&existing, &size, name.c_str()) == 0 && existing) {
      had_previous = true;
      previous = existing;
      std::free(existing);
    }
#else
    if (const char *existing = std::getenv(name.c_str())) {
      had_previous = true;
      previous = existing;
    }
#endif
    set_env_var(name.c_str(), value);
  }

  ~ScopedEnvVar()
  {
    set_env_var(name.c_str(), had_previous ? previous.c_str() : nullptr);
  }

  std::string name;
  bool had_previous = false;
  std::string previous;
};

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

bool write_synthetic_trace_session_capture(const std::filesystem::path &bundle)
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
  buffer = session.stage_raw_asset(std::move(buffer));
  session.append_call_event(make_call_event(
      12,
      "ID3D12Resource::Unmap",
      "{\"resource_object_id\":200,\"subresource\":0,\"written_begin\":0,\"written_end\":4,\"written_size\":4,"
          "\"buffer_path\":\"" + buffer.relative_path.generic_string() + "\"}",
      {200},
      {buffer.blob_id}));

  apitrace::trace::AssetRecord vs;
  vs.blob_id = 78;
  vs.kind = apitrace::trace::AssetKind::ShaderDxbc;
  vs.debug_name = "raw-vs-bytecode";
  vs.payload_bytes = {'v', 's', '-', 'd', 'x', 'b', 'c'};
  vs = session.stage_raw_asset(std::move(vs));
  apitrace::trace::AssetRecord ps;
  ps.blob_id = 79;
  ps.kind = apitrace::trace::AssetKind::ShaderDxbc;
  ps.debug_name = "raw-ps-bytecode";
  ps.payload_bytes = {'p', 's', '-', 'd', 'x', 'b', 'c'};
  ps = session.stage_raw_asset(std::move(ps));
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
    if (!expect(session.raw_commit_cadence_bytes() == 512,
                "periodic raw commit test did not configure raw commit cadence")) {
      return false;
    }
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
    buffer = session.stage_raw_asset(std::move(buffer));
    session.append_call_event(make_call_event(
        200,
        "ID3D12Resource::Unmap",
        "{\"resource_object_id\":700,\"subresource\":0,\"written_begin\":0,\"written_end\":" +
            std::to_string(buffer.byte_size) + ",\"written_size\":" + std::to_string(buffer.byte_size) +
            ",\"buffer_path\":\"" + buffer.relative_path.generic_string() + "\"}",
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

std::string sha256_bytes(const std::string &text)
{
  return apitrace::trace::content_hash_bytes(text.data(), text.size());
}

std::string sha256_bytes(const std::vector<std::uint8_t> &bytes)
{
  return apitrace::trace::content_hash_bytes(bytes.empty() ? nullptr : bytes.data(), bytes.size());
}

std::string replace_all_copy(std::string text, std::string_view from, std::string_view to)
{
  if (from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to.data(), to.size());
    pos += to.size();
  }
  return text;
}

json sorted_json_array(json array, const std::vector<std::string> &keys)
{
  if (!array.is_array()) {
    return array;
  }
  std::sort(array.begin(), array.end(), [&](const json &lhs, const json &rhs) {
    for (const auto &key : keys) {
      const auto left = lhs.value(key, std::string());
      const auto right = rhs.value(key, std::string());
      if (left != right) {
        return left < right;
      }
    }
    return lhs.dump() < rhs.dump();
  });
  return array;
}

json normalized_assets_json(const std::filesystem::path &bundle)
{
  auto assets = json::parse(std::ifstream(bundle / "assets.json"), nullptr, false);
  if (!assets.is_discarded() && assets.contains("assets")) {
    assets["assets"] = sorted_json_array(assets["assets"], {"path", "kind", "content_hash"});
  }
  return assets;
}

json normalized_checksums_json(const std::filesystem::path &bundle)
{
  auto checksums = json::parse(std::ifstream(bundle / "checksums.json"), nullptr, false);
  if (!checksums.is_discarded()) {
    checksums.erase("bundle_hash");
    auto files = checksums.find("files");
    if (files != checksums.end() && files->is_object()) {
      files->erase("callstream.jsonl");
    }
  }
  return checksums;
}

std::vector<std::string> normalized_callstream_lines(const std::filesystem::path &bundle)
{
  std::vector<std::string> lines;
  std::ifstream input(bundle / "callstream.jsonl", std::ios::binary);
  std::string line;
  while (std::getline(input, line)) {
    auto record = json::parse(line, nullptr, false);
    if (!record.is_discarded()) {
      if (record.value("record_kind", std::string()) == "bundle_header") {
        record["time_origin_ns"] = 0;
        record["monotonic_origin_ns"] = 0;
      }
      if (record.contains("elapsed_ns")) {
        record["elapsed_ns"] = 0;
      }
      line = record.dump();
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

bool compare_asset_file_bytes(const std::filesystem::path &left, const std::filesystem::path &right)
{
  const auto assets = normalized_assets_json(left);
  if (!expect(!assets.is_discarded() && assets.contains("assets"), "left assets.json missing for equivalence compare")) {
    return false;
  }
  for (const auto &asset : assets["assets"]) {
    const auto path = asset.value("path", std::string());
    if (path.empty()) {
      continue;
    }
    if (!expect(read_file_bytes(left / path) == read_file_bytes(right / path),
                "streaming equivalence asset bytes mismatch")) {
      std::cerr << "asset path: " << path << "\n";
      return false;
    }
  }
  return true;
}

bool compare_finalized_bundles(const std::filesystem::path &left, const std::filesystem::path &right)
{
  if (!expect(normalized_assets_json(left) == normalized_assets_json(right),
              "streaming equivalence assets.json mismatch")) {
    return false;
  }
  const auto left_objects = left / "objects" / "objects.json";
  const auto right_objects = right / "objects" / "objects.json";
  if ((std::filesystem::exists(left_objects) || std::filesystem::exists(right_objects)) &&
      !expect(read_file_bytes(left_objects) == read_file_bytes(right_objects),
              "streaming equivalence object index mismatch")) {
    return false;
  }
  return compare_asset_file_bytes(left, right);
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

bool write_passthrough_blob_remap_collision_raw_capture(
    const std::filesystem::path &bundle,
    std::uint64_t provisional_blob_id,
    const std::string &provisional_asset_path,
    const std::string &passthrough_blob_line)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open passthrough remap raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::string seed_asset_path = "buffers/existing-collision.buffer";
  const std::string seed_line =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12Resource::Unmap\",\"result_code\":0,\"blob_refs\":[" +
      std::to_string(provisional_blob_id) +
      "],\"payload\":{\"blob_id\":" + std::to_string(provisional_blob_id) +
      ",\"resource_object_id\":200,\"subresource\":0,\"written_begin\":0,\"written_end\":3,\"written_size\":3"
      ",\"buffer_path\":\"" + seed_asset_path + "\"}}";
  const std::vector<std::uint8_t> seed_blob = {0x01, 0x02, 0x03};
  const std::vector<std::uint8_t> passthrough_blob = {0xaa, 0xbb, 0xcc, 0xdd};
  const auto seed_raw_blob_id = writer.append_blob(
      seed_blob.data(),
      seed_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      1);
  const auto raw_blob_id = writer.append_blob(
      passthrough_blob.data(),
      passthrough_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      2);
  if (!expect(seed_raw_blob_id != kInvalidRawBlobId && raw_blob_id != kInvalidRawBlobId,
              "failed to append passthrough remap bytes")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  PassthroughBlobDescriptor seed_descriptor;
  seed_descriptor.provisional_asset_path = seed_asset_path;
  seed_descriptor.final_blob_id = provisional_blob_id;
  seed_descriptor.raw_blob_id = seed_raw_blob_id;
  seed_descriptor.raw_blob_kind = static_cast<std::uint32_t>(RawBlobKind::Buffer);
  seed_descriptor.debug_name = "existing-collision-buffer";

  PassthroughBlobDescriptor descriptor;
  descriptor.provisional_asset_path = provisional_asset_path;
  descriptor.final_blob_id = provisional_blob_id;
  descriptor.raw_blob_id = raw_blob_id;
  descriptor.raw_blob_kind = static_cast<std::uint32_t>(RawBlobKind::Buffer);
  descriptor.debug_name = "passthrough-remapped-buffer";

  if (!append_raw_event(
          writer,
          1,
          RawEventOpcode::PassthroughWithBlob,
          encode_passthrough_with_blob_payload(seed_line, {seed_descriptor})) ||
      !append_raw_event(
          writer,
          2,
          RawEventOpcode::PassthroughWithBlob,
          encode_passthrough_with_blob_payload(passthrough_blob_line, {descriptor}))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit passthrough remap raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool write_duplicate_content_raw_capture(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open duplicate-content raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> duplicate_blob = {0xde, 0xad, 0xbe, 0xef};
  const std::vector<std::uint8_t> unique_blob = {0xca, 0xfe, 0xba, 0xbe};
  const auto duplicate_raw_id_a = writer.append_blob(
      duplicate_blob.data(),
      duplicate_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      1);
  const auto unique_raw_id = writer.append_blob(
      unique_blob.data(),
      unique_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      2);
  const auto duplicate_raw_id_b = writer.append_blob(
      duplicate_blob.data(),
      duplicate_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      3);
  if (!expect(duplicate_raw_id_a != kInvalidRawBlobId &&
                  unique_raw_id != kInvalidRawBlobId &&
                  duplicate_raw_id_b != kInvalidRawBlobId,
              "failed to append duplicate-content raw blobs")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!append_raw_event(
          writer,
          1,
          RawEventOpcode::ResourceCreate,
          encode_resource_create_payload(100, 200, 1, 4096, 1, 1, 1, 87, 0, 4, "resource-a")) ||
      !append_raw_event(
          writer,
          2,
          RawEventOpcode::ResourceCreate,
          encode_resource_create_payload(100, 201, 1, 4096, 1, 1, 1, 87, 0, 4, "resource-b")) ||
      !append_raw_event(
          writer,
          3,
          RawEventOpcode::ResourceCreate,
          encode_resource_create_payload(100, 202, 1, 4096, 1, 1, 1, 87, 0, 4, "resource-c")) ||
      !append_raw_event(
          writer,
          4,
          RawEventOpcode::ResourceUnmap,
          encode_resource_unmap_payload(200, duplicate_raw_id_a, 0, duplicate_blob.size())) ||
      !append_raw_event(
          writer,
          5,
          RawEventOpcode::ResourceUnmap,
          encode_resource_unmap_payload(201, unique_raw_id, 16, 16 + unique_blob.size())) ||
      !append_raw_event(
          writer,
          6,
          RawEventOpcode::ResourceUnmap,
          encode_resource_unmap_payload(202, duplicate_raw_id_b, 32, 32 + duplicate_blob.size()))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit duplicate-content raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool write_texture_unmap_raw_capture(
    const std::filesystem::path &bundle,
    bool include_prior_map)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open texture-unmap raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> texture_blob = {0x11, 0x22, 0x33, 0x44, 0x55};
  const auto texture_raw_id = writer.append_blob(
      texture_blob.data(),
      texture_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      2);
  if (!expect(texture_raw_id != kInvalidRawBlobId, "failed to append texture-unmap raw blob")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!append_raw_event(
          writer,
          1,
          RawEventOpcode::ResourceCreate,
          encode_resource_create_payload(100, 900, 2, 64, 64, 1, 1, 87, 0, 4, "texture-resource"))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  if (include_prior_map) {
    const std::string map_line =
        "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1002,\"elapsed_ns\":0,"
        "\"function\":\"ID3D12Resource::Map\",\"result_code\":0,\"object_refs\":[900],"
        "\"payload\":{\"subresource\":3,\"read_range\":null,\"mapped\":true}}";
    if (!append_raw_event(
            writer,
            2,
            RawEventOpcode::Passthrough,
            encode_passthrough_final_json_payload(map_line))) {
      std::cerr << writer.last_error() << "\n";
      return false;
    }
  }
  if (!append_raw_event(
          writer,
          include_prior_map ? 3 : 2,
          RawEventOpcode::ResourceUnmap,
          encode_resource_unmap_payload(900, texture_raw_id, 16, 16 + texture_blob.size()))) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit texture-unmap raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool write_streaming_equivalence_raw_capture(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open streaming-equivalence raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> duplicate_blob(4096, 0x5c);
  std::uint64_t sequence = 1;
  if (!append_raw_event(writer, sequence++, RawEventOpcode::FrameBegin, encode_frame_boundary_payload(0))) {
    return false;
  }
  for (std::uint64_t index = 0; index < 96; ++index) {
    const auto resource_id = 2000 + index;
    std::vector<std::uint8_t> blob;
    if (index % 3 == 0) {
      blob = duplicate_blob;
    } else {
      blob.assign(2048 + static_cast<std::size_t>(index % 7), static_cast<std::uint8_t>(index));
    }
    const auto raw_blob_id = writer.append_blob(
        blob.data(),
        blob.size(),
        static_cast<std::uint32_t>(RawBlobKind::Buffer),
        sequence + 1);
    if (!expect(raw_blob_id != kInvalidRawBlobId, "failed to append streaming-equivalence raw blob")) {
      return false;
    }
    if (!append_raw_event(
            writer,
            sequence++,
            RawEventOpcode::ResourceCreate,
            encode_resource_create_payload(
                100,
                resource_id,
                1,
                4096 + index,
                1,
                1,
                1,
                87,
                0,
                4,
                "streaming-resource-" + std::to_string(index))) ||
        !append_raw_event(
            writer,
            sequence++,
            RawEventOpcode::ResourceUnmap,
            encode_resource_unmap_payload(resource_id, raw_blob_id, index, index + blob.size())) ||
        !append_raw_event(
            writer,
            sequence++,
            RawEventOpcode::DrawInstanced,
            encode_draw_instanced_payload(500, 3, 1, static_cast<std::uint32_t>(index), 0))) {
      std::cerr << writer.last_error() << "\n";
      return false;
    }
  }
  if (!append_raw_event(writer, sequence++, RawEventOpcode::PresentCall, encode_present_payload(600, 0, 1, 0)) ||
      !append_raw_event(writer, sequence++, RawEventOpcode::PresentBoundary, encode_present_payload(600, 0, 1, 0)) ||
      !append_raw_event(writer, sequence++, RawEventOpcode::FrameEnd, encode_frame_boundary_payload(0))) {
    return false;
  }

  if (!expect(writer.flush_commit(), "failed to commit streaming-equivalence raw capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();
  return true;
}

bool validate_raw_blob_assets_are_path_backed(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open path-backed raw capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }

  std::unordered_map<std::uint64_t, RawBlobExtent> extent_by_id;
  for (const auto &extent : reader.blob_extents()) {
    extent_by_id.emplace(extent.raw_blob_id, extent);
  }

  RawEventDecoder decoder(reader);
  bool saw_path_backed_asset = false;
  const bool streamed = reader.for_each_event([&](RawEventRecord &&record) {
    DecodedRawEvent decoded;
    if (!decoder.decode_event(record, decoded)) {
      std::cerr << decoder.last_error() << "\n";
      return false;
    }
    for (const auto &asset : decoded.assets) {
      const auto extent_it = extent_by_id.find(asset.blob_id);
      if (!expect(extent_it != extent_by_id.end(), "decoded asset blob id missing from raw blob index") ||
          !expect(asset.payload_bytes.empty(), "decoded raw asset retained payload bytes") ||
          !expect(asset.payload_path.generic_string() == "raw/blobs.bin", "decoded raw asset payload path mismatch") ||
          !expect(asset.payload_offset == extent_it->second.offset, "decoded raw asset payload offset mismatch") ||
          !expect(asset.byte_size == extent_it->second.size, "decoded raw asset size mismatch")) {
        return false;
      }
      saw_path_backed_asset = true;
    }
    return true;
  });
  return expect(streamed, "failed to stream path-backed raw capture") &&
         expect(saw_path_backed_asset, "path-backed raw capture did not decode any assets");
}

bool load_asset_payload_bytes_for_legacy_baseline(
    const std::filesystem::path &bundle,
    apitrace::trace::AssetRecord &asset)
{
  if (!asset.payload_bytes.empty() || asset.payload_path.empty()) {
    return true;
  }
  std::ifstream input(bundle / asset.payload_path, std::ios::binary);
  if (!expect(input.is_open(), "legacy baseline failed to open payload source")) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(asset.payload_offset), std::ios::beg);
  if (!expect(input.good(), "legacy baseline failed to seek payload source")) {
    return false;
  }
  if (!expect(asset.byte_size <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()),
              "legacy baseline payload too large for test")) {
    return false;
  }
  asset.payload_bytes.resize(static_cast<std::size_t>(asset.byte_size));
  if (!asset.payload_bytes.empty()) {
    input.read(reinterpret_cast<char *>(asset.payload_bytes.data()), static_cast<std::streamsize>(asset.payload_bytes.size()));
    if (!expect(static_cast<std::size_t>(input.gcount()) == asset.payload_bytes.size(),
                "legacy baseline short payload read")) {
      asset.payload_bytes.clear();
      return false;
    }
  }
  asset.payload_path.clear();
  asset.payload_offset = 0;
  return true;
}

bool validate_final_bundle(const std::filesystem::path &bundle)
{
  const auto records = read_jsonl(bundle / "callstream.jsonl");
  if (!expect(records.size() == 5, "unexpected callstream record count")) {
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
  if (!expect(functions[0] == "ID3D12GraphicsCommandList::SetPipelineState" &&
                  functions[1] == "ID3D12Resource::Unmap" &&
                  functions[2] == "ID3D12Device::CreateGraphicsPipelineState" &&
                  functions[3] == "ID3D12GraphicsCommandList::DrawInstanced",
              "raw-to-final callstream shape mismatch")) {
    return false;
  }

  const auto &unmap = records[2];
  if (!expect(unmap["blob_refs"].is_array() && unmap["blob_refs"].size() == 1, "Unmap should reference one buffer blob")) {
    return false;
  }
  const auto buffer_blob_id = unmap["blob_refs"][0].get<std::uint64_t>();
  const auto buffer_path = unmap["payload"].value("buffer_path", std::string());
  if (!expect(buffer_path.rfind("buffers/", 0) == 0 && buffer_path.find("asset-") == std::string::npos,
              "Unmap buffer path was not canonicalized")) {
    return false;
  }
  const auto &unmap_payload = unmap["payload"];
  if (!expect(unmap_payload.value("subresource", UINT64_MAX) == 0 &&
                  unmap_payload.value("written_begin", UINT64_MAX) == 0 &&
                  unmap_payload.value("written_end", 0ull) == 4 &&
                  unmap_payload.value("written_size", 0ull) == 4 &&
                  !unmap_payload.contains("written_range"),
              "Unmap flat payload shape mismatch")) {
    return false;
  }

  const auto &pipeline = records[3];
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
  if (!expect(read_file_bytes(bundle / buffer_asset_path) == std::vector<std::uint8_t>({0x01, 0x02, 0x03, 0x04}),
              "canonical buffer asset bytes mismatch")) {
    return false;
  }
  return true;
}

bool validate_texture_unmap_raw_bundle(const std::filesystem::path &bundle)
{
  const auto records = read_jsonl(bundle / "callstream.jsonl");
  std::vector<json> unmaps;
  for (const auto &record : records) {
    if (record.value("function", std::string()) == "ID3D12Resource::Unmap") {
      unmaps.push_back(record);
    }
  }
  if (!expect(unmaps.size() == 1, "texture-unmap bundle should contain one Unmap")) {
    return false;
  }
  const auto &payload = unmaps.front()["payload"];
  if (!expect(payload.value("resource_object_id", 0ull) == 900 &&
                  payload.value("subresource", UINT64_MAX) == 3 &&
                  payload.value("written_begin", UINT64_MAX) == 16 &&
                  payload.value("written_end", 0ull) == 21 &&
                  payload.value("written_size", 0ull) == 5 &&
                  payload.value("buffer_path", std::string()).rfind("buffers/", 0) == 0 &&
                  !payload.contains("written_range"),
              "texture-unmap flat payload shape mismatch")) {
    return false;
  }
  return true;
}

bool legacy_materialize_raw_capture_to_final_bundle(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace;
  using namespace apitrace::trace::raw;

  RawCaptureReader reader;
  if (!expect(reader.open(bundle), "legacy materializer failed to open raw capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }
  const auto raw_events = reader.read_events();
  const auto decoded = decode_raw_events(reader, raw_events);
  if (!expect(decoded.error.empty(), "legacy materializer failed to decode raw capture")) {
    std::cerr << decoded.error << "\n";
    return false;
  }

  TraceBundleWriter writer;
  if (!expect(writer.open(bundle), "legacy materializer failed to open bundle writer")) {
    return false;
  }
  writer.write_metadata({ApiKind::D3D12, kFormatVersion, "raw-to-final", false});
  for (const auto &decoded_event : decoded.events) {
    if (decoded_event.passthrough) {
      for (const auto &asset : decoded_event.assets) {
        auto legacy_asset = asset;
        if (!load_asset_payload_bytes_for_legacy_baseline(bundle, legacy_asset)) {
          return false;
        }
        writer.register_asset(std::move(legacy_asset));
      }
      writer.append_callstream_json_line(decoded_event.passthrough_jsonl_record);
      continue;
    }
    auto event = decoded_event.event;
    for (const auto &asset : decoded_event.assets) {
      auto legacy_asset = asset;
      if (!load_asset_payload_bytes_for_legacy_baseline(bundle, legacy_asset)) {
        return false;
      }
      const auto input_blob_id = legacy_asset.blob_id;
      const auto input_relative_path = legacy_asset.relative_path.generic_string();
      auto registered = writer.register_asset(std::move(legacy_asset));
      if (registered.blob_id != input_blob_id) {
        for (auto &blob_id : event.blob_refs) {
          if (blob_id == input_blob_id) {
            blob_id = registered.blob_id;
          }
        }
      }
      const auto registered_relative_path = registered.relative_path.generic_string();
      if (!input_relative_path.empty() && input_relative_path != registered_relative_path) {
        event.payload = replace_all_copy(event.payload, input_relative_path, registered_relative_path);
      }
    }
    writer.append_call_event(std::move(event));
  }
  writer.close();
  return true;
}

bool validate_duplicate_content_raw_bundle(const std::filesystem::path &bundle)
{
  const auto records = read_jsonl(bundle / "callstream.jsonl");
  if (!expect(records.size() == 7, "unexpected duplicate-content callstream record count")) {
    return false;
  }

  std::vector<json> unmaps;
  for (const auto &record : records) {
    if (record.value("function", std::string()) == "ID3D12Resource::Unmap") {
      unmaps.push_back(record);
    }
  }
  if (!expect(unmaps.size() == 3, "duplicate-content raw finalize suppressed Unmap events")) {
    return false;
  }

  const auto duplicate_path_a = unmaps[0]["payload"].value("buffer_path", std::string());
  const auto unique_path = unmaps[1]["payload"].value("buffer_path", std::string());
  const auto duplicate_path_b = unmaps[2]["payload"].value("buffer_path", std::string());
  if (!expect(!duplicate_path_a.empty() &&
                  duplicate_path_a == duplicate_path_b &&
                  duplicate_path_a != unique_path,
              "duplicate-content raw blobs were not rewritten to one canonical asset path")) {
    return false;
  }
  if (!expect(duplicate_path_a.rfind("buffers/", 0) == 0 &&
                  duplicate_path_a.find("asset-") == std::string::npos,
              "duplicate-content buffer path was not canonicalized")) {
    return false;
  }
  if (!expect(std::filesystem::exists(bundle / duplicate_path_a) &&
                  std::filesystem::exists(bundle / unique_path),
              "deduped canonical buffer asset files missing")) {
    return false;
  }

  const auto assets_json = json::parse(std::ifstream(bundle / "assets.json"), nullptr, false);
  if (!expect(!assets_json.is_discarded() && assets_json.contains("assets"), "duplicate-content assets.json missing")) {
    return false;
  }

  std::size_t duplicate_path_entries = 0;
  std::size_t unique_path_entries = 0;
  std::string duplicate_hash;
  std::string unique_hash;
  for (const auto &asset : assets_json["assets"]) {
    const auto path = asset.value("path", std::string());
    if (path == duplicate_path_a) {
      ++duplicate_path_entries;
      duplicate_hash = asset.value("content_hash", std::string());
    }
    if (path == unique_path) {
      ++unique_path_entries;
      unique_hash = asset.value("content_hash", std::string());
    }
  }
  if (!expect(duplicate_path_entries == 2 &&
                  unique_path_entries == 1 &&
                  !duplicate_hash.empty() &&
                  !unique_hash.empty() &&
                  duplicate_hash != unique_hash,
              "assets.json did not preserve deduped blob references on canonical files")) {
    return false;
  }
  if (!expect(read_file_bytes(bundle / duplicate_path_a) == std::vector<std::uint8_t>({0xde, 0xad, 0xbe, 0xef}) &&
                  read_file_bytes(bundle / unique_path) == std::vector<std::uint8_t>({0xca, 0xfe, 0xba, 0xbe}),
              "deduped canonical buffer asset bytes mismatch")) {
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

bool validate_passthrough_blob_remap_bundle(
    const std::filesystem::path &bundle,
    std::uint64_t provisional_blob_id,
    const std::string &provisional_asset_path,
    bool expect_metal_payload)
{
  const auto records = read_jsonl(bundle / "callstream.jsonl");
  if (!expect(records.size() == 3, "unexpected passthrough remap callstream record count")) {
    return false;
  }
  const auto &seed = records[1];
  if (!expect(seed["blob_refs"].is_array() &&
                  seed["blob_refs"].size() == 1 &&
                  seed["blob_refs"][0].get<std::uint64_t>() == provisional_blob_id,
              "passthrough remap seed event did not keep provisional blob id")) {
    return false;
  }
  const auto &event = records[2];
  if (!expect(event["blob_refs"].is_array() && event["blob_refs"].size() == 1,
              "passthrough remap event missing one blob ref")) {
    return false;
  }
  const auto published_blob_id = event["blob_refs"][0].get<std::uint64_t>();
  if (!expect(published_blob_id != provisional_blob_id,
              "passthrough remap event kept dangling provisional blob id")) {
    return false;
  }

  const auto payload = event.value("payload", json::object());
  if (expect_metal_payload) {
    if (!expect(event.value("function", std::string()) == "MTLBuffer.updateContents",
                "metal passthrough remap function mismatch") ||
        !expect(payload.value("blob_id", 0ull) == published_blob_id,
                "metal passthrough payload blob_id was not remapped") ||
        !expect(payload.value("data_path", std::string()) != provisional_asset_path,
                "metal passthrough payload path kept provisional asset path")) {
      return false;
    }
  } else {
    if (!expect(event.value("function", std::string()) == "ID3D12Resource::Unmap",
                "D3D12 passthrough remap function mismatch") ||
        !expect(payload.value("blob_id", 0ull) == published_blob_id,
                "D3D12 passthrough payload blob_id was not remapped") ||
        !expect(payload.value("buffer_path", std::string()) != provisional_asset_path,
                "D3D12 passthrough payload path kept provisional asset path")) {
      return false;
    }
  }

  const auto assets_json = json::parse(std::ifstream(bundle / "assets.json"), nullptr, false);
  if (!expect(!assets_json.is_discarded() && assets_json.contains("assets"),
              "passthrough remap assets.json missing")) {
    return false;
  }
  std::string published_path;
  bool saw_seed_blob = false;
  bool saw_provisional_blob = false;
  for (const auto &asset : assets_json["assets"]) {
    const auto blob_id = asset.value("blob_id", 0ull);
    const auto path = asset.value("path", std::string());
    if (blob_id == published_blob_id) {
      published_path = path;
    }
    if (blob_id == provisional_blob_id) {
      saw_seed_blob = true;
    }
    if (blob_id == provisional_blob_id && path == provisional_asset_path) {
      saw_provisional_blob = true;
    }
  }
  if (!expect(saw_seed_blob, "passthrough remap collision seed asset missing") ||
      !expect(!saw_provisional_blob, "passthrough remap published asset kept provisional blob id") ||
      !expect(!published_path.empty(), "passthrough remap published asset missing from index") ||
      !expect(read_file_bytes(bundle / published_path) == std::vector<std::uint8_t>({0xaa, 0xbb, 0xcc, 0xdd}),
              "passthrough remap published asset bytes mismatch")) {
    return false;
  }
  return true;
}

bool run_streaming_equivalence_test(
    const std::filesystem::path &source_bundle,
    const std::filesystem::path &legacy_bundle,
    const std::filesystem::path &streaming_bundle,
    const char *finalize,
    const char *check)
{
  return write_streaming_equivalence_raw_capture(source_bundle) &&
         validate_raw_blob_assets_are_path_backed(source_bundle) &&
         copy_directory(source_bundle, legacy_bundle) &&
         copy_directory(source_bundle, streaming_bundle) &&
         legacy_materialize_raw_capture_to_final_bundle(legacy_bundle) &&
         run_command(quote_arg(finalize) + " --no-progress --jobs 1 " + quote_arg(legacy_bundle)) &&
         run_command(quote_arg(finalize) + " --raw-format --no-progress --jobs 1 " + quote_arg(streaming_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(legacy_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(streaming_bundle)) &&
         compare_finalized_bundles(legacy_bundle, streaming_bundle);
}

bool write_sequence_regression_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  std::filesystem::create_directories(bundle);
  std::ofstream metadata(bundle / "metadata.json", std::ios::binary | std::ios::trunc);
  metadata << "{\"schema\":\"apitrace.bundle.v1\",\"api\":\"d3d12\",\"created_by\":\"sequence-regression-test\","
              "\"version\":1,\"entry_file\":\"callstream.jsonl\"}\n";
  metadata.close();

  std::ofstream callstream(bundle / "callstream.jsonl", std::ios::binary | std::ios::trunc);
  callstream
      << "{\"record_kind\":\"bundle_header\",\"sequence\":1,\"time_ns\":1001,\"payload\":{\"label\":\"header\"}}\n"
      << "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1002,\"function\":\"ID3D12GraphicsCommandList::DrawInstanced\",\"result_code\":0,\"payload\":{\"records\":[[2,7]],\"columns\":[\"sequence\",\"value\"],\"nested\":{\"d3d_sequence\":2}}}\n"
      << "{\"record_kind\":\"boundary\",\"sequence\":3,\"time_ns\":1003,\"boundary\":\"DebugMarker\",\"payload\":{\"label\":\"before-reset\"}}\n"
      << "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1004,\"function\":\"ID3D12GraphicsCommandList::Dispatch\",\"result_code\":0,\"payload\":{\"ops\":[[1,8]],\"columns\":[\"d3d_sequence\",\"value\"],\"sequence\":1}}\n"
      << "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1005,\"function\":\"ID3D12GraphicsCommandList::SetPipelineState\",\"result_code\":0,\"payload\":{\"label\":\"after-reset\",\"nested\":{\"sequence\":2}}}\n"
      << "{\"record_kind\":\"boundary\",\"sequence\":1,\"time_ns\":1006,\"boundary\":\"DebugMarker\",\"payload\":{\"records\":[[1]],\"columns\":[\"sequence\"]}}\n"
      << "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1007,\"function\":\"ID3D12GraphicsCommandList::DrawInstanced\",\"result_code\":0,\"payload\":{\"label\":\"tail\",\"array\":[{\"sequence\":2}]}}\n";
  callstream.close();

  std::ofstream checksums(bundle / "checksums.json", std::ios::binary | std::ios::trunc);
  checksums << "{\"schema\":\"apitrace.checksums.v1\",\"files\":{}}\n";
  return true;
}

bool write_reference_rewrite_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  std::filesystem::create_directories(bundle / "buffers");

  const std::vector<std::uint8_t> seed_blob = {0x01, 0x02, 0x03};
  const std::vector<std::uint8_t> remapped_blob = {0xaa, 0xbb, 0xcc, 0xdd};
  const auto seed_hash = sha256_bytes(seed_blob);
  const auto remapped_hash = sha256_bytes(remapped_blob);
  const std::string seed_path = "buffers/" + seed_hash + ".buffer";
  const std::string remapped_path = "buffers/" + remapped_hash + ".buffer";

  {
    std::ofstream output(bundle / seed_path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(seed_blob.data()), static_cast<std::streamsize>(seed_blob.size()));
  }
  {
    std::ofstream output(bundle / remapped_path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(remapped_blob.data()), static_cast<std::streamsize>(remapped_blob.size()));
  }

  const std::uint64_t colliding_blob_id = 4462313;
  const std::string metadata =
      "{\"schema\":\"apitrace.bundle.v1\",\"api\":\"d3d12\",\"created_by\":\"reference-rewrite-test\","
      "\"version\":1,\"entry_file\":\"callstream.jsonl\"}\n";
  const std::string assets =
      "{\n"
      "  \"assets\": [\n"
      "    {\n"
      "      \"binary_payload\": true,\n"
      "      \"blob_id\": " + std::to_string(colliding_blob_id) + ",\n"
      "      \"byte_size\": " + std::to_string(seed_blob.size()) + ",\n"
      "      \"content_hash\": \"" + seed_hash + "\",\n"
      "      \"debug_name\": \"reference-rewrite-seed\",\n"
      "      \"kind\": \"Buffer\",\n"
      "      \"metal\": false,\n"
      "      \"path\": \"" + seed_path + "\"\n"
      "    },\n"
      "    {\n"
      "      \"binary_payload\": true,\n"
      "      \"blob_id\": " + std::to_string(colliding_blob_id) + ",\n"
      "      \"byte_size\": " + std::to_string(remapped_blob.size()) + ",\n"
      "      \"content_hash\": \"" + remapped_hash + "\",\n"
      "      \"debug_name\": \"reference-rewrite-remapped\",\n"
      "      \"kind\": \"Buffer\",\n"
      "      \"metal\": false,\n"
      "      \"path\": \"" + remapped_path + "\"\n"
      "    }\n"
      "  ]\n"
      "}\n";
  const std::string callstream =
      "{\"record_kind\":\"bundle_header\",\"format_version\":1,\"api\":\"D3D12\",\"producer\":\"reference-rewrite-test\",\"has_metal_callstream\":false,\"time_origin_ns\":1000,\"monotonic_origin_ns\":2000,\"entry_file\":\"callstream.jsonl\"}\n"
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12Resource::Unmap\",\"result_code\":0,\"blob_refs\":[" +
      std::to_string(colliding_blob_id) +
      "],\"payload\":{\"blob_id\":" + std::to_string(colliding_blob_id) +
      ",\"buffer_path\":\"" + seed_path +
      "\",\"resource_object_id\":200,\"subresource\":0,\"written_begin\":0,\"written_end\":3,\"written_size\":3}}\n"
      "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1002,\"elapsed_ns\":0,\"function\":\"ID3D12Resource::Unmap\",\"result_code\":0,\"blob_refs\":[" +
      std::to_string(colliding_blob_id) +
      "],\"payload\":{\"blob_id\":" + std::to_string(colliding_blob_id) +
      ",\"buffer_path\":\"" + remapped_path +
      "\",\"nested\":{\"blob_id\":" + std::to_string(colliding_blob_id) +
      "},\"resource_object_id\":201,\"subresource\":0,\"written_begin\":0,\"written_end\":4,\"written_size\":4}}\n";
  const std::string checksums =
      "{\"schema\":\"apitrace.checksums.v1\",\"files\":{"
      "\"assets.json\":\"sha256:" + sha256_bytes(assets) + ":" + std::to_string(assets.size()) + "\","
      "\"callstream.jsonl\":\"sha256:" + sha256_bytes(callstream) + ":" + std::to_string(callstream.size()) + "\","
      "\"metadata.json\":\"sha256:" + sha256_bytes(metadata) + ":" + std::to_string(metadata.size()) + "\","
      "\"" + seed_path + "\":\"sha256:" + seed_hash + ":" + std::to_string(seed_blob.size()) + "\","
      "\"" + remapped_path + "\":\"sha256:" + remapped_hash + ":" + std::to_string(remapped_blob.size()) + "\""
      "}}\n";

  std::ofstream(bundle / "metadata.json", std::ios::binary | std::ios::trunc) << metadata;
  std::ofstream(bundle / "assets.json", std::ios::binary | std::ios::trunc) << assets;
  std::ofstream(bundle / "callstream.jsonl", std::ios::binary | std::ios::trunc) << callstream;
  std::ofstream(bundle / "checksums.json", std::ios::binary | std::ios::trunc) << checksums;
  return true;
}

bool run_sequence_repair_parallel_parity_test(
    const std::filesystem::path &source_bundle,
    const std::filesystem::path &serial_bundle,
    const std::filesystem::path &parallel_bundle,
    const char *finalize,
    const char *check)
{
  ScopedEnvVar forced_chunk_size("APITRACE_TEST_SEQUENCE_REPAIR_CHUNK_BYTES", "160");
  return write_sequence_regression_bundle(source_bundle) &&
         copy_directory(source_bundle, serial_bundle) &&
         copy_directory(source_bundle, parallel_bundle) &&
         run_command(quote_arg(finalize) + " --no-progress --jobs 1 " + quote_arg(serial_bundle)) &&
         run_command(quote_arg(finalize) + " --no-progress --jobs 4 " + quote_arg(parallel_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(serial_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(parallel_bundle)) &&
         expect(read_file_bytes(serial_bundle / "callstream.jsonl") ==
                    read_file_bytes(parallel_bundle / "callstream.jsonl"),
                "sequence repair serial/parallel callstream mismatch");
}

bool run_reference_rewrite_parallel_parity_test(
    const std::filesystem::path &source_bundle,
    const std::filesystem::path &serial_bundle,
    const std::filesystem::path &parallel_bundle,
    const char *finalize,
    const char *check)
{
  ScopedEnvVar forced_chunk_size("APITRACE_TEST_REFERENCE_REWRITE_CHUNK_BYTES", "180");
  return write_reference_rewrite_bundle(source_bundle) &&
         copy_directory(source_bundle, serial_bundle) &&
         copy_directory(source_bundle, parallel_bundle) &&
         run_command(quote_arg(finalize) + " --no-progress --jobs 1 " + quote_arg(serial_bundle)) &&
         run_command(quote_arg(finalize) + " --no-progress --jobs 4 " + quote_arg(parallel_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(serial_bundle)) &&
         run_command(quote_arg(check) + " --verify-hashes " + quote_arg(parallel_bundle)) &&
         expect(read_file_bytes(serial_bundle / "callstream.jsonl") ==
                    read_file_bytes(parallel_bundle / "callstream.jsonl"),
                "reference rewrite serial/parallel callstream mismatch");
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
  const auto passthrough_d3d12_remap_bundle = work_dir / "synthetic-passthrough-d3d12-remap.apitrace";
  const auto passthrough_metal_remap_bundle = work_dir / "synthetic-passthrough-metal-remap.apitrace";
  const auto duplicate_content_bundle = work_dir / "synthetic-duplicate-content-raw.apitrace";
  const auto no_end_raw_bundle = work_dir / "synthetic-no-end-raw.apitrace";
  const auto texture_unmap_bundle = work_dir / "synthetic-texture-unmap-raw.apitrace";
  const auto texture_unmap_missing_map_bundle = work_dir / "synthetic-texture-unmap-missing-map-raw.apitrace";
  const auto streaming_equivalence_bundle = work_dir / "synthetic-streaming-equivalence.apitrace";
  const auto streaming_equivalence_legacy_bundle = work_dir / "synthetic-streaming-equivalence-legacy.apitrace";
  const auto streaming_equivalence_raw_bundle = work_dir / "synthetic-streaming-equivalence-raw.apitrace";
  const auto sequence_regression_source_bundle = work_dir / "synthetic-sequence-regression-source.apitrace";
  const auto sequence_regression_serial_bundle = work_dir / "synthetic-sequence-regression-serial.apitrace";
  const auto sequence_regression_parallel_bundle = work_dir / "synthetic-sequence-regression-parallel.apitrace";
  const auto reference_rewrite_source_bundle = work_dir / "synthetic-reference-rewrite-source.apitrace";
  const auto reference_rewrite_serial_bundle = work_dir / "synthetic-reference-rewrite-serial.apitrace";
  const auto reference_rewrite_parallel_bundle = work_dir / "synthetic-reference-rewrite-parallel.apitrace";
  const std::string passthrough_before =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12GraphicsCommandList::SetPipelineState\",\"result_code\":0,\"object_refs\":[500,700],\"payload\":{\"state\":\"passthrough-before\",\"spacing\":\"kept exactly\"}}";
  const std::string passthrough_after =
      "{\"record_kind\":\"boundary\",\"sequence\":3,\"time_ns\":1003,\"elapsed_ns\":0,\"boundary\":\"DebugMarker\",\"payload\":{\"label\":\"passthrough-after\",\"nested\":{\"value\":42}}}";
  const std::string passthrough_blob_line =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12Resource::Unmap\",\"result_code\":0,\"object_refs\":[200],\"blob_refs\":[77],\"payload\":{\"resource_object_id\":200,\"subresource\":0,\"written_begin\":0,\"written_end\":4,\"written_size\":4,\"buffer_path\":\"buffers/asset-000000000000004d.buffer\"}}";
  const std::string passthrough_non_blob_line =
      "{\"record_kind\":\"call\",\"sequence\":2,\"time_ns\":1002,\"elapsed_ns\":0,\"function\":\"ID3D12GraphicsCommandList::DrawInstanced\",\"result_code\":0,\"object_refs\":[500],\"payload\":{\"vertex_count_per_instance\":3,\"instance_count\":1,\"start_vertex_location\":0,\"start_instance_location\":0}}";
  const std::uint64_t remap_collision_blob_id = 4462313;
  const std::string remap_provisional_path = "buffers/asset-00000000004462313.buffer";
  const std::string passthrough_d3d12_remap_line =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"ID3D12Resource::Unmap\",\"result_code\":0,\"blob_refs\":[" +
      std::to_string(remap_collision_blob_id) +
      "],\"payload\":{\"blob_id\":" + std::to_string(remap_collision_blob_id) +
      ",\"resource_object_id\":200,\"subresource\":0,\"written_begin\":0,\"written_end\":4,\"written_size\":4" +
      ",\"buffer_path\":\"" + remap_provisional_path +
      "\",\"nested\":{\"blob_id\":" + std::to_string(remap_collision_blob_id) +
      "}}}";
  const std::string passthrough_metal_remap_line =
      "{\"record_kind\":\"call\",\"sequence\":1,\"time_ns\":1001,\"elapsed_ns\":0,\"function\":\"MTLBuffer.updateContents\",\"result_code\":0,\"blob_refs\":[" +
      std::to_string(remap_collision_blob_id) +
      "],\"payload\":{\"blob_id\":" + std::to_string(remap_collision_blob_id) +
      ",\"data_path\":\"" + remap_provisional_path +
      "\",\"bytes\":{\"blob_id\":" + std::to_string(remap_collision_blob_id) +
      "},\"range\":{\"offset\":0,\"length\":4}}}";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const bool ok =
      write_synthetic_trace_session_capture(bundle) &&
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
      write_passthrough_blob_remap_collision_raw_capture(
          passthrough_d3d12_remap_bundle,
          remap_collision_blob_id,
          remap_provisional_path,
          passthrough_d3d12_remap_line) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress --jobs 1 " + quote_arg(passthrough_d3d12_remap_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(passthrough_d3d12_remap_bundle)) &&
      validate_passthrough_blob_remap_bundle(
          passthrough_d3d12_remap_bundle,
          remap_collision_blob_id,
          remap_provisional_path,
          false) &&
      write_passthrough_blob_remap_collision_raw_capture(
          passthrough_metal_remap_bundle,
          remap_collision_blob_id,
          remap_provisional_path,
          passthrough_metal_remap_line) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress --jobs 1 " + quote_arg(passthrough_metal_remap_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(passthrough_metal_remap_bundle)) &&
      validate_passthrough_blob_remap_bundle(
          passthrough_metal_remap_bundle,
          remap_collision_blob_id,
          remap_provisional_path,
          true) &&
      write_duplicate_content_raw_capture(duplicate_content_bundle) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress " + quote_arg(duplicate_content_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(duplicate_content_bundle)) &&
      validate_duplicate_content_raw_bundle(duplicate_content_bundle) &&
      write_no_end_periodic_commit_capture(no_end_raw_bundle) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress --jobs 1 " + quote_arg(no_end_raw_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(no_end_raw_bundle)) &&
      write_texture_unmap_raw_capture(texture_unmap_bundle, true) &&
      run_command(quote_arg(argv[1]) + " --raw-format --no-progress --jobs 1 " + quote_arg(texture_unmap_bundle)) &&
      run_command(quote_arg(argv[2]) + " --verify-hashes " + quote_arg(texture_unmap_bundle)) &&
      validate_texture_unmap_raw_bundle(texture_unmap_bundle) &&
      write_texture_unmap_raw_capture(texture_unmap_missing_map_bundle, false) &&
      run_command_expect_failure(
          quote_arg(argv[1]) + " --raw-format --no-progress --jobs 1 " + quote_arg(texture_unmap_missing_map_bundle)) &&
      run_streaming_equivalence_test(
          streaming_equivalence_bundle,
          streaming_equivalence_legacy_bundle,
          streaming_equivalence_raw_bundle,
          argv[1],
          argv[2]) &&
      run_sequence_repair_parallel_parity_test(
          sequence_regression_source_bundle,
          sequence_regression_serial_bundle,
          sequence_regression_parallel_bundle,
          argv[1],
          argv[2]) &&
      run_reference_rewrite_parallel_parity_test(
          reference_rewrite_source_bundle,
          reference_rewrite_serial_bundle,
          reference_rewrite_parallel_bundle,
          argv[1],
          argv[2]);

  if (ok) {
    std::filesystem::remove_all(work_dir);
  } else {
    std::cerr << "preserving work_dir=" << work_dir << "\n";
  }
  if (!ok) {
    return 1;
  }
  std::cout << "raw_to_final synthetic e2e test passed\n";
  return 0;
#endif
}
