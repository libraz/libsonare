"""Generate a synthetic 73-second test audio file at 44100 Hz stereo.

The signal is intentionally musical-ish: a slow chord progression with
percussive bursts, so analyze() and feature extractors have meaningful
content to chew on. The file is deterministic (fixed RNG seed) and the
duration / sample rate match the figures published in the homepage docs.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

SR = 44100
DURATION_SEC = 73
SEED = 1729

OUT_PATH = Path(__file__).parent / "fixtures" / "bench_73s_44100.wav"


def _chord_progression(t: np.ndarray) -> np.ndarray:
    progression_hz = [
        (220.0, 261.63, 329.63),
        (196.0, 246.94, 293.66),
        (174.61, 220.0, 261.63),
        (164.81, 196.0, 246.94),
    ]
    bar_sec = DURATION_SEC / (len(progression_hz) * 4)
    out = np.zeros_like(t)
    for bar in range(len(progression_hz) * 4):
        chord = progression_hz[bar % len(progression_hz)]
        start = bar * bar_sec
        end = start + bar_sec
        mask = (t >= start) & (t < end)
        seg = np.zeros_like(t[mask])
        for f in chord:
            seg += 0.18 * np.sin(2 * np.pi * f * t[mask])
            seg += 0.06 * np.sin(2 * np.pi * 2 * f * t[mask])
        env = np.linspace(1.0, 0.6, seg.size)
        out[mask] = seg * env
    return out


def _drum_track(rng: np.random.Generator, n_samples: int) -> np.ndarray:
    track = np.zeros(n_samples, dtype=np.float64)
    beat_period = int(SR * 60 / 120)
    burst = int(SR * 0.05)
    env = np.exp(-np.linspace(0, 6, burst))
    for i in range(0, n_samples - burst, beat_period):
        noise = rng.standard_normal(burst) * env * 0.35
        track[i : i + burst] += noise
    return track


def main() -> None:
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    if OUT_PATH.exists():
        print(f"[skip] {OUT_PATH} already exists")
        return

    rng = np.random.default_rng(SEED)
    n_samples = SR * DURATION_SEC
    t = np.linspace(0, DURATION_SEC, n_samples, endpoint=False)

    mono = _chord_progression(t) + _drum_track(rng, n_samples)
    mono = mono / np.max(np.abs(mono)) * 0.85
    stereo = np.stack([mono, mono * 0.92 + rng.standard_normal(n_samples) * 0.002], axis=-1)

    sf.write(OUT_PATH, stereo.astype(np.float32), SR, subtype="PCM_16")
    print(f"[ok] wrote {OUT_PATH} ({n_samples} samples, {DURATION_SEC}s @ {SR} Hz stereo)")


if __name__ == "__main__":
    main()
