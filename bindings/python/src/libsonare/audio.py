"""Audio wrapper for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import TYPE_CHECKING

from ._ffi import SONARE_OK, load_library

if TYPE_CHECKING:
    pass

_lib: ctypes.CDLL | None = None


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure."""
    if rc != SONARE_OK:
        lib = _get_lib()
        msg = lib.sonare_error_message(rc)
        raise RuntimeError(msg.decode("utf-8") if msg else f"sonare error {rc}")


class Audio:
    """Wrapper around the SonareAudio opaque pointer.

    Supports context manager protocol for deterministic resource cleanup.
    """

    def __init__(self, handle: ctypes.c_void_p, lib: ctypes.CDLL) -> None:
        self._handle = handle
        self._lib = lib

    @classmethod
    def from_file(cls, path: str) -> Audio:
        """Load audio from a file path (WAV, MP3, etc.)."""
        lib = _get_lib()
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_file(
            path.encode("utf-8"),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    def from_buffer(
        cls,
        data: Sequence[float] | list[float],
        sample_rate: int = 22050,
    ) -> Audio:
        """Create audio from a float sample buffer.

        Args:
            data: Audio samples as a list of floats or any sequence/array-like.
                  numpy arrays are accepted via the buffer protocol.
            sample_rate: Sample rate in Hz (default 22050).
        """
        lib = _get_lib()
        length = len(data)
        c_array = (ctypes.c_float * length)(*data)
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_buffer(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @classmethod
    def from_memory(cls, data: bytes) -> Audio:
        """Create audio from in-memory WAV/MP3 binary data.

        Args:
            data: Raw file bytes (WAV, MP3, etc.).
        """
        lib = _get_lib()
        length = len(data)
        c_array = (ctypes.c_uint8 * length).from_buffer_copy(data)
        handle = ctypes.c_void_p()
        rc = lib.sonare_audio_from_memory(
            c_array,
            ctypes.c_size_t(length),
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, lib)

    @property
    def data(self) -> list[float]:
        """Return audio samples as a list of floats."""
        ptr = self._lib.sonare_audio_data(self._handle)
        length = self._lib.sonare_audio_length(self._handle)
        return [ptr[i] for i in range(length)]

    @property
    def length(self) -> int:
        """Return the number of audio samples."""
        return int(self._lib.sonare_audio_length(self._handle))

    @property
    def sample_rate(self) -> int:
        """Return the sample rate in Hz."""
        return int(self._lib.sonare_audio_sample_rate(self._handle))

    @property
    def duration(self) -> float:
        """Return the audio duration in seconds."""
        return float(self._lib.sonare_audio_duration(self._handle))

    def close(self) -> None:
        """Free the underlying audio resource."""
        if self._handle:
            self._lib.sonare_audio_free(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> Audio:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()
