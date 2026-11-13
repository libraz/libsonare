"""Gate readings of a voice under a set of tuning overrides, and the audit of one pair of them.

A fit's own loss cannot say what it did to a gated dimension it does not weight.
`compare_voice` takes the readings over every gate a patch reaches; `audit` pairs
two such readings by (key, stat) and names what the second did to each bound.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CAPTURE_DIR = HERE / "capture"
PYTHON = [sys.executable]

#: How a (key, stat) moved between the two readings, in the order `audit` decides them.
STATES = (
    "crossed",
    "worse_over",
    "cleared",
    "better",
    "worse_within",
    "unchanged",
    "unmeasured",
)


def compare(voice: dict, out: Path, lib: Path, overrides: str = "") -> None:
    # The fit's own tuning build: any other library ignores the overrides, and the
    # post-fit reading would then repeat the pre-fit one without saying so.
    env = {**os.environ, "SONARE_LIB_PATH": str(lib)}
    env.pop("SONARE_TUNING_OVERRIDES", None)
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    cmd = PYTHON + [
        str(HERE / "profile.py"),
        "compare",
        "--config",
        str(CAPTURE_DIR / f"{voice['capture']}.json"),
        "--timbre",
        voice["timbre"],
        "--gate",
        voice["gate"],
        "--json",
        str(out),
    ]
    log = out.with_suffix(".log")
    with log.open("w") as fh:
        subprocess.run(cmd, env=env, stdout=fh, stderr=subprocess.STDOUT, check=False)
    if not out.exists():
        raise RuntimeError(f"{voice['slug']}: compare wrote nothing, see {log}")


def compare_voice(
    voice: dict, out: Path, lib: Path, overrides: str = "", baseline: dict | None = None
) -> None:
    """One voice's gate readings over every gate its patch reaches, keyed by capture.

    `baseline` is recorded beside the readings, so a pinned pre says what it pinned.
    """
    if "gates" not in voice:
        compare(voice, out, lib, overrides)
        body = json.loads(out.read_text())
    else:
        readings = []
        for g in voice["gates"]:
            part = out.with_name(f"{out.stem}.{g['capture']}.json")
            if not part.exists():
                compare({**voice, **g}, part, lib, overrides)
            for r in json.loads(part.read_text())["gate"]["readings"]:
                readings.append({**r, "key": f"{g['capture']}:{r['key']}"})
        body = {"gate": {"readings": readings}}
    if baseline is not None:
        body["baseline"] = baseline
    out.write_text(json.dumps(body, indent=2) + "\n")


def _readings(body: dict) -> dict[tuple[str, str], dict]:
    return {(r["key"], r["stat"]): r for r in (body.get("gate") or {}).get("readings", [])}


def _state(bound: float | None, before: float | None, after: float | None) -> str:
    if before is None or after is None:
        return "unmeasured"
    if bound is None:
        return "unchanged" if after == before else ("better" if after < before else "worse_within")
    was, now = before > bound, after > bound
    if now and not was:
        return "crossed"
    if now and was:
        if after > before:
            return "worse_over"
        return "better" if after < before else "unchanged"
    if was:
        return "cleared"
    if after == before:
        return "unchanged"
    return "better" if after < before else "worse_within"


def audit(pre: dict, post: dict, weighted: set[str] | None) -> list[dict]:
    """Per-(key, stat) before/after entries over two `compare_voice` bodies.

    `weighted` names the gate dimensions the objective weighs (None: unknown, so
    each entry's flag is None). A key is `<capture>:<dimension>` or a bare dimension.
    A reading missing or unmeasured on one side is `unmeasured`, with `missing`
    naming the side(s), so it is never read as `unchanged`.
    """
    a, b = _readings(pre), _readings(post)
    out = []
    for key, stat in sorted(a.keys() | b.keys()):
        ra, rb = a.get((key, stat)), b.get((key, stat))
        before = ra.get("value") if ra else None
        after = rb.get("value") if rb else None
        bound = next((r.get("limit") for r in (ra, rb) if r and r.get("limit") is not None), None)
        missing = [n for n, v in (("before", before), ("after", after)) if v is None]
        entry = {
            "key": key,
            "stat": stat,
            "bound": bound,
            "before": before,
            "after": after,
            "delta": None if missing else after - before,
            "weighted": None if weighted is None else key.rpartition(":")[2] in weighted,
            "state": _state(bound, before, after),
        }
        if missing:
            entry["missing"] = "+".join(missing)
        out.append(entry)
    return out


def weighted_dimensions(weights: dict[str, float], table: dict) -> set[str]:
    """The gate dimensions whose carrying term (or any of several) has a positive weight."""
    found = set()
    for dim, named in table.items():
        terms = (named,) if isinstance(named, str) else named
        if any(weights.get(t, 0.0) > 0.0 for t in terms):
            found.add(dim)
    return found


def summary(entries: list[dict]) -> dict[str, list[dict]]:
    """The entries a fit made worse past a bound: newly over it, or further over it."""
    return {s: [e for e in entries if e["state"] == s] for s in ("crossed", "worse_over")}


def format_flagged(entries: list[dict]) -> list[str]:
    """The stderr lines naming every `crossed` and `worse_over` entry."""
    flagged = [e for s in summary(entries).values() for e in s]
    if not flagged:
        return ["gate audit: no gated statistic crossed or moved further past its bound"]
    lines = [f"gate audit: {len(flagged)} gated statistic(s) pushed past a bound by this fit:"]
    for e in flagged:
        flag = {True: "weighted", False: "UNWEIGHTED", None: "weighting unknown"}[e["weighted"]]
        lines.append(
            f"  {e['state']:<10} {e['key']} {e['stat']}: "
            f"{e['before']:.4g} -> {e['after']:.4g} vs bound {e['bound']:.4g} ({flag})"
        )
    return lines
