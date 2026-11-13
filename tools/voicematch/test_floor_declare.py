"""A recording floor is declared only when note-independent and stationary; hum is counted either way."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from floor_declare import (
    BASIS_INTER,
    BASIS_SUB_F0,
    CENSOR_FLOOR,
    CENSOR_HUM,
    declare,
    hum_lines,
)
from metrics_note import body_bands

SR = 44100
DUR = 2.0
NOTES = [48, 51, 54, 57, 60, 63, 66, 69, 72]


def _hz(note: int) -> float:
    return 440.0 * 2 ** ((note - 69) / 12.0)


def _white(density_db_hz: float, n: int, rng) -> np.ndarray:
    return rng.normal(0.0, np.sqrt(10 ** (density_db_hz / 10.0) * SR / 2.0), n)


def _sine(hz: float, dbfs: float, n: int) -> np.ndarray:
    return np.sqrt(2.0) * 10 ** (dbfs / 20.0) * np.sin(2 * np.pi * hz * np.arange(n) / SR)


def _capture(noise, hum_dbfs=None, lines=(), tone_tau=1.0, tone_amp=0.1):
    """Rows of a nine-note capture: a decaying tone plus `noise(note, n, rng, t)` plus fixed lines."""
    rng = np.random.default_rng(5)
    n = int(DUR * SR)
    t = np.arange(n) / SR
    rows, wins = [], []
    for note in NOTES:
        f0 = _hz(note)
        x = np.zeros(n)
        h = 1
        while h * f0 < 12000:
            x += (
                tone_amp
                / h
                * np.cos(2 * np.pi * h * f0 * t + rng.uniform(0, 6.28))
                * np.exp(-t / tone_tau)
            )
            h += 1
        x += noise(note, n, rng, t)
        if hum_dbfs is not None:
            x += _sine(50.0, hum_dbfs, n) + _sine(150.0, hum_dbfs - 10.0, n)
        for hz, db in lines:
            x += _sine(hz, db, n)
        bands = body_bands(x, SR, f0)
        rows.append(
            {
                "timbre": "t0",
                "note": note,
                "velocity": 100,
                "f0_hz": f0,
                "body_under_db": round(bands[0], 2),
                "tnr_db": 20.0,
                "tnr_mask": None,
            }
        )
        wins.append({"x": x[int(0.12 * SR) : int(1.62 * SR)], "held": x, "sr": SR})
    return rows, wins


def test_stationary_sub_f0_floor_with_hum_is_declared_and_censors():
    rows, wins = _capture(lambda note, n, rng, t: _white(-108, n, rng), hum_dbfs=-85.0)
    rec, states = declare(rows, wins)
    assert rec["declared"] and rec["basis"] == BASIS_SUB_F0
    assert rec["stationary"]["verdict"] and rec["stationary"]["floor_like_share"] >= 0.6
    assert -112 < rec["level_db_hz"] < -104
    assert rec["hum"] is not None and abs(rec["hum"]["hz"] - 50.0) < 0.5
    assert rec["censors"] == ["body", "tnr"]
    assert all(s["censors"]["body"] in (CENSOR_FLOOR, CENSOR_HUM) for s in states)
    assert all(s["source"] == "clearance" for s in states)


def test_noise_that_falls_with_the_tone_is_no_floor():
    def noise(note, n, rng, t):
        return _white(-95, n, rng) * np.exp(-t / 1.0)

    rows, wins = _capture(noise, hum_dbfs=-90.0)
    rec, states = declare(rows, wins)
    assert not rec["declared"]
    assert not rec["stationary"]["verdict"]
    assert rec["reason"] == "not stationary in time"
    assert all(s["censors"]["tnr"] is None for s in states)


def test_noise_that_depends_on_the_note_is_no_floor():
    def noise(note, n, rng, t):
        return _white(-125 + 3.0 * (note - 48), n, rng)

    rows, wins = _capture(noise)
    rec, _ = declare(rows, wins)
    assert not rec["declared"]
    assert rec["reason"] == "not note-independent"


def test_hum_alone_censors_body_on_hum_without_a_floor():
    rows, wins = _capture(lambda note, n, rng, t: _white(-160, n, rng), hum_dbfs=-80.0)
    rec, states = declare(rows, wins)
    assert not rec["declared"]
    assert rec["hum"] is not None
    assert all(s["censors"]["body"] == CENSOR_HUM for s in states)
    assert all(s["source"] is None for s in states)


def test_fixed_non_mains_lines_are_recorded_and_not_a_floor():
    lines = [(4321.7, -102.0)]
    rows, wins = _capture(
        lambda note, n, rng, t: _white(-160, n, rng), lines=lines, tone_tau=1e9, tone_amp=0.01
    )
    rec, states = declare(rows, wins)
    assert not rec["declared"]
    assert rec["hum"] is None
    assert {round(ln["hz"]) for ln in rec["lines"]} == {4322}
    assert all(s["censors"]["body"] is None and s["censors"]["tnr"] is None for s in states)


def test_an_inter_partial_floor_is_recorded_and_censors_nothing():
    # Noise only between partials: no sub-f0 evidence, but a stationary note-independent bed.
    def noise(note, n, rng, t):
        return _white(-112, n, rng)

    rows, wins = _capture(noise, tone_amp=0.1)
    # Rows of one high register, so no band is wholly under any note's 0.7 f0 across 3 rows.
    rec, states = declare(rows[:2], wins[:2])
    assert rec["basis"] in (BASIS_INTER, None)
    if rec["declared"]:
        assert rec["censors"] == []
        assert all(s["censors"]["body"] is None for s in states)


def test_hum_family_needs_its_fundamental():
    def line(hz):
        return {"hz": hz, "dbfs": -90.0, "coverage": 1.0, "mains": None}

    assert [ln["hz"] for ln in hum_lines([line(50.0), line(150.0)])] == [50.0, 150.0]
    assert hum_lines([line(359.4)]) == []
    assert hum_lines([line(150.0)]) == []
