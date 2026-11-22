"""The window a statistic was read on, stored in the reference row and in the gate, and checked.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_window_storage.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

import profile_gate
from profile_gate import (
    format_window,
    gate_readings,
    window_mismatch,
    window_spans,
    write_gate_file,
)
from test_profile_window import _corpus, _measured

SHORT = {"model_s": 0.68, "reference_s": 0.68}
LONG = {"model_s": 1.5, "reference_s": 1.5}


@pytest.fixture(autouse=True)
def _build(monkeypatch):
    monkeypatch.setattr(profile_gate, "_model_build_state", lambda: {"built_utc": "build-1"})


def _summary(n=10):
    return {"decay": {"median": 5.0, "abs_median": 5.0, "p90": 5.0, "n": n}}


def _gate(tmp_path: Path, window: dict | None) -> Path:
    path = tmp_path / "g.json"
    write_gate_file(_summary(), path, "ref", 1.0, windows={"decay": window} if window else None)
    return path


def test_a_reference_row_stores_the_window_the_model_is_read_through_and_its_own_span(tmp_path):
    root, cfg = _corpus(tmp_path, [1500, 800], gate_ms=800)
    rows = _measured(tmp_path, "oneshot", root, cfg)
    assert [r["window"] for r in rows] == [
        {"model_s": 1.5, "reference_s": 1.5},
        {"model_s": 0.8, "reference_s": 0.8},
    ]
    single = _measured(tmp_path, "single", root, {**cfg, "source_class": "library"})
    assert [r["window"] for r in single] == [{"model_s": 0.8, "reference_s": 0.8}] * 2


def test_per_note_windows_are_summarised_by_their_median_and_range():
    spans = window_spans(
        0.68, 680, model_windows=[0.68, 1.12, 0.77], reference_spans=[0.68, 1.12, 0.77]
    )
    assert spans["decay"] == {
        "model_s": 0.77,
        "reference_s": 0.77,
        "model_range_s": [0.68, 1.12],
        "reference_range_s": [0.68, 1.12],
    }
    assert format_window(spans["decay"]) == "0.77 (0.68-1.12) s / 0.77 (0.68-1.12) s"
    assert window_spans(0.68, 680)["decay"] == {"model_s": 0.68, "reference_s": 0.68}


def test_a_write_stores_the_window_in_each_window_dependent_statistic(tmp_path):
    bounds = json.loads(_gate(tmp_path, SHORT).read_text())["bounds"]["decay"]
    assert all(bounds["provenance"][s]["window"] == SHORT for s in ("median", "abs_median", "p90"))


def test_a_kept_bound_keeps_the_window_it_was_set_on(tmp_path):
    gate = _gate(tmp_path, SHORT)
    write_gate_file(_summary(), gate, "ref", 1.0, windows={"decay": SHORT})
    prov = json.loads(gate.read_text())["bounds"]["decay"]["provenance"]
    assert prov["median"]["outcome"] == "kept"
    assert prov["median"]["window"] == SHORT


def _over(value):
    return {"decay": {"median": value, "abs_median": value, "p90": value, "n": 10}}


def test_a_bound_whose_window_moved_is_re_recorded_with_the_old_one_beside_it(tmp_path, capsys):
    """The ratchet holds within one window; across two the old number bounds another quantity."""
    gate = _gate(tmp_path, SHORT)
    capsys.readouterr()
    write_gate_file(_over(9.0), gate, "ref", 1.0, windows={"decay": LONG})
    doc = json.loads(gate.read_text())
    bound = doc["bounds"]["decay"]
    assert bound["median"] == 9.0
    prov = bound["provenance"]["median"]
    assert prov["outcome"] == "rewindowed"
    assert prov["window"] == LONG
    assert prov["rewindowed_from"] == {"bound": 5.0, "window": SHORT}
    out = capsys.readouterr().out
    assert "rewindowed decay.median bound 9 (model residual 9 on the new window; was 5 on" in out
    assert "kept the recorded bound" not in out
    found = profile_gate.provenance_findings(doc)
    assert ("decay", "median", 9.0) in found["rewindowed"] and not found["exceeded"]
    assert any("re-recorded for a window change" in n for n in profile_gate.provenance_notes(doc))


def test_a_bound_with_no_stored_window_is_judged_on_the_window_it_was_read_on(tmp_path):
    gate = _gate(tmp_path, None)
    write_gate_file(
        _over(9.0), gate, "ref", 1.0, windows={"decay": LONG}, unrecorded_windows={"decay": SHORT}
    )
    prov = json.loads(gate.read_text())["bounds"]["decay"]["provenance"]["median"]
    assert prov["outcome"] == "rewindowed"
    write_gate_file(
        _over(12.0), gate, "ref", 1.0, windows={"decay": LONG}, unrecorded_windows={"decay": SHORT}
    )
    prov = json.loads(gate.read_text())["bounds"]["decay"]["provenance"]["median"]
    assert prov["outcome"] == "exceeded"


def test_a_bound_on_the_unchanged_window_stays_tighten_only(tmp_path):
    gate = _gate(tmp_path, None)
    write_gate_file(
        _over(9.0), gate, "ref", 1.0, windows={"decay": SHORT}, unrecorded_windows={"decay": SHORT}
    )
    b = json.loads(gate.read_text())["bounds"]["decay"]
    assert b["median"] == 5.0 and b["provenance"]["median"]["outcome"] == "exceeded"


def test_a_statistic_is_refused_on_a_window_other_than_the_one_it_was_recorded_on(tmp_path, capsys):
    gate = _gate(tmp_path, SHORT)
    rc = profile_module.check_gate(_summary(), gate, "ref", windows={"decay": LONG})
    out = capsys.readouterr().out
    assert rc == 2
    assert "REFUSED" in out
    assert "model window 1.5 s / 1.5 s but the bound was recorded on 0.68 s / 0.68 s" in out
    assert "every recorded bound held" not in out


def test_a_statistic_on_its_recorded_window_is_judged_as_before(tmp_path):
    gate = _gate(tmp_path, SHORT)
    assert profile_module.check_gate(_summary(), gate, "ref", windows={"decay": SHORT}) == 0
    over = {"decay": {"median": 9.0, "abs_median": 9.0, "p90": 9.0, "n": 10}}
    assert profile_module.check_gate(over, gate, "ref", windows={"decay": SHORT}) == 1


def test_a_gate_with_no_stored_window_is_read_and_says_so(tmp_path, capsys):
    gate = _gate(tmp_path, None)
    rc = profile_module.check_gate(_summary(), gate, "ref", windows={"decay": LONG})
    out = capsys.readouterr().out
    assert rc == 0
    assert "window unrecorded" in out
    assert "REFUSED" not in out


def test_a_refused_reading_carries_no_value_and_says_why(tmp_path):
    bounds = json.loads(_gate(tmp_path, SHORT).read_text())["bounds"]
    got = gate_readings(_summary(), bounds, {"decay": LONG})
    assert {r["value"] for r in got} == {None}
    assert all("recorded on 0.68 s / 0.68 s" in r["refused"] for r in got)
    assert [r["value"] for r in gate_readings(_summary(), bounds, {"decay": SHORT})] == [5.0] * 3


def test_windows_with_nothing_recorded_never_mismatch():
    assert window_mismatch(None, LONG) is None
    assert window_mismatch(SHORT, None) is None
    assert window_mismatch(SHORT, dict(SHORT)) is None
    assert window_mismatch(SHORT, LONG) is not None
