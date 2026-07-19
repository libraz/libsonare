"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json

from ._cli_common import (
    PITCH_NAMES,
    _array_stats,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)


def cmd_rhythm(args: argparse.Namespace) -> int:
    from . import analyze_rhythm

    samples, sr = _load_audio(args.file)
    r = analyze_rhythm(samples, sample_rate=sr)

    if args.json:
        print(
            json.dumps(
                {
                    "bpm": round(r.bpm, 2),
                    "time_signature": {
                        "numerator": r.time_signature.numerator,
                        "denominator": r.time_signature.denominator,
                    },
                    "groove_type": r.groove_type,
                    "syncopation": round(r.syncopation, 4),
                    "pattern_regularity": round(r.pattern_regularity, 4),
                    "tempo_stability": round(r.tempo_stability, 4),
                    "beat_intervals": _array_stats(r.beat_intervals),
                }
            )
        )
    else:
        print("  Rhythm:")
        print(f"    BPM:                {r.bpm:.2f}")
        print(
            f"    Time signature:     {r.time_signature.numerator}/{r.time_signature.denominator}"
        )
        print(f"    Groove:             {r.groove_type}")
        print(f"    Syncopation:        {r.syncopation:.4f}")
        print(f"    Pattern regularity: {r.pattern_regularity:.4f}")
        print(f"    Tempo stability:    {r.tempo_stability:.4f}")
        print(f"    Beat intervals:     {len(r.beat_intervals)}")
    return 0


def cmd_dynamics(args: argparse.Namespace) -> int:
    from . import analyze_dynamics

    samples, sr = _load_audio(args.file)
    r = analyze_dynamics(samples, sample_rate=sr)

    if args.json:
        print(
            json.dumps(
                {
                    "dynamic_range_db": round(r.dynamic_range_db, 4),
                    "peak_db": round(r.peak_db, 4),
                    "rms_db": round(r.rms_db, 4),
                    "crest_factor": round(r.crest_factor, 4),
                    "loudness_range_db": round(r.loudness_range_db, 4),
                    "is_compressed": r.is_compressed,
                    "loudness": _array_stats(r.loudness_rms_db),
                }
            )
        )
    else:
        print("  Dynamics:")
        print(f"    Dynamic range:  {r.dynamic_range_db:.2f} dB")
        print(f"    Peak:           {r.peak_db:.2f} dB")
        print(f"    RMS:            {r.rms_db:.2f} dB")
        print(f"    Crest factor:   {r.crest_factor:.4f}")
        print(f"    Loudness range: {r.loudness_range_db:.2f} dB")
        print(f"    Compressed:     {r.is_compressed}")
    return 0


def cmd_timbre(args: argparse.Namespace) -> int:
    from . import analyze_timbre

    samples, sr = _load_audio(args.file)
    r = analyze_timbre(
        samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length, n_mels=args.n_mels
    )

    if args.json:
        print(
            json.dumps(
                {
                    "brightness": round(r.brightness, 4),
                    "warmth": round(r.warmth, 4),
                    "density": round(r.density, 4),
                    "roughness": round(r.roughness, 4),
                    "complexity": round(r.complexity, 4),
                    "spectral_centroid": _array_stats(r.spectral_centroid),
                    "spectral_flatness": _array_stats(r.spectral_flatness),
                    "spectral_rolloff": _array_stats(r.spectral_rolloff),
                }
            )
        )
    else:
        print("  Timbre:")
        print(f"    Brightness: {r.brightness:.4f}")
        print(f"    Warmth:     {r.warmth:.4f}")
        print(f"    Density:    {r.density:.4f}")
        print(f"    Roughness:  {r.roughness:.4f}")
        print(f"    Complexity: {r.complexity:.4f}")
    return 0


