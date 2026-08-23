#!/usr/bin/env python3
"""Auto-fit physical-model voice calibration constants against an oracle.

Closes the voicematch tuning loop mechanically: set one or more of a voice's
calibration constants, render the model and the reference ("oracle") from the
same MIDI, score the timbre mismatch, and minimise it.

Run from the repo root through the bindings' rye environment:

    rye run --pyproject bindings/python/pyproject.toml \\
        python tools/voicematch/autofit.py --spec tools/voicematch/specs/piano.json \\
            --program 0 --pattern sustain --notes 48,60,72 --max-evals 200

This module is the loop and the CLI; the pieces it drives live beside it —
`knobs` (what a fit may move and over what range), `catalogue` (what the library
reports about its own knob space), `loss` (from a render to the number being
minimised), `optimizers`, `staging` (screening and staged fitting), `writeback`
and `report`.

Knobs — two kinds, freely mixed in one spec
-------------------------------------------
A *runtime* knob names a `SONARE_TUNABLE` constant (see src/util/tunable.h):

    [{ "tunable": "kHammerWidthHarmonics", "min": 2.5, "max": 9.0, "scale": "log" }]

Its default is read straight from the source, and the fit sets it through the
`SONARE_TUNING_OVERRIDES` environment variable — so the library is built ONCE
for the whole run and an evaluation costs a render, not a rebuild. This is the
form to use; a spec made only of runtime knobs affords hundreds of evaluations
where a rebuilding one affords tens.

A *source* knob points a regex at a numeric literal instead, for a constant
that has not been made tunable:

    [{ "file": "src/midi/synth/brass_voice.cpp",
       "pattern": "kLipCouple = ([0-9.]+)f", "min": 2.0, "max": 8.0 }]

The regex needs exactly one capturing group selecting the literal (the `f`
suffix stays out of the group), and must match its file exactly once. Any
source knob in the spec forces a rebuild per evaluation.

Oracle — a captured corpus, fluidsynth, or your own WAV
-------------------------------------------------------
`--corpus` is the one to reach for when a capture exists. It points at the
directory `capture.py corpus` wrote, and the probe then IS the capture: its
notes, its velocities and its gate come from the manifest, and the oracle is the
captured audio assembled onto that timeline.

    autofit.py --spec tools/voicematch/specs/piano_corpus.json --program 0 \
        --corpus <capture dir> --corpus-timbre c7-close \
        --notes 36,60,84 --velocities 56,120 --optimizer cmaes --max-evals 200

That matters more than it sounds. Without it a fit scores a stimulus of its own
— three notes at one velocity held two seconds — while `profile.py compare`
reports against a grid of fifteen notes at four velocities held eight, and a
value fitted on the first cannot be read off the second. It is also what makes
the aftersound scoreable at all: nothing shorter than the capture's gate has any
frames in the 2-6 s band the `tail` term fits. `--notes` / `--velocities` cut
the grid down, and the run prints how much audio a render costs before it starts.

By default the oracle is instead rendered live with fluidsynth + a GM SoundFont.
To fit against something else with no capture — a VST, a plugin host, a
recording of a real instrument — export the probe and hand back the rendering:

    voicematch.py export-probe --programs 0 --pattern sustain
    autofit.py --spec ... --program 0 --oracle-wav /path/to/rendered.wav

The WAV is aligned to the score automatically (any lead-in silence is measured
and removed), so it does not have to start on the first sample.

Drums
-----
`--drum-note N` fits a percussion instrument rather than a GM program:

    autofit.py --spec auto --program 0 --drum-note 38 \
        --optimizer cmaes --max-evals 200 --validate-velocities 48,88,112

The probe moves to the drum channel, where the note number selects the
instrument and `--program` selects the kit, and it strikes that one instrument
at three velocities — velocity being the only axis a drum note varies along.
Scoring moves with it: a hit has no fundamental, so the harmonic ladder, the
intonation error and the tone-to-noise ratio all measure a frequency the sound
does not contain, and the percussion set (`--w-band`, `--w-bdecay`) measures the
1/3-octave level profile and how fast each octave of it dies instead. See
`loss.percussion_terms`. `--validate-velocities` is the held-out check, since
there is no register to hold notes out of.

Loss
----
`--w-harm` / `--w-cents` / `--w-tnr` / `--w-env` / `--w-init` / `--w-slope`
weight the interpretable per-note metrics — `--w-band` / `--w-bdecay` / `--w-env`
for a drum probe — and `--w-mss` adds a multi-scale STFT distance over the whole
render, which sees everything the metric set does not model. See
`loss.loss_terms` and `loss.percussion_terms`.

Four more exist because a shape metric cannot see them:

    --w-tail    the per-harmonic decay 2-6 s in — the aftersound, which on a
                piano is most of the note and which no probe shorter than the
                capture's gate has any frames to fit
    --w-crest   peak minus held RMS per note. Gain-invariant, and the only term
                that can fail on a note whose envelope never falls after its
                attack; every other term here is normalised past that
    --w-level   how the loudness is distributed across the grid, with the grid's
                own median offset removed so an output-gain difference is not
                fitted with voicing knobs
    --w-hf      the attack's high-band tilt in 20 ms slices over the first
                120 ms, where a strike-noise path with the wrong filter order
                arrives as a tick rather than as brightness

**Every weight except harm, cents and tnr defaults to zero.** A run left on the
defaults scores a time-averaged harmonic ladder, an intonation error and a noise
floor, and nothing else — no envelope, no decay, no level, no attack. Rather
than remembering which flags a given voice needs, put them in the spec:

    { "weights": {"harm": 1, "slope": 1, "tail": 2, "crest": 2, "level": 2},
      "knobs": [ ... ] }

An explicit `--w-*` on the command line still wins. `specs/piano_corpus.json` is
the worked example.

Each term is normalised to its value at the start point, so the start scores
exactly 1.0 and a reported 0.85 means 15 % better than the compiled-in values.
Without that the weights would be dominated by whichever term happens to be
numerically largest — the harmonic term is an L1 sum in dB and runs to tens
while the multi-scale term is a fraction. `--raw-loss` restores unit weighting.

Ranges
------
A search reports the best point it was allowed to visit, so a range that does
not contain the answer produces a result indistinguishable from one that does:
the value pins to the bound and is written back as an optimum. Every run
therefore ends by naming any knob sitting on an end of its range — including one
that started there, which a start-to-best diff can never show, because by that
measure nothing happened.

Optimiser
---------
`--optimizer coord` (default) is coordinate descent with a golden-section line
search per knob — cheap, and readable when a knob has an obvious optimum, but
it stalls on interacting knobs and cannot be parallelised (each probe is chosen
from the previous one's result). `--optimizer cmaes` handles interaction, is the
better choice once runtime knobs make evaluations cheap, and renders its whole
population concurrently under `--workers N`. `--restarts N` spends a bigger
budget on escaping a local optimum rather than on refining one.

Cutting the problem down
------------------------
`--spec auto` offers every knob the program's patch and engine expose, which for
most programs is more than a fit can use well:

    --screen        probe each knob at both ends, fit only the ones that move
                    the loss, and name the ones dropped
    --stages        fit the excitation knobs against the onset evidence, then
                    the decay knobs against the decay evidence, then everything
                    together — instead of asking one search to separate two
                    things that trade against each other
    --validate-notes  score the result on notes the fit never saw. Nothing else
                    in the run can tell whether the values generalise or whether
                    they are right on three probe notes and wrong elsewhere.

Write-back
----------
A `SONARE_TUNABLE` knob is written back as its new compiled-in default, and a
source knob as its new literal. A per-program patch field has neither — the
program table builds most patches through helper lambdas taking positional
arguments, so no literal in it belongs to a named field — and is written as an
explicit `o.<patch>.<field> = <value>f;` in the table source, which is the idiom
that table already uses for its own exceptions. A drum note is written the same
way into the drum table (`t[38].percussion.wire_buzz = ...f;`), which names every
note directly. Family patches (`fam3.`) are built by a loop with no per-patch
site, so their values are reported rather than written. See `writeback`.

Safety: the pristine text of every source-knob file is snapshotted at startup
and restored in a `finally` block, so an exception or Ctrl-C never leaves the
tree perturbed. The restore covers the files this run wrote and only while they
still hold what it wrote, so a fit left running for hours cannot roll back
whatever else was edited in the tree meanwhile. On a normal run the best values
are then written back and a unified diff plus the loss trajectory are printed;
`--dry-run` reports the diff it would have applied without writing it. `--out`
additionally records the whole result as JSON.

Build isolation: a dedicated build dir (default `build-autofit`) is configured
with `-DBUILD_SHARED=ON`, plus `-DBUILD_TUNING=ON` when the spec has runtime
knobs. Each model render runs in a fresh subprocess with SONARE_LIB_PATH
pointed at that dir's dylib, so a rebuilding run never reads a dylib already
mapped into this process. Do not point this at build-python-shared — that dir
is shared with other tooling.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo
sys.path.insert(0, str(Path(__file__).resolve().parent))

from _repo import REPO_ROOT  # noqa: E402
from build_lib import build_shared, configure_build, dylib_path  # noqa: E402
from catalogue import Catalogue, drum_patch_key, dump_catalogue  # noqa: E402
from corpus import Corpus, corpus_oracle, corpus_pattern, describe, load_corpus  # noqa: E402
from knobs import (  # noqa: E402
    at_bound,
    auto_spec,
    build_knobs,
    format_value,
    load_spec,
    load_spec_weights,
    tunable_overrides,
)
from loss import (  # noqa: E402
    LOSS_TERMS,
    LossWeights,
    cli_weights,
    mss_distance,
    probe_rows,
    score_terms,
)
from metrics import normalize_rms, to_mono  # noqa: E402
from optimizers import cma_es, optimize  # noqa: E402
from patterns import build_pattern, pattern_length  # noqa: E402
from render_model import render_model  # noqa: E402
from render_oracle import (  # noqa: E402
    add_oracle_args,
    obtain_oracle,
    oracle_may_carry_room,
)
from report import report_result  # noqa: E402
from room import apply_room, estimate_room, fit_room_ir  # noqa: E402
from smf import write_smf  # noqa: E402
from staging import SubEvaluator, run_stages, screen_knobs  # noqa: E402
from writeback import materialize, restore, write_edits  # noqa: E402

SR = 48000


# --------------------------------------------------------------------------- #
# The probe: what is rendered, and what comes back measured
# --------------------------------------------------------------------------- #
def _score(program: int, pattern_name: str, notes_csv: str, velocities_csv: str = "",
           corpus: Corpus | None = None):
    kwargs = {}
    if notes_csv:
        kwargs["notes"] = tuple(int(n) for n in notes_csv.split(","))
    if velocities_csv:
        kwargs["velocities"] = tuple(int(v) for v in velocities_csv.split(","))
    # A corpus probe is laid out by the capture rather than by a builder: its
    # note list, its gate and its slot spacing all come from the manifest, so
    # the model renders the same stimulus the reference was recorded under.
    pattern = corpus_pattern(corpus, **kwargs) if corpus is not None \
        else build_pattern(pattern_name, program, **kwargs)
    return pattern, pattern_length(pattern), pattern.analysis_notes


def resolve_corpus(args) -> Corpus | None:
    """Load the capture manifest a corpus run scores against, if there is one."""
    path = getattr(args, "corpus", "")
    if not path:
        return None
    return load_corpus(path, getattr(args, "corpus_timbre", ""))


def catalogue_pattern(args) -> str:
    """The pattern name the knob dump renders under.

    The dump exists to make the library sound the voice once and report every
    key it consulted, so any pattern that sounds it will do — but it renders in
    a subprocess that builds the probe from a name alone, and a corpus probe is
    built from a manifest that subprocess is not given. Sounding the same notes
    as a sustain probe reports the same keys.
    """
    return "sustain" if args.pattern == "corpus" else args.pattern


def check_holdout_oracle(args) -> None:
    """Refuse a hold-out check that would be scored against the wrong audio.

    `--oracle-wav` is one fixed rendering of one probe: it ignores the score
    entirely, so a hold-out run asking for different notes gets the *fitted*
    notes' audio back and compares the model's held-out register against it.
    The comparison still produces a number, and the sustain pattern's timeline
    does not depend on pitch, so nothing downstream looks wrong — the report
    then states that the values do or do not generalise on the strength of a
    measurement taken against other notes.

    The fix is a second rendering, `--validate-oracle-wav`, of the probe
    exported for the held-out set. Every other oracle route re-renders from the
    score and needs nothing. Checked before the fit rather than after it,
    because the alternative is discovering it hours later.
    """
    if not getattr(args, "oracle_wav", ""):
        if getattr(args, "validate_oracle_wav", ""):
            raise ValueError(
                "--validate-oracle-wav belongs with --oracle-wav: the fit and its "
                "hold-out have to be scored against the same reference, and this run's "
                "fit oracle is rendered here rather than supplied"
            )
        return
    percussive = getattr(args, "percussive", False)
    held = (getattr(args, "validate_velocities", "") if percussive
            else getattr(args, "validate_notes", ""))
    if not held or getattr(args, "validate_oracle_wav", ""):
        return
    axis = "--validate-velocities" if percussive else "--validate-notes"
    flag = "--velocities" if percussive else "--notes"
    raise ValueError(
        f"{axis} with --oracle-wav needs its own reference: the WAV is a fixed "
        f"rendering of the fitted probe, so the hold-out would be scored against the "
        f"audio of the notes the fit already saw. Export the hold-out probe "
        f"(`voicematch.py export-probe --pattern {args.pattern} {flag} {held}`), render "
        f"it the same way, and pass it as --validate-oracle-wav; or drop {axis}."
    )


def resolve_probe(args) -> None:
    """Settle the probe the whole run scores against, and what it can measure.

    `--drum-note` is the one flag that changes the shape of a run rather than a
    value in it: it moves the probe onto the drum channel, where a note number
    selects an instrument rather than a pitch, and swaps the harmonic metric set
    for the percussion one. Everything downstream reads `args.percussive` rather
    than re-deriving it, so the decision is made once and in one place.
    """
    if args.drum_note is not None:
        if not 0 <= args.drum_note < 128:
            raise ValueError(f"--drum-note must be 0..127, got {args.drum_note}")
        if args.pattern == "sustain":  # the melodic default; an explicit one wins
            args.pattern = "drum"
        if not args.notes:
            args.notes = str(args.drum_note)
    corpus = resolve_corpus(args)
    if corpus is not None:
        if args.drum_note is not None:
            raise ValueError(
                "--corpus and --drum-note are alternatives: a captured corpus is a grid of "
                "pitched single notes, and a drum probe scores a channel it has no captures for"
            )
        if getattr(args, "oracle_wav", ""):
            raise ValueError(
                "--corpus and --oracle-wav both name the reference; a corpus run assembles "
                "its oracle from the capture, so drop one of them"
            )
        args.pattern = "corpus"
    pattern, _, analysis_notes = _score(
        args.program, args.pattern, args.notes, args.velocities, corpus=corpus
    )
    if corpus is not None:
        print(describe(corpus, pattern), file=sys.stderr)
    args.percussive = pattern.percussive
    if args.percussive and args.drum_note is None:
        raise ValueError(
            f"pattern {args.pattern!r} probes the drum channel; pass --drum-note N so the "
            f"fit knows which drum note's knobs to move"
        )
    if args.drum_note is not None and not args.percussive:
        raise ValueError(
            f"--drum-note needs a drum-channel pattern; {args.pattern!r} is written on "
            f"channel 1 and would sound a pitch rather than the kit"
        )
    check_holdout_oracle(args)
    if not analysis_notes:
        per_note = {t: w for t, w in cli_weights(args).items() if t != "mss" and w > 0.0}
        if per_note:
            raise ValueError(
                f"pattern {args.pattern!r} has no analyzable notes, so the per-note terms "
                f"{sorted(per_note)} have nothing to measure. Weight only --w-mss, or "
                f"pick a pattern with analysis notes."
            )


def oracle_reference(args) -> tuple[list[dict], np.ndarray, np.ndarray | None]:
    """Resolve the oracle once: per-note metrics, mono render, and its room.

    The mono render is what the multi-scale STFT term compares against; it is
    kept in the parent process so an evaluation only has to ship the model's
    audio back.

    The third return value is the space the oracle was recorded in, or None when
    it is dry or the caller asked for no room correction. Every model render is
    convolved with a response matching it before any metric is taken. Without
    that, an oracle rendered by an external host in a hall — which is the only
    way most reference recordings of an organ, a harp or a string section exist
    — makes the release read as far too long, the tone-to-noise as far too low
    and the sustain slope as far too flat, and the fit spends its knobs
    reproducing the building instead of the instrument.
    """
    corpus = resolve_corpus(args)
    pattern, total, _ = _score(
        args.program, args.pattern, args.notes, getattr(args, "velocities", ""), corpus=corpus
    )
    if corpus is not None:
        # Assembled from the capture rather than played: the reference for a
        # corpus run already exists as audio, one file per slot, and nothing
        # about it depends on a plugin still being installed.
        audio = corpus_oracle(corpus, pattern, SR)
    else:
        smf_bytes = write_smf(
            pattern.notes, program=args.program, channel=pattern.channel, end_pad=pattern.tail
        )
        audio = obtain_oracle(args, smf_bytes, total, SR, [n.start for n in pattern.notes])

    # Only an oracle rendered outside libsonare's dry path can carry a room —
    # a supplied WAV, or an AudioUnit that was not asked to switch its effects
    # off. The built-in fluidsynth oracle is rendered with its reverb and chorus
    # units switched off, so any decay measured there is the instrument's own,
    # and estimating a room from it would invent one and then convolve the model
    # with it.
    room = None
    may_carry_room = (not corpus.dry) if corpus is not None else oracle_may_carry_room(args)
    if getattr(args, "room", "auto") != "none" and may_carry_room:
        measured = estimate_room(
            audio, SR, [(n.start, n.start + n.dur) for n in pattern.notes]
        )
        if measured.is_dry():
            print(f"oracle room: dry (RT60 {measured.rt60_s:.2f}s) — no room correction",
                  file=sys.stderr)
        else:
            print(f"oracle room: RT60 {measured.rt60_s:.2f}s, "
                  f"tail level {measured.tail_db:+.1f}dB, HF ratio {measured.hf_ratio:.2f} — "
                  f"the model is placed in a matching space before every measurement",
                  file=sys.stderr)
            if measured.truncated():
                print(f"  note: the probe's shortest silence is {measured.tail_window_s:.1f}s, "
                      f"less than the {measured.rt60_s * 25.0 / 60.0:.1f}s this decay needs to "
                      f"fall 25 dB — the RT60 is likely underestimated. Re-export the probe "
                      f"with --pattern room-probe to measure it properly.", file=sys.stderr)
            room = measured

    raw = to_mono(audio)
    mono = normalize_rms(raw)
    return probe_rows(mono, pattern, SR, raw=raw), mono, room


def render_model_rows_subprocess(
    build_dir: Path, program: int, pattern_name: str, notes_csv: str,
    *, velocities_csv: str = "", overrides: str = "", want_audio: bool = False,
    room_ir: Path | None = None, corpus: Corpus | None = None,
) -> tuple[list[dict], np.ndarray | None]:
    """Render the model in a fresh subprocess; return per-note metrics (+ audio).

    A subprocess is required for two reasons: a rebuilding run must load the
    just-rebuilt dylib (the same process would keep the first-loaded image
    mapped), and a runtime-knob run must have `SONARE_TUNING_OVERRIDES` in the
    environment before the library's static initialisers read it.

    The mono render comes back through a temporary `.npy` rather than stdout —
    a ten-second probe is millions of samples, which JSON would spend more time
    encoding than the render itself takes.
    """
    dylib = dylib_path(build_dir)
    if dylib is None:
        raise RuntimeError(f"no libsonare dylib found under {build_dir}")
    env = dict(os.environ)
    env["SONARE_LIB_PATH"] = str(dylib)
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    else:
        env.pop("SONARE_TUNING_OVERRIDES", None)

    cmd = [sys.executable, str(Path(__file__).resolve()), "_render_metrics",
           "--program", str(program), "--pattern", pattern_name, "--notes", notes_csv,
           "--velocities", velocities_csv]
    if room_ir is not None:
        cmd += ["--room-ir", str(room_ir)]
    if corpus is not None:
        # Only the manifest travels: the child renders the model on the corpus
        # timeline and never touches the captured audio, which stays in the
        # parent where the oracle rows were measured from it once.
        cmd += ["--corpus", str(corpus.root), "--corpus-timbre", corpus.timbre]
    with tempfile.TemporaryDirectory(prefix="autofit_") as tmp:
        audio_path = Path(tmp) / "model.npy"
        if want_audio:
            cmd += ["--dump-audio", str(audio_path)]
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if proc.returncode != 0:
            raise RuntimeError(
                f"model render failed (rc={proc.returncode}):\n{proc.stderr.strip()[-2000:]}"
            )
        audio = np.load(audio_path) if want_audio and audio_path.exists() else None
    return json.loads(proc.stdout), audio


# --------------------------------------------------------------------------- #
# The objective the optimisers are handed
# --------------------------------------------------------------------------- #
class Evaluator:
    """Builds (when it must), renders, and scores a knob-value vector.

    A spec of runtime knobs only builds once for the whole run and pushes each
    candidate through the environment; one source knob anywhere in the spec
    puts every evaluation back behind a rebuild.

    The cache holds the raw per-term mismatch rather than the combined loss, so
    a staged fit that changes the weights between stages can re-score a point it
    has already rendered for free instead of re-rendering it under weights the
    cached number no longer reflects.
    """

    def __init__(self, knobs, pristine, oracle, oracle_audio, args, build_dir, room_ir=None,
                 corpus=None):
        self.knobs = knobs
        self.pristine = pristine
        self.oracle = oracle
        self.oracle_audio = oracle_audio
        self.args = args
        self.build_dir = build_dir
        self.room_ir = room_ir
        self.corpus = corpus
        self.percussive = bool(getattr(args, "percussive", False))
        self.needs_rebuild = any(k.tunable is None for k in knobs)
        self.built = False
        # What this process has actually written to the tree, and what it wrote,
        # so the restore in the `finally` puts back its own edits and nothing
        # else. `pristine` is wider than this on purpose: it also snapshots the
        # declaration file of every runtime knob, which the fit never writes.
        self.written: dict[Path, str] = {}
        self.cache: dict[tuple[str, ...], dict[str, float] | None] = {}
        self.n_builds = 0
        self.stage = "fit"
        # (best_so_far, this_loss, stage). The stage matters because a staged
        # fit changes the weights between stages, so two losses are only
        # comparable within one of them.
        self.trajectory: list[tuple[float, float, str]] = []
        self.best_loss = math.inf
        self.best_values: list[float] | None = None
        self.loss = LossWeights(cli_weights(args))
        self.normalize = not args.raw_loss
        self.baseline_terms: dict[str, float] | None = None
        # A rebuild rewrites the shared tree, so its evaluations can only ever
        # run one at a time however many workers were asked for.
        self.workers = 1 if self.needs_rebuild else max(1, args.workers)
        self.quiet = False
        self._offset_reported = False

    def key(self, values: list[float]) -> tuple[str, ...]:
        return tuple(format_value(v) for v in values)

    def restage(self, weights: dict[str, float], name: str = "fit") -> None:
        """Switch to a new set of term weights and forget the previous best.

        The cached renders survive — only the way they are scored changes — but
        a best-so-far measured under the previous weights is not comparable and
        would otherwise be carried into a stage that never chose it.
        """
        self.stage = name
        self.loss = LossWeights(weights)
        if self.baseline_terms is not None:
            # Same reference point, re-scored: the per-term scales do not depend
            # on the weights, but what the start point scores does, and that is
            # what every stage's loss is a ratio of.
            self.loss.calibrate(self.baseline_terms)
        self.best_loss = math.inf
        self.best_values = None

    def _ensure_built(self) -> None:
        if not self.built and not self.needs_rebuild:
            build_shared(self.build_dir, self.args.cmake, self.args.jobs)
            self.n_builds += 1
            self.built = True

    def _render_terms(self, values: list[float]) -> dict[str, float] | None:
        """Render one candidate and reduce it to raw loss terms. Thread-safe."""
        want_audio = self.args.w_mss > 0.0
        model_rows, model_audio = render_model_rows_subprocess(
            self.build_dir, self.args.program, self.args.pattern, self.args.notes,
            velocities_csv=self.args.velocities,
            overrides=tunable_overrides(self.knobs, values),
            want_audio=want_audio, room_ir=self.room_ir, corpus=self.corpus,
        )
        mss = 0.0
        if want_audio and model_audio is not None:
            mss = mss_distance(model_audio, self.oracle_audio)
        return score_terms(model_rows, self.oracle, n_harm=self.args.n_harm, mss=mss,
                           percussive=self.percussive)

    def _score_cached(self, values: list[float], terms: dict[str, float] | None) -> float:
        """Score an already-rendered candidate under the current weights.

        A cache hit still decides things. Every stage begins by re-scoring the
        point it inherited, which is by construction already rendered, so a
        stage whose samples never beat its own start point would otherwise end
        with `best_loss` still at infinity and hand back the first finite loss
        it happened to see — a candidate worse than the one it was given, on its
        way into the source.
        """
        loss = self.loss.combine(terms)
        if loss < self.best_loss:
            self.best_loss = loss
            self.best_values = list(values)
        return loss

    def _report_level_offset(self, terms: dict[str, float]) -> None:
        """Name the whole-grid level difference once, since the loss removes it.

        The level term scores how the level is distributed across the probe,
        with the grid's median offset taken out — otherwise a fit would spend
        its knobs on an output gain, which is not a property of the instrument
        and not what any of these knobs are for. That leaves the offset itself
        unmeasured by anything, and a model that is uniformly nine decibels over
        its reference is worth knowing about even though no knob here should be
        the one to fix it.
        """
        if self._offset_reported:
            return
        offset = terms.get("level_offset_db")
        if offset is None:
            return
        self._offset_reported = True
        if abs(offset) >= 1.0:
            print(f"level: the model's held RMS runs {offset:+.1f} dB against the reference "
                  f"across the whole grid. The level term scores the spread around that "
                  f"offset, not the offset itself.", file=sys.stderr)

    def _record(self, values: list[float], terms: dict[str, float] | None) -> float:
        """Score a rendered candidate, update the best, and log it."""
        if terms is not None:
            self._report_level_offset(terms)
        if self.normalize and self.loss.scales is None and terms is not None:
            self.baseline_terms = dict(terms)
            self.loss.calibrate(terms)
        loss = self.loss.combine(terms)
        if loss < self.best_loss:
            self.best_loss = loss
            self.best_values = list(values)
        self.trajectory.append((self.best_loss, loss, self.stage))
        if not self.quiet:
            # float() before the format: a numpy scalar's repr would drown the
            # line. Past a handful of knobs the vector is unreadable anyway, so
            # only the terms that moved the loss are worth the width.
            if len(values) <= 8:
                shown = "values=[" + ", ".join(f"{float(v):.5g}" for v in values) + "] "
            else:
                shown = self._term_summary(terms)
            print(
                f"  eval #{len(self.trajectory)} builds={self.n_builds} "
                f"{shown}loss={loss:.4f} (best {self.best_loss:.4f})",
                file=sys.stderr,
            )
        return loss

    def _term_summary(self, terms: dict[str, float] | None) -> str:
        """The active terms as ratios of their value at the start point."""
        if terms is None:
            return "unscorable "
        scales = self.loss.scales
        parts = []
        for name in self.loss.active():
            value = terms.get(name, 0.0)
            parts.append(f"{name}={value / scales[name]:.2f}" if scales else
                         f"{name}={value:.3g}")
        return " ".join(parts) + " "

    def __call__(self, values: list[float]) -> float:
        key = self.key(values)
        if key in self.cache:
            return self._score_cached(values, self.cache[key])
        if self.needs_rebuild:
            # `full`: every source-knob file, every knob, whether or not this
            # candidate moved it. A file left out because its knob formats back
            # to its start value would still hold the previous candidate's text,
            # and this render would score a vector nothing ever assembled.
            write_edits(
                materialize(self.knobs, values, self.pristine, full=True), self.written
            )
            build_shared(self.build_dir, self.args.cmake, self.args.jobs)
            self.n_builds += 1
        else:
            self._ensure_built()
        terms = self._render_terms(values)
        self.cache[key] = terms
        return self._record(values, terms)

    def evaluate_batch(self, batch: list[list[float]]) -> list[float]:
        """Score several candidates at once, rendering them concurrently.

        Each render is an independent subprocess, so the only thing serialising
        them was the loop that launched them. Threads are the right tool despite
        the GIL for exactly that reason: the work happens in child processes and
        `subprocess.run` releases the interpreter while it waits.

        Scoring stays on this thread and in submission order, so the trajectory,
        the log and the best-so-far read identically to a serial run — only the
        wall clock differs. A batch containing a rebuilding knob falls back to
        the serial path, since a rebuild rewrites the tree every render reads.
        """
        if self.workers <= 1 or self.needs_rebuild or len(batch) <= 1:
            return [self(values) for values in batch]
        self._ensure_built()

        pending = {}
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            for values in batch:
                key = self.key(values)
                if key not in self.cache and key not in pending:
                    pending[key] = pool.submit(self._render_terms, values)
            results: list[float] = []
            for values in batch:
                key = self.key(values)
                if key in pending:
                    self.cache[key] = pending.pop(key).result()
                    results.append(self._record(values, self.cache[key]))
                else:
                    results.append(self._score_cached(values, self.cache[key]))
        return results


def report_pinned(knobs, best_values: list[float]) -> list[str]:
    """Name every knob whose result sits on the end of its range.

    A search reports the best point it was allowed to visit, and a range that
    does not contain the answer produces one indistinguishable from a range that
    does: the value pins to the bound and is written back as an optimum. It is
    the most expensive failure this tool has, because nothing about the output
    looks wrong — the loss went down, the diff is small, the report is clean.
    The treble decay constant was searched over [0.5, 3.0] when it wanted 5.0,
    and 3.0 is what such a run would have reported.

    A pinned knob is not automatically wrong. A bound the engine enforces is a
    real end of the space, and an optimum genuinely sitting there is a result.
    What it is never safe to do is read it as an interior optimum, so it is
    named and the run says which end and how to widen it.

    Every knob is checked, not only the ones that moved. A start value the spec
    had to clamp into range starts pinned and stays pinned, and that is the case
    worth catching most: it never appears in a start-to-best diff, because by
    that measure nothing happened.
    """
    pinned = []
    for knob, value in zip(knobs, best_values):
        end = at_bound(knob, value)
        if end is not None:
            limit = knob.lo if end == "minimum" else knob.hi
            pinned.append(f"{knob.label} = {value:g} at its {end} ({limit:g})")
    return pinned


def validate(args, build_dir, knobs, start_values, best_values, room_ir) -> dict | None:
    """Score the start and the best values on a probe the fit never saw.

    A fit reports its own objective, which it minimised — the one number that
    cannot tell anyone whether the values generalise. Three probe notes are
    enough to pin a physical voice into a configuration that is right at those
    three and wrong a fifth above, and nothing in the fit itself would show it.

    A drum probe has no register to hold notes out of, so the held-out axis is
    velocity instead (`--validate-velocities`): the same instrument struck
    harder and softer than anything the fit scored.

    Costs one oracle resolution plus two model renders, once, at the end.
    """
    percussive = getattr(args, "percussive", False)
    holdout = argparse.Namespace(**vars(args))
    # A fixed WAV is one rendering of one probe, so the hold-out gets its own.
    # `check_holdout_oracle` has already refused the combination without it.
    holdout.oracle_wav = getattr(args, "validate_oracle_wav", "")
    if percussive:
        if not args.validate_velocities:
            return None
        holdout.velocities = args.validate_velocities
        axis, held = "velocities", args.validate_velocities
    else:
        if not args.validate_notes:
            return None
        holdout.notes = args.validate_notes
        axis, held = "notes", args.validate_notes
    print(f"validating on held-out {axis} {held}...", file=sys.stderr)
    oracle_rows, oracle_audio, _ = oracle_reference(holdout)
    if not oracle_rows:
        print(f"  the held-out {axis} produced no analyzable oracle rows — skipped",
              file=sys.stderr)
        return None

    want_audio = args.w_mss > 0.0
    weights = LossWeights(cli_weights(args))

    corpus = resolve_corpus(holdout)

    def score(values: list[float]) -> float:
        rows, audio = render_model_rows_subprocess(
            build_dir, args.program, args.pattern, holdout.notes,
            velocities_csv=holdout.velocities,
            overrides=tunable_overrides(knobs, values),
            want_audio=want_audio, room_ir=room_ir, corpus=corpus,
        )
        mss = mss_distance(audio, oracle_audio) if want_audio and audio is not None else 0.0
        terms = score_terms(rows, oracle_rows, n_harm=args.n_harm, mss=mss,
                            percussive=percussive)
        if weights.scales is None and terms is not None:
            weights.calibrate(terms)
        return weights.combine(terms)

    at_start = score(start_values)  # calibrates, so it scores exactly 1.0
    at_best = score(best_values)
    return {"axis": axis, "held_out": held, "start": at_start, "best": at_best}


# --------------------------------------------------------------------------- #
# Internal render subcommand (runs in the per-eval subprocess)
# --------------------------------------------------------------------------- #
def render_metrics_main(argv: list[str]) -> int:
    """Render the model for one score and print its per-note metrics as JSON.

    Invoked as a subprocess by the optimiser with SONARE_LIB_PATH (and, for a
    runtime-knob fit, SONARE_TUNING_OVERRIDES) already set in the environment.
    """
    p = argparse.ArgumentParser(prog="autofit.py _render_metrics")
    p.add_argument("--program", type=int, required=True)
    p.add_argument("--pattern", required=True)
    p.add_argument("--notes", default="")
    p.add_argument("--velocities", default="")
    p.add_argument("--dump-audio", default="", dest="dump_audio",
                   help="also write the normalized mono render to this .npy path")
    p.add_argument("--room-ir", default="", dest="room_ir",
                   help="convolve the render with this .npy impulse response first")
    p.add_argument("--corpus", default="", help="capture manifest laying out the probe")
    p.add_argument("--corpus-timbre", default="", dest="corpus_timbre")
    a = p.parse_args(argv)

    corpus = load_corpus(a.corpus, a.corpus_timbre) if a.corpus else None
    pattern, total, _ = _score(a.program, a.pattern, a.notes, a.velocities, corpus=corpus)
    smf_bytes = write_smf(
        pattern.notes, program=a.program, channel=pattern.channel, end_pad=pattern.tail
    )
    audio = np.asarray(render_model(smf_bytes, total, SR), dtype=np.float32)
    if a.room_ir:
        # Applied here rather than in the parent so the per-note metrics and the
        # multi-scale term both see the same roomed signal.
        audio = apply_room(audio, np.load(a.room_ir))
    raw = to_mono(audio)
    mono = normalize_rms(raw)
    rows = probe_rows(mono, pattern, SR, raw=raw)
    if a.dump_audio:
        np.save(a.dump_audio, mono.astype(np.float32))
    print(json.dumps(rows))
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def apply_spec_weights(args, argv: list[str]) -> None:
    """Let a spec set the term weights it needs, unless the command line said otherwise.

    Eleven of the `--w-*` flags default to zero, which means a fit run without
    them scores only the harmonic ladder, the intonation and the noise floor —
    every envelope, decay, level and attack term silent. That default is right
    for nothing in particular and has to be overridden per voice, from memory,
    on every run. A spec that carries its own weights makes the run
    reproducible from one file.

    An explicit flag always wins, decided by whether it appears in argv rather
    than by comparing against the default, so passing a flag its own default
    value still counts as having asked for it.
    """
    if args.spec == "auto":
        return
    spec_weights = load_spec_weights(Path(args.spec).resolve())
    if not spec_weights:
        return
    explicit = {a.split("=", 1)[0] for a in argv if a.startswith("--w-")}
    applied = []
    for term, value in spec_weights.items():
        if term not in LOSS_TERMS:
            raise ValueError(
                f"spec {args.spec}: {term!r} is not a loss term "
                f"(they are {', '.join(LOSS_TERMS)})"
            )
        flag = f"--w-{term}"
        if flag in explicit:
            continue
        setattr(args, f"w_{term}", value)
        applied.append(f"{term}={value:g}")
    if applied:
        print(f"spec weights: {' '.join(applied)}", file=sys.stderr)


def run(args, argv: list[str] | None = None) -> int:
    build_dir = (REPO_ROOT / args.build_dir).resolve()
    if build_dir.name == "build-python-shared":
        raise ValueError("refusing to use build-python-shared; pick a private build dir")

    resolve_probe(args)
    apply_spec_weights(args, argv if argv is not None else sys.argv[1:])
    weights = cli_weights(args)
    print(f"probe: pattern {args.pattern!r} on MIDI channel "
          f"{10 if args.percussive else 1}, scored with the "
          f"{'percussion' if args.percussive else 'harmonic'} metric set; weights "
          + " ".join(f"{t}={w:g}" for t, w in weights.items() if w > 0.0),
          file=sys.stderr)

    # A catalogue is needed whenever a spec might name a per-program patch field
    # (which has no declaration in src/ to validate against) and always for
    # --spec auto. Producing it needs a tuning build, so it configures and
    # builds first — the same build the run then uses.
    auto = args.spec == "auto"
    catalogue: Catalogue | None = None
    if auto or args.dump_knobs:
        configure_build(build_dir, args.cmake, tuning=True)
        build_shared(build_dir, args.cmake, args.jobs)
        dylib = dylib_path(build_dir)
        catalogue = dump_catalogue(
            args.program, catalogue_pattern(args), str(dylib) if dylib else None,
            sr=SR, notes=args.notes,
        )
        print(f"catalogue: {len(catalogue.defaults)} knobs across "
              f"{len(catalogue.programs)} programs, {len(catalogue.bounds)} clamp bounds",
              file=sys.stderr)

    if args.dump_knobs:
        key = (drum_patch_key(args.drum_note) if args.drum_note is not None
               else catalogue.programs.get(args.program, ""))
        rows = sorted(
            (k, v) for k, v in catalogue.defaults.items()
            if not args.program_only or (key and k.startswith(key + "."))
        )
        if args.drum_note is not None:
            print(f"# drum note {args.drum_note} is voiced by patch {key!r}")
        else:
            print(f"# program {args.program} is voiced by patch {key!r}")
        print("# key\tdefault\tmin\tmax")
        for k, v in rows:
            bound = catalogue.bound_for(k)
            span = f"\t{bound[0]:g}\t{bound[1]:g}" if bound else "\t-\t-"
            print(f"{k}\t{v:g}{span}")
        return 0

    if auto:
        spec = auto_spec(args.program, catalogue, drum_note=args.drum_note)
        subject = (f"drum note {args.drum_note}" if args.drum_note is not None
                   else f"program {args.program}")
        patch = (drum_patch_key(args.drum_note) if args.drum_note is not None
                 else catalogue.programs.get(args.program))
        print(f"--spec auto: {len(spec)} knobs for {subject} "
              f"(patch {patch!r} + its engine)", file=sys.stderr)
    else:
        spec = load_spec(Path(args.spec).resolve())
        if any("." in e.get("tunable", "") for e in spec):
            configure_build(build_dir, args.cmake, tuning=True)
            build_shared(build_dir, args.cmake, args.jobs)
            dylib = dylib_path(build_dir)
            catalogue = dump_catalogue(
                args.program, catalogue_pattern(args), str(dylib) if dylib else None,
                sr=SR, notes=args.notes,
            )

    pristine: dict[Path, str] = {}
    knobs = build_knobs(spec, pristine, catalogue)

    n_runtime = sum(1 for k in knobs if k.tunable is not None)
    n_source = len(knobs) - n_runtime
    if n_source:
        print(f"{len(knobs)} knobs ({n_runtime} runtime, {n_source} source) — "
              f"a source knob rebuilds the library every evaluation; converting it to "
              f"SONARE_TUNABLE would make this run far cheaper", file=sys.stderr)
    else:
        print(f"{len(knobs)} runtime knobs — building once, then rendering per evaluation",
              file=sys.stderr)

    configure_build(build_dir, args.cmake, tuning=n_runtime > 0)

    print("resolving oracle (once)...", file=sys.stderr)
    corpus = resolve_corpus(args)
    oracle, oracle_audio, room = oracle_reference(args)

    best_values: list[float] = [k.start_value for k in knobs]
    with tempfile.TemporaryDirectory(prefix="autofit_room_") as tmp:
        ir_path: Path | None = None
        if room is not None:
            # Fit the response against a dry model render at the start values,
            # so the roomed model *measures* the oracle's space rather than
            # merely being convolved with a room of the same nominal size. One
            # extra render, once.
            build_shared(build_dir, args.cmake, args.jobs)
            _, dry_model = render_model_rows_subprocess(
                build_dir, args.program, args.pattern, args.notes,
                velocities_csv=args.velocities, want_audio=True, corpus=corpus,
            )
            if dry_model is None:
                raise RuntimeError("room correction needs the model render, which came back empty")
            pattern, _, _ = _score(args.program, args.pattern, args.notes, args.velocities,
                                   corpus=corpus)
            spans = [(n.start, n.start + n.dur) for n in pattern.notes]
            ir = fit_room_ir(dry_model[:, None], SR, spans, room)
            ir_path = Path(tmp) / "room.npy"
            np.save(ir_path, ir)
        evaluator = Evaluator(
            knobs, pristine, oracle, oracle_audio, args, build_dir, room_ir=ir_path,
            corpus=corpus,
        )
        if evaluator.workers > 1:
            print(f"rendering up to {evaluator.workers} candidates concurrently",
                  file=sys.stderr)
        try:
            optimizer = cma_es if args.optimizer == "cmaes" else optimize
            fit_knobs, problem = knobs, evaluator
            if args.screen:
                kept = screen_knobs(evaluator, knobs, args)
                if len(kept) < len(knobs):
                    # The dropped knobs stay in `knobs` at their start values, so
                    # the report and the write-back still cover the whole spec.
                    fit_knobs = [knobs[i] for i in kept]
                    problem = SubEvaluator(evaluator, kept, [k.start_value for k in knobs])
            if args.stages:
                run_stages(problem, fit_knobs, args, optimizer)
            else:
                optimizer(problem, fit_knobs, args)
        finally:
            # Always undo this run's own edits, whatever happened above — and
            # only those, so a file someone else changed meanwhile survives.
            restore(pristine, evaluator.written)

        if evaluator.best_values is not None:
            best_values = evaluator.best_values

        pinned = report_pinned(knobs, best_values)
        if pinned:
            print(f"\n{len(pinned)} knob{'s' if len(pinned) > 1 else ''} ended on a range "
                  f"bound — the search could not go further, so this may not be the optimum:",
                  file=sys.stderr)
            for line in pinned:
                print(f"  {line}", file=sys.stderr)
            print("  Widen the range in the spec and re-run before trusting these values.",
                  file=sys.stderr)

        extra = {
            "room": room.to_dict() if room is not None else None,
            "pinned": pinned,
            "validation": validate(
                args, build_dir, knobs, [k.start_value for k in knobs], best_values, ir_path
            ),
        }
    report_result(knobs, pristine, best_values, evaluator, args, extra)
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "_render_metrics":
        return render_metrics_main(sys.argv[2:])

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--spec", required=True,
                        help="knob spec JSON path, or 'auto' to derive the knob list for "
                             "--program from the library's own catalogue")
    parser.add_argument("--program", type=int, required=True,
                        help="GM program to score, or the drum kit with --drum-note")
    parser.add_argument("--drum-note", type=int, default=None, dest="drum_note",
                        help="fit a percussion instrument instead of a GM program: the "
                             "probe moves to the drum channel, where this note number "
                             "selects the instrument (38 acoustic snare, 36 kick, 46 open "
                             "hi-hat) and --program selects the kit. Scored with the "
                             "percussion metric set, and --spec auto offers that note's "
                             "own patch fields")
    parser.add_argument("--pattern", default="sustain", help="probe pattern (default: sustain, "
                                                             "or 'drum' with --drum-note)")
    parser.add_argument("--notes", default="", help="override probe notes, e.g. '48,60,72'")
    parser.add_argument("--velocities", default="",
                        help="override probe velocities, e.g. '64,100,127' (the 'drum' and "
                             "'velocity' patterns)")
    parser.add_argument("--dump-knobs", action="store_true", dest="dump_knobs",
                        help="list every knob the library reports, with its default, and exit")
    parser.add_argument("--program-only", action="store_true", dest="program_only",
                        help="with --dump-knobs, list only this program's patch fields")
    parser.add_argument("--room", default="auto", choices=("auto", "none"),
                        help="auto (default): measure the oracle's reverberation and place "
                             "every model render in a matching space before scoring, so a "
                             "reference recorded in a hall does not read as timbre; "
                             "none: score as rendered")
    parser.add_argument("--corpus", default="",
                        help="score against a captured single-note corpus: the directory a "
                             "`capture.py corpus` run wrote (or its manifest.json). The probe "
                             "is then the capture's own grid — its notes, its velocities and "
                             "its gate — and the oracle is the captured audio assembled onto "
                             "that timeline, so the fit and the reference profile measure the "
                             "same stimulus. Cuts down with --notes / --velocities")
    parser.add_argument("--corpus-timbre", default="", dest="corpus_timbre",
                        help="which timbre of the corpus to fit against (default: the first "
                             "the manifest lists)")
    add_oracle_args(parser)
    parser.add_argument("--max-evals", type=int, default=30, dest="max_evals",
                        help="total evaluations (default: 30; raise it well past this "
                             "when the spec is runtime knobs only)")
    parser.add_argument("--optimizer", default="coord", choices=("coord", "cmaes"),
                        help="coord: golden-section coordinate descent (default). "
                             "cmaes: covariance-matrix adaptation, which handles knobs "
                             "that trade against each other and renders its population "
                             "concurrently")
    parser.add_argument("--per-knob-evals", type=int, default=6, dest="per_knob_evals",
                        help="golden-section budget per knob per pass (coord only, default: 6)")
    parser.add_argument("--population", type=int, default=0,
                        help="CMA-ES population size (default: 4 + 3*ln(n))")
    parser.add_argument("--sigma0", type=float, default=0.25,
                        help="CMA-ES initial step, as a fraction of each knob's range")
    parser.add_argument("--seed", type=int, default=0, help="CMA-ES sampling seed")
    parser.add_argument("--restarts", type=int, default=0,
                        help="CMA-ES restarts from a fresh random point with a doubled "
                             "population when a run stalls (default: 0). They share "
                             "--max-evals rather than each getting their own")
    parser.add_argument("--workers", type=int, default=1,
                        help="model renders to run concurrently (default: 1). Only helps "
                             "--optimizer cmaes, whose population is independent; a "
                             "rebuilding spec is forced back to 1")
    parser.add_argument("--screen", action="store_true",
                        help="probe each knob at both ends first and drop the ones that do "
                             "not move the loss, then fit only the rest. Costs 2 evaluations "
                             "per knob out of --max-evals; the dropped knobs are named")
    parser.add_argument("--screen-threshold", type=float, default=0.002,
                        dest="screen_threshold",
                        help="smallest loss change over a knob's whole range that counts as "
                             "an effect (default: 0.002, i.e. 0.2%% of the start loss)")
    parser.add_argument("--stages", action="store_true",
                        help="fit the excitation knobs against the onset evidence, then the "
                             "decay knobs against the decay evidence, then everything under "
                             "the weights given here — instead of all of it at once")
    parser.add_argument("--validate-notes", default="", dest="validate_notes",
                        help="score the result on these notes as well (e.g. '43,55,67'). "
                             "They must be disjoint from --notes to mean anything: this is "
                             "the only check that the fit did not overfit the probe")
    parser.add_argument("--validate-velocities", default="", dest="validate_velocities",
                        help="the same check for a drum fit, which has no register to hold "
                             "notes out of: score the result at these velocities as well "
                             "(e.g. '48,88,112'), disjoint from the probe's")
    parser.add_argument("--validate-oracle-wav", default="", dest="validate_oracle_wav",
                        help="the reference for the held-out probe, rendered the same way "
                             "as --oracle-wav. Required with it, because a fixed WAV is one "
                             "rendering of one probe and cannot supply the held-out notes")
    parser.add_argument("--out", default="",
                        help="write the result (knob values, losses, overrides, validation) "
                             "to this JSON path")
    parser.add_argument("--n-harm", type=int, default=10, dest="n_harm",
                        help="harmonics counted in the L1 timbre term (default: 10)")
    parser.add_argument("--w-harm", type=float, default=1.0, dest="w_harm",
                        help="weight on the harmonic-profile L1 term")
    parser.add_argument("--w-cents", type=float, default=0.5, dest="w_cents",
                        help="weight on the intonation (cents) term")
    parser.add_argument("--w-tnr", type=float, default=1.0, dest="w_tnr",
                        help="weight on the noise-floor (TNR shortfall) term")
    parser.add_argument("--w-env", type=float, default=None, dest="w_env",
                        help="weight on the temporal-envelope term (sustain slope / release / "
                             "attack; attack / decay / crest for a drum). Defaults to 0 for a "
                             "pitched voice and 1 for a drum, where it is most of the identity")
    parser.add_argument("--w-band", type=float, default=1.0, dest="w_band",
                        help="drum fits: weight on the 1/3-octave level-profile term, the "
                             "percussion analogue of the harmonic ladder")
    parser.add_argument("--w-bdecay", type=float, default=1.0, dest="w_bdecay",
                        help="drum fits: weight on the per-octave-band decay-slope term")
    parser.add_argument("--w-init", type=float, default=0.0, dest="w_init",
                        help="weight on the per-harmonic ONSET-ladder term (excitation evidence)")
    parser.add_argument("--w-slope", type=float, default=0.0, dest="w_slope",
                        help="weight on the per-harmonic decay-slope term (loop evidence)")
    parser.add_argument("--w-tail", type=float, default=0.0, dest="w_tail",
                        help="weight on the per-harmonic decay slope 2-6 s in, which only has "
                             "frames to fit when the probe holds a note that long. This is "
                             "the aftersound: on a piano it is most of the note, and no "
                             "two-second probe can reach it")
    parser.add_argument("--w-hf", type=float, default=0.0, dest="w_hf",
                        help="weight on the attack's high-band balance, measured in 20 ms "
                             "slices over the first 120 ms. Catches a strike-noise or "
                             "excitation path whose top end is wrong for a few tens of "
                             "milliseconds — a tick, which the ear finds instantly and a "
                             "whole-timeline spectral distance averages away")
    parser.add_argument("--w-level", type=float, default=0.0, dest="w_level",
                        help="weight on the level BALANCE across the probe grid: how loud each "
                             "note is relative to the others, with the grid's own median "
                             "offset removed so an output-gain difference is not fitted. "
                             "Needs a probe with more than one note to mean anything")
    parser.add_argument("--w-crest", type=float, default=0.0, dest="w_crest",
                        help="weight on peak-minus-held-RMS per note. Gain-invariant, and the "
                             "one term that sees a note whose envelope never falls after its "
                             "attack — every other term here is normalised past it")
    parser.add_argument("--w-mss", type=float, default=0.0, dest="w_mss",
                        help="weight on the multi-scale STFT distance over the whole render "
                             "(sees what the per-note metric set does not model)")
    parser.add_argument("--raw-loss", action="store_true", dest="raw_loss",
                        help="weight the terms in their own units instead of normalising "
                             "each to its value at the start point. The weights then mean "
                             "whatever the units make them mean")
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
