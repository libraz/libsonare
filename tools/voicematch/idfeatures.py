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


def _nearest_note(note: int, notes, reach: int | None) -> int | None:
    """The note of @p notes nearest @p note, within @p reach semitones (None: any)."""
    if not notes:
        return None
    best = min(notes, key=lambda n: (abs(n - note), n))
    return best if reach is None or abs(best - note) <= reach else None


def align(
    target: list[dict], rows: list[dict], *, note_reach: int | None = 0
) -> list[tuple[dict, dict]]:
    """Pairs of (target cell, the nearest note in @p rows at the nearest velocity)."""
    by: dict[int, list[dict]] = {}
    for r in rows:
        by.setdefault(r["note"], []).append(r)
    out = []
    for t in target:
        note = _nearest_note(t["note"], by.keys(), note_reach)
        if note is None:
            continue
        best = min(by[note], key=lambda r: abs(r["velocity"] - t["velocity"]))
        if abs(best["velocity"] - t["velocity"]) <= VELOCITY_REACH:
            out.append((t, best))
    return out


def _median(v: list[float]) -> float:
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def pair_distances(
    a: list[dict], b: list[dict], *, swings: bool = True, note_reach: int | None = 0
) -> dict[str, float]:
    """Per feature, the median |a - b| over aligned cells (and aligned notes for swings)."""
    deltas: dict[str, list[float]] = {}
    for x, y in align(a, b, note_reach=note_reach):
        fx, fy = cell_features(x), cell_features(y)
        for k in fx.keys() & fy.keys():
            deltas.setdefault(k, []).append(abs(fx[k] - fy[k]))
    if swings:
        sa, sb = note_swings(a), note_swings(b)
        for note, swa in sa.items():
            other = _nearest_note(note, sb.keys(), note_reach)
            if other is None:
                continue
            for k in swa.keys() & sb[other].keys():
                deltas.setdefault(k, []).append(abs(swa[k] - sb[other][k]))
    return {k: _median(v) for k, v in deltas.items() if len(v) >= MIN_SHARED}


#: How a capture is cut in two for a split-half positive. Alternation keeps both
#: halves on the same register; the register cut does not, so a feature that only
#: interpolates across the grid passes alternation and fails the register cut.
SPLITS = ("note", "velocity", "register")


