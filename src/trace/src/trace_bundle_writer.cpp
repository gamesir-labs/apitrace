#include "apitrace/trace_bundle_io.hpp"
#include "metal_callstream_writer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace apitrace::trace {

namespace {

using json = nlohmann::json;

constexpr const char *kObjectsDirectoryName = "objects";
constexpr const char *kObjectIndexFileName = "objects.json";
constexpr const char *kShadersDirectoryName = "shaders";
constexpr const char *kTexturesDirectoryName = "textures";
constexpr const char *kBuffersDirectoryName = "buffers";
constexpr const char *kPipelinesDirectoryName = "pipelines";
constexpr const char *kSidebandAssetShardFileName = "sideband-assets.json";

std::uint64_t monotonic_nanoseconds()
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void update_atomic_max(std::atomic_uint64_t &target, std::uint64_t value)
{
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

bool is_bundle_relative_path(const std::filesystem::path &path)
{
  if (path.empty() || path == std::filesystem::path(".") || path.is_absolute()) {
    return false;
  }

  for (const auto &component : path) {
    if (component == std::filesystem::path("..")) {
      return false;
    }
  }

  return true;
}

std::optional<std::filesystem::path> bundle_relative_path(
    const std::filesystem::path &path,
    const std::filesystem::path &root)
{
  if (path.empty() || root.empty()) {
    return std::nullopt;
  }

  const auto lexical_relative = path.lexically_normal().lexically_relative(root.lexically_normal());
  if (is_bundle_relative_path(lexical_relative)) {
    return lexical_relative;
  }

  std::error_code error;
  auto relative = std::filesystem::relative(path, root, error);
  if (error) {
    return std::nullopt;
  }

  relative = relative.lexically_normal();
  if (!is_bundle_relative_path(relative)) {
    return std::nullopt;
  }

  return relative;
}

struct AsyncStreamStats {
  std::uint64_t mutex_lock_count = 0;
  std::uint64_t mutex_wait_ns = 0;
  std::uint64_t mutex_max_wait_ns = 0;
  std::uint64_t mutex_hold_ns = 0;
  std::uint64_t mutex_max_hold_ns = 0;
  std::uint64_t enqueue_count = 0;
  std::uint64_t enqueue_bytes = 0;
  std::uint64_t wait_count = 0;
  std::uint64_t wait_ns = 0;
  std::uint64_t max_wait_ns = 0;
  std::uint64_t serialize_count = 0;
  std::uint64_t serialize_ns = 0;
  std::uint64_t max_serialize_ns = 0;
  std::uint64_t write_count = 0;
  std::uint64_t write_bytes = 0;
  std::uint64_t write_ns = 0;
  std::uint64_t max_write_ns = 0;
};

struct WriterLockStats {
  std::atomic_uint64_t lock_count{0};
  std::atomic_uint64_t wait_ns{0};
  std::atomic_uint64_t max_wait_ns{0};
  std::atomic_uint64_t hold_ns{0};
  std::atomic_uint64_t max_hold_ns{0};
};

struct WriterLockStatsSnapshot {
  std::uint64_t lock_count = 0;
  std::uint64_t wait_ns = 0;
  std::uint64_t max_wait_ns = 0;
  std::uint64_t hold_ns = 0;
  std::uint64_t max_hold_ns = 0;
};

struct WriterPhaseStats {
  std::uint64_t count = 0;
  std::uint64_t ns = 0;
  std::uint64_t max_ns = 0;
};

template <typename T>
T delta_value(T current, T previous)
{
  return current >= previous ? current - previous : T{};
}

WriterPhaseStats delta_phase(const WriterPhaseStats &current, const WriterPhaseStats &previous)
{
  return WriterPhaseStats{
      delta_value(current.count, previous.count),
      delta_value(current.ns, previous.ns),
      current.max_ns,
  };
}

AsyncStreamStats delta_stream_stats(const AsyncStreamStats &current, const AsyncStreamStats &previous)
{
  AsyncStreamStats value;
  value.mutex_lock_count = delta_value(current.mutex_lock_count, previous.mutex_lock_count);
  value.mutex_wait_ns = delta_value(current.mutex_wait_ns, previous.mutex_wait_ns);
  value.mutex_max_wait_ns = current.mutex_max_wait_ns;
  value.mutex_hold_ns = delta_value(current.mutex_hold_ns, previous.mutex_hold_ns);
  value.mutex_max_hold_ns = current.mutex_max_hold_ns;
  value.enqueue_count = delta_value(current.enqueue_count, previous.enqueue_count);
  value.enqueue_bytes = delta_value(current.enqueue_bytes, previous.enqueue_bytes);
  value.wait_count = delta_value(current.wait_count, previous.wait_count);
  value.wait_ns = delta_value(current.wait_ns, previous.wait_ns);
  value.max_wait_ns = current.max_wait_ns;
  value.serialize_count = delta_value(current.serialize_count, previous.serialize_count);
  value.serialize_ns = delta_value(current.serialize_ns, previous.serialize_ns);
  value.max_serialize_ns = current.max_serialize_ns;
  value.write_count = delta_value(current.write_count, previous.write_count);
  value.write_bytes = delta_value(current.write_bytes, previous.write_bytes);
  value.write_ns = delta_value(current.write_ns, previous.write_ns);
  value.max_write_ns = current.max_write_ns;
  return value;
}

WriterLockStatsSnapshot delta_lock_stats(
    const WriterLockStatsSnapshot &current,
    const WriterLockStatsSnapshot &previous)
{
  return WriterLockStatsSnapshot{
      delta_value(current.lock_count, previous.lock_count),
      delta_value(current.wait_ns, previous.wait_ns),
      current.max_wait_ns,
      delta_value(current.hold_ns, previous.hold_ns),
      current.max_hold_ns,
  };
}

struct AtomicWriterPhaseStats {
  std::atomic_uint64_t count{0};
  std::atomic_uint64_t ns{0};
  std::atomic_uint64_t max_ns{0};
};

WriterLockStatsSnapshot snapshot_writer_lock_stats(const WriterLockStats &stats)
{
  return WriterLockStatsSnapshot{
      stats.lock_count.load(std::memory_order_relaxed),
      stats.wait_ns.load(std::memory_order_relaxed),
      stats.max_wait_ns.load(std::memory_order_relaxed),
      stats.hold_ns.load(std::memory_order_relaxed),
      stats.max_hold_ns.load(std::memory_order_relaxed),
  };
}

void record_writer_lock_wait(WriterLockStats *stats, std::uint64_t wait_ns)
{
  if (!stats)
    return;
  stats->lock_count.fetch_add(1, std::memory_order_relaxed);
  stats->wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
  update_atomic_max(stats->max_wait_ns, wait_ns);
}

void record_writer_lock_hold(WriterLockStats *stats, std::uint64_t hold_ns)
{
  if (!stats)
    return;
  stats->hold_ns.fetch_add(hold_ns, std::memory_order_relaxed);
  update_atomic_max(stats->max_hold_ns, hold_ns);
}

void record_writer_phase(WriterPhaseStats &stats, std::uint64_t elapsed_ns)
{
  ++stats.count;
  stats.ns += elapsed_ns;
  stats.max_ns = std::max(stats.max_ns, elapsed_ns);
}

WriterPhaseStats snapshot_writer_phase_stats(const AtomicWriterPhaseStats &stats)
{
  return WriterPhaseStats{
      stats.count.load(std::memory_order_relaxed),
      stats.ns.load(std::memory_order_relaxed),
      stats.max_ns.load(std::memory_order_relaxed),
  };
}

void record_writer_phase(AtomicWriterPhaseStats &stats, std::uint64_t elapsed_ns)
{
  stats.count.fetch_add(1, std::memory_order_relaxed);
  stats.ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
  update_atomic_max(stats.max_ns, elapsed_ns);
}

class TimedWriterPhase {
public:
  TimedWriterPhase(WriterPhaseStats *stats) : stats_(stats), start_ns_(stats ? monotonic_nanoseconds() : 0) {}
  TimedWriterPhase(AtomicWriterPhaseStats *stats) : atomic_stats_(stats), start_ns_(stats ? monotonic_nanoseconds() : 0) {}
  TimedWriterPhase(const TimedWriterPhase &) = delete;
  TimedWriterPhase &operator=(const TimedWriterPhase &) = delete;

  ~TimedWriterPhase()
  {
    if (stats_)
      record_writer_phase(*stats_, monotonic_nanoseconds() - start_ns_);
    if (atomic_stats_)
      record_writer_phase(*atomic_stats_, monotonic_nanoseconds() - start_ns_);
  }

private:
  WriterPhaseStats *stats_ = nullptr;
  AtomicWriterPhaseStats *atomic_stats_ = nullptr;
  std::uint64_t start_ns_ = 0;
};

class TimedAtomicWriterPhase {
public:
  TimedAtomicWriterPhase(AtomicWriterPhaseStats *stats) : stats_(stats), start_ns_(stats ? monotonic_nanoseconds() : 0) {}
  TimedAtomicWriterPhase(const TimedAtomicWriterPhase &) = delete;
  TimedAtomicWriterPhase &operator=(const TimedAtomicWriterPhase &) = delete;

  ~TimedAtomicWriterPhase()
  {
    if (stats_)
      record_writer_phase(*stats_, monotonic_nanoseconds() - start_ns_);
  }

private:
  AtomicWriterPhaseStats *stats_ = nullptr;
  std::uint64_t start_ns_ = 0;
};

class TimedWriterLock {
public:
  TimedWriterLock(std::mutex &mutex, WriterLockStats *stats) : mutex_(mutex), stats_(stats)
  {
    const auto wait_start_ns = stats_ ? monotonic_nanoseconds() : 0;
    mutex_.lock();
    acquired_ns_ = stats_ ? monotonic_nanoseconds() : 0;
    if (stats_)
      record_writer_lock_wait(stats_, acquired_ns_ - wait_start_ns);
  }

  TimedWriterLock(const TimedWriterLock &) = delete;
  TimedWriterLock &operator=(const TimedWriterLock &) = delete;

  TimedWriterLock(TimedWriterLock &&other) noexcept
      : mutex_(other.mutex_), stats_(other.stats_), acquired_ns_(other.acquired_ns_), owns_(other.owns_)
  {
    other.owns_ = false;
  }

  ~TimedWriterLock()
  {
    if (!owns_)
      return;
    if (stats_)
      record_writer_lock_hold(stats_, monotonic_nanoseconds() - acquired_ns_);
    mutex_.unlock();
  }

private:
  std::mutex &mutex_;
  WriterLockStats *stats_ = nullptr;
  std::uint64_t acquired_ns_ = 0;
  bool owns_ = true;
};

class TimedTryWriterLock {
public:
  TimedTryWriterLock(std::mutex &mutex, WriterLockStats *stats) : mutex_(mutex), stats_(stats)
  {
    if (!mutex_.try_lock())
      return;
    owns_ = true;
    acquired_ns_ = stats_ ? monotonic_nanoseconds() : 0;
    if (stats_)
      record_writer_lock_wait(stats_, 0);
  }

  TimedTryWriterLock(const TimedTryWriterLock &) = delete;
  TimedTryWriterLock &operator=(const TimedTryWriterLock &) = delete;

  ~TimedTryWriterLock()
  {
    if (!owns_)
      return;
    if (stats_)
      record_writer_lock_hold(stats_, monotonic_nanoseconds() - acquired_ns_);
    mutex_.unlock();
  }

  [[nodiscard]] bool owns_lock() const
  {
    return owns_;
  }

  explicit operator bool() const
  {
    return owns_;
  }

private:
  std::mutex &mutex_;
  WriterLockStats *stats_ = nullptr;
  std::uint64_t acquired_ns_ = 0;
  bool owns_ = false;
};

std::unique_lock<std::mutex> timed_unique_lock(std::mutex &mutex, WriterLockStats *stats)
{
  std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
  const auto wait_start_ns = stats ? monotonic_nanoseconds() : 0;
  lock.lock();
  if (stats)
    record_writer_lock_wait(stats, monotonic_nanoseconds() - wait_start_ns);
  return lock;
}


class Sha256 {
public:
  void update(const std::uint8_t *data, std::size_t size)
  {
    for (std::size_t index = 0; index < size; ++index) {
      buffer_[buffer_size_++] = data[index];
      bit_count_ += 8;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_.data());
        buffer_size_ = 0;
      }
    }
  }

  void update(const std::vector<std::uint8_t> &data)
  {
    update(data.data(), data.size());
  }

  void update(std::string_view text)
  {
    update(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  }

  [[nodiscard]] std::string final_hex()
  {
    auto local_buffer = buffer_;
    const auto local_buffer_size = buffer_size_;
    const auto local_bit_count = bit_count_;

    local_buffer[local_buffer_size] = 0x80;
    for (std::size_t index = local_buffer_size + 1; index < local_buffer.size(); ++index) {
      local_buffer[index] = 0;
    }

    if (local_buffer_size >= 56) {
      transform(local_buffer.data());
      local_buffer.fill(0);
    }

    const std::uint64_t bit_count_be = byteswap64(local_bit_count);
    std::memcpy(local_buffer.data() + 56, &bit_count_be, sizeof(bit_count_be));
    transform(local_buffer.data());

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::uint32_t value : state_) {
      output << std::setw(8) << value;
    }
    return output.str();
  }

private:
  static constexpr std::array<std::uint32_t, 64> kTable = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  };

  static std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count)
  {
    return (value >> count) | (value << (32 - count));
  }

  static std::uint32_t byteswap32(std::uint32_t value)
  {
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
  }

  static std::uint64_t byteswap64(std::uint64_t value)
  {
    return ((value & 0x00000000000000ffull) << 56) |
           ((value & 0x000000000000ff00ull) << 40) |
           ((value & 0x0000000000ff0000ull) << 24) |
           ((value & 0x00000000ff000000ull) << 8) |
           ((value & 0x000000ff00000000ull) >> 8) |
           ((value & 0x0000ff0000000000ull) >> 24) |
           ((value & 0x00ff000000000000ull) >> 40) |
           ((value & 0xff00000000000000ull) >> 56);
  }

  void transform(const std::uint8_t *chunk)
  {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
      std::uint32_t word = 0;
      std::memcpy(&word, chunk + (index * 4), sizeof(word));
      schedule[index] = byteswap32(word);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
      const auto s0 = rotate_right(schedule[index - 15], 7) ^ rotate_right(schedule[index - 15], 18) ^
                      (schedule[index - 15] >> 3);
      const auto s1 = rotate_right(schedule[index - 2], 17) ^ rotate_right(schedule[index - 2], 19) ^
                      (schedule[index - 2] >> 10);
      schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < schedule.size(); ++index) {
      const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temp1 = h + s1 + choose + kTable[index] + schedule[index];
      const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667,
      0xbb67ae85,
      0x3c6ef372,
      0xa54ff53a,
      0x510e527f,
      0x9b05688c,
      0x1f83d9ab,
      0x5be0cd19,
  };
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_ = 0;
  std::uint64_t bit_count_ = 0;
};

std::string sha256_bytes(const std::vector<std::uint8_t> &bytes)
{
  Sha256 sha256;
  sha256.update(bytes);
  return sha256.final_hex();
}

std::string sha256_text(std::string_view text)
{
  Sha256 sha256;
  sha256.update(text);
  return sha256.final_hex();
}

std::uint64_t fnv1a_update(std::uint64_t hash, const std::uint8_t *data, std::size_t size)
{
  constexpr std::uint64_t kPrime = 1099511628211ull;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= kPrime;
  }
  return hash;
}

std::string sha256_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  Sha256 sha256;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  while (input.good()) {
    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = static_cast<std::size_t>(input.gcount());
    if (read_count != 0) {
      sha256.update(buffer.data(), read_count);
    }
  }
  return sha256.final_hex();
}

std::uint64_t regular_file_size_or_zero(const std::filesystem::path &path)
{
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return 0;
  }
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

std::string bundle_hash_from_records(const std::vector<ChecksumRecord> &files)
{
  auto sorted_files = files;
  std::sort(sorted_files.begin(), sorted_files.end(), [](const ChecksumRecord &lhs, const ChecksumRecord &rhs) {
    return lhs.relative_path.generic_string() < rhs.relative_path.generic_string();
  });
  std::string bundle_fingerprint_source;
  for (const auto &record : sorted_files) {
    bundle_fingerprint_source += record.relative_path.generic_string();
    bundle_fingerprint_source += "=";
    bundle_fingerprint_source += record.digest;
    if (record.has_byte_size) {
      bundle_fingerprint_source += "#";
      bundle_fingerprint_source += std::to_string(record.byte_size);
    }
    bundle_fingerprint_source += "\n";
  }
  return "sha256:" + sha256_text(bundle_fingerprint_source);
}

std::vector<ChecksumRecord> unique_checksum_records(const std::vector<ChecksumRecord> &files)
{
  std::vector<ChecksumRecord> unique;
  std::unordered_map<std::string, std::size_t> indices;
  unique.reserve(files.size());
  indices.reserve(files.size());
  for (const auto &record : files) {
    const auto key = record.relative_path.generic_string();
    const auto existing = indices.find(key);
    if (existing == indices.end()) {
      indices.emplace(key, unique.size());
      unique.push_back(record);
    } else {
      unique[existing->second] = record;
    }
  }
  return unique;
}

std::string json_escape(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (const unsigned char ch : text) {
    switch (ch) {
    case '\"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (ch < 0x20) {
        std::ostringstream codepoint;
        codepoint << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
        escaped += codepoint.str();
      } else {
        escaped.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  return escaped;
}

std::string api_name(ApiKind api)
{
  switch (api) {
  case ApiKind::D3D11:
    return "D3D11";
  case ApiKind::D3D12:
    return "D3D12";
  default:
    return "Unknown";
  }
}

std::string boundary_name(BoundaryKind boundary)
{
  switch (boundary) {
  case BoundaryKind::Frame:
    return "Frame";
  case BoundaryKind::CommandList:
    return "CommandList";
  case BoundaryKind::Submit:
    return "Submit";
  case BoundaryKind::Present:
    return "Present";
  case BoundaryKind::Fence:
    return "Fence";
  case BoundaryKind::Barrier:
    return "Barrier";
  case BoundaryKind::DebugMarker:
    return "DebugMarker";
  }
  return "Unknown";
}

std::string object_kind_name(ObjectKind kind)
{
  switch (kind) {
  case ObjectKind::Device:
    return "Device";
  case ObjectKind::Context:
    return "Context";
  case ObjectKind::CommandQueue:
    return "CommandQueue";
  case ObjectKind::CommandAllocator:
    return "CommandAllocator";
  case ObjectKind::CommandList:
    return "CommandList";
  case ObjectKind::CommandSignature:
    return "CommandSignature";
  case ObjectKind::Fence:
    return "Fence";
  case ObjectKind::SwapChain:
    return "SwapChain";
  case ObjectKind::Heap:
    return "Heap";
  case ObjectKind::Resource:
    return "Resource";
  case ObjectKind::View:
    return "View";
  case ObjectKind::Shader:
    return "Shader";
  case ObjectKind::PipelineState:
    return "PipelineState";
  case ObjectKind::RootSignature:
    return "RootSignature";
  case ObjectKind::DescriptorHeap:
    return "DescriptorHeap";
  case ObjectKind::QueryHeap:
    return "QueryHeap";
  case ObjectKind::Unknown:
  default:
    return "Unknown";
  }
}

std::string asset_extension(AssetKind kind)
{
  switch (kind) {
  case AssetKind::ShaderDxbc:
    return ".dxbc";
  case AssetKind::ShaderDxil:
    return ".dxil";
  case AssetKind::RootSignature:
    return ".rootsig";
  case AssetKind::Texture:
    return ".texture";
  case AssetKind::Buffer:
    return ".buffer";
  case AssetKind::Pipeline:
    return ".pipeline.json";
  case AssetKind::ObjectIndex:
    return ".json";
  case AssetKind::Analysis:
    return ".jsonl";
  case AssetKind::Unknown:
  default:
    return ".bin";
  }
}

std::filesystem::path asset_directory(const BundleLayout &layout, AssetKind kind)
{
  switch (kind) {
  case AssetKind::ShaderDxbc:
  case AssetKind::ShaderDxil:
  case AssetKind::RootSignature:
    return layout.shaders_directory_path;
  case AssetKind::Texture:
    return layout.textures_directory_path;
  case AssetKind::Buffer:
    return layout.buffers_directory_path;
  case AssetKind::Pipeline:
    return layout.pipelines_directory_path;
  case AssetKind::ObjectIndex:
    return layout.objects_directory_path;
  case AssetKind::Analysis:
    return layout.analysis_directory_path;
  case AssetKind::Unknown:
  default:
    return layout.root_path;
  }
}

std::string asset_directory_name(AssetKind kind)
{
  switch (kind) {
  case AssetKind::ShaderDxbc:
  case AssetKind::ShaderDxil:
  case AssetKind::RootSignature:
    return kShadersDirectoryName;
  case AssetKind::Texture:
    return kTexturesDirectoryName;
  case AssetKind::Buffer:
    return kBuffersDirectoryName;
  case AssetKind::Pipeline:
    return kPipelinesDirectoryName;
  case AssetKind::ObjectIndex:
    return kObjectsDirectoryName;
  case AssetKind::Analysis:
    return kAnalysisDirectoryName;
  case AssetKind::Unknown:
  default:
    return ".";
  }
}

std::string encode_integer_array(const std::vector<std::uint64_t> &values)
{
  std::ostringstream output;
  output << "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ",";
    }
    output << values[index];
  }
  output << "]";
  return output.str();
}

std::string normalize_payload(std::string_view payload)
{
  if (payload.empty()) {
    return "{}";
  }
  return std::string(payload);
}

void add_checksum_candidate(
    std::vector<std::filesystem::path> &relative_paths,
    std::unordered_set<std::string> &seen,
    const std::filesystem::path &relative_path)
{
  if (relative_path.empty() || relative_path == std::filesystem::path(kChecksumsFileName) ||
      relative_path.is_absolute()) {
    return;
  }

  const auto key = relative_path.generic_string();
  if (key.empty() || key == "." || key.find("..") != std::string::npos) {
    return;
  }

  if (seen.insert(key).second) {
    relative_paths.push_back(relative_path);
  }
}

void add_checksum_candidate_absolute(
    const BundleLayout &layout,
    std::vector<std::filesystem::path> &relative_paths,
    std::unordered_set<std::string> &seen,
    const std::filesystem::path &absolute_path)
{
  if (absolute_path.empty()) {
    return;
  }

  std::error_code error;
  if (!std::filesystem::is_regular_file(absolute_path, error) || error) {
    return;
  }

  const auto relative_path = bundle_relative_path(absolute_path, layout.root_path);
  if (!relative_path) {
    return;
  }
  add_checksum_candidate(relative_paths, seen, *relative_path);
}

std::vector<std::filesystem::path> collect_bundle_relative_paths_full_scan(const BundleLayout &layout)
{
  std::vector<std::filesystem::path> relative_paths;
  if (!std::filesystem::exists(layout.root_path)) {
    return relative_paths;
  }

  for (const auto &entry : std::filesystem::recursive_directory_iterator(layout.root_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    const auto relative_path = bundle_relative_path(entry.path(), layout.root_path);
    if (!relative_path || *relative_path == std::filesystem::path(kChecksumsFileName)) {
      continue;
    }
    relative_paths.push_back(*relative_path);
  }

  std::sort(relative_paths.begin(), relative_paths.end());
  relative_paths.erase(std::unique(relative_paths.begin(), relative_paths.end()), relative_paths.end());
  return relative_paths;
}

std::vector<std::filesystem::path> collect_writer_known_relative_paths(
    const BundleLayout &layout,
    const std::unordered_map<std::string, std::string> &known_file_digests,
    const std::vector<std::filesystem::path> &rewritten_paths)
{
  std::vector<std::filesystem::path> relative_paths;
  std::unordered_set<std::string> seen;

  for (const auto &entry : known_file_digests) {
    add_checksum_candidate(relative_paths, seen, std::filesystem::path(entry.first));
  }
  for (const auto &relative_path : rewritten_paths) {
    add_checksum_candidate(relative_paths, seen, relative_path);
  }

  add_checksum_candidate_absolute(layout, relative_paths, seen, layout.callstream_path);
  add_checksum_candidate_absolute(layout, relative_paths, seen, layout.metal_callstream_path);
  add_checksum_candidate_absolute(layout, relative_paths, seen, layout.asset_index_path);
  add_checksum_candidate_absolute(layout, relative_paths, seen, layout.object_index_path);
  add_checksum_candidate_absolute(layout, relative_paths, seen, layout.translation_links_path);

  std::sort(relative_paths.begin(), relative_paths.end());
  relative_paths.erase(std::unique(relative_paths.begin(), relative_paths.end()), relative_paths.end());
  return relative_paths;
}

void write_text_atomic(const std::filesystem::path &path, std::string_view text)
{
  std::filesystem::create_directories(path.parent_path());
  auto temporary_path = path;
  temporary_path += ".tmp";
  {
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
  }

  std::error_code error;
  std::filesystem::rename(temporary_path, path, error);
  if (!error) {
    return;
  }

  error.clear();
  std::filesystem::copy_file(
      temporary_path,
      path,
      std::filesystem::copy_options::overwrite_existing,
      error);
  std::error_code remove_error;
  std::filesystem::remove(temporary_path, remove_error);
}

std::string event_record_json(const EventRecord &event)
{
  std::ostringstream output;
  if (event.kind == EventKind::Boundary) {
    output << "{\"record_kind\":\"boundary\""
           << ",\"sequence\":" << event.callsite.sequence
           << ",\"boundary\":\"" << boundary_name(event.boundary) << "\""
           << ",\"payload\":" << normalize_payload(event.payload)
           << "}";
    return output.str();
  }

  if (event.kind == EventKind::ObjectCreate || event.kind == EventKind::ObjectDestroy || event.kind == EventKind::ResourceBlob) {
    output << "{\"record_kind\":\""
           << (event.kind == EventKind::ObjectCreate ? "object_create" :
               event.kind == EventKind::ObjectDestroy ? "object_destroy" : "resource_blob")
           << "\""
           << ",\"sequence\":" << event.callsite.sequence
           << ",\"object_id\":" << event.object_id
           << ",\"object_kind\":\"" << object_kind_name(event.object_kind) << "\""
           << ",\"parent_object_id\":" << event.parent_object_id
           << ",\"debug_name\":\"" << json_escape(event.object_debug_name) << "\""
           << ",\"object_refs\":" << encode_integer_array(
                  std::vector<std::uint64_t>(event.object_refs.begin(), event.object_refs.end()))
           << ",\"blob_refs\":" << encode_integer_array(
                  std::vector<std::uint64_t>(event.blob_refs.begin(), event.blob_refs.end()));
    if (!event.payload.empty() && event.payload != "{}") {
      output << ",\"payload\":" << normalize_payload(event.payload);
    }
    output << "}";
    return output.str();
  }

  output << "{\"record_kind\":\"call\""
         << ",\"sequence\":" << event.callsite.sequence
         << ",\"function\":\"" << json_escape(event.callsite.function_name) << "\""
         << ",\"result_code\":" << event.callsite.result_code;
  if (!event.object_refs.empty()) {
    output << ",\"object_refs\":" << encode_integer_array(
                  std::vector<std::uint64_t>(event.object_refs.begin(), event.object_refs.end()));
  }
  if (!event.blob_refs.empty()) {
    output << ",\"blob_refs\":" << encode_integer_array(
                  std::vector<std::uint64_t>(event.blob_refs.begin(), event.blob_refs.end()));
  }
  output << ",\"payload\":" << normalize_payload(event.payload)
         << "}";
  return output.str();
}

std::string checksum_index_json(const ChecksumIndex &checksums)
{
  const auto files = unique_checksum_records(checksums.files);
  std::ostringstream output;
  output << "{\n"
         << "  \"format_version\": " << checksums.format_version << ",\n"
         << "  \"bundle_hash\": \"" << json_escape(checksums.bundle_hash) << "\",\n"
         << "  \"files\": {\n";
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto &record = files[index];
    output << "    \"" << json_escape(record.relative_path.generic_string()) << "\": \""
           << json_escape(
                  record.algorithm + ":" + record.digest +
                  (record.has_byte_size ? (":" + std::to_string(record.byte_size)) : std::string())) << "\"";
    if (index + 1 != files.size()) {
      output << ",";
    }
    output << "\n";
  }
  output << "  }\n"
         << "}\n";
  return output.str();
}

