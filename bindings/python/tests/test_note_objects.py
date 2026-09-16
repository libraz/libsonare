"""Tests for the note-object Python facade.

``extract_notes`` / ``render_notes`` / ``decompose_note_pitch`` / ``split_note``
/ ``merge_notes`` are a ctypes pass-through over the C ABI of the same names.
The F0 track is caller-supplied, so these cases hand the extractor a synthetic
contour rather than a detector's output: what is under test is the marshalling
and the edit semantics, not the segmenter (which has its own coverage).

The C ABI carries every note's amplitude envelope in one pool the edits index
into; this facade gives each :class:`NoteEdit` its own array and packs the pool
itself, so the cases that would exercise a bad offset on the C side are here
cases that the packing put each note's curve on the right note.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import TypeAlias

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE, sine

# Audio, its F0 track and its voiced flags; the three arguments every note-set
# entry point takes together.
Source: TypeAlias = tuple[NDArray[np.float32], NDArray[np.float32], NDArray[np.int32]]
SourceWithNotes: TypeAlias = tuple[
    NDArray[np.float32], NDArray[np.float32], NDArray[np.int32], list[libsonare.NoteObject]
]

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
HOP = 512
FRAME_RATE = SR / HOP
NOTE_SECONDS = 0.4
# A3 / C#4 / E4: a semitone apart at the closest, far outside the 50-cent
# default segmentation threshold.
NOTE_HZ = (220.0, 277.18, 329.63)

# Skip a margin wider than the default 5 ms edge cross-fade when comparing spans.
FADE_MARGIN = int(0.01 * SR)

CENTS_PER_OCTAVE = 1200.0
# Ten whole periods per hop, so every frame of the stepped tone below carries
# exactly its own amplitude over root two and no two frames read alike.
STEP_HZ = FRAME_RATE * 10.0


def _melody() -> tuple[NDArray[np.float32], NDArray[np.float32], NDArray[np.int32]]:
    """A three-note arpeggio plus the F0 track and voiced flags describing it."""
    audio = np.concatenate([sine(hz, NOTE_SECONDS, SR, amp=0.4) for hz in NOTE_HZ])
    n_frames = len(audio) // HOP
    per_note = n_frames // len(NOTE_HZ)
    f0_hz = np.full(n_frames, NOTE_HZ[-1], dtype=np.float32)
    for i, hz in enumerate(NOTE_HZ):
        f0_hz[i * per_note : (i + 1) * per_note] = hz
    return audio, f0_hz, np.ones(n_frames, dtype=np.int32)


def _extract() -> tuple[NDArray[np.float32], list[libsonare.NoteObject]]:
    audio, f0_hz, voiced = _melody()
    notes = libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced)
    # The track is a step function on frame boundaries, so the segmentation is
    # exact: one note per pitch, 17 frames of 512 samples each.
    assert [(n.frame_start, n.frame_end) for n in notes] == [(0, 17), (17, 34), (34, 51)]
    assert [(n.onset_sample, n.offset_sample) for n in notes] == [
        (0, 8704),
        (8704, 17408),
        (17408, 26112),
    ]
    return audio, notes


def _rms(samples: NDArray[np.float32]) -> float:
    return float(np.sqrt(np.mean(np.asarray(samples, dtype=np.float64) ** 2)))


def _fm_tone(
    centre_hz: float, depth_cents: float, rate_hz: float, n_samples: int
) -> NDArray[np.float32]:
    """A sine whose pitch swings ``depth_cents`` either side of ``centre_hz``."""
    t = np.arange(n_samples) / SR
    hz = centre_hz * 2.0 ** (depth_cents * np.sin(2 * np.pi * rate_hz * t) / CENTS_PER_OCTAVE)
    phase = np.concatenate([[0.0], np.cumsum(2 * np.pi * hz / SR)[:-1]])
    return (0.4 * np.sin(phase)).astype(np.float32)


def _injected_f0(
    centre_hz: float, n_frames: int, components: Sequence[tuple[float, float]]
) -> NDArray[np.float32]:
    """An F0 curve whose cents above ``centre_hz`` are exactly the components' sum."""
    t = np.arange(n_frames) / FRAME_RATE
    cents = np.zeros(n_frames, dtype=np.float64)
    for hz, depth_cents in components:
        cents += depth_cents * np.sin(2 * np.pi * hz * t)
    return (centre_hz * 2.0 ** (cents / CENTS_PER_OCTAVE)).astype(np.float32)


def _cents_above(f0_hz: NDArray[np.float32], centre_hz: float) -> NDArray[np.float64]:
    return CENTS_PER_OCTAVE * np.log2(np.asarray(f0_hz, dtype=np.float64) / centre_hz)


# A vibrato fast enough to sit above the default 3 Hz split and slow enough to
# sit below an 8 Hz one, which is what tells the two curves apart.
VIBRATO_CENTRE_HZ = 220.0
VIBRATO_DEPTH_CENTS = 30.0
VIBRATO_RATE_HZ = 5.5


def _vibrato_source(n_frames: int = 34) -> Source:
    """An FM tone plus the F0 track describing it, frame-aligned to the hop."""
    audio = _fm_tone(VIBRATO_CENTRE_HZ, VIBRATO_DEPTH_CENTS, VIBRATO_RATE_HZ, n_frames * HOP)
    f0_hz = _injected_f0(VIBRATO_CENTRE_HZ, n_frames, [(VIBRATO_RATE_HZ, VIBRATO_DEPTH_CENTS)])
    return audio, f0_hz, np.ones(n_frames, dtype=np.int32)


