"""Tests for the stereo offline dehum Python wrapper.

``mastering_repair_dehum_stereo`` is a ctypes pass-through over
``mastering::repair::dehum_stereo``. Unlike the stereo decrackle wrapper, the
stereo rule here is conditional on ``adaptive``: with it clear (the default)
each channel runs the fixed cascade at the configured frequency, entirely
independently. With it set, the tracker reads the channel mean once and both
cascades follow that one frequency, so ``applied_fundamental_hz`` and
``fundamental_drift_hz`` come out identical in both reports even when the two
channels' own ``detected`` measurements -- always taken from that channel's own
input, whatever ``adaptive`` says -- differ. A test exercising only the default
config never witnesses the sharing rule at all, so the fixtures below give the
two channels genuinely different hum frequencies and cover both states.
"""

from __future__ import annotations

from collections.abc import Sequence

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000

# Length of SonareHumDetection.harmonic_dbfs -- SONARE_DEHUM_MAX_HARMONICS in
# sonare_c_mastering.h.
_MAX_HARMONICS = 16


def _plant_hum(
    bed: NDArray[np.float32], sr: int, fundamental_hz: float, levels_db: Sequence[float]
) -> NDArray[np.float32]:
    """Adds a harmonic series at ``fundamental_hz`` to ``bed`` at the given dB levels."""
    t = np.arange(bed.shape[0], dtype=np.float64) / sr
    out = bed.astype(np.float64).copy()
    for k, level_db in enumerate(levels_db, start=1):
        amp = 10.0 ** (level_db / 20.0)
        out += amp * np.sin(2.0 * np.pi * fundamental_hz * k * t + 0.23 * k)
    return out.astype(np.float32)


_HUM_LEVELS_DB = (-26.0, -32.0, -38.0)

# The two channels carry the mains fundamental at different frequencies, both
# inside the default 2 Hz search window around the configured 50 Hz -- content
# a "track one channel and apply to both" implementation and a "track each
# channel apart" implementation disagree about.
_LEFT_HUM_HZ = 49.4
_RIGHT_HUM_HZ = 50.6


def _hum_fixture() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    left = _plant_hum(sine(440.0, 1.0, sr=SR, amp=0.3), SR, _LEFT_HUM_HZ, _HUM_LEVELS_DB)
    right = _plant_hum(sine(880.0, 1.0, sr=SR, amp=0.3), SR, _RIGHT_HUM_HZ, _HUM_LEVELS_DB)
    return left, right


