#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="${APITRACE_WINE_BIN:-$ROOT_DIR/../wine-enviroment/bin/wine}"
if [ ! -x "$WINE_BIN" ]; then
    WINE_BIN="$(command -v wine || true)"
fi

HOST_BUILD_DIR="${APITRACE_HOST_BUILD_DIR:-$ROOT_DIR/build/cmake-arm64}"
WINDOWS_BUILD_DIR="${APITRACE_WINDOWS_BUILD_DIR:-$ROOT_DIR/build/windows-cross}"
TEST_PREFIX="${APITRACE_D3D12_TEST_PREFIX:-$ROOT_DIR/test/artifacts/d3d12}"
LINK_DIR="${APITRACE_LINK_ARTIFACT_DIR:-$ROOT_DIR/test/artifacts/link}"
LINK_TRACE="${APITRACE_LINK_TRACE_BUNDLE:-$ROOT_DIR/test/artifacts/link-probe-d3d12.apitrace}"
METAL_RETRACE_BUNDLE="$LINK_DIR/d3d12-metal-retrace.apitrace"
RUN_LOG="$LINK_DIR/d3d12-trace.log"
METAL_RETRACE_LOG="$LINK_DIR/d3d12-metal-retrace.log"
METAL_COMPARE_LOG="$LINK_DIR/d3d12-metal-compare.log"

DXMT_REPO_ROOT="${APITRACE_DXMT_REPO_ROOT:-$ROOT_DIR/../dxmt}"
DXMT_BUILD_DIR="${APITRACE_DXMT_BUILD_DIR:-$DXMT_REPO_ROOT/build-gs-native-builtin}"
DXMT_STAGE_DIR="${APITRACE_DXMT_STAGE_DIR:-$ROOT_DIR/test/artifacts/dxmt-runtime-d3d12}"
DXMT_RUNTIME_ROOT=""
DXMT_UNIX_DIR=""

DEMO_BIN_DIR="$TEST_PREFIX/bin"
DEMO_EXE="$DEMO_BIN_DIR/apitrace_test_d3d12.exe"
ROOT_D3D12_PROXY_DLL="$WINDOWS_BUILD_DIR/d3d12.dll"
NATIVE_RETRACE="$HOST_BUILD_DIR/retrace"

fail() {
    echo "error: $*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || fail "missing file: $1"
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
    meson compile -C "$DXMT_BUILD_DIR"
    DESTDIR="$DXMT_STAGE_DIR" meson install -C "$DXMT_BUILD_DIR" --tags runtime,nvext
    DXMT_RUNTIME_ROOT="$(resolve_dxmt_stage_prefix)"
    DXMT_UNIX_DIR="$DXMT_RUNTIME_ROOT/x86_64-unix"
}

prepare_demo_runtime() {
    require_file "$ROOT_D3D12_PROXY_DLL"
    require_file "$DEMO_EXE"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12.dll"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12core.dll"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/dxgi.dll"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/winemetal.dll"

    cp "$ROOT_D3D12_PROXY_DLL" "$DEMO_BIN_DIR/d3d12.dll"
    cp "$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12core.dll" "$DEMO_BIN_DIR/d3d12core.dll"
    cp "$DXMT_RUNTIME_ROOT/x86_64-windows/dxgi.dll" "$DEMO_BIN_DIR/dxgi.dll"
    cp "$DXMT_RUNTIME_ROOT/x86_64-windows/winemetal.dll" "$DEMO_BIN_DIR/winemetal.dll"
}

prepare_wine_env() {
    export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d12,d3d12core,dxgi,winemetal=n,b"
    export WINEDEBUG="-all"
    export WINEARCH="win64"
    export WINEPREFIX="${APITRACE_WINEPREFIX:-$ROOT_DIR/test/artifacts/wineprefix-d3d12}"
    export APITRACE_D3D12_BACKEND="dxmt"
    export APITRACE_DOWNSTREAM_D3D12="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12.dll"
    export DXMT_EXPERIMENT_DX12_SUPPORT=1
    export WINEDLLPATH="$DXMT_RUNTIME_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
    export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"
    export APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1
}

trace_d3d12_to_metal() {
    rm -rf "$LINK_TRACE"
    prepare_wine_env
    export APITRACE_TRACE_BUNDLE="$LINK_TRACE"
    export APITRACE_METAL_BUNDLE="$LINK_TRACE"
    "$WINE_BIN" "$DEMO_EXE" | tr -d '\r' | tee "$RUN_LOG"
    unset APITRACE_TRACE_BUNDLE
    unset APITRACE_METAL_BUNDLE

    require_file "$LINK_TRACE/callstream.jsonl"
    require_file "$LINK_TRACE/metal-callstream.jsonl"
    require_file "$LINK_TRACE/analysis/translation-links.jsonl"
    if grep -F "scene start: dxr_smoke" "$RUN_LOG" >/dev/null; then
        fail "dxr_smoke should remain skipped in d3d12 all"
    fi
}

