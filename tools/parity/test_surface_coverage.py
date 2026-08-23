#!/usr/bin/env python3
"""Regression tests for the tracked per-runtime capability matrix.

The matrix exists to qualify a claim ("the same engine in every runtime") with
the number behind it, so the two ways it could quietly stop qualifying anything
are what these tests pin: counting a reviewed gap as coverage, and losing a
domain because a header was added or split.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_surface_coverage.py
"""

from __future__ import annotations

from pathlib import Path
import sys

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod  # noqa: E402
import compare  # noqa: E402
import surface_coverage  # noqa: E402
from model import Extraction, FunctionSig  # noqa: E402


def _c(*declarations: tuple[str, str]) -> Extraction:
    """C extraction from (key, declaring header) pairs."""
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(key=key, surface="c", raw_name=key, file=header, line=1)
        for key, header in declarations
    ]
    return ex


def _facade(surface: str, *keys: str) -> Extraction:
    ex = Extraction(surface=surface)
    ex.functions = [
        FunctionSig(key=key, surface=surface, raw_name=key, file=f"{surface}.ts", line=1)
        for key in keys
    ]
    return ex


def _report(c: Extraction, facades: dict[str, Extraction], allow=None):
    extractions = {"c": c, **facades}
    return compare.build_report(
        extractions, allow or allowlist_mod.Allowlist(), ["c", *facades]
    )


def _headers(c: Extraction) -> dict[str, str]:
    return {sig.key: sig.file for sig in c.functions}


def test_domain_folds_the_split_project_headers_into_one_row() -> None:
    """Five project headers are one capability domain to a reader."""
    domains = {
        surface_coverage.domain_of(f"include/sonare/sonare_c_project_{part}.h")
        for part in ("core", "edit", "midi", "annotate", "instruments")
    }
    assert domains == {"project & arrangement"}, domains


def test_an_unmapped_header_still_gets_a_row() -> None:
    """A new public header adds a domain instead of vanishing from the table."""
    assert (
        surface_coverage.domain_of("include/sonare/sonare_c_time_stretch.h")
        == "time stretch"
    )


def test_an_allowlisted_gap_still_counts_as_a_gap() -> None:
    """An allowlist entry reviews a divergence; it does not restore a capability."""
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_apply_named_processor"]}
    c = _c(("mastering_apply_named_processor", "include/sonare/sonare_c_mastering.h"))
    rep = _report(c, {"python": _facade("python")}, allow)
    assert not rep.active(), [f.message for f in rep.active()]
    missing = surface_coverage.unreachable_keys(rep)
    assert "mastering_apply_named_processor" in missing["python"], missing


def test_a_lifecycle_helper_is_not_a_gap() -> None:
    """Destructors are answered by the facade object model, not by a function."""
    c = _c(("audio_free", "include/sonare/sonare_c_types_functions.h"))
    rep = _report(c, {"python": _facade("python")})
    assert "audio_free" not in surface_coverage.unreachable_keys(rep)["python"]


def test_row_counts_and_totals_agree_with_the_findings() -> None:
    c = _c(
        ("mastering_apply", "include/sonare/sonare_c_mastering.h"),
        ("mastering_analyze", "include/sonare/sonare_c_mastering.h"),
        ("mixing_add_bus", "include/sonare/sonare_c_mixing.h"),
    )
    facades = {
        "python": _facade("python", "mastering_apply", "mixing_add_bus"),
        "node": _facade("node", "mastering_apply", "mastering_analyze", "mixing_add_bus"),
        "wasm": _facade("wasm", "mastering_apply", "mastering_analyze", "mixing_add_bus"),
        "cli": _facade("cli"),
    }
    rows = surface_coverage.build_rows(_report(c, facades), _headers(c))
    by_domain = {domain: (total, counts) for domain, total, counts in rows}
    assert by_domain["mastering"][0] == 2
    assert by_domain["mastering"][1]["python"] == 1
    assert by_domain["mastering"][1]["node"] == 2
    assert by_domain["mixing & routing"][1]["python"] == 1
    assert by_domain["mixing & routing"][1]["cli"] == 0


def test_the_tracked_table_matches_the_current_surfaces() -> None:
    """The committed matrix is the one this repository's surfaces produce."""
    root = _HERE.parent.parent
    tracked = root / surface_coverage.DEFAULT_OUTPUT
    assert tracked.exists(), tracked
    import check_parity  # noqa: PLC0415

    rendered = surface_coverage.render(
        check_parity.run(root=root), surface_coverage.c_declaration_headers(root)
    )
    assert tracked.read_text(encoding="utf-8") == rendered, (
        "run make surface-coverage"
    )


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
