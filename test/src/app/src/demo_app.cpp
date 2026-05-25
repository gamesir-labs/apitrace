#include "app/demo_app.hpp"

#include "app/cli.hpp"
#include "runtime/dx11/runtime.hpp"
#include "runtime/dx12/runtime.hpp"
#include "scenes/dx11/scenes.hpp"
#include "scenes/dx12/scenes.hpp"

#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace demo::app {

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;
constexpr const char *kWindowClassName = "apitrace-test-demo";
constexpr const char *kWindowTitle = "apitrace test demo";

using EmitSceneMarkerFn = void (WINAPI *)(const char *, const char *, const char *);

unsigned int resolve_frame_budget()
{
    const unsigned int budget = demo::read_env_u32("APITRACE_TRIANGLE_MAX_FRAMES", 0);
    return budget == 0 ? 1U : budget;
}

std::vector<const demo::scenes::dx11::SceneDefinition *> dx11_scene_order()
{
    std::vector<const demo::scenes::dx11::SceneDefinition *> ordered;
    for (const demo::scenes::dx11::SceneDefinition &scene : demo::scenes::dx11::registered_scenes()) {
        if (scene.implemented) {
            ordered.push_back(&scene);
        }
    }
    return ordered;
}

std::vector<const demo::scenes::dx12::SceneDefinition *> dx12_scene_order()
{
    std::vector<const demo::scenes::dx12::SceneDefinition *> ordered;
    for (const demo::scenes::dx12::SceneDefinition &scene : demo::scenes::dx12::registered_scenes()) {
        if (scene.name == std::string_view("dxr_smoke")) {
            continue;
        }
        ordered.push_back(&scene);
    }
    return ordered;
}

const char *dx_mode_name(DxMode mode)
{
    return mode == DxMode::dx12 ? "dx12" : "dx11";
}

EmitSceneMarkerFn resolve_scene_marker(DxMode mode)
{
    static EmitSceneMarkerFn d3d11_marker = []() -> EmitSceneMarkerFn {
        HMODULE module = GetModuleHandleA("d3d11.dll");
        if (!module) {
            return nullptr;
        }
        return reinterpret_cast<EmitSceneMarkerFn>(
            GetProcAddress(module, "apitrace_d3d11_emit_scene_marker")
        );
    }();

    static EmitSceneMarkerFn d3d12_marker = []() -> EmitSceneMarkerFn {
        HMODULE module = GetModuleHandleA("d3d12.dll");
        if (!module) {
            return nullptr;
        }
        return reinterpret_cast<EmitSceneMarkerFn>(
            GetProcAddress(module, "apitrace_d3d12_emit_scene_marker")
        );
    }();

    return mode == DxMode::dx12 ? d3d12_marker : d3d11_marker;
}

void emit_scene_marker(DxMode mode, const char *scene_name, const char *phase)
{
    EmitSceneMarkerFn emit_marker = resolve_scene_marker(mode);
    if (emit_marker && scene_name && phase) {
        emit_marker(scene_name, dx_mode_name(mode), phase);
    }
}

int list_scenes(DxMode mode)
{
    if (mode == DxMode::dx12) {
        for (const demo::scenes::dx12::SceneDefinition &scene : demo::scenes::dx12::registered_scenes()) {
            std::printf("%s\n", scene.name);
        }
        return 0;
    }

    for (const demo::scenes::dx11::SceneDefinition &scene : demo::scenes::dx11::registered_scenes()) {
        if (scene.implemented) {
            std::printf("%s\n", scene.name);
        }
    }
    return 0;
}

