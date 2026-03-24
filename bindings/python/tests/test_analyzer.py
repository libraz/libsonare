"""Tests for libsonare analyzer functions."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import math

import pytest


def _generate_sine(freq: float, sr: int, duration: float) -> list[float]:
    """Generate a sine wave test signal."""
    n = int(sr * duration)
    return [math.sin(2 * math.pi * freq * i / sr) for i in range(n)]


def _lib_available() -> bool:
    """Check if libsonare shared library is available."""
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return True

    project_root = Path(__file__).parent.parent.parent.parent
    lib_name = "libsonare.dylib" if sys.platform == "darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    return build_path.exists()


pytestmark = pytest.mark.skipif(not _lib_available(), reason="libsonare shared library not found")


def test_version() -> None:
    """sonare_version returns a non-empty string."""
    from libsonare import version

    v = version()
    assert isinstance(v, str)
    assert len(v) > 0


def test_detect_bpm_with_silence() -> None:
    """detect_bpm does not crash on silent audio."""
    from libsonare import detect_bpm

    silence = [0.0] * 22050  # 1 second of silence
    bpm = detect_bpm(silence, sample_rate=22050)
    assert isinstance(bpm, float)


def test_detect_key_with_silence() -> None:
    """detect_key does not crash on silent audio."""
    from libsonare import detect_key

    silence = [0.0] * 22050
    key = detect_key(silence, sample_rate=22050)
    assert key.root is not None
    assert key.mode is not None
    assert isinstance(key.confidence, float)


def test_analyze_with_silence() -> None:
    """analyze does not crash on silent audio."""
    from libsonare import analyze

    silence = [0.0] * 22050
    result = analyze(silence, sample_rate=22050)
    assert isinstance(result.bpm, float)
    assert isinstance(result.bpm_confidence, float)
    assert result.key is not None
    assert result.time_signature is not None
    assert isinstance(result.beat_times, list)


def test_audio_detect_bpm() -> None:
    """Audio.detect_bpm returns a float."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    bpm = audio.detect_bpm()
    assert isinstance(bpm, float)


def test_audio_detect_key() -> None:
    """Audio.detect_key returns a Key with a root."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    key = audio.detect_key()
    assert key.root is not None


def test_audio_detect_beats() -> None:
    """Audio.detect_beats returns a list."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    beats = audio.detect_beats()
    assert isinstance(beats, list)


def test_audio_detect_onsets() -> None:
    """Audio.detect_onsets returns a list."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    onsets = audio.detect_onsets()
    assert isinstance(onsets, list)


def test_audio_analyze() -> None:
    """Audio.analyze returns a full AnalysisResult."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    result = audio.analyze()
    assert isinstance(result.bpm, float)
    assert result.key is not None


## Effects tests


def test_hpss() -> None:
    """hpss separates harmonic and percussive components."""
    from libsonare import hpss

    tone = _generate_sine(440, 22050, 1.0)
    result = hpss(tone, sample_rate=22050)
    assert len(result.harmonic) == len(tone)
    assert len(result.percussive) == len(tone)


def test_harmonic() -> None:
    """harmonic extracts harmonic component."""
    from libsonare import harmonic

    tone = _generate_sine(440, 22050, 1.0)
    result = harmonic(tone, sample_rate=22050)
    assert len(result) == len(tone)


def test_percussive() -> None:
    """percussive extracts percussive component."""
    from libsonare import percussive

    tone = _generate_sine(440, 22050, 1.0)
    result = percussive(tone, sample_rate=22050)
    assert len(result) == len(tone)


def test_time_stretch() -> None:
    """time_stretch changes audio duration."""
    from libsonare import time_stretch

    tone = _generate_sine(440, 22050, 1.0)
    fast = time_stretch(tone, sample_rate=22050, rate=2.0)
    assert len(fast) < len(tone)
    slow = time_stretch(tone, sample_rate=22050, rate=0.5)
    assert len(slow) > len(tone)


