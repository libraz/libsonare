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
    entries = gate_audit.audit(pre, post, {"tnr": (gate_audit.WEIGHTED, ("tnr",))})
    found = gate_audit.summary(entries)["crossed"]
    assert [(e["key"], e["weighted"], e["delta"]) for e in found] == [
        ("pad:balance", gate_audit.UNWEIGHTED, 38.0 - 9.7)
    ]
    assert "UNWEIGHTED" in "\n".join(gate_audit.format_flagged(entries))


def test_weighted_is_none_without_a_mapping():
    (e,) = gate_audit.audit(_body(("a", "median", 1, 2)), _body(("a", "median", 1, 2)), None)
    assert e["weighted"] is None


def test_a_reading_missing_on_one_side_is_unmeasured():
    pre = _body(("a", "median", 1.0, 2.0))
    post = _body(("a", "median", 1.0, 2.0), ("b", "median", 3.0, 2.0))
    entries = {e["key"]: e for e in gate_audit.audit(pre, post, {})}
    assert entries["b"]["state"] == "unmeasured" and entries["b"]["missing"] == "before"
    nul = _body(("a", "median", None, 2.0))
    (e,) = gate_audit.audit(pre, nul, {})
    assert e["state"] == "unmeasured" and e["missing"] == "after"


def _weighing(weights):
    return gate_audit.weighting(weights, refit.GATE_TERMS, refit.GATE_OWN_TERMS)


def test_a_mapped_term_reading_another_quantity_is_a_proxy():
    got = _weighing({"harm": 1.0, "tnr": 0.0})
    assert got["balance"] == (gate_audit.PROXY, ("harm",))
    # Centroid has a term of its own, so the ladder no longer stands in for it.
    assert "centroid_pct" not in got
    assert "tnr" not in got


def test_the_term_reading_the_dimension_itself_is_weighted():
    got = _weighing({"tnr": 1.0, "bright": 5.0, "harm": 1.0})
    assert got["tnr"] == (gate_audit.WEIGHTED, ("tnr",))
    # The own term outranks the proxy mapped beside it.
    assert got["centroid_pct"] == (gate_audit.WEIGHTED, ("bright",))


def test_a_one_to_many_entry_names_its_term():
    got = _weighing({"modedyn": 0.5})
    assert got["attack_hf_swing"] == (gate_audit.PROXY, ("modedyn",))
    got = _weighing({"hfdyn": 1.0, "modedyn": 0.5})
    assert got["attack_hf_swing"] == (gate_audit.WEIGHTED, ("hfdyn",))


def test_the_pad_case_reads_proxy_and_unweighted():
    """The pad fit weighed harm and not velspan: balance went 9.7 -> 38.4, vel_range 0.6 -> 14."""
    weights = {"harm": 1.0, "cents": 0.5, "tnr": 1.0, "mod": 1.0, "env": 0.5, "slope": 0.5}
    pre = _body(("pad_m:balance", "median", 9.73, 12.16), ("pad:vel_range", "median", 0.58, 1.0))
    post = _body(("pad_m:balance", "median", 38.37, 12.16), ("pad:vel_range", "median", 13.95, 1.0))
    text = "\n".join(gate_audit.format_flagged(gate_audit.audit(pre, post, _weighing(weights))))
    assert "pad_m:balance median: 9.73 -> 38.37 vs bound 12.16 (proxy via harm)" in text
    assert "pad:vel_range median: 0.58 -> 13.95 vs bound 1 (UNWEIGHTED)" in text


def test_an_unclassified_percussion_mapping_reads_unknown():
    own = refit.PERCUSSION_GATE_OWN_TERMS
    got = gate_audit.weighting(
        {"band": 1.0, "velspan": 1.0}, refit.PERCUSSION_GATE_TERMS, own, classified=set(own)
    )
    assert got["band_tilt"] == (None, ("band",))
    assert got["vel_range"] == (gate_audit.WEIGHTED, ("velspan",))


def test_a_percussion_mapping_measured_as_a_proxy_reads_proxy():
    from loss_weights import PERCUSSION_GATE_PROXIES

    own = refit.PERCUSSION_GATE_OWN_TERMS
    got = gate_audit.weighting(
        {"band": 1.0, "ring": 1.0},
        refit.PERCUSSION_GATE_TERMS,
        own,
        classified=set(own) | PERCUSSION_GATE_PROXIES,
    )
    assert got["band_tilt"] == (gate_audit.PROXY, ("band",))
    assert got["band_shape"] == (gate_audit.WEIGHTED, ("band",))
    assert got["ring"] == (gate_audit.PROXY, ("ring",))


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
    autofit._fold_gate_audit(args, Path("lib"), compare=lambda *a, **k: None)
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
    monkeypatch.setattr(autofit, "library_path", lambda b: Path("lib.dylib"))
    out = tmp_path / "fit.json"
    out.write_text(json.dumps({"overrides": "p.x=1", "fit_profile": {"weights": {"tnr": 1.0}}}))
    seen = []

    def compare(v, path, lib, overrides="", base_overrides=""):
        seen.append(overrides)
        value = 38.0 if overrides else 9.7
        path.write_text(json.dumps(_body(("c:balance", "median", value, 12.0))))

    autofit._fold_gate_audit(_args(tmp_path), Path("lib"), compare=compare)
    rec = json.loads(out.read_text())["gate_audit"]
    assert seen == ["", "p.x=1"]
    assert rec["gates"] == ["c"] and rec["lib"] == "lib.dylib"
    assert [e["key"] for e in rec["crossed"]] == ["c:balance"] and rec["worse_over"] == []
    assert rec["entries"][0]["weighted"] == gate_audit.UNWEIGHTED
    assert (tmp_path / "fit.json.gate_audit").is_dir()


