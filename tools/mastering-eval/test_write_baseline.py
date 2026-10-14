"""Regression tests for mastering-eval's step completeness and shared defaults."""

from __future__ import annotations

import math

import pytest
from nonvacuity import _declick_defaults
from run import _declick_params
from write_baseline import STEP_FLOOR_SOURCE, STEP_MATERIAL, one_step_resolution


def _row(case: str, *, delta: dict | None = None, item: str = STEP_MATERIAL) -> dict:
    return {
        "case": case,
        "item": item,
        "delta": delta if delta is not None else {"response": 2.0, "quiet": 0.5},
    }


def _complete_gate() -> dict:
    rows = []
    for knob in STEP_FLOOR_SOURCE:
        rows.extend((_row(f"{knob}-"), _row(f"{knob}+")))
    return {
        "tables": {"steps": rows},
        "ensembles": [
            {
                "case": source,
                "spread": {
                    "response": {"range": 1.0},
                    "quiet": {"range": 1.0},
                },
            }
            for source in STEP_FLOOR_SOURCE.values()
        ],
        "steps_not_taken": [],
    }


def test_complete_ledger_keeps_legacy_resolution_shape_and_order():
    result = one_step_resolution(_complete_gate())

    assert "coverage" not in result
    assert list(result["ratio_over_floor"]) == ["response", "quiet"]
    assert result["clears_floor_for_every_knob"] == ["response"]
    assert result["silent_for_every_knob"] == ["quiet"]
    assert set(result["ratio_over_floor"]["response"]) == set(STEP_FLOOR_SOURCE)


def test_missing_step_sign_is_not_a_measured_knob():
    gate = _complete_gate()
    gate["tables"]["steps"] = [
        row for row in gate["tables"]["steps"] if row["case"] != "reduction_db+"
    ]
    gate["steps_not_taken"] = [
        {"knob": "reduction_db", "default": 26.0, "requested": 31.2, "reason": "probe"}
    ]

    result = one_step_resolution(gate)
    coverage = result["coverage"]

    assert coverage["complete"] is False
    assert coverage["missing_step_cases"] == ["reduction_db+"]
    assert coverage["steps_not_taken"] == gate["steps_not_taken"]
    assert "reduction_db" not in result["ratio_over_floor"]["response"]
    assert result["clears_floor_for_every_knob"] == []
    assert result["silent_for_every_knob"] == []


def test_missing_floor_source_cannot_be_hidden_by_the_other_knobs():
    gate = _complete_gate()
    gate["ensembles"] = [entry for entry in gate["ensembles"] if entry["case"] != "D3s"]

    result = one_step_resolution(gate)

    assert result["coverage"]["missing_floor_sources"] == ["D3s"]
    assert set(result["ratio_over_floor"]["response"]) == {
        "over_subtraction",
        "spectral_floor",
    }
    assert result["clears_floor_for_every_knob"] == []


def test_none_delta_does_not_call_max_or_create_a_ratio():
    gate = _complete_gate()
    for row in gate["tables"]["steps"]:
        if row["case"] == "over_subtraction+":
            row["delta"]["response"] = None

    result = one_step_resolution(gate)

    assert result["coverage"]["complete"] is False
    assert "over_subtraction" not in result["ratio_over_floor"]["response"]
    assert set(result["ratio_over_floor"]["response"]) == {
        "reduction_db",
        "noise_estimation_quantile",
        "spectral_floor",
    }


def _assert_incomplete_without_invalid_ratios(result: dict) -> None:
    assert result["coverage"]["complete"] is False
    assert result["clears_floor_for_every_knob"] == []
    assert result["silent_for_every_knob"] == []
    for ratios in result["ratio_over_floor"].values():
        assert all(math.isfinite(value) for value in ratios.values())


@pytest.mark.parametrize("bad_floor", [0.0, math.nan, math.inf, -math.inf])
def test_invalid_floor_values_are_skipped_without_invalid_ratios(bad_floor: float):
    gate = _complete_gate()
    for entry in gate["ensembles"]:
        if entry["case"] == "D3s":
            entry["spread"]["response"]["range"] = bad_floor
            entry["spread"]["quiet"]["range"] = bad_floor

    result = one_step_resolution(gate)

    _assert_incomplete_without_invalid_ratios(result)
    assert "reduction_db" not in result["ratio_over_floor"]["response"]
    assert "noise_estimation_quantile" not in result["ratio_over_floor"]["quiet"]


@pytest.mark.parametrize("bad_delta", [math.nan, math.inf, -math.inf, [], None])
def test_invalid_delta_values_are_skipped_without_invalid_ratios(bad_delta):
    gate = _complete_gate()
    for row in gate["tables"]["steps"]:
        if row["case"] == "spectral_floor-":
            row["delta"]["response"] = bad_delta
            row["delta"]["quiet"] = bad_delta

    result = one_step_resolution(gate)

    _assert_incomplete_without_invalid_ratios(result)
    assert "spectral_floor" not in result["ratio_over_floor"]["response"]
    assert "spectral_floor" not in result["ratio_over_floor"]["quiet"]


def test_missing_both_step_signs_cannot_complete_a_knob():
    gate = _complete_gate()
    gate["tables"]["steps"] = [
        row
        for row in gate["tables"]["steps"]
        if row["case"] not in {"over_subtraction-", "over_subtraction+"}
    ]

    result = one_step_resolution(gate)

    _assert_incomplete_without_invalid_ratios(result)
    assert result["coverage"]["missing_step_cases"] == [
        "over_subtraction-",
        "over_subtraction+",
    ]
    assert "over_subtraction" not in result["ratio_over_floor"]["response"]


def test_wrong_material_does_not_complete_a_pair():
    gate = _complete_gate()
    for row in gate["tables"]["steps"]:
        if row["case"] == "spectral_floor+":
            row["item"] = "speech_dynamic_hum50"

    result = one_step_resolution(gate)

    assert result["coverage"]["missing_step_cases"] == ["spectral_floor+"]
    assert "spectral_floor" not in result["ratio_over_floor"]["response"]


@pytest.mark.parametrize(
    "defect",
    [
        {"amplitudes": [0.4, -0.2], "width_samples": 3},
        {"amplitudes": [0.75, -0.35, 0.12], "width_samples": 1},
        {"amplitudes": [-0.01, 0.06], "width_samples": 64},
    ],
)
def test_declick_defaults_follow_runner_for_real_defect(defect: dict):
    assert _declick_defaults(defect) == _declick_params(defect)


def test_declick_defaults_keep_none_fallback():
    assert _declick_defaults(None) == {"threshold": 0.8, "max_click_samples": 8}
