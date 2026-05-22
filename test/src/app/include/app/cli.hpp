#pragma once

#include <string>

namespace demo::app {

enum class DxMode {
    dx11,
    dx12,
};

struct CliOptions {
    DxMode dx_mode = DxMode::dx11;
    std::string scene = "smoke_triangle";
    bool list_scenes = false;
};

CliOptions parse_cli(int argc, char **argv);
const char *to_string(DxMode mode) noexcept;

} // namespace demo::app
