"""Streaming mastering and advanced analyzer API tests."""

from __future__ import annotations

import math

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


def test_stream_analyzer_rejects_malformed_config_geometry() -> None:
    """StreamAnalyzer rejects malformed config geometry, matching every surface.

    The shared C++ constructor and the flat C ABI enforce the same relationship
    and positive-value contract, so Python construction raises instead of
    silently producing garbage spectra.
    """
    from libsonare import SonareError, StreamAnalyzer, StreamConfig

    for bad in (
        StreamConfig(sample_rate=0),
        StreamConfig(n_fft=0),
        StreamConfig(n_mels=0),
        StreamConfig(n_fft=1024, hop_length=2048),
        StreamConfig(fmin=8000.0, fmax=4000.0),
        StreamConfig(max_progression_entries=0),
    ):
        with pytest.raises(SonareError):
            StreamAnalyzer(bad)

    # A well-formed config still constructs.
    StreamAnalyzer(StreamConfig(sample_rate=22050)).close()


def test_stream_analyzer_bounds_unread_frames_and_reports_drops() -> None:
    from libsonare import StreamAnalyzer, StreamConfig

    config = StreamConfig(
        sample_rate=8000,
        n_fft=32,
        hop_length=32,
        n_mels=8,
        max_pending_frames=3,
        max_progression_entries=3,
    )
    with StreamAnalyzer(config) as analyzer:
        analyzer.process([0.0] * (32 * 64))
        stats = analyzer.stats()
        assert stats.pending_frames == 3
        assert stats.dropped_output_frames > 0
        assert stats.pending_frames + stats.dropped_output_frames == stats.total_frames
        assert stats.dropped_chord_progression_entries == 0
        assert stats.dropped_bar_progression_entries == 0


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


def test_streaming_mastering_chain_preserves_ndarray_input() -> None:
    """process_* must not overwrite a float32 ndarray input (non-destructive)."""
    import numpy as np

    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 3.0})
    chain.prepare(sample_rate=44100, max_block_size=256, num_channels=1)
    block = np.full(256, 0.25, dtype=np.float32)
    original = block.copy()
    out = chain.process_mono(block)
    # The dry input is untouched; the returned wet block differs from it.
    assert np.array_equal(block, original)
    assert any(abs(out[i] - float(original[i])) > 1e-6 for i in range(len(out)))
    chain.close()


def test_streaming_equalizer_preserves_ndarray_input() -> None:
    """StreamingEqualizer.process_mono must not overwrite a float32 ndarray input."""
    import numpy as np

    from libsonare import StreamingEqualizer

    with StreamingEqualizer(sample_rate=44100, max_block_size=256) as eq:
        eq.set_band(
            0,
            {"type": "Peak", "frequencyHz": 1000.0, "gainDb": 6.0, "q": 1.0, "enabled": True},
        )
        block = np.full(256, 0.25, dtype=np.float32)
        original = block.copy()
        eq.process_mono(block)
        assert np.array_equal(block, original)


def test_streaming_mastering_chain_reports_stage_names() -> None:
    """stage_names() exposes the realized stages after prepare()."""
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
    # No stages are realized until prepare() runs.
    assert chain.stage_names() == []
    chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
    names = chain.stage_names()
    assert isinstance(names, list)
    assert all(isinstance(name, str) for name in names)
    assert any("eq.tilt" in name for name in names)
    chain.close()


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


