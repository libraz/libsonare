"""Measurement and spectral-feature commands over an input file."""

from __future__ import annotations

import argparse
import os
from collections.abc import Sequence
from typing import Any

from ._cli_common import (
    _array_stats,
    _color_enabled,
    _load_audio_known_channels,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _c_int,
    _closed_unit_float,
    _ContractArgumentParser,
    _effective,
    _finite_float,
    _given,
    _nonnegative_finite_float,
    _positive_finite_float,
    _positive_int,
    _positive_pitch_frequency,
)

# The tempogram ratio factors the native command reads at.
_TEMPOGRAM_RATIO_FACTORS = (0.5, 1.0, 2.0, 3.0, 4.0)

# Regions the clipping report lists before summarising the rest.
_CLIPPING_REGIONS_SHOWN = 20


def _styled(text: str, *codes: str) -> str:
    """Wrap text in ANSI codes when the terminal takes them."""
    if not _color_enabled():
        return text
    return "".join(codes) + text + "\033[0m"


_BOLD = "\033[1m"
_CYAN = "\033[36m"
_YELLOW = "\033[33m"
_BLUE = "\033[34m"


def _print_file_heading(args: argparse.Namespace, label: str, style: str, suffix: str = "") -> None:
    """The file name and the section label the native report opens with."""
    print()
    print(_styled(os.path.basename(args.file), _CYAN, _BOLD))
    print("  " + _styled(f"> {label}{suffix}", style))


def _stats_line(stats: dict[str, float | int]) -> str:
    return (
        f"  Stats: mean={stats['mean']:.4f}, std={stats['std']:.4f}, "
        f"min={stats['min']:.4f}, max={stats['max']:.4f}"
    )


def cmd_melody(args: argparse.Namespace) -> int:
    from . import analyze_melody

    samples, sr = _load_audio(args.file)
    result = analyze_melody(
        samples,
        sr,
        **_given(args, "threshold", "hop_length", "f0_min_hz", "f0_max_hz"),
    )
    # The contour measures come from voiced frames; with none there is no melody.
    voiced = bool(result.points) and result.mean_frequency_hz > 0.0
    if args.json:
        payload: dict[str, Any] = {}
        if voiced:
            payload["pitch_range_octaves"] = result.pitch_range_octaves
            payload["mean_frequency_hz"] = result.mean_frequency_hz
            payload["pitch_stability"] = result.pitch_stability
            payload["vibrato_rate_hz"] = result.vibrato_rate_hz
        payload["pitch_count"] = len(result.points)
        print(_strict_json_dumps(payload))
    else:
        _print_file_heading(args, "Melody Analysis:", _CYAN)
        if voiced:
            print(f"    Pitch Range:     {result.pitch_range_octaves:.2f} octaves")
            print(f"    Mean Frequency:  {result.mean_frequency_hz:.1f} Hz")
            print(f"    Pitch Stability: {result.pitch_stability:.2f}")
            print(f"    Vibrato Rate:    {result.vibrato_rate_hz:.1f} Hz")
        else:
            print("    no melody")
        print(f"    Pitch Points:    {len(result.points)}")
        print()
    return 0


def cmd_boundaries(args: argparse.Namespace) -> int:
    from . import detect_boundaries

    samples, sr = _load_audio(args.file)
    result = detect_boundaries(
        samples,
        sr,
        **_given(
            args,
            "n_fft",
            "hop_length",
            "kernel_size",
            "threshold",
            "absolute_threshold",
            "n_mfcc",
            "n_chroma",
            peak_distance_sec="peak_distance",
            reference_window_sec="reference_window",
        ),
        use_mfcc=not args.no_mfcc,
        use_chroma=not args.no_chroma,
    )
    bounds = result.boundaries
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "count": len(bounds),
                    "boundaries": [
                        {"time": b.time_sec, "frame": b.frame, "strength": b.strength}
                        for b in bounds
                    ],
                }
            )
        )
    else:
        _print_file_heading(args, "Structural Boundaries", _BLUE, f" ({len(bounds)} detected):")
        for index, b in enumerate(bounds):
            minutes = int(b.time_sec) // 60
            seconds = int(b.time_sec) % 60
            print(f"    {index + 1}. {minutes:02d}:{seconds:02d} (strength {b.strength:.2f})")
        print()
    return 0


