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
do not. What it costs is a render per range end per note per velocity, and the
cost of a render here is its interpreter spawn rather than its audio: the
override table is read once at library load, so a render cannot share a process
with a *different* override. It can share one with the rest of its own grid,
which is what `render_batch` does -- one spawn per knob-end instead of one per
cell, and the 17 specs go from eight minutes to three and a half.

Two axes, because one is how the by-hand version kept being wrong. The note is
the axis a single-note probe misses; the velocity is the one the fitting notes
name first, since a dynamics control holds a fixed-velocity probe still and
reads exactly like a dead knob. A knob that moves at one of the two velocities
and not the other is reported rather than being folded into `partial`.

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

`--census` points the same probe at the bank instead of the specs -- per patch,
which of its own fields cannot move the render it voices -- and writes a result
stamped with the bank generation it was taken against. See `census` below for
what it deliberately leaves out and why it never fails.
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

import catalogue as catalogue_mod
from catalogue import drum_patch_key, resolve_knob_name, scan_tunables
from check_specs import SPEC_DIR
from identity import PERCUSSION_CHANNEL, render_batch

#: Below this a render has not sounded. Well under the quietest real note the
#: bank produces and well over the denormal dust a closed envelope leaves.
SILENCE_PEAK = 1.0e-6

#: Seven notes at even fourths from C2 to C8. Wide rather than instrument-shaped
#: on purpose: the grid is what tells a register-graded knob from a dead one,
#: and narrowing it to a compass would hide exactly the case it exists for.
DEFAULT_NOTES = (36, 48, 60, 72, 84, 96, 108)

#: A soft and a loud strike. Two rather than one because velocity is the axis
#: the fitting notes name first: a dynamics control holds a single-velocity
#: probe still and reads exactly like a dead knob. Two also lets a knob that
#: moves at one velocity and not the other be named as such, which is a fact
#: about the knob rather than about the grid.
DEFAULT_VELOCITIES = (32, 100)


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
    #: The GS variation the patch was probed at, since a spec scoped to one is
    #: swept at the address that sounds it rather than at the capital tone.
    bank: int = 0
    skipped: str = ""
    live: dict[str, list[int]] = field(default_factory=dict)
    excuses: dict[str, str] = field(default_factory=dict)
    #: Per knob, the velocities at which it moved anything at all.
    velocities: dict[str, set[int]] = field(default_factory=dict)

    def velocity_gated(self, velocities: tuple[int, ...]) -> list[str]:
        """Knobs live at one velocity of several -- a dynamics control, named."""
        if len(velocities) < 2:
            return []
        return sorted(
            name for name, hit in self.velocities.items()
            if len(hit) == 1 and not self.excuses.get(name)
        )

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


def patch_addresses(catalogue) -> dict[str, tuple[int, int]]:
    """Each patch the bank has, once, at the address to render it at.

    Bank 0 wins where a patch has one, since that is the address a plain GM file
    uses. Thirty patches have no bank-0 address at all -- the GS variations,
    where a registration differs from the program it varies -- and rendering one
    at bank 0 does not give a wrong answer but no answer: the capital tone sounds
    instead, so an override scoped to the variation moves nothing and the patch
    reads as having no live field. The spec sweep and the census both need this
    map, and they need the same one: a patch addressable by one and not the other
    is how the census came to miss thirty of them.
    """
    seen: dict[str, tuple[int, int]] = {}
    for (program, bank), patch in sorted(catalogue.programs.items(), key=lambda kv: kv[0][::-1]):
        seen.setdefault(patch, (program, bank))
    return seen


def derive_program(knobs: list[Knob], catalogue) -> tuple[int | None, str | None, int, str]:
    """The (program, patch, bank) a spec is about, or why it could not be derived.

    Two routes, in order. A knob prefixed with a patch name resolves directly
    through the address map, at that patch's own address -- a variation included,
    since the capital tone would not answer for it. Otherwise a knob prefixed
    with an engine file stem (`piano_voice.kFoo`) names the engine, and a patch
    the catalogue reports on that engine stands in for it -- which is what a fit
    over such a spec is implicitly doing anyway. The stand-in prefers a capital
    tone, because an engine constant is shared by every patch on the engine and
    the choice among them should land on the address a plain GM file reaches;
    a variation answers only for an engine that has no bank-0 patch at all.
    Both routes read the scoped name, so `scan_spec` scopes before calling this.
    """
    addresses = patch_addresses(catalogue)
    for knob in knobs:
        head = knob.name.split(".")[0]
        if head in addresses:
            program, bank = addresses[head]
            return program, head, bank, ""
    for knob in knobs:
        head = knob.name.split(".")[0]
        mode = head.removesuffix("_voice")
        on_engine = sorted((patch for patch, engine in catalogue.modes.items()
                            if engine == mode and patch in addresses),
                           key=lambda p: (addresses[p][1] != 0, p))
        if on_engine:
            program, bank = addresses[on_engine[0]]
            return program, on_engine[0], bank, ""
    return None, None, 0, "no knob names a patch or an engine the catalogue reports"


