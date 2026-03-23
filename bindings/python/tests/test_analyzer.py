"""Tests for libsonare analyzer functions."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest


def _lib_available() -> bool:
    """Check if libsonare shared library is available."""
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return True

    project_root = Path(__file__).parent.parent.parent.parent
    lib_name = "libsonare.dylib" if sys.platform == "darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    return build_path.exists()


pytestmark = pytest.mark.skipif(not _lib_available(), reason="libsonare shared library not found")


def test_version() -> None:
    """sonare_version returns a non-empty string."""
    from libsonare import version

    v = version()
    assert isinstance(v, str)
    assert len(v) > 0


def test_detect_bpm_with_silence() -> None:
    """detect_bpm does not crash on silent audio."""
    from libsonare import detect_bpm

    silence = [0.0] * 22050  # 1 second of silence
    bpm = detect_bpm(silence, sample_rate=22050)
    assert isinstance(bpm, float)


def test_detect_key_with_silence() -> None:
    """detect_key does not crash on silent audio."""
    from libsonare import detect_key

    silence = [0.0] * 22050
    key = detect_key(silence, sample_rate=22050)
    assert key.root is not None
    assert key.mode is not None
    assert isinstance(key.confidence, float)


def test_analyze_with_silence() -> None:
    """analyze does not crash on silent audio."""
    from libsonare import analyze

    silence = [0.0] * 22050
    result = analyze(silence, sample_rate=22050)
    assert isinstance(result.bpm, float)
    assert isinstance(result.bpm_confidence, float)
    assert result.key is not None
    assert result.time_signature is not None
    assert isinstance(result.beat_times, list)


def test_invalid_sample_rate() -> None:
    """Invalid sample rate raises RuntimeError."""
    from libsonare import detect_bpm

    silence = [0.0] * 100
    with pytest.raises(RuntimeError):
        detect_bpm(silence, sample_rate=0)
