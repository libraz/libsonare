"""Run librosa benchmarks and merge with the C++ libsonare results.

All measurements use the same "standalone from raw audio" methodology so the
comparison is fair: every call reconstructs whatever intermediate state it
needs (STFT, Mel, etc.) from the original samples, just as a one-shot user
of the API would experience.

Usage:
    1. Build & run the C++ benchmark first:
         cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCH=ON \
                              -DBUILD_TESTING=OFF -DBUILD_CLI=OFF
         cmake --build build-bench -j
         ./build-bench/bin/sonare_bench benchmarks/fixtures/bench_73s_44100.wav \
                                        benchmarks/results_cpp.json
    2. Run this script to add the librosa side and write benchmarks/results.json.
"""

from __future__ import annotations

import importlib.metadata
import json
import os
import shutil
import statistics
import subprocess
import time
import zlib
from collections.abc import Callable
from pathlib import Path

import librosa
import soundfile

FIXTURE = Path(__file__).parent / "fixtures" / "bench_73s_44100.wav"
CPP_RESULTS = Path(__file__).parent / "results_cpp.json"
MERGED_RESULTS = Path(__file__).parent / "results.json"

RUNS = 3
RESAMPLED_SR = 22050

# A run is treated as contended past this fraction of the machine's threads.
# Matches the threshold sonare_bench applies to the C++ half of the table.
BUSY_LOAD_FRACTION = 0.25


def _load_average() -> float:
    try:
        return os.getloadavg()[0]
    except (OSError, AttributeError):
        return -1.0


# The two resamplers may differ by a sample at the tail; anything wider means a
# different input or a different rate.
RESAMPLED_LENGTH_TOLERANCE = 2


def _fixture_input(resampled_samples: int) -> dict[str, int]:
    """Describes the input this process actually timed, in sonare_bench's `input` shape."""
    raw = FIXTURE.read_bytes()
    info = soundfile.info(str(FIXTURE))
    return {
        "file_bytes": len(raw),
        "file_crc32": zlib.crc32(raw),
        "source_samples": info.frames,
        "source_sample_rate": info.samplerate,
        "resampled_samples": resampled_samples,
        "resampled_sample_rate": RESAMPLED_SR,
    }


def verify_same_input(cpp_input: dict | None, python_input: dict[str, int]) -> None:
    """Refuses to merge results unless both halves timed the same recording."""
    if not isinstance(cpp_input, dict):
        raise SystemExit(
            f"{CPP_RESULTS} has no `input` block; re-run sonare_bench to regenerate it"
        )
    mismatches: list[str] = []
    for key, expected in python_input.items():
        actual = cpp_input.get(key)
        if key == "resampled_samples":
            if not isinstance(actual, int) or abs(actual - expected) > RESAMPLED_LENGTH_TOLERANCE:
                mismatches.append(f"{key}: sonare_bench={actual} librosa side={expected}")
        elif actual != expected:
            mismatches.append(f"{key}: sonare_bench={actual} librosa side={expected}")
    if mismatches:
        raise SystemExit(
            "sonare_bench and the librosa side did not use the same input:\n  "
            + "\n  ".join(mismatches)
        )


def _bench(fn: Callable[[], object], runs: int = RUNS) -> float:
    times_ms: list[float] = []
    for _ in range(runs):
        t0 = time.perf_counter()
        fn()
        times_ms.append((time.perf_counter() - t0) * 1000)
    return statistics.median(times_ms)


