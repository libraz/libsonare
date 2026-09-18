"""Reference-melody assignment: ``note_targets_from_smf`` / ``assign_note_targets``.

The notes are hand-built at 48 kHz on half-second spans and every median is
440 Hz, which is MIDI 69 exactly, so each expected shift is a whole number rather
than a tolerance. The reference melodies are real SMF bytes written by the
library's own project exporter -- the fixture, not the thing under test.

Every configuration argument is checked by running the same call twice, differing
only in that one argument, and asserting the two answers differ. A case that only
asserted "no exception" would pass on a facade that dropped the argument on the
floor, which is the whole failure mode a configuration surface has.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare
from libsonare import (
    ErrorCode,
    NoteEdit,
    NoteObject,
    NoteTarget,
    Project,
    SonareError,
    SonareValueError,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 48000
HALF_SECOND = SR // 2
# 440 Hz is MIDI 69 exactly, so every expected shift below is target - 69.
A4_HZ = 440.0

# An answer as the three things a caller reads off one call: how many notes got a
# target, what each note's shift became, and which notes were muted.
Answer = tuple[int, tuple[float, ...], tuple[bool, ...]]


def _note(onset_sample: int, offset_sample: int, median_hz: float = A4_HZ) -> NoteObject:
    return NoteObject(
        onset_sample=onset_sample,
        offset_sample=offset_sample,
        median_hz=median_hz,
        edit=NoteEdit(time_stretch_ratio=1.0),
    )


def _answer(
    notes: list[NoteObject], targets: list[NoteTarget], **options: object
) -> tuple[Answer, list[NoteObject]]:
    """Run one assignment and reduce it to a comparable answer."""
    edited, assigned = libsonare.assign_note_targets(notes, SR, targets, **options)  # type: ignore[arg-type]
    answer = (
        assigned,
        tuple(round(note.edit.pitch_shift_semitones, 4) for note in edited),
        tuple(note.edit.muted for note in edited),
    )
    return answer, edited


def _two_quarter_notes_smf() -> bytes:
    """C4 over 0-0.5 s and G4 over 0.5-1.0 s, at the project's default 120 BPM."""
    project = Project()
    try:
        _track_id, clip_id = project.add_midi_clip(0.0, 4.0)
        project.set_midi_events(
            clip_id,
            [
                Project.midi_note_on(0.0, 0, 0, 60, 100),
                Project.midi_note_off(1.0, 0, 0, 60, 0),
                Project.midi_note_on(1.0, 0, 0, 67, 100),
                Project.midi_note_off(2.0, 0, 0, 67, 0),
            ],
        )
        return project.export_smf()
    finally:
        project.close()


# ---------------------------------------------------------------------------
# note_targets_from_smf
# ---------------------------------------------------------------------------


def test_the_smf_reader_returns_the_written_melody() -> None:
    targets = libsonare.note_targets_from_smf(_two_quarter_notes_smf())

    assert [(t.start_sec, t.end_sec, t.target_midi) for t in targets] == [
        (0.0, 0.5, 60.0),
        (0.5, 1.0, 67.0),
    ]


def test_the_conductor_track_is_not_counted_in_the_track_index() -> None:
    """The exporter writes a tempo-map track; the melody is still at index 0."""
    data = _two_quarter_notes_smf()

    assert len(libsonare.note_targets_from_smf(data, track_index=0)) == 2
    with pytest.raises(SonareError) as caught:
        libsonare.note_targets_from_smf(data, track_index=1)
    assert caught.value.code == ErrorCode.INVALID_PARAMETER


@pytest.mark.parametrize("data", [b"not a midi file at all", b""])
def test_the_smf_reader_refuses_bytes_that_are_not_a_midi_file(data: bytes) -> None:
    with pytest.raises(SonareError) as caught:
        libsonare.note_targets_from_smf(data)
    assert caught.value.code == ErrorCode.INVALID_FORMAT


def test_a_fractional_track_index_is_refused_rather_than_truncated() -> None:
    data = _two_quarter_notes_smf()

    with pytest.raises(SonareValueError, match="track_index"):
        libsonare.note_targets_from_smf(data, track_index=0.5)  # type: ignore[arg-type]


def test_the_melody_a_file_carries_is_what_gets_assigned() -> None:
    """The two halves of the pair, end to end: read a melody, follow it."""
    targets = libsonare.note_targets_from_smf(_two_quarter_notes_smf())
    notes = [_note(0, HALF_SECOND), _note(HALF_SECOND, 2 * HALF_SECOND)]

    edited, assigned = libsonare.assign_note_targets(notes, SR, targets)

    assert assigned == 2
    # C4 and G4 against a 440 Hz take: 60 - 69 and 67 - 69.
    assert [note.edit.pitch_shift_semitones for note in edited] == pytest.approx(
        [-9.0, -2.0], abs=1e-4
    )


