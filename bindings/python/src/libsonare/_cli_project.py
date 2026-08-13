"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import sys
from typing import Any, cast

from ._cli_common import (
    EXIT_INVALID_STATE,
    _atomic_write_bytes,
    _legacy_exit_codes,
    _read_bounded,
    _strict_json_dumps,
    _write_project_bounce_wav,
)

_MAX_PROJECT_OR_MIDI_BYTES = 64 * 1024 * 1024

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
    return Project.from_json_with_diagnostics(data)


def _write_project_json(project: object, path: str) -> int:
    data = cast(Any, project).to_json_bytes()
    _atomic_write_bytes(path, data)
    return len(data)


def _project_bounce(
    args: argparse.Namespace, *, force_synth: bool = False, command_name: str = "project bounce"
) -> int:
    if not args.output:
        raise ValueError(f"{command_name} requires --output")
    project = _load_project(args.input)
    try:
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
        else:
            sample_rate = int(round(project_sample_rate))
        kwargs = {
            "total_frames": args.frames,
            "block_size": args.block_size,
            "num_channels": args.channels,
            "sample_rate": sample_rate,
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
        valid = not (strict and diagnostics)
        if args.json:
            print(
                _strict_json_dumps(
                    {"valid": valid, "bytes": bytes_written, "diagnostics": diagnostics}
                )
            )
        else:
            print(f"  Project JSON is valid ({bytes_written} bytes canonical)")
            for entry in diagnostics:
                print(f"  warning: {entry}", file=sys.stderr)
        if not valid:
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
    return _project_bounce(args, force_synth=True, command_name="midi-render")
