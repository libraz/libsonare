"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import math
import statistics
from typing import Any

import numpy as np

from ._cli_common import (
    PITCH_NAMES,
    _color_enabled,
    _format_time,
    _parse_key_profile,
    _parse_meter_candidates,
    _parse_mode,
    _parse_modes,
    _parse_pitch_class,
    _section_type_label,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _c_int,
    _candidate_count,
    _ContractArgumentParser,
    _effective,
    _finite_float,
    _given,
    _nonnegative_finite_float,
    _pitch_threshold,
    _positive_finite_float,
    _positive_int,
    _positive_pitch_frequency,
)
from ._errors import _unknown_error
from ._runtime import _mode_value
from ._types_enums import _GENRE_HINTS


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
    # load_library's strict ABI-version equality check means any dylib that
    # loads at all already exposes sonare_audio_file_num_channels.
    channels = Audio.file_num_channels(args.file)
    if channels <= 0:
        raise _unknown_error("libsonare returned an invalid audio channel count")
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
    key_options: dict[str, Any] = {
        "sample_rate": sr,
        "use_hpss": args.hpss,
        "loudness_weighted": args.loudness_weighted,
        **_given(
            args, "n_fft", "hop_length", chroma_high_pass_hz="high_pass_hz", genre_hint="genre_hint"
        ),
    }
    # A value the caller spelled reaches the library as written, an empty one included.
    if args.modes is not None:
        key_options["modes"] = _parse_modes(args.modes)
    if args.profile is not None:
        key_options["profile"] = _parse_key_profile(args.profile)
    key = detect_key(samples, **key_options)
    name = f"{PITCH_NAMES[key.root.value]} {key.mode}"
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
            "mode": _mode_value(key.mode, "key.mode"),
            "confidence": key.confidence,
            "name": name,
        }
        if candidates:
            payload["candidates"] = [
                {
                    "root": candidate.key.root.value,
                    "mode": _mode_value(candidate.key.mode, "key.mode"),
                    "confidence": candidate.key.confidence,
                    "name": f"{PITCH_NAMES[candidate.key.root.value]} {candidate.key.mode}",
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
                candidate_name = f"{PITCH_NAMES[candidate.key.root.value]} {candidate.key.mode}"
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
        print(_strict_json_dumps(beats.tolist()))
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
        print(_strict_json_dumps(downbeats.tolist()))
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
        print(_strict_json_dumps(onsets.tolist()))
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
    key_context: dict[str, Any] = {}
    if args.key_root is not None:
        key_context["key_root"] = _parse_pitch_class(args.key_root)
    if args.key_mode is not None:
        key_context["key_mode"] = _parse_mode(args.key_mode)
    result = detect_chords(
        samples,
        sample_rate=sr,
        use_triads_only=not args.with_seventh,
        use_beat_sync=not args.no_beat_sync,
        use_hmm=not args.no_hmm,
        use_key_context=args.key_context,
        detect_inversions=args.detect_inversions,
        **_given(
            args,
            "n_fft",
            "hop_length",
            "min_duration_sec",
            "threshold",
            "hmm_beam_width",
            "chroma_method",
            smoothing_window_sec="smoothing_window",
        ),
        **key_context,
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
                            "start": chord.start_sec,
                            "end": chord.end_sec,
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
                f"({chord.start_sec:.2f}s - {chord.end_sec:.2f}s, "
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
        threshold=args.threshold,
        **_given(args, "n_fft", "hop_length", min_section_sec="min_duration_sec"),
    )
    sections = result.sections
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "form": result.form,
                    "count": len(sections),
                    "sections": [
                        {
                            # The spelling `analyze` serializes, so a consumer
                            # matching on `type == "chorus"` need not know which
                            # command produced the document.
                            "type": _section_type_label(section.type),
                            "start": section.start_sec,
                            "end": section.end_sec,
                            "energy": section.energy_level,
                            "confidence": section.confidence,
                            "group": section.group,
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
                f"    {index:2d}. {_section_type_label(section.type):<12s} "
                f"({section.start_sec:.2f}s - {section.end_sec:.2f}s, "
                f"energy: {section.energy_level:.2f}, "
                f"confidence: {section.confidence:.0%})"
            )
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    from . import analyze

    samples, sr = _load_audio(args.file)
    # A list the caller spelled reaches the library as written, an empty one included.
    candidates: dict[str, Any] = {}
    if args.meter_candidates is not None:
        candidates["meter_candidate_numerators"] = _parse_meter_candidates(args.meter_candidates)
    r = analyze(
        samples,
        sample_rate=sr,
        use_triads_only=not args.with_seventh,
        use_hpss=args.hpss,
        **_given(args, "chroma_high_pass_hz", "meter_denominator"),
        **candidates,
    )
    key_name = f"{PITCH_NAMES[r.key.root.value]} {r.key.mode}"

    if args.json:
        beats = [{"time": beat.time_sec, "strength": beat.strength} for beat in r.beats]
        # The downbeats are the beats that open a bar; the document spells them
        # as positions in ``beats``.
        # Both lists are decoded from one wire document, so a downbeat carries
        # its beat's time exactly.
        downbeat_times = {beat.time_sec for beat in r.downbeats}
        downbeat_indices = [i for i, beat in enumerate(r.beats) if beat.time_sec in downbeat_times]
        chords = [
            {
                "name": chord.name,
                "start": chord.start_sec,
                "end": chord.end_sec,
                "confidence": chord.confidence,
            }
            for chord in r.chords
        ]
        sections = [
            {
                "type": _section_type_label(section.type),
                "start": section.start_sec,
                "end": section.end_sec,
                "group": section.group,
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
                        "mode": _mode_value(r.key.mode, "key.mode"),
                        "confidence": r.key.confidence,
                        "name": key_name,
                    },
                    "time_signature": {
                        "numerator": r.time_signature.numerator,
                        "denominator": r.time_signature.denominator,
                        "confidence": r.time_signature.confidence,
                    },
                    "beats": beats,
                    "downbeat_indices": downbeat_indices,
                    "downbeat_phase": r.downbeat_phase,
                    "chords": chords,
                    "sections": sections,
                    "timbre": {
                        "brightness": timbre.brightness,
                        "warmth": timbre.warmth,
                        "density": timbre.density,
                        "roughness": timbre.roughness,
                        "complexity": timbre.complexity,
                    },
                    "dynamics": {
                        "dynamic_range_db": dynamics.dynamic_range_db,
                        "loudness_range_lu": dynamics.loudness_range_lu,
                        "crest_factor": dynamics.crest_factor,
                        "is_compressed": dynamics.is_compressed,
                    },
                    "rhythm": {
                        "syncopation": rhythm.syncopation,
                        "groove_type": rhythm.groove_type,
                        "pattern_regularity": rhythm.pattern_regularity,
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
        print(f"  > Beats: {len(r.beats)}")
    return 0


def cmd_mel(args: argparse.Namespace) -> int:
    from . import mel_spectrogram

    samples, sr = _load_audio(args.file)
    result = mel_spectrogram(
        samples,
        sample_rate=sr,
        fmax_hz=args.fmax_hz,
        htk=args.htk,
        **_given(args, "n_fft", "hop_length", "n_mels", "fmin_hz"),
    )
    n_mels, n_frames = result.power.shape
    power = result.power.ravel().tolist()
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "n_mels": n_mels,
                    "n_frames": n_frames,
                    "duration": n_frames * result.hop_length / result.sample_rate
                    if result.sample_rate > 0
                    else 0.0,
                    "sample_rate": result.sample_rate,
                    "hop_length": result.hop_length,
                    "stats": {
                        "min": round(min(power), 6) if power else 0.0,
                        "max": round(max(power), 6) if power else 0.0,
                        "mean": round(sum(power) / len(power), 6) if power else 0.0,
                    },
                }
            )
        )
    else:
        print("  Mel Spectrogram:")
        print(f"    Shape: {n_mels} mels x {n_frames} frames")
    return 0


def cmd_chroma(args: argparse.Namespace) -> int:
    from . import chroma

    samples, sr = _load_audio(args.file)
    result = chroma(samples, sample_rate=sr, **_given(args, "n_fft", "hop_length"))
    n_chroma, n_frames = result.features.shape
    mean_energy = result.mean_energy.tolist()
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "n_chroma": n_chroma,
                    "n_frames": n_frames,
                    "duration": n_frames * result.hop_length / result.sample_rate
                    if result.sample_rate > 0
                    else 0.0,
                    "mean_energy": [round(e, 6) for e in mean_energy],
                }
            )
        )
    else:
        print(f"  Chromagram: {n_chroma} bins x {n_frames} frames")
        print("  Mean energy per pitch class:")
        max_energy = max(mean_energy) if mean_energy else 0
        for i, e in enumerate(mean_energy):
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
    fft = _given(args, "n_fft", "hop_length")
    frames = _given(args, frame_length="n_fft", hop_length="hop_length")

    def _stats(vals: list[float]) -> dict[str, float]:
        if not vals:
            return {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
        return {
            "mean": float(statistics.mean(vals)),
            "std": float(statistics.pstdev(vals)) if len(vals) > 1 else 0.0,
            "min": float(min(vals)),
            "max": float(max(vals)),
        }

    centroid = spectral_centroid(samples, sr, **fft).tolist()
    bandwidth = spectral_bandwidth(samples, sr, **fft).tolist()
    rolloff = spectral_rolloff(samples, sr, **fft).tolist()
    flatness = spectral_flatness(samples, sr, **fft).tolist()
    zcr = zero_crossing_rate(samples, sr, **frames).tolist()
    rms = rms_energy(samples, sr, **frames).tolist()
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
    from . import _features_pitch, pitch_pyin, pitch_yin

    samples, sr = _load_audio(args.file)
    algo = getattr(args, "algorithm", "pyin")
    # Keep direct callers that construct the historical three-field Namespace
    # on the old call shape; parser-built pitch invocations always carry the
    # analysis fields and forward the ones the caller spelled.
    use_pitch_options = any(
        hasattr(args, name) for name in ("hop_length", "f0_min_hz", "f0_max_hz", "threshold")
    )
    track = pitch_yin if algo == "yin" else pitch_pyin
    hop_length = int(
        _effective(
            args,
            _features_pitch.pitch_yin if algo == "yin" else _features_pitch.pitch_pyin,
            "hop_length",
        )
    )
    if use_pitch_options:
        # A bound the caller left out is not passed, so the library chooses it.
        result = track(
            samples,
            sample_rate=sr,
            **_given(args, "hop_length", "threshold", "f0_min_hz", "f0_max_hz"),
        )
    else:
        result = track(samples, sample_rate=sr)

    if args.json:
        n_frames = len(result.f0)
        voiced_count = int(result.voiced_flag.sum())
        voiced_ratio = voiced_count / n_frames if n_frames else math.nan
        payload: dict[str, object] = {
            "algorithm": algo,
            "n_frames": n_frames,
            "voiced_count": voiced_count,
            "voiced_ratio": voiced_ratio,
            "median_f0": result.median_f0,
            "mean_f0": result.mean_f0,
        }
        # The contour is opt-in: one value per frame. An unvoiced frame's
        # non-finite f0 serializes as null.
        if getattr(args, "frames", False):
            payload["times"] = [index * hop_length / sr for index in range(n_frames)]
            payload["f0"] = result.f0.tolist()
            payload["voiced_flag"] = result.voiced_flag.tolist()
            payload["voiced_prob"] = result.voiced_prob.tolist()
        print(_strict_json_dumps(payload))
    else:
        print(f"  Pitch Tracking ({algo}):")
        print(f"    Frames:    {len(result.f0)}")
        print(f"    Median F0: {result.median_f0:.1f} Hz")
        print(f"    Mean F0:   {result.mean_f0:.1f} Hz")
    return 0


def register_analysis_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the core analysis commands on the top-level subparsers."""
    stdout_options = shared.stdout_options
    fft_stdout_options = shared.fft_stdout_options
    mel_options = shared.mel_options

    sub.add_parser("version", parents=[stdout_options], help="Show version")
    sub.add_parser("doctor", parents=[stdout_options], help="Show build and runtime diagnostics")
    sub.add_parser("info", parents=[stdout_options], help="Show audio file information")
    sub.add_parser("bpm", parents=[stdout_options], help="Detect BPM")
    key_p = sub.add_parser("key", parents=[stdout_options], help="Detect musical key")
    # Declared here rather than inherited: key takes no --win-length or --window options.
    _cli_domain(
        key_p.add_argument("--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)"),
        minimum=1,
    )
    key_p.add_argument("--hop-length", type=_c_int, default=None, help="Hop length (default: 512)")
    key_p.add_argument(
        "--candidates",
        type=_candidate_count,
        default=None,
        metavar="N",
        help="Also show the top N key candidates",
    )
    key_p.add_argument(
        "--hpss",
        action="store_true",
        help="Use harmonic audio for key chroma",
    )
    key_p.add_argument(
        "--loudness-weighted", action="store_true", help="Weight key chroma frames by RMS"
    )
    key_p.add_argument(
        "--high-pass-hz",
        type=_finite_float,
        default=None,
        help="High-pass cutoff before key analysis (default: 80.0)",
    )
    key_p.add_argument(
        "--modes",
        type=str,
        default=None,
        help="Candidate modes: major-minor, all, or comma-separated mode names "
        "(default: major-minor)",
    )
    key_p.add_argument(
        "--profile",
        type=str,
        default=None,
        help="Key profile: ks, temperley, shaath, edmt, edma, edmm, or bellman "
        "(default: chosen by --genre-hint)",
    )
    _cli_domain(
        key_p.add_argument(
            "--genre-hint",
            default=None,
            help="Genre hint for key profile selection, e.g. auto, edm, pop, classical, jazz "
            "(default: auto)",
        ),
        choices=_GENRE_HINTS,
    )
    sub.add_parser("beats", parents=[stdout_options], help="Detect beat times")
    sub.add_parser("downbeats", parents=[stdout_options], help="Detect downbeat times")
    sub.add_parser("onsets", parents=[stdout_options], help="Detect onset times")
    chords_p = sub.add_parser("chords", parents=[stdout_options], help="Detect chord progression")
    _cli_domain(
        chords_p.add_argument(
            "--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)"
        ),
        minimum=1,
    )
    chords_p.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )
    chords_p.add_argument(
        "--min-duration-sec",
        type=_finite_float,
        default=None,
        help="Minimum chord duration in seconds (default: 0.3)",
    )
    _cli_domain(
        chords_p.add_argument(
            "--smoothing-window",
            type=_finite_float,
            default=None,
            help="Chroma smoothing window in seconds (default: 2.0)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    chords_p.add_argument(
        "--threshold",
        type=_finite_float,
        default=None,
        help="Chord detection confidence threshold (default: 0.5)",
    )
    chords_p.add_argument(
        "--with-seventh", action="store_true", help="Include seventh chords in the output"
    )
    _cli_domain(
        chords_p.add_argument(
            "--chroma-method",
            default=None,
            help="Chroma extraction: cqt, stft or nnls (default: cqt)",
        ),
        choices=("cqt", "stft", "nnls"),
    )
    chords_p.add_argument(
        "--no-beat-sync", action="store_true", help="Disable beat-synchronous chroma pooling"
    )
    chords_p.add_argument(
        "--no-hmm", action="store_true", help="Pick each chord independently instead of an HMM"
    )
    # 0 keeps the full search rather than disabling the decoder, so the range is
    # closed at the bottom and only a negative width is refused.
    _cli_domain(
        chords_p.add_argument(
            "--hmm-beam-width",
            type=_c_int,
            default=None,
            help="HMM decoder beam width (default: 24)",
        ),
        minimum=0,
    )
    chords_p.add_argument(
        "--key-context", action="store_true", help="Bias detection with a key context"
    )
    chords_p.add_argument(
        "--key-root",
        default=None,
        help="Key-context root pitch class, e.g. C, F# (default: C)",
    )
    chords_p.add_argument(
        "--key-mode",
        default=None,
        help="Key-context mode, e.g. major, minor (default: major)",
    )
    chords_p.add_argument(
        "--detect-inversions", action="store_true", help="Report chord inversions (bass note)"
    )
    sections_p = sub.add_parser(
        "sections", parents=[fft_stdout_options], help="Detect song structure"
    )
    _cli_domain(
        sections_p.add_argument(
            "--min-duration-sec",
            type=_finite_float,
            default=None,
            help="Minimum section duration in seconds (default: 4.0)",
        ),
        minimum=0.0,
    )
    _cli_domain(
        sections_p.add_argument(
            "--threshold",
            type=_finite_float,
            default=None,
            help="Relative boundary threshold (default: 0.22)",
        ),
        minimum=0.0,
    )
    analyze_p = sub.add_parser("analyze", parents=[stdout_options], help="Full music analysis")
    analyze_p.add_argument(
        "--with-seventh", action="store_true", help="Include seventh chords in the analysis"
    )
    analyze_p.add_argument(
        "--hpss", action="store_true", help="Apply harmonic-percussive separation to the chroma"
    )
    analyze_p.add_argument(
        "--chroma-high-pass-hz",
        type=_nonnegative_finite_float,
        default=None,
        help="High-pass cutoff for chroma analysis in Hz (default: 80.0)",
    )
    # An odd meter is only ever reported if its numerator was asked for, so
    # without this option the CLI could not reach one at all.
    analyze_p.add_argument(
        "--meter-candidates",
        type=str,
        default=None,
        help="Comma-separated meter numerators to score, e.g. 3,4,5,7 (default: 3,4,6)",
    )
    analyze_p.add_argument(
        "--meter-denominator",
        type=_positive_int,
        default=None,
        help="Beat unit reported for the detected meter (default: 4)",
    )
    mel_p = sub.add_parser("mel", parents=[mel_options], help="Compute mel spectrogram")
    # A lower band edge of 0 Hz is a real edge, so the range is closed at the bottom.
    _cli_domain(
        mel_p.add_argument(
            "--fmin-hz",
            type=_finite_float,
            default=None,
            help="Lowest mel band frequency in Hz (default: 0.0)",
        ),
        minimum=0,
    )
    mel_p.add_argument(
        "--fmax-hz",
        type=_positive_finite_float,
        default=None,
        help="Highest mel band frequency in Hz (default: the Nyquist frequency)",
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
    )
    pitch_p.add_argument(
        "--frames",
        action="store_true",
        help="With --json, add the per-frame times, f0, voiced_flag and voiced_prob arrays",
    )
    pitch_p.add_argument(
        "--threshold",
        type=_pitch_threshold,
        default=None,
        help="YIN threshold, above 0 and at most 1 (default: 0.1)",
    )
    pitch_p.add_argument(
        "--hop-length",
        type=_positive_int,
        default=None,
        help="Hop length in samples (default: 512)",
    )
    pitch_p.add_argument(
        "--f0-min-hz",
        type=_positive_pitch_frequency,
        default=None,
        help="Minimum frequency in Hz (default: 65.0)",
    )
    pitch_p.add_argument(
        "--f0-max-hz",
        type=_positive_pitch_frequency,
        default=None,
        help="Maximum frequency in Hz (default: 2093.0)",
    )
