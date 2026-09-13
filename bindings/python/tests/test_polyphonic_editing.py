"""Tests for the polyphonic note-editing Python facade.

:class:`libsonare.PolyphonicAnalysis` is a ctypes wrapper over the
``sonare_polyphonic_*`` C ABI, which keeps the analysis -- a complex spectrogram
plus the complex weight of every bin each note claimed -- on the C side and hands
the caller a handle. What is under test here is therefore the handle's lifecycle
and the marshalling, not the separation: every ``*_count`` entry point is a query
the caller is supposed to make FIRST to size a buffer, and a capacity shorter
than the data is clamped rather than refused, so the facade does the two step
itself and a caller must never see a truncated curve.

The material is a sustained E4 + B4 dyad, half a second at 44100. A ridge has to
last 140 ms by default to survive tracking, so that is close to the cheapest
input the chain resolves at all, and the figures the cases below pin -- two
notes, 44 frames, medians at the two tones -- were measured on this exact
fixture rather than assumed.
"""

from __future__ import annotations

import ctypes
from collections.abc import Iterator

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 44100
CHORD_SECONDS = 0.5
CHORD_SAMPLES = int(SR * CHORD_SECONDS)
# E4 and B4, a fifth apart: far outside the 50-cent default separation, and B4 is
# not a partial of E4, so neither tone can be folded into the other's ridge.
CHORD_HZ = (329.63, 493.88)

# The analysis frames the whole source at the default 4096 / 512 framing, centred,
# so the count is CHORD_SAMPLES // 512 + 1 rather than a measured constant.
DEFAULT_HOP = 512
EXPECTED_FRAMES = CHORD_SAMPLES // DEFAULT_HOP + 1
# Both tones resolve, one note each.
EXPECTED_NOTES = 2
# A tone is matched to a note's median within this; the two medians landed within
# 0.01 Hz of their tones, so the window is slack rather than a fitted bound.
PITCH_TOLERANCE_CENTS = 50.0


def _chord() -> NDArray[np.float32]:
    """A sustained two-note chord, the fixture material for the whole module."""
    return np.sum(
        [sine(hz, CHORD_SECONDS, SR, amp=0.3) for hz in CHORD_HZ], axis=0, dtype=np.float32
    )


def _cents(hz: float, reference: float) -> float:
    return abs(float(np.log2(hz / reference)) * 1200.0)


def _rms(samples: NDArray[np.float32]) -> float:
    return float(np.sqrt(np.mean(np.square(samples.astype(np.float64))))) if samples.size else 0.0


@pytest.fixture(scope="module")
def chord() -> NDArray[np.float32]:
    return _chord()


@pytest.fixture(scope="module")
def analysis(chord: NDArray[np.float32]) -> Iterator[libsonare.PolyphonicAnalysis]:
    """One analysis shared by every read-only case; an edit case builds its own.

    Module scoped because the analysis is the expensive part and nothing below
    mutates this handle -- ``set_note_edit`` is the only writer on the surface and
    every case that calls it opens a handle of its own, so no case here depends on
    another's ordering.
    """
    with libsonare.PolyphonicAnalysis.analyze(chord, SR) as handle:
        yield handle


@pytest.fixture(scope="module")
def notes(analysis: libsonare.PolyphonicAnalysis) -> list[libsonare.NoteObject]:
    found = analysis.notes()
    # Non-vacuity: every case below reads a note, and an analysis that tracked no
    # ridge would make all of them pass without looking at anything.
    assert found, "the chord tracked no ridge; the cases below would be vacuous"
    return found


# ---------------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------------


def test_close_is_idempotent(chord: NDArray[np.float32]) -> None:
    """A second close is a no-op, not a double free."""
    handle = libsonare.PolyphonicAnalysis(chord, SR)
    handle.close()
    handle.close()


def test_destroy_and_delete_alias_close(chord: NDArray[np.float32]) -> None:
    """The cross-binding names release the same handle."""
    handle = libsonare.PolyphonicAnalysis(chord, SR)
    handle.destroy()
    handle.delete()
    with pytest.raises(RuntimeError, match="closed"):
        handle.note_count()