def probe_knobs(knobs: list[Knob], lib: str, program: int, channel: int,
                notes: tuple[int, ...], velocities: tuple[int, ...],
                workers: int, bank: int = 0
                ) -> tuple[dict[str, list[int]], dict[str, set[int]]]:
    """Render each knob's range ends over the grid; where it moved, and at which velocity.

    One subprocess per knob-end rather than per cell: the override is fixed at
    library load and the grid is not, so the whole grid rides on one spawn.
    """
    cells = [(note, vel) for note in notes for vel in velocities]

    def probe(job: tuple[Knob, float]) -> tuple[str, list[str]]:
        knob, value = job
        got = render_batch(lib, program, channel, cells, f"{knob.name}={value}", bank)
        return knob.name, [digest for digest, _ in got]

    jobs = [(knob, value) for knob in knobs for value in (knob.lo, knob.hi)]
    ends: dict[str, list[list[str]]] = {knob.name: [] for knob in knobs}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for name, digests in pool.map(probe, jobs):
            ends[name].append(digests)

    live: dict[str, set[int]] = {knob.name: set() for knob in knobs}
    moved_at: dict[str, set[int]] = {knob.name: set() for knob in knobs}
    for name, (lo_side, hi_side) in ends.items():
        for (note, vel), a, b in zip(cells, lo_side, hi_side):
            if a != b:
                live[name].add(note)
                moved_at[name].add(vel)
    return {name: sorted(hit) for name, hit in live.items()}, moved_at


def scan_spec(path: Path, catalogue, lib: str, notes: tuple[int, ...],
              velocities: tuple[int, ...], workers: int) -> SpecReport:
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
    program, patch, bank, why = derive_program(knobs, catalogue)
    report.program, report.patch, report.bank, report.skipped = program, patch, bank, why
    if program is None:
        return report

    report.excuses = {knob.name: knob.excuse for knob in knobs if knob.excuse}
    report.live, report.velocities = probe_knobs(
        knobs, lib, program, 0, notes, velocities, workers, bank)
    return report


#: Three notes in the middle of the compass. A census screens rather than
#: proves: anything it names earns the per-semitone ladder the fitting notes
#: describe, so a wider grid here would buy resolution nothing reads.
CENSUS_NOTES = (48, 60, 72)


@dataclass
class PatchReport:
    """One patch's own fields, and which of them reach its render."""

    patch: str
    program: int
    channel: int
    #: The GS variation this patch was probed at. A patch reachable from no
    #: bank-0 program has no other address.
    bank: int = 0
    inert: list[str] = field(default_factory=list)
    total: int = 0
    #: Set when the patch rendered silence, in which case `inert` says nothing.
    silent: bool = False

    def share(self) -> float:
        return len(self.inert) / self.total if self.total else 0.0


def census_jobs(catalogue, notes: tuple[int, ...], drums: bool
                ) -> list[tuple[str, int, int, int, tuple[int, ...], int | None]]:
    """Every melodic patch at its own address, plus one job per drum note.

    The addresses are `patch_addresses`; what this adds is the grid and the
    channel each job is probed on.
    """
    jobs = [(patch, program, bank, 0, notes, None)
            for patch, (program, bank) in sorted(patch_addresses(catalogue).items())]
    if drums:
        jobs += [(drum_patch_key(n), 0, 0, PERCUSSION_CHANNEL, (n,), n) for n in range(128)]
    return jobs


