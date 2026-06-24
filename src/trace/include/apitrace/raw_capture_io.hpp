#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace apitrace::trace::raw {

constexpr std::uint32_t kRawCaptureFormatVersion = 1;
constexpr std::uint32_t kRawCaptureEndianLittle = 0x12345678u;
constexpr std::uint64_t kInvalidRawBlobId = 0;

struct RawEventHeader {
  std::uint64_t sequence = 0;
  std::uint64_t thread_id = 0;
  std::uint64_t timestamp_or_monotonic_counter = 0;
  std::uint32_t opcode = 0;
  std::uint32_t result_or_flags = 0;
  std::uint64_t payload_len = 0;
};

struct RawBlobExtent {
  std::uint64_t raw_blob_id = kInvalidRawBlobId;
  std::uint32_t kind = 0;
  std::uint64_t producing_sequence = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct RawEventRecord {
  RawEventHeader header;
  std::vector<std::uint8_t> payload;
};

struct RawCommitMeta {
  std::uint64_t events_committed_bytes = 0;
  std::uint64_t blobs_committed_bytes = 0;
  std::uint64_t blob_index_committed_bytes = 0;
};

// Minimal, capture-fast raw stream writer. It emits opaque opcode-typed payloads
// and raw blob extents only; hashing, deduplication, canonical names, JSON, and
// semantic decoding belong to the later finalize phase. The current
// implementation serializes append calls with one mutex. Future producers can
// shard staging buffers per thread and keep this class as the ordered file
// publication point without changing the on-disk format.
class RawCaptureWriter {
public:
  RawCaptureWriter();
  ~RawCaptureWriter();

  bool open(const std::filesystem::path &bundle_root);
  void close();

  bool append_event(const RawEventHeader &header, const void *payload, std::uint64_t payload_size);
  std::uint64_t append_blob(const void *bytes, std::uint64_t size, std::uint32_t kind, std::uint64_t producing_sequence);
  bool flush_commit();
  bool flush_commit_if_needed(std::uint64_t bytes_threshold);

  bool is_open() const noexcept;
  const std::filesystem::path &raw_root() const noexcept;
  RawCommitMeta committed_prefix() const noexcept;
  const std::string &last_error() const noexcept;

private:
  bool write_headers_locked();
  bool write_commit_meta_locked();
  bool close_locked();
  bool fail_locked(std::string message);

  mutable std::mutex mutex_;
  std::mutex commit_mutex_;
  std::filesystem::path bundle_root_;
  std::filesystem::path raw_root_;
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::string last_error_;
};

class RawCaptureReader {
public:
  RawCaptureReader();
  ~RawCaptureReader();

  bool open(const std::filesystem::path &bundle_root);
  void close();

  bool is_open() const noexcept;
  std::vector<RawEventRecord> read_events() const;
  bool read_blob(std::uint64_t raw_blob_id, std::vector<std::uint8_t> &bytes) const;
  bool find_blob_extent(std::uint64_t raw_blob_id, RawBlobExtent &extent) const;
  const std::vector<RawBlobExtent> &blob_extents() const noexcept;
  RawCommitMeta committed_prefix() const noexcept;
  const std::string &last_error() const noexcept;

private:
  bool load_headers_and_commit();
  bool load_blob_index();
  bool fail(std::string message);

  std::filesystem::path bundle_root_;
  std::filesystem::path raw_root_;
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::string last_error_;
};

} // namespace apitrace::trace::raw
