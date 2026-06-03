"""Mastering and mixing analyzer API tests."""

from __future__ import annotations

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


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
    # +6.02 dB trim and -6.02 dB fader cancel to unity net gain. With the Balance
    # pan law no longer applying a spurious -3 dB attenuation at center, a centered
    # signal passes through at unity instead of sqrt(0.5).
    assert result.left == pytest.approx([1.0, 1.0], abs=0.01)
    assert result.right == pytest.approx([0.0, 0.0], abs=0.0002)
    assert result.sample_rate == 48000
    assert len(result.meters) == 1
    assert math.isfinite(result.meters[0].peak_db_l)
    assert isinstance(result.meters[0].likely_mono_compatible, bool)

    # A per-strip option whose length does not match the strip count raises a
    # clear ValueError rather than an opaque IndexError deep in the loop.
    with pytest.raises(ValueError, match="one entry per strip"):
        libsonare.mix_stereo(
            [([1.0, 1.0], [0.0, 0.0]), ([1.0, 1.0], [0.0, 0.0])],
            sample_rate=48000,
            fader_db=[-6.0206],  # only one entry for two strips
        )
