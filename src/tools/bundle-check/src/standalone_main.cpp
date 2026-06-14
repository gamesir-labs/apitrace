#include "apitrace/tools/cli_entries.hpp"

// Thin wrapper: the standalone bundle-check executable. Logic lives in run_bundle_check so the
// unified `apitrace` CLI can dispatch to the same entry point.
int main(int argc, char **argv)
{
  return apitrace::tools::run_bundle_check(argc, argv);
}
