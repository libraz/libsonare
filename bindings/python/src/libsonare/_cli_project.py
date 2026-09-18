"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import sys
import urllib.parse
import urllib.request
from typing import Any, cast

from ._cli_common import (
    EXIT_INVALID_STATE,
    _atomic_write_bytes,
    _legacy_exit_codes,
    _load_audio_channels,
    _read_bounded,
    _strict_json_dumps,
    _write_project_bounce_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._runtime import ErrorCode, SonareError

_MAX_PROJECT_OR_MIDI_BYTES = 64 * 1024 * 1024

# A project source's serialized `kind`: 0 audio, 1 MIDI (SourceKind). Only an
# audio source carries a `uri`, so the kind decides which entries are addressable
# by --audio at all.
_SOURCE_KIND_AUDIO = 0

# The only URI scheme --resolve-audio opens. The core never opens a URI itself,
# so resolving one is this front-end's own file I/O, and it has no business
# fetching over a network to feed a render.
_FILE_URI_SCHEME = "file"

# Subcommands whose result goes to stdout only; accepting -o would silently
# discard the requested destination, so it is rejected instead.
_PROJECT_NO_OUTPUT = frozenset({"abi", "compile", "synth-presets"})


def _load_project(path: str) -> object:
    from . import Project

    data = _read_bounded(path, _MAX_PROJECT_OR_MIDI_BYTES)
    return Project.from_json(data)


def _load_project_with_diagnostics(path: str) -> object:
    from . import Project

    data = _read_bounded(path, _MAX_PROJECT_OR_MIDI_BYTES)
    try:
        return Project.from_json_with_diagnostics(data)
    except ValueError as exc:
        # ``Project.from_json*`` intentionally keeps its public ValueError
        # contract.  The CLI, however, publishes the C-ABI InvalidFormat
        # class for malformed project documents, so translate only at this
        # private command boundary.
        raise SonareError(int(ErrorCode.INVALID_FORMAT), str(exc)) from exc


def _write_project_json(project: object, path: str) -> int:
    data = cast(Any, project).to_json_bytes()
    _atomic_write_bytes(path, data)
    return len(data)


def _parse_audio_binding(assignment: str) -> tuple[int, str]:
    """Split one ``--audio <source_id>=FILE`` assignment.

    A source is addressed by id alone. Its URI is also unique in the document,
    but accepting either spelling would put two addresses on one binding, and
    the id is the one the unresolved-source report already names.

    The split is on the first ``=``, so a path containing one stays intact.
    """
    source_id_text, separator, path = assignment.partition("=")
    if not separator or not path:
        raise ValueError(f"--audio expects <source_id>=FILE, got {assignment!r}")
    try:
        source_id = int(source_id_text.strip(), 10)
    except ValueError:
        raise ValueError(f"--audio source id must be an integer, got {source_id_text!r}") from None
    if source_id <= 0:
        raise ValueError(f"--audio source id must be positive, got {source_id}")
    return source_id, path


def _audio_source_uris(document: bytes) -> dict[int, str]:
    """Map each audio source id in the document to the URI it references.

    Read from the document rather than from the loaded handle: the flat source
    descriptor carries the URI in a fixed-width field, so anything longer arrives
    truncated, and a truncated URI is one ``--resolve-audio`` would then try to
    open. The document is the only place the whole string survives.
    """
    sources = json.loads(document).get("sources", [])
    uris: dict[int, str] = {}
    for source in sources:
        if int(source.get("kind", 0)) != _SOURCE_KIND_AUDIO:
            continue
        uris[int(source.get("id", 0))] = str(source.get("uri", ""))
    return uris


def _path_from_file_uri(uri: str) -> str | None:
    """Resolve a ``file://`` URI to a local path, or None for anything else."""
    parsed = urllib.parse.urlparse(uri)
    if parsed.scheme != _FILE_URI_SCHEME:
        return None
    # An authority naming another host is a remote path this front-end cannot
    # open; only the empty and localhost authorities address the local machine.
    if parsed.netloc not in ("", "localhost"):
        return None
    return urllib.request.url2pathname(parsed.path)


def _bind_source_audio(project: object, source_id: int, path: str) -> None:
    """Decode an audio file and register its PCM against one audio source."""
    planes, sample_rate = _load_audio_channels(path)
    channels = len(planes)
    frames = min((len(plane) for plane in planes), default=0)
    if channels == 0 or frames == 0:
        raise ValueError(f"--audio {source_id}={path} decoded no samples")
    interleaved = [plane[frame] for frame in range(frames) for plane in planes]
    cast(Any, project).set_source_audio(source_id, interleaved, channels, sample_rate)


def _resolve_bounce_audio_sources(project: object, args: argparse.Namespace) -> None:
    """Supply the PCM a bounce needs for the document's audio sources.

    Project JSON references audio by URI / storage handle only and the core
    never opens either, so a document whose clips reference audio has nothing to
    render from until a host binds samples. This front-end is that host:
    ``--audio`` binds one named source to a file and ``--resolve-audio`` opens
    the ``file://`` URIs the document already carries. Anything still unresolved
    is refused naming every source and its URI, rather than reaching the render
    as a bare invalid-state error that says nothing about what is missing.
    """
    assignments = list(getattr(args, "audio", None) or [])
    resolve = bool(getattr(args, "resolve_audio", False))
    # A document with no audio sources at all -- every MIDI-only project -- has
    # nothing to resolve and nothing to report, so it never pays for the second
    # read the URI map costs.
    if not assignments and not resolve and not cast(Any, project).unresolved_audio_source_ids():
        return
    uris = _audio_source_uris(_read_bounded(args.input, _MAX_PROJECT_OR_MIDI_BYTES))
    for assignment in assignments:
        source_id, path = _parse_audio_binding(assignment)
        if source_id not in uris:
            raise ValueError(f"--audio {assignment}: the project has no audio source {source_id}")
        _bind_source_audio(project, source_id, path)
    if resolve:
        for source_id in cast(Any, project).unresolved_audio_source_ids():
            uri = uris.get(source_id, "")
            path = _path_from_file_uri(uri)
            if path is None:
                raise ValueError(
                    f"source {source_id} ({uri}): --resolve-audio opens file:// URIs only; "
                    f"pass --audio {source_id}=FILE"
                )
            _bind_source_audio(project, source_id, path)
    remaining = cast(Any, project).unresolved_audio_source_ids()
    if remaining:
        # One line per source, each carrying its own id, because the caller has
        # to act on every one of them: stopping at the first turns a document
        # with four missing takes into four runs.
        raise SonareError(
            int(ErrorCode.INVALID_STATE),
            "\n".join(
                f"source {source_id} ({uris.get(source_id, '')}) has no audio; "
                f"pass --audio {source_id}=FILE or --resolve-audio"
                for source_id in remaining
            ),
        )


def _project_bounce(
    args: argparse.Namespace,
    *,
    force_synth: bool = False,
    command_name: str = "project bounce",
    binds_source_audio: bool = True,
) -> int:
    if not args.output:
        raise ValueError(f"{command_name} requires --output")
    project = _load_project(args.input)
    try:
        if binds_source_audio:
            _resolve_bounce_audio_sources(project, args)
        project_sample_rate = cast(Any, project).get_sample_rate()
        # Render at the project's own sample rate by default (args.sample_rate is
        # None unless the user passed --sample-rate); an explicit --sample-rate is
        # only accepted when it matches the project's rate, otherwise report the
        # mismatch by name instead of letting the C ABI reject it generically.
        requested_sample_rate = args.sample_rate
        if requested_sample_rate is not None and requested_sample_rate > 0:
            if abs(requested_sample_rate - project_sample_rate) > 1e-6:
                raise ValueError(
                    f"--sample-rate {requested_sample_rate} does not match the project's "
                    f"sample rate ({project_sample_rate:g} Hz); {command_name} renders at "
                    "the project's own rate"
                )
            sample_rate = requested_sample_rate
            bounce_sample_rate = requested_sample_rate
        else:
            # floor(x + 0.5) rather than round(): Python rounds a .5 tie to even
            # and the native CLI's std::lround rounds it away from zero, so a
            # project at exactly 44100.5 Hz tagged its WAV 44100 here and 44101
            # there. Sample rates are positive, so the two agree everywhere else.
            sample_rate = int(math.floor(project_sample_rate + 0.5))
            # 0 is the C ABI's "render at the project's own rate" sentinel, and
            # it is the only way the full-precision rate reaches the render.
            # Pinning the rounded value here made a project whose rate is not an
            # integer fail the ABI's own equality check against that rate.
            bounce_sample_rate = 0
        kwargs = {
            "total_frames": args.frames,
            "block_size": args.block_size,
            "num_channels": args.channels,
            "sample_rate": bounce_sample_rate,
            "instrument_latency_samples": args.instrument_latency,
        }
        use_synth = force_synth or args.synth is not None
        if use_synth:
            audio = cast(Any, project).bounce_with_synth_instrument(
                args.synth or None, auto_select_gm=not bool(args.synth), **kwargs
            )
        else:
            audio = cast(Any, project).bounce(**kwargs)
        frames, channels = _write_project_bounce_wav(args.output, audio, sample_rate)
        if args.json:
            print(
                _strict_json_dumps(
                    {
                        "output": args.output,
                        "frames": frames,
                        "channels": channels,
                        "sample_rate": sample_rate,
                        "synth": bool(use_synth),
                    }
                )
            )
        else:
            print(f"  Bounced {frames} frames ({channels} ch @ {sample_rate} Hz)")
        return 0
    finally:
        cast(Any, project).close()


def cmd_project(args: argparse.Namespace) -> int:
    from . import Project, project_abi_version, synth_preset_names

    subcommand = args.project_command
    if subcommand in _PROJECT_NO_OUTPUT and getattr(args, "output", None):
        raise ValueError(f"project {subcommand} does not write an output file; remove --output")
    if subcommand == "abi":
        version = project_abi_version()
        print(_strict_json_dumps({"abi_version": version}) if args.json else version)
        return 0
    if subcommand == "new":
        if not args.output:
            raise ValueError("project new requires --output")
        project = Project()
        try:
            if args.sample_rate > 0:
                project.set_sample_rate(args.sample_rate)
            bytes_written = _write_project_json(project, args.output)
        finally:
            project.close()
        if args.json:
            print(_strict_json_dumps({"output": args.output, "bytes": bytes_written}))
        else:
            print(f"  Wrote empty project: {args.output}")
        return 0
    if subcommand == "validate":
        loaded = cast(Any, _load_project_with_diagnostics(args.input))
        project = loaded.project
        # The native loader joins repair diagnostics (dangling clip sources,
        # source-kind mismatches, invalid warp maps, ...) newline-separated.
        diagnostics = [line for line in loaded.diagnostics.split("\n") if line]
        try:
            if args.output:
                bytes_written = _write_project_json(project, args.output)
            else:
                bytes_written = len(cast(Any, project).to_json_bytes())
        finally:
            cast(Any, project).close()
        strict = getattr(args, "strict", False)
        # A successful parse is valid even when the loader repaired dangling
        # references and emitted diagnostics.  Strict mode changes the exit
        # status only; it must preserve the successful payload and canonical
        # artifact so callers can inspect or persist the repair.
        valid = True
        if args.json:
            print(
                _strict_json_dumps(
                    {
                        "valid": valid,
                        "bytes": bytes_written,
                        "diagnostic_count": len(diagnostics),
                        "diagnostics": diagnostics,
                    }
                )
            )
        elif strict and diagnostics:
            # A diagnostic that changes the exit status is part of the result and
            # goes to stdout; one that does not is advisory and goes to stderr.
            # Strict mode returns a failing status, so the diagnostics are the
            # result and replace the success line: printing "valid" while exiting
            # non-zero left no way to see what was wrong without --json.
            print(f"  Project JSON loaded with {len(diagnostics)} diagnostic(s):")
            for entry in diagnostics:
                print(f"  {entry}")
        else:
            print(f"  Project JSON is valid ({bytes_written} bytes canonical)")
            # Advisory: these did not change the exit status, so stdout carries
            # the result alone and they go to stderr. Dropping them would hide a
            # repaired dangling reference from anyone not also passing --json.
            for entry in diagnostics:
                print(f"  warning: {entry}", file=sys.stderr)
        if strict and diagnostics:
            return 1 if _legacy_exit_codes() else EXIT_INVALID_STATE
        return 0
    if subcommand == "compile":
        project = cast(Project, _load_project(args.input))
        try:
            result = cast(Any, project).compile()
            if args.json:
                print(
                    _strict_json_dumps(
                        {
                            "has_timeline": result.has_timeline,
                            "diagnostic_count": result.diagnostic_count,
                            "diagnostics": [
                                {
                                    "code": diagnostic.code,
                                    "severity": diagnostic.severity,
                                    "target_id": diagnostic.target_id,
                                    "message": diagnostic.message,
                                }
                                for diagnostic in result.diagnostics
                            ],
                            "messages": result.messages,
                        }
                    )
                )
            else:
                print(
                    "  Compiled"
                    if result.has_timeline
                    else f"  Compiled with errors ({result.diagnostic_count} diagnostics)"
                )
            if result.has_timeline:
                return 0
            return 1 if _legacy_exit_codes() else EXIT_INVALID_STATE
        finally:
            cast(Any, project).close()
    if subcommand == "bounce":
        return _project_bounce(args)
    if subcommand == "export-smf":
        if not args.output:
            raise ValueError("project export-smf requires --output")
        project = cast(Project, _load_project(args.input))
        try:
            data = cast(Any, project).export_smf()
        finally:
            cast(Any, project).close()
        _atomic_write_bytes(args.output, data)
        print(
            _strict_json_dumps({"output": args.output, "bytes": len(data)})
            if args.json
            else args.output
        )
        return 0
    if subcommand == "import-smf":
        if not args.output:
            raise ValueError("project import-smf requires --output")
        data = _read_bounded(args.smf, _MAX_PROJECT_OR_MIDI_BYTES)
        project = Project()
        try:
            first_clip = project.import_smf(data)
            bytes_written = _write_project_json(project, args.output)
        finally:
            project.close()
        if args.json:
            print(
                _strict_json_dumps(
                    {"output": args.output, "first_clip_id": first_clip, "bytes": bytes_written}
                )
            )
        else:
            print(f"  Imported SMF: {args.output}")
        return 0
    if subcommand == "export-midi2":
        if not args.output:
            raise ValueError("project export-midi2 requires --output")
        project = cast(Project, _load_project(args.input))
        try:
            data = cast(Any, project).export_clip_file()
        finally:
            cast(Any, project).close()
        _atomic_write_bytes(args.output, data)
        print(
            _strict_json_dumps({"output": args.output, "bytes": len(data)})
            if args.json
            else args.output
        )
        return 0
    if subcommand == "import-midi2":
        if not args.output:
            raise ValueError("project import-midi2 requires --output")
        data = _read_bounded(args.midi2, _MAX_PROJECT_OR_MIDI_BYTES)
        project = Project()
        try:
            first_clip = project.import_clip_file(data)
            bytes_written = _write_project_json(project, args.output)
        finally:
            project.close()
        if args.json:
            print(
                _strict_json_dumps(
                    {"output": args.output, "first_clip_id": first_clip, "bytes": bytes_written}
                )
            )
        else:
            print(f"  Imported MIDI2 Clip File: {args.output}")
        return 0
    if subcommand == "synth-presets":
        names = synth_preset_names()
        print(_strict_json_dumps({"presets": names}) if args.json else "\n".join(names))
        return 0
    raise ValueError(f"unknown project subcommand: {subcommand}")


def cmd_midi_render(args: argparse.Namespace) -> int:
    # No audio-source binding here: `midi-render` declares neither --audio nor
    # --resolve-audio, so it has no way to supply the PCM an unresolved source
    # needs and naming those options in its refusal would advertise a flag the
    # command does not accept.
    return _project_bounce(
        args, force_synth=True, command_name="midi-render", binds_source_audio=False
    )


def cmd_transcribe(args: argparse.Namespace) -> int:
    from . import Project

    if not args.output:
        raise ValueError("transcribe requires --output")
    if args.tempo_bpm is not None and args.tempo_bpm <= 0.0:
        raise ValueError("--tempo-bpm must be greater than 0")

    samples, sr = _load_audio(args.file)
    project = Project()
    try:
        # The clip's grid is the PROJECT's tempo map, which is why an explicit
        # tempo is installed rather than passed: `Project.transcribe_to_clip`
        # takes none. Leaving it out detects one, as `transcribe` does.
        if args.tempo_bpm is None:
            tempo_bpm = float(cast(Any, project).auto_tempo(samples, sr))
        else:
            tempo_bpm = float(args.tempo_bpm)
            cast(Any, project).set_tempo_segments([{"start_ppq": 0.0, "bpm": tempo_bpm}])
        # PPQ coordinates are beats, so the take's length in beats is what the
        # clip has to span for its last note-off to fall inside it.
        duration = len(samples) / sr if sr > 0 else 0.0
        length_ppq = max(1.0, float(math.ceil(duration * tempo_bpm / 60.0)))
        _track_id, clip_id = cast(Any, project).add_midi_clip(0.0, length_ppq)
        note_count = cast(Any, project).transcribe_to_clip(
            clip_id,
            samples,
            sr,
            polyphonic=args.polyphonic,
            reference_hz=args.reference_hz,
            fmin=args.fmin,
            fmax=args.fmax,
            min_note_ms=args.min_note_ms,
            segmentation_threshold_cents=args.segmentation_threshold_cents,
            velocity_floor_db=args.velocity_floor_db,
            fixed_velocity=args.fixed_velocity,
            group=args.group,
            channel=args.channel,
        )
        data = cast(Any, project).export_smf()
    finally:
        cast(Any, project).close()
    _atomic_write_bytes(args.output, data)
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "output": args.output,
                    "note_count": note_count,
                    "tempo_bpm": tempo_bpm,
                    "bytes": len(data),
                }
            )
        )
    else:
        print(f"  Transcribed {note_count} notes at {tempo_bpm:.2f} BPM")
        print(f"    Wrote: {args.output}")
    return 0
