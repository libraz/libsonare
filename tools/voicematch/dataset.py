"""Generate (knob vector -> measurement) pairs by rendering the model itself.

An amortized inverse — a model that reads a measurement and predicts the knobs
that produced it — needs a corpus of such pairs, and unlike everything else in
this directory that corpus does not come from an instrument. It comes from
libsonare, rendered at knob settings nobody chose for musical reasons. So this
is a generator rather than a measurement, and it is the only file here that
deliberately produces audio no one would want to hear.

**Every fit already builds this data and throws it away.** `autofit.Evaluator`
keys a cache by the knob vector and stores the measured terms against it, which
is exactly the shape of a training pair, and the dict dies with the process.
Recovering those would not have been enough even so, for two reasons this file
exists to fix:

  - **The distribution is wrong.** CMA-ES concentrates its samples wherever it
    is converging, which is the one region an inverse least needs to be told
    about. Sampling here is uniform over each knob's own range.
  - **The measurement is too small.** A fit collapses a render to about sixteen
    scalars. Inverting a hundred-odd knobs from sixteen numbers is badly
    underdetermined, so what is written here is the full `probe_rows` output —
    98 numbers per note, the harmonic ladder and onset skeleton and attack
    bands included.

Sampling is uniform in each knob's OWN scale: a knob declared `log` is drawn
uniformly in log space. That is the same convention `--spec auto` derives its
ranges with, so a unit of sample density means the same thing as a unit of
search step, and a magnitude knob spanning three decades is not sampled almost
entirely in its top decade.

The compiled-in default is always sample 0. It is the one point whose
measurement can be checked against an ordinary build, so a corpus that does not
contain it cannot be validated at all — but the check is a tolerance and not an
equality, and the reason is worth knowing before reading a diff. Every knob
reaches the library as TEXT at six significant figures (`knobs.format_value`,
which also has to emit valid C++ literals), so a full-precision compiled
constant cannot be pushed back in exactly. Sample 0 therefore renders the
default voice closely rather than bit-identically. Measured on program 0, the
WORST field is the onset skeleton's decay slope at 1.9e-2 dB/s (5.5e-4
relative); the attack bands move 0.01 dB, which is one unit in their last
reported place, and f0 agrees to 6e-9 relative. That is far below anything the
metric set resolves — the smallest cap in the loss is 12 dB — and it
is not a `SONARE_TUNING` behaviour difference — the two configurations still
render identically for identical values; these are not identical values.

The same six figures are why the recorded parameter vector is round-tripped
through `format_value` before it is written. A pair whose input is the drawn
value while its audio came from the rounded one is mislabelled, quietly, in
every sample.

A render that fails or falls silent is written with `ok: false` and its reason,
never dropped. Dropping it teaches a model that the region does not exist, when
what is true is that the region sounds like nothing — and a corpus that quietly
omits its failures is the same defect as a metric that skips its unusable points
and averages the rest.
"""

from __future__ import annotations

import argparse
import gzip
import json
import math
import os
import random
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT  # noqa: E402
from autofit import render_model_rows_subprocess  # noqa: E402
from build_lib import dylib_path  # noqa: E402
from catalogue import dump_catalogue  # noqa: E402
from knobs import (  # noqa: E402
    Knob,
    auto_spec,
    build_knobs,
    format_value,
    tunable_overrides,
)

SR = 48000

#: Same untracked root the capture corpus uses, for the same reason: this is
#: bulk generated data, it is large, and it does not belong in the tree.
DATASET_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser() / "dataset"

#: Bumped when the row schema changes, so a reader can refuse a corpus written
#: against a different measurement rather than silently training on two.
SCHEMA = 1


def _open(path: Path, mode: str):
    """Open plain or gzipped by suffix — a corpus this size is worth compressing."""
    if path.suffix == ".gz":
        return gzip.open(path, mode + "t", encoding="utf-8")
    return open(path, mode, encoding="utf-8")


def sample_values(knobs: list[Knob], rng: random.Random) -> list[float]:
    """One uniformly drawn point, in each knob's own scale."""
    out = []
    for k in knobs:
        if k.log and k.lo > 0.0 and k.hi > 0.0:
            out.append(math.exp(rng.uniform(math.log(k.lo), math.log(k.hi))))
        else:
            out.append(rng.uniform(k.lo, k.hi))
    return out


def _silent(rows: list[dict]) -> bool:
    """Whether the render produced nothing worth measuring.

    A silent candidate is the failure mode this whole directory keeps tripping
    over: it scores a perfect harmonic ladder because every partial is at the
    floor and the floor guard skips them all. Here it is not scored at all, only
    labelled, so a reader can decide whether silence is a class worth learning.
    """
    for row in rows:
        ladder = row.get("harmonics_db") or []
        if any(v is not None and v > -120.0 for v in ladder[1:]):
            return False
    return True


def manifest(program: int, pattern: str, notes: str, velocities: str,
             knobs: list[Knob], seed: int) -> dict:
    """What a reader needs to interpret every row that follows.

    The knob order is fixed here and never repeated per row — a hundred labels
    on every line of a million-line file is most of the file.
    """
    return {
        "schema": SCHEMA,
        "program": program,
        "pattern": pattern,
        "notes": notes,
        "velocities": velocities,
        "seed": seed,
        "knobs": [
            {"label": k.label, "tunable": k.tunable, "lo": k.lo, "hi": k.hi,
             "log": k.log, "default": k.start_value}
            for k in knobs
        ],
    }


