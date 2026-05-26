#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="${APITRACE_WINE_BIN:-$ROOT_DIR/../wine-enviroment/bin/wine}"
if [ ! -x "$WINE_BIN" ]; then
    WINE_BIN="$(command -v wine || true)"
fi
if [ -z "$WINE_BIN" ] || [ ! -x "$WINE_BIN" ]; then
    echo "missing wine binary" >&2
    exit 1
fi

ROOT_BUILD_DIR="$ROOT_DIR/build/windows-cross"
TEST_BUILD_DIR="$ROOT_DIR/test/build/windows-x86_64-dx12"
TEST_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/dx12"
WINE_PREFIX="$ROOT_DIR/test/artifacts/wineprefix-dx12"
ROOT_TOOLCHAIN="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
DEMO_EXE="$TEST_PREFIX/bin/apitrace_test_demo.exe"
DEMO_BIN_DIR="$TEST_PREFIX/bin"
RETRACE_BIN_DIR="$TEST_PREFIX/retrace"
TRACE_BUNDLE="$TEST_PREFIX/dx12-scene-all.apitrace"
PIXEL_FIXTURE_DIR="$ROOT_DIR/test/fixtures/retrace/d3d12-scene-all-pixel"
PIXEL_FIXTURE_BUNDLE="$PIXEL_FIXTURE_DIR/dx12-scene-all-pixel.apitrace"
PIXEL_FIXTURE_UPDATE_BUNDLE="$TEST_PREFIX/dx12-scene-all-pixel-fixture-update.apitrace"
PIXEL_RETRACE_BUNDLE="$TEST_PREFIX/dx12-scene-all-pixel-retrace.apitrace"
GENERATED_TRACE_BUNDLES="$TRACE_BUNDLE $PIXEL_FIXTURE_UPDATE_BUNDLE $PIXEL_RETRACE_BUNDLE"
ROOT_D3D12_PROXY_DLL="$ROOT_BUILD_DIR/d3d12.dll"
RETRACE_EXE="$ROOT_BUILD_DIR/retrace.exe"
DXMT_REPO_ROOT="${APITRACE_DXMT_REPO_ROOT:-$ROOT_DIR/../dxmt}"
DXMT_BUILD_DIR="${APITRACE_DXMT_BUILD_DIR:-$DXMT_REPO_ROOT/build-gs-native-builtin}"
DXMT_STAGE_DIR="${APITRACE_DXMT_STAGE_DIR:-$ROOT_DIR/test/artifacts/dxmt-runtime-d3d12}"
DXMT_RUNTIME_ROOT=""
DXMT_D3D12_DLL=""
DXMT_D3D12CORE_DLL=""
DXMT_DXGI_DLL=""
DXMT_WINEMETAL_DLL=""
DXMT_UNIX_DIR=""
D3D_COMPILER_DLL="$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-windows/d3dcompiler_47.dll"

EXPECTED_LIST_SCENES="smoke_triangle indexed_instancing textured_quad depth_blend_scissor offscreen_copy_composite mip_sampling msaa_resolve"
EXPECTED_LIST_SCENES="$EXPECTED_LIST_SCENES barrier_state_transitions descriptor_root_signature_rebind indirect_draw compute_uav_writeback resource_lifecycle dxr_smoke mesh_shader_smoke"
EXPECTED_RUN_SCENES="smoke_triangle indexed_instancing textured_quad depth_blend_scissor offscreen_copy_composite mip_sampling msaa_resolve"
EXPECTED_RUN_SCENES="$EXPECTED_RUN_SCENES barrier_state_transitions descriptor_root_signature_rebind indirect_draw compute_uav_writeback resource_lifecycle mesh_shader_smoke"

cleanup_generated_trace_bundles() {
    if [ "${APITRACE_KEEP_GENERATED_TRACES:-0}" = "1" ]; then
        return
    fi
    for bundle in $GENERATED_TRACE_BUNDLES; do
        rm -rf "$bundle"
    done
}

trap cleanup_generated_trace_bundles EXIT