def _vibrato_notes() -> SourceWithNotes:
    audio, f0_hz, voiced = _vibrato_source()
    notes = libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced)
    # The excursion stays inside the 50-cent segmentation threshold, so this is
    # one note. Asserted rather than tolerated: the cutoff case below needs a
    # span long enough for a 3 Hz filter, which a broken-up track would not give.
    assert len(notes) == 1
    return audio, f0_hz, voiced, notes


def _stepped_tone(n_frames: int) -> NDArray[np.float32]:
    """A tone whose amplitude steps up once per frame.

    Every frame's RMS is therefore its own, which is what makes a misaligned
    amplitude slice visible: on a flat tone a slice pointing at a neighbour
    reads correct.
    """
    i = np.arange(n_frames * HOP)
    amplitude = 0.1 + 0.02 * (i // HOP)
    return (amplitude * np.sin(2 * np.pi * STEP_HZ * i / SR)).astype(np.float32)


def _plain_source(n_frames: int = 24) -> Source:
    """One voiced run over the whole track."""
    return (
        _stepped_tone(n_frames),
        np.full(n_frames, STEP_HZ, dtype=np.float32),
        np.ones(n_frames, dtype=np.int32),
    )


def _gapped_source(n_frames: int = 24) -> Source:
    """Two unvoiced gaps, so the segmenter emits three notes with a gap between."""
    audio, f0_hz, voiced = _plain_source(n_frames)
    for lo, hi in ((8, 10), (17, 19)):
        f0_hz[lo:hi] = 0.0
        voiced[lo:hi] = 0
    return audio, f0_hz, voiced


def _gapped_notes() -> SourceWithNotes:
    audio, f0_hz, voiced = _gapped_source()
    notes = libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced)
    assert [(n.frame_start, n.frame_end) for n in notes] == [(0, 8), (10, 17), (19, 24)]
    return audio, f0_hz, voiced, notes


def _plain_notes() -> SourceWithNotes:
    audio, f0_hz, voiced = _plain_source()
    notes = libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced)
    assert [(n.frame_start, n.frame_end) for n in notes] == [(0, 24)]
    return audio, f0_hz, voiced, notes


def _assert_edited(out: NDArray[np.float32], source: NDArray[np.float32]) -> None:
    """Assert the edit moved the output off ``source`` and left a signal behind.

    A bare difference check is a one-sided bound: silence differs from the
    source by the source's own peak and so passes one, and so does a blow-up.
    The amplitude band is what rules those two out.
    """
    assert float(np.max(np.abs(out - source))) > 0.05
    source_peak = float(np.max(np.abs(source)))
    out_peak = float(np.max(np.abs(out)))
    assert 0.5 * source_peak < out_peak < 2.0 * source_peak


def test_extracted_notes_describe_the_contour() -> None:
    _, notes = _extract()
    previous_end = -1
    for note in notes:
        assert note.onset_sample >= previous_end
        assert note.offset_sample > note.onset_sample
        previous_end = note.offset_sample
        assert note.frame_end > note.frame_start
        # Every span holds one constant pitch, so the metric is at its ceiling.
        assert note.f0_stability == 1.0
        # Every span was fed one of the three constant pitches.
        assert min(abs(note.median_hz - hz) for hz in NOTE_HZ) < 1.0
        assert note.edit == libsonare.NoteEdit(), "a freshly extracted note carries no edit"


def test_f0_stability_falls_below_its_ceiling_on_a_vibrato() -> None:
    # A steady span reads 1.0, which every float field would read if the
    # marshalling were wired to the wrong one. A vibrato pins a value only this
    # field can produce.
    audio = sine(220.0, 0.8, SR, amp=0.4)
    n_frames = len(audio) // HOP
    # +-30 cents at 5 Hz: inside the 50-cent threshold, so the span stays one note.
    cents = 30.0 * np.sin(2 * np.pi * 5.0 * np.arange(n_frames) / FRAME_RATE)
    f0_hz = (220.0 * 2 ** (cents / 1200.0)).astype(np.float32)
    voiced = np.ones(n_frames, dtype=np.int32)

    notes = libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced)
    assert len(notes) == 1
    assert 0.0 < notes[0].f0_stability < 0.9


def test_each_note_carries_its_own_amplitude_slice() -> None:
    _, notes = _extract()
    for note in notes:
        assert note.amplitude.dtype == np.float32
        assert len(note.amplitude) == note.frame_end - note.frame_start
        assert np.all(np.isfinite(note.amplitude))
        assert float(np.max(note.amplitude)) > 0.0

    # The slices must be independent buffers, not views into one array the C
    # side owned: writing to one may not reach another, and none of them may
    # outlive `sonare_free_note_objects` as a view into freed memory.
    first = notes[0].amplitude.copy()
    notes[-1].amplitude[:] = 0.0
    np.testing.assert_array_equal(notes[0].amplitude, first)


def test_identity_edits_reproduce_the_input_exactly() -> None:
    audio, notes = _extract()
    rendered = libsonare.render_notes(audio, SR, notes)
    assert rendered.shape == audio.shape
    np.testing.assert_array_equal(rendered, audio)


def test_no_notes_reproduces_the_input_exactly() -> None:
    audio, _, _ = _melody()
    np.testing.assert_array_equal(libsonare.render_notes(audio, SR, []), audio)


