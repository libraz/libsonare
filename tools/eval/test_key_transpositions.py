"""Tests for the source-only published-transposition audit."""

from __future__ import annotations

import csv
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_key_transpositions import audit_source, main

WORK = "Schubert_D911-01"
PERFORMANCE = "HU33"
ANNOTATORS = ("ann1", "ann2", "ann3")


def write_table(path: Path, columns: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, delimiter=";")
        writer.writeheader()
        writer.writerows(rows)


def local_path(root: Path, side: str, annotator: str, work_id: str = WORK) -> Path:
    folder = f"ann_{side}_localkey-{annotator}"
    suffix = "_HU33" if side == "audio" else ""
    return root / "02_Annotations" / folder / f"{work_id}{suffix}.csv"


def make_source(
    root: Path,
    *,
    score_keys: tuple[str, ...] = ("C:maj", "G:maj", "A:min"),
    audio_keys: tuple[str, ...] = ("D:maj", "A:maj", "B:min"),
) -> None:
    annotation_dir = root / "02_Annotations"
    write_table(
        annotation_dir / "ann_score_globalkey.csv",
        ("WorkID", "key"),
        [{"WorkID": WORK, "key": "C:maj"}],
    )
    write_table(
        annotation_dir / "ann_audio_globalkey.csv",
        ("WorkID", "PerformanceID", "key"),
        [{"WorkID": WORK, "PerformanceID": PERFORMANCE, "key": "D:maj"}],
    )
    for annotator in ANNOTATORS:
        for side, labels in (("score", score_keys), ("audio", audio_keys)):
            path = local_path(root, side, annotator)
            rows = [
                {"start": str(index * 10), "end": str((index + 1) * 10), "key": label}
                for index, label in enumerate(labels)
            ]
            write_table(path, ("start", "end", "key"), rows)


