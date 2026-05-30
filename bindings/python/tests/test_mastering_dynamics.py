"""Tests for the offline mastering dynamics Python wrappers.

These functions are ctypes pass-throughs over mastering::dynamics::Compressor /
Gate / TransientShaper. The suite verifies that buffers come back as numpy
``float32`` arrays with the right length, the latency value is plumbed
through, and detector aliases are accepted.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

from libsonare import (
    mastering_dynamics_compressor,
    mastering_dynamics_gate,
    mastering_dynamics_transient_shaper,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 44100


def _sine(freq: float, dur: float = 1.0, amp: float = 0.8) -> NDArray[np.float32]:
    n = int(SR * dur)
    return (amp * np.sin(2.0 * np.pi * freq * np.arange(n) / SR)).astype(np.float32)


class TestCompressor:
    def test_default_params_runs(self) -> None:
        x = _sine(440.0)
        y, latency = mastering_dynamics_compressor(x, SR)
        assert isinstance(y, np.ndarray)
        assert y.dtype == np.float32
        assert y.shape == x.shape
        assert np.all(np.isfinite(y))
        assert latency >= 0

    def test_reduces_peak_above_threshold(self) -> None:
        x = _sine(440.0, amp=0.9)
        y, _ = mastering_dynamics_compressor(
            x, SR, threshold_db=-30.0, ratio=10.0, attack_ms=0.1, release_ms=10.0
        )
        # After the attack settles, the steady-state peak should be below the
        # input peak. Skip the first half to avoid the transient.
        half = len(x) // 2
        x_peak = float(np.max(np.abs(x[half:])))
        y_peak = float(np.max(np.abs(y[half:])))
        assert y_peak < x_peak * 0.95

    def test_detector_accepts_string_and_int(self) -> None:
        x = _sine(440.0)
        ya, _ = mastering_dynamics_compressor(x, SR, detector="peak")
        yb, _ = mastering_dynamics_compressor(x, SR, detector=0)
        np.testing.assert_array_equal(ya, yb)

    def test_detector_log_rms_alias(self) -> None:
        x = _sine(440.0)
        ya, _ = mastering_dynamics_compressor(x, SR, detector="log_rms")
        yb, _ = mastering_dynamics_compressor(x, SR, detector=2)
        np.testing.assert_array_equal(ya, yb)

    def test_rejects_unknown_detector_string(self) -> None:
        x = _sine(440.0)
        with pytest.raises(ValueError, match="unknown compressor detector"):
            mastering_dynamics_compressor(x, SR, detector="not-a-mode")

    def test_rejects_unknown_detector_int(self) -> None:
        x = _sine(440.0)
        with pytest.raises(ValueError, match="unknown compressor detector"):
            mastering_dynamics_compressor(x, SR, detector=999)

    def test_empty_input_raises(self) -> None:
        with pytest.raises(ValueError, match="samples must not be empty"):
            mastering_dynamics_compressor(np.array([], dtype=np.float32), SR)


class TestGate:
    def test_default_params_runs(self) -> None:
        x = _sine(440.0)
        y, latency = mastering_dynamics_gate(x, SR)
        assert isinstance(y, np.ndarray)
        assert y.dtype == np.float32
        assert y.shape == x.shape
        assert np.all(np.isfinite(y))
        assert latency >= 0

    def test_attenuates_quiet_signal(self) -> None:
        # A signal well below the threshold should be attenuated by the gate's
        # closed-state range.
        x = _sine(440.0, dur=0.5, amp=0.001)
        y, _ = mastering_dynamics_gate(x, SR, threshold_db=-20.0, range_db=-60.0)
        # Tail half: the envelope should have settled below the input level.
        half = len(x) // 2
        x_peak = float(np.max(np.abs(x[half:])))
        y_peak = float(np.max(np.abs(y[half:])))
        assert y_peak < x_peak

    def test_explicit_kwargs(self) -> None:
        x = _sine(440.0, dur=0.3)
        y, _ = mastering_dynamics_gate(
            x,
            SR,
            threshold_db=-40.0,
            attack_ms=5.0,
            release_ms=100.0,
            range_db=-90.0,
            hold_ms=10.0,
            close_threshold_db=-45.0,
            key_hpf_hz=120.0,
        )
        assert y.shape == x.shape


class TestTransientShaper:
    def test_default_params_runs(self) -> None:
        x = _sine(440.0)
        y, latency = mastering_dynamics_transient_shaper(x, SR)
        assert isinstance(y, np.ndarray)
        assert y.dtype == np.float32
        assert y.shape == x.shape
        assert np.all(np.isfinite(y))
        assert latency >= 0

    def test_different_gains_change_output(self) -> None:
        x = _sine(440.0)
        ya, _ = mastering_dynamics_transient_shaper(x, SR, attack_gain_db=6.0, sustain_gain_db=0.0)
        yb, _ = mastering_dynamics_transient_shaper(x, SR, attack_gain_db=0.0, sustain_gain_db=-6.0)
        assert not np.array_equal(ya, yb)

    def test_explicit_kwargs(self) -> None:
        x = _sine(440.0, dur=0.3)
        y, _ = mastering_dynamics_transient_shaper(
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
        )
        assert y.shape == x.shape
