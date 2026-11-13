"""Where one recording of a module one-shot capture ends, and what its end is.

A one-shot capture reads each recording to its own end instead of to one gate, so
the window end is a per-row quantity: the recorded length, or the sound's end plus
a margin where the sound reaches its floor inside the file. A row also says what
its end is: still sounding (a trim) or an editor fade, so a reader of its decay
does not take the end for a decay to silence.
"""

from __future__ import annotations

import numpy as np
from metrics_note import band_power_db

#: Kept past the last frame in which a partial still clears the bed. The partial readings run
#: on 93 ms frames hopped by half, so a margin under one frame leaves the clear-to-bed fall
#: unread; 100 ms is that frame rounded up to the 10 ms envelope grid.
WINDOW_MARGIN_MS = 100
#: Length of the file's end the floor and the partials' clearance are read from.
END_READ_S = 0.12
#: Partials over the floor by more than this are still sounding.
TRUNCATED_OVER_FLOOR_DB = 6.0
#: A floor is flat: the end level may differ from the stretch before it by no more than this.
FLOOR_FLAT_DB = 3.0
#: Partials whose clearance is read, as `metrics_floor.CLEAR_PARTIALS`.
CLEAR_PARTIALS = 8
#: Deficit of the last `FADE_S` against the trend line before it that makes an editor fade.
FADE_DROP_DB = 3.0
FADE_S = 0.03
#: The trend line is fitted over the 0.4 s ending 0.1 s before the file's end.
FADE_TREND_S = 0.4
FADE_TREND_GAP_S = 0.1
#: 16-bit quantisation noise sits near -98 dBFS; a fade is read only above this.
QUANTISATION_DBFS = -90.0

RULE_RECORDED = "recorded length"
RULE_SOUND_END = "sound end + margin"
MARK_TRUNCATED = "truncated"
MARK_FADE_END = "fade end"


def _rms_db(seg: np.ndarray) -> float:
    return float(
        20.0 * np.log10(max(float(np.sqrt(np.mean(np.asarray(seg, np.float64) ** 2))), 1e-30))
    )


def end_clearance_db(seg: np.ndarray, sr: int, partials_hz: list[float]) -> float | None:
    """Median over the first partials of how far each stands over the bed midway to the next.

    Read on the file's last `END_READ_S`, where `metrics_floor.partial_clearance`'s 93 ms
    frames leave too few frames to read. Band density per Hz on both sides, and the band
    never narrower than two bins. None where no partial has a neighbour to read.
    """
    out = []
    for i, f in enumerate(partials_hz[:CLEAR_PARTIALS]):
        nxt = (
            partials_hz[i + 1]
            if i + 1 < len(partials_hz)
            else f + (f - partials_hz[i - 1] if i > 0 else f)
        )
        spacing = nxt - f
        if spacing <= 0.0:
            continue
        half = max(spacing / 8.0, 2.0 * sr / len(seg))
        peak = band_power_db(seg, sr, f - half, f + half)
        bed = band_power_db(seg, sr, f + spacing / 2.0 - half, f + spacing / 2.0 + half)
        if peak is not None and bed is not None:
            out.append(peak - bed)
    return float(np.median(out)) if out else None


def fade_deficit_db(t: np.ndarray, env_db: np.ndarray) -> float | None:
    """How far the last `FADE_S` of the envelope sits under the line fitted before it.

    The line is fitted from the envelope's peak (or `FADE_TREND_S` back, whichever is
    later) to `FADE_TREND_GAP_S` before the end. None under six frames of trend.
    """
    end = float(t[-1])
    start = max(end - FADE_TREND_S, float(t[int(np.argmax(env_db))]))
    trend = (t >= start) & (t <= end - FADE_TREND_GAP_S)
    if int(trend.sum()) < 6:
        return None
    line = np.polyval(np.polyfit(t[trend], env_db[trend], 1), t)
    last = t >= end - FADE_S
    return float((line - env_db)[last].mean())


def row_window(
    held: np.ndarray,
    sr: int,
    recorded_ms: int,
    partials_hz: list[float],
    t: np.ndarray,
    env_db: np.ndarray,
    clear_end_s: float | None,
) -> dict:
    """The window end of one recording and what its file end is.

    @p held runs from the note's onset to the file's end; @p t and @p env_db are its
    10 ms envelope; @p clear_end_s is where the last frame in which any of the first
    partials still clears the bed ends (None if none ever did). The sound has ended inside
    the file when the last `END_READ_S` is flat and its partials stand no more than
    `TRUNCATED_OVER_FLOOR_DB` over the bed: the window then ends `WINDOW_MARGIN_MS` after
    that frame. Otherwise the sound is still going at the file end (`truncated`) and the
    window is the recorded length. Returns `end_ms`, `rule`, `recorded_ms`,
    `end_clearance_db`, `sound_end_ms` (when read) and `marks`, a mapping from mark to its
    reason.
    """
    n_end = int(END_READ_S * sr)
    marks: dict[str, str] = {}
    clearance = end_clearance_db(held[-n_end:], sr, partials_hz) if held.size >= n_end else None
    flat = True
    if held.size >= 2 * n_end:
        flat = abs(_rms_db(held[-2 * n_end : -n_end]) - _rms_db(held[-n_end:])) <= FLOOR_FLAT_DB
    sound_end_ms = None
    if clearance is not None and clearance > TRUNCATED_OVER_FLOOR_DB:
        marks[MARK_TRUNCATED] = (
            f"partials stand {clearance:.1f} dB over the bed in the last {END_READ_S * 1000:.0f} ms"
        )
    elif not flat:
        marks[MARK_TRUNCATED] = (
            f"level still moving by more than {FLOOR_FLAT_DB:g} dB in the last "
            f"{END_READ_S * 1000:.0f} ms, so the end is not a floor"
        )
    elif clearance is not None and clear_end_s is not None:
        sound_end_ms = int(1000.0 * clear_end_s)
    end_ms, rule = recorded_ms, RULE_RECORDED
    if sound_end_ms is not None and sound_end_ms + WINDOW_MARGIN_MS < recorded_ms:
        end_ms, rule = sound_end_ms + WINDOW_MARGIN_MS, RULE_SOUND_END
    deficit = fade_deficit_db(t, env_db)
    if (
        deficit is not None
        and deficit > FADE_DROP_DB
        and float(env_db[t >= t[-1] - FADE_S][0]) > QUANTISATION_DBFS
    ):
        marks[MARK_FADE_END] = (
            f"the last {FADE_S * 1000:.0f} ms fall {deficit:.1f} dB under the trend"
        )
    out: dict = {"end_ms": end_ms, "rule": rule, "recorded_ms": recorded_ms}
    if clearance is not None:
        out["end_clearance_db"] = round(clearance, 2)
    if sound_end_ms is not None:
        out["sound_end_ms"] = sound_end_ms
    if marks:
        out["marks"] = marks
    return out