def test_use_after_close_raises(chord: NDArray[np.float32]) -> None:
    """Every accessor refuses a released handle instead of dereferencing NULL."""
    handle = libsonare.PolyphonicAnalysis(chord, SR)
    handle.close()
    for call in (
        handle.note_count,
        handle.frame_count,
        handle.notes,
        handle.polyphony,
        handle.render,
    ):
        with pytest.raises(RuntimeError, match="closed"):
            call()
    with pytest.raises(RuntimeError, match="closed"):
        handle.note_f0(0)
    with pytest.raises(RuntimeError, match="closed"):
        handle.note_envelope(0)
    with pytest.raises(RuntimeError, match="closed"):
        handle.set_note_edit(0, libsonare.NoteEdit())


def test_context_manager_closes(chord: NDArray[np.float32]) -> None:
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.frame_count() > 0
    with pytest.raises(RuntimeError, match="closed"):
        handle.frame_count()


def test_empty_and_non_finite_input_are_rejected() -> None:
    """The facade names the argument rather than leaving the C ABI's bare code."""
    with pytest.raises(SonareValueError, match="samples"):
        libsonare.PolyphonicAnalysis(np.empty(0, dtype=np.float32), SR)
    bad = _chord()
    bad[10] = np.nan
    with pytest.raises(SonareValueError, match="NaN"):
        libsonare.PolyphonicAnalysis(bad, SR)


# ---------------------------------------------------------------------------
# What the analysis found
# ---------------------------------------------------------------------------


def test_the_dyad_resolves_into_one_note_per_tone(
    analysis: libsonare.PolyphonicAnalysis, notes: list[libsonare.NoteObject]
) -> None:
    """The measured shape of this fixture: two notes over the whole framing."""
    assert analysis.note_count() == len(notes) == EXPECTED_NOTES
    assert analysis.frame_count() == EXPECTED_FRAMES


def test_notes_carry_their_spans_and_the_identity_edit(
    notes: list[libsonare.NoteObject],
) -> None:
    """A freshly analysed note is measured, unedited, and inside the source."""
    for note in notes:
        assert 0 <= note.onset_sample < note.offset_sample <= CHORD_SAMPLES
        assert 0 <= note.frame_start < note.frame_end <= EXPECTED_FRAMES
        assert note.median_hz > 0.0
        assert 0.0 <= note.f0_stability <= 1.0
        assert note.edit == libsonare.NoteEdit()


def test_each_tone_gets_a_note_at_its_own_pitch(notes: list[libsonare.NoteObject]) -> None:
    """Every chord tone is matched by exactly one note's median pitch.

    Matched both ways round, because one note landing on a tone and the other on
    its octave would satisfy a per-note check alone.
    """
    for tone in CHORD_HZ:
        matches = [n for n in notes if _cents(n.median_hz, tone) < PITCH_TOLERANCE_CENTS]
        assert len(matches) == 1, f"{tone} Hz matched {len(matches)} notes"


def test_each_notes_f0_curve_holds_its_own_pitch(
    analysis: libsonare.PolyphonicAnalysis, notes: list[libsonare.NoteObject]
) -> None:
    """A sustained tone's curve is flat at the note's median, frame by frame.

    The F0 accessor is the one curve a host reads to draw a note, so a marshalling
    slip that returned a neighbour's frames or a half-filled buffer has to show up
    as a value off the tone rather than only as a wrong length.
    """
    for index, note in enumerate(notes):
        curve = analysis.note_f0(index)
        assert curve.size > 0
        # Checked before the cents conversion, which an unvoiced 0 would send to
        # infinity and report as a pitch error rather than as the gap it is.
        assert np.all(curve > 0.0)
        assert max(_cents(float(hz), note.median_hz) for hz in curve) < PITCH_TOLERANCE_CENTS


def test_polyphony_has_one_entry_per_frame(analysis: libsonare.PolyphonicAnalysis) -> None:
    counts = analysis.polyphony()
    assert counts.dtype == np.int32
    assert len(counts) == analysis.frame_count() == EXPECTED_FRAMES
    assert np.all(counts >= 0)


