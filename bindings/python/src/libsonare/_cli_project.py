"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
from typing import Any, cast

from ._cli_common import (
    EXIT_INVALID_STATE,
    _legacy_exit_codes,
    _strict_json_dumps,
    _write_project_bounce_wav,
)


def _load_project(path: str) -> object:
    from . import Project

    with open(path, encoding="utf-8") as fh:
        return Project.from_json(fh.read())


def _write_project_json(project: object, path: str) -> int:
    data = cast(Any, project).to_json_bytes()
    with open(path, "wb") as fh:
        fh.write(data)
    return len(data)


def _project_bounce(
    args: argparse.Namespace, *, force_synth: bool = False, command_name: str = "project bounce"
) -> int:
    if not args.output:
        raise ValueError(f"{command_name} requires --output")
    project = _load_project(args.input)
    try:
        kwargs = {
            "total_frames": args.frames,
            "block_size": args.block_size,
            "num_channels": args.channels,
            "sample_rate": args.sample_rate,
            "instrument_latency_samples": args.instrument_latency,
        }
        use_synth = force_synth or args.synth is not None
        if use_synth:
            audio = cast(Any, project).bounce_with_synth_instrument(args.synth or None, **kwargs)
        else:
            audio = cast(Any, project).bounce(**kwargs)
        frames, channels = _write_project_bounce_wav(args.output, audio, args.sample_rate)
        if args.json:
            print(
                _strict_json_dumps(
                    {
                        "output": args.output,
                        "frames": frames,
                        "channels": channels,
                        "sample_rate": args.sample_rate,
                        "synth": bool(use_synth),
                    }
                )
            )
        else:
            print(f"  Bounced {frames} frames ({channels} ch @ {args.sample_rate} Hz)")
        return 0
    finally:
        cast(Any, project).close()


def cmd_project(args: argparse.Namespace) -> int:
    from . import Project, project_abi_version, synth_preset_names

    subcommand = args.project_command
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
        project = _load_project(args.input)
        try:
            bytes_written = 0
            if args.output:
                bytes_written = _write_project_json(project, args.output)
            else:
                bytes_written = len(cast(Any, project).to_json_bytes())
        finally:
            cast(Any, project).close()
        if args.json:
            print(_strict_json_dumps({"valid": True, "bytes": bytes_written}))
        else:
            print(f"  Project JSON is valid ({bytes_written} bytes canonical)")
        return 0
    if subcommand == "compile":
        project = _load_project(args.input)
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
        project = _load_project(args.input)
        try:
            data = cast(Any, project).export_smf()
        finally:
            cast(Any, project).close()
        with open(args.output, "wb") as fh:
            fh.write(data)
        print(
            _strict_json_dumps({"output": args.output, "bytes": len(data)})
            if args.json
            else args.output
        )
        return 0
    if subcommand == "import-smf":
        if not args.output:
            raise ValueError("project import-smf requires --output")
        with open(args.smf, "rb") as fh:
            data = fh.read()
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
        project = _load_project(args.input)
        try:
            data = cast(Any, project).export_clip_file()
        finally:
            cast(Any, project).close()
        with open(args.output, "wb") as fh:
            fh.write(data)
        print(
            _strict_json_dumps({"output": args.output, "bytes": len(data)})
            if args.json
            else args.output
        )
        return 0
    if subcommand == "import-midi2":
        if not args.output:
            raise ValueError("project import-midi2 requires --output")
        with open(args.midi2, "rb") as fh:
            data = fh.read()
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
