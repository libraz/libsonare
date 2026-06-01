"""Tests for libsonare analyzer functions."""

from __future__ import annotations

import io
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE


def _generate_sine(freq: float, sr: int, duration: float) -> list[float]:
    """Generate a sine wave test signal."""
    n = int(sr * duration)
    return [math.sin(2 * math.pi * freq * i / sr) for i in range(n)]


pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def _ffmpeg_cli() -> str | None:
    """Locate the ffmpeg CLI on PATH; return None when unavailable."""
    return shutil.which("ffmpeg")


def _has_ffmpeg_build_support() -> bool:
    """Return whether the loaded libsonare was compiled with FFmpeg support.

    Safe to call at collection time: imports lazily so we don't fail when the
    shared library is missing (the pytestmark above already skips in that case).
    """
    try:
        import libsonare

        return libsonare.has_ffmpeg_support()
    except Exception:
        return False


def test_version() -> None:
    """sonare_version returns a non-empty string."""
    from libsonare import version

    v = version()
    assert isinstance(v, str)
    assert len(v) > 0


def test_has_ffmpeg_support_returns_bool() -> None:
    """has_ffmpeg_support is exposed and returns a bool."""
    import libsonare

    assert isinstance(libsonare.has_ffmpeg_support(), bool)


@pytest.mark.skipif(
    not _has_ffmpeg_build_support() or _ffmpeg_cli() is None,
    reason="requires libsonare built with FFmpeg and ffmpeg CLI on PATH",
)
def test_audio_from_file_decodes_m4a() -> None:
    """Audio.from_file decodes a real .m4a (AAC) file when FFmpeg is linked."""
    import libsonare

    ffmpeg = _ffmpeg_cli()
    assert ffmpeg is not None  # narrowed for the type checker; pytest.skip above guards
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        m4a_path = os.path.join(tmpdir, "tone.m4a")
        subprocess.run(
            [
                ffmpeg,
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:duration=0.5:sample_rate=22050",
                "-ac",
                "1",
                "-y",
                wav_path,
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                ffmpeg,
                "-i",
                wav_path,
                "-c:a",
                "aac",
                "-b:a",
                "64k",
                "-y",
                m4a_path,
            ],
            check=True,
            capture_output=True,
        )
        audio = libsonare.Audio.from_file(m4a_path)
        try:
            assert audio.length > 1000
            assert audio.sample_rate > 0
            data = audio.data
            assert isinstance(data, list)
            assert len(data) == audio.length
            peak = max(abs(v) for v in data)
            assert 0.01 < peak <= 1.0
        finally:
            audio.close()


def test_dunder_version_attribute() -> None:
    """libsonare.__version__ is exposed as a non-empty string."""
    import libsonare

    assert isinstance(libsonare.__version__, str)
    assert len(libsonare.__version__) > 0


def test_unsupported_format_error_is_actionable() -> None:
    """Loading an unsupported format raises a helpful, format-specific error."""
    from libsonare import Audio

    # Write a small file with an .m4a extension but no real audio payload, so
    # we can verify the error path even without an actual M4A decoder. (When
    # the library is built with FFmpeg support, this path raises a decode-time
    # error from FFmpeg instead.)
    with tempfile.NamedTemporaryFile(suffix=".m4a", delete=False) as f:
        f.write(b"not really an m4a file")
        path = f.name

    try:
        with pytest.raises(RuntimeError) as exc:
            Audio.from_file(path)
        msg = str(exc.value)
        # Must always carry *some* concrete signal, not just "Invalid format".
        assert msg != "Invalid format"
        assert msg.lower() != "decode failed"
        # Default (no-FFmpeg) build surfaces the actionable hint; FFmpeg build
        # surfaces FFmpeg's own error string. Either way the message must be
        # meaningfully long.
        assert len(msg) > 20
    finally:
        os.unlink(path)


