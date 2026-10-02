"""One condition cannot establish level balance after removing common gain."""

import sys
from pathlib import Path

import pytest

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]

from autofit_resolve import resolve_probe
from autofit_test_fixtures import _probe_args
from fit_quality import quality_report
from loss_dimensions import _level_terms
from loss_weights import LossWeights, cli_weights, unmeasurable_terms


def test_one_condition_level_is_a_tautology_and_is_not_a_fit_axis():
    reference = [{"held_rms_dbfs": -30.0}]
    for level in (-90.0, -30.0, -3.0):
        assert _level_terms([{"held_rms_dbfs": level}], reference)[0] == 0.0
    args = _probe_args(program=40, pattern="sustain", notes="67")
    resolve_probe(args)
    assert "level" not in cli_weights(args)
    assert "level" in dict(unmeasurable_terms(args))


def test_explicit_level_requires_a_contrast_after_removing_gain():
    with pytest.raises(ValueError, match="level.*contrast"):
        resolve_probe(_probe_args(program=40, pattern="sustain", notes="67", w_level=1.0))


def test_level_weight_loaded_after_probe_resolution_still_requires_contrast():
    args = _probe_args(program=40, pattern="sustain", notes="67")
    resolve_probe(args)
    args.w_level = 1.0  # Spec weights are applied after resolving the probe.
    with pytest.raises(ValueError, match="level.*contrast"):
        cli_weights(args)


@pytest.mark.parametrize("pattern,notes", [("sustain", "55,67"), ("velocity", "67")])
def test_register_or_velocity_contrast_keeps_level_balance(pattern, notes):
    args = _probe_args(program=40, pattern=pattern, notes=notes)
    resolve_probe(args)
    assert cli_weights(args)["level"] > 0.0


@pytest.mark.parametrize("missing", [None, float("nan")])
def test_one_usable_reference_level_does_not_establish_a_measured_contrast(missing):
    oracle = [
        {"note": 60, "velocity": 80, "held_rms_dbfs": -30.0},
        {"note": 72, "velocity": 80, "held_rms_dbfs": missing},
    ]
    counts = {}
    level, _, _ = _level_terms(oracle, oracle, counts=counts)
    terms = {"level": level, "comparable": 1.0, **counts}
    objective = LossWeights({"level": 1.0})
    objective.anchor(terms)
    report = quality_report(terms, objective)
    assert level == 0.0
    assert report["terms"]["level"]["residual_units"] is None
    assert report["met"] is False


def test_repeated_same_condition_is_not_a_level_contrast():
    oracle = [{"note": 60, "velocity": 80, "held_rms_dbfs": -30.0}] * 2
    counts = {}
    level, _, _ = _level_terms(oracle, oracle, counts=counts)
    terms = {"level": level, "comparable": 1.0, **counts}
    objective = LossWeights({"level": 1.0})
    objective.anchor(terms)
    assert quality_report(terms, objective)["met"] is False


def test_even_grid_uses_symmetric_common_gain():
    oracle = [{"held_rms_dbfs": -30.0}, {"held_rms_dbfs": -30.0}]
    model = [{"held_rms_dbfs": -30.0}, {"held_rms_dbfs": -24.0}]
    balance, _, gain = _level_terms(model, oracle)
    assert gain == 3.0
    assert balance == 3.0
    assert _level_terms(model[::-1], oracle) == (balance, 0.0, gain)
    shifted = [{"held_rms_dbfs": row["held_rms_dbfs"] + 9.0} for row in model]
    assert _level_terms(shifted, oracle)[0] == balance


def test_capped_string_stretch_cannot_be_a_structural_diagnosis():
    from diagnose import diagnose
    from loss import score_terms
    from metrics import MIN_PARTIALS_FOR_B

    reference = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
        "inharmonicity_b": 0.0,
        "inharmonicity_partials": MIN_PARTIALS_FOR_B,
    }
    oracle = [reference]
    model = [reference | {"inharmonicity_b": 0.01}]
    terms = score_terms(model, oracle)
    assert terms["stiff"] == 50.0
    assert terms["stiff_capped"] == 1.0
    objective = LossWeights({"stiff": 1.0})
    objective.anchor(terms)
    assert not quality_report(terms, objective)["terms"]["stiff"]["coverage_complete"]
    diag = diagnose(
        terms,
        [("decay", "lo", terms, "spec"), ("decay", "hi", terms, "spec")],
        objective.weights,
        objective=objective,
    )
    assert next(t for t in diag.terms if t.term == "stiff").verdict == "measurement-limited"
    assert not diag.structural()
