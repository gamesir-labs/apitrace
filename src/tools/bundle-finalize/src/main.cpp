#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"
#include "apitrace/d3d12_replay.hpp"
#include "apitrace/raw_event_codec.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/tools/cli_entries.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char *kSidebandAssetIndexPath = "analysis/sideband-assets.json";
constexpr const char *kMaxTruncateFramesEnv = "DXMT_FINALIZE_MAX_TRUNCATE_FRAMES";
constexpr std::uint64_t kDefaultMaxTruncateFrames = 120;
constexpr std::size_t kFileCopyBufferSize = 8ull * 1024ull * 1024ull;
constexpr const char *kEmptySha256Digest =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::size_t default_job_count()
{
  return static_cast<std::size_t>(std::thread::hardware_concurrency()) + 2;
}

enum class AssetSource {
  Primary,
  Sideband,
};

struct Options {
  fs::path bundle_root;
  std::size_t jobs = default_job_count();
  bool dry_run = false;
  bool raw_format = false;
  bool keep_duplicates = false;
  bool profile = false;
  bool progress = false;
  bool verify_existing_canonical = false;
  bool verify_jsonl_records = false;
  bool persist_replay_model_only = false;
  std::uint64_t max_truncate_frames = kDefaultMaxTruncateFrames;
};

struct AssetEntry {
  std::uint64_t blob_id = 0;
  std::string path;
  std::string kind = "Unknown";
  std::string debug_name;
  std::string content_hash;
  std::string fast_fingerprint;
  std::uint64_t byte_size = 0;
  std::uint64_t actual_size = 0;
  std::string payload_path;
  std::uint64_t payload_offset = 0;
  bool metal = false;
  bool binary_payload = true;
  AssetSource source = AssetSource::Primary;
  bool file_exists = false;
  bool original_file_exists = false;
  bool payload_slice_exists = false;
  bool safe_path = false;
  bool safe_payload_path = false;
  std::string digest;
  std::string canonical_path;
};

struct ShaderAssetRef {
  std::uint64_t blob_id = 0;
  std::uint64_t byte_size = 0;
  std::string path;
};

struct IncompleteSpoolRef {
  std::uint64_t blob_id = 0;
  std::uint64_t sequence = 0;
  std::string path;
};

struct Stats {
  std::size_t input_assets = 0;
  std::size_t indexed_assets = 0;
  std::size_t hashed_assets = 0;
  std::size_t rewritten_assets = 0;
  std::size_t duplicate_assets = 0;
  std::size_t removed_files = 0;
  std::size_t rewritten_text_files = 0;
  std::size_t remapped_blob_ids = 0;
  std::size_t rewritten_blob_ref_files = 0;
  std::size_t rewritten_primary_blob_ref_records = 0;
  std::size_t preserved_referenced_missing_assets = 0;
  std::size_t sanitized_jsonl_files = 0;
  std::size_t dropped_jsonl_lines = 0;
  std::size_t truncated_inconsistent_jsonl_files = 0;
  std::uint64_t truncated_inconsistent_jsonl_bytes = 0;
  std::uint64_t truncated_inconsistent_frames = 0;
  std::uint64_t surviving_frame_count = 0;
  std::size_t dropped_unreferenced_truncated_assets = 0;
  std::size_t refreshed_asset_hashes = 0;
  std::size_t checksum_files = 0;
  std::size_t checksum_hashed_files = 0;
  std::size_t hashed_unique_files = 0;
  std::size_t primary_blob_id_conflicts = 0;
  std::size_t restored_inline_query_assets = 0;
  std::size_t recovered_spooled_assets = 0;
  std::uint64_t recovered_spooled_asset_bytes = 0;
  std::size_t repaired_missing_device_objects = 0;
  std::size_t rebuilt_d3d12_pipeline_assets = 0;
  std::size_t incomplete_d3d12_pipeline_semantics = 0;
  bool d3d12_replay_model_written = false;
  std::uint64_t d3d12_replay_model_json_bytes = 0;
  std::uint64_t d3d12_replay_model_blob_bytes = 0;
  std::size_t preserved_referenced_duplicate_assets = 0;
  std::size_t dropped_unreferenced_missing_assets = 0;
  std::size_t removed_orphan_asset_files = 0;
  std::uint64_t hashed_asset_bytes = 0;
  std::uint64_t refreshed_asset_bytes = 0;
  std::uint64_t checksum_hashed_bytes = 0;
  std::uint64_t duplicate_bytes = 0;
  std::size_t repaired_d3d12_pipeline_events = 0;
  std::size_t unresolved_d3d12_pipeline_asset_refs = 0;
  std::uint64_t jsonl_records = 0;
  std::uint64_t input_bytes = 0;
  std::uint64_t output_bytes = 0;
  std::uint64_t rewritten_records = 0;
  std::uint64_t jsonl_passes = 0;
  std::uint64_t digest_cache_hits = 0;
  std::uint64_t digest_cache_misses = 0;
  std::uint64_t pipeline_cache_hits = 0;
  std::uint64_t pipeline_cache_misses = 0;
  std::uint64_t referenced_paths_collected = 0;
  std::uint64_t rewritten_digest_files = 0;
  std::uint64_t checksum_reused_rewritten_files = 0;
  std::uint64_t checksum_reused_prior_files = 0;
  std::uint64_t checksum_reused_prior_bytes = 0;
  std::uint64_t asset_hash_reused_files = 0;
  std::uint64_t asset_hash_reused_bytes = 0;
  std::size_t spooled_assets = 0;
  std::size_t materialized_spooled_assets = 0;
  std::size_t removed_spool_files = 0;
  std::size_t reused_existing_canonical_files = 0;
  std::uint64_t reused_existing_canonical_bytes = 0;
  std::size_t verified_existing_canonical_files = 0;
  std::uint64_t verified_existing_canonical_bytes = 0;
  std::size_t removed_stale_canonical_files = 0;
  std::uint64_t spooled_asset_bytes = 0;
  std::size_t recovered_unindexed_asset_aliases = 0;
  std::uint64_t sequence_regression_segments = 0;
  std::uint64_t remapped_sequence_records = 0;
  std::size_t raw_to_final_events = 0;
  std::size_t raw_to_final_assets = 0;
  std::uint64_t raw_to_final_asset_bytes = 0;
};

std::unordered_map<std::uint64_t, std::string> build_effective_path_by_blob_id(const std::vector<AssetEntry> &assets);

class FileDigestCache {
public:
  std::string digest_file(const fs::path &path)
  {
    const auto key = cache_key(path);
    const auto metadata = read_metadata(path);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = entries_.find(key);
      if (found != entries_.end() && found->second.size == metadata.size &&
          found->second.mtime == metadata.mtime) {
        hits_.fetch_add(1, std::memory_order_relaxed);
        return found->second.digest;
      }
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    const auto digest = apitrace::trace::content_hash_file(path);
    remember(path, digest, metadata.size);
    return digest;
  }

  void remember(const fs::path &path, const std::string &digest, std::uint64_t size)
  {
    if (digest.empty()) {
      return;
    }
    Entry entry;
    entry.digest = digest;
    entry.size = size;
    entry.mtime = read_mtime(path);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[cache_key(path)] = std::move(entry);
  }

  std::uint64_t hits() const noexcept
  {
    return hits_.load(std::memory_order_relaxed);
  }

  std::uint64_t misses() const noexcept
  {
    return misses_.load(std::memory_order_relaxed);
  }

private:
  struct Metadata {
    std::uint64_t size = 0;
    fs::file_time_type mtime{};
  };

  struct Entry {
    std::string digest;
    std::uint64_t size = 0;
    fs::file_time_type mtime{};
  };

  static std::string cache_key(const fs::path &path)
  {
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    return (error ? path : absolute).lexically_normal().generic_string();
  }

  static fs::file_time_type read_mtime(const fs::path &path)
  {
    std::error_code error;
    const auto mtime = fs::last_write_time(path, error);
    return error ? fs::file_time_type{} : mtime;
  }

  static Metadata read_metadata(const fs::path &path)
  {
    Metadata metadata;
    std::error_code error;
    const auto size = fs::file_size(path, error);
    metadata.size = error ? 0 : static_cast<std::uint64_t>(size);
    metadata.mtime = read_mtime(path);
    return metadata;
  }

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::atomic_uint64_t hits_{0};
  std::atomic_uint64_t misses_{0};
};

class FileCopyScratch {
public:
  FileCopyScratch()
      : buffer_(kFileCopyBufferSize)
  {
  }

  char *data()
  {
    return buffer_.data();
  }

  const char *data() const
  {
    return buffer_.data();
  }

  std::size_t size() const
  {
    return buffer_.size();
  }

private:
  std::vector<char> buffer_;
};

bool copy_file_range(
    const fs::path &source,
    std::uint64_t offset,
    std::uint64_t byte_size,
    const fs::path &destination,
    std::string &error_message,
    FileCopyScratch &scratch)
{
  std::ifstream input(source, std::ios::binary);
  if (!input.is_open()) {
    error_message = "failed to open payload slice source";
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input.good()) {
    error_message = "failed to seek payload slice source";
    return false;
  }
  fs::create_directories(destination.parent_path());
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    error_message = "failed to open payload slice destination";
    return false;
  }

  std::uint64_t remaining = byte_size;
  while (remaining != 0) {
    const auto chunk_size = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(scratch.size())));
    input.read(scratch.data(), chunk_size);
    const auto read_count = static_cast<std::size_t>(input.gcount());
    if (read_count == 0) {
      error_message = "short payload slice read";
      return false;
    }
    output.write(scratch.data(), static_cast<std::streamsize>(read_count));
    if (!output.good()) {
      error_message = "failed to write payload slice destination";
      return false;
    }
    remaining -= read_count;
  }
  return true;
}

bool stderr_is_tty()
{
#ifdef _WIN32
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

bool ci_environment()
{
  const char *ci = std::getenv("CI");
  return ci && *ci && std::string(ci) != "0";
}

std::uint64_t max_truncate_frames_from_env()
{
  const char *value = std::getenv(kMaxTruncateFramesEnv);
  if (!value || !*value) {
    return kDefaultMaxTruncateFrames;
  }
  char *end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || (end && *end != '\0')) {
    std::cerr << "warning: ignoring invalid " << kMaxTruncateFramesEnv
              << "=" << value << "; using " << kDefaultMaxTruncateFrames << "\n";
    return kDefaultMaxTruncateFrames;
  }
  return static_cast<std::uint64_t>(parsed);
}

std::string format_bytes(std::uint64_t bytes)
{
  const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream out;
  if (unit == 0) {
    out << bytes << units[unit];
  } else {
    out << std::fixed << std::setprecision(1) << value << units[unit];
  }
  return out.str();
}

class ProgressReporter {
public:
  explicit ProgressReporter(bool enabled) : enabled_(enabled) {}

  void begin_stage(std::size_t stage_index, std::size_t stage_count, const char *name)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    stage_started_ = std::chrono::steady_clock::now();
    last_emit_ = {};
    stage_index_ = stage_index;
    stage_count_ = stage_count;
    stage_name_ = name ? name : "";
    emit_locked("started", 0, 0, 0, 0, true);
  }

  void update(std::uint64_t items_done, std::uint64_t item_count, std::uint64_t bytes_done, std::uint64_t byte_count)
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (last_emit_.time_since_epoch().count() != 0 &&
        now - last_emit_ < std::chrono::milliseconds(500) &&
        items_done < item_count) {
      return;
    }
    emit_locked("running", items_done, item_count, bytes_done, byte_count, false);
  }

  void end_stage()
  {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    emit_locked("done", 0, 0, 0, 0, true);
    std::cerr << "\n";
    line_active_ = false;
  }

  void clear_line()
  {
    if (!enabled_ || !line_active_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::cerr << "\r" << std::string(last_line_width_, ' ') << "\r";
    line_active_ = false;
  }

private:
  void emit_locked(
      const char *state,
      std::uint64_t items_done,
      std::uint64_t item_count,
      std::uint64_t bytes_done,
      std::uint64_t byte_count,
      bool force)
  {
    const auto now = std::chrono::steady_clock::now();
    if (!force &&
        last_emit_.time_since_epoch().count() != 0 &&
        now - last_emit_ < std::chrono::milliseconds(500)) {
      return;
    }
    last_emit_ = now;
    const auto elapsed = std::chrono::duration<double>(now - stage_started_).count();
    std::ostringstream line;
    line << "bundle-finalize progress"
         << " stage=" << stage_index_ << "/" << stage_count_
         << " name=" << stage_name_
         << " state=" << state
         << " elapsed_s=" << std::fixed << std::setprecision(1) << elapsed;
    if (item_count != 0) {
      line << " items=" << items_done << "/" << item_count;
    }
    if (byte_count != 0) {
      line << " bytes=" << format_bytes(bytes_done) << "/" << format_bytes(byte_count);
    }
    if (items_done != 0 && item_count != 0 && items_done < item_count && elapsed > 0.0) {
      const auto rate = static_cast<double>(items_done) / elapsed;
      if (rate > 0.0) {
        const auto eta = static_cast<double>(item_count - items_done) / rate;
        line << " eta_s=" << std::fixed << std::setprecision(1) << eta;
      }
    }

    const auto text = line.str();
    std::cerr << "\r" << text;
    if (last_line_width_ > text.size()) {
      std::cerr << std::string(last_line_width_ - text.size(), ' ');
    }
    last_line_width_ = text.size();
    line_active_ = true;
  }

  bool enabled_ = false;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point stage_started_{};
  std::chrono::steady_clock::time_point last_emit_{};
  std::size_t stage_index_ = 0;
  std::size_t stage_count_ = 0;
  std::size_t last_line_width_ = 0;
  std::string stage_name_;
  bool line_active_ = false;
};

void print_usage(const char *argv0)
{
  std::cerr
      << "Usage: " << argv0 << " [--dry-run] [--keep-duplicates] [--jobs N] [--no-progress] <trace-bundle>\n"
      << "\n"
      << "Finalizes a captured bundle offline: merges sideband assets, deduplicates assets,\n"
      << "rewrites text references, writes assets.json, and refreshes checksums.json.\n"
      << "\n"
      << "Options:\n"
      << "  --profile          Print per-stage timing and throughput to stderr.\n"
      << "  --raw-format       Materialize raw/events.bin + raw/blobs.bin before normal finalization.\n"
      << "  --progress         Force interactive progress on stderr.\n"
      << "  --no-progress      Disable interactive progress on stderr.\n"
      << "  --verify-existing-canonical\n"
      << "                     Re-hash existing canonical asset files before reusing them.\n"
      << "  --verify-jsonl-records\n"
      << "                     Parse every JSONL record during reference rewriting.\n"
      << "  --tolerate-missing-blobs\n"
      << "                     Deprecated compatibility alias; missing blobs are truncated, never zero-filled.\n"
      << "  --persist-replay-model-only\n"
      << "                     Only rebuild the D3D12 replay model and update its checksums.\n";
}

std::string replace_all_copy(std::string text, std::string_view from, std::string_view to)
{
  if (from.empty()) {
    return text;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to.data(), to.size());
    pos += to.size();
  }
  return text;
}

bool materialize_raw_capture_to_final_bundle(const Options &options, Stats &stats)
{
  if (options.dry_run) {
    std::cerr << "error: --raw-format cannot be combined with --dry-run before materialization exists\n";
    return false;
  }

  apitrace::trace::raw::RawCaptureReader reader;
  if (!reader.open(options.bundle_root)) {
    std::cerr << "error: failed to open raw capture: " << reader.last_error() << "\n";
    return false;
  }
  const auto raw_events = reader.read_events();
  const auto decoded = apitrace::trace::raw::decode_raw_events(reader, raw_events);
  if (!decoded.error.empty()) {
    std::cerr << "error: failed to decode raw capture: " << decoded.error << "\n";
    return false;
  }

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(options.bundle_root)) {
    std::cerr << "error: failed to open final bundle writer for raw materialization\n";
    return false;
  }
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, apitrace::trace::kFormatVersion, "raw-to-final", false});

  for (const auto &decoded_event : decoded.events) {
    if (!decoded_event.passthrough_jsonl_record.empty()) {
      writer.append_callstream_json_line(decoded_event.passthrough_jsonl_record);
      ++stats.raw_to_final_events;
      continue;
    }
    auto event = decoded_event.event;
    for (const auto &asset : decoded_event.assets) {
      const auto input_blob_id = asset.blob_id;
      const auto input_relative_path = asset.relative_path.generic_string();
      auto registered = writer.register_asset(asset);
      ++stats.raw_to_final_assets;
      stats.raw_to_final_asset_bytes += registered.byte_size;
      if (registered.blob_id != input_blob_id) {
        for (auto &blob_id : event.blob_refs) {
          if (blob_id == input_blob_id) {
            blob_id = registered.blob_id;
          }
        }
      }
      const auto registered_relative_path = registered.relative_path.generic_string();
      if (!input_relative_path.empty() && input_relative_path != registered_relative_path) {
        event.payload = replace_all_copy(event.payload, input_relative_path, registered_relative_path);
      }
    }
    writer.append_call_event(std::move(event));
    ++stats.raw_to_final_events;
  }
  writer.close();
  return true;
}

std::optional<Options> parse_args(int argc, char **argv)
{
  Options options;
  options.progress = stderr_is_tty() && !ci_environment();
  options.max_truncate_frames = max_truncate_frames_from_env();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--raw-format") {
      options.raw_format = true;
    } else if (arg == "--keep-duplicates") {
      options.keep_duplicates = true;
    } else if (arg == "--profile") {
      options.profile = true;
    } else if (arg == "--progress") {
      options.progress = true;
    } else if (arg == "--no-progress") {
      options.progress = false;
    } else if (arg == "--verify-existing-canonical") {
      options.verify_existing_canonical = true;
    } else if (arg == "--verify-jsonl-records") {
      options.verify_jsonl_records = true;
    } else if (arg == "--tolerate-missing-blobs") {
      std::cerr << "warning: --tolerate-missing-blobs is deprecated; "
                << "bundle-finalize now truncates incomplete tails and never zero-fills missing blobs\n";
    } else if (arg == "--persist-replay-model-only") {
      options.persist_replay_model_only = true;
    } else if (arg == "--jobs") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return std::nullopt;
      }
      const auto jobs = std::stoull(argv[++i]);
      options.jobs = static_cast<std::size_t>(std::max<std::uint64_t>(1, jobs));
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return std::nullopt;
    } else if (options.bundle_root.empty()) {
      options.bundle_root = fs::path(arg);
    } else {
      print_usage(argv[0]);
      return std::nullopt;
    }
  }

  if (options.bundle_root.empty()) {
    print_usage(argv[0]);
    return std::nullopt;
  }
  return options;
}

double elapsed_seconds(std::chrono::steady_clock::time_point started)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

template <typename Func>
void run_stage(
    const Options &options,
    ProgressReporter &progress,
    std::size_t &stage_index,
    std::size_t stage_count,
    const char *name,
    Func &&func)
{
  const auto started = std::chrono::steady_clock::now();
  progress.begin_stage(++stage_index, stage_count, name);
  func();
  progress.end_stage();
  if (options.profile) {
    std::cerr << "bundle-finalize-profile stage=" << name
              << " elapsed_s=" << elapsed_seconds(started) << "\n";
  }
}

bool safe_relative_path(const fs::path &path)
{
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  for (const auto &part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

std::uint8_t hex_digit_value(char digit)
{
  if (digit >= '0' && digit <= '9') {
    return static_cast<std::uint8_t>(digit - '0');
  }
  if (digit >= 'a' && digit <= 'f') {
    return static_cast<std::uint8_t>(digit - 'a' + 10);
  }
  if (digit >= 'A' && digit <= 'F') {
    return static_cast<std::uint8_t>(digit - 'A' + 10);
  }
  return 0xff;
}

bool decode_hex_bytes(
    const std::string &encoded,
    std::uint64_t expected_size,
    std::vector<std::uint8_t> &bytes)
{
  if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 2) ||
      encoded.size() != static_cast<std::size_t>(expected_size) * 2) {
    return false;
  }
  bytes.assign(static_cast<std::size_t>(expected_size), 0);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto high = hex_digit_value(encoded[index * 2]);
    const auto low = hex_digit_value(encoded[index * 2 + 1]);
    if (high > 0xf || low > 0xf) {
      return false;
    }
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

std::string asset_extension(const std::string &kind)
{
  if (kind == "ShaderDxbc") {
    return ".dxbc";
  }
  if (kind == "ShaderDxil") {
    return ".dxil";
  }
  if (kind == "RootSignature") {
    return ".rootsig";
  }
  if (kind == "Texture") {
    return ".texture";
  }
  if (kind == "Buffer") {
    return ".buffer";
  }
  if (kind == "Pipeline") {
    return ".pipeline.json";
  }
  if (kind == "ObjectIndex") {
    return ".json";
  }
  if (kind == "Analysis") {
    return ".jsonl";
  }
  return ".bin";
}

std::string asset_directory_name(const std::string &kind)
{
  if (kind == "ShaderDxbc" || kind == "ShaderDxil" || kind == "RootSignature") {
    return "shaders";
  }
  if (kind == "Texture") {
    return "textures";
  }
  if (kind == "Buffer") {
    return "buffers";
  }
  if (kind == "Pipeline") {
    return "pipelines";
  }
  if (kind == "ObjectIndex") {
    return "objects";
  }
  if (kind == "Analysis") {
    return apitrace::trace::kAnalysisDirectoryName;
  }
  return ".";
}

std::string asset_kind_from_path(const fs::path &path)
{
  const auto generic = path.generic_string();
  if (generic.rfind("shaders/", 0) == 0) {
    const auto extension = path.extension().string();
    if (extension == ".rootsig") {
      return "RootSignature";
    }
    if (extension == ".dxbc") {
      return "ShaderDxbc";
    }
    if (extension == ".dxil") {
      return "ShaderDxil";
    }
    return "Shader";
  }
  if (generic.rfind("textures/", 0) == 0) {
    return "Texture";
  }
  if (generic.rfind("buffers/", 0) == 0) {
    return "Buffer";
  }
  if (generic.rfind("pipelines/", 0) == 0) {
    return "Pipeline";
  }
  if (generic.rfind("metal/libraries/", 0) == 0) {
    return "Unknown";
  }
  if (generic.rfind("metal/pipelines/", 0) == 0) {
    return "Pipeline";
  }
  if (generic.rfind("metal/textures/", 0) == 0) {
    return "Texture";
  }
  if (generic.rfind("metal/buffers/", 0) == 0) {
    return "Buffer";
  }
  if (generic.rfind("objects/", 0) == 0) {
    return "ObjectIndex";
  }
  if (generic.rfind(std::string(apitrace::trace::kAnalysisDirectoryName) + "/", 0) == 0) {
    return "Analysis";
  }
  return "Unknown";
}

std::string canonical_asset_path(const AssetEntry &asset)
{
  if (asset.metal && asset.kind == "Pipeline") {
    return (fs::path("metal") / "pipelines" / (asset.digest + ".pipeline.json")).generic_string();
  }
  if (asset.metal && asset.kind == "Unknown") {
    return (fs::path("metal") / "libraries" / (asset.digest + ".metallib")).generic_string();
  }
  const auto directory = asset_directory_name(asset.kind);
  const auto filename = asset.digest + asset_extension(asset.kind);
  if (directory == ".") {
    return filename;
  }
  return (fs::path(directory) / filename).generic_string();
}

bool is_path_reference_key(const std::string &key)
{
  return (key.size() >= 5 && key.compare(key.size() - 5, 5, "_path") == 0) || key == "path";
}

void collect_asset_references_from_json(
    const json &node,
    std::vector<std::uint64_t> &blob_refs,
    std::vector<std::string> &paths)
{
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it.key() == "blob_refs" && it.value().is_array()) {
        for (const auto &ref : it.value()) {
          if (ref.is_number_unsigned()) {
            blob_refs.push_back(ref.get<std::uint64_t>());
          }
        }
        continue;
      }
      if (is_path_reference_key(it.key()) && it.value().is_string()) {
        paths.push_back(it.value().get<std::string>());
      }
      collect_asset_references_from_json(it.value(), blob_refs, paths);
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      collect_asset_references_from_json(item, blob_refs, paths);
    }
  }
}

bool is_canonical_jsonl_asset_path_value(std::string_view path)
{
  return path.rfind("shaders/asset-", 0) == 0 ||
         path.rfind("pipelines/asset-", 0) == 0 ||
         path.rfind("metal/libraries/asset-", 0) == 0 ||
         path.rfind("metal/pipelines/asset-", 0) == 0;
}

bool jsonl_line_may_contain_rewritable_asset_path(std::string_view line)
{
  std::size_t search = 0;
  while ((search = line.find("path\"", search)) != std::string::npos) {
    const auto colon = line.find(':', search + 5);
    if (colon == std::string::npos) {
      return false;
    }
    const auto value_begin = line.find('"', colon + 1);
    if (value_begin == std::string::npos) {
      search = colon + 1;
      continue;
    }
    const auto value_end = line.find('"', value_begin + 1);
    if (value_end == std::string::npos) {
      return false;
    }
    const auto value = line.substr(value_begin + 1, value_end - value_begin - 1);
    if (!value.empty() && !is_canonical_jsonl_asset_path_value(value)) {
      return true;
    }
    search = value_end + 1;
  }
  return false;
}

bool text_may_contain_rewritable_asset_path(std::string_view text)
{
  return jsonl_line_may_contain_rewritable_asset_path(text) ||
         text.find("_path\"") != std::string::npos ||
         text.find("\"path\"") != std::string::npos;
}

void collect_pipeline_dependency_paths(const json &node, std::vector<std::string> &paths)
{
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it.key().size() >= 5 &&
          it.key().compare(it.key().size() - 5, 5, "_path") == 0 &&
          it.value().is_string()) {
        const auto path = it.value().get<std::string>();
        if (path.rfind("shaders/", 0) == 0) {
          paths.push_back(path);
        }
      }
      collect_pipeline_dependency_paths(it.value(), paths);
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      collect_pipeline_dependency_paths(item, paths);
    }
  }
}

std::optional<AssetEntry> make_discovered_asset(
    const fs::path &bundle_root,
    std::uint64_t blob_id,
    const std::string &path)
{
  if (blob_id == 0) {
    return std::nullopt;
  }
  const fs::path asset_path(path);
  if (!safe_relative_path(asset_path) ||
      !fs::is_regular_file(bundle_root / asset_path)) {
    return std::nullopt;
  }
  AssetEntry asset;
  asset.blob_id = blob_id;
  asset.path = asset_path.generic_string();
  asset.kind = asset_kind_from_path(asset_path);
  asset.metal = asset.path.rfind("metal/", 0) == 0;
  asset.source = AssetSource::Primary;
  return asset;
}

void add_discovered_asset(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    Stats &stats,
    std::unordered_set<std::uint64_t> &indexed_blob_ids,
    std::uint64_t blob_id,
    const std::string &path)
{
  if (indexed_blob_ids.find(blob_id) != indexed_blob_ids.end()) {
    return;
  }
  auto asset = make_discovered_asset(bundle_root, blob_id, path);
  if (!asset) {
    return;
  }
  assets.push_back(std::move(*asset));
  indexed_blob_ids.insert(blob_id);
  ++stats.input_assets;
}

std::uint64_t committed_spool_end(const std::vector<AssetEntry> &assets, const std::string &payload_path)
{
  std::uint64_t end = 0;
  for (const auto &asset : assets) {
    if (asset.payload_path != payload_path || asset.byte_size == 0) {
      continue;
    }
    end = std::max(end, asset.payload_offset + asset.byte_size);
  }
  return end;
}

std::optional<AssetEntry> recover_spooled_unmap_asset(
    const fs::path &bundle_root,
    std::uint64_t blob_id,
    const std::string &path,
    std::uint64_t byte_size,
    const std::string &payload_path,
    std::uint64_t payload_offset,
    bool *spool_incomplete)
{
  if (spool_incomplete) {
    *spool_incomplete = false;
  }
  if (blob_id == 0 || byte_size == 0) {
    return std::nullopt;
  }
  const fs::path asset_path(path);
  const fs::path payload_relative(payload_path);
  if (!safe_relative_path(asset_path) || !safe_relative_path(payload_relative)) {
    return std::nullopt;
  }
  const auto payload_absolute = bundle_root / payload_relative;
  std::error_code error;
  if (!fs::is_regular_file(payload_absolute, error) || error) {
    return std::nullopt;
  }
  const auto payload_size = fs::file_size(payload_absolute, error);
  if (error || payload_offset > payload_size || byte_size > payload_size - payload_offset) {
    if (!error && spool_incomplete) {
      *spool_incomplete = true;
    }
    return std::nullopt;
  }

  AssetEntry asset;
  asset.blob_id = blob_id;
  asset.path = asset_path.generic_string();
  asset.kind = "Buffer";
  asset.debug_name = "d3d12-resource-unmap";
  asset.byte_size = byte_size;
  asset.actual_size = byte_size;
  asset.payload_path = payload_relative.generic_string();
  asset.payload_offset = payload_offset;
  asset.payload_slice_exists = true;
  asset.safe_payload_path = true;
  asset.safe_path = true;
  asset.binary_payload = true;
  asset.source = AssetSource::Primary;
  return asset;
}

std::optional<std::string> raw_asset_path_for_blob_id(
    const fs::path &bundle_root,
    std::uint64_t blob_id,
    std::initializer_list<const char *> extensions)
{
  std::ostringstream stem;
  stem << "asset-" << std::hex << std::setw(16) << std::setfill('0') << blob_id;
  for (const auto *extension : extensions) {
    const auto relative = fs::path("shaders") / (stem.str() + extension);
    if (fs::is_regular_file(bundle_root / relative)) {
      return relative.generic_string();
    }
  }
  return std::nullopt;
}

bool is_d3d12_pipeline_create_function(const std::string &function)
{
  return function == "ID3D12Device::CreateGraphicsPipelineState" ||
         function == "CreateGraphicsPipelineState" ||
         function == "ID3D12Device::CreateComputePipelineState" ||
         function == "CreateComputePipelineState" ||
         function == "ID3D12Device2::CreatePipelineState" ||
         function == "CreatePipelineState";
}

