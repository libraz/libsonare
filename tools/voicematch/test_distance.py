"""Tests for the distance table — the common ruler, the censor count and the ranking.

Nothing here renders: a compare summary is written by hand, so each test knows
the residual it should read back.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_distance.py -q
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import distance
from profile_gate import FLOOR_GUESSES, GENERIC_FLOOR


def _stat(abs_median: float, n: int, median: float | None = None) -> dict:
    return {
        "median": abs_median if median is None else median,
        "abs_median": abs_median,
        "p90": 2.0 * abs_median,
        "n": n,
    }


def test_units_come_from_the_labels():
    assert distance.unit_of("stretch") == "cents"
    assert distance.unit_of("decay") == "dB/s"
    assert distance.unit_of("attack") == "ms"
    # A sign convention in parentheses is not a unit.
    assert distance.unit_of("stereo") == "?"
    assert distance.unit_of("no-such-dimension") == "?"


def test_brightness_reads_in_octaves():
    assert distance.octaves_from_pct(100.0) == pytest.approx(1.0)
    assert distance.octaves_from_pct(-50.0) == pytest.approx(-1.0)
    assert distance.octaves_from_pct(0.0) == 0.0
    assert distance.octaves_from_pct(-100.0) is None
    assert distance.octaves_from_pct(None) is None


def test_a_measured_spread_outranks_the_fallback_floor():
    dims = distance.dimension_rows({"decay": _stat(2.0, 10)}, {"decay": 4.0}, 10, 5)
    assert dims[0]["floors"] == pytest.approx(0.5)
    assert dims[0]["floor_source"] == "measured spread"
    dims = distance.dimension_rows({"decay": _stat(2.0, 10)}, {}, 10, 5)
    assert dims[0]["floors"] == pytest.approx(2.0 / FLOOR_GUESSES["decay"])
    assert dims[0]["floor_source"] == "guess"
    dims = distance.dimension_rows({"made_up": _stat(3.0, 10)}, {}, 10, 5)
    assert dims[0]["floors"] == pytest.approx(3.0 / GENERIC_FLOOR)
    assert dims[0]["floor_source"] == "default"


def test_censored_rows_are_counted_against_the_right_population():
    summary = {"decay": _stat(1.0, 7), "vel_range": _stat(1.0, 4)}
    dims = {d["dimension"]: d for d in distance.dimension_rows(summary, {}, 12, 4)}
    # A grid dimension is held to every cell, a per-note one to every note.
    assert (dims["decay"]["expected"], dims["decay"]["censored"]) == (12, 5)
    assert (dims["vel_range"]["expected"], dims["vel_range"]["censored"]) == (4, 0)


def test_an_empty_dimension_scores_nothing_rather_than_zero():
    dims = distance.dimension_rows({"decay": _stat(0.0, 0)}, {}, 12, 4)
    assert dims[0]["floors"] is None
    assert dims[0]["censored"] == 12
    assert distance.voice_distance(dims) is None


def test_ranking_separates_a_match_from_a_miss():
    """The control: a model at the reference reads 0, one off by k floors reads k."""
    spread = {"decay": 1.0, "stretch": 1.0}
    match = distance.dimension_rows(
        {"decay": _stat(0.0, 9), "stretch": _stat(0.0, 9)}, spread, 9, 3
    )
    miss = distance.dimension_rows({"decay": _stat(3.0, 9), "stretch": _stat(4.0, 9)}, spread, 9, 3)
    assert distance.voice_distance(match) == 0.0
    assert distance.voice_distance(miss) == pytest.approx(3.5)
    assert distance.voice_rms(miss) == pytest.approx(math.sqrt((9 + 16) / 2))
    assert [d["dimension"] for d in miss] == ["stretch", "decay"]


def test_every_capture_reaches_a_voice():
    rows = json.loads(distance.STATUS_JSON.read_text())["voices"]
    served = distance.serving(rows)
    assert set(served) == set(distance.capture_ids())
    assert all(voices for voices, _ in served.values())


def test_table_reads_a_result_back(tmp_path: Path):
    cap = "piano"
    timbre = distance.timbre_for(cap)
    (tmp_path / f"{cap}.json").write_text(
        json.dumps(
            {"timbre": timbre, "summary": {"decay": _stat(1.0, 3)}, "gate": {"readings": []}}
        )
    )
    result = distance.table(tmp_path, {"piano": "keys"})
    (entry,) = result["captures"]
    assert entry["capture"] == cap
    assert entry["group"] == "keys"
    assert entry["censored_rows"] == entry["dimensions"][0]["expected"] - 3
    assert cap not in result["not_run"]
    assert "## keys" in distance.markdown(result)


def test_a_velocity_swing_row_carries_the_recordings_behind_it():
    layers = {"independent": 4, "velocities": 12, "notes": 4, "collapsed_notes": [60]}
    summary = {"vel_range": _stat(1.0, 4), "decay": _stat(1.0, 12)}
    dims = {d["dimension"]: d for d in distance.dimension_rows(summary, {}, 12, 4, layers)}
    assert dims["vel_range"]["layers"] == {"independent": 4, "velocities": 12}
    assert "layers" not in dims["decay"]
