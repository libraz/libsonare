"""Mixer scene and channel-strip commands for the Python command-line interface.

The scene commands hand a whole multi-track mix to the assistant and print the
scene it proposes; ``mix`` and ``mix-strip`` render one. They are a family of
their own rather than a corner of mastering because a scene describes several
tracks' relationship, where a mastering chain describes one finished stereo
program.
"""

from __future__ import annotations

import argparse
import json
import os
from typing import Any

from ._cli_common import (
    _atomic_wav_writer,
    _json_key_to_snake_case,
    _load_channels_or_downmix,
    _parse_kv_params,
    _resample,
    _strict_json_dumps,
    _write_channel_output,
    _write_wav_stereo_frames,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_options import SharedParsers, _ContractArgumentParser


def _mix_assistant_options(params: dict[str, float]) -> dict[str, Any]:
    """Map ``suggest-mix --params`` keys onto ``suggest_mix_scene`` keywords.

    The accepted set is the assistant's own option table, so either spelling the
    C ABI reads is accepted and nothing here restates the list. Two conversions
    are this boundary's: an option whose config field is a bool is converted
    rather than handed a number, and an integral value for an integer option is
    narrowed, since ``--params`` parses every value as a float.
    """
    from .mixing_assistant import _INTEGER_PARAMS, _PARAM_KEYS

    options: dict[str, Any] = {}
    for key, value in params.items():
        name = _json_key_to_snake_case(key)
        if name not in _PARAM_KEYS:
            raise ValueError(f"unknown suggest-mix param: {key}")
        if name.startswith("enable_"):
            options[name] = value != 0.0
        elif name in _INTEGER_PARAMS and value.is_integer():
            options[name] = int(value)
        else:
            options[name] = value
    return options


def _mix_assistant_tracks(entries: list[str], sample_rate: int) -> list[Any]:
    """Load each ``[ID=]WAV`` entry as one track at ``sample_rate``.

    A two-channel file is handed over as a stereo track rather than a downmix,
    because the assistant's image domain is the one thing that reads a track's
    two channels: it measures interchannel cancellation, width and mono risk, and
    on a folded copy of a stereo stem every one of those measurements describes a
    signal the caller never has. A mono file stays mono rather than being
    duplicated across both sides, which would present it to the same domain as a
    stereo track of zero width.
    """
    from .mixing_assistant import MixTrackInput

    tracks: list[Any] = []
    for entry in entries:
        track_id, separator, path = entry.partition("=")
        if not separator:
            # Bare path: the file's own name is the id the scene addresses it by.
            path = entry
            track_id = os.path.splitext(os.path.basename(path))[0]
        if not track_id:
            raise ValueError(f"--input track id must not be empty: {entry}")
        if not path:
            raise ValueError(f"--input requires a file path: {entry}")
        planes, track_rate = _load_channels_or_downmix(path)
        left = planes[0]
        right = planes[1] if len(planes) == 2 else None
        if track_rate != sample_rate:
            left = _resample(left, track_rate, sample_rate)
            if right is not None:
                right = _resample(right, track_rate, sample_rate)
        tracks.append(MixTrackInput(track_id=track_id, left=left, right=right, name=track_id))
    return tracks


def _resolve_tempo_bpm(raw: str, entries: list[str]) -> float:
    """Read ``--tempo-bpm``, detecting it from the first track when asked to.

    ``auto`` measures the first ``--input`` rather than the whole set: a tempo is
    a property of the song, so every track shares one, and detecting it once on
    the track the caller listed first keeps which file was measured visible in
    the command instead of hidden in an averaging rule.
    """
    from . import detect_bpm

    if raw.strip().lower() != "auto":
        return float(raw)
    _, _, path = entries[0].partition("=")
    samples, sample_rate = _load_audio(path or entries[0])
    return float(detect_bpm(samples, sample_rate=sample_rate))


def cmd_suggest_mix(args: argparse.Namespace) -> int:
    from . import suggest_mix_scene

    if not args.input:
        raise ValueError("suggest-mix requires at least one --input")
    options = _mix_assistant_options(_parse_kv_params(args.params) if args.params else {})
    if args.tempo_bpm:
        # The dedicated option and `--params tempoBpm=` reach the same field, so
        # naming both is a contradiction rather than a precedence question.
        if "tempo_bpm" in options:
            raise ValueError("--tempo-bpm and --params tempoBpm= set the same value")
        options["tempo_bpm"] = _resolve_tempo_bpm(args.tempo_bpm, args.input)
    tracks = _mix_assistant_tracks(args.input, args.sample_rate)
    document = suggest_mix_scene(tracks, sample_rate=args.sample_rate, **options)
    if args.scene_out:
        # Written from the document already in hand rather than through
        # suggest_mix_scene_json, which would re-run an STFT per track and every
        # pairwise pass to reach the same scene. That the two agree is pinned by
        # a test rather than assumed here.
        with open(args.scene_out, "w", encoding="utf-8") as fh:
            fh.write(_strict_json_dumps(document.get("scene", {})) + "\n")
    print(_strict_json_dumps(document))
    return 0


def cmd_mixing_presets(args: argparse.Namespace) -> int:
    from . import mixing_scene_preset_names

    names = mixing_scene_preset_names()
    print(_strict_json_dumps({"presets": names}) if args.json else "\n".join(names))
    return 0


def cmd_mixing_preset(args: argparse.Namespace) -> int:
    from . import mixing_scene_preset_json

    # The declared argparse default is the only default: a second fallback here
    # would be a value the published contract does not name.
    print(mixing_scene_preset_json(args.preset))
    return 0


def _load_strip_pair(path: str, sample_rate: int) -> tuple[list[float], list[float]]:
    """Load one file as the stereo pair a mixer strip processes.

    A strip is stereo, so a mono file is carried on both sides and a stereo one
    keeps its own two channels instead of being folded and duplicated, which
    threw away the image of every stereo stem fed to the mixer. A source with
    more channels than a strip has goes through the downmixing loader, which
    says so.
    """
    planes, in_sr = _load_channels_or_downmix(path)
    left = list(planes[0])
    right = list(planes[1]) if len(planes) == 2 else list(left)
    if in_sr != sample_rate:
        left = list(_resample(left, in_sr, sample_rate))
        right = list(_resample(right, in_sr, sample_rate))
    return left, right


def _mix_strip_channels(
    entries: list[str], strip_ids: list[str], sample_rate: int
) -> tuple[list[list[float]], list[list[float]], int]:
    """Resolve ``--input`` entries onto the scene's strips, in strip order.

    Two spellings normalize here so the rest of the command sees one shape.
    Addressed by id — either an explicit ``ID=WAV`` or a bare path whose base
    name names a strip — a scene may carry strips no input feeds, which is what
    an assistant-suggested scene always looks like: its effect returns are fed
    by sends rather than by a file, and requiring a silent WAV for each of them
    made the suggestion unrenderable without one. Addressed positionally, the
    historical form, every strip takes the input at its own index; it is kept
    because a built-in preset's strip ids are fixed vocabulary that a file on
    disk has no reason to match.

    A strip no entry names is fed silence rather than dropped: it may still
    carry an insert whose tail belongs in the mix.

    Returns the per-strip left and right buffers and their shared length. Inputs
    shorter than the longest are padded rather than the set being truncated to
    the shortest, which would delete a part that only enters late in the song.
    """
    resolved: dict[int, tuple[list[float], list[float]]] = {}
    positional: list[tuple[list[float], list[float]]] = []
    addressed = False
    index_of = {strip_id: index for index, strip_id in enumerate(strip_ids)}

    for entry in entries:
        track_id, separator, path = entry.partition("=")
        if not separator:
            path = entry
            track_id = os.path.splitext(os.path.basename(path))[0]
        elif not track_id:
            raise ValueError(f"--input strip id must not be empty: {entry}")
        if not path:
            raise ValueError(f"--input requires a file path: {entry}")

        pair = _load_strip_pair(path, sample_rate)

        target = index_of.get(track_id)
        if target is None:
            if separator:
                # An explicit id is an assertion about the scene, so a miss is
                # the caller's mistake rather than a reason to fall back.
                raise ValueError(
                    f"--input names strip {track_id!r}, which the scene does not have "
                    f"(strips: {', '.join(strip_ids)})"
                )
            positional.append(pair)
            continue
        if target in resolved:
            raise ValueError(f"--input names strip {track_id!r} more than once")
        addressed = True
        resolved[target] = pair

    if addressed and positional:
        raise ValueError(
            "--input entries must either all name a strip or all be positional; "
            f"{', '.join(sorted(strip_ids))} are the scene's strips"
        )
    if positional and len(positional) != len(strip_ids):
        raise ValueError(
            f"scene has {len(strip_ids)} strips but {len(positional)} inputs were given; "
            "name them as --input ID=WAV to feed only some of them"
        )
    for index, pair in enumerate(positional):
        resolved[index] = pair

    length = max((len(pair[0]) for pair in resolved.values()), default=0)

    def _padded(side: int) -> list[list[float]]:
        out: list[list[float]] = []
        for index in range(len(strip_ids)):
            buffer = resolved[index][side] if index in resolved else []
            out.append(buffer + [0.0] * (length - len(buffer)))
        return out

    return _padded(0), _padded(1), length


def cmd_mix(args: argparse.Namespace) -> int:
    from . import Mixer, mixing_scene_preset_json

    if args.input and not args.output:
        raise ValueError("mix with --input requires --output")
    if args.output and not args.input:
        raise ValueError("mix with --output requires at least one --input")

    # Resolve the scene JSON from either a file or a built-in preset.
    if args.scene:
        with open(args.scene, encoding="utf-8") as fh:
            scene_json = fh.read()
    elif args.preset:
        scene_json = mixing_scene_preset_json(args.preset)
    else:
        raise ValueError("either --scene or --preset is required")

    mixer = Mixer.from_scene_json(
        scene_json, sample_rate=args.sample_rate, block_size=args.block_size
    )
    try:
        strip_count = mixer.strip_count()

        rendered_samples = 0
        if args.input:
            # Each input WAV feeds one strip (mono inputs are duplicated to both
            # channels). Inputs that were captured at a different sample rate are
            # resampled to the mixer rate so a 44.1 kHz stem is not played back
            # fast at the 48 kHz default.
            strip_ids = [
                str(strip.get("id", "")) for strip in json.loads(scene_json).get("strips", [])
            ]
            # The ids are read from the scene text while the buffers are handed
            # to the compiled mixer, so a disagreement between the two would
            # misalign every strip silently rather than fail.
            if len(strip_ids) != strip_count:
                raise ValueError(
                    f"scene text declares {len(strip_ids)} strips but the mixer compiled "
                    f"{strip_count}"
                )
            left_channels, right_channels, length = _mix_strip_channels(
                args.input, strip_ids, args.sample_rate
            )
            mixer.compile()
            # The mixer reports its graph latency separately. Output begins at
            # sample zero without trimming so routing alignment is preserved.
            with _atomic_wav_writer(args.output, 2, args.sample_rate) as wav:
                for offset in range(0, length or 0, args.block_size):
                    end = min(offset + args.block_size, length or 0)
                    result = mixer.process_stereo(
                        [channel[offset:end] for channel in left_channels],
                        [channel[offset:end] for channel in right_channels],
                    )
                    _write_wav_stereo_frames(wav, result.left, result.right)
                    rendered_samples += len(result.left)

                tail_remaining = mixer.tail_samples()
                while tail_remaining > 0:
                    count = min(tail_remaining, args.block_size)
                    result = mixer.drain_tail_stereo(count)
                    _write_wav_stereo_frames(wav, result.left, result.right)
                    rendered_samples += len(result.left)
                    tail_remaining -= count

        if args.json:
            payload: dict[str, object] = {
                "strip_count": strip_count,
                "sample_rate": args.sample_rate,
                "block_size": args.block_size,
            }
            if args.input:
                payload["rendered_samples"] = rendered_samples
                payload["output"] = args.output
            print(_strict_json_dumps(payload))
        else:
            print("  Mixer:")
            print(f"    Strips:      {strip_count}")
            print(f"    Sample rate: {args.sample_rate} Hz")
            print(f"    Block size:  {args.block_size}")
            if args.input:
                print(f"    Rendered:    {rendered_samples} samples (stereo)")
                print(f"    Wrote: {args.output}")
    finally:
        mixer.close()
    return 0


# The spellings the native front-end accepts, lowercased before the lookup, with
# the scene JSON's integer encoding of mixing::PanMode. Mixer.set_pan takes its
# own vocabulary, which is not the same set, so this command resolves its option
# here rather than through the facade.
_MIX_STRIP_PAN_MODES = {
    "balance": 0,
    "stereopan": 1,
    "stereo-pan": 1,
    "pan": 1,
    "dualpan": 2,
    "dual-pan": 2,
}


def _mix_strip_pan_mode(value: str) -> int:
    """Resolve one ``--pan-mode`` spelling to its scene JSON encoding."""
    mode = _MIX_STRIP_PAN_MODES.get(value.lower())
    if mode is None:
        raise ValueError(f"invalid pan mode: {value}")
    return mode


def _mix_strip_scene_json(
    input_trim_db: float, fader_db: float, pan: float, pan_mode: int, width: float
) -> str:
    """Build the one-strip-to-master scene this command renders through.

    The native front-end applies a bare channel strip to the buffer, which this
    surface has no facade for; a single strip routed straight to master with no
    inserts and no sends is the same signal path. Every field the strip carries
    is spelled out so the scene defaults cannot drift away from the bare strip's.
    """
    return json.dumps(
        {
            "version": 1,
            "buses": [{"id": "master", "role": "master", "inserts": []}],
            "connections": [{"source": "s", "destination": "master"}],
            "strips": [
                {
                    "id": "s",
                    "inputTrimDb": input_trim_db,
                    "faderDb": fader_db,
                    "pan": pan,
                    "panMode": pan_mode,
                    "panLaw": 0,
                    "width": width,
                    "channelDelaySamples": 0,
                    "dualPanLeft": -1,
                    "dualPanRight": 1,
                    "inserts": [],
                    "muted": False,
                    "polarityInvertLeft": False,
                    "polarityInvertRight": False,
                    "sends": [],
                    "soloSafe": False,
                    "soloed": False,
                    "vcaOffsetDb": 0,
                }
            ],
            "vcaGroups": [],
        }
    )


def cmd_mix_strip(args: argparse.Namespace) -> int:
    """Run one channel strip over a file and write the stereo result."""
    from . import Mixer

    width = float(getattr(args, "width", 1.0))
    planes, sample_rate = _load_channels_or_downmix(args.file)
    left = list(planes[0])
    right = list(planes[1]) if len(planes) == 2 else list(left)
    if len(planes) != 2 and width != 1.0:
        raise ValueError("--width requires a stereo input")

    scene_json = _mix_strip_scene_json(
        float(getattr(args, "input_trim_db", 0.0)),
        float(getattr(args, "fader_db", 0.0)),
        float(getattr(args, "pan", 0.0)),
        _mix_strip_pan_mode(getattr(args, "pan_mode", "balance")),
        width,
    )

    # One block covering the whole file, as the native front-end does: the strip
    # is prepared for that block size and processed once.
    frames = len(left)
    mixer = Mixer.from_scene_json(scene_json, sample_rate=sample_rate, block_size=max(frames, 1))
    try:
        mixer.compile()
        # The native sets every value before prepare(), which snaps the
        # smoothers. A scene applies them after, so without this the first
        # block glides up to them instead of opening at them.
        mixer.settle(0)
        result = mixer.process_stereo([left], [right])
        meter = mixer.strip_meter(0)
    finally:
        mixer.close()

    if args.output:
        _write_channel_output(args.output, [result.left, result.right], sample_rate)

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "sample_rate": sample_rate,
                    "length": frames,
                    "meter": {
                        "peak_db_l": meter.peak_db_l,
                        "peak_db_r": meter.peak_db_r,
                        "rms_db_l": meter.rms_db_l,
                        "rms_db_r": meter.rms_db_r,
                        "correlation": meter.correlation,
                        "mono_compat_width": meter.mono_compat_width,
                        "likely_mono_compatible": meter.likely_mono_compatible,
                        "max_true_peak_db": meter.max_true_peak_db,
                    },
                }
            )
        )
    else:
        print("  Channel strip:")
        print(f"    Samples:     {frames}")
        print(f"    Sample rate: {sample_rate} Hz")
        print(f"    Correlation: {meter.correlation:.6f}")
        print(f"    Mono-compatible: {'yes' if meter.likely_mono_compatible else 'no'}")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def register_mixing_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the mixer scene and channel-strip commands."""
    common = shared.common
    stdout_options = shared.stdout_options

    # Mixing commands
    sub.add_parser(
        "mixing-presets", parents=[stdout_options], help="List built-in mixer scene presets"
    )
    mixing_preset_p = sub.add_parser(
        "mixing-preset", parents=[stdout_options], help="Print a built-in mixer scene preset"
    )
    mixing_preset_p.add_argument(
        "--preset", default="vocalReverbSend", help="Built-in scene preset name"
    )

    suggest_mix_p = sub.add_parser(
        "suggest-mix",
        parents=[stdout_options],
        help="Suggest a mixer scene from several tracks as JSON",
    )
    # One assignment per occurrence, as --set and --edit do: an id written with
    # the path keeps the pairing in one token, so no second repeatable option has
    # to be kept in step with this one.
    suggest_mix_p.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="[ID=]WAV",
        help=(
            "Per-track input WAV (repeat once per track); a stereo file keeps both "
            "channels and more than two are downmixed, then resampled to "
            "--sample-rate; ID defaults to the file's base name"
        ),
    )
    suggest_mix_p.add_argument(
        "--sample-rate",
        type=int,
        default=48000,
        help="Shared analysis sample rate (default: 48000)",
    )
    suggest_mix_p.add_argument(
        "--params", default="", help="Assistant params as k=v,k=v (default: the library's own)"
    )
    suggest_mix_p.add_argument(
        "--tempo-bpm",
        default="",
        metavar="BPM|auto",
        help=(
            "Tempo the suggested delay times are voiced against; 'auto' detects it "
            "from the first --input (omitted: the transport's fallback tempo)"
        ),
    )
    suggest_mix_p.add_argument(
        "--scene-out",
        default="",
        metavar="FILE",
        help="Also write just the suggested scene, in the form 'mix --scene' reads",
    )

    mix_p = sub.add_parser(
        "mix",
        parents=[common],
        help="Load a mixer scene (JSON file or preset) and optionally render inputs",
        description=(
            "A stereo input keeps its own two channels and a mono one is carried "
            "on both; an input with more channels than a strip has is downmixed, "
            "with a warning. Inputs at a different sample rate are resampled to "
            "--sample-rate before mixing."
        ),
    )
    mix_group = mix_p.add_mutually_exclusive_group(required=True)
    mix_group.add_argument("--scene", default="", help="Path to a scene JSON file")
    mix_group.add_argument("--preset", default="", help="Built-in scene preset name")
    mix_p.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="[ID=]WAV",
        help=(
            "Input WAV for one strip (repeat once per fed strip); resampled to "
            "--sample-rate; requires --output to render. ID names the strip and "
            "defaults to the file's base name, and a strip no entry names is fed "
            "silence. Entries that name no strip at all are taken positionally "
            "instead, one per strip in scene order"
        ),
    )
    mix_p.add_argument(
        "--sample-rate", type=int, default=48000, help="Mixer sample rate (default: 48000)"
    )
    mix_p.add_argument(
        "--block-size", type=int, default=512, help="Mixer max block size (default: 512)"
    )

    mix_strip_p = sub.add_parser(
        "mix-strip",
        parents=[common],
        help="Run one channel strip over a file and write the stereo result",
        description=(
            "A stereo input keeps its own two channels and a mono one is carried "
            "on both. The output is always stereo, and --width requires a stereo "
            "input because a duplicated mono pair carries no side signal to widen."
        ),
    )
    mix_strip_p.add_argument(
        "--input-trim-db", type=float, default=0.0, help="Input trim in dB (default: 0)"
    )
    mix_strip_p.add_argument("--fader-db", type=float, default=0.0, help="Fader in dB (default: 0)")
    mix_strip_p.add_argument(
        "--pan", type=float, default=0.0, help="Pan position, -1 to 1 (default: 0)"
    )
    mix_strip_p.add_argument(
        "--pan-mode",
        default="balance",
        help="Pan mode: balance, stereo-pan or dual-pan (default: balance)",
    )
    mix_strip_p.add_argument("--width", type=float, default=1.0, help="Stereo width (default: 1.0)")