def test_detect_bpm_with_silence() -> None:
    """detect_bpm does not crash on silent audio."""
    from libsonare import detect_bpm

    silence = [0.0] * 22050  # 1 second of silence
    bpm = detect_bpm(silence, sample_rate=22050)
    assert isinstance(bpm, float)


def test_detect_key_with_silence() -> None:
    """detect_key returns the documented C-major fallback on silence."""
    from libsonare import PitchClass, detect_key
    from libsonare.types import Mode

    silence = [0.0] * 22050
    key = detect_key(silence, sample_rate=22050)
    # Silence has no tonal content, so detect_key falls back to C major.
    assert key.root == PitchClass.C
    assert key.mode == Mode.MAJOR
    assert isinstance(key.confidence, float)
    # Confidence on silence stays low (empirically ~0.25).
    assert key.confidence < 0.5


def test_analyze_with_silence() -> None:
    """analyze returns the documented fallbacks on silent audio."""
    from libsonare import PitchClass, analyze
    from libsonare.types import Mode

    silence = [0.0] * 22050
    result = analyze(silence, sample_rate=22050)
    assert isinstance(result.bpm, float)
    assert isinstance(result.bpm_confidence, float)
    # Silence has no tonal content: detect_key falls back to C major.
    assert result.key.root == PitchClass.C
    assert result.key.mode == Mode.MAJOR
    # Time signature falls back to 4/4 when no rhythmic content is present.
    assert result.time_signature.numerator == 4
    assert result.time_signature.denominator == 4
    assert isinstance(result.beat_times, list)


def test_compat_numeric_and_signal_utilities() -> None:
    """Compatibility utilities are available through Python."""
    import libsonare

    assert libsonare.frames_to_samples(4, hop_length=512) == 2048
    assert libsonare.samples_to_frames(2048, hop_length=512) == 4

    power_db = libsonare.power_to_db([1.0, 0.01], ref=1.0, amin=1e-10, top_db=80.0)
    assert power_db[0] == pytest.approx(0.0, abs=1e-5)
    assert power_db[1] == pytest.approx(-20.0, abs=1e-4)
    assert libsonare.db_to_power(power_db, ref=1.0)[1] == pytest.approx(0.01, rel=1e-5)

    amp_db = libsonare.amplitude_to_db([1.0, 0.5], ref=1.0, amin=1e-5, top_db=80.0)
    assert amp_db[0] == pytest.approx(0.0, abs=1e-5)
    assert libsonare.db_to_amplitude(amp_db, ref=1.0)[1] == pytest.approx(0.5, rel=1e-5)

    emphasized = libsonare.preemphasis([1.0, 1.0, 1.0], coef=0.5, zi=0.0)
    assert emphasized == pytest.approx([1.0, 0.5, 0.5])
    assert libsonare.deemphasis(emphasized, coef=0.5, zi=0.0)[2] == pytest.approx(1.0, abs=1e-5)

    framed = libsonare.frame_signal([1.0, 2.0, 3.0, 4.0], frame_length=2, hop_length=1)
    assert framed[0] == 3
    assert framed[1] == pytest.approx([1.0, 2.0, 2.0, 3.0, 3.0, 4.0])
    assert libsonare.fix_length([1.0, 2.0], target_size=4, pad_value=-1.0) == pytest.approx(
        [1.0, 2.0, -1.0, -1.0]
    )
    assert libsonare.fix_frames([2, 4], x_min=0, x_max=5, pad=True) == [0, 2, 4, 5]
    assert libsonare.peak_pick([0.0, 1.0, 0.0, 2.0, 0.0], 1, 1, 1, 1, 0.0, 0) == [1, 3]
    assert libsonare.vector_normalize([3.0, 4.0], norm_type=2) == pytest.approx([0.6, 0.8])


