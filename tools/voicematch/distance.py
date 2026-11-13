"""How far each voice is from its oracle, per dimension, on one ruler across the bank.

A gate answers "no worse than when this was recorded" and `status.py`'s
agreement reads the medians a gate stored on the day it was written. Neither
says how far the model is from the reference NOW, and neither puts a piano's
tuning error beside a flute's brightness error. This does both.

`run` renders every capture's grid through `profile.py compare --json` against
one library and keeps each result. `table` reads them back. Each gated
statistic stays in its own unit (cents, dB, dB/s, ms, % of the centroid) and
is also divided by the dimension's floor — the references' own measured spread
where the gate records one, else the floor the gate writer falls back on — so
"3.2 floors" means the same thing on every voice. Which floor it was travels
with the number, because a guessed floor ranks less firmly than a measured one.

Censoring is counted, not hidden: `censored` is the grid rows the reference
holds for the timbre minus the rows that survived into the statistic. A
dimension whose rows mostly vanished reads small because it was barely asked.

Grouping (an owner per engine) is an optional `--groups` JSON mapping engine to
a group name, so how work is divided stays out of the tool.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import status
from _repo import REPO_ROOT
from profile_gate import DELTA_LABELS, PER_NOTE_DIMENSIONS, dimension_floor
from profile_measure import is_percussion

HERE = Path(__file__).resolve().parent
CAPTURE_DIR = HERE / "capture"
REFERENCE_DIR = HERE / "reference"
STATUS_JSON = REPO_ROOT / "tools" / "voice-status.json"
POLICY_JSON = HERE / "policy.json"
PYTHON = [sys.executable]

#: The statistic a voice is ranked on. The median absolute error is the one that
#: cannot be cancelled by errors of opposite sign; p90 is carried beside it.
RANK_STAT = "abs_median"
TOP_DIMENSIONS = 3

_UNIT = re.compile(r"\(([^()]*)\)\s*$")


def unit_of(key: str) -> str:
    """The unit `DELTA_LABELS` states for @p key, or `?` where it states none."""
    label = DELTA_LABELS.get(key, "")
    m = _UNIT.search(label)
    # A parenthetical stating a sign convention ("0 = mono") is not a unit.
    if not m or "=" in m.group(1):
        return "?"
    return m.group(1).split(",")[-1].strip()


def octaves_from_pct(pct: float | None) -> float | None:
    """A signed centroid ratio in percent as octaves; None where it has no log."""
    if pct is None or pct <= -100.0:
        return None
    return math.log2(1.0 + pct / 100.0)


def capture_ids() -> list[str]:
    return sorted(p.stem for p in CAPTURE_DIR.glob("*.json") if not p.name.endswith(".local.json"))


def timbre_for(cap: str) -> str:
    """The timbre the capture's gate was recorded on, else the capture's first."""
    gate = REFERENCE_DIR / f"{cap}_gate.json"
    if gate.exists():
        t = json.loads(gate.read_text()).get("timbre")
        if t:
            return t
    raw = json.loads((CAPTURE_DIR / f"{cap}.json").read_text())
    return raw["timbres"][0]["id"]


def compare_one(cap: str, out_dir: Path, lib: str) -> dict:
    """One capture's `profile.py compare --json`, kept beside its log; resumable."""
    out = out_dir / f"{cap}.json"
    log = out_dir / f"{cap}.log"
    if out.exists():
        return {"capture": cap, "state": "cached"}
    if not (REFERENCE_DIR / f"{cap}.json").exists():
        return {"capture": cap, "state": "no reference"}
    cmd = PYTHON + [
        str(HERE / "profile.py"),
        "compare",
        "--config",
        str(CAPTURE_DIR / f"{cap}.json"),
        "--timbre",
        timbre_for(cap),
        "--json",
        str(out),
        "--rows",
        str(out_dir / f"{cap}.rows.json"),
    ]
    gate = REFERENCE_DIR / f"{cap}_gate.json"
    if gate.exists():
        cmd += ["--gate", str(gate)]
    env = {**os.environ, "SONARE_LIB_PATH": lib}
    env.pop("SONARE_TUNING_OVERRIDES", None)
    with log.open("w") as fh:
        subprocess.run(cmd, env=env, stdout=fh, stderr=subprocess.STDOUT, check=False)
    return {"capture": cap, "state": "measured" if out.exists() else "failed"}


def run(out_dir: Path, lib: str, jobs: int, only: set[str] | None) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    lib_path = Path(lib)
    manifest = {
        "lib": str(lib_path),
        "lib_mtime": lib_path.resolve().stat().st_mtime if lib_path.exists() else None,
        "head": subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip(),
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    caps = [c for c in capture_ids() if not only or c in only]
    failed = 0
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(compare_one, c, out_dir, lib): c for c in caps}
        for f in as_completed(futures):
            r = f.result()
            failed += r["state"] == "failed"
            print(f"{r['capture']}: {r['state']}", flush=True)
    return 1 if failed else 0


