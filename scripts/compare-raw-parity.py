#!/usr/bin/env python3
import argparse
import filecmp
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd):
    result = subprocess.run(cmd, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(map(str, cmd))}")


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path):
    with path.open("rb") as stream:
        return json.load(stream)


def copy_dual_bundle(source, target):
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(
        source,
        target,
        ignore=shutil.ignore_patterns("*.bundle-finalize.tmp"),
        symlinks=True,
    )


def asset_index(bundle):
    data = read_json(bundle / "assets.json")
    assets = data.get("assets", [])
    index = {}
    for asset in assets:
        rel = asset.get("path", "")
        if not rel:
            raise RuntimeError(f"{bundle}: asset entry missing path")
        path = bundle / rel
        if not path.is_file():
            raise RuntimeError(f"{bundle}: asset file missing: {rel}")
        key = (
            rel,
            str(asset.get("kind", "")),
            str(asset.get("content_hash", "")),
            int(asset.get("byte_size", -1)),
        )
        value = file_sha256(path)
        if key in index and index[key] != value:
            raise RuntimeError(f"{bundle}: duplicate asset key with different content: {rel}")
        index[key] = value
    return index


def checksum_files(bundle):
    data = read_json(bundle / "checksums.json")
    files = data.get("files", {})
    if not isinstance(files, dict):
        raise RuntimeError(f"{bundle}: checksums.json files must be an object")
    return files


def compare_maps(label, old_map, raw_map):
    if old_map == raw_map:
        return
    old_keys = set(old_map)
    raw_keys = set(raw_map)
    for key in sorted(old_keys - raw_keys):
        raise RuntimeError(f"{label} mismatch: missing from raw-finalized: {key}")
    for key in sorted(raw_keys - old_keys):
        raise RuntimeError(f"{label} mismatch: extra in raw-finalized: {key}")
    for key in sorted(old_keys & raw_keys):
        if old_map[key] != raw_map[key]:
            raise RuntimeError(f"{label} mismatch at {key}: old={old_map[key]} raw={raw_map[key]}")


def compare_file_bytes(label, old_path, raw_path):
    if filecmp.cmp(old_path, raw_path, shallow=False):
        return
    old_hash = file_sha256(old_path) if old_path.exists() else "<missing>"
    raw_hash = file_sha256(raw_path) if raw_path.exists() else "<missing>"
    raise RuntimeError(f"{label} byte mismatch: old={old_hash} raw={raw_hash}")


def main():
    parser = argparse.ArgumentParser(
        description="Finalize a dual-write bundle through old and raw paths, then compare parity."
    )
    parser.add_argument("dual_write_bundle", type=Path)
    parser.add_argument("--bundle-finalize", default="bundle-finalize", type=Path)
    parser.add_argument("--bundle-check", default="bundle-check", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--jobs", default="1")
    parser.add_argument("--keep-work", action="store_true")
    args = parser.parse_args()

    source = args.dual_write_bundle.resolve()
    if not source.is_dir():
        raise RuntimeError(f"bundle root is not a directory: {source}")
    if not (source / "raw" / "events.bin").is_file():
        raise RuntimeError(f"missing raw sideband: {source / 'raw' / 'events.bin'}")

    work = args.work_dir
    if work is None:
        work = source.parent / f"{source.name}.raw-parity-work"
    work = work.resolve()
    old_bundle = work / "old-finalized.apitrace"
    raw_bundle = work / "raw-finalized.apitrace"

    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    try:
        copy_dual_bundle(source, old_bundle)
        copy_dual_bundle(source, raw_bundle)

        run([args.bundle_finalize, "--no-progress", "--jobs", args.jobs, old_bundle])
        run([args.bundle_finalize, "--raw-format", "--no-progress", "--jobs", args.jobs, raw_bundle])
        run([args.bundle_check, "--verify-hashes", old_bundle])
        run([args.bundle_check, "--verify-hashes", raw_bundle])

        compare_file_bytes("callstream.jsonl", old_bundle / "callstream.jsonl", raw_bundle / "callstream.jsonl")
        compare_maps("assets.json canonical asset index", asset_index(old_bundle), asset_index(raw_bundle))
        compare_maps("checksums.json file index", checksum_files(old_bundle), checksum_files(raw_bundle))

        old_objects = old_bundle / "objects" / "objects.json"
        raw_objects = raw_bundle / "objects" / "objects.json"
        if old_objects.exists() or raw_objects.exists():
            compare_file_bytes("objects/objects.json", old_objects, raw_objects)

        print("raw-parity PASS")
        print(f"old_bundle={old_bundle}")
        print(f"raw_bundle={raw_bundle}")
        print(f"callstream_sha256={file_sha256(old_bundle / 'callstream.jsonl')}")
        print(f"asset_count={len(asset_index(old_bundle))}")
        print(f"checksum_file_count={len(checksum_files(old_bundle))}")
        return 0
    except Exception as exc:
        print(f"raw-parity FAIL: {exc}", file=sys.stderr)
        print(f"old_bundle={old_bundle}", file=sys.stderr)
        print(f"raw_bundle={raw_bundle}", file=sys.stderr)
        return 1
    finally:
        if not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
