"""Loss normalisation, and which ceiling a fit's residual is scored under.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit_loss.py -q
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import json

import autofit
import autofit_resolve
import loss_cells
from autofit import (
    SUSTAIN_DRIFT_PENALTY_PER_DB_S,
    SUSTAIN_SLOPE_CAP_DB_S,
    Evaluator,
    reference_band_edge,
    sustain_excess_db_s,
)
from autofit_test_fixtures import (
    _probe_args,
    _terms,
    _write_corpus,
)
from corpus import load_corpus
from loss import (
    HARM_REACH,
    LOSS_TERMS,
    TERM_UNITS,
    LossWeights,
    band_min_note_s,
    cli_weights,
    dropped_weights,
    loss_terms,
)
from metrics import THIRD_OCTAVE_CENTERS, measure_band_edge
from patterns import sustain_pattern


# --------------------------------------------------------------------------- #
# Loss normalisation
# --------------------------------------------------------------------------- #
def test_the_start_point_scores_exactly_one():
    weights = LossWeights({"harm": 1.0, "cents": 0.5, "tnr": 1.0})
    start = _terms(harm=42.0, cents=0.3, tnr=0.0)
    weights.calibrate(start)
    assert weights.combine(start) == pytest.approx(1.0)


def test_a_term_that_starts_at_zero_costs_exactly_what_it_grows_to():
    """A noise penalty of exactly zero at the start must not divide by nothing.

    It used to divide by its own start value, which is why it needed a floor.
    Now it divides by a fixed perceptual unit, so the case is arithmetic rather
    than a special one: a dB of new noise is a dB of new noise.
    """
    weights = LossWeights({"harm": 1.0, "tnr": 1.0})
    start = _terms(harm=40.0, tnr=0.0)
    weights.calibrate(start)
    grown = weights.combine(_terms(harm=40.0, tnr=1.0)) - weights.combine(start)
    assert grown == pytest.approx(1.0 / TERM_UNITS["tnr"] / weights.reference)


def test_normalisation_makes_unequal_units_comparable():
    """Moving any term by one perceptual unit moves the loss by the same amount.

    What this has always guarded is that `--w-harm 1` and `--w-cents 1` mean the
    same amount of pull, rather than the term that happens to be numerically
    larger deciding the objective on its own. It guarded it by dividing each
    term by its value at the start point, which bought comparability at the
    start and nowhere else: one raw unit then pulled by `w / start`, so the
    dimension that began furthest out was the one the objective was flattest
    along. The comparability is now in the units themselves and holds
    everywhere, so the check is one unit rather than one half.
    """
    weights = LossWeights({"harm": 1.0, "cents": 1.0})
    start = _terms(harm=60.0, cents=8.0)
    weights.calibrate(start)
    base = weights.combine(start)
    by_harm = base - weights.combine(_terms(harm=60.0 - TERM_UNITS["harm"], cents=8.0))
    by_cents = base - weights.combine(_terms(harm=60.0, cents=8.0 - TERM_UNITS["cents"]))
    assert by_harm == pytest.approx(by_cents)
    # And the property the old normalisation could not have: halving the term
    # that is further out buys more than halving the one that is closer in.
    # Here that is `cents` at eight units against `harm` at five, which the raw
    # numbers say the other way round — the units are what decides.
    assert 8.0 / TERM_UNITS["cents"] > 60.0 / TERM_UNITS["harm"]
    halved_harm = weights.combine(_terms(harm=30.0, cents=8.0))
    halved_cents = weights.combine(_terms(harm=60.0, cents=4.0))
    assert halved_cents < halved_harm


def test_a_term_is_scaled_by_its_unit_and_never_by_its_start_value():
    """The start value must not decide how hard the objective pulls on a term.

    Two voices whose harmonic ladder is 6 dB out and 60 dB out have to feel the
    same pull per dB, or the objective is flattest exactly where the voice is
    worst — and the reported number means nothing across voices, since both
    start at 1.0 whatever they started from.
    """
    near, far = LossWeights({"harm": 1.0}), LossWeights({"harm": 1.0})
    near.calibrate(_terms(harm=6.0))
    far.calibrate(_terms(harm=60.0))
    assert near.scales["harm"] == far.scales["harm"] == TERM_UNITS["harm"]
    # One dB off the ladder is worth the same on both, before the display layer
    # each divides by.
    assert (
        near.combine(_terms(harm=5.0)) - near.combine(_terms(harm=6.0))
    ) * near.reference == pytest.approx(
        (far.combine(_terms(harm=59.0)) - far.combine(_terms(harm=60.0))) * far.reference
    )


def test_a_term_made_entirely_of_caps_is_reported_as_unreached():
    """A capped aggregate reads its WORST when nothing could be compared.

    Every empty-set guard here looks for a term that has fallen to zero, which
    is what an averaged term does when its points run out. A term whose cells
    are charged the cap instead does the opposite — it saturates — so no guard
    sees it, and under a fixed perceptual scale its size is the cap rather than
    a distance. Measured on the bank's rendered probes, two voices' `slope` came
    to 24 cells with not one comparison among them.
    """
    weights = LossWeights({"slope": 1.0, "harm": 1.0})
    start = _terms(harm=40.0, slope=36.0)
    start |= {"slope_cells": 24.0, "slope_capped": 24.0}
    weights.calibrate(start)
    unreached = weights.unreached(start)
    assert [e[0] for e in unreached] == ["slope"]
    assert unreached[0][2] == 24.0
    # And it is still charged: reporting is not excusing, because a cell nothing
    # could compare must not be dropped from the mean.
    assert weights.combine(start) == pytest.approx(1.0)
    assert weights.combine(_terms(harm=40.0, slope=0.0)) < 1.0


def test_a_term_whose_reference_offered_no_cells_is_reported_too():
    """`tail` on a two-second probe: 0.0, its best score, from nothing at all."""
    weights = LossWeights({"tail": 1.0, "harm": 1.0})
    start = _terms(harm=40.0, tail=0.0) | {"tail_cells": 0.0, "tail_capped": 0.0}
    weights.calibrate(start)
    assert [e[0] for e in weights.unreached(start)] == ["tail"]
    assert weights.unreached(start)[0][2] == 0.0


def test_a_term_with_comparisons_behind_it_is_not_reported():
    weights = LossWeights({"slope": 1.0})
    start = _terms(slope=12.0) | {"slope_cells": 24.0, "slope_capped": 23.0}
    weights.calibrate(start)
    assert weights.unreached(start) == []


def test_every_loss_term_has_a_perceptual_unit():
    """A term with no unit divides by nothing and takes the objective with it."""
    missing = [t for t in LOSS_TERMS if t not in TERM_UNITS]
    assert not missing, f"no perceptual unit for {missing}"
    assert all(TERM_UNITS[t] > 0.0 for t in LOSS_TERMS)


def test_the_harmonic_term_reaches_every_bin_the_ladder_measures():
    """A bin above `n_harm` is charged by nothing at all.

    `harm` stops at `n_harm` and `tnr`'s harmonic mask treats the bins above it
    as partials rather than as noise, so the difference between the two is a
    band of the spectrum no term prices in either direction.
    """
    ladder = [0.0, -6.0, -12.0, -18.0, -24.0, -30.0, -36.0, -42.0, -48.0, -54.0, -60.0, -66.0]
    assert HARM_REACH == len(ladder)

    def row(top: float) -> dict:
        out = list(ladder)
        out[-1] = out[-2] = top
        return {
            "harmonics_db": out,
            "f0_cents_err": 0.0,
            "tnr_db": 40.0,
            "f0_hz": 440.0,
            "note": 69,
            "velocity": 100,
            "sustain_slope_db_s": 0.0,
            "release_ms": 0.0,
            "attack_ms": 0.0,
        }

    quiet, loud = row(-66.0), row(-50.0)
    assert loss_terms([quiet], [loud], n_harm=10)["harm"] == pytest.approx(0.0)
    assert loss_terms([quiet], [loud])["harm"] > 0.0


def test_raw_weighting_is_left_alone_without_calibration():
    weights = LossWeights({"harm": 1.0, "cents": 1.0})
    assert weights.combine(_terms(harm=60.0, cents=8.0)) == pytest.approx(68.0)


def test_unscorable_renders_are_infinite():
    assert LossWeights({"harm": 1.0}).combine(None) == float("inf")


def test_loss_terms_rejects_a_row_count_mismatch():
    row = {
        "harmonics_db": [0.0],
        "f0_cents_err": 0.0,
        "tnr_db": 0.0,
        "sustain_slope_db_s": 0.0,
        "release_ms": 0.0,
        "attack_ms": 0.0,
    }
    assert loss_terms([row], [row, row], n_harm=1) is None


def test_a_pattern_with_no_analysis_notes_is_scorable_on_the_whole_timeline():
    """`scale` and `room-probe` have nothing per-note; the multi-scale term still does.

    No rows on either side is a pattern that carries no per-note evidence, not a
    render that failed — which is what a count mismatch is.
    """
    terms = loss_terms([], [], n_harm=1, mss=0.4)
    assert terms is not None
    assert terms["mss"] == pytest.approx(0.4)
    assert terms["harm"] == 0.0


def test_a_model_cleaner_than_its_oracle_is_not_penalised():
    def row(tnr):
        return {
            "harmonics_db": [0.0],
            "f0_cents_err": 0.0,
            "tnr_db": tnr,
            "sustain_slope_db_s": 0.0,
            "release_ms": 0.0,
            "attack_ms": 0.0,
        }

    cleaner = loss_terms([row(40.0)], [row(20.0)], n_harm=1)
    noisier = loss_terms([row(10.0)], [row(20.0)], n_harm=1)
    assert cleaner["tnr"] == 0.0
    assert noisier["tnr"] == pytest.approx(10.0)


def test_a_silent_render_does_not_win_the_harmonic_term():
    """Silencing a gain was the cheapest way to score a perfect harmonic ladder.

    A render with no signal reports h1 at 0 dB by definition and every partial
    above it at the -120 dB floor. The floor guard then skips all of them, h1
    matches h1 exactly, and the term sums to 0.0 — its best possible value. It
    is not a match, it is the absence of anything to match, and the candidate
    has to be unscorable rather than optimal.
    """
    silent = {
        "harmonics_db": [0.0] + [-120.0] * 11,
        "f0_cents_err": 0.0,
        "tnr_db": 0.0,
        "sustain_slope_db_s": 0.0,
        "release_ms": 0.0,
        "attack_ms": 0.0,
    }
    sounding = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
    }
    assert loss_terms([silent], [sounding], n_harm=12) is None
    assert loss_terms([sounding], [sounding], n_harm=12) is not None


def skeleton(bands, n=6):
    """A skeleton block whose three decay bands all carry `bands`."""
    return {
        "init_db": list(bands) + [None] * (12 - len(bands)),
        "early_db_s": list(bands),
        "late_db_s": list(bands),
        "tail_db_s": list(bands),
    }


def sounding_row(**over):
    row = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
        "skeleton": skeleton([-2.0] * 6),
        "held_rms_dbfs": -20.0,
        "held_crest_db": 12.0,
    }
    return row | over


def test_a_voice_that_stopped_sounding_does_not_win_the_decay_terms():
    """The same defect as the harmonic ladder's, one layer down.

    Comparing only the bands present on both sides means a model with no bands
    at all contributes nothing, and the decay terms average to 0.0 - their best
    value. Taking the amplitude envelope's sustain to zero reaches it: the note
    dies, every band goes unfitted at once, and three terms report a perfect
    match for a voice that makes no sound after its attack.
    """
    oracle = sounding_row()
    dead = sounding_row(skeleton=skeleton([None] * 6), held_crest_db=None)
    alive_but_wrong = sounding_row(skeleton=skeleton([-9.0] * 6))

    dead_terms = loss_terms([dead], [oracle], n_harm=12)
    wrong_terms = loss_terms([alive_but_wrong], [oracle], n_harm=12)
    for term in ("slope", "tail", "init"):
        assert dead_terms[term] > wrong_terms[term], term
    assert dead_terms["crest"] > 0.0


def test_a_band_the_reference_has_nothing_in_is_still_skipped():
    """The asymmetry is the point: an absent ORACLE value is a short probe.

    The aftersound band has no frames on a two-second probe, and charging the
    model for the reference's own silence would make every short probe score as
    a broken voice.
    """
    oracle = sounding_row(skeleton=skeleton([None] * 6))
    model = sounding_row(skeleton=skeleton([-2.0] * 6))
    terms = loss_terms([model], [oracle], n_harm=12)
    assert terms["slope"] == 0.0 and terms["tail"] == 0.0


def test_a_render_that_lost_only_its_top_partials_is_still_scored():
    """The guard is about having no partials at all, not about having few.

    A dark note is a defect the harmonic term exists to report, so rejecting it
    as unscorable would hide exactly what the fit is for.
    """
    dark = {
        "harmonics_db": [0.0, -8.0] + [-120.0] * 10,
        "f0_cents_err": 0.0,
        "tnr_db": 20.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
    }
    bright = {
        "harmonics_db": [0.0, -3.0, -6.0] + [-9.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
    }
    terms = loss_terms([dark], [bright], n_harm=12)
    assert terms is not None and terms["harm"] > 0.0
    # 5 dB on h2, weighted by how audible that partial is: these rows name
    # neither a pitch nor a measured f0, so only the masking half of the weight
    # applies and h2 at -3 dB under the loudest bin keeps 93 % of its vote.
    assert terms["harm"] == pytest.approx(5.0 * (1.0 - 3.0 / 45.0))
    # Unweighted it is exactly the raw difference, which is what
    # --flat-partial-weighting restores.
    flat = loss_terms([dark], [bright], n_harm=12, audibility=False)
    assert flat["harm"] == pytest.approx(5.0)


# --------------------------------------------------------------------------- #
# The resolved weight, not the flag
# --------------------------------------------------------------------------- #
def test_a_run_that_never_named_a_weight_still_knows_whether_it_needs_audio():
    """Every `--w-*` defaults to None so an unset weight stays distinguishable
    from an explicit zero, and the number it stands for comes from the
    instrument's class. Anything deciding on `args.w_mss` itself therefore
    compares None against a float, which is a TypeError rather than a weight -
    and the guard that renders twice to prove the overrides reach the library
    asks that question before the weights have been resolved, so every drum fit
    hit it.
    """
    base = {f"w_{t}": None for t in LOSS_TERMS}
    base.update(program=0, drum_note=42, percussive=True, raw_loss=False, workers=1)
    args = argparse.Namespace(**base)

    resolved = cli_weights(args)
    assert isinstance(resolved.get("mss", 0.0), float)

    ev = Evaluator([], {}, [], None, args, Path("build-none"))
    assert ev.want_audio is (resolved.get("mss", 0.0) > 0.0)


# --------------------------------------------------------------------------- #
# which ceiling a fit is scored under
# --------------------------------------------------------------------------- #
def _rows_discriminating_to(index: int, count: int = 12) -> list[dict]:
    """Profiles that tell their instruments apart up to `index` and not above it."""
    rng = np.random.default_rng(7)
    rows = []
    for _ in range(count):
        profile = rng.normal(-20.0, 9.0, len(THIRD_OCTAVE_CENTERS))
        # One shared value above the index: no scatter is what a band that has
        # stopped separating the kit looks like.
        profile[index + 1 :] = -30.0
        rows.append({"bands_db": [float(v) for v in profile]})
    return rows


def _committed_profile(tmp_path: Path, monkeypatch, ident: str, edge) -> None:
    """Stand `reference/<ident>.json` up with one measured ceiling in it."""
    reference = tmp_path / "reference"
    reference.mkdir(parents=True, exist_ok=True)
    (reference / f"{ident}.json").write_text(
        json.dumps({"id": ident, "capture": {"band_edge_hz": edge}, "rows": []})
    )
    monkeypatch.setattr(autofit_resolve, "HERE", tmp_path)


def test_the_rows_alone_measure_the_wider_of_the_two_ceilings():
    """The premise of the rest: a single oracle answers 8 kHz here."""
    assert measure_band_edge(_rows_discriminating_to(22)) == 8000.0


def test_a_fit_is_held_to_the_ceiling_the_gate_scores_against(tmp_path, monkeypatch, capsys):
    """One oracle can measure whether IT discriminates and nothing else. The
    capture's own profile is measured across every reference it has, so it also
    knows where they stop AGREEING — and that is the range the gate reads. Left
    on its own measurement the fit optimises bands the gate does not score."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    _committed_profile(tmp_path, monkeypatch, corpus.capture_id, 5000.0)
    assert reference_band_edge(corpus, _rows_discriminating_to(22)) == 5000.0
    assert "held to 5.0 kHz" in capsys.readouterr().err