def test_compat_silence_and_rhythm_utilities() -> None:
    """Silence/rhythm compatibility utilities are available through Python."""
    import libsonare

    samples = [0.0, 0.0, 1.0, 1.0, 0.0, 0.0]
    trimmed = libsonare.trim_silence(samples, top_db=20.0, frame_length=2, hop_length=1)
    assert len(trimmed[0]) > 0
    assert trimmed[2] > trimmed[1]
    intervals = libsonare.split_silence(samples, top_db=20.0, frame_length=2, hop_length=1)
    assert isinstance(intervals, list)

    pcen_values = libsonare.pcen([1.0, 2.0, 3.0, 4.0], n_bins=2, n_frames=2)
    assert len(pcen_values) == 4

    chroma = [0.0] * 24
    chroma[0] = 1.0
    chroma[12] = 1.0
    tonnetz_values = libsonare.tonnetz(chroma, n_chroma=12, n_frames=2)
    assert len(tonnetz_values) == 12

    onset = [0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0]
    temp = libsonare.tempogram(onset, sample_rate=22050, hop_length=512, win_length=4)
    assert temp[0] == len(onset)
    assert len(temp[1]) == 4 * len(onset)
    cosine = libsonare.tempogram(
        onset, sample_rate=22050, hop_length=512, win_length=4, mode="cosine"
    )
    assert cosine[0] == len(onset)
    assert len(cosine[1]) == 4 * len(onset)
    assert all(-1.000001 <= value <= 1.000001 for value in cosine[1])
    assert len(libsonare.plp(onset, sample_rate=22050, hop_length=512, win_length=4)) == len(onset)


def test_audio_detect_bpm() -> None:
    """Audio.detect_bpm returns a float."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    bpm = audio.detect_bpm()
    assert isinstance(bpm, float)


def test_audio_detect_key() -> None:
    """Audio.detect_key returns the documented C-major fallback on silence."""
    from libsonare import Audio, PitchClass
    from libsonare.types import Mode

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    key = audio.detect_key()
    # Silence has no tonal content, so detect_key falls back to C major.
    assert key.root == PitchClass.C
    assert key.mode == Mode.MAJOR
    # Name accessors must agree with the resolved root/mode.
    assert key.name == "C major"
    assert key.short_name == "C"
    assert key.shortName == key.short_name


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


def test_mastering_returns_audio_and_loudness_metadata() -> None:
    """mastering is exposed as a standalone function and Audio method."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr)]

    result = libsonare.mastering(samples, sample_rate=sr, target_lufs=-18.0)
    assert isinstance(result.samples, list)
    assert len(result.samples) == len(samples)
    assert result.sample_rate == sr
    assert math.isfinite(result.input_lufs)
    assert math.isfinite(result.output_lufs)
    assert math.isfinite(result.applied_gain_db)
    assert result.output_lufs == pytest.approx(-18.0, abs=0.1)

    audio = libsonare.Audio.from_buffer(samples, sample_rate=sr)
    try:
        audio_result = audio.mastering(target_lufs=-18.0)
        assert len(audio_result.samples) == len(samples)
        assert audio_result.sample_rate == sr
    finally:
        audio.close()


