"""Tests for the before/after gate audit of a finished fit, on synthetic readings.

python -m pytest tools/voicematch/test_gate_audit.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import gate_audit
import refit


def _body(*readings):
    return {
        "gate": {
            "readings": [
                {"key": k, "stat": s, "value": v, "limit": lim, "over": False, "excess": None}
                for k, s, v, lim in readings
            ]
        }
    }


def _state(before, after, bound=10.0):
    (e,) = gate_audit.audit(
        _body(("c:x", "median", before, bound)), _body(("c:x", "median", after, bound)), None
    )
    return e["state"]


def test_every_state():
    assert _state(5.0, 12.0) == "crossed"
    assert _state(12.0, 15.0) == "worse_over"
    assert _state(12.0, 5.0) == "cleared"
    assert _state(12.0, 11.0) == "better"
    assert _state(5.0, 4.0) == "better"
    assert _state(4.0, 5.0) == "worse_within"
    assert _state(5.0, 5.0) == "unchanged"
    assert _state(12.0, 12.0) == "unchanged"


def test_an_unweighted_dimension_crossing_its_bound_is_listed():
    pre = _body(("pad:balance", "median", 9.7, 12.0), ("pad:tnr", "median", 3.0, 6.0))
    post = _body(("pad:balance", "median", 38.0, 12.0), ("pad:tnr", "median", 2.0, 6.0))
    entries = gate_audit.audit(pre, post, {"tnr"})
    found = gate_audit.summary(entries)["crossed"]
    assert [(e["key"], e["weighted"], e["delta"]) for e in found] == [
        ("pad:balance", False, 38.0 - 9.7)
    ]
    assert "UNWEIGHTED" in "\n".join(gate_audit.format_flagged(entries))


def test_weighted_is_none_without_a_mapping():
    (e,) = gate_audit.audit(_body(("a", "median", 1, 2)), _body(("a", "median", 1, 2)), None)
    assert e["weighted"] is None


def test_a_reading_missing_on_one_side_is_unmeasured():
    pre = _body(("a", "median", 1.0, 2.0))
    post = _body(("a", "median", 1.0, 2.0), ("b", "median", 3.0, 2.0))
    entries = {e["key"]: e for e in gate_audit.audit(pre, post, set())}
    assert entries["b"]["state"] == "unmeasured" and entries["b"]["missing"] == "before"
    nul = _body(("a", "median", None, 2.0))
    (e,) = gate_audit.audit(pre, nul, set())
    assert e["state"] == "unmeasured" and e["missing"] == "after"


def test_the_term_table_marks_weighted_dimensions():
    got = gate_audit.weighted_dimensions({"harm": 1.0, "tnr": 0.0}, refit.GATE_TERMS)
    assert "balance" in got and "tnr" not in got


def _args(tmp_path, **kw):
    out = tmp_path / "fit.json"
    base = {
        "out": str(out),
        "diagnose": False,
        "dump_knobs": False,
        "grid": 0,
        "program": 90,
        "bank": 0,
        "drum_note": None,
    }
    return argparse.Namespace(**{**base, **kw})


def _skipped(args):
    autofit._fold_gate_audit(args, Path("lib"), compare=lambda *a: None)
    return json.loads(Path(args.out).read_text())["gate_audit"]["skipped"]


def test_skip_reasons(tmp_path, monkeypatch):
    args = _args(tmp_path, out="")
    autofit._fold_gate_audit(args, Path("lib"))  # no --out: nothing to write, nothing raised
    out = tmp_path / "fit.json"
    out.write_text("{}")
    assert "--diagnose" in _skipped(_args(tmp_path, diagnose=True))
    assert "--dump-knobs" in _skipped(_args(tmp_path, dump_knobs=True))
    assert "--grid" in _skipped(_args(tmp_path, grid=3))
    monkeypatch.setattr(autofit, "_gate_voice", lambda a: (None, "no gate resolves to this voice"))
    assert _skipped(_args(tmp_path)) == "no gate resolves to this voice"


def test_a_finished_run_records_the_audit(tmp_path, monkeypatch):
    voice = {"slug": "v", "patch": "p", "gates": [{"capture": "c", "gate": "g", "timbre": "t"}]}
    monkeypatch.setattr(autofit, "_gate_voice", lambda a: (voice, ""))
    monkeypatch.setattr(autofit, "dylib_path", lambda b: Path("lib.dylib"))
    out = tmp_path / "fit.json"
    out.write_text(json.dumps({"overrides": "p.x=1", "fit_profile": {"weights": {"tnr": 1.0}}}))
    seen = []

    def compare(v, path, lib, overrides=""):
        seen.append(overrides)
        value = 38.0 if overrides else 9.7
        path.write_text(json.dumps(_body(("c:balance", "median", value, 12.0))))

    autofit._fold_gate_audit(_args(tmp_path), Path("lib"), compare=compare)
    rec = json.loads(out.read_text())["gate_audit"]
    assert seen == ["", "p.x=1"]
    assert rec["gates"] == ["c"] and rec["lib"] == "lib.dylib"
    assert [e["key"] for e in rec["crossed"]] == ["c:balance"] and rec["worse_over"] == []
    assert rec["entries"][0]["weighted"] is False
    assert (tmp_path / "fit.json.gate_audit").is_dir()


def test_refit_reads_through_the_moved_functions():
    assert refit.gate_audit is gate_audit
    assert callable(gate_audit.compare) and callable(gate_audit.compare_voice)
    assert not hasattr(refit, "_compare") and not hasattr(refit, "_compare_voice")
