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


def _capture(cid, program, bank, *, drums=False, rig=None):
    from types import SimpleNamespace

    raw = {"id": cid} if rig is None else {"id": cid, "rig": rig}
    return SimpleNamespace(id=cid, program=program, bank=bank, drums=drums, raw=raw)


def test_a_capital_is_judged_by_every_gate_its_patch_reaches(tmp_path):
    import json

    import boundary
    from refit import capital_gates

    for cid in (
        "harpsichord",
        "harpsichord_alt",
        "harpsichord_lsb",
        "harpsichord_keyoff",
        "harpsichord_baked",
        "drums",
    ):
        (tmp_path / f"{cid}_gate.json").write_text(json.dumps({"timbre": f"t-{cid}"}))
    rows = [
        {"program": 6, "bank": 0, "kit": False, "patch": "harpsichord", "capture": "harpsichord"},
        {"program": 6, "bank": 3, "kit": False, "patch": "harpsichord"},
        {"program": 6, "bank": 24, "kit": False, "patch": "harpsichord_keyoff"},
    ]
    pool = [
        _capture("harpsichord_alt", 6, 0),  # a second recording at the same address
        _capture("harpsichord", 6, 0),
        _capture("harpsichord_lsb", 6, 3),  # the patch's other bank address
        _capture("harpsichord_keyoff", 6, 24),  # a variation: its own patch
        _capture("harpsichord_baked", 6, 0, rig=boundary.RIG_BAKED),  # not at the instrument
        _capture("drums", 6, 0, drums=True),
        _capture("unwritten", 6, 0),  # no gate file
    ]
    gates = capital_gates(rows[0], rows, pool, tmp_path, copies={})
    assert [g["capture"] for g in gates] == ["harpsichord", "harpsichord_alt", "harpsichord_lsb"]
    assert gates[1]["timbre"] == "t-harpsichord_alt"


def test_a_capital_is_judged_by_the_gates_of_the_patches_copied_from_it(tmp_path):
    import json

    from refit import capital_gates

    for cid in ("sitar", "koto", "koto_wide", "harp"):
        (tmp_path / f"{cid}_gate.json").write_text(json.dumps({"timbre": f"t-{cid}"}))
    rows = [
        {"program": 104, "bank": 0, "kit": False, "patch": "sitar", "capture": "sitar"},
        {"program": 107, "bank": 0, "kit": False, "patch": "koto"},
        {"program": 107, "bank": 8, "kit": False, "patch": "koto_wide"},
        {"program": 46, "bank": 0, "kit": False, "patch": "harp"},
    ]
    pool = [
        _capture("sitar", 104, 0),
        _capture("koto", 107, 0),  # copied from the sitar
        _capture("koto_wide", 107, 8),  # copied from the koto: reached through it
        _capture("harp", 46, 0),  # its own patch
    ]
    copies = {"koto": "sitar", "koto_wide": "koto", "shamisen": "sitar"}
    gates = capital_gates(rows[0], rows, pool, tmp_path, copies=copies)
    assert [g["capture"] for g in gates] == ["sitar", "koto", "koto_wide"]


def test_struct_copies_read_the_program_tables():
    from writeback import struct_copies

    copies = struct_copies()
    assert copies["marimba_wide"] == "marimba"
    assert copies["koto"] == "sitar"


def test_every_gate_a_capital_reaches_is_read_and_keyed_by_capture(tmp_path, monkeypatch):
    import json

    import refit

    def fake_compare(voice, out, lib, overrides=""):
        reading = {"key": "decay", "stat": "p90", "value": 1.0, "limit": 2.0, "over": False}
        out.write_text(
            json.dumps({"gate": {"readings": [dict(reading, capture=voice["capture"])]}})
        )

    monkeypatch.setattr(refit, "_compare", fake_compare)
    voice = {
        "slug": "p006-harpsichord",
        "capture": "harpsichord",
        "gates": [
            {"capture": "harpsichord", "gate": "a_gate.json", "timbre": "t0"},
            {"capture": "harpsichord_alt", "gate": "b_gate.json", "timbre": "t1"},
        ],
    }
    out = tmp_path / "pre.json"
    refit._compare_voice(voice, out, Path("lib"))
    keys = [r["key"] for r in json.loads(out.read_text())["gate"]["readings"]]
    assert keys == ["harpsichord:decay", "harpsichord_alt:decay"]


COPIES = {"wide": "cap", "wider": "wide"}
EXPLICIT = {"wide": frozenset({"x", "modal"})}


