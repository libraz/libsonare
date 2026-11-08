"""Tests for reference-defined transition and interior scoring."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import score_key_regions as regions


def interval(start: float, end: float, root: int = 0, mode: str = "major") -> dict:
    return {"start": start, "end": end, "root": root, "mode": mode}


def reference(records: list[tuple[str, float, list[list[dict]]]]) -> dict:
    return {
        "format_version": "timed_keys_reference_v1",
        "dataset": "synthetic",
        "dataset_version": "1",
        "split_status": "synthetic_test",
        "records": [
            {
                "id": record_id,
                "duration_seconds": duration,
                "annotations": [
                    {"annotator": name, "sha256": f"{index:064x}", "intervals": labels}
                    for index, (name, labels) in enumerate(
                        zip(("ann1", "ann2", "ann3"), annotations, strict=True), start=1
                    )
                ],
            }
            for record_id, duration, annotations in records
        ],
    }


def prediction(records: list[tuple[str, list[dict]]]) -> dict:
    return {
        "method_id": "synthetic",
        "records": [{"id": record_id, "intervals": labels} for record_id, labels in records],
    }


def score(refs: dict, preds: dict) -> dict:
    return regions.score_regions(refs, preds, "a" * 64, "b" * 64)


class BoundaryRegionTests(unittest.TestCase):
    def test_one_change_has_analytic_four_second_masks_and_adds_to_full(self) -> None:
        labels = [interval(0, 5), interval(5, 10, 7)]
        report = score(
            reference([("song", 10, [labels, labels, labels])]),
            prediction([("song", [interval(0, 10)])]),
        )
        row = report["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(row["reference_change_times"], [5])
        self.assertEqual(row["boundary"]["scored_seconds"], 8)
        self.assertEqual(row["interior"]["scored_seconds"], 2)
        self.assertEqual(row["boundary"]["exact_seconds"], 4)
        self.assertEqual(row["interior"]["exact_seconds"], 1)
        for field in (
            "scored_seconds",
            "exact_seconds",
            "weighted_correct_seconds",
            "predicted_seconds_on_reference",
        ):
            self.assertAlmostEqual(
                row["boundary"][field] + row["interior"][field], row["full_reference"][field]
            )
        self.assertEqual(report["boundary_radius_seconds"], 4.0)

    def test_overlapping_change_bands_are_unioned_without_double_counting(self) -> None:
        labels = [interval(0, 5), interval(5, 9, 2), interval(9, 19, 7)]
        report = score(
            reference([("song", 19, [labels, labels, labels])]),
            prediction([("song", [interval(0, 19)])]),
        )
        row = report["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(row["reference_change_times"], [5, 9])
        self.assertEqual(row["boundary"]["scored_seconds"], 12)
        self.assertEqual(row["interior"]["scored_seconds"], 7)

    def test_touching_same_key_splits_do_not_create_boundaries(self) -> None:
        split = [interval(0, 2), interval(2, 5), interval(5, 10, 7)]
        joined = [interval(0, 5), interval(5, 10, 7)]
        reports = [
            score(
                reference([("song", 10, [labels, labels, labels])]),
                prediction([("song", [interval(0, 10)])]),
            )
            for labels in (split, joined)
        ]
        split_row = reports[0]["per_annotator"]["ann1"]["per_track"]["song"]
        joined_row = reports[1]["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(split_row["reference_change_times"], [5])
        self.assertEqual(split_row["boundary"], joined_row["boundary"])

    def test_annotation_gap_is_not_a_transition(self) -> None:
        labels = [interval(0, 2), interval(4, 5, 7)]
        report = score(
            reference([("song", 7, [labels, labels, labels])]),
            prediction([("song", [interval(0, 7)])]),
        )
        row = report["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(row["reference_change_times"], [])
        self.assertEqual(row["boundary"]["scored_seconds"], 0)
        self.assertFalse(row["boundary"]["measured"])
        self.assertEqual(row["interior"]["scored_seconds"], 3)

    def test_unanimous_masks_use_original_changes_and_do_not_bridge_agreement_gap(self) -> None:
        first = [interval(0, 5), interval(5, 10, 7)]
        with_gap = [interval(0, 3), interval(7, 10, 7)]
        report = score(
            reference([("song", 10, [first, with_gap, first])]),
            prediction([("song", [interval(0, 10)])]),
        )
        unanimous = report["unanimous_reference"]["per_track"]["song"]
        self.assertEqual(unanimous["reference_change_times"], [5])
        self.assertEqual(unanimous["full_reference"]["scored_seconds"], 6)
        self.assertEqual(unanimous["boundary"]["scored_seconds"], 4)
        self.assertEqual(unanimous["interior"]["scored_seconds"], 2)

    def test_prediction_gap_is_wrong_and_coverage_is_partitioned(self) -> None:
        labels = [interval(0, 5), interval(5, 10, 7)]
        report = score(
            reference([("song", 10, [labels, labels, labels])]),
            prediction([("song", [interval(0, 5), interval(6, 10, 7)])]),
        )
        row = report["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(row["boundary"]["scored_seconds"], 8)
        self.assertEqual(row["boundary"]["predicted_seconds_on_reference"], 7)
        self.assertEqual(row["boundary"]["missing_prediction_seconds"], 1)
        self.assertAlmostEqual(row["boundary"]["prediction_coverage_fraction"], 7 / 8)
        self.assertEqual(row["interior"]["scored_seconds"], 2)
        self.assertEqual(row["interior"]["predicted_seconds_on_reference"], 2)
        self.assertEqual(row["interior"]["missing_prediction_seconds"], 0)
        self.assertEqual(row["full_reference"]["predicted_seconds_on_reference"], 9)
        self.assertEqual(row["full_reference"]["missing_prediction_seconds"], 1)
        pooled = report["per_annotator"]["ann1"]["regions"]["boundary"]["pooled"]
        self.assertEqual(pooled["missing_prediction_seconds"], 1)

    def test_no_true_change_is_unmeasured_for_boundary_and_keeps_interior(self) -> None:
        labels = [interval(0, 4), interval(4, 10)]
        report = score(
            reference([("song", 10, [labels, labels, labels])]),
            prediction([("song", [interval(0, 10)])]),
        )
        row = report["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(row["boundary"]["scored_seconds"], 0)
        self.assertFalse(row["boundary"]["measured"])
        self.assertIsNone(row["boundary"]["exact_accuracy"])
        self.assertEqual(row["interior"]["scored_seconds"], 10)

    def test_invalid_prediction_uses_canonical_fail_closed_validation(self) -> None:
        labels = [interval(0, 10)]
        with self.assertRaisesRegex(ValueError, "overlap or unsorted"):
            score(
                reference([("song", 10, [labels, labels, labels])]),
                prediction([("song", [interval(0, 6), interval(5, 10)])]),
            )

    def test_masks_do_not_depend_on_prediction_method(self) -> None:
        labels = [interval(0, 5), interval(5, 10, 7)]
        refs = reference([("song", 10, [labels, labels, labels])])
        first = score(refs, prediction([("song", [interval(0, 10)])]))
        second = score(refs, prediction([("song", [interval(0, 5, 7), interval(5, 10)])]))
        for source in ("per_annotator", "unanimous_reference"):
            a = first[source]
            b = second[source]
            if source == "per_annotator":
                a = a["ann1"]["per_track"]["song"]
                b = b["ann1"]["per_track"]["song"]
            else:
                a = a["per_track"]["song"]
                b = b["per_track"]["song"]
            self.assertEqual(a["reference_change_times"], b["reference_change_times"])
            self.assertEqual(a["boundary_intervals"], b["boundary_intervals"])

    def test_cli_writes_a_versioned_report_with_input_hashes(self) -> None:
        labels = [interval(0, 5), interval(5, 10, 7)]
        refs = reference([("song", 10, [labels, labels, labels])])
        preds = prediction([("song", [interval(0, 10)])])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ref_path, pred_path, output_path = (
                root / "references.json",
                root / "predictions.json",
                root / "regions.json",
            )
            ref_path.write_text(json.dumps(refs), encoding="utf-8")
            pred_path.write_text(json.dumps(preds), encoding="utf-8")
            subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("score_key_regions.py")),
                    "--references",
                    str(ref_path),
                    "--predictions",
                    str(pred_path),
                    "--output",
                    str(output_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["report_version"], "key_regions_v1")
            self.assertEqual(
                report["reference_sha256"], hashlib.sha256(ref_path.read_bytes()).hexdigest()
            )
            self.assertEqual(
                report["predictions_sha256"], hashlib.sha256(pred_path.read_bytes()).hexdigest()
            )


if __name__ == "__main__":
    unittest.main()