def test_gain_edit_changes_only_that_notes_span() -> None:
    audio, notes = _extract()
    baseline = libsonare.render_notes(audio, SR, notes)

    notes[0].edit.gain_db = -20.0
    edited = libsonare.render_notes(audio, SR, notes)

    start = notes[0].onset_sample + FADE_MARGIN
    stop = notes[0].offset_sample - FADE_MARGIN
    assert _rms(edited[start:stop]) < 0.25 * _rms(baseline[start:stop])

    other = notes[-1]
    np.testing.assert_array_equal(
        edited[other.onset_sample + FADE_MARGIN : other.offset_sample - FADE_MARGIN],
        baseline[other.onset_sample + FADE_MARGIN : other.offset_sample - FADE_MARGIN],
    )


def test_muted_note_silences_its_span() -> None:
    audio, notes = _extract()
    baseline = libsonare.render_notes(audio, SR, notes)

    notes[0].edit.muted = True
    edited = libsonare.render_notes(audio, SR, notes)

    start = notes[0].onset_sample + FADE_MARGIN
    stop = notes[0].offset_sample - FADE_MARGIN
    assert _rms(edited[start:stop]) < 1e-4

    other = notes[-1]
    np.testing.assert_array_equal(
        edited[other.onset_sample + FADE_MARGIN : other.offset_sample - FADE_MARGIN],
        baseline[other.onset_sample + FADE_MARGIN : other.offset_sample - FADE_MARGIN],
    )