std::string object_index_json(const std::vector<ObjectRecord> &objects)
{
  std::ostringstream output;
  output << "{\n"
         << "  \"objects\": [\n";
  for (std::size_t index = 0; index < objects.size(); ++index) {
    const auto &object = objects[index];
    output << "    {\n"
           << "      \"object_id\": " << object.object_id << ",\n"
           << "      \"object_kind\": \"" << object_kind_name(object.kind) << "\",\n"
           << "      \"parent_object_id\": " << object.parent_object_id << ",\n"
           << "      \"debug_name\": \"" << json_escape(object.debug_name) << "\"\n"
           << "    }";
    if (index + 1 != objects.size()) {
      output << ",";
    }
    output << "\n";
  }
  output << "  ]\n"
         << "}\n";
  return output.str();
}

std::string asset_kind_name(AssetKind kind)
{
  switch (kind) {
  case AssetKind::Unknown:
    return "Unknown";
  case AssetKind::ShaderDxbc:
    return "ShaderDxbc";
  case AssetKind::ShaderDxil:
    return "ShaderDxil";
  case AssetKind::RootSignature:
    return "RootSignature";
  case AssetKind::Texture:
    return "Texture";
  case AssetKind::Buffer:
    return "Buffer";
  case AssetKind::Pipeline:
    return "Pipeline";
  case AssetKind::ObjectIndex:
    return "ObjectIndex";
  case AssetKind::Analysis:
    return "Analysis";
  }
  return "Unknown";
}

AssetKind asset_kind_from_name(std::string_view name)
{
  if (name == "ShaderDxbc") {
    return AssetKind::ShaderDxbc;
  }
  if (name == "ShaderDxil") {
    return AssetKind::ShaderDxil;
  }
  if (name == "RootSignature") {
    return AssetKind::RootSignature;
  }
  if (name == "Texture") {
    return AssetKind::Texture;
  }
  if (name == "Buffer") {
    return AssetKind::Buffer;
  }
  if (name == "Pipeline") {
    return AssetKind::Pipeline;
  }
  if (name == "ObjectIndex") {
    return AssetKind::ObjectIndex;
  }
  if (name == "Analysis") {
    return AssetKind::Analysis;
  }
  return AssetKind::Unknown;
}

void append_asset_index_entries(
    std::ostringstream &output,
    const std::vector<AssetRecord> &assets,
    const std::filesystem::path &bundle_root,
    const std::unordered_map<std::string, std::string> &path_aliases,
    const std::unordered_map<std::string, std::string> &known_file_digests,
    std::unordered_map<std::string, std::uint64_t> &known_file_sizes,
    bool metal,
    bool &first)
{
  std::unordered_set<std::string> seen;
  for (const auto &asset : assets) {
    if (asset.blob_id == 0 || asset.relative_path.empty()) {
      continue;
    }
    auto relative_path = asset.relative_path.generic_string();
    if (const auto alias = path_aliases.find(relative_path); alias != path_aliases.end()) {
      relative_path = alias->second;
    }
    auto content_hash = asset.content_hash;
    if (const auto digest = known_file_digests.find(relative_path); digest != known_file_digests.end())
      content_hash = digest->second;
    auto byte_size = asset.byte_size;
    if (const auto known_size = known_file_sizes.find(relative_path); known_size != known_file_sizes.end()) {
      byte_size = known_size->second;
    } else {
      std::error_code stat_error;
      const auto actual_size = std::filesystem::file_size(bundle_root / relative_path, stat_error);
      if (!stat_error) {
        byte_size = actual_size;
        known_file_sizes.emplace(relative_path, actual_size);
      }
    }
    const auto key = std::to_string(asset.blob_id) + "#" + relative_path + "#" + (metal ? "metal" : "d3d");
    if (!seen.insert(key).second) {
      continue;
    }
    if (!first) {
      output << ",\n";
    }
    first = false;
    output << "    {"
           << "\"blob_id\":" << asset.blob_id
           << ",\"path\":\"" << json_escape(relative_path) << "\""
           << ",\"kind\":\"" << asset_kind_name(asset.kind) << "\""
           << ",\"metal\":" << (metal ? "true" : "false")
           << ",\"binary_payload\":" << (asset.binary_payload ? "true" : "false")
           << ",\"byte_size\":" << byte_size;
    if (!asset.debug_name.empty())
      output << ",\"debug_name\":\"" << json_escape(asset.debug_name) << "\"";
    if (!content_hash.empty())
      output << ",\"content_hash\":\"" << json_escape(content_hash) << "\"";
    output
           << "}";
  }
}

std::string asset_index_json(
    const std::vector<AssetRecord> &assets,
    const std::vector<AssetRecord> &metal_assets,
    const std::filesystem::path &bundle_root,
    const std::unordered_map<std::string, std::string> &path_aliases,
    const std::unordered_map<std::string, std::string> &known_file_digests,
    std::unordered_map<std::string, std::uint64_t> known_file_sizes)
{
  std::ostringstream output;
  bool first = true;
  output << "{\n"
         << "  \"assets\": [\n";
  append_asset_index_entries(output, assets, bundle_root, path_aliases, known_file_digests, known_file_sizes, false, first);
  append_asset_index_entries(output, metal_assets, bundle_root, path_aliases, known_file_digests, known_file_sizes, true, first);
  if (!first) {
    output << "\n";
  }
  output << "  ]\n"
         << "}\n";
  return output.str();
}

std::optional<AssetRecord> asset_record_from_json(const json &entry)
{
  if (!entry.is_object()) {
    return std::nullopt;
  }

  AssetRecord asset;
  asset.blob_id = entry.value("blob_id", 0ull);
  asset.relative_path = entry.value("path", std::string());
  asset.kind = asset_kind_from_name(entry.value("kind", std::string("Unknown")));
  asset.debug_name = entry.value("debug_name", std::string());
  asset.content_hash = entry.value("content_hash", std::string());
  asset.fast_fingerprint = entry.value("fast_fingerprint", std::string());
  asset.byte_size = entry.value("byte_size", 0ull);
  asset.binary_payload = entry.value("binary_payload", true);
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return std::nullopt;
  }
  return asset;
}

void merge_asset_record(std::vector<AssetRecord> &records, const AssetRecord &asset)
{
  for (auto &record : records) {
    if (record.blob_id == asset.blob_id) {
      record = asset;
      return;
    }
  }
  records.push_back(asset);
}

std::uint64_t max_blob_id(const std::vector<AssetRecord> &assets, const std::vector<AssetRecord> &metal_assets)
{
  std::uint64_t max_id = 0;
  for (const auto &asset : assets)
    max_id = std::max(max_id, static_cast<std::uint64_t>(asset.blob_id));
  for (const auto &asset : metal_assets)
    max_id = std::max(max_id, static_cast<std::uint64_t>(asset.blob_id));
  return max_id;
}

bool blob_id_maps_to_path(
    const std::vector<AssetRecord> &assets,
    const std::vector<AssetRecord> &metal_assets,
    std::uint64_t blob_id,
    const std::filesystem::path &relative_path)
{
  for (const auto &asset : assets) {
    if (asset.blob_id == blob_id)
      return asset.relative_path == relative_path;
  }
  for (const auto &asset : metal_assets) {
    if (asset.blob_id == blob_id)
      return asset.relative_path == relative_path;
  }
  return false;
}

bool blob_id_is_used(
    const std::vector<AssetRecord> &assets,
    const std::vector<AssetRecord> &metal_assets,
    std::uint64_t blob_id)
{
  for (const auto &asset : assets) {
    if (asset.blob_id == blob_id)
      return true;
  }
  for (const auto &asset : metal_assets) {
    if (asset.blob_id == blob_id)
      return true;
  }
  return false;
}

std::uint64_t allocate_unique_blob_id(
    const std::vector<AssetRecord> &assets,
    const std::vector<AssetRecord> &metal_assets,
    std::uint64_t &next_blob_id)
{
  do {
    ++next_blob_id;
  } while (next_blob_id == 0 || blob_id_is_used(assets, metal_assets, next_blob_id));
  return next_blob_id;
}

std::unordered_map<std::uint64_t, std::uint64_t> merge_sideband_asset_shard(
    const std::filesystem::path &path,
    std::vector<AssetRecord> &assets,
    std::vector<AssetRecord> &metal_assets)
{
  std::unordered_map<std::uint64_t, std::uint64_t> blob_remap;
  if (!std::filesystem::is_regular_file(path)) {
    return blob_remap;
  }

  std::ifstream input(path);
  const auto root = json::parse(input, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return blob_remap;
  }
  const auto list = root.find("assets");
  if (list == root.end() || !list->is_array()) {
    return blob_remap;
  }

  auto next_blob_id = max_blob_id(assets, metal_assets);
  for (const auto &entry : *list) {
    auto asset = asset_record_from_json(entry);
    if (!asset) {
      continue;
    }
    const auto original_blob_id = static_cast<std::uint64_t>(asset->blob_id);
    if (blob_id_is_used(assets, metal_assets, original_blob_id) &&
        !blob_id_maps_to_path(assets, metal_assets, original_blob_id, asset->relative_path)) {
      asset->blob_id = allocate_unique_blob_id(assets, metal_assets, next_blob_id);
      blob_remap.emplace(original_blob_id, static_cast<std::uint64_t>(asset->blob_id));
    }
    if (entry.value("metal", false)) {
      merge_asset_record(metal_assets, *asset);
    } else {
      merge_asset_record(assets, *asset);
    }
  }
  return blob_remap;
}

void add_asset_index_paths(
    std::vector<std::filesystem::path> &relative_paths,
    std::unordered_set<std::string> &seen,
    const std::filesystem::path &asset_index_path)
{
  if (!std::filesystem::is_regular_file(asset_index_path)) {
    return;
  }

  std::ifstream input(asset_index_path);
  const auto root = json::parse(input, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return;
  }

  const auto list = root.find("assets");
  if (list == root.end() || !list->is_array()) {
    return;
  }

  for (const auto &entry : *list) {
    const auto path = entry.find("path");
    if (path != entry.end() && path->is_string()) {
      add_checksum_candidate(relative_paths, seen, std::filesystem::path(path->get<std::string>()));
    }
  }
}

std::optional<std::pair<std::string, std::uint64_t>> rewrite_metal_callstream_blob_refs(
    const BundleLayout &layout,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_remap)
{
  if (blob_remap.empty())
    return std::nullopt;

  const auto path = layout.root_path / kMetalCallstreamFileName;
  if (!std::filesystem::is_regular_file(path))
    return std::nullopt;

  const auto temp_path = layout.root_path / (std::string(kMetalCallstreamFileName) + ".blob-remap.tmp");
  std::ifstream input(path, std::ios::binary);
  std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
  if (!input || !output)
    return std::nullopt;

  std::string line;
  bool changed = false;
  while (std::getline(input, line)) {
    auto record = json::parse(line, nullptr, false);
    if (record.is_discarded() || !record.is_object()) {
      output << line << "\n";
      continue;
    }
    auto refs = record.find("blob_refs");
    if (refs != record.end() && refs->is_array()) {
      for (auto &ref : *refs) {
        if (!ref.is_number_integer())
          continue;
        const auto value = ref.get<std::int64_t>();
        if (value <= 0)
          continue;
        const auto found = blob_remap.find(static_cast<std::uint64_t>(value));
        if (found == blob_remap.end())
          continue;
        ref = found->second;
        changed = true;
      }
    }
    output << record.dump() << "\n";
  }
  output.close();
  input.close();

  if (!changed) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return std::nullopt;
  }

  std::error_code rename_error;
  std::filesystem::rename(temp_path, path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return std::nullopt;
  }

  const auto digest = sha256_file(path);
  std::error_code size_error;
  const auto byte_size = std::filesystem::file_size(path, size_error);
  return std::make_pair(digest, size_error ? 0ull : static_cast<std::uint64_t>(byte_size));
}

bool should_write_asset_index(
    const std::filesystem::path &asset_index_path,
    const std::vector<AssetRecord> &assets,
    const std::vector<AssetRecord> &metal_assets)
{
  if (!assets.empty() || !metal_assets.empty()) {
    return true;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(asset_index_path, error) || error) {
    return true;
  }
  const auto existing_size = std::filesystem::file_size(asset_index_path, error);
  return error || existing_size <= 32;
}

std::filesystem::path analysis_path_for_stream(const BundleLayout &layout, std::string_view stream_name)
{
  std::filesystem::path name(stream_name);
  if (!name.has_extension()) {
    name += ".jsonl";
  }
  return layout.analysis_directory_path / name;
}

bool env_flag_enabled(const char *name)
{
  const char *value = std::getenv(name);
  return value && *value && std::strcmp(value, "0") != 0 &&
         std::strcmp(value, "false") != 0 && std::strcmp(value, "no") != 0;
}

bool env_flag_enabled_default(const char *name, bool default_value)
{
  const char *value = std::getenv(name);
  if (!value || !*value)
    return default_value;
  return std::strcmp(value, "0") != 0 &&
         std::strcmp(value, "false") != 0 && std::strcmp(value, "no") != 0;
}

std::size_t env_size_or_default(const char *name, std::size_t default_value)
{
  const char *value = std::getenv(name);
  if (!value || !*value)
    return default_value;

  char *end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  if (end == value)
    return default_value;
  return static_cast<std::size_t>(parsed);
}

struct SparseWriteResult {
  bool ok = true;
  std::uint64_t zero_run_count = 0;
  std::uint64_t zero_bytes_skipped = 0;
};

struct SparseIoStats {
  AtomicWriterPhaseStats *zero_scan = nullptr;
  AtomicWriterPhaseStats *hash_update = nullptr;
  AtomicWriterPhaseStats *stream_write = nullptr;
  AtomicWriterPhaseStats *seek = nullptr;
  AtomicWriterPhaseStats *final_touch = nullptr;
  AtomicWriterPhaseStats *open = nullptr;
  AtomicWriterPhaseStats *close = nullptr;
};

constexpr std::size_t kSparseZeroRunBytes = 64ull * 1024ull;

bool is_zero_range(const std::uint8_t *data, std::size_t size)
{
  static constexpr std::array<std::uint8_t, 256> kZeros{};
  while (size >= kZeros.size()) {
    if (std::memcmp(data, kZeros.data(), kZeros.size()) != 0)
      return false;
    data += kZeros.size();
    size -= kZeros.size();
  }
  return size == 0 || std::memcmp(data, kZeros.data(), size) == 0;
}

bool find_next_sparse_zero_run(
    const std::vector<std::uint8_t> &payload,
    std::size_t search_offset,
    std::size_t &run_start,
    std::size_t &run_end)
{
  if (payload.size() < kSparseZeroRunBytes || search_offset >= payload.size())
    return false;

  const auto *data = payload.data();
  std::size_t search = search_offset;
  while (search + kSparseZeroRunBytes <= payload.size()) {
    const auto *zero = static_cast<const std::uint8_t *>(
        std::memchr(data + search, 0, payload.size() - search));
    if (!zero)
      return false;

    const auto candidate = static_cast<std::size_t>(zero - data);
    if (candidate + kSparseZeroRunBytes > payload.size())
      return false;

    if (!is_zero_range(data + candidate, kSparseZeroRunBytes)) {
      search = candidate + 1;
      continue;
    }

    auto end = candidate + kSparseZeroRunBytes;
    while (end + kSparseZeroRunBytes <= payload.size() &&
           is_zero_range(data + end, kSparseZeroRunBytes)) {
      end += kSparseZeroRunBytes;
    }
    while (end < payload.size() && payload[end] == 0)
      ++end;

    run_start = candidate;
    run_end = end;
    return true;
  }

  return false;
}

SparseWriteResult write_payload_sparse(
    std::ofstream &output,
    const std::vector<std::uint8_t> &payload,
    const SparseIoStats *stats = nullptr)
{
  SparseWriteResult result;
  if (!output.is_open()) {
    return result;
  }

  auto write_range = [&output, &payload, stats](std::size_t offset, std::size_t size) {
    if (size == 0)
      return true;
    TimedAtomicWriterPhase phase(stats ? stats->stream_write : nullptr);
    output.write(
        reinterpret_cast<const char *>(payload.data() + offset),
        static_cast<std::streamsize>(size));
    return output.good();
  };

  std::size_t segment_start = 0;
  std::size_t zero_start = 0;
  std::size_t zero_end = 0;
  for (;;) {
    bool found_zero_run = false;
    {
      TimedAtomicWriterPhase phase(stats ? stats->zero_scan : nullptr);
      found_zero_run = find_next_sparse_zero_run(payload, segment_start, zero_start, zero_end);
    }
    if (!found_zero_run)
      break;
    if (!write_range(segment_start, zero_start - segment_start)) {
      result.ok = false;
      return result;
    }
    const auto run_size = zero_end - zero_start;
    {
      TimedAtomicWriterPhase phase(stats ? stats->seek : nullptr);
      output.seekp(static_cast<std::streamoff>(run_size), std::ios::cur);
    }
    if (!output.good()) {
      result.ok = false;
      return result;
    }
    ++result.zero_run_count;
    result.zero_bytes_skipped += run_size;
    segment_start = zero_end;
  }

  if (!write_range(segment_start, payload.size() - segment_start)) {
    result.ok = false;
    return result;
  }

  if (!payload.empty()) {
    TimedAtomicWriterPhase phase(stats ? stats->final_touch : nullptr);
    output.seekp(static_cast<std::streamoff>(payload.size() - 1), std::ios::beg);
    const char last_byte = static_cast<char>(payload.back());
    output.write(&last_byte, 1);
  }
  result.ok = output.good();
  return result;
}

struct HashedSparseWriteResult {
  std::string digest;
  SparseWriteResult sparse;
};

bool existing_file_matches_content_hash(
    const std::filesystem::path &absolute_path,
    std::string_view content_hash,
    std::uint64_t expected_byte_size,
    std::uint64_t &byte_size,
    bool *size_mismatch);

bool existing_file_matches_content_addressed_size(
    const std::filesystem::path &absolute_path,
    std::uint64_t expected_byte_size,
    std::uint64_t &byte_size,
    bool *size_mismatch);

HashedSparseWriteResult hash_and_write_payload_sparse(
    std::ofstream &output,
    const std::vector<std::uint8_t> &payload,
    const SparseIoStats *stats = nullptr)
{
  HashedSparseWriteResult result;
  Sha256 sha256;
  if (!output.is_open()) {
    TimedAtomicWriterPhase phase(stats ? stats->hash_update : nullptr);
    sha256.update(payload.data(), payload.size());
    result.digest = sha256.final_hex();
    return result;
  }

  auto write_range = [&output, &payload, stats](std::size_t offset, std::size_t size) {
    if (size == 0)
      return true;
    TimedAtomicWriterPhase phase(stats ? stats->stream_write : nullptr);
    output.write(
        reinterpret_cast<const char *>(payload.data() + offset),
        static_cast<std::streamsize>(size));
    return output.good();
  };

  std::size_t hash_segment_start = 0;
  std::size_t segment_start = 0;
  static constexpr std::array<std::uint8_t, 1024 * 1024> kHashZeros{};
  auto hash_zero_range = [&sha256, stats](std::size_t size) {
    TimedAtomicWriterPhase phase(stats ? stats->hash_update : nullptr);
    while (size >= kHashZeros.size()) {
      sha256.update(kHashZeros.data(), kHashZeros.size());
      size -= kHashZeros.size();
    }
    if (size != 0)
      sha256.update(kHashZeros.data(), size);
  };
  auto finish_digest = [&]() {
    if (hash_segment_start < payload.size()) {
      TimedAtomicWriterPhase phase(stats ? stats->hash_update : nullptr);
      sha256.update(payload.data() + hash_segment_start, payload.size() - hash_segment_start);
      hash_segment_start = payload.size();
    }
    return sha256.final_hex();
  };
  std::size_t zero_start = 0;
  std::size_t zero_end = 0;
  for (;;) {
    bool found_zero_run = false;
    {
      TimedAtomicWriterPhase phase(stats ? stats->zero_scan : nullptr);
      found_zero_run = find_next_sparse_zero_run(payload, segment_start, zero_start, zero_end);
    }
    if (!found_zero_run)
      break;
    if (hash_segment_start < zero_start) {
      TimedAtomicWriterPhase phase(stats ? stats->hash_update : nullptr);
      sha256.update(payload.data() + hash_segment_start, zero_start - hash_segment_start);
    }
    const auto run_size = zero_end - zero_start;
    hash_zero_range(run_size);
    hash_segment_start = zero_end;
    if (!write_range(segment_start, zero_start - segment_start)) {
      result.sparse.ok = false;
      result.digest = finish_digest();
      return result;
    }
    {
      TimedAtomicWriterPhase phase(stats ? stats->seek : nullptr);
      output.seekp(static_cast<std::streamoff>(run_size), std::ios::cur);
    }
    if (!output.good()) {
      result.sparse.ok = false;
      result.digest = finish_digest();
      return result;
    }
    ++result.sparse.zero_run_count;
    result.sparse.zero_bytes_skipped += run_size;
    segment_start = zero_end;
  }

  const auto digest = finish_digest();

  if (!write_range(segment_start, payload.size() - segment_start)) {
    result.sparse.ok = false;
    result.digest = digest;
    return result;
  }

  if (!payload.empty()) {
    TimedAtomicWriterPhase phase(stats ? stats->final_touch : nullptr);
    output.seekp(static_cast<std::streamoff>(payload.size() - 1), std::ios::beg);
    const char last_byte = static_cast<char>(payload.back());
    output.write(&last_byte, 1);
  }
  result.sparse.ok = output.good();
  result.digest = digest;
  return result;
}

class AsyncLineWriter {
public:
  struct CheckpointSnapshot {
    std::string digest;
    std::uint64_t byte_size = 0;
    bool valid = false;
  };

  struct PendingItem {
    std::string line;
    std::optional<EventRecord> event;
    std::size_t pending_bytes = 0;
  };

  AsyncLineWriter() = default;
  AsyncLineWriter(const AsyncLineWriter &) = delete;
  AsyncLineWriter &operator=(const AsyncLineWriter &) = delete;

  ~AsyncLineWriter()
  {
    close();
  }

  void set_max_pending_bytes(std::size_t max_pending_bytes)
  {
    max_pending_bytes_ = max_pending_bytes == 0 ? kDefaultMaxPendingBytes : max_pending_bytes;
  }

  void set_lock_stats(WriterLockStats *stats)
  {
    lock_stats_ = stats;
  }

  bool open(const std::filesystem::path &path, std::ios::openmode mode)
  {
    close();
    stream_.open(path, std::ios::binary | mode);
    if (!stream_.is_open())
      return false;
    stop_ = false;
    digest_ = Sha256{};
    digest_valid_ = (mode & std::ios::app) == 0;
    final_digest_.clear();
    worker_ = std::thread([this]() { run(); });
    return true;
  }

  bool is_open() const
  {
    return stream_.is_open();
  }

  void write_line(std::string line)
  {
    if (!is_open())
      return;
    line.push_back('\n');
    PendingItem item;
    item.pending_bytes = line.size();
    item.line = std::move(line);
    enqueue(std::move(item));
  }

  void write_event(EventRecord event)
  {
    if (!is_open())
      return;
    PendingItem item;
    item.pending_bytes = pending_size(event);
    item.event = std::move(event);
    enqueue(std::move(item));
  }

  void enqueue(PendingItem item)
  {
    const auto pending_size = item.pending_bytes;
    const auto wait_start_ns = monotonic_nanoseconds();
    bool waited = false;
    {
      auto lock = timed_unique_lock(mutex_, lock_stats_);
      cv_.wait(lock, [this, pending_size, &waited]() {
        if (!(stop_ ||
              pending_bytes_ == 0 ||
              pending_bytes_ + pending_size <= max_pending_bytes_)) {
          waited = true;
        }
        return stop_ ||
               pending_bytes_ == 0 ||
               pending_bytes_ + pending_size <= max_pending_bytes_;
      });
      if (stop_)
        return;
      const auto hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;
      if (waited) {
        const auto wait_ns = monotonic_nanoseconds() - wait_start_ns;
        wait_count_.fetch_add(1, std::memory_order_relaxed);
        wait_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
        update_atomic_max(max_wait_ns_, wait_ns);
      }
      enqueue_count_.fetch_add(1, std::memory_order_relaxed);
      enqueue_bytes_.fetch_add(pending_size, std::memory_order_relaxed);
      pending_bytes_ += pending_size;
      peak_pending_bytes_ = std::max(peak_pending_bytes_, pending_bytes_);
      queue_.push_back(std::move(item));
      if (lock_stats_)
        record_writer_lock_hold(lock_stats_, monotonic_nanoseconds() - hold_start_ns);
    }
    cv_.notify_one();
  }

  void flush()
  {
    auto lock = timed_unique_lock(mutex_, lock_stats_);
    cv_.wait(lock, [this]() { return queue_.empty() && !writing_; });
    lock.unlock();
    if (stream_.is_open())
      stream_.flush();
  }

  void close()
  {
    {
      TimedWriterLock lock(mutex_, lock_stats_);
      if (!stream_.is_open() && !worker_.joinable())
        return;
      stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable())
      worker_.join();
    if (stream_.is_open()) {
      stream_.flush();
      stream_.close();
    }
  }

