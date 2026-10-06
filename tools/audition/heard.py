"""What the ear said, read back out of the listening log.

    python3 tools/audition/heard.py                 # every voice with notes
    python3 tools/audition/heard.py p040-violin     # one voice, in full
    python3 tools/audition/heard.py --grade broken  # only the worst verdicts
    python3 tools/audition/heard.py --json          # for a tool rather than a person

The audition page writes one JSON line per note under the scratch root, next to
what was sounding when it was written. Nothing read it: a verdict the ear
reached sat in a file whose name nobody had a reason to type, which is the same
trip most of what gets heard never made. This is the other end of it.

TWO FIELDS AND THEY ARE NOT THE SAME QUESTION. `grade` is the verdict — `ok`,
`acceptable`, `wrong-instrument`, `broken` — and `tag` is the finest point the
narrowing reached, `onset/hard` or `tone/dark`. The tag is the same string
whether the voice is shippable or a different instrument, so it can be read only
beside its grade. A tag is where to listen again, never a parameter to move: it
came out of a question asked in a listener's own words and there is no knob on
the other side of it.

A NOTE IS ABOUT A RECORDING AND A VOICE MOVES. A v2 note carries the ids of
the render it was heard on; it is `current` when that render is still the
page's, its comparison is still defined, and the bank registry the library
embedded still stands for the voice's own unit and the shared ones
(`signoff.bank_state`). A known change since is `stale`; missing evidence -- a
v1 note, an old library, no path record -- is `unverified`, and no version is
back-filled onto it. The day posted says only when it was heard; a v1 note heard
before its unit last moved is `stale`, since the render it heard cannot be newer.
Verdicts are reduced per comparison, so a DI verdict never stands in for the
product's; candidates, comparisons and preferences are listed apart.

A KIT IS ONE PART AND FORTY-ODD INSTRUMENTS, and its unit is the drum note. A
kit has no patch unit at all — `d000`-`d127` are versioned separately — so a
verdict filed against "the kit" is attributable to nothing. The log carries
which strike the note was taken on and which notes that strike held, so each
one resolves to its own `dNNN`, is named from the GM drum map, and is dated
against that unit alone.

Only the standard library, like the server that writes the log; `--signoff`
alone re-derives the comparison boundary, which imports numpy.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
if str(REPO_ROOT / "tools" / "voicematch") not in sys.path:
    sys.path.append(str(REPO_ROOT / "tools" / "voicematch"))

import policy
import signoff as claims

#: The same root the server writes to and the rest of the harness renders into.
SCRATCH_ROOT = (
    Path(os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch")
    .expanduser()
    .resolve()
)
FEEDBACK_ROOT = SCRATCH_ROOT / "feedback"
AUDITION_ROOT = SCRATCH_ROOT / "audition"

#: Where a voice's own generation and the date it last moved are recorded.
BANK_VERSIONS = REPO_ROOT / "tools" / "bank-versions.json"

#: The verdicts, worst first. The order is the reading order: a voice that
#: barely sounds is a missing mechanism and outranks anything about its colour.
GRADES = ("broken", "wrong-instrument", "off", "acceptable", "ok")

#: What each verdict says, in the words the page asked the question in. Not the
#: page's own labels — those are the listener's side of it, and these are what
#: the verdict means for the work.
MEANS = {
    "broken": "barely sounds — a mechanism is missing, not a constant",
    "wrong-instrument": "recognisably something else",
    "off": "something is off, no verdict reached",
    "acceptable": "the instrument, not the reference — liveable",
    "ok": "fine as it stands",
    "": "no verdict",
}


def read_log(path: Path) -> list[dict]:
    """Every entry in one log, skipping any line that is not one."""
    out: list[dict] = []
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return out
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if isinstance(entry, dict):
            out.append(entry)
    return out


def logs(root: Path | None = None) -> dict[str, list[dict]]:
    """Every voice that has been said anything about, by set id.

    The root is resolved per call rather than bound as a default: a default is
    evaluated once at import, so anything pointing this at another tree -- a
    test, or a second scratch root -- would go on reading the first one.
    """
    root = root or FEEDBACK_ROOT
    found = {}
    for path in sorted(root.glob("*.jsonl")):
        entries = read_log(path)
        if entries:
            found[path.stem] = entries
    return found


def manifest_of(set_id: str) -> dict:
    """The page's manifest as it stands now, empty when there is none.

    The directory is the one the server serves under this id, so a set the
    server names after its parent, or suffixes, is found by the same name.
    """
    # Imported here: serve imports this module, so a top-level import would cycle.
    import serve

    root = serve.resolve_set(set_id, AUDITION_ROOT.parent.resolve())
    if root is None:
        return {}
    try:
        manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return manifest if isinstance(manifest, dict) else {}


def voice_of(set_id: str) -> dict:
    """What the page said it was of, from the render's own manifest.

    Read from the render rather than derived from the name: a set is a
    directory of audio, and which patch answers a program is a fallback-table
    decision that moves.
    """
    voice = manifest_of(set_id).get("voice")
    return voice if isinstance(voice, dict) else {}


#: The comparison a v1 note is counted under, by the scope its source was rendered at.
DEFAULT_COMPARISON = {
    claims.SCOPE_PRODUCT: policy.GM_GS_PRODUCT,
    claims.SCOPE_INSTRUMENT: policy.INSTRUMENT_DI,
}


def roles_of(set_id: str) -> dict[str, str]:
    """Each version's role on the page -- `model`, `reference` or `comparison` -- by key."""
    sources = manifest_of(set_id).get("sources")
    if not isinstance(sources, dict):
        return {}
    return {k: str(v.get("role") or "") for k, v in sources.items() if isinstance(v, dict)}


