"""Stationary lines a voice latches on at every note: found per capture, masked from the readings.

A recorded voice can carry narrow lines at fixed frequencies whatever note plays (the
concert grands': 10885.8, 10947.6, 17132.7, 17983.9, 18210.1 Hz and copies 3900 Hz
above them). They are the voice's own, not the recording room's floor, so they are
recorded per capture and masked from partial location and the band readings rather
than subtracted as a floor.
"""

from __future__ import annotations

from typing import NamedTuple

import numpy as np
from metrics_partials import MAX_FIT_PARTIALS, fit_partial_series, partial_hz

#: Reason a capture's rows carry for the lines they were read around.
STATIONARY_LINES_REASON = "stationary lines, voice-latched"
#: Lowest frequency a line is searched from; the grands' lowest is 10.9 kHz, the bank's 1.1 kHz.
LINE_MIN_HZ = 1000.0
#: Height over the running median of the spectrum, in dB, that makes a bin a line.
LINE_PROMINENCE_DB = 15.0
#: Half-width, in Hz, of the running median the prominence is taken against.
LINE_FLOOR_HALF_HZ = 200.0
#: A line this far under the segment's RMS is arithmetic residue, not a recorded line.
LINE_BELOW_RMS_DB = 110.0
#: Two takes' lines are one line within this many Hz; a line's spread over takes is 0.1-0.5 Hz.
LINE_MATCH_HZ = 0.4
#: Share of a capture's takes a line has to stand in, and the fewest takes that can say so.
LINE_SHARE = 0.75
LINE_MIN_TAKES = 5
#: Where in the gate the held part is read for lines: the late half, after the note's own partials fall.
LINE_WINDOW = (0.5, 0.95)
#: Bins either side of a line that are replaced, and the bins either side of those the replacement is taken from.
MASK_BINS = 4
MASK_AROUND_BINS = 40


class Line(NamedTuple):
    """A stationary line: where, and in how many of a capture's takes."""

    hz: float
    takes: int


