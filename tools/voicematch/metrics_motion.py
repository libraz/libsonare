"""Pitch motion as one reading per recording, and whether one periodic motion made it.

A row's `fm` is `metrics_partials.pitch_motion`'s cents (peak to peak) and rate span.
The gate reads one figure per recording, so `recording_motion` reduces a timbre's rows
to one. It is gated only where one source made the motion (a player's vibrato, an LFO) and not where
a baked ensemble or chorus did, whose lines are several voices; a vibrato model cannot
reproduce the latter, so the dimension does not hold it. Unwarping the held note along the
fundamental's phase track collapses the partials' 20 dB widths for the former and leaves them
for the latter, and is recorded per row (`warp_reading`).
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from enum import Enum

import numpy as np

#: Passband of the phase track the note is unwarped along, as a fraction of f0; it stops at twice.
WARP_CUTOFF_F0_FRACTION = 0.2

#: Partials whose line widths are read, and the 1-based ones the ratio averages.
WIDTH_PARTIALS = 6
RATIO_PARTIALS = (2, 3, 4)

#: Partials whose frequency tracks are correlated with the fundamental's, and the edge share of
#: a track dropped. The median over them is a note's coherence.
COHERENCE_PARTIALS = range(2, 7)
TRACK_TRIM = 0.1

#: Coherence of the lowest real single-source note (violin 0.749) and of the highest ensemble (3-5
#: synthetic voices 0.391, real ensembles' recording medians 0.282); the margin is violin's per-note
#: standard deviation (0.065; oboe 0.003, a synthetic vibrato down to 1.5 c 0.000), the largest scatter.
LOWEST_SINGLE_SOURCE = 0.749
HIGHEST_ENSEMBLE = 0.391
COHERENCE_MARGIN = 0.065
SINGLE_SOURCE_FLOOR = LOWEST_SINGLE_SOURCE - COHERENCE_MARGIN
ENSEMBLE_CEILING = HIGHEST_ENSEMBLE + COHERENCE_MARGIN

#: A width is read within this many dB of the line's peak.
WIDTH_DB = 20.0

#: Zero-padding of the width spectrum, and the cubic resampler's oversampling of the unwarp.
PAD_FACTOR = 8
OVERSAMPLE = 4

#: A reading on fewer than this share of a recording's rows is not one recording's motion.
MIN_ROW_SHARE = 0.5

#: Dimension name the knob `reach` rules require for a motion field.
DIMENSION = "motion"


def _phase_track(seg: np.ndarray, sr: int, f0: float, cutoff: float, stop: float) -> np.ndarray:
    """The fundamental's unwrapped phase (radians) under a raised-cosine passband."""
    t = np.arange(len(seg)) / sr
    spec = np.fft.fft(seg * np.exp(-2j * np.pi * f0 * t))
    away = np.abs(np.fft.fftfreq(len(seg), 1.0 / sr))
    spec *= 0.5 * (1.0 + np.cos(np.pi * np.clip((away - cutoff) / (stop - cutoff), 0.0, 1.0)))
    return np.unwrap(np.angle(np.fft.ifft(spec)))


def _cubic(y: np.ndarray, pos: np.ndarray) -> np.ndarray:
    i = np.clip(np.floor(pos).astype(np.int64), 1, len(y) - 3)
    u = pos - i
    a, b, c, d = y[i - 1], y[i], y[i + 1], y[i + 2]
    return b + 0.5 * u * (c - a + u * (2 * a - 5 * b + 4 * c - d + u * (3 * (b - c) + d - a)))


def unwarp(seg: np.ndarray, sr: int, f0: float) -> np.ndarray | None:
    """`seg` resampled so the fundamental's phase advances uniformly, or None if it cannot be.

    None where the phase track's time map is not monotone: a beat or detune, which has no
    single pitch to take out.
    """
    x = np.asarray(seg, dtype=np.float64)
    cutoff = WARP_CUTOFF_F0_FRACTION * f0
    phi = _phase_track(x, sr, f0, cutoff, 2.0 * cutoff)
    n = len(x)
    t = np.arange(n, dtype=np.float64)
    tau = t + phi * sr / (2.0 * np.pi * f0)
    tau -= tau[0]
    if np.any(np.diff(tau) <= 0.0):
        return None
    src = np.interp(np.linspace(0.0, tau[-1], n), tau, t)
    up = np.fft.irfft(np.fft.rfft(x), n * OVERSAMPLE) * OVERSAMPLE
    return _cubic(up, src * OVERSAMPLE)


