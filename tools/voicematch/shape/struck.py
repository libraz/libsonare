"""What a struck, unpitched piece needs that a played string does not.

Everything else in this package assumes the note number names a pitch. Six of
the seven loss terms mask by the played note's own partial series, and every
analysis window is an absolute number of seconds chosen against a piano note
that sounds for eight of them. Pointed at a drum kit, neither assumption holds
and neither fails loudly:

  - `note_hz(42)` is a real frequency, so the harmonic mask is built, applied,
    and notches a pitch that is not in the signal. The terms downstream return
    numbers rather than errors.
  - A closed hi-hat has fallen sixty decibels 0.67 s after the strike, and the
    aftersound windows begin at 2.0 and 2.5 s. They measure silence, and the
    floor discipline drops those cells, so the term reports a small error for a
    piece it did not look at.

A kit is also not one length. Its pieces span an order of magnitude — the same
capture holds a hat that is over in two thirds of a second and a ride that is
still sounding after eight — so no single set of absolute windows can serve
them. The windows here are per piece and come from the reference's own decay,
which is the only thing that means the same for both.

Two measurements replace the partial-masked terms rather than standing in for
them, because what a struck plate has to get right is not what a string does:

  - How many things are ringing. A cymbal is a field of hundreds of overlapping
    inharmonic modes; a small resonator bank at the same level is a tuned bar.
    `density.modal_density` counts them, and counting is what separates here:
    graded against fields of 2 to 256 partials in one band it reads 2, 4, 7, 12,
    24, 43, 72 with white noise at 30, and moves by under 5 % when the same
    field is given a decay. `envelope_diffuseness` is the other half of that
    module and is NOT usable at this geometry — over the short, stationary
    window a struck piece allows, every one of those fields and the noise read
    between 0.22 and 0.25 in the 2-4 kHz band, which is the diffuse floor. What
    varies over a longer window is the decay inside it and not the texture.
  - How the strike separates from the sustain. Hardness lives in the first
    tenth of a second: a piece can hold the right band levels over the whole hit
    and still read as soft because the top of its band is gone by the time the
    ear has finished deciding what it heard. Measured as each band's share of
    the strike against its share of the aftersound, which is the two-rate
    reading `admittance` takes per partial, taken per band instead because an
    unpitched piece has no partial to take it on.
"""

from __future__ import annotations

import numpy as np

from .density import DENSITY_BANDS, band_snr_db, modal_density

#: Octave bands the prompt/late split is read in. The same grid `density` counts
#: in, so a piece that reads sparse and a piece that reads dull are reported
#: against one another band by band.
STRUCK_BANDS = DENSITY_BANDS

#: A band must stand this far over the recording's own floor before its texture
#: is a reading rather than a description of that floor.
STRUCK_SNR_DB = 12.0

#: Clips, in the units each term is measured in: resonances counted in a band,
#: and decibels of band share moved between the strike and the aftersound.
DENSITY_CLIP = 40.0
PROMPT_CLIP = 20.0

#: How much of the aftersound one texture reading spans. Long enough to resolve
#: separate resonances in the lowest band it is asked about, short enough that
#: the piece is not decaying appreciably across it.
DENSITY_SPAN_S = 0.30

#: Shortest analysis window worth taking. Below this a window holds too few
#: cycles of the bottom band to say anything about texture.
MIN_WINDOW_S = 0.05

#: dB under a window's own total at which a band counts as absent from it. Used
#: as a floor rather than as a rejection, so a band that vanishes is measured as
#: having vanished.
DEAD_DB = 60.0


