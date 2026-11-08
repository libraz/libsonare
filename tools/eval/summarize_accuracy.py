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
import math
import statistics
import sys
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any

_FIXTURE_HELPERS = Path(__file__).resolve().parents[2] / "tests" / "fixtures"
if str(_FIXTURE_HELPERS) not in sys.path:
    sys.path.insert(0, str(_FIXTURE_HELPERS))

# Standalone invocation needs the helper path above before importing it.
from observation_values import require_correct

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
        "accuracy": fraction(require_correct(item) for item in observations),
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
        "tracks_above_threshold": fraction(value >= EVENT_CORRECT_FMEASURE for value in f_measures),
        "threshold": EVENT_CORRECT_FMEASURE,
    }


def summarize_meter(observations: list[dict[str, Any]]) -> dict[str, Any]:
    return {"accuracy": fraction(require_correct(item) for item in observations)}


def summarize_chord(observations: list[dict[str, Any]]) -> dict[str, Any]:
    metric_fields = {
        "wcsr_maj_min": ("chord_wcsr", "wcsr"),
        "wcsr_exact": ("chord_exact_wcsr", "exact_wcsr"),
        "root_accuracy": ("chord_root_accuracy", "root_accuracy"),
        "quality_accuracy": ("chord_quality_accuracy", "quality_accuracy"),
        "bass_accuracy": ("chord_bass_acc", "accuracy"),
    }
    versions = {str(item.get("metric_version", "legacy_midpoint_v1")) for item in observations}
    if len(versions) > 1:
        raise ValueError(f"cannot aggregate mixed chord metric versions: {sorted(versions)}")
    version = next(iter(versions), "legacy_midpoint_v1")
    if version not in {"legacy_midpoint_v1", "chord_overlap_v2"}:
        raise ValueError(f"unsupported chord metric version: {version}")
    if version == "chord_overlap_v2":
        for item in observations:
            counters = (
                "correct_seconds",
                "scored_seconds",
                "annotated_seconds",
                "ignored_seconds",
                "missing_prediction_seconds",
            )
            try:
                values = {name: float(item[name]) for name in counters}
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(
                    "chord_overlap_v2 observation is missing valid duration fields"
                ) from error
            if any(not math.isfinite(value) or value < 0 for value in values.values()):
                raise ValueError("chord_overlap_v2 duration fields must be finite and nonnegative")
            correct = values["correct_seconds"]
            scored = values["scored_seconds"]
            annotated = values["annotated_seconds"]
            ignored = values["ignored_seconds"]
            missing = values["missing_prediction_seconds"]
            if correct > scored + 1e-9 or missing > scored + 1e-9:
                raise ValueError(
                    "chord_overlap_v2 correct/missing duration exceeds scored duration"
                )
            if not math.isclose(scored + ignored, annotated, rel_tol=1e-9, abs_tol=1e-9):
                raise ValueError(
                    "chord_overlap_v2 scored plus ignored duration must equal annotated duration"
                )
            score = item.get("score")
            if scored == 0 and score is not None:
                raise ValueError("chord_overlap_v2 unmeasured observation must have a null score")
            if scored > 0:
                if score is None or not math.isfinite(float(score)):
                    raise ValueError(
                        "chord_overlap_v2 measured observation must have a finite score"
                    )
                if not math.isclose(float(score), correct / scored, rel_tol=1e-9, abs_tol=1e-9):
                    raise ValueError("chord_overlap_v2 score does not match its seconds")

    result: dict[str, Any] = {"metric_version": version}
    if version == "chord_overlap_v2":
        result["root_scope"] = "known_chords"
        result["quality_scope"] = "known_chords"
        result["exact_scope"] = "known_chords"
        result["bass_scope"] = "explicit_known_bass"
    else:
        result["root_scope"] = "legacy_midpoint_v1"
        result["bass_scope"] = "legacy_midpoint_v1"

    coverage_by_metric: dict[str, Any] = {}
    attempted_fixtures = len({str(item.get("fixture", "")) for item in observations})
    all_scored_fixtures: set[str] = set()
    for output_name, (metric, legacy_field) in metric_fields.items():
        rows: list[dict[str, Any]] = []
        for item in observations:
            if version == "chord_overlap_v2":
                if item.get("metric") == metric and float(item.get("scored_seconds", 0.0)) > 0:
                    rows.append(item)
            elif (item.get("metric") == metric and item.get(legacy_field) is not None) or (
                metric != "chord_wcsr"
                and item.get("metric") == "chord_detail"
                and item.get(legacy_field) is not None
            ):
                rows.append(item)

        if version == "chord_overlap_v2":
            attempted_rows = [item for item in observations if item.get("metric") == metric]
            values = [float(item["score"]) for item in rows if item.get("score") is not None]
            correct_seconds = sum(float(item["correct_seconds"]) for item in rows)
            scored_seconds = sum(float(item["scored_seconds"]) for item in rows)
            pooled = correct_seconds / scored_seconds if scored_seconds > 0 else None
            result[f"{output_name}_pooled"] = pooled
            result[f"{output_name}_correct_seconds"] = correct_seconds
            result[f"{output_name}_scored_seconds"] = scored_seconds
            result[f"{output_name}_track_macro"] = mean(values)
            result[output_name] = mean(values)  # Keep the historical field as track macro.
            scored_fixtures = {str(item.get("fixture", "")) for item in rows}
            all_scored_fixtures.update(scored_fixtures)
            coverage_by_metric[output_name] = {
                "attempted_fixtures": len(
                    {
                        str(item.get("fixture", ""))
                        for item in observations
                        if item.get("metric") == metric
                    }
                ),
                "scored_fixtures": len(scored_fixtures),
                "scored_seconds": scored_seconds,
                "annotated_seconds": sum(
                    float(item["annotated_seconds"]) for item in attempted_rows
                ),
                "ignored_seconds": sum(float(item["ignored_seconds"]) for item in attempted_rows),
            }
        else:
            values = [float(item[legacy_field]) for item in rows]
            result[f"{output_name}_track_macro"] = mean(values)
            result[output_name] = mean(values)
            coverage_by_metric[output_name] = {
                "attempted_fixtures": len(
                    {
                        str(item.get("fixture", ""))
                        for item in observations
                        if item.get("metric") in (metric, "chord_detail")
                    }
                ),
                "scored_fixtures": len({str(item.get("fixture", "")) for item in rows}),
                "scored_seconds": None,
            }

    if version == "chord_overlap_v2":
        result["coverage"] = {
            "attempted_fixtures": attempted_fixtures,
            "scored_fixtures": len(all_scored_fixtures),
            "by_metric": coverage_by_metric,
        }
    return result


