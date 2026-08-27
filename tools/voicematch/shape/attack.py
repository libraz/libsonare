"""Where each band's energy sits inside the first quarter second of a strike.

The reading that separates a piece whose bands are all at the right level from
one that puts them there in the wrong order. Every window is anchored on the
side's own onset, so a voice with a slow amplitude attack is not read over a
window its own latency chose.

Two traps this is arranged around, both of which produced a confident wrong
answer before it existed:

- **A tilt is a ratio and a ratio can be held by a defect in either band.** A
  closed hat measured 17.5 dB of high-minus-low error in its first 5 ms; split
  into bands, 10.7 dB of that was a low-frequency EXCESS and only 6 was a
  high-frequency deficit, and the mechanism each half wants is a different one.
  So the bands are reported separately and the ratio is left to the reader.
- **Windows of different lengths are not comparable.** Normalising a 5 ms
  window by the same side's 250 ms window mixes a real difference with the
  ratio of the two lengths and with what a Hann window costs a short segment;
  it read as the reference having nothing at all in its first 5 ms, which is
  false. Every cell here is normalised by ONE fixed span, and a row only ever
  compares the two sides at the same window length.
"""

from __future__ import annotations

import numpy as np

#: Onset-anchored window ends, in seconds.
ATTACK_WINDOWS = (0.005, 0.010, 0.020, 0.040, 0.120, 0.250)

#: Octave-ish bands, low enough to catch a plate's fundamental field and high
#: enough to catch the contact.
ATTACK_BANDS = ((60, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000),
                (4000, 8000), (8000, 16000))

#: The span every cell is normalised by. Fixed rather than per-row: it is what
#: makes a column a shape in time instead of a level, and what keeps two rows of
#: different window length off the same axis.
NORM_SPAN_S = 0.250

#: Envelope smoothing and the fraction of its peak that counts as the onset.
_ENV_MS = 1.0
_ONSET_FRACTION = 0.1


def onset_index(sig: np.ndarray, sr: float) -> int:
    """First sample at which the smoothed envelope reaches a tenth of its peak.

    Anchoring on the peak rather than on an absolute floor is what makes this
    survive a take with lead-in silence: a captured note commonly starts a
    tenth of a second into the file, and a fixed threshold finds the room
    rather than the strike.
    """
    width = max(1, int(_ENV_MS * 0.001 * sr))
    env = np.convolve(np.abs(sig), np.ones(width) / width, mode="same")
    peak = int(np.argmax(env))
    if peak == 0:
        return 0
    return int(np.argmax(env[:peak + 1] >= _ONSET_FRACTION * env[peak]))


#: Cells below this, relative to the normalising span, are reported at the
#: floor. It is applied AFTER normalisation on purpose: an absolute floor on a
#: raw power is a level, and a level-blind reading whose floor is a level stops
#: being level-blind as soon as the take is quiet enough to reach it.
FLOOR_DB = -120.0


def _band_power(sig: np.ndarray, i0: int, span: float, sr: float, bands) -> np.ndarray:
    seg = sig[i0:i0 + int(span * sr)]
    if seg.size < 4:
        return np.full(len(bands), np.nan)
    n = 1
    while n < seg.size:
        n <<= 1
    n <<= 3
    power = np.abs(np.fft.rfft(seg * np.hanning(seg.size), n)) ** 2
    freq = np.fft.rfftfreq(n, 1.0 / sr)
    return np.array([float(power[(freq >= lo) & (freq < hi)].sum()) for lo, hi in bands])


def profile(sig: np.ndarray, sr: float, windows=ATTACK_WINDOWS,
            bands=ATTACK_BANDS) -> np.ndarray:
    """Band level in each onset-anchored window, in dB relative to this side's
    own energy over NORM_SPAN_S. Shape is (len(windows), len(bands))."""
    i0 = onset_index(sig, sr)
    norm = float(np.sum(np.asarray(sig, dtype=np.float64)[i0:i0 + int(NORM_SPAN_S * sr)] ** 2))
    if norm <= 0.0:
        return np.full((len(windows), len(bands)), np.nan)
    ratio = np.array([_band_power(sig, i0, w, sr, bands) for w in windows]) / norm
    with np.errstate(divide="ignore"):
        return np.maximum(10.0 * np.log10(ratio), FLOOR_DB)


def compare(model: np.ndarray, reference: np.ndarray, sr: float,
            windows=ATTACK_WINDOWS, bands=ATTACK_BANDS) -> np.ndarray:
    """Model minus reference, dB, per window and band. Positive = the model has
    that band earlier, or louder relative to its own strike, than the
    reference does."""
    return (profile(model, sr, windows, bands)
            - profile(reference, sr, windows, bands))


def format_table(diff: np.ndarray, windows=ATTACK_WINDOWS, bands=ATTACK_BANDS) -> str:
    """The comparison as the rows a reader scans down a band and across time."""
    def label(lo):
        return str(lo) if lo < 1000 else f"{lo // 1000}k"

    lines = [f"{'window':>8}" + "".join(f"{label(lo):>8}" for lo, _ in bands)]
    for w, row in zip(windows, diff):
        lines.append(f"{int(w * 1000):>6d}ms"
                     + "".join("     nan" if np.isnan(v) else f"{v:>+8.1f}" for v in row))
    return "\n".join(lines)
