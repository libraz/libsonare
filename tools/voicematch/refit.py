"""Refit the bank's voices under the current objective, and decide each adoption by the gate.

A fit's own loss cannot decide whether its values should ship: a search can
lower the weighted objective by selling a dimension the gate holds. So each
voice runs the same four steps, recorded under `<scratch>/refit/<objective>/`:

    pre       profile.py compare --gate --json with the shipped values
    fit       autofit.py --spec auto --program-only --dry-run --out, against the
              capture's corpus, training on alternate notes and three velocities
              and holding the other notes out
    post      the same compare with the fit's overrides applied
    verdict   adopt only when values moved, the held-out notes improved, no gated
              statistic newly exceeds its bound and the summed excess over the
              bounds did not grow

`plan` lists the voices, `run` executes the steps (resuming wherever a step's
file already exists), `report` prints the verdicts, and `adopt` writes the
adopted values into the program tables and records every finished fit in
`fits.json`. A fit that improves the held-out notes while newly exceeding a
bound, or moving a switch, is a trade, and `resolve` settles it by the furthest
point along the search the gate accepts, refitting once with the broken
dimensions weighed up when no point is.

Variations derive from their capital's patch at table-build time, so `--banks
capitals` (the default) refits capitals and a later `--banks variations` pass
starts from what was adopted, and `--banks kits` fits each drum note of a kit
against its machine captures. A patch shared by several voices is fitted once,
against the first voice's capture.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import bank
import boundary
import fits as fits_mod
from _repo import REPO_ROOT
from build_lib import _cached_option, dylib_path
from capture import CORPUS_ROOT
from knobs import format_value
from objective import objective_digest
from writeback import (
    patch_field_assignments,
    switch_field,
    write_drum_fields,
    write_family_fields,
    write_patch_fields,
)

HERE = Path(__file__).resolve().parent
STATUS = REPO_ROOT / "tools" / "voice-status.json"
CAPTURE_DIR = HERE / "capture"
REFERENCE_DIR = HERE / "reference"
PYTHON = [sys.executable]

#: Evaluations per resolved knob: held-out loss bottoms between 10 and 30 per knob on
#: the voices measured, and 12 cost the slowest of them about one per cent.
EVALS_PER_KNOB = 12
#: Normalized held-out improvement a fit must show to count as generalising.
HELD_OUT_MARGIN = 0.005


def run_root(digest: str) -> Path:
    return CORPUS_ROOT / "refit" / digest


def _split(values: list[int]) -> tuple[list[int], list[int]]:
    """Alternate entries: training on the even positions, held out on the odd ones."""
    ordered = sorted(values)
    return ordered[0::2], ordered[1::2]


def _three(velocities: list[int]) -> list[int]:
    v = sorted(velocities)
    return v if len(v) <= 3 else [v[0], v[len(v) // 2], v[-1]]


def plan_kits(rows: list[dict]) -> tuple[list[dict], list[dict]]:
    """One entry per drum note a kit's machine captures hold.

    A note is fitted against the capture with the longest gate that holds it, since
    a longer gate carries its decay as well as its strike, and judged by every gate
    the kit has. The module holds one velocity per note, so there is no held-out
    condition and the gate decides alone.
    """
    todo, skipped = [], []
    for row in (r for r in rows if r["kit"]):
        caps = [
            json.loads((CAPTURE_DIR / f"{c.id}.json").read_text())
            for c in bank.captures_for(row["program"], kit=True)
        ]
        gates = [
            {
                "capture": c["id"],
                "gate": str(REFERENCE_DIR / f"{c['id']}_gate.json"),
                "timbre": json.loads((REFERENCE_DIR / f"{c['id']}_gate.json").read_text())[
                    "timbre"
                ],
            }
            for c in caps
            if (REFERENCE_DIR / f"{c['id']}_gate.json").exists()
        ]
        machine = sorted(
            (c for c in caps if c.get("source_class") == "module"),
            key=lambda c: -int(c["gate_ms"]),
        )
        if not machine or not gates:
            skipped.append({"slug": row["slug"], "reason": "no machine capture with a gate"})
            continue
        for note in sorted({n for c in machine for n in c["notes"]}):
            cap = next(c for c in machine if note in c["notes"])
            corpus = CORPUS_ROOT / "capture" / cap["id"]
            if not (corpus / "manifest.json").exists():
                skipped.append({"slug": f"{row['slug']}-n{note}", "reason": "no corpus"})
                continue
            todo.append(
                {
                    "slug": f"{row['slug']}-n{note:03d}",
                    "program": row["program"],
                    "bank": 0,
                    "patch": f"d{note:03d}",
                    "engine": row.get("engine"),
                    "capture": cap["id"],
                    "timbre": cap["timbres"][0]["id"],
                    "corpus": str(corpus),
                    "drum_note": note,
                    "gates": gates,
                    "gate_only": True,
                }
            )
    return todo, skipped


def plan(banks: str) -> tuple[list[dict], list[dict]]:
    """(voices to fit, voices skipped with the reason)."""
    rows = json.loads(STATUS.read_text())["voices"]
    if banks == "kits":
        return plan_kits(rows)
    todo, skipped, seen = [], [], set()
    for row in sorted(rows, key=lambda r: (r["bank"], r["program"])):
        want_capital = banks == "capitals"
        if row["kit"] or (row["bank"] == 0) != want_capital:
            continue
        slug, patch, cap = row["slug"], row.get("patch"), row.get("capture")
        if not cap:
            skipped.append({"slug": slug, "reason": "no capture"})
            continue
        if not patch:
            skipped.append({"slug": slug, "reason": "no patch reported"})
            continue
        if patch in seen:
            skipped.append(
                {"slug": slug, "reason": f"patch {patch} already fitted by another voice"}
            )
            continue
        raw = json.loads((CAPTURE_DIR / f"{cap}.json").read_text())
        gate = REFERENCE_DIR / f"{cap}_gate.json"
        where = boundary.reference_boundary(boundary.Reference.from_capture(raw), row["program"])
        if where != boundary.BOUNDARY_INSTRUMENT:
            skipped.append({"slug": slug, "reason": f"reference boundary is {where}"})
            continue
        if not gate.exists():
            skipped.append({"slug": slug, "reason": "no gate to judge an adoption by"})
            continue
        corpus = CORPUS_ROOT / "capture" / cap
        if not (corpus / "manifest.json").exists():
            skipped.append({"slug": slug, "reason": "no corpus on this machine"})
            continue
        train, held = _split(raw["notes"])
        if not held:
            skipped.append({"slug": slug, "reason": "one note: nothing to hold out"})
            continue
        seen.add(patch)
        todo.append(
            {
                "slug": slug,
                "program": row["program"],
                "bank": row["bank"],
                "patch": patch,
                "engine": row.get("engine"),
                "capture": cap,
                "timbre": json.loads(gate.read_text())["timbre"],
                "corpus": str(corpus),
                "gate": str(gate),
                "notes": train,
                "validate_notes": held,
                "velocities": _three(raw["velocities"]),
            }
        )
    return todo, skipped


def _compare(voice: dict, out: Path, lib: Path, overrides: str = "") -> None:
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


def _compare_voice(voice: dict, out: Path, lib: Path, overrides: str = "") -> None:
    """One voice's gate readings; a kit's are every kit gate's, keyed by capture."""
    if "gates" not in voice:
        _compare(voice, out, lib, overrides)
        return
    readings = []
    for g in voice["gates"]:
        part = out.with_name(f"{out.stem}.{g['capture']}.json")
        if not part.exists():
            _compare({**voice, **g}, part, lib, overrides)
        for r in json.loads(part.read_text())["gate"]["readings"]:
            readings.append({**r, "key": f"{g['capture']}:{r['key']}"})
    out.write_text(json.dumps({"gate": {"readings": readings}}, indent=2) + "\n")


