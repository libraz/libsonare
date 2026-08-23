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

    # release_ms == 0 (or omitted) must reproduce the library default (50 ms).
    omitted = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0)
    zero = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0, release_ms=0.0)
    explicit = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0, release_ms=50.0)
    assert zero.samples == omitted.samples
    assert zero.samples == explicit.samples

    with pytest.raises(libsonare.SonareError):
        libsonare.mastering(samples, sample_rate=sr, target_lufs=float("nan"))
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering(samples, sample_rate=sr, ceiling_db=float("inf"))
    recovered = libsonare.mastering(samples, sample_rate=sr, target_lufs=-14.0, ceiling_db=-1.0)
    assert all(math.isfinite(value) for value in recovered.samples)
    assert isinstance(recovered.loudness_target_limited, bool)
    ceiling_limited = libsonare.mastering(
        samples, sample_rate=sr, target_lufs=-2.0, ceiling_db=-1.0
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

    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_process("stereo.imager", samples, sample_rate=sr, params={"width": 1.1})
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_process(
            "dynamics.compressor", samples, sample_rate=sr, params={"ratio": float("nan")}
        )


def test_mastering_processor_catalog_reports_kind_and_flags() -> None:
    """The processor catalog classifies kind and exposes capability flags."""
    import libsonare

    catalog = libsonare.mastering_processor_catalog()
    assert isinstance(catalog, list)
    assert len(catalog) > 0

    by_id = {entry["id"]: entry for entry in catalog}

    compressor = by_id["dynamics.compressor"]
    assert compressor["kind"] == "realtime"
    assert compressor["realtimeInsertable"] is True
    assert isinstance(compressor["latencySamples"], int)
    assert isinstance(compressor["tailSamples"], int)
    assert compressor["realtimeCost"] == "low"
    # Per-channel/linked processors process every plane in one call.
    assert compressor["channelPolicy"] == "multichannel"

    assert by_id["match.abCrossfade"]["kind"] == "pair"

    optimize = by_id["maximizer.loudnessOptimize"]
    assert optimize["kind"] == "offline"
    assert optimize["realtimeInsertable"] is False

    assert by_id["eq.midSide"]["stereoOnly"] is True
    # Inherently-stereo processors are wrapped on the front L/R pair.
    assert by_id["eq.midSide"]["channelPolicy"] == "stereoPairOnly"
    assert by_id["stereo.imager"]["channelPolicy"] == "stereoPairOnly"
    assert by_id["stereo.haasEnhancer"]["tailSamples"] == 576
    assert by_id["stereo.phaseAlign"]["tailSamples"] == 0
    assert by_id["effects.reverb.velvet"]["realtimeCost"] == "high"
    assert by_id["effects.reverb.fdn"]["realtimeCost"] == "moderate"


def test_capability_catalog_aggregates_processors_and_presets() -> None:
    """The aggregate catalog retains native version, ABI, and processor metadata."""
    import libsonare

    catalog = libsonare.capability_catalog()

    assert catalog["version"] == libsonare.version()
    assert catalog["abi"]["project"] == 1
    assert catalog["processors"]
    assert "pop" in catalog["presets"]["mastering"]
    assert "harp-plucked" in catalog["presets"]["synth"]
    for names in catalog["presets"].values():
        assert len(names) == len(set(names))
    compressor = next(
        entry for entry in catalog["processors"] if entry["id"] == "dynamics.compressor"
    )
    assert compressor["category"] == "dynamics"
    assert compressor["realtimeCost"] == "low"
    assert {"name", "type", "min", "max", "default", "unit"} <= set(compressor["params"][0])


def test_insert_param_metadata_reaches_the_python_facade() -> None:
    """Type, design default and measured range survive the C-ABI JSON hop.

    The values themselves are pinned in the C++ suite against the config structs
    and the construction path; what this covers is that the facade hands a host
    the whole descriptor rather than the name/id pair it used to carry.
    """
    import libsonare

    params = {
        param["name"]: param
        for param in libsonare.mastering_insert_param_info("dynamics.compressor")
    }
    # A validated control publishes its accepted range; an open one publishes
    # null on both, which is the catalog stating no limit rather than "unknown".
    assert params["ratio"]["min"] == 1.0
    assert params["ratio"]["max"] is None
    assert params["thresholdDb"]["min"] is None
    # The default is the config struct's own field initializer.
    assert params["ratio"]["default"] == 2.0
    assert params["thresholdDb"]["default"] == -18.0
    assert params["thresholdDb"]["unit"] == "dB"
    # A boolean config field arrives as a JSON boolean, with no range.
    assert params["autoMakeup"]["type"] == "boolean"
    assert params["autoMakeup"]["default"] is False
    assert params["autoMakeup"]["min"] is None

    # A two-sided range, and the per-band EQ surface, which is only declared
    # because the band readers publish their keys without being handed one.
    imager = {
        param["name"]: param for param in libsonare.mastering_insert_param_info("stereo.imager")
    }
    assert (imager["width"]["min"], imager["width"]["max"]) == (0.0, 2.0)
    parametric = {
        param["name"]: param for param in libsonare.mastering_insert_param_info("eq.parametric")
    }
    assert parametric["band0.frequencyHz"]["default"] == 1000.0
    assert parametric["band0.gainDb"]["unit"] == "dB"


def test_mastering_pair_accepts_differing_reference_length() -> None:
    """Pair process/analyze accept a reference that differs in length from source."""
    import libsonare

    sr = 44100
    source = [0.18 * math.sin(2 * math.pi * 440 * i / sr) for i in range(sr // 4)]
    reference = [0.12 * math.sin(2 * math.pi * 880 * i / sr) for i in range(sr // 8)]  # shorter
    assert len(reference) != len(source)

    paired = libsonare.mastering_pair_process(
        "match.abCrossfade", source, reference, sample_rate=sr, params={"mix": 0.5}
    )
    assert len(paired.samples) > 0

    pair_json = libsonare.mastering_pair_analyze(
        "match.referenceLoudness", source, reference, sample_rate=sr
    )
    assert '"referenceLufs"' in pair_json


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


def _decorrelated_pair(sr: int, seconds: int) -> tuple[list[float], list[float], list[float]]:
    n = sr * seconds
    left = [0.2 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(n)]
    right = [0.2 * math.sin(2 * math.pi * 1731 * i / sr) for i in range(n)]
    downmix = [0.5 * (a + b) for a, b in zip(left, right, strict=True)]
    return left, right, downmix


def test_stereo_mastering_analysis_uses_channel_summed_loudness() -> None:
    """The stereo analysis entry points measure the pair, not a downmix."""
    import json

    import libsonare

    sr = 48000
    left, right, downmix = _decorrelated_pair(sr, 4)

    stereo = json.loads(libsonare.mastering_streaming_preview_stereo(left, right, sample_rate=sr))
    mono = json.loads(libsonare.mastering_streaming_preview(downmix, sample_rate=sr))
    stereo_lufs = stereo["platforms"][0]["integratedLufs"]
    mono_lufs = mono["platforms"][0]["integratedLufs"]

    # BS.1770 sums the channel powers while the 0.5*(L+R) downmix quarters them,
    # so a decorrelated pair reads 6.02 dB above what a mono caller can measure,
    # and the normalization gain built on it is off by the same amount.
    assert stereo_lufs - mono_lufs == pytest.approx(6.02, abs=0.3)
    interleaved = [value for pair in zip(left, right, strict=True) for value in pair]
    assert stereo_lufs == pytest.approx(
        libsonare.lufs_interleaved(interleaved, 2, sr).integrated_lufs, abs=0.01
    )
    assert stereo["platforms"][0]["normalizationGainDb"] == pytest.approx(
        -14.0 - stereo_lufs, abs=0.001
    )

    profile = json.loads(libsonare.mastering_audio_profile_stereo(left, right, sample_rate=sr))
    assert profile["loudness"]["integratedLufs"] == pytest.approx(stereo_lufs, abs=0.01)
    assert "centroidHz" in profile["spectral"]

    suggestion = json.loads(
        libsonare.mastering_assistant_suggest_stereo(left, right, sample_rate=sr)
    )
    assert "chainConfig" in suggestion
    assert "explanation" in suggestion


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


def test_mastering_chain_validates_offline_input() -> None:
    """The offline mastering chain rejects empty / out-of-range-rate / non-finite
    input. Validation lives in the C++ core (MasteringChain::process_mono /
    process_stereo), so every surface (C ABI, Node, WASM, Python) behaves the
    same way."""
    import libsonare

    sr = 44100
    ok = [0.1 * math.sin(2 * math.pi * 220 * i / sr) for i in range(2048)]

    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_chain([], sample_rate=sr)
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_chain(ok, sample_rate=100)
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_chain(ok, sample_rate=10_000_000)

    bad = list(ok)
    bad[10] = float("nan")
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_chain(bad, sample_rate=sr)

    # Valid input still masters and preserves length.
    result = libsonare.mastering_chain(ok, sample_rate=sr)
    assert len(result.samples) == len(ok)


def test_mastering_chain_reports_output_metrics() -> None:
    """The chain result exposes output true-peak (dBTP), LRA, and per-stage gain
    reductions computed by the core. The gain-reduction stages are a subset
    of the reported stages, in the same order."""
    import math

    import libsonare

    sr = 44100
    samples = [0.3 * math.sin(2 * math.pi * 220 * i / sr) for i in range(4096)]
    config = {
        "dynamics": {"compressor": {"thresholdDb": -30.0, "ratio": 4.0}},
        "loudness": {"targetLufs": -14.0, "ceilingDb": -1.0},
    }
    result = libsonare.mastering_chain(samples, sample_rate=sr, config=config)

    # Output true peak must be finite and near/under the -1 dBTP ceiling.
    assert math.isfinite(result.output_true_peak_dbtp)
    assert result.output_true_peak_dbtp <= 0.0
    assert math.isfinite(result.output_lra)
    assert result.output_lra >= 0.0
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
    preset_result = libsonare.master_audio(samples, sample_rate=sr, preset_name="pop")
    assert math.isfinite(preset_result.output_true_peak_dbtp)
    assert math.isfinite(preset_result.output_lra)
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
            samples, sample_rate=sr, target_lufs=target_lufs, ceiling_db=ceiling_db
        )
        named = libsonare.mastering_process(
            "maximizer.loudnessOptimize",
            samples,
            sample_rate=sr,
            params={"targetLufs": target_lufs, "ceilingDb": ceiling_db},
        )
        # The chain's loudness stage is the one master_audio composes; a preset
        # only supplies different target / ceiling values.
        chain = libsonare.mastering_chain(
            samples,
            sample_rate=sr,
            config={"loudness": {"targetLufs": target_lufs, "ceilingDb": ceiling_db}},
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
    params = {"targetLufs": target_lufs, "ceilingDb": ceiling_db, "truePeakOversample": 4}

    mono = libsonare.mastering_process(
        "maximizer.loudnessOptimize", samples, sample_rate=sr, params=params
    )
    stereo = libsonare.mastering_process_stereo(
        "maximizer.loudnessOptimize", samples, samples, sample_rate=sr, params=params
    )

    assert mono.loudness_target_limited is True
    assert stereo.loudness_target_limited == mono.loudness_target_limited
    assert stereo.output_lufs < target_lufs


def test_mastering_assistant_suggest_follows_target_platform() -> None:
    """A delivery target reaches the assistant as a name and moves the loudness."""
    import libsonare

    sr = 48000
    samples = [0.2 * math.sin(2 * math.pi * 220 * i / sr) for i in range(sr)]

    names = libsonare.mastering_platform_names()
    assert "broadcast" in names
    assert "club" in names

    # Without a target the assistant returns the streaming default of -14 LUFS.
    default_json = libsonare.mastering_assistant_suggest(samples, sample_rate=sr)
    assert '"loudness.targetLufs":-14' in default_json

    broadcast_json = libsonare.mastering_assistant_suggest(
        samples, sample_rate=sr, params={"targetPlatform": "broadcast"}
    )
    assert '"loudness.targetLufs":-23' in broadcast_json

    # Only a loud delivery format moves the ceiling as well as the target.
    club_json = libsonare.mastering_assistant_suggest(
        samples, sample_rate=sr, params={"targetPlatform": "club"}
    )
    assert '"loudness.targetLufs":-9' in club_json
    assert '"loudness.ceilingDb":-0.3' in club_json

    with pytest.raises(ValueError):
        libsonare.mastering_assistant_suggest(
            samples, sample_rate=sr, params={"targetPlatform": "vinyl"}
        )
    # The numeric index is a transport detail, not part of the Python vocabulary.
    with pytest.raises(ValueError):
        libsonare.mastering_assistant_suggest(samples, sample_rate=sr, params={"targetPlatform": 2})
