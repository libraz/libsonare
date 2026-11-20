"""Whole-file gain, fade and biquad filter commands."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
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


def register_dsp_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the gain, fade and filter commands."""
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
