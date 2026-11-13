"""Whether the partials' frequency tracks say one source or several made a note's motion.

The keep rule's one condition (`coherence_state`); shared byte for byte with the motion gate's file.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from enum import Enum

import numpy as np

#: Passband of the phase track the note is unwarped along, as a fraction of f0; it stops at twice.
WARP_CUTOFF_F0_FRACTION = 0.2


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


def _phase_track(seg: np.ndarray, sr: int, f0: float, cutoff: float, stop: float) -> np.ndarray:
    """The fundamental's unwrapped phase (radians) under a raised-cosine passband."""
    t = np.arange(len(seg)) / sr
    spec = np.fft.fft(seg * np.exp(-2j * np.pi * f0 * t))
    away = np.abs(np.fft.fftfreq(len(seg), 1.0 / sr))
    spec *= 0.5 * (1.0 + np.cos(np.pi * np.clip((away - cutoff) / (stop - cutoff), 0.0, 1.0)))
    return np.unwrap(np.angle(np.fft.ifft(spec)))


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
