#!/usr/bin/env python3
"""Auto-fit physical-model voice calibration constants against the oracle synth.

Closes the voicematch tuning loop mechanically: perturb one or more numeric
literals in the C++ voice sources, rebuild the shared library, render the model
and the reference ("oracle") synth from the same MIDI, score the timbre
mismatch, and minimise it with a pure-Python coordinate descent.

Run from the repo root through the bindings' rye environment:

    rye run --pyproject bindings/python/pyproject.toml \\
        python tools/voicematch/autofit.py --spec tools/voicematch/specs/example.json \\
            --program 56 --pattern sustain --notes 48,60,72 --max-evals 30

The tunable knobs are described by a spec JSON (see specs/example.json):

    [
      {
        "file": "src/midi/synth/brass_voice.cpp",
        "pattern": "kLipCouple = ([0-9.]+)f",
        "min": 2.0, "max": 8.0, "scale": "linear"
      }
    ]

Each knob's `pattern` is a Python regex with exactly one capturing group that
selects the numeric literal to tune (the surrounding text, including any `f`
suffix, is left untouched). The pattern must match its file exactly once; zero
or multiple matches abort before anything is touched.

Safety: the pristine text of every target file is snapshotted at startup and
restored in a `finally` block, so an exception or Ctrl-C never leaves the tree
perturbed. On a normal run the best values are then written back and a unified
diff plus the loss trajectory are printed. `--dry-run` restores pristine and
skips the write, reporting the diff it would have applied.

Build isolation: a dedicated build dir (default `build-autofit`) is configured
once with `-DBUILD_SHARED=ON` and rebuilt (`--target sonare_shared`) per
evaluation. Each model render runs in a fresh subprocess with SONARE_LIB_PATH
pointed at that dir's dylib, so every evaluation loads the freshly built code
(a dylib already mapped into this process would otherwise stay stale across
rebuilds). Do not point this at build-python-shared — that dir is shared with
other tooling.
"""

from __future__ import annotations

import argparse
import difflib
import json
import math
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from metrics import analyze_note, normalize_rms, to_mono
from patterns import build_pattern, pattern_length
from render_model import render_model
from render_oracle import render_oracle_fluidsynth
from smf import write_smf

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SR = 48000

# The spectral centroid is deliberately excluded from the loss: it depends on
# the probe note set (register weighting) and has been an unreliable, noisy
# signal in this harness. Match harmonic profile, intonation, and noise floor
# instead.


# --------------------------------------------------------------------------- #
# Spec + knob model
# --------------------------------------------------------------------------- #
@dataclass
class Knob:
    """One tunable numeric literal located by a single-group regex."""

    file: Path
    pattern: str
    lo: float
    hi: float
    log: bool
    span_start: int  # group(1) start offset in the pristine file text
    span_end: int
    start_value: float


def _load_spec(spec_path: Path) -> list[dict]:
    data = json.loads(spec_path.read_text())
    if not isinstance(data, list) or not data:
        raise ValueError(f"spec {spec_path} must be a non-empty JSON array of knobs")
    return data


def build_knobs(spec: list[dict], pristine: dict[Path, str]) -> list[Knob]:
    """Validate every knob's regex (exactly one match) and capture its span.

    Populates `pristine` with each unique file's original text as a side effect.
    """
    knobs: list[Knob] = []
    for i, entry in enumerate(spec):
        try:
            rel = entry["file"]
            pattern = entry["pattern"]
            lo = float(entry["min"])
            hi = float(entry["max"])
        except KeyError as exc:
            raise ValueError(f"knob #{i}: missing required field {exc}") from None
        scale = entry.get("scale", "linear")
        if scale not in ("linear", "log"):
            raise ValueError(f"knob #{i}: scale must be 'linear' or 'log', got {scale!r}")
        if lo >= hi:
            raise ValueError(f"knob #{i}: min ({lo}) must be < max ({hi})")
        if scale == "log" and lo <= 0.0:
            raise ValueError(f"knob #{i}: log scale needs min > 0, got {lo}")

        path = (REPO_ROOT / rel).resolve()
        if not path.exists():
            raise FileNotFoundError(f"knob #{i}: file not found: {rel}")
        if path not in pristine:
            pristine[path] = path.read_text()
        text = pristine[path]

        compiled = re.compile(pattern)
        matches = list(compiled.finditer(text))
        if len(matches) != 1:
            raise ValueError(
                f"knob #{i}: pattern {pattern!r} matched {len(matches)} times in {rel} "
                f"(need exactly 1)"
            )
        m = matches[0]
        if m.lastindex is None or m.lastindex < 1:
            raise ValueError(f"knob #{i}: pattern {pattern!r} has no capturing group")
        try:
            start_value = float(m.group(1))
        except (TypeError, ValueError):
            raise ValueError(
                f"knob #{i}: captured text {m.group(1)!r} is not a number"
            ) from None
        clamped = min(max(start_value, lo), hi)
        if clamped != start_value:
            print(
                f"note: knob #{i} start value {start_value} outside [{lo}, {hi}], "
                f"clamped to {clamped}",
                file=sys.stderr,
            )
        knobs.append(
            Knob(
                file=path,
                pattern=pattern,
                lo=lo,
                hi=hi,
                log=(scale == "log"),
                span_start=m.start(1),
                span_end=m.end(1),
                start_value=clamped,
            )
        )
    return knobs