def _fit(
    voice: dict, out: Path, build_dir: str, per_knob: int, weights: dict[str, float] | None = None
) -> None:
    if "drum_note" in voice:
        grid = ["--drum-note", str(voice["drum_note"])]
    else:
        grid = [
            "--notes",
            ",".join(map(str, voice["notes"])),
            "--velocities",
            ",".join(map(str, voice["velocities"])),
            "--validate-notes",
            ",".join(map(str, voice["validate_notes"])),
        ]
    cmd = PYTHON + [
        str(HERE / "autofit.py"),
        "--spec",
        "auto",
        "--program",
        str(voice["program"]),
        "--bank",
        str(voice["bank"]),
        "--program-only",
        "--corpus",
        voice["corpus"],
        "--corpus-timbre",
        voice["timbre"],
        *grid,
        "--optimizer",
        "coord",
        "--per-knob-evals",
        "3",
        "--metric-threads",
        "2",
        "--evals-per-knob",
        str(per_knob),
        "--build-dir",
        build_dir,
        "--dry-run",
        "--out",
        str(out),
    ]
    for term, weight in sorted((weights or {}).items()):
        cmd += [f"--w-{term}", f"{weight:g}"]
    log = out.with_suffix(".log")
    with log.open("w") as fh:
        subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, check=False)
    if not out.exists():
        raise RuntimeError(f"{voice['slug']}: the fit wrote no report, see {log}")


