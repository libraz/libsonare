"""Tests for the multi-scale spectral term.

`mss_distance` is reached only when a render has no scorable harmonic ladder,
which is every noise voice and no pitched one, so it went a long time without
being run at all: its octave weighting is a per-bin vector and the normalising
scale averaged it against a whole (frames, bins) STFT, which numpy refuses
rather than broadcasts. These cover the shape and the two values that pin the
term down.
"""

from __future__ import annotations

import numpy as np
import pytest

from loss import MSS_FFT_SIZES, mss_distance


def _noise(n: int, seed: int) -> np.ndarray:
    return np.random.default_rng(seed).standard_normal(n) * 0.1


def test_multi_frame_input_is_scored_rather_than_refused():
    """The STFT is two-dimensional; the octave weights are one bin long."""
    n = MSS_FFT_SIZES[-1] * 8
    value = mss_distance(_noise(n, 1), _noise(n, 2))
    assert np.isfinite(value)
    assert value > 0.0


def test_a_signal_against_itself_scores_zero():
    n = MSS_FFT_SIZES[-1] * 8
    x = _noise(n, 3)
    assert mss_distance(x, x) == pytest.approx(0.0, abs=1e-12)


def test_a_louder_copy_scores_above_an_identical_one():
    """The linear half is normalised by the oracle's own weighted magnitude, so
    a gain difference has to survive that normalisation to be visible."""
    n = MSS_FFT_SIZES[-1] * 8
    x = _noise(n, 4)
    assert mss_distance(x * 2.0, x) > mss_distance(x, x)


def test_a_render_shorter_than_the_largest_window_is_not_scored():
    short = _noise(MSS_FFT_SIZES[-1] // 2, 5)
    assert mss_distance(short, short) == 0.0
