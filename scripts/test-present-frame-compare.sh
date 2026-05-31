#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS_BASELINE="$TMP_DIR/pass-baseline.apitrace"
PASS_CANDIDATE="$TMP_DIR/pass-candidate.apitrace"
FAIL_BASELINE="$TMP_DIR/fail-baseline.apitrace"
FAIL_CANDIDATE="$TMP_DIR/fail-candidate.apitrace"
INDEX_BASELINE="$TMP_DIR/index-baseline.apitrace"
INDEX_CANDIDATE="$TMP_DIR/index-candidate.apitrace"
SEQUENCE_BASELINE="$TMP_DIR/sequence-baseline.apitrace"
SEQUENCE_CANDIDATE="$TMP_DIR/sequence-candidate.apitrace"
PITCH_BASELINE="$TMP_DIR/pitch-baseline.apitrace"
PITCH_CANDIDATE="$TMP_DIR/pitch-candidate.apitrace"
CROSS_BASELINE="$TMP_DIR/cross-baseline.apitrace"
CROSS_CANDIDATE="$TMP_DIR/cross-candidate.apitrace"
CROSS_FAIL_CANDIDATE="$TMP_DIR/cross-fail-candidate.apitrace"
POISON_CANDIDATE="$TMP_DIR/poison-candidate.apitrace"
D3D12_POISON_CANDIDATE="$TMP_DIR/d3d12-poison-candidate.apitrace"
METAL_POISON_CANDIDATE="$TMP_DIR/metal-poison-candidate.apitrace"
DIFF_DIR="$TMP_DIR/diff"
PASS_LOG="$TMP_DIR/pass.log"
FAIL_LOG="$TMP_DIR/fail.log"
INDEX_LOG="$TMP_DIR/index.log"
SEQUENCE_LOG="$TMP_DIR/sequence.log"
PITCH_LOG="$TMP_DIR/pitch.log"
CROSS_LOG="$TMP_DIR/cross.log"
CROSS_FAIL_LOG="$TMP_DIR/cross-fail.log"
POISON_LOG="$TMP_DIR/poison.log"
D3D12_POISON_LOG="$TMP_DIR/d3d12-poison.log"
METAL_POISON_LOG="$TMP_DIR/metal-poison.log"

python3 - "$PASS_BASELINE" "$PASS_CANDIDATE" "$FAIL_BASELINE" "$FAIL_CANDIDATE" "$INDEX_BASELINE" "$INDEX_CANDIDATE" "$SEQUENCE_BASELINE" "$SEQUENCE_CANDIDATE" "$PITCH_BASELINE" "$PITCH_CANDIDATE" "$CROSS_BASELINE" "$CROSS_CANDIDATE" "$CROSS_FAIL_CANDIDATE" <<'PY'
import json
import pathlib
import sys


def present_record(debug_name: str, width: int, height: int, pixel_format: str, frame_path: pathlib.Path, frame_index: int, sequence: int, row_pitch: int | None = None) -> dict:
    return {
        "record_kind": "resource_blob",
        "sequence": sequence,
        "object_id": 0,
        "object_kind": "Unknown",
        "parent_object_id": 0,
        "debug_name": debug_name,
        "object_refs": [],
        "blob_refs": [sequence],
        "payload": {
            "frame_index": frame_index,
            "width": width,
            "height": height,
            "row_pitch": row_pitch if row_pitch is not None else width * 4,
            "sync_interval": 1,
            "flags": 0,
            "format": pixel_format,
            "frame_path": frame_path.as_posix(),
        },
    }


def write_bundle(bundle: pathlib.Path, debug_name: str, pixel_format: str, width: int, height: int, rgba: bytes, frame_index: int = 1) -> None:
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
    write_callstream(bundle, debug_name, [present_record(debug_name, width, height, pixel_format, frame_path, frame_index, 1)])


