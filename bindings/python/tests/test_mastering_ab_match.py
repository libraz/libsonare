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
    LoudnessMatch,
    SonareError,
    SonareValueError,
    lufs,
    mastering_ab_match_loudness,
)

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000
SOURCE = sine(220.0, 1.6, sr=SR, amp=0.05)
REFERENCE = sine(997.0, 2.3, sr=SR, amp=0.5)


def test_matched_take_measures_at_the_reference_loudness() -> None:
    matched, match = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    assert isinstance(match, LoudnessMatch)
    reference_lufs = lufs(REFERENCE, SR).integrated_lufs
    assert lufs(SOURCE, SR).integrated_lufs < reference_lufs - 10.0
    assert lufs(matched, SR).integrated_lufs == pytest.approx(reference_lufs, abs=0.05)


def test_matched_take_is_the_source_under_the_reported_gain() -> None:
    matched, match = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    assert match is not None
    assert matched.dtype == np.float32
    assert matched.shape == SOURCE.shape
    assert matched.shape != REFERENCE.shape
    gain = 10.0 ** (match.applied_gain_db / 20.0)
    np.testing.assert_allclose(matched, SOURCE.astype(np.float64) * gain, rtol=1e-5, atol=1e-7)


def test_reported_peak_is_the_matched_take_not_the_source() -> None:
    matched, match = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    assert match is not None
    # True peak is measured on an oversampled signal, so it sits at or above the
    # sample peak rather than exactly on it.
    sample_peak_db = 20.0 * np.log10(float(np.max(np.abs(matched))))
    assert match.matched_true_peak_dbtp == pytest.approx(sample_peak_db, abs=0.5)
    assert match.matched_true_peak_dbtp > 20.0 * np.log10(float(np.max(np.abs(SOURCE))))


def test_match_omitted_returns_the_same_audio() -> None:
    audio_only, match = mastering_ab_match_loudness(SOURCE, REFERENCE, SR, with_match=False)
    assert match is None
    matched, _ = mastering_ab_match_loudness(SOURCE, REFERENCE, SR)
    np.testing.assert_array_equal(audio_only, matched)


def test_silent_source_reports_non_finite_loudness_and_no_gain() -> None:
    silent = np.zeros(SR // 2, dtype=np.float32)
    matched, match = mastering_ab_match_loudness(silent, REFERENCE, SR)
    assert match is not None
    assert not np.isfinite(match.source_lufs)
    assert match.applied_gain_db == 0.0
    np.testing.assert_array_equal(matched, silent)


def test_invalid_sample_rate_is_rejected_by_the_c_abi() -> None:
    with pytest.raises(SonareError) as excinfo:
        mastering_ab_match_loudness(SOURCE, REFERENCE, 0)
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_empty_take_is_rejected_by_name() -> None:
    with pytest.raises(SonareValueError, match="source"):
        mastering_ab_match_loudness([], REFERENCE, SR)
