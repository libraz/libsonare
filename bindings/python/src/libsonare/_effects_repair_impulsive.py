"""Impulsive-artifact repair: declick, declip and decrackle."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._effects_repair_common import (
    _run_detection,
    _run_repair,
)
from ._ffi import (
    SONARE_DECRACKLE_MODE_MEDIAN,
    SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,
    SonareClickDetection,
    SonareClipDetection,
    SonareCrackleDetection,
    SonareDeclickConfig,
    SonareDeclickStereoResult,
    SonareDeclipConfig,
    SonareDeclipStereoResult,
    SonareDecrackleConfig,
    SonareDecrackleStereoResult,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _as_float32_buffer,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _resolve_enum,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
)
from .types import (
    ClickDetection,
    ClipDetection,
    CrackleDetection,
    DeclickReport,
    DeclickStereoResult,
    DeclipReport,
    DeclipStereoResult,
    DecrackleReport,
    DecrackleStereoResult,
)

_DECRACKLE_MODE_NAMES = {
    "median": SONARE_DECRACKLE_MODE_MEDIAN,  # noqa: F405
    "waveletshrinkage": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
    "wavelet_shrinkage": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
    "wavelet": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
}


def _coerce_decrackle_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DECRACKLE_MODE_NAMES,
        "decrackle mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


@_guard_buffer("samples")
def mastering_repair_declick(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> np.ndarray:
    """Offline LPC-based declicker.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Amplitude threshold vs LPC prediction (default 0.8).
        neighbor_ratio: Ratio vs neighbour amplitude (default 4.0).
        max_click_samples: Maximum click run length in samples (default 8).
        lpc_order: LPC order used for prediction (default 20).
        residual_ratio: Residual / signal threshold (default 8.0).

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.
    """
    # Narrowed ahead of the sign check: int() takes 8.7 as 8, a run length nobody asked for.
    max_click_value = _narrow_int(
        max_click_samples, "mastering_repair_declick: max_click_samples", _C_INT_MIN, _C_INT_MAX
    )
    lpc_order_value = _narrow_int(
        lpc_order, "mastering_repair_declick: lpc_order", _C_INT_MIN, _C_INT_MAX
    )
    if max_click_value <= 0:
        raise SonareValueError("max_click_samples must be positive")
    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    config = SonareDeclickConfig(  # noqa: F405
        threshold=float(threshold),
        neighbor_ratio=float(neighbor_ratio),
        max_click_samples=max_click_value,
        lpc_order=lpc_order_value,
        residual_ratio=float(residual_ratio),
    )
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_mastering_repair_declick(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


def _extract_click_detection(raw: Any) -> ClickDetection:
    return ClickDetection(
        count=int(raw.count),
        rejected=int(raw.rejected),
        longest_run_samples=int(raw.longest_run_samples),
        per_second=float(raw.per_second),
    )


@_guard_buffer("samples")
def mastering_repair_detect_clicks(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> ClickDetection:
    """Measure clicks without repairing them.

    Runs the same LPC analysis :func:`mastering_repair_declick` runs, so a run
    counted here is one the repair would act on; a cheaper threshold-only scan
    would report runs it leaves alone. The arguments are the declicker's own, so
    a measurement describes the repair that would follow it only when the two
    calls are configured alike::

        detected = libsonare.mastering_repair_detect_clicks(samples, 44100)
        if detected.per_second > 1.0:
            samples = libsonare.mastering_repair_declick(samples, 44100)

    ``count`` counts RUNS meeting the repair criteria, not samples, and
    ``rejected`` counts runs the criteria excluded as outliers -- a large
    ``rejected`` says the configured run length or neighbour ratio is too tight
    for this material, not that the material is clean.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Amplitude threshold vs LPC prediction (default 0.8).
        neighbor_ratio: Ratio vs neighbour amplitude (default 4.0).
        max_click_samples: Maximum click run length in samples (default 8).
        lpc_order: LPC order used for prediction (default 20).
        residual_ratio: Residual / signal threshold (default 8.0).

    Returns:
        :class:`~libsonare.types.ClickDetection`.

    Raises:
        SonareValueError: If ``max_click_samples`` is not positive, or if the
            buffer is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    max_click_value = _narrow_int(
        max_click_samples,
        "mastering_repair_detect_clicks: max_click_samples",
        _C_INT_MIN,
        _C_INT_MAX,
    )
    lpc_order_value = _narrow_int(
        lpc_order, "mastering_repair_detect_clicks: lpc_order", _C_INT_MIN, _C_INT_MAX
    )
    if max_click_value <= 0:
        raise SonareValueError("max_click_samples must be positive")
    config = SonareDeclickConfig(  # noqa: F405
        threshold=float(threshold),
        neighbor_ratio=float(neighbor_ratio),
        max_click_samples=max_click_value,
        lpc_order=lpc_order_value,
        residual_ratio=float(residual_ratio),
    )
    return _extract_click_detection(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_clicks,
            samples,
            sample_rate,
            config,
            SonareClickDetection,  # noqa: F405
        )
    )


