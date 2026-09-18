"""End-to-end CLI coverage for ``mix-strip``.

Every positive assertion reads the rendered PCM rather than the exit status: the
command's whole product is the WAV it writes, so a suite that only checks a
status passes on a handler that writes silence or ignores its options. The
option assertions are therefore differential -- a changed option has to change
the bytes, and where the direction is well defined the level or the balance is
asserted too.
"""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 48000
FRAMES = 12000

EXIT_INVALID_PARAMETER = 3


def _run_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


def _write_wav(path: Path, planes: list[list[float]]) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(len(planes))
        handle.setsampwidth(2)
        handle.setframerate(SR)
        frames = bytearray()
        for index in range(len(planes[0])):
            for plane in planes:
                value = max(-1.0, min(1.0, plane[index]))
                frames += struct.pack("<h", int(round(value * 32767.0)))
        handle.writeframes(bytes(frames))


def _read_wav(path: Path) -> tuple[list[int], list[int], int, int]:
    """Return the two PCM code planes, the sample rate and the channel count."""
    with wave.open(str(path), "rb") as handle:
        channels = handle.getnchannels()
        sample_rate = handle.getframerate()
        raw = handle.readframes(handle.getnframes())
    codes = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    return codes[0::channels], codes[1::channels] if channels > 1 else [], sample_rate, channels


