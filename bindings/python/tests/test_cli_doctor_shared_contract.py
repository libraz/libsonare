"""Tests for the shared ``sonare doctor --json`` payload contract."""

from __future__ import annotations

import argparse
import json

import libsonare._analysis_music as analysis_music
import libsonare._cli_common as cli_common


def test_doctor_json_uses_the_closed_shared_capability_shape(monkeypatch, capsys) -> None:
    descriptor = {
        "version": "1.5.5",
        "abi": {"project": 1, "engine": 3},
        "platform": "darwin-arm64",
        "features": {"mastering": True, "mixing": True, "fx": True, "ffmpeg": False},
        "decode": {"builtin": ["wav", "mp3"], "ffmpeg": []},
        "simd": "neon",
        "hardwareConcurrency": 10,
        "futureOnlyField": "not part of the CLI contract",
    }
    monkeypatch.setattr(analysis_music, "capabilities", lambda: descriptor)

    def library_path_must_not_be_read() -> str:
        raise AssertionError("library path is a human-output detail, not shared JSON")

    monkeypatch.setattr(cli_common, "resolved_library_path", library_path_must_not_be_read)

    assert cli_common.cmd_doctor(argparse.Namespace(json=True)) == 0

    payload = json.loads(capsys.readouterr().out)
    assert set(payload) == {
        "version",
        "abi",
        "platform",
        "features",
        "decode",
        "simd",
        "hardware_concurrency",
    }
    assert payload["hardware_concurrency"] == 10
    assert "hardwareConcurrency" not in payload
    assert payload["abi"] == descriptor["abi"]
    assert payload["features"] == descriptor["features"]
    assert payload["decode"] == descriptor["decode"]