resolve_mingw_runtime() {
    dll_name="$1"
    stdlib_path="$(x86_64-w64-mingw32-g++ -print-file-name=libstdc++-6.dll)"
    if [ "$stdlib_path" = "libstdc++-6.dll" ] || [ ! -f "$stdlib_path" ]; then
        stdlib_path=""
    fi
    if [ -n "$stdlib_path" ]; then
        toolchain_lib_dir="$(dirname "$stdlib_path")"
        toolchain_bin_dir="$(CDPATH= cd -- "$toolchain_lib_dir/../bin" && pwd)"
    else
        toolchain_bin_dir=""
    fi

    case "$dll_name" in
        libgcc_s_seh-1.dll)
            candidate="$(x86_64-w64-mingw32-g++ -print-file-name="$dll_name")"
            if [ "$candidate" = "$dll_name" ]; then
                candidate=""
            fi
            ;;
        libstdc++-6.dll)
            candidate="$stdlib_path"
            ;;
        libwinpthread-1.dll)
            candidate="$toolchain_bin_dir/$dll_name"
            ;;
        *)
            candidate=""
            ;;
    esac

    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    printf '%s\n' ""
    return 1
}

resolve_dxmt_stage_prefix() {
    meson introspect "$DXMT_BUILD_DIR" --buildoptions | python3 -c 'import json, os, sys
data = json.load(sys.stdin)
prefix = ""
for item in data:
    if item.get("name") == "prefix":
        prefix = item.get("value") or ""
        break
if not prefix:
    sys.exit(1)
print(os.path.normpath(sys.argv[1] + prefix))
' "$DXMT_STAGE_DIR"
}

stage_dxmt_runtime() {
    if [ ! -f "$DXMT_BUILD_DIR/build.ninja" ]; then
        echo "missing DXMT build directory: $DXMT_BUILD_DIR" >&2
        exit 1
    fi

    rm -rf "$DXMT_STAGE_DIR"
    meson compile -C "$DXMT_BUILD_DIR"
    DESTDIR="$DXMT_STAGE_DIR" meson install -C "$DXMT_BUILD_DIR" --tags runtime,nvext

    DXMT_RUNTIME_ROOT="$(resolve_dxmt_stage_prefix)"
    DXMT_D3D12_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12.dll"
    DXMT_D3D12CORE_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12core.dll"
    DXMT_DXGI_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/dxgi.dll"
    DXMT_WINEMETAL_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/winemetal.dll"
    DXMT_UNIX_DIR="$DXMT_RUNTIME_ROOT/x86_64-unix"
}

resolve_dxc_bundle_dir() {
    candidate="$(find "$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c" -name dxcompiler.dll -print -quit 2>/dev/null || true)"
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        dirname -- "$candidate"
        return 0
    fi

    candidate="$(find "$HOME/Library/Application Support/CrossOver/Bottles" -name dxcompiler.dll -print -quit 2>/dev/null || true)"
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        dirname -- "$candidate"
        return 0
    fi

    return 1
}

rm -rf "$ROOT_BUILD_DIR" "$TEST_BUILD_DIR" "$TEST_PREFIX"

stage_dxmt_runtime

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT_BUILD_DIR"

cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$TEST_BUILD_DIR"
cmake --install "$TEST_BUILD_DIR" --prefix "$TEST_PREFIX"

for required_path in "$ROOT_D3D12_PROXY_DLL" "$RETRACE_EXE" "$DXMT_D3D12_DLL" "$DXMT_D3D12CORE_DLL" "$DXMT_DXGI_DLL" "$DXMT_WINEMETAL_DLL" "$D3D_COMPILER_DLL"; do
    if [ ! -f "$required_path" ]; then
        echo "missing D3D12 validation dependency: $required_path" >&2
        exit 1
    fi
done