  [[nodiscard]] std::string digest() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return final_digest_;
  }

  [[nodiscard]] std::string current_digest() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    if (!digest_valid_) {
      return {};
    }
    auto digest = digest_;
    return digest.final_hex();
  }

  [[nodiscard]] CheckpointSnapshot checkpoint_snapshot()
  {
    auto lock = timed_unique_lock(mutex_, lock_stats_);
    cv_.wait(lock, [this]() { return queue_.empty() && !writing_; });
    CheckpointSnapshot snapshot;
    if (!stream_.is_open() || !digest_valid_) {
      return snapshot;
    }
    stream_.flush();
    const auto position = stream_.tellp();
    if (position < 0) {
      return snapshot;
    }
    auto digest = digest_;
    snapshot.digest = digest.final_hex();
    snapshot.byte_size = static_cast<std::uint64_t>(position);
    snapshot.valid = true;
    return snapshot;
  }

  [[nodiscard]] std::size_t peak_pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return peak_pending_bytes_;
  }

  [[nodiscard]] std::size_t pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return pending_bytes_;
  }

  [[nodiscard]] std::size_t queued_lines() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return queue_.size();
  }

  [[nodiscard]] AsyncStreamStats stats() const
  {
    AsyncStreamStats value;
    if (lock_stats_) {
      const auto snapshot = snapshot_writer_lock_stats(*lock_stats_);
      value.mutex_lock_count = snapshot.lock_count;
      value.mutex_wait_ns = snapshot.wait_ns;
      value.mutex_max_wait_ns = snapshot.max_wait_ns;
      value.mutex_hold_ns = snapshot.hold_ns;
      value.mutex_max_hold_ns = snapshot.max_hold_ns;
    }
    value.enqueue_count = enqueue_count_.load(std::memory_order_relaxed);
    value.enqueue_bytes = enqueue_bytes_.load(std::memory_order_relaxed);
    value.wait_count = wait_count_.load(std::memory_order_relaxed);
    value.wait_ns = wait_ns_.load(std::memory_order_relaxed);
    value.max_wait_ns = max_wait_ns_.load(std::memory_order_relaxed);
    value.serialize_count = serialize_count_.load(std::memory_order_relaxed);
    value.serialize_ns = serialize_ns_.load(std::memory_order_relaxed);
    value.max_serialize_ns = max_serialize_ns_.load(std::memory_order_relaxed);
    value.write_count = write_count_.load(std::memory_order_relaxed);
    value.write_bytes = write_bytes_.load(std::memory_order_relaxed);
    value.write_ns = write_ns_.load(std::memory_order_relaxed);
    value.max_write_ns = max_write_ns_.load(std::memory_order_relaxed);
    return value;
  }

private:
  static constexpr std::size_t kDefaultMaxPendingBytes = 64ull * 1024ull * 1024ull;

  static std::size_t pending_size(const EventRecord &event)
  {
    return sizeof(EventRecord) +
           event.callsite.function_name.size() +
           event.object_refs.size() * sizeof(ObjectId) +
           event.blob_refs.size() * sizeof(BlobId) +
           event.object_debug_name.size() +
           event.payload.size() +
           128;
  }

  void run()
  {
    std::vector<PendingItem> batch;
    batch.reserve(4096);
    std::size_t batch_bytes = 0;
    for (;;) {
      {
        auto lock = timed_unique_lock(mutex_, lock_stats_);
        cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        if (queue_.empty() && stop_)
          break;
        const auto hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;
        writing_ = true;
        while (!queue_.empty() && batch.size() < 4096) {
          batch_bytes += queue_.front().pending_bytes;
          batch.push_back(std::move(queue_.front()));
          queue_.pop_front();
        }
        if (lock_stats_)
          record_writer_lock_hold(lock_stats_, monotonic_nanoseconds() - hold_start_ns);
      }

      for (auto &item : batch) {
        std::string serialized_line;
        std::string *line = &item.line;
        if (item.event) {
          const auto serialize_start_ns = monotonic_nanoseconds();
          serialized_line = event_record_json(*item.event);
          serialized_line.push_back('\n');
          const auto serialize_ns = monotonic_nanoseconds() - serialize_start_ns;
          serialize_count_.fetch_add(1, std::memory_order_relaxed);
          serialize_ns_.fetch_add(serialize_ns, std::memory_order_relaxed);
          update_atomic_max(max_serialize_ns_, serialize_ns);
          line = &serialized_line;
        }
        const auto write_start_ns = monotonic_nanoseconds();
        stream_.write(line->data(), static_cast<std::streamsize>(line->size()));
        if (digest_valid_)
          digest_.update(reinterpret_cast<const std::uint8_t *>(line->data()), line->size());
        const auto write_ns = monotonic_nanoseconds() - write_start_ns;
        write_count_.fetch_add(1, std::memory_order_relaxed);
        write_bytes_.fetch_add(line->size(), std::memory_order_relaxed);
        write_ns_.fetch_add(write_ns, std::memory_order_relaxed);
        update_atomic_max(max_write_ns_, write_ns);
      }
      batch.clear();

      {
        TimedWriterLock lock(mutex_, lock_stats_);
        pending_bytes_ = batch_bytes > pending_bytes_ ? 0 : pending_bytes_ - batch_bytes;
        batch_bytes = 0;
        writing_ = false;
      }
      cv_.notify_all();
    }
    {
      TimedWriterLock lock(mutex_, lock_stats_);
      if (digest_valid_)
        final_digest_ = digest_.final_hex();
    }
  }

  std::ofstream stream_;
  Sha256 digest_;
  std::string final_digest_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<PendingItem> queue_;
  std::size_t max_pending_bytes_ = kDefaultMaxPendingBytes;
  std::size_t pending_bytes_ = 0;
  std::size_t peak_pending_bytes_ = 0;
  std::atomic_uint64_t enqueue_count_{0};
  std::atomic_uint64_t enqueue_bytes_{0};
  std::atomic_uint64_t wait_count_{0};
  std::atomic_uint64_t wait_ns_{0};
  std::atomic_uint64_t max_wait_ns_{0};
  std::atomic_uint64_t serialize_count_{0};
  std::atomic_uint64_t serialize_ns_{0};
  std::atomic_uint64_t max_serialize_ns_{0};
  std::atomic_uint64_t write_count_{0};
  std::atomic_uint64_t write_bytes_{0};
  std::atomic_uint64_t write_ns_{0};
  std::atomic_uint64_t max_write_ns_{0};
  WriterLockStats *lock_stats_ = nullptr;
  bool stop_ = false;
  bool writing_ = false;
  bool digest_valid_ = false;
};

class AsyncMetalEventWriter {
public:
  AsyncMetalEventWriter() = default;
  AsyncMetalEventWriter(const AsyncMetalEventWriter &) = delete;
  AsyncMetalEventWriter &operator=(const AsyncMetalEventWriter &) = delete;

  ~AsyncMetalEventWriter()
  {
    close();
  }

  void set_max_pending_bytes(std::size_t max_pending_bytes)
  {
    max_pending_bytes_ = max_pending_bytes == 0 ? kDefaultMaxPendingBytes : max_pending_bytes;
  }

  void set_lock_stats(WriterLockStats *stats)
  {
    lock_stats_ = stats;
  }

  bool open(const std::filesystem::path &path, std::ios::openmode mode)
  {
    close();
    stream_.open(path, std::ios::binary | mode);
    if (!stream_.is_open())
      return false;
    stop_ = false;
    digest_ = Sha256{};
    digest_valid_ = (mode & std::ios::app) == 0;
    final_digest_.clear();
    worker_ = std::thread([this]() { run(); });
    return true;
  }

  bool is_open() const
  {
    return stream_.is_open();
  }

  void write_event(MetalEventRecord event)
  {
    if (!is_open())
      return;
    const auto event_size = pending_size(event);
    const auto wait_start_ns = monotonic_nanoseconds();
    bool waited = false;
    {
      auto lock = timed_unique_lock(mutex_, lock_stats_);
      cv_.wait(lock, [this, event_size, &waited]() {
        if (!(stop_ ||
              pending_bytes_ == 0 ||
              pending_bytes_ + event_size <= max_pending_bytes_)) {
          waited = true;
        }
        return stop_ ||
               pending_bytes_ == 0 ||
               pending_bytes_ + event_size <= max_pending_bytes_;
      });
      if (stop_)
        return;
      const auto hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;
      if (waited) {
        const auto wait_ns = monotonic_nanoseconds() - wait_start_ns;
        wait_count_.fetch_add(1, std::memory_order_relaxed);
        wait_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
        update_atomic_max(max_wait_ns_, wait_ns);
      }
      enqueue_count_.fetch_add(1, std::memory_order_relaxed);
      enqueue_bytes_.fetch_add(event_size, std::memory_order_relaxed);
      pending_bytes_ += event_size;
      peak_pending_bytes_ = std::max(peak_pending_bytes_, pending_bytes_);
      queue_.push_back(QueuedEvent{std::move(event), event_size});
      if (lock_stats_)
        record_writer_lock_hold(lock_stats_, monotonic_nanoseconds() - hold_start_ns);
    }
    cv_.notify_one();
  }

  void flush()
  {
    auto lock = timed_unique_lock(mutex_, lock_stats_);
    cv_.wait(lock, [this]() { return queue_.empty() && !writing_; });
    lock.unlock();
    if (stream_.is_open())
      stream_.flush();
  }

  void close()
  {
    {
      TimedWriterLock lock(mutex_, lock_stats_);
      if (!stream_.is_open() && !worker_.joinable())
        return;
      stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable())
      worker_.join();
    if (stream_.is_open()) {
      stream_.flush();
      stream_.close();
    }
  }

  [[nodiscard]] std::string digest() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return final_digest_;
  }

  [[nodiscard]] AsyncLineWriter::CheckpointSnapshot checkpoint_snapshot()
  {
    auto lock = timed_unique_lock(mutex_, lock_stats_);
    cv_.wait(lock, [this]() { return queue_.empty() && !writing_; });
    AsyncLineWriter::CheckpointSnapshot snapshot;
    if (!stream_.is_open() || !digest_valid_) {
      return snapshot;
    }
    stream_.flush();
    const auto position = stream_.tellp();
    if (position < 0) {
      return snapshot;
    }
    auto digest = digest_;
    snapshot.digest = digest.final_hex();
    snapshot.byte_size = static_cast<std::uint64_t>(position);
    snapshot.valid = true;
    return snapshot;
  }

  [[nodiscard]] std::size_t peak_pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return peak_pending_bytes_;
  }

  [[nodiscard]] std::size_t pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return pending_bytes_;
  }

  [[nodiscard]] std::size_t queued_events() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return queue_.size();
  }

  [[nodiscard]] AsyncStreamStats stats() const
  {
    AsyncStreamStats value;
    if (lock_stats_) {
      const auto snapshot = snapshot_writer_lock_stats(*lock_stats_);
      value.mutex_lock_count = snapshot.lock_count;
      value.mutex_wait_ns = snapshot.wait_ns;
      value.mutex_max_wait_ns = snapshot.max_wait_ns;
      value.mutex_hold_ns = snapshot.hold_ns;
      value.mutex_max_hold_ns = snapshot.max_hold_ns;
    }
    value.enqueue_count = enqueue_count_.load(std::memory_order_relaxed);
    value.enqueue_bytes = enqueue_bytes_.load(std::memory_order_relaxed);
    value.wait_count = wait_count_.load(std::memory_order_relaxed);
    value.wait_ns = wait_ns_.load(std::memory_order_relaxed);
    value.max_wait_ns = max_wait_ns_.load(std::memory_order_relaxed);
    value.serialize_count = serialize_count_.load(std::memory_order_relaxed);
    value.serialize_ns = serialize_ns_.load(std::memory_order_relaxed);
    value.max_serialize_ns = max_serialize_ns_.load(std::memory_order_relaxed);
    value.write_count = write_count_.load(std::memory_order_relaxed);
    value.write_bytes = write_bytes_.load(std::memory_order_relaxed);
    value.write_ns = write_ns_.load(std::memory_order_relaxed);
    value.max_write_ns = max_write_ns_.load(std::memory_order_relaxed);
    return value;
  }

private:
  struct QueuedEvent {
    MetalEventRecord event;
    std::size_t pending_bytes = 0;
  };

  static constexpr std::size_t kDefaultMaxPendingBytes = 64ull * 1024ull * 1024ull;

  static std::size_t pending_size(const MetalEventRecord &event)
  {
    return sizeof(MetalEventRecord) +
           event.function_name.size() +
           event.payload.size() +
           event.object_refs.size() * sizeof(std::uint64_t) +
           event.blob_refs.size() * sizeof(BlobId) +
           128;
  }

  void run()
  {
    std::vector<QueuedEvent> batch;
    batch.reserve(4096);
    std::size_t batch_bytes = 0;
    for (;;) {
      {
        auto lock = timed_unique_lock(mutex_, lock_stats_);
        cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        if (queue_.empty() && stop_)
          break;
        const auto hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;
        writing_ = true;
        while (!queue_.empty() && batch.size() < 4096) {
          batch_bytes += queue_.front().pending_bytes;
          batch.push_back(std::move(queue_.front()));
          queue_.pop_front();
        }
        if (lock_stats_)
          record_writer_lock_hold(lock_stats_, monotonic_nanoseconds() - hold_start_ns);
      }

      for (const auto &queued : batch) {
        const auto serialize_start_ns = monotonic_nanoseconds();
        auto line = detail::metal_event_record_json(queued.event);
        const auto serialize_ns = monotonic_nanoseconds() - serialize_start_ns;
        serialize_count_.fetch_add(1, std::memory_order_relaxed);
        serialize_ns_.fetch_add(serialize_ns, std::memory_order_relaxed);
        update_atomic_max(max_serialize_ns_, serialize_ns);
        line.push_back('\n');
        const auto write_start_ns = monotonic_nanoseconds();
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (digest_valid_)
          digest_.update(reinterpret_cast<const std::uint8_t *>(line.data()), line.size());
        const auto write_ns = monotonic_nanoseconds() - write_start_ns;
        write_count_.fetch_add(1, std::memory_order_relaxed);
        write_bytes_.fetch_add(line.size(), std::memory_order_relaxed);
        write_ns_.fetch_add(write_ns, std::memory_order_relaxed);
        update_atomic_max(max_write_ns_, write_ns);
      }
      batch.clear();

      {
        TimedWriterLock lock(mutex_, lock_stats_);
        pending_bytes_ = batch_bytes > pending_bytes_ ? 0 : pending_bytes_ - batch_bytes;
        batch_bytes = 0;
        writing_ = false;
      }
      cv_.notify_all();
    }
    {
      TimedWriterLock lock(mutex_, lock_stats_);
      if (digest_valid_)
        final_digest_ = digest_.final_hex();
    }
  }

  std::ofstream stream_;
  Sha256 digest_;
  std::string final_digest_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedEvent> queue_;
  std::size_t max_pending_bytes_ = kDefaultMaxPendingBytes;
  std::size_t pending_bytes_ = 0;
  std::size_t peak_pending_bytes_ = 0;
  std::atomic_uint64_t enqueue_count_{0};
  std::atomic_uint64_t enqueue_bytes_{0};
  std::atomic_uint64_t wait_count_{0};
  std::atomic_uint64_t wait_ns_{0};
  std::atomic_uint64_t max_wait_ns_{0};
  std::atomic_uint64_t serialize_count_{0};
  std::atomic_uint64_t serialize_ns_{0};
  std::atomic_uint64_t max_serialize_ns_{0};
  std::atomic_uint64_t write_count_{0};
  std::atomic_uint64_t write_bytes_{0};
  std::atomic_uint64_t write_ns_{0};
  std::atomic_uint64_t max_write_ns_{0};
  WriterLockStats *lock_stats_ = nullptr;
  bool stop_ = false;
  bool writing_ = false;
  bool digest_valid_ = false;
};

class AsyncAssetWriter {
public:
  AsyncAssetWriter() = default;
  AsyncAssetWriter(const AsyncAssetWriter &) = delete;
  AsyncAssetWriter &operator=(const AsyncAssetWriter &) = delete;

  ~AsyncAssetWriter()
  {
    close();
  }

  void set_max_pending_bytes(std::size_t max_pending_bytes)
  {
    max_pending_bytes_ = max_pending_bytes == 0 ? kDefaultMaxPendingBytes : max_pending_bytes;
    hard_max_pending_bytes_ = std::max(hard_max_pending_bytes_, max_pending_bytes_);
  }

  void set_hard_max_pending_bytes(std::size_t max_pending_bytes)
  {
    hard_max_pending_bytes_ = max_pending_bytes == 0 ? default_hard_max_pending_bytes() : max_pending_bytes;
    hard_max_pending_bytes_ = std::max(hard_max_pending_bytes_, max_pending_bytes_);
  }

	  void set_bulk_threshold(std::size_t threshold)
	  {
	    bulk_threshold_ = threshold == 0 ? kDefaultBulkThreshold : threshold;
	  }

	  void set_sparse_write_enabled(bool enabled)
	  {
	    sparse_write_enabled_ = enabled;
	  }

  void set_worker_count(std::size_t worker_count)
  {
    worker_count_ = std::max<std::size_t>(1, worker_count);
  }

  void set_lock_stats(WriterLockStats *stats)
  {
    lock_stats_ = stats;
  }

	  struct Stats {
	    WriterLockStatsSnapshot mutex;
	    WriterPhaseStats hash_payload_only;
	    WriterPhaseStats existing_file_check;
	    WriterPhaseStats hash_and_write;
	    WriterPhaseStats write_without_hash;
	    WriterPhaseStats sparse_zero_scan;
	    WriterPhaseStats sparse_hash_update;
	    WriterPhaseStats sparse_stream_write;
    WriterPhaseStats sparse_seek;
    WriterPhaseStats sparse_final_touch;
    WriterPhaseStats file_open;
    WriterPhaseStats file_close;
    WriterPhaseStats final_file_check;
    WriterPhaseStats late_write;
    WriterPhaseStats link_or_copy;
    WriterPhaseStats completion_lock;
    WriterPhaseStats completion_callback;
    std::uint64_t enqueue_capacity_wait_count = 0;
    std::uint64_t enqueue_capacity_wait_ns = 0;
    std::uint64_t enqueue_capacity_max_wait_ns = 0;
    std::uint64_t enqueue_soft_overflow_count = 0;
    std::uint64_t enqueue_soft_overflow_bytes = 0;
    std::uint64_t enqueue_regular_count = 0;
    std::uint64_t enqueue_regular_bytes = 0;
    std::uint64_t enqueue_bulk_count = 0;
    std::uint64_t enqueue_bulk_bytes = 0;
    std::uint64_t fingerprint_wait_count = 0;
    std::uint64_t fingerprint_wait_ns = 0;
    std::uint64_t fingerprint_max_wait_ns = 0;
  };

  struct Completion {
    AssetRecord asset;
    std::filesystem::path relative_path;
    std::filesystem::path final_relative_path;
    std::string digest;
    std::string content_key;
    std::string fingerprint_key;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    std::uint64_t sparse_zero_run_count = 0;
    std::uint64_t sparse_zero_bytes_skipped = 0;
    MetalAssetKind metal_kind = MetalAssetKind::Buffer;
    bool metal = false;
	    bool hash_only_candidate = false;
	    bool hash_only_avoided_write = false;
	    std::uint64_t hash_only_write_bytes_avoided = 0;
	    bool existing_hash_size_mismatch = false;
	    bool deferred_content_hash = false;
	  };

  void set_completion_callback(std::function<void(const Completion &)> callback)
  {
    completion_callback_ = std::move(callback);
  }

  [[nodiscard]] std::size_t pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return pending_bytes_;
  }

  [[nodiscard]] std::size_t queued_jobs() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t regular_pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return regular_pending_bytes_;
  }

  [[nodiscard]] std::size_t bulk_pending_bytes() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return bulk_pending_bytes_;
  }

  [[nodiscard]] std::size_t soft_max_pending_bytes() const
  {
    return max_pending_bytes_;
  }

  [[nodiscard]] std::size_t hard_max_pending_bytes() const
  {
    return hard_max_pending_bytes_;
  }

  bool enqueue(
      std::filesystem::path target_path,
      std::filesystem::path relative_path,
      std::filesystem::path hashed_directory_path,
      std::filesystem::path hashed_directory_relative_path,
      std::string hashed_extension,
      std::shared_ptr<const std::vector<std::uint8_t>> payload,
      std::string content_key,
      std::string fingerprint_key,
	      AssetRecord asset,
	      MetalAssetKind metal_kind,
	      bool metal,
	      bool hash_only_if_final_exists = false,
	      bool defer_content_hash = false,
	      std::function<void()> after_queue = {})
  {
    if (!payload || payload->empty())
      return false;

    const auto payload_size = payload->size();
    const bool bulk_layer = payload_size >= bulk_threshold_;
    {
      auto lock = timed_unique_lock(mutex_, lock_stats_);
      auto hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;
      if (workers_.empty()) {
        stop_ = false;
        workers_.reserve(worker_count_);
        for (std::size_t index = 0; index < worker_count_; ++index)
          workers_.emplace_back([this]() { run(); });
      }

      if (stop_)
        return false;
      bool waited_for_capacity = false;
      const auto wait_start_ns = monotonic_nanoseconds();
      cv_work_.wait(lock, [this, payload_size, &waited_for_capacity]() {
        if (!(stop_ ||
              pending_bytes_ == 0 ||
              pending_bytes_ + payload_size <= hard_max_pending_bytes_)) {
          waited_for_capacity = true;
        }
        return stop_ ||
               pending_bytes_ == 0 ||
               pending_bytes_ + payload_size <= hard_max_pending_bytes_;
      });
      if (stop_)
        return false;
      if (waited_for_capacity) {
        const auto wait_ns = monotonic_nanoseconds() - wait_start_ns;
        enqueue_capacity_wait_count_.fetch_add(1, std::memory_order_relaxed);
        enqueue_capacity_wait_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
        update_atomic_max(enqueue_capacity_max_wait_ns_, wait_ns);
      }
      hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;

      const bool order_fingerprint = !bulk_layer;
      std::uint64_t fingerprint_order = 0;
      if (order_fingerprint && !fingerprint_key.empty() && content_key.empty()) {
        fingerprint_order = ++fingerprint_enqueued_counts_[fingerprint_key];
      }
      if (pending_bytes_ + payload_size > max_pending_bytes_) {
        enqueue_soft_overflow_count_.fetch_add(1, std::memory_order_relaxed);
        enqueue_soft_overflow_bytes_.fetch_add(payload_size, std::memory_order_relaxed);
      }
      pending_bytes_ += payload_size;
      if (bulk_layer) {
        bulk_pending_bytes_ += payload_size;
        enqueue_bulk_count_.fetch_add(1, std::memory_order_relaxed);
        enqueue_bulk_bytes_.fetch_add(payload_size, std::memory_order_relaxed);
      } else {
        regular_pending_bytes_ += payload_size;
        enqueue_regular_count_.fetch_add(1, std::memory_order_relaxed);
        enqueue_regular_bytes_.fetch_add(payload_size, std::memory_order_relaxed);
      }
      queue_.push_back(Job{
          std::move(target_path),
          std::move(relative_path),
          std::move(hashed_directory_path),
          std::move(hashed_directory_relative_path),
          std::move(hashed_extension),
          std::move(payload),
          std::move(content_key),
          std::move(fingerprint_key),
          std::move(asset),
	          metal_kind,
	          metal,
	          hash_only_if_final_exists,
	          defer_content_hash,
	          fingerprint_order,
	          bulk_layer,
	      });
      if (after_queue)
        after_queue();
      if (lock_stats_)
        record_writer_lock_hold(lock_stats_, monotonic_nanoseconds() - hold_start_ns);
    }
    cv_work_.notify_one();
    return true;
  }

  void close()
  {
    {
      TimedWriterLock lock(mutex_, lock_stats_);
      if (workers_.empty())
        return;
      stop_ = true;
    }
    cv_work_.notify_all();
    cv_fingerprint_.notify_all();
    for (auto &worker : workers_) {
      if (worker.joinable())
        worker.join();
    }
    workers_.clear();
  }

  [[nodiscard]] std::unordered_map<std::string, std::string> completed_path_aliases() const
  {
    TimedWriterLock lock(mutex_, lock_stats_);
    return completed_path_aliases_;
  }

  [[nodiscard]] Stats stats() const
  {
    Stats stats;
    if (lock_stats_)
      stats.mutex = snapshot_writer_lock_stats(*lock_stats_);
	    stats.hash_payload_only = snapshot_writer_phase_stats(hash_payload_only_stats_);
	    stats.existing_file_check = snapshot_writer_phase_stats(existing_file_check_stats_);
	    stats.hash_and_write = snapshot_writer_phase_stats(hash_and_write_stats_);
	    stats.write_without_hash = snapshot_writer_phase_stats(write_without_hash_stats_);
	    stats.sparse_zero_scan = snapshot_writer_phase_stats(sparse_zero_scan_stats_);
    stats.sparse_hash_update = snapshot_writer_phase_stats(sparse_hash_update_stats_);
    stats.sparse_stream_write = snapshot_writer_phase_stats(sparse_stream_write_stats_);
    stats.sparse_seek = snapshot_writer_phase_stats(sparse_seek_stats_);
    stats.sparse_final_touch = snapshot_writer_phase_stats(sparse_final_touch_stats_);
    stats.file_open = snapshot_writer_phase_stats(file_open_stats_);
    stats.file_close = snapshot_writer_phase_stats(file_close_stats_);
    stats.final_file_check = snapshot_writer_phase_stats(final_file_check_stats_);
    stats.late_write = snapshot_writer_phase_stats(late_write_stats_);
    stats.link_or_copy = snapshot_writer_phase_stats(link_or_copy_stats_);
    stats.completion_lock = snapshot_writer_phase_stats(completion_lock_stats_);
    stats.completion_callback = snapshot_writer_phase_stats(completion_callback_stats_);
    stats.enqueue_capacity_wait_count = enqueue_capacity_wait_count_.load(std::memory_order_relaxed);
    stats.enqueue_capacity_wait_ns = enqueue_capacity_wait_ns_.load(std::memory_order_relaxed);
    stats.enqueue_capacity_max_wait_ns = enqueue_capacity_max_wait_ns_.load(std::memory_order_relaxed);
    stats.enqueue_soft_overflow_count = enqueue_soft_overflow_count_.load(std::memory_order_relaxed);
    stats.enqueue_soft_overflow_bytes = enqueue_soft_overflow_bytes_.load(std::memory_order_relaxed);
    stats.enqueue_regular_count = enqueue_regular_count_.load(std::memory_order_relaxed);
    stats.enqueue_regular_bytes = enqueue_regular_bytes_.load(std::memory_order_relaxed);
    stats.enqueue_bulk_count = enqueue_bulk_count_.load(std::memory_order_relaxed);
    stats.enqueue_bulk_bytes = enqueue_bulk_bytes_.load(std::memory_order_relaxed);
    stats.fingerprint_wait_count = fingerprint_wait_count_.load(std::memory_order_relaxed);
    stats.fingerprint_wait_ns = fingerprint_wait_ns_.load(std::memory_order_relaxed);
    stats.fingerprint_max_wait_ns = fingerprint_max_wait_ns_.load(std::memory_order_relaxed);
    return stats;
  }

