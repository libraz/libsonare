"""Tests for the multi-signal silence splitter.

What several takes of one part share is the silence, not the sound: a signal
sounding somewhere every OTHER signal is silent must still keep its own
interval, and that -- not agreement between signals -- is what these tests
measure.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import SonareValueError, split_silence, split_silence_common

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
