#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DXMT_REPO_ROOT="${APITRACE_DXMT_REPO_ROOT:-$(CDPATH= cd -- "$ROOT_DIR/../.." && pwd)}"
NATIVE_BUILD_DIR="${APITRACE_NATIVE_BUILD_DIR:-$ROOT_DIR/build/cmake-native-d3d}"
DXMT_NATIVE_BUILD_DIR="${APITRACE_DXMT_NATIVE_BUILD_DIR:-$DXMT_REPO_ROOT/build-gs-native}"
WINE_TEST_PREFIX="${APITRACE_D3D12_WINE_TEST_PREFIX:-$ROOT_DIR/test/artifacts/d3d12}"
NATIVE_PREFIX="${APITRACE_D3D12_NATIVE_PREFIX:-$ROOT_DIR/test/artifacts/d3d12-native}"
TRACE_BUNDLE="$WINE_TEST_PREFIX/trace.apitrace"
POISON_TRACE_BUNDLE="$NATIVE_PREFIX/poison-trace.apitrace"
NATIVE_RETRACE_BUNDLE="$NATIVE_PREFIX/retrace.apitrace"
POISON_RETRACE_BUNDLE="$NATIVE_PREFIX/poison-retrace.apitrace"
NATIVE_RETRACE_LOG="$NATIVE_PREFIX/retrace.log"
POISON_RETRACE_LOG="$NATIVE_PREFIX/poison-retrace.log"
COMPARE_LOG="$NATIVE_PREFIX/compare.log"
POISON_COMPARE_LOG="$NATIVE_PREFIX/poison-compare.log"
POISON_BASELINE_COMPARE_LOG="$NATIVE_PREFIX/poison-baseline-compare.log"
RETRACE_BIN="$NATIVE_BUILD_DIR/retrace"
BUNDLE_CHECK_BIN="$NATIVE_BUILD_DIR/bundle-check"

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

if [ "${APITRACE_REUSE_D3D12_TRACE:-0}" != "1" ] || [ ! -f "$TRACE_BUNDLE/callstream.jsonl" ]; then
  # Reuse the existing Wine path to produce the authoritative D3D12 trace bundle.
  APITRACE_TEST_PREFIX="$WINE_TEST_PREFIX" bash "$ROOT_DIR/scripts/test-d3d12.sh"
fi
require_file "$TRACE_BUNDLE/callstream.jsonl"

ensure_ninja_build_dir "$NATIVE_BUILD_DIR"
mkdir -p "$NATIVE_PREFIX"

cmake -S "$ROOT_DIR" -B "$NATIVE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPITRACE_BUILD_METAL_BACKEND=ON \
  -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=ON \
  -DAPITRACE_DXMT_NATIVE_BUILD_DIR="$DXMT_NATIVE_BUILD_DIR"
cmake --build "$NATIVE_BUILD_DIR" --target retrace
cmake --build "$NATIVE_BUILD_DIR" --target apitrace_bundle_check
require_file "$RETRACE_BIN"
require_file "$BUNDLE_CHECK_BIN"

"$BUNDLE_CHECK_BIN" "$TRACE_BUNDLE" >/dev/null
"$RETRACE_BIN" --validate-only "$TRACE_BUNDLE" >/dev/null
python3 "$ROOT_DIR/scripts/lib/poison_present_frame.py" \
  --api d3d12 \
  --source "$TRACE_BUNDLE" \
  --output "$POISON_TRACE_BUNDLE" \
  --frame-index 0
"$BUNDLE_CHECK_BIN" "$POISON_TRACE_BUNDLE" >/dev/null
"$RETRACE_BIN" --validate-only "$POISON_TRACE_BUNDLE" >/dev/null

rm -rf "$NATIVE_RETRACE_BUNDLE"
DXMT_EXPERIMENT_DX12_SUPPORT=1 \
APITRACE_TRACE_BUNDLE="$NATIVE_RETRACE_BUNDLE" \
APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
  "$RETRACE_BIN" "$TRACE_BUNDLE" | tee "$NATIVE_RETRACE_LOG"
require_file "$NATIVE_RETRACE_BUNDLE/callstream.jsonl"

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$TRACE_BUNDLE" \
  --candidate "$NATIVE_RETRACE_BUNDLE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 | tee "$COMPARE_LOG"
grep -F "failed_frames=0" "$COMPARE_LOG" >/dev/null || fail "native d3d12 compare failed"

rm -rf "$POISON_RETRACE_BUNDLE"
DXMT_EXPERIMENT_DX12_SUPPORT=1 \
APITRACE_TRACE_BUNDLE="$POISON_RETRACE_BUNDLE" \
APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
  "$RETRACE_BIN" "$POISON_TRACE_BUNDLE" | tee "$POISON_RETRACE_LOG"
require_file "$POISON_RETRACE_BUNDLE/callstream.jsonl"

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$TRACE_BUNDLE" \
  --candidate "$POISON_RETRACE_BUNDLE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 | tee "$POISON_BASELINE_COMPARE_LOG"
grep -F "failed_frames=0" "$POISON_BASELINE_COMPARE_LOG" >/dev/null ||
  fail "native d3d12 poisoned retrace no longer matches original command-rendered trace"

set +e
python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$POISON_TRACE_BUNDLE" \
  --candidate "$POISON_RETRACE_BUNDLE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 | tee "$POISON_COMPARE_LOG"
poison_compare_status=$?
set -e
if [ "$poison_compare_status" -eq 0 ] &&
    grep -F "failed_frames=0" "$POISON_COMPARE_LOG" >/dev/null &&
    grep -F "failed_tiles=0" "$POISON_COMPARE_LOG" >/dev/null; then
  fail "poisoned baseline matched native D3D12 retrace output; native replay may be reusing recorded PresentFrame pixels"
fi

echo "test-d3d12-native PASS"
