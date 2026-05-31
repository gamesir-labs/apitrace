#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT_BUILD_DIR="${APITRACE_ROOT_BUILD_DIR:-$ROOT_DIR/build/cmake-metal-arm64}"
TEST_PREFIX="${APITRACE_TEST_PREFIX:-$ROOT_DIR/test/artifacts/cross-api-smoke}"
TRACE_BUNDLE="$TEST_PREFIX/trace.apitrace"
POISON_TRACE_BUNDLE="$TEST_PREFIX/poisoned-trace.apitrace"
METAL_RETRACE_BUNDLE="$TEST_PREFIX/metal-retrace.apitrace"
POISON_METAL_RETRACE_BUNDLE="$TEST_PREFIX/poisoned-metal-retrace.apitrace"
COMPARE_LOG="$TEST_PREFIX/compare.log"
METAL_RETRACE_LOG="$TEST_PREFIX/metal-retrace.log"
METAL_RETRACE_COMPARE_LOG="$TEST_PREFIX/metal-retrace-compare.log"
POISON_METAL_RETRACE_LOG="$TEST_PREFIX/poisoned-metal-retrace.log"
POISON_METAL_BASELINE_COMPARE_LOG="$TEST_PREFIX/poisoned-metal-baseline-compare.log"
POISON_METAL_COMPARE_LOG="$TEST_PREFIX/poisoned-metal-compare.log"
METAL_NATIVE_LOG="$TEST_PREFIX/metal-native-replay-smoke.log"
BUNDLE_CHECK_BIN="$ROOT_BUILD_DIR/bundle-check"
RETRACE_BIN="$ROOT_BUILD_DIR/retrace"
SMOKE_BIN="$ROOT_BUILD_DIR/apitrace_test_cross_api_bundle_closure"
REQUIRE_METAL_NATIVE_REPLAY="${APITRACE_REQUIRE_METAL_NATIVE_REPLAY:-0}"

fail() {
  echo "error: $*" >&2
  exit 1
}

require_file() {
  [ -f "$1" ] || fail "missing file: $1"
}

skip_or_fail_metal_native() {
  echo "test-cross-api-smoke SKIP native Metal retrace: $*" >&2
  if [ "$REQUIRE_METAL_NATIVE_REPLAY" != "0" ]; then
    fail "native Metal retrace skipped but APITRACE_REQUIRE_METAL_NATIVE_REPLAY=$REQUIRE_METAL_NATIVE_REPLAY"
  fi
  return 77
}

run_metal_retrace_capture() {
  local source_bundle="$1"
  local capture_bundle="$2"
  local log_path="$3"

  rm -rf "$capture_bundle"
  set +e
  APITRACE_TRACE_BUNDLE="$capture_bundle" \
  APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
    "$RETRACE_BIN" --metal "$source_bundle" 2>&1 | tee "$log_path"
  local status=$?
  set -e
  if [ "$status" -ne 0 ]; then
    if grep -F "MTLCreateSystemDefaultDevice returned nil" "$log_path" >/dev/null; then
      skip_or_fail_metal_native "MTLCreateSystemDefaultDevice returned nil"
      return 77
    fi
    exit "$status"
  fi
  require_file "$capture_bundle/callstream.jsonl"
}

mkdir -p "$TEST_PREFIX"

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DAPITRACE_BUILD_METAL_BACKEND=ON \
  -DAPITRACE_BUILD_D3D_NATIVE_RETRACE=OFF

cmake --build "$ROOT_BUILD_DIR" --target \
  apitrace_test_cross_api_bundle_closure \
  apitrace_test_metal_native_replay_smoke \
  apitrace_bundle_check \
  apitrace_retrace

require_file "$SMOKE_BIN"
require_file "$BUNDLE_CHECK_BIN"
require_file "$RETRACE_BIN"

rm -rf "$TRACE_BUNDLE"
"$SMOKE_BIN" "$TRACE_BUNDLE" "$BUNDLE_CHECK_BIN" "$RETRACE_BIN"

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --baseline-api d3d12 \
  --candidate-api metal \
  --baseline "$TRACE_BUNDLE" \
  --candidate "$TRACE_BUNDLE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 | tee "$COMPARE_LOG"

