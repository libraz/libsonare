"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import sys
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass
from typing import Any, cast

from ._cli_common import (
    _MAX_PROJECT_OR_MIDI_BYTES,
    EXIT_INVALID_STATE,
    _absent_if_none,
    _atomic_write_bytes,
    _catalog_token,
    _load_audio_channels,
    _note_output_fit,
    _OutputFit,
    _read_bounded,
    _strict_json_dumps,
    _write_project_bounce_wav,
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
    _NumericTextError,
    _positive_int,
    _read_integer_text,
    _tempo_bpm,
)
from ._cli_utilities import _MAX_GENERATOR_SAMPLE_RATE, _MIN_GENERATOR_SAMPLE_RATE
from ._narrowing import _INT64_MAX, _INT64_MIN
from ._project_instruments import SynthPatch
from ._runtime import _UINT32_MAX, ErrorCode, SonareError

# The only URI scheme --resolve-audio opens. The core never opens a URI itself,
# so resolving one is this front-end's own file I/O, and it has no business
# fetching over a network to feed a render.
_FILE_URI_SCHEME = "file"

# The mode an aligned clip plays its warp map through. Anchors encode a rate
# difference, and a clip left in any other mode ignores it.
_ALIGNED_WARP_MODE = "timeStretch"


def _load_project(path: str) -> object:
    return cast(Any, _load_project_with_diagnostics(path)).project


def _project_from_document(document: bytes) -> object:
    """Load already-read project bytes, publishing the CLI's malformed-document class."""
    from . import Project

    try:
        return Project.from_json_with_diagnostics(document)
    except SonareError:
        # Carries the C ABI's own code, so the exit status matches the native CLI.
        raise
    except ValueError as exc:
        # A refusal before the C ABI (e.g. the document is not UTF-8) is a
        # malformed document too.
        raise SonareError(int(ErrorCode.INVALID_FORMAT), str(exc)) from exc


def _load_project_with_diagnostics(path: str) -> object:
    return _project_from_document(_read_bounded(path, _MAX_PROJECT_OR_MIDI_BYTES))


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
    if not separator or not path or not source_id_text:
        raise ValueError("argument --audio: must be <source_id>=FILE")
    try:
        source_id = _read_integer_text(source_id_text, _INT64_MIN, _INT64_MAX)
    except _NumericTextError as exc:
        raise ValueError(f"argument --audio: <source_id> {exc}") from None
    if not 1 <= source_id <= _UINT32_MAX:
        raise ValueError("argument --audio: <source_id> must be an integer in [1, 4294967295]")
    return source_id, path


def _audio_source_uris(project: Any, document: bytes) -> dict[int, str]:
    """Map each audio source id of the loaded project to the URI it references.

    Which sources are audio comes from the loaded project, so the library's
    document reader is the only one that interprets a source kind. The URI is
    read from the document: the flat source descriptor carries it in a
    fixed-width field, so anything longer arrives truncated, and a truncated URI
    is one ``--resolve-audio`` would then try to open.
    """
    uris = {source.id: "" for source in project.sources() if source.kind == "audio"}
    for source in json.loads(document).get("sources", []):
        source_id, uri = source.get("id"), source.get("uri")
        if isinstance(source_id, int) and source_id in uris and isinstance(uri, str):
            uris[source_id] = uri
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
    if (
        not assignments
        and not resolve
        and not cast(Any, project).unresolved_audio_source_ids().size
    ):
        return
    uris = _audio_source_uris(project, _read_bounded(args.input, _MAX_PROJECT_OR_MIDI_BYTES))
    for assignment in assignments:
        source_id, path = _parse_audio_binding(assignment)
        if source_id not in uris:
            raise ValueError(f"--audio {assignment}: the project has no audio source {source_id}")
        _bind_source_audio(project, source_id, path)
    if resolve:
        for source_id in cast(Any, project).unresolved_audio_source_ids():
            uri = uris.get(source_id, "")
            resolved = _path_from_file_uri(uri)
            if resolved is None:
                raise ValueError(
                    f"source {source_id} ({uri}): --resolve-audio opens file:// URIs only; "
                    f"pass --audio {source_id}=FILE"
                )
            _bind_source_audio(project, source_id, resolved)
    remaining = cast(Any, project).unresolved_audio_source_ids()
    if remaining.size:
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


