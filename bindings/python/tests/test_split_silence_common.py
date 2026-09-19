"""Tests for the multi-signal silence splitter.

What several takes of one part share is the silence, not the sound: a signal
sounding somewhere every OTHER signal is silent must still keep its own
interval, and that -- not agreement between signals -- is what these tests
measure.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    SonareValueError,
    split_silence,
    split_silence_common,
    split_silence_common_with_report,
)

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
DURATION = 1.0
TOP_DB = 60.0
FRAME_LENGTH = 2048
HOP_LENGTH = 512


def _tone_in_window(active_start: float, active_end: float) -> np.ndarray:
    """Return a 1-second silent signal with a tone sounding in [active_start, active_end)."""
    signal = np.zeros(int(SR * DURATION), dtype=np.float32)
    start = int(SR * active_start)
    end = int(SR * active_end)
    signal[start:end] = sine(440.0, (end - start) / SR, sr=SR)[: end - start]
    return signal


SIGNAL_A = _tone_in_window(0.0, 0.2)
SIGNAL_B = _tone_in_window(0.5, 0.7)


def test_single_signal_matches_split_silence() -> None:
    expected = split_silence(SIGNAL_A, TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    assert split_silence_common([SIGNAL_A], TOP_DB, FRAME_LENGTH, HOP_LENGTH) == expected


def test_union_keeps_each_signals_own_sounding_interval() -> None:
    """B's interval survives even though A is silent throughout it, and vice versa."""
    interval_a = split_silence(SIGNAL_A, TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    interval_b = split_silence(SIGNAL_B, TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    combined = split_silence_common([SIGNAL_A, SIGNAL_B], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    assert combined == sorted(interval_a + interval_b)


def test_order_does_not_affect_the_result() -> None:
    ab = split_silence_common([SIGNAL_A, SIGNAL_B], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    ba = split_silence_common([SIGNAL_B, SIGNAL_A], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    assert ab == ba


def test_signal_ending_before_it_would_sound_contributes_nothing() -> None:
    truncated_b = SIGNAL_B[: int(SR * 0.3)]  # cut before B's tone starts at 0.5s
    assert not np.any(truncated_b)
    only_a = split_silence(SIGNAL_A, TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    combined = split_silence_common([SIGNAL_A, truncated_b], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    assert combined == only_a


def test_all_silent_signals_return_no_intervals() -> None:
    silence = np.zeros(int(SR * DURATION), dtype=np.float32)
    assert split_silence_common([silence, silence], TOP_DB, FRAME_LENGTH, HOP_LENGTH) == []


def test_empty_signal_among_others_is_rejected_by_name() -> None:
    with pytest.raises(SonareValueError, match=r"signals\[1\]"):
        split_silence_common([SIGNAL_A, []], TOP_DB, FRAME_LENGTH, HOP_LENGTH)


def test_no_signals_is_rejected_by_name() -> None:
    with pytest.raises(SonareValueError, match="signals"):
        split_silence_common([])


def test_report_reads_a_part_that_never_stops_as_unfixable_by_threshold() -> None:
    """A ceiling near 0 says no threshold opens a gap, whatever top_db is passed."""
    tone = sine(440.0, DURATION, sr=SR)
    intervals, report = split_silence_common_with_report([tone], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    assert len(intervals) == 1
    assert report.max_signal_intervals == 1
    assert report.min_signal_intervals == 1
    # A steady tone's frames sit within a hair of each other, so the deepest dip
    # is nowhere near any usable threshold: that is what says "not the setting".
    assert report.silence_ceiling_db < 6.0


def test_report_reads_takes_that_never_go_quiet_together_against_top_db() -> None:
    """The ceiling at or above top_db, with one interval, is the alignment case."""
    early = _tone_in_window(0.0, 0.5)
    late = _tone_in_window(0.5, 1.0)
    intervals, report = split_silence_common_with_report(
        [early, late], TOP_DB, FRAME_LENGTH, HOP_LENGTH
    )
    assert len(intervals) == 1
    # The silence is there at this very setting, so a single interval means the
    # takes do not share it. The interval counts do NOT say so -- a take that
    # sounds once and stops counts 1, the same as a take with no silence at all.
    assert report.silence_ceiling_db >= TOP_DB
    assert report.max_signal_intervals == 1
    assert report.min_signal_intervals == 1


def test_report_ceiling_under_top_db_names_a_threshold_that_splits() -> None:
    """A dip both takes share, too shallow for top_db: the ceiling is actionable."""
    take = sine(440.0, DURATION, sr=SR).copy()
    take[int(SR * 0.4) : int(SR * 0.6)] *= np.float32(10.0 ** (-40.0 / 20.0))
    other = take.copy()

    loose_intervals, loose = split_silence_common_with_report(
        [take, other], TOP_DB, FRAME_LENGTH, HOP_LENGTH
    )
    assert len(loose_intervals) == 1
    assert loose.max_signal_intervals == 1
    # Below the threshold in use, which is what separates this case from the
    # test above, where the ceiling sits at or over it.
    assert loose.silence_ceiling_db < TOP_DB
    assert loose.silence_ceiling_db > 30.0

    tight_intervals, tight = split_silence_common_with_report(
        [take, other], loose.silence_ceiling_db - 5.0, FRAME_LENGTH, HOP_LENGTH
    )
    assert len(tight_intervals) == 2
    assert tight.min_signal_intervals == 2


def test_report_variant_returns_the_same_intervals_as_the_plain_one() -> None:
    plain = split_silence_common([SIGNAL_A, SIGNAL_B], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    intervals, _ = split_silence_common_with_report(
        [SIGNAL_A, SIGNAL_B], TOP_DB, FRAME_LENGTH, HOP_LENGTH
    )
    assert intervals == plain


def test_report_variant_rejects_the_same_inputs_by_the_same_name() -> None:
    with pytest.raises(SonareValueError, match=r"signals\[1\]"):
        split_silence_common_with_report([SIGNAL_A, []], TOP_DB, FRAME_LENGTH, HOP_LENGTH)
    with pytest.raises(SonareValueError, match="signals"):
        split_silence_common_with_report([])
