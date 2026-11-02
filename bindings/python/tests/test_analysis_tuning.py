"""Tuning offset on batch analysis, options on ``analyze_with_progress``, and the
Roman numerals ``analyze`` attaches to its chords."""

from __future__ import annotations

import inspect
import math

import pytest

import libsonare
from libsonare import ErrorCode, SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_SR = 22050


def _detuned_pop_loop(detune: float, loops: int = 1) -> list[float]:
    """I-V-vi-IV in C major at 120 BPM, re-struck every beat, every partial detuned."""
    chords = [(48, 60, 64, 67), (43, 59, 62, 67), (45, 60, 64, 69), (41, 60, 65, 69)]
    beat = int(0.5 * _SR)
    out: list[float] = []
    for _ in range(loops):
        for chord in chords:
            freqs = [440.0 * 2.0 ** ((m - 69 + detune) / 12.0) for m in chord]
            for _beat in range(4):
                for n in range(beat):
                    t = n / _SR
                    env = math.exp(-3.0 * t) * min(1.0, t * 200.0)
                    value = sum(
                        math.sin(2.0 * math.pi * f * h * t) / h for f in freqs for h in range(1, 5)
                    )
                    out.append(0.08 * env * value)
    return out


@pytest.fixture(scope="module")
def detuned() -> list[float]:
    return _detuned_pop_loop(-0.45, loops=2)


def test_estimate_tuning_reads_flat_as_negative(detuned: list[float]) -> None:
    estimate = libsonare.estimate_tuning(detuned, _SR)
    assert -0.5 < estimate < -0.4


def test_detect_chords_tuning_recovers_plain_triads(detuned: list[float]) -> None:
    estimate = libsonare.estimate_tuning(detuned, _SR)
    # The STFT front end with the full vocabulary is the one a detuned triad misleads.
    front = {"chroma_method": "stft", "use_triads_only": False}
    off_result = libsonare.detect_chords(detuned, _SR, tuning=0.0, **front)
    off = {c.name for c in off_result.chords}
    on = {c.name for c in libsonare.detect_chords(detuned, _SR, tuning=estimate, **front).chords}
    default_result = libsonare.detect_chords(detuned, _SR, **front)
    default = {c.name for c in default_result.chords}
    assert off_result.tuning == 0.0
    assert default_result.tuning == pytest.approx(-0.45, abs=0.05)
    for triad in ("C", "G", "Am", "F"):
        assert triad in on
        assert triad not in off
        assert triad in default


def test_defaults_measure_the_tuning_and_read_the_key(detuned: list[float]) -> None:
    key = libsonare.detect_key(detuned, _SR)
    assert (key.root, key.mode) == (libsonare.PitchClass.C, libsonare.Mode.MAJOR)
    assert key.tuning == pytest.approx(-0.45, abs=0.05)

    chords = libsonare.detect_chords(detuned, _SR)
    assert chords.tuning == pytest.approx(-0.45, abs=0.05)
    assert {"C", "G", "Am", "F"} <= {c.name for c in chords.chords}

    with libsonare.Audio.from_buffer(detuned, _SR) as audio:
        assert audio.detect_key().tuning == key.tuning
        assert audio.detect_chords().tuning == chords.tuning


def test_explicit_zero_tuning_is_honoured(detuned: list[float]) -> None:
    assert libsonare.detect_key(detuned, _SR, tuning=0).tuning == 0.0
    assert libsonare.detect_chords(detuned, _SR, tuning=0).tuning == 0.0
    labels = libsonare.chord_functional_analysis(
        detuned, libsonare.PitchClass.C, libsonare.Mode.MAJOR, _SR, tuning=0
    )
    assert labels


def test_key_candidates_take_a_tuning(detuned: list[float]) -> None:
    estimate = libsonare.estimate_tuning(detuned, _SR)
    c_major = (libsonare.PitchClass.C, libsonare.Mode.MAJOR)

    default = libsonare.detect_key_candidates(detuned, _SR)
    auto = libsonare.detect_key_candidates(detuned, _SR, tuning="auto")
    given = libsonare.detect_key_candidates(detuned, _SR, tuning=estimate)
    zero = libsonare.detect_key_candidates(detuned, _SR, tuning=0)
    for ranked in (default, auto, given, zero):
        assert ranked
    for ranked in (default, auto, given):
        assert (ranked[0].key.root, ranked[0].key.mode) == c_major
    assert [(c.key.root, c.key.mode) for c in default] == [(c.key.root, c.key.mode) for c in auto]
    # A given tuning reaches the core: 0 and the measured value rank differently.
    assert [c.correlation for c in zero] != [c.correlation for c in given]

    with libsonare.Audio.from_buffer(detuned, _SR) as audio:
        assert audio.detect_key_candidates(tuning=estimate)[0].key == given[0].key
        assert audio.detect_key_candidates(tuning=0)[0].correlation == zero[0].correlation


