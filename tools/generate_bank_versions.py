#!/usr/bin/env python3
"""Regenerate or check the tracked version registry of the instrument bank.

The bank is a product that is edited: a voice is fitted against a reference, a
family's levels are rebalanced, a GS send weight moves. Each of those changes
what a consumer hears from the same MIDI file, and none of them is visible in
any version this repository already carries — the semver release version moves
for reasons that have nothing to do with a voice, and the ABI versions are about
struct layout and deliberately never move during development.

So each voice carries a generation of its own here, and the registry is what
makes it true rather than a claim. Every unit's values are read from the library
itself under `SONARE_TUNING_DUMP` — the same code the render uses, so a parse
cannot drift from it — and fingerprinted. A unit whose fingerprint has moved
gets its version incremented and a history line; a run with `--check` fails
instead, which is what stops a voice changing without its version saying so.

WHAT A UNIT IS. Three kinds, and every knob the library reports belongs to
exactly one:

  patch   a named GM fallback patch, or a `famN` family patch
  drum    one note of the drum table, `d000` to `d127`
  shared  a group of calibration constants: an engine's own (`piano_voice`,
          `brass_voice`), the GS effect scales (`gs_effects`), and the two
          fallback tables that weight every program (`gm_fallback_map`, whose
          `kSends*` are the CC91 weighting, and `gm_fallback_families`)

An engine's constants are a unit rather than being folded into the voices that
use them, and that is a limitation stated rather than hidden: the dump does not
record which engine a patch runs on, so a `brass_voice` change cannot be
attributed to the brass patches alone. A voice's full identity is therefore its
own version together with the bank generation, the way a package version sits
alongside the runtime it was built for.

    python3 tools/generate_bank_versions.py --library build-tuning/lib/libsonare.dylib
    python3 tools/generate_bank_versions.py --library ... --check

The library must be a `-DBUILD_TUNING=ON` build. Nothing else reports its own
knob space, and a normal build silently ignores `SONARE_TUNING_DUMP` — which the
generator refuses rather than reading as an empty bank.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import date as date_type
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "voicematch"))

DEFAULT_OUTPUT = Path("tools/bank-versions.json")

#: Constant groups that are not a patch. Everything else the dump reports is one.
SHARED_PREFIXES = frozenset({
    "gm_fallback_families",
    "gm_fallback_map",
    "gs_effects",
})

DRUM_UNIT = re.compile(r"^d(\d{3})$")
FAMILY_UNIT = re.compile(r"^fam(\d+)$")


def unit_kind(prefix: str, patches: set[str]) -> str:
    """Which of the three kinds a dumped prefix is.

    A patch is recognised by the library's own program map rather than by its
    name, except for the drum table, which has no program-map entry at all — a
    drum note is not a GM program, so the map has nothing to say about it.
    """
    if DRUM_UNIT.match(prefix):
        return "drum"
    if prefix in patches or FAMILY_UNIT.match(prefix):
        return "patch"
    if prefix in SHARED_PREFIXES:
        return "shared"
    # Every remaining prefix is a file stem carrying `SONARE_TUNABLE` engine
    # constants. Classified last rather than listed, so a voice engine added to
    # the tree is versioned without this file being edited.
    return "shared"


def fingerprint(values: dict[str, float]) -> str:
    """A unit's identity: its keys and values, order-independent.

    `repr` rather than a format string, because it round-trips a float exactly
    and a rounded one would let a change below the printed precision pass as no
    change at all.
    """
    body = "\n".join(f"{k}={values[k]!r}" for k in sorted(values))
    return hashlib.sha256(body.encode()).hexdigest()


def read_units(library: Path) -> tuple[dict[str, dict[str, float]], set[str]]:
    """Every knob the library reports, grouped into units, plus its patch names."""
    from catalogue import dump_catalogue

    try:
        catalogue = dump_catalogue(0, "sustain", str(library), sr=48000)
    except RuntimeError as exc:
        raise SystemExit(
            f"{library}: {str(exc).splitlines()[0]}\n"
            f"The registry is read from the library's own knob dump, which only a "
            f"-DBUILD_TUNING=ON build writes."
        ) from exc
    units: dict[str, dict[str, float]] = {}
    for key, value in catalogue.defaults.items():
        prefix, _, rest = key.partition(".")
        if not rest:
            continue
        units.setdefault(prefix, {})[rest] = value
    return units, set(catalogue.programs.values())


def load_registry(path: Path) -> dict:
    if not path.exists():
        return {"bank_generation": 0, "units": {}}
    data = json.loads(path.read_text())
    data.setdefault("bank_generation", 0)
    data.setdefault("units", {})
    return data


DOC = (
    "Generated by tools/generate_bank_versions.py from a -DBUILD_TUNING=ON build; "
    "do not edit by hand. Each unit is a voice, a drum note or a group of shared "
    "calibration constants, carrying the generation it is on, a fingerprint of its "
    "values and the history of its bumps. `make bank-versions-check` fails when a "
    "unit's values have moved without its version following, so a change to what "
    "the bank sounds like cannot ship unversioned. The values are here so the diff "
    "of a bump says what moved."
)


def rebuild(registry: dict, units: dict[str, dict[str, float]], patches: set[str],
            note: str, when: str) -> tuple[dict, list[str], list[str]]:
    """The registry as it should be, plus the units that changed and vanished."""
    held = registry["units"]
    changed = [
        name for name, values in sorted(units.items())
        if held.get(name, {}).get("fingerprint") != fingerprint(values)
    ]
    gone = sorted(set(held) - set(units))
    generation = registry["bank_generation"] + (1 if changed or gone else 0)

    out: dict[str, dict] = {}
    for name, values in sorted(units.items()):
        previous = held.get(name, {})
        digest = fingerprint(values)
        version = int(previous.get("version", 0))
        history = list(previous.get("history", []))
        if name in changed:
            version += 1
            history.append({
                "version": version,
                "generation": generation,
                "date": when,
                "note": note or ("initial" if version == 1 else "unrecorded"),
            })
        out[name] = {
            "kind": unit_kind(name, patches),
            "version": version,
            "fingerprint": digest,
            "history": history,
            "values": {k: values[k] for k in sorted(values)},
        }
    return (
        {"_doc": DOC, "bank_generation": generation, "units": out},
        changed,
        gone,
    )


def report(changed: list[str], gone: list[str], generation: int, limit: int = 20) -> None:
    """Name what moved. A count alone sends the reader back to the diff."""
    if gone:
        print(f"  {len(gone)} unit(s) no longer in the bank: {', '.join(gone[:limit])}"
              + (" …" if len(gone) > limit else ""))
    if changed:
        print(f"  {len(changed)} unit(s) changed, now generation {generation}:")
        for name in changed[:limit]:
            print(f"    {name}")
        if len(changed) > limit:
            print(f"    … and {len(changed) - limit} more")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--library", type=Path, required=True,
                        help="path to a -DBUILD_TUNING=ON libsonare shared library")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="tracked registry path")
    parser.add_argument("--check", action="store_true",
                        help="fail instead of rewriting a stale registry")
    parser.add_argument("--note", default="",
                        help="one line recorded against every unit bumped by this run, "
                             "saying what the change was")
    parser.add_argument("--date", default="",
                        help="the date recorded on this run's history entries "
                             "(default: today)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    units, patches = read_units(args.library)
    if not units:
        raise SystemExit(f"{args.library}: the knob dump was empty")
    registry = load_registry(args.output)
    when = args.date or date_type.today().isoformat()
    rebuilt, changed, gone = rebuild(registry, units, patches, args.note, when)

    if args.check:
        if not changed and not gone:
            print(f"{args.output}: current at generation {rebuilt['bank_generation']} "
                  f"({len(units)} units)")
            return 0
        print(f"{args.output} is stale — the bank has changed and the registry has not.")
        report(changed, gone, rebuilt["bank_generation"])
        print(f"\nRegenerate it in the same change as the edit that moved them:\n"
              f"  python3 {Path(__file__).name} --library {args.library} "
              f"--note '<what changed>'")
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(rebuilt, indent=1, sort_keys=False) + "\n")
    if changed or gone:
        print(f"{args.output}: generation {rebuilt['bank_generation']}")
        report(changed, gone, rebuilt["bank_generation"])
        if not args.note:
            print("\nnote: no --note given, so every bump is recorded as 'unrecorded'. "
                  "The version says a voice moved and only the note can say why.")
    else:
        print(f"{args.output}: unchanged at generation {rebuilt['bank_generation']} "
              f"({len(units)} units)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
