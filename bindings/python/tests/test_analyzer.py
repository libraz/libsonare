"""Tests for libsonare analyzer functions."""

from __future__ import annotations

import io
import math
import os
import struct
import sys
import tempfile
import wave
from pathlib import Path

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


## Detection with real signals


def test_detect_bpm_click_track() -> None:
    """detect_bpm detects ~120 BPM from a click track."""
    from libsonare import detect_bpm

    sr = 22050
    duration = 4.0
    bpm = 120
    n = int(sr * duration)
    samples = [0.0] * n
    samples_per_beat = sr * 60 / bpm
    for beat in range(int(duration * bpm / 60)):
        start = int(beat * samples_per_beat)
        for i in range(min(100, n - start)):
            samples[start + i] = math.sin(math.pi * i / 100)

    detected = detect_bpm(samples, sample_rate=sr)
    assert detected > bpm * 0.85
    assert detected < bpm * 1.15


def test_detect_key_a_major() -> None:
    """detect_key identifies A as root from 440 Hz sine."""
    from libsonare import PitchClass, detect_key
    from libsonare.types import Mode

    tone = _generate_sine(440, 22050, 2.0)
    key = detect_key(tone, sample_rate=22050)
    assert key.confidence > 0.0
    assert key.confidence <= 1.0
    assert isinstance(key.root, PitchClass)
    assert isinstance(key.mode, Mode)


def test_detect_beats_returns_sorted() -> None:
    """detect_beats returns sorted positive times."""
    from libsonare import detect_beats

    tone = _generate_sine(440, 22050, 4.0)
    beats = detect_beats(tone, sample_rate=22050)
    assert isinstance(beats, list)
    for i in range(1, len(beats)):
        assert beats[i] > beats[i - 1]


def test_detect_onsets_function() -> None:
    """detect_onsets returns a list of onset times."""
    from libsonare import detect_onsets

    tone = _generate_sine(440, 22050, 2.0)
    onsets = detect_onsets(tone, sample_rate=22050)
    assert isinstance(onsets, list)
    for t in onsets:
        assert t >= 0.0


## Analyze with real signal


def test_analyze_with_signal() -> None:
    """analyze returns meaningful results for a real signal."""
    from libsonare import analyze

    tone = _generate_sine(440, 22050, 4.0)
    result = analyze(tone, sample_rate=22050)
    assert result.bpm > 0
    assert result.bpm_confidence >= 0.0
    assert result.bpm_confidence <= 1.0
    assert result.key.confidence >= 0.0
    assert result.time_signature.numerator > 0
    assert result.time_signature.denominator > 0


## Audio class properties


def test_audio_properties() -> None:
    """Audio properties return correct values."""
    from libsonare import Audio

    sr = 22050
    samples = _generate_sine(440, sr, 1.0)
    audio = Audio.from_buffer(samples, sample_rate=sr)
    assert audio.length == len(samples)
    assert audio.sample_rate == sr
    assert abs(audio.duration - 1.0) < 0.01
    data = audio.data
    assert len(data) == len(samples)


def test_audio_from_file() -> None:
    """Audio.from_file loads a WAV file."""
    from libsonare import Audio

    # Create a small WAV file
    sr = 22050
    samples = _generate_sine(440, sr, 0.5)
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        wav_path = f.name
    try:
        with wave.open(wav_path, "w") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sr)
            int_samples = [int(max(-32768, min(32767, s * 32767))) for s in samples]
            wf.writeframes(struct.pack(f"<{len(int_samples)}h", *int_samples))

        audio = Audio.from_file(wav_path)
        assert audio.length > 0
        assert audio.sample_rate == sr
        assert abs(audio.duration - 0.5) < 0.05
    finally:
        os.unlink(wav_path)


def test_audio_from_memory() -> None:
    """Audio.from_memory loads WAV from bytes."""
    from libsonare import Audio

    sr = 22050
    samples = _generate_sine(440, sr, 0.5)
    buf = io.BytesIO()
    with wave.open(buf, "w") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        int_samples = [int(max(-32768, min(32767, s * 32767))) for s in samples]
        wf.writeframes(struct.pack(f"<{len(int_samples)}h", *int_samples))

    audio = Audio.from_memory(buf.getvalue())
    assert audio.length > 0
    assert audio.sample_rate == sr


## Audio class context manager and resource management


def test_audio_context_manager() -> None:
    """Audio works as context manager and releases resources."""
    from libsonare import Audio

    samples = _generate_sine(440, 22050, 0.5)
    with Audio.from_buffer(samples, sample_rate=22050) as audio:
        assert audio.length > 0
        bpm = audio.detect_bpm()
        assert isinstance(bpm, float)
    # After __exit__, handle should be cleared


## Audio class method coverage (missing methods)


def test_audio_mel_spectrogram() -> None:
    """Audio.mel_spectrogram returns correct shape."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.mel_spectrogram()
    assert result.n_mels == 128
    assert result.n_frames > 0


def test_audio_mfcc() -> None:
    """Audio.mfcc returns correct coefficient count."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.mfcc(n_mfcc=13)
    assert result.n_mfcc == 13
    assert result.n_frames > 0