def write_callstream(bundle: pathlib.Path, debug_name: str, records: list[dict]) -> None:
    bundle.mkdir(parents=True, exist_ok=True)
    (bundle / "objects").mkdir(exist_ok=True)
    header = {
        "record_kind": "bundle_header",
        "format_version": 1,
        "api": "Unknown",
        "producer": "present_frame_compare_selftest",
        "has_metal_callstream": debug_name == "MetalPresentFrame",
        "entry_file": "callstream.jsonl",
    }
    (bundle / "callstream.jsonl").write_text(
        "\n".join(json.dumps(record) for record in [header, *records]) + "\n",
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
index_baseline = pathlib.Path(sys.argv[5])
index_candidate = pathlib.Path(sys.argv[6])
sequence_baseline = pathlib.Path(sys.argv[7])
sequence_candidate = pathlib.Path(sys.argv[8])
pitch_baseline = pathlib.Path(sys.argv[9])
pitch_candidate = pathlib.Path(sys.argv[10])
cross_baseline = pathlib.Path(sys.argv[11])
cross_candidate = pathlib.Path(sys.argv[12])
cross_fail_candidate = pathlib.Path(sys.argv[13])

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

index_rgba = make_rgba(64, 64, (90, 80, 70, 255))
write_bundle(index_baseline, "D3D12PresentFrame", "rgba8", 64, 64, bytes(index_rgba), frame_index=4)
write_bundle(index_candidate, "D3D12PresentFrame", "rgba8", 64, 64, bytes(index_rgba), frame_index=5)

sequence_rgba = make_rgba(32, 32, (11, 22, 33, 255))
write_bundle(sequence_candidate, "D3D11PresentFrame", "rgba8", 32, 32, bytes(sequence_rgba), frame_index=1)
sequence_baseline.mkdir(parents=True, exist_ok=True)
(sequence_baseline / "textures").mkdir(exist_ok=True)
frame_a = pathlib.Path("textures/frame-a.bin")
frame_b = pathlib.Path("textures/frame-b.bin")
(sequence_baseline / frame_a).write_bytes(bytes(sequence_rgba))
(sequence_baseline / frame_b).write_bytes(bytes(sequence_rgba))
write_callstream(
    sequence_baseline,
    "D3D11PresentFrame",
    [
        present_record("D3D11PresentFrame", 32, 32, "rgba8", frame_a, 2, 1),
        present_record("D3D11PresentFrame", 32, 32, "rgba8", frame_b, 2, 2),
    ],
)

pitch_rgba = make_rgba(5, 3, (101, 102, 103, 255))
write_bundle(pitch_baseline, "D3D12PresentFrame", "rgba8", 5, 3, bytes(pitch_rgba), frame_index=6)
pitch_candidate.mkdir(parents=True, exist_ok=True)
(pitch_candidate / "textures").mkdir(exist_ok=True)
pitch_frame = pathlib.Path("textures/frame-padded.bin")
padded_rows = []
for row in range(3):
    row_begin = row * 5 * 4
    padded_rows.append(bytes(pitch_rgba[row_begin : row_begin + 5 * 4]) + b"\x00" * 12)
(pitch_candidate / pitch_frame).write_bytes(b"".join(padded_rows))
write_callstream(
    pitch_candidate,
    "D3D12PresentFrame",
    [present_record("D3D12PresentFrame", 5, 3, "rgba8", pitch_frame, 6, 1, row_pitch=32)],
)

cross_rgba = make_rgba(160, 120, (44, 55, 66, 255))
write_bundle(cross_baseline, "D3D12PresentFrame", "rgba8", 160, 120, bytes(cross_rgba), frame_index=7)
write_bundle(cross_candidate, "MetalPresentFrame", "bgra8", 160, 120, bytes(cross_rgba), frame_index=7)
cross_fail_rgba = bytearray(cross_rgba)
for row in range(0, 120):
    for column in range(100, 160):
        offset = (row * 160 + column) * 4
        cross_fail_rgba[offset : offset + 4] = bytes((200, 10, 80, 255))
write_bundle(cross_fail_candidate, "MetalPresentFrame", "bgra8", 160, 120, bytes(cross_fail_rgba), frame_index=7)
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

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$INDEX_BASELINE" \
  --candidate "$INDEX_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$INDEX_LOG" 2>&1; then
  echo "expected compare failure for mismatched frame_index" >&2
  exit 1
fi

grep -F "frame_index differs baseline=4 candidate=5" "$INDEX_LOG" >/dev/null

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d11 \
  --baseline "$SEQUENCE_BASELINE" \
  --candidate "$SEQUENCE_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$SEQUENCE_LOG" 2>&1; then
  echo "expected compare failure for non-increasing frame_index" >&2
  exit 1
fi

grep -F "frame_index is not strictly increasing (2 after 2)" "$SEQUENCE_LOG" >/dev/null

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$PITCH_BASELINE" \
  --candidate "$PITCH_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$PITCH_LOG"

grep -F "failed_frames=0" "$PITCH_LOG" >/dev/null
grep -F "failed_tiles=0" "$PITCH_LOG" >/dev/null

python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --baseline-api d3d12 \
  --candidate-api metal \
  --baseline "$CROSS_BASELINE" \
  --candidate "$CROSS_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$CROSS_LOG"

grep -F "api=d3d12-to-metal" "$CROSS_LOG" >/dev/null
grep -F "failed_frames=0" "$CROSS_LOG" >/dev/null
grep -F "failed_tiles=0" "$CROSS_LOG" >/dev/null

python3 "$ROOT_DIR/scripts/lib/poison_present_frame.py" \
  --api d3d11 \
  --source "$PASS_BASELINE" \
  --output "$POISON_CANDIDATE" \
  --frame-index 1 >/dev/null

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d11 \
  --baseline "$PASS_BASELINE" \
  --candidate "$POISON_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$POISON_LOG"; then
  echo "expected compare failure for poisoned present frame" >&2
  exit 1
fi
grep -F "failed_frames=1" "$POISON_LOG" >/dev/null
grep -F "failed_tiles=2" "$POISON_LOG" >/dev/null

python3 "$ROOT_DIR/scripts/lib/poison_present_frame.py" \
  --api d3d12 \
  --source "$PITCH_BASELINE" \
  --output "$D3D12_POISON_CANDIDATE" \
  --frame-index 6 >/dev/null

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api d3d12 \
  --baseline "$PITCH_BASELINE" \
  --candidate "$D3D12_POISON_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$D3D12_POISON_LOG"; then
  echo "expected compare failure for poisoned d3d12 present frame" >&2
  exit 1
fi
grep -F "failed_frames=1" "$D3D12_POISON_LOG" >/dev/null
grep -F "failed_tiles=1" "$D3D12_POISON_LOG" >/dev/null

python3 "$ROOT_DIR/scripts/lib/poison_present_frame.py" \
  --api metal \
  --source "$CROSS_CANDIDATE" \
  --output "$METAL_POISON_CANDIDATE" \
  --frame-index 7 >/dev/null

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --api metal \
  --baseline "$CROSS_CANDIDATE" \
  --candidate "$METAL_POISON_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$METAL_POISON_LOG"; then
  echo "expected compare failure for poisoned metal present frame" >&2
  exit 1
fi
grep -F "failed_frames=1" "$METAL_POISON_LOG" >/dev/null
grep -F "failed_tiles=4" "$METAL_POISON_LOG" >/dev/null

if python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
  --baseline-api d3d12 \
  --candidate-api metal \
  --baseline "$CROSS_BASELINE" \
  --candidate "$CROSS_FAIL_CANDIDATE" \
  --tile 100 \
  --tile-pixel-threshold 0.95 >"$CROSS_FAIL_LOG"; then
  echo "expected compare failure for d3d12-to-metal mismatched tile" >&2
  exit 1
fi

grep -F "api=d3d12-to-metal" "$CROSS_FAIL_LOG" >/dev/null
grep -F "failed_frames=1" "$CROSS_FAIL_LOG" >/dev/null
grep -F "failed_tiles=2" "$CROSS_FAIL_LOG" >/dev/null

echo "present_frame_compare selftest PASS"
