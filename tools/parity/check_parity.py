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


def run(
    root: Path | None = None,
    allowlist_path: Path | None = None,
    core_map_path: Path | None = None,
    selected: list[str] | None = None,
):
    """Build and return the parity :class:`compare.Report` for ``root``.

    The reusable core of :func:`main` (no argv parsing / no printing), so callers
    and tests can inspect findings directly. C is always included; surface order
    is canonicalized.
    """
    root = root or _repo_root()
    allow = allowlist_mod.load(allowlist_path or (_HERE / "allowlist.toml"))
    core_configs = core_defaults.load(core_map_path or (_HERE / "core_map.toml"), root)

    selected = list(selected or SURFACES)
    if "c" not in selected:
        selected = ["c", *selected]
    selected = [s for s in SURFACES if s in selected]

    extractions = {s: _EXTRACTORS[s](root) for s in selected}
    wasm_int = wasm_internal.extract(root) if "wasm" in selected else None
    return compare.build_report(extractions, allow, selected, core_configs, wasm_int)


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

    rep = run(args.root, args.allowlist, args.core_map, selected)

    if args.json:
        print(report_mod.to_json(rep))
    else:
        print(report_mod.to_markdown(rep))

    return 0 if not rep.active() else 1


if __name__ == "__main__":
    raise SystemExit(main())
