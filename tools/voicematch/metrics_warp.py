"""The note re-read on the clock its fundamental keeps, and when that reading is kept.

Under a vibrato every partial is a comb of FM lines, and the same pitch track moves
all of them by the same cents, so one time warp along the fundamental's phase stills
the whole series. That is what tone-to-noise, the valleys and the stiffness fit
should read. It is kept only where one source made the motion, that is where the partials'
frequency tracks follow the fundamental's (`keep_rule`): a unison or an ensemble has no
single pitch track, the warp smears its lines instead, and a fit read through it finds a
stiffness the strings do not have. Between the measured ranges the row is unresolved and takes
neither reading. The same rule decides every reading, so a row never takes one from the warped
signal and another from the raw one. The change in line widths is recorded as a diagnostic.
"""

from __future__ import annotations

from typing import NamedTuple

import numpy as np
from metrics_modulation import Vibrato
from metrics_motion import COHERENCE_PARTIALS, CoherenceState, coherence_state, partial_tracks
from metrics_partials import fm_margin_hz

#: A Hann window's width at 20 dB under its peak, in bins (1/T each): 3.22/T, read off a pure tone.
HANN_W20_BINS = 3.22
#: Partials whose line widths are read (2..4): the fundamental sits among the body's own motion.
WIDTH_PARTIALS = (2, 3, 4)
#: The line region either side of a partial, at most this many Hz and half the fundamental.
WIDTH_REGION_HZ = 30.0
#: Mean line width over a still tone's from which a note's lines are broad. Over 1.5 s a pair
#: detuned 6-8 c reads 3.1-3.8, a two-voice chorus 3.7-10.7, a steady tone or one with noise 1.
BROAD_RATIO = 2.5
#: Samples per input sample of the oversampled signal the warp reads from.
OVERSAMPLE = 4


class LineStructure(NamedTuple):
    """What the lines and tracks of one note's partials say about its voices.

    `width_ratio` (the warp's mean width after over before, a diagnostic) is None where there was
    no pitch motion to warp by or the warp was refused. `state`, `coherence` and `reason` are
    `keep_rule`'s answer, None where there was no motion or the series has no sixth partial to
    read. `unwarped` is the segment on the fundamental's clock, set only for a single source;
    an ensemble is read raw and an undecided row takes no reading at all.
    """

    broad_ratio: float | None
    width_ratio: float | None
    unwarped: np.ndarray | None
    broad: bool
    state: CoherenceState | None = None
    coherence: float | None = None
    reason: str | None = None


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


def keep_rule(
    seg: np.ndarray, sr: int, f0: float
) -> tuple[CoherenceState | None, float | None, str | None]:
    """Whether one source made the note's pitch motion -> (state, coherence, reason): the one condition.

    Read from the partials' own frequency tracks (`metrics_motion.coherence_state`). None where the
    series has no sixth partial under Nyquist to correlate with the fundamental.
    """
    orders = range(1, max(COHERENCE_PARTIALS) + 1)
    if (orders[-1] + 0.5) * f0 >= 0.45 * sr:
        return None, None, None
    return coherence_state(partial_tracks(seg, sr, f0, orders))


def line_structure(seg: np.ndarray, sr: int, f0: float, motion: Vibrato | None) -> LineStructure:
    """The line widths of `seg`, the keep rule's answer, and its warp where one source made the motion.

    `f0` is the fundamental's own frequency (the peak near the key's pitch), not
    a fit's, so the reading comes before the fit and does not depend on it.
    `broad` is a note whose lines are broader than a still tone's by `BROAD_RATIO` and
    which is not read warped: several voices, or a vibrato the rule cannot call one source's.
    """
    raw = line_widths(seg, sr, f0)
    if not raw:
        return LineStructure(None, None, None, False)
    lobe = HANN_W20_BINS * sr / len(seg)
    broad = float(np.mean(raw)) / lobe
    if motion is None:
        return LineStructure(broad, None, None, broad >= BROAD_RATIO)
    state, coherence, reason = keep_rule(seg, sr, f0)
    warped = unwarp(seg, sr, f0, motion)
    ratio = None
    if warped is not None:
        after = line_widths(warped, sr, f0)
        ratio = float(np.mean(after)) / float(np.mean(raw)) if after else None
    kept = warped if state is CoherenceState.SINGLE_SOURCE else None
    return LineStructure(
        broad, ratio, kept, broad >= BROAD_RATIO and kept is None, state, coherence, reason
    )
