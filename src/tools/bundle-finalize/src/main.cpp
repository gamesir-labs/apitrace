#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"

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

enum class AssetSource {
  Primary,
  Sideband,
};

struct Options {
  fs::path bundle_root;
  std::size_t jobs = std::max(1u, std::thread::hardware_concurrency());
  bool dry_run = false;
  bool keep_duplicates = false;
  bool profile = false;
  bool progress = false;
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
  bool metal = false;
  bool binary_payload = true;
  AssetSource source = AssetSource::Primary;
  bool file_exists = false;
  bool safe_path = false;
  std::string digest;
  std::string canonical_path;
};

struct ShaderAssetRef {
  std::uint64_t blob_id = 0;
  std::uint64_t byte_size = 0;
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
  std::size_t sanitized_jsonl_files = 0;
  std::size_t dropped_jsonl_lines = 0;
  std::size_t refreshed_asset_hashes = 0;
  std::size_t checksum_files = 0;
  std::size_t checksum_hashed_files = 0;
  std::size_t hashed_unique_files = 0;
  std::size_t primary_blob_id_conflicts = 0;
  std::size_t restored_inline_query_assets = 0;
  std::size_t rebuilt_d3d12_pipeline_assets = 0;
  std::size_t incomplete_d3d12_pipeline_semantics = 0;
  std::size_t preserved_referenced_duplicate_assets = 0;
  std::size_t dropped_unreferenced_missing_assets = 0;
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
      << "  --progress         Force interactive progress on stderr.\n"
      << "  --no-progress      Disable interactive progress on stderr.\n";
}

std::optional<Options> parse_args(int argc, char **argv)
{
  Options options;
  options.progress = stderr_is_tty() && !ci_environment();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--keep-duplicates") {
      options.keep_duplicates = true;
    } else if (arg == "--profile") {
      options.profile = true;
    } else if (arg == "--progress") {
      options.progress = true;
    } else if (arg == "--no-progress") {
      options.progress = false;
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

bool is_canonical_jsonl_asset_path_value(const std::string &path)
{
  return path.rfind("buffers/asset-", 0) == 0 ||
         path.rfind("shaders/asset-", 0) == 0 ||
         path.rfind("pipelines/asset-", 0) == 0 ||
         path.rfind("metal/libraries/asset-", 0) == 0;
}

bool jsonl_line_may_contain_rewritable_asset_path(const std::string &line)
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

bool text_may_contain_rewritable_asset_path(const std::string &text)
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
  asset.source = AssetSource::Primary;
  return asset;
}

void add_discovered_asset(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    Stats &stats,
    const std::unordered_set<std::uint64_t> &indexed_blob_ids,
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
  ++stats.input_assets;
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
    const std::unordered_set<std::uint64_t> &indexed_blob_ids,
    bool legacy_asset_discovery,
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

    if (record.value("record_kind", std::string()) == "object_create" &&
        paths.size() == 1 &&
        record.contains("object_id") &&
        record["object_id"].is_number_unsigned()) {
      asset_path_by_object_id[record["object_id"].get<std::uint64_t>()] = paths.front();
    }

    const auto function = record.value("function", std::string());
    const auto payload = record.value("payload", json::object());
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
      stats);
  discover_raw_asset_references_from_jsonl(
      bundle_root,
      fs::path(apitrace::trace::kMetalCallstreamFileName),
      assets,
      indexed_blob_ids,
      legacy_asset_discovery,
      stats);
  discover_raw_asset_references_from_jsonl(
      bundle_root,
      fs::path(apitrace::trace::kAnalysisDirectoryName) / apitrace::trace::kTranslationLinksFileName,
      assets,
      indexed_blob_ids,
      legacy_asset_discovery,
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
  return asset.actual_size != 0 ? asset.actual_size : asset.byte_size;
}

