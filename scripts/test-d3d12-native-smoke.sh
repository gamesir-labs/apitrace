#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DXMT_REPO_ROOT="${APITRACE_DXMT_REPO_ROOT:-$(CDPATH= cd -- "$ROOT_DIR/../.." && pwd)}"
NATIVE_BUILD_DIR="${APITRACE_NATIVE_BUILD_DIR:-$ROOT_DIR/build/cmake-native-d3d}"
DXMT_NATIVE_BUILD_DIR="${APITRACE_DXMT_NATIVE_BUILD_DIR:-$DXMT_REPO_ROOT/build-gs-native}"
WINDOWS_BUILD_DIR="${APITRACE_WINDOWS_BUILD_DIR:-$ROOT_DIR/build/windows-cross-d3d12-assets}"
WINDOWS_TEST_BUILD_DIR="${APITRACE_WINDOWS_TEST_BUILD_DIR:-$ROOT_DIR/test/build/windows-x86_64-d3d12-assets}"
ROOT_TOOLCHAIN="${APITRACE_TOOLCHAIN:-$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake}"
GAMESIR_ROOT="${APITRACE_GAMESIR_ROOT:-$(CDPATH= cd -- "$DXMT_REPO_ROOT/.." && pwd)}"
WINE_ENV_ROOT="${APITRACE_WINE_ENV_ROOT:-$GAMESIR_ROOT/wine-enviroment}"
WINE_BIN="${APITRACE_WINE_BIN:-$WINE_ENV_ROOT/bin/wine}"
D3D_COMPILER_DLL="${APITRACE_D3D_COMPILER_DLL:-$WINE_ENV_ROOT/lib/wine/x86_64-windows/d3dcompiler_47.dll}"
WINE_D3D12_DLL="${APITRACE_WINE_D3D12_DLL:-$WINE_ENV_ROOT/lib/wine/x86_64-windows/d3d12.dll}"
DXCOMPILER_DYLIB="${APITRACE_DXCOMPILER_DYLIB:-$WINE_ENV_ROOT/D3DMetal/external/D3DMetal.framework/Versions/A/Resources/libdxcompiler.dylib}"
DXC_INCLUDE_DIR="${APITRACE_DXC_INCLUDE_DIR:-$HOME/.local/opt/dxc-linux-1.9.2602/include}"
TEST_PREFIX="${APITRACE_TEST_PREFIX:-$ROOT_DIR/test/artifacts/d3d12-native-smoke}"
WINE_PREFIX="${APITRACE_WINEPREFIX:-$TEST_PREFIX/wineprefix-assets}"
TRACE_BUNDLE="$TEST_PREFIX/trace.apitrace"
POISON_TRACE_BUNDLE="$TEST_PREFIX/poison-trace.apitrace"
NATIVE_RETRACE_BUNDLE="$TEST_PREFIX/retrace.apitrace"
POISON_RETRACE_BUNDLE="$TEST_PREFIX/poison-retrace.apitrace"
NATIVE_RETRACE_LOG="$TEST_PREFIX/retrace.log"
POISON_RETRACE_LOG="$TEST_PREFIX/poison-retrace.log"
COMPARE_LOG="$TEST_PREFIX/compare.log"
POISON_COMPARE_LOG="$TEST_PREFIX/poison-compare.log"
ABI_LOG="$TEST_PREFIX/abi.log"
ENV_LOG="$TEST_PREFIX/environment.log"
SMOKE_BIN="$NATIVE_BUILD_DIR/apitrace_test_d3d12_native_replay_smoke"
ASSET_DUMP_EXE="$TEST_PREFIX/bin/apitrace_d3d12_asset_dump.exe"
NATIVE_ASSET_DUMP_BIN="$TEST_PREFIX/bin/apitrace_d3d12_native_asset_dump"
ASSET_DIR="$TEST_PREFIX/d3d12-assets"
ASSET_DUMP_LOG="$TEST_PREFIX/asset-dump.log"
ABI_BIN="$NATIVE_BUILD_DIR/test/abi-check/apitrace_native_d3d12_smoke"
RETRACE_BIN="$NATIVE_BUILD_DIR/retrace"
BUNDLE_CHECK_BIN="$NATIVE_BUILD_DIR/bundle-check"
REQUIRE_NATIVE_REPLAY="${APITRACE_REQUIRE_D3D_NATIVE_REPLAY:-0}"
ARM64_BUILD_DIR="${APITRACE_METAL_ROOT_BUILD_DIR:-$ROOT_DIR/build/cmake-metal-arm64}"
METAL_NATIVE_SMOKE_BIN="$ARM64_BUILD_DIR/apitrace_test_metal_native_replay_smoke"
METAL_PROBE_LOG="$TEST_PREFIX/metal-probe.log"