def judged(entry: dict, roles: dict[str, str]) -> str:
    """The library version a note is about, or "" when the log cannot say.

    The page records whichever version was selected when written, and a note
    written while the reference sounds is still about the model -- only
    `against` (recorded since the page began writing it) says which one.
    """
    cond = entry.get("conditions") or {}
    version = str(cond.get("version") or "")
    if roles.get(version) == "reference":
        return str(cond.get("against") or "")
    return version


def scope_of(subject: str, manifest: dict) -> str:
    """The scope a source was rendered at, from the page, a v1 `path: direct` or a `-di` key."""
    source = (manifest.get("sources") or {}).get(subject) or {}
    scope = source.get("scope") if isinstance(source, dict) else None
    if scope in DEFAULT_COMPARISON:
        return scope
    direct = isinstance(source, dict) and source.get("path") == "direct"
    direct = direct or subject.endswith("-di")
    return claims.SCOPE_INSTRUMENT if direct else claims.SCOPE_PRODUCT


def evaluation_of(entry: dict, manifest: dict, roles: dict[str, str]) -> dict:
    """What a note judged, at which scope and against what.

    A v2 note says so itself. A v1 note is read through the page: its subject's
    `scope` (or a v1 `path: direct`) places it, and its comparison is the one
    that scope defaults to.
    """
    evaluation = entry.get("evaluation")
    if entry.get("schema_version") == 2 and isinstance(evaluation, dict):
        judged_source = str(evaluation.get("judged_source") or "")
        return {
            "comparison_id": str(evaluation.get("comparison_id") or ""),
            # A v2 note on a page with no comparisons carries no scope; read it as v1 does.
            "scope": str(evaluation.get("scope") or "") or scope_of(judged_source, manifest),
            "judged": judged_source,
            "oracle": str(evaluation.get("oracle_source") or ""),
            "blind": bool(evaluation.get("blind")),
        }
    subject = judged(entry, roles)
    cond = entry.get("conditions") or {}
    against = cond.get("compared_against")
    return {
        "comparison_id": "",
        "scope": scope_of(subject, manifest),
        "judged": subject,
        "oracle": str(against.get("version") or "") if isinstance(against, dict) else "",
        "blind": bool(cond.get("blind")),
    }


def freshness(
    entry: dict, manifest: dict, evaluation: dict, units: list[str], last_moved: str
) -> tuple[str, list[str], dict]:
    """Whether the recording a note was taken on is still the voice: `(state, reasons, bank)`.

    `bank` is the registry state the recording was rendered under -- its
    generation and the versions of `units` -- when it could be established.
    """
    evidence = entry.get("evidence")
    at = str(entry.get("at") or "")[:10]
    predates = last_moved and at < last_moved
    stale_by_date = (
        claims.STALE,
        [f"heard {at}, before {'/'.join(units)} last moved on {last_moved}"],
        {},
    )
    if entry.get("schema_version") != 2 or not isinstance(evidence, dict):
        if predates:
            return stale_by_date
        why = "v1 note" if entry.get("schema_version") != 2 else "the note recorded no evidence"
        return claims.UNVERIFIED, [f"{why}: which recording was heard is unknown"], {}
    subject = evaluation["judged"]
    if not subject:
        return claims.UNVERIFIED, ["a blind run names no single recording"], {}
    if manifest.get("schema_version") != 2:
        if predates:
            return stale_by_date
        return claims.UNVERIFIED, ["the page's manifest records no render evidence"], {}
    item = next(
        (i for i in manifest.get("items") or [] if i.get("id") == evidence.get("take")), None
    )
    held = ((item or {}).get("evidence") or {}).get(subject)
    if not isinstance(held, dict):
        return (
            claims.STALE,
            [f"{subject} on take {evidence.get('take')} is no longer on the page"],
            {},
        )
    if held.get("asset_id") != evidence.get("asset_id"):
        return claims.STALE, ["the recording heard has since been re-rendered"], {}
    if evidence.get("set_generation") != manifest.get("set_generation"):
        return (
            claims.UNVERIFIED,
            ["the page was regenerated; what was heard beside this recording cannot be matched"],
            {},
        )
    if (held.get("request_id"), held.get("build_id")) != (
        evidence.get("request_id"),
        evidence.get("build_id"),
    ):
        return claims.UNVERIFIED, ["the note's request or build id disagrees with the page"], {}
    cid = evaluation["comparison_id"]
    comparison = next((c for c in manifest.get("comparisons") or [] if c.get("id") == cid), None)
    if comparison is None:
        return claims.STALE, [f"comparison {cid or '(none)'} is not defined on the page"], {}
    oracle = evaluation["oracle"]
    allowed = list(comparison.get("model_sources") or [])
    if entry.get("tag") == "prefer":
        # A preference can be put forward for a reference or comparison key too.
        allowed += comparison.get("oracle_sources") or []
    if (
        comparison.get("scope") != evaluation["scope"]
        or subject not in allowed
        or (oracle and oracle not in (comparison.get("oracle_sources") or []))
    ):
        return claims.STALE, [f"comparison {cid} no longer pairs {subject} with {oracle}"], {}
    record = held.get("path")
    if not isinstance(record, dict):
        return (
            claims.UNVERIFIED,
            ["no render-path record: the library was not a tuning build"],
            {},
        )
    if record.get("complete") is not True:
        return (
            claims.UNVERIFIED,
            ["render-path record is incomplete: the full render was not witnessed"],
            {},
        )
    return claims.bank_state(record.get("bank_registry_digest"), units, BANK_VERSIONS)


