"""Mastering and mixing analyzer API tests."""

from __future__ import annotations

import re

import numpy as np

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


def test_mastering_returns_audio_and_loudness_metadata() -> None:
    """mastering is exposed as a standalone function and Audio method."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr)]

    result = libsonare.mastering(samples, sample_rate=sr, target_lufs=-18.0)
    assert isinstance(result.samples, np.ndarray)
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


def test_the_true_peak_ceiling_keyword_is_ceiling_dbtp() -> None:
    """The ceiling is held on a true-peak measurement, so its keyword carries dBTP."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 2)]
    with pytest.raises(TypeError, match="ceiling_db"):
        libsonare.mastering(samples, sample_rate=sr, ceiling_db=-1.0)  # type: ignore[call-arg]
    with pytest.raises(TypeError, match="ceiling_db"):
        libsonare.Audio.from_buffer(samples, sample_rate=sr).mastering(ceiling_db=-1.0)  # type: ignore[call-arg]
    # A loud target drives the limiter, so the ceiling given bounds the output peak.
    result = libsonare.mastering(samples, sample_rate=sr, target_lufs=-6.0, ceiling_dbtp=-3.0)
    assert float(np.max(np.abs(result.samples))) <= 10 ** ((-3.0 + 0.2) / 20)


def test_an_omitted_true_peak_oversample_is_the_library_default() -> None:
    """Omitted hands the core its 4x default; 0 is refused and the core owns the set."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 2)]
    omitted = libsonare.mastering(samples, sample_rate=sr, target_lufs=-6.0)
    explicit = libsonare.mastering(
        samples, sample_rate=sr, target_lufs=-6.0, true_peak_oversample=4
    )
    np.testing.assert_array_equal(omitted.samples, explicit.samples)
    audio = libsonare.Audio.from_buffer(samples, sample_rate=sr)
    np.testing.assert_array_equal(audio.mastering(target_lufs=-6.0).samples, omitted.samples)
    with pytest.raises(libsonare.SonareValueError, match="true_peak_oversample"):
        libsonare.mastering(samples, sample_rate=sr, true_peak_oversample=0)
    with pytest.raises(libsonare.SonareValueError, match="true_peak_oversample"):
        audio.mastering(true_peak_oversample=0)
    with pytest.raises(libsonare.SonareValueError, match="1, 2, 4, 8"):
        libsonare.mastering(samples, sample_rate=sr, true_peak_oversample=3)


def test_mastering_accepts_release_and_input_rate_knobs() -> None:
    """The appended maximizer knobs are accepted and zero-init stays default."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 2)]

    tuned = libsonare.mastering(
        samples,
        sample_rate=sr,
        target_lufs=-14.0,
        release_ms=250.0,
        apply_gain_at_input_rate=True,
    )
    assert len(tuned.samples) == len(samples)
    assert all(math.isfinite(v) for v in tuned.samples)

    # An omitted release_ms is the library default (50 ms); an explicit 0 is refused.
    omitted = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0)
    explicit = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0, release_ms=50.0)
    np.testing.assert_array_equal(omitted.samples, explicit.samples)
    for refused in (0.0, -1.0):
        with pytest.raises(libsonare.SonareValueError, match="release_ms"):
            libsonare.mastering(samples, sample_rate=sr, release_ms=refused)

    with pytest.raises(libsonare.SonareError):
        libsonare.mastering(samples, sample_rate=sr, target_lufs=float("nan"))
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering(samples, sample_rate=sr, ceiling_dbtp=float("inf"))
    recovered = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0, ceiling_dbtp=-1.0)
    assert all(math.isfinite(value) for value in recovered.samples)
    assert isinstance(recovered.loudness_target_limited, bool)
    ceiling_limited = libsonare.mastering(
        samples, sample_rate=sr, target_lufs=-2.0, ceiling_dbtp=-1.0
    )
    assert ceiling_limited.loudness_target_limited


