#include "scenes/dx12/scenes.hpp"

#include <algorithm>
#include <string>

namespace demo::scenes::dx12 {

namespace {

using demo::runtime::dx12::Dx12Runtime;
using demo::runtime::dx12::ValidationResult;

ValidationResult unimplemented_scene(const char *scene_name)
{
    return {false, std::string(scene_name) + ": dx12 scene is reserved but not implemented yet"};
}

ValidationResult run_smoke_triangle(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("smoke_triangle");
}

ValidationResult run_indexed_instancing(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("indexed_instancing");
}

ValidationResult run_textured_quad(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("textured_quad");
}

ValidationResult run_depth_blend_scissor(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("depth_blend_scissor");
}

ValidationResult run_offscreen_copy_composite(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("offscreen_copy_composite");
}

ValidationResult run_mip_sampling(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("mip_sampling");
}

ValidationResult run_msaa_resolve(Dx12Runtime &, unsigned int)
{
    return unimplemented_scene("msaa_resolve");
}

const std::vector<SceneDefinition> kScenes = {
    {"smoke_triangle", SceneTier::core, false, &run_smoke_triangle},
    {"indexed_instancing", SceneTier::core, false, &run_indexed_instancing},
    {"textured_quad", SceneTier::core, false, &run_textured_quad},
    {"depth_blend_scissor", SceneTier::core, false, &run_depth_blend_scissor},
    {"offscreen_copy_composite", SceneTier::core, false, &run_offscreen_copy_composite},
    {"mip_sampling", SceneTier::extended, false, &run_mip_sampling},
    {"msaa_resolve", SceneTier::extended, false, &run_msaa_resolve},
};

} // namespace

const std::vector<SceneDefinition> &registered_scenes()
{
    return kScenes;
}

const SceneDefinition *find_scene(std::string_view name)
{
    const auto it = std::find_if(
        kScenes.begin(),
        kScenes.end(),
        [&](const SceneDefinition &scene) {
            return name == scene.name;
        }
    );
    return it == kScenes.end() ? nullptr : &*it;
}

} // namespace demo::scenes::dx12
