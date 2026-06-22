#include "apitrace/tools/cli_entries.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>

// Thin wrapper: the standalone retrace executable. Logic lives in run_retrace so the unified
// `apitrace` CLI can dispatch to the same entry point.
int main(int argc, char **argv)
{
  // DIAG: surface otherwise-uncaught C++ exceptions (rootcbv64k bundle load crash).
  auto write_fatal = [](const char *msg) {
    std::fprintf(stderr, "[retrace-fatal] %s\n", msg);
    std::fprintf(stdout, "[retrace-fatal] %s\n", msg);
    std::fflush(stderr); std::fflush(stdout);
    if (const char *p = std::getenv("DXMT_RETRACE_FATAL_LOG")) {
      if (FILE *f = std::fopen(p, "w")) { std::fprintf(f, "%s\n", msg); std::fclose(f); }
    }
  };
  try {
    return apitrace::tools::run_retrace(argc, argv);
  } catch (const std::exception &e) {
    write_fatal(e.what());
    return 99;
  } catch (...) {
    write_fatal("uncaught non-std exception");
    return 98;
  }
}
