#!/usr/bin/env python3
"""Evaluate optional fixture report observations against Phase 3-style targets."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from statistics import mean
from typing import Any

from json_safe import dumps_strict, write_json_strict


DEFAULT_TARGETS = {
    "bpm": {"pass_rate_min": 0.85, "octave_error_rate_max": 0.05, "min_count": 50},
    "key": {"pass_rate_min": 0.80, "min_count": 50},
    "key_mirex": {"mean_min": 0.80, "min_count": 50},
    "key_top3": {"pass_rate_min": 0.90, "min_count": 50},
    "key_top5": {"pass_rate_min": 0.95, "min_count": 50},
    "meter": {"pass_rate_min": 0.80, "min_count": 6},
    "meter_4_4": {"pass_rate_min": 0.80, "min_count": 3},
    "meter_3_4": {"pass_rate_min": 0.80, "min_count": 2},
    "meter_6_8": {"pass_rate_min": 0.80, "min_count": 1},
    "beat": {"mean_min": 0.80, "min_count": 10},
    "beat_improvement": {"mean_min": 0.05, "min_count": 10},
    "downbeat": {"mean_min": 0.75, "min_count": 20},
    "downbeat_4_4": {"mean_min": 0.75, "min_count": 10},
    "downbeat_3_4": {"mean_min": 0.70, "min_count": 5},
    "downbeat_6_8": {"mean_min": 0.65, "min_count": 5},
    "chord_wcsr": {"mean_min": 0.78, "min_count": 12},
    "chord_extended_wcsr": {"mean_min": 0.75, "min_count": 12},
    "chord_exact_wcsr": {"mean_min": 0.85, "min_count": 12},
    "chord_root_acc": {"mean_min": 0.85, "min_count": 12},
    "chord_quality_acc": {"mean_min": 0.85, "min_count": 12},
    "chord_bass_acc": {"mean_min": 0.70, "min_count": 12},
    "chord_change_rate": {"mean_max": 60.0},
    "chord_change_reduction": {"mean_min": 0.50, "min_count": 12},
    "rt60": {"pass_rate_min": 1.0, "min_count": 6},
    "rt60_ir": {"pass_rate_min": 1.0, "min_count": 3},
    "rt60_blind": {"pass_rate_min": 1.0, "min_count": 3},
    "edt": {"pass_rate_min": 1.0},
    "c50": {"pass_rate_min": 1.0},
    "c80": {"pass_rate_min": 1.0},
    "d50": {"pass_rate_min": 1.0},
    "ebu_lufs": {"pass_rate_min": 1.0, "min_count": 5},
    "ebu_lra": {"pass_rate_min": 1.0},
    "ebu_true_peak": {"pass_rate_min": 1.0},
    "wasm_size": {"mean_max": 30.0, "min_count": 1},
}


def all_observations(report: dict[str, Any]) -> list[dict[str, Any]]:
    observations: list[dict[str, Any]] = []
    for run in report.get("runs", []):
        observations.extend(run.get("observations", []))
    return observations


def metric_value(item: dict[str, Any]) -> float | None:
    metric = item.get("metric")
    if metric == "bpm":
        return float(item["relative_error_percent"])
    if metric == "key":
        return 1.0 if bool(item.get("correct")) else 0.0
    if metric == "key_mirex":
        score = item.get("mirex_score")
        return float(score) if score is not None else None
    if metric == "key_top3":
        rank = item.get("expected_rank")
        return 1.0 if rank is not None and int(rank) > 0 and int(rank) <= 3 else 0.0
    if metric == "key_top5":
        rank = item.get("expected_rank")
        return 1.0 if rank is not None and int(rank) > 0 and int(rank) <= 5 else 0.0
    if metric == "meter" or str(metric).startswith("meter_"):
        return 1.0 if bool(item.get("correct")) else 0.0
    if metric == "beat_improvement":
        return float(item["improvement"])
    if metric in ("beat", "downbeat") or str(metric).startswith("downbeat_"):
        return float(item["f_measure"])
    if metric in ("chord_wcsr", "chord_extended_wcsr"):
        return float(item["wcsr"])
    if metric == "chord_exact_wcsr":
        return float(item["exact_wcsr"])
    if metric == "chord_root_acc":
        return float(item["root_accuracy"])
    if metric == "chord_quality_acc":
        return float(item["quality_accuracy"])
    if metric == "chord_bass_acc":
        return float(item["accuracy"])
    if metric == "chord_change_rate":
        return float(item["changes_per_minute"])
    if metric == "chord_change_reduction":
        return float(item["reduction"])
    if metric == "rt60" or str(metric).startswith("rt60_"):
        return abs(float(item["measured_sec"]) - float(item["expected_sec"]))
    if metric in ("edt", "c50", "c80", "d50", "ebu_lufs", "ebu_lra", "ebu_true_peak"):
        return abs(float(item["measured"]) - float(item["expected"]))
    if metric == "wasm_size":
        increase = item.get("increase_percent")
        return float(increase) if increase is not None else None
    return None


def metric_pass(item: dict[str, Any]) -> bool | None:
    metric = item.get("metric")
    if metric == "bpm":
        return float(item["relative_error_percent"]) <= float(item["tolerance_percent"])
    if metric == "key":
        return bool(item.get("correct")) if item.get("expected_root") is not None else None
    if metric == "key_mirex":
        return None
    if metric == "key_top3":
        rank = item.get("expected_rank")
        return int(rank) > 0 and int(rank) <= 3 if rank is not None else None
    if metric == "key_top5":
        rank = item.get("expected_rank")
        return int(rank) > 0 and int(rank) <= 5 if rank is not None else None
    if metric == "meter" or str(metric).startswith("meter_"):
        return bool(item.get("correct"))
    if metric == "beat_improvement":
        threshold = item.get("threshold")
        return float(item["improvement"]) >= float(threshold) if threshold is not None else None
    if metric in ("beat", "downbeat") or str(metric).startswith("downbeat_"):
        return float(item["f_measure"]) >= float(item["threshold"])
    if metric in ("chord_wcsr", "chord_extended_wcsr"):
        threshold = item.get("threshold")
        return float(item["wcsr"]) >= float(threshold) if threshold is not None else None
    if metric in ("chord_exact_wcsr", "chord_root_acc", "chord_quality_acc"):
        return None
    if metric == "chord_bass_acc":
        return float(item["accuracy"]) >= float(item["threshold"])
    if metric == "chord_change_rate":
        threshold = item.get("threshold")
        return float(item["changes_per_minute"]) <= float(threshold) if threshold is not None else None
    if metric == "chord_change_reduction":
        threshold = item.get("threshold")
        return float(item["reduction"]) >= float(threshold) if threshold is not None else None
    if metric == "rt60" or str(metric).startswith("rt60_"):
        return abs(float(item["measured_sec"]) - float(item["expected_sec"])) <= float(item["tolerance_sec"])
    if metric in ("edt", "c50", "c80", "d50", "ebu_lufs", "ebu_lra", "ebu_true_peak"):
        return abs(float(item["measured"]) - float(item["expected"])) <= float(item["tolerance"])
    if metric == "wasm_size":
        increase = item.get("increase_percent")
        return float(increase) <= 30.0 if increase is not None else None
    return None


def is_bpm_octave_error(item: dict[str, Any]) -> bool:
    measured = float(item["measured_bpm"])
    expected = float(item["expected_bpm"])
    if measured <= 0.0 or expected <= 0.0:
        return False
    ratio = measured / expected
    tolerance = float(item.get("tolerance_percent", 4.0)) / 100.0
    for factor in (0.25, 0.5, 2.0, 4.0):
        if abs(ratio - factor) <= factor * tolerance:
            return True
    return False


def field_histogram(items: list[dict[str, Any]], field: str) -> dict[str, int]:
    histogram: dict[str, int] = {}
    for item in items:
        value = item.get(field)
        if value is not None:
            key = str(value)
            histogram[key] = histogram.get(key, 0) + 1
    return dict(sorted(histogram.items()))


def finite_field_mean(items: list[dict[str, Any]], field: str) -> float | None:
    values = [
        float(item[field])
        for item in items
        if item.get(field) is not None and math.isfinite(float(item[field]))
    ]
    return mean(values) if values else None


def summarize_metric(metric: str, items: list[dict[str, Any]], target: dict[str, float]) -> dict[str, Any]:
    values = [metric_value(item) for item in items]
    values = [value for value in values if value is not None and math.isfinite(value)]
    passes = [metric_pass(item) for item in items]
    passes = [passed for passed in passes if passed is not None]
    summary: dict[str, Any] = {
        "metric": metric,
        "count": len(items),
        "scored_count": len(values),
        "threshold_passed": sum(1 for passed in passes if passed),
        "threshold_count": len(passes),
        "target": target,
    }
    summary["passed"] = summary["threshold_passed"]
    summary["pass_count"] = summary["threshold_count"]
    if values:
        summary["mean"] = mean(values)
        summary["min"] = min(values)
        summary["max"] = max(values)
    if passes:
        summary["threshold_pass_rate"] = summary["threshold_passed"] / len(passes)
        summary["pass_rate"] = summary["threshold_pass_rate"]
    if metric == "bpm" and items:
        octave_errors = sum(1 for item in items if is_bpm_octave_error(item))
        summary["octave_errors"] = octave_errors
        summary["octave_error_rate"] = octave_errors / len(items)
    if (metric == "downbeat" or str(metric).startswith("downbeat_")) and items:
        signatures = field_histogram(items, "time_signature")
        if signatures:
            summary["time_signature_counts"] = signatures
    if metric in (
        "chord_exact_wcsr",
        "chord_root_acc",
        "chord_quality_acc",
    ) and items:
        for field, output_key in (
            ("exact_wcsr", "mean_exact_wcsr"),
            ("root_accuracy", "mean_root_accuracy"),
            ("quality_accuracy", "mean_quality_accuracy"),
        ):
            value = finite_field_mean(items, field)
            if value is not None:
                summary[output_key] = value
    if metric == "chord_bass_acc" and items:
        summary["bass_threshold_counts"] = field_histogram(items, "threshold")
    if metric == "rt60" or str(metric).startswith("rt60_"):
        modes = field_histogram(items, "mode")
        if modes:
            summary["mode_counts"] = modes
    if metric in ("key", "key_mirex", "key_top3", "key_top5") and items:
        ranks = [int(item["expected_rank"]) for item in items if item.get("expected_rank")]
        if ranks:
            summary["top3_count"] = sum(1 for rank in ranks if rank <= 3)
            summary["top3_rate"] = summary["top3_count"] / len(items)
            summary["top5_count"] = sum(1 for rank in ranks if rank <= 5)
            summary["top5_rate"] = summary["top5_count"] / len(items)
        mirex_scores = [
            float(item["mirex_score"]) for item in items if item.get("mirex_score") is not None
        ]
        if mirex_scores:
            summary["mirex_weighted_score"] = mean(mirex_scores)
            categories: dict[str, int] = {}
            for item in items:
                category = item.get("mirex_category")
                if category:
                    categories[str(category)] = categories.get(str(category), 0) + 1
            summary["mirex_categories"] = dict(sorted(categories.items()))
        mode_confusion: dict[str, int] = {}
        root_interval_histogram: dict[str, int] = {}
        for item in items:
            expected_mode = item.get("expected_mode")
            measured_mode = item.get("measured_mode")
            if expected_mode is not None and measured_mode is not None:
                key = f"{expected_mode}->{measured_mode}"
                mode_confusion[key] = mode_confusion.get(key, 0) + 1
            expected_root = item.get("expected_root")
            measured_root = item.get("measured_root")
            if expected_root is not None and measured_root is not None:
                try:
                    interval = (int(measured_root) - int(expected_root)) % 12
                except (TypeError, ValueError):
                    continue
                interval_key = str(interval)
                root_interval_histogram[interval_key] = (
                    root_interval_histogram.get(interval_key, 0) + 1
                )
        if mode_confusion:
            summary["mode_confusion"] = dict(sorted(mode_confusion.items()))
        if root_interval_histogram:
            summary["root_interval_histogram"] = dict(
                sorted(root_interval_histogram.items(), key=lambda pair: int(pair[0]))
            )
        rank_histogram: dict[str, int] = {}
        for item in items:
            rank = item.get("expected_rank")
            if rank is not None:
                key = str(rank)
                rank_histogram[key] = rank_histogram.get(key, 0) + 1
        if rank_histogram:
            summary["expected_rank_histogram"] = dict(
                sorted(rank_histogram.items(), key=lambda pair: int(pair[0]))
            )
        gaps = [float(item["correlation_gap"]) for item in items if item.get("correlation_gap") is not None]
        if gaps:
            summary["mean_correlation_gap"] = mean(gaps)
            summary["max_correlation_gap"] = max(gaps)

    ok = True
    if "mean_min" in target:
        ok = bool(values) and summary["mean"] >= target["mean_min"]
    if "mean_max" in target:
        ok = bool(values) and summary["mean"] <= target["mean_max"]
    if "pass_rate_min" in target:
        ok = ok and bool(passes) and summary["pass_rate"] >= target["pass_rate_min"]
    if "octave_error_rate_max" in target:
        ok = (
            ok
            and bool(items)
            and summary.get("octave_error_rate", 1.0) <= target["octave_error_rate_max"]
        )
    if "min_count" in target:
        ok = ok and summary["scored_count"] >= int(target["min_count"])
    summary["ok"] = ok
    return summary


def select_targets(
    targets: dict[str, dict[str, float]], metrics: list[str] | None
) -> dict[str, dict[str, float]]:
    if not metrics:
        return targets
    unknown = [metric for metric in metrics if metric not in targets]
    if unknown:
        known = ", ".join(sorted(targets))
        raise SystemExit(f"unknown metric(s): {', '.join(unknown)}; known metrics: {known}")
    return {metric: targets[metric] for metric in metrics}


def evaluate(report: dict[str, Any], targets: dict[str, dict[str, float]], require_metrics: bool) -> dict[str, Any]:
    grouped: dict[str, list[dict[str, Any]]] = {metric: [] for metric in targets}
    for item in all_observations(report):
        metric = str(item.get("metric", ""))
        if metric in grouped:
            grouped[metric].append(item)
        if metric == "key":
            for derived in ("key_mirex", "key_top3", "key_top5"):
                if derived in grouped:
                    grouped[derived].append({**item, "metric": derived})
        if metric == "chord_detail":
            for derived in ("chord_exact_wcsr", "chord_root_acc", "chord_quality_acc"):
                if derived in grouped:
                    grouped[derived].append({**item, "metric": derived})
        if metric == "meter":
            expected_numerator = item.get("expected_numerator")
            expected_denominator = item.get("expected_denominator")
            if expected_numerator is not None and expected_denominator is not None:
                derived = f"meter_{expected_numerator}_{expected_denominator}"
                if derived in grouped:
                    grouped[derived].append({**item, "metric": derived})
        if metric == "downbeat":
            time_signature = item.get("time_signature")
            if time_signature:
                derived = "downbeat_" + str(time_signature).replace("/", "_")
                if derived in grouped:
                    grouped[derived].append(item)
        if metric == "rt60":
            mode = item.get("mode")
            if mode:
                derived = "rt60_" + str(mode)
                if derived in grouped:
                    grouped[derived].append(item)

    summaries = []
    for metric, target in targets.items():
        if grouped[metric] or require_metrics:
            summaries.append(summarize_metric(metric, grouped[metric], target))
    return {
        "ok": all(item["ok"] for item in summaries),
        "summaries": summaries,
        "require_metrics": require_metrics,
    }


def parse_targets(path: Path | None) -> dict[str, dict[str, float]]:
    if path is None:
        return DEFAULT_TARGETS
    data = json.loads(path.read_text(encoding="utf-8"))
    return {str(metric): {str(key): float(value) for key, value in target.items()} for metric, target in data.items()}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--targets", type=Path, help="JSON target override map.")
    parser.add_argument("--output", type=Path, help="Write JSON evaluation to this path.")
    parser.add_argument(
        "--require-metrics",
        action="store_true",
        help="Fail when a target metric has no observations.",
    )
    parser.add_argument(
        "--metrics",
        nargs="+",
        help="Evaluate only these target metrics, e.g. --metrics bpm key.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = json.loads(args.report.read_text(encoding="utf-8"))
    targets = select_targets(parse_targets(args.targets), args.metrics)
    result = evaluate(report, targets, args.require_metrics)
    if args.output:
        write_json_strict(args.output, result, indent=2, sort_keys=True)
    else:
        print(dumps_strict(result, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
