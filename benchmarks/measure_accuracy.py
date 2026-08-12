"""Score the analyzers against the synthetic fixture's known ground truth.

The benchmark page publishes how fast analysis runs. Speed on its own says
nothing about whether the answers are right, so this script scores the same
fixture the timings use. The audio is synthesised by generate_audio.py from an
explicit tempo, beat grid, chord progression and key, and that description is
written out beside the WAV — so unlike a real recording, this fixture comes
with an unarguable answer key.

What is scored:

  tempo   absolute and relative error against the synthesised BPM, plus the
          MIREX convention of counting an estimate correct within 4%.
  beats   F-measure against the beat grid with the standard +/-70 ms window.
          Reported both as-is and after removing a constant offset, since a
          tracker that is right about the pulse but latches half a frame late
          is a different failure from one that is off the grid.
  chords  frame-wise agreement at 10 ms resolution, scored twice: on the root
          alone, and on root plus triad quality.
  key     exact match, with the relative major/minor counted separately
          because the progression is diatonic to both.

Read the result as a floor, not a benchmark. Synthetic audio has no
performance timing, no timbral ambiguity and no production; passing here means
the analyzers recover a signal that was constructed to be recoverable. It does
not predict accuracy on real recordings, which needs an annotated corpus of
real music this repository does not ship.

Usage:
    python3 benchmarks/measure_accuracy.py [--cli build-release/bin/sonare]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CLI = REPOSITORY_ROOT / "build-release" / "bin" / "sonare"
DEFAULT_FIXTURE = REPOSITORY_ROOT / "benchmarks" / "fixtures" / "bench_73s_44100.wav"

# The window mir_eval uses for beat tracking. A beat is credited when it lands
# within this distance of a reference beat.
BEAT_TOLERANCE_SEC = 0.070
# MIREX counts a tempo estimate correct inside this relative error.
TEMPO_TOLERANCE = 0.04
# Chord agreement is sampled rather than segment-matched, so a boundary that is
# a few milliseconds early costs a few frames instead of a whole segment.
CHORD_FRAME_SEC = 0.010

PITCH_CLASS_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
PITCH_CLASS_INDEX = {
    "C": 0, "B#": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "Fb": 4,
    "F": 5, "E#": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10,
    "Bb": 10, "B": 11, "Cb": 11,
}


def run_cli(cli: Path, command: str, fixture: Path) -> Any:
    """Invoke one CLI subcommand and return its parsed JSON output."""
    result = subprocess.run(
        [str(cli), command, str(fixture), "--json", "--quiet"],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def chord_quality(name: str) -> str:
    """Reduce a chord label to the triad quality a root+quality score uses.

    Extensions are deliberately collapsed: calling the Am bar "Amadd9" is a
    different kind of answer from calling it G major, and scoring them the same
    would hide which one happened.
    """
    body = re.sub(r"^[A-G][#b]?", "", name)
    body = body.split("/")[0]
    if body.startswith(("m", "min")) and not body.startswith("maj"):
        return "minor"
    if body.startswith("dim"):
        return "diminished"
    if body.startswith("aug"):
        return "augmented"
    return "major"


def chord_root(chord: dict) -> int | None:
    if isinstance(chord.get("root"), int):
        return chord["root"]
    match = re.match(r"^([A-G][#b]?)", chord.get("name", ""))
    return PITCH_CLASS_INDEX.get(match.group(1)) if match else None


def beat_f_measure(reference: list[float], estimated: list[float], offset: float = 0.0) -> dict:
    """Greedy one-to-one match inside the tolerance window, as mir_eval does."""
    shifted = sorted(t - offset for t in estimated)
    unused = list(reference)
    matched = 0
    for beat in shifted:
        best_index, best_distance = None, BEAT_TOLERANCE_SEC
        for index, ref in enumerate(unused):
            distance = abs(ref - beat)
            if distance <= best_distance:
                best_index, best_distance = index, distance
        if best_index is not None:
            matched += 1
            unused.pop(best_index)
    precision = matched / len(shifted) if shifted else 0.0
    recall = matched / len(reference) if reference else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {"precision": precision, "recall": recall, "f1": f1, "matched": matched}


def median_offset(reference: list[float], estimated: list[float]) -> float:
    """Median signed distance from each estimate to its nearest reference beat."""
    if not reference or not estimated:
        return 0.0
    deltas = sorted(beat - min(reference, key=lambda r: abs(r - beat)) for beat in estimated)
    middle = len(deltas) // 2
    if len(deltas) % 2 == 1:
        return deltas[middle]
    return (deltas[middle - 1] + deltas[middle]) * 0.5


def label_at(segments: list[dict], time: float) -> dict | None:
    for segment in segments:
        if segment["start"] <= time < segment["end"]:
            return segment
    return None


def score_chords(truth: list[dict], estimated: list[dict], duration: float) -> dict:
    frames = int(duration / CHORD_FRAME_SEC)
    root_hits = quality_hits = scored = 0
    for frame in range(frames):
        time = frame * CHORD_FRAME_SEC
        reference = label_at(truth, time)
        prediction = label_at(estimated, time)
        if reference is None:
            continue
        scored += 1
        if prediction is None:
            continue
        if chord_root(prediction) == reference["root"]:
            root_hits += 1
            if chord_quality(prediction["name"]) == reference["quality"]:
                quality_hits += 1
    return {
        "framesScored": scored,
        "rootAccuracy": root_hits / scored if scored else 0.0,
        "rootAndQualityAccuracy": quality_hits / scored if scored else 0.0,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--json-output", type=Path, default=None)
    args = parser.parse_args()

    truth_path = args.fixture.with_suffix(".groundtruth.json")
    if not args.cli.exists():
        print(f"CLI not found at {args.cli}. Build it with BUILD_CLI=ON first.", file=sys.stderr)
        sys.exit(1)
    if not truth_path.exists():
        print(f"Ground truth not found at {truth_path}.", file=sys.stderr)
        print("Run: python3 benchmarks/generate_audio.py", file=sys.stderr)
        sys.exit(1)

    truth = json.loads(truth_path.read_text(encoding="utf-8"))
    analysis = run_cli(args.cli, "analyze", args.fixture)
    standalone_key = run_cli(args.cli, "key", args.fixture)
    standalone_chords = run_cli(args.cli, "chords", args.fixture)

    # Tempo.
    tempo_error = abs(analysis["bpm"] - truth["tempoBpm"])
    tempo_relative = tempo_error / truth["tempoBpm"]

    # Beats. analyze() reports beats as objects; the standalone command emits
    # bare times.
    estimated_beats = [b["time"] if isinstance(b, dict) else b for b in analysis["beats"]]
    offset = median_offset(truth["beatTimes"], estimated_beats)
    beats_raw = beat_f_measure(truth["beatTimes"], estimated_beats)
    beats_aligned = beat_f_measure(truth["beatTimes"], estimated_beats, offset)

    # Key, from both the pipeline and the standalone detector — they run
    # different code and are allowed to disagree, so both are reported.
    def score_key(result: dict) -> str:
        if result.get("root") == truth["key"]["root"] and "minor" in result.get("name", ""):
            return "exact"
        if result.get("root") == truth["relativeKey"]["root"] and "major" in result.get("name", ""):
            return "relative"
        return "wrong"

    pipeline_key = score_key(analysis["key"])
    detector_key = score_key(standalone_key)

    duration = truth["durationSec"]
    chords_pipeline = score_chords(truth["chords"], analysis["chords"], duration)
    chords_standalone = score_chords(truth["chords"], standalone_chords["chords"], duration)

    print(f"Fixture     : {args.fixture.name} ({duration}s, synthetic)")
    print(f"Ground truth: {truth_path.name}")
    print()
    print("Tempo")
    print(f"  reference        : {truth['tempoBpm']:.2f} BPM")
    print(f"  estimated        : {analysis['bpm']:.2f} BPM")
    print(f"  error            : {tempo_error:.2f} BPM ({tempo_relative * 100:.2f}%)")
    print(f"  within 4% (MIREX): {'yes' if tempo_relative <= TEMPO_TOLERANCE else 'no'}")
    print()
    print(f"Beats (+/-{BEAT_TOLERANCE_SEC * 1000:.0f} ms window, {len(truth['beatTimes'])} reference beats)")
    print(f"  detected         : {len(estimated_beats)}")
    print(f"  F-measure        : {beats_raw['f1']:.4f}"
          f"  (P {beats_raw['precision']:.4f} / R {beats_raw['recall']:.4f})")
    print(f"  median offset    : {offset * 1000:+.1f} ms")
    print(f"  F after offset   : {beats_aligned['f1']:.4f}"
          f"  (P {beats_aligned['precision']:.4f} / R {beats_aligned['recall']:.4f})")
    print()
    print("Key")
    print(f"  reference        : {truth['key']['name']} (relative {truth['relativeKey']['name']})")
    print(f"  analyze()        : {analysis['key']['name']} -> {pipeline_key}")
    print(f"  key command      : {standalone_key['name']} -> {detector_key}")
    print()
    print(f"Chords (frame-wise at {CHORD_FRAME_SEC * 1000:.0f} ms)")
    print(f"  analyze()   root : {chords_pipeline['rootAccuracy']:.4f}"
          f"   root+quality: {chords_pipeline['rootAndQualityAccuracy']:.4f}")
    print(f"  chords cmd  root : {chords_standalone['rootAccuracy']:.4f}"
          f"   root+quality: {chords_standalone['rootAndQualityAccuracy']:.4f}")
    print()
    print("The fixture is synthetic: fixed tempo, sustained triads, no performance")
    print("timing and no production. These scores say the analyzers recover a signal")
    print("built to be recoverable, which is a floor rather than a benchmark. Accuracy")
    print("on real recordings needs an annotated corpus this repository does not ship.")

    if args.json_output:
        args.json_output.write_text(
            json.dumps(
                {
                    "fixture": args.fixture.name,
                    "durationSec": duration,
                    "tempo": {
                        "referenceBpm": truth["tempoBpm"],
                        "estimatedBpm": analysis["bpm"],
                        "absoluteErrorBpm": tempo_error,
                        "relativeError": tempo_relative,
                        "withinMirexTolerance": tempo_relative <= TEMPO_TOLERANCE,
                    },
                    "beats": {
                        "referenceCount": len(truth["beatTimes"]),
                        "estimatedCount": len(estimated_beats),
                        "toleranceSec": BEAT_TOLERANCE_SEC,
                        "raw": beats_raw,
                        "medianOffsetSec": offset,
                        "offsetCorrected": beats_aligned,
                    },
                    "key": {
                        "reference": truth["key"]["name"],
                        "relative": truth["relativeKey"]["name"],
                        "pipeline": {"name": analysis["key"]["name"], "verdict": pipeline_key},
                        "standalone": {"name": standalone_key["name"], "verdict": detector_key},
                    },
                    "chords": {"pipeline": chords_pipeline, "standalone": chords_standalone},
                },
                indent=2,
            ),
            encoding="utf-8",
        )


if __name__ == "__main__":
    main()
