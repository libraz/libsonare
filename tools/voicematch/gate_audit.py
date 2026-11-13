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

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from knobs import merge_overrides
from profile_gate import format_window

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


def compare(
    voice: dict, out: Path, lib: Path, overrides: str = "", base_overrides: str = ""
) -> None:
    # The fit's own tuning build: any other library ignores the overrides, and the
    # post-fit reading would then repeat the pre-fit one without saying so.
    env = {**os.environ, "SONARE_LIB_PATH": str(lib)}
    env.pop("SONARE_TUNING_OVERRIDES", None)
    overrides = merge_overrides(base_overrides, overrides)
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


def _with_windows(body: dict) -> list[dict]:
    """The body's gate readings, each carrying its window and the rows its dimension censored.

    A window-dependent one carries the span it was read over; `damper` carries how many
    rows its median dropped as censored, so a damper reading is not taken for the grid's.
    """
    windows = body.get("windows") or {}
    censored = body.get("censored") or {}
    return [
        {
            **r,
            **({"window": windows[r["key"]]} if r["key"] in windows else {}),
            **({"censored": censored[r["key"]]} if r["key"] in censored else {}),
        }
        for r in (body.get("gate") or {}).get("readings", [])
    ]


def compare_voice(
    voice: dict,
    out: Path,
    lib: Path,
    overrides: str = "",
    baseline: dict | None = None,
    base_overrides: str = "",
) -> None:
    """One voice's gate readings over every gate its patch reaches, keyed by capture.

    `baseline` is recorded beside the readings, so a pinned pre says what it pinned.
    """
    if "gates" not in voice:
        compare(voice, out, lib, overrides, base_overrides=base_overrides)
        body = json.loads(out.read_text())
        if body.get("gate"):
            body["gate"]["readings"] = _with_windows(body)
        if body.get("motion"):
            body["motion_states"] = {voice["capture"]: body["motion"]["states"]}
    else:
        readings = []
        motion_states = {}
        for g in voice["gates"]:
            part = out.with_name(f"{out.stem}.{g['capture']}.json")
            if not part.exists():
                compare({**voice, **g}, part, lib, overrides, base_overrides=base_overrides)
            record = json.loads(part.read_text())
            for r in _with_windows(record):
                readings.append({**r, "key": f"{g['capture']}:{r['key']}"})
            if record.get("motion"):
                motion_states[g["capture"]] = record["motion"]["states"]
        body = {"gate": {"readings": readings}}
        if motion_states:
            body["motion_states"] = motion_states
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


def audit(pre: dict, post: dict, weighting: dict[str, tuple] | None) -> list[dict]:
    """Per-(key, stat) before/after entries over two `compare_voice` bodies.

    `weighting` is `weighting()`'s per-dimension (flag, terms); a dimension it does
    not name is unweighted, and None means unknown, so every flag is None. A key is
    `<capture>:<dimension>` or a bare dimension.
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
        window = next((r["window"] for r in (ra, rb) if r and r.get("window")), None)
        censored = next((r["censored"] for r in (ra, rb) if r and "censored" in r), None)
        entry = {
            "key": key,
            "stat": stat,
            "bound": bound,
            "before": before,
            "after": after,
            "delta": None if missing else after - before,
            "state": _state(bound, before, after),
        }
        flag, terms = (
            (None, ())
            if weighting is None
            else weighting.get(key.rpartition(":")[2], (UNWEIGHTED, ()))
        )
        entry["weighted"] = flag
        if window:
            entry["window"] = window
        if censored is not None:
            entry["censored"] = censored
        if terms:
            entry["term"] = "+".join(terms)
        if missing:
            entry["missing"] = "+".join(missing)
        out.append(entry)
    return out


#: The objective weighs the dimension's own quantity.
WEIGHTED = "weighted"
#: It weighs a term mapped to the dimension that reads a different quantity.
PROXY = "proxy"
#: Nothing the objective weighs is mapped to the dimension.
UNWEIGHTED = "unweighted"


def weighting(
    weights: dict[str, float], table: dict, own: dict[str, tuple[str, ...]], *, classified=None
) -> dict[str, tuple[str | None, tuple[str, ...]]]:
    """Per gate dimension: (flag, the weighted terms behind it).

    `own` lists the terms reading a dimension's own quantity; `table` the terms
    mapped to it. A weighted own term is `weighted`; else a weighted mapped term is
    `proxy`; else `unweighted`. A dimension outside @p classified (default: every
    one) whose only weighted term is a mapped one has an unknown flag (None).
    """
    out: dict[str, tuple[str | None, tuple[str, ...]]] = {}
    for dim in table.keys() | own.keys():
        named = table.get(dim, ())
        mapped = (named,) if isinstance(named, str) else tuple(named)
        hit_own = tuple(t for t in own.get(dim, ()) if weights.get(t, 0.0) > 0.0)
        hit_map = tuple(t for t in mapped if weights.get(t, 0.0) > 0.0 and t not in hit_own)
        if hit_own:
            out[dim] = (WEIGHTED, hit_own)
        elif hit_map:
            known = classified is None or dim in classified
            out[dim] = (PROXY if known else None, hit_map)
    return out


def summary(entries: list[dict]) -> dict[str, list[dict]]:
    """The entries a fit made worse past a bound: newly over it, or further over it."""
    return {s: [e for e in entries if e["state"] == s] for s in ("crossed", "worse_over")}


def format_motion_states(states: dict[str, dict[str, int]]) -> list[str]:
    """One line per capture counting its reference rows by coherence state, undecided included."""
    return [
        f"  motion coherence {capture}: " + ", ".join(f"{state} {n}" for state, n in counts.items())
        for capture, counts in sorted(states.items())
    ]


def format_flagged(entries: list[dict]) -> list[str]:
    """The stderr lines naming every `crossed` and `worse_over` entry."""
    flagged = [e for s in summary(entries).values() for e in s]
    counts = ", ".join(f"{state} {sum(e['state'] == state for e in entries)}" for state in STATES)
    if not flagged:
        return [
            "gate audit: no gated statistic crossed or moved further past its bound",
            f"  states: {counts}",
        ]
    lines = [
        f"gate audit: {len(flagged)} gated statistic(s) pushed past a bound by this fit:",
        f"  states: {counts}",
    ]
    for e in flagged:
        flag = {
            WEIGHTED: "weighted",
            PROXY: "proxy",
            UNWEIGHTED: "UNWEIGHTED",
            None: "weighting unknown",
        }[e["weighted"]]
        if e.get("term"):
            flag += f" via {e['term']}"
        span = f" [window {format_window(e['window'])}]" if e.get("window") else ""
        if e.get("censored") is not None:
            span += f" [{e['censored']} damper rows censored]"
        lines.append(
            f"  {e['state']:<10} {e['key']} {e['stat']}: "
            f"{e['before']:.4g} -> {e['after']:.4g} vs bound {e['bound']:.4g} ({flag}){span}"
        )
    return lines
