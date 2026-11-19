"""Tests for ``facts.field_bindings``: name pairing, unpaired rows, and non-vacuity.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_facts.py
"""

from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import facts
from model import FunctionSig, Param, RecordField, RecordShape


def _sig(surface: str, *params: tuple[str, str]) -> FunctionSig:
    return FunctionSig(
        key="demo",
        surface=surface,
        params=[Param(name=n, raw_name=r) for n, r in params],
        file=f"{surface}.src",
        line=7,
    )


def test_param_pairs_by_canonical_name_and_reports_a_rename_as_two_rows():
    c = _sig("c", ("hop_length", "hop_length"), ("sr", "sr"), ("config", "config"))
    node = _sig(
        "node", ("hop_length", "hopLength"), ("sample_rate", "sampleRate"), ("request", "request")
    )
    rows = {(b.c_subject, b.surface_spelling) for b in facts._param_rows(c, node, "node")}
    assert ("hop_length", "hopLength") in rows, rows
    assert ("sr", None) in rows and (None, "sampleRate") in rows, rows
    assert not any("config" in (b or "") or "request" in (s or "") for b, s in rows), rows


def test_structural_sample_rate_pairs_with_the_facade_spelling_only_when_bound():
    c = FunctionSig(
        key="demo",
        surface="c",
        params=[
            Param(name="samples", raw_name="samples", structural=True),
            Param(name="sample_rate", raw_name="sample_rate", structural=True),
        ],
        file="c.h",
        line=1,
    )
    rows = {
        (b.c_subject, b.surface_spelling)
        for b in facts._param_rows(c, _sig("python", ("sr", "sr")), "python")
    }
    assert rows == {("sample_rate", "sr")}, rows
    assert facts._param_rows(c, _sig("node"), "node") == []


def test_record_skips_facade_only_fields_and_locates_unbound_rows_in_c():
    c = RecordShape(
        key="demo",
        surface="c",
        fields=[RecordField("gain_db", "gain_db"), RecordField("q", "q")],
        file="c.h",
        line=3,
    )
    ts = RecordShape(
        key="demo",
        surface="wasm",
        fields=[RecordField("gain_db", "gainDb"), RecordField("on_progress", "onProgress")],
        file="w.ts",
        line=9,
    )
    rows = facts._record_rows(c, ts, "wasm")
    by = {b.c_subject: b for b in rows}
    assert by["gain_db"].surface_spelling == "gainDb" and by["gain_db"].file == "w.ts"
    assert by["q"].surface_spelling is None and by["q"].file == "c.h"
    assert all(b.c_subject is not None for b in rows), rows


def test_real_tree_yields_bound_rows_of_both_kinds_on_every_facade():
    rows = facts.field_bindings(_HERE.parent.parent)
    bound = Counter((b.surface, b.kind) for b in rows if b.c_subject and b.surface_spelling)
    for surface in facts.FACADES:
        for kind in ("param", "record_field"):
            assert bound[(surface, kind)] > 0, f"{surface} {kind}: no paired row"


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
