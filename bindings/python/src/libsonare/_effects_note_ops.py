"""Note-level editing: extraction, rendering and per-note operations."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

from ._effects_note_model import (
    _NOTE_STRUCT_VERSION,
    NoteObject,
    PitchDecomposition,
    _note_extractor_config,
    _note_set_edit,
    _note_set_index,
    _note_voicing_arrays,
    _notes_from_c,
    _notes_to_c,
)
from ._ffi import (
    SonareNoteObjectsResult,
    SonareNoteRenderConfig,
    SonarePitchDecompositionResult,
)
from ._runtime import (
    SonareValueError,
    _call_float_transform,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int32,
    _to_c_size_t,
    _unsupported_effect_symbol,
    _validate_samples,
)


@_guard_buffer("samples")
def note_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    onset_sample: int = 0,
    offset_sample: int | None = None,
    stretch_ratio: float = 1.0,
) -> list[float]:
    """Time-stretch a single note region without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        onset_sample: Start sample index of the note region.
        offset_sample: End sample index of the note region (defaults to the input length).
        stretch_ratio: Stretch factor for the region (>1 lengthens).

    Returns:
        List of samples with the note region stretched.
    """
    resolved_offset = len(samples) if offset_sample is None else offset_sample
    return _call_float_transform(
        "sonare_note_stretch",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(onset_sample, "onset_sample"),
        _to_c_int(resolved_offset, "resolved_offset"),
        _to_c_float(stretch_ratio, "stretch_ratio"),
    )


@_guard_buffer("samples")
def note_move(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    onset_sample: int = 0,
    offset_sample: int | None = None,
    target_onset_sample: int = 0,
) -> list[float]:
    """Move a note region to a new onset without changing its duration."""
    resolved_offset = len(samples) if offset_sample is None else offset_sample
    return _call_float_transform(
        "sonare_note_move",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(onset_sample, "onset_sample"),
        _to_c_int(resolved_offset, "resolved_offset"),
        _to_c_int(target_onset_sample, "target_onset_sample"),
    )


@_guard_buffer("samples", shape_only=("f0_hz",))
def extract_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    *,
    voiced: Sequence[int] | list[int] | np.ndarray | None = None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]:
    """Extract editable note objects from audio and a caller-supplied F0 track.

    Spans come from the same segmenter :func:`note_segments` uses, so the two
    agree on where the notes are; each note here additionally carries its median
    pitch, two measured quality figures, its own slice of the amplitude curve,
    and an identity :class:`NoteEdit` ready to be filled in. Feed the result
    back to :func:`render_notes` to hear the edits.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        f0_hz: Per-frame F0 in Hz, one entry per analysis frame. Every value
            must be finite and non-negative; zero denotes an unvoiced frame.
            :func:`pitch_pyin` emits NaN for an unvoiced frame unless it is
            called with ``fill_na=True``, which returns the zero this expects,
            so a track taken from it needs that argument.
        frame_rate: F0 frames per second; must be finite and positive.
        voiced: Per-frame voiced flags (non-zero = voiced). Preferred over
            ``voiced_prob``; pass :func:`pitch_pyin`'s ``voiced_flag`` here.
        voiced_prob: Per-frame voicing probability in ``[0, 1]``, read ONLY when
            ``voiced`` is ``None``. pYIN's ``voiced_prob`` is a frame's voiced
            observation mass and rises with F0 for a fixed frame length, so
            thresholding it drops entire low registers — prefer ``voiced``.
        segmentation_threshold_cents: Pitch jump that starts a new note;
            ``None`` keeps the library default (50 cents).
        min_note_ms: Shortest note kept; ``None`` keeps the default (30 ms).
        reference_hz: Reference for ``median_cents``; ``None`` keeps the default
            (A4 = 440 Hz).
        voiced_threshold: Value of ``voiced_prob`` at or above which a frame
            counts as voiced; ``None`` keeps the default (0.5). Ignored when
            ``voiced`` is supplied.

    Returns:
        List of :class:`NoteObject` in time order; empty when the F0 track
        segments into no stable notes.

    Raises:
        SonareValueError: If neither ``voiced`` nor ``voiced_prob`` is given, if
            either has a different length than ``f0_hz``, or if a buffer is
            empty. A non-finite ``f0_hz`` frame is read as carrying no pitch,
            not refused.

    Example:
        pYIN's default leaves an unvoiced frame as ``NaN`` and this function
        reads it as what it is -- a frame carrying no pitch -- so ``fill_na``
        is a choice about the contour you want rather than a requirement.

        >>> pitch = libsonare.pitch_pyin(samples, sample_rate=sr, hop_length=512)
        >>> notes = libsonare.extract_notes(
        ...     samples,
        ...     sr,
        ...     pitch.f0,
        ...     sr / 512,
        ...     voiced=[int(v) for v in pitch.voiced_flag],
        ... )
        >>> notes[0].edit.gain_db = -6.0
        >>> quieter = libsonare.render_notes(samples, sr, notes)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_extract_notes"):
        raise _unsupported_effect_symbol("sonare_extract_notes")

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array, voiced_array = _note_voicing_arrays("extract_notes", n_frames, voiced, voiced_prob)
    config = _note_extractor_config(
        segmentation_threshold_cents, min_note_ms, reference_hz, voiced_threshold
    )
    out = SonareNoteObjectsResult()
    rc = lib.sonare_extract_notes(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        f0_array,
        prob_array,
        voiced_array,
        _to_c_size_t(n_frames, "n_frames"),
        _to_c_float(frame_rate, "frame_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _notes_from_c(out)
    finally:
        lib.sonare_free_note_objects(ctypes.byref(out))


@_guard_buffer("samples")
def render_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    notes: Sequence[NoteObject],
    *,
    f0_hz: Sequence[float] | list[float] | np.ndarray | None = None,
    frame_rate: float | None = None,
    fade_ms: float | None = None,
    vibrato_cutoff_hz: float | None = None,
) -> np.ndarray:
    """Render edited note objects over their source audio.

    Each note's ``onset_sample``, ``offset_sample`` and ``edit`` are read, plus
    its frame bounds and ``median_hz`` when a curve edit needs them, so the list
    :func:`extract_notes` returned can be passed straight back. A note whose
    edit is the identity is not resynthesized, so a set whose edits are all
    identity reproduces the input exactly.

    Overlap is checked on the *source* spans only. Where an edit's
    ``time_offset_samples`` lands a note is not, and a note lengthened past its
    own span writes into its neighbours' samples, so two moved or stretched
    notes may be written over each other.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        notes: Notes to render; an empty sequence returns the input unchanged.
        f0_hz: The F0 track the notes were extracted from. Required only by
            ``NoteEdit.vibrato_depth_change`` and ``NoteEdit.drift_change``,
            which act on the note's own pitch curve -- that curve is this array
            sliced by ``[frame_start, frame_end)``, which the caller already
            holds, so it is not carried on the notes. Every other edit ignores
            it.
        frame_rate: F0 frames per second; required when ``f0_hz`` is given and
            ignored otherwise.
        fade_ms: Equal-power cross-fade at each edited note's edges; ``None``
            keeps the library default (5 ms). A hard cut is deliberately not
            selectable, because the seam it leaves behind is a click.
        vibrato_cutoff_hz: Boundary between the drift and the vibrato that the
            two curve edits act on; ``None`` keeps the default (3 Hz). Pass what
            :func:`decompose_note_pitch` was called with -- a host that draws
            the vibrato at one cutoff and edits it at another edits a curve it
            never showed anyone.

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite, or if ``f0_hz``
            was given without ``frame_rate``.
        SonareError: If the C call rejects the request (e.g. a note span that
            overlaps another's, or a curve edit with no ``f0_hz``).

    Example:
        >>> notes[0].edit.vibrato_depth_change = -1.0
        >>> flattened = libsonare.render_notes(
        ...     audio, sr, notes, f0_hz=pitch.f0, frame_rate=sr / 512
        ... )
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_render_notes"):
        raise _unsupported_effect_symbol("sonare_render_notes")

    c_array, length = _to_c_float_array(samples)
    c_notes, note_count, envelopes, envelope_count = _notes_to_c("render_notes", notes)

    f0_array = None
    n_frames = 0
    # Unread when there is no track, which is how the C ABI spells "no frames".
    c_frame_rate = 0.0
    if f0_hz is not None:
        if frame_rate is None:
            raise SonareValueError("render_notes: pass frame_rate with f0_hz")
        f0_array, n_frames = _to_c_float_array(
            _validate_samples("render_notes", f0_hz, arg_name="f0_hz")
        )
        c_frame_rate = float(frame_rate)

    config = SonareNoteRenderConfig(
        _NOTE_STRUCT_VERSION,
        0.0 if fade_ms is None else fade_ms,
        0.0 if vibrato_cutoff_hz is None else vibrato_cutoff_hz,
    )
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_render_notes(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                c_notes,
                _to_c_size_t(note_count, "note_count"),
                envelopes,
                _to_c_size_t(envelope_count, "envelope_count"),
                f0_array,
                _to_c_size_t(n_frames, "n_frames"),
                _to_c_float(c_frame_rate, "frame_rate"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_guard_buffer(shape_only=("f0_hz",))
def decompose_note_pitch(
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    median_hz: float,
    *,
    vibrato_cutoff_hz: float | None = None,
) -> PitchDecomposition:
    """Split one note's pitch curve into a centre, a drift and a vibrato.

    This is what a host draws when it shows a note's pitch, and what
    ``NoteEdit.vibrato_depth_change`` and ``NoteEdit.drift_change`` then scale.
    Hand :func:`render_notes` the same ``vibrato_cutoff_hz``, or the curve being
    edited is not the curve that was drawn.

    The note's own F0 curve is not returned by :func:`extract_notes`, because it
    is the caller's own track sliced by the note's frame bounds; pass that slice
    here together with the note's ``median_hz``.

    Args:
        f0_hz: The note's slice of the F0 track, one entry per frame. Every
            value must be finite and non-negative; zero denotes an unvoiced
            frame.
        frame_rate: F0 frames per second; must be finite and positive.
        median_hz: The note's ``median_hz``; must be finite and non-negative. A
            note with no pitch is spelled 0, so a negative value is a caller bug
            rather than a second way of saying that.
        vibrato_cutoff_hz: Boundary between the two curves; ``None`` keeps the
            default (3 Hz).

    Returns:
        :class:`PitchDecomposition`. A note with no usable pitch is reported as
        a zero ``centre_hz`` and two empty curves rather than as an error.

    Raises:
        SonareValueError: If ``f0_hz`` is empty. A non-finite frame is read
            as carrying no pitch, not refused.
        SonareError: If the C call rejects the request.

    Example:
        >>> note = notes[0]
        >>> curve = libsonare.decompose_note_pitch(
        ...     f0_hz[note.frame_start : note.frame_end], sr / 512, note.median_hz
        ... )
        >>> curve.vibrato_cents.max()  # doctest: +SKIP
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_decompose_note_pitch"):
        raise _unsupported_effect_symbol("sonare_decompose_note_pitch")

    f0_array, n_frames = _to_c_float_array(f0_hz)
    out = SonarePitchDecompositionResult()
    rc = lib.sonare_decompose_note_pitch(
        f0_array,
        _to_c_size_t(n_frames, "n_frames"),
        _to_c_float(frame_rate, "frame_rate"),
        _to_c_float(median_hz, "median_hz"),
        _to_c_float(0.0 if vibrato_cutoff_hz is None else vibrato_cutoff_hz, "vibrato_cutoff_hz"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return PitchDecomposition(
            centre_hz=float(out.centre_hz),
            drift_cents=_from_c_float_array(out.drift_cents, out.count),
            vibrato_cents=_from_c_float_array(out.vibrato_cents, out.count),
        )
    finally:
        lib.sonare_free_pitch_decomposition(ctypes.byref(out))


@_guard_buffer("samples", shape_only=("f0_hz",))
def split_note(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject],
    index: int,
    frame: int,
    *,
    voiced: Sequence[int] | list[int] | np.ndarray | None = None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]:
    """Split one note in two at a track frame, returning the whole new note set.

    Both halves are re-derived from the audio and the track the way
    :func:`extract_notes` derives its own, rather than by patching the fields of
    the note they replace. Both inherit the source note's edit, and its
    amplitude envelope is cut at the same proportion so each half keeps its own
    part of it -- which means a note whose edit is the identity still renders
    bit for bit after being split. A one-entry envelope is a constant over the
    span, so both halves get that same entry.

    Every note in the set, not just the two halves, has its spans, curves,
    medians and stability re-derived from ``samples`` and the track, because a
    :class:`NoteObject` carries no curves for this call to copy through. The
    frame bounds are therefore what a note is identified by here, and the audio
    and track arguments must be the ones the set was extracted from, or the
    whole set is re-measured against something else.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        f0_hz: Per-frame F0 in Hz, the track the set was extracted from.
        frame_rate: F0 frames per second; must be finite and positive.
        notes: The current note set. Each note's ``[frame_start, frame_end)``
            must be non-empty and inside the track.
        index: Note to split.
        frame: Track frame to cut at, strictly inside that note's own span.
        voiced: Per-frame voiced flags (non-zero = voiced). Preferred over
            ``voiced_prob``.
        voiced_prob: Per-frame voicing probability, read ONLY when ``voiced`` is
            ``None``.
        segmentation_threshold_cents: Pitch jump that starts a new note;
            ``None`` keeps the library default (50 cents).
        min_note_ms: Shortest note kept; ``None`` keeps the default (30 ms).
        reference_hz: Reference for ``median_cents``; ``None`` keeps the default
            (A4 = 440 Hz).
        voiced_threshold: Value of ``voiced_prob`` at or above which a frame
            counts as voiced; ``None`` keeps the default (0.5).

    Returns:
        The whole new list of :class:`NoteObject` in time order, one longer than
        ``notes``.

    Raises:
        SonareValueError: If neither ``voiced`` nor ``voiced_prob`` is given, if
            either has a different length than ``f0_hz``, if ``index`` is not a
            non-negative integer a ``size_t`` holds, or if a buffer is empty or
            non-finite.
        SonareError: If the C call rejects the request (e.g. a frame on or
            outside the note's own boundaries).

    Example:
        >>> notes = libsonare.split_note(audio, sr, f0_hz, sr / 512, notes, 1, 18,
        ...                              voiced=voiced)
    """
    return _note_set_edit(
        "split_note",
        "sonare_split_note",
        samples,
        sample_rate,
        f0_hz,
        frame_rate,
        notes,
        voiced,
        voiced_prob,
        _note_extractor_config(
            segmentation_threshold_cents, min_note_ms, reference_hz, voiced_threshold
        ),
        (
            _to_c_size_t(_note_set_index("split_note", "index", index), "index"),
            _to_c_int32(frame, "frame"),
        ),
    )


@_guard_buffer("samples", shape_only=("f0_hz",))
def merge_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject],
    first: int,
    last: int,
    *,
    voiced: Sequence[int] | list[int] | np.ndarray | None = None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]:
    """Join a run of notes into one, returning the whole new note set.

    The result spans from ``notes[first]``'s onset to ``notes[last]``'s offset,
    including whatever the segmenter cut out between them, and its measured
    fields are derived over that whole span -- the pitch and amplitude of an
    unvoiced gap live in the track and the audio, not in either neighbour.

    It takes ``notes[first]``'s edit, envelope included. Notes carrying
    different edits have no single correct answer here, so the rule is stated
    rather than guessed at; set the edit afterwards if it matters.

    Every note in the set is re-derived from ``samples`` and the track, exactly
    as :func:`split_note` describes.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        f0_hz: Per-frame F0 in Hz, the track the set was extracted from.
        frame_rate: F0 frames per second; must be finite and positive.
        notes: The current note set. Each note's ``[frame_start, frame_end)``
            must be non-empty and inside the track.
        first: Index of the first note to join; must be ``< last``.
        last: Index of the last note to join, inclusive.
        voiced: Per-frame voiced flags (non-zero = voiced). Preferred over
            ``voiced_prob``.
        voiced_prob: Per-frame voicing probability, read ONLY when ``voiced`` is
            ``None``.
        segmentation_threshold_cents: Pitch jump that starts a new note;
            ``None`` keeps the library default (50 cents).
        min_note_ms: Shortest note kept; ``None`` keeps the default (30 ms).
        reference_hz: Reference for ``median_cents``; ``None`` keeps the default
            (A4 = 440 Hz).
        voiced_threshold: Value of ``voiced_prob`` at or above which a frame
            counts as voiced; ``None`` keeps the default (0.5).

    Returns:
        The whole new list of :class:`NoteObject` in time order, ``last - first``
        shorter than ``notes``.

    Raises:
        SonareValueError: If neither ``voiced`` nor ``voiced_prob`` is given, if
            either has a different length than ``f0_hz``, if ``first`` or
            ``last`` is not a non-negative integer a ``size_t`` holds, or if a
            buffer is empty. A non-finite ``f0_hz`` frame is read as carrying no
            pitch, not refused.
        SonareError: If the C call rejects the request (e.g. a run that does not
            ascend, or one that runs past the end of the set).

    Example:
        >>> notes = libsonare.merge_notes(audio, sr, f0_hz, sr / 512, notes, 0, 1,
        ...                               voiced=voiced)
    """
    return _note_set_edit(
        "merge_notes",
        "sonare_merge_notes",
        samples,
        sample_rate,
        f0_hz,
        frame_rate,
        notes,
        voiced,
        voiced_prob,
        _note_extractor_config(
            segmentation_threshold_cents, min_note_ms, reference_hz, voiced_threshold
        ),
        (
            _to_c_size_t(_note_set_index("merge_notes", "first", first), "first"),
            _to_c_size_t(_note_set_index("merge_notes", "last", last), "last"),
        ),
    )
