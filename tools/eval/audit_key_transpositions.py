#!/usr/bin/env python3
"""Check published score-to-performance transposition in source key labels."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
from typing import Any

from prepare_timed_keys import parse_key

REPORT_VERSION = "source_key_transposition_audit_v1"
ANNOTATORS = ("ann1", "ann2", "ann3")
SCORE_GLOBAL_COLUMNS = ("WorkID", "key")
AUDIO_GLOBAL_COLUMNS = ("WorkID", "PerformanceID", "key")
LOCAL_COLUMNS = ("start", "end", "key")
LIMITATIONS = [
    "This checks source conversion consistency only; it does not establish musical truth.",
    "Row-sequence comparison does not validate interior boundary alignment by listening.",
    "The three annotation variants were iteratively informed and are not independent votes.",
    "No audio is read and no model predictions or annotation repairs are made.",
]


def _relative_path(source: Path, path: Path) -> str:
    return path.relative_to(source).as_posix()


def _input_file(source: Path, path: Path, kind: str) -> dict[str, Any]:
    item: dict[str, Any] = {"path": _relative_path(source, path), "kind": kind, "sha256": None}
    if not path.is_file():
        item["status"] = "missing"
        return item
    try:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        item["sha256"] = digest.hexdigest()
        item["status"] = "readable"
    except OSError as error:
        item["status"] = "unreadable"
        item["read_error"] = str(error)
    return item


def _read_global(path: Path, expected_columns: tuple[str, ...]) -> dict[str, Any]:
    result: dict[str, Any] = {"rows": [], "file_failure": None, "row_failures": []}
    try:
        with path.open(encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream, delimiter=";", strict=True)
            if tuple(reader.fieldnames or ()) != expected_columns:
                result["file_failure"] = (
                    f"expected columns {';'.join(expected_columns)}, got {reader.fieldnames!r}"
                )
                return result
            for line_number, row in enumerate(reader, start=2):
                work_id = row.get("WorkID")
                if None in row or any(row.get(column) is None for column in expected_columns):
                    failure = {
                        "row_index": line_number - 2,
                        "reason": "malformed global-key CSV row",
                    }
                    if work_id:
                        failure["work_id"] = work_id
                    result["row_failures"].append(failure)
                result["rows"].append(dict(row))
    except (OSError, UnicodeError, csv.Error) as error:
        result["file_failure"] = f"could not read global-key CSV: {error}"
    return result


def _global_key(
    loaded: dict[str, Any],
    *,
    work_id: str,
    performance_id: str | None,
    field_failures: list[dict[str, Any]],
) -> tuple[str | None, tuple[int, str] | None]:
    if loaded["file_failure"]:
        field_failures.append({"reason": loaded["file_failure"]})
        return None, None
    for failure in loaded["row_failures"]:
        if failure.get("work_id") == work_id or "work_id" not in failure:
            field_failures.append(failure)

    candidates = [row for row in loaded["rows"] if row.get("WorkID") == work_id]
    if performance_id is not None:
        candidates = [row for row in candidates if row.get("PerformanceID") == performance_id]
    if not candidates:
        field_failures.append({"reason": "missing published global-key row"})
        return None, None
    if len(candidates) != 1:
        field_failures.append(
            {"reason": f"duplicate global-key ID ({len(candidates)} matching rows)"}
        )
        return None, None

    label = candidates[0].get("key")
    try:
        if label is None:
            raise ValueError("missing key label")
        return label, parse_key(label)
    except (TypeError, ValueError) as error:
        field_failures.append({"reason": f"invalid published global key {label!r}: {error}"})
        return label, None


def _read_local(path: Path) -> tuple[list[dict[str, Any]] | None, dict[str, Any] | None]:
    if not path.is_file():
        return None, {"reason": f"missing local-key file: {path.name}"}
    rows: list[dict[str, Any]] = []
    try:
        with path.open(encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream, delimiter=";", strict=True)
            if tuple(reader.fieldnames or ()) != LOCAL_COLUMNS:
                return None, {
                    "reason": f"expected local-key columns {';'.join(LOCAL_COLUMNS)}, "
                    f"got {reader.fieldnames!r}"
                }
            for index, row in enumerate(reader):
                label = row.get("key")
                parsed: tuple[int, str] | None = None
                error: str | None = None
                try:
                    if None in row or any(row.get(column) is None for column in LOCAL_COLUMNS):
                        raise ValueError("malformed local-key CSV row")
                    start, end = float(row["start"]), float(row["end"])
                    if not math.isfinite(start) or not math.isfinite(end) or end <= start:
                        raise ValueError("invalid local-key start/end times")
                    parsed = parse_key(label)
                except (TypeError, ValueError) as parse_error:
                    error = str(parse_error)
                rows.append(
                    {
                        "row_index": index,
                        "label": label,
                        "parsed": parsed,
                        "parse_error": error,
                    }
                )
    except (OSError, UnicodeError, csv.Error) as error:
        return None, {"reason": f"could not read local-key CSV: {error}"}
    return rows, None


def _failure(
    reason: str,
    *,
    row_index: int | None = None,
    score_label: str | None = None,
    audio_label: str | None = None,
) -> dict[str, Any]:
    return {
        "row_index": row_index,
        "score_label": score_label,
        "audio_label": audio_label,
        "reason": reason,
    }


def _audit_variant(
    source: Path,
    annotation_dir: Path,
    work_id: str,
    annotator: str,
    performance_id: str,
    expected_delta: int | None,
    inherited_failures: list[dict[str, Any]],
    input_files: list[dict[str, Any]],
) -> dict[str, Any]:
    score_path = annotation_dir / f"ann_score_localkey-{annotator}" / f"{work_id}.csv"
    audio_path = (
        annotation_dir / f"ann_audio_localkey-{annotator}" / f"{work_id}_{performance_id}.csv"
    )
    score_input = _input_file(source, score_path, f"{annotator} score local key")
    audio_input = _input_file(source, audio_path, f"{annotator} audio local key")
    input_files.extend((score_input, audio_input))
    score_rows, score_error = _read_local(score_path)
    audio_rows, audio_error = _read_local(audio_path)
    failures = [
        {
            "row_index": failure.get("row_index"),
            "score_label": failure.get("score_label"),
            "audio_label": failure.get("audio_label"),
            "reason": failure["reason"],
        }
        for failure in inherited_failures
    ]
    if score_error:
        failures.append(_failure(f"score {score_error['reason']}"))
    if audio_error:
        failures.append(_failure(f"audio {audio_error['reason']}"))
    if score_rows is not None and not score_rows:
        failures.append(_failure("score local-key file has no rows"))
    if audio_rows is not None and not audio_rows:
        failures.append(_failure("audio local-key file has no rows"))

    score_count = len(score_rows) if score_rows is not None else None
    audio_count = len(audio_rows) if audio_rows is not None else None
    if score_rows is not None and audio_rows is not None:
        if len(score_rows) != len(audio_rows):
            failures.append(
                _failure(f"score/audio row count mismatch ({len(score_rows)} vs {len(audio_rows)})")
            )
        for index, (score_row, audio_row) in enumerate(zip(score_rows, audio_rows)):
            score_label, audio_label = score_row["label"], audio_row["label"]
            invalid = []
            if score_row["parse_error"]:
                invalid.append(f"invalid score row: {score_row['parse_error']}")
            if audio_row["parse_error"]:
                invalid.append(f"invalid audio row: {audio_row['parse_error']}")
            if invalid:
                failures.append(
                    _failure(
                        "; ".join(invalid),
                        row_index=index,
                        score_label=score_label,
                        audio_label=audio_label,
                    )
                )
                continue
            score_root, score_mode = score_row["parsed"]
            audio_root, audio_mode = audio_row["parsed"]
            observed_delta = (audio_root - score_root) % 12
            reasons = []
            if expected_delta is not None and observed_delta != expected_delta:
                reasons.append(
                    f"root transposition {observed_delta} semitones, expected {expected_delta}"
                )
            if score_mode != audio_mode:
                reasons.append(f"mode changed from {score_mode} to {audio_mode}")
            if reasons:
                failures.append(
                    _failure(
                        "; ".join(reasons),
                        row_index=index,
                        score_label=score_label,
                        audio_label=audio_label,
                    )
                )
        for index in range(
            min(len(score_rows), len(audio_rows)), max(len(score_rows), len(audio_rows))
        ):
            score_label = score_rows[index]["label"] if index < len(score_rows) else None
            audio_label = audio_rows[index]["label"] if index < len(audio_rows) else None
            failures.append(
                _failure(
                    "row has no counterpart in the other local-key file",
                    row_index=index,
                    score_label=score_label,
                    audio_label=audio_label,
                )
            )

    return {
        "annotator": annotator,
        "score_input": score_input,
        "audio_input": audio_input,
        "score_row_count": score_count,
        "audio_row_count": audio_count,
        "expected_root_transposition_mod12": expected_delta,
        "qualified": not failures,
        "failures": failures,
    }


def audit_source(source: Path, performance_id: str, work_ids: list[str]) -> dict[str, Any]:
    source = source.resolve()
    annotation_dir = source / "02_Annotations"
    score_global_path = annotation_dir / "ann_score_globalkey.csv"
    audio_global_path = annotation_dir / "ann_audio_globalkey.csv"
    score_global_input = _input_file(source, score_global_path, "score global key")
    audio_global_input = _input_file(source, audio_global_path, "audio global key")
    input_files = [score_global_input, audio_global_input]
    score_global = _read_global(score_global_path, SCORE_GLOBAL_COLUMNS)
    audio_global = _read_global(audio_global_path, AUDIO_GLOBAL_COLUMNS)

    results = []
    qualified_ids = []
    quarantined_ids = []
    for work_id in work_ids:
        global_failures: list[dict[str, Any]] = []
        score_label, score_key = _global_key(
            score_global,
            work_id=work_id,
            performance_id=None,
            field_failures=global_failures,
        )
        audio_label, audio_key = _global_key(
            audio_global,
            work_id=work_id,
            performance_id=performance_id,
            field_failures=global_failures,
        )
        expected_delta = None
        if score_key is not None and audio_key is not None:
            expected_delta = (audio_key[0] - score_key[0]) % 12
            if score_key[1] != audio_key[1]:
                global_failures.append(
                    {
                        "reason": f"published global mode changed from {score_key[1]} to {audio_key[1]}"
                    }
                )

        variants = [
            _audit_variant(
                source,
                annotation_dir,
                work_id,
                annotator,
                performance_id,
                expected_delta,
                global_failures,
                input_files,
            )
            for annotator in ANNOTATORS
        ]
        work_failures = [dict(failure) for failure in global_failures]
        work_qualified = not work_failures and all(variant["qualified"] for variant in variants)
        result = {
            "work_id": work_id,
            "performance_id": performance_id,
            "score_global_key": score_label,
            "audio_global_key": audio_label,
            "expected_root_transposition_mod12": expected_delta,
            "qualified": work_qualified,
            "failures": work_failures,
            "variants": variants,
        }
        results.append(result)
        (qualified_ids if work_qualified else quarantined_ids).append(work_id)

    return {
        "report_version": REPORT_VERSION,
        "source_version": "Winterreise v2.1",
        "performance_id": performance_id,
        "method": (
            "For each requested work and ann1/ann2/ann3, compare complete ordered score and matching "
            "performance local-key rows using the published global-key root transposition. Root spellings "
            "are normalized with prepare_timed_keys.parse_key; local modes must remain unchanged. "
            "No timing alignment, audio, predictions, or label repair is performed."
        ),
        "limitations": LIMITATIONS,
        "input_files": input_files,
        "results": results,
        "qualified_work_ids": qualified_ids,
        "quarantined_work_ids": quarantined_ids,
    }


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path, required=True, help="extracted Winterreise source root"
    )
    parser.add_argument("--performance", default="HU33", help="audio PerformanceID (default: HU33)")
    parser.add_argument("--works", nargs="+", required=True, metavar="WORK_ID")
    parser.add_argument("--output", type=Path, required=True, help="JSON report path")
    args = parser.parse_args(argv)
    if len(set(args.works)) != len(args.works):
        parser.error("--works must not contain duplicate IDs")
    annotation_dir = args.source.resolve() / "02_Annotations"
    protected = {
        annotation_dir / "ann_score_globalkey.csv",
        annotation_dir / "ann_audio_globalkey.csv",
    }
    for work_id in args.works:
        for annotator in ANNOTATORS:
            protected.add(annotation_dir / f"ann_score_localkey-{annotator}" / f"{work_id}.csv")
            protected.add(
                annotation_dir
                / f"ann_audio_localkey-{annotator}"
                / f"{work_id}_{args.performance}.csv"
            )
    if args.output.resolve() in {path.resolve() for path in protected}:
        parser.error("--output must not overwrite a source annotation input")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    report = audit_source(args.source, args.performance, args.works)
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0 if not report["quarantined_work_ids"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
