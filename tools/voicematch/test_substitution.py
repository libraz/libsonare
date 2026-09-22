"""Tests for the substitution control — the grid grouping and the three-way verdict.

Nothing here renders. The arithmetic under test belongs to `profile_gate`, so
what these cover is what `substitution.py` adds on top of it: which captures may
be compared at all, that the number handed to a bound is the median absolute
row delta and not something else, and that a dimension two instruments cannot
compare is separated from one they compare and disagree on.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_substitution.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import substitution


def _profile(deltas: list[float], base: float = -4.0) -> tuple[dict, dict]:
    """Two profiles differing by a known per-row amount on the held-note decay.

    One dimension only: every other field is left out, which the spread treats
    as uncomparable, so the median under test is taken over exactly the numbers
    this function put in.
    """
    notes = [48, 52, 56, 60, 64, 68, 72][: len(deltas)]
    left = [{"timbre": "t", "note": n, "velocity": 80, "decay_db_s": base} for n in notes]
    right = [
        {"timbre": "t", "note": n, "velocity": 80, "decay_db_s": base + d}
        for n, d in zip(notes, deltas)
    ]
    return {"rows": left}, {"rows": right}


def test_grid_of_needs_both_axes():
    assert substitution.grid_of({"notes": [60], "velocities": [80]}) == ((60,), (80,))
    assert substitution.grid_of({"notes": [60]}) is None
    assert substitution.grid_of({"velocities": [80]}) is None


def test_grid_of_is_a_set_rather_than_the_written_order():
    """Two captures naming the same grid in a different order share it."""
    assert substitution.grid_of(
        {"notes": [72, 48], "velocities": [80, 32]}
    ) == substitution.grid_of({"notes": [48, 72], "velocities": [32, 80]})


def test_grid_groups_are_largest_first():
    captures = {
        "a": {"notes": [60], "velocities": [80]},
        "b": {"notes": [60, 64], "velocities": [80]},
        "c": {"notes": [60, 64], "velocities": [80]},
        "d": {"notes": [60, 64], "velocities": [80]},
    }
    groups = substitution.grid_groups(captures)
    assert [len(ids) for ids in groups.values()] == [3, 1]


def test_substitute_is_the_median_absolute_row_delta():
    """The number a bound is compared against, against the same number by hand."""
    deltas = [1.0, -2.0, 3.0, -10.0]
    left, right = _profile(deltas)
    spread = substitution.substitute(left, right, "t", "t")
    assert spread["decay"] == pytest.approx(float(np.median(np.abs(deltas))))


def test_substituting_a_profile_for_itself_reads_zero():
    """The control the rest is read against: no difference means no error."""
    left, _right = _profile([1.0, 2.0])
    spread = substitution.substitute(left, left, "t", "t")
    assert spread["decay"] == pytest.approx(0.0)


def test_rows_of_takes_the_timbre_the_gate_names():
    profile = {
        "rows": [
            {"timbre": "gm001", "note": 60, "velocity": 80},
            {"timbre": "gm002", "note": 60, "velocity": 80},
        ]
    }
    assert [r["timbre"] for r in substitution._rows_of(profile, "gm002")] == ["gm002"]
    # A gate naming a timbre this profile no longer carries falls back rather
    # than returning nothing: an empty side would read as a dimension that could
    # not be measured, which is a different finding from a renamed timbre.
    assert [r["timbre"] for r in substitution._rows_of(profile, "gm999")] == ["gm001"]


def test_judge_separates_a_disagreement_from_a_comparison_that_did_not_happen():
    gate = {
        "bounds": {
            "decay": {"abs_median": 1.0},
            "damper": {"abs_median": 5.0},
            "stereo": {"median": 0.3},
        }
    }
    passed, failed, unreached = substitution.judge({"decay": 4.0}, gate)
    assert passed == set()
    assert failed == {"decay"}
    # `damper` was never measured by this pair and `stereo` has no bound on the
    # column this control reads. Neither is a verdict, so neither is counted as
    # one in either direction.
    assert unreached == {"damper", "stereo"}


def test_judge_holds_a_value_sitting_exactly_on_its_bound():
    gate = {"bounds": {"decay": {"abs_median": 2.0}}}
    passed, failed, _unreached = substitution.judge({"decay": 2.0}, gate)
    assert (passed, failed) == ({"decay"}, set())
    passed, failed, _unreached = substitution.judge({"decay": 2.0001}, gate)
    assert (passed, failed) == (set(), {"decay"})


def test_the_corpus_reaches_a_verdict_and_the_verdict_is_not_uniform():
    """Over the committed corpus: the control is clean and the finding is real.

    Two claims at once, because either alone is worthless. The identity control
    must clear every bound — a run that cannot pass a zero-delta pair is
    measuring its own plumbing — and the substitution rate must sit strictly
    between the two ends, since an instrument that passed everything and one
    that failed everything would both be reported by a broken comparison.
    """
    captures = substitution.gated_captures()
    groups = {g: ids for g, ids in substitution.grid_groups(captures).items() if len(ids) >= 2}
    assert groups, "no two gated captures share a grid"
    grid = next(iter(groups))
    result = substitution.run_group(groups[grid])
    assert result["identity_control"]["clean"] == result["identity_control"]["of"] >= 2
    assert result["comparisons"] > 0
    assert 0.0 < result["mean_pass_rate"] < 1.0


def test_main_exits_zero():
    """Read-only and never a gate, on the same terms as `status.py`."""
    assert substitution.main(["--json"]) == 0
