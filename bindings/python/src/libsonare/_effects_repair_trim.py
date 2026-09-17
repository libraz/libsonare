"""Silence trimming repair over mono, stereo and detection-only forms."""

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
    SONARE_TRIM_SILENCE_MODE_LUFS_GATED,
    SONARE_TRIM_SILENCE_MODE_PEAK,
    SonareTrimRange,
    SonareTrimSilenceConfig,
    SonareTrimSilenceStereoResult,
)
from ._runtime import (
    _SIZE_T_MAX,
    SonareValueError,
    _check,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _resolve_enum,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
)
from .types import (
    TrimRange,
    TrimReport,
    TrimSilenceStereoResult,
)

_TRIM_SILENCE_MODE_NAMES = {
    "peak": SONARE_TRIM_SILENCE_MODE_PEAK,  # noqa: F405
    "lufsgated": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
    "lufs_gated": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
    "lufs": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
}


def _coerce_trim_silence_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _TRIM_SILENCE_MODE_NAMES,
        "trim_silence mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


@_guard_buffer("samples")
def mastering_repair_trim_silence(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> np.ndarray:
    """Offline silence trimmer (peak threshold or LUFS-gated)."""
    if padding_samples < 0:
        raise SonareValueError("padding_samples must be non-negative")
    config = SonareTrimSilenceConfig(  # noqa: F405
        threshold=float(threshold),
        # Narrowed rather than coerced: int() takes 0.5 as 0, which is "no padding at all".
        padding_samples=_narrow_int(
            padding_samples, "mastering_repair_trim_silence: padding_samples", 0, _SIZE_T_MAX
        ),
        mode=_coerce_trim_silence_mode(mode),
        gate_lufs=float(gate_lufs),
        window_ms=float(window_ms),
    )
    return _run_repair(
        _get_lib().sonare_mastering_repair_trim_silence, samples, sample_rate, config
    )


def _extract_trim_range(raw: Any) -> TrimRange:
    return TrimRange(first=int(raw.first), last_exclusive=int(raw.last_exclusive))


def _extract_trim_report(raw: Any) -> TrimReport:
    return TrimReport(
        range=_extract_trim_range(raw.range),
        removed_head_samples=int(raw.removed_head_samples),
        removed_tail_samples=int(raw.removed_tail_samples),
    )


@_guard_buffer("samples")
def mastering_repair_detect_trim_range(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimRange:
    """Measure the range a trim pass would keep, without trimming.

    The padding ``padding_samples`` asks for is ALREADY INSIDE the returned
    range, so this is the range :func:`mastering_repair_trim_silence` would cut
    to rather than the detected extent of the signal::

        kept = libsonare.mastering_repair_detect_trim_range(samples, 44100)
        head, tail = kept.first, len(samples) - kept.last_exclusive

    A buffer with nothing above the threshold reports ``(length, length)`` --
    an empty range at the end of the buffer, not at its start.

    Which arguments are live depends on ``mode``: ``threshold`` is read only in
    ``"peak"`` mode, and ``gate_lufs`` and ``window_ms`` only in
    ``"lufs_gated"``. That gated mode compares an UNWEIGHTED RMS over a window
    centred on each sample, so the figure it gates on is dBFS rather than a
    BS.1770 loudness, and ``window_ms`` sizes that window and does nothing else.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Peak threshold, ``"peak"`` mode only (default 0.001).
        padding_samples: Samples to retain either side of the kept range,
            clamped to the buffer (default 0). A pass that kept nothing is not
            padded.
        mode: ``"peak"`` or ``"lufs_gated"`` (default ``"peak"``).
        gate_lufs: Gate in dBFS, ``"lufs_gated"`` mode only (default -60.0).
        window_ms: Analysis window, ``"lufs_gated"`` mode only (default 400.0).

    Returns:
        :class:`~libsonare.types.TrimRange`, half-open and in input-buffer
        coordinates.

    Raises:
        SonareValueError: If ``padding_samples`` is negative, if ``mode`` cannot
            be resolved, or if the buffer is empty or carries a non-finite
            sample.
        SonareError: If the C call rejects the request.
    """
    # Refused here rather than at the C validator: `padding_samples` is a
    # size_t, so a negative count arrives as a value near SIZE_MAX and the core
    # rejects it as out of range -- a true statement about the wrapped value
    # that says nothing about what the caller passed. Matches the trim entries.
    if padding_samples < 0:
        raise SonareValueError("padding_samples must be non-negative")
    config = SonareTrimSilenceConfig(  # noqa: F405
        threshold=float(threshold),
        padding_samples=_narrow_int(
            padding_samples, "mastering_repair_detect_trim_range: padding_samples", 0, _SIZE_T_MAX
        ),
        mode=_coerce_trim_silence_mode(mode),
        gate_lufs=float(gate_lufs),
        window_ms=float(window_ms),
    )
    return _extract_trim_range(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_trim_range,
            samples,
            sample_rate,
            config,
            SonareTrimRange,  # noqa: F405
        )
    )


@_guard_buffer("left", "right")
def mastering_repair_detect_trim_range_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimRange:
    """Measure the one range a stereo trim pass would cut both channels to.

    Each channel is scanned on its own and the two ranges are UNIONED, so the
    pair keeps whatever either channel calls signal -- the range
    :func:`mastering_repair_trim_silence_stereo` would apply, without the
    trimmed audio::

        kept = libsonare.mastering_repair_detect_trim_range_stereo(left, right, 44100)

    A channel with nothing above the threshold contributes NO EDGE at all rather
    than an edge at the buffer's end, so one silent channel does not widen the
    range: the union of a silent channel and an active one is the active
    channel's range exactly. No downmix is read, because summing to mono halves
    material carried by one channel alone and cancels an antiphase pair outright,
    either of which would read full-level audio as silence.

    Unlike :func:`mastering_repair_trim_silence_stereo` this hands back the
    union alone; the per-channel scans it was formed from come back only from
    the trimming call.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Peak threshold, ``"peak"`` mode only (default 0.001).
        padding_samples: Samples to retain either side of the kept range,
            clamped to the buffer (default 0).
        mode: ``"peak"`` or ``"lufs_gated"`` (default ``"peak"``).
        gate_lufs: Gate in dBFS, ``"lufs_gated"`` mode only (default -60.0).
        window_ms: Analysis window, ``"lufs_gated"`` mode only (default 400.0).

    Returns:
        :class:`~libsonare.types.TrimRange`, the union of the two channels'
        scans.

    Raises:
        SonareValueError: If ``padding_samples`` is negative, if ``mode`` cannot
            be resolved, if the two channels differ in length, or if either
            buffer is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    if padding_samples < 0:
        raise SonareValueError("padding_samples must be non-negative")

    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareTrimSilenceConfig(  # noqa: F405
        threshold=float(threshold),
        padding_samples=_narrow_int(
            padding_samples,
            "mastering_repair_detect_trim_range_stereo: padding_samples",
            0,
            _SIZE_T_MAX,
        ),
        mode=_coerce_trim_silence_mode(mode),
        gate_lufs=float(gate_lufs),
        window_ms=float(window_ms),
    )
    out = SonareTrimRange()  # noqa: F405
    rc = lib.sonare_mastering_repair_detect_trim_range_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    return _extract_trim_range(out)


@_guard_buffer("left", "right")
def mastering_repair_trim_silence_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> TrimSilenceStereoResult:
    """Trims a stereo pair to one shared range.

    Each channel is scanned on its own and the two ranges are UNIONED, so the
    pair keeps whatever either channel calls signal and both outputs come back
    the same length. Cutting each channel by its own range would not keep them
    aligned, which is why the contract forbids it. No downmix is read: summing
    to mono halves material carried by one channel alone and cancels an
    antiphase pair outright, either of which would read audio as silence.

    Two things here differ from every other stereo repair on this surface.
    ``result.length`` is the OUTPUT length, not an echo of the input -- trimming
    shortens the pair. And the result can be EMPTY: when neither channel carries
    signal, ``left`` and ``right`` come back as empty lists with ``length`` 0 and
    nothing is raised, because a pair of silent channels is a measurement rather
    than a refusal.

    ``report.range`` is the union that was applied to both channels;
    ``left_range`` and ``right_range`` are the per-channel scans it was formed
    from, so a caller can see which channel decided each edge. A channel
    carrying nothing reports an empty range and contributes nothing to the
    union::

        result = libsonare.mastering_repair_trim_silence_stereo(left, right, 44100)
        onset = result.left_range.first, result.right_range.first

    Which config fields are live depends on ``mode``: ``threshold`` is read only
    in ``"peak"`` mode, and ``gate_lufs`` and ``window_ms`` only in
    ``"lufs_gated"``. That gated mode compares an UNWEIGHTED RMS over a window
    centred on each sample, so the figure it gates on is dBFS rather than a
    BS.1770 loudness, and ``window_ms`` sizes that window and does nothing else.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Peak threshold, ``"peak"`` mode only (default 0.001).
        padding_samples: Samples to retain either side of the kept range,
            clamped to the buffer so it can never reach past an end (default 0).
            A pass that kept nothing is not padded.
        mode: ``"peak"`` or ``"lufs_gated"`` (default ``"peak"``).
        gate_lufs: Gate in dBFS, ``"lufs_gated"`` mode only (default -60.0).
        window_ms: Analysis window, ``"lufs_gated"`` mode only (default 400.0).

    Returns:
        :class:`TrimSilenceStereoResult` with the trimmed channels, the union
        range that was applied, and the two per-channel scans behind it.

    Raises:
        SonareValueError: If ``padding_samples`` is negative or the two channels
            differ in length.
        SonareError: If the C call rejects the request.
    """
    # Refused here rather than at the C validator: `padding_samples` is a
    # size_t, so a negative count arrives as a value near SIZE_MAX and the core
    # rejects it as out of range -- a true statement about the wrapped value
    # that says nothing about what the caller passed. Matches the mono entry.
    if padding_samples < 0:
        raise SonareValueError("padding_samples must be non-negative")

    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareTrimSilenceConfig(  # noqa: F405
        threshold=float(threshold),
        padding_samples=_narrow_int(
            padding_samples,
            "mastering_repair_trim_silence_stereo: padding_samples",
            0,
            _SIZE_T_MAX,
        ),
        mode=_coerce_trim_silence_mode(mode),
        gate_lufs=float(gate_lufs),
        window_ms=float(window_ms),
    )
    out = SonareTrimSilenceStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_trim_silence_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        # An all-silent pair comes back as (NULL, NULL, 0) and SONARE_OK, so the
        # length is read before either pointer -- sizing from the input length
        # instead would index a NULL pointer and raise on a documented success.
        n = int(out.length)
        return TrimSilenceStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            report=_extract_trim_report(out.report),
            left_range=_extract_trim_range(out.left_range),
            right_range=_extract_trim_range(out.right_range),
        )
    finally:
        # No dedicated free function: `left`/`right` are each released with
        # sonare_free_floats (see SonareTrimSilenceStereoResult in
        # sonare_c_mastering.h). A refused call and an empty result both leave
        # the pointers NULL, which the guards below skip.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)
