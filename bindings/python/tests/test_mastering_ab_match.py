"""Tests for the A/B loudness-match Python wrapper.

The two takes below differ in frequency, duration and level, so a call that
returned the reference -- or the source ungained -- fails on shape as well as on
content rather than passing a check that only compares scalars the C struct
reports about itself.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    ErrorCode,
    LoudnessMatchResult,
    SonareValueError,
    lufs,
    mastering_ab_match_loudness,
    mastering_ab_match_loudness_stereo,
)

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000
SOURCE = sine(220.0, 1.6, sr=SR, amp=0.05)
REFERENCE = sine(997.0, 2.3, sr=SR, amp=0.5)


def test_matched_take_measures_at_the_reference_loudness() -> None:
    result = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    assert isinstance(result, LoudnessMatchResult)
    with pytest.raises(TypeError):
        iter(result)
    assert result.sample_rate == SR
    reference_lufs = lufs(REFERENCE, SR).integrated_lufs
    assert lufs(SOURCE, SR).integrated_lufs < reference_lufs - 10.0
    assert lufs(result.samples, SR).integrated_lufs == pytest.approx(reference_lufs, abs=0.05)


def test_matched_take_is_the_source_under_the_reported_gain() -> None:
    result = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    matched = result.samples
    assert matched.dtype == np.float32
    assert matched.shape == SOURCE.shape
    assert matched.shape != REFERENCE.shape
    gain = 10.0 ** (result.applied_gain_db / 20.0)
    np.testing.assert_allclose(matched, SOURCE.astype(np.float64) * gain, rtol=1e-5, atol=1e-7)


def test_reported_peak_is_the_matched_take_not_the_source() -> None:
    result = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    matched = result.samples
    # True peak is measured on an oversampled signal, so it sits at or above the
    # sample peak rather than exactly on it.
    sample_peak_db = 20.0 * np.log10(float(np.max(np.abs(matched))))
    assert result.matched_true_peak_dbtp == pytest.approx(sample_peak_db, abs=0.5)
    assert result.matched_true_peak_dbtp > 20.0 * np.log10(float(np.max(np.abs(SOURCE))))


@pytest.mark.parametrize("silent_role", ["source", "reference"])
def test_a_silent_take_is_refused_naming_it(silent_role: str) -> None:
    silent = np.zeros(SR // 2, dtype=np.float32)
    takes = {"source": SOURCE, "reference": REFERENCE, silent_role: silent}
    with pytest.raises(
        SonareValueError,
        match=rf"^mastering_ab_match_loudness: {silent_role} has no measurable loudness$",
    ):
        mastering_ab_match_loudness(takes["source"], takes["reference"], SR)


@pytest.mark.parametrize("silent_role", ["source", "reference"])
def test_a_silent_stereo_take_is_refused_naming_both_its_channels(silent_role: str) -> None:
    silent = np.zeros(SR // 2, dtype=np.float32)
    takes = {"source": SOURCE, "reference": REFERENCE, silent_role: silent}
    with pytest.raises(
        SonareValueError,
        match=(
            rf"^mastering_ab_match_loudness_stereo: {silent_role}_left and {silent_role}_right "
            r"have no measurable loudness$"
        ),
    ):
        mastering_ab_match_loudness_stereo(
            takes["source"], takes["source"], takes["reference"], takes["reference"], SR
        )


def test_invalid_sample_rate_is_rejected_by_the_c_abi() -> None:
    with pytest.raises(SonareValueError) as excinfo:
        mastering_ab_match_loudness(SOURCE, REFERENCE, 0)
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_empty_take_is_rejected_by_name() -> None:
    with pytest.raises(SonareValueError, match="source"):
        mastering_ab_match_loudness([], REFERENCE, SR)
