"""Vibrato, tremolo, beat, and the width of the fundamental."""

from __future__ import annotations

from typing import NamedTuple

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
#: this is 0.44 s, a cycle and a third of the slowest vibrato — under that the
#: peak found is the window rather than the note.
MOD_MIN_FRAMES = 40
VIBRATO_BAND_HZ = (3.0, 9.0)
BEAT_BAND_HZ = (0.3, 3.0)
#: Fewest bins a band must hold for its peak to be a measurement. Spacing is the
#: frame rate over the frame count, so `MOD_MIN_FRAMES` leaves the beat band one
#: bin wide, where `argmax` returns that bin whatever the note did.
MOD_MIN_BAND_BINS = 2
#: How far either side of the nominal pitch the level tracker looks. Wider than
#: the deepest vibrato any instrument uses, so a vibrato cannot carry the
#: fundamental out of the search range and read as tremolo.
MOD_TRACK_CENTS = 120.0
MOD_TRACK_POINTS = 49
#: Pitch-modulation bands either side of the vibrato band, read off the same
#: demodulated fundamental as the vibrato; 45 Hz clears the engine's 40 Hz LFO
#: ceiling.
FM_SLOW_BAND_HZ = (1.0, 3.0)
FM_FAST_BAND_HZ = (9.0, 45.0)
#: The demodulation lowpass passes up to this fraction of f0 and stops by
#: `FM_STOP_F0_FRACTION`, short of a sub-octave rank (an organ's 16', f0/2 away)
#: as well as the second partial; 0.35 still passes the vibrato band at 27.5 Hz.
FM_LOWPASS_F0_FRACTION = 0.35
FM_STOP_F0_FRACTION = 0.45
FM_HOP_S = 0.005
#: Tapered and then trimmed at both ends of the segment, so its edges do not
#: ring through the lowpass into the track.
FM_EDGE_S = 0.05
#: Where the demodulated level falls this far under its peak, the phase is the
#: floor's rather than the note's, so the track stops there.
FM_FLOOR_DB = 40.0
#: Least power the passband must hold over what the floor around it puts in the
#: same bandwidth. Measured: noise, drums and notes with no fundamental 1-16.5 dB
#: and reading tens of cents; the weakest real vibrato, a low cello, 19.7 dB.
FM_MIN_FUNDAMENTAL_DB = 18.0


def _band_peak(
    spectrum: np.ndarray, rate_hz: float, band: tuple[float, float], n: int
) -> tuple[float, float] | None:
    """Strongest component of a modulation spectrum inside `band`, or None.

    Returns (peak-to-peak amplitude in the input's own units, rate in Hz).
    The amplitude conversion assumes the Hann window the caller applied: a
    sinusoid of amplitude A lands at |X| = A*n/4, so peak-to-peak is 8|X|/n.

    None, rather than the zero this used to return, where the window resolved
    fewer than `MOD_MIN_BAND_BINS` of the band: a zero depth claims the note is
    dead still, which this module's own contract reserves for a note that was
    looked at. A band is covered when the window is long enough for it and not
    because it was named — the longest window here is 2.35 s, so the beat band's
    0.3 Hz edge is under the 0.43 Hz spacing and is never itself reachable.
    """
    freqs = np.fft.rfftfreq(n, 1.0 / rate_hz)
    mask = (freqs >= band[0]) & (freqs <= band[1])
    if int(np.count_nonzero(mask)) < MOD_MIN_BAND_BINS:
        return None
    idx = np.where(mask)[0]
    k = idx[int(np.argmax(spectrum[idx]))]
    return float(8.0 * spectrum[k] / n), float(freqs[k])


