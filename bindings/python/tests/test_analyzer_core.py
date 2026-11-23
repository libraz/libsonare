"""Core analyzer API tests."""

from __future__ import annotations

# ruff: noqa: F403,F405
import numpy as np

import libsonare

from ._analyzer_helpers import *


def test_version() -> None:
    """sonare_version returns a non-empty string."""
    from libsonare import version

    v = version()
    assert isinstance(v, str)
    assert len(v) > 0


def test_ffmpeg_presence_is_a_capabilities_feature() -> None:
    """FFmpeg presence is read from capabilities(); no standalone probe exists."""
    import libsonare
    from libsonare._runtime import _get_lib

    ffmpeg = libsonare.capabilities().features.ffmpeg
    assert isinstance(ffmpeg, bool)
    assert ffmpeg == bool(_get_lib().sonare_has_feature(b"ffmpeg"))
    assert not hasattr(libsonare, "has_ffmpeg_support")


def test_capabilities_features_agree_with_has_feature() -> None:
    """Every capabilities() feature field matches the native sonare_has_feature probe."""
    import dataclasses

    import libsonare
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    features = libsonare.capabilities().features
    names = [field.name for field in dataclasses.fields(features)]
    assert names
    for name in names:
        head, *rest = name.split("_")
        key = head + "".join(part.capitalize() for part in rest)
        present = getattr(features, name)
        assert bool(lib.sonare_has_feature(key.encode())) is present, key


def test_vocal_edit_abi_version_is_none_when_feature_is_compiled_out() -> None:
    """vocal_edit_abi_version reports None when sonare_has_feature says vocalEdit is absent."""
    from unittest import mock

    from libsonare import vocal_edit

    lib = mock.Mock()
    lib.sonare_has_feature.return_value = 0
    with mock.patch.object(vocal_edit, "_get_lib", return_value=lib):
        assert vocal_edit.vocal_edit_abi_version() is None
    lib.sonare_has_feature.assert_called_once_with(b"vocalEdit")
    lib.sonare_vocal_edit_abi_version.assert_not_called()


def test_abi_versions_are_none_when_feature_is_compiled_out() -> None:
    """voice_changer_abi_version / project_abi_version report None for a zero ABI."""
    from unittest import mock

    import libsonare
    from libsonare import _analysis_music, _project_model

    lib = mock.Mock()
    lib.sonare_voice_changer_abi_version.return_value = 0
    lib.sonare_project_abi_version.return_value = 0
    with (
        mock.patch.object(_analysis_music, "_get_lib", return_value=lib),
        mock.patch.object(_project_model, "_get_lib", return_value=lib),
    ):
        assert libsonare.voice_changer_abi_version() is None
        assert libsonare.project_abi_version() is None


def test_capabilities_returns_native_build_descriptor() -> None:
    """capabilities returns the canonical C ABI build descriptor as a dataclass."""
    import libsonare

    descriptor = libsonare.capabilities()

    assert isinstance(descriptor, libsonare.Capabilities)
    assert descriptor.version == libsonare.version()
    assert descriptor.abi.project == (libsonare.project_abi_version() or 0)
    assert descriptor.abi.engine == libsonare.engine_abi_version()
    assert descriptor.decode.builtin == ("wav", "mp3")
    assert isinstance(descriptor.platform, str)
    assert isinstance(descriptor.simd, str)
    assert descriptor.hardware_concurrency >= 1


def test_capabilities_dataclass_fields_match_the_payload() -> None:
    """The dataclasses carry exactly the keys the C ABI JSON carries.

    Derived from the payload rather than from a written-down list, so a key
    added on the C side fails here instead of drifting until someone reads
    both files.
    """
    import dataclasses

    import libsonare
    from libsonare._analysis_music import _capabilities_document
    from libsonare.types import (
        AudioFormatCapabilities,
        CapabilitiesAbi,
        CapabilitiesFeatures,
    )

    def wire_key(name: str) -> str:
        head, *rest = name.split("_")
        return head + "".join(part.capitalize() for part in rest)

    document = _capabilities_document()
    descriptor = libsonare.capabilities()
    for section, cls in (
        ("abi", CapabilitiesAbi),
        ("features", CapabilitiesFeatures),
        ("decode", AudioFormatCapabilities),
        ("encode", AudioFormatCapabilities),
    ):
        assert isinstance(getattr(descriptor, section), cls), section
        names = {wire_key(field.name) for field in dataclasses.fields(cls)}
        assert set(document[section]) == names, section


