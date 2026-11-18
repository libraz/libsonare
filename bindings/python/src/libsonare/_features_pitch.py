"""Pitch tracking, tuning estimation and note segmentation for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._ffi import (
    SonareNoteSegmenterConfig,
    SonareNoteSegmentsResult,
    SonarePitchResult,
)
from ._runtime import (
    SonareValueError,
    _check,
    _from_c_float_array,
    _from_c_float_matrix,
    _get_lib,
    _guard_buffer,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
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
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    resolution: float = 0.01,
    bins_per_octave: int = 12,
) -> float:
    """Global tuning offset of an audio signal (librosa.estimate_tuning).

    Args:
        samples: Audio samples.
        sr: Sample rate in Hz (default 22050).
        n_fft: FFT window size (default 2048).
        hop_length: Hop length in samples (default 512).
        resolution: Tuning resolution in fractions of a bin (default 0.01).
        bins_per_octave: Pitch bins per octave (default 12).

    Returns:
        Tuning offset in fractions of a bin of ``bins_per_octave``, in
        ``[-0.5, 0.5)``. Exactly ``-0.5`` is a legitimate result (a pitch half a
        bin flat); ``+0.5`` is not attainable, because a residual of ``+0.5``
        wraps to ``-0.5``.

    Note:
        A librosa mirror: the offset is in fractions of a bin, not cents
        (at 12 bins per octave multiply by 100 for cents). :func:`analyze` and
        :func:`detect_chords` measure the tuning in cents themselves with
        ``tuning_cents="auto"``, and :func:`tuning_to_reference_hz` converts
        cents to Hz.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.c_float(0.0)
    rc = lib.sonare_estimate_tuning(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_float(resolution, "resolution"),
        _to_c_int(bins_per_octave, "bins_per_octave"),
        ctypes.byref(out),
    )
    _check(rc)
    return float(out.value)


def tuning_to_reference_hz(tuning_cents: float, a4_hz: float = 440.0) -> float:
    """Reference frequency of an A4 raised by ``tuning_cents`` cents.

    The converter from the analysis unit (``tuning_cents``, in ``[-50, 50)`` for
    the analysis options) to the Hz a pitch reference is stated in, such as
    ``reference_hz``: ``a4_hz * 2 ** (tuning_cents / 1200)``.

    Args:
        tuning_cents: Finite offset in cents.
        a4_hz: Finite positive concert pitch the tuning is measured from (default 440).

    Returns:
        The reference frequency in Hz.

    Raises:
        SonareValueError: For a non-finite ``tuning_cents`` or a non-positive ``a4_hz``.
    """
    out = ctypes.c_float(0.0)
    _check(
        _get_lib().sonare_tuning_to_reference_hz(
            _to_c_float(tuning_cents, "tuning_cents"),
            _to_c_float(a4_hz, "a4_hz"),
            ctypes.byref(out),
        )
    )
    return float(out.value)


def reference_hz_to_tuning(reference_hz: float, a4_hz: float = 440.0) -> float:
    """Tuning, in cents, of a recording whose A4 sits at ``reference_hz``.

    Inverse of :func:`tuning_to_reference_hz`: ``1200 * log2(reference_hz / a4_hz)``.

    Args:
        reference_hz: Finite positive reference frequency.
        a4_hz: Finite positive concert pitch (default 440).

    Returns:
        The tuning in cents.

    Raises:
        SonareValueError: For a non-positive ``reference_hz`` or ``a4_hz``.
    """
    out = ctypes.c_float(0.0)
    _check(
        _get_lib().sonare_reference_hz_to_tuning(
            _to_c_float(reference_hz, "reference_hz"),
            _to_c_float(a4_hz, "a4_hz"),
            ctypes.byref(out),
        )
    )
    return float(out.value)


