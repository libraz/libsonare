"""A recording's own noise floor, the noise between a note's partials, and where partials clear it.

Every other floor in `profile_measure` is relative to the note's peak, which says
nothing about what the recording itself holds. These readings use `band_power_db`
throughout, so a floor band and `body_under_db` are directly comparable.
"""

from __future__ import annotations

import numpy as np
from metrics_note import BODY_MIN_F0_HZ, band_power_db
from shape.partials import BED_CLEAR_DB

#: Margin kept between the preroll window's end and the note-on, for the plugin's first buffer.
FLOOR_GUARD_S = 0.01
#: The shortest preroll window a floor is read from.
MIN_FLOOR_S = 0.05
#: Level (dBFS RMS) at or under which a window is digital silence rather than a floor.
SILENT_DBFS = -200.0
#: Length of the file's end read as a floor when there is no preroll to read.
TAIL_FLOOR_S = 0.1
#: How far under the held note's peak the file's end must sit to be a floor and not a release.
TAIL_FLOOR_UNDER_PEAK_DB = 60.0
#: Octave band centres of `floor_octaves_db`; edges are c/sqrt(2) and c*sqrt(2).
FLOOR_OCTAVE_CENTRES_HZ = (63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)
#: Upper edge of the broadband floor reading, capped at Nyquist.
FLOOR_TOP_HZ = 16000.0
#: Valleys needed before a slope or a coupling means anything.
MIN_VALLEYS = 4
#: Half-width of a valley band, as a fraction of the partial spacing.
VALLEY_HALF_WIDTH = 1.0 / 8.0
#: Partials whose clearance is read, as `partial_decay` reads them.
CLEAR_PARTIALS = 8

#: A body band within this many dB of the recorded floor is read as the floor.
BODY_FLOOR_MARGIN_DB = 6.0


def _rms_dbfs(seg: np.ndarray) -> float:
    rms = float(np.sqrt(np.mean(np.asarray(seg, dtype=np.float64) ** 2)))
    return float(20.0 * np.log10(max(rms, 1e-30)))


def recording_floor(
    mono: np.ndarray, sr: int, preroll_s: float, held_peak_db: float, f0: float = 0.0
) -> dict:
    """The recording's own floor: from the preroll, else from a file end far under the note.

    `floor_source` is None, with nothing else, where neither is a floor: digital
    silence, or a tail within `TAIL_FLOOR_UNDER_PEAK_DB` of the peak (a release).
    `f0` adds `floor_body_db` over the same band `body_under_db` reads.
    """
    n_pre = int((preroll_s - FLOOR_GUARD_S) * sr)
    tail_n = int(TAIL_FLOOR_S * sr)
    seg, source = None, None
    if n_pre >= int(MIN_FLOOR_S * sr) and _rms_dbfs(mono[:n_pre]) > SILENT_DBFS:
        seg, source = mono[:n_pre], "preroll"
    elif tail_n > 0 and len(mono) >= tail_n:
        tail = mono[-tail_n:]
        level = _rms_dbfs(tail)
        if level > SILENT_DBFS and level <= held_peak_db - TAIL_FLOOR_UNDER_PEAK_DB:
            seg, source = tail, "tail"
    if seg is None:
        return {"floor_source": None}
    top = min(FLOOR_TOP_HZ, sr / 2.0)
    out: dict = {"floor_source": source}

    def rnd(v: float | None) -> float | None:
        return None if v is None else round(v, 2)

    out["floor_db"] = rnd(band_power_db(seg, sr, 40.0, top))
    root2 = float(np.sqrt(2.0))
    out["floor_octaves_db"] = [
        rnd(band_power_db(seg, sr, c / root2, c * root2)) if c / root2 < sr / 2.0 else None
        for c in FLOOR_OCTAVE_CENTRES_HZ
    ]
    if f0 >= BODY_MIN_F0_HZ:
        body = rnd(band_power_db(seg, sr, 40.0, f0 * 0.7))
        if body is not None:
            out["floor_body_db"] = body
    return out


def _ols_slope(x: np.ndarray, y: np.ndarray) -> float | None:
    if x.size < 2 or float(np.ptp(x)) <= 0.0:
        return None
    return float(np.polyfit(x, y, 1)[0])


def valley_profile(
    seg: np.ndarray, sr: int, partials_hz: list[float], partials_db: list[float]
) -> dict:
    """The noise density midway between neighbouring partials, and how it follows the ladder.

    `valley_db` is dB/Hz at each midpoint; `valley_slope_db_oct` its slope per
    octave of frequency; `valley_ladder_coupling` its slope against the mean level
    of the two partials either side (1 = floor rises dB for dB with the ladder).
    Absent below `MIN_VALLEYS` valleys.
    """
    spacings = np.diff(np.asarray(partials_hz, dtype=np.float64))
    if spacings.size == 0:
        return {}
    # A pair spanning an unfound harmonic would put its midpoint on a partial.
    limit = 1.5 * float(spacings[spacings > 0].min()) if (spacings > 0).any() else 0.0
    freqs, levels, valley = [], [], []
    for i, sp in enumerate(spacings):
        if sp <= 0.0 or sp > limit or i + 1 >= len(partials_db):
            continue
        mid = partials_hz[i] + 0.5 * sp
        half = VALLEY_HALF_WIDTH * sp
        p = band_power_db(seg, sr, mid - half, mid + half)
        if p is None:
            continue
        freqs.append(mid)
        levels.append(0.5 * (partials_db[i] + partials_db[i + 1]))
        valley.append(p - 10.0 * float(np.log10(2.0 * half)))
    if len(valley) < MIN_VALLEYS:
        return {}
    v = np.asarray(valley)
    out: dict = {"valley_db": [round(float(x), 2) for x in v]}
    slope = _ols_slope(np.log2(np.asarray(freqs)), v)
    if slope is not None:
        out["valley_slope_db_oct"] = round(slope, 3)
    coupling = _ols_slope(np.asarray(levels), v)
    if coupling is not None:
        out["valley_ladder_coupling"] = round(coupling, 3)
    return out


