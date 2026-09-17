"""Note objects, their pitch decomposition and the ctypes marshalling."""

from __future__ import annotations

import ctypes
import dataclasses
from collections.abc import Sequence
from numbers import Integral

import numpy as np

from ._ffi import (
    SonareNoteEdit,
    SonareNoteExtractorConfig,
    SonareNoteObject,
    SonareNoteObjectsResult,
    SonareTranscribeConfig,
    SonareTranscribeResult,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _INT64_MAX,
    _INT64_MIN,
    SonareValueError,
    _check,
    _from_c_float_array,
    _get_lib,
    _int_refusal,
    _narrow_int,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _unsupported_effect_symbol,
    _validate_c_int_field,
    _validate_scalar,
)

# Both note-object configs are at layout version 1; 0 selects the same layout.
_NOTE_STRUCT_VERSION = 1

# The transcription config's own layout version. Unlike the note-object configs
# above, 0 is rejected rather than read as version 1.
_TRANSCRIBE_STRUCT_VERSION = 1


@dataclasses.dataclass
class NoteEdit:
    """A pending, non-destructive change to one :class:`NoteObject`.

    The default instance is the identity edit, so a note carrying it is left
    untouched by :func:`render_notes` (and a set whose edits are all identity
    reproduces the input exactly). Mutate the fields in place to schedule work.

    Within one note :func:`render_notes` applies these in a fixed order: pitch
    curve, time stretch, pitch shift, formant warp, amplitude envelope, gain.

    Attributes:
        time_offset_samples: Moves the note along the timeline; negative moves
            it earlier.
        pitch_shift_semitones: Transposes the note; pitch only, duration is
            unchanged.
        gain_db: Level change applied over the note's span.
        time_stretch_ratio: ``>1`` lengthens the note, ``<1`` shortens it, with
            pitch preserved.
        muted: ``True`` silences the note's span; the other fields then do not
            apply.
        formant_shift_semitones: Moves the spectral envelope, on top of whatever
            ``pitch_shift_semitones`` already did to it. 0 runs no warp at all,
            so a pitch-only edit is not charged for an LPC analysis-resynthesis
            round it did not ask for; a pitch shift drags the formants with it,
            so holding them still is ``-pitch_shift_semitones`` and the chipmunk
            is the default. Saturates near -10.3 and +8.7 semitones rather than
            being rejected.
        vibrato_depth_change: Scales the vibrato measured over the note, stated
            as a change from it: 0 keeps it, ``-1`` flattens it, ``+1`` doubles
            it. It acts on the note's own pitch curve, so :func:`render_notes`
            rejects a note carrying it unless ``f0_hz`` is passed too.
        drift_change: The same, for the slow drift around the note's centre
            pitch. The two curves are split at ``vibrato_cutoff_hz``.
        amplitude_envelope: ``float32`` linear gain points over the note's span,
            on top of ``gain_db``; empty is no envelope. A set of gain points
            rather than a signal: it is stretched over whatever length the note
            renders at, so it survives a time stretch and need not match the
            note's frame count, and one entry is a constant gain.

    Example:
        >>> edit = libsonare.NoteEdit(gain_db=-3.0)
        >>> edit.amplitude_envelope = np.array([1.0, 0.0], dtype=np.float32)
        >>> edit.vibrato_depth_change = -1.0  # flatten this note's vibrato
    """

    time_offset_samples: int = 0
    pitch_shift_semitones: float = 0.0
    gain_db: float = 0.0
    time_stretch_ratio: float = 1.0
    muted: bool = False
    formant_shift_semitones: float = 0.0
    vibrato_depth_change: float = 0.0
    drift_change: float = 0.0
    # Out of the generated comparison, which an ndarray field makes raise on an
    # ambiguous truth value. __eq__ below puts the curve back in element by
    # element, because an edit carrying an envelope is not the identity edit and
    # must not compare equal to one.
    amplitude_envelope: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )

    def __eq__(self, other: object) -> bool:
        """Compare every field, the envelope element by element."""
        if not isinstance(other, NoteEdit):
            return NotImplemented
        return self._scalars() == other._scalars() and np.array_equal(
            np.asarray(self.amplitude_envelope), np.asarray(other.amplitude_envelope)
        )

    def _scalars(self) -> tuple[object, ...]:
        return (
            self.time_offset_samples,
            self.pitch_shift_semitones,
            self.gain_db,
            self.time_stretch_ratio,
            self.muted,
            self.formant_shift_semitones,
            self.vibrato_depth_change,
            self.drift_change,
        )