def lines_in(seg: np.ndarray, sr: int) -> tuple[np.ndarray, np.ndarray]:
    """Narrow lines over `LINE_MIN_HZ` -> (Hz, level in dB of a full-scale sine's equivalent)."""
    w = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(np.asarray(seg, dtype=np.float64) * w)) * 2.0 / w.sum()
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    db = 20.0 * np.log10(np.maximum(mag, 1e-15))
    bin_hz = float(freqs[1])
    half = int(LINE_FLOOR_HALF_HZ / bin_hz)
    peaks = np.nonzero((db[1:-1] > db[:-2]) & (db[1:-1] >= db[2:]))[0] + 1
    peaks = peaks[(freqs[peaks] >= LINE_MIN_HZ) & (freqs[peaks] < 0.47 * sr)]
    step = max(1, half // 4)
    centres = np.arange(0, len(db), step)
    median = np.array([np.median(db[max(0, i - half) : i + half + 1]) for i in centres])
    floor = np.interp(peaks, centres, median)
    peaks = peaks[db[peaks] - floor >= LINE_PROMINENCE_DB]
    a, b, c = db[peaks - 1], db[peaks], db[peaks + 1]
    den = a - 2.0 * b + c
    delta = np.where(np.abs(den) > 1e-12, 0.5 * (a - c) / np.where(den == 0.0, 1.0, den), 0.0)
    return freqs[peaks] + delta * bin_hz, b - 0.25 * (a - c) * delta


def held_lines(
    seg: np.ndarray, sr: int, gate_s: float, preroll_s: float
) -> tuple[np.ndarray, np.ndarray] | None:
    """`lines_in` over a take's late held part, or None where there is nothing to read."""
    a, b = (int((preroll_s + f * gate_s) * sr) for f in LINE_WINDOW)
    held = seg[a:b]
    if len(held) < sr // 4 or not np.any(held):
        return None
    hz, level = lines_in(held, sr)
    rms = 20.0 * np.log10(np.sqrt(np.mean(np.asarray(held, dtype=np.float64) ** 2)) + 1e-30)
    keep = level > rms - LINE_BELOW_RMS_DB
    return hz[keep], level[keep]


def stationary_lines(takes: list[np.ndarray]) -> list[Line]:
    """Lines standing at one frequency in `LINE_SHARE` of the takes, none under `LINE_MIN_TAKES` takes.

    `takes` holds each take's line frequencies; a capture with fewer takes than
    `LINE_MIN_TAKES` cannot tell a voice's line from a note's partial and has none.
    """
    if len(takes) < LINE_MIN_TAKES:
        return []
    found = sorted((float(f), i) for i, hz in enumerate(takes) for f in hz)
    out: list[Line] = []
    j = 0
    while j < len(found):
        group = [found[j]]
        j += 1
        while j < len(found) and found[j][0] - group[0][0] <= 2.0 * LINE_MATCH_HZ:
            group.append(found[j])
            j += 1
        seen = {i for _, i in group}
        if len(seen) >= max(LINE_MIN_TAKES, LINE_SHARE * len(takes)):
            out.append(Line(round(float(np.median([f for f, _ in group])), 1), len(seen)))
    return out


def mask_lines(freqs: np.ndarray, mag: np.ndarray, lines_hz: list[float]) -> np.ndarray:
    """`mag` with each line's bins replaced by the median of the bins around them, or `mag` itself."""
    if not lines_hz or freqs.size < 2:
        return mag
    bin_hz = float(freqs[1] - freqs[0])
    out = np.array(mag, dtype=np.float64)
    for hz in lines_hz:
        k = round(hz / bin_hz)
        if k - MASK_AROUND_BINS < 0 or k + MASK_AROUND_BINS >= len(out):
            continue
        around = np.r_[
            mag[k - MASK_AROUND_BINS : k - MASK_BINS - 1],
            mag[k + MASK_BINS + 2 : k + MASK_AROUND_BINS + 1],
        ]
        out[k - MASK_BINS : k + MASK_BINS + 1] = float(np.median(around))
    return out


#: How many bins either side of a partial of the note's own series a stationary line is still taken
#: for that partial. An organ's line that is also a harmonic of every note a tritone apart sits on one bin.
LINE_ON_PARTIAL_BINS = 3.0


def lines_off_the_series(
    freqs: np.ndarray,
    mag: np.ndarray,
    f0_seed: float,
    h1_mag: float,
    sr: int,
    modal: bool,
    lines_hz: list[float],
) -> list[float]:
    """The stationary lines that are not also a partial of this note's own (unmasked) series."""
    if not lines_hz or modal:
        return lines_hz
    f0, b, _ = fit_partial_series(freqs, mag, f0_seed, h1_mag, sr)
    tol = LINE_ON_PARTIAL_BINS * float(freqs[1] - freqs[0])
    series = [partial_hz(f0, n, b) for n in range(1, MAX_FIT_PARTIALS + 1)]
    return [hz for hz in lines_hz if all(abs(hz - p) > tol for p in series)]


#: Reason a row carries for the body-band lines it was read around. They follow the note's level, so
#: they are in the source and not the recording chain; this is not a floor and not hum.
BODY_LINES_REASON = "recurring fixed-frequency lines in the source"
#: Height over the running median of the body band, in dB, that makes a bin a body line. Over the
#: clarinet's notes from n54 up its lines stand 13-68 dB over it; a broadband bed has none above 12.
BODY_LINE_PROMINENCE_DB = 12.0
#: Half-width, in Hz, of the running median the prominence is taken against.
BODY_LINE_FLOOR_HALF_HZ = 10.0
#: Two notes' lines are one line within this many Hz of the first (one step of the padded grid). The
#: clarinet's recurring lines read 0.00-0.25 Hz apart, chance neighbours in the flute and violin 0.5-1.0.
BODY_LINE_MATCH_HZ = 0.3
#: Fewest notes, among those whose band reaches it, a line recurs on; the clarinet's 161 Hz line stands on 3 of 10.
BODY_LINE_MIN_NOTES = 3
#: A line this close to the note's own f0/2, f0/3 or f0/4 is the note's, not a recurring one.
BODY_LINE_SUBHARMONICS = (2, 3, 4)
BODY_LINE_SUBHARMONIC_HZ = 1.0
#: Mains multiples (50 and 60 Hz up to the eighth, within 0.12 Hz per multiple) are hum, which the
#: floor declares, not a line in the source.
MAINS_HZ = (50.0, 60.0)
MAINS_MAX_MULTIPLE = 8
MAINS_TOLERANCE_HZ = 0.12
#: A peak this close to a line this much stronger is that line's Hann sidelobe (first sidelobes
#: stand 2.5 and 3.5 bins out, 31 and 41 dB down), not a line.
BODY_LINE_SIDELOBE_HZ = 4.5
BODY_LINE_SIDELOBE_DB = 25.0
#: Zero-padding of the body window's spectrum, so a line is placed to a fraction of a bin.
BODY_LINE_PAD = 4
#: A line is read at the strongest bin this close to it (half a bin of the window's halves).
BODY_LINE_HALF_BINS_HZ = 2.0
#: Median drift, in dB over the window, within which a recurring line is sustained. The clarinet's
#: lines read -1.2 to +1.0 dB, the piano's soundboard modes, which decay, -8 to -34.
BODY_LINE_MAX_DRIFT_DB = 3.0


def _on_mains(hz: float) -> bool:
    """Whether `hz` sits on a multiple of the mains frequency."""
    for base in MAINS_HZ:
        m = round(hz / base)
        if 1 <= m <= MAINS_MAX_MULTIPLE and abs(hz - m * base) <= MAINS_TOLERANCE_HZ * m:
            return True
    return False


def _line_level_db(seg: np.ndarray, sr: int, hz: float) -> float:
    """Level of the strongest bin within `BODY_LINE_HALF_BINS_HZ` of `hz` in a padded Hann spectrum."""
    power = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), len(seg) * BODY_LINE_PAD)) ** 2
    freqs = np.fft.rfftfreq(len(seg) * BODY_LINE_PAD, 1.0 / sr)
    near = np.abs(freqs - hz) <= BODY_LINE_HALF_BINS_HZ
    return float(10.0 * np.log10(power[near].max() + 1e-30))


