#!/usr/bin/env python3
"""Compare PresentFrame assets from two trace bundles using fixed-size tiles."""

from __future__ import annotations

import argparse
import binascii
import json
import math
import pathlib
import struct
import sys
import zlib


DEBUG_NAME_BY_API = {
    "d3d11": "D3D11PresentFrame",
    "d3d12": "D3D12PresentFrame",
    "metal": "MetalPresentFrame",
}

FORMAT_BY_API = {
    "d3d11": {"rgba8", "rgba8unorm"},
    "d3d12": {"rgba8", "rgba8unorm"},
    "metal": {"bgra8", "bgra8unorm", "rgba8", "rgba8unorm"},
}


class Frame:
    def __init__(
        self,
        debug_name: str,
        occurrence_index: int,
        frame_index: int,
        width: int,
        height: int,
        row_pitch: int,
        pixel_format: str,
        path: pathlib.Path,
    ) -> None:
        self.debug_name = debug_name
        self.occurrence_index = occurrence_index
        self.frame_index = frame_index
        self.width = width
        self.height = height
        self.row_pitch = row_pitch
        self.pixel_format = pixel_format
        self.path = path

    @property
    def payload_size(self) -> int:
        return self.row_pitch * self.height


class TileMismatch:
    def __init__(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        mismatched_pixels: int,
        total_pixels: int,
        max_channel_delta: int,
    ) -> None:
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self.mismatched_pixels = mismatched_pixels
        self.total_pixels = total_pixels
        self.max_channel_delta = max_channel_delta

    @property
    def matched_pixels(self) -> int:
        return self.total_pixels - self.mismatched_pixels

    @property
    def matched_ratio(self) -> float:
        return self.matched_pixels / self.total_pixels


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


def detect_api(bundle: pathlib.Path) -> str:
    callstream = bundle / "callstream.jsonl"
    if not callstream.is_file():
        fail(f"missing callstream.jsonl in {bundle}")

    seen: set[str] = set()
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
            debug_name = str(record.get("debug_name", ""))
            for api, expected_name in DEBUG_NAME_BY_API.items():
                if debug_name == expected_name:
                    seen.add(api)

    if len(seen) == 1:
        return next(iter(seen))
    if not seen:
        fail(f"{bundle} has no PresentFrame resource blobs")
    fail(f"{bundle} has multiple PresentFrame kinds; pass --api explicitly")


def load_frames(bundle: pathlib.Path, api: str) -> list[Frame]:
    callstream = bundle / "callstream.jsonl"
    if not callstream.is_file():
        fail(f"missing callstream.jsonl in {bundle}")

    debug_name = DEBUG_NAME_BY_API[api]
    allowed_formats = FORMAT_BY_API[api]
    frames: list[Frame] = []
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
            if record.get("debug_name") != debug_name:
                continue
            payload = record.get("payload")
            if not isinstance(payload, dict):
                fail(f"{callstream}:{line_number}: {debug_name} missing payload")
            try:
                pixel_format = normalize_format(str(payload["format"]))
                if str(payload["format"]).lower() not in allowed_formats:
                    fail(f"{callstream}:{line_number}: unsupported {debug_name} format {payload['format']!r}")
                frame = Frame(
                    debug_name=debug_name,
                    occurrence_index=len(frames),
                    frame_index=int(payload["frame_index"]),
                    width=int(payload["width"]),
                    height=int(payload["height"]),
                    row_pitch=int(payload["row_pitch"]),
                    pixel_format=pixel_format,
                    path=pathlib.Path(str(payload["frame_path"])),
                )
            except KeyError as exc:
                fail(f"{callstream}:{line_number}: {debug_name} missing {exc.args[0]}")

            if frame.width <= 0 or frame.height <= 0 or frame.row_pitch < frame.width * 4:
                fail(f"{callstream}:{line_number}: invalid frame dimensions")

            absolute = bundle / frame.path
            if not absolute.is_file():
                fail(f"{callstream}:{line_number}: missing frame asset {frame.path}")
            if absolute.stat().st_size != frame.payload_size:
                fail(
                    f"{callstream}:{line_number}: frame asset size {absolute.stat().st_size} "
                    f"does not match row_pitch * height {frame.payload_size}"
                )
            frames.append(frame)
    return frames


def read_frame_rgba(bundle: pathlib.Path, frame: Frame) -> bytes:
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


def iter_tiles(width: int, height: int, tile_size: int) -> list[tuple[int, int, int, int]]:
    tiles: list[tuple[int, int, int, int]] = []
    for y in range(0, height, tile_size):
        for x in range(0, width, tile_size):
            tiles.append((x, y, min(tile_size, width - x), min(tile_size, height - y)))
    return tiles


def compare_tile(
    baseline: bytes,
    candidate: bytes,
    frame_width: int,
    tile_x: int,
    tile_y: int,
    tile_width: int,
    tile_height: int,
) -> TileMismatch:
    mismatched_pixels = 0
    max_channel_delta = 0
    row_stride = frame_width * 4
    for row in range(tile_height):
        row_base = (tile_y + row) * row_stride + tile_x * 4
        for column in range(tile_width):
            pixel_base = row_base + column * 4
            pixel_mismatch = False
            pixel_max_delta = 0
            for channel in range(4):
                delta = abs(baseline[pixel_base + channel] - candidate[pixel_base + channel])
                pixel_max_delta = max(pixel_max_delta, delta)
                max_channel_delta = max(max_channel_delta, delta)
                if delta != 0:
                    pixel_mismatch = True
            if pixel_mismatch:
                mismatched_pixels += 1
    return TileMismatch(
        x=tile_x,
        y=tile_y,
        width=tile_width,
        height=tile_height,
        mismatched_pixels=mismatched_pixels,
        total_pixels=tile_width * tile_height,
        max_channel_delta=max_channel_delta,
    )


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


