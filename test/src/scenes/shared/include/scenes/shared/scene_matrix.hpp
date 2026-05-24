#pragma once

#include <array>
#include <string_view>

namespace demo::scenes::shared {

enum class SceneTier {
    core,
    extended,
};

struct SceneMatrixEntry {
    const char *name;
    SceneTier tier;
    const char *api_assertion;
    const char *visual_assertion;
};

const std::array<SceneMatrixEntry, 14> &scene_matrix();
const SceneMatrixEntry *find_scene_matrix_entry(std::string_view name);

} // namespace demo::scenes::shared