def body_lines_in(
    seg: np.ndarray, sr: int, f0: float, lo_hz: float = 40.0
) -> list[tuple[float, float]]:
    """Narrow lines in `lo_hz` .. 0.7 f0 of a note's body window -> [(Hz, drift in dB)].

    Not on the note's own subharmonics. The drift is the line's level in the window's second
    half over its first, which is what tells a sustained line from a resonance that decays.
    """
    n = len(seg)
    seg = np.asarray(seg, dtype=np.float64)
    power = np.abs(np.fft.rfft(seg * np.hanning(n), n * BODY_LINE_PAD)) ** 2
    freqs = np.fft.rfftfreq(n * BODY_LINE_PAD, 1.0 / sr)
    db = 10.0 * np.log10(power + 1e-30)
    half = int(BODY_LINE_FLOOR_HALF_HZ / float(freqs[1]))
    peaks = np.nonzero((db[1:-1] > db[:-2]) & (db[1:-1] >= db[2:]))[0] + 1
    peaks = peaks[(freqs[peaks] >= lo_hz) & (freqs[peaks] < 0.7 * f0)]
    base = np.array([np.median(db[max(0, i - half) : i + half + 1]) for i in peaks])
    peaks = peaks[db[peaks] - base >= BODY_LINE_PROMINENCE_DB] if peaks.size else peaks
    own = [f0 / k for k in BODY_LINE_SUBHARMONICS]
    out = []
    for i in peaks:
        hz = freqs[i]
        stronger = db[peaks] - db[i] >= BODY_LINE_SIDELOBE_DB
        if np.any(stronger & (np.abs(freqs[peaks] - hz) <= BODY_LINE_SIDELOBE_HZ)):
            continue
        if _on_mains(float(hz)):
            continue
        if all(abs(hz - s) > BODY_LINE_SUBHARMONIC_HZ for s in own):
            drift = _line_level_db(seg[n // 2 :], sr, hz) - _line_level_db(seg[: n // 2], sr, hz)
            out.append((float(hz), drift))
    return out


def _steady_body_line_groups(
    notes: list[tuple[float, list[tuple[float, float]]]],
) -> list[tuple[float, list[int]]]:
    """(Hz, indices into `notes`) of every line standing at one absolute frequency and sustained."""
    found = sorted((hz, drift, i) for i, (_, lines) in enumerate(notes) for hz, drift in lines)
    out: list[tuple[float, list[int]]] = []
    j = 0
    while j < len(found):
        group = [found[j]]
        j += 1
        while j < len(found) and found[j][0] - group[0][0] <= BODY_LINE_MATCH_HZ:
            group.append(found[j])
            j += 1
        steady = abs(float(np.median([d for _, d, _ in group]))) <= BODY_LINE_MAX_DRIFT_DB
        if steady:
            hz = round(float(np.median([h for h, _, _ in group])), 1)
            out.append((hz, sorted({i for _, _, i in group})))
    return out


def recurring_body_line_groups(
    notes: list[tuple[float, list[tuple[float, float]]]],
) -> list[tuple[float, list[int]]]:
    """(Hz, indices into `notes`) of the lines `recurring_body_lines` finds."""
    return [
        (hz, idx) for hz, idx in _steady_body_line_groups(notes) if len(idx) >= BODY_LINE_MIN_NOTES
    ]


def recurring_body_lines(notes: list[tuple[float, list[tuple[float, float]]]]) -> list[Line]:
    """Sustained lines standing at one absolute frequency on `BODY_LINE_MIN_NOTES` notes of any f0.

    `notes` holds each note's (f0, `body_lines_in`). The count is of notes whatever their f0;
    a line whose median drift over its notes exceeds `BODY_LINE_MAX_DRIFT_DB` is a decaying
    resonance (a body mode), not a sustained line, and does not recur.
    """
    return [Line(hz, len(idx)) for hz, idx in recurring_body_line_groups(notes)]


#: Why a row carries a sustained line seen on one note too few to be masked.
BODY_LINE_MISSED_REASON = f"recurs on {BODY_LINE_MIN_NOTES - 1} notes; N={BODY_LINE_MIN_NOTES}"


def missed_body_lines(
    notes: list[tuple[float, list[tuple[float, float]]]],
) -> list[tuple[float, list[int]]]:
    """(Hz, indices into `notes`) of sustained lines on exactly one note fewer than `BODY_LINE_MIN_NOTES`.

    The same pass as `recurring_body_lines`; these are recorded as known misses, never masked.
    """
    return [
        (hz, idx)
        for hz, idx in _steady_body_line_groups(notes)
        if len(idx) == BODY_LINE_MIN_NOTES - 1
    ]


#: Seconds past the gate's end before the tail is read as idle, so the release is not read.
IDLE_AFTER_RELEASE_S = 0.25
#: Shortest preroll or tail, in seconds, a line's idle level is read on.
IDLE_MIN_S = 0.05
#: An idle line within this many dB of its level under the note is the same tone, standing idle.
IDLE_SAME_LEVEL_DB = 6.0
PRESENCE_IDLE = "idle"
PRESENCE_NOTE_ONLY = "note_only"
PRESENCE_UNREADABLE = "unreadable"


def line_amplitude_dbfs(seg: np.ndarray, sr: int, hz: float) -> float | None:
    """Amplitude, in dBFS, of a sinusoid at `hz` in `seg`, or None where `seg` is digital silence."""
    seg = np.asarray(seg, dtype=np.float64)
    if not np.any(seg):
        return None
    window = np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(seg * window, len(seg) * BODY_LINE_PAD))
    freqs = np.fft.rfftfreq(len(seg) * BODY_LINE_PAD, 1.0 / sr)
    near = np.abs(freqs - hz) <= BODY_LINE_HALF_BINS_HZ
    return float(20.0 * np.log10(2.0 * spec[near].max() / window.sum() + 1e-30))


def line_presence(note_dbfs: list[float], idle: list[np.ndarray], sr: int, hz: float) -> dict:
    """Whether a masked line stands in the idle preroll or tail at its level under the note.

    `note_dbfs` holds the line's amplitude in the body window of each note it was found on,
    `idle` those notes' preroll and post-release segments. The mask does not read this.
    """
    readable = [seg for seg in idle if len(seg) >= int(IDLE_MIN_S * sr)]
    note_db = float(np.median(note_dbfs))
    levels = [line_amplitude_dbfs(seg, sr, hz) for seg in readable]
    heard = [db for db in levels if db is not None]
    idle_db = max(heard) if heard else None
    if not readable:
        state = PRESENCE_UNREADABLE
    elif idle_db is not None and idle_db >= note_db - IDLE_SAME_LEVEL_DB:
        state = PRESENCE_IDLE
    else:
        state = PRESENCE_NOTE_ONLY
    return {
        "presence": state,
        "note_dbfs": round(note_db, 1),
        "idle_dbfs": None if idle_db is None else round(idle_db, 1),
    }
