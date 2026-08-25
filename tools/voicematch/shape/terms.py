"""The six comparisons, each answering a question the others cannot.

They are kept apart because they name different repairs, and combined as
energies so that none can be bought with another at a discount. A fit that
improves the tone by dulling the strike should read as no improvement, and with
a single blended number it reads as progress.

    spectrum   the two pictures, cell by cell. The tone.
    onset      the first 350 ms through a transform short enough to see it.
    residue    energy away from the played note's own partials, as a ratio.
    invariance what is left of every note's residue once each is levelled --
               a frequency that answers whatever you strike is a resonator
               singing its own pitch, which is what a bell is.
    release    what the damper leaves behind after note-off.
    balance    which bands carry the render's energy, as a share of its own
               total, in the body of the note and again in the aftersound.

Each of the last five exists because the first was measured to be blind to it.
The onset is six tenths of one percent of the spectrogram's cells, so every fit
run on the picture alone dulled the attack. The residue escapes through the bed
mask, which lets a low ring sit under a treble note for nearly nothing. The
release escapes through the weight floor, since a released note ringing seventy
decibels under its own peak is weighted at three percent. And the invariance
term escapes all of them, because a resonator that answers every note equally is
never a large error on any one of them.

`balance` closes a gap of a different kind. Every other term here is one-sided
or floored somewhere, and all of them in the same direction: a model that rings
LESS than the instrument pays almost nothing. Measured, a set fitted without
this term carried 53 dB less above four kilohertz in the aftersound than the
instrument, half the audible partials in the mid register, and 23 dB less in the
bottom octave -- and scored better for all of it.
"""

from __future__ import annotations

import numpy as np

#: Onset transform: short enough that its first frame is inside the strike.
ONSET_NFFT, ONSET_HOP = 256, 64
ONSET_SPAN = 0.35
ONSET_BANDS = ((60, 250), (250, 1000), (1000, 3000), (3000, 8000), (8000, 16000))
ONSET_CLIP = 24.0

RESIDUE_WINDOWS = ((0.8, 3.0), (3.0, 7.0))
RESIDUE_CLIP = 20.0
INVARIANCE_CLIP = 24.0
RELEASE_CLIP = 30.0

_OWIN = np.hanning(ONSET_NFFT)


def onset_stats(sig: np.ndarray, sr: int = 48000, start: float = 0.1):
    """Band level over the first 60 ms and time-to-rise, per band.

    A separate and much shorter transform than either analysis scale, because
    neither can see this: the long window's first frame is centred at 85 ms, by
    which time the strike is over.

    The rise is the first crossing of six decibels under the peak, not the peak
    itself. Both sides beat hard in the first tenths of a second -- several
    unisons and a dozen partials inside one band -- so the largest sample is as
    likely to be a beat maximum as the end of the rise, and an argmax on it
    reports the beat period instead.
    """
    freq = np.fft.rfftfreq(ONSET_NFFT, 1.0 / sr)
    sel = [np.where((freq >= lo) & (freq < hi))[0] for lo, hi in ONSET_BANDS]
    i0 = int(start * sr)
    x = np.ascontiguousarray(sig[i0:i0 + int(ONSET_SPAN * sr)])
    cols = (len(x) - ONSET_NFFT) // ONSET_HOP + 1
    fr = np.lib.stride_tricks.as_strided(
        x, shape=(cols, ONSET_NFFT),
        strides=(x.strides[0] * ONSET_HOP, x.strides[0])) * _OWIN
    p = np.abs(np.fft.rfft(fr, axis=1)) ** 2
    n60 = max(1, int(0.06 * sr / ONSET_HOP))
    lvl, rise = [], []
    for s in sel:
        e = p[:, s].sum(axis=1)
        lvl.append(10 * np.log10(max(float(e[:n60].mean()), 1e-30)))
        thr = e.max() * 10.0 ** (-0.6)
        i = int(np.argmax(e >= thr)) if (e >= thr).any() else len(e) - 1
        rise.append((float(i) + 1.0) * ONSET_HOP / sr * 1000.0)
    return np.array(lvl), np.array(rise)


