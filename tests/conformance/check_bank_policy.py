"""Hold the bank's policy to the bank it ranks, and to the tool that reads it.

`tools/voicematch/policy.json` is decisions: which timbre rule a slot falls
under, what order the bank is worked in, which slots a neighbour answers, and
what each release goal asks for. `tools/voice-status.json` is facts, generated
from what the library and the references reported. The two are deliberately
kept apart and resolved only at print time, so nothing in the tree compares
them until here.

Everything the policy names is read through `.get()` chains, and every one of
them answers `[]` or `{}` for a key that moved. So a program number outside the
GM range, a goal naming a voice the bank has no row for, two tiers claiming one
rank, an invented key nothing reads: each degrades into a different but
plausible ranking rather than into an error.

**Nothing here asserts that any voice has got anywhere.** A goal is where
attention goes and never a condition on shipping — `policy.json`'s own
`_goals_do_not_gate` says so and `tools/voicematch/docs/status.md` repeats it.
Every failure below is about the policy being well-formed and about the bank
being able to answer it. A stage appears in one place only, as the check that a
goal's declared step is a rung of the ladder rather than a number between two.

## A kit is a second number space over the same integers

A kit is named by the rhythm-part program a file selects it with — the number
`tools/voice-status.json` carries — and never by its `kGsDrumKits` index. So
`kits: [0]` and `programs: [0]` name different things and a tier's two lists
resolve separately. Read as one, a tier listing kit 8 also ranks the celesta.

That is why a kit number is held twice here. `kGsDrumKits` defines 26 rhythm
sets while the bank carries one kit row, because `status.py` derives its kit
rows from the captures that exist rather than from the GS map. So a kit number
must be a program some GS map defines a set at, and it must have a row in the
bank. Failing the first is a wrong number; failing the second is a kit ranked
or goaled before anything can report on it, which is what arrives the first
time a second kit is added to a tier.

## The tool is asked to run, because parsing is not the failure mode

The last class runs `status.py`'s own renderer over the bank for every declared
goal. A policy can parse, satisfy every field rule above, and still kill the
only tool that reads it — a tier with no `name`, a goal whose step indexes past
the ladder — and none of the comparisons above can see it.

## What the floors are for

Every population below is read out of a file through a `.get()` that answers
empty for a key that moved, and every membership rule holds vacuously over an
empty one. The floors are what stops a clean report over nothing; each is what
was there when it was written, and a deliberate reduction moves it in the same
change.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import re
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

POLICY = "tools/voicematch/policy.json"
BANK = "tools/voice-status.json"
GS_LAYER = "src/midi/synth/gs_layer.h"
CAPTURE_DIR = "tools/voicematch/capture"

#: What kind of source answered a capture. A classification and not an
#: identity: the product and its presets stay in the untracked
#: `<id>.local.json`, and this is the one fact about them a check downstream
#: needs. Same shape and same reason as `voicing.md`'s `rig` — a rigged
#: reference looks identical to a direct one until a model has acquired a
#: cabinet, and a library's idea of a synth pad looks identical to the machine's.
SOURCE_CLASSES = ("module", "dedicated", "library")

#: The class a `machine` reference axis can be answered by. A slot naming a
#: sound the machine invented has nothing standing behind it for a recording to
#: be made of, so the module is the only source that can be its target.
MACHINE_SOURCE = "module"

#: A capture's `room` saying the recording carries a space. The module is
#: captured with every effect off, so this and `module` cannot both be true —
#: `capture.py`'s ROOM_PRESENT, spelled here rather than imported because this
#: check reads files under `--root` and must not need the tools tree to parse.
ROOM_PRESENT = "present"

#: The ladder's rungs, in fifths — `tools/voicematch/docs/status.md`. A goal's
#: step has to be one of these: `status.py` indexes `STAGES` by it, so a number
#: between two rungs names a step that does not exist.
LADDER = tuple(n / 5.0 for n in range(6))

#: A generated row may carry none of these. The rule is in `status.md` and in
#: `.claude/rules/synth-bank.md`: `tools/voice-status.json` holds what the
#: library and the references reported, and a tier, a rank or a goal is a
#: decision. Baking one in makes the generated file stale whenever the policy
#: moves with no voice having changed.
DECISION_KEYS = ("tier", "rank", "goal", "goals", "priority", "release")

#: Keys a tier entry may carry. Anything else is read by nothing: the policy is
#: consumed through `.get()`, so an invented or misspelled key is silent and
#: the tier goes on being resolved by whatever is left.
TIER_KEYS = {"name", "rank", "programs", "kits", "default", "reason"}

#: The same for a goal. `banks` is deliberately not here — a goal names capital
#: tones and kits, never a variation, and a key that looked like it would work
#: is how that rule would be broken.
GOAL_KEYS = {"stage", "programs", "kits", "reason"}

#: And for a `reference_layer` branch. `programs` is absent from `default` and
#: from `kits`, which are the two branches that take everything not listed.
LAYER_KEYS = {"timbre", "behaviour", "programs", "reason"}

#: What the two axes of `reference_layer` may say.
LAYER_VALUES = {"instrument", "machine"}

#: Documentation keys, as everywhere else in this tree's JSON.
DOC_PREFIX = "_"

# Each is what the scan reached when this was written. They are not thresholds
# on the bank's progress — every one counts declarations and rows, never a
# stage.
# A ratchet rather than a floor, and the only one here: it may fall and may not
# rise. Forty machine-defined slots are answered by something other than the
# module, and nobody can clear that today — a module reference per slot is
# open-ended work against hardware, and a red build nobody can clear is the
# shape this file refuses everywhere else. A *new* one is clearable, by whoever
# added the capture, which is what this catches. Lower it in the change that
# captures a module reference; the failure says so.
CEILING = {
    "machine_slots_without_a_module_reference": 40,
}

FLOOR = {
    "rows": 180,
    "capital_programs": 128,
    "tier_members": 60,
    "goal_members": 35,
    "machine_defined": 40,
    "gs_kits": 26,
    "goals_run": 1,
    "captures": 125,
    "captures_classified": 125,
}


class Slot(typing.NamedTuple):
    """One number a policy block names, and where it was named."""

    where: str
    kind: str  # "program" or "kit"
    number: int

    @property
    def display(self) -> str:
        return f"{self.where}: {self.kind} {self.number}"


def _load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}


def _objects(block) -> dict:
    """@p block's object-valued entries, or empty where it is not a mapping.

    A block of the wrong shape reads as empty here and is reported by the rule
    that was looking for what it should have held, rather than raising out of a
    helper that was only collecting numbers.
    """
    if not isinstance(block, dict):
        return {}
    return {k: v for k, v in block.items() if isinstance(v, dict)}


_GS_KIT = re.compile(
    r"\{\s*(?P<program>\d+)\s*,\s*(?P<index>\d+)\s*,\s*\"(?P<name>[^\"]*)\"\s*,"
    r"\s*GsToneMap::\w+\s*\}"
)


def gs_kit_programs(root: Path) -> dict[int, str]:
    """Rhythm-part program to kit name, from `kGsDrumKits`'s own declaration.

    Read out of the header rather than mirrored here, for the reason every
    other table in this directory is: a mirror agrees with itself. The array's
    extent is taken from the declaration too, so a kit appended without the
    count moving reads as an array this cannot parse rather than as a short one.
    """
    text = (root / GS_LAYER).read_text(encoding="utf-8") if (root / GS_LAYER).is_file() else ""
    anchor = re.search(r"kGsDrumKits\s*=\s*\{\{(?P<body>.*?)\}\};", text, re.DOTALL)
    declared = re.search(r"std::array<GsDrumKit,\s*(\d+)>\s+kGsDrumKits", text)
    if anchor is None or declared is None:
        return {}
    kits = {int(m.group("program")): m.group("name") for m in _GS_KIT.finditer(anchor.group("body"))}
    return kits if len(kits) == int(declared.group(1)) else {}


def tier_slots(policy: dict) -> list[Slot]:
    """Every number the tiers name, tagged with which space it is in."""
    out: list[Slot] = []
    for tier in policy.get("tiers") or []:
        if not isinstance(tier, dict):
            continue
        where = f"tier `{tier.get('name', '?')}`"
        out += [Slot(where, "program", n) for n in tier.get("programs") or []
                if isinstance(n, int)]
        out += [Slot(where, "kit", n) for n in tier.get("kits") or [] if isinstance(n, int)]
    return out


def goal_slots(policy: dict) -> list[Slot]:
    """Every number the goals name, tagged with which space it is in."""
    out: list[Slot] = []
    for name, goal in sorted(_objects(policy.get("goals")).items()):
        if name.startswith(DOC_PREFIX):
            continue
        where = f"goal `{name}`"
        out += [Slot(where, "program", n) for n in goal.get("programs") or []
                if isinstance(n, int)]
        out += [Slot(where, "kit", n) for n in goal.get("kits") or [] if isinstance(n, int)]
    return out


def layer_slots(policy: dict) -> list[Slot]:
    """Every number the reference layer names. One space: these are programs."""
    out: list[Slot] = []
    for name, branch in _objects(policy.get("reference_layer")).items():
        if name.startswith(DOC_PREFIX):
            continue
        out += [Slot(f"reference_layer.{name}", "program", n)
                for n in branch.get("programs") or [] if isinstance(n, int)]
    return out


def read_captures(root: Path) -> tuple[dict[str, dict], list[str]]:
    """Every tracked capture definition by id, and the files that did not parse.

    The untracked `<id>.local.json` overlay is never opened. It holds the
    product and each timbre's preset and is untracked on purpose, so nothing
    mechanical may depend on it being there — which is exactly why the class of
    source is a field in the tracked file rather than something inferred from
    the sibling.
    """
    out: dict[str, dict] = {}
    unreadable: list[str] = []
    directory = root / CAPTURE_DIR
    if not directory.is_dir():
        return out, unreadable
    for path in sorted(directory.glob("*.json")):
        if path.name.endswith(".local.json"):
            continue
        display = f"{CAPTURE_DIR}/{path.name}"
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except ValueError as error:
            unreadable.append(f"  {display}: {error}")
            continue
        if not isinstance(raw, dict) or not raw.get("id"):
            unreadable.append(f"  {display}: no `id`, so nothing can attach to it")
            continue
        out[str(raw["id"])] = {
            "path": display,
            "program": raw.get("program"),
            "bank": int(raw.get("bank") or 0),
            "source_class": raw.get("source_class"),
            "room": raw.get("room"),
        }
    return out, unreadable


def machine_answers(scan: Scan) -> dict[str, int]:
    """How the reference behind each machine-defined slot was sourced.

    Counted and reported, never failed on. A slot answered by the wrong class
    is a reference nobody has captured yet, and acquiring one is open-ended work
    against hardware — the same shape as the calibration this file is careful
    never to gate, and a red build here would be a red build nobody could clear.
    What it may not do is stay invisible: a pad fitted against a library's idea
    of what a warm pad is passes every gate, every test and every audit, and
    reads as finished.
    """
    layer = (scan.policy.get("reference_layer") or {})
    branch = layer.get("machine_defined") if isinstance(layer, dict) else None
    programs = (branch or {}).get("programs") or [] if isinstance(branch, dict) else []
    by_program = {c["program"]: c for c in scan.captures.values() if not c["bank"]}
    out = dict.fromkeys((*SOURCE_CLASSES, "unclassified", "uncaptured"), 0)
    for number in programs:
        capture = by_program.get(number)
        if capture is None:
            out["uncaptured"] += 1
            continue
        value = capture["source_class"]
        out[value if value in SOURCE_CLASSES else "unclassified"] += 1
    return out


class Scan:
    """The policy, the bank it ranks, and the GS kit table behind its numbers."""

    def __init__(self, root: Path = ROOT) -> None:
        self.root = root
        self.policy = _load(root / POLICY)
        self.rows = (_load(root / BANK) or {}).get("voices") or []
        self.gs_kits = gs_kit_programs(root)
        self.captures, self.capture_errors = read_captures(root)
        # A tier or a goal names a capital tone. A variation has no priority of
        # its own -- it ranks with its capital -- so it is not a name a policy
        # may use, and resolving against every row would let one through.
        self.capitals = {r["program"] for r in self.rows
                         if not r.get("kit") and not r.get("bank")}
        self.kits = {r["program"] for r in self.rows if r.get("kit")}
        self.slugs = {r["slug"] for r in self.rows}
        self.tier_slots = tier_slots(self.policy)
        self.goal_slots = goal_slots(self.policy)
        self.layer_slots = layer_slots(self.policy)
        # Filled by the render class: which goals the shipping tool was put
        # through. A run that never happened and a run that found nothing print
        # the same thing otherwise.
        self.goals_run: list[str] = []


def _anchors(scan: Scan) -> list[str]:
    out: list[str] = []
    if not scan.policy:
        out.append(f"{POLICY}: unreadable or empty; every rule below resolves against it")
    if not scan.rows:
        out.append(f"{BANK}: no voices; every membership rule below holds over an empty bank")
    if not scan.gs_kits:
        out.append(
            f"{GS_LAYER}: `kGsDrumKits` did not parse to its own declared extent, so a kit "
            "number is held to nothing"
        )
    return out


def _numbers(scan: Scan) -> list[str]:
    """Programs out of the GM range, in either number space."""
    return [
        f"  {slot.display} is outside 0..127 — GM program numbers are zero-based here, "
        f"for a kit as much as for a melodic voice"
        for slot in (*scan.tier_slots, *scan.goal_slots, *scan.layer_slots)
        if not 0 <= slot.number <= 127
    ]


def _tiers(scan: Scan) -> list[str]:
    """The tier list's own shape: names, ranks, and the single default."""
    raw = scan.policy.get("tiers") or []
    out: list[str] = []
    if not isinstance(raw, list):
        return [f"  `tiers` is {type(raw).__name__}, not a list; the order is its order"]
    tiers = [t for t in raw if isinstance(t, dict)]
    out += [f"  tier {index} is {type(t).__name__}, not an object"
            for index, t in enumerate(raw) if not isinstance(t, dict)]
    names = [t.get("name") for t in tiers]
    for index, tier in enumerate(tiers):
        if not str(tier.get("name") or "").strip():
            out.append(f"  tier {index} carries no `name`; `status.py` reads it unguarded")
        if not isinstance(tier.get("rank"), int):
            out.append(f"  tier `{tier.get('name', index)}` carries no integer `rank`")
        stray = sorted(set(tier) - TIER_KEYS - {k for k in tier if k.startswith(DOC_PREFIX)})
        if stray:
            out.append(
                f"  tier `{tier.get('name', index)}` carries {', '.join(stray)}, which nothing "
                f"reads — the policy is consumed through `.get()`, so the tier goes on being "
                f"resolved by the keys that are left"
            )
    duplicated = sorted({n for n in names if names.count(n) > 1 and n})
    if duplicated:
        out.append(f"  two tiers are named {', '.join(duplicated)}")
    ranks = [t["rank"] for t in tiers if isinstance(t.get("rank"), int)]
    if ranks and sorted(ranks) != list(range(1, len(ranks) + 1)):
        out.append(
            f"  the ranks are {sorted(ranks)}; they have to be unique and contiguous from 1, "
            f"or the order the bank is worked in has a gap or a tie in it"
        )
    defaults = [t.get("name") for t in tiers if t.get("default")]
    if len(defaults) != 1:
        out.append(
            f"  {len(defaults)} tier(s) carry `default: true` ({', '.join(map(str, defaults)) or 'none'}) "
            f"— exactly one takes every program no other tier names, and with none the "
            f"fallback rank is one past the last tier for every unlisted voice"
        )
    return out


