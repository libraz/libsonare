"""A fit against several captures at once: the joint loss, its per-capture readings,
the per-capture hold-out and guards, and how the capture list reaches the records.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_autofit_joint.py -q
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import autofit_resolve
import fits
import policy
import refit
import report
from _repo import shared_library_name
from autofit import (
    Evaluator,
    JointEvaluator,
    capture_weight,
    joint_winner,
    validate_joint,
)
from autofit_test_fixtures import _fit_args, _terms, _write_corpus
from knobs import Knob, KnobClasses
from loss import LOSS_TERMS, LossWeights, cli_weights
from loss_weights import TERM_UNITS

STUB_POLICY = {
    "reference_layer": {
        "default": {"timbre": "instrument", "behaviour": "instrument"},
        "machine_defined": {"timbre": "machine", "behaviour": "machine", "programs": [127]},
    }
}


@pytest.fixture(autouse=True)
def _slot_aimed_at_nothing(monkeypatch):
    """The synthetic captures here carry no source class; the layer tests set their own policy."""
    monkeypatch.setattr(policy, "load", lambda path=None: {})


KNOB = Knob(label="x.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="x.k")
START = [0.5]


def _args(**weights):
    """Only the named terms weigh; everything else is zero, so a test owns its terms."""
    zeros = {f"w_{t}": 0.0 for t in LOSS_TERMS}
    return _fit_args(**{**zeros, **{f"w_{t}": w for t, w in weights.items()}})


def _units(term: str, units: float) -> float:
    return units * TERM_UNITS[term]


def _member(tmp_path, monkeypatch, terms_of, **weights) -> Evaluator:
    """An `Evaluator` whose renders are `terms_of(values)`, with no build behind it."""
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    member = Evaluator([KNOB], {}, [{"note": 60}], None, _args(**weights), tmp_path)
    member._render_terms = lambda values, **_: terms_of(values)
    return member


def _joint(*members, labels=None) -> JointEvaluator:
    labels = labels or [f"cap{i}" for i in range(len(members))]
    return JointEvaluator(list(members), labels, [KNOB], members[0].args)


def _harm(units_at):
    """Terms whose `harm` is `units_at(value)` perceptual units."""
    return lambda values, **_: _terms(harm=_units("harm", units_at(values[0])))


# --------------------------------------------------------------------------- #
# The joint loss
# --------------------------------------------------------------------------- #
def test_each_capture_is_normalised_against_its_own_start(tmp_path, monkeypatch):
    """Raw units differ by capture; the joint reads 1.0 at the start whatever they are."""
    a = _member(tmp_path, monkeypatch, _harm(lambda v: 10.0 * abs(v - 0.2)), harm=1.0)
    b = _member(tmp_path, monkeypatch, _harm(lambda v: 40.0 * abs(v - 0.8)), harm=1.0)
    joint = _joint(a, b)
    assert joint(START) == pytest.approx(1.0, abs=1e-12)
    # a: 3 -> 0 (ratio 0), b: 12 -> 24 (ratio 2); the mean of the two ratios.
    assert joint([0.2]) == pytest.approx(1.0)
    assert joint.capture_losses([0.2]) == pytest.approx([0.0, 2.0])
    assert len(joint.trajectory) == 2
    assert joint.best_values == START


def test_a_capture_starting_near_perfect_counts_in_proportion(tmp_path, monkeypatch):
    """Its calibrated ratio would turn measurement noise into a large loss; the floor stops that."""
    tiny = _member(
        tmp_path, monkeypatch, _harm(lambda v: 0.01 * (1.0 + 1.0 * abs(v - 0.5) / 0.5)), harm=1.0
    )
    big = _member(tmp_path, monkeypatch, _harm(lambda v: 5.0), harm=1.0)
    joint = _joint(tiny, big)
    assert joint(START) == pytest.approx(1.0, abs=1e-12)
    assert capture_weight(tiny.loss) == pytest.approx(0.01)
    assert capture_weight(big.loss) == 1.0
    # tiny doubles (ratio 2), big holds (ratio 1). Unfloored that is 1.5.
    moved = joint([1.0])
    assert tiny.loss_for([1.0]) == pytest.approx(2.0)
    assert moved == pytest.approx((0.01 * 2.0 + 1.0) / 1.01)
    assert moved < 1.01


def test_every_capture_starting_perfect_still_scores(tmp_path, monkeypatch):
    """No capture above the floor must not divide by zero: they all count equally."""
    a = _member(tmp_path, monkeypatch, _harm(lambda v: 0.0), harm=1.0)
    b = _member(tmp_path, monkeypatch, _harm(lambda v: 0.0), harm=1.0)
    joint = _joint(a, b)
    assert joint(START) == 0.0
    assert capture_weight(a.loss) == 0.0
    assert joint.capture_weights() == [1.0, 1.0]


def test_a_gain_on_one_capture_bought_at_the_others_cost_shows_in_per_capture(
    tmp_path, monkeypatch
):
    def gains(v):  # halves at 0.2
        return _terms(
            harm=_units("harm", 4.0) * (1.0 - 0.5 * (v[0] < 0.4)), cents=_units("cents", 4.0)
        )

    def loses(v):  # the same move makes cents 1.5x worse
        return _terms(
            harm=_units("harm", 4.0), cents=_units("cents", 4.0) * (1.0 + 0.5 * (v[0] < 0.4))
        )

    a = _member(tmp_path, monkeypatch, gains, harm=1.0, cents=1.0)
    b = _member(tmp_path, monkeypatch, loses, harm=1.0, cents=1.0)
    joint = _joint(a, b, labels=["nylon:t0", "ukulele:t0"])
    joint(START)
    moved = [0.2]
    joint(moved)
    # harm 4->2 and cents 4->4 on a (ratio 0.75); harm 4->4 and cents 4->6 on b (1.25).
    assert joint.capture_losses(moved) == pytest.approx([0.75, 1.25])
    assert joint(moved) == pytest.approx(1.0), "the joint alone cannot tell this from no change"

    rows = report.per_capture_rows(joint, START, moved, moved, {})
    by = {r["label"]: r for r in rows}
    assert by["nylon:t0"]["loss"]["search"] == pytest.approx(0.75)
    assert by["ukulele:t0"]["loss"]["search"] == pytest.approx(1.25)
    assert by["nylon:t0"]["search_winner"]["worse_terms"] == []
    assert by["ukulele:t0"]["search_winner"]["worse_terms"] == ["cents"]
    assert (
        by["ukulele:t0"]["terms"]["cents"]["selected_units"]
        > by["ukulele:t0"]["terms"]["cents"]["start_units"]
    )


def test_a_fence_tripped_on_one_capture_is_paid_at_full_strength(tmp_path, monkeypatch):
    def drifting(v):
        return _terms(harm=_units("harm", 4.0), level_offset_db=10.0 * v[0])

    quiet = _member(tmp_path, monkeypatch, _harm(lambda v: 4.0), harm=1.0)
    drift = _member(tmp_path, monkeypatch, drifting, harm=1.0)
    for member in (quiet, drift):
        member.args.max_level_drift_db = 1.0
    joint = _joint(quiet, drift)
    joint(START)
    # 5 dB of level against the 1 dB allowance: 4 dB over, 0.1 per dB on a start scoring 1.0.
    joint([1.0])
    penalty = drift._level_drift_penalty(drift.terms_for([1.0]))
    assert penalty == pytest.approx(0.1 * 4.0)
    assert joint([1.0]) == pytest.approx(1.0 + penalty)


def test_one_capture_scores_the_same_through_the_joint_as_alone(tmp_path, monkeypatch):
    """A single --corpus never builds a joint, so its loss is the evaluator's own."""
    terms_of = lambda v: _terms(harm=_units("harm", 3.0 + 2.0 * v[0]), cents=_units("cents", 2.0))
    alone = _member(tmp_path, monkeypatch, terms_of, harm=1.0, cents=1.0)
    inside = _joint(_member(tmp_path, monkeypatch, terms_of, harm=1.0, cents=1.0))
    reference = LossWeights(cli_weights(alone.args))
    reference.anchor(terms_of(START))
    reference.calibrate(terms_of(START))
    for value in (0.5, 0.9, 0.1):
        assert alone([value]) == reference.combine(terms_of([value]))
        assert inside([value]) == pytest.approx(alone([value]), abs=1e-12)
    # Pinned: harm 4 units of 4 at start, 4.8 at 0.9; cents flat, so (4.8 + 2) / (4 + 2).
    assert alone([0.9]) == pytest.approx(6.8 / 6.0)


