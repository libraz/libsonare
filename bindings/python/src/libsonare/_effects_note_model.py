"""Note objects, their pitch decomposition and the ctypes marshalling."""

from __future__ import annotations

import ctypes
import dataclasses
import math
from collections.abc import Sequence
from numbers import Integral

import numpy as np
from numpy.typing import NDArray

from ._errors import _refuse, _refuse_integer, _refuse_type
from ._ffi import (
    SonareNoteEdit,
    SonareNoteExtractorConfig,
    SonareNoteObject,
    SonareNoteObjectsResult,
    SonareTranscribeConfig,
    SonareTranscribeResult,
)
from ._narrowing import _is_number, _narrow_double, _resolve_zero_default
from ._project_model import ProjectMidiEvent, _midi_event_from_c
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _SIZE_T_MAX,
    SonareValueError,
    _check,
    _from_c_float_array,
    _get_lib,
    _int_refusal,
    _narrow_int,
    _resolve_sample_bound,
    _resolve_time_offset,
    _Respelling,
    _respellings,
    _SampleBound,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _validate_scalar,
)

# Monophonic note models, by C ABI value (0 is the default, segments).
_MONOPHONIC_MODELS = {"segments": 0, "singing": 1}


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
            it earlier. 0 is the identity. A shift stated in seconds is a
            :class:`NoteEditInput`.
        pitch_semitones: Transposes the note; pitch only, duration is
            unchanged.
        gain_db: Level change applied over the note's span.
        time_stretch_ratio: Duration ratio: ``>1`` lengthens the note, ``<1``
            shortens it, with pitch preserved. The opposite sense of
            :func:`time_stretch`'s ``speed_ratio``, which is a speed.
        muted: ``True`` silences the note's span; the other fields then do not
            apply.
        formant_semitones: Moves the spectral envelope, on top of whatever
            ``pitch_semitones`` already did to it. 0 runs no warp at all,
            so a pitch-only edit is not charged for an LPC analysis-resynthesis
            round it did not ask for; a pitch shift drags the formants with it,
            so holding them still is ``-pitch_semitones`` and the chipmunk
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
    pitch_semitones: float = 0.0
    gain_db: float = 0.0
    time_stretch_ratio: float = 1.0
    muted: bool = False
    formant_semitones: float = 0.0
    vibrato_depth_change: float = 0.0
    drift_change: float = 0.0
    # Out of the generated comparison, which an ndarray field makes raise on an
    # ambiguous truth value. __eq__ below puts the curve back in element by
    # element, because an edit carrying an envelope is not the identity edit and
    # must not compare equal to one.
    amplitude_envelope: NDArray[np.float32] = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )

    def __eq__(self, other: object) -> bool:
        """Compare every field, the envelope element by element."""
        if not isinstance(other, NoteEdit):
            return NotImplemented
        return _edit_fields(self) == _edit_fields(other) and np.array_equal(
            np.asarray(self.amplitude_envelope), np.asarray(other.amplitude_envelope)
        )


