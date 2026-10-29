"""Parity-sensitive checks for the real-spectrum resampler."""

from __future__ import annotations

import numpy as np
import pytest
import sofa_to_shrf as s2s


@pytest.mark.parametrize(
    ("source_length", "target_length"),
    [(4, 8), (4, 7), (5, 8), (5, 7), (8, 4), (7, 4), (8, 5), (7, 5)],
)
def test_resample_preserves_dc_and_sub_nyquist_tones(
    source_length: int, target_length: int
) -> None:
    shared_max_frequency = min((source_length - 1) // 2, (target_length - 1) // 2)
    frequency = max(1, shared_max_frequency)
    source_phase = 2.0 * np.pi * frequency * np.arange(source_length) / source_length
    source = 2.5 + np.cos(source_phase) + 0.25 * np.sin(source_phase)
    original = source.copy()

    resampled = s2s._fft_resample(source, target_length)

    target_phase = 2.0 * np.pi * frequency * np.arange(target_length) / target_length
    expected = 2.5 + np.cos(target_phase) + 0.25 * np.sin(target_phase)
    np.testing.assert_allclose(resampled, expected, rtol=1e-12, atol=1e-12)
    np.testing.assert_array_equal(source, original)


@pytest.mark.parametrize(("source_length", "target_length"), [(4, 8), (4, 7)])
def test_upsampling_splits_even_input_nyquist_without_gain(
    source_length: int, target_length: int
) -> None:
    source = (-1.0) ** np.arange(source_length)
    expected = np.cos(2.0 * np.pi * (source_length // 2) * np.arange(target_length) / target_length)

    resampled = s2s._fft_resample(source, target_length)

    np.testing.assert_allclose(resampled, expected, rtol=1e-12, atol=1e-12)


@pytest.mark.parametrize(("source_length", "target_length"), [(8, 4), (7, 4)])
def test_downsampling_to_even_output_merges_nyquist_pair_without_loss(
    source_length: int, target_length: int
) -> None:
    frequency = target_length // 2
    source_phase = 2.0 * np.pi * frequency * np.arange(source_length) / source_length
    source = np.cos(source_phase)
    expected = (-1.0) ** np.arange(target_length)

    resampled = s2s._fft_resample(source, target_length)

    np.testing.assert_allclose(resampled, expected, rtol=1e-12, atol=1e-12)


def test_same_length_resampling_preserves_samples_and_leaves_input_unchanged() -> None:
    source = np.array([0.0, -0.0, 1.25, -2.0], dtype=np.float64)
    original = source.copy()

    resampled = s2s._fft_resample(source, len(source))

    np.testing.assert_array_equal(resampled, original)
    np.testing.assert_array_equal(source, original)
    assert resampled is not source