def product_rig_of(manifest: dict) -> bool | None:
    """Whether the page established that the default playback binds no rig.

    `make_audition` folds the rig-cleared render into the product one only when
    it found no rig bound, so `model` among the instrument comparison's model
    sources says False; anything else is not established.
    """
    shipped = claims.SHIPPED_SOURCE[claims.SCOPE_PRODUCT]
    for comparison in manifest.get("comparisons") or []:
        if comparison.get("id") == policy.INSTRUMENT_DI and shipped in (
            comparison.get("model_sources") or []
        ):
            return False
    return None


def drum_name(note: int) -> str:
    """What the GM map calls this drum note, from the harness's own table."""
    tools = REPO_ROOT / "tools" / "voicematch"
    if str(tools) not in sys.path:
        sys.path.append(str(tools))
    try:
        import gm_names
    except ImportError:
        return ""
    return gm_names.GM_DRUM_NAMES.get(note, "")


def note_units(entry: dict, voice: dict) -> list[str]:
    """The versioned unit(s) this note is about.

    **A kit is one part holding forty-odd instruments, and the unit of work is
    the drum note.** `bank-versions` versions `d000`-`d127` separately for that
    reason, so a note taken while the snare was sounding is about `d038` and
    says nothing about the hi-hat in the same kit. The page records which
    strike it was and which notes that strike held, so the attribution is
    already in the log and only has to be read.

    Empty where nothing can be attributed: a melodic page built without a
    tuning build names no patch, and a kit note taken between strikes names no
    note. Empty is "nothing known", never "current".
    """
    if voice.get("kit"):
        hit = (entry.get("conditions") or {}).get("hit") or {}
        struck = hit.get("notes") or [] if isinstance(hit, dict) else []
        return [
            f"d{n['note']:03d}"
            for n in struck
            if isinstance(n, dict) and isinstance(n.get("note"), int)
        ]
    patch = voice.get("patch") or ""
    return [patch] if patch else []


def unit_facts() -> dict[str, tuple[int, str]]:
    """Every versioned unit's generation and the date it last changed."""
    try:
        units = json.loads(BANK_VERSIONS.read_text(encoding="utf-8")).get("units") or {}
    except (OSError, ValueError):
        return {}
    out = {}
    for name, unit in units.items():
        if not isinstance(unit, dict):
            continue
        history = [h for h in (unit.get("history") or []) if isinstance(h, dict)]
        dates = sorted(str(h.get("date") or "") for h in history)
        out[name] = (int(unit.get("version") or 0), dates[-1] if dates else "")
    return out


def where(entry: dict, voice: dict, roles: dict[str, str] | None = None) -> str:
    """The one line saying what was sounding when this was written."""
    roles = roles or {}
    cond = entry.get("conditions") or {}
    bits = [str(cond.get("take_label") or cond.get("take") or "")]
    version = cond.get("version")
    if cond.get("blind"):
        bits.append("blind")
    elif version and roles.get(str(version)) == "reference":
        against = cond.get("against")
        bits.append(
            f"{version} sounding, about {against}"
            if against
            else f"{version} sounding (reference; model version not recorded)"
        )
    elif version:
        bits.append(str(version))
    hit = cond.get("hit")
    if isinstance(hit, dict):
        struck = [n for n in (hit.get("notes") or []) if isinstance(n, dict)]
        # On a kit the note number IS the instrument, so it is named rather
        # than left as a number a reader has to look up to know whether the
        # verdict was about a snare or a cymbal.
        if voice.get("kit") and struck:
            named = [
                f"{drum_name(n['note']) or 'note'} {n['note']}"
                for n in struck
                if isinstance(n.get("note"), int)
            ]
            bits.append(" + ".join(named))
        bits.append(f"hit {hit.get('n')}")
    at = cond.get("playhead")
    if isinstance(at, (int, float)):
        bits.append(f"{at:.2f}s")
    region = cond.get("region")
    if region:
        bits.append(f"region {region[0]:.2f}-{region[1]:.2f}s")
    return " · ".join(b for b in bits if b)


