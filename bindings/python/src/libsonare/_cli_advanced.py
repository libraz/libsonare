"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import statistics

from ._cli_common import (
    PITCH_NAMES,
    _absent_if_none,
    _array_stats,
    _fmt_optional,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _c_int,
    _ContractArgumentParser,
    _effective,
    _finite_float,
    _given,
    _tempo_bpm,
)

# The library's constant-Q hop bound, which keeps the bin-0 filter a finite length.
_MAX_CQT_HOP_LENGTH = 65536


def cmd_rhythm(args: argparse.Namespace) -> int:
    from . import analyze_rhythm

    samples, sr = _load_audio(args.file)
    r = analyze_rhythm(
        samples,
        sample_rate=sr,
        **_given(args, "start_bpm", "bpm_min", "bpm_max", "n_fft", "hop_length"),
    )

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "bpm": _absent_if_none(r.bpm),
                    "time_signature": {
                        "numerator": r.time_signature.numerator,
                        "denominator": r.time_signature.denominator,
                        "confidence": r.time_signature.confidence,
                    },
                    "groove_type": r.groove_type,
                    "syncopation": _absent_if_none(r.syncopation),
                    "pattern_regularity": _absent_if_none(r.pattern_regularity),
                    "tempo_stability": _absent_if_none(r.tempo_stability),
                    "beat_intervals": _native_array_stats(r.beat_intervals.tolist()),
                }
            )
        )
    else:
        print("  Rhythm:")
        print(f"    BPM:                {_fmt_optional(r.bpm, '.2f')}")
        print(
            f"    Time signature:     {r.time_signature.numerator}/{r.time_signature.denominator}"
        )
        print(f"    Groove:             {r.groove_type}")
        print(f"    Syncopation:        {_fmt_optional(r.syncopation, '.4f')}")
        print(f"    Pattern regularity: {_fmt_optional(r.pattern_regularity, '.4f')}")
        print(f"    Tempo stability:    {_fmt_optional(r.tempo_stability, '.4f')}")
        print(f"    Beat intervals:     {len(r.beat_intervals)}")
    return 0


def _native_array_stats(values: list[float]) -> dict[str, float | int]:
    """Return the unrounded population statistics used by native CLI JSON."""
    if not values:
        return {"count": 0, "mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
    mean = statistics.mean(values)
    return {
        "count": len(values),
        "mean": mean,
        "std": statistics.pstdev(values) if len(values) > 1 else 0.0,
        "min": min(values),
        "max": max(values),
    }


def cmd_dynamics(args: argparse.Namespace) -> int:
    from . import analyze_dynamics

    samples, sr = _load_audio(args.file)
    r = analyze_dynamics(samples, sample_rate=sr, **_given(args, "window_sec", "hop_length"))

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "dynamic_range_db": _absent_if_none(r.dynamic_range_db),
                    "peak_db": r.peak_db,
                    "rms_db": r.rms_db,
                    "crest_factor": r.crest_factor,
                    "loudness_range_lu": _absent_if_none(r.loudness_range_lu),
                    "is_compressed": r.is_compressed,
                    "loudness": _array_stats(r.loudness_rms_db.tolist()),
                }
            )
        )
    else:
        print("  Dynamics:")
        print(f"    Dynamic range:  {_fmt_optional(r.dynamic_range_db, '.2f', ' dB')}")
        print(f"    Peak:           {r.peak_db:.2f} dB")
        print(f"    RMS:            {r.rms_db:.2f} dB")
        print(f"    Crest factor:   {_fmt_optional(r.crest_factor, '.4f')}")
        print(f"    Loudness range: {_fmt_optional(r.loudness_range_lu, '.2f', ' dB')}")
        print(f"    Compressed:     {r.is_compressed}")
    return 0


