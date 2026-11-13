"""Whether a capture carries a recording floor under its notes, and what that floor censors.

A floor is declared only where the between-partial level is note-independent across
the capture's rows AND stationary in time through the hold. Fixed lines (mains hum,
a voice's latched lines) are found, masked from the floor and recorded, never read
as one. A sub-f0 floor censors `body` and `tnr` at the floor; mains hum is counted
into the censor whether or not a floor is declared; an inter-partial floor is
recorded and censors nothing. The result is written into each reference row as
`floor_state` and into the profile as `recording_floor`, so a gate or a loss that
reads a censor can be traced to it.
"""

from __future__ import annotations

import numpy as np
from metrics import _spectrum
from metrics_floor import BODY_FLOOR_MARGIN_DB, FLOOR_TOP_HZ
from metrics_note import BODY_MIN_F0_HZ
from metrics_partials import (
    TNR_TOP_HZ,
    am_margin_hz,
    fm_margin_hz,
    partial_hz,
    tnr_mask_parts,
)
from shape.partials import BED_CLEAR_DB

FLOOR_LO_HZ = 40.0
#: Third-octave band ratio of the floor's frequency axis.
BAND_EDGE_RATIO = 2.0 ** (1.0 / 3.0)
#: Bins a band needs before its median density is read.
MIN_BAND_BINS = 8
#: Median of an exponential power distribution to its mean, in dB.
MEDIAN_TO_MEAN_DB = -10.0 * np.log10(np.log(2.0))
#: A row's band within this of the floor counts as at the floor, for the note-independence share.
AT_FLOOR_DB = 3.0
#: Padding factor of the line search's refinement spectrum.
PAD = 4
#: A narrow peak is a line candidate at this prominence over its neighbourhood.
LINE_PROM_DB = 12.0
#: A line already found is read on a row where it stands this far over its ring.
LINE_RECHECK_DB = 8.0
LINE_TOL_HZ = 0.2
LINE_BASE_BINS = 41
LINE_LO_HZ = 20.0
#: A recorded line is on at least this share of the rows and is mains or at least this loud (dBFS, sine RMS).
LINE_RECORD_COVERAGE, LINE_RECORD_MIN_DBFS = 0.5, -125.0
MAINS_HZ = (50.0, 60.0)
#: Highest mains multiple read as hum, and the highest hum line kept.
MAINS_MAX_MULTIPLE = 8

#: Floor level (dB/Hz) under which a spectrum is silence rather than a floor.
LEVEL_MIN_DB_HZ = -150.0
#: Sub-f0 bands must agree to this spread (dB) across rows of different f0.
SUB_TIGHT_SD_DB = 3.5
#: Rows' own bands at the floor, as a share, for an inter-partial floor to be note-independent.
AGREE_SHARE_MIN = 0.6

#: Time tracking: 0.25 s frames at a 0.1 s hop from 0.12 s into the gate, shorter frames on a short gate.
FRAME_S, HOP_S, START_S = 0.25, 0.10, 0.12
#: Slope (dB/s) of the between-partial level from the hum line's, over which it is not stationary.
#: About ten slope standard errors of a 40-frame fit, and twice under the weakest decay measured (-1.6).
SLOPE_TOL_DB_S = 1.0
#: Correlation of the between-partial level with the tone's at which it is read as the tone's own.
#: Chance |r| over 40 frames stays under 0.3.
CO_CORR = 0.6
#: Tone level sd (dB) under which a correlation says nothing.
TONE_VARIES_DB = 1.5
#: Share of evaluable rows that must read as floor-like, and the rows needed to say.
ROW_SHARE_MIN = 0.6
MIN_EVAL_ROWS = 3
MIN_FRAMES = 10
#: Length of the end of the gate read for partials' clearance over the bed.
CLEARANCE_TAIL_S = 0.12
#: Partials whose clearance is counted, as `partial_clearance` reads them.
CLEAR_PARTIALS = 8

