"""Tests for the remaining offline mastering repair Python wrappers
(declip / decrackle / dehum / dereverb_classical / trim_silence).
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


class TestMasteringRepairDeclip:
    def test_default(self) -> None:
        samples = sine(440.0, 0.3, amp=1.0)
        samples = np.clip(samples * 2.0, -0.9, 0.9).astype(np.float32)
        out = libsonare.mastering_repair_declip(samples, SR)
        assert out.shape == samples.shape
        assert out.dtype == np.float32

    def test_explicit_kwargs(self) -> None:
        samples = sine(440.0, 0.2, amp=0.3)
        out = libsonare.mastering_repair_declip(
            samples, SR, clip_threshold=0.85, lpc_order=24, iterations=1, lpc_blend=0.5
        )
        assert out.shape == samples.shape


class TestMasteringRepairDecrackle:
    def _sample(self) -> NDArray[np.float32]:
        samples = sine(440.0, 0.3, amp=0.3).copy()
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
        signal = sine(440.0, 0.5, amp=0.5)
        hum = sine(50.0, 0.5, amp=0.2)
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
        samples = sine(440.0, 0.5, amp=0.5)
        out = libsonare.mastering_repair_dereverb_classical(samples, SR)
        assert out.shape == samples.shape

    def test_wpe_enabled(self) -> None:
        samples = sine(440.0, 0.5, amp=0.5)
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
        samples = sine(440.0, 0.1, amp=0.3)
        with pytest.raises(ValueError, match="power of two"):
            libsonare.mastering_repair_dereverb_classical(samples, SR, n_fft=1500)

    def test_rejects_hop_greater_than_n_fft(self) -> None:
        samples = sine(440.0, 0.1, amp=0.3)
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_dereverb_classical(samples, SR, n_fft=1024, hop_length=2048)


class TestMasteringRepairDereverbConfigForRoom:
    # Bands 125/250/500/1k/2k/4k: only the middle pair counts, so the outliers
    # on either side are far from the answer on purpose.
    MID_BANDS = [9.0, 9.0, 1.0, 2.0, 9.0, 9.0]

    def _estimate(
        self, volume: float, rt60_bands: list[float] | None = None
    ) -> libsonare.RoomEstimate:
        return libsonare.RoomEstimate(
            volume=volume,
            length=0.0,
            width=0.0,
            height=0.0,
            drr_db=0.0,
            confidence=0.0,
            absorption_bands=[],
            rt60_bands=self.MID_BANDS if rt60_bands is None else rt60_bands,
        )

    def test_sets_the_two_fields_a_measurement_determines(self) -> None:
        config = libsonare.mastering_repair_dereverb_config_for_room(self._estimate(2500.0))
        assert config["t60_sec"] == pytest.approx(1.5)
        assert config["late_delay_ms"] == pytest.approx(50.0)  # sqrt(2500)

    def test_taste_fields_come_back_as_they_went_in(self) -> None:
        config = libsonare.mastering_repair_dereverb_config_for_room(
            self._estimate(2500.0),
            attenuation=0.9,
            threshold=0.02,
            over_subtraction=1.4,
            spectral_floor=0.05,
            n_fft=2048,
            hop_length=512,
        )
        assert config["attenuation"] == pytest.approx(0.9)
        assert config["threshold"] == pytest.approx(0.02)
        assert config["over_subtraction"] == pytest.approx(1.4)
        assert config["spectral_floor"] == pytest.approx(0.05)
        assert config["n_fft"] == 2048
        assert config["hop_length"] == 512

    def test_mixing_time_follows_the_volume(self) -> None:
        small = libsonare.mastering_repair_dereverb_config_for_room(self._estimate(100.0))
        large = libsonare.mastering_repair_dereverb_config_for_room(self._estimate(20000.0))
        assert small["late_delay_ms"] == pytest.approx(10.0)
        assert large["late_delay_ms"] == pytest.approx(141.42, rel=1e-3)

    def test_an_estimate_with_no_volume_configures_only_the_time(self) -> None:
        config = libsonare.mastering_repair_dereverb_config_for_room(
            self._estimate(0.0), late_delay_ms=33.0
        )
        assert config["t60_sec"] == pytest.approx(1.5)
        assert config["late_delay_ms"] == pytest.approx(33.0)

    def test_an_estimate_with_no_bands_configures_only_the_delay(self) -> None:
        config = libsonare.mastering_repair_dereverb_config_for_room(
            self._estimate(900.0, rt60_bands=[]), t60_sec=0.7
        )
        assert config["t60_sec"] == pytest.approx(0.7)
        assert config["late_delay_ms"] == pytest.approx(30.0)

    def test_a_band_that_did_not_converge_is_skipped_not_averaged_in(self) -> None:
        nan = float("nan")
        config = libsonare.mastering_repair_dereverb_config_for_room(
            self._estimate(400.0, rt60_bands=[0.0, 0.0, nan, 2.0, 0.0, 0.0])
        )
        assert config["t60_sec"] == pytest.approx(2.0)

    def test_none_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="estimate"):
            libsonare.mastering_repair_dereverb_config_for_room(None)  # type: ignore[arg-type]

    def test_omitted_fields_are_the_library_defaults_not_zeros(self) -> None:
        # The C ABI takes every config field literally -- there is no
        # zero-is-default rule -- so the keyword defaults here are the library's
        # own, and an estimate alone yields a config that is ready to run.
        config = libsonare.mastering_repair_dereverb_config_for_room(self._estimate(2500.0))
        assert config["threshold"] == pytest.approx(0.05)
        assert config["attenuation"] == pytest.approx(0.5)
        assert config["n_fft"] == 1024
        assert config["hop_length"] == 256
        assert config["over_subtraction"] == pytest.approx(1.0)
        assert config["spectral_floor"] == pytest.approx(0.08)
        assert config["wpe_enabled"] is False
        assert config["wpe_iterations"] == 2
        assert config["wpe_taps"] == 3
        assert config["wpe_strength"] == pytest.approx(0.7)

    def test_an_explicit_zero_is_taken_literally(self) -> None:
        config = libsonare.mastering_repair_dereverb_config_for_room(
            self._estimate(2500.0), attenuation=0.0, spectral_floor=0.0
        )
        assert config["attenuation"] == 0.0
        assert config["spectral_floor"] == 0.0

    def test_the_result_runs_the_dereverberator(self) -> None:
        samples = sine(440.0, 0.3, amp=0.5)
        config = libsonare.mastering_repair_dereverb_config_for_room(self._estimate(2500.0))
        out = libsonare.mastering_repair_dereverb_classical(samples, SR, **config)
        assert out.shape == samples.shape
        assert np.isfinite(out).all()


class TestMasteringRepairTrimSilence:
    def _sample(self) -> NDArray[np.float32]:
        pad = np.zeros(1200, dtype=np.float32)
        signal = sine(440.0, 0.2, amp=0.5)
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
