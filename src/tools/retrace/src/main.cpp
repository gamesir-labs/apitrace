#include "apitrace/api.hpp"

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
      " [--validate-only] [--metal] [--metal-backend <name>] <trace-path>\n";
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
      "metal_presents_seen: " + std::to_string(statistics.metal_presents_seen) + "\n";
}

} // namespace

int main(int argc, char **argv)
{
  std::string trace_path;
  apitrace::replay::ReplayOptions options;
#if defined(APITRACE_HAS_D3D_NATIVE) && !defined(_WIN32)
  options.backend = apitrace::replay::BackendKind::NativeD3D12;
#endif

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--metal") {
      options.enable_metal_retrace = true;
      options.backend = apitrace::replay::BackendKind::MetalTranslation;
      continue;
    }
    if (arg == "--validate-only") {
      options.validate_only = true;
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

  apitrace::replay::ReplaySession session(options);
  if (!session.run()) {
    const auto &statistics = session.statistics();
    if (!statistics.backend_name.empty() || statistics.calls_replayed != 0 ||
        statistics.frames_seen != 0 || statistics.presents_seen != 0) {
      write_stdout(format_statistics(statistics));
    }
    write_stderr("retrace failed: " + session.last_error() + "\n");
    return 1;
  }

  const auto &statistics = session.statistics();
  write_stdout(format_statistics(statistics));
  return 0;
}
