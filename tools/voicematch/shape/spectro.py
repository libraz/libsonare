"""Log-frequency spectrograms at three resolutions, energy-preserving.

Three scales, and each is load-bearing. The middle window resolves partials at
the bottom of a keyboard, where consecutive harmonics of C2 sit 65 Hz apart; the
short one resolves the attack, where a long window smears the first 170 ms into
a single column and reports the average of a transient. Fitted on a long window
alone a search is free to get the strike wrong as long as the steady tone lands;
on the short one alone it cannot separate two adjacent bass partials at all.

The longest window is not about partials but about **modes**. A piano's low
register is a field of individual resonances rather than a diffuse bottom end,
and two of them can sit 3.58 Hz apart. Separating those is decided by the window
LENGTH and by nothing else: not by the transform size, not by interpolation, not
by the direct-projection refinement `loss.py` uses to place a partial off the
bin grid — that improves where a peak is, not whether two of them are one. Two
components 3.58 Hz apart need at least 0.28 s to come apart, and 8192 samples is
0.17 s, so at that scale they are one peak however finely it is located. 32768
samples is 0.68 s, which resolves them with margin.

Rows are log-spaced and each row sums the power of every bin inside it, so a row
carries energy rather than a sampled magnitude. That matters at the top of the
range, where one row spans dozens of bins: an interpolating resampler would
report whichever bin it landed on and a partial could hide between two rows.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

#: (n_fft, hop, display rows) per scale.
#:
#: Appended rather than ordered by length, so scale 0 and scale 1 keep the
#: meaning every consumer indexing them by number already has.
DEFAULT_SCALES = ((8192, 2048, 240), (1024, 256, 160), (32768, 8192, 240))


def rows_hz(rows: int, f_lo: float, f_hi: float) -> np.ndarray:
    """Geometric centre frequency of each display row."""
    e = np.exp(np.linspace(np.log(f_lo), np.log(f_hi), rows + 1))
    return np.sqrt(e[:-1] * e[1:])


def _aggregator(n_fft: int, rows: int, sr: int, f_lo: float, f_hi: float) -> np.ndarray:
    """Rows-by-bins matrix mapping an rfft power spectrum onto the log grid.

    A row that falls between two bins -- which happens at the bottom, where the
    grid is finer than the transform -- takes the nearest bin rather than
    nothing, so the row carries the level of the frequency it names instead of
    reading as silence and dragging the floor discipline into counting it.
    """
    freq = np.fft.rfftfreq(n_fft, 1.0 / sr)
    edges = np.exp(np.linspace(np.log(f_lo), np.log(f_hi), rows + 1))
    a = np.zeros((rows, len(freq)))
    for r in range(rows):
        idx = np.where((freq >= edges[r]) & (freq < edges[r + 1]))[0]
        if len(idx) == 0:
            idx = np.array([int(np.argmin(np.abs(freq - np.sqrt(edges[r] * edges[r + 1]))))])
        a[r, idx] = 1.0
    return a


@dataclass
class Spectro:
    """A fixed analysis geometry: sample rate, duration, band, and scales."""

    sample_rate: int = 48000
    seconds: float = 9.6
    f_lo: float = 27.5
    f_hi: float = 16000.0
    scales: tuple[tuple[int, int, int], ...] = DEFAULT_SCALES
    _plan: list = field(default_factory=list, repr=False)

    def __post_init__(self) -> None:
        for n_fft, hop, rows in self.scales:
            self._plan.append((
                n_fft, hop, rows, np.hanning(n_fft),
                _aggregator(n_fft, rows, self.sample_rate, self.f_lo, self.f_hi),
            ))

    def rows_hz(self, scale: int) -> np.ndarray:
        return rows_hz(self.scales[scale][2], self.f_lo, self.f_hi)

    def times(self, scale: int, cols: int) -> np.ndarray:
        """Centre time of each column, in seconds from the start of the signal."""
        n_fft, hop, _ = self.scales[scale]
        return (np.arange(cols) * hop + n_fft / 2) / self.sample_rate

    def columns(self, scale: int, cols: int, t0: float, t1: float) -> np.ndarray:
        t = self.times(scale, cols)
        return (t >= t0) & (t < t1)

    def __call__(self, sig: np.ndarray) -> list[np.ndarray]:
        """dB spectrogram per scale, zero-padded or truncated to the fixed length.

        The length is fixed rather than taken from the signal so that every
        render in a comparison has the same time axis whatever its own tail did.
        A model that stops early is then short by silence, which the floor
        discipline handles, rather than by columns, which nothing would.
        """
        n = int(self.seconds * self.sample_rate)
        x = np.zeros(n)
        m = min(n, len(sig))
        x[:m] = sig[:m]
        out = []
        for n_fft, hop, _rows, win, agg in self._plan:
            cols = (n - n_fft) // hop + 1
            frames = np.lib.stride_tricks.as_strided(
                x, shape=(cols, n_fft),
                strides=(x.strides[0] * hop, x.strides[0])) * win
            p = np.abs(np.fft.rfft(frames, axis=1)) ** 2
            out.append(10 * np.log10(np.maximum(agg @ p.T, 1e-30)))
        return out
