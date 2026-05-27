#pragma once

#include "runtime/metal/runtime.hpp"

#include <string_view>
#include <vector>

namespace demo::scenes::metal {

struct SceneDefinition {
    const char *name;
    demo::runtime::metal::ValidationResult (*run)(demo::runtime::metal::MetalRuntime &runtime);
};

const std::vector<SceneDefinition> &registered_scenes();
const SceneDefinition *find_scene(std::string_view name);

} // namespace demo::scenes::metal
