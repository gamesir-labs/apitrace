#include "apitrace/capture_runtime.hpp"
#include "apitrace/d3d12_capture.hpp"
#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

std::filesystem::path unique_work_dir()
{
  const auto base = std::filesystem::temp_directory_path();
  return base / ("apitrace-d3d12-rawonly-unmap-test-" + std::to_string(static_cast<unsigned long long>(
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

bool set_trace_bundle_env(const std::filesystem::path &bundle)
{
#ifdef _WIN32
  return _putenv_s("APITRACE_TRACE_BUNDLE", bundle.string().c_str()) == 0;
#else
  return setenv("APITRACE_TRACE_BUNDLE", bundle.string().c_str(), 1) == 0;
#endif
}

void clear_capture_env()
{
#ifdef _WIN32
  _putenv_s("APITRACE_TRACE_BUNDLE", "");
#else
  unsetenv("APITRACE_TRACE_BUNDLE");
#endif
}

void *fake_object(std::uintptr_t address)
{
  return reinterpret_cast<void *>(address);
}

std::uint64_t read_le64(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
  }
  return value;
}

bool run_rawonly_unmap_fast_path(const std::filesystem::path &bundle)
{
  using namespace apitrace::trace::raw;

  std::filesystem::remove_all(bundle);
  apitrace::runtime::shutdown_process_trace_session();
  apitrace::d3d12::reset_raw_unmap_fast_path_for_test();
  if (!set_trace_bundle_env(bundle)) {
    std::cerr << "failed to configure raw capture\n";
    return false;
  }

  auto *device = fake_object(0x21000);
  auto *resource = fake_object(0x22000);
  apitrace::d3d12::record_d3d12_create_device(device);
  apitrace::d3d12::record_object_create(
      resource,
      apitrace::d3d12::CaptureObjectKind::Resource,
      device,
      "ID3D12Resource");

  std::vector<std::uint8_t> first = {0x10, 0x11, 0x12, 0x13};
  std::vector<std::uint8_t> same = first;
  std::vector<std::uint8_t> changed = {0x10, 0x21, 0x12, 0x13};
  apitrace::d3d12::record_resource_unmap(resource, 0, 64, 68, first.data(), first.size());
  apitrace::d3d12::record_resource_unmap(resource, 0, 64, 68, same.data(), same.size());
  apitrace::d3d12::record_resource_unmap(resource, 0, 64, 68, changed.data(), changed.size());

  apitrace::runtime::shutdown_process_trace_session();
  clear_capture_env();

  const auto counters = apitrace::d3d12::raw_unmap_fast_path_counters_for_test();
  if (!expect(counters.unmap_candidates == 3, "unexpected raw unmap candidate count") ||
      !expect(counters.unchanged_skipped == 1, "unchanged raw unmap was not skipped") ||
      !expect(counters.emitted_blob_bytes == first.size() + changed.size(), "unexpected emitted blob byte count") ||
      !expect(counters.raw_write_failures == 0, "raw unmap write failure was recorded")) {
    return false;
  }

  RawCaptureReader reader;
  if (!expect(reader.open(bundle), "failed to open raw capture")) {
    std::cerr << reader.last_error() << "\n";
    return false;
  }
  const auto records = reader.read_events();
  std::vector<RawEventRecord> unmaps;
  for (const auto &record : records) {
    if (record.header.opcode == static_cast<std::uint32_t>(RawEventOpcode::ResourceUnmap)) {
      unmaps.push_back(record);
    }
  }
  if (!expect(unmaps.size() == 2, "raw unchanged unmap emitted an extra event") ||
      !expect(reader.blob_extents().size() == 2, "raw unchanged unmap emitted an extra blob")) {
    return false;
  }

  std::vector<std::uint8_t> blob0;
  std::vector<std::uint8_t> blob1;
  const auto blob0_id = read_le64(unmaps[0].payload, 8);
  const auto blob1_id = read_le64(unmaps[1].payload, 8);
  if (!expect(read_le64(unmaps[0].payload, 16) == 64 &&
                  read_le64(unmaps[0].payload, 24) == 68,
              "first raw unmap range mismatch") ||
      !expect(read_le64(unmaps[1].payload, 16) == 64 &&
                  read_le64(unmaps[1].payload, 24) == 68,
              "second raw unmap range mismatch") ||
      !expect(reader.read_blob(blob0_id, blob0), "failed to read first raw unmap blob") ||
      !expect(reader.read_blob(blob1_id, blob1), "failed to read second raw unmap blob") ||
      !expect(blob0 == first, "first raw unmap blob mismatch") ||
      !expect(blob1 == changed, "changed raw unmap blob mismatch")) {
    return false;
  }

  const auto decoded = decode_raw_events(reader, unmaps);
  if (!expect(decoded.error.empty(), "raw unmap events failed to decode")) {
    std::cerr << decoded.error << "\n";
    return false;
  }
  std::vector<std::uint8_t> decoded_blob0;
  std::vector<std::uint8_t> decoded_blob1;
  if (!expect(decoded.events.size() == 2 &&
                  decoded.events[0].event.callsite.function_name == "ID3D12Resource::Unmap" &&
                  decoded.events[1].event.callsite.function_name == "ID3D12Resource::Unmap" &&
                  decoded.events[0].assets.size() == 1 &&
                  decoded.events[1].assets.size() == 1 &&
                  decoded.events[0].assets[0].payload_path.generic_string() == "raw/blobs.bin" &&
                  decoded.events[1].assets[0].payload_path.generic_string() == "raw/blobs.bin",
              "decoded raw unmap event/blob mismatch") ||
      !expect(reader.read_blob(blob0_id, decoded_blob0), "failed to read decoded first raw unmap blob") ||
      !expect(reader.read_blob(blob1_id, decoded_blob1), "failed to read decoded second raw unmap blob") ||
      !expect(decoded_blob0 == first, "decoded first raw unmap blob mismatch") ||
      !expect(decoded_blob1 == changed, "decoded changed raw unmap blob mismatch")) {
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
  const auto bundle = work_dir / "rawonly-unmap.apitrace";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const bool ok = run_rawonly_unmap_fast_path(bundle);
  std::filesystem::remove_all(work_dir);
  if (!ok) {
    return 1;
  }
  std::cout << "d3d12 raw unmap fast path passed\n";
  return 0;
#endif
}
