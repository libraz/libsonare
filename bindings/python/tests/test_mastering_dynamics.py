"""Tests for the offline mastering dynamics Python wrappers.

These functions are ctypes pass-throughs over mastering::dynamics::Compressor /
Gate / TransientShaper. The suite verifies that buffers come back as numpy
``float32`` arrays with the right length, the latency value is plumbed
through, and detector aliases are accepted.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    DynamicsProcessorResult,
    ErrorCode,
    SonareValueError,
    mastering_dynamics_compressor,
    mastering_dynamics_gate,
    mastering_dynamics_transient_shaper,
)

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 44100


class TestCompressor:
    def test_default_params_runs(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.8)
        result = mastering_dynamics_compressor(x, SR)
        assert isinstance(result, DynamicsProcessorResult)
        with pytest.raises(TypeError):
            iter(result)
        y, latency = result.samples, result.latency_samples
        assert isinstance(y, np.ndarray)
        assert y.dtype == np.float32
        assert y.shape == x.shape
        assert np.all(np.isfinite(y))
        assert latency >= 0

    def test_reduces_peak_above_threshold(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.9)
        y = mastering_dynamics_compressor(
            x, SR, threshold_db=-30.0, ratio=10.0, attack_ms=0.1, release_ms=10.0
        ).samples
        # After the attack settles, the steady-state peak should be below the
        # input peak. Skip the first half to avoid the transient.
        half = len(x) // 2
        x_peak = float(np.max(np.abs(x[half:])))
        y_peak = float(np.max(np.abs(y[half:])))
        assert y_peak < x_peak * 0.95

    def test_detector_is_a_token(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.8)
        with pytest.raises(
            TypeError,
            match="^mastering_dynamics_compressor: detector must be one of peak, rms, logRms$",
        ):
            mastering_dynamics_compressor(x, SR, detector=0)  # type: ignore[arg-type]
        with pytest.raises(
            ValueError,
            match="^mastering_dynamics_compressor: detector must be one of peak, rms, logRms$",
        ):
            mastering_dynamics_compressor(x, SR, detector="log_rms")  # type: ignore[arg-type]

    def test_rejects_unknown_detector_string(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.8)
        with pytest.raises(
            ValueError,
            match="^mastering_dynamics_compressor: detector must be one of peak, rms, logRms$",
        ):
            mastering_dynamics_compressor(x, SR, detector="not-a-mode")  # type: ignore[arg-type]

    def test_sidechain_high_pass_eases_compression_of_a_low_tone(self) -> None:
        x = sine(60.0, 0.5, sr=SR, amp=0.8)
        plain = mastering_dynamics_compressor(x, SR, threshold_db=-30.0, ratio=8.0).samples
        keyed = mastering_dynamics_compressor(
            x, SR, threshold_db=-30.0, ratio=8.0, sidechain_high_pass_hz=2000.0
        ).samples
        assert float(np.max(np.abs(keyed))) > float(np.max(np.abs(plain)))

    @pytest.mark.parametrize("corner", [0.0, -1.0])
    def test_refuses_a_sidechain_high_pass_that_is_not_positive(self, corner: float) -> None:
        x = sine(440.0, 0.1, sr=SR, amp=0.5)
        with pytest.raises(SonareValueError, match="sidechain_high_pass_hz"):
            mastering_dynamics_compressor(x, SR, sidechain_high_pass_hz=corner)

    def test_empty_input_raises(self) -> None:
        with pytest.raises(ValueError, match="samples must not be empty"):
            mastering_dynamics_compressor(np.array([], dtype=np.float32), SR)


class TestGate:
    def test_default_params_runs(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.8)
        result = mastering_dynamics_gate(x, SR)
        y, latency = result.samples, result.latency_samples
        assert isinstance(y, np.ndarray)
        assert y.dtype == np.float32
        assert y.shape == x.shape
        assert np.all(np.isfinite(y))
        assert latency >= 0

    def test_attenuates_quiet_signal(self) -> None:
        # A signal well below the threshold should be attenuated by the gate's
        # closed-state range.
        x = sine(440.0, 0.5, sr=SR, amp=0.001)
        y = mastering_dynamics_gate(x, SR, threshold_db=-20.0, range_db=-60.0).samples
        # Tail half: the envelope should have settled below the input level.
        half = len(x) // 2
        x_peak = float(np.max(np.abs(x[half:])))
        y_peak = float(np.max(np.abs(y[half:])))
        assert y_peak < x_peak

    def test_explicit_kwargs(self) -> None:
        x = sine(440.0, 0.3, sr=SR, amp=0.8)
        y = mastering_dynamics_gate(
            x,
            SR,
            threshold_db=-40.0,
            attack_ms=5.0,
            release_ms=100.0,
            range_db=-90.0,
            hold_ms=10.0,
            close_threshold_db=-45.0,
            key_high_pass_hz=120.0,
        ).samples
        assert y.shape == x.shape

    def test_a_zero_key_high_pass_is_the_unfiltered_key(self) -> None:
        x = sine(440.0, 0.3, sr=SR, amp=0.8)
        omitted = mastering_dynamics_gate(x, SR).samples
        zero = mastering_dynamics_gate(x, SR, key_high_pass_hz=0.0).samples
        np.testing.assert_array_equal(zero, omitted)

    def test_negative_infinite_range_db_is_supported(self) -> None:
        samples = np.zeros(512, dtype=np.float32)
        result = mastering_dynamics_gate(samples, SR, range_db=float("-inf"))
        assert result.samples.shape == samples.shape
        assert result.samples.dtype == np.float32

    @pytest.mark.parametrize("range_db", [1.0, float("inf"), float("nan")])
    def test_range_db_uses_native_domain(self, range_db: float) -> None:
        samples = np.zeros(512, dtype=np.float32)
        with pytest.raises(SonareValueError) as caught:
            mastering_dynamics_gate(samples, SR, range_db=range_db)
        assert caught.value.code == ErrorCode.INVALID_PARAMETER
        assert str(caught.value) == "mastering_dynamics_gate: range_db must be finite and at most 0"

    def test_range_db_refuses_finite_values_outside_float_width(self) -> None:
        samples = np.zeros(512, dtype=np.float32)
        with pytest.raises(SonareValueError) as caught:
            mastering_dynamics_gate(samples, SR, range_db=-1e39)
        assert str(caught.value) == (
            "mastering_dynamics_gate: range_db must be finite and in [-3.4028235e38, 3.4028235e38]"
        )

    def test_refuses_a_negative_key_high_pass(self) -> None:
        x = sine(440.0, 0.1, sr=SR, amp=0.5)
        with pytest.raises(SonareValueError, match="key_high_pass_hz"):
            mastering_dynamics_gate(x, SR, key_high_pass_hz=-1.0)


class TestTransientShaper:
    def test_default_params_runs(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.8)
        result = mastering_dynamics_transient_shaper(x, SR)
        y, latency = result.samples, result.latency_samples
        assert isinstance(y, np.ndarray)
        assert y.dtype == np.float32
        assert y.shape == x.shape
        assert np.all(np.isfinite(y))
        assert latency >= 0

    def test_different_gains_change_output(self) -> None:
        x = sine(440.0, 1.0, sr=SR, amp=0.8)
        ya = mastering_dynamics_transient_shaper(
            x, SR, attack_gain_db=6.0, sustain_gain_db=0.0
        ).samples
        yb = mastering_dynamics_transient_shaper(
            x, SR, attack_gain_db=0.0, sustain_gain_db=-6.0
        ).samples
        assert not np.array_equal(ya, yb)

    def test_explicit_kwargs(self) -> None:
        x = sine(440.0, 0.3, sr=SR, amp=0.8)
        y = mastering_dynamics_transient_shaper(
            x,
            SR,
            attack_gain_db=3.0,
            sustain_gain_db=-2.0,
            fast_attack_ms=0.5,
            fast_release_ms=30.0,
            slow_attack_ms=20.0,
            slow_release_ms=250.0,
            sensitivity=1.2,
            max_gain_db=10.0,
            gain_smoothing_ms=1.0,
            lookahead_ms=2.0,
        ).samples
        assert y.shape == x.shape
