#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="${APITRACE_WINE_BIN:-$ROOT_DIR/../wine-enviroment/bin/wine}"
WINE_BIN="${WINE_BIN:-$(command -v wine || true)}"
ROOT_BUILD_DIR="${APITRACE_ROOT_BUILD_DIR:-$ROOT_DIR/build/windows-cross}"
TEST_BUILD_DIR="${APITRACE_TEST_BUILD_DIR:-$ROOT_DIR/test/build/windows-x86_64-d3d12}"
TEST_PREFIX="${APITRACE_TEST_PREFIX:-$ROOT_DIR/test/artifacts/d3d12}"
WINE_PREFIX="${APITRACE_WINEPREFIX:-$ROOT_DIR/test/artifacts/wineprefix-d3d12}"
ROOT_TOOLCHAIN="${APITRACE_TOOLCHAIN:-$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake}"
TRACE_BUNDLE="$TEST_PREFIX/trace.apitrace"
RETRACE_BUNDLE="$TEST_PREFIX/retrace.apitrace"
DEMO_BIN_DIR="$TEST_PREFIX/bin"
DEMO_EXE="$DEMO_BIN_DIR/apitrace_test_d3d12.exe"
RETRACE_BIN_DIR="$TEST_PREFIX/retrace-bin"
RETRACE_EXE="$RETRACE_BIN_DIR/retrace.exe"
ROOT_D3D12_PROXY_DLL="$ROOT_BUILD_DIR/d3d12.dll"
ROOT_RETRACE_EXE="$ROOT_BUILD_DIR/retrace.exe"
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
COMPARE_LOG="$TEST_PREFIX/compare.log"
RUN_LOG="$TEST_PREFIX/run.log"
RETRACE_LOG="$TEST_PREFIX/retrace.log"

fail() {
    echo "error: $*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || fail "missing file: $1"
}

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
    [ -f "$DXMT_BUILD_DIR/build.ninja" ] || fail "missing DXMT build directory: $DXMT_BUILD_DIR"
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

ensure_ninja_build_dir() {
    build_dir="$1"
    if [ -f "$build_dir/CMakeCache.txt" ] && ! grep -F 'CMAKE_GENERATOR:INTERNAL=Ninja' "$build_dir/CMakeCache.txt" >/dev/null 2>&1; then
        rm -rf "$build_dir"
    fi
}

step_build() {
    rm -rf "$ROOT_BUILD_DIR" "$TEST_BUILD_DIR" "$TEST_PREFIX"
    mkdir -p "$TEST_PREFIX" "$RETRACE_BIN_DIR"
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

    require_file "$ROOT_D3D12_PROXY_DLL"
    require_file "$ROOT_RETRACE_EXE"
    require_file "$DXMT_D3D12_DLL"
    require_file "$DXMT_D3D12CORE_DLL"
    require_file "$DXMT_DXGI_DLL"
    require_file "$DXMT_WINEMETAL_DLL"
    require_file "$D3D_COMPILER_DLL"

    for runtime_dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
        runtime_path="$(resolve_mingw_runtime "$runtime_dll" || true)"
        [ -n "$runtime_path" ] || fail "missing MinGW runtime DLL: $runtime_dll"
        cp "$runtime_path" "$DEMO_BIN_DIR/$runtime_dll"
        cp "$runtime_path" "$RETRACE_BIN_DIR/$runtime_dll"
    done

    cp "$ROOT_D3D12_PROXY_DLL" "$DEMO_BIN_DIR/d3d12.dll"
    cp "$DXMT_D3D12CORE_DLL" "$DEMO_BIN_DIR/d3d12core.dll"
    cp "$DXMT_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
    cp "$DXMT_WINEMETAL_DLL" "$DEMO_BIN_DIR/winemetal.dll"
    cp "$D3D_COMPILER_DLL" "$DEMO_BIN_DIR/d3dcompiler_47.dll"

    cp "$ROOT_RETRACE_EXE" "$RETRACE_EXE"
    cp "$DXMT_D3D12_DLL" "$RETRACE_BIN_DIR/d3d12.dll"
    cp "$DXMT_D3D12CORE_DLL" "$RETRACE_BIN_DIR/d3d12core.dll"
    cp "$DXMT_DXGI_DLL" "$RETRACE_BIN_DIR/dxgi.dll"
    cp "$DXMT_WINEMETAL_DLL" "$RETRACE_BIN_DIR/winemetal.dll"
    cp "$D3D_COMPILER_DLL" "$RETRACE_BIN_DIR/d3dcompiler_47.dll"
}