SOURCE_CLEARANCE = "clearance"
BASIS_SUB_F0 = "sub-f0"
BASIS_INTER = "inter-partial"
CENSOR_FLOOR = "reference band at the declared sub-f0 floor (stationary, note-independent)"
CENSOR_HUM = "reference band at mains hum"
INTER_PARTIAL_NOTE = "inter-partial floor: recorded only, censors nothing"


def _band_edges(sr: int) -> np.ndarray:
    top = min(FLOOR_TOP_HZ, sr / 2.0)
    edges = [FLOOR_LO_HZ]
    while edges[-1] * BAND_EDGE_RATIO <= top:
        edges.append(edges[-1] * BAND_EDGE_RATIO)
    return np.asarray(edges)


def _psd(x: np.ndarray, sr: int, pad: int = 1):
    n = len(x)
    w = np.hanning(n)
    spec = np.fft.rfft(np.asarray(x, dtype=np.float64) * w, n=n * pad)
    psd = 2.0 * np.abs(spec) ** 2 / (sr * float(np.sum(w**2)))
    return np.fft.rfftfreq(n * pad, 1.0 / sr), psd, float(np.sum(w)), float(np.sum(w**2))


def _tonal_mask(freqs, f0, b, tnr_mask, top_hz, bin_hz, floor_margin_bins=3.0):
    """Bins within a partial's reach, as `tone_to_noise_db` lays them (wider of 3 bins)."""
    mask = np.zeros(freqs.shape, dtype=bool)
    if f0 <= 0.0:
        return mask
    motion, tremolo = tnr_mask_parts(tnr_mask or {})
    am = 0.0 if tremolo is None else am_margin_hz(f0, bin_hz, tremolo)
    n = 1
    while True:
        target = partial_hz(f0, n, b)
        margin = max(fm_margin_hz(target, f0, bin_hz, motion), am, floor_margin_bins * bin_hz)
        half = min(max(0.02 * target, margin), max(0.25 * f0, floor_margin_bins * bin_hz))
        if target - half > top_hz:
            break
        mask |= (freqs >= target - half) & (freqs <= target + half)
        n += 1
    return mask


def _tnr_mask_exact(freqs, f0, b, tnr_mask, top, bin_hz):
    """`tone_to_noise_db`'s own mask."""
    motion, tremolo = tnr_mask_parts(tnr_mask or {})
    mask = np.zeros(freqs.shape, dtype=bool)
    am = 0.0 if tremolo is None else am_margin_hz(f0, bin_hz, tremolo)
    n = 1
    while True:
        target = partial_hz(f0, n, b)
        margin = max(fm_margin_hz(target, f0, bin_hz, motion), am)
        half = min(max(0.02 * target, margin), 0.25 * f0)
        if target - half > top:
            break
        mask |= (freqs >= target - half) & (freqs <= target + half)
        n += 1
    return mask


def _running_median(a: np.ndarray, k: int) -> np.ndarray:
    pad = k // 2
    ap = np.pad(a, pad, mode="edge")
    out = np.empty_like(a)
    step = 20000
    for s in range(0, len(a), step):
        seg = ap[s : s + step + 2 * pad]
        win = np.lib.stride_tricks.sliding_window_view(seg, k)
        out[s : s + len(win)] = np.median(win, axis=1)[: len(a) - s]
    return out


def _mains_multiple(f: float) -> tuple[float, int] | None:
    for base in MAINS_HZ:
        m = round(f / base)
        if 1 <= m <= MAINS_MAX_MULTIPLE and abs(f - m * base) <= 0.12 * m:
            return base, m
    return None


