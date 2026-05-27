#!/usr/bin/env python3
"""Compare MetalPresentFrame debug assets from two trace bundles."""

from __future__ import annotations

import argparse
import binascii
import json
import pathlib
import struct
import sys
import zlib


class Frame:
    def __init__(
        self,
        index: int,
        width: int,
        height: int,
        row_pitch: int,
        sync_interval: int,
        flags: int,
        pixel_format: str,
        path: pathlib.Path,
    ) -> None:
        self.index = index
        self.width = width
        self.height = height
        self.row_pitch = row_pitch
        self.sync_interval = sync_interval
        self.flags = flags
        self.pixel_format = pixel_format
        self.path = path

    @property
    def payload_size(self) -> int:
        return self.row_pitch * self.height


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def normalize_format(name: str) -> str:
    lowered = name.lower()
    if lowered in {"rgba8", "rgba8unorm"}:
        return "rgba8"
    if lowered in {"bgra8", "bgra8unorm"}:
        return "bgra8"
    fail(f"unsupported frame format {name!r}")


def load_frames(bundle: pathlib.Path) -> dict[int, Frame]:
    callstream = bundle / "callstream.jsonl"
    if not callstream.is_file():
        fail(f"missing callstream.jsonl in {bundle}")

    frames: dict[int, Frame] = {}
    with callstream.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                fail(f"{callstream}:{line_number}: invalid JSON: {exc}")
            if record.get("record_kind") != "resource_blob":
                continue
            if record.get("debug_name") != "MetalPresentFrame":
                continue
            payload = record.get("payload")
            if not isinstance(payload, dict):
                fail(f"{callstream}:{line_number}: MetalPresentFrame missing payload")
            try:
                frame = Frame(
                    index=int(payload["frame_index"]),
                    width=int(payload["width"]),
                    height=int(payload["height"]),
                    row_pitch=int(payload["row_pitch"]),
                    sync_interval=int(payload["sync_interval"]),
                    flags=int(payload["flags"]),
                    pixel_format=normalize_format(str(payload["format"])),
                    path=pathlib.Path(str(payload["frame_path"])),
                )
            except KeyError as exc:
                fail(f"{callstream}:{line_number}: MetalPresentFrame missing {exc.args[0]}")
            if frame.width <= 0 or frame.height <= 0 or frame.row_pitch < frame.width * 4:
                fail(f"{callstream}:{line_number}: invalid frame dimensions")
            if frame.index in frames:
                fail(f"{callstream}:{line_number}: duplicate frame_index {frame.index}")
            absolute = bundle / frame.path
            if not absolute.is_file():
                fail(f"{callstream}:{line_number}: missing frame asset {frame.path}")
            actual_size = absolute.stat().st_size
            if actual_size != frame.payload_size:
                fail(
                    f"{callstream}:{line_number}: frame asset size {actual_size} "
                    f"does not match row_pitch * height {frame.payload_size}"
                )
            frames[frame.index] = frame
    return frames


