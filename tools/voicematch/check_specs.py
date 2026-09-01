"""Every fit spec still names knobs that exist.

A spec outlives the mechanism it was written for. When the harpsichord's
excitation became one period of an ideally plucked string's bridge force, the
contact-width pair it replaced went out of the engine and stayed in
`specs/harpsichord.json`, where the only thing that ever noticed was a fit
somebody tried to run — six days later, as a traceback after the corpus had
already been resolved.

`build_knobs` refuses a name the library never reported, so nothing silently
sweeps a dead key; what was missing is anything that asks before a run needs
the answer. This asks. It resolves every spec's knobs against the same
catalogue `autofit` validates against, and names each one that resolves to
nothing.

It cannot say a knob is *useless* — that is `--diagnose`'s inert list, which
needs renders and a reference. This is the cheaper half: the knob is not there
at all.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import catalogue as catalogue_mod  # noqa: E402
from catalogue import resolve_knob_name, scan_tunables  # noqa: E402

HERE = Path(__file__).resolve().parent
SPEC_DIR = HERE / "specs"


def spec_knobs(path: Path) -> list[str]:
    """The `tunable` name of every knob in a spec, in file order.

    A spec is a bare list or an object with a `knobs` array; both forms are
    shipped. A knob addressed by `file`/`pattern` edits source and names no
    tunable, so it has nothing to resolve.
    """
    raw = json.loads(path.read_text())
    entries = raw if isinstance(raw, list) else raw.get("knobs", [])
    return [e["tunable"] for e in entries if isinstance(e, dict) and "tunable" in e]


def missing(paths: list[Path], catalogue) -> dict[str, list[str]]:
    """Per spec, the knobs that resolve to neither a tunable nor a reported key."""
    tunables = scan_tunables()
    out: dict[str, list[str]] = {}
    for path in paths:
        dead = []
        for name in spec_knobs(path):
            resolved = resolve_knob_name(name, tunables)
            if resolved not in tunables and resolved not in catalogue.defaults:
                dead.append(name)
        if dead:
            out[path.name] = dead
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", default=None, help="a -DBUILD_TUNING=ON library")
    ap.add_argument("--sr", type=int, default=48000)
    args = ap.parse_args()

    paths = sorted(SPEC_DIR.glob("*.json"))
    catalogue = catalogue_mod.dump_catalogue(0, "sustain", args.lib, sr=args.sr)
    dead = missing(paths, catalogue)
    if not dead:
        print(f"every knob in {len(paths)} spec(s) resolves against the library's catalogue")
        return 0
    for name, knobs in sorted(dead.items()):
        print(f"specs/{name}: {len(knobs)} knob(s) name nothing the library reports")
        for knob in knobs:
            print(f"  {knob}")
    print("\nA spec keeps the name of a mechanism the engine no longer has. Delete the knob, "
          "or point it at what replaced it.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