def line_widths(seg: np.ndarray, sr: int, f0: float) -> list[float]:
    """The 20 dB width in Hz of each of the first `WIDTH_PARTIALS` partials' lines.

    One Hann spectrum over the whole segment; each line is the strongest bin within 0.3 f0 of
    k*f0, read inside min(0.5 f0, 30 Hz) of it.
    """
    n = len(seg)
    nfft = 1 << int(np.ceil(np.log2(n * PAD_FACTOR)))
    power = np.abs(np.fft.rfft(np.asarray(seg, dtype=np.float64) * np.hanning(n), nfft)) ** 2
    db = 10.0 * np.log10(power + 1e-300)
    df = sr / nfft
    reach = min(0.5 * f0, 30.0)
    widths = []
    for k in range(1, WIDTH_PARTIALS + 1):
        lo, hi = int((k * f0 - 0.3 * f0) / df), int((k * f0 + 0.3 * f0) / df)
        peak = lo + int(np.argmax(power[lo : hi + 1]))
        a, b = int((peak * df - reach) / df), int((peak * df + reach) / df)
        inside = np.where(db[a : b + 1] >= db[peak] - WIDTH_DB)[0]
        widths.append(float((inside[-1] - inside[0]) * df))
    return widths


def warp_reading(seg: np.ndarray, sr: int, f0: float) -> dict:
    """How far unwarping along the fundamental's phase narrows the lines, as a row stores it.

    `w20_ratio` is the mean width of partials 2-4 after over before; `refused` names why there
    is none.
    """
    warped = unwarp(seg, sr, f0)
    if warped is None:
        return {"refused": "the fundamental's phase track is not monotone"}
    idx = [k - 1 for k in RATIO_PARTIALS]
    raw, flat = line_widths(seg, sr, f0), line_widths(warped, sr, f0)
    before = np.mean([raw[i] for i in idx])
    after = np.mean([flat[i] for i in idx])
    if before <= 0.0:
        return {"refused": "the lines are one bin wide before unwarping"}
    return {"w20_ratio": round(float(after / before), 3)}


class CoherenceState(str, Enum):
    """Whether the partials' frequency tracks say one source or several made the motion."""

    SINGLE_SOURCE = "single_source"
    ENSEMBLE = "ensemble"
    UNDECIDED = "undecided"


#: A partial's frequency track as `partial_tracks` returns it, by partial order.
Tracks = Mapping[int, np.ndarray]


def partial_tracks(
    seg: np.ndarray, sr: int, f0: float, orders=range(1, 7)
) -> dict[int, np.ndarray]:
    """Each partial's instantaneous frequency deviation divided by its order, mean removed.

    Read under the same raised-cosine passband as the unwarp, with `TRACK_TRIM` of each end dropped.
    """
    x = np.asarray(seg, dtype=np.float64)
    edge = int(TRACK_TRIM * len(x))
    cutoff = WARP_CUTOFF_F0_FRACTION * f0
    out = {}
    for k in orders:
        phi = _phase_track(x, sr, k * f0, cutoff, 2.0 * cutoff)
        dev = (np.gradient(phi) * sr / (2.0 * np.pi * k))[edge:-edge]
        out[k] = dev - dev.mean()
    return out


def track_coherence(tracks: Tracks) -> float:
    """Median over partials 2-6 of the correlation of their tracks with the fundamental's."""
    base = tracks[1]
    return float(
        np.median([np.corrcoef(tracks[k], base)[0, 1] for k in COHERENCE_PARTIALS if k in tracks])
    )


def partial_coherence(seg: np.ndarray, sr: int, f0: float) -> float:
    """`track_coherence` of `seg`'s partials: 1.0 for one source, little for independent voices.

    Partial k of a sum follows whichever voice dominates it, so voices do not move it together.
    """
    return track_coherence(partial_tracks(seg, sr, f0, range(1, max(COHERENCE_PARTIALS) + 1)))


