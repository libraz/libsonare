"""Level normalization and silence trimming (librosa mirrors)."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Literal, TypeAlias

import numpy as np

from ._errors import _entry, _refuse, _refuse_type
from ._ffi import SonareNormalizeStereoResult
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _int_refusal,
    _narrow_int,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_samples,
    _validate_scalar,
)
from .types import NormalizeStereoResult

# Trim's own RMS framing, which is not the STFT framing the spectral effects
# share -- the two happen to agree on 2048/512 and mean different things.
_DEFAULT_TRIM_FRAME_LENGTH = 2048
_DEFAULT_TRIM_HOP_LENGTH = 512


NormalizeMode: TypeAlias = Literal["peak", "rms"]
"""The level a normalizer drives to ``target_db``: the sample peak or the RMS."""

# The C entries take the target as a required argument; this per-mode default
# for an omitted target is the facade default Node and WASM share.
_DEFAULT_NORMALIZE_TARGET_DB = {"peak": 0.0, "rms": -20.0}
_NORMALIZE_SYMBOLS = {"peak": "sonare_normalize", "rms": "sonare_normalize_rms"}
_NORMALIZE_STEREO_SYMBOLS = {
    "peak": "sonare_normalize_stereo",
    "rms": "sonare_normalize_rms_stereo",
}


def _normalize_mode(mode: object) -> str:
    """Refuse a mode outside the :data:`NormalizeMode` tokens, naming them."""
    if not isinstance(mode, str):
        raise _refuse_type("mode must be 'peak' or 'rms'")
    if mode not in _DEFAULT_NORMALIZE_TARGET_DB:
        raise _refuse(f"mode must be 'peak' or 'rms' (got {mode!r})")
    return mode


def _trim_length(value: int, arg_name: str) -> int:
    """Narrow one ``trim`` window length, refusing anything a C ``int`` would wrap.

    The type and range halves are :func:`_narrow_int`, the check every ``_to_c_*``
    reader runs; positive is this argument's own.
    """
    domain = f"must be an integer in [1, {_C_INT_MAX}]"
    try:
        length = _narrow_int(value, arg_name, _C_INT_MIN, _C_INT_MAX)
    except (TypeError, SonareValueError) as exc:
        raise _int_refusal(value, arg_name, domain) from exc
    if length <= 0:
        raise _refuse(f"{arg_name} {domain}")
    return length


def _validate_trim_options(frame_length: int, hop_length: int) -> tuple[int, int]:
    return _trim_length(frame_length, "frame_length"), _trim_length(hop_length, "hop_length")


def _normalize_target(target_db: float) -> float:
    """Narrow one normalizer's ``target_db`` to the domain the C entry accepts.

    Refusing here names the function and the argument; the same value reaching
    the C ABI comes back as a bare ``[4] Invalid parameter``.
    """
    target_db = _validate_scalar(target_db, "target_db")
    target_db_c = ctypes.c_float(target_db).value
    if not np.isfinite(target_db_c):
        raise _refuse("target_db must be finite and in [-3.4028235e38, 3.4028235e38]")
    if target_db_c > 0.0:
        raise _refuse("target_db must be at or below 0 dBFS")
    return target_db_c


def _run_normalize_stereo(
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
    target_db_c = _normalize_target(target_db)
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left, arg_name="left")
    right_array, right_length = _to_c_float_array(right, arg_name="right")
    if left_length != right_length:
        raise _refuse("left and right channel lengths must match")
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
            left=_from_c_float_array(out.left, length),
            right=_from_c_float_array(out.right, length),
            length=length,
            applied_gain_db=float(out.applied_gain_db),
        )
    finally:
        lib.sonare_free_normalize_stereo_result(ctypes.byref(out))


@_entry("normalize")
def normalize(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    mode: NormalizeMode = "peak",
    target_db: float | None = None,
) -> np.ndarray:
    """Normalize audio so its peak or RMS level reaches ``target_db``.

    The ``"rms"`` mode hard-clips the result to ``[-1, 1]``, which a target loud
    enough to drive the peaks past full scale will reach.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        mode: ``"peak"`` (default) or ``"rms"``; any other value is refused by name.
        target_db: Finite target at or below 0 dBFS. Omitted, 0.0 dBFS for
            ``"peak"`` and -20.0 dBFS for ``"rms"``, as on Node and WASM.

    Returns:
        ``float32`` array of normalized samples.
    """
    resolved = _normalize_mode(mode)
    _validate_samples(samples)
    target_db_c = _normalize_target(
        _DEFAULT_NORMALIZE_TARGET_DB[resolved] if target_db is None else target_db
    )
    symbol = _NORMALIZE_SYMBOLS[resolved]
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        rc = getattr(lib, symbol)(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.c_float(target_db_c),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


@_entry("normalize_stereo")
@_guard_buffer("left", "right")
def normalize_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    mode: NormalizeMode = "peak",
    target_db: float | None = None,
) -> NormalizeStereoResult:
    """Normalize a stereo pair on one gain measured across both channels.

    This is the two-channel counterpart to :func:`normalize`. The peak (or, with
    ``mode="rms"``, the RMS over both channels' samples together) is taken across
    the pair and the resulting gain is applied to both channels, so the louder
    channel reaches ``target_db`` and the other keeps its distance from it.
    Normalizing each channel on its own gain instead would lift the quieter side
    until the two levels matched, which changes the stereo balance rather than
    the level. The ``"rms"`` mode hard-clips the result to ``[-1, 1]``.

    A pair that cannot be processed together is refused: either channel empty,
    or the two lengths unequal. The C entry takes one sample rate for the pair,
    so the two channels cannot disagree on it here. A silent pair comes back
    untouched with ``applied_gain_db`` at 0.0.

    A legacy library that lacks the native ``sonare_normalize_stereo`` entry
    point cannot provide the operation, so it raises ``SonareError(NotSupported)``.

    Args:
        left: Left channel samples.
        right: Right channel samples, same length as ``left``.
        sample_rate: Sample rate in Hz, shared by both channels.
        mode: ``"peak"`` (default) or ``"rms"``; any other value is refused by name.
        target_db: Finite target at or below 0 dBFS. Omitted, 0.0 dBFS for
            ``"peak"`` and -20.0 dBFS for ``"rms"``, as on Node and WASM.

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
    resolved = _normalize_mode(mode)
    return _run_normalize_stereo(
        _NORMALIZE_STEREO_SYMBOLS[resolved],
        left,
        right,
        sample_rate,
        _DEFAULT_NORMALIZE_TARGET_DB[resolved] if target_db is None else target_db,
    )


@_entry("trim")
def trim(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    threshold_db: float = -60.0,
    frame_length: int = _DEFAULT_TRIM_FRAME_LENGTH,
    hop_length: int = _DEFAULT_TRIM_HOP_LENGTH,
) -> np.ndarray:
    """Trim silence from the beginning and end of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        threshold_db: Silence threshold in dB (default -60.0).
        frame_length: RMS analysis frame length in samples (default 2048).
        hop_length: RMS analysis hop length in samples (default 512).

    Returns:
        ``float32`` array of trimmed samples.
    """
    _validate_samples(samples)
    frame_length, hop_length = _validate_trim_options(frame_length, hop_length)
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
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
        _check(rc)
        return _from_c_float_array(out, out_length.value)
