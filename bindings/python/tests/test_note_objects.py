"""Tests for the ``extract_notes`` / ``render_notes`` Python facade.

The note-object API is a ctypes pass-through over the C ABI
``sonare_extract_notes`` / ``sonare_render_notes``. The F0 track is
caller-supplied, so these cases hand the extractor a synthetic three-note
contour rather than a detector's output: what is under test is the marshalling
and the edit semantics, not the segmenter (which has its own coverage).
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE, sine

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