def test_capabilities_and_catalog_are_frozen() -> None:
    """Assigning an attribute of a capabilities or catalog result is refused."""
    import dataclasses

    import libsonare

    descriptor = libsonare.capabilities()
    with pytest.raises(dataclasses.FrozenInstanceError):
        descriptor.version = "other"  # type: ignore[misc]
    catalog = libsonare.capability_catalog()
    with pytest.raises(dataclasses.FrozenInstanceError):
        catalog.version = "other"  # type: ignore[misc]


def test_capabilities_features_are_the_fifteen_build_options() -> None:
    """capabilities().features has exactly one bool field per build option."""
    import dataclasses

    import libsonare

    features = libsonare.capabilities().features
    expected = [
        "mastering",
        "mixing",
        "mixing_assistant",
        "graph",
        "fx",
        "ffmpeg",
        "instrument_param_automation",
        "arrangement",
        "project",
        "acoustic_sim",
        "playback",
        "pitch_editor",
        "vocal_edit",
        "voice_changer",
        "assist",
    ]
    assert sorted(field.name for field in dataclasses.fields(features)) == sorted(expected)
    for name in expected:
        assert isinstance(getattr(features, name), bool), name


def test_capabilities_use_snake_case_attributes_only() -> None:
    """The wire's camelCase spelling is not an attribute; the snake_case field is."""
    import libsonare

    descriptor = libsonare.capabilities()
    assert not hasattr(descriptor, "hardwareConcurrency")
    assert isinstance(descriptor.hardware_concurrency, int)
    assert descriptor.hardware_concurrency > 0


def test_capability_catalog_processor_params_and_slots_are_typed() -> None:
    """A catalog processor carries per-param dataclasses and a tuple of slots."""
    import libsonare

    processor = libsonare.capability_catalog().processors[0]
    assert isinstance(processor.params[0].rt_safe, bool)
    assert isinstance(processor.slots, tuple)


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
            assert isinstance(data, np.ndarray)
            assert len(data) == audio.length
            peak = float(np.max(np.abs(data)))
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
    from libsonare import Audio, ErrorCode, SonareError

    # Write a small file with an .m4a extension but no real audio payload, so
    # we can verify the error path even without an actual M4A decoder. (When
    # the library is built with FFmpeg support, this path raises a decode-time
    # error from FFmpeg instead.)
    with tempfile.NamedTemporaryFile(suffix=".m4a", delete=False) as f:
        f.write(b"not really an m4a file")
        path = f.name

    try:
        with pytest.raises(SonareError) as exc:
            Audio.from_file(path)
        assert exc.value.code in (ErrorCode.INVALID_FORMAT, ErrorCode.DECODE_FAILED)
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
    """detect_bpm finds no tempo in silence rather than reporting its search seed."""
    from libsonare import detect_bpm

    silence = [0.0] * 22050  # 1 second of silence
    assert detect_bpm(silence, sample_rate=22050) is None


def test_detect_key_with_silence() -> None:
    """detect_key returns the documented C-major fallback on silence."""
    from libsonare import PitchClass, detect_key

    silence = [0.0] * 22050
    key = detect_key(silence, sample_rate=22050)
    # Silence has no tonal content, so detect_key falls back to C major.
    assert key.root == PitchClass.C
    assert key.mode == "major"
    assert isinstance(key.confidence, float)
    # Confidence on silence stays low (empirically ~0.25).
    assert key.confidence < 0.5


def test_analyze_with_silence() -> None:
    """analyze returns the documented fallbacks on silent audio."""
    from libsonare import PitchClass, analyze

    silence = [0.0] * 22050
    result = analyze(silence, sample_rate=22050)
    assert result.bpm is None
    assert isinstance(result.bpm_confidence, float)
    # Silence has no tonal content: detect_key falls back to C major.
    assert result.key.root == PitchClass.C
    assert result.key.mode == "major"
    # Time signature falls back to 4/4 when no rhythmic content is present.
    assert result.time_signature.numerator == 4
    assert result.time_signature.denominator == 4
    assert isinstance(result.beats, list)


