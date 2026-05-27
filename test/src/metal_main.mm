#include "runtime/metal/runtime.hpp"
#include "scenes/metal/scenes.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CliOptions {
    std::string scene = "metal_smoke_triangle";
    bool list_scenes = false;
};

[[noreturn]] void fail_usage(const char *message)
{
    if (message && *message) {
        std::fprintf(stderr, "%s\n", message);
    }
    std::fprintf(stderr, "usage: apitrace_metal_demo [--scene <name|all>] [--list-scenes] [--metal]\n");
    std::exit(EXIT_FAILURE);
}

CliOptions parse_cli(int argc, char **argv)
{
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--scene") {
            if (index + 1 >= argc) {
                fail_usage("missing value for --scene");
            }
            options.scene = argv[++index];
            continue;
        }
        if (arg == "--list-scenes") {
            options.list_scenes = true;
            continue;
        }
        if (arg == "--metal") {
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            fail_usage(nullptr);
        }
        fail_usage("unknown argument");
    }
    return options;
}

} // namespace

int main(int argc, char **argv)
{
    const CliOptions options = parse_cli(argc, argv);
    const auto &scenes = demo::scenes::metal::registered_scenes();

    if (options.list_scenes) {
        for (const auto &scene : scenes) {
            std::printf("%s\n", scene.name);
        }
        return 0;
    }

    demo::runtime::metal::MetalRuntime runtime =
        demo::runtime::metal::MetalRuntime::create(256, 256);

    std::vector<const demo::scenes::metal::SceneDefinition *> selected;
    if (options.scene == "all") {
        for (const auto &scene : scenes) {
            selected.push_back(&scene);
        }
    } else {
        const auto *scene = demo::scenes::metal::find_scene(options.scene);
        if (scene == nullptr) {
            std::printf("scene fail: %s reason=unknown scene\n", options.scene.c_str());
            std::printf("failed=1\n");
            return 1;
        }
        selected.push_back(scene);
    }

    unsigned int failed = 0;
    for (const auto *scene : selected) {
        std::printf("scene start: %s\n", scene->name);
        const demo::runtime::metal::ValidationResult result = scene->run(runtime);
        if (result.passed) {
            std::printf("scene pass: %s\n", scene->name);
        } else {
            std::printf("scene fail: %s reason=%s\n", scene->name, result.reason.c_str());
            ++failed;
        }
    }

    std::printf("failed=%u\n", failed);
    return failed == 0 ? 0 : 1;
}