void discover_raw_asset_references_from_jsonl(
    const fs::path &bundle_root,
    const fs::path &relative_path,
    std::vector<AssetEntry> &assets,
    std::unordered_set<std::uint64_t> &indexed_blob_ids,
    bool legacy_asset_discovery,
    IncompleteSpoolRef *incomplete_spool_ref,
    Stats &stats)
{
  const auto absolute_path = bundle_root / relative_path;
  if (!fs::is_regular_file(absolute_path)) {
    return;
  }

  std::ifstream input(absolute_path, std::ios::binary);
  ++stats.jsonl_passes;
  std::string line;
  const bool infer_object_blob_assets = indexed_blob_ids.empty() && legacy_asset_discovery;
  std::unordered_map<std::uint64_t, std::string> asset_path_by_object_id;
  const std::string asset_spool_path = (fs::path("spool") / "asset-payloads.bin").generic_string();
  std::uint64_t next_spool_offset = committed_spool_end(assets, asset_spool_path);
  bool spool_recovery_closed = incomplete_spool_ref && incomplete_spool_ref->blob_id != 0;
  while (std::getline(input, line)) {
    const bool has_path_field =
        line.find("_path\"") != std::string::npos ||
        line.find("\"path\"") != std::string::npos;
    const bool has_raw_pso = line.find("\"pso_raw_version\"") != std::string::npos;
    const bool has_object_blob_refs =
        infer_object_blob_assets &&
        line.find("\"object_refs\"") != std::string::npos &&
        line.find("\"blob_refs\"") != std::string::npos;
    if (!has_path_field && !has_raw_pso && !has_object_blob_refs) {
      continue;
    }
    const auto record = json::parse(line, nullptr, false);
    if (record.is_discarded()) {
      continue;
    }

    std::vector<std::uint64_t> blob_refs;
    std::vector<std::string> paths;
    collect_asset_references_from_json(record, blob_refs, paths);

    const auto function = record.value("function", std::string());
    const auto payload = record.value("payload", json::object());
    if (relative_path == fs::path(apitrace::trace::kCallstreamFileName) &&
        function == "ID3D12Resource::Unmap" &&
        !spool_recovery_closed &&
        payload.is_object() &&
        blob_refs.size() == 1 &&
        indexed_blob_ids.find(blob_refs.front()) == indexed_blob_ids.end() &&
        payload.contains("buffer_path") &&
        payload["buffer_path"].is_string() &&
        payload.contains("written_size") &&
        (payload["written_size"].is_number_unsigned() || payload["written_size"].is_number_integer())) {
      const auto blob_id = blob_refs.front();
      const auto buffer_path = payload["buffer_path"].get<std::string>();
      const auto &written_size_node = payload["written_size"];
      std::uint64_t byte_size = 0;
      if (written_size_node.is_number_unsigned()) {
        byte_size = written_size_node.get<std::uint64_t>();
      } else {
        const auto signed_size = written_size_node.get<std::int64_t>();
        if (signed_size < 0) {
          continue;
        }
        byte_size = static_cast<std::uint64_t>(signed_size);
      }
      bool spool_incomplete = false;
      if (auto recovered = recover_spooled_unmap_asset(
              bundle_root,
              blob_id,
              buffer_path,
              byte_size,
              asset_spool_path,
              next_spool_offset,
              &spool_incomplete)) {
        assets.push_back(std::move(*recovered));
        indexed_blob_ids.insert(blob_id);
        next_spool_offset += byte_size;
        ++stats.input_assets;
        ++stats.recovered_spooled_assets;
        stats.recovered_spooled_asset_bytes += byte_size;
      } else if (spool_incomplete && incomplete_spool_ref && incomplete_spool_ref->blob_id == 0) {
        incomplete_spool_ref->blob_id = blob_id;
        incomplete_spool_ref->sequence = record.value("sequence", 0ull);
        incomplete_spool_ref->path = buffer_path;
        spool_recovery_closed = true;
      }
    }

    if (record.value("record_kind", std::string()) == "object_create" &&
        paths.size() == 1 &&
        record.contains("object_id") &&
        record["object_id"].is_number_unsigned()) {
      asset_path_by_object_id[record["object_id"].get<std::uint64_t>()] = paths.front();
    }

    if (is_d3d12_pipeline_create_function(function) &&
        payload.is_object() &&
        payload.value("pso_raw_version", 0) == 1) {
      std::size_t indexed_paths = 0;
      for (; indexed_paths < paths.size() && indexed_paths < blob_refs.size(); ++indexed_paths) {
        add_discovered_asset(bundle_root, assets, stats, indexed_blob_ids, blob_refs[indexed_paths], paths[indexed_paths]);
      }
      for (std::size_t index = indexed_paths; index < blob_refs.size(); ++index) {
        const auto blob_id = blob_refs[index];
        if (const auto path = raw_asset_path_for_blob_id(bundle_root, blob_id, {".dxil", ".dxbc"})) {
          add_discovered_asset(bundle_root, assets, stats, indexed_blob_ids, blob_id, *path);
        }
      }
      continue;
    }
    if (blob_refs.empty() || paths.empty()) {
      if (!blob_refs.empty() && record.contains("object_refs") && record["object_refs"].is_array()) {
        std::vector<std::string> object_asset_paths;
        for (const auto &object_ref : record["object_refs"]) {
          if (!object_ref.is_number_unsigned()) {
            continue;
          }
          const auto path_it = asset_path_by_object_id.find(object_ref.get<std::uint64_t>());
          if (path_it != asset_path_by_object_id.end()) {
            object_asset_paths.push_back(path_it->second);
          }
        }
        for (std::size_t index = 0; index < object_asset_paths.size() && index < blob_refs.size(); ++index) {
          add_discovered_asset(bundle_root, assets, stats, indexed_blob_ids, blob_refs[index], object_asset_paths[index]);
        }
      }
      continue;
    }
    if (is_d3d12_pipeline_create_function(function) &&
        payload.is_object() &&
        payload.contains("pipeline_path") &&
        payload["pipeline_path"].is_string()) {
      const auto pipeline_path = payload["pipeline_path"].get<std::string>();
      add_discovered_asset(bundle_root, assets, stats, indexed_blob_ids, blob_refs[0], pipeline_path);

      const auto pipeline_json_path = bundle_root / fs::path(pipeline_path);
      std::ifstream pipeline_input(pipeline_json_path, std::ios::binary);
      const auto pipeline_json = json::parse(pipeline_input, nullptr, false);
      if (!pipeline_json.is_discarded()) {
        std::vector<std::string> dependency_paths;
        collect_pipeline_dependency_paths(pipeline_json, dependency_paths);
        for (std::size_t index = 0;
             index < dependency_paths.size() && index + 1 < blob_refs.size();
             ++index) {
          add_discovered_asset(bundle_root, assets, stats, indexed_blob_ids, blob_refs[index + 1], dependency_paths[index]);
        }
      }
      continue;
    }

    for (std::size_t index = 0; index < paths.size(); ++index) {
      if (index >= blob_refs.size()) {
        break;
      }
      add_discovered_asset(bundle_root, assets, stats, indexed_blob_ids, blob_refs[index], paths[index]);
    }
  }
}

void discover_raw_asset_references(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    bool legacy_asset_discovery,
    IncompleteSpoolRef *incomplete_spool_ref,
    Stats &stats)
{
  std::unordered_set<std::uint64_t> indexed_blob_ids;
  for (const auto &asset : assets) {
    if (asset.blob_id != 0) {
      indexed_blob_ids.insert(asset.blob_id);
    }
  }
  discover_raw_asset_references_from_jsonl(
      bundle_root,
      fs::path(apitrace::trace::kCallstreamFileName),
      assets,
      indexed_blob_ids,
      legacy_asset_discovery,
      incomplete_spool_ref,
      stats);
  discover_raw_asset_references_from_jsonl(
      bundle_root,
      fs::path(apitrace::trace::kMetalCallstreamFileName),
      assets,
      indexed_blob_ids,
      legacy_asset_discovery,
      nullptr,
      stats);
  discover_raw_asset_references_from_jsonl(
      bundle_root,
      fs::path(apitrace::trace::kAnalysisDirectoryName) / apitrace::trace::kTranslationLinksFileName,
      assets,
      indexed_blob_ids,
      legacy_asset_discovery,
      nullptr,
      stats);
}

void restore_inline_query_assets(const fs::path &bundle_root, const Options &options, Stats &stats)
{
  const auto callstream_path = bundle_root / apitrace::trace::kCallstreamFileName;
  if (!fs::is_regular_file(callstream_path)) {
    return;
  }

  std::ifstream input(callstream_path, std::ios::binary);
  ++stats.jsonl_passes;
  std::string line;
  while (std::getline(input, line)) {
    if (line.find("ID3D12GraphicsCommandList::ResolveQueryDataResult") == std::string::npos) {
      continue;
    }
    const auto record = json::parse(line, nullptr, false);
    if (record.is_discarded() ||
        record.value("function", std::string()) != "ID3D12GraphicsCommandList::ResolveQueryDataResult") {
      continue;
    }

    const auto payload_it = record.find("payload");
    if (payload_it == record.end() || !payload_it->is_object()) {
      continue;
    }
    const auto &payload = *payload_it;
    if (!payload.contains("buffer_path") || !payload["buffer_path"].is_string() ||
        !payload.contains("resolved_data_hex") || !payload["resolved_data_hex"].is_string()) {
      continue;
    }

    const fs::path relative_path(payload["buffer_path"].get<std::string>());
    if (!safe_relative_path(relative_path)) {
      continue;
    }

    const auto expected_size = payload.value("resolved_size", 0ull);
    std::vector<std::uint8_t> bytes;
    if (!decode_hex_bytes(payload["resolved_data_hex"].get<std::string>(), expected_size, bytes)) {
      continue;
    }

    const auto absolute_path = bundle_root / relative_path;
    std::error_code error;
    const bool file_is_complete =
        fs::is_regular_file(absolute_path, error) &&
        !error &&
        fs::file_size(absolute_path, error) == expected_size &&
        !error;
    if (file_is_complete) {
      continue;
    }

    ++stats.restored_inline_query_assets;
    if (options.dry_run) {
      continue;
    }

    fs::create_directories(absolute_path.parent_path(), error);
    if (error) {
      std::cerr << "warning: failed to create directory for " << relative_path.generic_string()
                << ": " << error.message() << "\n";
      continue;
    }

    std::ofstream output(absolute_path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
      output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
      std::cerr << "warning: failed to restore inline query asset " << relative_path.generic_string() << "\n";
    }
  }
}

void load_asset_index(
    const fs::path &path,
    AssetSource source,
    std::vector<AssetEntry> &assets,
    Stats &stats)
{
  if (!fs::is_regular_file(path)) {
    return;
  }

  std::ifstream input(path);
  const auto root = json::parse(input, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    std::cerr << "warning: failed to parse " << path << "\n";
    return;
  }
  const auto list = root.find("assets");
  if (list == root.end() || !list->is_array()) {
    return;
  }

  for (const auto &entry : *list) {
    if (!entry.is_object()) {
      continue;
    }
    AssetEntry asset;
    asset.blob_id = entry.value("blob_id", 0ull);
    asset.path = entry.value("path", std::string());
    asset.kind = entry.value("kind", std::string("Unknown"));
    asset.metal = entry.value("metal", false);
    asset.binary_payload = entry.value("binary_payload", true);
    asset.source = source;
    asset.byte_size = entry.value("byte_size", 0ull);
    asset.debug_name = entry.value("debug_name", std::string());
    asset.content_hash = entry.value("content_hash", std::string());
    asset.fast_fingerprint = entry.value("fast_fingerprint", std::string());
    asset.payload_path = entry.value("payload_path", std::string());
    asset.payload_offset = entry.value("payload_offset", 0ull);
    if (asset.blob_id == 0 || asset.path.empty()) {
      continue;
    }
    assets.push_back(std::move(asset));
    ++stats.input_assets;
  }
}

std::vector<AssetEntry> merge_assets(std::vector<AssetEntry> assets)
{
  std::vector<AssetEntry> merged;
  std::unordered_map<std::string, std::size_t> index_by_key;
  merged.reserve(assets.size());
  for (auto &asset : assets) {
    const auto key = std::to_string(asset.blob_id) + "#" + asset.path;
    const auto existing = index_by_key.find(key);
    if (existing == index_by_key.end()) {
      index_by_key.emplace(key, merged.size());
      merged.push_back(std::move(asset));
    } else {
      merged[existing->second] = std::move(asset);
    }
  }
  return merged;
}

std::string effective_asset_path(const AssetEntry &asset)
{
  return asset.canonical_path.empty() ? asset.path : asset.canonical_path;
}

void add_unambiguous_alias(
    std::unordered_map<std::string, std::string> &aliases,
    std::unordered_set<std::string> &ambiguous_aliases,
    const std::string &from,
    const std::string &to)
{
  if (from.empty() || to.empty() || from == to || ambiguous_aliases.find(from) != ambiguous_aliases.end()) {
    return;
  }
  const auto [it, inserted] = aliases.emplace(from, to);
  if (!inserted && it->second != to) {
    aliases.erase(it);
    ambiguous_aliases.insert(from);
  }
}

void merge_unambiguous_aliases(
    std::unordered_map<std::string, std::string> &aliases,
    std::unordered_set<std::string> &ambiguous_aliases,
    const std::unordered_map<std::string, std::string> &extra_aliases)
{
  for (const auto &[from, to] : extra_aliases) {
    add_unambiguous_alias(aliases, ambiguous_aliases, from, to);
  }
}

bool is_shader_asset(const AssetEntry &asset)
{
  return asset.kind == "ShaderDxil" || asset.kind == "ShaderDxbc";
}

bool is_pipeline_asset(const AssetEntry &asset)
{
  return asset.kind == "Pipeline";
}

std::uint64_t asset_storage_size(const AssetEntry &asset)
{
  if (asset.digest == kEmptySha256Digest || asset.content_hash == kEmptySha256Digest) {
    return 0;
  }
  return asset.actual_size != 0 ? asset.actual_size : asset.byte_size;
}

void stat_assets(const fs::path &bundle_root, std::vector<AssetEntry> &assets)
{
  for (auto &asset : assets) {
    const fs::path relative(effective_asset_path(asset));
    asset.safe_path = safe_relative_path(relative);
    if (!asset.payload_path.empty()) {
      const fs::path payload_relative(asset.payload_path);
      asset.safe_payload_path = safe_relative_path(payload_relative);
      if (asset.safe_payload_path && asset.byte_size != 0) {
        const auto payload_absolute = bundle_root / payload_relative;
        std::error_code payload_error;
        if (fs::is_regular_file(payload_absolute, payload_error) && !payload_error) {
          const auto payload_size = fs::file_size(payload_absolute, payload_error);
          if (!payload_error &&
              asset.payload_offset <= payload_size &&
              asset.byte_size <= payload_size - asset.payload_offset) {
            asset.payload_slice_exists = true;
            asset.actual_size = asset.byte_size;
          }
        }
      }
    }
    if (!asset.safe_path) {
      continue;
    }
    const auto absolute = bundle_root / relative;
    std::error_code error;
    if (!fs::is_regular_file(absolute, error) || error) {
      if (!asset.content_hash.empty()) {
        asset.digest = asset.content_hash;
        if (asset.content_hash == kEmptySha256Digest) {
          asset.byte_size = 0;
          asset.actual_size = 0;
        } else if (asset.byte_size != 0) {
          asset.actual_size = asset.byte_size;
        }
      }
      continue;
    }
    const auto size = fs::file_size(absolute, error);
    if (error) {
      continue;
    }
    asset.actual_size = static_cast<std::uint64_t>(size);
    asset.file_exists = true;
    asset.original_file_exists = true;
    if (asset.byte_size == 0) {
      asset.byte_size = asset.actual_size;
    }
  }
}

void hash_assets(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    std::size_t jobs,
    bool reuse_existing_hashes,
    Stats &stats,
    FileDigestCache &digest_cache,
    ProgressReporter *progress)
{
  struct PathGroup {
    std::string path;
    std::string payload_path;
    std::uint64_t payload_offset = 0;
    bool payload_slice = false;
    std::vector<std::size_t> asset_indices;
    std::string digest;
    std::uint64_t byte_size = 0;
  };

  std::vector<PathGroup> groups;
  std::unordered_map<std::string, std::size_t> group_by_path;
  std::uint64_t total_bytes = 0;
  groups.reserve(assets.size());
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto &asset = assets[index];
    if (!asset.file_exists &&
        !asset.payload_slice_exists &&
        asset.content_hash == kEmptySha256Digest) {
      auto &mutable_asset = assets[index];
      mutable_asset.digest = mutable_asset.content_hash;
      mutable_asset.byte_size = 0;
      mutable_asset.actual_size = 0;
      ++stats.hashed_assets;
      continue;
    }
    if (asset.file_exists &&
        reuse_existing_hashes &&
        !asset.content_hash.empty() &&
        asset.content_hash != kEmptySha256Digest &&
        asset.actual_size != 0 &&
        asset.byte_size == asset.actual_size) {
      auto &mutable_asset = assets[index];
      mutable_asset.digest = mutable_asset.content_hash;
      digest_cache.remember(
          bundle_root / fs::path(effective_asset_path(mutable_asset)),
          mutable_asset.digest,
          mutable_asset.actual_size);
      ++stats.hashed_assets;
      ++stats.asset_hash_reused_files;
      stats.asset_hash_reused_bytes += mutable_asset.actual_size;
      continue;
    }
    if (!asset.file_exists && !asset.payload_slice_exists) {
      continue;
    }
    const auto path = effective_asset_path(asset);
    const auto group_key = asset.payload_slice_exists
                               ? (std::string("slice#") + asset.payload_path + "#" +
                                  std::to_string(asset.payload_offset) + "#" + std::to_string(asset.byte_size))
                               : (std::string("file#") + path);
    const auto [it, inserted] = group_by_path.emplace(group_key, groups.size());
    if (inserted) {
      PathGroup group;
      group.path = path;
      group.payload_path = asset.payload_path;
      group.payload_offset = asset.payload_offset;
      group.payload_slice = asset.payload_slice_exists;
      group.byte_size = asset.payload_slice_exists ? asset.byte_size : asset.actual_size;
      group.asset_indices.push_back(index);
      total_bytes += group.byte_size;
      groups.push_back(std::move(group));
    } else {
      groups[it->second].asset_indices.push_back(index);
    }
  }

  std::vector<std::size_t> file_group_indices;
  struct PayloadTask {
    std::string payload_path;
    std::vector<std::size_t> group_indices;
    std::uint64_t byte_size = 0;
  };
  std::unordered_map<std::string, std::size_t> payload_task_by_path;
  std::vector<PayloadTask> payload_tasks;
  for (std::size_t index = 0; index < groups.size(); ++index) {
    auto &group = groups[index];
    if (!group.payload_slice) {
      file_group_indices.push_back(index);
      continue;
    }
    const auto [it, inserted] = payload_task_by_path.emplace(group.payload_path, payload_tasks.size());
    if (inserted) {
      PayloadTask task;
      task.payload_path = group.payload_path;
      payload_tasks.push_back(std::move(task));
    }
    auto &task = payload_tasks[it->second];
    task.group_indices.push_back(index);
    task.byte_size += group.byte_size;
  }
  for (auto &task : payload_tasks) {
    std::sort(task.group_indices.begin(), task.group_indices.end(), [&](std::size_t lhs, std::size_t rhs) {
      const auto &a = groups[lhs];
      const auto &b = groups[rhs];
      if (a.payload_offset != b.payload_offset) {
        return a.payload_offset < b.payload_offset;
      }
      return a.byte_size < b.byte_size;
    });
  }
  std::sort(payload_tasks.begin(), payload_tasks.end(), [](const PayloadTask &lhs, const PayloadTask &rhs) {
    return lhs.byte_size > rhs.byte_size;
  });

  std::atomic<std::uint64_t> completed_files{0};
  std::atomic<std::uint64_t> completed_bytes{0};
  std::mutex error_mutex;
  std::vector<std::string> errors;
  auto note_completed = [&](const PathGroup &group) {
    const auto done_files = completed_files.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto done_bytes = completed_bytes.fetch_add(group.byte_size, std::memory_order_relaxed) + group.byte_size;
    if (progress) {
      progress->update(done_files, groups.size(), done_bytes, total_bytes);
    }
  };
  auto record_error = [&](const std::string &message) {
    std::lock_guard<std::mutex> lock(error_mutex);
    errors.push_back(message);
  };

  std::atomic<std::size_t> next_file_group{0};
  const auto file_worker = [&]() {
    for (;;) {
      const auto cursor = next_file_group.fetch_add(1, std::memory_order_relaxed);
      if (cursor >= file_group_indices.size()) {
        return;
      }
      auto &group = groups[file_group_indices[cursor]];
      try {
        group.digest = digest_cache.digest_file(bundle_root / group.path);
      } catch (const std::exception &error) {
        record_error(group.path + ": " + error.what());
      }
      note_completed(group);
    }
  };

  auto digest_payload_slice = [](
      std::ifstream &input,
      std::uint64_t offset,
      std::uint64_t byte_size,
      std::vector<std::uint8_t> &buffer) -> std::string {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input.good()) {
      throw std::runtime_error("failed to seek payload slice");
    }

    apitrace::trace::ContentHasher hasher;
    std::uint64_t remaining = byte_size;
    while (remaining != 0) {
      const auto chunk_size = static_cast<std::streamsize>(
          std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
      input.read(reinterpret_cast<char *>(buffer.data()), chunk_size);
      const auto read_count = static_cast<std::size_t>(input.gcount());
      if (read_count == 0) {
        throw std::runtime_error("short payload slice read");
      }
      hasher.update(buffer.data(), read_count);
      remaining -= read_count;
    }
    return hasher.final_hex();
  };

  std::atomic<std::size_t> next_payload_task{0};
  const auto payload_worker = [&]() {
    std::vector<std::uint8_t> buffer(kFileCopyBufferSize);
    for (;;) {
      const auto task_index = next_payload_task.fetch_add(1, std::memory_order_relaxed);
      if (task_index >= payload_tasks.size()) {
        return;
      }
      const auto &task = payload_tasks[task_index];
      std::ifstream input(bundle_root / task.payload_path, std::ios::binary);
      if (!input.is_open()) {
        for (const auto group_index : task.group_indices) {
          record_error(groups[group_index].path + ": failed to open payload slice file");
          note_completed(groups[group_index]);
        }
        continue;
      }
      for (const auto group_index : task.group_indices) {
        auto &group = groups[group_index];
        try {
          group.digest = digest_payload_slice(input, group.payload_offset, group.byte_size, buffer);
        } catch (const std::exception &error) {
          record_error(group.path + ": " + error.what());
        }
        note_completed(group);
      }
    }
  };

  std::vector<std::thread> threads;
  const auto file_thread_count = std::min(jobs, file_group_indices.size());
  threads.reserve(file_thread_count);
  for (std::size_t i = 0; i < file_thread_count; ++i) {
    threads.emplace_back(file_worker);
  }
  for (auto &thread : threads) {
    thread.join();
  }
  threads.clear();
  const auto payload_thread_count = std::min(jobs, payload_tasks.size());
  threads.reserve(payload_thread_count);
  for (std::size_t i = 0; i < payload_thread_count; ++i) {
    threads.emplace_back(payload_worker);
  }
  for (auto &thread : threads) {
    thread.join();
  }
  if (progress) {
    progress->update(groups.size(), groups.size(), total_bytes, total_bytes);
  }

  for (const auto &error : errors) {
    std::cerr << "warning: hash failed for " << error << "\n";
  }

  for (const auto &group : groups) {
    if (group.digest.empty()) {
      continue;
    }
    ++stats.hashed_unique_files;
    stats.hashed_asset_bytes += group.byte_size;
    for (const auto index : group.asset_indices) {
      auto &asset = assets[index];
      if (asset.content_hash != group.digest || asset.byte_size != group.byte_size) {
        ++stats.refreshed_asset_hashes;
      }
      asset.digest = group.digest;
      asset.content_hash = group.digest;
      asset.byte_size = group.byte_size;
      asset.actual_size = group.byte_size;
      if (group.payload_slice) {
        ++stats.spooled_assets;
        stats.spooled_asset_bytes += group.byte_size;
      }
      ++stats.hashed_assets;
    }
  }
}

std::unordered_map<std::string, std::string> ensure_canonical_files(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    const Options &options,
    Stats &stats,
    FileDigestCache &digest_cache)
{
  std::unordered_map<std::string, std::string> aliases;
  std::unordered_set<std::string> ambiguous_aliases;
  std::unordered_map<std::string, std::vector<std::size_t>> groups;
  FileCopyScratch copy_scratch;
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto &asset = assets[index];
    if (asset.digest.empty()) {
      continue;
    }
    const auto size = asset_storage_size(asset);
    if (size == 0 && asset.digest != kEmptySha256Digest) {
      continue;
    }
    const auto key = std::string(asset.metal ? "metal#" : "generic#") + asset.kind + "#" +
                     (asset.binary_payload ? "bin" : "text") + "#" +
                     std::to_string(size) + "#" +
                     asset.digest;
    groups[key].push_back(index);
  }

  for (auto &[_, group] : groups) {
    std::sort(group.begin(), group.end(), [&](std::size_t lhs, std::size_t rhs) {
      if (assets[lhs].file_exists != assets[rhs].file_exists) {
        return assets[lhs].file_exists;
      }
      if (assets[lhs].payload_slice_exists != assets[rhs].payload_slice_exists) {
        return assets[lhs].payload_slice_exists;
      }
      return assets[lhs].path < assets[rhs].path;
    });
    auto &canonical = assets[group.front()];
    canonical.canonical_path = canonical_asset_path(canonical);
    const auto canonical_absolute = bundle_root / canonical.canonical_path;
    const auto source_absolute = bundle_root / canonical.path;

    const auto canonical_size = canonical.actual_size ? canonical.actual_size : canonical.byte_size;
    const bool canonical_is_known_empty =
        canonical.digest == kEmptySha256Digest;
    const auto materialized_size = canonical_is_known_empty ? 0 : canonical_size;

    if (!options.dry_run &&
        (canonical.file_exists || canonical.payload_slice_exists || canonical_is_known_empty) &&
        canonical.path != canonical.canonical_path) {
      fs::create_directories(canonical_absolute.parent_path());
      std::error_code error;
      if (fs::exists(canonical_absolute, error)) {
        const auto existing_size = fs::file_size(canonical_absolute, error);
        bool remove_existing = error || existing_size != materialized_size;
        if (!remove_existing) {
          if (options.verify_existing_canonical) {
            remove_existing = digest_cache.digest_file(canonical_absolute) != canonical.digest;
            if (!remove_existing) {
              ++stats.verified_existing_canonical_files;
              stats.verified_existing_canonical_bytes += existing_size;
            }
          } else {
            // The canonical filename is derived from the digest hash_assets just computed for the
            // source file or spool slice. Avoid re-reading the duplicate canonical file during
            // resumed finalization; use bundle-check --verify-hashes for a strict corruption audit.
            digest_cache.remember(canonical_absolute, canonical.digest, materialized_size);
            ++stats.reused_existing_canonical_files;
            stats.reused_existing_canonical_bytes += existing_size;
          }
        }
        if (remove_existing) {
          fs::remove(canonical_absolute, error);
          ++stats.removed_stale_canonical_files;
        }
      }
      if (!fs::exists(canonical_absolute, error)) {
        if (canonical_is_known_empty) {
          std::ofstream output(canonical_absolute, std::ios::binary);
          if (!output.is_open()) {
            std::cerr << "warning: failed to create canonical empty asset "
                      << canonical.canonical_path << "\n";
            canonical.canonical_path = canonical.path;
          } else {
            output.close();
            canonical.file_exists = true;
            digest_cache.remember(canonical_absolute, canonical.digest, 0);
            canonical.byte_size = 0;
            canonical.actual_size = 0;
          }
        } else if (canonical.payload_slice_exists) {
          std::string copy_error;
          if (!copy_file_range(
                  bundle_root / canonical.payload_path,
                  canonical.payload_offset,
                  canonical.byte_size,
                  canonical_absolute,
                  copy_error,
                  copy_scratch)) {
            std::cerr << "warning: failed to materialize spooled asset " << canonical.canonical_path
                      << " from " << canonical.payload_path << ": " << copy_error << "\n";
            canonical.canonical_path = canonical.path;
          } else {
            ++stats.materialized_spooled_assets;
            canonical.file_exists = true;
            canonical.payload_slice_exists = false;
            digest_cache.remember(canonical_absolute, canonical.digest, materialized_size);
          }
        } else {
          fs::create_hard_link(source_absolute, canonical_absolute, error);
          if (error) {
            fs::copy_file(source_absolute, canonical_absolute, fs::copy_options::overwrite_existing, error);
          }
          if (error) {
            std::cerr << "warning: failed to create canonical asset " << canonical.canonical_path
                      << " from " << canonical.path << ": " << error.message() << "\n";
            canonical.canonical_path = canonical.path;
          } else {
            digest_cache.remember(canonical_absolute, canonical.digest, materialized_size);
          }
        }
      }
    }

    for (const auto index : group) {
      auto &asset = assets[index];
      asset.canonical_path = canonical.canonical_path.empty() ? canonical.path : canonical.canonical_path;
      asset.content_hash = asset.digest;
      asset.byte_size = asset_storage_size(asset);
      asset.actual_size = asset.byte_size;
      if (!asset.canonical_path.empty() && asset.canonical_path != asset.path) {
        asset.file_exists = true;
      }
      if (!asset.payload_path.empty()) {
        asset.payload_path.clear();
        asset.payload_offset = 0;
        asset.payload_slice_exists = false;
      }
      if (asset.path != asset.canonical_path) {
        add_unambiguous_alias(aliases, ambiguous_aliases, asset.path, asset.canonical_path);
        ++stats.rewritten_assets;
      }
    }
    if (group.size() > 1) {
      stats.duplicate_assets += group.size() - 1;
      stats.duplicate_bytes += asset_storage_size(assets[group.front()]) * (group.size() - 1);
    }
  }
  return aliases;
}

std::unordered_map<std::string, std::string> build_aliases(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::string, std::string> aliases;
  std::unordered_set<std::string> ambiguous_aliases;
  for (const auto &asset : assets) {
    auto canonical_path = asset.canonical_path;
    const auto size = asset_storage_size(asset);
    const bool can_materialize =
        asset.file_exists ||
        asset.payload_slice_exists ||
        (size == 0 && asset.digest == kEmptySha256Digest);
    if (canonical_path.empty() && can_materialize && !asset.digest.empty()) {
      canonical_path = canonical_asset_path(asset);
    }
    if (!canonical_path.empty() && asset.path != canonical_path) {
      add_unambiguous_alias(aliases, ambiguous_aliases, asset.path, canonical_path);
    }
  }
  return aliases;
}

