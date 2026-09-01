"""Where every voice in the bank stands, as one number and the reasons for it.

The bank is the master. A capture, a reference profile, a gate and a calibration
candidate are all attachments to a bank entry, and each was readable only
through the tool that produced it — `profile.py status --all` covers the four
instruments a capture exists for and says nothing about the other 124, which is
the half of the bank where the next round's work actually is.

So this walks all of it and writes `tools/voice-status.json`, which is committed
and is what the CLI table and the audition page's bank map both read. Generating
it needs a `-DBUILD_TUNING=ON` library, since the engine voicing each patch is
reported by the library rather than parsed out of it; reading it needs nothing,
which is why the generated file is tracked rather than produced on demand.

## The stage

One number per voice, in fifths, each step a predicate over facts already on
disk rather than a weighting anyone chose:

    0.0  untouched   no deliberate patch: a family default on the subtractive engine
    0.2  voiced      a deliberate engine and patch answer it
    0.4  measured    two or more reference timbres, a measured profile, a current gate
    0.6  covered     every canonical dimension gated, or excused with a reason
    0.8  agreeing    most gated dimensions sit inside the reference's own spread
    1.0  settled     no structural residual, and the musical take signed off

Two properties are deliberate. The first is that **a stage is a floor, not a
score**: a voice sits at the highest step whose predicate holds, and an open
write-back candidate is a badge rather than a demotion, because a candidate
nobody has adopted means there may be more to gain — not that what shipped is
worse than it was.

The second is that **coverage is all-or-nothing** (0.6). A canonical dimension
is gated, or it is named in the capture's `dimensions_na` with a reason, or it
is a gap; there is no fraction to tune and no majority to argue about. The
piano's two exclusions were already argued in prose and are now data, which is
the difference between an exclusion and an oversight.

`agreeing` compares each gated bound against the spread of the references
against *each other*. A voice inside that spread is as close to the instrument
as two presets of the instrument are to one another, which is the strongest
claim this harness can make and the reason it is the last step before the two
that need a human.

The two claims 1.0 needs are the two nothing on disk implies — a diagnosis of
what calibration cannot reach, and somebody's word that a take is the
instrument — so they are recorded by hand in `signoff.json`, keyed by the same
slug everything else here is. Both expire with the bank they were taken
against, and `signoff` tells the two ways they expire apart.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import bank  # noqa: E402
import catalogue as catalogue_mod  # noqa: E402
import signoff  # noqa: E402
from _repo import REPO_ROOT  # noqa: E402
from toneclass import canonical_dimensions  # noqa: E402

HERE = Path(__file__).resolve().parent
REFERENCE_DIR = HERE / "reference"
CALIBRATIONS = HERE / "calibrations.json"
OUT_PATH = REPO_ROOT / "tools" / "voice-status.json"
BANK_VERSIONS = REPO_ROOT / "tools" / "bank-versions.json"

#: The step names, low to high. The index is the stage in fifths.
STAGES = ("untouched", "voiced", "measured", "covered", "agreeing", "settled")

#: The engine a program falls to when nothing chose one for it. Deliberate for a
#: synth lead and a default everywhere else, which is why the untouched
#: predicate needs the patch as well: `tremolo_strings` and `orchestra_hit` are
#: subtractive on purpose, while `fam10` through `fam15` are eight synth programs
#: sharing one patch nobody has voiced apart.
DEFAULT_ENGINE = "subtractive"

#: A patch named `famN` is the family fallback rather than a voice written for
#: an instrument. On a physical engine that is still a deliberate choice — `fam0`
#: is the piano family on the piano engine — so it only reads as untouched
#: together with the default engine.
FAMILY_PATCH_PREFIX = "fam"


def engine_for(voice, catalogue) -> str | None:
    """The engine voicing a bank entry.

    A kit is not its program's melodic patch: on channel 10 the program selects
    the kit and the note selects the instrument, so the engine belongs to the
    drum notes and asking the program map gives whatever melodic voice shares
    the number — program 0 answers `piano`, which is exactly wrong.
    """
    if catalogue is None:
        return None
    if not voice.kit:
        return catalogue.mode_for(voice.program, voice.bank)
    modes = {m for k, m in catalogue.modes.items()
             if k.startswith("d") and k[1:].isdigit()}
    if not modes:
        return None
    return modes.pop() if len(modes) == 1 else "mixed"


def _load(path: Path) -> dict:
    return json.loads(path.read_text()) if path.is_file() else {}


def _display(path: Path) -> str:
    """A path as it reads in a message: repo-relative where it is under the repo."""
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def open_candidates() -> dict[str, list[str]]:
    """Each voice's recorded-but-unadopted calibration settings, by slug."""
    out: dict[str, list[str]] = {}
    for slug, entry in _load(CALIBRATIONS).items():
        if slug.startswith("_"):
            continue
        out[slug] = [v["name"] for v in entry.get("variants", [])]
    return out


