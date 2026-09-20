"""Where every voice in the bank stands, as one number and the reasons for it.

The bank is the master. A capture, a reference profile, a gate and a calibration
candidate are all attachments to a bank entry, and each was readable only
through the tool that produced it — `profile.py status --all` covers the
instruments a capture exists for and says nothing about the voices no capture
covers, which is where the next round's work usually is.

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
    0.4  targeted    a reference exists and a profile has been measured from it
    0.6  fitted      a current gate, over every canonical dimension
    0.8  heard       somebody listened and it is the instrument
    1.0  settled     and calibration reaches everything the model is asked for

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

**One reference is enough, and the ear is what promotes a voice.** Both follow
from `docs/objective.md`, which is the contract this ladder implements: the
reference is the target rather than a sample of a hidden truth, so a second
timbre is not required, and a green gate is not acceptance because voices have
passed every recorded bound while sounding wrong. `agreement` — each gated
bound against the spread of the references against *each other* — is still
computed and printed where a capture happens to carry several timbres, as
information about how much the target itself wobbles. It decides nothing.

So a gate means the mechanical work is done and locked, `heard` means somebody
said the take is the instrument, and the two claims live by hand in
`signoff.json` keyed by the same slug everything else here is. Both expire with
the bank they were taken against, and `signoff` tells the two ways they expire
apart. `settled` adds the other claim: a diagnosis of what calibration cannot
reach, with every unreachable term accepted — an open one is a missing
mechanism in the model rather than a limit, and naming it is what turns "nobody
has looked" into work.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import bank
import catalogue as catalogue_mod
import signoff
from _repo import REPO_ROOT
from toneclass import canonical_dimensions

HERE = Path(__file__).resolve().parent
REFERENCE_DIR = HERE / "reference"
CALIBRATIONS = HERE / "calibrations.json"
POLICY = HERE / "policy.json"
OUT_PATH = REPO_ROOT / "tools" / "voice-status.json"
BANK_VERSIONS = REPO_ROOT / "tools" / "bank-versions.json"

#: The step names, low to high. The index is the stage in fifths.
STAGES = ("untouched", "voiced", "targeted", "fitted", "heard", "settled")

#: The step at which a capital's timbre has been accepted by ear, and so the
#: step at which a variation under it is worth capturing. `variations_follow_
#: capital` is about a capital that still moves invalidating its variations;
#: below `heard` nobody has said the capital is the instrument yet, and above it
#: what remains is a diagnosis rather than a re-voicing.
HEARD_STAGE = STAGES.index("heard") / 5.0

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
        if slug.startswith(signoff.DOC_PREFIX):
            continue
        out[slug] = [v["name"] for v in entry.get("variants", [])]
    return out


def tier_of(pol: dict, program: int, bank: int, *, kit: bool) -> tuple[int, str]:
    """The working-priority tier a slot falls in, as (rank, name).

    Read at print time rather than written into `tools/voice-status.json`,
    because a tier is a decision and that file holds what the library and the
    references reported. Baking it in would make an ordering change need a
    `-DBUILD_TUNING=ON` rebuild to take effect, and would make the generated
    file stale every time the policy moved with no voice having changed.

    A variation is ranked with its capital rather than on its own: it is the
    capital copied and narrowed, so it cannot be worked before the capital and
    has no priority of its own. `variations_follow_capital` records that the
    tier is deliberately shared, and the caller is what defers the work.

    `kit` says which of two number spaces `program` is in and has no default. A
    kit is named by the rhythm-part program a file selects it with, so `kits`
    and `programs` are separate namespaces over the same integers: read as one,
    a tier listing kit 8 also ranks the celesta.
    """
    tiers = pol.get("tiers") or []
    if not tiers:
        # No policy loaded at all. Rank 0 rather than 1: the caller renders it as
        # `t-`, where a number would read as the top tier and quietly report the
        # whole bank — the long tail included — as the most urgent thing to work
        # on, which is the one answer a missing file must not be able to give.
        return 0, "unranked"
    fallback = (len(tiers) + 1, "unranked")
    for t in tiers:
        if t.get("default"):
            fallback = (int(t.get("rank", fallback[0])), t["name"])
            continue
        if program in set(t.get("kits" if kit else "programs") or []):
            return int(t["rank"]), t["name"]
    return fallback


def goal_members(goal: dict, rows: list[dict]) -> list[dict]:
    """The rows a goal names.

    A goal names capital tones and kits, never a variation: a variation is not
    something a file is owed, and one listed here would block the goal on a
    capture nobody has a source for.
    """
    progs = set(goal.get("programs") or [])
    kits = set(goal.get("kits") or [])
    return [r for r in rows if not r["bank"]
            and (r["program"] in kits if r["kit"] else r["program"] in progs)]


def goal_progress(pol: dict, rows: list[dict]) -> list[dict]:
    """Each goal's members and how many of them have reached its step.

    Reported, never enforced. Calibrating a voice is open-ended analog work, so
    a version whose date arrives with a goal unmet ships with it unmet and the
    goal carries over; nothing built on this may block a release, a merge or a
    CI run, and a goal named after a version is not thereby a due date.

    A `_`-prefixed key documents the block rather than naming a goal, the same
    as in `calibrations.json`, `signoff.json` and every capture definition. It
    is skipped here rather than read as a goal with no stage: the convention
    holds everywhere else in this tree, so a note written into this block is
    what a reader would expect to be able to do.
    """
    out = []
    for name, goal in sorted((pol.get("goals") or {}).items()):
        if name.startswith(signoff.DOC_PREFIX):
            continue
        want = float(goal.get("stage", 1.0))
        here = goal_members(goal, rows)
        out.append({
            "name": name,
            "stage": want,
            "total": len(here),
            "met": len([r for r in here if r["stage"] >= want]),
            "short": [r for r in here if r["stage"] < want],
        })
    return out


def variation_split(rows: list[dict], pol: dict) -> tuple[int, int]:
    """Variations below `heard`, split by whether their capital is heard yet.

    The two need different work and the flat queue says neither. One is a
    capture nobody should start — the capital it copies is still moving — and
    the other is a capture ready to run.
    """
    waiting = ready = 0
    for row in rows:
        if row["stage"] >= HEARD_STAGE:
            continue
        capital = capital_of(row, rows, pol)
        if capital is None:
            continue
        if capital["stage"] < HEARD_STAGE:
            waiting += 1
        else:
            ready += 1
    return waiting, ready


def signoff_census(rows: list[dict]) -> dict:
    """How many of the two hand-written claims are recorded, and how many hold.

    Reported, never enforced, and for the same reason as `goal_progress`: this
    says where the bank's recorded work is, not whether anything may ship.

    The two claims are read a step apart — the musical one promotes a voice to
    `heard` and the structural one carries it to `settled` — so a structural
    diagnosis taken before anyone listened raises no stage at all, and goes on
    ageing against the bank it was measured on. `structure_only` is that count.
    Nothing else surfaces it: the stage column shows where a voice stopped and
    not what has been recorded past the step it stopped at.
    """
    out = {"structure": 0, "structure_current": 0,
           "music": 0, "music_current": 0, "structure_only": 0}
    for r in rows:
        axes = r.get("axes") or {}
        structure, music = axes.get("structure"), axes.get("music")
        for claim, key in ((structure, "structure"), (music, "music")):
            if not claim:
                continue
            out[key] += 1
            if claim["state"] == signoff.CURRENT:
                out[f"{key}_current"] += 1
        if structure and not music:
            out["structure_only"] += 1
    return out


def gate_agreement(gate: dict) -> dict:
    """How many gated dimensions sit inside the references' own spread.

    A bound is compared against `reference_spread`, the distance between the
    reference timbres on that dimension. Where the capture has one timbre the
    spread is empty and no dimension can be adjudicated at all, which is a
    different answer from "none of them agree" and is reported as such.

    A spread of zero is the same answer for one dimension: the references agree
    to finer than the metric resolves, so there is no width to read a bound
    against and the ratio has no denominator. A tonewheel organ's arrival is one
    -- both registrations speak inside a single 5 ms envelope hop.

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
    inside, outside, unjudgeable = [], {}, []
    for dim, b in bounds.items():
        if dim not in spread:
            continue
        if spread[dim] <= 0.0:
            unjudgeable.append(dim)
            continue
        measured = b["median"] / margin
        if measured <= spread[dim] * 1.0001:
            inside.append(dim)
        else:
            outside[dim] = round(measured / spread[dim], 2)
    out = {
        "inside": len(inside),
        "total": len(inside) + len(outside),
        "outside": dict(sorted(outside.items(), key=lambda kv: -kv[1])),
    }
    if unjudgeable:
        out["unjudgeable"] = sorted(unjudgeable)
    return out


