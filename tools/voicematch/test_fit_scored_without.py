"""A fit scored with a weighed term switched off is recorded so, and never reads as current."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fits


def _report(**extra):
    return {
        "objective": "o1",
        "knobs": [{"key": "sfx_seashore.va.cutoff_hz", "start": 1.0}],
        "fit_profile": {"weights": {"env": 1.0, "mss": 0.0}},
        **extra,
    }


def test_an_explicit_zero_weight_is_recorded_rather_than_dropped():
    _, entry = fits.record_fit(_report(), capture="seashore", timbre="t0", outcome="adopted")
    assert entry["weights"] == {"env": 1.0, "mss": 0.0}
    assert "scored_without" not in entry


def test_a_fit_scored_without_a_term_is_never_current_and_names_the_term():
    unit, entry = fits.record_fit(
        _report(scored_without=["mss"]), capture="seashore", timbre="t0", outcome="adopted"
    )
    assert entry["scored_without"] == ["mss"]
    got = fits.axis([unit], {unit: entry}, "o1")
    assert got["state"] == fits.SCORED_WITHOUT and got["terms"] == ["mss"]
    full = {k: v for k, v in entry.items() if k != "scored_without"}
    assert fits.axis([unit], {unit: full}, "o1")["state"] == fits.CURRENT


def test_a_later_objective_still_reads_stale_first():
    unit, entry = fits.record_fit(
        _report(scored_without=["mss"]), capture="seashore", timbre="t0", outcome="adopted"
    )
    assert fits.axis([unit], {unit: entry}, "o2", equivalents=set())["state"] == fits.STALE
