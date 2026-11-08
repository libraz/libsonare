"""Create fixed temporal-key diagnostic predictions from exported production chroma."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

STATE_COUNT = 24
TEMPERATURE = 0.08
WINDOW_SECONDS = 8.0
STAY_PROBABILITY = 24.0 / 47.0
OTHER_PROBABILITY = 1.0 / 47.0


def _softmax(values: list[float]) -> list[float]:
    maximum = max(values)
    weights = [math.exp(value - maximum) for value in values]
    total = sum(weights)
    if not math.isfinite(total) or total <= 0.0:
        raise ValueError("invalid softmax total")
    return [weight / total for weight in weights]


def _candidate_distribution(candidates: Any, expected_profile: str | None = None) -> list[float]:
    if not isinstance(candidates, list) or len(candidates) != STATE_COUNT:
        raise ValueError("each key frame must contain exactly 24 candidates")
    logits = [0.0] * STATE_COUNT
    seen: set[int] = set()
    for item in candidates:
        if not isinstance(item, dict):
            raise TypeError("candidate must be an object")
        root = item.get("root")
        mode = item.get("mode")
        correlation = item.get("correlation")
        if isinstance(root, bool) or not isinstance(root, int) or not 0 <= root < 12:
            raise ValueError(f"invalid pitch-class root: {root!r}")
        if mode not in ("major", "minor"):
            raise ValueError(f"invalid mode: {mode!r}")
        if isinstance(correlation, bool) or not isinstance(correlation, (int, float)):
            raise TypeError("candidate correlation must be numeric")
        correlation = float(correlation)
        if not math.isfinite(correlation):
            raise ValueError("candidate correlation must be finite")
        if (
            expected_profile is not None
            and item.get("profile", expected_profile) != expected_profile
        ):
            raise ValueError("candidate profile differs from frozen whole-track profile")
        state = root * 2 + (mode == "minor")
        if state in seen:
            raise ValueError(f"duplicate key state {state}")
        seen.add(state)
        logits[state] = correlation / TEMPERATURE
    if len(seen) != STATE_COUNT:
        raise ValueError("candidate states do not cover all 24 keys")
    return _softmax(logits)


def _validate_export(export: Any) -> tuple[float, list[tuple[float, float, list[float]]]]:
    if not isinstance(export, dict):
        raise TypeError("export must be an object")
    duration = export.get("duration")
    if isinstance(duration, bool) or not isinstance(duration, (int, float)):
        raise TypeError("duration must be numeric")
    duration = float(duration)
    if not math.isfinite(duration) or duration <= 0:
        raise ValueError("duration must be positive and finite")
    profile = export.get("selected_profile")
    if not isinstance(profile, str) or not profile:
        raise ValueError("selected_profile must be a non-empty string")
    global_candidates = export.get("global_candidates")
    global_probs = _candidate_distribution(global_candidates, profile)
    windows = export.get("windows")
    if not isinstance(windows, list) or not windows:
        raise ValueError("export must contain non-empty windows")
    parsed: list[tuple[float, float, list[float]]] = []
    previous_end = 0.0
    for window in windows:
        if not isinstance(window, dict):
            raise TypeError("window must be an object")
        start, end = window.get("start"), window.get("end")
        if any(isinstance(v, bool) or not isinstance(v, (int, float)) for v in (start, end)):
            raise ValueError("window timestamps must be numeric")
        start, end = float(start), float(end)
        if not math.isfinite(start) or not math.isfinite(end) or start < 0 or end <= start:
            raise ValueError("invalid window interval")
        expected_start = len(parsed) * WINDOW_SECONDS
        expected_end = min(expected_start + WINDOW_SECONDS, duration)
        if abs(start - expected_start) > 1e-6 or abs(end - expected_end) > 1e-6:
            raise ValueError(
                "windows must use exact 8-second wall-clock cells, clipped at duration"
            )
        if abs(start - previous_end) > 1e-6:
            raise ValueError("windows must contiguously cover [0,duration]")
        if window.get("selected_profile") != profile:
            raise ValueError("window must declare the frozen whole-track profile")
        probabilities = _candidate_distribution(window.get("candidates"), profile)
        parsed.append((start, end, probabilities))
        previous_end = end
    if abs(previous_end - duration) > 1e-6:
        raise ValueError("windows must end at recording duration")
    return duration, [(0.0, duration, global_probs), *parsed]


def _logsumexp(values: list[float]) -> float:
    maximum = max(values)
    if maximum == -math.inf:
        return maximum
    return maximum + math.log(math.fsum(math.exp(value - maximum) for value in values))


def _logaddexp(left: float, right: float) -> float:
    maximum = max(left, right)
    if maximum == -math.inf:
        return maximum
    return maximum + math.log1p(math.exp(min(left, right) - maximum))


def _log_emissions(emissions: list[list[float]], durations: list[float]) -> list[list[float]]:
    if not emissions or len(emissions) != len(durations):
        raise ValueError("emissions and durations must be non-empty and aligned")
    if any(len(row) != STATE_COUNT for row in emissions):
        raise ValueError("each emission row must have 24 states")
    result = []
    for row, duration in zip(emissions, durations, strict=True):
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError("window durations must be positive and finite")
        if any(not math.isfinite(value) or value <= 0 for value in row):
            raise ValueError("emissions must be positive and finite")
        result.append([math.log(value) * duration / WINDOW_SECONDS for value in row])
    return result


def _hmm_posterior(emissions: list[list[float]], durations: list[float]) -> list[list[float]]:
    """24-state forward-backward with the frozen symmetric transition matrix."""
    log_emission = _log_emissions(emissions, durations)
    log_prior = -math.log(STATE_COUNT)
    log_stay_remainder = math.log(STAY_PROBABILITY - OTHER_PROBABILITY)
    log_other = math.log(OTHER_PROBABILITY)
    forward = [[log_prior + value for value in log_emission[0]]]
    for t in range(1, len(emissions)):
        previous = forward[t - 1]
        common_incoming = log_other + _logsumexp(previous)
        frame = [
            log_emission[t][state]
            + _logaddexp(common_incoming, log_stay_remainder + previous[state])
            for state in range(STATE_COUNT)
        ]
        forward.append(frame)

    backward = [[0.0] * STATE_COUNT for _ in emissions]
    for t in range(len(emissions) - 2, -1, -1):
        next_values = [
            log_emission[t + 1][state] + backward[t + 1][state] for state in range(STATE_COUNT)
        ]
        common_outgoing = log_other + _logsumexp(next_values)
        backward[t] = [
            _logaddexp(common_outgoing, log_stay_remainder + next_values[state])
            for state in range(STATE_COUNT)
        ]

    result: list[list[float]] = []
    for fwd, bwd in zip(forward, backward, strict=True):
        log_marginal = [left + right for left, right in zip(fwd, bwd, strict=True)]
        normalizer = _logsumexp(log_marginal)
        result.append([math.exp(value - normalizer) for value in log_marginal])
    return result


def _key_interval(start: float, end: float, probs: list[float]) -> dict[str, Any]:
    state = max(range(STATE_COUNT), key=probs.__getitem__)
    return {
        "start": start,
        "end": end,
        "root": state // 2,
        "mode": "minor" if state % 2 else "major",
    }


def _prediction_records(
    exports: list[dict[str, Any]],
) -> tuple[dict[str, list[dict[str, Any]]], dict[str, Any]]:
    records: dict[str, list[dict[str, Any]]] = {
        "raw_global": [],
        "independent_8s": [],
        "hmm_8s": [],
    }
    diagnostics: dict[str, Any] = {}
    seen_ids: set[str] = set()
    for item in exports:
        if not isinstance(item, dict) or not isinstance(item.get("id"), str) or not item["id"]:
            raise ValueError("each export needs a non-empty string id")
        recording_id = item["id"]
        if recording_id in seen_ids:
            raise ValueError(f"duplicate export id: {recording_id}")
        seen_ids.add(recording_id)
        duration, parsed = _validate_export(item.get("export"))
        global_probs = parsed[0][2]
        windows = parsed[1:]
        emissions = [row[2] for row in windows]
        durations = [end - start for start, end, _ in windows]
        posteriors = _hmm_posterior(emissions, durations)
        records["raw_global"].append(
            {"id": recording_id, "intervals": [_key_interval(0.0, duration, global_probs)]}
        )
        records["independent_8s"].append(
            {
                "id": recording_id,
                "intervals": [_key_interval(start, end, probs) for start, end, probs in windows],
            }
        )
        records["hmm_8s"].append(
            {
                "id": recording_id,
                "intervals": [
                    _key_interval(start, end, probs)
                    for (start, end, _), probs in zip(windows, posteriors, strict=True)
                ],
            }
        )
        occupancy = [0.0] * STATE_COUNT
        for (_, _, _), probs, span in zip(windows, posteriors, durations, strict=True):
            for state, probability in enumerate(probs):
                occupancy[state] += probability * span / duration
        diagnostics[recording_id] = {
            "purpose": "duration_weighted_posterior_occupancy_diagnostic_only",
            "selected_profile": item["export"]["selected_profile"],
            "occupancy": [
                {"root": state // 2, "mode": "minor" if state % 2 else "major", "mass": mass}
                for state, mass in enumerate(occupancy)
            ],
        }
    return records, diagnostics


def build_predictions(exports: list[dict[str, Any]]) -> dict[str, Any]:
    records, diagnostics = _prediction_records(exports)
    return {
        "predictions": {
            method: {"method_id": method, "records": method_records}
            for method, method_records in records.items()
        },
        "diagnostics": diagnostics,
        "protocol": {
            "version": "temporal_key_diagnostic_v3",
            "temperature": TEMPERATURE,
            "window_seconds": WINDOW_SECONDS,
            "transition_stay": STAY_PROBABILITY,
            "transition_other": OTHER_PROBABILITY,
            "transition_prior": "uniform",
            "evidence_duration_power": True,
            "path_estimator": "forward_backward_posterior_argmax",
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