def test_an_oracle_that_shows_no_ceiling_of_its_own_still_takes_the_measured_one(
    tmp_path,
    monkeypatch,
):
    """A reference discriminating all the way up is the case the announcement
    formats differently, and it is the one where the committed ceiling matters
    most: nothing about the oracle alone would have cut anything."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    _committed_profile(tmp_path, monkeypatch, corpus.capture_id, 5000.0)
    wide = _rows_discriminating_to(len(THIRD_OCTAVE_CENTERS) - 1)
    assert measure_band_edge(wide) is None
    assert reference_band_edge(corpus, wide) == 5000.0


def test_a_narrower_oracle_is_not_widened_by_the_committed_profile(
    tmp_path,
    monkeypatch,
    capsys,
):
    """The lower of the two wins in both directions. A corpus re-captured since
    the profile was measured can be the narrower one, and a ceiling that rises
    because a file on disk is older than the audio is this guard running
    backwards."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    _committed_profile(tmp_path, monkeypatch, corpus.capture_id, 5000.0)
    assert reference_band_edge(corpus, _rows_discriminating_to(19)) == 4000.0
    assert "held to" not in capsys.readouterr().err


def test_a_capture_with_no_measured_profile_keeps_its_own_ceiling(tmp_path, monkeypatch):
    """Nothing to read is not a reason to refuse to fit. It is a reason the fit
    is weaker, which is what the single-reference edge already says."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    monkeypatch.setattr(autofit_resolve, "HERE", tmp_path)
    assert reference_band_edge(corpus, _rows_discriminating_to(22)) == 8000.0


def test_a_run_with_no_corpus_reads_no_profile(tmp_path, monkeypatch):
    """An oracle from a plugin or a WAV has no capture id, so there is no
    profile it could be matched against without guessing one."""
    monkeypatch.setattr(autofit_resolve, "HERE", tmp_path)
    assert reference_band_edge(None, _rows_discriminating_to(22)) == 8000.0


def test_a_profile_that_recorded_no_ceiling_does_not_invent_one(tmp_path, monkeypatch):
    """`band_edge_hz` is null for every capture that carries its whole range,
    and null has to stay distinguishable from zero — which would cut every
    band."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    _committed_profile(tmp_path, monkeypatch, corpus.capture_id, None)
    assert reference_band_edge(corpus, _rows_discriminating_to(22)) == 8000.0


