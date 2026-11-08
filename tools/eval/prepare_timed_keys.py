#!/usr/bin/env python3
"""Prepare strict, performance-aligned Winterreise local-key references."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import wave
from pathlib import Path
from typing import Any

FORMAT_VERSION = "timed_keys_reference_v1"
PITCH_CLASSES = {
    "C": 0,
    "B#": 0,
    "C#": 1,
    "DB": 1,
    "D": 2,
    "D#": 3,
    "EB": 3,
    "E": 4,
    "FB": 4,
    "E#": 5,
    "F": 5,
    "F#": 6,
    "GB": 6,
    "G": 7,
    "G#": 8,
    "AB": 8,
    "A": 9,
    "A#": 10,
    "BB": 10,
    "B": 11,
    "CB": 11,
}
KEY_RE = re.compile(r"^([A-Ga-g])([#b]?):(maj|min)$")
ANNOTATORS = ("ann1", "ann2", "ann3")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_key(label: str) -> tuple[int, str]:
    match = KEY_RE.fullmatch(label.strip())
    if not match:
        raise ValueError(f"unsupported key label {label!r}")
    pitch = match.group(1).upper() + match.group(2).replace("b", "B")
    try:
        root = PITCH_CLASSES[pitch]
    except KeyError as error:
        raise ValueError(f"unsupported pitch spelling {pitch!r}") from error
    return root, "major" if match.group(3) == "maj" else "minor"


def wav_duration(path: Path) -> float:
    try:
        with wave.open(str(path), "rb") as source:
            rate = source.getframerate()
            if rate <= 0:
                raise ValueError(f"{path}: invalid sample rate {rate}")
            return source.getnframes() / rate
    except (wave.Error, EOFError) as error:
        raise ValueError(f"{path}: invalid WAV: {error}") from error


def read_annotation(path: Path, duration: float) -> list[dict[str, Any]]:
    intervals: list[dict[str, Any]] = []
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = csv.DictReader(stream, delimiter=";")
        if rows.fieldnames != ["start", "end", "key"]:
            raise ValueError(f"{path}: expected columns start;end;key, got {rows.fieldnames!r}")
        previous_end = 0.0
        for line, row in enumerate(rows, start=2):
            try:
                start = float(row["start"])
                end = float(row["end"])
                if not math.isfinite(start) or not math.isfinite(end):
                    raise ValueError("nonfinite time")
                if start < 0 or end <= start or end > duration:
                    raise ValueError(f"invalid interval [{start}, {end}) for duration {duration}")
                if start < previous_end:
                    raise ValueError(f"overlap or unsorted interval starts before {previous_end}")
                root, mode = parse_key(row["key"])
            except (AttributeError, TypeError, ValueError) as error:
                raise ValueError(f"{path}:{line}: {error}") from error
            intervals.append({"start": start, "end": end, "root": root, "mode": mode})
            previous_end = end
    if not intervals:
        raise ValueError(f"{path}: no key intervals")
    return intervals


def prepare(source: Path) -> dict[str, Any]:
    source = source.resolve()
    wav_dir = source / "01_RawData" / "audio_wav"
    annotation_dir = source / "02_Annotations"
    wavs = sorted(wav_dir.glob("*_HU33.wav"))
    if not wavs:
        raise ValueError(f"no HU33 WAV files found under {wav_dir}")
    records = []
    expected_ids = {path.stem for path in wavs}
    for annotator in ANNOTATORS:
        folder = annotation_dir / f"ann_audio_localkey-{annotator}"
        found = {path.stem for path in folder.glob("*_HU33.csv")}
        if found != expected_ids:
            raise ValueError(
                f"{folder}: annotation stems differ from audio; missing={sorted(expected_ids - found)}, "
                f"extra={sorted(found - expected_ids)}"
            )
    for audio_path in wavs:
        record_id = audio_path.stem
        duration = wav_duration(audio_path)
        annotation_records = []
        for annotator in ANNOTATORS:
            annotation_path = (
                annotation_dir / f"ann_audio_localkey-{annotator}" / f"{record_id}.csv"
            )
            annotation_records.append(
                {
                    "annotator": annotator,
                    "path": annotation_path.relative_to(source).as_posix(),
                    "sha256": sha256_file(annotation_path),
                    "intervals": read_annotation(annotation_path, duration),
                }
            )
        composer, work_id, performer = record_id.split("_")
        records.append(
            {
                "id": record_id,
                "work_group": f"{composer}_{work_id}",
                "performance_id": performer,
                "performer_group": "Gerhard Huesch / Hanns-Udo Mueller (HU33)",
                "composer_group": "Franz Schubert",
                "edited_audio": work_id == "D911-01",
                "audio_annotation_transform": {"offset_seconds": 0, "rate_ratio": 1},
                "audio": audio_path.relative_to(source).as_posix(),
                "duration_seconds": duration,
                "audio_sha256": sha256_file(audio_path),
                "annotations": annotation_records,
            }
        )
    return {
        "format_version": FORMAT_VERSION,
        "dataset": "Schubert Winterreise Dataset",
        "dataset_version": "2.1",
        "source_record_url": "https://zenodo.org/records/10839767",
        "annotation_license": "CC-BY-3.0",
        "audio_license": "PDM-1.0 (HU33 included license)",
        "split_status": "external_domain_control_only_not_independent_pop_artists",
        "reference_origin": "score-derived and audio-aligned; local-key annotators reported separately",
        "annotation_process": (
            "Related expert revisions, not independent blind annotations; previous labels were "
            "known to the annotators (Weiss et al. 2021, section 4.3)."
        ),
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path, required=True, help="extracted Winterreise v2.1 root"
    )
    parser.add_argument("--output", type=Path, required=True, help="output JSON path")
    args = parser.parse_args()
    result = prepare(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(
        f"prepared {len(result['records'])} tracks with {len(ANNOTATORS)} annotators: {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