def _refusal(log: Path) -> str:
    """The last line of a fit log that ended without a report: the reason it gave."""
    lines = [ln for ln in log.read_text().splitlines() if ln.strip()] if log.exists() else []
    return lines[-1].strip() if lines else "the fit wrote no report"


def _excess(readings: list[dict]) -> tuple[float, set[tuple[str, str]]]:
    over = {(r["key"], r["stat"]) for r in readings if r["over"]}
    total = sum(max(r["excess"] or 0.0, 0.0) for r in readings if r["over"])
    return total, over


def switch_moves(report: dict) -> list[str]:
    moved = []
    for knob, value in zip(report["knobs"], report["selected"]["values"]):
        path = knob["key"].partition(".")[2]
        if switch_field(path) and round(value) != round(knob["start"]):
            moved.append(knob["key"])
    return moved


def verdict(pre: dict, fit: dict, post: dict, *, gate_only: bool = False) -> dict:
    """The adoption decision for one voice, with the numbers it rests on.

    `gate_only` is for a grid with one condition per note, a drum module's single
    velocity, where no held-out condition exists and the gate is the whole judgement.
    """
    moved = [
        k["key"]
        for k, v in zip(fit["knobs"], fit["selected"]["values"])
        if format_value(v) != format_value(k["start"])
    ]
    val = fit.get("validation") or {}
    held = (
        val["start"] - val["best"]
        if val.get("independent") and "start" in val and "best" in val
        else None
    )
    pre_sum, pre_over = _excess(pre["gate"]["readings"])
    post_sum, post_over = _excess(post["gate"]["readings"])
    newly = sorted(f"{k} {s}" for k, s in post_over - pre_over)
    out = {
        "moved": len(moved),
        "loss": fit["loss"].get("selected"),
        "held_out_gain": held,
        "excess": [pre_sum, post_sum],
        "newly_over": newly,
        "switches": switch_moves(fit),
        "refused": (fit.get("write_back") or {}).get("refused"),
    }
    if out["refused"]:
        out["decision"], out["why"] = "kept", f"write-back refused: {out['refused']}"
    elif not moved:
        out["decision"], out["why"] = "kept", "no value moved"
    elif held is None and not gate_only:
        out["decision"], out["why"] = "kept", "no independent held-out result"
    elif held is not None and held <= HELD_OUT_MARGIN:
        out["decision"], out["why"] = "kept", f"held-out notes did not improve ({held:+.4f})"
    elif out["switches"]:
        out["decision"], out["why"] = "trade", f"switch field moved: {', '.join(out['switches'])}"
    elif newly:
        out["decision"], out["why"] = "trade", f"newly over its bound: {', '.join(newly)}"
    elif post_sum > pre_sum + 1e-9:
        out["decision"], out["why"] = "kept", f"gate excess grew {pre_sum:.3f} -> {post_sum:.3f}"
    else:
        gain = "gate only" if held is None else f"held-out {held:+.4f}"
        out["decision"], out["why"] = (
            "adopt",
            (f"{gain}, gate excess {pre_sum:.3f} -> {post_sum:.3f}"),
        )
    return out