def _membership(scan: Scan) -> list[str]:
    """Every number a policy block names, resolved against the bank."""
    out: list[str] = []
    for slot in (*scan.tier_slots, *scan.goal_slots, *scan.layer_slots):
        if not 0 <= slot.number <= 127:
            continue  # already reported, and it resolves against nothing
        if slot.kind == "kit":
            if slot.number not in scan.gs_kits:
                out.append(
                    f"  {slot.display} is not a rhythm-part program any GS map defines a set "
                    f"at — a kit is named by the program a file selects it with, never by its "
                    f"`kGsDrumKits` index"
                )
            elif slot.number not in scan.kits:
                out.append(
                    f"  {slot.display} (`{scan.gs_kits[slot.number]}`) has no row in {BANK}: the "
                    f"bank's kit rows come from the captures that exist, so nothing can report "
                    f"on this kit until one is captured for it"
                )
        elif slot.number not in scan.capitals:
            out.append(
                f"  {slot.display} resolves to no capital row in {BANK}"
            )
    return out


def _goals(scan: Scan) -> list[str]:
    """Each goal's declared step, its keys, and that it names no variation."""
    raw = scan.policy.get("goals") or {}
    if not isinstance(raw, dict):
        return [f"  `goals` is {type(raw).__name__}, not an object keyed by goal name"]
    out: list[str] = [
        f"  goal `{name}` is {type(value).__name__}, not an object"
        for name, value in sorted(raw.items())
        if not name.startswith(DOC_PREFIX) and not isinstance(value, dict)
    ]
    rows_by_program: dict[int, list[dict]] = {}
    for row in scan.rows:
        if not row.get("kit"):
            rows_by_program.setdefault(row["program"], []).append(row)
    for name, goal in sorted(_objects(scan.policy.get("goals")).items()):
        if name.startswith(DOC_PREFIX):
            continue
        stage = goal.get("stage")
        if not isinstance(stage, (int, float)) or not any(
                abs(float(stage) - rung) < 1e-9 for rung in LADDER):
            out.append(
                f"  goal `{name}` asks for stage {stage!r}, which is not a rung of the ladder "
                f"({', '.join(f'{r:.1f}' for r in LADDER)}) — `status.py` names the step by "
                f"indexing on it"
            )
        stray = sorted(set(goal) - GOAL_KEYS - {k for k in goal if k.startswith(DOC_PREFIX)})
        if stray:
            out.append(
                f"  goal `{name}` carries {', '.join(stray)}, which nothing reads — a goal "
                f"names capital tones and kits, and a key that looks like it would widen that "
                f"is silently ignored instead"
            )
        for number in goal.get("programs") or []:
            variations = [r for r in rows_by_program.get(number, []) if r.get("bank")]
            if variations and number not in scan.capitals:
                out.append(
                    f"  goal `{name}` names program {number}, which the bank carries only as "
                    f"variation(s): {', '.join(r['slug'] for r in variations)}"
                )
    return out


