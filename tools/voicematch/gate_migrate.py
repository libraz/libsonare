"""Rewrite a gate file in the per-bound provenance format, values unchanged.

Each bound statistic gains a `provenance` record. `set_utc` / `set_build` come
from the gate's git history: the oldest commit in the unbroken run back from
HEAD whose value equals the file's today. `held_utc`, `measured` and `outcome`
are not recoverable and are written as null.

    python gate_migrate.py --out-dir <dir> [reference/<id>_gate.json ...]

Writing into `reference/` (or over the inputs) needs `--in-place`.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
REFERENCE_DIR = HERE / "reference"
STATS = ("median", "abs_median", "p90")

#: (commit, gate document at that commit), newest first.
History = list[tuple[str, dict]]


def _git(*args: str) -> str:
    done = subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True, check=False
    )
    return done.stdout


def read_history(path: Path) -> History:
    """The committed versions of a gate file, newest first. Read-only git."""
    try:
        rel = path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return []  # outside the repository: no history to read
    out: History = []
    for commit in _git("log", "--format=%H", "--", rel).split():
        try:
            out.append((commit, json.loads(_git("show", f"{commit}:{rel}"))))
        except ValueError:
            continue
    return out


def set_point(history: History, dim: str, stat: str, value: float) -> tuple[str | None, str | None]:
    """`(recorded_utc, built_utc)` of the oldest commit in the unbroken run holding `value`."""
    since: tuple[str | None, str | None] = (None, None)
    for _commit, doc in history:
        was = ((doc.get("bounds") or {}).get(dim) or {}).get(stat)
        if was is None or abs(was - value) >= 1e-9:
            break
        since = (doc.get("recorded_utc") or None, (doc.get("model_build") or {}).get("built_utc"))
    return since


def migrate(gate: dict, history: History) -> dict:
    """The gate with a `provenance` record on every bound that has none."""
    out = json.loads(json.dumps(gate))
    for dim, bound in (out.get("bounds") or {}).items():
        if "provenance" in bound:
            continue
        bound["provenance"] = {}
        for stat in STATS:
            if stat not in bound:
                continue
            set_utc, set_build = set_point(history, dim, stat, bound[stat])
            bound["provenance"][stat] = {
                "set_utc": set_utc,
                "set_build": set_build,
                "held_utc": None,
                "measured": None,
                "outcome": None,
            }
    return out


def _when(stamp: str) -> datetime:
    return datetime.fromisoformat(stamp)


def restamped_but_unchanged(gate: dict) -> bool:
    """Whether `recorded_utc` is newer than every known `set_utc` in the gate."""
    stamps = [
        p["set_utc"]
        for b in (gate.get("bounds") or {}).values()
        for p in (b.get("provenance") or {}).values()
        if p.get("set_utc")
    ]
    recorded = gate.get("recorded_utc")
    return bool(stamps and recorded and _when(recorded) > max(_when(s) for s in stamps))


def run(gates: list[Path], out_dir: Path | None, in_place: bool) -> int:
    if out_dir is None and not in_place:
        print("give --out-dir (or --in-place)", file=sys.stderr)
        return 2
    if not in_place and out_dir.resolve().is_relative_to(REFERENCE_DIR.resolve()):
        print(f"refusing to write into {REFERENCE_DIR}: pass --in-place", file=sys.stderr)
        return 2
    total = with_set = no_history = 0
    newer: list[str] = []
    for path in gates:
        history = read_history(path)
        migrated = migrate(json.loads(path.read_text()), history)
        stats = [p for b in migrated.get("bounds", {}).values() for p in b["provenance"].values()]
        total += len(stats)
        with_set += sum(1 for p in stats if p["set_utc"])
        if not history:
            no_history += len(stats)
        if restamped_but_unchanged(migrated):
            newer.append(path.name)
        dest = out_dir / path.name if out_dir else path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(json.dumps(migrated, indent=2) + "\n")
    print(f"{len(gates)} gates, {total} bound statistics")
    print(f"  with a set_utc: {with_set}")
    print(f"  no history (uncommitted): {no_history}")
    print(f"  recorded_utc newer than every set_utc: {len(newer)}")
    for name in newer:
        print(f"    {name}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gates", nargs="*", type=Path, help="default: every reference/*_gate.json")
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args(argv)
    gates = args.gates or sorted(REFERENCE_DIR.glob("*_gate.json"))
    return run(gates, args.out_dir, args.in_place)


if __name__ == "__main__":
    sys.exit(main())