def gate_agreement(gate: dict) -> dict:
    """How many gated dimensions sit inside the references' own spread.

    A bound is compared against `reference_spread`, the distance between the
    reference timbres on that dimension. Where the capture has one timbre the
    spread is empty and no dimension can be adjudicated at all, which is a
    different answer from "none of them agree" and is reported as such.

    The margin comes back off the bound first. A bound is the measurement times
    the gate's slack, and that slack is there so a regression guard survives
    measurement noise — it says nothing about how close the voice is. Left in,
    it would make a voice have to beat the references by the slack before it
    counted as level with them, and would make every stage on this ladder move
    when a gate is re-recorded at a different margin with no audio changing.
    """
    spread = gate.get("reference_spread") or {}
    bounds = gate.get("bounds") or {}
    if not spread:
        return {"inside": 0, "total": 0, "unjudgeable": sorted(bounds)}
    margin = float(gate.get("margin") or 1.0) or 1.0
    inside, outside = [], {}
    for dim, b in bounds.items():
        if dim not in spread:
            continue
        measured = b["median"] / margin
        if measured <= spread[dim] * 1.0001:
            inside.append(dim)
        else:
            outside[dim] = round(measured / spread[dim], 2)
    return {
        "inside": len(inside),
        "total": len(inside) + len(outside),
        "outside": dict(sorted(outside.items(), key=lambda kv: -kv[1])),
    }


def coverage(voice, cap_raw: dict, gate: dict) -> dict:
    """Which of this class's dimensions are gated, excused, or missing."""
    canon = canonical_dimensions(voice.program, percussive=voice.kit)
    gated = set(gate.get("bounds") or {})
    excused = dict(cap_raw.get("dimensions_na") or {})
    gaps = [d for d in canon if d not in gated and d not in excused]
    return {
        "canonical": len(canon),
        "gated": len([d for d in canon if d in gated]),
        "excused": sorted(excused),
        "gaps": gaps,
        "complete": not gaps,
    }


def profile_facts(ident: str) -> dict:
    """What the committed reference profile and gate say about an instrument."""
    profile = _load(REFERENCE_DIR / f"{ident}.json")
    gate = _load(REFERENCE_DIR / f"{ident}_gate.json")
    stale = None
    if gate:
        against = gate.get("reference_measured_utc")
        if against is None:
            stale = "unknown"
        elif against != profile.get("measured_utc"):
            stale = "stale"
        else:
            stale = "current"
    return {
        "profile_rows": len(profile.get("rows") or []),
        "measured_utc": profile.get("measured_utc", ""),
        "gate_recorded": bool(gate),
        "gate_state": stale,
        "gate_timbre": gate.get("timbre", ""),
        "_gate": gate,
    }


def stage_for(axes: dict) -> int:
    """The highest step whose predicate holds. See the module docstring."""
    untouched = (not axes["engine"]
                 or (axes["engine"] == DEFAULT_ENGINE
                     and (axes["patch"] or "").startswith(FAMILY_PATCH_PREFIX)))
    if untouched:
        return 0
    if not (axes["timbres"] >= 2 and axes["profile_rows"] > 0
            and axes["gate_state"] == "current"):
        return 1
    if not axes["coverage"]["complete"]:
        return 2
    agree = axes["agreement"]
    if not (agree["total"] and agree["inside"] * 2 > agree["total"]):
        return 3
    # `settled` needs both claims recorded, both still current against the bank,
    # and every unreachable term accepted. Unknown is not satisfied.
    if not signoff.settled(axes["structure"], axes["music"]):
        return 4
    return 5