private:
  static constexpr std::size_t kDefaultMaxPendingBytes = 1024ull * 1024ull * 1024ull;
  static constexpr std::size_t kDefaultBulkThreshold = 1024ull * 1024ull;

  static constexpr std::size_t default_hard_max_pending_bytes()
  {
    return kDefaultMaxPendingBytes * 4;
  }

  struct Job {
    std::filesystem::path target_path;
    std::filesystem::path relative_path;
    std::filesystem::path hashed_directory_path;
    std::filesystem::path hashed_directory_relative_path;
    std::string hashed_extension;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    std::string content_key;
    std::string fingerprint_key;
    AssetRecord asset;
	    MetalAssetKind metal_kind = MetalAssetKind::Buffer;
	    bool metal = false;
	    bool hash_only_if_final_exists = false;
	    bool defer_content_hash = false;
	    std::uint64_t fingerprint_order = 0;
	    bool bulk_layer = false;
	  };

  void run()
  {
    for (;;) {
      Job job;
      {
        auto lock = timed_unique_lock(mutex_, lock_stats_);
        cv_work_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        if (queue_.empty() && stop_)
          break;
        const auto hold_start_ns = lock_stats_ ? monotonic_nanoseconds() : 0;
        job = std::move(queue_.front());
        queue_.pop_front();
        writing_ = true;
        if (lock_stats_)
          record_writer_lock_hold(lock_stats_, monotonic_nanoseconds() - hold_start_ns);
      }

      auto wait_for_fingerprint_turn = [&]() {
        if (job.fingerprint_key.empty() || job.fingerprint_order == 0)
          return;
        auto lock = timed_unique_lock(mutex_, lock_stats_);
        bool waited_for_turn = false;
        const auto wait_start_ns = monotonic_nanoseconds();
        cv_fingerprint_.wait(lock, [this, &job, &waited_for_turn]() {
          const auto completed = fingerprint_completed_counts_.find(job.fingerprint_key);
          const auto completed_count = completed == fingerprint_completed_counts_.end() ? 0 : completed->second;
          const bool ready = completed_count + 1 == job.fingerprint_order;
          waited_for_turn = waited_for_turn || !ready;
          return ready;
        });
        if (waited_for_turn) {
          const auto wait_ns = monotonic_nanoseconds() - wait_start_ns;
          fingerprint_wait_count_.fetch_add(1, std::memory_order_relaxed);
          fingerprint_wait_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
          update_atomic_max(fingerprint_max_wait_ns_, wait_ns);
        }
      };

      if (!job.hash_only_if_final_exists)
        wait_for_fingerprint_turn();

      std::filesystem::create_directories(job.target_path.parent_path());

      const bool can_hash_address =
          !job.hashed_directory_relative_path.empty() && !job.hashed_extension.empty();
      auto hash_payload_only = [&]() {
        Sha256 sha256;
        sha256.update(*job.payload);
        return sha256.final_hex();
      };

      std::string digest;
      bool hash_only_avoided_write = false;
      bool existing_hash_size_mismatch = false;
	      if (!job.defer_content_hash && job.hash_only_if_final_exists && can_hash_address) {
	        TimedAtomicWriterPhase phase(lock_stats_ ? &hash_payload_only_stats_ : nullptr);
	        digest = hash_payload_only();
	      }

      if (job.hash_only_if_final_exists)
        wait_for_fingerprint_turn();

      std::ofstream output;
      if (digest.empty() && !can_hash_address && !job.asset.content_hash.empty()) {
        std::uint64_t existing_byte_size = 0;
        bool size_mismatch = false;
        bool existing_matches = false;
        {
          TimedAtomicWriterPhase phase(lock_stats_ ? &existing_file_check_stats_ : nullptr);
          existing_matches = existing_file_matches_content_hash(
              job.target_path,
              job.asset.content_hash,
              job.asset.byte_size,
              existing_byte_size,
              &size_mismatch);
        }
        if (existing_matches) {
          digest = job.asset.content_hash;
        }
        existing_hash_size_mismatch = size_mismatch;
      }
	      if (digest.empty()) {
	        TimedAtomicWriterPhase phase(lock_stats_ ? &file_open_stats_ : nullptr);
	        output.open(job.target_path, std::ios::binary | std::ios::trunc);
	      }
	      HashedSparseWriteResult hashed_write;
	      if (digest.empty() && job.defer_content_hash) {
	        SparseIoStats sparse_stats;
	        if (lock_stats_) {
	          sparse_stats.zero_scan = &sparse_zero_scan_stats_;
	          sparse_stats.stream_write = &sparse_stream_write_stats_;
	          sparse_stats.seek = &sparse_seek_stats_;
	          sparse_stats.final_touch = &sparse_final_touch_stats_;
	        }
	        TimedAtomicWriterPhase phase(lock_stats_ ? &write_without_hash_stats_ : nullptr);
	        if (sparse_write_enabled_) {
	          hashed_write.sparse = write_payload_sparse(output, *job.payload, &sparse_stats);
	        } else if (output.is_open()) {
	          TimedAtomicWriterPhase write_phase(lock_stats_ ? &sparse_stream_write_stats_ : nullptr);
	          output.write(
	              reinterpret_cast<const char *>(job.payload->data()),
	              static_cast<std::streamsize>(job.payload->size()));
	          hashed_write.sparse.ok = output.good();
	        }
	      } else if (digest.empty()) {
	        SparseIoStats sparse_stats;
	        if (lock_stats_) {
	          sparse_stats.zero_scan = &sparse_zero_scan_stats_;
          sparse_stats.hash_update = &sparse_hash_update_stats_;
          sparse_stats.stream_write = &sparse_stream_write_stats_;
          sparse_stats.seek = &sparse_seek_stats_;
          sparse_stats.final_touch = &sparse_final_touch_stats_;
        }
        TimedAtomicWriterPhase phase(lock_stats_ ? &hash_and_write_stats_ : nullptr);
        hashed_write = hash_and_write_payload_sparse(output, *job.payload, &sparse_stats);
        digest = hashed_write.digest;
      }
      auto sparse_result = hashed_write.sparse;
      if (!sparse_result.ok) {
        TimedAtomicWriterPhase phase(lock_stats_ ? &file_close_stats_ : nullptr);
        output.close();
      }
      if (output.is_open()) {
        TimedAtomicWriterPhase phase(lock_stats_ ? &file_close_stats_ : nullptr);
        output.close();
      }

      auto final_relative_path = job.relative_path;
      auto final_path = job.target_path;
	      if (!digest.empty() && can_hash_address) {
        final_relative_path = job.hashed_directory_relative_path / (digest + job.hashed_extension);
        final_path = job.hashed_directory_path / (digest + job.hashed_extension);
        std::filesystem::create_directories(final_path.parent_path());
        std::uint64_t existing_byte_size = 0;
        bool size_mismatch = false;
        bool existing_matches = false;
        {
          TimedAtomicWriterPhase phase(lock_stats_ ? &final_file_check_stats_ : nullptr);
          existing_matches = existing_file_matches_content_addressed_size(
              final_path,
              job.payload->size(),
              existing_byte_size,
              &size_mismatch);
        }
        if (existing_matches) {
          hash_only_avoided_write = job.hash_only_if_final_exists;
          if (job.target_path != final_path && !std::filesystem::exists(job.target_path)) {
            TimedAtomicWriterPhase phase(lock_stats_ ? &link_or_copy_stats_ : nullptr);
            std::filesystem::create_directories(job.target_path.parent_path());
            std::error_code link_error;
            std::filesystem::create_hard_link(final_path, job.target_path, link_error);
            if (link_error) {
              link_error.clear();
              std::filesystem::copy_file(
                  final_path,
                  job.target_path,
                  std::filesystem::copy_options::overwrite_existing,
                  link_error);
            }
          }
        } else if (job.target_path != final_path) {
          existing_hash_size_mismatch = existing_hash_size_mismatch || size_mismatch;
          if (!std::filesystem::exists(job.target_path)) {
            TimedAtomicWriterPhase phase(lock_stats_ ? &late_write_stats_ : nullptr);
            std::ofstream late_output(job.target_path, std::ios::binary | std::ios::trunc);
            SparseIoStats sparse_stats;
            if (lock_stats_) {
              sparse_stats.zero_scan = &sparse_zero_scan_stats_;
              sparse_stats.stream_write = &sparse_stream_write_stats_;
              sparse_stats.seek = &sparse_seek_stats_;
              sparse_stats.final_touch = &sparse_final_touch_stats_;
            }
            const auto late_sparse_result = write_payload_sparse(late_output, *job.payload, &sparse_stats);
            if (late_output.is_open()) {
              TimedAtomicWriterPhase close_phase(lock_stats_ ? &file_close_stats_ : nullptr);
              late_output.close();
            }
            sparse_result.zero_run_count += late_sparse_result.zero_run_count;
            sparse_result.zero_bytes_skipped += late_sparse_result.zero_bytes_skipped;
            if (!late_sparse_result.ok) {
              final_relative_path = job.relative_path;
              final_path = job.target_path;
            }
          }
          if (std::filesystem::exists(job.target_path) && final_path != job.target_path) {
            TimedAtomicWriterPhase phase(lock_stats_ ? &link_or_copy_stats_ : nullptr);
            std::error_code error;
            std::filesystem::remove(final_path, error);
            error.clear();
            std::filesystem::create_hard_link(job.target_path, final_path, error);
            if (error) {
              error.clear();
              std::filesystem::copy_file(
                  job.target_path,
                  final_path,
                  std::filesystem::copy_options::overwrite_existing,
                  error);
              if (error) {
                final_relative_path = job.relative_path;
                final_path = job.target_path;
              }
            }
          }
        }
      }

      const auto payload_size = job.payload->size();
      const auto temporary_key = job.relative_path.generic_string();
      const auto digest_key = final_relative_path.generic_string();
      const bool hash_addressed = can_hash_address && temporary_key != digest_key;
      {
        TimedAtomicWriterPhase phase(lock_stats_ ? &completion_lock_stats_ : nullptr);
        TimedWriterLock lock(mutex_, lock_stats_);
        if (hash_addressed) {
          completed_path_aliases_[temporary_key] = digest_key;
        }
        if (!job.fingerprint_key.empty() && job.fingerprint_order != 0) {
          fingerprint_completed_counts_[job.fingerprint_key] = std::max(
              fingerprint_completed_counts_[job.fingerprint_key],
              job.fingerprint_order);
          const auto enqueued = fingerprint_enqueued_counts_.find(job.fingerprint_key);
          const auto completed = fingerprint_completed_counts_.find(job.fingerprint_key);
          if (enqueued != fingerprint_enqueued_counts_.end() &&
              completed != fingerprint_completed_counts_.end() &&
              completed->second >= enqueued->second) {
            fingerprint_enqueued_counts_.erase(enqueued);
            fingerprint_completed_counts_.erase(completed);
          }
        }
        pending_bytes_ -= payload_size;
        if (job.bulk_layer) {
          bulk_pending_bytes_ = payload_size > bulk_pending_bytes_ ? 0 : bulk_pending_bytes_ - payload_size;
        } else {
          regular_pending_bytes_ = payload_size > regular_pending_bytes_ ? 0 : regular_pending_bytes_ - payload_size;
        }
        writing_ = false;
      }
      cv_work_.notify_all();
      cv_fingerprint_.notify_all();
      if (completion_callback_) {
	        TimedAtomicWriterPhase phase(lock_stats_ ? &completion_callback_stats_ : nullptr);
	        completion_callback_(Completion{
	            job.asset,
	            job.relative_path,
	            final_relative_path,
	            digest,
	            std::move(job.content_key),
            std::move(job.fingerprint_key),
            job.payload,
            sparse_result.zero_run_count,
            sparse_result.zero_bytes_skipped,
            job.metal_kind,
            job.metal,
	            job.hash_only_if_final_exists,
	            hash_only_avoided_write,
	            hash_only_avoided_write ? payload_size : 0,
	            existing_hash_size_mismatch,
	            job.defer_content_hash,
	        });
	      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_work_;
  std::vector<std::thread> workers_;
  std::deque<Job> queue_;
  std::unordered_map<std::string, std::string> completed_path_aliases_;
  std::unordered_map<std::string, std::uint64_t> fingerprint_enqueued_counts_;
  std::unordered_map<std::string, std::uint64_t> fingerprint_completed_counts_;
  std::size_t max_pending_bytes_ = kDefaultMaxPendingBytes;
  std::size_t hard_max_pending_bytes_ = default_hard_max_pending_bytes();
	  std::size_t bulk_threshold_ = kDefaultBulkThreshold;
	  std::size_t worker_count_ = 4;
	  bool sparse_write_enabled_ = false;
	  std::size_t pending_bytes_ = 0;
  std::size_t regular_pending_bytes_ = 0;
  std::size_t bulk_pending_bytes_ = 0;
  std::atomic_uint64_t enqueue_capacity_wait_count_{0};
  std::atomic_uint64_t enqueue_capacity_wait_ns_{0};
  std::atomic_uint64_t enqueue_capacity_max_wait_ns_{0};
  std::atomic_uint64_t enqueue_soft_overflow_count_{0};
  std::atomic_uint64_t enqueue_soft_overflow_bytes_{0};
  std::atomic_uint64_t enqueue_regular_count_{0};
  std::atomic_uint64_t enqueue_regular_bytes_{0};
  std::atomic_uint64_t enqueue_bulk_count_{0};
  std::atomic_uint64_t enqueue_bulk_bytes_{0};
  std::atomic_uint64_t fingerprint_wait_count_{0};
  std::atomic_uint64_t fingerprint_wait_ns_{0};
  std::atomic_uint64_t fingerprint_max_wait_ns_{0};
	  AtomicWriterPhaseStats hash_payload_only_stats_;
	  AtomicWriterPhaseStats existing_file_check_stats_;
	  AtomicWriterPhaseStats hash_and_write_stats_;
	  AtomicWriterPhaseStats write_without_hash_stats_;
	  AtomicWriterPhaseStats sparse_zero_scan_stats_;
  AtomicWriterPhaseStats sparse_hash_update_stats_;
  AtomicWriterPhaseStats sparse_stream_write_stats_;
  AtomicWriterPhaseStats sparse_seek_stats_;
  AtomicWriterPhaseStats sparse_final_touch_stats_;
  AtomicWriterPhaseStats file_open_stats_;
  AtomicWriterPhaseStats file_close_stats_;
  AtomicWriterPhaseStats final_file_check_stats_;
  AtomicWriterPhaseStats late_write_stats_;
  AtomicWriterPhaseStats link_or_copy_stats_;
  AtomicWriterPhaseStats completion_lock_stats_;
  AtomicWriterPhaseStats completion_callback_stats_;
  WriterLockStats *lock_stats_ = nullptr;
  bool stop_ = false;
  bool writing_ = false;
  std::function<void(const Completion &)> completion_callback_;
  std::condition_variable cv_fingerprint_;
};

AsyncAssetWriter::Stats delta_asset_writer_stats(
    const AsyncAssetWriter::Stats &current,
    const AsyncAssetWriter::Stats &previous)
{
  AsyncAssetWriter::Stats value;
  value.mutex = delta_lock_stats(current.mutex, previous.mutex);
	  value.hash_payload_only = delta_phase(current.hash_payload_only, previous.hash_payload_only);
	  value.existing_file_check = delta_phase(current.existing_file_check, previous.existing_file_check);
	  value.hash_and_write = delta_phase(current.hash_and_write, previous.hash_and_write);
	  value.write_without_hash = delta_phase(current.write_without_hash, previous.write_without_hash);
	  value.sparse_zero_scan = delta_phase(current.sparse_zero_scan, previous.sparse_zero_scan);
  value.sparse_hash_update = delta_phase(current.sparse_hash_update, previous.sparse_hash_update);
  value.sparse_stream_write = delta_phase(current.sparse_stream_write, previous.sparse_stream_write);
  value.sparse_seek = delta_phase(current.sparse_seek, previous.sparse_seek);
  value.sparse_final_touch = delta_phase(current.sparse_final_touch, previous.sparse_final_touch);
  value.file_open = delta_phase(current.file_open, previous.file_open);
  value.file_close = delta_phase(current.file_close, previous.file_close);
  value.final_file_check = delta_phase(current.final_file_check, previous.final_file_check);
  value.late_write = delta_phase(current.late_write, previous.late_write);
  value.link_or_copy = delta_phase(current.link_or_copy, previous.link_or_copy);
  value.completion_lock = delta_phase(current.completion_lock, previous.completion_lock);
  value.completion_callback = delta_phase(current.completion_callback, previous.completion_callback);
  value.enqueue_capacity_wait_count =
      delta_value(current.enqueue_capacity_wait_count, previous.enqueue_capacity_wait_count);
  value.enqueue_capacity_wait_ns =
      delta_value(current.enqueue_capacity_wait_ns, previous.enqueue_capacity_wait_ns);
  value.enqueue_capacity_max_wait_ns = current.enqueue_capacity_max_wait_ns;
  value.enqueue_soft_overflow_count =
      delta_value(current.enqueue_soft_overflow_count, previous.enqueue_soft_overflow_count);
  value.enqueue_soft_overflow_bytes =
      delta_value(current.enqueue_soft_overflow_bytes, previous.enqueue_soft_overflow_bytes);
  value.enqueue_regular_count =
      delta_value(current.enqueue_regular_count, previous.enqueue_regular_count);
  value.enqueue_regular_bytes =
      delta_value(current.enqueue_regular_bytes, previous.enqueue_regular_bytes);
  value.enqueue_bulk_count =
      delta_value(current.enqueue_bulk_count, previous.enqueue_bulk_count);
  value.enqueue_bulk_bytes =
      delta_value(current.enqueue_bulk_bytes, previous.enqueue_bulk_bytes);
  value.fingerprint_wait_count =
      delta_value(current.fingerprint_wait_count, previous.fingerprint_wait_count);
  value.fingerprint_wait_ns =
      delta_value(current.fingerprint_wait_ns, previous.fingerprint_wait_ns);
  value.fingerprint_max_wait_ns = current.fingerprint_max_wait_ns;
  return value;
}

std::filesystem::path generated_asset_relative_path(
    std::string_view directory_name,
    std::string_view extension,
    std::uint64_t id)
{
  std::ostringstream filename;
  filename << "asset-" << std::hex << std::setw(16) << std::setfill('0') << id << extension;
  return std::filesystem::path(directory_name) / filename.str();
}

std::string asset_content_key(AssetKind kind, std::string_view content_hash)
{
  return std::to_string(static_cast<unsigned int>(kind)) + ":" + std::string(content_hash);
}

std::string asset_fingerprint_key(AssetKind kind, std::string_view fast_fingerprint)
{
  return std::to_string(static_cast<unsigned int>(kind)) + ":" + std::string(fast_fingerprint);
}

std::string metal_asset_content_key(MetalAssetKind metal_kind, AssetKind kind, std::string_view content_hash)
{
  return std::string("metal:") +
         std::to_string(static_cast<unsigned int>(metal_kind)) + ":" +
         std::to_string(static_cast<unsigned int>(kind)) + ":" +
         std::string(content_hash);
}

std::string metal_asset_fingerprint_key(MetalAssetKind metal_kind, AssetKind kind, std::string_view fast_fingerprint)
{
  return std::string("metal:") +
         std::to_string(static_cast<unsigned int>(metal_kind)) + ":" +
         std::to_string(static_cast<unsigned int>(kind)) + ":" +
         std::string(fast_fingerprint);
}

struct PendingAssetEntry {
  AssetRecord asset;
  std::shared_ptr<const std::vector<std::uint8_t>> payload;
  std::uint64_t payload_hash = 0;
};

struct PendingAssetBucket {
  std::unordered_map<std::uint64_t, std::vector<PendingAssetEntry>> entries_by_payload_hash;
  std::size_t entry_count = 0;
};

using PendingAssetIndex = std::unordered_map<std::string, PendingAssetBucket>;

struct PendingAssetSnapshot {
  std::vector<PendingAssetEntry> entries;
  std::size_t prefilter_candidates = 0;
};

struct PendingAssetLookupResult {
  std::optional<AssetRecord> asset;
  std::string alias_source_path;
  std::uint64_t compared_bytes = 0;
};

struct CachedAssetEvictionEntry {
  bool metal = false;
  std::string fingerprint_key;
  std::string relative_path_key;
  std::size_t payload_size = 0;
};

bool payload_equal(const std::vector<std::uint8_t> &lhs, const std::vector<std::uint8_t> &rhs)
{
  return lhs.size() == rhs.size() &&
         (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

std::uint64_t payload_dedup_hash(const std::vector<std::uint8_t> &payload)
{
  constexpr std::uint64_t kOffset = 14695981039346656037ull;
  std::uint64_t hash = kOffset;
  const auto size = payload.size();
  hash = fnv1a_update(hash, reinterpret_cast<const std::uint8_t *>(&size), sizeof(size));
  if (!payload.empty())
    hash = fnv1a_update(hash, payload.data(), payload.size());
  return hash;
}

AssetRecord alias_asset_record(AssetRecord requested, const AssetRecord &stored)
{
  requested.relative_path = stored.relative_path;
  requested.content_hash = stored.content_hash;
  requested.fast_fingerprint = stored.fast_fingerprint;
  requested.binary_payload = stored.binary_payload;
  requested.payload_bytes.clear();
  return requested;
}

PendingAssetLookupResult find_pending_asset(
    const std::vector<PendingAssetEntry> &entries,
    const std::vector<std::uint8_t> &payload,
    std::size_t exact_compare_limit)
{
  PendingAssetLookupResult result;
  if (payload.size() > exact_compare_limit)
    return result;

  for (const auto &entry : entries) {
    if (!entry.payload)
      continue;
    const auto compared_bytes = std::min(entry.payload->size(), payload.size());
    result.compared_bytes += compared_bytes;
    if (payload_equal(*entry.payload, payload)) {
      result.asset = entry.asset;
      result.alias_source_path = entry.asset.relative_path.generic_string();
      return result;
    }
  }
  return result;
}

PendingAssetSnapshot snapshot_pending_assets(
    const PendingAssetIndex &pending_assets,
    const std::string &fingerprint_key,
    std::size_t payload_size,
    std::size_t exact_compare_limit,
    std::uint64_t payload_hash)
{
  PendingAssetSnapshot snapshot;
  if (payload_size > exact_compare_limit)
    return snapshot;
  const auto found = pending_assets.find(fingerprint_key);
  if (found == pending_assets.end())
    return snapshot;
  snapshot.prefilter_candidates = found->second.entry_count;
  const auto matching_hash = found->second.entries_by_payload_hash.find(payload_hash);
  if (matching_hash == found->second.entries_by_payload_hash.end())
    return snapshot;
  snapshot.entries = matching_hash->second;
  return snapshot;
}

bool has_pending_asset(
    const PendingAssetIndex &pending_assets,
    const std::string &fingerprint_key)
{
  if (fingerprint_key.empty())
    return false;
  const auto found = pending_assets.find(fingerprint_key);
  return found != pending_assets.end() && found->second.entry_count != 0;
}

std::string effective_fast_fingerprint(const AssetRecord &asset, const std::vector<std::uint8_t> &payload)
{
  if (!asset.fast_fingerprint.empty())
    return asset.fast_fingerprint;
  return fast_fingerprint_bytes(payload.data(), payload.size());
}

bool existing_file_matches_content_hash(
    const std::filesystem::path &absolute_path,
    std::string_view content_hash,
    std::uint64_t expected_byte_size,
    std::uint64_t &byte_size,
    bool *size_mismatch)
{
  if (size_mismatch)
    *size_mismatch = false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(absolute_path, error))
    return false;

  const auto file_size = std::filesystem::file_size(absolute_path, error);
  if (error) {
    byte_size = 0;
    return false;
  }
  byte_size = file_size;
  if (expected_byte_size != 0 && file_size != expected_byte_size) {
    if (size_mismatch)
      *size_mismatch = true;
    return false;
  }

  if (sha256_file(absolute_path) != content_hash)
    return false;

  return true;
}

bool existing_file_matches_content_addressed_size(
    const std::filesystem::path &absolute_path,
    std::uint64_t expected_byte_size,
    std::uint64_t &byte_size,
    bool *size_mismatch)
{
  if (size_mismatch)
    *size_mismatch = false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(absolute_path, error))
    return false;

  const auto file_size = std::filesystem::file_size(absolute_path, error);
  if (error) {
    byte_size = 0;
    return false;
  }
  byte_size = file_size;
  if (expected_byte_size != 0 && file_size != expected_byte_size) {
    if (size_mismatch)
      *size_mismatch = true;
    return false;
  }

  return true;
}

void remove_pending_asset(
    PendingAssetIndex &pending_assets,
    const std::string &fingerprint_key,
    const std::filesystem::path &relative_path)
{
  if (fingerprint_key.empty())
    return;

  auto found = pending_assets.find(fingerprint_key);
  if (found == pending_assets.end())
    return;

  const auto key = relative_path.generic_string();
  auto &bucket = found->second;
  for (auto hash_bucket = bucket.entries_by_payload_hash.begin();
       hash_bucket != bucket.entries_by_payload_hash.end();) {
    auto &entries = hash_bucket->second;
    const auto previous_size = entries.size();
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const PendingAssetEntry &entry) {
        return entry.asset.relative_path.generic_string() == key;
      }),
      entries.end());
    bucket.entry_count -= previous_size - entries.size();
    if (entries.empty()) {
      hash_bucket = bucket.entries_by_payload_hash.erase(hash_bucket);
    } else {
      ++hash_bucket;
    }
  }
  if (bucket.entry_count == 0)
    pending_assets.erase(found);
}

void remove_cached_asset(
    PendingAssetIndex &cached_assets,
    const std::string &fingerprint_key,
    const std::string &relative_path_key)
{
  if (fingerprint_key.empty())
    return;

  auto found = cached_assets.find(fingerprint_key);
  if (found == cached_assets.end())
    return;

  auto &bucket = found->second;
  for (auto hash_bucket = bucket.entries_by_payload_hash.begin();
       hash_bucket != bucket.entries_by_payload_hash.end();) {
    auto &entries = hash_bucket->second;
    const auto previous_size = entries.size();
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const PendingAssetEntry &entry) {
        return entry.asset.relative_path.generic_string() == relative_path_key;
      }),
      entries.end());
    bucket.entry_count -= previous_size - entries.size();
    if (entries.empty()) {
      hash_bucket = bucket.entries_by_payload_hash.erase(hash_bucket);
    } else {
      ++hash_bucket;
    }
  }
  if (bucket.entry_count == 0)
    cached_assets.erase(found);
}

void add_cached_asset(
    PendingAssetIndex &cached_assets,
    std::deque<CachedAssetEvictionEntry> &eviction_order,
    std::size_t &cache_bytes,
    std::size_t cache_budget,
    bool metal,
    const std::string &fingerprint_key,
    AssetRecord asset,
    std::shared_ptr<const std::vector<std::uint8_t>> payload,
    std::uint64_t payload_hash)
{
  if (cache_budget == 0 || fingerprint_key.empty() || !payload || payload->empty() || payload->size() > cache_budget)
    return;

  auto &bucket = cached_assets[fingerprint_key];
  bucket.entries_by_payload_hash[payload_hash].push_back(PendingAssetEntry{asset, payload, payload_hash});
  ++bucket.entry_count;
  cache_bytes += payload->size();
  eviction_order.push_back(CachedAssetEvictionEntry{
      metal,
      fingerprint_key,
      asset.relative_path.generic_string(),
      payload->size(),
  });
}

std::size_t cached_asset_prefilter_count(
    const PendingAssetIndex &cached_assets,
    const std::string &fingerprint_key)
{
  const auto found = cached_assets.find(fingerprint_key);
  return found == cached_assets.end() ? 0 : found->second.entry_count;
}

void trim_cached_assets(
    PendingAssetIndex &cached_assets,
    PendingAssetIndex &cached_metal_assets,
    std::deque<CachedAssetEvictionEntry> &eviction_order,
    std::size_t &cache_bytes,
    std::size_t cache_budget)
{
  while (cache_bytes > cache_budget && !eviction_order.empty()) {
    const auto entry = eviction_order.front();
    eviction_order.pop_front();
    if (entry.metal) {
      remove_cached_asset(cached_metal_assets, entry.fingerprint_key, entry.relative_path_key);
    } else {
      remove_cached_asset(cached_assets, entry.fingerprint_key, entry.relative_path_key);
    }
    cache_bytes = entry.payload_size > cache_bytes ? 0 : cache_bytes - entry.payload_size;
  }
}

