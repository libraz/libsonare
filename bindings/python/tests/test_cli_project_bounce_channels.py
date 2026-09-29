"""``project bounce --channels``: the accepted speaker-layout widths.

The core accepts a bounce channel count of 1, 2, 6 or 8, each at most the
scene master's own layout width -- the master-width comparison lives in the
C ABI and is not repeated here. These cover the accepted set end to end
through the installed console script, matching the native CLI's own tests.
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
from pathlib import Path

EXIT_INVALID_PARAMETER = 3

_CLEAN_PROJECT = {"version": 1, "sample_rate": 48000, "tracks": [], "clips": []}


def _console_script() -> Path:
    script = Path(sys.executable).parent / "sonare"
    assert script.is_file(), f"installed console script is missing: {script}"
    return script


def _wav_channel_count(path: Path) -> int:
    """Read nChannels from the fmt chunk directly.

    A >2-channel bounce is WAVE_FORMAT_EXTENSIBLE (matching the native CLI's
    own writer), which stdlib `wave` cannot open.
    """
    with open(path, "rb") as handle:
        header = handle.read(24)
    return struct.unpack("<H", header[22:24])[0]


def _run_console(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(_console_script()), *args], capture_output=True, text=True)


def _write_project(path: Path, document: dict) -> Path:
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def _surround_master_project(layout: str) -> dict:
    """A project whose scene master carries ``layout`` ("5.1" or "7.1")."""
    return {
        **_CLEAN_PROJECT,
        "scene": {
            "version": 1,
            "buses": [{"id": "bus.main", "role": "master", "layout": layout}],
        },
    }


def test_bounce_refuses_a_width_no_speaker_layout_has(tmp_path: Path) -> None:
    project = _write_project(tmp_path / "project.json", _CLEAN_PROJECT)
    output = tmp_path / "bounce.wav"

    for count in ("3", "4"):
        result = _run_console(
            "project", "bounce", "--in", str(project), "-o", str(output), "--channels", count
        )
        assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
        assert not output.exists()


def test_bounce_refuses_channels_that_exceed_a_stereo_master(tmp_path: Path) -> None:
    """6 and 8 are accepted speaker-layout widths, but a project with no scene
    caps its master at stereo, so the render still refuses -- once the C ABI's
    own master-width check runs, not this front-end's option validation."""
    project = _write_project(tmp_path / "project.json", _CLEAN_PROJECT)
    output = tmp_path / "bounce.wav"

    for count in ("6", "8"):
        result = _run_console(
            "project", "bounce", "--in", str(project), "-o", str(output), "--channels", count
        )
        assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
        assert not output.exists()


def test_bounce_renders_channels_up_to_a_surround_masters_width(tmp_path: Path) -> None:
    project_51 = _write_project(tmp_path / "project-51.json", _surround_master_project("5.1"))
    project_71 = _write_project(tmp_path / "project-71.json", _surround_master_project("7.1"))
    output = tmp_path / "bounce.wav"

    # --frames is required: the project has no tracks or clips, so the
    # auto-derive-from-arrangement default (omitting --frames) renders 0
    # frames, which the shared _check_wav_frame_count guard now refuses
    # (consolidation-016) instead of silently writing an empty WAV. Matches
    # the native CLI's own mirror of this case (cli_project_test.cpp, "bounce
    # renders 6/8 channels up to a surround master's own width"), which
    # passes --frames 256 for the same reason.
    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(project_51),
        "-o",
        str(output),
        "--frames",
        "256",
        "--channels",
        "6",
    )
    assert result.returncode == 0, result.stderr
    assert _wav_channel_count(output) == 6

    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(project_71),
        "-o",
        str(output),
        "--frames",
        "256",
        "--channels",
        "8",
    )
    assert result.returncode == 0, result.stderr
    assert _wav_channel_count(output) == 8

    # 8 is a valid speaker-layout width, but still exceeds a 5.1 master's own
    # 6-channel width.
    result = _run_console(
        "project", "bounce", "--in", str(project_51), "-o", str(output), "--channels", "8"
    )
    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
