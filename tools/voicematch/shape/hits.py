"""One row per strike of a phrase take, so a listening note can name a hit.

`takes` measures a phrase as a whole, which is the right shape for what a phrase
adds and the wrong shape for a complaint. A listener hears "the second one is
wrong" and the take-level table has no second one in it, so the hit has to be
recovered by reading the phrase set and counting -- by hand, by whoever happens
to know where the schedule lives. That is a defect in this harness rather than in
the description: the audition manifest already carries every strike's note,
velocity and time, and nothing was reading them.

Each hit gets an id, `<take>#<n>`, printed beside the note it plays and the URL
that sounds it. That id is the vocabulary a listening note should use, and it is
also what this module measures: the window from one strike to the next, on both
sides, after removing the take's shared gain so what is left is tilt rather than
loudness.

Two things it refuses to report rather than guess. Strikes closer together than
`FUSED_S` are one audible event and are named as one row, because no window
separates them -- a groove's kick and hat land on the same beat and a per-hit
number for either would be the pair. And a band that does not clear the source's
own floor is left blank, per source, so a table cell is never a reading of noise.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

from .takes import (BANDS, SNR_DB, band_db, density_db, load, noise_db, rms_db,
                    window_for)

#: Strikes within this of each other are one event. A drummer's flam is about
#: 30 ms and reads as one thickened stroke rather than as two notes, so the line
#: is drawn above it: anything this close shares a window and cannot be given a
#: number of its own.
FUSED_S = 0.035

#: Longest window a single hit is measured over. A let-ring cymbal would
#: otherwise take the rest of the take, which makes its row a restatement of the
#: whole-take table it is meant to break down.
MAX_WINDOW_S = 2.5

#: Shortest one worth a reading. Below this the band FFT has too few samples at
#: the low end to mean anything, and `band_db` already returns its sentinel.
MIN_WINDOW_S = 0.08


def hit_rows(item) -> list[dict]:
    """The take's schedule as numbered rows, simultaneous strikes fused.

    Sorted by time, which the manifest is not: `make_audition` writes a take's
    voices in the order the phrase builder appended them, so a groove's hats
    arrive before its kick regardless of when either sounds. Numbering that
    order would give the listener's "second one" a different answer from the
    ear's.
    """
    notes = sorted(item["meta"]["notes"], key=lambda n: (n["start"], n["note"]))
    rows: list[dict] = []
    for n in notes:
        if rows and n["start"] - rows[-1]["start"] <= FUSED_S:
            rows[-1]["notes"].append(n)
            continue
        rows.append({"start": float(n["start"]), "notes": [n]})
    for i, r in enumerate(rows, 1):
        r["n"] = i
    return rows


def hit_windows(rows, total: float) -> None:
    """Give each row the span from its own strike to the next one.

    Capped at `MAX_WINDOW_S`, and the last row is capped the same way rather
    than running to the end of the file, so every row is a comparable length of
    the same instrument's decay and not a mixture of that and how long the
    renderer happened to pad.
    """
    for i, r in enumerate(rows):
        nxt = rows[i + 1]["start"] if i + 1 < len(rows) else total
        r["window"] = (r["start"], min(nxt, r["start"] + MAX_WINDOW_S, total))


def onset_shift(x, sr, nominal: float, search: float = 0.25) -> float:
    """How far this source's first strike sits from where the schedule put it.

    A plugin renders with its own latency and the model does not, and the two
    are compared hit by hit here, so an uncorrected offset moves every window
    across the strike it is meant to contain. Measured once per source from the
    first strike -- the rest of the phrase is scheduled from the same clock, so
    one offset places all of them.

    Taken as the first sample within the search span to reach 20 dB under that
    span's peak, which is a strike rather than a threshold crossing of the tail
    that preceded it.
    """
    lo = max(int((nominal - search) * sr), 0)
    hi = min(int((nominal + search) * sr), len(x))
    if hi - lo < 32:
        return 0.0
    seg = np.abs(np.asarray(x[lo:hi], dtype=np.float64))
    peak = float(seg.max())
    if peak <= 0.0:
        return 0.0
    idx = int(np.argmax(seg >= peak * 0.1))
    return (lo + idx) / sr - nominal


def shared_gain_db(tracks, source: str, reference: str, window) -> float:
    """One gain per take, so a per-hit table reads tilt and not loudness.

    The same choice `takes.band_error` makes and for the same reason, taken over
    the same body window so the two tables can be read against each other.
    """
    x, sr = tracks[source]
    r, rsr = tracks[reference]
    return rms_db(r, rsr, window) - rms_db(x, sr, window)


def measure_hit(tracks, rows, shifts, floors, source: str) -> None:
    """Fill in one source's peak and per-band levels for every row."""
    x, sr = tracks[source]
    shift = shifts[source]
    for r in rows:
        lo, hi = r["window"][0] + shift, r["window"][1] + shift
        if hi - lo < MIN_WINDOW_S:
            r.setdefault("miss", set()).add(source)
            continue
        seg = np.abs(np.asarray(x[int(lo * sr):int(hi * sr)], dtype=np.float64))
        r.setdefault("peak", {})[source] = (
            20 * np.log10(max(float(seg.max()), 1e-30)) if len(seg) else -300.0)
        floor = noise_db(x, sr, (lo, hi), floors[source])
        band = {}
        for b in BANDS:
            if density_db(x, sr, b, (lo, hi)) - floor < SNR_DB:
                continue
            band[b] = band_db(x, sr, b, (lo, hi))
        r.setdefault("band", {})[source] = band