std::unordered_map<std::string, std::uint64_t> build_blob_id_by_effective_path(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::string, std::uint64_t> blob_id_by_path;
  std::unordered_set<std::string> ambiguous_paths;
  for (const auto &asset : assets) {
    const auto path = effective_asset_path(asset);
    if (path.empty() || asset.blob_id == 0 || ambiguous_paths.find(path) != ambiguous_paths.end()) {
      continue;
    }
    const auto [it, inserted] = blob_id_by_path.emplace(path, asset.blob_id);
    if (!inserted && it->second != asset.blob_id) {
      blob_id_by_path.erase(it);
      ambiguous_paths.insert(path);
    }
  }
  return blob_id_by_path;
}

std::unordered_map<std::uint64_t, std::uint64_t> normalize_blob_ids(
    std::vector<AssetEntry> &assets,
    Stats &stats,
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint64_t>> *blob_id_remap_by_path = nullptr)
{
  constexpr std::size_t kConflictWarningLimit = 16;
  std::uint64_t next_blob_id = 0;
  for (const auto &asset : assets) {
    next_blob_id = std::max(next_blob_id, asset.blob_id);
  }

  std::vector<std::size_t> order;
  order.reserve(assets.size());
  for (std::size_t index = 0; index < assets.size(); ++index) {
    order.push_back(index);
  }
  std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    if (assets[lhs].source != assets[rhs].source) {
      return assets[lhs].source == AssetSource::Primary;
    }
    return lhs < rhs;
  });

  std::unordered_map<std::uint64_t, std::string> path_by_blob_id;
  std::unordered_map<std::uint64_t, std::uint64_t> sideband_blob_remap;
  std::size_t conflict_warnings = 0;
  for (const auto index : order) {
    auto &asset = assets[index];
    const auto path = effective_asset_path(asset);
    const auto [it, inserted] = path_by_blob_id.emplace(asset.blob_id, path);
    if (inserted || it->second == path) {
      continue;
    }

    const auto old_blob_id = asset.blob_id;
    asset.blob_id = ++next_blob_id;
    path_by_blob_id.emplace(asset.blob_id, path);
    if (blob_id_remap_by_path && !path.empty()) {
      (*blob_id_remap_by_path)[old_blob_id][path] = asset.blob_id;
      if (asset.path != path) {
        (*blob_id_remap_by_path)[old_blob_id][asset.path] = asset.blob_id;
      }
    }
    ++stats.remapped_blob_ids;
    if (asset.source == AssetSource::Sideband) {
      sideband_blob_remap.emplace(old_blob_id, asset.blob_id);
    } else {
      ++stats.primary_blob_id_conflicts;
      if (conflict_warnings < kConflictWarningLimit) {
        std::cerr << "warning: remapped primary blob_id " << old_blob_id
                  << " to " << asset.blob_id << " for " << path << "\n";
      }
      ++conflict_warnings;
    }
  }
  if (conflict_warnings > kConflictWarningLimit) {
    std::cerr << "warning: suppressed " << (conflict_warnings - kConflictWarningLimit)
              << " additional primary blob_id remap warnings\n";
  }
  return sideband_blob_remap;
}

void remove_duplicate_files(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const std::unordered_map<std::string, std::string> &aliases,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    const Options &options,
    Stats &stats)
{
  if (options.keep_duplicates) {
    return;
  }
  std::unordered_set<std::string> indexed_paths;
  for (const auto &asset : assets) {
    const auto effective_path = effective_asset_path(asset);
    if (!effective_path.empty()) {
      indexed_paths.insert(effective_path);
    }
    if (!asset.canonical_path.empty()) {
      indexed_paths.insert(asset.canonical_path);
    }
  }
  for (const auto &[old_path, new_path] : aliases) {
    (void)new_path;
    if (indexed_paths.find(old_path) != indexed_paths.end()) {
      ++stats.preserved_referenced_duplicate_assets;
      continue;
    }
    if (referenced_paths_after_rewrite.find(old_path) != referenced_paths_after_rewrite.end()) {
      ++stats.preserved_referenced_duplicate_assets;
      continue;
    }
    if (options.dry_run) {
      ++stats.removed_files;
      continue;
    }
    std::error_code error;
    if (fs::remove(bundle_root / old_path, error)) {
      ++stats.removed_files;
    } else if (error) {
      std::cerr << "warning: failed to remove duplicate asset " << old_path << ": "
                << error.message() << "\n";
    }
  }
}

std::vector<fs::path> text_reference_files(const fs::path &bundle_root);
bool is_typed_asset_file_path(const fs::path &relative);
std::vector<fs::path> typed_asset_directories();

std::unordered_map<std::string, std::string> build_pipeline_file_aliases(
    const fs::path &bundle_root,
    FileDigestCache &digest_cache)
{
  std::unordered_map<std::string, std::string> aliases;
  std::error_code error;
  const auto pipeline_dir = bundle_root / "pipelines";
  if (!fs::is_directory(pipeline_dir, error) || error) {
    return aliases;
  }
  for (fs::directory_iterator it(pipeline_dir, error), end; it != end && !error; it.increment(error)) {
    if (!it->is_regular_file(error) || error) {
      continue;
    }
    const auto path = it->path();
    if (path.extension() != ".json") {
      continue;
    }
    const auto relative = fs::relative(path, bundle_root, error);
    if (error) {
      continue;
    }
    const auto digest = digest_cache.digest_file(path);
    const auto canonical = (fs::path("pipelines") / (digest + ".pipeline.json")).generic_string();
    const auto relative_text = relative.generic_string();
    if (relative_text != canonical) {
      aliases[relative_text] = canonical;
    }
  }
  return aliases;
}

std::unordered_map<std::string, std::string> build_unindexed_asset_file_aliases(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    FileDigestCache &digest_cache,
    Stats &stats)
{
  std::unordered_map<std::string, std::string> canonical_by_digest;
  std::unordered_set<std::string> ambiguous_digests;
  std::unordered_set<std::string> indexed_paths;
  for (const auto &asset : assets) {
    if (!asset.path.empty()) {
      indexed_paths.insert(asset.path);
    }
    const auto effective_path = effective_asset_path(asset);
    if (!effective_path.empty()) {
      indexed_paths.insert(effective_path);
    }
    if (!asset.canonical_path.empty()) {
      indexed_paths.insert(asset.canonical_path);
    }
    if (asset.content_hash.empty()) {
      continue;
    }
    const auto canonical_path = effective_asset_path(asset);
    if (canonical_path.empty()) {
      continue;
    }
    const auto [it, inserted] = canonical_by_digest.emplace(asset.content_hash, canonical_path);
    if (!inserted && it->second != canonical_path) {
      canonical_by_digest.erase(it);
      ambiguous_digests.insert(asset.content_hash);
    }
  }

  std::unordered_map<std::string, std::string> aliases;
  std::error_code error;
  for (const auto &relative_directory : typed_asset_directories()) {
    const auto directory = bundle_root / relative_directory;
    if (!fs::is_directory(directory, error) || error) {
      error.clear();
      continue;
    }
    for (fs::recursive_directory_iterator it(directory, error), end; it != end && !error; it.increment(error)) {
      if (!it->is_regular_file(error) || error) {
        continue;
      }
      const auto relative = fs::relative(it->path(), bundle_root, error);
      if (error || !safe_relative_path(relative) || !is_typed_asset_file_path(relative)) {
        error.clear();
        continue;
      }
      const auto relative_text = relative.generic_string();
      if (indexed_paths.find(relative_text) != indexed_paths.end() ||
          relative.filename().generic_string().find("asset-") == std::string::npos) {
        continue;
      }
      const auto digest = digest_cache.digest_file(it->path());
      if (digest.empty() || ambiguous_digests.find(digest) != ambiguous_digests.end()) {
        continue;
      }
      const auto canonical = canonical_by_digest.find(digest);
      if (canonical == canonical_by_digest.end() || canonical->second == relative_text) {
        continue;
      }
      aliases.emplace(relative_text, canonical->second);
      ++stats.recovered_unindexed_asset_aliases;
    }
  }
  return aliases;
}

void collect_path_references_from_json(
    const json &node,
    const std::unordered_map<std::string, std::string> &aliases,
    std::unordered_set<std::string> &paths,
    bool include_alias_sources = true)
{
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it->is_string() && is_path_reference_key(it.key())) {
        const auto path = it->get<std::string>();
        const auto alias = aliases.find(path);
        if (include_alias_sources || alias == aliases.end()) {
          paths.insert(path);
        }
        if (alias != aliases.end()) {
          paths.insert(alias->second);
        }
      } else {
        collect_path_references_from_json(it.value(), aliases, paths, include_alias_sources);
      }
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      collect_path_references_from_json(item, aliases, paths, include_alias_sources);
    }
  }
}

bool text_may_contain_path_reference(std::string_view text)
{
  return text.find("_path\"") != std::string::npos ||
         text.find("\"path\"") != std::string::npos;
}

bool text_may_contain_blob_refs(std::string_view text)
{
  return text.find("\"blob_refs\"") != std::string::npos;
}

struct JsonlLineTokens {
  bool path_key = false;
  bool suffix_path_key = false;
  bool asset_token = false;
  bool blob_refs_key = false;
  bool blob_id_key = false;
};

JsonlLineTokens scan_jsonl_line_tokens(std::string_view text)
{
  JsonlLineTokens tokens;
  for (std::size_t cursor = 0; cursor < text.size(); ++cursor) {
    switch (text[cursor]) {
    case '"':
      if (!tokens.path_key &&
          cursor + 6 <= text.size() &&
          text[cursor + 1] == 'p' &&
          text[cursor + 2] == 'a' &&
          text[cursor + 3] == 't' &&
          text[cursor + 4] == 'h' &&
          text[cursor + 5] == '"') {
        tokens.path_key = true;
        cursor += 5;
      } else if (cursor + 11 <= text.size() &&
                 text[cursor + 1] == 'b' &&
                 text[cursor + 2] == 'l' &&
                 text[cursor + 3] == 'o' &&
                 text[cursor + 4] == 'b' &&
                 text[cursor + 5] == '_') {
        if (!tokens.blob_refs_key &&
            text[cursor + 6] == 'r' &&
            text[cursor + 7] == 'e' &&
            text[cursor + 8] == 'f' &&
            text[cursor + 9] == 's' &&
            text[cursor + 10] == '"') {
          tokens.blob_refs_key = true;
          cursor += 10;
        } else if (!tokens.blob_id_key &&
                   cursor + 9 <= text.size() &&
                   text[cursor + 6] == 'i' &&
                   text[cursor + 7] == 'd' &&
                   text[cursor + 8] == '"') {
          tokens.blob_id_key = true;
          cursor += 8;
        }
      }
      break;
    case '_':
      if (!tokens.suffix_path_key &&
          cursor + 6 <= text.size() &&
          text[cursor + 1] == 'p' &&
          text[cursor + 2] == 'a' &&
          text[cursor + 3] == 't' &&
          text[cursor + 4] == 'h' &&
          text[cursor + 5] == '"') {
        tokens.suffix_path_key = true;
        cursor += 5;
      }
      break;
    case 'a':
      if (!tokens.asset_token &&
          cursor + 6 <= text.size() &&
          text[cursor + 1] == 's' &&
          text[cursor + 2] == 's' &&
          text[cursor + 3] == 'e' &&
          text[cursor + 4] == 't' &&
          text[cursor + 5] == '-') {
        tokens.asset_token = true;
        cursor += 5;
      }
      break;
    default:
      break;
    }

    if (tokens.path_key &&
        tokens.suffix_path_key &&
        tokens.asset_token &&
        tokens.blob_refs_key &&
        tokens.blob_id_key) {
      break;
    }
  }
  return tokens;
}

bool tokens_may_contain_path_reference(const JsonlLineTokens &tokens)
{
  return tokens.path_key || tokens.suffix_path_key;
}

std::size_t skip_json_whitespace(std::string_view text, std::size_t cursor);

bool jsonl_key_is_path_reference(std::string_view key)
{
  return key == "path" ||
         (key.size() >= 5 && key.substr(key.size() - 5) == "_path");
}

struct JsonlPathRewriteCandidates {
  bool alias_path = false;
  bool single_blob_asset_alias = false;
  bool path_mapped_blob_refs = false;
};

JsonlPathRewriteCandidates scan_jsonl_path_rewrite_candidates(
    std::string_view line,
    const std::unordered_map<std::string, std::string> &aliases,
    const std::unordered_set<std::string> &blob_id_remap_paths,
    bool need_single_blob_asset_alias,
    bool need_path_mapped_blob_refs)
{
  JsonlPathRewriteCandidates candidates;
  std::size_t cursor = 0;
  while ((cursor = line.find('"', cursor)) != std::string_view::npos) {
    const auto key_begin = cursor + 1;
    const auto key_end = line.find('"', key_begin);
    if (key_end == std::string_view::npos) {
      break;
    }
    const auto key = line.substr(key_begin, key_end - key_begin);
    cursor = key_end + 1;
    if (!jsonl_key_is_path_reference(key)) {
      continue;
    }

    auto value = skip_json_whitespace(line, cursor);
    if (value >= line.size() || line[value] != ':') {
      continue;
    }
    value = skip_json_whitespace(line, value + 1);
    if (value >= line.size() || line[value] != '"') {
      continue;
    }
    const auto value_begin = value + 1;
    auto value_end = value_begin;
    bool escaped = false;
    while (value_end < line.size()) {
      if (line[value_end] == '\\') {
        escaped = true;
        ++value_end;
        if (value_end < line.size()) {
          ++value_end;
        }
        continue;
      }
      if (line[value_end] == '"') {
        break;
      }
      ++value_end;
    }
    if (value_end >= line.size()) {
      break;
    }

    if (escaped) {
      candidates.alias_path = !aliases.empty();
      candidates.single_blob_asset_alias = need_single_blob_asset_alias;
      candidates.path_mapped_blob_refs = need_path_mapped_blob_refs && !blob_id_remap_paths.empty();
      return candidates;
    }

    const auto path = line.substr(value_begin, value_end - value_begin);
    if (!aliases.empty() && aliases.find(std::string(path)) != aliases.end()) {
      candidates.alias_path = true;
    }
    if (need_single_blob_asset_alias && path.find("asset-") != std::string_view::npos) {
      candidates.single_blob_asset_alias = true;
    }
    if (need_path_mapped_blob_refs &&
        !blob_id_remap_paths.empty() &&
        blob_id_remap_paths.find(std::string(path)) != blob_id_remap_paths.end()) {
      candidates.path_mapped_blob_refs = true;
    }
    if (candidates.alias_path &&
        (!need_single_blob_asset_alias || candidates.single_blob_asset_alias) &&
        (!need_path_mapped_blob_refs || candidates.path_mapped_blob_refs)) {
      return candidates;
    }
    cursor = value_end + 1;
  }
  return candidates;
}

bool is_json_whitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_json_digit(char c)
{
  return c >= '0' && c <= '9';
}

bool is_json_number_end(char c)
{
  return c == ',' || c == ']' || c == '}' || is_json_whitespace(c);
}

std::size_t skip_json_whitespace(std::string_view text, std::size_t cursor)
{
  while (cursor < text.size() && is_json_whitespace(text[cursor])) {
    ++cursor;
  }
  return cursor;
}

bool parse_json_unsigned_at(
    std::string_view text,
    std::size_t cursor,
    std::uint64_t &value,
    std::size_t &after)
{
  if (cursor >= text.size() || !is_json_digit(text[cursor])) {
    return false;
  }

  std::uint64_t parsed = 0;
  bool overflow = false;
  while (cursor < text.size() && is_json_digit(text[cursor])) {
    const auto digit = static_cast<std::uint64_t>(text[cursor] - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      overflow = true;
    } else {
      parsed = parsed * 10 + digit;
    }
    ++cursor;
  }

  after = cursor;
  if (overflow || (cursor < text.size() && !is_json_number_end(text[cursor]))) {
    return false;
  }
  value = parsed;
  return true;
}

bool text_may_contain_blob_id_remap(
    std::string_view text,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap)
{
  if (blob_id_remap.empty()) {
    return false;
  }

  std::size_t search = 0;
  while ((search = text.find("\"blob_id\"", search)) != std::string::npos) {
    const auto colon = text.find(':', search + 9);
    if (colon == std::string::npos) {
      break;
    }
    auto cursor = skip_json_whitespace(text, colon + 1);
    std::uint64_t value = 0;
    std::size_t after = cursor;
    if (parse_json_unsigned_at(text, cursor, value, after) &&
        blob_id_remap.find(value) != blob_id_remap.end()) {
      return true;
    }
    search = colon + 1;
  }

  search = 0;
  while ((search = text.find("\"blob_refs\"", search)) != std::string::npos) {
    const auto colon = text.find(':', search + 11);
    if (colon == std::string::npos) {
      break;
    }
    auto cursor = skip_json_whitespace(text, colon + 1);
    if (cursor >= text.size() || text[cursor] != '[') {
      search = colon + 1;
      continue;
    }
    const auto array_end = text.find(']', cursor + 1);
    const auto limit = array_end == std::string::npos ? text.size() : array_end;
    ++cursor;
    while (cursor < limit) {
      cursor = skip_json_whitespace(text, cursor);
      if (cursor >= limit) {
        break;
      }
      if (text[cursor] == ',') {
        ++cursor;
        continue;
      }
      std::uint64_t value = 0;
      std::size_t after = cursor;
      if (parse_json_unsigned_at(text, cursor, value, after)) {
        if (blob_id_remap.find(value) != blob_id_remap.end()) {
          return true;
        }
        cursor = after;
        continue;
      }
      ++cursor;
    }
    search = limit;
  }

  return false;
}

struct JsonlLineView {
  std::string_view line;
  std::uint64_t offset = 0;
  std::uint64_t byte_size = 0;
  bool has_newline = false;
};

template <typename Callback>
bool scan_jsonl_file(const fs::path &path, Callback &&callback)
{
  constexpr std::uint64_t kMaxChunkSize = 64ull * 1024ull * 1024ull;
  constexpr std::uint64_t kMinChunkSize = 64ull * 1024ull;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }

  std::error_code size_error;
  const auto file_size = fs::file_size(path, size_error);
  const auto file_size64 = static_cast<std::uint64_t>(file_size);
  const auto chunk_size = static_cast<std::size_t>(
      size_error ? kMaxChunkSize : std::min(kMaxChunkSize, std::max(kMinChunkSize, file_size64)));
  std::vector<char> buffer(chunk_size);
  std::string carry;
  std::uint64_t absolute_offset = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = input.gcount();
    if (read_count <= 0) {
      break;
    }

    std::string_view chunk(buffer.data(), static_cast<std::size_t>(read_count));
    if (!carry.empty()) {
      carry.append(chunk.data(), chunk.size());
      chunk = std::string_view(carry);
    }

    std::size_t line_begin = 0;
    while (true) {
      const auto newline = chunk.find('\n', line_begin);
      if (newline == std::string_view::npos) {
        break;
      }
      auto line = chunk.substr(line_begin, newline - line_begin);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      JsonlLineView view;
      view.line = line;
      view.offset = absolute_offset;
      view.byte_size = static_cast<std::uint64_t>(newline - line_begin + 1);
      view.has_newline = true;
      if (!callback(view)) {
        return true;
      }
      absolute_offset += view.byte_size;
      line_begin = newline + 1;
    }

    if (line_begin < chunk.size()) {
      if (carry.empty()) {
        carry.assign(chunk.substr(line_begin));
      } else {
        carry.erase(0, line_begin);
      }
    } else {
      carry.clear();
    }
  }

  if (!carry.empty()) {
    auto line = std::string_view(carry);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    JsonlLineView view;
    view.line = line;
    view.offset = absolute_offset;
    view.byte_size = static_cast<std::uint64_t>(carry.size());
    view.has_newline = false;
    return callback(view);
  }

  return true;
}

struct JsonlRewriteScanBatch {
  std::uint64_t offset = 0;
  std::uint64_t byte_size = 0;
  std::uint64_t line_count = 0;
  bool has_candidate = false;
  JsonlLineView candidate;
};

bool line_has_jsonl_rewrite_candidate(
    std::string_view line,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap)
{
  return line.find("asset-") != std::string_view::npos ||
         line.find("\"path\"") != std::string_view::npos ||
         line.find("_path\"") != std::string_view::npos ||
         text_may_contain_blob_id_remap(line, blob_id_remap);
}

template <typename Callback>
bool scan_jsonl_file_for_rewrite(
    const fs::path &path,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap,
    Callback &&callback)
{
  constexpr std::uint64_t kMaxChunkSize = 64ull * 1024ull * 1024ull;
  constexpr std::uint64_t kMinChunkSize = 64ull * 1024ull;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }

  std::error_code size_error;
  const auto file_size = fs::file_size(path, size_error);
  const auto file_size64 = static_cast<std::uint64_t>(file_size);
  const auto chunk_size = static_cast<std::size_t>(
      size_error ? kMaxChunkSize : std::min(kMaxChunkSize, std::max(kMinChunkSize, file_size64)));
  std::vector<char> buffer(chunk_size);
  std::string carry;
  std::uint64_t absolute_offset = 0;
  std::uint64_t skipped_offset = 0;
  std::uint64_t skipped_bytes = 0;
  std::uint64_t skipped_lines = 0;

  auto flush_skipped = [&]() -> bool {
    if (skipped_bytes == 0) {
      return true;
    }
    JsonlRewriteScanBatch batch;
    batch.offset = skipped_offset;
    batch.byte_size = skipped_bytes;
    batch.line_count = skipped_lines;
    skipped_bytes = 0;
    skipped_lines = 0;
    return callback(batch);
  };

  auto add_skipped = [&](std::uint64_t offset, std::uint64_t byte_size, std::uint64_t line_count) {
    if (byte_size == 0 || line_count == 0) {
      return;
    }
    if (skipped_bytes == 0) {
      skipped_offset = offset;
    }
    skipped_bytes += byte_size;
    skipped_lines += line_count;
  };

  auto emit_candidate_line = [&](std::string_view text, std::size_t begin, std::size_t end) -> bool {
    if (!flush_skipped()) {
      return false;
    }
    auto line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\n') {
      line.remove_suffix(1);
    }
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    JsonlRewriteScanBatch batch;
    batch.offset = absolute_offset + static_cast<std::uint64_t>(begin);
    batch.byte_size = static_cast<std::uint64_t>(end - begin);
    batch.line_count = 1;
    batch.has_candidate = true;
    batch.candidate.line = line;
    batch.candidate.offset = batch.offset;
    batch.candidate.byte_size = batch.byte_size;
    batch.candidate.has_newline = end > begin && text[end - 1] == '\n';
    return callback(batch);
  };

  auto process_complete_text = [&](std::string_view text) -> bool {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
      const auto newline = text.find('\n', cursor);
      if (newline == std::string_view::npos) {
        break;
      }
      const auto end = newline + 1;
      auto line = text.substr(cursor, newline - cursor);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (!line_has_jsonl_rewrite_candidate(line, blob_id_remap)) {
        add_skipped(
            absolute_offset + static_cast<std::uint64_t>(cursor),
            static_cast<std::uint64_t>(end - cursor),
            1);
        cursor = end;
        continue;
      }
      if (!emit_candidate_line(text, cursor, end)) {
        return false;
      }
      cursor = end;
    }
    absolute_offset += static_cast<std::uint64_t>(text.size());
    return true;
  };

  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = input.gcount();
    if (read_count <= 0) {
      break;
    }

    std::string_view chunk(buffer.data(), static_cast<std::size_t>(read_count));
    if (!carry.empty()) {
      carry.append(chunk.data(), chunk.size());
      chunk = std::string_view(carry);
    }

    const auto last_newline = chunk.rfind('\n');
    const auto complete_size = last_newline == std::string_view::npos ? 0 : last_newline + 1;
    if (!process_complete_text(chunk.substr(0, complete_size))) {
      return true;
    }

    if (complete_size < chunk.size()) {
      if (carry.empty()) {
        carry.assign(chunk.substr(complete_size));
      } else {
        carry.erase(0, complete_size);
      }
    } else {
      carry.clear();
    }
  }

  if (!carry.empty()) {
    auto line = std::string_view(carry);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const auto byte_size = static_cast<std::uint64_t>(carry.size());
    const bool candidate = line_has_jsonl_rewrite_candidate(line, blob_id_remap);
    if (!candidate) {
      add_skipped(absolute_offset, byte_size, 1);
    } else {
      if (!flush_skipped()) {
        return true;
      }
      JsonlRewriteScanBatch batch;
      batch.offset = absolute_offset;
      batch.byte_size = byte_size;
      batch.line_count = 1;
      batch.has_candidate = true;
      batch.candidate.line = line;
      batch.candidate.offset = absolute_offset;
      batch.candidate.byte_size = byte_size;
      batch.candidate.has_newline = false;
      if (!callback(batch)) {
        return true;
      }
    }
  }

  return flush_skipped();
}

std::unordered_set<std::string> collect_referenced_asset_paths(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::string> &aliases)
{
  std::unordered_set<std::string> referenced_paths;
  for (const auto &relative : text_reference_files(bundle_root)) {
    const auto absolute = bundle_root / relative;
    if (relative.extension() == ".jsonl") {
      scan_jsonl_file(absolute, [&](const JsonlLineView &line_view) {
        const auto line = line_view.line;
        const auto tokens = scan_jsonl_line_tokens(line);
        if (!tokens_may_contain_path_reference(tokens)) {
          return true;
        }
        const auto record = json::parse(std::string(line), nullptr, false);
        if (!record.is_discarded()) {
          collect_path_references_from_json(record, aliases, referenced_paths);
        }
        return true;
      });
      continue;
    }

    std::ifstream input(absolute, std::ios::binary);
    if (!input.is_open()) {
      continue;
    }
    std::ostringstream text_buffer;
    text_buffer << input.rdbuf();
    const auto text = text_buffer.str();
    if (!text_may_contain_rewritable_asset_path(text) &&
        !text_may_contain_path_reference(text)) {
      continue;
    }
    const auto root = json::parse(text, nullptr, false);
    if (!root.is_discarded()) {
      collect_path_references_from_json(root, aliases, referenced_paths);
    }
  }
  return referenced_paths;
}

void collect_blob_references_from_json(const json &node, std::unordered_set<std::uint64_t> &blob_refs)
{
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it.key() == "blob_refs" && it.value().is_array()) {
        for (const auto &ref : it.value()) {
          if (ref.is_number_unsigned()) {
            blob_refs.insert(ref.get<std::uint64_t>());
          }
        }
        continue;
      }
      collect_blob_references_from_json(it.value(), blob_refs);
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      collect_blob_references_from_json(item, blob_refs);
    }
  }
}

void collect_blob_references_from_jsonl_line(std::string_view line, std::unordered_set<std::uint64_t> &blob_refs)
{
  std::size_t search = 0;
  while ((search = line.find("\"blob_refs\"", search)) != std::string_view::npos) {
    const auto colon = line.find(':', search + 11);
    if (colon == std::string_view::npos) {
      break;
    }
    auto cursor = skip_json_whitespace(line, colon + 1);
    if (cursor >= line.size() || line[cursor] != '[') {
      search = colon + 1;
      continue;
    }
    ++cursor;
    while (cursor < line.size()) {
      cursor = skip_json_whitespace(line, cursor);
      if (cursor >= line.size()) {
        return;
      }
      if (line[cursor] == ']') {
        search = cursor + 1;
        break;
      }
      if (line[cursor] == ',') {
        ++cursor;
        continue;
      }
      std::uint64_t value = 0;
      std::size_t after = cursor;
      if (parse_json_unsigned_at(line, cursor, value, after)) {
        blob_refs.insert(value);
        cursor = after;
        continue;
      }
      ++cursor;
    }
  }
}

bool path_exists_as_asset_file(const fs::path &bundle_root, const std::string &path)
{
  if (path.empty()) {
    return false;
  }
  const fs::path relative(path);
  std::error_code error;
  return safe_relative_path(relative) &&
         is_typed_asset_file_path(relative) &&
         fs::is_regular_file(bundle_root / relative, error) &&
         !error;
}

std::optional<std::pair<std::uint64_t, std::string>> single_blob_ref_asset_path_pair(
    const fs::path &bundle_root,
    const json &record)
{
  if (!record.is_object()) {
    return std::nullopt;
  }
  const auto refs = record.find("blob_refs");
  if (refs == record.end() || !refs->is_array() || refs->size() != 1 || !(*refs)[0].is_number_unsigned()) {
    return std::nullopt;
  }
  std::vector<std::uint64_t> blob_refs;
  std::vector<std::string> paths;
  collect_asset_references_from_json(record, blob_refs, paths);
  std::string matched_path;
  for (const auto &path : paths) {
    if (!path_exists_as_asset_file(bundle_root, path)) {
      continue;
    }
    if (!matched_path.empty() && matched_path != path) {
      return std::nullopt;
    }
    matched_path = path;
  }
  if (matched_path.empty() || blob_refs.size() != 1 || paths.size() != 1) {
    return std::nullopt;
  }
  return std::make_pair((*refs)[0].get<std::uint64_t>(), matched_path);
}

void repair_finalized_single_blob_asset_index(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    bool enabled)
{
  if (!enabled) {
    return;
  }

  std::unordered_map<std::uint64_t, std::size_t> asset_index_by_blob_id;
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto blob_id = assets[index].blob_id;
    if (blob_id != 0) {
      asset_index_by_blob_id.emplace(blob_id, index);
    }
  }

  const auto callstream_path = bundle_root / apitrace::trace::kCallstreamFileName;
  if (!fs::is_regular_file(callstream_path)) {
    return;
  }

  scan_jsonl_file(callstream_path, [&](const JsonlLineView &line_view) {
    const auto line = line_view.line;
    const auto tokens = scan_jsonl_line_tokens(line);
    if (!tokens.blob_refs_key || !tokens_may_contain_path_reference(tokens)) {
      return true;
    }
    const auto record = json::parse(std::string(line), nullptr, false);
    if (record.is_discarded()) {
      return true;
    }
    const auto pair = single_blob_ref_asset_path_pair(bundle_root, record);
    if (!pair) {
      return true;
    }
    const auto &[blob_id, path] = *pair;
    const auto index = asset_index_by_blob_id.find(blob_id);
    if (index == asset_index_by_blob_id.end()) {
      auto asset = make_discovered_asset(bundle_root, blob_id, path);
      if (asset) {
        assets.push_back(std::move(*asset));
        asset_index_by_blob_id.emplace(blob_id, assets.size() - 1);
      }
      return true;
    }

    auto &asset = assets[index->second];
    const auto current_path = effective_asset_path(asset);
    if (current_path == path || asset.path == path) {
      return true;
    }
    if (!path_exists_as_asset_file(bundle_root, path)) {
      return true;
    }
    asset.path = path;
    asset.canonical_path.clear();
    asset.kind = asset_kind_from_path(fs::path(path));
    asset.metal = path.rfind("metal/", 0) == 0;
    asset.payload_path.clear();
    asset.payload_offset = 0;
    asset.payload_slice_exists = false;
    asset.file_exists = true;
    asset.original_file_exists = true;
    asset.safe_path = true;
    asset.safe_payload_path = false;
    asset.content_hash.clear();
    asset.digest.clear();
    asset.byte_size = 0;
    asset.actual_size = 0;
    return true;
  });
}