def _layers(scan: Scan) -> list[str]:
    """The reference layer's two axes, and the keys each branch may carry."""
    layer = scan.policy.get("reference_layer") or {}
    if not isinstance(layer, dict):
        return [f"  `reference_layer` is {type(layer).__name__}, not an object"]
    out: list[str] = []
    for name, branch in sorted(layer.items()):
        if name.startswith(DOC_PREFIX):
            continue
        if not isinstance(branch, dict):
            out.append(f"  reference_layer.{name} is not an object")
            continue
        for axis in ("timbre", "behaviour"):
            value = branch.get(axis)
            if value not in LAYER_VALUES:
                out.append(
                    f"  reference_layer.{name}.{axis} is {value!r}; the two answers are "
                    f"{' and '.join(sorted(LAYER_VALUES))}, and a slot with neither takes "
                    f"whichever the reader assumes"
                )
        if not str(branch.get("reason") or "").strip():
            out.append(f"  reference_layer.{name} carries no reason")
        stray = sorted(set(branch) - LAYER_KEYS - {k for k in branch if k.startswith(DOC_PREFIX)})
        if stray:
            out.append(f"  reference_layer.{name} carries {', '.join(stray)}, which nothing reads")
    if "default" not in layer:
        out.append(
            "  reference_layer has no `default` branch, so a program no other branch names "
            "falls to whatever the reader supplies"
        )
    return out


