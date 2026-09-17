"""Harmonic-percussive source separation and its single-component forms."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SonareHpssResult,
)
from ._runtime import (
    _DEFAULT_EFFECT_HOP_LENGTH,
    _DEFAULT_EFFECT_N_FFT,
    SonareValueError,
    _call_float_transform,
    _check,
    _get_lib,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _unsupported_effect_symbol,
    _validate_effect_fft_options,
    _validate_hpss_kernel,
    _validate_samples,
)
from .types import HpssResult


def hpss(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int = _DEFAULT_EFFECT_HOP_LENGTH,
    hard_mask: bool = False,
    *,
    validate: bool = True,
) -> HpssResult:
    """Perform harmonic-percussive source separation.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        kernel_harmonic: Harmonic median filter kernel size, in STFT frames: a
            positive odd integer at most 524287. The ceiling is 524288 and an
            even kernel is refused, so 524287 is the largest legal value.
        kernel_percussive: Percussive median filter kernel size, in STFT bins,
            under the same rule.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 2
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window (default 512).
        hard_mask: Use binary harmonic/percussive masks (default ``False``).
        validate: Reject empty / NaN / Inf input (default ``True``).

    Returns:
        HpssResult with harmonic and percussive components.
    """
    _validate_samples("hpss", samples, validate=validate)
    n_fft, hop_length = _validate_effect_fft_options("hpss", n_fft, hop_length)
    kernel_harmonic = _validate_hpss_kernel("hpss", kernel_harmonic, "kernel_harmonic")
    kernel_percussive = _validate_hpss_kernel("hpss", kernel_percussive, "kernel_percussive")
    if not isinstance(hard_mask, bool):
        raise SonareValueError("hpss: hard_mask must be a bool")

    lib = _get_lib()
    use_soft_mask = 0 if hard_mask else 1
    if not hasattr(lib, "sonare_hpss_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != _DEFAULT_EFFECT_HOP_LENGTH or hard_mask:
            raise _unsupported_effect_symbol("sonare_hpss_ex")
        if not hasattr(lib, "sonare_hpss"):
            raise _unsupported_effect_symbol("sonare_hpss")
        return _hpss_legacy(lib, samples, sample_rate, kernel_harmonic, kernel_percussive)

    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss_ex(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(use_soft_mask, "use_soft_mask"),
        ctypes.c_int(0),
        ctypes.byref(out),
        None,
    )
    _check(rc)
    try:
        n = out.length
        return HpssResult(
            harmonic=[float(out.harmonic[i]) for i in range(n)],
            percussive=[float(out.percussive[i]) for i in range(n)],
            length=int(n),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))


def _hpss_legacy(
    lib: ctypes.CDLL,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int,
    kernel_percussive: int,
) -> HpssResult:
    """Call the pre-extended HPSS entry point for legacy-default requests."""
    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.length
        return HpssResult(
            harmonic=[float(out.harmonic[i]) for i in range(n)],
            percussive=[float(out.percussive[i]) for i in range(n)],
            length=int(n),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))


def harmonic(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> list[float]:
    """Extract the harmonic component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of harmonic component samples.
    """
    _validate_samples("harmonic", samples, validate=validate)
    return _call_float_transform("sonare_harmonic", samples, _to_c_int(sample_rate, "sample_rate"))


def percussive(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> list[float]:
    """Extract the percussive component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of percussive component samples.
    """
    _validate_samples("percussive", samples, validate=validate)
    return _call_float_transform(
        "sonare_percussive", samples, _to_c_int(sample_rate, "sample_rate")
    )
