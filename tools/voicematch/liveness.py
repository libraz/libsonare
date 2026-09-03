"""Every fit spec still names knobs that MOVE something.

`check_specs` asks whether a knob resolves; this asks whether it does anything.
The two failures look nothing alike from the outside and only the first one has
ever had a gate. A knob that resolves and is inert costs a fit an axis, widens
its covariance and reads in a report as a mechanism that was tried -- and it is
invisible, because the fit converges, the loss falls and the knob comes back at
whatever value the optimizer last handed it.

The check is the one the sweeps kept arriving at by hand: render the voice at
each end of the knob's stated range and compare the raw float32 bytes. It needs
no reference and no corpus, so it covers all 17 specs rather than the handful
with an oracle, and it cannot be talked out of an answer -- bytes differ or they
do not. What it costs is a render per range end per note, about 150 ms each.

Three verdicts, and two of them are defects:

  - `dead` -- byte-identical at every note of the grid. The knob cannot move
    this program's render at all and the spec is asserting otherwise. Fails,
    unless the knob carries a `dead` string saying why it is kept anyway.
  - `stale` -- a knob carrying that excuse that has since come alive. Fails
    too, and for the same reason the parity allowlist expires its entries: an
    excuse left behind goes on asserting a reviewed decision about a knob that
    no longer needs one.
  - `partial` -- live at some notes and not others. Reported, never failed: a
    register-graded control is *supposed* to stop somewhere, and the grid does
    not know the instrument's compass. It is worth reading anyway, because the
    note it stops at is not always the one the spec assumes -- three of this
    bank's specs were fitting the top of a compass with knobs that stop below
    it.

An excuse is a sentence in the spec beside the knob, not a line here, so it is
read by whoever is about to sweep the knob. Keeping one is a real choice: the
knob stays in the fit's covariance and in its report. The alternative is to
drop it, which is what the guitar's brightness knob got.

The program is derived rather than declared, so it cannot drift: a knob named
for a patch names the patch that voices a program, and a knob named for an
engine file names the engine, which the same catalogue maps back to a patch.
A spec neither of those reaches is skipped and said to be skipped.

    python tools/voicematch/liveness.py --lib build-tuning/lib/libsonare.dylib

A note at which NO knob in a spec moves anything is reported too. That is the
positive control: a note the voice does not sound renders silence at both ends
of every range, which reads exactly like a spec full of dead knobs.
"""

from __future__ import annotations

import argparse
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import catalogue as catalogue_mod  # noqa: E402
from catalogue import resolve_knob_name, scan_tunables  # noqa: E402
from check_specs import SPEC_DIR  # noqa: E402
from identity import render_hash  # noqa: E402

#: Seven notes at even fourths from C2 to C8. Wide rather than instrument-shaped
#: on purpose: the grid is what tells a register-graded knob from a dead one,
#: and narrowing it to a compass would hide exactly the case it exists for.
DEFAULT_NOTES = (36, 48, 60, 72, 84, 96, 108)


@dataclass
class Knob:
    """One spec entry, reduced to what a render needs."""

    name: str
    lo: float
    hi: float
    #: Why a knob known to move nothing is kept in the spec anyway. Empty for
    #: the ordinary case, where moving nothing is a defect.
    excuse: str = ""


@dataclass
class SpecReport:
    """What one spec's knobs did over the grid."""

    spec: str
    program: int | None = None
    patch: str | None = None
    skipped: str = ""
    live: dict[str, list[int]] = field(default_factory=dict)
    excuses: dict[str, str] = field(default_factory=dict)

    def dead(self) -> list[str]:
        """Knobs that moved nothing at any note and carry no excuse."""
        return sorted(
            name for name, notes in self.live.items()
            if not notes and not self.excuses.get(name)
        )

    def excused(self) -> list[str]:
        """Knobs that moved nothing and say why they are kept."""
        return sorted(
            name for name, notes in self.live.items()
            if not notes and self.excuses.get(name)
        )

    def stale(self) -> list[str]:
        """Knobs excused as dead that have since come alive."""
        return sorted(
            name for name, notes in self.live.items()
            if notes and self.excuses.get(name)
        )

    def partial(self, notes: tuple[int, ...]) -> list[str]:
        """Knobs live at some notes and not others, an excused one aside."""
        return sorted(
            name for name, hit in self.live.items()
            if hit and len(hit) < len(notes) and not self.excuses.get(name)
        )

    def silent_notes(self, notes: tuple[int, ...]) -> list[int]:
        """Notes at which nothing in this spec moved -- probably an unvoiced note."""
        if not self.live:
            return []
        return [n for n in notes if not any(n in hit for hit in self.live.values())]


def spec_entries(path: Path) -> list[Knob]:
    """Every knob in a spec that has a name and two distinct range ends."""
    raw = json.loads(path.read_text())
    entries = raw if isinstance(raw, list) else raw.get("knobs", [])
    out = []
    for e in entries:
        if not isinstance(e, dict) or "tunable" not in e:
            continue
        lo, hi = e.get("min"), e.get("max")
        if lo is None or hi is None or lo == hi:
            continue
        out.append(Knob(e["tunable"], float(lo), float(hi), str(e.get("dead", ""))))
    return out


