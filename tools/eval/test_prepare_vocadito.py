"""Tests for the offline Vocadito archive preparer."""

from __future__ import annotations

import csv
import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_vocadito as preparer


def _make_archive(
    path: Path,
    *,
    missing_member: str | None = None,
    bad_annotation: tuple[str, str, bytes] | None = None,
) -> Path:
    metadata = io.StringIO(newline="")
    writer = csv.writer(metadata, lineterminator="\n")
    writer.writerow(("track_id", "singer_id", "average_pitch", "language"))
    writer.writerows(
        (
            ("1", "S1", "55", "English"),
            ("2", "S2", "60", "Spanish"),
            ("3", "S1", "55", "English"),
            ("4", "S3", "65", "French"),
        )
    )

    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("vocadito_metadata.csv", metadata.getvalue().encode("utf-8"))
        for track_id in ("1", "2", "3", "4"):
            audio_member = f"Audio/vocadito_{track_id}.wav"
            if audio_member != missing_member:
                audio_bytes = b"" if track_id == "2" else b"RIFF" + track_id.encode("ascii")
                archive.writestr(audio_member, audio_bytes)
            for version in ("A1", "A2"):
                annotation_member = f"Annotations/Notes/vocadito_{track_id}_notes{version}.csv"
                if annotation_member == missing_member:
                    continue
                if track_id == "2":
                    # This reserved-singer member must remain unopened.
                    annotation_bytes = b"\xff reserved annotation"
                elif version == "A1":
                    annotation_bytes = b"0.125,220.0,0.25\n"
                else:
                    annotation_bytes = b"0.5,440,0.125\n"
                if bad_annotation is not None and bad_annotation[:2] == (track_id, version):
                    annotation_bytes = bad_annotation[2]
                archive.writestr(annotation_member, annotation_bytes)
    return path


def _prepare_fixture(archive_path: Path, output: Path) -> dict[str, object]:
    digest = hashlib.md5(archive_path.read_bytes()).hexdigest()
    with mock.patch.object(preparer, "EXPECTED_ARCHIVE_MD5", digest):
        return preparer.prepare_archive(archive_path, output)


def _manifest_rows(output: Path) -> list[list[str]]:
    manifest = (output / "notes_manifest.tsv").read_text(encoding="utf-8").splitlines()
    return [line.split("\t") for line in manifest if line and not line.startswith("#")]


class PrepareVocaditoTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_audit_split_is_singer_disjoint_and_outputs_both_annotations(self) -> None:
        archive_path = _make_archive(self.tmp_path / "source.zip")
        output = self.tmp_path / "prepared"

        provenance = _prepare_fixture(archive_path, output)

        self.assertEqual(provenance["reserved_singer_ids"], ["S2"])
        self.assertEqual(provenance["audit_singer_ids"], ["S3", "S1"])
        self.assertTrue(
            set(provenance["reserved_singer_ids"]).isdisjoint(provenance["audit_singer_ids"])
        )
        self.assertEqual(provenance["reserved_track_ids"], ["2"])
        self.assertEqual(provenance["audit_track_ids"], ["1", "3", "4"])
        self.assertTrue(
            set(provenance["reserved_track_ids"]).isdisjoint(provenance["audit_track_ids"])
        )

        rows = _manifest_rows(output)
        self.assertEqual(len(rows), 6)
        self.assertEqual({row[0] for row in rows}, {"vocadito-a1", "vocadito-a2"})
        self.assertTrue(all(row[5] == "report_only" and row[3:5] == ["0", "0"] for row in rows))
        self.assertEqual(
            {row[1] for row in rows},
            {"audio/vocadito_1.wav", "audio/vocadito_3.wav", "audio/vocadito_4.wav"},
        )

        self.assertEqual((output / "audio/vocadito_1.wav").read_bytes(), b"RIFF1")
        self.assertFalse((output / "audio/vocadito_2.wav").exists())
        self.assertFalse((output / "annotations/vocadito_2_notesA1.tsv").exists())
        self.assertEqual(
            (output / "annotations/vocadito_1_notesA1.tsv").read_text(), "0.125\t0.375\t220.0\n"
        )
        self.assertEqual(
            (output / "annotations/vocadito_1_notesA2.tsv").read_text(), "0.5\t0.625\t440\n"
        )

        counts = provenance["counts"]
        self.assertEqual(counts["total_singers"], 3)
        self.assertEqual(counts["total_tracks"], 4)
        self.assertEqual(counts["reserved_tracks"], 1)
        self.assertEqual(counts["audit_tracks"], 3)
        self.assertEqual(counts["manifest_rows"], 6)
        split_file = json.loads((output / "split.json").read_text(encoding="utf-8"))
        self.assertEqual(split_file["dataset_doi"], "10.5281/zenodo.5578807")
        self.assertEqual(
            split_file["archive_md5"], hashlib.md5(archive_path.read_bytes()).hexdigest()
        )
        self.assertIn("split.json", split_file["files_sha256_excludes"][0])
        for relative_path, digest in split_file["files_sha256"].items():
            self.assertEqual(hashlib.sha256((output / relative_path).read_bytes()).hexdigest(), digest)

    def test_malformed_or_empty_audit_annotation_fails_without_partial_output(self) -> None:
        for bad_row in (b"nan,220,0.25\n", b"0.1,-220,0.25\n", b"0.1,220,0\n", b"0.1,220\n", b""):
            with self.subTest(bad_row=bad_row):
                root = self.tmp_path / f"case-{len(list(self.tmp_path.iterdir()))}"
                root.mkdir()
                archive_path = _make_archive(
                    root / "source.zip", bad_annotation=("1", "A1", bad_row)
                )
                output = root / "prepared"

                with self.assertRaises(preparer.PreparationError):
                    _prepare_fixture(archive_path, output)

                self.assertFalse(output.exists())
                self.assertEqual(list(root.glob(".prepared.staging-*")), [])

    def test_missing_required_audit_member_fails_without_partial_output(self) -> None:
        missing = "Annotations/Notes/vocadito_3_notesA2.csv"
        archive_path = _make_archive(self.tmp_path / "source.zip", missing_member=missing)
        output = self.tmp_path / "prepared"

        with self.assertRaisesRegex(preparer.PreparationError, "missing required ZIP member"):
            _prepare_fixture(archive_path, output)

        self.assertFalse(output.exists())

    def test_checksum_is_checked_before_opening_zip_or_creating_output(self) -> None:
        archive_path = self.tmp_path / "not-a-zip.zip"
        archive_path.write_bytes(b"not a ZIP archive")
        output = self.tmp_path / "prepared"
        stderr = io.StringIO()

        with mock.patch.object(preparer, "EXPECTED_ARCHIVE_MD5", "0" * 32), redirect_stderr(
            stderr
        ), self.assertRaises(SystemExit) as exit_info:
            preparer.main(["--archive", str(archive_path), "--output", str(output)])

        self.assertEqual(exit_info.exception.code, 2)
        self.assertIn("MD5 mismatch", stderr.getvalue())
        self.assertFalse(output.exists())

    def test_nonempty_output_is_never_overwritten(self) -> None:
        archive_path = _make_archive(self.tmp_path / "source.zip")
        output = self.tmp_path / "prepared"
        output.mkdir()
        marker = output / "keep.txt"
        marker.write_text("existing", encoding="utf-8")

        with self.assertRaisesRegex(preparer.PreparationError, "not empty"):
            _prepare_fixture(archive_path, output)

        self.assertEqual(marker.read_text(encoding="utf-8"), "existing")
        self.assertEqual(list(output.iterdir()), [marker])

    def test_empty_output_directory_is_populated_atomically(self) -> None:
        archive_path = _make_archive(self.tmp_path / "source.zip")
        output = self.tmp_path / "prepared"
        output.mkdir()

        _prepare_fixture(archive_path, output)

        self.assertTrue((output / "split.json").is_file())
        self.assertEqual(len(_manifest_rows(output)), 6)


if __name__ == "__main__":
    unittest.main()
