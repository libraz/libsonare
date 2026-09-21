"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import math
import statistics
import sys
import wave

import numpy as np

from ._cli_common import (
    MODE_NAMES,
    PITCH_NAMES,
    _color_enabled,
    _format_time,
    _parse_key_profile,
    _parse_meter_candidates,
    _parse_mode,
    _parse_modes,
    _parse_pitch_class,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _candidate_count,
    _finite_float,
    _nonnegative_finite_float,
    _pitch_threshold,
    _positive_int,
    _positive_pitch_frequency,
)


def cmd_version(args: argparse.Namespace) -> int:
    from . import version

    v = version()
    if args.json:
        print(_strict_json_dumps({"cli": "python", "cli_version": v, "lib_version": v}))
    else:
        print(f"libsonare {v} (Python CLI)")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    from .audio import Audio

    samples, sr = _load_audio(args.file)
    values = np.asarray(samples, dtype=np.float64)
    n = int(values.size)
    dur = n / sr if sr else 0.0
    try:
        channels = Audio.file_channel_count(args.file)
    except RuntimeError as exc:
        # ``sonare_audio_file_channel_count`` is additive. Preserve the
        # historical stdlib WAV metadata path when a same-ABI older library
        # lacks that optional symbol; encoded formats still fail rather than
        # reporting a successful but meaningless channels=0.
        if "does not expose sonare_audio_file_channel_count" not in str(exc):
            raise
        try:
            with wave.open(args.file, "rb") as wav:
                channels = wav.getnchannels()
        except (wave.Error, OSError) as wave_exc:
            raise exc from wave_exc
    if channels <= 0:
        raise RuntimeError("libsonare returned an invalid audio channel count")
    peak = float(np.max(np.abs(values))) if n else 0.0
    rms = float(np.sqrt(np.sum(np.square(values), dtype=np.float64) / n)) if n else 0.0

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "path": args.file,
                    "duration": dur,
                    "sample_rate": sr,
                    "channels": channels,
                    "samples": n,
                    "peak_db": 20.0 * math.log10(max(peak, 1e-10)),
                    "rms_db": 20.0 * math.log10(max(rms, 1e-10)),
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
        print(_strict_json_dumps({"bpm": bpm}))
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
    # ``--hpss`` was the historical spelling.  Keep accepting it in handler
    # namespaces while the parser exposes the canonical ``--use-hpss`` flag.
    # A parser alias may bind both spellings to ``use_hpss``; reading both
    # attributes also keeps direct handler callers and older integrations
    # compatible.
    use_hpss = bool(getattr(args, "use_hpss", False) or getattr(args, "hpss", False))
    key_options = {
        "sample_rate": sr,
        "n_fft": n_fft,
        "hop_length": args.hop_length,
        "use_hpss": use_hpss,
        "loudness_weighted": args.loudness_weighted,
        "high_pass_hz": args.high_pass_hz,
        "modes": _parse_modes(args.modes) if args.modes else None,
        "profile": _parse_key_profile(args.profile) if args.profile else None,
        "genre_hint": args.genre_hint or None,
    }
    key = detect_key(samples, **key_options)
    name = f"{PITCH_NAMES[key.root.value]} {MODE_NAMES[key.mode.value]}"
    # The ``true`` shorthand is resolved to a count by the parser, the one place
    # that sees the literal; a negative count clamps to none, as on the native
    # CLI.
    candidate_count = max(0, int(getattr(args, "candidates", 0)))
    candidates = (
        detect_key_candidates(samples, **key_options)[:candidate_count] if candidate_count else []
    )
    if args.json:
        payload: dict[str, object] = {
            "root": key.root.value,
            "mode": key.mode.value,
            "confidence": key.confidence,
            "name": name,
        }
        if candidates:
            payload["candidates"] = [
                {
                    "root": candidate.key.root.value,
                    "mode": candidate.key.mode.value,
                    "confidence": candidate.key.confidence,
                    "name": f"{PITCH_NAMES[candidate.key.root.value]} "
                    f"{MODE_NAMES[candidate.key.mode.value]}",
                    "correlation": candidate.correlation,
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
        print(_strict_json_dumps(list(beats)))
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
        print(_strict_json_dumps(list(downbeats)))
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
        print(_strict_json_dumps(list(onsets)))
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
                    "progression": " - ".join(chord.name for chord in result.chords),
                    "count": len(result.chords),
                    "chords": [
                        {
                            "name": chord.name,
                            "root": chord.root.value,
                            "quality": chord.quality,
                            # PitchClass.C == 0 is falsy, so a plain ``or`` would
                            # drop a C bass note back to the root. Guard on None.
                            "bass": (chord.root if chord.bass is None else chord.bass).value,
                            "start": chord.start,
                            "end": chord.end,
                            "confidence": chord.confidence,
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


def cmd_sections(args: argparse.Namespace) -> int:
    from . import analyze_sections

    samples, sr = _load_audio(args.file)
    result = analyze_sections(
        samples,
        sample_rate=sr,
        n_fft=args.n_fft,
        hop_length=args.hop_length,
        min_section_sec=args.min_duration,
    )
    sections = result.sections
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "count": len(sections),
                    "sections": [
                        {
                            # The spelling `analyze` serializes, so a consumer
                            # matching on `type == "chorus"` need not know which
                            # command produced the document.
                            "type": section.type.name.casefold().replace("_", "-"),
                            "start": section.start,
                            "end": section.end,
                            "energy": section.energy_level,
                            "confidence": section.confidence,
                        }
                        for section in sections
                    ],
                }
            )
        )
    else:
        print(f"  Sections ({len(sections)}):")
        for index, section in enumerate(sections, start=1):
            print(
                f"    {index:2d}. {section.type.name.casefold().replace('_', '-'):<12s} "
                f"({section.start:.2f}s - {section.end:.2f}s, "
                f"energy: {section.energy_level:.2f}, "
                f"confidence: {section.confidence:.0%})"
            )
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    from . import analyze

    samples, sr = _load_audio(args.file)
    candidates = _parse_meter_candidates(args.meter_candidates)
    r = analyze(
        samples,
        sample_rate=sr,
        use_triads_only=not args.with_seventh,
        use_hpss=not args.no_hpss,
        chroma_highpass_hz=args.chroma_highpass,
        # An empty list means the option was not given: pass None so the core
        # seeds its own default rather than being handed a set it rejects.
        meter_candidate_numerators=candidates or None,
        meter_denominator=args.meter_denominator,
    )
    key_name = f"{PITCH_NAMES[r.key.root.value]} {MODE_NAMES[r.key.mode.value]}"

    if args.json:
        beats = [
            {"time": beat.time, "strength": beat.strength if beat.strength is not None else 0.0}
            for beat in r.beats
        ]
        chords = [
            {
                "name": chord.name,
                "start": chord.start,
                "end": chord.end,
                "confidence": chord.confidence,
            }
            for chord in r.chords
        ]
        sections = [
            {
                "type": section.type.name.casefold().replace("_", "-"),
                "start": section.start,
                "end": section.end,
            }
            for section in r.sections
        ]
        timbre = r.timbre
        dynamics = r.dynamics
        rhythm = r.rhythm
        print(
            _strict_json_dumps(
                {
                    "bpm": r.bpm,
                    "bpm_confidence": r.bpm_confidence,
                    "key": {
                        "root": r.key.root.value,
                        "mode": r.key.mode.value,
                        "confidence": r.key.confidence,
                        "name": key_name,
                    },
                    "time_signature": {
                        "numerator": r.time_signature.numerator,
                        "denominator": r.time_signature.denominator,
                        "confidence": r.time_signature.confidence,
                    },
                    "beats": beats,
                    "downbeat_indices": list(r.downbeat_indices),
                    "downbeat_phase": r.downbeat_phase,
                    "chords": chords,
                    "sections": sections,
                    "timbre": {
                        "brightness": timbre.brightness if timbre else 0.0,
                        "warmth": timbre.warmth if timbre else 0.0,
                        "density": timbre.density if timbre else 0.0,
                        "roughness": timbre.roughness if timbre else 0.0,
                        "complexity": timbre.complexity if timbre else 0.0,
                    },
                    "dynamics": {
                        "dynamic_range_db": dynamics.dynamic_range_db if dynamics else 0.0,
                        "loudness_range_db": dynamics.loudness_range_db if dynamics else 0.0,
                        "crest_factor": dynamics.crest_factor if dynamics else 0.0,
                        "is_compressed": dynamics.is_compressed if dynamics else False,
                    },
                    "rhythm": {
                        "syncopation": rhythm.syncopation if rhythm else 0.0,
                        "groove_type": rhythm.groove_type if rhythm else "",
                        "pattern_regularity": rhythm.pattern_regularity if rhythm else 0.0,
                    },
                    "form": r.form,
                }
            )
        )
    else:
        # Match the native CLI: both output streams must be terminals, and
        # NO_COLOR must be absent, before human output gets ANSI sequences.
        use_color = _color_enabled()
        bpm_style = "\033[32m\033[1m" if use_color else ""
        key_style = "\033[35m\033[1m" if use_color else ""
        reset = "\033[0m" if use_color else ""
        print(
            f"\n  {bpm_style}> Estimated BPM : {r.bpm:.2f} BPM  "
            f"(conf {r.bpm_confidence * 100:.1f}%){reset}"
        )
        print(
            f"  {key_style}> Estimated Key : {key_name}  "
            f"(conf {r.key.confidence * 100:.1f}%){reset}"
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
                    "duration": result.n_frames * result.hop_length / result.sample_rate
                    if result.sample_rate > 0
                    else 0.0,
                    "sample_rate": result.sample_rate,
                    "hop_length": result.hop_length,
                    "stats": {
                        "min": round(min(result.power), 6) if result.power else 0.0,
                        "max": round(max(result.power), 6) if result.power else 0.0,
                        "mean": round(sum(result.power) / len(result.power), 6)
                        if result.power
                        else 0.0,
                    },
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
                    "duration": result.n_frames * result.hop_length / result.sample_rate
                    if result.sample_rate > 0
                    else 0.0,
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

    def _stats(vals: list[float]) -> dict[str, float]:
        if not vals:
            return {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
        return {
            "mean": float(statistics.mean(vals)),
            "std": float(statistics.pstdev(vals)) if len(vals) > 1 else 0.0,
            "min": float(min(vals)),
            "max": float(max(vals)),
        }

    centroid = spectral_centroid(samples, sr, nf, hl)
    bandwidth = spectral_bandwidth(samples, sr, nf, hl)
    rolloff = spectral_rolloff(samples, sr, nf, hl)
    flatness = spectral_flatness(samples, sr, nf, hl)
    zcr = zero_crossing_rate(samples, sr, nf, hl)
    rms = rms_energy(samples, sr, nf, hl)
    features = {
        "centroid": _stats(centroid),
        "bandwidth": _stats(bandwidth),
        "rolloff": _stats(rolloff),
        "flatness": _stats(flatness),
        "zcr": _stats(zcr),
        "rms": _stats(rms),
    }

    if args.json:
        print(_strict_json_dumps({"n_frames": len(centroid), "features": features}))
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
    if algo not in {"yin", "pyin"}:
        raise ValueError("--algorithm must be 'yin' or 'pyin'")
    # Keep direct callers that construct the historical three-field Namespace
    # on the old call shape; parser-built pitch invocations always carry the
    # four explicit analysis fields and therefore forward them to the core.
    use_pitch_options = any(
        hasattr(args, name) for name in ("hop_length", "fmin", "fmax", "threshold")
    )
    hop_length = int(getattr(args, "hop_length", 512))
    fmin = float(getattr(args, "fmin", 65.0))
    fmax = float(getattr(args, "fmax", 2093.0))
    threshold = float(getattr(args, "threshold", 0.1))
    if algo == "yin":
        if use_pitch_options:
            result = pitch_yin(
                samples,
                sample_rate=sr,
                hop_length=hop_length,
                fmin=fmin,
                fmax=fmax,
                threshold=threshold,
            )
        else:
            result = pitch_yin(samples, sample_rate=sr)
    else:
        if use_pitch_options:
            result = pitch_pyin(
                samples,
                sample_rate=sr,
                hop_length=hop_length,
                fmin=fmin,
                fmax=fmax,
                threshold=threshold,
            )
        else:
            result = pitch_pyin(samples, sample_rate=sr)

    if args.json:
        voiced_count = sum(1 for flag in result.voiced_flag if flag)
        voiced_ratio = voiced_count / result.n_frames if result.n_frames else math.nan
        print(
            _strict_json_dumps(
                {
                    "algorithm": algo,
                    "n_frames": result.n_frames,
                    "voiced_count": voiced_count,
                    "voiced_ratio": voiced_ratio,
                    "median_f0": result.median_f0,
                    "mean_f0": result.mean_f0,
                }
            )
        )
    else:
        print(f"  Pitch Tracking ({algo}):")
        print(f"    Frames:    {result.n_frames}")
        print(f"    Median F0: {result.median_f0:.1f} Hz")
        print(f"    Mean F0:   {result.mean_f0:.1f} Hz")
    return 0


def register_analysis_parsers(sub: argparse._SubParsersAction, shared: SharedParsers) -> None:
    """Register the core analysis commands on the top-level subparsers."""
    stdout_options = shared.stdout_options
    fft_stdout_options = shared.fft_stdout_options
    mel_options = shared.mel_options

    sub.add_parser("version", parents=[stdout_options], help="Show version")
    sub.add_parser("doctor", parents=[stdout_options], help="Show build and runtime diagnostics")
    sub.add_parser("info", parents=[stdout_options], help="Show audio file information")
    sub.add_parser("bpm", parents=[stdout_options], help="Detect BPM")
    key_p = sub.add_parser("key", parents=[stdout_options], help="Detect musical key")
    # Key detection keeps a 4096-sample analysis default (better low-frequency
    # resolution) rather than the shared 2048. A None sentinel distinguishes
    # "left at the default" from an explicit value, matching the native CLI and
    # the detect_key() library default. These FFT options are declared here
    # instead of inherited from fft_options: parents= shares the actual argument
    # objects, so a set_defaults override would mutate the shared --n-fft used by
    # the other analysis commands.
    key_p.add_argument(
        "--n-fft", type=int, default=4096, help="FFT size (default: 4096 for key analysis)"
    )
    key_p.add_argument("--hop-length", type=int, default=512, help="Hop length (default: 512)")
    key_p.add_argument(
        "--candidates",
        type=_candidate_count,
        default=None,
        metavar="N",
        help="Also show the top N key candidates",
    )
    key_p.add_argument(
        "--use-hpss",
        "--hpss",
        dest="use_hpss",
        action="store_true",
        help="Use harmonic audio for key chroma",
    )
    key_p.add_argument(
        "--loudness-weighted", action="store_true", help="Weight key chroma frames by RMS"
    )
    key_p.add_argument(
        "--high-pass-hz",
        type=_finite_float,
        default=0.0,
        help="High-pass cutoff before key analysis",
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
    sub.add_parser("beats", parents=[stdout_options], help="Detect beat times")
    sub.add_parser("downbeats", parents=[stdout_options], help="Detect downbeat times")
    sub.add_parser("onsets", parents=[stdout_options], help="Detect onset times")
    chords_p = sub.add_parser(
        "chords", parents=[fft_stdout_options], help="Detect chord progression"
    )
    chords_p.add_argument(
        "--min-duration", type=_finite_float, default=0.3, help="Minimum chord duration in seconds"
    )
    _cli_domain(
        chords_p.add_argument(
            "--smoothing-window",
            type=_finite_float,
            default=2.0,
            help="Chroma smoothing window in seconds",
        ),
        minimum=0,
        exclusive_minimum=True,
        reject_exit="invalid_parameter",
    )
    chords_p.add_argument(
        "--threshold", type=_finite_float, default=0.5, help="Chord detection confidence threshold"
    )
    chords_p.add_argument(
        "--triads-only", action="store_true", help="Restrict output to triad qualities"
    )
    chords_p.add_argument(
        "--nnls", action="store_true", help="Use NNLS chroma instead of the STFT chroma"
    )
    chords_p.add_argument(
        "--no-beat-sync", action="store_true", help="Disable beat-synchronous chroma pooling"
    )
    chords_p.add_argument(
        "--use-hmm", action="store_true", help="Decode the chord sequence with an HMM"
    )
    # 0 keeps the full search rather than disabling the decoder, so the range is
    # closed at the bottom and only a negative width is refused.
    _cli_domain(
        chords_p.add_argument(
            "--hmm-beam-width", type=int, default=24, help="HMM decoder beam width"
        ),
        minimum=0,
        reject_exit="invalid_parameter",
    )
    chords_p.add_argument(
        "--key-context", action="store_true", help="Bias detection with a key context"
    )
    chords_p.add_argument(
        "--key-root", default="C", help="Key-context root pitch class (e.g. C, F#)"
    )
    chords_p.add_argument(
        "--key-mode", default="major", help="Key-context mode (e.g. major, minor)"
    )
    chords_p.add_argument(
        "--detect-inversions", action="store_true", help="Report chord inversions (bass note)"
    )
    sections_p = sub.add_parser(
        "sections", parents=[fft_stdout_options], help="Detect song structure"
    )
    sections_p.add_argument(
        "--min-duration",
        type=_finite_float,
        default=4.0,
        help="Minimum section duration in seconds (default: 4.0)",
    )
    analyze_p = sub.add_parser("analyze", parents=[stdout_options], help="Full music analysis")
    analyze_p.add_argument(
        "--with-seventh", action="store_true", help="Include seventh chords in the analysis"
    )
    analyze_p.add_argument(
        "--no-hpss", action="store_true", help="Disable harmonic-percussive separation"
    )
    analyze_p.add_argument(
        "--chroma-highpass",
        type=_nonnegative_finite_float,
        default=80.0,
        help="High-pass cutoff for chroma analysis in Hz (default: 80.0)",
    )
    # An odd meter is only ever reported if its numerator was asked for, so
    # without this option the CLI could not reach one at all.
    analyze_p.add_argument(
        "--meter-candidates",
        type=str,
        default="",
        help="Comma-separated meter numerators to score, e.g. 3,4,5,7 (default: 3,4,6)",
    )
    analyze_p.add_argument(
        "--meter-denominator",
        type=_positive_int,
        default=4,
        help="Beat unit reported for the detected meter (default: 4)",
    )
    mel_p = sub.add_parser("mel", parents=[mel_options], help="Compute mel spectrogram")
    # 0 is the librosa default both bounds are spelled with, so the range is
    # closed at the bottom rather than starting above it. Declared on the action
    # rather than on the type callable: argparse takes any finite number here and
    # the filterbank is what refuses a negative, so the refusal is the
    # invalid-parameter class, not the usage one.
    _cli_domain(
        mel_p.add_argument(
            "--fmin", type=_finite_float, default=0.0, help="Lowest mel band frequency in Hz"
        ),
        minimum=0,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        mel_p.add_argument(
            "--fmax",
            type=_finite_float,
            default=0.0,
            help="Highest mel band frequency in Hz (0 = Nyquist)",
        ),
        minimum=0,
        reject_exit="invalid_parameter",
    )
    mel_p.add_argument(
        "--htk", action="store_true", help="Use the HTK mel formula instead of Slaney"
    )
    sub.add_parser("chroma", parents=[fft_stdout_options], help="Compute chromagram")
    sub.add_parser("spectral", parents=[fft_stdout_options], help="Compute spectral features")
    # Pitch is a stdout-only analysis command. Its frequency/threshold domains
    # mirror the PitchConfig checks in the core API.
    pitch_p = sub.add_parser("pitch", parents=[stdout_options], help="Track pitch")
    # cmd_pitch raises for any other name, so the accepted set is declared here
    # even though argparse itself does not enforce it.
    _cli_domain(
        pitch_p.add_argument("--algorithm", default="pyin"),
        choices=("yin", "pyin"),
        reject_exit="invalid_parameter",
    )
    pitch_p.add_argument(
        "--threshold",
        type=_pitch_threshold,
        default=0.1,
        help="YIN threshold (> 0 and <= 1; default: 0.1)",
    )
    pitch_p.add_argument(
        "--hop-length",
        type=_positive_int,
        default=512,
        help="Hop length in samples (default: 512)",
    )
    pitch_p.add_argument(
        "--fmin",
        type=_positive_pitch_frequency,
        default=65.0,
        help="Minimum frequency in Hz (default: 65.0)",
    )
    pitch_p.add_argument(
        "--fmax",
        type=_positive_pitch_frequency,
        default=2093.0,
        help="Maximum frequency in Hz (default: 2093.0)",
    )
