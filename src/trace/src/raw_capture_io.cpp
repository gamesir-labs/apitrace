#include "apitrace/raw_capture_io.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace apitrace::trace::raw {

namespace {

constexpr std::uint64_t make_magic(const char (&text)[9])
{
  return (static_cast<std::uint64_t>(text[0]) << 0) |
         (static_cast<std::uint64_t>(text[1]) << 8) |
         (static_cast<std::uint64_t>(text[2]) << 16) |
         (static_cast<std::uint64_t>(text[3]) << 24) |
         (static_cast<std::uint64_t>(text[4]) << 32) |
         (static_cast<std::uint64_t>(text[5]) << 40) |
         (static_cast<std::uint64_t>(text[6]) << 48) |
         (static_cast<std::uint64_t>(text[7]) << 56);
}

constexpr std::uint64_t kEventsMagic = make_magic("APTREVT1");
constexpr std::uint64_t kBlobsMagic = make_magic("APTRBLB1");
constexpr std::uint64_t kBlobIndexMagic = make_magic("APTRBIX1");
constexpr std::uint64_t kCommitMagic = make_magic("APTRCMT1");
constexpr std::uint64_t kFileHeaderSize = 16;
constexpr std::uint64_t kEventHeaderSize = 40;
constexpr std::uint64_t kBlobExtentRecordSize = 36;
constexpr std::uint64_t kCommitMetaSize = 40;

void put_u32(std::array<std::uint8_t, 8> &buffer, std::uint32_t value)
{
  buffer[0] = static_cast<std::uint8_t>(value);
  buffer[1] = static_cast<std::uint8_t>(value >> 8);
  buffer[2] = static_cast<std::uint8_t>(value >> 16);
  buffer[3] = static_cast<std::uint8_t>(value >> 24);
}

void put_u64(std::array<std::uint8_t, 8> &buffer, std::uint64_t value)
{
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    buffer[index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
}

std::uint32_t read_u32(const std::uint8_t *bytes)
{
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t *bytes)
{
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

bool write_u32(std::ofstream &output, std::uint32_t value)
{
  std::array<std::uint8_t, 8> buffer{};
  put_u32(buffer, value);
  output.write(reinterpret_cast<const char *>(buffer.data()), 4);
  return static_cast<bool>(output);
}

bool write_u64(std::ofstream &output, std::uint64_t value)
{
  std::array<std::uint8_t, 8> buffer{};
  put_u64(buffer, value);
  output.write(reinterpret_cast<const char *>(buffer.data()), 8);
  return static_cast<bool>(output);
}

bool read_exact(std::ifstream &input, void *bytes, std::uint64_t size)
{
  if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  input.read(reinterpret_cast<char *>(bytes), static_cast<std::streamsize>(size));
  return static_cast<std::uint64_t>(input.gcount()) == size;
}

bool write_file_header(std::ofstream &output, std::uint64_t magic)
{
  return write_u64(output, magic) &&
         write_u32(output, kRawCaptureFormatVersion) &&
         write_u32(output, kRawCaptureEndianLittle);
}

bool read_file_header(std::ifstream &input, std::uint64_t expected_magic)
{
  std::array<std::uint8_t, kFileHeaderSize> header{};
  if (!read_exact(input, header.data(), header.size())) {
    return false;
  }
  return read_u64(header.data()) == expected_magic &&
         read_u32(header.data() + 8) == kRawCaptureFormatVersion &&
         read_u32(header.data() + 12) == kRawCaptureEndianLittle;
}

bool write_event_header(std::ofstream &output, const RawEventHeader &header)
{
  return write_u64(output, header.sequence) &&
         write_u64(output, header.thread_id) &&
         write_u64(output, header.timestamp_or_monotonic_counter) &&
         write_u32(output, header.opcode) &&
         write_u32(output, header.result_or_flags) &&
         write_u64(output, header.payload_len);
}

RawEventHeader read_event_header(const std::array<std::uint8_t, kEventHeaderSize> &bytes)
{
  RawEventHeader header;
  header.sequence = read_u64(bytes.data());
  header.thread_id = read_u64(bytes.data() + 8);
  header.timestamp_or_monotonic_counter = read_u64(bytes.data() + 16);
  header.opcode = read_u32(bytes.data() + 24);
  header.result_or_flags = read_u32(bytes.data() + 28);
  header.payload_len = read_u64(bytes.data() + 32);
  return header;
}

bool write_blob_extent(std::ofstream &output, const RawBlobExtent &extent)
{
  return write_u64(output, extent.raw_blob_id) &&
         write_u32(output, extent.kind) &&
         write_u64(output, extent.producing_sequence) &&
         write_u64(output, extent.offset) &&
         write_u64(output, extent.size);
}

RawBlobExtent read_blob_extent(const std::array<std::uint8_t, kBlobExtentRecordSize> &bytes)
{
  RawBlobExtent extent;
  extent.raw_blob_id = read_u64(bytes.data());
  extent.kind = read_u32(bytes.data() + 8);
  extent.producing_sequence = read_u64(bytes.data() + 12);
  extent.offset = read_u64(bytes.data() + 20);
  extent.size = read_u64(bytes.data() + 28);
  return extent;
}

bool write_commit_meta_file(const std::filesystem::path &path, const RawCommitMeta &meta)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  return write_u64(output, kCommitMagic) &&
         write_u32(output, kRawCaptureFormatVersion) &&
         write_u32(output, kRawCaptureEndianLittle) &&
         write_u64(output, meta.events_committed_bytes) &&
         write_u64(output, meta.blobs_committed_bytes) &&
         write_u64(output, meta.blob_index_committed_bytes);
}

bool read_commit_meta_file(const std::filesystem::path &path, RawCommitMeta &meta)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::array<std::uint8_t, kCommitMetaSize> bytes{};
  if (!read_exact(input, bytes.data(), bytes.size())) {
    return false;
  }
  if (read_u64(bytes.data()) != kCommitMagic ||
      read_u32(bytes.data() + 8) != kRawCaptureFormatVersion ||
      read_u32(bytes.data() + 12) != kRawCaptureEndianLittle) {
    return false;
  }
  meta.events_committed_bytes = read_u64(bytes.data() + 16);
  meta.blobs_committed_bytes = read_u64(bytes.data() + 24);
  meta.blob_index_committed_bytes = read_u64(bytes.data() + 32);
  return true;
}

std::uint64_t tellp_u64(std::ofstream &output)
{
  const auto position = output.tellp();
  if (position < std::streampos(0)) {
    return 0;
  }
  return static_cast<std::uint64_t>(position);
}

std::uint64_t file_size_or_zero(const std::filesystem::path &path)
{
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

std::uint64_t clamp_prefix(std::uint64_t prefix, std::uint64_t file_size)
{
  return std::min(prefix, file_size);
}

} // namespace

class RawCaptureWriter::Impl {
public:
  std::ofstream events;
  std::ofstream blobs;
  std::ofstream blob_index;
  RawCommitMeta committed;
  std::uint64_t next_blob_id = 1;
};

RawCaptureWriter::RawCaptureWriter() = default;

RawCaptureWriter::~RawCaptureWriter()
{
  close();
}

bool RawCaptureWriter::open(const std::filesystem::path &bundle_root)
{
  std::lock_guard<std::mutex> lock(mutex_);
  close();

  bundle_root_ = bundle_root;
  raw_root_ = bundle_root_ / "raw";

  std::error_code error;
  std::filesystem::create_directories(raw_root_, error);
  if (error) {
    last_error_ = "failed to create raw capture directory: " + error.message();
    return false;
  }

  impl_ = std::make_unique<Impl>();
  impl_->events.open(raw_root_ / "events.bin", std::ios::binary | std::ios::trunc);
  impl_->blobs.open(raw_root_ / "blobs.bin", std::ios::binary | std::ios::trunc);
  impl_->blob_index.open(raw_root_ / "blobs.idx", std::ios::binary | std::ios::trunc);
  if (!impl_->events || !impl_->blobs || !impl_->blob_index) {
    return fail_locked("failed to open raw capture files");
  }
  if (!write_headers_locked()) {
    return fail_locked("failed to write raw capture file headers");
  }

  impl_->committed.events_committed_bytes = kFileHeaderSize;
  impl_->committed.blobs_committed_bytes = kFileHeaderSize;
  impl_->committed.blob_index_committed_bytes = kFileHeaderSize;
  if (!write_commit_meta_locked()) {
    return fail_locked("failed to write initial raw commit metadata");
  }
  return true;
}

void RawCaptureWriter::close()
{
  if (!impl_) {
    return;
  }
  if (impl_->events.is_open()) {
    impl_->events.close();
  }
  if (impl_->blobs.is_open()) {
    impl_->blobs.close();
  }
  if (impl_->blob_index.is_open()) {
    impl_->blob_index.close();
  }
  impl_.reset();
}

bool RawCaptureWriter::append_event(
    const RawEventHeader &header,
    const void *payload,
    std::uint64_t payload_size)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!impl_ || !impl_->events) {
    return fail_locked("raw capture writer is not open");
  }
  if (header.payload_len != payload_size) {
    return fail_locked("raw event payload length does not match header");
  }
  if (payload_size != 0 && payload == nullptr) {
    return fail_locked("raw event payload pointer is null");
  }
  if (!write_event_header(impl_->events, header)) {
    return fail_locked("failed to write raw event header");
  }
  if (payload_size != 0) {
    if (payload_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
      return fail_locked("raw event payload is too large for this host stream");
    }
    impl_->events.write(reinterpret_cast<const char *>(payload), static_cast<std::streamsize>(payload_size));
  }
  if (!impl_->events) {
    return fail_locked("failed to write raw event payload");
  }
  return true;
}

