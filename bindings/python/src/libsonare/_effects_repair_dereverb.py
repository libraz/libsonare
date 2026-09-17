"""Reverberation repair and its room-estimate configuration helper."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._effects_repair_common import (
    _linked_channel_planes,
    _linked_output_planes,
    _run_detection,
    _run_repair,
)
from ._ffi import (
    SonareDereverbClassicalConfig,
    SonareDereverbReport,
    SonareDereverbStereoResult,
    SonareReverbDetection,
    SonareRoomEstimate,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _check,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _planar_channel_arrays,
    _require_power_of_two,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _unsupported_effect_symbol,
)
from .types import (
    DereverbClassicalConfig,
    DereverbLinkedResult,
    DereverbReport,
    DereverbStereoResult,
    ReverbDetection,
    RoomEstimate,
)


@_guard_buffer("samples")
def mastering_repair_dereverb_classical(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> np.ndarray:
    """Offline classical dereverberator (spectral subtraction + optional WPE)."""
    # The core requires a power of two here (dereverb_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    # Narrowed ahead of the range check: int() takes 256.7 as 256, a hop nobody asked for.
    hop_length_value = _narrow_int(
        hop_length, "mastering_repair_dereverb_classical: hop_length", _C_INT_MIN, _C_INT_MAX
    )
    if hop_length_value <= 0 or hop_length_value > n_fft_value:
        raise SonareValueError("hop_length must be in (0, n_fft]")
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=_narrow_int(
            wpe_iterations,
            "mastering_repair_dereverb_classical: wpe_iterations",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_taps=_narrow_int(
            wpe_taps, "mastering_repair_dereverb_classical: wpe_taps", _C_INT_MIN, _C_INT_MAX
        ),
        wpe_strength=float(wpe_strength),
    )
    return _run_repair(
        _get_lib().sonare_mastering_repair_dereverb_classical, samples, sample_rate, config
    )


def _extract_reverb_detection(raw: Any) -> ReverbDetection:
    return ReverbDetection(
        late_decay_ratio_db=float(raw.late_decay_ratio_db),
        late_predictability=float(raw.late_predictability),
    )


@_guard_buffer("samples")
def mastering_repair_detect_reverb(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> ReverbDetection:
    """Measure reverberation without dereverberating.

    NOT an ISO 3382 reverberation time: no Schroeder integration, no noise-floor
    truncation, STFT bins rather than octave bands, and music is not a free
    decay. Use :func:`libsonare.detect_acoustic` for a graded RT60; this reports
    what the dereverb module itself measures while deciding how much to
    subtract::

        detected = libsonare.mastering_repair_detect_reverb(samples, 44100)
        print(detected.late_decay_ratio_db)

    A buffer shorter than ``n_fft`` is PADDED for analysis and accepted, unlike
    :func:`mastering_repair_detect_noise_floor`, which refuses one.

    ``late_decay_ratio_db`` less negative means the material sustains across the
    module's own late lag, which a late tail does and a dry offset does not, so a
    reverberant input reads HIGHER here than the same material dry.
    ``late_predictability`` is exactly zero unless ``wpe_enabled`` is set -- the
    measurement rather than an unset field -- and under it only the WPE
    covariance and solve run, never the subtraction.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Late-reverb gate the repair would use (default 0.0); not read
            here.
        attenuation: Suppression amount the repair would use (default 1.0); not
            read here.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples, in ``(0, n_fft]`` (default 256).
        t60_sec: Estimated T60 in seconds (default 0.4).
        late_delay_ms: Late-reverb onset relative to direct (default 50.0).
        over_subtraction: Berouti alpha the repair would use (default 1.0); not
            read here.
        spectral_floor: Berouti beta the repair would use (default 0.08); not
            read here.
        wpe_enabled: Run the WPE analysis, which is what fills
            ``late_predictability`` (default False).
        wpe_iterations: WPE EM iterations (default 2).
        wpe_taps: WPE filter taps (default 3).
        wpe_strength: WPE blend weight (default 0.7).

    Returns:
        :class:`~libsonare.types.ReverbDetection`.

    Raises:
        SonareValueError: If ``n_fft`` is not a power of two, if ``hop_length``
            is outside ``(0, n_fft]``, or if the buffer is empty or carries a
            non-finite sample.
        SonareError: If the C call rejects the request.
    """
    # The core requires a power of two here (dereverb_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    hop_length_value = _narrow_int(
        hop_length, "mastering_repair_detect_reverb: hop_length", _C_INT_MIN, _C_INT_MAX
    )
    if hop_length_value <= 0 or hop_length_value > n_fft_value:
        raise SonareValueError("hop_length must be in (0, n_fft]")
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=_narrow_int(
            wpe_iterations, "mastering_repair_detect_reverb: wpe_iterations", _C_INT_MIN, _C_INT_MAX
        ),
        wpe_taps=_narrow_int(
            wpe_taps, "mastering_repair_detect_reverb: wpe_taps", _C_INT_MIN, _C_INT_MAX
        ),
        wpe_strength=float(wpe_strength),
    )
    return _extract_reverb_detection(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_reverb,
            samples,
            sample_rate,
            config,
            SonareReverbDetection,  # noqa: F405
        )
    )


def _extract_dereverb_report(raw: Any) -> DereverbReport:
    return DereverbReport(
        detected=_extract_reverb_detection(raw.detected),
        mean_reduction_db=float(raw.mean_reduction_db),
        suppressed_fraction=float(raw.suppressed_fraction),
        wpe_predictor_norm=float(raw.wpe_predictor_norm),
    )


@_guard_buffer("left", "right")
def mastering_repair_dereverb_classical_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbStereoResult:
    """Dereverberates a stereo pair with one channel-linked mask.

    The mask is built from the channel-summed power, and the WPE stage
    accumulates over both channels and applies one predictor set to each, so
    neither stage can move an interchannel level or phase difference. That is
    also why the result carries one ``report`` rather than one per channel.

    Every field of that report is a ratio or a fraction, so unlike
    :func:`mastering_repair_denoise_classical_stereo` nothing here shifts with
    the channel count and a stereo figure is comparable against a mono one.

    ``detected.late_predictability`` and ``wpe_predictor_norm`` are both
    exactly zero unless ``wpe_enabled`` is set, which it is not by default --
    that is the measurement, not an unset field.

    An input shorter than ``n_fft`` is padded for analysis rather than
    rejected, which is the opposite of
    :func:`mastering_repair_denoise_classical_stereo`.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Late-reverb detection gate; 0 admits everything (default 0).
        attenuation: Suppression amount, linear (default 1.0, full).
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples, in ``(0, n_fft]`` (default 256).
        t60_sec: Estimated T60 in seconds (default 0.4).
        late_delay_ms: Late-reverb onset relative to direct (default 50.0).
        over_subtraction: Berouti alpha (default 1.0).
        spectral_floor: Berouti beta (default 0.08).
        wpe_enabled: Enable the WPE pre-stage (default False).
        wpe_iterations: WPE EM iterations (default 2).
        wpe_taps: WPE filter taps (default 3).
        wpe_strength: WPE blend weight (default 0.7).

    Returns:
        :class:`DereverbStereoResult` with the dereverberated channels and the
        one report the shared mask produced.

    Raises:
        SonareValueError: If ``n_fft`` is not a power of two, if ``hop_length``
            is outside ``(0, n_fft]``, or if the two channels differ in length.
        SonareError: If the C call rejects the request.
    """
    # The core requires a power of two here (dereverb_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    hop_length_value = _narrow_int(
        hop_length,
        "mastering_repair_dereverb_classical_stereo: hop_length",
        _C_INT_MIN,
        _C_INT_MAX,
    )
    if hop_length_value <= 0 or hop_length_value > n_fft_value:
        raise SonareValueError("hop_length must be in (0, n_fft]")

    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=_narrow_int(
            wpe_iterations,
            "mastering_repair_dereverb_classical_stereo: wpe_iterations",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_taps=_narrow_int(
            wpe_taps,
            "mastering_repair_dereverb_classical_stereo: wpe_taps",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_strength=float(wpe_strength),
    )
    out = SonareDereverbStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_dereverb_classical_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        n = int(out.length)
        return DereverbStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            report=_extract_dereverb_report(out.report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDereverbStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


def mastering_repair_dereverb_classical_linked(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbLinkedResult:
    """Dereverberates any number of channels with one channel-linked mask.

    The N-channel form of
    :func:`mastering_repair_dereverb_classical_stereo`. Both stages are shared
    across the whole set rather than just a pair: one mask over the
    channel-summed power, and one WPE predictor set fitted over every
    channel's statistics, so neither can move an interchannel level or phase
    difference however many channels there are. That is also why the result
    carries one ``report`` rather than one per channel.

    A single channel reproduces :func:`mastering_repair_dereverb_classical`
    bit for bit, and two channels reproduce
    :func:`mastering_repair_dereverb_classical_stereo` plane for plane, with
    ``channels[0]`` the left and ``channels[1]`` the right::

        result = libsonare.mastering_repair_dereverb_classical_linked(
            [left, right, centre], 44100
        )
        assert len(result.channels) == 3

    An input shorter than ``n_fft`` is padded for analysis rather than
    rejected, which is the opposite of
    :func:`mastering_repair_denoise_classical_linked`.

    Every field of the report is a ratio or a fraction, so unlike the denoise
    entry nothing here shifts with the channel count and a figure measured
    over a set is comparable against a mono one.
    ``detected.late_predictability`` and ``wpe_predictor_norm`` are both
    exactly zero unless ``wpe_enabled`` is set, which it is not by default --
    that is the measurement, not an unset field.

    Args:
        channels: One input buffer per channel, in the order the outputs come
            back, or a 2-D ``(channels, frames)`` array. All channels must be
            the same length; the C form has one length for the set.
        sample_rate: Sample rate in Hz, shared by every channel (default 22050).
        threshold: Late-reverb detection gate; 0 admits everything (default 0).
        attenuation: Suppression amount, linear (default 1.0, full).
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples, in ``(0, n_fft]`` (default 256).
        t60_sec: Estimated T60 in seconds (default 0.4).
        late_delay_ms: Late-reverb onset relative to direct (default 50.0).
        over_subtraction: Berouti alpha (default 1.0).
        spectral_floor: Berouti beta (default 0.08).
        wpe_enabled: Enable the WPE pre-stage (default False).
        wpe_iterations: WPE EM iterations (default 2).
        wpe_taps: WPE filter taps (default 3).
        wpe_strength: WPE blend weight (default 0.7).

    Returns:
        :class:`DereverbLinkedResult` with one output buffer per input channel
        and the one report the shared mask produced.

    Raises:
        SonareValueError: If ``n_fft`` is not a power of two, if ``hop_length``
            is outside ``(0, n_fft]``, if ``channels`` is empty, if the
            channels disagree in length, or if any channel is empty or carries
            a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    # The core requires a power of two here (dereverb_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    hop_length_value = _narrow_int(
        hop_length,
        "mastering_repair_dereverb_classical_linked: hop_length",
        _C_INT_MIN,
        _C_INT_MAX,
    )
    if hop_length_value <= 0 or hop_length_value > n_fft_value:
        raise SonareValueError("hop_length must be in (0, n_fft]")

    lib = _get_lib()
    symbol = "sonare_mastering_repair_dereverb_classical_linked"
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    planes = _linked_channel_planes("mastering_repair_dereverb_classical_linked", channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=_narrow_int(
            wpe_iterations,
            "mastering_repair_dereverb_classical_linked: wpe_iterations",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_taps=_narrow_int(
            wpe_taps,
            "mastering_repair_dereverb_classical_linked: wpe_taps",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_strength=float(wpe_strength),
    )
    out_buffers, out_ptrs = _linked_output_planes(len(arrays), frame_count)
    report = SonareDereverbReport()  # noqa: F405
    _check(
        lib.sonare_mastering_repair_dereverb_classical_linked(
            ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            _to_c_size_t(len(arrays), "channel_count"),
            _to_c_size_t(frame_count, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.cast(out_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            ctypes.byref(report),
        )
    )
    return DereverbLinkedResult(
        channels=out_buffers,
        report=_extract_dereverb_report(report),
    )


def mastering_repair_dereverb_config_for_room(
    estimate: RoomEstimate,
    *,
    threshold: float = 0.0,
    attenuation: float = 1.0,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> DereverbClassicalConfig:
    """Point a dereverb config at a measured room.

    The pair to :func:`libsonare.estimate_room`, which measures a recording
    blind. Returns a complete config for
    :func:`mastering_repair_dereverb_classical`, so the caller does not have to
    know which reverberation-time band to use or how the late delay relates to
    room size::

        estimate = libsonare.estimate_room(samples, sr)
        config = libsonare.mastering_repair_dereverb_config_for_room(estimate)
        clean = libsonare.mastering_repair_dereverb_classical(samples, sr, **config)

    What the room decides is *where* the tail is. Exactly two keys come back
    changed from what was passed in:

    * ``t60_sec`` -- the mid-frequency reverberation time, the average of the
      500 Hz and 1 kHz octaves an ISO 3382 room is quoted by.
    * ``late_delay_ms`` -- Polack's mixing time, sqrt(volume) in milliseconds,
      past which the response is a diffuse tail rather than separable early
      reflections.

    When NEITHER mid band converged, ``t60_sec`` falls back to the average of
    whatever bands did, so a low-band-only estimate configures something rather
    than nothing -- that value is no longer a mid-frequency figure. Only
    ``volume``, ``rt60_bands`` and the band count are read; the rest of the
    estimate is ignored.

    How *much* to remove is taste rather than measurement, so ``attenuation``,
    ``threshold``, ``over_subtraction`` and ``spectral_floor`` are never
    written; they come back as the float32 image of what was passed in, which
    is the value the dereverb call would have used anyway. A measurement that
    did not converge leaves its own key
    alone, so a partial estimate still configures the half it measured; a
    low-``confidence`` estimate is still applied, because whether to trust it is
    the caller's call.

    Args:
        estimate: The measured room, from :func:`libsonare.estimate_room`. Only
            ``volume`` and ``rt60_bands`` are read.
        threshold, attenuation, n_fft, hop_length, t60_sec, late_delay_ms,
            over_subtraction, spectral_floor, wpe_enabled, wpe_iterations,
            wpe_taps, wpe_strength: The config to point at the room. Every one
            defaults to the library's own dereverb default, matching
            :func:`mastering_repair_dereverb_classical`, so calling this with
            only ``estimate`` returns a config that is ready to run. The C ABI
            underneath reads and writes the whole config and takes every field
            literally -- it has no "zero means default" rule -- which is why
            these defaults are spelled out here instead of left at zero.

    Returns:
        A complete :class:`~libsonare.types.DereverbClassicalConfig` to splat
        into :func:`mastering_repair_dereverb_classical`.
    """
    lib = _get_lib()
    symbol = "sonare_mastering_repair_dereverb_apply_room_estimate"
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    if estimate is None:
        raise SonareValueError(
            "mastering_repair_dereverb_config_for_room: estimate must not be None"
        )
    bands, band_count = _to_c_float_array(estimate.rt60_bands, arg_name="estimate.rt60_bands")
    c_estimate = SonareRoomEstimate(
        volume=float(estimate.volume),
        rt60_bands=ctypes.cast(bands, ctypes.POINTER(ctypes.c_float)),
        band_count=band_count,
    )
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        # Integrality alone: this entry rewrites two fields and takes the rest
        # literally, so it carries none of the dereverb call's own domains.
        n_fft=_narrow_int(
            n_fft, "mastering_repair_dereverb_config_for_room: n_fft", _C_INT_MIN, _C_INT_MAX
        ),
        hop_length=_narrow_int(
            hop_length,
            "mastering_repair_dereverb_config_for_room: hop_length",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=_narrow_int(
            wpe_iterations,
            "mastering_repair_dereverb_config_for_room: wpe_iterations",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_taps=_narrow_int(
            wpe_taps,
            "mastering_repair_dereverb_config_for_room: wpe_taps",
            _C_INT_MIN,
            _C_INT_MAX,
        ),
        wpe_strength=float(wpe_strength),
    )
    _check(
        getattr(lib, symbol)(ctypes.byref(c_estimate), ctypes.byref(config)),
    )
    return DereverbClassicalConfig(
        threshold=float(config.threshold),
        attenuation=float(config.attenuation),
        n_fft=int(config.n_fft),
        hop_length=int(config.hop_length),
        t60_sec=float(config.t60_sec),
        late_delay_ms=float(config.late_delay_ms),
        over_subtraction=float(config.over_subtraction),
        spectral_floor=float(config.spectral_floor),
        wpe_enabled=bool(config.wpe_enabled),
        wpe_iterations=int(config.wpe_iterations),
        wpe_taps=int(config.wpe_taps),
        wpe_strength=float(config.wpe_strength),
    )