def _row_peaks(x, sr, f0, b, tnr_mask):
    """Narrow peaks standing `LINE_PROM_DB` over their neighbourhood, outside the partials."""
    freqs, psd, sw, sw2 = _psd(x, sr, PAD)
    ufreqs, upsd, _, _ = _psd(x, sr, 1)
    bin_hz = ufreqs[1] - ufreqs[0]
    top = min(FLOOR_TOP_HZ, sr / 2.0)
    db_u = 10.0 * np.log10(upsd + 1e-300)
    base = _running_median(db_u, LINE_BASE_BINS)
    cand = np.where(
        (db_u - base >= LINE_PROM_DB)
        & (db_u >= np.roll(db_u, 1))
        & (db_u >= np.roll(db_u, -1))
        & (ufreqs >= LINE_LO_HZ)
        & (ufreqs <= top)
    )[0]
    tmask = (
        _tonal_mask(freqs, f0, b, tnr_mask, top, bin_hz) if f0 > 0 else np.zeros(freqs.shape, bool)
    )
    db_p = 10.0 * np.log10(psd + 1e-300)
    peaks = []
    for k in cand:
        lo, hi = max(0, (k - 2) * PAD), min(len(psd), (k + 3) * PAD + 1)
        j = lo + int(np.argmax(psd[lo:hi]))
        if tmask[j]:
            continue
        fj = freqs[j]
        if 0 < j < len(psd) - 1:
            la, lb, lc = db_p[j - 1], db_p[j], db_p[j + 1]
            den = la - 2 * lb + lc
            if den < 0:
                fj = freqs[j] + 0.5 * (la - lc) / den * (freqs[1] - freqs[0])
        # Sine amplitude: peak |X| = A * sum(w) / 2, psd = 2 |X|^2 / (sr sum(w^2)).
        xm = np.sqrt(psd[j] * sr * sw2 / 2.0)
        peaks.append(
            (float(fj), float(20.0 * np.log10(max(2.0 * xm / sw, 1e-30))), float(db_u[k] - base[k]))
        )
    return peaks, (freqs, db_p, tmask, base, ufreqs, sw, sw2, sr)


def _recheck(ctx, fc):
    freqs, db_p, _t, base, ufreqs, _sw, _sw2, _sr = ctx
    df = freqs[1] - freqs[0]
    k0 = round(fc / df)
    w = int(np.ceil(LINE_TOL_HZ / df))
    sel = np.arange(max(0, k0 - w), min(len(freqs), k0 + w + 1))
    if sel.size == 0:
        return None
    j = sel[int(np.argmax(db_p[sel]))]
    ku = int(np.argmin(np.abs(ufreqs - freqs[j])))
    lo, hi = max(0, ku - 20), min(len(base), ku + 21)
    ring = np.r_[np.arange(lo, max(lo, ku - 6)), np.arange(min(hi, ku + 7), hi)]
    return None if ring.size == 0 else (j, ring)


def _find_lines(rows_ctx, notes):
    """Cluster peaks across rows; keep those present on most observable rows and notes."""
    allp = sorted((f, ri, lvl) for ri, (peaks, _c) in enumerate(rows_ctx) for f, lvl, _p in peaks)
    clusters, cur = [], []
    for item in allp:
        if cur and item[0] - cur[-1][0] > LINE_TOL_HZ:
            clusters.append(cur)
            cur = []
        cur.append(item)
    if cur:
        clusters.append(cur)
    lines = []
    for cl in clusters:
        fc = float(np.median([c[0] for c in cl]))
        if len({c[1] for c in cl}) < max(2, 0.25 * len(rows_ctx)):
            continue
        pres_rows = {c[1] for c in cl if abs(c[0] - fc) <= LINE_TOL_HZ}
        obs, pres, levels = [], [], []
        for ri, (peaks, ctx) in enumerate(rows_ctx):
            freqs, db_p, tmask, _b, _u, sw, sw2, sr = ctx
            if fc > min(FLOOR_TOP_HZ, sr / 2.0):
                continue
            if tmask[min(len(freqs) - 1, round(fc / (freqs[1] - freqs[0])))]:
                continue
            obs.append(ri)
            hit = None
            if ri in pres_rows:
                near = [p for p in peaks if abs(p[0] - fc) <= LINE_TOL_HZ]
                hit = max(near, key=lambda p: p[1]) if near else None
            else:
                r = _recheck(ctx, fc)
                if r is not None:
                    jj, ring = r
                    prom = db_p[jj] - np.median(db_p[::PAD][ring])
                    if prom >= LINE_RECHECK_DB:
                        xm = np.sqrt(10 ** (db_p[jj] / 10.0) * sr * sw2 / 2.0)
                        hit = (freqs[jj], 20.0 * np.log10(max(2.0 * xm / sw, 1e-30)), prom)
            if hit is not None:
                pres.append(ri)
                levels.append(hit[1])
        notes_obs = {notes[i] for i in obs}
        notes_pres = {notes[i] for i in pres}
        need = min(3, len(set(notes)))
        if len(notes_obs) >= need and len(notes_pres) >= need and len(pres) >= 0.75 * len(obs):
            peak = float(np.mean(levels))
            mains = _mains_multiple(fc)
            lines.append(
                {
                    "hz": round(fc, 3),
                    "dbfs": round(peak - 3.01, 2),
                    "rows_present": len(pres),
                    "rows_observable": len(obs),
                    "coverage": round(len(obs) / len(rows_ctx), 2),
                    "mains": None if mains is None else f"{int(mains[0])}Hz x{mains[1]}",
                }
            )
    return lines