fail() {
  echo "error: $*" >&2
  exit 1
}

skip_or_fail() {
  echo "test-d3d12-native-smoke SKIP: $*" >&2
  if [ -f "$ENV_LOG" ]; then
    echo "test-d3d12-native-smoke environment summary:" >&2
    sed -n '1,40p' "$ENV_LOG" >&2
  fi
  if [ -f "$METAL_PROBE_LOG" ]; then
    echo "test-d3d12-native-smoke Metal probe summary:" >&2
    sed -n '1,20p' "$METAL_PROBE_LOG" >&2
  fi
  if [ -f "$ABI_LOG" ]; then
    echo "test-d3d12-native-smoke ABI summary:" >&2
    sed -n '1,20p' "$ABI_LOG" >&2
  fi
  if [ "$REQUIRE_NATIVE_REPLAY" != "0" ]; then
    fail "native D3D12 smoke skipped but APITRACE_REQUIRE_D3D_NATIVE_REPLAY=$REQUIRE_NATIVE_REPLAY"
  fi
  exit 77
}

require_file() {
  [ -f "$1" ] || fail "missing file: $1"
}

ensure_ninja_build_dir() {
  local build_dir="$1"
  if [ -f "$build_dir/CMakeCache.txt" ] &&
      ! grep -F 'CMAKE_GENERATOR:INTERNAL=Ninja' "$build_dir/CMakeCache.txt" >/dev/null 2>&1; then
    rm -rf "$build_dir"
  fi
}

ensure_metal_probe_build() {
  ensure_ninja_build_dir "$ARM64_BUILD_DIR"
  cmake -S "$ROOT_DIR" -B "$ARM64_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DAPITRACE_BUILD_METAL_BACKEND=ON \
    -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=OFF
  cmake --build "$ARM64_BUILD_DIR" --target apitrace_test_metal_native_replay_smoke
}

resolve_mingw_runtime() {
  local dll_name="$1"
  local stdlib_path
  stdlib_path="$(x86_64-w64-mingw32-g++ -print-file-name=libstdc++-6.dll)"
  if [ "$stdlib_path" = "libstdc++-6.dll" ] || [ ! -f "$stdlib_path" ]; then
    stdlib_path=""
  fi
  local toolchain_bin_dir=""
  if [ -n "$stdlib_path" ]; then
    local toolchain_lib_dir
    toolchain_lib_dir="$(dirname "$stdlib_path")"
    toolchain_bin_dir="$(CDPATH= cd -- "$toolchain_lib_dir/../bin" && pwd)"
  fi

  local candidate=""
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
  esac

  if [ -n "$candidate" ] && [ -f "$candidate" ]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  return 1
}

has_native_dxc_asset_dump_inputs() {
  [ -f "$DXCOMPILER_DYLIB" ] &&
    [ -d "$DXC_INCLUDE_DIR/dxc" ] &&
    xcrun --find clang++ >/dev/null 2>&1
}

build_native_dxc_asset_dump() {
  mkdir -p "$(dirname "$NATIVE_ASSET_DUMP_BIN")"
  cmake -S "$ROOT_DIR" -B "$NATIVE_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DAPITRACE_BUILD_METAL_BACKEND=ON \
    -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=ON \
    -DAPITRACE_DXMT_NATIVE_BUILD_DIR="$DXMT_NATIVE_BUILD_DIR" \
    -DAPITRACE_DXC_INCLUDE_DIR="$DXC_INCLUDE_DIR"
  cmake --build "$NATIVE_BUILD_DIR" --target apitrace_d3d12_native_asset_dump
  cp "$NATIVE_BUILD_DIR/apitrace_d3d12_native_asset_dump" "$NATIVE_ASSET_DUMP_BIN"
  require_file "$NATIVE_ASSET_DUMP_BIN"
}

dump_assets_with_native_dxc() {
  build_native_dxc_asset_dump
  "$NATIVE_ASSET_DUMP_BIN" "$DXCOMPILER_DYLIB" "$ASSET_DIR" | tee "$ASSET_DUMP_LOG"
  grep -F "D3D12_NATIVE_ASSET_DUMP_OK" "$ASSET_DUMP_LOG" >/dev/null ||
    fail "native D3D12 asset dump failed"
}