def _approximated(scan: Scan) -> list[str]:
    """Each approximated slot names a voice in the bank and says what it cannot reach."""
    block = scan.policy.get("approximated")
    if block is None:
        return [(f"  {POLICY} declares no `approximated` block; an empty one is a claim that "
                 f"every slot is answered by a patch written for it, and an absent one is not")]
    if not isinstance(block, dict):
        return [f"  `approximated` is {type(block).__name__}, not an object keyed by voice slug"]
    out: list[str] = []
    for slug, entry in sorted(block.items()):
        if slug.startswith(DOC_PREFIX):
            continue
        if slug not in scan.slugs:
            out.append(f"  approximated `{slug}` matches no voice in {BANK}")
        if not isinstance(entry, dict):
            out.append(
                f"  approximated `{slug}` is {type(entry).__name__}, not an object naming the "
                f"voice answering it and what the model cannot reach"
            )
            continue
        answered = str(entry.get("answered_by") or "").strip()
        if not answered:
            out.append(f"  approximated `{slug}` names no `answered_by`")
        elif answered not in scan.slugs:
            out.append(
                f"  approximated `{slug}` is answered by `{answered}`, which matches no voice "
                f"in {BANK}"
            )
        if not str(entry.get("reason") or "").strip():
            out.append(
                f"  approximated `{slug}` carries no reason — nothing downstream can tell an "
                f"approximation from an unfinished voice, which is what the entry is for"
            )
    return out


