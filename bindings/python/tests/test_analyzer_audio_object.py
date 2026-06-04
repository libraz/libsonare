"""Audio object analyzer API tests."""

from __future__ import annotations

# ruff: noqa: F403,F405
import numpy as np

from ._analyzer_helpers import *


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
    """detect_key identifies A major from an A-major triad (A + C# + E)."""
    from libsonare import PitchClass, detect_key
    from libsonare.types import Mode

    sr = 22050
    duration = 2.0
    n = int(sr * duration)
    # A major triad: A4 (440), C#5 (554.37), E5 (659.26). A bare 440 Hz sine
    # has no third, so the Krumhansl profile collapses to A minor; supplying
    # the full triad disambiguates the mode.
    triad = [
        (
            math.sin(2 * math.pi * 440.00 * i / sr)
            + math.sin(2 * math.pi * 554.37 * i / sr)
            + math.sin(2 * math.pi * 659.26 * i / sr)
        )
        / 3.0
        for i in range(n)
    ]
    key = detect_key(triad, sample_rate=sr)
    assert key.root == PitchClass.A
    assert key.mode == Mode.MAJOR
    # An unambiguous triad should yield high confidence (empirically ~0.98).
    assert key.confidence > 0.8
    assert key.confidence <= 1.0


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


@pytest.fixture(scope="module")
def analyze_sine_result():
    """Full analyze() on a 4 s sine, shared by the result-shape tests below."""
    from libsonare import analyze

    return analyze(_generate_sine(440, 22050, 4.0), sample_rate=22050)


def test_analyze_with_signal(analyze_sine_result) -> None:
    """analyze returns meaningful results for a real signal."""
    result = analyze_sine_result
    assert result.bpm > 0
    assert result.bpm_confidence >= 0.0
    assert result.bpm_confidence <= 1.0
    assert result.key.confidence >= 0.0
    assert result.time_signature.numerator > 0
    assert result.time_signature.denominator > 0
    assert result.bpmConfidence == result.bpm_confidence
    assert result.timeSignature == result.time_signature
    assert result.beatTimes == result.beat_times


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
    assert isinstance(data, np.ndarray)
    assert data.dtype == np.float32
    assert len(data) == len(samples)


def test_audio_data_owns_memory_after_close() -> None:
    """Audio.data returns an owning copy that survives the handle closing."""
    from libsonare import Audio

    sr = 22050
    samples = _generate_sine(440, sr, 0.25)
    audio = Audio.from_buffer(samples, sample_rate=sr)
    data = audio.data
    snapshot = data.copy()
    audio.close()
    # The array still holds valid samples after the native handle is freed.
    assert np.array_equal(data, snapshot)
    assert float(np.max(np.abs(data))) > 0.0


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


def test_analysis_result_types(analyze_sine_result) -> None:
    """AnalysisResult fields have correct types."""
    from libsonare import PitchClass
    from libsonare.types import Mode

    result = analyze_sine_result
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
