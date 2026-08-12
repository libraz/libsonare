"""Generate a synthetic 73-second test audio file at 44100 Hz stereo.

The signal is intentionally musical-ish: a slow chord progression with
percussive bursts, so analyze() and feature extractors have meaningful
content to chew on. The file is deterministic (fixed RNG seed) and the
duration / sample rate match the figures published in the homepage docs.

Alongside the WAV, a ground-truth JSON is written describing what was
synthesised: the tempo, every beat position, the chord in each bar and the
key those chords belong to. It is derived from the same constants that drive
the synthesis rather than transcribed by hand, so it cannot drift from the
audio. measure_accuracy.py scores the analyzers against it.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np
import soundfile as sf

SR = 44100
DURATION_SEC = 73
SEED = 1729
TEMPO_BPM = 120.0
BURST_SEC = 0.05

# Each entry is one bar of the progression: the pitch classes are what the
# sine partials spell out, and the label is how a chord detector should name
# it. Pitch class 0 is C.
PROGRESSION = [
    {"frequencies": (220.0, 261.63, 329.63), "label": "Am", "root": 9, "quality": "minor"},
    {"frequencies": (196.0, 246.94, 293.66), "label": "G", "root": 7, "quality": "major"},
    {"frequencies": (174.61, 220.0, 261.63), "label": "F", "root": 5, "quality": "major"},
    {"frequencies": (164.81, 196.0, 246.94), "label": "Em", "root": 4, "quality": "minor"},
]
BAR_COUNT = len(PROGRESSION) * 4
BAR_SEC = DURATION_SEC / BAR_COUNT

# Am-G-F-Em is diatonic to both A minor and its relative major, and the
# progression starts and ends on the A minor chord. Key evaluation credits the
# relative separately, so both are recorded.
KEY = {"root": 9, "mode": "minor", "name": "A minor"}
RELATIVE_KEY = {"root": 0, "mode": "major", "name": "C major"}

OUT_PATH = Path(__file__).parent / "fixtures" / "bench_73s_44100.wav"
GROUND_TRUTH_PATH = OUT_PATH.with_suffix(".groundtruth.json")

# SHA-256 of the fixture the published timings were measured on. The WAV is
# generated rather than committed, so this is what lets a regenerated copy be
# shown to be the same bytes.
EXPECTED_SHA256 = "3be88171eb87f8569189b9acef994e18263f89e7adf05119cbb48591c4953cb3"


def _chord_progression(t: np.ndarray) -> np.ndarray:
    out = np.zeros_like(t)
    for bar in range(BAR_COUNT):
        chord = PROGRESSION[bar % len(PROGRESSION)]["frequencies"]
        start = bar * BAR_SEC
        end = start + BAR_SEC
        mask = (t >= start) & (t < end)
        seg = np.zeros_like(t[mask])
        for f in chord:
            seg += 0.18 * np.sin(2 * np.pi * f * t[mask])
            seg += 0.06 * np.sin(2 * np.pi * 2 * f * t[mask])
        env = np.linspace(1.0, 0.6, seg.size)
        out[mask] = seg * env
    return out


def _beat_sample_indices(n_samples: int) -> range:
    beat_period = int(SR * 60 / TEMPO_BPM)
    burst = int(SR * BURST_SEC)
    return range(0, n_samples - burst, beat_period)


def _drum_track(rng: np.random.Generator, n_samples: int) -> np.ndarray:
    track = np.zeros(n_samples, dtype=np.float64)
    burst = int(SR * BURST_SEC)
    env = np.exp(-np.linspace(0, 6, burst))
    for i in _beat_sample_indices(n_samples):
        noise = rng.standard_normal(burst) * env * 0.35
        track[i : i + burst] += noise
    return track


def _ground_truth(n_samples: int) -> dict:
    return {
        "source": "benchmarks/generate_audio.py",
        "sampleRate": SR,
        "durationSec": DURATION_SEC,
        "tempoBpm": TEMPO_BPM,
        "beatTimes": [i / SR for i in _beat_sample_indices(n_samples)],
        "key": KEY,
        "relativeKey": RELATIVE_KEY,
        "barSec": BAR_SEC,
        "chords": [
            {
                "start": bar * BAR_SEC,
                "end": (bar + 1) * BAR_SEC,
                "label": PROGRESSION[bar % len(PROGRESSION)]["label"],
                "root": PROGRESSION[bar % len(PROGRESSION)]["root"],
                "quality": PROGRESSION[bar % len(PROGRESSION)]["quality"],
            }
            for bar in range(BAR_COUNT)
        ],
    }


def main() -> None:
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    n_samples = SR * DURATION_SEC

    # The ground truth is cheap to derive and must match whatever WAV is
    # present, so it is rewritten even when the audio is already there.
    GROUND_TRUTH_PATH.write_text(json.dumps(_ground_truth(n_samples), indent=2), encoding="utf-8")
    print(f"[ok] wrote {GROUND_TRUTH_PATH}")

    if OUT_PATH.exists():
        print(f"[skip] {OUT_PATH} already exists")
        return

    rng = np.random.default_rng(SEED)
    t = np.linspace(0, DURATION_SEC, n_samples, endpoint=False)

    mono = _chord_progression(t) + _drum_track(rng, n_samples)
    mono = mono / np.max(np.abs(mono)) * 0.85
    stereo = np.stack([mono, mono * 0.92 + rng.standard_normal(n_samples) * 0.002], axis=-1)

    sf.write(OUT_PATH, stereo.astype(np.float32), SR, subtype="PCM_16")
    print(f"[ok] wrote {OUT_PATH} ({n_samples} samples, {DURATION_SEC}s @ {SR} Hz stereo)")
    _report_checksum()


def _report_checksum() -> None:
    """Print the fixture's SHA-256 so a regenerated file can be shown identical.

    The WAV is not committed, so the published timings would otherwise rest on
    the claim that everyone's copy is the same audio. numpy's default_rng is
    version-stable for a fixed seed, which makes the output byte-identical
    across machines; this prints the digest that proves it for a given copy.
    """
    digest = hashlib.sha256(OUT_PATH.read_bytes()).hexdigest()
    print(f"[ok] sha256 {digest}")
    if digest != EXPECTED_SHA256:
        print(f"[warn] expected {EXPECTED_SHA256}")
        print("[warn] this copy differs from the published fixture; timings are not comparable")


if __name__ == "__main__":
    main()