def _tuning_lib(build_dir: str) -> Path:
    path = (REPO_ROOT / build_dir).resolve()
    lib = dylib_path(path)
    if lib is None or _cached_option(path, "BUILD_TUNING") != "ON":
        raise RuntimeError(f"{build_dir} holds no BUILD_TUNING=ON library to apply overrides with")
    return lib


def run_voice(voice: dict, root: Path, build_dir: str, per_knob: int) -> dict:
    d = root / voice["slug"]
    d.mkdir(parents=True, exist_ok=True)
    (d / "voice.json").write_text(json.dumps(voice, indent=2) + "\n")
    pre, fit, post, ver = d / "pre.json", d / "fit.json", d / "post.json", d / "verdict.json"
    lib = _tuning_lib(build_dir)
    if not pre.exists():
        _compare_voice(voice, pre, lib)
    if not fit.exists():
        _fit(voice, fit, build_dir, per_knob)
    fit_report = json.loads(fit.read_text())
    if not post.exists():
        _compare_voice(voice, post, lib, fit_report.get("overrides") or "")
    result = {
        "slug": voice["slug"],
        **verdict(
            json.loads(pre.read_text()),
            fit_report,
            json.loads(post.read_text()),
            gate_only=bool(voice.get("gate_only")),
        ),
    }
    ver.write_text(json.dumps(result, indent=2) + "\n")
    return result


def writable(report: dict, values: list[float]) -> list[float]:
    """The values as the write-back would land them: a switch is never written, so it
    stays at its shipped value."""
    return [
        k["start"] if switch_field(k["key"].partition(".")[2]) else v
        for k, v in zip(report["knobs"], values)
    ]


def overrides_for(report: dict, values: list[float]) -> str:
    return ",".join(
        f"{k['key']}={format_value(v)}"
        for k, v in zip(report["knobs"], values)
        if format_value(v) != format_value(k["start"])
    )


def gate_load(readings: list[dict]) -> float:
    """Mean of each bounded statistic over its limit: lower is closer to the reference."""
    ratios = [
        r["value"] / r["limit"] for r in readings if r["value"] is not None and r["limit"] > 0
    ]
    return sum(ratios) / len(ratios) if ratios else float("inf")


