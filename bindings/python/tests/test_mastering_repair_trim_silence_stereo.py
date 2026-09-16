"""Tests for the stereo silence-trimmer Python wrapper.

``mastering_repair_trim_silence_stereo`` is a ctypes pass-through over
``mastering::repair::trim_silence_stereo``, and it breaks three shapes the six
stereo repairs before it all share.

``result.length`` is an OUTPUT length: trimming shortens the pair, where every
other stereo repair hands back exactly the input length. The result can be
EMPTY -- an all-silent pair returns NULL buffers and length 0 alongside
SONARE_OK, which the wrapper must present as two empty lists rather than a
refusal or a dereference of NULL. And the report shape is a third distinct one:
one ``report`` plus ``left_range``/``right_range``, where the first four
siblings carry ``left_report``/``right_report`` and the denoise/dereverb pair
carry one ``report`` alone.

The fixtures below deliberately give the two channels signal over DIFFERENT
spans. A pair that agreed would pass equally against an implementation cutting
each channel by its own range -- the one rule the contract forbids, because it
would not keep the two outputs the same length.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
N = SR  # one second, so every index below is inside the buffer

# The two spans, disjoint and in this order: left opens first, right closes
# last, so the union takes one edge from each channel and a wrapper that read
# only one of them cannot produce it.
LEFT_SPAN = (4000, 8000)
RIGHT_SPAN = (12000, 16000)


def _burst(span: tuple[int, int], amp: float = 0.5) -> NDArray[np.float32]:
    """Digital silence with one loud block, so the edges are unambiguous."""
    out = np.zeros(N, dtype=np.float32)
    out[span[0] : span[1]] = amp
    return out


def _offset_pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    return _burst(LEFT_SPAN), _burst(RIGHT_SPAN)


def _silent_pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    return np.zeros(N, dtype=np.float32), np.zeros(N, dtype=np.float32)


class TestMasteringRepairTrimSilenceStereo:
    def test_output_length_is_the_kept_range_not_the_input_length(self) -> None:
        """The field that differs from every other stereo repair.

        A wrapper written by pattern-matching the siblings would size its arrays
        from the input; here that would over-read the two heap buffers by the
        amount that was trimmed away.
        """
        left, right = _offset_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)

        kept = RIGHT_SPAN[1] - LEFT_SPAN[0]
        assert result.length == kept
        assert result.length < N
        assert len(result.left) == kept
        assert len(result.right) == kept
        assert isinstance(result.left, list)
        assert isinstance(result.right, list)

    def test_both_channels_are_cut_to_the_union_not_to_their_own_range(self) -> None:
        """The rule the contract forbids breaking, checked by content.

        The channels carry signal over disjoint spans, so per-channel cutting
        would leave two outputs of 4000 samples each holding their own burst.
        Cutting both to the union instead puts each burst at its own offset
        inside a shared 12000-sample window, with the other channel silent
        there.
        """
        left, right = _offset_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)

        out_left = np.asarray(result.left, dtype=np.float32)
        out_right = np.asarray(result.right, dtype=np.float32)
        origin = result.report.range.first

        left_lo, left_hi = LEFT_SPAN[0] - origin, LEFT_SPAN[1] - origin
        right_lo, right_hi = RIGHT_SPAN[0] - origin, RIGHT_SPAN[1] - origin

        assert np.all(out_left[left_lo:left_hi] == pytest.approx(0.5))
        assert np.all(out_left[right_lo:right_hi] == 0.0)
        assert np.all(out_right[right_lo:right_hi] == pytest.approx(0.5))
        assert np.all(out_right[left_lo:left_hi] == 0.0)

    def test_report_carries_one_range_plus_the_two_it_was_unioned_from(self) -> None:
        """Shape C: one ``report`` and TWO per-channel ranges.

        Each per-channel range must be its own channel's span -- asserting only
        that the union is right would pass against a wrapper that copied
        ``report.range`` into both.
        """
        left, right = _offset_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)

        assert not hasattr(result, "left_report")
        assert not hasattr(result, "right_report")
        assert isinstance(result.report, libsonare.TrimReport)
        assert isinstance(result.report.range, libsonare.TrimRange)
        assert isinstance(result.left_range, libsonare.TrimRange)
        assert isinstance(result.right_range, libsonare.TrimRange)

        assert (result.left_range.first, result.left_range.last_exclusive) == LEFT_SPAN
        assert (result.right_range.first, result.right_range.last_exclusive) == RIGHT_SPAN
        assert result.left_range != result.right_range

        assert result.report.range.first == min(result.left_range.first, result.right_range.first)
        assert result.report.range.last_exclusive == max(
            result.left_range.last_exclusive, result.right_range.last_exclusive
        )

    def test_a_channel_carrying_nothing_contributes_nothing_to_the_union(self) -> None:
        """An empty per-channel range is skipped rather than unioned in.

        Unioning an empty ``(N, N)`` range in numerically would push the kept
        range's end to the end of the buffer and trim nothing off the tail, so
        this separates the documented rule from the arithmetic one.
        """
        left, _ = _offset_pair()
        silence = np.zeros(N, dtype=np.float32)
        result = libsonare.mastering_repair_trim_silence_stereo(left, silence, SR)

        assert result.right_range.first == result.right_range.last_exclusive
        assert (result.report.range.first, result.report.range.last_exclusive) == LEFT_SPAN
        assert result.length == LEFT_SPAN[1] - LEFT_SPAN[0]

    def test_an_all_silent_pair_returns_two_empty_arrays_and_does_not_raise(self) -> None:
        """The success the C entry signals with NULL buffers and length 0.

        The wrapper must read ``length`` before either pointer: a wrapper that
        sized its arrays from the INPUT length raises ``ValueError: NULL pointer
        access`` here -- ctypes guards both the index and the ``.contents`` a
        cast-to-array goes through -- which is a refusal the contract says this
        call does not make. A following valid call is made on the same process,
        so a failure that left the library in a bad state would still show.
        """
        silent_left, silent_right = _silent_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(silent_left, silent_right, SR)

        assert result.length == 0
        assert result.left == []
        assert result.right == []

        left, right = _offset_pair()
        follow_up = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)
        assert follow_up.length == RIGHT_SPAN[1] - LEFT_SPAN[0]

    def test_the_all_silent_report_puts_the_whole_buffer_in_the_head(self) -> None:
        """``(length, length)``, so the head takes everything and the tail is 0.

        The split between the ends is arbitrary there; what is not arbitrary is
        that the two still sum to the input length, which is the figure a caller
        reporting "how much went" reads.
        """
        silent_left, silent_right = _silent_pair()
        report = libsonare.mastering_repair_trim_silence_stereo(
            silent_left, silent_right, SR
        ).report

        assert (report.range.first, report.range.last_exclusive) == (N, N)
        assert report.removed_head_samples == N
        assert report.removed_tail_samples == 0
        assert report.removed_head_samples + report.removed_tail_samples == N

    def test_removed_counts_account_for_the_whole_input(self) -> None:
        """The non-degenerate half of the claim above."""
        left, right = _offset_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)
        report = result.report

        assert report.removed_head_samples == LEFT_SPAN[0]
        assert report.removed_tail_samples == N - RIGHT_SPAN[1]
        assert report.removed_head_samples + result.length + report.removed_tail_samples == N

    def test_threshold_is_live_in_peak_mode_and_inert_in_the_gated_one(self) -> None:
        """``threshold`` is read ONLY by peak mode.

        Both halves are needed: the gated half alone would pass against a
        wrapper that never marshalled ``threshold`` at all, and the peak half
        alone would pass against one that read it in both modes.
        """
        left, right = _offset_pair()
        bursts_at = 0.5

        peak_low = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="peak", threshold=0.001
        )
        peak_high = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="peak", threshold=bursts_at * 2.0
        )
        assert peak_low.length == RIGHT_SPAN[1] - LEFT_SPAN[0]
        # Nothing clears a threshold above the bursts, so the pair empties.
        assert peak_high.length == 0

        gated_low = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="lufs_gated", threshold=0.001, gate_lufs=-40.0, window_ms=20.0
        )
        gated_high = libsonare.mastering_repair_trim_silence_stereo(
            left,
            right,
            SR,
            mode="lufs_gated",
            threshold=bursts_at * 2.0,
            gate_lufs=-40.0,
            window_ms=20.0,
        )
        # The gated pass kept something, so the two below agree about a
        # measurement rather than about a pair of empty results.
        assert gated_low.length > 0
        assert gated_high.length == gated_low.length
        assert gated_high.report.range == gated_low.report.range

    def test_gate_lufs_and_window_ms_are_live_only_in_the_gated_mode(self) -> None:
        """The mirror of the assertion above, for the two gated-only fields.

        ``window_ms`` sizes an RMS window centred on each sample, so a wider one
        smears the burst's energy further and opens the kept range earlier;
        neither field may move anything under peak mode.
        """
        left, right = _offset_pair()

        narrow = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="lufs_gated", gate_lufs=-40.0, window_ms=10.0
        )
        wide = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="lufs_gated", gate_lufs=-40.0, window_ms=60.0
        )
        assert narrow.report.range.first > 0
        assert wide.report.range.first < narrow.report.range.first
        assert wide.length > narrow.length

        # A gate near the bursts' own level, not above them: both passes keep
        # something, so the two lengths separate by a measurement rather than by
        # one side collapsing to the empty result.
        strict = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="lufs_gated", gate_lufs=-7.0, window_ms=20.0
        )
        lenient = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="lufs_gated", gate_lufs=-40.0, window_ms=20.0
        )
        assert strict.length > 0
        assert strict.length < lenient.length

        peak_a = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="peak", gate_lufs=-6.0, window_ms=10.0
        )
        peak_b = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, mode="peak", gate_lufs=-40.0, window_ms=60.0
        )
        assert peak_a.length == peak_b.length
        assert peak_a.report.range == peak_b.report.range

    def test_padding_widens_the_kept_range_and_is_clamped_to_the_buffer(self) -> None:
        left, right = _offset_pair()
        pad = 1000

        padded = libsonare.mastering_repair_trim_silence_stereo(
            left, right, SR, padding_samples=pad
        )
        assert padded.report.range.first == LEFT_SPAN[0] - pad
        assert padded.report.range.last_exclusive == RIGHT_SPAN[1] + pad
        assert padded.length == (RIGHT_SPAN[1] - LEFT_SPAN[0]) + 2 * pad

        # Clamped rather than wrapped or refused: a padding wider than either
        # margin can only reach the ends of the buffer.
        clamped = libsonare.mastering_repair_trim_silence_stereo(left, right, SR, padding_samples=N)
        assert clamped.report.range.first == 0
        assert clamped.report.range.last_exclusive == N
        assert clamped.length == N

    def test_an_all_silent_pass_is_not_padded(self) -> None:
        """Padding a range that kept nothing would resurrect samples it dropped."""
        silent_left, silent_right = _silent_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(
            silent_left, silent_right, SR, padding_samples=1000
        )
        assert result.length == 0
        assert (result.report.range.first, result.report.range.last_exclusive) == (N, N)

    def test_rejects_a_negative_padding_count_before_it_becomes_a_size_t(self) -> None:
        """``padding_samples`` is a ``size_t``, so -1 would arrive near SIZE_MAX.

        The core does refuse that -- anything above SIZE_MAX/2 is out of range --
        but as a statement about the wrapped value, not about what was passed.
        This wrapper refuses it by name first, matching the mono entry. The
        second half pins that the native refusal is still reachable, so the
        pre-check is narrowing the message rather than hiding a live path.
        """
        left, right = _offset_pair()

        with pytest.raises(ValueError, match="padding_samples"):
            libsonare.mastering_repair_trim_silence_stereo(left, right, SR, padding_samples=-1)

        with pytest.raises(libsonare.SonareError, match="padding_samples"):
            libsonare.mastering_repair_trim_silence_stereo(
                left, right, SR, padding_samples=(1 << 64) - 1
            )

    def test_explicit_kwargs(self) -> None:
        left, right = _offset_pair()
        result = libsonare.mastering_repair_trim_silence_stereo(
            left,
            right,
            SR,
            threshold=0.01,
            padding_samples=128,
            mode="peak",
            gate_lufs=-50.0,
            window_ms=200.0,
        )
        assert result.length == (RIGHT_SPAN[1] - LEFT_SPAN[0]) + 256
        assert result.report.range.first == LEFT_SPAN[0] - 128

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _offset_pair()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_trim_silence_stereo(left, right[:-10], SR)

    def test_rejects_an_unknown_mode(self) -> None:
        left, right = _offset_pair()
        with pytest.raises(ValueError, match="mode"):
            libsonare.mastering_repair_trim_silence_stereo(left, right, SR, mode="nonexistent")

    def test_rejects_empty_input(self) -> None:
        """An empty INPUT is a refusal, unlike an empty RESULT.

        The two look alike from the outside and the contract separates them:
        nothing to scan is invalid, while scanning and finding nothing is a
        measurement.
        """
        empty = np.zeros(0, dtype=np.float32)
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_trim_silence_stereo(empty, empty, SR)

    def test_native_refusal_leaves_no_stale_allocation(self) -> None:
        """``sample_rate`` is validated natively, not by this binding.

        The out-struct is cleared before the argument checks, so the refused
        call leaves NULL buffers and a zeroed report rather than anything to
        free twice; the following call proves the process survived it.
        """
        left, right = _offset_pair()
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_trim_silence_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)
        assert result.length == RIGHT_SPAN[1] - LEFT_SPAN[0]