std::size_t replace_asset_path_aliases_in_line(
    std::string &line,
    const std::unordered_map<std::string, std::string> &path_aliases)
{
  if (path_aliases.empty())
    return 0;

  std::size_t replaced = 0;
  auto is_path_character = [](char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '/' ||
           value == '.' ||
           value == '_' ||
           value == '-';
  };

  std::size_t search_position = 0;
  while ((search_position = line.find("asset-", search_position)) != std::string::npos) {
    std::size_t path_begin = search_position;
    while (path_begin > 0 && is_path_character(line[path_begin - 1]))
      --path_begin;

    std::size_t path_end = search_position + 6;
    while (path_end < line.size() && is_path_character(line[path_end]))
      ++path_end;

    const auto candidate = line.substr(path_begin, path_end - path_begin);
    const auto alias = path_aliases.find(candidate);
    if (alias == path_aliases.end()) {
      search_position += 6;
      continue;
    }

    line.replace(path_begin, candidate.size(), alias->second);
    search_position = path_begin + alias->second.size();
    ++replaced;
  }
  return replaced;
}

bool is_rewrite_candidate(const std::filesystem::path &path)
{
  const auto extension = path.extension().generic_string();
  return extension == ".json" || extension == ".jsonl";
}

void add_rewrite_candidate(
    std::vector<std::filesystem::path> &candidates,
    std::unordered_set<std::string> &seen,
    const std::filesystem::path &path)
{
  if (path.empty() || !is_rewrite_candidate(path))
    return;
  const auto key = path.generic_string();
  if (seen.insert(key).second)
    candidates.push_back(path);
}

bool payload_may_reference_asset_alias(
    const std::filesystem::path &relative_path,
    const std::vector<std::uint8_t> &payload)
{
  if (!is_rewrite_candidate(relative_path) || payload.empty())
    return false;
  const auto payload_text = std::string_view(
      reinterpret_cast<const char *>(payload.data()),
      payload.size());
  return payload_text.find("asset-") != std::string_view::npos;
}

struct RewriteAssetReferenceResult {
  std::vector<std::filesystem::path> rewritten_relative_paths;
  std::unordered_map<std::string, std::string> rewritten_digests;
  std::uint64_t candidates_scanned = 0;
  std::uint64_t candidates_skipped_clean = 0;
  std::uint64_t replacements = 0;
};

RewriteAssetReferenceResult rewrite_bundle_asset_references(
    const BundleLayout &layout,
    const std::unordered_map<std::string, std::string> &path_aliases,
    const std::vector<std::filesystem::path> &candidate_paths)
{
  RewriteAssetReferenceResult result;
  if (path_aliases.empty())
    return result;

  for (const auto &source_path : candidate_paths) {
    if (source_path.empty() ||
        source_path == layout.checksums_path ||
        !std::filesystem::is_regular_file(source_path) ||
        !is_rewrite_candidate(source_path))
      continue;
    ++result.candidates_scanned;

    std::ifstream input(source_path, std::ios::binary);
    if (!input.is_open())
      continue;

    bool needs_rewrite = false;
    std::string line;
    while (std::getline(input, line)) {
      if (replace_asset_path_aliases_in_line(line, path_aliases) != 0) {
        needs_rewrite = true;
        break;
      }
    }
    input.close();
    if (!needs_rewrite) {
      ++result.candidates_skipped_clean;
      continue;
    }

    input.clear();
    input.open(source_path, std::ios::binary);
    if (!input.is_open())
      continue;

    const auto temporary_path = source_path.string() + ".rewrite.tmp";
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
      continue;

    Sha256 rewritten_digest;
    std::size_t replacement_count = 0;
    while (std::getline(input, line)) {
      replacement_count += replace_asset_path_aliases_in_line(line, path_aliases);
      output << line;
      rewritten_digest.update(line);
      if (!input.eof()) {
        output << '\n';
        rewritten_digest.update(std::string_view("\n", 1));
      }
    }
    input.close();
    output.close();

    if (replacement_count == 0) {
      std::filesystem::remove(temporary_path);
      continue;
    }
    result.replacements += replacement_count;

    std::error_code error;
    std::filesystem::rename(temporary_path, source_path, error);
    if (error) {
      std::filesystem::copy_file(
          temporary_path,
          source_path,
          std::filesystem::copy_options::overwrite_existing,
          error);
      std::filesystem::remove(temporary_path);
    }
    const auto relative_path = bundle_relative_path(source_path, layout.root_path);
    if (relative_path) {
      result.rewritten_relative_paths.push_back(*relative_path);
      result.rewritten_digests[relative_path->generic_string()] = rewritten_digest.final_hex();
    }
  }
  return result;
}

} // namespace

std::string content_hash_bytes(const void *data, std::size_t size)
{
  Sha256 sha256;
  if (data && size) {
    sha256.update(static_cast<const std::uint8_t *>(data), size);
  }
  return sha256.final_hex();
}

std::string content_hash_file(const std::filesystem::path &path)
{
  return sha256_file(path);
}

std::string content_hash_file_prefix(const std::filesystem::path &path, std::uint64_t byte_size)
{
  std::ifstream input(path, std::ios::binary);
  Sha256 sha256;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  std::uint64_t remaining = byte_size;
  while (input.good() && remaining != 0) {
    const auto chunk_size = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
    input.read(reinterpret_cast<char *>(buffer.data()), chunk_size);
    const auto read_count = static_cast<std::size_t>(input.gcount());
    if (read_count != 0) {
      sha256.update(buffer.data(), read_count);
      remaining -= read_count;
    }
  }
  return sha256.final_hex();
}

std::string fast_fingerprint_bytes(const void *data, std::size_t size)
{
  constexpr std::uint64_t kOffset = 14695981039346656037ull;
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  std::uint64_t hash = kOffset;
  hash = fnv1a_update(hash, reinterpret_cast<const std::uint8_t *>(&size), sizeof(size));
  if (bytes && size) {
    static constexpr std::size_t kSampleSize = 4096;
    static constexpr std::array<std::uint64_t, 5> kSampleNumerators = {0, 1, 2, 3, 4};
    std::size_t previous_end = 0;
    for (const auto numerator : kSampleNumerators) {
      const auto sample_index = static_cast<std::size_t>(numerator);
      const auto offset = size <= kSampleSize
                              ? 0
                              : ((size - kSampleSize) * sample_index) / (kSampleNumerators.size() - 1);
      const auto sample_end = std::min(size, offset + kSampleSize);
      if (sample_end <= previous_end)
        continue;
      const auto sample_begin = std::max(offset, previous_end);
      hash = fnv1a_update(hash, bytes + sample_begin, sample_end - sample_begin);
      previous_end = sample_end;
    }
  }
  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << hash << ":" << std::dec << size;
  return output.str();
}

struct TraceBundleWriter::Impl {
  BundleLayout layout;
  TraceMetadata metadata;
  std::vector<EventRecord> events;
  std::vector<MetalEventRecord> metal_events;
  std::vector<AssetRecord> assets;
  std::vector<AssetRecord> metal_assets;
  std::vector<ObjectRecord> objects;
  std::vector<std::string> analysis_streams;
  std::vector<AnalysisRecord> analysis_records;
  ChecksumIndex checksums;
  AsyncLineWriter callstream_stream;
  AsyncMetalEventWriter metal_callstream_stream;
  std::mutex event_mutex;
  std::mutex metadata_mutex;
  std::mutex metal_callstream_open_mutex;
  std::unordered_map<std::string, AsyncLineWriter> analysis_stream_files;
  std::size_t async_line_max_pending_bytes = 64ull * 1024ull * 1024ull;
  std::unordered_set<std::string> written_files;
  std::unordered_map<std::string, AssetRecord> assets_by_content_hash;
  std::unordered_map<std::string, AssetRecord> metal_assets_by_content_hash;
  std::unordered_map<std::string, std::string> content_hash_by_fast_fingerprint;
  std::unordered_map<std::string, std::string> metal_content_hash_by_fast_fingerprint;
  std::mutex fingerprint_hash_mutex;
  PendingAssetIndex pending_assets_by_fast_fingerprint;
  PendingAssetIndex pending_metal_assets_by_fast_fingerprint;
  PendingAssetIndex cached_assets_by_fast_fingerprint;
  PendingAssetIndex cached_metal_assets_by_fast_fingerprint;
  std::deque<CachedAssetEvictionEntry> cached_asset_eviction_order;
  std::unordered_map<std::string, std::string> known_file_digests;
  std::unordered_map<std::string, std::uint64_t> known_file_sizes;
  std::unordered_map<std::string, std::string> completed_path_aliases;
	  std::mutex asset_record_mutex;
	  std::mutex asset_mutex;
	  AsyncAssetWriter asset_writer;
	  std::mutex asset_completion_mutex;
	  std::condition_variable asset_completion_cv;
	  std::deque<AsyncAssetWriter::Completion> asset_completion_events;
	  std::thread asset_completion_thread;
	  std::atomic_uint64_t next_asset_id{1};
  std::size_t async_asset_threshold = 1024ull * 1024ull;
  std::size_t exact_dedup_compare_threshold = 1ull * 1024ull * 1024ull;
  std::size_t foreground_asset_dedup_threshold = 1024ull * 1024ull;
  std::size_t completed_asset_cache_budget = 256ull * 1024ull * 1024ull;
  std::size_t completed_asset_cache_bytes = 0;
  std::uint64_t asset_register_calls = 0;
  std::uint64_t metal_asset_register_calls = 0;
  std::uint64_t asset_empty_payloads = 0;
  std::uint64_t asset_payload_bytes_seen = 0;
  std::uint64_t asset_payload_move_registrations = 0;
  std::uint64_t asset_payload_move_bytes = 0;
  std::uint64_t asset_pending_dedup_hits = 0;
  std::uint64_t asset_completed_cache_hits = 0;
  std::uint64_t asset_hash_dedup_hits = 0;
  std::uint64_t asset_known_hash_hits = 0;
  std::uint64_t asset_known_hash_bytes_avoided = 0;
  std::uint64_t asset_exact_dedup_compares = 0;
  std::uint64_t asset_exact_dedup_compare_bytes = 0;
  std::uint64_t asset_hash_prefilter_candidates = 0;
  std::uint64_t asset_hash_prefilter_matches = 0;
  std::uint64_t asset_exact_dedup_skipped_large = 0;
  std::uint64_t asset_payload_cache_skipped_large = 0;
  std::uint64_t asset_sync_hashes = 0;
  std::uint64_t asset_sync_hash_bytes = 0;
  std::uint64_t asset_async_enqueued = 0;
  std::uint64_t asset_async_queue_rejected = 0;
  std::uint64_t asset_async_hash_only_candidates = 0;
  std::uint64_t asset_async_hash_only_write_avoids = 0;
  std::uint64_t asset_async_hash_only_write_bytes_avoided = 0;
  std::uint64_t asset_async_hash_only_late_writes = 0;
  std::atomic_uint64_t foreground_dedup_lock_bypass_count{0};
  std::atomic_uint64_t foreground_hash_hint_lock_bypass_count{0};
  std::atomic_uint64_t foreground_alias_lock_bypass_count{0};
  std::uint64_t asset_sync_writes = 0;
  std::uint64_t asset_existing_hash_size_mismatches = 0;
  std::uint64_t asset_sparse_zero_run_count = 0;
  std::uint64_t asset_sparse_zero_bytes_skipped = 0;
  std::uint64_t genericized_metal_resources = 0;
  std::uint64_t async_path_aliases = 0;
  std::uint64_t asset_rewrite_candidates_scanned = 0;
  std::uint64_t asset_rewrite_candidates_skipped_clean = 0;
  std::uint64_t asset_rewrite_replacements = 0;
  std::uint64_t asset_rewrite_digest_reuses = 0;
  std::uint64_t rewritten_asset_reference_files = 0;
  std::unordered_set<std::string> asset_payloads_may_reference_alias;
  std::uint64_t callstream_peak_pending_bytes = 0;
  std::uint64_t metal_callstream_peak_pending_bytes = 0;
  std::uint64_t analysis_peak_pending_bytes = 0;
  AsyncStreamStats callstream_stats;
  AsyncStreamStats metal_callstream_stats;
  AsyncStreamStats analysis_stream_stats;
  AsyncAssetWriter::Stats asset_writer_stats;
  WriterLockStats event_lock_stats;
  WriterLockStats metadata_lock_stats;
  WriterLockStats metal_callstream_open_lock_stats;
  WriterLockStats asset_record_lock_stats;
  WriterLockStats asset_lock_stats;
  WriterLockStats checkpoint_lock_stats;
  WriterLockStats callstream_lock_stats;
  WriterLockStats metal_callstream_lock_stats;
  WriterLockStats analysis_stream_lock_stats;
  WriterLockStats async_asset_lock_stats;
  AtomicWriterPhaseStats asset_register_phase_total;
  AtomicWriterPhaseStats asset_register_phase_payload_prepare;
  AtomicWriterPhaseStats asset_register_phase_fingerprint;
  AtomicWriterPhaseStats asset_register_phase_payload_hash;
  AtomicWriterPhaseStats asset_register_phase_index_snapshot;
  AtomicWriterPhaseStats asset_register_phase_exact_compare;
  AtomicWriterPhaseStats asset_register_phase_sync_hash;
  AtomicWriterPhaseStats asset_register_phase_async_enqueue;
  AtomicWriterPhaseStats asset_register_phase_alias_scan;
  AtomicWriterPhaseStats asset_register_phase_existing_file_check;
  AtomicWriterPhaseStats asset_register_phase_sync_write;
  AtomicWriterPhaseStats asset_register_phase_publish;
  AtomicWriterPhaseStats metal_event_phase_total;
  AtomicWriterPhaseStats metal_event_phase_open;
  AtomicWriterPhaseStats metal_event_phase_asset_ref_scan;
  AtomicWriterPhaseStats metal_event_phase_enqueue;
  WriterPhaseStats close_phase_analysis_flush;
  WriterPhaseStats close_phase_callstream_flush;
  WriterPhaseStats close_phase_metal_callstream_flush;
  WriterPhaseStats close_phase_asset_writer_drain;
  WriterPhaseStats close_phase_alias_rewrite;
  WriterPhaseStats close_phase_asset_index;
  WriterPhaseStats close_phase_writer_stats;
  WriterPhaseStats close_phase_checksum;
  std::atomic_bool callstream_may_reference_asset_alias{false};
  std::atomic_bool metal_callstream_may_reference_asset_alias{false};
  std::unordered_set<std::string> analysis_streams_may_reference_asset_alias;
  bool open = false;
  std::atomic_bool metadata_written{false};
  bool append_existing_primary_callstream = false;
  TraceBundleOpenMode open_mode = TraceBundleOpenMode::Primary;
  bool metal_callstream_should_append = false;
  std::atomic_bool metal_callstream_open_attempted{false};
  bool cache_events = false;
  bool inline_finalize = false;
  bool async_callstream_serialize = true;
  bool writer_stats = false;
  std::mutex checkpoint_mutex;
  std::condition_variable checkpoint_cv;
  std::thread checkpoint_thread;
  std::chrono::milliseconds checkpoint_interval{0};
  std::uint64_t checkpoint_event_interval = 0;
  std::uint64_t checkpoint_asset_bytes_interval = 0;
  std::atomic_uint64_t checkpoint_signal_events{0};
  std::atomic_uint64_t checkpoint_signal_asset_bytes{0};
	  bool checkpoint_stop = false;
	  bool checkpoint_now = false;
	  bool asset_completion_stop = false;
	  bool asset_completion_processing = false;
	  bool full_scan_checkpoint_written = false;

	  ~Impl()
	  {
	    stop_checkpoint_thread();
	    asset_writer.close();
	    drain_asset_completion_events();
	    stop_asset_completion_thread();
	  }

  std::string writer_stats_json(std::size_t checksum_file_count) const
  {
    auto write_phase = [](std::ostringstream &output, const char *name, const WriterPhaseStats &stats) {
      output << ",\"" << name << "_count\":" << stats.count
             << ",\"" << name << "_ns\":" << stats.ns
             << ",\"" << name << "_max_ns\":" << stats.max_ns;
    };
    const auto event_lock = snapshot_writer_lock_stats(event_lock_stats);
    const auto metadata_lock = snapshot_writer_lock_stats(metadata_lock_stats);
    const auto metal_open_lock = snapshot_writer_lock_stats(metal_callstream_open_lock_stats);
    const auto asset_record_lock = snapshot_writer_lock_stats(asset_record_lock_stats);
    const auto asset_lock = snapshot_writer_lock_stats(asset_lock_stats);
    const auto checkpoint_lock = snapshot_writer_lock_stats(checkpoint_lock_stats);
    std::ostringstream output;
    output << "{\"record_type\":\"writer_stats\""
           << ",\"root\":\"" << json_escape(layout.root_path.generic_string()) << "\""
           << ",\"asset_register_calls\":" << asset_register_calls
           << ",\"metal_asset_register_calls\":" << metal_asset_register_calls
           << ",\"genericized_metal_resources\":" << genericized_metal_resources
           << ",\"empty_payloads\":" << asset_empty_payloads
           << ",\"payload_bytes_seen\":" << asset_payload_bytes_seen
           << ",\"payload_move_registrations\":" << asset_payload_move_registrations
           << ",\"payload_move_bytes\":" << asset_payload_move_bytes
           << ",\"pending_dedup_hits\":" << asset_pending_dedup_hits
           << ",\"completed_cache_hits\":" << asset_completed_cache_hits
           << ",\"hash_dedup_hits\":" << asset_hash_dedup_hits
           << ",\"known_hash_hits\":" << asset_known_hash_hits
           << ",\"known_hash_bytes_avoided\":" << asset_known_hash_bytes_avoided
           << ",\"exact_dedup_compares\":" << asset_exact_dedup_compares
           << ",\"exact_dedup_compare_bytes\":" << asset_exact_dedup_compare_bytes
           << ",\"hash_prefilter_candidates\":" << asset_hash_prefilter_candidates
           << ",\"hash_prefilter_matches\":" << asset_hash_prefilter_matches
           << ",\"exact_dedup_skipped_large\":" << asset_exact_dedup_skipped_large
           << ",\"payload_cache_skipped_large\":" << asset_payload_cache_skipped_large
           << ",\"sync_hashes\":" << asset_sync_hashes
           << ",\"sync_hash_bytes\":" << asset_sync_hash_bytes
           << ",\"async_enqueued\":" << asset_async_enqueued
           << ",\"async_queue_rejected\":" << asset_async_queue_rejected
           << ",\"async_hash_only_candidates\":" << asset_async_hash_only_candidates
           << ",\"async_hash_only_write_avoids\":" << asset_async_hash_only_write_avoids
           << ",\"async_hash_only_write_bytes_avoided\":" << asset_async_hash_only_write_bytes_avoided
           << ",\"async_hash_only_late_writes\":" << asset_async_hash_only_late_writes
           << ",\"foreground_dedup_lock_bypass_count\":"
           << foreground_dedup_lock_bypass_count.load(std::memory_order_relaxed)
           << ",\"foreground_hash_hint_lock_bypass_count\":"
           << foreground_hash_hint_lock_bypass_count.load(std::memory_order_relaxed)
           << ",\"foreground_alias_lock_bypass_count\":"
           << foreground_alias_lock_bypass_count.load(std::memory_order_relaxed)
           << ",\"sync_writes\":" << asset_sync_writes
           << ",\"existing_hash_size_mismatches\":" << asset_existing_hash_size_mismatches
           << ",\"sparse_zero_run_count\":" << asset_sparse_zero_run_count
           << ",\"sparse_zero_bytes_skipped\":" << asset_sparse_zero_bytes_skipped
           << ",\"completed_cache_bytes\":" << completed_asset_cache_bytes
           << ",\"callstream_peak_pending_bytes\":" << callstream_peak_pending_bytes
           << ",\"callstream_mutex_lock_count\":" << callstream_stats.mutex_lock_count
           << ",\"callstream_mutex_wait_ns\":" << callstream_stats.mutex_wait_ns
           << ",\"callstream_mutex_max_wait_ns\":" << callstream_stats.mutex_max_wait_ns
           << ",\"callstream_mutex_hold_ns\":" << callstream_stats.mutex_hold_ns
           << ",\"callstream_mutex_max_hold_ns\":" << callstream_stats.mutex_max_hold_ns
           << ",\"metal_callstream_peak_pending_bytes\":" << metal_callstream_peak_pending_bytes
           << ",\"metal_callstream_mutex_lock_count\":" << metal_callstream_stats.mutex_lock_count
           << ",\"metal_callstream_mutex_wait_ns\":" << metal_callstream_stats.mutex_wait_ns
           << ",\"metal_callstream_mutex_max_wait_ns\":" << metal_callstream_stats.mutex_max_wait_ns
           << ",\"metal_callstream_mutex_hold_ns\":" << metal_callstream_stats.mutex_hold_ns
           << ",\"metal_callstream_mutex_max_hold_ns\":" << metal_callstream_stats.mutex_max_hold_ns
           << ",\"analysis_peak_pending_bytes\":" << analysis_peak_pending_bytes
           << ",\"analysis_mutex_lock_count\":" << analysis_stream_stats.mutex_lock_count
           << ",\"analysis_mutex_wait_ns\":" << analysis_stream_stats.mutex_wait_ns
           << ",\"analysis_mutex_max_wait_ns\":" << analysis_stream_stats.mutex_max_wait_ns
           << ",\"analysis_mutex_hold_ns\":" << analysis_stream_stats.mutex_hold_ns
           << ",\"analysis_mutex_max_hold_ns\":" << analysis_stream_stats.mutex_max_hold_ns
           << ",\"event_mutex_lock_count\":" << event_lock.lock_count
           << ",\"event_mutex_wait_ns\":" << event_lock.wait_ns
           << ",\"event_mutex_max_wait_ns\":" << event_lock.max_wait_ns
           << ",\"event_mutex_hold_ns\":" << event_lock.hold_ns
           << ",\"event_mutex_max_hold_ns\":" << event_lock.max_hold_ns
           << ",\"metadata_mutex_lock_count\":" << metadata_lock.lock_count
           << ",\"metadata_mutex_wait_ns\":" << metadata_lock.wait_ns
           << ",\"metadata_mutex_max_wait_ns\":" << metadata_lock.max_wait_ns
           << ",\"metadata_mutex_hold_ns\":" << metadata_lock.hold_ns
           << ",\"metadata_mutex_max_hold_ns\":" << metadata_lock.max_hold_ns
           << ",\"metal_open_mutex_lock_count\":" << metal_open_lock.lock_count
           << ",\"metal_open_mutex_wait_ns\":" << metal_open_lock.wait_ns
           << ",\"metal_open_mutex_max_wait_ns\":" << metal_open_lock.max_wait_ns
           << ",\"metal_open_mutex_hold_ns\":" << metal_open_lock.hold_ns
           << ",\"metal_open_mutex_max_hold_ns\":" << metal_open_lock.max_hold_ns
           << ",\"asset_record_mutex_lock_count\":" << asset_record_lock.lock_count
           << ",\"asset_record_mutex_wait_ns\":" << asset_record_lock.wait_ns
           << ",\"asset_record_mutex_max_wait_ns\":" << asset_record_lock.max_wait_ns
           << ",\"asset_record_mutex_hold_ns\":" << asset_record_lock.hold_ns
           << ",\"asset_record_mutex_max_hold_ns\":" << asset_record_lock.max_hold_ns
           << ",\"asset_mutex_lock_count\":" << asset_lock.lock_count
           << ",\"asset_mutex_wait_ns\":" << asset_lock.wait_ns
           << ",\"asset_mutex_max_wait_ns\":" << asset_lock.max_wait_ns
           << ",\"asset_mutex_hold_ns\":" << asset_lock.hold_ns
           << ",\"asset_mutex_max_hold_ns\":" << asset_lock.max_hold_ns
           << ",\"checkpoint_mutex_lock_count\":" << checkpoint_lock.lock_count
           << ",\"checkpoint_mutex_wait_ns\":" << checkpoint_lock.wait_ns
           << ",\"checkpoint_mutex_max_wait_ns\":" << checkpoint_lock.max_wait_ns
           << ",\"checkpoint_mutex_hold_ns\":" << checkpoint_lock.hold_ns
           << ",\"checkpoint_mutex_max_hold_ns\":" << checkpoint_lock.max_hold_ns
           << ",\"asset_writer_mutex_lock_count\":" << asset_writer_stats.mutex.lock_count
           << ",\"asset_writer_mutex_wait_ns\":" << asset_writer_stats.mutex.wait_ns
           << ",\"asset_writer_mutex_max_wait_ns\":" << asset_writer_stats.mutex.max_wait_ns
           << ",\"asset_writer_mutex_hold_ns\":" << asset_writer_stats.mutex.hold_ns
           << ",\"asset_writer_mutex_max_hold_ns\":" << asset_writer_stats.mutex.max_hold_ns
           << ",\"asset_writer_capacity_wait_count\":" << asset_writer_stats.enqueue_capacity_wait_count
           << ",\"asset_writer_capacity_wait_ns\":" << asset_writer_stats.enqueue_capacity_wait_ns
           << ",\"asset_writer_capacity_max_wait_ns\":" << asset_writer_stats.enqueue_capacity_max_wait_ns
           << ",\"asset_writer_soft_overflow_count\":" << asset_writer_stats.enqueue_soft_overflow_count
           << ",\"asset_writer_soft_overflow_bytes\":" << asset_writer_stats.enqueue_soft_overflow_bytes
           << ",\"asset_writer_regular_count\":" << asset_writer_stats.enqueue_regular_count
           << ",\"asset_writer_regular_bytes\":" << asset_writer_stats.enqueue_regular_bytes
           << ",\"asset_writer_bulk_count\":" << asset_writer_stats.enqueue_bulk_count
           << ",\"asset_writer_bulk_bytes\":" << asset_writer_stats.enqueue_bulk_bytes
           << ",\"asset_writer_fingerprint_wait_count\":" << asset_writer_stats.fingerprint_wait_count
           << ",\"asset_writer_fingerprint_wait_ns\":" << asset_writer_stats.fingerprint_wait_ns
           << ",\"asset_writer_fingerprint_max_wait_ns\":" << asset_writer_stats.fingerprint_max_wait_ns
           ;
	    write_phase(output, "asset_writer_hash_payload_only", asset_writer_stats.hash_payload_only);
	    write_phase(output, "asset_writer_existing_file_check", asset_writer_stats.existing_file_check);
	    write_phase(output, "asset_writer_hash_and_write", asset_writer_stats.hash_and_write);
	    write_phase(output, "asset_writer_write_without_hash", asset_writer_stats.write_without_hash);
	    write_phase(output, "asset_writer_sparse_zero_scan", asset_writer_stats.sparse_zero_scan);
    write_phase(output, "asset_writer_sparse_hash_update", asset_writer_stats.sparse_hash_update);
    write_phase(output, "asset_writer_sparse_stream_write", asset_writer_stats.sparse_stream_write);
    write_phase(output, "asset_writer_sparse_seek", asset_writer_stats.sparse_seek);
    write_phase(output, "asset_writer_sparse_final_touch", asset_writer_stats.sparse_final_touch);
    write_phase(output, "asset_writer_file_open", asset_writer_stats.file_open);
    write_phase(output, "asset_writer_file_close", asset_writer_stats.file_close);
    write_phase(output, "asset_writer_final_file_check", asset_writer_stats.final_file_check);
    write_phase(output, "asset_writer_late_write", asset_writer_stats.late_write);
    write_phase(output, "asset_writer_link_or_copy", asset_writer_stats.link_or_copy);
    write_phase(output, "asset_writer_completion_lock", asset_writer_stats.completion_lock);
    write_phase(output, "asset_writer_completion_callback", asset_writer_stats.completion_callback);
    write_phase(output, "asset_register_total", snapshot_writer_phase_stats(asset_register_phase_total));
    write_phase(output, "asset_register_payload_prepare", snapshot_writer_phase_stats(asset_register_phase_payload_prepare));
    write_phase(output, "asset_register_fingerprint", snapshot_writer_phase_stats(asset_register_phase_fingerprint));
    write_phase(output, "asset_register_payload_hash", snapshot_writer_phase_stats(asset_register_phase_payload_hash));
    write_phase(output, "asset_register_index_snapshot", snapshot_writer_phase_stats(asset_register_phase_index_snapshot));
    write_phase(output, "asset_register_exact_compare", snapshot_writer_phase_stats(asset_register_phase_exact_compare));
    write_phase(output, "asset_register_sync_hash", snapshot_writer_phase_stats(asset_register_phase_sync_hash));
    write_phase(output, "asset_register_async_enqueue", snapshot_writer_phase_stats(asset_register_phase_async_enqueue));
    write_phase(output, "asset_register_alias_scan", snapshot_writer_phase_stats(asset_register_phase_alias_scan));
    write_phase(output, "asset_register_existing_file_check", snapshot_writer_phase_stats(asset_register_phase_existing_file_check));
    write_phase(output, "asset_register_sync_write", snapshot_writer_phase_stats(asset_register_phase_sync_write));
    write_phase(output, "asset_register_publish", snapshot_writer_phase_stats(asset_register_phase_publish));
    write_phase(output, "metal_event_total", snapshot_writer_phase_stats(metal_event_phase_total));
    write_phase(output, "metal_event_open", snapshot_writer_phase_stats(metal_event_phase_open));
    write_phase(output, "metal_event_asset_ref_scan", snapshot_writer_phase_stats(metal_event_phase_asset_ref_scan));
    write_phase(output, "metal_event_enqueue", snapshot_writer_phase_stats(metal_event_phase_enqueue));
    write_phase(output, "close_analysis_flush", close_phase_analysis_flush);
    write_phase(output, "close_callstream_flush", close_phase_callstream_flush);
    write_phase(output, "close_metal_callstream_flush", close_phase_metal_callstream_flush);
    write_phase(output, "close_asset_writer_drain", close_phase_asset_writer_drain);
    write_phase(output, "close_alias_rewrite", close_phase_alias_rewrite);
    write_phase(output, "close_asset_index", close_phase_asset_index);
    write_phase(output, "close_writer_stats", close_phase_writer_stats);
    write_phase(output, "close_checksum", close_phase_checksum);
    output << ",\"callstream_enqueue_count\":" << callstream_stats.enqueue_count
           << ",\"callstream_enqueue_bytes\":" << callstream_stats.enqueue_bytes
           << ",\"callstream_wait_count\":" << callstream_stats.wait_count
           << ",\"callstream_wait_ns\":" << callstream_stats.wait_ns
           << ",\"callstream_max_wait_ns\":" << callstream_stats.max_wait_ns
           << ",\"callstream_write_count\":" << callstream_stats.write_count
           << ",\"callstream_write_bytes\":" << callstream_stats.write_bytes
           << ",\"callstream_write_ns\":" << callstream_stats.write_ns
           << ",\"callstream_max_write_ns\":" << callstream_stats.max_write_ns
           << ",\"metal_callstream_enqueue_count\":" << metal_callstream_stats.enqueue_count
           << ",\"metal_callstream_enqueue_bytes\":" << metal_callstream_stats.enqueue_bytes
           << ",\"metal_callstream_wait_count\":" << metal_callstream_stats.wait_count
           << ",\"metal_callstream_wait_ns\":" << metal_callstream_stats.wait_ns
           << ",\"metal_callstream_max_wait_ns\":" << metal_callstream_stats.max_wait_ns
           << ",\"metal_callstream_serialize_count\":" << metal_callstream_stats.serialize_count
           << ",\"metal_callstream_serialize_ns\":" << metal_callstream_stats.serialize_ns
           << ",\"metal_callstream_max_serialize_ns\":" << metal_callstream_stats.max_serialize_ns
           << ",\"metal_callstream_write_count\":" << metal_callstream_stats.write_count
           << ",\"metal_callstream_write_bytes\":" << metal_callstream_stats.write_bytes
           << ",\"metal_callstream_write_ns\":" << metal_callstream_stats.write_ns
           << ",\"metal_callstream_max_write_ns\":" << metal_callstream_stats.max_write_ns
           << ",\"analysis_enqueue_count\":" << analysis_stream_stats.enqueue_count
           << ",\"analysis_enqueue_bytes\":" << analysis_stream_stats.enqueue_bytes
           << ",\"analysis_wait_count\":" << analysis_stream_stats.wait_count
           << ",\"analysis_wait_ns\":" << analysis_stream_stats.wait_ns
           << ",\"analysis_max_wait_ns\":" << analysis_stream_stats.max_wait_ns
           << ",\"analysis_write_count\":" << analysis_stream_stats.write_count
           << ",\"analysis_write_bytes\":" << analysis_stream_stats.write_bytes
           << ",\"analysis_write_ns\":" << analysis_stream_stats.write_ns
           << ",\"analysis_max_write_ns\":" << analysis_stream_stats.max_write_ns
           << ",\"async_path_aliases\":" << async_path_aliases
           << ",\"asset_rewrite_candidates_scanned\":" << asset_rewrite_candidates_scanned
           << ",\"asset_rewrite_candidates_skipped_clean\":" << asset_rewrite_candidates_skipped_clean
           << ",\"asset_rewrite_replacements\":" << asset_rewrite_replacements
           << ",\"asset_rewrite_digest_reuses\":" << asset_rewrite_digest_reuses
           << ",\"rewritten_asset_reference_files\":" << rewritten_asset_reference_files
           << ",\"assets_indexed\":" << assets.size()
           << ",\"metal_assets_indexed\":" << metal_assets.size()
           << ",\"checksum_files\":" << checksum_file_count
           << "}";
    return output.str();
  }

