"""Tests for the refit driver's adoption verdict, the fit registry and the objective fingerprint.

python -m pytest tools/voicematch/test_refit.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fits
import objective
from profile_gate import gate_readings
from refit import HELD_OUT_MARGIN, verdict

SWITCH = "p.one_shot"  # declared I_TYPED in the override layer, so a switch field
FLOAT = "p.gain"

# Pairwise over the six inputs (coverwise, seed 7), then each guard on its own
# against an otherwise adoptable fit, and the adoptable fit itself.
PAIRWISE = [
    (False, False, "better", True, False, True),
    (True, False, "none", False, True, False),
    (False, True, "flat", True, True, False),
    (True, False, "flat", True, False, False),
    (False, True, "none", True, True, True),
    (True, True, "better", False, False, True),
    (False, False, "flat", False, True, True),
    (True, False, "better", False, False, False),
    (False, True, "better", True, True, True),
    (True, True, "none", False, False, False),
]
GOOD = (False, True, "better", False, False, False)
SINGLE = [
    (True, True, "better", False, False, False),
    (False, False, "better", False, False, False),
    (False, True, "none", False, False, False),
    (False, True, "flat", False, False, False),
    (False, True, "better", True, False, False),
    (False, True, "better", False, True, False),
    (False, True, "better", False, False, True),
]


def _case(refused, moved, held, switch, newly, grew):
    knobs = [{"key": FLOAT, "start": 1.0}, {"key": SWITCH, "start": 0.0}]
    values = [2.0 if moved else 1.0, 1.0 if switch else 0.0]
    gains = {"none": None, "flat": HELD_OUT_MARGIN / 2, "better": 0.05}
    validation = (
        {"independent": False}
        if gains[held] is None
        else {"independent": True, "start": 1.0, "best": 1.0 - gains[held]}
    )
    fit = {
        "knobs": knobs,
        "selected": {"values": values},
        "loss": {"selected": 0.9},
        "validation": validation,
        "write_back": {"refused": "objective_went_blind" if refused else None},
    }
    over = {"key": "decay", "stat": "p90", "value": 2.0, "limit": 1.0, "over": True, "excess": 1.0}
    pre = {"gate": {"readings": [dict(over)]}}
    post_readings = [dict(over, excess=1.5 if grew else 1.0, value=2.5 if grew else 2.0)]
    if newly:
        post_readings.append(
            {
                "key": "attack",
                "stat": "median",
                "value": 3.0,
                "limit": 2.0,
                "over": True,
                "excess": 0.5,
            }
        )
    return pre, fit, {"gate": {"readings": post_readings}}


@pytest.mark.parametrize("row", PAIRWISE + SINGLE + [GOOD])
def test_a_fit_is_adopted_only_when_every_guard_holds(row):
    refused, moved, held, switch, newly, grew = row
    result = verdict(*_case(*row))
    # A switch landing in another bucket is a moved value too.
    generalises = not refused and (moved or switch) and held == "better"
    if generalises and not (switch or newly or grew):
        assert result["decision"] == "adopt"
    elif generalises and (switch or newly):
        assert result["decision"] == "trade"
    else:
        assert result["decision"] == "kept"


def test_gate_readings_mirror_the_gate_comparison():
    summary = {"decay": {"median": -3.0, "abs_median": 3.0, "p90": 5.0, "n": 10}}
    bounds = {"decay": {"median": 2.0, "p90": 6.0}, "attack": {"median": 1.0}}
    rows = {(r["key"], r["stat"]): r for r in gate_readings(summary, bounds)}
    assert rows[("decay", "median")]["value"] == 3.0  # the signed median is held by magnitude
    assert rows[("decay", "median")]["over"] and rows[("decay", "median")]["excess"] == 0.5
    assert not rows[("decay", "p90")]["over"]
    assert rows[("attack", "median")]["over"] and rows[("attack", "median")]["value"] is None


def test_the_fit_axis_is_stale_when_the_objective_moved():
    records = {"piano": {"objective": "old", "date": "2026-09-26", "outcome": "adopted"}}
    assert fits.axis(["piano"], records, "old")["state"] == fits.CURRENT
    assert fits.axis(["piano"], records, "new")["state"] == fits.STALE
    assert fits.axis(["organ"], records, "new")["state"] == fits.NONE
    assert fits.axis([], records, "new")["state"] == fits.NONE


def test_a_record_names_the_one_patch_its_knobs_belong_to():
    report = {"objective": "abc", "knobs": [{"key": "violin.gain"}, {"key": "violin.drive"}]}
    unit, entry = fits.record_fit(
        report,
        capture="violin",
        timbre="t",
        outcome="kept",
        reason="no value moved",
        today="2026-10-15",
    )
    assert unit == "violin" and entry["objective"] == "abc" and entry["reason"]
    with pytest.raises(ValueError):
        fits.record_fit(
            {"objective": "abc", "knobs": [{"key": "a.x"}, {"key": "b.x"}]},
            capture="c",
            timbre="t",
            outcome="kept",
        )
    with pytest.raises(ValueError):
        fits.record_fit({"knobs": [{"key": "a.x"}]}, capture="c", timbre="t", outcome="kept")


def test_the_fingerprint_ignores_prose_and_sees_code(tmp_path):
    def digest(body: str) -> str:
        for name in objective.ROOTS:
            (tmp_path / f"{name}.py").write_text("x = 1\n")
        (tmp_path / "loss.py").write_text("import helper\n" + body)
        (tmp_path / "helper.py").write_text("def h():\n    return 1\n")
        return objective.objective_digest(tmp_path)

    base = digest('def f():\n    """Doc — one."""\n    return 2  # note\n')
    assert (
        digest('def f():\n    """Doc — rewritten\n    over two lines."""\n\n    return 2\n') == base
    )
    assert digest('def f():\n    """Doc — one."""\n    return 3  # note\n') != base
    assert "helper" in objective.objective_modules(tmp_path)
    (tmp_path / "helper.py").write_text("def h():\n    return 2\n")
    assert objective.objective_digest(tmp_path) != base, "an imported module is part of it"


def test_a_trade_is_refitted_with_the_broken_dimensions_weighed_up():
    from refit import reweighed

    report = {"fit_profile": {"weights": {"tnr": 0.5, "harm": 1.0, "env": 1.0}}}
    weights = reweighed(report, ["tnr p90", "balance abs_median", "damper median"])
    assert weights == {"tnr": 1.0, "harm": 2.0, "env": 2.0}
    assert reweighed(report, ["tnr p90"]) == {"tnr": 1.0}
    assert reweighed(report, ["stereo median"]) is None, "a dimension with no term has no refit"


def test_a_grid_with_one_condition_per_note_is_judged_by_the_gate_alone():
    pre, fit, post = _case(False, True, "none", False, False, False)
    assert verdict(pre, fit, post)["decision"] == "kept"
    assert verdict(pre, fit, post, gate_only=True)["decision"] == "adopt"
    pre, fit, post = _case(False, True, "none", False, True, False)
    assert verdict(pre, fit, post, gate_only=True)["decision"] == "trade"


def test_a_kit_trade_is_reweighed_with_the_percussion_terms():
    from refit import reweighed

    report = {"fit_profile": {"weights": {"band": 1.0, "ring": 0.5}}}
    weights = reweighed(
        report, ["drums_module_hit:level p90", "drums_module:band_shape median"], percussive=True
    )
    assert weights == {"band": 2.0}
    assert reweighed(report, ["drums_module_hit:level p90"], percussive=True) is None


@pytest.mark.parametrize("last_good", [-1, 0, 3, 6, 9])
def test_the_furthest_point_the_gate_accepts_is_found_by_bisection(
    tmp_path, monkeypatch, last_good
):
    import json as _json

    import refit

    over = {"key": "tnr", "stat": "p90", "limit": 1.0}
    (tmp_path / "pre.json").write_text(
        _json.dumps({"gate": {"readings": [dict(over, value=0.9, over=False, excess=-0.1)]}})
    )
    path = [
        {"eval": i + 1, "loss": 1.0 - i / 20, "stage": "fit", "values": [float(i)]}
        for i in range(10)
    ]
    report = {"knobs": [{"key": FLOAT, "start": -1.0}], "path": path}
    compared = []

    def fake_compare(voice, out, lib, overrides):
        i = int(float(overrides.split("=")[1]))
        compared.append(i)
        value = 0.8 - i / 100 if i <= last_good else 1.5  # past the crossing, the bound breaks
        out.write_text(
            _json.dumps(
                {
                    "gate": {
                        "readings": [dict(over, value=value, over=value > 1.0, excess=value - 1.0)]
                    }
                }
            )
        )

    monkeypatch.setattr(refit, "_compare_voice", fake_compare)
    found = refit.along_the_path({}, tmp_path, Path("lib"), report, "fit")
    if last_good < 0:
        assert found["accepted"] is None
    else:
        assert found["accepted"]["eval"] == last_good + 1
    assert len(compared) <= 5, "bisection, not a walk"


def test_a_first_round_that_fails_alike_stops_the_pass():
    from concurrent.futures import ThreadPoolExecutor

    import refit

    def boom():
        raise RuntimeError("refusing to fit with src/ dirty")

    with ThreadPoolExecutor(max_workers=1) as pool:
        futures = {pool.submit(boom): "a", pool.submit(boom): "b"}
        assert refit._drain(futures, 2) == 1
    with ThreadPoolExecutor(max_workers=1) as pool:
        futures = {
            pool.submit(boom): "a",
            pool.submit(lambda: {"decision": "kept", "why": "nothing"}): "b",
            pool.submit(boom): "c",
        }
        assert refit._drain(futures, 2) == 0


def test_a_dimension_two_classes_read_differently_raises_the_term_the_class_weighs():
    from refit import reweighed

    modal = {"fit_profile": {"weights": {"modedyn": 1.0, "hfdyn": 0.0}}}
    assert reweighed(modal, ["attack_hf_swing median"]) == {"modedyn": 2.0}
    struck = {"fit_profile": {"weights": {"hfdyn": 1.0}}}
    assert reweighed(struck, ["attack_hf_swing median"]) == {"hfdyn": 2.0}
    assert reweighed({"fit_profile": {"weights": {}}}, ["attack_hf_swing p90"]) == {"hfdyn": 1.0}


def test_a_reweighed_round_the_fit_refuses_keeps_the_shipped_values(tmp_path, monkeypatch):
    import json as _json

    import refit

    reason = "ValueError: --w-tail reads the 2-6 s decay band, and the longest note is 1 s"
    (tmp_path / "verdict.json").write_text(
        _json.dumps(
            {
                "decision": "trade",
                "why": "newly over its bound: aftersound median",
                "newly_over": ["aftersound median"],
            }
        )
    )
    (tmp_path / "voice.json").write_text(_json.dumps({"slug": "p045"}))
    (tmp_path / "fit.json").write_text(_json.dumps({"fit_profile": {"weights": {"harm": 1.0}}}))

    def refusing_fit(voice, out, build_dir, per_knob, weights=None):
        out.with_suffix(".log").write_text(f"Traceback\n  ...\n{reason}\n")
        raise RuntimeError("the fit wrote no report")

    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    monkeypatch.setattr(refit, "along_the_path", lambda *a: {"accepted": None})
    monkeypatch.setattr(refit, "_fit", refusing_fit)
    ver = refit.resolve_voice(tmp_path, "build", 8)
    assert ver["decision"] == "kept"
    assert ver["attempts"][-1] == {
        "fit": "fit_r1.json",
        "weights": {"tail": 1.0},
        "refused": reason,
    }