def format_value(v: float) -> str:
    """Render a float as a C++ literal fragment that always carries a decimal.

    The captured group excludes any `f` suffix, so the substituted text must be
    a valid float literal on its own (e.g. `22` would become an invalid `22f`).
    """
    s = f"{v:.6g}"
    if "e" not in s and "E" not in s and "." not in s:
        s += ".0"
    return s


def materialize(knobs: list[Knob], values: list[float], pristine: dict[Path, str]) -> dict[Path, str]:
    """Produce each file's text with all its knob values spliced in.

    Splicing is computed against the pristine text (never re-matched on an
    already-edited buffer) so multiple knobs in one file cannot interfere.
    """
    by_file: dict[Path, list[tuple[Knob, float]]] = {}
    for knob, value in zip(knobs, values):
        by_file.setdefault(knob.file, []).append((knob, value))
    result: dict[Path, str] = {}
    for path, items in by_file.items():
        text = pristine[path]
        # Splice from the end so earlier offsets stay valid.
        for knob, value in sorted(items, key=lambda kv: kv[0].span_start, reverse=True):
            text = text[: knob.span_start] + format_value(value) + text[knob.span_end :]
        result[path] = text
    return result


def restore(pristine: dict[Path, str]) -> None:
    """Write every snapshotted file back to its pristine text."""
    for path, text in pristine.items():
        path.write_text(text)


# --------------------------------------------------------------------------- #
# Build + render + loss
# --------------------------------------------------------------------------- #
def configure_build(build_dir: Path, cmake: str) -> None:
    """Configure the isolated build dir once (idempotent)."""
    if (build_dir / "CMakeCache.txt").exists():
        return
    print(f"configuring {build_dir} (Release, BUILD_SHARED=ON)...", file=sys.stderr)
    subprocess.run(
        [cmake, "-S", str(REPO_ROOT), "-B", str(build_dir),
         "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED=ON"],
        check=True,
    )


def build_shared(build_dir: Path, cmake: str, jobs: int) -> None:
    """Rebuild the shared library target in the isolated build dir."""
    proc = subprocess.run(
        [cmake, "--build", str(build_dir), "--target", "sonare_shared", f"-j{jobs}"],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"build failed (rc={proc.returncode}):\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}"
        )
    # Ensure the freshly built dylib is loadable by ctypes on macOS.
    dylib = dylib_path(build_dir)
    if sys.platform == "darwin" and dylib is not None and shutil.which("install_name_tool"):
        subprocess.run(
            ["install_name_tool", "-id", "@loader_path/libsonare.dylib", str(dylib)],
            capture_output=True, text=True,
        )


def dylib_path(build_dir: Path) -> Path | None:
    """Locate the built libsonare shared library in the isolated build dir."""
    direct = build_dir / "lib" / "libsonare.dylib"
    if direct.exists():
        return direct
    for cand in build_dir.rglob("libsonare.dylib"):
        return cand
    for cand in build_dir.rglob("libsonare.so"):
        return cand
    return None


def oracle_rows(program: int, pattern_name: str, notes_csv: str, sf2: str) -> list[dict]:
    """Render the oracle once and return its per-note metrics (as report rows)."""
    pattern, total, analysis_notes = _score(program, pattern_name, notes_csv)
    smf_bytes = write_smf(pattern.notes, program=program, end_pad=pattern.tail)
    audio = render_oracle_fluidsynth(
        smf_bytes, total, SR, soundfont=Path(sf2) if sf2 else None,
    )
    mono = normalize_rms(to_mono(audio))
    return [
        analyze_note(mono, SR, note, note.start + note.dur + pattern.tail).to_dict()
        for note in analysis_notes
    ]


