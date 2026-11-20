"""Whole-file gain, fade and biquad filter commands."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable, Sequence
from typing import Any

from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_common import (
    _strict_json_dumps,
    _write_wav,
)
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _c_int,
    _ContractArgumentParser,
    _effective,
    _finite_float,
    _given,
    _positive_finite_float,
)
from ._effects_level import BiquadFilterType

# The `--type` tokens, each mapped to the library's `type` spelling.
_FILTER_TYPES: dict[str, BiquadFilterType] = {
    "high-pass": "highPass",
    "low-pass": "lowPass",
    "band-pass": "bandPass",
    "notch": "notch",
}


def _save_result(
    args: argparse.Namespace,
    samples: Sequence[float],
    sample_rate: int,
    fields: dict[str, Any],
) -> int:
    """Write the output WAV and report it: the JSON payload, else a "Saved to" line."""
    _write_wav(args.output, samples, sample_rate)
    if args.json:
        duration = len(samples) / sample_rate if sample_rate > 0 else 0.0
        print(
            _strict_json_dumps({"output": args.output, **fields, "duration": duration}),
        )
    else:
        print(f"Saved to {args.output}", file=sys.stderr)
    return 0


def cmd_gain(args: argparse.Namespace) -> int:
    from . import mastering_apply_processor

    samples, sr = _load_audio(args.file)
    result = mastering_apply_processor(
        "utility.gain", samples, sample_rate=sr, params={"levelDb": args.gain_db}
    )
    return _save_result(args, result.samples, result.sample_rate, {"gain_db": args.gain_db})


def cmd_fade(args: argparse.Namespace) -> int:
    from . import fade

    samples, sr = _load_audio(args.file)
    # A side left out is no fade, which the library spells as a length of 0.
    fade_in_sec = 0.0 if args.fade_in_sec is None else args.fade_in_sec
    fade_out_sec = 0.0 if args.fade_out_sec is None else args.fade_out_sec
    result = fade(samples, sr, fade_in_sec=fade_in_sec, fade_out_sec=fade_out_sec)
    return _save_result(
        args, result, sr, {"fade_in_sec": fade_in_sec, "fade_out_sec": fade_out_sec}
    )


def cmd_filter(args: argparse.Namespace) -> int:
    from . import biquad_filter

    samples, sr = _load_audio(args.file)
    # An omitted order is the library's own default; the payload reports it.
    order = _effective(args, biquad_filter, "order")
    result = biquad_filter(
        samples,
        sr,
        type=_FILTER_TYPES[args.type],
        **_given(args, "order"),
        zero_phase=args.zero_phase,
        cutoff_hz=args.cutoff_hz,
        center_hz=args.center_hz,
        bandwidth_hz=args.bandwidth_hz,
    )
    return _save_result(
        args,
        result,
        sr,
        {
            "type": args.type,
            "order": order,
            "cutoff_hz": args.cutoff_hz,
            "center_hz": args.center_hz,
            "bandwidth_hz": args.bandwidth_hz,
            "zero_phase": bool(args.zero_phase),
        },
    )


def _emphasis(args: argparse.Namespace, apply: Callable[..., Any]) -> int:
    samples, sr = _load_audio(args.file)
    coef = _effective(args, apply, "coef")
    result = apply(samples, **_given(args, "coef"))
    _write_wav(args.output, result, sr)
    if args.json:
        print(_strict_json_dumps({"output": args.output, "coef": coef, "samples": len(result)}))
    else:
        print(f"Saved to {args.output}", file=sys.stderr)
    return 0


def cmd_preemphasis(args: argparse.Namespace) -> int:
    from . import preemphasis

    return _emphasis(args, preemphasis)


def cmd_deemphasis(args: argparse.Namespace) -> int:
    from . import deemphasis

    return _emphasis(args, deemphasis)


def _mel_geometry(args: argparse.Namespace) -> dict[str, Any]:
    """The mel analysis geometry, with the hop pinned to the analysis default the inverse shares."""
    from . import mel_spectrogram

    return {
        "n_fft": _effective(args, mel_spectrogram, "n_fft"),
        "hop_length": _effective(args, mel_spectrogram, "hop_length"),
        "n_mels": _effective(args, mel_spectrogram, "n_mels"),
    }


def cmd_mel_to_audio(args: argparse.Namespace) -> int:
    from . import mel_spectrogram, mel_to_audio

    samples, sr = _load_audio(args.file)
    geometry = _mel_geometry(args)
    edges = _given(args, "fmin_hz", "fmax_hz")
    mel = mel_spectrogram(samples, sr, **geometry, **edges)
    n_iter = _effective(args, mel_to_audio, "n_iter")
    result = mel_to_audio(
        mel.power,
        sr=sr,
        n_fft=geometry["n_fft"],
        hop_length=geometry["hop_length"],
        **_given(args, n_iter="n_iter", fmin="fmin_hz", fmax="fmax_hz"),
    )
    n_mels, n_frames = mel.power.shape
    _write_wav(args.output, result, sr)
    return _report_inverse(
        args, len(result) / sr, {"n_mels": n_mels, "n_frames": n_frames, "n_iter": n_iter}
    )


def cmd_mfcc_to_audio(args: argparse.Namespace) -> int:
    from . import mfcc, mfcc_to_audio

    samples, sr = _load_audio(args.file)
    geometry = _mel_geometry(args)
    n_mfcc = _effective(args, mfcc, "n_mfcc")
    n_iter = _effective(args, mfcc_to_audio, "n_iter")
    edges = _given(args, fmin="fmin_hz", fmax="fmax_hz")
    coefficients = mfcc(samples, sr, **geometry, n_mfcc=n_mfcc, **edges).coefficients
    result = mfcc_to_audio(
        coefficients,
        n_mels=geometry["n_mels"],
        sr=sr,
        n_fft=geometry["n_fft"],
        hop_length=geometry["hop_length"],
        n_iter=n_iter,
        **edges,
    )
    _write_wav(args.output, result, sr)
    return _report_inverse(
        args,
        len(result) / sr,
        {"n_mfcc": n_mfcc, "n_frames": coefficients.shape[1], "n_iter": n_iter},
    )


def _report_inverse(args: argparse.Namespace, duration: float, fields: dict[str, Any]) -> int:
    if args.json:
        print(_strict_json_dumps({"output": args.output, **fields, "duration": duration}))
    else:
        print(f"Saved to {args.output}", file=sys.stderr)
    return 0


def register_dsp_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the gain, fade, filter, emphasis and mel/MFCC inversion commands."""
    common = shared.common

    gain_p = sub.add_parser("gain", parents=[common], help="Apply gain in dB")
    gain_p.add_argument("--gain-db", type=_finite_float, help="Gain in dB (negative attenuates)")

    fade_p = sub.add_parser("fade", parents=[common], help="Apply fade in/out")
    _cli_domain(
        fade_p.add_argument(
            "--fade-in-sec", type=_finite_float, default=None, help="Fade-in length in seconds"
        ),
        minimum=0,
    )
    _cli_domain(
        fade_p.add_argument(
            "--fade-out-sec", type=_finite_float, default=None, help="Fade-out length in seconds"
        ),
        minimum=0,
    )

    filter_p = sub.add_parser(
        "filter",
        parents=[common],
        help="Apply a biquad filter (high-pass, low-pass, band-pass, notch)",
    )
    _cli_domain(
        filter_p.add_argument("--type", help="Filter type"),
        choices=tuple(_FILTER_TYPES),
    )
    _cli_domain(
        filter_p.add_argument(
            "--order",
            type=_c_int,
            default=None,
            help="Filter order; 4 cascades two sections (default: 2)",
        ),
        choices=(2, 4),
    )
    _cli_domain(
        filter_p.add_argument(
            "--cutoff-hz",
            type=_finite_float,
            default=None,
            help="High-pass / low-pass corner in Hz",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        filter_p.add_argument(
            "--center-hz", type=_finite_float, default=None, help="Band-pass / notch center in Hz"
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        filter_p.add_argument(
            "--bandwidth-hz",
            type=_finite_float,
            default=None,
            help="Band-pass / notch bandwidth in Hz",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    filter_p.add_argument(
        "--zero-phase",
        action="store_true",
        help="Run the filter forward and backward, which cancels the phase shift",
    )

    for name, help_text in (
        ("preemphasis", "Apply pre-emphasis filtering"),
        ("deemphasis", "Apply de-emphasis filtering"),
    ):
        emphasis_p = sub.add_parser(name, parents=[common], help=help_text)
        emphasis_p.add_argument(
            "--coef", type=_finite_float, default=None, help="Filter coefficient (default: 0.97)"
        )

    for name, help_text in (
        ("mel-to-audio", "Reconstruct audio from a mel spectrogram"),
        ("mfcc-to-audio", "Reconstruct audio from MFCC"),
    ):
        inverse_p = sub.add_parser(name, parents=[common], help=help_text)
        if name == "mfcc-to-audio":
            _cli_domain(
                inverse_p.add_argument(
                    "--n-mfcc", type=_c_int, default=None, help="MFCC count (default: 20)"
                ),
                minimum=1,
            )
        _cli_domain(
            inverse_p.add_argument(
                "--n-iter", type=_c_int, default=None, help="Griffin-Lim iterations (default: 32)"
            ),
            minimum=1,
        )
        inverse_p.add_argument(
            "--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)"
        )
        inverse_p.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
        )
        inverse_p.add_argument(
            "--n-mels", type=_c_int, default=None, help="Number of mel bands (default: 128)"
        )
        _cli_domain(
            inverse_p.add_argument(
                "--fmin-hz",
                type=_finite_float,
                default=None,
                help="Lowest mel band frequency in Hz (default: 0.0)",
            ),
            minimum=0,
        )
        inverse_p.add_argument(
            "--fmax-hz",
            type=_positive_finite_float,
            default=None,
            help="Highest mel band frequency in Hz (default: the Nyquist frequency)",
        )