def _captures(scan: Scan) -> list[str]:
    """Every capture says what class of source answered it, or it says nothing."""
    out = list(scan.capture_errors)
    for _id, capture in sorted(scan.captures.items(), key=lambda kv: kv[1]["path"]):
        value = capture["source_class"]
        if value is None:
            out.append(
                f"  {capture['path']} declares no `source_class`. Absence is unclassified and "
                f"never a default — the dangerous reading is the one a missing field falls "
                f"into, and a slot fitted against the wrong kind of source is indistinguishable "
                f"from one fitted against the right kind"
            )
        elif value not in SOURCE_CLASSES:
            out.append(
                f"  {capture['path']}: `source_class` is {value!r}; the classes are "
                f"{', '.join(SOURCE_CLASSES)}"
            )
        elif value == MACHINE_SOURCE and capture["room"] == ROOM_PRESENT:
            out.append(
                f"  {capture['path']} is a {MACHINE_SOURCE} capture declaring a room. The "
                f"module is captured with reverb, chorus, delay and the insertion effect all "
                f"off, so one of the two is wrong — a module left with its effects on, or a "
                f"capture from something else labelled as one. A capture carrying the "
                f"module's own reverb drives the room fit into matching a tank rather than "
                f"a building"
            )
    return out


def _decisions(scan: Scan) -> list[str]:
    """A generated row carrying a decision."""
    out: list[str] = []
    for row in scan.rows:
        found = sorted({k for k in (*row, *(row.get("axes") or {})) if k in DECISION_KEYS})
        if found:
            out.append(
                f"  {row.get('slug', '?')} carries {', '.join(found)} — {BANK} holds what the "
                f"library and the references reported, and a tier, a rank or a goal is a "
                f"decision that belongs in {POLICY}"
            )
    return out