def test_named_mastering_processors() -> None:
    """Named mastering processors are exposed through the shared API."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 2)]

    names = libsonare.mastering_processor_names()
    assert "dynamics.compressor" in names
    assert "eq.equalizer" in names
    assert "stereo.imager" in names

    result = libsonare.mastering_process(
        "dynamics.compressor",
        samples,
        sample_rate=sr,
        params={"thresholdDb": -24.0, "ratio": 1.5},
    )
    assert len(result.samples) == len(samples)
    assert math.isfinite(result.output_lufs)

    eq_result = libsonare.mastering_process(
        "eq.equalizer",
        samples,
        sample_rate=sr,
        params={
            "band0.enabled": 1.0,
            "band0.frequencyHz": 440.0,
            "band0.gainDb": 6.0,
            "band0.q": 1.0,
            "autoGain": 1.0,
        },
    )
    assert len(eq_result.samples) == len(samples)
    assert math.isfinite(eq_result.output_lufs)

    stereo = libsonare.mastering_process_stereo(
        "stereo.imager",
        samples,
        samples,
        sample_rate=sr,
        params={"width": 1.1},
    )
    assert len(stereo.left) == len(samples)
    assert len(stereo.right) == len(samples)


def test_mastering_pair_and_stereo_analysis() -> None:
    """Pair and stereo mastering APIs return shared processor output/JSON."""
    import libsonare

    sr = 44100
    samples = [0.18 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 4)]
    reference = [0.12 * math.sin(2 * math.pi * 880 * i / sr) for i in range(sr // 4)]

    assert "match.abCrossfade" in libsonare.mastering_pair_processor_names()
    assert "match.referenceLoudness" in libsonare.mastering_pair_analysis_names()
    assert "stereo.monoCompatCheck" in libsonare.mastering_stereo_analysis_names()

    paired = libsonare.mastering_pair_process(
        "match.abCrossfade",
        samples,
        reference,
        sample_rate=sr,
        params={"mix": 0.25},
    )
    assert len(paired.samples) == len(samples)

    pair_json = libsonare.mastering_pair_analyze(
        "match.referenceLoudness",
        samples,
        reference,
        sample_rate=sr,
    )
    assert '"sourceLufs"' in pair_json
    assert '"referenceLufs"' in pair_json

    stereo_json = libsonare.mastering_stereo_analyze(
        "stereo.monoCompatCheck",
        samples,
        reference,
        sample_rate=sr,
    )
    assert '"correlation"' in stereo_json


def test_mastering_streaming_preview_returns_shared_json() -> None:
    """Streaming preview is exposed through the Python mastering API."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(sr)]

    preview_json = libsonare.mastering_streaming_preview(
        samples,
        sample_rate=sr,
        platforms=[{"name": "Unit Test", "targetLufs": 0.0, "ceilingDb": -6.0}],
    )

    assert '"platforms"' in preview_json
    assert '"name":"Unit Test"' in preview_json
    assert '"normalizationGainDb"' in preview_json
    assert '"ceilingRisk":true' in preview_json


def test_mastering_assistant_suggest_returns_shared_json() -> None:
    """Mastering assistant suggestion is exposed through Python."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 220 * i / sr) for i in range(sr * 3)]

    suggestion_json = libsonare.mastering_assistant_suggest(
        samples,
        sample_rate=sr,
        params={"targetLufs": -13.0, "ceilingDb": -0.8},
    )

    assert '"chainConfig"' in suggestion_json
    assert '"explanation"' in suggestion_json
    assert '"genreCandidates"' in suggestion_json
    assert '"loudness.targetLufs":-13' in suggestion_json
    assert '"loudness.ceilingDb":-0.8' in suggestion_json


def test_mastering_audio_profile_returns_shared_json() -> None:
    """Mastering assistant audio profile is exposed through Python."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 330 * i / sr) for i in range(sr * 2)]

    profile_json = libsonare.mastering_audio_profile(
        samples,
        sample_rate=sr,
        params={"nFft": 1024, "hopLength": 256},
    )

    assert '"durationSec"' in profile_json
    assert '"loudness"' in profile_json
    assert '"integratedLufs"' in profile_json
    assert '"spectral"' in profile_json
    assert '"centroidHz"' in profile_json
    assert '"dynamics"' in profile_json
    assert '"genreCandidates"' in profile_json


def test_mixing_presets_and_stereo_mix() -> None:
    """Mixing scene presets and simple stereo mix are exposed."""
    import libsonare

    assert "vocalReverbSend" in libsonare.mixing_scene_preset_names()
    assert '"vocal"' in libsonare.mixing_scene_preset_json("vocalReverbSend")

    result = libsonare.mix_stereo(
        [([1.0, 1.0], [0.0, 0.0])],
        sample_rate=48000,
        fader_db=[-6.0206],
        input_trim_db=[6.0206],
    )
    assert result.left == pytest.approx([math.sqrt(0.5), math.sqrt(0.5)], abs=0.004)
    assert result.right == pytest.approx([0.0, 0.0], abs=0.0002)
    assert result.sample_rate == 48000
    assert len(result.meters) == 1
    assert math.isfinite(result.meters[0].peak_db_l)
    assert isinstance(result.meters[0].likely_mono_compatible, bool)


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