# --------------------------------------------------------------------------- #
# Hold-out, per capture
# --------------------------------------------------------------------------- #
class _Held:
    """A stand-in for one capture's hold-out scorer."""

    def __init__(self, best, weight=1.0, axis="notes", held="48,72"):
        self.best, self._weight = best, weight
        self.axis, self.held = axis, held
        self.last_quality = {"full_known_coverage": True}
        self.calls = 0

    def __call__(self, values):
        self.calls += 1
        return 1.0 if self.calls == 1 else self.best

    def weight(self):
        return self._weight


def _captures(*labels):
    return [
        argparse.Namespace(label=label, args=argparse.Namespace(), ir_path=None, reach=None)
        for label in labels
    ]


def _serve(monkeypatch, scorers):
    queue = iter(scorers)

    def fake(args, build_dir, knobs, room_ir, reach=None):
        scorer = next(queue)
        return None if scorer is None else (scorer, scorer.axis, scorer.held)

    monkeypatch.setattr(autofit, "holdout_scorer", fake)


def test_each_capture_is_held_out_on_its_own_partition_and_named_if_worse(monkeypatch):
    _serve(monkeypatch, [_Held(0.8, held="48,72"), _Held(1.2, held="50,74")])
    result = validate_joint(_captures("nylon:t0", "ukulele:t0"), None, [KNOB], START, [0.2])
    assert [p["held_out"] for p in result["per_capture"]] == ["48,72", "50,74"]
    assert [p["worse"] for p in result["per_capture"]] == [False, True]
    assert result["worse"] == ["ukulele:t0"]
    assert result["start"] == pytest.approx(1.0)
    assert result["best"] == pytest.approx(1.0)  # (0.8 + 1.2) / 2
    assert "independent" not in result


def test_the_joint_hold_out_weighs_a_near_perfect_capture_down(monkeypatch):
    _serve(monkeypatch, [_Held(2.0, weight=0.01), _Held(0.9, weight=1.0)])
    result = validate_joint(_captures("a", "b"), None, [KNOB], START, [0.2])
    assert result["best"] == pytest.approx((0.01 * 2.0 + 0.9) / 1.01)
    assert result["worse"] == ["a"]


def test_a_capture_with_no_hold_out_makes_the_joint_one_not_independent(monkeypatch):
    _serve(monkeypatch, [_Held(0.9), None])
    result = validate_joint(_captures("a", "b"), None, [KNOB], START, [0.2])
    assert result["independent"] is False
    assert result["uncovered"] == ["b"]
    assert result["per_capture"][1] == {"capture": "b", "scored": False}


def test_no_hold_out_anywhere_is_no_result(monkeypatch):
    _serve(monkeypatch, [None, None])
    assert validate_joint(_captures("a", "b"), None, [KNOB], START, [0.2]) is None


def test_the_partition_is_checked_per_capture_and_names_the_offender(tmp_path):
    ok = _fit_args(corpus=str(_write_corpus(tmp_path / "a", notes=(60, 72))), validate_notes="48")
    bad = _fit_args(corpus=str(_write_corpus(tmp_path / "b", notes=(60, 72))), validate_notes="60")
    assert autofit.partition_joint([ok], ["a"])["independent"] is True
    with pytest.raises(ValueError, match=r"capture b: --validate-notes overlaps"):
        autofit.partition_joint([ok, bad], ["a", "b"])


# --------------------------------------------------------------------------- #
# Guards: any capture tripping trips the fit
# --------------------------------------------------------------------------- #
def _blind_on_second(v):
    notes = 0.0 if v[0] > 0.7 else 3.0
    return _terms(harm=_units("harm", 4.0 - 2.0 * v[0]), stiff=1.0, stiff_notes=notes)


def _steady(v):
    return _terms(harm=_units("harm", 4.0 - 2.0 * v[0]), stiff=1.0, stiff_notes=3.0)


def _two(tmp_path, monkeypatch, second):
    a = _member(tmp_path, monkeypatch, _steady, harm=1.0, stiff=0.1)
    b = _member(tmp_path, monkeypatch, second, harm=1.0, stiff=0.1)
    joint = _joint(a, b, labels=["a:t0", "b:t0"])
    for value in ([0.5], [0.6], [0.9]):
        joint(value)
    return joint


