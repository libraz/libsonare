"""The substitution control: how many of a gate's bounds a DIFFERENT instrument passes.

A gate's bounds are recorded from whatever the voice measured on the day
(`profile_gate.write_gate_file` takes the run's own numbers times a margin), so a
green gate says "no worse than when this was written" and never "this is the
instrument the slot names". Those two readings are separated by one question:
put another instrument's reference rows where the model's would go and count how
many of the gate's dimensions it passes. A bound a choir clears against a sitar's
gate is not identifying the sitar, and the dimensions that clear most often are
the ones whose bounds are holding nothing.

Only captures sharing a grid can be substituted — same notes, same velocities —
since the comparison is per grid cell. Read-only over committed data: no render,
no build, no library. It exits 0 whatever it finds, on the same terms as
`status.py`, because a target that failed on "a gate is loose" could not be the
thing a calibration round reads to decide which gate to re-record.
"""

from __future__ import annotations

import argparse
import itertools
import json
from collections import Counter, defaultdict
from pathlib import Path

from capture import load_config
from profile_gate import DELTA_LABELS, reference_spread

HERE = Path(__file__).resolve().parent
CAPTURE_DIR = HERE / "capture"
REFERENCE_DIR = HERE / "reference"

#: The bound column this control can reach. `reference_spread` pools each pair's
#: per-row deltas and returns their median absolute value, which is exactly
#: `summarize_deltas`'s `abs_median` and nothing else — the signed median and the
#: p90 the gate also holds are not recoverable from a pooled median. So this is
#: one of the three columns `check_gate` applies, applied exactly, rather than a
#: reimplementation of all three that could drift from it.
COMPARED_STAT = "abs_median"


def grid_of(cfg: dict) -> tuple[tuple[int, ...], tuple[int, ...]] | None:
    """The (notes, velocities) a capture probes, or None if it names no grid."""
    notes, velocities = cfg.get("notes"), cfg.get("velocities")
    if not notes or not velocities:
        return None
    return tuple(sorted(set(notes))), tuple(sorted(set(velocities)))


def gated_captures() -> dict[str, dict]:
    """Every capture that has both a measured reference and a recorded gate.

    Both are required rather than one: a capture with no gate has no bounds to
    substitute against, and one with no reference has no rows to substitute in.
    """
    out = {}
    for path in sorted(CAPTURE_DIR.glob("*.json")):
        if path.name.endswith(".local.json"):
            continue
        cfg = load_config(path)
        ident = cfg["id"]
        if not (REFERENCE_DIR / f"{ident}.json").is_file():
            continue
        if not (REFERENCE_DIR / f"{ident}_gate.json").is_file():
            continue
        out[ident] = cfg
    return out


def grid_groups(captures: dict[str, dict]) -> dict[tuple, list[str]]:
    """Captures grouped by the grid they share, largest group first."""
    groups: dict[tuple, list[str]] = defaultdict(list)
    for ident, cfg in captures.items():
        grid = grid_of(cfg)
        if grid is not None:
            groups[grid].append(ident)
    return dict(sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])))


def _rows_of(profile: dict, timbre: str | None) -> list[dict]:
    """One timbre's rows, re-tagged so two profiles can be pooled as a pair.

    A capture carrying several timbres would otherwise collide: the spread keys
    rows by (note, velocity) per timbre, so folding two timbres under one tag
    silently keeps whichever came last. The gate names the timbre its bounds were
    recorded against, and that is the one this side is represented by.
    """
    rows = profile.get("rows", [])
    present = sorted({r["timbre"] for r in rows})
    chosen = timbre if timbre in present else (present[0] if present else None)
    return [r for r in rows if r["timbre"] == chosen]


def substitute(reference: dict, substituted: dict,
               ref_timbre: str | None, sub_timbre: str | None) -> dict[str, float]:
    """Per-dimension error of one instrument measured against another.

    Runs through `reference_spread`, which is the function the compare table
    already uses to put two references through the same per-row arithmetic the
    model goes through. Calling it rather than repeating it is the point: a
    control that computed its own deltas would be comparing against a gate whose
    numbers came from somewhere else.
    """
    rows = ([{**r, "timbre": "reference"} for r in _rows_of(reference, ref_timbre)]
            + [{**r, "timbre": "substituted"} for r in _rows_of(substituted, sub_timbre)])
    return reference_spread({"rows": rows})


def judge(spread: dict[str, float], gate: dict) -> tuple[set[str], set[str], set[str]]:
    """Which of a gate's bounds the substituted instrument clears, fails, or cannot reach.

    Three outcomes rather than two. A dimension the pair could not measure at all
    — every row censored, which is what a capped damper does — has no verdict in
    it, and counting one as either a pass or a failure would put a number where
    no comparison happened. `check_gate` calls it a failure because there it means
    a run stopped producing a column it used to; here it means these two
    instruments have nothing to compare, which is not the gate's looseness.
    """
    passed, failed, unreached = set(), set(), set()
    for dimension, bound in gate.get("bounds", {}).items():
        value = spread.get(dimension)
        limit = bound.get(COMPARED_STAT)
        if value is None or limit is None:
            unreached.add(dimension)
        elif value <= limit:
            passed.add(dimension)
        else:
            failed.add(dimension)
    return passed, failed, unreached