for runtime_dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    runtime_path="$(resolve_mingw_runtime "$runtime_dll" || true)"
    if [ -z "$runtime_path" ] || [ ! -f "$runtime_path" ]; then
        echo "missing MinGW runtime DLL: $runtime_dll" >&2
        exit 1
    fi
    cp "$runtime_path" "$DEMO_BIN_DIR/$runtime_dll"
    mkdir -p "$RETRACE_BIN_DIR"
    cp "$runtime_path" "$RETRACE_BIN_DIR/$runtime_dll"
done
cp "$ROOT_D3D12_PROXY_DLL" "$DEMO_BIN_DIR/d3d12.dll"
cp "$DXMT_D3D12CORE_DLL" "$DEMO_BIN_DIR/d3d12core.dll"
cp "$DXMT_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$DEMO_BIN_DIR/winemetal.dll"
cp "$D3D_COMPILER_DLL" "$DEMO_BIN_DIR/d3dcompiler_47.dll"

mkdir -p "$RETRACE_BIN_DIR"
cp "$RETRACE_EXE" "$RETRACE_BIN_DIR/retrace.exe"
cp "$DXMT_D3D12_DLL" "$RETRACE_BIN_DIR/d3d12.dll"
cp "$DXMT_D3D12CORE_DLL" "$RETRACE_BIN_DIR/d3d12core.dll"
cp "$DXMT_DXGI_DLL" "$RETRACE_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$RETRACE_BIN_DIR/winemetal.dll"
cp "$D3D_COMPILER_DLL" "$RETRACE_BIN_DIR/d3dcompiler_47.dll"

DXC_BUNDLE_DIR="$(resolve_dxc_bundle_dir || true)"
if [ -n "$DXC_BUNDLE_DIR" ] && [ -f "$DXC_BUNDLE_DIR/dxcompiler.dll" ] && [ -f "$DXC_BUNDLE_DIR/dxil.dll" ]; then
    cp "$DXC_BUNDLE_DIR/dxcompiler.dll" "$DEMO_BIN_DIR/dxcompiler.dll"
    cp "$DXC_BUNDLE_DIR/dxil.dll" "$DEMO_BIN_DIR/dxil.dll"
    cp "$DXC_BUNDLE_DIR/dxcompiler.dll" "$RETRACE_BIN_DIR/dxcompiler.dll"
    cp "$DXC_BUNDLE_DIR/dxil.dll" "$RETRACE_BIN_DIR/dxil.dll"
fi

export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d12,d3d12core,dxgi,winemetal=n,b"
export WINEDEBUG="-all"
export WINEARCH="win64"
export WINEPREFIX="$WINE_PREFIX"
export APITRACE_D3D12_BACKEND="dxmt"
export APITRACE_DOWNSTREAM_D3D12="$DXMT_D3D12_DLL"
export DXMT_EXPERIMENT_DX12_SUPPORT="1"
export WINEDLLPATH="$DXMT_RUNTIME_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"
export APITRACE_TRIANGLE_MAX_FRAMES="${APITRACE_D3D12_TRACE_FRAMES:-180}"
export APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=0
PIXEL_COMPARE_FRAMES="${APITRACE_D3D12_PIXEL_COMPARE_FRAMES:-3}"
PIXEL_COMPARE_TOLERANCE="${APITRACE_D3D12_PIXEL_COMPARE_TOLERANCE:-0}"

if [ ! -f "$WINE_PREFIX/system.reg" ]; then
    "$WINE_BIN" wineboot -u >/dev/null 2>&1 || true
fi

list_output="$("$WINE_BIN" "$DEMO_EXE" --list-scenes --dx dx12 | tr -d '\r')"
if [ "$list_output" != "$(printf '%s\n' $EXPECTED_LIST_SCENES)" ]; then
    echo "unexpected dx12 scene list:" >&2
    printf '%s\n' "$list_output" >&2
    exit 1
fi

rm -rf "$TRACE_BUNDLE"
export APITRACE_TRACE_BUNDLE="$TRACE_BUNDLE"
run_output="$("$WINE_BIN" "$DEMO_EXE" --dx dx12 --scene all | tr -d '\r')"
unset APITRACE_TRACE_BUNDLE
for scene in $EXPECTED_RUN_SCENES; do
    if ! printf '%s\n' "$run_output" | grep -F "scene pass: $scene" >/dev/null && \
       ! printf '%s\n' "$run_output" | grep -F "scene skip: $scene" >/dev/null; then
        echo "missing dx12 result for scene: $scene" >&2
        printf '%s\n' "$run_output" >&2
        exit 1
    fi
