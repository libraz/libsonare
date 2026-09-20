"""Does a voice still sound at the end of a held note, and for as long as its reference does.

The question no committed gate asks. `decay_db_s` is measured on every captured
note and sits in every reference, but it is not a canonical dimension for the
SUSTAINED class -- excluded on the grounds that a sustained voice does not
decay, which is a property of the REFERENCE and is exactly what has to be
checked of the model. With it out of the set the dimension is neither gated nor
excusable, so nothing reports it, and a voice whose column stops oscillating
renders a note that falls away under a compare table of ordinary-looking
numbers: every other dimension is self-relative (partials against h1, register
against its own median, centroid as a percentage of the reference's), so a
signal on its way to the noise floor keeps producing in-range ratios until it
arrives.

What this measures is the EXCESS: how much further the model falls over a held
note than its own reference falls over the same hold. Not the fall itself -- a
pad or an effect is meant to evolve, and six of them do, between 6 and 13 dB,
which against their own references is under 7. The reference supplies the
target, and the comparison is two-sided, because a model that holds where its
reference lets go is wrong the same way round.

It judges one population and names the rest rather than passing them: voices
whose reference is still sounding at the end of the hold. The target is a rate
carried out to the hold, and on a voice that has already stopped that is an
extrapolation from a fifth of a second across two seconds -- see
REFERENCE_SUSTAIN_DB, which is where a first version of this filled its list
with arithmetic rather than with voices.

Read-only: renders the model, reads `capture/` and `reference/`, writes
nothing. Exits 0 whatever it finds, on the same terms as `status.py` -- a
target that failed on "there is work to do" could not be the thing a
calibration round reads to decide where to start.

    make voicematch-sustain-check
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gm_names import gm_name
from toneclass import tone_class

HERE = Path(__file__).resolve().parent
CAPTURE_DIR = HERE / "capture"
REFERENCE_DIR = HERE / "reference"

SR = 48000
ONSET_S = 0.1
HOLD_S = 2.0
#: Where in the hold the two readings are taken. The first is late enough that
#: a slow speaker has arrived -- the church registrations take 145 ms -- and the
#: second is just before the key lifts.
EARLY_S = ONSET_S + 0.3
LATE_S = ONSET_S + HOLD_S - 0.1
WINDOW_S = 0.05

#: How far the model's fall may differ from its reference's before it is
#: reported, in dB over the whole hold. Set from the reed engine, which then
#: supplied both populations an order of magnitude apart: the voices that held
#: sat within 1.6 dB of their reference and the ones that rang down started at
#: 15.5, so the line was not drawn through either. All ten reeds now hold, so
#: the separation the value was read from is no longer in the bank to re-read —
#: `test_sustain_check.py` places specimens either side of it instead. Ten also
#: keeps out the pads and effects, whose own evolution the reference describes
#: only approximately and which reach 6.6.
EXCESS_TOLERANCE_DB = 10.0

#: How far a reference may itself fall over the hold and still be a target.
#:
#: The population this can judge is voices whose reference is STILL SOUNDING at
#: the end of the hold, and it is not a scoping convenience: the target here is
#: a rate read over the reference's own fit window and carried out to the hold,
#: which is an extrapolation, and on a voice that has already stopped it
#: extrapolates a rate measured over a fifth of a second across two seconds.
#: Guitar fret noise predicts -607 dB that way, so a model merely falling to its
#: own noise floor reads as 555 dB of excess and the list fills with arithmetic.
#: Outside this bound the question is the decay RATE, which `decay_db_s` already
#: answers directly and without extrapolating anything.
REFERENCE_SUSTAIN_DB = 6.0

_RENDER = """
import sys, numpy as np
sys.path.insert(0, {tools!r})
from render_model import render_model
from smf import Note, write_smf
smf = write_smf([Note(note={note}, velocity={velocity}, start={onset}, dur={hold})],
                program={program})
