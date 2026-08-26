"""Writing back the level a per-piece fit divided out."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape import regain  # noqa: E402

BASE = {"d042.gain": 1.0, "d049.gain": 2.0, "d052.gain": 4.0}


def test_a_piece_measured_loud_has_its_gain_brought_down():
    gains, stuck = regain.corrections(BASE, {}, {42: 6.0206})
    assert gains["d042.gain"] == pytest.approx(0.5, abs=1e-3)
    assert not stuck


def test_a_piece_measured_quiet_has_its_gain_brought_up():
    gains, stuck = regain.corrections(BASE, {}, {49: -6.0206})
    assert gains["d049.gain"] == pytest.approx(4.0, abs=1e-3)
    assert not stuck


def test_the_fit_s_own_gain_is_what_gets_corrected_not_the_shipped_one():
    """A fit that already moved the gain must be corrected from where it left it.

    Correcting from the shipped value instead would discard the fit's move and
    re-apply the whole offset on top of it.
    """
    gains, _ = regain.corrections(BASE, {"d042.gain": 2.0}, {42: 6.0206})
    assert gains["d042.gain"] == pytest.approx(1.0, abs=1e-3)


def test_a_piece_the_clamp_cannot_satisfy_is_named_rather_than_truncated():
    """The same finding as a fitted coordinate sitting at a range bound.

    Silently clipping would leave the piece short and the correction reporting
    success, which is how a level error survives a level correction.
    """
    gains, stuck = regain.corrections(BASE, {}, {52: -12.0})
    assert gains["d052.gain"] == regain.GAIN_MAX
    assert [n for n, _want, _got in stuck] == [52]
    want = [w for n, w, _ in stuck][0]
    assert want > regain.GAIN_MAX


def test_a_piece_the_reference_could_not_be_read_for_is_left_alone():
    gains, stuck = regain.corrections(BASE, {}, {})
    assert gains == {} and stuck == []


def test_a_note_with_no_gain_coordinate_is_skipped_rather_than_invented():
    gains, _ = regain.corrections(BASE, {}, {38: 3.0})
    assert "d038.gain" not in gains


PROFILE_TABLE = """
 note  vel |  tilt Δdb  shape db  decay Δdb/s  centroid Δ%  attack Δms  crest Δdb  level Δdb
-----------------------------------------------------------------------------------------
   42   48 |      +1.5       6.8       +45.32        +13.6        +1.0       -3.0       -5.2
   42   64 |      +3.0       7.7       +48.96        +21.7        +0.5       -4.4       -4.8
   42   88 |      +2.0       8.1       +29.22        +16.8        -8.0       -1.7       -2.0
   46  100 |      +5.2      16.2        -1.46        +11.2        -5.0       -3.8       -3.6

                                                  median  |median|  rows
  band tilt, + = model is brighter (dB)              +2.71      3.80    54
"""


def test_the_level_column_is_read_per_note_from_the_gate_s_own_table():
    got = regain.parse_profile_levels(PROFILE_TABLE)
    assert got == {42: pytest.approx(-4.8), 46: pytest.approx(-3.6)}


def test_the_summary_block_is_not_mistaken_for_a_row():
    """Its lines carry numbers too, and one of them ends in a row count."""
    assert 54 not in regain.parse_profile_levels(PROFILE_TABLE)


def test_a_table_with_no_rows_yields_no_correction():
    assert regain.parse_profile_levels("no rows here\n---\n") == {}