# ---------------------------------------------------------------------------
# assign_note_targets -- what it writes
# ---------------------------------------------------------------------------


def test_assignment_writes_the_shift_its_target_asks_for() -> None:
    notes = [_note(0, HALF_SECOND), _note(HALF_SECOND, 2 * HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0), NoteTarget(0.5, 1.0, 72.0)]

    (assigned, shifts, mutes), _edited = _answer(notes, targets)

    assert assigned == 2
    assert shifts == pytest.approx((-9.0, 3.0), abs=1e-4)
    assert mutes == (False, False)


def test_the_callers_notes_are_not_rewritten() -> None:
    """The C call edits in place; this facade copies in and copies out."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    edited, _assigned = libsonare.assign_note_targets(
        notes, SR, targets, unmatched_policy="mute", max_correction_semitones=1.0
    )

    assert notes[0].edit.pitch_shift_semitones == 0.0
    assert notes[0].edit.muted is False
    assert edited[0] is not notes[0]
    assert edited[0].edit is not notes[0].edit
    assert edited[0].edit.pitch_shift_semitones == pytest.approx(-1.0, abs=1e-4)


def test_everything_the_assignment_does_not_write_survives_it() -> None:
    envelope = np.array([1.0, 0.5, 0.25], dtype=np.float32)
    note = NoteObject(
        onset_sample=0,
        offset_sample=HALF_SECOND,
        frame_start=3,
        frame_end=24,
        median_hz=A4_HZ,
        median_cents=1200.0,
        f0_stability=0.75,
        edit=NoteEdit(
            time_offset_samples=128,
            gain_db=-3.0,
            time_stretch_ratio=1.5,
            formant_shift_semitones=2.0,
            vibrato_depth_change=-1.0,
            drift_change=0.5,
            amplitude_envelope=envelope,
        ),
    )

    edited, assigned = libsonare.assign_note_targets([note], SR, [NoteTarget(0.0, 0.5, 60.0)])

    assert assigned == 1
    result = edited[0]
    assert result.edit.pitch_shift_semitones == pytest.approx(-9.0, abs=1e-4)
    assert (result.onset_sample, result.offset_sample) == (0, HALF_SECOND)
    assert (result.frame_start, result.frame_end) == (3, 24)
    assert (result.median_cents, result.f0_stability) == (1200.0, 0.75)
    assert result.edit.time_offset_samples == 128
    assert result.edit.gain_db == -3.0
    assert result.edit.time_stretch_ratio == 1.5
    assert result.edit.formant_shift_semitones == 2.0
    assert result.edit.vibrato_depth_change == -1.0
    assert result.edit.drift_change == 0.5
    assert np.array_equal(result.edit.amplitude_envelope, envelope)


def test_a_note_with_no_measured_pitch_is_never_assigned_or_edited() -> None:
    """No policy reaches it: there is no pitch to correct from."""
    unpitched = [_note(0, HALF_SECOND, median_hz=0.0)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    for policy in ("leave", "mute", "nearest"):
        (assigned, shifts, mutes), _edited = _answer(unpitched, targets, unmatched_policy=policy)
        assert (assigned, shifts, mutes) == (0, (0.0,), (False,)), policy


def test_an_empty_note_set_and_an_empty_reference_are_both_answers() -> None:
    assert libsonare.assign_note_targets([], SR, []) == ([], 0)

    notes = [_note(0, HALF_SECOND)]
    (assigned, shifts, mutes), _edited = _answer(notes, [], unmatched_policy="mute")
    assert (assigned, shifts, mutes) == (0, (0.0,), (True,))


# ---------------------------------------------------------------------------
# Positive controls: each argument changes the answer
# ---------------------------------------------------------------------------


def test_each_unmatched_policy_gives_a_different_answer() -> None:
    """One pitched note, one target two seconds away -- the policy decides."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(2.0, 2.5, 72.0)]

    leave, _ = _answer(notes, targets, unmatched_policy="leave")
    mute, _ = _answer(notes, targets, unmatched_policy="mute")
    nearest, _ = _answer(notes, targets, unmatched_policy="nearest")

    assert leave == (0, (0.0,), (False,))
    assert mute == (0, (0.0,), (True,))
    # NEAREST takes the target however far away it is, and counts as assigned.
    assert nearest == (1, (3.0,), (False,))
    assert len({leave, mute, nearest}) == 3


def test_leave_is_the_default_policy() -> None:
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(2.0, 2.5, 72.0)]

    default, _ = _answer(notes, targets)

    assert default == _answer(notes, targets, unmatched_policy="leave")[0]
    assert default != _answer(notes, targets, unmatched_policy="mute")[0]