def oracle_flag(cond: dict) -> str:
    """`""` when a note was judged against the policy reference; the reason otherwise.

    `"unknown"` covers a note with no `compared_against` at all -- one written
    before the page recorded it, or one recorded explicitly as nothing having
    played yet -- since neither carries an oracle to check the verdict
    against. `"comparison"` is a real answer, not a gap: the note is dated and
    attributable, it was just taken against the layer the policy did not aim
    this voice at.
    """
    ca = cond.get("compared_against")
    if not isinstance(ca, dict) or not ca.get("role"):
        return "unknown"
    return "" if ca["role"] == "reference" else "comparison"


def kind_of(entry: dict, evaluation: dict) -> str:
    """`verdict` for a grade on a scope's default playback, else `preference` or `exploratory`.

    A grade on a candidate, a comparison capture or an unrecorded subject is
    about something that does not ship, so it is listed but not reduced.
    """
    if not entry.get("grade"):
        return "preference" if entry.get("tag") == "prefer" else "exploratory"
    if evaluation["judged"] != claims.SHIPPED_SOURCE.get(evaluation["scope"]):
        return "exploratory"
    return "verdict"


#: Freshness, best first: the order a population's headline pool is chosen in.
_POOLS = (claims.CURRENT, claims.UNVERIFIED, claims.STALE)


def populations(notes: list[dict]) -> list[dict]:
    """One reduction per comparison, so no scope's verdict overrides another's.

    The headline of each is the worst verdict among its current notes; with
    none, among the unverified; and only then among the stale, marked as such.
    """
    out: dict[str, dict] = {}
    for note in notes:
        if note["kind"] != "verdict":
            continue
        key = note["comparison_id"] or DEFAULT_COMPARISON[note["scope"]]
        out.setdefault(key, {"comparison_id": key, "scope": note["scope"], "notes": []})
        out[key]["notes"].append(note)
    for pop in out.values():
        pool = next(
            (f, [n for n in pop["notes"] if n["freshness"] == f])
            for f in _POOLS
            if any(n["freshness"] == f for n in pop["notes"])
        )
        pop["freshness"], pop["worst"] = pool[0], _worst(pool[1])
        pop["n"] = len(pop["notes"])
        pop["n_current"] = sum(n["freshness"] == claims.CURRENT for n in pop.pop("notes"))
    return sorted(
        out.values(), key=lambda p: (p["scope"] != claims.SCOPE_PRODUCT, p["comparison_id"])
    )


def digest(set_id: str, entries: list[dict]) -> dict:
    """One voice's notes, with what is known about the voice around them."""
    manifest = manifest_of(set_id)
    voice = voice_of(set_id)
    roles = roles_of(set_id)
    facts = unit_facts()
    notes = []
    for ordinal, entry in enumerate(entries):
        at = str(entry.get("at") or "")
        units = note_units(entry, voice)
        # The latest of them: a fill strikes six toms and a note about it is
        # stale as soon as any one of the six has moved under it.
        moved = max((facts.get(u, (0, ""))[1] for u in units), default="")
        cond = entry.get("conditions") or {}
        evaluation = evaluation_of(entry, manifest, roles)
        state, reasons, bank = freshness(entry, manifest, evaluation, units, moved)
        notes.append(
            {
                "id": entry.get("id"),
                "log_ordinal": ordinal,
                "at": at,
                "grade": str(entry.get("grade") or ""),
                "tag": str(entry.get("tag") or ""),
                "text": str(entry.get("text") or ""),
                "lang": str(entry.get("lang") or ""),
                "where": where(entry, voice, roles),
                "judged": evaluation["judged"],
                "schema": 2 if entry.get("schema_version") == 2 else 1,
                "comparison_id": evaluation["comparison_id"],
                "scope": evaluation["scope"],
                "oracle": evaluation["oracle"],
                "blind": evaluation["blind"],
                "kind": kind_of(entry, evaluation),
                "freshness": state,
                "freshness_reasons": reasons,
                "bank": bank,
                # The oracle the note was judged against; flag is empty for the policy reference.
                "compared_against": cond.get("compared_against"),
                "oracle_flag": oracle_flag(cond),
                "units": units,
                "last_moved": moved,
                # When it was heard against when the unit last moved, both to a
                # day; a note ON the day a voice moved is not marked.
                "predates_last_move": bool(moved and at[:10] < moved),
            }
        )
    notes.sort(key=lambda n: n["at"], reverse=True)
    pops = populations(notes)
    # The headline is the product's: a DI verdict is about the instrument alone.
    product = next((p for p in pops if p["scope"] == claims.SCOPE_PRODUCT), None)
    patch = "" if voice.get("kit") else (voice.get("patch") or "")
    return {
        "set": set_id,
        # Which version was put forward as the one to keep, and how often. A
        # voice carrying a set of recorded candidates is a question — which of
        # these should ship — and a list of notes does not answer it however
        # carefully each one is read.
        "preferred": preferred(entries),
        "blind_runs": blind_runs(entries),
        # A kit has no patch unit of its own — the per-note units on each note
        # below are what it is versioned by — so this is empty for one, and the
        # header says so rather than reporting a voice-wide generation a kit
        # does not have.
        "kit": bool(voice.get("kit")),
        "patch": patch,
        "version": facts.get(patch, (0, ""))[0],
        "last_moved": facts.get(patch, (0, ""))[1],
        "worst": product["worst"] if product else "",
        "worst_freshness": product["freshness"] if product else "",
        "worst_predates_last_move": bool(product and product["freshness"] == claims.STALE),
        "populations": pops,
        "exploratory": sum(n["kind"] == "exploratory" for n in notes),
        "notes": notes,
    }