def expected_rows(cap: str, timbre: str) -> tuple[int, int]:
    """(grid rows, distinct notes) the reference holds for @p timbre."""
    ref = json.loads((REFERENCE_DIR / f"{cap}.json").read_text())
    rows = [r for r in ref["rows"] if r.get("timbre") == timbre]
    return len(rows), len({r["note"] for r in rows})


def dimension_rows(summary: dict, spread: dict, grid: int, notes: int) -> list[dict]:
    """Every dimension of one compare summary on the common ruler, worst first."""
    out = []
    for key, s in summary.items():
        if not isinstance(s, dict) or RANK_STAT not in s:
            continue
        floor, source = dimension_floor(key, spread)
        want = notes if key in PER_NOTE_DIMENSIONS else grid
        n = int(s.get("n", 0))
        row = {
            "dimension": key,
            "unit": unit_of(key),
            "median": s.get("median"),
            "abs_median": s.get("abs_median"),
            "p90": s.get("p90"),
            "n": n,
            "expected": want,
            "censored": max(want - n, 0),
            "floor": floor,
            "floor_source": source,
            "floors": None if not n else s[RANK_STAT] / floor,
            "floors_p90": None if not n or s.get("p90") is None else s["p90"] / floor,
        }
        if key == "centroid_pct":
            row["octaves"] = octaves_from_pct(s.get("median"))
        out.append(row)
    out.sort(key=lambda r: -(r["floors"] if r["floors"] is not None else -1.0))
    return out