def cmd_timbre(args: argparse.Namespace) -> int:
    from . import analyze_timbre

    samples, sr = _load_audio(args.file)
    r = analyze_timbre(samples, sample_rate=sr, **_given(args, "n_fft", "hop_length", "n_mels"))

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "brightness": _absent_if_none(r.brightness),
                    "warmth": _absent_if_none(r.warmth),
                    "density": _absent_if_none(r.density),
                    "roughness": _absent_if_none(r.roughness),
                    "complexity": _absent_if_none(r.complexity),
                    "spectral_centroid": _array_stats(r.spectral_centroid.tolist()),
                    "spectral_flatness": _array_stats(r.spectral_flatness.tolist()),
                    "spectral_rolloff": _array_stats(r.spectral_rolloff.tolist()),
                }
            )
        )
    else:
        print("  Timbre:")
        print(f"    Brightness: {_fmt_optional(r.brightness, '.4f')}")
        print(f"    Warmth:     {_fmt_optional(r.warmth, '.4f')}")
        print(f"    Density:    {_fmt_optional(r.density, '.4f')}")
        print(f"    Roughness:  {_fmt_optional(r.roughness, '.4f')}")
        print(f"    Complexity: {_fmt_optional(r.complexity, '.4f')}")
    return 0


def cmd_lufs(args: argparse.Namespace) -> int:
    from ._cli_common import _load_audio_channels
    from ._cli_playback import _interleave_planes
    from ._features import lufs_interleaved, lufs_series_interleaved

    # Every source channel, summed per ITU-R BS.1770, so a stereo file reads the
    # value `master` reports for it rather than the downmix's.
    planes, sr = _load_audio_channels(args.file)
    channels = len(planes)
    samples = _interleave_planes(planes)
    r = lufs_interleaved(samples, channels, sample_rate=sr)

    payload: dict[str, object] = {
        "integrated_lufs": _absent_if_none(r.integrated_lufs),
        "momentary_lufs": _absent_if_none(r.momentary_lufs),
        "short_term_lufs": _absent_if_none(r.short_term_lufs),
        "loudness_range_lu": _absent_if_none(r.loudness_range_lu),
    }
    series = lufs_series_interleaved(samples, channels, sample_rate=sr) if args.series else None
    if args.json:
        if series is not None:
            payload["momentary_series"] = [float(value) for value in series.momentary_lufs]
            payload["short_term_series"] = [float(value) for value in series.short_term_lufs]
        print(_strict_json_dumps(payload))
    else:
        print("  Loudness (LUFS):")
        print(f"    Integrated:     {r.integrated_lufs:.2f} LUFS")
        print(f"    Momentary:      {_fmt_optional(r.momentary_lufs, '.2f', ' LUFS')}")
        print(f"    Short-term:     {_fmt_optional(r.short_term_lufs, '.2f', ' LUFS')}")
        print(f"    Loudness range: {_fmt_optional(r.loudness_range_lu, '.2f', ' LU')}")
        if series is not None:
            print(f"    Momentary samples:  {len(series.momentary_lufs)}")
            print(f"    Short-term samples: {len(series.short_term_lufs)}")
    return 0


def cmd_onset_strength(args: argparse.Namespace) -> int:
    from ._conversions import onset_strength

    samples, sr = _load_audio(args.file)
    env = onset_strength(samples, sr=sr, **_given(args, "n_fft", "hop_length", "n_mels")).tolist()
    hop_length = _effective(args, onset_strength, "hop_length")
    # The time of the first frame holding the maximum; 0 for an empty envelope, as the stats are.
    peak_frame = env.index(max(env)) if env else 0
    peak_time = peak_frame * hop_length / sr if sr > 0 else 0.0

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "count": len(env),
                    "hop_length": hop_length,
                    # The analyzed audio's duration, not the frame grid's span.
                    "duration": len(samples) / sr if sr > 0 else 0.0,
                    "peak_time": peak_time,
                    "stats": _array_stats(env, with_count=False),
                    "values": [round(v, 6) for v in env],
                }
            )
        )
    else:
        stats = _array_stats(env)
        print("  Onset envelope:")
        print(f"    Frames: {stats['count']}")
        print(f"    Mean:   {stats['mean']:.6f}")
        print(f"    Max:    {stats['max']:.6f}")
        print(f"    Peak:   {peak_time:.2f}s")
    return 0


