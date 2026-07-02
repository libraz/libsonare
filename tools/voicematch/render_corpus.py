#!/usr/bin/env python3
"""Render a GM listening/metrics corpus through the SF2-less fallback path.

For each representative GM program this builds an idiomatic short phrase,
imports it as an SMF into a libsonare project, bounces it through
``bounce_with_sf2_instrument`` with NO SoundFont loaded (so every program
plays through the NativeSynth GM fallback bank — the path under improvement),
writes a WAV, and prints objective per-file metrics:

  rms / peak / crest, stereo width (1 - |L,R correlation|), spectral centroid
  mean and coefficient of variation (timbral movement), 85% rolloff, attack
  time, release tail length, and harmonic partial count at the phrase's
  sustained midpoint.

Usage (from repo root):
  rye run --pyproject bindings/python/pyproject.toml \
      python tools/voicematch/render_corpus.py [--out DIR] [--programs 0,40,56]

A reference render per program can be added next to the model render with
``--fluidsynth path/to/soundfont.sf2`` (uses the fluidsynth CLI).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "bindings" / "python" / "src"))

from smf import Note, write_smf  # noqa: E402

import libsonare  # noqa: E402

SR = 48000

# ---------------------------------------------------------------------------
# Idiomatic phrases per instrument family (notes chosen mid-register).
# ---------------------------------------------------------------------------


def _sustained(note: int, dur: float = 2.5, vel: int = 96) -> list[Note]:
    return [Note(note, vel, 0.1, dur)]


def _melody(base: int, vel: int = 96) -> list[Note]:
    steps = [0, 2, 4, 7, 4, 2, 0]
    return [Note(base + s, vel, 0.1 + i * 0.35, 0.32) for i, s in enumerate(steps)] + [
        Note(base + 7, vel, 0.1 + len(steps) * 0.35, 1.6)
    ]


def _chord(base: int, dur: float = 2.5, vel: int = 90) -> list[Note]:
    return [Note(base + iv, vel, 0.1, dur) for iv in (0, 4, 7, 12)]


def _arpeggio(base: int, vel: int = 100) -> list[Note]:
    ivs = [0, 4, 7, 12, 16, 12, 7, 4]
    return [Note(base + iv, vel, 0.1 + i * 0.22, 1.2) for i, iv in enumerate(ivs)]


def _velocity_ramp(note: int) -> list[Note]:
    return [Note(note, v, 0.1 + i * 0.5, 0.4) for i, v in enumerate((32, 64, 96, 127))]


def _bass_line(base: int) -> list[Note]:
    seq = [(0, 0.0), (0, 0.5), (7, 1.0), (5, 1.5), (0, 2.0), (3, 2.5)]
    return [Note(base + iv, 100, 0.1 + t, 0.4) for iv, t in seq]


def _drum_pattern() -> list[Note]:
    ev: list[Note] = []
    for beat in range(8):
        t = 0.1 + beat * 0.25
        if beat % 2 == 0:
            ev.append(Note(36, 110, t, 0.1))  # kick
        else:
            ev.append(Note(38, 100, t, 0.1))  # snare
        ev.append(Note(42, 80, t, 0.05))  # closed hat
    ev.append(Note(49, 105, 2.1, 0.1))  # crash
    return ev


# (program, slug, note list, is_drums)
CORPUS: list[tuple[int, str, list[Note], bool]] = [
    (0, "acoustic-grand", _arpeggio(60) + _chord(48, dur=2.0), False),
    (0, "grand-velocity", _velocity_ramp(60), False),
    (11, "vibraphone", _arpeggio(72), False),
    (19, "church-organ", _chord(48, dur=3.0), False),
    (24, "nylon-guitar", _arpeggio(52), False),
    (27, "clean-electric", _arpeggio(52), False),
    (33, "finger-bass", _bass_line(36), False),
    (40, "violin", _melody(67), False),
    (42, "cello", _sustained(48, dur=3.0), False),
    (46, "harp", _arpeggio(60), False),
    (48, "string-ensemble", _chord(55, dur=3.0), False),
    (52, "choir-aahs", _chord(55, dur=3.0), False),
    (56, "trumpet", _melody(67), False),
    (57, "trombone", _sustained(53, dur=2.5), False),
    (61, "brass-section", _chord(53, dur=2.5), False),
    (65, "alto-sax", _melody(65), False),
    (68, "oboe", _melody(69), False),
    (71, "clarinet", _melody(62), False),
    (73, "flute", _melody(74), False),
    (-1, "drums", _drum_pattern(), True),
]


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------


def _spectral_frames(mono: np.ndarray, sr: int, n_fft: int = 4096, hop: int = 1024):
    win = np.hanning(n_fft)
    frames = []
    for start in range(0, len(mono) - n_fft, hop):
        seg = mono[start : start + n_fft] * win
        mag = np.abs(np.fft.rfft(seg))
        frames.append(mag)
    return np.asarray(frames), np.fft.rfftfreq(n_fft, 1.0 / sr)


def _harmonic_count(mono: np.ndarray, sr: int, t: float) -> int:
    n = 8192
    start = int(t * sr)
    if start + n > len(mono):
        start = max(0, len(mono) - n)
    seg = mono[start : start + n] * np.hanning(n)
    mag = np.abs(np.fft.rfft(seg))
    if mag.max() <= 0:
        return 0
    db = 20 * np.log10(mag / mag.max() + 1e-12)
    # Count spectral peaks above -50 dBFS-rel below 8 kHz.
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    peaks = 0
    for i in range(2, len(db) - 2):
        if freqs[i] > 8000:
            break
        if db[i] > -50 and db[i] > db[i - 1] and db[i] >= db[i + 1] and db[i] > db[i - 2] and db[i] >= db[i + 2]:
            peaks += 1
    return peaks


def metrics(audio: np.ndarray, sr: int) -> dict:
    left = audio[:, 0].astype(np.float64)
    right = audio[:, 1].astype(np.float64) if audio.shape[1] > 1 else left
    mono = 0.5 * (left + right)

    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    rms = float(np.sqrt(np.mean(mono**2)))
    if peak < 1e-6:
        return {"silent": True, "peak": peak}

    denom = float(np.sqrt(np.mean(left**2) * np.mean(right**2)))
    corr = float(np.mean(left * right) / denom) if denom > 0 else 1.0
    width = 1.0 - abs(corr)

    mags, freqs = _spectral_frames(mono, sr)
    energy = mags.sum(axis=1)
    active = energy > energy.max() * 1e-3
    cents = (mags[active] @ freqs) / np.maximum(mags[active].sum(axis=1), 1e-12)
    centroid_mean = float(cents.mean()) if cents.size else 0.0
    centroid_cv = float(cents.std() / max(cents.mean(), 1e-9)) if cents.size else 0.0

    cum = np.cumsum(mags[active].mean(axis=0)) if active.any() else np.zeros(1)
    rolloff = float(freqs[int(np.searchsorted(cum, 0.85 * cum[-1]))]) if cum[-1] > 0 else 0.0

    env = np.abs(mono)
    k = max(1, sr // 200)
    env = np.convolve(env, np.ones(k) / k, mode="same")
    thresh = env.max()
    above = np.nonzero(env > 0.9 * thresh)[0]
    onset = np.nonzero(env > 0.1 * thresh)[0]
    attack_ms = float((above[0] - onset[0]) / sr * 1000.0) if above.size and onset.size else 0.0

    tail_idx = np.nonzero(env > 0.01 * thresh)[0]
    tail_s = float((tail_idx[-1] - tail_idx[0]) / sr) if tail_idx.size else 0.0

    return {
        "silent": False,
        "peak": round(peak, 4),
        "rms": round(rms, 5),
        "crest_db": round(20 * np.log10(peak / max(rms, 1e-9)), 1),
        "stereo_width": round(width, 3),
        "centroid_hz": round(centroid_mean, 0),
        "centroid_cv": round(centroid_cv, 3),
        "rolloff85_hz": round(rolloff, 0),
        "attack_ms": round(attack_ms, 1),
        "active_s": round(tail_s, 2),
        "harmonics_mid": _harmonic_count(mono, sr, 1.0),
    }


# ---------------------------------------------------------------------------
# Render
# ---------------------------------------------------------------------------


def write_wav(path: Path, audio: np.ndarray, sr: int) -> None:
    pcm = (np.clip(audio, -1, 1) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def render_model(smf_bytes: bytes, seconds: float) -> np.ndarray:
    proj = libsonare.Project()
    try:
        proj.import_smf(smf_bytes)
        audio = proj.bounce_with_sf2_instrument(
            total_frames=int(seconds * SR), sample_rate=SR, num_channels=2
        )
    finally:
        proj.close()
    return audio


def render_fluidsynth(smf_path: Path, sf2: Path, out: Path, seconds: float) -> None:
    subprocess.run(
        ["fluidsynth", "-ni", "-g", "0.6", "-r", str(SR), "-F", str(out), str(sf2), str(smf_path)],
        check=True,
        capture_output=True,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/voicematch_corpus")
    ap.add_argument("--programs", default="", help="comma-separated GM programs to restrict to")
    ap.add_argument("--fluidsynth", default="", help="soundfont path for reference renders")
    ap.add_argument("--tag", default="model", help="subdirectory name for this render set")
    args = ap.parse_args()

    out_dir = Path(args.out) / args.tag
    out_dir.mkdir(parents=True, exist_ok=True)
    restrict = {int(p) for p in args.programs.split(",") if p.strip()} if args.programs else None

    results = {}
    for program, slug, notes, is_drums in CORPUS:
        if restrict is not None and program not in restrict:
            continue
        end = max(n.start + n.dur for n in notes)
        seconds = end + 2.0  # room for release tails
        channel = 9 if is_drums else 0
        # dry=False: the corpus measures the out-of-the-box playback state
        # (GS power-on sends), unlike the voicematch timbre loop which zeroes
        # the sends against its dry oracle.
        smf_bytes = write_smf(
            notes, program=max(program, 0) if not is_drums else -1, channel=channel, dry=False
        )

        audio = render_model(smf_bytes, seconds)
        name = f"{program:03d}_{slug}" if program >= 0 else f"drm_{slug}"
        write_wav(out_dir / f"{name}.wav", audio, SR)
        results[name] = metrics(audio, SR)

        if args.fluidsynth:
            smf_path = out_dir / f"{name}.mid"
            smf_path.write_bytes(smf_bytes)
            ref = out_dir / f"{name}.ref.wav"
            render_fluidsynth(smf_path, Path(args.fluidsynth), ref, seconds)

    (out_dir / "metrics.json").write_text(json.dumps(results, indent=2))

    header = f"{'file':26} {'peak':>6} {'rms':>7} {'crest':>6} {'width':>6} {'cent':>6} {'cCV':>6} {'roll':>6} {'atk':>6} {'act':>5} {'harm':>4}"
    print(header)
    for name, m in results.items():
        if m.get("silent"):
            print(f"{name:26} SILENT (peak={m['peak']:.6f})")
            continue
        print(
            f"{name:26} {m['peak']:6.3f} {m['rms']:7.4f} {m['crest_db']:6.1f} {m['stereo_width']:6.3f} "
            f"{m['centroid_hz']:6.0f} {m['centroid_cv']:6.3f} {m['rolloff85_hz']:6.0f} {m['attack_ms']:6.1f} "
            f"{m['active_s']:5.2f} {m['harmonics_mid']:4d}"
        )
    print(f"\nWAVs + metrics.json -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