def test_compat_numeric_and_signal_utilities() -> None:
    """Compatibility utilities are available through Python."""
    import libsonare

    assert libsonare.frames_to_samples(4, hop_length=512) == 2048
    assert libsonare.samples_to_frames(2048, hop_length=512) == 4

    power_db = libsonare.power_to_db([1.0, 0.01], ref=1.0, amin=1e-10, top_db=80.0)
    assert isinstance(power_db, np.ndarray) and power_db.shape == (2,)
    assert power_db[0] == pytest.approx(0.0, abs=1e-5)
    assert power_db[1] == pytest.approx(-20.0, abs=1e-4)
    assert libsonare.db_to_power(power_db, ref=1.0)[1] == pytest.approx(0.01, rel=1e-5)

    amp_db = libsonare.amplitude_to_db([1.0, 0.5], ref=1.0, amin=1e-5, top_db=80.0)
    assert amp_db[0] == pytest.approx(0.0, abs=1e-5)
    assert libsonare.db_to_amplitude(amp_db, ref=1.0)[1] == pytest.approx(0.5, rel=1e-5)

    emphasized = libsonare.preemphasis([1.0, 1.0, 1.0], coef=0.5, zi=0.0)
    np.testing.assert_allclose(emphasized, [1.0, 0.5, 0.5], rtol=1e-6)
    assert libsonare.deemphasis(emphasized, coef=0.5, zi=0.0)[2] == pytest.approx(1.0, abs=1e-5)

    framed = libsonare.frame_signal([1.0, 2.0, 3.0, 4.0], frame_length=2, hop_length=1)
    # One column per frame: [1, 2], [2, 3], [3, 4].
    np.testing.assert_allclose(framed, [[1.0, 2.0, 3.0], [2.0, 3.0, 4.0]])
    np.testing.assert_allclose(
        libsonare.fix_length([1.0, 2.0], target_size=4, pad_value=-1.0), [1.0, 2.0, -1.0, -1.0]
    )
    with pytest.raises(ValueError):
        libsonare.pad_center([1.0, 2.0], target_size=-1)
    with pytest.raises(ValueError):
        libsonare.fix_length([1.0, 2.0], target_size=-1)
    np.testing.assert_array_equal(
        libsonare.fix_frames([2, 4], x_min=0, x_max=5, pad=True), [0, 2, 4, 5]
    )
    # Matches librosa.util.peak_pick exactly (index 0 is a peak under its
    # first-frame rule: x[0] >= max/mean of the leading window).
    np.testing.assert_array_equal(
        libsonare.peak_pick([0.0, 1.0, 0.0, 2.0, 0.0], 1, 1, 1, 1, 0.0, 0), [0, 1, 3]
    )
    np.testing.assert_allclose(
        libsonare.vector_normalize([3.0, 4.0], norm="l2"), [0.6, 0.8], rtol=1e-6
    )


def test_compat_silence_and_rhythm_utilities() -> None:
    """Silence/rhythm compatibility utilities are available through Python."""
    import libsonare

    samples = [0.0, 0.0, 1.0, 1.0, 0.0, 0.0]
    trimmed = libsonare.trim_silence(samples, top_db=20.0, frame_length=2, hop_length=1)
    assert len(trimmed.audio) > 0
    assert trimmed.end_sample > trimmed.start_sample
    intervals = libsonare.split_silence(samples, top_db=20.0, frame_length=2, hop_length=1)
    assert isinstance(intervals, np.ndarray)
    assert intervals.ndim == 2 and intervals.shape[1] == 2

    pcen_values = libsonare.pcen([[1.0, 2.0], [3.0, 4.0]])
    assert pcen_values.shape == (2, 2)

    chroma = np.zeros((12, 2), dtype=np.float32)
    chroma[0, 0] = 1.0
    chroma[6, 0] = 1.0
    tonnetz_values = libsonare.tonnetz(chroma)
    assert tonnetz_values.shape == (6, 2)

    onset = [0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0]
    temp = libsonare.tempogram(onset, sr=22050, hop_length=512, win_length=4)
    assert temp.shape == (4, len(onset))
    cosine = libsonare.tempogram(onset, sr=22050, hop_length=512, win_length=4, mode="cosine")
    assert cosine.shape == (4, len(onset))
    assert np.all((cosine >= -1.000001) & (cosine <= 1.000001))
    assert len(libsonare.plp(onset, sr=22050, hop_length=512, win_length=4)) == len(onset)


