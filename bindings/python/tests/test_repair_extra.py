"""Tests for the remaining offline mastering repair Python wrappers
(declip / decrackle / dehum / dereverb_classical / trim_silence).
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _sine(freq: float, duration_sec: float, amp: float = 0.3) -> NDArray[np.float32]:
    n = int(SR * duration_sec)
    return (amp * np.sin(2.0 * np.pi * freq * np.arange(n) / SR)).astype(np.float32)


class TestMasteringRepairDeclip:
    def test_default(self) -> None:
        samples = _sine(440.0, 0.3, amp=1.0)
        samples = np.clip(samples * 2.0, -0.9, 0.9).astype(np.float32)
        out = libsonare.mastering_repair_declip(samples, SR)
        assert out.shape == samples.shape
        assert out.dtype == np.float32

    def test_explicit_kwargs(self) -> None:
        samples = _sine(440.0, 0.2)
        out = libsonare.mastering_repair_declip(
            samples, SR, clip_threshold=0.85, lpc_order=24, iterations=1, lpc_blend=0.5
        )
        assert out.shape == samples.shape


class TestMasteringRepairDecrackle:
    def _sample(self) -> NDArray[np.float32]:
        samples = _sine(440.0, 0.3).copy()
        for i in range(500, samples.shape[0], 1700):
            samples[i] = 0.95 if i % 2 == 0 else -0.95
        return samples

    def test_median_default(self) -> None:
        samples = self._sample()
        out = libsonare.mastering_repair_decrackle(samples, SR)
        assert out.shape == samples.shape

    def test_wavelet_shrinkage(self) -> None:
        samples = self._sample()
        out = libsonare.mastering_repair_decrackle(
            samples, SR, mode="waveletShrinkage", threshold=0.4, levels=4
        )
        assert out.shape == samples.shape

    def test_unknown_mode_raises(self) -> None:
        samples = self._sample()
        with pytest.raises(ValueError, match="unknown decrackle mode"):
            libsonare.mastering_repair_decrackle(samples, SR, mode="not-a-mode")

    def test_unknown_mode_int_raises(self) -> None:
        samples = self._sample()
        with pytest.raises(ValueError, match="unknown decrackle mode"):
            libsonare.mastering_repair_decrackle(samples, SR, mode=999)


class TestMasteringRepairDehum:
    def _sample(self) -> NDArray[np.float32]:
        signal = _sine(440.0, 0.5, amp=0.5)
        hum = _sine(50.0, 0.5, amp=0.2)
        return (signal + hum).astype(np.float32)

    def test_static_notch_default(self) -> None:
        samples = self._sample()
        out = libsonare.mastering_repair_dehum(samples, SR)
        assert out.shape == samples.shape

    def test_adaptive_tracking(self) -> None:
        samples = self._sample()
        out = libsonare.mastering_repair_dehum(
            samples,
            SR,
            fundamental_hz=50.0,
            harmonics=4,
            q=20.0,
            adaptive=True,
            search_range_hz=2.0,
            adaptation=0.25,
            frame_size=2048,
            pll_bandwidth=0.01,
        )
        assert out.shape == samples.shape


class TestMasteringRepairDereverbClassical:
    def test_default(self) -> None:
        samples = _sine(440.0, 0.5, amp=0.5)
        out = libsonare.mastering_repair_dereverb_classical(samples, SR)
        assert out.shape == samples.shape

    def test_wpe_enabled(self) -> None:
        samples = _sine(440.0, 0.5, amp=0.5)
        out = libsonare.mastering_repair_dereverb_classical(
            samples,
            SR,
            wpe_enabled=True,
            wpe_iterations=2,
            wpe_taps=3,
            wpe_strength=0.7,
            n_fft=1024,
            hop_length=256,
        )
        assert out.shape == samples.shape

    def test_rejects_non_power_of_two_n_fft(self) -> None:
        samples = _sine(440.0, 0.1)
        with pytest.raises(ValueError, match="power of two"):
            libsonare.mastering_repair_dereverb_classical(samples, SR, n_fft=1500)

    def test_rejects_hop_greater_than_n_fft(self) -> None:
        samples = _sine(440.0, 0.1)
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_dereverb_classical(samples, SR, n_fft=1024, hop_length=2048)


class TestMasteringRepairTrimSilence:
    def _sample(self) -> NDArray[np.float32]:
        pad = np.zeros(1200, dtype=np.float32)
        signal = _sine(440.0, 0.2, amp=0.5)
        return np.concatenate([pad, signal, pad]).astype(np.float32)

    def test_peak_mode_shortens_buffer(self) -> None:
        samples = self._sample()
        out = libsonare.mastering_repair_trim_silence(samples, SR)
        assert 0 < out.shape[0] < samples.shape[0]

    def test_lufs_gated_with_padding(self) -> None:
        samples = self._sample()
        out = libsonare.mastering_repair_trim_silence(
            samples,
            SR,
            mode="lufsGated",
            gate_lufs=-40.0,
            window_ms=400.0,
            padding_samples=600,
        )
        assert out.shape[0] > 0

    def test_unknown_mode_raises(self) -> None:
        samples = self._sample()
        with pytest.raises(ValueError, match="unknown trim_silence mode"):
            libsonare.mastering_repair_trim_silence(samples, SR, mode="not-a-mode")

    def test_unknown_mode_int_raises(self) -> None:
        samples = self._sample()
        with pytest.raises(ValueError, match="unknown trim_silence mode"):
            libsonare.mastering_repair_trim_silence(samples, SR, mode=999)

    def test_negative_padding_raises(self) -> None:
        samples = self._sample()
        with pytest.raises(ValueError, match="padding_samples"):
            libsonare.mastering_repair_trim_silence(samples, SR, padding_samples=-1)