void stat_assets(const fs::path &bundle_root, std::vector<AssetEntry> &assets)
{
  for (auto &asset : assets) {
    const fs::path relative(asset.path);
    asset.safe_path = safe_relative_path(relative);
    if (!asset.safe_path) {
      continue;
    }
    const auto absolute = bundle_root / relative;
    std::error_code error;
    if (!fs::is_regular_file(absolute, error) || error) {
      if (!asset.content_hash.empty() && asset.byte_size != 0) {
        asset.digest = asset.content_hash;
        asset.actual_size = asset.byte_size;
      }
      continue;
    }
    const auto size = fs::file_size(absolute, error);
    if (error) {
      continue;
    }
    asset.actual_size = static_cast<std::uint64_t>(size);
    asset.file_exists = true;
    if (asset.byte_size == 0) {
      asset.byte_size = asset.actual_size;
    }
  }
}

void hash_assets(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    std::size_t jobs,
    Stats &stats,
    FileDigestCache &digest_cache,
    ProgressReporter *progress)
{
  struct PathGroup {
    std::string path;
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
    if (!asset.file_exists) {
      continue;
    }
    if (!asset.content_hash.empty() &&
        asset.byte_size != 0 &&
        asset.actual_size == asset.byte_size) {
      auto &mutable_asset = assets[index];
      mutable_asset.digest = mutable_asset.content_hash;
      ++stats.hashed_assets;
      ++stats.asset_hash_reused_files;
      stats.asset_hash_reused_bytes += mutable_asset.actual_size;
      continue;
    }
    const auto [it, inserted] = group_by_path.emplace(asset.path, groups.size());
    if (inserted) {
      PathGroup group;
      group.path = asset.path;
      group.byte_size = asset.actual_size;
      group.asset_indices.push_back(index);
      total_bytes += group.byte_size;
      groups.push_back(std::move(group));
    } else {
      groups[it->second].asset_indices.push_back(index);
    }
  }

  std::atomic<std::size_t> next{0};
  std::atomic<std::uint64_t> completed_files{0};
  std::atomic<std::uint64_t> completed_bytes{0};
  std::mutex error_mutex;
  std::vector<std::string> errors;
  const auto worker = [&]() {
    for (;;) {
      const auto cursor = next.fetch_add(1, std::memory_order_relaxed);
      if (cursor >= groups.size()) {
        return;
      }
      auto &group = groups[cursor];
      try {
        group.digest = digest_cache.digest_file(bundle_root / group.path);
      } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(error_mutex);
        errors.push_back(group.path + ": " + error.what());
      }
      const auto done_files = completed_files.fetch_add(1, std::memory_order_relaxed) + 1;
      const auto done_bytes = completed_bytes.fetch_add(group.byte_size, std::memory_order_relaxed) + group.byte_size;
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
    std::cerr << "warning: hash failed for " << error << "\n";
  }

  for (const auto &group : groups) {
    if (group.digest.empty()) {
      continue;
    }
    ++stats.hashed_unique_files;
    stats.hashed_asset_bytes += group.byte_size;
    for (const auto index : group.asset_indices) {
      assets[index].digest = group.digest;
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
  std::unordered_map<std::string, std::vector<std::size_t>> groups;
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto &asset = assets[index];
    if ((asset.actual_size == 0 && asset.byte_size == 0) || asset.digest.empty()) {
      continue;
    }
    const auto key = asset.kind + "#" + (asset.binary_payload ? "bin" : "text") + "#" +
                     std::to_string(asset.actual_size ? asset.actual_size : asset.byte_size) + "#" +
                     asset.digest;
    groups[key].push_back(index);
  }

  for (auto &[_, group] : groups) {
    std::sort(group.begin(), group.end(), [&](std::size_t lhs, std::size_t rhs) {
      if (assets[lhs].file_exists != assets[rhs].file_exists) {
        return assets[lhs].file_exists;
      }
      return assets[lhs].path < assets[rhs].path;
    });
    auto &canonical = assets[group.front()];
    canonical.canonical_path = canonical_asset_path(canonical);
    const auto canonical_absolute = bundle_root / canonical.canonical_path;
    const auto source_absolute = bundle_root / canonical.path;

    if (!options.dry_run && canonical.file_exists &&
        canonical.path != canonical.canonical_path) {
      fs::create_directories(canonical_absolute.parent_path());
      std::error_code error;
      if (fs::exists(canonical_absolute, error)) {
        const auto existing_size = fs::file_size(canonical_absolute, error);
        if (error || existing_size != canonical.actual_size ||
            digest_cache.digest_file(canonical_absolute) != canonical.digest) {
          fs::remove(canonical_absolute, error);
        }
      }
      if (!fs::exists(canonical_absolute, error)) {
        fs::create_hard_link(source_absolute, canonical_absolute, error);
        if (error) {
          fs::copy_file(source_absolute, canonical_absolute, fs::copy_options::overwrite_existing, error);
        }
        if (error) {
          std::cerr << "warning: failed to create canonical asset " << canonical.canonical_path
                    << " from " << canonical.path << ": " << error.message() << "\n";
          canonical.canonical_path = canonical.path;
        } else {
          digest_cache.remember(canonical_absolute, canonical.digest, canonical.actual_size);
        }
      }
    }

    for (const auto index : group) {
      auto &asset = assets[index];
      asset.canonical_path = canonical.canonical_path.empty() ? canonical.path : canonical.canonical_path;
      asset.content_hash = asset.digest;
      asset.byte_size = asset.actual_size ? asset.actual_size : asset.byte_size;
      if (asset.path != asset.canonical_path) {
        aliases[asset.path] = asset.canonical_path;
        ++stats.rewritten_assets;
      }
    }
    if (group.size() > 1) {
      stats.duplicate_assets += group.size() - 1;
      stats.duplicate_bytes +=
          (assets[group.front()].actual_size ? assets[group.front()].actual_size
                                             : assets[group.front()].byte_size) *
          (group.size() - 1);
    }
  }
  return aliases;
}

std::unordered_map<std::string, std::string> build_aliases(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::string, std::string> aliases;
  for (const auto &asset : assets) {
    auto canonical_path = asset.canonical_path;
    if (canonical_path.empty() && !asset.digest.empty()) {
      canonical_path = canonical_asset_path(asset);
    }
    if (!canonical_path.empty() && asset.path != canonical_path) {
      aliases[asset.path] = canonical_path;
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

std::unordered_map<std::uint64_t, std::uint64_t> normalize_blob_ids(std::vector<AssetEntry> &assets, Stats &stats)
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
    const std::unordered_map<std::string, std::string> &aliases,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    const Options &options,
    Stats &stats)
{
  if (options.keep_duplicates) {
    return;
  }
  for (const auto &[old_path, new_path] : aliases) {
    (void)new_path;
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

std::unordered_set<std::string> collect_referenced_asset_paths(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::string> &aliases)
{
  std::unordered_set<std::string> referenced_paths;
  if (aliases.empty()) {
    return referenced_paths;
  }
  for (const auto &relative : text_reference_files(bundle_root)) {
    const auto absolute = bundle_root / relative;
    std::ifstream input(absolute, std::ios::binary);
    if (!input.is_open()) {
      continue;
    }

    if (relative.extension() == ".jsonl") {
      std::string line;
      while (std::getline(input, line)) {
        if (!jsonl_line_may_contain_rewritable_asset_path(line)) {
          continue;
        }
        const auto record = json::parse(line, nullptr, false);
        if (!record.is_discarded()) {
          collect_path_references_from_json(record, aliases, referenced_paths);
        }
      }
      continue;
    }

    std::ostringstream text_buffer;
    text_buffer << input.rdbuf();
    const auto text = text_buffer.str();
    if (!text_may_contain_rewritable_asset_path(text)) {
      continue;
    }
    const auto root = json::parse(text, nullptr, false);
    if (!root.is_discarded()) {
      collect_path_references_from_json(root, aliases, referenced_paths);
    }
  }
  return referenced_paths;
}

void drop_unreferenced_missing_assets(
    std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &referenced_paths_after_rewrite,
    Stats &stats)
{
  const auto old_size = assets.size();
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
              return false;
            }
            return true;
          }),
      assets.end());
  stats.dropped_unreferenced_missing_assets += old_size - assets.size();
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

class HashedOutputFile {
public:
  bool open(const fs::path &path)
  {
    output_.open(path, std::ios::binary | std::ios::trunc);
    return output_.is_open();
  }

  void write_line(const std::string &line)
  {
    output_ << line << '\n';
    hasher_.update(line);
    hasher_.update("\n");
    size_ += static_cast<std::uint64_t>(line.size()) + 1;
  }

  void write_text(const std::string &text)
  {
    output_ << text;
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

  std::error_code error;
  const auto pipeline_dir = bundle_root / "pipelines";
  if (!fs::is_directory(pipeline_dir, error) || error) {
    return files;
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
  return files;
}

std::unordered_set<std::string> rewrite_text_references(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::string> &aliases,
    const std::unordered_map<std::string, std::uint64_t> &blob_id_by_effective_path,
    const std::unordered_map<std::uint64_t, std::string> &effective_path_by_blob_id,
    const Options &options,
    Stats &stats,
    ProgressReporter *progress,
    std::unordered_set<std::string> *referenced_paths = nullptr,
    FileDigestCache *digest_cache = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> *rewritten_digests = nullptr)
{
  std::unordered_set<std::string> rewritten_paths;
  if (aliases.empty() && effective_path_by_blob_id.empty()) {
    return rewritten_paths;
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
    std::ifstream input(absolute, std::ios::binary);
    if (!input.is_open()) {
      ++file_index;
      continue;
    }

    json root;
    if (relative.extension() == ".jsonl") {
      ++stats.jsonl_passes;
      bool changed = false;
      const auto temporary = temporary_rewrite_path(absolute);
      HashedOutputFile output;
      if (!options.dry_run) {
        output.open(temporary);
        if (!output.is_open()) {
          std::cerr << "warning: failed to open temporary rewrite file " << temporary << "\n";
          ++file_index;
          continue;
        }
      }
      std::string line;
      while (std::getline(input, line)) {
        ++stats.jsonl_records;
        stats.input_bytes += static_cast<std::uint64_t>(line.size()) + 1;
        if (!jsonl_line_may_contain_rewritable_asset_path(line) &&
            (effective_path_by_blob_id.empty() || line.find("asset-") == std::string::npos)) {
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
        const bool paths_changed = rewrite_path_refs(record, aliases);
        const bool single_blob_asset_alias_changed =
            rewrite_single_blob_asset_alias_refs(record, effective_path_by_blob_id);
        const bool blob_refs_changed =
            rewrite_blob_refs_for_effective_paths(
                record,
                blob_id_by_effective_path,
                paths_changed || single_blob_asset_alias_changed);
        if (paths_changed || single_blob_asset_alias_changed || blob_refs_changed) {
          if (referenced_paths) {
            collect_path_references_from_json(record, aliases, *referenced_paths, false);
          }
          const auto rewritten = record.dump();
          if (!options.dry_run) {
            output.write_line(rewritten);
          }
          note_jsonl_output(stats, rewritten);
          changed = true;
          ++stats.rewritten_records;
        } else {
          if (referenced_paths) {
            collect_path_references_from_json(record, aliases, *referenced_paths, false);
          }
          if (!options.dry_run) {
            output.write_line(line);
          }
          note_jsonl_output(stats, line);
        }
      }
      if (!changed) {
        if (!options.dry_run) {
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

    std::ostringstream text_buffer;
    text_buffer << input.rdbuf();
    const auto text = text_buffer.str();
    if (!text_may_contain_rewritable_asset_path(text)) {
      ++file_index;
      processed_bytes += size_error ? 0 : file_size;
      if (progress) {
        progress->update(file_index, reference_files.size(), processed_bytes, total_bytes);
      }
      continue;
    }
    root = json::parse(text, nullptr, false);
    const bool paths_changed = !root.is_discarded() && rewrite_path_refs(root, aliases);
    const bool single_blob_asset_alias_changed =
        !root.is_discarded() && rewrite_single_blob_asset_alias_refs(root, effective_path_by_blob_id);
    const bool blob_refs_changed =
        !root.is_discarded() &&
        rewrite_blob_refs_for_effective_paths(
            root,
            blob_id_by_effective_path,
            paths_changed || single_blob_asset_alias_changed);
    if (root.is_discarded() || (!paths_changed && !single_blob_asset_alias_changed && !blob_refs_changed)) {
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
      if (path_it != shader_ref_by_path.end() &&
          (bytecode_size == 0 || path_it->second.byte_size == bytecode_size)) {
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
        line.find("CreatePipelineState") == std::string::npos) {
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
    } else if (legacy_asset_discovery && repair_pipeline_asset_event(
                   bundle_root,
                   record,
                   options,
                   legacy_asset_discovery,
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

void write_checksums(
    const fs::path &bundle_root,
    const std::vector<AssetEntry> &assets,
    const Options &options,
    Stats &stats,
    FileDigestCache &digest_cache,
    ProgressReporter *progress,
    const std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> &rewritten_digests = {})
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
    if (!asset.path.empty()) {
      known[asset.path] = {asset.content_hash, asset.byte_size};
    }
  }
  for (const auto &[path, digest_and_size] : rewritten_digests) {
    known[path] = digest_and_size;
  }

  std::vector<ChecksumRecord> records;
  std::vector<HashTask> hash_tasks;
  std::error_code error;
  for (fs::recursive_directory_iterator it(bundle_root, error), end; it != end && !error; it.increment(error)) {
    if (!it->is_regular_file(error) || error) {
      continue;
    }
    const auto relative = fs::relative(it->path(), bundle_root, error);
    if (error) {
      continue;
    }
    const auto relative_text = relative.generic_string();
    if (relative_text == apitrace::trace::kChecksumsFileName || is_host_metadata_file(relative)) {
      continue;
    }
    const auto size = static_cast<std::uint64_t>(fs::file_size(it->path(), error));
    if (error) {
      continue;
    }
    auto digest = known.find(relative_text);
    if (digest == known.end() || digest->second.second != size) {
      const auto prior = prior_checksums.find(relative_text);
      const auto mtime = fs::last_write_time(it->path(), error);
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
        hash_tasks.push_back({records.size() - 1, it->path()});
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

} // namespace

int main(int argc, char **argv)
{
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

  const auto started = std::chrono::steady_clock::now();
  constexpr std::size_t kStageCount = 18;
  std::size_t stage_index = 0;
  ProgressReporter progress(options.progress);
  Stats stats;
  FileDigestCache digest_cache;
  std::vector<AssetEntry> assets;
  std::unordered_map<std::string, std::string> canonical_aliases;
  std::size_t loaded_asset_count = 0;
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
      discover_raw_asset_references(options.bundle_root, assets, legacy_asset_discovery, stats);
    }
  });
  run_stage(options, progress, stage_index, kStageCount, "merge_assets", [&] {
    assets = merge_assets(std::move(assets));
    stats.indexed_assets = assets.size();
  });
  run_stage(options, progress, stage_index, kStageCount, "stat_assets", [&] {
    stat_assets(options.bundle_root, assets);
  });
  run_stage(options, progress, stage_index, kStageCount, "hash_assets", [&] {
    hash_assets(options.bundle_root, assets, options.jobs, stats, digest_cache, &progress);
  });
  run_stage(options, progress, stage_index, kStageCount, "ensure_canonical_files", [&] {
    const auto stage_aliases = ensure_canonical_files(options.bundle_root, assets, options, stats, digest_cache);
    for (const auto &[from, to] : stage_aliases) {
      canonical_aliases[from] = to;
    }
  });

  std::unordered_map<std::uint64_t, std::uint64_t> blob_id_remap;
  run_stage(options, progress, stage_index, kStageCount, "normalize_blob_ids", [&] {
    blob_id_remap = normalize_blob_ids(assets, stats);
  });
  std::unordered_map<std::string, std::string> aliases;
  std::unordered_map<std::string, std::uint64_t> blob_id_by_effective_path;
  std::unordered_map<std::uint64_t, std::string> effective_path_by_blob_id;
  std::unordered_set<std::string> referenced_paths_after_rewrite;
  std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> rewritten_digests;
  run_stage(options, progress, stage_index, kStageCount, "build_aliases", [&] {
    aliases = build_aliases(assets);
    for (const auto &[from, to] : canonical_aliases) {
      aliases[from] = to;
    }
    if (loaded_asset_count == 0 && legacy_asset_discovery) {
      const auto initial_pipeline_file_aliases = build_pipeline_file_aliases(options.bundle_root, digest_cache);
      for (const auto &[from, to] : initial_pipeline_file_aliases) {
        aliases[from] = to;
      }
    }
    blob_id_by_effective_path = build_blob_id_by_effective_path(assets);
    effective_path_by_blob_id = build_effective_path_by_blob_id(assets);
  });
  std::unordered_set<std::string> rewritten_paths;
  bool rebuilt_assets_changed = false;
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
            options,
            stats,
            &progress,
            legacy_asset_discovery ? &referenced_paths_after_rewrite : nullptr,
            &digest_cache,
            &rewritten_digests);
  });
  run_stage(options, progress, stage_index, kStageCount, "sanitize_tail", [&] {
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
      hash_assets(options.bundle_root, assets, options.jobs, stats, digest_cache, &progress);
      const auto stage_aliases = ensure_canonical_files(options.bundle_root, assets, options, stats, digest_cache);
      for (const auto &[from, to] : stage_aliases) {
        canonical_aliases[from] = to;
      }
      const auto rebuilt_blob_id_remap = normalize_blob_ids(assets, stats);
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
    for (const auto &[from, to] : canonical_aliases) {
      aliases[from] = to;
    }
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
            options,
            stats,
            &progress,
            legacy_asset_discovery ? &referenced_paths_after_rewrite : nullptr,
            &digest_cache,
            &rewritten_digests);
    rewritten_paths.insert(rebuilt_rewrites.begin(), rebuilt_rewrites.end());
    if (!rebuilt_rewrites.empty()) {
      stat_assets(options.bundle_root, assets);
      hash_assets(options.bundle_root, assets, options.jobs, stats, digest_cache, &progress);
      const auto stage_aliases = ensure_canonical_files(options.bundle_root, assets, options, stats, digest_cache);
      for (const auto &[from, to] : stage_aliases) {
        canonical_aliases[from] = to;
      }
      assets = merge_assets(std::move(assets));
    }
    aliases = build_aliases(assets);
    for (const auto &[from, to] : canonical_aliases) {
      aliases[from] = to;
    }
    if (loaded_asset_count == 0 && legacy_asset_discovery) {
      const auto pipeline_file_aliases = build_pipeline_file_aliases(options.bundle_root, digest_cache);
      for (const auto &[from, to] : pipeline_file_aliases) {
        aliases[from] = to;
      }
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
    if (!options.dry_run && !aliases.empty() && referenced_paths_after_rewrite.empty()) {
      referenced_paths_after_rewrite = collect_referenced_asset_paths(options.bundle_root, aliases);
    }
    stats.referenced_paths_collected = referenced_paths_after_rewrite.size();
    drop_unreferenced_missing_assets(assets, referenced_paths_after_rewrite, stats);
  });
  run_stage(options, progress, stage_index, kStageCount, "remove_duplicate_files", [&] {
    remove_duplicate_files(options.bundle_root, aliases, referenced_paths_after_rewrite, options, stats);
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
  run_stage(options, progress, stage_index, kStageCount, "write_checksums", [&] {
    write_checksums(options.bundle_root, assets, options, stats, digest_cache, &progress, rewritten_digests);
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
            << " duplicate_assets=" << stats.duplicate_assets
            << " duplicate_bytes=" << stats.duplicate_bytes
            << " restored_inline_query_assets=" << stats.restored_inline_query_assets
            << " rebuilt_d3d12_pipeline_assets=" << stats.rebuilt_d3d12_pipeline_assets
            << " repaired_d3d12_pipeline_events=" << stats.repaired_d3d12_pipeline_events
            << " incomplete_d3d12_pipeline_semantics=" << stats.incomplete_d3d12_pipeline_semantics
            << " unresolved_d3d12_pipeline_asset_refs=" << stats.unresolved_d3d12_pipeline_asset_refs
            << " preserved_referenced_duplicate_assets=" << stats.preserved_referenced_duplicate_assets
            << " dropped_unreferenced_missing_assets=" << stats.dropped_unreferenced_missing_assets
            << " remapped_blob_ids=" << stats.remapped_blob_ids
            << " rewritten_text_files=" << stats.rewritten_text_files
            << " rewritten_blob_ref_files=" << stats.rewritten_blob_ref_files
            << " sanitized_jsonl_files=" << stats.sanitized_jsonl_files
            << " dropped_jsonl_lines=" << stats.dropped_jsonl_lines
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
            << " removed_files=" << stats.removed_files
            << " dry_run=" << (options.dry_run ? "true" : "false")
            << " elapsed_s=" << elapsed << "\n";
  return 0;
}