def main() -> None:
    if not FIXTURE.exists():
        raise SystemExit(
            f"missing fixture {FIXTURE}; run `python generate_audio.py` first"
        )
    if not CPP_RESULTS.exists():
        raise SystemExit(
            f"missing {CPP_RESULTS}; build and run sonare_bench first (see module docstring)"
        )

    cpp = json.loads(CPP_RESULTS.read_text())
    cpp_cached = {row["feature"]: row["libsonare_ms"] for row in cpp["per_feature_cached"]}
    libsonare_full_ms = cpp["full_analysis_ms"]
    libsonare_per_feature = {row["feature"]: row["libsonare_ms"]
                             for row in cpp["per_feature_standalone"]}

    y, _ = librosa.load(str(FIXTURE), sr=RESAMPLED_SR, mono=True)
    verify_same_input(cpp.get("input"), _fixture_input(len(y)))
    print(f"fixture: {FIXTURE.name} (resampled to {RESAMPLED_SR} Hz, {len(y)} samples)")
    print(f"runs per case: {RUNS}")

    hardware_threads = os.cpu_count() or 0
    load_before = _load_average()
    print(f"load average before: {load_before:.2f} ({hardware_threads} hardware threads)")
    if hardware_threads > 0 and load_before > hardware_threads * BUSY_LOAD_FRACTION:
        print(
            "WARNING: the machine is already busy. librosa's HPSS and pYIN are the\n"
            "         cases that inflate most under contention, which quietly changes\n"
            "         the published speedups. Do not publish this run."
        )
    print()

    cases: dict[str, Callable[[], object]] = {
        "STFT": lambda: librosa.stft(y, n_fft=2048, hop_length=512),
        "Mel Spectrogram": lambda: librosa.feature.melspectrogram(
            y=y, sr=RESAMPLED_SR, n_fft=2048, hop_length=512, n_mels=128),
        "HPSS": lambda: librosa.effects.hpss(y),
        "Onset Strength": lambda: librosa.onset.onset_strength(y=y, sr=RESAMPLED_SR),
        "Chroma": lambda: librosa.feature.chroma_stft(
            y=y, sr=RESAMPLED_SR, n_fft=2048, hop_length=512),
        "Beat Track": lambda: librosa.beat.beat_track(y=y, sr=RESAMPLED_SR),
        "MFCC": lambda: librosa.feature.mfcc(
            y=y, sr=RESAMPLED_SR, n_fft=2048, hop_length=512, n_mfcc=13),
        "pYIN": lambda: librosa.pyin(y, fmin=65.0, fmax=2093.0, sr=RESAMPLED_SR),
        "Spectral Centroid": lambda: librosa.feature.spectral_centroid(
            y=y, sr=RESAMPLED_SR, n_fft=2048, hop_length=512),
    }

    print(f"{'Feature':<20s} {'librosa (ms)':>14s} {'libsonare (ms)':>16s} {'speedup':>10s}")
    print("-" * 64)
    per_feature: list[dict[str, float | str]] = []
    for label, fn in cases.items():
        los_ms = _bench(fn)
        lib_ms = libsonare_per_feature[label]
        speedup = los_ms / lib_ms if lib_ms > 0 else float("inf")
        per_feature.append({
            "feature": label,
            "librosa_ms": round(los_ms, 2),
            "libsonare_ms": round(lib_ms, 2),
            **(
                {"libsonare_cached_ms": round(cpp_cached[label], 2)}
                if label in cpp_cached else {}
            ),
            "speedup": round(speedup, 2),
        })
        print(f"{label:<20s} {los_ms:>14.2f} {lib_ms:>16.2f} {speedup:>9.2f}x")

    print(f"\nFull analysis (libsonare native C++): {libsonare_full_ms:.0f} ms")

    bpm_detector_ms: float | None = None
    bpm_detector_version: str | None = None
    if shutil.which("bpm-detector"):
        print("found bpm-detector — running --comprehensive for an apples-to-apples "
              "full-pipeline comparison...")
        # Which build produced the comparison number decides whether anyone can
        # check it later, so the version goes into results.json rather than
        # being described in prose on the homepage. The tool has no --version
        # flag, so this comes from the installed distribution metadata.
        try:
            bpm_detector_version = importlib.metadata.version("bpm-detector")
        except importlib.metadata.PackageNotFoundError:
            bpm_detector_version = None
        t0 = time.perf_counter()
        subprocess.run(
            ["bpm-detector", "--comprehensive", "--quiet", str(FIXTURE)],
            check=True, capture_output=True,
        )
        bpm_detector_ms = (time.perf_counter() - t0) * 1000
        print(f"bpm-detector --comprehensive: {bpm_detector_ms / 1000:.1f}s "
              f"({bpm_detector_ms / libsonare_full_ms:.1f}x slower than libsonare)")

    MERGED_RESULTS.write_text(json.dumps({
        "fixture": FIXTURE.name,
        "duration_sec": cpp["duration_sec"],
        "source_sample_rate": cpp["source_sample_rate"],
        "resampled_sample_rate": RESAMPLED_SR,
        "runs_per_case": RUNS,
        "methodology": (
            "Both libraries measured 'standalone from raw audio': every "
            "per-feature call reconstructs STFT/Mel from the resampled samples. "
            "libsonare uses chrono::steady_clock inside C++; librosa uses "
            "time.perf_counter around the Python call."
        ),
        "librosa_version": librosa.__version__,
        "hardware_threads": hardware_threads,
        "load_average_before": round(load_before, 2),
        "load_average_after": round(_load_average(), 2),
        "input": cpp["input"],
        "measurement": {
            "librosa": {
                "hardware_threads": hardware_threads,
                "load_average_before": round(load_before, 2),
                "load_average_after": round(_load_average(), 2),
                "runs_per_case": RUNS,
                "timing_source": "time.perf_counter (Python call)",
            },
            "libsonare": {
                "hardware_threads": cpp["hardware_threads"],
                "load_average_before": cpp["load_average_before"],
                "load_average_after": cpp["load_average_after"],
                "runs_per_case": cpp["runs_per_case"],
                "timing_source": cpp["timing_source"],
            },
        },
        "full_analysis": {
            "libsonare_ms": round(libsonare_full_ms, 2),
            "libsonare_with_cancel_callback_ms": round(
                cpp["full_analysis_with_cancel_callback_ms"], 2
            ),
            "bpm_detector_comprehensive_ms": (
                round(bpm_detector_ms, 2) if bpm_detector_ms is not None else None
            ),
            "bpm_detector_speedup": (
                round(bpm_detector_ms / libsonare_full_ms, 2)
                if bpm_detector_ms is not None else None
            ),
            "bpm_detector_version": bpm_detector_version,
            "note": (
                "Full pipeline (BPM + key + beats + chords + sections + timbre + dynamics) "
                "running entirely inside libsonare C++. The comparison target is "
                "bpm-detector --comprehensive: a pipeline built on librosa, not librosa "
                "itself. It is the same author's earlier Python project, chosen because it "
                "computes the same set of features end to end; librosa alone has no "
                "equivalent one-shot pipeline to time. Ratios against a different "
                "librosa-based pipeline will differ."
            ),
        },
        "per_feature": per_feature,
    }, indent=2))
    print(f"\nwrote {MERGED_RESULTS}")


if __name__ == "__main__":
    main()