@dataclasses.dataclass(kw_only=True)
class NoteEditInput:
    """A note edit a host builds, for :class:`NoteObjectInput` and :class:`NoteSetEntry`.

    The fields and defaults are :class:`NoteEdit`'s, so the default instance is
    the identity edit; the time shift may be stated in seconds and the envelope
    may be any one-dimensional sequence of gains. A returned :class:`NoteEdit`
    is accepted wherever this is.

    Attributes:
        time_offset_samples: :attr:`NoteEdit.time_offset_samples`; ``None`` is
            no shift. Not with ``time_offset_sec``.
        time_offset_sec: The shift in seconds, rounded to the nearest sample at
            the call's sample rate. Not with ``time_offset_samples``.
        pitch_semitones: :attr:`NoteEdit.pitch_semitones`.
        gain_db: :attr:`NoteEdit.gain_db`.
        time_stretch_ratio: :attr:`NoteEdit.time_stretch_ratio`.
        muted: :attr:`NoteEdit.muted`.
        formant_semitones: :attr:`NoteEdit.formant_semitones`.
        vibrato_depth_change: :attr:`NoteEdit.vibrato_depth_change`.
        drift_change: :attr:`NoteEdit.drift_change`.
        amplitude_envelope: :attr:`NoteEdit.amplitude_envelope`, as any
            one-dimensional sequence of linear gains; empty is no envelope.
    """

    time_offset_samples: int | None = None
    time_offset_sec: float | None = None
    pitch_semitones: float = 0.0
    gain_db: float = 0.0
    time_stretch_ratio: float = 1.0
    muted: bool = False
    formant_semitones: float = 0.0
    vibrato_depth_change: float = 0.0
    drift_change: float = 0.0
    # Out of the generated comparison for the reason NoteEdit gives.
    amplitude_envelope: Sequence[float] | NDArray[np.float32] = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )

    def __eq__(self, other: object) -> bool:
        """Compare every field, the envelope element by element."""
        if not isinstance(other, NoteEditInput):
            return NotImplemented
        return (
            _edit_fields(self) == _edit_fields(other)
            and self.time_offset_sec == other.time_offset_sec
            and np.array_equal(
                np.asarray(self.amplitude_envelope), np.asarray(other.amplitude_envelope)
            )
        )


def _edit_fields(edit: NoteEdit | NoteEditInput) -> tuple[object, ...]:
    """The scalar fields both note edit records define."""
    return (
        edit.time_offset_samples,
        edit.pitch_semitones,
        edit.gain_db,
        edit.time_stretch_ratio,
        edit.muted,
        edit.formant_semitones,
        edit.vibrato_depth_change,
        edit.drift_change,
    )