a = render_model(smf, total_seconds={total}, sr={sr}, rig=False)
np.asarray(a, dtype="<f4").tofile({out!r})
"""


def shipped_ids() -> list[str]:
    return sorted(p.stem for p in CAPTURE_DIR.glob("*.json") if not p.name.endswith(".local.json"))


def rms_db(x: np.ndarray) -> float:
    # `float()` on the way out, not only on the way in: `np.log10` of a Python
    # float still returns `np.float64`, which propagates through every
    # arithmetic below and turns the verdict into `np.bool` -- true enough to
    # pass every assertion and not serialisable, so `--json` is where it shows.
    return float(20.0 * np.log10(float(np.sqrt(np.mean(np.square(x)))) + 1e-15))


def render_held(program: int, note: int, velocity: int, out: Path) -> np.ndarray | None:
    """One held note through the GM bank, in a fresh process."""
    code = _RENDER.format(tools=str(HERE), note=note, velocity=velocity, onset=ONSET_S,
                          hold=HOLD_S, program=program, total=ONSET_S + HOLD_S + 1.0,
                          sr=SR, out=str(out))
    done = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                          check=False)
    if done.returncode != 0 or not out.exists():
        return None
    return np.fromfile(out, dtype="<f4").reshape(-1, 2).astype(np.float64).mean(axis=1)


def reference_fall(rows: list[dict], note: int) -> float | None:
    """How far the reference falls over the same hold, in dB.

    From the rows of that one note where there are any, since a decay rate is a
    property of the register rather than of the instrument; the grid median is
    the fallback, and a reference carrying no rate at all is unjudgeable rather
    than zero.
    """
    here = [r["decay_db_s"] for r in rows
            if r.get("note") == note and r.get("decay_db_s") is not None]
    everywhere = [r["decay_db_s"] for r in rows if r.get("decay_db_s") is not None]
    pool = here or everywhere
    return float(np.median(pool)) * HOLD_S if pool else None


def check_one(cap_id: str, scratch: Path) -> dict:
    """Render one held note of this voice and compare its fall with its reference's."""
    ref_path = REFERENCE_DIR / f"{cap_id}.json"
    if not ref_path.exists():
        return {"id": cap_id, "status": "no-reference"}
    profile = json.loads(ref_path.read_text())
    rows = profile.get("rows") or []
    notes = sorted({r["note"] for r in rows if "note" in r})
    if not notes:
        return {"id": cap_id, "status": "no-rows"}
    cfg = json.loads((CAPTURE_DIR / f"{cap_id}.json").read_text())
    program = int(profile.get("capture", {}).get("program", cfg.get("program", 0)))
    note = notes[len(notes) // 2]
    velocities = sorted({r["velocity"] for r in rows if r.get("note") == note and "velocity" in r})
    velocity = velocities[-1] if velocities else 100

    audio = render_held(program, note, velocity, scratch / f"{cap_id}.f32")
    if audio is None:
        return {"id": cap_id, "status": "render-failed"}

    def at(t: float) -> float:
        i = int(t * SR)
        return rms_db(audio[i:i + int(WINDOW_S * SR)])

    early, late = at(EARLY_S), at(LATE_S)
    model_fall = late - early
    ref_fall = reference_fall(rows, note)
    if ref_fall is None:
        return {"id": cap_id, "status": "reference-has-no-decay-rate",
                "program": program, "note": note, "model_fall_db": model_fall}
    if abs(ref_fall) > REFERENCE_SUSTAIN_DB:
        # Out of population, and said so rather than passed: see
        # REFERENCE_SUSTAIN_DB for why an extrapolated target stops meaning
        # anything once the reference has already stopped.
        return {"id": cap_id, "status": "reference-does-not-sustain",
                "program": program, "note": note, "model_fall_db": model_fall,
                "reference_fall_db": ref_fall}
    excess = model_fall - ref_fall
    return {
        "id": cap_id, "status": "compared", "program": program, "note": note,
        "velocity": velocity, "early_dbfs": early, "late_dbfs": late,
        "model_fall_db": model_fall, "reference_fall_db": ref_fall, "excess_db": excess,
        "tone_class": str(tone_class(program)).replace("ToneClass.", ""),
        "beyond_tolerance": bool(abs(excess) > EXCESS_TOLERANCE_DB),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ids", default="", help="comma-separated capture ids (default: all shipped)")
    ap.add_argument("--json", action="store_true", help="print the full per-id results as JSON")
    args = ap.parse_args(argv)

    ids = [i.strip() for i in args.ids.split(",") if i.strip()] or shipped_ids()
    import tempfile

    results: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="sustain-check-") as tmp:
        scratch = Path(tmp)
        for i, cap_id in enumerate(ids, 1):
            r = check_one(cap_id, scratch)
            results.append(r)
            note = (f" excess {r['excess_db']:+.1f} dB" if r["status"] == "compared" else "")
            print(f"[{i}/{len(ids)}] {cap_id}: {r['status']}{note}", file=sys.stderr)

    if args.json:
        print(json.dumps(results, indent=1))
        return 0

    compared = [r for r in results if r["status"] == "compared"]
    flagged = sorted((r for r in compared if r["beyond_tolerance"]), key=lambda r: r["excess_db"])
    # Reach is an output: a run that compared nothing looks exactly like a clean
    # one, and this is the number that separates them.
    print(f"\ncomparisons: {len(compared)} of {len(ids)} ids — voices whose own reference is "
          f"still sounding at the end of the hold, which is the only population an "
          f"extrapolated target can judge")
    for status in sorted({r["status"] for r in results} - {"compared"}):
        n = [r["id"] for r in results if r["status"] == status]
        print(f"  {status} ({len(n)}): {', '.join(n[:8])}{' ...' if len(n) > 8 else ''}")
    print(f"beyond {EXCESS_TOLERANCE_DB:.0f} dB of their own reference's fall: {len(flagged)}")
    if flagged:
        print(f"\n  {'voice':24s} {'prog':>4s} {'class':16s} {'model':>8s} {'ref':>8s} {'excess':>9s}")
        for r in flagged:
            print(f"  {gm_name(r['program']):24s} {r['program']:>4d} {r['tone_class']:16s} "
                  f"{r['model_fall_db']:8.1f} {r['reference_fall_db']:8.1f} {r['excess_db']:+9.1f}")
    print(
        "\n`model` and `ref` are dB over one held note; `excess` is the model minus the reference,\n"
        "so it is signed: negative is a voice that stops sounding while its reference holds, and\n"
        "positive is one that holds while its reference decays. Both are wrong and neither shows\n"
        "up in a compare table, whose every other column is a ratio the signal keeps producing on\n"
        "its way to the noise floor."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
