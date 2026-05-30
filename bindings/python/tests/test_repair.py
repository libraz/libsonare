"""Tests for the offline mastering repair Python wrappers.

These functions are ctypes pass-throughs over mastering::repair::declick and
mastering::repair::denoise_classical. The suite verifies that buffers come
back as numpy ``float32`` arrays with the right length, options plumb
through correctly, and validation rejects bad parameters.
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


def _with_noise(samples: NDArray[np.float32], amp: float) -> NDArray[np.float32]:
    rng = np.random.default_rng(42)
    return (samples + (rng.random(samples.shape).astype(np.float32) - 0.5) * amp).astype(np.float32)


class TestMasteringRepairDeclick:
    def test_default_options_return_same_length(self) -> None:
        samples = _sine(440.0, 0.3).copy()
        # Inject a few clicks.
        for i in range(5):
            samples[1000 + i * 1200] = 1.0
        out = libsonare.mastering_repair_declick(samples, SR)
        assert isinstance(out, np.ndarray)
        assert out.dtype == np.float32
        assert out.shape == samples.shape
        assert np.isfinite(out).all()

    def test_explicit_kwargs(self) -> None:
        samples = _sine(440.0, 0.2)
        out = libsonare.mastering_repair_declick(
            samples,
            SR,
            threshold=0.7,
            neighbor_ratio=4.0,
            max_click_samples=8,
            lpc_order=16,
            residual_ratio=6.0,
        )
        assert out.shape == samples.shape

    def test_rejects_non_positive_max_click_samples(self) -> None:
        samples = _sine(440.0, 0.2)
        with pytest.raises(ValueError, match="max_click_samples"):
            libsonare.mastering_repair_declick(samples, SR, max_click_samples=0)


class TestMasteringRepairDenoiseClassical:
    def test_logmmse_default(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.5, amp=0.5), 0.4)
        out = libsonare.mastering_repair_denoise_classical(noisy, SR)
        assert isinstance(out, np.ndarray)
        assert out.dtype == np.float32
        assert out.shape == noisy.shape

    def test_spectral_subtraction(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.5, amp=0.5), 0.4)
        out = libsonare.mastering_repair_denoise_classical(
            noisy,
            SR,
            mode="spectralSubtraction",
            noise_estimator="quantile",
            n_fft=1024,
            hop_length=256,
            over_subtraction=2.0,
            spectral_floor=0.05,
            speech_presence_gain=False,
        )
        assert out.shape == noisy.shape

    def test_mmse_stsa_with_int_enum(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.5, amp=0.5), 0.4)
        out = libsonare.mastering_repair_denoise_classical(
            noisy,
            SR,
            mode=libsonare._ffi.SONARE_DENOISE_MODE_MMSE_STSA,
            n_fft=512,
            hop_length=128,
        )
        assert out.shape == noisy.shape

    def test_rejects_non_power_of_two_n_fft(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.1, amp=0.5), 0.4)
        with pytest.raises(ValueError, match="power of two"):
            libsonare.mastering_repair_denoise_classical(noisy, SR, n_fft=1500)

    def test_rejects_non_positive_hop_length(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.1, amp=0.5), 0.4)
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_denoise_classical(noisy, SR, hop_length=0)

    def test_rejects_unknown_mode_string(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.1, amp=0.5), 0.4)
        with pytest.raises(ValueError, match="unknown denoise mode"):
            libsonare.mastering_repair_denoise_classical(noisy, SR, mode="not-a-mode")

    def test_rejects_unknown_mode_int(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.1, amp=0.5), 0.4)
        with pytest.raises(ValueError, match="unknown denoise mode"):
            libsonare.mastering_repair_denoise_classical(noisy, SR, mode=999)

    def test_rejects_unknown_noise_estimator_string(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.1, amp=0.5), 0.4)
        with pytest.raises(ValueError, match="unknown denoise noise estimator"):
            libsonare.mastering_repair_denoise_classical(
                noisy, SR, noise_estimator="not-an-estimator"
            )

    def test_rejects_unknown_noise_estimator_int(self) -> None:
        noisy = _with_noise(_sine(440.0, 0.1, amp=0.5), 0.4)
        with pytest.raises(ValueError, match="unknown denoise noise estimator"):
            libsonare.mastering_repair_denoise_classical(noisy, SR, noise_estimator=999)