class KeyTranspositionAuditTests(unittest.TestCase):
    def test_valid_enharmonic_transposition_qualifies_without_mutating_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root, score_keys=("C:maj", "C#:min"), audio_keys=("D:maj", "Eb:min"))
            before = {path: path.read_bytes() for path in (root / "02_Annotations").rglob("*.csv")}

            report = audit_source(root, PERFORMANCE, [WORK])

            self.assertEqual(report["qualified_work_ids"], [WORK])
            self.assertEqual(report["quarantined_work_ids"], [])
            result = report["results"][0]
            self.assertEqual(result["expected_root_transposition_mod12"], 2)
            self.assertTrue(all(variant["qualified"] for variant in result["variants"]))
            for path, contents in before.items():
                self.assertEqual(path.read_bytes(), contents)
            hashes = {item["path"]: item["sha256"] for item in report["input_files"]}
            for path, contents in before.items():
                relative = path.relative_to(root).as_posix()
                self.assertEqual(hashes[relative], hashlib.sha256(contents).hexdigest())

    def test_one_variant_with_rowwise_transposition_drift_quarantines_whole_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            drifted = local_path(root, "audio", "ann2")
            write_table(
                drifted,
                ("start", "end", "key"),
                [
                    {"start": "0", "end": "10", "key": "D:maj"},
                    {"start": "10", "end": "20", "key": "Bb:maj"},
                    {"start": "20", "end": "30", "key": "C#:min"},
                ],
            )

            report = audit_source(root, PERFORMANCE, [WORK])

            self.assertEqual(report["qualified_work_ids"], [])
            self.assertEqual(report["quarantined_work_ids"], [WORK])
            result = report["results"][0]
            self.assertTrue(result["variants"][0]["qualified"])
            self.assertFalse(result["variants"][1]["qualified"])
            self.assertEqual(
                [failure["row_index"] for failure in result["variants"][1]["failures"]],
                [1, 2],
            )
            self.assertEqual(result["variants"][1]["failures"][0]["score_label"], "G:maj")
            self.assertEqual(result["variants"][1]["failures"][0]["audio_label"], "Bb:maj")

    def test_mode_mismatch_fails_variant(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            write_table(
                local_path(root, "audio", "ann1"),
                ("start", "end", "key"),
                [
                    {"start": "0", "end": "10", "key": "D:min"},
                    {"start": "10", "end": "20", "key": "A:maj"},
                    {"start": "20", "end": "30", "key": "B:min"},
                ],
            )

            report = audit_source(root, PERFORMANCE, [WORK])

            failure = report["results"][0]["variants"][0]["failures"][0]
            self.assertEqual(failure["row_index"], 0)
            self.assertIn("mode", failure["reason"])
            self.assertEqual(failure["score_label"], "C:maj")
            self.assertEqual(failure["audio_label"], "D:min")

    def test_malformed_local_key_row_fails_closed_with_labels(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            write_table(
                local_path(root, "audio", "ann1"),
                ("start", "end", "key"),
                [
                    {"start": "0", "end": "10", "key": "not-a-key"},
                    {"start": "10", "end": "20", "key": "A:maj"},
                    {"start": "20", "end": "30", "key": "B:min"},
                ],
            )

            report = audit_source(root, PERFORMANCE, [WORK])

            failure = report["results"][0]["variants"][0]["failures"][0]
            self.assertEqual(failure["row_index"], 0)
            self.assertEqual(failure["score_label"], "C:maj")
            self.assertEqual(failure["audio_label"], "not-a-key")
            self.assertIn("unsupported key label", failure["reason"])

    def test_unterminated_quotes_never_qualify_global_or_local_sources(self):
        malformed = [
            (
                "02_Annotations/ann_audio_localkey-ann1/Schubert_D911-01_HU33.csv",
                'start;end;key\n0;10;D:maj\n10;20;A:maj\n20;30;"B:min\n',
            ),
            ("02_Annotations/ann_score_globalkey.csv", 'WorkID;key\nSchubert_D911-01;"C:maj\n'),
            (
                "02_Annotations/ann_audio_globalkey.csv",
                'WorkID;PerformanceID;key\nSchubert_D911-01;HU33;"D:maj\n',
            ),
        ]
        for relative, contents in malformed:
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary) / "source"
                make_source(root)
                (root / relative).write_text(contents, encoding="utf-8")
                report = audit_source(root, PERFORMANCE, [WORK])
                self.assertEqual(report["qualified_work_ids"], [])
                self.assertEqual(report["quarantined_work_ids"], [WORK])

    def test_missing_local_file_quarantines_work_with_diagnostic(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            local_path(root, "audio", "ann3").unlink()

            report = audit_source(root, PERFORMANCE, [WORK])

            result = report["results"][0]
            self.assertEqual(report["quarantined_work_ids"], [WORK])
            self.assertFalse(result["variants"][2]["qualified"])
            self.assertIn("missing", result["variants"][2]["failures"][0]["reason"])

    def test_differing_local_row_count_quarantines_variant(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            write_table(
                local_path(root, "audio", "ann3"),
                ("start", "end", "key"),
                [{"start": "0", "end": "10", "key": "D:maj"}],
            )

            report = audit_source(root, PERFORMANCE, [WORK])

            failures = report["results"][0]["variants"][2]["failures"]
            self.assertTrue(any("row count" in failure["reason"] for failure in failures))
            self.assertEqual(report["quarantined_work_ids"], [WORK])

    def test_duplicate_global_id_quarantines_selected_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            score_global = root / "02_Annotations/ann_score_globalkey.csv"
            write_table(
                score_global,
                ("WorkID", "key"),
                [
                    {"WorkID": WORK, "key": "C:maj"},
                    {"WorkID": WORK, "key": "C:maj"},
                ],
            )

            report = audit_source(root, PERFORMANCE, [WORK])

            self.assertEqual(report["quarantined_work_ids"], [WORK])
            self.assertTrue(
                any(
                    "duplicate" in failure["reason"] for failure in report["results"][0]["failures"]
                )
            )

    def test_global_mode_mismatch_quarantines_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            write_table(
                root / "02_Annotations/ann_audio_globalkey.csv",
                ("WorkID", "PerformanceID", "key"),
                [{"WorkID": WORK, "PerformanceID": PERFORMANCE, "key": "D:min"}],
            )

            report = audit_source(root, PERFORMANCE, [WORK])

            self.assertEqual(report["quarantined_work_ids"], [WORK])
            self.assertTrue(
                any(
                    "global mode" in failure["reason"]
                    for failure in report["results"][0]["failures"]
                )
            )

    def test_cli_writes_report_before_returning_quarantine_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            make_source(root)
            local_path(root, "score", "ann1").unlink()
            output = Path(temporary) / "report.json"

            status = main(
                [
                    "--source",
                    str(root),
                    "--works",
                    WORK,
                    "--output",
                    str(output),
                ]
            )

            self.assertEqual(status, 1)
            self.assertTrue(output.is_file())
            self.assertIn(WORK, output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
