"""Recurring fixed-frequency lines in the body band: found across notes, masked, the bed kept."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from floor_declare import CENSOR_FLOOR, CENSOR_HUM
from metrics_floor import BELOW_FLOOR_AFTER_LINES, body_floor_state
from metrics_lines import BODY_LINES_REASON, body_lines_in, recurring_body_lines
from metrics_note import BODY_WINDOW_S, body_bands, midi_to_hz
from profile_measure import body_lines_masked

SR = 48000
LINES = (99.5, 161.0)
NOTES = (54, 58, 62, 66, 70, 74)


def bed(seconds: float, level: float, seed: int, top_hz: float = 400.0) -> np.ndarray:
    """A broadband sub-f0 bed: noise lowpassed at `top_hz`."""
    spec = np.fft.rfft(np.random.default_rng(seed).standard_normal(int(seconds * SR)))
    spec[np.fft.rfftfreq(int(seconds * SR), 1.0 / SR) > top_hz] = 0.0
    x = np.fft.irfft(spec, int(seconds * SR))
    return level * x / x.std()


def note(n: int, lines: tuple[float, ...], line_level: float = 1e-2, bed_level: float = 1e-3):
    f0 = midi_to_hz(n)
    t = np.arange(int(1.6 * SR)) / SR
    x = sum(np.sin(2 * np.pi * k * f0 * t) / k for k in range(1, 8)) * 0.1
    x = x + bed(1.6, bed_level, n)
    for hz in lines:
        x = x + line_level * np.sin(2 * np.pi * hz * t + n)
    return f0, x


def body_window(x: np.ndarray) -> np.ndarray:
    return x[int(BODY_WINDOW_S[0] * SR) : int(BODY_WINDOW_S[1] * SR)]


def test_two_fixed_lines_recurring_across_notes_are_found_and_the_bed_is_not():
    takes = [
        (f0, body_lines_in(body_window(x), SR, f0)) for f0, x in (note(n, LINES) for n in NOTES)
    ]
    found = recurring_body_lines(takes)
    assert [ln.hz for ln in found] == pytest.approx(LINES, abs=0.3)
    assert all(ln.takes >= 3 for ln in found)


def test_a_bed_alone_has_no_recurring_line():
    takes = [(f0, body_lines_in(body_window(x), SR, f0)) for f0, x in (note(n, ()) for n in NOTES)]
    assert recurring_body_lines(takes) == []


def test_a_line_on_one_note_is_not_recurring():
    takes = [
        (f0, body_lines_in(body_window(x), SR, f0))
        for f0, x in (note(n, LINES if n == 62 else ()) for n in NOTES)
    ]
    assert recurring_body_lines(takes) == []


def test_a_line_on_the_notes_own_subharmonic_is_the_notes_not_a_recurring_one():
    f0, x = note(66, ())
    t = np.arange(x.size) / SR
    x = x + 1e-2 * np.sin(2 * np.pi * f0 / 2 * t)
    assert body_lines_in(body_window(x), SR, f0) == []


def test_the_masked_row_loses_the_lines_and_keeps_the_bed():
    f0, lined = note(66, LINES)
    _, bare = note(66, ())
    removed = body_lines_masked(lined, SR, f0, list(LINES))
    assert removed["reason"] == BODY_LINES_REASON and removed["hz"] == [99.5, 161.0]
    assert removed["share_removed"] > 0.9
    after = body_bands(lined, SR, f0, LINES)[0]
    assert after == pytest.approx(body_bands(bare, SR, f0)[0], abs=1.5)
    assert body_bands(lined, SR, f0)[0] > after + 10.0


def test_a_row_with_no_line_in_its_band_is_left_alone():
    f0, x = note(54, LINES)
    assert body_lines_masked(x, SR, f0, [300.0]) == {}
    assert body_bands(x, SR, f0, [300.0]) == body_bands(x, SR, f0)


def test_a_body_at_the_floor_once_its_lines_are_masked_is_below_the_floor():
    row = {"floor_state": {"censors": {"body": CENSOR_FLOOR, "tnr": None}}}
    assert body_floor_state(row) == CENSOR_FLOOR
    row["body_lines"] = {"hz": [99.5], "share_removed": 0.97, "reason": BODY_LINES_REASON}
    assert body_floor_state(row) == BELOW_FLOOR_AFTER_LINES
    row["floor_state"]["censors"]["body"] = CENSOR_HUM
    assert body_floor_state(row) == CENSOR_HUM
    row["floor_state"]["censors"]["body"] = None
    assert body_floor_state(row) is None


def test_a_recurring_resonance_that_decays_is_a_body_mode_and_not_a_line():
    """A soundboard's modes ring at the same frequencies for every note but fall as the note does."""
    takes = []
    for n in NOTES:
        f0, x = note(n, ())
        tt = np.arange(x.size) / SR
        x = x + 3e-2 * np.exp(-3.0 * tt) * np.sin(2 * np.pi * 111.5 * tt)
        takes.append((f0, body_lines_in(body_window(x), SR, f0)))
    assert any(abs(hz - 111.5) < 0.5 for _, lines in takes for hz, _ in lines)
    assert recurring_body_lines(takes) == []


def test_a_mains_line_is_hum_and_not_a_line_in_the_source():
    takes = [
        (f0, body_lines_in(body_window(x), SR, f0)) for f0, x in (note(n, (100.0,)) for n in NOTES)
    ]
    assert all(lines == [] for _, lines in takes)