  void snapshot_async_writer_stats()
  {
    callstream_peak_pending_bytes = callstream_stream.peak_pending_bytes();
    callstream_stats = callstream_stream.stats();
    if (metal_callstream_stream.is_open()) {
      metal_callstream_peak_pending_bytes = metal_callstream_stream.peak_pending_bytes();
      metal_callstream_stats = metal_callstream_stream.stats();
    }
    asset_writer_stats = asset_writer.stats();
  }

  std::filesystem::path write_writer_stats_analysis(const std::string &stats_json, std::string &digest)
  {
    std::filesystem::create_directories(layout.analysis_directory_path);
    const auto stats_path = layout.analysis_directory_path / "writer-stats.jsonl";
    std::ofstream output(stats_path, std::ios::binary | std::ios::app);
    if (!output.is_open()) {
      return {};
    }
    output << stats_json << "\n";
    output.close();
    digest = sha256_file(stats_path);
    const auto relative_path = bundle_relative_path(stats_path, layout.root_path);
    if (relative_path) {
      TimedWriterLock lock(asset_mutex, writer_stats ? &asset_lock_stats : nullptr);
      known_file_digests[relative_path->generic_string()] = digest;
    }
    return stats_path;
  }

  bool ensure_metal_callstream_open()
  {
    if (!open) {
      return false;
    }
    if (metal_callstream_stream.is_open()) {
      return true;
    }
    if (metal_callstream_open_attempted.load(std::memory_order_acquire)) {
      return metal_callstream_stream.is_open();
    }

    TimedWriterLock lock(metal_callstream_open_mutex, writer_stats ? &metal_callstream_open_lock_stats : nullptr);
    if (metal_callstream_stream.is_open()) {
      return true;
    }
    if (metal_callstream_open_attempted.load(std::memory_order_relaxed)) {
      return false;
    }

    const auto mode = metal_callstream_should_append ? std::ios::app : std::ios::trunc;
    const auto opened = metal_callstream_stream.open(layout.metal_callstream_path, mode);
    metal_callstream_open_attempted.store(true, std::memory_order_release);
    return opened;
  }

  std::vector<std::filesystem::path> rewrite_candidates() const
  {
    std::vector<std::filesystem::path> candidates;
    std::unordered_set<std::string> seen;
    add_rewrite_candidate(candidates, seen, layout.callstream_path);
    add_rewrite_candidate(candidates, seen, layout.metal_callstream_path);
    add_rewrite_candidate(candidates, seen, layout.asset_index_path);
    add_rewrite_candidate(candidates, seen, layout.object_index_path);
    add_rewrite_candidate(candidates, seen, layout.translation_links_path);
    for (const auto &stream_name : analysis_streams_may_reference_asset_alias)
      add_rewrite_candidate(candidates, seen, analysis_path_for_stream(layout, stream_name));
    add_rewrite_candidate(candidates, seen, layout.analysis_directory_path / "writer-stats.jsonl");
    for (const auto &relative_path : asset_payloads_may_reference_alias)
      add_rewrite_candidate(candidates, seen, layout.root_path / relative_path);
    return candidates;
  }

  std::unordered_map<std::string, std::string> completed_asset_path_aliases()
  {
    auto aliases = asset_writer.completed_path_aliases();
    TimedWriterLock lock(asset_mutex, writer_stats ? &asset_lock_stats : nullptr);
    aliases.insert(completed_path_aliases.begin(), completed_path_aliases.end());
    return aliases;
  }

  void publish_asset(const AssetRecord &asset)
  {
    TimedWriterLock lock(asset_record_mutex, writer_stats ? &asset_record_lock_stats : nullptr);
    assets.push_back(asset);
  }

  void publish_metal_asset(const AssetRecord &asset)
  {
    TimedWriterLock lock(asset_record_mutex, writer_stats ? &asset_record_lock_stats : nullptr);
    metal_assets.push_back(asset);
  }

	  void publish_queued_asset_for_async_write(const AssetRecord &asset, bool metal)
	  {
		    if (metal) {
		      publish_metal_asset(asset);
		    } else {
		      publish_asset(asset);
		    }
	    signal_checkpoint_work(0, asset.byte_size);
		  }

	  void enqueue_asset_completion(AsyncAssetWriter::Completion completion)
	  {
	    {
	      std::lock_guard lock(asset_completion_mutex);
	      asset_completion_events.push_back(std::move(completion));
	    }
	    asset_completion_cv.notify_one();
	  }

		  void process_asset_completion(const AsyncAssetWriter::Completion &completion)
		  {
		    const auto completed_byte_size = completion.asset.byte_size;
		    auto finalized = completion.asset;
		    if (!completion.digest.empty())
		      finalized.content_hash = completion.digest;
		    finalized.relative_path = completion.final_relative_path;
		    finalized.payload_bytes.clear();
	    std::vector<std::pair<std::uint64_t, std::filesystem::path>> alias_completions;
	    const bool can_cache_payload =
	        completion.payload && completion.payload->size() <= exact_dedup_compare_threshold;
	    const auto cache_payload_hash =
	        can_cache_payload ? payload_dedup_hash(*completion.payload) : 0;

	    {
	      TimedWriterLock lock(asset_mutex, writer_stats ? &asset_lock_stats : nullptr);
	      if (completion.existing_hash_size_mismatch)
	        ++asset_existing_hash_size_mismatches;
	      asset_sparse_zero_run_count += completion.sparse_zero_run_count;
	      asset_sparse_zero_bytes_skipped += completion.sparse_zero_bytes_skipped;
		      if (!completion.digest.empty() && completion.metal) {
		        const auto final_key = completion.content_key.empty()
		                                 ? metal_asset_content_key(completion.metal_kind, finalized.kind, completion.digest)
		                                 : completion.content_key;
		        metal_assets_by_content_hash[final_key] = finalized;
		      } else if (!completion.digest.empty()) {
		        const auto final_key = completion.content_key.empty()
		                                 ? asset_content_key(finalized.kind, completion.digest)
		                                 : completion.content_key;
		        assets_by_content_hash[final_key] = finalized;
		      }
		      if (!completion.digest.empty())
		        known_file_digests[finalized.relative_path.generic_string()] = completion.digest;
		      known_file_sizes[finalized.relative_path.generic_string()] = finalized.byte_size;
	      if (completion.relative_path != completion.final_relative_path) {
	        const auto temporary_key = completion.relative_path.generic_string();
	        const auto final_key = completion.final_relative_path.generic_string();
	        completed_path_aliases[temporary_key] = final_key;
	        known_file_sizes.erase(temporary_key);
	        known_file_sizes[final_key] = finalized.byte_size;
	        if (asset_payloads_may_reference_alias.erase(temporary_key) != 0)
	          asset_payloads_may_reference_alias.insert(final_key);
	      }
	      if (!completion.fingerprint_key.empty() && completion.hash_only_candidate &&
	          completion.hash_only_avoided_write) {
	        ++asset_async_hash_only_write_avoids;
	        asset_async_hash_only_write_bytes_avoided += completion.hash_only_write_bytes_avoided;
	      } else if (!completion.fingerprint_key.empty() && completion.hash_only_candidate &&
	                 completion.content_key.empty() &&
	                 !completion.hash_only_avoided_write &&
	                 completion.relative_path != completion.final_relative_path) {
	        ++asset_async_hash_only_late_writes;
	      }

		    if (!completion.fingerprint_key.empty() && !completion.digest.empty()) {
	        if (completion.metal) {
	          remove_pending_asset(
	              pending_metal_assets_by_fast_fingerprint,
	              completion.fingerprint_key,
	              completion.relative_path);
	          if (can_cache_payload) {
	            add_cached_asset(
	                cached_metal_assets_by_fast_fingerprint,
	                cached_asset_eviction_order,
	                completed_asset_cache_bytes,
	                completed_asset_cache_budget,
	                true,
	                completion.fingerprint_key,
	                finalized,
	                completion.payload,
	                cache_payload_hash);
	          } else {
	            ++asset_payload_cache_skipped_large;
	          }
	        } else {
	          remove_pending_asset(
	              pending_assets_by_fast_fingerprint,
	              completion.fingerprint_key,
	              completion.relative_path);
	          if (can_cache_payload) {
	            add_cached_asset(
	                cached_assets_by_fast_fingerprint,
	                cached_asset_eviction_order,
	                completed_asset_cache_bytes,
	                completed_asset_cache_budget,
	                false,
	                completion.fingerprint_key,
	                finalized,
	                completion.payload,
	                cache_payload_hash);
	          } else {
	            ++asset_payload_cache_skipped_large;
	          }
	        }
	        trim_cached_assets(
	            cached_assets_by_fast_fingerprint,
	            cached_metal_assets_by_fast_fingerprint,
	            cached_asset_eviction_order,
	            completed_asset_cache_bytes,
	            completed_asset_cache_budget);
	      }
	    }
	    if (!completion.fingerprint_key.empty()) {
	      TimedWriterLock lock(fingerprint_hash_mutex, writer_stats ? &asset_lock_stats : nullptr);
	      if (completion.metal) {
	        metal_content_hash_by_fast_fingerprint[completion.fingerprint_key] = completion.digest;
	      } else {
	        content_hash_by_fast_fingerprint[completion.fingerprint_key] = completion.digest;
	      }
	    }
	    {
	      TimedWriterLock lock(asset_record_mutex, writer_stats ? &asset_record_lock_stats : nullptr);
	      auto &records = completion.metal ? metal_assets : assets;
	      for (const auto &record : records) {
	        if (record.relative_path == completion.asset.relative_path) {
	          alias_completions.emplace_back(record.blob_id, record.relative_path);
	        }
	      }
	    }
	    update_published_asset_record(finalized, completion.metal, completion.asset.relative_path);
	    for (const auto &[blob_id, original_path] : alias_completions) {
	      if (blob_id == finalized.blob_id)
	        continue;
	      auto alias = finalized;
	      alias.blob_id = blob_id;
	      update_published_asset_record(alias, completion.metal, original_path);
	    }
	    signal_checkpoint_work(0, completed_byte_size);
	  }

	  void start_asset_completion_thread()
	  {
	    if (asset_completion_thread.joinable())
	      return;
	    asset_completion_stop = false;
	    asset_completion_processing = false;
	    asset_completion_thread = std::thread([this]() {
	      for (;;) {
	        AsyncAssetWriter::Completion completion;
	        {
	          std::unique_lock lock(asset_completion_mutex);
	          asset_completion_cv.wait(lock, [this]() {
	            return asset_completion_stop || !asset_completion_events.empty();
	          });
	          if (asset_completion_events.empty() && asset_completion_stop)
	            break;
	          completion = std::move(asset_completion_events.front());
	          asset_completion_events.pop_front();
	          asset_completion_processing = true;
	        }
	        process_asset_completion(completion);
	        {
	          std::lock_guard lock(asset_completion_mutex);
	          asset_completion_processing = false;
	        }
	        asset_completion_cv.notify_all();
	      }
	    });
	  }

	  void drain_asset_completion_events()
	  {
	    std::unique_lock lock(asset_completion_mutex);
	    asset_completion_cv.wait(lock, [this]() {
	      return asset_completion_events.empty() && !asset_completion_processing;
	    });
	  }

	  void stop_asset_completion_thread()
	  {
	    {
	      std::lock_guard lock(asset_completion_mutex);
	      asset_completion_stop = true;
	    }
	    asset_completion_cv.notify_all();
	    if (asset_completion_thread.joinable())
	      asset_completion_thread.join();
	  }

	  void update_published_asset_record(
      const AssetRecord &finalized,
      bool metal,
      const std::filesystem::path &original_relative_path)
  {
    const auto original_key = original_relative_path.generic_string();
    TimedWriterLock lock(asset_record_mutex, writer_stats ? &asset_record_lock_stats : nullptr);
    auto &records = metal ? metal_assets : assets;
    for (auto &record : records) {
      if (record.blob_id == finalized.blob_id &&
          record.relative_path.generic_string() == original_key) {
        record = finalized;
      }
    }
  }

  void signal_checkpoint_work(std::uint64_t event_count, std::uint64_t asset_bytes)
  {
    bool should_wake = false;
    if (event_count != 0 && checkpoint_event_interval != 0) {
      const auto total = checkpoint_signal_events.fetch_add(event_count, std::memory_order_relaxed) + event_count;
      should_wake = should_wake || total >= checkpoint_event_interval;
    }
    if (asset_bytes != 0 && checkpoint_asset_bytes_interval != 0) {
      const auto total = checkpoint_signal_asset_bytes.fetch_add(asset_bytes, std::memory_order_relaxed) + asset_bytes;
      should_wake = should_wake || total >= checkpoint_asset_bytes_interval;
    }
    if (!should_wake)
      return;

    {
      TimedWriterLock lock(checkpoint_mutex, writer_stats ? &checkpoint_lock_stats : nullptr);
      checkpoint_now = true;
    }
    checkpoint_cv.notify_one();
  }

	  void drain_async_work_for_checkpoint()
	  {
    {
      TimedWriterPhase phase(writer_stats ? &close_phase_analysis_flush : nullptr);
      for (auto &entry : analysis_stream_files) {
        if (entry.second.is_open()) {
          entry.second.flush();
          analysis_peak_pending_bytes = std::max<std::uint64_t>(
              analysis_peak_pending_bytes,
              entry.second.peak_pending_bytes());
          const auto stats = entry.second.stats();
          analysis_stream_stats.enqueue_count += stats.enqueue_count;
          analysis_stream_stats.enqueue_bytes += stats.enqueue_bytes;
          analysis_stream_stats.wait_count += stats.wait_count;
          analysis_stream_stats.wait_ns += stats.wait_ns;
          analysis_stream_stats.max_wait_ns =
              std::max(analysis_stream_stats.max_wait_ns, stats.max_wait_ns);
          analysis_stream_stats.write_count += stats.write_count;
          analysis_stream_stats.write_bytes += stats.write_bytes;
          analysis_stream_stats.write_ns += stats.write_ns;
          analysis_stream_stats.max_write_ns =
              std::max(analysis_stream_stats.max_write_ns, stats.max_write_ns);
        }
      }
    }
    if (writer_stats) {
      const auto analysis_lock = snapshot_writer_lock_stats(analysis_stream_lock_stats);
      analysis_stream_stats.mutex_lock_count = analysis_lock.lock_count;
      analysis_stream_stats.mutex_wait_ns = analysis_lock.wait_ns;
      analysis_stream_stats.mutex_max_wait_ns = analysis_lock.max_wait_ns;
      analysis_stream_stats.mutex_hold_ns = analysis_lock.hold_ns;
      analysis_stream_stats.mutex_max_hold_ns = analysis_lock.max_hold_ns;
    }
    if (callstream_stream.is_open()) {
      TimedWriterPhase phase(writer_stats ? &close_phase_callstream_flush : nullptr);
      callstream_stream.flush();
      callstream_peak_pending_bytes = callstream_stream.peak_pending_bytes();
      callstream_stats = callstream_stream.stats();
      callstream_stream.close();
    }
    if (metal_callstream_stream.is_open()) {
      TimedWriterPhase phase(writer_stats ? &close_phase_metal_callstream_flush : nullptr);
      metal_callstream_stream.flush();
      metal_callstream_peak_pending_bytes = metal_callstream_stream.peak_pending_bytes();
      metal_callstream_stats = metal_callstream_stream.stats();
      metal_callstream_stream.close();
    }
	    {
	      TimedWriterPhase phase(writer_stats ? &close_phase_asset_writer_drain : nullptr);
	      asset_writer.close();
	      drain_asset_completion_events();
	      asset_writer_stats = asset_writer.stats();
	    }
	  }

  void write_writer_stats_for_checkpoint(std::unordered_map<std::string, std::string> &known_digest_snapshot)
  {
    if (!writer_stats)
      return;
    TimedWriterPhase phase(writer_stats ? &close_phase_writer_stats : nullptr);
    const auto relative_path = std::filesystem::path(kAnalysisDirectoryName) / "writer-stats.jsonl";
    auto relative_paths = collect_bundle_relative_paths_full_scan(layout);
    std::unordered_set<std::string> seen;
    for (const auto &path : relative_paths)
      seen.insert(path.generic_string());
    add_checksum_candidate(relative_paths, seen, relative_path);
    std::sort(relative_paths.begin(), relative_paths.end());
    relative_paths.erase(std::unique(relative_paths.begin(), relative_paths.end()), relative_paths.end());
    std::string digest;
    const auto stats_json = writer_stats_json(relative_paths.size());
    if (!write_writer_stats_analysis(stats_json, digest).empty()) {
      known_digest_snapshot[relative_path.generic_string()] = digest;
    }
    std::cerr << "apitrace-writer-stats " << stats_json << "\n";
  }

  void checkpoint_once(bool full_scan = false)
  {
    if (!open)
      return;
    if (!full_scan &&
        open_mode == TraceBundleOpenMode::Primary &&
        std::filesystem::is_regular_file(layout.root_path / "seal-checkpoint-primary.ready")) {
      return;
    }
    if (!full_scan && full_scan_checkpoint_written)
      return;

    struct StreamCheckpointSnapshot {
      std::string name;
      std::filesystem::path path;
      std::string digest;
    };
    std::vector<StreamCheckpointSnapshot> analysis_stream_snapshot;
    for (auto &entry : analysis_stream_files) {
      if (entry.second.is_open()) {
        entry.second.flush();
        analysis_stream_snapshot.push_back({entry.first, analysis_path_for_stream(layout, entry.first), entry.second.current_digest()});
      }
    }
    AsyncLineWriter::CheckpointSnapshot callstream_snapshot;
    if (callstream_stream.is_open()) {
      callstream_snapshot = callstream_stream.checkpoint_snapshot();
    }
    AsyncLineWriter::CheckpointSnapshot metal_callstream_snapshot;
    if (metal_callstream_stream.is_open()) {
      metal_callstream_snapshot = metal_callstream_stream.checkpoint_snapshot();
    }
    std::vector<AssetRecord> asset_snapshot;
    std::vector<AssetRecord> metal_asset_snapshot;
    std::unordered_map<std::string, std::string> known_file_digests_snapshot;
    std::unordered_map<std::string, std::uint64_t> known_file_sizes_snapshot;
    {
      TimedWriterLock lock(asset_record_mutex, writer_stats ? &asset_record_lock_stats : nullptr);
      asset_snapshot = assets;
      metal_asset_snapshot = metal_assets;
    }
	    {
	      TimedWriterLock lock(asset_mutex, writer_stats ? &asset_lock_stats : nullptr);
	      known_file_digests_snapshot = known_file_digests;
	      known_file_sizes_snapshot = known_file_sizes;
	    }
	    if (full_scan)
	      write_writer_stats_for_checkpoint(known_file_digests_snapshot);
	    if (callstream_snapshot.valid) {
      known_file_digests_snapshot[kCallstreamFileName] = callstream_snapshot.digest;
      known_file_sizes_snapshot[kCallstreamFileName] = callstream_snapshot.byte_size;
    }
    if (metal_callstream_snapshot.valid) {
      known_file_digests_snapshot[kMetalCallstreamFileName] = metal_callstream_snapshot.digest;
      known_file_sizes_snapshot[kMetalCallstreamFileName] = metal_callstream_snapshot.byte_size;
    }
    for (const auto &entry : analysis_stream_snapshot) {
      std::error_code error;
      if (std::filesystem::is_regular_file(entry.path, error) && !error) {
        const auto relative_path = bundle_relative_path(entry.path, layout.root_path);
        if (relative_path) {
          known_file_digests_snapshot[relative_path->generic_string()] =
              !entry.digest.empty() ? entry.digest : sha256_file(entry.path);
        }
      }
    }

	      std::unordered_map<std::string, std::string> async_path_aliases;
	      if (full_scan && inline_finalize) {
	        async_path_aliases = completed_asset_path_aliases();
        const auto rewrite_result = rewrite_bundle_asset_references(
            layout,
            async_path_aliases,
            rewrite_candidates());
        this->async_path_aliases = async_path_aliases.size();
        asset_rewrite_candidates_scanned += rewrite_result.candidates_scanned;
        asset_rewrite_candidates_skipped_clean += rewrite_result.candidates_skipped_clean;
        asset_rewrite_replacements += rewrite_result.replacements;
        rewritten_asset_reference_files += rewrite_result.rewritten_relative_paths.size();
        for (const auto &entry : rewrite_result.rewritten_digests) {
          known_file_digests_snapshot[entry.first] = entry.second;
          known_file_sizes_snapshot.erase(entry.first);
        }
      }
      const auto sideband_asset_index = asset_index_json(
          asset_snapshot,
          metal_asset_snapshot,
          layout.root_path,
          async_path_aliases,
          known_file_digests_snapshot,
	          known_file_sizes_snapshot);
	      write_text_atomic(layout.analysis_directory_path / kSidebandAssetShardFileName, sideband_asset_index);
	    if (open_mode == TraceBundleOpenMode::SidebandOnly) {
	      checkpoint_signal_events.store(0, std::memory_order_relaxed);
	      checkpoint_signal_asset_bytes.store(0, std::memory_order_relaxed);
	      return;
	    }

    if (open_mode == TraceBundleOpenMode::Primary && inline_finalize &&
        should_write_asset_index(layout.asset_index_path, asset_snapshot, metal_asset_snapshot)) {
      std::unordered_map<std::string, std::string> async_path_aliases;
      if (full_scan) {
        const auto blob_remap = merge_sideband_asset_shard(
            layout.analysis_directory_path / kSidebandAssetShardFileName,
            asset_snapshot,
            metal_asset_snapshot);
        if (const auto rewritten = rewrite_metal_callstream_blob_refs(layout, blob_remap)) {
          known_file_digests_snapshot[kMetalCallstreamFileName] = rewritten->first;
          known_file_sizes_snapshot[kMetalCallstreamFileName] = rewritten->second;
        }
        async_path_aliases = completed_asset_path_aliases();
        const auto rewrite_result = rewrite_bundle_asset_references(
            layout,
            async_path_aliases,
            rewrite_candidates());
        this->async_path_aliases = async_path_aliases.size();
        asset_rewrite_candidates_scanned += rewrite_result.candidates_scanned;
        asset_rewrite_candidates_skipped_clean += rewrite_result.candidates_skipped_clean;
        asset_rewrite_replacements += rewrite_result.replacements;
        rewritten_asset_reference_files += rewrite_result.rewritten_relative_paths.size();
        for (const auto &entry : rewrite_result.rewritten_digests) {
          known_file_digests_snapshot[entry.first] = entry.second;
          known_file_sizes_snapshot.erase(entry.first);
        }
      }
      const auto asset_index = asset_index_json(
          asset_snapshot,
          metal_asset_snapshot,
          layout.root_path,
          async_path_aliases,
          known_file_digests_snapshot,
          known_file_sizes_snapshot);
      write_text_atomic(layout.asset_index_path, asset_index);
      known_file_digests_snapshot[kAssetIndexFileName] =
          content_hash_bytes(asset_index.data(), asset_index.size());
    }

    if (!inline_finalize) {
      checkpoint_signal_events.store(0, std::memory_order_relaxed);
      checkpoint_signal_asset_bytes.store(0, std::memory_order_relaxed);
      if (full_scan)
        full_scan_checkpoint_written = true;
      return;
    }

    auto relative_paths = full_scan
                              ? collect_bundle_relative_paths_full_scan(layout)
                              : collect_writer_known_relative_paths(layout, known_file_digests_snapshot, {});
    std::unordered_set<std::string> seen;
    for (const auto &relative_path : relative_paths)
      seen.insert(relative_path.generic_string());
    auto add_asset_paths = [&](const std::vector<AssetRecord> &records) {
      for (const auto &asset : records)
        add_checksum_candidate(relative_paths, seen, asset.relative_path);
    };
    add_asset_paths(asset_snapshot);
    add_asset_paths(metal_asset_snapshot);
	    add_checksum_candidate(relative_paths, seen, std::filesystem::path(kAssetIndexFileName));
	    add_asset_index_paths(relative_paths, seen, layout.asset_index_path);
	    for (const auto &entry : known_file_digests_snapshot)
	      add_checksum_candidate(relative_paths, seen, std::filesystem::path(entry.first));
    if (std::filesystem::is_directory(layout.root_path)) {
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator(layout.root_path, error)) {
        if (error)
          break;
        if (!entry.is_regular_file(error) || error)
          continue;
        const auto filename = entry.path().filename().generic_string();
        if (filename == std::string(kChecksumsFileName) ||
            filename.rfind(std::string(kChecksumsFileName) + ".", 0) == 0 ||
            filename.rfind(std::string(kAssetIndexFileName) + ".", 0) == 0) {
          continue;
        }
        add_checksum_candidate_absolute(layout, relative_paths, seen, entry.path());
      }
    }
    std::sort(relative_paths.begin(), relative_paths.end());
    relative_paths.erase(std::unique(relative_paths.begin(), relative_paths.end()), relative_paths.end());

