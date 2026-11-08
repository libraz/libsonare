"""Prepare the audit-only files from a verified Vocadito V3 archive.

The archive is read in place. Reserved-singer audio is never copied or opened,
and ZIP member names are never used as output paths.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import re
import shutil
import tempfile
import zipfile
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import TextIO

EXPECTED_ARCHIVE_MD5 = "dea40fd18f14d899643c4ba221b33a46"
DATASET_DOI = "10.5281/zenodo.5578807"
SPLIT_RULE_VERSION = "libsonare-vocadito-v3-split-v1"
METADATA_MEMBER = "vocadito_metadata.csv"
METADATA_COLUMNS = ("track_id", "singer_id", "average_pitch", "language")
ANNOTATION_VERSIONS = ("A1", "A2")
MANIFEST_HEADER = "# dataset\tfile\tannotation\tmin_f_note\tmin_f_note_offset\tflags\n"
TRACK_ID_PATTERN = re.compile(r"[0-9]+\Z")


class PreparationError(ValueError):
    """The source archive or requested output cannot be prepared safely."""


@dataclass(frozen=True)
class TrackMetadata:
    track_id: str
    singer_id: str


def _archive_digests(path: Path) -> tuple[str, str]:
    md5 = hashlib.md5()
    sha256 = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            md5.update(chunk)
            sha256.update(chunk)
    return md5.hexdigest(), sha256.hexdigest()


def _check_output_target(output: Path) -> None:
    if output.is_symlink():
        raise PreparationError(f"output must not be a symbolic link: {output}")
    if not output.exists():
        return
    if not output.is_dir():
        raise PreparationError(f"output exists and is not a directory: {output}")
    if any(output.iterdir()):
        raise PreparationError(f"output directory is not empty: {output}")


def _unique_member(archive: zipfile.ZipFile, name: str) -> zipfile.ZipInfo:
    matches = [info for info in archive.infolist() if info.filename == name]
    if not matches:
        raise PreparationError(f"missing required ZIP member: {name}")
    if len(matches) != 1:
        raise PreparationError(f"duplicate required ZIP member: {name}")
    if matches[0].is_dir():
        raise PreparationError(f"required ZIP member is a directory: {name}")
    return matches[0]


def _parse_metadata(archive: zipfile.ZipFile) -> list[TrackMetadata]:
    member = _unique_member(archive, METADATA_MEMBER)
    try:
        metadata_text = archive.read(member).decode("utf-8-sig")
    except (UnicodeDecodeError, zipfile.BadZipFile, RuntimeError) as exc:
        raise PreparationError(f"could not read {METADATA_MEMBER}: {exc}") from exc

    reader = csv.DictReader(io.StringIO(metadata_text, newline=""))
    if tuple(reader.fieldnames or ()) != METADATA_COLUMNS:
        raise PreparationError(f"{METADATA_MEMBER} columns must be {','.join(METADATA_COLUMNS)}")

    tracks: list[TrackMetadata] = []
    seen_track_ids: set[str] = set()
    for row_number, row in enumerate(reader, start=2):
        if None in row or any(row.get(column) is None for column in METADATA_COLUMNS):
            raise PreparationError(f"malformed metadata row {row_number}: expected four columns")
        track_id = row["track_id"]
        singer_id = row["singer_id"]
        language = row["language"]
        if not TRACK_ID_PATTERN.fullmatch(track_id):
            raise PreparationError(f"invalid track_id on metadata row {row_number}")
        if track_id in seen_track_ids:
            raise PreparationError(f"duplicate track_id in metadata: {track_id}")
        if not singer_id or singer_id != singer_id.strip():
            raise PreparationError(f"invalid singer_id on metadata row {row_number}")
        if not language or language != language.strip():
            raise PreparationError(f"invalid language on metadata row {row_number}")
        try:
            average_pitch = float(row["average_pitch"])
        except ValueError as exc:
            raise PreparationError(f"invalid average_pitch on metadata row {row_number}") from exc
        if not math.isfinite(average_pitch) or average_pitch <= 0.0:
            raise PreparationError(f"invalid average_pitch on metadata row {row_number}")
        seen_track_ids.add(track_id)
        tracks.append(TrackMetadata(track_id=track_id, singer_id=singer_id))

    if not tracks:
        raise PreparationError(f"{METADATA_MEMBER} contains no tracks")
    return tracks


def _split_tracks(
    tracks: list[TrackMetadata],
) -> tuple[list[str], list[str], list[TrackMetadata], list[TrackMetadata]]:
    singer_ids = {track.singer_id for track in tracks}
    singer_order = sorted(
        singer_ids,
        key=lambda singer_id: hashlib.sha256(
            f"{SPLIT_RULE_VERSION}:{singer_id}".encode()
        ).hexdigest(),
    )
    reserved_singer_count = math.ceil(len(singer_order) / 3)
    reserved_singers = singer_order[:reserved_singer_count]
    audit_singers = singer_order[reserved_singer_count:]
    reserved_set = set(reserved_singers)
    reserved_tracks = sorted(
        (track for track in tracks if track.singer_id in reserved_set),
        key=lambda track: (int(track.track_id), track.track_id),
    )
    audit_tracks = sorted(
        (track for track in tracks if track.singer_id not in reserved_set),
        key=lambda track: (int(track.track_id), track.track_id),
    )
    return reserved_singers, audit_singers, reserved_tracks, audit_tracks


def _parse_annotation(source: TextIO, member: str) -> tuple[bytes, int]:
    rows: list[str] = []
    try:
        for row_number, row in enumerate(csv.reader(source), start=1):
            if len(row) != 3:
                raise PreparationError(
                    f"malformed annotation row {row_number} in {member}: expected three columns"
                )
            try:
                onset = Decimal(row[0].strip())
                pitch_hz = Decimal(row[1].strip())
                duration = Decimal(row[2].strip())
                onset_float, pitch_float, duration_float = map(float, (onset, pitch_hz, duration))
            except (InvalidOperation, ValueError, OverflowError) as exc:
                raise PreparationError(
                    f"malformed number on annotation row {row_number} in {member}"
                ) from exc
            if (
                not onset.is_finite()
                or not pitch_hz.is_finite()
                or not duration.is_finite()
                or not all(
                    math.isfinite(value) for value in (onset_float, pitch_float, duration_float)
                )
                or onset < 0
                or pitch_hz <= 0
                or duration <= 0
            ):
                raise PreparationError(
                    f"invalid onset, pitch, or duration on annotation row {row_number} in {member}"
                )
            offset = onset + duration
            if not offset.is_finite() or not math.isfinite(float(offset)):
                raise PreparationError(
                    f"non-finite offset on annotation row {row_number} in {member}"
                )
            rows.append(f"{onset:f}\t{offset:f}\t{pitch_hz:f}\n")
    except csv.Error as exc:
        raise PreparationError(f"malformed CSV in {member}: {exc}") from exc

    if not rows:
        raise PreparationError(f"empty annotation passage: {member}")
    return "".join(rows).encode("utf-8"), len(rows)


def _copy_member(
    archive: zipfile.ZipFile,
    member: zipfile.ZipInfo,
    destination: Path,
) -> str:
    digest = hashlib.sha256()
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        with archive.open(member, "r") as source, destination.open("wb") as output:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                output.write(chunk)
                digest.update(chunk)
    except (OSError, zipfile.BadZipFile, RuntimeError, EOFError) as exc:
        raise PreparationError(
            f"could not copy required ZIP member {member.filename}: {exc}"
        ) from exc
    if member.file_size == 0:
        raise PreparationError(f"empty required audio member: {member.filename}")
    return digest.hexdigest()


def _convert_annotation(
    archive: zipfile.ZipFile,
    member: zipfile.ZipInfo,
    destination: Path,
) -> tuple[str, int]:
    try:
        with (
            archive.open(member, "r") as raw,
            io.TextIOWrapper(raw, encoding="utf-8-sig", newline="") as text,
        ):
            converted, note_count = _parse_annotation(text, member.filename)
    except (UnicodeDecodeError, zipfile.BadZipFile, RuntimeError, EOFError) as exc:
        raise PreparationError(f"could not read annotation {member.filename}: {exc}") from exc
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(converted)
    return hashlib.sha256(converted).hexdigest(), note_count


def _write_json(path: Path, document: dict[str, object]) -> None:
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _prepare_verified_archive(
    archive: zipfile.ZipFile,
    output: Path,
    archive_md5: str,
    archive_sha256: str,
) -> dict[str, object]:
    _check_output_target(output)
    tracks = _parse_metadata(archive)
    reserved_singers, audit_singers, reserved_tracks, audit_tracks = _split_tracks(tracks)

    required_members: dict[str, zipfile.ZipInfo] = {}
    for track in audit_tracks:
        required_names = [f"Audio/vocadito_{track.track_id}.wav"]
        required_names.extend(
            f"Annotations/Notes/vocadito_{track.track_id}_notes{version}.csv"
            for version in ANNOTATION_VERSIONS
        )
        for name in required_names:
            required_members[name] = _unique_member(archive, name)

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.staging-", dir=output.parent))
    committed = False
    try:
        files_sha256: dict[str, str] = {}
        annotation_note_counts: dict[str, dict[str, int]] = {}
        manifest_rows: list[str] = []
        for track in audit_tracks:
            audio_member = f"Audio/vocadito_{track.track_id}.wav"
            audio_relative = f"audio/vocadito_{track.track_id}.wav"
            files_sha256[audio_relative] = _copy_member(
                archive, required_members[audio_member], staging / audio_relative
            )
            annotation_note_counts[track.track_id] = {}
            for version in ANNOTATION_VERSIONS:
                source_member = f"Annotations/Notes/vocadito_{track.track_id}_notes{version}.csv"
                annotation_relative = f"annotations/vocadito_{track.track_id}_notes{version}.tsv"
                digest, note_count = _convert_annotation(
                    archive, required_members[source_member], staging / annotation_relative
                )
                files_sha256[annotation_relative] = digest
                annotation_note_counts[track.track_id][version] = note_count
                manifest_rows.append(
                    f"vocadito-{version.lower()}\t{audio_relative}\t{annotation_relative}"
                    "\t0\t0\treport_only\n"
                )

        manifest_bytes = (MANIFEST_HEADER + "".join(manifest_rows)).encode("utf-8")
        (staging / "notes_manifest.tsv").write_bytes(manifest_bytes)
        files_sha256["notes_manifest.tsv"] = hashlib.sha256(manifest_bytes).hexdigest()

        provenance: dict[str, object] = {
            "dataset": "Vocadito V3",
            "dataset_doi": DATASET_DOI,
            "archive_md5": archive_md5,
            "archive_sha256": archive_sha256,
            "split_rule_version": SPLIT_RULE_VERSION,
            "split_rule": (
                "Sort unique singer IDs by ascending SHA256 of "
                "'libsonare-vocadito-v3-split-v1:' + singer ID; reserve the first "
                "ceil(singer_count / 3) singers; audit all remaining singers."
            ),
            "reserved_singer_ids": reserved_singers,
            "audit_singer_ids": audit_singers,
            "reserved_track_ids": [track.track_id for track in reserved_tracks],
            "audit_track_ids": [track.track_id for track in audit_tracks],
            "annotation_versions": list(ANNOTATION_VERSIONS),
            "counts": {
                "total_singers": len(reserved_singers) + len(audit_singers),
                "total_tracks": len(tracks),
                "reserved_singers": len(reserved_singers),
                "reserved_tracks": len(reserved_tracks),
                "audit_singers": len(audit_singers),
                "audit_tracks": len(audit_tracks),
                "audio_files": len(audit_tracks),
                "annotation_files": len(audit_tracks) * len(ANNOTATION_VERSIONS),
                "manifest_rows": len(manifest_rows),
                "annotation_notes_by_track": annotation_note_counts,
            },
            "files_sha256": dict(sorted(files_sha256.items())),
            "files_sha256_excludes": ["split.json (self-referential provenance)"],
        }
        _write_json(staging / "split.json", provenance)

        _check_output_target(output)
        if output.exists():
            output.rmdir()
        staging.rename(output)
        committed = True
    finally:
        if not committed:
            shutil.rmtree(staging, ignore_errors=True)
    return provenance


def prepare_archive(archive_path: Path, output: Path) -> dict[str, object]:
    """Verify the official archive checksum and atomically prepare audit files."""
    archive_path = Path(archive_path)
    output = Path(output)
    _check_output_target(output)
    try:
        archive_md5, archive_sha256 = _archive_digests(archive_path)
    except OSError as exc:
        raise PreparationError(f"could not read archive: {exc}") from exc
    if archive_md5 != EXPECTED_ARCHIVE_MD5:
        raise PreparationError(
            f"archive MD5 mismatch: expected {EXPECTED_ARCHIVE_MD5}, got {archive_md5}"
        )
    try:
        with zipfile.ZipFile(archive_path, "r") as archive:
            return _prepare_verified_archive(archive, output, archive_md5, archive_sha256)
    except zipfile.BadZipFile as exc:
        raise PreparationError(f"invalid ZIP archive: {exc}") from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Prepare the audit-only subset from a verified Vocadito V3 archive."
    )
    parser.add_argument("--archive", required=True, type=Path, help="verified source ZIP archive")
    parser.add_argument("--output", required=True, type=Path, help="new or empty output directory")
    args = parser.parse_args(argv)
    try:
        provenance = prepare_archive(args.archive, args.output)
    except (PreparationError, OSError, zipfile.BadZipFile) as exc:
        parser.error(str(exc))
    counts = provenance["counts"]
    assert isinstance(counts, dict)
    print(
        f"Prepared {counts['audit_tracks']} audit tracks from {counts['audit_singers']} singers "
        f"({counts['manifest_rows']} report-only manifest rows)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