def _renders(scan: Scan, status) -> list[str]:
    """`status.py`'s own renderer, over the bank and over each declared goal."""
    if status is None:
        return [
            ("  tools/voicematch/status.py could not be imported, so nothing here was put "
             "through the tool that reads the policy")
        ]
    out: list[str] = []
    goals = sorted(name for name in (scan.policy.get("goals") or {})
                   if not name.startswith(DOC_PREFIX))
    for goal in [None, *goals]:
        sink = io.StringIO()
        try:
            with contextlib.redirect_stdout(sink):
                status.render_table(scan.rows, every=True, pol=scan.policy, goal=goal)
        except Exception as error:  # noqa: BLE001 - the class is the finding
            out.append(
                f"  `status.py --goal {goal}`: {type(error).__name__}: {error}"
                if goal else f"  `status.py`: {type(error).__name__}: {error}"
            )
            continue
        if goal is None:
            continue
        if f"goal {goal}:" not in sink.getvalue():
            out.append(
                f"  `status.py --goal {goal}` ran and printed no line for the goal, so the "
                f"name the policy declares is not the name the tool resolves"
            )
            continue
        scan.goals_run.append(goal)
    return out


def _measured(scan: Scan) -> dict[str, int]:
    return {
        "rows": len(scan.rows),
        "capital_programs": len(scan.capitals),
        "tier_members": len(scan.tier_slots),
        "goal_members": len(scan.goal_slots),
        "machine_defined": len(scan.layer_slots),
        "gs_kits": len(scan.gs_kits),
        "goals_run": len(scan.goals_run),
        "captures": len(scan.captures),
        "captures_classified": sum(1 for c in scan.captures.values()
                                   if c["source_class"] in SOURCE_CLASSES),
    }


