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

import json
import shutil
import statistics
import subprocess
import time
from pathlib import Path
from typing import Callable

import librosa

FIXTURE = Path(__file__).parent / "fixtures" / "bench_73s_44100.wav"
CPP_RESULTS = Path(__file__).parent / "results_cpp.json"
MERGED_RESULTS = Path(__file__).parent / "results.json"

RUNS = 3
RESAMPLED_SR = 22050


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
    libsonare_full_ms = cpp["full_analysis_ms"]
    libsonare_per_feature = {row["feature"]: row["libsonare_ms"]
                             for row in cpp["per_feature_standalone"]}

    y, _ = librosa.load(str(FIXTURE), sr=RESAMPLED_SR, mono=True)
    print(f"fixture: {FIXTURE.name} (resampled to {RESAMPLED_SR} Hz, {len(y)} samples)")
    print(f"runs per case: {RUNS}\n")

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
            "speedup": round(speedup, 2),
        })
        print(f"{label:<20s} {los_ms:>14.2f} {lib_ms:>16.2f} {speedup:>9.2f}x")

    print(f"\nFull analysis (libsonare native C++): {libsonare_full_ms:.0f} ms")

    bpm_detector_ms: float | None = None
    if shutil.which("bpm-detector"):
        print("found bpm-detector — running --comprehensive for an apples-to-apples "
              "full-pipeline comparison...")
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
        "full_analysis": {
            "libsonare_ms": round(libsonare_full_ms, 2),
            "bpm_detector_comprehensive_ms": (
                round(bpm_detector_ms, 2) if bpm_detector_ms is not None else None
            ),
            "bpm_detector_speedup": (
                round(bpm_detector_ms / libsonare_full_ms, 2)
                if bpm_detector_ms is not None else None
            ),
            "note": (
                "Full pipeline (BPM + key + beats + chords + sections + timbre + dynamics) "
                "running entirely inside libsonare C++. The closest librosa-based equivalent "
                "is bpm-detector --comprehensive."
            ),
        },
        "per_feature": per_feature,
    }, indent=2))
    print(f"\nwrote {MERGED_RESULTS}")


if __name__ == "__main__":
    main()
