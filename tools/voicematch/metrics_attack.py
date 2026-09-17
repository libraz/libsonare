"""Band balance and narrowband rings through a note's attack."""

from __future__ import annotations

import numpy as np
from smf import Note

# The attack window, and how it is cut up. Six 20 ms slices covering the first
# 120 ms: long enough to reach past the hammer contact and the bloom, short
# enough that a burst confined to the first frame is not averaged away. A
# whole-timeline spectral distance cannot see one of these — a 43 ms excess of
# +53 dB at 20-24 kHz on a note that matches within half a dB everywhere else
# dilutes into the multi-scale term's average and never moves it.
ATTACK_WINDOW_MS = 20.0
ATTACK_WINDOWS = 6
# Bands above the last harmonic anyone models. This is where a strike-noise
# path with the wrong filter order announces itself: a single pole falls at
# 6 dB/octave, which no radiating mechanism does, and leaves a burst still
# 23 dB up at 16 kHz that the ear reads as a tick rather than as brightness.
ATTACK_BANDS_HZ = ((4000.0, 8000.0), (8000.0, 12000.0), (12000.0, 16000.0),
                   (16000.0, 20000.0), (20000.0, 24000.0))

# What the low attack bands are measured against, and why it is not the
# window's own broadband level.
#
# A share of the total is compositional: the bands sum to unity, so whichever
# band is loudest sets the denominator for all of them and a defect confined to
# one band fabricates a delta in every other. Measured on a band-limited 40 Hz
# excess with nothing else wrong, the three bands from 200 Hz up each reported
# 4.92 dB of difference that was not there; against this anchor they report
# exactly zero, and the band the defect is actually in reports 48.
#
# 200 Hz to 4 kHz holds real signal for every note on the keyboard — the eighth
# through hundred-and-forty-fifth partial of an A0, the first and second of a
# C7 — and sits above the bands a bass defect lives in, so that defect cannot
# move its own reference. It is a partition point rather than an exclusion: the
# three bands inside it are read as a share OF it, which is meaningful, since
# only a band identical to the anchor would be degenerate.
#
# `attack_bands` deliberately does not use it — see the note there. Its 20 ms
# slice is shorter than one cycle of the frequencies this anchor is meant to be
# independent of, so the leakage lands in the anchor and the fix inverts.
ATTACK_ANCHOR_HZ = (200.0, 4000.0)


def _anchor_power(freqs: np.ndarray, power: np.ndarray, sr: int) -> float:
    """Power in the reference band every attack measure is expressed against."""
    lo, hi = ATTACK_ANCHOR_HZ
    if lo >= sr / 2:
        return 0.0
    mask = (freqs >= lo) & (freqs < min(hi, sr / 2))
    return float(power[mask].sum())


def _bands_against_anchor(freqs: np.ndarray, power: np.ndarray, sr: int,
                          bands, anchor: float) -> list[float | None]:
    """Each band's level relative to `anchor`, in dB; None above Nyquist."""
    out: list[float | None] = []
    for lo, hi in bands:
        if lo >= sr / 2:
            out.append(None)
            continue
        mask = (freqs >= lo) & (freqs < min(hi, sr / 2))
        ratio = float(power[mask].sum()) / anchor
        out.append(round(float(10.0 * np.log10(max(ratio, 1e-12))), 2))
    return out


def attack_bands(mono: np.ndarray, sr: int, note: Note,
                 onset: float) -> list[float | None]:
    """High-band balance through the attack: one value per band per time slice.

    Each value is the band's level relative to that slice's own broadband level,
    so the measure says nothing about how loud the attack was and everything
    about its spectral tilt. That is what makes it usable on the RMS-normalised
    signal the rest of the metric set reads, and what makes it comparable
    between a model and a reference captured at different gains.

    **Not the shared anchor `attack_low_bands` uses, and the difference is the
    window rather than a preference.** A share of the total is compositional, so
    a defect in one band does move the others, and an anchor outside both band
    sets is what fixes that — at 50 ms. It does not survive a 20 ms slice: a
    slice is shorter than one cycle of the frequencies the low measure is
    watching, so a 40 Hz excess leaks straight into a 200 Hz - 4 kHz anchor and
    contaminates the reference it was supposed to be independent of. Measured on
    a band-limited low-frequency defect, anchoring made this term's worst band
    move 6.83 dB where the share of the total moved 1.18. The anchor is kept
    where it works and not carried here for symmetry.

    `onset` is where the attack actually begins, which is not necessarily where
    the note was scheduled — see `note_onset`. A slice is 20 ms, so a model that
    speaks 30 ms after its note-on would otherwise have its first two slices
    compared against a reference's silence.

    Bands above Nyquist and slices past the end of the render come back None
    rather than as a floor value, so a shorter render contributes nothing to the
    term instead of contributing a fabricated match.
    """
    win = int(sr * ATTACK_WINDOW_MS / 1000.0)
    on = int(onset * sr)
    out: list[float | None] = []
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    window = np.hanning(win)
    for i in range(ATTACK_WINDOWS):
        a = on + i * win
        seg = mono[a : a + win]
        if len(seg) < win:
            out.extend([None] * len(ATTACK_BANDS_HZ))
            continue
        power = np.abs(np.fft.rfft(seg * window)) ** 2
        total = float(power.sum())
        if total <= 0.0:
            out.extend([None] * len(ATTACK_BANDS_HZ))
            continue
        out.extend(_bands_against_anchor(freqs, power, sr, ATTACK_BANDS_HZ, total))
    return out