def test_a_guard_tripped_by_the_second_capture_keeps_the_defaults(tmp_path, monkeypatch):
    joint = _two(tmp_path, monkeypatch, _blind_on_second)
    assert joint.best_values == [0.9]
    assert joint_winner([KNOB], [0.9], joint, None) == START
    assert joint.write_back_refusal == "measurement_dropout"
    assert joint.refused_by == ["b:t0"]
    assert joint.members[0].write_back_refusal is None


def test_the_same_winner_is_kept_when_no_capture_trips(tmp_path, monkeypatch):
    joint = _two(tmp_path, monkeypatch, _steady)
    assert joint_winner([KNOB], [0.9], joint, None) == [0.9]
    assert joint.write_back_refusal is None
    assert joint.members[1].selected_values == [0.9]


def test_losing_to_the_start_is_a_joint_reading_not_a_capture_one(tmp_path, monkeypatch):
    """One capture ending above its start does not refuse a fit the joint loss improves."""
    a = _member(tmp_path, monkeypatch, _harm(lambda v: 4.0 - 3.0 * v), harm=1.0)
    b = _member(tmp_path, monkeypatch, _harm(lambda v: 4.0 + 1.0 * v), harm=1.0)
    joint = _joint(a, b)
    joint(START)
    joint([0.9])
    assert joint.capture_losses([0.9])[1] > 1.0 > joint.best_loss
    assert joint_winner([KNOB], [0.9], joint, None) == [0.9]


def test_the_joint_hold_out_refuses_and_names_the_capture_that_got_worse(tmp_path, monkeypatch):
    joint = _two(tmp_path, monkeypatch, _steady)
    held = {
        "axis": "notes",
        "start": 1.0,
        "best": 1.05,
        "worse": ["b:t0"],
        "per_capture": [],
    }
    assert joint_winner([KNOB], [0.9], joint, held) == START
    assert joint.write_back_refusal == "fitted_to_the_probe"
    assert joint.refused_by == ["b:t0"]


def test_a_capture_worse_held_out_refuses_even_when_the_joint_improves(tmp_path, monkeypatch):
    joint = _two(tmp_path, monkeypatch, _steady)
    held = {
        "axis": "notes",
        "start": 1.0,
        "best": 0.9,  # the joint improves ...
        "worse": ["b:t0"],  # ... but one capture's own hold-out got worse past the margin
        "per_capture": [],
    }
    assert joint_winner([KNOB], [0.9], joint, held) == START
    assert joint.write_back_refusal == "fitted_to_the_probe"
    assert joint.refused_by == ["b:t0"]


def test_a_capture_within_the_margin_does_not_refuse(monkeypatch, tmp_path):
    _serve(monkeypatch, [_Held(0.5), _Held(1.004)])
    result = validate_joint(_captures("a:t0", "b:t0"), None, [KNOB], START, [0.9])
    assert result["worse"] == []
    joint = _two(tmp_path, monkeypatch, _steady)
    assert joint_winner([KNOB], [0.9], joint, result) == [0.9]


def test_per_capture_flags_a_capture_that_ends_above_its_start(tmp_path, monkeypatch):
    a = _member(tmp_path, monkeypatch, _harm(lambda v: 4.0 - 3.0 * v), harm=1.0)
    b = _member(tmp_path, monkeypatch, _harm(lambda v: 4.0 + 1.0 * v), harm=1.0)
    joint = _joint(a, b, labels=["a:t0", "b:t0"])
    joint(START)
    joint([0.9])
    rows = {r["label"]: r for r in report.per_capture_rows(joint, START, [0.9], [0.9], {})}
    assert rows["a:t0"]["worse_than_start"] is False
    assert rows["b:t0"]["worse_than_start"] is True


def test_the_per_capture_line_says_a_capture_ended_above_its_start(capsys):
    row = {
        "label": "b:t0",
        "weight": 1.0,
        "worse_than_start": True,
        "loss": {"start": 1.0, "search": 1.2, "selected": 1.2},
        "search_winner": {"regressions": []},
    }
    report.print_per_capture([row])
    assert "KEPT ABOVE ITS START" in capsys.readouterr().out


# --------------------------------------------------------------------------- #
# What names the captures
# --------------------------------------------------------------------------- #
def test_per_capture_flags_are_aligned_with_corpus_by_position(tmp_path):
    args = argparse.Namespace(
        corpus=["a", "b"],
        corpus_timbre=["t0", ""],
        corpus_notes=["36,48", "40"],
        corpus_velocities=None,
        corpus_validate_notes=["60", ""],
        corpus_validate_velocities=None,
    )
    specs = autofit_resolve.capture_specs(args)
    assert [(s.path, s.timbre, s.notes, s.validate_notes) for s in specs] == [
        ("a", "t0", "36,48", "60"),
        ("b", "", "40", ""),
    ]
    args.corpus_timbre = ["only-one"]
    with pytest.raises(ValueError, match="--corpus-timbre is given 1 time"):
        autofit_resolve.capture_specs(args)


def test_a_single_corpus_keeps_its_plain_shape(tmp_path):
    parser = autofit.build_parser()
    got = parser.parse_args(
        ["--spec", "auto", "--program", "0", "--corpus", "x", "--corpus-timbre", "t"]
    )
    specs = autofit_resolve.capture_specs(got)
    assert [(s.path, s.timbre) for s in specs] == [("x", "t")]
    assert (
        autofit_resolve.capture_specs(parser.parse_args(["--spec", "auto", "--program", "0"])) == []
    )
    again = parser.parse_args(
        ["--spec", "auto", "--program", "0", "--corpus", "x", "--corpus", "y"]
    )
    assert [s.path for s in autofit_resolve.capture_specs(again)] == ["x", "y"]


def test_the_probe_flags_hold_only_where_every_capture_can_measure(tmp_path):
    long_ = _write_corpus(tmp_path / "long", notes=(60, 72))
    short = _write_corpus(tmp_path / "short", notes=(60, 72), gate_ms=500, seconds=1.2)
    args = _fit_args(corpus=[str(long_), str(short)], corpus_timbre=None)
    caps = autofit_resolve.resolve_probes(args)
    assert len(caps) == 2
    assert caps[0].has_tail_window is False and args.has_tail_window is False
    with pytest.raises(ValueError, match="--w-tail"):
        autofit_resolve.resolve_probes(
            _fit_args(corpus=[str(long_), str(short)], corpus_timbre=None, w_tail=1.0)
        )


