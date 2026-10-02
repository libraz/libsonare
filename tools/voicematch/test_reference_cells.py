"""Reference-indexed loss cells stay priced when a candidate goes missing."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from loss import LEVEL_DELTA_CAP_DB, TERM_UNITS, LossWeights, loss_terms, percussion_terms


def _skeleton(values, *, tail=True):
    values = list(values)
    result = {
        "init_db": list(values),
        "early_db_s": list(values),
        "late_db_s": list(values),
    }
    if tail:
        result["tail_db_s"] = list(values)
    return result


def _pitched_row(**over):
    row = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
        "held_rms_dbfs": -20.0,
        "held_crest_db": 12.0,
        "attack_hf_db": [1.0, 2.0, 3.0],
        "attack_lf_db": [1.0, 2.0, 3.0],
        "skeleton": _skeleton([-2.0] * 6),
    }
    row.update(over)
    return row


def _drum_row(**over):
    row = {
        "bands_db": [0.0, -6.0, -12.0],
        "band_decay_db_s": [-20.0, -30.0],
        "attack_ms": 1.0,
        "decay_ms": 200.0,
        "crest_db": 10.0,
        "held_rms_dbfs": -20.0,
        "held_crest_db": 12.0,
    }
    row.update(over)
    return row


def test_missing_or_truncated_candidate_arrays_keep_each_oracle_cell():
    oracle = _pitched_row()
    full = loss_terms([_pitched_row()], [oracle])
    missing = loss_terms(
        [
            _pitched_row(
                skeleton=None,
                attack_hf_db=[],
                attack_lf_db=[],
            )
        ],
        [oracle],
    )
    assert missing["init"] > full["init"]
    assert missing["slope"] > full["slope"]
    assert missing["tail"] > full["tail"]
    assert missing["hf"] > full["hf"]
    assert missing["lf"] > full["lf"]
    for term in ("init", "slope", "tail", "hf", "lf"):
        assert missing[f"{term}_absent"] == missing[f"{term}_cells"]

    truncated = loss_terms(
        [
            _pitched_row(
                skeleton=_skeleton([-2.0] * 2),
                attack_hf_db=[1.0],
                attack_lf_db=[1.0],
            )
        ],
        [oracle],
    )
    assert truncated["init_cells"] == 6.0 and truncated["init_absent"] == 4.0
    assert truncated["slope_cells"] == 12.0 and truncated["slope_absent"] == 8.0
    assert truncated["tail_cells"] == 6.0 and truncated["tail_absent"] == 4.0
    assert truncated["hf_cells"] == 3.0 and truncated["hf_absent"] == 2.0
    assert truncated["lf_cells"] == 3.0 and truncated["lf_absent"] == 2.0


def test_reference_without_new_arrays_remains_unknown():
    oracle = _pitched_row()
    oracle.pop("skeleton")
    oracle.pop("attack_hf_db")
    oracle.pop("attack_lf_db")
    candidate = _pitched_row(
        skeleton=None,
        attack_hf_db=[],
        attack_lf_db=[],
    )
    terms = loss_terms([candidate], [oracle])
    for term in ("init", "slope", "tail", "hf", "lf"):
        assert terms[f"{term}_cells"] == 0.0
        assert terms[f"{term}_absent"] == 0.0
        assert terms[term] == 0.0


def test_level_is_gain_offset_invariant_and_reports_shared_coverage():
    oracle = [_pitched_row(held_rms_dbfs=-30.0), _pitched_row(held_rms_dbfs=-32.0)]
    model = [_pitched_row(held_rms_dbfs=-21.0), _pitched_row(held_rms_dbfs=-23.0)]
    terms = loss_terms(model, oracle)
    assert terms["level"] == pytest.approx(0.0)
    assert terms["level_offset_db"] == pytest.approx(9.0)
    assert terms["level_notes"] == 2.0
    assert terms["level_expected"] == 2.0


def test_missing_level_is_capped_and_partial_level_keeps_reference_denominator():
    oracle = [_pitched_row(held_rms_dbfs=-30.0), _pitched_row(held_rms_dbfs=-32.0)]
    all_missing = loss_terms(
        [_pitched_row(held_rms_dbfs=None, held_crest_db=None)] * 2,
        oracle,
    )
    assert all_missing["level"] == pytest.approx(LEVEL_DELTA_CAP_DB)
    assert all_missing["level_notes"] == 0.0
    assert all_missing["level_expected"] == 2.0

    partial = loss_terms(
        [_pitched_row(held_rms_dbfs=-21.0), _pitched_row(held_rms_dbfs=None)],
        oracle,
    )
    assert partial["level"] == pytest.approx(LEVEL_DELTA_CAP_DB / 2.0)
    assert partial["level_notes"] == 1.0
    assert partial["level_expected"] == 2.0


def test_level_coverage_is_emitted_for_percussion_too():
    oracle = [_drum_row(held_rms_dbfs=-30.0)]
    model = [_drum_row(held_rms_dbfs=None, held_crest_db=None)]
    terms = percussion_terms(model, oracle)
    assert terms["level"] == pytest.approx(LEVEL_DELTA_CAP_DB)
    assert terms["level_notes"] == 0.0
    assert terms["level_expected"] == 1.0


def test_loss_weight_anchor_uses_level_expected_and_lf_metric_set_aliases():
    level = LossWeights({"level": 1.0})
    level.calibrate(
        {
            "level": 18.0,
            "level_notes": 2.0,
            "level_expected": 4.0,
        }
    )
    candidate = {"level": 0.0, "level_notes": 2.0, "level_expected": 4.0}
    assert level.coverage("level", candidate) == pytest.approx(0.5)

    pitched_lf = LossWeights({"lf": 1.0})
    pitched_lf.calibrate({"lf": 4.0, "lf_cells": 4.0})
    assert pitched_lf.coverage("lf", {"lf": 0.0, "lf_cells": 2.0}) == pytest.approx(0.5)

    percussion_lf = LossWeights({"lf": 1.0})
    percussion_lf.calibrate({"lf": 4.0, "lf_notes": 4.0})
    assert percussion_lf.coverage("lf", {"lf": 0.0, "lf_notes": 2.0}) == pytest.approx(0.5)


def test_loss_weight_anchor_rejects_nonfinite_level_reference():
    weights = LossWeights({"level": 1.0})
    with pytest.raises(ValueError, match="finite"):
        weights.anchor({"level": 1.0, "level_expected": float("nan")})


def test_all_missing_level_keeps_its_reference_denominator_when_baseline_is_zero():
    weights = LossWeights({"level": 0.5})
    weights.calibrate({"level": 0.0, "level_notes": 2.0, "level_expected": 2.0})
    missing = {"level": LEVEL_DELTA_CAP_DB, "level_notes": 0.0, "level_expected": 2.0}
    raw_cap_units = 0.5 * LEVEL_DELTA_CAP_DB / TERM_UNITS["level"]
    assert weights.combine(missing) * weights.reference == pytest.approx(raw_cap_units)


def test_a_partially_missing_level_is_charged_its_cap_once():
    weights = LossWeights({"level": 1.0})
    weights.calibrate({"level": 1.0, "level_notes": 4.0, "level_expected": 4.0})
    raw = (2.0 * LEVEL_DELTA_CAP_DB + 2.0 * 1.0) / 4.0
    partial = {"level": raw, "level_notes": 2.0, "level_expected": 4.0}
    assert weights.coverage("level", partial) == pytest.approx(0.5)
    contribution = weights.term_contributions(partial)["level"]
    assert contribution == pytest.approx(raw / TERM_UNITS["level"])