def residue_ratio(spectro, S: np.ndarray, harmonic: np.ndarray, scale: int = 0):
    """Energy away from the played note's partials over energy on them, in dB.

    A ratio inside one render, so neither a gain nor a bed enters on the model
    side. That is the point of having it: the bed comparison lets a resonator
    put a low ring under a treble note for almost nothing, and a search will
    take that offer. This term does not care what the reference's floor is, only
    whether the model rings at frequencies the struck string cannot account for.
    """
    P = 10.0 ** (S / 10.0)
    out = []
    for t0, t1 in RESIDUE_WINDOWS:
        c = spectro.columns(scale, S.shape[1], t0, t1)
        if not c.any():
            out.append(0.0)
            continue
        col = P[:, c].mean(axis=1)
        out.append(10 * np.log10(max(float(col[~harmonic].sum()), 1e-30)
                                 / max(float(col[harmonic].sum()), 1e-30)))
    return np.array(out)


def residue_valid(spectro, clean: np.ndarray, bed: np.ndarray, harmonic: np.ndarray,
                  scale: int = 0, margin: float = 10.0):
    """Windows where the reference still has a note to take a ratio of.

    Once a note has decayed into its own recorded floor the ratio stops being a
    property of the instrument: the harmonic rows hold noise, the residue rows
    hold the same noise, and the quotient is about one whatever was played.
    Scored anyway it becomes a target the model is asked to match, and matching
    a noise floor's self-ratio is how a resonator that rings for twenty seconds
    gets rewarded.
    """
    Pc, Pb = 10.0 ** (clean / 10.0), 10.0 ** (bed / 10.0)
    out = []
    for t0, t1 in RESIDUE_WINDOWS:
        c = spectro.columns(scale, clean.shape[1], t0, t1)
        if not c.any():
            out.append(False)
            continue
        h = float(Pc[:, c].mean(axis=1)[harmonic].sum())
        b = float(Pb[:, c].mean(axis=1)[harmonic].sum())
        out.append(10 * np.log10(max(h, 1e-30) / max(b, 1e-30)) > margin)
    return np.array(out)


def residue_curve(spectro, S: np.ndarray, harmonic: np.ndarray, scale: int = 0,
                  window=(0.8, 3.0)) -> np.ndarray:
    """The sustain spectrum on its own level, with the note's partials removed."""
    c = spectro.columns(scale, S.shape[1], *window)
    col = (10.0 ** (S / 10.0))[:, c].mean(axis=1)
    out = 10 * np.log10(np.maximum(col, 1e-30) / max(float(col.max()), 1e-30))
    out[harmonic] = -400.0
    return out


def invariant_floor(curves) -> np.ndarray:
    """What survives in every note's residue once each is put on its own level.

    A bell is not a timbre but a relationship: the same frequencies answer
    whatever you strike. So the quantity is the row-wise minimum across notes.

    Taken on the residue rather than on the whole spectrum, which is what makes
    it usable on a corpus of octaves at all -- a plain across-note minimum would
    report every shared harmonic as note-invariant. Removing each note's own
    partials first removes exactly that, and it also removes the part of any
    partial-mask error that follows the note, leaving errors that do not.
    """
    return np.min(np.stack(curves), axis=0)


#: Aftersound window the recurrence term reads, and how far a row must stand
#: over its neighbourhood to count as a peak there.
RECUR_WINDOW = (2.0, 5.0)
RECUR_PROMINENCE_DB = 6.0
#: Percentage points of recurrence excess above which the term stops growing.
RECUR_CLIP = 100.0