@dataclasses.dataclass
class NoteObject:
    """One editable note returned by :func:`extract_notes`.

    Sample bounds are half-open into the source audio; frame bounds are
    half-open into the caller's own F0 track, so the note's pitch curve is
    ``f0_hz[frame_start:frame_end]`` — it is deliberately not repeated here.
    The amplitude curve is measured by the extractor and therefore is: it is one
    RMS value per F0 frame, already sliced to this note.

    :func:`render_notes` reads ``onset_sample``, ``offset_sample`` and ``edit``,
    plus the frame bounds and ``median_hz`` when a curve edit needs them, so a
    host may hand back exactly what :func:`extract_notes` produced, or build a
    bare ``NoteObject(onset_sample=..., offset_sample=...)`` for a span it found
    some other way.

    The span has no default, so a note built by hand states it. The other
    surfaces declare the same two fields mandatory on their own note input and
    reject an omitted one; a default of 0 here would instead have been a
    zero-length span, rendering the note's edit as nothing.

    Attributes:
        onset_sample: First sample of the note.
        offset_sample: One past the last sample of the note.
        frame_start: First F0 frame of the note.
        frame_end: One past the last F0 frame of the note.
        median_hz: Median pitch of the span in Hz.
        median_cents: Median pitch in cents above the extractor's
            ``reference_hz``.
        f0_stability: Pitch steadiness in ``[0, 1]``; 1 is perfectly steady.
        amplitude: ``float32`` RMS curve, one value per frame of the span. Not
            part of ``==``, which compares the span, the metrics and the edit.
        edit: The pending :class:`NoteEdit`, identity on a freshly extracted
            note.
    """

    onset_sample: int
    offset_sample: int
    frame_start: int = 0
    frame_end: int = 0
    median_hz: float = 0.0
    median_cents: float = 0.0
    f0_stability: float = 0.0
    # Out of the comparison: an ndarray field makes the generated __eq__ raise
    # on ambiguous truth. The span already determines this curve.
    amplitude: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )
    edit: NoteEdit = dataclasses.field(default_factory=NoteEdit)


@dataclasses.dataclass
class PitchDecomposition:
    """One note's pitch curve split into a centre, a slow drift and a vibrato.

    ``drift_cents[i] + vibrato_cents[i]`` is the note's own pitch at frame ``i``,
    in cents above ``centre_hz``, to within float rounding, so the three parts
    reconstruct the curve. The drift filter is zero phase, so neither curve is
    shifted in time against the audio.

    Frames whose F0 is unusable carry no measurement, so the curve is held at
    the nearest usable neighbour across them. Both curves therefore have an
    entry everywhere; a host marking the held ones reads them off its own
    ``f0_hz``, which is exact.

    Attributes:
        centre_hz: The note's steady pitch in Hz. 0 when the note carries no
            usable pitch, and then both curves are empty.
        drift_cents: ``float32`` slow deviation from ``centre_hz``, in cents.
            Not part of ``==``, which an ndarray field would make raise on an
            ambiguous truth value.
        vibrato_cents: ``float32`` fast deviation, over the same frames. Out of
            ``==`` for the same reason.
    """

    centre_hz: float = 0.0
    drift_cents: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )
    vibrato_cents: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )


def _note_voicing_arrays(
    fn_name: str,
    n_frames: int,
    voiced: Sequence[int] | list[int] | np.ndarray | None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None,
) -> tuple[object, object]:
    """Marshal the two per-frame voicing arrays; at least one is required."""
    if voiced is None and voiced_prob is None:
        raise SonareValueError(f"{fn_name}: pass voiced or voiced_prob")
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
        if prob_len != n_frames:
            raise SonareValueError(f"{fn_name}: voiced_prob must have f0_hz's length")
    voiced_array = None
    if voiced is not None:
        voiced_array, voiced_len = _to_c_int_array(voiced, "voiced")
        if voiced_len != n_frames:
            raise SonareValueError(f"{fn_name}: voiced must have f0_hz's length")
    return prob_array, voiced_array


