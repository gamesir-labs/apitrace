#pragma once

#include "apitrace/raw_capture_io.hpp"
#include "apitrace/raw_event_codec.hpp"

#include <cstdint>
#include <vector>

namespace apitrace::d3d12 {

class RawSink {
public:
  explicit RawSink(trace::raw::RawCaptureWriter *writer, std::uint64_t commit_cadence_bytes = 0) noexcept;

  bool append_binary_event(
      std::uint64_t sequence,
      trace::raw::RawEventOpcode opcode,
      const std::vector<std::uint8_t> &encoded_payload,
      std::uint32_t result_or_flags = 0,
      std::uint64_t thread_id = 0,
      std::uint64_t timestamp_or_monotonic_counter = 0) noexcept;
  bool append_binary_event(
      std::uint64_t sequence,
      trace::raw::RawEventOpcode opcode,
      const void *encoded_payload,
      std::uint64_t encoded_payload_size,
      std::uint32_t result_or_flags = 0,
      std::uint64_t thread_id = 0,
      std::uint64_t timestamp_or_monotonic_counter = 0) noexcept;
  std::uint64_t append_blob_for_event(
      const void *bytes,
      std::uint64_t size,
      trace::raw::RawBlobKind kind,
      std::uint64_t producing_sequence) noexcept;
  bool flush_commit_if_needed() noexcept;

  explicit operator bool() const noexcept;
  trace::raw::RawCaptureWriter *writer() const noexcept;
  const char *last_error() const noexcept;

private:
  trace::raw::RawCaptureWriter *writer_ = nullptr;
  std::uint64_t commit_cadence_bytes_ = 0;
};

} // namespace apitrace::d3d12