def _slope_row(note: int, velocity: int, slope: float) -> dict:
    return {"note": note, "velocity": velocity, "sustain_slope_db_s": slope}


def test_the_sustain_excess_is_the_worst_note_not_the_average():
    """One note falling away is a broken voice; the mean of the grid hides it."""
    model = [_slope_row(48, 100, 0.0), _slope_row(60, 100, -40.0), _slope_row(72, 100, 0.0)]
    oracle = [_slope_row(48, 100, -1.0), _slope_row(60, 100, -1.0), _slope_row(72, 100, -1.0)]
    worst, pairs = sustain_excess_db_s(model, oracle)
    assert pairs == 3
    assert worst == pytest.approx(-39.0)


def test_the_sustain_excess_pairs_on_the_note_not_the_position():
    """A probe whose rows came back reordered must not subtract register from register."""
    model = [_slope_row(72, 100, -2.0), _slope_row(48, 100, -2.0)]
    oracle = [_slope_row(48, 100, -2.0), _slope_row(72, 100, -20.0)]
    worst, pairs = sustain_excess_db_s(model, oracle)
    assert pairs == 2
    # Paired by position this would read -18.0 on one side and +18.0 on the
    # other; paired by note both notes hold exactly where their reference does.
    assert worst == pytest.approx(0.0)