def along_the_path(voice: dict, d: Path, lib: Path, report: dict, tag: str) -> dict | None:
    """The furthest point of the search the gate accepts, found by bisection.

    The search walks away from the shipped values one improvement at a time, and a
    bound the objective does not weigh is crossed somewhere along that walk; the
    points before it are the loss's improvements the gate allows. A point is
    accepted when no bound is newly exceeded, the summed excess did not grow and
    the gate load fell.
    """
    path = report.get("path") or []
    if not path:
        return None
    pre = json.loads((d / "pre.json").read_text())["gate"]["readings"]
    pre_sum, pre_over = _excess(pre)
    pre_load = gate_load(pre)
    checked: dict[int, dict] = {}

    def accepted(i: int) -> bool:
        values = writable(report, path[i]["values"])
        out = d / f"path_{tag}_{path[i]['eval']}.json"
        if not out.exists():
            _compare_voice(voice, out, lib, overrides_for(report, values))
        post = json.loads(out.read_text())["gate"]["readings"]
        post_sum, post_over = _excess(post)
        ok = not post_over - pre_over and post_sum <= pre_sum + 1e-9 and gate_load(post) < pre_load
        checked[i] = {
            "eval": path[i]["eval"],
            "loss": path[i]["loss"],
            "accepted": ok,
            "load": gate_load(post),
            "values": values,
        }
        return ok

    lo, hi = -1, len(path)
    if accepted(len(path) - 1):
        lo = len(path) - 1
    else:
        hi = len(path) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if accepted(mid):
                lo = mid
            else:
                hi = mid
    tried = [checked[i] | {"values": None} for i in sorted(checked)]
    if lo < 0:
        return {"accepted": None, "tried": tried, "pre_load": pre_load}
    return {"accepted": checked[lo], "tried": tried, "pre_load": pre_load}


#: The fit term that carries each gated dimension. A dimension the gate holds and the
#: objective weighs too little is how a fit improves its loss while breaking a bound,
#: so a trade is first refitted with these terms weighted up. `stereo` has no term.
#: A tuple names the terms that read one dimension in different tone classes; the
#: ones the fit's class weighs are raised.
GATE_TERMS = {
    "stretch": "stiff",
    "decay": "slope",
    "partial_tilt": "slope",
    "damper": "env",
    "attack": "env",
    "balance": "harm",
    "centroid_pct": "harm",
    "tnr": "tnr",
    "vel_range": "dyn",
    "register": "level",
    "aftersound": "tail",
    "doubling": "tail",
    "body": "lf",
    "attack_hf": "hf",
    "attack_hf_swing": ("hfdyn", "modedyn"),
    "crest": "crest",
}
#: The same for a drum note, whose objective is the percussion term set. `level`
#: and `dyn` are absent: a module note is one velocity, and both need a contrast.
PERCUSSION_GATE_TERMS = {
    "band_tilt": "band",
    "band_shape": "band",
    "band_decay": "bdecay",
    "ring": "ring",
    "tonality": "tonal",
    "tilt_rise": "rise",
    "centroid_pct": "bright",
    "attack": "env",
    "crest": "crest",
}


def reweighed(
    report: dict, newly: list[str], *, percussive: bool = False
) -> dict[str, float] | None:
    weights = dict(report.get("fit_profile", {}).get("weights") or {})
    table = PERCUSSION_GATE_TERMS if percussive else GATE_TERMS
    # A kit's readings are keyed `<capture>:<dimension>`.
    dims = {d.split()[0].rpartition(":")[2] for d in newly}
    terms: set[str] = set()
    for d in dims:
        named = table.get(d, ())
        named = (named,) if isinstance(named, str) else named
        weighed = [t for t in named if weights.get(t, 0.0) > 0.0]
        terms.update(weighed or named[:1])
    if not terms:
        return None
    # Only the raised terms: the rest resolve to the same class defaults unasked, and
    # naming one the probe cannot compute (a level with no contrast) is refused.
    return {t: max(2.0 * weights.get(t, 0.0), 1.0) for t in terms}


