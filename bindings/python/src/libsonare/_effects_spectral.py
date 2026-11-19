"""Region-wise spectral editing."""

from __future__ import annotations

import ctypes
import dataclasses
import math
from collections.abc import Sequence
from typing import TYPE_CHECKING

import numpy as np
from numpy.typing import NDArray

from ._errors import SonareValueError, _entry, _refuse
from ._ffi import (
    SonareSpectralEditConfig,
    SonareSpectralRegionOp,
)
from ._narrowing import _narrow_double, _narrow_int
from ._runtime import (
    _C_INT_MAX,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _require_power_of_two,
    _resolve_sample_bound,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _token_ordinal,
    _validate_c_int_field,
)
from ._types_enums import _SPECTRAL_EDIT_MODES, _WINDOW_TYPES

if TYPE_CHECKING:
    from ._types_enums import SpectralEditMode, WindowType


@dataclasses.dataclass
class SpectralRegionOp:
    """One time x frequency rectangle edit op for :func:`spectral_edit`.

    ``mode`` is one of ``"gain"``, ``"attenuate"``, ``"mute"`` or ``"heal"``.

    Each time bound is given in samples (``start_sample`` / ``end_sample``) or in
    seconds (``start_sec`` / ``end_sec``, rounded to the nearest sample at the
    call's ``sample_rate``), one spelling per bound; giving both is refused. An
    omitted start is 0 and an omitted end (``None``) spans to the end of the
    signal, matching the Node/WASM facades where ``endSample`` is omitted. A
    negative bound is refused.
    """

    start_sample: int | None = None
    """Region start in input samples; past the end reads as the end, a negative value is
    refused. None is 0. Not with ``start_sec``."""
    end_sample: int | None = None
    """Region end (exclusive) in input samples; past the end reads as the end, a negative value
    is refused. None spans to the end. Not with ``end_sec``."""
    low_hz: float = 0.0
    """Region low frequency edge in Hz, clamped to ``[0, nyquist]``."""
    high_hz: float | None = None
    """Region high frequency edge in Hz, finite and greater than 0; None or ``>= nyquist`` means
    Nyquist."""
    gain_db: float = 0.0
    """Gain in dB for ``"gain"`` / ``"attenuate"``; ignored by ``"mute"`` and ``"heal"``."""
    mode: SpectralEditMode = "gain"
    """How the op modifies the masked bins, a :data:`SpectralEditMode` token."""
    start_sec: float | None = None
    """Region start in seconds, rounded to the nearest sample; None is unset. Not with
    ``start_sample``."""
    end_sec: float | None = None
    """Region end in seconds, rounded to the nearest sample; None is unset. Not with
    ``end_sample``."""


def _spectral_high_hz(value: float | None, name: str) -> float:
    """Map a region's high edge onto the C field, where ``<= 0`` reads as Nyquist."""
    if value is None:
        return 0.0
    try:
        high_hz = _narrow_double(value, name)
    except SonareValueError:
        high_hz = math.nan
    if not high_hz > 0:
        raise _refuse(f"{name} must be finite and greater than 0; omit it for Nyquist")
    return high_hz