def _note_extractor_config(
    segmentation_threshold_cents: float | None,
    min_note_ms: float | None,
    reference_hz: float | None,
    voiced_threshold: float | None,
) -> SonareNoteExtractorConfig:
    """Build the extractor config; every field takes its library default at 0."""
    return SonareNoteExtractorConfig(
        _NOTE_STRUCT_VERSION,
        0.0 if segmentation_threshold_cents is None else segmentation_threshold_cents,
        0.0 if min_note_ms is None else min_note_ms,
        0.0 if reference_hz is None else reference_hz,
        0.0 if voiced_threshold is None else voiced_threshold,
    )


@dataclasses.dataclass
class TranscribeResult:
    """What one transcription found, ready to hand to a MIDI clip.

    Attributes:
        events: Note-on / note-off pairs as ``(ppq, data0, data1)``, the shape
            :meth:`Project.set_midi_events` takes, in canonical PPQ order with a
            note-off before a note-on that shares its tick. Two events per note,
            so ``len(events)`` is ``2 * note_count``.
        note_count: Number of notes.
        tempo_bpm: The tempo the PPQ coordinates were built on -- the requested
            value when one was given, and the detected one otherwise.
    """

    events: list[tuple[float, int, int]] = dataclasses.field(default_factory=list)
    note_count: int = 0
    tempo_bpm: float = 0.0


def _transcribe_int(fn_name: str, value: object, arg_name: str, low: int, high: int) -> int:
    """Narrow one transcription config integer, naming the entry point it came from."""
    domain = f"must be an integer in [{low}, {high}]"
    try:
        return _narrow_int(value, arg_name, low, high)
    except SonareValueError as exc:
        raise _int_refusal(fn_name, value, arg_name, domain) from exc


def _transcribe_positive(fn_name: str, value: float, arg_name: str) -> float:
    """Accept one transcription config float that has to be finite and positive."""
    number = _validate_scalar(fn_name, value, arg_name)
    if number <= 0.0:
        raise SonareValueError(f"{fn_name}: {arg_name} must be positive")
    return number


def _transcribe_config(
    fn_name: str,
    *,
    polyphonic: bool,
    reference_hz: float | None,
    fmin: float | None,
    fmax: float | None,
    min_note_ms: float | None,
    segmentation_threshold_cents: float | None,
    velocity_floor_db: float | None,
    fixed_velocity: int | None,
    group: int,
    channel: int,
) -> SonareTranscribeConfig:
    """Build the transcription config, refusing an out-of-domain field by name.

    ``None`` is this binding's spelling of "keep the library default" and the C
    ABI's is 0, so a value the caller passed is always a request: an explicit
    ``velocity_floor_db=0.0`` or ``fixed_velocity=0`` is a mistake rather than
    the default, and is refused here. Whether ``fmin`` and ``fmax`` bracket each
    other is left to the core, which is the side that has both resolved
    defaults; passing both wrong way round is named here.
    """
    config = SonareTranscribeConfig()
    config.struct_version = _TRANSCRIBE_STRUCT_VERSION
    config.polyphonic = 1 if polyphonic else 0
    for arg_name, value in (
        ("reference_hz", reference_hz),
        ("fmin", fmin),
        ("fmax", fmax),
        ("min_note_ms", min_note_ms),
        ("segmentation_threshold_cents", segmentation_threshold_cents),
    ):
        setattr(
            config,
            arg_name,
            0.0 if value is None else _transcribe_positive(fn_name, value, arg_name),
        )
    if fmin is not None and fmax is not None and config.fmax <= config.fmin:
        raise SonareValueError(f"{fn_name}: fmax must be above fmin")

    if velocity_floor_db is None:
        config.velocity_floor_db = 0.0
    else:
        level = _validate_scalar(fn_name, velocity_floor_db, "velocity_floor_db")
        if level >= 0.0:
            raise SonareValueError(f"{fn_name}: velocity_floor_db must be negative")
        config.velocity_floor_db = level

    config.fixed_velocity = (
        0
        if fixed_velocity is None
        else _transcribe_int(fn_name, fixed_velocity, "fixed_velocity", 1, 127)
    )
    config.group = _transcribe_int(fn_name, group, "group", 0, 15)
    config.channel = _transcribe_int(fn_name, channel, "channel", 0, 15)
    return config


