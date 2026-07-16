#include "apitrace/event_types.hpp"
#include "apitrace/asset_index.hpp"
#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace apitrace::tools {
bool test_hook_bundle_finalize_reference_collection_matches_two_pass(
    const std::filesystem::path &bundle_root,
    std::size_t *path_ref_count,
    std::size_t *blob_id_ref_count);
bool test_hook_hash_assets_shared_payload_slices(
    const std::filesystem::path &bundle_root,
    std::size_t jobs,
    std::size_t *payload_slice_tasks,
    std::size_t *planned_payload_threads,
    std::vector<std::string> *content_hashes);
bool test_hook_repair_finalized_single_blob_asset_index(
    const std::filesystem::path &bundle_root,
    std::uint64_t expected_blob_id,
    const std::string &expected_path);
} // namespace apitrace::tools

namespace {

using json = nlohmann::json;

void set_env_var(const char *name, const char *value)
{
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unset_env_var(const char *name)
{
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string shell_quote_path(const std::filesystem::path &path)
{
#ifdef _WIN32
  std::string quoted = "\"";
  for (const char ch : path.string()) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += "\"";
#else
  std::string quoted = "'";
  for (const char ch : path.string()) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
#endif
  return quoted;
}

int run_shell_command(const std::string &command)
{
#ifdef _WIN32
  const auto shell_command = "\"" + command + "\"";
  return std::system(shell_command.c_str());
#else
  return std::system(command.c_str());
#endif
}

int run_tool(const std::filesystem::path &tool, const std::filesystem::path &bundle)
{
  return run_shell_command(shell_quote_path(tool) + " --no-progress " + shell_quote_path(bundle));
}

int run_bundle_check(const std::filesystem::path &bundle_check, const std::filesystem::path &bundle)
{
  return run_shell_command(shell_quote_path(bundle_check) + " " + shell_quote_path(bundle));
}

int run_bundle_finalize_with_threshold(
    const std::filesystem::path &bundle_finalize,
    const std::filesystem::path &bundle,
    const char *threshold,
    const std::filesystem::path &stderr_path = {})
{
  set_env_var("DXMT_FINALIZE_MAX_TRUNCATE_FRAMES", threshold);
  auto command = shell_quote_path(bundle_finalize) + " --no-progress " + shell_quote_path(bundle);
  if (!stderr_path.empty()) {
    command += " 2> " + shell_quote_path(stderr_path);
  }
  const auto status = run_shell_command(command);
  unset_env_var("DXMT_FINALIZE_MAX_TRUNCATE_FRAMES");
  return status;
}

void write_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void append_present_frame(
    apitrace::trace::TraceBundleWriter &writer,
    std::uint64_t frame_index,
    std::uint64_t &sequence)
{
  apitrace::trace::EventRecord frame_begin;
  frame_begin.kind = apitrace::trace::EventKind::Boundary;
  frame_begin.boundary = apitrace::trace::BoundaryKind::Frame;
  frame_begin.callsite.sequence = sequence++;
  frame_begin.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"label\":\"FrameBegin\"}";
  writer.append_call_event(frame_begin);

  apitrace::trace::EventRecord present_call;
  present_call.kind = apitrace::trace::EventKind::Call;
  present_call.callsite.sequence = sequence++;
  present_call.callsite.function_name = "IDXGISwapChain::Present";
  present_call.callsite.result_code = 0;
  present_call.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(present_call);

  apitrace::trace::EventRecord present_boundary;
  present_boundary.kind = apitrace::trace::EventKind::Boundary;
  present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
  present_boundary.callsite.sequence = sequence++;
  present_boundary.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(present_boundary);

  apitrace::trace::EventRecord frame_end;
  frame_end.kind = apitrace::trace::EventKind::Boundary;
  frame_end.boundary = apitrace::trace::BoundaryKind::Frame;
  frame_end.callsite.sequence = sequence++;
  frame_end.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"label\":\"FrameEnd\"}";
  writer.append_call_event(frame_end);
}

bool write_missing_blob_materialized_fixture(
    const std::filesystem::path &bundle,
    std::uint64_t frames_before_missing,
    std::uint64_t frames_after_missing)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "bundle-finalize-integrity-test", false});

