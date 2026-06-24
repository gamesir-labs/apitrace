#include "apitrace/raw_capture_io.hpp"

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
  return base / ("apitrace-raw-capture-test-" + std::to_string(static_cast<unsigned long long>(
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

bool operator==(const apitrace::trace::raw::RawEventHeader &lhs,
                const apitrace::trace::raw::RawEventHeader &rhs)
{
  return lhs.sequence == rhs.sequence &&
         lhs.thread_id == rhs.thread_id &&
         lhs.timestamp_or_monotonic_counter == rhs.timestamp_or_monotonic_counter &&
         lhs.opcode == rhs.opcode &&
         lhs.result_or_flags == rhs.result_or_flags &&
         lhs.payload_len == rhs.payload_len;
}

bool operator==(const apitrace::trace::raw::RawBlobExtent &lhs,
                const apitrace::trace::raw::RawBlobExtent &rhs)
{
  return lhs.raw_blob_id == rhs.raw_blob_id &&
         lhs.kind == rhs.kind &&
         lhs.producing_sequence == rhs.producing_sequence &&
         lhs.offset == rhs.offset &&
         lhs.size == rhs.size;
}

void append_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
  std::ofstream output(path, std::ios::binary | std::ios::app);
  output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool run_roundtrip_test(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);

  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open raw writer")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> event_payload_a = {0x01, 0x02, 0x03, 0x04};
  const std::vector<std::uint8_t> event_payload_b = {'d', '3', 'd', '1', '2'};
  const std::vector<std::uint8_t> event_payload_c;
  const std::vector<std::uint8_t> blob_a = {0xaa, 0xbb, 0xcc, 0xdd, 0xee};
  const std::vector<std::uint8_t> blob_b = {'r', 'o', 'o', 't', 's', 'i', 'g'};

  std::vector<RawEventRecord> expected_events;
  expected_events.push_back({RawEventHeader{1, 10, 1000, 0x101, 0, event_payload_a.size()}, event_payload_a});
  expected_events.push_back({RawEventHeader{2, 11, 1010, 0x202, 0x80000001u, event_payload_b.size()}, event_payload_b});
  expected_events.push_back({RawEventHeader{3, 10, 1020, 0x303, 7, event_payload_c.size()}, event_payload_c});

  for (const auto &event : expected_events) {
    if (!expect(writer.append_event(event.header, event.payload.data(), event.payload.size()),
                "failed to append raw event")) {
      std::cerr << writer.last_error() << "\n";
      return false;
    }
  }

  const auto blob_a_id = writer.append_blob(blob_a.data(), blob_a.size(), 17, 1);
  const auto blob_b_id = writer.append_blob(blob_b.data(), blob_b.size(), 23, 2);
  if (!expect(blob_a_id != kInvalidRawBlobId && blob_b_id != kInvalidRawBlobId,
              "failed to append raw blobs")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  if (!expect(writer.flush_commit(), "failed to commit raw prefix")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  writer.close();

  RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open raw reader")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }

  const auto events = reader.read_events();
  if (!expect(events.size() == expected_events.size(), "raw reader returned wrong event count")) {
    return false;
  }
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (!expect(events[index].header == expected_events[index].header,
                "raw event header mismatch") ||
        !expect(events[index].payload == expected_events[index].payload,
                "raw event payload mismatch")) {
      return false;
    }
  }

  std::vector<std::uint8_t> actual_blob;
  if (!expect(reader.read_blob(blob_a_id, actual_blob), "failed to read first raw blob") ||
      !expect(actual_blob == blob_a, "first raw blob mismatch") ||
      !expect(reader.read_blob(blob_b_id, actual_blob), "failed to read second raw blob") ||
      !expect(actual_blob == blob_b, "second raw blob mismatch")) {
    return false;
  }

  if (!expect(reader.blob_extents().size() == 2, "wrong raw blob extent count")) {
    return false;
  }
  RawBlobExtent extent;
  if (!expect(reader.find_blob_extent(blob_a_id, extent), "missing first raw blob extent") ||
      !expect(extent.raw_blob_id == blob_a_id &&
                  extent.kind == 17 &&
                  extent.producing_sequence == 1 &&
                  extent.size == blob_a.size(),
              "first raw blob extent metadata mismatch")) {
    return false;
  }
  if (!expect(reader.find_blob_extent(blob_b_id, extent), "missing second raw blob extent") ||
      !expect(extent.raw_blob_id == blob_b_id &&
                  extent.kind == 23 &&
                  extent.producing_sequence == 2 &&
                  extent.size == blob_b.size(),
              "second raw blob extent metadata mismatch")) {
    return false;
  }
  return true;
}

bool run_truncated_tail_test(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);

  RawCaptureWriter writer;
  if (!expect(writer.open(bundle), "failed to open raw writer for tail test")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }

  const std::vector<std::uint8_t> committed_payload = {'o', 'k'};
  const RawEventRecord committed_event{
      RawEventHeader{41, 7, 9000, 0x4444, 12, committed_payload.size()},
      committed_payload,
  };
  if (!expect(writer.append_event(
                  committed_event.header,
                  committed_event.payload.data(),
                  committed_event.payload.size()),
              "failed to append committed tail-test event")) {
    return false;
  }
  const std::vector<std::uint8_t> committed_blob = {0x10, 0x20, 0x30};
  const auto committed_blob_id = writer.append_blob(committed_blob.data(), committed_blob.size(), 99, 41);
  if (!expect(committed_blob_id != kInvalidRawBlobId, "failed to append committed tail-test blob")) {
    return false;
  }
  if (!expect(writer.flush_commit(), "failed to commit tail-test prefix")) {
    std::cerr << writer.last_error() << "\n";
    return false;
  }
  const auto committed = writer.committed_prefix();
  writer.close();

  append_bytes(bundle / "raw" / "events.bin", {0x2a, 0, 0, 0, 0, 0, 0, 0, 0xff});
  append_bytes(bundle / "raw" / "blobs.bin", {0xde, 0xad, 0xbe});
  append_bytes(bundle / "raw" / "blobs.idx", {0x7b, 0, 0, 0, 0});

  RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open tail-test raw reader")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }

  if (!expect(reader.committed_prefix().events_committed_bytes == committed.events_committed_bytes &&
                  reader.committed_prefix().blobs_committed_bytes == committed.blobs_committed_bytes &&
                  reader.committed_prefix().blob_index_committed_bytes == committed.blob_index_committed_bytes,
              "reader did not preserve committed prefix metadata")) {
    return false;
  }

  const auto events = reader.read_events();
  if (!expect(events.size() == 1, "tail-test reader did not ignore incomplete trailing event") ||
      !expect(events[0].header == committed_event.header, "tail-test committed event header mismatch") ||
      !expect(events[0].payload == committed_event.payload, "tail-test committed event payload mismatch")) {
    return false;
  }

  std::vector<std::uint8_t> actual_blob;
  if (!expect(reader.read_blob(committed_blob_id, actual_blob),
              "tail-test reader could not read committed blob") ||
      !expect(actual_blob == committed_blob, "tail-test committed blob mismatch") ||
      !expect(reader.blob_extents().size() == 1,
              "tail-test reader did not ignore incomplete trailing blob/index data")) {
    return false;
  }
  return true;
}

} // namespace

int main()
{
#if !defined(APITRACE_ENABLE_TEST_HOOKS)
  std::cerr << "test requires APITRACE_ENABLE_TEST_HOOKS\n";
  return 77;
#else
  const auto work_dir = unique_work_dir();
  const auto roundtrip_bundle = work_dir / "roundtrip.apitrace";
  const auto tail_bundle = work_dir / "tail.apitrace";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const bool ok = run_roundtrip_test(roundtrip_bundle) &&
                  run_truncated_tail_test(tail_bundle);
  std::filesystem::remove_all(work_dir);
  if (!ok) {
    return 1;
  }
  std::cout << "raw capture roundtrip and tail tests passed\n";
  return 0;
#endif
}