retrace_metal() {
    require_file "$NATIVE_RETRACE"
    rm -rf "$METAL_RETRACE_BUNDLE"
    APITRACE_TRACE_BUNDLE="$METAL_RETRACE_BUNDLE" \
    APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
        "$NATIVE_RETRACE" --metal "$LINK_TRACE" | tee "$METAL_RETRACE_LOG"
    require_file "$METAL_RETRACE_BUNDLE/callstream.jsonl"
}

compare_metal() {
    python3 - "$ROOT_DIR" "$LINK_TRACE" "$METAL_RETRACE_BUNDLE" <<'PY' | tee "$METAL_COMPARE_LOG"
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
baseline = pathlib.Path(sys.argv[2])
candidate = pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "scripts" / "lib"))
import present_frame_compare as compare

baseline_frames = compare.load_frames(baseline, "d3d12")
candidate_frames = compare.load_frames(candidate, "metal")
if len(baseline_frames) != len(candidate_frames):
    compare.fail(f"frame counts differ: baseline={len(baseline_frames)} candidate={len(candidate_frames)}")

failed_frames = 0
failed_tiles = 0
max_channel_delta = 0
for index, (baseline_frame, candidate_frame) in enumerate(zip(baseline_frames, candidate_frames)):
    if (
        baseline_frame.width != candidate_frame.width
        or baseline_frame.height != candidate_frame.height
        or baseline_frame.row_pitch != candidate_frame.row_pitch
    ):
        compare.fail(
            f"frame {index}: metadata differs "
            f"baseline=({baseline_frame.width}x{baseline_frame.height}, row_pitch={baseline_frame.row_pitch}) "
            f"candidate=({candidate_frame.width}x{candidate_frame.height}, row_pitch={candidate_frame.row_pitch})"
        )

    baseline_pixels = compare.read_frame_rgba(baseline, baseline_frame)
    candidate_pixels = compare.read_frame_rgba(candidate, candidate_frame)
    frame_failures = []
    for tile_x, tile_y, tile_width, tile_height in compare.iter_tiles(
        baseline_frame.width,
        baseline_frame.height,
        100,
    ):
        tile = compare.compare_tile(
            baseline_pixels,
            candidate_pixels,
            baseline_frame.width,
            tile_x,
            tile_y,
            tile_width,
            tile_height,
        )
        max_channel_delta = max(max_channel_delta, tile.max_channel_delta)
        if tile.matched_ratio + 1e-12 < 0.95:
            frame_failures.append(tile)

    if frame_failures:
        failed_frames += 1
        failed_tiles += len(frame_failures)
        for tile in frame_failures[:20]:
            print(
                f"frame={index} baseline_frame_index={baseline_frame.frame_index} "
                f"candidate_frame_index={candidate_frame.frame_index} "
                f"tile=({tile.x},{tile.y}) size={tile.width}x{tile.height} "
                f"matched_ratio={tile.matched_ratio:.4f} max_channel_delta={tile.max_channel_delta}"
            )

print("api=d3d12-to-metal")
print(f"frames_compared={len(baseline_frames)}")
print(f"failed_frames={failed_frames}")
print(f"failed_tiles={failed_tiles}")
print("tile_size=100")
print("tile_pixel_threshold=0.95")
print(f"max_channel_delta={max_channel_delta}")
raise SystemExit(1 if failed_frames else 0)
PY
    grep -F "failed_frames=0" "$METAL_COMPARE_LOG" >/dev/null || fail "d3d12-to-metal compare failed"
}

check_metal_link_coverage() {
    python3 - "$LINK_TRACE/analysis/translation-links.jsonl" <<'PY'
import json
import pathlib
import sys

required_scope_kinds = {"encoder", "draw_to_metal_calls"}


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


path = pathlib.Path(sys.argv[1])
if not path.is_file():
    fail(f"missing translation-links file: {path}")

coverage = {}
with path.open("r", encoding="utf-8") as stream:
    for line_number, line in enumerate(stream, 1):
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            fail(f"{path}:{line_number}: invalid JSON: {exc}")
        if record.get("record_type") != "scope":
            continue
        d3d_sequence = int(record.get("d3d_sequence", 0))
        if d3d_sequence == 0:
            continue
        scope_kind = str(record.get("scope_kind", ""))
        coverage.setdefault(d3d_sequence, set()).add(scope_kind)

if not coverage:
    fail(f"{path}: no non-zero d3d_sequence scope records found")

missing = []
for d3d_sequence in sorted(coverage):
    required_missing = sorted(required_scope_kinds - coverage[d3d_sequence])
    if required_missing:
        missing.append(f"{d3d_sequence}:{','.join(required_missing)}")

if missing:
    print("missing_link_coverage=" + ";".join(missing))
    raise SystemExit(1)

print(f"d3d_sequences={len(coverage)}")
print("coverage=PASS")
PY
}

[ -n "$WINE_BIN" ] && [ -x "$WINE_BIN" ] || fail "missing wine binary"
require_file "$NATIVE_RETRACE"
mkdir -p "$LINK_DIR"

stage_dxmt_runtime
prepare_demo_runtime
trace_d3d12_to_metal
check_metal_link_coverage
retrace_metal
compare_metal

echo "test-d3d-metal-link PASS"
