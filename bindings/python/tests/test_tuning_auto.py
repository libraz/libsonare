"""``tuning="auto"`` on analysis and transcription, the tuning converters, and the
Mel / MFCC results that carry their forward parameters."""

from __future__ import annotations

import math

import pytest

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_SR = 22050
# A4 = 446 Hz, as a semitone fraction.
_DETUNE = 12.0 * math.log2(446.0 / 440.0)


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
    return libsonare.estimate_tuning(loop, _SR)


def test_auto_tuning_is_measured_on_every_entry_point(loop: list[float], measured: float) -> None:
    assert measured == pytest.approx(_DETUNE, abs=0.03)
    chords = libsonare.detect_chords(loop, _SR, tuning="auto")
    assert chords.tuning == pytest.approx(measured, abs=1e-6)
    key = libsonare.detect_key(loop, _SR, tuning="auto")
    assert key.tuning == pytest.approx(measured, abs=1e-6)
    assert isinstance(key, libsonare.KeyDetection)
    assert libsonare.analyze(loop, _SR, tuning="auto").tuning == pytest.approx(measured, abs=1e-6)
    with libsonare.Audio.from_buffer(loop, _SR) as audio:
        assert audio.detect_key(tuning="auto").tuning == key.tuning


def test_given_tuning_is_reported_as_given(loop: list[float]) -> None:
    assert libsonare.detect_chords(loop, _SR, tuning=0.1).tuning == pytest.approx(0.1, abs=1e-6)
    assert libsonare.detect_chords(loop, _SR).tuning == 0.0
    assert libsonare.detect_key(loop, _SR, tuning=0.2).tuning == pytest.approx(0.2, abs=1e-6)
    assert libsonare.detect_key(loop, _SR).tuning == 0.0
    assert libsonare.analyze(loop, _SR, tuning=-0.3).tuning == pytest.approx(-0.3, abs=1e-6)


def test_auto_equals_the_measured_value_applied_explicitly(
    loop: list[float], measured: float
) -> None:
    auto = libsonare.detect_key(loop, _SR, tuning="auto")
    explicit = libsonare.detect_key(loop, _SR, tuning=measured)
    assert (auto.root, auto.mode) == (explicit.root, explicit.mode)


def test_a_tuning_that_is_neither_a_number_nor_auto_is_refused(loop: list[float]) -> None:
    with pytest.raises(SonareValueError, match="tuning must be a number or 'auto'"):
        libsonare.detect_key(loop, _SR, tuning="other")
    with pytest.raises(SonareError):
        libsonare.detect_key(loop, _SR, tuning=0.7)


def test_converters_round_trip_and_name_the_reference_pitch() -> None:
    assert libsonare.tuning_to_reference_hz(0.0) == pytest.approx(440.0, abs=1e-3)
    assert libsonare.tuning_to_reference_hz(_DETUNE) == pytest.approx(446.0, abs=0.02)
    assert libsonare.reference_hz_to_tuning(446.0) == pytest.approx(_DETUNE, abs=1e-3)
    assert libsonare.tuning_to_reference_hz(0.0, 432.0) == pytest.approx(432.0, abs=1e-3)
    for tuning in (-0.5, -0.2, 0.2349, 0.49, 7.0):
        hz = libsonare.tuning_to_reference_hz(tuning, 432.0)
        assert libsonare.reference_hz_to_tuning(hz, 432.0) == pytest.approx(tuning, abs=1e-3)


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
    assert result.tuning == pytest.approx(0.3, abs=0.1)

    given = libsonare.transcribe(_tones(1.0), _SR, tempo_bpm=120.0, reference_hz=432.0)
    assert given.tuning == pytest.approx(libsonare.reference_hz_to_tuning(432.0), abs=1e-4)
    assert libsonare.transcribe(_tones(1.0), _SR, tempo_bpm=120.0).tuning == 0.0

    with pytest.raises(SonareValueError, match="reference_hz must be a number or 'auto'"):
        libsonare.transcribe(_tones(1.0), _SR, reference_hz="other")


def test_transcribe_to_clip_accepts_auto() -> None:
    project = libsonare.Project()
    _track_id, clip_id = project.add_midi_clip(0.0, 4.0)
    notes = project.transcribe_to_clip(clip_id, _tones(1.0), _SR, reference_hz="auto")
    assert notes > 0


_MEL_SAMPLES = [0.5 * math.sin(2.0 * math.pi * 440.0 * i / _SR) for i in range(_SR // 2)]


def test_mel_result_carries_the_forward_parameters() -> None:
    mel = libsonare.mel_spectrogram(_MEL_SAMPLES, _SR, 1024, 256, 40, 100.0, 0.0, True)
    assert (mel.n_mels, mel.sample_rate, mel.hop_length, mel.n_fft) == (40, _SR, 256, 1024)
    assert mel.fmin == 100.0
    assert mel.fmax == _SR / 2
    assert mel.htk is True
    assert mel.is_db is False


def test_mfcc_result_carries_the_mel_parameters_and_the_lifter() -> None:
    result = libsonare.mfcc(_MEL_SAMPLES, _SR, 1024, 256, 40, 13, 0.0, 8000.0, False, 22.0)
    assert (result.n_mfcc, result.n_mels, result.sample_rate) == (13, 40, _SR)
    assert (result.hop_length, result.n_fft) == (256, 1024)
    assert result.fmax == 8000.0
    assert result.htk is False
    assert result.is_db is False
    assert result.lifter == 22.0


def test_result_helpers_equal_the_explicit_inverse() -> None:
    mel = libsonare.mel_spectrogram(_MEL_SAMPLES, _SR, 1024, 256, 40, 100.0, 4000.0, True)
    assert libsonare.mel_result_to_stft(mel) == libsonare.mel_to_stft(
        mel.power, 40, mel.n_frames, _SR, 1024, 100.0, 4000.0, True
    )
    assert libsonare.mel_result_to_audio(mel, n_iter=2) == libsonare.mel_to_audio(
        mel.power, 40, mel.n_frames, _SR, 1024, 256, 100.0, 4000.0, 2, True
    )

    result = libsonare.mfcc(_MEL_SAMPLES, _SR, 1024, 256, 40, 13, 0.0, 8000.0, False, 22.0)
    from_result = libsonare.mfcc_result_to_mel(result)
    assert from_result == libsonare.mfcc_to_mel(result.coefficients, 13, result.n_frames, 40, 22.0)
    # The lifter is part of the answer: an inverse that dropped it would differ.
    assert from_result != libsonare.mfcc_to_mel(result.coefficients, 13, result.n_frames, 40, 0.0)
    assert libsonare.mfcc_result_to_audio(result, n_iter=2) == libsonare.mfcc_to_audio(
        result.coefficients, 13, result.n_frames, 40, _SR, 1024, 256, 0.0, 8000.0, 2, False, 22.0
    )


def test_a_mel_result_in_db_is_refused_naming_the_conversion() -> None:
    import dataclasses

    mel = libsonare.mel_spectrogram(_MEL_SAMPLES, _SR, 1024, 256, 40)
    in_db = dataclasses.replace(mel, power=mel.db, is_db=True)
    with pytest.raises(SonareValueError, match="db_to_power"):
        libsonare.mel_result_to_stft(in_db)
    with pytest.raises(SonareValueError, match="db_to_power"):
        libsonare.mel_result_to_audio(in_db)
