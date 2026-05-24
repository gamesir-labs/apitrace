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
DXMT_D3D12_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/d3d12/d3d12.dll"
DXMT_D3D12CORE_DLL="$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-windows/d3d12core.dll"
DXMT_DXGI_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/dxgi/dxgi.dll"
DXMT_WINEMETAL_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/winemetal.dll"
DXMT_UNIX_DIR="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/unix"
DXMT_PACKAGE_ROOT="$HOME/Library/Application Support/com.gamemac.test/wine-engine/downloads/dxmt-v0.80"
DXMT_WINE_ROOT=""

EXPECTED_SCENES="smoke_triangle indexed_instancing textured_quad depth_blend_scissor offscreen_copy_composite mip_sampling msaa_resolve"
EXPECTED_SCENES="$EXPECTED_SCENES barrier_state_transitions descriptor_root_signature_rebind indirect_draw compute_uav_writeback resource_lifecycle dxr_smoke mesh_shader_smoke"

resolve_libwinpthread() {
    candidate="$(x86_64-w64-mingw32-g++ -print-file-name=libwinpthread-1.dll)"
    if [ "$candidate" != "libwinpthread-1.dll" ] && [ -f "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    compiler_bin_dir="$(dirname -- "$(command -v x86_64-w64-mingw32-g++)")"
    search_root="$(CDPATH= cd -- "$compiler_bin_dir/.." && pwd)"
    candidate="$(find "$search_root" -path '*/x86_64-w64-mingw32/*/libwinpthread-1.dll' | head -n 1)"
    if [ -z "$candidate" ] || [ ! -f "$candidate" ]; then
        printf '%s\n' ""
        return 1
    fi
    printf '%s\n' "$candidate"
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

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT_BUILD_DIR"

cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$TEST_BUILD_DIR"
cmake --install "$TEST_BUILD_DIR" --prefix "$TEST_PREFIX"

if [ -d "$DXMT_PACKAGE_ROOT/wine" ]; then
    DXMT_WINE_ROOT="$DXMT_PACKAGE_ROOT/wine"
    DXMT_D3D12_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/d3d12.dll"
    DXMT_D3D12CORE_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/d3d12core.dll"
    DXMT_DXGI_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/dxgi.dll"
    DXMT_WINEMETAL_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/winemetal.dll"
    DXMT_UNIX_DIR="$DXMT_PACKAGE_ROOT/wine/x86_64-unix"
fi

for required_path in "$DXMT_D3D12_DLL" "$DXMT_D3D12CORE_DLL" "$DXMT_DXGI_DLL" "$DXMT_WINEMETAL_DLL"; do
    if [ ! -f "$required_path" ]; then
        echo "missing DXMT runtime: $required_path" >&2
        exit 1
    fi
done

runtime_path="$(resolve_libwinpthread || true)"
if [ -z "$runtime_path" ] || [ ! -f "$runtime_path" ]; then
    echo "missing MinGW runtime DLL: libwinpthread-1.dll" >&2
    exit 1
fi
cp "$runtime_path" "$DEMO_BIN_DIR/libwinpthread-1.dll"
cp "$DXMT_D3D12_DLL" "$DEMO_BIN_DIR/d3d12.dll"
cp "$DXMT_D3D12CORE_DLL" "$DEMO_BIN_DIR/d3d12core.dll"
cp "$DXMT_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$DEMO_BIN_DIR/winemetal.dll"

DXC_BUNDLE_DIR="$(resolve_dxc_bundle_dir || true)"
if [ -n "$DXC_BUNDLE_DIR" ] && [ -f "$DXC_BUNDLE_DIR/dxcompiler.dll" ] && [ -f "$DXC_BUNDLE_DIR/dxil.dll" ]; then
    cp "$DXC_BUNDLE_DIR/dxcompiler.dll" "$DEMO_BIN_DIR/dxcompiler.dll"
    cp "$DXC_BUNDLE_DIR/dxil.dll" "$DEMO_BIN_DIR/dxil.dll"
fi

export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d12,d3d12core,dxgi,winemetal=n,b"
export WINEDEBUG="-all"
export WINEARCH="win64"
export WINEPREFIX="$WINE_PREFIX"
export DXMT_EXPERIMENT_DX12_SUPPORT="${DXMT_EXPERIMENT_DX12_SUPPORT:-1}"
if [ -n "$DXMT_WINE_ROOT" ]; then
    export WINEDLLPATH="$DXMT_WINE_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
else
    export WINEDLLPATH="$ROOT_DIR/../wine-enviroment/lib/wine"
fi
export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"

if [ ! -f "$WINE_PREFIX/system.reg" ]; then
    "$WINE_BIN" wineboot -u >/dev/null 2>&1 || true
fi

list_output="$("$WINE_BIN" "$DEMO_EXE" --list-scenes --dx dx12 | tr -d '\r')"
if [ "$list_output" != "$(printf '%s\n' $EXPECTED_SCENES)" ]; then
    echo "unexpected dx12 scene list:" >&2
    printf '%s\n' "$list_output" >&2
    exit 1
fi

run_output="$("$WINE_BIN" "$DEMO_EXE" --dx dx12 --scene all | tr -d '\r')"
for scene in $EXPECTED_SCENES; do
    if ! printf '%s\n' "$run_output" | grep -F "scene pass: $scene" >/dev/null && \
       ! printf '%s\n' "$run_output" | grep -F "scene skip: $scene" >/dev/null; then
        echo "missing dx12 result for scene: $scene" >&2
        printf '%s\n' "$run_output" >&2
        exit 1
    fi
done

if ! printf '%s\n' "$run_output" | grep -F "failed=0" >/dev/null; then
    echo "unexpected dx12 summary" >&2
    printf '%s\n' "$run_output" >&2
    exit 1
fi