done

if printf '%s\n' "$run_output" | grep -F "scene start: dxr_smoke" >/dev/null; then
    echo "dxr_smoke should not run as part of dx12 scene all" >&2
    printf '%s\n' "$run_output" >&2
    exit 1
fi

if ! printf '%s\n' "$run_output" | grep -F "failed=0" >/dev/null; then
    echo "unexpected dx12 summary" >&2
    printf '%s\n' "$run_output" >&2
    exit 1
fi

if [ ! -f "$TRACE_BUNDLE/callstream.jsonl" ] || [ ! -f "$TRACE_BUNDLE/checksums.json" ]; then
    echo "missing dx12 trace bundle root files: $TRACE_BUNDLE" >&2
    exit 1
fi

if [ ! -d "$TRACE_BUNDLE/pipelines" ] || ! find "$TRACE_BUNDLE/pipelines" -type f -name '*.pipeline.json' | grep . >/dev/null; then
    echo "missing dx12 pipeline records in bundle: $TRACE_BUNDLE/pipelines" >&2
    exit 1
fi

if ! grep -R -F '"root_signature_object_id":' "$TRACE_BUNDLE/pipelines" >/dev/null || \
   ! grep -R -F '"input_layout":' "$TRACE_BUNDLE/pipelines" >/dev/null || \
   ! grep -R -F '"blend_state":' "$TRACE_BUNDLE/pipelines" >/dev/null || \
   ! grep -R -F '"rasterizer_state":' "$TRACE_BUNDLE/pipelines" >/dev/null || \
   ! grep -R -F '"depth_stencil_state":' "$TRACE_BUNDLE/pipelines" >/dev/null || \
   ! grep -R -F '"sample_desc":' "$TRACE_BUNDLE/pipelines" >/dev/null || \
   ! grep -R -F '"num_render_targets":' "$TRACE_BUNDLE/pipelines" >/dev/null; then
    echo "missing dx12 replay-reconstructable graphics pipeline payloads in bundle" >&2
    exit 1
fi

present_frame_count="$(grep -c '"debug_name":"D3D12PresentFrame"' "$TRACE_BUNDLE/callstream.jsonl" || true)"
if [ "$present_frame_count" -ne 0 ]; then
    echo "dx12 default trace must not capture D3D12PresentFrame debug assets" >&2
    exit 1
fi

present_call_count="$(grep -c '"function":"IDXGISwapChain::Present"' "$TRACE_BUNDLE/callstream.jsonl" || true)"
present_boundary_count="$(grep -c '"boundary":"Present"' "$TRACE_BUNDLE/callstream.jsonl" || true)"
if [ "$present_call_count" -eq 0 ] || [ "$present_call_count" -ne "$present_boundary_count" ]; then
    echo "dx12 present semantic counts diverge: calls=$present_call_count boundaries=$present_boundary_count" >&2
    exit 1
fi

frame_begin_count="$(grep -F '"boundary":"Frame"' "$TRACE_BUNDLE/callstream.jsonl" | grep -c '"label":"FrameBegin"' || true)"
frame_end_count="$(grep -F '"boundary":"Frame"' "$TRACE_BUNDLE/callstream.jsonl" | grep -c '"label":"FrameEnd"' || true)"
if [ "$frame_begin_count" -ne "$present_call_count" ] || [ "$frame_end_count" -ne "$present_call_count" ]; then
    echo "dx12 frame boundary counts diverge: begins=$frame_begin_count ends=$frame_end_count presents=$present_call_count" >&2
    exit 1
fi
if ! grep -F '"boundary":"Frame"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"label":"FrameBegin"' | grep -F '"frame_index":0' >/dev/null || \
   ! grep -F '"boundary":"Frame"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"label":"FrameEnd"' | grep -F '"frame_index":0' >/dev/null; then
    echo "missing dx12 frame boundary frame_index payloads in bundle" >&2
    exit 1
