"""Per-band power of a percussion hit, and each band's post-peak decay rate."""

from __future__ import annotations

import numpy as np


# --------------------------------------------------------------------------- #
# Percussion
# --------------------------------------------------------------------------- #
def _band_power(freqs: np.ndarray, power: np.ndarray, centers, ratio: float) -> np.ndarray:
    """Power summed into each band around `centers`, half-band width `ratio`."""
    out = np.empty(len(centers))
    for i, c in enumerate(centers):
        mask = (freqs >= c / ratio) & (freqs < c * ratio)
        out[i] = float(power[mask].sum())
    return out


# How a band's decay rate is estimated, and why not by fitting the log envelope.
#
# The previous estimator regressed the raw log-magnitude series from its peak
# frame down to 25 dB below it. On a noisy transient that is dominated by which
# frame the peak happened to land in, and the reference says so: measured across
# `reference/drums.json`, the SAME physical instrument struck at six velocities
# gives band decay rates spanning a median of 43 to 249 dB/s depending on the
# band, and a 90th percentile over 1200 dB/s. The loss caps the model-vs-
# reference difference at 60 dB/s. So the reference's own strike-to-strike
# variation already exceeded the cap in most bands: `bdecay` was a saturated
# constant with no gradient in it, not a measurement of anything.
#
# A Schroeder backward integration is the standard answer and it is already in
# this tree — `room.py` uses it to measure reverberation. Integrating the
# remaining energy from each moment forward removes exactly the frame-to-frame
# ripple that decided the old fit, and the -5..-25 dB span is the T20 convention
# for the same reason: the first 5 dB is the strike rather than the decay, and
# the bottom of the range is where the recording's floor takes over.
#
# The estimate also has to be able to refuse. A band with no decay in it, or one
# whose energy curve is not a line, returns None rather than a slope, which the
# loss already treats as an absent reference value.
EDC_FIT_RANGE_DB = (-5.0, -25.0)
#: How far the curve has to fall before a slope through it is a measurement.
#: Under this the fit spans too little to separate a rate from an offset.
EDC_MIN_DROP_DB = 12.0
#: How straight the energy decay curve has to be. A real decay is close to a
#: line in dB; a band that is being re-excited, or that is mostly floor, is not,
#: and the value fitted to it is a number rather than a rate.
EDC_MIN_R2 = 0.90
EDC_MIN_FRAMES = 6


def _edc_slope(series_power: np.ndarray, times: np.ndarray) -> tuple[float, float] | None:
    """Decay rate in dB/s from a Schroeder backward integration -> (slope, r2).

    `series_power` is a band's power per frame, already trimmed to start at that
    band's own peak. None when the curve does not fall far enough, has too few
    frames, or is not straight enough to have a rate.
    """
    if len(series_power) < EDC_MIN_FRAMES:
        return None
    edc = np.cumsum(series_power[::-1])[::-1]
    if edc[0] <= 0.0:
        return None
    edc_db = 10.0 * np.log10(np.maximum(edc / edc[0], 1e-30))
    total_drop = edc_db[0] - edc_db[-1]
    if total_drop < EDC_MIN_DROP_DB:
        return None
    hi, lo = EDC_FIT_RANGE_DB
    # Where the curve is genuinely shallower than the nominal span, fit whatever
    # it does cover rather than refusing: a short window on a fast band is the
    # common case and the -25 dB point simply is not in it.
    lo = max(lo, edc_db[-1] + 1.0)
    mask = (edc_db <= hi) & (edc_db >= lo)
    if np.count_nonzero(mask) < 4:
        return None
    t, y = times[mask], edc_db[mask]
    slope, intercept = np.polyfit(t, y, 1)
    resid = y - (slope * t + intercept)
    ss_res = float(np.sum(resid**2))
    ss_tot = float(np.sum((y - y.mean()) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0.0 else 0.0
    if r2 < EDC_MIN_R2:
        return None
    return float(slope), float(r2)


def _band_decay(
    seg: np.ndarray, sr: int, centers, ratio: float,
    *, n_fft: int = 1024, hop: int = 256,
) -> list[float | None]:
    """Post-peak decay rate in dB/s for each band, or None where unfittable.

    A percussion hit is a decay from the first sample, so each band is measured
    from its own peak frame downward — bands do not peak together, and a wire
    buzz that arrives after the shell would read as a rising slope if they were
    all anchored on the broadband onset.

    The rate itself comes from a Schroeder backward integration rather than from
    a regression on the log envelope; see `_edc_slope` for the measurement that
    made that necessary.
    """
    if len(seg) < n_fft:
        return [None] * len(centers)
    n_frames = (len(seg) - n_fft) // hop + 1
    frames = np.lib.stride_tricks.sliding_window_view(seg, n_fft)[::hop][:n_frames]
    power = np.abs(np.fft.rfft(frames * np.hanning(n_fft), axis=1)) ** 2
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    times = (np.arange(n_frames) * hop + n_fft / 2) / sr

    out: list[float | None] = []
    for c in centers:
        mask = (freqs >= c / ratio) & (freqs < c * ratio)
        if not mask.any():
            out.append(None)
            continue
        band = np.asarray(power[:, mask].sum(axis=1), dtype=np.float64)
        peak = int(np.argmax(band))
        got = _edc_slope(band[peak:], times[peak:] - times[peak])
        out.append(None if got is None else round(got[0], 2))
    return out
