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
        --corpus <capture dir> --corpus-timbre grand-227 \
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
1/3-octave level profile and how fast each octave of it dies instead, with
`--w-kit` over the relations between the kit's own families. See
`loss.percussion_terms`. `--validate-velocities` is the held-out check, since
there is no register to hold notes out of.

Loss
----
`--w-harm` / `--w-cents` / `--w-tnr` / `--w-env` / `--w-init` / `--w-slope`
weight the interpretable per-note metrics — `--w-band` / `--w-bdecay` / `--w-env`
for a drum probe — and `--w-mss` adds a multi-scale STFT distance over the whole
render, which sees everything the metric set does not model. See
`loss.loss_terms` and `loss.percussion_terms`.

Eight more exist because a shape metric cannot see them:

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
    --w-lf      the same window's low and mid bands, 20 Hz to 4 kHz, over one
                50 ms window. The register that needs it is the bass, where an
                excitation with no highpass of its own puts the attack's energy
                below where any of it radiates — a note felt and never heard,
                and one that every sustain-window term reads as correct
    --w-stiff   how far each string stretches its partials, in cents at the
                twelfth. Making the ladder track the series is what stopped a
                stiffness error arriving as tens of decibels of fabricated
                harmonic error; this is where the real quantity goes instead
    --w-dyn     how brightness tracks velocity, fitted per pitch. Every other
                term compares one note at a time and averages, so the objective
                is otherwise blind to the trend BETWEEN notes — which is the
                axis a physical model should beat a sampled reference on, since
                a sample library has only as many curves as velocity layers
    --w-kit     the relations inside a drum kit: the tom series, the hi-hat
                trio, the cymbals. Drum fits only, and the counterpart to
                --w-dyn on the other axis — that one reads the trend across
                velocities, this one the shape of a family across its members.
                A kit whose instruments are each individually plausible and
                collectively in the wrong relation is what a listener calls a
                bad kit, and every per-hit term is capped, so once the members
                are far enough out there is no gradient left pointing at the
                repair

**A weight nobody names comes from the instrument's class, not from zero.**
`toneclass.py` answers by what the voice IS — a struck string starts with its
decay and its strike weighted, a modal voice with `modes` in place of `harm` —
so a spec's `weights` block says what the class got wrong rather than the whole
vector, and a term it omits is still weighted:

    { "weights": {"harm": 1, "slope": 1, "tail": 2, "crest": 2, "level": 2},
      "knobs": [ ... ] }

An explicit `--w-*` on the command line still wins, and a run prints what it
resolved to. `specs/piano_corpus.json` is the worked example.

Each term is divided by one perceptual unit of itself — a dB of ladder error, a
cent, a dB/s — so a weight says what that unit is worth against every other
term's, and the same number means the same thing on every voice. A second and
separate layer divides the whole sum by what the start point scored, so the
start reads exactly 1.0 and a reported 0.85 means 15 % better than the
compiled-in values. `--raw-loss` skips both.

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

`--max-evals` counts candidates the search looked at, not renders it paid for.
The two are the same number until a store is warm, and after that the budget has
to stay on the first: a search is defined by where it went, and making it stop
early because the going was cheap would answer a different question than the
same command answered yesterday.

What an evaluation costs
------------------------
Three parts, on a fifteen-note sustain probe: about 0.2 s of process start, half
a second of render, and a third of a second measuring the render — the last of
which is mostly the partial refinement inside `skeleton_note`.

    --metric-threads N  measure N of the probe's notes at once inside one
                    render. The notes are independent, so this is exact; it is
                    also the only concurrency `--optimizer coord` can use, since
                    its line search picks each probe from the previous result.
                    Defaults to three, and to one whenever `--workers` is already
                    spending the machine a level up — the two are alternatives
                    rather than a product.
    --no-cache      re-render candidates an earlier run already measured.
                    Otherwise the raw terms are kept under the scratch root,
                    keyed on the library's bytes, the whole harness source, the
                    probe and the oracle, so a change to any of them starts cold
                    rather than answering from a scorer that no longer exists. A
                    rebuilding spec keeps no store at all: its library is
                    different for every candidate.

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

