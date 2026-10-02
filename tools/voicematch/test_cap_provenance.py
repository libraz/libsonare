"""Cap provenance survives the production score, quality and diagnosis paths."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]

from diagnose import MEASUREMENT_LIMITED, diagnose
from fit_quality import quality_report
from loss import LossWeights, score_terms
from metrics import THIRD_OCTAVE_CENTERS
from test_reference_cells import _drum_row, _pitched_row


def _diagnose(terms, objective, *, percussive=False):
    probes = [("probe", "lo", terms, "clamp"), ("probe", "hi", terms, "clamp")]
    return diagnose(
        terms,
        probes,
        objective.weights,
        objective=objective,
        percussive=percussive,
    )


def test_pitched_score_terms_reports_caps_and_keeps_self_measurement_valid():
    oracle = [
        _pitched_row(note=60, velocity=80),
        _pitched_row(note=72, velocity=80),
    ]
    same = score_terms(oracle, oracle)
    assert same is not None
    for key in ("level_capped", "crest_capped", "dyn_capped", "hfdyn_capped", "stiff_capped"):
        assert same[key] == 0.0

    objective = LossWeights({"level": 1.0})
    objective.anchor(same)
    report = quality_report(same, objective, baseline_terms=same)
    assert report["terms"]["level"]["coverage_complete"]
    assert report["met"]
    assert next(
        term for term in _diagnose(same, objective).terms if term.term == "level"
    ).verdict == ("matched")


def test_pitched_score_terms_cap_becomes_unknown_in_quality_and_diagnosis():
    oracle = [
        _pitched_row(note=60, velocity=80),
        _pitched_row(note=72, velocity=80),
    ]
    model = [
        _pitched_row(note=60, velocity=80, held_rms_dbfs=None, held_crest_db=None),
        _pitched_row(note=72, velocity=80),
    ]
    baseline = score_terms(oracle, oracle)
    capped = score_terms(model, oracle)
    assert baseline is not None and capped is not None
    assert capped["level_capped"] == 1.0
    assert capped["crest_capped"] == 1.0

    objective = LossWeights({"level": 1.0})
    objective.anchor(baseline)
    report = quality_report(capped, objective, baseline_terms=baseline)
    assert report["terms"]["level"]["coverage_complete"] is False
    assert report["terms"]["level"]["target_met"] is False
    assert report["full_known_coverage"] is False
    assert report["met"] is False
    diagnosed = _diagnose(capped, objective)
    assert (
        next(term for term in diagnosed.terms if term.term == "level").verdict
        == MEASUREMENT_LIMITED
    )

    crest_objective = LossWeights({"crest": 1.0})
    crest_objective.anchor(baseline)
    crest_report = quality_report(capped, crest_objective, baseline_terms=baseline)
    assert crest_report["terms"]["crest"]["coverage_complete"] is False
    assert crest_report["terms"]["crest"]["target_met"] is False
    assert crest_report["full_known_coverage"] is False
    assert (
        next(
            term for term in _diagnose(capped, crest_objective).terms if term.term == "crest"
        ).verdict
        == MEASUREMENT_LIMITED
    )


def test_dynamics_producers_prefix_their_cap_counts():
    reference = []
    model = []
    for velocity in (20, 120):
        harmonics = [0.0, -6.0, -12.0] + [0.0] * 9
        attack = [0.0] * 30
        reference.append(
            _pitched_row(
                note=60,
                velocity=velocity,
                harmonics_db=list(harmonics),
                attack_hf_db=list(attack),
            )
        )
        if velocity == 120:
            harmonics[3:10] = [60.0] * 7
            attack = [60.0 if i % 5 == 0 else 0.0 for i in range(30)]
        model.append(
            _pitched_row(
                note=60,
                velocity=velocity,
                harmonics_db=harmonics,
                attack_hf_db=attack,
            )
        )

    same = score_terms(reference, reference)
    terms = score_terms(model, reference)
    assert same is not None and terms is not None
    assert terms["dyn_groups"] == terms["hfdyn_groups"] == 1.0
    assert terms["dyn_capped"] == 1.0
    assert terms["hfdyn_capped"] == 1.0

    for name in ("dyn", "hfdyn"):
        objective = LossWeights({name: 1.0})
        objective.anchor(same)
        self_report = quality_report(same, objective, baseline_terms=same)
        assert self_report["terms"][name]["coverage_complete"]
        assert self_report["terms"][name]["target_met"]
        assert self_report["met"]
        assert next(
            term for term in _diagnose(same, objective).terms if term.term == name
        ).verdict == ("matched")

        capped_report = quality_report(terms, objective, baseline_terms=same)
        assert capped_report["terms"][name]["coverage_complete"] is False
        assert capped_report["terms"][name]["target_met"] is False
        assert capped_report["full_known_coverage"] is False
        assert capped_report["met"] is False
        assert (
            next(term for term in _diagnose(terms, objective).terms if term.term == name).verdict
            == MEASUREMENT_LIMITED
        )


def _drum_row_with_bands(bands):
    return _drum_row(bands_db=list(bands))


def test_percussion_score_terms_reports_lf_and_kit_caps():
    reference = _drum_row_with_bands([0.0] * len(THIRD_OCTAVE_CENTERS))
    low_hot = _drum_row_with_bands(
        [30.0 if center <= 160.0 else 0.0 for center in THIRD_OCTAVE_CENTERS]
    )
    same = score_terms([reference], [reference], percussive=True)
    capped_lf = score_terms([low_hot], [reference], percussive=True)
    assert same is not None and capped_lf is not None
    assert same["lf_capped"] == 0.0
    assert capped_lf["lf_capped"] == 1.0

    velocities = (20, 120)
    dyn_reference = [
        _drum_row_with_bands([0.0] * len(THIRD_OCTAVE_CENTERS)) | {"note": 38, "velocity": velocity}
        for velocity in velocities
    ]
    dyn_model = [
        _drum_row_with_bands(
            [0.0] * (len(THIRD_OCTAVE_CENTERS) * 2 // 3)
            + [60.0] * (len(THIRD_OCTAVE_CENTERS) - len(THIRD_OCTAVE_CENTERS) * 2 // 3)
            if velocity == 120
            else [0.0] * len(THIRD_OCTAVE_CENTERS)
        )
        | {"note": 38, "velocity": velocity}
        for velocity in velocities
    ]
    dyn_same = score_terms(dyn_reference, dyn_reference, percussive=True)
    assert all(len(row["bands_db"]) == len(THIRD_OCTAVE_CENTERS) for row in dyn_model)
    dyn_capped = score_terms(dyn_model, dyn_reference, percussive=True)
    assert dyn_same is not None and dyn_capped is not None
    assert dyn_same["dyn_capped"] == 0.0
    assert dyn_capped["dyn_groups"] == 1.0
    assert dyn_capped["dyn_capped"] == 1.0

    oracle = []
    model = []
    for note, frequency in ((60, 100.0), (62, 400.0)):
        common = {
            "note": note,
            "velocity": 100,
            "tone_f0_hz": frequency,
            "decay_ms": 100.0 if note == 60 else 200.0,
            "centroid_hz": 1000.0 if note == 60 else 4000.0,
            "peak_dbfs": -20.0 if note == 60 else -10.0,
        }
        oracle.append(_drum_row_with_bands([0.0] * len(THIRD_OCTAVE_CENTERS)) | common)
        model_frequency = 100.0 if note == 60 else 100.0 * 2.0**8
        model.append(
            _drum_row_with_bands([0.0] * len(THIRD_OCTAVE_CENTERS))
            | (common | {"tone_f0_hz": model_frequency})
        )
    groups = {"family": [60, 62]}
    kit_same = score_terms(oracle, oracle, percussive=True, groups=groups)
    kit_capped = score_terms(model, oracle, percussive=True, groups=groups)
    assert kit_same is not None and kit_capped is not None
    assert kit_same["kit_capped"] == 0.0
    assert kit_capped["kit_capped"] > 0.0

    objective = LossWeights({"lf": 1.0})
    objective.anchor(same)
    report = quality_report(capped_lf, objective, baseline_terms=same)
    assert report["terms"]["lf"]["coverage_complete"] is False
    assert next(
        term for term in _diagnose(capped_lf, objective, percussive=True).terms if term.term == "lf"
    ).verdict == (MEASUREMENT_LIMITED)

    kit_objective = LossWeights({"kit": 1.0})
    kit_objective.anchor(kit_same)
    kit_report = quality_report(kit_capped, kit_objective, baseline_terms=kit_same)
    assert kit_report["terms"]["kit"]["coverage_complete"] is False
    diagnosed = _diagnose(kit_capped, kit_objective, percussive=True)
    assert (
        next(term for term in diagnosed.terms if term.term == "kit").verdict == MEASUREMENT_LIMITED
    )


def test_capped_or_missing_sustain_slope_is_measurement_limited():
    oracle = [_pitched_row(sustain_slope_db_s=-3.0)]
    baseline = score_terms(oracle, oracle)
    objective = LossWeights({"env": 1.0})
    objective.anchor(baseline)
    assert baseline["env_capped"] == 0.0
    assert quality_report(baseline, objective)["met"]
    for slope in (-33.0, -60.0, float("nan")):
        terms = score_terms([_pitched_row(sustain_slope_db_s=slope)], oracle)
        assert terms["env"] == 30.0
        assert terms["env_capped"] == 1.0
        assert not quality_report(terms, objective)["terms"]["env"]["coverage_complete"]
        diag = _diagnose(terms, objective)
        assert next(t for t in diag.terms if t.term == "env").verdict == MEASUREMENT_LIMITED
        assert not diag.structural()
