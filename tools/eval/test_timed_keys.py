"""Regression tests for interval-integrated timed-key evaluation."""

from __future__ import annotations

import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evaluate_timed_keys import evaluate, weighted_key_score
from prepare_timed_keys import parse_key, prepare, read_annotation


def ref_interval(start: float, end: float, root: int = 0, mode: str = "major"):
    return {"start": start, "end": end, "root": root, "mode": mode}


def ref_doc(tracks):
    return {
        "format_version": "timed_keys_reference_v1",
        "dataset": "test",
        "dataset_version": "1",
        "split_status": "test only",
        "records": [
            {
                "id": track_id,
                "duration_seconds": duration,
                "annotations": [
                    {"annotator": annotator, "sha256": f"{index:064x}", "intervals": intervals}
                    for index, (annotator, intervals) in enumerate(
                        zip(("ann1", "ann2", "ann3"), annotations), 1
                    )
                ],
            }
            for track_id, duration, annotations in tracks
        ],
    }


def pred_doc(tracks):
    return {
        "method_id": "test-method",
        "records": [{"id": track_id, "intervals": intervals} for track_id, intervals in tracks],
    }


class TimedKeyEvaluationTests(unittest.TestCase):
    def evaluate(self, references, predictions):
        return evaluate(references, predictions, "a" * 64, "b" * 64)

    def test_short_correct_is_duration_weighted_not_midpoint_sampled(self):
        annotations = [[ref_interval(0, 10)]] * 3
        report = self.evaluate(
            ref_doc([("song", 10, annotations)]),
            pred_doc([("song", [ref_interval(4.9, 5.1)])]),
        )
        score = report["per_annotator"]["ann1"]["pooled"]["weighted_score"]
        self.assertAlmostEqual(score, 0.02)

    def test_splitting_identical_prediction_does_not_change_score(self):
        annotations = [[ref_interval(0, 10)]] * 3
        references = ref_doc([("song", 10, annotations)])
        whole = self.evaluate(
            references, pred_doc([("song", [ref_interval(0, 4, 0), ref_interval(4, 10, 7)])])
        )
        split = self.evaluate(
            references,
            pred_doc(
                [
                    (
                        "song",
                        [
                            ref_interval(0, 2, 0),
                            ref_interval(2, 4, 0),
                            ref_interval(4, 8, 7),
                            ref_interval(8, 10, 7),
                        ],
                    )
                ]
            ),
        )
        self.assertAlmostEqual(
            whole["per_annotator"]["ann1"]["pooled"]["weighted_score"],
            split["per_annotator"]["ann1"]["pooled"]["weighted_score"],
        )

    def test_reference_and_prediction_gaps_are_handled_separately(self):
        ann = [ref_interval(2, 8)]
        report = self.evaluate(
            ref_doc([("song", 10, [ann, ann, ann])]),
            pred_doc([("song", [ref_interval(0, 4), ref_interval(6, 10, 7)])]),
        )
        item = report["per_annotator"]["ann1"]["per_track"]["song"]
        self.assertEqual(item["scored_seconds"], 6)
        self.assertEqual(item["predicted_seconds_on_reference"], 4)
        self.assertAlmostEqual(item["prediction_coverage_fraction"], 4 / 6)
        self.assertAlmostEqual(item["score"], 0.5)

    def test_three_analysts_are_scored_independently_and_disagreement_is_reported(self):
        annotations = [
            [ref_interval(0, 10, 0, "major")],
            [ref_interval(0, 10, 0, "minor")],
            [ref_interval(0, 10, 7, "major")],
        ]
        report = self.evaluate(
            ref_doc([("song", 10, annotations)]), pred_doc([("song", [ref_interval(0, 10)])])
        )
        scores = [
            report["per_annotator"][name]["pooled"]["weighted_score"]
            for name in ("ann1", "ann2", "ann3")
        ]
        self.assertEqual(scores, [1.0, 0.2, 0.5])
        self.assertEqual(report["annotator_disagreement"]["pooled_disagreement_fraction"], 1.0)

    def test_pooled_duration_score_and_track_macro_are_both_visible(self):
        perfect = [ref_interval(0, 10)]
        short_truth = ref_interval(0, 1)
        references = ref_doc(
            [
                ("long", 10, [[perfect[0]], [perfect[0]], [perfect[0]]]),
                ("short", 1, [[short_truth], [short_truth], [short_truth]]),
            ]
        )
        predictions = pred_doc([("long", perfect), ("short", [ref_interval(0, 1, 1)])])
        report = self.evaluate(references, predictions)["per_annotator"]["ann1"]
        self.assertAlmostEqual(report["pooled"]["weighted_score"], 10 / 11)
        self.assertAlmostEqual(report["track_macro_weighted_score"], 0.5)

    def test_enharmonic_parse_supports_transposed_performance_label(self):
        self.assertEqual(parse_key("D#:maj"), parse_key("Eb:maj"))
        self.assertEqual(weighted_key_score(ref_interval(0, 1, 3), ref_interval(0, 1, 3))[0], 1.0)
        self.assertEqual(
            weighted_key_score(ref_interval(0, 1, 0, "major"), ref_interval(0, 1, 9, "minor")),
            (0.3, "relative"),
        )

    def test_prediction_overlap_nonfinite_times_and_noninteger_roots_fail_closed(self):
        annotations = [[ref_interval(0, 10)]] * 3
        references = ref_doc([("song", 10, annotations)])
        for intervals in (
            [ref_interval(0, 6), ref_interval(5, 10)],
            [{"start": 0, "end": float("nan"), "root": 0, "mode": "major"}],
            [{"start": 0, "end": 10, "root": 1.9, "mode": "major"}],
            [{"start": 0, "end": 10, "root": True, "mode": "major"}],
            [{"start": 0, "end": 10, "root": "1", "mode": "major"}],
        ):
            with self.subTest(intervals=intervals), self.assertRaises(ValueError):
                self.evaluate(references, pred_doc([("song", intervals)]))

    def test_unanimous_reference_scores_only_shared_agreement_support(self):
        annotations = [
            [ref_interval(0, 5), ref_interval(5, 10, 1)],
            [ref_interval(0, 5), ref_interval(5, 10, 2)],
            [ref_interval(0, 5), ref_interval(5, 10, 3)],
        ]
        report = self.evaluate(
            ref_doc([("song", 10, annotations)]),
            pred_doc([("song", [ref_interval(0, 10)])]),
        )
        unanimous = report["unanimous_reference"]
        self.assertEqual(unanimous["pooled"]["scored_seconds"], 5)
        self.assertEqual(unanimous["pooled"]["exact_accuracy"], 1.0)
        self.assertEqual(unanimous["track_macro_weighted_score"], 1.0)
        self.assertEqual(unanimous["per_track"]["song"]["scored_seconds"], 5)

    def test_reference_interval_validation_rejects_overlaps(self):
        annotations = [[ref_interval(0, 6), ref_interval(5, 10)]] * 3
        with self.assertRaisesRegex(ValueError, "overlap"):
            self.evaluate(
                ref_doc([("song", 10, annotations)]), pred_doc([("song", [ref_interval(0, 10)])])
            )

    def test_reference_gap_with_no_predictions_is_unmeasured_only_if_no_truth(self):
        annotations = [[], [], []]
        report = self.evaluate(ref_doc([("song", 10, annotations)]), pred_doc([("song", [])]))
        metric = report["per_annotator"]["ann1"]["pooled"]
        self.assertFalse(metric["measured"])
        self.assertIsNone(metric["weighted_score"])

    def test_preparer_reads_three_reference_files_and_keeps_gaps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "01_RawData/audio_wav").mkdir(parents=True)
            with wave.open(
                str(root / "01_RawData/audio_wav/Schubert_D911-02_HU33.wav"), "wb"
            ) as audio:
                audio.setnchannels(1)
                audio.setsampwidth(2)
                audio.setframerate(10)
                audio.writeframes(b"\0\0" * 100)
            for annotator in ("ann1", "ann2", "ann3"):
                folder = root / f"02_Annotations/ann_audio_localkey-{annotator}"
                folder.mkdir(parents=True)
                (folder / "Schubert_D911-02_HU33.csv").write_text(
                    "start;end;key\n1;4;Db:maj\n6;9;G:min\n", encoding="utf-8"
                )
                (folder / "Schubert_D911-02_SC06.csv").write_text(
                    "start;end;key\n0;9;D:maj\n", encoding="utf-8"
                )
            result = prepare(root)
            self.assertEqual(len(result["records"]), 1)
            (folder / "Schubert_D911-03_HU33.csv").write_text(
                "start;end;key\n0;9;D:maj\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "extra=.*Schubert_D911-03_HU33"):
                prepare(root)
        record = result["records"][0]
        self.assertEqual(record["id"], "Schubert_D911-02_HU33")
        self.assertEqual(record["work_group"], "Schubert_D911-02")
        self.assertEqual(record["performer_group"], "Gerhard Huesch / Hanns-Udo Mueller (HU33)")
        self.assertEqual(
            record["audio_annotation_transform"], {"offset_seconds": 0, "rate_ratio": 1}
        )
        self.assertEqual(record["duration_seconds"], 10)
        self.assertEqual(record["annotations"][0]["intervals"][0]["root"], 1)
        self.assertEqual(record["annotations"][0]["intervals"][1]["start"], 6)

    def test_annotation_nonfinite_or_overlap_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "keys.csv"
            path.write_text("start;end;key\n0;6;C:maj\n5;8;G:maj\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "overlap"):
                read_annotation(path, 10)
            path.write_text("start;end;key\n0;NaN;C:maj\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "nonfinite"):
                read_annotation(path, 10)


if __name__ == "__main__":
    unittest.main()