def next_action(axes: dict, stage: int, candidates: list[str]) -> str:
    """The one move that would raise this voice's stage, in a line."""
    if stage == 0:
        return "no deliberate voice: pick an engine and write a patch"
    if stage == 1:
        if axes["timbres"] == 0:
            return "capture an oracle: no reference exists for this voice"
        if axes["timbres"] < 2:
            return ("capture a second reference timbre: with one, no dimension "
                    "can be judged against the references' own spread")
        if not axes["profile_rows"]:
            return "measure the captured corpus into a reference profile"
        if axes["gate_state"] != "current":
            return f"the gate is {axes['gate_state']}: re-record it against the current profile"
    if stage == 2:
        gaps = axes["coverage"]["gaps"]
        return (f"{len(gaps)} canonical dimension(s) neither gated nor excused: "
                f"{', '.join(gaps)}")
    if stage == 3:
        out = axes["agreement"].get("outside") or {}
        worst = ", ".join(f"{d} {r}x" for d, r in list(out.items())[:3])
        return f"outside the reference spread: {worst}"
    if stage == 4:
        rest = _last_step(axes)
        if candidates:
            return f"judge the recorded candidate(s) — {', '.join(candidates)} — then {rest}"
        return rest
    return "settled"


def _last_step(axes: dict) -> str:
    """Which half of the last step is missing, and why.

    The two claims fail in four ways between them and they need different work,
    so naming the half is the whole value of the sentence: re-running a
    diagnosis and listening to a take are not interchangeable.
    """
    structure, music = axes["structure"], axes["music"]
    if structure is None:
        return "record a structural residual with autofit --diagnose"
    if structure["state"] == signoff.STALE:
        return "the patch has moved since the diagnosis: re-run autofit --diagnose"
    if structure["state"] == signoff.UNVERIFIED:
        return ("a shared calibration unit has moved since the diagnosis and nothing can "
                "attribute it: re-run autofit --diagnose")
    if structure["open"]:
        return (f"{len(structure['open'])} term(s) no knob reaches and nobody has accepted: "
                f"{', '.join(structure['open'])}")
    if music is None:
        return "listen to a take and sign it off in signoff.json"
    if music["state"] != signoff.CURRENT:
        return f"the sign-off is {music['state']} against this bank: listen again"
    return "settled"


def build(catalogue) -> list[dict]:
    """One entry per bank voice, in program order with kits last."""
    pool = bank.captures()
    kits = sorted({c.program for c in pool if c.drums}) or [0]
    voices = bank.voices(catalogue=catalogue, kits=kits)
    cands = open_candidates()
    claims = signoff.load()
    generation, unit_versions = signoff.bank_versions(BANK_VERSIONS)
    rows = []
    for v in voices:
        cap = v.capture
        facts = profile_facts(cap.id) if cap else {
            "profile_rows": 0, "measured_utc": "", "gate_recorded": False,
            "gate_state": None, "gate_timbre": "", "_gate": {},
        }
        gate = facts.pop("_gate")
        claim = claims.get(v.slug, signoff.Record())
        # A kit's voices are its drum notes, so it has no single patch unit and
        # only the bank generation can date a claim about it.
        patch_version = unit_versions.get(v.patch or "", 0)
        axes = {
            "engine": engine_for(v, catalogue),
            "patch": v.patch or None,
            "timbres": len(cap.timbres) if cap else 0,
            "profile_rows": facts["profile_rows"],
            "gate_state": facts["gate_state"],
            "coverage": coverage(v, cap.raw if cap else {}, gate),
            "agreement": gate_agreement(gate),
            "structure": signoff.axis(claim.structure, generation, patch_version),
            "music": signoff.axis(claim.music, generation, patch_version),
        }
        stage = stage_for(axes)
        open_here = cands.get(v.slug, [])
        rows.append({
            "slug": v.slug,
            "program": v.program,
            "bank": v.bank,
            "kit": v.kit,
            "name": v.name,
            "group": v.group,
            "tone_class": v.tone.value,
            "patch": v.patch or None,
            "engine": axes["engine"],
            "capture": cap.id if cap else None,
            "stage": stage / 5.0,
            "stage_name": STAGES[stage],
            "open_candidates": open_here,
            "axes": axes,
            "next": next_action(axes, stage, open_here),
        })
    return rows


