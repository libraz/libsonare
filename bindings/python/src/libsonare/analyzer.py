"""High-level analysis functions wrapping the libsonare C quick API."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import SONARE_OK, SonareAnalysisResult, SonareKey, load_library
from .types import AnalysisResult, Key, Mode, PitchClass, TimeSignature

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


def _to_c_float_array(
    samples: Sequence[float] | list[float],
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Convert a sample sequence to a ctypes float array."""
    length = len(samples)
    c_array = (ctypes.c_float * length)(*samples)
    return c_array, length


def detect_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> float:
    """Detect the BPM (tempo) of audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected BPM value.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_bpm = ctypes.c_float()
    rc = lib.sonare_detect_bpm(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.byref(out_bpm))
    _check(rc)
    return float(out_bpm.value)


def detect_key(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> Key:
    """Detect the musical key of audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected Key with root, mode, and confidence.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_key = SonareKey()
    rc = lib.sonare_detect_key(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                               ctypes.byref(out_key))
    _check(rc)
    return Key(
        root=PitchClass(out_key.root),
        mode=Mode(out_key.mode),
        confidence=float(out_key.confidence),
    )


def detect_beats(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect beat positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of beat times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_beats(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    count = out_count.value
    result = [float(out_times[i]) for i in range(count)]
    if out_times and count > 0:
        lib.sonare_free_floats(out_times)
    return result


def detect_onsets(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect onset positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of onset times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_onsets(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    count = out_count.value
    result = [float(out_times[i]) for i in range(count)]
    if out_times and count > 0:
        lib.sonare_free_floats(out_times)
    return result


def analyze(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> AnalysisResult:
    """Run full audio analysis on samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        AnalysisResult with BPM, key, time signature, and beat times.

    Raises:
        RuntimeError: If analysis fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAnalysisResult()
    rc = lib.sonare_analyze(c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate),
                            ctypes.byref(out))
    _check(rc)

    beat_times = [float(out.beat_times[i]) for i in range(out.beat_count)]

    result = AnalysisResult(
        bpm=float(out.bpm),
        bpm_confidence=float(out.bpm_confidence),
        key=Key(
            root=PitchClass(out.key.root),
            mode=Mode(out.key.mode),
            confidence=float(out.key.confidence),
        ),
        time_signature=TimeSignature(
            numerator=int(out.time_signature.numerator),
            denominator=int(out.time_signature.denominator),
            confidence=float(out.time_signature.confidence),
        ),
        beat_times=beat_times,
    )

    lib.sonare_free_result(ctypes.byref(out))
    return result


def version() -> str:
    """Return the libsonare version string."""
    lib = _get_lib()
    v = lib.sonare_version()
    return v.decode("utf-8") if v else ""
