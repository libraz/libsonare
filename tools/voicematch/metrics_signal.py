"""Mono reduction, level and spectrum primitives, and audibility weighting."""

from __future__ import annotations

import numpy as np


N_HARMONICS = 12


def midi_to_hz(note: int) -> float:
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


# How a stereo render is reduced to the one channel every metric reads, and why
# the default is the one with a known defect.
#
# `mean` sums the channels, which comb-filters whatever is decorrelated between
# them. A reference captured through two spaced close mics is decorrelated by
# construction — that is what spacing is for — so the sum has notches at the
# frequencies where the path difference is half a wavelength, and a notch that
# lands on a partial reads as several dB of harmonic error the model is then
# asked to reproduce. A mono model has no such notches and cannot.
#
# It is still the default, and the reason is not that it is right. Every
# committed profile in `reference/` was measured through it, and those files
# cannot be re-measured without the plugin they came from. Changing the
# reduction would silently redefine what they contain. So the fix is offered
# rather than imposed: `--mono-mode left` (or `loudest`) takes one channel and
# has no sum in it at all, and `channel_correlation` reports how much of a
# difference that could make for a given render.
MONO_MODES = ("mean", "left", "loudest")


def to_mono(audio: np.ndarray, mode: str = "mean") -> np.ndarray:
    """Reduce (frames, channels) to one channel of float64.

    `mean` sums; `left` takes the first channel; `loudest` takes whichever
    channel carries the most energy. See `MONO_MODES` for why a sum is the
    default despite comb-filtering a spaced-pair capture.
    """
    if audio.ndim == 1:
        return audio.astype(np.float64)
    a = audio.astype(np.float64)
    if a.shape[1] == 1:
        return a[:, 0]
    if mode == "left":
        return a[:, 0]
    if mode == "loudest":
        return a[:, int(np.argmax((a**2).mean(axis=0)))]
    if mode != "mean":
        raise ValueError(f"unknown mono mode {mode!r} (choose from {MONO_MODES})")
    return a.mean(axis=1)


def channel_correlation(audio: np.ndarray) -> float | None:
    """How alike a stereo render's two channels are, as a correlation in [-1, 1].

    None for anything that is not two-channel. Near 1 means summing them is
    harmless; well below it means the sum is comb-filtered and a per-partial
    level read off that sum is partly the microphone spacing rather than the
    instrument. Reported rather than acted on — see `MONO_MODES`.
    """
    if audio.ndim != 2 or audio.shape[1] != 2:
        return None
    a = audio.astype(np.float64)
    left, right = a[:, 0] - a[:, 0].mean(), a[:, 1] - a[:, 1].mean()
    denom = float(np.std(left) * np.std(right))
    if denom <= 0.0:
        return None
    return float(np.mean(left * right) / denom)


def normalize_rms(audio: np.ndarray, target_rms: float = 0.05) -> np.ndarray:
    """Scale the whole render to a common RMS so level metrics compare balance."""
    rms = float(np.sqrt(np.mean(audio**2)))
    if rms < 1e-8:
        return audio
    return audio * (target_rms / rms)


def _db(x: np.ndarray | float, floor: float = 1e-12) -> np.ndarray | float:
    return 20.0 * np.log10(np.maximum(x, floor))


def _rms_envelope(y: np.ndarray, sr: int, hop_ms: float = 5.0, win_ms: float = 10.0):
    """Frame RMS envelope; returns (times_sec, rms) arrays."""
    hop = max(1, int(sr * hop_ms / 1000.0))
    win = max(hop, int(sr * win_ms / 1000.0))
    n = max(0, (len(y) - win) // hop + 1)
    if n == 0:
        return np.zeros(1), np.array([float(np.sqrt(np.mean(y**2)))])
    frames = np.lib.stride_tricks.sliding_window_view(y, win)[::hop][:n]
    rms = np.sqrt(np.mean(frames**2, axis=1))
    times = (np.arange(n) * hop + win / 2) / sr
    return times, rms


def _spectrum(seg: np.ndarray, sr: int):
    """Hann-windowed magnitude spectrum; returns (freqs, magnitude)."""
    win = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg * win))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return freqs, mag


def _peak_near(freqs: np.ndarray, mag: np.ndarray, center_hz: float, tolerance_cents: float) -> tuple[float, float]:
    """Strongest bin within ±tolerance_cents of center_hz -> (freq, magnitude).

    Refines the peak frequency by parabolic interpolation over log-magnitude.
    """
    lo = center_hz * 2.0 ** (-tolerance_cents / 1200.0)
    hi = center_hz * 2.0 ** (tolerance_cents / 1200.0)
    idx = np.where((freqs >= lo) & (freqs <= hi))[0]
    if idx.size == 0:
        return center_hz, 0.0
    k = idx[np.argmax(mag[idx])]
    if 0 < k < len(mag) - 1 and mag[k] > 0:
        with np.errstate(divide="ignore"):
            a, b, c = np.log(np.maximum(mag[k - 1 : k + 2], 1e-12))
        denom = a - 2 * b + c
        delta = 0.5 * (a - c) / denom if abs(denom) > 1e-12 else 0.0
        delta = float(np.clip(delta, -0.5, 0.5))
        return float(freqs[k] + delta * (freqs[1] - freqs[0])), float(mag[k])
    return float(freqs[k]), float(mag[k])


