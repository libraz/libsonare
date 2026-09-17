"""Tests for the band vocabulary's two ceilings.

Both functions answer "how high can this capture still be believed", and they
answer it from different evidence — one reference's ability to tell its own
instruments apart, and two references' agreement about where those instruments
are. Neither implies the other, so each needs a font of data that trips it while
leaving the other clean, and `shared_band_edge` needs to be shown taking each.

The profiles are synthetic on purpose. A test built from `reference/drums.json`
would be scored against the very capture that motivated the check, and could not
distinguish a working detector from one tuned until that file came out right.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent))

from metrics_bands import (
    BAND_FLOOR_DB,
    OCTAVE_CENTERS,
    THIRD_OCTAVE_CENTERS,
    band_edge_index,
    measure_agreement_edge,
    measure_band_edge,
    shared_band_edge,
)
from metrics_hit import analyze_hit
from smf import Note

N_BANDS = len(THIRD_OCTAVE_CENTERS)
#: Enough instruments and velocities that a quartile is a quartile.
NOTES = tuple(range(35, 47))
VELOCITIES = (64, 100, 127)

#: Across-instrument variation, and how much one recording of an instrument
#: differs from another of the same one. Chosen so the difference between two
#: references has an IQR near the 12 dB the drum capture measures, which is what
#: the agreement ratio is taken against.
INSTRUMENT_SD_DB = 9.0
RECORDING_SD_DB = 6.3


def _identities(rng: np.random.Generator) -> list[np.ndarray]:
    """One band profile per instrument, shared by every reference of the kit."""
    return [rng.normal(0.0, INSTRUMENT_SD_DB, N_BANDS) for _ in NOTES]


def _reference(
    timbre: str,
    identities: list[np.ndarray],
    rng: np.random.Generator,
    *,
    rolloff_from_hz: float | None = None,
    rolloff_db_per_band: float = 5.0,
    deaf_from_hz: float | None = None,
) -> list[dict]:
    """Rows for one reference recording of the kit.

    `rolloff_from_hz` gives it a chain that darkens every instrument equally
    above that band — the thing only an agreement test can see, since each
    instrument still sits where it did relative to the others.
    `deaf_from_hz` collapses the instruments onto one another instead, which is
    what `measure_band_edge` was written for.

    The default slope is gentle enough that the darkened bands stay off the
    floor. A steeper one censors them, which is a second defect on top of the
    one under test and would let these pass for the wrong reason.
    """
    rows = []
    for note, identity in zip(NOTES, identities):
        for velocity in VELOCITIES:
            profile = identity + rng.normal(0.0, RECORDING_SD_DB, N_BANDS)
            for i, centre in enumerate(THIRD_OCTAVE_CENTERS):
                if deaf_from_hz is not None and centre > deaf_from_hz:
                    profile[i] = -20.0
                elif rolloff_from_hz is not None and centre > rolloff_from_hz:
                    steps = sum(1 for c in THIRD_OCTAVE_CENTERS
                                if rolloff_from_hz < c <= centre)
                    profile[i] -= rolloff_db_per_band * steps
            rows.append({"timbre": timbre, "note": note, "velocity": velocity,
                         "bands_db": [float(max(v, BAND_FLOOR_DB)) for v in profile]})
    return rows


@pytest.fixture
def rng() -> np.random.Generator:
    return np.random.default_rng(20260918)


# --------------------------------------------------------------------------
# Discrimination: one reference, against itself.


def test_a_reference_that_separates_its_whole_range_has_no_ceiling(rng):
    assert measure_band_edge(_reference("kit", _identities(rng), rng)) is None


def test_a_reference_whose_top_bands_stop_separating_reports_where(rng):
    rows = _reference("kit", _identities(rng), rng, deaf_from_hz=4000.0)
    assert measure_band_edge(rows) == 4000.0


def test_too_few_rows_is_unjudgeable_rather_than_unlimited(rng):
    rows = _reference("kit", _identities(rng), rng)[:4]
    assert measure_band_edge(rows) is None


# --------------------------------------------------------------------------
# Agreement: two references, against each other.


def test_one_reference_cannot_be_asked_whether_it_agrees(rng):
    """The honest answer, and the reason most captures get no agreement edge."""
    assert measure_agreement_edge(_reference("kit", _identities(rng), rng)) is None


def test_two_references_of_the_same_kit_agree_to_the_top(rng):
    ids = _identities(rng)
    rows = _reference("kit-a", ids, rng) + _reference("kit-b", ids, rng)
    assert measure_agreement_edge(rows) is None


def _chained(ids, slope: float) -> list[dict]:
    """Two references of one kit, one of them darkened above 4 kHz."""
    return (_reference("kit-a", ids, np.random.default_rng(7),
                       rolloff_from_hz=4000.0, rolloff_db_per_band=slope)
            + _reference("kit-b", ids, np.random.default_rng(8)))


def test_a_band_limited_reference_is_caught_while_it_still_discriminates(rng):
    """The drum capture's shape: one reference darkened above a corner.

    Every instrument keeps its place relative to the others, so the spread test
    passes on both sides — this is the case that was reaching the fit.
    """
    ids = _identities(rng)
    rows = _chained(ids, 8.0)
    for timbre in ("kit-a", "kit-b"):
        assert measure_band_edge([r for r in rows if r["timbre"] == timbre]) is None
    assert measure_agreement_edge(rows) == 4000.0


def test_the_edge_falls_where_the_chain_overtakes_the_kit_not_where_it_starts(rng):
    """A gentle chain is still evidence for a while, and the edge says so.

    Worth pinning because the obvious expectation — the edge is the corner — is
    wrong, and a test asserting it would have to be silenced by weakening the
    ratio rather than by reading what it means.
    """
    ids = _identities(rng)
    edges = [measure_agreement_edge(_chained(ids, slope)) for slope in (3.0, 5.0, 8.0)]
    assert edges == [8000.0, 6300.0, 4000.0]


def test_one_disagreeing_band_low_down_is_not_a_ceiling(rng):
    """A ceiling is where a roll-off starts, so it is the highest contiguous band.

    Without this a resonance at 1 kHz would throw away everything above it. The
    real capture has exactly such a band, at a ratio of 1.02.
    """
    ids = _identities(rng)
    rows = _reference("kit-a", ids, rng) + _reference("kit-b", ids, rng)
    at = THIRD_OCTAVE_CENTERS.index(1000.0)
    for row in rows:
        if row["timbre"] == "kit-b":
            row["bands_db"][at] -= 40.0
    assert measure_agreement_edge(rows) is None


def test_references_that_share_no_band_report_the_bottom_not_the_absence(rng):
    """`None` means unjudgeable everywhere else, so it must not mean this.

    Built directly rather than through a chain, because a capture whose two
    references agree nowhere is not a roll-off — it is two different
    instruments, and nothing shaped like a recording produces it.
    """
    rows = []
    for timbre, offset in (("kit-a", 0.0), ("kit-b", -40.0)):
        for note in NOTES:
            for velocity in VELOCITIES:
                rows.append({
                    "timbre": timbre, "note": note, "velocity": velocity,
                    "bands_db": [offset + 0.1 * note] * N_BANDS})
    assert measure_agreement_edge(rows) == THIRD_OCTAVE_CENTERS[0]


def test_a_band_floored_on_one_side_does_not_soften_the_verdict(rng):
    """Dropping those cells censors the disagreement towards zero.

    So the edge can only be placed too high by it, never too low — asserted by
    making the same chain steeper, which floors more cells and must not move the
    answer back up.
    """
    ids = _identities(rng)
    steep = (_reference("kit-a", ids, rng, rolloff_from_hz=4000.0,
                        rolloff_db_per_band=40.0)
             + _reference("kit-b", ids, rng))
    assert measure_agreement_edge(steep) <= 4000.0


# --------------------------------------------------------------------------
# The two combined.


def test_the_narrower_of_the_two_tests_sets_the_shared_edge(rng):
    """Each reference discriminates to the top; they stop agreeing at 4 kHz.

    The pair that matters: before the agreement test existed this capture
    reported no ceiling at all, and every band above 4 kHz was scored as if it
    held a target.
    """
    ids = _identities(rng)
    rows = _chained(ids, 8.0)
    assert all(e is None for e in
               (measure_band_edge([r for r in rows if r["timbre"] == t])
                for t in ("kit-a", "kit-b")))
    assert shared_band_edge(rows) == 4000.0


def test_a_deaf_reference_still_sets_the_edge_when_they_agree(rng):
    """The other direction, so the fold cannot be satisfied by one test alone."""
    ids = _identities(rng)
    rows = (_reference("kit-a", ids, rng, deaf_from_hz=2000.0)
            + _reference("kit-b", ids, rng, deaf_from_hz=2000.0))
    assert measure_agreement_edge(rows) is None
    assert shared_band_edge(rows) == 2000.0


def test_the_edge_index_counts_the_bands_at_or_below_it():
    assert band_edge_index(None) == N_BANDS
    assert band_edge_index(4000.0) == THIRD_OCTAVE_CENTERS.index(4000.0) + 1
    assert band_edge_index(THIRD_OCTAVE_CENTERS[-1]) == N_BANDS


def test_the_edge_index_cuts_the_octave_bands_at_the_same_place():
    assert band_edge_index(None, OCTAVE_CENTERS) == len(OCTAVE_CENTERS)
    assert band_edge_index(5000.0, OCTAVE_CENTERS) == OCTAVE_CENTERS.index(4000.0) + 1


# --------------------------------------------------------------------------
# What the edge reaches inside one hit.


def _bright_hit(sr: int = 48000, seconds: float = 0.6) -> np.ndarray:
    """A decaying strike with most of its energy above 5 kHz.

    A cymbal's shape as far as this matters: if the edge did not reach a field,
    that field reads the 9 kHz content and moves when the edge does not.
    """
    t = np.arange(int(seconds * sr)) / sr
    body = np.sin(2 * np.pi * 900.0 * t) * np.exp(-t / 0.25)
    wash = np.sin(2 * np.pi * 9000.0 * t) * np.exp(-t / 0.08) * 4.0
    return (body + wash).astype(np.float64)


@pytest.mark.parametrize("field", ["bands_db", "band_decay_db_s", "centroid_hz"])
def test_every_field_a_comparison_reads_is_cut_at_the_edge(field):
    """The guard has three siblings and had been on one of them.

    Parametrised rather than asserted together so a field that stops being cut
    fails by name — the failure this is written against is one of the three
    silently keeping its full range while the other two shrink.
    """
    hit = _bright_hit()
    note = Note(49, 100, 0.0, 0.05)
    wide = analyze_hit(hit, 48000, note, 0.6)
    cut = analyze_hit(hit, 48000, note, 0.6, max_band_hz=5000.0)
    assert getattr(wide, field) != getattr(cut, field)


def test_a_capture_with_no_edge_is_measured_exactly_as_before():
    """The other half: `None` must leave every field at its full range."""
    hit = _bright_hit()
    note = Note(49, 100, 0.0, 0.05)
    assert (analyze_hit(hit, 48000, note, 0.6).to_dict()
            == analyze_hit(hit, 48000, note, 0.6, max_band_hz=None).to_dict())


def test_the_octave_decay_reports_a_cut_band_as_unmeasured_not_as_zero():
    """`None` is what every reader already skips; a number there would be scored."""
    cut = analyze_hit(_bright_hit(), 48000, Note(49, 100, 0.0, 0.05), 0.6,
                      max_band_hz=5000.0)
    keep = band_edge_index(5000.0, OCTAVE_CENTERS)
    assert all(v is None for v in cut.band_decay_db_s[keep:])
    assert any(v is not None for v in cut.band_decay_db_s[:keep])
