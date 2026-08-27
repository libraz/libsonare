"""Cut a short passage of real music out of a Bach corpus and commit the notes.

Every take in this harness is written to isolate one thing — a note, a doubling,
a damper, a choked hi-hat. None of them is music, and a voice can pass all of
them and still be unpleasant to listen to for ten seconds: the register balance
across a real line, how a chord bloom sits under a moving part, whether the
decay leaves room for the next entry. That is what a musical take is for, and it
is judged by ear alone — nothing here measures it.

The passage is extracted once and **committed** as note data, so a clone renders
it with nothing installed. The corpus it came from is a sibling repository
(`$SONARE_BACH_ROOT`, default `../bach-mcp/data/reference`), read only when an
excerpt is (re)cut.

## What the source can and cannot give

The corpus stores onsets and durations in beats against a tempo map, which this
integrates into seconds. What it does not store is dynamics: every note of a
piece carries one velocity. So a musical take says nothing about velocity
response, and the `dynamics-*` takes remain the only thing that does — cutting a
passage does not retire them.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXCERPT_DIR = HERE / "excerpts"
DEFAULT_CORPUS = HERE.parent.parent.parent / "bach-mcp" / "data" / "reference"

#: Where the first note lands, matching every other take in `phrases.py` — the
#: renders are compared by eye on a waveform as well as by ear, and a passage
#: starting at a different offset from its neighbours is hard to read.
LEAD_IN_S = 0.3


def corpus_root() -> Path:
    return Path(os.environ.get("SONARE_BACH_ROOT") or DEFAULT_CORPUS).expanduser()


def beat_to_seconds(work: dict) -> callable:
    """A beats-to-seconds map for one work, integrating its tempo changes.

    The corpus gives tempo as a list of (tick, bpm) and beats as fractions, so
    neither alone converts an onset: a piece with a hundred tempo marks spends
    a different number of seconds in each span.
    """
    tpb = float(work.get("ticks_per_beat") or 480)
    marks = sorted(((t["tick"] / tpb, float(t["bpm"])) for t in work.get("tempos") or []),
                   key=lambda m: m[0])
    if not marks or marks[0][0] > 0:
        marks.insert(0, (0.0, 120.0))
    # Cumulative seconds at the start of each tempo span.
    starts, elapsed = [], 0.0
    for i, (beat, bpm) in enumerate(marks):
        starts.append(elapsed)
        if i + 1 < len(marks):
            elapsed += (marks[i + 1][0] - beat) * 60.0 / bpm

    def convert(beat: float) -> float:
        idx = 0
        for i, (at, _) in enumerate(marks):
            if at <= beat:
                idx = i
            else:
                break
        at, bpm = marks[idx]
        return starts[idx] + (beat - at) * 60.0 / bpm

    return convert


def cut(work: dict, *, roles: list[str], first_beat: float, last_beat: float) -> list[dict]:
    """Every note of the named tracks whose onset falls in the beat window."""
    convert = beat_to_seconds(work)
    kept = [n for t in work["tracks"] if not roles or t["role"] in roles
            for n in t["notes"] if first_beat <= n["onset"] < last_beat]
    if not kept:
        return []
    # Anchored on the first note rather than on the window, so a passage with an
    # upbeat starts where every other take does instead of opening on a rest.
    zero = convert(min(n["onset"] for n in kept))
    out = []
    for track in work["tracks"]:
        if roles and track["role"] not in roles:
            continue
        for n in track["notes"]:
            if not (first_beat <= n["onset"] < last_beat):
                continue
            start = convert(n["onset"]) - zero + LEAD_IN_S
            end = convert(n["onset"] + n["duration"]) - zero + LEAD_IN_S
            out.append({
                "pitch": int(n["pitch"]),
                "velocity": int(n["velocity"]),
                "start": round(start, 4),
                "duration": round(max(end - start, 0.02), 4),
            })
    return sorted(out, key=lambda n: (n["start"], n["pitch"]))


def build(stem: str, *, ident: str, label: str, note: str, roles: list[str],
          first_beat: float, last_beat: float) -> dict:
    path = corpus_root() / f"{stem}.json"
    if not path.is_file():
        raise SystemExit(
            f"{path} not found. Set SONARE_BACH_ROOT to the corpus, or leave the "
            f"committed excerpt alone — rendering one needs nothing.")
    work = json.loads(path.read_text())
    notes = cut(work, roles=roles, first_beat=first_beat, last_beat=last_beat)
    if not notes:
        raise SystemExit(f"{stem}: no notes in beats [{first_beat}, {last_beat})")
    span = max(n["start"] + n["duration"] for n in notes)
    return {
        "_": "Committed note data, cut from a Bach corpus by extract_excerpt.py. "
             "Judged by ear; nothing measures it. Velocity is flat across the "
             "source piece, so this says nothing about velocity response.",
        "id": ident,
        "label": label,
        "note": note,
        "source": {
            "bwv": work.get("bwv"),
            "work": stem,
            "category": work.get("category", ""),
            "instrument": work.get("instrument", ""),
            "movement": work.get("movement", ""),
            "tonic": f"{work.get('tonic', '')} {work.get('mode', '')}".strip(),
            "roles": roles or [t["role"] for t in work["tracks"]],
            "beats": [first_beat, last_beat],
        },
        "seconds": round(span, 3),
        "notes": notes,
    }


#: The committed set: one passage per tone class, chosen for what a line exposes
#: that an isolated note cannot. A class rather than an instrument, because a
#: musical take has to exist for every program and only four have a capture.
CUTS = [
    dict(stem="BWV846_prelude", ident="bwv846-prelude", roles=["manual"],
         first_beat=0.0, last_beat=16.0,
         label="WTC I — Prelude in C, opening",
         note="broken chords held under each other: the bloom of overlapping "
              "decays, and whether the next entry has room"),
    dict(stem="BWV847_fugue", ident="bwv847-fugue", roles=["manual"],
         first_beat=0.0, last_beat=18.0,
         label="WTC I — Fugue in C minor, subject and answer",
         note="two entries of one subject in different registers: whether the "
              "ring of the first is still audible under the second"),
    dict(stem="BWV639", ident="bwv639-chorale", roles=["manual", "pedal"],
         first_beat=0.0, last_beat=8.0,
         label="Ich ruf zu dir, BWV 639 — opening",
         note="a sustained line over a moving inner voice and a pedal: register "
              "balance across three parts that never stop"),
    dict(stem="BWV996_1", ident="bwv996-prelude", roles=["v1", "v2"],
         first_beat=0.0, last_beat=14.0,
         label="Lute Suite BWV 996 — Praeludium, opening",
         note="a plucked line against a held bass: how long a pluck lasts under "
              "the next one, and whether the bass survives it"),
    dict(stem="BWV1007_1", ident="bwv1007-prelude", roles=["solo_0", "solo_1"],
         first_beat=0.0, last_beat=12.0,
         label="Cello Suite No. 1 — Prelude, opening",
         note="one line across a wide compass, mostly stepwise: where a bowed or "
              "blown voice changes character as it climbs"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", default="", help="comma-separated excerpt ids")
    ap.add_argument("--check", action="store_true",
                    help="fail if a committed excerpt differs from a fresh cut")
    args = ap.parse_args()

    wanted = {s.strip() for s in args.only.split(",") if s.strip()}
    EXCERPT_DIR.mkdir(parents=True, exist_ok=True)
    stale = []
    for spec in CUTS:
        if wanted and spec["ident"] not in wanted:
            continue
        payload = json.dumps(build(**spec), indent=2, ensure_ascii=False) + "\n"
        path = EXCERPT_DIR / f"{spec['ident']}.json"
        if args.check:
            if not path.is_file() or path.read_text() != payload:
                stale.append(spec["ident"])
            continue
        path.write_text(payload)
        got = json.loads(payload)
        print(f"{path.name:24s} {got['seconds']:5.1f}s  {len(got['notes']):3d} notes  "
              f"{got['label']}")
    if args.check:
        if stale:
            print("excerpts differ from a fresh cut: " + ", ".join(stale))
            return 1
        print(f"{EXCERPT_DIR.name}/: current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
