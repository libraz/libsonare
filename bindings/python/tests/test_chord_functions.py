"""Tests for chord_functions: harmonic labels for chords that are already known."""

from __future__ import annotations

import inspect
import math

import pytest

import libsonare
from libsonare import Chord, Key, PitchClass

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 22050
C_MAJOR = Key(PitchClass.C, "major", 1.0)


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
    assert [entry.roman_numeral for entry in result] == ["I", "IV", "V", "I"]
    assert [entry.function for entry in result] == ["tonic", "subdominant", "dominant", "tonic"]
    assert [(e.start_sec, e.end_sec, e.root, e.quality) for e in result] == [
        (c.start_sec, c.end_sec, c.root, c.quality) for c in chords
    ]


def test_detected_progression_keeps_its_timing_and_gains_the_labels() -> None:
    detected = libsonare.detect_chords(
        _progression_audio(), sample_rate=SR, use_beat_sync=False, use_triads_only=True
    )
    result = libsonare.chord_functions(detected, key=C_MAJOR)
    assert len(result) == len(detected.chords) > 0
    assert [(e.start_sec, e.end_sec, e.root, e.quality) for e in result] == [
        (c.start_sec, c.end_sec, c.root, c.quality) for c in detected.chords
    ]
    # Consecutive duplicates collapse to the progression C F G C.
    collapsed = [
        e.roman_numeral
        for i, e in enumerate(result)
        if i == 0 or e.roman_numeral != result[i - 1].roman_numeral
    ]
    assert collapsed == ["I", "IV", "V", "I"]
    # The result object and its chord list are interchangeable inputs.
    assert libsonare.chord_functions(detected.chords, key=C_MAJOR) == result


def test_key_may_be_a_mapping_or_a_pair() -> None:
    chords = [Chord(PitchClass.G, "dominant7", 0.0, 1.0, 0.9)]
    expected = libsonare.chord_functions(chords, key=C_MAJOR)
    assert expected[0].roman_numeral == "V7"
    assert libsonare.chord_functions(chords, key={"root": PitchClass.C, "mode": "major"}) == (
        expected
    )
    assert libsonare.chord_functions(chords, key=(PitchClass.C, "major")) == expected


def test_minor_key_unknown_and_chromatic_chords() -> None:
    chords = [
        Chord(PitchClass.A, "minor", 0.0, 1.0, 0.9),
        Chord(PitchClass.D, "minor", 1.0, 2.0, 0.9),
        Chord(PitchClass.E, "major", 2.0, 3.0, 0.9),
        Chord(PitchClass.C, "unknown", 3.0, 4.0, 0.0),
        Chord(PitchClass.AS, "major", 4.0, 5.0, 0.9),
    ]
    result = libsonare.chord_functions(chords, key=Key(PitchClass.A, "minor", 1.0))
    assert [e.function for e in result] == [
        "tonic",
        "subdominant",
        "dominant",
        "none",
        "chromatic",
    ]
    assert result[3].roman_numeral == "N.C."


def test_empty_input_and_refusals() -> None:
    assert libsonare.chord_functions([], key=C_MAJOR) == []
    with pytest.raises(ValueError):
        libsonare.chord_functions([Chord(PitchClass.C, "bogus", 0.0, 1.0, 0.9)], key=C_MAJOR)
    with pytest.raises(ValueError):
        libsonare.chord_functions([], key={"root": PitchClass.C})


def test_chord_detection_defaults_and_cqt_front_end() -> None:
    for fn in (
        libsonare.detect_chords,
        libsonare.chord_functional_analysis,
        libsonare.Audio.detect_chords,
    ):
        params = inspect.signature(fn).parameters
        assert params["use_triads_only"].default is True
        assert params["use_hmm"].default is True
        assert params["chroma_method"].default == "cqt"

    audio = _progression_audio()
    assert libsonare.detect_chords(audio, SR, chroma_method="cqt").chords
    assert libsonare.chord_functional_analysis(
        audio, key_root=PitchClass.C, chroma_method="cqt", sample_rate=22050
    )
    with pytest.raises(ValueError):
        libsonare.detect_chords(audio, SR, chroma_method="bogus")


def test_analyze_defaults_skip_hpss_and_decode_chords_with_the_hmm() -> None:
    params = inspect.signature(libsonare.analyze).parameters
    assert params["use_hpss"].default is False
    assert params["use_chord_hmm"].default is True


def test_key_defaults_use_a_short_fft_and_a_chroma_high_pass() -> None:
    for entry in (
        libsonare.detect_key,
        libsonare.detect_key_candidates,
        libsonare.Audio.detect_key,
        libsonare.Audio.detect_key_candidates,
    ):
        params = inspect.signature(entry).parameters
        assert params["n_fft"].default == 2048
        assert params["chroma_high_pass_hz"].default == 80.0