def cmd_nnls_chroma(args: argparse.Namespace) -> int:
    from ._conversions import nnls_chroma

    samples, sr = _load_audio(args.file)
    chroma_rows = nnls_chroma(samples, sample_rate=sr, **_given(args, "hop_length"))
    hop_length = _effective(args, nnls_chroma, "hop_length")
    n_chroma = 12
    n_frames = chroma_rows.shape[1]
    # One row per pitch class; mean energy per bin.
    mean_energy = [sum(row) / len(row) if row else 0.0 for row in chroma_rows.tolist()]

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "n_chroma": n_chroma,
                    "n_frames": n_frames,
                    "duration": n_frames * hop_length / sr if sr > 0 else 0.0,
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
    from ._conversions import onset_strength, tempogram

    samples, sr = _load_audio(args.file)
    env = onset_strength(samples, sr=sr, **_given(args, "n_fft", "hop_length", "n_mels"))
    matrix = tempogram(env, sr=sr, **_given(args, "hop_length", "win_length"))
    n_frames = matrix.shape[1]
    data = matrix.ravel().tolist()
    win_length = args.win_length if args.win_length is not None else matrix.shape[0]

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "win_length": win_length,
                    "n_frames": n_frames,
                    "stats": _array_stats(data, with_count=False),
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
    from ._conversions import onset_strength, plp

    samples, sr = _load_audio(args.file)
    env = onset_strength(samples, sr=sr, **_given(args, "n_fft", "hop_length", "n_mels"))
    # plp keeps librosa's keyword names; an omitted bound keeps the library's range.
    pulse = plp(
        env,
        sr=sr,
        **_given(args, "hop_length", "win_length", tempo_min="bpm_min", tempo_max="bpm_max"),
    ).tolist()

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "n_frames": len(pulse),
                    "stats": _array_stats(pulse, with_count=False),
                }
            )
        )
    else:
        stats = _array_stats(pulse)
        print("  Predominant local pulse (PLP):")
        print(f"    Frames: {stats['count']}")
        print(f"    Mean:   {stats['mean']:.6f}")
        print(f"    Max:    {stats['max']:.6f}")
    return 0


def register_advanced_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the rhythm, dynamics and spectral descriptor commands."""
    stdout_options = shared.stdout_options
    fft_stdout_options = shared.fft_stdout_options
    mel_options = shared.mel_options

    rhythm_p = sub.add_parser(
        "rhythm", parents=[fft_stdout_options], help="Analyze rhythm primitives"
    )
    rhythm_p.add_argument(
        "--start-bpm",
        type=_tempo_bpm,
        default=None,
        help="Tempo reported when nothing is detected (default: 120.0)",
    )
    rhythm_p.add_argument(
        "--bpm-min", type=_tempo_bpm, default=None, help="Lowest tempo in BPM (default: 60.0)"
    )
    rhythm_p.add_argument(
        "--bpm-max", type=_tempo_bpm, default=None, help="Highest tempo in BPM (default: 200.0)"
    )
    dynamics_p = sub.add_parser(
        "dynamics", parents=[stdout_options], help="Analyze dynamics/loudness"
    )
    _cli_domain(
        dynamics_p.add_argument(
            "--window-sec",
            type=_finite_float,
            default=None,
            help="Loudness window in seconds (default: 0.4)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    # Dynamics windows the loudness series but runs no FFT, so it takes the hop
    # control without the matching --n-fft.
    _cli_domain(
        dynamics_p.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
        ),
        minimum=1,
    )
    sub.add_parser("timbre", parents=[mel_options], help="Analyze timbre/spectral shape")
    lufs_p = sub.add_parser("lufs", parents=[stdout_options], help="Compute LUFS loudness")
    lufs_p.add_argument(
        "--series", action="store_true", help="Also emit momentary/short-term LUFS series"
    )
    sub.add_parser(
        "onset-strength", parents=[mel_options], help="Compute the onset strength envelope"
    )
    nnls_p = sub.add_parser("nnls-chroma", parents=[stdout_options], help="Compute NNLS chroma")
    _cli_domain(
        nnls_p.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
        ),
        minimum=1,
        maximum=_MAX_CQT_HOP_LENGTH,
    )
    tempogram_p = sub.add_parser(
        "tempogram", parents=[mel_options], help="Compute autocorrelation tempogram"
    )
    tempogram_p.add_argument(
        "--win-length", type=_c_int, default=None, help="Autocorrelation window (default: 384)"
    )
    plp_p = sub.add_parser("plp", parents=[mel_options], help="Compute predominant local pulse")
    plp_p.add_argument(
        "--bpm-min",
        type=_tempo_bpm,
        help="Lower bound of the tempo search (omitted: the library's range)",
    )
    plp_p.add_argument(
        "--bpm-max",
        type=_tempo_bpm,
        help="Upper bound of the tempo search (omitted: the library's range)",
    )
    plp_p.add_argument(
        "--win-length", type=_c_int, default=None, help="Autocorrelation window (default: 384)"
    )
