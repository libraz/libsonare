"""``tuning_cents="auto"`` on analysis and transcription, the tuning converters, and the
Mel / MFCC results that carry their forward parameters."""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_SR = 22050
# A4 = 446 Hz, as a semitone fraction.
_DETUNE = 12.0 * math.log2(446.0 / 440.0)
_DETUNE_CENTS = 100.0 * _DETUNE


def _detuned_pop_loop(detune: float) -> list[float]:
    """I-V-vi-IV in C major at 120 BPM, re-struck every beat, every partial detuned."""
    chords = [(48, 60, 64, 67), (43, 59, 62, 67), (45, 60, 64, 69), (41, 60, 65, 69)]
    beat = int(0.5 * _SR)
    out: list[float] = []
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
def loop() -> list[float]:
    return _detuned_pop_loop(_DETUNE)


@pytest.fixture(scope="module")
def measured(loop: list[float]) -> float:
    # estimate_tuning reports fractions of a bin; the analysis options speak cents.
    return 100.0 * libsonare.estimate_tuning(loop, _SR)


def test_auto_tuning_is_measured_on_every_entry_point(loop: list[float], measured: float) -> None:
    assert measured == pytest.approx(_DETUNE_CENTS, abs=3.0)
    chords = libsonare.detect_chords(loop, _SR, tuning_cents="auto")
    assert chords.tuning_cents == pytest.approx(measured, abs=1e-4)
    key = libsonare.detect_key(loop, _SR, tuning_cents="auto")
    assert key.tuning_cents == pytest.approx(measured, abs=1e-4)
    assert isinstance(key, libsonare.KeyDetection)
    assert libsonare.analyze(loop, _SR, tuning_cents="auto").tuning_cents == pytest.approx(
        measured, abs=1e-4
    )
    with libsonare.Audio.from_buffer(loop, _SR) as audio:
        assert audio.detect_key(tuning_cents="auto").tuning_cents == key.tuning_cents


def test_default_tuning_is_measured_on_every_entry_point(loop: list[float]) -> None:
    assert libsonare.detect_chords(loop, _SR).tuning_cents == pytest.approx(_DETUNE_CENTS, abs=5.0)
    key = libsonare.detect_key(loop, _SR)
    assert key.tuning_cents == pytest.approx(_DETUNE_CENTS, abs=5.0)
    assert (key.root, key.mode) == (libsonare.PitchClass.C, "major")
    assert libsonare.analyze(loop, _SR).tuning_cents == pytest.approx(_DETUNE_CENTS, abs=5.0)
    progressed = libsonare.analyze(loop, _SR, on_progress=lambda _p, _stage: None)
    assert progressed.tuning_cents == pytest.approx(_DETUNE_CENTS, abs=5.0)
    names = {c.name for c in libsonare.detect_chords(loop, _SR).chords}
    assert {"C", "G", "Am", "F"} <= names
    functional = libsonare.chord_functional_analysis(loop, _SR, libsonare.PitchClass.C, "major")
    assert {"C", "G", "Am", "F"} <= {c.name for c in functional}


def test_given_tuning_is_reported_as_given(loop: list[float]) -> None:
    assert libsonare.detect_chords(loop, _SR, tuning_cents=10.0).tuning_cents == pytest.approx(
        10.0, abs=1e-4
    )
    assert libsonare.detect_chords(loop, _SR, tuning_cents=0).tuning_cents == 0.0
    assert libsonare.detect_key(loop, _SR, tuning_cents=20.0).tuning_cents == pytest.approx(
        20.0, abs=1e-4
    )
    assert libsonare.detect_key(loop, _SR, tuning_cents=0).tuning_cents == 0.0
    assert libsonare.analyze(loop, _SR, tuning_cents=-30.0).tuning_cents == pytest.approx(
        -30.0, abs=1e-4
    )
    assert libsonare.analyze(loop, _SR, tuning_cents=0).tuning_cents == 0.0


def test_key_candidates_accept_a_number_and_auto(loop: list[float], measured: float) -> None:
    c_major = (libsonare.PitchClass.C, "major")
    for tuning in ("auto", measured, 0):
        ranked = libsonare.detect_key_candidates(loop, _SR, tuning_cents=tuning)
        assert ranked
        assert (ranked[0].key.root, ranked[0].key.mode) == c_major
    auto = libsonare.detect_key_candidates(loop, _SR, tuning_cents="auto")
    explicit = libsonare.detect_key_candidates(loop, _SR, tuning_cents=measured)
    assert [c.correlation for c in auto] == pytest.approx(
        [c.correlation for c in explicit], abs=1e-4
    )
    with pytest.raises(SonareValueError, match="tuning_cents must be a number or 'auto'"):
        libsonare.detect_key_candidates(loop, _SR, tuning_cents="other")
    with pytest.raises(SonareError):
        libsonare.detect_key_candidates(loop, _SR, tuning_cents=70.0)


def test_auto_equals_the_measured_value_applied_explicitly(
    loop: list[float], measured: float
) -> None:
    auto = libsonare.detect_key(loop, _SR, tuning_cents="auto")
    explicit = libsonare.detect_key(loop, _SR, tuning_cents=measured)
    assert (auto.root, auto.mode) == (explicit.root, explicit.mode)
    assert auto.tuning_cents == pytest.approx(explicit.tuning_cents, abs=1e-4)


def test_a_tuning_that_is_neither_a_number_nor_auto_is_refused(loop: list[float]) -> None:
    with pytest.raises(SonareValueError, match="tuning_cents must be a number or 'auto'"):
        libsonare.detect_key(loop, _SR, tuning_cents="other")
    with pytest.raises(SonareError):
        libsonare.detect_key(loop, _SR, tuning_cents=70.0)