@_entry("spectral_edit")
@_guard_buffer("samples")
def spectral_edit(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    ops: Sequence[SpectralRegionOp],
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    window: WindowType = "hann",
    heal_radius_frames: int | None = None,
) -> NDArray[np.float32]:
    """Region-based spectral editing (STFT -> per-op bin/frame masking -> iSTFT).

    Stateless mono transform; the output has the same length and sample rate as
    the input. Each entry of ``ops`` is a :class:`SpectralRegionOp` describing a
    time x frequency rectangle that is applied in order. An empty ``ops`` list is
    the identity transform.

    Args:
        samples: Audio samples (mono).
        sample_rate: Sample rate in Hz.
        ops: Sequence of :class:`SpectralRegionOp` region edits, applied in order.
        n_fft: STFT size; a power of two in ``[4, 262144]``. Default: the
            library's (2048).
        hop_length: STFT hop; must satisfy ``0 < hop_length <= n_fft / 2``.
            Default: ``n_fft / 4``, derived by the library.
        window: Analysis window, one of ``"hann"``, ``"hamming"``,
            ``"blackman"``, ``"rectangular"`` (default ``"hann"``).
        heal_radius_frames: Neighbour frames each side used by ``"heal"`` mode;
            at least 1. Default: the library's (2).

    Returns:
        ``float32`` edited samples.

    Note:
        Leaving a value out is spelled ``None``; an explicit 0 is refused, since
        0 is the C ABI's spelling of "the default". The shape rules of a given
        value -- ``n_fft`` a power of two, ``hop_length`` within
        ``(0, n_fft / 2]`` when ``n_fft`` is given too, ``heal_radius_frames``
        positive -- are validated eagerly here and raise :class:`SonareValueError`, which is both a
        ``ValueError`` (the idiomatic Python contract) and a ``SonareError``. The
        upper bound on ``n_fft`` is a value rule and is checked by the C++ core
        instead, so exceeding it raises a plain :class:`SonareError`; both carry
        ``ErrorCode.INVALID_PARAMETER``, so only ``except ValueError`` tells them
        apart. The Node and WASM facades refuse the same given values before the
        core is reached, so the accepted input range is identical across surfaces.
    """
    # An omitted value reaches the C config as 0, its "keep the default". A given
    # one is narrowed first: int(0.5) is that 0, so a fraction would run at the default.
    c_n_fft = c_hop = c_heal = 0
    if n_fft is not None:
        c_n_fft = _validate_c_int_field(n_fft, "n_fft")
        # spectral_edit.cpp requires a power of two, stricter than the shared even-size rule.
        _require_power_of_two(c_n_fft, "n_fft")
        if c_n_fft < 4:
            raise _refuse("n_fft must be a power of two >= 4")
    if hop_length is not None:
        c_hop = _validate_c_int_field(hop_length, "hop_length")
        if c_hop <= 0 or (c_n_fft and c_hop > c_n_fft // 2):
            raise _refuse("hop_length must satisfy 0 < hop_length <= n_fft / 2")
    if heal_radius_frames is not None:
        c_heal = _narrow_int(heal_radius_frames, "heal_radius_frames", 1, _C_INT_MAX)
    window_value = _token_ordinal(window, _WINDOW_TYPES, "window")

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareSpectralEditConfig(
        n_fft=c_n_fft,
        hop_length=c_hop,
        window=window_value,
        heal_radius_frames=c_heal,
    )

    n_ops = len(ops)
    c_ops = None
    if n_ops > 0:
        c_ops = (SonareSpectralRegionOp * n_ops)()
        for i, op in enumerate(ops):
            # An omitted end spans to the end of the signal; the core clamps to
            # [0, length], so negatives are refused here rather than clamped.
            start_sample = _resolve_sample_bound(
                op.start_sample,
                op.start_sec,
                sample_rate,
                f"ops[{i}].start_sample",
                f"ops[{i}].start_sec",
            )
            end_sample = _resolve_sample_bound(
                op.end_sample,
                op.end_sec,
                sample_rate,
                f"ops[{i}].end_sample",
                f"ops[{i}].end_sec",
            )
            c_ops[i] = SonareSpectralRegionOp(
                start_sample=0 if start_sample is None else start_sample,
                end_sample=length if end_sample is None else end_sample,
                low_hz=_narrow_double(op.low_hz, f"ops[{i}].low_hz"),
                high_hz=_spectral_high_hz(op.high_hz, f"ops[{i}].high_hz"),
                gain_db=_narrow_double(op.gain_db, f"ops[{i}].gain_db"),
                mode=_token_ordinal(op.mode, _SPECTRAL_EDIT_MODES, f"ops[{i}].mode"),
            )

    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_spectral_edit(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(config),
                c_ops,
                _to_c_size_t(n_ops, "n_ops"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)
