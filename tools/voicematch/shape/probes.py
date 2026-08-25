"""Diagnostics: measurements that answer a question rather than drive a search.

A loss term has to be safe to minimise, which costs it resolution -- it is
clipped, weighted, floored and pooled. These are not. Each one is a plain
statement about a render that a person can check, and they exist because the ear
kept reporting defects the loss was structurally unable to see. Every one of
them started as a complaint in words.

    tail_residue    "a bell-like ring is left"       -- energy in a band, late,
                    that the struck string cannot account for
    sustain_colour  "it sounds metallic"             -- the balance between the
                    low partials and the middle ones, a second and a half in
    decay_profile   "it rings too long"              -- decay rate as a function
                    of partial frequency, which is the slope metal has and wood
                    does not
    onset_profile   "the attack has no richness"     -- band level and rise time
                    through the first 350 ms

Where a probe compares against the reference it removes the reference's recorded
floor first and refuses partials that have fallen into it. A probe that skips
unusable data points and averages the rest scores an empty set as perfect, which
is a shape this harness has already been caught in once.
"""

from __future__ import annotations

import numpy as np

from . import terms
from .partials import Track, decay_db_s, level_db, partial_hz

#: Band the ear reads as metallic when it rings on.
METAL_BAND = (800.0, 6000.0)
METAL_TAIL = (2.5, 6.5)
METAL_EARLY = (0.2, 0.6)

COLOUR_T = 1.5
COLOUR_LOW = (1, 2)
COLOUR_MID = (3, 10)

DECAY_WINDOW = (0.5, 3.5)
DECAY_BINS = ((60, 250), (250, 700), (700, 2000), (2000, 5000), (5000, 15500))


def tail_residue(spectro, sig, note, bed=None, band=METAL_BAND,
                 tail=METAL_TAIL, early=METAL_EARLY) -> float:
    """Late residue inside a band, relative to the note's own early level, in dB.

    The loss has a residue term already and it does not catch this, for three
    reasons this measure removes. That term is broadband, so a ring at two
    kilohertz is averaged against the low end where most residue energy lives
    whatever the render does. It is skipped once the reference's partials have
    fallen into the recorded floor, which in the treble is most of the tail --
    so a model that rings for six seconds is unscored exactly where it is worst.
    And the invariance term takes an across-note minimum, which is the right
    shape for a resonator that answers every note and blind to one that answers
    half of them.

    Normalising by the note's own early harmonic level rather than by the
    concurrent level is what keeps the number meaningful after the reference has
    decayed: the denominator is how loud the note was, not how loud its floor is.
    """
    from .partials import fit_inharmonicity, harmonic_rows, note_hz
    G = spectro(sig)[0]
    if bed is not None:
        G, _ = bed.clean(spectro, G, 0)
    f0 = note_hz(note)
    B = fit_inharmonicity(sig, f0, spectro.sample_rate)
    hm = harmonic_rows(spectro.rows_hz(0), f0, B)
    rows = spectro.rows_hz(0)
    inband = (rows >= band[0]) & (rows < band[1])
    P = 10.0 ** (G / 10.0)
    e = float(P[:, spectro.columns(0, G.shape[1], *early)].mean(axis=1)[hm].sum())
    t = float(P[:, spectro.columns(0, G.shape[1], *tail)].mean(axis=1)[~hm & inband].sum())
    return 10 * np.log10(max(t, 1e-30) / max(e, 1e-30))


def sustain_colour(track: Track, sig: np.ndarray, t: float = COLOUR_T) -> float | None:
    """Partials 3-10 minus partials 1-2 at a fixed time, in dB.

    A struck bass string a second and a half in is nearly all fundamental and
    octave. If partials three to ten are still within a few decibels of them the
    note reads as hard and metallic even though every frequency in it belongs to
    the note -- which is why a residue measure can come back clean while the ear
    reports metal.
    """
    lv = {k: level_db(track.envelope(sig, k), t, sr=track.sr) for k, _ in track.ks}
    low = [lv[k] for k in range(COLOUR_LOW[0], COLOUR_LOW[1] + 1) if k in lv]
    mid = [lv[k] for k in range(COLOUR_MID[0], COLOUR_MID[1] + 1) if k in lv]
    if not low or not mid:
        return None
    return float(10 * np.log10(np.mean(10 ** (np.array(mid) / 10.0)))
                 - 10 * np.log10(np.mean(10 ** (np.array(low) / 10.0))))