def test_hand_built_note_needs_only_a_span_and_an_edit() -> None:
    # render_notes reads onset_sample, offset_sample and edit only, so a host
    # that found a span some other way can build the note itself.
    audio, _, _ = _melody()
    span = libsonare.NoteObject(onset_sample=0, offset_sample=len(audio) // 2)
    span.edit.muted = True
    rendered = libsonare.render_notes(audio, SR, [span])
    assert _rms(rendered[FADE_MARGIN : len(audio) // 2 - FADE_MARGIN]) < 1e-4
    np.testing.assert_array_equal(
        rendered[len(audio) // 2 + FADE_MARGIN :], audio[len(audio) // 2 + FADE_MARGIN :]
    )


def test_a_hand_built_note_has_to_state_its_span() -> None:
    # The span has no default, so a note built without one does not exist to be
    # rendered. A default of 0 made the two bounds equal, and a zero-length span
    # renders as nothing: the edit below would have been dropped in silence
    # while the call reported success, which is what the sibling surfaces reject.
    #
    # The refusal arrives in a different currency here, deliberately: the bounds
    # are declared fields with no default, so a note missing one cannot be built
    # at all and the answer is a TypeError from the dataclass rather than a
    # library error from the render. Node and WASM read a plain object at the
    # call and answer with a SonareError carrying InvalidParameter. Both name the
    # field; only Python refuses before the call exists.
    #
    # There is no split/merge asymmetry to record on this surface either:
    # split_note and merge_notes take Sequence[NoteObject], the same type, where
    # Node and WASM have a separate NoteSetEntry that omits the bounds on
    # purpose. So a Python note always carries a span, everywhere.
    audio, _, _ = _melody()
    # One field at a time as well as both, so the message has to name the field
    # that is actually missing rather than merely mentioning the constructor.
    for kwargs, missing in (
        ({}, ("onset_sample", "offset_sample")),
        ({"onset_sample": 0}, ("offset_sample",)),
        ({"offset_sample": len(audio) // 2}, ("onset_sample",)),
    ):
        with pytest.raises(TypeError) as raised:
            libsonare.NoteObject(**kwargs)  # type: ignore[call-arg]
        for field in missing:
            assert field in str(raised.value)
        # And the field that WAS given is not reported missing.
        for given in kwargs:
            assert f"'{given}'" not in str(raised.value)

    stated = libsonare.NoteObject(onset_sample=0, offset_sample=len(audio) // 2)
    stated.edit.gain_db = -60.0
    rendered = libsonare.render_notes(audio, SR, [stated])
    # The control: the span that IS stated reaches the render and changes it.
    assert _rms(rendered[FADE_MARGIN : len(audio) // 2 - FADE_MARGIN]) < _rms(
        audio[FADE_MARGIN : len(audio) // 2 - FADE_MARGIN]
    )


def test_unvoiced_track_segments_into_no_notes() -> None:
    audio = sine(220.0, 0.5, SR, amp=0.4)
    n_frames = len(audio) // HOP
    f0_hz = np.zeros(n_frames, dtype=np.float32)
    voiced = np.zeros(n_frames, dtype=np.int32)
    assert libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced) == []


def test_voiced_prob_selects_voicing_when_no_flags_are_given() -> None:
    audio, f0_hz, voiced = _melody()
    by_flags = libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced)
    by_prob = libsonare.extract_notes(
        audio,
        SR,
        f0_hz,
        FRAME_RATE,
        voiced_prob=np.ones(len(f0_hz), dtype=np.float32),
    )
    assert [(n.onset_sample, n.offset_sample) for n in by_prob] == [
        (n.onset_sample, n.offset_sample) for n in by_flags
    ]


def test_extract_notes_rejects_invalid_arguments() -> None:
    audio, f0_hz, voiced = _melody()
    with pytest.raises(SonareValueError, match="voiced"):
        libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE)
    with pytest.raises(SonareValueError):
        libsonare.extract_notes(audio, SR, f0_hz, FRAME_RATE, voiced=voiced[:-1])
    with pytest.raises(SonareValueError):
        libsonare.extract_notes(
            audio, SR, f0_hz, FRAME_RATE, voiced_prob=np.ones(3, dtype=np.float32)
        )
    with pytest.raises(SonareValueError, match="extract_notes"):
        libsonare.extract_notes(np.zeros(0, dtype=np.float32), SR, f0_hz, FRAME_RATE, voiced=voiced)
    with pytest.raises(SonareValueError, match="extract_notes"):
        libsonare.extract_notes(
            np.full(len(audio), np.nan, dtype=np.float32), SR, f0_hz, FRAME_RATE, voiced=voiced
        )
    with pytest.raises(SonareError):
        libsonare.extract_notes(audio, SR, f0_hz, 0.0, voiced=voiced)


def test_render_notes_rejects_invalid_arguments() -> None:
    audio, notes = _extract()
    with pytest.raises(SonareValueError, match="render_notes"):
        libsonare.render_notes(np.zeros(0, dtype=np.float32), SR, notes)
    with pytest.raises(SonareError):
        libsonare.render_notes(audio, SR, notes, fade_ms=-1.0)


# --- The edit fields added on top of the P1 four ----------------------------


def test_note_edit_equality_sees_the_amplitude_envelope() -> None:
    # NoteEdit() is the documented identity edit, so an edit carrying a curve
    # must not compare equal to one -- which the generated __eq__ would, since
    # an ndarray field has to sit outside it.
    identity = libsonare.NoteEdit()
    carrying = libsonare.NoteEdit()
    assert identity == carrying
    carrying.amplitude_envelope = np.array([0.5], dtype=np.float32)
    assert identity != carrying
    identity.amplitude_envelope = np.array([0.5], dtype=np.float32)
    assert identity == carrying


def test_a_default_edit_is_still_the_identity_once_it_carries_the_new_fields() -> None:
    audio, f0_hz, _, notes = _vibrato_notes()
    for note in notes:
        assert note.edit == libsonare.NoteEdit()
        assert note.edit.formant_shift_semitones == 0.0
        assert note.edit.vibrato_depth_change == 0.0
        assert note.edit.drift_change == 0.0
        assert note.edit.amplitude_envelope.size == 0

    # The track is handed in, so the identity has to survive the path that reads
    # it rather than only the one that never looks.
    rendered = libsonare.render_notes(audio, SR, notes, f0_hz=f0_hz, frame_rate=FRAME_RATE)
    np.testing.assert_array_equal(rendered, audio)

    # Non-vacuity, one new field at a time: each moves the output off the source,
    # so the equality above is the identity rather than an edit nobody read.
    def moves_the_output(field: str, value: object) -> None:
        for note in notes:
            note.edit = libsonare.NoteEdit()
            setattr(note.edit, field, value)
        edited = libsonare.render_notes(audio, SR, notes, f0_hz=f0_hz, frame_rate=FRAME_RATE)
        _assert_edited(edited, audio)

    moves_the_output("formant_shift_semitones", 2.0)
    moves_the_output("vibrato_depth_change", -1.0)
    moves_the_output("drift_change", 1.0)
    moves_the_output("amplitude_envelope", np.array([0.25, 1.0], dtype=np.float32))


def test_a_constant_amplitude_envelope_matches_the_same_gain_in_db() -> None:
    # Both spellings scale the span by the same factor, so this pins the
    # envelope's value rather than only that something changed.
    audio, notes = _extract()
    start = notes[0].onset_sample + FADE_MARGIN
    stop = notes[0].offset_sample - FADE_MARGIN

    notes[0].edit.amplitude_envelope = np.array([0.5, 0.5], dtype=np.float32)
    by_envelope = libsonare.render_notes(audio, SR, notes)

    notes[0].edit.amplitude_envelope = np.empty(0, dtype=np.float32)
    notes[0].edit.gain_db = -20.0 * float(np.log10(2.0))
    by_gain = libsonare.render_notes(audio, SR, notes)

    assert _rms(by_envelope[start:stop]) == pytest.approx(_rms(by_gain[start:stop]), rel=1e-3)
    # And both really are half the source, so the agreement is not two identical
    # no-ops agreeing with each other.
    assert _rms(by_gain[start:stop]) == pytest.approx(0.5 * _rms(audio[start:stop]), rel=1e-3)


def test_each_note_carries_its_own_amplitude_envelope() -> None:
    # Note 0 ramps, note 1 is a constant and note 2 carries none, so a packing
    # that put a curve on the wrong note shows up as the wrong shape.
    audio, notes = _extract()
    notes[0].edit.amplitude_envelope = np.array([0.25, 1.0], dtype=np.float32)
    notes[1].edit.amplitude_envelope = np.array([0.5], dtype=np.float32)
    rendered = libsonare.render_notes(audio, SR, notes)

    def quarter_ratio(note: libsonare.NoteObject, quarter: int) -> float:
        start = note.onset_sample + FADE_MARGIN
        span = (note.offset_sample - FADE_MARGIN) - start
        lo = start + span * quarter // 4
        hi = start + span * (quarter + 1) // 4
        return _rms(rendered[lo:hi]) / _rms(audio[lo:hi])

    ramp = [quarter_ratio(notes[0], q) for q in range(4)]
    assert all(lower < upper for lower, upper in zip(ramp, ramp[1:], strict=False))
    # A 0.25 -> 1.0 ramp averages about 0.34 over the first quarter and about
    # 0.91 over the last, so these bounds hold without pinning the interpolation.
    assert ramp[0] < 0.5
    assert ramp[-1] > 0.8

    # One entry is a constant gain, flat across the whole span rather than a ramp.
    for quarter in range(4):
        assert quarter_ratio(notes[1], quarter) == pytest.approx(0.5, rel=0.05)

    # The third note's edit stays the identity, so its span is untouched.
    other = notes[2]
    np.testing.assert_array_equal(
        rendered[other.onset_sample + FADE_MARGIN : other.offset_sample - FADE_MARGIN],
        audio[other.onset_sample + FADE_MARGIN : other.offset_sample - FADE_MARGIN],
    )


def test_render_notes_rejects_an_unusable_amplitude_envelope() -> None:
    audio, notes = _extract()
    for bad in (np.nan, np.inf, -np.inf, -1.0):
        notes[0].edit.amplitude_envelope = np.array([bad, 1.0], dtype=np.float32)
        with pytest.raises(SonareError):
            libsonare.render_notes(audio, SR, notes)

    notes[0].edit.amplitude_envelope = np.zeros((2, 2), dtype=np.float32)
    with pytest.raises(SonareValueError, match="amplitude_envelope"):
        libsonare.render_notes(audio, SR, notes)

    # Positive control: the same note with a usable curve renders.
    notes[0].edit.amplitude_envelope = np.array([0.25, 1.0], dtype=np.float32)
    assert libsonare.render_notes(audio, SR, notes).shape == audio.shape


# --- Curve edits and the track they act on ----------------------------------


def test_render_notes_requires_frame_rate_alongside_f0_hz() -> None:
    audio, f0_hz, _, notes = _vibrato_notes()
    with pytest.raises(SonareValueError, match="frame_rate"):
        libsonare.render_notes(audio, SR, notes, f0_hz=f0_hz)


def test_a_curve_edit_is_rejected_without_a_track_and_applied_with_one() -> None:
    audio, f0_hz, _, notes = _vibrato_notes()
    for field, value in (("vibrato_depth_change", -1.0), ("drift_change", 1.0)):
        for note in notes:
            note.edit = libsonare.NoteEdit()
            setattr(note.edit, field, value)

        # The curve the edit acts on is the caller's own track, and there is none.
        with pytest.raises(SonareError):
            libsonare.render_notes(audio, SR, notes)

        # Its companion: the same edit with the track renders and moves the
        # audio, which is what makes the rejection about the track rather than
        # about the field.
        edited = libsonare.render_notes(audio, SR, notes, f0_hz=f0_hz, frame_rate=FRAME_RATE)
        _assert_edited(edited, audio)


def test_the_vibrato_cutoff_decides_which_curve_a_curve_edit_acts_on() -> None:
    audio, f0_hz, _, notes = _vibrato_notes()
    for note in notes:
        note.edit.vibrato_depth_change = -1.0

    def rendered(cutoff_hz: float | None) -> NDArray[np.float32]:
        return libsonare.render_notes(
            audio, SR, notes, f0_hz=f0_hz, frame_rate=FRAME_RATE, vibrato_cutoff_hz=cutoff_hz
        )

    default = rendered(None)
    # None and an explicit 3 both select the library default.
    np.testing.assert_array_equal(rendered(3.0), default)

    # The 5.5 Hz swing sits above a 3 Hz cut and below an 8 Hz one, so a cutoff
    # nobody read would render these two the same.
    wide = rendered(8.0)
    _assert_edited(wide, default)

    # And in the direction the filter dictates: at 3 Hz the swing is vibrato and
    # flattening takes it, at 8 Hz it is mostly drift and survives, so the
    # default render is the one that moved further from the source. Wired
    # backwards this inverts rather than merely shrinking.
    assert float(np.max(np.abs(audio - default))) > 1.2 * float(np.max(np.abs(audio - wide)))


# --- decompose_note_pitch ---------------------------------------------------


def test_decompose_note_pitch_splits_a_curve_into_two_parts_that_add_back_up() -> None:
    centre_hz = 196.0
    n_frames = 400
    f0_hz = _injected_f0(centre_hz, n_frames, [(0.5, 60.0), (5.5, 40.0)])

    curve = libsonare.decompose_note_pitch(f0_hz, FRAME_RATE, centre_hz)
    assert curve.centre_hz == pytest.approx(centre_hz)
    assert curve.drift_cents.shape == (n_frames,)
    assert curve.vibrato_cents.shape == (n_frames,)
    assert curve.drift_cents.dtype == np.float32

    total = curve.drift_cents.astype(np.float64) + curve.vibrato_cents.astype(np.float64)
    assert float(np.max(np.abs(total - _cents_above(f0_hz, centre_hz)))) < 1e-3

    # Two zero curves satisfy the sum as well, so both have to carry something.
    # 0.5 Hz and 5.5 Hz sit either side of the 3 Hz cut, so each does.
    assert float(np.max(np.abs(curve.drift_cents))) > 10.0
    assert float(np.max(np.abs(curve.vibrato_cents))) > 10.0


def test_the_decompose_note_pitch_cutoff_takes_its_default_at_none() -> None:
    centre_hz = 220.0
    f0_hz = _injected_f0(centre_hz, 400, [(0.5, 60.0), (5.5, 40.0)])

    def both_curves(cutoff_hz: float | None) -> NDArray[np.float32]:
        curve = libsonare.decompose_note_pitch(
            f0_hz, FRAME_RATE, centre_hz, vibrato_cutoff_hz=cutoff_hz
        )
        return np.concatenate([curve.drift_cents, curve.vibrato_cents])

    explicit_default = both_curves(3.0)
    np.testing.assert_array_equal(both_curves(None), explicit_default)
    # 0 is the C ABI's spelling of the default and reaches it unchanged.
    np.testing.assert_array_equal(both_curves(0.0), explicit_default)

    # Non-vacuity: the cutoff does decide the split, so the equalities above are
    # the default being applied rather than an argument nobody reads.
    assert not np.array_equal(both_curves(8.0), explicit_default)


def test_decompose_note_pitch_reports_a_note_with_no_usable_pitch_as_empty() -> None:
    centre_hz = 220.0
    n_frames = 200
    f0_hz = _injected_f0(centre_hz, n_frames, [(5.0, 30.0)])

    # A measurement that came up empty is not a bad argument, so it is reported
    # rather than raised: no usable frame, and no centre to measure against.
    for curve_in, median_hz in ((np.zeros(n_frames, dtype=np.float32), centre_hz), (f0_hz, 0.0)):
        curve = libsonare.decompose_note_pitch(curve_in, FRAME_RATE, median_hz)
        assert curve.centre_hz == 0.0
        assert curve.drift_cents.size == 0
        assert curve.vibrato_cents.size == 0

    # The same curve with a centre is not an empty measurement, so neither of the
    # two above passes by emptying every call.
    usable = libsonare.decompose_note_pitch(f0_hz, FRAME_RATE, centre_hz)
    assert usable.centre_hz == pytest.approx(centre_hz)
    assert usable.drift_cents.size == n_frames


def test_decompose_note_pitch_rejects_invalid_arguments() -> None:
    centre_hz = 220.0
    n_frames = 200
    f0_hz = _injected_f0(centre_hz, n_frames, [(5.0, 30.0)])

    with pytest.raises(SonareValueError, match="decompose_note_pitch"):
        libsonare.decompose_note_pitch(np.zeros(0, dtype=np.float32), FRAME_RATE, centre_hz)

    # An all-NaN contour is a note with no usable pitch, which the header says
    # comes back as a zero centre and two empty curves rather than as an error.
    unmeasured = libsonare.decompose_note_pitch(
        np.full(n_frames, np.nan, dtype=np.float32), FRAME_RATE, centre_hz
    )
    assert unmeasured.centre_hz == 0.0
    assert len(unmeasured.drift_cents) == 0
    assert len(unmeasured.vibrato_cents) == 0

    # A negative frame is the same statement as a NaN one -- this frame carries
    # no pitch -- so it is read rather than refused, and the note still resolves
    # around the frames that do carry one.
    negative = f0_hz.copy()
    negative[7] = -1.0
    assert libsonare.decompose_note_pitch(negative, FRAME_RATE, centre_hz).centre_hz > 0.0

    # The frame rate and the centre below are still refused, so the acceptances
    # above are the contour's values being read and not a dropped check.
    for frame_rate in (0.0, -100.0, np.nan, np.inf):
        with pytest.raises(SonareError):
            libsonare.decompose_note_pitch(f0_hz, frame_rate, centre_hz)

    # 0 is the no-pitch spelling, so only a value that cannot be a centre at all
    # is rejected.
    for median_hz in (-1.0, np.nan, np.inf, -np.inf):
        with pytest.raises(SonareError):
            libsonare.decompose_note_pitch(f0_hz, FRAME_RATE, median_hz)

    # None is the default spelling, so only a value that cannot be a cutoff is.
    for cutoff_hz in (-1.0, np.nan, np.inf, -np.inf):
        with pytest.raises(SonareError):
            libsonare.decompose_note_pitch(
                f0_hz, FRAME_RATE, centre_hz, vibrato_cutoff_hz=cutoff_hz
            )

    # Positive control: the same call with nothing poisoned succeeds.
    assert libsonare.decompose_note_pitch(f0_hz, FRAME_RATE, centre_hz).drift_cents.size == n_frames


# --- split_note -------------------------------------------------------------


def test_split_note_keeps_every_notes_amplitude_slice_on_its_own_frames() -> None:
    audio, f0_hz, voiced, notes = _gapped_notes()
    before = [note.amplitude.copy() for note in notes]

    # No two entries agree, so a slice compared against the wrong one cannot
    # pass; the companion every value comparison below needs.
    sorted_all = np.sort(np.concatenate(before))
    assert float(np.min(np.diff(sorted_all))) > 1e-4

    split = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, 13, voiced=voiced)
    assert len(split) == 4

    # The cut lands where it was asked for and the spans stay contiguous.
    assert (split[1].frame_start, split[1].frame_end) == (10, 13)
    assert (split[2].frame_start, split[2].frame_end) == (13, 17)
    assert split[1].offset_sample == split[2].onset_sample
    assert split[1].onset_sample == notes[1].onset_sample
    assert split[2].offset_sample == notes[1].offset_sample

    # Every curve, the untouched notes' included, holds its own values. Offsets
    # that look right while pointing at a neighbour's data only the values catch.
    np.testing.assert_allclose(split[0].amplitude, before[0], atol=1e-6)
    np.testing.assert_allclose(split[3].amplitude, before[2], atol=1e-6)
    np.testing.assert_allclose(
        np.concatenate([split[1].amplitude, split[2].amplitude]), before[1], atol=1e-6
    )

    # The untouched notes are re-derived rather than copied, and come back equal.
    assert split[0] == notes[0]
    assert split[3] == notes[2]

    # No note carried an envelope in, so none comes back carrying one.
    assert all(note.edit.amplitude_envelope.size == 0 for note in split)


def test_split_note_cuts_the_source_notes_envelope_at_the_same_proportion() -> None:
    audio, f0_hz, voiced, notes = _plain_notes()
    assert (notes[0].frame_start, notes[0].frame_end) == (0, 24)

    notes[0].edit.amplitude_envelope = np.array([0.25, 1.0], dtype=np.float32)
    split = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 0, 12, voiced=voiced)
    assert len(split) == 2

    # Exact, not approximate. The cut is at (12 - 0) / (24 - 0) = 0.5 of a
    # two-point envelope, so the split anchors the endpoints and inserts one
    # interpolated point at 0.25 * 0.5 + 1.0 * 0.5 = 0.625; nothing is resampled
    # and every value involved is exact in binary32.
    assert split[0].edit.amplitude_envelope.tolist() == [0.25, 0.625]
    assert split[1].edit.amplitude_envelope.tolist() == [0.625, 1.0]


def test_a_one_entry_envelope_is_a_constant_so_both_halves_keep_it() -> None:
    audio, f0_hz, voiced, notes = _plain_notes()
    notes[0].edit.amplitude_envelope = np.array([0.5], dtype=np.float32)
    split = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 0, 12, voiced=voiced)
    assert len(split) == 2
    for half in split:
        assert half.edit.amplitude_envelope.tolist() == [0.5]


def test_split_note_normalizes_the_identity_edit_and_still_renders_bit_for_bit() -> None:
    audio, f0_hz, voiced, notes = _plain_notes()
    split = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 0, 12, voiced=voiced)
    assert len(split) == 2
    for half in split:
        assert half.edit == libsonare.NoteEdit()
        assert half.edit.time_stretch_ratio == 1.0
    np.testing.assert_array_equal(libsonare.render_notes(audio, SR, split), audio)


def test_split_note_rejects_an_out_of_range_index_and_a_frame_outside_the_note() -> None:
    audio, f0_hz, voiced, notes = _gapped_notes()

    for index in (len(notes), len(notes) + 4):
        with pytest.raises(SonareError):
            libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, index, 13, voiced=voiced)
    with pytest.raises(SonareValueError, match="index"):
        libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, -1, 13, voiced=voiced)

    # Strictly inside the note's own span, so neither of its boundaries is a
    # legal cut and neither is a frame belonging to another note.
    for frame in (10, 17, 5, 20, -1, 100):
        with pytest.raises(SonareError):
            libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, frame, voiced=voiced)

    with pytest.raises(SonareValueError, match="voiced"):
        libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, 13)
    with pytest.raises(SonareValueError):
        libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, 13, voiced=voiced[:-1])
    with pytest.raises(SonareError):
        libsonare.split_note(audio, SR, f0_hz, 0.0, notes, 1, 13, voiced=voiced)

    # Positive controls: one frame in from either end of the note is legal, and
    # both halves keep a span.
    for frame in (11, 16):
        split = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, frame, voiced=voiced)
        assert len(split) == 4
        assert split[1].frame_end == frame
        assert split[2].frame_start == frame


