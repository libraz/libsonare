"""Whether a captured note is a sample looped bit-exactly, and what that makes unreadable.

A sampler holds a sustained note by repeating one cycle. The repetition is sample-exact,
so anything read across it measures the loop and not the instrument. The test is the
envelope-normalised correlation of the held note with itself at a lag of at least 100 ms
(several periods of any pitched note, so natural jitter has accumulated); a loop leaves
a residual 40 to 70 dB under the signal, a stable natural source does not.

Contract: pure functions over a mono segment, no I/O, deterministic.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

# Best-frame residual at lag >= 100 ms. Rows known not to loop reach -33.6 dB at closest
# (bass_fingered; piano -29.7, electric_guitar_di -31.5, jittered synthetic -29); loops: -36.
# -36 sits in that 2.4 dB gap; rows in -36..-40 count as looped, -30..-36 are `borderline`.
LOOPED_BEST_DB = -36.0
# Median over the frames whose lag range reaches the best period; a loop only some show is not one.
LOOPED_MEDIAN_DB = -30.0
# With fewer such frames a failing median may be the pre-join lead-in outvoting the loop, so it
# cannot call the row "not". A passing one stands: clarinet n074/n082 are looped at -95..-107 dB
# on a single reaching frame, and accordion n053 (463 ms in 3 s) has exactly three.
MIN_REACHABLE_FRAMES = 3
BORDERLINE_DB = -30.0
# Between the cut and this residual a row must also repeat at one period: a stable natural
# source can reach -36..-40 dB at one frame, a loop does at its own period in every frame.
STRONG_LOOP_DB = -40.0
PERIOD_AGREEMENT = 0.01
MIN_AGREEING_FRAMES = 2

TEMPLATE_S = 0.25
LAG_MIN_S = 0.010
LAG_MAX_S = 2.0
LAG_LONG_S = 0.1
TEMPLATE_STARTS_S = (0.4, 0.8, 1.2, 1.6, 2.0, 2.6)
PAD = 64
# The loop period is the smallest lag within 6 dB of the best long lag's residual; candidates
# are the first 12 peaks within 15 dB of it at integer lag (a fractional lag recovers up to 40 dB).
PERIOD_TOLERANCE_DB = 6.0
PERIOD_CANDIDATE_DB = 15.0
PERIOD_CANDIDATES = 12
ENVELOPE_FLOOR = 1e-6
SIGNAL_FLOOR = 1e-5
LADDER_WINDOW_S = (0.7, 1.9)
# Join: the 50 ms sliding residual at the period holds this share of the best residual in dB,
# 95 % of 200 ms; half keeps a -90 dB loop apart from its natural lead-in and a -40 dB one at -20.
JOIN_DEPTH = 0.5
JOIN_RUN_S = 0.2
JOIN_WIN_S = 0.05

VERDICT_LOOPED = "looped"
VERDICT_NOT = "not"
VERDICT_NOT_TESTABLE = "not_testable"
#: A reference row measured before the loop was recorded, read by a model note.
VERDICT_UNRECORDED = "unrecorded"

#: Captures whose join is late enough (0.32..0.42 s) to read the spectral dimensions up to it.
READ_TO_JOIN_CAPTURES = ("cello", "accordion", "contrabass", "electric_grand_module")
#: A row reads to its join only when the join leaves a spectrum window this long after 0.12 s.
READ_TO_JOIN_MIN_S = 0.3

#: Where each loop-affected dimension is read, seconds from the onset (profile_measure's windows).
DIMENSION_WINDOW_S = {
    "stretch": (0.12, 1.62),
    "tnr": (0.12, 1.62),
    "balance": (0.12, 1.62),
    "centroid_pct": (0.12, 1.62),
    "register": (0.12, 1.62),
    "body": (0.2, 1.2),
    "decay": (0.1, 3.0),
    "aftersound": (0.3, 3.0),
    "doubling": (0.1, 3.0),
    "partial_tilt": (0.12, 3.0),
    "band_decay": (0.1, 3.0),
    "ring": (0.1, 3.0),
    "onset_settle": (0.2, 1.2),
    "damper": (3.0, 3.5),
    "crest": (0.2, 1.2),
}
#: Read from the note's own onset to its envelope peak (`attack_ms`): the window is per row.
ONSET_DIMENSIONS = ("attack", "arrival")
#: Dimensions that read the loop itself: the exclusion is the verdict, whatever the window.
LOOP_READ_DIMENSIONS = ("tnr", "stretch")
#: Dimensions that follow the note over time; past the join they follow the loop's envelope.
TIME_VARYING_DIMENSIONS = (
    "decay",
    "aftersound",
    "doubling",
    "partial_tilt",
    "band_decay",
    "ring",
    "onset_settle",
    "damper",
)
#: Kept on a looped capture, as a reading of the one cycle the sampler repeats.
SNAPSHOT_DIMENSIONS = ("balance", "centroid_pct", "register", "crest")
SNAPSHOT_LABEL = "snapshot of one frozen cycle"

#: Measured on unlooped notes (piano, oboe, alto_sax; 7 rows, seam-matched sample-exact loops):
#: periodising a 100-300 ms stretch moves tnr by -11 to -14 dB and body by +2 to +7 dB (medians),
#: whatever the line spacing (3 to 10 Hz alike), so long loops do not read as their note.
PERIODISED_NOTE = (
    "periodising an unlooped note sample-exactly moved tnr -11..-14 dB "
    "and body +2..+7 dB at 100-300 ms; open: a seam statistic did not separate loop seams "
    "from arbitrary boundaries (naive loops 2.5-69, unlooped notes 1.3-6.5), so no row is "
    "granted a snapshot"
)
#: `body` reads 40 Hz to 0.7 f0. One loop line in it is enough: past an exact repeat the sub-f0
#: power is the cycle-to-cycle difference frozen into the loop, not radiated content.
BODY_BAND_BOTTOM_HZ = 40.0
BODY_BAND_TOP = 0.7
BODY_MIN_LINES = 1

# Harmonic ladder of the cycle: power within +-0.35 f0 of each of the first 20 harmonics, re h1.
LADDER_HARMONICS = 20
LADDER_BAND = 0.35
LADDER_FLOOR_DB = -80.0
# Notes whose ladders sit this far apart (median over shared velocities, mean |dB| over the
# harmonics both carry) share a stored waveform. Clarinet pairs read 0.9-3.8 inside a group and
# 11.0-20.8 across; oboe 0.4-1.0 and 8.1-9.2. 6.0 sits in the empty gap of both.
GROUP_SAME_DB = 6.0
# A note is uncertain when any other note sits between these distances: too far to be sure it
# shares the stored waveform, too near to be sure it does not (the cut is inside the band).
GROUP_UNCERTAIN_DB = (4.0, 8.0)
MIN_LADDER_HARMONICS = 4


def _slow_envelope_normalised(x: np.ndarray, sr: int, win_s: float = 0.25):
    n = int(win_s * sr) | 1
    w = np.hanning(n + 2)[1:-1]
    w /= w.sum()
    pad = np.zeros(n // 2)
    e2 = np.convolve(np.concatenate([pad, x * x, pad]), w, "valid")[: len(x)]
    m = np.convolve(np.concatenate([pad, np.ones(len(x)), pad]), w, "valid")[: len(x)]
    e = np.sqrt(e2 / np.maximum(m, 1e-9))
    return x / np.maximum(e, ENVELOPE_FLOOR), e


def _fractional_segment(x: np.ndarray, start: float, n: int) -> np.ndarray | None:
    """`x[start : start + n]` for a fractional start, by FFT advance."""
    i0 = int(np.floor(start))
    fr = start - i0
    a, b = i0 - PAD, i0 + n + PAD
    if a < 0 or b > len(x):
        return None
    seg = x[a:b]
    if fr == 0.0:
        return seg[PAD : PAD + n]
    k = np.fft.rfftfreq(len(seg))
    y = np.fft.irfft(np.fft.rfft(seg) * np.exp(2j * np.pi * k * fr), len(seg))
    return y[PAD : PAD + n]


def _ncorr(a: np.ndarray, b: np.ndarray) -> float:
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 0 else 0.0


def _r_at(x: np.ndarray, t: int, length: int, lag: float) -> float | None:
    if t + lag + length + PAD > len(x):
        return None
    s2 = _fractional_segment(x, t + lag, length)
    return None if s2 is None else _ncorr(x[t : t + length], s2)


def _refine(x, t, length, l0, span=0.5, step=0.0625):
    best = (-2.0, l0)
    for d in np.arange(-span, span + 1e-9, step):
        r = _r_at(x, t, length, l0 + d)
        if r is not None and r > best[0]:
            best = (r, l0 + d)
    r, lag = best
    rm, rp = _r_at(x, t, length, lag - step), _r_at(x, t, length, lag + step)
    if rm is not None and rp is not None:
        den = rm - 2 * r + rp
        if den < 0:
            lag = lag + 0.5 * step * (rm - rp) / den
            r2 = _r_at(x, t, length, lag)
            if r2 is not None and r2 > r:
                r = r2
    return r, lag


def _lag_curve(x, t, length, lmin, lmax):
    """Normalised correlation of the template at `t` against every integer lag in [lmin, lmax]."""
    tm = x[t : t + length]
    nt = np.sqrt((tm * tm).sum())
    reg = x[t + lmin : t + lmax + length]
    n = 1 << int(np.ceil(np.log2(len(reg) + length)))
    c = np.fft.irfft(np.fft.rfft(reg, n) * np.conj(np.fft.rfft(tm, n)), n)[: lmax - lmin + 1]
    cs = np.cumsum(np.concatenate([[0.0], reg * reg]))
    en = np.sqrt(np.maximum(cs[length : length + len(c)] - cs[: len(c)], 0))
    return c / (nt * en + 1e-30)


def _frame(xn: np.ndarray, sr: int, t: int, end: int) -> dict | None:
    length = int(TEMPLATE_S * sr)
    lmin, llong = int(LAG_MIN_S * sr), int(LAG_LONG_S * sr)
    lmax3 = min(int(LAG_MAX_S * sr), (end - t - length - PAD) // 3)
    lmax1 = min(int(LAG_MAX_S * sr), end - t - length - PAD)
    three = lmax3 > llong + 8
    lmax = lmax3 if three else lmax1
    if lmax <= llong + 8:
        return None
    r = _lag_curve(xn, t, length, lmin, lmax)
    ll = int(r[llong - lmin :].argmax()) + llong
    r_long, l_long = _refine(xn, t, length, float(ll))
    # The period is the smallest lag that repeats about as exactly as the best long lag. A
    # pitched tone also repeats at its own period to -30..-50 dB, which is not the loop.
    best_db = _db_residual(r_long)
    peaks = (r[1:-1] >= r[:-2]) & (r[1:-1] >= r[2:])
    near = _db_residual_array(r[1:-1]) <= _db_residual(r[ll - lmin]) + PERIOD_CANDIDATE_DB
    lag, r_short = l_long, r_long
    for i in np.where(peaks & near)[0][:PERIOD_CANDIDATES] + 1:
        cand_r, cand_l = _refine(xn, t, length, float(i + lmin))
        if _db_residual(cand_r) <= best_db + PERIOD_TOLERANCE_DB:
            lag, r_short = cand_l, cand_r
            break
    return {"L": lag, "r": r_short, "r_long": r_long, "l_long": l_long, "lmax": lmax}


def _join_s(xn: np.ndarray, sr: int, lag: float, end: int, best_db: float) -> float | None:
    """First time the signal repeats at `lag` within half the best residual (dB) for 200 ms."""
    n = 1 << int(np.ceil(np.log2(len(xn) + 4096)))
    k = np.fft.rfftfreq(n)
    xs = np.fft.irfft(np.fft.rfft(xn, n) * np.exp(2j * np.pi * k * lag), n)[: len(xn)]
    m = end - int(lag) - 8
    x, y = xn[:m], xs[:m]
    w = int(JOIN_WIN_S * sr)

    def cs(v):
        return np.cumsum(np.concatenate([[0.0], v]))

    sxy, sxx, syy = cs(x * y), cs(x * x), cs(y * y)
    cnt = len(x) - w + 1
    if cnt <= 0:
        return None
    num = sxy[w : w + cnt] - sxy[:cnt]
    a = sxx[w : w + cnt] - sxx[:cnt]
    b = syy[w : w + cnt] - syy[:cnt]
    hit = _db_residual_array(num / (np.sqrt(a * b) + 1e-30)) <= best_db * JOIN_DEPTH
    run = int(JOIN_RUN_S * sr)
    if not hit.any() or len(hit) <= run:
        return None
    c = np.cumsum(np.concatenate([[0], hit.astype(int)]))
    ok = (c[run:] - c[:-run]) >= int(0.95 * run)
    return float(np.argmax(ok) / sr) if ok.any() else None


def _db_residual(r: float) -> float:
    return float(10 * np.log10(max(1 - r * r, 1e-12)))


def _db_residual_array(r: np.ndarray) -> np.ndarray:
    return 10 * np.log10(np.maximum(1 - r * r, 1e-12))


def held_ladder(held: np.ndarray, sr: int, f0_hz: float) -> list[float | None] | None:
    """The harmonic ladder of a held note, from 0.7 to 1.9 s after its onset; None if too short.

    The same window and bands for a looped and an unlooped note: a loop's lines carry the
    ladder of the stored cycle, so a note and its transposition read alike either way.
    """
    seg = np.asarray(held[int(LADDER_WINDOW_S[0] * sr) : int(LADDER_WINDOW_S[1] * sr)], float)
    if seg.size < int(0.5 * sr) or f0_hz <= 0.0:
        return None
    power = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    return ladder_db(np.fft.rfftfreq(len(seg), 1.0 / sr), power, f0_hz, sr)


def ladder_db(freqs: np.ndarray, power: np.ndarray, f0_hz: float, sr: int) -> list[float | None]:
    """Power around each of the first harmonics of `f0_hz`, dB re the first; None past Nyquist."""
    out: list[float | None] = []
    for h in range(1, LADDER_HARMONICS + 1):
        lo, hi = (h - LADDER_BAND) * f0_hz, (h + LADDER_BAND) * f0_hz
        p = float(power[(freqs >= lo) & (freqs < hi)].sum()) if hi <= 0.45 * sr else 0.0
        out.append(10.0 * np.log10(p) if p > 0.0 else None)
    if out[0] is None:
        return [None] * LADDER_HARMONICS
    return [None if v is None else round(v - out[0], 1) for v in out]


def ladder_distance(a: list, b: list) -> float | None:
    """Mean |dB| difference over the harmonics above the first that both ladders carry."""
    diffs = [
        abs(x - y)
        for x, y in zip(a[1:], b[1:], strict=True)
        if x is not None and y is not None and max(x, y) >= LADDER_FLOOR_DB
    ]
    return float(np.mean(diffs)) if len(diffs) >= MIN_LADDER_HARMONICS else None


def detect_loop(held: np.ndarray, sr: int) -> dict:
    """The loop record of one held note (from its onset): verdict, period, residual, join.

    `not_testable` where the note is too short to hold a template and one 100 ms lag.
    The residual is the best frame's, in dB of the envelope-normalised signal.
    """
    xn, env = _slow_envelope_normalised(np.asarray(held, dtype=np.float64), sr)
    ok = np.where(env >= SIGNAL_FLOOR)[0]
    if len(ok) == 0:
        return {"verdict": VERDICT_NOT_TESTABLE, "reason": "silent"}
    end = int(ok[-1]) + 1
    xn = xn[:end]
    frames = []
    for ts in TEMPLATE_STARTS_S:
        t = int(ts * sr)
        if t + int((TEMPLATE_S + 3 * LAG_MIN_S) * sr) + PAD > end:
            continue
        f = _frame(xn, sr, t, end)
        if f:
            f["t"] = t
            frames.append(f)
    if not frames:
        return {
            "verdict": VERDICT_NOT_TESTABLE,
            "reason": "shorter than one template and a 100 ms lag",
        }
    best = max(frames, key=lambda f: f["r_long"])
    best_db = _db_residual(best["r_long"])
    # A frame sees the loop when its lag range holds a multiple of the period of 100 ms or more.
    need = np.ceil(LAG_LONG_S * sr / best["L"]) * best["L"]
    reaching = [f for f in frames if f["lmax"] >= need]
    median_db = _db_residual(float(np.median([f["r_long"] for f in (reaching or frames)])))
    looped = best_db <= LOOPED_BEST_DB and median_db <= LOOPED_MEDIAN_DB
    if best_db <= LOOPED_BEST_DB and not looped and len(reaching) < MIN_REACHABLE_FRAMES:
        return {
            "verdict": VERDICT_NOT_TESTABLE,
            "reason": f"cannot test: {len(reaching)} frame(s) reach the loop lag of "
            f"{1000.0 * need / sr:.0f} ms and their median fails; {MIN_REACHABLE_FRAMES} "
            "are needed to call the row not looped",
            "residual_db": round(best_db, 1),
        }
    agreeing = sum(abs(f["L"] - best["L"]) <= PERIOD_AGREEMENT * best["L"] for f in reaching)
    unconfirmed = looped and best_db > STRONG_LOOP_DB and agreeing < MIN_AGREEING_FRAMES
    looped = looped and not unconfirmed
    rec: dict = {
        "verdict": VERDICT_LOOPED if looped else VERDICT_NOT,
        "residual_db": round(best_db, 1),
        "median_db": round(median_db, 1),
        "frames": len(frames),
        "reaching_frames": len(reaching),
        "reach_ms": round(1000.0 * max(f["lmax"] for f in frames) / sr, 1),
    }
    if unconfirmed:
        rec["period_agreeing_frames"] = agreeing
    if not looped:
        rec["borderline"] = best_db <= BORDERLINE_DB
        return rec
    lag = best["L"]
    join = _join_s(xn, sr, lag, end, best_db)
    rec["period_ms"] = round(1000.0 * lag / sr, 3)
    rec["join_s"] = None if join is None else round(join, 3)
    return rec


def body_loop_lines(f0_hz: float, period_ms: float) -> dict | None:
    """How many of the loop's k/period lines fall in the band `body` reads, below the fundamental.

    An exact repeat holds energy only on those lines, so a loop of several cycles of the
    note puts all of its sub-f0 content on them. None where `body` is not read for this f0.
    """
    from metrics_note import BODY_MIN_F0_HZ

    if f0_hz < BODY_MIN_F0_HZ:
        return None
    lines = int(np.floor(BODY_BAND_TOP * f0_hz * period_ms / 1000.0)) - int(
        np.ceil(BODY_BAND_BOTTOM_HZ * period_ms / 1000.0)
    )
    lines += 1
    return {"lines_below_f0": max(lines, 0), "on_lines": lines >= BODY_MIN_LINES}


def read_to_join_s(rec: dict, capture_id: str | None) -> float | None:
    """The time the spectral reads stop at, or None where the note is read whole."""
    if capture_id not in READ_TO_JOIN_CAPTURES or rec.get("verdict") != VERDICT_LOOPED:
        return None
    join = rec.get("join_s")
    return join if join is not None and join >= READ_TO_JOIN_MIN_S else None


def exclusions(
    rec: dict,
    *,
    body_on_lines: bool = False,
    attack_ms: float | None = None,
    body_floor: str | None = None,
) -> tuple[dict[str, str], dict[str, dict]]:
    """(excluded, snapshot): per dimension, why a looped note cannot answer it, and what it answers.

    Past the join the sound is the sampler's repetition, so `tnr`, `stretch` and the
    time-varying dimensions read the loop. Where the note is read only up to its join
    (`read_to_join_s` in the record) the spectral dimensions are read there instead and
    the time-varying ones stay excluded. A dimension whose window ends before the join
    is not affected. `attack` and `arrival` end at the row's `attack_ms`; they are excluded
    when the join falls inside that window, the envelope ramp laid over a repeated cycle.
    """
    if rec.get("verdict") != VERDICT_LOOPED:
        return {}, {}
    join = rec.get("join_s")
    until = rec.get("read_to_join_s")
    pitch = rec.get("period_pitch_periods")
    period = f"loop period {rec['period_ms']} ms" + (
        f" = {pitch} pitch periods" if pitch is not None else ""
    )
    where = (
        f"{period}, exact repetition from {join} s, residual {rec['residual_db']} dB"
        if join is not None
        else f"{period}, residual {rec['residual_db']} dB, no join found"
    )
    excluded: dict[str, str] = {}
    snapshot: dict[str, dict] = {}
    for dim, (start, end) in DIMENSION_WINDOW_S.items():
        if until is not None and dim not in TIME_VARYING_DIMENSIONS:
            continue
        past = join is None or end > join
        if dim in SNAPSHOT_DIMENSIONS:
            if past:
                snapshot[dim] = {"label": SNAPSHOT_LABEL}
        elif dim == "body":
            # Exact from the window's start, so none of what it reads is the instrument's.
            if body_on_lines and join is not None and join <= start:
                lead = (
                    f"{body_floor} (body-band censor); its loop lines also recur at 1/period"
                    if body_floor
                    else "sub-f0 power is the loop's own lines at 1/period"
                )
                excluded[dim] = f"{lead} ({where}); {PERIODISED_NOTE}"
        elif past:
            kind = "reads the loop" if dim in LOOP_READ_DIMENSIONS else "time-varying past the join"
            note = f"; {PERIODISED_NOTE}" if dim in ("tnr", "body") else ""
            excluded[dim] = f"{kind} ({where}){note}"
    if attack_ms is not None and (join is None or join <= attack_ms / 1000.0):
        for dim in ONSET_DIMENSIONS:
            excluded[dim] = f"onset is the envelope over the loop ({where})"
    return excluded, snapshot


def loop_summary(rows: list[dict]) -> dict:
    """The capture-level loop statement of a reference profile's rows.

    `status` is `looped` where 80 % or more of the tested rows loop, `not_looped` at 20 %
    or less, `mixed` between. Descriptive only: the objective reads each row's own record.
    """
    recs = [r["loop"] for r in rows if isinstance(r.get("loop"), dict)]
    tested = [r for r in recs if r.get("verdict") in (VERDICT_LOOPED, VERDICT_NOT)]
    looped = [r for r in tested if r["verdict"] == VERDICT_LOOPED]
    if not tested:
        return {"status": VERDICT_NOT_TESTABLE, "rows": len(recs), "tested": 0}
    share = len(looped) / len(tested)
    status = "looped" if share >= 0.8 else "not_looped" if share <= 0.2 else "mixed"
    out: dict = {
        "status": status,
        "rows": len(recs),
        "tested": len(tested),
        "looped_rows": len(looped),
        "borderline_rows": sum(bool(r.get("borderline")) for r in tested),
        "not_testable_rows": sum(r.get("verdict") == VERDICT_NOT_TESTABLE for r in recs),
        # Rows that lose objective terms; the other rows of the capture keep them.
        "unweighted_rows": sum(bool(unweighted_terms(r)) for r in recs),
    }
    groups = groups_of(rows)
    if groups:
        out["groups"] = groups
    if looped:
        joins = [r["join_s"] for r in looped if r.get("join_s") is not None]
        out["period_ms_median"] = round(float(np.median([r["period_ms"] for r in looped])), 3)
        out["residual_db_median"] = round(float(np.median([r["residual_db"] for r in looped])), 1)
        out["join_s_median"] = round(float(np.median(joins)), 3) if joins else None
        out["read_to_join_rows"] = sum("read_to_join_s" in r for r in looped)
        union: dict[str, str] = {}
        for r in looped:
            for dim, why in (r.get("excluded") or {}).items():
                union.setdefault(dim, why)
        out["excluded"] = {d: union[d] for d in sorted(union)}
        # Notes of one stored waveform are one reading of a frozen cycle, so the count that
        # stands for independent snapshot readings is the groups, not the rows.
        independent = len(groups_of(rows)) or len(looped)
        out["snapshot"] = {
            d: {"label": SNAPSHOT_LABEL, "independent_readings": independent}
            for d in SNAPSHOT_DIMENSIONS
        }
        out["body_on_lines_rows"] = sum("body" in (r.get("excluded") or {}) for r in looped)
    return out


#: The reference-row field each snapshot dimension is read from.
DIMENSION_FIELD = {
    "balance": "partials_db",
    "centroid_pct": "centroid_hz",
    "register": "held_peak_dbfs",
    "stretch": "cents_vs_et",
    "tnr": "tnr_db",
    "body": "body_below_f0_db",
}

REGISTER_FLAG = (
    "level read across notes that share one stored waveform: the spectrum is one fixed shape "
    "shifted with pitch, so the register curve over the group is a transposition artefact"
)


def _note_distances(ladders: dict[tuple[int, int], list]) -> dict[tuple[int, int], float]:
    """Median ladder distance between two notes over the velocities both carry."""
    notes = sorted({n for n, _ in ladders})
    out: dict[tuple[int, int], float] = {}
    for i, a in enumerate(notes):
        for b in notes[i + 1 :]:
            shared = [v for n, v in ladders if n == a and (b, v) in ladders]
            ds = [ladder_distance(ladders[(a, v)], ladders[(b, v)]) for v in shared]
            ds = [d for d in ds if d is not None]
            if ds:
                out[(a, b)] = float(np.median(ds))
    return out


def _cluster_notes(notes: list[int], dist: dict[tuple[int, int], float]) -> list[list[int]]:
    """Average-link agglomeration of notes, merging while the closest pair is within the cut."""
    clusters = [[n] for n in notes]

    def link(x: list[int], y: list[int]) -> float | None:
        ds = [dist.get((min(a, b), max(a, b))) for a in x for b in y]
        return None if any(d is None for d in ds) else float(np.mean(ds))

    while True:
        best = None
        for i, x in enumerate(clusters):
            for j in range(i + 1, len(clusters)):
                d = link(x, clusters[j])
                if d is not None and d <= GROUP_SAME_DB and (best is None or d < best[0]):
                    best = (d, i, j)
        if best is None:
            return sorted(clusters, key=min)
        _, i, j = best
        clusters[i] = sorted(clusters[i] + clusters[j])
        del clusters[j]


def assign_groups(rows: list[dict]) -> None:
    """Give every row that carries a ladder the id of the stored-waveform group of its note.

    Notes whose ladders match (see `GROUP_SAME_DB`) are transpositions of one stored
    waveform, so they are one reading, not several, looped or not. A note matching no other is
    its own group. Sets `group` and `group_notes` in the row's `loop` record, a `register`
    flag on a shared group, and `group_uncertain` (both readings) where another note's ladder
    sits in the `GROUP_UNCERTAIN_DB` band. Ids run per capture, by lowest note within a timbre.
    """
    next_id = 0
    for timbre in dict.fromkeys(r.get("timbre") for r in rows):
        mine = [
            r
            for r in rows
            if r.get("timbre") == timbre
            and isinstance(r.get("loop"), dict)
            and r["loop"].get("ladder_db")
        ]
        ladders = {(r["note"], r["velocity"]): r["loop"]["ladder_db"] for r in mine}
        notes = sorted({r["note"] for r in mine})
        dist = _note_distances(ladders)
        group_of: dict[int, int] = {}
        members: dict[int, list[int]] = {}
        for cluster in _cluster_notes(notes, dist):
            for n in cluster:
                group_of[n] = next_id
            members[next_id] = cluster
            next_id += 1
        lo, hi = GROUP_UNCERTAIN_DB
        for r in mine:
            rec, n = r["loop"], r["note"]
            g = group_of[n]
            rec["group"], rec["group_notes"] = g, members[g]
            to = {m: dist[(min(n, m), max(n, m))] for m in notes if (min(n, m), max(n, m)) in dist}
            inside = [d for m, d in to.items() if m in members[g]]
            outside = [d for m, d in to.items() if m not in members[g]]
            for key in ("group_uncertain", "flagged"):
                rec.pop(key, None)
            between = sorted(m for m, d in to.items() if lo <= d <= hi)
            if between:
                rec["group_uncertain"] = {
                    "ambiguous_with": between,
                    "loosest_inside_db": round(max(inside), 1) if inside else None,
                    "nearest_outside_db": round(min(outside), 1) if outside else None,
                    "band_db": list(GROUP_UNCERTAIN_DB),
                }
            for entry in (rec.get("snapshot") or {}).values():
                entry["group"] = g
            if len(members[g]) > 1:
                rec["flagged"] = {"register": REGISTER_FLAG}


def groups_of(rows: list[dict]) -> dict[str, list[int]]:
    """The groups of a capture's looped rows: id -> notes."""
    out: dict[str, list[int]] = {}
    for r in rows:
        rec = r.get("loop")
        if isinstance(rec, dict) and rec.get("group") is not None:
            out[str(rec["group"])] = rec["group_notes"]
    return dict(sorted(out.items(), key=lambda kv: int(kv[0])))


