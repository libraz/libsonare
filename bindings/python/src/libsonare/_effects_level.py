"""Level normalization and silence trimming (librosa mirrors)."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _float_array_result,
    _get_lib,
    _int_refusal,
    _narrow_int,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _unsupported_effect_symbol,
    _validate_samples,
    _validate_scalar,
)

# Trim's own RMS framing, which is not the STFT framing the spectral effects
# share -- the two happen to agree on 2048/512 and mean different things.
_DEFAULT_TRIM_FRAME_LENGTH = 2048
_DEFAULT_TRIM_HOP_LENGTH = 512


def _trim_length(value: int, arg_name: str) -> int:
    """Narrow one ``trim`` window length, refusing anything a C ``int`` would wrap.

    The type and range halves are :func:`_narrow_int`, the check every ``_to_c_*``
    reader runs; positive is this argument's own.
    """
    domain = "must fit in a positive signed 32-bit integer"
    try:
        length = _narrow_int(value, arg_name, _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise _int_refusal("trim", value, arg_name, domain) from exc
    if length <= 0:
        raise SonareValueError(f"trim: {arg_name} {domain}")
    return length


def _validate_trim_options(frame_length: int, hop_length: int) -> tuple[int, int]:
    return _trim_length(frame_length, "frame_length"), _trim_length(hop_length, "hop_length")


def normalize(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = 0.0,
    *,
    validate: bool = True,
) -> list[float]:
    """Normalize audio to a target dB level.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Finite target peak at or below 0 dBFS (default 0.0).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of normalized samples.
    """
    _validate_samples("normalize", samples, validate=validate)
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_normalize(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_float(target_db, "target_db"),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _float_array_result(out, out_length.value)


def normalize_rms(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = -20.0,
    *,
    validate: bool = True,
) -> list[float]:
    """Normalize audio by RMS level to a target dBFS value.

    This is the RMS counterpart to :func:`normalize`, using the native
    ``sonare_normalize_rms`` entry point and hard-clipping the result to
    ``[-1, 1]``.  A legacy library that lacks this symbol cannot provide the
    same operation, so it raises ``SonareError(NotSupported)``.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Finite RMS target at or below 0 dBFS (default -20.0).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.
    """
    _validate_samples("normalize_rms", samples, validate=validate)
    target_db = _validate_scalar("normalize_rms", target_db, "target_db")
    target_db_c = ctypes.c_float(target_db).value
    if not np.isfinite(target_db_c):
        raise SonareValueError("normalize_rms: target_db must fit in a finite float32 value")
    if target_db_c > 0.0:
        raise SonareValueError("normalize_rms: target_db must be at or below 0 dBFS")
    lib = _get_lib()
    if not hasattr(lib, "sonare_normalize_rms"):
        raise _unsupported_effect_symbol("sonare_normalize_rms")
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_normalize_rms(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.c_float(target_db_c),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


def trim(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    threshold_db: float = -60.0,
    frame_length: int = _DEFAULT_TRIM_FRAME_LENGTH,
    hop_length: int = _DEFAULT_TRIM_HOP_LENGTH,
    *,
    validate: bool = True,
) -> list[float]:
    """Trim silence from the beginning and end of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Silence threshold in dB (default -60.0).
        frame_length: RMS analysis frame length in samples (default 2048).
        hop_length: RMS analysis hop length in samples (default 512).
        validate: Reject empty / NaN / Inf input (default ``True``).

    Returns:
        List of trimmed samples.
    """
    _validate_samples("trim", samples, validate=validate)
    frame_length, hop_length = _validate_trim_options(frame_length, hop_length)
    lib = _get_lib()
    if not hasattr(lib, "sonare_trim_ex"):
        if frame_length != _DEFAULT_TRIM_FRAME_LENGTH or hop_length != _DEFAULT_TRIM_HOP_LENGTH:
            raise _unsupported_effect_symbol("sonare_trim_ex")
        if not hasattr(lib, "sonare_trim"):
            raise _unsupported_effect_symbol("sonare_trim")
        fn_name = "sonare_trim"
    else:
        fn_name = "sonare_trim_ex"
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        if fn_name == "sonare_trim_ex":
            rc = lib.sonare_trim_ex(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(threshold_db, "threshold_db"),
                _to_c_int(frame_length, "frame_length"),
                _to_c_int(hop_length, "hop_length"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        else:
            rc = lib.sonare_trim(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(threshold_db, "threshold_db"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        _check(rc)
        return _float_array_result(out, out_length.value)