def test_a_probe_with_nothing_comparable_reports_zero_pairs():
    """Zero is this quantity's best value, so the count is what tells them apart."""
    worst, pairs = sustain_excess_db_s([{"note": 60, "velocity": 100}], [_slope_row(60, 100, -1.0)])
    assert (worst, pairs) == (0.0, 0)
    assert sustain_excess_db_s([], [])[1] == 0


def test_a_note_that_reached_digital_zero_is_charged_rather_than_skipped():
    """The case the fence exists for, and the one it could not see.

    `analyze_note` fits the sustain slope through the dB clamp, so a note that
    died returns None instead of a number. Skipped, a render whose every note
    fell silent scored +1.00 against a reference falling 1 dB/s — better than a
    render holding exactly like it.
    """
    oracle = [_slope_row(60, 100, -1.0), _slope_row(64, 100, -1.0)]
    gone = [
        {"note": 60, "velocity": 100, "sustain_slope_db_s": None},
        {"note": 64, "velocity": 100, "sustain_slope_db_s": None},
    ]
    worst, pairs = sustain_excess_db_s(gone, oracle)
    assert (worst, pairs) == (-SUSTAIN_SLOPE_CAP_DB_S, 2)
    # And it must stay worse than any voice that is actually sounding.
    held = [_slope_row(60, 100, -1.0), _slope_row(64, 100, -1.0)]
    assert worst < sustain_excess_db_s(held, oracle)[0]
    # A row that never carried the measurement at all is the probe's shape, not
    # a dead note, and is still skipped.
    assert sustain_excess_db_s([{"note": 60, "velocity": 100}], oracle) == (0.0, 0)