def test_converters_round_trip_and_name_the_reference_pitch() -> None:
    assert libsonare.tuning_to_reference_hz(0.0) == pytest.approx(440.0, abs=1e-3)
    assert libsonare.tuning_to_reference_hz(_DETUNE_CENTS) == pytest.approx(446.0, abs=0.02)
    assert libsonare.reference_hz_to_tuning(446.0) == pytest.approx(_DETUNE_CENTS, abs=0.1)
    assert libsonare.tuning_to_reference_hz(0.0, 432.0) == pytest.approx(432.0, abs=1e-3)
    for tuning in (-50.0, -20.0, 23.49, 49.0, 700.0):
        hz = libsonare.tuning_to_reference_hz(tuning, 432.0)
        assert libsonare.reference_hz_to_tuning(hz, 432.0) == pytest.approx(tuning, abs=0.1)


def test_converters_refuse_values_outside_their_domain() -> None:
    with pytest.raises(SonareError):
        libsonare.tuning_to_reference_hz(float("nan"))
    with pytest.raises(SonareError):
        libsonare.tuning_to_reference_hz(0.0, 0.0)
    with pytest.raises(SonareError):
        libsonare.reference_hz_to_tuning(-1.0)


def _tones(stretch: float) -> list[float]:
    notes = (261.626, 329.628, 391.995)
    per = round(_SR * 0.4)
    gap = round(_SR * 0.06)
    out: list[float] = []
    for hz in notes:
        for i in range(per):
            envelope = min(1.0, i / 200.0, (per - i) / 200.0)
            out.append(0.5 * envelope * math.sin(2.0 * math.pi * hz * stretch * i / _SR))
        out.extend([0.0] * gap)
    return out


def test_transcribe_reference_auto_reports_the_tuning_used() -> None:
    result = libsonare.transcribe(
        _tones(2.0 ** (0.3 / 12.0)), _SR, tempo_bpm=120.0, reference_hz="auto"
    )
    assert result.note_count > 0
    assert result.tuning_cents == pytest.approx(30.0, abs=10.0)

    given = libsonare.transcribe(_tones(1.0), _SR, tempo_bpm=120.0, reference_hz=432.0)
    assert given.tuning_cents == pytest.approx(libsonare.reference_hz_to_tuning(432.0), abs=0.01)
    assert libsonare.transcribe(_tones(1.0), _SR, tempo_bpm=120.0).tuning_cents == 0.0

    with pytest.raises(SonareValueError, match="reference_hz must be a number or 'auto'"):
        libsonare.transcribe(_tones(1.0), _SR, reference_hz="other")


def test_transcribe_to_clip_accepts_auto() -> None:
    project = libsonare.Project()
    _track_id, clip_id = project.add_midi_clip(0.0, 4.0)
    notes = project.transcribe_to_clip(clip_id, _tones(1.0), _SR, reference_hz="auto")
    assert notes > 0


_MEL_SAMPLES = [0.5 * math.sin(2.0 * math.pi * 440.0 * i / _SR) for i in range(_SR // 2)]


def test_mel_result_carries_the_forward_parameters() -> None:
    mel = libsonare.mel_spectrogram(_MEL_SAMPLES, _SR, 1024, 256, 40, 100.0, None, True)
    assert (mel.power.shape[0], mel.sample_rate, mel.hop_length, mel.n_fft) == (40, _SR, 256, 1024)
    assert mel.fmin_hz == 100.0
    assert mel.fmax_hz == _SR / 2
    assert mel.htk is True
    assert not hasattr(mel, "is_db")


def test_mfcc_result_carries_the_mel_parameters_and_the_lifter() -> None:
    result = libsonare.mfcc(_MEL_SAMPLES, _SR, 1024, 256, 40, 13, 0.0, 8000.0, False, 22.0)
    assert (result.coefficients.shape[0], result.n_mels, result.sample_rate) == (13, 40, _SR)
    assert (result.hop_length, result.n_fft) == (256, 1024)
    assert result.fmax_hz == 8000.0
    assert result.htk is False
    assert not hasattr(result, "is_db")
    assert result.lifter == 22.0


def test_the_lifter_is_part_of_the_mfcc_inverse() -> None:
    result = libsonare.mfcc(_MEL_SAMPLES, _SR, 1024, 256, 40, 13, 0.0, 8000.0, False, 22.0)
    n_frames = result.coefficients.shape[1]
    lifted = libsonare.mfcc_to_mel(result.coefficients, result.n_mels, result.lifter)
    assert lifted.shape == (40, n_frames)
    # An inverse that dropped the lifter would equal this one.
    unlifted = libsonare.mfcc_to_mel(result.coefficients, result.n_mels, 0.0)
    assert not np.allclose(lifted, unlifted)


def test_a_mel_result_inverts_with_the_parameters_it_carries() -> None:
    mel = libsonare.mel_spectrogram(_MEL_SAMPLES, _SR, 1024, 256, 40, 100.0, 4000.0, True)
    stft = libsonare.mel_to_stft(mel.power, _SR, mel.n_fft, mel.fmin_hz, mel.fmax_hz, mel.htk)
    assert stft.shape == (1024 // 2 + 1, mel.power.shape[1])
    # The Mel range is part of the answer: the default range inverts differently.
    default_range = libsonare.mel_to_stft(mel.power, _SR, mel.n_fft, 0.0, None, mel.htk)
    assert not np.allclose(stft, default_range)
