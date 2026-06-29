#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"
#include "apitrace/trace_session.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path unique_work_dir()
{
  const auto base = std::filesystem::temp_directory_path();
  return base / ("apitrace-session-raw-passthrough-test-" + std::to_string(static_cast<unsigned long long>(
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

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool read_asset_payload(
    const std::filesystem::path &bundle,
    const apitrace::trace::AssetRecord &asset,
    std::vector<std::uint8_t> &bytes)
{
  if (!asset.payload_bytes.empty()) {
    bytes = asset.payload_bytes;
    return true;
  }
  if (asset.payload_path.empty()) {
    return false;
  }
  std::ifstream input(bundle / asset.payload_path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(asset.payload_offset), std::ios::beg);
  if (!input.good()) {
    return false;
  }
  bytes.resize(static_cast<std::size_t>(asset.byte_size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::size_t>(input.gcount()) == bytes.size();
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

apitrace::trace::EventRecord make_event(
    std::uint64_t sequence,
    const char *function_name,
    std::string payload,
    std::vector<apitrace::trace::BlobId> blob_refs = {})
{
  apitrace::trace::EventRecord event;
  event.kind = apitrace::trace::EventKind::Call;
  event.callsite.sequence = sequence;
  event.callsite.function_name = function_name;
  event.callsite.result_code = 0;
  event.payload = std::move(payload);
  event.blob_refs = std::move(blob_refs);
  return event;
}

bool run_passthrough_test(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  {
    apitrace::TraceSession session(trace_options(bundle));
    session.begin();
    session.append_call_event(make_event(11, "ID3D12GraphicsCommandList::DrawInstanced", "{\"vertex_count\":3}"));

    apitrace::trace::AssetRecord asset;
    asset.blob_id = 77;
    asset.kind = apitrace::trace::AssetKind::Buffer;
    asset.debug_name = "d3d12-resource-unmap";
    asset.payload_bytes = {0x01, 0x02, 0x03, 0x04};
    asset = session.stage_raw_asset(std::move(asset));
    session.append_call_event(make_event(
        12,
        "ID3D12Resource::Unmap",
        "{\"buffer_path\":\"" + asset.relative_path.generic_string() + "\",\"written_range\":{\"begin\":0,\"end\":4}}",
        {asset.blob_id}));
    session.end();
  }

  const auto callstream_lines = read_lines(bundle / "callstream.jsonl");
  if (!expect(callstream_lines.size() == 1, "raw capture should only write the callstream header before finalize")) {
    return false;
  }

  apitrace::trace::raw::RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open raw capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }
  const auto records = reader.read_events();
  if (!expect(records.size() == 2, "expected one passthrough and one blob passthrough raw event")) {
    return false;
  }
  if (!expect(records[0].header.sequence == 11, "passthrough sequence mismatch") ||
      !expect(records[0].header.opcode == static_cast<std::uint32_t>(RawEventOpcode::PassthroughFinalJson),
              "passthrough opcode mismatch")) {
    return false;
  }
  if (!expect(records[1].header.sequence == 12, "blob passthrough sequence mismatch") ||
      !expect(records[1].header.opcode == static_cast<std::uint32_t>(RawEventOpcode::PassthroughWithBlob),
              "blob passthrough opcode mismatch")) {
    return false;
  }

  const auto decoded = apitrace::trace::raw::decode_raw_events(reader, records);
  if (!expect(decoded.error.empty(), "failed to decode raw passthrough events")) {
    std::cerr << decoded.error << "\n";
    return false;
  }
  if (!expect(decoded.events.size() == 2 &&
                  decoded.events[1].passthrough &&
                  decoded.events[1].assets.size() == 1,
              "blob passthrough did not decode one materialized asset")) {
    return false;
  }
  std::vector<std::uint8_t> asset_bytes;
  if (!expect(decoded.events[1].assets[0].payload_path.generic_string() == "raw/blobs.bin",
              "blob passthrough asset was not path-backed") ||
      !expect(read_asset_payload(bundle, decoded.events[1].assets[0], asset_bytes),
              "blob passthrough failed to read path-backed raw bytes") ||
      !expect(asset_bytes == std::vector<std::uint8_t>({0x01, 0x02, 0x03, 0x04}),
              "blob passthrough materialized wrong raw bytes")) {
    return false;
  }

  return expect(decoded.events[0].passthrough_jsonl_record.find(
                    "\"function\":\"ID3D12GraphicsCommandList::DrawInstanced\"") != std::string::npos,
                "passthrough payload did not preserve function");
}

bool run_raw_passthrough_test(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  {
    apitrace::TraceSession session(trace_options(bundle));
    session.begin();
    if (!expect(session.raw_capture_writer() != nullptr, "raw capture did not create raw writer")) {
      return false;
    }
    session.append_call_event(make_event(31, "ID3D12CommandQueue::Signal", "{\"value\":2}"));

    apitrace::trace::AssetRecord asset;
    asset.kind = apitrace::trace::AssetKind::Buffer;
    asset.debug_name = "raw-staged-buffer";
    asset.payload_bytes = {0xa0, 0xb1, 0xc2};
    asset = session.stage_raw_asset(std::move(asset));
    session.append_call_event(make_event(
        32,
        "ID3D12Device::CreateCommittedResource",
        "{\"buffer_path\":\"" + asset.relative_path.generic_string() + "\",\"byte_size\":3}",
        {asset.blob_id}));
    session.end();
  }

  const auto callstream_lines = read_lines(bundle / "callstream.jsonl");
  if (!expect(callstream_lines.size() == 1, "raw should not write old-path callstream events") ||
      !expect(read_text(bundle / "assets.json").find("\"blob_id\"") == std::string::npos,
              "raw should not register old-path assets") ||
      !expect(!std::filesystem::exists(bundle / "spool" / "asset-payloads.bin"),
              "raw should not write old-path asset spool")) {
    return false;
  }

  apitrace::trace::raw::RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open raw raw capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }
  const auto records = reader.read_events();
  if (!expect(records.size() == 2, "raw should emit non-blob and blob passthrough events") ||
      !expect(records[0].header.opcode == static_cast<std::uint32_t>(RawEventOpcode::PassthroughFinalJson),
              "raw non-blob event was not passthrough") ||
      !expect(records[1].header.opcode == static_cast<std::uint32_t>(RawEventOpcode::PassthroughWithBlob),
              "raw blob event was not passthrough-with-blob") ||
      !expect(reader.blob_extents().size() == 1, "raw blob event did not append one raw blob")) {
    return false;
  }
  const auto decoded = decode_raw_events(reader, records);
  if (!expect(decoded.error.empty(), "failed to decode raw passthrough events")) {
    std::cerr << decoded.error << "\n";
    return false;
  }
  if (!expect(decoded.events.size() == 2 &&
                  decoded.events[0].passthrough &&
                  decoded.events[1].passthrough &&
                  decoded.events[1].assets.size() == 1,
              "raw passthrough decode shape mismatch") ||
      !expect(decoded.events[1].assets[0].payload_path.generic_string() == "raw/blobs.bin",
              "raw staged blob was not path-backed")) {
    return false;
  }
  std::vector<std::uint8_t> asset_bytes;
  if (!expect(read_asset_payload(bundle, decoded.events[1].assets[0], asset_bytes),
              "raw staged blob failed to read path-backed bytes") ||
      !expect(asset_bytes == std::vector<std::uint8_t>({0xa0, 0xb1, 0xc2}),
              "raw staged blob bytes changed")) {
    return false;
  }
  return expect(decoded.events[0].passthrough_jsonl_record.find("\"function\":\"ID3D12CommandQueue::Signal\"") !=
                        std::string::npos &&
                    decoded.events[1].passthrough_jsonl_record.find(
                        "\"function\":\"ID3D12Device::CreateCommittedResource\"") != std::string::npos,
                "raw passthrough JSON did not preserve functions");
}

} // namespace

int main()
{
  const auto root = unique_work_dir();
  const bool ok = run_passthrough_test(root / "raw-on.apitrace") &&
                  run_raw_passthrough_test(root / "raw.apitrace");
  std::filesystem::remove_all(root);
  return ok ? 0 : 1;
}
