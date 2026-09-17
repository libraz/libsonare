"""The 1/3-octave and octave band vocabulary, and the scalars read off it."""

from __future__ import annotations

import itertools

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


#: How far below its own loudest band a band still counts as present. The floor
#: keeps bands with no content on either side from reading as a large difference
#: between two noise floors.
BAND_FLOOR_DB = -60.0

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


#: How far the systematic offset between two references may stand over their
#: across-instrument scatter before a band is reporting the recordings rather
#: than the instruments. One: past it what the two captures share outweighs what
#: separates the objects they recorded. The drum capture answers 5 kHz anywhere
#: in 0.79-1.27, so that answer does not sit on this number.
BAND_AGREEMENT_MAX_RATIO = 1.0


def _pair_agreement_edge(left: dict, right: dict) -> float | None:
    """The highest band two references agree in, or `None` if unjudgeable."""
    shared = [cell for cell in left if cell in right]
    if len(shared) < BAND_EDGE_MIN_ROWS:
        return None
    a = np.asarray([left[c] for c in shared], dtype=np.float64)
    b = np.asarray([right[c] for c in shared], dtype=np.float64)
    for i in range(len(THIRD_OCTAVE_CENTERS) - 1, -1, -1):
        # A cell floored on either side carries no reading to difference. They
        # are dropped rather than differenced against the floor, which censors
        # the disagreement towards zero — so this understates and can only
        # place the edge too high.
        live = (a[:, i] > BAND_FLOOR_DB) & (b[:, i] > BAND_FLOOR_DB)
        if int(live.sum()) < BAND_EDGE_MIN_ROWS:
            continue
        lo, mid, hi = np.percentile(a[live, i] - b[live, i], (25, 50, 75))
        # Multiplied rather than divided so a band the references read
        # identically — zero shift over zero scatter — is the agreement it is,
        # instead of a division that has to be special-cased into one.
        if abs(float(mid)) > BAND_AGREEMENT_MAX_RATIO * float(hi - lo):
            continue
        if i == len(THIRD_OCTAVE_CENTERS) - 1:
            return None
        return float(THIRD_OCTAVE_CENTERS[i])
    # Nothing agreed anywhere. Reported as the bottom of the vocabulary rather
    # than as "no ceiling", because two references that share no band are a
    # capture that has not been shown to measure one instrument.
    return float(THIRD_OCTAVE_CENTERS[0])


def measure_agreement_edge(rows: list[dict]) -> float | None:
    """The highest 1/3-octave band this capture's references still agree in, in Hz.

    `measure_band_edge` asks whether a band separates one instrument from the
    next. That is necessary and it is not sufficient: a reference can separate
    its own instruments perfectly while sitting tens of dB away from where they
    actually are, and every instrument being wrong by the same amount is exactly
    what a recording chain does. A band like that passes the spread test and
    carries no usable target, so the model is fitted to the chain.

    The separator is which part of the difference the references share. Two
    recordings of the same instrument differ for two reasons — they are
    different instruments, which scatters the difference from piece to piece,
    and they are different chains, which shifts all of it one way. The scatter
    is the across-instrument IQR and the shift is the median, so a band where
    the median stands over the IQR is reporting the chains.

    Measured on the two-kit drum capture: the median offset wanders between -9
    and +6 dB from 50 Hz to 4 kHz against an IQR held near 12, then runs to
    -11.4, -17.5 and -24.4 dB at 5, 6.3 and 8 kHz while the IQR does not move.
    One reference is band-limited to about 11 kHz — its content at 14 kHz sits
    120 dB under its own peak, which is removal rather than roll-off — and the
    bands below that ceiling carry its anti-alias skirt as if it were the kit.

    Walked from the top like `measure_band_edge`, and for the same reason: the
    edge is where a roll-off begins, so a lone disagreeing band low down is a
    resonance rather than a ceiling. `None` when there is no second reference to
    compare against, which is most captures — an agreement test needs two
    recordings and cannot be faked from one.
    """
    by_timbre: dict[str, dict[tuple, np.ndarray]] = {}
    for row in rows or []:
        profile = row.get("bands_db")
        if not profile or len(profile) != len(THIRD_OCTAVE_CENTERS):
            continue
        cell = (row.get("note"), row.get("velocity"))
        by_timbre.setdefault(str(row.get("timbre", "")), {})[cell] = np.asarray(
            profile, dtype=np.float64)
    if len(by_timbre) < 2:
        return None
    edges = [_pair_agreement_edge(by_timbre[x], by_timbre[y])
             for x, y in itertools.combinations(sorted(by_timbre), 2)]
    known = [e for e in edges if e is not None]
    return min(known) if known else None


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

    Two things end a band's usefulness and this is where they are combined: a
    reference that cannot tell its instruments apart up there
    (`measure_band_edge`, one per reference) and references that can but do not
    agree about where the instruments are (`measure_agreement_edge`, across
    them). Neither implies the other — the drum capture's references each
    discriminate to 8 kHz and stop agreeing at 5 — so the second cannot be left
    to the first, and a capture carrying only one reference is judged by the
    first alone.
    """
    known = [e for e in band_edges_by_timbre(rows).values() if e is not None]
    agreement = measure_agreement_edge(rows)
    if agreement is not None:
        known.append(agreement)
    return min(known) if known else None


def band_edge_index(max_band_hz: float | None,
                    centres: tuple[float, ...] = THIRD_OCTAVE_CENTERS) -> int:
    """How many of `centres` sit at or below `max_band_hz`.

    Takes the centre list so the octave decay bands are cut at the same place as
    the 1/3-octave profile. A per-band measurement above the edge is the chain
    whichever resolution it was taken at, and the two reading different ranges
    would put a decay in the answer that the profile it sits beside excludes.
    """
    if max_band_hz is None:
        return len(centres)
    return sum(1 for c in centres if c <= max_band_hz + 1e-6)