int run_dx11(const CliOptions &options)
{
    std::printf("dx mode: dx11\n");

    auto runtime = demo::runtime::dx11::Dx11Runtime::create(
        kWindowWidth,
        kWindowHeight,
        kWindowClassName,
        kWindowTitle
    );
    const unsigned int frame_budget = resolve_frame_budget();

    std::vector<const demo::scenes::dx11::SceneDefinition *> selected_scenes;
    if (options.scene == "all") {
        selected_scenes = dx11_scene_order();
    } else {
        const demo::scenes::dx11::SceneDefinition *scene = demo::scenes::dx11::find_scene(options.scene);
        if (!scene) {
            std::printf("scene fail: %s reason=unknown scene\n", options.scene.c_str());
            std::printf("summary: passed=0 failed=1 skipped=0\n");
            return 1;
        }
        if (!scene->implemented) {
            std::printf("scene fail: %s reason=scene is reserved but not implemented\n", options.scene.c_str());
            std::printf("summary: passed=0 failed=1 skipped=0\n");
            return 1;
        }
        selected_scenes.push_back(scene);
    }

    unsigned int passed = 0;
    unsigned int failed = 0;
    unsigned int skipped = 0;

    for (const demo::scenes::dx11::SceneDefinition *scene : selected_scenes) {
        std::printf("scene start: %s\n", scene->name);
        emit_scene_marker(DxMode::dx11, scene->name, "start");
        const demo::runtime::dx11::ValidationResult result = scene->run(runtime, frame_budget);
        emit_scene_marker(DxMode::dx11, scene->name, "end");
        if (result.passed) {
            std::printf("scene pass: %s\n", scene->name);
            ++passed;
        } else if (result.skipped) {
            std::printf("scene skip: %s reason=%s\n", scene->name, result.reason.c_str());
            ++skipped;
        } else {
            std::printf("scene fail: %s reason=%s\n", scene->name, result.reason.c_str());
            ++failed;
        }
        runtime.clear_state();
    }

    std::printf("summary: passed=%u failed=%u skipped=%u\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

int run_dx12(const CliOptions &options)
{
    std::printf("dx mode: dx12\n");

    std::vector<const demo::scenes::dx12::SceneDefinition *> selected_scenes;
    if (options.scene == "all") {
        selected_scenes = dx12_scene_order();
    } else {
        const demo::scenes::dx12::SceneDefinition *scene = demo::scenes::dx12::find_scene(options.scene);
        if (!scene) {
            std::printf("scene fail: %s reason=unknown scene\n", options.scene.c_str());
            std::printf("summary: passed=0 failed=1 skipped=0\n");
            return 1;
        }
        selected_scenes.push_back(scene);
    }

    auto runtime = demo::runtime::dx12::Dx12Runtime::create(
        kWindowWidth,
        kWindowHeight,
        kWindowClassName,
        kWindowTitle
    );
    const unsigned int frame_budget = resolve_frame_budget();

    unsigned int passed = 0;
    unsigned int failed = 0;
    unsigned int skipped = 0;

    for (const demo::scenes::dx12::SceneDefinition *scene : selected_scenes) {
        std::printf("scene start: %s\n", scene->name);
        emit_scene_marker(DxMode::dx12, scene->name, "start");
        const demo::runtime::dx12::ValidationResult result = scene->run(runtime, frame_budget);
        emit_scene_marker(DxMode::dx12, scene->name, "end");
        if (result.passed) {
            std::printf("scene pass: %s\n", scene->name);
            ++passed;
        } else if (result.skipped) {
            std::printf("scene skip: %s reason=%s\n", scene->name, result.reason.c_str());
            ++skipped;
        } else {
            std::printf("scene fail: %s reason=%s\n", scene->name, result.reason.c_str());
            ++failed;
        }
        runtime.clear_state();
    }

    std::printf("summary: passed=%u failed=%u skipped=%u\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

} // namespace

int run_demo_app(int argc, char **argv)
{
    const CliOptions options = parse_cli(argc, argv);

    if (options.list_scenes) {
        return list_scenes(options.dx_mode);
    }

    switch (options.dx_mode) {
    case DxMode::dx11:
        return run_dx11(options);
    case DxMode::dx12:
        return run_dx12(options);
    default:
        std::fprintf(stderr, "unsupported dx mode\n");
        return 1;
    }
}

} // namespace demo::app
