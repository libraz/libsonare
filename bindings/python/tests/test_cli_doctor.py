"""Tests for the ``sonare doctor`` diagnostics command."""

from __future__ import annotations

import argparse
import json
import sys

import pytest

import libsonare._analysis_music as analysis_music
import libsonare._cli_common as cli_common
import libsonare.cli as cli


def _descriptor() -> dict[str, object]:
    return {
        "version": "1.5.5",
        "abi": {"project": 1, "engine": 3},
        "platform": "darwin-arm64",
        "features": {"mastering": True, "mixing": True, "fx": True, "ffmpeg": False},
        "decode": {"builtin": ["wav", "mp3"], "ffmpeg": []},
        "simd": "neon",
        "hardwareConcurrency": 10,
    }


def test_doctor_json_uses_the_canonical_shared_capability_shape(monkeypatch, capsys) -> None:
    monkeypatch.setattr(analysis_music, "capabilities", _descriptor)
    monkeypatch.setattr(cli_common, "resolved_library_path", lambda: "/tmp/libsonare.dylib")

    assert cli_common.cmd_doctor(argparse.Namespace(json=True)) == 0

    assert json.loads(capsys.readouterr().out) == {
        "version": "1.5.5",
        "abi": {"project": 1, "engine": 3},
        "platform": "darwin-arm64",
        "features": {"mastering": True, "mixing": True, "fx": True, "ffmpeg": False},
        "decode": {"builtin": ["wav", "mp3"], "ffmpeg": []},
        "simd": "neon",
        "hardware_concurrency": 10,
    }


def test_doctor_human_output_summarizes_the_loaded_build(monkeypatch, capsys) -> None:
    monkeypatch.setattr(analysis_music, "capabilities", _descriptor)
    monkeypatch.setattr(cli_common, "resolved_library_path", lambda: "/tmp/libsonare.dylib")

    assert cli_common.cmd_doctor(argparse.Namespace(json=False)) == 0

    output = capsys.readouterr().out
    assert "libsonare 1.5.5" in output
    assert "Library:              /tmp/libsonare.dylib" in output
    assert "project=1, engine=3" in output
    assert "Decode (built-in):    wav, mp3" in output
    assert "Decode (FFmpeg):      none" in output


def test_doctor_parser_dispatches_the_json_flag(monkeypatch) -> None:
    seen: dict[str, bool] = {}

    def doctor(args: argparse.Namespace) -> int:
        seen["json"] = args.json
        return 0

    monkeypatch.setattr(cli, "cmd_doctor", doctor)
    monkeypatch.setattr(sys, "argv", ["sonare", "doctor", "--json"])

    with pytest.raises(SystemExit) as exc:
        cli.main()

    assert exc.value.code == 0
    assert seen == {"json": True}
