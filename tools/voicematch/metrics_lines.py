"""Stationary lines a voice latches on at every note: found per capture, masked from the readings.

A recorded voice can carry narrow lines at fixed frequencies whatever note plays (the
concert grands': 10885.8, 10947.6, 17132.7, 17983.9, 18210.1 Hz and copies 3900 Hz
above them). They are the voice's own, not the recording room's floor, so they are
recorded per capture and masked from partial location and the band readings rather
than subtracted as a floor.
"""

from __future__ import annotations

from typing import NamedTuple

import numpy as np
from metrics_partials import MAX_FIT_PARTIALS, fit_partial_series, partial_hz

#: Reason a capture's rows carry for the lines they were read around.
STATIONARY_LINES_REASON = "stationary lines, voice-latched"
#: Lowest frequency a line is searched from; the grands' lowest is 10.9 kHz, the bank's 1.1 kHz.
LINE_MIN_HZ = 1000.0
#: Height over the running median of the spectrum, in dB, that makes a bin a line.
LINE_PROMINENCE_DB = 15.0
#: Half-width, in Hz, of the running median the prominence is taken against.
LINE_FLOOR_HALF_HZ = 200.0
#: A line this far under the segment's RMS is arithmetic residue, not a recorded line.
LINE_BELOW_RMS_DB = 110.0
#: Two takes' lines are one line within this many Hz; a line's spread over takes is 0.1-0.5 Hz.
LINE_MATCH_HZ = 0.4
#: Share of a capture's takes a line has to stand in, and the fewest takes that can say so.
LINE_SHARE = 0.75
LINE_MIN_TAKES = 5
#: Where in the gate the held part is read for lines: the late half, after the note's own partials fall.
LINE_WINDOW = (0.5, 0.95)
#: Bins either side of a line that are replaced, and the bins either side of those the replacement is taken from.
MASK_BINS = 4
MASK_AROUND_BINS = 40


class Line(NamedTuple):
    """A stationary line: where, and in how many of a capture's takes."""

    hz: float
    takes: int


def lines_in(seg: np.ndarray, sr: int) -> tuple[np.ndarray, np.ndarray]:
    """Narrow lines over `LINE_MIN_HZ` -> (Hz, level in dB of a full-scale sine's equivalent)."""
    w = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(np.asarray(seg, dtype=np.float64) * w)) * 2.0 / w.sum()
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    db = 20.0 * np.log10(np.maximum(mag, 1e-15))
    bin_hz = float(freqs[1])
    half = int(LINE_FLOOR_HALF_HZ / bin_hz)
    peaks = np.nonzero((db[1:-1] > db[:-2]) & (db[1:-1] >= db[2:]))[0] + 1
    peaks = peaks[(freqs[peaks] >= LINE_MIN_HZ) & (freqs[peaks] < 0.47 * sr)]
    step = max(1, half // 4)
    centres = np.arange(0, len(db), step)
    median = np.array([np.median(db[max(0, i - half) : i + half + 1]) for i in centres])
    floor = np.interp(peaks, centres, median)
    peaks = peaks[db[peaks] - floor >= LINE_PROMINENCE_DB]
    a, b, c = db[peaks - 1], db[peaks], db[peaks + 1]
    den = a - 2.0 * b + c
    delta = np.where(np.abs(den) > 1e-12, 0.5 * (a - c) / np.where(den == 0.0, 1.0, den), 0.0)
    return freqs[peaks] + delta * bin_hz, b - 0.25 * (a - c) * delta


def held_lines(
    seg: np.ndarray, sr: int, gate_s: float, preroll_s: float
) -> tuple[np.ndarray, np.ndarray] | None:
    """`lines_in` over a take's late held part, or None where there is nothing to read."""
    a, b = (int((preroll_s + f * gate_s) * sr) for f in LINE_WINDOW)
    held = seg[a:b]
    if len(held) < sr // 4 or not np.any(held):
        return None
    hz, level = lines_in(held, sr)
    rms = 20.0 * np.log10(np.sqrt(np.mean(np.asarray(held, dtype=np.float64) ** 2)) + 1e-30)
    keep = level > rms - LINE_BELOW_RMS_DB
    return hz[keep], level[keep]


def stationary_lines(takes: list[np.ndarray]) -> list[Line]:
    """Lines standing at one frequency in `LINE_SHARE` of the takes, none under `LINE_MIN_TAKES` takes.

    `takes` holds each take's line frequencies; a capture with fewer takes than
    `LINE_MIN_TAKES` cannot tell a voice's line from a note's partial and has none.
    """
    if len(takes) < LINE_MIN_TAKES:
        return []
    found = sorted((float(f), i) for i, hz in enumerate(takes) for f in hz)
    out: list[Line] = []
    j = 0
    while j < len(found):
        group = [found[j]]
        j += 1
        while j < len(found) and found[j][0] - group[0][0] <= 2.0 * LINE_MATCH_HZ:
            group.append(found[j])
            j += 1
        seen = {i for _, i in group}
        if len(seen) >= max(LINE_MIN_TAKES, LINE_SHARE * len(takes)):
            out.append(Line(round(float(np.median([f for f, _ in group])), 1), len(seen)))
    return out


def mask_lines(freqs: np.ndarray, mag: np.ndarray, lines_hz: list[float]) -> np.ndarray:
    """`mag` with each line's bins replaced by the median of the bins around them, or `mag` itself."""
    if not lines_hz or freqs.size < 2:
        return mag
    bin_hz = float(freqs[1] - freqs[0])
    out = np.array(mag, dtype=np.float64)
    for hz in lines_hz:
        k = round(hz / bin_hz)
        if k - MASK_AROUND_BINS < 0 or k + MASK_AROUND_BINS >= len(out):
            continue
        around = np.r_[
            mag[k - MASK_AROUND_BINS : k - MASK_BINS - 1],
            mag[k + MASK_BINS + 2 : k + MASK_AROUND_BINS + 1],
        ]
        out[k - MASK_BINS : k + MASK_BINS + 1] = float(np.median(around))
    return out


#: How many bins either side of a partial of the note's own series a stationary line is still taken
#: for that partial. An organ's line that is also a harmonic of every note a tritone apart sits on one bin.
LINE_ON_PARTIAL_BINS = 3.0


def lines_off_the_series(
    freqs: np.ndarray,
    mag: np.ndarray,
    f0_seed: float,
    h1_mag: float,
    sr: int,
    modal: bool,
    lines_hz: list[float],
) -> list[float]:
    """The stationary lines that are not also a partial of this note's own (unmasked) series."""
    if not lines_hz or modal:
        return lines_hz
    f0, b, _ = fit_partial_series(freqs, mag, f0_seed, h1_mag, sr)
    tol = LINE_ON_PARTIAL_BINS * float(freqs[1] - freqs[0])
    series = [partial_hz(f0, n, b) for n in range(1, MAX_FIT_PARTIALS + 1)]
    return [hz for hz in lines_hz if all(abs(hz - p) > tol for p in series)]