def cmd_lufs(args: argparse.Namespace) -> int:
    from ._features import lufs, momentary_lufs, short_term_lufs

    samples, sr = _load_audio(args.file)
    r = lufs(samples, sample_rate=sr)

    payload: dict[str, object] = {
        "integrated": round(r.integrated_lufs, 4),
        "momentary": round(r.momentary_lufs, 4),
        "short_term": round(r.short_term_lufs, 4),
        "loudness_range": round(r.loudness_range, 4),
    }
    momentary_series: list[float] = []
    short_term_series: list[float] = []
    if args.series:
        momentary_series = momentary_lufs(samples, sample_rate=sr)
        short_term_series = short_term_lufs(samples, sample_rate=sr)

    if args.json:
        if args.series:
            payload["momentary_series"] = [round(v, 4) for v in momentary_series]
            payload["short_term_series"] = [round(v, 4) for v in short_term_series]
        print(_strict_json_dumps(payload))
    else:
        print("  Loudness (LUFS):")
        print(f"    Integrated:     {r.integrated_lufs:.2f} LUFS")
        print(f"    Momentary:      {r.momentary_lufs:.2f} LUFS")
        print(f"    Short-term:     {r.short_term_lufs:.2f} LUFS")
        print(f"    Loudness range: {r.loudness_range:.2f} LU")
        if args.series:
            print(f"    Momentary samples:  {len(momentary_series)}")
            print(f"    Short-term samples: {len(short_term_series)}")
    return 0


def cmd_onset_envelope(args: argparse.Namespace) -> int:
    from ._conversions import onset_envelope

    samples, sr = _load_audio(args.file)
    env = onset_envelope(
        samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length, n_mels=args.n_mels
    )

    if args.json:
        print(json.dumps({"stats": _array_stats(env), "values": [round(v, 6) for v in env]}))
    else:
        stats = _array_stats(env)
        print("  Onset envelope:")
        print(f"    Frames: {stats['count']}")
        print(f"    Mean:   {stats['mean']:.6f}")
        print(f"    Max:    {stats['max']:.6f}")
    return 0


def cmd_nnls_chroma(args: argparse.Namespace) -> int:
    from ._conversions import nnls_chroma

    samples, sr = _load_audio(args.file)
    n_frames, data = nnls_chroma(samples, sample_rate=sr)
    n_chroma = 12
    # data is row-major [12 x n_frames]; mean energy per bin.
    mean_energy = []
    for bin_index in range(n_chroma):
        start = bin_index * n_frames
        row = data[start : start + n_frames]
        mean_energy.append(sum(row) / len(row) if row else 0.0)

    if args.json:
        print(
            json.dumps(
                {
                    "n_chroma": n_chroma,
                    "n_frames": n_frames,
                    "mean_energy": [round(e, 6) for e in mean_energy],
                }
            )
        )
    else:
        print(f"  NNLS chroma: {n_chroma} bins x {n_frames} frames")
        print("  Mean energy per pitch class:")
        max_energy = max(mean_energy) if mean_energy else 0
        for i, e in enumerate(mean_energy):
            bar = "#" * int(e * 50 / max_energy) if max_energy > 0 else ""
            print(f"    {PITCH_NAMES[i]:2s} {e:.4f} {bar}")
    return 0


def cmd_tempogram(args: argparse.Namespace) -> int:
    from ._conversions import onset_envelope, tempogram

    samples, sr = _load_audio(args.file)
    env = onset_envelope(
        samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length, n_mels=args.n_mels
    )
    n_frames, data = tempogram(env, sample_rate=sr, hop_length=args.hop_length)
    win_length = (len(data) // n_frames) if n_frames else 0

    if args.json:
        print(
            json.dumps(
                {
                    "win_length": win_length,
                    "n_frames": n_frames,
                    "stats": _array_stats(data),
                }
            )
        )
    else:
        print("  Tempogram:")
        print(f"    Win length: {win_length}")
        print(f"    Frames:     {n_frames}")
        stats = _array_stats(data)
        print(f"    Mean:       {stats['mean']:.6f}")
        print(f"    Max:        {stats['max']:.6f}")
    return 0


def cmd_plp(args: argparse.Namespace) -> int:
    from ._conversions import onset_envelope, plp

    samples, sr = _load_audio(args.file)
    env = onset_envelope(
        samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length, n_mels=args.n_mels
    )
    pulse = plp(env, sample_rate=sr, hop_length=args.hop_length)

    if args.json:
        print(json.dumps({"stats": _array_stats(pulse)}))
    else:
        stats = _array_stats(pulse)
        print("  Predominant local pulse (PLP):")
        print(f"    Frames: {stats['count']}")
        print(f"    Mean:   {stats['mean']:.6f}")
        print(f"    Max:    {stats['max']:.6f}")
    return 0
