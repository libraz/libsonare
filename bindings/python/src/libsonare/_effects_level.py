"""Level normalization and silence trimming (librosa mirrors)."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._ffi import SonareNormalizeStereoResult
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _float_array_result,
    _get_lib,
    _guard_buffer,
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
from .types import NormalizeStereoResult

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


def _normalize_target(fn_name: str, target_db: float) -> float:
    """Narrow one normalizer's ``target_db`` to the domain the C entry accepts.

    Refusing here names the function and the argument; the same value reaching
    the C ABI comes back as a bare ``[4] Invalid parameter``.
    """
    target_db = _validate_scalar(fn_name, target_db, "target_db")
    target_db_c = ctypes.c_float(target_db).value
    if not np.isfinite(target_db_c):
        raise SonareValueError(f"{fn_name}: target_db must fit in a finite float32 value")
    if target_db_c > 0.0:
        raise SonareValueError(f"{fn_name}: target_db must be at or below 0 dBFS")
    return target_db_c


def _run_normalize_stereo(
    fn_name: str,
    symbol: str,
    left: np.ndarray,
    right: np.ndarray,
    sample_rate: int,
    target_db: float,
) -> NormalizeStereoResult:
    """Shared body of the two stereo normalizers, which differ only in symbol.

    The C entry takes ONE length for the pair, so a length mismatch cannot reach
    it -- it would be read as two buffers of whichever length was passed. This
    refuses it here instead, which is also where the pair's own precondition
    (see ``require_stereo_pair``) is expressible on this surface.
    """
    target_db_c = _normalize_target(fn_name, target_db)
    lib = _get_lib()
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    left_array, left_length = _to_c_float_array(left, fn_name=fn_name, arg_name="left")
    right_array, right_length = _to_c_float_array(right, fn_name=fn_name, arg_name="right")
    if left_length != right_length:
        raise SonareValueError(f"{fn_name}: left and right channel lengths must match")
    out = SonareNormalizeStereoResult()
    rc = getattr(lib, symbol)(
        left_array,
        right_array,
        _to_c_size_t(left_length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.c_float(target_db_c),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        length = int(out.length)
        return NormalizeStereoResult(
            left=_float_array_result(out.left, length),
            right=_float_array_result(out.right, length),
            length=length,
            applied_gain_db=float(out.applied_gain_db),
        )
    finally:
        # No dedicated free function for this result: each channel is released
        # with sonare_free_floats (see SonareNormalizeStereoResult in
        # sonare_c_effects.h). A refused call leaves `out` at the empty result
        # the C entry writes before validating, so both pointers are NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


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
    target_db_c = _normalize_target("normalize_rms", target_db)
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


@_guard_buffer("left", "right")
def normalize_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    target_db: float = 0.0,
    *,
    validate: bool = True,
) -> NormalizeStereoResult:
    """Peak-normalize a stereo pair on a gain measured across both channels.

    This is the two-channel counterpart to :func:`normalize`. The peak is taken
    across the pair and the resulting gain is applied to both channels, so the
    louder channel reaches ``target_db`` and the other keeps its distance from
    it. Normalizing each channel on its own gain instead would lift the quieter
    side until the two peaks matched, which changes the stereo balance rather
    than the level.

    A pair that cannot be processed together is refused: either channel empty,
    or the two lengths unequal. The C entry takes one sample rate for the pair,
    so the two channels cannot disagree on it here. A silent pair comes back
    untouched with ``applied_gain_db`` at 0.0.

    A legacy library that lacks the native ``sonare_normalize_stereo`` entry
    point cannot provide the operation, so it raises ``SonareError(NotSupported)``.

    Args:
        left: Left channel samples.
        right: Right channel samples, same length as ``left``.
        sample_rate: Sample rate in Hz, shared by both channels (default 22050).
        target_db: Finite target peak at or below 0 dBFS (default 0.0).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        :class:`NormalizeStereoResult` with the two normalized channels, their
        shared length, and the one gain that was applied to both.

    Example:
        >>> import libsonare
        >>> quiet = [0.05, -0.05] * 512
        >>> loud = [0.2, -0.2] * 512
        >>> result = libsonare.normalize_stereo(quiet, loud, 22050, target_db=-3.0)
        >>> round(result.applied_gain_db, 2)  # 0.2 -> -3 dBFS
        10.98
        >>> round(max(abs(v) for v in result.left), 3)  # the quiet side keeps its distance
        0.177
    """
    return _run_normalize_stereo(
        "normalize_stereo", "sonare_normalize_stereo", left, right, sample_rate, target_db
    )


@_guard_buffer("left", "right")
def normalize_rms_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    target_db: float = -20.0,
    *,
    validate: bool = True,
) -> NormalizeStereoResult:
    """RMS-normalize a stereo pair on a gain measured across both channels.

    This is the two-channel counterpart to :func:`normalize_rms`. The level
    driven to ``target_db`` is the root mean square over the two channels'
    samples together -- the quadratic mean of the per-channel figures, not their
    average -- and the resulting gain goes to both channels, so the stereo
    balance is preserved for the same reason :func:`normalize_stereo` shares its
    gain. As in the mono case the result is hard-clipped to ``[-1, 1]``, which an
    RMS target loud enough to drive peaks past full scale will reach.

    Shares :func:`normalize_stereo`'s pair policy: either channel empty or the
    two lengths unequal is refused, and a silent pair comes back untouched with
    ``applied_gain_db`` at 0.0.

    A legacy library that lacks the native ``sonare_normalize_rms_stereo`` entry
    point cannot provide the operation, so it raises ``SonareError(NotSupported)``.

    Args:
        left: Left channel samples.
        right: Right channel samples, same length as ``left``.
        sample_rate: Sample rate in Hz, shared by both channels (default 22050).
        target_db: Finite RMS target at or below 0 dBFS (default -20.0).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        :class:`NormalizeStereoResult` with the two normalized channels, their
        shared length, and the one gain that was applied to both.

    Example:
        >>> import libsonare
        >>> quiet = [0.05, -0.05] * 512
        >>> loud = [0.2, -0.2] * 512
        >>> result = libsonare.normalize_rms_stereo(quiet, loud, 22050, target_db=-20.0)
        >>> round(result.applied_gain_db, 2)  # joint RMS 0.1458 (-16.73 dBFS) -> -20
        -3.27
    """
    return _run_normalize_stereo(
        "normalize_rms_stereo", "sonare_normalize_rms_stereo", left, right, sample_rate, target_db
    )


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
