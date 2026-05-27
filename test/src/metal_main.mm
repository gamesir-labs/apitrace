#include "runtime/metal/runtime.hpp"
#include "scenes/metal/scenes.hpp"

#include <cstdio>
#include <vector>

int main()
{
    const auto &scenes = demo::scenes::metal::registered_scenes();
    demo::runtime::metal::MetalRuntime runtime =
        demo::runtime::metal::MetalRuntime::create(256, 256);

    std::vector<const demo::scenes::metal::SceneDefinition *> selected;
    selected.reserve(scenes.size());
    for (const auto &scene : scenes) {
        selected.push_back(&scene);
    }

    unsigned int passed = 0;
    unsigned int failed = 0;
    for (const auto *scene : selected) {
        std::printf("scene start: %s\n", scene->name);
        const demo::runtime::metal::ValidationResult result = scene->run(runtime);
        if (result.passed) {
            std::printf("scene pass: %s\n", scene->name);
            ++passed;
        } else {
            std::printf("scene fail: %s reason=%s\n", scene->name, result.reason.c_str());
            ++failed;
        }
    }

    std::printf("summary: passed=%u failed=%u skipped=0\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
