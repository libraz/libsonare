"""Tests for the ``spectral_edit`` Python facade.

Region-based spectral editing is a ctypes pass-through over the C ABI
``sonare_spectral_edit`` (STFT -> per-op bin/frame masking -> iSTFT). The suite
verifies the identity transform preserves the signal, a band attenuation lowers
that band's energy, and validation rejects bad parameters.
"""

from __future__ import annotations

from typing import get_args

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import SonareValueError
from libsonare.types import SpectralEditMode, WindowType

from ._helpers import LIB_AVAILABLE, raises_coded, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _band_energy(samples: NDArray[np.float32], low_hz: float, high_hz: float) -> float:
    spec = np.fft.rfft(samples)
    freqs = np.fft.rfftfreq(len(samples), 1.0 / SR)
    mask = (freqs >= low_hz) & (freqs <= high_hz)
    return float(np.sum(np.abs(spec[mask]) ** 2))


def test_identity_transform_preserves_length_and_signal() -> None:
    x = sine(440.0, 0.5, amp=0.4)
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
    x = sine(1000.0, 0.5, amp=0.4) + sine(5000.0, 0.5, amp=0.4)
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
    x = sine(1000.0, 0.5, amp=0.4) + sine(5000.0, 0.5, amp=0.4)
    op = libsonare.SpectralRegionOp(0, len(x), 4000.0, 6000.0, mode="mute")
    out = np.asarray(libsonare.spectral_edit(x, SR, [op]), dtype=np.float32)
    high_before = _band_energy(x, 4000.0, 6000.0)
    high_after = _band_energy(out, 4000.0, 6000.0)
    assert high_after < high_before * 0.05


def test_omitted_end_sample_spans_whole_signal() -> None:
    # An omitted end_sample (the -1 sentinel) must edit the whole signal, like
    # the Node/WASM facades whose endSample defaults to the signal length. A
    # region with start_sample=0 and no end_sample should mute the whole band.
    x = sine(1000.0, 0.5, amp=0.4) + sine(5000.0, 0.5, amp=0.4)
    op = libsonare.SpectralRegionOp(start_sample=0, low_hz=4000.0, high_hz=6000.0, mode="mute")
    out = np.asarray(libsonare.spectral_edit(x, SR, [op]), dtype=np.float32)
    assert len(out) == len(x)
    high_before = _band_energy(x, 4000.0, 6000.0)
    high_after = _band_energy(out, 4000.0, 6000.0)
    assert high_after < high_before * 0.05


_HEAL_OP = libsonare.SpectralRegionOp(
    start_sample=2000, end_sample=8000, low_hz=300.0, high_hz=600.0, gain_db=-24.0, mode="heal"
)


@pytest.mark.parametrize(
    ("field", "library_default"),
    [("n_fft", 2048), ("hop_length", 512), ("heal_radius_frames", 2)],
)
def test_an_omitted_framing_value_is_the_library_default(field: str, library_default: int) -> None:
    """Leaving a value out hands the core its own default, which equals the explicit one."""
    x = sine(440.0, 0.5, amp=0.4)
    omitted = libsonare.spectral_edit(x, SR, [_HEAL_OP])
    explicit = libsonare.spectral_edit(x, SR, [_HEAL_OP], **{field: library_default})
    np.testing.assert_array_equal(omitted, explicit)


@pytest.mark.parametrize("field", ["n_fft", "hop_length", "heal_radius_frames"])
def test_an_explicit_zero_is_refused_rather_than_read_as_the_default(field: str) -> None:
    """0 is the C ABI's spelling of the default; absence is None."""
    x = sine(440.0, 0.5, amp=0.4)
    with pytest.raises(SonareValueError, match=field):
        libsonare.spectral_edit(x, SR, [], **{field: 0})
    with raises_coded():
        libsonare.spectral_edit(x, SR, [], **{field: -1})


def test_invalid_parameters_raise() -> None:
    x = sine(440.0, 0.25, amp=0.4)
    with raises_coded():
        libsonare.spectral_edit(x, SR, [], n_fft=2000)  # not a power of two
    with pytest.raises(ValueError, match=r"n_fft must be a power of two in \[4, 262144\]"):
        libsonare.spectral_edit(x, SR, [], n_fft=2, hop_length=1, window="rectangular")
    with raises_coded():
        libsonare.spectral_edit(x, SR, [], n_fft=2048, hop_length=2048)  # hop > n_fft/2
    with raises_coded():
        op = libsonare.SpectralRegionOp(0, len(x), 0.0, 0.0, mode="nonsense")  # type: ignore[arg-type]
        libsonare.spectral_edit(x, SR, [op])


def test_the_result_is_a_float32_array() -> None:
    out = libsonare.spectral_edit(sine(440.0, 0.25, amp=0.4), SR, [])
    assert isinstance(out, np.ndarray) and out.dtype == np.float32


def test_every_window_token_reaches_the_core() -> None:
    x = sine(440.0, 0.25, amp=0.4)
    op = libsonare.SpectralRegionOp(low_hz=300.0, high_hz=600.0, gain_db=-24.0)
    outs = {w: libsonare.spectral_edit(x, SR, [op], window=w) for w in get_args(WindowType)}
    assert not np.allclose(outs["hann"], outs["rectangular"])


@pytest.mark.parametrize("mode", get_args(SpectralEditMode))
def test_every_mode_token_runs(mode: str) -> None:
    x = sine(440.0, 0.25, amp=0.4)
    op = libsonare.SpectralRegionOp(low_hz=300.0, high_hz=600.0, gain_db=-12.0, mode=mode)  # type: ignore[arg-type]
    assert np.all(np.isfinite(libsonare.spectral_edit(x, SR, [op])))


@pytest.mark.parametrize(
    ("value", "error"), [("Hann", SonareValueError), ("rect", SonareValueError), (0, TypeError)]
)
def test_a_window_outside_the_tokens_is_refused(value: object, error: type) -> None:
    with pytest.raises(error):
        libsonare.spectral_edit(sine(440.0, 0.1), SR, [], window=value)  # type: ignore[arg-type]


@pytest.mark.parametrize(("value", "error"), [("Mute", SonareValueError), (2, TypeError)])
def test_a_mode_outside_the_tokens_is_refused(value: object, error: type) -> None:
    op = libsonare.SpectralRegionOp(low_hz=300.0, high_hz=600.0, mode=value)  # type: ignore[arg-type]
    with pytest.raises(error, match="mode"):
        libsonare.spectral_edit(sine(440.0, 0.1), SR, [op])
