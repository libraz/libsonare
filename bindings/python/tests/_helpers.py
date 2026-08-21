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

    This is the ONLY skip mechanism, not a final guard behind another one. The
    conftest neither skips nor filters collection: it only points
    ``SONARE_LIB_PATH`` at a development build when the checkout has one, and
    leaves an installed wheel to find its own bundled library. Every module
    that touches the C layer therefore has to declare its own::

        pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason=...)

    A module that omits it fails at collection with an import error rather than
    skipping, because nothing upstream is watching for it.
    """
    try:
        from libsonare._runtime import _get_lib

        _get_lib()
        return True
    except Exception:
        return False


LIB_AVAILABLE: bool = is_lib_available()
