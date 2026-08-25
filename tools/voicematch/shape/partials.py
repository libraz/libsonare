"""Tracking a string's own partials, and knowing when the track is unusable.

Everything here exists so that a statement about "the third partial" is about
the third partial. Two things get in the way. A real string is stiff, so its
partials are stretched and the k-th one is not at k*f0; and a partial that has
decayed into the reference's recorded floor produces a flat envelope, from which
a straight-line fit reports a decay rate near zero with nothing to mark the
number as noise. The first version of a decay table written without that guard
reported a C6 partial at six kilohertz losing half a decibel per second, which
no string does.

The stiffness coefficient is fitted from the reference rather than taken from
the model, so both sides are measured against the instrument's own series. The
floor is probed in the valley midway to the next partial with the identical
filter and the identical smoothing, which removes the units question: whatever
is there is what the partial has to stand clear of, in the units the partial is
measured in. An earlier version compared a row-aggregated spectrogram power
against a time-domain envelope amplitude and withheld every rate it had.
"""

from __future__ import annotations

import numpy as np

#: dB a partial must hold over the local floor for its rate to be reported.
BED_CLEAR_DB = 8.0


def note_hz(note: int) -> float:
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


#: dB a candidate peak must stand over its own search window's median. Twenty
#: rather than ten because noise alone reaches a factor of four across a window
#: this wide, and a partial reaches three to five orders of magnitude -- the two
#: populations are nowhere near each other, so the threshold is set clear of the
#: noisy one rather than midway.
PEAK_CLEAR_DB = 20.0
#: Consecutive missing partials that end the search.
MISS_LIMIT = 2


def fit_inharmonicity(sig: np.ndarray, f0: float, sr: int = 48000,
                      n_partials: int = 12, window=(0.6, 2.6)) -> float:
    """Least-squares B in f_k = k*f0*sqrt(1 + B*k^2), from the signal's own peaks.

    Each partial is located by taking the strongest bin in a window that opens
    upward from the harmonic position, because stiffness only ever raises a
    partial. Fewer than four locatable partials returns zero rather than a fit
    through noise -- a voice with no series to fit is better described as
    harmonic than as arbitrarily stretched.

    A candidate is only accepted where something is actually standing there.
    Without that guard a window holding no partial still returns its loudest
    bin, which sits at the window's lower edge and reads as a *negative*
    stretch -- and since the fit weights by k squared, two empty windows at the
    top outvote every real partial below them. Any band-limited voice hits this,
    and so does a piano at low velocity.

    Two consecutive misses end the search rather than one, because a single
    partial can be genuinely absent: a string struck at one eighth of its length
    has almost no eighth partial, and stopping there would discard the series
    above it.
    """
    seg = sig[int(window[0] * sr):int(window[1] * sr)]
    if len(seg) < 4096:
        return 0.0
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    fr = np.fft.rfftfreq(len(seg), 1.0 / sr)
    ks, fs, misses = [], [], 0
    for k in range(1, n_partials + 1):
        m = (fr >= k * f0 * 0.985) & (fr < k * f0 * 1.06)
        if m.sum() < 4:
            break
        band = sp[m]
        peak = float(band.max())
        if peak <= np.median(band) * 10.0 ** (PEAK_CLEAR_DB / 20.0):
            misses += 1
            if misses >= MISS_LIMIT:
                break
            continue
        misses = 0
        ks.append(k)
        fs.append(fr[m][int(np.argmax(band))])
    if len(ks) < 4:
        return 0.0
    ks, fs = np.array(ks, float), np.array(fs)
    y = (fs / (ks * f0)) ** 2 - 1.0
    return float(np.clip(np.dot(ks ** 2, y) / np.dot(ks ** 2, ks ** 2), 0.0, 0.01))


def partial_hz(f0: float, B: float, k: int) -> float:
    return k * f0 * np.sqrt(1.0 + B * k * k)


def series(f0: float, B: float, f_max: float = 15500.0, k_max: int = 32):
    """(k, frequency) for every partial under the ceiling."""
    out = []
    for k in range(1, k_max + 1):
        f = partial_hz(f0, B, k)
        if f > f_max:
            break
        out.append((k, f))
    return out


