"""Tests for the stereo offline decrackle Python wrapper.

``mastering_repair_decrackle_stereo`` is a ctypes pass-through over
``mastering::repair::decrackle_stereo``. Unlike the stereo declicker and
declipper it does NOT link runs across channels: crackle is surface damage
landing at different instants in each channel, so each channel is decrackled
independently through the same, shared, validated config. The fixtures below
therefore put different content in the two channels rather than a shared
event, and there is no ``linked_runs`` field to assert about.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# Left channel: a quiet tone with three widely spaced impulses well clear of
# each other's 3-sample median window, so each is detected on its own.
_LEFT_IMPULSE_INDICES = (1000, 3000, 5000)
_LEFT_IMPULSE_DELTA = 0.6

# Right channel: a different tone, two impulses at different locations and a
# different (negative) delta -- content a "decrackle one channel and return
# it twice" implementation cannot reproduce, and a different detected count
# from the left channel's.
_RIGHT_IMPULSE_INDICES = (1500, 4000)
_RIGHT_IMPULSE_DELTA = -0.5


def _fixture() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    left = sine(440.0, 0.3, sr=SR, amp=0.1).copy()
    right = sine(880.0, 0.3, sr=SR, amp=0.1).copy()
    for i in _LEFT_IMPULSE_INDICES:
        left[i] += _LEFT_IMPULSE_DELTA
    for i in _RIGHT_IMPULSE_INDICES:
        right[i] += _RIGHT_IMPULSE_DELTA
    return left, right


# A wavelet-mode fixture: a small sine plus three impulses inside one 256-sample
# Haar analysis window, mirroring the C++ "Decrackle wavelet shrinkage reduces
# crackle energy" fixture. Left and right carry impulses at different indices
# and signs so the two channels' reports differ.
_WAVELET_LENGTH = 256


def _wavelet_channel(indices_and_deltas: tuple[tuple[int, float], ...]) -> NDArray[np.float32]:
    samples = 0.1 * np.sin(2.0 * np.pi * np.arange(_WAVELET_LENGTH) / 64.0)
    for index, delta in indices_and_deltas:
        samples[index] += delta
    return samples.astype(np.float32)


def _wavelet_fixture() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    left = _wavelet_channel(((32, 0.35), (96, -0.32), (160, 0.30)))
    right = _wavelet_channel(((48, -0.28), (112, 0.33)))
    return left, right


class TestMasteringRepairDecrackleStereo:
    def test_default_options_return_matching_shapes(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_decrackle_stereo(left, right, SR)

        assert isinstance(result.left, list)
        assert isinstance(result.right, list)
        assert len(result.left) == len(left)
        assert len(result.right) == len(right)
        assert result.length == len(left)
        assert np.isfinite(result.left).all()
        assert np.isfinite(result.right).all()

        # Not the same data -- the control a "decrackle one channel and
        # return it twice" implementation cannot pass, since the input
        # channels carry different tones and different impulses.
        assert result.left != result.right

    def test_channels_are_independent_with_no_linking(self) -> None:
        """Non-vacuity, and the fact this repair has no linking at all.

        Each channel's own median criterion must have found exactly its own
        impulses -- asserted exactly, not with a loose inequality, so a
        fixture that stops witnessing detection goes red rather than passing
        by accident. There is no ``linked_runs`` field here: declick and
        declip repair the union of both channels' runs; decrackle does not.
        """
        left, right = _fixture()
        result = libsonare.mastering_repair_decrackle_stereo(left, right, SR)

        assert result.left_report.detected.sample_count == len(_LEFT_IMPULSE_INDICES)
        assert result.right_report.detected.sample_count == len(_RIGHT_IMPULSE_INDICES)

        # Median mode replaces exactly what it detected.
        assert result.left_report.replaced_samples == result.left_report.detected.sample_count
        assert result.right_report.replaced_samples == result.right_report.detected.sample_count

    def test_median_mode_fills_only_replaced_samples(self) -> None:
        """The wavelet-only fields stay zero when median mode ran.

        A ctypes mirror that dropped or misordered a field would still pass a
        test exercising only the default mode, since three of the report's
        five fields are never witnessed there -- so this asserts all of them.
        """
        left, right = _fixture()
        result = libsonare.mastering_repair_decrackle_stereo(left, right, SR, mode="median")

        for report in (result.left_report, result.right_report):
            assert report.replaced_samples > 0
            assert report.detail_coefficients == 0
            assert report.shrunk_coefficients == 0
            assert report.noise_sigma == 0.0

    def test_wavelet_mode_fills_the_other_fields_and_detection_is_still_median(self) -> None:
        """Wavelet mode fills the opposite fields, but ``detected`` never changes.

        ``detected`` is always the median criterion regardless of mode, so it
        stays populated in wavelet mode even though ``replaced_samples`` --
        the median repair's own field -- is 0 because median repair did not
        run. That combination is correct, not a bug.
        """
        left, right = _wavelet_fixture()
        result = libsonare.mastering_repair_decrackle_stereo(
            left, right, SR, threshold=0.08, mode="waveletShrinkage"
        )

        for report in (result.left_report, result.right_report):
            assert report.detected.sample_count > 0
            assert report.replaced_samples == 0
            assert report.detail_coefficients > 0
            assert report.shrunk_coefficients > 0
            assert report.noise_sigma > 0.0

        # Different impulse placements produce different reports.
        assert result.left_report.detected.sample_count != result.right_report.detected.sample_count

    def test_threshold_reaches_the_median_detector(self) -> None:
        """Lowering ``threshold`` must move ``detected.sample_count`` in median mode.

        Reaching a range check on the way in proves only that the wrapper
        passed the value along; this proves it reaches ``count_crackle``
        itself. Both channels sit on a silent baseline with one impulse each,
        so the local median either side of the impulse is exactly 0 and the
        deviation is exactly the impulse's own magnitude -- an assertion this
        exact would not survive if threshold went nowhere.
        """
        left = np.zeros(64, dtype=np.float32)
        right = np.zeros(64, dtype=np.float32)
        left[32] = 0.3
        right[40] = 0.3

        undetected = libsonare.mastering_repair_decrackle_stereo(left, right, SR, threshold=0.4)
        assert undetected.left_report.detected.sample_count == 0
        assert undetected.right_report.detected.sample_count == 0

        detected = libsonare.mastering_repair_decrackle_stereo(left, right, SR, threshold=0.2)
        assert detected.left_report.detected.sample_count == 1
        assert detected.right_report.detected.sample_count == 1

    def test_wavelet_threshold_below_the_adaptive_value_moves_shrunk_coefficients(self) -> None:
        """In wavelet mode ``threshold`` is a cap, not the threshold itself.

        ``bayes_shrink_threshold`` returns ``min(max_threshold, noise_variance
        / signal_sigma)``, so the configured value only binds -- and only then
        can it move the outcome -- when it sits below what BayesShrink
        computes from the signal; a cap far above that value changes nothing.
        Soft-thresholding only zeroes a coefficient inside the operative
        threshold, so a cap small enough to bind pulls the effective threshold
        toward zero and zeroes almost nothing, the opposite of what "tighter
        cap" suggests -- measured here rather than assumed, which is exactly
        the trap this comparison is for.
        """
        left, right = _wavelet_fixture()

        not_binding = libsonare.mastering_repair_decrackle_stereo(
            left, right, SR, threshold=5.0, mode="waveletShrinkage"
        )
        binding = libsonare.mastering_repair_decrackle_stereo(
            left, right, SR, threshold=1e-4, mode="waveletShrinkage"
        )

        assert binding.left_report.shrunk_coefficients == 0
        assert not_binding.left_report.shrunk_coefficients > 0
        assert binding.right_report.shrunk_coefficients == 0
        assert not_binding.right_report.shrunk_coefficients > 0

    def test_explicit_kwargs(self) -> None:
        left, right = _fixture()
        result = libsonare.mastering_repair_decrackle_stereo(
            left,
            right,
            SR,
            threshold=0.5,
            mode="median",
            levels=3,
        )
        assert result.length == len(left)

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _fixture()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_decrackle_stereo(left, right[:-10], SR)

    def test_rejects_empty_input(self) -> None:
        empty = np.zeros(0, dtype=np.float32)
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_decrackle_stereo(empty, empty, SR)

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
            libsonare.mastering_repair_decrackle_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_decrackle_stereo(left, right, SR)
        assert result.length == len(left)