def test_streaming_mastering_chain_accepts_loudness_with_static_gain() -> None:
    """A loudness-enabled config no longer raises when a static gain is supplied."""
    from libsonare import StreamingMasteringChain

    # Without a static gain a loudness target raises (cannot measure online)...
    with pytest.raises(RuntimeError):
        StreamingMasteringChain({"loudness.targetLufs": -14.0})

    # ...but supplying a precomputed static gain makes it processable.
    chain = StreamingMasteringChain(
        {"loudness.targetLufs": -14.0},
        loudness_static_gain_db=3.0,
        loudness_static_gain_peak_db=-1.0,
    )
    chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
    out = chain.process_mono([0.1] * 512)
    assert len(out) == 512
    assert all(math.isfinite(x) for x in out)
    chain.reset()


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
    """mastering_chain preserves void progress callback compatibility."""
    from libsonare import mastering_chain

    calls: list[tuple[float, str]] = []

    def on_progress(progress: float, stage: str) -> None:
        calls.append((progress, stage))

    result = mastering_chain(
        samples=[0.1] * 22050,
        sample_rate=22050,
        config={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
        on_progress=on_progress,
    )
    assert len(result.samples) == 22050
    assert len(calls) >= 2
    stages = [s for _, s in calls]
    assert "eq.tilt" in stages
    assert "dynamics.compressor" in stages
    # Final progress reaches 1.0.
    assert calls[-1][0] == pytest.approx(1.0, abs=1e-5)


def test_mastering_chain_false_progress_result_cancels_without_a_result() -> None:
    """Returning False after progress exceeds .5 cancels the native chain."""
    from libsonare import SonareError, mastering_chain
    from libsonare._runtime import _get_lib

    if not hasattr(_get_lib(), "sonare_mastering_chain_with_progress_ex"):
        pytest.skip("libsonare built without cancellation-capable mastering progress")

    calls: list[float] = []
    result = None

    def on_progress(progress: float, _stage: str) -> object:
        calls.append(progress)
        return False if progress > 0.5 else None

    with pytest.raises(SonareError) as exc:
        result = mastering_chain(
            samples=[0.1] * 22050,
            sample_rate=22050,
            config={
                "eq": {"tilt": {"tiltDb": 1.0}},
                "dynamics": {"compressor": {"thresholdDb": -24.0}},
            },
            on_progress=on_progress,
        )

    assert exc.value.code == 8
    assert any(progress > 0.5 for progress in calls)
    assert result is None


def test_mastering_chain_cancel_callable_cancels_without_a_result() -> None:
    """The keyword-only cancellation callback is forwarded to the C ABI."""
    from libsonare import SonareError, mastering_chain
    from libsonare._runtime import _get_lib

    if not hasattr(_get_lib(), "sonare_mastering_chain_with_progress_ex"):
        pytest.skip("libsonare built without cancellation-capable mastering progress")

    result = None
    with pytest.raises(SonareError) as exc:
        result = mastering_chain(
            samples=[0.1] * 22050,
            sample_rate=22050,
            config={"eq": {"tilt": {"tiltDb": 1.0}}},
            cancel=lambda: True,
        )

    assert exc.value.code == 8
    assert result is None


@pytest.mark.parametrize(
    "operation",
    ["chain-stereo", "preset-mono", "preset-stereo"],
)
def test_remaining_mastering_progress_variants_forward_cancellation(operation: str) -> None:
    """Every chain/preset mono/stereo progress variant uses its matching _ex ABI."""
    from libsonare import (
        SonareError,
        master_audio,
        master_audio_stereo,
        mastering_chain_stereo,
    )
    from libsonare._runtime import _get_lib

    symbols = {
        "chain-stereo": "sonare_mastering_chain_stereo_with_progress_ex",
        "preset-mono": "sonare_master_audio_with_progress_ex",
        "preset-stereo": "sonare_master_audio_stereo_with_progress_ex",
    }
    if not hasattr(_get_lib(), symbols[operation]):
        pytest.skip("libsonare built without cancellation-capable mastering progress")

    samples = [0.1] * 22050
    result = None
    with pytest.raises(SonareError) as exc:
        if operation == "chain-stereo":
            result = mastering_chain_stereo(
                samples,
                samples,
                sample_rate=22050,
                config={"eq": {"tilt": {"tiltDb": 1.0}}},
                cancel=lambda: True,
            )
        elif operation == "preset-mono":
            result = master_audio(samples, sample_rate=22050, cancel=lambda: True)
        else:
            result = master_audio_stereo(samples, samples, sample_rate=22050, cancel=lambda: True)

    assert exc.value.code == 8
    assert result is None


def test_mastering_chain_nested_and_flat_config_equivalent() -> None:
    """The nested (canonical) and flat dot-notation configs select the same stages."""
    from libsonare import mastering_chain

    samples = [0.1] * 22050
    nested = mastering_chain(
        samples=samples,
        sample_rate=22050,
        config={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
    )
    flat = mastering_chain(
        samples=samples,
        sample_rate=22050,
        config={"eq.tilt.tiltDb": 1.0, "dynamics.compressor.thresholdDb": -24.0},
    )
    assert nested.stages == flat.stages
    assert "eq.tilt" in nested.stages
    assert "dynamics.compressor" in nested.stages


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
