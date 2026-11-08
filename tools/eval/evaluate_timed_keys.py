#!/usr/bin/env python3
"""Score timed key predictions against all supplied local-key analysts."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
from typing import Any

from prepare_timed_keys import ANNOTATORS, FORMAT_VERSION

EVALUATOR_VERSION = "timed_key_overlap_v1"


def _load_json(path: Path) -> tuple[Any, str]:
    raw = path.read_bytes()
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise ValueError(f"{path}: invalid JSON: {error}") from error
    return value, hashlib.sha256(raw).hexdigest()


def _validate_intervals(intervals: Any, duration: float, where: str) -> list[dict[str, Any]]:
    if not isinstance(intervals, list):
        raise TypeError(f"{where}: intervals must be a list")
    result = []
    previous_end = 0.0
    for index, item in enumerate(intervals):
        label = f"{where}[{index}]"
        if not isinstance(item, dict):
            raise TypeError(f"{label}: expected object")
        try:
            start, end = float(item["start"]), float(item["end"])
            if not math.isfinite(start) or not math.isfinite(end):
                raise ValueError("nonfinite time")
            if start < 0 or end <= start or end > duration:
                raise ValueError(f"invalid interval [{start}, {end}) for duration {duration}")
            if start < previous_end:
                raise ValueError(f"overlap or unsorted start before {previous_end}")
            raw_root = item["root"]
            if type(raw_root) is not int or raw_root not in range(12):
                raise ValueError(f"root must be an integer from 0 through 11, got {raw_root!r}")
            root = raw_root
            mode = item["mode"]
            if type(mode) is not str or mode not in ("major", "minor"):
                raise ValueError(f"invalid root/mode {root}/{mode!r}")
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            raise ValueError(f"{label}: {error}") from error
        normalized = {"start": start, "end": end, "root": root, "mode": mode}
        result.append(normalized)
        previous_end = end
    return result


def weighted_key_score(expected: dict[str, Any], measured: dict[str, Any]) -> tuple[float, str]:
    expected_root, measured_root = expected["root"], measured["root"]
    expected_mode, measured_mode = expected["mode"], measured["mode"]
    if expected_root == measured_root and expected_mode == measured_mode:
        return 1.0, "exact"
    interval = (measured_root - expected_root) % 12
    if expected_mode == measured_mode and interval in (5, 7):
        return 0.5, "fifth"
    if expected_mode != measured_mode:
        relative_root = (expected_root + (9 if expected_mode == "major" else 3)) % 12
        if measured_root == relative_root:
            return 0.3, "relative"
        if measured_root == expected_root:
            return 0.2, "parallel"
    return 0.0, "other"


def _boundaries(*collections: list[dict[str, Any]]) -> list[float]:
    return sorted(
        {
            point
            for intervals in collections
            for item in intervals
            for point in (item["start"], item["end"])
        }
    )


def _at(intervals: list[dict[str, Any]], midpoint: float) -> dict[str, Any] | None:
    for item in intervals:
        if item["start"] <= midpoint < item["end"]:
            return item
        if item["start"] > midpoint:
            break
    return None


def _record_score(
    reference: list[dict[str, Any]], prediction: list[dict[str, Any]]
) -> dict[str, Any]:
    points = _boundaries(reference, prediction)
    scored = exact = weighted = predicted = 0.0
    categories = {"exact": 0.0, "fifth": 0.0, "relative": 0.0, "parallel": 0.0, "other": 0.0}
    for start, end in itertools.pairwise(points):
        duration = end - start
        if duration <= 0:
            continue
        midpoint = start + duration / 2
        expected = _at(reference, midpoint)
        if expected is None:
            continue
        scored += duration
        measured = _at(prediction, midpoint)
        if measured is None:
            categories["other"] += duration
            continue
        predicted += duration
        score, category = weighted_key_score(expected, measured)
        categories[category] += duration
        weighted += duration * score
        exact += duration * (category == "exact")
    return {
        "scored_seconds": scored,
        "predicted_seconds_on_reference": predicted,
        "prediction_coverage_fraction": predicted / scored if scored > 0 else None,
        "exact_seconds": exact,
        "weighted_correct_seconds": weighted,
        "score": weighted / scored if scored > 0 else None,
        "exact_accuracy": exact / scored if scored > 0 else None,
        "measured": scored > 0,
        "categories_seconds": categories,
    }


def _unanimous_intervals(annotations: list[list[dict[str, Any]]]) -> list[dict[str, Any]]:
    """Return intervals where all analysts cover time with the same key."""
    points = _boundaries(*annotations)
    agreed: list[dict[str, Any]] = []
    for start, end in itertools.pairwise(points):
        if end <= start:
            continue
        labels = [_at(intervals, start + (end - start) / 2) for intervals in annotations]
        if any(label is None for label in labels):
            continue
        first = labels[0]
        if any(
            (label["root"], label["mode"]) != (first["root"], first["mode"]) for label in labels[1:]
        ):
            continue
        if (
            agreed
            and agreed[-1]["end"] == start
            and (agreed[-1]["root"], agreed[-1]["mode"]) == (first["root"], first["mode"])
        ):
            agreed[-1]["end"] = end
        else:
            agreed.append(
                {"start": start, "end": end, "root": first["root"], "mode": first["mode"]}
            )
    return agreed


def _aggregate_scores(per_track: dict[str, dict[str, Any]]) -> dict[str, Any]:
    measured = [entry for entry in per_track.values() if entry["measured"]]
    scored = sum(entry["scored_seconds"] for entry in measured)
    weighted = sum(entry["weighted_correct_seconds"] for entry in measured)
    exact = sum(entry["exact_seconds"] for entry in measured)
    predicted = sum(entry["predicted_seconds_on_reference"] for entry in measured)
    return {
        "pooled": {
            "measured_tracks": len(measured),
            "scored_seconds": scored,
            "weighted_correct_seconds": weighted,
            "weighted_score": weighted / scored if scored else None,
            "predicted_seconds_on_reference": predicted,
            "prediction_coverage_fraction": predicted / scored if scored else None,
            "exact_seconds": exact,
            "exact_accuracy": exact / scored if scored else None,
            "measured": scored > 0,
        },
        "track_macro_weighted_score": (
            sum(entry["score"] for entry in measured) / len(measured) if measured else None
        ),
        "track_macro_exact_accuracy": (
            sum(entry["exact_accuracy"] for entry in measured) / len(measured) if measured else None
        ),
        "track_macro_prediction_coverage": (
            sum(entry["prediction_coverage_fraction"] for entry in measured) / len(measured)
            if measured
            else None
        ),
        "per_track": per_track,
    }


def _annotator_disagreement(annotations: list[list[dict[str, Any]]]) -> dict[str, float | None]:
    points = _boundaries(*annotations)
    common = disagree = 0.0
    for start, end in itertools.pairwise(points):
        duration = end - start
        if duration <= 0:
            continue
        labels = [_at(intervals, start + duration / 2) for intervals in annotations]
        if any(label is None for label in labels):
            continue
        common += duration
        identities = {(label["root"], label["mode"]) for label in labels if label is not None}
        if len(identities) > 1:
            disagree += duration
    return {
        "common_annotated_seconds": common,
        "disagreement_seconds": disagree,
        "disagreement_fraction": disagree / common if common else None,
    }


def evaluate(
    references: dict[str, Any],
    predictions: dict[str, Any],
    reference_sha256: str,
    predictions_sha256: str | None = None,
) -> dict[str, Any]:
    if references.get("format_version") != FORMAT_VERSION:
        raise ValueError(f"unsupported reference format {references.get('format_version')!r}")
    if not isinstance(predictions.get("method_id"), str) or not predictions["method_id"].strip():
        raise ValueError("predictions.method_id must be a nonempty string")
    reference_records = references.get("records")
    prediction_records = predictions.get("records")
    if not isinstance(reference_records, list) or not isinstance(prediction_records, list):
        raise TypeError("references and predictions must each contain records arrays")
    refs: dict[str, dict[str, Any]] = {}
    for record in reference_records:
        record_id = record.get("id")
        if not isinstance(record_id, str) or record_id in refs:
            raise ValueError(f"missing or duplicate reference id {record_id!r}")
        duration = float(record["duration_seconds"])
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError(f"{record_id}: invalid duration")
        annotations = record.get("annotations", [])
        by_annotator = {annotation.get("annotator"): annotation for annotation in annotations}
        if len(annotations) != len(ANNOTATORS) or set(by_annotator) != set(ANNOTATORS):
            raise ValueError(f"{record_id}: expected exactly annotators {ANNOTATORS}")
        normalized = {}
        for annotator in ANNOTATORS:
            annotation = by_annotator[annotator]
            digest = annotation.get("sha256")
            if (
                not isinstance(digest, str)
                or len(digest) != 64
                or any(c not in "0123456789abcdef" for c in digest)
            ):
                raise ValueError(f"{record_id}/{annotator}: missing valid annotation SHA-256")
            normalized[annotator] = _validate_intervals(
                annotation.get("intervals"), duration, f"{record_id}/{annotator}"
            )
        refs[record_id] = {
            **record,
            "duration_seconds": duration,
            "validated_annotations": normalized,
        }
    preds: dict[str, list[dict[str, Any]]] = {}
    for record in prediction_records:
        record_id = record.get("id")
        if not isinstance(record_id, str) or record_id in preds:
            raise ValueError(f"missing or duplicate prediction id {record_id!r}")
        if record_id not in refs:
            raise ValueError(f"unexpected prediction id {record_id!r}")
        preds[record_id] = _validate_intervals(
            record.get("intervals"), refs[record_id]["duration_seconds"], f"prediction/{record_id}"
        )
    if set(preds) != set(refs):
        raise ValueError(
            f"prediction IDs differ from references; missing={sorted(set(refs) - set(preds))}"
        )

    per_annotator: dict[str, dict[str, Any]] = {}
    for annotator in ANNOTATORS:
        per_track = {}
        for record_id, record in refs.items():
            per_track[record_id] = _record_score(
                record["validated_annotations"][annotator], preds[record_id]
            )
        per_annotator[annotator] = _aggregate_scores(per_track)
    unanimous_per_track = {
        record_id: _record_score(
            _unanimous_intervals(
                [record["validated_annotations"][annotator] for annotator in ANNOTATORS]
            ),
            preds[record_id],
        )
        for record_id, record in refs.items()
    }
    unanimous = _aggregate_scores(unanimous_per_track)
    disagreements = {
        record_id: _annotator_disagreement(
            [record["validated_annotations"][annotator] for annotator in ANNOTATORS]
        )
        for record_id, record in refs.items()
    }
    total_common = sum(value["common_annotated_seconds"] for value in disagreements.values())
    total_disagree = sum(value["disagreement_seconds"] for value in disagreements.values())
    return {
        "evaluator_version": EVALUATOR_VERSION,
        "reference_format_version": FORMAT_VERSION,
        "method_id": predictions["method_id"],
        "dataset": references.get("dataset"),
        "dataset_version": references.get("dataset_version"),
        "split_status": references.get("split_status"),
        "reference_sha256": reference_sha256,
        "predictions_sha256": predictions_sha256,
        "record_count": len(refs),
        "metric": "MIREX2017+ duration-weighted key score; exact=1, fifth=0.5, relative=0.3, parallel=0.2",
        "per_annotator": per_annotator,
        "unanimous_reference": unanimous,
        "annotator_disagreement": {
            "per_track": disagreements,
            "pooled_common_annotated_seconds": total_common,
            "pooled_disagreement_seconds": total_disagree,
            "pooled_disagreement_fraction": total_disagree / total_common if total_common else None,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--predictions", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    references, reference_hash = _load_json(args.references)
    predictions, predictions_hash = _load_json(args.predictions)
    report = evaluate(references, predictions, reference_hash, predictions_hash)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"evaluated {report['record_count']} tracks for {report['method_id']}: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
