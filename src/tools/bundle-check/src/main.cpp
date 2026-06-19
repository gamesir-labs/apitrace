#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"
#include "apitrace/checksum_index.hpp"
#include "apitrace/tools/cli_entries.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;

std::size_t default_job_count()
{
  return static_cast<std::size_t>(std::thread::hardware_concurrency()) + 2;
}

struct CheckOptions {
  std::size_t jobs = default_job_count();
};

struct ChecksumValidationIssue {
  std::string relative;
  std::string message;
};

struct ChecksumValidationResult {
  std::size_t checked = 0;
  std::vector<ChecksumValidationIssue> issues;
};

void print_usage(const char *argv0)
{
  std::cerr << "usage: " << (argv0 ? argv0 : "bundle-check")
            << " [--verify-hashes] [--jobs N] <trace-bundle>\n"
            << "\n"
            << "bundle-check is an integrity checker for transferred bundles only.\n"
            << "It validates checksums.json by comparing listed file hashes and the manifest\n"
            << "bundle_hash. It does not inspect callstream/assets/replay semantics.\n";
}

bool path_is_safe(const std::filesystem::path &path)
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

std::string file_label(const std::filesystem::path &path)
{
  return path.empty() ? std::string("<unknown>") : path.generic_string();
}

bool parse_checksum_value(
    const json &value,
    const std::string &relative_path,
    const std::filesystem::path &checksums_path,
    apitrace::trace::ChecksumRecord &record,
    std::string &error)
{
  if (!value.is_string() && !value.is_object()) {
    error = file_label(checksums_path) + ": file digest must be a string or object";
    return false;
  }

  const std::string encoded =
      value.is_string() ? value.get<std::string>() : value.value("digest", std::string());
  const auto separator = encoded.find(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 >= encoded.size()) {
    error = file_label(checksums_path) + ": malformed digest entry for " + relative_path;
    return false;
  }

  record.relative_path = relative_path;
  record.algorithm = encoded.substr(0, separator);
  const auto size_separator = encoded.find(':', separator + 1);
  record.digest = encoded.substr(
      separator + 1,
      size_separator == std::string::npos ? std::string::npos : size_separator - separator - 1);
  if (record.digest.empty()) {
    error = file_label(checksums_path) + ": empty digest entry for " + relative_path;
    return false;
  }

  if (size_separator != std::string::npos) {
    try {
      record.byte_size = std::stoull(encoded.substr(size_separator + 1));
      record.has_byte_size = true;
    } catch (const std::exception &) {
      error = file_label(checksums_path) + ": malformed byte size for " + relative_path;
      return false;
    }
  }

  if (value.is_object() && value.contains("byte_size")) {
    if (!value["byte_size"].is_number_unsigned()) {
      error = file_label(checksums_path) + ": malformed byte size for " + relative_path;
      return false;
    }
    record.byte_size = value["byte_size"].get<std::uint64_t>();
    record.has_byte_size = true;
  }
  return true;
}

bool parse_checksums(
    const std::filesystem::path &checksums_path,
    apitrace::trace::ChecksumIndex &checksums,
    std::string &error)
{
  std::ifstream input(checksums_path, std::ios::binary);
  if (!input.is_open()) {
    error = "missing required file: " + file_label(checksums_path);
    return false;
  }

  const auto parsed = json::parse(input, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    error = file_label(checksums_path) + ": invalid JSON";
    return false;
  }

  checksums = apitrace::trace::ChecksumIndex{};
  checksums.format_version = parsed.value("format_version", checksums.format_version);
  checksums.bundle_hash = parsed.value("bundle_hash", std::string());

  const auto files_it = parsed.find("files");
  if (files_it == parsed.end() || !files_it->is_object()) {
    error = file_label(checksums_path) + ": files must be an object";
    return false;
  }

  for (const auto &[relative_path, digest_value] : files_it->items()) {
    apitrace::trace::ChecksumRecord record;
    if (!parse_checksum_value(digest_value, relative_path, checksums_path, record, error)) {
      return false;
    }
    if (!path_is_safe(record.relative_path)) {
      error = file_label(checksums_path) + ": unsafe checksum path: " + relative_path;
      return false;
    }
    if (record.relative_path == std::filesystem::path(apitrace::trace::kChecksumsFileName)) {
      error = file_label(checksums_path) + ": checksums.json must not checksum itself";
      return false;
    }
    checksums.files.push_back(std::move(record));
  }
  return true;
}

