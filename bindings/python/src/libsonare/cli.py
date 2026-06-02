"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any, cast

from .types import KeyProfile, Mode, PitchClass

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


def _write_wav_stereo(path: str, left: list[float], right: list[float], sample_rate: int) -> None:
    """Write a stereo 16-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled by 32767.
    """
    import struct
    import wave

    frames = bytearray()
    count = min(len(left), len(right))
    for i in range(count):
        for s in (left[i], right[i]):
            clamped = -1.0 if s < -1.0 else (1.0 if s > 1.0 else s)
            frames += struct.pack("<h", int(round(clamped * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _array_stats(vals: list[float]) -> dict[str, float | int]:
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


def _load_voice_preset_pack(path: str, preset_id: str) -> dict:
    with open(path, encoding="utf-8") as fh:
        pack = json.load(fh)
    presets = pack.get("presets")
    if not isinstance(presets, list):
        raise ValueError("preset pack must contain a presets array")
    matches = [
        preset for preset in presets if isinstance(preset, dict) and preset.get("id") == preset_id
    ]
    if len(matches) > 1:
        raise ValueError(f"duplicate preset id in preset pack: {preset_id}")
    if not matches:
        raise ValueError(f"preset not found in preset pack: {preset_id}")
    return matches[0]


def _parse_voice_set_value(raw: str):
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return raw


def _set_nested_value(root: dict, path: str, value) -> None:
    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("empty --set path")
    cursor = root
    for part in parts[:-1]:
        child = cursor.get(part)
        if not isinstance(child, dict):
            child = {}
            cursor[part] = child
        cursor = child
    cursor[parts[-1]] = value


def _apply_voice_macro_override(root: dict, path: str, value) -> None:
    # Maps the UI macro names (pitch/formant/space/intensity/output) to
    # concrete dsp.* paths so `--set macros.X=...` from the CLI is convenient.
    # CLI-only sugar; the core loader treats `dsp` as authoritative and never
    # derives dsp from macros (see backup/realtime-voice-changer-brushup-plan.md).
    # Keep the mapping in sync with `apply_voice_macro_override` in
    # tools/sonare_cli.cpp.
    if not isinstance(value, (int, float)):
        return
    if path == "macros.pitch":
        _set_nested_value(root, "dsp.retune.semitones", value)
    elif path == "macros.formant":
        _set_nested_value(root, "dsp.formant.factor", value)
    elif path == "macros.space":
        _set_nested_value(root, "dsp.reverb.mix", value)
    elif path == "macros.intensity":
        _set_nested_value(root, "dsp.compressor.ratio", 1.0 + value * 4.0)
    elif path == "macros.output":
        _set_nested_value(root, "dsp.outputGainDb", value)


def _apply_voice_sets(preset: str | dict, assignments: list[str] | None) -> str | dict:
    if not assignments:
        return preset
    root = json.loads(preset) if isinstance(preset, str) else json.loads(json.dumps(preset))
    for group in assignments:
        for assignment in [item for item in group.split(",") if item]:
            if "=" not in assignment:
                raise ValueError(f"invalid --set assignment: {assignment}")
            path, raw = assignment.split("=", 1)
            value = _parse_voice_set_value(raw)
            _set_nested_value(root, path, value)
            _apply_voice_macro_override(root, path, value)
    return root


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
    # Respect the user-supplied --n-fft. Key detection prefers n_fft >= 4096
    # for better low-frequency resolution; warn (but don't silently rewrite)
    # when the caller left the default 2048.
    if args.n_fft < 4096:
        print(
            "Warning: key detection prefers --n-fft >= 4096 for better resolution",
            file=sys.stderr,
        )
    key_options = {
        "sample_rate": sr,
        "n_fft": args.n_fft,
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

    def _stats(vals: list[float]) -> dict[str, float]:
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


def cmd_pitch_correct(args: argparse.Namespace) -> int:
    from . import pitch_correct_to_midi

    samples, sr = _load_audio(args.file)
    result = pitch_correct_to_midi(
        samples,
        sample_rate=sr,
        current_midi=args.current_midi,
        target_midi=args.target_midi,
    )

    if args.output:
        _write_wav(args.output, result, sr)

    if args.json:
        payload: dict[str, object] = {"length": len(result), "sample_rate": sr}
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
    else:
        print(f"  Pitch correct: {len(result)} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_note_stretch(args: argparse.Namespace) -> int:
    from . import note_stretch

    samples, sr = _load_audio(args.file)
    result = note_stretch(
        samples,
        sample_rate=sr,
        onset_sample=args.onset,
        offset_sample=args.offset,
        stretch_ratio=args.ratio,
    )

    if args.output:
        _write_wav(args.output, result, sr)

    if args.json:
        payload: dict[str, object] = {"length": len(result), "sample_rate": sr}
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
    else:
        print(f"  Note stretch: {len(result)} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_voice_change(args: argparse.Namespace) -> int:
    from . import realtime_voice_changer_preset_json, voice_change, voice_change_realtime

    samples, sr = _load_audio(args.file)
    if args.preset or args.preset_json or args.preset_pack or args.set:
        preset: str | dict[str, Any]
        if args.preset_json:
            with open(args.preset_json, encoding="utf-8") as fh:
                preset = json.load(fh)
        elif args.preset_pack:
            preset = _load_voice_preset_pack(args.preset_pack, args.preset or "neutral-monitor")
        elif args.set:
            preset = json.loads(
                realtime_voice_changer_preset_json(args.preset or "neutral-monitor")
            )
        else:
            preset = args.preset
        preset = _apply_voice_sets(preset, args.set)
        result = voice_change_realtime(samples, sample_rate=sr, preset=preset)
    else:
        result = voice_change(
            samples,
            sample_rate=sr,
            pitch_semitones=args.pitch_semitones,
            formant_factor=args.formant_factor,
        )

    if args.output:
        _write_wav(args.output, result, sr)

    if args.json:
        payload: dict[str, object] = {"length": len(result), "sample_rate": sr}
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
    else:
        print(f"  Voice change: {len(result)} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_voice_presets(args: argparse.Namespace) -> int:
    from . import realtime_voice_changer_preset_names

    names = realtime_voice_changer_preset_names()
    if args.json:
        print(json.dumps({"presets": names}))
    else:
        for name in names:
            print(name)
    return 0


def cmd_voice_preset(args: argparse.Namespace) -> int:
    from . import realtime_voice_changer_preset_json

    print(realtime_voice_changer_preset_json(args.preset))
    return 0


def cmd_voice_preset_validate(args: argparse.Namespace) -> int:
    from . import validate_realtime_voice_changer_preset_json

    if args.preset:
        preset = _load_voice_preset_pack(args.file, args.preset)
        preset = _apply_voice_sets(preset, args.set)
        text = json.dumps(preset)
    else:
        with open(args.file, encoding="utf-8") as fh:
            text = fh.read()
        text_or_preset = _apply_voice_sets(text, args.set)
        text = json.dumps(text_or_preset) if isinstance(text_or_preset, dict) else text_or_preset
    result = validate_realtime_voice_changer_preset_json(text)
    print(
        json.dumps(result) if args.json else result.get("normalizedJson", result.get("error", ""))
    )
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


def cmd_estimate_room(args: argparse.Namespace) -> int:
    from . import estimate_room

    samples, sr = _load_audio(args.file)
    est = estimate_room(
        samples,
        sample_rate=sr,
        aspect_hint_lw=args.aspect_lw,
        aspect_hint_lh=args.aspect_lh,
        reference_absorption=args.reference_absorption,
        prefer_eyring=not args.sabine,
    )
    if args.json:
        print(
            json.dumps(
                {
                    "volume": round(est.volume, 3),
                    "length": round(est.length, 3),
                    "width": round(est.width, 3),
                    "height": round(est.height, 3),
                    "drr_db": round(est.drr_db, 3),
                    "confidence": round(est.confidence, 4),
                    "rt60_bands": [round(b, 4) for b in est.rt60_bands],
                    "absorption_bands": [round(b, 4) for b in est.absorption_bands],
                }
            )
        )
    else:
        print("  Room estimate:")
        print(f"    Volume:     {est.volume:.1f} m^3")
        print(f"    Dimensions: {est.length:.2f} x {est.width:.2f} x {est.height:.2f} m")
        print(f"    DRR:        {est.drr_db:.2f} dB")
        print(f"    Confidence: {est.confidence:.1%}")
    return 0


def cmd_synthesize_rir(args: argparse.Namespace) -> int:
    from . import synthesize_rir

    if not args.output:
        print("Error: synthesize-rir requires --output", file=sys.stderr)
        return 1
    result = synthesize_rir(
        args.length,
        args.width,
        args.height,
        source=(args.source_x, args.source_y, args.source_z),
        listener=(args.listener_x, args.listener_y, args.listener_z),
        absorption=args.absorption,
        sample_rate=args.sample_rate,
        ism_order=args.ism_order,
        seed=args.seed,
    )
    if result.has_error:
        print("Error: invalid room geometry (source/listener outside the room)", file=sys.stderr)
        return 1
    _write_wav(args.output, result.rir, result.sample_rate)
    if args.json:
        print(json.dumps({"output": args.output, "samples": len(result.rir)}))
    else:
        print(f"  Saved RIR ({len(result.rir)} samples) to {args.output}")
    return 0


def cmd_room_morph(args: argparse.Namespace) -> int:
    from . import room_morph

    if not args.output:
        print("Error: room-morph requires --output", file=sys.stderr)
        return 1
    samples, sr = _load_audio(args.file)
    result = room_morph(
        samples,
        sr,
        args.length,
        args.width,
        args.height,
        source=(args.source_x, args.source_y, args.source_z),
        listener=(args.listener_x, args.listener_y, args.listener_z),
        absorption=args.absorption,
        source_tail_suppression=args.suppression,
        wet=args.wet,
        ism_order=args.ism_order,
        seed=args.seed,
    )
    _write_wav(args.output, result, sr)
    if args.json:
        print(json.dumps({"output": args.output, "samples": len(result)}))
    else:
        print(f"  Saved morphed audio ({len(result)} samples) to {args.output}")
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
            "gainScale": float(args.gain_scale),
            "outputGainDb": float(args.output_gain_db),
            "outputPan": float(args.output_pan),
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


def cmd_mix(args: argparse.Namespace) -> int:
    from . import Mixer, mixing_scene_preset_json

    # Resolve the scene JSON from either a file or a built-in preset.
    if args.scene:
        with open(args.scene, encoding="utf-8") as fh:
            scene_json = fh.read()
    elif args.preset:
        scene_json = mixing_scene_preset_json(args.preset)
    else:
        raise ValueError("either --scene or --preset is required")

    mixer = Mixer.from_scene_json(
        scene_json, sample_rate=args.sample_rate, block_size=args.block_size
    )
    try:
        strip_count = mixer.strip_count()

        rendered = False
        out_left: list[float] = []
        out_right: list[float] = []
        if args.input:
            # Process each input WAV as one strip (mono inputs are duplicated to
            # both channels). All inputs must share a length.
            left_channels: list[list[float]] = []
            right_channels: list[list[float]] = []
            length: int | None = None
            for path in args.input:
                samples, _sr = _load_audio(path)
                if length is None:
                    length = len(samples)
                elif len(samples) != length:
                    raise ValueError("all --input files must have the same length")
                left_channels.append(list(samples))
                right_channels.append(list(samples))
            if len(left_channels) != strip_count:
                raise ValueError(
                    f"scene has {strip_count} strips but {len(left_channels)} inputs were given"
                )
            mixer.compile()
            out_left, out_right = mixer.process_stereo(left_channels, right_channels)
            rendered = True
            if args.output:
                _write_wav_stereo(args.output, out_left, out_right, args.sample_rate)

        if args.json:
            payload: dict[str, object] = {
                "strip_count": strip_count,
                "sample_rate": args.sample_rate,
                "block_size": args.block_size,
            }
            if rendered:
                payload["rendered_samples"] = len(out_left)
                if args.output:
                    payload["output"] = args.output
            print(json.dumps(payload))
        else:
            print("  Mixer:")
            print(f"    Strips:      {strip_count}")
            print(f"    Sample rate: {args.sample_rate} Hz")
            print(f"    Block size:  {args.block_size}")
            if rendered:
                print(f"    Rendered:    {len(out_left)} samples (stereo)")
                if args.output:
                    print(f"    Wrote: {args.output}")
    finally:
        mixer.close()
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

    # Editing commands
    pitch_correct_p = sub.add_parser(
        "pitch-correct", parents=[common], help="Pitch-correct from a current to a target MIDI note"
    )
    pitch_correct_p.add_argument(
        "--current-midi", type=float, default=69.0, help="Current pitch as a MIDI note number"
    )
    pitch_correct_p.add_argument(
        "--target-midi", type=float, default=69.0, help="Target pitch as a MIDI note number"
    )
    note_stretch_p = sub.add_parser(
        "note-stretch", parents=[common], help="Time-stretch a single note region"
    )
    note_stretch_p.add_argument(
        "--onset", type=int, default=0, help="Start sample index of the note region"
    )
    note_stretch_p.add_argument(
        "--offset", type=int, default=0, help="End sample index of the note region"
    )
    note_stretch_p.add_argument(
        "--ratio", type=float, default=1.0, help="Stretch factor for the region (>1 lengthens)"
    )
    voice_change_p = sub.add_parser(
        "voice-change", parents=[common], help="Apply a voice-change effect"
    )
    voice_change_p.add_argument(
        "--pitch-semitones", type=float, default=0.0, help="Pitch shift in semitones"
    )
    voice_change_p.add_argument(
        "--formant-factor", type=float, default=1.0, help="Formant scaling factor (1.0 = unchanged)"
    )
    voice_change_p.add_argument("--preset", default="", help="Realtime voice changer preset id")
    voice_change_p.add_argument("--preset-json", help="Realtime voice changer preset JSON file")
    voice_change_p.add_argument(
        "--preset-pack", help="Realtime voice changer preset pack JSON file"
    )
    voice_change_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields, e.g. dsp.outputGainDb=-2",
    )
    voice_presets_p = sub.add_parser("voice-presets", help="List realtime voice changer presets")
    voice_presets_p.add_argument("--json", action="store_true", help="Emit JSON")
    voice_preset_p = sub.add_parser("voice-preset", help="Print a realtime voice changer preset")
    voice_preset_p.add_argument("--preset", default="neutral-monitor", help="Preset id")
    voice_preset_p.add_argument("--json", action="store_true", help="Emit JSON")
    voice_preset_validate_p = sub.add_parser(
        "voice-preset-validate", parents=[common], help="Validate and normalize voice preset JSON"
    )
    voice_preset_validate_p.add_argument(
        "--preset", default="", help="Preset id when validating a pack"
    )
    voice_preset_validate_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields before validation",
    )

    # Analysis commands
    acoustic_p = sub.add_parser("acoustic", parents=[common], help="Estimate acoustic parameters")
    acoustic_p.add_argument("--ir", action="store_true", help="Treat input as an impulse response")

    def _add_room_geometry(p: argparse.ArgumentParser) -> None:
        p.add_argument("--length", type=float, default=7.0, help="Room length (m)")
        p.add_argument("--width", type=float, default=5.0, help="Room width (m)")
        p.add_argument("--height", type=float, default=3.0, help="Room height (m)")
        p.add_argument("--absorption", type=float, default=0.2, help="Uniform wall absorption")
        p.add_argument("--source-x", type=float, default=1.0)
        p.add_argument("--source-y", type=float, default=1.0)
        p.add_argument("--source-z", type=float, default=1.2)
        p.add_argument("--listener-x", type=float, default=5.0)
        p.add_argument("--listener-y", type=float, default=4.0)
        p.add_argument("--listener-z", type=float, default=1.7)
        p.add_argument("--ism-order", type=int, default=3, help="Image-source reflection order")
        p.add_argument("--seed", type=int, default=1, help="Deterministic late-tail seed")

    estimate_room_p = sub.add_parser(
        "estimate-room", parents=[common], help="Estimate equivalent room from a recording"
    )
    estimate_room_p.add_argument("--aspect-lw", type=float, default=1.0, help="length/width prior")
    estimate_room_p.add_argument("--aspect-lh", type=float, default=1.0, help="length/height prior")
    estimate_room_p.add_argument(
        "--reference-absorption", type=float, default=0.15, help="absorption prior"
    )
    estimate_room_p.add_argument(
        "--sabine", action="store_true", help="Use the Sabine model (default Eyring)"
    )

    synth_rir_p = sub.add_parser(
        "synthesize-rir", parents=[common], help="Synthesize a room impulse response from geometry"
    )
    _add_room_geometry(synth_rir_p)
    synth_rir_p.add_argument("--sample-rate", type=int, default=48000, help="Output sample rate")

    room_morph_p = sub.add_parser(
        "room-morph", parents=[common], help="Morph reverberation toward a target room"
    )
    _add_room_geometry(room_morph_p)
    room_morph_p.add_argument("--wet", type=float, default=0.5, help="Target-room mix [0,1]")
    room_morph_p.add_argument(
        "--suppression", type=float, default=0.5, help="Source-tail suppression [0,1]"
    )

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
    eq_p.add_argument("--gain-scale", type=float, default=1.0)
    eq_p.add_argument("--output-gain-db", type=float, default=0.0)
    eq_p.add_argument("--output-pan", type=float, default=0.0)
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

    # Mixing commands
    mix_p = sub.add_parser(
        "mix",
        parents=[common],
        help="Load a mixer scene (JSON file or preset) and optionally render inputs",
    )
    mix_group = mix_p.add_mutually_exclusive_group(required=True)
    mix_group.add_argument("--scene", default="", help="Path to a scene JSON file")
    mix_group.add_argument("--preset", default="", help="Built-in scene preset name")
    mix_p.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="WAV",
        help="Per-strip input WAV (repeat once per strip); requires --output to render",
    )
    mix_p.add_argument(
        "--sample-rate", type=int, default=48000, help="Mixer sample rate (default: 48000)"
    )
    mix_p.add_argument(
        "--block-size", type=int, default=512, help="Mixer max block size (default: 512)"
    )

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
        "pitch-correct",
        "note-stretch",
        "voice-change",
        "acoustic",
        "estimate-room",
        "room-morph",
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
        "voice-preset-validate",
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
        "pitch-correct": cmd_pitch_correct,
        "note-stretch": cmd_note_stretch,
        "voice-change": cmd_voice_change,
        "voice-presets": cmd_voice_presets,
        "voice-preset": cmd_voice_preset,
        "voice-preset-validate": cmd_voice_preset_validate,
        "acoustic": cmd_acoustic,
        "estimate-room": cmd_estimate_room,
        "synthesize-rir": cmd_synthesize_rir,
        "room-morph": cmd_room_morph,
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
        "mix": cmd_mix,
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