fi

if find "$TRACE_BUNDLE" -path "$TRACE_BUNDLE/textures/*.texture" -type f | grep . >/dev/null; then
    echo "dx12 default trace must not write present-frame texture assets" >&2
    exit 1
fi

buffer_path_count="$(grep -c '"buffer_path":"buffers/' "$TRACE_BUNDLE/callstream.jsonl" || true)"
if [ "$buffer_path_count" -eq 0 ]; then
    echo "missing dx12 mapped buffer asset references in bundle: $TRACE_BUNDLE/callstream.jsonl" >&2
    exit 1
fi

if [ ! -d "$TRACE_BUNDLE/buffers" ] || ! find "$TRACE_BUNDLE/buffers" -type f -name '*.buffer' | grep . >/dev/null; then
    echo "missing dx12 mapped buffer assets in bundle: $TRACE_BUNDLE/buffers" >&2
    exit 1
fi

require_call_record() {
    function_name="$1"
    if ! grep -F "\"function\":\"$function_name\"" "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
        echo "missing dx12 semantic call record: $function_name" >&2
        exit 1
    fi
}

require_call_record "ID3D12Device::CreateCommittedResource"
require_call_record "ID3D12Device::CreateDescriptorHeap"
require_call_record "ID3D12Device::CreateCommandSignature"
require_call_record "ID3D12Device::CreateRootSignature"
require_call_record "ID3D12Device::CreateGraphicsPipelineState"
require_call_record "ID3D12Device::CreateFence"
require_call_record "ID3D12CommandQueue::Signal"
require_call_record "ID3D12CommandQueue::Wait"
require_call_record "ID3D12Fence::SetEventOnCompletion"
require_call_record "ID3D12Fence::Signal"
require_call_record "ID3D12Fence::GetCompletedValue"
require_call_record "ID3D12GraphicsCommandList::SetPipelineState"
require_call_record "ID3D12GraphicsCommandList::SetGraphicsRootSignature"
require_call_record "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant"
require_call_record "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants"
require_call_record "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView"
require_call_record "ID3D12GraphicsCommandList::IASetVertexBuffers"
require_call_record "ID3D12GraphicsCommandList::OMSetRenderTargets"
require_call_record "ID3D12GraphicsCommandList::ResourceBarrier"
require_call_record "ID3D12GraphicsCommandList::ClearRenderTargetView"
require_call_record "ID3D12GraphicsCommandList::CopyResource"
require_call_record "ID3D12GraphicsCommandList::ResolveSubresource"
require_call_record "ID3D12GraphicsCommandList::ExecuteIndirect"
require_call_record "ID3D12GraphicsCommandList::Dispatch"
require_call_record "ID3D12Resource::Map"
require_call_record "ID3D12Resource::Unmap"
require_call_record "IDXGISwapChain::Present"

if ! grep -F '"function":"D3D12CreateDevice"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"minimum_feature_level":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommandQueue"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"priority":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommandQueue"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"node_mask":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommandAllocator"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"type":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommandList"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"node_mask":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommandList"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"type":' >/dev/null || \
   ! grep -F '"function":"ID3D12CommandQueue::ExecuteCommandLists"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"command_list_count":' >/dev/null; then
    echo "missing dx12 queue/list creation and submission semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"descriptor_size":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null || \
   ! grep -F '"cpu_start":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetDescriptorHeaps"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"heap_count":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"base_descriptor":' >/dev/null; then
    echo "missing dx12 descriptor heap relocation metadata in bundle" >&2
    exit 1
fi

