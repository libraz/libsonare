#!/usr/bin/env python3
"""Separate pitch-tracker errors from segmentation errors in monophonic notes.

For each Vocadito recording this runs ``sonare_eval_note_oracle`` and scores
three conditions against each annotator's notes:

* ``system``: the default monophonic transcription.
* ``reference_f0``: the same segmentation stages fed the annotated F0 contour
  instead of the tracker's, which bounds what segmentation reaches with a
  correct contour.
* ``reference_segments``: each annotated note span labelled with the median of
  the tracker's voiced frames inside it, which bounds pitch labelling given
  correct boundaries.

It also reports frame-level voicing and pitch accuracy of the tracker against
the annotated contour. Note matching follows the MIREX defaults: 50 ms onset,
50 cents pitch, offset within max(50 ms, 20% of the reference duration).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import subprocess
from pathlib import Path
from typing import Any

ONSET_TOLERANCE_SEC = 0.05
PITCH_TOLERANCE_CENTS = 50.0
OFFSET_RATIO = 0.2
OFFSET_MIN_SEC = 0.05
CONDITIONS = ("system", "reference_f0", "reference_segments")
ANNOTATORS = ("A1", "A2")


def cents(a_hz: float, b_hz: float) -> float:
    return 1200.0 * math.log2(a_hz / b_hz)


def read_reference_notes(path: Path) -> list[dict[str, float]]:
    notes = []
    with path.open(newline="") as handle:
        for row in csv.reader(handle):
            if not row:
                continue
            onset, hz, duration = (float(value) for value in row[:3])
            notes.append({"onset_sec": onset, "offset_sec": onset + duration, "hz": hz})
    return notes


def read_contour(path: Path) -> list[tuple[float, float]]:
    with path.open(newline="") as handle:
        return [(float(row[0]), float(row[1])) for row in csv.reader(handle) if row]


def matches(ref: dict[str, float], est: dict[str, float], kind: str) -> bool:
    if abs(ref["onset_sec"] - est["onset_sec"]) > ONSET_TOLERANCE_SEC:
        return False
    if kind == "onset":
        return True
    if abs(cents(est["hz"], ref["hz"])) > PITCH_TOLERANCE_CENTS:
        return False
    if kind == "note":
        return True
    duration = ref["offset_sec"] - ref["onset_sec"]
    tolerance = max(OFFSET_MIN_SEC, OFFSET_RATIO * duration)
    return abs(ref["offset_sec"] - est["offset_sec"]) <= tolerance


def matched_count(refs: list[dict[str, float]], ests: list[dict[str, float]], kind: str) -> int:
    """Maximum one-to-one matching by augmenting paths."""
    candidates = [[j for j, est in enumerate(ests) if matches(ref, est, kind)] for ref in refs]
    owner: dict[int, int] = {}

    def augment(i: int, seen: set[int]) -> bool:
        for j in candidates[i]:
            if j in seen:
                continue
            seen.add(j)
            if j not in owner or augment(owner[j], seen):
                owner[j] = i
                return True
        return False

    return sum(1 for i in range(len(refs)) if augment(i, set()))


def segment_labels(
    refs: list[dict[str, float]], f0_hz: list[float], frame_sec: float
) -> list[dict[str, float]]:
    """Reference spans labelled with the tracker's median voiced F0 inside each."""
    labelled = []
    for ref in refs:
        first = math.ceil(ref["onset_sec"] / frame_sec)
        last = math.floor(ref["offset_sec"] / frame_sec)
        voiced = sorted(hz for hz in f0_hz[first : last + 1] if hz > 0.0)
        if not voiced:
            continue
        labelled.append({**ref, "hz": voiced[len(voiced) // 2]})
    return labelled


def frame_metrics(
    f0_hz: list[float], frame_sec: float, contour: list[tuple[float, float]]
) -> dict[str, int]:
    """Counts for voicing recall/false alarm and raw pitch/chroma accuracy."""
    counts = {
        "ref_voiced": 0,
        "ref_unvoiced": 0,
        "voiced_hits": 0,
        "false_alarms": 0,
        "pitch_hits": 0,
        "chroma_hits": 0,
    }
    for time_sec, ref_hz in contour:
        frame = round(time_sec / frame_sec)
        if frame >= len(f0_hz):
            continue
        est_hz = f0_hz[frame]
        if ref_hz > 0.0:
            counts["ref_voiced"] += 1
            if est_hz > 0.0:
                counts["voiced_hits"] += 1
                error = cents(est_hz, ref_hz)
                if abs(error) <= PITCH_TOLERANCE_CENTS:
                    counts["pitch_hits"] += 1
                folded = (error + 600.0) % 1200.0 - 600.0
                if abs(folded) <= PITCH_TOLERANCE_CENTS:
                    counts["chroma_hits"] += 1
        else:
            counts["ref_unvoiced"] += 1
            if est_hz > 0.0:
                counts["false_alarms"] += 1
    return counts


def boundary_profile(refs: list[dict[str, float]], ests: list[dict[str, float]]) -> dict[str, int]:
    """Counts of the boundary error shapes, independent of pitch.

    A reference is split when two or more estimates start inside it, an
    estimate is merged when two or more references start inside it, and a
    note overlapping nothing on the other side is uncovered or spurious. A
    start counts when it falls in [onset - 50 ms, offset - 50 ms).
    """

    def starts_inside(span: dict[str, float], notes: list[dict[str, float]]) -> int:
        low = span["onset_sec"] - ONSET_TOLERANCE_SEC
        high = span["offset_sec"] - ONSET_TOLERANCE_SEC
        return sum(1 for n in notes if low <= n["onset_sec"] < high)

    def overlaps(a: dict[str, float], notes: list[dict[str, float]]) -> bool:
        return any(
            n["onset_sec"] < a["offset_sec"] and n["offset_sec"] > a["onset_sec"] for n in notes
        )

    split = [starts_inside(ref, ests) for ref in refs]
    return {
        "split_references": sum(1 for count in split if count >= 2),
        "extra_estimates_from_splits": sum(count - 1 for count in split if count >= 2),
        "merged_estimates": sum(1 for est in ests if starts_inside(est, refs) >= 2),
        "uncovered_references": sum(1 for ref in refs if not overlaps(ref, ests)),
        "spurious_estimates": sum(1 for est in ests if not overlaps(est, refs)),
    }


def f_measure(tp: int, n_ref: int, n_est: int) -> float | None:
    if n_ref + n_est == 0:
        return None
    return 2.0 * tp / (n_ref + n_est)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def evaluate(root: Path, tool: Path, track_ids: list[str]) -> dict[str, Any]:
    pooled: dict[str, dict[str, dict[str, int]]] = {
        f"{cond}/{ann}": {"n_ref": 0, "n_est": 0, "onset": 0, "note": 0, "offset": 0}
        for cond in CONDITIONS
        for ann in ANNOTATORS
    }
    frames = {key: 0 for key in frame_metrics([], 1.0, [])}
    unlabelled = {ann: 0 for ann in ANNOTATORS}
    recordings = []
    for track_id in track_ids:
        audio = root / "Audio" / f"vocadito_{track_id}.wav"
        contour_path = root / "Annotations" / "F0" / f"vocadito_{track_id}_f0.csv"
        output = subprocess.run(
            [str(tool), str(audio), "--reference-f0", str(contour_path)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        export = json.loads(output)
        f0_hz = export["track"]["f0_hz"]
        frame_sec = export["frame_sec"]
        recording: dict[str, Any] = {
            "id": track_id,
            "audio_sha256": sha256(audio),
            "reference_f0_sha256": sha256(contour_path),
        }
        for key, value in frame_metrics(f0_hz, frame_sec, read_contour(contour_path)).items():
            frames[key] += value
        for ann in ANNOTATORS:
            refs = read_reference_notes(
                root / "Annotations" / "Notes" / f"vocadito_{track_id}_notes{ann}.csv"
            )
            labelled = segment_labels(refs, f0_hz, frame_sec)
            unlabelled[ann] += len(refs) - len(labelled)
            estimates = {
                "system": export["notes"],
                "reference_f0": export["oracle_notes"],
                "reference_segments": labelled,
            }
            for cond, ests in estimates.items():
                counts = {
                    kind: matched_count(refs, ests, kind) for kind in ("onset", "note", "offset")
                }
                counts.update(boundary_profile(refs, ests))
                bucket = pooled[f"{cond}/{ann}"]
                bucket["n_ref"] += len(refs)
                bucket["n_est"] += len(ests)
                for kind, value in counts.items():
                    bucket[kind] = bucket.get(kind, 0) + value
                recording[f"{cond}/{ann}"] = {"n_ref": len(refs), "n_est": len(ests), **counts}
        recordings.append(recording)

    summary = {}
    for key, bucket in pooled.items():
        summary[key] = {
            **bucket,
            **{
                f"f_{kind}": f_measure(bucket[kind], bucket["n_ref"], bucket["n_est"])
                for kind in ("onset", "note", "offset")
            },
        }
    voiced = frames["ref_voiced"]
    frame_summary = {
        **frames,
        "voicing_recall": frames["voiced_hits"] / voiced if voiced else None,
        "voicing_false_alarm": (
            frames["false_alarms"] / frames["ref_unvoiced"] if frames["ref_unvoiced"] else None
        ),
        "raw_pitch_accuracy": frames["pitch_hits"] / voiced if voiced else None,
        "raw_chroma_accuracy": frames["chroma_hits"] / voiced if voiced else None,
    }
    return {
        "tool_sha256": sha256(tool),
        "criteria": {
            "onset_tolerance_sec": ONSET_TOLERANCE_SEC,
            "pitch_tolerance_cents": PITCH_TOLERANCE_CENTS,
            "offset_ratio": OFFSET_RATIO,
            "offset_min_sec": OFFSET_MIN_SEC,
        },
        "recordings_scored": len(recordings),
        "summary": summary,
        "frames": frame_summary,
        "reference_segments_without_voiced_frames": unlabelled,
        "recordings": recordings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--vocadito",
        type=Path,
        required=True,
        help="extracted Vocadito root holding Audio/ and Annotations/",
    )
    parser.add_argument("--tool", type=Path, default=Path("build/bin/sonare_eval_note_oracle"))
    parser.add_argument("--tracks", nargs="*", help="track ids; default every audio file")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    track_ids = args.tracks or sorted(
        (path.stem.removeprefix("vocadito_") for path in (args.vocadito / "Audio").glob("*.wav")),
        key=int,
    )
    report = evaluate(args.vocadito, args.tool, track_ids)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for key, row in report["summary"].items():
        print(
            f"{key:28s} n_est={row['n_est']:5d} f_onset={row['f_onset']:.3f} "
            f"f_note={row['f_note']:.3f} f_offset={row['f_offset']:.3f}"
        )
    frames = report["frames"]
    print(
        f"frames: voicing_recall={frames['voicing_recall']:.3f} "
        f"false_alarm={frames['voicing_false_alarm']:.3f} "
        f"rpa={frames['raw_pitch_accuracy']:.3f} rca={frames['raw_chroma_accuracy']:.3f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
