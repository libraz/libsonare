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
import wave
from pathlib import Path

EXIT_INVALID_PARAMETER = 3

_CLEAN_PROJECT = {"version": 7, "sampleRate": 48000, "tracks": [], "clips": []}


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


def _write_midi_project(path: Path) -> tuple[Path, int]:
    import libsonare as ls

    project = ls.Project()
    try:
        project.set_sample_rate(48000)
        clip_id = project.add_midi_clip(0.0, 4.0).clip_id
        project.set_midi_events(
            clip_id,
            [
                ls.midi_note_on(0.0, 0, 0, 60, 100),
                ls.midi_note_off(2.0, 0, 0, 60, 0),
            ],
        )
        expected_count_in_frames = project.count_in_frames(1)
        path.write_text(project.to_json(), encoding="utf-8")
    finally:
        project.close()
    return path, expected_count_in_frames


def _wav_frames_and_data(path: Path) -> tuple[int, bytes]:
    with wave.open(str(path), "rb") as wav:
        frames = wav.getnframes()
        return frames, wav.readframes(frames)


def _surround_master_project(layout: str) -> dict:
    """A project whose scene master carries ``layout`` ("5.1" or "7.1")."""
    return {
        **_CLEAN_PROJECT,
        "scene": {
            "version": 2,
            "buses": [{"id": "bus.main", "role": "master", "layout": layout}],
        },
    }


def test_bounce_count_in_adds_the_reported_prefix_length(tmp_path: Path) -> None:
    project, expected_count_in_frames = _write_midi_project(tmp_path / "midi-project.json")
    without_path = tmp_path / "without-count-in.wav"
    with_path = tmp_path / "with-count-in.wav"

    without = _run_console("project", "bounce", str(project), "-o", str(without_path), "--json")
    with_count_in = _run_console(
        "project",
        "bounce",
        str(project),
        "-o",
        str(with_path),
        "--count-in-bars",
        "1",
        "--json",
    )

    assert without.returncode == 0, without.stderr
    assert with_count_in.returncode == 0, with_count_in.stderr
    without_frames, _ = _wav_frames_and_data(without_path)
    with_frames, _ = _wav_frames_and_data(with_path)
    without_payload = json.loads(without.stdout)
    count_in_payload = json.loads(with_count_in.stdout)
    assert without_payload["count_in_frames"] == 0
    assert expected_count_in_frames > 0
    assert count_in_payload["count_in_frames"] == expected_count_in_frames
    assert with_frames == count_in_payload["frames"]
    assert without_frames == without_payload["frames"]
    assert with_frames - without_frames == expected_count_in_frames


def test_bounce_metronome_changes_the_render(tmp_path: Path) -> None:
    project, _ = _write_midi_project(tmp_path / "midi-project.json")
    without_path = tmp_path / "without-metronome.wav"
    with_path = tmp_path / "with-metronome.wav"
    common = [
        "project",
        "bounce",
        str(project),
        "--count-in-bars",
        "1",
        "--frames",
        "10000",
        "--json",
    ]

    without = _run_console(*common, "-o", str(without_path), "--metronome=off")
    with_metronome = _run_console(*common, "-o", str(with_path), "--metronome")

    assert without.returncode == 0, without.stderr
    assert with_metronome.returncode == 0, with_metronome.stderr
    without_payload = json.loads(without.stdout)
    with_payload = json.loads(with_metronome.stdout)
    without_frames, without_samples = _wav_frames_and_data(without_path)
    with_frames, with_samples = _wav_frames_and_data(with_path)
    assert without_payload["frames"] == with_payload["frames"] == 10000
    assert without_payload["count_in_frames"] == with_payload["count_in_frames"] == 10000
    assert without_frames == with_frames == 10000
    assert without_samples != with_samples