  apitrace::trace::AssetRecord asset;
  asset.blob_id = 940;
  asset.kind = apitrace::trace::AssetKind::Buffer;
  asset.debug_name = "valid-unmap-buffer";
  asset.payload_bytes.assign(256, 0x44);
  asset = writer.register_asset(std::move(asset));

  std::uint64_t sequence = 1;
  for (std::uint64_t frame = 0; frame < frames_before_missing; ++frame) {
    append_present_frame(writer, frame, sequence);
  }

  apitrace::trace::EventRecord valid_unmap;
  valid_unmap.kind = apitrace::trace::EventKind::Call;
  valid_unmap.callsite.sequence = sequence++;
  valid_unmap.callsite.function_name = "ID3D12Resource::Unmap";
  valid_unmap.callsite.result_code = 0;
  valid_unmap.blob_refs = {asset.blob_id};
  valid_unmap.payload = std::string("{\"buffer_path\":\"") + asset.relative_path.generic_string() + "\"}";
  writer.append_call_event(valid_unmap);

  apitrace::trace::EventRecord missing_unmap;
  missing_unmap.kind = apitrace::trace::EventKind::Call;
  missing_unmap.callsite.sequence = sequence++;
  missing_unmap.callsite.function_name = "ID3D12Resource::Unmap";
  missing_unmap.callsite.result_code = 0;
  missing_unmap.blob_refs = {941};
  missing_unmap.payload = "{\"buffer_path\":\"buffers/asset-missing-middle.buffer\"}";
  writer.append_call_event(missing_unmap);

  for (std::uint64_t frame = frames_before_missing;
       frame < frames_before_missing + frames_after_missing;
       ++frame) {
    append_present_frame(writer, frame, sequence);
  }
  writer.close();
  return true;
}

bool append_raw_passthrough_event(
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

bool write_missing_blob_raw_capture(
    const std::filesystem::path &bundle,
    std::uint64_t frames_before_missing,
    std::uint64_t frames_after_missing)
{
  using namespace apitrace::trace::raw;

  const auto materialized_fixture = bundle.parent_path() / (bundle.filename().string() + ".fixture");
  if (!write_missing_blob_materialized_fixture(
          materialized_fixture,
          frames_before_missing,
          frames_after_missing)) {
    return false;
  }

  std::vector<std::string> lines;
  {
    std::ifstream input(materialized_fixture / "callstream.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
      const auto record = json::parse(line, nullptr, false);
      if (!record.is_discarded() && record.value("record_kind", std::string()) != "bundle_header") {
        lines.push_back(std::move(line));
      }
    }
  }

  std::filesystem::remove_all(bundle);
  RawCaptureWriter writer;
  if (!writer.open(bundle)) {
    std::filesystem::remove_all(materialized_fixture);
    return false;
  }

  const std::vector<std::uint8_t> valid_blob(256, 0x44);
  const auto raw_blob_id = writer.append_blob(
      valid_blob.data(),
      valid_blob.size(),
      static_cast<std::uint32_t>(RawBlobKind::Buffer),
      1);
  if (raw_blob_id == kInvalidRawBlobId) {
    std::filesystem::remove_all(materialized_fixture);
    return false;
  }

  bool attached_valid_blob = false;
  std::uint64_t raw_sequence = 1;
  for (const auto &line : lines) {
    const auto record = json::parse(line, nullptr, false);
    std::vector<std::uint8_t> payload;
    auto opcode = RawEventOpcode::Passthrough;
    if (!record.is_discarded() &&
        record.value("function", std::string()) == "ID3D12Resource::Unmap" &&
        record.value("blob_refs", json::array()) == json::array({940})) {
      PassthroughBlobDescriptor descriptor;
      descriptor.provisional_asset_path = record.value("payload", json::object()).value("buffer_path", std::string());
      descriptor.final_blob_id = 940;
      descriptor.raw_blob_id = raw_blob_id;
      descriptor.raw_blob_kind = static_cast<std::uint32_t>(RawBlobKind::Buffer);
      descriptor.debug_name = "valid-unmap-buffer";
      payload = encode_passthrough_with_blob_payload(line, {descriptor});
      opcode = RawEventOpcode::PassthroughWithBlob;
      attached_valid_blob = true;
    } else {
      payload = encode_passthrough_final_json_payload(line);
    }
    if (!append_raw_passthrough_event(writer, raw_sequence++, opcode, payload)) {
      std::filesystem::remove_all(materialized_fixture);
      return false;
    }
  }

  const bool committed = attached_valid_blob && writer.flush_commit();
  writer.close();
  std::filesystem::remove_all(materialized_fixture);
  return committed;
}

std::size_t count_present_frames(const std::filesystem::path &bundle)
{
  std::ifstream input(bundle / "callstream.jsonl", std::ios::binary);
  std::unordered_set<std::uint64_t> frames;
  std::string line;
  while (std::getline(input, line)) {
    if (line.find("IDXGISwapChain::Present") == std::string::npos ||
        line.find("\"frame_index\"") == std::string::npos) {
      continue;
    }
    const auto marker = line.find("\"frame_index\"");
    const auto colon = line.find(':', marker + 13);
    if (colon == std::string::npos) {
      continue;
    }
    std::size_t cursor = colon + 1;
    while (cursor < line.size() && line[cursor] == ' ') {
      ++cursor;
    }
    std::uint64_t frame = 0;
    bool saw_digit = false;
    while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') {
      saw_digit = true;
      frame = (frame * 10) + static_cast<std::uint64_t>(line[cursor] - '0');
      ++cursor;
    }
    if (saw_digit) {
      frames.insert(frame);
    }
  }
  return frames.size();
}