    ChecksumIndex checkpoint_checksums = checksums;
    checkpoint_checksums.files.clear();
    checkpoint_checksums.format_version = metadata.format_version;
    for (const auto &relative_path : relative_paths) {
      const auto absolute_path = layout.root_path / relative_path;
      if (!std::filesystem::exists(absolute_path) || std::filesystem::is_directory(absolute_path))
        continue;

      const auto key = relative_path.generic_string();
      ChecksumRecord file_record;
      file_record.relative_path = relative_path;
      if (const auto known_size = known_file_sizes_snapshot.find(key);
          known_size != known_file_sizes_snapshot.end()) {
        file_record.byte_size = known_size->second;
      } else {
        file_record.byte_size = regular_file_size_or_zero(absolute_path);
      }
      file_record.has_byte_size = true;
      if (const auto known_digest = known_file_digests_snapshot.find(key);
          known_digest != known_file_digests_snapshot.end()) {
        file_record.digest = known_digest->second;
      } else {
        file_record.digest = sha256_file(absolute_path);
      }
      checkpoint_checksums.files.push_back(file_record);
    }
    checkpoint_checksums.files = unique_checksum_records(checkpoint_checksums.files);
    checkpoint_checksums.bundle_hash = bundle_hash_from_records(checkpoint_checksums.files);
    write_text_atomic(layout.checksums_path, checksum_index_json(checkpoint_checksums));
    if (full_scan)
      full_scan_checkpoint_written = true;
    checkpoint_signal_events.store(0, std::memory_order_relaxed);
    checkpoint_signal_asset_bytes.store(0, std::memory_order_relaxed);
  }

  void start_checkpoint_thread()
  {
    if ((checkpoint_interval.count() <= 0 &&
         checkpoint_event_interval == 0 &&
         checkpoint_asset_bytes_interval == 0) ||
        checkpoint_thread.joinable())
      return;

    {
      TimedWriterLock lock(checkpoint_mutex, writer_stats ? &checkpoint_lock_stats : nullptr);
      checkpoint_stop = false;
    }
    checkpoint_thread = std::thread([this]() {
      auto lock = timed_unique_lock(checkpoint_mutex, writer_stats ? &checkpoint_lock_stats : nullptr);
      for (;;) {
        if (checkpoint_interval.count() > 0) {
          checkpoint_cv.wait_for(lock, checkpoint_interval, [this]() {
            return checkpoint_stop || checkpoint_now;
          });
        } else {
          checkpoint_cv.wait(lock, [this]() {
            return checkpoint_stop || checkpoint_now;
          });
        }
        if (checkpoint_stop)
          break;
        checkpoint_now = false;
        lock.unlock();
        checkpoint_once();
        lock.lock();
        checkpoint_cv.notify_all();
      }
    });
  }

  void stop_checkpoint_thread()
  {
    {
      TimedWriterLock lock(checkpoint_mutex, writer_stats ? &checkpoint_lock_stats : nullptr);
      checkpoint_stop = true;
    }
    checkpoint_cv.notify_all();
    if (checkpoint_thread.joinable())
      checkpoint_thread.join();
  }

  // TODO: split buffered readable indexes from buffered raw asset writes once persistence begins.
  // TODO: add explicit writer-phase state so open/write/close sequencing can be validated.
};

TraceBundleWriter::TraceBundleWriter() : impl_(std::make_unique<Impl>()) {}

TraceBundleWriter::~TraceBundleWriter() = default;

bool TraceBundleWriter::open(const std::filesystem::path &bundle_root, TraceBundleOpenMode mode)
{
  impl_ = std::make_unique<Impl>();
  impl_->open_mode = mode;
  impl_->cache_events = env_flag_enabled("APITRACE_CACHE_EVENTS");
  impl_->inline_finalize = env_flag_enabled("APITRACE_INLINE_FINALIZE");
  impl_->async_callstream_serialize =
      env_flag_enabled_default("APITRACE_ASYNC_CALLSTREAM_SERIALIZE", true);
  impl_->writer_stats = env_flag_enabled("APITRACE_WRITER_STATS");
  impl_->async_asset_threshold = env_size_or_default("APITRACE_ASYNC_ASSET_THRESHOLD", impl_->async_asset_threshold);
  impl_->exact_dedup_compare_threshold =
      env_size_or_default("APITRACE_EXACT_DEDUP_COMPARE_THRESHOLD", impl_->exact_dedup_compare_threshold);
  impl_->foreground_asset_dedup_threshold =
      env_size_or_default("APITRACE_FOREGROUND_ASSET_DEDUP_THRESHOLD", impl_->foreground_asset_dedup_threshold);
  impl_->completed_asset_cache_budget =
      env_size_or_default("APITRACE_COMPLETED_ASSET_CACHE_MAX", impl_->completed_asset_cache_budget);
  impl_->async_line_max_pending_bytes =
      env_size_or_default("APITRACE_ASYNC_LINE_MAX_PENDING", impl_->async_line_max_pending_bytes);
  impl_->callstream_stream.set_max_pending_bytes(impl_->async_line_max_pending_bytes);
  impl_->metal_callstream_stream.set_max_pending_bytes(impl_->async_line_max_pending_bytes);
	  impl_->asset_writer.set_max_pending_bytes(env_size_or_default("APITRACE_ASYNC_ASSET_MAX_PENDING", 1024ull * 1024ull * 1024ull));
	  impl_->asset_writer.set_hard_max_pending_bytes(env_size_or_default("APITRACE_ASYNC_ASSET_HARD_MAX_PENDING", 4ull * 1024ull * 1024ull * 1024ull));
	  impl_->asset_writer.set_bulk_threshold(env_size_or_default("APITRACE_ASYNC_ASSET_BULK_THRESHOLD", impl_->async_asset_threshold));
	  impl_->asset_writer.set_worker_count(env_size_or_default("APITRACE_ASYNC_ASSET_WORKERS", 4));
	  impl_->asset_writer.set_sparse_write_enabled(env_flag_enabled("APITRACE_ASSET_SPARSE_WRITE"));
	  if (impl_->writer_stats) {
    impl_->callstream_stream.set_lock_stats(&impl_->callstream_lock_stats);
    impl_->metal_callstream_stream.set_lock_stats(&impl_->metal_callstream_lock_stats);
    impl_->asset_writer.set_lock_stats(&impl_->async_asset_lock_stats);
  }
  impl_->start_asset_completion_thread();
  impl_->asset_writer.set_completion_callback([impl = impl_.get()](const AsyncAssetWriter::Completion &completion) {
    if (impl)
      impl->enqueue_asset_completion(completion);
  });
  impl_->layout.root_path = bundle_root;
  impl_->layout.callstream_path = bundle_root / kCallstreamFileName;
  impl_->layout.metal_callstream_path = bundle_root / kMetalCallstreamFileName;
  impl_->layout.checksums_path = bundle_root / kChecksumsFileName;
  impl_->layout.asset_index_path = bundle_root / kAssetIndexFileName;
  impl_->layout.analysis_directory_path = bundle_root / kAnalysisDirectoryName;
  impl_->layout.translation_links_path = impl_->layout.analysis_directory_path / kTranslationLinksFileName;
  impl_->layout.objects_directory_path = bundle_root / kObjectsDirectoryName;
  impl_->layout.object_index_path = impl_->layout.objects_directory_path / kObjectIndexFileName;
  impl_->layout.shaders_directory_path = bundle_root / kShadersDirectoryName;
  impl_->layout.textures_directory_path = bundle_root / kTexturesDirectoryName;
  impl_->layout.buffers_directory_path = bundle_root / kBuffersDirectoryName;
  impl_->layout.pipelines_directory_path = bundle_root / kPipelinesDirectoryName;
  impl_->layout.metal_directory_path = bundle_root / kMetalDirectoryName;
  impl_->layout.metal_libraries_directory_path = impl_->layout.metal_directory_path / kMetalLibrariesDirectoryName;
  impl_->layout.metal_pipelines_directory_path = impl_->layout.metal_directory_path / kMetalPipelinesDirectoryName;
  impl_->layout.metal_buffers_directory_path = impl_->layout.metal_directory_path / kMetalBuffersDirectoryName;
  impl_->layout.metal_textures_directory_path = impl_->layout.metal_directory_path / kMetalTexturesDirectoryName;

  std::filesystem::create_directories(impl_->layout.root_path);
  std::filesystem::create_directories(impl_->layout.analysis_directory_path);
  std::filesystem::create_directories(impl_->layout.objects_directory_path);
  std::filesystem::create_directories(impl_->layout.shaders_directory_path);
  std::filesystem::create_directories(impl_->layout.textures_directory_path);
  std::filesystem::create_directories(impl_->layout.buffers_directory_path);
  std::filesystem::create_directories(impl_->layout.pipelines_directory_path);
  std::filesystem::create_directories(impl_->layout.metal_libraries_directory_path);
  std::filesystem::create_directories(impl_->layout.metal_pipelines_directory_path);

  const bool existing_primary_callstream =
      std::filesystem::is_regular_file(impl_->layout.callstream_path) &&
      std::filesystem::file_size(impl_->layout.callstream_path) > 0;
  impl_->metal_callstream_should_append =
      mode != TraceBundleOpenMode::Primary &&
      std::filesystem::is_regular_file(impl_->layout.metal_callstream_path);
  if (mode == TraceBundleOpenMode::Primary) {
    const auto callstream_mode =
        existing_primary_callstream ? std::ios::app : std::ios::trunc;
    impl_->callstream_stream.open(impl_->layout.callstream_path, callstream_mode);
    impl_->open = impl_->callstream_stream.is_open();
    impl_->metadata_written.store(impl_->open && existing_primary_callstream, std::memory_order_relaxed);
    impl_->append_existing_primary_callstream = impl_->metadata_written.load(std::memory_order_relaxed);
  } else {
    impl_->metadata_written.store(existing_primary_callstream, std::memory_order_relaxed);
    impl_->open = true;
  }
  if (impl_->open && mode == TraceBundleOpenMode::Primary) {
    write_object_index({});
  } else if (impl_->open && !std::filesystem::exists(impl_->layout.object_index_path)) {
    write_object_index({});
  }
  impl_->checkpoint_interval = std::chrono::milliseconds(
      env_size_or_default("APITRACE_CHECKPOINT_INTERVAL_MS", 0));
  impl_->checkpoint_event_interval =
      env_size_or_default("APITRACE_CHECKPOINT_EVENT_INTERVAL", impl_->checkpoint_event_interval);
  impl_->checkpoint_asset_bytes_interval =
      env_size_or_default("APITRACE_CHECKPOINT_ASSET_BYTES", impl_->checkpoint_asset_bytes_interval);
  if (impl_->open)
    impl_->start_checkpoint_thread();
  return impl_->open;
}

void TraceBundleWriter::write_metadata(const TraceMetadata &metadata)
{
  impl_->metadata = metadata;
  if (!impl_->open || !impl_->callstream_stream.is_open()) {
    return;
  }
  TimedWriterLock lock(impl_->metadata_mutex, impl_->writer_stats ? &impl_->metadata_lock_stats : nullptr);
  if (impl_->metadata_written.load(std::memory_order_acquire)) {
    return;
  }

  std::ostringstream line;
  line << "{\"record_kind\":\"bundle_header\""
       << ",\"format_version\":" << metadata.format_version
       << ",\"api\":\"" << api_name(metadata.api) << "\""
       << ",\"producer\":\"" << json_escape(metadata.producer) << "\""
       << ",\"has_metal_callstream\":" << (metadata.has_metal_callstream ? "true" : "false")
       << ",\"entry_file\":\"" << kCallstreamFileName << "\""
       << "}";
  impl_->callstream_stream.write_line(line.str());
  impl_->metadata_written.store(true, std::memory_order_release);
}

void TraceBundleWriter::append_call_event(const EventRecord &event)
{
  EventRecord copy = event;
  append_call_event(std::move(copy));
}

void TraceBundleWriter::append_call_event(EventRecord &&event)
{
  if (impl_->cache_events) {
    TimedWriterLock lock(impl_->event_mutex, impl_->writer_stats ? &impl_->event_lock_stats : nullptr);
    impl_->events.push_back(event);
  }
  if (!impl_->open || !impl_->callstream_stream.is_open()) {
    return;
  }

  if (!impl_->metadata_written.load(std::memory_order_acquire)) {
    write_metadata(impl_->metadata);
  }

  if (!event.blob_refs.empty() || event.payload.find("asset-") != std::string::npos) {
    impl_->callstream_may_reference_asset_alias.store(true, std::memory_order_relaxed);
  }
  if (impl_->async_callstream_serialize) {
    impl_->callstream_stream.write_event(std::move(event));
  } else {
    impl_->callstream_stream.write_line(event_record_json(event));
  }
  impl_->signal_checkpoint_work(1, 0);
}

void TraceBundleWriter::append_metal_event(const MetalEventRecord &event)
{
  TimedWriterPhase total_phase(impl_->writer_stats ? &impl_->metal_event_phase_total : nullptr);
  if (impl_->cache_events) {
    TimedWriterLock lock(impl_->event_mutex, impl_->writer_stats ? &impl_->event_lock_stats : nullptr);
    impl_->metal_events.push_back(event);
  }
  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->metal_event_phase_open : nullptr);
    if (!impl_->ensure_metal_callstream_open()) {
      return;
    }
  }
  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->metal_event_phase_asset_ref_scan : nullptr);
    if (event.payload.find("asset-") != std::string::npos ||
        event.function_name.find("asset-") != std::string::npos)
      impl_->metal_callstream_may_reference_asset_alias.store(true, std::memory_order_relaxed);
  }
  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->metal_event_phase_enqueue : nullptr);
    impl_->metal_callstream_stream.write_event(event);
  }
  impl_->signal_checkpoint_work(1, 0);
}

AssetRecord TraceBundleWriter::register_asset(const AssetRecord &asset)
{
  AssetRecord copy = asset;
  return register_asset(std::move(copy));
}

AssetRecord TraceBundleWriter::register_asset(AssetRecord &&asset)
{
  TimedWriterPhase total_phase(impl_->writer_stats ? &impl_->asset_register_phase_total : nullptr);
  AssetRecord finalized = std::move(asset);
  const bool moved_payload = !finalized.payload_bytes.empty();
  ++impl_->asset_register_calls;
  if (finalized.payload_bytes.empty()) {
    ++impl_->asset_empty_payloads;
    impl_->publish_asset(finalized);
    return finalized;
  }
  const auto declared_byte_size = finalized.byte_size;
  if (!finalized.content_hash.empty()) {
    const auto content_key = asset_content_key(finalized.kind, finalized.content_hash);
    {
      TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
      if (const auto existing = impl_->assets_by_content_hash.find(content_key);
          existing != impl_->assets_by_content_hash.end()) {
        ++impl_->asset_hash_dedup_hits;
        ++impl_->asset_known_hash_hits;
        impl_->asset_known_hash_bytes_avoided += declared_byte_size != 0
                                                     ? declared_byte_size
                                                     : finalized.payload_bytes.size();
        finalized = alias_asset_record(std::move(finalized), existing->second);
        impl_->publish_asset(finalized);
        return finalized;
      }
    }
    const auto hash_relative_path =
        std::filesystem::path(asset_directory_name(finalized.kind)) /
        (finalized.content_hash + asset_extension(finalized.kind));
    std::uint64_t existing_byte_size = 0;
    bool existing_matches = false;
    {
      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_existing_file_check : nullptr);
      existing_matches = existing_file_matches_content_hash(
          impl_->layout.root_path / hash_relative_path,
          finalized.content_hash,
          declared_byte_size,
          existing_byte_size,
          nullptr);
    }
    if (existing_matches) {
      finalized.relative_path = hash_relative_path;
      finalized.byte_size = declared_byte_size != 0 ? declared_byte_size : existing_byte_size;
      finalized.payload_bytes.clear();
      ++impl_->asset_hash_dedup_hits;
      ++impl_->asset_known_hash_hits;
      impl_->asset_known_hash_bytes_avoided += finalized.byte_size;
      {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->assets_by_content_hash.emplace(content_key, finalized);
        impl_->known_file_digests[finalized.relative_path.generic_string()] = finalized.content_hash;
        impl_->known_file_sizes[finalized.relative_path.generic_string()] = finalized.byte_size;
      }
      impl_->publish_asset(finalized);
      return finalized;
    }
  }

	  std::shared_ptr<std::vector<std::uint8_t>> payload;
  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_payload_prepare : nullptr);
    payload = std::make_shared<std::vector<std::uint8_t>>(std::move(finalized.payload_bytes));
    finalized.byte_size = payload->size();
    impl_->asset_payload_bytes_seen += payload->size();
    if (moved_payload) {
      ++impl_->asset_payload_move_registrations;
      impl_->asset_payload_move_bytes += payload->size();
    }
  }
	  if (impl_->inline_finalize) {
	    TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_fingerprint : nullptr);
	    finalized.fast_fingerprint = effective_fast_fingerprint(finalized, *payload);
	  }
	  const auto fingerprint_key = impl_->inline_finalize
	      ? asset_fingerprint_key(finalized.kind, finalized.fast_fingerprint)
	      : std::string();
	  const bool foreground_dedup =
	      impl_->inline_finalize && payload->size() < impl_->foreground_asset_dedup_threshold;
	  if (!fingerprint_key.empty()) {
	    PendingAssetSnapshot cached_snapshot;
	    bool skipped_large_compare =
	        !foreground_dedup || payload->size() > impl_->exact_dedup_compare_threshold;
	    if (foreground_dedup) {
	      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_index_snapshot : nullptr);
	      TimedTryWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
	      if (lock) {
	        cached_snapshot.prefilter_candidates =
	            cached_asset_prefilter_count(impl_->cached_assets_by_fast_fingerprint, fingerprint_key);
	      } else {
	        impl_->foreground_dedup_lock_bypass_count.fetch_add(1, std::memory_order_relaxed);
	      }
	    }
	    if (skipped_large_compare)
	      ++impl_->asset_exact_dedup_skipped_large;
	    if (foreground_dedup &&
	        !skipped_large_compare &&
	        cached_snapshot.prefilter_candidates != 0 &&
	        impl_->completed_asset_cache_budget != 0 &&
	        impl_->completed_asset_cache_bytes != 0) {
	      std::uint64_t payload_hash = 0;
	      {
	        TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_payload_hash : nullptr);
	        payload_hash = payload_dedup_hash(*payload);
	      }
	      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_index_snapshot : nullptr);
	      TimedTryWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
	      if (lock) {
	        cached_snapshot = snapshot_pending_assets(
	            impl_->cached_assets_by_fast_fingerprint,
	            fingerprint_key,
	            payload->size(),
	            impl_->exact_dedup_compare_threshold,
	            payload_hash);
	        impl_->asset_hash_prefilter_candidates += cached_snapshot.prefilter_candidates;
	        impl_->asset_hash_prefilter_matches += cached_snapshot.entries.size();
	      } else {
	        impl_->foreground_dedup_lock_bypass_count.fetch_add(1, std::memory_order_relaxed);
	      }
	    }
	    PendingAssetLookupResult cached;
	    if (foreground_dedup) {
	      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_exact_compare : nullptr);
	      cached = find_pending_asset(
	          cached_snapshot.entries,
          *payload,
          impl_->exact_dedup_compare_threshold);
        if (cached.compared_bytes != 0) {
          ++impl_->asset_exact_dedup_compares;
          impl_->asset_exact_dedup_compare_bytes += cached.compared_bytes;
        }
      }
	    if (cached.asset) {
	      ++impl_->asset_completed_cache_hits;
	      finalized = alias_asset_record(std::move(finalized), *cached.asset);
	      impl_->publish_asset(finalized);
	      return finalized;
	    }
	  }

	  if (finalized.content_hash.empty()) {
	    if (finalized.relative_path.empty()) {
	      finalized.relative_path = generated_asset_relative_path(
	          asset_directory_name(finalized.kind),
	          asset_extension(finalized.kind),
	          impl_->next_asset_id.fetch_add(1, std::memory_order_relaxed));
	    }
	    const auto target_path = impl_->layout.root_path / finalized.relative_path;
	    const auto content_key = std::string();
	    bool hash_only_if_final_exists = false;
	    if (!fingerprint_key.empty()) {
	      TimedWriterLock lock(impl_->fingerprint_hash_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
	      hash_only_if_final_exists =
	          impl_->content_hash_by_fast_fingerprint.find(fingerprint_key) !=
	          impl_->content_hash_by_fast_fingerprint.end();
	    }
		    if (impl_->inline_finalize) {
		      bool may_reference_alias = false;
		      {
		        TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_alias_scan : nullptr);
		        may_reference_alias = payload_may_reference_asset_alias(finalized.relative_path, *payload);
		      }
		      if (may_reference_alias) {
		        TimedTryWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
		        if (lock) {
		          impl_->asset_payloads_may_reference_alias.insert(finalized.relative_path.generic_string());
		        } else {
		          impl_->foreground_alias_lock_bypass_count.fetch_add(1, std::memory_order_relaxed);
		        }
		      }
		    }
	    bool enqueued = false;
	    {
	      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_async_enqueue : nullptr);
	      enqueued = impl_->asset_writer.enqueue(
	            target_path,
	            finalized.relative_path,
	            asset_directory(impl_->layout, finalized.kind),
	            std::filesystem::path(asset_directory_name(finalized.kind)),
	            asset_extension(finalized.kind),
	            payload,
	            content_key,
	            fingerprint_key,
		            finalized,
		            MetalAssetKind::Buffer,
		            false,
		            hash_only_if_final_exists,
		            true,
		            [impl = impl_.get(), finalized]() {
		              impl->publish_queued_asset_for_async_write(finalized, false);
		            });
	    }
	    if (enqueued) {
	      ++impl_->asset_async_enqueued;
	      if (hash_only_if_final_exists)
	        ++impl_->asset_async_hash_only_candidates;
	  finalized.payload_bytes.clear();
	  return finalized;
	}
	    ++impl_->asset_async_queue_rejected;
	    ++impl_->asset_sync_hashes;
	    impl_->asset_sync_hash_bytes += payload->size();
	    {
	      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_sync_hash : nullptr);
	      finalized.content_hash = sha256_bytes(*payload);
	    }
	  }
  const auto content_key = asset_content_key(finalized.kind, finalized.content_hash);
  {
    TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
    if (const auto existing = impl_->assets_by_content_hash.find(content_key);
        existing != impl_->assets_by_content_hash.end()) {
      ++impl_->asset_hash_dedup_hits;
      finalized = alias_asset_record(std::move(finalized), existing->second);
      impl_->publish_asset(finalized);
      return finalized;
    }
    if (!fingerprint_key.empty())
      impl_->content_hash_by_fast_fingerprint[fingerprint_key] = finalized.content_hash;
  }
  if (finalized.relative_path.empty()) {
    finalized.relative_path =
        std::filesystem::path(asset_directory_name(finalized.kind)) / (finalized.content_hash + asset_extension(finalized.kind));
  }

  if (payload->size() >= impl_->async_asset_threshold) {
    const auto target_path = impl_->layout.root_path / finalized.relative_path;
    bool enqueued = false;
    {
      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_async_enqueue : nullptr);
      enqueued = impl_->asset_writer.enqueue(
            target_path,
            finalized.relative_path,
            std::filesystem::path(),
            std::filesystem::path(),
            std::string(),
            payload,
            content_key,
            fingerprint_key,
	            finalized,
		            MetalAssetKind::Buffer,
		            false,
		            false,
		            false,
		            [impl = impl_.get(), finalized]() {
		              impl->publish_queued_asset_for_async_write(finalized, false);
		            });
    }
    if (enqueued) {
      ++impl_->asset_async_enqueued;
      if (impl_->inline_finalize) {
        bool may_reference_alias = false;
        {
          TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_alias_scan : nullptr);
          may_reference_alias = payload_may_reference_asset_alias(finalized.relative_path, *payload);
        }
        if (may_reference_alias) {
          TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
          impl_->asset_payloads_may_reference_alias.insert(finalized.relative_path.generic_string());
        }
      }
      {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->assets_by_content_hash.emplace(content_key, finalized);
      }
      return finalized;
    }
    ++impl_->asset_async_queue_rejected;
  }

  const auto target_path = impl_->layout.root_path / finalized.relative_path;
  std::filesystem::create_directories(target_path.parent_path());
  std::uint64_t existing_byte_size = 0;
  if (impl_->inline_finalize) {
    bool may_reference_alias = false;
    {
      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_alias_scan : nullptr);
      may_reference_alias = payload_may_reference_asset_alias(finalized.relative_path, *payload);
    }
    if (may_reference_alias) {
      TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
      impl_->asset_payloads_may_reference_alias.insert(finalized.relative_path.generic_string());
    }
  }
  bool size_mismatch = false;
  bool existing_matches = false;
  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_existing_file_check : nullptr);
    existing_matches = existing_file_matches_content_hash(
        target_path,
        finalized.content_hash,
        payload->size(),
        existing_byte_size,
        &size_mismatch);
  }
  if (!existing_matches) {
    if (size_mismatch)
      ++impl_->asset_existing_hash_size_mismatches;
    ++impl_->asset_sync_writes;
    {
      TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_sync_write : nullptr);
      std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
      const auto sparse_result = write_payload_sparse(output, *payload);
      impl_->asset_sparse_zero_run_count += sparse_result.zero_run_count;
      impl_->asset_sparse_zero_bytes_skipped += sparse_result.zero_bytes_skipped;
    }
  }

  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_publish : nullptr);
    impl_->publish_asset(finalized);
  }
  {
    TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
    impl_->assets_by_content_hash.emplace(content_key, finalized);
    impl_->known_file_digests[finalized.relative_path.generic_string()] = finalized.content_hash;
    impl_->known_file_sizes[finalized.relative_path.generic_string()] = finalized.byte_size;
  }
  finalized.payload_bytes.clear();
  return finalized;
}

