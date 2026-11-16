"""Tests for the refit driver's adoption verdict, the fit registry and the objective fingerprint.

python -m pytest tools/voicematch/test_refit.py
"""

from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fits
import gate_audit
import objective
from profile_gate import gate_readings
from refit import HELD_OUT_MARGIN, IN_SAMPLE_MARGIN, verdict

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


def _own_notes(fit, start=1.0, best=0.9):
    """Give a fit the full-objective quality a gate-only judgement reads."""
    quality = {"absolute_weighted_mean_units": best, "baseline": {}}
    quality["baseline"]["absolute_weighted_mean_units"] = start
    fit["selected"]["quality"] = quality
    return fit


def test_a_grid_with_one_condition_per_note_is_judged_by_the_gate_alone():
    pre, fit, post = _case(False, True, "none", False, False, False)
    _own_notes(fit)
    assert verdict(pre, fit, post)["decision"] == "kept"
    assert verdict(pre, fit, post, gate_only=True)["decision"] == "adopt"
    pre, fit, post = _case(False, True, "none", False, True, False)
    _own_notes(fit)
    assert verdict(pre, fit, post, gate_only=True)["decision"] == "trade"


def test_a_gate_only_fit_under_the_gain_floor_is_kept():
    pre, fit, post = _case(False, True, "none", False, False, False)
    _own_notes(fit, 1.0, 1.0 - IN_SAMPLE_MARGIN / 2)
    got = verdict(pre, fit, post, gate_only=True)
    assert got["decision"] == "kept"
    assert got["why"] == f"confirm gain under the floor ({IN_SAMPLE_MARGIN / 2:+.4f})"
    assert "in_sample" not in got


def test_a_gate_only_fit_with_no_readable_gain_is_kept():
    pre, fit, post = _case(False, True, "none", False, False, False)
    got = verdict(pre, fit, post, gate_only=True)
    assert got["decision"] == "kept" and got["why"] == "no in-sample gain to judge"
    assert got["in_sample_gain"] is None


def test_the_confirm_pair_outranks_the_fits_own_numbers():
    pre, fit, post = _case(False, True, "none", False, False, False)
    _own_notes(fit, 1.0, 0.5)
    fit["confirm"] = {"local": {"start": {"loss": 1.0}, "best": {"loss": 0.999}}}
    got = verdict(pre, fit, post, gate_only=True)
    assert got["decision"] == "kept" and got["in_sample_gain"] == pytest.approx(0.001)
    fit["confirm"]["local"]["best"]["loss"] = 0.9
    got = verdict(pre, fit, post, gate_only=True)
    assert got["decision"] == "adopt" and got["in_sample_gain"] == pytest.approx(0.1)


def test_a_search_without_a_weighed_term_is_judged_on_the_confirm_pair_alone():
    pre, fit, post = _case(False, True, "better", False, False, False)
    fit["scored_without"] = ["mss"]
    got = verdict(pre, fit, post)
    assert got["decision"] == "kept"
    assert got["why"] == "searched without mss and not confirmed with it here"
    assert got["held_out_gain"] is None and got["searched_held_out_gain"] > 0
    fit["confirm"] = {"local": {"start": {"loss": 1.0}, "best": {"loss": 0.999}}}
    got = verdict(pre, fit, post)
    assert got["decision"] == "kept" and got["why"].startswith("confirm with mss on gained under")
    fit["confirm"]["local"]["best"]["loss"] = 0.9
    got = verdict(pre, fit, post)
    assert got["decision"] == "adopt" and got["why"].startswith("confirm +0.1000 with mss on")
    assert got["scored_without"] == ["mss"]


def test_an_adoption_with_no_held_out_condition_is_marked_in_sample():
    pre, fit, post = _case(False, True, "none", False, False, False)
    _own_notes(fit)
    assert verdict(pre, fit, post, gate_only=True)["in_sample"] is True
    pre, fit, post = _case(False, True, "better", False, False, False)
    got = verdict(pre, fit, post)
    assert got["decision"] == "adopt" and "in_sample" not in got


@pytest.mark.parametrize(
    ("start", "value", "branch"),
    [(0.0, 0.6, True), (0.0, 0.00005, False), (0.7, 0.0, True), (0.7, 0.4, False)],
)
def test_a_sustain_crossing_silence_is_a_branch_and_is_held(start, value, branch):
    """A one-shot that gains a sustain rings until a note-off a drum never sends."""
    from refit import switch_moves, writable

    knobs = [{"key": "d036.amp_env.sustain", "start": start}, {"key": FLOAT, "start": 1.0}]
    fit = {"knobs": knobs, "selected": {"values": [value, 2.0]}}
    assert switch_moves(fit) == (["d036.amp_env.sustain"] if branch else [])
    assert writable(fit, [value, 2.0]) == [start if branch else value, 2.0]
    pre, good, post = _case(*GOOD)
    good["knobs"], good["selected"] = fit["knobs"], fit["selected"]
    assert verdict(pre, good, post)["decision"] == ("trade" if branch else "adopt")


def test_a_count_landing_in_another_bucket_is_held():
    """A mode count adds or removes a resonator the gate window may never reach."""
    from refit import switch_moves, writable

    knobs = [{"key": "d048.percussion.num_modes", "start": 7.0}, {"key": FLOAT, "start": 1.0}]
    moved = {"knobs": knobs, "selected": {"values": [8.2, 2.0]}}
    assert switch_moves(moved) == ["d048.percussion.num_modes"]
    assert writable(moved, [8.2, 2.0]) == [7.0, 2.0]
    still = {"knobs": knobs, "selected": {"values": [7.3, 2.0]}}
    assert switch_moves(still) == []


def test_a_release_is_held_only_where_no_gate_reads_the_key_lift(tmp_path, monkeypatch):
    import refit

    monkeypatch.setattr(refit, "REFERENCE_DIR", tmp_path)
    (tmp_path / "blind_gate.json").write_text('{"bounds": {"attack": {}, "body": {}}}')
    (tmp_path / "lifts_gate.json").write_text('{"bounds": {"attack": {}, "damper": {}}}')
    blind = refit.unmeasured({"gates": [{"capture": "blind"}]})
    assert "amp_env.release_ms" in blind
    both = refit.unmeasured({"gates": [{"capture": "blind"}, {"capture": "lifts"}]})
    assert "amp_env.release_ms" not in both
    # A capture declaring the dimension is not enough: the gate has to bound it.
    named = tmp_path / "named_gate.json"
    named.write_text('{"bounds": {"attack": {}}, "dimensions": ["damper"]}')
    assert "amp_env.release_ms" in refit.unmeasured({"gates": [{"gate": str(named)}]})

    knobs = [{"key": "pad.amp_env.release_ms", "start": 580.0}, {"key": FLOAT, "start": 1.0}]
    fit = {"knobs": knobs, "selected": {"values": [947.0, 2.0]}}
    assert refit.switch_moves(fit, blind) == ["pad.amp_env.release_ms"]
    assert refit.writable(fit, [947.0, 2.0], blind) == [580.0, 2.0]
    assert refit.switch_moves(fit) == []
    pre, good, post = _case(*GOOD)
    good["knobs"], good["selected"] = fit["knobs"], fit["selected"]
    assert refit.verdict(pre, good, post, unjudged=blind)["decision"] == "trade"
    assert refit.verdict(pre, good, post)["decision"] == "adopt"


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

    def fake_compare(voice, out, lib, overrides, **_):
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

    monkeypatch.setattr(gate_audit, "compare_voice", fake_compare)
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

    def fake_compare(voice, out, lib, overrides="", **_):
        reading = {"key": "decay", "stat": "p90", "value": 1.0, "limit": 2.0, "over": False}
        out.write_text(
            json.dumps({"gate": {"readings": [dict(reading, capture=voice["capture"])]}})
        )

    monkeypatch.setattr(gate_audit, "compare", fake_compare)
    voice = {
        "slug": "p006-harpsichord",
        "capture": "harpsichord",
        "gates": [
            {"capture": "harpsichord", "gate": "a_gate.json", "timbre": "t0"},
            {"capture": "harpsichord_alt", "gate": "b_gate.json", "timbre": "t1"},
        ],
    }
    out = tmp_path / "pre.json"
    gate_audit.compare_voice(voice, out, Path("lib"))
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


def _pins(base, field="pinned"):
    return {p["key"]: (p["value"], p["source"].split(",")[0]) for p in base[field]}