def summarize_structure(observations: list[dict[str, Any]]) -> dict[str, Any]:
    """Mean HR.5F, HR3F and pairwise F per `features` value.

    `both` is the primary reading and carries the unprefixed keys; any other
    feature set (timbre, harmony) is reported beside it under a prefix.
    """
    result: dict[str, Any] = {}
    for features in sorted({str(item["features"]) for item in observations}):
        subset = [item for item in observations if item["features"] == features]
        prefix = "" if features == "both" else f"{features}_"
        result[f"{prefix}mean_hr05f"] = mean(float(item["hr05f"]) for item in subset)
        result[f"{prefix}mean_hr3f"] = mean(float(item["hr3f"]) for item in subset)
        result[f"{prefix}mean_pairwise_f"] = mean(float(item["pairwise_f"]) for item in subset)
    result.setdefault("mean_hr3f", None)
    return result


def summarize_notes(observations: list[dict[str, Any]]) -> dict[str, Any]:
    """Mean note F-measures, and the predicted-to-annotated note count ratio.

    Above 1 the estimate has more notes than the annotation, which is over-splitting;
    below 1 it is merging or missing notes.
    """
    truth = sum(int(item["truth_count"]) for item in observations)
    predicted = sum(int(item["predicted_count"]) for item in observations)
    return {
        "mean_f_note": mean(float(item["f_note"]) for item in observations),
        "mean_f_note_offset": mean(float(item["f_note_offset"]) for item in observations),
        "mean_f_onset": mean(float(item["f_onset"]) for item in observations),
        "note_count_ratio": predicted / truth if truth else None,
    }


DIMENSIONS = (
    Dimension("key", ("key",), summarize_key, "accuracy"),
    Dimension("bpm", ("bpm",), summarize_bpm, "accuracy"),
    Dimension("beat", ("beat",), summarize_events, "mean_f_measure"),
    Dimension("downbeat", ("downbeat",), summarize_events, "mean_f_measure"),
    Dimension("meter", ("meter",), summarize_meter, "accuracy"),
    Dimension(
        "chord",
        (
            "chord_wcsr",
            "chord_exact_wcsr",
            "chord_root_accuracy",
            "chord_quality_accuracy",
            "chord_detail",  # Legacy midpoint report compatibility.
            "chord_bass_acc",
        ),
        summarize_chord,
        "wcsr_maj_min",
    ),
    Dimension("structure", ("structure",), summarize_structure, "mean_hr3f"),
    Dimension("notes", ("notes",), summarize_notes, "mean_f_note"),
)


def all_observations(report: dict[str, Any]) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    for run in report.get("runs", []):
        items.extend(run.get("observations", []))
    return items


