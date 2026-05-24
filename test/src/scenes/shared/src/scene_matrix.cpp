#include "scenes/shared/scene_matrix.hpp"

#include <algorithm>

namespace demo::scenes::shared {

namespace {

const std::array<SceneMatrixEntry, 7> kSceneMatrix = {{
    {
        "smoke_triangle",
        SceneTier::core,
        "swapchain / backbuffer / RTV / viewport / Draw / Present",
        "clear color overdrawn by a centered tinted triangle; background corners remain unchanged",
    },
    {
        "indexed_instancing",
        SceneTier::core,
        "vertex buffer / index buffer / instance buffer / DrawIndexed / DrawIndexedInstanced",
        "multiple indexed quads land at distinct instance offsets with stable per-instance colors",
    },
    {
        "textured_quad",
        SceneTier::core,
        "Texture2D / ShaderResourceView / SamplerState / UpdateSubresource / shader resource binding",
        "uploaded checkerboard-style texture survives sampling and tint modulation on the final quad",
    },
    {
        "depth_blend_scissor",
        SceneTier::core,
        "depth texture / DSV / depth state / blend state / rasterizer state / scissor",
        "depth-tested overlap, alpha blend, and scissor exclusion are simultaneously visible in the final frame",
    },
    {
        "offscreen_copy_composite",
        SceneTier::core,
        "offscreen render target / CopyResource / SRV-RTV reuse / second-pass composite",
        "offscreen pass is copied and composited back onto the swapchain with preserved color separation",
    },
    {
        "mip_sampling",
        SceneTier::extended,
        "mip chain / mip sampling / mip view and sampling path",
        "different screen regions resolve to distinct mip levels with predictable color transitions",
    },
    {
        "msaa_resolve",
        SceneTier::extended,
        "MSAA render target / resolve / MSAA-to-single-sample output path",
        "resolved edges stay anti-aliased while coverage and background pixels remain stable after resolve",
    },
}};

} // namespace

const std::array<SceneMatrixEntry, 7> &scene_matrix()
{
    return kSceneMatrix;
}

const SceneMatrixEntry *find_scene_matrix_entry(std::string_view name)
{
    const auto it = std::find_if(
        kSceneMatrix.begin(),
        kSceneMatrix.end(),
        [&](const SceneMatrixEntry &entry) {
            return name == entry.name;
        }
    );
    return it == kSceneMatrix.end() ? nullptr : &*it;
}

} // namespace demo::scenes::shared
