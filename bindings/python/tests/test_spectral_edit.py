"""Tests for the ``spectral_edit`` Python facade.

Region-based spectral editing is a ctypes pass-through over the C ABI
``sonare_spectral_edit`` (STFT -> per-op bin/frame masking -> iSTFT). The suite
verifies the identity transform preserves the signal, a band attenuation lowers
that band's energy, and validation rejects bad parameters.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _sine(freq: float, duration_sec: float, amp: float = 0.4) -> NDArray[np.float32]:
    n = int(SR * duration_sec)
    return (amp * np.sin(2.0 * np.pi * freq * np.arange(n) / SR)).astype(np.float32)


def _band_energy(samples: NDArray[np.float32], low_hz: float, high_hz: float) -> float:
    spec = np.fft.rfft(samples)
    freqs = np.fft.rfftfreq(len(samples), 1.0 / SR)
    mask = (freqs >= low_hz) & (freqs <= high_hz)
    return float(np.sum(np.abs(spec[mask]) ** 2))


def test_identity_transform_preserves_length_and_signal() -> None:
    x = _sine(440.0, 0.5)
    out = np.asarray(libsonare.spectral_edit(x, SR, []), dtype=np.float32)
    assert len(out) == len(x)
    assert np.all(np.isfinite(out))
    # Skip windowed edges; the interior reconstructs the input.
    skip = 2048
    sig = float(np.sum(x[skip:-skip] ** 2))
    noise = float(np.sum((x[skip:-skip] - out[skip:-skip]) ** 2))
    snr_db = 10.0 * np.log10(sig / noise)
    assert snr_db > 20.0


def test_attenuate_band_lowers_that_band() -> None:
    x = _sine(1000.0, 0.5) + _sine(5000.0, 0.5)
    op = libsonare.SpectralRegionOp(
        start_sample=0,
        end_sample=len(x),
        low_hz=4000.0,
        high_hz=6000.0,
        gain_db=-24.0,
        mode="attenuate",
    )
    out = np.asarray(libsonare.spectral_edit(x, SR, [op]), dtype=np.float32)
    assert len(out) == len(x)

    high_before = _band_energy(x, 4000.0, 6000.0)
    high_after = _band_energy(out, 4000.0, 6000.0)
    low_before = _band_energy(x, 800.0, 1200.0)
    low_after = _band_energy(out, 800.0, 1200.0)

    # Target band drops substantially; the 1 kHz tone is preserved.
    assert 10.0 * np.log10(high_after / high_before) < -15.0
    assert abs(10.0 * np.log10(low_after / low_before)) < 3.0


def test_mute_mode_zeros_a_band() -> None:
    x = _sine(1000.0, 0.5) + _sine(5000.0, 0.5)
    op = libsonare.SpectralRegionOp(0, len(x), 4000.0, 6000.0, mode="mute")
    out = np.asarray(libsonare.spectral_edit(x, SR, [op]), dtype=np.float32)
    high_before = _band_energy(x, 4000.0, 6000.0)
    high_after = _band_energy(out, 4000.0, 6000.0)
    assert high_after < high_before * 0.05


def test_invalid_parameters_raise() -> None:
    x = _sine(440.0, 0.25)
    with pytest.raises((ValueError, Exception)):
        libsonare.spectral_edit(x, SR, [], n_fft=2000)  # not a power of two
    with pytest.raises((ValueError, Exception)):
        libsonare.spectral_edit(x, SR, [], n_fft=2048, hop_length=2048)  # hop > n_fft/2
    with pytest.raises((ValueError, Exception)):
        op = libsonare.SpectralRegionOp(0, len(x), 0.0, 0.0, mode="nonsense")
        libsonare.spectral_edit(x, SR, [op])
