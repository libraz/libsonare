"""Tests for the noise-band grid the denoise floor is reported on.

The grid exists to separate two things a ``band_floor_dbfs`` entry cannot tell
apart on its own: a band that measured the floor sentinel because the region was
quiet, and one that reads the sentinel because the geometric band edges rounded
to the same bin and NO bin landed in it. So the load-bearing test here is not
that the numbers are non-decreasing -- it is that the grid actually explains the
sentinels a real measurement produces, checked with both an empty and a
non-empty band present so the comparison has two sides to decide between.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000
N_FFT = 1024
BAND_COUNT = 32
EDGE_COUNT = BAND_COUNT + 1
FLOOR_SENTINEL_DBFS = -120.0

# The grid at SR / N_FFT: the lowest bands are narrower than the 46.9 Hz bin
# spacing, so six of them round shut.
EMPTY_BANDS = 6
FILLED_BANDS = BAND_COUNT - EMPTY_BANDS


def _noise(sigma: float = 0.01, length: int = SR // 2, seed: int = 7) -> NDArray[np.float32]:
    return np.random.default_rng(seed).normal(0.0, sigma, length).astype(np.float32)


class TestNoiseBandBins:
    def test_explains_which_band_floors_are_sentinels(self) -> None:
        """The reason this entry exists, measured against a real floor."""
        bins = libsonare.mastering_repair_noise_band_bins(n_fft=N_FFT, sample_rate=SR)
        floor = libsonare.mastering_repair_detect_noise_floor(_noise(), SR, n_fft=N_FFT)

        empty = [k for k in range(BAND_COUNT) if bins[k] == bins[k + 1]]
        filled = [k for k in range(BAND_COUNT) if bins[k] < bins[k + 1]]
        # With either side empty the comparison below would decide nothing.
        assert len(empty) == EMPTY_BANDS
        assert len(filled) == FILLED_BANDS

        for k in empty:
            assert floor.band_floor_dbfs[k] == FLOOR_SENTINEL_DBFS
        for k in filled:
            assert floor.band_floor_dbfs[k] > FLOOR_SENTINEL_DBFS

    def test_is_a_non_decreasing_cover_of_the_one_sided_spectrum(self) -> None:
        bins = libsonare.mastering_repair_noise_band_bins(n_fft=N_FFT, sample_rate=SR)

        assert isinstance(bins, list)
        assert len(bins) == EDGE_COUNT
        assert all(isinstance(edge, int) for edge in bins)
        assert bins == sorted(bins)
        assert bins[0] == 0
        assert bins[-1] == N_FFT // 2 + 1

    def test_follows_both_halves_of_the_geometry(self) -> None:
        """A grid that ignored either argument would pass every check above."""
        base = libsonare.mastering_repair_noise_band_bins(n_fft=N_FFT, sample_rate=SR)
        wider = libsonare.mastering_repair_noise_band_bins(n_fft=2 * N_FFT, sample_rate=SR)
        slower = libsonare.mastering_repair_noise_band_bins(n_fft=N_FFT, sample_rate=SR // 2)

        assert wider != base
        assert wider[-1] == N_FFT + 1
        # Same bin count, half the Hz per bin: only the interior can move.
        assert slower != base
        assert slower[-1] == base[-1]

    def test_defaults_to_the_geometry_the_sibling_facades_default_to(self) -> None:
        assert libsonare.mastering_repair_noise_band_bins() == (
            libsonare.mastering_repair_noise_band_bins(n_fft=1024, sample_rate=22050)
        )

    def test_refuses_a_non_power_of_two_window_by_name(self) -> None:
        with pytest.raises(libsonare.SonareValueError, match="n_fft"):
            libsonare.mastering_repair_noise_band_bins(n_fft=1000, sample_rate=SR)

    def test_refuses_a_zero_window_by_name(self) -> None:
        with pytest.raises(libsonare.SonareValueError, match="n_fft"):
            libsonare.mastering_repair_noise_band_bins(n_fft=0, sample_rate=SR)

    def test_refuses_a_non_positive_sample_rate(self) -> None:
        """The C layer's own refusal; nothing on this side screens the rate."""
        with pytest.raises(libsonare.SonareError) as excinfo:
            libsonare.mastering_repair_noise_band_bins(n_fft=N_FFT, sample_rate=0)

        assert not isinstance(excinfo.value, libsonare.SonareValueError)
