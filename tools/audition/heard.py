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

A NOTE IS DATED AND A VOICE MOVES. `tools/bank-versions.json` records when each
unit last changed, so a note taken before that is marked: the voice it was
taken on is not the one in the tree. That is not "answered" — nothing here
knows whether the bump addressed what was heard — it means re-audition before
acting rather than acting on a description of an older render.

A KIT IS ONE PART AND FORTY-ODD INSTRUMENTS, and its unit is the drum note. A
kit has no patch unit at all — `d000`-`d127` are versioned separately — so a
verdict filed against "the kit" is attributable to nothing. The log carries
which strike the note was taken on and which notes that strike held, so each
one resolves to its own `dNNN`, is named from the GM drum map, and is dated
against that unit alone.

Only the standard library, like the server that writes the log.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]

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


def voice_of(set_id: str) -> dict:
    """What the page said it was of, from the render's own manifest.

    Read from the render rather than derived from the name: a set is a
    directory of audio, and which patch answers a program is a fallback-table
    decision that moves.
    """
    try:
        manifest = json.loads(
            (AUDITION_ROOT / set_id / "manifest.json").read_text(encoding="utf-8")
        )
    except (OSError, ValueError):
        return {}
    voice = manifest.get("voice")
    return voice if isinstance(voice, dict) else {}


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


def where(entry: dict, voice: dict) -> str:
    """The one line saying what was sounding when this was written."""
    cond = entry.get("conditions") or {}
    bits = [str(cond.get("take_label") or cond.get("take") or "")]
    version = cond.get("version")
    if cond.get("blind"):
        bits.append("blind")
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