# The attack's low end, and why it is one window rather than six slices. The
# high-band measure above trades frequency resolution for time resolution,
# which is the right trade from 4 kHz up: a tick is a burst confined to one
# slice, and 50 Hz bins are ample there. At the bottom of the range that trade
# inverts. The two bands that carry a bass note's attack — 20-60 and 60-200 Hz
# — are one bin and three at a 20 ms slice's resolution, so they cannot be told
# apart at all. A single 50 ms window resolves 20 Hz instead, and is still
# short enough that a 30 ms strike event dominates it rather than averaging
# into the sustain behind it.
ATTACK_LF_WINDOW_MS = 50.0
# Bands from the bottom of the audible range up to where ATTACK_BANDS_HZ takes
# over, so the two measures tile the spectrum without either one paying for the
# same error twice. DC sits outside the lowest band deliberately: a constant
# offset is a property of whatever captured the signal, not of the instrument.
#
# The lowest band contains the fundamental on the bottom octave — A0 is 27.5 Hz
# — and that is not an oversight. This is the attack's band balance, not a
# sub-fundamental measure, and the bottom octave is exactly where the defect it
# exists to catch was found: a bass note carrying 43 dB more 20-60 Hz than its
# reference while sitting 15-20 dB under it above 200 Hz, which is a note that
# is felt and never heard. Nothing else here could see it. The harmonic ladder
# is h1-normalised, so an excess at h1 is invisible by construction; every
# other pitched metric reads the sustain window, which a strike event is over
# before it opens; and a whole-timeline spectral distance averages a 50 ms
# event away.
# The two bands below the anchor are the measurement; the three inside it are a
# partition of the anchor itself, which is not degenerate — a band is only
# self-referential if it IS the anchor — and says how the mid weight is
# distributed. Only a band that equals ATTACK_ANCHOR_HZ would have to go.
ATTACK_LF_BANDS_HZ = ((20.0, 60.0), (60.0, 200.0), (200.0, 800.0),
                      (800.0, 2000.0), (2000.0, 4000.0))


def attack_low_bands(mono: np.ndarray, sr: int, note: Note,
                     onset: float) -> list[float | None]:
    """Low- and mid-band balance through the attack: one value per band.

    Each value is the band's level relative to `ATTACK_ANCHOR_HZ`, exactly as
    `attack_bands` reports the top end and against the same reference, so the
    two measures are commensurate and neither prices the other's defect. The
    measure says nothing about how loud the attack was and everything about its
    spectral tilt, which is what makes it usable on the RMS-normalised signal
    the rest of the metric set reads.

    `onset` is where the attack actually begins — see `note_onset`.

    A render too short to fill the window, one with nothing in the anchor band,
    and a band above Nyquist all come back None rather than as a floor value,
    so they contribute nothing to the term instead of contributing a fabricated
    match.
    """
    win = int(sr * ATTACK_LF_WINDOW_MS / 1000.0)
    on = int(onset * sr)
    seg = mono[on : on + win]
    if len(seg) < win:
        return [None] * len(ATTACK_LF_BANDS_HZ)
    power = np.abs(np.fft.rfft(seg * np.hanning(win))) ** 2
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    anchor = _anchor_power(freqs, power, sr)
    if anchor <= 0.0:
        return [None] * len(ATTACK_LF_BANDS_HZ)
    return _bands_against_anchor(freqs, power, sr, ATTACK_LF_BANDS_HZ, anchor)


