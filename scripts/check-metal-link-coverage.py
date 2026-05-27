#!/usr/bin/env python3
"""Validate metal translation-link coverage for non-zero D3D sequences."""

from __future__ import annotations

import json
import pathlib
import sys


REQUIRED_SCOPE_KINDS = {"encoder", "draw_to_metal_calls"}


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: check-metal-link-coverage.py <translation-links.jsonl>")

    path = pathlib.Path(sys.argv[1])
    if not path.is_file():
        fail(f"missing translation-links file: {path}")

    coverage: dict[int, set[str]] = {}
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

    missing: list[str] = []
    for d3d_sequence in sorted(coverage):
        present = coverage[d3d_sequence]
        required_missing = sorted(REQUIRED_SCOPE_KINDS - present)
        if required_missing:
            missing.append(f"{d3d_sequence}:{','.join(required_missing)}")

    if missing:
        print("missing_link_coverage=" + ";".join(missing))
        return 1

    print(f"d3d_sequences={len(coverage)}")
    print("coverage=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