def demodulated_pitch(
    seg: np.ndarray, sr: int, f0: float, cutoff: float, stop: float
) -> np.ndarray | None:
    """The fundamental's pitch track in cents from `f0` at `FM_HOP_S`.

    The fundamental is shifted to 0 Hz, lowpassed under `cutoff` with a
    raised-cosine roll-off to zero at `stop`, and its phase differentiated over
    `FM_HOP_S` steps: an instantaneous frequency whose bandwidth is the lowpass,
    not a tracker window. None when the segment, or the part of it above the
    floor, is too short to hold a track, and when the passband is not
    `FM_MIN_FUNDAMENTAL_DB` over the floor within f0 of the fundamental: the
    phase is then the noise's, read as hundreds of cents.
    """
    edge = int(FM_EDGE_S * sr)
    hop = int(FM_HOP_S * sr)
    if len(seg) <= 2 * edge + hop * MOD_MIN_FRAMES:
        return None
    t = np.arange(len(seg)) / sr
    ramp = np.ones(len(seg))
    ramp[:edge] = ramp[-edge:][::-1] = np.hanning(2 * edge)[:edge]
    spectrum = np.fft.fft(seg * ramp * np.exp(-2j * np.pi * f0 * t))
    away = np.abs(np.fft.fftfreq(len(seg), 1.0 / sr))
    power = np.abs(spectrum) ** 2
    passband = away < cutoff
    # The median within f0 is the floor: only the fundamental and a sub-octave rank sit there.
    floor = float(np.median(power[away < f0])) * int(np.count_nonzero(passband))
    if not float(power[passband].sum()) > floor * 10.0 ** (FM_MIN_FUNDAMENTAL_DB / 10.0):
        return None
    gain = 0.5 * (1.0 + np.cos(np.pi * np.clip((away - cutoff) / (stop - cutoff), 0.0, 1.0)))
    spectrum *= gain
    base = np.fft.ifft(spectrum)[edge:-edge][::hop]
    level = np.abs(base)
    if float(level.max()) <= 0.0:
        return None
    below = np.nonzero(level < level.max() * 10.0 ** (-FM_FLOOR_DB / 20.0))[0]
    if below.size:
        base = base[: below[0]]
    if len(base) <= MOD_MIN_FRAMES:
        return None
    offset_hz = np.diff(np.unwrap(np.angle(base))) * sr / (2.0 * np.pi * hop)
    return 1200.0 * np.log2(np.maximum(f0 + offset_hz, 1e-6) / f0)


def _demodulated_cents(seg: np.ndarray, sr: int, f0: float) -> tuple[np.ndarray, float] | None:
    """`demodulated_pitch` under `FM_LOWPASS_F0_FRACTION` of f0, detrended, and its lowpass.

    The roll-off reaches zero at `FM_STOP_F0_FRACTION` of f0, before the nearest
    other component can sit.
    """
    cutoff = min(FM_FAST_BAND_HZ[1], FM_LOWPASS_F0_FRACTION * f0)
    cents = demodulated_pitch(seg, sr, f0, cutoff, FM_STOP_F0_FRACTION * f0)
    if cents is None:
        return None
    idx = np.arange(len(cents), dtype=np.float64)
    return cents - np.polyval(np.polyfit(idx, cents, 1), idx), cutoff


def pitch_modulation(seg: np.ndarray, sr: int, f0: float) -> dict[str, tuple[float, float] | None]:
    """Strongest pitch modulation in the vibrato band and in each band beside it.

    Read off `_demodulated_cents`, one track for all three bands. Returns, per
    band name (`vibrato`, `slow`, `fast`), (peak-to-peak cents, rate in Hz) as
    `_band_peak` does, or None where the band is above the lowpass or unresolved
    by the track's length.
    """
    out: dict[str, tuple[float, float] | None] = {"vibrato": None, "slow": None, "fast": None}
    demodulated = _demodulated_cents(seg, sr, f0)
    if demodulated is None:
        return out
    cents, cutoff = demodulated
    n = len(cents)
    fm = np.abs(np.fft.rfft(cents * np.hanning(n)))
    rate = 1.0 / FM_HOP_S
    bands = (("vibrato", VIBRATO_BAND_HZ), ("slow", FM_SLOW_BAND_HZ), ("fast", FM_FAST_BAND_HZ))
    for name, (lo, hi) in bands:
        if min(hi, cutoff) > lo:
            out[name] = _band_peak(fm, rate, (lo, min(hi, cutoff)), n)
    return out