def merged_agreement(gates: list[dict]) -> dict:
    """`gate_agreement` over every gate answering one voice.

    Folded per gate rather than over one merged gate, because a bound is read
    against its own gate's `reference_spread` and divided by its own `margin`.
    Pooling the bounds first would read each against whichever gate's spread
    happened to land in the merged dict.
    """
    inside = total = 0
    outside: dict = {}
    unjudgeable: list[str] = []
    for gate in gates:
        one = gate_agreement(gate)
        inside += one["inside"]
        total += one["total"]
        # A gate whose capture has one timbre has no spread to adjudicate
        # against, and reports no `outside` at all rather than an empty one.
        outside.update(one.get("outside") or {})
        unjudgeable.extend(one.get("unjudgeable") or [])
    out = {
        "inside": inside,
        "total": total,
        "outside": dict(sorted(outside.items(), key=lambda kv: -kv[1])),
    }
    if unjudgeable:
        out["unjudgeable"] = sorted(set(unjudgeable))
    return out


def coverage(voice, cap_raws: list[dict], gates: list[dict]) -> dict:
    """Which of this class's dimensions are gated, excused, or missing.

    Read across every capture answering the voice, because a voice is answered
    by as many as its axes take: the standard kit's colour and ring bounds sit
    on the module grids while `vel_range` stays on the sampled kit, and asking
    any one of the four reports the other eleven bounds as absent.

    A dimension gated anywhere is gated, whatever a second capture says about
    it. An excuse is a statement about the source that carries it — `drums.json`
    excuses the colour it handed to the module grids — so it settles a dimension
    only where nothing gates it.

    **There are TWO registers a reason can be written in and they say different
    things**, so both are read and they are reported apart. A capture's
    `dimensions_na` says the source cannot carry the dimension at all; a gate's
    `_unbounded` says the source carries it and no bound could be recorded from
    this comparison, which is where a live disagreement between model and
    reference gets written down. Reading only the first reported every such
    entry as an oversight — the state its own prose had already ruled out.
    """
    canon = canonical_dimensions(voice.program, percussive=voice.kit)
    gated = {d for gate in gates for d in (gate.get("bounds") or {})}
    excused = {d for raw in cap_raws for d in (raw.get("dimensions_na") or {})}
    unbounded = {d for gate in gates for d in (gate.get("_unbounded") or {})}
    excused -= gated
    unbounded -= gated | excused
    gaps = [d for d in canon if d not in gated and d not in excused
            and d not in unbounded]
    out = {
        "canonical": len(canon),
        "gated": len([d for d in canon if d in gated]),
        "excused": sorted(excused),
        "gaps": gaps,
        # The two registers part company here, and this is the whole reason they
        # are read apart. `dimensions_na` is permanent — the source does not
        # carry the dimension and no later run will change that — so it
        # completes coverage. `_unbounded` says this comparison recorded no
        # bound, which covers both "nothing more can be measured" and "the model
        # has nothing there yet"; the prose says which and no reader can. So it
        # stops the dimension reading as an oversight and does not let the voice
        # claim a coverage it does not have.
        "complete": not gaps and not (unbounded & set(canon)),
    }
    if unbounded & set(canon):
        out["unbounded"] = sorted(unbounded & set(canon))
    return out


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