std::unordered_set<std::uint64_t> collect_referenced_blob_ids(const fs::path &bundle_root)
{
  std::unordered_set<std::uint64_t> referenced_blob_ids;
  for (const auto &relative : text_reference_files(bundle_root)) {
    const auto absolute = bundle_root / relative;
    if (relative.extension() == ".jsonl") {
      scan_jsonl_file(absolute, [&](const JsonlLineView &line_view) {
        const auto line = line_view.line;
        const auto tokens = scan_jsonl_line_tokens(line);
        if (!tokens.blob_refs_key) {
          return true;
        }
        collect_blob_references_from_jsonl_line(line, referenced_blob_ids);
        return true;
      });
      continue;
    }

    std::ifstream input(absolute, std::ios::binary);
    if (!input.is_open()) {
      continue;
    }
    std::ostringstream text_buffer;
    text_buffer << input.rdbuf();
    const auto text = text_buffer.str();
    if (text.find("\"blob_refs\"") == std::string::npos) {
      continue;
    }
    const auto root = json::parse(text, nullptr, false);
    if (!root.is_discarded()) {
      collect_blob_references_from_json(root, referenced_blob_ids);
    }
  }
  return referenced_blob_ids;
}

bool is_typed_asset_file_path(const fs::path &relative)
{
  auto part = relative.begin();
  if (part == relative.end()) {
    return false;
  }

  const auto first = part->generic_string();
  ++part;
  if (first == "buffers") {
    return relative.extension() == ".buffer";
  }
  if (first == "textures") {
    return relative.extension() == ".texture";
  }
  if (first == "shaders") {
    const auto extension = relative.extension().generic_string();
    return extension == ".dxbc" || extension == ".dxil" || extension == ".rootsig";
  }
  if (first == "pipelines") {
    return relative.extension() == ".json";
  }
  if (first != "metal" || part == relative.end()) {
    return false;
  }

  const auto second = part->generic_string();
  if (second == "buffers") {
    return relative.extension() == ".buffer";
  }
  if (second == "textures") {
    return relative.extension() == ".texture";
  }
  if (second == "libraries") {
    return relative.extension() == ".metallib";
  }
  if (second == "pipelines") {
    return relative.extension() == ".json";
  }
  return false;
}

void collect_live_asset_paths(
    const std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    std::unordered_set<std::string> &live_paths)
{
  for (const auto &asset : assets) {
    if (!asset.path.empty()) {
      live_paths.insert(asset.path);
    }
    const auto effective_path = effective_asset_path(asset);
    if (!effective_path.empty()) {
      live_paths.insert(effective_path);
    }
    if (!asset.canonical_path.empty()) {
      live_paths.insert(asset.canonical_path);
    }
  }
  live_paths.insert(referenced_paths_after_rewrite.begin(), referenced_paths_after_rewrite.end());
}

std::vector<fs::path> typed_asset_directories()
{
  return {
      fs::path("buffers"),
      fs::path("textures"),
      fs::path("shaders"),
      fs::path("pipelines"),
      fs::path("metal") / "buffers",
      fs::path("metal") / "textures",
      fs::path("metal") / "libraries",
      fs::path("metal") / "pipelines",
  };
}

void remove_orphan_asset_files(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    const Options &options,
    Stats &stats)
{
  std::unordered_set<std::string> live_paths;
  collect_live_asset_paths(assets, referenced_paths_after_rewrite, live_paths);

  for (const auto &relative_directory : typed_asset_directories()) {
    const auto directory = bundle_root / relative_directory;
    std::error_code error;
    if (!fs::is_directory(directory, error) || error) {
      continue;
    }
    for (fs::recursive_directory_iterator it(directory, error), end; it != end && !error; it.increment(error)) {
      if (!it->is_regular_file(error) || error) {
        continue;
      }
      const auto relative = fs::relative(it->path(), bundle_root, error);
      if (error || !safe_relative_path(relative) || !is_typed_asset_file_path(relative)) {
        continue;
      }
      const auto relative_text = relative.generic_string();
      if (live_paths.find(relative_text) != live_paths.end()) {
        continue;
      }
      ++stats.removed_orphan_asset_files;
      if (options.dry_run) {
        ++stats.removed_files;
        continue;
      }
      std::error_code remove_error;
      if (fs::remove(it->path(), remove_error)) {
        ++stats.removed_files;
      } else if (remove_error) {
        std::cerr << "warning: failed to remove orphan asset " << relative_text << ": "
                  << remove_error.message() << "\n";
      }
    }
  }
}

void remove_payload_spool_files(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const Options &options,
    Stats &stats)
{
  std::unordered_set<std::string> payload_paths;
  for (const auto &asset : assets) {
    if (!asset.payload_path.empty() && safe_relative_path(fs::path(asset.payload_path))) {
      payload_paths.insert(asset.payload_path);
    }
  }
  const auto spool_root = bundle_root / "spool";
  std::error_code scan_error;
  if (fs::is_directory(spool_root, scan_error) && !scan_error) {
    for (fs::recursive_directory_iterator it(spool_root, scan_error), end; it != end && !scan_error; it.increment(scan_error)) {
      if (!it->is_regular_file(scan_error) || scan_error) {
        continue;
      }
      const auto relative = fs::relative(it->path(), bundle_root, scan_error);
      if (!scan_error && safe_relative_path(relative)) {
        payload_paths.insert(relative.generic_string());
      }
    }
  }

  for (const auto &relative_text : payload_paths) {
    if (options.dry_run) {
      ++stats.removed_spool_files;
      continue;
    }
    std::error_code error;
    if (fs::remove(bundle_root / relative_text, error)) {
      ++stats.removed_spool_files;
      ++stats.removed_files;
    } else if (error) {
      std::cerr << "warning: failed to remove payload spool " << relative_text << ": "
                << error.message() << "\n";
    }
  }

  if (!options.dry_run) {
    std::error_code error;
    fs::remove_all(bundle_root / "spool", error);
  }
}

void drop_unreferenced_missing_assets(
    std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    const std::unordered_set<std::uint64_t> &referenced_blob_ids_after_rewrite,
    Stats &stats)
{
  const auto old_size = assets.size();
  std::size_t preserved_missing = 0;
  assets.erase(
      std::remove_if(
          assets.begin(),
          assets.end(),
          [&](const AssetEntry &asset) {
            if (asset.file_exists || asset.path.empty()) {
              return false;
            }
            const auto effective_path = effective_asset_path(asset);
            if (referenced_paths_after_rewrite.find(effective_path) != referenced_paths_after_rewrite.end() ||
                referenced_paths_after_rewrite.find(asset.path) != referenced_paths_after_rewrite.end()) {
              if (!asset.file_exists) {
                ++preserved_missing;
              }
              return false;
            }
            if (asset.blob_id != 0 && referenced_blob_ids_after_rewrite.find(asset.blob_id) != referenced_blob_ids_after_rewrite.end()) {
              ++preserved_missing;
              return false;
            }
            return true;
          }),
      assets.end());
  stats.dropped_unreferenced_missing_assets += old_size - assets.size();
  stats.preserved_referenced_missing_assets += preserved_missing;
}

void drop_unreferenced_truncated_assets(
    std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    const std::unordered_set<std::uint64_t> &referenced_blob_ids_after_rewrite,
    Stats &stats)
{
  const auto old_size = assets.size();
  assets.erase(
      std::remove_if(
          assets.begin(),
          assets.end(),
          [&](const AssetEntry &asset) {
            if (asset.path.empty()) {
              return false;
            }
            if (asset.blob_id != 0 &&
                referenced_blob_ids_after_rewrite.find(asset.blob_id) != referenced_blob_ids_after_rewrite.end()) {
              return false;
            }
            const auto effective_path = effective_asset_path(asset);
            if (referenced_paths_after_rewrite.find(asset.path) != referenced_paths_after_rewrite.end() ||
                referenced_paths_after_rewrite.find(effective_path) != referenced_paths_after_rewrite.end()) {
              return false;
            }
            if (!asset.canonical_path.empty() &&
                referenced_paths_after_rewrite.find(asset.canonical_path) != referenced_paths_after_rewrite.end()) {
              return false;
            }
            return true;
          }),
      assets.end());
  stats.dropped_unreferenced_truncated_assets += old_size - assets.size();
}

bool rewrite_path_refs(json &node, const std::unordered_map<std::string, std::string> &aliases)
{
  bool changed = false;
  if (aliases.empty()) {
    return false;
  }

  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it->is_string() && is_path_reference_key(it.key())) {
        const auto found = aliases.find(it->get<std::string>());
        if (found != aliases.end()) {
          *it = found->second;
          changed = true;
        }
      } else {
        changed = rewrite_path_refs(it.value(), aliases) || changed;
      }
    }
  } else if (node.is_array()) {
    for (auto &item : node) {
      changed = rewrite_path_refs(item, aliases) || changed;
    }
  }
  return changed;
}

bool rewrite_exact_alias_string_values(json &node, const std::unordered_map<std::string, std::string> &aliases)
{
  bool changed = false;
  if (aliases.empty()) {
    return false;
  }

  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it->is_string()) {
        const auto found = aliases.find(it->get<std::string>());
        if (found != aliases.end()) {
          *it = found->second;
          changed = true;
        }
      } else {
        changed = rewrite_exact_alias_string_values(it.value(), aliases) || changed;
      }
    }
  } else if (node.is_array()) {
    for (auto &item : node) {
      changed = rewrite_exact_alias_string_values(item, aliases) || changed;
    }
  }
  return changed;
}

bool rewrite_blob_refs_for_effective_paths(
    json &node,
    const std::unordered_map<std::string, std::uint64_t> &blob_id_by_effective_path,
    bool path_references_changed)
{
  if (!path_references_changed || blob_id_by_effective_path.empty() || !node.is_object()) {
    return false;
  }

  auto refs = node.find("blob_refs");
  if (refs == node.end() || !refs->is_array()) {
    return false;
  }

  std::vector<std::uint64_t> blob_refs;
  std::vector<std::string> paths;
  collect_asset_references_from_json(node, blob_refs, paths);
  if (paths.empty() || blob_refs.empty()) {
    return false;
  }

  bool changed = false;
  for (std::size_t index = 0; index < paths.size() && index < refs->size(); ++index) {
    auto &ref = (*refs)[index];
    if (!ref.is_number_unsigned()) {
      continue;
    }
    const auto path = blob_id_by_effective_path.find(paths[index]);
    if (path == blob_id_by_effective_path.end()) {
      continue;
    }
    const auto blob_id = path->second;
    if (blob_id != 0 && ref.get<std::uint64_t>() != blob_id) {
      ref = blob_id;
      changed = true;
    }
  }
  return changed;
}

bool rewrite_blob_refs_by_referenced_paths(
    json &node,
    const std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint64_t>> &remap_by_path)
{
  if (remap_by_path.empty() || !node.is_object()) {
    return false;
  }
  auto refs = node.find("blob_refs");
  if (refs == node.end() || !refs->is_array()) {
    return false;
  }

  std::vector<std::uint64_t> blob_refs;
  std::vector<std::string> paths;
  collect_asset_references_from_json(node, blob_refs, paths);
  if (blob_refs.empty() || paths.empty()) {
    return false;
  }

  bool changed = false;
  for (std::size_t index = 0; index < refs->size() && index < blob_refs.size() && index < paths.size(); ++index) {
    auto &ref = (*refs)[index];
    if (!ref.is_number_unsigned()) {
      continue;
    }
    const auto old_blob_id = ref.get<std::uint64_t>();
    const auto by_old_blob = remap_by_path.find(old_blob_id);
    if (by_old_blob == remap_by_path.end()) {
      continue;
    }
    const auto by_path = by_old_blob->second.find(paths[index]);
    if (by_path == by_old_blob->second.end() || by_path->second == 0 || by_path->second == old_blob_id) {
      continue;
    }
    ref = by_path->second;
    changed = true;
  }
  return changed;
}

bool asset_alias_matches_effective_path(const std::string &alias, const std::string &effective_path)
{
  if (alias.empty() || effective_path.empty() || alias == effective_path) {
    return false;
  }
  if (alias.find("asset-") == std::string::npos) {
    return false;
  }
  const fs::path alias_path(alias);
  const fs::path final_path(effective_path);
  return safe_relative_path(final_path) &&
         (alias_path.extension() == final_path.extension() ||
          alias_path.parent_path().generic_string().rfind("metal/", 0) == 0);
}

bool rewrite_asset_alias_strings_for_effective_path(json &node, const std::string &effective_path)
{
  bool changed = false;
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it->is_string() && is_path_reference_key(it.key())) {
        const auto current = it->get<std::string>();
        if (asset_alias_matches_effective_path(current, effective_path)) {
          *it = effective_path;
          changed = true;
          continue;
        }
      }
      changed = rewrite_asset_alias_strings_for_effective_path(it.value(), effective_path) || changed;
    }
  } else if (node.is_array()) {
    for (auto &item : node) {
      changed = rewrite_asset_alias_strings_for_effective_path(item, effective_path) || changed;
    }
  }
  return changed;
}

bool rewrite_single_blob_asset_alias_refs(
    json &node,
    const std::unordered_map<std::uint64_t, std::string> &effective_path_by_blob_id)
{
  if (effective_path_by_blob_id.empty() || !node.is_object()) {
    return false;
  }
  auto refs = node.find("blob_refs");
  if (refs == node.end() || !refs->is_array() || refs->size() != 1 || !(*refs)[0].is_number_unsigned()) {
    return false;
  }
  const auto path = effective_path_by_blob_id.find((*refs)[0].get<std::uint64_t>());
  if (path == effective_path_by_blob_id.end()) {
    return false;
  }
  return rewrite_asset_alias_strings_for_effective_path(node, path->second);
}

fs::path temporary_rewrite_path(const fs::path &path)
{
  return path.parent_path() / (path.filename().generic_string() + ".bundle-finalize.tmp");
}

bool replace_with_temporary_file(const fs::path &path, const fs::path &temporary)
{
  std::error_code error;
  fs::rename(temporary, path, error);
  if (!error) {
    return true;
  }
  std::cerr << "warning: failed to replace " << path << " with " << temporary
            << ": " << error.message() << "\n";
  fs::remove(temporary, error);
  return false;
}

void note_jsonl_output(Stats &stats, const std::string &line)
{
  stats.output_bytes += static_cast<std::uint64_t>(line.size()) + 1;
}

void note_jsonl_output(Stats &stats, std::string_view line)
{
  stats.output_bytes += static_cast<std::uint64_t>(line.size()) + 1;
}

class HashedOutputFile {
public:
  bool open(const fs::path &path)
  {
    output_.open(path, std::ios::binary | std::ios::trunc);
    return output_.is_open();
  }

  void write_line(const std::string &line)
  {
    write_line(std::string_view(line));
  }

  void write_line(std::string_view line)
  {
    output_.write(line.data(), static_cast<std::streamsize>(line.size()));
    output_.put('\n');
    hasher_.update(line);
    hasher_.update("\n");
    size_ += static_cast<std::uint64_t>(line.size()) + 1;
  }

  void write_text(const std::string &text)
  {
    write_text(std::string_view(text));
  }

  void write_text(std::string_view text)
  {
    output_.write(text.data(), static_cast<std::streamsize>(text.size()));
    hasher_.update(text);
    size_ += static_cast<std::uint64_t>(text.size());
  }

  void close()
  {
    output_.close();
  }

  bool is_open() const
  {
    return output_.is_open();
  }

  std::pair<std::string, std::uint64_t> digest_and_size()
  {
    return {hasher_.final_hex(), size_};
  }

private:
  std::ofstream output_;
  apitrace::trace::ContentHasher hasher_;
  std::uint64_t size_ = 0;
};

bool copy_file_prefix_to_output(
    const fs::path &path,
    std::uint64_t byte_count,
    HashedOutputFile &output,
    FileCopyScratch &scratch)
{
  if (byte_count == 0) {
    return true;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }

  std::uint64_t remaining = byte_count;
  while (remaining != 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(scratch.size())));
    input.read(scratch.data(), chunk);
    const auto read_count = input.gcount();
    if (read_count <= 0) {
      return false;
    }
    output.write_text(std::string_view(scratch.data(), static_cast<std::size_t>(read_count)));
    remaining -= static_cast<std::uint64_t>(read_count);
  }
  return true;
}

bool copy_file_range_to_output(
    const fs::path &path,
    std::uint64_t offset,
    std::uint64_t byte_count,
    HashedOutputFile &output,
    FileCopyScratch &scratch)
{
  if (byte_count == 0) {
    return true;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input.good()) {
    return false;
  }

  std::uint64_t remaining = byte_count;
  while (remaining != 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(scratch.size())));
    input.read(scratch.data(), chunk);
    const auto read_count = input.gcount();
    if (read_count <= 0) {
      return false;
    }
    output.write_text(std::string_view(scratch.data(), static_cast<std::size_t>(read_count)));
    remaining -= static_cast<std::uint64_t>(read_count);
  }
  return true;
}

void remember_rewritten_digest(
    const fs::path &bundle_root,
    const fs::path &relative,
    FileDigestCache &digest_cache,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests,
    Stats &stats)
{
  const auto absolute = bundle_root / relative;
  std::error_code error;
  const auto size = static_cast<std::uint64_t>(fs::file_size(absolute, error));
  if (error) {
    return;
  }
  constexpr std::uint64_t kRewrittenDigestInlineLimit = 64ull * 1024ull * 1024ull;
  if (size > kRewrittenDigestInlineLimit) {
    return;
  }
  const auto digest = digest_cache.digest_file(absolute);
  rewritten_digests[relative.generic_string()] = {digest, size};
  ++stats.rewritten_digest_files;
}

void remember_rewritten_digest(
    const fs::path &bundle_root,
    const fs::path &relative,
    const std::pair<std::string, std::uint64_t> &digest_and_size,
    FileDigestCache &digest_cache,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests,
    Stats &stats)
{
  if (digest_and_size.first.empty()) {
    return;
  }
  rewritten_digests[relative.generic_string()] = digest_and_size;
  digest_cache.remember(bundle_root / relative, digest_and_size.first, digest_and_size.second);
  ++stats.rewritten_digest_files;
}

std::vector<fs::path> text_reference_files(const fs::path &bundle_root)
{
  std::vector<fs::path> files;
  const fs::path entries[] = {
      fs::path(apitrace::trace::kCallstreamFileName),
      fs::path(apitrace::trace::kMetalCallstreamFileName),
      fs::path(apitrace::trace::kAnalysisDirectoryName) / apitrace::trace::kTranslationLinksFileName,
  };
  for (const auto &entry : entries) {
    if (fs::is_regular_file(bundle_root / entry)) {
      files.push_back(entry);
    }
  }

  for (const auto &pipeline_dir : {bundle_root / "pipelines", bundle_root / "metal" / "pipelines"}) {
    std::error_code error;
    if (!fs::is_directory(pipeline_dir, error) || error) {
      continue;
    }
    for (fs::directory_iterator it(pipeline_dir, error), end; it != end && !error; it.increment(error)) {
      if (!it->is_regular_file(error) || error) {
        continue;
      }
      const auto relative = fs::relative(it->path(), bundle_root, error);
      if (error) {
        continue;
      }
      files.push_back(relative);
    }
  }
  return files;
}

bool is_pipeline_reference_file(const fs::path &relative)
{
  const auto generic = relative.generic_string();
  return generic.rfind("pipelines/", 0) == 0 ||
         generic.rfind("metal/pipelines/", 0) == 0;
}

bool rewrite_blob_refs(json &node, const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap);
std::vector<std::string> recover_embedded_jsonl_records(const std::string &line);

std::unordered_set<std::string> rewrite_text_references(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::string> &aliases,
    const std::unordered_map<std::string, std::uint64_t> &blob_id_by_effective_path,
    const std::unordered_map<std::uint64_t, std::string> &effective_path_by_blob_id,
    const std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint64_t>> &blob_id_remap_by_path,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap,
    const Options &options,
    Stats &stats,
    ProgressReporter *progress,
    std::unordered_set<std::string> *referenced_paths = nullptr,
    FileDigestCache *digest_cache = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> *rewritten_digests = nullptr)
{
  std::unordered_set<std::string> rewritten_paths;
  if (aliases.empty() &&
      effective_path_by_blob_id.empty() &&
      blob_id_remap_by_path.empty() &&
      blob_id_remap.empty()) {
    return rewritten_paths;
  }

  std::unordered_set<std::string> blob_id_remap_paths;
  for (const auto &[_, by_path] : blob_id_remap_by_path) {
    for (const auto &[path, remapped_blob_id] : by_path) {
      if (remapped_blob_id != 0) {
        blob_id_remap_paths.insert(path);
      }
    }
  }
  const auto reference_files = text_reference_files(bundle_root);
  std::uint64_t file_index = 0;
  std::uint64_t total_bytes = 0;
  for (const auto &relative : reference_files) {
    std::error_code size_error;
    total_bytes += static_cast<std::uint64_t>(fs::file_size(bundle_root / relative, size_error));
  }
  std::uint64_t processed_bytes = 0;
  for (const auto &relative : reference_files) {
    const auto relative_text = relative.generic_string();
    const auto absolute = bundle_root / relative;
    std::error_code size_error;
    const auto file_size = static_cast<std::uint64_t>(fs::file_size(absolute, size_error));

    json root;
    if (relative.extension() == ".jsonl") {
      ++stats.jsonl_passes;
      bool changed = false;
      const auto temporary = temporary_rewrite_path(absolute);
      HashedOutputFile output;
      FileCopyScratch copy_scratch;
      bool output_open = false;
      bool rewrite_failed = false;
      auto ensure_output = [&](std::uint64_t prefix_bytes) -> bool {
        if (options.dry_run || output_open) {
          return true;
        }
        output.open(temporary);
        if (!output.is_open()) {
          std::cerr << "warning: failed to open temporary rewrite file " << temporary << "\n";
          return false;
        }
        output_open = true;
        if (!copy_file_prefix_to_output(absolute, prefix_bytes, output, copy_scratch)) {
          output.close();
          output_open = false;
          std::error_code remove_error;
          fs::remove(temporary, remove_error);
          std::cerr << "warning: failed to copy unchanged JSONL prefix from " << absolute << "\n";
          return false;
        }
        return true;
      };
      auto write_original_line = [&](std::string_view output_line) {
        if (!options.dry_run && output_open) {
          output.write_line(output_line);
        }
        note_jsonl_output(stats, output_line);
      };
      auto note_original_batch = [&](const JsonlRewriteScanBatch &batch) {
        stats.output_bytes += batch.byte_size;
        if (!options.dry_run && output_open) {
          if (!copy_file_range_to_output(absolute, batch.offset, batch.byte_size, output, copy_scratch)) {
            rewrite_failed = true;
            std::cerr << "warning: failed to copy unchanged JSONL range from " << absolute << "\n";
            return false;
          }
        }
        return true;
      };
      auto write_changed_line = [&](std::uint64_t prefix_bytes, const std::string &output_line) -> bool {
        if (!options.dry_run) {
          if (!ensure_output(prefix_bytes)) {
            rewrite_failed = true;
            return false;
          }
          output.write_line(output_line);
        }
        note_jsonl_output(stats, output_line);
        return true;
      };
      bool sanitized_jsonl = false;
      bool remapped_blob_refs_file = false;
      std::size_t dropped_lines = 0;
      auto process_jsonl_line = [&](const JsonlLineView &line_view) {
        const auto line = line_view.line;
        const auto tokens = scan_jsonl_line_tokens(line);
        const bool may_have_path_reference = tokens_may_contain_path_reference(tokens);
        const auto path_candidates = may_have_path_reference
            ? scan_jsonl_path_rewrite_candidates(
                  line,
                  aliases,
                  blob_id_remap_paths,
                  !effective_path_by_blob_id.empty() && tokens.asset_token,
                  !blob_id_remap_by_path.empty() && tokens.blob_refs_key)
            : JsonlPathRewriteCandidates{};
        const bool may_rewrite_asset_path = path_candidates.alias_path;
        const bool may_rewrite_single_blob_alias = path_candidates.single_blob_asset_alias;
        const bool may_rewrite_path_mapped_blob_refs = path_candidates.path_mapped_blob_refs;
        const bool may_rewrite_blob_refs =
            (tokens.blob_refs_key || tokens.blob_id_key) &&
            text_may_contain_blob_id_remap(line, blob_id_remap);
        if (!may_rewrite_asset_path &&
            !may_rewrite_single_blob_alias &&
            !may_rewrite_path_mapped_blob_refs &&
            !may_rewrite_blob_refs &&
            !options.verify_jsonl_records) {
          write_original_line(line);
          return true;
        }
        const auto line_text = std::string(line);
        auto record = json::parse(line_text, nullptr, false);
        if (record.is_discarded()) {
          if (may_rewrite_blob_refs || options.verify_jsonl_records) {
            if (!ensure_output(line_view.offset)) {
              rewrite_failed = true;
              return false;
            }
            bool recovered_any = false;
            for (const auto &recovered : recover_embedded_jsonl_records(line_text)) {
              auto recovered_record = json::parse(recovered, nullptr, false);
              if (recovered_record.is_discarded()) {
                continue;
              }
              const bool recovered_blob_refs_changed = rewrite_blob_refs(recovered_record, blob_id_remap);
              const auto output_line = recovered_blob_refs_changed ? recovered_record.dump() : recovered;
              if (!options.dry_run && output_open) {
                output.write_line(output_line);
              }
              note_jsonl_output(stats, output_line);
              if (recovered_blob_refs_changed) {
                remapped_blob_refs_file = true;
                ++stats.rewritten_records;
              }
              recovered_any = true;
            }
            sanitized_jsonl = true;
            changed = true;
            if (!recovered_any) {
              ++dropped_lines;
            }
            return true;
          }
          write_original_line(line);
          return true;
        }
        const bool paths_changed = rewrite_path_refs(record, aliases);
        const bool single_blob_asset_alias_changed =
            rewrite_single_blob_asset_alias_refs(record, effective_path_by_blob_id);
        const bool blob_refs_changed =
            rewrite_blob_refs_for_effective_paths(
                record,
                blob_id_by_effective_path,
                paths_changed || single_blob_asset_alias_changed);
        const bool path_mapped_blob_refs_changed =
            rewrite_blob_refs_by_referenced_paths(record, blob_id_remap_by_path);
        const bool remapped_blob_refs_changed = rewrite_blob_refs(record, blob_id_remap);
        remapped_blob_refs_file = remapped_blob_refs_file || remapped_blob_refs_changed;
        if (paths_changed ||
            single_blob_asset_alias_changed ||
            blob_refs_changed ||
            path_mapped_blob_refs_changed ||
            remapped_blob_refs_changed) {
          if (referenced_paths) {
            collect_path_references_from_json(record, aliases, *referenced_paths, false);
          }
          const auto rewritten = record.dump();
          if (!write_changed_line(line_view.offset, rewritten)) {
            return false;
          }
          changed = true;
          if (blob_refs_changed || path_mapped_blob_refs_changed || remapped_blob_refs_changed) {
            ++stats.rewritten_primary_blob_ref_records;
          }
          ++stats.rewritten_records;
        } else {
          if (referenced_paths) {
            collect_path_references_from_json(record, aliases, *referenced_paths, false);
          }
          write_original_line(line);
        }
        return true;
      };
      bool scanned_jsonl = false;
      if (options.verify_jsonl_records) {
        scanned_jsonl = scan_jsonl_file(absolute, [&](const JsonlLineView &line_view) {
          ++stats.jsonl_records;
          stats.input_bytes += line_view.byte_size;
          return process_jsonl_line(line_view);
        });
      } else {
        scanned_jsonl =
            scan_jsonl_file_for_rewrite(
                absolute,
                blob_id_remap,
                [&](const JsonlRewriteScanBatch &batch) {
                  stats.jsonl_records += batch.line_count;
                  stats.input_bytes += batch.byte_size;
                  if (!batch.has_candidate) {
                    return note_original_batch(batch);
                  }
                  return process_jsonl_line(batch.candidate);
                });
      }
      if (!scanned_jsonl) {
        ++file_index;
        continue;
      }
      if (rewrite_failed) {
        if (!options.dry_run && output_open) {
          output.close();
          std::error_code remove_error;
          fs::remove(temporary, remove_error);
        }
        ++file_index;
        processed_bytes += size_error ? 0 : file_size;
        if (progress) {
          progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
        }
        continue;
      }
      if (sanitized_jsonl) {
        ++stats.sanitized_jsonl_files;
        stats.dropped_jsonl_lines += dropped_lines;
      }
      if (remapped_blob_refs_file) {
        ++stats.rewritten_blob_ref_files;
      }
      if (!changed) {
        if (!options.dry_run && output_open) {
          output.close();
          std::error_code remove_error;
          fs::remove(temporary, remove_error);
        }
        ++file_index;
        processed_bytes += size_error ? 0 : file_size;
        if (progress) {
          progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
        }
        continue;
      }
      ++stats.rewritten_text_files;
      rewritten_paths.insert(relative_text);
      if (!options.dry_run) {
        if (!output_open) {
          ++file_index;
          processed_bytes += size_error ? 0 : file_size;
          if (progress) {
            progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
          }
          continue;
        }
        const auto digest_and_size = output.digest_and_size();
        output.close();
        if (!replace_with_temporary_file(absolute, temporary)) {
          rewritten_paths.erase(relative_text);
        } else if (digest_cache && rewritten_digests) {
          remember_rewritten_digest(
              bundle_root,
              relative,
              digest_and_size,
              *digest_cache,
              *rewritten_digests,
              stats);
        }
      }
      ++file_index;
      processed_bytes += size_error ? 0 : file_size;
      if (progress) {
        progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
      }
      continue;
    }

    std::ifstream input(absolute, std::ios::binary);
    if (!input.is_open()) {
      ++file_index;
      continue;
    }
    std::ostringstream text_buffer;
    text_buffer << input.rdbuf();
    const auto text = text_buffer.str();
    const bool pipeline_reference_file = is_pipeline_reference_file(relative);
    const bool may_rewrite_path_mapped_blob_refs =
        !blob_id_remap_by_path.empty() &&
        text_may_contain_blob_refs(text) &&
        text_may_contain_path_reference(text);
    const bool may_rewrite_blob_refs =
        text_may_contain_blob_id_remap(text, blob_id_remap);
    if (!pipeline_reference_file &&
        !text_may_contain_rewritable_asset_path(text) &&
        !may_rewrite_path_mapped_blob_refs &&
        !may_rewrite_blob_refs) {
      ++file_index;
      processed_bytes += size_error ? 0 : file_size;
      if (progress) {
        progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
      }
      continue;
    }
    root = json::parse(text, nullptr, false);
    const bool paths_changed = !root.is_discarded() && rewrite_path_refs(root, aliases);
    const bool exact_alias_values_changed =
        pipeline_reference_file &&
        !root.is_discarded() &&
        rewrite_exact_alias_string_values(root, aliases);
    const bool single_blob_asset_alias_changed =
        !root.is_discarded() && rewrite_single_blob_asset_alias_refs(root, effective_path_by_blob_id);
    const bool blob_refs_changed =
        !root.is_discarded() &&
        rewrite_blob_refs_for_effective_paths(
            root,
            blob_id_by_effective_path,
            paths_changed || exact_alias_values_changed || single_blob_asset_alias_changed);
    const bool path_mapped_blob_refs_changed =
        !root.is_discarded() &&
        rewrite_blob_refs_by_referenced_paths(root, blob_id_remap_by_path);
    const bool remapped_blob_refs_changed =
        !root.is_discarded() &&
        rewrite_blob_refs(root, blob_id_remap);
    if (root.is_discarded() ||
        (!paths_changed && !exact_alias_values_changed && !single_blob_asset_alias_changed &&
         !blob_refs_changed && !path_mapped_blob_refs_changed && !remapped_blob_refs_changed)) {
      if (referenced_paths && !root.is_discarded()) {
        collect_path_references_from_json(root, aliases, *referenced_paths, false);
      }
      ++file_index;
      processed_bytes += size_error ? 0 : file_size;
      if (progress) {
        progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
      }
      continue;
    }
    ++stats.rewritten_text_files;
    rewritten_paths.insert(relative_text);
    if (referenced_paths) {
      collect_path_references_from_json(root, aliases, *referenced_paths, false);
    }
    if (blob_refs_changed || path_mapped_blob_refs_changed || remapped_blob_refs_changed) {
      ++stats.rewritten_primary_blob_ref_records;
    }
    if (!options.dry_run) {
      HashedOutputFile output;
      output.open(absolute);
      const auto rewritten = root.dump();
      output.write_line(rewritten);
      const auto digest_and_size = output.digest_and_size();
      output.close();
      if (digest_cache && rewritten_digests) {
        remember_rewritten_digest(
            bundle_root,
            relative,
            digest_and_size,
            *digest_cache,
            *rewritten_digests,
            stats);
      }
    }
    ++file_index;
    processed_bytes += size_error ? 0 : file_size;
    if (progress) {
      progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
    }
  }
  return rewritten_paths;
}