def preferred(entries: list[dict]) -> list[dict]:
    """Each version put forward as the one to keep, most-backed first.

    Kept apart from the verdicts rather than folded into them. A preference
    ranks candidates against each other and a verdict measures one against the
    reference, so a version can be both the best of a bad set and not good
    enough — which is the state a bank of candidates is usually in.

    `sighted` is carried because it decides what the count is worth: a
    preference formed with the names visible is what somebody would ship, and a
    blind run's tally is what the ear actually separated. Summing the two would
    lose the distinction that makes the blind run worth doing.
    """
    tally: dict[str, dict] = {}
    for entry in entries:
        if entry.get("tag") != "prefer":
            continue
        cond = entry.get("conditions") or {}
        # A legacy page used the preference tag for a blind tally. A blind
        # answer is evidence about what the ear separated, not a sighted act
        # of choosing which version to ship.
        if cond.get("blind") or entry.get("blind_answers"):
            continue
        version = cond.get("version")
        if not version:
            continue
        seen = tally.setdefault(version, {"version": version, "n": 0, "takes": [], "sighted": 0})
        seen["n"] += 1
        if not cond.get("blind"):
            seen["sighted"] += 1
        take = cond.get("take")
        if take and take not in seen["takes"]:
            seen["takes"].append(take)
    return sorted(tally.values(), key=lambda v: (-v["n"], v["version"]))


def _blind_answers(entry: dict) -> list[dict]:
    """Read a blind result in the current format, with the old tally fallback.

    The page now sends one answer per take in ``blind_answers``. Before that
    field existed, the log carried only ``conditions.picks`` and
    ``conditions.unseparated``. Both are results, but neither belongs in the
    sighted preference tally.
    """
    cond = entry.get("conditions") or {}
    evidence = entry.get("evidence") or {}
    evaluation = entry.get("evaluation") or {}
    raw = entry.get("blind_answers")
    answers: list[dict] = []
    if isinstance(raw, list):
        for answer in raw:
            if not isinstance(answer, dict):
                continue
            take = answer.get("take")
            picked = answer.get("picked")
            abstained = bool(answer.get("abstained"))
            if not take or (not picked and not abstained):
                continue
            answers.append(
                {
                    "take": str(take),
                    "picked": str(picked) if picked else None,
                    "abstained": abstained,
                    "comparison_id": str(
                        answer.get("comparison_id")
                        or cond.get("comparison_id")
                        or evaluation.get("comparison_id")
                        or ""
                    ),
                    "set_generation": str(
                        answer.get("set_generation")
                        or evidence.get("set_generation")
                        or cond.get("set_generation")
                        or ""
                    ),
                }
            )
    # A partially migrated log can have an empty or malformed answer list while
    # retaining the old tally. Fall back only when no usable current answers
    # survived, so a current run is never counted twice.
    if answers:
        return answers
    picks = cond.get("picks")
    if isinstance(picks, dict):
        for take, picked in picks.items():
            if not take or not picked:
                continue
            answers.append(
                {
                    "take": str(take),
                    "picked": str(picked),
                    "abstained": False,
                    "comparison_id": str(
                        cond.get("comparison_id") or evaluation.get("comparison_id") or ""
                    ),
                    "set_generation": str(
                        evidence.get("set_generation") or cond.get("set_generation") or ""
                    ),
                }
            )
    unseparated = cond.get("unseparated")
    if isinstance(unseparated, (list, tuple, set)):
        for take in unseparated:
            if not take:
                continue
            answers.append(
                {
                    "take": str(take),
                    "picked": None,
                    "abstained": True,
                    "comparison_id": str(
                        cond.get("comparison_id") or evaluation.get("comparison_id") or ""
                    ),
                    "set_generation": str(
                        evidence.get("set_generation") or cond.get("set_generation") or ""
                    ),
                }
            )
    return answers


