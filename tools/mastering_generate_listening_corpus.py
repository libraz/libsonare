#!/usr/bin/env python3
"""Generate deterministic mastering listening fixtures.

The files are intentionally synthetic so they can be regenerated without
shipping licensed audio.  They cover transients, clipped material, hum/noise,
stereo width, and a dense full-mix-like bed for before/after processor checks.
"""

from __future__ import annotations

import argparse
import math
import struct
import wave
from pathlib import Path


SAMPLE_RATE = 48_000
DURATION_SECONDS = 4.0


def clamp(value: float) -> float:
    return max(-1.0, min(1.0, value))


def write_wav(path: Path, frames: list[tuple[float, float]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        data = bytearray()
        for left, right in frames:
            data += struct.pack("<hh", int(clamp(left) * 32767), int(clamp(right) * 32767))
        wav.writeframes(bytes(data))


def envelope(t: float, attack: float, release: float) -> float:
    if t < attack:
        return t / max(attack, 1e-6)
    return math.exp(-(t - attack) / max(release, 1e-6))


def transient_fixture() -> list[tuple[float, float]]:
    frames: list[tuple[float, float]] = []
    for n in range(int(SAMPLE_RATE * DURATION_SECONDS)):
        t = n / SAMPLE_RATE
        beat = t % 0.5
        kick = envelope(beat, 0.002, 0.08) * math.sin(2.0 * math.pi * (52.0 + 80.0 * math.exp(-beat * 40.0)) * t)
        hat = 0.12 * envelope((t + 0.125) % 0.25, 0.001, 0.018) * math.sin(2.0 * math.pi * 8400.0 * t)
        left = 0.9 * kick + hat
        right = 0.86 * kick - hat
        frames.append((left, right))
    return frames


def clipped_fixture() -> list[tuple[float, float]]:
    frames: list[tuple[float, float]] = []
    for n in range(int(SAMPLE_RATE * DURATION_SECONDS)):
        t = n / SAMPLE_RATE
        dry = 1.35 * math.sin(2.0 * math.pi * 997.0 * t) + 0.32 * math.sin(2.0 * math.pi * 2991.0 * t)
        clipped = clamp(dry * 0.82)
        frames.append((clipped, -0.96 * clipped))
    return frames


def noise_hum_fixture() -> list[tuple[float, float]]:
    frames: list[tuple[float, float]] = []
    state = 0x12345678
    for n in range(int(SAMPLE_RATE * DURATION_SECONDS)):
        t = n / SAMPLE_RATE
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        white = ((state / 0xFFFFFFFF) * 2.0 - 1.0) * 0.04
        tone = 0.32 * math.sin(2.0 * math.pi * 220.0 * t) * (0.6 + 0.4 * math.sin(2.0 * math.pi * 0.35 * t))
        hum = 0.11 * math.sin(2.0 * math.pi * 60.0 * t) + 0.04 * math.sin(2.0 * math.pi * 120.0 * t)
        frames.append((tone + hum + white, tone * 0.92 + hum - white * 0.7))
    return frames


def stereo_mix_fixture() -> list[tuple[float, float]]:
    frames: list[tuple[float, float]] = []
    for n in range(int(SAMPLE_RATE * DURATION_SECONDS)):
        t = n / SAMPLE_RATE
        bass = 0.36 * math.sin(2.0 * math.pi * 74.0 * t)
        pad_l = 0.18 * math.sin(2.0 * math.pi * 330.0 * t + 0.4 * math.sin(2.0 * math.pi * 0.3 * t))
        pad_r = 0.18 * math.sin(2.0 * math.pi * 333.0 * t - 0.4 * math.sin(2.0 * math.pi * 0.27 * t))
        lead = 0.2 * math.sin(2.0 * math.pi * 880.0 * t) * (0.7 + 0.3 * math.sin(2.0 * math.pi * 4.0 * t))
        frames.append((bass + pad_l + lead, bass + pad_r - 0.35 * lead))
    return frames


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="tests/mastering/audio")
    args = parser.parse_args()

    out = Path(args.output_dir)
    fixtures = {
        "transients_dry.wav": transient_fixture(),
        "clipped_tone_dry.wav": clipped_fixture(),
        "noise_hum_dry.wav": noise_hum_fixture(),
        "stereo_mix_dry.wav": stereo_mix_fixture(),
    }
    for name, frames in fixtures.items():
        write_wav(out / name, frames)
        print(out / name)


if __name__ == "__main__":
    main()
