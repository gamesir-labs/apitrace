#include "scenes/shared/scene_matrix.hpp"

#include <algorithm>

namespace demo::scenes::shared {

namespace {

const std::array<SceneMatrixEntry, 14> kSceneMatrix = {{
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
    {
        "barrier_state_transitions",
        SceneTier::core,
        "ResourceBarrier / split barrier / render-target to SRV transition coverage",
        "split barrier keeps two cleared offscreen targets visible as stable left/right colors in the final composite",
    },
    {
        "descriptor_root_signature_rebind",
        SceneTier::core,
        "descriptor heap / root signature / CBV and SRV rebinding",
        "the same pipeline rebinds two SRVs and two tint constants to produce distinct left/right quads",
    },
    {
        "indirect_draw",
        SceneTier::core,
        "ExecuteIndirect / draw argument buffer / count buffer",
        "indirect command submission draws two distinct quads with stable left/right colors",
    },
    {
        "compute_uav_writeback",
        SceneTier::core,
        "compute dispatch / UAV writeback / SRV sampling after state transition",
        "compute writes a four-quadrant texture that survives the UAV-to-SRV transition into the final quad",
    },
    {
        "resource_lifecycle",
        SceneTier::core,
        "resource create / release / recreate / descriptor refresh",
        "recreated texture resources keep producing the final solid color after repeated lifetime churn",
    },
    {
        "dxr_smoke",
        SceneTier::extended,
        "raytracing tier probe / AS build-update / SBT / DispatchRays smoke entry",
        "raytracing support is probed explicitly and the scene stays skip-safe when the device or compiler path is unavailable",
    },
    {
        "mesh_shader_smoke",
        SceneTier::extended,
        "mesh shader tier probe / task-mesh PSO switch / DispatchMesh smoke entry",
        "mesh shader support is probed explicitly and the scene stays skip-safe when the device or compiler path is unavailable",
    },
}};

} // namespace

const std::array<SceneMatrixEntry, 14> &scene_matrix()
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