def blind_runs(entries: list[dict]) -> list[dict]:
    """Group blind results by comparison and set generation.

    Picks and abstentions stay in their own collections. A source picked in a
    blind run is not a preference made with its label visible, and an abstained
    take must not disappear merely because it has no source to tally.
    """
    groups: dict[tuple[str, str], dict] = {}
    for entry in entries:
        answers = _blind_answers(entry)
        if not answers:
            continue
        for answer in answers:
            key = (answer["comparison_id"], answer["set_generation"])
            run = groups.setdefault(
                key,
                {
                    "comparison_id": answer["comparison_id"],
                    "set_generation": answer["set_generation"],
                    "picked": {},
                    "abstained": [],
                },
            )
            take = answer["take"]
            if answer["abstained"]:
                run["abstained"].append(take)
            elif answer["picked"]:
                run["picked"].setdefault(answer["picked"], []).append(take)
    return [groups[key] for key in sorted(groups)]


def _worst(notes: list[dict]) -> str:
    for grade in GRADES:
        if any(n["grade"] == grade for n in notes):
            return grade
    return ""


def collect(only: list[str], grade: str) -> list[dict]:
    found = logs()
    if only:
        found = {k: v for k, v in found.items() if k in only}
    out = [digest(k, v) for k, v in sorted(found.items())]
    if grade:
        for voice in out:
            voice["notes"] = [n for n in voice["notes"] if n["grade"] == grade]
        out = [v for v in out if v["notes"]]
    # Whatever was heard most recently is what somebody is in the middle of.
    out.sort(key=lambda v: v["notes"][0]["at"] if v["notes"] else "", reverse=True)
    return out


def render(voices: list[dict], full: bool) -> str:
    if not voices:
        return (
            f"nothing has been said yet — {FEEDBACK_ROOT}\n"
            "the page writes here as it is listened to: tools/audition/serve.py"
        )
    lines = []
    total = sum(len(v["notes"]) for v in voices)
    lines.append(f"{total} note(s) on {len(voices)} voice(s) — {FEEDBACK_ROOT}")
    for voice in voices:
        head = voice["set"]
        if voice["patch"]:
            head += f"  ({voice['patch']}"
            head += f" v{voice['version']}" if voice["version"] else ""
            head += f", last moved {voice['last_moved']}" if voice["last_moved"] else ""
            head += ")"
        elif voice["kit"]:
            head += "  (a kit — versioned per drum note, so each note is dated on its own)"
        if voice["worst"]:
            head += f"  →  {MEANS.get(voice['worst'], voice['worst'])}"
            if voice["worst_predates_last_move"]:
                head += "  (nothing since the voice moved)"
            elif voice["worst_freshness"] != claims.CURRENT:
                head += f"  ({voice['worst_freshness']}: no verdict on a current recording)"
        lines.append("")
        lines.append(head)
        for pop in voice["populations"]:
            lines.append(
                f"  {pop['comparison_id']:<16} {pop['worst'] or '-':<16} "
                f"{pop['n_current']}/{pop['n']} current"
                + ("" if pop["freshness"] == claims.CURRENT else f", read from {pop['freshness']}")
            )
        if voice["exploratory"]:
            lines.append(
                f"  {'exploratory':<16} {voice['exploratory']} note(s) on candidates, "
                "comparisons or an unrecorded subject"
            )
        if voice["preferred"]:
            kept = "   ".join(
                f"{p['version']} {p['n']}"
                + ("" if p["sighted"] == p["n"] else f" ({p['sighted']} sighted)")
                for p in voice["preferred"]
            )
            lines.append(f"  put forward to keep:  {kept}")
        for run in voice.get("blind_runs", []):
            picked = "   ".join(
                f"{source} {len(takes)}" for source, takes in sorted(run["picked"].items())
            )
            abstained = (
                f"could not tell {len(run['abstained'])}"
                if run["abstained"]
                else "could not tell 0"
            )
            comparison = run["comparison_id"] or "legacy"
            generation = run["set_generation"] or "legacy"
            lines.append(f"  blind {comparison} @{generation}: {picked or 'picked 0'}; {abstained}")
        shown = voice["notes"] if full else voice["notes"][:4]
        for note in shown:
            mark = (
                (f"  ·  taken before {'/'.join(note['units'])} last moved ({note['last_moved']})")
                if note["predates_last_move"]
                else ""
            )
            unit = f"[{'/'.join(note['units'])}] " if voice["kit"] and note["units"] else ""
            # A verdict against a non-reference or unknown oracle is marked.
            flag = f"  ⚠ oracle={note['oracle_flag']}" if note["oracle_flag"] else ""
            fresh = "" if note["freshness"] == claims.CURRENT else f"  [{note['freshness']}]"
            lines.append(
                f"  {note['at'][:16]}  {note['grade'] or '-':<16} "
                f"{note['tag'] or '-':<23} {unit}{note['where']}{mark}{flag}{fresh}"
            )
            if note["text"]:
                for row in note["text"].splitlines():
                    lines.append(f"      {row}")
        if len(voice["notes"]) > len(shown):
            lines.append(
                f"      … {len(voice['notes']) - len(shown)} more (heard.py {voice['set']})"
            )
    return "\n".join(lines)