def test_the_sustain_excess_is_not_clamped_on_the_holding_side():
    """The fence subtracts two readings, so a clamp on one would become a charge."""
    model = [_slope_row(60, 100, -1.0)]
    oracle = [_slope_row(60, 100, -5.0)]
    assert sustain_excess_db_s(model, oracle)[0] == pytest.approx(4.0)


def _fence(start: float | None, limit: float = 3.0, unit: float = 1.0):
    ev = Evaluator.__new__(Evaluator)
    ev.args = argparse.Namespace(max_sustain_drift_db_s=limit)
    ev.start_sustain_excess_db_s = start
    ev.fence_unit = unit
    return ev


def test_the_sustain_fence_charges_nothing_at_the_point_it_is_anchored_on():
    """Anchored on the start, so the start itself is always inside it.

    Including a voice that already falls away: repairing a mechanism that
    cannot oscillate is not something a knob can be asked for, and a fence
    charging the start would spend the whole fit's budget asking.
    """
    for start in (0.0, -92.36):
        assert _fence(start)._sustain_drift_penalty({"sustain_excess_db_s": start}) == 0.0


def test_the_sustain_fence_charges_a_candidate_that_falls_further():
    ev = _fence(-2.0, limit=3.0)
    # Inside the allowance, exactly as before.
    assert ev._sustain_drift_penalty({"sustain_excess_db_s": -5.0}) == 0.0
    # One dB/s past it, at the documented rate.
    assert ev._sustain_drift_penalty({"sustain_excess_db_s": -6.0}) == pytest.approx(
        autofit.SUSTAIN_DRIFT_PENALTY_PER_DB_S
    )
    # And it is one-sided: holding better than the start is never charged.
    assert ev._sustain_drift_penalty({"sustain_excess_db_s": +40.0}) == 0.0


