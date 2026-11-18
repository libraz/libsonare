"""Offline mastering dynamics: compressor, gate and transient shaper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._errors import SonareValueError
from ._ffi import (
    SonareCompressorConfig,
    SonareGateConfig,
    SonareTransientShaperConfig,
)
from ._narrowing import _narrow_double
from ._runtime import (
    _check,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
    _to_c_int,
    _to_c_size_t,
    _token_ordinal,
    _validate_samples,
)
from ._types_enums import _COMPRESSOR_DETECTORS, CompressorDetector
from ._types_mastering import DynamicsProcessorResult


def _coerce_compressor_detector(value: CompressorDetector) -> int:
    return _token_ordinal(value, _COMPRESSOR_DETECTORS, "compressor detector")


# The C config still pairs an enable flag with a corner it validates as positive
# even while the flag is off, so a disabled sidechain high-pass carries this.
_SIDECHAIN_HIGH_PASS_UNUSED_HZ = 100.0


def _high_pass_corner(value: object, name: str, *, positive: bool) -> float:
    """Narrow a given high-pass corner, refusing one outside its domain by name."""
    corner = _narrow_double(value, name)
    if corner < 0 or (positive and corner == 0):
        bound = "positive" if positive else "non-negative"
        raise SonareValueError(f"{name} must be {bound} (omit it for no high-pass)")
    return corner


def _run_dynamics(
    lib_fn: Any,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config: Any,
    *,
    fn_name: str = "mastering_dynamics",
) -> DynamicsProcessorResult:
    """Invoke a dynamics ``(samples, sr, &config, &out, &out_length, &out_latency)``
    C call and return its :class:`DynamicsProcessorResult`.
    """
    lib = _get_lib()
    in_buf = _validate_samples(fn_name, samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    latency = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib_fn(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(latency),
        )
        _check(rc)
        return DynamicsProcessorResult(
            samples=_from_c_float_array(out, out_length.value),
            latency_samples=int(latency.value),
        )


def mastering_dynamics_compressor(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    threshold_db: float = -18.0,
    ratio: float = 2.0,
    attack_ms: float = 10.0,
    release_ms: float = 100.0,
    knee_db: float = 0.0,
    makeup_gain_db: float = 0.0,
    auto_makeup: bool = False,
    detector: CompressorDetector = "rms",
    sidechain_high_pass_hz: float | None = None,
    pdr_time_ms: float = 0.0,
    pdr_release_scale: float = 1.0,
) -> DynamicsProcessorResult:
    """Apply the offline feed-forward compressor.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        threshold_db: Compression threshold in dB (default -18).
        ratio: Compression ratio (clamped to >= 1; default 2.0).
        attack_ms: Attack time in milliseconds (default 10); one time constant, about
            63% of the way at the set time.
        release_ms: Release time in milliseconds (default 100); as ``attack_ms``.
        knee_db: Soft-knee width in dB (default 0).
        makeup_gain_db: Static makeup gain in dB (default 0).
        auto_makeup: Whether to enable automatic makeup gain (default False).
        detector: Level detector, ``"peak"``, ``"rms"`` or ``"logRms"`` (default ``"rms"``).
        sidechain_high_pass_hz: Sidechain high-pass corner in Hz, above 0.
            ``None`` (the default) leaves the sidechain unfiltered.
        pdr_time_ms: Program-dependent release time in ms (default 0).
        pdr_release_scale: PDR release multiplier (default 1.0).

    Returns:
        A :class:`DynamicsProcessorResult`; ``samples`` is a ``numpy.ndarray`` of
        ``float32`` with ``shape == input.shape``.
    """
    config = SonareCompressorConfig(
        threshold_db=_narrow_double(threshold_db, "threshold_db"),
        ratio=_narrow_double(ratio, "ratio"),
        attack_ms=_narrow_double(attack_ms, "attack_ms"),
        release_ms=_narrow_double(release_ms, "release_ms"),
        knee_db=_narrow_double(knee_db, "knee_db"),
        makeup_gain_db=_narrow_double(makeup_gain_db, "makeup_gain_db"),
        auto_makeup=1 if auto_makeup else 0,
        detector=_coerce_compressor_detector(detector),
        sidechain_hpf_enabled=0 if sidechain_high_pass_hz is None else 1,
        sidechain_hpf_hz=_SIDECHAIN_HIGH_PASS_UNUSED_HZ
        if sidechain_high_pass_hz is None
        else _high_pass_corner(sidechain_high_pass_hz, "sidechain_high_pass_hz", positive=True),
        pdr_time_ms=_narrow_double(pdr_time_ms, "pdr_time_ms"),
        pdr_release_scale=_narrow_double(pdr_release_scale, "pdr_release_scale"),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_compressor,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_compressor",
    )


def mastering_dynamics_gate(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    threshold_db: float = -50.0,
    attack_ms: float = 2.0,
    release_ms: float = 80.0,
    range_db: float = -80.0,
    hold_ms: float = 0.0,
    close_threshold_db: float = -50.0,
    key_high_pass_hz: float | None = None,
) -> DynamicsProcessorResult:
    """Apply the offline noise gate.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        threshold_db: Open-state threshold in dB (default -50).
        attack_ms: Attack time in milliseconds (default 2); one time constant, about
            63% of the way at the set time.
        release_ms: Release time in milliseconds (default 80); as ``attack_ms``.
        range_db: Closed-state attenuation in dB (default -80).
        hold_ms: Minimum open time in milliseconds (default 0).
        close_threshold_db: Hysteresis close-threshold in dB; clamped to
            ``<= threshold_db`` (default -50).
        key_high_pass_hz: Key high-pass corner in Hz, at least 0. ``None`` (the
            default) leaves the key unfiltered, as does 0 Hz.

    Returns:
        A :class:`DynamicsProcessorResult`.
    """
    config = SonareGateConfig(
        threshold_db=_narrow_double(threshold_db, "threshold_db"),
        attack_ms=_narrow_double(attack_ms, "attack_ms"),
        release_ms=_narrow_double(release_ms, "release_ms"),
        range_db=_narrow_double(range_db, "range_db"),
        hold_ms=_narrow_double(hold_ms, "hold_ms"),
        close_threshold_db=_narrow_double(close_threshold_db, "close_threshold_db"),
        key_hpf_hz=0.0
        if key_high_pass_hz is None
        else _high_pass_corner(key_high_pass_hz, "key_high_pass_hz", positive=False),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_gate,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_gate",
    )


def mastering_dynamics_transient_shaper(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    attack_gain_db: float = 3.0,
    sustain_gain_db: float = 0.0,
    fast_attack_ms: float = 0.0,
    fast_release_ms: float = 20.0,
    slow_attack_ms: float = 15.0,
    slow_release_ms: float = 200.0,
    sensitivity: float = 1.0,
    max_gain_db: float = 12.0,
    gain_smoothing_ms: float = 0.0,
    lookahead_ms: float = 0.0,
) -> DynamicsProcessorResult:
    """Apply the offline envelope-difference transient shaper.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        attack_gain_db: Attack-band gain in dB (default +3).
        sustain_gain_db: Sustain-band gain in dB (default 0).
        fast_attack_ms: Fast-envelope attack time in ms (default 0).
        fast_release_ms: Fast-envelope release time in ms (default 20).
        slow_attack_ms: Slow-envelope attack time in ms (default 15).
        slow_release_ms: Slow-envelope release time in ms (default 200).
        sensitivity: Envelope difference sensitivity (clamped >= 0; default 1).
        max_gain_db: Safety clamp on applied gain in dB (default 12).
        gain_smoothing_ms: Gain-signal smoothing time in ms
            (default 0 = disabled).
        lookahead_ms: Lookahead time in ms (default 0 = disabled).

    Returns:
        A :class:`DynamicsProcessorResult`.
    """
    config = SonareTransientShaperConfig(
        attack_gain_db=_narrow_double(attack_gain_db, "attack_gain_db"),
        sustain_gain_db=_narrow_double(sustain_gain_db, "sustain_gain_db"),
        fast_attack_ms=_narrow_double(fast_attack_ms, "fast_attack_ms"),
        fast_release_ms=_narrow_double(fast_release_ms, "fast_release_ms"),
        slow_attack_ms=_narrow_double(slow_attack_ms, "slow_attack_ms"),
        slow_release_ms=_narrow_double(slow_release_ms, "slow_release_ms"),
        sensitivity=_narrow_double(sensitivity, "sensitivity"),
        max_gain_db=_narrow_double(max_gain_db, "max_gain_db"),
        gain_smoothing_ms=_narrow_double(gain_smoothing_ms, "gain_smoothing_ms"),
        lookahead_ms=_narrow_double(lookahead_ms, "lookahead_ms"),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_transient_shaper,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_transient_shaper",
    )
