#!/usr/bin/env python3
"""Roll per-fixture observations up into publishable accuracy figures.

Input is the JSON report `tests/fixtures/run_optional_fixture_report.py` writes:
one observation per fixture per metric. This turns that into the aggregate
numbers a docs page can carry -- key accuracy, chord WCSR, beat F-measure and
the rest -- broken down by dataset, because a mean over a corpus nobody can see
is not a measurement anyone can check.

Two rules the output enforces, both about not reporting more than was measured:

- A dimension with no observations is reported as `unmeasured`, never as a
  score. An aggregate over an empty set is not 0% and not 100%; it is nothing.
- Every figure carries the fixture count and the dataset it came from. A single
  number without those is not a result.

Only `report_only` manifest rows produce observations: a gating row asserts and
prints nothing. Measurement runs therefore mark their rows `report_only` and
leave the gate rows to CI. See tools/eval/README.md.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path
from typing import Any, Callable, Iterable


# Relative BPM error counted as correct, the MIREX Acc1 convention.
BPM_ACCURACY_TOLERANCE = 0.04

# F-measure at or above which a beat / downbeat track counts as correct. The
# mean F-measure is the primary figure; this is the secondary "how many tracks
# did it get" reading, and 0.5 is only a convention.
EVENT_CORRECT_FMEASURE = 0.5


def mean(values: Iterable[float]) -> float | None:
    data = list(values)
    return statistics.fmean(data) if data else None


def fraction(values: Iterable[bool]) -> float | None:
    data = list(values)
    return sum(1 for value in data if value) / len(data) if data else None


def dataset_of(fixture: str) -> str:
    """The dataset a fixture name belongs to.

    Observations name a fixture as `<dataset>/<path>`, which is how the manifest
    row's first two columns reach the report.
    """
    head, separator, _ = fixture.partition("/")
    return head if separator else "(unnamed)"


class Dimension:
    """One accuracy dimension and how its observations aggregate."""

    def __init__(
        self,
        name: str,
        metrics: tuple[str, ...],
        summarize: Callable[[list[dict[str, Any]]], dict[str, Any]],
        headline: str,
    ) -> None:
        self.name = name
        self.metrics = metrics
        self.summarize = summarize
        self.headline = headline


def summarize_key(observations: list[dict[str, Any]]) -> dict[str, Any]:
    categories: dict[str, int] = {}
    for item in observations:
        category = str(item.get("mirex_category", "other"))
        categories[category] = categories.get(category, 0) + 1
    return {
        "accuracy": fraction(bool(item.get("correct")) for item in observations),
        "mirex_score": mean(
            float(item["mirex_score"])
            for item in observations
            if item.get("mirex_score") is not None
        ),
        "mirex_categories": categories,
    }


def summarize_bpm(observations: list[dict[str, Any]]) -> dict[str, Any]:
    errors = [float(item["relative_error_percent"]) / 100.0 for item in observations]
    return {
        "accuracy": fraction(error <= BPM_ACCURACY_TOLERANCE for error in errors),
        "accuracy_tolerance": BPM_ACCURACY_TOLERANCE,
        "median_relative_error": statistics.median(errors) if errors else None,
    }


def summarize_events(observations: list[dict[str, Any]]) -> dict[str, Any]:
    f_measures = [float(item["f_measure"]) for item in observations]
    return {
        "mean_f_measure": mean(f_measures),
        "tracks_above_threshold": fraction(
            value >= EVENT_CORRECT_FMEASURE for value in f_measures
        ),
        "threshold": EVENT_CORRECT_FMEASURE,
    }


def summarize_meter(observations: list[dict[str, Any]]) -> dict[str, Any]:
    return {"accuracy": fraction(bool(item.get("correct")) for item in observations)}


def summarize_chord(observations: list[dict[str, Any]]) -> dict[str, Any]:
    def collect(metric: str, field: str) -> list[float]:
        return [
            float(item[field])
            for item in observations
            if item.get("metric") == metric and item.get(field) is not None
        ]

    return {
        "wcsr_maj_min": mean(collect("chord_wcsr", "wcsr")),
        "wcsr_exact": mean(collect("chord_detail", "exact_wcsr")),
        "root_accuracy": mean(collect("chord_detail", "root_accuracy")),
        "quality_accuracy": mean(collect("chord_detail", "quality_accuracy")),
        "bass_accuracy": mean(collect("chord_bass_acc", "accuracy")),
    }


DIMENSIONS = (
    Dimension("key", ("key",), summarize_key, "accuracy"),
    Dimension("bpm", ("bpm",), summarize_bpm, "accuracy"),
    Dimension("beat", ("beat",), summarize_events, "mean_f_measure"),
    Dimension("downbeat", ("downbeat",), summarize_events, "mean_f_measure"),
    Dimension("meter", ("meter",), summarize_meter, "accuracy"),
    Dimension(
        "chord",
        ("chord_wcsr", "chord_detail", "chord_bass_acc"),
        summarize_chord,
        "wcsr_maj_min",
    ),
)


def all_observations(report: dict[str, Any]) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    for run in report.get("runs", []):
        items.extend(run.get("observations", []))
    return items


def fixture_count(observations: list[dict[str, Any]]) -> int:
    return len({str(item.get("fixture", "")) for item in observations})


def summarize_dimension(
    dimension: Dimension, observations: list[dict[str, Any]]
) -> dict[str, Any]:
    selected = [
        item for item in observations if item.get("metric") in dimension.metrics
    ]
    if not selected:
        # No observations is not a score. Saying so is the whole point: a mean
        # over an empty set would read as a measurement that never happened.
        return {"measured": False, "fixtures": 0}

    result: dict[str, Any] = {
        "measured": True,
        "fixtures": fixture_count(selected),
        "overall": dimension.summarize(selected),
        "by_dataset": {},
    }
    datasets = sorted({dataset_of(str(item.get("fixture", ""))) for item in selected})
    for dataset in datasets:
        subset = [
            item
            for item in selected
            if dataset_of(str(item.get("fixture", ""))) == dataset
        ]
        result["by_dataset"][dataset] = {
            "fixtures": fixture_count(subset),
            **dimension.summarize(subset),
        }
    return result


def summarize(report: dict[str, Any]) -> dict[str, Any]:
    observations = all_observations(report)
    dimensions = {
        dimension.name: summarize_dimension(dimension, observations)
        for dimension in DIMENSIONS
    }
    return {
        "generated_at": report.get("generated_at"),
        "measured_dimensions": sorted(
            name for name, value in dimensions.items() if value["measured"]
        ),
        "unmeasured_dimensions": sorted(
            name for name, value in dimensions.items() if not value["measured"]
        ),
        "dimensions": dimensions,
    }


def format_value(value: Any) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, float):
        return f"{value * 100.0:.1f}%"
    return str(value)


def to_markdown(summary: dict[str, Any]) -> str:
    lines = [
        "| dimension | dataset | fixtures | headline | detail |",
        "| --- | --- | --- | --- | --- |",
    ]
    for dimension in DIMENSIONS:
        entry = summary["dimensions"][dimension.name]
        if not entry["measured"]:
            lines.append(
                f"| {dimension.name} | — | 0 | unmeasured | no fixtures configured |"
            )
            continue
        rows = [("all", entry["fixtures"], entry["overall"])]
        rows.extend(
            (dataset, values["fixtures"], values)
            for dataset, values in sorted(entry["by_dataset"].items())
        )
        for dataset, count, values in rows:
            headline = format_value(values.get(dimension.headline))
            detail = ", ".join(
                f"{key} {format_value(value)}"
                for key, value in values.items()
                if key not in {dimension.headline, "fixtures", "mirex_categories"}
                and not isinstance(value, dict)
            )
            lines.append(
                f"| {dimension.name} | {dataset} | {count} | {headline} | {detail or '—'} |"
            )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "report",
        type=Path,
        help="JSON report written by tests/fixtures/run_optional_fixture_report.py",
    )
    parser.add_argument(
        "--output", type=Path, help="Write the summary here instead of stdout."
    )
    parser.add_argument(
        "--markdown",
        action="store_true",
        help="Emit a markdown table instead of JSON.",
    )
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="DIMENSION",
        help=(
            "Fail when this dimension has no observations. Repeatable. Use it in a "
            "publishing pipeline so an empty corpus cannot quietly ship a page of "
            "'unmeasured' rows as if they were results."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = json.loads(args.report.read_text())
    summary = summarize(report)

    missing = [
        name
        for name in args.require
        if name not in summary["dimensions"]
        or not summary["dimensions"][name]["measured"]
    ]

    text = (
        to_markdown(summary)
        if args.markdown
        else json.dumps(summary, indent=2, sort_keys=True)
    )
    if args.output:
        args.output.write_text(text + "\n")
    else:
        print(text)

    if missing:
        print(
            f"unmeasured dimensions required by --require: {', '.join(missing)}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
