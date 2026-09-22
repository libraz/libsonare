"""Two-render byte comparison per NativeSynth engine.

Answers two separate questions, one per requested representative GM program:

  (a) reproducible  -- render the same single note twice, in two separate
      Python processes (two fresh `render_model` calls, two fresh dylib
      loads), and byte-compare the audio.
  (b) machine-gun    -- render one hit and two hits of the same note, subtract
      to isolate the second hit's own output, and compare it against the
      first's.

(b) is the question the "second hit repeats the first" suspicion is actually
about; (a) is the control that says whether the renderer is deterministic at
all, and (b)'s subtraction is only valid because (a) comes back identical.

Three things decide a verdict rather than one, because each of the other two
was once read as the answer and is not: bytes differing is not a waveform
differing (a per-voice pan scatter moves every sample without changing what is
played), and a difference is not attributable at all while the second note-on
is still perturbing the first note's ringing voice. `compare_b` reports
`inconclusive` in that case instead of a verdict its own arithmetic cannot
support.

Read-only: does not touch src/, does not write to tools/voicematch/reference or
tools/voicematch/capture, does not touch git.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from render_model import ensure_lib_path, render_model
from smf import Note, write_smf

NOTE = 60
VELOCITY = 100
HIT_DUR = 1.0
GAP = 3.0  # seconds between the two hits in the machine-gun render
SR = 48000
ONSET = 0.2

# One representative GM program per NativeSynth engine (SynthEngineMode in
# src/midi/synth/excitation_axes.h), chosen from the engine tag comments in
# gm_fallback_programs_*.h. kSample is excluded (host-supplied PCM, not
# reachable without a SoundFont).
ENGINES: list[tuple[str, int, str]] = [
    ("brass", 56, "Trumpet"),
    ("reed", 64, "Soprano Sax"),
    ("free_reed", 21, "Accordion"),
    ("flute", 73, "Flute"),
    ("bowed_string", 40, "Violin"),
    ("vocal", 52, "Choir Aahs"),
    ("pipe_organ", 19, "Church Organ"),
    ("piano", 0, "Acoustic Grand Piano"),
    ("plucked_ks", 24, "Nylon Guitar"),
    ("modal", 11, "Vibraphone"),
    ("percussion", 47, "Timpani"),
    ("additive", 16, "Drawbar Organ"),
    ("fm", 4, "Electric Piano 1"),
    ("subtractive", 80, "Lead 1 (square)"),
]


def single_note_smf(program: int) -> bytes:
    return write_smf(
        [Note(note=NOTE, velocity=VELOCITY, start=ONSET, dur=HIT_DUR)], program=program
    )


def double_hit_smf(program: int) -> bytes:
    notes = [
        Note(note=NOTE, velocity=VELOCITY, start=ONSET, dur=HIT_DUR),
        Note(note=NOTE, velocity=VELOCITY, start=ONSET + GAP, dur=HIT_DUR),
    ]
    return write_smf(notes, program=program)


_SUBPROCESS_SNIPPET = """
import sys
sys.path.insert(0, {tools_dir!r})
from determinism_check import single_note_smf
from render_model import render_model
audio = render_model(single_note_smf({program}), total_seconds=3.5, sr={sr})
audio.astype("<f4").tofile({out!r})
"""


def render_once_in_subprocess(program: int, out_path: Path) -> None:
    """Render the single-note file in a brand-new Python process."""
    code = _SUBPROCESS_SNIPPET.format(
        tools_dir=str(Path(__file__).resolve().parent), program=program, sr=SR, out=str(out_path)
    )
    subprocess.run([sys.executable, "-c", code], check=True)


def rms_db(x: np.ndarray) -> float:
    rms = float(np.sqrt(np.mean(np.square(x, dtype=np.float64))))
    return 20.0 * np.log10(rms + 1e-12)


def compare_a(program: int, tmp_dir: Path, tag: str) -> tuple[bool, float, float]:
    """(a) reproducible: same note, two separate processes."""
    out1, out2 = tmp_dir / f"{tag}.a1.f32", tmp_dir / f"{tag}.a2.f32"
    render_once_in_subprocess(program, out1)
    render_once_in_subprocess(program, out2)
    a1 = np.fromfile(out1, dtype="<f4")
    a2 = np.fromfile(out2, dtype="<f4")
    n = min(len(a1), len(a2))
    diff = a1[:n].astype(np.float64) - a2[:n].astype(np.float64)
    identical = bool(np.array_equal(a1, a2))
    return (
        identical,
        float(np.max(np.abs(diff))) if n else 0.0,
        rms_db(diff) if n else float("-inf"),
    )


INAUDIBLE_RATIO_DB = -60.0  # diff/hit1 rms ratio at or below this reads as "unchanged"


def compare_b(program: int) -> dict:
    """(b) machine-gun: is the second hit's own output the same as the first's.

    hit1's tail is still ringing when hit2 attacks (HIT_DUR=1.0, release runs
    past GAP=3.0s for several engines), so a naive window-vs-window compare
    scores every engine as DIFF regardless of what hit2 actually sounds like --
    hit1's own tail leaks into hit2's window. Isolate hit2's contribution by
    subtraction instead, using (a)'s bit-identical-render result as the
    causality argument: A (one hit) and B (two hits) run hit1 through
    identical input up to hit2's note-on, so B-A cancels hit1's tail and
    leaves exactly what hit2 added -- unless hit2's note-on itself perturbs
    hit1's still-ringing voice (stealing/retrigger), which would show up as a
    nonzero B-A *before* hit2's onset and is reported as its own finding.
    """
    total_seconds = ONSET + GAP + HIT_DUR + 2.5
    window = round((GAP - 0.1) * SR)
    start1 = round(ONSET * SR)
    start2 = round((ONSET + GAP) * SR)

    a_audio = np.atleast_2d(
        render_model(single_note_smf(program), total_seconds=total_seconds, sr=SR)
    )
    b_audio = np.atleast_2d(
        render_model(double_hit_smf(program), total_seconds=total_seconds, sr=SR)
    )
    a = np.asarray(a_audio, dtype=np.float64)
    b = np.asarray(b_audio, dtype=np.float64)
    residual = b - a

    pre_hit2_bleed = float(np.max(np.abs(residual[:start2])))  # must be 0.0 for B-A to mean hit2

    hit1 = a[start1 : start1 + window]  # hit1's own output (A has no hit2)
    hit2_isolated = residual[start2 : start2 + window]  # hit2's own output (B minus A)
    diff = hit1 - hit2_isolated

    hit1_rms_dbfs = rms_db(hit1)
    ratio_db = rms_db(diff) - hit1_rms_dbfs
    bleed_ratio_db = 20.0 * np.log10(pre_hit2_bleed + 1e-12) - hit1_rms_dbfs

    # A per-voice pan scatter puts the two hits at different positions, and a
    # summed or per-channel compare then reads a placement difference as a
    # timbre one. Divide out each channel's own best-fit scalar first; what
    # survives is waveform, which is the only thing a repeated strike is judged
    # on. `scalars` differing between channels IS the pan difference.
    scalars, residual_ratio_db = [], -np.inf
    for ch in range(hit1.shape[1] if hit1.ndim > 1 else 1):
        h1 = hit1[:, ch] if hit1.ndim > 1 else hit1
        h2 = hit2_isolated[:, ch] if hit2_isolated.ndim > 1 else hit2_isolated
        energy = float(np.dot(h1, h1))
        scale = float(np.dot(h1, h2) / energy) if energy > 0.0 else 0.0
        scalars.append(scale)
        residual_ratio_db = max(residual_ratio_db, rms_db(h2 - scale * h1) - rms_db(h1))

    bit_identical = bool(np.array_equal(hit1, hit2_isolated))
    if bit_identical:
        # Valid whatever the bleed: nothing perturbed can come back identical.
        verdict = "machine-gun"
    elif bleed_ratio_db > INAUDIBLE_RATIO_DB:
        # hit2's note-on moved hit1's still-ringing voice, so B-A is not hit2
        # alone and the contamination only ever widens the difference.
        verdict = "inconclusive"
    elif residual_ratio_db <= INAUDIBLE_RATIO_DB:
        verdict = "machine-gun"
    else:
        verdict = "varies"
    return {
        "pre_hit2_bleed": pre_hit2_bleed,
        "bleed_ratio_db": bleed_ratio_db,
        "bit_identical": bit_identical,
        "hit1_rms_dbfs": hit1_rms_dbfs,
        "ratio_db": ratio_db,
        "scalars": scalars,
        "residual_ratio_db": residual_ratio_db,
        "verdict": verdict,
    }


def main() -> int:
    ensure_lib_path()
    import tempfile

    with tempfile.TemporaryDirectory(prefix="determinism-check-") as tmp:
        tmp_dir = Path(tmp)
        for name, program, gm_name in ENGINES:
            a_ident, _, _ = compare_a(program, tmp_dir, name)
            b = compare_b(program)
            a_flag = "same" if a_ident else "DIFF"
            gains = "/".join(f"{s:.3f}" for s in b["scalars"])
            print(
                f"{name:14s} GM{program:3d} {gm_name:22s} "
                f"(a)repro={a_flag:4s} | raw={b['ratio_db']:7.1f} "
                f"depanned={b['residual_ratio_db']:7.1f} bleed={b['bleed_ratio_db']:6.1f} "
                f"gain={gains:15s} -> {b['verdict']}"
            )
    print(
        "\nall figures are dB relative to the first hit's own RMS. `raw` compares the two hits,\n"
        "`depanned` compares them after dividing out each channel's best-fit scalar, and that is\n"
        "the column the verdict reads -- a per-voice pan scatter moves `raw` without changing a\n"
        "waveform. `bleed` is how much hit2's note-on moved hit1's still-ringing voice; above the\n"
        f"{INAUDIBLE_RATIO_DB:.0f} dB line the subtraction no longer isolates hit2 and the verdict is withheld\n"
        "rather than reported, since that contamination only ever widens the difference."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