# --------------------------------------------------------------------------- #
# The record
# --------------------------------------------------------------------------- #
def test_the_fit_record_carries_the_capture_list():
    report_ = {
        "objective": "d",
        "knobs": [{"key": "nylon.a"}],
        "corpora": [
            {"capture": "nylon", "timbre": "t0"},
            {"capture": "ukulele", "timbre": "t1"},
        ],
    }
    unit, entry = fits.record_fit(report_, capture="nylon", timbre="t0", outcome="adopted")
    assert unit == "nylon"
    assert entry["corpora"] == [
        {"capture": "nylon", "timbre": "t0"},
        {"capture": "ukulele", "timbre": "t1"},
    ]
    assert (entry["capture"], entry["timbre"]) == ("nylon", "t0")


def test_a_single_capture_fit_records_a_list_of_one():
    _, entry = fits.record_fit(
        {"objective": "d", "knobs": [{"key": "nylon.a"}]},
        capture="nylon",
        timbre="t0",
        outcome="kept",
    )
    assert entry["corpora"] == [{"capture": "nylon", "timbre": "t0"}]


def test_the_out_record_has_the_corpora_and_a_per_capture_block(tmp_path, monkeypatch, capsys):
    a = _member(tmp_path, monkeypatch, _steady, harm=1.0)
    b = _member(tmp_path, monkeypatch, _steady, harm=1.0)
    joint = _joint(a, b, labels=["a:t0", "b:t0"])
    for value in ([0.5], [0.9]):
        joint(value)
    out = tmp_path / "result.json"
    args = _args(harm=1.0)
    args.out, args.dry_run, args.drum_note, args.program = str(out), True, None, 0
    joint.args = args
    corpora = [
        {"label": "a:t0", "capture": "a", "timbre": "t0"},
        {"label": "b:t0", "capture": "b", "timbre": "t0"},
    ]
    report.report_result(
        [KNOB],
        {},
        [0.9],
        joint,
        args,
        {
            "corpora": corpora,
            "captures": corpora,
            "validation": None,
            "oracle": {"cache": "c"},
            "scored_without": ["mss"],
        },
    )
    record = json.loads(out.read_text())
    assert [c["capture"] for c in record["corpora"]] == ["a", "b"]
    # `fits.record_fit` and the verdict read these off the record, never off the run.
    assert record["oracle"] == {"cache": "c"} and record["scored_without"] == ["mss"]
    assert [r["label"] for r in record["per_capture"]] == ["a:t0", "b:t0"]
    assert all({"start", "search", "selected"} <= set(r["loss"]) for r in record["per_capture"])
    assert "== per capture" in capsys.readouterr().out


# --------------------------------------------------------------------------- #
# refit --joint
# --------------------------------------------------------------------------- #
def _voice(tmp_path):
    return {
        "slug": "p024",
        "program": 24,
        "bank": 0,
        "patch": "nylon",
        "capture": "nylon",
        "timbre": "t0",
        "corpus": str(tmp_path / "scratch" / "capture" / "nylon"),
        "notes": [36, 60],
        "validate_notes": [48, 72],
        "velocities": [40, 80],
        "gates": [
            {"capture": "nylon", "gate": "g", "timbre": "t0", "patch": "nylon"},
            {"capture": "ukulele", "gate": "g", "timbre": "t1", "patch": "ukulele"},
            {"capture": "gone", "gate": "g", "timbre": "t0", "patch": "gone"},
            {"capture": "single", "gate": "g", "timbre": "t0", "patch": "single"},
        ],
    }


def _refit_world(tmp_path, monkeypatch):
    scratch, defs = tmp_path / "scratch", tmp_path / "defs"
    defs.mkdir()
    monkeypatch.setattr(refit, "CORPUS_ROOT", scratch)
    monkeypatch.setattr(refit, "CAPTURE_DIR", defs)
    for cap, notes in (("ukulele", [40, 52, 64, 76]), ("single", [50])):
        (scratch / "capture" / cap).mkdir(parents=True)
        (scratch / "capture" / cap / "manifest.json").write_text("{}")
        (defs / f"{cap}.json").write_text(
            json.dumps({"notes": notes, "velocities": [30, 60, 90, 120]})
        )


def test_joint_names_every_gate_capture_it_can_fit_and_says_why_not_the_others(
    tmp_path, monkeypatch
):
    _refit_world(tmp_path, monkeypatch)
    usable, dropped = refit.joint_captures(_voice(tmp_path))
    assert [u["capture"] for u in usable] == ["nylon", "ukulele"]
    assert usable[1]["notes"] == [40, 64] and usable[1]["validate_notes"] == [52, 76]
    assert usable[1]["timbre"] == "t1"
    assert {d["capture"]: d["reason"] for d in dropped} == {
        "gone": "no corpus on this machine",
        "single": "one note: nothing to hold out",
    }


def test_a_voice_with_one_usable_capture_is_not_a_joint_fit(tmp_path, monkeypatch):
    _refit_world(tmp_path, monkeypatch)
    voice = _voice(tmp_path)
    voice["gates"] = voice["gates"][:1]
    assert refit.joint_captures(voice)[0] == []


def test_refit_joint_passes_every_gate_capture_to_autofit(tmp_path, monkeypatch):
    _refit_world(tmp_path, monkeypatch)
    voice = _voice(tmp_path)
    voice["joint"], _ = refit.joint_captures(voice)
    seen = []

    def fake_run(cmd, **kwargs):
        seen.append(cmd)
        (tmp_path / "fit.json").write_text("{}")

    monkeypatch.setattr(refit.subprocess, "run", fake_run)
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    refit._fit(voice, tmp_path / "fit.json", "build", 12)
    cmd = seen[0]
    assert cmd.count("--corpus") == 2
    assert [cmd[i + 1] for i, c in enumerate(cmd) if c == "--corpus"] == [
        voice["corpus"],
        str(tmp_path / "scratch" / "capture" / "ukulele"),
    ]
    assert [cmd[i + 1] for i, c in enumerate(cmd) if c == "--corpus-timbre"] == ["t0", "t1"]
    assert [cmd[i + 1] for i, c in enumerate(cmd) if c == "--corpus-validate-notes"] == [
        "48,72",
        "52,76",
    ]
    assert "--notes" not in cmd and "--validate-notes" not in cmd