def _score(program: int, pattern_name: str, notes_csv: str):
    kwargs = {}
    if notes_csv:
        kwargs["notes"] = tuple(int(n) for n in notes_csv.split(","))
    pattern = build_pattern(pattern_name, program, **kwargs)
    return pattern, pattern_length(pattern), pattern.analysis_notes


def compute_loss(
    model_rows: list[dict],
    oracle_rows_: list[dict],
    *,
    n_harm: int,
    w_harm: float,
    w_cents: float,
    w_tnr: float,
) -> float:
    """Mean per-note weighted mismatch between the model and the oracle.

    Per note, from the same fields report.json carries:
      - harmonic profile: L1 distance of h1-normalized harmonics_db over the
        first `n_harm` harmonics (the most directly actionable timbre signal);
      - intonation: absolute f0 cents difference from the oracle;
      - noise floor: TNR shortfall, penalised only when the model is noisier
        than the oracle (a model cleaner than the oracle is not penalised, since
        the sampled oracle carries natural vibrato/breath noise).
    The spectral centroid is intentionally not part of the loss.
    """
    if not model_rows or len(model_rows) != len(oracle_rows_):
        return math.inf
    total = 0.0
    for m, o in zip(model_rows, oracle_rows_):
        harm_l1 = 0.0
        for mh, oh in zip(m["harmonics_db"][:n_harm], o["harmonics_db"][:n_harm]):
            if mh > -120.0 and oh > -120.0:
                harm_l1 += abs(mh - oh)
        cents = abs(m["f0_cents_err"] - o["f0_cents_err"])
        tnr_delta = m["tnr_db"] - o["tnr_db"]  # model - oracle
        tnr_pen = max(0.0, -tnr_delta)  # only when the model is noisier
        note_loss = w_harm * harm_l1 + w_cents * cents + w_tnr * tnr_pen
        if not math.isfinite(note_loss):
            return math.inf
        total += note_loss
    return total / len(model_rows)


def render_model_rows_subprocess(
    build_dir: Path, program: int, pattern_name: str, notes_csv: str,
) -> list[dict]:
    """Render the model in a fresh subprocess and return its per-note metrics.

    A subprocess is required so each evaluation loads the just-rebuilt dylib;
    the same process would keep the first-loaded image mapped across rebuilds.
    """
    dylib = dylib_path(build_dir)
    if dylib is None:
        raise RuntimeError(f"no libsonare dylib found under {build_dir}")
    env = dict(os.environ)
    env["SONARE_LIB_PATH"] = str(dylib)
    proc = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "_render_metrics",
         "--program", str(program), "--pattern", pattern_name, "--notes", notes_csv],
        capture_output=True, text=True, env=env,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"model render failed (rc={proc.returncode}):\n{proc.stderr.strip()[-2000:]}"
        )
    return json.loads(proc.stdout)


# --------------------------------------------------------------------------- #
# Optimiser (pure-Python coordinate descent + golden-section)
# --------------------------------------------------------------------------- #
class Evaluator:
    """Builds, renders, and scores a knob-value vector, with caching."""

    def __init__(self, knobs, pristine, oracle, args, build_dir):
        self.knobs = knobs
        self.pristine = pristine
        self.oracle = oracle
        self.args = args
        self.build_dir = build_dir
        self.cache: dict[tuple[str, ...], float] = {}
        self.n_builds = 0
        self.trajectory: list[tuple[float, float]] = []  # (best_so_far, this_loss)
        self.best_loss = math.inf
        self.best_values: list[float] | None = None

    def key(self, values: list[float]) -> tuple[str, ...]:
        return tuple(format_value(v) for v in values)

    def __call__(self, values: list[float]) -> float:
        key = self.key(values)
        if key in self.cache:
            return self.cache[key]
        edited = materialize(self.knobs, values, self.pristine)
        for path, text in edited.items():
            path.write_text(text)
        build_shared(self.build_dir, self.args.cmake, self.args.jobs)
        self.n_builds += 1
        model_rows = render_model_rows_subprocess(
            self.build_dir, self.args.program, self.args.pattern, self.args.notes,
        )
        loss = compute_loss(
            model_rows, self.oracle,
            n_harm=self.args.n_harm,
            w_harm=self.args.w_harm, w_cents=self.args.w_cents, w_tnr=self.args.w_tnr,
        )
        self.cache[key] = loss
        if loss < self.best_loss:
            self.best_loss = loss
            self.best_values = list(values)
        self.trajectory.append((self.best_loss, loss))
        print(
            f"  eval #{len(self.trajectory)} builds={self.n_builds} "
            f"values={[round(v, 5) for v in values]} loss={loss:.4f} "
            f"(best {self.best_loss:.4f})",
            file=sys.stderr,
        )
        return loss