A run refuses to start with src/ dirty, since whatever is there is what gets
compiled and fit against with nothing in the output saying so — this tree
routinely has several sessions editing src/ at once. `--allow-dirty-src` fits
against it anyway (the normal case when it is your own in-flight engine edit),
and either way `--out` records HEAD's sha and the dirty paths, if any.
"""

from __future__ import annotations

import argparse
import itertools
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

from _repo import REPO_ROOT

# The split modules below hold what this file used to define inline. Every
# importer reads this module by name and the tests patch attributes on it, so
# the whole surface is re-exported here.
# ruff: noqa: F401
from autofit_report import report_pinned, winner_or_defaults
from autofit_resolve import (
    _score,
    apply_spec_weights,
    catalogue_pattern,
    check_holdout_oracle,
    reference_band_edge,
    resolve_corpus,
    resolve_probe,
)
from build_lib import build_shared, configure_build, dylib_path
from capture import CORPUS_ROOT, ROOM_NONE, model_rig
from catalogue import Catalogue, drum_patch_key, dump_catalogue
from corpus import (
    PERCUSSION_CHANNEL as CORPUS_PERCUSSION_CHANNEL,
)
from corpus import (
    Corpus,
    check_rig,
    corpus_oracle,
    corpus_pattern,
    describe,
    load_corpus,
)
from diagnose import run_diagnosis
from eval_cache import digest, file_digest, open_cache, source_digest
from knobs import (
    at_bound,
    auto_spec,
    build_knobs,
    format_value,
    load_spec,
    load_spec_weights,
    tunable_overrides,
)
from loss import (
    HARM_REACH,
    KIT_MIN_MEMBERS,
    LOSS_TERMS,
    SUSTAIN_SLOPE_CAP_DB_S,
    LossWeights,
    cli_weights,
    dropped_weights,
    mss_distance,
    probe_rows,
    refused_weights,
    score_terms,
)
from metrics import (
    MONO_MODES,
    channel_correlation,
    normalize_rms,
    to_mono,
)
from optimizers import cma_es, optimize
from patterns import DRUM_GATE_HELP, build_pattern, pattern_length
from render_model import render_model
from render_oracle import (
    add_oracle_args,
    check_oracle_rig,
    obtain_oracle,
    oracle_may_carry_room,
)
from report import report_result
from room import apply_room, estimate_room, fit_room_ir
from smf import write_smf
from staging import SubEvaluator, run_stages, screen_knobs
from writeback import materialize, restore, write_edits

SR = 48000


def render_rate(corpus) -> int:
    """The rate this run renders at: the corpus's own, or the default without one.

    `corpus_oracle` refuses a corpus whose rate differs from the render's rather
    than resampling a reference, so a run against a capture recorded at anything
    but 48 kHz cannot start until the model moves to meet it. Every capture with
    `source_class: module` is at 44.1 kHz, so before this none of them could be
    fitted against at all.
    """
    return int(corpus.sample_rate) if corpus is not None else SR


#: Under this much inter-channel correlation, summing a stereo reference to mono
#: comb-filters it enough to matter — a spaced close pair on a piano runs around
#: 0.5 through the midrange. Reported rather than acted on; see `--mono-mode`.
STEREO_COMB_CORRELATION = 0.8

# What a decibel of level drift past the allowance costs, on a loss the start
# point scores 1.0. Steep on purpose: the drift the fence exists to stop was
# 10 to 31 dB, and no shape a fit can buy at that price is worth keeping.
LEVEL_DRIFT_PENALTY_PER_DB = 0.1

# What a dB/s of extra fall past the allowance costs, on the same 1.0 start.
# Matched to the level fence because it stops the same class of trade: a shape
# bought by taking the note away rather than by voicing it.
SUSTAIN_DRIFT_PENALTY_PER_DB_S = 0.1

# Both rates above are per unit of a loss the start point scores 1.0, so both
# are multiplied by what one such unit is worth in the units the run reports.
# The floor is there for a start point that scores zero, which no fence should
# be able to divide the whole run by.
FENCE_UNIT_FLOOR = 1e-6


def sustain_excess_db_s(model_rows: list[dict], oracle_rows: list[dict]) -> tuple[float, int]:
    """How much faster than its reference the model's worst note falls away.

    Negative is the model falling faster. Paired on (note, velocity) rather than
    by position, because a probe whose notes were reordered would otherwise
    subtract one register from another.

    Returns the pair count too: a probe that carries no comparable note scores
    0.0 here, which is this quantity's best value, and a fence cannot tell that
    from a voice that holds.
    """
    ref = {(r.get("note"), r.get("velocity")): r.get("sustain_slope_db_s") for r in oracle_rows}
    deltas = []
    for row in model_rows:
        theirs = ref.get((row.get("note"), row.get("velocity")))
        if theirs is None:
            continue  # the reference holds nothing to fall from
        # Two absences arrive as the same None and mean opposite things. No key
        # is the probe's shape — this row never carried a sustain slope — and
        # charging for that would charge a voice for how it was measured. A key
        # holding None is `analyze_note` saying the window sat on the dB clamp,
        # which is the note being gone: skip that and the worst case this fence
        # exists for reads as absent, and a render whose every note died scores
        # BETTER than one holding exactly like its reference.
        if "sustain_slope_db_s" not in row:
            continue
        mine = row["sustain_slope_db_s"]
        deltas.append(-SUSTAIN_SLOPE_CAP_DB_S if mine is None else float(mine) - float(theirs))
    # Not clamped at zero: a voice holding BETTER than its reference reads
    # positive here, and the fence subtracting one reading from another needs
    # the true value on both sides or it would charge the difference between a
    # clamp and a measurement.
    return (min(deltas) if deltas else 0.0), len(deltas)


# --------------------------------------------------------------------------- #
# The probe: what is rendered, and what comes back measured
# --------------------------------------------------------------------------- #
def oracle_reference(args) -> tuple[list[dict], np.ndarray, np.ndarray | None, float | None]:
    """Resolve the oracle once: per-note metrics, mono render, its room, its band edge.

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

    The fourth value is the highest 1/3-octave band this reference can actually
    measure, for a percussion probe (`reference_band_edge`), or None when it
    carries the whole analysis range. It is resolved HERE and then handed to
    every model render of the run: a bandwidth is a property of the reference,
    and if the two sides derived it separately the model would normalise its
    band profile against a different set of bands than the reference did and
    every band reading would move.
    """
    corpus = resolve_corpus(args)
    sr = render_rate(corpus)
    pattern, total, _ = _score(
        args.program,
        args.pattern,
        args.notes,
        getattr(args, "velocities", ""),
        corpus=corpus,
        gate_ms=getattr(args, "drum_gate_ms", 0),
    )
    if corpus is not None:
        # Assembled from the capture rather than played: the reference for a
        # corpus run already exists as audio, one file per slot, and nothing
        # about it depends on a plugin still being installed.
        audio = corpus_oracle(corpus, pattern, sr)
    else:
        smf_bytes = write_smf(
            pattern.notes,
            program=args.program,
            bank=getattr(args, "bank", 0),
            channel=pattern.channel,
            end_pad=pattern.tail,
        )
        audio = obtain_oracle(args, smf_bytes, total, sr, [n.start for n in pattern.notes])

    # Only an oracle rendered outside libsonare's dry path can carry a room —
    # a supplied WAV, or an AudioUnit that was not asked to switch its effects
    # off. The built-in fluidsynth oracle is rendered with its reverb and chorus
    # units switched off, so any decay measured there is the instrument's own,
    # and estimating a room from it would invent one and then convolve the model
    # with it.
    room = None
    # `dry` and `room` are separate assertions and a corpus needs neither to be
    # true to have no space in it: a plugin with no effect section answers
    # `dry: false` for want of anything to switch off, and 57 of the captures
    # here do. Without the second half of this, every one of them had a room
    # estimated from its own note tails and convolved onto the model.
    may_carry_room = (
        (not corpus.dry and corpus.room != ROOM_NONE)
        if corpus is not None
        else oracle_may_carry_room(args)
    )
    if getattr(args, "room", "auto") != "none" and may_carry_room:
        measured = estimate_room(audio, sr, [(n.start, n.start + n.dur) for n in pattern.notes])
        if measured.is_dry():
            print(
                f"oracle room: dry (RT60 {measured.rt60_s:.2f}s) — no room correction",
                file=sys.stderr,
            )
        elif measured.gated():
            print(
                f"oracle room: RT60 {measured.rt60_s:.2f}s, "
                f"tail level {measured.tail_db:+.1f}dB — NOT corrected. The probe holds each "
                f"note for {measured.note_window_s * 1000:.0f} ms against that decay, so the "
                f"tail level measures the gate rather than the room and a correction fitted to "
                f"it invents one. The reference's space stays in every decay term of the "
                f"objective; do not read a fitted decay as the instrument's.",
                file=sys.stderr,
            )
        else:
            print(
                f"oracle room: RT60 {measured.rt60_s:.2f}s, "
                f"tail level {measured.tail_db:+.1f}dB, HF ratio {measured.hf_ratio:.2f} — "
                f"the model is placed in a matching space before every measurement",
                file=sys.stderr,
            )
            if measured.truncated():
                print(
                    f"  note: the probe's shortest silence is {measured.tail_window_s:.1f}s, "
                    f"less than the {measured.rt60_s * 25.0 / 60.0:.1f}s this decay needs to "
                    f"fall 25 dB — the RT60 is likely underestimated. Re-export the probe "
                    f"with --pattern room-probe to measure it properly.",
                    file=sys.stderr,
                )
            room = measured

    # The model renders mono, so the reduction only ever matters on the oracle
    # side — where it matters a great deal: summing a spaced close pair comb-
    # filters whatever is decorrelated between the channels, and a notch that
    # lands on a partial reads as harmonic error the model is then asked to
    # reproduce. The correlation is reported so the risk is visible; the
    # reduction stays a sum by default because every committed profile in
    # `reference/` was measured through one.
    mode = getattr(args, "mono_mode", "mean")
    corr = channel_correlation(audio)
    if mode == "mean" and corr is not None and corr < STEREO_COMB_CORRELATION:
        print(
            f"  note: the oracle's channels correlate at {corr:+.2f}, so summing "
            f"them comb-filters what differs between them. --mono-mode left "
            f"takes one channel and has no sum in it.",
            file=sys.stderr,
        )
    raw = to_mono(audio, mode)
    mono = normalize_rms(raw)
    threads = resolve_metric_threads(args)
    rows = probe_rows(mono, pattern, sr, raw=raw, threads=threads)
    edge = reference_band_edge(corpus, rows) if pattern.percussive else None
    if edge is not None:
        # Re-measured rather than patched. The band profile is normalised to the
        # loudest band inside the edge, and that is not a scaling that can be
        # applied to an already-floored profile without inventing the values the
        # floor took away.
        rows = probe_rows(mono, pattern, sr, raw=raw, max_band_hz=edge, threads=threads)
        print(
            f"reference bandwidth: {edge / 1000.0:.1f} kHz — bands above it are "
            f"the capture chain rather than the kit, and are excluded from the "
            f"band profile on BOTH sides",
            file=sys.stderr,
        )
    return rows, mono, room, edge


#: What `--metric-threads auto` resolves to. Measuring a note is mostly the
#: partial refinement in `skeleton_note`, which spends its time inside numpy
#: with the interpreter lock dropped; past three the lock is what is left and
#: the curve flattens (measured 1.6x at two, 2.1x at three, 2.1x at four). It is
#: also the ceiling this machine is meant to keep concurrent work under.
AUTO_METRIC_THREADS = 3


def _corpus_identity(corpus: Corpus | None) -> list:
    """What a corpus contributes to a model render, as a comparable value.

    The timeline and nothing else — the notes played, their velocities, the
    window each slot gets, and the three classifications that decide how the
    render is treated. The captured audio is absent on purpose: the model is
    rendered against the manifest and the reference it is compared with is
    hashed separately, as the oracle rows it was already reduced to.
    """
    if corpus is None:
        return []
    return [
        corpus.timbre,
        corpus.sample_rate,
        corpus.gate_s,
        corpus.preroll_s,
        corpus.slot_s,
        corpus.channel,
        corpus.dry,
        corpus.room,
        corpus.rig,
        list(corpus.notes),
        list(corpus.velocities),
        sorted((list(k), v) for k, v in corpus.slots.items()),
        sorted(corpus.note_map.items()),
    ]


def resolve_metric_threads(args) -> int:
    """How many of a probe's notes one render may measure at once.

    Zero means auto. A run that already renders candidates concurrently is
    handled by the caller, not here: this answers what one render may use, and
    `Evaluator` is what knows whether that render has siblings.
    """
    asked = getattr(args, "metric_threads", 0)
    if asked and asked > 0:
        return asked
    return AUTO_METRIC_THREADS


def render_model_rows_subprocess(
    build_dir: Path,
    program: int,
    pattern_name: str,
    notes_csv: str,
    *,
    velocities_csv: str = "",
    overrides: str = "",
    want_audio: bool = False,
    room_ir: Path | None = None,
    corpus: Corpus | None = None,
    gate_ms: int = 0,
    bank: int = 0,
    band_edge_hz: float | None = None,
    metric_threads: int = 1,
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

    cmd = [
        sys.executable,
        str(Path(__file__).resolve()),
        "_render_metrics",
        "--program",
        str(program),
        "--pattern",
        pattern_name,
        "--notes",
        notes_csv,
        "--velocities",
        velocities_csv,
    ]
    if bank:
        cmd += ["--bank", str(bank)]
    if metric_threads > 1:
        cmd += ["--metric-threads", str(metric_threads)]
    if gate_ms:
        cmd += ["--drum-gate-ms", str(gate_ms)]
    if band_edge_hz:
        cmd += ["--band-edge-hz", str(band_edge_hz)]
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
        proc = subprocess.run(cmd, capture_output=True, check=False, text=True, env=env)
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

    def __init__(
        self,
        knobs,
        pristine,
        oracle,
        oracle_audio,
        args,
        build_dir,
        room_ir=None,
        corpus=None,
        band_edge_hz=None,
    ):
        self.knobs = knobs
        # Resolved from the oracle once — see `oracle_reference`. Both sides
        # have to normalise their band profile over the same set of bands.
        self.band_edge_hz = band_edge_hz
        self.pristine = pristine
        self.oracle = oracle
        self.oracle_audio = oracle_audio
        self.args = args
        self.build_dir = build_dir
        self.room_ir = room_ir
        self.corpus = corpus
        # The capture's own families of notes, for the kit-relation term. Read
        # off the corpus rather than the pattern, because a family is a fact
        # about the reference and a probe that names a subset of its notes still
        # scores the relations among the ones it kept.
        self.groups = dict(getattr(corpus, "groups", None) or {})
        # Whether a render has to hand its audio back, which only the multi-scale
        # term needs. Read from the RESOLVED weight rather than from `--w-mss`:
        # every `--w-*` flag defaults to None so that "not given" stays
        # distinguishable from "given as zero", and the number it stands for
        # comes from the instrument's own class.
        self.want_audio = cli_weights(args).get("mss", 0.0) > 0.0
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
        # Renders, as against evaluations: `trajectory` counts the candidates the
        # search looked at and this counts the ones it had to pay for.
        self.n_renders = 0
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
        # Where the model's whole-grid level sat at the start point and where
        # the winner left it. The difference is what no loss term charges for.
        self.start_level_offset_db: float | None = None
        self.start_sustain_excess_db_s: float | None = None
        self.sustain_fence_bit = False
        self._anchored = False
        self.fence_unit = 1.0
        self.best_level_offset_db: float | None = None
        # How many notes the one-sided noise term still had something to say
        # about, at the start and at the winner. `loss.py` charges `tnr` only
        # where the model is noisier, so a candidate that walks the model past
        # the reference collects the term's whole unit and reports zero.
        self.start_tnr_notes: float | None = None
        self.best_tnr_notes: float | None = None
        # A rebuild rewrites the shared tree, so its evaluations can only ever
        # run one at a time however many workers were asked for.
        self.workers = 1 if self.needs_rebuild else max(1, args.workers)
        # Concurrency inside a render, which is only free while there is none
        # around it: a batch already has every core busy on whole candidates,
        # and threads under that compete for the same interpreter lock without
        # anything left to win. So the two are alternatives rather than a
        # product, which also keeps the run's total thread count off the number
        # of notes the probe happens to have.
        self.metric_threads = 1 if self.workers > 1 else resolve_metric_threads(args)
        self.quiet = False
        self._offset_reported = False
        # Which candidates already have a `trajectory` entry, so that a repeat —
        # a stage re-reading the point it inherited — is scored again without
        # being counted again.
        self.seen: set[tuple[str, ...]] = set()
        # Opened on the first evaluation rather than here, because the signature
        # is taken from the library's own bytes and the build that produces them
        # is itself deferred to the first evaluation.
        self.disk = open_cache(CORPUS_ROOT, "", enabled=False)
        self._cache_opened = False

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

    def cache_signature(self) -> str:
        """Everything a stored term value depends on, folded into one name.

        The library's bytes, the harness's own source, the probe's layout, and
        the oracle the terms are a mismatch against. A run whose signature
        matches another's may read its values; anything else starts cold. The
        knob LABELS are in it and their values are not — the values are the key
        within the file, and two specs over different knobs must not share one.
        """
        dylib = dylib_path(self.build_dir)
        oracle = json.dumps(self.oracle, sort_keys=True, default=str).encode()
        audio = self.oracle_audio.tobytes() if self.oracle_audio is not None else b""
        spec = json.dumps(
            {
                "lib": file_digest(dylib) if dylib is not None else "absent",
                "code": source_digest(Path(__file__).resolve().parent),
                "program": getattr(self.args, "program", 0),
                "bank": getattr(self.args, "bank", 0),
                "pattern": self.args.pattern,
                "notes": self.args.notes,
                "velocities": self.args.velocities,
                "gate_ms": getattr(self.args, "drum_gate_ms", 0),
                "band_edge_hz": self.band_edge_hz,
                "room_ir": file_digest(self.room_ir) if self.room_ir else "",
                # The corpus by what it lays out rather than by where it lives: the
                # model render never touches the captured audio, it plays the
                # timeline the manifest describes, and that manifest is an editable
                # file at a path that does not change when its grid or its gate
                # does. Keying on the path alone would answer an edited capture with
                # the old capture's measurements.
                "corpus": _corpus_identity(self.corpus),
                "n_harm": getattr(self.args, "n_harm", 0),
                "percussive": self.percussive,
                "flat": bool(getattr(self.args, "flat_partial_weighting", False)),
                "want_audio": self.want_audio,
                "groups": sorted((k, sorted(v)) for k, v in self.groups.items()),
                "knobs": [k.label for k in self.knobs],
            },
            sort_keys=True,
        ).encode()
        return digest(spec, oracle, audio)

    def _ensure_cache(self) -> None:
        """Open the store for this run's signature and adopt what it holds."""
        if self._cache_opened:
            return
        self._cache_opened = True
        if self.needs_rebuild or getattr(self.args, "no_cache", False):
            # A rebuilding fit compiles a different library for every candidate,
            # so its signature changes underneath each one and no key it could
            # write would ever be read back.
            return
        self.disk = open_cache(CORPUS_ROOT, self.cache_signature())
        self.cache.update(self.disk.entries)
        if self.disk.dropped:
            print(
                f"cache: {self.disk.path.name} had grown past its size limit and was started over",
                file=sys.stderr,
            )
        if self.disk.loaded:
            print(
                f"cache: {self.disk.loaded} candidates already measured ({self.disk.path})",
                file=sys.stderr,
            )

    def _ensure_built(self) -> None:
        if not self.built and not self.needs_rebuild:
            build_shared(self.build_dir, self.args.cmake, self.args.jobs)
            self.n_builds += 1
            self.built = True
        self._ensure_cache()

    def check_overrides_reach(self, knobs: list) -> None:
        """Prove the runtime overrides actually reach the library, before fitting.

        `render_model_rows_subprocess` puts `SONARE_TUNING_OVERRIDES` in the
        child's environment and takes it on faith from there. A library built
        without `BUILD_TUNING` ignores the variable entirely, and so does a run
        that loaded a different dylib than the one just built — in both cases
        every candidate renders the compiled-in defaults, every evaluation
        returns the same loss, and the fit reports the start point as the winner
        of a search it never actually ran. Nothing about that reads as a failure.

        `dump_catalogue` already refuses this exact configuration on its own
        path; this is the same check on the path that renders.

        The whole spec is pushed to the far end of every knob's range at once,
        not one knob at a time. A single knob can be genuinely inert, which is a
        finding about that knob; the entire spec moving nothing is a finding
        about the plumbing, and telling the two apart is the point.

        That first render is silent whenever any one knob's far end silences the
        voice, and several always do on a short probe: `amp_env.delay_ms` reaches
        5 s and the attacks 20 s, both longer than a plucked instrument's whole
        gate. Silence is not evidence about the plumbing either way, so the check
        falls back to pushing knobs one at a time and needs only one of them to
        move a measurement. The all-at-once render stays the fast path because it
        usually answers in two renders instead of one per knob.
        """
        n_runtime = sum(1 for k in knobs if k.tunable is not None)
        if not n_runtime:
            return

        def far(k):
            if k.tunable is None:
                return k.start_value
            return k.hi if abs(k.hi - k.start_value) >= abs(k.start_value - k.lo) else k.lo

        start = [k.start_value for k in knobs]
        base = self._render_terms(start)
        if base is None:
            raise RuntimeError(
                "the override reach check could not score a render at the spec's "
                "own start values — the probe measures nothing on this voice as it "
                "ships, so there is no baseline for a fit to improve on. This is a "
                "probe or capture problem rather than an override one."
            )
        moved = self._render_terms([far(k) for k in knobs])
        how = "to the far end of every range at once"
        if moved is None:
            how = "to the far end of one range at a time"
            for i, k in enumerate(knobs):
                if k.tunable is None:
                    continue
                one = list(start)
                one[i] = far(k)
                alone = self._render_terms(one)
                if alone is not None and any(abs(base[t] - alone[t]) >= 1e-12 for t in LOSS_TERMS):
                    return
            moved = base
        if all(abs(base[t] - moved[t]) < 1e-12 for t in LOSS_TERMS):
            raise RuntimeError(
                f"{n_runtime} runtime knobs were pushed {how} and not one "
                f"measurement moved. The overrides are "
                f"not reaching the library: either it was built without "
                f"BUILD_TUNING, or the dylib being loaded is not the one that was "
                f"just built (check SONARE_LIB_PATH against {self.build_dir}). "
                f"A fit run in this state searches nothing and reports the start "
                f"point as its winner."
            )

    def _render_terms(self, values: list[float]) -> dict[str, float] | None:
        """Render one candidate and reduce it to raw loss terms. Thread-safe."""
        want_audio = self.want_audio
        model_rows, model_audio = render_model_rows_subprocess(
            self.build_dir,
            self.args.program,
            self.args.pattern,
            self.args.notes,
            velocities_csv=self.args.velocities,
            overrides=tunable_overrides(self.knobs, values),
            want_audio=want_audio,
            room_ir=self.room_ir,
            corpus=self.corpus,
            gate_ms=getattr(self.args, "drum_gate_ms", 0),
            bank=getattr(self.args, "bank", 0),
            band_edge_hz=self.band_edge_hz,
            metric_threads=self.metric_threads,
        )
        mss = 0.0
        if want_audio and model_audio is not None:
            mss = mss_distance(model_audio, self.oracle_audio)
        terms = score_terms(
            model_rows,
            self.oracle,
            n_harm=self.args.n_harm,
            mss=mss,
            percussive=self.percussive,
            groups=self.groups,
            audibility=not getattr(self.args, "flat_partial_weighting", False),
        )
        if terms is not None:
            worst, pairs = sustain_excess_db_s(model_rows, self.oracle)
            terms["sustain_excess_db_s"] = worst
            terms["sustain_excess_pairs"] = float(pairs)
        return terms

    def _score_cached(self, values: list[float], terms: dict[str, float] | None) -> float:
        """Score a candidate whose measurement was already in hand.

        A cache hit still decides things. Every stage begins by re-scoring the
        point it inherited, which is by construction already measured, so a
        stage whose samples never beat its own start point would otherwise end
        with `best_loss` still at infinity and hand back the first finite loss
        it happened to see — a candidate worse than the one it was given, on its
        way into the source.
        """
        return self._record(values, terms, rendered=False)

    def _level_drift_penalty(self, terms: dict[str, float] | None) -> float:
        """What a candidate pays for moving the voice's whole-grid level.

        Nothing else charges for it. Every term is either normalised by the
        note's own level or measured around the grid's median offset — on
        purpose, because an output gain is not a property of the instrument and
        none of these knobs should be spent on one. The cost is that the offset
        is free, and a candidate that quietens the voice into a better-looking
        spectrum wins on the objective as written.

        Measured on the GM kit's hi-hats: two of three fits took that route, one
        of them 31 dB down with a band profile bit-identical to the same values
        at the original gain. So this is a fence and not a term — inside the
        allowance it is exactly zero, so a fit that stays put scores what it
        always did, and outside it rises fast enough to be worth more than any
        shape the level could have bought.
        """
        limit = float(getattr(self.args, "max_level_drift_db", 0.0) or 0.0)
        if limit <= 0.0 or terms is None or self.start_level_offset_db is None:
            return 0.0
        offset = terms.get("level_offset_db")
        if offset is None:
            return 0.0
        excess = abs(offset - self.start_level_offset_db) - limit
        return LEVEL_DRIFT_PENALTY_PER_DB * excess * self.fence_unit if excess > 0.0 else 0.0

    def _report_sustain_excess(self, terms: dict[str, float]) -> None:
        """Name where the start point sits against its reference's own fall, once.

        Printed whether or not the fence is armed, and printed with its pair
        count, because zero reads the same as a voice that holds.
        """
        if self.quiet:
            return
        here, pairs = terms.get("sustain_excess_db_s"), int(terms.get("sustain_excess_pairs", 0))
        if here is None:
            return
        if not pairs:
            print(
                "  sustain: no note of this probe is comparable, so the fence is inert",
                file=sys.stderr,
            )
            return
        print(
            f"  sustain: start falls {float(here):+.2f} dB/s against its reference's own "
            f"slope, worst of {pairs} notes",
            file=sys.stderr,
        )

    def _sustain_drift_penalty(self, terms: dict[str, float] | None) -> float:
        """What a candidate pays for letting the note fall away faster than it did.

        Nothing else stops it. `env` does carry the sustain slope, but every
        normalised term divides by its own value at the start point, so a voice
        that already falls steeply has a flat objective in exactly the dimension
        it is worst in — and four shipped reeds ride that all the way down, one
        of them to digital zero before the key lifts, while their compare tables
        read normally because every other column is a ratio the signal keeps
        producing on its way to the noise floor.

        Anchored on the START point rather than on the reference, and one-sided.
        Repairing a voice that cannot oscillate is a mechanism change, not a
        knob, so demanding it here would only spend the fit's budget on a
        dimension it cannot reach. What this stops is the fit taking a note
        AWAY: inside the allowance the score is exactly what it always was.
        """
        limit = float(getattr(self.args, "max_sustain_drift_db_s", 0.0) or 0.0)
        if limit <= 0.0 or terms is None or self.start_sustain_excess_db_s is None:
            return 0.0
        here = terms.get("sustain_excess_db_s")
        if here is None:
            return 0.0
        excess = self.start_sustain_excess_db_s - float(here) - limit
        return SUSTAIN_DRIFT_PENALTY_PER_DB_S * excess * self.fence_unit if excess > 0.0 else 0.0

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
            print(
                f"level: the model's held RMS runs {offset:+.1f} dB against the reference "
                f"across the whole grid. The level term scores the spread around that "
                f"offset, not the offset itself.",
                file=sys.stderr,
            )

    def _calibrate_once(self, terms: dict[str, float] | None) -> None:
        """Make the first point scored this run the one every loss is relative to.

        Whether that point was rendered or read back from the store. Every term
        is divided by its value here so that a weight means the same thing across
        terms whose raw units differ by orders of magnitude — so a run that skips
        this is not a faster run, it is a run minimising a different quantity.
        Both scoring paths therefore pass through it, and the search calls the
        start point first in either optimiser.
        """
        if terms is not None:
            self._report_level_offset(terms)
        if self.normalize and self.loss.scales is None and terms is not None:
            self.baseline_terms = dict(terms)
            self.loss.calibrate(terms)
        # The fences anchor here too, and OUTSIDE the branch above: they are
        # about where the start point sat, which is a fact about the voice and
        # not about whether the terms are being normalised. Inside it, a
        # --raw-loss run left both anchors at None and every fence silently off
        # — a guard that holds on the default path and lets go on the other one,
        # which is the shape that gets found by walking into it.
        if not self._anchored and terms is not None:
            self._anchored = True
            self.start_level_offset_db = terms.get("level_offset_db")
            self.start_sustain_excess_db_s = terms.get("sustain_excess_db_s")
            self.start_tnr_notes = terms.get("tnr_notes")
            # What one unit of loss is worth in the units this run reports. The
            # fence rates are per dB on a loss the start scores 1.0, so on a raw
            # run — where the start scores its own weighted sum, two orders of
            # magnitude up — the same rate would be a rounding error.
            self.fence_unit = max(self.loss.combine(terms), FENCE_UNIT_FLOOR)
            self._report_sustain_excess(terms)

    def _record(
        self, values: list[float], terms: dict[str, float] | None, *, rendered: bool = True
    ) -> float:
        """Score a candidate, update the best, and log it if it is a new one.

        `trajectory` gets one entry per DISTINCT candidate scored, which is what
        `--max-evals` budgets and what the report counts — not one per render.
        The two are the same number until a store is warm, and the search has to
        be the one defined by where it went rather than by what it had to pay.
        A repeat is silent: a stage re-scoring the point it inherited is not a
        thirteenth evaluation, it is the twelfth read under new weights.
        """
        key = self.key(values)
        fresh = key not in self.seen
        self.seen.add(key)
        if rendered:
            self.n_renders += 1
        self._calibrate_once(terms)
        sustain_charge = self._sustain_drift_penalty(terms)
        if sustain_charge > 0.0 and not self.sustain_fence_bit:
            # Said once, where it happens. A fence nobody ever sees fire reads
            # the same as a fence that cannot: this is the line that separates
            # "the search never offered a note worth taking away" from "the
            # fence is inert", and a campaign over the bank is the population
            # that answers it.
            self.sustain_fence_bit = True
            if not self.quiet:
                print(
                    f"  sustain: fence first charged here — this candidate falls "
                    f"{float(terms['sustain_excess_db_s']):+.2f} dB/s against its "
                    f"reference, the start point fell "
                    f"{float(self.start_sustain_excess_db_s):+.2f}",
                    file=sys.stderr,
                )
        loss = self.loss.combine(terms) + self._level_drift_penalty(terms) + sustain_charge
        if loss < self.best_loss:
            self.best_loss = loss
            self.best_values = list(values)
            # Kept alongside the best values because nothing else can recover
            # it afterwards, and because it is the one number a fit can move
            # freely: the level term scores the spread around the grid's median
            # offset, so a candidate that bought its shape by making the voice
            # quieter scores exactly as if it had not.
            self.best_level_offset_db = None if terms is None else terms.get("level_offset_db")
            self.best_tnr_notes = None if terms is None else terms.get("tnr_notes")
        if not fresh:
            return loss
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
                f"  eval #{len(self.trajectory)} "
                f"{'builds=' + str(self.n_builds) if rendered else 'stored'} "
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
            parts.append(f"{name}={value / scales[name]:.2f}" if scales else f"{name}={value:.3g}")
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
            write_edits(materialize(self.knobs, values, self.pristine, full=True), self.written)
            build_shared(self.build_dir, self.args.cmake, self.args.jobs)
            self.n_builds += 1
        else:
            # The store is named after the library's bytes, so it cannot be
            # opened until the build that produces them has run — which makes
            # the first evaluation of a warm run miss here and hit below.
            self._ensure_built()
            if key in self.cache:
                return self._score_cached(values, self.cache[key])
        terms = self._render_terms(values)
        self.cache[key] = terms
        self.disk.put(key, terms)
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
                    self.disk.put(key, self.cache[key])
                    results.append(self._record(values, self.cache[key]))
                else:
                    results.append(self._score_cached(values, self.cache[key]))
        return results