def test_the_sustain_fence_is_off_when_it_has_no_anchor_or_no_reading():
    """Both halves come from the same render, so neither arrives alone by design.

    Asserted anyway, because the alternative to returning zero here is charging
    a candidate the difference between a measurement and a missing value.
    """
    assert _fence(None)._sustain_drift_penalty({"sustain_excess_db_s": -50.0}) == 0.0
    assert _fence(-2.0)._sustain_drift_penalty({}) == 0.0
    assert _fence(-2.0)._sustain_drift_penalty(None) == 0.0
    assert _fence(-2.0, limit=0.0)._sustain_drift_penalty({"sustain_excess_db_s": -50.0}) == 0.0


def test_the_sustain_fence_is_charged_in_the_units_the_run_reports():
    """The rate is per unit of a loss the start scores 1.0, so it has to scale.

    A `--raw-loss` run scores its start as a weighted sum of raw terms, two
    orders of magnitude up, and an unscaled fence there is a rounding error on
    the one path that also has no normalisation to catch the trade.
    """
    one = _fence(-2.0, unit=1.0)._sustain_drift_penalty({"sustain_excess_db_s": -13.0})
    raw = _fence(-2.0, unit=112.0)._sustain_drift_penalty({"sustain_excess_db_s": -13.0})
    assert one == pytest.approx(SUSTAIN_DRIFT_PENALTY_PER_DB_S * 8.0)
    assert raw == pytest.approx(one * 112.0)


