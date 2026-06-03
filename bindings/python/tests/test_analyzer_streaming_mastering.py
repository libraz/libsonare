"""Streaming mastering and advanced analyzer API tests."""

from __future__ import annotations

import math

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


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
