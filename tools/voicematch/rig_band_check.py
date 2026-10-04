"""Do the driven default rigs reach their amped references' distortion band and crest?

Renders programs 29 and 30 through the model floor with the bank's default rig
in place (the Sf2 path with no SoundFont, `render_model(rig=True)`) and reads two
numbers per probe note, on the same terms from both sides:

- the share of energy above 2 kHz, over 0.3-1.3 s after the onset, and
- the crest, the whole note's peak over the RMS of that same window.

Both references are `rig: baked` library captures, so the comparison is chain
against chain and is an acceptance check, never a fit target. The share is read
against a reference whose cabinet already rolls off near 4 kHz, so the script
prints each side's band edge as well: a share met by energy past the
reference's edge is not a match.

A second render per program is the control: the same amplifier at the same
trims with the pedal slot emptied. The pedal earns its place only when the
adopted chain sits closer to the reference crest than the control does. The
control is a tuning override, so it needs a `BUILD_TUNING` library; through a
shipped one the script says the control could not be taken rather than
printing the adopted numbers twice.

Local measurement, read-only, not gated. Exits 1 when a program misses either
bound or the control is not beaten, 0 otherwise.

    SONARE_LIB_PATH=build-tuning/lib/libsonare.dylib \\
        rye run --pyproject bindings/python/pyproject.toml \\
        python tools/voicematch/rig_band_check.py
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from render_model import REPO_ROOT, ensure_lib_path, render_model
from smf import Note, write_smf
from wavio import read_wav

CAPTURE_DIR = HERE / "capture"
CORPUS_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser()

#: Program -> (capture id, tuning key of the binding's pedal slot).
PROGRAMS = {
    29: ("overdriven_guitar", "gm_fallback_map.kRigCrunchPre"),
    30: ("distortion_guitar", "gm_fallback_map.kRigLeadPre"),
}
VELOCITY = 104
SR = 48000
HOLD_S = 2.0
TAIL_S = 0.3
WINDOW_S = (0.3, 1.3)
SPLIT_HZ = 2000.0
#: Acceptance bounds on the medians, in dB.
SHARE_TOLERANCE_DB = 6.0
CREST_TOLERANCE_DB = 4.0
#: An onset is the first sample within this fraction of the note's peak.
ONSET_FRACTION = 0.01
#: The band edge is where the smoothed spectrum last stands within this of its peak.
EDGE_DB = 60.0


def note_metrics(x: np.ndarray, sr: int) -> dict[str, float]:
    """Share above SPLIT_HZ, crest and peak of one note, onset-aligned."""
    mono = x.mean(axis=1) if x.ndim == 2 else x
    mono = mono.astype(np.float64)
    peak = float(np.max(np.abs(mono)))
    onset = int(np.argmax(np.abs(mono) > ONSET_FRACTION * peak))
    seg = mono[onset + int(WINDOW_S[0] * sr) : onset + int(WINDOW_S[1] * sr)]
    rms = float(np.sqrt(np.mean(seg**2)))
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    share = float(spec[freqs > SPLIT_HZ].sum() / spec.sum())
    # Third-octave-ish smoothing so a single bin between partials cannot set the edge.
    smooth = np.convolve(spec, np.ones(64) / 64, mode="same")
    level = 10 * np.log10(smooth + 1e-30)
    above = np.nonzero(level > level.max() - EDGE_DB)[0]
    return {
        "share_db": 10 * np.log10(share + 1e-20),
        "crest_db": 20 * np.log10(peak / (rms + 1e-20) + 1e-20),
        "peak_db": 20 * np.log10(peak + 1e-20),
        "edge_hz": float(freqs[above[-1]]) if above.size else 0.0,
    }


def reference_metrics(capture_id: str) -> tuple[list[int], list[dict[str, float]]]:
    """The probe notes a capture declares and its measured WAVs at VELOCITY."""
    capture = json.loads((CAPTURE_DIR / f"{capture_id}.json").read_text())
    timbre = capture["timbres"][0]["id"]
    rows = []
    for note in capture["notes"]:
        path = CORPUS_ROOT / "capture" / capture_id / timbre / f"n{note:03d}_v{VELOCITY:03d}.wav"
        audio, sr = read_wav(path)
        rows.append(note_metrics(audio, sr))
    return capture["notes"], rows


def model_metrics(program: int, notes: list[int]) -> list[dict[str, float]]:
    """Render each note through the bank's rig and measure it."""
    rows = []
    for note in notes:
        smf = write_smf([Note(note, VELOCITY, 0.0, HOLD_S)], program=program)
        rows.append(note_metrics(render_model(smf, HOLD_S + TAIL_S, SR, rig=True), SR))
    return rows