# --------------------------------------------------------------------------- #
# Audibility weighting
# --------------------------------------------------------------------------- #
# Two reasons an L1 over partials in dB is not what a listener hears, and both
# of them bite hardest exactly where this harness is used.
#
# Absolute frequency: A0's fundamental at 27.5 Hz and its eighth partial at
# 220 Hz are more than 30 dB apart in how loud they are for the same level, and
# an unweighted sum gives them the same vote. A bass note's timbre lives in the
# partials, not in the fundamental, and the unweighted term says otherwise.
#
# Level within the note: a partial 50 dB under the loudest one in the same note
# is masked by it. An error of 10 dB there is inaudible and is charged in full,
# so a fit can spend real knobs moving something no one can hear while a 3 dB
# error on the loudest partial waits.
#
# Neither is a model of hearing. A-weighting is a 40-phon curve applied at every
# level, and the masking weight is a slope rather than a spreading function.
# They are corrections in the right direction, and both are switchable, because
# a term whose value changes meaning between releases is worse than one that is
# merely crude.


def a_weight_db(freq_hz: np.ndarray | float) -> np.ndarray | float:
    """IEC 61672 A-weighting in dB, 0 dB at 1 kHz."""
    f = np.asarray(freq_hz, dtype=np.float64)
    f2 = np.maximum(f, 1e-6) ** 2
    num = (12194.0**2) * f2**2
    den = ((f2 + 20.6**2)
           * np.sqrt((f2 + 107.7**2) * (f2 + 737.9**2))
           * (f2 + 12194.0**2))
    with np.errstate(divide="ignore", invalid="ignore"):
        ra = np.where(den > 0.0, num / den, 0.0)
        out = 20.0 * np.log10(np.maximum(ra, 1e-30)) + 2.00

    return float(out) if np.isscalar(freq_hz) or np.ndim(freq_hz) == 0 else out


#: Under this much below the note's loudest partial, a partial's weight has
#: fallen to `AUDIBILITY_MIN_WEIGHT`. Chosen as a plausible masking span rather
#: than measured: within a critical band the slope is steeper and across the
#: spectrum it is shallower, so this is one number standing in for a surface.
AUDIBILITY_MASK_SPAN_DB = 45.0
#: What a fully masked partial still counts for. Not zero, because a partial
#: nobody can hear in the reference can still be the one the model is 40 dB LOUD
#: on, and a zero weight would license that.
AUDIBILITY_MIN_WEIGHT = 0.1
#: Range the A-weighting is allowed to span, in dB. Uncapped it runs to -50 dB
#: at 20 Hz, which does not down-weight the bottom octave so much as delete it.
AUDIBILITY_A_FLOOR_DB = -20.0


def audibility_weights(freqs_hz, levels_db, *, a_weight: bool = True,
                       mask: bool = True) -> np.ndarray:
    """Per-partial (or per-band) weights in [AUDIBILITY_MIN_WEIGHT, 1].

    `levels_db` are relative levels within one note — the h1-normalised ladder
    or the peak-normalised band profile — so the masking half needs no absolute
    calibration. `freqs_hz` may contain None for a bin that has no frequency,
    which is weighted by level alone.

    Both halves are multiplicative and each is switchable, because a term that
    silently changes meaning is worse than a crude one that does not.
    """
    n = len(levels_db)
    w = np.ones(n, dtype=np.float64)
    if mask:
        finite = [v for v in levels_db if v is not None and np.isfinite(v)]
        if finite:
            top = max(finite)
            for i, v in enumerate(levels_db):
                if v is None or not np.isfinite(v):
                    w[i] = AUDIBILITY_MIN_WEIGHT
                    continue
                frac = 1.0 - (top - v) / AUDIBILITY_MASK_SPAN_DB
                w[i] *= float(np.clip(frac, AUDIBILITY_MIN_WEIGHT, 1.0))
    if a_weight:
        for i, f in enumerate(freqs_hz):
            if f is None or not np.isfinite(f) or f <= 0.0:
                continue
            aw = max(float(a_weight_db(f)), AUDIBILITY_A_FLOOR_DB)
            # Referenced to 0 dB at 1 kHz and expressed as a gain in [0, 1], so
            # the weight can only ever reduce a partial's vote.
            w[i] *= float(10.0 ** (min(aw, 0.0) / 20.0))
    return np.maximum(w, AUDIBILITY_MIN_WEIGHT * AUDIBILITY_MIN_WEIGHT)
