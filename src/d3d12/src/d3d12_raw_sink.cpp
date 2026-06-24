#include "apitrace/d3d12_raw_sink.hpp"

namespace apitrace::d3d12 {

RawSink::RawSink(trace::raw::RawCaptureWriter *writer, std::uint64_t commit_cadence_bytes) noexcept
    : writer_(writer),
      commit_cadence_bytes_(commit_cadence_bytes)
{
}

bool RawSink::append_binary_event(
    std::uint64_t sequence,
    trace::raw::RawEventOpcode opcode,
    const std::vector<std::uint8_t> &encoded_payload,
    std::uint32_t result_or_flags,
    std::uint64_t thread_id,
    std::uint64_t timestamp_or_monotonic_counter) noexcept
{
  return append_binary_event(
      sequence,
      opcode,
      encoded_payload.empty() ? nullptr : encoded_payload.data(),
      static_cast<std::uint64_t>(encoded_payload.size()),
      result_or_flags,
      thread_id,
      timestamp_or_monotonic_counter);
}

bool RawSink::append_binary_event(
    std::uint64_t sequence,
    trace::raw::RawEventOpcode opcode,
    const void *encoded_payload,
    std::uint64_t encoded_payload_size,
    std::uint32_t result_or_flags,
    std::uint64_t thread_id,
    std::uint64_t timestamp_or_monotonic_counter) noexcept
{
  if (!writer_) {
    return false;
  }

  trace::raw::RawEventHeader header;
  header.sequence = sequence;
  header.thread_id = thread_id;
  header.timestamp_or_monotonic_counter = timestamp_or_monotonic_counter;
  header.opcode = static_cast<std::uint32_t>(opcode);
  header.result_or_flags = result_or_flags;
  header.payload_len = encoded_payload_size;
  if (!writer_->append_event(header, encoded_payload, encoded_payload_size)) {
    return false;
  }
  return flush_commit_if_needed();
}

std::uint64_t RawSink::append_blob_for_event(
    const void *bytes,
    std::uint64_t size,
    trace::raw::RawBlobKind kind,
    std::uint64_t producing_sequence) noexcept
{
  if (!writer_) {
    return trace::raw::kInvalidRawBlobId;
  }
  return writer_->append_blob(bytes, size, static_cast<std::uint32_t>(kind), producing_sequence);
}

bool RawSink::flush_commit_if_needed() noexcept
{
  if (!writer_) {
    return false;
  }
  return writer_->flush_commit_if_needed(commit_cadence_bytes_);
}

RawSink::operator bool() const noexcept
{
  return writer_ != nullptr;
}

trace::raw::RawCaptureWriter *RawSink::writer() const noexcept
{
  return writer_;
}

const char *RawSink::last_error() const noexcept
{
  return writer_ ? writer_->last_error().c_str() : "raw sink has no writer";
}

} // namespace apitrace::d3d12