#: Shallowest vibrato that `vibrato_extent` reports: under it the first sidebands
#: of a low fundamental sit below -40 dB.
VIBRATO_EXTENT_MIN_CENTS = 1.0
#: Least share of the pitch track's modulation energy the vibrato line must hold.
#: Measured: wind and string vibrato 0.8-0.99; piano unison beating and a wandering
#: bass fundamental 0.0-0.2, though they read 1-100 cents.
VIBRATO_LINE_SHARE = 0.5
#: Fewest vibrato cycles the track must hold. The Hann main lobe spans two bins, so
#: below three cycles the line's lower neighbour carries the detrended drift at DC.
VIBRATO_MIN_CYCLES = 3
#: Top of the track `VIBRATO_BAND_SHARE` is taken over: the band's second harmonic,
#: where a beat's periodic distortion lands. Above it is fast FM from another
#: component in the passband, which cannot pose as a vibrato-band line.
VIBRATO_SHARE_TOP_HZ = 2.0 * VIBRATO_BAND_HZ[1]
#: Least share of the track under `VIBRATO_SHARE_TOP_HZ` a wandering vibrato's band
#: must hold. Measured over 2800 reference rows: 0 struck or plucked rows of 1628 newly
#: pass at 0.8 and 2 at 0.7, which admits 40 bowed, sung and blown ones.
VIBRATO_BAND_SHARE = 0.7
#: Shallowest band-shape vibrato, peak-to-peak. Measured on the line test: 32 of the
#: 1628 struck or plucked rows pass it at 1 cent and 4 at 3.
VIBRATO_BAND_MIN_CENTS = 3.0
#: Band bins within this of the band's peak set a wandering vibrato's rate span.
VIBRATO_SPAN_DB = 10.0


class Vibrato(NamedTuple):
    """A pitch modulation held over a segment, as `vibrato_extent` reads it.

    `cents` is peak-to-peak; the rate runs `rate_hz`..`rate_hi_hz`.
    """

    cents: float
    rate_hz: float
    rate_hi_hz: float


def vibrato_extent(seg: np.ndarray, sr: int, f0: float) -> Vibrato | None:
    """The vibrato held in `seg`, or None.

    Read over the caller's own segment, so it describes the spectrum taken from
    it, off the same `_demodulated_cents` track as `pitch_modulation`'s vibrato
    band, and stricter than that band's plain peak. The band's peak must stand
    above both neighbours and lie at least `VIBRATO_MIN_CYCLES` bins up: a peak
    on the band's lower edge with the spectrum still rising below it is slow
    drift or beating, and a track a decay or a bow stroke cut to a quarter
    second holds one cycle, whose line is the drift's leakage.

    Two shapes pass. A periodic vibrato's line (its bin and both neighbours)
    carries `VIBRATO_LINE_SHARE` of the track's energy above DC and is at least
    `VIBRATO_EXTENT_MIN_CENTS` deep. A player's vibrato whose rate wanders, or
    whose line shares the track with fast FM from elsewhere, carries
    `VIBRATO_BAND_SHARE` of the track under `VIBRATO_SHARE_TOP_HZ` across the
    band and is at least `VIBRATO_BAND_MIN_CENTS` deep. Either is read as the
    band's RMS depth, as a sinusoid's peak-to-peak, over the rates within
    `VIBRATO_SPAN_DB` of its peak: the line alone reads a wandering rate shallow
    and narrow, and a mask built on it leaves the outer sidebands as noise.
    None when the segment is too short to track or neither shape holds.
    """
    if f0 <= 0.0 or f0 > sr / 2.0:
        return None
    demodulated = _demodulated_cents(np.asarray(seg, dtype=np.float64), sr, f0)
    if demodulated is None or demodulated[1] <= VIBRATO_BAND_HZ[1]:
        return None
    cents = demodulated[0]
    n = len(cents)
    rate = 1.0 / FM_HOP_S
    taper = np.hanning(n)
    fm = np.abs(np.fft.rfft(cents * taper))
    peak = _band_peak(fm, rate, VIBRATO_BAND_HZ, n)
    if peak is None:
        return None
    k = round(peak[1] * n / rate)
    if k < VIBRATO_MIN_CYCLES or fm[k] < max(fm[k - 1], fm[k + 1]):
        return None
    power = fm**2
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    band = (freqs >= VIBRATO_BAND_HZ[0]) & (freqs <= VIBRATO_BAND_HZ[1])
    in_band = float(np.sum(power[band]))
    # Parseval through the taper: band RMS in cents, as a sinusoid's peak-to-peak.
    depth = 4.0 * float(np.sqrt(in_band / (n * np.sum(taper**2))))
    line = float(np.sum(power[k - 1 : k + 2]))
    periodic = peak[0] >= VIBRATO_EXTENT_MIN_CENTS and line >= VIBRATO_LINE_SHARE * float(
        np.sum(power[1:])
    )
    near = float(np.sum(power[1:][freqs[1:] <= VIBRATO_SHARE_TOP_HZ]))
    wandering = depth >= VIBRATO_BAND_MIN_CENTS and in_band >= VIBRATO_BAND_SHARE * near
    if not (periodic or wandering):
        return None
    span = freqs[band][power[band] >= power[k] * 10.0 ** (-VIBRATO_SPAN_DB / 10.0)]
    return Vibrato(depth, float(span.min()), float(span.max()))


