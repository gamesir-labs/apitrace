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
TRACE_BUNDLE="$TEST_PREFIX/dx12-scene-all.apitrace"
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
done
cp "$ROOT_D3D12_PROXY_DLL" "$DEMO_BIN_DIR/d3d12.dll"
cp "$RETRACE_EXE" "$DEMO_BIN_DIR/retrace.exe"
cp "$DXMT_D3D12CORE_DLL" "$DEMO_BIN_DIR/d3d12core.dll"
cp "$DXMT_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$DEMO_BIN_DIR/winemetal.dll"
cp "$D3D_COMPILER_DLL" "$DEMO_BIN_DIR/d3dcompiler_47.dll"

DXC_BUNDLE_DIR="$(resolve_dxc_bundle_dir || true)"
if [ -n "$DXC_BUNDLE_DIR" ] && [ -f "$DXC_BUNDLE_DIR/dxcompiler.dll" ] && [ -f "$DXC_BUNDLE_DIR/dxil.dll" ]; then
    cp "$DXC_BUNDLE_DIR/dxcompiler.dll" "$DEMO_BIN_DIR/dxcompiler.dll"
    cp "$DXC_BUNDLE_DIR/dxil.dll" "$DEMO_BIN_DIR/dxil.dll"
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

retrace_output="$("$WINE_BIN" "$DEMO_BIN_DIR/retrace.exe" "$TRACE_BUNDLE" | tr -d '\r')"
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
