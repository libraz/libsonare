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
from metrics_lines import (
    BODY_LINE_MISSED_REASON,
    BODY_LINES_REASON,
    PRESENCE_IDLE,
    PRESENCE_NOTE_ONLY,
    PRESENCE_UNREADABLE,
    body_lines_in,
    line_presence,
    missed_body_lines,
    recurring_body_lines,
)
from metrics_note import BODY_WINDOW_S, body_bands, midi_to_hz
from profile_measure import body_lines_masked, measure_note

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


def test_a_row_carrying_only_missed_lines_keeps_the_floor_reason():
    row = {
        "floor_state": {"censors": {"body": CENSOR_FLOOR, "tnr": None}},
        "body_lines": {
            "missed": {"lines": [{"hz": 72.0, "share": 0.9}], "reason": BODY_LINE_MISSED_REASON}
        },
    }
    assert body_floor_state(row) == CENSOR_FLOOR


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


def test_a_line_on_one_note_too_few_is_a_known_miss_and_is_not_masked():
    takes = [
        (f0, body_lines_in(body_window(x), SR, f0))
        for f0, x in (note(n, (72.0,) if n in (58, 66) else ()) for n in NOTES)
    ]
    assert all(abs(ln.hz - 72.0) > 0.5 for ln in recurring_body_lines(takes))
    missed = missed_body_lines(takes)
    assert [(round(hz), idx) for hz, idx in missed] == [(72, [1, 3])]
    assert BODY_LINE_MISSED_REASON == "recurs on 2 notes; N=3"


def test_a_line_on_enough_notes_is_masked_and_not_missed():
    takes = [
        (f0, body_lines_in(body_window(x), SR, f0)) for f0, x in (note(n, LINES) for n in NOTES)
    ]
    assert missed_body_lines(takes) == []


def _tone(hz: float, seconds: float, level: float) -> np.ndarray:
    return level * np.sin(2 * np.pi * hz * np.arange(int(seconds * SR)) / SR)


def test_a_line_standing_idle_at_its_level_under_the_note_is_an_idle_tone():
    under = 20 * np.log10(1e-3)
    got = line_presence([under], [_tone(137.8, 0.1, 1e-3), _tone(137.8, 0.3, 1e-3)], SR, 137.8)
    assert got["presence"] == PRESENCE_IDLE and abs(got["idle_dbfs"] - under) < 1.0


def test_a_line_absent_from_a_silent_preroll_and_tail_is_part_of_the_note():
    got = line_presence([-110.0], [np.zeros(int(0.1 * SR)), np.zeros(int(0.3 * SR))], SR, 137.8)
    assert got == {"presence": PRESENCE_NOTE_ONLY, "note_dbfs": -110.0, "idle_dbfs": None}


def test_a_line_with_no_preroll_or_tail_to_read_is_unreadable():
    got = line_presence([-110.0], [np.zeros(0), np.zeros(int(0.01 * SR))], SR, 137.8)
    assert got["presence"] == PRESENCE_UNREADABLE


def test_a_row_records_its_missed_line_with_its_share_and_does_not_mask_it():
    _, x = note(58, (72.0,), line_level=3e-2)
    audio = np.concatenate([np.zeros(int(0.1 * SR)), x, np.zeros(int(0.5 * SR))])
    plain = measure_note(audio, SR, 58, preroll_s=0.1, gate_s=1.5)
    row = measure_note(audio, SR, 58, preroll_s=0.1, gate_s=1.5, body_lines_missed_hz=[72.0])
    missed = row["body_lines"]["missed"]
    assert "hz" not in row["body_lines"] and missed["reason"] == BODY_LINE_MISSED_REASON
    assert [ln["hz"] for ln in missed["lines"]] == [72.0] and missed["lines"][0]["share"] > 0.5
    assert row["body_under_db"] == plain["body_under_db"]