def test_a_variation_baseline_names_its_capitals_adoption_and_its_own_without_pinning():
    from refit import BANK_VERSIONS, baseline_pins

    voice = {"patch": "wide", "gates": [{"patch": "wide"}]}
    base = baseline_pins(voice, None, _history(), {"wide": "cap"}, {"wide": frozenset({"x"})})
    assert _pins(base, "descends_from") == {
        "wide.a": ("1.0", f"{BANK_VERSIONS}@c1"),  # before the capital's adoption reached it
        "wide.x": ("0.5", f"{BANK_VERSIONS}@c2"),  # before its own
    }, "c1 moved b without a fit record, so the run stops there"
    assert base["pinned"] == [] and base["overrides"] == "" and not base["unresolved"]


def test_a_capital_baseline_pins_its_fits_start_and_names_what_it_descends_from():
    from refit import baseline_pins

    voice = {"patch": "cap", "gates": [{"patch": "cap"}, {"patch": "wide"}]}
    fit = {"knobs": [{"key": "cap.a", "start": 3.0}, {"key": "cap.c", "start": 5.0}]}
    base = baseline_pins(voice, fit, _history(), {"wide": "cap"}, {"wide": frozenset({"x"})})
    assert _pins(base) == {
        "cap.a": ("3.0", "fit.json start"),
        "cap.c": ("5.0", "fit.json start"),
        "wide.a": ("3.0", "fit.json start"),  # repeated on the copy that inherits it
        "wide.c": ("5.0", "fit.json start"),
    }
    assert _pins(base, "descends_from") == {
        "cap.a": ("1.0", "tools/bank-versions.json@c1"),
        "wide.a": ("1.0", "tools/bank-versions.json@c1"),
    }, "the copy's own adoption of x is the copy's, not the capital's"


def test_an_adoption_the_registry_does_not_carry_is_reported_not_guessed():
    from refit import baseline_pins

    voice = {"patch": "wide", "gates": [{"patch": "wide"}]}
    newer = {"cap": {"outcome": "adopted", "o": 4}, "wide": {"outcome": "adopted", "o": 3}}
    base = baseline_pins(
        voice, None, _history(worktree_fits=newer), {"wide": "cap"}, {"wide": frozenset({"x"})}
    )
    assert base["unresolved"] and base["unresolved"][0].startswith("cap:")
    alone = baseline_pins(voice, None, _history()[:1], {"wide": "cap"}, {})
    assert alone["descends_from"] == [] and "no history" in alone["unresolved"][0]