def _anchoring_evaluator(normalize: bool, start_terms: dict):
    ev = Evaluator.__new__(Evaluator)
    ev.normalize = normalize
    ev.quiet = True
    ev._offset_reported = True
    ev._anchored = False
    ev.fence_unit = 1.0
    ev.start_level_offset_db = None
    ev.start_sustain_excess_db_s = None
    ev.baseline_terms = None

    class _Loss:
        scales = None

        def calibrate(self, terms):
            self.scales = {}

        def combine(self, terms):
            return 112.0 if not normalize else 1.0

    ev.loss = _Loss()
    ev._calibrate_once(start_terms)
    return ev


def test_both_fences_anchor_whether_or_not_the_terms_are_normalised():
    """A guard that holds on the default path and lets go on the other one.

    Anchoring inside the normalisation branch left `--raw-loss` with both
    anchors at None, which turns every fence off — and off silently, since a
    fence that never charges prints exactly what a fit that never needed one
    does.
    """
    terms = {"level_offset_db": -41.6, "sustain_excess_db_s": -88.44}
    for normalize in (True, False):
        ev = _anchoring_evaluator(normalize, terms)
        assert ev.start_level_offset_db == pytest.approx(-41.6)
        assert ev.start_sustain_excess_db_s == pytest.approx(-88.44)
    assert _anchoring_evaluator(True, terms).fence_unit == pytest.approx(1.0)
    assert _anchoring_evaluator(False, terms).fence_unit == pytest.approx(112.0)


def test_a_class_default_the_probe_cannot_fit_is_dropped_and_named():
    """`dyn` on a `sustain` probe: no curve to fit, and 0.0 is its best score.

    Carried at its class weight it would report a perfect dynamics match on
    every candidate and dilute the objective by its share of it. Dropped
    silently is how a weight the instrument asked for goes missing with nothing
    to say so, which is what `dropped_weights` exists to print.
    """
    args = _probe_args(program=70, percussive=False, has_velocity_spread=False)
    # Could this have gone red: the class does ask for it on a probe that can.
    assert (
        cli_weights(_probe_args(program=70, percussive=False, has_velocity_spread=True))["dyn"]
        > 0.0
    )
    assert "dyn" not in cli_weights(args)
    assert [t for t, _ in dropped_weights(args)] == ["dyn"]
    # Named on the command line it is refused by the caller instead, so the two
    # reports never say the same thing twice.
    args.w_dyn = 1.0
    assert dropped_weights(args) == []


