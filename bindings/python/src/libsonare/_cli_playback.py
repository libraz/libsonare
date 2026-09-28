"""`sonare playback`: render movie audio to speakers or binaural headphones."""

from __future__ import annotations

import argparse
import math
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from ._cli_common import (
    _load_audio_channels,
    _load_json_object,
    _set_nested_value,
    _strict_json_dumps,
    _write_frame_major_wav,
)
from ._cli_inventory import _cli_domain, _cli_scalar_type
from ._cli_options import SharedParsers, _finite_float
from ._playback import HrtfSet, PlaybackLoudnessMeter, PlaybackRenderer, render_playback
from ._runtime import SonareValueError


def _unit_interval(value: str) -> float:
    parsed = _finite_float(value)
    if parsed < 0.0 or parsed > 1.0:
        raise argparse.ArgumentTypeError("must be between 0 and 1")
    return parsed


def _playback_dialogue_db(value: str) -> float:
    parsed = _finite_float(value)
    if parsed < -12.0 or parsed > 12.0:
        raise argparse.ArgumentTypeError("must be between -12 and 12")
    return parsed


def _playback_program_lufs(value: str) -> float:
    parsed = _finite_float(value)
    if parsed < -70.0 or parsed > 0.0:
        raise argparse.ArgumentTypeError("must be between -70 and 0")
    return parsed


def _playback_target_lufs(value: str) -> float:
    parsed = _finite_float(value)
    if parsed < -40.0 or parsed > -5.0:
        raise argparse.ArgumentTypeError("must be between -40 and -5")
    return parsed


_cli_scalar_type(_unit_interval, "number")
_cli_scalar_type(_playback_dialogue_db, "number")
_cli_scalar_type(_playback_program_lufs, "number")
_cli_scalar_type(_playback_target_lufs, "number")


def _hrtf_path(value: str) -> str:
    """Identity `type=`, tagged as a path: `--hrtf` is not in `_PATH_OPTION_NAMES`."""
    return value


_cli_scalar_type(_hrtf_path, "path")


def register_playback_parsers(sub: argparse._SubParsersAction[Any], shared: SharedParsers) -> None:
    """Register `sonare playback`."""
    p = sub.add_parser(
        "playback",
        parents=[shared.common],
        help="Render movie audio to speakers or binaural headphones (upmix/binaural/night mode)",
    )
    p.add_argument(
        "--target",
        choices=["headphones", "stereo", "5.1", "7.1"],
        default=None,
        help="Output target (default: headphones)",
    )
    p.add_argument(
        "--input-layout",
        choices=["auto", "mono", "stereo", "5.1", "7.1"],
        default=None,
        help="Input channel layout (default: auto)",
    )
    p.add_argument(
        "--config", type=str, default=None, help="JSON config document, overlaid by flags"
    )
    p.add_argument("--no-upmix", action="store_true", help="Disable the stereo-to-surround upmix")
    _cli_domain(
        p.add_argument(
            "--night", type=_unit_interval, default=None, help="Night-mode DRC amount (default: 0)"
        ),
        minimum=0.0,
        maximum=1.0,
    )
    _cli_domain(
        p.add_argument(
            "--dialogue-db",
            type=_playback_dialogue_db,
            default=None,
            help="Dialogue level gain in dB (default: 0)",
        ),
        minimum=-12.0,
        maximum=12.0,
    )
    _cli_domain(
        p.add_argument(
            "--program-lufs",
            type=_playback_program_lufs,
            default=None,
            help="Program loudness in LUFS (default: measured from the whole input)",
        ),
        minimum=-70.0,
        maximum=0.0,
    )
    _cli_domain(
        p.add_argument(
            "--target-lufs",
            type=_playback_target_lufs,
            default=None,
            help="Target loudness in LUFS (default: -24)",
        ),
        minimum=-40.0,
        maximum=-5.0,
    )
    p.add_argument(
        "--room",
        choices=["none", "living_room", "home_theater", "screening_room"],
        default=None,
        help="Room preset (ignored by a speakers target)",
    )
    p.add_argument(
        "--hrtf", type=_hrtf_path, default=None, help="SHRF v1 HRTF set file (headphones target)"
    )


