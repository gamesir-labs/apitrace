#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  std::uint64_t hashed_asset_bytes = 0;
  std::uint64_t refreshed_asset_bytes = 0;
  std::uint64_t checksum_hashed_bytes = 0;
  std::uint64_t duplicate_bytes = 0;
};

void print_usage(const char *argv0)
{
  std::cerr
      << "Usage: " << argv0 << " [--dry-run] [--keep-duplicates] [--jobs N] <trace-bundle>\n"
      << "\n"
      << "Finalizes a captured bundle offline: merges sideband assets, deduplicates assets,\n"
      << "rewrites text references, writes assets.json, and refreshes checksums.json.\n"
      << "\n"
      << "Options:\n"
      << "  --profile          Print per-stage timing and throughput to stderr.\n";
}

std::optional<Options> parse_args(int argc, char **argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--keep-duplicates") {
      options.keep_duplicates = true;
    } else if (arg == "--profile") {
      options.profile = true;
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
void run_stage(const Options &options, const char *name, Func &&func)
{
  const auto started = std::chrono::steady_clock::now();
  func();
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

std::string canonical_asset_path(const AssetEntry &asset)
{
  const auto directory = asset_directory_name(asset.kind);
  const auto filename = asset.digest + asset_extension(asset.kind);
  if (directory == ".") {
    return filename;
  }
  return (fs::path(directory) / filename).generic_string();
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

void hash_assets(const fs::path &bundle_root, std::vector<AssetEntry> &assets, std::size_t jobs, Stats &stats)
{
  struct PathGroup {
    std::string path;
    std::vector<std::size_t> asset_indices;
    std::string digest;
    std::uint64_t byte_size = 0;
  };

  std::vector<PathGroup> groups;
  std::unordered_map<std::string, std::size_t> group_by_path;
  groups.reserve(assets.size());
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto &asset = assets[index];
    if (!asset.file_exists) {
      continue;
    }
    const auto [it, inserted] = group_by_path.emplace(asset.path, groups.size());
    if (inserted) {
      PathGroup group;
      group.path = asset.path;
      group.byte_size = asset.actual_size;
      group.asset_indices.push_back(index);
      groups.push_back(std::move(group));
    } else {
      groups[it->second].asset_indices.push_back(index);
    }
  }

  std::atomic<std::size_t> next{0};
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
        group.digest = apitrace::trace::content_hash_file(bundle_root / group.path);
      } catch (const std::exception &error) {
        std::lock_guard<std::mutex> lock(error_mutex);
        errors.push_back(group.path + ": " + error.what());
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

void ensure_canonical_files(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    const Options &options,
    Stats &stats)
{
  std::unordered_map<std::string, std::vector<std::size_t>> groups;
  for (std::size_t index = 0; index < assets.size(); ++index) {
    const auto &asset = assets[index];
    if (!asset.file_exists || asset.digest.empty()) {
      continue;
    }
    const auto key = asset.kind + "#" + (asset.binary_payload ? "bin" : "text") + "#" +
                     std::to_string(asset.actual_size) + "#" + asset.digest;
    groups[key].push_back(index);
  }

  for (auto &[_, group] : groups) {
    std::sort(group.begin(), group.end(), [&](std::size_t lhs, std::size_t rhs) {
      return assets[lhs].path < assets[rhs].path;
    });
    auto &canonical = assets[group.front()];
    canonical.canonical_path = canonical_asset_path(canonical);
    const auto canonical_absolute = bundle_root / canonical.canonical_path;
    const auto source_absolute = bundle_root / canonical.path;

    if (!options.dry_run && canonical.path != canonical.canonical_path) {
      fs::create_directories(canonical_absolute.parent_path());
      std::error_code error;
      if (fs::exists(canonical_absolute, error)) {
        const auto existing_size = fs::file_size(canonical_absolute, error);
        if (error || existing_size != canonical.actual_size ||
            apitrace::trace::content_hash_file(canonical_absolute) != canonical.digest) {
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
        }
      }
    }

    for (const auto index : group) {
      auto &asset = assets[index];
      asset.canonical_path = canonical.canonical_path.empty() ? canonical.path : canonical.canonical_path;
      asset.content_hash = asset.digest;
      asset.byte_size = asset.actual_size;
      if (asset.path != asset.canonical_path) {
        ++stats.rewritten_assets;
      }
    }
    if (group.size() > 1) {
      stats.duplicate_assets += group.size() - 1;
      stats.duplicate_bytes += assets[group.front()].actual_size * (group.size() - 1);
    }
  }
}

std::unordered_map<std::string, std::string> build_aliases(const std::vector<AssetEntry> &assets)
{
  std::unordered_map<std::string, std::string> aliases;
  for (const auto &asset : assets) {
    if (!asset.canonical_path.empty() && asset.path != asset.canonical_path) {
      aliases.emplace(asset.path, asset.canonical_path);
    }
  }
  return aliases;
}

std::unordered_map<std::uint64_t, std::uint64_t> normalize_blob_ids(std::vector<AssetEntry> &assets, Stats &stats)
{
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
      std::cerr << "warning: remapped primary blob_id " << old_blob_id
                << " to " << asset.blob_id << " for " << path << "\n";
    }
  }
  return sideband_blob_remap;
}

void remove_duplicate_files(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::string> &aliases,
    const Options &options,
    Stats &stats)
{
  if (options.keep_duplicates) {
    return;
  }
  for (const auto &[old_path, new_path] : aliases) {
    (void)new_path;
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

void replace_all(std::string &text, const std::string &from, const std::string &to)
{
  if (from.empty() || from == to) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::unordered_set<std::string> rewrite_text_references(
    const fs::path &bundle_root,
    const std::unordered_map<std::string, std::string> &aliases,
    const Options &options,
    Stats &stats)
{
  std::unordered_set<std::string> rewritten_paths;
  if (aliases.empty()) {
    return rewritten_paths;
  }

  std::vector<std::pair<std::string, std::string>> replacements(aliases.begin(), aliases.end());
  std::sort(replacements.begin(), replacements.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.first.size() > rhs.first.size();
  });

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
    if (relative_text == apitrace::trace::kChecksumsFileName ||
        relative_text == apitrace::trace::kAssetIndexFileName) {
      continue;
    }
    const auto extension = it->path().extension().string();
    if (extension != ".json" && extension != ".jsonl") {
      continue;
    }

    std::ifstream input(it->path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto rewritten = text;
    for (const auto &[old_path, new_path] : replacements) {
      replace_all(rewritten, old_path, new_path);
    }
    if (rewritten == text) {
      continue;
    }
    ++stats.rewritten_text_files;
    rewritten_paths.insert(relative_text);
    if (!options.dry_run) {
      std::ofstream output(it->path(), std::ios::binary | std::ios::trunc);
      output << rewritten;
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

void sanitize_and_rewrite_jsonl_file(
    const fs::path &path,
    const std::unordered_map<std::uint64_t, std::uint64_t> &blob_id_remap,
    const Options &options,
    Stats &stats)
{
  if (!fs::is_regular_file(path)) {
    return;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return;
  }

  std::vector<std::string> lines;
  std::string line;
  bool changed = false;
  bool remapped_blob_refs = false;
  std::size_t dropped_lines = 0;
  while (std::getline(input, line)) {
    auto record = json::parse(line, nullptr, false);
    if (!record.is_discarded() && rewrite_blob_refs(record, blob_id_remap)) {
      lines.push_back(record.dump());
      changed = true;
      remapped_blob_refs = true;
    } else if (!record.is_discarded()) {
      lines.push_back(std::move(line));
    } else {
      changed = true;
      ++dropped_lines;
    }
  }

  if (!changed) {
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

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  for (const auto &rewritten_line : lines) {
    output << rewritten_line << '\n';
  }
}

void sanitize_jsonl_tail_file(const fs::path &path, const Options &options, Stats &stats)
{
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size == 0) {
    return;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
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
}

void write_asset_index(const fs::path &bundle_root, const std::vector<AssetEntry> &assets, const Options &options)
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
    std::ofstream output(bundle_root / apitrace::trace::kAssetIndexFileName, std::ios::binary | std::ios::trunc);
    output << root.dump(2) << "\n";
  }
}

void write_sideband_asset_index(const fs::path &bundle_root, const std::vector<AssetEntry> &assets, const Options &options)
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

  if (!options.dry_run) {
    write_asset_index(bundle_root / apitrace::trace::kAnalysisDirectoryName, sideband_assets, options);
    std::error_code error;
    fs::rename(
        bundle_root / apitrace::trace::kAnalysisDirectoryName / apitrace::trace::kAssetIndexFileName,
        bundle_root / kSidebandAssetIndexPath,
        error);
    if (error) {
      std::cerr << "warning: failed to refresh sideband asset index: " << error.message() << "\n";
    }
  }
}

void refresh_asset_metadata_from_files(
    const fs::path &bundle_root,
    std::vector<AssetEntry> &assets,
    const std::unordered_set<std::string> &rewritten_paths,
    std::size_t jobs,
    Stats &stats)
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
        group.digest = apitrace::trace::content_hash_file(absolute);
        group.size = size;
      } catch (const std::exception &exception) {
        std::lock_guard<std::mutex> lock(error_mutex);
        errors.push_back(group.path + ": " + exception.what());
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

void write_checksums(const fs::path &bundle_root, const std::vector<AssetEntry> &assets, const Options &options, Stats &stats)
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

  std::unordered_map<std::string, std::pair<std::string, std::uint64_t>> known;
  for (const auto &asset : assets) {
    if (!asset.canonical_path.empty() && !asset.content_hash.empty()) {
      known[asset.canonical_path] = {asset.content_hash, asset.byte_size};
    }
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
    if (relative_text == apitrace::trace::kChecksumsFileName) {
      continue;
    }
    const auto size = static_cast<std::uint64_t>(fs::file_size(it->path(), error));
    if (error) {
      continue;
    }
    auto digest = known.find(relative_text);
    if (digest == known.end() || digest->second.second != size) {
      ++stats.checksum_hashed_files;
      stats.checksum_hashed_bytes += size;
      records.push_back({relative_text, std::string(), size});
      hash_tasks.push_back({records.size() - 1, it->path()});
    } else {
      records.push_back({relative_text, digest->second.first, digest->second.second});
    }
  }
  stats.checksum_files = records.size();

  std::atomic<std::size_t> next{0};
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
        records[task.record_index].digest = apitrace::trace::content_hash_file(task.absolute_path);
      } catch (const std::exception &exception) {
        std::lock_guard<std::mutex> lock(error_mutex);
        errors.push_back(records[task.record_index].path + ": " + exception.what());
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
  Stats stats;
  std::vector<AssetEntry> assets;
  run_stage(options, "load_asset_indexes", [&] {
    load_asset_index(options.bundle_root / apitrace::trace::kAssetIndexFileName, AssetSource::Primary, assets, stats);
    load_asset_index(options.bundle_root / kSidebandAssetIndexPath, AssetSource::Sideband, assets, stats);
  });
  run_stage(options, "merge_assets", [&] {
    assets = merge_assets(std::move(assets));
    stats.indexed_assets = assets.size();
  });
  run_stage(options, "stat_assets", [&] {
    stat_assets(options.bundle_root, assets);
  });
  run_stage(options, "hash_assets", [&] {
    hash_assets(options.bundle_root, assets, options.jobs, stats);
  });
  run_stage(options, "ensure_canonical_files", [&] {
    ensure_canonical_files(options.bundle_root, assets, options, stats);
  });

  std::unordered_map<std::uint64_t, std::uint64_t> blob_id_remap;
  run_stage(options, "normalize_blob_ids", [&] {
    blob_id_remap = normalize_blob_ids(assets, stats);
  });
  std::unordered_map<std::string, std::string> aliases;
  run_stage(options, "build_aliases", [&] {
    aliases = build_aliases(assets);
  });
  std::unordered_set<std::string> rewritten_paths;
  run_stage(options, "rewrite_text_references", [&] {
    rewritten_paths = rewrite_text_references(options.bundle_root, aliases, options, stats);
  });
  run_stage(options, "sanitize_jsonl", [&] {
    const auto callstream = options.bundle_root / apitrace::trace::kCallstreamFileName;
    const auto metal_callstream = options.bundle_root / apitrace::trace::kMetalCallstreamFileName;
    const auto translation_links =
        options.bundle_root / apitrace::trace::kAnalysisDirectoryName / apitrace::trace::kTranslationLinksFileName;
    if (blob_id_remap.empty()) {
      sanitize_jsonl_tail_file(callstream, options, stats);
      sanitize_jsonl_tail_file(metal_callstream, options, stats);
      sanitize_jsonl_tail_file(translation_links, options, stats);
    } else {
      sanitize_jsonl_tail_file(callstream, options, stats);
      sanitize_and_rewrite_jsonl_file(metal_callstream, blob_id_remap, options, stats);
      sanitize_and_rewrite_jsonl_file(translation_links, blob_id_remap, options, stats);
    }
  });
  run_stage(options, "remove_duplicate_files", [&] {
    remove_duplicate_files(options.bundle_root, aliases, options, stats);
  });
  if (!options.dry_run) {
    run_stage(options, "refresh_asset_metadata", [&] {
      refresh_asset_metadata_from_files(options.bundle_root, assets, rewritten_paths, options.jobs, stats);
    });
  }
  run_stage(options, "write_asset_indexes", [&] {
    write_asset_index(options.bundle_root, assets, options);
    write_sideband_asset_index(options.bundle_root, assets, options);
  });
  run_stage(options, "write_checksums", [&] {
    write_checksums(options.bundle_root, assets, options, stats);
  });

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "bundle-finalize: "
            << "input_assets=" << stats.input_assets
            << " indexed_assets=" << stats.indexed_assets
            << " hashed_assets=" << stats.hashed_assets
            << " rewritten_assets=" << stats.rewritten_assets
            << " duplicate_assets=" << stats.duplicate_assets
            << " duplicate_bytes=" << stats.duplicate_bytes
            << " remapped_blob_ids=" << stats.remapped_blob_ids
            << " rewritten_text_files=" << stats.rewritten_text_files
            << " rewritten_blob_ref_files=" << stats.rewritten_blob_ref_files
            << " sanitized_jsonl_files=" << stats.sanitized_jsonl_files
            << " dropped_jsonl_lines=" << stats.dropped_jsonl_lines
            << " refreshed_asset_hashes=" << stats.refreshed_asset_hashes
            << " hashed_unique_files=" << stats.hashed_unique_files
            << " hashed_asset_bytes=" << stats.hashed_asset_bytes
            << " refreshed_asset_bytes=" << stats.refreshed_asset_bytes
            << " checksum_files=" << stats.checksum_files
            << " checksum_hashed_files=" << stats.checksum_hashed_files
            << " checksum_hashed_bytes=" << stats.checksum_hashed_bytes
            << " removed_files=" << stats.removed_files
            << " dry_run=" << (options.dry_run ? "true" : "false")
            << " elapsed_s=" << elapsed << "\n";
  return 0;
}
