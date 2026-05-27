#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT_BUILD_DIR="${APITRACE_ROOT_BUILD_DIR:-$ROOT_DIR/build/cmake-arm64}"
TEST_BUILD_DIR="${APITRACE_TEST_BUILD_DIR:-$ROOT_DIR/test/build/macos-native-arm64}"
TEST_PREFIX="${APITRACE_TEST_PREFIX:-$ROOT_DIR/test/artifacts/metal}"
TRACE_BUNDLE="$TEST_PREFIX/trace.apitrace"
RETRACE_BUNDLE="$TEST_PREFIX/retrace.apitrace"
DEMO_BIN="$TEST_PREFIX/bin/apitrace_test_metal"
RETRACE_BIN="$ROOT_BUILD_DIR/retrace"
RUN_LOG="$TEST_PREFIX/run.log"
RETRACE_LOG="$TEST_PREFIX/retrace.log"
COMPARE_LOG="$TEST_PREFIX/compare.log"

EXPECTED_SCENES=(
  metal_smoke_triangle
  metal_indexed_instancing
  metal_textured_quad
  metal_compute_uav
  metal_indirect_draw
  metal_argument_buffer
  metal_multi_pass
  metal_present_smoke
)

fail() {
  echo "error: $*" >&2
  exit 1
}

require_file() {
  [ -f "$1" ] || fail "missing file: $1"
}

ensure_ninja_build_dir() {
  local build_dir="$1"
  if [ -f "$build_dir/CMakeCache.txt" ] && ! grep -F 'CMAKE_GENERATOR:INTERNAL=Ninja' "$build_dir/CMakeCache.txt" >/dev/null 2>&1; then
    rm -rf "$build_dir"
  fi
}

step_build() {
  rm -rf "$TEST_PREFIX"
  mkdir -p "$TEST_PREFIX"

  /opt/homebrew/bin/cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DAPITRACE_BUILD_METAL_BACKEND=ON
  /opt/homebrew/bin/cmake --build "$ROOT_BUILD_DIR"

  /opt/homebrew/bin/cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DAPITRACE_BUILD_METAL_DEMO=ON \
    -DAPITRACE_ROOT_BUILD_DIR="$ROOT_BUILD_DIR"
  /opt/homebrew/bin/cmake --build "$TEST_BUILD_DIR"
  /opt/homebrew/bin/cmake --install "$TEST_BUILD_DIR" --prefix "$TEST_PREFIX"

  require_file "$DEMO_BIN"
  require_file "$RETRACE_BIN"
}

step_trace() {
  rm -rf "$TRACE_BUNDLE"
  APITRACE_METAL_BUNDLE="$TRACE_BUNDLE" "$DEMO_BIN" | tee "$RUN_LOG"
  for scene_name in "${EXPECTED_SCENES[@]}"; do
    grep -F "scene pass: $scene_name" "$RUN_LOG" >/dev/null || fail "missing scene pass: $scene_name"
  done
  grep -F "summary: passed=8 failed=0 skipped=0" "$RUN_LOG" >/dev/null || fail "unexpected metal summary"
  require_file "$TRACE_BUNDLE/callstream.jsonl"
}

step_retrace() {
  rm -rf "$RETRACE_BUNDLE"
  APITRACE_TRACE_BUNDLE="$RETRACE_BUNDLE" APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
    "$RETRACE_BIN" --metal "$TRACE_BUNDLE" | tee "$RETRACE_LOG"
  require_file "$RETRACE_BUNDLE/callstream.jsonl"
}

step_compare() {
  python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
    --api metal \
    --baseline "$TRACE_BUNDLE" \
    --candidate "$RETRACE_BUNDLE" \
    --tile 100 \
    --tile-pixel-threshold 0.95 | tee "$COMPARE_LOG"
  grep -F "failed_frames=0" "$COMPARE_LOG" >/dev/null || fail "metal compare failed"
}

ensure_ninja_build_dir "$ROOT_BUILD_DIR"
ensure_ninja_build_dir "$TEST_BUILD_DIR"

step_build
step_trace
step_retrace
step_compare

echo "test-metal PASS"