def test_plp_omitted_tempo_bounds_are_the_core_range() -> None:
    sr, hop = 22050, 512
    frames = np.arange(400)
    # Pulses every 7 frames sit at ~370 BPM, so the 300 BPM ceiling of a wider band moves them.
    onset = np.where(frames % 7 == 0, 1.0, 0.0).astype(np.float32)
    onset += np.where(frames % 43 == 0, 0.6, 0.0).astype(np.float32)
    omitted = libsonare.plp(onset, sr=sr, hop_length=hop)
    explicit = libsonare.plp(onset, sr=sr, hop_length=hop, tempo_min=60.0, tempo_max=200.0)
    np.testing.assert_array_equal(omitted, explicit)
    wider = libsonare.plp(onset, sr=sr, hop_length=hop, tempo_min=30.0, tempo_max=300.0)
    assert not np.array_equal(omitted, wider)


def test_audio_detect_bpm() -> None:
    """Audio.detect_bpm finds no tempo in silence."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    assert audio.detect_bpm() is None


def test_tempo_rhythm_timbre_melody_and_pitch_are_not_measured_on_silence() -> None:
    """Nothing to measure reads as None, never as a seed tempo or a 0 descriptor."""
    import libsonare

    silence = np.zeros(22050 * 3, dtype=np.float32)
    bpm = libsonare.analyze_bpm(silence, 22050)
    assert (bpm.bpm, bpm.candidates) == (None, [])
    rhythm = libsonare.analyze_rhythm(silence, 22050)
    assert (rhythm.bpm, rhythm.syncopation, rhythm.pattern_regularity) == (None, None, None)
    assert rhythm.tempo_stability is None
    timbre = libsonare.analyze_timbre(silence, 22050)
    for frame in (timbre, *timbre.timbre_over_time):
        assert (frame.brightness, frame.warmth, frame.density) == (None, None, None)
        assert (frame.roughness, frame.complexity) == (None, None)
    melody = libsonare.analyze_melody(silence, 22050)
    assert (melody.pitch_range_octaves, melody.pitch_stability) == (None, None)
    assert (melody.mean_frequency_hz, melody.vibrato_rate_hz) == (None, None)
    for track in (libsonare.pitch_yin(silence, 22050), libsonare.pitch_pyin(silence, 22050)):
        assert (track.median_f0, track.mean_f0) == (None, None)


def test_rhythm_measures_need_enough_beats_on_audible_input() -> None:
    """A clip too short to hold two beat intervals has no regularity or stability."""
    import libsonare

    t = np.arange(int(22050 * 0.4)) / 22050
    tone = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    rhythm = libsonare.analyze_rhythm(tone, 22050)
    assert rhythm.syncopation is None
    assert (rhythm.pattern_regularity, rhythm.tempo_stability) == (None, None)


def test_audio_detect_key() -> None:
    """Audio.detect_key returns the documented C-major fallback on silence."""
    from libsonare import Audio, PitchClass

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    key = audio.detect_key()
    # Silence has no tonal content, so detect_key falls back to C major.
    assert key.root == PitchClass.C
    assert key.mode == "major"
    # Name accessors must agree with the resolved root/mode.
    assert key.name == "C major"
    assert key.short_name == "C"


def test_audio_detect_beats() -> None:
    """Audio.detect_beats returns an array."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    beats = audio.detect_beats()
    assert isinstance(beats, np.ndarray)


def test_audio_detect_onsets() -> None:
    """Audio.detect_onsets returns an array."""
    from libsonare import Audio

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    onsets = audio.detect_onsets()
    assert isinstance(onsets, np.ndarray)