def test_without_joint_a_fit_names_its_own_capture_only(tmp_path, monkeypatch):
    voice = _voice(tmp_path)
    seen = []
    monkeypatch.setattr(
        refit.subprocess,
        "run",
        lambda cmd, **k: (seen.append(cmd), (tmp_path / "fit.json").write_text("{}")),
    )
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    refit._fit(voice, tmp_path / "fit.json", "build", 12)
    cmd = seen[0]
    assert cmd.count("--corpus") == 1 and "--notes" in cmd and "--validate-notes" in cmd
    assert "--corpus-notes" not in cmd


def test_the_joint_flag_is_off_unless_asked_for():
    assert refit.JOINT is False


# --------------------------------------------------------------------------- #
# The whole run
# --------------------------------------------------------------------------- #
def test_a_joint_run_reports_each_capture_and_records_both(tmp_path, monkeypatch):
    """Two --corpus through `run`: one value vector, a record with the capture list and a block per capture."""
    import numpy as np
    from catalogue import Catalogue

    lib = tmp_path / shared_library_name()
    lib.write_bytes(b"lib")
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", []))
    monkeypatch.setattr(autofit, "src_files", list)
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "_fold_gate_audit", lambda *a, **k: None)
    catalogue = Catalogue(
        defaults={"nylon.gain": 0.5, "piano_voice.kHammerWidthHarmonics": 5.0},
        programs={(0, 0): "nylon", (1, 0): "ukulele"},
        bounds={},
        modes={},
        rigs={},
    )
    monkeypatch.setattr(autofit, "_tuning_catalogue", lambda *a, **k: catalogue)
    monkeypatch.setattr(autofit_resolve, "capture_program", lambda corpus: (1, 0))
    monkeypatch.setattr(
        autofit, "oracle_reference", lambda args: ([], np.zeros(4, dtype=np.float32), None, None)
    )
    rendered = []

    def terms(self, values, **_):
        rendered.append((self.args.program, None if self.reach is None else self.reach.signature))
        v = values[0] / 9.0  # the knob runs 2.5 to 9
        if self.args.program == 0:
            return _terms(harm=_units("harm", 14.0 - 12.0 * v), cents=_units("cents", 4.0))
        return _terms(harm=_units("harm", 10.0), cents=_units("cents", 3.0 + 6.0 * v))

    monkeypatch.setattr(Evaluator, "_render_terms", terms)
    scorers = {0: _Held(0.7, held="48"), 1: _Held(1.3, held="50")}

    def holdout(args, build_dir, knobs, room_ir, reach=None):
        scorer = scorers[args.program]
        return scorer, scorer.axis, scorer.held

    monkeypatch.setattr(autofit, "holdout_scorer", holdout)

    spec = tmp_path / "spec.json"
    spec.write_text(json.dumps([{"tunable": "kHammerWidthHarmonics", "min": 2.5, "max": 9.0}]))
    nylon = _write_corpus(tmp_path / "nylon", notes=(60, 72))
    ukulele = _write_corpus(tmp_path / "ukulele", notes=(60, 72))
    for root, name in ((nylon, "nylon"), (ukulele, "ukulele")):
        manifest = json.loads((root / "manifest.json").read_text())
        (root / "manifest.json").write_text(json.dumps({**manifest, "id": name}))
    out = tmp_path / "out.json"
    args = autofit.build_parser().parse_args(
        [
            *("--spec", str(spec), "--program", "0", "--lib", str(lib), "--dry-run"),
            *("--corpus", str(nylon), "--corpus", str(ukulele)),
            *("--corpus-validate-notes", "48", "--corpus-validate-notes", "50"),
            *("--optimizer", "coord", "--max-evals", "12", "--no-cache"),
            *("--w-harm", "1", "--w-cents", "1", "--out", str(out)),
        ]
    )
    assert autofit.run(args, []) == 0

    record = json.loads(out.read_text())
    assert [(c["program"], c["reach"]) for c in record["corpora"]] == [
        (0, None),
        (1, "nylon->ukulele"),
    ]
    assert {r for r in rendered} >= {(0, None), (1, "nylon->ukulele")}
    assert [r["label"] for r in record["per_capture"]] == [c["label"] for c in record["corpora"]]
    first, second = record["per_capture"]
    assert first["loss"]["start"] == pytest.approx(1.0)
    assert first["loss"]["search"] < 1.0 < second["loss"]["search"]
    assert second["search_winner"]["worse_terms"] == ["cents"]
    assert [p["worse"] for p in record["validation"]["per_capture"]] == [False, True]
    assert record["validation"]["worse"] == [record["corpora"][1]["label"]]
    assert record["loss"]["start"] == pytest.approx(1.0)
    assert len(record["write_back"]["per_capture"]) == 2


def test_a_joint_run_dumps_the_catalogue_once_at_the_primary_rate(tmp_path, monkeypatch):
    """Two --corpus, --lib and a hand spec with a dotted key reach the dump without a single-capture lookup."""
    import numpy as np
    from catalogue import Catalogue

    lib = tmp_path / shared_library_name()
    lib.write_bytes(b"lib")
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", []))
    monkeypatch.setattr(autofit, "src_files", list)
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "_fold_gate_audit", lambda *a, **k: None)
    catalogue = Catalogue(
        defaults={"nylon.gain": 0.5},
        programs={(0, 0): "nylon", (1, 0): "ukulele"},
        bounds={},
        modes={},
        rigs={},
    )
    dumps = []

    def fake_dump(program, pattern, lib_path, **kw):
        dumps.append((program, kw["sr"]))
        return catalogue

    monkeypatch.setattr(autofit, "dump_catalogue", fake_dump)
    monkeypatch.setattr(autofit, "load_knob_classes", lambda path, cat: KnobClasses({}))
    monkeypatch.setattr(autofit_resolve, "capture_program", lambda corpus: (1, 0))
    monkeypatch.setattr(
        autofit, "oracle_reference", lambda args: ([], np.zeros(4, dtype=np.float32), None, None)
    )
    monkeypatch.setattr(
        Evaluator,
        "_render_terms",
        lambda self, values, **_: _terms(harm=_units("harm", 14.0 - 12.0 * values[0])),
    )
    scorers = {0: _Held(0.7, held="48"), 1: _Held(0.7, held="50")}
    monkeypatch.setattr(
        autofit,
        "holdout_scorer",
        lambda args, build_dir, knobs, room_ir, reach=None: (
            scorers[args.program],
            scorers[args.program].axis,
            scorers[args.program].held,
        ),
    )
    spec = tmp_path / "spec.json"
    spec.write_text(json.dumps([{"tunable": "nylon.gain", "min": 0.0, "max": 1.0}]))
    nylon = _write_corpus(tmp_path / "nylon", notes=(60, 72))
    ukulele = _write_corpus(tmp_path / "ukulele", notes=(60, 72))
    args = autofit.build_parser().parse_args(
        [
            *("--spec", str(spec), "--program", "0", "--lib", str(lib), "--dry-run"),
            *("--corpus", str(nylon), "--corpus", str(ukulele)),
            *("--corpus-validate-notes", "48", "--corpus-validate-notes", "50"),
            *("--optimizer", "coord", "--max-evals", "6", "--no-cache"),
            *("--w-harm", "1", "--out", str(tmp_path / "out.json")),
        ]
    )
    assert autofit.run(args, []) == 0
    assert dumps and {sr for _, sr in dumps} == {48000}
    assert {program for program, _ in dumps} == {0}


