#include "app/demo_app.hpp"
#include "runtime/dx11/runtime.hpp"
#include "runtime/dx12/runtime.hpp"
#include "scenes/dx11/scenes.hpp"
#include "scenes/dx12/scenes.hpp"

#include <windows.h>

#include <cstdio>
#include <string_view>
#include <vector>

namespace demo::app {

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;
constexpr const char *kWindowClassName = "apitrace-test-demo";
constexpr const char *kWindowTitle = "apitrace test demo";

using EmitSceneMarkerFn = void (WINAPI *)(const char *, const char *, const char *);
constexpr unsigned int kFrameBudget = 1;

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

enum class DemoMode {
    d3d11,
    d3d12,
};

const char *dx_mode_name(DemoMode mode)
{
    return mode == DemoMode::d3d12 ? "dx12" : "dx11";
}

EmitSceneMarkerFn resolve_scene_marker(DemoMode mode)
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

    return mode == DemoMode::d3d12 ? d3d12_marker : d3d11_marker;
}

void emit_scene_marker(DemoMode mode, const char *scene_name, const char *phase)
{
    EmitSceneMarkerFn emit_marker = resolve_scene_marker(mode);
    if (emit_marker && scene_name && phase) {
        emit_marker(scene_name, dx_mode_name(mode), phase);
    }
}

int run_dx11()
{
    std::printf("dx mode: dx11\n");

    auto runtime = demo::runtime::dx11::Dx11Runtime::create(
        kWindowWidth,
        kWindowHeight,
        kWindowClassName,
        kWindowTitle
    );
    const std::vector<const demo::scenes::dx11::SceneDefinition *> selected_scenes = dx11_scene_order();

    unsigned int passed = 0;
    unsigned int failed = 0;
    unsigned int skipped = 0;

    for (const demo::scenes::dx11::SceneDefinition *scene : selected_scenes) {
        std::printf("scene start: %s\n", scene->name);
        emit_scene_marker(DemoMode::d3d11, scene->name, "start");
        const demo::runtime::dx11::ValidationResult result = scene->run(runtime, kFrameBudget);
        emit_scene_marker(DemoMode::d3d11, scene->name, "end");
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

int run_dx12()
{
    std::printf("dx mode: dx12\n");

    auto runtime = demo::runtime::dx12::Dx12Runtime::create(
        kWindowWidth,
        kWindowHeight,
        kWindowClassName,
        kWindowTitle
    );
    const std::vector<const demo::scenes::dx12::SceneDefinition *> selected_scenes = dx12_scene_order();

    unsigned int passed = 0;
    unsigned int failed = 0;
    unsigned int skipped = 0;

    for (const demo::scenes::dx12::SceneDefinition *scene : selected_scenes) {
        std::printf("scene start: %s\n", scene->name);
        emit_scene_marker(DemoMode::d3d12, scene->name, "start");
        const demo::runtime::dx12::ValidationResult result = scene->run(runtime, kFrameBudget);
        emit_scene_marker(DemoMode::d3d12, scene->name, "end");
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

int run_d3d11_demo()
{
    return run_dx11();
}

int run_d3d12_demo()
{
    return run_dx12();
}

} // namespace demo::app