def resolve_voice(d: Path, build_dir: str, per_knob: int) -> dict | None:
    """Settle a trade by the furthest point along the search the gate accepts.

    Failing that on the fit itself, the fit is repeated once with the terms carrying
    the broken bounds weighed up, and its own path is searched the same way. With
    neither, the shipped values are kept.
    """
    ver = json.loads((d / "verdict.json").read_text())
    if ver["decision"] != "trade" or ver.get("resolved"):
        return None
    lib = _tuning_lib(build_dir)
    voice = json.loads((d / "voice.json").read_text())
    report = json.loads((d / "fit.json").read_text())
    attempts = []
    found = along_the_path(voice, d, lib, report, "fit")
    attempts.append({"fit": "fit.json", **(found or {"accepted": None})})
    chosen = ("fit.json", found) if found and found["accepted"] else None
    if chosen is None:
        weights = reweighed(report, ver["newly_over"], percussive="drum_note" in voice)
        if weights is not None:
            fit_path = d / "fit_r1.json"
            refused = None
            if not fit_path.exists():
                try:
                    _fit(voice, fit_path, build_dir, per_knob, weights)
                except RuntimeError:
                    # A raised term the probe cannot read (a tail band past every note) is
                    # refused by the fit itself; that leaves the shipped values.
                    refused = _refusal(fit_path.with_suffix(".log"))
            if refused is not None:
                attempts.append({"fit": "fit_r1.json", "weights": weights, "refused": refused})
            else:
                again = json.loads(fit_path.read_text())
                found = along_the_path(voice, d, lib, again, "r1")
                attempts.append(
                    {"fit": "fit_r1.json", "weights": weights, **(found or {"accepted": None})}
                )
                if found and found["accepted"]:
                    chosen = ("fit_r1.json", found)
    if chosen is None:
        ver.update(decision="kept", why=f"no point of the search kept every bound ({ver['why']})")
    else:
        name, found = chosen
        point = found["accepted"]
        ver.update(
            decision="adopt",
            fit=name,
            values=point["values"],
            why=f"furthest point the gate accepts along {name} (eval {point['eval']}, loss "
            f"{point['loss']:.4f}); gate load {found['pre_load']:.3f} -> {point['load']:.3f}",
        )
    ver["attempts"] = attempts
    ver["resolved"] = True
    (d / "verdict.json").write_text(json.dumps(ver, indent=2) + "\n")
    return ver


def adopt(root: Path, only: set[str]) -> int:
    """Write every `adopt` verdict's values into the program tables; record every fit."""
    records = fits_mod.load()
    per_patch: dict[str, list[tuple[str, float]]] = {}
    per_family: dict[int, list[tuple[str, float]]] = {}
    per_drum: dict[int, list[tuple[str, float]]] = {}
    recorded = 0
    for d in sorted(p for p in root.iterdir() if (p / "verdict.json").exists()):
        ver = json.loads((d / "verdict.json").read_text())
        if only and ver["slug"] not in only:
            continue
        if ver["decision"] == "trade":
            continue
        voice = json.loads((d / "voice.json").read_text())
        report = json.loads((d / ver.get("fit", "fit.json")).read_text())
        outcome = "adopted" if ver["decision"] == "adopt" else "kept"
        if outcome == "adopted":
            knobs = [
                SimpleNamespace(tunable=k["key"], file=None, start_value=k["start"])
                for k in report["knobs"]
            ]
            values = ver.get("values") or report["selected"]["values"]
            named, drums, families, other = patch_field_assignments(knobs, values)
            for note, fields in drums.items():
                per_drum.setdefault(note, []).extend(fields)
            for patch, fields in named.items():
                per_patch.setdefault(patch, []).extend(fields)
            for family, fields in families.items():
                per_family.setdefault(family, []).extend(fields)
            if other:
                print(f"{voice['slug']}: reported only, no write site: {', '.join(other)}")
        unit, entry = fits_mod.record_fit(
            report,
            capture=voice["capture"],
            timbre=voice["timbre"],
            outcome=outcome,
            reason="" if outcome == "adopted" else ver["why"],
        )
        records[unit] = entry
        recorded += 1
    edited = write_patch_fields(per_patch)
    edited.update(write_family_fields(per_family, edited))
    edited.update(write_drum_fields(per_drum, edited))
    for path, text in edited.items():
        path.write_text(text)
        print(f"wrote {path.relative_to(REPO_ROOT)}")
    fits_mod.save(records)
    adopted = len(per_patch) + len(per_family) + len(per_drum)
    print(f"{recorded} fit(s) recorded in {fits_mod.FITS_PATH.name}, {adopted} unit(s) adopted")
    return 0