def test_primary_corpus_is_the_first_capture_and_none_without_one(tmp_path):
    first = _write_corpus(tmp_path / "a", notes=(60,))
    second = _write_corpus(tmp_path / "b", notes=(60,))
    args = argparse.Namespace(corpus=[str(first), str(second)], corpus_timbre=None)
    assert Path(autofit_resolve.primary_corpus(args).root) == first
    assert autofit_resolve.primary_corpus(argparse.Namespace(corpus="")) is None
    with pytest.raises(ValueError, match="several captures"):
        autofit_resolve.resolve_corpus(args)


# --------------------------------------------------------------------------- #
# the layer a capture answers
# --------------------------------------------------------------------------- #
def _aim(monkeypatch):
    monkeypatch.setattr(policy, "load", lambda path=None: STUB_POLICY)


def _layer_world(tmp_path, monkeypatch, classes):
    """The refit world with one extra gate capture per entry of @p classes (id -> source class)."""
    _refit_world(tmp_path, monkeypatch)
    for cap, source_class in classes.items():
        (tmp_path / "scratch" / "capture" / cap).mkdir(parents=True)
        (tmp_path / "scratch" / "capture" / cap / "manifest.json").write_text("{}")
        (tmp_path / "defs" / f"{cap}.json").write_text(
            json.dumps(
                {
                    "notes": [40, 52, 64, 76],
                    "velocities": [30, 60, 90, 120],
                    "source_class": source_class,
                }
            )
        )
    _aim(monkeypatch)


def _layer_voice(tmp_path, own_class, others, *, program=0):
    gates = [{"capture": "nylon", "gate": "g", "timbre": "t0", "patch": "nylon"}]
    gates += [{"capture": c, "gate": "g", "timbre": "t0", "patch": c} for c in others]
    voice = _voice(tmp_path)
    voice.update(program=program, bank=0, gates=gates, source_class=own_class)
    return voice


def test_a_joint_fit_takes_only_the_captures_on_the_wanted_layer(tmp_path, monkeypatch):
    _layer_world(tmp_path, monkeypatch, {"lib": "library", "mod": "module"})
    voice = _layer_voice(tmp_path, "library", ["lib", "mod"])
    usable, dropped = refit.joint_captures(voice)
    assert [u["capture"] for u in usable] == ["nylon", "lib"]
    assert dropped == [
        {
            "capture": "mod",
            "layer": "off-layer",
            "reason": "comparison (off-layer): source class module",
        }
    ]


def test_a_machine_defined_slot_takes_the_module_and_leaves_the_library_a_comparison(
    tmp_path, monkeypatch
):
    _layer_world(tmp_path, monkeypatch, {"lib": "library", "mod": "module"})
    voice = _layer_voice(tmp_path, "module", ["lib", "mod"], program=127)
    usable, dropped = refit.joint_captures(voice)
    assert [u["capture"] for u in usable] == ["nylon", "mod"]
    assert [(d["capture"], d["layer"]) for d in dropped] == [("lib", "off-layer")]


def test_an_unclassified_capture_is_never_a_fit_target(tmp_path, monkeypatch):
    _layer_world(tmp_path, monkeypatch, {"lib": "library", "bare": None})
    usable, dropped = refit.joint_captures(_layer_voice(tmp_path, "library", ["lib", "bare"]))
    assert [u["capture"] for u in usable] == ["nylon", "lib"]
    assert "unclassified" in dropped[0]["reason"]


def test_a_voice_whose_own_capture_is_off_layer_is_not_fitted_jointly(tmp_path, monkeypatch):
    _layer_world(tmp_path, monkeypatch, {"lib": "library"})
    usable, dropped = refit.joint_captures(_layer_voice(tmp_path, "module", ["lib"]))
    assert usable == [] and dropped[0]["capture"] == "nylon"


def _hand_listed(tmp_path, classes, **given):
    caps = {}
    for cap in classes:
        root = _write_corpus(tmp_path / cap, notes=(60, 72))
        caps[cap] = root
    return _fit_args(
        corpus=[str(r) for r in caps.values()],
        corpus_timbre=None,
        program=0,
        bank=0,
        drum_note=None,
        **given,
    )


def _serve_definitions(monkeypatch, classes):
    monkeypatch.setattr(
        autofit.bank,
        "captures",
        lambda: [argparse.Namespace(id=cid, source_class=sc) for cid, sc in classes.items()],
    )
    monkeypatch.setattr(
        autofit,
        "open_corpus",
        lambda args, spec: argparse.Namespace(
            capture_id=Path(spec.path).name, timbre="t", root=Path(spec.path)
        ),
    )


def test_a_hand_listed_off_layer_capture_is_refused_without_the_flag(tmp_path, monkeypatch):
    classes = {"lib": "library", "mod": "module"}
    _aim(monkeypatch)
    _serve_definitions(monkeypatch, classes)
    args = _hand_listed(tmp_path, classes)
    with pytest.raises(ValueError, match=r"off-layer capture\(s\) mod:t.*--allow-off-layer"):
        autofit.check_capture_layers(args)