def test_streaming_mastering_chain_processes_mono_block() -> None:
    """StreamingMasteringChain processes a 512-sample mono block in place."""
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
    chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
    block = [0.1] * 512
    out = chain.process_mono(block)
    assert len(out) == len(block)
    assert any(abs(out[i] - block[i]) > 1e-6 for i in range(len(out)))
    chain.reset()


def test_streaming_equalizer_processes_blocks_and_exposes_spectrum() -> None:
    """StreamingEqualizer exposes the native SonareEq handle to Python."""
    from libsonare import StreamingEqualizer

    sr = 48000
    block = [0.2 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(512)]

    with StreamingEqualizer(sample_rate=sr, max_block_size=512) as eq:
        eq.set_gain_scale(0.5)
        eq.set_output_gain_db(3.0)
        eq.set_output_pan(0.0)
        eq.set_band(
            0,
            {
                "type": "Peak",
                "frequencyHz": 1000.0,
                "gainDb": 6.0,
                "q": 1.0,
                "enabled": True,
            },
        )
        out = eq.process_mono(block)
        assert len(out) == len(block)
        assert max(abs(sample) for sample in out) > max(abs(sample) for sample in block)

        snapshot = eq.spectrum()
        assert snapshot.seq == 1
        assert len(snapshot.pre_left) == 256
        assert len(snapshot.post_left) == 256
        assert 2.5 < snapshot.band_gain_db[0] < 3.5

        eq.set_phase_mode("linear")
        assert eq.latency_samples > 0


def test_streaming_equalizer_match_configures_bands() -> None:
    """StreamingEqualizer.match forwards to the live EQ match C API."""
    from libsonare import StreamingEqualizer

    sr = 48000
    source = [0.08 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(1024)]
    reference = [0.35 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(1024)]

    with StreamingEqualizer(sample_rate=sr, max_block_size=len(source)) as eq:
        eq.match(source, reference, max_bands=4)
        out = eq.process_mono(source)
        assert len(out) == len(source)
        assert any(gain > 0.5 for gain in eq.spectrum().band_gain_db)


def test_streaming_mastering_chain_rejects_denoise() -> None:
    """StreamingMasteringChain refuses configurations enabling repair.denoise."""
    from libsonare import StreamingMasteringChain

    with pytest.raises(RuntimeError):
        StreamingMasteringChain({"repair.denoise.enabled": 1})


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


def test_mastering_chain_invokes_progress_callback() -> None:
    """mastering_chain forwards per-stage progress to on_progress callback."""
    from libsonare import mastering_chain

    calls: list[tuple[float, str]] = []

    def on_progress(progress: float, stage: str) -> None:
        calls.append((progress, stage))

    result = mastering_chain(
        samples=[0.1] * 22050,
        sample_rate=22050,
        config={"eq.tilt.tiltDb": 1.0, "dynamics.compressor.thresholdDb": -24.0},
        on_progress=on_progress,
    )
    assert len(result.samples) == 22050
    assert len(calls) >= 2
    stages = [s for _, s in calls]
    assert "eq.tilt" in stages
    assert "dynamics.compressor" in stages
    # Final progress reaches 1.0.
    assert calls[-1][0] == pytest.approx(1.0, abs=1e-5)