def test_min_overlap_ratio_decides_whether_a_target_counts() -> None:
    """The target covers 0.2 s of a 0.5 s note, which is 40%."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.3, 0.7, 72.0)]

    strict, _ = _answer(notes, targets, min_overlap_ratio=0.5)
    loose, _ = _answer(notes, targets, min_overlap_ratio=0.3)

    assert strict == (0, (0.0,), (False,))
    assert loose == (1, (3.0,), (False,))
    assert strict != loose


def test_the_min_overlap_ratio_default_comes_from_the_library() -> None:
    """``None`` is 0.5, which is neither of the two values 0 could stand for."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.3, 0.7, 72.0)]

    assert _answer(notes, targets)[0] == _answer(notes, targets, min_overlap_ratio=0.5)[0]
    assert _answer(notes, targets)[0] != _answer(notes, targets, min_overlap_ratio=0.3)[0]


def test_a_zero_min_overlap_ratio_is_any_overlap_at_all() -> None:
    """0 is its own meaning here, not a second spelling of the default."""
    # The target reaches 0.01 s into the note, which is 2% of its span.
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.49, 0.6, 72.0)]

    default, _ = _answer(notes, targets)
    zero, _ = _answer(notes, targets, min_overlap_ratio=0.0)

    assert default == (0, (0.0,), (False,))
    assert zero == (1, (3.0,), (False,))


def test_max_correction_semitones_saturates_the_shift() -> None:
    """The target is two octaves up; what comes back is the bound, not 24."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 93.0)]

    default, _ = _answer(notes, targets)
    narrow, _ = _answer(notes, targets, max_correction_semitones=3.0)

    assert default == (1, (12.0,), (False,))
    assert narrow == (1, (3.0,), (False,))
    assert default != narrow


def test_a_zero_correction_bound_assigns_the_target_and_moves_nothing() -> None:
    """0 is a value, not the unset sentinel -- the note is assigned and still."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    default, _ = _answer(notes, targets)
    saturated, _ = _answer(notes, targets, max_correction_semitones=0.0)

    assert default == (1, (-9.0,), (False,))
    assert saturated == (1, (0.0,), (False,))


def test_the_longest_overlap_wins_and_a_tie_goes_to_the_earlier_target() -> None:
    notes = [_note(0, HALF_SECOND)]
    longer_second = [NoteTarget(0.0, 0.1, 60.0), NoteTarget(0.1, 0.5, 72.0)]
    tied = [NoteTarget(0.0, 0.25, 60.0), NoteTarget(0.25, 0.5, 72.0)]

    assert _answer(notes, longer_second, min_overlap_ratio=0.0)[0] == (1, (3.0,), (False,))
    assert _answer(notes, tied, min_overlap_ratio=0.0)[0] == (1, (-9.0,), (False,))


# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------


def test_an_unknown_policy_is_refused_by_name() -> None:
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    with pytest.raises(SonareValueError, match=r"'leave', 'mute', 'nearest'"):
        libsonare.assign_note_targets(notes, SR, targets, unmatched_policy="Nearest")


def test_an_integer_policy_is_not_a_policy() -> None:
    """The ordinals are the C ABI's; a caller spells the behaviour it wants."""
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    for value in (0, 1, 2, True, None):
        with pytest.raises(SonareValueError, match="unmatched_policy"):
            libsonare.assign_note_targets(
                notes,
                SR,
                targets,
                unmatched_policy=value,  # type: ignore[arg-type]
            )


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_a_non_finite_configuration_value_is_refused_by_name(bad: float) -> None:
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    with pytest.raises(SonareValueError, match="min_overlap_ratio"):
        libsonare.assign_note_targets(notes, SR, targets, min_overlap_ratio=bad)
    with pytest.raises(SonareValueError, match="max_correction_semitones"):
        libsonare.assign_note_targets(notes, SR, targets, max_correction_semitones=bad)


@pytest.mark.parametrize("field", ["start_sec", "end_sec", "target_midi"])
def test_a_non_finite_target_is_refused_naming_which_one(field: str) -> None:
    notes = [_note(0, HALF_SECOND)]
    values = {"start_sec": 0.0, "end_sec": 0.5, "target_midi": 60.0}
    values[field] = math.nan
    targets = [NoteTarget(0.0, 0.5, 60.0), NoteTarget(**values)]  # type: ignore[arg-type]

    with pytest.raises(SonareValueError, match=rf"targets\[1\]\.{field}"):
        libsonare.assign_note_targets(notes, SR, targets)


def test_an_out_of_domain_configuration_value_is_rejected_by_the_c_call() -> None:
    notes = [_note(0, HALF_SECOND)]
    targets = [NoteTarget(0.0, 0.5, 60.0)]

    with pytest.raises(SonareError):
        libsonare.assign_note_targets(notes, SR, targets, min_overlap_ratio=1.5)
    with pytest.raises(SonareError):
        libsonare.assign_note_targets(notes, SR, targets, max_correction_semitones=-1.0)
    with pytest.raises(SonareError):
        libsonare.assign_note_targets(notes, 0, targets)