if ! grep -F '"gpu_virtual_address":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null || \
   ! grep -F '"buffer_location":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
    echo "missing dx12 GPU virtual address relocation metadata in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"dimension":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"depth_or_array_size":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"sample_count":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"layout":' >/dev/null; then
    echo "missing dx12 resource descriptor semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"optimized_clear_value":{' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"color":[' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateCommittedResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"depth":' >/dev/null; then
    echo "missing dx12 optimized clear value semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12Device::CreateFence"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"initial_value":' >/dev/null || \
   ! grep -F '"function":"ID3D12CommandQueue::Signal"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"fence_value":' >/dev/null || \
   ! grep -F '"function":"ID3D12CommandQueue::Wait"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"fence_value":' >/dev/null || \
   ! grep -F '"function":"ID3D12Fence::Signal"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"fence_value":' >/dev/null || \
   ! grep -F '"function":"ID3D12Fence::SetEventOnCompletion"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"fence_value":' >/dev/null || \
   ! grep -F '"function":"ID3D12Fence::GetCompletedValue"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"completed_value":' >/dev/null; then
    echo "missing dx12 fence synchronization semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"root_parameter_index":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"buffer_location":' >/dev/null; then
    echo "missing dx12 root descriptor binding payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"constant_count":3' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"dst_offset":0' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"values":[' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"constant_count":1' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"dst_offset":3' >/dev/null; then
    echo "missing dx12 root 32-bit constants payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::ClearRenderTargetView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"color":[' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ClearRenderTargetView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"rect_count":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ClearRenderTargetView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"rects":[{"left":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ClearDepthStencilView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"clear_flags":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ClearDepthStencilView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"depth":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ClearDepthStencilView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"stencil":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ClearDepthStencilView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"rects":[{"left":' >/dev/null; then
    echo "missing dx12 clear operation semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12Device::CreateShaderResourceView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"view":{' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateShaderResourceView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"shader_4_component_mapping":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateUnorderedAccessView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"view":{' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateRenderTargetView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"view":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateDepthStencilView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"flags":' >/dev/null; then
    echo "missing dx12 structured descriptor view payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12Device::CreateConstantBufferView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"buffer_location":' >/dev/null || \
   ! grep -F '"function":"ID3D12Device::CreateConstantBufferView"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"size_in_bytes":' >/dev/null; then
    echo "missing dx12 CBV descriptor payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"views":[' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null || \
   ! grep -F '"render_targets":[' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
    echo "missing dx12 full binding array payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::RSSetViewports"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"viewports":[{"x":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::RSSetViewports"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"min_depth":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::RSSetScissorRects"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"rects":[{"left":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::IASetPrimitiveTopology"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"primitive_topology":' >/dev/null; then
    echo "missing dx12 raster state semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"dst":{"resource_object_id":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null || \
   ! grep -F '"src":{"resource_object_id":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
    echo "missing dx12 structured CopyTextureRegion location payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::CopyResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"dst_resource_object_id":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::CopyResource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"src_resource_object_id":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ResolveSubresource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"dst_resource_object_id":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ResolveSubresource"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"dst_subresource":' >/dev/null; then
    echo "missing dx12 copy/resolve resource semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12Device::CreateCommandSignature"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"arguments":[' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ExecuteIndirect"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"arg_buffer_offset":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::ExecuteIndirect"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"count_buffer_offset":' >/dev/null; then
    echo "missing dx12 indirect execution semantic payloads in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::Dispatch"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"thread_group_count_x":' >/dev/null; then
    echo "missing dx12 dispatch semantic payloads in bundle" >&2
    exit 1
fi

if printf '%s\n' "$run_output" | grep -F "scene pass: mesh_shader_smoke" >/dev/null; then
    if ! grep -F '"function":"ID3D12GraphicsCommandList6::DispatchMesh"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"thread_group_count_x":' >/dev/null; then
        echo "missing dx12 mesh dispatch semantic payloads in bundle" >&2
        exit 1
    fi
fi

if ! grep -F '"gpu_start":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
    echo "missing dx12 descriptor heap GPU base metadata in bundle" >&2
    exit 1
fi

if ! grep -F '"sync_interval":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null || \
   ! grep -F '"flags":' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
    echo "missing dx12 present semantic payload in bundle" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::DrawInstanced"' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null && \
   ! grep -F '"function":"ID3D12GraphicsCommandList::DrawIndexedInstanced"' "$TRACE_BUNDLE/callstream.jsonl" >/dev/null; then
    echo "missing dx12 draw semantic call records" >&2
    exit 1
fi

if ! grep -F '"function":"ID3D12GraphicsCommandList::DrawInstanced"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"vertex_count_per_instance":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::DrawInstanced"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"start_vertex_location":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::DrawIndexedInstanced"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"index_count_per_instance":' >/dev/null || \
   ! grep -F '"function":"ID3D12GraphicsCommandList::DrawIndexedInstanced"' "$TRACE_BUNDLE/callstream.jsonl" | grep -F '"base_vertex_location":' >/dev/null; then
    echo "missing dx12 draw parameter semantic payloads in bundle" >&2
    exit 1
fi

set +e
retrace_output="$(
    cd "$RETRACE_BIN_DIR"
    "$WINE_BIN" "$RETRACE_BIN_DIR/retrace.exe" "$TRACE_BUNDLE" 2>&1
)"
retrace_status=$?
set -e
retrace_output="$(printf '%s' "$retrace_output" | tr -d '\r')"
if [ "$retrace_status" -ne 0 ]; then
    echo "dx12 native retrace failed" >&2
    printf '%s\n' "$retrace_output" >&2
    exit 1
fi

if ! printf '%s\n' "$retrace_output" | grep -F "backend: bundle-d3d12" >/dev/null; then
    echo "unexpected dx12 retrace backend" >&2
    printf '%s\n' "$retrace_output" >&2
    exit 1
fi

if ! printf '%s\n' "$retrace_output" | grep -E "calls_replayed: [1-9][0-9]*" >/dev/null; then
    echo "dx12 retrace did not replay calls" >&2
    printf '%s\n' "$retrace_output" >&2
    exit 1
fi

if ! printf '%s\n' "$retrace_output" | grep -E "presents_seen: [1-9][0-9]*" >/dev/null; then
    echo "dx12 retrace did not see present boundaries" >&2
    printf '%s\n' "$retrace_output" >&2
    exit 1
fi

if [ "${APITRACE_D3D12_UPDATE_PIXEL_FIXTURE:-0}" = "1" ]; then
    rm -rf "$PIXEL_FIXTURE_UPDATE_BUNDLE"
    export APITRACE_TRIANGLE_MAX_FRAMES="$PIXEL_COMPARE_FRAMES"
    export APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1
    export APITRACE_TRACE_BUNDLE="$PIXEL_FIXTURE_UPDATE_BUNDLE"
    pixel_fixture_output="$("$WINE_BIN" "$DEMO_EXE" --dx dx12 --scene all | tr -d '\r')"
    unset APITRACE_TRACE_BUNDLE
    export APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=0
    export APITRACE_TRIANGLE_MAX_FRAMES="${APITRACE_D3D12_TRACE_FRAMES:-180}"

    for scene in $EXPECTED_RUN_SCENES; do
        if ! printf '%s\n' "$pixel_fixture_output" | grep -F "scene pass: $scene" >/dev/null && \
           ! printf '%s\n' "$pixel_fixture_output" | grep -F "scene skip: $scene" >/dev/null; then
            echo "missing dx12 pixel fixture result for scene: $scene" >&2
            printf '%s\n' "$pixel_fixture_output" >&2
            exit 1
        fi
    done
    if ! printf '%s\n' "$pixel_fixture_output" | grep -F "failed=0" >/dev/null; then
        echo "unexpected dx12 pixel fixture summary" >&2
        printf '%s\n' "$pixel_fixture_output" >&2
        exit 1
    fi

    pixel_fixture_frame_count="$(grep -c '"debug_name":"D3D12PresentFrame"' "$PIXEL_FIXTURE_UPDATE_BUNDLE/callstream.jsonl" || true)"
    if [ "$pixel_fixture_frame_count" -eq 0 ]; then
        echo "dx12 pixel fixture update did not capture D3D12PresentFrame debug assets" >&2
        exit 1
    fi

    rm -rf "$PIXEL_FIXTURE_BUNDLE"
    mkdir -p "$PIXEL_FIXTURE_DIR"
    cp -R "$PIXEL_FIXTURE_UPDATE_BUNDLE" "$PIXEL_FIXTURE_BUNDLE"
fi

if [ ! -f "$PIXEL_FIXTURE_BUNDLE/callstream.jsonl" ]; then
    echo "missing dx12 pixel compare fixture: $PIXEL_FIXTURE_BUNDLE" >&2
    echo "rerun with APITRACE_D3D12_UPDATE_PIXEL_FIXTURE=1 to refresh it" >&2
    exit 1
fi

pixel_fixture_frame_count="$(grep -c '"debug_name":"D3D12PresentFrame"' "$PIXEL_FIXTURE_BUNDLE/callstream.jsonl" || true)"
if [ "$pixel_fixture_frame_count" -eq 0 ]; then
    echo "dx12 pixel compare fixture has no D3D12PresentFrame debug assets" >&2
    exit 1
fi

cp "$ROOT_D3D12_PROXY_DLL" "$RETRACE_BIN_DIR/d3d12.dll"
rm -rf "$PIXEL_RETRACE_BUNDLE"
export APITRACE_TRACE_BUNDLE="$PIXEL_RETRACE_BUNDLE"
export APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1
set +e
pixel_retrace_output="$(
    cd "$RETRACE_BIN_DIR"
    "$WINE_BIN" "$RETRACE_BIN_DIR/retrace.exe" "$PIXEL_FIXTURE_BUNDLE" 2>&1
)"
pixel_retrace_status=$?
set -e
unset APITRACE_TRACE_BUNDLE
unset APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES
cp "$DXMT_D3D12_DLL" "$RETRACE_BIN_DIR/d3d12.dll"
pixel_retrace_output="$(printf '%s' "$pixel_retrace_output" | tr -d '\r')"
if [ "$pixel_retrace_status" -ne 0 ]; then
    echo "dx12 retrace pixel capture failed" >&2
    printf '%s\n' "$pixel_retrace_output" >&2
    exit 1
fi

if ! printf '%s\n' "$pixel_retrace_output" | grep -F "backend: bundle-d3d12" >/dev/null; then
    echo "unexpected dx12 pixel retrace backend" >&2
    printf '%s\n' "$pixel_retrace_output" >&2
    exit 1
fi

pixel_retrace_frame_count="$(grep -c '"debug_name":"D3D12PresentFrame"' "$PIXEL_RETRACE_BUNDLE/callstream.jsonl" || true)"
if [ "$pixel_retrace_frame_count" -eq 0 ]; then
    echo "dx12 retrace pixel capture did not write D3D12PresentFrame debug assets" >&2
    printf '%s\n' "$pixel_retrace_output" >&2
    exit 1
fi

set +e
pixel_compare_output="$(
    "$ROOT_DIR/scripts/compare-d3d12-present-frames.py" \
        --tolerance "$PIXEL_COMPARE_TOLERANCE" \
        "$PIXEL_FIXTURE_BUNDLE" \
        "$PIXEL_RETRACE_BUNDLE" 2>&1
)"
pixel_compare_status=$?
set -e
pixel_compare_output="$(printf '%s' "$pixel_compare_output" | tr -d '\r')"
printf '%s\n' "$pixel_compare_output"
if [ "$pixel_compare_status" -ne 0 ]; then
    echo "dx12 retrace pixel comparison failed" >&2
    exit 1
fi

if ! printf '%s\n' "$pixel_compare_output" | grep -F "mismatched_frames=0" >/dev/null || \
   ! printf '%s\n' "$pixel_compare_output" | grep -F "mismatched_pixels=0" >/dev/null || \
   ! printf '%s\n' "$pixel_compare_output" | grep -F "max_channel_delta=0" >/dev/null; then
    echo "dx12 retrace pixel comparison reported non-zero differences" >&2
    exit 1
fi
