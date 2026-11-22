"""Read a gate's bounds against a fresh compare without moving any of them.

A migrated gate knows when each bound value was set but not whether the model
has been inside it since, and a re-record would change bounds as well as answer
that. This applies the readings of a `profile.py compare --gate --json` run
(as `distance.py run` writes them) to each bound statistic's provenance: the
unpadded measurement, `held_utc` now when it is within the bound, and outcome
`kept` or `exceeded`; a hold drops `held_tree`, since the run's tree is not known here; a reading `profile_gate.unadjudicable` set aside changes nothing. Bound values, `set_*` and every other field are untouched,
and a statistic the run did not measure is left as it was.

    uv run --locked --project bindings/python \\
        python tools/voicematch/gate_verify.py --runs <distance run dir> [--write]
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import UTC, datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from profile_gate import OUTCOME_EXCEEDED, OUTCOME_KEPT


def apply(gate: dict, readings: list[dict], now: str) -> tuple[dict, dict[str, list[str]]]:
    """The gate with each read statistic's provenance updated, and what was found."""
    out = json.loads(json.dumps(gate))
    found: dict[str, list[str]] = {
        "held": [],
        "exceeded": [],
        "unmeasured": [],
        "unadjudicable": [],
    }
    for r in readings:
        bound = (out.get("bounds") or {}).get(r["key"])
        if not isinstance(bound, dict) or r["stat"] not in bound:
            continue
        name = f"{r['key']}.{r['stat']}"
        if r.get("unadjudicable"):
            found["unadjudicable"].append(name)
            continue
        if r.get("value") is None:
            found["unmeasured"].append(name)
            continue
        prov = bound.setdefault("provenance", {}).setdefault(r["stat"], {})
        prov["measured"] = round(float(r["value"]), 4)
        if r["value"] <= bound[r["stat"]]:
            prov["held_utc"], prov["outcome"] = now, OUTCOME_KEPT
            # The run's tree is not known here, so this hold names none and earns no clean stamp.
            prov.pop("held_tree", None)
            found["held"].append(name)
        else:
            prov.setdefault("held_utc", None)
            prov["outcome"] = OUTCOME_EXCEEDED
            found["exceeded"].append(name)
    return out, found


def verify(runs: Path, write: bool, now: str | None = None) -> dict[str, dict[str, list[str]]]:
    """Every gate a run read: what held, what is exceeded, what went unmeasured."""
    now = now or datetime.now(UTC).isoformat(timespec="seconds")
    report = {}
    for run in sorted(runs.glob("*.json")):
        body = json.loads(run.read_text())
        gate_info = body.get("gate") if isinstance(body, dict) else None
        if not gate_info or not gate_info.get("path"):
            continue
        gate_path = Path(gate_info["path"])
        if not gate_path.is_absolute():
            gate_path = Path.cwd() / gate_path
        if not gate_path.exists():
            continue
        gate = json.loads(gate_path.read_text())
        if gate_info.get("reference_measured_utc") != gate.get("reference_measured_utc"):
            report[gate_path.name] = {"skipped": ["the run read a different gate generation"]}
            continue
        updated, found = apply(gate, gate_info.get("readings") or [], now)
        report[gate_path.name] = found
        if write and updated != gate:
            gate_path.write_text(json.dumps(updated, indent=2) + "\n")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=Path, required=True, help="a distance.py run directory")
    parser.add_argument("--write", action="store_true", help="write the provenance back")
    parser.add_argument("--json", default="", dest="json_out")
    args = parser.parse_args(argv)
    report = verify(args.runs, args.write)
    gates_over = sorted(g for g, f in report.items() if f.get("exceeded"))
    stats = {
        k: sum(len(f.get(k, [])) for f in report.values())
        for k in ("held", "exceeded", "unadjudicable")
    }
    print(
        f"{len(report)} gates read; {stats['held']} statistics held, {stats['exceeded']} exceeded "
        f"in {len(gates_over)} gates, {stats['unadjudicable']} not adjudicable"
    )
    for g in gates_over:
        print(f"  {g}: {', '.join(report[g]['exceeded'])}")
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
