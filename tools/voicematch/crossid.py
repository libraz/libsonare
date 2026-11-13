"""Cross-identification: is each model nearer its own reference than any other instrument's?

A gate is a ratchet on today's model and a distance table says how far a voice
is from its oracle; neither can say the voice is the wrong instrument, because
both only ever look at the one reference the capture names. This reads the
model against EVERY reference that shares grid cells with it and asks two
questions:

  - rank: is the capture's own reference the nearest one? A model nearer some
    other program's reference than its own is measured as that other thing.
  - neighbourhood: is the model nearer its own reference than that reference is
    to the nearest different instrument's? A model further out than the nearest
    neighbour has left the region its reference stands for, whatever its rank.

Every pair goes through one delta function (`profile_gate.reference_spread`, the
one the compare table's spread uses) and one ruler (each dimension divided by
its fallback floor, never a capture's own spread, so all candidates of a model
are measured alike). A candidate counts only where it shares enough cells and
enough of the dimensions the model-to-own comparison measured.

Captures of the same patch (a module twin, a wide variant's capital) are the
same instrument and are not rivals. Captures in the same GM family of eight
are rivals of a weaker kind: losing to a bright piano is a nuance of a grand,
losing to a string section is the wrong instrument, so `rank` and the
neighbourhood count other families only and `family_rank` reports the rest.

Pitched captures only; a kit's notes are instruments and need a per-note pass.
Model rows come from `distance.py run`, which writes them beside each compare.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import distance
from profile_gate import dimension_floor, reference_spread
from profile_measure import is_percussion

#: Fewer shared (note, velocity) cells than this and a pair is not compared.
MIN_SHARED_CELLS = 4
#: Fewer dimensions measurable on both sides of a contest and it is not held.
MIN_DIMENSIONS = 5


def rows_for(profile: dict, timbre: str | None) -> list[dict]:
    rows = profile.get("rows", [])
    present = sorted({r["timbre"] for r in rows})
    chosen = timbre if timbre in present else (present[0] if present else None)
    return [r for r in rows if r["timbre"] == chosen]


def _cells(rows: list[dict]) -> set[tuple[int, int]]:
    return {(r["note"], r["velocity"]) for r in rows}


def pair(a_rows: list[dict], b_rows: list[dict], cells: set | None = None) -> dict[str, float]:
    """Per-dimension median |a - b| over the shared cells (restricted to @p cells if given)."""
    shared = _cells(a_rows) & _cells(b_rows)
    if cells is not None:
        shared &= cells
    if len(shared) < MIN_SHARED_CELLS:
        return {}
    pooled = [{**r, "timbre": "a"} for r in a_rows if (r["note"], r["velocity"]) in shared] + [
        {**r, "timbre": "b"} for r in b_rows if (r["note"], r["velocity"]) in shared
    ]
    return reference_spread({"rows": pooled})


def floors(spread: dict[str, float], dims: set[str]) -> dict[str, float]:
    return {k: spread[k] / dimension_floor(k)[0] for k in dims if k in spread}


def median(values: list[float]) -> float | None:
    if not values:
        return None
    v = sorted(values)
    n = len(v)
    return v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])


def contest(model: list[dict], own: list[dict], rival: list[dict]) -> dict | None:
    """Model-to-own against model-to-rival, on the SAME cells and the SAME dimensions.

    A dimension one side cannot measure leaves both, or a rival lacking the
    model's worst dimension would win by its absence. The median over
    dimensions, in floors, keeps one recording-dependent dimension (a release
    tail, a microphone's brightness) from deciding identity alone.
    """
    cells = _cells(model) & _cells(own) & _cells(rival)
    if len(cells) < MIN_SHARED_CELLS:
        return None
    s_own, s_rival = pair(model, own, cells), pair(model, rival, cells)
    dims = set(s_own) & set(s_rival)
    if len(dims) < MIN_DIMENSIONS:
        return None
    f_own, f_rival = floors(s_own, dims), floors(s_rival, dims)
    return {
        "cells": len(cells),
        "dimensions": len(dims),
        "own": median(list(f_own.values())),
        "rival": median(list(f_rival.values())),
        "won": sum(1 for k in dims if f_own[k] <= f_rival[k]),
    }


def identify(
    model_rows: list[dict],
    own: str,
    references: dict[str, list[dict]],
    siblings: set[str],
    family: set[str] = frozenset(),
) -> dict:
    """Rank @p own among every comparable reference for one model."""
    full = pair(model_rows, references[own])
    d_own = median(list(floors(full, set(full)).values()))
    contests = []
    for cap, rows in references.items():
        if cap == own or cap in siblings:
            continue
        c = contest(model_rows, references[own], rows)
        if c is not None:
            contests.append({"capture": cap, "family": cap in family, **c})
    beaten = sorted(
        (c for c in contests if c["rival"] < c["own"]), key=lambda c: c["rival"] - c["own"]
    )
    # Losing to another member of the same family (a bright piano for a grand)
    # is a nuance; losing to another family is the wrong instrument.
    losses = [c for c in beaten if not c["family"]]
    nearest = min(
        (c for c in contests if not c["family"]), key=lambda c: c["rival"] - c["own"], default=None
    )
    # The neighbourhood: is the model further from its reference than the
    # nearest different instrument's reference is, on the same cells and dimensions?
    outside = []
    for cap, rows in references.items():
        if cap == own or cap in siblings or cap in family:
            continue
        cells = _cells(model_rows) & _cells(references[own]) & _cells(rows)
        if len(cells) < MIN_SHARED_CELLS:
            continue
        s_model, s_nn = pair(model_rows, references[own], cells), pair(references[own], rows, cells)
        dims = set(s_model) & set(s_nn)
        if len(dims) < MIN_DIMENSIONS:
            continue
        dm = median(list(floors(s_model, dims).values()))
        dn = median(list(floors(s_nn, dims).values()))
        if dm > dn:
            outside.append({"capture": cap, "model": dm, "neighbour": dn})
    outside.sort(key=lambda o: o["neighbour"] - o["model"])
    return {
        "own_distance": d_own,
        "own_dimensions": len(full),
        "rank": 1 + len(losses),
        "candidates": sum(1 for c in contests if not c["family"]),
        "family_rank": 1 + len(beaten),
        "family_candidates": len(contests),
        "nearer": losses[:5],
        "family_nearer": [c for c in beaten if c["family"]][:5],
        "nearest_other": nearest,
        "outside_of": outside[:5],
        "wrong_rank": bool(losses),
        "outside_neighbourhood": bool(outside),
    }


def load_references() -> dict[str, list[dict]]:
    out = {}
    for cap in distance.capture_ids():
        raw = json.loads((distance.CAPTURE_DIR / f"{cap}.json").read_text())
        path = distance.REFERENCE_DIR / f"{cap}.json"
        if is_percussion(raw) or not path.exists():
            continue
        out[cap] = rows_for(json.loads(path.read_text()), distance.timbre_for(cap))
    return out


def sibling_map() -> dict[str, set[str]]:
    """Capture -> the other captures measuring the same patch."""
    rows = json.loads(distance.STATUS_JSON.read_text())["voices"]
    served = distance.serving(rows)
    patches = {cap: {v.get("patch") for v in vs} - {None} for cap, (vs, _) in served.items()}
    return {
        cap: {o for o, p in patches.items() if o != cap and p & mine}
        for cap, mine in patches.items()
    }


def family_map() -> dict[str, set[str]]:
    """Capture -> captures measuring a voice in the same GM family of eight."""
    rows = json.loads(distance.STATUS_JSON.read_text())["voices"]
    served = distance.serving(rows)
    groups = {cap: {v.get("group") for v in vs} - {None} for cap, (vs, _) in served.items()}
    return {
        cap: {o for o, g in groups.items() if o != cap and g & mine} for cap, mine in groups.items()
    }


def run(models: dict[str, list[dict]], references: dict[str, list[dict]]) -> list[dict]:
    sib = sibling_map()
    fam = family_map()
    out = []
    for cap, rows in sorted(models.items()):
        if cap not in references:
            continue
        out.append(
            {
                "capture": cap,
                **identify(rows, cap, references, sib.get(cap, set()), fam.get(cap, set())),
            }
        )
    return out


def control(references: dict[str, list[dict]], profiles: dict[str, dict]) -> list[dict]:
    """Known-signal control: a capture's SECOND timbre stands in for the model.

    A different recording of the same instrument must rank its own capture
    first; a detector that cannot do that on a real second recording cannot be
    trusted to flag a model.
    """
    sib = sibling_map()
    fam = family_map()
    out = []
    for cap, prof in sorted(profiles.items()):
        if cap not in references:
            continue
        chosen = distance.timbre_for(cap)
        others = sorted({r["timbre"] for r in prof["rows"]} - {chosen})
        for t in others:
            stand_in = [r for r in prof["rows"] if r["timbre"] == t]
            out.append(
                {
                    "capture": cap,
                    "stand_in": t,
                    **identify(stand_in, cap, references, sib.get(cap, set()), fam.get(cap, set())),
                }
            )
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--rows", default="", help="directory of `<capture>.rows.json` from distance.py run"
    )
    parser.add_argument(
        "--control", action="store_true", help="run the second-timbre control instead"
    )
    parser.add_argument("--json", default="", dest="json_out")
    args = parser.parse_args(argv)
    references = load_references()
    if args.control:
        profiles = {
            c: json.loads((distance.REFERENCE_DIR / f"{c}.json").read_text()) for c in references
        }
        result = control(references, profiles)
    else:
        if not args.rows:
            parser.error("--rows is required unless --control")
        models = {
            p.name.removesuffix(".rows.json"): json.loads(p.read_text())["rows"]
            for p in Path(args.rows).glob("*.rows.json")
        }
        result = run(models, references)
    flagged = [r for r in result if r["wrong_rank"] or r["outside_neighbourhood"]]
    for r in result:
        near = r["nearest_other"] or {}
        out = (r["outside_of"] or [{}])[0]
        mark = ("RANK " if r["wrong_rank"] else "") + ("OUT " if r["outside_neighbourhood"] else "")
        print(
            f"{r['capture']:28s} {r.get('stand_in', ''):14s} own={_f(r['own_distance'])} "
            f"rank={r['rank']}/{r['candidates'] + 1} fam={r['family_rank']}/{r['family_candidates'] + 1} closest-rival={near.get('capture', '-')}"
            f"({_f(near.get('own'))} vs {_f(near.get('rival'))}) "
            f"outside={out.get('capture', '-')}({_f(out.get('model'))}>{_f(out.get('neighbour'))}) {mark}"
        )
    print(f"\n{len(flagged)} of {len(result)} flagged")
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, indent=1) + "\n")
    return 0


def _f(v: float | None) -> str:
    return "-" if v is None else f"{v:.2f}"


if __name__ == "__main__":
    sys.exit(main())
