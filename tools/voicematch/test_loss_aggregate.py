"""A good average must not hide a wrong register or velocity."""

from __future__ import annotations

import copy
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fit_quality import quality_report
from loss import score_terms
from loss_aggregate import robust_score_terms
from loss_weights import LossWeights


def _pitched_rows(count=10):
    return [
        {
            "note": 48 + i,
            "velocity": 80,
            "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
            "f0_cents_err": 0.0,
            "tnr_db": 30.0,
            "sustain_slope_db_s": -3.0,
            "release_ms": 80.0,
            "attack_ms": 5.0,
            "held_rms_dbfs": -20.0,
            "held_crest_db": 12.0,
        }
        for i in range(count)
    ]


def test_one_bad_register_remains_visible_in_the_objective_and_report():
    oracle = _pitched_rows()
    model = copy.deepcopy(oracle)
    model[-1]["f0_cents_err"] = 30.0
    ordinary = score_terms(model, oracle)
    robust = robust_score_terms(model, oracle)
    assert ordinary["cents"] == pytest.approx(3.0)
    assert robust["cents"] == pytest.approx(10.0)
    assert robust["cents_mean"] == pytest.approx(3.0)
    assert robust["cents_worst"] == pytest.approx(30.0)
    assert robust["cents_worst_note"] == 57


def test_velocity_level_outlier_uses_the_whole_grid_gain_offset():
    oracle = _pitched_rows()
    model = copy.deepcopy(oracle)
    for row in model:
        row["held_rms_dbfs"] += 7.0
    model[-1]["held_rms_dbfs"] += 12.0
    robust = robust_score_terms(model, oracle)
    assert robust["level"] == pytest.approx(4.0)
    assert robust["level_mean"] == pytest.approx(1.2)
    assert robust["level_worst"] == pytest.approx(12.0)
    assert robust["level_offset_db"] == pytest.approx(7.0)


def test_a_wrong_kit_piece_cannot_hide_among_nine_matching_hits():
    oracle = [
        {
            "note": 36 + i,
            "velocity": 96,
            "bands_db": [0.0, -6.0, -12.0],
            "band_decay_db_s": [-3.0, -6.0],
            "attack_ms": 2.0,
            "decay_ms": 250.0,
            "crest_db": 12.0,
            "modal_density": [4.0] * 7,
            "modal_density_valid": [True] * 7,
        }
        for i in range(10)
    ]
    model = copy.deepcopy(oracle)
    model[-1]["modal_density"] = [1.0] * 7
    robust = robust_score_terms(model, oracle, percussive=True)
    assert robust["density_mean"] == pytest.approx(2.1)
    assert robust["density"] == pytest.approx(7.0)
    assert robust["density_worst"] == pytest.approx(21.0)
    assert robust["density_worst_note"] == 45


def test_a_uniform_grid_keeps_its_original_term_values():
    oracle = _pitched_rows(3)
    model = copy.deepcopy(oracle)
    for row in model:
        row["f0_cents_err"] = 4.0
    robust = robust_score_terms(model, oracle)
    ordinary = score_terms(model, oracle)
    for term in ("harm", "cents", "env", "level", "crest"):
        assert robust[term] == pytest.approx(ordinary[term])


def test_legacy_mean_mode_retains_the_original_objective():
    oracle = _pitched_rows()
    model = copy.deepcopy(oracle)
    model[-1]["f0_cents_err"] = 30.0
    assert robust_score_terms(model, oracle, tail_fraction=0.0) == score_terms(model, oracle)


def test_a_large_single_note_error_cannot_be_reported_as_a_met_target():
    oracle = _pitched_rows(40)
    model = copy.deepcopy(oracle)
    model[-1]["f0_cents_err"] = 10.0
    terms = robust_score_terms(model, oracle)
    weights = LossWeights({"cents": 1.0})
    weights.calibrate(robust_score_terms(oracle, oracle))
    report = quality_report(terms, weights)
    assert report["terms"]["cents"]["residual_units"] <= 1.0
    assert report["terms"]["cents"]["target_met"] is False
    assert report["terms"]["cents"]["worst_condition"]["note"] == 87


