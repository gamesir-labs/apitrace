#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/cmake"
TEST_BUILD_DIR="$ROOT_DIR/test/build/macos-native"
TEST_PREFIX="$ROOT_DIR/test/artifacts/macos-native"
DEMO_BIN="$TEST_PREFIX/bin/apitrace_metal_demo"
TRACE_BUNDLE="$TEST_PREFIX/metal-scene-all.apitrace"
PIXEL_FIXTURE="$ROOT_DIR/test/fixtures/retrace/metal-scene-all-pixel/metal-scene-all-pixel.apitrace"
PIXEL_RETRACE="$TEST_PREFIX/metal-scene-all-pixel-retrace.apitrace"
RUN_LOG="$TEST_PREFIX/run.log"
PIXEL_COMPARE_LOG="$TEST_PREFIX/pixel-compare.log"

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

require_metal_call() {
  local function_name="$1"
  grep -F "\"function\":\"$function_name\"" "$TRACE_BUNDLE/metal-callstream.jsonl" >/dev/null \
    || { echo "missing metal call: $function_name" >&2; exit 1; }
}

rm -rf "$TEST_BUILD_DIR" "$TEST_PREFIX" "$TRACE_BUNDLE" "$PIXEL_RETRACE"
mkdir -p "$TEST_PREFIX"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DAPITRACE_BUILD_METAL_BACKEND=ON
cmake --build "$BUILD_DIR"

cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAPITRACE_BUILD_METAL_DEMO=ON \
  -DAPITRACE_ROOT_BUILD_DIR="$BUILD_DIR"
cmake --build "$TEST_BUILD_DIR"
cmake --install "$TEST_BUILD_DIR" --prefix "$TEST_PREFIX"

APITRACE_METAL_BUNDLE="$TRACE_BUNDLE" "$DEMO_BIN" --scene all | tee "$RUN_LOG"
for scene_name in "${EXPECTED_SCENES[@]}"; do
  grep -F "scene pass: $scene_name" "$RUN_LOG" >/dev/null || { echo "missing scene pass: $scene_name" >&2; exit 1; }
done
grep -F "failed=0" "$RUN_LOG" >/dev/null

for function_name in \
  MTLCommandBuffer.commit \
  MTLRenderCommandEncoder.drawPrimitives \
  MTLRenderCommandEncoder.drawIndexedPrimitives \
  MTLRenderCommandEncoder.drawPrimitivesIndirect \
  MTLComputeCommandEncoder.dispatchThreadgroups \
  MTLComputeCommandEncoder.dispatchThreadgroupsIndirect \
  MTLBlitCommandEncoder.copyFromBuffer \
  MTLRenderCommandEncoder.useResources \
  MTLRenderCommandEncoder.setVertexBuffer \
  MTLRenderCommandEncoder.setFragmentTexture \
  MTLDevice.newRenderPipelineState \
  MTLDevice.newComputePipelineState \
  MTLCommandBuffer.presentDrawable; do
  require_metal_call "$function_name"
done

python3 "$ROOT_DIR/scripts/check-metal-link-coverage.py" \
  "$TRACE_BUNDLE/analysis/translation-links.jsonl"

APITRACE_TRACE_BUNDLE="$PIXEL_RETRACE" \
APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
  "$BUILD_DIR/retrace" --metal "$PIXEL_FIXTURE"

python3 "$ROOT_DIR/scripts/compare-metal-present-frames.py" --tolerance 0 \
  "$PIXEL_FIXTURE" "$PIXEL_RETRACE" | tee "$PIXEL_COMPARE_LOG"
grep -F "mismatched_frames=0" "$PIXEL_COMPARE_LOG" >/dev/null
grep -F "mismatched_pixels=0" "$PIXEL_COMPARE_LOG" >/dev/null
grep -F "max_channel_delta=0" "$PIXEL_COMPARE_LOG" >/dev/null

echo "validate-metal-native PASS"
