#pragma once

#include "runtime/dx12/runtime.hpp"
#include "scenes/shared/scene_matrix.hpp"

#include <string_view>
#include <vector>

namespace demo::scenes::dx12 {

using SceneTier = demo::scenes::shared::SceneTier;

struct SceneDefinition {
    const char *name;
    SceneTier tier;
    bool implemented;
    demo::runtime::dx12::ValidationResult (*run)(
        demo::runtime::dx12::Dx12Runtime &runtime,
        unsigned int frame_budget
    );
};

const std::vector<SceneDefinition> &registered_scenes();
const SceneDefinition *find_scene(std::string_view name);

} // namespace demo::scenes::dx12