#: Above this partial index a quarter-tone mask stops separating anything: at a
#: quarter-tone tolerance neighbouring partials are closer together than the
#: tolerance itself once k exceeds 1/(2^(1/12) - 1), so the mask fuses into one
#: continuous band and marks the whole upper spectrum of a bass note as "on
#: partial". That is correct for a term asking how much of the sound is the
#: string, and wrong for one asking what ELSE is up there -- which then has
#: nothing left to look at. Callers that need the second pass this as a cap.
RESOLVABLE_PARTIAL = 16


def harmonic_rows(hz: np.ndarray, f0: float, B: float, tol_octaves: float = 1.0 / 24.0,
                  max_partial: int = 128):
    """Boolean mask of display rows within a quarter tone of any partial.

    `max_partial` bounds the series. The default keeps the whole ladder, which
    is what a purity or residue measure wants; `RESOLVABLE_PARTIAL` is the cap
    above which the mask stops discriminating and starts covering everything.
    """
    k = np.arange(1, max(1, max_partial) + 1)[:, None]
    f = partial_hz(f0, B, k)
    d = np.abs(np.log2(np.maximum(hz[None, :], 1e-9) / np.maximum(f, 1e-9)))
    return d.min(axis=0) < tol_octaves


def band_envelope(sig: np.ndarray, f: float, bw: float, sr: int = 48000) -> np.ndarray:
    """Smoothed amplitude envelope of one band, by zeroing the rest of the spectrum.

    The smoothing length is four cycles of the band's own centre, so a low
    partial is averaged over a long window and a high one over a short one --
    otherwise a fixed window either fails to smooth the bass or destroys the
    treble's early decay.
    """
    n = len(sig)
    S = np.fft.rfft(sig)
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    x = np.fft.irfft(np.where((fr >= f - bw) & (fr < f + bw), S, 0.0), n)
    w = max(16, int(4 * sr / max(f, 20.0)))
    return np.convolve(np.abs(x), np.ones(w) / w, mode="same")


def level_db(env: np.ndarray, t: float, span: float = 0.2, sr: int = 48000) -> float:
    i = int(t * sr)
    return 20 * np.log10(max(float(np.mean(env[i:i + int(span * sr)])), 1e-14))


def decay_db_s(env: np.ndarray, t0: float, t1: float, floor_db: float = -300.0,
               sr: int = 48000) -> float | None:
    """Straight-line decay rate over a window, or None if it dips under the floor.

    Returning None rather than a number is the whole point: a rate fitted
    through a stretch where the partial has fallen into the recording's floor is
    a property of the recording.
    """
    d = 20 * np.log10(np.maximum(env[int(t0 * sr):int(t1 * sr)], 1e-14))
    if len(d) < 200 or d.min() < floor_db:
        return None
    return float(np.polyfit(np.arange(len(d)) / sr, d, 1)[0])


class Track:
    """One note's partial series, with the reference's local floor attached."""

    def __init__(self, ref: np.ndarray, note: int, sr: int = 48000,
                 k_max: int = 16, f_max: float = 15500.0):
        self.sr = sr
        self.note = note
        self.f0 = note_hz(note)
        self.B = fit_inharmonicity(ref, self.f0, sr)
        self.ks = series(self.f0, self.B, f_max, k_max)
        self.bw = max(8.0, self.f0 * 0.3)
        self._floor: dict[int, np.ndarray] = {}
        self._ref = ref

    def envelope(self, sig: np.ndarray, k: int) -> np.ndarray:
        return band_envelope(sig, partial_hz(self.f0, self.B, k), self.bw, self.sr)

    def floor(self, k: int) -> np.ndarray:
        """The reference's level in the valley between partial k and k+1."""
        if k not in self._floor:
            f = partial_hz(self.f0, self.B, k)
            fn = partial_hz(self.f0, self.B, k + 1)
            self._floor[k] = band_envelope(self._ref, 0.5 * (f + fn), self.bw, self.sr)
        return self._floor[k]

    def clear(self, k: int, t: float) -> bool:
        """Whether the reference's partial k still stands over the local floor."""
        return level_db(self.envelope(self._ref, k), t) > \
            level_db(self.floor(k), t) + BED_CLEAR_DB