std::uint64_t RawCaptureWriter::append_blob(
    const void *bytes,
    std::uint64_t size,
    std::uint32_t kind,
    std::uint64_t producing_sequence)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!impl_ || !impl_->blobs || !impl_->blob_index) {
    fail_locked("raw capture writer is not open");
    return kInvalidRawBlobId;
  }
  if (size != 0 && bytes == nullptr) {
    fail_locked("raw blob pointer is null");
    return kInvalidRawBlobId;
  }
  if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    fail_locked("raw blob is too large for this host stream");
    return kInvalidRawBlobId;
  }

  RawBlobExtent extent;
  extent.raw_blob_id = impl_->next_blob_id++;
  extent.kind = kind;
  extent.producing_sequence = producing_sequence;
  extent.offset = tellp_u64(impl_->blobs);
  extent.size = size;

  if (size != 0) {
    impl_->blobs.write(reinterpret_cast<const char *>(bytes), static_cast<std::streamsize>(size));
  }
  if (!impl_->blobs) {
    fail_locked("failed to write raw blob bytes");
    return kInvalidRawBlobId;
  }
  if (!write_blob_extent(impl_->blob_index, extent)) {
    fail_locked("failed to write raw blob extent");
    return kInvalidRawBlobId;
  }
  return extent.raw_blob_id;
}