def decay_profile(track: Track, sig: np.ndarray, window=DECAY_WINDOW,
                  guard: bool = True):
    """[(frequency, rate in dB/s)] for every partial usable on the reference.

    `guard` withholds a partial whose reference envelope has fallen into the
    local floor. Withheld on the reference and not on the model, deliberately:
    the question is what the instrument does there, and if the capture cannot
    say, no answer from the model is scoreable either.
    """
    out = []
    for k, _ in track.ks:
        if guard and not track.clear(k, window[1] - 0.3):
            continue
        r = decay_db_s(track.envelope(sig, k), *window, sr=track.sr)
        if r is not None:
            out.append((partial_hz(track.f0, track.B, k), r))
    return out


def decay_bins(profiles, bins=DECAY_BINS):
    """Mean rate and sample count per frequency band, pooled over notes.

    Takes a list of profiles rather than one, because the slope only means
    anything across a span of frequencies and a single note rarely covers one.
    """
    flat = [p for prof in profiles for p in prof]
    out = {}
    for lo, hi in bins:
        vals = [r for f, r in flat if lo <= f < hi]
        out[(lo, hi)] = (float(np.mean(vals)), len(vals)) if vals else (None, 0)
    return out


def onset_profile(sig: np.ndarray, sr: int = 48000, start: float = 0.1):
    """(band level over the first 60 ms, time to rise) per band, both arrays."""
    return terms.onset_stats(sig, sr, start)


#: Octave bands for the balance probe, from the bottom of a keyboard upward.
BALANCE_BANDS = ((30, 60), (60, 125), (125, 250), (250, 500), (500, 1000),
                 (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000))


def band_balance(spectro, sig, window, bands=BALANCE_BANDS):
    """Each band's share of the render's energy in a window, in dB.

    Expressed against the render's own total rather than against an absolute
    level, so two renders are comparable without aligning a gain first -- which
    matters because the question "where is the weight" is a question about
    balance and survives any amount of overall loudness error. A model that is
    thirteen decibels loud and still thin reads correctly here and does not read
    correctly on any absolute measure.
    """
    G = spectro(sig)[0]
    hz = spectro.rows_hz(0)
    col = (10.0 ** (G / 10.0))[:, spectro.columns(0, G.shape[1], *window)].mean(axis=1)
    total = max(float(col.sum()), 1e-30)
    return {b: 10 * np.log10(max(float(col[(hz >= b[0]) & (hz < b[1])].sum()), 1e-30)
                             / total) for b in bands}


def partial_census(track: Track, sig: np.ndarray, t: float = 1.0,
                   floor_db: float = 60.0, k_max: int = 40):
    """How many of the note's partials are still audible, and how high they go.

    Richness is partly just count. A tone carrying eight partials and one
    carrying thirty can have the same spectral centroid, the same band balance
    and the same decay, and only one of them sounds like an instrument.

    The floor is relative to the note's own strongest partial at the same
    instant, so this says nothing about level and everything about how far the
    series reaches before it disappears.
    """
    from .partials import partial_hz
    lv = {}
    for k in range(1, k_max + 1):
        f = partial_hz(track.f0, track.B, k)
        if f > 15500.0:
            break
        lv[k] = level_db(track.envelope(sig, k), t, sr=track.sr)
    if not lv:
        return 0, 0
    top = max(lv.values())
    clear = [k for k, v in lv.items() if v > top - floor_db]
    return len(clear), (max(clear) if clear else 0)