def _interleave_planes(planes: Sequence[Sequence[float]]) -> list[float]:
    channels = len(planes)
    frames = len(planes[0]) if planes else 0
    interleaved = [0.0] * (frames * channels)
    for channel, plane in enumerate(planes):
        for frame in range(frames):
            interleaved[frame * channels + channel] = plane[frame]
    return interleaved


def _playback_config_document(args: argparse.Namespace) -> dict[str, Any]:
    """Overlay the CLI's flags onto the (optional) --config document.

    An absent flag leaves whatever the document already says (or the schema
    default) alone, so "flags win" only applies to a flag the caller gave.
    """
    document = _load_json_object(args.config) if args.config else {}
    if args.target is not None:
        if args.target == "headphones":
            _set_nested_value(document, "target.kind", "headphones")
            # headphones forbids target.layout; drop one a --config set.
            target = document.get("target")
            if isinstance(target, dict):
                target.pop("layout", None)
        else:
            _set_nested_value(document, "target.kind", "speakers")
            _set_nested_value(document, "target.layout", args.target)
    if args.input_layout is not None:
        _set_nested_value(document, "input.layout", args.input_layout)
    if args.no_upmix:
        _set_nested_value(document, "upmix.enabled", False)
    if args.night is not None:
        _set_nested_value(document, "night_mode.amount", args.night)
    if args.dialogue_db is not None:
        _set_nested_value(document, "dialogue_level_db", args.dialogue_db)
    if args.target_lufs is not None:
        _set_nested_value(document, "loudness.target_lufs", args.target_lufs)
    if args.room is not None:
        _set_nested_value(document, "room.preset", args.room)
    return document


def _resolved_program_lufs(
    args: argparse.Namespace, interleaved: Sequence[float], channels: int, sample_rate: int
) -> float | None:
    """The caller's --program-lufs, or a fresh measure of the whole input.

    Returns ``None`` (leaving the config's own null/schema default) when there
    is nothing to overlay: an input too short or too quiet for BS.1770 gating
    to report anything measures at the library's generic dB floor, well
    outside the schema's accepted range.
    """
    if args.program_lufs is not None:
        return float(args.program_lufs)
    meter = PlaybackLoudnessMeter(channels, sample_rate)
    try:
        meter.push_interleaved(interleaved)
        measured = meter.integrated_lufs()
    finally:
        meter.close()
    if not math.isfinite(measured) or measured < -70.0 or measured > 0.0:
        return None
    return measured


def cmd_playback(args: argparse.Namespace) -> int:
    if not args.output:
        raise SonareValueError("playback requires --output")

    planes, sample_rate = _load_audio_channels(args.file)
    channels = len(planes)
    if channels not in (1, 2, 6, 8):
        raise SonareValueError(
            f"sonare playback accepts a 1, 2, 6, or 8 channel input file (got {channels} channels)"
        )
    interleaved = _interleave_planes(planes)

    config = _playback_config_document(args)
    program_lufs = _resolved_program_lufs(args, interleaved, channels, sample_rate)
    if program_lufs is not None:
        _set_nested_value(config, "loudness.program_lufs", program_lufs)

    hrtf: HrtfSet | None = None
    if args.hrtf:
        try:
            hrtf_bytes = Path(args.hrtf).read_bytes()
        except OSError as exc:
            raise SonareValueError(f"cannot open --hrtf file: {args.hrtf}") from exc
        hrtf = HrtfSet.from_bytes(hrtf_bytes)

    renderer: PlaybackRenderer | None = None
    try:
        # A second handle just to learn the output channel count and (for
        # --json) the renderer's own diagnostics; the one-shot render below
        # owns no handle of its own to ask.
        renderer = PlaybackRenderer(config, hrtf=hrtf, sample_rate=sample_rate)
        out_channels = renderer.output_channels()
        rendered = render_playback(
            interleaved, channels=channels, sample_rate=sample_rate, config=config, hrtf=hrtf
        )
        diagnostics = renderer.diagnostics() if args.json else None
    finally:
        if renderer is not None:
            renderer.close()
        if hrtf is not None:
            hrtf.close()

    frames, _ = _write_frame_major_wav(
        args.output, rendered.reshape((-1, out_channels)), sample_rate
    )

    if args.json:
        print(_strict_json_dumps(diagnostics))
    else:
        print(f"  Rendered {frames} frames ({out_channels} ch @ {sample_rate} Hz) to {args.output}")
    return 0
