"""Room-acoustic commands for the Python command-line interface.

These commands describe a space rather than process a signal: ``acoustic``
reads decay parameters off a recording or an impulse response, ``estimate-room``
infers a geometry from one, and ``synthesize-rir`` and ``room-morph`` render
impulse responses back out.
"""

from __future__ import annotations

import argparse
import inspect
import sys
from typing import Any, cast

from ._cli_common import (
    EXIT_INVALID_PARAMETER,
    _fmt_optional,
    _load_channels_or_downmix,
    _omit_absent,
    _strict_json_dumps,
    _write_channel_output,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _c_int,
    _ContractArgumentParser,
    _finite_float,
    _given,
    _int64,
    _positive_finite_float,
    _positive_int,
)
from ._errors import SonareError

# Upper bound of the receiver spacing, in metres; the library's own limit.
_MAX_RECEIVER_SPACING_M = 4.0


def _print_diagnostic_warnings(result: Any) -> None:
    """Prints one stderr line per warning the acoustic synthesis reported.

    One line per entry rather than one joined line, so the two front-ends print
    the same thing: the native binary walks the diagnostic list the same way, and
    a message holding the join separator would be unreadable on either.
    """
    # Direct attribute access rather than a getattr default: a result type that
    # stopped carrying the field should fail here, not print nothing.
    for diagnostic in result.diagnostics:
        if diagnostic.severity != "warning":
            continue
        print(f"warning: {diagnostic.code}: {diagnostic.message}", file=sys.stderr)


def _float_list(values: Any) -> list[float] | None:
    return None if values is None else [float(value) for value in values]


def _placement(args: argparse.Namespace) -> dict[str, Any]:
    """The source and listener positions, each passed only when one of its axes was spelled.

    An axis left out keeps the library's coordinate: the missing ones are read from
    the library's own default position, never restated here.
    """
    from ._acoustic import synthesize_rir

    defaults = inspect.signature(synthesize_rir).parameters
    placement: dict[str, tuple[float, float, float]] = {}
    for keyword in ("source", "listener"):
        axes = [getattr(args, f"{keyword}_{axis}") for axis in "xyz"]
        if all(value is None for value in axes):
            continue
        base = defaults[keyword].default
        placement[keyword] = cast(
            tuple[float, float, float],
            tuple(base[i] if value is None else value for i, value in enumerate(axes)),
        )
    return placement


def cmd_acoustic(args: argparse.Namespace) -> int:
    from . import analyze_impulse_response, detect_acoustic

    samples, sr = _load_audio(args.file)
    # A setting the caller left out is not passed, so the library chooses it.
    if args.ir:
        result = analyze_impulse_response(
            samples, sample_rate=sr, **_given(args, "n_octave_bands", "min_decay_db")
        )
    else:
        result = detect_acoustic(
            samples,
            sample_rate=sr,
            **_given(args, "n_octave_bands", "min_decay_db", "noise_floor_margin_db"),
        )

    if args.json:
        print(
            _strict_json_dumps(
                _omit_absent(
                    {
                        "rt60": result.rt60,
                        "edt": result.edt,
                        "c50": result.c50,
                        "c80": result.c80,
                        "d50": result.d50,
                        "confidence": result.confidence,
                        "rt60_bands": [float(value) for value in result.rt60_bands],
                        "edt_bands": _float_list(result.edt_bands),
                        "c50_bands": _float_list(result.c50_bands),
                        "c80_bands": _float_list(result.c80_bands),
                    },
                    "edt",
                    "c50",
                    "c80",
                    "d50",
                    "edt_bands",
                    "c50_bands",
                    "c80_bands",
                )
            )
        )
    else:
        mode = "impulse response" if args.ir else "blind"
        print(f"  Acoustic ({mode}):")
        print(f"    RT60:       {result.rt60:.3f} s")
        print(f"    EDT:        {_fmt_optional(result.edt, '.3f', ' s')}")
        print(f"    C50:        {_fmt_optional(result.c50, '.2f', ' dB')}")
        print(f"    C80:        {_fmt_optional(result.c80, '.2f', ' dB')}")
        print(f"    D50:        {_fmt_optional(result.d50, '.3f')}")
        print(f"    Confidence: {result.confidence:.1%}")
    return 0


def cmd_estimate_room(args: argparse.Namespace) -> int:
    from . import estimate_room

    samples, sr = _load_audio(args.file)
    # A setting the caller left out is not passed, so the library chooses it.
    est = estimate_room(
        samples,
        sample_rate=sr,
        prefer_eyring=not args.sabine,
        **_given(
            args,
            "n_octave_bands",
            "min_decay_db",
            "noise_floor_margin_db",
            aspect_hint_lw="aspect_lw",
            aspect_hint_lh="aspect_lh",
            reference_absorption="reference_absorption",
        ),
    )
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "volume": est.volume,
                    "length": est.length_m,
                    "width": est.width_m,
                    "height": est.height_m,
                    "drr_db": est.drr_db,
                    "confidence": est.confidence,
                    "rt60_bands": [float(value) for value in est.rt60_bands],
                    "absorption_bands": [float(value) for value in est.band_absorption],
                }
            )
        )
    else:
        print("  Room estimate:")
        print(f"    Volume:     {est.volume:.1f} m^3")
        print(f"    Dimensions: {est.length_m:.2f} x {est.width_m:.2f} x {est.height_m:.2f} m")
        print(f"    DRR:        {est.drr_db:.2f} dB")
        print(f"    Confidence: {est.confidence:.1%}")
    return 0