def _transcribe_sample_rate(fn_name: str, sample_rate: int) -> int:
    """Refuse a non-positive rate here, where the argument still has a name."""
    return _transcribe_int(fn_name, sample_rate, "sample_rate", 1, _C_INT_MAX)


def _transcribe_result_from_c(out: SonareTranscribeResult) -> TranscribeResult:
    """Copy a transcription out of C memory, before it is released.

    Read length-first: the event pointer is NULL when nothing was found, and a
    ctypes NULL raises on dereference rather than reading zeros.
    """
    events = [
        (float(out.events[i].ppq), int(out.events[i].data0), int(out.events[i].data1))
        for i in range(int(out.count))
    ]
    return TranscribeResult(
        events=events,
        note_count=int(out.note_count),
        tempo_bpm=float(out.tempo_bpm),
    )


def _notes_from_c(out: SonareNoteObjectsResult) -> list[NoteObject]:
    """Build the note list from a C result, before it is released.

    The two concatenated pools are copied out once and each note is handed its
    own slice of both, so no caller has to redo the offset arithmetic and no
    returned array is a view into memory the C side is about to free.
    """
    amplitude = _from_c_float_array(out.amplitude, out.amplitude_count)
    envelopes = _from_c_float_array(out.envelopes, out.envelope_count)
    return [_note_from_c(out.notes[i], amplitude, envelopes) for i in range(out.count)]


def _note_from_c(row: SonareNoteObject, amplitude: np.ndarray, envelopes: np.ndarray) -> NoteObject:
    """Build one :class:`NoteObject` from a C row and the two shared pools."""
    start = int(row.amplitude_offset)
    stop = start + int(row.frame_end) - int(row.frame_start)
    envelope_start = int(row.edit.envelope_offset)
    envelope_stop = envelope_start + int(row.edit.envelope_count)
    return NoteObject(
        onset_sample=int(row.onset_sample),
        offset_sample=int(row.offset_sample),
        frame_start=int(row.frame_start),
        frame_end=int(row.frame_end),
        median_hz=float(row.median_hz),
        median_cents=float(row.median_cents),
        f0_stability=float(row.f0_stability),
        amplitude=amplitude[start:stop].copy(),
        edit=NoteEdit(
            time_offset_samples=int(row.edit.time_offset_samples),
            pitch_shift_semitones=float(row.edit.pitch_shift_semitones),
            gain_db=float(row.edit.gain_db),
            time_stretch_ratio=float(row.edit.time_stretch_ratio),
            muted=bool(row.edit.muted),
            formant_shift_semitones=float(row.edit.formant_shift_semitones),
            vibrato_depth_change=float(row.edit.vibrato_depth_change),
            drift_change=float(row.edit.drift_change),
            amplitude_envelope=envelopes[envelope_start:envelope_stop].copy(),
        ),
    )