def census(catalogue, lib: str, notes: tuple[int, ...], velocities: tuple[int, ...],
           workers: int, drums: bool):
    """Per patch, which of its own fields cannot move the render it voices.

    The bank's patches rather than its programs, because one patch commonly
    voices several and probing each program would ask the same question 128
    times. Engine constants are left out: they are shared by every patch on
    that engine, so a null against one program says nothing about the constant.

    A drum note's patch is addressed by note rather than through the program
    map, and sounds on the percussion channel at its own note -- so its grid is
    that one note, which is all a drum note has.

    Yields one report per patch as it finishes rather than returning the set,
    because the bank takes minutes and a run that prints nothing until the end
    is indistinguishable from a hung one.
    """
    from knobs import auto_spec

    for patch, program, bank, channel, grid, drum_note in census_jobs(catalogue, notes, drums):
        try:
            entries = auto_spec(program, catalogue, drum_note=drum_note, bank=bank)
        except ValueError:
            continue  # a drum note outside the kit, or a program with no patch
        knobs = [Knob(e["tunable"], float(e["min"]), float(e["max"]))
                 for e in entries if e["tunable"].startswith(patch + ".")
                 and e["min"] != e["max"]]
        if not knobs:
            continue
        # One unmodified render first. The catalogue reports a patch for all 128
        # drum notes while a kit sounds about 47 of them, and a note it does not
        # sound renders silence -- under which every field is byte-identical and
        # the patch would otherwise be counted as wholly inert.
        loudest = max(peak for _, peak in render_batch(
            lib, program, channel, [(n, max(velocities)) for n in grid], "", bank))
        if loudest < SILENCE_PEAK:
            yield PatchReport(patch=patch, program=program, bank=bank, channel=channel,
                              total=len(knobs), silent=True)
            continue
        live, _ = probe_knobs(knobs, lib, program, channel, grid, velocities, workers, bank)
        yield PatchReport(
            patch=patch, program=program, bank=bank, channel=channel, total=len(knobs),
            inert=sorted(name for name, hit in live.items() if not hit))


#: `tools/bank-versions.json`, whose generation a census is only valid against.
BANK_VERSIONS = Path(__file__).resolve().parents[1] / "bank-versions.json"


def bank_generation() -> int | None:
    """The bank generation the registry currently records, if it is readable."""
    try:
        return int(json.loads(BANK_VERSIONS.read_text())["bank_generation"])
    except (OSError, KeyError, ValueError):
        return None


def write_census(path: Path, reports: list[PatchReport], notes: tuple[int, ...],
                 velocities: tuple[int, ...]) -> None:
    """Write the census, stamped with the bank generation it was taken against.

    The stamp is what makes a committed census honest without an hour-scale
    check target: a voice fitted or a family rebalanced moves the generation,
    and a census recorded against an older one is a claim about a bank nobody
    is running any more. `--census-check` compares the two and says so.
    """
    path.write_text(json.dumps({
        "_": ("Per patch, which of its own fields could not move the render it voices. "
              "Generated by `make spec-liveness-census`; see tools/voicematch/docs/"
              "fitting.md. A census screens rather than proves - a null here earns the "
              "per-semitone ladder, and a patch is free not to use a field its engine "
              "offers, so this is not a defect list."),
        "bank_generation": bank_generation(),
        "notes": list(notes),
        "velocities": list(velocities),
        "patches": {
            r.patch: {
                "program": r.program,
                # Absent means bank 0, which is every patch a plain GM file
                # reaches; a variation carries the address it was probed at.
                **({"bank": r.bank} if r.bank else {}),
                "channel": r.channel,
                "fields": r.total,
                "silent": r.silent,
                "inert": [n.split(".", 1)[1] for n in r.inert],
            }
            for r in sorted(reports, key=lambda r: r.patch)
        },
    }, indent=2) + "\n")


def check_census(path: Path) -> int:
    """Fail when a recorded census predates the bank it claims to describe."""
    if not path.exists():
        print(f"{path}: no census recorded -- run `make spec-liveness-census`")
        return 1
    recorded = json.loads(path.read_text()).get("bank_generation")
    current = bank_generation()
    if recorded == current:
        print(f"{path}: current (bank generation {current})")
        return 0
    print(f"{path}: taken against bank generation {recorded}, the bank is now {current}. "
          "A voice moved since this was measured, so what it says about that voice's "
          "fields is a claim about a bank nobody runs. Regenerate it or delete it.")
    return 1