def existing_rows(path: Path) -> int:
    """How many samples a corpus already holds, so a run resumes rather than restarts.

    Counts data rows only; the manifest is line 0. A truncated final line — a
    run killed mid-write — is not counted, and is overwritten by the append.
    """
    if not path.exists():
        return 0
    n = 0
    with _open(path, "r") as fh:
        for i, line in enumerate(fh):
            if i == 0:
                continue
            line = line.strip()
            if not line:
                continue
            try:
                json.loads(line)
            except json.JSONDecodeError:
                break
            n += 1
    return n


def generate(args) -> int:
    build_dir = Path(args.build_dir)
    dylib = dylib_path(build_dir)
    if dylib is None:
        raise SystemExit(
            f"no libsonare dylib under {build_dir} — configure it with BUILD_TUNING=ON "
            f"first (autofit.py does this; see build_lib.configure_build)"
        )
    # The library reports its own knob space, defaults and clamp bounds, so the
    # generator's ranges are the same ones a fit would search rather than a
    # second list that can drift from them.
    catalogue = dump_catalogue(args.program, args.pattern, str(dylib),
                               sr=SR, notes=args.notes, bank=args.bank)
    spec = auto_spec(args.program, catalogue, bank=args.bank)
    knobs = build_knobs(spec, {}, catalogue)
    runtime = [k for k in knobs if k.tunable is not None]
    if len(runtime) != len(knobs):
        raise SystemExit(
            f"{len(knobs) - len(runtime)} of {len(knobs)} knobs need a source rebuild; "
            f"this generator renders from one build and cannot move those"
        )

    out = Path(args.out) if args.out else DATASET_ROOT / f"p{args.program:03d}.jsonl.gz"
    out.parent.mkdir(parents=True, exist_ok=True)
    done = existing_rows(out)
    if done == 0:
        with _open(out, "w") as fh:
            fh.write(json.dumps(manifest(args.program, args.pattern, args.notes,
                                         args.velocities, knobs, args.seed)) + "\n")
    want = args.samples - done
    if want <= 0:
        print(f"{out} already holds {done} samples; nothing to do")
        return 0
    print(f"{out}: {done} samples present, generating {want} more "
          f"({len(knobs)} knobs, {args.workers} workers)", file=sys.stderr)

    # Seeded from the run's seed AND the resume point, so a resumed run does not
    # redraw the points it already has.
    rng = random.Random((args.seed, done).__hash__())
    plan: list[tuple[int, list[float]]] = []
    for i in range(done, done + want):
        # Sample 0 is the compiled-in default; see the module docstring.
        plan.append((i, [k.start_value for k in knobs] if i == 0
                     else sample_values(knobs, rng)))

    def render(item: tuple[int, list[float]]) -> dict:
        index, values = item
        # Record the value that was RENDERED, not the one that was drawn. The
        # override channel is text at six significant figures, so a sampled
        # 0.000965242193 reaches the library as 0.000965242 — and a pair whose
        # input is the drawn value while its audio came from the rounded one is
        # mislabelled by construction. Round-tripping first makes them the same
        # number, which costs nothing and is the whole integrity of the corpus.
        values = [float(format_value(v)) for v in values]
        row: dict = {"i": index, "v": values}
        try:
            rows, _ = render_model_rows_subprocess(
                build_dir, args.program, args.pattern, args.notes,
                velocities_csv=args.velocities, bank=args.bank,
                overrides=tunable_overrides(knobs, values),
            )
        except Exception as exc:                      # noqa: BLE001 - recorded, not raised
            row["ok"] = False
            row["why"] = f"{type(exc).__name__}: {exc}"[:400]
            return row
        if not rows:
            row["ok"] = False
            row["why"] = "no analysable notes"
            return row
        if _silent(rows):
            row["ok"] = False
            row["why"] = "silent"
            row["rows"] = rows
            return row
        row["ok"] = True
        row["rows"] = rows
        return row

    written = failures = 0
    with _open(out, "a") as fh, ThreadPoolExecutor(max_workers=args.workers) as pool:
        for row in pool.map(render, plan):
            fh.write(json.dumps(row) + "\n")
            written += 1
            failures += not row["ok"]
            if written % 25 == 0:
                fh.flush()
                print(f"  {done + written}/{args.samples}  ({failures} unusable)",
                      file=sys.stderr)
    print(f"{out}: {done + written} samples, {failures} of this run's {written} unusable",
          file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="dataset.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--program", type=int, required=True)
    p.add_argument("--bank", type=int, default=0)
    p.add_argument("--pattern", default="sustain")
    p.add_argument("--notes", default="", help="probe notes, CSV; the pattern's own if empty")
    p.add_argument("--velocities", default="")
    p.add_argument("--samples", type=int, default=1000,
                   help="total the corpus should hold, counting what it already has")
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--workers", type=int, default=8)
    p.add_argument("--build-dir", default="build-tuning",
                   help="a BUILD_TUNING=ON build; the knobs are pushed in through "
                        "SONARE_TUNING_OVERRIDES, so it is built once and never rebuilt")
    p.add_argument("--out", default="",
                   help=f"output path; default {DATASET_ROOT}/p<NNN>.jsonl.gz. "
                        f"A .gz suffix is compressed — at 7 kB a sample uncompressed, "
                        f"a corpus large enough to invert from is worth it")
    return generate(p.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
