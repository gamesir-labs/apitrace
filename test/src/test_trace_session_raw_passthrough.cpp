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

apitrace::TraceOptions trace_options(const std::filesystem::path &bundle, bool raw_enabled)
{
  apitrace::TraceOptions options;
  options.api = apitrace::trace::ApiKind::D3D12;
  options.bundle_root = bundle;
  options.capture.raw_mode = raw_enabled
      ? apitrace::runtime::CaptureOptions::CaptureRawMode::DualWrite
      : apitrace::runtime::CaptureOptions::CaptureRawMode::Off;
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
    apitrace::TraceSession session(trace_options(bundle, true));
    session.begin();
    session.append_call_event(make_event(11, "ID3D12GraphicsCommandList::DrawInstanced", "{\"vertex_count\":3}"));

    apitrace::trace::AssetRecord asset;
    asset.blob_id = 77;
    asset.kind = apitrace::trace::AssetKind::Buffer;
    asset.debug_name = "d3d12-resource-unmap";
    asset.payload_bytes = {0x01, 0x02, 0x03, 0x04};
    asset = session.register_asset(std::move(asset));
    session.append_call_event(make_event(
        12,
        "ID3D12Resource::Unmap",
        "{\"buffer_path\":\"" + asset.relative_path.generic_string() + "\",\"written_range\":{\"begin\":0,\"end\":4}}",
        {asset.blob_id}));
    session.end();
  }

  const auto callstream_lines = read_lines(bundle / "callstream.jsonl");
  if (!expect(callstream_lines.size() == 3, "expected header plus two callstream events")) {
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
  if (!expect(decoded.events[1].assets[0].payload_bytes == std::vector<std::uint8_t>({0x01, 0x02, 0x03, 0x04}),
              "blob passthrough materialized wrong raw bytes")) {
    return false;
  }

  const std::string passthrough(
      reinterpret_cast<const char *>(records[0].payload.data()),
      reinterpret_cast<const char *>(records[0].payload.data() + records[0].payload.size()));
  return expect(passthrough == callstream_lines[1], "passthrough payload did not byte-match callstream line");
}

bool run_flag_off_test(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  {
    apitrace::TraceSession session(trace_options(bundle, false));
    session.begin();
    session.append_call_event(make_event(21, "ID3D12CommandQueue::Signal", "{\"value\":1}"));
    session.end();
  }

  return expect(!std::filesystem::exists(bundle / "raw" / "events.bin"), "raw events file exists when flag is off");
}

bool run_raw_only_unwired_test(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  {
    apitrace::TraceOptions options = trace_options(bundle, false);
    options.capture.raw_mode = apitrace::runtime::CaptureOptions::CaptureRawMode::RawOnly;
    apitrace::TraceSession session(std::move(options));
    session.begin();
    if (!expect(session.capture_raw_mode() == apitrace::runtime::CaptureOptions::CaptureRawMode::RawOnly,
                "session did not report raw-only mode") ||
        !expect(session.raw_capture_writer() != nullptr, "raw-only mode did not create raw writer")) {
      return false;
    }
    session.append_call_event(make_event(31, "ID3D12CommandQueue::Signal", "{\"value\":2}"));
    session.end();
  }

  const auto callstream_lines = read_lines(bundle / "callstream.jsonl");
  if (!expect(callstream_lines.size() == 1, "raw-only unwired path should only write metadata")) {
    return false;
  }

  apitrace::trace::raw::RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open raw-only raw capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }
  return expect(reader.read_events().empty(), "raw-only unwired path should not emit passthrough events");
}

} // namespace

int main()
{
  const auto root = unique_work_dir();
  const bool ok = run_passthrough_test(root / "raw-on.apitrace") &&
                  run_flag_off_test(root / "raw-off.apitrace") &&
                  run_raw_only_unwired_test(root / "raw-only.apitrace");
  std::filesystem::remove_all(root);
  return ok ? 0 : 1;
}