# Note indices past the largest ``size_t``, each with the index the C conversion
# folds it to. The fold is what makes these the interesting inputs rather than a
# merely huge index: every one lands on a live note in the three-note set above,
# so each has a plausible SUCCESSFUL outcome waiting for it and a
# refusal-shaped assertion alone cannot see that.
WRAPPING_NOTE_INDICES = [(2**64, 0), (2**64 + 1, 1), (2**64 + 2, 2), (2 * 2**64, 0)]


def _spans(notes: Sequence[libsonare.NoteObject]) -> list[tuple[int, int]]:
    return [(note.frame_start, note.frame_end) for note in notes]


def test_split_note_refuses_an_index_past_size_t_rather_than_splitting_the_folded_one() -> None:
    """Python integers are unbounded, so the range check has to be the binding's.

    The negative half is refused by the facade's own guard; the ceiling is the
    conversion's, and without it an index past ``size_t`` reaches the core as a
    small one. The refusal is anchored on the argument it names, because the
    facade's other refusals name ``frame`` and ``voiced`` and an unanchored
    match would accept either in place of this one.
    """
    audio, f0_hz, voiced, notes = _gapped_notes()

    # Positive control: two legitimate indices split different notes.
    first = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 0, 4, voiced=voiced)
    second = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, 13, voiced=voiced)
    assert _spans(first) != _spans(second)

    for value, _folds_to in WRAPPING_NOTE_INDICES:
        with pytest.raises(SonareValueError, match=r"^index must be"):
            libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, value, 13, voiced=voiced)

    # What makes 2**64 + 1 the interesting value rather than a merely huge one:
    # it folds to 1, and index 1 is a legal split with a result waiting. A
    # dropped range check does not raise, it returns this -- byte for byte the
    # right answer to a question the caller never asked.
    assert _spans(second) == [(0, 8), (10, 13), (13, 17), (19, 24)]