def coherence_state(
    source: float | Tracks | Sequence[float | Tracks],
) -> tuple[CoherenceState, float, str]:
    """(state, coherence, reason) of one note or one recording, from its partials' frequency tracks.

    `source` is a note's tracks (`partial_tracks`), a note's coherence, or a sequence of either, one
    per note of a recording; a recording's coherence is the median of its notes'. At least
    `SINGLE_SOURCE_FLOOR` is `SINGLE_SOURCE`, at most `ENSEMBLE_CEILING` is `ENSEMBLE`, and between
    them `UNDECIDED`, which every caller treats as the costlier error's side: not a single source.
    """
    items = [source] if isinstance(source, (int, float, Mapping)) else list(source)
    if not items:
        raise ValueError("no note to read a coherence from")
    value = float(
        np.median([track_coherence(i) if isinstance(i, Mapping) else float(i) for i in items])
    )
    if value >= SINGLE_SOURCE_FLOOR:
        return (
            CoherenceState.SINGLE_SOURCE,
            value,
            f"coherence {value:.3f} at least {SINGLE_SOURCE_FLOOR:.3f}, a single source's",
        )
    if value <= ENSEMBLE_CEILING:
        why = "the partials' tracks do not follow the fundamental's, so the motion is not one source's"
        return (
            CoherenceState.ENSEMBLE,
            value,
            f"coherence {value:.3f} at most {ENSEMBLE_CEILING:.3f}: {why}",
        )
    return (
        CoherenceState.UNDECIDED,
        value,
        f"coherence {value:.3f} between the single-source and ensemble ranges",
    )


def recording_motion(rows: list[dict]) -> dict:
    """One timbre's rows as the motion reading its gate holds.

    `cents` and `rate_hz` are the medians of the rows' `fm` (peak-to-peak cents; the rate span's
    geometric mean). `gated` is True only where most rows read a motion and the median of their
    `fm_coherence` reads as a single source (`coherence_state`); never a verdict per note, though
    `states` counts the rows in each state. `reason` says why not otherwise. A row without an `fm`
    holds still. `warp_ratio`, the median unwarp width ratio, is carried as a
    reading and does not decide. A row without an `fm_coherence` (a reference measured before it
    was recorded) is not gated.
    """
    moving = [r for r in rows if r.get("fm")]
    out: dict = {"rows": len(rows), "moving": len(moving)}
    if not moving:
        out.update(gated=False, reason="no pitch motion read on any row")
        return out
    out["cents"] = round(float(np.median([r["fm"][0] for r in moving])), 3)
    out["rate_hz"] = round(float(np.median([np.sqrt(r["fm"][1] * r["fm"][2]) for r in moving])), 3)
    ratios = [r["fm_warp"]["w20_ratio"] for r in moving if "w20_ratio" in (r.get("fm_warp") or {})]
    if ratios:
        out["warp_ratio"] = round(float(np.median(ratios)), 3)
    if len(moving) < MIN_ROW_SHARE * len(rows):
        out.update(gated=False, reason=f"pitch motion read on {len(moving)} of {len(rows)} rows")
        return out
    if any("fm_coherence" not in r for r in moving):
        out.update(gated=False, reason="the rows record no coherence reading")
        return out
    readings = [r["fm_coherence"] for r in moving]
    out["states"] = {
        state.value: sum(coherence_state(c)[0] is state for c in readings)
        for state in CoherenceState
    }
    state, out["coherence"], reason = coherence_state(readings)
    out["coherence"] = round(out["coherence"], 3)
    out["coherence_state"] = state.value
    # Gating an ensemble is the costlier error, as it charges a model for motion no single voice has.
    out.update(gated=state is CoherenceState.SINGLE_SOURCE, reason=reason)
    return out


def motion_deltas(reference: list[dict], model: list[dict]) -> list[float]:
    """Model minus reference in doublings of depth and of rate, where the reference is gated.

    One pair per recording, so a gate on it is a statement about one reading. Empty where the
    reference is not gated. A model that holds still reads the extractor's own minimum depth,
    which is the smallest motion `pitch_motion` reports, and has no rate to compare.
    """
    from metrics_modulation import VIBRATO_EXTENT_MIN_CENTS

    ref = recording_motion(reference)
    if not ref["gated"]:
        return []
    mod = recording_motion(model)
    depth = max(mod.get("cents", 0.0), VIBRATO_EXTENT_MIN_CENTS)
    out = [float(np.log2(depth / ref["cents"]))]
    if "rate_hz" in mod:
        out.append(float(np.log2(mod["rate_hz"] / ref["rate_hz"])))
    return out
