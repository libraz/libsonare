"""The bank's p90 of a dimension's |median| in floors, written to `floor-units.json`.

`refit.py` raises a dimension's own term before the first fit when its measured
|median| is at least this many floors from the reference. The figure is read off
a distance run (`profile.py compare --json` over every capture, as `distance.py
run` keeps them), each dimension's `abs_median` divided by `dimension_floor`
under its own gate's `reference_spread`.

    python floor_units.py --runs DIR [--out PATH]
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import UTC, datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent

from profile_gate import dimension_floor

REFERENCE_DIR = HERE / "reference"
OUT_PATH = HERE / "floor-units.json"
#: The quantile of the pooled readings the file records.
QUANTILE = 0.9


def run_files(runs: Path) -> list[Path]:
    """The per-capture summaries of a run directory, without its row dumps and manifest."""
    return sorted(
        f
        for f in runs.glob("*.json")
        if not f.name.endswith(".rows.json") and f.name != "manifest.json"
    )


def floor_units(runs: Path) -> list[float]:
    """Every dimension of every capture's summary, as `abs_median` in floors."""
    out: list[float] = []
    for f in run_files(runs):
        body = json.loads(f.read_text())
        gate = REFERENCE_DIR / f"{f.stem}_gate.json"
        spread = json.loads(gate.read_text()).get("reference_spread", {}) if gate.exists() else {}
        for dim, s in (body.get("summary") or {}).items():
            if not isinstance(s, dict) or "abs_median" not in s:
                continue
            floor, _source = dimension_floor(dim, spread)
            if floor > 0:
                out.append(s["abs_median"] / floor)
    return sorted(out)


def quantile(values: list[float], q: float) -> float:
    """The value at rank `int(q * n)` of the sorted @p values."""
    return values[min(len(values) - 1, int(q * len(values)))]


def record(runs: Path) -> dict:
    """The file's content: the quantile, how many readings it came from, and where."""
    values = floor_units(runs)
    if not values:
        raise ValueError(f"{runs} holds no summary with an abs_median")
    newest = max(f.stat().st_mtime for f in run_files(runs))
    return {
        "p90": round(quantile(values, QUANTILE), 1),
        "readings": len(values),
        "captures": len(run_files(runs)),
        "source": str(runs),
        "run_date": datetime.fromtimestamp(newest, UTC).date().isoformat(),
        "generated": datetime.now(UTC).date().isoformat(),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--runs", type=Path, required=True, help="a verification run directory")
    ap.add_argument("--out", type=Path, default=OUT_PATH)
    args = ap.parse_args(argv)
    body = record(args.runs)
    args.out.write_text(json.dumps(body, indent=2) + "\n")
    print(f"{args.out.name}: p90 {body['p90']:g} floors over {body['readings']} readings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