def test_an_override_on_a_patch_is_repeated_on_each_gate_patch_inheriting_it():
    from refit import reach_copies

    voice = {"patch": "cap", "gates": [{"patch": "cap"}, {"patch": "wide"}, {"patch": "wider"}]}
    out = reach_copies("cap.a=2.0,cap.x=3.0,cap.modal.decay=1.0", voice, COPIES, EXPLICIT)
    assert out.split(",") == [
        "cap.a=2.0",
        "cap.x=3.0",
        "cap.modal.decay=1.0",
        "wide.a=2.0",
        "wider.a=2.0",
    ], "a field or enclosing struct a copy assigns is its own"
    assert reach_copies("wide.x=4.0", voice, COPIES, EXPLICIT) == "wide.x=4.0,wider.x=4.0"
    assert reach_copies("cap.a=2.0,wide.a=5.0", voice, COPIES, EXPLICIT) == (
        "cap.a=2.0,wide.a=5.0,wider.a=2.0"
    ), "an entry already named is not overwritten"
    assert reach_copies("", voice, COPIES, EXPLICIT) == ""


def test_the_tuning_layer_runs_after_the_tables_copy_their_patches():
    # The premise reach_copies rests on: an override on a source never reaches its copy.
    from _repo import REPO_ROOT

    text = (REPO_ROOT / "src/midi/synth/gm_fallback_programs.cpp").read_text()
    assert text.index("configure_variation_programs(o);") < text.index("SONARE_GM_TUNE_ONE)")


def test_explicit_fields_read_the_program_tables():
    from refit import explicit_fields, owner
    from writeback import struct_copies

    explicit = explicit_fields()
    assert {"stereo_spread", "modal.modes2.ratio"} <= explicit["marimba_wide"]
    assert owner("marimba_wide", "stereo_spread", struct_copies(), explicit) == "marimba_wide"
    assert owner("marimba_wide", "modal.modes0.gain", struct_copies(), explicit) == "marimba"


def _snap(rev, cap, wide, fits_):
    return {
        "rev": rev,
        "values": {"cap": cap, "wide": wide},
        "fingerprints": {"cap": repr(sorted(cap.items())), "wide": repr(sorted(wide.items()))},
        "fits": fits_,
    }


def _history(*, worktree_fits=None):
    """c3 adopts the copy's own field, c2 the capital (reaching the copy), c1 is no refit."""
    f3 = {"cap": {"outcome": "adopted", "o": 2}, "wide": {"outcome": "adopted", "o": 3}}
    f2 = {"cap": {"outcome": "adopted", "o": 2}, "wide": None}
    c3 = _snap("c3", {"a": 3.0, "b": 2.0}, {"a": 3.0, "b": 2.0, "x": 9.0}, f3)
    c2 = _snap("c2", {"a": 3.0, "b": 2.0}, {"a": 3.0, "b": 2.0, "x": 0.5}, f2)
    c1 = _snap("c1", {"a": 1.0, "b": 2.0}, {"a": 1.0, "b": 2.0, "x": 0.5}, {"cap": None})
    c0 = _snap("c0", {"a": 1.0, "b": 0.0}, {"a": 1.0, "b": 0.0, "x": 0.5}, {"cap": None})
    worktree = dict(c3, rev="worktree", fits=worktree_fits or f3)
    return [worktree, c3, c2, c1, c0]


def _pins(base):
    return {p["key"]: (p["value"], p["source"].split(",")[0]) for p in base["pinned"]}


def test_a_variation_baseline_undoes_its_capitals_adoption_and_its_own():
    from refit import BANK_VERSIONS, baseline_pins

    voice = {"patch": "wide", "gates": [{"patch": "wide"}]}
    base = baseline_pins(voice, None, _history(), {"wide": "cap"}, {"wide": frozenset({"x"})})
    assert _pins(base) == {
        "wide.a": ("1.0", f"{BANK_VERSIONS}@c1"),  # before the capital's adoption reached it
        "wide.x": ("0.5", f"{BANK_VERSIONS}@c2"),  # before its own
    }, "c1 moved b without a fit record, so the run stops there"
    assert base["overrides"] == "wide.a=1.0,wide.x=0.5" and not base["unresolved"]


def test_a_capital_baseline_leaves_the_fields_its_copy_adopted_itself():
    from refit import baseline_pins

    voice = {"patch": "cap", "gates": [{"patch": "cap"}, {"patch": "wide"}]}
    fit = {"knobs": [{"key": "cap.a", "start": 3.0}, {"key": "cap.c", "start": 5.0}]}
    base = baseline_pins(voice, fit, _history(), {"wide": "cap"}, {"wide": frozenset({"x"})})
    assert _pins(base) == {
        "cap.a": ("1.0", "tools/bank-versions.json@c1"),  # the history outranks the fit's start
        "cap.c": ("5.0", "fit.json start"),
        "wide.a": ("1.0", "tools/bank-versions.json@c1"),
        "wide.c": ("5.0", "fit.json start"),  # repeated on the copy that inherits it
    }


def test_an_adoption_the_registry_does_not_carry_is_reported_not_guessed():
    from refit import baseline_pins

    voice = {"patch": "wide", "gates": [{"patch": "wide"}]}
    newer = {"cap": {"outcome": "adopted", "o": 4}, "wide": {"outcome": "adopted", "o": 3}}
    base = baseline_pins(
        voice, None, _history(worktree_fits=newer), {"wide": "cap"}, {"wide": frozenset({"x"})}
    )
    assert base["unresolved"] and base["unresolved"][0].startswith("cap:")
    alone = baseline_pins(voice, None, _history()[:1], {"wide": "cap"}, {})
    assert alone["pinned"] == [] and "no history" in alone["unresolved"][0]