def merged_facts(every: list[dict]) -> dict:
    """`profile_facts` of every capture answering one voice, as one reading.

    `gate_state` takes the worst of the gates that exist, since the coverage
    claim rests on all of them at once and a stale bound anywhere makes part of
    it stale. **A capture carrying no gate is not a stale one**: it contributes
    no bound, so it neither completes the coverage nor spoils it, and a grid
    added before its gate is recorded must not knock a voice off the ladder.
    """
    if not every:
        return {"profile_rows": 0, "measured_utc": "", "gate_recorded": False,
                "gate_state": None, "gate_timbre": ""}
    states = {f["gate_state"] for f in every if f["gate_state"]}
    worst = next((s for s in ("unknown", "stale", "current") if s in states), None)
    return {
        # The representative's, for the two that describe one reading rather
        # than the set: which reference the page plays and when it was taken.
        "measured_utc": every[0]["measured_utc"],
        "gate_timbre": every[0]["gate_timbre"],
        "profile_rows": sum(f["profile_rows"] for f in every),
        "gate_recorded": any(f["gate_recorded"] for f in every),
        "gate_state": worst,
    }


def stage_for(axes: dict) -> int:
    """The highest step whose predicate holds. See the module docstring."""
    untouched = (not axes["engine"]
                 or (axes["engine"] == DEFAULT_ENGINE
                     and (axes["patch"] or "").startswith(FAMILY_PATCH_PREFIX)))
    if untouched:
        return 0
    # One timbre is a target, not half a measurement: see `docs/objective.md`.
    if not (axes["timbres"] >= 1 and axes["profile_rows"] > 0):
        return 1
    if not (axes["gate_state"] == "current" and axes["coverage"]["complete"]):
        return 2
    music = axes["music"]
    if not (music and music["state"] == signoff.CURRENT):
        return 3
    # `settled` needs both claims recorded, both still current against the bank,
    # and every unreachable term accepted. Unknown is not satisfied.
    if not signoff.settled(axes["structure"], axes["music"]):
        return 4
    return 5