def _extract_declick_report(raw: Any) -> DeclickReport:
    return DeclickReport(
        detected=_extract_click_detection(raw.detected),
        repaired_runs=int(raw.repaired_runs),
        repaired_samples=int(raw.repaired_samples),
        linked_runs=int(raw.linked_runs),
        lpc_model_used=bool(raw.lpc_model_used),
    )


@_guard_buffer("left", "right")
def mastering_repair_declick_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> DeclickStereoResult:
    """Declicks a stereo pair, repairing the union of both channels' runs.

    A common-mode click repaired on one side only would move the stereo
    image, so a run either channel's detector selects is repaired in both.
    Only the selection is shared: each channel's fill comes from its own
    samples and its own LPC model, which is why the two reports can differ.
    Merged runs can leave a repaired region longer than ``max_click_samples``
    -- that cap governs what may be selected, not how far a selection reaches
    once both channels agree.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Amplitude threshold vs LPC prediction (default 0.8).
        neighbor_ratio: Ratio vs neighbour amplitude (default 4.0).
        max_click_samples: Maximum click run length in samples (default 8).
        lpc_order: LPC order used for prediction (default 20).
        residual_ratio: Residual / signal threshold (default 8.0).

    Returns:
        :class:`DeclickStereoResult` with the declicked channels and each
        channel's own detection/repair report.
    """
    max_click_value = _narrow_int(
        max_click_samples,
        "mastering_repair_declick_stereo: max_click_samples",
        _C_INT_MIN,
        _C_INT_MAX,
    )
    lpc_order_value = _narrow_int(
        lpc_order, "mastering_repair_declick_stereo: lpc_order", _C_INT_MIN, _C_INT_MAX
    )
    if max_click_value <= 0:
        raise SonareValueError("max_click_samples must be positive")
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDeclickConfig(  # noqa: F405
        threshold=float(threshold),
        neighbor_ratio=float(neighbor_ratio),
        max_click_samples=max_click_value,
        lpc_order=lpc_order_value,
        residual_ratio=float(residual_ratio),
    )
    out = SonareDeclickStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_declick_stereo(
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
        return DeclickStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_declick_report(out.left_report),
            right_report=_extract_declick_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDeclickStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


@_guard_buffer("samples")
def mastering_repair_declip(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> np.ndarray:
    """Offline LPC-based declipper.

    Only clipped runs of at most 512 consecutive samples are reconstructed with the
    LPC solver. The cap is a fixed sample count: it is not derived from ``lpc_order``,
    from ``sample_rate``, or from any other argument, so its duration depends on the
    rate (~10.7 ms at 48 kHz). A longer run is filled with cubic / linear interpolation
    instead, which keeps the solver's dense matrices bounded by the cap rather than by
    the input. Exceeding the cap silently changes the reconstruction method rather than
    raising: ``lpc_order``, ``iterations`` and ``lpc_blend`` have no effect on the
    interpolated run.
    """
    # Narrowed rather than coerced: int() takes 2.7 as 2, an iteration count nobody asked for.
    config = SonareDeclipConfig(  # noqa: F405
        clip_threshold=float(clip_threshold),
        lpc_order=_narrow_int(
            lpc_order, "mastering_repair_declip: lpc_order", _C_INT_MIN, _C_INT_MAX
        ),
        iterations=_narrow_int(
            iterations, "mastering_repair_declip: iterations", _C_INT_MIN, _C_INT_MAX
        ),
        lpc_blend=float(lpc_blend),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_declip, samples, sample_rate, config)


def _extract_clip_detection(raw: Any) -> ClipDetection:
    return ClipDetection(
        sample_count=int(raw.sample_count),
        sample_fraction=float(raw.sample_fraction),
        run_count=int(raw.run_count),
        longest_run_samples=int(raw.longest_run_samples),
        flat_run_count=int(raw.flat_run_count),
        longest_flat_run_samples=int(raw.longest_flat_run_samples),
        flat_sample_count=int(raw.flat_sample_count),
        flat_level=float(raw.flat_level),
    )


@_guard_buffer("samples")
def mastering_repair_detect_clipping(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> ClipDetection:
    """Measure clipping without repairing it.

    Counts samples at or past ``clip_threshold``; NO other argument reaches the
    result. ``lpc_order``, ``iterations`` and ``lpc_blend`` are accepted so one
    set of arguments configures both this call and
    :func:`mastering_repair_declip`, and ``sample_rate`` is validated without
    being read, since no field here is a rate::

        detected = libsonare.mastering_repair_detect_clipping(samples, 44100)
        if detected.sample_fraction > 0.0:
            samples = libsonare.mastering_repair_declip(samples, 44100)

    ``longest_run_samples`` past 512 is the one field that predicts the repair's
    method rather than its extent: a longer run takes the interpolation fallback
    instead of the LPC solver, for which the three solver arguments do nothing.

    The flat-top fields (``flat_run_count``, ``longest_flat_run_samples``,
    ``flat_sample_count``, ``flat_level``) answer a different question: they
    count runs of at least three consecutive bit-identical samples within 1 dB
    of the signal's peak, wherever that peak sits, rather than samples at or
    past ``clip_threshold``. That lets them fire on material clipped in one
    tool and attenuated in another, which leaves nothing at the threshold and
    so reads as clean on the four fields above. They do NOT read
    ``clip_threshold`` at all, and a genuinely flat-topped waveform -- a
    square or pulse train, or a fully limited master -- counts as clipped
    here too and cannot be told apart from it in the time domain.

    The reverse also holds, and matters more: anything that moves samples
    independently erases a real flat top, so a zero here is not proof the
    material was never clipped. Resampling, lossy coding, and a stereo
    downmix all do this -- a downmix in particular, since the two channels
    are rarely bit-identical, so averaging them moves each sample by a
    different amount and a plateau stops being exactly level. Detect each
    channel before mixing them down, not after.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        clip_threshold: Amplitude at or above which a sample counts as clipped
            (default 0.98).
        lpc_order: LPC order the repair would use (default 36); not read here.
        iterations: Reconstruction iterations the repair would use (default 2);
            not read here.
        lpc_blend: LPC vs interpolation blend the repair would use (default
            0.65); not read here.

    Returns:
        :class:`~libsonare.types.ClipDetection`.

    Raises:
        SonareValueError: If the buffer is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    config = SonareDeclipConfig(  # noqa: F405
        clip_threshold=float(clip_threshold),
        lpc_order=_narrow_int(
            lpc_order, "mastering_repair_detect_clipping: lpc_order", _C_INT_MIN, _C_INT_MAX
        ),
        iterations=_narrow_int(
            iterations, "mastering_repair_detect_clipping: iterations", _C_INT_MIN, _C_INT_MAX
        ),
        lpc_blend=float(lpc_blend),
    )
    return _extract_clip_detection(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_clipping,
            samples,
            sample_rate,
            config,
            SonareClipDetection,  # noqa: F405
        )
    )


def _extract_declip_report(raw: Any) -> DeclipReport:
    return DeclipReport(
        detected=_extract_clip_detection(raw.detected),
        lpc_reconstructed_runs=int(raw.lpc_reconstructed_runs),
        interpolated_runs=int(raw.interpolated_runs),
        repaired_samples=int(raw.repaired_samples),
        linked_runs=int(raw.linked_runs),
    )


@_guard_buffer("left", "right")
def mastering_repair_declip_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> DeclipStereoResult:
    """Declips a stereo pair over the union of both channels' clipped runs.

    Each channel reconstructs the whole of every union run it has at least
    one clipped sample in; a channel with none in a run is left untouched
    there -- reconstructing unclipped audio to match the other side would
    replace real samples with an estimate, so this is not
    :func:`mastering_repair_declick_stereo`'s behaviour. A clipped plateau
    present in only one channel therefore produces no linking at all: linking
    needs both channels clipped in the same region with different extents.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        clip_threshold: Amplitude above which a sample is considered clipped
            (default 0.98).
        lpc_order: LPC order used for prediction (default 36).
        iterations: LPC reconstruction iterations (default 2).
        lpc_blend: LPC vs interpolation blend (default 0.65).

    Returns:
        :class:`DeclipStereoResult` with the declipped channels and each
        channel's own detection/repair report.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDeclipConfig(  # noqa: F405
        clip_threshold=float(clip_threshold),
        lpc_order=_narrow_int(
            lpc_order, "mastering_repair_declip_stereo: lpc_order", _C_INT_MIN, _C_INT_MAX
        ),
        iterations=_narrow_int(
            iterations, "mastering_repair_declip_stereo: iterations", _C_INT_MIN, _C_INT_MAX
        ),
        lpc_blend=float(lpc_blend),
    )
    out = SonareDeclipStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_declip_stereo(
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
        return DeclipStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_declip_report(out.left_report),
            right_report=_extract_declip_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDeclipStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


@_guard_buffer("samples")
def mastering_repair_decrackle(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> np.ndarray:
    """Offline crackle suppressor (median or wavelet-shrinkage)."""
    config = SonareDecrackleConfig(  # noqa: F405
        threshold=float(threshold),
        mode=_coerce_decrackle_mode(mode),
        # Narrowed rather than coerced: int() takes 4.7 as 4, a depth nobody asked for.
        levels=_narrow_int(levels, "mastering_repair_decrackle: levels", _C_INT_MIN, _C_INT_MAX),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_decrackle, samples, sample_rate, config)


def _extract_crackle_detection(raw: Any) -> CrackleDetection:
    return CrackleDetection(
        sample_count=int(raw.sample_count),
        sample_fraction=float(raw.sample_fraction),
        per_second=float(raw.per_second),
    )


@_guard_buffer("samples")
def mastering_repair_detect_crackle(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> CrackleDetection:
    """Measure crackle without repairing it.

    Measured by the MEDIAN criterion whatever ``mode`` says: wavelet shrinkage
    removes crackle without ever deciding a sample is crackle, so these counts
    describe what median mode would replace and not what wavelet mode would do::

        detected = libsonare.mastering_repair_detect_crackle(samples, 44100)
        if detected.per_second > 5.0:
            samples = libsonare.mastering_repair_decrackle(samples, 44100)

    ``mode`` and ``levels`` are accepted so one set of arguments configures both
    this call and :func:`mastering_repair_decrackle`; neither reaches the result.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Deviation from the local median above which a sample counts
            as crackle (default 0.4).
        mode: ``"median"`` (default) or ``"waveletShrinkage"``; accepted for
            symmetry with the repair and not read here.
        levels: Wavelet decomposition levels the repair would use (default 4);
            not read here.

    Returns:
        :class:`~libsonare.types.CrackleDetection`.

    Raises:
        SonareValueError: If ``mode`` cannot be resolved, or if the buffer is
            empty or carries a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    config = SonareDecrackleConfig(  # noqa: F405
        threshold=float(threshold),
        mode=_coerce_decrackle_mode(mode),
        levels=_narrow_int(
            levels, "mastering_repair_detect_crackle: levels", _C_INT_MIN, _C_INT_MAX
        ),
    )
    return _extract_crackle_detection(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_crackle,
            samples,
            sample_rate,
            config,
            SonareCrackleDetection,  # noqa: F405
        )
    )


def _extract_decrackle_report(raw: Any) -> DecrackleReport:
    return DecrackleReport(
        detected=_extract_crackle_detection(raw.detected),
        replaced_samples=int(raw.replaced_samples),
        detail_coefficients=int(raw.detail_coefficients),
        shrunk_coefficients=int(raw.shrunk_coefficients),
        noise_sigma=float(raw.noise_sigma),
    )


@_guard_buffer("left", "right")
def mastering_repair_decrackle_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> DecrackleStereoResult:
    """Decrackles a stereo pair, each channel on its own.

    Crackle is surface damage landing at different instants in each channel,
    so unlike :func:`mastering_repair_declick_stereo` and
    :func:`mastering_repair_declip_stereo` there is no shared run for the two
    channels to agree about: this is two independent mono passes over a
    shared, validated config, and each channel's report is its own.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Median mode: deviation threshold. Wavelet mode: a cap on
            the BayesShrink threshold, which only binds when the configured
            value is below what BayesShrink computes from the signal itself
            (default 0.4).
        mode: ``"median"`` (default) or ``"waveletShrinkage"``; an integer in
            ``SONARE_DECRACKLE_MODE_*`` is also accepted.
        levels: Wavelet mode: number of Haar decomposition levels (default 4).

    Returns:
        :class:`DecrackleStereoResult` with the decrackled channels and each
        channel's own detection/repair report.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDecrackleConfig(  # noqa: F405
        threshold=float(threshold),
        mode=_coerce_decrackle_mode(mode),
        levels=_narrow_int(
            levels, "mastering_repair_decrackle_stereo: levels", _C_INT_MIN, _C_INT_MAX
        ),
    )
    out = SonareDecrackleStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_decrackle_stereo(
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
        return DecrackleStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_decrackle_report(out.left_report),
            right_report=_extract_decrackle_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDecrackleStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)