@pytest.mark.parametrize("fraction", [-0.1, 1.1, float("nan")])
def test_invalid_tail_fraction_is_refused(fraction):
    rows = _pitched_rows()
    with pytest.raises(ValueError, match="tail_fraction"):
        robust_score_terms(rows, rows, tail_fraction=fraction)


def test_one_silent_row_cannot_make_the_whole_grid_unscorable():
    oracle = _pitched_rows()
    for row in oracle:
        row["modal_hz"] = [110.0, 220.0, 330.0]
    model = copy.deepcopy(oracle)
    model[-1]["modal_hz"] = []
    model[-1]["f0_cents_err"] = 30.0
    ordinary = score_terms(model, oracle)
    assert ordinary is not None
    assert score_terms([model[-1]], [oracle[-1]]) is None
    robust = robust_score_terms(model, oracle)
    assert robust is not None
    assert robust["cents_mean"] == pytest.approx(ordinary["cents"])
    assert robust["cents"] >= ordinary["cents"]


def _velocity_rows(pitches=20):
    from loss_dimensions import ATTACK_BANDS_HZ

    rows = []
    for i in range(pitches):
        for velocity in (40, 110):
            row = _pitched_rows(1)[0]
            row.update(
                note=48 + i,
                velocity=velocity,
                peak_dbfs=-20.0 + (velocity - 40) / 10.0,
                attack_hf_db=[-30.0 + (velocity - 40) / 5.0] * len(ATTACK_BANDS_HZ) * 6,
            )
            rows.append(row)
    return rows


def test_two_outlier_notes_in_twenty_move_a_presence_tail_term():
    from loss_aggregate import PRESENCE_TAIL_FRACTION

    oracle = _velocity_rows()
    model = copy.deepcopy(oracle)
    for row in model:
        if row["note"] in (50, 60):
            row["held_crest_db"] += 6.0
            if row["velocity"] == 110:
                row["attack_hf_db"] = [v + 8.0 for v in row["attack_hf_db"]]
                row["peak_dbfs"] += 4.0
    tails = {t: PRESENCE_TAIL_FRACTION for t in ("crest", "hfdyn", "velspan")}
    mean = robust_score_terms(model, oracle, tail_fraction=0.0)
    tail = robust_score_terms(model, oracle, tail_fraction=0.0, term_tails=tails)
    # Two of twenty pitches: a tenth of the mean's reading, the whole of the tail's.
    assert mean["crest"] == pytest.approx(tail["crest_mean"]) == pytest.approx(0.6)
    assert tail["crest"] == pytest.approx(6.0) and tail["crest_tail_fraction"] == 0.1
    assert tail["velspan"] == pytest.approx(10.0 * mean["velspan"]) == pytest.approx(4.0)
    assert tail["hfdyn"] == pytest.approx(10.0 * mean["hfdyn"])
    assert tail["hfdyn_groups"] == 20 and tail["hfdyn_worst_note"] in (50, 60)
    # A term no tail names keeps its reduction.
    assert tail["cents"] == mean["cents"] and "dyn_upper_tail" not in tail


def test_a_presence_tail_is_taken_only_where_the_gate_bounds_a_p90():
    from loss_aggregate import own_term_aggregates

    terms = {
        "body": {"statistics": ["abs_median", "median", "p90"]},
        "hfdyn": {"statistics": ["median", "p90"]},
        "velspan": {"statistics": ["abs_median"]},
        "modedyn": {"statistics": ["p90"]},
    }
    own = own_term_aggregates(terms, 0.25)
    assert own == {"body": 0.1, "hfdyn": 0.1}
    assert terms["body"]["covers"] == ["abs_median", "median", "p90"]
    assert terms["hfdyn"]["aggregate"] == "tail" and terms["hfdyn"]["covers"] == ["median", "p90"]
    assert terms["velspan"]["aggregate"] == "mean" and terms["velspan"]["covers"] == ["abs_median"]
    # A grid term with no per-pitch reduction cannot take the tail, and says its p90 is unseen.
    assert terms["modedyn"]["aggregate"] == "mean" and terms["modedyn"]["covers"] == []
    # A run-wide tail narrower than a tenth is kept.
    assert own_term_aggregates({"body": {"statistics": ["p90"]}}, 0.05) == {"body": 0.05}