@_guard_buffer("samples")
def piptrack(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    fmin: float = 150.0,
    fmax: float = 4000.0,
    threshold: float = 0.1,
) -> PiptrackResult:
    """Return per-bin spectral pitch candidates and their peak magnitudes.

    Each field is a float32 array of shape ``(n_bins, n_frames)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    n_bins = ctypes.c_int()
    n_frames = ctypes.c_int()
    pitches = ctypes.POINTER(ctypes.c_float)()
    magnitudes = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_piptrack(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sr, "sr"),
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
        return PiptrackResult(
            pitches=_from_c_float_matrix(pitches, n_bins.value, n_frames.value),
            magnitudes=_from_c_float_matrix(magnitudes, n_bins.value, n_frames.value),
        )
    finally:
        if pitches:
            lib.sonare_free_floats(pitches)
        if magnitudes:
            lib.sonare_free_floats(magnitudes)


def _pitch_result(out: SonarePitchResult) -> PitchResult:
    n = out.n_frames
    flags = np.array([out.voiced_flag[i] for i in range(n)], dtype=np.bool_)
    return PitchResult(
        f0=_from_c_float_array(out.f0, n),
        voiced_prob=_from_c_float_array(out.voiced_prob, n),
        voiced_flag=flags,
        median_f0=float(out.median_f0),
        mean_f0=float(out.mean_f0),
    )


def pitch_yin(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    frame_length: int = 2048,
    hop_length: int = 512,
    f0_min_hz: float = 65.0,
    f0_max_hz: float = 2093.0,
    threshold: float = 0.1,
) -> PitchResult:
    """Estimate fundamental frequency using the YIN algorithm.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).
        f0_min_hz: Lowest F0 searched, in Hz (default 65.0).
        f0_max_hz: Highest F0 searched, in Hz (default 2093.0).
        threshold: YIN threshold (default 0.1).

    Returns:
        PitchResult with f0, voiced probabilities, and statistics. Every
        complete frame carries a finite f0 estimate, as librosa.yin does;
        ``voiced_flag`` tells the threshold crossings apart.
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
        _to_c_float(f0_min_hz, "f0_min_hz"),
        _to_c_float(f0_max_hz, "f0_max_hz"),
        _to_c_float(threshold, "threshold"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _pitch_result(out)
    finally:
        lib.sonare_free_pitch_result(ctypes.byref(out))


def pitch_pyin(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    frame_length: int = 2048,
    hop_length: int = 512,
    f0_min_hz: float = 65.0,
    f0_max_hz: float = 2093.0,
    threshold: float = 0.1,
    fill_na: bool = False,
) -> PitchResult:
    """Estimate fundamental frequency using the pYIN algorithm.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        frame_length: Frame length in samples (default 2048).
        hop_length: Hop length in samples (default 512).
        f0_min_hz: Lowest F0 searched, in Hz (default 65.0).
        f0_max_hz: Highest F0 searched, in Hz (default 2093.0).
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
        _to_c_float(f0_min_hz, "f0_min_hz"),
        _to_c_float(f0_max_hz, "f0_max_hz"),
        _to_c_float(threshold, "threshold"),
        ctypes.c_int(1 if fill_na else 0),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _pitch_result(out)
    finally:
        lib.sonare_free_pitch_result(ctypes.byref(out))


@_guard_buffer(shape_only=("f0_hz",))
def note_segments(
    f0_hz: Sequence[float] | list[float],
    frame_rate: float,
    *,
    voiced: Sequence[int] | list[int] | None = None,
    voiced_prob: Sequence[float] | list[float] | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteSegment]:
    """Segment a monophonic F0 track into stable note regions.

    Zero-Hz F0 frames and unvoiced frames delimit note regions. Voicing comes
    from ``voiced`` (per-frame voiced flags, non-zero = voiced, as
    :func:`pitch_pyin` reports ``voiced_flag``) or ``voiced_prob`` (a per-frame
    probability in ``[0, 1]``, thresholded at ``voiced_threshold``, default
    0.5). ``voiced_prob`` is used only to derive voicing when ``voiced`` is
    ``None``. The F0 and voicing arrays must have equal non-zero length.

    Do **not** pass :func:`pitch_pyin`'s ``voiced_prob``: that value is the
    frame's voiced observation mass and rises with F0 for a fixed
    ``frame_length``, so a fixed threshold silently returns no segments at all
    for low-register material. Pass its ``voiced_flag`` as ``voiced``.

    Raises:
        TypeError: If neither ``voiced`` nor ``voiced_prob`` is given.
        SonareValueError: If a voicing array's length differs from ``f0_hz``.
    """
    if voiced is None and voiced_prob is None:
        raise TypeError("note_segments() requires voiced or voiced_prob")
    lib = _get_lib()
    f0_array, f0_count = _to_c_float_array(f0_hz)
    probability_array = None
    flags_array = None
    if voiced is not None:
        flags_array, voiced_count = _to_c_int_array(voiced, "voiced")
    else:
        probability_array, voiced_count = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
    if voiced_count != f0_count:
        raise SonareValueError(
            f"note_segments: f0_hz and the voicing array must have equal length, "
            f"got {f0_count} and {voiced_count}"
        )
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
        probability_array,
        flags_array,
        _to_c_size_t(f0_count, "n_frames"),
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
                start_sec=float(out.segments[i].start_sec),
                end_sec=float(out.segments[i].end_sec),
                median_cents=float(out.segments[i].median_cents),
            )
            for i in range(out.count)
        ]
    finally:
        lib.sonare_free_note_segments_result(ctypes.byref(out))