def test_audio_chroma() -> None:
    """Audio.chroma returns 12 pitch classes."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.chroma()
    assert result.n_chroma == 12


def test_audio_spectral_features() -> None:
    """Audio spectral methods return non-empty lists."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    assert len(audio.spectral_centroid()) > 0
    assert len(audio.spectral_bandwidth()) > 0
    assert len(audio.spectral_rolloff()) > 0
    assert len(audio.spectral_flatness()) > 0
    assert len(audio.zero_crossing_rate()) > 0
    assert len(audio.rms_energy()) > 0


def test_audio_effects() -> None:
    """Audio effect methods return correct-length results."""
    from libsonare import Audio

    samples = _generate_sine(440, 22050, 1.0)
    audio = Audio.from_buffer(samples, sample_rate=22050)

    h = audio.harmonic()
    assert len(h) == len(samples)

    p = audio.percussive()
    assert len(p) == len(samples)

    n = audio.normalize()
    assert len(n) == len(samples)

    stretched = audio.time_stretch(rate=1.5)
    assert len(stretched) > 0
    assert len(stretched) < len(samples)

    shifted = audio.pitch_shift(semitones=2.0)
    assert len(shifted) > 0

    trimmed = audio.trim()
    assert len(trimmed) > 0
    assert len(trimmed) <= len(samples)


def test_audio_stft_db() -> None:
    """Audio.stft_db returns dB spectrogram."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    n_bins, n_frames, db = audio.stft_db()
    assert n_bins == 1025
    assert n_frames > 0
    assert len(db) == n_bins * n_frames


def test_audio_pitch_pyin() -> None:
    """Audio.pitch_pyin detects 440 Hz."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.pitch_pyin()
    assert 400 < result.median_f0 < 480


def test_audio_resample() -> None:
    """Audio.resample changes sample count."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 1.0), sample_rate=22050)
    result = audio.resample(target_sr=44100)
    assert len(result) > audio.length * 1.8


## Edge cases


def test_different_sample_rates() -> None:
    """Functions work with non-default sample rates."""
    from libsonare import detect_bpm, mel_spectrogram

    tone_44k = _generate_sine(440, 44100, 2.0)
    bpm = detect_bpm(tone_44k, sample_rate=44100)
    assert isinstance(bpm, float)
    assert bpm > 0

    mel = mel_spectrogram(tone_44k, sample_rate=44100)
    assert mel.n_mels == 128


def test_short_audio() -> None:
    """Functions handle very short audio without crashing."""
    from libsonare import mel_spectrogram, stft

    short = _generate_sine(440, 22050, 0.1)  # 100ms
    result = stft(short, sample_rate=22050)
    assert result.n_frames >= 0

    mel = mel_spectrogram(short, sample_rate=22050)
    assert mel.n_frames >= 0


def test_two_tone_chroma() -> None:
    """chroma peaks at the correct pitch classes for a C+E two-tone."""
    from libsonare import chroma

    sr = 22050
    duration = 2.0
    n = int(sr * duration)
    # C4 (261.63 Hz) + E4 (329.63 Hz)
    samples = [
        0.5 * math.sin(2 * math.pi * 261.63 * i / sr)
        + 0.5 * math.sin(2 * math.pi * 329.63 * i / sr)
        for i in range(n)
    ]
    result = chroma(samples, sample_rate=sr)
    assert result.n_chroma == 12
    assert len(result.mean_energy) == 12
    # C is index 0, E is index 4 — these should be among the highest
    max_energy = max(result.mean_energy)
    assert max_energy > 0


## Type validation


def test_analysis_result_types() -> None:
    """AnalysisResult fields have correct types."""
    from libsonare import PitchClass, analyze
    from libsonare.types import Mode

    tone = _generate_sine(440, 22050, 4.0)
    result = analyze(tone, sample_rate=22050)
    assert isinstance(result.bpm, float)
    assert isinstance(result.bpm_confidence, float)
    assert isinstance(result.key.root, PitchClass)
    assert isinstance(result.key.mode, Mode)
    assert isinstance(result.key.confidence, float)
    assert isinstance(result.time_signature.numerator, int)
    assert isinstance(result.time_signature.denominator, int)
    assert isinstance(result.beat_times, list)
    for t in result.beat_times:
        assert isinstance(t, float)


def test_stft_result_types() -> None:
    """StftResult fields have correct types and shapes."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    result = stft(tone, sample_rate=22050)
    assert isinstance(result.n_bins, int)
    assert isinstance(result.n_frames, int)
    assert isinstance(result.n_fft, int)
    assert isinstance(result.hop_length, int)
    assert isinstance(result.sample_rate, int)
    assert isinstance(result.magnitude, list)
    assert isinstance(result.power, list)
    assert result.n_fft == 2048
    assert result.hop_length == 512
    assert len(result.magnitude) == result.n_bins * result.n_frames
    assert len(result.power) == result.n_bins * result.n_frames