def test_a_probe_with_a_velocity_axis_keeps_the_dynamics_default():
    args = _probe_args(program=70, percussive=False, has_velocity_spread=True)
    assert cli_weights(args)["dyn"] > 0.0
    assert dropped_weights(args) == []


def test_the_aftersound_band_is_dropped_when_no_note_reaches_it():
    """`tail` reads a band opening at 2.0 s; the default probe holds 2.0 s.

    A band with no frames in it is skipped on both sides, which scores exactly
    0.0 — the term's best value — so every candidate of every fit that weighted
    it reported a perfect aftersound. Measured across the bank's rendered
    probes, `tail` came to 546 cells of which 546 were skipped.
    """
    args = _probe_args(program=0, percussive=False, has_tail_window=False)
    assert cli_weights(_probe_args(program=0, percussive=False, has_tail_window=True))["tail"] > 0.0
    assert "tail" not in cli_weights(args)
    assert [t for t, _ in dropped_weights(args)] == ["tail"]


def test_the_shortest_note_that_reaches_a_band_is_derived_from_the_grid():
    """Hand-copied, this number describes a window the code no longer uses."""
    assert band_min_note_s("tail_db_s") == pytest.approx(2.07)
    assert band_min_note_s("early_db_s") < band_min_note_s("late_db_s")
    # And the default probe falls under it, which is the whole finding.
    assert sustain_pattern(0).analysis_notes[0].dur < band_min_note_s("tail_db_s")


def test_the_cell_census_reads_what_the_loss_reported():
    """The instrument must not re-derive the loop it is auditing.

    A census computed from its own copy of the aggregation would agree with the
    loss right up until one of them changed, and the one that drifted would be
    the one nobody was reading.
    """
    terms = {
        "slope_cells": 24.0,
        "slope_capped": 24.0,
        "slope_absent": 13.0,
        "slope_skipped": 0.0,
        "tail_cells": 0.0,
        "tail_capped": 0.0,
        "tail_absent": 0.0,
        "tail_skipped": 18.0,
    }
    got = loss_cells.census(terms)
    assert dict(got["slope"]) == {"compared": 0, "clipped": 11, "absent": 13, "skipped": 0}
    assert dict(got["tail"]) == {"compared": 0, "clipped": 0, "absent": 0, "skipped": 18}
    # A term that reports no cells at all is absent from the census rather than
    # present with four zeros, which would read as a term that was looked at.
    assert "harm" not in got


def test_the_four_cell_outcomes_are_what_the_loss_actually_emits():
    """A round trip through the real reducer, so the key names cannot drift."""
    ladder = [0.0, -6.0, -12.0, -18.0, -24.0, -30.0, -36.0, -42.0, -48.0, -54.0, -60.0, -66.0]
    base = {
        "harmonics_db": ladder,
        "f0_cents_err": 0.0,
        "tnr_db": 40.0,
        "f0_hz": 440.0,
        "note": 69,
        "velocity": 100,
        "sustain_slope_db_s": 0.0,
        "release_ms": 0.0,
        "attack_ms": 0.0,
    }
    skeleton = {
        "init_db": [0.0] * 12,
        "early_db_s": [-1.0] * 12,
        "late_db_s": [-1.0] * 12,
        "tail_db_s": [None] * 12,
    }
    model = {**base, "skeleton": skeleton}
    # The oracle's aftersound band is empty, exactly as a two-second probe
    # leaves it; its early and late bands are 100 dB/s away, past the cap.
    oracle = {
        **base,
        "skeleton": {**skeleton, "early_db_s": [-101.0] * 12, "late_db_s": [-101.0] * 12},
    }
    terms = loss_terms([model], [oracle])
    assert terms is not None
    census = loss_cells.census(terms)
    assert census["slope"]["clipped"] == 12 and census["slope"]["compared"] == 0
    assert census["tail"]["skipped"] == 6 and census["tail"]["compared"] == 0
    assert terms["tail"] == 0.0, "a band with no cells scores the term's best"