def test_refit_reads_through_the_moved_functions():
    assert refit.gate_audit is gate_audit
    assert callable(gate_audit.compare) and callable(gate_audit.compare_voice)
    assert not hasattr(refit, "_compare") and not hasattr(refit, "_compare_voice")


def test_the_audit_block_counts_every_state_even_when_nothing_crossed():
    pre = _body(("a", "median", 1.0, 2.0))
    post = _body(("a", "median", 0.5, 2.0))
    lines = gate_audit.format_flagged(gate_audit.audit(pre, post, {}))
    assert (
        lines[1]
        == "  states: crossed 0, worse_over 0, cleared 0, better 1, worse_within 0, unchanged 0, unmeasured 0"
    )


def test_a_flagged_entry_prints_its_own_window():
    short = {"model_s": 0.6, "reference_s": 0.6}
    long = {"model_s": 2.0, "reference_s": 2.0}

    def body(a, b):
        return {
            "gate": {
                "readings": [
                    {
                        "key": "module:decay",
                        "stat": "p90",
                        "value": a,
                        "limit": 1.0,
                        "window": short,
                    },
                    {
                        "key": "library:decay",
                        "stat": "p90",
                        "value": b,
                        "limit": 1.0,
                        "window": long,
                    },
                    {"key": "module:stretch", "stat": "p90", "value": a, "limit": 1.0},
                ]
            }
        }

    entries = gate_audit.audit(body(0.5, 0.5), body(2.0, 2.0), None)
    text = "\n".join(gate_audit.format_flagged(entries))
    by_key = {e["key"]: e for e in entries}
    assert by_key["module:decay"]["window"] == short
    assert "window" not in by_key["module:stretch"]
    assert (
        "module:decay p90: 0.5 -> 2 vs bound 1 (weighting unknown) [window 0.6 s / 0.6 s]" in text
    )
    assert "library:decay p90: 0.5 -> 2 vs bound 1 (weighting unknown) [window 2 s / 2 s]" in text
    assert "module:stretch p90: 0.5 -> 2 vs bound 1 (weighting unknown)\n" in text + "\n"


def test_compare_voice_carries_each_gates_windows_onto_its_readings(tmp_path, monkeypatch):
    def fake_compare(voice, out, lib, overrides="", **_):
        span = 0.6 if voice["capture"] == "module" else 2.0
        windows = {"decay": {"model_s": span, "reference_s": span}}
        readings = [
            {"key": k, "stat": "p90", "value": 1.0, "limit": 1.0, "over": False, "excess": 0.0}
            for k in ("decay", "stretch")
        ]
        out.write_text(json.dumps({"gate": {"readings": readings}, "windows": windows}))

    monkeypatch.setattr(gate_audit, "compare", fake_compare)
    voice = {"slug": "v", "gates": [{"capture": "module"}, {"capture": "library"}]}
    out = tmp_path / "pre.json"
    gate_audit.compare_voice(voice, out, Path("lib"))
    got = {r["key"]: r.get("window") for r in json.loads(out.read_text())["gate"]["readings"]}
    assert got["module:decay"]["model_s"] == 0.6 and got["library:decay"]["model_s"] == 2.0
    assert got["module:stretch"] is None


def test_compare_voice_keeps_each_gates_per_note_rows_by_capture(tmp_path, monkeypatch):
    def fake_compare(voice, out, lib, overrides="", **_):
        body = {"gate": {"readings": []}}
        if voice["capture"] == "hit":
            body["rows"] = [{"note": 71, "velocity": 100, "crest": 3.1}]
        out.write_text(json.dumps(body))

    monkeypatch.setattr(gate_audit, "compare", fake_compare)
    voice = {"slug": "v", "gates": [{"capture": "hit"}, {"capture": "mid"}]}
    out = tmp_path / "pre.json"
    gate_audit.compare_voice(voice, out, Path("lib"))
    got = json.loads(out.read_text())["note_rows"]
    assert got == {"hit": [{"note": 71, "velocity": 100, "crest": 3.1}]}
