"""The A4 ruler a stretch reading is taken against, and the bounds that carry it.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_a4_ruler.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gate_verify
import profile_gate
from profile_gate import FLOOR_FROM_RULER, gate_readings, window_mismatch, write_gate_file
from profile_summary import a4_ruler, a4_two_point_cents


@pytest.fixture(autouse=True)
def _build(monkeypatch):
    monkeypatch.setattr(profile_gate, "_model_build_state", lambda: {"built_utc": "build-1"})


def _rows(cents: dict[int, float], timbre: str = "t") -> list[dict]:
    return [
        {"timbre": timbre, "note": n, "velocity": 100, "cents_vs_et": c} for n, c in cents.items()
    ]


#: The accordion's shape: a local dip at n65 that a two-point interpolation lands on.
DIP = {53: 4.5, 59: 2.1, 65: -1.2, 71: 0.6, 77: 3.3, 83: 3.0}


def test_the_ruler_is_the_median_of_the_octave_either_side_of_a4_not_two_points():
    ruler = a4_ruler(_rows(DIP), "t")
    assert ruler == {"offset_c": 1.35, "scatter_c": 2.25, "n": 4, "notes": [59, 77]}
    assert a4_two_point_cents(_rows(DIP), "t") == pytest.approx(0.0)


def test_a_grid_off_a4_reads_its_nearest_octave_and_is_never_extrapolated():
    sloped = {n: 0.5 * (n - 40) for n in (40, 44, 48, 52)}
    ruler = a4_ruler(_rows(sloped), "t")
    assert ruler["notes"] == [40, 52] and "slope_c_per_oct" not in ruler
    assert ruler["offset_c"] == pytest.approx(3.0)


def test_a_resolved_audible_slope_inside_the_window_is_fitted_beside_the_offset():
    # 6 c/oct of stretch through A4 at -1 c, with a little scatter on top.
    noise = {57: 0.1, 61: -0.1, 65: 0.05, 69: -0.05, 73: 0.1, 77: -0.1, 81: 0.0}
    cents = {n: -1.0 + 0.5 * (n - 69) + e for n, e in noise.items()}
    ruler = a4_ruler(_rows(cents), "t")
    assert ruler["slope_c_per_oct"] == pytest.approx(6.0, abs=0.1)
    assert ruler["offset_c"] == pytest.approx(-1.0, abs=0.05)
    assert ruler["scatter_c"] < 0.2


def test_a_slope_too_small_to_hear_is_left_in_the_median():
    cents = {n: 0.02 * (n - 69) for n in (60, 66, 72, 78)}
    assert "slope_c_per_oct" not in a4_ruler(_rows(cents), "t")


def test_no_pitch_reading_gives_no_ruler():
    assert a4_ruler([{"timbre": "t", "note": 60, "velocity": 100}], "t") is None


def _stretch(median: float) -> dict:
    return {"stretch": {"median": median, "abs_median": median, "p90": median, "n": 6}}


def _ruler(offset: float, scatter: float = 0.2) -> dict:
    return {
        "stretch": {"ruler": {"offset_c": offset, "scatter_c": scatter, "n": 4, "notes": [60, 78]}}
    }


def _write(path: Path, median: float, windows: dict, unrecorded: dict | None = None) -> dict:
    write_gate_file(
        _stretch(median), path, "t", 1.0, windows=windows, unrecorded_windows=unrecorded
    )
    return json.loads(path.read_text())["bounds"]["stretch"]


def test_a_stretch_bound_read_on_another_ruler_is_re_recorded_with_the_old_one_beside_it(tmp_path):
    gate = tmp_path / "g.json"
    _write(gate, 1.0, None)
    bound = _write(gate, 3.0, _ruler(0.5), unrecorded={"stretch": {"ruler": {"offset_c": -0.6}}})
    prov = bound["provenance"]["median"]
    assert bound["median"] == 3.0 and prov["outcome"] == "rewindowed"
    assert prov["rewindowed_from"] == {"bound": 1.0, "window": {"ruler": {"offset_c": -0.6}}}
    assert prov["window"] == _ruler(0.5)["stretch"]


def test_a_ruler_that_did_not_move_keeps_the_stretch_bound_tighten_only(tmp_path):
    gate = tmp_path / "g.json"
    _write(gate, 1.0, _ruler(0.5))
    bound = _write(gate, 3.0, _ruler(0.55))
    assert bound["median"] == 1.0 and bound["provenance"]["median"]["outcome"] == "exceeded"
    assert window_mismatch(_ruler(0.5)["stretch"], _ruler(0.55)["stretch"]) is None


def test_a_stretch_bound_is_never_recorded_below_the_rulers_scatter(tmp_path):
    bound = _write(tmp_path / "g.json", 0.5, _ruler(0.5, scatter=2.5))
    assert bound["floor"] == 2.5 and bound["floor_from"] == FLOOR_FROM_RULER
    assert bound["median"] == 2.5 and bound["provenance"]["median"]["outcome"] == "new"


def test_a_kept_stretch_bound_under_the_rulers_scatter_is_raised_to_it(tmp_path):
    gate = tmp_path / "g.json"
    _write(gate, 1.0, _ruler(0.5, scatter=0.2))
    bound = _write(gate, 1.5, _ruler(0.5, scatter=3.0))
    prov = bound["provenance"]["median"]
    assert bound["median"] == 3.0 and prov["outcome"] == "floored"
    assert prov["floored_from"] == {"bound": 1.0}
    notes = profile_gate.provenance_notes(json.loads(gate.read_text()))
    assert "stretch.median: raised from 1.0 to the A4 ruler's scatter" in notes


def test_a_bound_under_a_measured_spread_floor_is_not_raised(tmp_path):
    gate = tmp_path / "g.json"
    summary = {"decay": {"median": 1.0, "abs_median": 1.0, "p90": 1.0, "n": 6}}
    write_gate_file(summary, gate, "t", 1.0)
    write_gate_file(
        {"decay": {"median": 1.5, "abs_median": 1.5, "p90": 1.5, "n": 6}},
        gate,
        "t",
        1.0,
        spread={"decay": 3.0},
    )
    bound = json.loads(gate.read_text())["bounds"]["decay"]
    assert bound["median"] == 1.0 and bound["provenance"]["median"]["outcome"] == "exceeded"


def test_a_bound_raised_to_the_rulers_scatter_still_reads_red_where_the_model_is_over_it(tmp_path):
    gate = tmp_path / "g.json"
    _write(gate, 1.0, _ruler(0.5, scatter=0.2))
    bound = _write(gate, 4.0, _ruler(0.5, scatter=3.0))
    prov = bound["provenance"]["median"]
    assert bound["median"] == 3.0 and prov["outcome"] == "exceeded"
    assert prov["floored_from"] == {"bound": 1.0}


def test_a_ruler_too_scattered_to_resolve_stretch_reads_not_adjudicable_never_inside(
    tmp_path, capsys
):
    gate = tmp_path / "g.json"
    _write(gate, 0.5, _ruler(0.5, scatter=7.2))
    bounds = json.loads(gate.read_text())["bounds"]
    for value in (1.0, 9.0):
        got = gate_readings(_stretch(value), bounds, _ruler(0.5, scatter=7.2))
        assert {(r["value"], r["over"]) for r in got} == {(value, None)}
        assert all("scatter 7.2 c is wider than 5 c" in r["unadjudicable"] for r in got)
        capsys.readouterr()
        rc = profile_gate.check_gate(_stretch(value), gate, "t", windows=_ruler(0.5, scatter=7.2))
        out = capsys.readouterr().out
        assert rc == 0 and "(cents): median not adjudicable, measurement-limited" in out
        assert "every recorded bound held" not in out and "3 not adjudicable" in out
    found = profile_gate.provenance_findings(json.loads(gate.read_text()))
    assert [(d, s) for d, s, _ in found["unadjudicable"]] == [
        ("stretch", s) for s in ("median", "abs_median", "p90")
    ]
    assert not found["exceeded"] and not found["unverified"]
    notes = profile_gate.provenance_notes(json.loads(gate.read_text()))
    assert "stretch.median: not adjudicable, measurement-limited" in notes[0]
    out, found = gate_verify.apply(json.loads(gate.read_text()), got, "NOW")
    assert found["unadjudicable"] and not found["held"] and not found["exceeded"]
    assert out == json.loads(gate.read_text())


def test_a_ruler_inside_the_limit_is_judged_as_before(tmp_path):
    gate = tmp_path / "g.json"
    _write(gate, 0.5, _ruler(0.5, scatter=4.9))
    bounds = json.loads(gate.read_text())["bounds"]
    got = gate_readings(_stretch(9.0), bounds, _ruler(0.5, scatter=4.9))
    assert all(r["over"] and "unadjudicable" not in r for r in got)
