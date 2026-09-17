"""What a reference profile records of its own method, when it last moved, and what is missing."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from capture import load_config
from phrases import build_takes

#: Every committed capture definition lives here. Globbed rather than listed:
#: the failure worth catching is an instrument added without being added to a
#: list, which a list cannot catch.
CAPTURE_DIR = Path(__file__).resolve().parent / "capture"


def committed_capture(cfg: dict, tracked: dict, manifest: dict) -> dict:
    """The capture block a reference profile records: the method, never the identity.

    The manifest is written from the merged configuration, so it carries the
    untracked overlay's half — the plugin's component triple, and the preset
    each slot was loaded from. Copying it through would put a product name into
    a committed file, which is the one thing the split of a capture definition
    into a tracked half and a `.local.json` half exists to prevent.

    So the descriptive fields come from the tracked definition and only the
    grid comes from the manifest — which is also what makes the manifest worth
    reading here at all: it records what was *rendered*, and a resumed capture
    can have covered less than the definition asks for.

    Intersecting the two also drops the model's own grid. `render-grid` adds it
    to the corpus as one more timbre on purpose, so that every tool reading a
    corpus reads the model with no special case; a profile of the *reference*
    is the one place that is wrong, since it would describe the thing being
    measured as one of the measurements.
    """
    by_id = {t["id"]: t for t in tracked.get("timbres", [])}
    manifest_timbres = [t for t in manifest["timbres"] if t["id"] in by_id]
    return {
        # The GM program the model answers this reference with. Recorded here
        # rather than taken from whichever config a later `compare` is handed,
        # so a profile cannot be diffed against a different instrument.
        "program": int(cfg.get("program", 0)),
        "params": manifest["params"],
        "sample_rate": manifest["sample_rate"],
        "gate_ms": manifest["gate_ms"],
        "tail": manifest["tail"],
        "preroll_ms": manifest["preroll_ms"],
        "timbres": [by_id[t["id"]] for t in manifest_timbres],
        "notes": manifest["notes"],
        "velocities": manifest["velocities"],
    }


def profile_body(profile: dict) -> str:
    """The measurement itself — the profile minus its stamp, serialized.

    Re-serializing rather than comparing the dicts is what makes a profile read
    back from disk comparable with one just built: the file's floats, lists and
    nulls come back through the same writer they went out through, so a list
    that was a tuple in memory or a float printed at a different width cannot
    read as a change when nothing changed.
    """
    return json.dumps({k: v for k, v in profile.items() if k != "measured_utc"},
                      indent=1, ensure_ascii=False)


def measurement_stamp(profile: dict, out_path: Path) -> str:
    """When this measurement last *changed*, not when it was last taken.

    A profile is a pure function of the captured WAVs and the analysis code —
    the capture writes audio to disk and every number here is read back from it,
    with nothing measured live — so re-running `measure` over an unchanged
    corpus with unchanged code must produce the identical file. A wall-clock
    stamp written on every run defeats that, and it defeats more than a diff:

    - Nothing can then say mechanically whether an analysis edit moved a
      committed reference, which is the one question that decides whether the
      edit is safe to land.
    - A gate records the stamp of the reference its bounds were read against
      (`reference_measured_utc`), so a re-measure that changed nothing would
      still report every gate as predating its own reference.

    So the stamp is carried forward whenever everything else is unchanged, and
    moves only when a number does.
    """
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    if not out_path.exists():
        return now
    try:
        previous = json.loads(out_path.read_text())
    except (OSError, ValueError):
        return now
    kept = previous.get("measured_utc", "")
    if kept and profile_body(previous) == profile_body(profile):
        return kept
    return now


# --------------------------------------------------------------------------
# readiness: what an instrument has, and what the next round needs


def readiness(cfg: dict, *, archive: Path, reference_dir: Path) -> dict:
    """What exists for one instrument, and what the absence of each piece costs.

    Written because the answer was previously assembled by hand out of four
    directories and a `git log`, which is slow, easy to get wrong, and exactly
    the sort of thing that stops a loop between rounds. Every field here is a
    file that either exists or does not; nothing is inferred from a timestamp on
    disk, because a checkout reorders those.
    """
    ident = cfg["id"]
    out: dict = {"id": ident, "program": int(cfg.get("program", 0)),
                 "takes_set": cfg.get("takes") or "", "blocking": [], "next": []}

    profile_path = reference_dir / f"{ident}.json"
    profile = json.loads(profile_path.read_text()) if profile_path.exists() else None
    out["profile_rows"] = len(profile["rows"]) if profile else 0
    out["measured_utc"] = (profile or {}).get("measured_utc", "")
    if profile is None:
        out["blocking"].append(
            "no reference profile: capture.py calibrate/corpus/verify then profile.py measure. "
            "Needs the plugin, so it cannot be done from a plain clone")
        return out

    # Which dimensions the reference can actually adjudicate. A column missing
    # from every row is not a dimension this instrument is short on -- it is one
    # the profile predates, and nothing downstream will ever say so.
    present = {k for r in profile["rows"] for k in r}
    wanted = list(cfg.get("dimensions") or [])
    needs = {"stretch": "cents_vs_et", "decay": "decay_db_s", "aftersound": "decay_late_db_s",
             "doubling": "decay_early_db_s", "body": "body_below_f0_db",
             "attack": "attack_ms", "stereo": "stereo_width", "damper": "damper_release_ms",
             "balance": "partials_db", "centroid_pct": "centroid_hz", "tnr": "tnr_db",
             "vel_range": "peak_dbfs", "register": "held_peak_dbfs"}
    out["dimensions"] = wanted or ["(all measured)"]
    out["unbacked"] = sorted(d for d in wanted if needs.get(d) and needs[d] not in present)
    if out["unbacked"]:
        out["next"].append(
            f"the profile carries no {', '.join(needs[d] for d in out['unbacked'])}: "
            f"re-run profile.py measure over the corpus, or drop those from `dimensions`")

    gate_path = reference_dir / f"{ident}_gate.json"
    gate = json.loads(gate_path.read_text()) if gate_path.exists() else None
    out["gate_bounds"] = len(gate.get("bounds", {})) if gate else 0
    out["gate_timbre"] = (gate or {}).get("timbre", "")
    if gate is None:
        out["next"].append(
            "no gate: nothing holds this voice to anything. profile.py compare "
            "--write-gate reference/<id>_gate.json once the numbers are worth holding")
    else:
        against = gate.get("reference_measured_utc")
        if against is None:
            out["gate_stale"] = "unknown"
            out["next"].append(
                "the gate predates the field recording which reference it was measured "
                "against, so its staleness cannot be checked: re-record it once")
        elif against != out["measured_utc"]:
            out["gate_stale"] = "stale"
            out["next"].append(
                f"the gate was recorded against a reference measured {against} and the "
                f"profile now reads {out['measured_utc']}: its bounds compare against an "
                f"instrument no longer in the file. Re-record before trusting a failure")
        else:
            out["gate_stale"] = "current"
        missing_bounds = [d for d in wanted if d not in gate.get("bounds", {})]
        if missing_bounds:
            out["next"].append(
                f"gated dimensions with no bound recorded: {', '.join(missing_bounds)} — "
                f"listed but unchecked, which reads as passing. Re-record the gate")

    index = archive / "index.json"
    held = set(json.loads(index.read_text()).get(ident, {})) if index.exists() else set()
    out["takes_archived"] = len(held)
    if cfg.get("takes"):
        try:
            wanted_takes = {t.id for t in build_takes(cfg["takes"], out["program"])}
        except KeyError:
            wanted_takes = set()
        out["takes_total"] = len(wanted_takes)
        if wanted_takes - held:
            out["next"].append(
                f"{len(wanted_takes - held)} of {len(wanted_takes)} phrase takes have no "
                f"archived reference, so `profile.py takes` is blind to them: run "
                f"make_audition.py --archive-references once. Needs the plugin")
    else:
        out["takes_total"] = 0
        out["next"].append(
            "the capture names no phrase set, so nothing measures what happens BETWEEN "
            "notes — every coupling in this voice is unmeasured. Add one to the capture")
    return out


def status(cfg: dict, *, archive: Path, reference_dir: Path, every: bool) -> int:
    """Print the readiness of one instrument, or of every shipped capture."""
    configs = []
    if every:
        for path in sorted(CAPTURE_DIR.glob("*.json")):
            if path.name.endswith(".local.json"):
                continue
            configs.append(load_config(path))
    else:
        configs = [cfg]

    rows = [readiness(c, archive=archive, reference_dir=reference_dir) for c in configs]
    print(f"  {'instrument':<14}{'prog':>5}{'rows':>6}{'gate':>7}{'gate vs ref':>13}"
          f"{'takes':>8}{'unbacked dims':>16}")
    for r in rows:
        gate = f"{r['gate_bounds']}" if r["gate_bounds"] else "-"
        stale = r.get("gate_stale", "-") if r["gate_bounds"] else "-"
        took = (f"{r.get('takes_archived', 0)}/{r.get('takes_total', 0)}"
                if r.get("takes_total") else "-")
        print(f"  {r['id']:<14}{r['program']:>5}{r['profile_rows']:>6}{gate:>7}{stale:>13}"
              f"{took:>8}{','.join(r['unbacked']) or '-':>16}")
    for r in rows:
        if not (r["blocking"] or r["next"]):
            continue
        print(f"\n  {r['id']}:")
        for line in r["blocking"] + r["next"]:
            print(f"    - {line}")
    if not any(r["blocking"] or r["next"] for r in rows):
        print("\n  every instrument is ready to measure.")
    # Never non-zero on an incomplete instrument: this reports, it does not gate.
    # A loop reads it to decide where to start, and a status command that exits
    # 1 on "there is work to do" cannot be used for that.
    return 0
