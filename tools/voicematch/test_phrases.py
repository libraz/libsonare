"""The phrase sets: that every voice has one, and that it is playable."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from phrases import (  # noqa: E402
    DRUM_CHANNEL, GENERIC_SETS, TAKE_SETS, build_takes, is_drum_set, take_set_for,
)
from toneclass import ToneClass  # noqa: E402

ALL_PROGRAMS = range(128)


def test_every_tone_class_has_a_generic_set():
    """A class with no set is a program the audition index cannot list.

    The index is the library's bank rather than the capture list, so a program
    that resolves to nothing is not a gap in one page — it is a program that
    cannot be auditioned at all.
    """
    assert set(GENERIC_SETS) == set(ToneClass)
    for name in GENERIC_SETS.values():
        assert name in TAKE_SETS


def test_every_program_resolves_to_a_set_that_exists():
    for program in ALL_PROGRAMS:
        assert take_set_for(program) in TAKE_SETS
    assert take_set_for(0, drum_note=38) == "drums"


@pytest.mark.parametrize("name", sorted(TAKE_SETS))
def test_a_set_is_playable_for_every_program(name):
    """Built across the whole bank, because a generic set fills its notes in.

    A register table entry near the top of the compass plus a phrase that
    reaches up an octave is how a set produces a note number MIDI has no room
    for — which renders as silence on one side and a transposition on the
    other, and looks like a voicing difference.
    """
    for program in ALL_PROGRAMS:
        phrases = build_takes(name, program)
        assert phrases, f"{name} produced no takes for program {program}"
        ids = [t.id for t in phrases]
        assert len(ids) == len(set(ids)), f"{name} repeats a take id"
        for take in phrases:
            assert take.notes, f"{name}/{take.id} has no notes"
            assert take.duration() > 0.0
            for note in take.notes:
                assert 0 <= note.note <= 127, f"{name}/{take.id} plays note {note.note}"
                assert 1 <= note.velocity <= 127
                assert note.start >= 0.0 and note.dur > 0.0
            for when, cc, value in take.cc_events:
                assert when >= 0.0 and 0 <= cc <= 127 and 0 <= value <= 127


def test_only_the_kit_set_is_on_the_drum_channel():
    """The channel is what makes a note number an instrument rather than a pitch.

    A melodic set that reached channel 10 would play the kit at every note it
    names, at the right length and the right level, and nothing would report it.
    """
    assert is_drum_set("drums")
    for name in TAKE_SETS:
        if name == "drums":
            continue
        assert not is_drum_set(name), f"{name} writes on the drum channel"


def test_a_generic_set_follows_the_program_compass():
    """The point of a generic set: a piccolo and a contrabass get their own notes."""
    piccolo = build_takes("sustained", 72)
    contrabass = build_takes("sustained", 43)
    lowest = min(n.note for t in piccolo for n in t.notes)
    highest = max(n.note for t in contrabass for n in t.notes)
    assert lowest > highest


def test_the_drum_set_stays_on_channel_ten():
    for take in build_takes("drums", 0):
        assert take.channel == DRUM_CHANNEL


@pytest.mark.parametrize("name", sorted(TAKE_SETS))
def test_a_set_keeps_each_group_in_one_run(name):
    """Takes sharing a group are adjacent, in every set and with the music take.

    The listening page opens a heading whenever the group changes, so a group
    that reappears after another one has intervened prints its heading twice
    with different takes under each — two headings that read as two subjects and
    are one.
    """
    for program in (0, 6, 40, 73):
        seen: list[str] = []
        for take in build_takes(name, program, music="bwv846-prelude"):
            if not take.group:
                continue
            if seen and seen[-1] == take.group:
                continue
            assert take.group not in seen, (
                f"{name}/{program}: group {take.group!r} reappears after "
                f"{seen[-1]!r}"
            )
            seen.append(take.group)