def digest(set_id: str, entries: list[dict]) -> dict:
    """One voice's notes, with what is known about the voice around them."""
    voice = voice_of(set_id)
    facts = unit_facts()
    notes = []
    for entry in entries:
        at = str(entry.get("at") or "")
        units = note_units(entry, voice)
        # The latest of them: a fill strikes six toms and a note about it is
        # stale as soon as any one of the six has moved under it.
        moved = max((facts.get(u, (0, ""))[1] for u in units), default="")
        notes.append(
            {
                "at": at,
                "grade": str(entry.get("grade") or ""),
                "tag": str(entry.get("tag") or ""),
                "text": str(entry.get("text") or ""),
                "lang": str(entry.get("lang") or ""),
                "where": where(entry, voice),
                "units": units,
                "last_moved": moved,
                # Compared as dates, which is all the log records to a day's
                # resolution on the bump side. A note ON the day a voice moved is
                # not marked: nothing here can order two events inside one day.
                "predates_last_move": bool(moved and at[:10] < moved),
            }
        )
    notes.sort(key=lambda n: n["at"], reverse=True)
    # The worst verdict still standing, which is the one a reader acts on. A
    # note taken before the voice moved is not it: the render it describes is
    # gone, and a summary line built from one sends somebody to fix a voice
    # nobody has heard. It is only fallen back to when nothing newer has a
    # verdict at all, and then it is marked as what it is.
    live = [n for n in notes if not n["predates_last_move"]]
    worst = _worst(live)
    stale = not worst
    if stale:
        worst = _worst(notes)
    patch = "" if voice.get("kit") else (voice.get("patch") or "")
    return {
        "set": set_id,
        # Which version was put forward as the one to keep, and how often. A
        # voice carrying a set of recorded candidates is a question — which of
        # these should ship — and a list of notes does not answer it however
        # carefully each one is read.
        "preferred": preferred(entries),
        # A kit has no patch unit of its own — the per-note units on each note
        # below are what it is versioned by — so this is empty for one, and the
        # header says so rather than reporting a voice-wide generation a kit
        # does not have.
        "kit": bool(voice.get("kit")),
        "patch": patch,
        "version": facts.get(patch, (0, ""))[0],
        "last_moved": facts.get(patch, (0, ""))[1],
        "worst": worst,
        "worst_predates_last_move": bool(worst and stale),
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
        lines.append("")
        lines.append(head)
        if voice["preferred"]:
            kept = "   ".join(
                f"{p['version']} {p['n']}"
                + ("" if p["sighted"] == p["n"] else f" ({p['sighted']} sighted)")
                for p in voice["preferred"]
            )
            lines.append(f"  put forward to keep:  {kept}")
        shown = voice["notes"] if full else voice["notes"][:4]
        for note in shown:
            mark = (
                (f"  ·  taken before {'/'.join(note['units'])} last moved ({note['last_moved']})")
                if note["predates_last_move"]
                else ""
            )
            unit = f"[{'/'.join(note['units'])}] " if voice["kit"] and note["units"] else ""
            lines.append(
                f"  {note['at'][:16]}  {note['grade'] or '-':<16} "
                f"{note['tag'] or '-':<23} {unit}{note['where']}{mark}"
            )
            if note["text"]:
                for row in note["text"].splitlines():
                    lines.append(f"      {row}")
        if len(voice["notes"]) > len(shown):
            lines.append(
                f"      … {len(voice['notes']) - len(shown)} more (heard.py {voice['set']})"
            )
    return "\n".join(lines)


def bank_generation_at(day: str) -> int:
    """The registry's generation as it stood on a given day, not today's.

    The highest generation any unit's history carries at or before that day --
    the registry records bumps rather than every day's value, so a day with no
    bump of its own correctly inherits the last one below it.
    """
    try:
        raw = json.loads(BANK_VERSIONS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 0
    return max(
        (
            int(h.get("generation") or 0)
            for u in (raw.get("units") or {}).values()
            if isinstance(u, dict)
            for h in (u.get("history") or [])
            if isinstance(h, dict) and str(h.get("date") or "") <= day
        ),
        default=0,
    )


def _standing_note(voice: dict) -> dict:
    """The one note behind a voice's own headline verdict.

    Reads the same pool `digest` reduces to its headline -- current notes, or
    every note once none of them carry a grade at all -- so this can never
    name a note the summary line itself would not point to.
    """
    notes = voice["notes"]
    pool = (
        notes
        if voice["worst_predates_last_move"]
        else [n for n in notes if not n["predates_last_move"]]
    )
    return next(n for n in pool if n["grade"] == voice["worst"])


def _entry_for(entries: list[dict], note: dict) -> dict:
    """The raw log line a computed note in `digest`'s output came from.

    `at` is recorded to the second, so two notes in the same second with the
    same grade are indistinguishable here; the first in log order wins.
    """
    for entry in entries:
        if (
            str(entry.get("at") or "") == note["at"]
            and str(entry.get("grade") or "") == note["grade"]
        ):
            return entry
    return {}


def signoff(set_id: str) -> dict:
    """The `music` block for one voice, filled from what is already on disk.

    Raises `ValueError` naming the reason nothing can be signed off -- no
    verdict stands, the standing one says it is not the instrument, or the
    note behind it predates the voice's own last move -- rather than handing
    back a record with the gap papered over. `by` and `note` are the human's
    words and are never guessed; they come back empty.
    """
    entries = logs().get(set_id)
    if not entries:
        raise ValueError(f"no notes for {set_id}")
    voice = digest(set_id, entries)
    worst = voice["worst"]
    if worst not in ("ok", "acceptable"):
        if not worst:
            raise ValueError(
                f"{set_id}: nothing but a preference tag -- a preference carries no grade"
            )
        chosen = _standing_note(voice)
        raise ValueError(
            f"{set_id}: standing verdict is {worst} ({chosen['at'][:10]}) -- "
            f"{MEANS.get(worst, worst)}"
        )
    chosen = _standing_note(voice)
    if chosen["predates_last_move"]:
        raise ValueError(
            f"{set_id}: note taken {chosen['at'][:10]} predates "
            f"{'/'.join(chosen['units'])}'s last move on {chosen['last_moved']}"
        )
    entry = _entry_for(entries, chosen)
    take = str((entry.get("conditions") or {}).get("take") or "")
    facts = unit_facts()
    units = chosen["units"]
    # The unit whose bump dated this note, so the version reported and the
    # date it is checked against describe the same unit.
    unit = max(units, key=lambda u: facts.get(u, (0, ""))[1]) if units else ""
    return {
        "provenance": {
            "date": chosen["at"][:10],
            "bank_generation": bank_generation_at(chosen["at"][:10]),
            "patch_version": facts.get(unit, (0, ""))[0],
        },
        "take": take,
        "by": "",
        "note": "",
    }


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
        help="print a `music` block for one voice, ready to paste into "
        "tools/voicematch/signoff.json",
    )
    args = ap.parse_args(argv)

    if args.signoff:
        try:
            block = signoff(args.signoff)
        except ValueError as e:
            print(str(e), file=sys.stderr)
            return 1
        print(f'"music": {json.dumps(block, ensure_ascii=False, indent=2)}')
        return 0

    voices = collect(args.sets, args.grade)
    if args.json:
        print(json.dumps(voices, ensure_ascii=False, indent=2))
    else:
        print(render(voices, full=bool(args.sets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
