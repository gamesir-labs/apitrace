#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT_BUILD_DIR="${APITRACE_ROOT_BUILD_DIR:-$ROOT_DIR/build/cmake-native-d3d-abi}"
DXMT_NATIVE_BUILD_DIR="${APITRACE_DXMT_NATIVE_BUILD_DIR:-$ROOT_DIR/../dxmt/build-gs-native}"
SMOKE_BIN="$ROOT_BUILD_DIR/test/abi-check/apitrace_native_d3d12_smoke"
SMOKE_LOG="$ROOT_DIR/test/artifacts/d3d-native-abi/smoke.log"

fail() {
  echo "error: $*" >&2
  exit 1
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

[ -d "$DXMT_NATIVE_BUILD_DIR" ] || fail "missing DXMT native build dir: $DXMT_NATIVE_BUILD_DIR"
require_file "$DXMT_NATIVE_BUILD_DIR/src/nativemetal/winemetal.dylib"
require_file "$DXMT_NATIVE_BUILD_DIR/src/dxgi/dxgi.dylib"
require_file "$DXMT_NATIVE_BUILD_DIR/src/d3d12/d3d12.dylib"

ensure_ninja_build_dir "$ROOT_BUILD_DIR"
mkdir -p "$(dirname "$SMOKE_LOG")"

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPITRACE_BUILD_METAL_BACKEND=ON \
  -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=ON \
  -DAPITRACE_DXMT_NATIVE_BUILD_DIR="$DXMT_NATIVE_BUILD_DIR"

cmake --build "$ROOT_BUILD_DIR" --target apitrace_native_d3d12_smoke
require_file "$SMOKE_BIN"

DXMT_EXPERIMENT_DX12_SUPPORT=1 "$SMOKE_BIN" "$DXMT_NATIVE_BUILD_DIR" | tee "$SMOKE_LOG"
grep -F "ABI_SMOKE_OK" "$SMOKE_LOG" >/dev/null || fail "ABI smoke did not report ABI_SMOKE_OK"

echo "test-d3d-native-abi PASS"
