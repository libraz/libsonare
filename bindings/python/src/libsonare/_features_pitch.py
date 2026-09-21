"""Pitch tracking, tuning estimation and note segmentation for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SonareNoteSegmenterConfig,
    SonareNoteSegmentsResult,
    SonarePitchResult,
)
from ._runtime import (
    _check,
    _float_array_result,
    _get_lib,
    _guard_buffer,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_samples,
)
from .types import NoteSegment, PiptrackResult, PitchResult


def pitch_tuning(
    frequencies: Sequence[float] | list[float],
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float:
    """Per-octave tuning offset from detected pitches (librosa.pitch_tuning).

    Args:
        frequencies: Detected pitch frequencies in Hz (non-positive ignored).
            Non-finite entries are rejected, so a pitch track carrying NaN for
            unvoiced frames -- what :func:`pitch_pyin` returns with
            ``fill_na=False`` -- must be filtered or produced with
            ``fill_na=True`` first.
        resolution: Tuning resolution in fractions of a bin (default 0.01).
        bins_per_octave: Pitch bins per octave (default 12).

    Returns:
        Tuning offset in fractions of a bin, in ``[-0.5, 0.5)``. Exactly
        ``-0.5`` is a legitimate result (a pitch half a bin flat); ``+0.5``
        is not attainable, because a residual of ``+0.5`` wraps to ``-0.5``.
    """
    lib = _get_lib()
    # Non-finite entries are dropped like non-positive ones, so an all-NaN track
    # would report 0.0 -- a legitimate "perfectly in tune" answer -- rather than
    # signalling that nothing usable was supplied.
    frequency_buf = _validate_samples("pitch_tuning", frequencies, arg_name="frequencies")
    c_array, length = _to_c_float_array(frequency_buf)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_pitch_tuning(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_float(resolution, "resolution"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_guard_buffer("samples")
def estimate_tuning(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float:
    """Global tuning offset of an audio signal (librosa.estimate_tuning).

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        resolution: Tuning resolution in fractions of a bin (default 0.01).
        bins_per_octave: Pitch bins per octave (default 12).

    Returns:
        Tuning offset in fractions of a bin, in ``[-0.5, 0.5)``. Exactly
        ``-0.5`` is a legitimate result (a pitch half a bin flat); ``+0.5``
        is not attainable, because a residual of ``+0.5`` wraps to ``-0.5``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_estimate_tuning(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(resolution, "resolution"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


@_guard_buffer("samples")
def piptrack(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 150.0,
    fmax: float = 4000.0,
    threshold: float = 0.1,
) -> PiptrackResult:
    """Return per-bin spectral pitch candidates and their peak magnitudes."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    n_bins = ctypes.c_int()
    n_frames = ctypes.c_int()
    pitches = ctypes.POINTER(ctypes.c_float)()
    magnitudes = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_piptrack(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_float(fmax, "fmax"),
        _to_c_float(threshold, "threshold"),
        ctypes.byref(n_bins),
        ctypes.byref(n_frames),
        ctypes.byref(pitches),
        ctypes.byref(magnitudes),
    )
    _check(rc)
    try:
        total = n_bins.value * n_frames.value
        return PiptrackResult(
            n_bins.value,
            n_frames.value,
            _float_array_result(pitches, total),
            _float_array_result(magnitudes, total),
        )
    finally:
        if pitches:
            lib.sonare_free_floats(pitches)
        if magnitudes:
            lib.sonare_free_floats(magnitudes)


def pitch_yin(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.1,
    fill_na: bool = False,
) -> PitchResult:
    """Estimate fundamental frequency using the YIN algorithm.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).
        fmin: Minimum frequency in Hz (default 65.0).
        fmax: Maximum frequency in Hz (default 2093.0).
        threshold: YIN threshold (default 0.1).
        fill_na: Retained for API compatibility. YIN always returns a finite
            estimate for each complete frame, as librosa.yin does; use
            ``voiced_flag`` to distinguish threshold crossings.

    Returns:
        PitchResult with f0, voiced probabilities, and statistics.
    """
    lib = _get_lib()
    sample_buf = _validate_samples("pitch_yin", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonarePitchResult()
    rc = lib.sonare_pitch_yin(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(frame_length, "frame_length"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_float(fmax, "fmax"),
        _to_c_float(threshold, "threshold"),
        ctypes.c_int(1 if fill_na else 0),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.n_frames
        return PitchResult(
            n_frames=n,
            f0=[float(out.f0[i]) for i in range(n)],
            voiced_prob=[float(out.voiced_prob[i]) for i in range(n)],
            voiced_flag=[bool(out.voiced_flag[i]) for i in range(n)],
            median_f0=float(out.median_f0),
            mean_f0=float(out.mean_f0),
        )
    finally:
        lib.sonare_free_pitch_result(ctypes.byref(out))


def pitch_pyin(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    frame_length: int = 2048,
    hop_length: int = 512,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    threshold: float = 0.1,
    fill_na: bool = False,
) -> PitchResult:
    """Estimate fundamental frequency using the pYIN algorithm.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).
        fmin: Minimum frequency in Hz (default 65.0).
        fmax: Maximum frequency in Hz (default 2093.0).
        threshold: YIN threshold (default 0.1).
        fill_na: If True, return 0 for unvoiced f0 frames. If False,
            keep those frames as NaN to match librosa-style pitch tracks.

    Returns:
        PitchResult with f0, voiced probabilities, and statistics.
    """
    lib = _get_lib()
    sample_buf = _validate_samples("pitch_pyin", samples)
    c_array, length = _to_c_float_array(sample_buf)
    out = SonarePitchResult()
    rc = lib.sonare_pitch_pyin(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(frame_length, "frame_length"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(fmin, "fmin"),
        _to_c_float(fmax, "fmax"),
        _to_c_float(threshold, "threshold"),
        ctypes.c_int(1 if fill_na else 0),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.n_frames
        return PitchResult(
            n_frames=n,
            f0=[float(out.f0[i]) for i in range(n)],
            voiced_prob=[float(out.voiced_prob[i]) for i in range(n)],
            voiced_flag=[bool(out.voiced_flag[i]) for i in range(n)],
            median_f0=float(out.median_f0),
            mean_f0=float(out.mean_f0),
        )
    finally:
        lib.sonare_free_pitch_result(ctypes.byref(out))


@_guard_buffer(shape_only=("f0_hz",))
def note_segments(
    f0_hz: Sequence[float] | list[float],
    voiced_prob: Sequence[float] | list[float],
    frame_rate: float,
    *,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteSegment]:
    """Segment a monophonic F0 track into stable note regions.

    Zero-Hz F0 frames and voicing values below ``voiced_threshold`` (default
    0.5) delimit note regions. The F0 and voicing arrays must have equal
    non-zero length.

    Pass :func:`pitch_pyin`'s ``voiced_flag`` as 0.0/1.0 for ``voiced_prob``.
    Do **not** pass its ``voiced_prob``: that value is the frame's voiced
    observation mass and rises with F0 for a fixed ``frame_length``, so a fixed
    threshold silently returns no segments at all for low-register material.
    """
    lib = _get_lib()
    f0_array, f0_count = _to_c_float_array(f0_hz)
    probability_array, probability_count = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
    config = SonareNoteSegmenterConfig(
        2,
        0.0 if segmentation_threshold_cents is None else segmentation_threshold_cents,
        0.0 if min_note_ms is None else min_note_ms,
        0.0 if reference_hz is None else reference_hz,
        0.0 if voiced_threshold is None else voiced_threshold,
    )
    out = SonareNoteSegmentsResult()
    rc = lib.sonare_note_segments(
        f0_array,
        _to_c_size_t(f0_count, "f0_count"),
        probability_array,
        _to_c_size_t(probability_count, "probability_count"),
        _to_c_float(frame_rate, "frame_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return [
            NoteSegment(
                frame_start=int(out.segments[i].frame_start),
                frame_end=int(out.segments[i].frame_end),
                start_seconds=float(out.segments[i].start_seconds),
                end_seconds=float(out.segments[i].end_seconds),
                median_cents=float(out.segments[i].median_cents),
            )
            for i in range(out.count)
        ]
    finally:
        lib.sonare_free_note_segments(ctypes.byref(out))
