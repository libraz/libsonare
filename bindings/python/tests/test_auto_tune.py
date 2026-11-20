"""Tests for scale_mask_for_mode, the named scale_mode_mask and auto_tune."""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
NOTE_SECONDS = 0.5
DETUNE_CENTS = 35.0
# pYIN quantizes pitch to 10-cent bins; the corrected pitch is judged to within one bin.
TOLERANCE_CENTS = 10.0
MELODY = [60, 64, 67, 64, 62, 65, 69, 67, 60, 64, 67, 72]

MASKS = {
    "major": 0b101010110101,
    "minor": 0b010110101101,
    "dorian": 0b011010101101,
    "phrygian": 0b010110101011,
    "lydian": 0b101011010101,
    "mixolydian": 0b011010110101,
    "locrian": 0b010101101011,
}


def _detuned_melody(detune_cents: float) -> np.ndarray:
    note_samples = int(NOTE_SECONDS * SR)
    t = np.arange(note_samples) / SR
    fade = np.minimum(
        1.0,
        np.minimum(
            np.arange(note_samples) / 200.0, (note_samples - np.arange(note_samples)) / 200.0
        ),
    )
    notes = []
    for midi in MELODY:
        hz = 440.0 * 2.0 ** ((midi - 69 + detune_cents / 100.0) / 12.0)
        tone = sum(np.sin(2 * math.pi * hz * h * t) / h for h in (1, 2, 3))
        notes.append(0.3 * fade * tone)
    return np.concatenate(notes).astype(np.float32)


def _median_midi(audio: np.ndarray, note: int) -> float:
    note_samples = int(NOTE_SECONDS * SR)
    begin = note * note_samples + note_samples // 2
    end = (note + 1) * note_samples - note_samples // 8
    pitch = libsonare.pitch_pyin(audio[begin:end], SR)
    midi = sorted(
        69.0 + 12.0 * math.log2(f0 / 440.0)
        for f0, voiced in zip(pitch.f0, pitch.voiced_flag, strict=True)
        if voiced and math.isfinite(f0)
    )
    return midi[len(midi) // 2]


def test_scale_mask_for_mode_returns_each_mode_for_every_root() -> None:
    for name, mask in MASKS.items():
        for root in (0, 5, 11):
            assert libsonare.scale_mask_for_mode(root, name) == mask
    assert libsonare.scale_mask_for_mode(0, "dorian") == MASKS["dorian"]
    assert libsonare.scale_mask_for_mode(0, "locrian") == MASKS["locrian"]


def test_scale_mask_for_mode_refuses_bad_arguments() -> None:
    with pytest.raises(libsonare.SonareValueError):
        libsonare.scale_mask_for_mode(0, "whole-tone")
    with pytest.raises(libsonare.SonareValueError, match="mode"):
        libsonare.scale_mask_for_mode(0, "Major")  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        libsonare.scale_mask_for_mode(0, 6)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        libsonare.scale_mask_for_mode(0, 7)  # type: ignore[arg-type]
    with pytest.raises(Exception):  # noqa: B017 - out-of-range root is the core's refusal
        libsonare.scale_mask_for_mode(12, "major")
    with pytest.raises(Exception):  # noqa: B017
        libsonare.scale_mask_for_mode(-1, "major")


def test_scale_mode_mask_name_equals_numeric_mask() -> None:
    samples = _detuned_melody(DETUNE_CENTS)
    pitch = libsonare.pitch_pyin(samples, SR)
    voiced = [1 if flag else 0 for flag in pitch.voiced_flag]
    for name in ("major", "dorian"):
        by_name = libsonare.pitch_correct_timevarying(
            samples, SR, pitch.f0, target_mode="scale", scale_mode_mask=name, voiced=voiced
        )
        by_mask = libsonare.pitch_correct_timevarying(
            samples, SR, pitch.f0, target_mode="scale", scale_mode_mask=MASKS[name], voiced=voiced
        )
        np.testing.assert_array_equal(by_name, by_mask)
    with pytest.raises(libsonare.SonareValueError):
        libsonare.pitch_correct_timevarying(
            samples, SR, pitch.f0, target_mode="scale", scale_mode_mask="nope", voiced=voiced
        )


def test_auto_tune_lands_a_detuned_melody_on_the_key_scale() -> None:
    audio = _detuned_melody(DETUNE_CENTS)
    result = libsonare.auto_tune(audio, SR, key=(libsonare.PitchClass.C, "major"))
    assert len(result.samples) == len(audio)
    assert result.key == libsonare.Key(libsonare.PitchClass.C, "major", 1.0)
    tuned = np.asarray(result.samples, dtype=np.float32)
    before = max(abs(_median_midi(audio, n) - m) for n, m in enumerate(MELODY))
    after = max(abs(_median_midi(tuned, n) - m) for n, m in enumerate(MELODY))
    assert before * 100.0 > DETUNE_CENTS - 10.0
    assert after * 100.0 < TOLERANCE_CENTS


def test_auto_tune_detect_equals_passing_the_detected_key() -> None:
    audio = _detuned_melody(DETUNE_CENTS)
    detected = libsonare.auto_tune(audio, SR)
    reference = libsonare.detect_key(audio, SR)
    assert detected.key.root == reference.root
    assert detected.key.mode == reference.mode
    assert detected.key.confidence == pytest.approx(reference.confidence)
    named = libsonare.auto_tune(audio, SR, key=reference)
    np.testing.assert_array_equal(named.samples, detected.samples)
    np.testing.assert_array_equal(
        libsonare.auto_tune(audio, SR, key="detect").samples, detected.samples
    )


def test_auto_tune_takes_the_key_in_several_shapes() -> None:
    audio = _detuned_melody(DETUNE_CENTS)
    pair = libsonare.auto_tune(audio, SR, key=(2, "dorian"))
    mapping = libsonare.auto_tune(audio, SR, key={"root": 2, "mode": "dorian"})
    np.testing.assert_array_equal(pair.samples, mapping.samples)
    assert pair.key.mode == "dorian"


def test_auto_tune_retune_amount_zero_leaves_the_audio_unchanged() -> None:
    audio = _detuned_melody(DETUNE_CENTS)
    result = libsonare.auto_tune(audio, SR, key=(0, "major"), retune_amount=0.0)
    assert np.max(np.abs(np.asarray(result.samples) - audio)) < 1e-3


def test_auto_tune_refuses_a_bad_key_and_retune_amount() -> None:
    audio = _detuned_melody(0.0)
    with pytest.raises(libsonare.SonareValueError):
        libsonare.auto_tune(audio, SR, key="C major")
    with pytest.raises(ValueError):
        libsonare.auto_tune(audio, SR, key=(12, "major"))
    with pytest.raises(
        libsonare.SonareValueError,
        match=r"^auto_tune: retune_amount must be finite and in \[0, 1\]$",
    ):
        libsonare.auto_tune(audio, SR, key=(0, "major"), retune_amount=2.0)