def _judged_dir(tmp_path, monkeypatch, *, fit_runs):
    import json

    import refit

    seen: dict[str, str] = {}

    def fake_compare(voice, out, lib, overrides="", **_):
        seen[out.name] = overrides
        over = "cap.a=9.0" in overrides
        reading = {"key": "decay", "stat": "p90", "value": 2.0 if over else 0.5, "limit": 1.0}
        reading.update(over=over, excess=1.0 if over else -0.5)
        out.write_text(json.dumps({"gate": {"readings": [reading]}}))

    def no_fit(*a, **k):
        fit_runs.append(a)
        raise RuntimeError("no fit may run")

    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    monkeypatch.setattr(gate_audit, "compare", fake_compare)
    monkeypatch.setattr(refit, "_fit", no_fit)
    monkeypatch.setattr(
        refit, "_catalogue", lambda voice, lib: SimpleNamespace(defaults={"cap.a": 3.0})
    )
    monkeypatch.setattr(refit, "struct_copies", lambda: {"wide": "cap"})
    monkeypatch.setattr(refit, "explicit_fields", lambda: {"wide": frozenset({"x"})})
    monkeypatch.setattr(refit, "registry_history", lambda units: iter(_history()))
    gate = tmp_path / "g"
    gate.write_text(json.dumps({"bounds": {"decay": {"p90": 1.0, "floor": 0.1}}}))
    voice = {
        "slug": "cap",
        "patch": "cap",
        "capture": "cap",
        "gates": [
            {"capture": "cap", "gate": str(gate), "timbre": "t", "patch": "cap"},
            {"capture": "wide", "gate": str(gate), "timbre": "t", "patch": "wide"},
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


def test_a_fit_from_what_ships_is_judged_against_what_ships_despite_an_adoption_history(
    tmp_path, monkeypatch
):
    import json

    # The fit starts at the shipped 3.0, which an adoption moved there from 1.0.
    runs: list = []
    refit, voice, d, seen = _judged_dir(tmp_path, monkeypatch, fit_runs=runs)
    result = refit.run_voice(voice, tmp_path, "build", 8)
    pre = json.loads((d / "pre.json").read_text())
    assert pre["baseline"]["overrides"] == "cap.a=3.0,wide.a=3.0"
    assert {p["key"]: p["value"] for p in pre["baseline"]["descends_from"]} == {
        "cap.a": "1.0",
        "wide.a": "1.0",
    }
    assert seen["pre.wide.json"] == "cap.a=3.0,wide.a=3.0"
    assert seen["post.wide.json"] == "cap.a=9.0,wide.a=9.0", "post reaches the copy too"
    assert result["newly_over"] == ["cap:decay p90", "wide:decay p90"] and not runs


def test_a_recorded_pre_pinned_away_from_what_ships_is_measured_again(tmp_path, monkeypatch):
    import json

    runs: list = []
    refit, voice, d, seen = _judged_dir(tmp_path, monkeypatch, fit_runs=runs)
    old = {"gate": {"readings": []}, "baseline": {"pinned": [{"key": "cap.a", "value": "1.0"}]}}
    (d / "pre.json").write_text(json.dumps(old))
    (d / "pre.wide.json").write_text("{}")
    refit.run_voice(voice, tmp_path, "build", 8)
    assert json.loads((d / "stale" / "pre.json").read_text()) == old
    assert (d / "stale" / "pre.wide.json").exists()
    pre = json.loads((d / "pre.json").read_text())
    assert pre["baseline"]["overrides"] == "cap.a=3.0,wide.a=3.0"
    assert seen["pre.wide.json"] == "cap.a=3.0,wide.a=3.0" and not runs


@pytest.mark.parametrize(
    ("ships", "recorded", "refused"),
    [
        (3.0, None, False),  # fit from what ships, an adoption in the history
        (3.0, "1.0", False),  # fit from what ships, an old pre pinned before the adoption
        (4.0, None, True),  # fit from a value that no longer ships
        (4.0, "3.0", True),
    ],
)
def test_the_refusal_is_produced_only_for_a_fit_whose_start_differs_from_what_ships(
    tmp_path, monkeypatch, ships, recorded, refused
):
    import json

    refit, voice, d, _seen = _judged_dir(tmp_path, monkeypatch, fit_runs=[])
    monkeypatch.setattr(
        refit, "_catalogue", lambda voice, lib: SimpleNamespace(defaults={"cap.a": ships})
    )
    if recorded is not None:
        pin = [{"key": "cap.a", "value": recorded}]
        body = {"gate": {"readings": []}, "baseline": {"pinned": pin}}
        (d / "pre.json").write_text(json.dumps(body))
    fit = json.loads((d / "fit.json").read_text())
    assert bool(refit.stale_starts([fit], {"cap.a": ships})) == refused
    for judge in (
        lambda: refit.run_voice(voice, tmp_path, "build", 8),
        lambda: refit.rejudge_voice(voice, tmp_path, "build"),
    ):
        if refused:
            with pytest.raises(
                RuntimeError, match=r"cap\.a 3\.0 -> 4\.0.*refit it from the shipped start"
            ):
                judge()
        else:
            judge()


def test_rejudge_recreates_the_judged_state_without_fitting(tmp_path, monkeypatch):
    import json

    runs: list = []
    refit, voice, d, seen = _judged_dir(tmp_path, monkeypatch, fit_runs=runs)
    monkeypatch.setattr(refit, "registry_history", lambda units: iter(()))
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


def test_rejudge_refuses_fits_started_from_values_no_longer_shipped(tmp_path, monkeypatch):
    import json

    refit, voice, d, seen = _judged_dir(tmp_path, monkeypatch, fit_runs=[])
    (d / "verdict.json").write_text(json.dumps({"decision": "adopt", "why": "old"}))
    (d / "pre.json").write_text("{}")
    monkeypatch.setattr(
        refit, "_catalogue", lambda voice, lib: SimpleNamespace(defaults={"cap.a": 4.0})
    )
    with pytest.raises(RuntimeError, match=r"cap\.a 3\.0 -> 4\.0.*refit it from the shipped start"):
        refit.rejudge_voice(voice, tmp_path, "build")
    assert (d / "pre.json").read_text() == "{}" and not seen, "nothing judged or deleted"


def test_a_start_is_stale_against_a_moved_or_vanished_shipped_value():
    from refit import stale_starts

    fits = [{"knobs": [{"key": "p.a", "start": 1.0}, {"key": "p.b", "start": 9.0, "head": 2.0}]}]
    assert stale_starts(fits, {"p.a": 1.0, "p.b": 2.0}) == []
    assert stale_starts(fits, {"p.b": 2.5}) == ["p.a 1.0 -> absent", "p.b 2.0 -> 2.5"]


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


def test_the_velocity_span_and_onset_gate_dimensions_name_their_fit_terms():
    from refit import GATE_TERMS

    assert GATE_TERMS["vel_range"] == "velspan"
    assert GATE_TERMS["onset"] == GATE_TERMS["onset_settle"] == "onset"


def test_a_red_dimension_raises_the_term_that_measures_it():
    from refit import reweighed

    report = {"fit_profile": {"weights": {"harm": 1.0, "lf": 1.0}}}
    assert reweighed(report, ["c:centroid_pct median"]) == {"bright": 1.0}
    assert reweighed(report, ["c:body abs_median"]) == {"body": 1.0}


def _prefit_dir(tmp_path, monkeypatch, red, *, weights, refuse_weighted=False):
    """A voice whose pre reading is red on `red` (dimension -> over), with every fit recorded."""
    import json

    import refit

    fits_run: list = []

    def fake_compare(voice, out, lib, overrides="", **_):
        readings = [
            {"key": dim, "stat": "p90", "value": 2.0 if over else 0.5, "limit": 1.0}
            | {"over": over, "excess": 1.0 if over else -0.5}
            for dim, over in red.items()
        ]
        out.write_text(json.dumps({"gate": {"readings": readings}}))

    def fake_fit(voice, out, build_dir, per_knob, weights=None):
        fits_run.append(weights)
        if weights and refuse_weighted:
            out.with_suffix(".log").write_text("ValueError: --w-body cannot be computed\n")
            raise RuntimeError("the fit wrote no report")
        out.write_text(
            json.dumps(
                {
                    "knobs": [{"key": "cap.a", "start": 3.0}],
                    "selected": {"values": [9.0]},
                    "loss": {"selected": 0.5},
                    "validation": {"independent": True, "start": 1.0, "best": 0.5},
                    "overrides": "cap.a=9.0",
                    "path": [],
                }
            )
        )

    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    monkeypatch.setattr(gate_audit, "compare", fake_compare)
    monkeypatch.setattr(refit, "_fit", fake_fit)
    monkeypatch.setattr(refit, "profile_weights", lambda voice: dict(weights))
    monkeypatch.setattr(refit, "struct_copies", dict)
    monkeypatch.setattr(refit, "explicit_fields", dict)
    monkeypatch.setattr(refit, "registry_history", lambda units: iter(()))  # pre ships
    voice = {
        "slug": "cap",
        "program": 0,
        "patch": "cap",
        "capture": "cap",
        "gates": [{"capture": "cap", "gate": "g", "timbre": "t", "patch": "cap"}],
    }
    return refit, voice, fits_run


def test_prefit_raises_a_red_dimensions_zero_weight_own_term(tmp_path, monkeypatch, capsys):
    import json

    red = {"body": True, "centroid_pct": True, "stretch": True, "decay": True, "tnr": False}
    refit, voice, fits_run = _prefit_dir(
        tmp_path, monkeypatch, red, weights={"harm": 1.0, "stiff": 2.0}
    )
    result = refit.run_voice(voice, tmp_path, "build", 8)
    assert fits_run == [{"body": 1.0, "bright": 1.0}], "stretch has a weighted own term"
    out = capsys.readouterr().out
    assert "[raised from zero weight: body, bright]" in out
    assert "red with no own term: decay" in out
    assert result["raised_from_zero"] == ["body", "bright"]
    assert result["prefit_weights"] == {"body": 1.0, "bright": 1.0}
    assert result["red_without_own_term"] == ["decay"], "slope is only a proxy for decay"
    assert json.loads((tmp_path / "cap" / "verdict.json").read_text())["raised_from_zero"]


def test_prefit_leaves_a_clean_pre_alone(tmp_path, monkeypatch):
    refit, voice, fits_run = _prefit_dir(
        tmp_path, monkeypatch, {"body": False, "stretch": True}, weights={"stiff": 1.0}
    )
    result = refit.run_voice(voice, tmp_path, "build", 8)
    assert fits_run == [None]
    assert "raised_from_zero" not in result and "prefit_weights" not in result


def test_a_refused_prefit_weight_falls_back_to_the_plain_fit(tmp_path, monkeypatch):
    refit, voice, fits_run = _prefit_dir(
        tmp_path, monkeypatch, {"body": True}, weights={"harm": 1.0}, refuse_weighted=True
    )
    result = refit.run_voice(voice, tmp_path, "build", 8)
    assert fits_run == [{"body": 1.0}, None]
    assert "raised_from_zero" not in result and "cannot be computed" in result["refused"]


def test_prefit_weights_double_a_nonzero_term_by_the_reweighed_rule():
    from refit import prefit_raise

    pre = {"gate": {"readings": [{"key": "cap:attack_hf", "stat": "m", "over": True}]}}
    assert prefit_raise({}, pre, {})["prefit_weights"] == {"hf": 1.0}
    kit = {"drum_note": 36}
    pre = {"gate": {"readings": [{"key": "crest", "stat": "m", "over": True}]}}
    assert prefit_raise(kit, pre, {})["prefit_weights"] == {"crest": 1.0}
    assert prefit_raise(kit, pre, {"crest": 3.0})["prefit_weights"] == {}


def _far_voice(tmp_path, monkeypatch, spread, k=10.0):
    """A one-gate voice whose gate holds @p spread, with the bank's far threshold set to @p k."""
    import json

    import refit

    gate = tmp_path / "cap_gate.json"
    gate.write_text(json.dumps({"reference_spread": spread}))
    units = tmp_path / "floor-units.json"
    units.write_text(json.dumps({"p90": k}))
    monkeypatch.setattr(refit, "FLOOR_UNITS_PATH", units)
    return {"gates": [{"capture": "cap", "gate": str(gate)}]}


def _far_pre(value):
    return {"gate": {"readings": [{"key": "cap:body", "stat": "abs_median", "value": value}]}}


def test_a_dimension_far_in_floors_raises_its_own_term(tmp_path, monkeypatch):
    from refit import prefit_raise

    voice = _far_voice(tmp_path, monkeypatch, {"body": 2.0})
    below = prefit_raise(voice, _far_pre(9.0 * 2.0), {})
    assert below["prefit_weights"] == {} and below["far"] == {} and below["raised_far"] == []
    above = prefit_raise(voice, _far_pre(11.0 * 2.0), {})
    assert above["prefit_weights"] == {"body": 1.0}
    assert above["raised_far"] == above["raised_from_zero"] == ["body"]
    assert above["far"] == {"body": {"floors": 11.0, "guessed": False}}
    assert prefit_raise(voice, _far_pre(22.0), {"body": 1.0})["prefit_weights"] == {}


def test_a_guessed_floor_is_labelled_in_the_far_line(tmp_path, monkeypatch):
    from profile_gate import dimension_floor
    from refit import far_line, prefit_raise

    floor, source = dimension_floor("body", {})
    assert source != "measured spread"
    voice = _far_voice(tmp_path, monkeypatch, {})
    far = prefit_raise(voice, _far_pre(11.0 * floor), {})["far"]["body"]
    assert far["guessed"]
    assert far_line("cap", "body", far, 10.0) == "cap: body far: 11 guessed floors (bank p90 10)"
    measured = {"floors": 11.0, "guessed": False}
    assert far_line("cap", "body", measured, 10.0) == "cap: body far: 11 floors (bank p90 10)"


def test_the_users_zero_weight_wins_over_a_far_raise(tmp_path, monkeypatch):
    import refit

    voice = _far_voice(tmp_path, monkeypatch, {"body": 2.0})
    monkeypatch.setattr(refit, "WEIGHTS", {"body": 0.0})
    got = refit.prefit_raise(voice, _far_pre(22.0), {"body": 0.0})
    assert got["prefit_weights"] == {} and got["raised_far"] == []
    assert got["user_zero"] == ["body"] and "body" in got["far"]


def test_the_verdict_records_raised_far(tmp_path, monkeypatch):
    import json

    refit, voice, fits_run = _prefit_dir(
        tmp_path, monkeypatch, {"body": False}, weights={"harm": 1.0}
    )
    gate = tmp_path / "cap_gate.json"
    gate.write_text(json.dumps({"reference_spread": {"body": 1.0}}))
    units = tmp_path / "floor-units.json"
    units.write_text(json.dumps({"p90": 10.0}))
    monkeypatch.setattr(refit, "FLOOR_UNITS_PATH", units)
    voice["gates"][0]["gate"] = str(gate)
    pre = tmp_path / "cap" / "pre.json"
    pre.parent.mkdir()
    reading = {"key": "cap:body", "stat": "abs_median", "value": 11.0, "limit": 99.0, "over": False}
    pre.write_text(json.dumps({"gate": {"readings": [reading]}}))
    result = refit.run_voice(voice, tmp_path, "build", 8)
    assert fits_run == [{"body": 1.0}]
    assert result["raised_far"] == result["raised_from_zero"] == ["body"]


def test_the_bank_floor_units_file_is_readable():
    from refit import far_floors

    assert far_floors() > 0.0


def test_refit_spec_replaces_the_auto_spec(tmp_path, monkeypatch):
    import subprocess

    import refit

    seen: list = []

    def fake_run(cmd, **kw):
        seen.append(cmd)
        Path(cmd[cmd.index("--out") + 1]).write_text("{}")
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(refit.subprocess, "run", fake_run)
    voice = {"slug": "v", "program": 1, "bank": 0, "corpus": "c", "timbre": "t", "notes": [60],
             "velocities": [90], "validate_notes": [61]}  # fmt: skip
    refit._fit(voice, tmp_path / "fit.json", "b", 8)
    plain = seen[-1]
    assert plain[plain.index("--spec") + 1] == "auto" and "--program-only" in plain
    assert "--start-overrides" not in plain
    monkeypatch.setattr(refit, "SPEC", "/x/spec.json")
    monkeypatch.setattr(refit, "START_OVERRIDES", "cap.a=2")
    refit._fit(voice, tmp_path / "fit.json", "b", 8)
    cmd = seen[-1]
    assert cmd[cmd.index("--spec") + 1] == "/x/spec.json" and "--program-only" not in cmd
    assert cmd[cmd.index("--start-overrides") + 1] == "cap.a=2"


def test_the_verdict_records_the_spec_and_its_knobs(tmp_path, monkeypatch):
    refit, voice, _ = _prefit_dir(tmp_path, monkeypatch, {"stretch": False}, weights={})
    monkeypatch.setattr(refit, "SPEC", "/x/spec.json")
    monkeypatch.setattr(refit, "SPEC_KNOBS", ["cap.a", "cap.b"])
    result = refit.run_voice(voice, tmp_path, "build", 8)
    assert result["spec"] == {"path": "/x/spec.json", "knobs": ["cap.a", "cap.b"]}


def test_a_start_override_point_does_not_change_what_ships():
    import refit

    knob = {"key": "cap.a", "start": 5.0, "head": 3.0}
    report = {"knobs": [knob], "selected": {"values": [3.0]}}
    assert refit.overrides_for(report, [3.0]) == ""
    assert refit.overrides_for(report, [5.0]) == "cap.a=5.0"
    pre = post = {"gate": {"readings": []}}
    fit = {**report, "loss": {}, "selected": {"values": [5.0]}}
    assert refit.verdict(pre, fit, post)["moved"] == 1, "against head, not the start point"


def _fit_cmd(tmp_path, monkeypatch, weights=None):
    import subprocess

    import refit

    seen: list = []

    def fake_run(cmd, **kw):
        seen.append(cmd)
        Path(cmd[cmd.index("--out") + 1]).write_text("{}")
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(refit.subprocess, "run", fake_run)
    voice = {"slug": "v", "program": 1, "bank": 0, "corpus": "c", "timbre": "t", "notes": [60],
             "velocities": [90], "validate_notes": [61]}  # fmt: skip
    refit._fit(voice, tmp_path / "fit.json", "b", 8, weights)
    return seen[-1]


def _flags(cmd):
    return {c[len("--w-") :]: cmd[i + 1] for i, c in enumerate(cmd) if c.startswith("--w-")}


def test_weights_reach_each_fit_as_its_own_weight_flags(tmp_path, monkeypatch):
    import refit

    assert _flags(_fit_cmd(tmp_path, monkeypatch)) == {}
    monkeypatch.setattr(refit, "WEIGHTS", refit.load_weights("slope=0,tail=0,bright=1,body=1"))
    assert _flags(_fit_cmd(tmp_path, monkeypatch)) == {
        "slope": "0",
        "tail": "0",
        "bright": "1",
        "body": "1",
    }
    # A term the fit itself raises outranks the user's weight for it.
    assert _flags(_fit_cmd(tmp_path, monkeypatch, {"slope": 1.0, "hf": 2.0})) == {
        "slope": "1",
        "tail": "0",
        "bright": "1",
        "body": "1",
        "hf": "2",
    }


def test_weights_are_read_inline_or_from_a_file_and_unknown_terms_are_refused(tmp_path):
    import refit

    assert refit.load_weights("") == {}
    assert refit.load_weights("slope=0, bright=1.5") == {"slope": 0.0, "bright": 1.5}
    path = tmp_path / "w.txt"
    path.write_text("slope=0  # off\nbody=1\n")
    assert refit.load_weights(f"@{path}") == {"slope": 0.0, "body": 1.0}
    for bad, why in (
        ("slpoe=0", "not a loss term"),
        ("slope", "not term=weight"),
        ("slope=x", "not a number"),
        ("slope=-1", "not negative"),
        ("slope=1,slope=2", "twice"),
        (f"@{tmp_path / 'missing'}", "not a file"),
    ):
        with pytest.raises(ValueError, match=why):
            refit.load_weights(bad)


def test_an_unknown_weight_term_stops_the_command_line(monkeypatch):
    import refit

    monkeypatch.setattr(sys, "argv", ["refit.py", "plan", "--weights", "slpoe=0"])
    with pytest.raises(SystemExit):
        refit.main()


def test_prefit_starts_from_the_user_weights_and_a_user_zero_outranks_a_red_dimension(
    tmp_path, monkeypatch, capsys
):
    import refit

    # body and centroid are red; slope was zeroed by the user and is not red, bright was
    # zeroed by the user and is.
    user = {"slope": 0.0, "bright": 0.0}
    monkeypatch.setattr(refit, "WEIGHTS", user)
    red = {"body": True, "centroid_pct": True, "stretch": False}
    refit_mod, voice, fits_run = _prefit_dir(
        tmp_path, monkeypatch, red, weights={"harm": 1.0, "stiff": 1.0, **user}
    )
    result = refit_mod.run_voice(voice, tmp_path, "build", 8)
    assert fits_run == [{"body": 1.0}], "body is raised from zero; slope and bright stay at 0"
    assert result["prefit_weights"] == {"body": 1.0}
    assert result["user_zero"] == ["bright"]
    assert result["weights"] == user
    assert "red, but kept at the weight 0 given: bright" in capsys.readouterr().out


def test_the_profile_the_prefit_starts_from_carries_the_user_weights(monkeypatch):
    import refit

    base = refit.profile_weights({"program": 0})
    monkeypatch.setattr(refit, "WEIGHTS", {"harm": 0.0, "body": 3.0})
    got = refit.profile_weights({"program": 0})
    assert got["harm"] == 0.0 and got["body"] == 3.0
    assert {k: v for k, v in got.items() if k not in ("harm", "body")} == {
        k: v for k, v in base.items() if k not in ("harm", "body")
    }


def test_a_reweighed_round_does_not_raise_a_term_the_user_zeroed(monkeypatch):
    import refit

    report = {"fit_profile": {"weights": {"harm": 1.0}}}
    assert refit.reweighed(report, ["c:body abs_median"]) == {"body": 1.0}
    monkeypatch.setattr(refit, "WEIGHTS", {"body": 0.0})
    assert refit.reweighed(report, ["c:body abs_median"]) is None


# The floor rule: a point is refused for worsening a statistic past its floor, and a
# trade is kept for the reason its nearest refused point shows.


def _row(stat, value, limit=2.0):
    return {
        "key": "cap:tnr",
        "stat": stat,
        "value": value,
        "limit": limit,
        "over": value > limit,
        "excess": (value - limit) / limit,
    }


def _readings(median, p90):
    return {"gate": {"readings": [_row("median", median), _row("p90", p90)]}}


def _floor_case(
    tmp_path, monkeypatch, point, *, pre=(0.5, 0.5), post=(0.5, 0.5), floor=0.1, steps=10
):
    """A settled-by-path trade on disk: `point(i)` gives the (median, p90) the path's i-th point reads."""
    import json as _json

    import refit

    d = tmp_path / "p1"
    d.mkdir()
    bound = {"median": 2.0, "p90": 2.0}
    if floor is not None:
        bound["floor"] = floor
    gate = tmp_path / "cap_gate.json"
    gate.write_text(_json.dumps({"bounds": {"tnr": bound}, "reference_spread": {"tnr": 0.7}}))
    voice = {
        "slug": "p1",
        "capture": "cap",
        "timbre": "t",
        "gates": [{"capture": "cap", "gate": str(gate)}],
    }
    (d / "voice.json").write_text(_json.dumps(voice))
    path = [
        {"eval": i + 1, "loss": 1.0 - i / 20, "stage": "fit", "values": [float(i)]}
        for i in range(steps)
    ]
    (d / "fit.json").write_text(
        _json.dumps(
            {
                "objective": "o1",
                "knobs": [{"key": FLOAT, "start": -1.0}],
                "selected": {"values": [float(steps - 1)]},
                "loss": {"selected": 0.5},
                "path": path,
            }
        )
    )
    (d / "pre.json").write_text(_json.dumps(_readings(*pre)))
    (d / "post.json").write_text(_json.dumps(_readings(*post)))
    (d / "verdict.json").write_text(
        _json.dumps(
            {
                "slug": "p1",
                "decision": "trade",
                "why": "newly over its bound",
                "newly_over": [],
                "moved": 1,
            }
        )
    )

    def fake_compare(voice, out, lib, overrides, **_):
        out.write_text(_json.dumps(_readings(*point(int(float(overrides.split("=")[1]))))))

    monkeypatch.setattr(gate_audit, "compare_voice", fake_compare)
    monkeypatch.setattr(refit, "reach_copies", lambda overrides, voice: overrides)
    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    held: dict = {}
    monkeypatch.setattr(refit.fits_mod, "load", lambda: held)
    monkeypatch.setattr(refit.fits_mod, "save", lambda r: held.update(r))
    for name in ("write_patch_fields", "write_family_fields", "write_drum_fields"):
        monkeypatch.setattr(refit, name, lambda *a, **k: {})
    return d, held


def test_the_furthest_gate_accepted_point_that_worsens_a_statistic_past_its_floor_is_passed_over(
    tmp_path, monkeypatch
):
    import refit

    # From point 6 on, p90 rises 0.4 (floor 0.1) while the gate load keeps falling.
    d, _ = _floor_case(tmp_path, monkeypatch, lambda i: (0.5 - 0.03 * i, 0.5 if i <= 5 else 0.9))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["decision"] == "adopt"
    assert ver["values"] == [5.0]
    assert "eval 6" in ver["why"]


def test_a_search_that_moves_nothing_past_its_floor_is_kept_for_it(tmp_path, monkeypatch):
    import json as _json

    import refit

    d, held = _floor_case(tmp_path, monkeypatch, lambda i: (0.5 - 0.005 * i, 0.5))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["decision"] == "kept" and ver["kept_for"] == "under_floor"
    assert ver["why"].startswith("no statistic past its floor")
    refit.adopt(tmp_path, set())
    assert held["p"]["outcome"] == "kept" and "blocking" not in held["p"]
    assert _json.loads((d / "verdict.json").read_text())["kept_for"] == "under_floor"


def test_a_search_whose_gate_load_never_falls_is_kept_under_its_floor_with_that_reason(
    tmp_path, monkeypatch
):
    import refit

    # Every point reads exactly pre: nothing grows, nothing worsens, the mean load does not fall.
    d, held = _floor_case(tmp_path, monkeypatch, lambda i: (0.5, 0.5))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["decision"] == "kept" and ver["kept_for"] == "under_floor"
    assert ver["why"].startswith("gate load not lower")
    refit.adopt(tmp_path, set())
    assert held["p"]["outcome"] == "kept" and "blocking" not in held["p"]


def test_a_path_that_starts_at_the_shipped_point_takes_its_evidence_from_the_next_point(
    tmp_path, monkeypatch
):
    import json as _json

    import refit

    # Point 0 is the shipped start (reads pre); point 1 on lowers the median but lifts the
    # p90 past its floor, so the nearest refused point is eval 2 and it is refused by the floor.
    d, _ = _floor_case(tmp_path, monkeypatch, lambda i: (0.5, 0.5) if i == 0 else (0.05, 0.9))
    fit = _json.loads((d / "fit.json").read_text())
    fit["knobs"][0]["start"] = 0.0
    (d / "fit.json").write_text(_json.dumps(fit))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["decision"] == "kept" and ver["kept_for"] == "floor"
    assert ver["refused_point"]["eval"] == 2


def test_a_bound_refusal_names_the_rows_of_the_refused_point_not_the_endpoint(
    tmp_path, monkeypatch
):
    import refit

    # Every point is over its bound; the full fit's endpoint (post.json) reads further still.
    d, held = _floor_case(tmp_path, monkeypatch, lambda i: (0.5, 3.0 + 0.1 * i), post=(0.5, 9.9))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["kept_for"] == "bound" and ver["refused_point"]["eval"] == 1
    refit.adopt(tmp_path, set())
    assert held["p"]["outcome"] == "blocked"
    assert held["p"]["blocking"] == [
        {
            "gate": "cap",
            "dimension": "tnr",
            "statistic": "p90",
            "before": 0.5,
            "after": 3.0,
            "bound": 2.0,
            "floor": 0.1,
            "why": "bound",
        }
    ]


def test_a_bound_refusal_names_a_row_already_over_that_the_refused_point_pushes_further(
    tmp_path, monkeypatch
):
    import refit

    # p90 is over at pre and grows by less than its floor: no row is newly over, yet it blocks.
    d, held = _floor_case(tmp_path, monkeypatch, lambda i: (0.3, 2.55), pre=(0.5, 2.5))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["kept_for"] == "bound"
    refit.adopt(tmp_path, set())
    assert held["p"]["outcome"] == "blocked"
    assert [(r["statistic"], r["before"], r["after"], r["why"]) for r in held["p"]["blocking"]] == [
        ("p90", 2.5, 2.55, "bound")
    ]


def test_a_floor_refusal_is_recorded_blocked_with_the_rows_past_their_floor(tmp_path, monkeypatch):
    import refit

    d, held = _floor_case(tmp_path, monkeypatch, lambda i: (0.1 - 0.001 * i, 0.7), post=(0.1, 0.9))
    ver = refit.resolve_voice(d, "build", 0)
    assert ver["decision"] == "kept" and ver["kept_for"] == "floor"
    assert ver["refused_point"]["worse"] == [
        {"key": "cap:tnr", "stat": "p90", "before": 0.5, "after": 0.7, "floor": 0.1}
    ]
    refit.adopt(tmp_path, set())
    assert held["p"]["outcome"] == "blocked"
    assert held["p"]["blocking"] == [
        {
            "gate": "cap",
            "dimension": "tnr",
            "statistic": "p90",
            "before": 0.5,
            "after": 0.7,
            "bound": 2.0,
            "floor": 0.1,
            "why": "floor",
        }
    ]


def test_a_settled_verdict_without_kept_for_is_refused_by_adopt(tmp_path, monkeypatch):
    import json as _json

    import refit

    d, _ = _floor_case(tmp_path, monkeypatch, lambda i: (0.5, 0.5))
    (d / "verdict.json").write_text(
        _json.dumps({"slug": "p1", "decision": "kept", "resolved": True, "why": "x"})
    )
    with pytest.raises(RuntimeError, match="kept_for"):
        refit.adopt(tmp_path, set())


def test_a_stored_zero_floor_is_a_floor_of_zero_and_a_missing_one_is_computed(tmp_path):
    import json as _json

    import refit
    from profile_gate import GENERIC_FLOOR

    def floor_with(bounds, spread=None):
        gate = tmp_path / f"g{len(list(tmp_path.iterdir()))}.json"
        gate.write_text(_json.dumps({"bounds": bounds, "reference_spread": spread or {}}))
        voice = {"capture": "cap", "gates": [{"capture": "cap", "gate": str(gate)}]}
        return refit.gate_floors(voice)

    assert floor_with({"tnr": {"median": 2.0, "floor": 0.0}}, {"tnr": 0.7})("cap:tnr") == 0.0
    assert floor_with({"tnr": {"median": 2.0}}, {"tnr": 0.7})("cap:tnr") == 0.7
    assert floor_with({"zz": {"median": 2.0}})("cap:zz") == GENERIC_FLOOR
    pre, post = [_row("median", 0.5)], [_row("median", 0.55)]
    moves = refit.floor_movement(pre, post, floor_with({"tnr": {"floor": 0.0}}))
    assert refit._worse_rows(moves)
    moves = refit.floor_movement(pre, post, floor_with({"tnr": {"floor": 0.1}}))
    assert not refit._worse_rows(moves)


# --------------------------------------------------------------------------
# gain anchor for a probe that cannot score level

GAIN = "d065.gain"
DRIVE = "d065.drive"


def _level_model(extra_db=4.0, curve=lambda x: x):
    """A stub of `model_level_db`: gain in dB (through `curve`), plus a drift when the fit's knob is set."""
    import math

    from knobs import parse_overrides

    def read(voice, overrides, lib):
        got = dict(parse_overrides(overrides))
        gain = float(got.get(GAIN, 1.0))
        return curve(20.0 * math.log10(gain)) + (extra_db if DRIVE in got else 0.0)

    return read


def _anchor_setup(monkeypatch, tmp_path, *, level=None, after_loss=1.0, bounds=(0.0, 4.0)):
    import refit
    import remote_fit

    monkeypatch.setattr(refit, "BASE_OVERRIDES", "")
    monkeypatch.setattr(refit, "reach_copies", lambda overrides, voice: overrides)
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    monkeypatch.setattr(refit, "model_level_db", level or _level_model())
    monkeypatch.setattr(
        refit,
        "_catalogue",
        lambda voice, lib: SimpleNamespace(defaults={GAIN: 1.0}, bounds={"gain": bounds}),
    )
    monkeypatch.setattr(refit, "_fit_cmd", lambda *a, **k: ["py", "autofit.py", "--spec", "auto"])
    losses = iter([1.0, after_loss])
    monkeypatch.setattr(
        remote_fit,
        "score_once",
        lambda argv, lib, out, start, fit=None: {
            "selected": {"quality": {"absolute_weighted_mean_units": next(losses)}}
        },
    )
    report = {
        "knobs": [{"key": DRIVE, "start": 0.0, "best": 0.5}],
        "selected": {"values": [0.5]},
        "overrides": f"{DRIVE}=0.5",
        "level_unscored": True,
    }
    voice = {"slug": "kit-n065", "patch": "d065", "drum_note": 65}
    (tmp_path / "fit.json").write_text("{}")
    return refit, voice, report


def test_the_gain_solve_converges_on_a_linear_level():
    from refit import ANCHOR_TOLERANCE_DB, solve_gain

    gain, left, first = solve_gain(
        lambda g: 20.0 * __import__("math").log10(g) + 4.0, 1.0, 0.0, 4.0
    )
    assert first == pytest.approx(4.0)
    assert abs(left) <= ANCHOR_TOLERANCE_DB and gain == pytest.approx(10 ** (-4.0 / 20), rel=1e-3)


def test_the_gain_solve_converges_on_a_mildly_saturating_level():
    import math

    from refit import ANCHOR_TOLERANCE_DB, solve_gain

    def diff(g):
        x = 20.0 * math.log10(g)
        return 0.8 * x + 0.01 * x * x + 3.0

    gain, left, _ = solve_gain(diff, 1.0, 0.0, 4.0)
    assert abs(left) <= ANCHOR_TOLERANCE_DB and gain < 1.0


def test_the_gain_solve_stays_inside_the_catalogue_range():
    from refit import solve_gain

    gain, _, _ = solve_gain(lambda g: 20.0 * __import__("math").log10(g) - 12.0, 1.0, 0.0, 2.0)
    assert gain == 2.0


def test_a_gain_anchor_is_recorded_and_rewrites_the_fit(monkeypatch, tmp_path):
    import json

    refit, voice, report = _anchor_setup(monkeypatch, tmp_path)
    got = refit.anchor_gain(voice, tmp_path, "build", report, Path("lib"))
    block = got[refit.GAIN_ANCHOR_KEY]
    assert block["knob"] == GAIN and block["level_before_db"] == pytest.approx(4.0)
    assert abs(block["level_after_db"]) <= refit.ANCHOR_TOLERANCE_DB
    assert block["after"] == pytest.approx(10 ** (-4.0 / 20), rel=1e-2)
    assert block["loss_before"] == 1.0 and block["loss_after"] == 1.0 and block["renders"] >= 3
    assert got["selected"]["values"] == [0.5, block["after"]]
    assert got["knobs"][-1]["key"] == GAIN and got["knobs"][-1]["start"] == 1.0
    assert f"{GAIN}=" in got["overrides"] and f"{DRIVE}=0.5" in got["overrides"]
    assert json.loads((tmp_path / "fit.json").read_text())[refit.GAIN_ANCHOR_KEY] == block


def test_a_gain_anchor_moves_a_gain_the_search_carries(monkeypatch, tmp_path):
    refit, voice, report = _anchor_setup(monkeypatch, tmp_path)
    report["knobs"].append({"key": GAIN, "start": 1.0, "best": 1.0, "min": 0.0, "max": 4.0})
    report["selected"]["values"].append(1.0)
    got = refit.anchor_gain(voice, tmp_path, "build", report, Path("lib"))
    assert len(got["knobs"]) == 2 and got["knobs"][1]["best"] == got["selected"]["values"][1]
    assert got["selected"]["values"][1] < 1.0


def test_a_loss_change_over_the_margin_leaves_the_values_and_records_failed(monkeypatch, tmp_path):
    refit, voice, report = _anchor_setup(
        monkeypatch, tmp_path, after_loss=1.0 + 2 * IN_SAMPLE_MARGIN
    )
    got = refit.anchor_gain(voice, tmp_path, "build", report, Path("lib"))
    assert got["selected"]["values"] == [0.5] and got["overrides"] == f"{DRIVE}=0.5"
    block = got[refit.GAIN_ANCHOR_KEY]
    assert block["failed"] == {"loss_before": 1.0, "loss_after": 1.0 + 2 * IN_SAMPLE_MARGIN}
    assert "loss_after" not in block


def test_a_patch_with_no_gain_field_is_recorded_and_left_alone(monkeypatch, tmp_path):
    refit, voice, report = _anchor_setup(monkeypatch, tmp_path)
    monkeypatch.setattr(refit, "_catalogue", lambda v, lib: SimpleNamespace(defaults={}, bounds={}))
    got = refit.anchor_gain(voice, tmp_path, "build", report, Path("lib"))
    assert got[refit.GAIN_ANCHOR_KEY]["skipped"] == "no gain field"
    assert got["selected"]["values"] == [0.5]


def test_the_in_sample_gain_uses_the_anchored_loss():
    pre, fit, post = _case(False, True, "none", False, False, False)
    _own_notes(fit, 1.0, 0.9)
    assert verdict(pre, fit, post, gate_only=True)["in_sample_gain"] == pytest.approx(0.1)
    fit["gain_anchor"] = {"loss_before": 0.9, "loss_after": 0.85}
    assert verdict(pre, fit, post, gate_only=True)["in_sample_gain"] == pytest.approx(0.15)
    fit["gain_anchor"] = {"failed": {"loss_before": 0.9, "loss_after": 0.7}}
    assert verdict(pre, fit, post, gate_only=True)["in_sample_gain"] == pytest.approx(0.1)


@pytest.mark.parametrize("unscored", [True, False])
def test_the_anchor_runs_only_when_the_report_says_level_went_unscored(
    monkeypatch, tmp_path, unscored
):
    import json

    import refit

    (tmp_path / "pre.json").write_text("{}")
    (tmp_path / "fit.json").write_text(json.dumps({"level_unscored": True} if unscored else {}))
    calls = []
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    monkeypatch.setattr(refit, "unmeasured", lambda voice: frozenset())
    monkeypatch.setattr(refit, "verdict", lambda *a, **k: {"decision": "kept"})
    monkeypatch.setattr(
        gate_audit, "compare_voice", lambda voice, out, *a, **k: out.write_text("{}")
    )

    def anchor(voice, d, build_dir, report, lib, per_knob):
        calls.append(voice["slug"])
        return report

    monkeypatch.setattr(refit, "anchor_gain", anchor)
    refit._judge_voice({"slug": "v"}, tmp_path, "build", 8)
    assert calls == (["v"] if unscored else [])


def test_an_anchor_that_rewrote_the_fit_drops_the_stale_per_capture_audits(monkeypatch, tmp_path):
    import json

    import refit

    (tmp_path / "pre.json").write_text("{}")
    (tmp_path / "fit.json").write_text(json.dumps({"level_unscored": True}))
    (tmp_path / "post.hit.json").write_text('{"stale": true}')
    seen = []
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    monkeypatch.setattr(refit, "_tuning_lib", lambda build_dir: Path("lib"))
    monkeypatch.setattr(refit, "unmeasured", lambda voice: frozenset())
    monkeypatch.setattr(refit, "verdict", lambda *a, **k: {"decision": "kept"})

    def audit(voice, out, *a, **k):
        seen.append((tmp_path / "post.hit.json").exists())
        out.write_text("{}")

    monkeypatch.setattr(gate_audit, "compare_voice", audit)
    monkeypatch.setattr(
        refit,
        "anchor_gain",
        lambda voice, d, b, report, lib, k: {**report, refit.GAIN_ANCHOR_KEY: {"loss_after": 1.0}},
    )
    refit._judge_voice({"slug": "v"}, tmp_path, "build", 8)
    assert seen == [False]


def test_autofit_records_a_probe_with_no_level_contrast(tmp_path):
    import json

    from autofit import _fold_level_unscored

    out = tmp_path / "fit.json"
    out.write_text("{}")
    _fold_level_unscored(str(out), SimpleNamespace(has_level_contrasts=True))
    assert "level_unscored" not in json.loads(out.read_text())
    _fold_level_unscored(str(out), SimpleNamespace(has_level_contrasts=False))
    assert json.loads(out.read_text())["level_unscored"] is True


# --------------------------------------------------------------------------- #
# tail check: the decay past the scored window
# --------------------------------------------------------------------------- #
TAIL_SR = 8000


def _decaying(seconds, slopes, knee=None, onset_s=0.0):
    """A 1 kHz tone falling at `slopes[0]` dB/s, then `slopes[1]` past `knee` seconds."""
    import numpy as np

    t = np.arange(int(seconds * TAIL_SR)) / TAIL_SR - onset_s
    db = slopes[0] * t
    if knee is not None:
        db = np.where(t < knee, db, slopes[0] * knee + slopes[1] * (t - knee))
    y = np.sin(2 * np.pi * 1000.0 * t) * 10.0 ** (db / 20.0)
    return np.where(t >= 0.0, y, 0.0)


def test_the_tail_measurement_reads_slope_and_time_to_minus_60_db():
    from refit import model_tail, reference_tail

    got = model_tail(_decaying(4.0, (-30.0,)), TAIL_SR, window_end_s=0.5)
    assert got["reached"] and got["t60_s"] == pytest.approx(2.0, abs=0.03)
    assert got["late_slope_db_s"] == pytest.approx(-30.0, abs=0.5)
    gone = model_tail(_decaying(4.0, (-200.0,)), TAIL_SR, window_end_s=0.5)
    assert gone["late_slope_db_s"] is None, "nothing rings past the window"
    # Fast then slow: the last slope is the slow one, projected from the peak to -60 dB.
    ref = reference_tail(_decaying(1.0, (-100.0, -20.0), knee=0.2), TAIL_SR, preroll_s=0.0)
    assert ref["last_slope_db_s"] == pytest.approx(-20.0, abs=0.5)
    assert ref["t60_s"] == pytest.approx(0.2 + 40.0 / 20.0, rel=0.03)


def test_the_tail_bound_is_the_slower_of_shipped_and_reference():
    from refit import tail_bound

    shipped = {"late_slope_db_s": -30.0, "t60_s": 2.0}
    reference = {"last_slope_db_s": -16.4, "t60_s": 3.0}
    assert tail_bound(shipped, reference) == {"slope_db_s": -16.4, "t60_s": 3.0}
    slow = {"late_slope_db_s": -10.0, "t60_s": 5.0}
    assert tail_bound(slow, reference) == {"slope_db_s": -10.0, "t60_s": 5.0}
    gone = {"late_slope_db_s": None, "t60_s": 0.1}
    assert tail_bound(gone, {"last_slope_db_s": -16.4, "t60_s": None}) == {
        "slope_db_s": -16.4,
        "t60_s": None,
    }


def _tail_voice(monkeypatch, tmp_path, candidate_slope, *, ref_exists=True):
    import refit

    ref = tmp_path / "n060_v100.wav"
    if ref_exists:
        ref.write_bytes(b"")
    corpus = SimpleNamespace(
        notes=(60,),
        velocities=(100,),
        renders={(60, 100): ref},
        note_map={},
        channel=10,
        rig="none",
        sample_rate=TAIL_SR,
        preroll_s=0.0,
        gate_for=lambda n, v: 0.06,
        slot_for=lambda n, v: 0.068,
    )
    rendered = []

    def render(targets, overrides, lib):
        rendered.append(overrides)
        slope = candidate_slope if "d060.x" in overrides else -30.0
        return [_decaying(4.0, (slope,)) for _ in targets]

    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    monkeypatch.setattr(refit, "_corpus", lambda path, timbre: corpus)
    monkeypatch.setattr(refit, "render_tails", render)
    monkeypatch.setattr(refit, "_reference_mono", lambda path: (_decaying(1.0, (-20.0,)), TAIL_SR))
    monkeypatch.setattr(refit, "struct_copies", dict)
    monkeypatch.setattr(refit, "explicit_fields", dict)
    voice = {
        "slug": "kit-n060",
        "patch": "d060",
        "capture": "hit",
        "timbre": "t0",
        "corpus": str(tmp_path),
        "drum_note": 60,
    }
    fit = {
        "knobs": [{"key": FLOAT, "start": 1.0}],
        "selected": {"values": [2.0]},
        "loss": {"selected": 0.5},
        "validation": {"independent": True, "start": 1.0, "best": 0.5},
        "overrides": "d060.x=2.0",
    }
    return refit, voice, fit, rendered


def _tail_verdict(refit, fit):
    empty = {"gate": {"readings": []}}
    return refit.verdict(empty, fit, empty)


def test_a_fit_ringing_past_the_window_is_kept_with_the_reason(monkeypatch, tmp_path):
    refit, voice, fit, rendered = _tail_voice(monkeypatch, tmp_path, candidate_slope=-10.0)
    assert _tail_verdict(refit, fit)["decision"] == "adopt", "adoptable without the check"
    record = refit.tail_check(voice, fit, Path("lib"))
    assert rendered == ["", "d060.x=2.0"], "shipped at the base, the candidate on top of it"
    assert record["violated"] and record["rows"][0]["violation"] == ["slope", "t60"]
    assert record["rows"][0]["window_end_s"] == 0.068
    out = _tail_verdict(refit, {**fit, refit.TAIL_CHECK_KEY: record})
    assert out["decision"] == "kept"
    assert out["why"].startswith("ring past the observed window (hit n60v100: slope -10.0 vs")
    assert out["tail_check"] is record


def test_a_fit_within_the_tail_bound_passes(monkeypatch, tmp_path):
    refit, voice, fit, _ = _tail_voice(monkeypatch, tmp_path, candidate_slope=-25.0)
    record = refit.tail_check(voice, fit, Path("lib"))
    assert not record["violated"]
    bound = record["rows"][0]["bound"]
    assert bound["slope_db_s"] == pytest.approx(-20.0, abs=0.5), "the reference is the slower"
    assert _tail_verdict(refit, {**fit, refit.TAIL_CHECK_KEY: record})["decision"] == "adopt"


def test_a_tail_check_without_reference_audio_is_skipped_and_blocks_nothing(monkeypatch, tmp_path):
    refit, voice, fit, rendered = _tail_voice(
        monkeypatch, tmp_path, candidate_slope=-10.0, ref_exists=False
    )
    record = refit.tail_check(voice, fit, Path("lib"))
    assert "no reference audio" in record["skipped"] and rendered == []
    assert _tail_verdict(refit, {**fit, refit.TAIL_CHECK_KEY: record})["decision"] == "adopt"


# A dimension the gate bounds and the objective could not weigh is judged after the fit.


def _unscorable_case(after, *, switch=False, dims=("decay",)):
    pre, fit, post = _case(*(GOOD[:3] + (switch,) + GOOD[4:]))
    fit["fit_profile"] = {"unscorable": list(dims)}
    row = {"key": "cap:decay", "stat": "abs_median", "limit": 9.0, "over": False, "excess": 0.0}
    pre["gate"]["readings"].append(dict(row, value=-1.0))
    post["gate"]["readings"].append(dict(row, value=after))
    return pre, fit, post


def test_a_fit_that_worsens_an_unscorable_dimension_past_its_floor_is_kept():
    result = verdict(*_unscorable_case(-1.5), floor_of=lambda key: 0.2)
    assert result["decision"] == "kept"
    assert result["why"] == (
        "unscorable decay worsened past its floor (abs_median -1->-1.5, floor 0.2)"
    )
    assert result["unscorable_worse"] == [
        {"key": "cap:decay", "stat": "abs_median", "before": -1.0, "after": -1.5, "floor": 0.2}
    ]
    assert result["moved_keys"] == [FLOAT]


def test_an_unscorable_dimension_moving_inside_its_floor_or_improving_does_not_block():
    for after in (-1.15, -0.2, 1.1):
        result = verdict(*_unscorable_case(after), floor_of=lambda key: 0.2)
        assert result["decision"] == "adopt" and result["unscorable_worse"] == []


def test_only_the_unscorable_dimensions_are_judged_by_their_floor():
    result = verdict(*_unscorable_case(-3.0, dims=("stereo",)), floor_of=lambda key: 0.2)
    assert result["decision"] == "adopt"


def test_the_unscorable_check_comes_before_a_trade():
    result = verdict(*_unscorable_case(-1.5, switch=True), floor_of=lambda key: 0.2)
    assert result["decision"] == "kept" and result["why"].startswith("unscorable decay")
    assert result["moved_keys"] == [FLOAT, SWITCH]


def test_an_unscorable_dimension_without_the_gates_floors_is_refused():
    with pytest.raises(ValueError, match="needs the gate's floors"):
        verdict(*_unscorable_case(-1.5))


def test_the_judged_voice_reads_the_floor_from_its_gate(tmp_path):
    import json as _json

    import refit

    gate = tmp_path / "cap_gate.json"
    gate.write_text(_json.dumps({"bounds": {"decay": {"median": 2.0, "floor": 0.0}}}))
    voice = {"capture": "cap", "gates": [{"capture": "cap", "gate": str(gate)}]}
    result = verdict(*_unscorable_case(-1.01), floor_of=refit.gate_floors(voice))
    assert result["decision"] == "kept" and "floor 0)" in result["why"]


def test_a_judge_library_older_than_the_newest_synth_commit_is_refused(tmp_path, monkeypatch):
    import os

    import refit

    lib = tmp_path / "libsonare.dylib"
    lib.write_bytes(b"")
    os.utime(lib, (1000.0, 1000.0))
    monkeypatch.setattr(refit, "newest_synth_commit", lambda: ("abc1234", 2000.0))
    with pytest.raises(RuntimeError, match=r"before abc1234 .* touched src/midi/synth"):
        refit.refuse_old_library(lib)
    os.utime(lib, (3000.0, 3000.0))
    refit.refuse_old_library(lib)


def _rows(centroid: float, crest: float) -> dict:
    """Per-note rows of a kit compare: the fitted note 71 and a neighbour."""
    rows = [
        {"note": 71, "velocity": 100, "centroid_pct": centroid, "crest": crest},
        {"note": 72, "velocity": 100, "centroid_pct": 1.0, "crest": 0.5},
    ]
    return {"drums_module_hit": rows}


def test_a_single_note_fit_is_kept_when_its_own_row_worsens_past_the_floor():
    """The kit-wide statistic does not see one note of sixty, so the note's own row is read."""
    pre, fit, post = _case(*GOOD)
    pre["note_rows"], post["note_rows"] = _rows(14.7, 3.1), _rows(24.9, 3.7)
    floors = {"drums_module_hit:centroid_pct": 5.0, "drums_module_hit:crest": 0.5}
    got = verdict(pre, fit, post, floor_of=floors.__getitem__, own_note=71)
    assert got["decision"] == "kept"
    assert (
        got["why"] == "own row centroid_pct n71v100 worsened past its floor (14.7->24.9, floor 5)"
    )
    assert [r["key"] for r in got["own_row_worse"]] == [
        "drums_module_hit:centroid_pct",
        "drums_module_hit:crest",
    ]
    # Within the floor, and on another note, nothing is held.
    post["note_rows"] = _rows(16.0, 3.3)
    assert verdict(pre, fit, post, floor_of=floors.__getitem__, own_note=71)["decision"] == "adopt"
    assert verdict(*_case(*GOOD))["decision"] == "adopt"


def test_a_single_note_fit_without_recorded_rows_is_refused():
    pre, fit, post = _case(*GOOD)
    post["note_rows"] = _rows(1.0, 1.0)
    with pytest.raises(ValueError, match="pre recorded no per-note rows"):
        verdict(pre, fit, post, floor_of=lambda key: 1.0, own_note=71)


def test_the_path_accepts_no_point_whose_own_row_the_verdict_would_keep(tmp_path, monkeypatch):
    import json as _json

    import refit

    over = {"key": "hit:tnr", "stat": "p90", "limit": 1.0}

    def body(i, rows=True):
        value = 0.8 - i / 100  # the kit statistic keeps improving all along the path
        out = {"gate": {"readings": [dict(over, value=value, over=False, excess=value - 1.0)]}}
        if rows:
            # The fitted note's own centroid worsens past its floor from the fifth point on.
            own = 1.0 + (10.0 if i >= 4 else 0.0)
            out["note_rows"] = {"hit": [{"note": 71, "velocity": 100, "centroid_pct": own}]}
        return out

    (tmp_path / "pre.json").write_text(_json.dumps(body(-1)))
    path = [
        {"eval": i + 1, "loss": 1.0 - i / 20, "stage": "fit", "values": [float(i)]}
        for i in range(10)
    ]
    report = {"knobs": [{"key": FLOAT, "start": -1.0}], "path": path}

    def fake_compare(voice, out, lib, overrides, **_):
        out.write_text(_json.dumps(body(int(float(overrides.split("=")[1])))))

    monkeypatch.setattr(gate_audit, "compare_voice", fake_compare)
    monkeypatch.setattr(refit, "gate_floors", lambda voice: lambda key: 5.0)
    found = refit.along_the_path({"drum_note": 71}, tmp_path, Path("lib"), report, "fit")
    assert found["accepted"]["eval"] == 4
    assert found["refused_point"]["own_row_worse"][0]["key"] == "hit:centroid_pct"

    def no_rows(voice, out, lib, overrides, **_):
        out.write_text(_json.dumps(body(int(float(overrides.split("=")[1])), rows=False)))

    for stale in tmp_path.glob("path_*.json"):
        stale.unlink()
    monkeypatch.setattr(gate_audit, "compare_voice", no_rows)
    with pytest.raises(ValueError, match="post recorded no per-note rows"):
        refit.along_the_path({"drum_note": 71}, tmp_path, Path("lib"), report, "fit")


def test_a_point_refused_only_by_its_own_row_is_kept_for_the_floor_and_names_the_row(tmp_path):
    import json as _json

    import refit

    own = {"key": "hit:crest", "note": 71, "velocity": 100, "before": 3.1, "after": 3.7}
    refused = {
        "bounds_ok": True,
        "excess_grew": False,
        "worse": [],
        "own_row_worse": [{**own, "floor": 0.5}],
        "file": "path_fit_5.json",
    }
    assert refit._kept_for({"accepted": None, "refused_point": refused}) == ("floor", refused)
    for name in ("pre.json", "path_fit_5.json"):
        (tmp_path / name).write_text(_json.dumps({"gate": {"readings": []}}))
    ver = {"kept_for": "floor", "refused_point": refused}
    rows = refit.blocking_rows(tmp_path, {"capture": "hit", "gates": []}, ver)
    assert rows == [
        {
            "gate": "hit",
            "dimension": "crest",
            "statistic": "n71v100",
            "before": 3.1,
            "after": 3.7,
            "bound": None,
            "floor": 0.5,
            "why": "floor",
        }
    ]