bool RawCaptureWriter::flush_commit()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!impl_) {
    return fail_locked("raw capture writer is not open");
  }
  impl_->events.flush();
  impl_->blobs.flush();
  impl_->blob_index.flush();
  if (!impl_->events || !impl_->blobs || !impl_->blob_index) {
    return fail_locked("failed to flush raw capture files");
  }
  impl_->committed.events_committed_bytes = tellp_u64(impl_->events);
  impl_->committed.blobs_committed_bytes = tellp_u64(impl_->blobs);
  impl_->committed.blob_index_committed_bytes = tellp_u64(impl_->blob_index);
  return write_commit_meta_locked();
}

bool RawCaptureWriter::is_open() const noexcept
{
  return impl_ != nullptr;
}

const std::filesystem::path &RawCaptureWriter::raw_root() const noexcept
{
  return raw_root_;
}

RawCommitMeta RawCaptureWriter::committed_prefix() const noexcept
{
  std::lock_guard<std::mutex> lock(mutex_);
  return impl_ ? impl_->committed : RawCommitMeta{};
}

const std::string &RawCaptureWriter::last_error() const noexcept
{
  static const std::string empty;
  return last_error_.empty() ? empty : last_error_;
}

bool RawCaptureWriter::write_headers_locked()
{
  return write_file_header(impl_->events, kEventsMagic) &&
         write_file_header(impl_->blobs, kBlobsMagic) &&
         write_file_header(impl_->blob_index, kBlobIndexMagic);
}