prepare_wine_env() {
    export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d12,d3d12core,dxgi,winemetal=n,b"
    export WINEDEBUG="-all"
    export WINEARCH="win64"
    export WINEPREFIX="$WINE_PREFIX"
    export APITRACE_D3D12_BACKEND="dxmt"
    export APITRACE_DOWNSTREAM_D3D12="$DXMT_D3D12_DLL"
    export DXMT_EXPERIMENT_DX12_SUPPORT="1"
    export WINEDLLPATH="$DXMT_RUNTIME_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
    export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"
    export APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1
    if [ ! -f "$WINE_PREFIX/system.reg" ]; then
        "$WINE_BIN" wineboot -u >/dev/null 2>&1 || true
    fi
}

step_trace() {
    rm -rf "$TRACE_BUNDLE"
    prepare_wine_env
    export APITRACE_TRACE_BUNDLE="$TRACE_BUNDLE"
    "$WINE_BIN" "$DEMO_EXE" | tr -d '\r' | tee "$RUN_LOG"
    unset APITRACE_TRACE_BUNDLE
    require_file "$TRACE_BUNDLE/callstream.jsonl"
    present_count="$(grep -c '"debug_name":"D3D12PresentFrame"' "$TRACE_BUNDLE/callstream.jsonl" || true)"
    [ "$present_count" -gt 0 ] || fail "trace bundle has no D3D12PresentFrame assets"
    if grep -F "scene start: dxr_smoke" "$RUN_LOG" >/dev/null; then
        fail "dxr_smoke should remain skipped in d3d12 all"
    fi
}

step_retrace() {
    rm -rf "$RETRACE_BUNDLE"
    prepare_wine_env
    export APITRACE_TRACE_BUNDLE="$RETRACE_BUNDLE"
    export APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1
    cp "$ROOT_D3D12_PROXY_DLL" "$RETRACE_BIN_DIR/d3d12.dll"
    (
        cd "$RETRACE_BIN_DIR"
        "$WINE_BIN" "$RETRACE_EXE" "$TRACE_BUNDLE"
    ) | tr -d '\r' | tee "$RETRACE_LOG"
    cp "$DXMT_D3D12_DLL" "$RETRACE_BIN_DIR/d3d12.dll"
    unset APITRACE_TRACE_BUNDLE
    unset APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES
    require_file "$RETRACE_BUNDLE/callstream.jsonl"
    present_count="$(grep -c '"debug_name":"D3D12PresentFrame"' "$RETRACE_BUNDLE/callstream.jsonl" || true)"
    [ "$present_count" -gt 0 ] || fail "retrace bundle has no D3D12PresentFrame assets"
}

step_compare() {
    python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
        --api d3d12 \
        --baseline "$TRACE_BUNDLE" \
        --candidate "$RETRACE_BUNDLE" \
        --tile 100 \
        --tile-pixel-threshold 0.95 | tee "$COMPARE_LOG"
    grep -F "failed_frames=0" "$COMPARE_LOG" >/dev/null || fail "d3d12 compare failed"
}

[ -n "$WINE_BIN" ] && [ -x "$WINE_BIN" ] || fail "missing wine binary"

ensure_ninja_build_dir "$ROOT_BUILD_DIR"
ensure_ninja_build_dir "$TEST_BUILD_DIR"

step_build
step_trace
step_retrace
step_compare

echo "test-d3d12 PASS"
