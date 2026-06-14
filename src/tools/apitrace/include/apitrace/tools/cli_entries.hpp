#pragma once

// Reusable entry points for the apitrace CLI tools. Each tool's main.cpp defines its logic as a
// run_*() function so it can be invoked both as a standalone executable (via a thin wrapper main)
// and dispatched from the unified `apitrace` CLI (apitrace --finalize / --bundle-check / --retrace).

namespace apitrace::tools {

// argc/argv are the tool-local argument vectors (argv[0] is the tool name, argv[1..] its options).
int run_bundle_finalize(int argc, char **argv);
int run_bundle_check(int argc, char **argv);
int run_retrace(int argc, char **argv);

} // namespace apitrace::tools
