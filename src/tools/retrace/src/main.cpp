#include "apitrace/api.hpp"
#include "apitrace/tools/cli_entries.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void print_usage(std::string_view argv0)
{
  const std::string message =
      "usage: " + std::string(argv0) +
      " [--validate-only] [--finalize-first] [--metal] [--metal-backend <name>] <trace-path>\n";
#ifdef _WIN32
  DWORD written = 0;
  const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
  if (handle != INVALID_HANDLE_VALUE && handle != nullptr &&
      WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) {
    return;
  }
#endif
  std::cerr << message;
}

void write_stdout(const std::string &message)
{
#ifdef _WIN32
  DWORD written = 0;
  const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle != INVALID_HANDLE_VALUE && handle != nullptr &&
      WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) {
    return;
  }
#endif
  std::cout << message;
}

// Mirror a message to APITRACE_RETRACE_LOG_FILE if set. Under Wine the console subsystem swallows
// WriteFile(STD_OUTPUT/STD_ERROR), so stats and "retrace failed: ..." are otherwise invisible; a
// file sink makes them observable. Best-effort: failure to open the file is ignored.
void write_log_file(const std::string &message)
{
  const char *path = std::getenv("APITRACE_RETRACE_LOG_FILE");
  if (path == nullptr || *path == '\0') {
    return;
  }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (out) {
    out << message;
  }
}

void write_stderr(const std::string &message)
{
#ifdef _WIN32
  DWORD written = 0;
  const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
  if (handle != INVALID_HANDLE_VALUE && handle != nullptr &&
      WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) {
    return;
  }
#endif
  std::cerr << message;
}

std::string format_statistics(const apitrace::replay::ReplayStatistics &statistics)
{
  return "retrace " + std::string(apitrace::version_string()) + "\n" +
      "backend: " + statistics.backend_name + "\n" +
      "calls_replayed: " + std::to_string(statistics.calls_replayed) + "\n" +
      "metal_calls_replayed: " + std::to_string(statistics.metal_calls_replayed) + "\n" +
      "frames_seen: " + std::to_string(statistics.frames_seen) + "\n" +
      "presents_seen: " + std::to_string(statistics.presents_seen) + "\n" +
      "metal_presents_seen: " + std::to_string(statistics.metal_presents_seen) + "\n" +
      "open_ms: " + std::to_string(statistics.open_ms) + "\n" +
      "backend_init_ms: " + std::to_string(statistics.backend_init_ms) + "\n" +
      "event_replay_ms: " + std::to_string(statistics.event_replay_ms) + "\n" +
      "finalize_ms: " + std::to_string(statistics.finalize_ms) + "\n" +
      (statistics.d3d12_event_ordered_counters.empty()
           ? std::string()
           : "d3d12_event_ordered_counters: " +
                 statistics.d3d12_event_ordered_counters + "\n");
}

std::string shell_quote(const std::string &value)
{
#ifdef _WIN32
  std::string quoted = "\"";
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      quoted += '\\';
    }
    quoted += ch;
  }
  quoted += "\"";
  return quoted;
#else
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

bool finalize_bundle(std::string_view argv0, const std::string &trace_path)
{
  auto executable_path = std::filesystem::path("bundle-finalize");
  const auto retrace_path = std::filesystem::path(std::string(argv0));
  if (retrace_path.has_parent_path()) {
    const auto sibling = retrace_path.parent_path() / "bundle-finalize";
    if (std::filesystem::exists(sibling)) {
      executable_path = sibling;
    }
  }
  if (!std::filesystem::exists(executable_path)) {
    executable_path = "bundle-finalize";
  }
  const auto command = shell_quote(executable_path.string()) + " " + shell_quote(trace_path);
  return std::system(command.c_str()) == 0;
}

} // namespace

int apitrace::tools::run_retrace(int argc, char **argv)
{
  std::string trace_path;
  bool finalize_first = false;
  apitrace::replay::ReplayOptions options;
#if defined(APITRACE_HAS_D3D_NATIVE) && !defined(_WIN32)
  options.backend = apitrace::replay::BackendKind::NativeD3D12;
#endif

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "-h" || arg == "--help") {
      print_usage(argc > 0 ? argv[0] : "retrace");
      return 0;
    }
    if (arg == "--metal") {
      options.enable_metal_retrace = true;
      options.backend = apitrace::replay::BackendKind::MetalTranslation;
      continue;
    }
    if (arg == "--validate-only") {
      options.validate_only = true;
      continue;
    }
    if (arg == "--finalize-first") {
      finalize_first = true;
      continue;
    }
    if (arg == "--metal-backend") {
      if (index + 1 >= argc) {
        print_usage(argc > 0 ? argv[0] : "retrace");
        return 1;
      }
      options.metal_backend_name = argv[++index];
      continue;
    }

    if (!trace_path.empty()) {
      print_usage(argc > 0 ? argv[0] : "retrace");
      return 1;
    }
    trace_path = std::string(arg);
  }

  if (trace_path.empty()) {
    print_usage(argc > 0 ? argv[0] : "retrace");
    return 1;
  }

  options.bundle_root = trace_path;
  if (finalize_first && !finalize_bundle(argc > 0 ? argv[0] : "retrace", trace_path)) {
    write_stderr("retrace failed: bundle-finalize failed\n");
    return 1;
  }

  apitrace::replay::ReplaySession session(options);
  if (!session.run()) {
    const auto &statistics = session.statistics();
    if (!statistics.backend_name.empty() || statistics.calls_replayed != 0 ||
        statistics.frames_seen != 0 || statistics.presents_seen != 0) {
      write_stdout(format_statistics(statistics));
      write_log_file(format_statistics(statistics));
    }
    write_stderr("retrace failed: " + session.last_error() + "\n");
    write_log_file("retrace failed: " + session.last_error() + "\n");
    return 1;
  }

  const auto &statistics = session.statistics();
  write_stdout(format_statistics(statistics));
  write_log_file(format_statistics(statistics));
  return 0;
}
