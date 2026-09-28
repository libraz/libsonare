"""`sonare playback`: native and Python front ends agree on output channel width.

Generates a small 5.1 WAV fixture and renders it through both CLI front ends,
checking a headphones target writes 2ch and a 7.1 target writes 8ch
WAVE_FORMAT_EXTENSIBLE -- the format neither stdlib `wave` nor a plain PCM
writer emits, so this is the one check that tells the two channel counts apart
from a file a media player would actually place correctly.
"""

from __future__ import annotations

import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SAMPLE_RATE = 22050
_FRAMES = 4410  # 0.2 s, matching the fixture length other CLI contract tests use
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE


def _native_cli() -> Path | None:
    """Locate the native sonare-cli binary this run should exercise.

    `SONARE_NATIVE_CLI` names it explicitly (a private `-B` build directory, per
    AGENT.md's rule against building into `build/` at the repo root); a couple
    of conventional build directories are tried as a fallback for a plain
    in-tree build.
    """
    env_path = os.environ.get("SONARE_NATIVE_CLI")
    if env_path:
        return Path(env_path)
    for candidate in ("build/bin/sonare-cli", "build-shared/bin/sonare-cli"):
        path = ROOT / candidate
        if path.is_file():
            return path
    return None


def _python_cli_argv() -> list[str]:
    """The Python CLI invocation, preferring the installed console script.

    Matches the sibling bounce-channels test's own convention; `-m
    libsonare.cli` is the fallback for a `sys.executable` without one
    installed alongside it.
    """
    script = Path(sys.executable).parent / "sonare"
    if script.is_file():
        return [str(script)]
    return [sys.executable, "-m", "libsonare.cli"]


def _write_surround_wav(path: Path) -> None:
    """A 6-channel (5.1) PCM16 WAV with a distinct tone per plane.

    Plain WAVE_FORMAT_PCM: reading a multichannel file does not require the
    EXTENSIBLE tag this test's own assertions are about on the *output* side.
    """
    frequencies = (220.0, 330.0, 440.0, 55.0, 110.0, 165.0)  # L R C LFE Ls Rs
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(len(frequencies))
        wav.setsampwidth(2)
        wav.setframerate(_SAMPLE_RATE)
        frames = bytearray()
        for n in range(_FRAMES):
            for frequency in frequencies:
                sample = 0.2 * math.sin(2.0 * math.pi * frequency * n / _SAMPLE_RATE)
                frames += struct.pack("<h", int(sample * 32767.0))
        wav.writeframesraw(bytes(frames))


def _wav_channels_and_format_tag(path: Path) -> tuple[int, int]:
    """(channel count, wFormatTag) read directly from the fmt chunk."""
    with open(path, "rb") as handle:
        header = handle.read(40)
    fmt_tag = struct.unpack("<H", header[20:22])[0]
    channels = struct.unpack("<H", header[22:24])[0]
    return channels, fmt_tag


class PlaybackCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.surround_wav = Path(self._tmp.name) / "surround-5.1.wav"
        _write_surround_wav(self.surround_wav)

    def _run(self, argv: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(argv, capture_output=True, text=True, cwd=ROOT, check=False)

    def _assert_target_writes(
        self, run_prefix: list[str], target: str, expected_channels: int, output: Path
    ) -> None:
        result = self._run(
            [*run_prefix, "playback", str(self.surround_wav), "-o", str(output), "--target", target]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        channels, fmt_tag = _wav_channels_and_format_tag(output)
        self.assertEqual(channels, expected_channels)
        if expected_channels > 2:
            self.assertEqual(fmt_tag, _WAVE_FORMAT_EXTENSIBLE)

    def test_native_headphones_output_is_stereo(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_target_writes(
            [str(native)], "headphones", 2, Path(self._tmp.name) / "native-headphones.wav"
        )

    def test_native_seven_one_output_is_extensible(self) -> None:
        native = _native_cli()
        if native is None:
            self.skipTest("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
        self._assert_target_writes([str(native)], "7.1", 8, Path(self._tmp.name) / "native-71.wav")

    def test_python_headphones_output_is_stereo(self) -> None:
        self._assert_target_writes(
            _python_cli_argv(), "headphones", 2, Path(self._tmp.name) / "python-headphones.wav"
        )

    def test_python_seven_one_output_is_extensible(self) -> None:
        self._assert_target_writes(
            _python_cli_argv(), "7.1", 8, Path(self._tmp.name) / "python-71.wav"
        )


if __name__ == "__main__":
    unittest.main()