# The value the native parser stores for a bare `project bounce --synth`; it spells GM routing too.
_BARE_SYNTH_SPELLING = "true"


def project_audio_refusal(args: argparse.Namespace) -> str | None:
    """The first malformed ``--audio <source_id>=FILE`` assignment, or None; argv alone decides."""
    for assignment in list(getattr(args, "audio", None) or []):
        try:
            _parse_audio_binding(assignment)
        except ValueError as exc:
            return str(exc)
    return None


def project_synth_refusal(args: argparse.Namespace, *, bare_flag: bool) -> str | None:
    """The refusal of a ``--synth`` naming no NativeSynth preset, or None."""
    value = getattr(args, "synth", None)
    if value is None or (bare_flag and value in ("", _BARE_SYNTH_SPELLING)):
        return None
    from . import synth_preset_names

    names = synth_preset_names()
    if _catalog_token(value, names) in names:
        return None
    return f"argument --synth: unknown synth preset '{value}'"


def _synth_preset(value: str | None, *, bare_flag: bool) -> str | None:
    """Resolve ``--synth`` to a library preset id in any letter case.

    Absent means GM auto-select, and so does the bare flag where there is one; a value
    spelled empty anywhere else is looked up like any other.
    """
    if value is None or (bare_flag and value in ("", _BARE_SYNTH_SPELLING)):
        return None
    from . import synth_preset_names

    return _catalog_token(value, synth_preset_names())