def _entry_for(entries: list[dict], note: dict) -> dict:
    """The raw log line a computed note in `digest`'s output came from.

    Server ids identify new entries; log ordinals identify legacy entries.
    """
    if note.get("id"):
        return next((entry for entry in entries if entry.get("id") == note["id"]), {})
    ordinal = note.get("log_ordinal")
    if isinstance(ordinal, int) and 0 <= ordinal < len(entries):
        return entries[ordinal]
    return {}


#: The two blocks `--signoff` writes, by the scope they are claims about.
BLOCKS = ((claims.SCOPE_PRODUCT, "music"), (claims.SCOPE_INSTRUMENT, "instrument"))


def _why_not(note: dict, roles: dict[str, str]) -> list[str]:
    """Every reason one graded note cannot be signed off, worst first."""
    out = []
    if note["grade"] not in ("ok", "acceptable"):
        out.append(MEANS.get(note["grade"], note["grade"]))
    subject = note["judged"]
    if not subject:
        out.append("an unrecorded version, not the shipped voice")
    elif subject == claims.SHIPPED_SOURCE[claims.SCOPE_INSTRUMENT]:
        out.append(
            f"{subject} is the direct-path (DI) diagnostic: at most an instrument claim, "
            "never the music block"
        )
    elif note["kind"] == "exploratory":
        role = roles.get(subject) or "candidate"
        role = "candidate" if role == "model" else role
        out.append(
            f"{subject} is a {role}, not the shipped voice ({claims.SHIPPED_SOURCE[claims.SCOPE_PRODUCT]})"
        )
    if note["freshness"] != claims.CURRENT:
        out.append(f"{note['freshness']}: {'; '.join(note['freshness_reasons'])}")
    return out


def _claim(set_id: str, manifest: dict, note: dict, entry: dict) -> tuple[dict | None, list[str]]:
    """One block for a current verdict on a scope's default playback, or the reasons there is none."""
    scope = note["scope"]
    cid = note["comparison_id"]
    comparison = next((c for c in manifest.get("comparisons") or [] if c.get("id") == cid), {})
    if not note["oracle"]:
        return None, [f"{cid}: no reference was recorded beside the verdict"]
    voice = manifest.get("voice") or {}
    capture = claims.capture_of_timbre(note["oracle"], str(voice.get("capture") or ""))
    if capture is None:
        return None, [f"{cid}: no capture in this tree holds {note['oracle']}"]
    program = int(voice.get("program", 0) or 0)
    boundary = claims.boundary_record(
        capture,
        claims.reference_raw(capture),
        scope,
        program,
        product_rig_of(manifest),
        str(comparison.get("status") or ""),
        bool(comparison.get("may_sign_off")),
    )
    evidence = entry.get("evidence") or {}
    item = next(i for i in manifest["items"] if i.get("id") == evidence.get("take"))
    held = item["evidence"][note["judged"]]
    path = held.get("path") if isinstance(held, dict) else None
    if not isinstance(path, dict) or path.get("complete") is not True:
        return None, [f"{cid}: render-path record is incomplete"]
    oracle_held = item["evidence"].get(note["oracle"])
    if not isinstance(oracle_held, dict):
        return None, [f"{cid}: oracle {note['oracle']} has no render evidence"]
    raw = claims.reference_raw(capture)
    if raw is None:
        return None, [f"{cid}: reference capture {capture} is not in this tree"]
    if oracle_held.get("status") != claims.ORACLE_VERIFIED:
        return None, [
            f"{cid}: oracle {note['oracle']} is {oracle_held.get('status') or 'unverified'}"
        ]
    if not all(oracle_held.get(k) for k in ("request_id", "source_id", "asset_id")):
        return None, [f"{cid}: oracle {note['oracle']} has incomplete source identity"]
    bank = note["bank"]
    units = bank.get("units") or {}
    facts = unit_facts()
    # The unit whose bump dated this note, as the version a hand reader looks for.
    unit = max(units, key=lambda u: facts.get(u, (0, ""))[1]) if units else ""
    provenance = {
        "date": note["at"][:10],
        "bank_generation": bank.get("bank_generation", 0),
        "patch_version": units.get(unit, 0),
    }
    record = {
        "comparison_id": cid,
        "scope": scope,
        "set": set_id,
        "set_generation": evidence.get("set_generation"),
        "take": evidence.get("take"),
        "judged": note["judged"],
        "program": program,
        "request_id": held.get("request_id"),
        "asset_id": held.get("asset_id"),
        "build_id": held.get("build_id"),
        "path_complete": path.get("complete"),
        "bank_registry_digest": path.get("bank_registry_digest"),
        "units": units,
        "counterpart": {
            "source": note["oracle"],
            "capture": capture,
            "capture_digest": claims.capture_digest(raw),
            "request_id": oracle_held.get("request_id"),
            "source_id": oracle_held.get("source_id"),
            "asset_id": oracle_held.get("asset_id"),
            "status": oracle_held.get("status"),
            "render_context": {
                "program": program,
                "bank": (item.get("meta") or {}).get("bank", raw.get("bank", 0)),
                "channel": (item.get("meta") or {}).get("channel", held.get("channel", 0)),
                "sends": (item["evidence"].get("model") or {}).get("sends", [0, 0, 0]),
            },
        },
        "boundary": boundary,
        "blind": note["blind"],
        "heard": note["at"],
    }
    state, why = claims.claim_eligibility(
        record,
        claims.Provenance(
            date=provenance["date"],
            bank_generation=provenance["bank_generation"],
            patch_version=provenance["patch_version"],
        ),
        scope=scope,
        registry=BANK_VERSIONS,
    )
    if state != claims.CURRENT:
        return None, [f"{cid}: {state}: {'; '.join(why)}"]
    block = {"provenance": provenance, "take": str(evidence.get("take") or ""), "evidence": record}
    if scope == claims.SCOPE_INSTRUMENT:
        block["rig_evidence"] = boundary["rig_evidence"]
    return block, []


