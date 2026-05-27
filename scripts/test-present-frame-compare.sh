#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS_BASELINE="$TMP_DIR/pass-baseline.apitrace"
PASS_CANDIDATE="$TMP_DIR/pass-candidate.apitrace"
FAIL_BASELINE="$TMP_DIR/fail-baseline.apitrace"
FAIL_CANDIDATE="$TMP_DIR/fail-candidate.apitrace"
DIFF_DIR="$TMP_DIR/diff"
PASS_LOG="$TMP_DIR/pass.log"
FAIL_LOG="$TMP_DIR/fail.log"

python3 - "$PASS_BASELINE" "$PASS_CANDIDATE" "$FAIL_BASELINE" "$FAIL_CANDIDATE" <<'PY'
import json
import pathlib
import sys


def write_bundle(bundle: pathlib.Path, debug_name: str, pixel_format: str, width: int, height: int, rgba: bytes) -> None:
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / "textures").mkdir(exist_ok=True)
    (bundle / "objects").mkdir(exist_ok=True)
    frame_path = pathlib.Path("textures/frame-0001.bin")
    payload = rgba
    if pixel_format == "bgra8":
        converted = bytearray(len(rgba))
        for offset in range(0, len(rgba), 4):
            r, g, b, a = rgba[offset : offset + 4]
            converted[offset + 0] = b
            converted[offset + 1] = g
            converted[offset + 2] = r
            converted[offset + 3] = a
        payload = bytes(converted)
    (bundle / frame_path).write_bytes(payload)
    (bundle / "callstream.jsonl").write_text(
        json.dumps(
            {
                "record_kind": "bundle_header",
                "format_version": 1,
                "api": "Unknown",
                "producer": "present_frame_compare_selftest",
                "has_metal_callstream": debug_name == "MetalPresentFrame",
                "entry_file": "callstream.jsonl",
            }
        )
        + "\n"
        + json.dumps(
            {
                "record_kind": "resource_blob",
                "sequence": 1,
                "object_id": 0,
                "object_kind": "Unknown",
                "parent_object_id": 0,
                "debug_name": debug_name,
                "object_refs": [],
                "blob_refs": [1],
                "payload": {
                    "frame_index": 1,
                    "width": width,
                    "height": height,
                    "row_pitch": width * 4,
                    "sync_interval": 1,
                    "flags": 0,
                    "format": pixel_format,
                    "frame_path": frame_path.as_posix(),
                },
            }
        )
        + "\n",
        encoding="utf-8",
    )
    (bundle / "checksums.json").write_text(
        json.dumps({"format_version": 1, "bundle_hash": "", "files": {}}) + "\n",
        encoding="utf-8",
    )
    (bundle / "objects" / "objects.json").write_text("[]\n", encoding="utf-8")


def make_rgba(width: int, height: int, fill: tuple[int, int, int, int]) -> bytearray:
    rgba = bytearray(width * height * 4)
    for offset in range(0, len(rgba), 4):
        rgba[offset : offset + 4] = bytes(fill)
    return rgba


pass_baseline = pathlib.Path(sys.argv[1])
pass_candidate = pathlib.Path(sys.argv[2])
fail_baseline = pathlib.Path(sys.argv[3])
fail_candidate = pathlib.Path(sys.argv[4])

pass_rgba = make_rgba(200, 100, (12, 34, 56, 255))
write_bundle(pass_baseline, "D3D11PresentFrame", "rgba8", 200, 100, bytes(pass_rgba))
write_bundle(pass_candidate, "D3D11PresentFrame", "rgba8", 200, 100, bytes(pass_rgba))

fail_rgba = make_rgba(150, 150, (20, 40, 60, 255))
fail_candidate_rgba = bytearray(fail_rgba)
for row in range(130, 150):
    for column in range(100, 107):
        offset = (row * 150 + column) * 4
        fail_candidate_rgba[offset : offset + 4] = bytes((220, 10, 30, 255))
write_bundle(fail_baseline, "MetalPresentFrame", "bgra8", 150, 150, bytes(fail_rgba))
write_bundle(fail_candidate, "MetalPresentFrame", "bgra8", 150, 150, bytes(fail_candidate_rgba))
PY

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --baseline "$PASS_BASELINE" \
  --candidate "$PASS_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$PASS_LOG"

grep -F "api=d3d11" "$PASS_LOG" >/dev/null
grep -F "failed_frames=0" "$PASS_LOG" >/dev/null
grep -F "failed_tiles=0" "$PASS_LOG" >/dev/null

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api metal \
  --baseline "$FAIL_BASELINE" \
  --candidate "$FAIL_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 \
  --write-diff "$DIFF_DIR" >"$FAIL_LOG"; then
  echo "expected compare failure for mismatched partial tile" >&2
  exit 1
fi

grep -F "failed_frames=1" "$FAIL_LOG" >/dev/null
grep -F "failed_tiles=1" "$FAIL_LOG" >/dev/null
grep -F "tile=(100,100)" "$FAIL_LOG" >/dev/null
test -f "$DIFF_DIR/frame-000000.png"

echo "present_frame_compare selftest PASS"