def test_color_saturation_stages_engage_only_when_meaningful() -> None:
    """Tape/exciter must not auto-engage on a zero-valued config (no coloration)."""
    from libsonare import mastering_chain

    def stages_for(config: dict) -> list[str]:
        return mastering_chain(samples=[0.1] * 22050, sample_rate=22050, config=config).stages

    # A zero-amount exciter / zero-drive tape is a no-op and stays bypassed.
    assert "saturation.exciter" not in stages_for({"saturation.exciter.amount": 0.0})
    assert "saturation.tape" not in stages_for(
        {"saturation.tape.driveDb": 0.0, "saturation.tape.saturation": 0.0}
    )
    # Meaningful params engage the stage.
    assert "saturation.exciter" in stages_for({"saturation.exciter.amount": 0.2})
    # An explicit enabled flag wins either way.
    assert "saturation.exciter" in stages_for(
        {"saturation.exciter.amount": 0.0, "saturation.exciter.enabled": True}
    )
    assert "saturation.tape" not in stages_for(
        {"saturation.tape.driveDb": 3.0, "saturation.tape.enabled": False}
    )


# ============================================================================
# Newly exposed analysis functions: onset envelope, Fourier tempogram,
# tempogram ratio, NNLS chroma, and LUFS loudness metering.
# ============================================================================


def _all_finite(values: list[float]) -> bool:
    return all(math.isfinite(v) for v in values)


def test_onset_envelope() -> None:
    """onset_envelope returns a finite per-frame envelope."""
    from libsonare import onset_envelope

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_envelope(samples, sample_rate=22050)
    assert len(env) > 0
    assert _all_finite(env)


def test_fourier_tempogram() -> None:
    """fourier_tempogram returns an [n_bins x n_frames] magnitude matrix."""
    from libsonare import fourier_tempogram, onset_envelope

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_envelope(samples, sample_rate=22050)
    win_length = 384
    n_frames, data = fourier_tempogram(env, sample_rate=22050, win_length=win_length)
    assert n_frames == len(env)
    n_bins = win_length // 2 + 1
    assert len(data) == n_bins * n_frames
    assert _all_finite(data)


def test_tempogram_ratio() -> None:
    """tempogram_ratio returns one finite value per factor."""
    from libsonare import onset_envelope, tempogram, tempogram_ratio

    samples = _generate_sine(440, 22050, 2.0)
    env = onset_envelope(samples, sample_rate=22050)
    win_length = 384
    _, tg = tempogram(env, sample_rate=22050)

    default_ratio = tempogram_ratio(tg, win_length=win_length, sample_rate=22050)
    assert len(default_ratio) == 5  # {0.5, 1, 2, 3, 4}
    assert _all_finite(default_ratio)

    explicit = tempogram_ratio(
        tg, win_length=win_length, sample_rate=22050, factors=[1.0, 2.0, 3.0]
    )
    assert len(explicit) == 3
    assert _all_finite(explicit)


def test_nnls_chroma() -> None:
    """nnls_chroma returns a row-major 12 x n_frames matrix."""
    from libsonare import nnls_chroma

    samples = _generate_sine(440, 22050, 2.0)
    n_frames, data = nnls_chroma(samples, sample_rate=22050)
    assert n_frames > 0
    assert len(data) == 12 * n_frames
    assert _all_finite(data)
    assert all(v >= 0.0 for v in data)  # NNLS output is non-negative


def test_lufs() -> None:
    """lufs returns finite loudness measures; louder signal reads higher."""
    from libsonare import lufs

    loud = _generate_sine(440, 48000, 3.0)
    quiet = [s * 0.1 for s in loud]

    loud_result = lufs(loud, sample_rate=48000)
    quiet_result = lufs(quiet, sample_rate=48000)

    for r in (loud_result, quiet_result):
        assert math.isfinite(r.integrated_lufs)
        assert math.isfinite(r.momentary_lufs)
        assert math.isfinite(r.short_term_lufs)
        assert math.isfinite(r.loudness_range)
        assert r.loudness_range >= 0.0

    assert loud_result.integrated_lufs > quiet_result.integrated_lufs


def test_momentary_and_short_term_lufs() -> None:
    """momentary_lufs and short_term_lufs return finite time series."""
    from libsonare import momentary_lufs, short_term_lufs

    samples = _generate_sine(440, 48000, 3.0)

    momentary = momentary_lufs(samples, sample_rate=48000)
    assert len(momentary) > 0
    assert _all_finite(momentary)

    short_term = short_term_lufs(samples, sample_rate=48000)
    assert len(short_term) > 0
    assert _all_finite(short_term)