def _to_opt(knob: Knob, value: float) -> float:
    return math.log(value) if knob.log else value


def _from_opt(knob: Knob, t: float) -> float:
    return math.exp(t) if knob.log else t


def golden_section(objective, a: float, b: float, max_evals: int, tol: float):
    """Minimise a unimodal 1-D objective on [a, b] within an eval budget.

    Returns (best_t, best_loss). `objective` is assumed cached, so re-probing a
    point is cheap.
    """
    inv_phi = (math.sqrt(5.0) - 1.0) / 2.0
    c = b - inv_phi * (b - a)
    d = a + inv_phi * (b - a)
    fc = objective(c)
    fd = objective(d)
    evals = 2
    best_t, best_f = (c, fc) if fc <= fd else (d, fd)
    while evals < max_evals and (b - a) > tol:
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - inv_phi * (b - a)
            fc = objective(c)
            evals += 1
            if fc < best_f:
                best_t, best_f = c, fc
        else:
            a, c, fc = c, d, fd
            d = a + inv_phi * (b - a)
            fd = objective(d)
            evals += 1
            if fd < best_f:
                best_t, best_f = d, fd
    return best_t, best_f


def optimize(evaluator: Evaluator, knobs: list[Knob], args) -> list[float]:
    """Coordinate descent over knobs; each pass golden-sections one knob."""
    current = [k.start_value for k in knobs]
    evaluator(current)  # baseline
    while len(evaluator.trajectory) < args.max_evals:
        improved = False
        for i, knob in enumerate(knobs):
            if len(evaluator.trajectory) >= args.max_evals:
                break
            budget = min(args.per_knob_evals, args.max_evals - len(evaluator.trajectory))
            if budget < 2:
                break
            a, b = _to_opt(knob, knob.lo), _to_opt(knob, knob.hi)
            tol = (b - a) * 1e-3

            def objective(t: float, _i=i, _knob=knob) -> float:
                trial = list(evaluator.best_values or current)
                trial[_i] = min(max(_from_opt(_knob, t), _knob.lo), _knob.hi)
                return evaluator(trial)

            best_t, best_f = golden_section(objective, a, b, budget, tol)
            if best_f < evaluator.best_loss + 1e-9 and evaluator.best_values is not None:
                best_val = min(max(_from_opt(knob, best_t), knob.lo), knob.hi)
                if abs(current[i] - best_val) > 0:
                    improved = True
                current[i] = best_val
        current = list(evaluator.best_values or current)
        if not improved:
            break
    return list(evaluator.best_values or current)


