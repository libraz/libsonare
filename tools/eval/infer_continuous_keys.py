"""Generate one-second key predictions with a duration-scaled transition model."""

from __future__ import annotations

import argparse
import itertools
import json
import math
from pathlib import Path
from typing import Any

from infer_temporal_keys import (
    STATE_COUNT,
    TEMPERATURE,
    _candidate_distribution,
    _key_interval,
    _logaddexp,
    _logsumexp,
    _validate_export,
)

FINE_WINDOW_SECONDS = 1.0
EMISSION_REFERENCE_SECONDS = 8.0
RHO_AT_EIGHT_SECONDS = 23.0 / 47.0
DECAY_RATE = -math.log(RHO_AT_EIGHT_SECONDS) / EMISSION_REFERENCE_SECONDS
FINE_POOLING = "physical_frame_support_overlap"


def _continuous_transition(delta_seconds: float) -> tuple[float, float]:
    """Return diagonal and off-diagonal transition probabilities for elapsed time."""
    if not math.isfinite(delta_seconds) or delta_seconds <= 0.0:
        raise ValueError("transition elapsed time must be positive and finite")
    rho = math.exp(-DECAY_RATE * delta_seconds)
    off_diagonal = -math.expm1(-DECAY_RATE * delta_seconds) / STATE_COUNT
    diagonal = rho + off_diagonal
    return diagonal, off_diagonal


def _log_evidence(emissions: list[list[float]], durations: list[float]) -> list[list[float]]:
    if not emissions or len(emissions) != len(durations):
        raise ValueError("emissions and durations must be non-empty and aligned")
    result = []
    for row, duration in zip(emissions, durations, strict=True):
        if len(row) != STATE_COUNT:
            raise ValueError("each emission row must have 24 states")
        if not math.isfinite(duration) or duration <= 0.0:
            raise ValueError("cell durations must be positive and finite")
        if any(not math.isfinite(value) or value <= 0.0 for value in row):
            raise ValueError("emissions must be positive and finite")
        result.append([math.log(value) * duration / EMISSION_REFERENCE_SECONDS for value in row])
    return result


def _cell_center_deltas(durations: list[float]) -> list[float]:
    if not durations:
        raise ValueError("at least one cell duration is required")
    if any(not math.isfinite(value) or value <= 0.0 for value in durations):
        raise ValueError("cell durations must be positive and finite")
    return [(previous + current) / 2.0 for previous, current in itertools.pairwise(durations)]


def _log_common_transition_mass(delta_seconds: float) -> float:
    """Return log((1-rho)/24) without losing tiny elapsed-time mass."""
    decay = DECAY_RATE * delta_seconds
    if decay == 0.0:
        return math.log(DECAY_RATE) + math.log(delta_seconds) - math.log(STATE_COUNT)
    if math.isinf(decay):
        return -math.log(STATE_COUNT)
    return math.log(-math.expm1(-decay)) - math.log(STATE_COUNT)


def _continuous_hmm_posteriors(
    emissions: list[list[float]], durations: list[float]
) -> list[list[float]]:
    """Stable log-domain forward-backward using the symmetric 24-state prior."""
    log_emissions = _log_evidence(emissions, durations)
    deltas = _cell_center_deltas(durations)
    forward = [[-math.log(STATE_COUNT) + value for value in log_emissions[0]]]
    for index, delta in enumerate(deltas, start=1):
        previous = forward[index - 1]
        log_common = _log_common_transition_mass(delta)
        log_rho = -DECAY_RATE * delta
        common_incoming = log_common + _logsumexp(previous)
        forward.append(
            [
                log_emissions[index][state] + _logaddexp(common_incoming, log_rho + previous[state])
                for state in range(STATE_COUNT)
            ]
        )

    backward = [[0.0] * STATE_COUNT for _ in log_emissions]
    for index in range(len(log_emissions) - 2, -1, -1):
        delta = deltas[index]
        next_values = [
            log_emissions[index + 1][state] + backward[index + 1][state]
            for state in range(STATE_COUNT)
        ]
        common_outgoing = _log_common_transition_mass(delta) + _logsumexp(next_values)
        log_rho = -DECAY_RATE * delta
        backward[index] = [
            _logaddexp(common_outgoing, log_rho + next_values[state])
            for state in range(STATE_COUNT)
        ]

    posteriors = []
    for forward_row, backward_row in zip(forward, backward, strict=True):
        log_marginal = [left + right for left, right in zip(forward_row, backward_row, strict=True)]
        normalizer = _logsumexp(log_marginal)
        posteriors.append([math.exp(value - normalizer) for value in log_marginal])
    return posteriors