def _significant(line: dict) -> bool:
    """Lines worth recording: on half the rows and mains or audible over the recording's own floor."""
    return line["coverage"] >= LINE_RECORD_COVERAGE and (
        line["mains"] is not None or line["dbfs"] >= LINE_RECORD_MIN_DBFS
    )


def hum_lines(lines: list[dict]) -> list[dict]:
    """The mains family: multiples of 50 or 60 Hz whose own fundamental is among the lines.

    A multiple alone is a coincidence (a 359 Hz line is 60 Hz x6 by arithmetic); the
    fundamental standing as a line is what makes the family hum.
    """
    fundamentals = {
        int(m[0]) for ln in lines if (m := _mains_multiple(ln["hz"])) is not None and m[1] == 1
    }
    out = []
    for ln in lines:
        m = _mains_multiple(ln["hz"])
        if m is not None and int(m[0]) in fundamentals:
            out.append(ln)
    return out


def _line_power(lines: list[dict], lo: float, hi: float, skip=None) -> float:
    """Mean-square power of the lines in [lo, hi), with `skip(hz)` dropping covered ones."""
    return float(
        sum(
            10 ** (ln["dbfs"] / 10.0)
            for ln in lines
            if lo <= ln["hz"] < hi and not (skip is not None and skip(ln["hz"]))
        )
    )


def _band_density(x, sr, f0, b, tnr_mask, lines):
    """Per-band inter-partial density (dB/Hz), median-based, partials and lines masked."""
    freqs, psd, _sw, _sw2 = _psd(x, sr, 1)
    bin_hz = freqs[1] - freqs[0]
    edges = _band_edges(sr)
    mask = (
        _tonal_mask(freqs, f0, b, tnr_mask, edges[-1], bin_hz)
        if f0 > 0
        else np.zeros(freqs.shape, bool)
    )
    for ln in lines:
        mask |= np.abs(freqs - ln["hz"]) <= 4.0 * bin_hz
    dens = np.full(len(edges) - 1, np.nan)
    for i in range(len(edges) - 1):
        sel = (freqs >= edges[i]) & (freqs < edges[i + 1]) & ~mask
        if sel.sum() >= MIN_BAND_BINS:
            med = float(np.median(psd[sel]))
            if med > 0:
                dens[i] = 10.0 * np.log10(med) + MEDIAN_TO_MEAN_DB
    return dens, edges


def _filled(centres, dens):
    ok = np.isfinite(dens)
    if not ok.any():
        return None
    return np.interp(np.log2(centres), np.log2(centres[ok]), dens[ok])