def test_every_curve_spans_its_note(
    analysis: libsonare.PolyphonicAnalysis, notes: list[libsonare.NoteObject]
) -> None:
    """The three per-note curves are indexed by the note's own frame span.

    This is the facade's two-step under test: the C entry points take a capacity
    and clamp to it, so a curve shorter than ``frame_end - frame_start`` would mean
    the facade sized its buffer off something other than the analysis's frames.

    ``note_envelope`` is deliberately not here. It is a set of gain points rather
    than a per-frame signal, so its length is the note's own point count and the
    span is not a bound on it at all.
    """
    for index, note in enumerate(notes):
        span = note.frame_end - note.frame_start
        for curve in (
            analysis.note_f0(index),
            analysis.note_amplitude(index),
            analysis.note_salience(index),
        ):
            assert curve.dtype == np.float32
            assert len(curve) == span
        assert np.all(analysis.note_f0(index) >= 0.0)
        assert np.all(analysis.note_amplitude(index) >= 0.0)


def test_notes_carry_the_amplitude_curve(
    analysis: libsonare.PolyphonicAnalysis, notes: list[libsonare.NoteObject]
) -> None:
    """``NoteObject.amplitude`` is the same curve the accessor returns."""
    for index, note in enumerate(notes):
        assert np.array_equal(note.amplitude, analysis.note_amplitude(index))


def test_a_fresh_note_carries_no_envelope(
    analysis: libsonare.PolyphonicAnalysis, notes: list[libsonare.NoteObject]
) -> None:
    """The identity edit has no envelope, read either way round.

    The zero-point read is worth its own case: it is the one where the accessor is
    called with a capacity of 0, which must still validate the note index rather
    than short-circuit into an empty answer.
    """
    assert all(note.edit.amplitude_envelope.size == 0 for note in notes)
    assert all(analysis.note_envelope(i).size == 0 for i in range(len(notes)))


def test_a_short_capacity_read_is_clamped_not_refused(
    analysis: libsonare.PolyphonicAnalysis, notes: list[libsonare.NoteObject]
) -> None:
    """The C contract the facade's two-step exists to hide.

    Reaching past the facade on purpose: a capacity below the curve's length is
    answered with ``SONARE_OK`` and a truncated ``out_count``, so a caller that
    sized its own buffer from anything but the frame count would silently read a
    short curve. The facade's answer above it is the full one.
    """
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    full = analysis.note_f0(0)
    assert len(full) > 1, "the first note spans one frame; this case cannot clamp"

    out = (ctypes.c_float * 1)()
    written = ctypes.c_size_t(123)
    rc = lib.sonare_polyphonic_note_f0(
        analysis._require_handle(),
        ctypes.c_size_t(0),
        out,
        ctypes.c_size_t(1),
        ctypes.byref(written),
    )
    assert rc == 0
    assert written.value == 1
    assert out[0] == pytest.approx(float(full[0]))
    assert len(analysis.note_f0(0)) == len(full)


# ---------------------------------------------------------------------------
# Index validation
# ---------------------------------------------------------------------------


def test_a_note_index_past_the_end_is_refused(analysis: libsonare.PolyphonicAnalysis) -> None:
    past = analysis.note_count()
    for call in (
        analysis.note_f0,
        analysis.note_amplitude,
        analysis.note_salience,
        analysis.note_envelope,
    ):
        with pytest.raises(SonareError):
            call(past)
    with pytest.raises(SonareError):
        analysis.set_note_edit(past, libsonare.NoteEdit())


def test_a_negative_note_index_is_refused_before_it_wraps(
    analysis: libsonare.PolyphonicAnalysis,
) -> None:
    """``size_t`` would turn -1 into a huge index, so the facade refuses it first."""
    for call in (
        analysis.note_f0,
        analysis.note_amplitude,
        analysis.note_salience,
        analysis.note_envelope,
    ):
        with pytest.raises(SonareValueError, match="non-negative"):
            call(-1)
    with pytest.raises(SonareValueError, match="non-negative"):
        analysis.set_note_edit(-1, libsonare.NoteEdit())