def _project_bounce(
    args: argparse.Namespace,
    *,
    force_synth: bool = False,
    command_name: str = "project bounce",
    binds_source_audio: bool = True,
) -> int:
    preset = _synth_preset(args.synth, bare_flag=not force_synth)
    project = _load_project(args.input)
    try:
        if binds_source_audio:
            _resolve_bounce_audio_sources(project, args)
        project_sample_rate = cast(Any, project).sample_rate
        # Render at the project's own sample rate by default (args.sample_rate is
        # None unless the user passed --sample-rate); an explicit --sample-rate is
        # only accepted when it matches the project's rate, otherwise report the
        # mismatch by name instead of letting the C ABI reject it generically.
        requested_sample_rate = args.sample_rate
        if requested_sample_rate is not None:
            if abs(requested_sample_rate - project_sample_rate) > 1e-6:
                raise ValueError(
                    f"--sample-rate {requested_sample_rate} does not match the project's "
                    f"sample rate ({project_sample_rate:g} Hz); {command_name} renders at "
                    "the project's own rate"
                )
            sample_rate = requested_sample_rate
        else:
            # A project rate is a whole number of hertz (loading refuses anything
            # else), so the WAV header carries it exactly.
            sample_rate = int(project_sample_rate)
        kwargs: dict[str, Any] = _given(args, "instrument_latency_samples")
        # What is left out stays with the library: the derived length, the project's rate and
        # the master's width.
        if args.frames is not None:
            kwargs["total_frames"] = args.frames
        if requested_sample_rate is not None:
            kwargs["sample_rate"] = requested_sample_rate
        if args.channels is not None:
            kwargs["num_channels"] = args.channels
        use_synth = force_synth or args.synth is not None
        if use_synth:
            patch = SynthPatch(use_gm_programs=True) if preset is None else preset
            audio = cast(Any, project).bounce(synth_instruments=[(0, patch)], **kwargs)
        else:
            audio = cast(Any, project).bounce(**kwargs)
        fit = _OutputFit().include(audio)
        frames, channels = _write_project_bounce_wav(args.output, audio, sample_rate, fit=fit)
        _note_output_fit(fit)
        if args.json:
            print(
                _strict_json_dumps(
                    {
                        "output": args.output,
                        "output_gain_db": fit.gain_db,
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


@dataclass(frozen=True)
class _AlignedTake:
    """One take's alignment, in the field order both stdout modes report."""

    source_id: int
    warp_ref_id: int
    clip_count: int
    anchor_count: int
    reference_frames: int
    take_frames: int
    mean_residual_frames: float


def _align_source_paths(
    args: argparse.Namespace, uris: dict[int, str], needed: list[int]
) -> dict[int, str]:
    """Resolve a local file path for every source the alignment has to decode.

    ``project bounce`` binds PCM to the project; this command decodes the files
    itself, so it takes the same two options for the same purpose and keeps the
    paths instead.
    """
    paths: dict[int, str] = {}
    for assignment in list(getattr(args, "audio", None) or []):
        source_id, path = _parse_audio_binding(assignment)
        if source_id not in uris:
            raise ValueError(f"--audio {assignment}: the project has no audio source {source_id}")
        paths[source_id] = path
    if getattr(args, "resolve_audio", False):
        for source_id in needed:
            if source_id in paths:
                continue
            uri = uris.get(source_id, "")
            resolved = _path_from_file_uri(uri)
            if resolved is None:
                raise ValueError(
                    f"source {source_id} ({uri}): --resolve-audio opens file:// URIs only; "
                    f"pass --audio {source_id}=FILE"
                )
            paths[source_id] = resolved
    missing = [source_id for source_id in needed if source_id not in paths]
    if missing:
        # One line per source, as `project bounce` reports it: the caller has to
        # act on every one of them, so a document with four unbound takes must
        # not take four runs to diagnose.
        raise SonareError(
            int(ErrorCode.INVALID_STATE),
            "\n".join(
                f"source {source_id} ({uris.get(source_id, '')}) has no audio; "
                f"pass --audio {source_id}=FILE or --resolve-audio"
                for source_id in missing
            ),
        )
    return paths


def _project_align_takes(args: argparse.Namespace) -> int:
    """Warp every take in a document onto one reference source's timeline."""
    from . import align_take_to_reference

    reference_source = int(args.reference_source)

    document = _read_bounded(args.input, _MAX_PROJECT_OR_MIDI_BYTES)
    project = cast(Any, _project_from_document(document)).project
    try:
        # The clip -> source map has no C-ABI getter, so it is read from the
        # document, which is also where the untruncated source URIs live.
        shape = json.loads(document)
        uris = _audio_source_uris(project, document)
        clips_by_source: dict[int, list[int]] = {}
        for clip in shape.get("clips", []):
            clips_by_source.setdefault(int(clip.get("sourceId", 0)), []).append(
                int(clip.get("id", 0))
            )
        source_ids = {int(source.get("id", 0)) for source in shape.get("sources", [])}
        if reference_source not in source_ids:
            raise ValueError(
                f"--reference-source {reference_source}: the project has no source "
                f"{reference_source}"
            )
        if reference_source not in uris:
            raise ValueError(
                f"--reference-source {reference_source}: source {reference_source} is not an "
                "audio source"
            )
        # A source no clip references is not a take: nothing would carry its warp
        # map, so it is neither aligned nor required to have a path.
        take_ids = sorted(
            source_id
            for source_id in clips_by_source
            if source_id != reference_source and source_id in uris
        )
        if not take_ids:
            raise ValueError(
                "no takes to align: no clip references an audio source other than "
                f"{reference_source}"
            )
        paths = _align_source_paths(args, uris, [reference_source, *take_ids])

        reference_samples, reference_rate = _load_audio(paths[reference_source])
        # The alignment returns both anchor axes in reference-rate samples; the
        # warp map reads both at the project rate.
        anchor_scale = project.sample_rate / float(reference_rate)
        # Ids already in the document are never reused and never renumbered.
        next_warp_id = max([int(entry.get("id", 0)) for entry in shape.get("warpMaps", [])] + [0])
        aligned: list[_AlignedTake] = []
        for source_id in take_ids:
            take_samples, take_rate = _load_audio(paths[source_id])
            aligned_take = align_take_to_reference(
                reference_samples,
                take_samples,
                reference_rate,
                take_sample_rate=take_rate,
                **_given(args, "hop_length", "bins_per_octave"),
            )
            anchors, alignment = aligned_take.anchors, aligned_take.alignment
            next_warp_id += 1
            cast(Any, project).set_warp_map(
                next_warp_id,
                [(warp * anchor_scale, source * anchor_scale) for warp, source in anchors],
                name=f"take-{source_id}",
            )
            clip_ids = sorted(clips_by_source[source_id])
            for clip_id in clip_ids:
                cast(Any, project).set_clip_warp_ref(clip_id, next_warp_id)
                # A map without a mode plays unaligned, so the mode is part of
                # the alignment rather than left to the caller.
                cast(Any, project).set_clip_warp_mode(clip_id, _ALIGNED_WARP_MODE)
            aligned.append(
                _AlignedTake(
                    source_id=source_id,
                    warp_ref_id=next_warp_id,
                    clip_count=len(clip_ids),
                    anchor_count=len(anchors),
                    reference_frames=alignment.reference_frames,
                    take_frames=alignment.take_frames,
                    mean_residual_frames=alignment.mean_residual_frames,
                )
            )
        bytes_written = _write_project_json(project, args.output)
    finally:
        cast(Any, project).close()

    if args.json:
        # Neither path is reported: both are the caller's own arguments.
        print(
            _strict_json_dumps(
                {
                    "reference_source": reference_source,
                    "take_count": len(aligned),
                    "takes": [asdict(take) for take in aligned],
                    "bytes": bytes_written,
                }
            )
        )
    else:
        print(f"Aligned {len(aligned)} take(s) against source {reference_source}")
        for take in aligned:
            print(
                f"  source {take.source_id} -> warp map {take.warp_ref_id}: "
                f"{take.anchor_count} anchors, "
                f"{take.reference_frames}/{take.take_frames} frames, "
                f"mean residual {take.mean_residual_frames:.3f} frames, "
                f"{take.clip_count} clip(s)"
            )
        print(f"Wrote {args.output} ({bytes_written} bytes)")
    return 0


def cmd_project(args: argparse.Namespace) -> int:
    from . import Project, project_abi_version, synth_preset_names

    subcommand = args.project_command
    if subcommand == "abi":
        # The CLI contract prints 0 when arrangement support is compiled out.
        version = project_abi_version() or 0
        print(_strict_json_dumps({"abi_version": version}) if args.json else version)
        return 0
    if subcommand == "new":
        project = Project()
        try:
            # A rate the caller spelled reaches the library as written, a zero included.
            if args.sample_rate is not None:
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
            return EXIT_INVALID_STATE
        return 0
    if subcommand == "compile":
        project = cast(Project, _load_project(args.input))
        try:
            # Compiled for the timeline only to report whether one was produced,
            # as the native front-end reports the C result's own flag.
            result = cast(Any, project).compile(with_timeline=True)
            has_timeline = result.timeline is not None
            if has_timeline:
                result.timeline.close()
            if args.json:
                print(
                    _strict_json_dumps(
                        {
                            "has_timeline": has_timeline,
                            "diagnostic_count": len(result.diagnostics),
                            "diagnostics": [
                                # A project-wide diagnostic names no target.
                                {
                                    "code": diagnostic.code,
                                    "severity": diagnostic.severity,
                                    "target_id": _absent_if_none(diagnostic.target_id),
                                    "message": diagnostic.message,
                                }
                                for diagnostic in result.diagnostics
                            ],
                            "messages": "\n".join(d.message for d in result.diagnostics),
                        }
                    )
                )
            else:
                print(
                    "  Compiled"
                    if has_timeline
                    else f"  Compiled with errors ({len(result.diagnostics)} diagnostics)"
                )
            if has_timeline:
                return 0
            return EXIT_INVALID_STATE
        finally:
            cast(Any, project).close()
    if subcommand == "bounce":
        return _project_bounce(args)
    if subcommand == "align-takes":
        return _project_align_takes(args)
    if subcommand == "export-smf":
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
        data = _read_bounded(args.input, _MAX_PROJECT_OR_MIDI_BYTES)
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
        data = _read_bounded(args.input, _MAX_PROJECT_OR_MIDI_BYTES)
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
        names = list(synth_preset_names())
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


def _transcribe_refusal(args: argparse.Namespace) -> str | None:
    """The refusal of a --transcribe combination decided from argv, or None when it reads."""
    f0_min, f0_max = args.f0_min_hz, args.f0_max_hz
    if f0_min is not None and f0_max is not None and f0_max <= f0_min:
        return (
            f"--f0-max-hz must be greater than --f0-min-hz "
            f"(--f0-min-hz {f0_min:g}, --f0-max-hz {f0_max:g})"
        )
    # 0 is "no split"; a given ratio otherwise has to exceed 1 (the facade's reattack_ratio rule).
    if args.reattack_ratio is not None and args.reattack_ratio != 0 and args.reattack_ratio <= 1:
        return "--reattack-ratio must be 0 (no split) or greater than 1"
    # The C ABI refuses both pairs (sonare_c_transcribe.cpp), naming the fields.
    if args.min_note_division is not None and args.min_note_ms is not None:
        return "--min-note-division and --min-note-ms cannot both be given"
    if args.polyphonic and (args.monophonic_model or "segments") != "segments":
        return "--monophonic-model singing cannot be combined with --polyphonic"
    return None


# The facade reads an absent ratio as "no split", so the polyphonic default the CLI states is given.
_DEFAULT_REATTACK_RATIO = 2.0


def _transcribe_reattack_ratio(args: argparse.Namespace) -> float | None:
    """The ratio the CLI means: 0 is no split, absent is the polyphonic default."""
    if args.reattack_ratio is None:
        return _DEFAULT_REATTACK_RATIO if args.polyphonic else None
    return args.reattack_ratio or None


def cmd_transcribe(args: argparse.Namespace) -> int:
    from . import Project

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
        clip_id = cast(Any, project).add_midi_clip(0.0, length_ppq).clip_id
        note_count = cast(Any, project).transcribe_to_clip(
            clip_id,
            samples,
            sr,
            polyphonic=args.polyphonic,
            reference_hz=args.reference_hz,
            f0_min_hz=args.f0_min_hz,
            f0_max_hz=args.f0_max_hz,
            min_note_ms=args.min_note_ms,
            segmentation_threshold_cents=args.segmentation_threshold_cents,
            velocity_floor_db=args.velocity_floor_db,
            fixed_velocity=args.fixed_velocity,
            max_polyphony=args.max_polyphony,
            min_frame_peak_ratio=args.min_frame_peak_ratio,
            min_ridge_peak_ratio=args.min_ridge_peak_ratio,
            reattack_ratio=_transcribe_reattack_ratio(args),
            min_note_division=args.min_note_division,
            monophonic_model=args.monophonic_model,
            **_given(args, "channel"),
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


def register_project_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the project, MIDI render and transcription commands."""
    common = shared.common

    # Project / MIDI commands
    project_p = sub.add_parser("project", parents=[common], help="Headless project / SMF commands")
    project_sub = project_p.add_subparsers(dest="project_command", required=True)
    # The project-level parser owns the common defaults.  Child parsers must
    # accept the same flags without installing their own defaults, otherwise a
    # value before the project subcommand (for example `project --json abi`)
    # is overwritten by the child parser's false/empty default.
    project_common = _ContractArgumentParser(add_help=False, argument_default=argparse.SUPPRESS)
    project_common.add_argument("--json", action="store_true")
    project_common.add_argument("-o", "--output", type=str)
    project_stdout_common = _ContractArgumentParser(
        add_help=False, argument_default=argparse.SUPPRESS
    )
    project_stdout_common.add_argument("--json", action="store_true")

    project_sub.add_parser(
        "abi", parents=[project_stdout_common], help="Print the project ABI version"
    )
    pnew = project_sub.add_parser(
        "new", parents=[project_common], help="Create an empty project JSON"
    )
    _cli_domain(
        pnew.add_argument(
            "--sample-rate",
            type=_c_int,
            default=None,
            help="Project sample rate (default: the project's own)",
        ),
        minimum=_MIN_GENERATOR_SAMPLE_RATE,
        maximum=_MAX_GENERATOR_SAMPLE_RATE,
    )
    for pname in ("validate", "compile"):
        if pname == "validate":
            # Keep the active validation route's action order aligned with the
            # public contract. Child defaults remain suppressed so flags
            # supplied before the project subcommand are not overwritten by
            # child-parser defaults.
            pp = project_sub.add_parser(pname, help="Project validate")
            pp.add_argument("--json", action="store_true", default=argparse.SUPPRESS)
            pp.add_argument(
                "--strict",
                action="store_true",
                help="Fail (non-zero exit) when the project loads with diagnostics",
            )
            pp.add_argument("input", metavar="project.json", help="Input project JSON")
            pp.add_argument("-o", "--output", type=str, default=argparse.SUPPRESS)
        else:
            pp = project_sub.add_parser(
                pname, parents=[project_stdout_common], help="Project compile"
            )
            pp.add_argument("input", metavar="project.json", help="Input project JSON")
    sf2_cli_note = (
        "SF2 / SoundFont and per-destination synth JSON are not wired through this CLI command; "
        "use the Project API for SoundFont-backed bounces."
    )
    pbounce = project_sub.add_parser(
        "bounce",
        parents=[project_common],
        help="Render project to WAV",
        description=sf2_cli_note,
    )
    pbounce.add_argument("input", metavar="project.json", help="Input project JSON")
    pbounce.add_argument(
        "--sample-rate",
        type=_c_int,
        default=None,
        help=(
            "Render sample rate; must equal the project's own rate (not a resample). "
            "Default: the project's own sample rate."
        ),
    )
    # The render length is an int64 count, so a lower bound alone leaves it unbounded above.
    _cli_domain(
        pbounce.add_argument(
            "--frames",
            type=_int64,
            default=None,
            help="Render length in frames (default: through the last event and its tail)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    pbounce.add_argument(
        "--channels",
        type=_positive_int,
        default=None,
        help=(
            "Render channel count: 1, 2, 6, or 8, at most the scene master's own width "
            "(default: the scene master's width)"
        ),
    )
    pbounce.add_argument(
        "--instrument-latency-samples",
        type=_c_int,
        default=None,
        help="Instrument latency to compensate, in samples (default: 0)",
    )
    # One assignment per occurrence, as --set / --edit / suggest-mix --input do:
    # the id written with the path keeps the pairing in one token, so no second
    # repeatable option has to be kept in step with this one.
    pbounce.add_argument(
        "--audio",
        action="append",
        default=[],
        metavar="SOURCE_ID=WAV",
        help=(
            "Bind decoded PCM to one of the project's audio sources (repeat once per "
            "source); project JSON carries a URI reference only, so a document with "
            "audio clips renders from nothing until its sources are bound"
        ),
    )
    pbounce.add_argument(
        "--resolve-audio",
        action="store_true",
        help=(
            "Open the file:// URIs the document's unresolved audio sources already "
            "carry; any other scheme is refused by name"
        ),
    )
    pbounce.add_argument(
        "--synth",
        nargs="?",
        const="",
        default=None,
        help=(
            "Bare flag uses GM program/channel routing and channel-10 drums; a value selects "
            "a fixed NativeSynth preset; "
            "no --sf2 or --synth-json CLI wiring"
        ),
    )
    palign = project_sub.add_parser(
        "align-takes",
        parents=[project_common],
        help="Align every take in a project against one reference source",
    )
    palign.add_argument("input", metavar="project.json", help="Input project JSON")
    _cli_domain(
        palign.add_argument(
            "--reference-source",
            type=_int64,
            help="Audio source id whose timeline every take is aligned to",
        ),
        minimum=1,
        maximum=_UINT32_MAX,
    )
    palign.add_argument(
        "--audio",
        action="append",
        default=[],
        metavar="SOURCE_ID=WAV",
        help=(
            "Supply the file one of the project's audio sources reads from (repeat once "
            "per source); project JSON carries a URI reference only, so a document's "
            "takes cannot be decoded until their files are named"
        ),
    )
    palign.add_argument(
        "--resolve-audio",
        action="store_true",
        help="Read the file:// URIs the document's audio sources already carry",
    )
    # The C entry spells "library value" as 0, so an explicit 0 is refused here and an
    # omitted flag passes nothing; the resolution's own grid is the core's to enforce.
    _cli_domain(
        palign.add_argument(
            "--hop-length",
            type=_c_int,
            default=None,
            help="Chroma hop in samples (default: 512)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        palign.add_argument(
            "--bins-per-octave",
            type=_c_int,
            default=None,
            help="Chroma bins per octave (default: 12)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    pexport_smf = project_sub.add_parser("export-smf", parents=[project_common], help="Export SMF")
    pexport_smf.add_argument("input", metavar="project.json", help="Input project JSON")
    pimport_smf = project_sub.add_parser("import-smf", parents=[project_common], help="Import SMF")
    pimport_smf.add_argument("input", metavar="song.mid", help="Input Standard MIDI File")
    pexport_midi2 = project_sub.add_parser(
        "export-midi2", parents=[project_common], help="Export MIDI 2.0 Clip File"
    )
    pexport_midi2.add_argument("input", metavar="project.json", help="Input project JSON")
    pimport_midi2 = project_sub.add_parser(
        "import-midi2", parents=[project_common], help="Import MIDI 2.0 Clip File"
    )
    pimport_midi2.add_argument("input", metavar="clip.midi2", help="Input MIDI 2.0 Clip File")
    project_sub.add_parser(
        "synth-presets", parents=[project_stdout_common], help="List NativeSynth presets"
    )

    midi_render_p = sub.add_parser(
        "midi-render",
        parents=[common],
        help="Render a MIDI project through NativeSynth",
        description=sf2_cli_note,
    )
    midi_render_p.add_argument("input", metavar="project.json", help="Input project JSON")
    midi_render_p.add_argument(
        "--sample-rate",
        type=_c_int,
        default=None,
        help=(
            "Render sample rate; must equal the project's own rate (not a resample). "
            "Default: the project's own sample rate."
        ),
    )
    # The render length is an int64 count, so a lower bound alone leaves it unbounded above.
    _cli_domain(
        midi_render_p.add_argument(
            "--frames",
            type=_int64,
            default=None,
            help="Render length in frames (default: through the last event and its tail)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    midi_render_p.add_argument(
        "--channels",
        type=_positive_int,
        default=None,
        help=(
            "Render channel count: 1, 2, 6, or 8, at most the scene master's own width "
            "(default: the scene master's width)"
        ),
    )
    midi_render_p.add_argument(
        "--instrument-latency-samples",
        type=_c_int,
        default=None,
        help="Instrument latency to compensate, in samples (default: 0)",
    )
    midi_render_p.add_argument(
        "--synth",
        default=None,
        help=(
            "NativeSynth preset (default: GM program/channel routing); "
            "no --sf2 or --synth-json CLI wiring"
        ),
    )

    transcribe_p = sub.add_parser(
        "transcribe", parents=[common], help="Transcribe audio to a Standard MIDI File"
    )
    transcribe_p.add_argument(
        "--tempo-bpm",
        type=_tempo_bpm,
        default=None,
        help="Tempo the PPQ grid is built on (default: detected from the audio)",
    )
    transcribe_p.add_argument(
        "--polyphonic",
        action="store_true",
        help="Read the multi-F0 chain, which finds overlapping notes",
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--reference-hz",
            type=_finite_float,
            default=None,
            help="Tuning reference the MIDI note numbers are measured against (default: 440.0)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--f0-min-hz",
            type=_finite_float,
            default=None,
            help="Low end of the F0 tracker range in Hz; omitted uses 65 mono or 55 poly",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--f0-max-hz",
            type=_finite_float,
            default=None,
            help="High end of the F0 tracker range in Hz; omitted uses 2093 mono or 1760 poly",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--min-note-ms",
            type=_finite_float,
            default=None,
            help="Shortest span kept as a note, in ms "
            "(default: 30.0 (segments), 45.0 (singing) or 60.0 (polyphonic))",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--min-note-division",
            type=_c_int,
            default=None,
            metavar="N",
            help="Shortest note kept, as a note value: N is a 1/N note at the tempo "
            "(32 = thirty-second note), 1-128; not with --min-note-ms",
        ),
        minimum=1.0,
        maximum=128.0,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--max-polyphony",
            type=_c_int,
            default=None,
            metavar="N",
            help="Polyphonic only: voices one frame may hold, 1-64 (default: 10)",
        ),
        minimum=1.0,
        maximum=64.0,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--min-frame-peak-ratio",
            type=_finite_float,
            default=None,
            help="Polyphonic only: a frame's search stops below this share of its first "
            "peak, 0-1 (default: 0.2)",
        ),
        minimum=0.0,
        maximum=1.0,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--min-ridge-peak-ratio",
            type=_finite_float,
            default=None,
            help="Polyphonic only: a ridge ends below this share of its own peak, 0-1 "
            "(default: 0.1)",
        ),
        minimum=0.0,
        maximum=1.0,
    )
    transcribe_p.add_argument(
        "--reattack-ratio",
        type=_finite_float,
        default=None,
        help="Polyphonic only: split a ridge where its salience climbs past this "
        "multiple of the level before, above 1, or 0 for no split (default: 2.0)",
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--segmentation-threshold-cents",
            type=_finite_float,
            default=None,
            help="Monophonic: segments ends a note where the pitch moves this many cents; "
            "singing reads it as the pitch movement kept within one note (default: 50.0)",
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--monophonic-model",
            default=None,
            metavar="{segments,singing}",
            help="Monophonic note model (default: segments): segments cuts the pitch track "
            "where it moves, singing holds vibrato and drift within a note; not with "
            "--polyphonic",
        ),
        choices=["segments", "singing"],
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--velocity-floor-db",
            type=_finite_float,
            default=None,
            help="Level mapped to velocity 1; must be negative (default: -48.0)",
        ),
        maximum=0,
        exclusive_maximum=True,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--fixed-velocity",
            type=_c_int,
            default=None,
            metavar="N",
            help="Give every note velocity N (1-127) and skip the level measurement",
        ),
        minimum=1,
        maximum=127,
    )
    _cli_domain(
        transcribe_p.add_argument(
            "--channel",
            type=_c_int,
            default=None,
            help="MIDI channel the events are emitted on (default: 0)",
        ),
        minimum=0,
        maximum=15,
    )