def test_merge_notes_refuses_a_run_past_size_t_rather_than_joining_the_folded_one() -> None:
    """Both ends are converted, so each is asserted by the argument it names.

    ``first`` and ``last`` are separate arguments of one call and only the first
    bad one is reported, so a refusal matched loosely would let one stand in for
    the other.
    """
    audio, f0_hz, voiced, notes = _gapped_notes()

    # Positive control: two legitimate runs join different notes.
    low = libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 0, 1, voiced=voiced)
    high = libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 1, 2, voiced=voiced)
    assert _spans(low) != _spans(high)

    for value, _folds_to in WRAPPING_NOTE_INDICES:
        with pytest.raises(SonareValueError, match=r"^first must be"):
            libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, value, 1, voiced=voiced)
        with pytest.raises(SonareValueError, match=r"^last must be"):
            libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 0, value, voiced=voiced)

    # The silent path a refusal-shaped assertion cannot reach: 2**64 and
    # 2**64 + 1 fold to 0 and 1, the run the control merged, so an unchecked
    # pair returns that result rather than failing.
    assert _spans(low) == [(0, 17), (19, 24)]
    with pytest.raises(SonareValueError, match=r"^first must be"):
        libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 2**64, 2**64 + 1, voiced=voiced)


# --- merge_notes ------------------------------------------------------------