def report(root: Path) -> int:
    rows = [json.loads(p.read_text()) for p in sorted(root.glob("*/verdict.json"))]
    for r in rows:
        held = "-" if r["held_out_gain"] is None else f"{r['held_out_gain']:+.3f}"
        pre, post = r["excess"]
        print(
            f"{r['decision']:6} {r['slug']:40} moved {r['moved']:3}  held {held:>7}  "
            f"excess {pre:6.3f} -> {post:6.3f}  {r['why']}"
        )
    counts = {d: sum(r["decision"] == d for r in rows) for d in ("adopt", "kept", "trade")}
    print(f"{len(rows)} verdict(s): " + ", ".join(f"{n} {d}" for d, n in counts.items()))
    return 0


def _drain(futures: dict, jobs: int) -> int:
    """Print each voice's outcome as it lands; stop when the first round all failed alike.

    One voice's failure must not stop the rest, but a first round where every
    voice failed with the same message is the pass itself failing (a dirty
    tree, a missing build), and the remaining hours would repeat it.
    """
    first: list[str | None] = []
    for fut in as_completed(futures):
        name = futures[fut]
        try:
            r = fut.result()
            if r is not None:
                print(f"{r['decision']:6} {name}: {r['why']}", flush=True)
            first.append(None)
        except (RuntimeError, OSError, ValueError, KeyError) as exc:
            print(f"error  {name}: {exc}", flush=True)
            first.append(str(exc))
        if len(first) == max(1, jobs) and first[0] is not None and len(set(first)) == 1:
            for pending in futures:
                pending.cancel()
            print(
                f"stopping: the first {len(first)} voices all failed with: {first[0]}", flush=True
            )
            return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("cmd", choices=("plan", "run", "resolve", "report", "adopt"))
    ap.add_argument("--banks", choices=("capitals", "variations", "kits"), default="capitals")
    ap.add_argument("--only", default="", help="comma-separated voice slugs")
    ap.add_argument("--jobs", type=int, default=2, help="voices fitted at once")
    ap.add_argument("--build-dir", default="build-autofit")
    ap.add_argument("--evals-per-knob", type=int, default=EVALS_PER_KNOB)
    ap.add_argument("--out", default="", help="results directory (default: keyed by objective)")
    args = ap.parse_args()
    only = {s.strip() for s in args.only.split(",") if s.strip()}
    root = Path(args.out) if args.out else run_root(objective_digest())

    if args.cmd == "report":
        return report(root)
    if args.cmd == "adopt":
        return adopt(root, only)
    if args.cmd == "resolve":
        dirs = [p for p in sorted(root.iterdir()) if (p / "verdict.json").exists()]
        dirs = [p for p in dirs if not only or p.name in only]
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = {
                pool.submit(resolve_voice, p, args.build_dir, args.evals_per_knob): p.name
                for p in dirs
            }
            return _drain(futures, args.jobs)
    todo, skipped = plan(args.banks)
    if only:
        todo = [v for v in todo if v["slug"] in only]
    if args.cmd == "plan":
        for v in todo:
            grid = (
                f"drum note {v['drum_note']}, judged by {len(v['gates'])} gates"
                if "drum_note" in v
                else f"notes {v['notes']} held {v['validate_notes']} vel {v['velocities']}"
            )
            print(f"fit   {v['slug']:40} {v['patch']:24} {v['capture']} {grid}")
        for s in skipped:
            print(f"skip  {s['slug']:40} {s['reason']}")
        print(f"{len(todo)} to fit, {len(skipped)} skipped; results under {root}")
        return 0
    root.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {
            pool.submit(run_voice, v, root, args.build_dir, args.evals_per_knob): v["slug"]
            for v in todo
        }
        return _drain(futures, args.jobs)


if __name__ == "__main__":
    sys.exit(main())
