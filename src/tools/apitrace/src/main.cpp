#include "apitrace/api.hpp"
#include "apitrace/tools/cli_entries.hpp"

#include <cstring>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

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
  fputs(message.c_str(), stderr);
}

void print_usage(const char *argv0)
{
  const std::string name = argv0 ? argv0 : "apitrace";
  write_stderr(
      "usage: " + name + " <command> [options] <trace-bundle>\n"
      "\n"
      "commands:\n"
      "  --finalize        Finalize a captured bundle (dedupe, index, persist replay model).\n"
      "  --bundle-check    Validate a bundle (semantics, closure, --verify-hashes for integrity).\n"
      "  --retrace         Replay a bundle.\n"
      "\n"
      "Run `" + name + " <command> --help` for command-specific options.\n");
}

// Build the sub-tool argv: argv[0] becomes "apitrace <command>", remaining args shift down by one
// so each run_*() sees its own option vector unchanged.
int dispatch(const char *program, const char *command, int argc, char **argv,
             int (*entry)(int, char **))
{
  std::string tool_name = std::string(program ? program : "apitrace") + " " + command;
  std::vector<char *> forwarded;
  forwarded.reserve(static_cast<std::size_t>(argc));
  forwarded.push_back(tool_name.data());
  for (int index = 2; index < argc; ++index) {
    forwarded.push_back(argv[index]);
  }
  return entry(static_cast<int>(forwarded.size()), forwarded.data());
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2) {
    print_usage(argc > 0 ? argv[0] : nullptr);
    return 2;
  }

  const std::string_view command(argv[1]);
  const char *program = argc > 0 ? argv[0] : "apitrace";

  if (command == "-h" || command == "--help") {
    print_usage(program);
    return 0;
  }
  if (command == "--finalize" || command == "finalize") {
    return dispatch(program, "--finalize", argc, argv, apitrace::tools::run_bundle_finalize);
  }
  if (command == "--bundle-check" || command == "bundle-check") {
    return dispatch(program, "--bundle-check", argc, argv, apitrace::tools::run_bundle_check);
  }
  if (command == "--retrace" || command == "retrace") {
    return dispatch(program, "--retrace", argc, argv, apitrace::tools::run_retrace);
  }

  write_stderr("apitrace: unknown command '" + std::string(command) + "'\n");
  print_usage(program);
  return 2;
}