def approximation(pol: dict, slug: str) -> dict | None:
    """The policy's record that this slot is answered by a neighbour, or None.

    An approximated slot has no oracle to capture and never will: the bank does
    not have the mechanism and is deliberately answering with the nearest voice
    it does have. Nothing downstream can tell that apart from an unfinished
    voice, so without this the slot's next action reads `capture an oracle`,
    which is precisely the work that is never going to happen for it.
    """
    entry = (pol.get("approximated") or {}).get(slug)
    return entry if isinstance(entry, dict) else None


def capital_of(row: dict, rows: list[dict], pol: dict) -> dict | None:
    """The bank-0 row a variation is a copy of, or None where the rule is off.

    Gated on `variations_follow_capital` rather than assumed. That flag is the
    decision that a variation has no priority and no schedule of its own, and a
    rule read from nowhere is a rule the policy cannot turn off — which is also
    what would let this be baked into the generated file, where it does not
    belong.
    """
    if not pol.get("variations_follow_capital"):
        return None
    if row["kit"] or not row["bank"]:
        return None
    return next((r for r in rows
                 if r["program"] == row["program"] and not r["bank"] and not r["kit"]), None)


def resolved_next(row: dict, pol: dict, rows: list[dict]) -> str:
    """This voice's next move with the policy folded in, at print time.

    The bank-only answer is generated into `tools/voice-status.json` and the
    policy is not, for the reason `tier_of` gives: a tier, a goal and an
    approximation are decisions, and baking one into the generated file would
    make it stale every time the policy moved with no voice having changed. So
    the two are resolved here, exactly where the rank is.

    Two things the generated answer cannot say. An approximated slot is
    terminal rather than uncaptured. And a variation is its capital copied and
    then narrowed, so the honest answer for one is whichever of two holds --
    the capital is not accepted yet and nothing here moves, or it is and this
    one can be captured -- and `next_action` sees neither the policy nor the
    other rows.
    """
    approx = approximation(pol, row["slug"])
    if approx is not None:
        answered = approx.get("answered_by") or "a neighbouring voice"
        return (f"approximated by {answered}, and terminal: "
                f"{approx.get('reason') or 'no reason recorded'}")
    capital = capital_of(row, rows, pol)
    if capital is not None and row["stage"] < HEARD_STAGE:
        if capital["stage"] < HEARD_STAGE:
            return (f"its capital {capital['slug']} is at {capital['stage']:.1f} and nothing "
                    f"here moves until it is heard: a variation is the capital copied and "
                    f"then narrowed, so starting one first buys a round of rework")
        return (f"its capital {capital['slug']} is heard: capture this variation from the "
                f"module and measure it")
    return row["next"]