require_wine_asset_dump_inputs() {
  [ -x "$WINE_BIN" ] || fail "missing wine binary: $WINE_BIN"
  require_file "$D3D_COMPILER_DLL"
  require_file "$WINE_D3D12_DLL"
}

require_shader_asset_pair() {
  if [ -f "$ASSET_DIR/fullscreen_triangle.vs.dxil" ]; then
    require_file "$ASSET_DIR/fullscreen_triangle.ps.dxil"
    return 0
  fi
  require_file "$ASSET_DIR/fullscreen_triangle.vs.dxbc"
  require_file "$ASSET_DIR/fullscreen_triangle.ps.dxbc"
}

[ -d "$DXMT_NATIVE_BUILD_DIR" ] || fail "missing DXMT native build dir: $DXMT_NATIVE_BUILD_DIR"
require_file "$DXMT_NATIVE_BUILD_DIR/src/nativemetal/winemetal.dylib"
require_file "$DXMT_NATIVE_BUILD_DIR/src/dxgi/dxgi.dylib"
require_file "$DXMT_NATIVE_BUILD_DIR/src/d3d12/d3d12.dylib"

ensure_ninja_build_dir "$NATIVE_BUILD_DIR"
ensure_ninja_build_dir "$WINDOWS_BUILD_DIR"
ensure_ninja_build_dir "$WINDOWS_TEST_BUILD_DIR"
mkdir -p "$TEST_PREFIX"

{
  echo "uid=$(id -u)"
  echo "arch=$(arch)"
  echo "launchctl_manager=$(launchctl managername 2>/dev/null || true)"
  echo "term=${TERM:-}"
  echo "dxmt_native_build_dir=$DXMT_NATIVE_BUILD_DIR"
  file "$DXMT_NATIVE_BUILD_DIR/src/nativemetal/winemetal.dylib" \
       "$DXMT_NATIVE_BUILD_DIR/src/dxgi/dxgi.dylib" \
       "$DXMT_NATIVE_BUILD_DIR/src/d3d12/d3d12.dylib" 2>/dev/null || true
  /usr/sbin/system_profiler SPDisplaysDataType -detailLevel mini 2>/dev/null |
    sed -n '1,80p' || true
} >"$ENV_LOG"

rm -rf "$ASSET_DIR"
mkdir -p "$ASSET_DIR"
if has_native_dxc_asset_dump_inputs; then
  dump_assets_with_native_dxc
else
  require_wine_asset_dump_inputs
  cmake -S "$ROOT_DIR" -B "$WINDOWS_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
  cmake --build "$WINDOWS_BUILD_DIR" --target apitrace_platform_windows_d3d12

  cmake -S "$ROOT_DIR/test" -B "$WINDOWS_TEST_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
  cmake --build "$WINDOWS_TEST_BUILD_DIR" --target apitrace_d3d12_asset_dump
  mkdir -p "$TEST_PREFIX/bin"
  cp "$WINDOWS_TEST_BUILD_DIR/apitrace_d3d12_asset_dump.exe" "$ASSET_DUMP_EXE"

  require_file "$ASSET_DUMP_EXE"
  require_file "$WINDOWS_BUILD_DIR/d3d12.dll"
  cp "$WINDOWS_BUILD_DIR/d3d12.dll" "$TEST_PREFIX/bin/d3d12.dll"
  cp "$D3D_COMPILER_DLL" "$TEST_PREFIX/bin/d3dcompiler_47.dll"
  for runtime_dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    runtime_path="$(resolve_mingw_runtime "$runtime_dll" || true)"
    [ -n "$runtime_path" ] || fail "missing MinGW runtime DLL: $runtime_dll"
    cp "$runtime_path" "$TEST_PREFIX/bin/$runtime_dll"
  done

  WINEDLLOVERRIDES="mscoree,mshtml=d;d3d12,d3dcompiler_47=n,b" \
  WINEDEBUG="-all" \
  WINEARCH="win64" \
  WINEPREFIX="$WINE_PREFIX" \
  APITRACE_DOWNSTREAM_D3D12="$WINE_D3D12_DLL" \
    "$WINE_BIN" "$ASSET_DUMP_EXE" "$ASSET_DIR" | tr -d '\r' | tee "$ASSET_DUMP_LOG"
  grep -F "D3D12_ASSET_DUMP_OK" "$ASSET_DUMP_LOG" >/dev/null || fail "D3D12 asset dump failed"
fi
require_file "$ASSET_DIR/fullscreen_triangle.rootsig"
require_shader_asset_pair