bool RawCaptureWriter::write_commit_meta_locked()
{
  return write_commit_meta_file(raw_root_ / "commit.meta", impl_->committed);
}

bool RawCaptureWriter::fail_locked(std::string message)
{
  if (impl_) {
    last_error_ = std::move(message);
  } else {
    last_error_ = std::move(message);
  }
  return false;
}

class RawCaptureReader::Impl {
public:
  RawCommitMeta committed;
  std::vector<RawBlobExtent> blob_extents;
  std::unordered_map<std::uint64_t, RawBlobExtent> blob_extent_by_id;
};

RawCaptureReader::RawCaptureReader() = default;

RawCaptureReader::~RawCaptureReader()
{
  close();
}

bool RawCaptureReader::open(const std::filesystem::path &bundle_root)
{
  close();
  bundle_root_ = bundle_root;
  raw_root_ = bundle_root_ / "raw";
  impl_ = std::make_unique<Impl>();
  if (!load_headers_and_commit()) {
    impl_.reset();
    return false;
  }
  if (!load_blob_index()) {
    impl_.reset();
    return false;
  }
  return true;
}

void RawCaptureReader::close()
{
  if (!impl_) {
    return;
  }
  impl_.reset();
}

bool RawCaptureReader::is_open() const noexcept
{
  return impl_ != nullptr;
}

std::vector<RawEventRecord> RawCaptureReader::read_events() const
{
  std::vector<RawEventRecord> records;
  if (!impl_) {
    return records;
  }

  const auto events_path = raw_root_ / "events.bin";
  std::ifstream input(events_path, std::ios::binary);
  if (!input || !read_file_header(input, kEventsMagic)) {
    return records;
  }

  const auto file_size = file_size_or_zero(events_path);
  const auto committed = clamp_prefix(impl_->committed.events_committed_bytes, file_size);
  std::uint64_t cursor = kFileHeaderSize;
  while (cursor + kEventHeaderSize <= committed) {
    std::array<std::uint8_t, kEventHeaderSize> header_bytes{};
    if (!read_exact(input, header_bytes.data(), header_bytes.size())) {
      break;
    }
    cursor += kEventHeaderSize;

    RawEventRecord record;
    record.header = read_event_header(header_bytes);
    if (record.header.payload_len > committed - cursor) {
      break;
    }
    if (record.header.payload_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      break;
    }
    record.payload.resize(static_cast<std::size_t>(record.header.payload_len));
    if (!record.payload.empty() &&
        !read_exact(input, record.payload.data(), record.payload.size())) {
      break;
    }
    cursor += record.header.payload_len;
    records.push_back(std::move(record));
  }
  return records;
}

bool RawCaptureReader::read_blob(std::uint64_t raw_blob_id, std::vector<std::uint8_t> &bytes) const
{
  bytes.clear();
  RawBlobExtent extent;
  if (!find_blob_extent(raw_blob_id, extent)) {
    return false;
  }
  if (extent.offset < kFileHeaderSize ||
      extent.offset + extent.size < extent.offset ||
      extent.offset + extent.size > impl_->committed.blobs_committed_bytes) {
    return false;
  }
  if (extent.size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  std::ifstream input(raw_root_ / "blobs.bin", std::ios::binary);
  if (!input || !read_file_header(input, kBlobsMagic)) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(extent.offset), std::ios::beg);
  bytes.resize(static_cast<std::size_t>(extent.size));
  if (!bytes.empty() && !read_exact(input, bytes.data(), bytes.size())) {
    bytes.clear();
    return false;
  }
  return true;
}