grep -F "api=d3d12-to-metal" "$COMPARE_LOG" >/dev/null || fail "cross-api compare did not run"
grep -F "failed_frames=0" "$COMPARE_LOG" >/dev/null || fail "cross-api compare failed frames"
grep -F "failed_tiles=0" "$COMPARE_LOG" >/dev/null || fail "cross-api compare failed tiles"

if run_metal_retrace_capture "$TRACE_BUNDLE" "$METAL_RETRACE_BUNDLE" "$METAL_RETRACE_LOG"; then
  python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
    --baseline-api d3d12 \
    --candidate-api metal \
    --baseline "$TRACE_BUNDLE" \
    --candidate "$METAL_RETRACE_BUNDLE" \
    --tile 100 \
    --tile-pixel-threshold 0.95 | tee "$METAL_RETRACE_COMPARE_LOG"

  grep -F "api=d3d12-to-metal" "$METAL_RETRACE_COMPARE_LOG" >/dev/null || fail "metal retrace compare did not run"
  grep -F "failed_frames=0" "$METAL_RETRACE_COMPARE_LOG" >/dev/null || fail "metal retrace compare failed frames"
  grep -F "failed_tiles=0" "$METAL_RETRACE_COMPARE_LOG" >/dev/null || fail "metal retrace compare failed tiles"

  python3 "$ROOT_DIR/scripts/lib/poison_present_frame.py" \
    --api metal \
    --source "$TRACE_BUNDLE" \
    --output "$POISON_TRACE_BUNDLE" \
    --frame-index 0 >/dev/null

  run_metal_retrace_capture "$POISON_TRACE_BUNDLE" "$POISON_METAL_RETRACE_BUNDLE" "$POISON_METAL_RETRACE_LOG"

  python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
    --baseline-api d3d12 \
    --candidate-api metal \
    --baseline "$TRACE_BUNDLE" \
    --candidate "$POISON_METAL_RETRACE_BUNDLE" \
    --tile 100 \
    --tile-pixel-threshold 0.95 | tee "$POISON_METAL_BASELINE_COMPARE_LOG"

  grep -F "failed_frames=0" "$POISON_METAL_BASELINE_COMPARE_LOG" >/dev/null ||
    fail "poisoned Metal retrace no longer matches original D3D12 baseline"
  grep -F "failed_tiles=0" "$POISON_METAL_BASELINE_COMPARE_LOG" >/dev/null ||
    fail "poisoned Metal retrace no longer matches original D3D12 tiles"

  set +e
  python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
    --api metal \
    --baseline "$POISON_TRACE_BUNDLE" \
    --candidate "$POISON_METAL_RETRACE_BUNDLE" \
    --tile 100 \
    --tile-pixel-threshold 0.95 | tee "$POISON_METAL_COMPARE_LOG"
  poison_compare_status=$?
  set -e
  if [ "$poison_compare_status" -eq 0 ] &&
      grep -F "failed_frames=0" "$POISON_METAL_COMPARE_LOG" >/dev/null &&
      grep -F "failed_tiles=0" "$POISON_METAL_COMPARE_LOG" >/dev/null; then
    fail "poisoned MetalPresentFrame matched Metal retrace output; native replay may be reusing recorded frame pixels"
  fi
fi

NATIVE_SMOKE_BIN="$ROOT_BUILD_DIR/apitrace_test_metal_native_replay_smoke"
if [ -x "$NATIVE_SMOKE_BIN" ]; then
  set +e
  "$NATIVE_SMOKE_BIN" "$TEST_PREFIX/metal-native-replay-smoke" | tee "$METAL_NATIVE_LOG"
  native_status=$?
  set -e
  if [ "$native_status" -ne 0 ] && [ "$native_status" -ne 77 ]; then
    exit "$native_status"
  fi
fi

echo "cross_api_smoke PASS"