def modulation_note(
    mono: np.ndarray, sr: int, note: Note, f0: float, onset: float | None = None
) -> dict:
    """Vibrato, tremolo and beat of one note's fundamental.

    Pitch movement in every band is read off `pitch_modulation`'s demodulated
    fundamental. The level is tracked with a zoomed DFT over a bank of
    candidate frequencies per 50 ms frame, so it follows the fundamental through
    a vibrato. It is not trusted with the pitch: below about 110 Hz its frames
    hold too few cycles, and the leakage of a neighbouring partial or of the
    fundamental's negative-frequency image beats at a multiple of f0 that the
    100 Hz frame rate aliases into the vibrato band — up to 14 cents read on a
    still tone, while its level reading stays under 0.03 dB. Both tracks are
    linearly detrended before the modulation spectrum is taken, so a note that
    is decaying (every level track) or drifting (a wind instrument warming) does
    not report its trend as a very slow modulation.

    Every field is None when the note is too short, too quiet or has no
    fundamental to track. A zero would be a claim that the note is dead still,
    which is a different finding from not having looked.
    """
    empty = {
        "vib_cents": None,
        "vib_rate_hz": None,
        "trem_db": None,
        "trem_rate_hz": None,
        "beat_db": None,
        "beat_rate_hz": None,
        "fm_slow_cents": None,
        "fm_slow_rate_hz": None,
        "fm_fast_cents": None,
        "fm_fast_rate_hz": None,
    }
    if f0 <= 0.0 or f0 > sr / 2.0:
        return empty
    start = (note.start if onset is None else onset) + MOD_WINDOW_S[0]
    end = min((note.start if onset is None else onset) + MOD_WINDOW_S[1], note.start + note.dur)
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
    basis = np.exp(-2j * np.pi * np.outer(cand, t))  # (points, win_n)
    amps = np.abs(frames @ basis.T)  # (frames, points)
    k = np.argmax(amps, axis=1)
    level = amps[np.arange(n_frames), k]
    if float(level.max()) <= 0.0:
        return empty

    level_db = 20.0 * np.log10(np.maximum(level, 1e-12))
    idx = np.arange(n_frames, dtype=np.float64)
    level_db = level_db - np.polyval(np.polyfit(idx, level_db, 1), idx)
    am = np.abs(np.fft.rfft(level_db * np.hanning(n_frames)))
    rate = 1.0 / MOD_FRAME_HOP_S

    # Per band, because they are not resolved together: the same window that
    # carries fourteen bins of vibrato carries one of beat.
    def _pair(peak: tuple[float, float] | None) -> tuple[float | None, float | None]:
        return (None, None) if peak is None else (round(peak[0], 2), round(peak[1], 2))

    trem_db, trem_rate = _pair(_band_peak(am, rate, VIBRATO_BAND_HZ, n_frames))
    beat_db, beat_rate = _pair(_band_peak(am, rate, BEAT_BAND_HZ, n_frames))
    pitch = pitch_modulation(seg, sr, f0)
    vib_cents, vib_rate = _pair(pitch["vibrato"])
    slow_cents, slow_rate = _pair(pitch["slow"])
    fast_cents, fast_rate = _pair(pitch["fast"])
    return {
        "vib_cents": vib_cents,
        "vib_rate_hz": vib_rate,
        "trem_db": trem_db,
        "trem_rate_hz": trem_rate,
        "beat_db": beat_db,
        "beat_rate_hz": beat_rate,
        "fm_slow_cents": slow_cents,
        "fm_slow_rate_hz": slow_rate,
        "fm_fast_cents": fast_cents,
        "fm_fast_rate_hz": fast_rate,
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