bool RawCaptureReader::find_blob_extent(std::uint64_t raw_blob_id, RawBlobExtent &extent) const
{
  if (!impl_) {
    return false;
  }
  const auto it = impl_->blob_extent_by_id.find(raw_blob_id);
  if (it == impl_->blob_extent_by_id.end()) {
    return false;
  }
  extent = it->second;
  return true;
}

const std::vector<RawBlobExtent> &RawCaptureReader::blob_extents() const noexcept
{
  static const std::vector<RawBlobExtent> empty;
  return impl_ ? impl_->blob_extents : empty;
}

RawCommitMeta RawCaptureReader::committed_prefix() const noexcept
{
  return impl_ ? impl_->committed : RawCommitMeta{};
}

const std::string &RawCaptureReader::last_error() const noexcept
{
  static const std::string empty;
  return last_error_.empty() ? empty : last_error_;
}

bool RawCaptureReader::load_headers_and_commit()
{
  std::ifstream events(raw_root_ / "events.bin", std::ios::binary);
  std::ifstream blobs(raw_root_ / "blobs.bin", std::ios::binary);
  std::ifstream blob_index(raw_root_ / "blobs.idx", std::ios::binary);
  if (!events || !blobs || !blob_index) {
    return fail("missing raw capture files");
  }
  if (!read_file_header(events, kEventsMagic) ||
      !read_file_header(blobs, kBlobsMagic) ||
      !read_file_header(blob_index, kBlobIndexMagic)) {
    return fail("invalid raw capture file header");
  }
  if (!read_commit_meta_file(raw_root_ / "commit.meta", impl_->committed)) {
    return fail("invalid raw capture commit metadata");
  }

  impl_->committed.events_committed_bytes =
      clamp_prefix(impl_->committed.events_committed_bytes, file_size_or_zero(raw_root_ / "events.bin"));
  impl_->committed.blobs_committed_bytes =
      clamp_prefix(impl_->committed.blobs_committed_bytes, file_size_or_zero(raw_root_ / "blobs.bin"));
  impl_->committed.blob_index_committed_bytes =
      clamp_prefix(impl_->committed.blob_index_committed_bytes, file_size_or_zero(raw_root_ / "blobs.idx"));
  return true;
}

bool RawCaptureReader::load_blob_index()
{
  std::ifstream input(raw_root_ / "blobs.idx", std::ios::binary);
  if (!input || !read_file_header(input, kBlobIndexMagic)) {
    return fail("invalid raw blob index");
  }

  std::uint64_t cursor = kFileHeaderSize;
  const auto committed = impl_->committed.blob_index_committed_bytes;
  const auto blobs_committed = impl_->committed.blobs_committed_bytes;
  while (cursor + kBlobExtentRecordSize <= committed) {
    std::array<std::uint8_t, kBlobExtentRecordSize> bytes{};
    if (!read_exact(input, bytes.data(), bytes.size())) {
      break;
    }
    cursor += kBlobExtentRecordSize;
    auto extent = read_blob_extent(bytes);
    if (extent.offset < kFileHeaderSize ||
        extent.offset + extent.size < extent.offset ||
        extent.offset + extent.size > blobs_committed) {
      continue;
    }
    impl_->blob_extent_by_id[extent.raw_blob_id] = extent;
    impl_->blob_extents.push_back(extent);
  }
  return true;
}

bool RawCaptureReader::fail(std::string message)
{
  if (impl_) {
    last_error_ = std::move(message);
  } else {
    last_error_ = std::move(message);
  }
  return false;
}

} // namespace apitrace::trace::raw
