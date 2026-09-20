"""What the screening report has to say about the bar that cut the knobs.

The count alone cannot be read: 27 of 34 kept is a good screening when the bar
sits in a gap and a coin flip when it sits inside a continuum, and the two print
the same line. These cases pin the readings that tell them apart, and the last
one pins the dilution invariance the `share` column exists for.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from staging import report_effect_distribution


def _report(effects, threshold, capsys) -> str:
    report_effect_distribution(effects, threshold)
    return capsys.readouterr().err


def _continuum(n: int = 34):
    """Effects spread evenly over four decades — no gap anywhere."""
    return [(f"k{i}", 10 ** (-5 + 4 * i / (n - 1))) for i in range(n)]


def _cliff():
    """Seven knobs that move nothing and ten that move a lot."""
    return ([(f"dead{i}", 1e-7) for i in range(7)]
            + [(f"live{i}", 0.05 + 0.01 * i) for i in range(10)])


def test_a_bar_inside_a_continuum_is_reported_as_one(capsys):
    out = _report(_continuum(), 0.002, capsys)
    assert "<- the bar is in this bucket" in out
    straddle = next(line for line in out.splitlines() if "straddling the bar" in line)
    assert "1.3x apart" in straddle, straddle


def test_a_bar_in_a_gap_is_reported_as_one(capsys):
    out = _report(_cliff(), 0.002, capsys)
    straddle = next(line for line in out.splitlines() if "straddling the bar" in line)
    assert "500000.0x apart" in straddle, straddle
    # Every threshold on the ladder keeps the same ten knobs, which is the
    # reading that says the exact value does not matter here.
    ladder = next(line for line in out.splitlines() if "another bar would keep" in line)
    assert ladder.count("->10") == 7, ladder


def test_the_share_column_is_invariant_to_dilution_and_the_count_is_not(capsys):
    """A saturated term scales every effect together; the absolute bar does not.

    This is the whole reason the share reading is printed. The control is the
    count on the same data: it MUST move, or the invariance below is invariance
    over a change that did not happen.
    """
    effects = _continuum()
    clean = _report(effects, 0.002, capsys)
    diluted = _report([(label, e * 0.572) for label, e in effects], 0.002, capsys)

    def line(out, needle):
        return next(ln for ln in out.splitlines() if needle in ln)

    clean_share = line(clean, "knobs kept by share:").split("knobs kept by share:")[1]
    diluted_share = line(diluted, "knobs kept by share:").split("knobs kept by share:")[1]
    assert clean_share == diluted_share

    clean_ladder = line(clean, "another bar would keep")
    diluted_ladder = line(diluted, "another bar would keep")
    assert clean_ladder != diluted_ladder, (
        "the control did not move, so the invariance above is over nothing"
    )
    assert "0.002->15" in clean_ladder, clean_ladder
    assert "0.002->13" in diluted_ladder, diluted_ladder


def test_an_empty_probe_prints_nothing_rather_than_dividing_by_a_largest(capsys):
    assert _report([], 0.002, capsys) == ""


def test_a_knob_that_moved_nothing_does_not_divide_the_straddle(capsys):
    out = _report([("dead", 0.0), ("live", 0.5)], 0.002, capsys)
    assert "moved nothing at all" in out
