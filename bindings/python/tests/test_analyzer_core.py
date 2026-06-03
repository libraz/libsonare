"""Core analyzer API tests."""

from __future__ import annotations

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


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