def test_bounce_refuses_count_in_bars_by_the_typed_option_before_reading_input(
    tmp_path: Path,
) -> None:
    missing_project = tmp_path / "absent-project.json"
    output = tmp_path / "bounce.wav"

    result = _run_console(
        "project",
        "bounce",
        str(missing_project),
        "-o",
        str(output),
        "--count-in-bars",
        "17",
    )

    assert result.returncode == 2
    assert "argument --count-in-bars: must be an integer in [0, 16]" in result.stderr
    assert not output.exists()


def test_bounce_refuses_a_width_no_speaker_layout_has(tmp_path: Path) -> None:
    project = _write_project(tmp_path / "project.json", _CLEAN_PROJECT)
    output = tmp_path / "bounce.wav"

    for count in ("3", "4"):
        result = _run_console(
            "project", "bounce", str(project), "-o", str(output), "--channels", count
        )
        assert result.returncode == 2, result.stderr
        assert not output.exists()


def test_bounce_refuses_channels_that_exceed_a_stereo_master(tmp_path: Path) -> None:
    """6 and 8 are accepted speaker-layout widths, but a project with no scene
    caps its master at stereo, so the render still refuses -- once the C ABI's
    own master-width check runs, not this front-end's option validation."""
    project = _write_project(tmp_path / "project.json", _CLEAN_PROJECT)
    output = tmp_path / "bounce.wav"

    for count in ("6", "8"):
        result = _run_console(
            "project", "bounce", str(project), "-o", str(output), "--channels", count
        )
        assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
        assert not output.exists()


def test_bounce_renders_channels_up_to_a_surround_masters_width(tmp_path: Path) -> None:
    project_51 = _write_project(tmp_path / "project-51.json", _surround_master_project("5.1"))
    project_71 = _write_project(tmp_path / "project-71.json", _surround_master_project("7.1"))
    output = tmp_path / "bounce.wav"

    # --frames is required: the project has no tracks or clips, so the
    # auto-derive-from-arrangement default (omitting --frames) renders 0
    # frames, which the shared _check_wav_request guard now refuses
    # (consolidation-016) instead of silently writing an empty WAV. Matches
    # the native CLI's own mirror of this case (cli_project_test.cpp, "bounce
    # renders 6/8 channels up to a surround master's own width"), which
    # passes --frames 256 for the same reason.
    result = _run_console(
        "project",
        "bounce",
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
        "project", "bounce", str(project_51), "-o", str(output), "--channels", "8"
    )
    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr


def test_project_commands_refuse_synth_audio_and_output_before_the_project_is_read(
    tmp_path,
) -> None:
    """A preset, an --audio binding or an --output on a stdout-only leaf is a usage error."""
    missing = str(tmp_path / "absent.json")
    out = str(tmp_path / "out.wav")
    rows = [
        (
            ["project", "bounce", missing, "-o", out, "--synth", "nosuchsynth"],
            "argument --synth: unknown synth preset 'nosuchsynth'",
        ),
        (
            ["midi-render", missing, "-o", out, "--synth", "nosuchsynth"],
            "argument --synth: unknown synth preset 'nosuchsynth'",
        ),
        (
            ["project", "bounce", missing, "-o", out, "--audio", "nofile"],
            "argument --audio: must be <source_id>=FILE",
        ),
        (
            ["project", "bounce", missing, "-o", out, "--audio", "0=a.wav"],
            "argument --audio: <source_id> must be an integer in [1, 4294967295]",
        ),
        (
            ["project", "align-takes", missing, "-o", out, "--reference-source", "1"]
            + ["--audio", "nofile"],
            "argument --audio: must be <source_id>=FILE",
        ),
        (
            ["project", "abi", "-o", out],
            "Command 'project' does not produce a file output; remove -o/--output",
        ),
        (
            ["project", "new", "-o", out, "--sample-rate", "100"],
            "argument --sample-rate: must be an integer in [8000, 384000]",
        ),
    ]
    for argv, text in rows:
        result = _run_console(*argv)
        assert result.returncode == 2, (argv, result.stderr)
        assert text in result.stderr, (argv, result.stderr)