bool verify_shared_payload_slice_hashing(const std::filesystem::path &root)
{
  const auto bundle = root / "shared-payload-slices.apitrace";
  std::filesystem::remove_all(bundle);
  std::filesystem::create_directories(bundle / "raw");

  const std::vector<std::uint8_t> payload = {
      'a', 'l', 'p', 'h', 'a',
      'b', 'r', 'a', 'v', 'o',
      'c', 'h', 'a', 'r', 'l',
      'd', 'e', 'l', 't', 'a',
  };
  write_bytes(bundle / "raw" / "blobs.bin", payload);

  std::size_t payload_slice_tasks = 0;
  std::size_t planned_payload_threads = 0;
  std::vector<std::string> actual_hashes;
  if (!apitrace::tools::test_hook_hash_assets_shared_payload_slices(
          bundle,
          3,
          &payload_slice_tasks,
          &planned_payload_threads,
          &actual_hashes)) {
    std::cerr << "hash_assets did not hash the shared payload fixture\n";
    return false;
  }
  if (payload_slice_tasks != 4 || planned_payload_threads <= 1) {
    std::cerr << "hash_assets did not schedule shared-payload work at slice granularity\n";
    return false;
  }

  std::vector<std::string> expected_hashes;
  for (std::size_t offset = 0; offset < payload.size(); offset += 5) {
    expected_hashes.push_back(apitrace::trace::content_hash_bytes(payload.data() + offset, 5));
  }
  if (actual_hashes != expected_hashes) {
    std::cerr << "hash_assets changed shared-payload slice hashes\n";
    return false;
  }

  return true;
}