bool rewrite_blob_refs(json &node, const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap)
{
  bool changed = false;
  if (blob_id_remap.empty()) {
    return false;
  }
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it.key() == "blob_refs" && it.value().is_array()) {
        for (auto &ref : it.value()) {
          if (!ref.is_number_unsigned()) {
            continue;
          }
          const auto found = blob_id_remap.find(ref.get<std::uint64_t>());
          if (found == blob_id_remap.end()) {
            continue;
          }
          ref = found->second;
          changed = true;
        }
      } else if (it.key() == "blob_id" && it.value().is_number_unsigned()) {
        const auto found = blob_id_remap.find(it.value().get<std::uint64_t>());
        if (found != blob_id_remap.end()) {
          it.value() = found->second;
          changed = true;
        }
      } else {
        changed = rewrite_blob_refs(it.value(), blob_id_remap) || changed;
      }
    }
  } else if (node.is_array()) {
    for (auto &item : node) {
      changed = rewrite_blob_refs(item, blob_id_remap) || changed;
    }
  }
  return changed;
}

bool append_sanitized_jsonl_record(
    const std::string &line,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap,
    HashedOutputFile *output,
    bool dry_run,
    Stats &stats,
    bool &remapped_blob_refs)
{
  auto record = json::parse(line, nullptr, false);
  if (record.is_discarded()) {
    return false;
  }

  if (rewrite_blob_refs(record, blob_id_remap)) {
    const auto rewritten = record.dump();
    if (output && !dry_run) {
      output->write_line(rewritten);
    }
    note_jsonl_output(stats, rewritten);
    remapped_blob_refs = true;
    ++stats.rewritten_records;
  } else {
    if (output && !dry_run) {
      output->write_line(line);
    }
    note_jsonl_output(stats, line);
  }
  return true;
}

std::vector<std::string> recover_embedded_jsonl_records(const std::string &line)
{
  std::vector<std::string> recovered;
  const char *const prefixes[] = {
      "{\"record_kind\"",
      "{\"call_kind\"",
  };

  for (const auto *prefix : prefixes) {
    std::size_t cursor = 1;
    while (cursor < line.size()) {
      const auto found = line.find(prefix, cursor);
      if (found == std::string::npos) {
        break;
      }
      cursor = found + 1;

      const auto candidate = line.substr(found);
      if (json::parse(candidate, nullptr, false).is_discarded()) {
        continue;
      }
      recovered.push_back(candidate);
      break;
    }
  }
  return recovered;
}

void sanitize_and_rewrite_jsonl_file(
    const fs::path &path,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap,
    const Options &options,
    Stats &stats,
    ProgressReporter *progress = nullptr,
    std::uint64_t file_index = 0,
    std::uint64_t file_count = 0,
    FileDigestCache *digest_cache = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> *rewritten_digests = nullptr)
{
  if (!fs::is_regular_file(path)) {
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }
  if (blob_id_remap.empty()) {
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }

  std::ifstream input(path, std::ios::binary);
  ++stats.jsonl_passes;
  if (!input.is_open()) {
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }

  const auto temporary = temporary_rewrite_path(path);
  HashedOutputFile output;
  if (!options.dry_run) {
    output.open(temporary);
    if (!output.is_open()) {
      std::cerr << "warning: failed to open temporary rewrite file " << temporary << "\n";
      return;
    }
  }

  std::string line;
  bool changed = false;
  bool remapped_blob_refs = false;
  std::size_t dropped_lines = 0;
  while (std::getline(input, line)) {
    ++stats.jsonl_records;
    stats.input_bytes += static_cast<std::uint64_t>(line.size()) + 1;
    if (append_sanitized_jsonl_record(line, blob_id_remap, options.dry_run ? nullptr : &output, options.dry_run, stats, remapped_blob_refs)) {
      changed = changed || remapped_blob_refs;
      continue;
    }

    bool recovered_any = false;
    for (const auto &recovered : recover_embedded_jsonl_records(line)) {
      recovered_any =
          append_sanitized_jsonl_record(
              recovered,
              blob_id_remap,
              options.dry_run ? nullptr : &output,
              options.dry_run,
              stats,
              remapped_blob_refs) ||
          recovered_any;
    }
    changed = true;
    if (!recovered_any) {
      ++dropped_lines;
    }
  }

  if (!changed) {
    if (!options.dry_run) {
      output.close();
      std::error_code remove_error;
      fs::remove(temporary, remove_error);
    }
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }
  ++stats.sanitized_jsonl_files;
  stats.dropped_jsonl_lines += dropped_lines;
  if (remapped_blob_refs) {
    ++stats.rewritten_blob_ref_files;
  }
  if (options.dry_run) {
    return;
  }

  const auto digest_and_size = output.digest_and_size();
  output.close();
  if (replace_with_temporary_file(path, temporary) && digest_cache && rewritten_digests) {
    const auto relative = fs::relative(path, options.bundle_root);
    remember_rewritten_digest(
        options.bundle_root,
        relative,
        digest_and_size,
        *digest_cache,
        *rewritten_digests,
        stats);
  }
  if (progress && file_count != 0) {
    progress->update(file_index, file_count, 0, 0);
  }
}

void sanitize_jsonl_tail_file(
    const fs::path &path,
    const Options &options,
    Stats &stats,
    ProgressReporter *progress = nullptr,
    std::uint64_t file_index = 0,
    std::uint64_t file_count = 0)
{
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size == 0) {
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }

  input.seekg(-1, std::ios::end);
  char last = '\0';
  input.get(last);
  if (last != '\n') {
    constexpr std::uint64_t kChunkSize = 1024 * 1024;
    std::uint64_t cursor = size;
    std::uint64_t truncate_at = 0;
    std::string chunk;
    while (cursor > 0) {
      const auto read_size = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkSize, cursor));
      cursor -= read_size;
      chunk.resize(read_size);
      input.seekg(static_cast<std::streamoff>(cursor), std::ios::beg);
      input.read(chunk.data(), static_cast<std::streamsize>(read_size));
      const auto found = chunk.find_last_of('\n');
      if (found != std::string::npos) {
        truncate_at = cursor + found + 1;
        break;
      }
    }
    if (truncate_at < size) {
      ++stats.sanitized_jsonl_files;
      ++stats.dropped_jsonl_lines;
      if (!options.dry_run) {
        fs::resize_file(path, truncate_at, error);
        if (error) {
          std::cerr << "warning: failed to truncate partial JSONL tail " << path
                    << ": " << error.message() << "\n";
        }
      }
    }
    if (progress && file_count != 0) {
      progress->update(file_index, file_count, 0, 0);
    }
    return;
  }

  constexpr std::uint64_t kTailWindow = 4ull * 1024ull * 1024ull;
  const auto read_start = size > kTailWindow ? size - kTailWindow : 0;
  std::string tail(static_cast<std::size_t>(size - read_start), '\0');
  input.seekg(static_cast<std::streamoff>(read_start), std::ios::beg);
  input.read(tail.data(), static_cast<std::streamsize>(tail.size()));

  auto line_end = tail.size();
  while (line_end > 0 && tail[line_end - 1] == '\n') {
    --line_end;
  }
  if (line_end == 0) {
    return;
  }
  const auto previous_newline = tail.rfind('\n', line_end - 1);
  const auto line_start = previous_newline == std::string::npos ? 0 : previous_newline + 1;
  const auto line = tail.substr(line_start, line_end - line_start);
  if (json::parse(line, nullptr, false).is_discarded()) {
    const auto truncate_at = read_start + line_start;
    ++stats.sanitized_jsonl_files;
    ++stats.dropped_jsonl_lines;
    if (!options.dry_run) {
      fs::resize_file(path, truncate_at, error);
      if (error) {
        std::cerr << "warning: failed to truncate invalid JSONL tail " << path
                  << ": " << error.message() << "\n";
      }
    }
  }
  if (progress && file_count != 0) {
    progress->update(file_index, file_count, 0, 0);
  }
}

std::uint64_t max_blob_id(const std::vector<AssetEntry> &assets)
{
  std::uint64_t max_id = 0;
  for (const auto &asset : assets) {
    max_id = std::max(max_id, asset.blob_id);
  }
  return max_id;
}

std::unordered_map<std::uint64_t, std::string> build_effective_path_by_blob_id(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::uint64_t, std::string> by_blob_id;
  for (const auto &asset : assets) {
    if (asset.blob_id != 0) {
      by_blob_id.emplace(asset.blob_id, effective_asset_path(asset));
    }
  }
  return by_blob_id;
}

std::unordered_map<std::uint64_t, AssetEntry> build_asset_by_blob_id(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::uint64_t, AssetEntry> by_blob_id;
  for (const auto &asset : assets) {
    if (asset.blob_id != 0) {
      by_blob_id.emplace(asset.blob_id, asset);
    }
  }
  return by_blob_id;
}

bool asset_payload_materialized(const AssetEntry &asset)
{
  if (asset.file_exists) {
    return true;
  }
  return !asset.payload_path.empty() && asset.payload_slice_exists;
}

struct CallstreamPrefixResult {
  bool truncated = false;
  bool failed = false;
  std::uint64_t kept_frame_count = 0;
  std::uint64_t dropped_frame_count = 0;
  std::uint64_t first_dropped_frame = 0;
  std::string error;
};

bool json_u64_value(const json &node, std::uint64_t &value)
{
  if (node.is_number_unsigned()) {
    value = node.get<std::uint64_t>();
    return true;
  }
  if (node.is_number_integer()) {
    const auto signed_value = node.get<std::int64_t>();
    if (signed_value < 0) {
      return false;
    }
    value = static_cast<std::uint64_t>(signed_value);
    return true;
  }
  return false;
}

bool json_u64_field(const json &node, const char *key, std::uint64_t &value)
{
  const auto it = node.find(key);
  return it != node.end() && json_u64_value(*it, value);
}

std::uint64_t json_u64_or_max(const json &node, const char *key)
{
  std::uint64_t value = 0;
  if (json_u64_field(node, key, value)) {
    return value;
  }
  return std::numeric_limits<std::uint64_t>::max();
}

bool payload_metadata_match(const json &lhs, const json &rhs)
{
  for (const auto *key : {"frame_index", "sync_interval", "flags"}) {
    if (!lhs.contains(key) || !rhs.contains(key) ||
        !(lhs[key].is_number_unsigned() || lhs[key].is_number_integer()) ||
        !(rhs[key].is_number_unsigned() || rhs[key].is_number_integer()) ||
        lhs[key].get<std::uint64_t>() != rhs[key].get<std::uint64_t>()) {
      return false;
    }
  }
  return true;
}

bool validate_record_asset_refs(
    const fs::path &bundle_root,
    const std::unordered_map<std::uint64_t, AssetEntry> &asset_by_blob_id,
    const json &record,
    bool has_blob_refs,
    std::string &reason,
    bool &recoverable_by_rewrite)
{
  recoverable_by_rewrite = false;
  if (!record.is_object() || !has_blob_refs) {
    return true;
  }

  std::vector<std::uint64_t> blob_refs;
  std::vector<std::string> paths;
  collect_asset_references_from_json(record, blob_refs, paths);
  for (const auto blob_id : blob_refs) {
    const auto asset_it = asset_by_blob_id.find(blob_id);
    if (asset_it == asset_by_blob_id.end()) {
      reason = "missing blob_id " + std::to_string(blob_id);
      return false;
    }
    if (!asset_payload_materialized(asset_it->second)) {
      reason = "missing payload for blob_id " + std::to_string(blob_id);
      return false;
    }
  }

  const bool refs_and_paths_are_paired = blob_refs.size() == paths.size();
  for (std::size_t index = 0; index < paths.size(); ++index) {
    const fs::path relative_path(paths[index]);
    if (!safe_relative_path(relative_path)) {
      reason = "unsafe asset path " + paths[index];
      return false;
    }
  }

  if (!refs_and_paths_are_paired) {
    return true;
  }

  for (std::size_t index = 0; index < blob_refs.size() && index < paths.size(); ++index) {
    const auto asset_it = asset_by_blob_id.find(blob_refs[index]);
    if (asset_it == asset_by_blob_id.end()) {
      continue;
    }
    const auto expected_path = effective_asset_path(asset_it->second);
    if (!expected_path.empty() && expected_path != paths[index] && asset_it->second.path != paths[index]) {
      reason = "blob_id " + std::to_string(blob_refs[index]) + " path mismatch";
      recoverable_by_rewrite = true;
      return false;
    }
    const fs::path relative_path(paths[index]);
    if (!fs::is_regular_file(bundle_root / relative_path) && !asset_payload_materialized(asset_it->second)) {
      reason = "missing asset path " + paths[index];
      return false;
    }
  }
  return true;
}

bool validate_line_blob_refs_fast(
    std::string_view line,
    bool has_blob_refs,
    const std::unordered_map<std::uint64_t, AssetEntry> &asset_by_blob_id,
    std::string &reason)
{
  if (!has_blob_refs) {
    return true;
  }

  std::size_t search = 0;
  while ((search = line.find("\"blob_refs\"", search)) != std::string_view::npos) {
    const auto colon = line.find(':', search + 11);
    if (colon == std::string_view::npos) {
      reason = "malformed blob_refs";
      return false;
    }
    auto cursor = skip_json_whitespace(line, colon + 1);
    if (cursor >= line.size() || line[cursor] != '[') {
      reason = "malformed blob_refs";
      return false;
    }
    ++cursor;

    for (;;) {
      cursor = skip_json_whitespace(line, cursor);
      if (cursor >= line.size()) {
        reason = "malformed blob_refs";
        return false;
      }
      if (line[cursor] == ']') {
        ++cursor;
        break;
      }
      if (line[cursor] == ',') {
        ++cursor;
        continue;
      }

      std::uint64_t blob_id = 0;
      std::size_t after = cursor;
      if (!parse_json_unsigned_at(line, cursor, blob_id, after)) {
        reason = "malformed blob_refs";
        return false;
      }

      const auto asset_it = asset_by_blob_id.find(blob_id);
      if (asset_it == asset_by_blob_id.end()) {
        reason = "missing blob_id " + std::to_string(blob_id);
        return false;
      }
      if (!asset_payload_materialized(asset_it->second)) {
        reason = "missing payload for blob_id " + std::to_string(blob_id);
        return false;
      }
      cursor = after;
    }

    search = cursor;
  }
  return true;
}

bool extract_payload(const json &record, json &payload)
{
  const auto payload_it = record.find("payload");
  if (payload_it == record.end()) {
    payload = json::object();
    return true;
  }
  if (!payload_it->is_object()) {
    return false;
  }
  payload = *payload_it;
  return true;
}

bool line_may_contain_present_progress_marker(std::string_view line)
{
  if (line.find("\"frame_index\"") == std::string::npos) {
    return false;
  }
  return line.find("\"boundary\":\"Present\"") != std::string::npos ||
         line.find("\"boundary\": \"Present\"") != std::string::npos ||
         line.find("\"boundary\":\"Frame\"") != std::string::npos ||
         line.find("\"boundary\": \"Frame\"") != std::string::npos ||
         line.find("D3D11PresentFrame") != std::string::npos ||
         line.find("D3D12PresentFrame") != std::string::npos ||
         line.find("IDXGISwapChain::Present") != std::string::npos ||
         line.find("IDXGISwapChain1::Present") != std::string::npos;
}

std::optional<std::uint64_t> frame_index_from_line(std::string_view line)
{
  const auto key = line.find("\"frame_index\"");
  if (key == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = line.find(':', key + 13);
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  std::size_t value = colon + 1;
  while (value < line.size() && line[value] == ' ') {
    ++value;
  }
  if (value >= line.size() || line[value] < '0' || line[value] > '9') {
    return std::nullopt;
  }
  std::uint64_t frame_index = 0;
  while (value < line.size() && line[value] >= '0' && line[value] <= '9') {
    frame_index = (frame_index * 10) + static_cast<std::uint64_t>(line[value] - '0');
    ++value;
  }
  return frame_index;
}

json parse_jsonl_line(std::string_view line)
{
  return json::parse(line.begin(), line.end(), nullptr, false);
}

std::optional<std::uint64_t> json_u64_value_optional(const json &value)
{
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value >= 0) {
      return static_cast<std::uint64_t>(signed_value);
    }
  }
  return std::nullopt;
}

bool add_u64_checked(std::uint64_t value, std::uint64_t delta, std::uint64_t &out)
{
  if (delta > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  out = value + delta;
  return true;
}

bool remap_json_sequence_value(json &value, std::uint64_t delta)
{
  const auto original = json_u64_value_optional(value);
  if (!original || *original == 0) {
    return false;
  }
  std::uint64_t remapped = 0;
  if (!add_u64_checked(*original, delta, remapped)) {
    return false;
  }
  if (remapped == *original) {
    return false;
  }
  value = remapped;
  return true;
}

bool remap_column_sequences(json &node, std::uint64_t delta)
{
  if (!node.is_object()) {
    return false;
  }
  const auto columns_it = node.find("columns");
  if (columns_it == node.end() || !columns_it->is_array()) {
    return false;
  }

  std::vector<std::size_t> sequence_columns;
  for (std::size_t index = 0; index < columns_it->size(); ++index) {
    const auto &column = (*columns_it)[index];
    if (!column.is_string()) {
      continue;
    }
    const auto name = column.get<std::string>();
    if (name == "sequence" || name == "d3d_sequence") {
      sequence_columns.push_back(index);
    }
  }
  if (sequence_columns.empty()) {
    return false;
  }

  bool changed = false;
  auto remap_rows = [&](json &rows) {
    if (!rows.is_array()) {
      return;
    }
    for (auto &row : rows) {
      if (!row.is_array()) {
        continue;
      }
      for (const auto column_index : sequence_columns) {
        if (column_index < row.size()) {
          changed = remap_json_sequence_value(row[column_index], delta) || changed;
        }
      }
    }
  };

  if (auto rows = node.find("ops"); rows != node.end()) {
    remap_rows(*rows);
  }
  if (auto rows = node.find("barriers"); rows != node.end()) {
    remap_rows(*rows);
  }
  if (auto rows = node.find("records"); rows != node.end()) {
    remap_rows(*rows);
  }
  return changed;
}

bool remap_embedded_sequence_fields(json &node, std::uint64_t delta, bool is_payload = false)
{
  bool changed = false;
  if (node.is_object()) {
    changed = remap_column_sequences(node, delta) || changed;
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (it.key() == "sequence" || it.key() == "d3d_sequence") {
        changed = remap_json_sequence_value(it.value(), delta) || changed;
        continue;
      }
      changed = remap_embedded_sequence_fields(it.value(), delta, is_payload || it.key() == "payload") || changed;
    }
  } else if (node.is_array() && is_payload) {
    for (auto &item : node) {
      changed = remap_embedded_sequence_fields(item, delta, true) || changed;
    }
  }
  return changed;
}

std::optional<std::uint64_t> line_sequence_fast(std::string_view line)
{
  constexpr std::string_view key = "sequence";
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }

    if (c == '"') {
      if (depth == 1 &&
          i + key.size() + 2 <= line.size() &&
          line.compare(i + 1, key.size(), key) == 0 &&
          line[i + key.size() + 1] == '"') {
        const auto colon = skip_json_whitespace(line, i + key.size() + 2);
        if (colon < line.size() && line[colon] == ':') {
          std::uint64_t value = 0;
          std::size_t after = 0;
          if (parse_json_unsigned_at(line, skip_json_whitespace(line, colon + 1), value, after)) {
            return value;
          }
        }
      }
      in_string = true;
      escaped = false;
      continue;
    }

    if (c == '{' || c == '[') {
      ++depth;
    } else if ((c == '}' || c == ']') && depth > 0) {
      --depth;
    }
  }
  return std::nullopt;
}

std::unordered_set<std::string> repair_callstream_sequence_regressions(
    const fs::path &bundle_root,
    const Options &options,
    Stats &stats,
    FileDigestCache &digest_cache,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests)
{
  std::unordered_set<std::string> rewritten_paths;
  const auto callstream_path = bundle_root / apitrace::trace::kCallstreamFileName;
  if (!fs::is_regular_file(callstream_path)) {
    return rewritten_paths;
  }

  bool needs_rewrite = false;
  std::uint64_t last_local_sequence = 0;
  ++stats.jsonl_passes;
  const bool detected = scan_jsonl_file(callstream_path, [&](const JsonlLineView &line_view) {
    ++stats.jsonl_records;
    stats.input_bytes += line_view.byte_size;
    const auto sequence = line_sequence_fast(line_view.line);
    if (!sequence || *sequence == 0) {
      return true;
    }
    if (*sequence < last_local_sequence) {
      needs_rewrite = true;
      return false;
    }
    last_local_sequence = *sequence;
    return true;
  });
  (void)detected;

  if (!needs_rewrite) {
    return rewritten_paths;
  }

  bool rewrite_failed = false;
  std::uint64_t last_global_sequence = 0;
  std::uint64_t segment_base = 0;
  last_local_sequence = 0;

  const auto temporary = temporary_rewrite_path(callstream_path);
  HashedOutputFile output;
  if (!options.dry_run) {
    output.open(temporary);
    if (!output.is_open()) {
      std::cerr << "warning: failed to open temporary rewrite file " << temporary << "\n";
      return rewritten_paths;
    }
  }

  ++stats.jsonl_passes;
  const bool scanned = scan_jsonl_file(callstream_path, [&](const JsonlLineView &line_view) {
    ++stats.jsonl_records;
    stats.input_bytes += line_view.byte_size;

    const auto local_sequence = line_sequence_fast(line_view.line);
    if (!local_sequence || *local_sequence == 0) {
      if (!options.dry_run) {
        output.write_line(line_view.line);
      }
      note_jsonl_output(stats, line_view.line);
      return true;
    }

    if (*local_sequence < last_local_sequence) {
      segment_base = last_global_sequence;
      last_local_sequence = 0;
      ++stats.sequence_regression_segments;
    }

    std::uint64_t global_sequence = 0;
    if (!add_u64_checked(*local_sequence, segment_base, global_sequence)) {
      rewrite_failed = true;
      std::cerr << "warning: sequence remap overflow in " << callstream_path
                << " at byte offset " << line_view.offset << "\n";
      return false;
    }
    last_local_sequence = *local_sequence;

    if (global_sequence < last_global_sequence) {
      rewrite_failed = true;
      std::cerr << "warning: sequence still regressed after remap in " << callstream_path
                << " at byte offset " << line_view.offset << "\n";
      return false;
    }

    if (global_sequence != *local_sequence) {
      auto record = parse_jsonl_line(line_view.line);
      if (record.is_discarded() || !record.is_object()) {
        rewrite_failed = true;
        std::cerr << "warning: failed to parse sequence-remap record in " << callstream_path
                  << " at byte offset " << line_view.offset << "\n";
        return false;
      }
      record["sequence"] = global_sequence;
      if (auto payload = record.find("payload"); payload != record.end()) {
        remap_embedded_sequence_fields(*payload, segment_base, true);
      }
      const auto rewritten = record.dump();
      if (!options.dry_run) {
        output.write_line(rewritten);
      }
      note_jsonl_output(stats, rewritten);
      ++stats.remapped_sequence_records;
    } else {
      if (!options.dry_run) {
        output.write_line(line_view.line);
      }
      note_jsonl_output(stats, line_view.line);
    }

    last_global_sequence = std::max(last_global_sequence, global_sequence);
    return true;
  });

  if (!scanned || rewrite_failed) {
    if (!options.dry_run) {
      output.close();
      std::error_code remove_error;
      fs::remove(temporary, remove_error);
    }
    return rewritten_paths;
  }

  ++stats.rewritten_text_files;
  rewritten_paths.insert(apitrace::trace::kCallstreamFileName);
  if (!options.dry_run) {
    const auto digest_and_size = output.digest_and_size();
    output.close();
    if (!replace_with_temporary_file(callstream_path, temporary)) {
      rewritten_paths.clear();
    } else {
      remember_rewritten_digest(
          bundle_root,
          fs::path(apitrace::trace::kCallstreamFileName),
          digest_and_size,
          digest_cache,
          rewritten_digests,
          stats);
    }
  }
  return rewritten_paths;
}

bool line_may_contain_callstream_prefix_state(std::string_view line, const JsonlLineTokens &tokens)
{
  (void)tokens;
  return line_may_contain_present_progress_marker(line);
}

