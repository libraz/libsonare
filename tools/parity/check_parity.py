#!/usr/bin/env python3
"""Cross-binding parity drift checker for libsonare.

Compares every language surface (Python, Node, WASM, CLI) against the C API
(the canonical ABI) and reports seven kinds of drift: coverage gaps, cross-facade
default drift, facade-vs-C++-core default drift (core_map.toml), argument
order/count/name mismatch, audio-input naming, enum value-set mismatch, and
WASM-internal wiring consistency (embind -> SonareModule type -> index.ts facade).

Standard library only (ast, re, json, argparse, pathlib, tomllib, dataclasses).
Read-only: it never modifies repository sources.

Usage:
    python tools/parity/check_parity.py                 # markdown report
    python tools/parity/check_parity.py --json          # JSON findings
    python tools/parity/check_parity.py --surface c,node # limit surfaces

Exit code 0 when there is no non-allowlisted drift, else 1 (CI-gate friendly).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Allow running as a script (no package install) by adding our dir to sys.path.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod  # noqa: E402
import compare  # noqa: E402
import core_defaults  # noqa: E402
import report as report_mod  # noqa: E402
from extractors import c_api, cli, node_ts, python_pyi, wasm_internal, wasm_ts  # noqa: E402
from model import SURFACES  # noqa: E402

_EXTRACTORS = {
    "c": c_api.extract,
    "python": python_pyi.extract,
    "node": node_ts.extract,
    "wasm": wasm_ts.extract,
    "cli": cli.extract,
}


def _repo_root() -> Path:
    # tools/parity/check_parity.py -> repo root is two levels up.
    return _HERE.parent.parent


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    ap.add_argument(
        "--surface",
        default=",".join(SURFACES),
        help="Comma-separated surfaces to include (default: all). C is always included.",
    )
    ap.add_argument(
        "--root",
        type=Path,
        default=_repo_root(),
        help="Repository root (default: inferred from tool location)",
    )
    ap.add_argument(
        "--allowlist",
        type=Path,
        default=_HERE / "allowlist.toml",
        help="Path to allowlist.toml",
    )
    ap.add_argument(
        "--core-map",
        type=Path,
        default=_HERE / "core_map.toml",
        help="Path to core_map.toml (facade-vs-C++-core default check)",
    )
    args = ap.parse_args(argv)

    selected = [s.strip() for s in args.surface.split(",") if s.strip()]
    invalid = [s for s in selected if s not in SURFACES]
    if invalid:
        ap.error(
            f"unknown surface(s): {', '.join(invalid)} (valid: {', '.join(SURFACES)})"
        )
    if "c" not in selected:
        selected = ["c", *selected]  # C is the canonical reference; always needed.
    # Preserve canonical surface order.
    selected = [s for s in SURFACES if s in selected]

    allow = allowlist_mod.load(args.allowlist)
    core_configs = core_defaults.load(args.core_map, args.root)

    extractions = {}
    for s in selected:
        extractions[s] = _EXTRACTORS[s](args.root)

    # The WASM-internal consistency check cross-validates the WASM binding's own
    # three files (embind -> SonareModule type -> index.ts facade); only run it
    # when the WASM surface is in scope.
    wasm_int = wasm_internal.extract(args.root) if "wasm" in selected else None

    rep = compare.build_report(extractions, allow, selected, core_configs, wasm_int)

    if args.json:
        print(report_mod.to_json(rep))
    else:
        print(report_mod.to_markdown(rep))

    return 0 if not rep.active() else 1


if __name__ == "__main__":
    raise SystemExit(main())