def cmd_synthesize_rir(args: argparse.Namespace) -> int:
    from . import synthesize_rir

    if not args.output:
        raise ValueError("synthesize-rir requires --output")
    try:
        result = synthesize_rir(
            sample_rate=args.sample_rate,
            max_sec=args.max_sec,
            prefer_eyring=not args.sabine,
            **_given(
                args,
                "absorption",
                "ism_order",
                "seed",
                length_m="length",
                width_m="width",
                height_m="height",
            ),
            **_placement(args),
        )
    except SonareError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return EXIT_INVALID_PARAMETER
    # Warnings survive a successful synthesis and describe a RIR the caller did
    # not ask for -- a tail cut against max_sec, a cap raised to fit the
    # direct sound, a request reduced to early reflections alone. Dropping them
    # made a truncated RIR indistinguishable from a complete one. They go to
    # stderr in every mode so the JSON document on stdout stays exactly the
    # payload both CLIs publish.
    _print_diagnostic_warnings(result)
    # 24-bit, not the 16-bit default. A synthesized RIR carries its physical
    # 1/(4*pi*d) attenuation, so its peak sits far below full scale and 16-bit
    # quantization would cost the tail roughly 36 dB of the headroom it needs;
    # half the reported samples came back exactly zero.
    _write_wav(args.output, result.rir, result.sample_rate, 24)
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "output": args.output,
                    "samples": len(result.rir),
                    "sample_rate": result.sample_rate,
                }
            )
        )
    else:
        print(f"  Saved RIR ({len(result.rir)} samples) to {args.output}")
    return 0


def cmd_room_morph(args: argparse.Namespace) -> int:
    from . import room_morph, room_morph_stereo

    if not args.output:
        raise ValueError("room-morph requires --output")
    planes, sr = _load_channels_or_downmix(args.file)
    use_stereo = len(planes) == 2
    options: dict[str, Any] = {
        "max_sec": args.max_sec,
        "prefer_eyring": not args.sabine,
        **_given(
            args,
            "absorption",
            "wet",
            "ism_order",
            "seed",
            source_tail_suppression="suppression",
        ),
        **_placement(args),
    }
    geometry = {"length_m": args.length, "width_m": args.width, "height_m": args.height}
    channels: list[list[float]]
    if use_stereo:
        result: Any = room_morph_stereo(
            planes[0],
            planes[1],
            sr,
            **geometry,
            **_given(args, receiver_spacing_m="receiver_spacing"),
            **options,
        )
        channels = [list(result.left), list(result.right)]
    else:
        result = room_morph(planes[0], sr, **geometry, **options)
        channels = [list(result.audio)]
    # The target RIR is synthesized by the code synthesize-rir uses, so the same
    # clamps fire and each one says the morph went through a room the caller did
    # not ask for. stderr in every mode, so the JSON document on stdout stays
    # exactly the payload both CLIs publish.
    _print_diagnostic_warnings(result)
    _write_channel_output(args.output, channels, sr)
    if args.json:
        print(
            _strict_json_dumps(
                {"output": args.output, "samples": len(channels[0]), "stereo": use_stereo}
            )
        )
    else:
        print(f"  Saved morphed audio ({len(channels[0])} samples) to {args.output}")
    return 0


