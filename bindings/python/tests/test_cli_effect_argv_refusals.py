"""Effect, voice-preset and note-editing refusals decided from argv: exit 2, before the read."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

MISSING = "/nonexistent-input.wav"
OUT = ["-o", "out.wav"]
PRESETS = "argument --preset: must be one of neutralMonitor, brightIdol"


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
        (
            ["hpss", MISSING, "--harmonic-only", "--percussive-only", *OUT],
            "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
            "--percussive-only, or --with-residual",
        ),
        (
            ["trim-silence", MISSING, "--threshold-db", "-50", "--top-db", "30"],
            "--threshold-db and --top-db are mutually exclusive",
        ),
        (
            ["decompose-stems", MISSING, "--hop-length", "0", *OUT],
            "argument --hop-length: must be an integer in [1, 2147483647]",
        ),
        (
            ["decompose-stems", MISSING, "--mask-power", "0.5", *OUT],
            "argument --mask-power: must be finite and at least 1",
        ),
        (
            ["voice-change", MISSING, "--preset", "neutralMonitor", "--preset-json", "x", *OUT],
            "voice-change preset selectors are mutually exclusive: choose one of --preset, "
            "--preset-json, or --preset-pack",
        ),
        (
            ["voice-change", MISSING, "--preset-pack", "p.json", *OUT],
            "--preset-pack requires --preset to select an entry",
        ),
        (
            ["voice-change", MISSING, "--set", "a=1", *OUT],
            "--set requires --preset, --preset-json, or --preset-pack",
        ),
        (
            ["voice-change", MISSING, "--preset", "neutralMonitor", "--set", "=1", *OUT],
            "invalid --set assignment: =1",
        ),
        (
            ["voice-change", MISSING, "--preset", "neutralMonitor", "--set", "..=1", *OUT],
            "empty --set path",
        ),
        (["voice-change", MISSING, "--preset", "noSuchPreset", *OUT], PRESETS),
        (["voice-preset", "--preset", "noSuchPreset"], PRESETS),
        (
            ["voice-preset-validate", "p.json", "--set", "nopath"],
            "invalid --set assignment: nopath",
        ),
        (
            ["polyphonic-render", MISSING, "--edit", "0.gain_db=abc", *OUT],
            "--edit 0.gain_db must be a number",
        ),
        (
            ["polyphonic-render", MISSING, "--edit", "x.gain_db=1", *OUT],
            "--edit x.gain_db must be an integer",
        ),
        (
            ["transcribe", MISSING, "--reattack-ratio", "0.5", "-o", "out.mid"],
            "--reattack-ratio must be 0 (no split) or greater than 1",
        ),
        (
            ["transcribe", MISSING, "--polyphonic", "--monophonic-model", "singing", "-o", "o.mid"],
            "--monophonic-model singing cannot be combined with --polyphonic",
        ),
        (
            ["transcribe", MISSING, "--f0-min-hz", "100", "--f0-max-hz", "50", "-o", "o.mid"],
            "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz 100, --f0-max-hz 50)",
        ),
        (
            ["transcribe", MISSING, "--velocity-floor-db", "5", "-o", "o.mid"],
            "argument --velocity-floor-db: must be finite and less than 0",
        ),
        (
            ["transcribe", MISSING, "--fixed-velocity", "200", "-o", "o.mid"],
            "argument --fixed-velocity: must be an integer in [1, 127]",
        ),
        (
            ["transcribe", MISSING, "--channel", "16", "-o", "o.mid"],
            "argument --channel: must be an integer in [0, 15]",
        ),
        (
            ["transcribe", MISSING, "--min-note-ms", "0", "-o", "o.mid"],
            "argument --min-note-ms: must be finite and greater than 0",
        ),
    ],
)
def test_argv_decided_refusals_precede_the_input_read(arguments: list[str], message: str) -> None:
    result = _run_cli(arguments)

    assert result.returncode == 2
    assert message in result.stderr