# What a narrowband ring in the attack is, and why it needs its own measure
# rather than another band.
#
# `attack_bands` prices the top end four kilohertz at a time, which is the right
# width for a tilt and the wrong one for a mode: a single lightly-damped
# resonance rung by the strike is a spike a few tens of hertz wide, and a band
# average reports it as a few decibels of extra brightness spread over the whole
# band. The term still charges for it — on this piano it charges the cap — but
# the number it produces says "the 8-12 kHz band is hot", which is not something
# anyone can act on.
#
# So this is a diagnostic and deliberately NOT a loss term. The energy is
# already priced by `hf`; pricing it twice would let one ring outvote the rest
# of the attack, and a peak list is a jumpy thing to hand an optimiser besides.
# What it adds is attribution: which frequency, how far above its neighbours,
# and — via `fixed_resonances` in `loss.py` — whether it sits at the same place
# on every note, which is what separates a resonator mode from a partial.
#
# Set at 40 Hz rather than at the top of the modelled harmonic range, because
# the ring this exists to find is not always up there. A piano's case and frame
# modes live between 40 Hz and 5 kHz, so a 4 kHz floor put the whole of the
# instrument's own structure out of range and left the diagnostic able to see
# only fold-back and undamped filters. Lowering it does not cost precision:
# against a sampled reference the notes below 4 kHz that survive every other
# gate came back four in number and on the reference side only.
ATTACK_PEAK_FLOOR_HZ = 40.0
# The baseline each peak is measured against is the spectrum smoothed over this
# span. Wide enough that a mode cannot lift its own baseline, narrow enough to
# follow the instrument's real rolloff rather than averaging across it.
ATTACK_PEAK_BASELINE_HZ = 2000.0
# How far above that baseline a bin has to stand to be called a peak. A partial
# on a dense low note clears 10 dB routinely; at 15 dB what survives on a piano
# is a partial that dominates its neighbours or a mode that has no business
# being there at all, and the two are told apart by pitch-independence.
ATTACK_PEAK_PROMINENCE_DB = 15.0
# How far below the window's own loudest bin a peak may sit and still be one.
# Prominence is a ratio against the neighbourhood, which says nothing about
# whether either has any signal in it: in a band that is numerically empty the
# smoothed baseline is round-off and anything above it reports tens of decibels
# of prominence over nothing. Measured on a synthetic string whose spectrum is
# genuinely silent between partials, that produced peaks at 80 dB prominence in
# bands 250 dB below the fundamental. Real renders carry a noise floor that
# hides this, which is exactly why it needed a guard rather than a test signal.
ATTACK_PEAK_FLOOR_DB = 80.0


def attack_peaks(mono: np.ndarray, sr: int, note: Note,
                 onset: float) -> list[tuple[float, float]]:
    """Isolated narrowband peaks in the attack, as (frequency Hz, prominence dB).

    Measured over the same span `attack_bands` covers, from the same detected
    `onset`, so a peak found here is a peak that measure is also charging for.
    Prominence is the excess over the locally smoothed spectrum, which makes it
    blind to the overall level and to the instrument's own rolloff — a peak is
    only a peak relative to what sits beside it.

    Returns an empty list when the window cannot be filled, rather than a
    fabricated one: no data and no peaks are different findings, and the caller
    counts notes so it can tell them apart.
    """
    win = int(sr * ATTACK_WINDOW_MS / 1000.0) * ATTACK_WINDOWS
    on = int(onset * sr)
    seg = mono[on : on + win]
    if len(seg) < win:
        return []
    power = np.abs(np.fft.rfft(seg * np.hanning(win))) ** 2
    if float(power.sum()) <= 0.0:
        return []
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    db = 10.0 * np.log10(power + 1e-30)
    bin_hz = float(freqs[1] - freqs[0])
    half = max(1, round(ATTACK_PEAK_BASELINE_HZ / bin_hz / 2.0))
    kernel = np.ones(2 * half + 1)
    # Divided by how many bins each output actually saw, rather than by the
    # kernel width. A plain smoothing pads the ends with zeros, and these are dB
    # — every real value is negative — so the padding pulls the baseline UP
    # towards 0 and the excess down with it. The span is 2 kHz wide, so at the
    # bottom of the spectrum that is most of the window: with the floor at 4 kHz
    # nothing was measured there and it did not matter, and at 40 Hz the first
    # 240 bins would each be judged against a baseline made mostly of padding.
    counts = np.convolve(np.ones_like(db), kernel, mode="same")
    baseline = np.convolve(db, kernel, mode="same") / counts
    excess = db - baseline
    # A bin in an empty band is not a peak however far it stands over its
    # neighbours; see ATTACK_PEAK_FLOOR_DB.
    excess[db < float(db.max()) - ATTACK_PEAK_FLOOR_DB] = 0.0
    lo = int(np.searchsorted(freqs, ATTACK_PEAK_FLOOR_HZ))
    hi = len(freqs) - half            # the smoothing tapers at the very top
    out: list[tuple[float, float]] = []
    i = lo
    while i < hi:
        if excess[i] < ATTACK_PEAK_PROMINENCE_DB:
            i += 1
            continue
        # Walk out to where the peak falls back to two thirds of the threshold,
        # so one resonance is reported once rather than once per bin over it.
        start = i
        while i < hi and excess[i] > ATTACK_PEAK_PROMINENCE_DB * (2.0 / 3.0):
            i += 1
        top = start + int(np.argmax(excess[start:i]))
        out.append((float(freqs[top]), float(excess[top])))
    return out
