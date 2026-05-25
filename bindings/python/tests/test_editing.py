"""Tests for the editing DSP wrappers (pitch correct, note stretch, voice change)."""

import math

import libsonare
from libsonare import Audio


def _tone(sample_rate: int = 22050, duration: float = 0.5, freq: float = 220.0) -> list[float]:
    """Synthesize a short sine tone."""
    count = int(sample_rate * duration)
    return [0.3 * math.sin(2 * math.pi * freq * i / sample_rate) for i in range(count)]


def test_pitch_correct_to_midi_function() -> None:
    sr = 22050
    samples = _tone(sr)

    result = libsonare.pitch_correct_to_midi(
        samples, sample_rate=sr, current_midi=57.0, target_midi=60.0
    )
    assert isinstance(result, list)
    assert len(result) > 0
    assert all(math.isfinite(x) for x in result)


def test_note_stretch_function() -> None:
    sr = 22050
    samples = _tone(sr)

    result = libsonare.note_stretch(
        samples, sample_rate=sr, onset_sample=2000, offset_sample=6000, stretch_ratio=1.25
    )
    assert isinstance(result, list)
    assert len(result) > 0
    assert all(math.isfinite(x) for x in result)


def test_voice_change_function() -> None:
    sr = 22050
    samples = _tone(sr)

    result = libsonare.voice_change(
        samples, sample_rate=sr, pitch_semitones=5.0, formant_factor=1.1
    )
    assert isinstance(result, list)
    assert len(result) > 0
    assert all(math.isfinite(x) for x in result)


def test_editing_audio_methods() -> None:
    sr = 22050
    samples = _tone(sr)

    audio = Audio.from_buffer(samples, sample_rate=sr)
    try:
        corrected = audio.pitch_correct_to_midi(current_midi=57.0, target_midi=60.0)
        assert len(corrected) > 0

        stretched = audio.note_stretch(onset_sample=2000, offset_sample=6000, stretch_ratio=1.25)
        assert len(stretched) > 0

        changed = audio.voice_change(pitch_semitones=5.0, formant_factor=1.1)
        assert len(changed) > 0
    finally:
        audio.close()
