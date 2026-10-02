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

# Onset-anchored windows keep the attack/body/tail order a whole-hit profile loses.
# The last one resolves the added body band and stays inside the 1.8 s hit ceiling.
EVOLUTION_WINDOWS = ((0.0, 0.015), (0.015, 0.060), (0.060, 0.240))
# One deep-body band is added below the seven texture bands; the density grid is unchanged.
EVOLUTION_BANDS = ((20.0, 60.0),) + TEXTURE_BANDS
EVOLUTION_BINS = len(EVOLUTION_BANDS)
EVOLUTION_WINDOWS_COUNT = len(EVOLUTION_WINDOWS)
EVOLUTION_FLATNESS_LOW_HZ = 1000.0
EVOLUTION_FLATNESS_HIGH_HZ = 8000.0
EVOLUTION_FLATNESS_FLOOR_DB = 120.0
# A positive FFT cell below this share of the common hit power is leakage or
# recording noise, not a measured component.  The same gate is used for
# evolution and flatness so a quiet tonal tail cannot turn into a diffuse hit.
EVOLUTION_MIN_REL_DB = -60.0
EVOLUTION_FLATNESS_MIN_REL_DB = EVOLUTION_MIN_REL_DB
EVOLUTION_MIN_CYCLES = 2.0
EVOLUTION_MIN_ACTUAL_BINS = 2
# A cell above this cannot carry a trustworthy distance: the reference has
# reached its own floor or the two hit windows contain different content.
# Keeping the cap in the producer makes provenance and every consumer agree.
EVOLUTION_CLIP_DB = 24.0
WINDOW_FLATNESS_CLIP_DB = 24.0


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


def _falling_half_hann(n: int) -> np.ndarray:
    """A one-sided Hann that leaves the first sample at full weight."""
    return np.cos(0.5 * np.pi * np.arange(n, dtype=np.float64) / n) ** 2


def _window_spectrum_power(
    segment: np.ndarray,
    sr: int,
    window_index: int,
) -> tuple[np.ndarray, np.ndarray, np.ndarray] | None:
    """Return actual-bin frequencies and normalized zero-padded power.

    The returned power is a mean-square spectrum: dividing by the taper's
    squared norm makes a short slice comparable with a long one, while the
    ``n_fft`` normalization preserves Parseval when the FFT is zero padded.
    Validity is decided by the *un-padded* frequencies by the caller, so extra
    FFT bins never manufacture resolution in a short recording.
    """
    x = np.asarray(segment, dtype=np.float64)
    if x.ndim != 1 or len(x) < 16 or not np.all(np.isfinite(x)):
        return None
    if window_index < 0:
        # The common ruler uses the unweighted, recorded span.  This is the
        # Parseval mean-square spectrum and keeps an onset at sample zero from
        # disappearing under a full Hann taper.
        taper = np.ones(len(x), dtype=np.float64)
    elif window_index == 0:
        taper = _falling_half_hann(len(x))
    else:
        taper = np.hanning(len(x))
    norm = float(np.sum(taper * taper))
    if norm <= 0.0 or not math.isfinite(norm):
        return None
    n_fft = 1 << max(10, math.ceil(math.log2(len(x))))
    weighted = x * taper
    spectrum = np.abs(np.fft.rfft(weighted, n_fft)) ** 2
    # The one-sided spectrum needs the negative-frequency energy restored before
    # it can be compared with the time-domain mean-square power.
    if n_fft > 2:
        spectrum[1:-1] *= 2.0
    power = spectrum / (float(n_fft) * norm)
    actual_freqs = np.fft.rfftfreq(len(x), 1.0 / sr)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    return actual_freqs, freqs, power


def _window_flatness(
    segment: np.ndarray,
    sr: int,
    *,
    low_hz: float,
    high_hz: float,
    window_index: int,
) -> float | None:
    """Spectral flatness on a recorded window, with no floor-as-signal answer."""
    result = _window_spectrum_power(segment, sr, window_index)
    if result is None:
        return None
    actual_freqs, freqs, power = result
    actual = (actual_freqs >= low_hz) & (actual_freqs <= high_hz)
    # Do not call a zero-padded line count a resolution increase.
    if int(np.count_nonzero(actual)) < 16:
        return None
    band = (freqs >= low_hz) & (freqs <= high_hz)
    values = np.asarray(power[band], dtype=np.float64)
    if values.size < 16:
        return None
    top = float(np.max(values))
    if top <= 0.0 or not math.isfinite(top):
        return None
    values = np.maximum(values, top * 10.0 ** (-EVOLUTION_FLATNESS_FLOOR_DB / 10.0))
    geometric = float(np.exp(np.mean(np.log(values))))
    arithmetic = float(np.mean(values))
    if arithmetic <= 0.0 or not math.isfinite(geometric):
        return None
    return round(10.0 * math.log10(geometric / arithmetic), 2)


