"""Offline mastering dynamics: compressor, gate and transient shaper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._ffi import (
    SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
    SONARE_COMPRESSOR_DETECTOR_PEAK,
    SONARE_COMPRESSOR_DETECTOR_RMS,
    SonareCompressorConfig,
    SonareGateConfig,
    SonareTransientShaperConfig,
)
from ._runtime import (
    _check,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
    _resolve_enum,
    _to_c_int,
    _to_c_size_t,
    _validate_samples,
)

_COMPRESSOR_DETECTOR_NAMES: dict[str, int] = {
    "peak": SONARE_COMPRESSOR_DETECTOR_PEAK,
    "rms": SONARE_COMPRESSOR_DETECTOR_RMS,
    "log_rms": SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
    "logrms": SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
}


def _coerce_compressor_detector(value: int | str) -> int:
    return _resolve_enum(
        value,
        _COMPRESSOR_DETECTOR_NAMES,
        "compressor detector",
        underscore=True,
        strip=True,
        validate_int=True,
    )


def _run_dynamics(
    lib_fn: Any,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config: Any,
    *,
    fn_name: str = "mastering_dynamics",
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Invoke a dynamics ``(samples, sr, &config, &out, &out_length, &out_latency)``
    C call and return ``(output ndarray, latency_samples)``.
    """
    lib = _get_lib()
    in_buf = _validate_samples(fn_name, samples, validate=validate)
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
        return _from_c_float_array(out, out_length.value), int(latency.value)


def mastering_dynamics_compressor(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -18.0,
    ratio: float = 2.0,
    attack_ms: float = 10.0,
    release_ms: float = 100.0,
    knee_db: float = 0.0,
    makeup_gain_db: float = 0.0,
    auto_makeup: bool = False,
    detector: int | str = "rms",
    sidechain_hpf_enabled: bool = False,
    sidechain_hpf_hz: float = 100.0,
    pdr_time_ms: float = 0.0,
    pdr_release_scale: float = 1.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline feed-forward compressor.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Compression threshold in dB (default -18).
        ratio: Compression ratio (clamped to >= 1; default 2.0).
        attack_ms: Attack time in milliseconds (default 10).
        release_ms: Release time in milliseconds (default 100).
        knee_db: Soft-knee width in dB (default 0).
        makeup_gain_db: Static makeup gain in dB (default 0).
        auto_makeup: Whether to enable automatic makeup gain (default False).
        detector: Detector mode, either an alias (``"peak"``, ``"rms"``,
            ``"log_rms"``) or an integer from
            :data:`SONARE_COMPRESSOR_DETECTOR_PEAK` and friends (default ``"rms"``).
        sidechain_hpf_enabled: Whether to high-pass-filter the sidechain
            (default False).
        sidechain_hpf_hz: Sidechain HPF cutoff in Hz (default 100).
        pdr_time_ms: Program-dependent release time in ms (default 0).
        pdr_release_scale: PDR release multiplier (default 1.0).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``. The output is a
        ``numpy.ndarray`` of ``float32`` with ``shape == samples.shape``.
    """
    config = SonareCompressorConfig(
        threshold_db=float(threshold_db),
        ratio=float(ratio),
        attack_ms=float(attack_ms),
        release_ms=float(release_ms),
        knee_db=float(knee_db),
        makeup_gain_db=float(makeup_gain_db),
        auto_makeup=1 if auto_makeup else 0,
        detector=_coerce_compressor_detector(detector),
        sidechain_hpf_enabled=1 if sidechain_hpf_enabled else 0,
        sidechain_hpf_hz=float(sidechain_hpf_hz),
        pdr_time_ms=float(pdr_time_ms),
        pdr_release_scale=float(pdr_release_scale),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_compressor,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_compressor",
        validate=validate,
    )


def mastering_dynamics_gate(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -50.0,
    attack_ms: float = 2.0,
    release_ms: float = 80.0,
    range_db: float = -80.0,
    hold_ms: float = 0.0,
    close_threshold_db: float = -50.0,
    key_hpf_hz: float = 0.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline noise gate.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Open-state threshold in dB (default -50).
        attack_ms: Attack time in milliseconds (default 2).
        release_ms: Release time in milliseconds (default 80).
        range_db: Closed-state attenuation in dB (default -80).
        hold_ms: Minimum open time in milliseconds (default 0).
        close_threshold_db: Hysteresis close-threshold in dB; clamped to
            ``<= threshold_db`` (default -50).
        key_hpf_hz: Sidechain HPF cutoff in Hz (default 0 = disabled).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``.
    """
    config = SonareGateConfig(
        threshold_db=float(threshold_db),
        attack_ms=float(attack_ms),
        release_ms=float(release_ms),
        range_db=float(range_db),
        hold_ms=float(hold_ms),
        close_threshold_db=float(close_threshold_db),
        key_hpf_hz=float(key_hpf_hz),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_gate,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_gate",
        validate=validate,
    )


def mastering_dynamics_transient_shaper(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
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
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline envelope-difference transient shaper.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
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
        Tuple of ``(output ndarray, latency_samples)``.
    """
    config = SonareTransientShaperConfig(
        attack_gain_db=float(attack_gain_db),
        sustain_gain_db=float(sustain_gain_db),
        fast_attack_ms=float(fast_attack_ms),
        fast_release_ms=float(fast_release_ms),
        slow_attack_ms=float(slow_attack_ms),
        slow_release_ms=float(slow_release_ms),
        sensitivity=float(sensitivity),
        max_gain_db=float(max_gain_db),
        gain_smoothing_ms=float(gain_smoothing_ms),
        lookahead_ms=float(lookahead_ms),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_transient_shaper,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_transient_shaper",
        validate=validate,
    )
