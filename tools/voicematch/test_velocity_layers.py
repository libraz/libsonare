"""Tests for the sampled-velocity-layer count.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_velocity_layers.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import velocity_layers as V


def cell(note, vel, ladder=(0.0, -6.0, -12.0), span=4.2, peak=-10.0):
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
    rows = [cell(60, 40), cell(60, 80), cell(60, 120, ladder=(0.0, -3.0, -7.0))]
    assert V.note_layers(rows) == {60: (3, 2)}
    s = V.layer_summary(rows + [cell(64, 40, span=3.0), cell(64, 80, span=3.5)])
    assert (s["velocities"], s["independent"], s["collapsed_notes"]) == (5, 4, [60])


def test_a_missing_field_never_reads_as_the_same_recording():
    a, b = cell(60, 40), cell(60, 80)
    b.pop("decay_span_s")
    a.pop("decay_span_s")
    assert V.note_layers([a, b]) == {60: (2, 2)}
