"""What happened to every cell the loss aggregates, across a set of rendered probes.

Half the loss terms are sums over a ladder, a band profile or a slice grid, and
the raw value of one cannot say what its cells were. Four outcomes arrive as
three different numbers and two of them are not measurements at all:

    compared    both sides had a value, inside the cap — the only measurement
    clipped     both sides had a value, at or past the cap
    absent      the reference had one, the model did not; the cap stands in
    skipped     the reference had none; nothing is charged, so the cell scores
                0.0, which is the term's BEST

The two failures point opposite ways, which is why neither shows up in a
results table. A term made of caps reads as its WORST and no empty-set guard
looks for it. A term made of skips reads as its best and every guard that does
look is satisfied — `tail` scored a perfect 0.0 on the whole bank because its
band opens at 2.0 s and the default probe holds 2.0 s.

Read-only: reads rendered probe directories and nothing else, writes nothing
under `reference/` or `capture/`, and exits 0 whatever it finds. Not wired into
CI — this is an instrument, and a gate built out of it would be a gate on how
short the probes happen to be.

    python loss_cells.py [--probes DIR] [--run NAME,NAME] [--json PATH]
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from loss import (
    LOSS_TERMS,
    SKELETON_MAX_S,
    band_min_note_s,
    cli_weights,
    mss_distance,
    probe_rows,
    score_terms,
)
from metrics import normalize_rms, to_mono
from patterns import (
    drum_pattern,
    pattern_length,
    sustain_pattern,
    velocity_pattern,
)
from wavio import read_wav

#: The four outcomes, in the order a reader wants them: what was measured
#: first, then the two that were not, then the free one.
OUTCOMES = ("compared", "clipped", "absent", "skipped")

#: Where a rendered model/oracle pair lives by default. One directory per probe,
#: each holding the `report.json` that says which pattern and program it was and
#: the two renders it produced.
DEFAULT_PROBES = Path(__file__).resolve().parent / "out"


def rebuild_pattern(report: dict, rows: list[dict], wav_s: float):
    """The pattern a cached probe was rendered from, or None if it cannot be.

    Rebuilt from the notes the report recorded rather than from the pattern's
    own defaults, because a run commonly narrows the grid — and a pattern that
    disagrees with the render puts every analysis window on the wrong note,
    which is a silent wrong answer rather than an error. `main` checks the
    rebuild against the report's own f0 before believing anything measured
    through it.
    """
    pitches = [r.get("note") for r in rows]
    velocities = [r.get("velocity") for r in rows]
    if any(p is None or v is None for p, v in zip(pitches, velocities)):
        return None
    name, program = report["pattern"], report["program"]
    if name == "sustain":
        return sustain_pattern(program, notes=tuple(pitches), velocity=velocities[0])
    if name == "velocity":
        return velocity_pattern(program, note=pitches[0], velocities=tuple(velocities))
    if name == "drum":
        pattern = drum_pattern(program, notes=(pitches[0],), velocities=tuple(velocities))
        if pattern_length(pattern) > wav_s + 0.05:
            # The long-decay gap postdates some of these renders, so a pattern
            # longer than the WAV is the gap rather than the note list.
            pattern = drum_pattern(
                program, notes=(pitches[0],), velocities=tuple(velocities), gap=2.0
            )
        return pattern
    return None


def census(terms: dict[str, float]) -> dict[str, Counter]:
    """The four-state tally each capped aggregate reported, by term."""
    out: dict[str, Counter] = {}
    for term in LOSS_TERMS:
        cells = terms.get(f"{term}_cells")
        if cells is None:
            continue
        capped = terms.get(f"{term}_capped", 0.0)
        absent = terms.get(f"{term}_absent", 0.0)
        out[term] = Counter(
            {
                "compared": int(cells - capped),
                "clipped": int(capped - absent),
                "absent": int(absent),
                "skipped": int(terms.get(f"{term}_skipped", 0.0)),
            }
        )
    return out


def resolved_weights(report: dict, rows: list[dict], pattern) -> dict[str, float]:
    """The weights a fit on this probe would actually resolve to.

    Read through `cli_weights` with the probe-shape flags rather than off the
    instrument's class directly, because those flags are what drop a class
    default the probe cannot fit. Asking the class alone would mark `tail`
    weighted on the very probes the gate exists to take it off.
    """
    velocities: dict[int, set[int]] = {}
    for note in pattern.analysis_notes:
        velocities.setdefault(note.note, set()).add(note.velocity)
    tail_min = band_min_note_s("tail_db_s")
    args = SimpleNamespace(
        program=report["program"],
        percussive=pattern.percussive,
        drum_note=rows[0]["note"] if pattern.percussive else None,
        has_analysis_notes=bool(pattern.analysis_notes),
        has_kit_groups=False,
        has_velocity_spread=any(len(v) >= 2 for v in velocities.values()),
        has_tail_window=any(min(n.dur, SKELETON_MAX_S) >= tail_min for n in pattern.analysis_notes),
        **{f"w_{term}": None for term in LOSS_TERMS},
    )
    return cli_weights(args)


def measure(directory: Path) -> dict | None:
    """Score one rendered probe directory and return its census, or None."""
    report = json.loads((directory / "report.json").read_text())
    rows = [n["model"] for n in (report.get("notes") or [])]
    if not rows:
        return {"skip": "report carries no notes"}
    try:
        model_wav, sr = read_wav(directory / "model.wav")
        oracle_wav, oracle_sr = read_wav(directory / "oracle.wav")
    except Exception as exc:  # noqa: BLE001
        return {"skip": f"render missing: {exc}"}
    if sr != oracle_sr:
        return {"skip": "the two renders disagree about the sample rate"}
    model_raw, oracle_raw = to_mono(model_wav), to_mono(oracle_wav)
    frames = min(len(model_raw), len(oracle_raw))
    model_raw, oracle_raw = model_raw[:frames], oracle_raw[:frames]
    pattern = rebuild_pattern(report, rows, frames / sr)
    if pattern is None:
        return {"skip": f"pattern {report['pattern']!r} cannot be rebuilt"}
    if len(pattern.analysis_notes) != len(rows):
        return {"skip": "the rebuilt pattern and the report disagree on note count"}
    model, oracle = normalize_rms(model_raw), normalize_rms(oracle_raw)
    model_rows = probe_rows(model, pattern, sr, raw=model_raw, threads=2)
    oracle_rows = probe_rows(oracle, pattern, sr, raw=oracle_raw, threads=2)
    # Did the rebuilt pattern land on the notes the render actually holds? The
    # report was measured through the same windows, so its f0 is the check: a
    # window off by one note moves it by hundreds of cents.
    drift = [
        abs(1200.0 * np.log2(got["f0_hz"] / want["f0_hz"]))
        for got, want in zip(model_rows, rows)
        if got.get("f0_hz") and want.get("f0_hz")
    ]
    if drift and max(drift) > 50.0:
        return {"skip": f"rebuilt windows are {max(drift):.0f} cents off the report"}
    terms = score_terms(
        model_rows, oracle_rows, mss=mss_distance(model, oracle), percussive=pattern.percussive
    )
    if terms is None:
        return {"skip": "the pair does not score (a render produced no partials)"}
    return {
        "gm_name": report.get("gm_name", ""),
        "pattern": report["pattern"],
        "notes": len(model_rows),
        "weighted": sorted(resolved_weights(report, rows, pattern)),
        "cells": {t: dict(c) for t, c in census(terms).items()},
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--probes", default=str(DEFAULT_PROBES), help="directory of rendered probe directories"
    )
    ap.add_argument("--run", default="", help="comma-separated probe names")
    ap.add_argument("--json", default="", help="write the full census here")
    args = ap.parse_args(argv)

    wanted = {r for r in args.run.split(",") if r}
    results, skipped = {}, []
    for report in sorted(Path(args.probes).glob("*/report.json")):
        name = report.parent.name
        if wanted and name not in wanted:
            continue
        got = measure(report.parent)
        if got is None or "skip" in got:
            skipped.append((name, got["skip"] if got else "unreadable"))
            continue
        results[name] = got

    grand: dict[str, Counter] = {}
    for got in results.values():
        for term, counts in got["cells"].items():
            grand.setdefault(term, Counter()).update(counts)
    cells = sum(sum(c.values()) for c in grand.values())
    # Reach first and unmissable: a run that classified nothing must not read
    # like a run that found nothing wrong.
    print(
        f"probes scored: {len(results)}   skipped: {len(skipped)}   "
        f"notes: {sum(g['notes'] for g in results.values())}   cells: {cells}"
    )
    for name, why in skipped:
        print(f"  skip {name}: {why}")
    if not cells:
        print("NOTHING WAS CLASSIFIED — this is not a clean result, it is no result")
        return 0

    print(
        f"\n{'term':8s} {'cells':>7s} "
        + " ".join(f"{o:>9s}" for o in OUTCOMES)
        + f" {'capped':>8s} {'skipped':>8s}"
    )
    for term in sorted(grand, key=lambda t: -sum(grand[t].values())):
        counts = grand[term]
        total = sum(counts.values())
        capped = counts["clipped"] + counts["absent"]
        measurable = total - counts["skipped"]
        print(
            f"{term:8s} {total:7d} "
            + " ".join(f"{counts[o]:9d}" for o in OUTCOMES)
            + f" {100.0 * capped / measurable if measurable else 0.0:7.1f}%"
            + f" {100.0 * counts['skipped'] / total:7.1f}%"
        )

    # A term with no comparison behind it, per probe. Both ends are reported
    # together because they are the same blindness: the number is not a
    # measurement, and which direction it lies in depends only on which of the
    # two reasons it had.
    print(
        "\nterms with no comparison behind them, by probe "
        "(w = a fit on this probe would weight it):"
    )
    found = 0
    for name, got in sorted(results.items()):
        for term, counts in sorted(got["cells"].items()):
            total = sum(counts.values())
            if not total or counts["compared"]:
                continue
            found += 1
            mark = "w" if term in got["weighted"] else " "
            reason = (
                "the reference offered no cell"
                if counts["skipped"] == total
                else "every cell is a cap"
            )
            print(
                f"  {mark} {name:16s} {got['gm_name'][:22]:22s} {term:7s} "
                f"{total:4d} cells — {reason}"
            )
    print(f"  {found} (probe, term) pairs" if found else "  none")
    if args.json:
        Path(args.json).write_text(
            json.dumps(
                {
                    "probes": results,
                    "skipped": skipped,
                    "totals": {t: dict(c) for t, c in grand.items()},
                },
                indent=1,
            )
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