def split_halves(rows: list[dict], by: str) -> tuple[list[dict], list[dict]]:
    """One capture's rows as two stand-in instruments."""
    notes = sorted({r["note"] for r in rows})
    if by == "note":
        odd = {n for i, n in enumerate(notes) if i % 2}
        return [r for r in rows if r["note"] not in odd], [r for r in rows if r["note"] in odd]
    if by == "register":
        low = set(notes[: len(notes) // 2])
        return [r for r in rows if r["note"] in low], [r for r in rows if r["note"] not in low]
    if by == "velocity":
        a, b = [], []
        for n in notes:
            cells = sorted((r for r in rows if r["note"] == n), key=lambda r: r["velocity"])
            a += cells[0::2]
            b += cells[1::2]
        return a, b
    raise ValueError(f"unknown split {by!r}")


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
    refs = _references()
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


def _references() -> dict[str, list[dict]]:
    refs = {}
    for cap in distance.capture_ids():
        raw = json.loads((distance.CAPTURE_DIR / f"{cap}.json").read_text())
        if is_percussion(raw) or not (distance.REFERENCE_DIR / f"{cap}.json").exists():
            continue
        refs[cap] = _rows(cap, None)
    return refs


def grade_split(by: str, refs: dict[str, list[dict]] | None = None) -> dict:
    """Per feature: does each capture's half stand nearer its own other half than any other's?

    The positive is a capture's two halves; the negatives are its first half against
    every other capture's second half, siblings excluded. `hit_rate` is the share of
    captures whose own other half is strictly nearest on that feature alone.
    """
    import crossid

    refs = _references() if refs is None else refs
    sib = crossid.sibling_map()
    halves = {c: split_halves(r, by) for c, r in refs.items()}
    halves = {c: h for c, h in halves.items() if h[0] and h[1]}
    pos: dict[str, list[float]] = {}
    neg: dict[str, list[float]] = {}
    hits: dict[str, list[bool]] = {}
    pairs: dict[str, tuple[dict, dict[str, dict]]] = {}
    for c, (a, b) in halves.items():
        own = pair_distances(a, b, note_reach=None)
        rival_pairs = {
            d: pair_distances(a, bd, note_reach=None)
            for d, (_, bd) in halves.items()
            if d != c and d not in sib.get(c, ())
        }
        pairs[c] = (own, rival_pairs)
        rivals: dict[str, list[float]] = {}
        for dist in rival_pairs.values():
            for k, v in dist.items():
                rivals.setdefault(k, []).append(v)
        for k, v in own.items():
            if not rivals.get(k):
                continue
            pos.setdefault(k, []).append(v)
            neg.setdefault(k, []).extend(rivals[k])
            hits.setdefault(k, []).append(v < min(rivals[k]))
    out = {
        k: {
            "captures": len(pos[k]),
            "same_median": _median(pos[k]),
            "across_median": _median(neg[k]),
            "auc": auc(pos[k], neg[k]),
            "hit_rate": sum(hits[k]) / len(hits[k]),
        }
        for k in sorted(pos)
    }
    scale = {k: out[k]["across_median"] for k in out if out[k]["across_median"] > 0.0}
    out[COMBINED] = combined_hits(pairs, scale)
    return out


def grade_twins(refs: dict[str, list[dict]] | None = None) -> dict:
    """The combined rank of each known same-instrument twin among every other capture.

    The harder test beside the split-half one: a twin is a second recording, so it
    differs in chain and room as well as in take, and only identity survives that.
    """
    import crossid

    refs = _references() if refs is None else refs
    sib = crossid.sibling_map()
    pairs: dict[str, tuple[dict, dict[str, dict]]] = {}
    for ca, ta, cb, tb, vel_ok in POSITIVES:
        a = _rows(ca, ta)
        own = pair_distances(a, _rows(cb, tb), swings=vel_ok, note_reach=None)
        rivals = {
            d: pair_distances(a, rows, swings=vel_ok, note_reach=None)
            for d, rows in refs.items()
            if d not in (ca, cb) and d not in sib.get(ca, ()) and d not in sib.get(cb, ())
        }
        pairs[f"{ca}:{ta}~{cb}:{tb}"] = (own, rivals)
    pooled: dict[str, list[float]] = {}
    for _, rivals in pairs.values():
        for dist in rivals.values():
            for k, v in dist.items():
                pooled.setdefault(k, []).append(v)
    scale = {k: _median(v) for k, v in pooled.items() if _median(v) > 0.0}
    per = {name: combined_hits({name: pr}, scale) for name, pr in pairs.items()}
    return {"pairs": per, COMBINED: combined_hits(pairs, scale)}


#: The row of a split grade that reads every feature at once.
COMBINED = "_combined"
#: Fewer features than this in common and a pair is not compared on the combination.
MIN_COMBINED_FEATURES = 5


def combined_distance(dist: dict[str, float], scale: dict[str, float], keys) -> float | None:
    """Median over @p keys of a feature distance in units of its typical across distance."""
    v = [dist[k] / scale[k] for k in keys if k in dist and k in scale]
    return _median(v) if len(v) >= MIN_COMBINED_FEATURES else None


def combined_hits(pairs: dict, scale: dict[str, float]) -> dict:
    """Rank of each capture's own other half among all rivals, every feature combined.

    Own and rival are read on the features both have, so a rival cannot win by
    lacking the one feature the own half is far on (the crossid contest rule).
    """
    ranks: list[int] = []
    pos: list[float] = []
    neg: list[float] = []
    for own, rivals in pairs.values():
        better, judged = 0, 0
        for dist in rivals.values():
            keys = own.keys() & dist.keys()
            o, r = combined_distance(own, scale, keys), combined_distance(dist, scale, keys)
            if o is None or r is None:
                continue
            judged += 1
            pos.append(o)
            neg.append(r)
            better += r <= o
        if judged:
            ranks.append(better + 1)
    n = len(ranks)
    return {
        "captures": n,
        "same_median": _median(pos) if pos else None,
        "across_median": _median(neg) if neg else None,
        "auc": auc(pos, neg) if pos else None,
        "hit_rate": sum(r == 1 for r in ranks) / n if n else None,
        "top5_rate": sum(r <= 5 for r in ranks) / n if n else None,
        "median_rank": _median([float(r) for r in ranks]) if n else None,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", default="", dest="json_out")
    parser.add_argument("--split", choices=SPLITS, default="", help="grade on split-half positives")
    parser.add_argument("--twins", action="store_true", help="rank the known twins, combined")
    args = parser.parse_args(argv)
    if args.twins:
        result = grade_twins()
        for name, r in result["pairs"].items():
            print(f"{name:55s} rank {r['median_rank']}")
        print(f"combined: {result[COMBINED]}")
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(result, indent=1) + "\n")
        return 0
    if args.split:
        result = grade_split(args.split)
        print(f"{'feature':22s} {'caps':>5s} {'same':>8s} {'across':>8s} {'auc':>6s} {'hit':>6s}")
        comb = result.pop(COMBINED)
        print(
            f"combined: {comb['captures']} captures, hit {comb['hit_rate']}, top5 "
            f"{comb['top5_rate']}, median rank {comb['median_rank']}, auc {comb['auc']}"
        )
        for k, r in sorted(result.items(), key=lambda kv: -kv[1]["auc"]):
            print(
                f"{k:22s} {r['captures']:5d} {r['same_median']:8.3f} {r['across_median']:8.3f} "
                f"{r['auc']:6.3f} {r['hit_rate']:6.3f}"
            )
        if args.json_out:
            Path(args.json_out).write_text(json.dumps({**result, COMBINED: comb}, indent=1) + "\n")
        return 0
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