def register_acoustic_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the room-measurement and impulse-response commands."""
    common = shared.common
    stdout_options = shared.stdout_options

    acoustic_p = sub.add_parser(
        "acoustic", parents=[stdout_options], help="Estimate acoustic parameters"
    )
    acoustic_p.add_argument("--ir", action="store_true", help="Treat input as an impulse response")
    acoustic_p.add_argument(
        "--n-octave-bands", type=_c_int, default=None, help="Octave-band count (default: 6)"
    )
    acoustic_p.add_argument(
        "--min-decay-db",
        type=_finite_float,
        default=None,
        help="Minimum decay range for the regression, in dB (default: 30.0)",
    )
    acoustic_p.add_argument(
        "--noise-floor-margin-db",
        type=_finite_float,
        default=None,
        help="Margin above the noise floor, in dB; unused with --ir (default: 10.0)",
    )

    def _add_room_geometry(p: argparse.ArgumentParser, *, library_geometry: bool) -> None:
        # room-morph takes the room's size as required arguments of its facade, so
        # its three dimensions keep the CLI's own values; synthesize-rir leaves
        # them to the library.
        p.add_argument(
            "--length",
            type=_finite_float,
            default=None if library_geometry else 7.0,
            help="Room length (m)" + (" (default: 7.0)" if library_geometry else ""),
        )
        p.add_argument(
            "--width",
            type=_finite_float,
            default=None if library_geometry else 5.0,
            help="Room width (m)" + (" (default: 5.0)" if library_geometry else ""),
        )
        p.add_argument(
            "--height",
            type=_finite_float,
            default=None if library_geometry else 3.0,
            help="Room height (m)" + (" (default: 3.0)" if library_geometry else ""),
        )
        # The room builder clamps rather than validates, so the refusal comes from
        # the material check beside it -- an invalid parameter, not a usage error.
        _cli_domain(
            p.add_argument(
                "--absorption",
                type=_finite_float,
                default=None,
                help="Uniform wall absorption (default: 0.2)",
            ),
            minimum=0,
            maximum=1,
            reject_exit="invalid_parameter",
        )
        p.add_argument(
            "--source-x",
            type=_finite_float,
            default=None,
            help="Source x position (m) (default: 1.0)",
        )
        p.add_argument(
            "--source-y",
            type=_finite_float,
            default=None,
            help="Source y position (m) (default: 1.0)",
        )
        p.add_argument(
            "--source-z",
            type=_finite_float,
            default=None,
            help="Source z position (m) (default: 1.2)",
        )
        p.add_argument(
            "--listener-x",
            type=_finite_float,
            default=None,
            help="Listener x position (m) (default: 5.0)",
        )
        p.add_argument(
            "--listener-y",
            type=_finite_float,
            default=None,
            help="Listener y position (m) (default: 4.0)",
        )
        p.add_argument(
            "--listener-z",
            type=_finite_float,
            default=None,
            help="Listener z position (m) (default: 1.7)",
        )
        p.add_argument(
            "--ism-order",
            type=_c_int,
            default=None,
            help="Image-source reflection order (default: 3)",
        )
        # The C field is a uint32 and 0 is its library-default sentinel, so the
        # range is closed at both ends rather than refusing only a negative.
        _cli_domain(
            p.add_argument(
                "--seed",
                type=_int64,
                default=None,
                help="Deterministic late-tail seed (default: 1)",
            ),
            minimum=0,
            maximum=0xFFFFFFFF,
            reject_exit="invalid_parameter",
        )
        p.add_argument(
            "--max-sec",
            type=_positive_finite_float,
            default=None,
            help="Hard cap on RIR/tail length in seconds (default: natural length)",
        )
        p.add_argument(
            "--sabine",
            action="store_true",
            help="Use the Sabine late-reverb model (default Eyring)",
        )

    estimate_room_p = sub.add_parser(
        "estimate-room", parents=[stdout_options], help="Estimate equivalent room from a recording"
    )
    estimate_room_p.add_argument(
        "--aspect-lw", type=_finite_float, default=None, help="length/width prior (default: 1.0)"
    )
    estimate_room_p.add_argument(
        "--aspect-lh", type=_finite_float, default=None, help="length/height prior (default: 1.0)"
    )
    estimate_room_p.add_argument(
        "--reference-absorption",
        type=_finite_float,
        default=None,
        help="absorption prior (default: 0.15)",
    )
    estimate_room_p.add_argument(
        "--sabine", action="store_true", help="Use the Sabine model (default Eyring)"
    )
    estimate_room_p.add_argument(
        "--n-octave-bands",
        type=_positive_int,
        default=None,
        metavar="N",
        help="Analyzer octave-band count (library default when omitted)",
    )

    synth_rir_p = sub.add_parser(
        "synthesize-rir", parents=[common], help="Synthesize a room impulse response from geometry"
    )
    _add_room_geometry(synth_rir_p, library_geometry=True)
    synth_rir_p.add_argument("--sample-rate", type=_c_int, default=48000, help="Output sample rate")

    room_morph_p = sub.add_parser(
        "room-morph", parents=[common], help="Morph reverberation toward a target room"
    )
    _add_room_geometry(room_morph_p, library_geometry=False)
    room_morph_p.add_argument(
        "--wet", type=_finite_float, default=None, help="Target-room mix [0,1] (default: 0.5)"
    )
    room_morph_p.add_argument(
        "--suppression",
        type=_finite_float,
        default=None,
        help="Source-tail suppression [0,1] (default: 0.5)",
    )
    # A two-channel input is morphed as a pair heard by two receivers this far apart.
    _cli_domain(
        room_morph_p.add_argument(
            "--receiver-spacing",
            type=_finite_float,
            default=None,
            help="Receiver spacing for a stereo input, in metres (0,4] (default: 0.5)",
        ),
        minimum=0,
        exclusive_minimum=True,
        maximum=_MAX_RECEIVER_SPACING_M,
        reject_exit="invalid_parameter",
    )
