"""Vibrato, tremolo, beat, and the width of the fundamental."""

from __future__ import annotations

import numpy as np

from smf import Note


# --------------------------------------------------------------------------- #
# Movement: vibrato, tremolo, and the beat of an ensemble
# --------------------------------------------------------------------------- #
# Nothing in this harness measured whether a note MOVES, and movement is most of
# what separates a played instrument from a synthesised one. The README has said
# so for as long as it has existed —
#
#   "A model reading 'cleaner, flatter' than the oracle often means 'add
#    vibrato/breath movement', not 'the oracle is worse'."
#
# — while the objective had no term that could ever say it. `tnr` is the closest
# and it is one-sided by construction: it charges the model for being NOISIER
# than the reference and is silent when the model is dead. So the one reading
# that reliably indicates a missing vibrato was the one reading the loss could
# not act on.
#
# What is measured is the fundamental's instantaneous frequency and level over
# the held part of the note, separated into three bands because they are three
# different mechanisms with three different repairs:
#
#   vibrato   3-9 Hz frequency modulation — the player's hand or embouchure
#   tremolo   3-9 Hz amplitude modulation — bowing pressure, breath
#   beat      0.3-3 Hz amplitude modulation — two or more sources slightly
#             detuned against each other, which is what a section, a unison
#             string pair and a chorused pad all are
#
# The beat band is why this also answers the ensemble question. A section
# patch's identity is how far apart its voices are tuned, and that is not
# reachable by any single-note spectral measure — but it is exactly a slow
# amplitude modulation of every partial, and one note is enough to read it.
MOD_FRAME_HOP_S = 0.01
MOD_FRAME_WIN_S = 0.05
#: Where the movement is read, in seconds from the detected onset. It starts
#: after the attack, since a rise is not a modulation, and it stops before a
#: struck note has decayed into its own floor.
MOD_WINDOW_S = (0.15, 2.5)
#: Fewest frames that make a modulation spectrum a measurement. At a 10 ms hop
#: this is 0.4 s, which is two and a half cycles of the slowest beat the band
#: covers — under that the peak found is the window rather than the note.
MOD_MIN_FRAMES = 40
VIBRATO_BAND_HZ = (3.0, 9.0)
BEAT_BAND_HZ = (0.3, 3.0)
#: How far either side of the nominal pitch the tracker looks. Wider than the
#: deepest vibrato any instrument uses, so the track cannot be clipped by its
#: own search range and report a shallow one.
MOD_TRACK_CENTS = 120.0
MOD_TRACK_POINTS = 49


def _band_peak(spectrum: np.ndarray, rate_hz: float, band: tuple[float, float],
               n: int) -> tuple[float, float]:
    """Strongest component of a modulation spectrum inside `band`.

    Returns (peak-to-peak amplitude in the input's own units, rate in Hz).
    The amplitude conversion assumes the Hann window the caller applied: a
    sinusoid of amplitude A lands at |X| = A*n/4, so peak-to-peak is 8|X|/n.
    """
    freqs = np.fft.rfftfreq(n, 1.0 / rate_hz)
    mask = (freqs >= band[0]) & (freqs <= band[1])
    if not mask.any():
        return 0.0, 0.0
    idx = np.where(mask)[0]
    k = idx[int(np.argmax(spectrum[idx]))]
    return float(8.0 * spectrum[k] / n), float(freqs[k])


