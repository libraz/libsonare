#!/usr/bin/env python3
"""Prove a new mechanism is inert at its default, and reachable when it is not.

Adding a mechanism to a voice means adding fields to a shared patch struct, a
branch to a render loop and a row to the clamp and override tables. All of that
is compiled into every voice in the bank, not just the one it was added for, so
"the default is the identity" is a claim about 128 programs and 47 drum notes
and it is routinely made without being checked. A tolerance is a different and
much weaker claim: a mechanism that leaks a fraction of a decibel into every
other instrument has still changed them, and nothing downstream will attribute
the change here.

So the check is on the raw float bytes of the render, against a library built
from the commit before the mechanism landed, and it comes in two halves that
have to be read together:

  - Every voice that leaves the mechanism at its default renders IDENTICALLY.
  - At least one voice with the mechanism switched on renders DIFFERENTLY.

The second half is not a formality. A silent build, a library the loader did
not pick, an override key that does not resolve and a branch that is never
reached all produce a perfect first half, and a check that reports only the
first half cannot tell any of them from success. What it would be confirming is
that nothing happened -- which is exactly what it was written to rule out.

    python tools/voicematch/identity.py \\
        --base /tmp/base/lib/libsonare.dylib --head build-tuning/lib/libsonare.dylib \\
        --programs 0,19,40,56,73 --drums 35,36,38,47 \\
        --reach 36:d036.percussion.plate_gain=1.0

`--reach` is `<drum note>:<override>` or `p<program>:<override>`, and may be
repeated. Overrides need the head library to be a `-DBUILD_TUNING=ON` build,
which the run checks rather than assumes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: Rendered per case. Long enough to carry a resonator's tail, since a
#: mechanism that only differs after the amplitude envelope has closed is still
#: a difference and a short window is how it gets missed.
SECONDS = 4.0
GATE_S = 0.5
VELOCITY = 100
PERCUSSION_CHANNEL = 9

_WORKER = r'''
import hashlib, sys
import numpy as np
sys.path.insert(0, "tools"); sys.path.insert(0, "tools/voicematch")
from render_model import render_model
from smf import Note, write_smf
note, program, channel, seconds, gate = (int(sys.argv[1]), int(sys.argv[2]),
                                         int(sys.argv[3]), float(sys.argv[4]),
                                         float(sys.argv[5]))
smf = write_smf([Note(note, %(vel)d, 0.1, gate)], program=program, channel=channel,
                end_pad=1.0)
a = np.asarray(render_model(smf, seconds, 48000), dtype=np.float32)
sys.stdout.write(hashlib.sha256(a.tobytes()).hexdigest())
''' % {"vel": VELOCITY}


def render_hash(lib: str, note: int, program: int, channel: int,
                overrides: str = "") -> str:
    """sha256 of one render's raw float32 bytes, from one library.

    A hash and not a comparison of arrays: the two libraries cannot be loaded
    into one process -- the override table is read once at load -- so each side
    is a subprocess and what crosses back has to be small.
    """
    env = dict(os.environ)
    env["SONARE_LIB_PATH"] = lib
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    else:
        env.pop("SONARE_TUNING_OVERRIDES", None)
    p = subprocess.run(
        [sys.executable, "-c", _WORKER, str(note), str(program), str(channel),
         str(SECONDS), str(GATE_S)],
        capture_output=True, text=True, env=env, cwd=REPO_ROOT)
    if p.returncode:
        raise RuntimeError(f"{lib}: {p.stderr[-2000:]}")
    return p.stdout.strip()


def parse_reach(spec: str) -> tuple[int, int, int, str]:
    """`36:key=value` or `p40:key=value` -> (note, program, channel, overrides)."""
    where, _, ov = spec.partition(":")
    if not ov:
        raise ValueError(f"--reach needs <note>:<override>, got {spec!r}")
    if where.startswith("p"):
        return 60, int(where[1:]), 0, ov
    return int(where), 0, PERCUSSION_CHANNEL, ov


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base", required=True, help="dylib built before the mechanism")
    p.add_argument("--head", required=True, help="dylib built with it")
    p.add_argument("--programs", default="", help="melodic programs, comma separated")
    p.add_argument("--drums", default="", help="drum notes, comma separated")
    p.add_argument("--note", type=int, default=60, help="note the programs are played at")
    p.add_argument("--reach", action="append", default=[],
                   help="<note>:<key=value> the mechanism must be audible through")
    p.add_argument("--json", default="", help="write the case table here")
    args = p.parse_args(argv)

    cases = []
    for prog in [int(x) for x in args.programs.split(",") if x.strip()]:
        cases.append((f"program {prog}", args.note, prog, 0))
    for note in [int(x) for x in args.drums.split(",") if x.strip()]:
        cases.append((f"drum {note}", note, 0, PERCUSSION_CHANNEL))
    if not cases:
        p.error("nothing to compare -- give --programs and/or --drums")

    rows, differing = [], []
    print(f"{'case':<14}{'base':<18}{'head':<18}verdict")
    for label, note, prog, chan in cases:
        b = render_hash(args.base, note, prog, chan)
        h = render_hash(args.head, note, prog, chan)
        same = b == h
        if not same:
            differing.append(label)
        rows.append({"case": label, "base": b, "head": h, "identical": same})
        print(f"{label:<14}{b[:16]:<18}{h[:16]:<18}{'same' if same else 'CHANGED'}")

    reach_rows, reached = [], []
    if args.reach:
        print(f"\n{'reach':<14}{'off':<18}{'on':<18}verdict")
    for spec in args.reach:
        note, prog, chan, ov = parse_reach(spec)
        off = render_hash(args.head, note, prog, chan)
        on = render_hash(args.head, note, prog, chan, overrides=ov)
        moved = off != on
        if moved:
            reached.append(ov)
        reach_rows.append({"override": ov, "off": off, "on": on, "reaches": moved})
        print(f"{ov.split('=')[0].split('.')[-1]:<14}{off[:16]:<18}{on[:16]:<18}"
              f"{'reaches' if moved else 'INERT'}")

    if args.json:
        Path(args.json).write_text(json.dumps(
            {"identity": rows, "reach": reach_rows}, indent=1) + "\n")

    ok = True
    if differing:
        print(f"\nFAIL: the default is not the identity for {', '.join(differing)}")
        ok = False
    if args.reach and not reached:
        # Without this the run above has confirmed only that nothing happened,
        # which every broken setup also confirms.
        print("\nFAIL: no override reached the render, so the identity result is "
              "vacuous -- check the library was built with -DBUILD_TUNING=ON and "
              "that the override keys exist")
        ok = False
    if ok:
        print(f"\nOK: {len(rows)} voices bit-identical at the default"
              + (f", {len(reached)} of {len(reach_rows)} overrides audible"
                 if args.reach else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