def next_action(axes: dict, stage: int, candidates: list[str]) -> str:
    """The one move that would raise this voice's stage, in a line.

    The bank alone, which is what makes it safe to generate. Anything the
    policy decides is folded in by `resolved_next` when the table is printed.
    """
    if stage == 0:
        return "no deliberate voice: pick an engine and write a patch"
    if stage == 1:
        if axes["timbres"] == 0:
            return "capture an oracle: no reference exists for this voice"
        return "measure the captured corpus into a reference profile"
    if stage == 2:
        if axes["gate_state"] is None:
            return ("write the first gate: `profile.py compare --write-gate` against "
                    "the profile just measured")
        if axes["gate_state"] != "current":
            return (f"the gate is {axes['gate_state']}: re-record it against the "
                    f"current profile")
        gaps = axes["coverage"]["gaps"]
        return (f"{len(gaps)} canonical dimension(s) neither gated nor excused: "
                f"{', '.join(gaps)}")
    if stage == 3:
        listen = "listen to a take and record the verdict in signoff.json"
        if candidates:
            return (f"judge the recorded candidate(s) — {', '.join(candidates)} — "
                    f"then {listen}")
        music = axes["music"]
        if music is not None:
            return f"the sign-off is {music['state']} against this bank: listen again"
        return listen
    if stage == 4:
        rest = _last_step(axes)
        if candidates:
            return f"judge the recorded candidate(s) — {', '.join(candidates)} — then {rest}"
        return rest
    return "settled"


def _last_step(axes: dict) -> str:
    """Why the structural claim does not yet earn the last step.

    Only reached once the take is signed off, so the musical half is settled by
    construction and every branch here is about the diagnosis. It fails in four
    ways and they need different work, which is why the sentence names which.
    """
    structure = axes["structure"]
    if structure is None:
        return "record a structural residual with autofit --diagnose"
    if structure["state"] == signoff.STALE:
        # A kit has no patch to name; what moved is one of its own drum notes.
        what = "the patch" if axes.get("patch") else "a voice of this kit"
        return f"{what} has moved since the diagnosis: re-run autofit --diagnose"
    if structure["state"] == signoff.UNVERIFIED:
        return ("a shared calibration unit has moved since the diagnosis and nothing can "
                "attribute it: re-run autofit --diagnose")
    if structure["open"]:
        return (f"{len(structure['open'])} term(s) no knob reaches and nobody has accepted: "
                f"{', '.join(structure['open'])} — a missing mechanism in the model, "
                f"or an acceptance with a reason")
    return "settled"