def test_the_allow_flag_fits_an_off_layer_capture_and_names_it(tmp_path, monkeypatch, capsys):
    classes = {"lib": "library", "mod": "module"}
    _aim(monkeypatch)
    _serve_definitions(monkeypatch, classes)
    args = _hand_listed(tmp_path, classes, allow_off_layer=True)
    layers = autofit.check_capture_layers(args)
    assert layers == {"lib:t": "wanted", "mod:t": "off-layer"}
    assert "--allow-off-layer: fitting off-layer capture(s) mod:t" in capsys.readouterr().err


def test_captures_on_the_wanted_layer_and_a_single_capture_pass_unremarked(tmp_path, monkeypatch):
    classes = {"lib": "library", "other": "dedicated"}
    _aim(monkeypatch)
    _serve_definitions(monkeypatch, classes)
    assert set(autofit.check_capture_layers(_hand_listed(tmp_path, classes)).values()) == {"wanted"}
    single = _hand_listed(tmp_path, {"mod": "module"})
    assert autofit.check_capture_layers(single) == {}


def test_every_per_capture_entry_carries_its_layer(tmp_path, monkeypatch):
    ok = _fit_args(corpus=str(_write_corpus(tmp_path / "a", notes=(60, 72))), validate_notes="48")
    other = _fit_args(
        corpus=str(_write_corpus(tmp_path / "b", notes=(60, 72))), validate_notes="48"
    )
    layers = {"a": "wanted", "b": "off-layer"}
    part = autofit.partition_joint([ok, other], ["a", "b"], layers)
    assert [(p["capture"], p["layer"]) for p in part["per_capture"]] == [
        ("a", "wanted"),
        ("b", "off-layer"),
    ]
    _serve(monkeypatch, [_Held(0.8, held="48"), None])
    caps = _captures("a", "b")
    caps[0].layer, caps[1].layer = "wanted", "off-layer"
    result = validate_joint(caps, None, [KNOB], START, [0.2])
    assert [p["layer"] for p in result["per_capture"]] == ["wanted", "off-layer"]


# --------------------------------------------------------------------------- #
# a voice whose own capture is off-layer
# --------------------------------------------------------------------------- #
def _plan_world(tmp_path, monkeypatch, classes, *, own="own"):
    """A one-voice status table (program 0, slot aimed at the instrument) over @p classes.

    @p classes maps capture id to source class; every capture has a definition, a gate and a
    corpus, and all of them reach the voice's patch.
    """
    _layer_world(tmp_path, monkeypatch, classes)
    ref = tmp_path / "ref"
    ref.mkdir()
    for cap in classes:
        (ref / f"{cap}_gate.json").write_text(json.dumps({"timbre": "t0"}))
    status_file = tmp_path / "voice-status.json"
    status_file.write_text(
        json.dumps(
            {
                "voices": [
                    {
                        "slug": "p000",
                        "program": 0,
                        "bank": 0,
                        "kit": False,
                        "patch": "grand",
                        "capture": own,
                    }
                ]
            }
        )
    )
    monkeypatch.setattr(refit, "STATUS", status_file)
    monkeypatch.setattr(refit, "REFERENCE_DIR", ref)
    monkeypatch.setattr(
        refit.bank,
        "captures",
        lambda: [argparse.Namespace(id=c, source_class=s) for c, s in classes.items()],
    )
    monkeypatch.setattr(
        refit.boundary, "reference_boundary", lambda *a, **k: refit.boundary.BOUNDARY_INSTRUMENT
    )
    monkeypatch.setattr(
        refit,
        "capital_gates",
        lambda *a, **k: [
            {"capture": c, "gate": str(ref / f"{c}_gate.json"), "timbre": "t0", "patch": "grand"}
            for c in sorted(classes, key=lambda c: c != own)
        ],
    )
    monkeypatch.setattr(refit, "JOINT", False)
    monkeypatch.setattr(refit, "ALLOW_OFF_LAYER", False)


def test_an_off_layer_own_capture_is_compared_and_the_on_layer_sibling_is_fitted(
    tmp_path, monkeypatch
):
    _plan_world(tmp_path, monkeypatch, {"own": "module", "lib": "library"})
    todo, skipped = refit.plan("capitals")
    assert skipped == [] and len(todo) == 1
    voice = todo[0]
    assert [p["capture"] for p in voice["fit_on"]] == ["lib"]
    assert voice["joint_dropped"][0]["capture"] == "own"
    assert voice["joint_dropped"][0]["reason"].startswith("comparison (off-layer)")
    assert "provisional_off_layer" not in voice
    # The fit names the sibling's corpus and cuts; the own capture is still judged by the gates.
    seen = []
    monkeypatch.setattr(
        refit.subprocess,
        "run",
        lambda cmd, **k: (seen.append(cmd), (tmp_path / "fit.json").write_text("{}")),
    )
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    refit._fit(voice, tmp_path / "fit.json", "build", 12)
    cmd = seen[0]
    assert cmd[cmd.index("--corpus") + 1] == str(tmp_path / "scratch" / "capture" / "lib")
    assert cmd[cmd.index("--notes") + 1] == ",".join(map(str, voice["fit_on"][0]["notes"]))
    assert "--allow-off-layer" not in cmd
    assert [g["capture"] for g in voice["gates"]] == ["own", "lib"]


def test_joint_mode_fits_the_on_layer_siblings_together_and_compares_the_own(tmp_path, monkeypatch):
    _plan_world(tmp_path, monkeypatch, {"own": "module", "a": "library", "b": "dedicated"})
    monkeypatch.setattr(refit, "JOINT", True)
    voice = refit.plan("capitals")[0][0]
    assert (
        [p["capture"] for p in voice["joint"]]
        == ["a", "b"]
        == [p["capture"] for p in voice["fit_on"]]
    )
    assert voice["joint_dropped"][0]["capture"] == "own"


def test_a_voice_with_no_on_layer_capture_is_refused_and_runs_with_the_flag(tmp_path, monkeypatch):
    _plan_world(tmp_path, monkeypatch, {"own": "module", "also": "module"})
    todo, skipped = refit.plan("capitals")
    assert todo == []
    assert skipped == [
        {
            "slug": "p000",
            "reason": "no on-layer reference for this voice: wanted instrument, have module",
        }
    ]
    monkeypatch.setattr(refit, "ALLOW_OFF_LAYER", True)
    todo, skipped = refit.plan("capitals")
    assert skipped == []
    voice = todo[0]
    assert voice["provisional_off_layer"] is True and voice["provisional_oracle"] == ["own"]
    seen = []
    monkeypatch.setattr(
        refit.subprocess,
        "run",
        lambda cmd, **k: (seen.append(cmd), (tmp_path / "fit.json").write_text("{}")),
    )
    monkeypatch.setattr(refit, "_check_pin", lambda: None)
    refit._fit(voice, tmp_path / "fit.json", "build", 12)
    assert "--allow-off-layer" in seen[0]
    assert seen[0][seen[0].index("--corpus") + 1] == voice["corpus"]