def test_merge_notes_spans_the_gap_it_joins_over_and_keeps_every_slice_aligned() -> None:
    audio, f0_hz, voiced, notes = _gapped_notes()
    before = [note.amplitude.copy() for note in notes]

    merged = libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 0, 1, voiced=voiced)
    # last - first notes go away, so the count drops by exactly one here.
    assert len(merged) == 2
    assert (merged[0].frame_start, merged[0].frame_end) == (0, 17)
    assert merged[0].onset_sample == notes[0].onset_sample
    assert merged[0].offset_sample == notes[1].offset_sample
    assert merged[1] == notes[2]

    # The merged note's curve covers the gap the segmenter cut at, so it grew by
    # the two frames neither neighbour carried.
    joined = merged[0].amplitude
    assert joined.shape == (17,)
    np.testing.assert_allclose(joined[:8], before[0], atol=1e-6)
    np.testing.assert_allclose(joined[10:], before[1], atol=1e-6)
    # The gap's own amplitude lives in the audio, not in either neighbour.
    assert float(joined[8]) > 0.0
    assert float(joined[9]) > 0.0
    np.testing.assert_allclose(merged[1].amplitude, before[2], atol=1e-6)

    assert all(note.edit.amplitude_envelope.size == 0 for note in merged)


def test_merge_notes_takes_the_first_notes_edit() -> None:
    audio, f0_hz, voiced, notes = _gapped_notes()
    notes[0].edit.gain_db = -3.0
    notes[0].edit.pitch_shift_semitones = 2.0
    notes[0].edit.amplitude_envelope = np.array([0.25, 1.0], dtype=np.float32)
    notes[1].edit.gain_db = 9.0
    notes[1].edit.muted = True

    merged = libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 0, 1, voiced=voiced)
    assert len(merged) == 2
    assert merged[0].edit.gain_db == -3.0
    assert merged[0].edit.pitch_shift_semitones == 2.0
    assert merged[0].edit.amplitude_envelope.size > 0
    # The second note's edit does not survive: the rule is the first note's edit,
    # not a merge of the two.
    assert merged[0].edit.muted is False