def cmd_meter(args: argparse.Namespace) -> int:
    from . import (
        metering_crest_factor_db,
        metering_dc_offset,
        metering_detect_clipping,
        metering_peak_db,
        metering_rms_db,
        metering_silence_ratio,
        metering_true_peak_dbtp,
    )

    samples, sr = _load_audio(args.file)
    peak = metering_peak_db(samples, sr)
    rms = metering_rms_db(samples, sr)
    crest = metering_crest_factor_db(samples, sr)
    # The ratio counts every clipped sample whatever the region length, so the report's
    # own ratio at the requested threshold is the quantity the native meter prints.
    clipping_ratio = metering_detect_clipping(
        samples, sr, **_given(args, threshold_db="clip_threshold_db")
    ).clipping_ratio
    silence = metering_silence_ratio(samples, sr)
    dc_offset = metering_dc_offset(samples, sr)
    true_peak = metering_true_peak_dbtp(samples, sr, **_given(args, oversample_factor="oversample"))
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "peak_db": peak,
                    "rms_db": rms,
                    "crest_factor_db": crest,
                    "true_peak_db": true_peak,
                    "clipping_ratio": clipping_ratio,
                    "silence_ratio": silence,
                    "dc_offset": dc_offset,
                }
            )
        )
    else:
        _print_file_heading(args, "Meters:", _YELLOW)
        print(f"    Peak:           {peak:.2f} dB")
        print(f"    RMS:            {rms:.2f} dB")
        print(f"    Crest Factor:   {crest:.2f} dB")
        print(f"    True Peak:      {true_peak:.2f} dBTP")
        print(f"    Clipping Ratio: {clipping_ratio:.6f}")
        print(f"    Silence Ratio:  {silence:.6f}")
        print(f"    DC Offset:      {dc_offset:.6f}")
        print()
    return 0


def cmd_clipping(args: argparse.Namespace) -> int:
    from . import metering_detect_clipping

    samples, sr = _load_audio(args.file)
    result = metering_detect_clipping(
        samples, sr, **_given(args, "threshold_db", min_region_samples="min_region")
    )
    regions = result.regions
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "clipped_samples": result.clipped_samples,
                    "clipping_ratio": result.clipping_ratio,
                    "max_clipped_peak": result.max_clipped_peak,
                    "regions": [
                        {
                            "start_sample": r.start_sample,
                            "end_sample": r.end_sample,
                            "length": r.length,
                            "peak": r.peak,
                        }
                        for r in regions
                    ],
                }
            )
        )
    else:
        print("Clipping Detection:")
        print(f"  Clipped Samples: {result.clipped_samples}")
        print(f"  Clipping Ratio:  {result.clipping_ratio:.6f}")
        print(f"  Max Peak:        {result.max_clipped_peak:.4f}")
        print(f"  Regions:         {len(regions)}")
        for index, r in enumerate(regions[:_CLIPPING_REGIONS_SHOWN]):
            print(
                f"    [{index}] {r.start_sample} - {r.end_sample} "
                f"(len {r.length}, peak {r.peak:.4f})"
            )
        if len(regions) > _CLIPPING_REGIONS_SHOWN:
            print(f"    ... ({len(regions) - _CLIPPING_REGIONS_SHOWN} more regions)")
    return 0


def cmd_dynamic_range(args: argparse.Namespace) -> int:
    from . import metering_dynamic_range

    samples, sr = _load_audio(args.file)
    result = metering_dynamic_range(
        samples,
        sr,
        **_given(args, "window_sec", "hop_sec", "low_percentile", "high_percentile"),
    )
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "dynamic_range_db": result.dynamic_range_db,
                    "low_percentile_db": result.low_percentile_db,
                    "high_percentile_db": result.high_percentile_db,
                    "window_rms_db": result.window_rms_db.tolist(),
                }
            )
        )
    else:
        print("Dynamic Range:")
        print(f"  Dynamic Range:   {result.dynamic_range_db:.2f} dB")
        print(f"  Low Percentile:  {result.low_percentile_db:.2f} dB")
        print(f"  High Percentile: {result.high_percentile_db:.2f} dB")
    return 0


