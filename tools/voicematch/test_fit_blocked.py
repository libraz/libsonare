"""A refit whose trade `resolve` could not settle is recorded `blocked`, with the rows that blocked it."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fits
import refit
import status


def _reading(key, stat, value, limit):
    return {
        "key": key,
        "stat": stat,
        "value": value,
        "limit": limit,
        "over": value > limit,
        "excess": (value - limit) / limit,
    }


def _voice_dir(root: Path, verdict: dict) -> Path:
    d = root / "p040-violin"
    d.mkdir(parents=True)
    gate = root / "violin_gate.json"
    gate.write_text(
        json.dumps(
            {
                "bounds": {"decay": {"median": 1.0, "floor": 0.4}, "tnr": {"p90": 6.0}},
                "reference_spread": {"tnr": 1.5},
            }
        )
    )
    (d / "voice.json").write_text(
        json.dumps(
            {
                "slug": "p040-violin",
                "capture": "violin",
                "timbre": "t0",
                "gates": [{"capture": "violin", "gate": str(gate)}],
            }
        )
    )
    (d / "fit.json").write_text(
        json.dumps(
            {
                "objective": "o1",
                "knobs": [{"key": "violin.bowed_string.bow_force", "start": 1.0}],
                "selected": {"values": [1.2]},
                "loss": {"selected": 0.9},
            }
        )
    )
    (d / "pre.json").write_text(
        json.dumps(
            {
                "gate": {
                    "readings": [
                        _reading("violin:decay", "median", 0.8, 1.0),
                        _reading("violin:tnr", "p90", 5.0, 6.0),
                    ]
                }
            }
        )
    )
    (d / "post.json").write_text(
        json.dumps(
            {
                "gate": {
                    "readings": [
                        _reading("violin:decay", "median", 1.3, 1.0),
                        _reading("violin:tnr", "p90", 6.6, 6.0),
                    ]
                }
            }
        )
    )
    (d / "verdict.json").write_text(json.dumps({"slug": "p040-violin", **verdict}))
    return d


@pytest.fixture
def records(monkeypatch):
    held: dict[str, dict] = {}
    monkeypatch.setattr(refit.fits_mod, "load", lambda: held)
    monkeypatch.setattr(refit.fits_mod, "save", lambda r: held.update(r))
    for name in ("write_patch_fields", "write_family_fields", "write_drum_fields"):
        monkeypatch.setattr(refit, name, lambda *a, **k: {})
    return held


_SETTLED = {
    "decision": "kept",
    "resolved": True,
    "why": "no point of the search kept every bound (newly over ...)",
    "newly_over": ["violin:decay median", "violin:tnr p90"],
    "attempts": [],
}


def test_a_trade_resolve_settled_as_kept_is_recorded_blocked_with_its_rows(tmp_path, records):
    _voice_dir(tmp_path, _SETTLED)
    refit.adopt(tmp_path, set())
    entry = records["violin"]
    assert entry["outcome"] == "blocked" and entry["objective"] == "o1"
    assert entry["blocking"] == [
        {
            "gate": "violin",
            "dimension": "decay",
            "statistic": "median",
            "before": 0.8,
            "after": 1.3,
            "bound": 1.0,
            "floor": 0.4,
        },
        {
            "gate": "violin",
            "dimension": "tnr",
            "statistic": "p90",
            "before": 5.0,
            "after": 6.6,
            "bound": 6.0,
            "floor": 1.5,
        },
    ]


def test_an_unresolved_trade_and_a_stopped_voice_are_not_recorded(tmp_path, records):
    _voice_dir(tmp_path / "a", {**_SETTLED, "decision": "trade", "resolved": False})
    _voice_dir(tmp_path / "b", {"decision": "stopped", "why": "harness moved"})
    refit.adopt(tmp_path / "a", set())
    refit.adopt(tmp_path / "b", set())
    assert records == {}


def test_a_kept_verdict_that_never_traded_stays_kept(tmp_path, records):
    _voice_dir(
        tmp_path, {"decision": "kept", "why": "held-out notes did not improve", "newly_over": []}
    )
    refit.adopt(tmp_path, set())
    assert records["violin"]["outcome"] == "kept" and "blocking" not in records["violin"]


def test_blocked_is_its_own_state_until_a_later_objective_makes_it_stale():
    rows = [
        {
            "gate": "violin",
            "dimension": "decay",
            "statistic": "median",
            "before": 0.8,
            "after": 1.3,
            "bound": 1.0,
            "floor": 0.4,
        }
    ]
    entry = {
        "objective": "o1",
        "date": "2026-11-15",
        "outcome": "blocked",
        "capture": "violin",
        "timbre": "t0",
        "blocking": rows,
    }
    now = fits.axis(["violin"], {"violin": entry}, "o1", equivalents=set())
    assert now["state"] == fits.BLOCKED and now["blocking"] == rows
    later = fits.axis(["violin"], {"violin": entry}, "o2", equivalents=set())
    assert later["state"] == fits.STALE and "blocking" not in later


def test_status_sends_a_blocked_voice_to_the_mechanism_not_to_a_refit():
    from test_status import _axes

    rows = [{"gate": "violin", "dimension": "decay", "statistic": "median"}]
    fit = {"state": fits.BLOCKED, "fittable": True, "blocking": rows}
    step = status.next_action(_axes(fit=fit), 3, [])
    assert "violin:decay" in step and "mechanism" in step and "refit.py" not in step


def test_existing_records_read_as_before_and_blocking_belongs_to_blocked_alone():
    kept = {"objective": "o1", "date": "2026-11-15", "outcome": "kept", "capture": "c"}
    adopted = {**kept, "outcome": "adopted"}
    for e in (kept, adopted):
        got = fits.axis(["u"], {"u": e}, "o1", equivalents=set())
        assert got == {"state": fits.CURRENT, "date": "2026-11-15", "outcome": e["outcome"]}
    report = {"objective": "o1", "knobs": [{"key": "u.x"}]}
    with pytest.raises(ValueError, match="blocking rows"):
        fits.record_fit(report, capture="c", timbre="t", outcome="blocked")
    with pytest.raises(ValueError, match="blocking rows"):
        fits.record_fit(report, capture="c", timbre="t", outcome="kept", blocking=[{"x": 1}])