def peak_rows(spectro, S, harmonic, window=RECUR_WINDOW,
              prominence_db=RECUR_PROMINENCE_DB, scale: int = 0):
    """Rows this note's aftersound peaks at, with its own partials removed.

    Half of the recurrence term; the other half is counting how many notes
    share a row, which only the caller can do because only it has them all.

    The partial mask handed in must be capped at a resolvable partial index --
    a quarter-tone mask fuses into a continuous band above about the sixteenth,
    so an uncapped one marks the whole upper spectrum of a bass note as the
    string and leaves this nothing to look at.
    """
    c = spectro.columns(scale, S.shape[1], window[0], window[1])
    if not c.any():
        return np.zeros(S.shape[0], dtype=bool)
    col = S[:, c].mean(axis=1)
    w = max(9, (len(col) // 24) | 1)
    pad = np.pad(col, w // 2, mode="edge")
    local = np.array([np.median(pad[i:i + w]) for i in range(len(col))])
    return (col > local + prominence_db) & ~harmonic


#: Octave bands the balance term compares, from the bottom of a keyboard up.
BALANCE_BANDS = ((30, 60), (60, 125), (125, 250), (250, 500), (500, 1000),
                 (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000))
#: Body of the note, then the aftersound. Two windows because "thin" and
#: "the tail is dead" are different complaints with different levers.
BALANCE_WINDOWS = ((0.2, 0.8), (2.5, 6.0))
BALANCE_CLIP = 15.0
#: A band this far under the reference's own total in a window holds nothing
#: the reference can be said to have, and is not asked about.
BALANCE_LIVE_DB = -70.0


def band_balance(spectro, S: np.ndarray, window, bands=BALANCE_BANDS, scale: int = 0):
    """Each band's share of this render's energy in a window, in dB.

    Against the render's own total rather than an absolute level, so two renders
    are comparable with no gain removed and none able to hide anything. That is
    what makes this the right shape for the question the ear keeps asking: a
    model can be thirteen decibels loud and still be thin, and every measure
    that starts by aligning levels answers a different question.

    It is also the one term here that is two-sided everywhere. The cell
    comparison weights a cell by the louder of the two sides and floors it
    eighty-five decibels under the note's peak, which puts the whole aftersound
    at or near the weight floor; the residue term is gated off once the
    reference has decayed; the invariance and release terms only charge for
    excess. Between them nothing costs a model anything for having LESS than the
    instrument late in the note, and a search will take that offer -- measured,
    a fitted set carried 53 dB less above four kilohertz in the aftersound than
    the instrument and scored better for it.
    """
    hz = spectro.rows_hz(scale)
    c = spectro.columns(scale, S.shape[1], *window)
    col = (10.0 ** (S / 10.0))[:, c].mean(axis=1)
    total = max(float(col.sum()), 1e-30)
    return np.array([
        10 * np.log10(max(float(col[(hz >= lo) & (hz < hi)].sum()), 1e-30) / total)
        for lo, hi in bands])


def _rms_db(seg: np.ndarray, floor: float = 1e-14) -> float:
    """RMS in dB, accumulated in double.

    Signals are carried as float32 -- that is what the renderer produces and
    twice the bytes buys nothing -- but a square is not safe in that width. A
    candidate whose parameters send the voice unstable renders samples large
    enough that squaring them overflows, and the statistic then reads as
    infinity rather than as the very large number it is. Such a candidate is
    rejected either way; accumulating in double means it is rejected for its
    actual value rather than for a saturation.
    """
    if not seg.size:
        return -140.0
    q = np.asarray(seg, dtype=np.float64)
    return 20 * np.log10(max(float(np.sqrt(np.mean(q * q))), floor))


def release_stats(sig: np.ndarray, pre, post, sr: int = 48000):
    """Level just before and well after note-off, both relative to the peak."""
    peak = 20 * np.log10(max(float(np.max(np.abs(np.asarray(sig, dtype=np.float64)))),
                             1e-14))
    return (_rms_db(sig[int(pre[0] * sr):int(pre[1] * sr)]) - peak,
            _rms_db(sig[int(post[0] * sr):int(post[1] * sr)]) - peak)


def held_db(sig: np.ndarray, window=(0.4, 1.4), sr: int = 48000) -> float:
    """The level the whole-set gain alignment is taken from."""
    return _rms_db(sig[int(window[0] * sr):int(window[1] * sr)], floor=1e-12)
