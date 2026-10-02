"""Absolute fit quality must keep residuals and measurement coverage honest."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import ClassVar

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from autofit import check_validation_partition
from fit_quality import quality_report
from loss_weights import TERM_UNITS, LossWeights


class _Loss:
    weights: ClassVar = {"harm": 2.0}
    baseline_counts: ClassVar = {"harm": 10.0}

    def active(self):
        return ("harm",)

    def coverage(self, name, terms):
        current = terms.get("harm_bins")
        if current is None:
            return None
        return current / self.baseline_counts[name]

    def term_contributions(self, terms, *, fixed_units=False):
        assert fixed_units
        return {"harm": self.weights["harm"] * terms["harm"] / TERM_UNITS["harm"]}


class _PitchedLfLoss:
    weights: ClassVar = {"lf": 1.0}
    baseline_counts: ClassVar = {"lf": 0.0}

    def active(self):
        return ("lf",)

    def coverage(self, name, terms):
        return 1.0

    def term_contributions(self, terms, *, fixed_units=False):
        assert fixed_units
        return {"lf": terms["lf"] / TERM_UNITS["lf"]}


def test_a_zero_residual_is_not_the_same_as_no_measurement():
    measured = quality_report({"harm": 0.0, "harm_bins": 10.0, "comparable": 1.0}, _Loss())
    absent = quality_report({"harm": 0.0, "harm_bins": 0.0, "comparable": 1.0}, _Loss())

    assert measured["terms"]["harm"]["residual_units"] == pytest.approx(0.0)
    assert measured["terms"]["harm"]["unmeasured"] is False
    assert measured["met"] is True
    assert absent["terms"]["harm"]["residual_units"] is None
    assert absent["terms"]["harm"]["unmeasured"] is True
    assert absent["full_known_coverage"] is False
    assert absent["met"] is False


def test_partial_dropout_reports_coverage_against_the_anchored_count():
    report = quality_report({"harm": 2.0, "harm_bins": 1.0, "comparable": 1.0}, _Loss())
    row = report["terms"]["harm"]
    assert row["count"] == pytest.approx(1.0)
    assert row["expected"] == pytest.approx(10.0)
    assert row["coverage"] == pytest.approx(0.1)
    assert row["residual_units"] == pytest.approx(2.0 / TERM_UNITS["harm"])
    assert row["target_met"] is False
    assert report["full_known_coverage"] is False
    assert report["met"] is False


def test_a_capped_cell_cannot_attain_the_target_even_with_zero_residual():
    report = quality_report(
        {
            "harm": 0.0,
            "harm_bins": 10.0,
            "harm_capped": 1.0,
            "comparable": 1.0,
        },
        _Loss(),
    )
    row = report["terms"]["harm"]
    assert row["residual_units"] == pytest.approx(0.0)
    assert row["coverage_complete"] is False
    assert row["target_met"] is False
    assert report["met"] is False


def test_missing_count_is_unmeasured_even_when_the_raw_term_is_zero():
    report = quality_report({"harm": 0.0, "comparable": 1.0}, _Loss())
    row = report["terms"]["harm"]
    assert row["count"] is None
    assert row["residual_units"] is None
    assert row["target_met"] is None
    assert report["met"] is False


def test_pitched_lf_uses_its_fixed_oracle_cells_instead_of_percussion_count():
    loss = _PitchedLfLoss()
    baseline = {"lf": 0.0, "lf_cells": 4.0, "comparable": 1.0}
    complete = quality_report(
        {"lf": 0.0, "lf_cells": 4.0, "comparable": 1.0},
        loss,
        baseline_terms=baseline,
    )
    partial = quality_report(
        {"lf": 0.0, "lf_cells": 2.0, "comparable": 1.0},
        loss,
        baseline_terms=baseline,
    )
    assert complete["terms"]["lf"]["count"] is None
    assert complete["terms"]["lf"]["coverage"] == pytest.approx(1.0)
    assert complete["met"] is True
    assert partial["terms"]["lf"]["coverage"] == pytest.approx(0.5)
    assert partial["met"] is False


def test_real_objective_resolves_pitched_lf_and_reference_level_counts():
    baseline = {
        "lf": 0.0,
        "lf_cells": 4.0,
        "level": 0.0,
        "level_notes": 2.0,
        "level_expected": 3.0,
        "comparable": 1.0,
    }
    loss = LossWeights({"lf": 1.0, "level": 1.0})
    loss.anchor(baseline)
    report = quality_report(baseline, loss)
    assert report["terms"]["lf"]["coverage_complete"] is True
    assert report["terms"]["lf"]["count"] == 4.0
    assert report["terms"]["level"]["coverage"] == pytest.approx(2.0 / 3.0)
    assert report["terms"]["level"]["target_met"] is False


def _args(**kwargs):
    base = argparse.Namespace(
        program=0,
        pattern="sustain",
        notes="60,72",
        velocities="",
        percussive=False,
        drum_gate_ms=0,
        corpus="",
        corpus_timbre="",
        validate_notes="",
        validate_velocities="",
    )
    for key, value in kwargs.items():
        setattr(base, key, value)
    return base


def test_validation_partition_rejects_overlap_even_when_fit_notes_are_explicit():
    with pytest.raises(ValueError, match="overlaps"):
        check_validation_partition(_args(validate_notes="72,84"))


def test_validation_partition_marks_missing_independent_axis():
    result = check_validation_partition(_args())
    assert result["status"] == "independent_missing"
    assert result["independent"] is False
