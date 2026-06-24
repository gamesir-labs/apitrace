#include "apitrace/d3d12_raw_sink.hpp"
#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path unique_work_dir()
{
  const auto base = std::filesystem::temp_directory_path();
  return base / ("apitrace-d3d12-raw-sink-test-" + std::to_string(static_cast<unsigned long long>(
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

bool run_raw_sink_roundtrip(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);

  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  apitrace::d3d12::RawSink sink(&writer, 1);
  const std::vector<std::uint8_t> blob = {0x10, 0x20, 0x30, 0x40};
  const auto raw_blob_id = sink.append_blob_for_event(
      blob.data(),
      static_cast<std::uint64_t>(blob.size()),
      RawBlobKind::Buffer,
      7);
  if (!expect(raw_blob_id != kInvalidRawBlobId, "failed to append raw blob via sink")) {
    std::cerr << sink.last_error() << "\n";
    return false;
  }

  const auto payload = encode_resource_unmap_payload(200, raw_blob_id, 64, 68);
  if (!expect(sink.append_binary_event(7, RawEventOpcode::ResourceUnmap, payload, 0, 11, 12345),
              "failed to append binary raw event via sink")) {
    std::cerr << sink.last_error() << "\n";
    return false;
  }
  if (!expect(writer.flush_commit(), "failed to flush raw sink capture")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();

  RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open raw reader")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }

  const auto records = reader.read_events();
  if (!expect(records.size() == 1, "raw sink wrote wrong event count") ||
      !expect(records[0].header.sequence == 7, "raw sink sequence mismatch") ||
      !expect(records[0].header.thread_id == 11, "raw sink thread id mismatch") ||
      !expect(records[0].header.timestamp_or_monotonic_counter == 12345,
              "raw sink timestamp mismatch") ||
      !expect(records[0].header.opcode == static_cast<std::uint32_t>(RawEventOpcode::ResourceUnmap),
              "raw sink opcode mismatch") ||
      !expect(records[0].payload == payload, "raw sink payload mismatch")) {
    return false;
  }

  std::vector<std::uint8_t> actual_blob;
  if (!expect(reader.read_blob(raw_blob_id, actual_blob), "failed to read raw sink blob") ||
      !expect(actual_blob == blob, "raw sink blob mismatch")) {
    return false;
  }

  RawBlobExtent extent;
  if (!expect(reader.find_blob_extent(raw_blob_id, extent), "missing raw sink blob extent") ||
      !expect(extent.kind == static_cast<std::uint32_t>(RawBlobKind::Buffer),
              "raw sink blob kind mismatch") ||
      !expect(extent.producing_sequence == 7, "raw sink blob producing sequence mismatch")) {
    return false;
  }

  const auto decoded = decode_raw_events(reader, records);
  if (!expect(decoded.error.empty(), "raw sink event failed to decode")) {
    std::cerr << decoded.error << "\n";
    return false;
  }
  return expect(decoded.events.size() == 1 &&
                    decoded.events[0].event.callsite.function_name == "ID3D12Resource::Unmap" &&
                    decoded.events[0].assets.size() == 1 &&
                    decoded.events[0].assets[0].payload_bytes == blob,
                "raw sink decoded event mismatch");
}

} // namespace

int main()
{
#if !defined(APITRACE_ENABLE_TEST_HOOKS)
  std::cerr << "test requires APITRACE_ENABLE_TEST_HOOKS\n";
  return 77;
#else
  const auto work_dir = unique_work_dir();
  const auto bundle = work_dir / "raw-sink.apitrace";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const bool ok = run_raw_sink_roundtrip(bundle);
  std::filesystem::remove_all(work_dir);
  if (!ok) {
    return 1;
  }
  std::cout << "d3d12 raw sink roundtrip passed\n";
  return 0;
#endif
}