# ---------------------------------------------------------------------------
# Editing and rendering
# ---------------------------------------------------------------------------


def test_an_unedited_render_has_the_source_length(
    analysis: libsonare.PolyphonicAnalysis, chord: NDArray[np.float32]
) -> None:
    """The round trip, which is not the source bit for bit but is the same audio."""
    rendered = analysis.render()
    assert rendered.dtype == np.float32
    assert len(rendered) == len(chord) == CHORD_SAMPLES
    assert np.all(np.isfinite(rendered))
    assert _rms(rendered) == pytest.approx(_rms(chord), rel=0.2)


def test_an_unedited_render_is_repeatable(analysis: libsonare.PolyphonicAnalysis) -> None:
    """Nothing in the render mutates the analysis it reads."""
    assert np.array_equal(analysis.render(), analysis.render())


def test_a_pitch_shift_reaches_the_render(chord: NDArray[np.float32]) -> None:
    """One semitone on one note of the dyad moves the output, and not marginally.

    The threshold is relative to the source's own peak because the fixture's
    amplitude is this module's choice; what was measured is that the difference is
    near full amplitude, a shifted partial beating against where it used to sit
    rather than a rounding-level wobble.
    """
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.note_count() == EXPECTED_NOTES
        before = handle.render()
        handle.set_note_edit(0, libsonare.NoteEdit(pitch_shift_semitones=1.0))
        after = handle.render()

    assert len(after) == len(before)
    peak_difference = float(np.max(np.abs(after.astype(np.float64) - before.astype(np.float64))))
    assert peak_difference > 0.25 * float(np.max(np.abs(chord)))


def test_muting_a_note_quietens_its_span(chord: NDArray[np.float32]) -> None:
    """The edit reaches the render, measured over the edited note's own span.

    Own handle rather than the module fixture: this is the only writer on the
    surface, so sharing one would make the read-only cases depend on the order.
    """
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        found = handle.notes()
        assert found, "the chord tracked no ridge; this case would be vacuous"
        note = found[0]
        before = handle.render()

        handle.set_note_edit(0, libsonare.NoteEdit(muted=True))
        after = handle.render()

        assert len(after) == len(before)
        assert not np.array_equal(after, before)
        span = slice(note.onset_sample, note.offset_sample)
        assert _rms(after[span]) < _rms(before[span])


def test_a_gain_edit_is_readable_back_on_the_note(chord: NDArray[np.float32]) -> None:
    """``notes()`` reports the pending edit, so a host can show what it set."""
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.note_count() == EXPECTED_NOTES
        handle.set_note_edit(0, libsonare.NoteEdit(gain_db=-6.0, pitch_shift_semitones=2.0))
        edit = handle.notes()[0].edit
        assert edit.gain_db == pytest.approx(-6.0)
        assert edit.pitch_shift_semitones == pytest.approx(2.0)
        assert edit.time_stretch_ratio == pytest.approx(1.0)


def test_setting_none_restores_the_identity_edit(chord: NDArray[np.float32]) -> None:
    """The replacement is whole, so ``None`` puts the note back as analysed."""
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.note_count() == EXPECTED_NOTES
        identity = handle.render()
        handle.set_note_edit(0, libsonare.NoteEdit(gain_db=-12.0))
        assert not np.array_equal(handle.render(), identity)
        handle.set_note_edit(0, None)
        assert handle.notes()[0].edit == libsonare.NoteEdit()
        assert np.array_equal(handle.render(), identity)


