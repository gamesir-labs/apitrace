#include "app/cli.hpp"

#include "demo_common.hpp"

#include <cstdio>
#include <string_view>

namespace demo::app {

namespace {

[[noreturn]] void fail_usage(const char *message)
{
    if (message && *message) {
        std::fprintf(stderr, "%s\n", message);
    }
    std::fprintf(
        stderr,
        "usage: apitrace_test_demo [--dx <dx11|dx12>] [--scene <name|all>] [--list-scenes]\n"
    );
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

DxMode parse_dx_mode(std::string_view value)
{
    if (value == "dx11") {
        return DxMode::dx11;
    }
    if (value == "dx12") {
        return DxMode::dx12;
    }
    fail_usage("unsupported dx mode");
}

} // namespace

CliOptions parse_cli(int argc, char **argv)
{
    CliOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--dx") {
            if (i + 1 >= argc) {
                fail_usage("missing value for --dx");
            }
            options.dx_mode = parse_dx_mode(argv[++i]);
            continue;
        }
        if (arg == "--scene") {
            if (i + 1 >= argc) {
                fail_usage("missing value for --scene");
            }
            options.scene = argv[++i];
            continue;
        }
        if (arg == "--list-scenes") {
            options.list_scenes = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            fail_usage(nullptr);
        }

        fail_usage("unknown argument");
    }

    return options;
}

const char *to_string(DxMode mode) noexcept
{
    switch (mode) {
    case DxMode::dx11:
        return "dx11";
    case DxMode::dx12:
        return "dx12";
    default:
        return "unknown";
    }
}

} // namespace demo::app
