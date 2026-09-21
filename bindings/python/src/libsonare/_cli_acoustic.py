"""Room-acoustic commands for the Python command-line interface.

These commands describe a space rather than process a signal: ``acoustic``
reads decay parameters off a recording or an impulse response, ``estimate-room``
infers a geometry from one, and ``synthesize-rir`` and ``room-morph`` render
impulse responses back out.
"""

from __future__ import annotations

import argparse
import sys
from typing import Any, cast

from ._cli_common import (
    EXIT_INVALID_PARAMETER,
    _legacy_exit_codes,
    _strict_json_dumps,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import SharedParsers, _ContractArgumentParser, _finite_float


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


def cmd_acoustic(args: argparse.Namespace) -> int:
    from . import analyze_impulse_response, detect_acoustic

    samples, sr = _load_audio(args.file)
    n_bands = cast(int | None, getattr(args, "n_bands", None))
    if n_bands is None:
        n_bands = cast(int, getattr(args, "n_octave_bands", 6))
    min_decay_db = getattr(args, "min_decay_db", 30.0)
    noise_floor_margin_db = getattr(args, "noise_floor_margin_db", 10.0)
    if args.ir:
        result = analyze_impulse_response(
            samples,
            sample_rate=sr,
            n_octave_bands=n_bands,
            min_decay_db=min_decay_db,
        )
    else:
        result = detect_acoustic(
            samples,
            sample_rate=sr,
            n_octave_bands=n_bands,
            min_decay_db=min_decay_db,
            noise_floor_margin_db=noise_floor_margin_db,
        )

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "rt60": result.rt60,
                    "edt": result.edt,
                    "c50": result.c50,
                    "c80": result.c80,
                    "d50": result.d50,
                    "confidence": result.confidence,
                    "is_blind": result.is_blind,
                    "rt60_bands": [float(value) for value in result.rt60_bands],
                    "edt_bands": [float(value) for value in result.edt_bands],
                    "c50_bands": [float(value) for value in result.c50_bands],
                    "c80_bands": [float(value) for value in result.c80_bands],
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
    n_bands = cast(int | None, getattr(args, "n_bands", None))
    if n_bands is None:
        n_bands = cast(int | None, getattr(args, "n_octave_bands", None))
    if n_bands is None:
        n_bands = 0
    est = estimate_room(
        samples,
        sample_rate=sr,
        aspect_hint_lw=args.aspect_lw,
        aspect_hint_lh=args.aspect_lh,
        reference_absorption=args.reference_absorption,
        prefer_eyring=not args.sabine,
        n_octave_bands=n_bands,
        min_decay_db=getattr(args, "min_decay_db", 0.0),
        noise_floor_margin_db=getattr(args, "noise_floor_margin_db", 0.0),
    )
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "volume": est.volume,
                    "length": est.length,
                    "width": est.width,
                    "height": est.height,
                    "drr_db": est.drr_db,
                    "confidence": est.confidence,
                    "rt60_bands": [float(value) for value in est.rt60_bands],
                    "absorption_bands": [float(value) for value in est.absorption_bands],
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
        raise ValueError("synthesize-rir requires --output")
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
        max_seconds=args.max_seconds,
        prefer_eyring=not args.sabine,
    )
    if result.has_error:
        detail = getattr(result, "error_message", "")
        print(f"Error: {detail or 'invalid room geometry'}", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    # Warnings survive a successful synthesis and describe a RIR the caller did
    # not ask for -- a tail cut against max_seconds, a cap raised to fit the
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
    from . import room_morph

    if not args.output:
        raise ValueError("room-morph requires --output")
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
        max_seconds=args.max_seconds,
        prefer_eyring=not args.sabine,
    )
    # The target RIR is synthesized by the code synthesize-rir uses, so the same
    # clamps fire and each one says the morph went through a room the caller did
    # not ask for. stderr in every mode, so the JSON document on stdout stays
    # exactly the payload both CLIs publish.
    _print_diagnostic_warnings(result)
    _write_wav(args.output, result.audio, sr)
    if args.json:
        print(_strict_json_dumps({"output": args.output, "samples": len(result.audio)}))
    else:
        print(f"  Saved morphed audio ({len(result.audio)} samples) to {args.output}")
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
    acoustic_p.add_argument("--n-bands", type=int, default=6)
    acoustic_p.add_argument("--min-decay-db", type=_finite_float, default=30.0)
    acoustic_p.add_argument("--noise-floor-margin-db", type=_finite_float, default=10.0)

    def _add_room_geometry(p: argparse.ArgumentParser) -> None:
        p.add_argument("--length", type=_finite_float, default=7.0, help="Room length (m)")
        p.add_argument("--width", type=_finite_float, default=5.0, help="Room width (m)")
        p.add_argument("--height", type=_finite_float, default=3.0, help="Room height (m)")
        # The room builder clamps rather than validates, so the refusal comes from
        # the material check beside it -- an invalid parameter, not a usage error.
        _cli_domain(
            p.add_argument(
                "--absorption", type=_finite_float, default=0.2, help="Uniform wall absorption"
            ),
            minimum=0,
            maximum=1,
            reject_exit="invalid_parameter",
        )
        p.add_argument("--source-x", type=_finite_float, default=1.0)
        p.add_argument("--source-y", type=_finite_float, default=1.0)
        p.add_argument("--source-z", type=_finite_float, default=1.2)
        p.add_argument("--listener-x", type=_finite_float, default=5.0)
        p.add_argument("--listener-y", type=_finite_float, default=4.0)
        p.add_argument("--listener-z", type=_finite_float, default=1.7)
        p.add_argument("--ism-order", type=int, default=3, help="Image-source reflection order")
        # The C field is a uint32 and 0 is its library-default sentinel, so the
        # range is closed at both ends rather than refusing only a negative.
        _cli_domain(
            p.add_argument("--seed", type=int, default=1, help="Deterministic late-tail seed"),
            minimum=0,
            maximum=0xFFFFFFFF,
            reject_exit="invalid_parameter",
        )
        p.add_argument(
            "--max-seconds",
            type=_finite_float,
            default=0.0,
            help="Hard cap on RIR/tail length in seconds (0 = natural length)",
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
        "--aspect-lw", type=_finite_float, default=1.0, help="length/width prior"
    )
    estimate_room_p.add_argument(
        "--aspect-lh", type=_finite_float, default=1.0, help="length/height prior"
    )
    estimate_room_p.add_argument(
        "--reference-absorption", type=_finite_float, default=0.15, help="absorption prior"
    )
    estimate_room_p.add_argument(
        "--sabine", action="store_true", help="Use the Sabine model (default Eyring)"
    )
    estimate_room_p.add_argument(
        "--n-octave-bands",
        "--n-bands",
        type=int,
        default=None,
        dest="n_octave_bands",
        metavar="N",
        help="Analyzer octave-band count (0 = library default)",
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
    room_morph_p.add_argument(
        "--wet", type=_finite_float, default=0.5, help="Target-room mix [0,1]"
    )
    room_morph_p.add_argument(
        "--suppression", type=_finite_float, default=0.5, help="Source-tail suppression [0,1]"
    )