def test_an_envelope_is_read_back_on_the_note_that_carries_it(
    chord: NDArray[np.float32],
) -> None:
    """The points go in, come back out, and land on the note that was set.

    Both accessors are checked, because they size their buffer differently: the
    standalone one asks the note for its point count, while ``notes()`` already has
    the count on the C row. The untouched note reading 0 points is what makes this
    a per-note read rather than one envelope shared by the analysis.
    """
    points = np.array([1.0, 0.25, 0.0], dtype=np.float32)
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.note_count() == EXPECTED_NOTES
        note = handle.notes()[0]
        identity = handle.render()

        edit = libsonare.NoteEdit()
        edit.amplitude_envelope = points
        handle.set_note_edit(0, edit)

        assert np.array_equal(handle.note_envelope(0), points)
        assert np.array_equal(handle.notes()[0].edit.amplitude_envelope, points)
        # Per note: nothing was set on note 1, which therefore still has no points.
        assert handle.note_envelope(1).size == 0
        assert handle.notes()[1].edit.amplitude_envelope.size == 0

        # And the points reached the render, not just the read-back path.
        faded = handle.render()
        assert not np.array_equal(faded, identity)
        # A ramp to silence, so the back half of the note's span loses level.
        middle = (note.onset_sample + note.offset_sample) // 2
        tail = slice(middle, note.offset_sample)
        assert _rms(faded[tail]) < _rms(identity[tail])


def test_an_envelope_survives_a_note_round_trip(chord: NDArray[np.float32]) -> None:
    """A note read, re-set unchanged, and read again keeps its envelope.

    The round trip is the reason the points have to be readable: ``notes()`` ->
    edit a field -> :meth:`set_note_edit` is the loop a host runs, and an envelope
    the read could not recover would be cleared by the write.
    """
    points = np.array([0.5, 1.0], dtype=np.float32)
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.note_count() == EXPECTED_NOTES
        first = libsonare.NoteEdit()
        first.amplitude_envelope = points
        handle.set_note_edit(0, first)

        carried = handle.notes()[0].edit
        carried.gain_db = -3.0
        handle.set_note_edit(0, carried)

        after = handle.notes()[0].edit
        assert after.gain_db == pytest.approx(-3.0)
        assert np.array_equal(after.amplitude_envelope, points)


def test_a_two_dimensional_envelope_is_refused(chord: NDArray[np.float32]) -> None:
    with libsonare.PolyphonicAnalysis(chord, SR) as handle:
        assert handle.note_count() == EXPECTED_NOTES
        edit = libsonare.NoteEdit()
        edit.amplitude_envelope = np.ones((2, 2), dtype=np.float32)
        with pytest.raises(SonareValueError, match="one-dimensional"):
            handle.set_note_edit(0, edit)


def test_render_rejects_a_negative_fade(analysis: libsonare.PolyphonicAnalysis) -> None:
    with pytest.raises(SonareError):
        analysis.render(fade_ms=-1.0)


# ---------------------------------------------------------------------------
# The config reaches the core
# ---------------------------------------------------------------------------


@pytest.mark.slow  # two further analyses of the chord; run via `make test-python-slow`
def test_the_hop_length_reaches_the_framing(chord: NDArray[np.float32]) -> None:
    """The framing comes from the config, at the exact count the hop implies.

    A facade that dropped the config (passing NULL, or zeroing the field it could
    not place) would give the default's frame count on both rows, so the pair is
    the check and each count on its own is the measurement.
    """
    for hop in (DEFAULT_HOP, 1024):
        with libsonare.PolyphonicAnalysis(chord, SR, hop_length=hop) as handle:
            frames = handle.frame_count()
            assert frames == CHORD_SAMPLES // hop + 1
            assert len(handle.polyphony()) == frames


@pytest.mark.slow  # one further analysis of the chord; run via `make test-python-slow`
def test_a_negative_selects_zero_on_the_four_fields_that_accept_it(
    chord: NDArray[np.float32],
) -> None:
    """The four fields where 0 is both the default and a legal value.

    A negative is how the C ABI spells 0 on those, and it must cross the facade
    unchanged: translating it to 0.0 would select the default instead, and
    clamping it to 0 here would hide the rejection the core owes any other
    negative. With all four at 0 nothing is dropped for being short or faint, so
    the analysis keeps at least the notes the defaults keep.
    """
    with libsonare.PolyphonicAnalysis(
        chord,
        SR,
        min_frame_peak_ratio=-1.0,
        min_separation_cents=-1.0,
        min_ridge_peak_ratio=-1.0,
        min_ridge_duration_ms=-1.0,
    ) as floored:
        assert floored.note_count() >= EXPECTED_NOTES
        assert floored.frame_count() == EXPECTED_FRAMES