def test_named_mastering_processors() -> None:
    """Named mastering processors are exposed through the shared API."""
    import libsonare

    sr = 22050
    samples = [0.2 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 2)]

    names = libsonare.mastering_processor_names()
    assert "dynamics.compressor" in names
    assert "eq.equalizer" in names
    assert "stereo.imager" in names

    result = libsonare.mastering_apply_processor(
        "dynamics.compressor",
        samples,
        sample_rate=sr,
        params={"thresholdDb": -24.0, "ratio": 1.5},
    )
    assert len(result.samples) == len(samples)
    assert math.isfinite(result.output_lufs)

    eq_result = libsonare.mastering_apply_processor(
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

    stereo = libsonare.mastering_apply_processor_stereo(
        "stereo.imager",
        samples,
        samples,
        sample_rate=sr,
        params={"width": 1.1},
    )
    assert len(stereo.left) == len(samples)
    assert len(stereo.right) == len(samples)

    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_apply_processor(
            "stereo.imager", samples, sample_rate=sr, params={"width": 1.1}
        )
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_apply_processor(
            "dynamics.compressor", samples, sample_rate=sr, params={"ratio": float("nan")}
        )


def test_mastering_processor_catalog_reports_kind_and_flags() -> None:
    """The processor catalog classifies kind and exposes capability flags."""
    import libsonare

    catalog = libsonare.mastering_processor_catalog()
    assert isinstance(catalog, list)
    assert len(catalog) > 0

    by_id = {entry.id: entry for entry in catalog}

    compressor = by_id["dynamics.compressor"]
    assert compressor.kind == "realtime"
    assert compressor.realtime_insertable is True
    assert isinstance(compressor.latency_samples, int)
    assert isinstance(compressor.tail_samples, int)
    assert compressor.realtime_cost == "low"
    # Per-channel/linked processors process every plane in one call.
    assert compressor.channel_policy == "multichannel"

    assert by_id["match.abCrossfade"].kind == "pair"

    optimize = by_id["maximizer.loudnessOptimize"]
    assert optimize.kind == "offline"
    assert optimize.realtime_insertable is False

    assert by_id["eq.midSide"].stereo_only is True
    # Inherently-stereo processors are wrapped on the front L/R pair.
    assert by_id["eq.midSide"].channel_policy == "stereoPairOnly"
    assert by_id["stereo.imager"].channel_policy == "stereoPairOnly"
    assert by_id["stereo.haasEnhancer"].tail_samples == 576
    assert by_id["stereo.phaseAlign"].tail_samples == 0
    assert by_id["effects.reverb.velvet"].realtime_cost == "high"
    assert by_id["effects.reverb.fdn"].realtime_cost == "moderate"

    # A repair stage publishes the bounds its own validation enforces, and says whether it can
    # be causal.
    assert by_id["repair.declick"].causal is False
    assert by_id["repair.declip"].causal is False
    for stage in ("decrackle", "dehum", "denoiseClassical", "dereverbClassical"):
        assert by_id[f"repair.{stage}"].causal is True
    assert compressor.causal is True
    click_run = next(p for p in by_id["repair.declick"].params if p.name == "maxClickSamples")
    assert (click_run.min, click_run.max, click_run.unit) == (1, 512, "samples")
    assert click_run.id is None
    assert click_run.rt_safe is False

    # The voice changer is a stereo-pair insert whose bounds are the ones construction refuses
    # outside of; its latency-changing switches are construction-only.
    voice = by_id["voice.changer"]
    assert (voice.kind, voice.category, voice.channel_policy) == (
        "realtime",
        "voice",
        "stereoPairOnly",
    )
    assert voice.causal is True
    assert voice.latency_samples > 0
    voice_params = {p.name: p for p in voice.params}
    semitones = voice_params["retunePitchSemitones"]
    assert (semitones.min, semitones.max, semitones.unit) == (-24, 24, "semitones")
    assert semitones.rt_safe is True
    for name in ("retuneGrainSizeSamples", "limiterEnableIspLimiter", "reverbSeed"):
        assert voice_params[name].id is None
        assert voice_params[name].rt_safe is False


@pytest.mark.parametrize(
    ("processor", "old", "new"),
    [
        ("dynamics.deesser", "bandpassQ", "bandPassQ"),
        ("effects.modulation.pitchShifter", "semitones", "pitchSemitones"),
        ("voice.changer", "retuneSemitones", "retunePitchSemitones"),
        ("repair.trimSilence", "threshold", "thresholdDb"),
    ],
)
def test_a_renamed_insert_key_is_refused_with_the_keys_the_insert_reads(
    processor: str, old: str, new: str
) -> None:
    """The old spelling is refused rather than read, listing the accepted keys, new one included."""
    import libsonare

    listed = rf"unknown key params\.{re.escape(old)} \(expected [^)]*\b{re.escape(new)}\b"
    with pytest.raises(libsonare.SonareValueError, match=listed) as caught:
        libsonare.mastering_apply_processor(processor, np.zeros(4800, np.float32), 48000, {old: 1})
    assert "renamed" not in str(caught.value)


def test_a_key_renamed_only_on_another_insert_lists_that_inserts_keys() -> None:
    import libsonare

    with pytest.raises(libsonare.SonareValueError) as caught:
        libsonare.mastering_apply_processor(
            "effects.reverb.velvet", np.zeros(4800, np.float32), 48000, {"semitones": 1.0}
        )
    assert str(caught.value) == (
        "mastering_apply_processor: unknown key params.semitones "
        "(expected decay, decaySec, densityHz, dryWet, enableShelf, reverbTimeSec)"
    )


def test_mastering_amp_preset_catalog_reports_index_name_and_params() -> None:
    """The amp-sim rig catalog lists built-in rigs in presetIndex order."""
    import libsonare

    catalog = libsonare.mastering_amp_preset_catalog()
    assert isinstance(catalog, list)
    assert len(catalog) > 0

    for expected_index, entry in enumerate(catalog):
        assert entry.index == expected_index

    first = catalog[0]
    assert first.name == "cleanCombo"
    assert "drive" in first.params
    # An enum-valued key carries its choice token, not an ordinal.
    assert isinstance(first.params["ampModel"], str)
    assert isinstance(first.params["powerTube"], str)


def test_capability_catalog_aggregates_processors_and_presets() -> None:
    """The aggregate catalog retains native version, ABI, and processor metadata."""
    import dataclasses

    import libsonare

    catalog = libsonare.capability_catalog()

    assert catalog.version == libsonare.version()
    assert catalog.abi.project == (libsonare.project_abi_version() or 0)
    assert catalog.processors
    assert "pop" in catalog.presets.mastering
    assert "harpPlucked" in catalog.presets.synth
    for names in dataclasses.astuple(catalog.presets):
        assert len(names) == len(set(names))
    compressor = next(entry for entry in catalog.processors if entry.id == "dynamics.compressor")
    assert compressor.category == "dynamics"
    assert compressor.realtime_cost == "low"
    assert {"name", "type", "min", "max", "default", "unit"} <= {
        field.name for field in dataclasses.fields(compressor.params[0])
    }


def test_insert_param_metadata_reaches_the_python_facade() -> None:
    """Type, design default and measured range survive the C-ABI JSON hop.

    The values themselves are pinned in the C++ suite against the config structs
    and the construction path; what this covers is that the facade hands a host
    the whole descriptor rather than the name/id pair it used to carry.
    """
    import libsonare

    params = {
        param.name: param for param in libsonare.mastering_insert_param_info("dynamics.compressor")
    }
    # A validated control publishes its accepted range; an open one publishes
    # null on both, which is the catalog stating no limit rather than "unknown".
    assert params["ratio"].min == 1.0
    assert params["ratio"].max is None
    assert params["thresholdDb"].min is None
    # The default is the config struct's own field initializer.
    assert params["ratio"].default == 2.0
    assert params["thresholdDb"].default == -18.0
    assert params["thresholdDb"].unit == "dB"
    # A boolean config field arrives as a JSON boolean, with no range.
    assert params["autoMakeup"].type == "boolean"
    assert params["autoMakeup"].default is False
    assert params["autoMakeup"].min is None

    # A two-sided range, and the per-band EQ surface, which is only declared
    # because the band readers publish their keys without being handed one.
    imager = {param.name: param for param in libsonare.mastering_insert_param_info("stereo.imager")}
    assert (imager["width"].min, imager["width"].max) == (0.0, 2.0)
    parametric = {
        param.name: param for param in libsonare.mastering_insert_param_info("eq.parametric")
    }
    assert parametric["band0.frequencyHz"].default == 1000.0
    assert parametric["band0.gainDb"].unit == "dB"


def test_mastering_pair_accepts_differing_reference_length() -> None:
    """Pair process/analyze accept a reference that differs in length from source."""
    import libsonare

    sr = 44100
    source = [0.18 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 4)]
    reference = [0.12 * math.sin(2 * math.pi * 880 * i / sr) for i in range(sr // 8)]  # shorter
    assert len(reference) != len(source)

    paired = libsonare.mastering_apply_pair_processor(
        "match.abCrossfade", source, reference, sample_rate=sr, params={"mix": 0.5}
    )
    assert len(paired.samples) > 0

    pair = libsonare.mastering_analyze_pair(
        "match.referenceLoudness", source, reference, sample_rate=sr
    )
    assert pair.reference_lufs is not None


def test_mastering_pair_and_stereo_analysis() -> None:
    """Pair and stereo mastering APIs return processor output and analysis records."""
    import libsonare

    sr = 44100
    samples = [0.18 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 4)]
    reference = [0.12 * math.sin(2 * math.pi * 880 * i / sr) for i in range(sr // 4)]

    assert "match.abCrossfade" in libsonare.mastering_pair_processor_names()
    assert "match.referenceLoudness" in libsonare.mastering_pair_analysis_names()
    assert "stereo.monoCompatCheck" in libsonare.mastering_stereo_analysis_names()

    paired = libsonare.mastering_apply_pair_processor(
        "match.abCrossfade",
        samples,
        reference,
        sample_rate=sr,
        params={"mix": 0.25},
    )
    assert len(paired.samples) == len(samples)

    pair = libsonare.mastering_analyze_pair(
        "match.referenceLoudness",
        samples,
        reference,
        sample_rate=sr,
    )
    assert isinstance(pair, libsonare.MatchReferenceLoudnessResult)
    assert pair.source_lufs is not None
    assert pair.reference_lufs is not None

    stereo = libsonare.mastering_analyze_stereo(
        "stereo.monoCompatCheck",
        samples,
        reference,
        sample_rate=sr,
    )
    assert isinstance(stereo, libsonare.StereoMonoCompatCheckResult)
    assert -1.0 <= stereo.correlation <= 1.0


@pytest.mark.parametrize(
    ("platform", "message"),
    [
        (
            {"name": "S", "targetLufs": -14.0, "ceiling_dbtp": -1.0},
            "unknown key platforms[0].targetLufs (expected name, target_lufs, ceiling_dbtp)",
        ),
        (
            {"name": "S", "target_lufs": -14.0, "ceilingDb": -1.0},
            "unknown key platforms[0].ceilingDb (expected name, target_lufs, ceiling_dbtp)",
        ),
        (
            {"name": "S", "target_lufs": -14.0, "ceiling_db": -1.0},
            "unknown key platforms[0].ceiling_db (expected name, target_lufs, ceiling_dbtp)",
        ),
        ({"name": "S", "target_lufs": -14.0}, "platforms[0].ceiling_dbtp must be given"),
        ({"target_lufs": -14.0}, "platforms[0].name must be given"),
        (
            {"name": "S", "target_lufs": -14.0, "ceiling_dbtp": -1.0, "gain": 0.0},
            "unknown key platforms[0].gain (expected name, target_lufs, ceiling_dbtp)",
        ),
    ],
)
def test_a_streaming_platform_takes_exactly_its_snake_case_keys(
    platform: dict[str, object], message: str
) -> None:
    import re

    import libsonare

    pinned = f"^mastering_streaming_preview: {re.escape(message)}$"
    with pytest.raises(libsonare.SonareValueError, match=pinned):
        libsonare.mastering_streaming_preview([0.1] * 4800, 48000, platforms=[platform])  # type: ignore[list-item]


def test_mastering_streaming_preview_returns_its_record() -> None:
    """Streaming preview is exposed through the Python mastering API."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(sr)]

    preview = libsonare.mastering_streaming_preview(
        samples,
        sample_rate=sr,
        platforms=[{"name": "Unit Test", "target_lufs": 0.0, "ceiling_dbtp": -6.0}],
    )

    (platform,) = preview.platforms
    assert platform.name == "Unit Test"
    assert platform.integrated_lufs is not None
    assert platform.normalization_gain_db == pytest.approx(-platform.integrated_lufs, abs=0.001)
    assert platform.ceiling_risk is True


def test_mastering_assistant_suggest_returns_its_record() -> None:
    """Mastering assistant suggestion is exposed through Python."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 220 * i / sr) for i in range(sr * 3)]

    suggestion = libsonare.mastering_assistant_suggest(
        samples,
        sample_rate=sr,
        params={"targetLufs": -13.0, "ceilingDbtp": -0.8},
    )

    assert suggestion.explanation
    assert not hasattr(suggestion, "genre_candidates")
    assert suggestion.chain_config["loudness.targetLufs"] == pytest.approx(-13.0)
    assert suggestion.chain_config["loudness.ceilingDbtp"] == pytest.approx(-0.8)


def test_mastering_audio_profile_returns_its_record() -> None:
    """Mastering assistant audio profile is exposed through Python."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 330 * i / sr) for i in range(sr * 2)]

    profile = libsonare.mastering_audio_profile(
        samples,
        sample_rate=sr,
        params={"nFft": 1024, "hopLength": 256},
    )

    assert profile.duration_sec == pytest.approx(2.0)
    assert profile.loudness.integrated_lufs is not None
    assert profile.spectral.centroid_hz == pytest.approx(330.0, rel=0.2)
    assert profile.dynamics.sustain_ratio >= 0.0
    assert profile.defects is None
    assert not hasattr(profile, "genre_candidates")


def _decorrelated_pair(sr: int, seconds: int) -> tuple[list[float], list[float], list[float]]:
    n = sr * seconds
    left = [0.2 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(n)]
    right = [0.2 * math.sin(2 * math.pi * 1731 * i / sr) for i in range(n)]
    downmix = [0.5 * (a + b) for a, b in zip(left, right, strict=True)]
    return left, right, downmix


def test_stereo_mastering_analysis_uses_channel_summed_loudness() -> None:
    """The stereo analysis entry points measure the pair, not a downmix."""
    import libsonare

    sr = 48000
    left, right, downmix = _decorrelated_pair(sr, 4)

    stereo = libsonare.mastering_streaming_preview_stereo(left, right, sample_rate=sr)
    mono = libsonare.mastering_streaming_preview(downmix, sample_rate=sr)
    stereo_lufs = stereo.platforms[0].integrated_lufs
    mono_lufs = mono.platforms[0].integrated_lufs
    assert stereo_lufs is not None
    assert mono_lufs is not None

    # BS.1770 sums the channel powers while the 0.5*(L+R) downmix quarters them,
    # so a decorrelated pair reads 6.02 dB above what a mono caller can measure,
    # and the normalization gain built on it is off by the same amount.
    assert stereo_lufs - mono_lufs == pytest.approx(6.02, abs=0.3)
    interleaved = [value for pair in zip(left, right, strict=True) for value in pair]
    assert stereo_lufs == pytest.approx(
        libsonare.lufs_interleaved(interleaved, 2, sr).integrated_lufs, abs=0.01
    )
    assert stereo.platforms[0].normalization_gain_db == pytest.approx(
        -14.0 - stereo_lufs, abs=0.001
    )

    profile = libsonare.mastering_audio_profile_stereo(left, right, sample_rate=sr)
    assert profile.loudness.integrated_lufs == pytest.approx(stereo_lufs, abs=0.01)
    assert profile.spectral.centroid_hz > 0.0

    suggestion = libsonare.mastering_assistant_suggest_stereo(left, right, sample_rate=sr)
    assert suggestion.chain_config
    assert suggestion.explanation
    assert suggestion.profile.integrated_lufs == pytest.approx(stereo_lufs, abs=0.01)


def test_stereo_crest_factor_survives_a_cancelling_pair() -> None:
    """The stereo crest meter keeps a measurement the downmix loses."""
    import libsonare

    sr = 48000
    left = [0.5 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr)]
    right = [-value for value in left]
    downmix = [0.5 * (a + b) for a, b in zip(left, right, strict=True)]

    assert libsonare.metering_crest_factor_db_stereo(left, right, sample_rate=sr) == pytest.approx(
        3.01, abs=0.5
    )
    # The anti-phase pair cancels to silence in the downmix, which reports the
    # 0 dB neutral rather than the pair's real 3 dB crest factor.
    assert libsonare.metering_crest_factor_db(downmix, sample_rate=sr) == pytest.approx(0.0)


def test_stereo_analysis_rejects_mismatched_channel_lengths() -> None:
    """Channel length mismatch is caught before reaching the C ABI."""
    import libsonare

    sr = 48000
    left = [0.1] * 1024
    right = [0.1] * 512
    with pytest.raises(ValueError):
        libsonare.mastering_streaming_preview_stereo(left, right, sample_rate=sr)
    with pytest.raises(ValueError):
        libsonare.mastering_audio_profile_stereo(left, right, sample_rate=sr)
    with pytest.raises(ValueError):
        libsonare.mastering_assistant_suggest_stereo(left, right, sample_rate=sr)
    with pytest.raises(ValueError):
        libsonare.metering_crest_factor_db_stereo(left, right, sample_rate=sr)


def test_mixing_presets_and_stereo_mix() -> None:
    """Mixing scene presets and simple stereo mix are exposed."""
    import libsonare

    assert "vocalReverbSend" in libsonare.mixing_scene_preset_names()
    assert any(
        strip["id"] == "vocal"
        for strip in libsonare.mixing_scene_preset("vocalReverbSend")["strips"]
    )

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

    # A per-strip option longer than the strip list raises a ValueError naming
    # it; a shorter one leaves the remaining strips at their defaults.
    two_strips = [([1.0, 1.0], [0.0, 0.0]), ([1.0, 1.0], [0.0, 0.0])]
    with pytest.raises(ValueError, match="'fader_db' has more entries than strips"):
        libsonare.mix_stereo(two_strips, sample_rate=48000, fader_db=[-6.0206, 0.0, 0.0])
    libsonare.mix_stereo(two_strips, sample_rate=48000, fader_db=[-6.0206])


def test_master_audio_validates_offline_input() -> None:
    """The offline mastering chain rejects empty / out-of-range-rate / non-finite
    input. Validation lives in the C++ core (MasteringChain::process_mono /
    process_stereo), so every surface (C ABI, Node, WASM, Python) behaves the
    same way."""
    import libsonare

    sr = 44100
    ok = [0.1 * math.sin(2 * math.pi * 220 * i / sr) for i in range(2048)]

    with pytest.raises(libsonare.SonareError):
        libsonare.master_audio([], sample_rate=sr, overrides={})
    with pytest.raises(libsonare.SonareError):
        libsonare.master_audio(ok, sample_rate=100, overrides={})
    with pytest.raises(libsonare.SonareError):
        libsonare.master_audio(ok, sample_rate=10_000_000, overrides={})

    bad = list(ok)
    bad[10] = float("nan")
    with pytest.raises(libsonare.SonareError):
        libsonare.master_audio(bad, sample_rate=sr, overrides={})

    # Valid input still masters and preserves length.
    result = libsonare.master_audio(ok, sample_rate=sr, overrides={})
    assert len(result.samples) == len(ok)


def test_master_audio_reports_output_metrics() -> None:
    """The chain result exposes output true-peak (dBTP), LRA, and per-stage gain
    reductions computed by the core. The gain-reduction stages are a subset
    of the reported stages, in the same order."""
    import math

    import libsonare

    sr = 44100
    samples = [0.3 * math.sin(2 * math.pi * 220 * i / sr) for i in range(4096)]
    config = {
        "dynamics": {"compressor": {"thresholdDb": -30.0, "ratio": 4.0}},
        "loudness": {"targetLufs": -14.0, "ceilingDbtp": -1.0},
    }
    result = libsonare.master_audio(samples, sample_rate=sr, overrides=config)

    # Output true peak must be finite and near/under the -1 dBTP ceiling.
    assert math.isfinite(result.output_true_peak_dbtp)
    assert result.output_true_peak_dbtp <= 0.0
    assert math.isfinite(result.output_loudness_range_lu)
    assert result.output_loudness_range_lu >= 0.0
    assert isinstance(result.loudness_target_limited, bool)
    assert result.report is not None
    assert result.report.before.integrated_lufs == result.input_lufs
    assert result.report.after.integrated_lufs == result.output_lufs
    assert result.report.after.true_peak_dbtp == result.output_true_peak_dbtp
    assert len(result.report.band_energy_delta_db) == 32
    assert result.report.max_gain_reduction_db <= 0.0

    # A compressor stage ran, so at least one gain-reduction entry is reported.
    gr_stages = [r.stage for r in result.stage_gain_reductions]
    assert "dynamics.compressor" in gr_stages
    for reduction in result.stage_gain_reductions:
        assert reduction.stage in result.stages
        assert reduction.gain_reduction_db <= 0.0

    # master_audio (preset + overrides path) exposes the same fields.
    preset_result = libsonare.master_audio(samples, sample_rate=sr, preset="pop")
    assert math.isfinite(preset_result.output_true_peak_dbtp)
    assert math.isfinite(preset_result.output_loudness_range_lu)
    assert isinstance(preset_result.stage_gain_reductions, list)
    assert preset_result.report is not None


def test_loudness_target_limited_agrees_across_entry_points() -> None:
    """Every entry point that normalizes loudness reports the same clamp.

    The named-processor and the simple-helper paths previously disagreed: the
    core computed the flag and the boundary replaced it with a constant false.
    """
    import libsonare

    sr = 22050
    samples = [0.5 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr)]

    def run_all(target_lufs: float, ceiling_db: float) -> list[bool]:
        simple = libsonare.mastering(
            samples, sample_rate=sr, target_lufs=target_lufs, ceiling_dbtp=ceiling_db
        )
        named = libsonare.mastering_apply_processor(
            "maximizer.loudnessOptimize",
            samples,
            sample_rate=sr,
            params={"targetLufs": target_lufs, "ceilingDbtp": ceiling_db},
        )
        # The chain's loudness stage is the one master_audio composes; a preset
        # only supplies different target / ceiling values.
        chain = libsonare.master_audio(
            samples,
            sample_rate=sr,
            overrides={"loudness": {"targetLufs": target_lufs, "ceilingDbtp": ceiling_db}},
        )
        return [
            simple.loudness_target_limited,
            named.loudness_target_limited,
            chain.loudness_target_limited,
            chain.report.loudness_target_limited,
        ]

    # The ceiling sits below the material's true peak while the target asks for
    # a boost, so the normalization gain is clamped on every path.
    assert run_all(-6.0, -12.0) == [True, True, True, True]
    assert run_all(-20.0, -1.0) == [False, False, False, False]


def test_loudness_target_limited_matches_on_the_stereo_named_processor() -> None:
    """The stereo named-processor path reports the clamp the mono path reports.

    The source is peak-normalized by a lone transient, so its program loudness
    stays far below the -6 LUFS target and the -3 dBTP ceiling is the binding
    constraint on both paths -- by a margin far wider than the 3 dB BS.1770
    channel-summing offset between the mono and the dual-mono stereo
    measurement.
    """
    import libsonare

    sr = 22050
    target_lufs = -6.0
    ceiling_db = -3.0
    samples = [0.05 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr)]
    samples[len(samples) // 2] = 1.0
    params = {"targetLufs": target_lufs, "ceilingDbtp": ceiling_db, "truePeakOversample": 4}

    mono = libsonare.mastering_apply_processor(
        "maximizer.loudnessOptimize", samples, sample_rate=sr, params=params
    )
    stereo = libsonare.mastering_apply_processor_stereo(
        "maximizer.loudnessOptimize", samples, samples, sample_rate=sr, params=params
    )

    assert mono.loudness_target_limited is True
    assert stereo.loudness_target_limited == mono.loudness_target_limited
    assert stereo.output_lufs < target_lufs


def test_mastering_assistant_refuses_snake_case_param_names() -> None:
    """An earlier snake_case param name is refused naming the lowerCamel one."""
    import libsonare
    from libsonare import SonareValueError

    samples = [0.2 * math.sin(2 * math.pi * 220 * i / 48000) for i in range(4800)]
    for earlier, current in (("target_lufs", "targetLufs"), ("target_platform", "targetPlatform")):
        with pytest.raises(SonareValueError, match=current) as raised:
            libsonare.mastering_assistant_suggest(samples, 48000, {earlier: 3.0})
        assert raised.value.code == int(libsonare.ErrorCode.INVALID_PARAMETER)


def test_mastering_assistant_suggest_follows_target_platform() -> None:
    """A delivery target reaches the assistant as a name and moves the loudness."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 220 * i / sr) for i in range(sr)]

    names = libsonare.mastering_platform_names()
    assert "broadcast" in names
    assert "club" in names

    # Without a target the assistant returns the streaming default of -14 LUFS.
    default = libsonare.mastering_assistant_suggest(samples, sample_rate=sr)
    assert default.chain_config["loudness.targetLufs"] == pytest.approx(-14.0)

    broadcast = libsonare.mastering_assistant_suggest(
        samples, sample_rate=sr, params={"targetPlatform": "broadcast"}
    )
    assert broadcast.chain_config["loudness.targetLufs"] == pytest.approx(-23.0)

    # Only a loud delivery format moves the ceiling as well as the target.
    club = libsonare.mastering_assistant_suggest(
        samples, sample_rate=sr, params={"targetPlatform": "club"}
    )
    assert club.chain_config["loudness.targetLufs"] == pytest.approx(-9.0)
    assert club.chain_config["loudness.ceilingDbtp"] == pytest.approx(-0.3)

    with pytest.raises(ValueError):
        libsonare.mastering_assistant_suggest(
            samples, sample_rate=sr, params={"targetPlatform": "vinyl"}
        )
    # The numeric index is a transport detail, not part of the Python vocabulary.
    with pytest.raises(ValueError):
        libsonare.mastering_assistant_suggest(samples, sample_rate=sr, params={"targetPlatform": 2})


def test_mastering_assistant_starts_from_the_named_preset() -> None:
    """``preset`` is a name here; the assistant never picks one from the audio."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 220 * i / sr) for i in range(sr)]

    def explanation(params: dict[str, float | int | bool | str] | None) -> list[str]:
        return libsonare.mastering_assistant_suggest(
            samples, sample_rate=sr, params=params
        ).explanation

    assert "base preset: streaming" in explanation(None)
    assert "base preset: classical" in explanation({"preset": "classical"})
    stereo = libsonare.mastering_assistant_suggest_stereo(
        samples, samples, sample_rate=sr, params={"preset": "jazz"}
    )
    assert "base preset: jazz" in stereo.explanation

    with pytest.raises(
        ValueError, match="^mastering_assistant_suggest: params.preset must be one of "
    ):
        explanation({"preset": "notAPreset"})
    # A restoration preset leaves level alone, which the suggestion never does.
    with pytest.raises(libsonare.SonareError, match="vinyl"):
        explanation({"preset": "vinyl"})
    # The numeric index is a transport detail, not part of the Python vocabulary.
    with pytest.raises(ValueError):
        explanation({"preset": 3})


def test_clean_input_reports_no_non_finite_substitution() -> None:
    """Every mastering result reports zero substitutions for a finite input.

    A substituting stage replaces a non-finite input sample with a finite
    in-domain one, leaving the output finite, in range and error-free while
    carrying samples unrelated to the input. Zero is therefore what says the
    output was computed from what was supplied, and every entry point must be
    able to say it.
    """
    import libsonare

    sr = 44100
    samples = [0.3 * math.sin(2 * math.pi * 220 * i / sr) for i in range(4096)]
    other = [0.2 * math.sin(2 * math.pi * 330 * i / sr) for i in range(4096)]
    config = {
        "dynamics": {"compressor": {"thresholdDb": -30.0, "ratio": 4.0}},
        "loudness": {"targetLufs": -14.0, "ceilingDbtp": -1.0},
    }

    results = [
        libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0),
        libsonare.mastering_apply_processor(
            "dynamics.compressor", samples, sample_rate=sr, params={"thresholdDb": -24.0}
        ),
        libsonare.mastering_apply_processor_stereo(
            "stereo.imager", samples, other, sample_rate=sr, params={"width": 1.1}
        ),
        libsonare.mastering_apply_pair_processor(
            "match.abCrossfade", samples, other, sample_rate=sr, params={"mix": 0.5}
        ),
        libsonare.master_audio(samples, sample_rate=sr, overrides=config),
        libsonare.master_audio_stereo(samples, other, sample_rate=sr, overrides=config),
        libsonare.master_audio(samples, sample_rate=sr, preset="pop"),
        libsonare.master_audio_stereo(samples, other, sample_rate=sr, preset="pop"),
    ]

    for result in results:
        assert result.non_finite_substitution_count == 0
        assert isinstance(result.non_finite_substitution_count, int)


def test_master_audio_runs_explicit_overrides_without_a_preset() -> None:
    """With no preset, an explicit overrides mapping is the chain that runs."""
    import libsonare

    sr = 22050
    tone = np.sin(2 * np.pi * 220 * np.arange(sr // 4) / sr).astype(np.float32) * 0.1
    chain = {"dynamics": {"compressor": {"enabled": True, "thresholdDb": -30.0}}}
    result = libsonare.master_audio(tone, sr, overrides=chain)
    assert "dynamics.compressor" in result.stages
    assert not np.array_equal(result.samples, tone)
