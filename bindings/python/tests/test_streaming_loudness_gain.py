"""Streaming loudness gain: the helper measures where the offline chain does."""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare

SR = 48000


def _program(amplitude: float, phase: float = 0.0) -> np.ndarray:
    t = np.arange(SR, dtype=np.float32) / SR
    return (
        amplitude * (0.5 * np.sin(2 * np.pi * 220 * t + phase) + 0.3 * np.sin(2 * np.pi * 1760 * t))
    ).astype(np.float32)


STAGED = {
    "loudness": {"targetLufs": -14.0},
    "eq": {"tilt": {"tiltDb": 3.0}},
    "dynamics": {"compressor": {"thresholdDb": -30.0, "ratio": 4.0, "makeupGainDb": 6.0}},
}


def test_mono_gain_equals_the_offline_applied_gain() -> None:
    samples = _program(0.05)
    for config in ({"loudness": {"targetLufs": -14.0}}, STAGED):
        gain = libsonare.streaming_loudness_gain(samples, SR, config)
        offline = libsonare.mastering_chain(samples, SR, config)
        assert gain.loudness_static_gain_db == pytest.approx(offline.applied_gain_db, abs=0)
        assert math.isfinite(gain.integrated_lufs)
        assert math.isfinite(gain.true_peak_db)


def test_measurement_follows_the_stages_before_loudness() -> None:
    samples = _program(0.05)
    staged = libsonare.streaming_loudness_gain(samples, SR, STAGED)
    plain = libsonare.streaming_loudness_gain(samples, SR, {"loudness": {"targetLufs": -14.0}})
    assert staged.integrated_lufs > plain.integrated_lufs + 1.0


def test_stereo_gain_equals_the_offline_applied_gain() -> None:
    left, right = _program(0.04), _program(0.05, 1.3)
    gain = libsonare.streaming_loudness_gain_stereo(left, right, SR, STAGED)
    offline = libsonare.mastering_chain_stereo(left, right, SR, STAGED)
    assert gain.loudness_static_gain_db == pytest.approx(offline.applied_gain_db, abs=0)


def test_silence_yields_zero_gain_and_builds_a_streaming_chain() -> None:
    silence = np.zeros(SR, dtype=np.float32)
    gain = libsonare.streaming_loudness_gain(silence, SR, STAGED)
    assert gain.loudness_static_gain_db == 0.0
    assert not math.isfinite(gain.integrated_lufs)
    with libsonare.StreamingMasteringChain(
        STAGED,
        loudness_static_gain_db=gain.loudness_static_gain_db,
        loudness_static_gain_peak_db=gain.true_peak_db,
    ) as chain:
        chain.prepare(SR, 512, 1)
        assert len(chain.process_mono([0.0] * 512)) == 512


def test_streaming_chain_built_from_the_helper_tracks_the_offline_render() -> None:
    samples = _program(0.05)
    gain = libsonare.streaming_loudness_gain(samples, SR, STAGED)
    offline = np.asarray(libsonare.mastering_chain(samples, SR, STAGED).samples)
    with libsonare.StreamingMasteringChain(
        STAGED,
        loudness_static_gain_db=gain.loudness_static_gain_db,
        loudness_static_gain_peak_db=gain.true_peak_db,
    ) as chain:
        chain.prepare(SR, 512, 1)
        out: list[float] = []
        for start in range(0, len(samples), 512):
            out.extend(chain.process_mono(samples[start : start + 512].tolist()))
        out.extend(chain.flush_mono())
    streamed = np.asarray(out, dtype=np.float32)
    # Latency-align; the streamed render leads the offline one by the chain latency.
    n = min(len(streamed), len(offline))
    tolerance = 0.02 * float(np.max(np.abs(offline)))
    best = min(
        float(np.max(np.abs(streamed[lag : lag + n - 1024] - offline[: n - 1024])))
        for lag in range(0, 4096, 1)
        if lag + n - 1024 <= len(streamed)
    )
    assert best <= tolerance


def test_rejects_bad_arguments() -> None:
    with pytest.raises(libsonare.SonareValueError):
        libsonare.streaming_loudness_gain_stereo(
            np.zeros(100, dtype=np.float32), np.zeros(99, dtype=np.float32), SR
        )
    with pytest.raises(libsonare.SonareError):
        libsonare.streaming_loudness_gain(np.zeros(SR, dtype=np.float32), SR, {"no": {"such": 1.0}})
