"""Feature function analyzer API tests."""

from __future__ import annotations

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


def test_audio_analyze() -> None:
    """Audio.analyze returns a full AnalysisResult with documented silence fallbacks."""
    from libsonare import Audio, PitchClass
    from libsonare.types import Mode

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    result = audio.analyze()
    assert isinstance(result.bpm, float)
    # Silence has no tonal content, so detect_key falls back to C major.
    assert result.key.root == PitchClass.C
    assert result.key.mode == Mode.MAJOR
    assert result.key.name == "C major"
    assert isinstance(result.beatTimes, list)
    assert isinstance(result.beats, list)


def test_analysis_primitives() -> None:
    """Detailed BPM and rhythm APIs expose reusable primitives."""
    from libsonare import (
        Audio,
        analyze_bpm,
        analyze_dynamics,
        analyze_rhythm,
        analyze_timbre,
        detect_chords,
    )

    sr = 22050
    duration = 4.0
    bpm = 120
    samples = [0.0] * int(sr * duration)
    samples_per_beat = sr * 60 / bpm
    for beat in range(int(duration * bpm / 60)):
        start = int(beat * samples_per_beat)
        for i in range(start, min(start + 200, len(samples))):
            samples[i] = 1.0

    bpm_result = analyze_bpm(samples, sample_rate=sr, max_candidates=5)
    assert bpm_result.bpm > 0
    assert bpm_result.confidence >= 0.0
    assert len(bpm_result.candidates) <= 5
    assert len(bpm_result.autocorrelation) > 0
    assert len(bpm_result.tempogram) > 0

    rhythm = analyze_rhythm(samples, sample_rate=sr)
    assert rhythm.bpm > 0
    assert rhythm.time_signature.numerator > 0
    assert rhythm.groove_type in {"straight", "shuffle", "swing"}
    assert rhythm.pattern_regularity >= 0.0
    assert rhythm.tempo_stability >= 0.0

    dynamics = analyze_dynamics(samples, sample_rate=sr)
    assert dynamics.peak_db <= 1.0
    assert len(dynamics.loudness_times) == len(dynamics.loudness_rms_db)

    tone = [
        0.25
        * (
            math.sin(2 * math.pi * 261.63 * i / sr)
            + math.sin(2 * math.pi * 329.63 * i / sr)
            + math.sin(2 * math.pi * 392.00 * i / sr)
        )
        for i in range(sr * 2)
    ]
    timbre = analyze_timbre(tone, sample_rate=sr)
    assert 0.0 <= timbre.brightness <= 1.0
    assert len(timbre.spectral_centroid) > 0

    chords = detect_chords(tone, sample_rate=sr, use_beat_sync=False)
    assert isinstance(chords.chords, list)

    audio = Audio.from_buffer(samples, sample_rate=sr)
    assert audio.analyze_bpm().bpm > 0
    assert audio.analyze_rhythm().bpm > 0
    assert len(audio.analyze_dynamics().loudness_times) > 0
    assert len(audio.analyze_timbre().spectral_centroid) > 0
    assert isinstance(audio.detect_chords(use_beat_sync=False).chords, list)


def test_chord_functional_analysis() -> None:
    """Functional analysis labels each detected chord with a Roman numeral."""
    from libsonare import (
        Mode,
        PitchClass,
        chord_functional_analysis,
        detect_chords,
    )

    sr = 22050
    tone = [
        0.25
        * (
            math.sin(2 * math.pi * 261.63 * i / sr)
            + math.sin(2 * math.pi * 329.63 * i / sr)
            + math.sin(2 * math.pi * 392.00 * i / sr)
        )
        for i in range(sr * 2)
    ]

    chords = detect_chords(tone, sample_rate=sr, use_beat_sync=False)
    romans = chord_functional_analysis(
        tone,
        key_root=PitchClass.C,
        key_mode=Mode.MAJOR,
        sample_rate=sr,
        use_beat_sync=False,
    )
    assert isinstance(romans, list)
    assert len(romans) == len(chords.chords)
    assert all(isinstance(label, str) and label for label in romans)

    with pytest.raises(ValueError):
        chord_functional_analysis(
            tone,
            key_root=PitchClass.C,
            sample_rate=sr,
            chroma_method="bogus",
        )


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


def test_zero_crossing_rate_uses_edge_padding() -> None:
    """zero_crossing_rate matches librosa-style edge padding for constant signals."""
    from libsonare import zero_crossing_rate

    result = zero_crossing_rate([-1.0] * 8, sample_rate=22050, frame_length=8, hop_length=4)
    assert len(result) > 0
    assert all(v == 0.0 for v in result)


def test_rms_energy() -> None:
    """rms_energy returns non-negative values."""
    from libsonare import rms_energy

    tone = _generate_sine(440, 22050, 1.0)
    result = rms_energy(tone, sample_rate=22050)
    assert len(result) > 0
    assert all(v >= 0 for v in result)


def test_spectral_scalars_repeated_calls_are_stable() -> None:
    """Repeated calls to the spectral/scalar helpers stay correct.

    Regression guard for the C-buffer free path: each helper now frees its
    ``sonare_free_floats`` output inside a ``try/finally``. Calling them many
    times must keep returning identical, well-formed results (a smoke test for
    the buffer lifecycle; it does not directly measure native heap growth).
    """
    from libsonare import (
        rms_energy,
        spectral_bandwidth,
        spectral_centroid,
        spectral_flatness,
        spectral_rolloff,
        zero_crossing_rate,
    )

    tone = _generate_sine(440, 22050, 1.0)
    helpers = (
        spectral_centroid,
        spectral_bandwidth,
        spectral_rolloff,
        spectral_flatness,
        zero_crossing_rate,
        rms_energy,
    )
    for helper in helpers:
        first = helper(tone, sample_rate=22050)
        assert len(first) > 0
        for _ in range(64):
            again = helper(tone, sample_rate=22050)
            assert again == first


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
