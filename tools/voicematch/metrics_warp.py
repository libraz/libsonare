"""The note re-read on the clock its fundamental keeps, and when that reading is kept.

Under a vibrato every partial is a comb of FM lines, and the same pitch track moves
all of them by the same cents, so one time warp along the fundamental's phase stills
the whole series. That is what tone-to-noise, the valleys and the stiffness fit
should read. It is kept only where it collapses the lines' widths: a unison or an
ensemble has no single pitch track, the warp smears its lines instead, and a fit
read through it finds a stiffness the strings do not have. The same rule decides
both readings, so a row never takes one from the warped signal and the other from
the raw one.
"""

from __future__ import annotations

from typing import NamedTuple

import numpy as np
from metrics_modulation import Vibrato
from metrics_partials import fm_margin_hz

#: A Hann window's width at 20 dB under its peak, in bins (1/T each): 3.22/T, read off a pure tone.
HANN_W20_BINS = 3.22
#: Partials whose line widths are read (2..4): the fundamental sits among the body's own motion.
WIDTH_PARTIALS = (2, 3, 4)
#: The line region either side of a partial, at most this many Hz and half the fundamental.
WIDTH_REGION_HZ = 30.0
#: Mean width after the warp over before it that counts as collapsed. Over 693 vibrato rows the
#: 51 the warp made newly stiff read 0.82-2.1; a 50 c vibrato 0.04-0.38; a chorus or a detuned pair 1 or more.
COLLAPSE_RATIO = 0.75
#: Mean line width over a still tone's from which a note's lines are broad. Over 1.5 s a pair
#: detuned 6-8 c reads 3.1-3.8, a two-voice chorus 3.7-10.7, a steady tone or one with noise 1.
BROAD_RATIO = 2.5
#: Samples per input sample of the oversampled signal the warp reads from.
OVERSAMPLE = 4


class LineStructure(NamedTuple):
    """What the lines of one note's partials say about its voices.

    `width_ratio` is None where there was no pitch motion to warp by or the warp was
    refused. `unwarped` is the segment on the fundamental's clock, set only where the
    warp collapsed the widths; every other reading stays on the raw signal.
    """

    broad_ratio: float | None
    width_ratio: float | None
    unwarped: np.ndarray | None
    broad: bool


def _phase_track(seg: np.ndarray, sr: int, f0: float, cutoff: float, stop: float) -> np.ndarray:
    """The fundamental's phase, in radians, around `f0`, passed under `cutoff` (rolled off to `stop`)."""
    x = np.asarray(seg, dtype=np.float64)
    t = np.arange(len(x)) / sr
    spec = np.fft.fft(x * np.exp(-2j * np.pi * f0 * t))
    away = np.abs(np.fft.fftfreq(len(x), 1.0 / sr))
    spec *= 0.5 * (1.0 + np.cos(np.pi * np.clip((away - cutoff) / (stop - cutoff), 0.0, 1.0)))
    return np.unwrap(np.angle(np.fft.ifft(spec)))


def _cubic(y: np.ndarray, pos: np.ndarray) -> np.ndarray:
    """Catmull-Rom read of `y` at fractional positions `pos`."""
    i = np.clip(np.floor(pos).astype(np.int64), 1, len(y) - 3)
    u = pos - i
    a, b, c, d = y[i - 1], y[i], y[i + 1], y[i + 2]
    return b + 0.5 * u * (c - a + u * (2 * a - 5 * b + 4 * c - d + u * (3 * (b - c) + d - a)))


def unwarp(seg: np.ndarray, sr: int, f0: float, motion: Vibrato) -> np.ndarray | None:
    """`seg` re-read on the clock its fundamental keeps, or None where that clock does not run forward.

    The fundamental's phase deviation from `f0` is read under its FM extent, and
    the signal resampled so the phase advances at a constant rate. Partial n moves
    n times the fundamental's cents, so every partial stands still; the series
    keeps the mean pitch it had.
    """
    bin_hz = sr / len(seg)
    cutoff = min(fm_margin_hz(f0, f0, bin_hz, motion), 0.25 * f0)
    phi = _phase_track(seg, sr, f0, cutoff, min(2.0 * cutoff, 0.5 * f0))
    n = len(seg)
    t = np.arange(n, dtype=np.float64)
    tau = t + phi * sr / (2.0 * np.pi * f0)
    tau -= tau[0]
    if np.any(np.diff(tau) <= 0.0):
        return None
    src = np.interp(np.linspace(0.0, tau[-1], n), tau, t)
    up = np.fft.irfft(np.fft.rfft(np.asarray(seg, dtype=np.float64)), n * OVERSAMPLE) * OVERSAMPLE
    return _cubic(up, src * OVERSAMPLE)


def line_widths(seg: np.ndarray, sr: int, f0: float) -> list[float]:
    """Width in Hz at 20 dB under the peak of each partial in `WIDTH_PARTIALS`, over one Hann FFT.

    The peak is the strongest bin within 0.3 f0 of k·f0, so a series stretched by
    stiffness is found at the low orders read here; the region is padded 8x.
    """
    n = len(seg)
    nf = 1 << int(np.ceil(np.log2(n * 8)))
    power = np.abs(np.fft.rfft(np.asarray(seg, dtype=np.float64) * np.hanning(n), nf)) ** 2
    level = 10.0 * np.log10(power + 1e-300)
    df = sr / nf
    reach = min(0.5 * f0, WIDTH_REGION_HZ)
    out: list[float] = []
    for k in WIDTH_PARTIALS:
        if (k + 0.3) * f0 >= 0.45 * sr:
            break
        a, b = int((k - 0.3) * f0 / df), int((k + 0.3) * f0 / df)
        peak = a + int(np.argmax(power[a : b + 1]))
        lo, hi = int((peak * df - reach) / df), int((peak * df + reach) / df)
        inside = np.nonzero(level[lo : hi + 1] >= level[peak] - 20.0)[0]
        out.append(float(inside[-1] - inside[0]) * df)
    return out


def line_structure(seg: np.ndarray, sr: int, f0: float, motion: Vibrato | None) -> LineStructure:
    """The line widths of `seg`, and its warp where the warp collapses them.

    `f0` is the fundamental's own frequency (the peak near the key's pitch), not
    a fit's, so the reading comes before the fit and does not depend on it.
    `broad` is a note whose lines are broader than a still tone's by `BROAD_RATIO` and
    which the warp does not collapse: several voices, or a vibrato the warp cannot follow.
    """
    raw = line_widths(seg, sr, f0)
    if not raw:
        return LineStructure(None, None, None, False)
    lobe = HANN_W20_BINS * sr / len(seg)
    broad = float(np.mean(raw)) / lobe
    warped = unwarp(seg, sr, f0, motion) if motion is not None else None
    if warped is None:
        return LineStructure(broad, None, None, broad >= BROAD_RATIO)
    after = line_widths(warped, sr, f0)
    ratio = float(np.mean(after)) / float(np.mean(raw)) if after else None
    kept = ratio is not None and ratio <= COLLAPSE_RATIO
    return LineStructure(broad, ratio, warped if kept else None, broad >= BROAD_RATIO and not kept)
