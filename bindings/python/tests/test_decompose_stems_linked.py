"""Tests for :func:`libsonare.decompose_stems_linked`.

The referent is :func:`libsonare.decompose_stems`: a single channel must
reproduce it bit for bit (division by one changes nothing), and the shared
mask must reconstruct every channel the same way the mono mask reconstructs
one.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library unavailable")

SR = 22050


def _tone(freq: float = 440.0, seconds: float = 0.4, phase: float = 0.0) -> np.ndarray:
    n = int(SR * seconds)
    t = np.arange(n, dtype=np.float32) / SR
    return (0.5 * np.sin(2.0 * math.pi * freq * t + phase)).astype(np.float32)


def test_one_channel_matches_decompose_stems_bit_for_bit() -> None:
    x = _tone(440.0, 0.3)
    mono = libsonare.decompose_stems(x, SR, n_components=2, n_fft=1024, hop_length=256, n_iter=20)
    linked = libsonare.decompose_stems_linked(
        [x], SR, n_components=2, n_fft=1024, hop_length=256, n_iter=20
    )
    assert len(linked["components"]) == len(mono["components"])
    for mono_component, linked_component in zip(
        mono["components"], linked["components"], strict=True
    ):
        linked_arr = np.asarray(linked_component)
        assert linked_arr.shape == (1, len(x))
        np.testing.assert_array_equal(linked_arr[0], np.asarray(mono_component))
    np.testing.assert_array_equal(np.asarray(linked["w"]), np.asarray(mono["w"]))
    np.testing.assert_array_equal(np.asarray(linked["h"]), np.asarray(mono["h"]))
    assert linked["sample_rate"] == mono["sample_rate"] == SR


def test_two_channels_reconstruct_and_carry_the_channel_axis() -> None:
    left = _tone(440.0, 0.4)
    right = _tone(440.0, 0.4, phase=0.3)
    res = libsonare.decompose_stems_linked(
        [left, right], SR, n_components=2, n_fft=1024, hop_length=256, n_iter=30
    )
    components = res["components"]
    assert len(components) == 2
    for component in components:
        arr = np.asarray(component)
        assert arr.shape == (2, len(left))
    assert np.asarray(res["w"]).shape == (1024 // 2 + 1, 2)
    assert np.asarray(res["h"]).shape[0] == 2

    total_left = sum(np.asarray(c, dtype=np.float32)[0] for c in components)
    total_right = sum(np.asarray(c, dtype=np.float32)[1] for c in components)
    interior = slice(1024, len(left) - 1024)
    left_err = float(np.linalg.norm(total_left[interior] - left[interior]))
    right_err = float(np.linalg.norm(total_right[interior] - right[interior]))
    assert left_err / float(np.linalg.norm(left[interior])) < 0.05
    assert right_err / float(np.linalg.norm(right[interior])) < 0.05


def test_empty_channels_is_rejected() -> None:
    with pytest.raises(ValueError):
        libsonare.decompose_stems_linked([], SR)


def test_mismatched_channel_lengths_are_rejected() -> None:
    left = _tone(440.0, 0.2)
    right = _tone(440.0, 0.3)
    with pytest.raises(ValueError):
        libsonare.decompose_stems_linked([left, right], SR)


def test_non_finite_channel_is_rejected() -> None:
    bad = _tone(440.0, 0.2).copy()
    bad[10] = float("nan")
    with pytest.raises(ValueError):
        libsonare.decompose_stems_linked([bad, _tone(440.0, 0.2)], SR)


def test_rejects_an_out_of_range_mask_power() -> None:
    with pytest.raises(ValueError):
        libsonare.decompose_stems_linked([_tone(440.0, 0.2)], SR, mask_power=0.5)