def run_census(args, catalogue, velocities: tuple[int, ...]) -> int:
    notes = tuple(int(n) for n in args.notes.split(",")) if args.notes else CENSUS_NOTES
    grid = ", ".join(str(n) for n in notes)
    vels = ", ".join(str(v) for v in velocities)
    print(f"notes {grid} at velocities {vels}, each patch's own fields only\n")
    reports = []
    for r in census(catalogue, args.lib, notes, velocities, args.workers, args.drums):
        reports.append(r)
        if r.silent:
            print(f"  {r.patch:24s} silent -- not probed", flush=True)
            continue
        pct = round(100 * r.share())
        print(f"  {r.patch:24s} {len(r.inert):3d} of {r.total:3d} inert ({pct:3d}%)",
              flush=True)

    sounded = [r for r in reports if not r.silent]
    silent = [r for r in reports if r.silent]
    print("\n== what each patch cannot reach ==")
    for r in sorted(sounded, key=lambda r: (-r.share(), r.patch)):
        if not r.inert:
            continue
        print(f"{r.patch}: {len(r.inert)} of {r.total}")
        for name in r.inert:
            print(f"  {name.split('.', 1)[1]}")
    clean = [r for r in sounded if not r.inert]
    print(f"\n{len(clean)} patch(es) reach every one of their own fields.")
    if silent:
        names = ", ".join(r.patch for r in silent)
        print(f"{len(silent)} patch(es) rendered silence and were not probed: {names}")
    total = sum(r.total for r in sounded)
    dead = sum(len(r.inert) for r in sounded)
    print(f"{dead} of {total} patch fields across {len(sounded)} sounding patches move no "
          "render over this grid. That is a census, not a verdict: a patch is free not to "
          "use a field its engine offers, and what a null here earns is the per-semitone "
          "ladder.")
    if args.out:
        write_census(Path(args.out), reports, notes, velocities)
        print(f"wrote {args.out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", default="build-tuning/lib/libsonare.dylib",
                    help="a -DBUILD_TUNING=ON library")
    ap.add_argument("--spec", default=None, help="one spec file, else all of them")
    ap.add_argument("--census", action="store_true",
                    help="every patch's own fields instead of the specs")
    ap.add_argument("--drums", action="store_true",
                    help="with --census, include the drum-note patches")
    ap.add_argument("--out", default=None,
                    help="with --census, also write the result as JSON")
    ap.add_argument("--census-check", default=None, metavar="PATH",
                    help="report whether a recorded census still matches the bank")
    ap.add_argument("--notes", default="")
    ap.add_argument("--velocities", default=",".join(str(v) for v in DEFAULT_VELOCITIES))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--sr", type=int, default=48000)
    args = ap.parse_args()

    if args.census_check:
        return check_census(Path(args.census_check))

    velocities = tuple(int(v) for v in args.velocities.split(",") if v.strip())
    catalogue = catalogue_mod.dump_catalogue(0, "sustain", args.lib, sr=args.sr)
    if args.census:
        return run_census(args, catalogue, velocities)

    notes = tuple(int(n) for n in (args.notes or ",".join(str(n) for n in DEFAULT_NOTES))
                  .split(",") if n.strip())
    paths = [Path(args.spec)] if args.spec else sorted(SPEC_DIR.glob("*.json"))

    reports = [scan_spec(p, catalogue, args.lib, notes, velocities, args.workers)
               for p in paths]
    grid = ", ".join(str(n) for n in notes)
    vels = ", ".join(str(v) for v in velocities)
    print(f"{len(paths)} spec(s) over notes {grid} at velocities {vels}\n")

    failed = 0
    for r in reports:
        if r.skipped:
            print(f"specs/{r.spec}: skipped -- {r.skipped}")
            continue
        dead, stale, partial = r.dead(), r.stale(), r.partial(notes)
        excused, silent = r.excused(), r.silent_notes(notes)
        gated = r.velocity_gated(velocities)
        at = f"program {r.program}" + (f" bank {r.bank}" if r.bank else "")
        head = f"specs/{r.spec}: {at} ({r.patch}), {len(r.live)} knob(s)"
        if not (dead or stale or partial or excused or silent or gated):
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
        for name in gated:
            at = ", ".join(str(v) for v in sorted(r.velocities[name]))
            print(f"  vel-only {name} -- moves only at velocity {at}")
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