# --------------------------------------------------------------------------- #
# Internal render subcommand (runs in the per-eval subprocess)
# --------------------------------------------------------------------------- #
def render_metrics_main(argv: list[str]) -> int:
    """Render the model for one score and print its per-note metrics as JSON.

    Invoked as a subprocess by the optimiser with SONARE_LIB_PATH already set
    to the isolated build dir's dylib.
    """
    p = argparse.ArgumentParser(prog="autofit.py _render_metrics")
    p.add_argument("--program", type=int, required=True)
    p.add_argument("--pattern", required=True)
    p.add_argument("--notes", default="")
    a = p.parse_args(argv)

    pattern, total, analysis_notes = _score(a.program, a.pattern, a.notes)
    smf_bytes = write_smf(pattern.notes, program=a.program, end_pad=pattern.tail)
    audio = render_model(smf_bytes, total, SR)
    mono = normalize_rms(to_mono(np.asarray(audio, dtype=np.float32)))
    rows = [
        analyze_note(mono, SR, note, note.start + note.dur + pattern.tail).to_dict()
        for note in analysis_notes
    ]
    print(json.dumps(rows))
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def report_result(knobs, pristine, best_values, evaluator, dry_run: bool) -> None:
    """Print the loss trajectory, per-knob deltas, and the source diff."""
    print("\n== loss trajectory (best-so-far) ==")
    if evaluator.trajectory:
        best_curve = [round(b, 4) for b, _ in evaluator.trajectory]
        print("  " + " -> ".join(str(x) for x in best_curve))
        print(f"  initial {evaluator.trajectory[0][1]:.4f}  ->  best {evaluator.best_loss:.4f}")
    else:
        print("  (no evaluations)")

    print("\n== knob values (start -> best) ==")
    for knob, start, best in zip(knobs, (k.start_value for k in knobs), best_values):
        rel = knob.file.relative_to(REPO_ROOT)
        print(f"  {rel}  {knob.pattern!r}:  {format_value(start)} -> {format_value(best)}")

    edited = materialize(knobs, best_values, pristine)
    print("\n== source diff (pristine -> best) ==")
    any_diff = False
    for path, new_text in edited.items():
        rel = str(path.relative_to(REPO_ROOT))
        diff = difflib.unified_diff(
            pristine[path].splitlines(keepends=True),
            new_text.splitlines(keepends=True),
            fromfile=f"a/{rel}", tofile=f"b/{rel}",
        )
        chunk = "".join(diff)
        if chunk:
            any_diff = True
            print(chunk, end="")
    if not any_diff:
        print("  (no change from pristine)")
    if dry_run:
        print("\n--dry-run: source left pristine, best values NOT written.")
    else:
        print("\nBest values written to source.")


def run(args) -> int:
    spec_path = Path(args.spec).resolve()
    spec = _load_spec(spec_path)
    pristine: dict[Path, str] = {}
    knobs = build_knobs(spec, pristine)

    build_dir = (REPO_ROOT / args.build_dir).resolve()
    if build_dir.name == "build-python-shared":
        raise ValueError("refusing to use build-python-shared; pick a private build dir")
    configure_build(build_dir, args.cmake)

    print("rendering oracle (once)...", file=sys.stderr)
    oracle = oracle_rows(args.program, args.pattern, args.notes, args.sf2)
    if not oracle:
        raise RuntimeError(
            f"pattern '{args.pattern}' has no analyzable notes for program {args.program}"
        )

    evaluator = Evaluator(knobs, pristine, oracle, args, build_dir)
    best_values: list[float] = [k.start_value for k in knobs]
    try:
        best_values = optimize(evaluator, knobs, args)
    finally:
        # Always return the tree to pristine, whatever happened above.
        restore(pristine)

    if evaluator.best_values is not None:
        best_values = evaluator.best_values

    if not args.dry_run:
        edited = materialize(knobs, best_values, pristine)
        for path, text in edited.items():
            path.write_text(text)

    report_result(knobs, pristine, best_values, evaluator, args.dry_run)
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "_render_metrics":
        return render_metrics_main(sys.argv[2:])

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--spec", required=True, help="knob spec JSON path")
    parser.add_argument("--program", type=int, required=True, help="GM program to score")
    parser.add_argument("--pattern", default="sustain", help="probe pattern (default: sustain)")
    parser.add_argument("--notes", default="", help="override probe notes, e.g. '48,60,72'")
    parser.add_argument("--sf2", default="", help="oracle SoundFont path override")
    parser.add_argument("--max-evals", type=int, default=30, dest="max_evals",
                        help="total build+render evaluations (default: 30)")
    parser.add_argument("--per-knob-evals", type=int, default=6, dest="per_knob_evals",
                        help="golden-section budget per knob per pass (default: 6)")
    parser.add_argument("--n-harm", type=int, default=10, dest="n_harm",
                        help="harmonics counted in the L1 timbre term (default: 10)")
    parser.add_argument("--w-harm", type=float, default=1.0, dest="w_harm",
                        help="weight on the harmonic-profile L1 term")
    parser.add_argument("--w-cents", type=float, default=0.5, dest="w_cents",
                        help="weight on the intonation (cents) term")
    parser.add_argument("--w-tnr", type=float, default=1.0, dest="w_tnr",
                        help="weight on the noise-floor (TNR shortfall) term")
    parser.add_argument("--build-dir", default="build-autofit", dest="build_dir",
                        help="isolated build dir (default: build-autofit)")
    parser.add_argument("--jobs", type=int, default=8, help="parallel build jobs")
    parser.add_argument("--cmake", default="cmake", help="cmake executable")
    parser.add_argument("--dry-run", action="store_true", dest="dry_run",
                        help="restore pristine and skip writing the best values")
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