def identity_control(group: list[str], profiles: dict, gates: dict) -> tuple[int, int]:
    """Each gate against its OWN reference substituted in: every bound must pass.

    The control the substitution numbers are read against. A zero-delta pair is
    the one case where the answer is known in advance, so a run that cannot
    reach 100 % here is measuring its own plumbing rather than the gates — and a
    low substitution rate would then mean nothing.
    """
    clean = 0
    for ident in group:
        spread = substitute(profiles[ident], profiles[ident],
                            gates[ident].get("timbre"), gates[ident].get("timbre"))
        _passed, failed, _unreached = judge(spread, gates[ident])
        if not failed:
            clean += 1
    return clean, len(group)


def run_group(group: list[str]) -> dict:
    """Every ordered pair of one grid group, judged against the first one's gate."""
    profiles = {i: json.loads((REFERENCE_DIR / f"{i}.json").read_text()) for i in group}
    gates = {i: json.loads((REFERENCE_DIR / f"{i}_gate.json").read_text()) for i in group}

    # The spread is symmetric in the pair — it pools absolute deltas — while the
    # gate is not, so each unordered pair is measured once and read twice.
    spreads = {(a, b): substitute(profiles[a], profiles[b],
                                  gates[a].get("timbre"), gates[b].get("timbre"))
               for a, b in itertools.combinations(group, 2)}

    per_dimension = defaultdict(Counter)
    by_gate: Counter[str] = Counter()
    clears: list[tuple[str, str]] = []
    rates: list[float] = []
    comparisons = 0
    for a, b in itertools.permutations(group, 2):
        spread = spreads.get((a, b)) or spreads[(b, a)]
        passed, failed, unreached = judge(spread, gates[a])
        for dimension in passed:
            per_dimension[dimension]["pass"] += 1
        for dimension in failed:
            per_dimension[dimension]["fail"] += 1
        for dimension in unreached:
            per_dimension[dimension]["unreached"] += 1
        comparisons += len(passed) + len(failed)
        decided = len(passed) + len(failed)
        rates.append(len(passed) / decided if decided else 1.0)
        if not failed:
            clears.append((a, b))
            by_gate[a] += 1
    control_clean, control_total = identity_control(group, profiles, gates)
    return {
        "members": sorted(group),
        "pairs": len(rates),
        "comparisons": comparisons,
        "cleared_every_bound": len(clears),
        "cleared_by_gate": dict(by_gate.most_common()),
        "mean_pass_rate": sum(rates) / len(rates) if rates else 0.0,
        "identity_control": {"clean": control_clean, "of": control_total},
        "dimensions": {d: dict(c) for d, c in sorted(per_dimension.items())},
    }


def print_group(grid: tuple, result: dict) -> None:
    notes, velocities = grid
    print(f"\ngrid: {len(notes)} notes {notes[0]}-{notes[-1]} x {len(velocities)} velocities, "
          f"{len(result['members'])} captures, {result['pairs']} ordered pairs")
    control = result["identity_control"]
    print(f"  identity control: {control['clean']}/{control['of']} gates clear every bound "
          f"against their own reference")
    if control["clean"] != control["of"]:
        print("  the control did not reach 100 %, so nothing below is a reading of the gates")
    print(f"  comparisons reaching a verdict: {result['comparisons']}")
    print(f"  pairs clearing EVERY bound of the other's gate: {result['cleared_every_bound']}")
    for ident, count in result["cleared_by_gate"].items():
        print(f"    {ident}: {count} of {len(result['members']) - 1} other instruments pass "
              f"its gate whole")
    print(f"  mean share of bounds a foreign instrument clears: "
          f"{100.0 * result['mean_pass_rate']:.1f} %")
    print("\n  per dimension, how often a foreign instrument clears the bound:")
    rows = sorted(result["dimensions"].items(),
                  key=lambda kv: -(kv[1].get("pass", 0)
                                   / max(1, kv[1].get("pass", 0) + kv[1].get("fail", 0))))
    for dimension, counts in rows:
        decided = counts.get("pass", 0) + counts.get("fail", 0)
        share = 100.0 * counts.get("pass", 0) / decided if decided else 0.0
        unreached = counts.get("unreached", 0)
        tail = f"  ({unreached} pairs could not measure it)" if unreached else ""
        print(f"    {dimension:13s} {counts.get('pass', 0):4d}/{decided:<4d} = {share:5.1f} %  "
              f"{DELTA_LABELS.get(dimension, dimension)}{tail}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--all-grids", action="store_true",
                        help="every grid shared by two or more captures, not only the largest")
    parser.add_argument("--json", action="store_true", help="machine-readable, one object")
    args = parser.parse_args(argv)

    groups = {g: ids for g, ids in grid_groups(gated_captures()).items() if len(ids) >= 2}
    if not groups:
        print("no two gated captures share a grid, so nothing can be substituted")
        return 0
    if not args.all_grids:
        first = next(iter(groups))
        groups = {first: groups[first]}

    results = {}
    for grid, ids in groups.items():
        results[grid] = run_group(ids)
    if args.json:
        print(json.dumps({"grids": [{"notes": list(g[0]), "velocities": list(g[1]), **r}
                                    for g, r in results.items()]}, indent=2))
        return 0
    print(f"substitution control over {sum(len(r['members']) for r in results.values())} "
          f"captures in {len(results)} shared grid(s)")
    print(f"the bound column compared is {COMPARED_STAT!r}; the signed median and the p90 the "
          f"gate also holds\nare not recoverable from a pooled spread, so a pair clearing "
          f"every bound here has cleared one\nof the three columns rather than the gate")
    for grid, result in results.items():
        print_group(grid, result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