def render_table(rows: list[dict], *, every: bool) -> None:
    """The CLI view: grouped by GM family, one line per voice."""
    shown = rows if every else [r for r in rows if r["stage"] > 0.2 or r["open_candidates"]]
    if not shown:
        print("  nothing past stage 0.2 — pass --all for the whole bank")
        return
    group = None
    for r in shown:
        if r["group"] != group:
            group = r["group"]
            print(f"\n  {group}")
        bar = "#" * int(r["stage"] * 5) + "." * (5 - int(r["stage"] * 5))
        flag = f"  [{len(r['open_candidates'])} unwritten]" if r["open_candidates"] else ""
        oracle = r["capture"] or "-"
        print(f"    {r['slug']:<32} {r['stage']:.1f} {bar}  {r['engine'] or '?':<15}"
              f" {oracle:<10}{flag}".rstrip())
        # Only past the oracle step, where the line differs per voice. Below it
        # every voice says the same sentence, and 150 copies of it bury the four
        # that say something.
        if 0.2 < r["stage"] < 1.0:
            print(f"      -> {r['next']}")
    total = len(rows)
    counts: dict[str, int] = {}
    for r in rows:
        counts[r["stage_name"]] = counts.get(r["stage_name"], 0) + 1
    print(f"\n  {total} voices: "
          + ", ".join(f"{n} {s}" for s, n in
                      sorted(counts.items(), key=lambda kv: STAGES.index(kv[0]))))
    no_oracle = [r for r in rows if not r["capture"]]
    if no_oracle:
        print(f"  {len(no_oracle)} with no oracle captured — that is the task list, "
              f"and nothing below stage 0.4 moves without one")
    unwritten = sum(len(r["open_candidates"]) for r in rows)
    if unwritten:
        print(f"  {unwritten} recorded calibration setting(s) not written back")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", default=None,
                    help="a -DBUILD_TUNING=ON library; without one the engine "
                         "column is blank and every voice reads as untouched")
    ap.add_argument("--write", action="store_true",
                    help=f"regenerate {_display(OUT_PATH)}")
    ap.add_argument("--check", action="store_true",
                    help="fail if the generated file is stale")
    ap.add_argument("--all", action="store_true",
                    help="print every voice, not only those past stage 0.2")
    ap.add_argument("--sr", type=int, default=48000)
    args = ap.parse_args()

    if args.write or args.check:
        catalogue = catalogue_mod.dump_catalogue(0, "sustain", args.lib, sr=args.sr)
        rows = build(catalogue)
        payload = json.dumps({"voices": rows}, indent=2, ensure_ascii=False) + "\n"
        if args.check:
            if not OUT_PATH.is_file():
                print(f"{OUT_PATH}: missing — run `make voice-status-refresh`")
                return 1
            if OUT_PATH.read_text() != payload:
                print(f"{OUT_PATH}: stale — run `make voice-status-refresh`")
                return 1
            print(f"{_display(OUT_PATH)}: current ({len(rows)} voices)")
            return 0
        OUT_PATH.write_text(payload)
        print(f"{_display(OUT_PATH)}: {len(rows)} voices")
        render_table(rows, every=args.all)
        return 0

    # The reading path needs no build: the generated file is committed precisely
    # so a plain clone can see where the bank stands.
    if not OUT_PATH.is_file():
        print(f"{OUT_PATH}: missing — run `make voice-status-refresh` "
              f"(needs a -DBUILD_TUNING=ON build)")
        return 0
    render_table(_load(OUT_PATH)["voices"], every=args.all)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