def rms_envelope(sig: np.ndarray, sr: int = 48000, hop_ms: float = 2.0) -> np.ndarray:
    """Short-window RMS of a hit, normalised to its own peak."""
    x = np.asarray(sig, dtype=np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    hop = max(1, int(hop_ms * 1e-3 * sr))
    n = len(x) // hop
    if n < 2:
        return np.zeros(1)
    e = np.sqrt(np.array([np.mean(x[i * hop:(i + 1) * hop] ** 2) for i in range(n)]) + 1e-20)
    return e / max(float(e.max()), 1e-20)


def decay_marks(sig: np.ndarray, sr: int = 48000, hop_ms: float = 2.0):
    """Seconds at which the hit has fallen 20 dB and 60 dB under its peak.

    Read forward from the peak and taken at the FIRST crossing, so a piece that
    dips and recovers — a cymbal whose shimmer swells after the strike — is
    measured on the fall the ear follows rather than on the last time it was
    ever that loud. Either mark comes back as the end of the signal when it is
    never reached, which is the honest answer for a piece still sounding when
    the capture stopped.
    """
    e = rms_envelope(sig, sr, hop_ms)
    hop_s = hop_ms * 1e-3
    end = len(e) * hop_s
    if len(e) < 2:
        return end, end
    db = 20.0 * np.log10(np.maximum(e, 1e-12))
    peak = int(np.argmax(db))

    def first_below(level: float) -> float:
        under = np.where(db[peak:] < level)[0]
        return end if not under.size else (peak + int(under[0])) * hop_s

    return first_below(-20.0), first_below(-60.0)


def windows(sig: np.ndarray, sr: int = 48000):
    """This piece's body and aftersound windows, from its own decay.

    The body runs from the strike to 20 dB down and the aftersound from there to
    60 dB down, so the two mean the same thing for a hi-hat and for a ride
    without either being told how long it is. A piece too short to fill the
    second window gets both windows over what it has rather than a window past
    its end, since a window past the end measures the floor and reports it as
    the piece.
    """
    t20, t60 = decay_marks(sig, sr)
    body = (0.0, max(MIN_WINDOW_S, t20))
    late = (body[1], max(body[1] + MIN_WINDOW_S, t60))
    return body, late


def floor_window(sig: np.ndarray, sr: int = 48000, margin_s: float = 0.05):
    """Where this recording's own floor can be read, or None if nowhere.

    Everything downstream of `band_snr_db` needs a stretch of the same recording
    that holds the floor and not the piece, and on a kit there is no fixed place
    to look: the pieces are an order of magnitude apart in length and the plane
    they share is padded to the longest of them. Reading a floor from the
    padding measures digital silence, which passes as an infinite signal-to-
    noise ratio and is then refused as non-finite — so every band drops out and
    the term it fed averages an empty set, which is the one arrangement that
    scores as perfect.

    So the window is the stretch between the piece's own t60 and the end of what
    was actually recorded, found as the last sample that is not exactly zero.
    None when that leaves too little to transform, which the caller must report
    rather than score.
    """
    x = np.asarray(sig, dtype=np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    live = np.nonzero(x)[0]
    if not live.size:
        return None
    end = (int(live[-1]) + 1) / sr
    _, t60 = decay_marks(x, sr)
    start = min(t60 + margin_s, end)
    if end - start < 4 * MIN_WINDOW_S:
        return None
    return (start, end)


def texture_window(late, span_s: float = DENSITY_SPAN_S):
    """A stationary slice of the aftersound to count resonances over.

    Not the whole aftersound. Counting needs frequency resolution, so it needs
    a window of a definite length, and `late` runs from 20 dB down to 60 dB
    down — forty decibels of decay inside one transform, which smears every
    peak by however fast the piece is dying. Fixing the length instead makes
    one piece's count comparable with another's, which is the whole point of
    counting.
    """
    start = late[0]
    end = min(late[1], start + span_s)
    if end - start < MIN_WINDOW_S:
        end = start + MIN_WINDOW_S
    return (start, end)


def mode_count(sig: np.ndarray, late, sr: int = 48000, bands=STRUCK_BANDS,
               floor=None):
    """Per band: resonances counted in the aftersound, and whether it was read.

    Returns (count, usable). A band that never stood over the recording's own
    floor is reported as unusable rather than as a number — the texture of a
    floor is the floor's and not the instrument's, and averaging it in scores
    silence as agreement.

    The count saturates: at this window length a field denser than roughly a
    hundred partials in a band reads about the same as one of seventy, and white
    noise reads about thirty. So a reading near or under thirty says "not
    resolvable as separate things", not "thirty modes", and two pieces both up
    at the ceiling are not thereby the same.
    """
    out = np.zeros(len(bands))
    ok = np.zeros(len(bands), dtype=bool)
    if floor is None:
        floor = floor_window(sig, sr)
    if floor is None:
        return out, ok
    win = texture_window(late)
    counts = modal_density(sig, 0, window=win, sr=sr, bands=tuple(bands), notch=False)
    for i, band in enumerate(bands):
        snr = band_snr_db(sig, band, window=win, floor_window=floor, sr=sr)
        if not np.isfinite(snr) or snr < STRUCK_SNR_DB:
            continue
        out[i] = float(counts[tuple(band)][0])
        ok[i] = True
    return out, ok


def _band_db(sig: np.ndarray, band, window, sr: int) -> float:
    """RMS level of one band over one window, in dB."""
    x = np.asarray(sig, dtype=np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    a, b = int(window[0] * sr), int(window[1] * sr)
    seg = x[max(0, a):min(len(x), b)]
    if len(seg) < 64:
        return -400.0
    n = 1
    while n < len(seg):
        n <<= 1
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / sr)
    m = (f >= band[0]) & (f < band[1])
    if not m.any():
        return -400.0
    return 10.0 * np.log10(max(float(S[m].sum()), 1e-30))


def prompt_late(sig: np.ndarray, body, late, sr: int = 48000, bands=STRUCK_BANDS):
    """Per band: its share of the strike, minus its share of the aftersound, in dB.

    A difference of shares rather than of levels, so the overall decay between
    the two windows divides out and what is left is how the *colour* moved. A
    hard piece keeps its top through the aftersound and reads near zero here; a
    piece whose top belongs to the strike alone reads strongly positive in the
    high bands, which is the closed hi-hat that measured every band level right
    and sounded like a small drum.

    Returns (value, alive), where `alive` is whether the band was there AT THE
    STRIKE. A band that dies between the two windows is not dropped — it is the
    finding, and the whole reason this exists. Its aftersound level is floored
    at `DEAD_DB` under the window's own total instead, so vanishing reads as the
    largest movement there is rather than as the estimator declining to answer.
    An earlier version gated on both windows and threw exactly this case away:
    a piece whose 2-4 kHz band went from 40 dB down at the strike to 163 down
    afterwards was reported as having no readable band there.

    A band missing from BOTH windows reads zero here, correctly — nothing moved.
    That is a hole in the spectrum rather than a colour that shifted, and it is
    what `balance` charges, over these same two windows.
    """
    out = np.zeros(len(bands))
    alive = np.zeros(len(bands), dtype=bool)
    b_db = np.array([_band_db(sig, b, body, sr) for b in bands])
    l_db = np.array([_band_db(sig, b, late, sr) for b in bands])
    b_tot = 10.0 * np.log10(max(float(np.sum(10.0 ** (b_db / 10.0))), 1e-30))
    l_tot = 10.0 * np.log10(max(float(np.sum(10.0 ** (l_db / 10.0))), 1e-30))
    for i in range(len(bands)):
        if b_db[i] < b_tot - DEAD_DB:
            continue
        late_share = max(l_db[i] - l_tot, -DEAD_DB)
        out[i] = (b_db[i] - b_tot) - late_share
        alive[i] = True
    return out, alive