CallstreamPrefixResult truncate_callstream_to_complete_prefix(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const Options &options,
    Stats &stats,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests)
{
  CallstreamPrefixResult result;
  const auto callstream_path = bundle_root / apitrace::trace::kCallstreamFileName;
  if (!fs::is_regular_file(callstream_path)) {
    return result;
  }

  const auto asset_by_blob_id = build_asset_by_blob_id(assets);
  ++stats.jsonl_passes;

  std::uint64_t line_number = 0;
  std::uint64_t header_end_offset = 0;
  std::uint64_t last_complete_offset = 0;
  std::uint64_t last_complete_frame = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t last_present_boundary_offset = 0;
  std::uint64_t last_present_boundary_frame = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t last_present_frame_asset_offset = 0;
  std::uint64_t last_present_frame_asset_frame = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t next_frame_begin = 0;
  std::uint64_t next_frame_end = 0;
  std::unordered_map<std::uint64_t, json> present_calls;
  std::unordered_set<std::uint64_t> present_boundaries;
  std::unordered_set<std::uint64_t> open_frames;
  bool saw_frame_boundary = false;
  bool inconsistent = false;
  std::string inconsistent_reason;
  std::uint64_t inconsistent_sequence = 0;
  std::unordered_set<std::uint64_t> later_progress_frames;
  auto complete_frame_offset = [&]() {
    if (saw_frame_boundary) {
      return last_complete_offset;
    }
    if (last_present_frame_asset_offset != 0) {
      return last_present_frame_asset_offset;
    }
    return last_present_boundary_offset;
  };
  auto complete_frame_index = [&]() {
    if (saw_frame_boundary) {
      return last_complete_frame;
    }
    if (last_present_frame_asset_offset != 0) {
      return last_present_frame_asset_frame;
    }
    return last_present_boundary_frame;
  };
  auto mark_inconsistent = [&](std::string reason, std::uint64_t sequence) {
    inconsistent = true;
    inconsistent_reason = std::move(reason);
    inconsistent_sequence = sequence;
    const auto kept_frame = complete_frame_index();
    result.kept_frame_count = kept_frame == std::numeric_limits<std::uint64_t>::max() ? 0 : kept_frame + 1;
    return true;
  };
  const bool scanned = scan_jsonl_file(callstream_path, [&](const JsonlLineView &line_view) {
    const auto line = line_view.line;
    const auto line_end_offset = line_view.offset + line_view.byte_size;
    ++line_number;
    ++stats.jsonl_records;
    stats.input_bytes += line_view.byte_size;

    if (inconsistent) {
      const auto frame_index = frame_index_from_line(line);
      if (frame_index &&
          *frame_index >= result.kept_frame_count &&
          line_may_contain_present_progress_marker(line)) {
        later_progress_frames.insert(*frame_index);
      }
      return true;
    }

    if (line.empty()) {
      return true;
    }

    const auto tokens = scan_jsonl_line_tokens(line);
    if (!options.verify_jsonl_records && tokens.blob_refs_key) {
      if (!validate_line_blob_refs_fast(line, true, asset_by_blob_id, inconsistent_reason)) {
        return mark_inconsistent(inconsistent_reason, line_sequence_fast(line).value_or(0));
      }
    }
    if (line_number != 1 &&
        !options.verify_jsonl_records &&
        !line_may_contain_callstream_prefix_state(line, tokens)) {
      return true;
    }

    const auto record = parse_jsonl_line(line);
    if (record.is_discarded() || !record.is_object()) {
      if (!options.verify_jsonl_records && !line_may_contain_callstream_prefix_state(line, tokens)) {
        return true;
      }
      return mark_inconsistent("invalid JSON at line " + std::to_string(line_number), 0);
    }

    bool recoverable_by_rewrite = false;
    if (options.verify_jsonl_records &&
        !validate_record_asset_refs(
            bundle_root,
            asset_by_blob_id,
            record,
            tokens.blob_refs_key,
            inconsistent_reason,
            recoverable_by_rewrite)) {
      if (recoverable_by_rewrite) {
        return true;
      }
      return mark_inconsistent(inconsistent_reason, record.value("sequence", 0ull));
    }

    const auto record_kind = record.value("record_kind", std::string());
    if (line_number == 1 && record_kind == "bundle_header") {
      header_end_offset = line_end_offset;
    }

    json payload;
    if (!extract_payload(record, payload)) {
      return mark_inconsistent(
          "payload is not an object at line " + std::to_string(line_number),
          record.value("sequence", 0ull));
    }
    const auto frame_index = json_u64_or_max(payload, "frame_index");
    const auto function = record.value("function", std::string());
    if (record_kind == "call" &&
        (function == "IDXGISwapChain::Present" ||
         function == "IDXGISwapChain1::Present" ||
         function == "IDXGISwapChain1::Present1") &&
        frame_index != std::numeric_limits<std::uint64_t>::max()) {
      present_calls[frame_index] = payload;
    } else if (record_kind == "boundary" &&
               record.value("boundary", std::string()) == "Present" &&
               frame_index != std::numeric_limits<std::uint64_t>::max()) {
      const auto call_it = present_calls.find(frame_index);
      if (call_it == present_calls.end() || !payload_metadata_match(call_it->second, payload)) {
        return mark_inconsistent(
            "Present boundary does not match captured Present call",
            record.value("sequence", 0ull));
      }
      present_boundaries.insert(frame_index);
      if (frame_index == 0 ||
          (last_present_boundary_frame != std::numeric_limits<std::uint64_t>::max() &&
           frame_index == last_present_boundary_frame + 1)) {
        last_present_boundary_frame = frame_index;
        last_present_boundary_offset = line_end_offset;
      }
    } else if (record_kind == "boundary" &&
               record.value("boundary", std::string()) == "Frame" &&
               frame_index != std::numeric_limits<std::uint64_t>::max()) {
      saw_frame_boundary = true;
      const auto label = payload.value("label", std::string());
      if (label == "FrameBegin") {
        if (frame_index == next_frame_begin) {
          open_frames.insert(frame_index);
          ++next_frame_begin;
        }
      } else if (label == "FrameEnd") {
        if (frame_index == next_frame_end &&
            open_frames.find(frame_index) != open_frames.end() &&
            present_boundaries.find(frame_index) != present_boundaries.end()) {
          open_frames.erase(frame_index);
          last_complete_frame = frame_index;
          last_complete_offset = line_end_offset;
          ++next_frame_end;
        }
      }
    } else if (record_kind == "resource_blob" &&
               (record.value("debug_name", std::string()) == "D3D11PresentFrame" ||
                record.value("debug_name", std::string()) == "D3D12PresentFrame") &&
               frame_index != std::numeric_limits<std::uint64_t>::max()) {
      if (present_boundaries.find(frame_index) == present_boundaries.end()) {
        return mark_inconsistent(
            "D3D12PresentFrame precedes Present boundary",
            record.value("sequence", 0ull));
      }
      if (frame_index == 0 ||
          (last_present_frame_asset_frame != std::numeric_limits<std::uint64_t>::max() &&
           frame_index == last_present_frame_asset_frame + 1)) {
        last_present_frame_asset_frame = frame_index;
        last_present_frame_asset_offset = line_end_offset;
      }
    }

    return true;
  });
  if (!scanned) {
    return result;
  }

  if (!inconsistent) {
    const bool incomplete_tail_frame =
        saw_frame_boundary &&
        (next_frame_begin != next_frame_end ||
         !open_frames.empty() ||
         (last_present_boundary_frame != std::numeric_limits<std::uint64_t>::max() &&
          (last_complete_frame == std::numeric_limits<std::uint64_t>::max() ||
           last_present_boundary_frame > last_complete_frame)));
    const auto frame = complete_frame_index();
    result.kept_frame_count = frame == std::numeric_limits<std::uint64_t>::max() ? 0 : frame + 1;
    if (!incomplete_tail_frame) {
      return result;
    }
    const auto target_offset = complete_frame_offset() != 0 ? complete_frame_offset() : header_end_offset;
    std::error_code size_error;
    const auto old_size = fs::file_size(callstream_path, size_error);
    if (size_error || old_size <= target_offset) {
      return result;
    }
    result.truncated = true;
    stats.surviving_frame_count = result.kept_frame_count;
    stats.truncated_inconsistent_jsonl_bytes += static_cast<std::uint64_t>(old_size - target_offset);
    ++stats.truncated_inconsistent_jsonl_files;
    std::cerr << "warning: truncating callstream.jsonl at " << target_offset
              << " bytes after " << result.kept_frame_count
              << " complete frames due to incomplete tail frame\n";
    if (options.dry_run) {
      return result;
    }
    std::error_code resize_error;
    fs::resize_file(callstream_path, target_offset, resize_error);
    if (resize_error) {
      std::cerr << "warning: failed to truncate callstream.jsonl: " << resize_error.message() << "\n";
      result.truncated = false;
      return result;
    }
    rewritten_digests.erase(apitrace::trace::kCallstreamFileName);
    return result;
  }

  const auto kept_frame = complete_frame_index();
  result.kept_frame_count = kept_frame == std::numeric_limits<std::uint64_t>::max() ? 0 : kept_frame + 1;
  result.dropped_frame_count = static_cast<std::uint64_t>(later_progress_frames.size());
  result.first_dropped_frame = result.kept_frame_count;

  if (!later_progress_frames.empty() && result.dropped_frame_count > options.max_truncate_frames) {
    result.failed = true;
    result.error = "mid-stream integrity failure: would truncate " +
                   std::to_string(result.dropped_frame_count) +
                   " frames from frame " + std::to_string(result.first_dropped_frame) +
                   " to end";
    if (!inconsistent_reason.empty()) {
      result.error += ": " + inconsistent_reason;
    }
    if (inconsistent_sequence != 0) {
      result.error += " at sequence " + std::to_string(inconsistent_sequence);
    }
    result.error += " -- capture lost data mid-stream (not just a tail); bundle unusable, re-capture needed.";
    return result;
  }

  const auto target_offset = complete_frame_offset() != 0 ? complete_frame_offset() : header_end_offset;
  std::error_code size_error;
  const auto old_size = fs::file_size(callstream_path, size_error);
  if (size_error || old_size <= target_offset) {
    return result;
  }
  result.truncated = true;
  stats.truncated_inconsistent_frames += result.dropped_frame_count;
  stats.surviving_frame_count = result.kept_frame_count;
  stats.truncated_inconsistent_jsonl_bytes += static_cast<std::uint64_t>(old_size - target_offset);
  ++stats.truncated_inconsistent_jsonl_files;
  std::cerr << "warning: truncating callstream.jsonl at " << target_offset
            << " bytes after " << result.kept_frame_count
            << " complete frames; dropped " << result.dropped_frame_count
            << " present frames due to " << inconsistent_reason;
  if (inconsistent_sequence != 0) {
    std::cerr << " at sequence " << inconsistent_sequence;
  }
  std::cerr << "\n";
  if (options.dry_run) {
    return result;
  }
  std::error_code resize_error;
  fs::resize_file(callstream_path, target_offset, resize_error);
  if (resize_error) {
    std::cerr << "warning: failed to truncate callstream.jsonl: " << resize_error.message() << "\n";
    result.truncated = false;
    return result;
  }
  rewritten_digests.erase(apitrace::trace::kCallstreamFileName);
  return result;
}

std::optional<std::uint64_t> metal_present_frame_index(const json &record)
{
  if (!record.is_object() || record.value("call_kind", std::string()) != "PresentDrawable") {
    return std::nullopt;
  }
  const auto payload_it = record.find("payload");
  if (payload_it == record.end() || !payload_it->is_object()) {
    return std::nullopt;
  }
  const auto frame_index = json_u64_or_max(*payload_it, "frame_index");
  if (frame_index == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  return frame_index;
}

bool line_may_contain_metal_frame_progress_marker(std::string_view line)
{
  return line.find("\"PresentDrawable\"") != std::string::npos &&
         line.find("\"frame_index\"") != std::string::npos;
}

void truncate_metal_callstream_to_frame_count(
    const fs::path &bundle_root,
    std::uint64_t frame_count,
    const Options &options,
    Stats &stats,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests)
{
  const auto metal_callstream_path = bundle_root / apitrace::trace::kMetalCallstreamFileName;
  if (!fs::is_regular_file(metal_callstream_path)) {
    return;
  }

  std::uint64_t target_offset = 0;
  if (frame_count != 0) {
    std::ifstream input(metal_callstream_path, std::ios::binary);
    if (!input.is_open()) {
      return;
    }
    ++stats.jsonl_passes;
    std::uint64_t offset = 0;
    std::string line;
    while (std::getline(input, line)) {
      const auto line_bytes = static_cast<std::uint64_t>(line.size() + (input.eof() ? 0 : 1));
      const auto line_end_offset = offset + line_bytes;
      if (line_may_contain_metal_frame_progress_marker(line)) {
        const auto record = json::parse(line, nullptr, false);
        if (!record.is_discarded()) {
          if (const auto frame_index = metal_present_frame_index(record);
              frame_index && *frame_index + 1 == frame_count) {
            target_offset = line_end_offset;
          }
        }
      }
      offset = line_end_offset;
    }
    if (target_offset == 0) {
      return;
    }
  }

  std::error_code size_error;
  const auto old_size = fs::file_size(metal_callstream_path, size_error);
  if (size_error || old_size <= target_offset) {
    return;
  }
  ++stats.truncated_inconsistent_jsonl_files;
  stats.truncated_inconsistent_jsonl_bytes += static_cast<std::uint64_t>(old_size - target_offset);
  std::cerr << "warning: truncating metal-callstream.jsonl at " << target_offset
            << " bytes to match " << frame_count << " complete D3D frames\n";
  if (options.dry_run) {
    return;
  }
  std::error_code resize_error;
  fs::resize_file(metal_callstream_path, target_offset, resize_error);
  if (resize_error) {
    std::cerr << "warning: failed to truncate metal-callstream.jsonl: " << resize_error.message() << "\n";
    return;
  }
  rewritten_digests.erase(apitrace::trace::kMetalCallstreamFileName);
}

void truncate_translation_links_to_frame_count(
    const fs::path &bundle_root,
    std::uint64_t frame_count,
    const Options &options,
    Stats &stats,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests)
{
  const fs::path relative_path =
      fs::path(apitrace::trace::kAnalysisDirectoryName) / apitrace::trace::kTranslationLinksFileName;
  const auto link_path = bundle_root / relative_path;
  if (!fs::is_regular_file(link_path)) {
    return;
  }

  std::ifstream input(link_path, std::ios::binary);
  if (!input.is_open()) {
    return;
  }
  ++stats.jsonl_passes;
  std::uint64_t offset = 0;
  std::uint64_t target_offset = 0;
  std::string line;
  while (std::getline(input, line)) {
    const auto line_bytes = static_cast<std::uint64_t>(line.size() + (input.eof() ? 0 : 1));
    const auto line_end_offset = offset + line_bytes;
    if (!line.empty()) {
      const auto record = json::parse(line, nullptr, false);
      if (!record.is_discarded() && record.is_object()) {
        const auto frame_id = record.value("frame_id", 0ull);
        if (frame_id >= frame_count) {
          break;
        }
      }
    }
    target_offset = line_end_offset;
    offset = line_end_offset;
  }

  std::error_code size_error;
  const auto old_size = fs::file_size(link_path, size_error);
  if (size_error || old_size <= target_offset) {
    return;
  }
  ++stats.truncated_inconsistent_jsonl_files;
  stats.truncated_inconsistent_jsonl_bytes += static_cast<std::uint64_t>(old_size - target_offset);
  std::cerr << "warning: truncating translation-links.jsonl at " << target_offset
            << " bytes to match " << frame_count << " complete D3D frames\n";
  if (options.dry_run) {
    return;
  }
  std::error_code resize_error;
  fs::resize_file(link_path, target_offset, resize_error);
  if (resize_error) {
    std::cerr << "warning: failed to truncate translation-links.jsonl: " << resize_error.message() << "\n";
    return;
  }
  rewritten_digests.erase(relative_path.generic_string());
}

std::unordered_map<std::uint64_t, ShaderAssetRef> build_shader_ref_by_blob_id(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::uint64_t, ShaderAssetRef> by_blob_id;
  for (const auto &asset : assets) {
    const auto path = effective_asset_path(asset);
    const auto size = asset_storage_size(asset);
    if (!is_shader_asset(asset) || asset.blob_id == 0 || path.empty() || size == 0 || !asset.file_exists) {
      continue;
    }
    by_blob_id.emplace(asset.blob_id, ShaderAssetRef{asset.blob_id, size, path});
  }
  return by_blob_id;
}

std::unordered_map<std::string, ShaderAssetRef> build_shader_ref_by_path(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::string, ShaderAssetRef> by_path;
  for (const auto &asset : assets) {
    const auto path = effective_asset_path(asset);
    const auto size = asset_storage_size(asset);
    if (!is_shader_asset(asset) || asset.blob_id == 0 || path.empty() || size == 0 || !asset.file_exists) {
      continue;
    }
    const auto [it, inserted] = by_path.emplace(path, ShaderAssetRef{asset.blob_id, size, path});
    if (!inserted && asset.blob_id < it->second.blob_id) {
      it->second.blob_id = asset.blob_id;
    }
  }
  return by_path;
}

std::unordered_map<std::uint64_t, ShaderAssetRef> build_unique_shader_ref_by_size(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::uint64_t, ShaderAssetRef> by_size;
  std::unordered_set<std::uint64_t> ambiguous_sizes;
  for (const auto &asset : assets) {
    const auto path = effective_asset_path(asset);
    const auto size = asset_storage_size(asset);
    if (!is_shader_asset(asset) || asset.blob_id == 0 || path.empty() || size == 0 || !asset.file_exists) {
      continue;
    }
    const auto [it, inserted] = by_size.emplace(size, ShaderAssetRef{asset.blob_id, size, path});
    if (!inserted && it->second.path != path) {
      ambiguous_sizes.insert(size);
    } else if (!inserted && asset.blob_id < it->second.blob_id) {
      it->second.blob_id = asset.blob_id;
    }
  }
  for (const auto size : ambiguous_sizes) {
    by_size.erase(size);
  }
  return by_size;
}

std::unordered_map<std::string, AssetEntry> build_asset_by_effective_path(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::string, AssetEntry> by_path;
  for (const auto &asset : assets) {
    if (!asset.path.empty()) {
      by_path.emplace(asset.path, asset);
    }
    const auto path = effective_asset_path(asset);
    if (!path.empty()) {
      by_path.emplace(path, asset);
    }
  }
  return by_path;
}

bool resolve_pipeline_shader_paths(
    json &pipeline,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &shader_ref_by_blob_id,
    const std::unordered_map<std::string, ShaderAssetRef> &shader_ref_by_path,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &unique_shader_ref_by_size,
    std::vector<std::uint64_t> &shader_blob_refs)
{
  bool complete = true;
  for (const auto *stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
    auto shader = pipeline.find(stage);
    if (shader == pipeline.end() || shader->is_null()) {
      continue;
    }
    if (!shader->is_object()) {
      complete = false;
      continue;
    }
    const auto bytecode_size = shader->value("bytecode_size", 0ull);
    const auto blob_id = shader->value("blob_id", 0ull);
    if (blob_id == 0) {
      if (!shader->contains(std::string(stage) + "_path")) {
        complete = false;
      }
      continue;
    }
    const auto ref_it = shader_ref_by_blob_id.find(blob_id);
    if (ref_it != shader_ref_by_blob_id.end() && ref_it->second.byte_size == bytecode_size) {
      (*shader)[std::string(stage) + "_path"] = ref_it->second.path;
      shader->erase("blob_id");
      shader_blob_refs.push_back(ref_it->second.blob_id);
      continue;
    }

    const auto stage_path_key = std::string(stage) + "_path";
    if (shader->contains(stage_path_key) && (*shader)[stage_path_key].is_string()) {
      const auto stage_path = (*shader)[stage_path_key].get<std::string>();
      const auto path_it = shader_ref_by_path.find(stage_path);
      if (path_it != shader_ref_by_path.end() && path_it->second.byte_size == bytecode_size) {
        shader->erase("blob_id");
        shader_blob_refs.push_back(path_it->second.blob_id);
        continue;
      }
    }

    const auto size_it = unique_shader_ref_by_size.find(bytecode_size);
    if (size_it == unique_shader_ref_by_size.end() || size_it->second.path.empty()) {
      complete = false;
      continue;
    }
    (*shader)[std::string(stage) + "_path"] = size_it->second.path;
    shader->erase("blob_id");
    shader_blob_refs.push_back(size_it->second.blob_id);
  }
  return complete;
}

void append_unique_blob_ref(json &refs, std::unordered_set<std::uint64_t> &seen, std::uint64_t blob_id)
{
  if (blob_id != 0 && seen.insert(blob_id).second) {
    refs.push_back(blob_id);
  }
}

bool blob_ref_sets_equal(const json &lhs, const json &rhs)
{
  if (!lhs.is_array() || !rhs.is_array()) {
    return false;
  }
  std::unordered_set<std::uint64_t> lhs_refs;
  std::unordered_set<std::uint64_t> rhs_refs;
  for (const auto &ref : lhs) {
    if (!ref.is_number_unsigned()) {
      return false;
    }
    lhs_refs.insert(ref.get<std::uint64_t>());
  }
  for (const auto &ref : rhs) {
    if (!ref.is_number_unsigned()) {
      return false;
    }
    rhs_refs.insert(ref.get<std::uint64_t>());
  }
  return lhs_refs == rhs_refs;
}

void append_pipeline_dependency_blob_refs(
    const fs::path &bundle_root,
    const json &pipeline,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::uint64_t, std::string> &path_by_blob_id,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets,
    json &blob_refs,
    std::unordered_set<std::uint64_t> &seen_refs);

bool canonicalize_pipeline_dependency_paths(json &pipeline, const std::unordered_map<std::string, AssetEntry> &assets_by_path);

std::optional<AssetEntry> ensure_pipeline_asset(
    const fs::path &bundle_root,
    const json &pipeline,
    const Options &options,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets,
    Stats &stats)
{
  const auto text = pipeline.dump();
  const auto digest = apitrace::trace::content_hash_bytes(text.data(), text.size());
  const auto relative_path = (fs::path("pipelines") / (digest + ".pipeline.json")).generic_string();
  if (const auto existing = assets_by_path.find(relative_path); existing != assets_by_path.end()) {
    return existing->second;
  }

  AssetEntry asset;
  asset.blob_id = ++next_blob_id;
  asset.path = relative_path;
  asset.kind = "Pipeline";
  asset.debug_name = "d3d12-rebuilt-pipeline";
  asset.content_hash = digest;
  asset.digest = digest;
  asset.fast_fingerprint = apitrace::trace::fast_fingerprint_bytes(text.data(), text.size());
  asset.byte_size = static_cast<std::uint64_t>(text.size());
  asset.actual_size = asset.byte_size;
  asset.binary_payload = false;
  asset.source = AssetSource::Primary;
  asset.file_exists = true;
  asset.safe_path = true;
  asset.canonical_path = relative_path;

  if (!options.dry_run) {
    const auto absolute_path = bundle_root / relative_path;
    std::error_code error;
    fs::create_directories(absolute_path.parent_path(), error);
    if (error) {
      std::cerr << "warning: failed to create pipeline asset directory: " << error.message() << "\n";
      return std::nullopt;
    }
    std::ofstream output(absolute_path, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    if (!output) {
      std::cerr << "warning: failed to write rebuilt pipeline asset " << relative_path << "\n";
      return std::nullopt;
    }
  }

  assets.push_back(asset);
  assets_by_path.emplace(relative_path, asset);
  ++stats.rebuilt_d3d12_pipeline_assets;
  return asset;
}

bool rebuild_pipeline_event(
    const fs::path &bundle_root,
    json &record,
    const Options &options,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::uint64_t, std::string> &path_by_blob_id,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &shader_ref_by_blob_id,
    const std::unordered_map<std::string, ShaderAssetRef> &shader_ref_by_path,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &unique_shader_ref_by_size,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets,
    Stats &stats)
{
  const auto function = record.value("function", std::string());
  if (!is_d3d12_pipeline_create_function(function)) {
    return false;
  }
  auto payload_it = record.find("payload");
  if (payload_it == record.end() || !payload_it->is_object()) {
    return false;
  }
  auto &payload = *payload_it;
  if (payload.contains("pipeline_path")) {
    return false;
  }
  if (payload.value("pso_raw_version", 0) != 1) {
    return false;
  }

  json pipeline = payload;
  const auto raw_kind = pipeline.value("pso_kind", std::string());
  pipeline.erase("pso_raw_version");
  pipeline.erase("pso_kind");
  if (raw_kind.empty()) {
    ++stats.incomplete_d3d12_pipeline_semantics;
    return false;
  }
  pipeline["type"] = raw_kind;

  std::vector<std::uint64_t> shader_blob_refs;
  if (!resolve_pipeline_shader_paths(
          pipeline,
          shader_ref_by_blob_id,
          shader_ref_by_path,
          unique_shader_ref_by_size,
          shader_blob_refs)) {
    ++stats.incomplete_d3d12_pipeline_semantics;
    return false;
  }
  canonicalize_pipeline_dependency_paths(pipeline, assets_by_path);

  const auto pipeline_asset =
      ensure_pipeline_asset(bundle_root, pipeline, options, next_blob_id, assets_by_path, assets, stats);
  if (!pipeline_asset) {
    ++stats.incomplete_d3d12_pipeline_semantics;
    return false;
  }

  payload["pipeline_path"] = pipeline_asset->canonical_path.empty() ? pipeline_asset->path : pipeline_asset->canonical_path;
  payload.erase("pso_raw_version");
  payload.erase("pso_kind");
  for (const auto *stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
    payload.erase(stage);
  }

  json blob_refs = json::array();
  std::unordered_set<std::uint64_t> seen_refs;
  blob_refs.push_back(pipeline_asset->blob_id);
  seen_refs.insert(pipeline_asset->blob_id);
  for (const auto blob_id : shader_blob_refs) {
    append_unique_blob_ref(blob_refs, seen_refs, blob_id);
  }
  append_pipeline_dependency_blob_refs(
      bundle_root,
      pipeline,
      next_blob_id,
      path_by_blob_id,
      assets_by_path,
      assets,
      blob_refs,
      seen_refs);
  record["blob_refs"] = std::move(blob_refs);
  path_by_blob_id.emplace(
      pipeline_asset->blob_id,
      pipeline_asset->canonical_path.empty() ? pipeline_asset->path : pipeline_asset->canonical_path);
  return true;
}

std::string shader_stage_path_key(const char *stage)
{
  return std::string(stage) + "_path";
}

std::optional<ShaderAssetRef> shader_ref_for_pipeline_stage(
    const json &shader,
    const char *stage,
    const std::vector<ShaderAssetRef> &call_shader_refs,
    const std::unordered_map<std::string, ShaderAssetRef> &shader_ref_by_path,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &unique_shader_ref_by_size)
{
  if (!shader.is_object()) {
    return std::nullopt;
  }
  const auto bytecode_size = shader.value("bytecode_size", 0ull);
  if (bytecode_size == 0) {
    return std::nullopt;
  }

  const auto path = shader.value(shader_stage_path_key(stage), std::string());
  if (!path.empty()) {
    const auto path_it = shader_ref_by_path.find(path);
    if (path_it != shader_ref_by_path.end() && path_it->second.byte_size == bytecode_size) {
      return path_it->second;
    }
  }

  std::optional<ShaderAssetRef> call_match;
  for (const auto &ref : call_shader_refs) {
    if (ref.byte_size != bytecode_size) {
      continue;
    }
    if (call_match && call_match->path != ref.path) {
      call_match.reset();
      break;
    }
    call_match = ref;
  }
  if (call_match) {
    return call_match;
  }

  const auto size_it = unique_shader_ref_by_size.find(bytecode_size);
  if (size_it != unique_shader_ref_by_size.end()) {
    return size_it->second;
  }
  return std::nullopt;
}

bool repair_pipeline_asset_stage_refs(
    json &pipeline,
    const std::vector<ShaderAssetRef> &call_shader_refs,
    const std::unordered_map<std::string, ShaderAssetRef> &shader_ref_by_path,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &unique_shader_ref_by_size,
    std::vector<std::uint64_t> &shader_blob_refs,
    Stats &stats)
{
  struct StageRepair {
    std::string stage;
    ShaderAssetRef ref;
  };

  std::vector<StageRepair> repairs;
  bool changed = false;
  for (const auto *stage : {"vs", "ps", "ds", "hs", "gs", "cs", "as", "ms"}) {
    auto shader = pipeline.find(stage);
    if (shader == pipeline.end() || shader->is_null()) {
      continue;
    }
    if (!shader->is_object()) {
      ++stats.unresolved_d3d12_pipeline_asset_refs;
      continue;
    }
    const auto ref = shader_ref_for_pipeline_stage(
        *shader,
        stage,
        call_shader_refs,
        shader_ref_by_path,
        unique_shader_ref_by_size);
    if (!ref) {
      ++stats.unresolved_d3d12_pipeline_asset_refs;
      return false;
    }
    repairs.push_back({stage, *ref});
  }

  for (const auto &repair : repairs) {
    auto shader = pipeline.find(repair.stage);
    if (shader == pipeline.end() || !shader->is_object()) {
      continue;
    }
    const auto &ref = repair.ref;
    shader_blob_refs.push_back(ref.blob_id);
    const auto key = shader_stage_path_key(repair.stage.c_str());
    if (shader->value(key, std::string()) != ref.path) {
      (*shader)[key] = ref.path;
      changed = true;
    }
  }
  return changed;
}

bool canonicalize_pipeline_dependency_paths(json &pipeline, const std::unordered_map<std::string, AssetEntry> &assets_by_path)
{
  bool changed = false;
  if (pipeline.is_object()) {
    for (auto it = pipeline.begin(); it != pipeline.end(); ++it) {
      if (it.key().size() >= 5 &&
          it.key().compare(it.key().size() - 5, 5, "_path") == 0 &&
          it.value().is_string()) {
        const auto path = it.value().get<std::string>();
        const auto asset = assets_by_path.find(path);
        if (asset != assets_by_path.end()) {
          const auto effective_path = effective_asset_path(asset->second);
          if (!effective_path.empty() && effective_path != path) {
            it.value() = effective_path;
            changed = true;
          }
        }
      }
      changed = canonicalize_pipeline_dependency_paths(it.value(), assets_by_path) || changed;
    }
  } else if (pipeline.is_array()) {
    for (auto &item : pipeline) {
      changed = canonicalize_pipeline_dependency_paths(item, assets_by_path) || changed;
    }
  }
  return changed;
}

std::optional<AssetEntry> ensure_asset_for_path(
    const fs::path &bundle_root,
    const std::string &path,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets)
{
  if (path.empty()) {
    return std::nullopt;
  }
  if (const auto existing = assets_by_path.find(path); existing != assets_by_path.end()) {
    return existing->second;
  }

  auto asset = make_discovered_asset(bundle_root, ++next_blob_id, path);
  if (!asset) {
    return std::nullopt;
  }
  assets.push_back(*asset);
  assets_by_path.emplace(asset->path, *asset);
  return *asset;
}

void append_pipeline_dependency_blob_refs(
    const fs::path &bundle_root,
    const json &pipeline,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::uint64_t, std::string> &path_by_blob_id,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets,
    json &blob_refs,
    std::unordered_set<std::uint64_t> &seen_refs)
{
  std::vector<std::string> dependency_paths;
  collect_pipeline_dependency_paths(pipeline, dependency_paths);
  for (const auto &path : dependency_paths) {
    const auto asset = ensure_asset_for_path(bundle_root, path, next_blob_id, assets_by_path, assets);
    if (!asset) {
      continue;
    }
    append_unique_blob_ref(blob_refs, seen_refs, asset->blob_id);
    path_by_blob_id.emplace(asset->blob_id, effective_asset_path(*asset));
  }
}

bool repair_pipeline_asset_event(
    const fs::path &bundle_root,
    json &record,
    const Options &options,
    bool legacy_asset_discovery,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::uint64_t, std::string> &path_by_blob_id,
    const std::unordered_map<std::uint64_t, AssetEntry> &asset_by_blob_id,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &shader_ref_by_blob_id,
    const std::unordered_map<std::string, ShaderAssetRef> &shader_ref_by_path,
    const std::unordered_map<std::uint64_t, ShaderAssetRef> &unique_shader_ref_by_size,
    std::unordered_map<std::string, json> &pipeline_json_cache,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets,
    Stats &stats)
{
  auto payload_it = record.find("payload");
  if (payload_it == record.end() || !payload_it->is_object()) {
    return false;
  }
  auto &payload = *payload_it;
  const auto pipeline_path = payload.value("pipeline_path", std::string());
  if (pipeline_path.empty()) {
    return false;
  }

  auto pipeline_cache_it = pipeline_json_cache.find(pipeline_path);
  json pipeline;
  if (pipeline_cache_it != pipeline_json_cache.end()) {
    ++stats.pipeline_cache_hits;
    pipeline = pipeline_cache_it->second;
  } else {
    ++stats.pipeline_cache_misses;
    const auto pipeline_absolute = bundle_root / pipeline_path;
    std::ifstream input(pipeline_absolute, std::ios::binary);
    if (!input.is_open()) {
      return false;
    }
    pipeline = json::parse(input, nullptr, false);
    if (!pipeline.is_discarded() && pipeline.is_object()) {
      pipeline_json_cache.emplace(pipeline_path, pipeline);
    }
  }
  if (pipeline.is_discarded() || !pipeline.is_object()) {
    return false;
  }

  std::vector<ShaderAssetRef> call_shader_refs;
  std::vector<std::uint64_t> existing_non_shader_refs;
  for (const auto &blob_ref : record.value("blob_refs", json::array())) {
    if (!blob_ref.is_number_unsigned()) {
      continue;
    }
    const auto blob_id = blob_ref.get<std::uint64_t>();
    if (const auto shader_it = shader_ref_by_blob_id.find(blob_id); shader_it != shader_ref_by_blob_id.end()) {
      call_shader_refs.push_back(shader_it->second);
      continue;
    }
    existing_non_shader_refs.push_back(blob_id);
  }

  std::vector<std::uint64_t> shader_blob_refs;
  const bool repaired_stage_refs = repair_pipeline_asset_stage_refs(
          pipeline,
          call_shader_refs,
          shader_ref_by_path,
          unique_shader_ref_by_size,
          shader_blob_refs,
          stats);
  std::vector<std::string> dependency_paths;
  collect_pipeline_dependency_paths(pipeline, dependency_paths);
  if (!repaired_stage_refs && (!legacy_asset_discovery || dependency_paths.empty())) {
    return false;
  }
  canonicalize_pipeline_dependency_paths(pipeline, assets_by_path);

  const auto pipeline_asset =
      ensure_pipeline_asset(bundle_root, pipeline, options, next_blob_id, assets_by_path, assets, stats);
  if (!pipeline_asset) {
    return false;
  }

  const auto new_pipeline_path = pipeline_asset->canonical_path.empty() ? pipeline_asset->path : pipeline_asset->canonical_path;
  json blob_refs = json::array();
  std::unordered_set<std::uint64_t> seen_refs;
  append_unique_blob_ref(blob_refs, seen_refs, pipeline_asset->blob_id);
  for (const auto blob_id : existing_non_shader_refs) {
    const auto asset_it = asset_by_blob_id.find(blob_id);
    if (asset_it != asset_by_blob_id.end() && !is_pipeline_asset(asset_it->second)) {
      append_unique_blob_ref(blob_refs, seen_refs, blob_id);
    }
  }
  for (const auto blob_id : shader_blob_refs) {
    append_unique_blob_ref(blob_refs, seen_refs, blob_id);
  }
  append_pipeline_dependency_blob_refs(
      bundle_root,
      pipeline,
      next_blob_id,
      path_by_blob_id,
      assets_by_path,
      assets,
      blob_refs,
      seen_refs);
  if (pipeline_path == new_pipeline_path &&
      record.contains("blob_refs") &&
      blob_ref_sets_equal(record["blob_refs"], blob_refs) &&
      !repaired_stage_refs) {
    return false;
  }
  payload["pipeline_path"] = new_pipeline_path;
  record["blob_refs"] = std::move(blob_refs);
  path_by_blob_id.emplace(
      pipeline_asset->blob_id,
      pipeline_asset->canonical_path.empty() ? pipeline_asset->path : pipeline_asset->canonical_path);
  ++stats.repaired_d3d12_pipeline_events;
  return true;
}

bool repair_unmap_asset_event(
    const fs::path &bundle_root,
    json &record,
    std::uint64_t &next_blob_id,
    std::unordered_map<std::uint64_t, std::string> &path_by_blob_id,
    std::unordered_map<std::string, AssetEntry> &assets_by_path,
    std::vector<AssetEntry> &assets)
{
  if (record.value("function", std::string()) != "ID3D12Resource::Unmap") {
    return false;
  }
  auto payload_it = record.find("payload");
  if (payload_it == record.end() || !payload_it->is_object()) {
    return false;
  }
  const auto buffer_path = payload_it->value("buffer_path", std::string());
  if (buffer_path.empty()) {
    return false;
  }
  const auto asset = ensure_asset_for_path(bundle_root, buffer_path, next_blob_id, assets_by_path, assets);
  if (!asset || asset->blob_id == 0) {
    return false;
  }

  std::unordered_set<std::uint64_t> seen;
  json blob_refs = json::array();
  append_unique_blob_ref(blob_refs, seen, asset->blob_id);
  const auto existing = record.find("blob_refs");
  if (existing != record.end() && existing->is_array()) {
    for (const auto &ref : *existing) {
      if (ref.is_number_unsigned()) {
        append_unique_blob_ref(blob_refs, seen, ref.get<std::uint64_t>());
      }
    }
  }
  if (existing != record.end() && blob_ref_sets_equal(*existing, blob_refs)) {
    return false;
  }
  record["blob_refs"] = std::move(blob_refs);
  path_by_blob_id.emplace(asset->blob_id, effective_asset_path(*asset));
  return true;
}

std::unordered_set<std::string> rebuild_d3d12_pipeline_semantics(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    const Options &options,
    bool legacy_asset_discovery,
    Stats &stats,
    ProgressReporter *progress,
    FileDigestCache *digest_cache = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> *rewritten_digests = nullptr)
{
  std::unordered_set<std::string> rewritten_paths;
  const auto callstream_path = bundle_root / apitrace::trace::kCallstreamFileName;
  if (!fs::is_regular_file(callstream_path)) {
    return rewritten_paths;
  }

  auto path_by_blob_id = build_effective_path_by_blob_id(assets);
  const auto asset_by_blob_id = build_asset_by_blob_id(assets);
  const auto shader_ref_by_blob_id = build_shader_ref_by_blob_id(assets);
  const auto shader_ref_by_path = build_shader_ref_by_path(assets);
  const auto unique_shader_ref_by_size = build_unique_shader_ref_by_size(assets);
  auto assets_by_path = build_asset_by_effective_path(assets);
  std::unordered_map<std::string, json> pipeline_json_cache;
  auto next_blob_id = max_blob_id(assets);
  std::ifstream input(callstream_path, std::ios::binary);
  ++stats.jsonl_passes;
  if (!input.is_open()) {
    return rewritten_paths;
  }

  const auto temporary = temporary_rewrite_path(callstream_path);
  HashedOutputFile output;
  if (!options.dry_run) {
    output.open(temporary);
    if (!output.is_open()) {
      std::cerr << "warning: failed to open temporary rewrite file " << temporary << "\n";
      return rewritten_paths;
    }
  }

  std::string line;
  bool changed = false;
  std::uint64_t line_index = 0;
  while (std::getline(input, line)) {
    ++line_index;
    ++stats.jsonl_records;
    stats.input_bytes += static_cast<std::uint64_t>(line.size()) + 1;
    if (line.find("CreateGraphicsPipelineState") == std::string::npos &&
        line.find("CreateComputePipelineState") == std::string::npos &&
        line.find("CreatePipelineState") == std::string::npos &&
        line.find("ID3D12Resource::Unmap") == std::string::npos) {
      if (!options.dry_run) {
        output.write_line(line);
      }
      note_jsonl_output(stats, line);
      continue;
    }
    auto record = json::parse(line, nullptr, false);
    if (record.is_discarded()) {
      if (!options.dry_run) {
        output.write_line(line);
      }
      note_jsonl_output(stats, line);
      continue;
    }
    if (rebuild_pipeline_event(
            bundle_root,
            record,
            options,
            next_blob_id,
            path_by_blob_id,
            shader_ref_by_blob_id,
            shader_ref_by_path,
            unique_shader_ref_by_size,
            assets_by_path,
            assets,
            stats)) {
      const auto rewritten = record.dump();
      if (!options.dry_run) {
        output.write_line(rewritten);
      }
      note_jsonl_output(stats, rewritten);
      changed = true;
      ++stats.rewritten_records;
    } else if (repair_unmap_asset_event(
                   bundle_root,
                   record,
                   next_blob_id,
                   path_by_blob_id,
                   assets_by_path,
                   assets)) {
      const auto rewritten = record.dump();
      if (!options.dry_run) {
        output.write_line(rewritten);
      }
      note_jsonl_output(stats, rewritten);
      changed = true;
      ++stats.rewritten_records;
    } else if (repair_pipeline_asset_event(
                   bundle_root,
                   record,
                   options,
                   true,
                   next_blob_id,
                   path_by_blob_id,
                   asset_by_blob_id,
                   shader_ref_by_blob_id,
                   shader_ref_by_path,
                   unique_shader_ref_by_size,
                   pipeline_json_cache,
                   assets_by_path,
                   assets,
                   stats)) {
      const auto rewritten = record.dump();
      if (!options.dry_run) {
        output.write_line(rewritten);
      }
      note_jsonl_output(stats, rewritten);
      changed = true;
      ++stats.rewritten_records;
    } else {
      if (!options.dry_run) {
        output.write_line(line);
      }
      note_jsonl_output(stats, line);
    }
    if (progress) {
      progress->update(line_index, 0, 0, 0);
    }
  }

  if (!changed) {
    if (!options.dry_run) {
      output.close();
      std::error_code remove_error;
      fs::remove(temporary, remove_error);
    }
    return rewritten_paths;
  }
  ++stats.rewritten_text_files;
  rewritten_paths.insert(apitrace::trace::kCallstreamFileName);
  if (!options.dry_run) {
    const auto digest_and_size = output.digest_and_size();
    output.close();
    if (!replace_with_temporary_file(callstream_path, temporary)) {
      rewritten_paths.clear();
    } else if (digest_cache && rewritten_digests) {
      remember_rewritten_digest(
          bundle_root,
          fs::path(apitrace::trace::kCallstreamFileName),
          digest_and_size,
          *digest_cache,
          *rewritten_digests,
          stats);
    }
  }
  return rewritten_paths;
}

void write_asset_index(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const Options &options,
    const fs::path &relative_path = fs::path(apitrace::trace::kAssetIndexFileName),
    FileDigestCache *digest_cache = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> *rewritten_digests = nullptr,
    Stats *stats = nullptr)
{
  json list = json::array();
  auto sorted = assets;
  std::sort(sorted.begin(), sorted.end(), [](const AssetEntry &lhs, const AssetEntry &rhs) {
    if (lhs.metal != rhs.metal) {
      return !lhs.metal && rhs.metal;
    }
    if (lhs.blob_id != rhs.blob_id) {
      return lhs.blob_id < rhs.blob_id;
    }
    return lhs.path < rhs.path;
  });

  for (const auto &asset : sorted) {
    if (asset.blob_id == 0 || asset.path.empty()) {
      continue;
    }
    json entry;
    entry["blob_id"] = asset.blob_id;
    entry["path"] = asset.canonical_path.empty() ? asset.path : asset.canonical_path;
    entry["kind"] = asset.kind;
    entry["metal"] = asset.metal;
    entry["binary_payload"] = asset.binary_payload;
    entry["byte_size"] = asset.byte_size;
    if (!asset.debug_name.empty()) {
      entry["debug_name"] = asset.debug_name;
    }
    if (!asset.content_hash.empty()) {
      entry["content_hash"] = asset.content_hash;
    }
    if (!asset.fast_fingerprint.empty()) {
      entry["fast_fingerprint"] = asset.fast_fingerprint;
    }
    list.push_back(std::move(entry));
  }

  json root;
  root["assets"] = std::move(list);
  if (!options.dry_run) {
    const auto text = root.dump(2) + "\n";
    const auto absolute = bundle_root / relative_path;
    fs::create_directories(absolute.parent_path());
    HashedOutputFile output;
    output.open(absolute);
    output.write_text(text);
    const auto digest_and_size = output.digest_and_size();
    output.close();
    if (digest_cache && rewritten_digests && stats) {
      remember_rewritten_digest(
          bundle_root,
          relative_path,
          digest_and_size,
          *digest_cache,
          *rewritten_digests,
          *stats);
    }
  }
}

void write_sideband_asset_index(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const Options &options,
    FileDigestCache *digest_cache = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> *rewritten_digests = nullptr,
    Stats *stats = nullptr)
{
  std::vector<AssetEntry> sideband_assets;
  for (const auto &asset : assets) {
    if (asset.source == AssetSource::Sideband) {
      sideband_assets.push_back(asset);
    }
  }
  if (sideband_assets.empty()) {
    return;
  }

  write_asset_index(
      bundle_root,
      sideband_assets,
      options,
      fs::path(kSidebandAssetIndexPath),
      digest_cache,
      rewritten_digests,
      stats);
}

void refresh_asset_metadata_from_files(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &rewritten_paths,
    std::size_t jobs,
    Stats &stats,
    FileDigestCache &digest_cache,
    ProgressReporter *progress)
{
  if (rewritten_paths.empty()) {
    return;
  }

  struct RefreshGroup {
    std::string path;
    std::vector<std::size_t> asset_indices;
    std::string digest;
    std::uint64_t size = 0;
  };

  std::vector<RefreshGroup> groups;
  std::unordered_map<std::string, std::size_t> group_by_path;
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto path = effective_asset_path(assets[index]);
    if (path.empty()) {
      continue;
    }
    if (rewritten_paths.find(path) == rewritten_paths.end() &&
        rewritten_paths.find(assets[index].path) == rewritten_paths.end()) {
      continue;
    }
    const auto [it, inserted] = group_by_path.emplace(path, groups.size());
    if (inserted) {
      RefreshGroup group;
      group.path = path;
      group.asset_indices.push_back(index);
      groups.push_back(std::move(group));
    } else {
      groups[it->second].asset_indices.push_back(index);
    }
  }

  std::atomic<std::size_t> next{0};
  std::atomic<std::uint64_t> completed_files{0};
  std::atomic<std::uint64_t> completed_bytes{0};
  std::uint64_t total_bytes = 0;
  for (const auto &group : groups) {
    std::error_code size_error;
    total_bytes += static_cast<std::uint64_t>(fs::file_size(bundle_root / group.path, size_error));
  }
  std::mutex error_mutex;
  std::vector<std::string> errors;
  const auto worker = [&]() {
    for (;;) {
      const auto cursor = next.fetch_add(1, std::memory_order_relaxed);
      if (cursor >= groups.size()) {
        return;
      }
      auto &group = groups[cursor];
      const auto absolute = bundle_root / group.path;
      std::error_code error;
      if (!fs::is_regular_file(absolute, error) || error) {
        continue;
      }
      const auto size = static_cast<std::uint64_t>(fs::file_size(absolute, error));
      if (error) {
        continue;
      }
      try {
        group.digest = digest_cache.digest_file(absolute);
        group.size = size;
      } catch (const std::exception &exception) {
        std::lock_guard<std::mutex> lock(error_mutex);
        errors.push_back(group.path + ": " + exception.what());
      }
      const auto done_files = completed_files.fetch_add(1, std::memory_order_relaxed) + 1;
      const auto done_bytes = completed_bytes.fetch_add(size, std::memory_order_relaxed) + size;
      if (progress) {
        progress->update(done_files, groups.size(), done_bytes, total_bytes);
      }
    }
  };

  std::vector<std::thread> threads;
  const auto thread_count = std::max<std::size_t>(1, std::min(jobs, groups.size()));
  threads.reserve(thread_count);
  for (std::size_t i = 0; i < thread_count; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &thread : threads) {
    thread.join();
  }
  if (progress) {
    progress->update(groups.size(), groups.size(), total_bytes, total_bytes);
  }
  for (const auto &error : errors) {
    std::cerr << "warning: refresh failed for " << error << "\n";
  }

  for (const auto &group : groups) {
    if (group.digest.empty()) {
      continue;
    }
    stats.refreshed_asset_bytes += group.size;
    for (const auto index : group.asset_indices) {
      auto &asset = assets[index];
      if (asset.content_hash != group.digest || asset.byte_size != group.size) {
        ++stats.refreshed_asset_hashes;
      }
      asset.content_hash = group.digest;
      asset.digest = group.digest;
      asset.byte_size = group.size;
      asset.actual_size = group.size;
    }
  }
}

bool object_index_has_id(const json &objects, std::uint64_t object_id)
{
  if (!objects.is_array()) {
    return false;
  }
  for (const auto &entry : objects) {
    if (!entry.is_object()) {
      continue;
    }
    std::uint64_t entry_id = 0;
    if (json_u64_field(entry, "object_id", entry_id) && entry_id == object_id) {
      return true;
    }
  }
  return false;
}

void collect_existing_object_index_ids(
    const fs::path &objects_path,
    json &root,
    std::unordered_set<std::uint64_t> &known_object_ids)
{
  std::ifstream input(objects_path);
  if (input.is_open()) {
    root = json::parse(input, nullptr, false);
  }
  if (root.is_discarded() || !root.is_object()) {
    root = json::object();
  }
  if (!root.contains("objects") || !root["objects"].is_array()) {
    root["objects"] = json::array();
  }

  for (const auto &entry : root["objects"]) {
    if (!entry.is_object()) {
      continue;
    }
    std::uint64_t object_id = 0;
    if (json_u64_field(entry, "object_id", object_id) && object_id != 0) {
      known_object_ids.insert(object_id);
    }
  }
}

void repair_missing_d3d12_device_objects(
    const fs::path &bundle_root,
    const Options &options,
    Stats &stats,
    FileDigestCache &digest_cache,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests)
{
  const auto callstream_path = bundle_root / apitrace::trace::kCallstreamFileName;
  if (!fs::is_regular_file(callstream_path)) {
    return;
  }

  const auto relative_objects_path = fs::path("objects") / "objects.json";
  const auto objects_path = bundle_root / relative_objects_path;
  json object_index;
  std::unordered_set<std::uint64_t> known_object_ids;
  collect_existing_object_index_ids(objects_path, object_index, known_object_ids);

  std::vector<std::uint64_t> repaired_device_ids;
  ++stats.jsonl_passes;
  scan_jsonl_file(callstream_path, [&](const JsonlLineView &line_view) {
    const auto line = line_view.line;
    ++stats.jsonl_records;
    stats.input_bytes += line_view.byte_size;
    const bool may_update_known_objects =
        line.find("\"object_create\"") != std::string_view::npos ||
        line.find("\"object_destroy\"") != std::string_view::npos;
    const bool may_reference_device_receiver =
        line.find("\"ID3D12Device::") != std::string_view::npos &&
        line.find("\"object_refs\"") != std::string_view::npos;
    if (!may_update_known_objects && !may_reference_device_receiver) {
      return true;
    }

    const auto record = parse_jsonl_line(line);
    if (record.is_discarded() || !record.is_object()) {
      return true;
    }

    if (may_update_known_objects) {
      std::uint64_t object_id = 0;
      if (json_u64_field(record, "object_id", object_id) && object_id != 0) {
        known_object_ids.insert(object_id);
      }
    }

    if (!may_reference_device_receiver ||
        record.value("record_kind", std::string("call")) != "call" ||
        record.value("function", std::string()).rfind("ID3D12Device::", 0) != 0) {
      return true;
    }
    const auto refs = record.find("object_refs");
    if (refs == record.end() || !refs->is_array() || refs->empty()) {
      return true;
    }

    std::uint64_t receiver_id = 0;
    if (!json_u64_value((*refs)[0], receiver_id)) {
      return true;
    }
    if (receiver_id == 0 || known_object_ids.find(receiver_id) != known_object_ids.end()) {
      return true;
    }

    known_object_ids.insert(receiver_id);
    repaired_device_ids.push_back(receiver_id);
    return true;
  });

  if (repaired_device_ids.empty()) {
    return;
  }

  auto &objects = object_index["objects"];
  for (const auto object_id : repaired_device_ids) {
    if (object_index_has_id(objects, object_id)) {
      continue;
    }
    json entry;
    entry["object_id"] = object_id;
    entry["object_kind"] = "Device";
    entry["parent_object_id"] = 0;
    entry["debug_name"] = "ID3D12Device";
    objects.push_back(std::move(entry));
    ++stats.repaired_missing_device_objects;
  }

  if (stats.repaired_missing_device_objects == 0 || options.dry_run) {
    return;
  }

  fs::create_directories(objects_path.parent_path());
  HashedOutputFile output;
  output.open(objects_path);
  if (!output.is_open()) {
    std::cerr << "warning: failed to repair object index " << objects_path << "\n";
    return;
  }
  output.write_text(object_index.dump(2) + "\n");
  const auto digest_and_size = output.digest_and_size();
  output.close();
  remember_rewritten_digest(
      bundle_root,
      relative_objects_path,
      digest_and_size,
      digest_cache,
      rewritten_digests,
      stats);
}

std::string bundle_hash_from_records(std::vector<std::pair<std::string, std::pair<std::string, std::uint64_t>>> files)
{
  std::sort(files.begin(), files.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.first < rhs.first;
  });
  std::string source;
  for (const auto &[path, digest_and_size] : files) {
    source += path;
    source += "=";
    source += digest_and_size.first;
    source += "#";
    source += std::to_string(digest_and_size.second);
    source += "\n";
  }
  return "sha256:" + apitrace::trace::content_hash_bytes(source.data(), source.size());
}