def read_frame_pixels(bundle: pathlib.Path, frame: Frame) -> bytes:
    data = (bundle / frame.path).read_bytes()
    row_bytes = frame.width * 4
    if frame.row_pitch == row_bytes:
        packed = data
    else:
        rows = []
        for row in range(frame.height):
            begin = row * frame.row_pitch
            rows.append(data[begin : begin + row_bytes])
        packed = b"".join(rows)

    if frame.pixel_format == "rgba8":
        return packed

    rgba = bytearray(len(packed))
    for offset in range(0, len(packed), 4):
        b, g, r, a = packed[offset : offset + 4]
        rgba[offset + 0] = r
        rgba[offset + 1] = g
        rgba[offset + 2] = b
        rgba[offset + 3] = a
    return bytes(rgba)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    crc = binascii.crc32(kind)
    crc = binascii.crc32(data, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def write_rgba_png(path: pathlib.Path, width: int, height: int, rgba: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    row_bytes = width * 4
    raw = bytearray()
    for row in range(height):
        raw.append(0)
        begin = row * row_bytes
        raw.extend(rgba[begin : begin + row_bytes])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    payload = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(payload)


def compare_pixels(expected: bytes, actual: bytes, tolerance: int) -> tuple[int, int, int, bytes]:
    if len(expected) != len(actual):
        fail("internal error: pixel buffer sizes differ")

    mismatched_pixels = 0
    max_channel_delta = 0
    total_channel_delta = 0
    diff = bytearray(len(expected))
    for offset in range(0, len(expected), 4):
        pixel_mismatch = False
        pixel_max = 0
        for channel in range(4):
            delta = abs(expected[offset + channel] - actual[offset + channel])
            max_channel_delta = max(max_channel_delta, delta)
            pixel_max = max(pixel_max, delta)
            total_channel_delta += delta
            if delta > tolerance:
                pixel_mismatch = True
        if pixel_mismatch:
            mismatched_pixels += 1
            diff[offset + 0] = pixel_max
            diff[offset + 1] = 0
            diff[offset + 2] = 255 - pixel_max
            diff[offset + 3] = 255
        else:
            diff[offset + 3] = 255
    return mismatched_pixels, max_channel_delta, total_channel_delta, bytes(diff)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare MetalPresentFrame debug assets from two apitrace bundles."
    )
    parser.add_argument("expected_bundle", type=pathlib.Path)
    parser.add_argument("actual_bundle", type=pathlib.Path)
    parser.add_argument("--tolerance", type=int, default=0, help="per-channel tolerance, default 0")
    parser.add_argument("--write-preview", type=pathlib.Path, help="write expected/actual PNG previews")
    parser.add_argument("--write-diff", type=pathlib.Path, help="write PNG diff heatmaps for mismatched frames")
    parser.add_argument("--max-report", type=int, default=20, help="maximum mismatched frame indices to print")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.tolerance < 0 or args.tolerance > 255:
        fail("--tolerance must be in [0, 255]")

    expected_frames = load_frames(args.expected_bundle)
    actual_frames = load_frames(args.actual_bundle)
    if not expected_frames:
        fail(f"{args.expected_bundle} has no MetalPresentFrame assets")
    if not actual_frames:
        fail(f"{args.actual_bundle} has no MetalPresentFrame assets")

    expected_indices = sorted(expected_frames)
    actual_indices = sorted(actual_frames)
    if expected_indices != actual_indices:
        missing = sorted(set(expected_indices) - set(actual_indices))
        extra = sorted(set(actual_indices) - set(expected_indices))
        fail(f"frame index sets differ; missing={missing[:20]} extra={extra[:20]}")

    mismatched: list[int] = []
    total_mismatched_pixels = 0
    max_channel_delta = 0
    total_channel_delta = 0
    total_pixels = 0

    for index in expected_indices:
        expected = expected_frames[index]
        actual = actual_frames[index]
        if (expected.width, expected.height) != (actual.width, actual.height):
            fail(
                f"frame {index}: dimensions differ "
                f"{expected.width}x{expected.height} != {actual.width}x{actual.height}"
            )
        if expected.sync_interval != actual.sync_interval or expected.flags != actual.flags:
            fail(
                f"frame {index}: present parameters differ "
                f"sync/flags {expected.sync_interval}/{expected.flags} != "
                f"{actual.sync_interval}/{actual.flags}"
            )

        expected_rgba = read_frame_pixels(args.expected_bundle, expected)
        actual_rgba = read_frame_pixels(args.actual_bundle, actual)
        if args.write_preview:
            write_rgba_png(
                args.write_preview / "expected" / f"frame-{index:06d}.png",
                expected.width,
                expected.height,
                expected_rgba,
            )
            write_rgba_png(
                args.write_preview / "actual" / f"frame-{index:06d}.png",
                actual.width,
                actual.height,
                actual_rgba,
            )

        frame_mismatched_pixels, frame_max_delta, frame_total_delta, diff = compare_pixels(
            expected_rgba,
            actual_rgba,
            args.tolerance,
        )
        total_pixels += expected.width * expected.height
        total_mismatched_pixels += frame_mismatched_pixels
        max_channel_delta = max(max_channel_delta, frame_max_delta)
        total_channel_delta += frame_total_delta
        if frame_mismatched_pixels:
            mismatched.append(index)
            if args.write_diff:
                write_rgba_png(
                    args.write_diff / f"frame-{index:06d}.png",
                    expected.width,
                    expected.height,
                    diff,
                )

    print(f"frames_compared={len(expected_indices)}")
    print(f"mismatched_frames={len(mismatched)}")
    print(f"mismatched_pixels={total_mismatched_pixels}")
    print(f"total_pixels={total_pixels}")
    print(f"max_channel_delta={max_channel_delta}")
    print(f"total_channel_delta={total_channel_delta}")
    if mismatched:
        shown = mismatched[: args.max_report]
        print("mismatched_frame_indices=" + ",".join(str(index) for index in shown))
        if len(shown) != len(mismatched):
            print(f"mismatched_frame_indices_truncated={len(mismatched) - len(shown)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