def _notes_to_c(fn_name: str, notes: Sequence[NoteObject]) -> tuple[object, int, object, int]:
    """Marshal a note list into the C note array plus its shared envelope pool.

    The C ABI carries every note's envelope in one array the edits index into,
    which is an ownership arrangement rather than something a caller should have
    to build; each :class:`NoteEdit` owns its own curve, so the pool is packed
    here and the offsets are derived.
    """
    note_count = len(notes)
    if note_count == 0:
        return None, 0, None, 0

    c_notes = (SonareNoteObject * note_count)()
    curves: list[np.ndarray] = []
    envelope_offset = 0
    for i, note in enumerate(notes):
        curve = np.asarray(note.edit.amplitude_envelope, dtype=np.float32)
        if curve.ndim != 1:
            raise SonareValueError(
                f"{fn_name}: notes[{i}].edit.amplitude_envelope must be one-dimensional"
            )
        curves.append(curve)
        # The metrics and the amplitude offset are not marshalled: nothing on
        # the far side reads them back.
        # Narrowed rather than coerced, as the edit below is: int() takes 100.7
        # as 100, and the note would be rendered from a boundary nobody asked for.
        c_notes[i].onset_sample = _narrow_int(
            note.onset_sample, f"{fn_name}: notes[{i}].onset_sample", _INT64_MIN, _INT64_MAX
        )
        c_notes[i].offset_sample = _narrow_int(
            note.offset_sample, f"{fn_name}: notes[{i}].offset_sample", _INT64_MIN, _INT64_MAX
        )
        c_notes[i].frame_start = _narrow_int(
            note.frame_start, f"{fn_name}: notes[{i}].frame_start", _C_INT_MIN, _C_INT_MAX
        )
        c_notes[i].frame_end = _narrow_int(
            note.frame_end, f"{fn_name}: notes[{i}].frame_end", _C_INT_MIN, _C_INT_MAX
        )
        c_notes[i].median_hz = float(note.median_hz)
        c_notes[i].edit = SonareNoteEdit(
            # Narrowed rather than coerced: int(0.5) is 0, which is this field's
            # identity, so a sub-sample shift would render unmoved and report
            # success.
            time_offset_samples=_validate_c_int_field(
                fn_name, note.edit.time_offset_samples, f"notes[{i}].edit.time_offset_samples"
            ),
            envelope_offset=envelope_offset,
            envelope_count=int(curve.size),
            pitch_shift_semitones=float(note.edit.pitch_shift_semitones),
            gain_db=float(note.edit.gain_db),
            time_stretch_ratio=float(note.edit.time_stretch_ratio),
            formant_shift_semitones=float(note.edit.formant_shift_semitones),
            vibrato_depth_change=float(note.edit.vibrato_depth_change),
            drift_change=float(note.edit.drift_change),
            muted=1 if note.edit.muted else 0,
        )
        envelope_offset += int(curve.size)

    if envelope_offset == 0:
        return c_notes, note_count, None, 0
    pool, pool_count = _to_c_float_array(np.concatenate(curves))
    return c_notes, note_count, pool, pool_count


def _note_set_index(fn_name: str, arg_name: str, value: int) -> int:
    """Reject a negative note index before ``size_t`` wraps it into a huge one.

    The sign half only. Every call site hands the result to :func:`_to_c_size_t`,
    which carries the ceiling -- an index past it folds onto a live note.
    """
    if isinstance(value, bool) or not isinstance(value, Integral):
        raise SonareValueError(f"{fn_name}: {arg_name} must be an integer")
    if int(value) < 0:
        raise SonareValueError(f"{fn_name}: {arg_name} must be non-negative")
    return int(value)


def _note_set_edit(
    fn_name: str,
    symbol: str,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject],
    voiced: Sequence[int] | list[int] | np.ndarray | None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None,
    config: SonareNoteExtractorConfig,
    cut: tuple[object, ...],
) -> list[NoteObject]:
    """Shared body of :func:`split_note` and :func:`merge_notes`.

    The two differ only in ``cut``, the pair of arguments naming where to cut or
    what to join; everything else -- the track, the note set and its envelope
    pool, the extractor config -- is marshalled identically.
    """
    lib = _get_lib()
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz, arg_name="f0_hz")
    prob_array, voiced_array = _note_voicing_arrays(fn_name, n_frames, voiced, voiced_prob)
    c_notes, note_count, envelopes, envelope_count = _notes_to_c(fn_name, notes)

    out = SonareNoteObjectsResult()
    rc = getattr(lib, symbol)(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        f0_array,
        prob_array,
        voiced_array,
        _to_c_size_t(n_frames, "n_frames"),
        _to_c_float(frame_rate, "frame_rate"),
        ctypes.byref(config),
        c_notes,
        _to_c_size_t(note_count, "note_count"),
        envelopes,
        _to_c_size_t(envelope_count, "envelope_count"),
        *cut,
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _notes_from_c(out)
    finally:
        lib.sonare_free_note_objects(ctypes.byref(out))
