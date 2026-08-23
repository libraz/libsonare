#!/usr/bin/env python3
"""Regression tests for the stale-allowlist-entry audit.

An allowlist entry is a recorded decision about one divergence. Once that
divergence is fixed the entry stops describing anything, but it does not stop
asserting: the next symbol to take the name inherits a blessing nobody granted
it. The audit exists to make that moment visible, so what these tests pin is
that it neither misses a dead entry nor accuses a live one.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_allowlist_audit.py
"""

from __future__ import annotations

from pathlib import Path
import sys

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod  # noqa: E402
import check_parity  # noqa: E402
import compare  # noqa: E402
from model import Extraction, FunctionSig, SURFACES  # noqa: E402


def _c(*keys: str) -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(key=k, surface="c", raw_name=k, file="c.h", line=1) for k in keys
    ]
    return ex


def _facade(surface: str, *keys: str) -> Extraction:
    ex = Extraction(surface=surface)
    ex.functions = [
        FunctionSig(key=k, surface=surface, raw_name=k, file=f"{surface}.ts", line=1)
        for k in keys
    ]
    return ex


def _report(c: Extraction, facade: Extraction, allow):
    return compare.build_report(
        {"c": c, facade.surface: facade}, allow, ["c", facade.surface]
    )


def test_an_entry_that_suppressed_a_gap_is_not_reported() -> None:
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_apply"]}
    _report(_c("mastering_apply"), _facade("python"), allow)
    assert allow.unused_entries() == [], allow.unused_entries()


def test_an_entry_whose_gap_was_fixed_is_reported_with_its_section() -> None:
    """The facade now exposes the key, so nothing consults the entry."""
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_apply"]}
    _report(_c("mastering_apply"), _facade("python", "mastering_apply"), allow)
    assert allow.unused_entries() == [("coverage.python", "mastering_apply")]


def test_a_wildcard_is_used_when_any_name_matches_it() -> None:
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_*"]}
    _report(_c("mastering_apply", "mastering_analyze"), _facade("python"), allow)
    assert allow.unused_entries() == [], allow.unused_entries()


def test_a_shared_surface_only_entry_counts_as_used_from_any_surface() -> None:
    """``[surface_only] any`` is consulted per surface; one hit is enough."""
    allow = allowlist_mod.Allowlist()
    allow.surface_only = {"any": ["require_module"]}
    assert allow.surface_only_ok("require_module", "wasm")
    assert allow.unused_entries() == [], allow.unused_entries()


def test_the_repository_allowlist_carries_no_stale_entry() -> None:
    """Every committed entry still excuses a divergence that exists today."""
    root = _HERE.parent.parent
    rep = check_parity.run(root=root, selected=list(SURFACES))
    stale = rep.allowlist.unused_entries()
    assert stale == [], [f"[{scope}] {pattern}" for scope, pattern in stale]


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:  # noqa: PERF203
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
