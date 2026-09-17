"""Region-wise spectral editing."""

from __future__ import annotations

import ctypes
import dataclasses
from collections.abc import Sequence

from ._ffi import (
    SONARE_SPECTRAL_EDIT_MODE_ATTENUATE,
    SONARE_SPECTRAL_EDIT_MODE_GAIN,
    SONARE_SPECTRAL_EDIT_MODE_HEAL,
    SONARE_SPECTRAL_EDIT_MODE_MUTE,
    SonareSpectralEditConfig,
    SonareSpectralRegionOp,
)
from ._runtime import (
    _INT64_MAX,
    _INT64_MIN,
    SonareValueError,
    _check,
    _float_array_result,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _require_power_of_two,
    _resolve_enum,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_c_int_field,
)

_SPECTRAL_EDIT_MODE_NAMES: dict[str, int] = {
    "gain": SONARE_SPECTRAL_EDIT_MODE_GAIN,
    "attenuate": SONARE_SPECTRAL_EDIT_MODE_ATTENUATE,
    "mute": SONARE_SPECTRAL_EDIT_MODE_MUTE,
    "heal": SONARE_SPECTRAL_EDIT_MODE_HEAL,
}


_SPECTRAL_EDIT_WINDOW_NAMES: dict[str, int] = {
    "hann": 0,
    "hamming": 1,
    "blackman": 2,
    "rectangular": 3,
    "rect": 3,
}


def _coerce_spectral_edit_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _SPECTRAL_EDIT_MODE_NAMES,
        "spectral edit mode",
        underscore=True,
        strip=True,
        validate_int=True,
        reject_bool=True,
    )


def _coerce_spectral_edit_window(value: int | str) -> int:
    return _resolve_enum(
        value,
        _SPECTRAL_EDIT_WINDOW_NAMES,
        "spectral edit window",
        underscore=True,
        strip=True,
        validate_int=True,
        reject_bool=True,
    )


@dataclasses.dataclass
class SpectralRegionOp:
    """One time x frequency rectangle edit op for :func:`spectral_edit`.

    Mirrors :class:`SonareSpectralRegionOp`. ``mode`` is one of ``"gain"``,
    ``"attenuate"``, ``"mute"`` or ``"heal"`` (or the matching integer enum
    value from ``SONARE_SPECTRAL_EDIT_MODE_*``).

    An omitted ``end_sample`` (left at the ``-1`` sentinel) spans to the end of
    the signal, matching the Node/WASM facades where ``endSample`` defaults to
    the signal length.
    """

    start_sample: int = 0
    end_sample: int = -1
    low_hz: float = 0.0
    high_hz: float = 0.0
    gain_db: float = 0.0
    mode: str | int = "gain"


@_guard_buffer("samples")
def spectral_edit(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    ops: Sequence[SpectralRegionOp],
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    window: str | int = "hann",
    heal_radius_frames: int = 2,
) -> list[float]:
    """Region-based spectral editing (STFT -> per-op bin/frame masking -> iSTFT).

    Stateless mono transform; the output has the same length and sample rate as
    the input. Each entry of ``ops`` is a :class:`SpectralRegionOp` describing a
    time x frequency rectangle that is applied in order. An empty ``ops`` list is
    the identity transform.

    Args:
        samples: Audio samples (mono).
        sample_rate: Sample rate in Hz.
        ops: Sequence of :class:`SpectralRegionOp` region edits, applied in order.
        n_fft: STFT size; a power of two in ``[2, 262144]`` (default 2048).
        hop_length: STFT hop; must satisfy ``0 < hop_length <= n_fft / 2``
            (default 512).
        window: Analysis window, one of ``"hann"``, ``"hamming"``,
            ``"blackman"``, ``"rectangular"`` (or the matching integer enum;
            default ``"hann"``).
        heal_radius_frames: Neighbour frames each side used by ``"heal"`` mode
            (default 2).

    Returns:
        List of edited samples.

    Note:
        The shape rules -- ``n_fft`` a power of two, ``hop_length`` within
        ``(0, n_fft / 2]``, ``heal_radius_frames`` non-negative -- are validated
        eagerly here and raise :class:`SonareValueError`, which is both a
        ``ValueError`` (the idiomatic Python contract) and a ``SonareError``. The
        upper bound on ``n_fft`` is a value rule and is checked by the C++ core
        instead, so exceeding it raises a plain :class:`SonareError`; both carry
        ``ErrorCode.INVALID_PARAMETER``, so only ``except ValueError`` tells them
        apart. The Node and WASM surfaces accept the same valid inputs but
        delegate every rejection to the core. The accepted input range is
        identical across surfaces.
    """
    # Narrowed ahead of the range checks below, and for a reason of its own: 0 is
    # the C sentinel for "keep the default" on all three, and int(0.5) is that 0,
    # so a fractional count would run at the default and report success.
    n_fft = _validate_c_int_field("spectral_edit", n_fft, "n_fft")
    hop_length = _validate_c_int_field("spectral_edit", hop_length, "hop_length")
    heal_radius_frames = _validate_c_int_field(
        "spectral_edit", heal_radius_frames, "heal_radius_frames"
    )
    # spectral_edit is deliberately stricter than the shared even-size rule:
    # src/effects/spectral_edit.cpp requires a power of two, so checking it here
    # reports the same rejection eagerly and by name instead of as a generic
    # invalid-parameter return from the core.
    _require_power_of_two(n_fft, "n_fft")
    if hop_length <= 0 or hop_length > n_fft // 2:
        raise SonareValueError("hop_length must satisfy 0 < hop_length <= n_fft / 2")
    if heal_radius_frames < 0:
        raise SonareValueError("heal_radius_frames must be non-negative")
    window_value = _coerce_spectral_edit_window(window)

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareSpectralEditConfig(
        n_fft=n_fft,
        hop_length=hop_length,
        window=int(window_value),
        heal_radius_frames=heal_radius_frames,
    )

    n_ops = len(ops)
    c_ops = None
    if n_ops > 0:
        c_ops = (SonareSpectralRegionOp * n_ops)()
        for i, op in enumerate(ops):
            # An omitted end_sample (-1 sentinel) spans to the end of the signal,
            # matching the Node/WASM facades; the core clamps to [0, length].
            # Narrowed ahead of the sentinel test: int() takes -0.5 as 0 and 100.7
            # as 100, so a fraction would read as a span the caller never asked for.
            requested_end = _narrow_int(
                op.end_sample, f"spectral_edit: ops[{i}].end_sample", _INT64_MIN, _INT64_MAX
            )
            end_sample = requested_end if requested_end >= 0 else length
            c_ops[i] = SonareSpectralRegionOp(
                start_sample=_narrow_int(
                    op.start_sample, f"spectral_edit: ops[{i}].start_sample", _INT64_MIN, _INT64_MAX
                ),
                end_sample=end_sample,
                low_hz=float(op.low_hz),
                high_hz=float(op.high_hz),
                gain_db=float(op.gain_db),
                mode=_coerce_spectral_edit_mode(op.mode),
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
        return _float_array_result(out, out_length.value)
