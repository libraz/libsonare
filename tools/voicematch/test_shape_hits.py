"""The per-hit breakdown of an audition page."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape import hits  # noqa: E402
from wavio import write_wav  # noqa: E402

SR = 8000


def item(notes, seconds=4.0, tid="take", label="A take"):
    return {"id": tid, "label": label,
            "meta": {"seconds": seconds, "notes": notes, "cc": []}}


def note(n, start, velocity=100, duration=0.05):
    return {"note": n, "velocity": velocity, "start": start, "duration": duration}


def test_hits_are_numbered_by_when_they_sound_not_by_manifest_order():
    """A phrase builder appends voice by voice; the ear meets them in time.

    The groove take writes eight hats, then its kicks, then its snares, so the
    manifest's third entry sounds sixth. Numbering the file's order would give
    "the second one" a different instrument at each end of the conversation,
    which is the whole failure this module exists to remove.
    """
    rows = hits.hit_rows(item([note(42, 0.3), note(42, 0.6), note(36, 0.3),
                               note(38, 0.9)]))
    assert [r["n"] for r in rows] == [1, 2, 3]
    assert [r["start"] for r in rows] == [0.3, 0.6, 0.9]
    assert sorted(n["note"] for n in rows[0]["notes"]) == [36, 42]


def test_simultaneous_strikes_share_one_row_because_no_window_separates_them():
    rows = hits.hit_rows(item([note(36, 0.3), note(42, 0.3 + hits.FUSED_S / 2)]))
    assert len(rows) == 1
    assert sorted(n["note"] for n in rows[0]["notes"]) == [36, 42]


def test_strikes_further_apart_than_the_fuse_are_separate_hits():
    rows = hits.hit_rows(item([note(36, 0.3), note(42, 0.3 + hits.FUSED_S * 2)]))
    assert [r["n"] for r in rows] == [1, 2]


def test_a_hit_is_measured_up_to_the_next_one():
    rows = hits.hit_rows(item([note(42, 0.3), note(42, 0.7)]))
    hits.hit_windows(rows, 4.0)
    assert rows[0]["window"] == pytest.approx((0.3, 0.7))


def test_a_let_ring_hit_is_capped_rather_than_taking_the_rest_of_the_take():
    """Otherwise the last row restates the whole-take table it breaks down."""
    rows = hits.hit_rows(item([note(51, 0.3)], seconds=13.0))
    hits.hit_windows(rows, 13.0)
    lo, hi = rows[0]["window"]
    assert hi - lo == pytest.approx(hits.MAX_WINDOW_S)


def test_the_onset_shift_finds_a_strike_the_plugin_rendered_late():
    """A reference carries the plugin's own latency and the model carries none.

    Uncorrected it moves every window off the strike it is meant to hold, and
    the further into the phrase a hit sits the more of the previous one it
    measures instead.
    """
    x = np.zeros(SR * 2, dtype=np.float64)
    x[int(0.5 * SR):int(0.5 * SR) + 200] = 1.0
    assert hits.onset_shift(x, SR, 0.5) == pytest.approx(0.0, abs=0.002)
    assert hits.onset_shift(x, SR, 0.44) == pytest.approx(0.06, abs=0.002)


def test_a_silent_source_reports_no_shift_rather_than_an_arbitrary_one():
    assert hits.onset_shift(np.zeros(SR, dtype=np.float64), SR, 0.5) == 0.0


def burst(sr, seconds, at, freq, amp=0.4, decay=40.0):
    t = np.arange(int(seconds * sr)) / sr
    e = np.where(t >= at, np.exp(-(t - at) * decay), 0.0)
    return amp * e * np.sin(2 * np.pi * freq * t)


def page(tmp_path, model, ref, notes, seconds=2.0):
    d = tmp_path / "set-under-test"
    (d / "take").mkdir(parents=True)
    write_wav(d / "take" / "model.wav", model.astype(np.float32), SR)
    write_wav(d / "take" / "kit-a.wav", ref.astype(np.float32), SR)
    man = {"title": "t",
           "sources": {"model": {"role": "model"}, "kit-a": {"role": "reference"}},
           "items": [item(notes, seconds=seconds)]}
    (d / "manifest.json").write_text(json.dumps(man))
    return d


def measured(model, ref, notes, seconds=2.0):
    """The rows and the shared gain, so a test can assert a cell's value."""
    it = item(notes, seconds=seconds)
    rows = hits.hit_rows(it)
    hits.hit_windows(rows, seconds)
    tracks = {"model": (model, SR), "kit-a": (ref, SR)}
    floors = {s: hits.window_for(it, "floor") for s in tracks}
    for s in tracks:
        hits.measure_hit(tracks, rows, {s: 0.0 for s in tracks}, floors, s)
    gain = hits.shared_gain_db(tracks, "model", "kit-a", hits.window_for(it, "body"))
    return rows, gain


