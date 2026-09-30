"""Tests for libsonare analyzer functions."""

from __future__ import annotations

import io
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE


def _generate_sine(freq: float, sr: int, duration: float) -> list[float]:
    """Generate a sine wave test signal."""
    n = int(sr * duration)
    return [math.sin(2 * math.pi * freq * i / sr) for i in range(n)]


pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def _ffmpeg_cli() -> str | None:
    """Locate the ffmpeg CLI on PATH; return None when unavailable."""
    return shutil.which("ffmpeg")


def _all_finite(values) -> bool:
    """Return True when every value in the iterable is finite (no NaN/Inf)."""
    return all(math.isfinite(v) for v in values)


def _read_wav_extensible_header(path: str) -> tuple[int, int, int]:
    """(channels, frames, sample_rate) of a WAVE_FORMAT_EXTENSIBLE file.

    Stdlib ``wave`` refuses format tag 0xFFFE, which is exactly what a >2
    channel bounce correctly writes (it carries the speaker-position mask a
    5.1/7.1 player needs, plain WAVE_FORMAT_PCM cannot). This parses the same
    fixed 68-byte RIFF/fmt/data header ``_ExtensibleWavWriter.write_header``
    (``libsonare._cli_common``) emits, instead of going through a reader that
    cannot open the format the product deliberately writes.
    """
    with open(path, "rb") as fh:
        header = fh.read(68)
    assert header[0:4] == b"RIFF"
    assert header[8:12] == b"WAVE"
    assert header[12:16] == b"fmt "
    fields = struct.unpack("<HHIIHHHHI", header[20:44])
    fmt_tag, channels, sample_rate, _byte_rate, _block_align, bits_per_sample = fields[:6]
    assert fmt_tag == 0xFFFE, f"expected WAVE_FORMAT_EXTENSIBLE (0xfffe), got {fmt_tag:#06x}"
    assert header[60:64] == b"data"
    (data_size,) = struct.unpack("<I", header[64:68])
    bytes_per_frame = channels * (bits_per_sample // 8)
    frames = data_size // bytes_per_frame if bytes_per_frame else 0
    return channels, frames, sample_rate


def _has_ffmpeg_build_support() -> bool:
    """Return whether the loaded libsonare was compiled with FFmpeg support.

    Safe to call at collection time: imports lazily so we don't fail when the
    shared library is missing (the pytestmark above already skips in that case).
    """
    try:
        import libsonare

        return libsonare.has_ffmpeg_support()
    except Exception:
        return False


def _write_test_wav(path: str, samples: list[float], sample_rate: int) -> None:
    """Write mono 16-bit PCM WAV using only the standard library."""
    frames = bytearray()
    for s in samples:
        clamped = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(round(clamped * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _run_cli(args: list[str]) -> subprocess.CompletedProcess:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


__all__ = [
    "LIB_AVAILABLE",
    "Path",
    "io",
    "math",
    "os",
    "pytest",
    "shutil",
    "struct",
    "subprocess",
    "sys",
    "tempfile",
    "wave",
    "_all_finite",
    "_ffmpeg_cli",
    "_generate_sine",
    "_has_ffmpeg_build_support",
    "_read_wav_extensible_header",
    "pytestmark",
]
