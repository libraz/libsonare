#!/usr/bin/env python3
"""Measure and enforce deterministic size budgets for shipped WASM files.

Only ``raw`` and ``gzip`` are compared against the baseline. ``code``, ``data``
and ``sha256`` are recorded for provenance and printed, never asserted: the
emscripten output is not byte-reproducible across machines or toolchain patch
releases, so a hash comparison would fail on every runner that is not the one
that wrote the baseline. Do not promote sha256 to a gate without first making
the build reproducible.

The package keeps two files in this format and they are not interchangeable.
``wasm-size-budget.json`` is the enforced ceiling ``check:wasm-size`` compares
against; ``wasm-size-baseline.json`` records what the build actually measured,
refreshed by ``check:wasm-size:record``. Both are written by ``--write-baseline``
against whichever path ``--baseline`` names, so pass the one being updated.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


def read_uleb(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise ValueError("truncated unsigned LEB128 value")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
        if shift > 35:
            raise ValueError("invalid unsigned LEB128 value")


def wasm_sections(data: bytes) -> dict[str, int]:
    if data[:8] != b"\x00asm\x01\x00\x00\x00":
        raise ValueError("not a WebAssembly binary")
    offset = 8
    sizes = {"code": 0, "data": 0}
    while offset < len(data):
        section_id = data[offset]
        offset += 1
        section_size, offset = read_uleb(data, offset)
        end = offset + section_size
        if end > len(data):
            raise ValueError("truncated WebAssembly section")
        if section_id == 10:
            sizes["code"] += section_size
        elif section_id == 11:
            sizes["data"] += section_size
        offset = end
    return sizes


def measure(path: Path) -> dict[str, int | str]:
    data = path.read_bytes()
    sections = wasm_sections(data)
    return {
        "raw": len(data),
        "gzip": len(gzip.compress(data, compresslevel=9, mtime=0)),
        "code": sections["code"],
        "data": sections["data"],
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def parse_artifact(value: str) -> tuple[str, Path]:
    name, separator, raw_path = value.partition("=")
    if not separator or not name or not raw_path:
        raise argparse.ArgumentTypeError("artifact must use NAME=PATH")
    return name, Path(raw_path)


def load_baseline(path: Path) -> dict[str, Any]:
    try:
        baseline = json.loads(path.read_text())
    except FileNotFoundError as error:
        raise SystemExit(f"missing WASM size baseline: {path}") from error
    if baseline.get("format") != 1 or not isinstance(baseline.get("artifacts"), dict):
        raise SystemExit(f"invalid WASM size baseline: {path}")
    return baseline


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--artifact", action="append", type=parse_artifact, required=True)
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--max-increase", type=float, default=0.02)
    args = parser.parse_args()

    if args.max_increase < 0:
        parser.error("--max-increase must be non-negative")

    current: dict[str, dict[str, int | str]] = {}
    for name, path in args.artifact:
        if name in current:
            parser.error(f"artifact listed more than once: {name}")
        if not path.is_file():
            raise SystemExit(f"missing WASM artifact: {path}")
        current[name] = measure(path)

    if args.write_baseline:
        payload = {
            "format": 1,
            "toolchain": {"emsdk": "5.0.2"},
            "artifacts": current,
        }
        args.baseline.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        print(f"wrote WASM size baseline: {args.baseline}")
        return 0

    baseline = load_baseline(args.baseline)
    failures: list[str] = []
    for name, result in current.items():
        expected = baseline["artifacts"].get(name)
        if not isinstance(expected, dict):
            failures.append(f"{name}: missing from baseline")
            continue
        for metric in ("raw", "gzip"):
            old = expected.get(metric)
            now = result[metric]
            if not isinstance(old, int):
                failures.append(f"{name}: baseline lacks {metric}")
                continue
            limit = old * (1 + args.max_increase)
            print(f"{name} {metric}: {now} bytes (baseline {old}, limit {limit:.0f})")
            if now > limit:
                failures.append(f"{name}: {metric} {now} exceeds {limit:.0f} (+{args.max_increase:.0%})")
        print(
            f"{name} code={result['code']} data={result['data']} sha256={result['sha256']}"
        )

    if failures:
        print("WASM size budget failed:", *failures, sep="\n  ", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