def build_tile_mask(
    frame_width: int,
    frame_height: int,
    failed_tiles: list[TileMismatch],
) -> bytes:
    rgba = bytearray(frame_width * frame_height * 4)
    row_stride = frame_width * 4
    for tile in failed_tiles:
        for row in range(tile.height):
            row_base = (tile.y + row) * row_stride + tile.x * 4
            for column in range(tile.width):
                pixel_base = row_base + column * 4
                rgba[pixel_base + 0] = 255
                rgba[pixel_base + 1] = 0
                rgba[pixel_base + 2] = 0
                rgba[pixel_base + 3] = 255
    return bytes(rgba)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare PresentFrame assets from two trace bundles.")
    parser.add_argument("--baseline", required=True, type=pathlib.Path)
    parser.add_argument("--candidate", required=True, type=pathlib.Path)
    parser.add_argument("--api", choices=sorted(DEBUG_NAME_BY_API))
    parser.add_argument("--tile", type=int, default=100, help="tile size and stride, default 100")
    parser.add_argument(
        "--tile-pixel-threshold",
        type=float,
        default=0.95,
        help="minimum per-tile matched pixel ratio in [0, 1], default 0.95",
    )
    parser.add_argument("--write-diff", type=pathlib.Path, help="write failed-tile mask PNGs")
    parser.add_argument("--max-report", type=int, default=20, help="maximum failed tiles to print")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.tile <= 0:
        fail("--tile must be positive")
    if not math.isfinite(args.tile_pixel_threshold) or args.tile_pixel_threshold <= 0.0 or args.tile_pixel_threshold > 1.0:
        fail("--tile-pixel-threshold must be in (0, 1]")
    if args.max_report <= 0:
        fail("--max-report must be positive")

    baseline_api = args.api or detect_api(args.baseline)
    candidate_api = args.api or detect_api(args.candidate)
    if baseline_api != candidate_api:
        fail(
            f"baseline and candidate PresentFrame kinds differ "
            f"({baseline_api} != {candidate_api}); pass --api explicitly if this is intentional"
        )
    api = baseline_api

    baseline_frames = load_frames(args.baseline, api)
    candidate_frames = load_frames(args.candidate, api)
    if not baseline_frames:
        fail(f"{args.baseline} has no {DEBUG_NAME_BY_API[api]} assets")
    if not candidate_frames:
        fail(f"{args.candidate} has no {DEBUG_NAME_BY_API[api]} assets")
    if len(baseline_frames) != len(candidate_frames):
        fail(f"frame counts differ: baseline={len(baseline_frames)} candidate={len(candidate_frames)}")

    failed_tiles_total = 0
    max_channel_delta = 0
    reports: list[str] = []
    failed_frames = 0

    for index, (baseline_frame, candidate_frame) in enumerate(zip(baseline_frames, candidate_frames)):
        if (
            baseline_frame.width != candidate_frame.width
            or baseline_frame.height != candidate_frame.height
            or baseline_frame.row_pitch != candidate_frame.row_pitch
            or baseline_frame.pixel_format != candidate_frame.pixel_format
        ):
            fail(
                f"frame {index}: metadata differs "
                f"baseline=({baseline_frame.width}x{baseline_frame.height}, row_pitch={baseline_frame.row_pitch}, format={baseline_frame.pixel_format}) "
                f"candidate=({candidate_frame.width}x{candidate_frame.height}, row_pitch={candidate_frame.row_pitch}, format={candidate_frame.pixel_format})"
            )

        baseline_pixels = read_frame_rgba(args.baseline, baseline_frame)
        candidate_pixels = read_frame_rgba(args.candidate, candidate_frame)
        frame_failures: list[TileMismatch] = []
        for tile_x, tile_y, tile_width, tile_height in iter_tiles(
            baseline_frame.width,
            baseline_frame.height,
            args.tile,
        ):
            tile = compare_tile(
                baseline_pixels,
                candidate_pixels,
                baseline_frame.width,
                tile_x,
                tile_y,
                tile_width,
                tile_height,
            )
            max_channel_delta = max(max_channel_delta, tile.max_channel_delta)
            if tile.matched_ratio + 1e-12 < args.tile_pixel_threshold:
                frame_failures.append(tile)

        if frame_failures:
            failed_frames += 1
            failed_tiles_total += len(frame_failures)
            for tile in frame_failures:
                if len(reports) < args.max_report:
                    reports.append(
                        "frame="
                        f"{index} baseline_frame_index={baseline_frame.frame_index} candidate_frame_index={candidate_frame.frame_index} "
                        f"tile=({tile.x},{tile.y}) size={tile.width}x{tile.height} "
                        f"mismatched_pixels={tile.mismatched_pixels} total_pixels={tile.total_pixels} "
                        f"matched_ratio={tile.matched_ratio:.4f} max_channel_delta={tile.max_channel_delta}"
                    )
            if args.write_diff:
                write_rgba_png(
                    args.write_diff / f"frame-{index:06d}.png",
                    baseline_frame.width,
                    baseline_frame.height,
                    build_tile_mask(baseline_frame.width, baseline_frame.height, frame_failures),
                )

    print(f"api={api}")
    print(f"frames_compared={len(baseline_frames)}")
    print(f"failed_frames={failed_frames}")
    print(f"failed_tiles={failed_tiles_total}")
    print(f"tile_size={args.tile}")
    print(f"tile_pixel_threshold={args.tile_pixel_threshold:.2f}")
    print(f"max_channel_delta={max_channel_delta}")
    for report in reports:
        print(report)
    if failed_frames:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
