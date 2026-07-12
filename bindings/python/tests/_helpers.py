"""Shared helpers for libsonare Python binding tests."""

from __future__ import annotations

import numpy as np


def sine(freq: float, duration_sec: float, sr: int = 22050, amp: float = 0.5) -> np.ndarray:
    """Return a mono float32 sine tone.

    Shared by the offline-DSP test suites so the tone formula lives in one
    place. The samples are ``amp * sin(2*pi*freq*n/sr)`` for ``n`` in
    ``range(int(sr * duration_sec))``, computed in float64 and cast to float32.
    """
    n = int(sr * duration_sec)
    return (amp * np.sin(2.0 * np.pi * freq * np.arange(n) / sr)).astype(np.float32)


def is_lib_available() -> bool:
    """Returns True if the libsonare shared library is loadable.

    The pytest conftest already short-circuits collection via ``pytest.skip``
    when the .dylib / .so cannot be found on disk, but loading can still fail
    at runtime (e.g. missing symbols, ABI drift). Tests that rely on the C
    layer use this as a final guard so a stale shared library degrades to
    skipped tests instead of import errors.
    """
    try:
        from libsonare._runtime import _get_lib

        _get_lib()
        return True
    except Exception:
        return False


LIB_AVAILABLE: bool = is_lib_available()