def _measured_ceilings(scan: Scan) -> dict[str, int]:
    counts = machine_answers(scan)
    return {
        "machine_slots_without_a_module_reference":
            sum(counts.values()) - counts[MACHINE_SOURCE],
    }


def _ceilings(scan: Scan, ceiling: dict[str, int]) -> list[str]:
    """The ratchet: a count that may fall and may not rise, in either direction.

    A rise is a machine-defined slot newly answered by something that is not the
    module, and it is reported because whoever added that capture can act on it.
    A fall is reported too, and for the reason every expiring record in this
    tree is: a ceiling left above what the tree reaches goes on blessing the
    room it no longer needs, so the next slot to take it inherits the blessing
    unexamined.
    """
    measured = _measured_ceilings(scan)
    out: list[str] = []
    for name, limit in ceiling.items():
        found = measured.get(name, 0)
        if found > limit:
            out.append(
                f"  {name}: {found}, above the {limit} recorded. A machine-defined slot is one "
                f"naming a sound the module invented, so nothing stands behind it for a "
                f"recording to answer with — capture the new one from the module, or say why "
                f"it is answered otherwise and raise this with the reason"
            )
        elif found < limit:
            out.append(
                f"  {name}: {found}, below the {limit} recorded. The ratchet has tightened and "
                f"nothing lowered it — re-record it at {found} in this change, so the next "
                f"slot answered by the wrong source cannot hide inside the old margin"
            )
    return out


def _floors(scan: Scan, floor: dict[str, int]) -> list[str]:
    measured = _measured(scan)
    return [
        f"  {name}: found {measured.get(name, 0)}, floor is {minimum} — the scan has stopped "
        f"reaching the population it is sized for, and every rule above holds over an empty one"
        for name, minimum in floor.items()
        if measured.get(name, 0) < minimum
    ]


