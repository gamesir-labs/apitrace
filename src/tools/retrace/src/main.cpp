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
  const std::string message = "usage: " + std::string(argv0) + " <trace-path>\n";
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
      "frames_seen: " + std::to_string(statistics.frames_seen) + "\n" +
      "presents_seen: " + std::to_string(statistics.presents_seen) + "\n";
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    print_usage(argc > 0 ? argv[0] : "retrace");
    return 1;
  }

  apitrace::replay::ReplayOptions options;
  options.bundle_root = argv[1];

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
