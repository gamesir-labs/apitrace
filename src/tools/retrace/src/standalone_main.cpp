#include "apitrace/tools/cli_entries.hpp"

// Thin wrapper: the standalone retrace executable. Logic lives in run_retrace so the unified
// `apitrace` CLI can dispatch to the same entry point.
int main(int argc, char **argv)
{
  return apitrace::tools::run_retrace(argc, argv);
}
