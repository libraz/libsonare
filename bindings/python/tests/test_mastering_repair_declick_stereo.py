"""Tests for the stereo offline declick Python wrapper.

``mastering_repair_declick_stereo`` is a ctypes pass-through over
``mastering::repair::declick_stereo``. Its defining behaviour over calling the
mono declicker twice is that a click either channel's detector selects is
repaired in BOTH channels, so the suite is built around fixtures where the two
channels carry different tones and their injected clicks sit at disjoint
sample positions -- a per-channel implementation would pass every assertion
that does not specifically probe the cross-channel linking.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# Left and right carry different tones so a per-channel implementation cannot
# satisfy the "channels are not identical" control by construction, and the
# injected clicks sit far enough apart (well past max_click_samples=8) that
# each is an independent run in a clean context on the OTHER channel.
_LEFT_CLICK_POSITIONS = (1000, 3000, 5000)
_RIGHT_ONLY_CLICK_POSITION = 2000


def _fixture() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    # amp 0.2, not 0.3: the detector wants a run to stand neighbor_ratio (4.0)
    # above its neighbours, so a 1.0 spike landing near a tone's own peak clears
    # the ratio at 0.2 and misses it at 0.3, making detection depend on the phase
    # the click happens to land on.
    left = sine(440.0, 0.3, sr=SR, amp=0.2).copy()
    right = sine(880.0, 0.3, sr=SR, amp=0.2).copy()
    for pos in _LEFT_CLICK_POSITIONS:
        left[pos] = 1.0
    right[_RIGHT_ONLY_CLICK_POSITION] = 1.0
    return left, right


class TestMasteringRepairDeclickStereo:
    def test_default_options_return_matching_shapes(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_declick_stereo(left, right, SR)

        assert isinstance(result.left, list)
        assert isinstance(result.right, list)
        assert len(result.left) == len(left)
        assert len(result.right) == len(right)
        assert result.length == len(left)
        assert np.isfinite(result.left).all()
        assert np.isfinite(result.right).all()

        # Not the same data -- the control a "declick one channel and return
        # it twice" implementation cannot pass, since the input channels
        # carry different tones.
        assert result.left != result.right

    def test_shared_run_is_repaired_in_both_channels(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_declick_stereo(left, right, SR)

        # Non-vacuity: each channel's own detector must have found its own
        # injected clicks before the cross-channel assertions below mean
        # anything.
        assert result.left_report.detected.count >= 1
        assert result.right_report.detected.count >= 1

        # The union-of-both-channels selection: a run only the OTHER channel
        # flagged is still repaired here, and reported as linked rather than
        # self-detected. A per-channel implementation reports linked_runs == 0
        # on both sides and repaired_runs == detected.count.
        assert result.left_report.linked_runs > 0
        assert result.right_report.linked_runs > 0
        assert result.left_report.repaired_runs > result.left_report.detected.count
        assert result.right_report.repaired_runs > result.right_report.detected.count

    def test_right_only_click_changes_left_channel_vs_mono(self) -> None:
        """The one assertion a mono-called-twice implementation cannot pass.

        The mono declicker never sees the right channel's click, so it leaves
        the left channel's samples at that position untouched. The stereo
        entry point repairs that position in the left channel too (a linked
        run), so the two must disagree exactly there.
        """
        left, right = _fixture()
        stereo = libsonare.mastering_repair_declick_stereo(left, right, SR)
        mono_left = libsonare.mastering_repair_declick(left, SR)

        assert stereo.left[_RIGHT_ONLY_CLICK_POSITION] != float(
            mono_left[_RIGHT_ONLY_CLICK_POSITION]
        )

    def test_explicit_kwargs(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_declick_stereo(
            left,
            right,
            SR,
            threshold=0.7,
            neighbor_ratio=4.0,
            max_click_samples=8,
            lpc_order=16,
            residual_ratio=6.0,
        )
        assert result.length == len(left)

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _fixture()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_declick_stereo(left, right[:-10], SR)

    def test_rejects_non_positive_max_click_samples(self) -> None:
        left, right = _fixture()
        with pytest.raises(ValueError, match="max_click_samples"):
            libsonare.mastering_repair_declick_stereo(left, right, SR, max_click_samples=0)

    def test_native_refusal_leaves_no_stale_allocation(self) -> None:
        """A refusal the Python layer does not pre-validate must not leak state.

        ``sample_rate`` is only validated natively, not by this binding, so
        this reaches the C call and must raise rather than return a result. A
        subsequent valid call on the same process must still succeed cleanly,
        which is the observable half of "the out-parameter starts (and, on a
        refusal, stays) a defined empty result rather than carrying over
        stale contents": nothing from the failed call is left to free twice
        or otherwise corrupt the next one.
        """
        left, right = _fixture()
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_declick_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_declick_stereo(left, right, SR)
        assert result.length == len(left)