def modulation_note(mono: np.ndarray, sr: int, note: Note, f0: float,
                    onset: float | None = None) -> dict:
    """Vibrato, tremolo and beat of one note's fundamental.

    The fundamental is tracked rather than assumed: a zoomed DFT over a bank of
    candidate frequencies per frame, which resolves a few cents at a 50 ms
    window where an FFT bin is 20 Hz. Both the frequency track and the level
    track are linearly detrended before the modulation spectrum is taken, so a
    note that is decaying (every level track) or drifting (a wind instrument
    warming) does not report its trend as a very slow modulation.

    Every field is None when the note is too short, too quiet or has no
    fundamental to track. A zero would be a claim that the note is dead still,
    which is a different finding from not having looked.
    """
    empty = {"vib_cents": None, "vib_rate_hz": None, "trem_db": None,
             "trem_rate_hz": None, "beat_db": None, "beat_rate_hz": None}
    if f0 <= 0.0 or f0 > sr / 2.0:
        return empty
    start = (note.start if onset is None else onset) + MOD_WINDOW_S[0]
    end = min((note.start if onset is None else onset) + MOD_WINDOW_S[1],
              note.start + note.dur)
    a, b = int(start * sr), int(end * sr)
    seg = np.asarray(mono[a:b], dtype=np.float64)
    win_n = int(MOD_FRAME_WIN_S * sr)
    hop = int(MOD_FRAME_HOP_S * sr)
    n_frames = (len(seg) - win_n) // hop + 1 if len(seg) >= win_n else 0
    if n_frames < MOD_MIN_FRAMES:
        return empty

    frames = np.lib.stride_tricks.sliding_window_view(seg, win_n)[::hop][:n_frames]
    frames = frames * np.hanning(win_n)
    span = 2.0 ** (MOD_TRACK_CENTS / 1200.0)
    cand = np.linspace(f0 / span, f0 * span, MOD_TRACK_POINTS)
    t = np.arange(win_n) / sr
    basis = np.exp(-2j * np.pi * np.outer(cand, t))       # (points, win_n)
    amps = np.abs(frames @ basis.T)                        # (frames, points)
    k = np.argmax(amps, axis=1)
    level = amps[np.arange(n_frames), k]
    if float(level.max()) <= 0.0:
        return empty

    # Parabolic refinement over the candidate grid, so the track is not
    # quantised to the grid spacing (5 cents at the default point count).
    log_cand = np.log2(cand)
    track = np.empty(n_frames)
    for i, ki in enumerate(k):
        if 0 < ki < MOD_TRACK_POINTS - 1:
            y0, y1, y2 = amps[i, ki - 1], amps[i, ki], amps[i, ki + 1]
            denom = y0 - 2 * y1 + y2
            d = float(np.clip(0.5 * (y0 - y2) / denom, -0.5, 0.5)) if abs(denom) > 1e-12 else 0.0
        else:
            d = 0.0
        step = log_cand[1] - log_cand[0]
        track[i] = log_cand[ki] + d * step
    cents = 1200.0 * (track - float(np.median(track)))
    level_db = 20.0 * np.log10(np.maximum(level, 1e-12))

    idx = np.arange(n_frames, dtype=np.float64)
    cents = cents - np.polyval(np.polyfit(idx, cents, 1), idx)
    level_db = level_db - np.polyval(np.polyfit(idx, level_db, 1), idx)

    window = np.hanning(n_frames)
    rate = 1.0 / MOD_FRAME_HOP_S
    fm = np.abs(np.fft.rfft(cents * window))
    am = np.abs(np.fft.rfft(level_db * window))
    vib_cents, vib_rate = _band_peak(fm, rate, VIBRATO_BAND_HZ, n_frames)
    trem_db, trem_rate = _band_peak(am, rate, VIBRATO_BAND_HZ, n_frames)
    beat_db, beat_rate = _band_peak(am, rate, BEAT_BAND_HZ, n_frames)
    return {
        "vib_cents": round(vib_cents, 2), "vib_rate_hz": round(vib_rate, 2),
        "trem_db": round(trem_db, 2), "trem_rate_hz": round(trem_rate, 2),
        "beat_db": round(beat_db, 2), "beat_rate_hz": round(beat_rate, 2),
    }


def f0_width_cents(freqs: np.ndarray, mag: np.ndarray, f0: float) -> float | None:
    """Width of the fundamental's spectral peak at -3 dB, in cents.

    A single string radiates one frequency and the width measured here is the
    analysis window's. Several sources a few cents apart — a piano's unison, a
    string section, a chorused pad — radiate a band, and the width is how far
    apart they are. That is the one property of an ensemble patch a single note
    can carry, and it is why this is taken alongside the beat rate rather than
    instead of it: the beat says how fast, this says how wide.

    None when the peak runs off the end of the analysed span, which is what a
    note with no stable fundamental gives.
    """
    if f0 <= 0.0 or len(freqs) < 4:
        return None
    lo = f0 * 2.0 ** (-200.0 / 1200.0)
    hi = f0 * 2.0 ** (200.0 / 1200.0)
    idx = np.where((freqs >= lo) & (freqs <= hi))[0]
    if idx.size < 4:
        return None
    local = mag[idx]
    k = int(np.argmax(local))
    top = float(local[k])
    if top <= 0.0:
        return None
    half = top * 10.0 ** (-3.0 / 20.0)
    left = k
    while left > 0 and local[left] > half:
        left -= 1
    right = k
    while right < len(local) - 1 and local[right] > half:
        right += 1
    if left == 0 or right == len(local) - 1:
        return None
    f_lo, f_hi = float(freqs[idx[left]]), float(freqs[idx[right]])
    if f_lo <= 0.0 or f_hi <= f_lo:
        return None
    return round(1200.0 * float(np.log2(f_hi / f_lo)), 1)
