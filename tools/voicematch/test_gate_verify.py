"""Tests for the gate verification pass: provenance moves, bounds never do.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_gate_verify.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gate_verify

SET = {"set_utc": "2026-09-10T00:00:00+00:00", "set_build": "B1", "held_utc": None}


def _gate():
    return {
        "reference_measured_utc": "R1",
        "bounds": {
            "decay": {
                "median": 2.0,
                "p90": 4.0,
                "provenance": {"median": dict(SET), "p90": dict(SET)},
            },
            "tnr": {"median": 6.0, "provenance": {"median": dict(SET)}},
        },
    }


def _reading(key, stat, value, limit):
    return {"key": key, "stat": stat, "value": value, "limit": limit, "over": None}


def test_a_reading_inside_holds_and_one_outside_is_exceeded():
    readings = [
        _reading("decay", "median", 1.5, 2.0),
        _reading("decay", "p90", 5.0, 4.0),
        _reading("tnr", "median", None, 6.0),
    ]
    out, found = gate_verify.apply(_gate(), readings, "NOW")
    held = out["bounds"]["decay"]["provenance"]["median"]
    over = out["bounds"]["decay"]["provenance"]["p90"]
    assert (held["held_utc"], held["outcome"], held["measured"]) == ("NOW", "kept", 1.5)
    assert (over["held_utc"], over["outcome"], over["measured"]) == (None, "exceeded", 5.0)
    assert found == {
        "held": ["decay.median"],
        "exceeded": ["decay.p90"],
        "unmeasured": ["tnr.median"],
        "unadjudicable": [],
    }
    # The unmeasured statistic is left exactly as it was.
    assert out["bounds"]["tnr"] == _gate()["bounds"]["tnr"]


def test_no_bound_value_and_no_set_stamp_ever_moves():
    readings = [_reading("decay", "median", 9.0, 2.0), _reading("decay", "p90", 0.1, 4.0)]
    out, _ = gate_verify.apply(_gate(), readings, "NOW")
    for dim, bound in _gate()["bounds"].items():
        for k, v in bound.items():
            if k != "provenance":
                assert out["bounds"][dim][k] == v
        for stat, p in bound["provenance"].items():
            got = out["bounds"][dim]["provenance"][stat]
            assert (got["set_utc"], got["set_build"]) == (p["set_utc"], p["set_build"])


def test_a_run_against_another_gate_generation_is_skipped(tmp_path):
    gate = tmp_path / "x_gate.json"
    gate.write_text(json.dumps(_gate()))
    runs = tmp_path / "runs"
    runs.mkdir()
    body = {"gate": {"path": str(gate), "reference_measured_utc": "R0", "readings": []}}
    (runs / "x.json").write_text(json.dumps(body))
    report = gate_verify.verify(runs, write=True, now="NOW")
    assert "skipped" in report["x_gate.json"]
    body["gate"]["reference_measured_utc"] = "R1"
    body["gate"]["readings"] = [_reading("decay", "median", 1.0, 2.0)]
    (runs / "x.json").write_text(json.dumps(body))
    gate_verify.verify(runs, write=True, now="NOW")
    written = json.loads(gate.read_text())
    assert written["bounds"]["decay"]["provenance"]["median"]["held_utc"] == "NOW"
