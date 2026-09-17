"""The 1/3-octave and octave band vocabulary, and the scalars read off it."""

from __future__ import annotations

import numpy as np

# ISO 1/3-octave centres, 50 Hz to 12.5 kHz: the resolution a percussion hit's
# spectrum is worth reporting at. Finer would resolve individual modes, which
# move with every knob and are not what a fit should chase; coarser would merge
# a snare's shell into its wires.
THIRD_OCTAVE_CENTERS = (
    50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0,
    800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0,
    8000.0, 10000.0, 12500.0,
)
THIRD_OCTAVE_RATIO = 2.0 ** (1.0 / 6.0)

# Decay is fit per octave band rather than per third-octave: a third-octave
# band of a noisy hit carries too few modes for a slope fit to be stable.
OCTAVE_CENTERS = (63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0)
OCTAVE_RATIO = 2.0 ** 0.5


#: The two ends of the 1/3-octave profile a hit's tilt is taken between. The
#: middle is left out on purpose: a kit's body lives there and every piece of it
#: puts energy there, so including it averages the two ends towards each other
#: and the tilt stops separating a dull snare from a bright one.
TILT_LOW_HZ = 500.0
TILT_HIGH_HZ = 2000.0


def band_tilt_db(bands_db: list[float] | None) -> float | None:
    """How much of a hit sits above 2 kHz rather than below 500 Hz.

    `analyze_hit` normalises the band profile to its own loudest band, so a
    whole-spectrum level offset has already been divided out and the only thing
    left to compare is the shape. This is the one number of that shape a listener
    would name first: a kit piece is dull or bright before it is anything else.

    Here rather than beside the comparison that reads it, because the loss reads
    it too and the two must be one ruler. They were the same arithmetic written
    once; a fit scored against a tilt defined anywhere but where the gate's tilt
    is defined would optimise a quantity the gate does not measure.
    """
    if not bands_db:
        return None
    centres = np.asarray(THIRD_OCTAVE_CENTERS[:len(bands_db)], dtype=np.float64)
    vals = np.asarray(bands_db[:len(centres)], dtype=np.float64)
    low, high = vals[centres <= TILT_LOW_HZ], vals[centres >= TILT_HIGH_HZ]
    if not len(low) or not len(high):
        return None
    return float(np.mean(high) - np.mean(low))


#: How far a band's across-instrument spread may fall below the capture's own
#: typical spread and still be called informative. Half: a band that separates
#: the kit half as well as the capture does on average is degraded but is still
#: answering the question, and one that separates it less than that is mostly
#: reporting a shared transfer function.
BAND_EDGE_MIN_SPREAD_FRACTION = 0.5
#: Fewest distinct rows a spread can be read from. Two instruments differ or
#: they do not; it takes a handful before "how much do they differ" is a
#: measurement rather than a pair.
BAND_EDGE_MIN_ROWS = 8


def measure_band_edge(rows: list[dict]) -> float | None:
    """The highest 1/3-octave band a capture still tells instruments apart in, in Hz.

    A sampled reference has a bandwidth, and it is not the analysis range. What
    marks the end of it is not where the energy stops — a capture rolling off
    still has energy above its edge — but where the band stops DISCRIMINATING.
    Below the edge a crash, a cowbell, a closed hi-hat and a cabasa read tens of
    dB apart because they are different objects; at and above it they converge,
    because what is left is one shared transfer function rather than four
    instruments. Measured on `reference/drums.json`, the across-instrument
    spread holds between 21 and 30 dB from 63 Hz to 8 kHz, falls to 9 dB at
    10 kHz and to nothing at 12.5 kHz.

    An energy test cannot find that boundary. 57 % of that capture's rows are
    still above the band floor at 8 kHz and 36 % at 10 kHz, so a floor count
    puts the edge wherever the threshold was chosen; the spread collapses at one
    place and says so.

    Nothing above the edge is evidence about the instrument, so nothing above it
    should be scored or normalised against: a model with a real cymbal wash is
    charged the cap in bands the reference cannot resolve, and if the model's
    own loudest band lands up there, the profile it is normalised against shifts
    and every OTHER band's reading moves with it. The second one is why this
    cannot be left to the loss to skip.

    This reads a set of DIFFERENT instruments, which is what a percussion
    capture is. A pitched capture is one instrument at many notes, where a
    collapsing spread is a property of the register rather than of the chain, so
    the caller applies this to the percussion metric set only.

    Returned as a band centre so it can be compared against
    `THIRD_OCTAVE_CENTERS` without a tolerance. `None` when the capture carries
    its whole range, or when there are too few rows to read a spread from.
    """
    profiles = [r.get("bands_db") for r in rows or []]
    profiles = [p for p in profiles if p and len(p) == len(THIRD_OCTAVE_CENTERS)]
    if len(profiles) < BAND_EDGE_MIN_ROWS:
        return None
    columns = np.asarray(profiles, dtype=np.float64)
    spread = np.percentile(columns, 75, axis=0) - np.percentile(columns, 25, axis=0)
    # The capture's own typical separation, taken over every band rather than
    # over a hand-picked "good" region: picking the region would be choosing the
    # answer, since the region is what the edge is being measured against.
    floor = float(np.median(spread)) * BAND_EDGE_MIN_SPREAD_FRACTION
    if floor <= 0.0:
        return None
    # Walked from the top down and stopped at the first band that discriminates.
    # The edge is where a roll-off begins, so it is the highest CONTIGUOUS
    # informative band; a lone wide band above a collapsed one is a resonance in
    # the capture path, not a return of bandwidth.
    for i in range(len(THIRD_OCTAVE_CENTERS) - 1, -1, -1):
        if spread[i] >= floor:
            if i == len(THIRD_OCTAVE_CENTERS) - 1:
                return None
            return float(THIRD_OCTAVE_CENTERS[i])
    return None


def band_edges_by_timbre(rows: list[dict]) -> dict[str, float | None]:
    """Each reference's own measurable ceiling, one per timbre."""
    timbres = sorted({str(r.get("timbre", "")) for r in rows})
    return {t: measure_band_edge([r for r in rows if str(r.get("timbre", "")) == t])
            for t in timbres}


def shared_band_edge(rows: list[dict]) -> float | None:
    """The highest band EVERY reference in this capture can still resolve.

    A bandwidth belongs to a recording, not to a capture: two references from
    different products are two recording chains, and one of them can be wider.
    Measured over the pooled rows the wider one carries the spread past the
    narrower one's ceiling, which un-floors bands where the narrow reference has
    nothing but its own roll-off — the artefact `measure_band_edge` exists to
    prevent, arriving from the other side.

    That matters most for the thing the edge is for. Above the narrow
    reference's ceiling the spread BETWEEN references is its floor against the
    other's real value, so a gate written from it holds the model to a bound
    that the measurement manufactured, and passes anything up there. Measured on
    the two-kit drum capture: one kit's across-instrument spread collapses to
    0.0 dB at 12.5 kHz while the other holds 60.0, and pooling them removed the
    ceiling entirely.

    So the most restrictive ceiling wins, and a reference with no measurable one
    does not raise it. `None` throughout means no reference showed a ceiling.
    """
    known = [e for e in band_edges_by_timbre(rows).values() if e is not None]
    return min(known) if known else None


def band_edge_index(max_band_hz: float | None) -> int:
    """How many 1/3-octave bands sit at or below `max_band_hz`."""
    if max_band_hz is None:
        return len(THIRD_OCTAVE_CENTERS)
    return sum(1 for c in THIRD_OCTAVE_CENTERS if c <= max_band_hz + 1e-6)
