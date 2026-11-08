"""Score key changes near annotation boundaries separately from stable intervals."""

from __future__ import annotations

import argparse
import itertools
import json
import math
from pathlib import Path
from typing import Any

import evaluate_timed_keys as timed

BOUNDARY_RADIUS_SECONDS = 4.0
REPORT_VERSION = "key_regions_v1"


def _key(interval: dict[str, Any]) -> tuple[int, str]:
    return interval["root"], interval["mode"]


def _union(intervals: list[tuple[float, float]]) -> list[tuple[float, float]]:
    merged: list[tuple[float, float]] = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def _coalesce_reference(
    intervals: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Join touching identical labels while leaving every annotation gap intact."""
    result: list[dict[str, Any]] = []
    for item in intervals:
        if result and result[-1]["end"] == item["start"] and _key(result[-1]) == _key(item):
            result[-1]["end"] = item["end"]
        else:
            result.append(dict(item))
    return result


def _true_changes(intervals: list[dict[str, Any]]) -> list[float]:
    coalesced = _coalesce_reference(intervals)
    return [
        left["end"]
        for left, right in itertools.pairwise(coalesced)
        if left["end"] == right["start"] and _key(left) != _key(right)
    ]


def _band_intervals(changes: list[float]) -> list[tuple[float, float]]:
    return _union(
        [
            (boundary - BOUNDARY_RADIUS_SECONDS, boundary + BOUNDARY_RADIUS_SECONDS)
            for boundary in changes
        ]
    )


def _intersect_reference(
    reference: list[dict[str, Any]], regions: list[tuple[float, float]]
) -> list[dict[str, Any]]:
    """Intersect a labelled timeline with a region union; gaps remain unscored."""
    result = []
    for interval in reference:
        for region_start, region_end in regions:
            if region_end <= interval["start"]:
                continue
            if region_start >= interval["end"]:
                break
            start = max(interval["start"], region_start)
            end = min(interval["end"], region_end)
            if end > start:
                result.append({**interval, "start": start, "end": end})
    return result


def _interior_reference(
    reference: list[dict[str, Any]], boundary_regions: list[tuple[float, float]]
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for interval in reference:
        cursor = interval["start"]
        for mask_start, mask_end in boundary_regions:
            if mask_end <= cursor:
                continue
            if mask_start >= interval["end"]:
                break
            if mask_start > cursor:
                end = min(mask_start, interval["end"])
                if end > cursor:
                    result.append({**interval, "start": cursor, "end": end})
            cursor = max(cursor, mask_end)
            if cursor >= interval["end"]:
                break
        if cursor < interval["end"]:
            result.append({**interval, "start": cursor, "end": interval["end"]})
    return result


def _close(left: float, right: float) -> bool:
    return math.isclose(left, right, rel_tol=1e-12, abs_tol=1e-10)


def _assert_additive(
    boundary: dict[str, Any], interior: dict[str, Any], full: dict[str, Any], where: str
) -> None:
    for field in (
        "scored_seconds",
        "exact_seconds",
        "weighted_correct_seconds",
        "predicted_seconds_on_reference",
    ):
        if not _close(boundary[field] + interior[field], full[field]):
            raise AssertionError(
                f"{where}: boundary + interior {field} does not equal full support"
            )
    if not _close(
        boundary["missing_prediction_seconds"] + interior["missing_prediction_seconds"],
        full["missing_prediction_seconds"],
    ):
        raise AssertionError(
            f"{where}: boundary + interior missing seconds do not equal full support"
        )


def _add_missing_prediction_seconds(score: dict[str, Any]) -> dict[str, Any]:
    missing = score["scored_seconds"] - score["predicted_seconds_on_reference"]
    score["missing_prediction_seconds"] = missing
    score["missing_prediction_fraction"] = (
        missing / score["scored_seconds"] if score["scored_seconds"] > 0 else None
    )
    return score


def _aggregate_with_missing_seconds(measurements: dict[str, dict[str, Any]]) -> dict[str, Any]:
    aggregate = timed._aggregate_scores(measurements)
    _add_missing_prediction_seconds(aggregate["pooled"])
    macro_coverage = aggregate["track_macro_prediction_coverage"]
    aggregate["track_macro_missing_prediction_fraction"] = (
        1 - macro_coverage if macro_coverage is not None else None
    )
    return aggregate


def _score_population(
    references: dict[str, list[dict[str, Any]]],
    predictions: dict[str, list[dict[str, Any]]],
) -> dict[str, Any]:
    """Score one reference population using changes from that population's source labels."""
    per_track: dict[str, Any] = {}
    for record_id, reference in references.items():
        changes = _true_changes(reference)
        mask = _band_intervals(changes)
        boundary_reference = _intersect_reference(reference, mask)
        interior_reference = _interior_reference(reference, mask)
        prediction = predictions[record_id]
        boundary = _add_missing_prediction_seconds(
            timed._record_score(boundary_reference, prediction)
        )
        interior = _add_missing_prediction_seconds(
            timed._record_score(interior_reference, prediction)
        )
        full = _add_missing_prediction_seconds(timed._record_score(reference, prediction))
        _assert_additive(boundary, interior, full, record_id)
        per_track[record_id] = {
            "boundary": boundary,
            "interior": interior,
            "full_reference": full,
            "boundary_intervals": [
                {"start": start, "end": end}
                for start, end in _union(
                    [(item["start"], item["end"]) for item in boundary_reference]
                )
            ],
            "interior_intervals": [
                {"start": item["start"], "end": item["end"]} for item in interior_reference
            ],
            "reference_change_times": sorted(set(changes)),
        }
    return per_track


def _summarize_population(per_track: dict[str, Any]) -> dict[str, Any]:
    regions = {}
    for name in ("boundary", "interior", "full_reference"):
        measurements = {record_id: value[name] for record_id, value in per_track.items()}
        regions[name] = _aggregate_with_missing_seconds(measurements)
    return {"regions": regions, "per_track": per_track}


def _validate_inputs(
    references: dict[str, Any],
    predictions: dict[str, Any],
    reference_sha256: str,
    predictions_sha256: str | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    # Reuse the canonical evaluator as the fail-closed schema and coverage validator.
    base_report = timed.evaluate(references, predictions, reference_sha256, predictions_sha256)
    reference_records = references["records"]
    predictions_by_id = {item["id"]: item["intervals"] for item in predictions["records"]}
    records = {}
    for record in reference_records:
        record_id = record["id"]
        duration = float(record["duration_seconds"])
        annotations = {
            item["annotator"]: timed._validate_intervals(
                item["intervals"], duration, f"{record_id}/{item['annotator']}"
            )
            for item in record["annotations"]
        }
        prediction = timed._validate_intervals(
            predictions_by_id[record_id], duration, f"prediction/{record_id}"
        )
        records[record_id] = {"annotations": annotations, "prediction": prediction}
    return base_report, records


def score_regions(
    references: dict[str, Any],
    predictions: dict[str, Any],
    reference_sha256: str,
    predictions_sha256: str | None = None,
) -> dict[str, Any]:
    base, records = _validate_inputs(references, predictions, reference_sha256, predictions_sha256)
    ids = list(records)
    prediction_by_id = {record_id: records[record_id]["prediction"] for record_id in ids}
    per_annotator = {}
    for annotator in timed.ANNOTATORS:
        refs = {record_id: records[record_id]["annotations"][annotator] for record_id in ids}
        per_annotator[annotator] = _summarize_population(_score_population(refs, prediction_by_id))

    unanimous_refs = {
        record_id: timed._unanimous_intervals(
            [records[record_id]["annotations"][name] for name in timed.ANNOTATORS]
        )
        for record_id in ids
    }
    original_sources_by_id = {
        record_id: [records[record_id]["annotations"][name] for name in timed.ANNOTATORS]
        for record_id in ids
    }
    # Each recording has its own source-boundary list; pass it independently to
    # the mask builder so boundaries from another recording cannot enter the mask.
    unanimous_per_track = {}
    for record_id, reference in unanimous_refs.items():
        sources = original_sources_by_id[record_id]
        changes = [change for source in sources for change in _true_changes(source)]
        mask = _band_intervals(changes)
        boundary_reference = _intersect_reference(reference, mask)
        interior_reference = _interior_reference(reference, mask)
        prediction = prediction_by_id[record_id]
        boundary = _add_missing_prediction_seconds(
            timed._record_score(boundary_reference, prediction)
        )
        interior = _add_missing_prediction_seconds(
            timed._record_score(interior_reference, prediction)
        )
        full = _add_missing_prediction_seconds(timed._record_score(reference, prediction))
        _assert_additive(boundary, interior, full, f"unanimous/{record_id}")
        unanimous_per_track[record_id] = {
            "boundary": boundary,
            "interior": interior,
            "full_reference": full,
            "boundary_intervals": [
                {"start": start, "end": end}
                for start, end in _union(
                    [(item["start"], item["end"]) for item in boundary_reference]
                )
            ],
            "interior_intervals": [
                {"start": item["start"], "end": item["end"]} for item in interior_reference
            ],
            "reference_change_times": sorted(set(changes)),
        }
    return {
        "report_version": REPORT_VERSION,
        "boundary_radius_seconds": BOUNDARY_RADIUS_SECONDS,
        "boundary_radius_basis": "half of previous 8-second analysis cell",
        "boundary_rule": (
            "Touching annotated intervals with different key labels; annotations gaps and endpoints "
            "are never boundaries. The union of ±4s bands is intersected with reference support."
        ),
        "unanimous_boundary_rule": (
            "Use the union of true changes from all three original analysts, intersected with "
            "unanimous support. Gaps in unanimous support are not changes."
        ),
        "evaluator_version": base["evaluator_version"],
        "reference_format_version": base["reference_format_version"],
        "method_id": base["method_id"],
        "dataset": base["dataset"],
        "dataset_version": base["dataset_version"],
        "split_status": base["split_status"],
        "reference_sha256": base["reference_sha256"],
        "predictions_sha256": base["predictions_sha256"],
        "record_count": base["record_count"],
        "per_annotator": per_annotator,
        "unanimous_reference": _summarize_population(unanimous_per_track),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", required=True, type=Path)
    parser.add_argument("--predictions", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    references, reference_hash = timed._load_json(args.references)
    predictions, predictions_hash = timed._load_json(args.predictions)
    report = score_regions(references, predictions, reference_hash, predictions_hash)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"scored boundary/interior regions for {report['record_count']} tracks: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
