"""Tests for the offline stereo / phase-scope / spectrum Python wrappers.

These are thin pass-throughs over sonare_c_editing.cpp entries: this suite
verifies the ctypes marshaling and that heap arrays come back as numpy float32
without leaking the underlying C buffer.
"""

from __future__ import annotations

import math

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


SR = 22050


def _sine(freq: float, duration: float) -> NDArray[np.float32]:
    t = np.linspace(0.0, duration, int(SR * duration), endpoint=False, dtype=np.float32)
    return np.asarray(0.5 * np.sin(2 * math.pi * freq * t), dtype=np.float32)


# ---------------------------------------------------------------------------
# Stereo wrappers
# ---------------------------------------------------------------------------


def test_metering_stereo_correlation_in_phase_and_inverted() -> None:
    left = _sine(440.0, 0.5)
    right = left.copy()
    inverted = -left
    assert libsonare.metering_stereo_correlation(left, right, SR) == pytest.approx(1.0, abs=1e-3)
    assert libsonare.metering_stereo_correlation(left, inverted, SR) == pytest.approx(
        -1.0, abs=1e-3
    )


def test_metering_stereo_width_mono_vs_inverted() -> None:
    left = _sine(440.0, 0.5)
    width_mono = libsonare.metering_stereo_width(left, left, SR)
    width_inv = libsonare.metering_stereo_width(left, -left, SR)
    assert abs(width_mono) < 1e-3
    assert width_inv > width_mono


def test_metering_vectorscope_returns_float32_arrays() -> None:
    left = _sine(440.0, 0.1)
    report = libsonare.metering_vectorscope(left, left, SR)
    assert isinstance(report, libsonare.VectorscopeReport)
    assert report.mid.dtype == np.float32
    assert report.side.dtype == np.float32
    assert report.mid.shape == left.shape
    assert report.side.shape == left.shape
    # In-phase: side ≈ 0 everywhere.
    assert float(np.max(np.abs(report.side))) < 1e-3


def test_metering_phase_scope_populates_summary_stats() -> None:
    left = _sine(440.0, 0.1)
    report = libsonare.metering_phase_scope(left, left, SR)
    assert isinstance(report, libsonare.PhaseScopeReport)
    assert report.mid.shape == left.shape
    assert report.side.shape == left.shape
    assert report.radius.shape == left.shape
    assert report.angle_rad.shape == left.shape
    assert report.correlation == pytest.approx(1.0, abs=1e-3)
    assert report.max_radius > 0


def test_metering_stereo_rejects_mismatched_lengths() -> None:
    left = _sine(440.0, 0.1)
    right = _sine(440.0, 0.05)
    with pytest.raises(ValueError):
        libsonare.metering_stereo_correlation(left, right, SR)
    with pytest.raises(ValueError):
        libsonare.metering_vectorscope(left, right, SR)


def test_metering_stereo_rejects_invalid_sample_rates() -> None:
    left = _sine(440.0, 0.1)
    with pytest.raises(RuntimeError):
        libsonare.metering_stereo_correlation(left, left, 0)
    with pytest.raises(RuntimeError):
        libsonare.metering_phase_scope(left, left, -1)


# ---------------------------------------------------------------------------
# Spectrum wrapper
# ---------------------------------------------------------------------------


def test_metering_spectrum_returns_expected_bins_and_peak() -> None:
    samples = _sine(1000.0, 0.5)
    n_fft = 2048
    report = libsonare.metering_spectrum(samples, SR, n_fft=n_fft)
    assert isinstance(report, libsonare.SpectrumReport)
    assert report.frequencies.shape == (n_fft // 2 + 1,)
    assert report.magnitude.shape == (n_fft // 2 + 1,)
    assert report.power.shape == (n_fft // 2 + 1,)
    assert report.db.shape == (n_fft // 2 + 1,)
    assert report.n_fft == n_fft
    assert report.sample_rate == SR
    peak_bin = int(np.argmax(report.magnitude))
    assert report.frequencies[peak_bin] == pytest.approx(1000.0, abs=60.0)
    expected_power = float(report.magnitude[peak_bin]) ** 2
    # Large-magnitude bins drift in the last sig figs; check ratio.
    assert abs(float(report.power[peak_bin]) - expected_power) / expected_power < 1e-5


def test_metering_spectrum_uses_defaults() -> None:
    samples = _sine(440.0, 0.5)
    report = libsonare.metering_spectrum(samples, SR)
    assert report.n_fft == 2048
    assert report.frequencies.shape == (2048 // 2 + 1,)


def test_metering_spectrum_rejects_non_power_of_two_n_fft() -> None:
    samples = _sine(440.0, 0.1)
    with pytest.raises(RuntimeError):
        libsonare.metering_spectrum(samples, SR, n_fft=1500)