def test_an_on_layer_capture_that_cannot_be_fitted_is_not_a_license_to_fit_off_layer(
    tmp_path, monkeypatch
):
    _plan_world(tmp_path, monkeypatch, {"own": "module", "lib": "library"})
    for f in (tmp_path / "scratch" / "capture" / "lib").iterdir():
        f.unlink()
    monkeypatch.setattr(refit, "ALLOW_OFF_LAYER", True)
    todo, skipped = refit.plan("capitals")
    assert todo == [] and "cannot be fitted" in skipped[0]["reason"]


def test_the_record_carries_the_provisional_flag_and_names_the_oracle():
    report_ = {
        "objective": "d",
        "knobs": [{"key": "grand.x"}],
        "provisional_off_layer": True,
        "provisional_oracle": ["own:t0"],
    }
    unit, entry = fits.record_fit(report_, capture="own", timbre="t0", outcome="adopted")
    assert unit == "grand"
    assert entry["provisional_off_layer"] is True and entry["provisional_oracle"] == ["own:t0"]
    plain = {"objective": "d", "knobs": [{"key": "grand.x"}]}
    assert (
        "provisional_off_layer"
        not in fits.record_fit(plain, capture="own", timbre="t0", outcome="adopted")[1]
    )


def test_the_fit_axis_shows_a_provisional_fit_as_its_own_state():
    entry = {
        "objective": "d",
        "date": "2026-01-01",
        "outcome": "adopted",
        "capture": "own",
        "timbre": "t0",
        "provisional_off_layer": True,
        "provisional_oracle": ["own:t0"],
        "base_overrides": "e.k=1",
    }
    got = fits.axis(["grand"], {"grand": entry}, "d", set())
    assert got["state"] == fits.OFF_LAYER
    assert got["note"] == "provisional: fitted against an off-layer oracle"
    assert got["oracle"] == ["own:t0"]
    # A fit under another objective is stale whatever it was fitted against.
    assert fits.axis(["grand"], {"grand": entry}, "new", set())["state"] == fits.STALE


def test_the_fit_axis_ranks_an_in_sample_fit_between_off_layer_and_under_base():
    entry = {
        "objective": "d",
        "date": "2026-01-01",
        "outcome": "adopted",
        "capture": "own",
        "timbre": "t0",
        "in_sample": True,
        "base_overrides": "e.k=1",
    }
    got = fits.axis(["kit"], {"kit": entry}, "d", set())
    assert got["state"] == fits.IN_SAMPLE
    assert got["note"] == "provisional: adopted on an in-sample gain, no held-out"
    off = {**entry, "provisional_off_layer": True, "provisional_oracle": ["own:t0"]}
    assert fits.axis(["kit"], {"kit": off}, "d", set())["state"] == fits.OFF_LAYER
    assert fits.axis(["kit"], {"kit": entry}, "new", set())["state"] == fits.STALE
    report = {"objective": "d", "knobs": [{"key": "kit.x"}], "in_sample": True}
    recorded = fits.record_fit(report, capture="own", timbre="t0", outcome="adopted")[1]
    assert recorded["in_sample"] is True


def test_the_report_of_a_fit_against_off_layer_captures_only_is_provisional(tmp_path, monkeypatch):
    _aim(monkeypatch)
    _serve_definitions(monkeypatch, {"mod": "module"})
    single = _hand_listed(tmp_path, {"mod": "module"})
    assert autofit.provisional_oracle(single) == ["mod:t"]
    assert autofit._provisional_record(single) == {
        "provisional_off_layer": True,
        "provisional_oracle": ["mod:t"],
    }
    _serve_definitions(monkeypatch, {"lib": "library", "mod": "module"})
    assert autofit.provisional_oracle(_hand_listed(tmp_path, {"lib": 0, "mod": 0})) == []


def test_a_capture_with_no_declared_class_is_named_at_the_start_of_an_autofit(
    tmp_path, monkeypatch, capsys
):
    classes = {"lib": "library", "bare": None}
    _aim(monkeypatch)
    _serve_definitions(monkeypatch, classes)
    args = _hand_listed(tmp_path, classes, allow_off_layer=True)
    autofit.check_capture_layers(args)
    assert "bare:t: no source_class declared: treated as off-layer" in capsys.readouterr().err
    # A slot aimed at nothing leaves nothing off-layer, so nothing is said.
    monkeypatch.setattr(policy, "load", lambda path=None: {})
    autofit.check_capture_layers(args)
    assert "no source_class declared" not in capsys.readouterr().err


def test_a_capture_with_no_declared_class_is_named_at_the_start_of_a_refit(tmp_path, monkeypatch):
    _plan_world(tmp_path, monkeypatch, {"own": "library", "bare": None, "ok": "library"})
    todo, _ = refit.plan("capitals")
    assert refit.undeclared_lines(todo) == ["bare: no source_class declared: treated as off-layer"]


def test_the_first_joint_point_caps_presence_over_every_capture(tmp_path, monkeypatch):
    def member(body_units):
        terms = {"harm": _units("harm", 1.0), "body": _units("body", body_units)}
        return _member(tmp_path, monkeypatch, lambda values: dict(terms), harm=1.0, body=0.25)

    quiet, loud = member(0.1), member(40.0)
    joint = _joint(quiet, loud)
    joint.args.presence = {
        "terms": {"body": {"dimensions": ["body"], "weight": 0.25}},
        "unscorable": [],
        "user_zero": {},
        "capped": False,
    }
    assert joint([KNOB.start_value]) == pytest.approx(1.0)
    capped = joint.args.presence["terms"]["body"]
    # The loud capture binds, and both captures score under its weight.
    assert capped["cap"] == "term" and capped["weight"] < 0.25
    for m in (quiet, loud):
        assert m.args.w_body == capped["weight"] and m.loss.weights["body"] == capped["weight"]
        assert m.loss.combine(m.baseline_terms) == pytest.approx(1.0)
