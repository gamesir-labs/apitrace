#!/usr/bin/env python3
"""Copy a trace bundle and poison one PresentFrame asset for replay tests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
from typing import Any


PRESENT_FRAME_KIND_BY_API = {
    "d3d11": "D3D11PresentFrame",
    "d3d12": "D3D12PresentFrame",
    "metal": "MetalPresentFrame",
}


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_json(path: pathlib.Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"missing file: {path}")
    except json.JSONDecodeError as exc:
        fail(f"invalid JSON in {path}: {exc}")


def write_json(path: pathlib.Path, value: Any) -> None:
    temp_path = path.with_name(path.name + ".tmp")
    temp_path.write_text(json.dumps(value, indent=2, separators=(",", ": ")) + "\n", encoding="utf-8")
    os.replace(temp_path, path)


def copy_or_link(src: str, dst: str) -> str:
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)
    return dst


def normalize_event_payload(event: dict[str, Any]) -> dict[str, Any]:
    payload = event.get("payload", {})
    if isinstance(payload, dict):
        return payload
    if isinstance(payload, str):
        try:
            parsed = json.loads(payload)
        except json.JSONDecodeError:
            return {}
        return parsed if isinstance(parsed, dict) else {}
    return {}


def iter_callstream_events(bundle: pathlib.Path) -> list[dict[str, Any]]:
    callstream_path = bundle / "callstream.jsonl"
    events: list[dict[str, Any]] = []
    try:
        with callstream_path.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, start=1):
                stripped = line.strip()
                if not stripped:
                    continue
                try:
                    event = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    fail(f"{callstream_path}:{line_number}: invalid JSON: {exc}")
                if isinstance(event, dict):
                    events.append(event)
    except FileNotFoundError:
        fail(f"missing file: {callstream_path}")
    return events


def find_present_frame(bundle: pathlib.Path, debug_name: str, frame_index: int | None) -> dict[str, Any]:
    matches: list[dict[str, Any]] = []
    for event in iter_callstream_events(bundle):
        if event.get("record_kind") != "resource_blob" or event.get("debug_name") != debug_name:
            continue
        payload = normalize_event_payload(event)
        if frame_index is not None and payload.get("frame_index") != frame_index:
            continue
        frame_path = payload.get("frame_path")
        if not isinstance(frame_path, str) or not frame_path:
            fail(f"{debug_name} event is missing frame_path")
        matches.append(
            {
                "frame_path": frame_path,
                "blob_refs": event.get("blob_refs", []),
                "payload": payload,
            }
        )

    if not matches:
        frame_label = "" if frame_index is None else f" frame_index={frame_index}"
        fail(f"no {debug_name}{frame_label} event found")
    if frame_index is None and len(matches) != 1:
        fail(f"bundle has {len(matches)} {debug_name} events; pass --frame-index")
    return matches[0]


def validate_relative_path(path_text: str) -> pathlib.Path:
    relative_path = pathlib.PurePosixPath(path_text)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        fail(f"unsafe PresentFrame path: {path_text}")
    return pathlib.Path(*relative_path.parts)


def update_assets_json(bundle: pathlib.Path, frame_path: str, new_digest: str, new_size: int) -> None:
    assets_path = bundle / "assets.json"
    if not assets_path.is_file():
        return
    assets = read_json(assets_path)
    entries = assets.get("assets")
    if not isinstance(entries, list):
        fail(f"{assets_path}: assets must be an array")
    updated = 0
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("path") != frame_path:
            continue
        if "content_hash" in entry:
            entry["content_hash"] = new_digest
        if "byte_size" in entry:
            entry["byte_size"] = new_size
        updated += 1
    if updated == 0:
        fail(f"{assets_path}: no asset entry for {frame_path}")
    write_json(assets_path, assets)


def bundle_hash_from_files(files: dict[str, str]) -> str:
    source = "".join(f"{path}={digest.split(':', 1)[-1]}\n" for path, digest in sorted(files.items()))
    return "sha256:" + sha256_bytes(source.encode("utf-8"))


def update_checksums_json(bundle: pathlib.Path, changed_paths: list[str]) -> None:
    checksums_path = bundle / "checksums.json"
    checksums = read_json(checksums_path)
    files = checksums.get("files")
    if not isinstance(files, dict):
        fail(f"{checksums_path}: files must be an object")

    for changed_path in changed_paths:
        data = (bundle / validate_relative_path(changed_path)).read_bytes()
        files[changed_path] = "sha256:" + sha256_bytes(data)

    checksums["bundle_hash"] = bundle_hash_from_files(files)
    write_json(checksums_path, checksums)


def poison_bytes(original: bytes, fill: int | None) -> bytes:
    if not original:
        fail("PresentFrame asset is empty")
    if fill is not None:
        return bytes([fill & 0xFF]) * len(original)
    return bytes(byte ^ 0xFF for byte in original)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", choices=sorted(PRESENT_FRAME_KIND_BY_API), required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--frame-index", type=int)
    parser.add_argument("--fill", type=lambda value: int(value, 0))
    args = parser.parse_args()

    if args.source.resolve() == args.output.resolve():
        fail("--source and --output must be different")
    if not args.source.is_dir():
        fail(f"missing source bundle: {args.source}")
    if args.frame_index is not None and args.frame_index < 0:
        fail("--frame-index must be non-negative")

    if args.output.exists():
        shutil.rmtree(args.output)
    shutil.copytree(args.source, args.output, copy_function=copy_or_link)

    debug_name = PRESENT_FRAME_KIND_BY_API[args.api]
    frame = find_present_frame(args.output, debug_name, args.frame_index)
    frame_path = frame["frame_path"]
    relative_path = validate_relative_path(frame_path)
    absolute_path = args.output / relative_path
    if not absolute_path.is_file():
        fail(f"missing PresentFrame asset: {frame_path}")

    original = absolute_path.read_bytes()
    absolute_path.unlink()
    absolute_path.write_bytes(poison_bytes(original, args.fill))
    original_size = len(original)
    new_digest = sha256_bytes(absolute_path.read_bytes())
    update_assets_json(args.output, frame_path, new_digest, original_size)
    changed_paths = [frame_path]
    if (args.output / "assets.json").is_file():
        changed_paths.append("assets.json")
    update_checksums_json(args.output, changed_paths)

    print(f"poisoned {debug_name} frame_path={frame_path} bytes={original_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