def _partial_bands(seg: np.ndarray, sr: int, freqs_hz: list[float]):
    """Per-frame band magnitude at each frequency, in `partial_decay`'s framing.

    Returns (frame times, [magnitude per frequency]) or None under six frames.
    """
    win = max(1024, int(0.093 * sr))
    hop = win // 2
    n_frames = max(1, (len(seg) - win) // hop + 1)
    if n_frames < 6:
        return None
    frames = np.lib.stride_tricks.sliding_window_view(seg, win)[::hop][:n_frames]
    spec = np.abs(np.fft.rfft(frames * np.hanning(win), axis=1))
    fr = np.fft.rfftfreq(win, 1.0 / sr)
    t = np.arange(n_frames) * hop / sr
    bands = []
    for f in freqs_hz:
        k = int(np.argmin(np.abs(fr - f)))
        bands.append(spec[:, max(0, k - 1) : k + 2].max(axis=1))
    return t, bands


def partial_clearance(
    seg: np.ndarray, sr: int, partials_hz: list[float], n_partials: int = CLEAR_PARTIALS
) -> dict:
    """`partial_clear_s` and `partial_decay_clear_db_s`, over the segment `partial_decay` reads.

    A partial is clear at a frame when it stands `BED_CLEAR_DB` over the same
    band measured in the valley midway to the next partial (a virtual one at the
    previous spacing for the top). The decay is `partial_decay`'s fit over the
    frames up to the last clear one; None where never clear or under five frames.
    """
    hz = list(partials_hz[:n_partials])
    if not hz:
        return {}
    valleys = []
    for i, f in enumerate(hz):
        nxt = partials_hz[i + 1] if i + 1 < len(partials_hz) else None
        if nxt is None:
            nxt = f + (f - partials_hz[i - 1] if i > 0 else f)
        valleys.append(0.5 * (f + nxt))
    got = _partial_bands(seg, sr, hz + valleys)
    if got is None:
        return {"partial_clear_s": [], "partial_decay_clear_db_s": []}
    t, bands = got
    clear_s: list[float | None] = []
    decay: list[float | None] = []
    for i in range(len(hz)):
        peak, bed = bands[i], bands[len(hz) + i]
        db = 20.0 * np.log10(np.maximum(peak / max(float(peak.max()), 1e-12), 1e-5))
        clear = 20.0 * np.log10(np.maximum(peak, 1e-12)) >= (
            20.0 * np.log10(np.maximum(bed, 1e-12)) + BED_CLEAR_DB
        )
        if not clear.any():
            clear_s.append(None)
            decay.append(None)
            continue
        last = int(np.nonzero(clear)[0][-1])
        clear_s.append(round(float(t[last]), 3))
        keep = (db > -55.0) & (np.arange(t.size) <= last)
        if keep.sum() < 5:
            decay.append(None)
            continue
        slope = float(np.polyfit(t[keep], db[keep], 1)[0])
        decay.append(round(slope, 2) if np.isfinite(slope) else None)
    return {"partial_clear_s": clear_s, "partial_decay_clear_db_s": decay}


def _floor_censor(ref_row: dict, dimension: str) -> str | None:
    """The reason `floor_state` censors `dimension` on this row, or None (rows measured before it: None)."""
    state = ref_row.get("floor_state")
    return None if not state else (state.get("censors") or {}).get(dimension)


#: A reference row whose body band, after its recurring source lines were masked, is the floor's.
BELOW_FLOOR_AFTER_LINES = "reference body below the recording's floor once recurring fixed-frequency lines in the source are masked"


def body_floor_state(ref_row: dict) -> str | None:
    """Why a reference row's body band is its recording's floor or hum, or None when it is not.

    Decided at measurement (`floor_declare`): the capture's floor is declared only
    where note-independent and stationary, and mains hum is counted into the band.
    The band is read with the row's recurring source lines masked (`body_lines`), so
    a floor censor on such a row names the masking.
    """
    from floor_declare import CENSOR_FLOOR  # floor_declare imports this module

    reason = _floor_censor(ref_row, "body")
    if reason == CENSOR_FLOOR and ref_row.get("body_lines"):
        return BELOW_FLOOR_AFTER_LINES
    return reason


def body_at_floor(ref_row: dict) -> bool:
    """Whether a reference row's body delta is censored (`body_floor_state` gives why)."""
    return body_floor_state(ref_row) is not None


def tnr_floor_state(ref_row: dict) -> str | None:
    """Why a reference row's tone-to-noise noise is its recording's floor or hum, or None."""
    return _floor_censor(ref_row, "tnr")


def tnr_at_floor(ref_row: dict) -> bool:
    """Whether a reference row's tone-to-noise delta is censored (`tnr_floor_state` gives why)."""
    return tnr_floor_state(ref_row) is not None
