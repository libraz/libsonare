"""Refusals the command line alone decides: exit 2, one text, before the input is read."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

MISSING = "/nonexistent-input.wav"
FFT = "argument --n-fft: must be an integer in [1, 2147483647]"
HOP = "argument --hop-length: must be an integer in [1, 2147483647]"
RATE = "argument --target-sample-rate: must be an integer in [8000, 384000]"


def _run_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args], capture_output=True, text=True, env=env
    )


@pytest.mark.parametrize(
    ("arguments", "message"),
    [
        (["key", MISSING, "--n-fft", "0"], FFT),
        (["chords", MISSING, "--n-fft", "0"], FFT),
        (["dynamics", MISSING, "--hop-length", "0"], HOP),
        (["key", MISSING, "--profile", "bogus"], "invalid key profile: bogus"),
        (["key", MISSING, "--modes", "bogus"], "invalid mode: bogus"),
        (["key", MISSING, "--modes", ""], "--modes must contain at least one mode"),
        (["chords", MISSING, "--key-root", "X"], "invalid pitch class: X"),
        (["chords", MISSING, "--key-mode", "bogus"], "invalid mode: bogus"),
        (["analyze", MISSING, "--meter-candidates", "3,x"], "invalid meter numerator: x"),
        (["resample", MISSING, "--target-sample-rate", "100", "-o", "out.wav"], RATE),
        (["resample", MISSING, "--target-sample-rate", "0", "-o", "out.wav"], RATE),
    ],
)
def test_argv_decided_refusals_precede_the_input_read(arguments: list[str], message: str) -> None:
    result = _run_cli(arguments)

    assert result.returncode == 2
    assert message in result.stderr
    assert "nonexistent" not in result.stderr
