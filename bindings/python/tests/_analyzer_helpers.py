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
    "pytestmark",
]