def signoff(set_id: str) -> dict:
    """The blocks one voice's current verdicts support, keyed `music` and `instrument`.

    `music` comes only from a current verdict on the product scope's default
    playback (`model`, no overrides) whose comparison `signoff.claim_eligibility`
    re-derives as matched and signable; `instrument` from the same on the
    instrument scope, never in place of `music`. Each scope's standing verdict
    is the worst of its current ones. Raises `ValueError` naming every reason
    when neither can be written. No listener text is carried.
    """
    entries = logs().get(set_id)
    if not entries:
        raise ValueError(f"no notes for {set_id}")
    manifest = manifest_of(set_id)
    roles = roles_of(set_id)
    voice = digest(set_id, entries)
    graded = [n for n in voice["notes"] if n["grade"]]
    if not graded:
        raise ValueError(f"{set_id}: nothing but a preference tag -- a preference carries no grade")
    blocks: dict[str, dict] = {}
    refusals: list[str] = []
    for scope, name in BLOCKS:
        pool = [
            n
            for n in graded
            if n["kind"] == "verdict" and n["scope"] == scope and n["freshness"] == claims.CURRENT
        ]
        if not pool:
            continue
        worst = _worst(pool)
        chosen = next(n for n in pool if n["grade"] == worst)
        if worst not in ("ok", "acceptable"):
            refusals.append(
                f"{scope}: standing verdict is {worst} ({chosen['at'][:10]}) -- "
                f"{MEANS.get(worst, worst)}"
            )
            continue
        block, why = _claim(set_id, manifest, chosen, _entry_for(entries, chosen))
        if block is None:
            refusals += why
        else:
            blocks[name] = block
    if blocks:
        return blocks
    for note in graded:
        if note["kind"] == "verdict" and note["freshness"] == claims.CURRENT:
            continue
        refusals.append(
            f"{note['at'][:10]} {note['grade']} on {note['judged'] or 'an unrecorded version'}: "
            + "; ".join(_why_not(note, roles))
        )
    raise ValueError(f"{set_id}: nothing to sign off\n  " + "\n  ".join(refusals))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sets", nargs="*", help="set ids; default every voice with notes")
    ap.add_argument(
        "--grade", default="", choices=("", *GRADES), help="only notes carrying this verdict"
    )
    ap.add_argument("--json", action="store_true", help="the same, structured")
    ap.add_argument(
        "--signoff",
        metavar="SET",
        help="print the `music` (product) and `instrument` (DI) blocks one voice's "
        "current verdicts support, ready to paste into tools/voicematch/signoff.json",
    )
    args = ap.parse_args(argv)

    if args.signoff:
        try:
            blocks = signoff(args.signoff)
        except ValueError as e:
            print(str(e), file=sys.stderr)
            return 1
        print(
            ",\n".join(
                f'"{k}": {json.dumps(v, ensure_ascii=False, indent=2)}' for k, v in blocks.items()
            )
        )
        return 0

    voices = collect(args.sets, args.grade)
    if args.json:
        print(json.dumps(voices, ensure_ascii=False, indent=2))
    else:
        print(render(voices, full=bool(args.sets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
