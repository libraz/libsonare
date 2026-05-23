#!/usr/bin/env python3
"""Audit optional fixture manifests without running audio analysis.

This reports which rows are configured, which local audio/annotation files are
present, and which rows are report-only. It is intended as a preflight check
before promoting local dataset rows into hard regression gates.
"""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass
from pathlib import Path

FORBIDDEN_OPTIONAL_KEYS = {
    "dataset_id",
    "download_url",
    "external_id",
    "license",
    "license_url",
    "rights",
    "rights" + "_" + "label",
    "source",
    "source_id",
    "source" + "_" + "ref",
    "source" + "_" + "url",
    "track_id",
    "url",
}

FORBIDDEN_VALUE_FRAGMENTS = (
    "http://",
    "https://",
    "cc" + "-by",
    "cc0",
    "copyright",
    "creative" + " commons",
    "doi:",
    "expert" + " revision",
    "public" + " domain",
)


@dataclass
class ManifestSpec:
    name: str
    path: Path
    audio_column: int
    annotation_column: int | None = None
    fixed_columns: int | None = None


@dataclass
class ManifestStats:
    name: str
    rows: int = 0
    ready: int = 0
    missing_audio: int = 0
    missing_annotation: int = 0
    report_only: int = 0
    enforced: int = 0
    provenance_violations: int = 0


def fixed_column_count(spec: ManifestSpec) -> int:
    if spec.fixed_columns is not None:
        return spec.fixed_columns
    if spec.annotation_column is not None:
        return max(spec.audio_column, spec.annotation_column) + 1
    return spec.audio_column + 1


def provenance_violations(row: list[str], optional_start: int) -> list[str]:
    violations: list[str] = []
    for token in row[optional_start:]:
        key, _, value = token.partition("=")
        normalized_key = key.strip().lower()
        normalized_value = value.strip().lower()
        normalized_token = token.strip().lower()
        if normalized_key in FORBIDDEN_OPTIONAL_KEYS:
            violations.append(token)
            continue
        if any(fragment in normalized_value for fragment in FORBIDDEN_VALUE_FRAGMENTS):
            violations.append(token)
            continue
        if any(fragment in normalized_token for fragment in FORBIDDEN_VALUE_FRAGMENTS):
            violations.append(token)
    return violations


def read_rows(path: Path) -> list[list[str]]:
    if not path.exists():
        return []
    rows: list[list[str]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.rstrip("\n")
            if not stripped or stripped.startswith("#"):
                continue
            rows.append(stripped.split("\t"))
    return rows


def audit_manifest(root: Path, spec: ManifestSpec, verbose: bool) -> ManifestStats:
    stats = ManifestStats(spec.name)
    rows = read_rows(spec.path)
    stats.rows = len(rows)
    optional_start = fixed_column_count(spec)
    for row in rows:
        if len(row) <= spec.audio_column:
            continue
        report_only = "report_only" in row
        stats.report_only += int(report_only)
        stats.enforced += int(not report_only)
        row_violations = provenance_violations(row, optional_start)
        stats.provenance_violations += len(row_violations)

        audio_path = root / row[spec.audio_column]
        annotation_path = (
            root / row[spec.annotation_column]
            if spec.annotation_column is not None and len(row) > spec.annotation_column
            else None
        )
        audio_ok = audio_path.exists()
        annotation_ok = annotation_path is None or annotation_path.exists()
        stats.missing_audio += int(not audio_ok)
        stats.missing_annotation += int(not annotation_ok)
        stats.ready += int(audio_ok and annotation_ok)

        if verbose:
            status = "ready" if audio_ok and annotation_ok else "missing"
            gate = "report_only" if report_only else "enforced"
            print(f"{spec.name}\t{status}\t{gate}\t{row[0]}\t{row[1]}")
            if not audio_ok:
                print(f"  missing audio: {audio_path}")
            if annotation_path is not None and not annotation_ok:
                print(f"  missing annotation: {annotation_path}")
            for violation in row_violations:
                print(f"  provenance violation: {violation}")
    return stats


def print_summary(stats: list[ManifestStats]) -> None:
    print(
        "manifest\trows\tready\tmissing_audio\tmissing_annotation\treport_only\tenforced\t"
        "provenance_violations"
    )
    for item in stats:
        print(
            f"{item.name}\t{item.rows}\t{item.ready}\t{item.missing_audio}\t"
            f"{item.missing_annotation}\t{item.report_only}\t{item.enforced}\t"
            f"{item.provenance_violations}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit optional libsonare fixture manifests.")
    parser.add_argument(
        "--music-root",
        type=Path,
        default=Path(os.environ.get("SONARE_MUSIC_FIXTURE_ROOT", "tests/fixtures/music_eval")),
        help="Root directory for music_eval manifests. Defaults to SONARE_MUSIC_FIXTURE_ROOT.",
    )
    parser.add_argument(
        "--acoustic-root",
        type=Path,
        default=Path(os.environ.get("SONARE_ACOUSTIC_FIXTURE_ROOT", "tests/fixtures/acoustic")),
        help="Root directory for acoustic manifests. Defaults to SONARE_ACOUSTIC_FIXTURE_ROOT.",
    )
    parser.add_argument(
        "--ebu-root",
        type=Path,
        default=Path(os.environ.get("SONARE_EBU_R128_FIXTURE_ROOT", "tests/fixtures/ebu_r128")),
        help="Root directory for EBU R128 manifests. Defaults to SONARE_EBU_R128_FIXTURE_ROOT.",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    music_specs = [
        ManifestSpec("music:bpm", args.music_root / "bpm_manifest.tsv", 1, fixed_columns=4),
        ManifestSpec("music:key", args.music_root / "key_manifest.tsv", 1, fixed_columns=4),
        ManifestSpec("music:meter", args.music_root / "meter_manifest.tsv", 1, fixed_columns=5),
        ManifestSpec("music:beat", args.music_root / "beat_manifest.tsv", 1, 2, fixed_columns=6),
        ManifestSpec("music:downbeat", args.music_root / "downbeat_manifest.tsv", 1, 2, fixed_columns=6),
        ManifestSpec("music:chord", args.music_root / "chord_manifest.tsv", 1, 2, fixed_columns=4),
    ]
    acoustic_specs = [
        ManifestSpec("acoustic:rt60", args.acoustic_root / "manifest.tsv", 1, fixed_columns=6)
    ]
    ebu_specs = [ManifestSpec("ebu:r128", args.ebu_root / "manifest.tsv", 0, fixed_columns=6)]

    stats = [audit_manifest(args.music_root, spec, args.verbose) for spec in music_specs]
    stats.extend(audit_manifest(args.acoustic_root, spec, args.verbose) for spec in acoustic_specs)
    stats.extend(audit_manifest(args.ebu_root, spec, args.verbose) for spec in ebu_specs)
    print_summary(stats)
    return 1 if any(item.provenance_violations for item in stats) else 0


if __name__ == "__main__":
    raise SystemExit(main())
