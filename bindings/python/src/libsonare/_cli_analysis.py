"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import sys

from ._cli_common import (
    MODE_NAMES,
    PITCH_NAMES,
    _array_stats,
    _format_time,
    _parse_key_profile,
    _parse_mode,
    _parse_modes,
    _parse_pitch_class,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)


def cmd_version(args: argparse.Namespace) -> int:
    from . import version

    v = version()
    if args.json:
        print(_strict_json_dumps({"lib_version": v, "cli": "python"}))
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
            _strict_json_dumps(
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
        print(_strict_json_dumps({"bpm": round(bpm, 2)}))
    else:
        print(f"  BPM: {bpm:.2f}")
    return 0


def cmd_key(args: argparse.Namespace) -> int:
    from . import detect_key, detect_key_candidates

    samples, sr = _load_audio(args.file)
    # Key detection keeps the historical 4096-sample default (better low-
    # frequency resolution) when --n-fft is left unset, matching the native CLI
    # and detect_key(). An explicit value (including 2048) is respected but a
    # warning is printed when it is below the recommended 4096.
    n_fft = args.n_fft
    if n_fft is None:
        n_fft = 4096
    elif n_fft < 4096:
        print(
            "Warning: key detection prefers --n-fft >= 4096 for better resolution",
            file=sys.stderr,
        )
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
        print(_strict_json_dumps(payload))
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
        print(_strict_json_dumps([round(b, 4) for b in beats]))
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
        print(_strict_json_dumps([round(d, 4) for d in downbeats]))
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
        print(_strict_json_dumps([round(o, 4) for o in onsets]))
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
            _strict_json_dumps(
                {
                    "count": len(result.chords),
                    "chords": [
                        {
                            "name": chord.name,
                            "root": chord.root.value,
                            "quality": chord.quality,
                            # PitchClass.C == 0 is falsy, so a plain ``or`` would
                            # drop a C bass note back to the root. Guard on None.
                            "bass": (chord.root if chord.bass is None else chord.bass).value,
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
            _strict_json_dumps(
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
        samples,
        sample_rate=sr,
        n_fft=args.n_fft,
        hop_length=args.hop_length,
        n_mels=args.n_mels,
        fmin=args.fmin,
        fmax=args.fmax,
        htk=args.htk,
    )
    if args.json:
        print(
            _strict_json_dumps(
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
            _strict_json_dumps(
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

    def _stats(vals: list[float]) -> dict[str, float | int]:
        return _array_stats(vals, digits=4, with_count=False)

    features = {
        "centroid": _stats(spectral_centroid(samples, sr, nf, hl)),
        "bandwidth": _stats(spectral_bandwidth(samples, sr, nf, hl)),
        "rolloff": _stats(spectral_rolloff(samples, sr, nf, hl)),
        "flatness": _stats(spectral_flatness(samples, sr, nf, hl)),
        "zcr": _stats(zero_crossing_rate(samples, sr, nf, hl)),
        "rms": _stats(rms_energy(samples, sr, nf, hl)),
    }

    if args.json:
        print(_strict_json_dumps({"features": features}))
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
            _strict_json_dumps(
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