def cell(row, gain, band):
    m = row["band"]["model"].get(band)
    k = row["band"]["kit-a"].get(band)
    return None if m is None or k is None else m + gain - k


def band_burst(at, lo, hi, amp, seconds=2.0, decay=12.0, seed=0):
    """A decaying burst of noise confined to one band.

    A pure tone will not do here. Its envelope puts enough energy near DC that
    the take's own infrasonic floor reading sits within ten decibels of a real
    band, and the gate -- correctly -- refuses to read it, so a test written on
    tones asserts against blanks. A struck instrument is a band of noise, which
    is what the gate was built for and what these rows are taken from.
    """
    n = int(seconds * SR)
    t = np.arange(n) / SR
    S = np.fft.rfft(np.random.default_rng(seed).standard_normal(n))
    fr = np.fft.rfftfreq(n, 1.0 / SR)
    S[(fr < lo) | (fr >= hi)] = 0.0
    x = np.fft.irfft(S, n)
    x /= max(float(np.abs(x).max()), 1e-12)
    return amp * np.where(t >= at, np.exp(-(t - at) * decay), 0.0) * x


def two_bands(at, low_amp, high_amp, seed=0):
    return (band_burst(at, 250.0, 500.0, low_amp, seed=seed)
            + band_burst(at, 1000.0, 2000.0, high_amp, seed=seed + 1))


def test_a_plain_level_offset_reads_as_no_band_difference(tmp_path):
    """The shared gain is what makes a non-zero cell a statement about tilt.

    Two sources that differ only in how loud they are must come back flat, or
    every row of the table is a restatement of loudness -- which the whole-take
    table already reports on its own line.
    """
    notes = [note(42, 0.3), note(42, 1.0)]
    model = two_bands(0.3, 0.4, 0.2, seed=1) + two_bands(1.0, 0.4, 0.2, seed=3)
    rows, gain = measured(model, model * 0.25, notes)
    # Negative: the gain is what the model is corrected BY, so a model four
    # times the reference's amplitude is brought down rather than the reference
    # brought up. The table's own line says "applied to" for that reason.
    assert gain == pytest.approx(-12.0, abs=0.3)
    for band in ((250, 500), (1000, 2000)):
        assert cell(rows[0], gain, band) == pytest.approx(0.0, abs=0.3)


def test_a_tilt_survives_the_shared_gain(tmp_path):
    """The same total power, put in different places, must still read."""
    notes = [note(42, 0.3), note(42, 1.0)]
    model = two_bands(0.3, 0.4, 0.1, seed=1) + two_bands(1.0, 0.4, 0.1, seed=3)
    ref = two_bands(0.3, 0.1, 0.4, seed=1) + two_bands(1.0, 0.1, 0.4, seed=3)
    rows, gain = measured(model, ref, notes)
    assert cell(rows[0], gain, (250, 500)) > 8.0
    assert cell(rows[0], gain, (1000, 2000)) < -8.0


def test_a_page_with_no_reference_says_so_rather_than_comparing_a_render_with_itself(
        tmp_path):
    d = tmp_path / "model-only"
    (d / "take").mkdir(parents=True)
    write_wav(d / "take" / "model.wav",
              burst(SR, 2.0, 0.3, 250.0).astype(np.float32), SR)
    man = {"title": "t", "sources": {"model": {"role": "model"}},
           "items": [item([note(42, 0.3)], seconds=2.0)]}
    (d / "manifest.json").write_text(json.dumps(man))
    with pytest.raises(ValueError, match="no reference"):
        hits.run(d, "u/")


def test_every_row_prints_the_note_it_plays_and_the_link_that_sounds_it(tmp_path):
    """The row is the vocabulary. An index alone is what we already had."""
    notes = [note(46, 0.3), note(42, 0.9)]
    model = burst(SR, 2.0, 0.3, 400.0) + burst(SR, 2.0, 0.9, 400.0)
    out = hits.run(page(tmp_path, model, model * 0.5, notes), "http://h/#s/")
    assert "http://h/#s/take/model" in out
    assert "http://h/#s/take/kit-a" in out
    assert "take#1" in out and "take#2" in out
    assert "46 Open Hi-Hat v100" in out
    assert "42 Closed Hi-Hat v100" in out
