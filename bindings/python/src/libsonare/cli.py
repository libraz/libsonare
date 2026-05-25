"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import sys

PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MODE_NAMES = ["major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"]

# NOTE: Some C++ CLI commands (sections, melody, boundaries, cqt, and the
# low-level math/unit converters) are intentionally NOT exposed here because
# they have no standalone Python library backing yet. They remain C++-CLI-only
# pending Python lib support.


def _load_audio(path: str) -> tuple[list[float], int]:
    """Load audio from file via the Audio class."""
    from .audio import Audio

    with Audio.from_file(path) as audio:
        return audio.data, audio.sample_rate


def _write_wav(path: str, samples: list[float], sample_rate: int) -> None:
    """Write mono 16-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled by 32767.
    """
    import struct
    import wave

    frames = bytearray()
    for s in samples:
        clamped = -1.0 if s < -1.0 else (1.0 if s > 1.0 else s)
        frames += struct.pack("<h", int(round(clamped * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _array_stats(vals: list[float]) -> dict:
    """Summary statistics for a numeric array (avoids dumping huge arrays)."""
    import statistics

    if not vals:
        return {"count": 0, "mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
    return {
        "count": len(vals),
        "mean": round(statistics.mean(vals), 6),
        "std": round(statistics.stdev(vals), 6) if len(vals) > 1 else 0.0,
        "min": round(min(vals), 6),
        "max": round(max(vals), 6),
    }


def _parse_kv_params(value: str) -> dict[str, float]:
    """Parse a ``k=v,k=v`` string into a dict of floats."""
    params: dict[str, float] = {}
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"invalid param (expected key=value): {item}")
        key, raw = item.split("=", 1)
        params[key.strip()] = float(raw.strip())
    return params


def _format_time(seconds: float) -> str:
    """Format seconds as mm:ss."""
    mm = int(seconds) // 60
    ss = int(seconds) % 60
    return f"{mm}:{ss:02d}"


def _parse_pitch_class(value: str):
    from .types import PitchClass

    names = {
        "C": PitchClass.C,
        "C#": PitchClass.CS,
        "DB": PitchClass.CS,
        "D": PitchClass.D,
        "D#": PitchClass.DS,
        "EB": PitchClass.DS,
        "E": PitchClass.E,
        "F": PitchClass.F,
        "F#": PitchClass.FS,
        "GB": PitchClass.FS,
        "G": PitchClass.G,
        "G#": PitchClass.GS,
        "AB": PitchClass.GS,
        "A": PitchClass.A,
        "A#": PitchClass.AS,
        "BB": PitchClass.AS,
        "B": PitchClass.B,
    }
    key = value.upper()
    if key not in names:
        raise ValueError(f"invalid pitch class: {value}")
    return names[key]


def _parse_mode(value: str):
    from .types import Mode

    key = value.lower()
    if key in ("major", "maj"):
        return Mode.MAJOR
    if key in ("minor", "min", "m"):
        return Mode.MINOR
    if key == "dorian":
        return Mode.DORIAN
    if key == "phrygian":
        return Mode.PHRYGIAN
    if key == "lydian":
        return Mode.LYDIAN
    if key == "mixolydian":
        return Mode.MIXOLYDIAN
    if key == "locrian":
        return Mode.LOCRIAN
    raise ValueError(f"invalid mode: {value}")


def _parse_modes(value: str):
    from .types import Mode

    key = value.lower()
    if key in ("major-minor", "majmin", "diatonic"):
        return [Mode.MAJOR, Mode.MINOR]
    if key in ("all", "modal"):
        return [
            Mode.MAJOR,
            Mode.MINOR,
            Mode.DORIAN,
            Mode.PHRYGIAN,
            Mode.LYDIAN,
            Mode.MIXOLYDIAN,
            Mode.LOCRIAN,
        ]
    return [_parse_mode(item.strip()) for item in value.split(",") if item.strip()]


def _parse_key_profile(value: str):
    from .types import KeyProfile

    names = {
        "ks": KeyProfile.KRUMHANSL_SCHMUCKLER,
        "krumhansl": KeyProfile.KRUMHANSL_SCHMUCKLER,
        "krumhansl-schmuckler": KeyProfile.KRUMHANSL_SCHMUCKLER,
        "temperley": KeyProfile.TEMPERLEY,
        "shaath": KeyProfile.SHAATH,
        "keyfinder": KeyProfile.SHAATH,
        "faraldo-edmt": KeyProfile.FARALDO_EDMT,
        "edmt": KeyProfile.FARALDO_EDMT,
        "faraldo-edma": KeyProfile.FARALDO_EDMA,
        "edma": KeyProfile.FARALDO_EDMA,
        "faraldo-edmm": KeyProfile.FARALDO_EDMM,
        "edmm": KeyProfile.FARALDO_EDMM,
        "bellman-budge": KeyProfile.BELLMAN_BUDGE,
        "bellman": KeyProfile.BELLMAN_BUDGE,
    }
    key = value.lower()
    if key not in names:
        raise ValueError(f"invalid key profile: {value}")
    return names[key]


def cmd_version(args: argparse.Namespace) -> int:
    from . import version

    v = version()
    if args.json:
        print(json.dumps({"lib_version": v, "cli": "python"}))
    else:
        print(f"libsonare {v} (Python CLI)")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    from .audio import Audio

    with Audio.from_file(args.file) as audio:
        sr = audio.sample_rate
        n = audio.length
        dur = audio.duration

    if args.json:
        print(
            json.dumps(
                {
                    "duration": round(dur, 3),
                    "sample_rate": sr,
                    "samples": n,
                }
            )
        )
    else:
        print(f"  Duration:    {_format_time(dur)} ({dur:.1f}s)")
        print(f"  Sample Rate: {sr} Hz")
        print(f"  Samples:     {n}")
    return 0


def cmd_bpm(args: argparse.Namespace) -> int:
    from . import detect_bpm

    samples, sr = _load_audio(args.file)
    bpm = detect_bpm(samples, sample_rate=sr)
    if args.json:
        print(json.dumps({"bpm": round(bpm, 2)}))
    else:
        print(f"  BPM: {bpm:.2f}")
    return 0


def cmd_key(args: argparse.Namespace) -> int:
    from . import detect_key, detect_key_candidates

    samples, sr = _load_audio(args.file)
    n_fft = 4096 if args.n_fft == 2048 else args.n_fft
    key_options = {
        "sample_rate": sr,
        "n_fft": n_fft,
        "hop_length": args.hop_length,
        "use_hpss": args.use_hpss,
        "loudness_weighted": args.loudness_weighted,
        "high_pass_hz": args.high_pass_hz,
        "modes": _parse_modes(args.modes) if args.modes else None,
        "profile": _parse_key_profile(args.profile) if args.profile else None,
        "genre_hint": args.genre_hint or None,
    }
    key = detect_key(samples, **key_options)
    name = f"{PITCH_NAMES[key.root.value]} {MODE_NAMES[key.mode.value]}"
    candidate_count = max(0, args.candidates)
    candidates = (
        detect_key_candidates(samples, **key_options)[:candidate_count] if candidate_count else []
    )
    if args.json:
        payload: dict[str, object] = {
            "root": key.root.value,
            "mode": key.mode.value,
            "confidence": round(key.confidence, 4),
            "name": name,
        }
        if candidates:
            payload["candidates"] = [
                {
                    "root": candidate.key.root.value,
                    "mode": candidate.key.mode.value,
                    "confidence": round(candidate.key.confidence, 4),
                    "name": f"{PITCH_NAMES[candidate.key.root.value]} "
                    f"{MODE_NAMES[candidate.key.mode.value]}",
                    "correlation": round(candidate.correlation, 6),
                }
                for candidate in candidates
            ]
        print(json.dumps(payload))
    else:
        print(f"  Key: {name} (confidence: {key.confidence:.1%})")
        if candidates:
            print("  Key candidates:")
            for index, candidate in enumerate(candidates, start=1):
                candidate_name = (
                    f"{PITCH_NAMES[candidate.key.root.value]} "
                    f"{MODE_NAMES[candidate.key.mode.value]}"
                )
                print(
                    f"    {index:2d}. {candidate_name} "
                    f"(corr: {candidate.correlation:.3f}, "
                    f"confidence: {candidate.key.confidence:.1%})"
                )
    return 0


def cmd_beats(args: argparse.Namespace) -> int:
    from . import detect_beats

    samples, sr = _load_audio(args.file)
    beats = detect_beats(samples, sample_rate=sr)
    if args.json:
        print(json.dumps([round(b, 4) for b in beats]))
    else:
        print(f"  Beat times ({len(beats)} beats):")
        for i, b in enumerate(beats[:20]):
            print(f"    {i + 1:3d}. {b:.3f}s")
        if len(beats) > 20:
            print(f"    ... ({len(beats) - 20} more)")
    return 0


def cmd_downbeats(args: argparse.Namespace) -> int:
    from . import detect_downbeats

    samples, sr = _load_audio(args.file)
    downbeats = detect_downbeats(samples, sample_rate=sr)
    if args.json:
        print(json.dumps([round(d, 4) for d in downbeats]))
    else:
        print(f"  Downbeat times ({len(downbeats)} downbeats):")
        for i, d in enumerate(downbeats[:20]):
            print(f"    {i + 1:3d}. {d:.3f}s")
        if len(downbeats) > 20:
            print(f"    ... ({len(downbeats) - 20} more)")
    return 0


def cmd_onsets(args: argparse.Namespace) -> int:
    from . import detect_onsets

    samples, sr = _load_audio(args.file)
    onsets = detect_onsets(samples, sample_rate=sr)
    if args.json:
        print(json.dumps([round(o, 4) for o in onsets]))
    else:
        print(f"  Onset times ({len(onsets)} onsets):")
        for i, o in enumerate(onsets[:20]):
            print(f"    {i + 1:3d}. {o:.3f}s")
        if len(onsets) > 20:
            print(f"    ... ({len(onsets) - 20} more)")
    return 0


def cmd_chords(args: argparse.Namespace) -> int:
    from . import detect_chords

    samples, sr = _load_audio(args.file)
    result = detect_chords(
        samples,
        sample_rate=sr,
        min_duration=args.min_duration,
        smoothing_window=args.smoothing_window,
        threshold=args.threshold,
        use_triads_only=args.triads_only,
        n_fft=args.n_fft,
        hop_length=args.hop_length,
        use_beat_sync=not args.no_beat_sync,
        use_hmm=args.use_hmm,
        hmm_beam_width=args.hmm_beam_width,
        use_key_context=args.key_context,
        key_root=_parse_pitch_class(args.key_root),
        key_mode=_parse_mode(args.key_mode),
        detect_inversions=args.detect_inversions,
        chroma_method="nnls" if args.nnls else "stft",
    )
    if args.json:
        print(
            json.dumps(
                {
                    "count": len(result.chords),
                    "chords": [
                        {
                            "name": chord.name,
                            "root": chord.root.value,
                            "quality": chord.quality,
                            "bass": (chord.bass or chord.root).value,
                            "start": round(chord.start, 6),
                            "end": round(chord.end, 6),
                            "confidence": round(chord.confidence, 4),
                        }
                        for chord in result.chords
                    ],
                }
            )
        )
    else:
        print(f"  Chords ({len(result.chords)} changes):")
        for index, chord in enumerate(result.chords[:40], start=1):
            print(
                f"    {index:2d}. {chord.name:<10s} "
                f"({chord.start:.2f}s - {chord.end:.2f}s, "
                f"confidence: {chord.confidence:.0%})"
            )
        if len(result.chords) > 40:
            print(f"    ... ({len(result.chords) - 40} more)")
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    from . import analyze

    samples, sr = _load_audio(args.file)
    r = analyze(samples, sample_rate=sr)
    key_name = f"{PITCH_NAMES[r.key.root.value]} {MODE_NAMES[r.key.mode.value]}"

    if args.json:
        print(
            json.dumps(
                {
                    "bpm": round(r.bpm, 2),
                    "bpm_confidence": round(r.bpm_confidence, 4),
                    "key": {
                        "root": r.key.root.value,
                        "mode": r.key.mode.value,
                        "confidence": round(r.key.confidence, 4),
                        "name": key_name,
                    },
                    "time_signature": {
                        "numerator": r.time_signature.numerator,
                        "denominator": r.time_signature.denominator,
                    },
                    "beats": len(r.beat_times),
                }
            )
        )
    else:
        print(
            f"\n  \033[32m\033[1m> Estimated BPM : {r.bpm:.2f} BPM  "
            f"(conf {r.bpm_confidence * 100:.1f}%)\033[0m"
        )
        print(
            f"  \033[35m\033[1m> Estimated Key : {key_name}  "
            f"(conf {r.key.confidence * 100:.1f}%)\033[0m"
        )
        print(f"  > Time Signature: {r.time_signature.numerator}/{r.time_signature.denominator}")
        print(f"  > Beats: {len(r.beat_times)}")
    return 0


def cmd_mel(args: argparse.Namespace) -> int:
    from . import mel_spectrogram

    samples, sr = _load_audio(args.file)
    result = mel_spectrogram(
        samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length, n_mels=args.n_mels
    )
    if args.json:
        print(
            json.dumps(
                {
                    "n_mels": result.n_mels,
                    "n_frames": result.n_frames,
                    "sample_rate": result.sample_rate,
                    "hop_length": result.hop_length,
                }
            )
        )
    else:
        print("  Mel Spectrogram:")
        print(f"    Shape: {result.n_mels} mels x {result.n_frames} frames")
    return 0


def cmd_chroma(args: argparse.Namespace) -> int:
    from . import chroma

    samples, sr = _load_audio(args.file)
    result = chroma(samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length)
    if args.json:
        print(
            json.dumps(
                {
                    "n_chroma": result.n_chroma,
                    "n_frames": result.n_frames,
                    "mean_energy": [round(e, 6) for e in result.mean_energy],
                }
            )
        )
    else:
        print(f"  Chromagram: {result.n_chroma} bins x {result.n_frames} frames")
        print("  Mean energy per pitch class:")
        max_energy = max(result.mean_energy) if result.mean_energy else 0
        for i, e in enumerate(result.mean_energy):
            bar = "#" * int(e * 50 / max_energy) if max_energy > 0 else ""
            print(f"    {PITCH_NAMES[i]:2s} {e:.4f} {bar}")
    return 0


def cmd_spectral(args: argparse.Namespace) -> int:
    import statistics

    from . import (
        rms_energy,
        spectral_bandwidth,
        spectral_centroid,
        spectral_flatness,
        spectral_rolloff,
        zero_crossing_rate,
    )

    samples, sr = _load_audio(args.file)
    nf = args.n_fft
    hl = args.hop_length

    def _stats(vals: list[float]) -> dict:
        if not vals:
            return {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
        return {
            "mean": round(statistics.mean(vals), 4),
            "std": round(statistics.stdev(vals), 4) if len(vals) > 1 else 0.0,
            "min": round(min(vals), 4),
            "max": round(max(vals), 4),
        }

    features = {
        "centroid": _stats(spectral_centroid(samples, sr, nf, hl)),
        "bandwidth": _stats(spectral_bandwidth(samples, sr, nf, hl)),
        "rolloff": _stats(spectral_rolloff(samples, sr, nf, hl)),
        "flatness": _stats(spectral_flatness(samples, sr, nf, hl)),
        "zcr": _stats(zero_crossing_rate(samples, sr, nf, hl)),
        "rms": _stats(rms_energy(samples, sr, nf, hl)),
    }

    if args.json:
        print(json.dumps({"features": features}))
    else:
        print("  Spectral Features:")
        print(f"  {'Feature':<15s} {'Mean':>10s} {'Std':>10s} {'Min':>10s} {'Max':>10s}")
        for name, s in features.items():
            fmt = ".1f" if name in ("centroid", "bandwidth", "rolloff") else ".4f"
            print(
                f"  {name:<15s} {s['mean']:>10{fmt}} {s['std']:>10{fmt}} "
                f"{s['min']:>10{fmt}} {s['max']:>10{fmt}}"
            )
    return 0


def cmd_pitch(args: argparse.Namespace) -> int:
    from . import pitch_pyin, pitch_yin

    samples, sr = _load_audio(args.file)
    algo = getattr(args, "algorithm", "pyin")
    if algo == "yin":
        result = pitch_yin(samples, sample_rate=sr)
    else:
        result = pitch_pyin(samples, sample_rate=sr)

    if args.json:
        print(
            json.dumps(
                {
                    "algorithm": algo,
                    "n_frames": result.n_frames,
                    "median_f0": round(result.median_f0, 2),
                    "mean_f0": round(result.mean_f0, 2),
                }
            )
        )
    else:
        print(f"  Pitch Tracking ({algo}):")
        print(f"    Frames:    {result.n_frames}")
        print(f"    Median F0: {result.median_f0:.1f} Hz")
        print(f"    Mean F0:   {result.mean_f0:.1f} Hz")
    return 0


def cmd_hpss(args: argparse.Namespace) -> int:
    from . import hpss

    samples, sr = _load_audio(args.file)
    result = hpss(samples, sample_rate=sr)

    h_energy = sum(abs(x) for x in result.harmonic) / len(result.harmonic)
    p_energy = sum(abs(x) for x in result.percussive) / len(result.percussive)

    if args.json:
        print(
            json.dumps(
                {
                    "length": result.length,
                    "sample_rate": result.sample_rate,
                    "harmonic_energy": round(h_energy, 6),
                    "percussive_energy": round(p_energy, 6),
                }
            )
        )
    else:
        print(f"  HPSS: {result.length} samples")
        print(f"  Harmonic energy:   {h_energy:.6f}")
        print(f"  Percussive energy: {p_energy:.6f}")
    return 0


def cmd_acoustic(args: argparse.Namespace) -> int:
    from . import analyze_impulse_response, detect_acoustic

    samples, sr = _load_audio(args.file)
    if args.ir:
        result = analyze_impulse_response(samples, sample_rate=sr)
    else:
        result = detect_acoustic(samples, sample_rate=sr)

    if args.json:
        print(
            json.dumps(
                {
                    "rt60": round(result.rt60, 4),
                    "edt": round(result.edt, 4),
                    "c50": result.c50,
                    "c80": result.c80,
                    "d50": result.d50,
                    "confidence": round(result.confidence, 4),
                    "is_blind": result.is_blind,
                }
            )
        )
    else:
        mode = "impulse response" if args.ir else "blind"
        print(f"  Acoustic ({mode}):")
        print(f"    RT60:       {result.rt60:.3f} s")
        print(f"    EDT:        {result.edt:.3f} s")
        print(f"    C50:        {result.c50:.2f} dB")
        print(f"    C80:        {result.c80:.2f} dB")
        print(f"    D50:        {result.d50:.3f}")
        print(f"    Confidence: {result.confidence:.1%}")
        print(f"    Blind:      {result.is_blind}")
    return 0


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
    from . import lufs, momentary_lufs, short_term_lufs

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
        print(json.dumps(payload))
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
    from . import onset_envelope

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
    from . import nnls_chroma

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
    from . import onset_envelope, tempogram

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
    from . import onset_envelope, plp

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


def cmd_mastering(args: argparse.Namespace) -> int:
    from .audio import Audio

    samples, sr = _load_audio(args.file)
    result = Audio.from_buffer(samples, sr).mastering(
        target_lufs=args.target_lufs, ceiling_db=args.ceiling_db
    )

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)

    if args.json:
        payload = {
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
    else:
        print("  Mastering:")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_mastering_processor(args: argparse.Namespace) -> int:
    from . import mastering_process

    samples, sr = _load_audio(args.file)
    params = _parse_kv_params(args.params) if args.params else {}
    result = mastering_process(args.processor, samples, sample_rate=sr, params=params)

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)

    if args.json:
        payload = {
            "processor": args.processor,
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
    else:
        print(f"  Mastering processor: {args.processor}")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_eq(args: argparse.Namespace) -> int:
    from . import mastering_process

    samples, sr = _load_audio(args.file)
    if args.params:
        params = _parse_kv_params(args.params)
    else:
        params = {
            "band0.enabled": 1.0,
            "band0.type": float(args.type),
            "band0.frequencyHz": float(args.frequency_hz),
            "band0.gainDb": float(args.gain_db),
            "band0.q": float(args.q),
            "band0.coeffMode": float(args.coeff_mode),
            "band0.slopeDbOct": float(args.slope_db_oct),
            "band0.placement": float(args.placement),
            "band0.proportionalQ": 1.0 if args.proportional_q else 0.0,
            "band0.dynamic": 1.0 if args.dynamic else 0.0,
            "band0.thresholdDb": float(args.threshold_db),
            "band0.autoThreshold": 1.0 if args.auto_threshold else 0.0,
            "band0.ratio": float(args.ratio),
            "band0.rangeDb": float(args.range_db),
            "band0.attackMs": float(args.attack_ms),
            "band0.releaseMs": float(args.release_ms),
            "band0.lookaheadMs": float(args.lookahead_ms),
            "band0.sidechainFreqHz": float(args.sidechain_freq_hz),
            "band0.sidechainQ": float(args.sidechain_q),
            "phaseMode": float(args.phase_mode),
            "resolution": float(args.resolution),
            "autoGain": 1.0 if args.auto_gain else 0.0,
        }
    result = mastering_process("eq.equalizer", samples, sample_rate=sr, params=params)

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)

    if args.json:
        payload = {
            "processor": "eq.equalizer",
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
    else:
        print("  Equalizer")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_mastering_processors(args: argparse.Namespace) -> int:
    from . import mastering_processor_names

    names = mastering_processor_names()
    if args.json:
        print(json.dumps(names))
    else:
        print("  Mastering processors:")
        for name in names:
            print(f"    {name}")
    return 0


def cmd_mastering_pair_processors(args: argparse.Namespace) -> int:
    from . import mastering_pair_processor_names

    names = mastering_pair_processor_names()
    if args.json:
        print(json.dumps(names))
    else:
        print("  Mastering pair processors:")
        for name in names:
            print(f"    {name}")
    return 0


def cmd_mastering_pair_analyses(args: argparse.Namespace) -> int:
    from . import mastering_pair_analysis_names

    names = mastering_pair_analysis_names()
    if args.json:
        print(json.dumps(names))
    else:
        print("  Mastering pair analyses:")
        for name in names:
            print(f"    {name}")
    return 0


def cmd_mastering_pair_analyze(args: argparse.Namespace) -> int:
    from . import mastering_pair_analyze

    source, sr = _load_audio(args.file)
    reference, ref_sr = _load_audio(args.reference)
    if len(source) != len(reference):
        # The library requires matching lengths; surface a clear error.
        raise ValueError(
            f"source ({len(source)}) and reference ({len(reference)}) "
            "lengths must match for pair analysis"
        )
    result_json = mastering_pair_analyze(args.analysis, source, reference, sample_rate=sr)
    # The library returns a JSON string regardless of --json; print it as-is.
    print(result_json)
    _ = ref_sr
    return 0


def main() -> None:
    """CLI entry point."""
    # Common arguments shared by all subcommands
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", help="Output JSON")
    common.add_argument("--n-fft", type=int, default=2048, help="FFT size (default: 2048)")
    common.add_argument("--hop-length", type=int, default=512, help="Hop length (default: 512)")
    common.add_argument(
        "--n-mels", type=int, default=128, help="Number of mel bands (default: 128)"
    )
    common.add_argument("-o", "--output", type=str, default="", help="Output file path")

    parser = argparse.ArgumentParser(
        prog="sonare",
        description="libsonare - Fast audio analysis (Python CLI)",
    )
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("version", parents=[common], help="Show version")
    sub.add_parser("info", parents=[common], help="Show audio file information")
    sub.add_parser("bpm", parents=[common], help="Detect BPM")
    key_p = sub.add_parser("key", parents=[common], help="Detect musical key")
    key_p.add_argument(
        "--candidates",
        type=int,
        default=0,
        metavar="N",
        help="Also show the top N key candidates",
    )
    key_p.add_argument("--use-hpss", action="store_true", help="Use harmonic audio for key chroma")
    key_p.add_argument(
        "--loudness-weighted", action="store_true", help="Weight key chroma frames by RMS"
    )
    key_p.add_argument(
        "--high-pass-hz", type=float, default=0.0, help="High-pass cutoff before key analysis"
    )
    key_p.add_argument(
        "--modes",
        type=str,
        default="",
        help="Candidate modes: major-minor, all, or comma-separated mode names",
    )
    key_p.add_argument(
        "--profile",
        type=str,
        default="",
        help="Key profile: ks, temperley, shaath, edmt, edma, edmm, or bellman",
    )
    key_p.add_argument(
        "--genre-hint",
        type=str,
        default="",
        help="Genre hint for key profile selection, e.g. auto, edm, pop, classical, jazz",
    )
    sub.add_parser("beats", parents=[common], help="Detect beat times")
    sub.add_parser("downbeats", parents=[common], help="Detect downbeat times")
    sub.add_parser("onsets", parents=[common], help="Detect onset times")
    chords_p = sub.add_parser("chords", parents=[common], help="Detect chord progression")
    chords_p.add_argument("--min-duration", type=float, default=0.3)
    chords_p.add_argument("--smoothing-window", type=float, default=2.0)
    chords_p.add_argument("--threshold", type=float, default=0.5)
    chords_p.add_argument("--triads-only", action="store_true")
    chords_p.add_argument("--nnls", action="store_true")
    chords_p.add_argument("--no-beat-sync", action="store_true")
    chords_p.add_argument("--use-hmm", action="store_true")
    chords_p.add_argument("--hmm-beam-width", type=int, default=24)
    chords_p.add_argument("--key-context", action="store_true")
    chords_p.add_argument("--key-root", default="C")
    chords_p.add_argument("--key-mode", default="major")
    chords_p.add_argument("--detect-inversions", action="store_true")
    sub.add_parser("analyze", parents=[common], help="Full music analysis")
    sub.add_parser("mel", parents=[common], help="Compute mel spectrogram")
    sub.add_parser("chroma", parents=[common], help="Compute chromagram")
    sub.add_parser("spectral", parents=[common], help="Compute spectral features")
    pitch_p = sub.add_parser("pitch", parents=[common], help="Track pitch")
    pitch_p.add_argument("--algorithm", choices=["yin", "pyin"], default="pyin")
    sub.add_parser("hpss", parents=[common], help="Harmonic-percussive separation")

    # Analysis commands
    acoustic_p = sub.add_parser("acoustic", parents=[common], help="Estimate acoustic parameters")
    acoustic_p.add_argument("--ir", action="store_true", help="Treat input as an impulse response")
    sub.add_parser("rhythm", parents=[common], help="Analyze rhythm primitives")
    sub.add_parser("dynamics", parents=[common], help="Analyze dynamics/loudness")
    sub.add_parser("timbre", parents=[common], help="Analyze timbre/spectral shape")
    lufs_p = sub.add_parser("lufs", parents=[common], help="Compute LUFS loudness")
    lufs_p.add_argument(
        "--series", action="store_true", help="Also emit momentary/short-term LUFS series"
    )
    sub.add_parser("onset-envelope", parents=[common], help="Compute the onset strength envelope")
    sub.add_parser("nnls-chroma", parents=[common], help="Compute NNLS chroma")
    sub.add_parser("tempogram", parents=[common], help="Compute autocorrelation tempogram")
    sub.add_parser("plp", parents=[common], help="Compute predominant local pulse")

    # Mastering commands
    mastering_p = sub.add_parser(
        "mastering", parents=[common], help="Loudness-normalize with a true-peak ceiling"
    )
    mastering_p.add_argument("--target-lufs", type=float, default=-14.0)
    mastering_p.add_argument("--ceiling-db", type=float, default=-1.0)
    mproc_p = sub.add_parser(
        "mastering-processor", parents=[common], help="Apply a named mastering processor"
    )
    mproc_p.add_argument("--processor", required=True, help="Processor name")
    mproc_p.add_argument("--params", default="", help="Params as k=v,k=v (floats)")
    eq_p = sub.add_parser("eq", parents=[common], help="Apply the unified equalizer")
    eq_p.add_argument("--params", default="", help="Params as k=v,k=v (overrides band shortcuts)")
    eq_p.add_argument(
        "--type",
        type=int,
        default=0,
        help=(
            "Band type enum: 0 peak, 1 low shelf, 2 high shelf, 3 low pass, "
            "4 high pass, 5 band pass, 6 notch, 7 tilt"
        ),
    )
    eq_p.add_argument("--frequency-hz", type=float, default=1000.0)
    eq_p.add_argument("--gain-db", type=float, default=0.0)
    eq_p.add_argument("--q", type=float, default=1.0)
    eq_p.add_argument("--coeff-mode", type=int, default=0, help="0 RBJ, 1 Vicanek")
    eq_p.add_argument("--slope-db-oct", type=int, default=12)
    eq_p.add_argument(
        "--placement", type=int, default=0, help="0 stereo, 1 left, 2 right, 3 mid, 4 side"
    )
    eq_p.add_argument(
        "--phase-mode", type=int, default=1, help="1 zero latency, 2 natural, 3 linear"
    )
    eq_p.add_argument(
        "--resolution",
        type=int,
        default=0,
        help="0 custom/default, 1 low, 2 medium, 3 high, 4 very high, 5 maximum",
    )
    eq_p.add_argument("--auto-gain", action="store_true")
    eq_p.add_argument("--proportional-q", action="store_true")
    eq_p.add_argument("--dynamic", action="store_true")
    eq_p.add_argument("--threshold-db", type=float, default=-24.0)
    eq_p.add_argument("--auto-threshold", action="store_true")
    eq_p.add_argument("--ratio", type=float, default=2.0)
    eq_p.add_argument("--range-db", type=float, default=-6.0)
    eq_p.add_argument("--attack-ms", type=float, default=5.0)
    eq_p.add_argument("--release-ms", type=float, default=50.0)
    eq_p.add_argument("--lookahead-ms", type=float, default=0.0)
    eq_p.add_argument("--sidechain-freq-hz", type=float, default=-1.0)
    eq_p.add_argument("--sidechain-q", type=float, default=1.0)
    sub.add_parser("mastering-processors", parents=[common], help="List mastering processor names")
    sub.add_parser(
        "mastering-pair-processors",
        parents=[common],
        help="List two-input mastering processor names",
    )
    sub.add_parser(
        "mastering-pair-analyses",
        parents=[common],
        help="List two-input mastering analysis names",
    )
    mpa_p = sub.add_parser(
        "mastering-pair-analyze", parents=[common], help="Run a two-input mastering analysis"
    )
    mpa_p.add_argument("--reference", required=True, help="Reference audio file")
    mpa_p.add_argument("--analysis", required=True, help="Analysis name")

    # Add file argument to all subcommands that need it
    for name in [
        "info",
        "bpm",
        "key",
        "beats",
        "downbeats",
        "onsets",
        "chords",
        "analyze",
        "mel",
        "chroma",
        "spectral",
        "pitch",
        "hpss",
        "acoustic",
        "rhythm",
        "dynamics",
        "timbre",
        "lufs",
        "onset-envelope",
        "nnls-chroma",
        "tempogram",
        "plp",
        "mastering",
        "eq",
        "mastering-processor",
        "mastering-pair-analyze",
    ]:
        sub.choices[name].add_argument("file", help="Audio file path")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    commands = {
        "version": cmd_version,
        "info": cmd_info,
        "bpm": cmd_bpm,
        "key": cmd_key,
        "beats": cmd_beats,
        "downbeats": cmd_downbeats,
        "onsets": cmd_onsets,
        "chords": cmd_chords,
        "analyze": cmd_analyze,
        "mel": cmd_mel,
        "chroma": cmd_chroma,
        "spectral": cmd_spectral,
        "pitch": cmd_pitch,
        "hpss": cmd_hpss,
        "acoustic": cmd_acoustic,
        "rhythm": cmd_rhythm,
        "dynamics": cmd_dynamics,
        "timbre": cmd_timbre,
        "lufs": cmd_lufs,
        "onset-envelope": cmd_onset_envelope,
        "nnls-chroma": cmd_nnls_chroma,
        "tempogram": cmd_tempogram,
        "plp": cmd_plp,
        "mastering": cmd_mastering,
        "eq": cmd_eq,
        "mastering-processor": cmd_mastering_processor,
        "mastering-processors": cmd_mastering_processors,
        "mastering-pair-processors": cmd_mastering_pair_processors,
        "mastering-pair-analyses": cmd_mastering_pair_analyses,
        "mastering-pair-analyze": cmd_mastering_pair_analyze,
    }

    handler = commands.get(args.command)
    if not handler:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1)

    try:
        sys.exit(handler(args))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