def _power_in(F, edges, lo, hi) -> float:
    """Integral of the floor density (dB/Hz per band) over [lo, hi] Hz, linear power."""
    centres = np.sqrt(edges[:-1] * edges[1:])
    filled = _filled(centres, F)
    if filled is None or hi <= lo:
        return 0.0
    grid = np.linspace(lo, hi, 2000)
    d = np.interp(np.log2(grid), np.log2(centres), filled)
    return float(np.sum(10 ** (d / 10.0)) * (hi - lo) / len(grid))


def _ols(t, y) -> float:
    return float(np.polyfit(t, y, 1)[0])


def _frames(held: np.ndarray, sr: int):
    seg = held[int(START_S * sr) :]
    for frame_s, hop_s in ((FRAME_S, HOP_S), (0.15, 0.05), (0.10, 0.04)):
        n, hop = int(frame_s * sr), int(hop_s * sr)
        if len(seg) >= n + (MIN_FRAMES - 1) * hop:
            idx = np.arange(0, len(seg) - n + 1, hop)
            w = np.hanning(n)
            spec = np.abs(np.fft.rfft(np.stack([seg[i : i + n] for i in idx]) * w, axis=1)) ** 2
            return 2.0 * spec / (sr * np.sum(w**2)), idx / sr, n
    return None, None, None


def _row_time(held, sr, f0, b, tnr_mask, lines, hum):
    """The between-partial level's slope through the hold, its coupling to the tone, the hum's slope."""
    psd, t, nfr = _frames(held, sr)
    if psd is None:
        return None
    fr = np.fft.rfftfreq(nfr, 1.0 / sr)
    bh = fr[1] - fr[0]
    m = _tonal_mask(fr, f0, b, tnr_mask, 4000.0, bh)
    lm = np.zeros(fr.shape, bool)
    for ln in lines:
        lm |= np.abs(fr - ln["hz"]) <= 3.0 * bh
    tone_bins = m & (fr >= 0.8 * f0) & (fr <= 4000)
    tone = 10 * np.log10(psd[:, tone_bins].sum(axis=1) * bh + 1e-300)
    sub = (fr >= FLOOR_LO_HZ) & (fr < 0.7 * f0) & ~m & ~lm
    if f0 >= 120 and sub.sum() >= 6:
        noise = 10 * np.log10(psd[:, sub].mean(axis=1) + 1e-300)
    else:
        inter = (fr >= FLOOR_LO_HZ) & (fr <= 4000) & ~m & ~lm
        noise = 10 * np.log10(np.median(psd[:, inter], axis=1) / np.log(2) + 1e-300)
    ok = np.isfinite(noise) & (noise > -250) & (tone > -250)
    if ok.sum() < MIN_FRAMES:
        return None
    out = {
        "noise_slope": _ols(t[ok], noise[ok]),
        "tone_slope": _ols(t[ok], tone[ok]),
        "tone_sd": float(np.std(tone[ok])),
    }
    out["corr"] = (
        float(np.corrcoef(noise[ok], tone[ok])[0, 1])
        if out["tone_sd"] > 1e-6 and np.std(noise[ok]) > 1e-9
        else None
    )
    slopes = []
    for ln in hum:
        if ln["hz"] > 120:
            continue
        k = round(ln["hz"] / bh)
        if m[max(0, k - 2) : k + 3].any():
            continue
        lvl = 10 * np.log10(psd[:, max(0, k - 1) : k + 2].sum(axis=1) + 1e-300)
        if np.all(np.isfinite(lvl)):
            slopes.append(_ols(t[ok], lvl[ok]))
    out["hum_slope"] = slopes[0] if slopes else None
    return out