def independent_count(rows: list[dict], dimension: str) -> int:
    """How many independent readings of `dimension` the rows hold.

    Notes of one stored-waveform group are one reading however many notes and velocities
    they cover; a row that is not looped, or looped but ungrouped, is its own. A row whose
    loop record excludes the dimension carries no reading of it.
    """
    field = DIMENSION_FIELD.get(dimension)
    seen: set = set()
    alone = 0
    for r in rows:
        rec = r.get("loop") if isinstance(r.get("loop"), dict) else {}
        if (field is not None and r.get(field) is None) or dimension in (rec.get("excluded") or {}):
            continue
        if rec.get("group") is not None:
            seen.add((r.get("timbre"), rec["group"]))
        else:
            alone += 1
    return len(seen) + alone


def independence_line(rows: list[dict], dimensions) -> str | None:
    """`dimension n of m` for each dimension whose reference rows share stored waveforms, else None.

    `n` is `independent_count`, `m` the rows that carry the reading; a dimension is listed
    only where grouping makes them differ.
    """
    parts = []
    for dim in dimensions:
        field = DIMENSION_FIELD.get(dim)
        if field is None:
            continue
        have = sum(1 for r in rows if r.get(field) is not None)
        n = independent_count(rows, dim)
        if n != have:
            parts.append(f"{dim} {n} of {have}")
    return ", ".join(parts) or None