def _import_status():
    """The shipping tool, or None. Imported from the tree, never from `--root`.

    A perturbed copy of the policy is data; the tool that reads it is code, and
    the question is whether the shipping one survives that policy.
    """
    for relative in ("tools", "tools/voicematch"):
        path = str(ROOT / relative)
        if path not in sys.path:
            sys.path.insert(0, path)
    try:
        import status
    except Exception:  # noqa: BLE001 - reported as a class, never swallowed
        return None
    return status


def evaluate(root: Path = ROOT, floor: dict[str, int] | None = None,
             ceiling: dict[str, int] | None = None) -> list[tuple[str, list[str]]]:
    """Every failure class, as (heading, lines). Empty means the policy holds."""
    scan = Scan(root)
    failures: list[tuple[str, list[str]]] = []

    anchors = _anchors(scan)
    if anchors:
        failures.append(("The policy or the bank it ranks could not be read", anchors))

    for heading, lines in (
        ("These program numbers are outside the GM range", _numbers(scan)),
        ("The tier list cannot express an order", _tiers(scan)),
        (
            ("These policy entries name a slot the bank has no row for. Every one of them is "
             "read through a `.get()` that answers empty, so the ranking loses a member "
             "rather than failing"),
            _membership(scan),
        ),
        ("These goals do not say what they ask for", _goals(scan)),
        ("The reference layer does not say which reference a slot is aimed at", _layers(scan)),
        (
            ("These captures do not say what kind of source answered them. The product "
             "itself stays untracked, so this classification is the only thing that can "
             "tell a slot aimed at the machine from one fitted against a library"),
            _captures(scan),
        ),
        (
            ("These approximations do not say what they are. An approximated slot is "
             "indistinguishable from an unfinished one to everything downstream, which is "
             "the whole reason the entry exists"),
            _approximated(scan),
        ),
        (
            ("These generated rows carry a decision. Reordering the bank would then need a "
             "`-DBUILD_TUNING=ON` rebuild, and the generated file would go stale every time "
             "the policy moved with no voice having changed"),
            _decisions(scan),
        ),
    ):
        if lines:
            failures.append((heading, lines))

    renders = _renders(scan, _import_status())
    if renders:
        failures.append(
            (
                ("The policy parses and the tool that reads it does not run. Everything above "
                 "compares fields; this is the one class that asks whether the only consumer "
                 "survives the file"),
                renders,
            )
        )

    ratchet = _ceilings(scan, CEILING if ceiling is None else ceiling)
    if ratchet:
        failures.append(
            (
                ("A recorded count has moved and nothing moved with it. This one ratchets: a "
                 "machine-defined slot answered by anything but the module is not clearable "
                 "today and is carried, but a new one is, and a margin left above the tree is "
                 "a place the next one hides"),
                ratchet,
            )
        )

    bar = _floors(scan, FLOOR if floor is None else floor)
    if bar:
        failures.append(("The scan no longer reaches the population it is sized for", bar))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    failures = evaluate(args.root)
    scan = Scan(args.root)
    _renders(scan, _import_status())
    measured = _measured(scan)
    machine = machine_answers(scan)

    if args.json:
        print(json.dumps({"measured": measured, "machine_defined_answered_by": machine,
                          "ceilings": _measured_ceilings(scan),
                          "failures": [{"heading": h, "lines": lines} for h, lines in failures]},
                         indent=2, ensure_ascii=False))
    else:
        print(f"{POLICY} against {BANK}")
        for name, count in measured.items():
            print(f"  {name}: {count} (floor {FLOOR[name]})")
        print(f"machine-defined slots, by what answered their reference "
              f"({MACHINE_SOURCE} is the only one the policy's own `machine` axis accepts)")
        for name, count in machine.items():
            print(f"  {name}: {count}")
        for name, count in _measured_ceilings(scan).items():
            print(f"  {name}: {count} (ratchet {CEILING[name]}, may fall and may not rise)")

    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