def _tail_clearance(held: np.ndarray, sr: int, f0: float, b: float) -> int | None:
    """Partials (of the first `CLEAR_PARTIALS`) standing `BED_CLEAR_DB` over the bed in the gate's last 120 ms."""
    n = int(CLEARANCE_TAIL_S * sr)
    if len(held) < n or f0 <= 0:
        return None
    seg = held[-n:]
    spec = np.abs(np.fft.rfft(seg * np.hanning(n)))
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    hz = [partial_hz(f0, i, b) for i in range(1, CLEAR_PARTIALS + 2)]
    cleared = 0
    for i in range(CLEAR_PARTIALS):
        if hz[i + 1] > sr / 2.0 - 100.0:
            break
        k = int(np.argmin(np.abs(fr - hz[i])))
        kv = int(np.argmin(np.abs(fr - 0.5 * (hz[i] + hz[i + 1]))))
        peak = spec[max(0, k - 1) : k + 2].max()
        bed = spec[max(0, kv - 1) : kv + 2].max()
        if 20.0 * np.log10(max(peak, 1e-12)) >= 20.0 * np.log10(max(bed, 1e-12)) + BED_CLEAR_DB:
            cleared += 1
    return cleared


def _pearson(a, b):
    a, b = np.asarray(a, float), np.asarray(b, float)
    ok = np.isfinite(a) & np.isfinite(b)
    if ok.sum() < 4 or np.std(a[ok]) < 1e-9 or np.std(b[ok]) < 1e-9:
        return None
    return float(np.corrcoef(a[ok], b[ok])[0, 1])


def _r(v, nd=2):
    return None if v is None else round(float(v), nd)


