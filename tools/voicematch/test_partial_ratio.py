"""Tests for the two-capture partial ratio.

The rows are synthetic: capture B is capture A through a known band-wise gain,
so the measurement has an exact answer to recover, and a profile taken from
`reference/` could only be scored against the capture that motivated the check.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))

from profile_summary import (
    RATIO_BANDS_HZ,
    fundamental_dbfs,
    partial_ratio_by_band,
    partial_ratio_pairs,
)


def gain_db(f_hz: float) -> float:
    """A mute-shaped transfer: fundamentals pass, a dip above them, a lift on top."""
    if f_hz < 500.0:
        return 8.0
    if f_hz < 1000.0:
        return -3.0
    if f_hz < 2000.0:
        return -12.0
    if f_hz < 3000.0:
        return 0.0
    return 5.0


def row(note: int, velocity: int, f0: float, anchor_dbfs: float, shift, absent=()) -> dict:
    """One profile row whose partials fall 2 dB per step, shifted by `shift(f)` re h1.

    A partial in `absent` is at the ladder's floor and carries no power in the RMS."""
    base = [-2.0 * i for i in range(20)]
    partials = [
        -120.0 if i in absent else p + shift(f0 * (i + 1)) - shift(f0) for i, p in enumerate(base)
    ]
    rms = anchor_dbfs + 10.0 * np.log10(sum(10.0 ** (p / 10.0) for p in partials if p > -120.0))
    return {
        "note": note,
        "velocity": velocity,
        "f0_hz": f0,
        "partials_db": partials,
        "rms_dbfs": float(rms),
    }


def test_fundamental_level_is_recovered_from_rms_and_partials():
    r = row(60, 80, 261.6, -30.0, lambda f: 0.0)
    assert abs(fundamental_dbfs(r) - -30.0) < 1e-9


def test_band_medians_recover_the_gain_that_separates_the_captures():
    cells = [(57, 60, 220.0), (57, 100, 220.0), (69, 60, 440.0), (69, 100, 440.0)]
    rows_a = [row(n, v, f0, -30.0, lambda f: 0.0) for n, v, f0 in cells]
    # B's fundamental carries the gain at its own frequency; its partials the rest.
    rows_b = [row(n, v, f0, -30.0 + gain_db(f0), gain_db) for n, v, f0 in cells]
    pairs = partial_ratio_pairs(rows_b, rows_a)
    assert len(pairs) == 4 * 20
    assert all(abs(delta - gain_db(f)) < 1e-6 for f, delta in pairs)
    out = partial_ratio_by_band(rows_b, rows_a)
    assert [o["lo_hz"] for o in out] == [lo for lo, _ in RATIO_BANDS_HZ]
    for o in out:
        if o["n"] == 0:
            continue
        centre = o["lo_hz"] + 1.0
        assert abs(o["median_db"] - gain_db(centre)) < 1e-6, o
        assert abs(o["q25_db"] - o["median_db"]) < 1e-6
        assert abs(o["re_low_db"] - (gain_db(centre) - 8.0)) < 1e-6
    assert sum(o["n"] for o in out) == 4 * 20


def test_floor_bins_and_unshared_cells_are_left_out():
    # A partial the ladder found nothing in on one side carries no reading.
    a = row(57, 60, 220.0, -30.0, lambda f: 0.0, absent={10})
    b = row(57, 60, 220.0, -30.0, lambda f: 0.0)
    only_a = row(69, 60, 440.0, -30.0, lambda f: 0.0)
    out = partial_ratio_by_band([a, only_a], [b])
    assert sum(o["n"] for o in out) == 19
    for o in out:
        if o["n"]:
            assert abs(o["median_db"]) < 1e-6


def test_a_capture_with_no_shared_cell_reports_empty_bands():
    a = row(57, 60, 220.0, -30.0, lambda f: 0.0)
    b = row(69, 60, 440.0, -30.0, lambda f: 0.0)
    out = partial_ratio_by_band([a], [b])
    assert all(o["n"] == 0 and "median_db" not in o for o in out)
