"""A fit started from named knob values: the start, its refusals, its record and its anchors."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import fits
import report
from knobs import (
    Knob,
    apply_start_overrides,
    head_of,
    load_start_overrides,
    tunable_overrides,
)


def _knob(label: str, *, lo=0.0, hi=10.0, start=3.0, tunable=True) -> Knob:
    return Knob(
        label=label, lo=lo, hi=hi, log=False, start_value=start, tunable=label if tunable else None
    )


def test_a_start_override_sets_the_initial_value_and_keeps_the_shipped_one():
    knobs = [_knob("a.x"), _knob("a.y", start=1.0)]
    applied = apply_start_overrides(knobs, "a.x=7", tunables={})
    assert applied == "a.x=7.0"
    assert [k.start_value for k in knobs] == [7.0, 1.0]
    assert [head_of(k) for k in knobs] == [3.0, 1.0]
    # The render path gets the start; the paste-ready string still lists the knob left there.
    assert tunable_overrides(knobs, [k.start_value for k in knobs]) == "a.x=7.0,a.y=1.0"
    assert tunable_overrides(knobs, [7.0, 1.0], changed_only=True) == "a.x=7.0"


def test_a_start_outside_the_range_is_clamped_and_says_so(capsys):
    knobs = [_knob("a.x")]
    assert apply_start_overrides(knobs, "a.x=99", tunables={}) == "a.x=10.0"
    assert knobs[0].start_value == 10.0
    assert "clamped to 10.0" in capsys.readouterr().err


def test_a_key_that_is_not_a_search_knob_is_refused():
    with pytest.raises(ValueError, match="not search knobs: a.nope"):
        apply_start_overrides([_knob("a.x")], "a.x=1,a.nope=2", tunables={})


def test_a_key_that_is_also_a_base_override_is_refused():
    with pytest.raises(ValueError, match="also in --base-overrides: a.x"):
        apply_start_overrides([_knob("a.x")], "a.x=1", "a.x=2", tunables={})
    with pytest.raises(ValueError, match="given twice"):
        apply_start_overrides([_knob("a.x")], "a.x=1,a.x=2", tunables={})


def test_inline_and_file_starts_read_the_same(tmp_path):
    f = tmp_path / "start.txt"
    f.write_text("a.x=1  # note\na.y=2\n")
    assert load_start_overrides(f"@{f}") == load_start_overrides("a.x=1,a.y=2")
    with pytest.raises(ValueError, match="start override a.x"):
        load_start_overrides("a.x=big")


def test_both_front_ends_take_the_flag():
    parsed = autofit.build_parser().parse_args(
        ["--spec", "auto", "--program", "0", "--start-overrides", "a.x=1"]
    )
    assert parsed.start_overrides == "a.x=1"


def test_a_fit_record_carries_the_start_next_to_the_base():
    rep = {"objective": "d", "knobs": [{"key": "brass.x"}], "base_overrides": "e.k=1",
           "start_overrides": "brass.x=2"}  # fmt: skip
    _, entry = fits.record_fit(rep, capture="c", timbre="t", outcome="adopted")
    assert entry["start_overrides"] == "brass.x=2" and entry["base_overrides"] == "e.k=1"
    _, plain = fits.record_fit(
        {"objective": "d", "knobs": [{"key": "brass.x"}]}, capture="c", timbre="t", outcome="kept"
    )
    assert "start_overrides" not in plain


def test_the_report_states_what_its_start_numbers_are_relative_to():
    assert "start-override point" in report._anchors("a.x=1")["start"]
    assert "HEAD's render" in report._anchors("")["start"]
    assert report._anchors("a.x=1")["gate_audit_pre"].startswith("HEAD")


def _fold(tmp_path, evaluator) -> dict:
    out = tmp_path / "r.json"
    out.write_text("{}")
    autofit._fold_write_back_verdict(str(out), evaluator)
    return json.loads(out.read_text())["level_anchor"]


def _evaluator(start_overrides: str, **extra):
    return SimpleNamespace(
        args=argparse.Namespace(start_overrides=start_overrides),
        write_back_refusal=None,
        start_level_offset_db=-2.0,
        best_level_offset_db=-3.0,
        **extra,
    )


def test_level_anchor_names_its_anchor_and_reads_head_apart(tmp_path):
    plain = _fold(tmp_path, _evaluator(""))
    assert plain["anchor"].startswith("start values (HEAD's render")
    assert "head_offset_db" not in plain and plain["drift_fence_anchor"] == "start"

    moved = _fold(tmp_path, _evaluator("a.x=7.0", head_level_offset_db=-1.0))
    assert "start-override point" in moved["anchor"] and "a.x=7.0" in moved["anchor"]
    assert moved["head_offset_db"] == -1.0 and moved["winner_minus_head_db"] == -2.0
    assert moved["winner_minus_start_db"] == -1.0 and moved["drift_fence_anchor"] == "start"

    gone = _fold(tmp_path, _evaluator("a.x=7.0", head_level_unavailable="no reading"))
    assert gone["head_offset_db"] is None and gone["head_offset_unavailable"] == "no reading"


def test_head_is_rendered_once_and_only_for_runtime_knobs():
    renders = []
    me = SimpleNamespace(
        head_level_offset_db=None,
        head_level_unavailable=None,
        terms_for=lambda values: None,
        _render_terms=lambda values: renders.append(values) or {"level_offset_db": -1.5},
    )
    knobs = [_knob("a.x")]
    apply_start_overrides(knobs, "a.x=7", tunables={})
    autofit.Evaluator.read_head_level(me, knobs)
    assert renders == [[3.0]] and me.head_level_offset_db == -1.5

    me.head_level_offset_db = None
    autofit.Evaluator.read_head_level(me, [_knob("s", tunable=False)])
    assert me.head_level_offset_db is None and "source knob" in me.head_level_unavailable
