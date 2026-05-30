#!/usr/bin/env python3
"""Synthetic listening/performance gate for realtime voice changer presets."""

from __future__ import annotations

import json
import math
import random
import time

import libsonare


def _tone(sample_rate: int, seconds: float, f0: float, *, gain: float = 0.2) -> list[float]:
    count = int(sample_rate * seconds)
    return [
        gain
        * (
            math.sin(2.0 * math.pi * f0 * i / sample_rate)
            + 0.35 * math.sin(2.0 * math.pi * f0 * 4.0 * i / sample_rate)
            + 0.18 * math.sin(2.0 * math.pi * f0 * 7.0 * i / sample_rate)
        )
        for i in range(count)
    ]


def _fixture_set(sample_rate: int, seconds: float) -> dict[str, list[float]]:
    count = int(sample_rate * seconds)
    rng = random.Random(0x51A2E)
    male = _tone(sample_rate, seconds, 135.0, gain=0.17)
    female = _tone(sample_rate, seconds, 235.0, gain=0.16)
    quiet = _tone(sample_rate, seconds, 170.0, gain=0.035)
    sibilant = _tone(sample_rate, seconds, 180.0, gain=0.12)
    noisy = _tone(sample_rate, seconds, 160.0, gain=0.13)
    plosive = _tone(sample_rate, seconds, 155.0, gain=0.13)

    for i in range(count):
        t = i / sample_rate
        envelope = 0.5 + 0.5 * math.sin(2.0 * math.pi * 0.9 * t)
        male[i] *= 0.65 + 0.35 * envelope
        female[i] *= 0.7 + 0.3 * math.sin(2.0 * math.pi * 1.3 * t + 0.4)
        quiet[i] *= 0.55 + 0.45 * envelope

        if int(t * 5.0) % 2 == 0:
            sibilant[i] += 0.04 * math.sin(2.0 * math.pi * 7200.0 * t)
            sibilant[i] += 0.025 * math.sin(2.0 * math.pi * 8900.0 * t)

        noisy[i] += 0.012 * (2.0 * rng.random() - 1.0)
        noisy[i] += 0.01 * math.sin(2.0 * math.pi * 60.0 * t)

        burst_phase = t % 0.75
        if burst_phase < 0.025:
            plosive[i] += 0.22 * math.exp(-burst_phase * 95.0) * (
                2.0 * rng.random() - 1.0
            )

    return {
        "synthetic-harmonic": _tone(sample_rate, seconds, 160.0),
        "male-voice-like": male,
        "female-voice-like": female,
        "quiet-voice-like": quiet,
        "sibilant-bursts": sibilant,
        "environment-noise": noisy,
        "plosive-bursts": plosive,
    }


def main() -> int:
    sample_rate = 48_000
    block = 128
    seconds = 3
    fixtures = _fixture_set(sample_rate, seconds)

    # Per-preset LUFS bands for the synthetic-harmonic fixture. The bands are
    # centered on the measured integrated LUFS of each preset on the canonical
    # synthetic-harmonic tone (160 Hz, three sinusoidal partials, 3 s at
    # 48 kHz, max block 128) and are ±3 LU wide. The narrow window catches
    # real DSP regressions — envelope follower drift, preset metadata
    # mismatches, output_gain_db typos — instead of just smoke-testing.
    # If a preset's compressor/limiter character changes legitimately, update
    # the center here together with the C++ preset definition.
    per_preset_lufs_band = {
        "neutral-monitor": (-23.5, -17.5),
        "bright-idol": (-24.0, -18.0),
        "soft-whisper": (-22.5, -16.5),
        "deep-narrator": (-23.5, -17.5),
        "robot-mascot": (-23.0, -17.0),
        "dark-villain": (-25.0, -19.0),
    }

    report: dict[str, object] = {"fixtures": {}, "presets": {}}
    harmonic_lufs_by_preset: dict[str, float] = {}
    for preset in libsonare.realtime_voice_changer_preset_names():
        preset_report: dict[str, object] = {}
        harmonic_lufs: float | None = None
        latency = 0
        for fixture_name, samples in fixtures.items():
            start = time.perf_counter()
            with libsonare.RealtimeVoiceChanger(
                sample_rate, preset, max_block_size=block, channels=1
            ) as changer:
                output: list[float] = []
                for pos in range(0, len(samples), block):
                    output.extend(changer.process_mono(samples[pos : pos + block]))
                latency = changer.latency_samples()
            elapsed = time.perf_counter() - start
            integrated = libsonare.lufs(output, sample_rate=sample_rate).integrated_lufs
            peak = max(abs(x) for x in output)
            realtime_ratio = elapsed / seconds
            preset_report[fixture_name] = {
                "integratedLufs": integrated,
                "peak": peak,
                "realtimeRatio": realtime_ratio,
            }
            if fixture_name == "synthetic-harmonic":
                harmonic_lufs = integrated
            if not all(math.isfinite(x) for x in output):
                raise SystemExit(f"{preset}/{fixture_name}: non-finite output")
            if peak > 1.001:
                raise SystemExit(f"{preset}/{fixture_name}: peak ceiling exceeded")
            if realtime_ratio >= 0.25:
                raise SystemExit(f"{preset}/{fixture_name}: realtime CPU budget exceeded")

        assert harmonic_lufs is not None
        harmonic_lufs_by_preset[preset] = harmonic_lufs
        preset_report["latencySamples"] = latency
        report["presets"][preset] = preset_report

        if preset in per_preset_lufs_band and math.isfinite(harmonic_lufs):
            lo, hi = per_preset_lufs_band[preset]
            if not (lo <= harmonic_lufs <= hi):
                raise SystemExit(
                    f"{preset}: synthetic-harmonic LUFS {harmonic_lufs:.2f} is outside the "
                    f"expected band [{lo:.1f}, {hi:.1f}] LU; the preset character has drifted."
                )

    for fixture_name, samples in fixtures.items():
        fixture_peak = max(abs(x) for x in samples)
        report["fixtures"][fixture_name] = {
            "length": len(samples),
            "peak": fixture_peak,
        }

    finite_harmonic = [v for v in harmonic_lufs_by_preset.values() if math.isfinite(v)]
    if finite_harmonic:
        report["lufsSpread"] = max(finite_harmonic) - min(finite_harmonic)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
