"""Regression tests for the reach behind each category's finding count.

A parity check reports a divergence it found. It has no way to report a
divergence it never looked for, so an extractor that stops feeding a category
produces the same output a clean tree does: zero findings, a `_none_` line, and
a summary row of zeroes. The category with the least reach in this repository is
``default`` — a facade that spells its defaults inside a request-object
normalizer puts them out of the signature the extractor reads, and roughly
ninety-six percent of the candidate pairs carry nothing to compare because of
it. That is a real limit, and it has to be legible as one rather than as a pass.

So the count of verdicts reached is an output, and a category that reached none
at all fails. Both halves are pinned here, each against a control that can go
the other way.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_comparison_reach.py
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod
import check_parity
import compare
import report as report_mod
from model import SURFACES, Extraction, FunctionSig, Param


def _facade(surface: str, key: str, param: str, default: str | None) -> Extraction:
    ex = Extraction(surface=surface)
    ex.functions = [
        FunctionSig(
            key=key,
            surface=surface,
            raw_name=key,
            params=[Param(name=param, raw_name=param, default=default)],
            file=f"{surface}.src",
            line=1,
        )
    ]
    return ex


def _c(key: str, param: str) -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(
            key=key,
            surface="c",
            raw_name=key,
            params=[Param(name=param, raw_name=param)],
            file="c.h",
            line=1,
        )
    ]
    return ex


def _report(allow, *facades: Extraction):
    extractions = {"c": _c("normalize", "target_db")}
    for ex in facades:
        extractions[ex.surface] = ex
    return compare.build_report(
        extractions, allow, ["c", *[ex.surface for ex in facades]]
    )


# ---------------------------------------------------------------------------
# The count itself.


def test_two_facades_that_each_spell_a_default_reach_a_verdict() -> None:
    """The positive control for everything below: this is what reach looks like."""
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", "0.0"),
    )
    assert rep.comparison_counts()["default"] == 1


def test_a_default_only_one_facade_spells_reaches_no_verdict() -> None:
    """The shape the report used to render as a clean row.

    Same two facades, same parameter, one default removed — the comparison is
    declined, nothing is found, and the finding count is identical to the case
    above. Only the reach separates them.
    """
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", None),
    )
    assert rep.comparison_counts().get("default", 0) == 0
    assert [f for f in rep.active() if f.category == "default"] == []
    assert [n["category"] for n in rep.not_compared if n["category"] == "default"]


def test_a_divergence_is_still_found_when_the_comparison_runs() -> None:
    """Non-vacuity for the counter: counting a verdict has not replaced reaching one."""
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "-20.0"),
        _facade("node", "normalize", "target_db", "0.0"),
    )
    assert rep.comparison_counts()["default"] == 1
    assert [f.key for f in rep.active() if f.category == "default"] == ["normalize"]


# ---------------------------------------------------------------------------
# The gate.
#
# ``_blind_categories`` judges the run the caller asked for, so these pass the
# full surface list alongside a two-facade report: the selection is the
# function's input, not something it rereads off the report.


def test_a_category_that_reached_no_verdict_is_named() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", None),
    )
    assert "default" in check_parity._blind_categories(rep, list(SURFACES))


def test_a_category_that_reached_one_verdict_is_not_named() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", "0.0"),
    )
    assert "default" not in check_parity._blind_categories(rep, list(SURFACES))


def test_a_partial_run_is_not_judged() -> None:
    """``--surface c,python`` deliberately does not build the WASM wiring check."""
    allow = allowlist_mod.Allowlist()
    rep = _report(allow, _facade("python", "normalize", "target_db", None))
    assert check_parity._blind_categories(rep, ["c", "python"]) == []


# ---------------------------------------------------------------------------
# What a reader sees.


def test_the_none_line_carries_the_reach_behind_it() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", None),
    )
    section = report_mod.to_markdown(rep).split("## Default drift (cross-facade) —")[1]
    line = section.split("\n")[2]
    assert "_none_" in line and "**0** comparison" in line, line


def test_the_real_tree_has_no_blind_category() -> None:
    """The case the gate exists for: an extractor that stops feeding a check.

    Every category has candidates in this repository, so a zero here is a tool
    that broke rather than a tree that is clean.
    """
    rep = check_parity.run()
    assert check_parity._blind_categories(rep, list(SURFACES)) == []


def _main() -> int:
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_main())