def note_groups(rows: list[dict], timbre: str | None = None) -> dict[int, int]:
    """Note -> stored-waveform group id, from reference rows; notes with no group are absent."""
    return {
        r["note"]: r["loop"]["group"]
        for r in rows
        if isinstance(r.get("loop"), dict)
        and r["loop"].get("group") is not None
        and (timbre is None or r.get("timbre") == timbre)
    }


def committed_note_groups(capture_id: str | None, timbre: str | None = None) -> dict[int, int]:
    """`note_groups` of the committed reference profile of a capture; empty without one."""
    if not capture_id:
        return {}
    path = Path(__file__).resolve().parent / "reference" / f"{capture_id}.json"
    if not path.exists():
        return {}
    return note_groups(json.loads(path.read_text()).get("rows") or [], timbre)


#: The dimension whose exclusion takes each objective term off a row: the term reads what the
#: dimension reads (`mod` and `slope` follow the decay, `tail` the aftersound).
LOOP_TERM_DIMENSION = {
    "cents": "stretch",
    "tnr": "tnr",
    "slope": "decay",
    "tail": "aftersound",
    "mod": "decay",
}


def unweighted_terms(rec: dict | None) -> list[str]:
    """The objective terms a row's loop record takes off it, from its own exclusions."""
    excluded = (rec or {}).get("excluded") or {}
    return [t for t, dim in LOOP_TERM_DIMENSION.items() if dim in excluded]


def committed_loop_cells(capture_id: str | None, timbre: str | None = None) -> dict[tuple, list]:
    """(note, velocity) -> unweighted terms, from the committed reference profile; empty without."""
    if not capture_id:
        return {}
    path = Path(__file__).resolve().parent / "reference" / f"{capture_id}.json"
    if not path.exists():
        return {}
    return {
        (r["note"], r["velocity"]): unweighted_terms(r.get("loop"))
        for r in json.loads(path.read_text()).get("rows") or []
        if timbre is None or r.get("timbre") == timbre
    }


def mark_loop_rows(rows: list[dict], capture_id: str | None, timbre: str | None = None) -> int:
    """Set `loop_unweighted` on the oracle rows whose reference row loops; the count marked.

    The loss skips those terms on a marked row and keeps them on every other row, so a
    capture that is only partly looped is weighted row by row.
    """
    cells = committed_loop_cells(capture_id, timbre)
    marked = 0
    for r in rows:
        terms = cells.get((r.get("note"), r.get("velocity")))
        if terms:
            r["loop_unweighted"] = terms
            marked += 1
    return marked