def _validate_fine_export(
    export: Any,
) -> tuple[float, str, list[tuple[float, float, int, list[float]]]]:
    duration, _ = _validate_export(export)
    fine_window_seconds = export.get("fine_window_seconds")
    if (
        type(fine_window_seconds) not in (int, float)
        or not math.isfinite(fine_window_seconds)
        or fine_window_seconds != FINE_WINDOW_SECONDS
    ):
        raise ValueError("export must declare fine_window_seconds=1.0")
    if export.get("fine_pooling") != FINE_POOLING:
        raise ValueError(f"export must declare fine_pooling={FINE_POOLING!r}")
    profile = export.get("selected_profile")
    raw_windows = export.get("fine_windows")
    expected_count = math.ceil(duration / FINE_WINDOW_SECONDS)
    if not isinstance(raw_windows, list) or len(raw_windows) != expected_count:
        raise ValueError(f"export must contain exactly {expected_count} fine windows")

    parsed: list[tuple[float, float, int, list[float]]] = []
    supported_total = 0.0
    for index, window in enumerate(raw_windows):
        if not isinstance(window, dict):
            raise TypeError(f"fine_windows[{index}] must be an object")
        start, end = window.get("start"), window.get("end")
        if any(type(value) not in (int, float) for value in (start, end)):
            raise TypeError(f"fine_windows[{index}] timestamps must be numeric")
        start, end = float(start), float(end)
        expected_start = index * FINE_WINDOW_SECONDS
        expected_end = min(expected_start + FINE_WINDOW_SECONDS, duration)
        if not math.isfinite(start) or not math.isfinite(end):
            raise ValueError(f"fine_windows[{index}] timestamps must be finite")
        if abs(start - expected_start) > 1e-9 or abs(end - expected_end) > 1e-9:
            raise ValueError("fine windows must use exact one-second cells, clipped at duration")
        if window.get("selected_profile") != profile:
            raise ValueError("fine window profile differs from frozen whole-track profile")
        frame_count = window.get("frame_count")
        if type(frame_count) is not int or frame_count < 1:
            raise ValueError(f"fine_windows[{index}].frame_count must be a positive integer")
        supported_seconds = window.get("supported_seconds")
        if type(supported_seconds) not in (int, float):
            raise TypeError(f"fine_windows[{index}].supported_seconds must be numeric")
        supported_seconds = float(supported_seconds)
        cell_duration = end - start
        if (
            not math.isfinite(supported_seconds)
            or supported_seconds <= 0.0
            or not math.isclose(supported_seconds, cell_duration, rel_tol=0.0, abs_tol=1e-9)
        ):
            raise ValueError(f"fine_windows[{index}] support must cover its full cell")
        probabilities = _candidate_distribution(window.get("candidates"), profile)
        parsed.append((start, end, frame_count, probabilities))
        supported_total += supported_seconds
    if not math.isclose(supported_total, duration, rel_tol=0.0, abs_tol=1e-8):
        raise ValueError("fine-cell support must sum to recording duration")
    return duration, profile, parsed


def build_predictions(exports: list[dict[str, Any]]) -> dict[str, Any]:
    records: dict[str, list[dict[str, Any]]] = {
        "independent_1s": [],
        "hmm_ct_1s": [],
    }
    diagnostics: dict[str, Any] = {}
    seen_ids: set[str] = set()
    for item in exports:
        if not isinstance(item, dict) or type(item.get("id")) is not str or not item["id"]:
            raise ValueError("each export needs a non-empty string id")
        recording_id = item["id"]
        if recording_id in seen_ids:
            raise ValueError(f"duplicate export id: {recording_id}")
        seen_ids.add(recording_id)
        export = item.get("export")
        _duration, profile, fine_windows = _validate_fine_export(export)
        emissions = [window[3] for window in fine_windows]
        durations = [end - start for start, end, _, _ in fine_windows]
        posteriors = _continuous_hmm_posteriors(emissions, durations)
        intervals = [(start, end, probabilities) for start, end, _, probabilities in fine_windows]
        records["independent_1s"].append(
            {
                "id": recording_id,
                "intervals": [
                    _key_interval(start, end, probabilities)
                    for start, end, probabilities in intervals
                ],
            }
        )
        records["hmm_ct_1s"].append(
            {
                "id": recording_id,
                "intervals": [
                    _key_interval(start, end, posterior)
                    for (start, end, _), posterior in zip(intervals, posteriors, strict=True)
                ],
            }
        )
        deltas = _cell_center_deltas(durations)
        diagnostics[recording_id] = {
            "selected_profile": profile,
            "cells": [
                {
                    "start": start,
                    "end": end,
                    "duration_seconds": end - start,
                    "frame_count": frame_count,
                    "transition_elapsed_seconds": deltas[index - 1] if index else None,
                    "posterior": posterior,
                }
                for index, ((start, end, frame_count, _), posterior) in enumerate(
                    zip(fine_windows, posteriors, strict=True)
                )
            ],
        }
    return {
        "predictions": {
            method: {"method_id": method, "records": values} for method, values in records.items()
        },
        "diagnostics": diagnostics,
        "protocol": {
            "version": "continuous_time_key_diagnostic_v1",
            "cell_seconds": FINE_WINDOW_SECONDS,
            "physical_pooling": FINE_POOLING,
            "temperature": TEMPERATURE,
            "emission_duration_reference_seconds": EMISSION_REFERENCE_SECONDS,
            "rho_at_8_seconds": RHO_AT_EIGHT_SECONDS,
            "decay_rate_per_second": DECAY_RATE,
            "initial_prior": "uniform_24_state",
            "transition": "symmetric_continuous_time_semigroup",
            "transition_parameters": "rho=exp(-lambda*delta); off=(1-rho)/24; diag=rho+off",
            "transition_elapsed_seconds": "between_contiguous_cell_centers",
            "decoder": "forward_backward_marginal_argmax",
            "parameter_search": False,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--output-prefix", required=True, type=Path)
    args = parser.parse_args()
    payload = json.loads(args.exports.read_text())
    exports = payload.get("exports") if isinstance(payload, dict) else None
    if not isinstance(exports, list):
        raise TypeError("exports JSON must be an object with an exports array")
    result = build_predictions(exports)
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    for method, prediction in result["predictions"].items():
        output = args.output_prefix.with_name(f"{args.output_prefix.name}-{method}.json")
        output.write_text(json.dumps(prediction, indent=2) + "\n")
    diagnostics_path = args.output_prefix.with_name(f"{args.output_prefix.name}-diagnostics.json")
    diagnostics_path.write_text(
        json.dumps({"diagnostics": result["diagnostics"], "protocol": result["protocol"]}, indent=2)
        + "\n"
    )


if __name__ == "__main__":
    main()
