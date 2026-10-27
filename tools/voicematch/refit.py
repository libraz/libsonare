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
bound, or moving a field the gate cannot judge (`_held`), is a trade, and
`resolve` settles it, with those fields at their shipped values, by the furthest
point along the search the gate accepts, refitting once with the broken
dimensions weighed up when no point is.

Each voice is judged by every gate its patch reaches: a capital by the gates of
every capture resolving to its patch, a kit note by every kit gate, and readings
are keyed `<capture>:<dimension>` in both. The tuning layer applies a patch's
overrides after the tables have copied it, so every override on a patch is
repeated on each gate patch that inherits the field.

`pre` is the state before the first adoption that reached the voice, not the
tables at measurement time: each knob of an existing fit is pinned to its start,
and every gate patch is put back to its values before the trailing run of refit
commits on it that recorded a unit in the voice's struct-copy chain, read from
the git history of `bank-versions.json` and `fits.json`. `pre.json` records each
pin and its source. `rejudge` re-measures pre, post and the verdict of given
slugs over their existing fits.

Variations derive from their capital's patch at table-build time, so `--banks
capitals` (the default) refits capitals and a later `--banks variations` pass
starts from what was adopted, and `--banks kits` fits each drum note of a kit
against its machine captures. A patch shared by several voices is fitted once,
against the first voice's capture.
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import re
import subprocess
import sys
from collections.abc import Iterable, Iterator
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
    _STRUCT_COPY,
    PROGRAM_TABLE_FILES,
    _key_path,
    integer_field,
    patch_field_assignments,
    struct_copies,
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
#: `DahdsrEnvelope::kSilenceLevel`: a sustain at or under it makes the envelope end
#: itself when the decay lands, so leaving it is a branch, not a value.
SILENT_SUSTAIN = 1.0e-4
#: The tracked registries a baseline is read back from: each patch's values as the
#: library reported them, and which unit each refit recorded.
BANK_VERSIONS = "tools/bank-versions.json"
FITS_JSON = "tools/voicematch/fits.json"
_EXPLICIT_FIELD = re.compile(r"^\s*o\.(\w+)\.([\w.\[\]]+)\s*=", re.MULTILINE)


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


def capital_gates(
    row: dict,
    rows: list[dict],
    pool: list,
    reference_dir: Path,
    copies: dict[str, str] | None = None,
) -> list[dict]:
    """Every gate a melodic voice's patch is read against, its own capture's first.

    A patch answers every capture whose (program, bank) the library resolves to it —
    an alternate recording at the same address, or the patch's second bank address —
    so an adoption is judged against all of their gates, the way a kit note is judged
    against every kit gate. The resolution is the status table's, which the library's
    catalogue wrote. A patch the program tables build as a copy of this one, at any
    address, carries the fitted values too, so its captures judge as well. A capture
    whose reference is not at the instrument boundary measures something else and
    judges nothing.
    """
    copies = struct_copies() if copies is None else copies
    reached = {row["patch"]}
    while grown := {c for c, src in copies.items() if src in reached} - reached:
        reached |= grown
    patch_of = {(r["program"], r["bank"]): r.get("patch") for r in rows if not r["kit"]}
    own = row.get("capture")
    found = []
    for cap in pool:
        if cap.drums:
            continue
        here = (cap.program, cap.bank) == (row["program"], row["bank"])
        if not here and patch_of.get((cap.program, cap.bank)) not in reached:
            continue
        gate = reference_dir / f"{cap.id}_gate.json"
        if not gate.exists():
            continue
        where = boundary.reference_boundary(boundary.Reference.from_capture(cap.raw), cap.program)
        if where != boundary.BOUNDARY_INSTRUMENT:
            continue
        found.append(
            {
                "capture": cap.id,
                "gate": str(gate),
                "timbre": json.loads(gate.read_text())["timbre"],
                "patch": row["patch"] if here else patch_of[(cap.program, cap.bank)],
            }
        )
    return sorted(found, key=lambda g: g["capture"] != own)


def plan(banks: str) -> tuple[list[dict], list[dict]]:
    """(voices to fit, voices skipped with the reason)."""
    rows = json.loads(STATUS.read_text())["voices"]
    if banks == "kits":
        return plan_kits(rows)
    pool = bank.captures()
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
                "gates": capital_gates(row, rows, pool, REFERENCE_DIR),
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


def _compare_voice(
    voice: dict, out: Path, lib: Path, overrides: str = "", baseline: dict | None = None
) -> None:
    """One voice's gate readings over every gate its patch reaches, keyed by capture.

    `baseline` is recorded beside the readings, so a pinned pre says what it pinned.
    """
    if "gates" not in voice:
        _compare(voice, out, lib, overrides)
        body = json.loads(out.read_text())
    else:
        readings = []
        for g in voice["gates"]:
            part = out.with_name(f"{out.stem}.{g['capture']}.json")
            if not part.exists():
                _compare({**voice, **g}, part, lib, overrides)
            for r in json.loads(part.read_text())["gate"]["readings"]:
                readings.append({**r, "key": f"{g['capture']}:{r['key']}"})
        body = {"gate": {"readings": readings}}
    if baseline is not None:
        body["baseline"] = baseline
    out.write_text(json.dumps(body, indent=2) + "\n")


def chain_of(patch: str, copies: dict[str, str]) -> list[str]:
    """The patch, the patch it is copied from, that one's source, and so on."""
    chain = [patch]
    while chain[-1] in copies and copies[chain[-1]] not in chain:
        chain.append(copies[chain[-1]])
    return chain


def assigned_after_copy(texts: Iterable[str]) -> dict[str, frozenset[str]]:
    """Per copied patch, the key paths assigned on it by name after its `o.<copy> = o.<src>;`.

    An assignment ahead of the copy line is overwritten by the copy, so it is not the
    copy's own value.
    """
    found: dict[str, set[str]] = {}
    for text in texts:
        copied_at = {m.group(1): m.end() for m in _STRUCT_COPY.finditer(text)}
        for m in _EXPLICIT_FIELD.finditer(text):
            patch, member = m.groups()
            if patch in copied_at and m.start() > copied_at[patch]:
                found.setdefault(patch, set()).add(_key_path(member))
    return {p: frozenset(s) for p, s in found.items()}


@functools.lru_cache(maxsize=1)
def explicit_fields() -> dict[str, frozenset[str]]:
    """`assigned_after_copy` over the program tables."""
    return assigned_after_copy((REPO_ROOT / rel).read_text() for rel in PROGRAM_TABLE_FILES)


def owner(target: str, path: str, copies: dict[str, str], explicit: dict) -> str:
    """The patch whose value of `path` `target` carries: the nearest in its copy chain
    assigning the field (or an enclosing struct) by name, else the chain's root."""
    chain = chain_of(target, copies)
    for patch in chain[:-1]:
        if any(path == e or path.startswith(e + ".") for e in explicit.get(patch, ())):
            return patch
    return chain[-1]


def inherits(target: str, path: str, source: str, copies: dict[str, str], explicit: dict) -> bool:
    """Whether `target`, copied from `source` at any depth, still carries its `path`."""
    chain = chain_of(target, copies)
    if source not in chain[1:]:
        return False
    return chain.index(owner(target, path, copies, explicit)) >= chain.index(source)


def gate_patches(voice: dict) -> list[str]:
    """Every patch the voice's gates render: its own, and each copy a gate reads."""
    own = voice.get("patch")
    found = {g.get("patch") or own for g in voice.get("gates", [])} | {own}
    return sorted(p for p in found if p)


def reach_copies(
    overrides: str, voice: dict, copies: dict | None = None, explicit: dict | None = None
) -> str:
    """The overrides with each patch-field entry repeated on every gate patch inheriting it."""
    if not overrides:
        return overrides
    copies = struct_copies() if copies is None else copies
    explicit = explicit_fields() if explicit is None else explicit
    entries = [e.partition("=") for e in overrides.split(",") if e]
    named = {k for k, _, _ in entries}
    out = [f"{k}={v}" for k, _, v in entries]
    for key, _, value in entries:
        unit, _, path = key.partition(".")
        for target in gate_patches(voice):
            copied = f"{target}.{path}"
            if copied not in named and inherits(target, path, unit, copies, explicit):
                out.append(f"{copied}={value}")
                named.add(copied)
    return ",".join(out)


def _registry(rev: str | None, rel: str) -> dict:
    """A tracked registry at a commit, or in the working tree for `None`."""
    if rev is None:
        path = REPO_ROOT / rel
        return json.loads(path.read_text()) if path.exists() else {}
    shown = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "show", f"{rev}:{rel}"],
        capture_output=True,
        text=True,
        check=False,
    )
    return json.loads(shown.stdout) if shown.returncode == 0 else {}


def registry_history(units: Iterable[str]) -> Iterator[dict]:
    """Both registries for `units`, newest first: the working tree, then every
    first-parent commit that changed either."""
    units = sorted(set(units))
    log = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "log", "--first-parent", "--format=%H", "--"]
        + [BANK_VERSIONS, FITS_JSON],
        capture_output=True,
        text=True,
        check=False,
    )
    for rev in [None, *log.stdout.split()]:
        bank_units = _registry(rev, BANK_VERSIONS).get("units", {})
        fit_units = _registry(rev, FITS_JSON).get("fits", {})
        yield {
            "rev": rev or "worktree",
            "values": {u: (bank_units.get(u) or {}).get("values", {}) for u in units},
            "fingerprints": {u: (bank_units.get(u) or {}).get("fingerprint") for u in units},
            "fits": {u: fit_units.get(u) for u in units},
        }


def baseline_pins(
    voice: dict,
    own_fit: dict | None,
    history: Iterable[dict],
    copies: dict | None = None,
    explicit: dict | None = None,
) -> dict:
    """The overrides that put every gate patch back before the first adoption reaching the voice.

    For each gate patch, the commits that moved its values are walked newest first
    while each also changed the `fits.json` record of a unit in that patch's
    struct-copy chain: the trailing run of refit write-backs its shipped values
    descend from. A field moved in the run whose `owner` is in the voice's own
    chain goes back to its value before the oldest such commit; a field a copy
    further down owns is that copy's adoption and stays. Each knob of an existing
    fit is pinned to its start, repeated on the copies inheriting it, where the
    history pinned nothing. An ancestor whose adoption `fits.json` records but
    `bank-versions.json` does not yet carry is reported unresolved and left at its
    shipped values.
    """
    copies = struct_copies() if copies is None else copies
    explicit = explicit_fields() if explicit is None else explicit
    own = voice["patch"]
    undo = set(chain_of(own, copies))
    targets = gate_patches(voice)
    chains = {t: set(chain_of(t, copies)) for t in targets}
    pins: dict[str, tuple[float, str]] = {}
    open_ = set(targets)
    unresolved: list[str] = []
    newer, pairs = None, 0
    for older in history:
        if newer is None:
            newer = older
            continue
        pairs += 1
        if newer["rev"] == "worktree":
            for unit in sorted(undo - {own}):
                entry = newer["fits"].get(unit)
                if (
                    entry != older["fits"].get(unit)
                    and (entry or {}).get("outcome") == "adopted"
                    and newer["fingerprints"].get(unit) == older["fingerprints"].get(unit)
                ):
                    unresolved.append(
                        f"{unit}: fits.json records an adoption {BANK_VERSIONS} does not "
                        "carry yet; regenerate it to pin that baseline"
                    )
        for t in sorted(open_):
            if older["fingerprints"].get(t) is None:
                open_.discard(t)  # the unit did not exist yet: nothing earlier to restore
                continue
            if newer["fingerprints"].get(t) == older["fingerprints"].get(t):
                continue
            recorded = sorted(u for u in chains[t] if newer["fits"].get(u) != older["fits"].get(u))
            if not recorded:
                open_.discard(t)
                continue
            now, before = newer["values"].get(t, {}), older["values"].get(t, {})
            moved = {k for k in now.keys() | before.keys() if now.get(k) != before.get(k)}
            source = (
                f"{BANK_VERSIONS}@{older['rev'][:12]}, "
                f"before {newer['rev'][:12]} ({', '.join(recorded)})"
            )
            for path in sorted(moved):
                if path in before and owner(t, path, copies, explicit) in undo:
                    pins[f"{t}.{path}"] = (before[path], source)
        if not open_:
            break
        newer = older
    if not pairs:
        unresolved.append(f"no history of {BANK_VERSIONS}; gate patches left at shipped values")
    for knob in (own_fit or {}).get("knobs", []):
        unit, _, path = knob["key"].partition(".")
        reached = [knob["key"]] + [
            f"{t}.{path}"
            for t in targets
            if t != unit and inherits(t, path, unit, copies, explicit)
        ]
        for key in reached:
            pins.setdefault(key, (knob["start"], "fit.json start"))
    pinned = [
        {"key": k, "value": format_value(v), "source": s} for k, (v, s) in sorted(pins.items())
    ]
    return {
        "overrides": ",".join(f"{p['key']}={p['value']}" for p in pinned),
        "pinned": pinned,
        "unresolved": unresolved,
    }


#: Forwarded to every autofit run; set from --allow-dirty-src in main().
ALLOW_DIRTY_SRC = False


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
    if ALLOW_DIRTY_SRC:
        cmd.append("--allow-dirty-src")
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


def unmeasured(voice: dict) -> frozenset[str]:
    """Field suffixes none of the voice's gates can see move.

    A release acts after the key lift, which only the `damper` dimension reads; a
    voice no gate of which declares it has its releases settled by nothing, so a
    fit moving one moves it blind.
    """
    for gate in voice.get("gates") or [{"capture": voice.get("capture")}]:
        path = CAPTURE_DIR / f"{gate['capture']}.json"
        if path.exists() and "damper" in json.loads(path.read_text()).get("dimensions", []):
            return frozenset()
    return frozenset({"release_ms"})


def _held(knob: dict, value: float, held: frozenset[str] = frozenset()) -> bool:
    """Whether the gate cannot judge this knob at `value`, so it stays at its start.

    A typed switch and a count by their bucket — a search lands on one, not on a
    number, and a count adds or removes a resonator the gate window may never
    reach; an envelope's sustain by whether it still ends the note on its own,
    since a one-shot that gains one rings until a note-off a drum never sends; and
    a field in `held`, which no gate of this voice measures.
    """
    path = knob["key"].partition(".")[2]
    if switch_field(path) or integer_field(path):
        return round(value) != round(knob["start"])
    if path.endswith("amp_env.sustain"):
        return (knob["start"] <= SILENT_SUSTAIN) != (value <= SILENT_SUSTAIN)
    if any(path.endswith(suffix) for suffix in held):
        return format_value(value) != format_value(knob["start"])
    return False


def switch_moves(report: dict, held: frozenset[str] = frozenset()) -> list[str]:
    return [
        knob["key"]
        for knob, value in zip(report["knobs"], report["selected"]["values"])
        if _held(knob, value, held)
    ]


def verdict(
    pre: dict,
    fit: dict,
    post: dict,
    *,
    gate_only: bool = False,
    unjudged: frozenset[str] = frozenset(),
) -> dict:
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
        "switches": switch_moves(fit, unjudged),
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
        out["decision"], out["why"] = (
            "trade",
            f"a field the gate cannot judge moved: {', '.join(out['switches'])}",
        )
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
        own_fit = json.loads(fit.read_text()) if fit.exists() else None
        units = {u for t in gate_patches(voice) for u in chain_of(t, struct_copies())}
        base = baseline_pins(voice, own_fit, registry_history(units))
        _compare_voice(voice, pre, lib, base["overrides"], baseline=base)
    if not fit.exists():
        _fit(voice, fit, build_dir, per_knob)
    fit_report = json.loads(fit.read_text())
    if not post.exists():
        _compare_voice(voice, post, lib, reach_copies(fit_report.get("overrides") or "", voice))
    result = {
        "slug": voice["slug"],
        **verdict(
            json.loads(pre.read_text()),
            fit_report,
            json.loads(post.read_text()),
            gate_only=bool(voice.get("gate_only")),
            unjudged=unmeasured(voice),
        ),
    }
    ver.write_text(json.dumps(result, indent=2) + "\n")
    return result


def writable(report: dict, values: list[float], held: frozenset[str] = frozenset()) -> list[float]:
    """The values as the write-back would land them: a switch is never written, and a
    value the gate cannot judge is held, so both stay at their shipped values."""
    return [
        k["start"] if switch_field(k["key"].partition(".")[2]) or _held(k, v, held) else v
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
        values = writable(report, path[i]["values"], unmeasured(voice))
        out = d / f"path_{tag}_{path[i]['eval']}.json"
        if not out.exists():
            _compare_voice(voice, out, lib, reach_copies(overrides_for(report, values), voice))
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
    "vel_range": "velspan",
    "onset": "onset",
    "onset_settle": "onset",
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
    # Readings are keyed `<capture>:<dimension>`.
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


def resolve_voice(d: Path, build_dir: str, per_knob: int, *, refit: bool = True) -> dict | None:
    """Settle a trade by the furthest point along the search the gate accepts.

    Failing that on the fit itself, the fit is repeated once with the terms carrying
    the broken bounds weighed up, and its own path is searched the same way. With
    neither, the shipped values are kept. Without `refit`, a repeat not already on
    disk is not run and the trade stays open for `resolve`.
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
            if not fit_path.exists() and not refit:
                attempts.append({"fit": "fit_r1.json", "weights": weights, "pending": True})
                ver["attempts"] = attempts
                (d / "verdict.json").write_text(json.dumps(ver, indent=2) + "\n")
                return ver
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


#: A voice's judged state, re-created by `rejudge` from the fits on disk.
_JUDGED = ("pre.", "post.", "path_")


def rejudge_voice(voice: dict, root: Path, build_dir: str) -> dict:
    """Re-measure one voice's pre, post and verdict over the fits already on disk.

    A trade is settled along those fits' paths; one that needs the reweighed repeat
    is left open for `resolve`.
    """
    d = root / voice["slug"]
    if not (d / "fit.json").exists():
        raise RuntimeError(f"{voice['slug']}: no fit.json under {root} to re-judge")
    ver = d / "verdict.json"
    was = json.loads(ver.read_text())["decision"] if ver.exists() else "unjudged"
    for path in d.iterdir():
        if path.name.startswith(_JUDGED) or path == ver:
            path.unlink()
    result = run_voice(voice, root, build_dir, per_knob=0)
    if result["decision"] == "trade":
        result = resolve_voice(d, build_dir, 0, refit=False) or result
    why = f"(was {was}) {result['why']}"
    if (result.get("attempts") or [{}])[-1].get("pending"):
        why += "; the reweighed fit is not on disk, run resolve"
    return {**result, "why": why}


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
    ap.add_argument("cmd", choices=("plan", "run", "rejudge", "resolve", "report", "adopt"))
    ap.add_argument("--banks", choices=("capitals", "variations", "kits"), default="capitals")
    ap.add_argument("--only", default="", help="comma-separated voice slugs")
    ap.add_argument("--jobs", type=int, default=2, help="voices fitted at once")
    ap.add_argument("--build-dir", default="build-autofit")
    ap.add_argument("--evals-per-knob", type=int, default=EVALS_PER_KNOB)
    ap.add_argument("--out", default="", help="results directory (default: keyed by objective)")
    ap.add_argument(
        "--allow-dirty-src",
        action="store_true",
        help="pass autofit's --allow-dirty-src: fit against an uncommitted src/ edit of your own",
    )
    args = ap.parse_args()
    global ALLOW_DIRTY_SRC
    ALLOW_DIRTY_SRC = args.allow_dirty_src
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
    if args.cmd == "rejudge":
        missing = sorted(only - {v["slug"] for v in todo})
        if not only or missing:
            print(f"rejudge takes --only slugs this --banks plan lists; not planned: {missing}")
            return 2
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            futures = {pool.submit(rejudge_voice, v, root, args.build_dir): v["slug"] for v in todo}
            return _drain(futures, args.jobs)
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
