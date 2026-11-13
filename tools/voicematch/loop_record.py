"""Whether a captured note is a sample looped bit-exactly, and what that makes unreadable.

A sampler holds a sustained note by repeating one cycle. The repetition is sample-exact,
so anything read across it measures the loop and not the instrument. The test is the
envelope-normalised correlation of the held note with itself at a lag of at least 100 ms
(several periods of any pitched note, so natural jitter has accumulated); a loop leaves
a residual 40 to 70 dB under the signal, a stable natural source does not.

Contract: pure functions over a mono segment, no I/O, deterministic.
"""

from __future__ import annotations

import numpy as np

# Residual of the best frame at lag >= 100 ms: library winds loop at -82..-104 dB, plucked
# loops at -37..-65, jittered natural sources read -29 dB or worse. -36 keeps the plucked
# positives and rejects the jittered control (-36..-29 is recorded as borderline).
LOOPED_BEST_DB = -36.0
# Median over the frames tried; a loop that only some frames show is not one.
LOOPED_MEDIAN_DB = -30.0
BORDERLINE_DB = -30.0

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

#: `body` reads 40 Hz to 0.7 f0. One loop line in it is enough: past an exact repeat the sub-f0
#: power is the cycle-to-cycle difference frozen into the loop, not radiated content.
BODY_BAND_BOTTOM_HZ = 40.0
BODY_BAND_TOP = 0.7
BODY_MIN_LINES = 1


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
    return {"L": lag, "r": r_short, "r_long": r_long}


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
    median_db = _db_residual(float(np.median([f["r_long"] for f in frames])))
    looped = best_db <= LOOPED_BEST_DB and median_db <= LOOPED_MEDIAN_DB
    rec: dict = {
        "verdict": VERDICT_LOOPED if looped else VERDICT_NOT,
        "residual_db": round(best_db, 1),
        "frames": len(frames),
    }
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
    rec: dict, *, body_on_lines: bool = False, attack_ms: float | None = None
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
    where = (
        f"loop period {rec['period_ms']} ms, exact repetition from {join} s, "
        f"residual {rec['residual_db']} dB"
        if join is not None
        else f"loop period {rec['period_ms']} ms, residual {rec['residual_db']} dB, no join found"
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
                excluded[dim] = f"sub-f0 power is the loop's subharmonics at 1/period ({where})"
        elif past:
            kind = "reads the loop" if dim in LOOP_READ_DIMENSIONS else "time-varying past the join"
            excluded[dim] = f"{kind} ({where})"
    if attack_ms is not None and (join is None or join <= attack_ms / 1000.0):
        for dim in ONSET_DIMENSIONS:
            excluded[dim] = f"onset is the envelope over the loop ({where})"
    return excluded, snapshot


def loop_summary(rows: list[dict]) -> dict:
    """The capture-level loop statement of a reference profile's rows.

    `status` is `looped` where 80 % or more of the tested rows loop, `not_looped` at 20 %
    or less, `mixed` between; the loss reads `looped` only.
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
    }
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
        out["snapshot"] = {d: {"label": SNAPSHOT_LABEL} for d in SNAPSHOT_DIMENSIONS}
        out["body_on_lines_rows"] = sum("body" in (r.get("excluded") or {}) for r in looped)
    return out