def voice_distance(dims: list[dict]) -> float | None:
    """Median of the measured dimensions' floors — the number a capture is ranked by.

    A median rather than a mean square, so one dimension on a guessed floor
    (brightness held to 1 % of the centroid) cannot rank the whole bank alone;
    `voice_rms` keeps the outlier-sensitive reading beside it.
    """
    vals = sorted(d["floors"] for d in dims if d["floors"] is not None)
    if not vals:
        return None
    n = len(vals)
    return vals[n // 2] if n % 2 else 0.5 * (vals[n // 2 - 1] + vals[n // 2])


def voice_rms(dims: list[dict]) -> float | None:
    """Root mean square of the measured dimensions' floors."""
    vals = [d["floors"] for d in dims if d["floors"] is not None]
    return math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else None


def serving(rows: list[dict]) -> dict[str, tuple[list[dict], str]]:
    """Capture id -> (the bank voices it measures, how that was decided).

    `voice-status.json` names one capture per voice, so a second capture of the
    same patch (a module twin, a kit's other windows) is reached through the
    patch it is named after, then its own address, then the kit flag.
    """
    out: dict[str, tuple[list[dict], str]] = {}
    for r in rows:
        if r.get("capture"):
            out.setdefault(r["capture"], ([], "voice-status"))[0].append(r)
    for cap in capture_ids():
        if cap in out:
            continue
        raw = json.loads((CAPTURE_DIR / f"{cap}.json").read_text())
        base = cap.removesuffix("_module")
        by_patch = [r for r in rows if r.get("patch") == base]
        by_address = [
            r
            for r in rows
            if not r["kit"]
            and r["program"] == raw.get("program")
            and r["bank"] == (raw.get("bank") or 0)
        ]
        if is_percussion(raw):
            out[cap] = ([r for r in rows if r["kit"]], "kit")
        elif by_patch:
            out[cap] = (by_patch, "patch")
        elif by_address:
            out[cap] = (by_address, "address")
    return out


def table(out_dir: Path, groups: dict[str, str]) -> dict:
    rows = json.loads(STATUS_JSON.read_text())["voices"]
    pol = json.loads(POLICY_JSON.read_text()) if POLICY_JSON.exists() else {}
    served = serving(rows)
    entries, missing = [], []
    for cap in capture_ids():
        path = out_dir / f"{cap}.json"
        if not path.exists():
            missing.append(cap)
            continue
        body = json.loads(path.read_text())
        timbre = body.get("timbre") or timbre_for(cap)
        gate_path = REFERENCE_DIR / f"{cap}_gate.json"
        gate = json.loads(gate_path.read_text()) if gate_path.exists() else {}
        grid, notes = expected_rows(cap, timbre)
        dims = dimension_rows(
            body.get("summary") or {}, gate.get("reference_spread") or {}, grid, notes
        )
        voices, served_by = served.get(cap, ([], "none"))
        engines = sorted({v.get("engine") or "?" for v in voices})
        tiers = [status.tier_of(pol, v["program"], v["bank"], kit=v["kit"]) for v in voices]
        best_tier = min(tiers, default=(0, "unserved"))
        gate_over = sorted(
            {r["key"] for r in (body.get("gate") or {}).get("readings", []) if r.get("over")}
        )
        entries.append(
            {
                "capture": cap,
                "timbre": timbre,
                "voices": [v["slug"] for v in voices],
                "served_by": served_by,
                "engine": ",".join(engines) if voices else None,
                "group": ",".join(sorted({groups.get(e, "?") for e in engines}))
                if voices
                else None,
                "tier": best_tier[1],
                "tier_rank": best_tier[0],
                "gate": gate_path.exists(),
                "gate_over": gate_over,
                "distance": voice_distance(dims),
                "distance_rms": voice_rms(dims),
                "measured": sum(1 for d in dims if d["n"]),
                "censored_rows": sum(d["censored"] for d in dims),
                "dimensions": dims,
            }
        )
    entries.sort(key=lambda e: -(e["distance"] or -1.0))
    manifest_path = out_dir / "manifest.json"
    return {
        "_": "Per-capture residual against its own reference. `floors` is abs_median over the "
        "dimension floor (measured reference spread where the gate records one, else the gate "
        "writer's fallback); `distance` is the median of a capture's floors, `distance_rms` "
        "their root mean square. Censored = reference "
        "grid rows minus rows that survived into the statistic.",
        "manifest": json.loads(manifest_path.read_text()) if manifest_path.exists() else {},
        "captures": entries,
        "not_run": missing,
        "no_gate": sorted(
            c for c in capture_ids() if not (REFERENCE_DIR / f"{c}_gate.json").exists()
        ),
    }


def _fmt(v: float | None, spec: str = ".2f") -> str:
    return "-" if v is None else format(v, spec)


def markdown(result: dict) -> str:
    lines = [
        "# Distance to oracle, per capture",
        "",
        f"Library: `{result['manifest'].get('lib')}` at `{result['manifest'].get('head', '')[:9]}`.",
        "",
        (
            "`dist` = median over measured dimensions of abs_median / floor (`rms` beside it). Top dimensions: "
            "name value unit (floors, floor source; censored/expected rows)."
        ),
        "",
    ]
    by_group: dict[str, list[dict]] = {}
    for e in result["captures"]:
        by_group.setdefault(e["group"] or "(no voice)", []).append(e)
    for group in sorted(by_group):
        lines += [
            f"## {group}",
            "",
            "| capture | voices | tier | dist | rms | gate over | censored | top dimensions |",
            "|---|---|---|---|---|---|---|---|",
        ]
        for e in by_group[group]:
            tops = []
            for d in e["dimensions"][:TOP_DIMENSIONS]:
                extra = f" ({_fmt(d.get('octaves'), '+.2f')} oct)" if "octaves" in d else ""
                tops.append(
                    f"{d['dimension']} {_fmt(d['abs_median'])} {d['unit']}{extra} "
                    f"[{_fmt(d['floors'], '.1f')}×, {d['floor_source']}; {d['censored']}/{d['expected']}]"
                )
            voices = ", ".join(e["voices"][:3]) + (" …" if len(e["voices"]) > 3 else "")
            lines.append(
                f"| {e['capture']} | {voices} | {e['tier']} | {_fmt(e['distance'])} | {_fmt(e['distance_rms'])} | "
                f"{', '.join(e['gate_over']) or '-'} | {e['censored_rows']} | {'; '.join(tops)} |"
            )
        lines.append("")
    if result["not_run"]:
        lines += ["## Not measured", "", ", ".join(result["not_run"]), ""]
    lines += ["## Captures with no gate file", "", ", ".join(result["no_gate"]) or "(none)", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_run = sub.add_parser("run", help="render every capture's grid and keep its compare JSON")
    p_run.add_argument("--out", required=True, help="directory the per-capture results go to")
    p_run.add_argument(
        "--lib", default=os.environ.get("SONARE_LIB_PATH", ""), help="libsonare to render with"
    )
    p_run.add_argument("--jobs", type=int, default=2)
    p_run.add_argument("--only", default="", help="comma-separated capture ids")
    p_tab = sub.add_parser("table", help="read the results back on one ruler")
    p_tab.add_argument("--out", required=True)
    p_tab.add_argument("--groups", default="", help="JSON file mapping engine -> group name")
    p_tab.add_argument("--json", default="", dest="json_out", help="write the table as JSON here")
    p_tab.add_argument("--md", default="", help="write the table as markdown here")
    args = parser.parse_args(argv)
    if args.cmd == "run":
        if not args.lib:
            parser.error("--lib or SONARE_LIB_PATH is required")
        only = {c for c in args.only.split(",") if c} or None
        return run(Path(args.out), args.lib, args.jobs, only)
    groups = json.loads(Path(args.groups).read_text()) if args.groups else {}
    result = table(Path(args.out), groups)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, indent=1) + "\n")
    text = markdown(result)
    if args.md:
        Path(args.md).write_text(text)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