def _load_reference(args: argparse.Namespace, sample_rate: int, length: int) -> Sequence[float]:
    """The reference channel, which has to match the input's rate and length."""
    if not args.reference:
        raise ValueError("--reference is required")
    samples, reference_rate = _load_audio_known_channels(args.reference, 0)
    if reference_rate != sample_rate:
        raise ValueError("reference sample rate must match input sample rate")
    if len(samples) != length:
        raise ValueError("reference length must match input length")
    return samples


def cmd_stereo(args: argparse.Namespace) -> int:
    from . import metering_stereo_correlation, metering_stereo_width

    left, sr = _load_audio(args.file)
    right = _load_reference(args, sr, len(left))
    correlation = metering_stereo_correlation(left, right, sr)
    width = metering_stereo_width(left, right, sr)
    if args.json:
        print(_strict_json_dumps({"correlation": correlation, "stereo_width": width}))
    else:
        print("Stereo Image:")
        print(f"  Correlation:  {correlation:.4f}")
        print(f"  Stereo Width: {width:.4f}")
    return 0


def cmd_phase(args: argparse.Namespace) -> int:
    from . import metering_phase_scope

    left, sr = _load_audio(args.file)
    right = _load_reference(args, sr, len(left))
    result = metering_phase_scope(left, right, sr)
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "correlation": result.correlation,
                    "average_abs_angle_rad": result.average_abs_angle_rad,
                    "max_radius": result.max_radius,
                }
            )
        )
    else:
        print("Phase Scope:")
        print(f"  Correlation:        {result.correlation:.4f}")
        print(f"  Avg Abs Angle:      {result.average_abs_angle_rad:.4f} rad")
        print(f"  Max Radius:         {result.max_radius:.4f}")
    return 0


def cmd_tonnetz(args: argparse.Namespace) -> int:
    from . import chroma, tonnetz

    samples, sr = _load_audio(args.file)
    chromagram = chroma(samples, sample_rate=sr, **_given(args, "n_fft", "hop_length"))
    values = tonnetz(chromagram.features)
    n_dims, n_frames = values.shape
    stats = _array_stats(values.ravel().tolist(), with_count=False)
    if args.json:
        print(_strict_json_dumps({"n_dims": n_dims, "n_frames": n_frames, "stats": stats}))
    else:
        print("Tonnetz:")
        print(f"  Shape: {n_dims} dims x {n_frames} frames")
        print(_stats_line(stats))
    return 0


def cmd_fourier_tempogram(args: argparse.Namespace) -> int:
    from ._conversions import fourier_tempogram, onset_strength

    samples, sr = _load_audio(args.file)
    env = onset_strength(samples, sr=sr, **_given(args, "hop_length"))
    matrix = fourier_tempogram(env, sr=sr, **_given(args, "hop_length", "win_length"))
    win_length = _effective(args, fourier_tempogram, "win_length")
    n_bins, n_frames = matrix.shape
    stats = _array_stats(matrix.ravel().tolist(), with_count=False)
    if args.json:
        print(
            _strict_json_dumps(
                {"win_length": win_length, "n_bins": n_bins, "n_frames": n_frames, "stats": stats}
            )
        )
    else:
        print("Fourier Tempogram:")
        print(f"  Shape: {n_bins} bins x {n_frames} frames")
        print(_stats_line(stats))
    return 0


def cmd_tempogram_ratio(args: argparse.Namespace) -> int:
    from ._conversions import onset_strength, tempogram, tempogram_ratio

    samples, sr = _load_audio(args.file)
    env = onset_strength(samples, sr=sr, **_given(args, "hop_length"))
    matrix = tempogram(env, sr=sr, **_given(args, "hop_length", "win_length"))
    ratios = tempogram_ratio(
        matrix,
        sr=sr,
        factors=list(_TEMPOGRAM_RATIO_FACTORS),
        **_given(args, "hop_length"),
    ).tolist()
    win_length = _effective(args, tempogram, "win_length")
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "win_length": win_length,
                    "ratios": [
                        {"factor": factor, "value": value}
                        for factor, value in zip(_TEMPOGRAM_RATIO_FACTORS, ratios, strict=True)
                    ],
                }
            )
        )
    else:
        print("Tempogram Ratio:")
        for factor, value in zip(_TEMPOGRAM_RATIO_FACTORS, ratios, strict=True):
            print(f"  factor {factor:.1f}: {value:.4f}")
    return 0