def holdout_scorer(args, build_dir, knobs, room_ir):
    """A callable that scores one knob vector on the held-out probe, or None.

    Split out of `validate` because the hold-out is not only an end-of-run
    verdict. A single winner scored on it says whether that point generalises
    and nothing about whether it is a peak or a plateau, and a coarse search's
    best point can be a fit-set artefact: on a closed hi-hat, the best point of
    a coarse grid read -10.3 % on the fit and -0.8 % on the hold-out, while
    re-cutting the same interval finer found -22 / -19 sitting between that
    grid's teeth. Both readings need the same scorer.

    Returns `(score, axis, held)`, or None when no hold-out was asked for or the
    held-out probe produced nothing measurable.
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
    print(f"resolving held-out {axis} {held}...", file=sys.stderr)
    oracle_rows, oracle_audio, _, band_edge = oracle_reference(holdout)
    if not oracle_rows:
        print(
            f"  the held-out {axis} produced no analyzable oracle rows — skipped", file=sys.stderr
        )
        return None

    resolved = cli_weights(args)
    weights = LossWeights(resolved)
    # From the resolved weight, not from the flag: `--w-mss` defaults to None so
    # that an unset weight and an explicit zero stay distinguishable.
    want_audio = resolved.get("mss", 0.0) > 0.0
    corpus = resolve_corpus(holdout)

    def score(values: list[float]) -> float:
        rows, audio = render_model_rows_subprocess(
            build_dir,
            args.program,
            args.pattern,
            holdout.notes,
            velocities_csv=holdout.velocities,
            overrides=tunable_overrides(knobs, values),
            want_audio=want_audio,
            room_ir=room_ir,
            corpus=corpus,
            gate_ms=getattr(holdout, "drum_gate_ms", 0),
            bank=getattr(args, "bank", 0),
            band_edge_hz=band_edge,
            metric_threads=resolve_metric_threads(args),
        )
        mss = mss_distance(audio, oracle_audio) if want_audio and audio is not None else 0.0
        terms = score_terms(
            rows,
            oracle_rows,
            n_harm=args.n_harm,
            mss=mss,
            percussive=percussive,
            groups=dict(getattr(corpus, "groups", None) or {}),
            audibility=not getattr(args, "flat_partial_weighting", False),
        )
        if weights.scales is None and terms is not None:
            weights.calibrate(terms)
        return weights.combine(terms)

    return score, axis, held


#: Most points a grid may enumerate. Each one is a model render on the fit probe
#: and, when a hold-out was asked for, a second on that — so this is a few
#: hundred renders, which is the scale at which looking at the surface is still
#: cheaper than the search it is replacing.
GRID_MAX_POINTS = 200


def run_grid(evaluator, knobs, args, build_dir, room_ir) -> int:
    """Enumerate the product of every knob in the spec and print the surface.

    Not a replacement for an optimiser and not a stage of one — this is what to
    run BEFORE trusting a search over two or three knobs that interact. The fit
    and the hold-out are printed for every point, so a winner that is a spike on
    the fit set and flat on the hold-out is visible as such instead of arriving
    as one number at the end.
    """
    if len(knobs) > 3:
        print(
            f"--grid enumerates a product, and {len(knobs)} knobs is not a surface "
            f"anyone can read. Narrow the spec to at most three.",
            file=sys.stderr,
        )
        return 2
    points = max(2, args.grid)
    total = points ** len(knobs)
    if total > GRID_MAX_POINTS:
        print(
            f"--grid {points} over {len(knobs)} knobs is {total} points, past the "
            f"{GRID_MAX_POINTS} this will enumerate. Use fewer points or fewer knobs.",
            file=sys.stderr,
        )
        return 2

    axes = [np.linspace(k.lo, k.hi, points).tolist() for k in knobs]
    combos = [list(c) for c in itertools.product(*axes)]
    print(f"grid: {total} points over {', '.join(k.label for k in knobs)}", file=sys.stderr)

    evaluator.quiet = True
    fit = evaluator.evaluate_batch(combos)
    evaluator.quiet = False

    held = holdout_scorer(args, build_dir, knobs, room_ir)
    hold_scores = [held[0](c) for c in combos] if held else [math.nan] * len(combos)

    labels = [k.label for k in knobs]
    width = max(len(lbl) for lbl in labels)
    print(
        "\n" + "  ".join(f"{lbl:>{width}}" for lbl in labels) + f"  {'fit':>10}  {'hold-out':>10}"
    )
    order = sorted(range(len(combos)), key=lambda i: fit[i])
    for i in order:
        values = "  ".join(f"{v:>{width}.4g}" for v in combos[i])
        hold = f"{hold_scores[i]:10.4f}" if math.isfinite(hold_scores[i]) else " " * 6 + "n/a"
        print(f"{values}  {fit[i]:10.4f}  {hold}")
    if held:
        print(
            f"\n  hold-out on {held[1]} {held[2]}. Read the two columns together: a "
            f"point that wins the fit and does nothing on the hold-out is a feature "
            f"of the fit set, and a broad region that is good on both is worth more "
            f"than a better isolated point."
        )
    else:
        print(
            "\n  No hold-out was asked for, so every number here is the fit set "
            "scoring itself. Add --validate-notes or --validate-velocities."
        )
    return 0


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
    resolved = holdout_scorer(args, build_dir, knobs, room_ir)
    if resolved is None:
        return None
    score, axis, held = resolved
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
    p.add_argument("--bank", type=int, default=0)
    p.add_argument("--pattern", required=True)
    p.add_argument("--notes", default="")
    p.add_argument("--velocities", default="")
    p.add_argument(
        "--dump-audio",
        default="",
        dest="dump_audio",
        help="also write the normalized mono render to this .npy path",
    )
    p.add_argument(
        "--room-ir",
        default="",
        dest="room_ir",
        help="convolve the render with this .npy impulse response first",
    )
    p.add_argument("--corpus", default="", help="capture manifest laying out the probe")
    p.add_argument("--corpus-timbre", default="", dest="corpus_timbre")
    p.add_argument("--drum-gate-ms", type=int, default=0, dest="drum_gate_ms")
    p.add_argument("--mono-mode", default="mean", dest="mono_mode", choices=list(MONO_MODES))
    p.add_argument(
        "--band-edge-hz",
        type=float,
        default=0.0,
        dest="band_edge_hz",
        help="the reference's own measurable ceiling; bands above it "
        "are excluded from the profile and from its normalisation",
    )
    p.add_argument(
        "--metric-threads",
        type=int,
        default=1,
        dest="metric_threads",
        help="measure this many of the probe's notes at once",
    )
    a = p.parse_args(argv)

    corpus = load_corpus(a.corpus, a.corpus_timbre) if a.corpus else None
    pattern, total, _ = _score(
        a.program, a.pattern, a.notes, a.velocities, corpus=corpus, gate_ms=a.drum_gate_ms
    )
    smf_bytes = write_smf(
        pattern.notes, program=a.program, bank=a.bank, channel=pattern.channel, end_pad=pattern.tail
    )
    # The model stops where the reference did. With no capture to ask, that is
    # the instrument's own boundary: a fit moves the voice, and the amplifier the
    # bank binds after it is not the voice's to answer for.
    rig = model_rig(corpus.rig) if corpus is not None else False
    audio = np.asarray(
        render_model(smf_bytes, total, render_rate(corpus), rig=rig), dtype=np.float32
    )
    if a.room_ir:
        # Applied here rather than in the parent so the per-note metrics and the
        # multi-scale term both see the same roomed signal.
        audio = apply_room(audio, np.load(a.room_ir))
    raw = to_mono(audio, a.mono_mode)
    mono = normalize_rms(raw)
    rows = probe_rows(
        mono,
        pattern,
        render_rate(corpus),
        raw=raw,
        max_band_hz=a.band_edge_hz or None,
        threads=a.metric_threads,
    )
    if a.dump_audio:
        np.save(a.dump_audio, mono.astype(np.float32))
    print(json.dumps(rows))
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def repo_tree_state() -> tuple[str | None, list[str]]:
    """HEAD's sha and any dirty path under src/, read once before a run touches anything.

    This has to run before the fit's own edits, not inside `build_shared` or at
    any of its per-candidate call sites: a rebuilding fit (a source knob with no
    `SONARE_TUNABLE` behind it) writes candidate values into src/ and rebuilds
    per candidate, so src/ is dirty by design for the run's whole duration once
    it starts — a check placed later would refuse that fit on its own second
    candidate.

    `git` missing or failing is reported and treated as absent provenance rather
    than refused: a tool that cannot run outside a checkout is a worse defect
    than the one this guards against.
    """
    try:
        status = subprocess.run(
            ["git", "status", "--porcelain", "--", "src/"],
            cwd=REPO_ROOT,
            capture_output=True,
            check=True,
            text=True,
        )
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            capture_output=True,
            check=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        print(
            f"tree state: git unavailable ({exc}); proceeding with no provenance", file=sys.stderr
        )
        return None, []
    dirty = [line for line in status.stdout.splitlines() if line.strip()]
    return head.stdout.strip(), dirty


def _fold_tree_provenance(out_path: str, head: str | None, dirty: list[str]) -> None:
    """Add this run's src/ tree state to an --out artifact already written to disk.

    A post-write step rather than a field threaded through report.py's or
    diagnose.py's own record shape, so every --out path carries the same two
    facts without each of them having to remember to.
    """
    if not out_path or not Path(out_path).exists():
        return
    path = Path(out_path)
    record = json.loads(path.read_text())
    record["tree"] = {"head": head, "dirty_src": dirty}
    path.write_text(json.dumps(record, indent=2) + "\n")


def _fold_write_back_verdict(out_path: str, evaluator) -> None:
    """Add why a finished fit kept the defaults, if it did.

    A refusal leaves a voice whose values did not change, which from every
    artifact reads identically to a search that found nothing worth writing.
    One of those is ordinary and one says the objective stopped being able to
    see the voice — a missing measurement axis rather than a fit problem — so
    the reason has to reach the record or the more valuable reading is lost
    with the scrollback. Same post-write shape as the tree state above.
    """
    if not out_path or not Path(out_path).exists():
        return
    path = Path(out_path)
    record = json.loads(path.read_text())
    record["write_back"] = {
        "refused": getattr(evaluator, "write_back_refusal", None),
        "tnr_notes": {
            "start": getattr(evaluator, "start_tnr_notes", None),
            "best": getattr(evaluator, "best_tnr_notes", None),
        },
    }
    path.write_text(json.dumps(record, indent=2) + "\n")


def run(args, argv: list[str] | None = None) -> int:
    build_dir = (REPO_ROOT / args.build_dir).resolve()
    if build_dir.name == "build-python-shared":
        raise ValueError("refusing to use build-python-shared; pick a private build dir")

    head_sha, dirty_src = repo_tree_state()
    if dirty_src and not getattr(args, "allow_dirty_src", False):
        raise RuntimeError(
            "refusing to fit with src/ dirty — whatever is there right now is what "
            "this run would compile and fit against, silently, with nothing in the "
            "output saying so:\n  "
            + "\n  ".join(dirty_src)
            + "\nCommit it, or pass --allow-dirty-src if this is your own in-flight "
            "engine edit."
        )
    if dirty_src:
        print(
            "src/ is dirty and --allow-dirty-src was given; fitting against it "
            "anyway:\n  " + "\n  ".join(dirty_src),
            file=sys.stderr,
        )

    resolve_probe(args)
    apply_spec_weights(args, argv if argv is not None else sys.argv[1:])
    weights = cli_weights(args)
    print(
        f"probe: pattern {args.pattern!r} on MIDI channel "
        f"{10 if args.percussive else 1}, scored with the "
        f"{'percussion' if args.percussive else 'harmonic'} metric set; weights "
        + " ".join(f"{t}={w:g}" for t, w in weights.items() if w > 0.0),
        file=sys.stderr,
    )
    refused = refused_weights(args)
    if refused:
        print(
            f"  --w-{' --w-'.join(refused)}: this metric set does not produce "
            f"{'that term' if len(refused) == 1 else 'those terms'}, so the weight is "
            f"multiplying a constant zero and the run is the same without it",
            file=sys.stderr,
        )
    # The other half: a weight nobody named, that the class asked for and the
    # probe's shape cannot fit. Dropping it is right; dropping it silently is
    # how a class default goes missing with nothing in the run to say so.
    dropped = dropped_weights(args)
    if dropped:
        kind = "kit" if args.percussive else "instrument"
        print(
            "  "
            + "; ".join(
                f"{t}: dropped from the {kind}'s class defaults because {why}" for t, why in dropped
            ),
            file=sys.stderr,
        )

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
            args.program,
            catalogue_pattern(args),
            str(dylib) if dylib else None,
            sr=render_rate(resolve_corpus(args)),
            notes=args.notes,
            bank=args.bank,
        )
        print(
            f"catalogue: {len(catalogue.defaults)} knobs across "
            f"{len({p for p, _ in catalogue.programs})} programs "
            f"({len(catalogue.programs)} with their variation banks), "
            f"{len(catalogue.bounds)} clamp bounds",
            file=sys.stderr,
        )

    if args.dump_knobs:
        key = (
            drum_patch_key(args.drum_note)
            if args.drum_note is not None
            else catalogue.patch_for(args.program, args.bank) or ""
        )
        rows = sorted(
            (k, v)
            for k, v in catalogue.defaults.items()
            if not args.program_only or (key and k.startswith(key + "."))
        )
        if args.drum_note is not None:
            print(f"# drum note {args.drum_note} is voiced by patch {key!r}")
        else:
            print(f"# program {args.program} bank {args.bank} is voiced by patch {key!r}")
            print(
                f"# program {args.program} has variation banks {catalogue.banks_for(args.program)}"
            )
        # Not the row count below, and the difference is the whole reason this
        # line exists: `auto_spec` drops a field the clamp leaves unbounded and
        # one whose range collapses, so anything scaled off the rows overshoots.
        try:
            taken = len(
                auto_spec(
                    args.program,
                    catalogue,
                    drum_note=args.drum_note,
                    bank=args.bank,
                    patch_only=args.program_only,
                )
            )
            print(f"# fit_knobs\t{taken}\tof {len(rows)} rows, what --spec auto takes")
        except ValueError as exc:
            print(f"# fit_knobs\t0\t--spec auto refuses this one: {exc}")
        print("# key\tdefault\tmin\tmax")
        for k, v in rows:
            bound = catalogue.bound_for(k)
            span = f"\t{bound[0]:g}\t{bound[1]:g}" if bound else "\t-\t-"
            print(f"{k}\t{v:g}{span}")
        return 0

    if auto:
        spec = auto_spec(
            args.program,
            catalogue,
            drum_note=args.drum_note,
            bank=args.bank,
            patch_only=args.program_only,
        )
        subject = (
            f"drum note {args.drum_note}"
            if args.drum_note is not None
            else f"program {args.program} bank {args.bank}"
        )
        patch = (
            drum_patch_key(args.drum_note)
            if args.drum_note is not None
            else catalogue.patch_for(args.program, args.bank)
        )
        print(
            f"--spec auto: {len(spec)} knobs for {subject} "
            f"(patch {patch!r}"
            f"{' alone' if args.program_only else ' + its engine'})",
            file=sys.stderr,
        )
    else:
        spec = load_spec(Path(args.spec).resolve())
        if any("." in e.get("tunable", "") for e in spec):
            configure_build(build_dir, args.cmake, tuning=True)
            build_shared(build_dir, args.cmake, args.jobs)
            dylib = dylib_path(build_dir)
            catalogue = dump_catalogue(
                args.program,
                catalogue_pattern(args),
                str(dylib) if dylib else None,
                sr=render_rate(resolve_corpus(args)),
                notes=args.notes,
            )

    pristine: dict[Path, str] = {}
    knobs = build_knobs(spec, pristine, catalogue)

    n_runtime = sum(1 for k in knobs if k.tunable is not None)
    n_source = len(knobs) - n_runtime
    if n_source:
        print(
            f"{len(knobs)} knobs ({n_runtime} runtime, {n_source} source) — "
            f"a source knob rebuilds the library every evaluation; converting it to "
            f"SONARE_TUNABLE would make this run far cheaper",
            file=sys.stderr,
        )
    else:
        print(
            f"{len(knobs)} runtime knobs — building once, then rendering per evaluation",
            file=sys.stderr,
        )

    configure_build(build_dir, args.cmake, tuning=n_runtime > 0)

    print("resolving oracle (once)...", file=sys.stderr)
    corpus = resolve_corpus(args)
    oracle, oracle_audio, room, band_edge = oracle_reference(args)

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
                build_dir,
                args.program,
                args.pattern,
                args.notes,
                velocities_csv=args.velocities,
                want_audio=True,
                corpus=corpus,
                gate_ms=getattr(args, "drum_gate_ms", 0),
                bank=getattr(args, "bank", 0),
                band_edge_hz=band_edge,
            )
            if dry_model is None:
                raise RuntimeError("room correction needs the model render, which came back empty")
            pattern, _, _ = _score(
                args.program,
                args.pattern,
                args.notes,
                args.velocities,
                corpus=corpus,
                gate_ms=getattr(args, "drum_gate_ms", 0),
            )
            spans = [(n.start, n.start + n.dur) for n in pattern.notes]
            ir = fit_room_ir(dry_model[:, None], render_rate(corpus), spans, room)
            ir_path = Path(tmp) / "room.npy"
            np.save(ir_path, ir)
        evaluator = Evaluator(
            knobs,
            pristine,
            oracle,
            oracle_audio,
            args,
            build_dir,
            room_ir=ir_path,
            corpus=corpus,
            band_edge_hz=band_edge,
        )
        if evaluator.workers > 1:
            print(f"rendering up to {evaluator.workers} candidates concurrently", file=sys.stderr)
        if args.diagnose:
            # Diagnosis replaces the fit rather than following it: what it
            # describes is the state of the tree, so the way to diagnose fitted
            # values is to let a fit write them back and run this next.
            try:
                # A diagnosis reads connectivity off exactly these renders, so
                # an unreached override turns every knob into a structural
                # finding about the voice. Same check, ahead of the same risk.
                evaluator.check_overrides_reach(knobs)
                run_diagnosis(evaluator, knobs, args, catalogue, out_path=args.out or "")
            finally:
                restore(pristine, evaluator.written)
            _fold_tree_provenance(args.out, head_sha, dirty_src)
            return 0
        if args.grid:
            # Also instead of fitting: the point is to look at the surface the
            # fit would have searched, so nothing is written back from it.
            try:
                evaluator.check_overrides_reach(knobs)
                return run_grid(evaluator, knobs, args, build_dir, ir_path)
            finally:
                restore(pristine, evaluator.written)
        try:
            # Before anything is searched, and unconditionally: a run whose
            # overrides never reach the library cannot fail any other way.
            evaluator.check_overrides_reach(knobs)
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
        # Before the report, because the hold-out decides which values the report
        # is about: it is scored on the winner and can then refuse it.
        validation = validate(
            args, build_dir, knobs, [k.start_value for k in knobs], best_values, ir_path
        )
        best_values = winner_or_defaults(knobs, best_values, evaluator, validation)

        pinned = report_pinned(knobs, best_values)
        if pinned:
            print(
                f"\n{len(pinned)} knob{'s' if len(pinned) > 1 else ''} ended on a range "
                f"bound — the search could not go further, so this may not be the optimum:",
                file=sys.stderr,
            )
            for line in pinned:
                print(f"  {line}", file=sys.stderr)
            print(
                "  Widen the range in the spec and re-run before trusting these values.",
                file=sys.stderr,
            )

        extra = {
            "room": room.to_dict() if room is not None else None,
            "pinned": pinned,
            "validation": validation,
        }
    report_result(knobs, pristine, best_values, evaluator, args, extra)
    _fold_tree_provenance(args.out, head_sha, dirty_src)
    _fold_write_back_verdict(args.out, evaluator)
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "_render_metrics":
        return render_metrics_main(sys.argv[2:])

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--spec",
        required=True,
        help="knob spec JSON path, or 'auto' to derive the knob list for "
        "--program from the library's own catalogue",
    )
    parser.add_argument(
        "--program",
        type=int,
        required=True,
        help="GM program to score, or the drum kit with --drum-note",
    )
    parser.add_argument(
        "--bank",
        type=int,
        default=0,
        help="GS variation bank of --program (default 0, the capital tone). "
        "A variation is its own patch with its own knobs, so this "
        "selects both what is rendered and what --spec auto offers: "
        "program 19 is a six-rank principal chorus at 0, three flute "
        "ranks at 8 and a full organ with reeds at 16. "
        "--dump-knobs lists the banks a program has",
    )
    parser.add_argument(
        "--drum-note",
        type=int,
        default=None,
        dest="drum_note",
        help="fit a percussion instrument instead of a GM program: the "
        "probe moves to the drum channel, where this note number "
        "selects the instrument (38 acoustic snare, 36 kick, 46 open "
        "hi-hat) and --program selects the kit. Scored with the "
        "percussion metric set, and --spec auto offers that note's "
        "own patch fields",
    )
    parser.add_argument(
        "--drum-gate-ms", type=int, default=0, dest="drum_gate_ms", help=DRUM_GATE_HELP
    )
    parser.add_argument(
        "--pattern",
        default="sustain",
        help="probe pattern (default: sustain, or 'drum' with --drum-note)",
    )
    parser.add_argument("--notes", default="", help="override probe notes, e.g. '48,60,72'")
    parser.add_argument(
        "--velocities",
        default="",
        help="override probe velocities, e.g. '64,100,127' (the 'drum' and 'velocity' patterns)",
    )
    parser.add_argument(
        "--dump-knobs",
        action="store_true",
        dest="dump_knobs",
        help="list every knob the library reports, with its default, and exit",
    )
    parser.add_argument(
        "--program-only",
        action="store_true",
        dest="program_only",
        help="offer only this program's (or drum note's) own patch "
        "fields, leaving out the calibration constants of the "
        "engine underneath it, which every other program on that "
        "engine shares. Applies to a fit as well as to "
        "--dump-knobs: a fit over one voice has nothing that "
        "could object to a shared constant moving, and a run "
        "over a grid of voices in turn moves the ground under "
        "the ones already done",
    )
    parser.add_argument(
        "--room",
        default="auto",
        choices=("auto", "none"),
        help="auto (default): measure the oracle's reverberation and place "
        "every model render in a matching space before scoring, so a "
        "reference recorded in a hall does not read as timbre; "
        "none: score as rendered",
    )
    parser.add_argument(
        "--corpus",
        default="",
        help="score against a captured single-note corpus: the directory a "
        "`capture.py corpus` run wrote (or its manifest.json). The probe "
        "is then the capture's own grid — its notes, its velocities and "
        "its gate — and the oracle is the captured audio assembled onto "
        "that timeline, so the fit and the reference profile measure the "
        "same stimulus. Cuts down with --notes / --velocities",
    )
    parser.add_argument(
        "--corpus-timbre",
        default="",
        dest="corpus_timbre",
        help="which timbre of the corpus to fit against (default: the first the manifest lists)",
    )
    parser.add_argument(
        "--allow-rigged-oracle",
        action="store_true",
        dest="allow_rigged_oracle",
        help="fit against a reference that carries a rig — an amplifier, a "
        "cabinet, a rotary speaker — or one nobody has classified on a "
        "family that could carry one. The instrument's knobs then "
        "absorb the amplifier: every metric improves and the values "
        "transfer to nothing once the rig is a stage of its own. "
        "Answer the capture's `rig` field instead wherever that is "
        "possible",
    )
    add_oracle_args(parser)
    parser.add_argument(
        "--max-evals",
        type=int,
        default=30,
        dest="max_evals",
        help="total evaluations (default: 30; raise it well past this "
        "when the spec is runtime knobs only)",
    )
    parser.add_argument(
        "--optimizer",
        default="coord",
        choices=("coord", "cmaes"),
        help="coord: golden-section coordinate descent (default). "
        "cmaes: covariance-matrix adaptation, which handles knobs "
        "that trade against each other and renders its population "
        "concurrently",
    )
    parser.add_argument(
        "--per-knob-evals",
        type=int,
        default=6,
        dest="per_knob_evals",
        help="golden-section budget per knob per pass (coord only, default: 6)",
    )
    parser.add_argument(
        "--population", type=int, default=0, help="CMA-ES population size (default: 4 + 3*ln(n))"
    )
    parser.add_argument(
        "--sigma0",
        type=float,
        default=0.25,
        help="CMA-ES initial step, as a fraction of each knob's range",
    )
    parser.add_argument("--seed", type=int, default=0, help="CMA-ES sampling seed")
    parser.add_argument(
        "--restarts",
        type=int,
        default=0,
        help="CMA-ES restarts from a fresh random point with a doubled "
        "population when a run stalls (default: 0). They share "
        "--max-evals rather than each getting their own",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=1,
        help="model renders to run concurrently (default: 1). Only helps "
        "--optimizer cmaes, whose population is independent; a "
        "rebuilding spec is forced back to 1",
    )
    parser.add_argument(
        "--metric-threads",
        type=int,
        default=0,
        dest="metric_threads",
        help=f"notes of the probe to measure at once WITHIN one render "
        f"(default: {AUTO_METRIC_THREADS}, and 1 whenever --workers "
        f"is spending the concurrency a level up). Measuring a note "
        f"is around a third of an evaluation and the notes are "
        f"independent, so this is the only concurrency a serial "
        f"optimiser like --optimizer coord can use",
    )
    parser.add_argument(
        "--no-cache",
        action="store_true",
        dest="no_cache",
        help="re-render every candidate instead of reading the ones an "
        "earlier run already measured. The store is keyed on the "
        "library's bytes, the harness source, the probe and the "
        "oracle, so a hit stands for the render it replaces; this "
        "is for proving that rather than assuming it",
    )
    parser.add_argument(
        "--diagnose",
        action="store_true",
        help="instead of fitting, report what the residual is made of and "
        "which parts of it no knob reaches — the difference between "
        "constants still slightly off and a mechanism the voice does "
        "not have. Costs 2 renders per knob and writes nothing; run it "
        "after a fit has written back, since it describes the tree as "
        "it stands. --out records the verdict as JSON",
    )
    parser.add_argument(
        "--grid",
        type=int,
        default=0,
        metavar="POINTS",
        help="instead of fitting, enumerate POINTS values of every knob "
        "in the spec (at most three) and print the fit and hold-out "
        "loss at each point of the product. What a search cannot "
        "show: whether its winner is a spike or a plateau, and "
        "whether a coarse grid's best point survives being scored "
        "on notes it never saw. Run it before trusting a search "
        "over knobs that interact",
    )
    parser.add_argument(
        "--screen",
        action="store_true",
        help="probe each knob at both ends first and drop the ones that do "
        "not move the loss, then fit only the rest. Costs 2 evaluations "
        "per knob out of --max-evals; the dropped knobs are named. Both "
        "ends and nothing between them, so a knob whose clamp is a guard "
        "rail rather than a search range can be dropped for looking flat "
        "across an interval it is only good in the middle of",
    )
    parser.add_argument(
        "--screen-threshold",
        type=float,
        default=0.002,
        dest="screen_threshold",
        help="smallest loss change over a knob's whole range that counts as "
        "an effect (default: 0.002, i.e. 0.2%% of the start loss)",
    )
    parser.add_argument(
        "--stages",
        action="store_true",
        help="fit the excitation knobs against the onset evidence, then the "
        "decay knobs against the decay evidence, then everything under "
        "the weights given here — instead of all of it at once",
    )
    parser.add_argument(
        "--validate-notes",
        default="",
        dest="validate_notes",
        help="score the result on these notes as well (e.g. '43,55,67'). "
        "They must be disjoint from --notes to mean anything: this is "
        "the only check that the fit did not overfit the probe",
    )
    parser.add_argument(
        "--validate-velocities",
        default="",
        dest="validate_velocities",
        help="the same check for a drum fit, which has no register to hold "
        "notes out of: score the result at these velocities as well "
        "(e.g. '48,88,112'), disjoint from the probe's",
    )
    parser.add_argument(
        "--validate-oracle-wav",
        default="",
        dest="validate_oracle_wav",
        help="the reference for the held-out probe, rendered the same way "
        "as --oracle-wav. Required with it, because a fixed WAV is one "
        "rendering of one probe and cannot supply the held-out notes",
    )
    parser.add_argument(
        "--out",
        default="",
        help="write the result (knob values, losses, overrides, validation) to this JSON path",
    )
    # The whole ladder `analyze_note` measures, and the same count `TERM_UNITS`
    # divides `harm` by — a lower default summed fewer cells than the unit
    # assumed and understated the term against every other one.
    parser.add_argument(
        "--n-harm",
        type=int,
        default=HARM_REACH,
        dest="n_harm",
        help=f"harmonics counted in the L1 timbre term "
        f"(default: {HARM_REACH}, the whole measured ladder)",
    )
    parser.add_argument(
        "--w-harm",
        type=float,
        default=None,
        dest="w_harm",
        help="weight on the harmonic-profile L1 term",
    )
    parser.add_argument(
        "--w-cents",
        type=float,
        default=None,
        dest="w_cents",
        help="weight on the intonation (cents) term",
    )
    parser.add_argument(
        "--w-tnr",
        type=float,
        default=None,
        dest="w_tnr",
        help="weight on the noise-floor (TNR shortfall) term",
    )
    parser.add_argument(
        "--w-env",
        type=float,
        default=None,
        dest="w_env",
        help="weight on the temporal-envelope term (sustain slope / release / "
        "attack; attack / decay / crest for a drum). Unset, the weight "
        "comes from the tone class: half for a sustained voice, where the "
        "spectrum is the identity, and 1 for a struck, plucked or modal "
        "one and for a drum, where the gesture is",
    )
    parser.add_argument(
        "--w-band",
        type=float,
        default=None,
        dest="w_band",
        help="drum fits: weight on the 1/3-octave level-profile term, the "
        "percussion analogue of the harmonic ladder",
    )
    parser.add_argument(
        "--w-bdecay",
        type=float,
        default=None,
        dest="w_bdecay",
        help="drum fits: weight on the per-octave-band decay-slope term",
    )
    parser.add_argument(
        "--w-tilt",
        type=float,
        default=None,
        dest="w_tilt",
        help="drum fits: weight on how far the hit's 2 kHz-and-up level "
        "sits from its 500 Hz-and-down level, against the same "
        "difference in the reference. A direction, which --w-band "
        "has none of",
    )
    parser.add_argument(
        "--w-bright",
        type=float,
        default=None,
        dest="w_bright",
        help="drum fits: weight on the spectral centroid as a percentage "
        "of the reference's. The gate's own arithmetic, so a fit "
        "moves the quantity the kit is judged on",
    )
    parser.add_argument(
        "--w-kit",
        type=float,
        default=None,
        dest="w_kit",
        help="drum fits: weight on the RELATIONS INSIDE THE KIT — the tom "
        "series, the hi-hat trio, the cymbals — as the sorted contrasts "
        "in pitch, decay, colour and level within each family the "
        "capture declares. Every other percussion term scores one hit "
        "against its own reference row and is capped, so a kit whose "
        "members are each individually plausible and collectively in "
        "the wrong relation reads as correct; worse, once the members "
        "are far enough out the per-hit terms saturate and the repair "
        "has no gradient to follow. Needs a corpus whose capture names "
        "its families AND a grid that covers one: --drum-note narrows "
        "to a single note by itself, so name the family with --notes",
    )
    parser.add_argument(
        "--w-init",
        type=float,
        default=None,
        dest="w_init",
        help="weight on the per-harmonic ONSET-ladder term (excitation evidence)",
    )
    parser.add_argument(
        "--w-slope",
        type=float,
        default=None,
        dest="w_slope",
        help="weight on the per-harmonic decay-slope term (loop evidence)",
    )
    parser.add_argument(
        "--w-tail",
        type=float,
        default=None,
        dest="w_tail",
        help="weight on the per-harmonic decay slope 2-6 s in, which only has "
        "frames to fit when the probe holds a note that long. This is "
        "the aftersound: on a piano it is most of the note, and no "
        "two-second probe can reach it",
    )
    parser.add_argument(
        "--w-hf",
        type=float,
        default=None,
        dest="w_hf",
        help="weight on the attack's high-band balance, measured in 20 ms "
        "slices over the first 120 ms. Catches a strike-noise or "
        "excitation path whose top end is wrong for a few tens of "
        "milliseconds — a tick, which the ear finds instantly and a "
        "whole-timeline spectral distance averages away",
    )
    parser.add_argument(
        "--w-lf",
        type=float,
        default=None,
        dest="w_lf",
        help="weight on the attack's low- and mid-band balance, 20 Hz to "
        "4 kHz over one 50 ms window. The bass counterpart of --w-hf: "
        "catches an excitation that dumps its energy below where the "
        "instrument radiates, which reads as a note with no onset and "
        "which every sustain-window term here scores as correct",
    )
    parser.add_argument(
        "--w-stiff",
        type=float,
        default=None,
        dest="w_stiff",
        help="weight on STRING STIFFNESS: how far the model stretches its "
        "twelfth partial against how far the reference does, in cents. "
        "The ladder is now measured along each string's own partial "
        "series, which is what makes it correct and also what leaves "
        "the series itself unpriced — a voice twice as stiff as its "
        "reference otherwise scores a clean sheet",
    )
    parser.add_argument(
        "--w-dyn",
        type=float,
        default=None,
        dest="w_dyn",
        help="weight on the DYNAMICS CURVE: how brightness tracks velocity, "
        "fitted per pitch so register is held fixed. The only term "
        "that lives in the relation between notes rather than inside "
        "one — a model can match every note of a grid one at a time "
        "and still get the trend between them wrong. Needs a probe "
        "with a velocity axis (--pattern velocity, or a drum probe)",
    )
    parser.add_argument(
        "--w-level",
        type=float,
        default=None,
        dest="w_level",
        help="weight on the level BALANCE across the probe grid: how loud each "
        "note is relative to the others, with the grid's own median "
        "offset removed so an output-gain difference is not fitted. "
        "Needs a probe with more than one note to mean anything",
    )
    parser.add_argument(
        "--max-level-drift-db",
        type=float,
        default=6.0,
        dest="max_level_drift_db",
        help="how far the winner may move the voice's whole-grid level away "
        "from the start point before the loss charges it (default 6 dB; "
        "0 disables). A fence rather than a term: inside it the score is "
        "unchanged. Every other term is level-normalised, so without it a "
        "candidate can buy a better spectrum by quietening the voice - "
        "measured at 31 dB down with a bit-identical band profile. It is "
        "anchored on the START point, so it also holds a voice AT a level "
        "an earlier round left it at: a knob that moves the level as well "
        "as the shape - a filter corner does - has to be fitted with the "
        "gain beside it, or the fence charges the move by more than the "
        "corrected shape is worth and the search walks the other way",
    )
    parser.add_argument(
        "--max-sustain-drift-db-s",
        type=float,
        default=3.0,
        dest="max_sustain_drift_db_s",
        help="how much faster than the start point a winner's worst note may "
        "fall away over its held section, in dB/s, before the loss "
        "charges it (default 3; 0 disables). The companion fence to "
        "--max-level-drift-db, against the trade that takes the note "
        "away rather than the gain: `env` does carry the sustain slope, "
        "but it is divided by its own value at the start point, so a "
        "voice that already falls steeply has a flat objective in the "
        "one dimension it is worst in. One-sided and anchored on the "
        "START, so a voice whose mechanism cannot sustain is not asked "
        "to fix that with a knob - what this stops is the fit making it "
        "worse",
    )
    parser.add_argument(
        "--w-crest",
        type=float,
        default=None,
        dest="w_crest",
        help="weight on peak-minus-held-RMS per note. Gain-invariant, and the "
        "one term that sees a note whose envelope never falls after its "
        "attack — every other term here is normalised past it",
    )
    parser.add_argument(
        "--w-mss",
        type=float,
        default=None,
        dest="w_mss",
        help="weight on the multi-scale STFT distance over the whole render "
        "(sees what the per-note metric set does not model)",
    )
    parser.add_argument(
        "--w-modes",
        type=float,
        default=None,
        dest="w_modes",
        help="weight on the MEASURED PARTIAL SERIES: the partials as found, "
        "paired against the reference's by frequency, priced in cents "
        "and dB. The harmonic ladder searches n*f0*sqrt(1+B*n^2), which "
        "describes a stiff string and nothing else, so on a bar, a bell, "
        "a plate or a membrane every bin above the fundamental reads the "
        "render's own noise floor - on BOTH sides. That covers GM 8-14, "
        "47, 55 and 112-118, and every drum note with a definite pitch. "
        "Weighted by default for those, and available for any voice",
    )
    parser.add_argument(
        "--w-mod",
        type=float,
        default=None,
        dest="w_mod",
        help="weight on MOVEMENT: vibrato depth and rate, tremolo, the slow "
        "beat of an ensemble or a unison pair, and how wide the "
        "fundamental is. A sampled reference is a recording of a player "
        "and carries all of it; a physical model renders a still note "
        "unless told otherwise, and every other term here reads that "
        "stillness as cleanliness - --w-tnr charges the model only for "
        "being NOISIER, so nothing could ever ask for vibrato",
    )
    parser.add_argument(
        "--flat-partial-weighting",
        action="store_true",
        dest="flat_partial_weighting",
        help="score every partial of the harmonic term equally, as this "
        "harness did before audibility weighting. By default a partial's "
        "vote is scaled by an A-weighting at its own frequency and by how "
        "far it sits under the loudest partial of the same note - so A0's "
        "27.5 Hz fundamental no longer outvotes the partials that carry "
        "its timbre, and a 10 dB error on something 50 dB down is no "
        "longer charged in full",
    )
    parser.add_argument(
        "--raw-loss",
        action="store_true",
        dest="raw_loss",
        help="weight the terms in their own units instead of normalising "
        "each to its value at the start point. The weights then mean "
        "whatever the units make them mean",
    )
    parser.add_argument(
        "--mono-mode",
        default="mean",
        dest="mono_mode",
        choices=list(MONO_MODES),
        help="how a stereo render is reduced to one channel "
        "(default: mean). A reference captured through a "
        "spaced close pair is decorrelated by construction, "
        "so summing it notches the frequencies where the "
        "path difference is half a wavelength - and a notch "
        "on a partial reads as harmonic error a mono model "
        "cannot reproduce. 'left' and 'loudest' take one "
        "channel and have no sum in them. The default is a "
        "sum because every committed profile in reference/ "
        "was measured through one and cannot be re-measured "
        "without the plugin it came from",
    )
    parser.add_argument(
        "--allow-dirty-src",
        action="store_true",
        dest="allow_dirty_src",
        help="fit anyway when src/ has uncommitted changes. Refused by "
        "default: this tree routinely has several sessions working "
        "in src/ at once, and whatever is there at build time is "
        "compiled and fit against with nothing in the output saying "
        "so. The normal reason to pass this is fitting against your "
        "own in-flight engine edit. The dirty paths and HEAD's sha "
        "are recorded in --out either way",
    )
    parser.add_argument(
        "--build-dir",
        default="build-autofit",
        dest="build_dir",
        help="isolated build dir (default: build-autofit)",
    )
    parser.add_argument("--jobs", type=int, default=8, help="parallel build jobs")
    parser.add_argument("--cmake", default="cmake", help="cmake executable")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        dest="dry_run",
        help="restore pristine and skip writing the best values",
    )
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