def fixture_count(observations: list[dict[str, Any]]) -> int:
    return len({str(item.get("fixture", "")) for item in observations})


def chord_has_measurement(observations: list[dict[str, Any]]) -> bool:
    return any(
        float(item.get("scored_seconds", 0.0)) > 0 and item.get("score") is not None
        if item.get("metric_version") == "chord_overlap_v2"
        else any(
            item.get(field) is not None
            for field in ("wcsr", "root_accuracy", "quality_accuracy", "exact_wcsr", "accuracy")
        )
        for item in observations
    )


def summarize_dimension(dimension: Dimension, observations: list[dict[str, Any]]) -> dict[str, Any]:
    selected = [item for item in observations if item.get("metric") in dimension.metrics]
    if not selected:
        # No observations is not a score. Saying so is the whole point: a mean
        # over an empty set would read as a measurement that never happened.
        return {"measured": False, "fixtures": 0}

    if dimension.name == "chord":
        values = dimension.summarize(selected)
        measured = chord_has_measurement(selected)
        if not measured:
            return {
                "measured": False,
                "fixtures": fixture_count(selected),
                "coverage": values.get("coverage"),
            }
    else:
        values = dimension.summarize(selected)
        measured = True

    result: dict[str, Any] = {
        "measured": True,
        "fixtures": fixture_count(selected),
        "overall": values,
        "by_dataset": {},
    }
    datasets = sorted({dataset_of(str(item.get("fixture", ""))) for item in selected})
    for dataset in datasets:
        subset = [item for item in selected if dataset_of(str(item.get("fixture", ""))) == dataset]
        if dimension.name == "chord":
            dataset_values = dimension.summarize(subset)
            result["by_dataset"][dataset] = {
                "fixtures": fixture_count(subset),
                **(
                    {"measured": False, "coverage": dataset_values.get("coverage")}
                    if not chord_has_measurement(subset)
                    else {"measured": True, **dataset_values}
                ),
            }
        else:
            result["by_dataset"][dataset] = {
                "fixtures": fixture_count(subset),
                **dimension.summarize(subset),
            }
    return result


def summarize(report: dict[str, Any]) -> dict[str, Any]:
    observations = all_observations(report)
    dimensions = {
        dimension.name: summarize_dimension(dimension, observations) for dimension in DIMENSIONS
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


def format_value(value: Any, field_name: str | None = None) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, float):
        if field_name and field_name.endswith("_seconds"):
            return f"{value:.3f}s"
        if field_name and field_name.endswith("_ratio"):
            return f"{value:.3f}"
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
            count = entry["fixtures"]
            coverage = entry.get("coverage")
            if dimension.name == "chord" and coverage and coverage.get("attempted_fixtures", 0) > 0:
                primary = max(
                    coverage["by_metric"].values(), key=lambda item: item["attempted_fixtures"]
                )
                reason = (
                    "no scored chord intervals; "
                    f"attempted {primary['attempted_fixtures']}, "
                    f"scored {primary['scored_seconds']:.3f}s, "
                    f"annotated {primary['annotated_seconds']:.3f}s, "
                    f"ignored {primary['ignored_seconds']:.3f}s"
                )
            else:
                reason = "no fixtures configured"
            lines.append(f"| {dimension.name} | — | {count} | unmeasured | {reason} |")
            continue
        rows = [("all", entry["fixtures"], entry["overall"])]
        rows.extend(
            (dataset, values["fixtures"], values)
            for dataset, values in sorted(entry["by_dataset"].items())
        )
        for dataset, count, values in rows:
            headline_field = dimension.headline
            if dimension.name == "chord" and values.get("metric_version") == "chord_overlap_v2":
                headline_field = "wcsr_maj_min_pooled"
            hidden_fields = {headline_field, "fixtures", "mirex_categories"}
            if dimension.name == "chord" and values.get("metric_version") == "chord_overlap_v2":
                hidden_fields.add("wcsr_maj_min")
            headline = format_value(values.get(headline_field))
            detail = ", ".join(
                f"{key} {format_value(value, key)}"
                for key, value in values.items()
                if key not in hidden_fields and not isinstance(value, dict)
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
    parser.add_argument("--output", type=Path, help="Write the summary here instead of stdout.")
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
    try:
        summary = summarize(report)
    except ValueError as error:
        print(f"invalid accuracy report: {error}", file=sys.stderr)
        return 2

    missing = [
        name
        for name in args.require
        if name not in summary["dimensions"] or not summary["dimensions"][name]["measured"]
    ]

    text = to_markdown(summary) if args.markdown else json.dumps(summary, indent=2, sort_keys=True)
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
