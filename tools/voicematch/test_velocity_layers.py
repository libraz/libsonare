"""Tests for the sampled-velocity-layer count.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_velocity_layers.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import velocity_layers as V


def cell(note, vel, ladder=(0.0, -6.0, -12.0, -15.0, -20.0), span=4.2, peak=-10.0):
    return {
        "note": note,
        "velocity": vel,
        "partials_db": list(ladder),
        "decay_span_s": span,
        "peak_dbfs": peak,
    }


def test_one_sample_at_three_gains_is_one_layer():
    rows = [cell(60, v, peak=-30.0 + v / 10) for v in (40, 80, 120)]
    assert V.note_layers(rows) == {60: (3, 1)}
    assert V.layer_summary(rows)["collapsed_notes"] == [60]


def test_a_brighter_harder_layer_is_its_own_recording():
    """The control: a different recording at the top velocity must count."""
    rows = [cell(60, 40), cell(60, 80), cell(60, 120, ladder=(0.0, -3.0, -7.0, -9.0, -12.0))]
    assert V.note_layers(rows) == {60: (3, 2)}
    other = (0.0, -5.0, -9.0, -14.0, -18.0)
    s = V.layer_summary(rows + [cell(64, 40), cell(64, 80, ladder=other)])
    assert (s["velocities"], s["independent"], s["collapsed_notes"]) == (5, 4, [60])


def test_a_missing_ladder_never_reads_as_the_same_recording():
    a, b = cell(60, 40), cell(60, 80)
    b.pop("partials_db")
    assert V.note_layers([a, b]) == {60: (2, 2)}


def hit(note, vel, bands, peak=-10.0):
    return {"note": note, "velocity": vel, "bands_db": list(bands), "peak_dbfs": peak}


BANDS = (-50.5, -4.1, 0.0, -15.6, -8.8, -11.5, -17.9)


def test_a_kit_key_under_gain_is_one_recording():
    """Kit rows read by their band ladder: one sample at six velocities is one layer."""
    rows = [hit(81, v, BANDS, peak=-40.0 + v / 4) for v in (20, 40, 60, 80, 100, 127)]
    assert V.note_layers(rows) == {81: (6, 1)}


def test_a_constant_offset_on_every_rung_is_still_one_recording():
    shifted = tuple(b - 3.0 for b in BANDS)
    assert V.same_recording(hit(119, 40, BANDS), hit(119, 100, shifted))
    # A tilt is a filter or another strike, not a gain.
    tilted = tuple(b - 0.5 * i for i, b in enumerate(BANDS))
    assert not V.same_recording(hit(119, 40, BANDS), hit(119, 100, tilted))


def test_too_few_rungs_never_reads_as_one_recording():
    short = (0.0, -6.0, -12.0)
    assert not V.same_recording(cell(60, 40, ladder=short), cell(60, 80, ladder=short))


def test_a_constant_square_law_across_notes_is_the_sampler():
    """The SoundFont default: amplitude as velocity squared on every note."""
    import math

    rows = [
        hit(n, v, BANDS, peak=-6.0 + 40.0 * math.log10(v / 127) + 0.3 * (n % 3))
        for n in (35, 38, 42)
        for v in (32, 64, 96, 127)
    ]
    s = V.layer_summary(rows)
    assert s["sampler_law"] and all(abs(e - 2.0) < 1e-6 for e in s["exponents"].values())
    # The control: strikes whose law differs by note are not the sampler.
    varied = [
        hit(n, v, BANDS, peak=20.0 * k * math.log10(v / 127))
        for n, k in ((35, 1.0), (38, 1.6), (42, 2.0))
        for v in (32, 64, 96, 127)
    ]
    assert not V.layer_summary(varied)["sampler_law"]


def test_a_spread_at_the_rounding_step_is_one_recording():
    """Stored rungs are rounded to 0.01 dB; a float difference of 0.0200000000000031 is two steps."""
    a = hit(67, 64, (-10.11, -20.0, -30.0, -40.0, 0.0))
    b = hit(67, 88, (-12.10, -22.01, -32.01, -42.01, -2.01))
    assert V.same_recording(a, b)
    # Three steps apart is no longer a pure gain.
    c = hit(67, 100, (-12.10, -22.02, -32.0, -42.0, -2.0))
    assert not V.same_recording(a, c)
