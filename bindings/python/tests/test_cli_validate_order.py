"""CLI tests: a cross-option refusal is reached before the input is read."""

from __future__ import annotations

import pytest

from ._analyzer_helpers import _run_cli

_UNREADABLE = "/nonexistent/sonare-tests/unreadable.wav"
_CHANNELS = (
    "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width"
)


@pytest.mark.parametrize(
    ("argv", "message"),
    [
        (
            ["key", _UNREADABLE, "--hpss", "--hop-length", "8"],
            "--hop-length must be at least 16 with --hpss (got 8)",
        ),
        (
            ["rhythm", _UNREADABLE, "--bpm-min", "250"],
            "--bpm-max must be greater than --bpm-min (--bpm-min 250, --bpm-max 200)",
        ),
        (["project", "bounce", _UNREADABLE, "--channels", "3", "-o", "{output}"], _CHANNELS),
        (["midi-render", _UNREADABLE, "--channels", "3", "-o", "{output}"], _CHANNELS),
        (["repair", _UNREADABLE], "--output is required unless --detect is given"),
    ],
)
def test_cross_option_refusal_precedes_the_input_read(tmp_path, argv, message) -> None:
    output = str(tmp_path / "o.wav")
    result = _run_cli([output if token == "{output}" else token for token in argv])
    assert result.returncode == 3, result.stderr
    assert message in result.stderr
    assert "unreadable" not in result.stderr


@pytest.mark.parametrize("channels", ["1", "2", "6", "8"])
def test_a_layout_channel_count_passes_validation(tmp_path, channels) -> None:
    result = _run_cli(
        ["project", "bounce", _UNREADABLE, "--channels", channels, "-o", str(tmp_path / "o.wav")]
    )
    assert result.returncode == 4, result.stderr
    assert "--channels" not in result.stderr