bool is_host_metadata_file(const fs::path &relative)
{
  for (const auto &component : relative) {
    const auto name = component.generic_string();
    if (name == ".DS_Store" || name == "Thumbs.db" || name == "desktop.ini") {
      return true;
    }
  }
  return false;
}

void add_checksum_candidate(std::vector<fs::path> &candidates, std::unordered_set<std::string> &seen, const fs::path &relative)
{
  if (relative.empty() || !safe_relative_path(relative) || is_host_metadata_file(relative)) {
    return;
  }
  const auto text = relative.generic_string();
  if (text == apitrace::trace::kChecksumsFileName || !seen.insert(text).second) {
    return;
  }
  candidates.push_back(relative);
}

std::vector<fs::path> checksum_candidate_files(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests,
    const std::unordered_set<std::string> &referenced_paths)
{
  std::vector<fs::path> candidates;
  std::unordered_set<std::string> seen;

  add_checksum_candidate(candidates, seen, fs::path(apitrace::trace::kCallstreamFileName));
  add_checksum_candidate(candidates, seen, fs::path(apitrace::trace::kMetalCallstreamFileName));
  add_checksum_candidate(candidates, seen, fs::path(apitrace::trace::kAssetIndexFileName));
  add_checksum_candidate(candidates, seen, fs::path(kSidebandAssetIndexPath));
  add_checksum_candidate(candidates, seen, fs::path("objects") / "objects.json");
  add_checksum_candidate(candidates, seen, fs::path(apitrace::trace::kD3D12ReplayModelJsonName));
  add_checksum_candidate(candidates, seen, fs::path(apitrace::trace::kD3D12ReplayModelBlobName));

  const auto analysis_root = bundle_root / apitrace::trace::kAnalysisDirectoryName;
  std::error_code error;
  if (fs::is_directory(analysis_root, error) && !error) {
    for (fs::directory_iterator it(analysis_root, error), end; it != end && !error; it.increment(error)) {
      if (!it->is_regular_file(error) || error) {
        continue;
      }
      const auto relative = fs::relative(it->path(), bundle_root, error);
      if (!error) {
        add_checksum_candidate(candidates, seen, relative);
      }
    }
  }

  for (const auto &asset : assets) {
    const auto effective_path = effective_asset_path(asset);
    if (!effective_path.empty()) {
      add_checksum_candidate(candidates, seen, fs::path(effective_path));
    }
    if (!asset.path.empty()) {
      add_checksum_candidate(candidates, seen, fs::path(asset.path));
    }
  }

  for (const auto &[path, _] : rewritten_digests) {
    add_checksum_candidate(candidates, seen, fs::path(path));
  }
  for (const auto &path : referenced_paths) {
    add_checksum_candidate(candidates, seen, fs::path(path));
  }

  return candidates;
}

std::optional<std::pair<std::string, std::uint64_t>> parse_checksum_value(const std::string &value)
{
  constexpr std::string_view prefix = "sha256:";
  if (value.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  const auto size_separator = value.rfind(':');
  if (size_separator == std::string::npos || size_separator <= prefix.size()) {
    return std::nullopt;
  }
  const auto digest = value.substr(prefix.size(), size_separator - prefix.size());
  if (digest.empty()) {
    return std::nullopt;
  }
  const auto size_text = value.substr(size_separator + 1);
  char *end = nullptr;
  errno = 0;
  const auto size = std::strtoull(size_text.c_str(), &end, 10);
  if (errno != 0 || end == size_text.c_str() || *end != '\0') {
    return std::nullopt;
  }
  return std::make_pair(digest, static_cast<std::uint64_t>(size));
}

std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> load_prior_checksums(
    const fs::path &bundle_root)
{
  std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> checksums;
  const auto path = bundle_root / apitrace::trace::kChecksumsFileName;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return checksums;
  }
  const auto root = json::parse(input, nullptr, false);
  if (root.is_discarded() || !root.contains("files") || !root["files"].is_object()) {
    return checksums;
  }
  for (auto it = root["files"].begin(); it != root["files"].end(); ++it) {
    if (!it.value().is_string()) {
      continue;
    }
    const auto parsed = parse_checksum_value(it.value().get<std::string>());
    if (parsed) {
      checksums[it.key()] = *parsed;
    }
  }
  return checksums;
}

bool write_checksums_from_known(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &known)
{
  std::vector<std::pair<std::string, std::pair<std::string, std::uint64_t>>> records;
  records.reserve(known.size());
  for (const auto &[path, digest_and_size] : known) {
    records.push_back({path, digest_and_size});
  }

  json files = json::object();
  std::sort(records.begin(), records.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.first < rhs.first;
  });
  for (const auto &[path, digest_and_size] : records) {
    files[path] = "sha256:" + digest_and_size.first + ":" + std::to_string(digest_and_size.second);
  }

  json root;
  root["format_version"] = 1;
  root["bundle_hash"] = bundle_hash_from_records(records);
  root["files"] = std::move(files);

  std::ofstream output(bundle_root / apitrace::trace::kChecksumsFileName, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  output << root.dump(2) << "\n";
  return static_cast<bool>(output);
}

void write_checksums(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const Options &options,
    Stats &stats,
    FileDigestCache &digest_cache,
    ProgressReporter *progress,
    const std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests,
    const std::unordered_set<std::string> &referenced_paths)
{
  struct ChecksumRecord {
    std::string path;
    std::string digest;
    std::uint64_t size = 0;
  };
  struct HashTask {
    std::size_t record_index = 0;
    fs::path absolute_path;
  };

  std::error_code checksum_time_error;
  const auto prior_checksum_mtime =
      fs::last_write_time(bundle_root / apitrace::trace::kChecksumsFileName, checksum_time_error);
  const auto prior_checksums = checksum_time_error ? std::unordered_map<std::string, std::pair<std::string, std::uint64_t>>()
                                                  : load_prior_checksums(bundle_root);

  std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> known;
  for (const auto &asset : assets) {
    if (asset.content_hash.empty()) {
      continue;
    }
    const auto effective_path = effective_asset_path(asset);
    if (!effective_path.empty()) {
      known[effective_path] = {asset.content_hash, asset.byte_size};
    }
    if (!asset.path.empty() && (asset.path == effective_path || asset.original_file_exists)) {
      known[asset.path] = {asset.content_hash, asset.byte_size};
    }
  }
  for (const auto &[path, digest_and_size] : rewritten_digests) {
    known[path] = digest_and_size;
  }

  std::vector<ChecksumRecord> records;
  std::vector<HashTask> hash_tasks;
  std::error_code error;
  const auto candidates = checksum_candidate_files(bundle_root, assets, rewritten_digests, referenced_paths);
  for (const auto &relative : candidates) {
    const auto relative_text = relative.generic_string();
    if (relative_text == apitrace::trace::kChecksumsFileName || is_host_metadata_file(relative)) {
      continue;
    }
    const auto absolute = bundle_root / relative;
    if (!fs::is_regular_file(absolute, error) || error) {
      continue;
    }
    const auto size = static_cast<std::uint64_t>(fs::file_size(absolute, error));
    if (error) {
      continue;
    }
    auto digest = known.find(relative_text);
    if (digest == known.end() || digest->second.second != size) {
      const auto prior = prior_checksums.find(relative_text);
      const auto mtime = fs::last_write_time(absolute, error);
      const bool can_reuse_prior =
          prior != prior_checksums.end() &&
          prior->second.second == size &&
          !error &&
          mtime <= prior_checksum_mtime;
      if (can_reuse_prior) {
        ++stats.checksum_reused_prior_files;
        stats.checksum_reused_prior_bytes += size;
        records.push_back({relative_text, prior->second.first, size});
      } else {
        ++stats.checksum_hashed_files;
        stats.checksum_hashed_bytes += size;
        records.push_back({relative_text, std::string(), size});
        hash_tasks.push_back({records.size() - 1, absolute});
      }
    } else {
      if (rewritten_digests.find(relative_text) != rewritten_digests.end()) {
        ++stats.checksum_reused_rewritten_files;
      }
      records.push_back({relative_text, digest->second.first, digest->second.second});
    }
  }
  stats.checksum_files = records.size();

  std::atomic<std::size_t> next{0};
  std::atomic<std::uint64_t> completed_files{0};
  std::atomic<std::uint64_t> completed_bytes{0};
  std::mutex error_mutex;
  std::vector<std::string> errors;
  const auto worker = [&]() {
    for (;;) {
      const auto cursor = next.fetch_add(1, std::memory_order_relaxed);
      if (cursor >= hash_tasks.size()) {
        return;
      }
      const auto &task = hash_tasks[cursor];
      try {
        records[task.record_index].digest = digest_cache.digest_file(task.absolute_path);
      } catch (const std::exception &exception) {
        std::lock_guard<std::mutex> lock(error_mutex);
        errors.push_back(records[task.record_index].path + ": " + exception.what());
      }
      const auto done_files = completed_files.fetch_add(1, std::memory_order_relaxed) + 1;
      const auto done_bytes =
          completed_bytes.fetch_add(records[task.record_index].size, std::memory_order_relaxed) +
          records[task.record_index].size;
      if (progress) {
        progress->update(done_files, hash_tasks.size(), done_bytes, stats.checksum_hashed_bytes);
      }
    }
  };

  std::vector<std::thread> threads;
  const auto thread_count = std::max<std::size_t>(1, std::min(options.jobs, hash_tasks.size()));
  threads.reserve(thread_count);
  for (std::size_t i = 0; i < thread_count; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &thread : threads) {
    thread.join();
  }
  if (progress) {
    progress->update(hash_tasks.size(), hash_tasks.size(), stats.checksum_hashed_bytes, stats.checksum_hashed_bytes);
  }
  for (const auto &message : errors) {
    std::cerr << "warning: checksum hash failed for " << message << "\n";
  }

  json files = json::object();
  std::sort(records.begin(), records.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.path < rhs.path;
  });
  for (const auto &record : records) {
    if (record.digest.empty()) {
      continue;
    }
    files[record.path] = "sha256:" + record.digest + ":" + std::to_string(record.size);
  }

  std::vector<std::pair<std::string, std::pair<std::string, std::uint64_t>>> hash_records;
  hash_records.reserve(records.size());
  for (const auto &record : records) {
    if (!record.digest.empty()) {
      hash_records.push_back({record.path, {record.digest, record.size}});
    }
  }

  json root;
  root["format_version"] = 1;
  root["bundle_hash"] = bundle_hash_from_records(std::move(hash_records));
  root["files"] = std::move(files);

  if (!options.dry_run) {
    std::ofstream output(bundle_root / apitrace::trace::kChecksumsFileName, std::ios::binary | std::ios::trunc);
    output << root.dump(2) << "\n";
  }
}

