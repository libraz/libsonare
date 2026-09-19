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


def test_a_louder_copy_scores_the_same_as_an_identical_one():
    """Level is not this term's business, and it used to be half of it.

    Both sides arrive scaled to a common RMS over the whole timeline, which
    equalises total energy rather than level: a model that decays faster than
    its reference holds less of it and is lifted by exactly that ratio. Every
    other term is a ratio against its own reference bin and cancels the lift;
    this one charged for it — 0.80 at 1.5x and 4.06 at 4x on renders that
    differed in nothing else — so the term that exists to see what the metric
    set does not model was partly reporting the normalisation.
    """
    n = MSS_FFT_SIZES[-1] * 8
    x = _noise(n, 4)
    for gain in (1.5, 2.0, 4.0):
        assert mss_distance(x * gain, x) == pytest.approx(0.0, abs=1e-9)


def test_a_spectral_difference_survives_the_gain_it_is_measured_through():
    """Taking the level out must not take the shape out with it."""
    n = MSS_FFT_SIZES[-1] * 8
    dull = np.cumsum(_noise(n, 6))          # -6 dB/octave against the source
    bright = _noise(n, 6)
    assert mss_distance(dull, bright) > 0.5
    assert mss_distance(dull * 4.0, bright) == pytest.approx(
        mss_distance(dull, bright), abs=1e-9)


def test_a_render_shorter_than_the_largest_window_is_not_scored():
    short = _noise(MSS_FFT_SIZES[-1] // 2, 5)
    assert mss_distance(short, short) == 0.0