@pytest.fixture(scope="module")
def stereo_wav(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """A stereo source whose two channels differ, so width and pan are observable."""
    left = [0.6 * math.sin(2.0 * math.pi * 220.0 * n / SR) for n in range(FRAMES)]
    right = [0.4 * math.sin(2.0 * math.pi * 330.0 * n / SR + 0.7) for n in range(FRAMES)]
    path = tmp_path_factory.mktemp("mix-strip") / "stereo.wav"
    _write_wav(path, [left, right])
    return path


@pytest.fixture(scope="module")
def mono_wav(tmp_path_factory: pytest.TempPathFactory) -> Path:
    samples = [0.6 * math.sin(2.0 * math.pi * 220.0 * n / SR) for n in range(FRAMES)]
    path = tmp_path_factory.mktemp("mix-strip") / "mono.wav"
    _write_wav(path, [samples])
    return path


def _render(source: Path, out: Path, *options: str) -> bytes:
    result = _run_cli(["mix-strip", str(source), *options, "-o", str(out)])
    assert result.returncode == 0, result.stderr
    return out.read_bytes()


def test_default_render_is_stereo_and_not_silence(stereo_wav: Path, tmp_path: Path) -> None:
    out = tmp_path / "out.wav"
    _render(stereo_wav, out)
    left, right, sample_rate, channels = _read_wav(out)

    assert channels == 2
    assert sample_rate == SR
    assert len(left) == FRAMES
    # A silent render would satisfy every shape assertion above.
    assert max(abs(code) for code in left) > 1000
    assert max(abs(code) for code in right) > 1000


def test_mono_input_renders_a_stereo_file(mono_wav: Path, tmp_path: Path) -> None:
    out = tmp_path / "out.wav"
    _render(mono_wav, out)
    left, right, _, channels = _read_wav(out)

    assert channels == 2
    assert len(left) == FRAMES
    # A mono source feeds both channels, so the pair is identical at pan centre.
    assert left == right


def test_fader_changes_the_bytes_and_the_level(stereo_wav: Path, tmp_path: Path) -> None:
    unity = _render(stereo_wav, tmp_path / "unity.wav")
    cut = _render(stereo_wav, tmp_path / "cut.wav", "--fader-db", "-12")
    boost = _render(stereo_wav, tmp_path / "boost.wav", "--fader-db", "6")

    assert cut != unity
    assert boost != unity

    peak_unity = max(abs(code) for code in _read_wav(tmp_path / "unity.wav")[0])
    peak_cut = max(abs(code) for code in _read_wav(tmp_path / "cut.wav")[0])
    peak_boost = max(abs(code) for code in _read_wav(tmp_path / "boost.wav")[0])
    assert peak_cut < peak_unity < peak_boost
    # -12 dB is a factor of four, which a byte comparison alone would not catch.
    assert peak_cut == pytest.approx(peak_unity / 3.981, rel=0.02)


def test_input_trim_is_a_separate_gain_stage(stereo_wav: Path, tmp_path: Path) -> None:
    trimmed = _render(stereo_wav, tmp_path / "trim.wav", "--input-trim-db", "-6")
    unity = _render(stereo_wav, tmp_path / "unity.wav")

    assert trimmed != unity
    peak_trim = max(abs(code) for code in _read_wav(tmp_path / "trim.wav")[0])
    peak_unity = max(abs(code) for code in _read_wav(tmp_path / "unity.wav")[0])
    assert peak_trim == pytest.approx(peak_unity / 1.995, rel=0.02)


def test_pan_moves_the_balance(stereo_wav: Path, tmp_path: Path) -> None:
    _render(stereo_wav, tmp_path / "right.wav", "--pan", "0.9")
    _render(stereo_wav, tmp_path / "left.wav", "--pan", "-0.9")

    right_left, right_right, _, _ = _read_wav(tmp_path / "right.wav")
    left_left, left_right, _, _ = _read_wav(tmp_path / "left.wav")

    assert max(abs(c) for c in right_right) > max(abs(c) for c in right_left)
    assert max(abs(c) for c in left_left) > max(abs(c) for c in left_right)


def test_pan_mode_spellings_select_the_mode_they_name(stereo_wav: Path, tmp_path: Path) -> None:
    balance = _render(stereo_wav, tmp_path / "balance.wav", "--pan", "0.5")
    hyphenated = _render(
        stereo_wav, tmp_path / "hyphen.wav", "--pan", "0.5", "--pan-mode", "stereo-pan"
    )
    compact = _render(
        stereo_wav, tmp_path / "compact.wav", "--pan", "0.5", "--pan-mode", "stereopan"
    )
    mixed_case = _render(
        stereo_wav, tmp_path / "mixed.wav", "--pan", "0.5", "--pan-mode", "stereoPan"
    )
    dual = _render(stereo_wav, tmp_path / "dual.wav", "--pan", "0.5", "--pan-mode", "dual-pan")

    # The three spellings of one mode agree, and the modes differ from each
    # other -- an option resolved to a constant would satisfy only the first.
    assert hyphenated == compact == mixed_case
    assert hyphenated != balance
    assert dual != balance
    assert dual != hyphenated


def test_invalid_pan_mode_is_a_parameter_error(stereo_wav: Path, tmp_path: Path) -> None:
    result = _run_cli(
        ["mix-strip", str(stereo_wav), "--pan-mode", "sideways", "-o", str(tmp_path / "out.wav")]
    )
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "invalid pan mode: sideways" in result.stderr
    assert not (tmp_path / "out.wav").exists()


def test_width_changes_the_side_signal(stereo_wav: Path, tmp_path: Path) -> None:
    narrow = _render(stereo_wav, tmp_path / "narrow.wav", "--width", "0.2")
    wide = _render(stereo_wav, tmp_path / "wide.wav", "--width", "1.8")
    assert narrow != wide

    def side_energy(path: Path) -> float:
        left, right, _, _ = _read_wav(path)
        return sum((a - b) ** 2 for a, b in zip(left, right, strict=True))

    assert side_energy(tmp_path / "narrow.wav") < side_energy(tmp_path / "wide.wav")


def test_width_on_a_mono_input_is_refused(mono_wav: Path, tmp_path: Path) -> None:
    out = tmp_path / "out.wav"
    result = _run_cli(["mix-strip", str(mono_wav), "--width", "1.5", "-o", str(out)])

    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "--width requires a stereo input" in result.stderr
    assert not out.exists()


def test_unit_width_on_a_mono_input_is_accepted(mono_wav: Path, tmp_path: Path) -> None:
    """The refusal is on a width that would widen nothing, not on naming the option."""
    out = tmp_path / "out.wav"
    result = _run_cli(["mix-strip", str(mono_wav), "--width", "1.0", "-o", str(out)])
    assert result.returncode == 0, result.stderr
    assert out.exists()


def test_the_first_block_opens_at_the_configured_gain(tmp_path: Path) -> None:
    """The strip is settled before the render, so nothing glides in.

    The gain smoothers take about 2560 samples to converge at 48 kHz. Rendering
    a constant signal makes an unsettled head visible as a level that is not
    constant, which is the whole difference between this path and the native
    front-end's bare strip.
    """
    source = tmp_path / "dc.wav"
    _write_wav(source, [[0.5] * FRAMES, [0.5] * FRAMES])
    out = tmp_path / "out.wav"
    _render(source, out, "--fader-db", "-12")

    left, _, _, _ = _read_wav(out)
    head = left[:2560]
    tail = left[-2560:]
    # The control: the fader did reach the signal at all.
    assert max(abs(code) for code in tail) < 0.5 * 32767 * 0.9
    assert max(abs(code) for code in tail) > 100
    assert max(head) - min(head) <= 1
    assert abs(max(head) - max(tail)) <= 1


def test_json_payload_shape(stereo_wav: Path, tmp_path: Path) -> None:
    result = _run_cli(
        ["mix-strip", str(stereo_wav), "--pan", "0.3", "--json", "-o", str(tmp_path / "out.wav")]
    )
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)

    assert list(payload) == ["sample_rate", "length", "meter"]
    assert payload["sample_rate"] == SR
    assert payload["length"] == FRAMES
    assert list(payload["meter"]) == [
        "peak_db_l",
        "peak_db_r",
        "rms_db_l",
        "rms_db_r",
        "correlation",
        "mono_compat_width",
        "likely_mono_compatible",
        "max_true_peak_db",
    ]
    assert isinstance(payload["meter"]["likely_mono_compatible"], bool)
    for key in ("peak_db_l", "peak_db_r", "rms_db_l", "rms_db_r", "correlation"):
        assert isinstance(payload["meter"][key], float)
    # A meter reading a silent strip would sit at the floor on both sides.
    assert payload["meter"]["peak_db_l"] > -60.0
    assert payload["meter"]["peak_db_r"] > -60.0


def test_the_meter_follows_the_fader(stereo_wav: Path, tmp_path: Path) -> None:
    """The reported meter is the strip's own post-fader tap, not the source's."""

    def peak_db(*options: str) -> float:
        result = _run_cli(
            ["mix-strip", str(stereo_wav), *options, "--json", "-o", str(tmp_path / "out.wav")]
        )
        assert result.returncode == 0, result.stderr
        return float(json.loads(result.stdout)["meter"]["peak_db_l"])

    assert peak_db("--fader-db", "-12") == pytest.approx(peak_db() - 12.0, abs=0.05)
