"""Instrument-identity features, and how well each one tells two instruments apart.

The gated dimensions mostly measure a recording: a microphone is a linear filter
and a gain and a room is a floor, so anything compared ACROSS frequencies (band
shares, centroid, absolute partial levels) moves with the chain rather than the
instrument. What survives is a frequency itself, or one frequency compared with
itself across time or across velocity: string stiffness, how each partial
decays, the shape of a two-stage decay, how a partial group's share grows from
soft to hard. This module derives those from the row fields a profile already
holds, so no reference has to be re-measured to use them.

`grade` is the part that decides whether any of them is worth anything. For
each feature it measures the distance between two recordings of the SAME
instrument (a capture's second timbre, or the same instrument from a second
source) and between DIFFERENT instruments, and reports how often a different
instrument reads further away than a same-instrument pair (an AUC). A feature
whose same-instrument distances are as large as its different-instrument ones
carries no identity, whatever it measures.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import distance
from profile_measure import is_percussion

#: Partials read by number, from the fundamental up.
PARTIALS = 8
#: A cell on the other side is the same note at the nearest velocity within this.
VELOCITY_REACH = 24
#: Fewer shared values than this and a pair does not read a feature.
MIN_SHARED = 3
#: Partial groups whose share of h1 is compared from soft to hard.
GROUPS = {"h2-3": (2, 3), "h4-6": (4, 6), "h7-12": (7, 12)}


def _log2(v: float | None) -> float | None:
    return math.log2(v) if v is not None and v > 0.0 else None


def _slope(xs: list[float], ys: list[float]) -> float | None:
    if len(xs) < 3:
        return None
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return None if den == 0.0 else sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den


def _group_db(partials_db: list, lo: int, hi: int) -> float | None:
    """Power sum of partials lo..hi relative to h1, in dB."""
    if not partials_db or len(partials_db) < lo:
        return None
    vals = [p for p in partials_db[lo - 1 : hi] if p is not None and p > -119.0]
    if not vals or partials_db[0] is None:
        return None
    return 10.0 * math.log10(sum(10.0 ** (p / 10.0) for p in vals)) - partials_db[0]


def cell_features(row: dict) -> dict[str, float]:
    """The identity features one measured note carries on its own."""
    out: dict[str, float] = {}
    if row.get("inharmonicity_reliable"):
        b = _log2(row.get("inharmonicity_b"))
        if b is not None:
            out["log2B"] = b
    decays = row.get("partial_decay_db_s") or []
    ks, ds = [], []
    for k, d in enumerate(decays[:PARTIALS], start=1):
        if d is not None and math.isfinite(d):
            out[f"pdecay_h{k}"] = d
            ks.append(float(k))
            ds.append(d)
    trend = _slope(ks, ds)
    if trend is not None:
        out["pdecay_trend"] = trend
    for key, name in (("decay_knee_s", "knee_log2"), ("decay_span_s", "span_log2")):
        v = _log2(row.get(key))
        if v is not None:
            out[name] = v
    early, late = row.get("decay_early_db_s"), row.get("decay_late_db_s")
    if early is not None and late is not None:
        out["two_stage"] = early - late
    ladder = row.get("partials_db") or []
    pts = [
        (math.log2(k), p)
        for k, p in enumerate(ladder[:PARTIALS], start=1)
        if p is not None and p > -119.0
    ]
    tilt = _slope([x for x, _ in pts], [y for _, y in pts])
    if tilt is not None:
        mx = sum(x for x, _ in pts) / len(pts)
        my = sum(y for _, y in pts) / len(pts)
        for k, p in enumerate(ladder[:PARTIALS], start=1):
            if k > 1 and p is not None and p > -119.0:
                out[f"ladder_detilt_h{k}"] = p - (my + tilt * (math.log2(k) - mx))
    for key, name in (("attack_ms", "attack_log2"), ("onset_settle_ms", "settle_log2")):
        v = _log2(row.get(key))
        if v is not None:
            out[name] = v
    if row.get("onset_overshoot_db") is not None:
        out["overshoot"] = row["onset_overshoot_db"]
    if not row.get("damper_capped"):
        v = _log2(row.get("damper_release_ms"))
        if v is not None:
            out["release_log2"] = v
    return out


def note_swings(rows: list[dict]) -> dict[int, dict[str, float]]:
    """Soft-to-hard change per note: the hardest velocity minus the softest."""
    by_note: dict[int, list[dict]] = {}
    for r in rows:
        by_note.setdefault(r["note"], []).append(r)
    out: dict[int, dict[str, float]] = {}
    for note, cells in by_note.items():
        if len(cells) < 2:
            continue
        soft = min(cells, key=lambda r: r["velocity"])
        hard = max(cells, key=lambda r: r["velocity"])
        sw: dict[str, float] = {}
        for g, (lo, hi) in GROUPS.items():
            a, b = (
                _group_db(soft.get("partials_db"), lo, hi),
                _group_db(hard.get("partials_db"), lo, hi),
            )
            if a is not None and b is not None:
                sw[f"~{g}"] = b - a
        # One partial against h1 at two velocities: a filter and a gain cancel
        # exactly. A group's power sum cancels only to within its own spread.
        ls, lh = soft.get("partials_db") or [], hard.get("partials_db") or []
        for k in range(2, min(len(ls), len(lh), PARTIALS) + 1):
            a, b = ls[k - 1], lh[k - 1]
            if a is not None and b is not None and a > -119.0 and b > -119.0:
                sw[f"~h{k}"] = (b - lh[0]) - (a - ls[0])
        for key, name in (("decay_db_s", "~decay"), ("peak_dbfs", "~level")):
            if soft.get(key) is not None and hard.get(key) is not None:
                sw[name] = hard[key] - soft[key]
        a, b = _log2(soft.get("attack_ms")), _log2(hard.get("attack_ms"))
        if a is not None and b is not None:
            sw["~attack_log2"] = b - a
        if sw:
            out[note] = sw
    return out


def align(target: list[dict], rows: list[dict]) -> list[tuple[dict, dict]]:
    """Pairs of (target cell, the same note in @p rows at the nearest velocity)."""
    by: dict[int, list[dict]] = {}
    for r in rows:
        by.setdefault(r["note"], []).append(r)
    out = []
    for t in target:
        cands = by.get(t["note"])
        if not cands:
            continue
        best = min(cands, key=lambda r: abs(r["velocity"] - t["velocity"]))
        if abs(best["velocity"] - t["velocity"]) <= VELOCITY_REACH:
            out.append((t, best))
    return out


def _median(v: list[float]) -> float:
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def pair_distances(a: list[dict], b: list[dict], *, swings: bool = True) -> dict[str, float]:
    """Per feature, the median |a - b| over aligned cells (and shared notes for swings)."""
    deltas: dict[str, list[float]] = {}
    for x, y in align(a, b):
        fx, fy = cell_features(x), cell_features(y)
        for k in fx.keys() & fy.keys():
            deltas.setdefault(k, []).append(abs(fx[k] - fy[k]))
    if swings:
        sa, sb = note_swings(a), note_swings(b)
        for note in sa.keys() & sb.keys():
            for k in sa[note].keys() & sb[note].keys():
                deltas.setdefault(k, []).append(abs(sa[note][k] - sb[note][k]))
    return {k: _median(v) for k, v in deltas.items() if len(v) >= MIN_SHARED}


def auc(positives: list[float], negatives: list[float]) -> float | None:
    """P(a different-instrument distance exceeds a same-instrument one), ties half."""
    if not positives or not negatives:
        return None
    wins = 0.0
    for n in negatives:
        for p in positives:
            wins += 1.0 if n > p else 0.5 if n == p else 0.0
    return wins / (len(positives) * len(negatives))


#: Same-instrument pairs: (capture, timbre a, capture, timbre b, velocity features usable).
#: Harpsichord baroque / concert-8-4 / room read one sample set, so only gm007
#: against them is independent; p027's two timbres share one gain law.
POSITIVES = (
    ("piano", "grand-227", "piano", "grand-274", True),
    ("piano", "grand-227", "piano", "grand-290", True),
    ("piano", "grand-274", "piano", "grand-290", True),
    ("harpsichord", "gm007", "harpsichord", "baroque", True),
    ("harpsichord", "gm007", "harpsichord", "concert-8-4", True),
    ("harpsichord", "gm007", "harpsichord", "room", True),
    ("pipe_organ", "plenum-a", "pipe_organ", "plenum-b", True),
    ("electric_guitar_di", "di", "electric_guitar_di", "low_string", False),
    ("electric_grand", None, "electric_grand_module", None, True),
    ("orchestral_harp", None, "orchestral_harp_module", None, True),
)


def _rows(cap: str, timbre: str | None) -> list[dict]:
    prof = json.loads((distance.REFERENCE_DIR / f"{cap}.json").read_text())
    t = timbre or distance.timbre_for(cap)
    return [r for r in prof["rows"] if r.get("timbre") == t]


def grade() -> dict:
    """Each feature's same-instrument against different-instrument distances."""
    import crossid

    pos: dict[str, list[float]] = {}
    for ca, ta, cb, tb, vel_ok in POSITIVES:
        d = pair_distances(_rows(ca, ta), _rows(cb, tb), swings=vel_ok)
        for k, v in d.items():
            pos.setdefault(k, []).append(v)
    refs = {}
    for cap in distance.capture_ids():
        raw = json.loads((distance.CAPTURE_DIR / f"{cap}.json").read_text())
        if is_percussion(raw) or not (distance.REFERENCE_DIR / f"{cap}.json").exists():
            continue
        refs[cap] = _rows(cap, None)
    sib, fam = crossid.sibling_map(), crossid.family_map()
    across: dict[str, list[float]] = {}
    within: dict[str, list[float]] = {}
    caps = sorted(refs)
    for i, a in enumerate(caps):
        for b in caps[i + 1 :]:
            if b in sib.get(a, ()):
                continue
            bucket = within if b in fam.get(a, ()) else across
            for k, v in pair_distances(refs[a], refs[b]).items():
                bucket.setdefault(k, []).append(v)
    out = {}
    for k in sorted(set(pos) | set(across)):
        p, n, w = pos.get(k, []), across.get(k, []), within.get(k, [])
        out[k] = {
            "positives": len(p),
            "negatives_across": len(n),
            "negatives_family": len(w),
            "same_median": _median(p) if p else None,
            "across_median": _median(n) if n else None,
            "family_median": _median(w) if w else None,
            "auc_across": auc(p, n),
            "auc_family": auc(p, w),
        }
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", default="", dest="json_out")
    args = parser.parse_args(argv)
    result = grade()
    rows = sorted(result.items(), key=lambda kv: -(kv[1]["auc_across"] or -1.0))
    print(
        f"{'feature':22s} {'pos':>4s} {'neg':>6s} {'same':>8s} {'family':>8s} {'across':>8s} {'auc_fam':>8s} {'auc_acr':>8s}"
    )
    for k, r in rows:
        f = lambda v: "-" if v is None else f"{v:8.3f}"
        print(
            f"{k:22s} {r['positives']:4d} {r['negatives_across']:6d} {f(r['same_median'])} "
            f"{f(r['family_median'])} {f(r['across_median'])} {f(r['auc_family'])} {f(r['auc_across'])}"
        )
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