def test_pitch_shift() -> None:
    """pitch_shift returns non-empty audio."""
    from libsonare import pitch_shift

    tone = _generate_sine(440, 22050, 1.0)
    result = pitch_shift(tone, sample_rate=22050, semitones=2.0)
    assert len(result) > 0


def test_normalize() -> None:
    """normalize boosts quiet audio toward target dB."""
    from libsonare import normalize

    quiet = [0.1 * math.sin(2 * math.pi * 440 * i / 22050) for i in range(22050)]
    result = normalize(quiet, sample_rate=22050, target_db=0.0)
    assert len(result) == len(quiet)
    peak = max(abs(v) for v in result)
    assert peak > 0.8


def test_trim() -> None:
    """trim removes leading and trailing silence."""
    from libsonare import trim

    # silence + tone + silence
    n = 22050
    samples = [0.0] * n
    start, end = n // 4, 3 * n // 4
    for i in range(start, end):
        samples[i] = 0.5 * math.sin(2 * math.pi * 440 * i / 22050)
    result = trim(samples, sample_rate=22050, threshold_db=-40.0)
    assert len(result) < len(samples)
    assert len(result) > 0


## Feature tests


def test_stft() -> None:
    """stft returns correct bin and frame dimensions."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    result = stft(tone, sample_rate=22050)
    assert result.n_bins == 1025
    assert result.n_frames > 0
    assert len(result.magnitude) == result.n_bins * result.n_frames


def test_stft_db() -> None:
    """stft_db returns dB-scaled spectrogram."""
    from libsonare import stft_db

    tone = _generate_sine(440, 22050, 1.0)
    n_bins, n_frames, db = stft_db(tone, sample_rate=22050)
    assert n_bins == 1025
    assert n_frames > 0
    assert len(db) == n_bins * n_frames


def test_mel_spectrogram() -> None:
    """mel_spectrogram returns correct mel band count."""
    from libsonare import mel_spectrogram

    tone = _generate_sine(440, 22050, 1.0)
    result = mel_spectrogram(tone, sample_rate=22050)
    assert result.n_mels == 128
    assert result.n_frames > 0


def test_mfcc() -> None:
    """mfcc returns correct coefficient dimensions."""
    from libsonare import mfcc

    tone = _generate_sine(440, 22050, 1.0)
    result = mfcc(tone, sample_rate=22050, n_mels=64, n_mfcc=13)
    assert result.n_mfcc == 13
    assert result.n_frames > 0
    assert len(result.coefficients) == result.n_mfcc * result.n_frames


def test_chroma() -> None:
    """chroma returns 12 pitch classes with mean energy."""
    from libsonare import chroma

    tone = _generate_sine(440, 22050, 1.0)
    result = chroma(tone, sample_rate=22050)
    assert result.n_chroma == 12
    assert result.n_frames > 0
    assert len(result.mean_energy) == 12


def test_spectral_centroid() -> None:
    """spectral_centroid returns non-empty list."""
    from libsonare import spectral_centroid

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_centroid(tone, sample_rate=22050)
    assert len(result) > 0


def test_spectral_bandwidth() -> None:
    """spectral_bandwidth returns non-empty list."""
    from libsonare import spectral_bandwidth

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_bandwidth(tone, sample_rate=22050)
    assert len(result) > 0


def test_spectral_rolloff() -> None:
    """spectral_rolloff returns non-empty list."""
    from libsonare import spectral_rolloff

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_rolloff(tone, sample_rate=22050)
    assert len(result) > 0


def test_spectral_flatness() -> None:
    """spectral_flatness returns non-empty list."""
    from libsonare import spectral_flatness

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_flatness(tone, sample_rate=22050)
    assert len(result) > 0


def test_zero_crossing_rate() -> None:
    """zero_crossing_rate returns values in [0, 1]."""
    from libsonare import zero_crossing_rate

    tone = _generate_sine(440, 22050, 1.0)
    result = zero_crossing_rate(tone, sample_rate=22050)
    assert len(result) > 0
    assert all(0 <= v <= 1 for v in result)


def test_rms_energy() -> None:
    """rms_energy returns non-negative values."""
    from libsonare import rms_energy

    tone = _generate_sine(440, 22050, 1.0)
    result = rms_energy(tone, sample_rate=22050)
    assert len(result) > 0
    assert all(v >= 0 for v in result)


def test_pitch_yin() -> None:
    """pitch_yin detects 440 Hz tone."""
    from libsonare import pitch_yin

    tone = _generate_sine(440, 22050, 1.0)
    result = pitch_yin(tone, sample_rate=22050)
    assert result.n_frames > 0
    assert 400 < result.median_f0 < 480


def test_pitch_pyin() -> None:
    """pitch_pyin detects 440 Hz tone."""
    from libsonare import pitch_pyin

    tone = _generate_sine(440, 22050, 1.0)
    result = pitch_pyin(tone, sample_rate=22050)
    assert result.n_frames > 0
    assert 400 < result.median_f0 < 480


## Conversion tests


def test_hz_to_mel() -> None:
    """hz_to_mel returns positive value for 440 Hz."""
    from libsonare import hz_to_mel

    assert hz_to_mel(440.0) > 0


def test_mel_to_hz() -> None:
    """mel_to_hz round-trips with hz_to_mel."""
    from libsonare import hz_to_mel, mel_to_hz

    mel = hz_to_mel(440.0)
    hz = mel_to_hz(mel)
    assert abs(hz - 440.0) < 1.0


def test_hz_to_midi() -> None:
    """hz_to_midi maps A4 (440 Hz) to MIDI 69."""
    from libsonare import hz_to_midi

    assert abs(hz_to_midi(440.0) - 69.0) < 0.01


def test_midi_to_hz() -> None:
    """midi_to_hz maps MIDI 69 to 440 Hz."""
    from libsonare import midi_to_hz

    assert abs(midi_to_hz(69.0) - 440.0) < 0.01


def test_hz_to_note() -> None:
    """hz_to_note maps 440 Hz to A4."""
    from libsonare import hz_to_note

    assert hz_to_note(440.0) == "A4"


def test_note_to_hz() -> None:
    """note_to_hz maps A4 to ~440 Hz."""
    from libsonare import note_to_hz

    assert abs(note_to_hz("A4") - 440.0) < 1.0


def test_frames_to_time() -> None:
    """frames_to_time converts frame index to seconds."""
    from libsonare import frames_to_time

    t = frames_to_time(1, 22050, 512)
    assert abs(t - 512 / 22050) < 1e-5


def test_time_to_frames() -> None:
    """time_to_frames converts seconds to frame index."""
    from libsonare import time_to_frames

    f = time_to_frames(1.0, 22050, 512)
    assert f == 22050 // 512


## Resample test


def test_resample() -> None:
    """resample changes sample count proportionally."""
    from libsonare import resample

    tone = _generate_sine(440, 22050, 1.0)
    result = resample(tone, src_sr=22050, target_sr=44100)
    assert len(result) > len(tone) * 1.8
    assert len(result) < len(tone) * 2.2


## Audio class method tests


def test_audio_stft() -> None:
    """Audio.stft returns correct bin count."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.stft()
    assert result.n_bins == 1025


def test_audio_hpss() -> None:
    """Audio.hpss returns harmonic component matching input length."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.hpss()
    assert len(result.harmonic) == audio.length


def test_audio_pitch_yin() -> None:
    """Audio.pitch_yin detects 440 Hz tone."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.pitch_yin()
    assert 400 < result.median_f0 < 480


def test_invalid_sample_rate() -> None:
    """Invalid sample rate raises RuntimeError."""
    from libsonare import detect_bpm

    silence = [0.0] * 100
    with pytest.raises(RuntimeError):
        detect_bpm(silence, sample_rate=0)