def declare(rows: list[dict], windows: list[dict]) -> tuple[dict, list[dict]]:
    """The capture's floor record and one `floor_state` per row (same order).

    `rows` are measured reference rows (`f0_hz`, `body_under_db`, `tnr_mask`,
    `inharmonicity_*`); `windows` hold each row's `x` (the fit window), `held`
    (the gate) and `sr`.
    """
    notes = [r["note"] for r in rows]
    f0s = [float(r["f0_hz"]) for r in rows]
    bs = [float(r["inharmonicity_b"]) if r.get("inharmonicity_reliable") else 0.0 for r in rows]
    masks = [r.get("tnr_mask") for r in rows]
    ctxs = [_row_peaks(w["x"], w["sr"], f0, b, m) for w, f0, b, m in zip(windows, f0s, bs, masks)]
    lines = _find_lines(ctxs, notes)
    hum = hum_lines(lines)
    rec: dict = {
        "declared": False,
        "rows": len(rows),
        "lines": [dict(ln) for ln in lines if _significant(ln)],
        "lines_found": len(lines),
        "hum": (
            None
            if not hum
            else {
                "hz": min(h["hz"] for h in hum),
                "dbfs": max(h["dbfs"] for h in hum),
                "lines": [h["hz"] for h in hum],
            }
        ),
    }

    dens_rows, edges = [], None
    for w, f0, b, m in zip(windows, f0s, bs, masks):
        d, edges = _band_density(w["x"], w["sr"], f0, b, m, lines)
        dens_rows.append(d)
    D = np.vstack(dens_rows)
    nb = D.shape[1]
    centres = np.sqrt(edges[:-1] * edges[1:])
    nvalid = np.isfinite(D).sum(axis=0)
    F = np.full(nb, np.nan)
    for i in range(nb):
        col = D[np.isfinite(D[:, i]), i]
        if col.size >= 3:
            F[i] = np.percentile(col, 10)
        elif col.size >= 1:
            F[i] = col.min()
    # Bands wholly under 0.7 f0 hold no partial, so a level shared by rows of different f0 there is not the note's.
    S = np.full_like(D, np.nan)
    for i, f0 in enumerate(f0s):
        if f0 > 0:
            S[i, edges[1:] <= 0.7 * f0] = D[i, edges[1:] <= 0.7 * f0]
    sub_med, sub_sd = np.full(nb, np.nan), np.full(nb, np.nan)
    for i in range(nb):
        col = S[np.isfinite(S[:, i]), i]
        if col.size >= 3:
            sub_med[i], sub_sd[i] = float(np.median(col)), float(np.std(col))
    sub_ok = np.isfinite(sub_med)
    sub_tight = sub_ok & (sub_sd <= SUB_TIGHT_SD_DB)
    sub_present = bool(
        sub_ok.sum() >= 4
        and sub_tight.sum() >= 0.6 * sub_ok.sum()
        and np.median(sub_med[sub_ok]) > LEVEL_MIN_DB_HZ
    )
    okb = np.isfinite(F) & (nvalid >= 3)
    note_independent = False
    level = basis = None
    if okb.sum() >= 4:
        if sub_present:
            F = np.where(sub_tight, sub_med, F)
            okb = np.isfinite(F) & (nvalid >= 3)
        level = float(np.median(F[okb]))
        agree = []
        for i in np.nonzero(okb)[0]:
            col = D[np.isfinite(D[:, i]), i]
            agree.append(float(np.mean(col - F[i] <= AT_FLOOR_DB)))
        agree_cap = float(np.mean(agree))
        note_independent = bool(
            level > LEVEL_MIN_DB_HZ and (sub_present or agree_cap >= AGREE_SHARE_MIN)
        )
        basis = BASIS_SUB_F0 if sub_present else BASIS_INTER
        rec.update(
            {
                "note_independent": note_independent,
                "basis": basis if note_independent else None,
                "level_db_hz": _r(level),
                "slope_db_oct": _r(np.polyfit(np.log2(centres[okb]), F[okb], 1)[0]),
                "agree_share": _r(agree_cap, 3),
            }
        )
    else:
        rec.update({"note_independent": False, "basis": None, "reason": "too few bands"})

    times = [
        _row_time(w["held"], w["sr"], f0, b, m, lines, hum)
        for w, f0, b, m in zip(windows, f0s, bs, masks)
    ]
    ev = [x for x in times if x]
    hum_slopes = [x["hum_slope"] for x in ev if x["hum_slope"] is not None]
    ref = float(np.median(hum_slopes)) if hum_slopes else 0.0
    for x in ev:
        stationary = abs(x["noise_slope"] - ref) <= SLOPE_TOL_DB_S
        comoves = bool(
            x["corr"] is not None and x["tone_sd"] >= TONE_VARIES_DB and x["corr"] >= CO_CORR
        )
        x["floor_like"] = bool(stationary and not comoves)
    share = float(np.mean([x["floor_like"] for x in ev])) if ev else None
    stationary_cap = bool(share is not None and len(ev) >= MIN_EVAL_ROWS and share >= ROW_SHARE_MIN)
    corrs = [x["corr"] for x in ev if x["corr"] is not None]
    rec["stationary"] = {
        "verdict": stationary_cap,
        "evaluable_rows": len(ev),
        "floor_like_share": _r(share, 3),
        "slope_db_s": _r(np.median([x["noise_slope"] for x in ev])) if ev else None,
        "tone_slope_db_s": _r(np.median([x["tone_slope"] for x in ev])) if ev else None,
        "corr": _r(np.median(corrs)) if corrs else None,
        "hum_reference_slope_db_s": _r(ref) if hum_slopes else None,
    }
    declared = bool(note_independent and stationary_cap)
    censoring = declared and basis == BASIS_SUB_F0
    rec["declared"] = declared
    if declared:
        rec["censors"] = ["body", "tnr"] if censoring else []
        if not censoring:
            rec["note"] = INTER_PARTIAL_NOTE
    else:
        rec["reason"] = rec.get("reason") or (
            "not note-independent" if not note_independent else "not stationary in time"
        )

    states = []
    for i, (r, w, f0, b, m) in enumerate(zip(rows, windows, f0s, bs, masks)):
        sr = w["sr"]
        t = times[i]
        st: dict = {
            "declared": declared,
            "basis": basis if declared else None,
            "source": r.get("floor_source") or (SOURCE_CLEARANCE if declared else None),
            "level_db_hz": _r(level) if declared else None,
            "stationary": None
            if t is None
            else {
                "slope_db_s": _r(t["noise_slope"]),
                "corr": _r(t["corr"]),
                "tone_slope_db_s": _r(t["tone_slope"]),
                "floor_like": t["floor_like"],
            },
            "clear_partials_tail": _tail_clearance(w["held"], sr, f0, b),
            "hum_dbfs": None if not hum else _r(max(h["dbfs"] for h in hum)),
            "censors": {"body": None, "tnr": None},
        }
        # Body: the measured band under the note against the floor plus the hum in it.
        under = r.get("body_under_db")
        if under is not None and f0 >= BODY_MIN_F0_HZ:
            hi = 0.7 * f0
            fl = _power_in(F, edges, FLOOR_LO_HZ, hi) if censoring else 0.0
            hp = _line_power(hum, FLOOR_LO_HZ, hi)
            st["body_floor_db"] = _r(10 * np.log10(fl + 1e-300)) if censoring else None
            st["body_hum_db"] = _r(10 * np.log10(hp)) if hp > 0 else None
            if fl + hp > 0 and under <= 10 * np.log10(fl + hp) + BODY_FLOOR_MARGIN_DB:
                st["censors"]["body"] = CENSOR_FLOOR if fl >= hp else CENSOR_HUM
        # Tone-to-noise: the row's noise over its band against the floor plus the hum, both with the hum counted.
        if r.get("tnr_db") is not None and f0 > 0:
            fr, _mag = _spectrum(w["x"], sr)
            bin_hz = fr[1] - fr[0]
            topc = min(TNR_TOP_HZ, float(fr[-1]))
            tonal = _tnr_mask_exact(fr, f0, b, m, topc, bin_hz)
            lo_t = max(FLOOR_LO_HZ, 0.8 * f0)

            def covered(hz, fr=fr, tonal=tonal):
                return bool(tonal[int(np.argmin(np.abs(fr - hz)))])

            hp = _line_power(hum, lo_t, topc, covered)
            num = den = 0.0
            Ff = _filled(centres, F) if censoring else None
            for bi in range(nb):
                lo_b, hi_b = max(edges[bi], lo_t), min(edges[bi + 1], topc)
                if hi_b <= lo_b or not np.isfinite(D[i, bi]):
                    continue
                num += 10 ** (D[i, bi] / 10.0) * (hi_b - lo_b)
                if Ff is not None:
                    den += 10 ** (Ff[bi] / 10.0) * (hi_b - lo_b)
            num, den = num + hp, den + hp
            if den > 0 and num / den <= 10 ** (BODY_FLOOR_MARGIN_DB / 10.0):
                fl_part = den - hp
                st["censors"]["tnr"] = CENSOR_FLOOR if fl_part >= hp else CENSOR_HUM
        states.append(st)
    return rec, states