def build(catalogue) -> list[dict]:
    """One entry per bank voice, in program order with kits last."""
    pool = bank.captures()
    kits = sorted({c.program for c in pool if c.drums}) or [0]
    voices = bank.voices(catalogue=catalogue, kits=kits)
    cands = open_candidates()
    claims = signoff.load()
    _generation, unit_versions = signoff.bank_versions(BANK_VERSIONS)
    shared_gen = signoff.moved_generation(BANK_VERSIONS, {"shared"})
    drum_gen = signoff.moved_generation(BANK_VERSIONS, {"drum"})
    rows = []
    for v in voices:
        cap = v.capture
        every = [profile_facts(c.id) for c in v.captures]
        gates = [f.pop("_gate") for f in every]
        facts = merged_facts(every)
        claim = claims.get(v.slug, signoff.Record())
        # A kit's voices are its drum notes, so it has no single patch unit:
        # the drum kinds stand in for the patch version it does not have.
        patch_version = unit_versions.get(v.patch or "", 0)
        own_gen = drum_gen if v.kit else 0
        axes = {
            "engine": engine_for(v, catalogue),
            "patch": v.patch or None,
            "timbres": max((len(c.timbres) for c in v.captures), default=0),
            "profile_rows": facts["profile_rows"],
            "gate_state": facts["gate_state"],
            "coverage": coverage(v, [c.raw for c in v.captures], gates),
            "agreement": merged_agreement(gates),
            "structure": signoff.axis(claim.structure, shared_gen, patch_version, own_gen),
            "music": signoff.axis(claim.music, shared_gen, patch_version, own_gen),
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


def render_table(rows: list[dict], *, every: bool, pol: dict, goal: str | None = None) -> None:
    """The CLI view: grouped by GM family, one line per voice."""
    goals = goal_progress(pol, rows)
    if goal is not None:
        match = next((g for g in goals if g["name"] == goal), None)
        if match is None:
            known = ", ".join(g["name"] for g in goals) or "none declared"
            print(f"  no goal named {goal} in {_display(POLICY)} — have: {known}")
            return
        rows = goal_members((pol.get("goals") or {})[goal], rows)
        goals = [match]
    shown = rows if every or goal else [
        r for r in rows if r["stage"] > 0.2 or r["open_candidates"]]
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
        rank, _name = tier_of(pol, r["program"], r["bank"], kit=r["kit"])
        tier = f"t{rank}" if rank else "t-"
        print(f"    {r['slug']:<32} {tier} {r['stage']:.1f} {bar}  {r['engine'] or '?':<15}"
              f" {oracle:<10}{flag}".rstrip())
        # Only past the oracle step, where the line differs per voice. Below it
        # every voice says the same sentence, and 150 copies of it bury the four
        # that say something — except where the policy has something to add,
        # which is per slot and is the reason it is worth a line.
        resolved = resolved_next(r, pol, rows)
        if 0.2 < r["stage"] < 1.0 or resolved != r["next"]:
            print(f"      -> {resolved}")
    total = len(rows)
    counts: dict[str, int] = {}
    for r in rows:
        counts[r["stage_name"]] = counts.get(r["stage_name"], 0) + 1
    print(f"\n  {total} voices: "
          + ", ".join(f"{n} {s}" for s, n in
                      sorted(counts.items(), key=lambda kv: STAGES.index(kv[0]))))
    # Counted apart from the unfinished voices rather than added to them: an
    # approximated slot has no oracle and is not waiting for one.
    approximated = [r for r in rows if approximation(pol, r["slug"])]
    no_oracle = [r for r in rows if not r["capture"] and r not in approximated]
    if no_oracle:
        print(f"  {len(no_oracle)} with no oracle captured — that is the task list, "
              f"and nothing below stage 0.4 moves without one")
    if approximated:
        print(f"  {len(approximated)} approximated by a neighbouring voice and terminal: "
              f"no oracle exists for them and none is being sought")
    waiting, ready = variation_split(rows, pol)
    if waiting or ready:
        print(f"  {waiting + ready} variation(s) below heard: {waiting} behind a capital that "
              f"is not heard yet, {ready} whose capital is and which can be captured")
    unwritten = sum(len(r["open_candidates"]) for r in rows)
    if unwritten:
        print(f"  {unwritten} recorded calibration setting(s) not written back")
    census = signoff_census(rows)
    if census["structure"] or census["music"]:
        print(f"  sign-offs: {census['music']} musical "
              f"({census['music_current']} current against this bank), "
              f"{census['structure']} structural "
              f"({census['structure_current']} current)")
    if census["structure_only"]:
        print(f"  {census['structure_only']} of those diagnoses sit on a voice nobody has "
              f"signed a take off on: the ear is read one step earlier, so none of them "
              f"raises a stage until somebody listens")
    for g in goals:
        step = STAGES[round(g["stage"] * 5)]
        print(f"  goal {g['name']}: {g['met']}/{g['total']} at {step} ({g['stage']:.1f})")
        if g["short"] and goal is not None:
            print("    short: " + ", ".join(r["slug"] for r in g["short"]))
    if goals:
        print("  a goal is where attention goes, never a condition on shipping: a version "
              "whose date arrives with the set unmet ships and the goal carries over")


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
    ap.add_argument("--goal", default=None, metavar="NAME",
                    help=f"print one goal's members and what each still needs "
                         f"(goals are declared in {_display(POLICY)}; a goal is "
                         f"never a condition on shipping)")
    ap.add_argument("--sr", type=int, default=48000)
    args = ap.parse_args()
    pol = _load(POLICY)

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
        render_table(rows, every=args.all, pol=pol, goal=args.goal)
        return 0

    # The reading path needs no build: the generated file is committed precisely
    # so a plain clone can see where the bank stands.
    if not OUT_PATH.is_file():
        print(f"{OUT_PATH}: missing — run `make voice-status-refresh` "
              f"(needs a -DBUILD_TUNING=ON build)")
        return 0
    render_table(_load(OUT_PATH)["voices"], every=args.all, pol=pol, goal=args.goal)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