AssetRecord TraceBundleWriter::register_metal_asset(MetalAssetKind kind, const AssetRecord &asset)
{
  AssetRecord copy = asset;
  return register_metal_asset(kind, std::move(copy));
}

AssetRecord TraceBundleWriter::register_metal_asset(MetalAssetKind kind, AssetRecord &&asset)
{
  ++impl_->metal_asset_register_calls;
  if (kind == MetalAssetKind::Buffer || kind == MetalAssetKind::Texture) {
    ++impl_->genericized_metal_resources;
    AssetRecord generic = std::move(asset);
    generic.kind = kind == MetalAssetKind::Buffer ? AssetKind::Buffer : AssetKind::Texture;
    return register_asset(std::move(generic));
  }

  AssetRecord finalized = std::move(asset);
  const bool moved_payload = !finalized.payload_bytes.empty();
  if (finalized.payload_bytes.empty()) {
    ++impl_->asset_empty_payloads;
    impl_->publish_metal_asset(finalized);
    return finalized;
  }
  const auto declared_byte_size = finalized.byte_size;
  if (!finalized.content_hash.empty()) {
    const auto content_key = metal_asset_content_key(kind, finalized.kind, finalized.content_hash);
    {
      TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
      if (const auto existing = impl_->metal_assets_by_content_hash.find(content_key);
          existing != impl_->metal_assets_by_content_hash.end()) {
        ++impl_->asset_hash_dedup_hits;
        ++impl_->asset_known_hash_hits;
        impl_->asset_known_hash_bytes_avoided += declared_byte_size != 0
                                                     ? declared_byte_size
                                                     : finalized.payload_bytes.size();
        finalized = alias_asset_record(std::move(finalized), existing->second);
        impl_->publish_metal_asset(finalized);
        return finalized;
      }
    }
    const auto hash_relative_path =
        std::filesystem::path(detail::metal_asset_directory_name(kind)) /
        (finalized.content_hash + detail::metal_asset_extension(kind));
    std::uint64_t existing_byte_size = 0;
    if (existing_file_matches_content_hash(
            impl_->layout.root_path / hash_relative_path,
            finalized.content_hash,
            declared_byte_size,
            existing_byte_size,
            nullptr)) {
      finalized.relative_path = hash_relative_path;
      finalized.byte_size = declared_byte_size != 0 ? declared_byte_size : existing_byte_size;
      finalized.payload_bytes.clear();
      ++impl_->asset_hash_dedup_hits;
      ++impl_->asset_known_hash_hits;
      impl_->asset_known_hash_bytes_avoided += finalized.byte_size;
      {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->metal_assets_by_content_hash.emplace(content_key, finalized);
        impl_->known_file_digests[finalized.relative_path.generic_string()] = finalized.content_hash;
        impl_->known_file_sizes[finalized.relative_path.generic_string()] = finalized.byte_size;
      }
      impl_->publish_metal_asset(finalized);
      return finalized;
    }
  }

  const auto payload = std::make_shared<std::vector<std::uint8_t>>(std::move(finalized.payload_bytes));
  finalized.byte_size = payload->size();
  impl_->asset_payload_bytes_seen += payload->size();
	  if (moved_payload) {
	    ++impl_->asset_payload_move_registrations;
	    impl_->asset_payload_move_bytes += payload->size();
	  }
	  if (impl_->inline_finalize) {
	    finalized.fast_fingerprint = effective_fast_fingerprint(finalized, *payload);
	  }
	  const auto fingerprint_key = impl_->inline_finalize
	      ? metal_asset_fingerprint_key(kind, finalized.kind, finalized.fast_fingerprint)
	      : std::string();
	  const bool foreground_dedup =
	      impl_->inline_finalize && payload->size() < impl_->foreground_asset_dedup_threshold;
	  if (!fingerprint_key.empty()) {
	    PendingAssetSnapshot cached_snapshot;
	    bool skipped_large_compare =
	        !foreground_dedup || payload->size() > impl_->exact_dedup_compare_threshold;
	    if (foreground_dedup) {
	      TimedTryWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
	      if (lock) {
	        cached_snapshot.prefilter_candidates =
	            cached_asset_prefilter_count(impl_->cached_metal_assets_by_fast_fingerprint, fingerprint_key);
	      } else {
	        impl_->foreground_dedup_lock_bypass_count.fetch_add(1, std::memory_order_relaxed);
	      }
	    }
	    if (skipped_large_compare)
	      ++impl_->asset_exact_dedup_skipped_large;
	    if (foreground_dedup &&
	        !skipped_large_compare &&
	        cached_snapshot.prefilter_candidates != 0 &&
	        impl_->completed_asset_cache_budget != 0 &&
	        impl_->completed_asset_cache_bytes != 0) {
	      std::uint64_t payload_hash = 0;
	      {
	        TimedWriterPhase phase(impl_->writer_stats ? &impl_->asset_register_phase_payload_hash : nullptr);
	        payload_hash = payload_dedup_hash(*payload);
	      }
	      TimedTryWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
	      if (lock) {
	        cached_snapshot = snapshot_pending_assets(
	            impl_->cached_metal_assets_by_fast_fingerprint,
	            fingerprint_key,
	            payload->size(),
	            impl_->exact_dedup_compare_threshold,
	            payload_hash);
	        impl_->asset_hash_prefilter_candidates += cached_snapshot.prefilter_candidates;
	        impl_->asset_hash_prefilter_matches += cached_snapshot.entries.size();
	      } else {
	        impl_->foreground_dedup_lock_bypass_count.fetch_add(1, std::memory_order_relaxed);
	      }
	    }
	    PendingAssetLookupResult cached;
	    if (foreground_dedup) {
	      cached = find_pending_asset(
	          cached_snapshot.entries,
	          *payload,
	          impl_->exact_dedup_compare_threshold);
	      if (cached.compared_bytes != 0) {
	        ++impl_->asset_exact_dedup_compares;
	        impl_->asset_exact_dedup_compare_bytes += cached.compared_bytes;
        }
      }
	    if (cached.asset) {
	      ++impl_->asset_completed_cache_hits;
	      finalized = alias_asset_record(std::move(finalized), *cached.asset);
	      impl_->publish_metal_asset(finalized);
	      return finalized;
	    }
	  }

	  if (finalized.content_hash.empty()) {
	    if (finalized.relative_path.empty()) {
	      finalized.relative_path = generated_asset_relative_path(
	          detail::metal_asset_directory_name(kind),
	          detail::metal_asset_extension(kind),
	          impl_->next_asset_id.fetch_add(1, std::memory_order_relaxed));
	    }
	    const auto target_path = impl_->layout.root_path / finalized.relative_path;
	    const auto content_key = std::string();
	    bool hash_only_if_final_exists = false;
	    if (!fingerprint_key.empty()) {
	      TimedWriterLock lock(impl_->fingerprint_hash_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
	      hash_only_if_final_exists =
	          impl_->metal_content_hash_by_fast_fingerprint.find(fingerprint_key) !=
	              impl_->metal_content_hash_by_fast_fingerprint.end();
	    }
		    if (impl_->inline_finalize &&
		        payload_may_reference_asset_alias(finalized.relative_path, *payload)) {
		      TimedTryWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
		      if (lock) {
		        impl_->asset_payloads_may_reference_alias.insert(finalized.relative_path.generic_string());
		      } else {
		        impl_->foreground_alias_lock_bypass_count.fetch_add(1, std::memory_order_relaxed);
		      }
		    }
	    if (impl_->asset_writer.enqueue(
	            target_path,
	            finalized.relative_path,
	            impl_->layout.root_path / detail::metal_asset_directory_name(kind),
	            std::filesystem::path(detail::metal_asset_directory_name(kind)),
	            detail::metal_asset_extension(kind),
	            payload,
	            content_key,
	            fingerprint_key,
		            finalized,
		            kind,
		            true,
		            hash_only_if_final_exists,
		            true,
		            [impl = impl_.get(), finalized]() {
		              impl->publish_queued_asset_for_async_write(finalized, true);
		            })) {
	      ++impl_->asset_async_enqueued;
	      if (hash_only_if_final_exists)
	        ++impl_->asset_async_hash_only_candidates;
	      finalized.payload_bytes.clear();
	      return finalized;
	    }
	    ++impl_->asset_async_queue_rejected;
	    ++impl_->asset_sync_hashes;
	    impl_->asset_sync_hash_bytes += payload->size();
	    finalized.content_hash = sha256_bytes(*payload);
	  }
  const auto content_key = metal_asset_content_key(kind, finalized.kind, finalized.content_hash);
  {
    TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
    if (const auto existing = impl_->metal_assets_by_content_hash.find(content_key);
        existing != impl_->metal_assets_by_content_hash.end()) {
      ++impl_->asset_hash_dedup_hits;
      finalized = alias_asset_record(std::move(finalized), existing->second);
      impl_->publish_metal_asset(finalized);
      return finalized;
    }
    if (!fingerprint_key.empty())
      impl_->metal_content_hash_by_fast_fingerprint[fingerprint_key] = finalized.content_hash;
  }
  if (finalized.relative_path.empty()) {
    finalized.relative_path = std::filesystem::path(detail::metal_asset_directory_name(kind)) /
                              (finalized.content_hash + detail::metal_asset_extension(kind));
  }

  if (payload->size() >= impl_->async_asset_threshold) {
    const auto target_path = impl_->layout.root_path / finalized.relative_path;
    if (impl_->asset_writer.enqueue(
            target_path,
            finalized.relative_path,
            std::filesystem::path(),
            std::filesystem::path(),
            std::string(),
            payload,
            content_key,
            fingerprint_key,
	            finalized,
		            kind,
		            true,
		            false,
		            false,
		            [impl = impl_.get(), finalized]() {
		              impl->publish_queued_asset_for_async_write(finalized, true);
		            })) {
      ++impl_->asset_async_enqueued;
      if (impl_->inline_finalize &&
          payload_may_reference_asset_alias(finalized.relative_path, *payload)) {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->asset_payloads_may_reference_alias.insert(finalized.relative_path.generic_string());
      }
      {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->metal_assets_by_content_hash.emplace(content_key, finalized);
      }
      return finalized;
    }
    ++impl_->asset_async_queue_rejected;
  }

  const auto target_path = impl_->layout.root_path / finalized.relative_path;
  std::filesystem::create_directories(target_path.parent_path());
  std::uint64_t existing_byte_size = 0;
  if (impl_->inline_finalize &&
      payload_may_reference_asset_alias(finalized.relative_path, *payload)) {
    TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
    impl_->asset_payloads_may_reference_alias.insert(finalized.relative_path.generic_string());
  }
  bool size_mismatch = false;
  if (!existing_file_matches_content_hash(
          target_path,
          finalized.content_hash,
          payload->size(),
          existing_byte_size,
          &size_mismatch)) {
    if (size_mismatch)
      ++impl_->asset_existing_hash_size_mismatches;
    ++impl_->asset_sync_writes;
    std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
    const auto sparse_result = write_payload_sparse(output, *payload);
    impl_->asset_sparse_zero_run_count += sparse_result.zero_run_count;
    impl_->asset_sparse_zero_bytes_skipped += sparse_result.zero_bytes_skipped;
  }

  impl_->publish_metal_asset(finalized);
  {
    TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
    impl_->metal_assets_by_content_hash.emplace(content_key, finalized);
    impl_->known_file_digests[finalized.relative_path.generic_string()] = finalized.content_hash;
    impl_->known_file_sizes[finalized.relative_path.generic_string()] = finalized.byte_size;
  }
  finalized.payload_bytes.clear();
  return finalized;
}

void TraceBundleWriter::write_object_index(const std::vector<ObjectRecord> &objects)
{
  impl_->objects = objects;
  if (!impl_->open) {
    return;
  }

  std::filesystem::create_directories(impl_->layout.object_index_path.parent_path());
  const auto json = object_index_json(objects);
  std::ofstream output(impl_->layout.object_index_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return;
  }
  output << json;
  output.flush();
  {
    const auto relative_path = bundle_relative_path(impl_->layout.object_index_path, impl_->layout.root_path);
    if (relative_path) {
      TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
      impl_->known_file_digests[relative_path->generic_string()] =
          content_hash_bytes(json.data(), json.size());
    }
  }
}

void TraceBundleWriter::declare_analysis_stream(std::string_view stream_name)
{
  if (stream_name.empty()) {
    return;
  }

  const auto already_declared = std::find_if(
      impl_->analysis_streams.begin(),
      impl_->analysis_streams.end(),
      [stream_name](const std::string &existing_stream_name) {
        return existing_stream_name == stream_name;
      }) != impl_->analysis_streams.end();
  if (!already_declared) {
    impl_->analysis_streams.emplace_back(stream_name);
  }
}

void TraceBundleWriter::append_analysis_record(const AnalysisRecord &record)
{
  impl_->analysis_records.push_back(record);
  append_analysis_line(
      record.stream_name,
      "{\"record_type\":\"" + json_escape(record.record_type) + "\",\"payload\":" +
          normalize_payload(record.payload) + "}");
}

void TraceBundleWriter::append_analysis_line(std::string_view stream_name, std::string_view json_line)
{
  if (!impl_->open || stream_name.empty()) {
    return;
  }

  declare_analysis_stream(stream_name);
  const auto stream_key = std::string(stream_name);
  auto &stream = impl_->analysis_stream_files[stream_key];
  if (!stream.is_open()) {
    stream.set_max_pending_bytes(impl_->async_line_max_pending_bytes);
    if (impl_->writer_stats)
      stream.set_lock_stats(&impl_->analysis_stream_lock_stats);
    const auto stream_path = analysis_path_for_stream(impl_->layout, stream_key);
    if (impl_->open_mode == TraceBundleOpenMode::Primary || !std::filesystem::is_regular_file(stream_path)) {
      stream.open(stream_path, std::ios::trunc);
    } else {
      stream.open(stream_path, std::ios::app);
    }
  }
  const auto line = std::string(json_line);
  if (line.find("asset-") != std::string::npos)
    impl_->analysis_streams_may_reference_asset_alias.insert(stream_key);
  stream.write_line(line);
}

void TraceBundleWriter::write_checksum_index(const ChecksumIndex &checksums)
{
  impl_->checksums = checksums;
}

void TraceBundleWriter::flush()
{
  if (!impl_ || !impl_->open) {
    return;
  }

  for (auto &entry : impl_->analysis_stream_files) {
    entry.second.flush();
  }
  if (impl_->callstream_stream.is_open()) {
    impl_->callstream_stream.flush();
  }
  if (impl_->metal_callstream_stream.is_open()) {
    impl_->metal_callstream_stream.flush();
  }
}

void TraceBundleWriter::checkpoint()
{
  if (!impl_ || !impl_->open) {
    return;
  }
  impl_->checkpoint_once();
}

void TraceBundleWriter::seal_checkpoint()
{
  if (!impl_ || !impl_->open) {
    return;
  }
  impl_->stop_checkpoint_thread();
  TimedWriterLock lock(impl_->event_mutex, impl_->writer_stats ? &impl_->event_lock_stats : nullptr);
  impl_->drain_async_work_for_checkpoint();
  impl_->checkpoint_once(true);
  const auto marker_name = impl_->open_mode == TraceBundleOpenMode::SidebandOnly
                               ? "seal-checkpoint-sideband.ready"
                               : "seal-checkpoint-primary.ready";
  write_text_atomic(impl_->layout.root_path / marker_name, "ready\n");
  impl_->open = false;
}

void TraceBundleWriter::close()
{
  if (!impl_->open) {
    return;
  }

  impl_->stop_checkpoint_thread();

  {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_analysis_flush : nullptr);
    for (auto &entry : impl_->analysis_stream_files) {
      if (entry.second.is_open()) {
        entry.second.flush();
        impl_->analysis_peak_pending_bytes = std::max<std::uint64_t>(
            impl_->analysis_peak_pending_bytes,
            entry.second.peak_pending_bytes());
        const auto stats = entry.second.stats();
        impl_->analysis_stream_stats.enqueue_count += stats.enqueue_count;
        impl_->analysis_stream_stats.enqueue_bytes += stats.enqueue_bytes;
        impl_->analysis_stream_stats.wait_count += stats.wait_count;
        impl_->analysis_stream_stats.wait_ns += stats.wait_ns;
        impl_->analysis_stream_stats.max_wait_ns =
            std::max(impl_->analysis_stream_stats.max_wait_ns, stats.max_wait_ns);
        impl_->analysis_stream_stats.write_count += stats.write_count;
        impl_->analysis_stream_stats.write_bytes += stats.write_bytes;
        impl_->analysis_stream_stats.write_ns += stats.write_ns;
        impl_->analysis_stream_stats.max_write_ns =
            std::max(impl_->analysis_stream_stats.max_write_ns, stats.max_write_ns);
        entry.second.close();
        const auto digest = entry.second.digest();
        if (!digest.empty()) {
          const auto relative_path = bundle_relative_path(
              analysis_path_for_stream(impl_->layout, entry.first),
              impl_->layout.root_path);
          if (relative_path) {
            TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
            impl_->known_file_digests[relative_path->generic_string()] = digest;
          }
        }
      }
    }
  }
  if (impl_->writer_stats) {
    const auto analysis_lock = snapshot_writer_lock_stats(impl_->analysis_stream_lock_stats);
    impl_->analysis_stream_stats.mutex_lock_count = analysis_lock.lock_count;
    impl_->analysis_stream_stats.mutex_wait_ns = analysis_lock.wait_ns;
    impl_->analysis_stream_stats.mutex_max_wait_ns = analysis_lock.max_wait_ns;
    impl_->analysis_stream_stats.mutex_hold_ns = analysis_lock.hold_ns;
    impl_->analysis_stream_stats.mutex_max_hold_ns = analysis_lock.max_hold_ns;
  }

  if (impl_->callstream_stream.is_open()) {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_callstream_flush : nullptr);
    {
      impl_->callstream_stream.flush();
      impl_->callstream_peak_pending_bytes = impl_->callstream_stream.peak_pending_bytes();
      impl_->callstream_stats = impl_->callstream_stream.stats();
      impl_->callstream_stream.close();
      const auto digest = impl_->callstream_stream.digest();
      if (!digest.empty()) {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->known_file_digests[kCallstreamFileName] = digest;
      }
    }
  }
  if (impl_->metal_callstream_stream.is_open()) {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_metal_callstream_flush : nullptr);
    {
      impl_->metal_callstream_stream.flush();
      impl_->metal_callstream_peak_pending_bytes = impl_->metal_callstream_stream.peak_pending_bytes();
      impl_->metal_callstream_stats = impl_->metal_callstream_stream.stats();
      impl_->metal_callstream_stream.close();
      const auto digest = impl_->metal_callstream_stream.digest();
      if (!digest.empty()) {
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->known_file_digests[kMetalCallstreamFileName] = digest;
      }
    }
  }
	  {
	    TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_asset_writer_drain : nullptr);
	    impl_->asset_writer.close();
	    impl_->drain_asset_completion_events();
	    impl_->asset_writer_stats = impl_->asset_writer.stats();
	  }
  const auto async_path_aliases = impl_->completed_asset_path_aliases();
  impl_->async_path_aliases = async_path_aliases.size();
  if (impl_->inline_finalize) {
    RewriteAssetReferenceResult rewrite_result;
    {
      TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_alias_rewrite : nullptr);
      rewrite_result = rewrite_bundle_asset_references(
          impl_->layout,
          async_path_aliases,
          impl_->rewrite_candidates());
    }
    impl_->asset_rewrite_candidates_scanned = rewrite_result.candidates_scanned;
    impl_->asset_rewrite_candidates_skipped_clean = rewrite_result.candidates_skipped_clean;
    impl_->asset_rewrite_replacements = rewrite_result.replacements;
    const auto &rewritten_paths = rewrite_result.rewritten_relative_paths;
    impl_->rewritten_asset_reference_files = rewritten_paths.size();
    if (!rewritten_paths.empty()) {
      TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
      for (const auto &relative_path : rewritten_paths) {
        const auto key = relative_path.generic_string();
        if (const auto digest = rewrite_result.rewritten_digests.find(key);
            digest != rewrite_result.rewritten_digests.end()) {
          impl_->known_file_digests[key] = digest->second;
          impl_->known_file_sizes.erase(key);
          ++impl_->asset_rewrite_digest_reuses;
        } else {
          impl_->known_file_digests.erase(key);
          impl_->known_file_sizes.erase(key);
        }
      }
    }
    for (const auto &entry : async_path_aliases) {
      if (entry.first != entry.second) {
        std::error_code remove_error;
        std::filesystem::remove(impl_->layout.root_path / entry.first, remove_error);
      }
    }
  }

  std::unordered_map<std::string, std::string> known_file_digests;
  std::unordered_map<std::string, std::uint64_t> known_file_sizes;
  {
    TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
    known_file_digests = impl_->known_file_digests;
    known_file_sizes = impl_->known_file_sizes;
  }
  if (impl_->open_mode == TraceBundleOpenMode::Primary && impl_->inline_finalize) {
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_asset_index : nullptr);
    std::vector<AssetRecord> assets;
    std::vector<AssetRecord> metal_assets;
    {
      TimedWriterLock lock(impl_->asset_record_mutex, impl_->writer_stats ? &impl_->asset_record_lock_stats : nullptr);
      assets = impl_->assets;
      metal_assets = impl_->metal_assets;
    }
    if (should_write_asset_index(impl_->layout.asset_index_path, assets, metal_assets)) {
      const auto blob_remap = merge_sideband_asset_shard(
          impl_->layout.analysis_directory_path / kSidebandAssetShardFileName,
          assets,
          metal_assets);
      if (const auto rewritten = rewrite_metal_callstream_blob_refs(impl_->layout, blob_remap)) {
        known_file_digests[kMetalCallstreamFileName] = rewritten->first;
        known_file_sizes[kMetalCallstreamFileName] = rewritten->second;
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->known_file_digests[kMetalCallstreamFileName] = rewritten->first;
        impl_->known_file_sizes[kMetalCallstreamFileName] = rewritten->second;
      }
      const auto asset_index = asset_index_json(
          assets,
          metal_assets,
          impl_->layout.root_path,
          async_path_aliases,
          known_file_digests,
          std::move(known_file_sizes));
      {
        std::ofstream asset_index_output(impl_->layout.asset_index_path, std::ios::binary | std::ios::trunc);
        asset_index_output << asset_index;
        asset_index_output.close();
        TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
        impl_->known_file_digests[kAssetIndexFileName] = content_hash_bytes(asset_index.data(), asset_index.size());
        known_file_digests[kAssetIndexFileName] = impl_->known_file_digests[kAssetIndexFileName];
      }
    }
  }

  ChecksumIndex checksums = impl_->checksums;
  if (impl_->writer_stats) {
    impl_->snapshot_async_writer_stats();
    TimedWriterPhase phase(impl_->writer_stats ? &impl_->close_phase_writer_stats : nullptr);
    const auto stats_relative_path = std::filesystem::path(kAnalysisDirectoryName) / "writer-stats.jsonl";
    auto stats_relative_paths = collect_bundle_relative_paths_full_scan(impl_->layout);
    std::unordered_set<std::string> stats_seen;
    for (const auto &relative_path : stats_relative_paths) {
      stats_seen.insert(relative_path.generic_string());
    }
    add_checksum_candidate(stats_relative_paths, stats_seen, stats_relative_path);
    std::sort(stats_relative_paths.begin(), stats_relative_paths.end());
    stats_relative_paths.erase(std::unique(stats_relative_paths.begin(), stats_relative_paths.end()), stats_relative_paths.end());
    std::string stats_digest;
    const auto stats_json = impl_->writer_stats_json(stats_relative_paths.size());
    const auto stats_path = impl_->write_writer_stats_analysis(stats_json, stats_digest);
    if (!stats_path.empty()) {
      const auto stats_key = stats_relative_path.generic_string();
      known_file_digests[stats_key] = stats_digest;
      TimedWriterLock lock(impl_->asset_mutex, impl_->writer_stats ? &impl_->asset_lock_stats : nullptr);
      impl_->known_file_digests[stats_key] = stats_digest;
    }
    std::cerr << "apitrace-writer-stats " << stats_json << "\n";
  }

  if (!impl_->inline_finalize || impl_->open_mode == TraceBundleOpenMode::SidebandOnly) {
    impl_->open = false;
    return;
  }

  TimedWriterPhase checksum_phase(impl_->writer_stats ? &impl_->close_phase_checksum : nullptr);
  const auto relative_paths = collect_bundle_relative_paths_full_scan(impl_->layout);

  std::unordered_map<std::string, ChecksumRecord> existing_records;
  existing_records.reserve(checksums.files.size());
  for (const auto &record : checksums.files) {
    existing_records[record.relative_path.generic_string()] = record;
  }

  checksums.files.clear();
  checksums.format_version = impl_->metadata.format_version;
  for (const auto &relative_path : relative_paths) {
    const auto absolute_path = impl_->layout.root_path / relative_path;
    if (!std::filesystem::exists(absolute_path) || std::filesystem::is_directory(absolute_path)) {
      continue;
    }
    const auto key = relative_path.generic_string();
    ChecksumRecord file_record;
    file_record.relative_path = relative_path;
    file_record.byte_size = regular_file_size_or_zero(absolute_path);
    file_record.has_byte_size = true;
    if (const auto known_digest = known_file_digests.find(key); known_digest != known_file_digests.end()) {
      file_record.digest = known_digest->second;
    } else {
      file_record.digest = sha256_file(absolute_path);
    }
    checksums.files.push_back(file_record);
  }
  checksums.files = unique_checksum_records(checksums.files);
  checksums.bundle_hash = bundle_hash_from_records(checksums.files);

  std::ofstream checksum_output(impl_->layout.checksums_path, std::ios::binary | std::ios::trunc);
  checksum_output << checksum_index_json(checksums);
  checksum_output.close();

  impl_->checksums = checksums;
  impl_->open = false;
}

bool TraceBundleWriter::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const BundleLayout &TraceBundleWriter::layout() const noexcept
{
  return impl_->layout;
}

} // namespace apitrace::trace