class TestMasteringRepairDehumStereo:
    def test_default_options_return_matching_shapes(self) -> None:
        left, right = _hum_fixture()
        result = libsonare.mastering_repair_dehum_stereo(left, right, SR)

        assert isinstance(result.left, list)
        assert isinstance(result.right, list)
        assert len(result.left) == len(left)
        assert len(result.right) == len(right)
        assert result.length == len(left)
        assert np.isfinite(result.left).all()
        assert np.isfinite(result.right).all()

        # Not the same data -- the two channels carry different tones and
        # different hum, and the notch filtering does not erase that.
        assert result.left != result.right

    def test_full_harmonic_array_past_nyquist_reads_the_floor(self) -> None:
        """The whole 16-entry array, not just its first slot.

        A ctypes mirror that dropped or misordered ``harmonic_dbfs`` would
        still pass a test asserting only entry 0. At 48 kHz a 3 kHz
        fundamental with all 16 harmonics requested reaches Nyquist at the
        eighth: the cascade notches the first seven regardless of what either
        channel carries, and nothing is there to measure past Nyquist, so
        those slots read the dB floor exactly rather than an unset zero.
        """
        left = sine(440.0, 1.0, sr=SR, amp=0.3)
        right = sine(660.0, 1.0, sr=SR, amp=0.3)
        result = libsonare.mastering_repair_dehum_stereo(
            left, right, SR, fundamental_hz=3000.0, harmonics=_MAX_HARMONICS
        )

        for report in (result.left_report, result.right_report):
            assert report.notched_harmonics == 7
            assert len(report.detected.harmonic_dbfs) == _MAX_HARMONICS
            for k in range(7, _MAX_HARMONICS):
                assert report.detected.harmonic_dbfs[k] == -120.0

    def test_fixed_mode_is_independent_and_drift_is_exactly_zero(self) -> None:
        """Without adaptive tracking, nothing is shared between the channels.

        Each cascade notches at the configured frequency regardless of what
        either channel's own hum sits at, so ``applied_fundamental_hz`` equals
        the configured value on both sides and ``fundamental_drift_hz`` is
        exactly zero -- the measurement without tracking, not an unset field.
        """
        left, right = _hum_fixture()
        result = libsonare.mastering_repair_dehum_stereo(
            left, right, SR, fundamental_hz=50.0, harmonics=3, adaptive=False
        )

        assert result.left_report.applied_fundamental_hz == 50.0
        assert result.right_report.applied_fundamental_hz == 50.0
        assert result.left_report.fundamental_drift_hz == 0.0
        assert result.right_report.fundamental_drift_hz == 0.0

        # `detected` is always measured from each channel's own input, so the
        # two channels' own hum frequencies still show up here even though
        # the fixed cascade never looked at them.
        assert (
            result.left_report.detected.fundamental_hz
            != result.right_report.detected.fundamental_hz
        )

    def test_adaptive_shares_the_tracked_fundamental_while_detection_stays_per_channel(
        self,
    ) -> None:
        """The rule this entry point exists for.

        With ``adaptive`` set, the tracker reads the channel mean once and
        both cascades follow that one frequency: ``applied_fundamental_hz``
        and ``fundamental_drift_hz`` come out identical in both reports by
        construction, even though each report's own ``detected`` measurement
        -- taken from that channel's own input -- still differs, since the two
        channels carry the mains tone at different frequencies. A default-only
        test cannot witness this: it never turns tracking on.
        """
        left, right = _hum_fixture()
        result = libsonare.mastering_repair_dehum_stereo(
            left,
            right,
            SR,
            fundamental_hz=50.0,
            harmonics=3,
            adaptive=True,
            search_range_hz=2.0,
        )

        # The asymmetry: each channel's own analysis differs...
        assert (
            result.left_report.detected.fundamental_hz
            != result.right_report.detected.fundamental_hz
        )
        # ...but what the shared tracker applied is exactly the same value.
        assert (
            result.left_report.applied_fundamental_hz == result.right_report.applied_fundamental_hz
        )

        # Tracking moved the frequency, clamped to the search window.
        assert result.left_report.fundamental_drift_hz > 0.0
        assert result.left_report.fundamental_drift_hz <= 2.0
        assert result.right_report.fundamental_drift_hz == result.left_report.fundamental_drift_hz

    def test_explicit_kwargs(self) -> None:
        left, right = _hum_fixture()
        result = libsonare.mastering_repair_dehum_stereo(
            left,
            right,
            SR,
            fundamental_hz=50.0,
            harmonics=3,
            q=15.0,
            adaptive=True,
            search_range_hz=2.0,
            adaptation=0.3,
            frame_size=1024,
            pll_bandwidth=0.02,
        )
        assert result.length == len(left)

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _hum_fixture()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_dehum_stereo(left, right[:-10], SR)

    def test_rejects_empty_input(self) -> None:
        empty = np.zeros(0, dtype=np.float32)
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_dehum_stereo(empty, empty, SR)

    def test_native_refusal_leaves_no_stale_allocation(self) -> None:
        """A refusal the Python layer does not pre-validate must not leak state.

        ``sample_rate`` is only validated natively, not by this binding, so
        this reaches the C call and must raise rather than return a result. A
        subsequent valid call on the same process must still succeed cleanly:
        the out-struct is cleared before the argument checks, so the refused
        call leaves NULL buffers and zeroed reports rather than anything to
        free twice or otherwise corrupt the next one.
        """
        left, right = _hum_fixture()
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_dehum_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_dehum_stereo(left, right, SR)
        assert result.length == len(left)