def _judged_dir(tmp_path, monkeypatch, *, fit_runs):
    import json

    import refit

    seen: dict[str, str] = {}

    def fake_compare(voice, out, lib, overrides=""):
        seen[out.name] = overrides
        over = "cap.a=9.0" in overrides
        reading = {"key": "decay", "stat": "p90", "value": 2.0 if over else 0.5, "limit": 1.0}
        reading.update(over=over, excess=1.0 if over else -0.5)
        out.write_text(json.dumps({"gate": {"readings": [reading]}}))

    def no_fit(*a, **k):
        fit_runs.append(a)
        raise RuntimeError("no fit may run")

    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    monkeypatch.setattr(refit, "_compare", fake_compare)
    monkeypatch.setattr(refit, "_fit", no_fit)
    monkeypatch.setattr(refit, "struct_copies", lambda: {"wide": "cap"})
    monkeypatch.setattr(refit, "explicit_fields", lambda: {"wide": frozenset({"x"})})
    monkeypatch.setattr(refit, "registry_history", lambda units: iter(_history()))
    voice = {
        "slug": "cap",
        "patch": "cap",
        "capture": "cap",
        "gates": [
            {"capture": "cap", "gate": "g", "timbre": "t", "patch": "cap"},
            {"capture": "wide", "gate": "g", "timbre": "t", "patch": "wide"},
        ],
    }
    d = tmp_path / "cap"
    d.mkdir()
    fit = {
        "knobs": [{"key": "cap.a", "start": 3.0}],
        "selected": {"values": [9.0]},
        "loss": {"selected": 0.5},
        "validation": {"independent": True, "start": 1.0, "best": 0.5},
        "overrides": "cap.a=9.0",
        "path": [{"eval": 1, "loss": 0.9, "values": [9.0]}],
    }
    (d / "fit.json").write_text(json.dumps(fit))
    return refit, voice, d, seen


def test_pre_is_pinned_to_the_baseline_and_says_so(tmp_path, monkeypatch):
    import json

    runs: list = []
    refit, voice, d, seen = _judged_dir(tmp_path, monkeypatch, fit_runs=runs)
    result = refit.run_voice(voice, tmp_path, "build", 8)
    pre = json.loads((d / "pre.json").read_text())
    assert pre["baseline"]["overrides"] == "cap.a=1.0,wide.a=1.0"
    assert seen["pre.wide.json"] == "cap.a=1.0,wide.a=1.0"
    assert seen["post.wide.json"] == "cap.a=9.0,wide.a=9.0", "post reaches the copy too"
    assert result["newly_over"] == ["cap:decay p90", "wide:decay p90"] and not runs


def test_rejudge_recreates_the_judged_state_without_fitting(tmp_path, monkeypatch):
    import json

    runs: list = []
    refit, voice, d, seen = _judged_dir(tmp_path, monkeypatch, fit_runs=runs)
    for stale in ("pre.json", "pre.wide.json", "post.json", "path_fit_1.wide.json"):
        (d / stale).write_text(json.dumps({"gate": {"readings": []}}))
    (d / "verdict.json").write_text(json.dumps({"decision": "adopt", "why": "old"}))
    (d / "fit_r1.log").write_text("kept\n")
    result = refit.rejudge_voice(voice, tmp_path, "build")
    assert result["decision"] == "trade" and result["why"].startswith("(was adopt)")
    assert result["why"].endswith("run resolve"), "the reweighed repeat is left to resolve"
    ver = json.loads((d / "verdict.json").read_text())
    assert not ver.get("resolved") and ver["attempts"][-1]["pending"]
    assert "pre.wide.json" in seen and "path_fit_1.wide.json" in seen, "re-measured, not reused"
    assert (d / "fit_r1.log").exists() and (d / "fit.json").exists() and not runs


def test_a_copy_owns_only_the_fields_it_reassigns_after_the_copy():
    from refit import assigned_after_copy, reach_copies

    table = (
        "  o.wide.gain = 0.1f;\n"  # ahead of the copy: overwritten by it
        "  o.wide = o.cap;\n"
        "  o.wide.modal.modes[2].ratio = 6.0f;\n"
        "  o.wide.stereo_spread = 0.5f;\n"
    )
    explicit = assigned_after_copy([table])
    assert explicit == {"wide": frozenset({"modal.modes2.ratio", "stereo_spread"})}
    voice = {"patch": "cap", "gates": [{"patch": "cap"}, {"patch": "wide"}]}
    out = reach_copies("cap.stereo_spread=0.2,cap.gain=0.3", voice, {"wide": "cap"}, explicit)
    assert out == "cap.stereo_spread=0.2,cap.gain=0.3,wide.gain=0.3"