@dataclasses.dataclass(frozen=True)
class NoteObject:
    """One editable note returned by :func:`extract_notes`.

    Sample bounds are half-open into the source audio; frame bounds are
    half-open into the caller's own F0 track, so the note's pitch curve is
    ``f0_hz[frame_start:frame_end]`` — it is deliberately not repeated here.
    The amplitude curve is measured by the extractor and therefore is: it is one
    RMS value per F0 frame, already sliced to this note.

    Every field is filled by the library. A note a host builds is a
    :class:`NoteObjectInput`; a returned note is accepted wherever that is, so
    the list :func:`extract_notes` produced can be passed back with its edits
    changed.

    Attributes:
        onset_sample: First sample of the note.
        offset_sample: One past the last sample of the note.
        frame_start: First F0 frame of the note.
        frame_end: One past the last F0 frame of the note.
        median_hz: Median pitch of the span in Hz; ``None`` when the span carries
            no usable pitch.
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
    frame_start: int
    frame_end: int
    median_hz: float | None
    median_cents: float
    f0_stability: float
    # Out of the comparison: an ndarray field makes the generated __eq__ raise
    # on ambiguous truth. The span already determines this curve.
    amplitude: NDArray[np.float32] = dataclasses.field(compare=False)
    edit: NoteEdit = dataclasses.field(default_factory=NoteEdit)


@dataclasses.dataclass(frozen=True, kw_only=True)
class NoteObjectInput:
    """A note a host hands to :func:`render_notes` for a span it found itself.

    The span is stated in samples or in seconds (rounded to the nearest sample
    at the call's sample rate), one spelling per bound. Neither is defaulted: a
    note with neither bound is refused, because a default of 0 would be a
    zero-length span that renders the note's edit as nothing. The frame bounds
    and ``median_hz`` are read only by a ``vibrato_depth_change`` /
    ``drift_change`` edit, which slices the F0 track with them.

    Attributes:
        onset_sample: First sample of the span. Give this or ``onset_sec``.
        onset_sec: First second of the span. Not with ``onset_sample``.
        offset_sample: One past the last sample of the span. Give this or
            ``offset_sec``.
        offset_sec: End of the span in seconds. Not with ``offset_sample``.
        frame_start: First frame of the span in the call's ``f0_hz``; ``None``
            is frame 0.
        frame_end: One past the last frame of the span; ``None`` is frame 0.
        median_hz: The centre pitch a curve edit measures its cents against;
            ``None`` is no pitch.
        edit: The edit to render; the identity when omitted.
    """

    onset_sample: int | None = None
    onset_sec: float | None = None
    offset_sample: int | None = None
    offset_sec: float | None = None
    frame_start: int | None = None
    frame_end: int | None = None
    median_hz: float | None = None
    edit: NoteEdit | NoteEditInput = dataclasses.field(default_factory=NoteEditInput)


@dataclasses.dataclass(frozen=True, kw_only=True)
class NoteSetEntry:
    """One note of the set :func:`split_note` and :func:`merge_notes` re-derive.

    A note set is identified by its frame bounds alone; every note is
    re-measured from the audio and the track, so nothing else is read. A
    returned :class:`NoteObject` is accepted wherever this is.

    Attributes:
        frame_start: First frame of the span in the call's ``f0_hz``.
        frame_end: One past the last frame of the span; greater than
            ``frame_start``.
        edit: The note's pending edit, carried onto the re-derived note; the
            identity when omitted.
    """

    frame_start: int
    frame_end: int
    edit: NoteEdit | NoteEditInput = dataclasses.field(default_factory=NoteEditInput)


@dataclasses.dataclass(frozen=True)
class PitchDecompositionResult:
    """One note's pitch curve split into a centre, a slow drift and a vibrato.

    ``drift_cents[i] + vibrato_cents[i]`` is the note's own pitch at frame ``i``,
    in cents above ``center_hz``, to within float rounding, so the three parts
    reconstruct the curve. The drift filter is zero phase, so neither curve is
    shifted in time against the audio.

    Frames whose F0 is unusable carry no measurement, so the curve is held at
    the nearest usable neighbour across them. Both curves therefore have an
    entry everywhere; a host marking the held ones reads them off its own
    ``f0_hz``, which is exact.

    Attributes:
        center_hz: The note's steady pitch in Hz. ``None`` when the note carries
            no usable pitch, and then both curves are empty.
        drift_cents: ``float32`` slow deviation from ``center_hz``, in cents.
            Not part of ``==``, which an ndarray field would make raise on an
            ambiguous truth value.
        vibrato_cents: ``float32`` fast deviation, over the same frames. Out of
            ``==`` for the same reason.
    """

    center_hz: float | None = None
    drift_cents: NDArray[np.float32] = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )
    vibrato_cents: NDArray[np.float32] = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )


def _note_voicing_arrays(
    n_frames: int,
    voiced: Sequence[int] | list[int] | np.ndarray | None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None,
) -> tuple[object, object]:
    """Marshal the given voicing arrays; the C entry refuses both or neither."""
    voiced_array = None
    prob_array = None
    if voiced is not None:
        voiced_array, voiced_len = _to_c_int_array(voiced, "voiced")
        if voiced_len != n_frames:
            raise _refuse("voiced must have f0_hz's length")
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
        if prob_len != n_frames:
            raise _refuse("voiced_prob must have f0_hz's length")
    return prob_array, voiced_array


def _note_extractor_config(
    segmentation_threshold_cents: float | None,
    min_note_ms: float | None,
    reference_hz: float | None,
    voiced_threshold: float | None,
) -> SonareNoteExtractorConfig:
    """Build the extractor config; a field left as None keeps its library default."""
    return SonareNoteExtractorConfig._given(
        segmentation_threshold_cents=segmentation_threshold_cents,
        min_note_ms=min_note_ms,
        reference_hz=reference_hz,
        voiced_threshold=voiced_threshold,
    )


@dataclasses.dataclass(frozen=True)
class TranscribeResult:
    """What one transcription found, ready to hand to a MIDI clip.

    Attributes:
        events: Note-on / note-off pairs, the records
            :meth:`Project.set_midi_events` takes, in canonical PPQ order with a
            note-off before a note-on that shares its tick. Two events per note,
            so ``len(events)`` is ``2 * note_count``.
        note_count: Number of notes.
        tempo_bpm: The tempo the PPQ coordinates were built on -- the requested
            value when one was given, and the detected one otherwise.
        tuning_cents: The tuning the note numbers were measured against, in
            cents from concert A440: the measured value under
            ``reference_hz="auto"``, otherwise the one ``reference_hz`` amounts
            to (:func:`reference_hz_to_tuning`).
    """

    events: list[ProjectMidiEvent] = dataclasses.field(default_factory=list)
    note_count: int = 0
    tempo_bpm: float = 0.0
    tuning_cents: float = 0.0


def _transcribe_int(value: object, arg_name: str, low: int, high: int) -> int:
    """Narrow one transcription config integer, naming the entry point it came from."""
    domain = f"must be an integer in [{low}, {high}]"
    try:
        return _narrow_int(value, arg_name, low, high)
    except (TypeError, SonareValueError) as exc:
        raise _int_refusal(value, arg_name, domain) from exc


def _transcribe_optional(value: object, arg_name: str) -> int:
    """One transcription config integer whose C ``0`` means "not given"."""
    if value is None:
        return 0
    return int(_resolve_zero_default(_to_c_int(value, arg_name).value, arg_name))


def _transcribe_positive(value: float, arg_name: str) -> float:
    """Accept one transcription config float that has to be finite and greater than 0."""
    number = _validate_scalar(value, arg_name)
    if number <= 0.0:
        raise _refuse(f"{arg_name} must be greater than 0")
    return number


def _transcribe_ratio(value: float, arg_name: str) -> float:
    """Narrow one polyphonic peak ratio; a negative or a value above 1 is refused here."""
    number = _validate_scalar(value, arg_name)
    if number < 0.0:
        raise _refuse(f"{arg_name} must not be negative")
    if number > 1.0:
        raise _refuse(f"{arg_name} must be at most 1")
    return number


def _transcribe_reattack(value: float) -> float:
    """Narrow a given ``reattack_ratio``: above 1, since ``None`` is "no split"."""
    number = _validate_scalar(value, "reattack_ratio")
    if number <= 1.0:
        raise _refuse("reattack_ratio must be above 1 (use None for no split)")
    return number


def _transcribe_monophonic_model(value: str | None) -> int:
    """Narrow ``monophonic_model`` to its C value; ``None`` is the segments default."""
    if value is None:
        return 0
    if not isinstance(value, str):
        raise _refuse_type("monophonic_model must be a string or None")
    if value not in _MONOPHONIC_MODELS:
        raise _refuse(f"monophonic_model must be one of {', '.join(_MONOPHONIC_MODELS)}")
    return _MONOPHONIC_MODELS[value]


def _transcribe_config(
    *,
    polyphonic: bool,
    reference_hz: float | str | None,
    f0_min_hz: float | None,
    f0_max_hz: float | None,
    min_note_ms: float | None,
    segmentation_threshold_cents: float | None,
    velocity_floor_db: float | None,
    fixed_velocity: int | None,
    group: int,
    channel: int,
    max_polyphony: int | None = None,
    min_frame_peak_ratio: float | None = None,
    min_ridge_peak_ratio: float | None = None,
    reattack_ratio: float | None = None,
    min_note_division: int | None = None,
    monophonic_model: str | None = None,
) -> SonareTranscribeConfig:
    """Build the transcription config, refusing an out-of-domain field by name.

    ``None`` keeps the default ``sonare_transcribe_config_init`` wrote (NaN or an
    UNSET 0 for the optional fields), so a value the caller passed is always a request: an explicit
    ``velocity_floor_db=0.0`` or ``fixed_velocity=0`` is a mistake rather than
    the default, and is refused here. Whether ``f0_min_hz`` and ``f0_max_hz`` bracket each
    other is left to the core, which is the side that has both resolved
    defaults; passing both wrong way round is named here.
    """
    config = SonareTranscribeConfig()
    config.polyphonic = 1 if polyphonic else 0
    if isinstance(reference_hz, str):
        if reference_hz != "auto":
            raise _refuse("reference_hz must be a number or 'auto'")
        config.reference_auto = 1
        reference_hz = None
    for arg_name, value in (
        ("reference_hz", reference_hz),
        ("f0_min_hz", f0_min_hz),
        ("f0_max_hz", f0_max_hz),
        ("min_note_ms", min_note_ms),
        ("segmentation_threshold_cents", segmentation_threshold_cents),
    ):
        if value is not None:
            setattr(config, arg_name, _transcribe_positive(value, arg_name))
    if f0_min_hz is not None and f0_max_hz is not None and config.f0_max_hz <= config.f0_min_hz:
        raise _refuse("f0_max_hz must be above f0_min_hz")

    if velocity_floor_db is not None:
        level = _validate_scalar(velocity_floor_db, "velocity_floor_db")
        if level >= 0.0:
            raise _refuse("velocity_floor_db must be negative")
        config.velocity_floor_db = level

    # Absence is the C ABI's 0; a typed 0 is refused in the domain the core states.
    config.fixed_velocity = _transcribe_optional(fixed_velocity, "fixed_velocity")
    config.group = _transcribe_int(group, "group", 0, 15)
    config.channel = _transcribe_int(channel, "channel", 0, 15)

    # Polyphonic-only fields: given on the monophonic path, the C ABI refuses
    # them by name, so nothing is pre-checked against `polyphonic` here.
    config.max_polyphony = _transcribe_optional(max_polyphony, "max_polyphony")
    if min_frame_peak_ratio is not None:
        config.min_frame_peak_ratio = _transcribe_ratio(
            min_frame_peak_ratio, "min_frame_peak_ratio"
        )
    if min_ridge_peak_ratio is not None:
        config.min_ridge_peak_ratio = _transcribe_ratio(
            min_ridge_peak_ratio, "min_ridge_peak_ratio"
        )
    if reattack_ratio is not None:
        config.reattack_ratio = _transcribe_reattack(reattack_ratio)

    # Both polyphonic and monophonic read the division; giving it together with
    # min_note_ms is refused by the C ABI, naming min_note_division.
    config.min_note_division = _transcribe_optional(min_note_division, "min_note_division")

    # Given with `polyphonic`, a non-default model is refused by the C ABI,
    # naming the field.
    config.monophonic_model = _transcribe_monophonic_model(monophonic_model)
    return config


def _transcribe_sample_rate(sample_rate: int) -> int:
    """Refuse a non-positive rate here, where the argument still has a name."""
    return _transcribe_int(sample_rate, "sample_rate", 1, _C_INT_MAX)


def _transcribe_result_from_c(out: SonareTranscribeResult) -> TranscribeResult:
    """Copy a transcription out of C memory, before it is released.

    Read length-first: the event pointer is NULL when nothing was found, and a
    ctypes NULL raises on dereference rather than reading zeros.
    """
    events = [_midi_event_from_c(out.events[i]) for i in range(int(out.count))]
    return TranscribeResult(
        events=events,
        note_count=int(out.note_count),
        tempo_bpm=float(out.tempo_bpm),
        tuning_cents=float(out.tuning_cents),
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
    envelope_start = int(row.edit.amplitude_envelope_offset)
    envelope_stop = envelope_start + int(row.edit.amplitude_envelope_count)
    return NoteObject(
        onset_sample=int(row.onset_sample),
        offset_sample=int(row.offset_sample),
        frame_start=int(row.frame_start),
        frame_end=int(row.frame_end),
        median_hz=_pitch_hz_from_c(row.median_hz),
        median_cents=float(row.median_cents),
        f0_stability=float(row.f0_stability),
        amplitude=amplitude[start:stop].copy(),
        edit=NoteEdit(
            time_offset_samples=int(row.edit.time_offset_samples),
            pitch_semitones=float(row.edit.pitch_semitones),
            gain_db=float(row.edit.gain_db),
            time_stretch_ratio=float(row.edit.time_stretch_ratio),
            muted=bool(row.edit.muted),
            formant_semitones=float(row.edit.formant_semitones),
            vibrato_depth_change=float(row.edit.vibrato_depth_change),
            drift_change=float(row.edit.drift_change),
            amplitude_envelope=envelopes[envelope_start:envelope_stop].copy(),
        ),
    )


def _pitch_hz_from_c(value: float) -> float | None:
    """A note's pitch from the C ABI, where 0 spells "no usable pitch"."""
    return float(value) if value > 0.0 else None


def _note_pitch_hz(value: float | None, name: str) -> float:
    """Map a note's absent pitch to the C ABI's 0; refuse a non-positive one by name."""
    if value is None:
        return 0.0
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _refuse_type(f"{name} must be a number or None")
    if not math.isfinite(value) or value <= 0.0:
        raise _refuse(f"{name} must be finite and greater than 0, or None for no pitch")
    return float(value)


def _note_edit_to_c(
    edit: NoteEdit | NoteEditInput, sample_rate: int, subject: str, envelope_offset: int
) -> tuple[SonareNoteEdit, NDArray[np.float32], _SampleBound]:
    """Read either note edit record onto the C struct; the one reader every note door uses.

    Returns the struct, the envelope curve it indexes and the resolved time
    offset, whose respelling the caller hands to :func:`_check`.
    """
    curve = np.asarray(edit.amplitude_envelope, dtype=np.float32)
    if curve.ndim != 1:
        raise _refuse(f"{subject}.amplitude_envelope must be one-dimensional")
    # Narrowed rather than coerced: int(0.5) is 0, which is this field's identity,
    # so a sub-sample shift would render unmoved and report success. Int64-bounded:
    # the ctypes field is c_int64.
    time_offset = _resolve_time_offset(
        0 if edit.time_offset_samples is None else edit.time_offset_samples,
        edit.time_offset_sec if isinstance(edit, NoteEditInput) else None,
        sample_rate,
        f"{subject}.time_offset_samples",
        f"{subject}.time_offset_sec",
    )
    c_edit = SonareNoteEdit(
        time_offset_samples=time_offset.value,
        amplitude_envelope_offset=envelope_offset,
        amplitude_envelope_count=int(curve.size),
        pitch_semitones=_narrow_double(edit.pitch_semitones, f"{subject}.pitch_semitones"),
        gain_db=_narrow_double(edit.gain_db, f"{subject}.gain_db"),
        time_stretch_ratio=_narrow_double(edit.time_stretch_ratio, f"{subject}.time_stretch_ratio"),
        formant_semitones=_narrow_double(edit.formant_semitones, f"{subject}.formant_semitones"),
        vibrato_depth_change=_narrow_double(
            edit.vibrato_depth_change, f"{subject}.vibrato_depth_change"
        ),
        drift_change=_narrow_double(edit.drift_change, f"{subject}.drift_change"),
        muted=1 if edit.muted else 0,
    )
    return c_edit, curve, time_offset


def _note_bound(note: object, name: str) -> tuple[object, object]:
    """A span bound's sample and second spellings; only :class:`NoteObjectInput` has the latter."""
    sample = getattr(note, f"{name}_sample", None)
    sec = getattr(note, f"{name}_sec", None) if isinstance(note, NoteObjectInput) else None
    return sample, sec


def _notes_to_c(
    notes: Sequence[NoteObject | NoteObjectInput | NoteSetEntry],
    sample_rate: int,
    *,
    require_span: bool,
    allow_invalid_span: bool = False,
) -> tuple[object, int, object, int, dict[str, _Respelling]]:
    """Marshal a note list into the C note array plus its shared envelope pool.

    The one reader for every note record: a field a record does not define is
    the C default (0), as on the other surfaces. ``require_span`` is set by
    :func:`render_notes` and
    :func:`assign_note_targets`, which require the sample span. The C ABI
    carries every envelope in one array the edits index into, so the pool is
    packed here and the offsets derived. ``allow_invalid_span`` passes signed
    positions through for an entry whose C contract owns their domain. The last
    element is the respelling of bounds given in seconds, for :func:`_check`.
    """
    note_count = len(notes)
    if note_count == 0:
        return None, 0, None, 0, {}
    respelled: dict[str, _Respelling] = {}

    c_notes = (SonareNoteObject * note_count)()
    curves: list[NDArray[np.float32]] = []
    envelope_offset = 0
    for i, note in enumerate(notes):
        # The metrics and the amplitude offset are not marshalled: nothing on
        # the far side reads them back.
        onset = _resolve_sample_bound(
            *_note_bound(note, "onset"),
            sample_rate,
            f"notes[{i}].onset_sample",
            f"notes[{i}].onset_sec",
            signed=allow_invalid_span,
        )
        offset = _resolve_sample_bound(
            *_note_bound(note, "offset"),
            sample_rate,
            f"notes[{i}].offset_sample",
            f"notes[{i}].offset_sec",
            signed=allow_invalid_span,
        )
        if require_span and (onset.value is None or offset.value is None):
            missing = "onset" if onset.value is None else "offset"
            raise _refuse(f"notes[{i}] needs {missing}_sample or {missing}_sec")
        c_notes[i].onset_sample = 0 if onset.value is None else onset.value
        c_notes[i].offset_sample = 0 if offset.value is None else offset.value
        for field in ("frame_start", "frame_end"):
            value = getattr(note, field)
            setattr(
                c_notes[i],
                field,
                0
                if value is None
                else _narrow_int(value, f"notes[{i}].{field}", _C_INT_MIN, _C_INT_MAX),
            )
        c_notes[i].median_hz = _note_pitch_hz(
            getattr(note, "median_hz", None), f"notes[{i}].median_hz"
        )
        c_edit, curve, time_offset = _note_edit_to_c(
            note.edit, sample_rate, f"notes[{i}].edit", envelope_offset
        )
        c_notes[i].edit = c_edit
        curves.append(curve)
        respelled.update(_respellings(onset, offset, time_offset))
        envelope_offset += int(curve.size)

    if envelope_offset == 0:
        return c_notes, note_count, None, 0, respelled
    pool, pool_count = _to_c_float_array(np.concatenate(curves))
    return c_notes, note_count, pool, pool_count, respelled


def _note_set_index(arg_name: str, value: int) -> int:
    """Reject a negative note index before ``size_t`` wraps it into a huge one.

    The sign half only. Every call site hands the result to :func:`_to_c_size_t`,
    which carries the ceiling -- an index past it folds onto a live note.
    """
    if isinstance(value, bool) or not isinstance(value, Integral):
        message = f"{arg_name} must be an integer"
        raise _refuse_integer(arg_name, message) if _is_number(value) else _refuse_type(message)
    if int(value) < 0:
        raise _refuse_integer(arg_name, f"{arg_name} must be an integer in [0, {_SIZE_T_MAX}]")
    return int(value)


def _note_set_edit(
    symbol: str,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject | NoteSetEntry],
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
    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz, arg_name="f0_hz")
    prob_array, voiced_array = _note_voicing_arrays(n_frames, voiced, voiced_prob)
    c_notes, note_count, envelopes, envelope_count, respelled = _notes_to_c(
        notes, sample_rate, require_span=False
    )

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
    _check(rc, flattened="config", respelled=respelled)
    try:
        return _notes_from_c(out)
    finally:
        lib.sonare_free_note_objects_result(ctypes.byref(out))