def cmd_cqt(args: argparse.Namespace) -> int:
    from . import cqt

    samples, sr = _load_audio(args.file)
    result = cqt(
        samples, sr=sr, **_given(args, "hop_length", "n_bins", "bins_per_octave", fmin="fmin_hz")
    )
    n_bins, n_frames = result.magnitude.shape
    duration = n_frames * result.hop_length / result.sample_rate if result.sample_rate else 0.0
    fmin = _effective(args, cqt, "fmin", "fmin_hz")
    bins_per_octave = _effective(args, cqt, "bins_per_octave")
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "n_bins": n_bins,
                    "n_frames": n_frames,
                    "duration": duration,
                    "fmin_hz": fmin,
                    "bins_per_octave": bins_per_octave,
                }
            )
        )
    else:
        requested_bins = _effective(args, cqt, "n_bins")
        fmax = fmin * 2.0 ** (requested_bins / bins_per_octave)
        print("Constant-Q Transform:")
        print(f"  Shape:           {n_bins} bins x {n_frames} frames")
        print(
            f"  Frequency Range: {fmin:.1f} - {fmax:.1f} Hz "
            f"({requested_bins // bins_per_octave} octaves)"
        )
        print(f"  Duration:        {duration:.2f}s")
    return 0


def register_feature_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the measurement and spectral-feature commands."""
    stdout_options = shared.stdout_options
    fft_stdout_options = shared.fft_stdout_options

    melody_p = sub.add_parser("melody", parents=[stdout_options], help="Track melody/pitch contour")
    melody_p.add_argument(
        "--threshold",
        type=_positive_finite_float,
        default=None,
        help="YIN threshold (default: 0.1)",
    )
    melody_p.add_argument(
        "--hop-length",
        type=_positive_int,
        default=None,
        help="Hop length in samples (default: 256)",
    )
    melody_p.add_argument(
        "--f0-min-hz",
        type=_positive_pitch_frequency,
        default=None,
        help="Minimum frequency in Hz (default: 65.0)",
    )
    melody_p.add_argument(
        "--f0-max-hz",
        type=_positive_pitch_frequency,
        default=None,
        help="Maximum frequency in Hz (default: 2093.0)",
    )

    boundaries_p = sub.add_parser(
        "boundaries", parents=[stdout_options], help="Detect structural boundaries"
    )
    boundaries_p.add_argument(
        "--threshold",
        type=_nonnegative_finite_float,
        default=None,
        help="Relative novelty threshold (default: 0.3)",
    )
    boundaries_p.add_argument(
        "--absolute-threshold",
        type=_nonnegative_finite_float,
        default=None,
        help="Absolute novelty floor (default: 0.005)",
    )
    boundaries_p.add_argument(
        "--kernel-size",
        type=_positive_int,
        default=None,
        help="Checkerboard kernel size (default: 64)",
    )
    boundaries_p.add_argument(
        "--n-mfcc", type=_positive_int, default=None, help="MFCC coefficients (default: 13)"
    )
    boundaries_p.add_argument(
        "--n-chroma", type=_positive_int, default=None, help="Chroma bins (default: 12)"
    )
    boundaries_p.add_argument(
        "--peak-distance",
        type=_nonnegative_finite_float,
        default=None,
        help="Minimum distance between boundaries in seconds (default: 2.0)",
    )
    boundaries_p.add_argument(
        "--reference-window",
        type=_nonnegative_finite_float,
        default=None,
        help="Window in seconds the novelty reference is taken over; 0 disables (default: 60.0)",
    )
    boundaries_p.add_argument("--no-mfcc", action="store_true", help="Leave the MFCC stream out")
    boundaries_p.add_argument(
        "--no-chroma", action="store_true", help="Leave the chroma stream out"
    )
    boundaries_p.add_argument("--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)")
    boundaries_p.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )

    meter_p = sub.add_parser(
        "meter",
        parents=[stdout_options],
        help="Measure basic level meters (peak/RMS/crest/true-peak)",
    )
    _cli_domain(
        meter_p.add_argument(
            "--clip-threshold-db",
            type=_finite_float,
            default=None,
            help="Clipping level in dBFS (default: the level of a linear 0.999)",
        ),
        minimum=-120,
        maximum=0,
    )
    _cli_domain(
        meter_p.add_argument(
            "--oversample",
            type=_c_int,
            default=None,
            help="True-peak oversampling factor (default: 4)",
        ),
        choices=(1, 2, 4, 8, 16),
    )

    clipping_p = sub.add_parser(
        "clipping", parents=[stdout_options], help="Detect clipped sample regions"
    )
    _cli_domain(
        clipping_p.add_argument(
            "--threshold-db",
            type=_finite_float,
            default=None,
            help="Clipping level in dBFS (default: the level of a linear 0.999)",
        ),
        minimum=-120,
        maximum=0,
    )
    _cli_domain(
        clipping_p.add_argument(
            "--min-region",
            type=_c_int,
            default=None,
            help="Shortest region in samples (default: 1)",
        ),
        minimum=0,
    )

    dynamic_p = sub.add_parser(
        "dynamic-range", parents=[stdout_options], help="Measure dynamic range (percentile RMS)"
    )
    dynamic_p.add_argument(
        "--window-sec",
        type=_positive_finite_float,
        default=None,
        help="Window length in seconds (default: 3.0)",
    )
    dynamic_p.add_argument(
        "--hop-sec",
        type=_positive_finite_float,
        default=None,
        help="Window hop in seconds (default: 1.0)",
    )
    dynamic_p.add_argument(
        "--low-percentile",
        type=_closed_unit_float,
        default=None,
        help="Low percentile in [0, 1] (default: 0.1)",
    )
    dynamic_p.add_argument(
        "--high-percentile",
        type=_closed_unit_float,
        default=None,
        help="High percentile in [0, 1] (default: 0.95)",
    )

    stereo_p = sub.add_parser(
        "stereo",
        parents=[stdout_options],
        help="Measure stereo correlation/width (needs --reference)",
    )
    stereo_p.add_argument("--reference", default=None, help="Audio file holding the other channel")

    phase_p = sub.add_parser(
        "phase",
        parents=[stdout_options],
        help="Measure phase scope summary (needs --reference)",
    )
    phase_p.add_argument("--reference", default=None, help="Audio file holding the other channel")

    sub.add_parser("tonnetz", parents=[fft_stdout_options], help="Compute tonal centroid features")

    fourier_p = sub.add_parser(
        "fourier-tempogram", parents=[stdout_options], help="Compute Fourier tempogram"
    )
    fourier_p.add_argument(
        "--win-length",
        type=_c_int,
        default=None,
        help="Window length in onset frames (default: 384)",
    )
    fourier_p.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )

    ratio_p = sub.add_parser(
        "tempogram-ratio", parents=[stdout_options], help="Compute tempogram ratio features"
    )
    ratio_p.add_argument(
        "--win-length",
        type=_c_int,
        default=None,
        help="Window length in onset frames (default: 384)",
    )
    ratio_p.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )

    cqt_p = sub.add_parser("cqt", parents=[stdout_options], help="Compute Constant-Q Transform")
    _cli_domain(
        cqt_p.add_argument(
            "--n-bins", type=_c_int, default=None, help="Number of bins (default: 84)"
        ),
        minimum=1,
    )
    _cli_domain(
        cqt_p.add_argument(
            "--bins-per-octave", type=_c_int, default=None, help="Bins per octave (default: 12)"
        ),
        minimum=1,
    )
    _cli_domain(
        cqt_p.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length in samples (default: 512)"
        ),
        minimum=1,
        maximum=65536,
    )
    cqt_p.add_argument(
        "--fmin-hz", type=_positive_finite_float, default=None, help="Lowest center frequency in Hz"
    )