std::string bundle_hash_from_records(const std::vector<apitrace::trace::ChecksumRecord> &files)
{
  auto sorted_files = files;
  std::sort(sorted_files.begin(), sorted_files.end(), [](const auto &lhs, const auto &rhs) {
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
  return "sha256:" + apitrace::trace::content_hash_bytes(
                       bundle_fingerprint_source.data(),
                       bundle_fingerprint_source.size());
}

std::optional<ChecksumValidationIssue> validate_checksum_record_contents(
    const std::filesystem::path &bundle_root,
    const apitrace::trace::ChecksumRecord &record)
{
  const auto relative = record.relative_path.generic_string();
  if (record.algorithm != "sha256") {
    return ChecksumValidationIssue{relative, "unsupported algorithm"};
  }

  const auto absolute = bundle_root / record.relative_path;
  std::error_code error;
  if (!std::filesystem::is_regular_file(absolute, error) || error) {
    return ChecksumValidationIssue{relative, "MISSING"};
  }

  const auto actual_size = static_cast<std::uint64_t>(std::filesystem::file_size(absolute, error));
  if (error) {
    return ChecksumValidationIssue{relative, "unreadable"};
  }
  if (record.has_byte_size && actual_size < record.byte_size) {
    return ChecksumValidationIssue{
        relative,
        "TRUNCATED (have " + std::to_string(actual_size) +
            " bytes, expected " + std::to_string(record.byte_size) + ")"};
  }

  const auto use_prefix = record.has_byte_size && actual_size > record.byte_size;
  const auto digest = use_prefix
                          ? apitrace::trace::content_hash_file_prefix(absolute, record.byte_size)
                          : apitrace::trace::content_hash_file(absolute);
  if (digest != record.digest) {
    return ChecksumValidationIssue{relative, "CORRUPT (hash mismatch)"};
  }
  return std::nullopt;
}

ChecksumValidationResult validate_checksum_records_parallel(
    const std::filesystem::path &bundle_root,
    const std::vector<apitrace::trace::ChecksumRecord> &records,
    std::size_t jobs)
{
  ChecksumValidationResult result;
  result.checked = records.size();
  if (records.empty()) {
    return result;
  }

  const auto thread_count = std::max<std::size_t>(
      1,
      std::min<std::size_t>(jobs == 0 ? 1 : jobs, records.size()));
  std::atomic<std::size_t> next_task{0};
  std::vector<std::vector<ChecksumValidationIssue>> thread_issues(thread_count);
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      for (;;) {
        const auto task_index = next_task.fetch_add(1, std::memory_order_relaxed);
        if (task_index >= records.size()) {
          return;
        }
        if (auto issue = validate_checksum_record_contents(bundle_root, records[task_index])) {
          thread_issues[thread_index].push_back(std::move(*issue));
        }
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  for (auto &issues : thread_issues) {
    result.issues.insert(
        result.issues.end(),
        std::make_move_iterator(issues.begin()),
        std::make_move_iterator(issues.end()));
  }
  return result;
}

} // namespace

int apitrace::tools::run_bundle_check(int argc, char **argv)
{
  CheckOptions options;
  std::filesystem::path bundle;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      print_usage(argc > 0 ? argv[0] : nullptr);
      return 0;
    }
    if (arg == "--verify-hashes") {
      // Kept as a compatibility no-op. Hash comparison is now the only bundle-check mode.
      continue;
    }
    if (arg == "--jobs") {
      if (index + 1 >= argc) {
        print_usage(argc > 0 ? argv[0] : nullptr);
        return 2;
      }
      try {
        const auto jobs = std::stoull(argv[++index]);
        options.jobs = static_cast<std::size_t>(std::max<std::uint64_t>(1, jobs));
      } catch (...) {
        print_usage(argc > 0 ? argv[0] : nullptr);
        return 2;
      }
      continue;
    }
    if (arg.empty() || arg[0] == '-' || !bundle.empty()) {
      print_usage(argc > 0 ? argv[0] : nullptr);
      return 2;
    }
    bundle = std::filesystem::path(arg);
  }

  if (bundle.empty()) {
    print_usage(argc > 0 ? argv[0] : nullptr);
    return 2;
  }

  apitrace::trace::ChecksumIndex checksums;
  std::string error;
  const auto checksums_path = bundle / apitrace::trace::kChecksumsFileName;
  if (!parse_checksums(checksums_path, checksums, error)) {
    std::cerr << "bundle-check failed: " << error << "\n";
    return 1;
  }

  const auto hash_result = validate_checksum_records_parallel(bundle, checksums.files, options.jobs);
  auto bad = hash_result.issues.size();
  for (const auto &issue : hash_result.issues) {
    std::cerr << "bundle-check: " << issue.message << " " << issue.relative << "\n";
  }

  const auto expected_bundle_hash = bundle_hash_from_records(checksums.files);
  if (!checksums.bundle_hash.empty() && checksums.bundle_hash != expected_bundle_hash) {
    std::cerr << "bundle-check: bundle_hash mismatch"
              << " expected " << expected_bundle_hash
              << " got " << checksums.bundle_hash << "\n";
    ++bad;
  }

  if (bad != 0) {
    std::cout << "bundle-check FAIL\n";
    std::cout << "checksum_files=" << checksums.files.size() << "\n";
    std::cout << "checksum_files_checked=" << hash_result.checked << "\n";
    std::cout << "checksum_failures=" << bad << "\n";
    return 1;
  }
  std::cout << "bundle-check PASS\n";
  std::cout << "checksum_files=" << checksums.files.size() << "\n";
  std::cout << "checksum_files_checked=" << hash_result.checked << "\n";
  std::cout << "checksum_failures=" << bad << "\n";
  return 0;
}
