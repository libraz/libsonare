#!/usr/bin/env python3
"""Compare two optional fixture JSON reports for local regression triage."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from typing import Any

from json_safe import dumps_strict, json_safe, write_json_strict


def observation_metric(item: dict[str, Any]) -> str:
    metric = str(item.get("metric", ""))
    if metric == "rt60" and item.get("mode"):
        return "rt60_" + str(item["mode"])
    if metric == "downbeat" and item.get("time_signature"):
        return "downbeat_" + str(item["time_signature"]).replace("/", "_")
    return metric


def observations(report: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    found: dict[tuple[str, str], dict[str, Any]] = {}
    for run in report.get("runs", []):
        for item in run.get("observations", []):
            metric = observation_metric(item)
            fixture = str(item.get("fixture", ""))
            if metric and fixture:
                found[(metric, fixture)] = item
    return found


def key_score(item: dict[str, Any]) -> float | tuple[int, int]:
    if item.get("mirex_score") is not None:
        return float(item["mirex_score"])
    if item.get("expected_root") is not None and item.get("expected_mode") is not None:
        return 1.0 if bool(item.get("correct")) else 0.0
    return int(item["measured_root"]), int(item["measured_mode"])


def score(item: dict[str, Any]) -> float | tuple[float, ...] | tuple[int, int] | None:
    metric = item.get("metric")
    if metric == "bpm":
        return float(item["relative_error_percent"])
    if metric == "rt60" or str(metric).startswith("rt60_"):
        return abs(float(item["measured_sec"]) - float(item["expected_sec"]))
    if metric in ("edt", "c50", "c80", "d50", "ebu_lufs", "ebu_lra", "ebu_true_peak"):
        return abs(float(item["measured"]) - float(item["expected"]))
    if metric == "wasm_size":
        if item.get("increase_percent") is not None:
            return float(item["increase_percent"])
        return float(item["current_bytes"])
    if metric in ("beat", "downbeat"):
        return float(item["f_measure"])
    if metric == "beat_improvement":
        return float(item["improvement"])
    if metric in ("chord_wcsr", "chord_extended_wcsr"):
        return float(item["wcsr"])
    if metric == "chord_detail":
        return (
            float(item["exact_wcsr"]),
            float(item["root_accuracy"]),
            float(item["quality_accuracy"]),
        )
    if metric == "chord_bass_acc":
        return float(item["accuracy"])
    if metric == "chord_change_rate":
        return float(item["changes_per_minute"])
    if metric == "chord_change_reduction":
        return float(item["reduction"])
    if metric == "key":
        return key_score(item)
    if metric == "meter":
        return int(item["measured_numerator"]), int(item["measured_denominator"])
    return None


def finite_number(value: Any) -> bool:
    return isinstance(value, (float, int)) and math.isfinite(float(value))


def finite_score(value: Any) -> bool:
    if isinstance(value, tuple):
        return all(finite_number(item) for item in value)
    return finite_number(value)


def json_safe_score(value: Any) -> Any:
    return json_safe(value)


def is_regression(metric: str, before: Any, after: Any, tolerance: float) -> bool:
    if before is None or after is None:
        return False
    before_finite = finite_score(before)
    after_finite = finite_score(after)
    if before_finite and not after_finite:
        return True
    if not before_finite or not after_finite:
        return False
    if metric == "chord_detail":
        return any(float(current) < float(baseline) - tolerance for baseline, current in zip(before, after))
    if metric in (
        "bpm",
        "rt60",
        "edt",
        "c50",
        "c80",
        "d50",
        "ebu_lufs",
        "ebu_lra",
        "ebu_true_peak",
        "wasm_size",
        "chord_change_rate",
    ) or metric.startswith("rt60_"):
        return float(after) > float(before) + tolerance
    if metric in (
        "beat",
        "beat_improvement",
        "downbeat",
        "chord_wcsr",
        "chord_extended_wcsr",
        "chord_bass_acc",
        "chord_change_reduction",
        "key",
    ) or metric.startswith("downbeat_"):
        if isinstance(before, tuple) or isinstance(after, tuple):
            return after != before
        return float(after) < float(before) - tolerance
    if metric == "meter":
        return after != before
    return False


def compare_reports(baseline: dict[str, Any], current: dict[str, Any], tolerance: float) -> dict[str, Any]:
    baseline_obs = observations(baseline)
    current_obs = observations(current)
    rows: list[dict[str, Any]] = []
    regressions = 0

    for key in sorted(set(baseline_obs) | set(current_obs)):
        metric, fixture = key
        before_item = baseline_obs.get(key)
        after_item = current_obs.get(key)
        before = score(before_item) if before_item else None
        after = score(after_item) if after_item else None
        missing = before_item is not None and after_item is None
        added = before_item is None and after_item is not None
        regressed = missing or is_regression(metric, before, after, tolerance)
        regressions += int(regressed)
        rows.append(
            {
                "metric": metric,
                "fixture": fixture,
                "baseline": json_safe_score(before),
                "current": json_safe_score(after),
                "missing": missing,
                "added": added,
                "regression": regressed,
            }
        )

    return {"ok": regressions == 0, "regressions": regressions, "comparisons": rows}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("current", type=Path)
    parser.add_argument("--output", type=Path, help="Write JSON comparison to this path.")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=1e-6,
        help="Numeric tolerance for score changes before flagging a regression.",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    import json

    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    args = parse_args()
    result = compare_reports(load_json(args.baseline), load_json(args.current), args.tolerance)
    if args.output:
        write_json_strict(args.output, result, indent=2, sort_keys=True)
    else:
        print(dumps_strict(result, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
