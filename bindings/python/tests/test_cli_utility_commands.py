"""End-to-end CLI coverage for the commands that take no input file.

Each command is checked against the library entry it calls, and each refusal against its
exact text, the usage exit, and the absence of any output file.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import wave
from pathlib import Path
from typing import Any

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

EXIT_USAGE = 2
EXIT_INVALID_PARAMETER = 3
LIST_REFUSAL = "argument --values: must be a comma-separated list of finite numbers"
PEAK = ["--pre-max", "1", "--post-max", "1", "--pre-avg", "1", "--post-avg", "1"]
PEAK_ARGS = [*PEAK, "--delta", "0.1", "--wait", "1"]


def _run_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args], capture_output=True, text=True, env=env
    )


def _json(args: list[str]) -> Any:
    result = _run_cli([*args, "--json"])
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def _wav_frames(path: Path) -> tuple[int, int]:
    with wave.open(str(path), "rb") as handle:
        return handle.getnframes(), handle.getframerate()


def _approx(actual: Any, expected: Any) -> None:
    np.testing.assert_allclose(actual, expected, rtol=1e-6, atol=1e-6)


def test_tone(tmp_path: Path) -> None:
    out = tmp_path / "t.wav"
    payload = _json(
        ["tone", "--frequency-hz", "440", "--duration-sec", "0.01", "--sample-rate", "8000"]
        + ["--phase", "0.5", "--amplitude", "0.5", "-o", str(out)]
    )
    expected = libsonare.tone(440.0, sr=8000, duration=0.01, phi=0.5, amplitude=0.5)
    assert set(payload) == {"output", "output_gain_db", "frequency_hz", "sample_rate", "duration"}
    assert payload["frequency_hz"] == 440 and payload["sample_rate"] == 8000
    assert _wav_frames(out) == (len(expected), 8000)
    _approx(payload["duration"], len(expected) / 8000)


def test_chirp(tmp_path: Path) -> None:
    out = tmp_path / "c.wav"
    payload = _json(
        ["chirp", "--fmax-hz", "2000", "--duration-sec", "0.01", "--sample-rate", "8000"]
        + ["--fmin-hz", "100", "--exponential", "-o", str(out)]
    )
    expected = libsonare.chirp(100.0, 2000.0, sr=8000, duration=0.01, linear=False)
    assert set(payload) == {
        "output",
        "output_gain_db",
        "fmin_hz",
        "fmax_hz",
        "sample_rate",
        "duration",
        "linear",
    }
    assert payload["linear"] is False and payload["fmin_hz"] == 100
    assert _wav_frames(out) == (len(expected), 8000)


def test_clicks(tmp_path: Path) -> None:
    out = tmp_path / "k.wav"
    payload = _json(
        ["clicks", "--times", "0.1,0.2", "--length", "2000", "--sample-rate", "8000"]
        + ["-o", str(out)]
    )
    assert set(payload) == {"output", "output_gain_db", "n_clicks", "sample_rate", "duration"}
    assert payload["n_clicks"] == 2
    assert _wav_frames(out) == (2000, 8000)


def test_frame_conversions() -> None:
    assert _json(["frames-to-samples", "--frames", "3"]) == {
        "samples": libsonare.frames_to_samples(3)
    }
    assert _json(["samples-to-frames", "--samples", "3000", "--n-fft", "1024"]) == {
        "frames": libsonare.samples_to_frames(3000, n_fft=1024)
    }
    result = _run_cli(["samples-to-frames", "--samples", "3000"])
    assert result.stdout.strip() == str(libsonare.samples_to_frames(3000))


@pytest.mark.parametrize(
    ("command", "extra", "call"),
    [
        ("power-to-db", ["--top-db", "20"], lambda v: libsonare.power_to_db(v, top_db=20.0)),
        ("amplitude-to-db", ["--ref", "2"], lambda v: libsonare.amplitude_to_db(v, ref=2.0)),
        ("db-to-power", ["--ref", "2"], lambda v: libsonare.db_to_power(v, ref=2.0)),
        ("db-to-amplitude", [], libsonare.db_to_amplitude),
        ("vector-normalize", ["--norm-type", "2"], lambda v: libsonare.vector_normalize(v, "l2")),
        (
            "pad-center",
            ["--size", "7", "--pad-value", "9"],
            lambda v: libsonare.pad_center(v, 7, 9.0),
        ),
        ("fix-length", ["--size", "2"], lambda v: libsonare.fix_length(v, 2)),
    ],
)
def test_value_list_commands(command: str, extra: list[str], call: Any) -> None:
    values = [1.0, 2.0, 3.0]
    payload = _json([command, "--values", "1,2,3", *extra])
    _approx(payload, call(values))


def test_frame_signal() -> None:
    payload = _json(
        ["frame-signal", "--values", "1,2,3,4,5,6", "--frame-length", "4"] + ["--hop-length", "2"]
    )
    assert set(payload) == {"n_frames", "frames"}
    assert payload["n_frames"] == 2
    assert payload["frames"] == [1, 2, 3, 4, 3, 4, 5, 6]


def test_fix_frames() -> None:
    assert _json(["fix-frames", "--values", "3,5,9", "--x-max", "7"]) == [0, 3, 5, 7]
    assert _json(["fix-frames", "--values", "3,5,9", "--no-pad"]) == [3, 5, 9]


def test_peak_pick() -> None:
    values = "0,1,0,0,2,0,0,3,0"
    expected = libsonare.peak_pick([float(v) for v in values.split(",")], 1, 1, 1, 1, 0.1, 1)
    assert _json(["peak-pick", "--values", values, *PEAK_ARGS]) == expected.tolist()


def test_pcen() -> None:
    matrix = np.arange(1, 7, dtype=float).reshape(2, 3)
    payload = _json(["pcen", "--values", "1,2,3,4,5,6", "--n-bins", "2", "--n-frames", "3"])
    _approx(payload, libsonare.pcen(matrix).reshape(-1))


def test_mastering_stereo_analyses() -> None:
    expected = libsonare.mastering_stereo_analysis_names()
    assert _json(["mastering-stereo-analyses"]) == {"analyses": expected}
    assert _run_cli(["mastering-stereo-analyses"]).stdout.split() == expected


REFUSALS: list[tuple[list[str], str]] = [
    (["power-to-db", "--values", "1,x"], LIST_REFUSAL),
    (["power-to-db", "--values", "1,nan"], LIST_REFUSAL),
    (["power-to-db", "--values", ""], "Missing required option '--values'"),
    (["fix-frames", "--values", "3,5.5"], LIST_REFUSAL),
    (
        ["clicks", "--times", "0.1,y", "-o", "{out}"],
        "argument --times: must be a comma-separated list of finite numbers",
    ),
    (["power-to-db"], "Missing required option '--values'"),
    (["clicks", "-o", "{out}"], "Missing required option '--times'"),
    (["peak-pick", "--values", "1", "--pre-max", "1"], "Missing required option '--post-max'"),
    (
        ["chirp", "--fmax-hz", "2000", "--exponential", "-o", "{out}"],
        "--fmin-hz must be greater than 0 for an exponential sweep",
    ),
    (
        ["pcen", "--values", "1,2,3", "--n-bins", "2", "--n-frames", "2"],
        "argument --values: must hold --n-bins times --n-frames values",
    ),
    (
        ["vector-normalize", "--values", "1", "--norm-type", "5"],
        "argument --norm-type: must be one of 0, 1, 2, 3",
    ),
    (
        ["tone", "--frequency-hz", "440", "--sample-rate", "100", "-o", "{out}"],
        "argument --sample-rate: must be an integer in [8000, 384000]",
    ),
    (
        ["pad-center", "--values", "1", "--size", "0"],
        "argument --size: must be an integer in [1, 2147483647]",
    ),
    (
        ["clicks", "--times", "0.1", "--length", "0", "-o", "{out}"],
        "argument --length: must be an integer in [1, 500000000]",
    ),
    (
        ["samples-to-frames", "--samples", "3", "--n-fft", "0"],
        "argument --n-fft: must be an integer in [1, 2147483647]",
    ),
    (
        ["fix-frames", "--values", "3", "--x-max", "-1"],
        "argument --x-max: must be an integer in [0, 2147483647]",
    ),
    (["frames-to-samples", "--frames", "3", "extra"], "unrecognized arguments: extra"),
]


@pytest.mark.parametrize(("argv", "text"), REFUSALS)
def test_usage_refusals(argv: list[str], text: str, tmp_path: Path) -> None:
    out = tmp_path / "never.wav"
    result = _run_cli([a.replace("{out}", str(out)) for a in argv])
    assert result.returncode == EXIT_USAGE
    assert text in result.stderr
    assert result.stdout == ""
    assert not out.exists()


def test_library_refusals_exit_invalid_parameter() -> None:
    result = _run_cli(["pad-center", "--values", "1,2,3", "--size", "2"])
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "target size smaller than input" in result.stderr