def run_worker(program: int, notes: list[int], overrides: list[str]) -> dict:
    """Render in a child process: the override table is read once per process."""
    with tempfile.TemporaryDirectory() as tmp:
        dump = Path(tmp) / "knobs.txt"
        env = dict(os.environ)
        env["SONARE_TUNING_OVERRIDES"] = ",".join(overrides)
        env["SONARE_TUNING_DUMP"] = str(dump)
        args = [sys.executable, __file__, "--worker", str(program), ",".join(map(str, notes))]
        out = subprocess.run(args, env=env, check=True, capture_output=True, text=True)
        rows = json.loads(out.stdout)
        # Only a tuning build writes the dump, so its absence says overrides were ignored.
        return {"rows": rows, "tuning": dump.exists()}


def median(rows: list[dict[str, float]], key: str) -> float:
    return float(np.median([r[key] for r in rows]))


def check_program(program: int, overrides: list[str]) -> bool:
    capture_id, pedal_key = PROGRAMS[program]
    notes, ref = reference_metrics(capture_id)
    adopted = run_worker(program, notes, overrides)
    control = run_worker(program, notes, [*overrides, f"{pedal_key}=0"])

    print(f"\nprogram {program} ({capture_id}), v{VELOCITY}")
    print("  note |  ref >2k  crest  peak  edge | model >2k  crest  peak  edge | ctrl >2k  crest")
    for i, note in enumerate(notes):
        r, m, c = ref[i], adopted["rows"][i], control["rows"][i]
        print(
            f"  {note:4d} | {r['share_db']:8.1f} {r['crest_db']:6.1f} {r['peak_db']:5.1f}"
            f" {r['edge_hz']:5.0f} | {m['share_db']:9.1f} {m['crest_db']:6.1f}"
            f" {m['peak_db']:5.1f} {m['edge_hz']:5.0f} | {c['share_db']:8.1f} {c['crest_db']:6.1f}"
        )
    share_delta = median(adopted["rows"], "share_db") - median(ref, "share_db")
    crest_delta = median(adopted["rows"], "crest_db") - median(ref, "crest_db")
    ctrl_crest_delta = median(control["rows"], "crest_db") - median(ref, "crest_db")
    peak_delta = median(adopted["rows"], "peak_db") - median(ref, "peak_db")
    print(
        f"  median  ref >2k {median(ref, 'share_db'):.1f} crest {median(ref, 'crest_db'):.1f}"
        f" | model >2k {median(adopted['rows'], 'share_db'):.1f}"
        f" crest {median(adopted['rows'], 'crest_db'):.1f}"
        f" | control >2k {median(control['rows'], 'share_db'):.1f}"
        f" crest {median(control['rows'], 'crest_db'):.1f}"
    )
    share_ok = abs(share_delta) <= SHARE_TOLERANCE_DB
    crest_ok = abs(crest_delta) <= CREST_TOLERANCE_DB
    print(
        f"  >2k share  model-ref {share_delta:+.1f} dB (bound ±{SHARE_TOLERANCE_DB:g}): "
        f"{'pass' if share_ok else 'FAIL'}"
    )
    print(
        f"  crest      model-ref {crest_delta:+.1f} dB (bound ±{CREST_TOLERANCE_DB:g}): "
        f"{'pass' if crest_ok else 'FAIL'}"
    )
    print(f"  peak       model-ref {peak_delta:+.1f} dB (information: the level trims)")
    if not control["tuning"]:
        print("  control    not taken: the library is not a BUILD_TUNING build")
        return False
    pedal_ok = abs(crest_delta) < abs(ctrl_crest_delta)
    print(
        f"  control    no-pedal crest-ref {ctrl_crest_delta:+.1f} dB; the pedal "
        f"{'is' if pedal_ok else 'is NOT'} closer to the reference crest"
    )
    return share_ok and crest_ok and pedal_ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument(
        "--program",
        type=int,
        choices=sorted(PROGRAMS),
        action="append",
        help="program to check (default: both)",
    )
    parser.add_argument(
        "--override",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="tuning override applied to both renders (tuning build only)",
    )
    parser.add_argument("--worker", nargs=2, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.worker:
        program, notes = int(args.worker[0]), [int(n) for n in args.worker[1].split(",")]
        json.dump(model_metrics(program, notes), sys.stdout)
        return 0
    print(f"library: {ensure_lib_path() or '(binding default)'}")
    ok = True
    for program in args.program or sorted(PROGRAMS):
        ok = check_program(program, args.override) and ok
    print(f"\nS5 {'pass' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