def test_audio_onset_envelope() -> None:
    """Audio.onset_envelope returns a finite per-frame envelope."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 2.0), sample_rate=22050)
    env = audio.onset_envelope()
    assert len(env) > 0
    assert _all_finite(env)


def test_audio_nnls_chroma() -> None:
    """Audio.nnls_chroma returns a row-major 12 x n_frames matrix."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 22050, 2.0), sample_rate=22050)
    n_frames, data = audio.nnls_chroma()
    assert n_frames > 0
    assert len(data) == 12 * n_frames
    assert _all_finite(data)


def test_audio_lufs() -> None:
    """Audio.lufs returns finite loudness measures."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 48000, 3.0), sample_rate=48000)
    result = audio.lufs()
    assert math.isfinite(result.integrated_lufs)
    assert math.isfinite(result.momentary_lufs)
    assert math.isfinite(result.short_term_lufs)
    assert math.isfinite(result.loudness_range)


def test_audio_momentary_and_short_term_lufs() -> None:
    """Audio.momentary_lufs and Audio.short_term_lufs return finite series."""
    from libsonare import Audio

    audio = Audio.from_buffer(_generate_sine(440, 48000, 3.0), sample_rate=48000)
    momentary = audio.momentary_lufs()
    short_term = audio.short_term_lufs()
    assert len(momentary) > 0
    assert len(short_term) > 0
    assert _all_finite(momentary)
    assert _all_finite(short_term)


def _write_test_wav(path: str, samples: list[float], sample_rate: int) -> None:
    """Write mono 16-bit PCM WAV using only the standard library."""
    frames = bytearray()
    for s in samples:
        clamped = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(round(clamped * 32767.0)))
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _run_cli(args: list[str]) -> subprocess.CompletedProcess:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


@pytest.mark.parametrize("command", ["lufs", "onset-envelope", "nnls-chroma", "tempogram"])
def test_cli_new_commands_smoke(command: str) -> None:
    """New CLI subcommands run end-to-end on a synthetic WAV and emit JSON."""
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "tone.wav")
        _write_test_wav(wav_path, _generate_sine(440, 22050, 2.0), 22050)

        result = _run_cli([command, wav_path, "--json"])
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip()


def test_analyze_sections_returns_section_result() -> None:
    from libsonare import Section, SectionResult, SectionType, analyze_sections

    samples = _generate_sine(220, 22050, 6.0) + _generate_sine(440, 22050, 6.0)
    result = analyze_sections(samples, sample_rate=22050, min_section_sec=2.0)
    assert isinstance(result, SectionResult)
    assert isinstance(result.sections, list)
    for section in result.sections:
        assert isinstance(section, Section)
        assert isinstance(section.type, SectionType)
        assert section.end >= section.start
        assert isinstance(section.name, str)


def test_analyze_melody_returns_melody_result() -> None:
    from libsonare import MelodyPoint, MelodyResult, analyze_melody

    samples = _generate_sine(220, 22050, 2.0)
    result = analyze_melody(samples, sample_rate=22050)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)
    assert math.isfinite(result.mean_frequency)
    for point in result.points[:8]:
        assert isinstance(point, MelodyPoint)
        assert math.isfinite(point.time)


def test_cqt_and_vqt_return_cqt_result() -> None:
    from libsonare import CqtResult, cqt, vqt

    samples = _generate_sine(220, 22050, 1.0)
    for result in (
        cqt(samples, sample_rate=22050, n_bins=24, bins_per_octave=12),
        vqt(samples, sample_rate=22050, n_bins=24, bins_per_octave=12, gamma=10.0),
    ):
        assert isinstance(result, CqtResult)
        assert result.n_bins == 24
        assert result.n_frames > 0
        assert len(result.magnitude) == result.n_bins * result.n_frames
        assert len(result.frequencies) == result.n_bins
