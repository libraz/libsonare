"""Input-domain checks for the single-strip rendering CLI."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")
EXIT_INVALID_PARAMETER = 3


def _stereo_wav(path: Path) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(48000)
        handle.writeframes(b"".join(struct.pack("<hh", 4000, -3000) for _ in range(64)))


def _run_cli(source: Path, output: Path, *options: str) -> subprocess.CompletedProcess[str]:
    src = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "libsonare.cli",
            "mix-strip",
            str(source),
            *options,
            "-o",
            str(output),
        ],
        capture_output=True,
        text=True,
        env=env,
    )


@pytest.fixture
def stereo_source(tmp_path: Path) -> Path:
    path = tmp_path / "source.wav"
    _stereo_wav(path)
    return path


@pytest.mark.parametrize(
    "option,value,expected_exit",
    [
        ("--pan", "1.5", EXIT_INVALID_PARAMETER),
        ("--pan", "-1.5", EXIT_INVALID_PARAMETER),
        ("--width", "-1", EXIT_INVALID_PARAMETER),
        ("--width", "2.5", EXIT_INVALID_PARAMETER),
        ("--fader-db", "1e100", 2),
        ("--input-trim-db", "1e100", 2),
        ("--pan", "nan", 2),
        ("--pan", "inf", 2),
        ("--width", "nan", 2),
        ("--fader-db", "inf", 2),
        ("--input-trim-db", "nan", 2),
    ],
)
def test_strip_control_errors_match_their_domain(
    stereo_source: Path, tmp_path: Path, option: str, value: str, expected_exit: int
) -> None:
    output = tmp_path / "invalid.wav"
    result = _run_cli(stereo_source, output, option, value, "--json")

    assert result.returncode == expected_exit, result.stderr
    assert not output.exists()


def test_valid_strip_controls_still_render(stereo_source: Path, tmp_path: Path) -> None:
    output = tmp_path / "valid.wav"
    result = _run_cli(
        stereo_source,
        output,
        "--pan",
        "0.5",
        "--pan-mode",
        "stereo-pan",
        "--width",
        "1.5",
        "--fader-db",
        "2",
        "--input-trim-db",
        "-3",
    )

    assert result.returncode == 0, result.stderr
    assert output.exists()


def test_malformed_user_scene_remains_invalid_format(stereo_source: Path, tmp_path: Path) -> None:
    from libsonare import mixing_scene_preset_json

    scene = tmp_path / "invalid-scene.json"
    document = json.loads(mixing_scene_preset_json("vocalReverbSend"))
    document["strips"][0]["pan"] = 1.5
    scene.write_text(json.dumps(document) + "\n")
    output = tmp_path / "invalid-scene.wav"
    src = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src + os.pathsep + env.get("PYTHONPATH", "")
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "libsonare.cli",
            "mix",
            "--scene",
            str(scene),
            "--input",
            str(stereo_source),
            "--output",
            str(output),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 5
    assert "scene" in result.stderr
    assert not output.exists()
