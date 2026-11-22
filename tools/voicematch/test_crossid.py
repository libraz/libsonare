"""Tests for cross-identification — the contest, its fairness rules and the family tier.

Rows are synthetic instruments with known field values, so each test knows which
reference a stand-in must be nearest.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_crossid.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import crossid

NOTES = (48, 55, 60, 67, 72)
VELS = (64, 100)


def instrument(decay, tnr, attack, attack_hf, body, centroid, *, jitter=0.0, drop=()):
    rows = []
    for i, n in enumerate(NOTES):
        for j, v in enumerate(VELS):
            wiggle = jitter * ((i + j) % 3 - 1)
            row = {
                "timbre": "t",
                "note": n,
                "velocity": v,
                "cents_vs_et": 0.0,
                "decay_db_s": decay + wiggle,
                "tnr_db": tnr + wiggle,
                "attack_ms": attack + 10 * wiggle,
                "attack_hf_db": attack_hf + wiggle,
                "body_below_f0_db": body + wiggle,
                "centroid_hz": centroid * (1.0 + 0.01 * wiggle),
            }
            for k in drop:
                row.pop(k)
            rows.append(row)
    return rows


STRING = instrument(-3.0, 30.0, 80.0, -20.0, -10.0, 1500.0)
BELL = instrument(-0.5, 45.0, 5.0, -5.0, -30.0, 4000.0)
PLUCK = instrument(-12.0, 20.0, 3.0, -12.0, -18.0, 2500.0)


def test_a_close_stand_in_ranks_its_own_reference_first():
    refs = {"string": STRING, "bell": BELL, "pluck": PLUCK}
    got = crossid.identify(
        instrument(-3.2, 29.0, 85.0, -19.0, -11.0, 1550.0, jitter=0.3), "string", refs, set()
    )
    assert got["rank"] == 1
    assert not got["wrong_rank"]
    assert got["candidates"] == 2


def test_a_model_of_another_instrument_is_flagged():
    """The control that the detector can fail: a bell played where the string should be."""
    refs = {"string": STRING, "bell": BELL, "pluck": PLUCK}
    got = crossid.identify(instrument(-0.6, 44.0, 6.0, -5.5, -29.0, 3900.0), "string", refs, set())
    assert got["wrong_rank"]
    assert got["nearer"][0]["capture"] == "bell"


def test_a_rival_cannot_win_by_lacking_a_dimension():
    # The model matches its string on everything but attack, where it is far
    # off. A different instrument that cannot measure attack must not come out
    # nearer because the one column the model is wrong on vanished from its side.
    model = instrument(-3.0, 30.0, 400.0, -20.0, -10.0, 1500.0)
    blind = instrument(-12.0, 20.0, 3.0, -12.0, -18.0, 2500.0, drop=("attack_ms",))
    assert "attack" in crossid.pair(model, STRING)
    c = crossid.contest(model, STRING, blind)
    assert c["own"] == 0.0
    assert c["rival"] > c["own"]
    got = crossid.identify(model, "string", {"string": STRING, "blind": blind}, set())
    assert not got["wrong_rank"]


def test_too_few_shared_cells_is_no_contest():
    few = [r for r in BELL if r["note"] == 60]
    assert crossid.contest(STRING, STRING, few) is None


def test_losing_to_the_same_family_is_not_the_wrong_instrument():
    refs = {
        "string": STRING,
        "viola": instrument(-3.1, 30.5, 82.0, -19.5, -10.5, 1520.0),
        "bell": BELL,
    }
    model = instrument(-3.1, 30.5, 82.0, -19.5, -10.5, 1520.0)
    got = crossid.identify(model, "string", refs, set(), {"viola"})
    assert not got["wrong_rank"]
    assert got["family_rank"] == 2
    assert got["family_nearer"][0]["capture"] == "viola"


def test_same_patch_captures_are_not_rivals():
    refs = {"string": STRING, "string_module": BELL}
    got = crossid.identify(BELL, "string", refs, {"string_module"})
    assert got["candidates"] == 0
    assert not got["wrong_rank"]