def test_a_split_undone_by_a_merge_returns_the_spans_and_curves_it_started_from() -> None:
    audio, f0_hz, voiced, notes = _gapped_notes()
    before = [note.amplitude.copy() for note in notes]

    split = libsonare.split_note(audio, SR, f0_hz, FRAME_RATE, notes, 1, 13, voiced=voiced)
    assert len(split) == 4

    rejoined = libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, split, 1, 2, voiced=voiced)
    assert len(rejoined) == len(notes)
    for restored, original, curve in zip(rejoined, notes, before, strict=True):
        assert restored == original
        np.testing.assert_allclose(restored.amplitude, curve, atol=1e-6)


def test_merge_notes_rejects_a_run_that_does_not_ascend_or_runs_past_the_set() -> None:
    audio, f0_hz, voiced, notes = _gapped_notes()

    # A run of one is not a merge, a run cannot run backwards, and last cannot
    # index past the end.
    for first, last in ((1, 1), (2, 1), (0, len(notes)), (len(notes), len(notes) + 1)):
        with pytest.raises(SonareError):
            libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, first, last, voiced=voiced)
    with pytest.raises(SonareValueError, match="first"):
        libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, -1, 1, voiced=voiced)

    with pytest.raises(SonareValueError, match="voiced"):
        libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 0, 1)

    # Positive control: merging the whole run collapses the list to one note over
    # the whole span, so none of the above passes by rejecting every merge.
    whole = libsonare.merge_notes(audio, SR, f0_hz, FRAME_RATE, notes, 0, 2, voiced=voiced)
    assert len(whole) == 1
    assert whole[0].frame_start == notes[0].frame_start
    assert whole[0].frame_end == notes[2].frame_end


def test_the_documented_pitch_pyin_to_extract_notes_pipeline_runs() -> None:
    """The shipped example is executable with either pitch_pyin spelling.

    pitch_pyin leaves an unvoiced frame as NaN by default and extract_notes
    reads that as a frame carrying no pitch, so fill_na is a choice about the
    contour rather than a requirement of the call. Both are asserted, against
    an input whose default track really does carry NaN -- otherwise the pair
    would agree for want of anything to disagree about.
    """
    import math

    import numpy as np

    import libsonare

    sr = 22050
    t = np.arange(sr, dtype=np.float32) / sr
    samples = (0.4 * np.sin(2.0 * math.pi * 220.0 * t)).astype(np.float32)
    samples[sr // 3 : 2 * sr // 3] = 0.0  # a silent passage -> unvoiced frames

    # Non-vacuity: the default track really is NaN-bearing on this input, so
    # the success below is the argument doing work rather than the input being
    # too clean to tell.
    default_track = libsonare.pitch_pyin(samples, sample_rate=sr, hop_length=512)
    assert np.isnan(np.asarray(default_track.f0, dtype=np.float64)).any()
    unfilled = libsonare.extract_notes(
        samples,
        sr,
        default_track.f0,
        sr / 512,
        voiced=[int(v) for v in default_track.voiced_flag],
    )
    assert unfilled, "the NaN-bearing track produced no notes"

    # The example as shipped.
    pitch = libsonare.pitch_pyin(samples, sample_rate=sr, hop_length=512, fill_na=True)
    notes = libsonare.extract_notes(
        samples,
        sr,
        pitch.f0,
        sr / 512,
        voiced=[int(v) for v in pitch.voiced_flag],
    )
    assert notes, "the documented pipeline produced no notes on a voiced input"
    notes[0].edit.gain_db = -6.0
    quieter = libsonare.render_notes(samples, sr, notes)
    assert len(quieter) == len(samples)
