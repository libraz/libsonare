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

import allowlist as allowlist_mod
import compare
import core_defaults
import report as report_mod
from extractors import (
    c_api,
    cli,
    node_ts,
    python_pyi,
    wasm_internal,
    wasm_ts,
)
from model import SURFACES

#: Allowlist section names that differ from the finding category they gate, for
#: matching an expired entry against the declined comparison behind it.
_AUDIT_CATEGORY = {"input_naming": "input"}

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
    extractors: dict | None = None,
):
    """Build and return the parity :class:`compare.Report` for ``root``.

    The reusable core of :func:`main` (no argv parsing / no printing), so callers
    and tests can inspect findings directly. C is always included; surface order
    is canonicalized.

    ``extractors`` overrides individual surface extractors. It exists so a
    caller can narrow one surface -- the capability matrix runs the CLI surface
    once per front-end -- and still get the checker's own reachability verdict
    rather than a second implementation of it.
    """
    root = root or _repo_root()
    allow = allowlist_mod.load(allowlist_path or (_HERE / "allowlist.toml"))
    core_configs = core_defaults.load(core_map_path or (_HERE / "core_map.toml"), root)

    selected = list(selected or SURFACES)
    if "c" not in selected:
        selected = ["c", *selected]
    selected = [s for s in SURFACES if s in selected]

    resolved = {**_EXTRACTORS, **(extractors or {})}
    extractions = {s: resolved[s](root) for s in selected}
    wasm_int = wasm_internal.extract(root) if "wasm" in selected else None
    return compare.build_report(extractions, allow, selected, core_configs, wasm_int)


def _audit_allowlist(rep, selected: list[str], path: Path) -> int:
    """Name every allowlist entry whose divergence a comparison no longer finds.

    Such an entry is not inert. It keeps asserting that a divergence under that
    name was reviewed and accepted, so the next symbol to take the name inherits
    the blessing without anyone looking at it -- which is how an allowlist stops
    being a record of decisions and becomes a hole. Removing an entry the moment
    its divergence is fixed is what keeps the file readable as decisions.

    An entry standing in front of a comparison that never runs fails the same
    way, because the tool derives that case itself -- a facade that folded its
    argument order into a request object is recorded as not compared, and is
    compared again the moment it goes back to positional parameters. A
    hand-written entry for it would excuse the restored divergence instead.

    Only a full-surface run can answer this: a comparison limited to two surfaces
    never consults the other two's entries, and every one of them would look
    expired.
    """
    if list(selected) != list(SURFACES):
        print(
            "--audit-allowlist needs every surface; rerun without --surface",
            file=sys.stderr,
        )
        return 2
    ratcheted = rep.allowlist.ratcheted_entries() if rep.allowlist else []
    if ratcheted:
        print(
            f"{len(ratcheted)} allowlist entr(ies) in a section held empty: {path}",
            file=sys.stderr,
        )
        for scope, pattern in ratcheted:
            print(f"  [{scope}] {pattern}", file=sys.stderr)
        print(
            "A differing core default or enum set is usually a surface that "
            "computes something else rather than one that spells a name "
            "differently -- fix the surface. If this really is the same value "
            "under another spelling, take its section out of "
            "allowlist.RATCHETED_SECTIONS in the same change, so the widening "
            "is reviewed instead of inherited.",
            file=sys.stderr,
        )
        return 1
    expired = rep.allowlist.expired_entries() if rep.allowlist else []
    if not expired:
        print(
            "every allowlist entry still excuses a divergence a comparison found, "
            f"and nothing sits in a held-empty section: {path}"
        )
        return 0
    # Why the comparison behind an entry produced nothing, where the run derived
    # one: the reason is what tells a reader whether to delete or to look closer.
    declined = {(n["category"], n["key"], n["surface"]): n["reason"] for n in rep.not_compared}
    print(
        f"{len(expired)} allowlist entr(ies) no longer suppress a divergence: {path}",
        file=sys.stderr,
    )
    for scope, pattern, state in expired:
        parts = scope.split(".")
        category = _AUDIT_CATEGORY.get(parts[0], parts[0])
        reason = declined.get((category, pattern, parts[-1])) or next(
            (r for (c, k, _), r in declined.items() if c == category and k == pattern),
            None,
        )
        note = f" — {reason}" if reason else ""
        print(f"  [{scope}] {pattern} ({state}){note}", file=sys.stderr)
    print(
        "Remove each one, or say in its reason why it must outlive the "
        "divergence it excuses. `stale` means the comparison ran and the "
        "surfaces agreed; `unconsulted` means no comparison looked the name up.",
        file=sys.stderr,
    )
    return 1


def _blind_categories(rep, selected: list[str]) -> list[str]:
    """Categories that reached no verdict, on a run that was able to reach one.

    A check whose extractor stops seeing the thing it compares does not report a
    failure — it reports nothing, which is exactly what a clean tree reports.
    Every category has candidates in this repository, so zero verdicts means the
    check no longer runs rather than that it ran and found nothing.

    Only a full-surface run can say this: with ``--surface c,python`` the WASM
    wiring check is deliberately not built, and the record and default checks
    lose the second facade they need.
    """
    if list(selected) != list(SURFACES):
        return []
    compared = rep.comparison_counts()
    return [c for c in report_mod.CATEGORIES if not compared.get(c)]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    ap.add_argument(
        "--audit-allowlist",
        action="store_true",
        help=(
            "Report allowlist entries whose divergence a comparison no longer "
            "finds, and exit 1 if any"
        ),
    )
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
        ap.error(f"unknown surface(s): {', '.join(invalid)} (valid: {', '.join(SURFACES)})")
    if "c" not in selected:
        selected = ["c", *selected]  # C is the canonical reference; always needed.
    # Preserve canonical surface order.
    selected = [s for s in SURFACES if s in selected]

    rep = run(args.root, args.allowlist, args.core_map, selected)

    if args.audit_allowlist:
        return _audit_allowlist(rep, selected, args.allowlist)

    if args.json:
        print(report_mod.to_json(rep))
    else:
        print(report_mod.to_markdown(rep))

    blind = _blind_categories(rep, selected)
    if blind:
        print(
            f"{len(blind)} categor(ies) reached no verdict: {', '.join(blind)}. "
            "Their clean rows are the absence of a measurement, not the result "
            "of one — find what stopped the extractor feeding them.",
            file=sys.stderr,
        )
        return 1

    return 0 if not rep.active() else 1


if __name__ == "__main__":
    raise SystemExit(main())
