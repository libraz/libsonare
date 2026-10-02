"""Texture measurements for percussion hits.

The ordinary percussion metrics describe a hit's level profile and its gross
envelope.  Those measurements cannot tell a dense, diffuse drum field from a
small set of narrow resonances when the two have the same band levels and
decay.  This module carries the two struck-piece measurements already used by
the shape fitter into the normal per-hit path:

``modal_density``
    Resonances per octave in the aftersound, with a recording-floor gate.
``prompt_late_db``
    The change in each octave band's share between the strike and the
    aftersound.

Each value has a parallel validity mask.  A short recording or one without a
usable floor is an unmeasured cell, never a zero-density or zero-colour hit.
The reference row remains the authority for which cells the loss asks about.
"""

from __future__ import annotations

# The constants are part of the small public metric surface and are re-exported
# by ``metrics`` for callers that use the aggregate module.
# ruff: noqa: F401
import math

import numpy as np
from shape.density import band_snr_db
from shape.struck import (
    DENSITY_BANDS,
    DENSITY_CLIP,
    DENSITY_SPAN_S,
    MIN_WINDOW_S,
    PROMPT_CLIP,
    STRUCK_SNR_DB,
    floor_window,
    mode_count,
    prompt_late,
    texture_window,
    windows,
)

# The shape path's density bands are the canonical grid.  Keeping a stable
# seven-cell output makes old profile readers and new live rows comparable.
TEXTURE_BANDS = tuple((float(lo), float(hi)) for lo, hi in DENSITY_BANDS)
TEXTURE_BAND_CENTERS = tuple(math.sqrt(lo * hi) for lo, hi in TEXTURE_BANDS)
TEXTURE_BINS = len(TEXTURE_BANDS)


def _mono(signal: np.ndarray) -> np.ndarray:
    """Return a finite mono view without manufacturing a noise floor."""
    x = np.asarray(signal, dtype=np.float64)
    if x.ndim > 1:
        x = np.mean(x, axis=1)
    return x


def _active_indices(max_band_hz: float | None) -> tuple[int, ...]:
    """Octave cells whose centre is inside the reference recording ceiling."""
    if max_band_hz is None:
        return tuple(range(TEXTURE_BINS))
    return tuple(i for i, centre in enumerate(TEXTURE_BAND_CENTERS) if centre <= max_band_hz + 1e-6)


def _inside(window: tuple[float, float], length_s: float) -> bool:
    """Whether a requested window is backed by recorded samples."""
    start, end = window
    return (
        math.isfinite(start)
        and math.isfinite(end)
        and start >= 0.0
        and end > start
        and end <= length_s + 1.0e-9
        and end - start >= MIN_WINDOW_S
    )


def _texture_windows(
    signal: np.ndarray, sr: int
) -> tuple[tuple[float, float], tuple[float, float]] | None:
    """Get finite, in-recording body and late windows from the struck estimator.

    ``shape.struck.windows`` deliberately permits a short piece to extend its
    minimum late window past the recording.  That is useful for exploratory
    shape reports, but it would turn zero padding into an apparent colour here.
    Reject the pair when either window is not actually recorded.
    """
    length_s = len(signal) / float(sr)
    body, late = windows(signal, sr)
    # Late colour on the density span keeps FFT resolution comparable across t60s.
    late = texture_window(late, span_s=DENSITY_SPAN_S)
    body_start, body_end = body
    body = (body_start, min(body_end, body_start + DENSITY_SPAN_S))
    if not _inside(body, length_s) or not _inside(late, length_s):
        return None
    return body, late


def analyze_texture(
    signal: np.ndarray,
    sr: int,
    *,
    max_band_hz: float | None = None,
) -> dict[str, list[float] | list[bool]]:
    """Measure modal density and prompt-to-late colour for one hit.

    The output always has seven cells, with cells above ``max_band_hz`` marked
    invalid.  A missing floor makes density invalid while prompt colour can
    still be measured from the two recorded windows.  A short or empty hit
    makes both measurements invalid.  No invalid cell receives an invented
    numeric value: its slot is ``0.0`` only as a serialisation placeholder and
    the corresponding validity flag is false.
    """
    x = _mono(signal)
    empty: dict[str, list[float] | list[bool]] = {
        "modal_density": [0.0] * TEXTURE_BINS,
        "modal_density_valid": [False] * TEXTURE_BINS,
        "prompt_late_db": [0.0] * TEXTURE_BINS,
        "prompt_late_valid": [False] * TEXTURE_BINS,
    }
    if sr <= 0 or len(x) < max(256, int(MIN_WINDOW_S * sr)) or not np.all(np.isfinite(x)):
        return empty
    active = _active_indices(max_band_hz)
    if not active:
        return empty
    pair = _texture_windows(x, sr)
    if pair is None:
        return empty
    body, late = pair

    # `mode_count` refuses without a post-release floor; pass it once rather than per band.
    floor = floor_window(x, sr)
    if floor is not None:
        bands = tuple(TEXTURE_BANDS[i] for i in active)
        density, density_ok = mode_count(x, late, sr, bands=bands, floor=floor)
        for i, value, ok in zip(active, density, density_ok):
            # Enforce the SNR gate here even if the estimator ever counts an under-floor band.
            snr = band_snr_db(x, TEXTURE_BANDS[i], window=late, floor_window=floor, sr=sr)
            valid = bool(ok and np.isfinite(value) and np.isfinite(snr) and snr >= STRUCK_SNR_DB)
            if valid:
                empty["modal_density"][i] = round(float(value), 3)
                empty["modal_density_valid"][i] = True

    # Prompt colour needs no floor, only the two recorded windows `_texture_windows` guarantees.
    bands = tuple(TEXTURE_BANDS[i] for i in active)
    prompt, prompt_ok = prompt_late(x, body, late, sr, bands=bands)
    for i, value, ok in zip(active, prompt, prompt_ok):
        valid = bool(ok and np.isfinite(value))
        if valid:
            empty["prompt_late_db"][i] = round(float(value), 3)
            empty["prompt_late_valid"][i] = True
    return empty