def record_floors(
    rows: list[dict], audios: list[tuple[np.ndarray, int]], *, preroll_s: float, gate_s: float
) -> dict:
    """Write `floor_state` into every pitched row and return the per-timbre floor records.

    `audios[i]` is `(audio, sr)` of `rows[i]`. A row not placed on a gate or with no
    fundamental takes no state. A declared capture names `clearance` as the source
    of the rows that have no outside-note floor of their own.
    """
    from profile_measure import (
        note_windows,  # profile_measure is the placer; it never imports this module
    )

    out: dict = {}
    for timbre in dict.fromkeys(r["timbre"] for r in rows):
        idx, wins = [], []
        for i, r in enumerate(rows):
            if r["timbre"] != timbre or not r.get("f0_hz"):
                continue
            audio, sr = audios[i]
            placed = note_windows(audio, sr, preroll_s, gate_s)
            if placed is None:
                continue
            _mono, held, a, b, _onset, _on, _off = placed
            idx.append(i)
            wins.append({"x": held[a:b], "held": held, "sr": sr})
        if len(idx) < 2:
            out[timbre] = {
                "declared": False,
                "rows": len(idx),
                "reason": "fewer than 2 measurable rows",
            }
            continue
        rec, states = declare([rows[i] for i in idx], wins)
        out[timbre] = rec
        for i, st in zip(idx, states):
            rows[i]["floor_state"] = st
            if rows[i].get("floor_source") is None and st["source"] is not None:
                rows[i]["floor_source"] = st["source"]
    return out