def derive_program(knobs: list[Knob], catalogue) -> tuple[int | None, str | None, str]:
    """The (program, patch) a spec is about, or why it could not be derived.

    Two routes, in order. A knob prefixed with a patch name resolves directly
    through the program map. Otherwise a knob prefixed with an engine file stem
    (`piano_voice.kFoo`) names the engine, and the first patch the catalogue
    reports on that engine answers for it -- which is what a fit over such a
    spec is implicitly doing anyway. Both read the scoped name, so `scan_spec`
    scopes before it calls this.
    """
    patch_to_program: dict[str, int] = {}
    for (program, bank), patch in sorted(catalogue.programs.items()):
        if bank == 0:
            patch_to_program.setdefault(patch, program)
    for knob in knobs:
        head = knob.name.split(".")[0]
        if head in patch_to_program:
            return patch_to_program[head], head, ""
    for knob in knobs:
        head = knob.name.split(".")[0]
        mode = head[: -len("_voice")] if head.endswith("_voice") else head
        for patch, engine in sorted(catalogue.modes.items()):
            if engine == mode and patch in patch_to_program:
                return patch_to_program[patch], patch, ""
    return None, None, "no knob names a patch or an engine the catalogue reports"


def scan_spec(path: Path, catalogue, lib: str, notes: tuple[int, ...],
              workers: int) -> SpecReport:
    """Render every knob's range ends at every note and record where they differ."""
    # The older specs name a constant bare, and the override table's keys are
    # flat and scoped, so scope here rather than at every use.
    tunables = scan_tunables()
    knobs = [Knob(resolve_knob_name(k.name, tunables), k.lo, k.hi, k.excuse)
             for k in spec_entries(path)]
    report = SpecReport(spec=path.name)
    if not knobs:
        report.skipped = "no knob carries a tunable name and a range"
        return report
    program, patch, why = derive_program(knobs, catalogue)
    report.program, report.patch, report.skipped = program, patch, why
    if program is None:
        return report

    def probe(job: tuple[Knob, int]) -> tuple[str, int, bool]:
        knob, note = job
        a = render_hash(lib, note, program, 0, f"{knob.name}={knob.lo}")
        b = render_hash(lib, note, program, 0, f"{knob.name}={knob.hi}")
        return knob.name, note, a != b

    jobs = [(knob, note) for knob in knobs for note in notes]
    report.live = {knob.name: [] for knob in knobs}
    report.excuses = {knob.name: knob.excuse for knob in knobs if knob.excuse}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for name, note, moved in pool.map(probe, jobs):
            if moved:
                report.live[name].append(note)
    for hit in report.live.values():
        hit.sort()
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", default="build-tuning/lib/libsonare.dylib",
                    help="a -DBUILD_TUNING=ON library")
    ap.add_argument("--spec", default=None, help="one spec file, else all of them")
    ap.add_argument("--notes", default=",".join(str(n) for n in DEFAULT_NOTES))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--sr", type=int, default=48000)
    args = ap.parse_args()

    notes = tuple(int(n) for n in args.notes.split(",") if n.strip())
    paths = [Path(args.spec)] if args.spec else sorted(SPEC_DIR.glob("*.json"))
    catalogue = catalogue_mod.dump_catalogue(0, "sustain", args.lib, sr=args.sr)

    reports = [scan_spec(p, catalogue, args.lib, notes, args.workers) for p in paths]
    grid = ", ".join(str(n) for n in notes)
    print(f"{len(paths)} spec(s) over notes {grid}\n")

    failed = 0
    for r in reports:
        if r.skipped:
            print(f"specs/{r.spec}: skipped -- {r.skipped}")
            continue
        dead, stale, partial = r.dead(), r.stale(), r.partial(notes)
        excused, silent = r.excused(), r.silent_notes(notes)
        head = f"specs/{r.spec}: program {r.program} ({r.patch}), {len(r.live)} knob(s)"
        if not (dead or stale or partial or excused or silent):
            print(f"{head} -- all live at every note")
            continue
        print(head)
        for name in dead:
            print(f"  DEAD     {name}")
        for name in stale:
            hit = ", ".join(str(n) for n in r.live[name])
            print(f"  STALE    {name} -- excused as dead, live at {hit}")
        for name in excused:
            print(f"  excused  {name} -- {r.excuses[name]}")
        for name in partial:
            hit = ", ".join(str(n) for n in r.live[name])
            print(f"  partial  {name} -- live at {hit}")
        if silent:
            where = ", ".join(str(n) for n in silent)
            print(f"  note {where}: nothing in this spec moved -- probably unvoiced here")
        failed += len(dead) + len(stale)

    if failed:
        print(f"\n{failed} knob(s) fail. One that moves no render at any note is a spec "
              "asserting a mechanism this program does not have: find the switch that "
              "gates it and record it as this knob's `dead` reason, or drop the knob. "
              "One excused as dead and since come alive has an excuse to delete.")
        return 1
    print("\nno spec sweeps a knob that moves nothing without saying why")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
