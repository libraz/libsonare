"""Renders the model over the same note/velocity grid `capture.py` captures.

The oracle corpus is one WAV per (note, velocity); the model has to be measured
the same way before a difference between them means anything. This writes the
model's grid into the same directory layout and the same file names, so
`analyze_harpsichord_corpus.py --timbre model` reads it with no special case.

The renders go through the SF2-less fallback path, so what is measured is the
NativeSynth voice under improvement and not a SoundFont.

    rye run --pyproject bindings/python/pyproject.toml \
        python tools/voicematch/render_model_grid.py --program 6
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from render_model import render_model  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import write_wav  # noqa: E402

# Matched to capture/harpsichord.json so the two grids are measurable against
# each other frame for frame.
PREROLL_S = 0.1
GATE_S = 4.0
TAIL_S = 2.0
SR = 48000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--program", type=int, default=6)
    ap.add_argument("--bank", type=int, default=0)
    ap.add_argument("--notes", default="29,36,41,48,53,60,65,72,77,84,89")
    ap.add_argument("--velocities", default="24,56,88,120")
    ap.add_argument("--timbre", default="model")
    ap.add_argument(
        "--out",
        default=".cache/voicematch/capture/harpsichord",
        help="corpus root; the timbre directory is created under it",
    )
    args = ap.parse_args()

    notes = [int(n) for n in args.notes.split(",")]
    velocities = [int(v) for v in args.velocities.split(",")]
    out = pathlib.Path(args.out) / args.timbre
    out.mkdir(parents=True, exist_ok=True)

    total = PREROLL_S + GATE_S + TAIL_S
    done = 0
    for note in notes:
        for vel in velocities:
            smf = write_smf(
                [Note(note=note, velocity=vel, start=PREROLL_S, dur=GATE_S)],
                program=args.program,
                end_pad=TAIL_S,
            )
            audio = render_model(smf, total, sr=SR)
            path = out / f"n{note:03d}_v{vel:03d}.wav"
            write_wav(path, np.asarray(audio), SR)
            done += 1
            print(f"[{done}/{len(notes) * len(velocities)}] {path.name} "
                  f"peak {float(np.abs(audio).max()):.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
