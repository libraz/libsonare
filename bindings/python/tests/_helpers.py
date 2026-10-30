"""Shared helpers for libsonare Python binding tests."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager

import numpy as np
import pytest


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


def _midi1_word(status: int, channel: int, data0: int, data1: int) -> int:
    return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (data0 << 8) | data1


def patch_mono_decode(monkeypatch, loader) -> None:
    """Route the CLI's mono decode through ``loader(path) -> (samples, rate)``.

    The one seam under both the downmixing and the channel-preserving loaders:
    a mono or downmixed source reaches ``_load_audio_known_channels``, so a test
    replaces the decode itself rather than a loader the production path
    selects between.
    """
    from libsonare import _cli_common

    monkeypatch.setattr(
        _cli_common,
        "_load_audio_known_channels",
        lambda path, channels, *, emit_warning=False: loader(path),
    )


@contextmanager
def raises_coded(code: int | None = None, match: str | None = None) -> Iterator[None]:
    """Assert the block raises a ``SonareError`` of exactly the class and code the cause maps to.

    ``code`` defaults to ``INVALID_PARAMETER``, which is always a ``SonareValueError``
    (a refused argument); every other code must be a plain coded ``SonareError``, so
    neither a generic ``RuntimeError`` nor the wrong class passes.
    """
    from libsonare import ErrorCode, SonareError, SonareValueError

    expected = int(ErrorCode.INVALID_PARAMETER if code is None else code)
    with pytest.raises(SonareError, match=match) as excinfo:
        yield
    error = excinfo.value
    assert error.code == expected, f"expected code {expected}, got {error.code}: {error}"
    if expected == int(ErrorCode.INVALID_PARAMETER):
        assert isinstance(error, SonareValueError), type(error)
    else:
        assert not isinstance(error, SonareValueError), type(error)