// Reconstruct the D3D12 replay object model from the finalized callstream once and persist it,
// so retrace can load it and skip the in-process initialize+replay_event reconstruction. Runs
// only for D3D12 bundles; never touches the GPU (no finalize_replay). Best-effort: a failure is
// reported but does not fail finalize (retrace falls back to in-process reconstruction).
bool persist_d3d12_replay_model(const fs::path &bundle_root, const Options &options, Stats &stats)
{
  if (options.dry_run) {
    return false;
  }

  apitrace::trace::TraceBundleReader reader;
  // The D3D12 object-model reconstruction consumes only the D3D12 event stream; it never touches
  // the Metal sideband. Skip loading metal-callstream.jsonl (can be multiple GB) — parsing it here
  // would dominate finalize time for nothing. Also skip per-asset checksum re-validation: open()
  // would otherwise sha256 every file in the (multi-GB, thousands-of-files) bundle, which is the
  // job of bundle-check --verify-hashes, not of model reconstruction. We are inside finalize, which
  // is recomputing checksums itself moments later, so the bytes are trusted here.
  apitrace::trace::TraceBundleReader::OpenOptions reader_options;
  reader_options.load_metal_sideband = false;
  reader_options.validate_checksum_contents = false;
  reader_options.validate_file_references = false;
  reader_options.discover_referenced_assets = false;
  if (!reader.open(bundle_root, reader_options)) {
    std::cerr << "warning: replay-model persist skipped: failed to reopen bundle: "
              << (reader.last_error().empty() ? "unknown error" : reader.last_error()) << "\n";
    return false;
  }
  if (reader.metadata().api != apitrace::trace::ApiKind::D3D12) {
    return false;
  }

  apitrace::d3d12::D3D12ReplayBackend backend;
  if (!backend.initialize(reader)) {
    std::cerr << "warning: replay-model persist skipped: backend initialize failed: "
              << backend.last_error() << "\n";
    return false;
  }
  for (const auto &event : reader.events()) {
    if (!backend.replay_event(event)) {
      std::cerr << "warning: replay-model persist skipped: replay_event failed at sequence "
                << event.callsite.sequence << ": " << backend.last_error() << "\n";
      return false;
    }
  }

  // Staleness binding is intentionally not recorded: retrace trusts the persisted model as valid
  // input (finalize+retrace are a configured pipeline) rather than paying a multi-GB re-hash per
  // run to catch a user swapping the callstream. Bundle-level integrity is still covered by
  // checksums.json (which hashes the model files too). Leave source_bundle_hash empty; schema_version
  // remains the format guard. Binding to the bundle_hash would also be circular — the model files
  // are part of the bundle, so they change the very hash they would try to record.
  const std::string source_bundle_hash;

  const auto json_path = bundle_root / apitrace::trace::kD3D12ReplayModelJsonName;
  const auto blob_path = bundle_root / apitrace::trace::kD3D12ReplayModelBlobName;
  std::error_code dir_error;
  fs::create_directories(json_path.parent_path(), dir_error);

  std::string error;
  if (!backend.save_replay_model(json_path, blob_path, source_bundle_hash, error)) {
    std::cerr << "warning: replay-model persist failed: " << error << "\n";
    return false;
  }

  std::error_code size_error;
  stats.d3d12_replay_model_written = true;
  stats.d3d12_replay_model_json_bytes =
      static_cast<std::uint64_t>(fs::file_size(json_path, size_error));
  stats.d3d12_replay_model_blob_bytes =
      static_cast<std::uint64_t>(fs::file_size(blob_path, size_error));
  return true;
}

int run_persist_replay_model_only(const Options &options)
{
  const auto started = std::chrono::steady_clock::now();
  constexpr std::size_t kStageCount = 3;
  std::size_t stage_index = 0;
  ProgressReporter progress(options.progress);
  Stats stats;
  FileDigestCache digest_cache;
  bool model_written = false;

  run_stage(options, progress, stage_index, kStageCount, "persist_d3d12_replay_model", [&] {
    model_written = persist_d3d12_replay_model(options.bundle_root, options, stats);
  });
  if (!model_written) {
    progress.clear_line();
    return 1;
  }

  std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> checksums;
  run_stage(options, progress, stage_index, kStageCount, "load_checksums", [&] {
    checksums = load_prior_checksums(options.bundle_root);
  });
  run_stage(options, progress, stage_index, kStageCount, "update_model_checksums", [&] {
    for (const auto &relative : {
             fs::path(apitrace::trace::kD3D12ReplayModelJsonName),
             fs::path(apitrace::trace::kD3D12ReplayModelBlobName)}) {
      const auto absolute = options.bundle_root / relative;
      std::error_code error;
      const auto size = static_cast<std::uint64_t>(fs::file_size(absolute, error));
      if (error) {
        std::cerr << "warning: failed to stat replay-model file " << relative.generic_string() << "\n";
        continue;
      }
      try {
        checksums[relative.generic_string()] = {digest_cache.digest_file(absolute), size};
        ++stats.checksum_hashed_files;
        stats.checksum_hashed_bytes += size;
      } catch (const std::exception &exception) {
        std::cerr << "warning: failed to hash replay-model file " << relative.generic_string()
                  << ": " << exception.what() << "\n";
      }
    }
    stats.checksum_files = checksums.size();
    if (!options.dry_run && !write_checksums_from_known(options.bundle_root, checksums)) {
      std::cerr << "error: failed to update " << apitrace::trace::kChecksumsFileName << "\n";
    }
  });

  stats.digest_cache_hits = digest_cache.hits();
  stats.digest_cache_misses = digest_cache.misses();
  progress.clear_line();
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "bundle-finalize: "
            << "d3d12_replay_model_written=" << (stats.d3d12_replay_model_written ? "true" : "false")
            << " d3d12_replay_model_json_bytes=" << stats.d3d12_replay_model_json_bytes
            << " d3d12_replay_model_blob_bytes=" << stats.d3d12_replay_model_blob_bytes
            << " checksum_files=" << stats.checksum_files
            << " checksum_hashed_files=" << stats.checksum_hashed_files
            << " checksum_hashed_bytes=" << stats.checksum_hashed_bytes
            << " digest_cache_hits=" << stats.digest_cache_hits
            << " digest_cache_misses=" << stats.digest_cache_misses
            << " dry_run=" << (options.dry_run ? "true" : "false")
            << " persist_replay_model_only=true"
            << " elapsed_s=" << elapsed << "\n";
  return 0;
}

} // namespace

int apitrace::tools::run_bundle_finalize(int argc, char **argv){
  const auto parsed = parse_args(argc, argv);
  if (!parsed) {
    const auto help_requested = std::any_of(argv + 1, argv + argc, [](const char *arg) {
      return std::string(arg) == "--help" || std::string(arg) == "-h";
    });
    return help_requested ? 0 : 2;
  }
  const auto options = *parsed;

  std::error_code error;
  if (!fs::is_directory(options.bundle_root, error) || error) {
    std::cerr << "error: bundle root is not a directory: " << options.bundle_root << "\n";
    return 2;
  }

  if (options.persist_replay_model_only) {
    return run_persist_replay_model_only(options);
  }

  const auto started = std::chrono::steady_clock::now();
  constexpr std::size_t kStageCount = 25;
  std::size_t stage_index = 0;
  ProgressReporter progress(options.progress);
  Stats stats;
  FileDigestCache digest_cache;
  std::vector<AssetEntry> assets;
  std::unordered_map<std::string, std::string> canonical_aliases;
  IncompleteSpoolRef incomplete_spool_ref;
  std::size_t loaded_asset_count = 0;
  bool raw_to_final_ok = true;
  run_stage(options, progress, stage_index, kStageCount, "raw_to_final", [&] {
    if (options.raw_format) {
      raw_to_final_ok = materialize_raw_capture_to_final_bundle(options, stats);
    }
  });
  if (!raw_to_final_ok) {
    return 1;
  }
  std::error_code callstream_size_error;
  const auto callstream_size =
      fs::file_size(options.bundle_root / apitrace::trace::kCallstreamFileName, callstream_size_error);
  constexpr std::uint64_t kLegacyDiscoveryCallstreamLimit = 64ull * 1024ull * 1024ull;
  const bool legacy_asset_discovery =
      !callstream_size_error &&
      callstream_size <= kLegacyDiscoveryCallstreamLimit;
  const bool finalized_indexed_bundle =
      !legacy_asset_discovery &&
      fs::is_regular_file(options.bundle_root / apitrace::trace::kChecksumsFileName);
  run_stage(options, progress, stage_index, kStageCount, "restore_inline_query_assets", [&] {
    restore_inline_query_assets(options.bundle_root, options, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "load_asset_indexes", [&] {
    load_asset_index(options.bundle_root / apitrace::trace::kAssetIndexFileName, AssetSource::Primary, assets, stats);
    load_asset_index(options.bundle_root / kSidebandAssetIndexPath, AssetSource::Sideband, assets, stats);
    loaded_asset_count = assets.size();
  });
  run_stage(options, progress, stage_index, kStageCount, "scan_raw_events", [&] {
    if (!finalized_indexed_bundle || assets.empty()) {
      discover_raw_asset_references(options.bundle_root, assets, legacy_asset_discovery, &incomplete_spool_ref, stats);
    }
  });
  run_stage(options, progress, stage_index, kStageCount, "merge_assets", [&] {
    assets = merge_assets(std::move(assets));
    repair_finalized_single_blob_asset_index(options.bundle_root, assets, finalized_indexed_bundle);
    stats.indexed_assets = assets.size();
  });
  run_stage(options, progress, stage_index, kStageCount, "stat_assets", [&] {
    stat_assets(options.bundle_root, assets);
  });
  run_stage(options, progress, stage_index, kStageCount, "hash_assets", [&] {
    hash_assets(
        options.bundle_root,
        assets,
        options.jobs,
        finalized_indexed_bundle && !options.verify_existing_canonical,
        stats,
        digest_cache,
        &progress);
  });
  run_stage(options, progress, stage_index, kStageCount, "ensure_canonical_files", [&] {
    const auto stage_aliases = ensure_canonical_files(options.bundle_root, assets, options, stats, digest_cache);
    std::unordered_set<std::string> ignored_ambiguous_aliases;
    merge_unambiguous_aliases(canonical_aliases, ignored_ambiguous_aliases, stage_aliases);
  });

  std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> rewritten_digests;
  std::unordered_map<std::uint64_t, std::uint64_t> blob_id_remap;
  std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint64_t>> blob_id_remap_by_path;
  run_stage(options, progress, stage_index, kStageCount, "normalize_blob_ids", [&] {
    blob_id_remap = normalize_blob_ids(assets, stats, &blob_id_remap_by_path);
  });
  std::unordered_map<std::string, std::string> aliases;
  std::unordered_set<std::string> ambiguous_aliases;
  std::unordered_map<std::string, std::uint64_t> blob_id_by_effective_path;
  std::unordered_map<std::uint64_t, std::string> effective_path_by_blob_id;
  std::unordered_set<std::string> referenced_paths_after_rewrite;
  run_stage(options, progress, stage_index, kStageCount, "build_aliases", [&] {
    aliases = build_aliases(assets);
    ambiguous_aliases.clear();
    merge_unambiguous_aliases(aliases, ambiguous_aliases, canonical_aliases);
    const auto unindexed_asset_file_aliases =
        build_unindexed_asset_file_aliases(options.bundle_root, assets, digest_cache, stats);
    merge_unambiguous_aliases(aliases, ambiguous_aliases, unindexed_asset_file_aliases);
    if (loaded_asset_count == 0 && legacy_asset_discovery) {
      const auto initial_pipeline_file_aliases = build_pipeline_file_aliases(options.bundle_root, digest_cache);
      merge_unambiguous_aliases(aliases, ambiguous_aliases, initial_pipeline_file_aliases);
    }
    blob_id_by_effective_path = build_blob_id_by_effective_path(assets);
    effective_path_by_blob_id = build_effective_path_by_blob_id(assets);
  });
  std::unordered_set<std::string> rewritten_paths;
  bool rebuilt_assets_changed = false;
  CallstreamPrefixResult callstream_prefix;
  bool finalize_failed = false;
  run_stage(options, progress, stage_index, kStageCount, "rewrite_references", [&] {
    const auto callstream = options.bundle_root / apitrace::trace::kCallstreamFileName;
    const auto metal_callstream = options.bundle_root / apitrace::trace::kMetalCallstreamFileName;
    const auto translation_links =
        options.bundle_root / apitrace::trace::kAnalysisDirectoryName / apitrace::trace::kTranslationLinksFileName;
    sanitize_jsonl_tail_file(callstream, options, stats, &progress, 1, 3);
    sanitize_jsonl_tail_file(metal_callstream, options, stats, &progress, 2, 3);
    sanitize_jsonl_tail_file(translation_links, options, stats, &progress, 3, 3);
    rewritten_paths =
        rewrite_text_references(
            options.bundle_root,
            aliases,
            blob_id_by_effective_path,
            effective_path_by_blob_id,
            blob_id_remap_by_path,
            blob_id_remap,
            options,
            stats,
            &progress,
            legacy_asset_discovery ? &referenced_paths_after_rewrite : nullptr,
            &digest_cache,
            &rewritten_digests);
  });
  run_stage(options, progress, stage_index, kStageCount, "sanitize_tail", [&] {
    if (!blob_id_remap.empty()) {
      return;
    }
    const auto callstream = options.bundle_root / apitrace::trace::kCallstreamFileName;
    const auto metal_callstream = options.bundle_root / apitrace::trace::kMetalCallstreamFileName;
    const auto translation_links =
        options.bundle_root / apitrace::trace::kAnalysisDirectoryName / apitrace::trace::kTranslationLinksFileName;
    sanitize_and_rewrite_jsonl_file(
        callstream, blob_id_remap, options, stats, &progress, 1, 3, &digest_cache, &rewritten_digests);
    sanitize_and_rewrite_jsonl_file(
        metal_callstream, blob_id_remap, options, stats, &progress, 2, 3, &digest_cache, &rewritten_digests);
    sanitize_and_rewrite_jsonl_file(
        translation_links, blob_id_remap, options, stats, &progress, 3, 3, &digest_cache, &rewritten_digests);
  });
  run_stage(options, progress, stage_index, kStageCount, "repair_callstream_sequences", [&] {
    const auto sequence_rewrites =
        repair_callstream_sequence_regressions(
            options.bundle_root,
            options,
            stats,
            digest_cache,
            rewritten_digests);
    rewritten_paths.insert(sequence_rewrites.begin(), sequence_rewrites.end());
  });
  run_stage(options, progress, stage_index, kStageCount, "truncate_inconsistent_streams", [&] {
    callstream_prefix =
        truncate_callstream_to_complete_prefix(
            options.bundle_root,
            assets,
            options,
            stats,
            rewritten_digests);
    if (callstream_prefix.failed) {
      std::cerr << "error: " << callstream_prefix.error << "\n";
      finalize_failed = true;
      return;
    }
    if (!callstream_prefix.truncated) {
      return;
    }
    truncate_metal_callstream_to_frame_count(
        options.bundle_root,
        callstream_prefix.kept_frame_count,
        options,
        stats,
        rewritten_digests);
    truncate_translation_links_to_frame_count(
        options.bundle_root,
        callstream_prefix.kept_frame_count,
        options,
        stats,
        rewritten_digests);
    referenced_paths_after_rewrite.clear();
  });
  if (finalize_failed) {
    return 1;
  }
  run_stage(options, progress, stage_index, kStageCount, "rebuild_semantics", [&] {
    const auto rebuilt_paths =
        rebuild_d3d12_pipeline_semantics(
            options.bundle_root,
            assets,
            options,
            loaded_asset_count == 0 && legacy_asset_discovery,
            stats,
            &progress,
            &digest_cache,
            &rewritten_digests);
    rewritten_paths.insert(rebuilt_paths.begin(), rebuilt_paths.end());
    if (!rebuilt_paths.empty()) {
      assets = merge_assets(std::move(assets));
      stat_assets(options.bundle_root, assets);
      hash_assets(
          options.bundle_root,
          assets,
          options.jobs,
          finalized_indexed_bundle && !options.verify_existing_canonical,
          stats,
          digest_cache,
          &progress);
      const auto stage_aliases = ensure_canonical_files(options.bundle_root, assets, options, stats, digest_cache);
      std::unordered_set<std::string> ignored_ambiguous_aliases;
      merge_unambiguous_aliases(canonical_aliases, ignored_ambiguous_aliases, stage_aliases);
      std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint64_t>> rebuilt_blob_id_remap_by_path;
      const auto rebuilt_blob_id_remap =
          normalize_blob_ids(assets, stats, &rebuilt_blob_id_remap_by_path);
      if (!rebuilt_blob_id_remap_by_path.empty()) {
        const auto rewritten =
            rewrite_text_references(
                options.bundle_root,
                aliases,
                blob_id_by_effective_path,
                effective_path_by_blob_id,
                rebuilt_blob_id_remap_by_path,
                {},
                options,
                stats,
                &progress,
                legacy_asset_discovery ? &referenced_paths_after_rewrite : nullptr,
                &digest_cache,
                &rewritten_digests);
        rewritten_paths.insert(rewritten.begin(), rewritten.end());
      }
      rebuilt_assets_changed = !rebuilt_blob_id_remap.empty();
      if (!rebuilt_blob_id_remap.empty()) {
        sanitize_and_rewrite_jsonl_file(
            options.bundle_root / apitrace::trace::kCallstreamFileName,
            rebuilt_blob_id_remap,
            options,
            stats,
            &progress,
            0,
            0,
            &digest_cache,
            &rewritten_digests);
      }
    }
  });
  run_stage(options, progress, stage_index, kStageCount, "rewrite_rebuilt_references", [&] {
    if (rewritten_paths.empty() && !rebuilt_assets_changed) {
      return;
    }
    aliases = build_aliases(assets);
    ambiguous_aliases.clear();
    merge_unambiguous_aliases(aliases, ambiguous_aliases, canonical_aliases);
    const auto unindexed_asset_file_aliases =
        build_unindexed_asset_file_aliases(options.bundle_root, assets, digest_cache, stats);
    merge_unambiguous_aliases(aliases, ambiguous_aliases, unindexed_asset_file_aliases);
    blob_id_by_effective_path = build_blob_id_by_effective_path(assets);
    effective_path_by_blob_id = build_effective_path_by_blob_id(assets);
    if (aliases.empty()) {
      return;
    }
    const auto rebuilt_rewrites =
        rewrite_text_references(
            options.bundle_root,
            aliases,
            blob_id_by_effective_path,
            effective_path_by_blob_id,
            {},
            {},
            options,
            stats,
            &progress,
            legacy_asset_discovery ? &referenced_paths_after_rewrite : nullptr,
            &digest_cache,
            &rewritten_digests);
    rewritten_paths.insert(rebuilt_rewrites.begin(), rebuilt_rewrites.end());
    if (!rebuilt_rewrites.empty()) {
      stat_assets(options.bundle_root, assets);
      hash_assets(
          options.bundle_root,
          assets,
          options.jobs,
          finalized_indexed_bundle && !options.verify_existing_canonical,
          stats,
          digest_cache,
          &progress);
      const auto stage_aliases = ensure_canonical_files(options.bundle_root, assets, options, stats, digest_cache);
      std::unordered_set<std::string> ignored_ambiguous_aliases;
      merge_unambiguous_aliases(canonical_aliases, ignored_ambiguous_aliases, stage_aliases);
      assets = merge_assets(std::move(assets));
    }
    aliases = build_aliases(assets);
    ambiguous_aliases.clear();
    merge_unambiguous_aliases(aliases, ambiguous_aliases, canonical_aliases);
    if (loaded_asset_count == 0 && legacy_asset_discovery) {
      const auto pipeline_file_aliases = build_pipeline_file_aliases(options.bundle_root, digest_cache);
      merge_unambiguous_aliases(aliases, ambiguous_aliases, pipeline_file_aliases);
    }
    blob_id_by_effective_path = build_blob_id_by_effective_path(assets);
    effective_path_by_blob_id = build_effective_path_by_blob_id(assets);
    if (!aliases.empty()) {
      const auto final_rewrites =
          rewrite_text_references(
              options.bundle_root,
              aliases,
              blob_id_by_effective_path,
              effective_path_by_blob_id,
              {},
              {},
              options,
              stats,
              &progress,
              legacy_asset_discovery ? &referenced_paths_after_rewrite : nullptr,
              &digest_cache,
              &rewritten_digests);
      rewritten_paths.insert(final_rewrites.begin(), final_rewrites.end());
    }
  });
  run_stage(options, progress, stage_index, kStageCount, "rebuild_reference_graph", [&] {
    if (referenced_paths_after_rewrite.empty()) {
      referenced_paths_after_rewrite = collect_referenced_asset_paths(options.bundle_root, aliases);
    }
    const auto referenced_blob_ids_after_rewrite = collect_referenced_blob_ids(options.bundle_root);
    stats.referenced_paths_collected = referenced_paths_after_rewrite.size();
    if (callstream_prefix.truncated && !options.dry_run) {
      drop_unreferenced_truncated_assets(
          assets,
          referenced_paths_after_rewrite,
          referenced_blob_ids_after_rewrite,
          stats);
    }
    drop_unreferenced_missing_assets(assets, referenced_paths_after_rewrite, referenced_blob_ids_after_rewrite, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "remove_duplicate_files", [&] {
    remove_duplicate_files(options.bundle_root, assets, aliases, referenced_paths_after_rewrite, options, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "remove_orphan_asset_files", [&] {
    remove_orphan_asset_files(options.bundle_root, assets, referenced_paths_after_rewrite, options, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "remove_payload_spool_files", [&] {
    remove_payload_spool_files(options.bundle_root, assets, options, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "refresh_asset_metadata", [&] {
    if (!options.dry_run) {
      refresh_asset_metadata_from_files(
          options.bundle_root,
          assets,
          rewritten_paths,
          options.jobs,
          stats,
          digest_cache,
          &progress);
    }
  });
  run_stage(options, progress, stage_index, kStageCount, "repair_object_index", [&] {
    repair_missing_d3d12_device_objects(
        options.bundle_root,
        options,
        stats,
        digest_cache,
        rewritten_digests);
  });
  run_stage(options, progress, stage_index, kStageCount, "write_indexes", [&] {
    write_asset_index(
        options.bundle_root,
        assets,
        options,
        fs::path(apitrace::trace::kAssetIndexFileName),
        &digest_cache,
        &rewritten_digests,
        &stats);
    write_sideband_asset_index(
        options.bundle_root,
        assets,
        options,
        &digest_cache,
        &rewritten_digests,
        &stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "persist_d3d12_replay_model", [&] {
    persist_d3d12_replay_model(options.bundle_root, options, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "write_checksums", [&] {
    write_checksums(
        options.bundle_root,
        assets,
        options,
        stats,
        digest_cache,
        &progress,
        rewritten_digests,
        referenced_paths_after_rewrite);
  });
  stats.digest_cache_hits = digest_cache.hits();
  stats.digest_cache_misses = digest_cache.misses();

  progress.clear_line();
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "bundle-finalize: "
            << "input_assets=" << stats.input_assets
            << " indexed_assets=" << stats.indexed_assets
            << " hashed_assets=" << stats.hashed_assets
            << " rewritten_assets=" << stats.rewritten_assets
            << " recovered_unindexed_asset_aliases=" << stats.recovered_unindexed_asset_aliases
            << " duplicate_assets=" << stats.duplicate_assets
            << " duplicate_bytes=" << stats.duplicate_bytes
            << " restored_inline_query_assets=" << stats.restored_inline_query_assets
            << " recovered_spooled_assets=" << stats.recovered_spooled_assets
            << " recovered_spooled_asset_bytes=" << stats.recovered_spooled_asset_bytes
            << " raw_to_final_events=" << stats.raw_to_final_events
            << " raw_to_final_assets=" << stats.raw_to_final_assets
            << " raw_to_final_asset_bytes=" << stats.raw_to_final_asset_bytes
            << " repaired_missing_device_objects=" << stats.repaired_missing_device_objects
            << " sequence_regression_segments=" << stats.sequence_regression_segments
            << " remapped_sequence_records=" << stats.remapped_sequence_records
            << " rebuilt_d3d12_pipeline_assets=" << stats.rebuilt_d3d12_pipeline_assets
            << " d3d12_replay_model_written=" << (stats.d3d12_replay_model_written ? "true" : "false")
            << " d3d12_replay_model_json_bytes=" << stats.d3d12_replay_model_json_bytes
            << " d3d12_replay_model_blob_bytes=" << stats.d3d12_replay_model_blob_bytes
            << " repaired_d3d12_pipeline_events=" << stats.repaired_d3d12_pipeline_events
            << " incomplete_d3d12_pipeline_semantics=" << stats.incomplete_d3d12_pipeline_semantics
            << " unresolved_d3d12_pipeline_asset_refs=" << stats.unresolved_d3d12_pipeline_asset_refs
            << " preserved_referenced_duplicate_assets=" << stats.preserved_referenced_duplicate_assets
            << " dropped_unreferenced_missing_assets=" << stats.dropped_unreferenced_missing_assets
            << " dropped_unreferenced_truncated_assets=" << stats.dropped_unreferenced_truncated_assets
            << " removed_orphan_asset_files=" << stats.removed_orphan_asset_files
            << " remapped_blob_ids=" << stats.remapped_blob_ids
            << " rewritten_text_files=" << stats.rewritten_text_files
            << " rewritten_blob_ref_files=" << stats.rewritten_blob_ref_files
            << " rewritten_primary_blob_ref_records=" << stats.rewritten_primary_blob_ref_records
            << " preserved_referenced_missing_assets=" << stats.preserved_referenced_missing_assets
            << " sanitized_jsonl_files=" << stats.sanitized_jsonl_files
            << " dropped_jsonl_lines=" << stats.dropped_jsonl_lines
            << " truncated_inconsistent_jsonl_files=" << stats.truncated_inconsistent_jsonl_files
            << " truncated_inconsistent_jsonl_bytes=" << stats.truncated_inconsistent_jsonl_bytes
            << " truncated_inconsistent_frames=" << stats.truncated_inconsistent_frames
            << " surviving_frame_count=" << stats.surviving_frame_count
            << " refreshed_asset_hashes=" << stats.refreshed_asset_hashes
            << " hashed_unique_files=" << stats.hashed_unique_files
            << " hashed_asset_bytes=" << stats.hashed_asset_bytes
            << " refreshed_asset_bytes=" << stats.refreshed_asset_bytes
            << " primary_blob_id_conflicts=" << stats.primary_blob_id_conflicts
            << " checksum_files=" << stats.checksum_files
            << " checksum_hashed_files=" << stats.checksum_hashed_files
            << " checksum_hashed_bytes=" << stats.checksum_hashed_bytes
            << " jsonl_records=" << stats.jsonl_records
            << " input_bytes=" << stats.input_bytes
            << " output_bytes=" << stats.output_bytes
            << " rewritten_records=" << stats.rewritten_records
            << " jsonl_passes=" << stats.jsonl_passes
            << " digest_cache_hits=" << stats.digest_cache_hits
            << " digest_cache_misses=" << stats.digest_cache_misses
            << " pipeline_cache_hits=" << stats.pipeline_cache_hits
            << " pipeline_cache_misses=" << stats.pipeline_cache_misses
            << " referenced_paths_collected=" << stats.referenced_paths_collected
            << " rewritten_digest_files=" << stats.rewritten_digest_files
            << " checksum_reused_rewritten_files=" << stats.checksum_reused_rewritten_files
            << " checksum_reused_prior_files=" << stats.checksum_reused_prior_files
            << " checksum_reused_prior_bytes=" << stats.checksum_reused_prior_bytes
            << " asset_hash_reused_files=" << stats.asset_hash_reused_files
            << " asset_hash_reused_bytes=" << stats.asset_hash_reused_bytes
            << " spooled_assets=" << stats.spooled_assets
            << " spooled_asset_bytes=" << stats.spooled_asset_bytes
            << " materialized_spooled_assets=" << stats.materialized_spooled_assets
            << " removed_spool_files=" << stats.removed_spool_files
            << " reused_existing_canonical_files=" << stats.reused_existing_canonical_files
            << " reused_existing_canonical_bytes=" << stats.reused_existing_canonical_bytes
            << " verified_existing_canonical_files=" << stats.verified_existing_canonical_files
            << " verified_existing_canonical_bytes=" << stats.verified_existing_canonical_bytes
            << " removed_stale_canonical_files=" << stats.removed_stale_canonical_files
            << " removed_files=" << stats.removed_files
            << " dry_run=" << (options.dry_run ? "true" : "false")
            << " verify_existing_canonical=" << (options.verify_existing_canonical ? "true" : "false")
            << " verify_jsonl_records=" << (options.verify_jsonl_records ? "true" : "false")
            << " persist_replay_model_only=false"
            << " elapsed_s=" << elapsed << "\n";
  return 0;
}
