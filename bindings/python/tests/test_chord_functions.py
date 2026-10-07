"""Tests for chord_functions: harmonic labels for chords that are already known."""

from __future__ import annotations

import math

import pytest

import libsonare
from libsonare import Chord, Key, Mode, PitchClass

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 22050
C_MAJOR = Key(PitchClass.C, Mode.MAJOR, 1.0)


def _triad(notes: tuple[float, float, float], seconds: float) -> list[float]:
    return [
        0.25 * sum(math.sin(2 * math.pi * f * i / SR) for f in notes)
        for i in range(int(SR * seconds))
    ]


def _progression_audio() -> list[float]:
    c = (261.63, 329.63, 392.00)
    f = (349.23, 440.00, 523.25)
    g = (392.00, 493.88, 587.33)
    return _triad(c, 1.5) + _triad(f, 1.5) + _triad(g, 1.5) + _triad(c, 1.5)


def test_hand_built_progression_is_labelled_in_c_major() -> None:
    chords = [
        Chord(PitchClass.C, "major", 0.0, 1.0, 0.9),
        Chord(PitchClass.F, "major", 1.0, 2.0, 0.9),
        Chord(PitchClass.G, "major", 2.0, 3.0, 0.9),
        Chord(PitchClass.C, "major", 3.0, 4.0, 0.9),
    ]
    result = libsonare.chord_functions(chords, key=C_MAJOR)
    assert [entry.roman for entry in result] == ["I", "IV", "V", "I"]
    assert [entry.function for entry in result] == ["tonic", "subdominant", "dominant", "tonic"]
    assert [(e.start, e.end, e.root, e.quality) for e in result] == [
        (c.start, c.end, c.root, c.quality) for c in chords
    ]


def test_detected_progression_keeps_its_timing_and_gains_the_labels() -> None:
    detected = libsonare.detect_chords(
        _progression_audio(), sample_rate=SR, use_beat_sync=False, use_triads_only=True
    )
    result = libsonare.chord_functions(detected, key=C_MAJOR)
    assert len(result) == len(detected.chords) > 0
    assert [(e.start, e.end, e.root, e.quality) for e in result] == [
        (c.start, c.end, c.root, c.quality) for c in detected.chords
    ]
    # Consecutive duplicates collapse to the progression C F G C.
    collapsed = [e.roman for i, e in enumerate(result) if i == 0 or e.roman != result[i - 1].roman]
    assert collapsed == ["I", "IV", "V", "I"]
    # The result object and its chord list are interchangeable inputs.
    assert libsonare.chord_functions(detected.chords, key=C_MAJOR) == result


def test_key_may_be_a_mapping_or_a_pair() -> None:
    chords = [Chord(PitchClass.G, "dominant7", 0.0, 1.0, 0.9)]
    expected = libsonare.chord_functions(chords, key=C_MAJOR)
    assert expected[0].roman == "V7"
    assert libsonare.chord_functions(chords, key={"root": PitchClass.C, "mode": Mode.MAJOR}) == (
        expected
    )
    assert libsonare.chord_functions(chords, key=(PitchClass.C, Mode.MAJOR)) == expected


def test_minor_key_unknown_and_chromatic_chords() -> None:
    chords = [
        Chord(PitchClass.A, "minor", 0.0, 1.0, 0.9),
        Chord(PitchClass.D, "minor", 1.0, 2.0, 0.9),
        Chord(PitchClass.E, "major", 2.0, 3.0, 0.9),
        Chord(PitchClass.C, "unknown", 3.0, 4.0, 0.0),
        Chord(PitchClass.AS, "major", 4.0, 5.0, 0.9),
    ]
    result = libsonare.chord_functions(chords, key=Key(PitchClass.A, Mode.MINOR, 1.0))
    assert [e.function for e in result] == [
        "tonic",
        "subdominant",
        "dominant",
        "none",
        "chromatic",
    ]
    assert result[3].roman == "N.C."


def test_empty_input_and_refusals() -> None:
    assert libsonare.chord_functions([], key=C_MAJOR) == []
    with pytest.raises(ValueError):
        libsonare.chord_functions([Chord(PitchClass.C, "bogus", 0.0, 1.0, 0.9)], key=C_MAJOR)
    with pytest.raises(ValueError):
        libsonare.chord_functions([], key={"root": PitchClass.C})
