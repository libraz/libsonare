"""Empty / NaN-Inf input guards for metering / dynamics / voice-changer wrappers.

Source-side helpers (`_validate_samples` in `_runtime.py`) raise typed
`ValueError`s with `{fn_name}: {arg_name}` prefixes so callers can pattern-match
on the wrapper rather than on a generic C error string. These tests pin the
behavior across all in-scope public wrappers.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import (
    lufs,
    mastering_dynamics_compressor,
    mastering_dynamics_gate,
    mastering_dynamics_transient_shaper,
    metering_peak_db,
    metering_rms_db,
    metering_stereo_correlation,
    metering_true_peak_db,
)
from libsonare._effects import voice_change, voice_change_realtime

SR = 22050


def _sine(n: int = 1024, freq: float = 440.0, amp: float = 0.5) -> np.ndarray:
    t = np.arange(n, dtype=np.float32) / SR
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _with_nan(n: int = 1024, index: int = 100) -> np.ndarray:
    buf = _sine(n)
    buf[index] = math.nan
    return buf


def _with_inf(n: int = 1024, index: int = 200) -> np.ndarray:
    buf = _sine(n)
    buf[index] = math.inf
    return buf


class TestEmptyGuards:
    def test_lufs_rejects_empty(self):
        with pytest.raises(ValueError, match=r"lufs: samples must not be empty"):
            lufs(np.empty(0, dtype=np.float32), SR)

    def test_metering_peak_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_peak_db: samples must not be empty"):
            metering_peak_db(np.empty(0, dtype=np.float32))

    def test_metering_rms_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_rms_db: samples must not be empty"):
            metering_rms_db(np.empty(0, dtype=np.float32))

    def test_metering_true_peak_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_true_peak_db: samples must not be empty"):
            metering_true_peak_db(np.empty(0, dtype=np.float32), SR)

    def test_metering_stereo_correlation_rejects_empty_left(self):
        with pytest.raises(
            ValueError,
            match=r"metering_stereo_correlation: left must not be empty",
        ):
            metering_stereo_correlation(np.empty(0, dtype=np.float32), _sine())

    def test_mastering_dynamics_compressor_rejects_empty(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_compressor: samples must not be empty",
        ):
            mastering_dynamics_compressor(np.empty(0, dtype=np.float32), SR)

    def test_mastering_dynamics_gate_rejects_empty(self):
        with pytest.raises(ValueError, match=r"mastering_dynamics_gate: samples must not be empty"):
            mastering_dynamics_gate(np.empty(0, dtype=np.float32), SR)

    def test_mastering_dynamics_transient_shaper_rejects_empty(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_transient_shaper: samples must not be empty",
        ):
            mastering_dynamics_transient_shaper(np.empty(0, dtype=np.float32), SR)

    def test_voice_change_rejects_empty(self):
        with pytest.raises(ValueError, match=r"voice_change: samples must not be empty"):
            voice_change(np.empty(0, dtype=np.float32))

    def test_voice_change_realtime_rejects_empty(self):
        with pytest.raises(ValueError, match=r"voice_change_realtime: samples must not be empty"):
            voice_change_realtime(np.empty(0, dtype=np.float32))


class TestNanInfGuards:
    def test_lufs_rejects_nan_with_index(self):
        with pytest.raises(ValueError, match=r"lufs: samples contains NaN or Inf at index 100"):
            lufs(_with_nan(), SR)

    def test_lufs_rejects_inf_with_index(self):
        with pytest.raises(ValueError, match=r"lufs: samples contains NaN or Inf at index 200"):
            lufs(_with_inf(), SR)

    def test_mastering_dynamics_compressor_rejects_nan(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_compressor: samples contains NaN or Inf at index 100",
        ):
            mastering_dynamics_compressor(_with_nan(), SR)

    def test_mastering_dynamics_gate_rejects_inf(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_gate: samples contains NaN or Inf at index 200",
        ):
            mastering_dynamics_gate(_with_inf(), SR)

    def test_metering_peak_db_rejects_nan(self):
        with pytest.raises(
            ValueError, match=r"metering_peak_db: samples contains NaN or Inf at index 100"
        ):
            metering_peak_db(_with_nan())


class TestValidateFalseSkipsNan:
    def test_lufs_validate_false(self):
        # Should NOT raise the Python-side NaN/Inf guard error. The C layer
        # may still reject NaN input with its own SonareException (surfaced as
        # RuntimeError), which is acceptable — we only assert that the
        # Python-side guard message did not fire.
        try:
            lufs(_with_nan(), SR, validate=False)
        except (ValueError, RuntimeError) as e:
            assert "contains NaN or Inf" not in str(e)

    def test_mastering_dynamics_compressor_validate_false(self):
        try:
            mastering_dynamics_compressor(_with_nan(), SR, validate=False)
        except (ValueError, RuntimeError) as e:
            assert "contains NaN or Inf" not in str(e)


class TestPositiveSmoke:
    def test_metering_rms_db_finite_on_sine(self):
        v = metering_rms_db(_sine(SR))
        assert math.isfinite(v)

    def test_mastering_dynamics_compressor_returns_same_length(self):
        sig = _sine(SR)
        out, latency = mastering_dynamics_compressor(sig, SR)
        assert len(out) == len(sig)
        assert isinstance(latency, int)
