"""`sonare playback`: render movie audio to speakers or binaural headphones."""

from __future__ import annotations

import argparse
import json
import math
from collections.abc import Sequence
from pathlib import Path
from typing import Any

import numpy as np

from ._cli_common import (
    _load_audio_channels,
    _set_nested_value,
    _strict_json_dumps,
    _write_frame_major_wav,
)
from ._cli_inventory import _cli_domain, _cli_scalar_type
from ._cli_options import SharedParsers, _finite_float
from ._playback import HrtfSet, PlaybackLoudnessMeter, PlaybackRenderer
from ._runtime import SonareValueError

# Matches src/playback/renderer.h's kOfflineRenderBlockFrames: the chunk size
# the C++ one-shot reference render loop uses.
_OFFLINE_RENDER_BLOCK_FRAMES = 4096


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


def _interleave_planes(planes: Sequence[Sequence[float]]) -> np.ndarray:
    """Interleave one array per channel into one frame-major float32 array.

    ``np.stack(..., axis=1).ravel()`` lands on the same memory a fresh
    C-contiguous array already holds in interleaved order, so the ``ravel()``
    is a view rather than a second copy. A pure-Python double loop here used
    to cost one Python-object write per sample -- ~2e9 of them for a 2-hour
    5.1 file -- dominating render time on anything past a few seconds.
    """
    if not planes:
        return np.empty(0, dtype=np.float32)
    return np.stack([np.asarray(plane, dtype=np.float32) for plane in planes], axis=1).ravel()


def _load_playback_config_document(path: str) -> dict[str, Any]:
    """Load --config's JSON document.

    A missing file is refused as a caller argument error, matching --hrtf's own
    behavior on this CLI and the native CLI's --config path, rather than the
    shared _load_json_object's generic file-not-found exit code.
    """
    try:
        with open(path, encoding="utf-8") as fh:
            loaded = json.load(fh)
    except OSError as exc:
        raise SonareValueError(f"cannot open --config file: {path}") from exc
    if not isinstance(loaded, dict):
        raise SonareValueError(f"--config JSON root must be an object: {path}")
    return loaded


def _playback_config_document(args: argparse.Namespace) -> dict[str, Any]:
    """Overlay the CLI's flags onto the (optional) --config document.

    An absent flag leaves whatever the document already says (or the schema
    default) alone, so "flags win" only applies to a flag the caller gave.
    """
    document = _load_playback_config_document(args.config) if args.config else {}
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


def _render_via_reporting_handle(
    renderer: PlaybackRenderer, interleaved: np.ndarray, in_channels: int, frames: int
) -> np.ndarray:
    """Render ``frames`` input frames through ``renderer`` block by block.

    Mirrors ``playback::render_interleaved`` (src/playback/renderer.cpp): the
    tail is padded with ``latency_samples()`` silent frames to flush the
    pipeline, and that many frames are dropped from the front of the output,
    so the returned audio and any diagnostics read from ``renderer`` afterward
    both come from the same, actually-processed handle.
    """
    out_channels = renderer.output_channels()
    latency = renderer.latency_samples()
    total = frames + latency
    result = np.zeros((frames, out_channels), dtype=np.float32)
    block = _OFFLINE_RENDER_BLOCK_FRAMES
    for start in range(0, total, block):
        count = min(block, total - start)
        in_block = np.zeros(count * in_channels, dtype=np.float32)
        if start < frames:
            available = min(count, frames - start)
            in_block[: available * in_channels] = interleaved[
                start * in_channels : (start + available) * in_channels
            ]
        out_block = renderer.process_interleaved(in_block, in_channels).reshape(count, out_channels)
        lo = max(latency - start, 0)
        if lo < count:
            dest_start = start + lo - latency
            result[dest_start : dest_start + (count - lo)] = out_block[lo:count]
    return result


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

    in_frames = len(planes[0]) if planes else 0

    renderer: PlaybackRenderer | None = None
    try:
        renderer = PlaybackRenderer(
            config,
            hrtf=hrtf,
            sample_rate=sample_rate,
            max_block_size=_OFFLINE_RENDER_BLOCK_FRAMES,
        )
        out_channels = renderer.output_channels()
        rendered = _render_via_reporting_handle(renderer, interleaved, channels, in_frames)
        # Read after the render so a limiter/loudness clamp or a non-finite
        # discard the render itself triggered shows up in the report.
        diagnostics = renderer.diagnostics() if args.json else None
    finally:
        if renderer is not None:
            renderer.close()
        if hrtf is not None:
            hrtf.close()

    frames, _ = _write_frame_major_wav(args.output, rendered, sample_rate)

    if args.json:
        print(_strict_json_dumps(diagnostics))
    else:
        print(f"  Rendered {frames} frames ({out_channels} ch @ {sample_rate} Hz) to {args.output}")
    return 0