@pytest.mark.slow
def test_analyze_tuning_recentres_key_and_numerals(detuned: list[float]) -> None:
    estimate = libsonare.estimate_tuning(detuned, _SR)
    off = libsonare.analyze(detuned, _SR, tuning=0)
    on = libsonare.analyze(detuned, _SR, tuning=estimate)
    default = libsonare.analyze(detuned, _SR)
    # The constant-Q key chroma folds +-0.5 semitone into each pitch class, so the key alone
    # cannot show the shift; the tuning each call reports does.
    assert off.tuning == 0.0
    assert on.tuning == pytest.approx(estimate, abs=1e-6)
    assert default.tuning == pytest.approx(-0.45, abs=0.05)
    assert (default.key.root, default.key.mode) == (libsonare.PitchClass.C, libsonare.Mode.MAJOR)
    assert (on.key.root, on.key.mode) == (libsonare.PitchClass.C, libsonare.Mode.MAJOR)
    numerals = {c.roman_numeral for c in on.chords if c.quality != "unknown"}
    assert {"I", "V", "vi", "IV"} <= numerals

    labels = libsonare.chord_functional_analysis(
        detuned, on.key.root, on.key.mode, _SR, tuning=estimate
    )
    detected = libsonare.detect_chords(detuned, _SR, tuning=estimate).chords
    assert [c.name for c in labels] == [c.name for c in detected]
    label_by_name = {c.name: c.roman for c in labels}
    compared = 0
    for chord in on.chords:
        if chord.quality != "unknown" and chord.name in label_by_name:
            compared += 1
            assert chord.roman_numeral == label_by_name[chord.name]
    assert compared >= 4


def test_tuning_out_of_range_is_invalid(detuned: list[float]) -> None:
    for call in (
        lambda: libsonare.analyze(detuned[:_SR], _SR, tuning=0.5),
        lambda: libsonare.analyze_with_progress(detuned[:_SR], _SR, tuning=-0.6),
        lambda: libsonare.detect_chords(detuned[:_SR], _SR, tuning=0.5),
        lambda: libsonare.detect_key_candidates(detuned[:_SR], _SR, tuning=0.5),
        lambda: libsonare.chord_functional_analysis(
            detuned[:_SR], libsonare.PitchClass.C, tuning=0.5
        ),
    ):
        with pytest.raises(SonareValueError) as info:
            call()
        assert info.value.code == ErrorCode.INVALID_PARAMETER


def test_analyze_with_progress_takes_the_analyze_keywords() -> None:
    analyze_params = inspect.signature(libsonare.analyze).parameters
    progress_params = inspect.signature(libsonare.analyze_with_progress).parameters
    for name, param in analyze_params.items():
        if param.kind is inspect.Parameter.KEYWORD_ONLY:
            assert name in progress_params
            assert progress_params[name].default == param.default


def test_analyze_with_progress_matches_analyze_under_options(detuned: list[float]) -> None:
    samples = detuned[: 4 * _SR]
    stages: list[str] = []
    options = {"compute_tempo_curve": True, "tuning": -0.45}
    with_progress = libsonare.analyze_with_progress(
        samples, _SR, lambda _p, stage: stages.append(stage), **options
    )
    plain = libsonare.analyze(samples, _SR, **options)
    assert stages
    assert with_progress.beat_local_bpm == plain.beat_local_bpm
    assert len(with_progress.beat_local_bpm) > 0
    assert (with_progress.key.root, with_progress.key.mode) == (plain.key.root, plain.key.mode)
    assert [c.roman_numeral for c in with_progress.chords] == [
        c.roman_numeral for c in plain.chords
    ]
    default = libsonare.analyze_with_progress(samples, _SR)
    assert default.beat_local_bpm == []


def test_analyze_with_progress_cancel_still_honoured(detuned: list[float]) -> None:
    with pytest.raises(SonareError) as info:
        libsonare.analyze_with_progress(detuned[: 2 * _SR], _SR, cancel=lambda: True, tuning=0.1)
    assert info.value.code == ErrorCode.CANCELLED