def describe(row, name) -> str:
    """The notes a row plays, as the ear meets them."""
    return " + ".join(f"{n['note']} {name(n['note'])} v{n['velocity']}"
                      for n in row["notes"])


def report(page, item, rows, source: str, reference: str, gain_db: float,
           name) -> str:
    """The per-hit table for one take, ids first."""
    out = [f"== {item['id']}: {item['label']}",
           f"   {page}{item['id']}/{source}   vs   {page}{item['id']}/{reference}",
           f"   one gain of {gain_db:+.1f} dB applied to {source} over the take body"]
    head = "".join(f"{lo // 1000 if lo >= 1000 else lo}{'k' if lo >= 1000 else ''}"
                   .rjust(7) for lo, _ in BANDS)
    tag_w = max(len(item["id"]) + 4, 8)
    out.append(f"   {'hit':<{tag_w}}{'plays':<44}{'win':>6}{'peak':>7}{head}")
    for r in rows:
        tag = f"{item['id']}#{r['n']}"
        win = r["window"][1] - r["window"][0]
        note = describe(r, name)
        if len(r["notes"]) > 1:
            note += "  [fused]"
        # Truncated rather than allowed to push the numbers out of their
        # columns: a table whose columns move with the longest label is read
        # wrong, and the hit id beside it already names the row without help.
        note = note if len(note) <= 43 else note[:42] + "…"
        peak = r.get("peak", {})
        cells = ""
        for b in BANDS:
            m = r.get("band", {}).get(source, {}).get(b)
            k = r.get("band", {}).get(reference, {}).get(b)
            cells += ("--" if m is None or k is None
                      else f"{m + gain_db - k:+.1f}").rjust(7)
        pk = ("--" if source not in peak or reference not in peak
              else f"{peak[source] + gain_db - peak[reference]:+.1f}")
        out.append(f"   {tag:<{tag_w}}{note:<44}{win:>5.2f}s{pk:>7}{cells}")
    out.append(f"   cells are {source} - {reference} in dB after that gain; "
               "-- is a band under one side's own floor; [fused] rows play "
               "several notes at once and cannot be separated")
    return "\n".join(out)


def run(directory, page_url: str = "", reference: str = "", source: str = "model",
        only: tuple[str, ...] = ()) -> str:
    """Every take of an audition page, one row per strike."""
    from gm_names import drum_name, gm_name  # noqa: F401  (drum_name is the one used)

    data = load(directory, reference)
    man = data["_manifest"]
    ref = data["_reference"]
    if ref is None:
        raise ValueError(f"{directory} has no reference source to compare against")
    blocks = []
    for item in man["items"]:
        if only and item["id"] not in only:
            continue
        tracks = data.get(item["id"])
        if not tracks or source not in tracks or ref not in tracks:
            continue
        rows = hit_rows(item)
        hit_windows(rows, float(item["meta"]["seconds"]))
        first = rows[0]["start"]
        floors = {s: window_for(item, "floor") for s in (source, ref)}
        shifts = {s: onset_shift(*tracks[s], first) for s in (source, ref)}
        body = window_for(item, "body")
        gain = shared_gain_db(tracks, source, ref, body)
        for s in (source, ref):
            measure_hit(tracks, rows, shifts, floors, s)
        blocks.append(report(page_url, item, rows, source, ref, gain, drum_name))
    return "\n\n".join(blocks)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="shape.hits",
        description="Break an audition page down one strike at a time, so a "
                    "listening note can name a hit instead of counting.")
    ap.add_argument("--page", required=True,
                    help="audition directory holding manifest.json and the takes")
    ap.add_argument("--url", default="http://127.0.0.1:8730/",
                    help="base of the audition server, so every row prints a "
                         "link that sounds exactly it")
    ap.add_argument("--source", default="model", help="the side under test")
    ap.add_argument("--reference", default="",
                    help="source key everything is measured against")
    ap.add_argument("--only", default="", help="comma-separated take ids")
    a = ap.parse_args(argv)
    # The set id is the directory's name, which is what `serve.py` publishes it
    # as and therefore what the page's own address uses. The manifest does not
    # carry one, and inventing a field for it here would give the two ends two
    # names for one set.
    d = Path(a.page)
    base = a.url.rstrip("/") + "/#" + d.name + "/"
    only = tuple(t for t in a.only.split(",") if t)
    print(run(d, base, a.reference, a.source, only))
    return 0


if __name__ == "__main__":
    sys.exit(main())
