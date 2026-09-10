"""Found-rather-than-predicted partials, for everything that is not a string."""

from __future__ import annotations

import numpy as np


# --------------------------------------------------------------------------- #
# Modal profile — the partial series of everything that is not a string
# --------------------------------------------------------------------------- #
# Why the harmonic ladder is not enough, and why this is not another window
# around a predicted place.
#
# Every pitched measurement above searches for partial n at n*f0*sqrt(1+B*n^2).
# That law describes a stiff string and nothing else. A bar, a bell, a plate or
# a membrane puts its partials at ratios no formula predicts and the model says
# so out loud: libsonare voices GM 8 at 1 : 2.756 : 5.404 : 8.933, GM 10 at
# 1 : 1.004 : 6.267 : 6.29 : 17.5, GM 14 at 0.5 : 1 : 2.76 : 5.4 : 8.9, and a
# timpano at 1 : 1.5 : 2 : 2.44. Not one of those ratios lands inside the
# ladder's +/-40 cent window around an integer multiple.
#
# What that produced was not an error. Measured on a synthesised celesta
# against a slightly differently tuned one, ten of the twelve ladder bins read
# the render's own noise floor on BOTH sides (-103 to -110 dB), the floor guard
# passed them because it tests for -120 rather than for the floor, and the
# harmonic term returned 15.09 — a confident number made almost entirely of the
# difference between two noise floors. A fit minimising it is shaping noise.
#
# So the partials are FOUND rather than predicted. The measurement is the same
# for a bell and for a violin; what changes is only that a violin's answer comes
# out at integer multiples.
#
# Frequencies are kept ABSOLUTE rather than as ratios to a detected anchor.
# Anchoring needs a mode chosen to anchor on, and the two obvious choices both
# fail: the lowest peak moves with the noise floor, and the strongest peak can
# be a different mode on the two sides, which shifts every ratio at once and
# reports one selection difference as a wholly retuned instrument. Both renders
# played the same MIDI note, so absolute cents is already the comparable
# quantity — and it prices mistuning, which a ratio cannot see at all.
MODAL_MAX_MODES = 12
#: How far under the strongest peak a peak still counts. Wide, because a bell's
#: upper modes are genuinely quiet and still audible; what it excludes is the
#: floor.
MODAL_FLOOR_DB = 55.0
#: How far above the locally smoothed spectrum a bin has to stand to be a mode
#: rather than a shoulder of one, and the span the baseline is smoothed over.
MODAL_PROMINENCE_DB = 8.0
MODAL_BASELINE_HZ = 400.0
#: Two peaks closer than this are one mode seen twice. A quarter tone: closer
#: than anything a bar's modes come, and wide enough to swallow the skirt a
#: Hann window puts either side of a partial.
MODAL_MERGE_CENTS = 50.0
#: Highest frequency worth reporting a mode at. Above this a sampled reference
#: is usually reporting its own capture bandwidth rather than the instrument.
MODAL_MAX_HZ = 12000.0


def measure_modes(freqs: np.ndarray, mag: np.ndarray, *,
                  n_max: int = MODAL_MAX_MODES,
                  floor_db: float = MODAL_FLOOR_DB) -> list[tuple[float, float]]:
    """The strongest isolated partials of a spectrum -> [(hz, db rel strongest)].

    Prominence against a locally smoothed baseline rather than a plain local
    maximum, so the skirt of a strong mode does not report as several weak ones
    and a dense low register does not report every ripple.

    Sorted by frequency, not by level, because the caller pairs them against
    another render's by frequency and a level ordering would only have to be
    undone. Returns fewer than `n_max` freely — a woodblock has two modes and
    padding the list would invent ten.
    """
    if len(freqs) < 8 or float(mag.max()) <= 0.0:
        return []
    db = 20.0 * np.log10(np.maximum(mag, 1e-12))
    bin_hz = float(freqs[1] - freqs[0])
    half = max(1, int(round(MODAL_BASELINE_HZ / bin_hz / 2.0)))
    kernel = np.ones(2 * half + 1) / (2 * half + 1)
    baseline = np.convolve(db, kernel, mode="same")
    top = float(db.max())
    cand: list[tuple[float, float]] = []
    hi = min(len(freqs) - 1, int(np.searchsorted(freqs, MODAL_MAX_HZ)))
    for i in range(1, hi):
        if db[i] < top - floor_db or db[i] - baseline[i] < MODAL_PROMINENCE_DB:
            continue
        if db[i] < db[i - 1] or db[i] < db[i + 1]:
            continue
        # Parabolic interpolation over log magnitude, the same refinement
        # `_peak_near` uses, so a mode is located to a fraction of a bin.
        a, b, c = db[i - 1], db[i], db[i + 1]
        denom = a - 2 * b + c
        delta = float(np.clip(0.5 * (a - c) / denom, -0.5, 0.5)) if abs(denom) > 1e-12 else 0.0
        cand.append((float(freqs[i] + delta * bin_hz), float(b)))
    if not cand:
        return []
    # Merge anything within a quarter tone, keeping the louder, then keep the
    # `n_max` loudest and hand them back in frequency order.
    cand.sort(key=lambda p: -p[1])
    kept: list[tuple[float, float]] = []
    for hz, level in cand:
        if any(abs(1200.0 * np.log2(hz / k[0])) < MODAL_MERGE_CENTS for k in kept):
            continue
        kept.append((hz, level))
        if len(kept) >= n_max:
            break
    kept.sort()
    peak = max(level for _, level in kept)
    return [(round(hz, 2), round(level - peak, 2)) for hz, level in kept]


def modal_profile(freqs: np.ndarray, mag: np.ndarray, expected_f0: float,
                  **kwargs) -> dict:
    """`measure_modes` as the fields a row carries.

    `modal_ratio` is against the note's nominal equal-tempered frequency rather
    than against a detected anchor: a fixed, known divisor cannot be chosen
    differently on the two sides, so the column is readable without being the
    thing the term compares.
    """
    modes = measure_modes(freqs, mag, **kwargs)
    return {
        "modal_hz": [hz for hz, _ in modes],
        "modal_db": [db for _, db in modes],
        "modal_ratio": [round(hz / expected_f0, 4) for hz, _ in modes]
        if expected_f0 > 0.0 else [],
    }