def analyze_evolution(
    signal: np.ndarray,
    sr: int,
    *,
    max_band_hz: float | None = None,
) -> dict[str, list]:
    """Measure spectral power evolution and short-window diffuseness.

    Every evolution cell is relative to one common RMS power taken over the
    recorded 0--240 ms span.  Per-window normalization would erase the very
    attack/body/tail ordering this measurement is meant to expose.  A requested
    window must be fully present in the recording, and low bands also need two
    cycles plus two *un-padded* frequency bins.  ``*_measured`` records a
    finite, geometry-valid value even below the -60 dB reference eligibility
    floor; ``*_valid`` is the reference-domain mask consumed by the loss. A
    missing cell is represented by ``0.0`` only as a serialization placeholder
    and has both masks false; it is never interpreted as silence.
    """
    empty = {
        "evolution_db": [[0.0] * EVOLUTION_BINS for _ in EVOLUTION_WINDOWS],
        "evolution_valid": [[False] * EVOLUTION_BINS for _ in EVOLUTION_WINDOWS],
        "evolution_measured": [[False] * EVOLUTION_BINS for _ in EVOLUTION_WINDOWS],
        "window_flatness_db": [0.0] * EVOLUTION_WINDOWS_COUNT,
        "window_flatness_valid": [False] * EVOLUTION_WINDOWS_COUNT,
        "window_flatness_measured": [False] * EVOLUTION_WINDOWS_COUNT,
    }
    if sr <= 0:
        return empty
    x = _mono(signal)
    if x.ndim != 1 or len(x) < 16 or not np.all(np.isfinite(x)):
        return empty
    span_end = min(EVOLUTION_WINDOWS[-1][1], len(x) / float(sr))
    if span_end <= 0.0:
        return empty
    span = x[: int(span_end * sr)]
    if len(span) < 16:
        return empty

    if max_band_hz is None:
        ceiling = float("inf")
    else:
        ceiling = float(max_band_hz)
        if not math.isfinite(ceiling):
            return empty
    # Nyquist is a ceiling too: a partly observed band must not look valid.
    ceiling = min(ceiling, float(sr) / 2.0)
    # One common span power, excluding energy above the reference ceiling, is every cell's denominator.
    span_result = _window_spectrum_power(span, sr, -1)
    if span_result is None:
        return empty
    _span_actual, span_freqs, span_power = span_result
    common_power = float(np.sum(span_power[span_freqs <= ceiling + 1.0e-6]))
    if common_power <= 0.0 or not math.isfinite(common_power):
        return empty
    bands = EVOLUTION_BANDS
    for window_index, (start_s, end_s) in enumerate(EVOLUTION_WINDOWS):
        if end_s > len(x) / float(sr) + 1.0e-9:
            continue
        start = round(start_s * sr)
        end = round(end_s * sr)
        segment = x[start:end]
        result = _window_spectrum_power(segment, sr, window_index)
        if result is None:
            continue
        actual_freqs, freqs, power = result
        for band_index, (low_hz, high_hz) in enumerate(bands):
            if high_hz > ceiling + 1.0e-6:
                continue
            duration = end_s - start_s
            if low_hz * duration < EVOLUTION_MIN_CYCLES:
                continue
            actual_bins = int(np.count_nonzero((actual_freqs >= low_hz) & (actual_freqs < high_hz)))
            if actual_bins < EVOLUTION_MIN_ACTUAL_BINS:
                continue
            selected = (freqs >= low_hz) & (freqs < high_hz)
            band_power = float(np.sum(power[selected]))
            if band_power <= 0.0 or not math.isfinite(band_power):
                continue
            value = 10.0 * math.log10(band_power / common_power)
            if math.isfinite(value):
                empty["evolution_db"][window_index][band_index] = round(value, 3)
                empty["evolution_measured"][window_index][band_index] = True
                empty["evolution_valid"][window_index][band_index] = (
                    band_power >= common_power * 10.0 ** (EVOLUTION_MIN_REL_DB / 10.0)
                )
        high = min(ceiling, EVOLUTION_FLATNESS_HIGH_HZ)
        flat_band = (freqs >= EVOLUTION_FLATNESS_LOW_HZ) & (freqs <= high)
        flat_power = float(np.sum(power[flat_band]))
        flatness = _window_flatness(
            segment,
            sr,
            low_hz=EVOLUTION_FLATNESS_LOW_HZ,
            high_hz=high,
            window_index=window_index,
        )
        if flatness is not None and flat_power > 0.0:
            empty["window_flatness_db"][window_index] = flatness
            empty["window_flatness_measured"][window_index] = True
            empty["window_flatness_valid"][window_index] = flat_power >= common_power * 10.0 ** (
                EVOLUTION_FLATNESS_MIN_REL_DB / 10.0
            )
    return empty


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
