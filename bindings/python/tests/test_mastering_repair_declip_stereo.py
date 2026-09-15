"""Tests for the stereo offline declip Python wrapper.

``mastering_repair_declip_stereo`` is a ctypes pass-through over
``mastering::repair::declip_stereo``, and its linking rule is NOT the
declicker's: a channel only reconstructs a union run when it has at least one
clipped sample of its own inside it, so a clipped plateau present in only one
channel produces no linking at all. The fixture below therefore clips BOTH
channels over the same region but with different extents -- the only shape
that can produce linking here -- plus a separate right-only clipped run that
must leave the left channel untouched.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# Both channels clip over [2000, 2010), and only the right channel keeps
# clipping through [2010, 2040): the union run is [2000, 2040), which left has
# at least one clipped sample in (so it is repaired there too, past its own
# clipped extent) while right's own detection already spans the whole run.
_SHARED_CLIP_START = 2000
_LEFT_CLIP_END = 2010
_RIGHT_CLIP_END = 2040

# A run clipped in the right channel only, far from the shared region, with
# nothing clipped in the left channel anywhere nearby.
_RIGHT_ONLY_CLIP_START = 5000
_RIGHT_ONLY_CLIP_END = 5010


def _fixture() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    left = sine(440.0, 0.3, sr=SR, amp=0.5).copy()
    right = sine(880.0, 0.3, sr=SR, amp=0.5).copy()
    left[_SHARED_CLIP_START:_LEFT_CLIP_END] = 1.0
    right[_SHARED_CLIP_START:_RIGHT_CLIP_END] = 1.0
    right[_RIGHT_ONLY_CLIP_START:_RIGHT_ONLY_CLIP_END] = 1.0
    return left, right


class TestMasteringRepairDeclipStereo:
    def test_default_options_return_matching_shapes(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_declip_stereo(left, right, SR)

        assert isinstance(result.left, list)
        assert isinstance(result.right, list)
        assert len(result.left) == len(left)
        assert len(result.right) == len(right)
        assert result.length == len(left)
        assert np.isfinite(result.left).all()
        assert np.isfinite(result.right).all()

        # Not the same data -- the control a "declip one channel and return
        # it twice" implementation cannot pass, since the input channels
        # carry different tones.
        assert result.left != result.right

    def test_wider_run_links_the_narrower_channel(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_declip_stereo(left, right, SR)

        # Non-vacuity: each channel's own detector must have found its own
        # clipped samples before the cross-channel assertions below mean
        # anything. Right has two runs (shared + right-only); left has one.
        assert result.left_report.detected.sample_count > 0
        assert result.left_report.detected.run_count >= 1
        assert result.right_report.detected.sample_count > 0
        assert result.right_report.detected.run_count >= 2

        # The narrower channel's reconstruction reaches past its own clipped
        # samples because the wider run on the other side pulled it along.
        assert result.left_report.linked_runs > 0

        # The right channel's own detected extent already spans the union
        # run it takes part in, so nothing there reaches past its own
        # detection -- this is the asymmetry a per-channel implementation
        # would erase (it would report 0 on both sides).
        assert result.right_report.linked_runs == 0

    def test_right_only_clip_leaves_left_untouched(self) -> None:
        """A clipped plateau present in only one channel produces no linking.

        The declicker repairs a run either channel selects in BOTH channels;
        the declipper does not -- a channel with no clipped sample of its own
        in a run is left exactly as it came in.
        """
        left, right = _fixture()
        result = libsonare.mastering_repair_declip_stereo(left, right, SR)

        left_slice = result.left[_RIGHT_ONLY_CLIP_START:_RIGHT_ONLY_CLIP_END]
        original_slice = left[_RIGHT_ONLY_CLIP_START:_RIGHT_ONLY_CLIP_END].tolist()
        assert left_slice == pytest.approx(original_slice)

    def test_linked_region_differs_from_mono(self) -> None:
        """The one assertion a mono-called-twice implementation cannot pass.

        The mono declipper never sees the right channel's clipped samples, so
        it leaves the left channel's [2010, 2040) span untouched (it is not
        clipped there). The stereo entry point reconstructs that span in the
        left channel too, because of the linked run, so the two must
        disagree there.
        """
        left, right = _fixture()
        stereo = libsonare.mastering_repair_declip_stereo(left, right, SR)
        mono_left = libsonare.mastering_repair_declip(left, SR)

        stereo_slice = stereo.left[_LEFT_CLIP_END:_RIGHT_CLIP_END]
        mono_slice = mono_left[_LEFT_CLIP_END:_RIGHT_CLIP_END].tolist()
        assert stereo_slice != pytest.approx(mono_slice)

    def test_explicit_kwargs(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_declip_stereo(
            left,
            right,
            SR,
            clip_threshold=0.85,
            lpc_order=24,
            iterations=1,
            lpc_blend=0.5,
        )
        assert result.length == len(left)

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _fixture()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_declip_stereo(left, right[:-10], SR)

    def test_native_refusal_leaves_no_stale_allocation(self) -> None:
        """A refusal the Python layer does not pre-validate must not leak state.

        ``sample_rate`` is only validated natively, not by this binding, so
        this reaches the C call and must raise rather than return a result. A
        subsequent valid call on the same process must still succeed cleanly:
        nothing from the failed call is left to free twice or otherwise
        corrupt the next one. The out-struct is cleared before the argument
        checks, so the refused call leaves NULL buffers and zeroed reports.
        """
        left, right = _fixture()
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_declip_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_declip_stereo(left, right, SR)
        assert result.length == len(left)