bool verify_metal_single_blob_index_repair(const std::filesystem::path &root)
{
  const auto bundle = root / "metal-single-blob-index-repair.apitrace";
  std::filesystem::remove_all(bundle);
  std::filesystem::create_directories(bundle);

  constexpr std::uint64_t blob_id = 0x12345678u;
  const std::string library_path = "metal/libraries/index-repair.metallib";
  write_bytes(bundle / library_path, {0x4d, 0x54, 0x4c, 0x42});

  {
    std::ofstream callstream(bundle / apitrace::trace::kCallstreamFileName,
                             std::ios::binary | std::ios::trunc);
    callstream << "{}\n";
  }
  {
    std::ofstream metal_callstream(
        bundle / apitrace::trace::kMetalCallstreamFileName,
        std::ios::binary | std::ios::trunc);
    metal_callstream
        << json({
               {"record_kind", "call"},
               {"function", "MTLDevice.newLibrary"},
               {"blob_refs", json::array({blob_id})},
               {"payload", {{"library_path", library_path}}},
           }).dump()
        << '\n';
  }

  if (!apitrace::tools::test_hook_repair_finalized_single_blob_asset_index(
          bundle, blob_id, library_path)) {
    std::cerr << "finalized index repair ignored Metal callstream blob references\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 4) {
    std::cerr << "usage: " << argv[0] << " <tmp-bundle-root> <bundle-check> <bundle-finalize>\n";
    return 2;
  }

  const std::filesystem::path root = argv[1];
  const std::filesystem::path bundle_check = argv[2];
  const std::filesystem::path bundle_finalize = argv[3];
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  if (!verify_shared_payload_slice_hashing(root)) {
    return 1;
  }
  if (!verify_metal_single_blob_index_repair(root)) {
    return 1;
  }

  const auto reference_fixture = root / "reference-collection-fixture.apitrace";
  if (!write_missing_blob_materialized_fixture(reference_fixture, 3, 2)) {
    std::cerr << "failed to write reference-collection fixture\n";
    return 1;
  }
  std::size_t fused_path_refs = 0;
  std::size_t fused_blob_id_refs = 0;
  if (!apitrace::tools::test_hook_bundle_finalize_reference_collection_matches_two_pass(
          reference_fixture,
          &fused_path_refs,
          &fused_blob_id_refs) ||
      fused_path_refs == 0 ||
      fused_blob_id_refs == 0) {
    std::cerr << "fused reference collection diverged from the two-pass baseline\n";
    return 1;
  }

  const auto tail_bundle = root / "tail-missing.apitrace";
  if (!write_missing_blob_raw_capture(tail_bundle, 3, 2)) {
    std::cerr << "failed to write tail fixture\n";
    return 1;
  }
  if (run_bundle_finalize_with_threshold(bundle_finalize, tail_bundle, "2") != 0) {
    std::cerr << "finalize rejected bounded tail truncation\n";
    return 1;
  }
  if (count_present_frames(tail_bundle) != 3 ||
      read_text(tail_bundle / "callstream.jsonl").find("asset-missing-middle.buffer") != std::string::npos ||
      !std::filesystem::is_regular_file(tail_bundle / "checksums.json")) {
    std::cerr << "finalize did not preserve the expected consistent prefix\n";
    return 1;
  }
  if (run_bundle_check(bundle_check, tail_bundle) != 0) {
    std::cerr << "bundle-check rejected the truncated prefix bundle\n";
    return 1;
  }

  const auto mid_bundle = root / "midstream-missing.apitrace";
  if (!write_missing_blob_raw_capture(mid_bundle, 2, 3)) {
    std::cerr << "failed to write midstream fixture\n";
    return 1;
  }
  const auto raw_events_before = read_text(mid_bundle / "raw" / "events.bin");
  const auto raw_blobs_before = read_text(mid_bundle / "raw" / "blobs.bin");
  const auto raw_blob_index_before = read_text(mid_bundle / "raw" / "blobs.idx");
  const auto raw_commit_before = read_text(mid_bundle / "raw" / "commit.meta");
  const auto stderr_path = root / "midstream.stderr";
  if (run_bundle_finalize_with_threshold(bundle_finalize, mid_bundle, "2", stderr_path) == 0) {
    std::cerr << "finalize accepted over-threshold midstream loss\n";
    return 1;
  }
  const auto stderr_text = read_text(stderr_path);
  if (stderr_text.find("mid-stream integrity failure: would truncate 3 frames from frame 2 to end") ==
          std::string::npos ||
      stderr_text.find("capture lost data mid-stream") == std::string::npos ||
      read_text(mid_bundle / "raw" / "events.bin") != raw_events_before ||
      read_text(mid_bundle / "raw" / "blobs.bin") != raw_blobs_before ||
      read_text(mid_bundle / "raw" / "blobs.idx") != raw_blob_index_before ||
      read_text(mid_bundle / "raw" / "commit.meta") != raw_commit_before ||
      read_text(mid_bundle / "callstream.jsonl").find("asset-missing-middle.buffer") == std::string::npos) {
    std::cerr << "finalize did not fail loudly while preserving the authoritative raw capture\n";
    return 1;
  }

  return 0;
}