cmake -S "$ROOT_DIR" -B "$NATIVE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPITRACE_BUILD_METAL_BACKEND=ON \
  -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=ON \
  -DAPITRACE_DXMT_NATIVE_BUILD_DIR="$DXMT_NATIVE_BUILD_DIR"

cmake --build "$NATIVE_BUILD_DIR" --target \
  apitrace_test_d3d12_native_replay_smoke \
  apitrace_native_d3d12_smoke \
  apitrace_bundle_check \
  apitrace_retrace

require_file "$SMOKE_BIN"
require_file "$ABI_BIN"
require_file "$BUNDLE_CHECK_BIN"
require_file "$RETRACE_BIN"

rm -rf "$TRACE_BUNDLE" "$POISON_TRACE_BUNDLE" "$NATIVE_RETRACE_BUNDLE" "$POISON_RETRACE_BUNDLE"
"$SMOKE_BIN" "$TRACE_BUNDLE" "$ASSET_DIR"
"$SMOKE_BIN" "$POISON_TRACE_BUNDLE" "$ASSET_DIR" --poison-present-frame
"$BUNDLE_CHECK_BIN" "$TRACE_BUNDLE" >/dev/null
"$BUNDLE_CHECK_BIN" "$POISON_TRACE_BUNDLE" >/dev/null
"$RETRACE_BIN" --validate-only "$TRACE_BUNDLE" >/dev/null
"$RETRACE_BIN" --validate-only "$POISON_TRACE_BUNDLE" >/dev/null

ensure_metal_probe_build
if [ -x "$METAL_NATIVE_SMOKE_BIN" ]; then
  set +e
  "$METAL_NATIVE_SMOKE_BIN" "$TEST_PREFIX/metal-probe" >"$METAL_PROBE_LOG" 2>&1
  metal_probe_status=$?
  set -e
  if [ "$metal_probe_status" -ne 0 ] && [ "$metal_probe_status" -ne 77 ]; then
    fail "Metal native probe failed before D3D native smoke; see $METAL_PROBE_LOG"
  fi
fi

DXMT_EXPERIMENT_DX12_SUPPORT=1 "$ABI_BIN" "$DXMT_NATIVE_BUILD_DIR" | tee "$ABI_LOG"
if grep -F "ABI_SMOKE_SKIP" "$ABI_LOG" >/dev/null; then
  skip_or_fail "no Metal-backed DXGI adapter in this process"
fi
grep -F "ABI_SMOKE_OK" "$ABI_LOG" >/dev/null || fail "ABI smoke did not report ABI_SMOKE_OK"

DXMT_EXPERIMENT_DX12_SUPPORT=1 \
APITRACE_TRACE_BUNDLE="$NATIVE_RETRACE_BUNDLE" \
APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
  "$RETRACE_BIN" "$TRACE_BUNDLE" | tee "$NATIVE_RETRACE_LOG"
require_file "$NATIVE_RETRACE_BUNDLE/callstream.jsonl"

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$TRACE_BUNDLE" \
  --candidate "$NATIVE_RETRACE_BUNDLE" \
  --tile 4 \
  --tile-pixel-threshold 1.0 | tee "$COMPARE_LOG"
grep -F "failed_frames=0" "$COMPARE_LOG" >/dev/null || fail "native d3d12 smoke compare failed frames"
grep -F "failed_tiles=0" "$COMPARE_LOG" >/dev/null || fail "native d3d12 smoke compare failed tiles"

DXMT_EXPERIMENT_DX12_SUPPORT=1 \
APITRACE_TRACE_BUNDLE="$POISON_RETRACE_BUNDLE" \
APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
  "$RETRACE_BIN" "$POISON_TRACE_BUNDLE" | tee "$POISON_RETRACE_LOG"
require_file "$POISON_RETRACE_BUNDLE/callstream.jsonl"

set +e
python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$POISON_TRACE_BUNDLE" \
  --candidate "$POISON_RETRACE_BUNDLE" \
  --tile 4 \
  --tile-pixel-threshold 1.0 | tee "$POISON_COMPARE_LOG"
poison_compare_status=$?
set -e
if [ "$poison_compare_status" -eq 0 ] &&
    grep -F "failed_frames=0" "$POISON_COMPARE_LOG" >/dev/null &&
    grep -F "failed_tiles=0" "$POISON_COMPARE_LOG" >/dev/null; then
  fail "poisoned baseline matched native retrace output; native replay may be reusing recorded PresentFrame pixels"
fi

echo "test-d3d12-native-smoke PASS"
