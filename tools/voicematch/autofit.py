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

from _repo import REPO_ROOT  # noqa: E402
from build_lib import build_shared, configure_build, dylib_path  # noqa: E402
from catalogue import Catalogue, drum_patch_key, dump_catalogue  # noqa: E402
from corpus import (  # noqa: E402
    PERCUSSION_CHANNEL as CORPUS_PERCUSSION_CHANNEL,
    Corpus, check_rig, corpus_oracle, corpus_pattern, describe, load_corpus,
)
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
    KIT_MIN_MEMBERS,
    LOSS_TERMS,
    LossWeights,
    cli_weights,
    mss_distance,
    probe_rows,
    score_terms,
)
from diagnose import run_diagnosis  # noqa: E402
from metrics import (  # noqa: E402
    MONO_MODES,
    channel_correlation,
    measure_band_edge,
    normalize_rms,
    to_mono,
)
from optimizers import cma_es, optimize  # noqa: E402
from patterns import DRUM_GATE_HELP, build_pattern, pattern_length  # noqa: E402
from render_model import render_model  # noqa: E402
from render_oracle import (  # noqa: E402
    add_oracle_args,
    check_oracle_rig,
    obtain_oracle,
    oracle_may_carry_room,
)
from report import report_result  # noqa: E402
from room import apply_room, estimate_room, fit_room_ir  # noqa: E402
from smf import write_smf  # noqa: E402
from staging import SubEvaluator, run_stages, screen_knobs  # noqa: E402
from writeback import materialize, restore, write_edits  # noqa: E402

SR = 48000
#: Under this much inter-channel correlation, summing a stereo reference to mono
#: comb-filters it enough to matter — a spaced close pair on a piano runs around
#: 0.5 through the midrange. Reported rather than acted on; see `--mono-mode`.
STEREO_COMB_CORRELATION = 0.8

# What a decibel of level drift past the allowance costs, on a loss the start
# point scores 1.0. Steep on purpose: the drift the fence exists to stop was
# 10 to 31 dB, and no shape a fit can buy at that price is worth keeping.
LEVEL_DRIFT_PENALTY_PER_DB = 0.1


# --------------------------------------------------------------------------- #
# The probe: what is rendered, and what comes back measured
# --------------------------------------------------------------------------- #
def _score(program: int, pattern_name: str, notes_csv: str, velocities_csv: str = "",
           corpus: Corpus | None = None, gate_ms: int = 0):
    kwargs = {}
    if notes_csv:
        kwargs["notes"] = tuple(int(n) for n in notes_csv.split(","))
    if velocities_csv:
        kwargs["velocities"] = tuple(int(v) for v in velocities_csv.split(","))
    if gate_ms:
        kwargs["dur"] = gate_ms / 1000.0
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
        # A corpus and a drum note go together exactly when the corpus is itself
        # a kit, captured on the drum channel. Against a pitched corpus the two
        # are still alternatives — the probe would sound a channel the capture
        # has no recordings for, and every slot would score model against
        # silence.
        if args.drum_note is not None and not corpus.percussive():
            raise ValueError(
                f"--drum-note needs a corpus captured on MIDI channel "
                f"{CORPUS_PERCUSSION_CHANNEL}; the {corpus.timbre!r} corpus is a grid of "
                f"pitched single notes, so a drum probe would score a channel it has no "
                f"captures for"
            )
        if args.drum_note is None and corpus.percussive():
            raise ValueError(
                f"the {corpus.timbre!r} corpus is a kit, captured on MIDI channel "
                f"{CORPUS_PERCUSSION_CHANNEL}; pass --drum-note N so the fit knows which "
                f"piece's knobs to move — a kit has one patch per note and no register to "
                f"interpolate across"
            )
        if getattr(args, "oracle_wav", ""):
            raise ValueError(
                "--corpus and --oracle-wav both name the reference; a corpus run assembles "
                "its oracle from the capture, so drop one of them"
            )
        # A reference that carries a rig is an acceptance target and never a fit
        # target, so the refusal comes before anything is built rather than hours
        # in. `--diagnose` measures which knobs reach which term rather than
        # moving any of them towards the reference, and is exempt; `--grid`
        # evaluates the same objective a fit would search and is not.
        if not getattr(args, "diagnose", False):
            check_rig(corpus, args.program,
                      allow=getattr(args, "allow_rigged_oracle", False))
        args.pattern = "corpus"
    elif not getattr(args, "diagnose", False):
        # No capture, so no record of a rig — and the hazard is the same size.
        # What the route means by carrying no record is not the same for all
        # three of them, which is what `check_oracle_rig` sorts out.
        check_oracle_rig(args, args.program,
                         allow=getattr(args, "allow_rigged_oracle", False))
    pattern, _, analysis_notes = _score(
        args.program, args.pattern, args.notes, args.velocities, corpus=corpus,
        gate_ms=getattr(args, "drum_gate_ms", 0),
    )
    if corpus is not None:
        print(describe(corpus, pattern), file=sys.stderr)
    args.percussive = pattern.percussive
    # Whether the probe has anything to measure a note at a time. `cli_weights`
    # fills unset weights from the instrument's class, and on a pattern with no
    # analysis notes every per-note term it would supply has nothing to read —
    # so the class defaults collapse to the whole-timeline ones instead of being
    # supplied and then refused. A weight named on the command line is still
    # refused, because asking for a measurement the probe cannot take is a
    # mistake worth reporting rather than one worth silently dropping.
    args.has_analysis_notes = bool(analysis_notes)
    # Whether this probe has any kit relation to read. The families come from
    # the capture, so a fit without a corpus has none, and a fit that narrowed
    # the grid to one drum note has none either — the term is about a family,
    # and one member is not one. Supplied as a class default, so it is dropped
    # rather than refused; an explicit --w-kit is refused below.
    probe_notes = {n.note for n in pattern.analysis_notes}
    args.has_kit_groups = any(
        len(probe_notes.intersection(members)) >= KIT_MIN_MEMBERS
        for members in (getattr(corpus, "groups", None) or {}).values()
    )
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
    # The dynamics term is fitted per pitch across velocity, so a probe that
    # sounds every note once has nothing for it to fit. Refused rather than
    # scored, because its unmeasurable value is 0.0 and 0.0 is also its best
    # possible score: weighted on a `sustain` probe it would report a perfect
    # dynamics match on every candidate and quietly dilute the whole objective.
    if cli_weights(args).get("dyn", 0.0) > 0.0:
        spread = {n.note: set() for n in pattern.analysis_notes}
        for n in pattern.analysis_notes:
            spread[n.note].add(n.velocity)
        if not any(len(v) >= 2 for v in spread.values()):
            raise ValueError(
                f"--w-dyn fits brightness against velocity per pitch, and pattern "
                f"{args.pattern!r} sounds each note at a single velocity, so there is no "
                f"curve to fit. Use --pattern velocity, a drum probe, or drop --w-dyn."
            )
    # Same shape as the dynamics refusal, on the other between-note axis. An
    # unscorable `kit` is 0.0, which is also its best value, so a run weighting
    # it against a probe with no family in it would report a perfect kit on
    # every candidate.
    if getattr(args, "w_kit", None) and not args.has_kit_groups:
        raise ValueError(
            f"--w-kit scores the relations inside a kit's own families, and this run has "
            f"none to read: pattern {args.pattern!r} covers fewer than {KIT_MIN_MEMBERS} "
            f"members of any family its capture declares. --drum-note narrows the grid to "
            f"that one note on its own, so name the whole family with --notes (the six "
            f"toms, the three hi-hats), or drop --w-kit."
        )
    if not analysis_notes:
        per_note = {t: w for t, w in cli_weights(args).items() if t != "mss" and w > 0.0}
        if per_note:
            raise ValueError(
                f"pattern {args.pattern!r} has no analyzable notes, so the per-note terms "
                f"{sorted(per_note)} have nothing to measure. Weight only --w-mss, or "
                f"pick a pattern with analysis notes."
            )


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
    measure, for a percussion probe (`measure_band_edge`), or None when it
    carries the whole analysis range. It is resolved HERE, from the oracle, and
    then handed to every model render of the run: a bandwidth is a property of
    the reference, and if the two sides derived it separately the model would
    normalise its band profile against a different set of bands than the
    reference did and every band reading would move.
    """
    corpus = resolve_corpus(args)
    pattern, total, _ = _score(
        args.program, args.pattern, args.notes, getattr(args, "velocities", ""), corpus=corpus,
        gate_ms=getattr(args, "drum_gate_ms", 0),
    )
    if corpus is not None:
        # Assembled from the capture rather than played: the reference for a
        # corpus run already exists as audio, one file per slot, and nothing
        # about it depends on a plugin still being installed.
        audio = corpus_oracle(corpus, pattern, SR)
    else:
        smf_bytes = write_smf(
            pattern.notes, program=args.program, bank=getattr(args, "bank", 0), channel=pattern.channel,
            end_pad=pattern.tail
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
        elif measured.gated():
            print(f"oracle room: RT60 {measured.rt60_s:.2f}s, "
                  f"tail level {measured.tail_db:+.1f}dB — NOT corrected. The probe holds each "
                  f"note for {measured.note_window_s * 1000:.0f} ms against that decay, so the "
                  f"tail level measures the gate rather than the room and a correction fitted to "
                  f"it invents one. The reference's space stays in every decay term of the "
                  f"objective; do not read a fitted decay as the instrument's.", file=sys.stderr)
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
        print(f"  note: the oracle's channels correlate at {corr:+.2f}, so summing "
              f"them comb-filters what differs between them. --mono-mode left "
              f"takes one channel and has no sum in it.", file=sys.stderr)
    raw = to_mono(audio, mode)
    mono = normalize_rms(raw)
    rows = probe_rows(mono, pattern, SR, raw=raw)
    edge = measure_band_edge(rows) if pattern.percussive else None
    if edge is not None:
        # Re-measured rather than patched. The band profile is normalised to the
        # loudest band inside the edge, and that is not a scaling that can be
        # applied to an already-floored profile without inventing the values the
        # floor took away.
        rows = probe_rows(mono, pattern, SR, raw=raw, max_band_hz=edge)
        print(f"reference bandwidth: {edge / 1000.0:.1f} kHz — bands above it are "
              f"the capture chain rather than the kit, and are excluded from the "
              f"band profile on BOTH sides", file=sys.stderr)
    return rows, mono, room, edge


def render_model_rows_subprocess(
    build_dir: Path, program: int, pattern_name: str, notes_csv: str,
    *, velocities_csv: str = "", overrides: str = "", want_audio: bool = False,
    room_ir: Path | None = None, corpus: Corpus | None = None, gate_ms: int = 0,
    bank: int = 0, band_edge_hz: float | None = None,
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
    if bank:
        cmd += ["--bank", str(bank)]
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
                 corpus=None, band_edge_hz=None):
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
        self.best_level_offset_db: float | None = None
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
        """
        n_runtime = sum(1 for k in knobs if k.tunable is not None)
        if not n_runtime:
            return

        def far(k):
            if k.tunable is None:
                return k.start_value
            return (k.hi if abs(k.hi - k.start_value) >= abs(k.start_value - k.lo)
                    else k.lo)

        base = self._render_terms([k.start_value for k in knobs])
        moved = self._render_terms([far(k) for k in knobs])
        if base is None or moved is None:
            raise RuntimeError(
                "the override reach check could not score a render — the model "
                "produced nothing measurable at the start values or at the ends "
                "of the spec's ranges"
            )
        if all(abs(base[t] - moved[t]) < 1e-12 for t in LOSS_TERMS):
            raise RuntimeError(
                f"{n_runtime} runtime knobs were pushed to the far end of every "
                f"range at once and not one measurement moved. The overrides are "
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
            self.build_dir, self.args.program, self.args.pattern, self.args.notes,
            velocities_csv=self.args.velocities,
            overrides=tunable_overrides(self.knobs, values),
            want_audio=want_audio, room_ir=self.room_ir, corpus=self.corpus,
            gate_ms=getattr(self.args, "drum_gate_ms", 0), bank=getattr(self.args, "bank", 0),
            band_edge_hz=self.band_edge_hz,
        )
        mss = 0.0
        if want_audio and model_audio is not None:
            mss = mss_distance(model_audio, self.oracle_audio)
        return score_terms(model_rows, self.oracle, n_harm=self.args.n_harm, mss=mss,
                           percussive=self.percussive, groups=self.groups,
                           audibility=not getattr(self.args, "flat_partial_weighting", False))

    def _score_cached(self, values: list[float], terms: dict[str, float] | None) -> float:
        """Score an already-rendered candidate under the current weights.

        A cache hit still decides things. Every stage begins by re-scoring the
        point it inherited, which is by construction already rendered, so a
        stage whose samples never beat its own start point would otherwise end
        with `best_loss` still at infinity and hand back the first finite loss
        it happened to see — a candidate worse than the one it was given, on its
        way into the source.
        """
        loss = self.loss.combine(terms) + self._level_drift_penalty(terms)
        if loss < self.best_loss:
            self.best_loss = loss
            self.best_values = list(values)
            self.best_level_offset_db = (
                None if terms is None else terms.get("level_offset_db")
            )
        return loss

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
        return LEVEL_DRIFT_PENALTY_PER_DB * excess if excess > 0.0 else 0.0

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
            self.start_level_offset_db = terms.get("level_offset_db")
        loss = self.loss.combine(terms) + self._level_drift_penalty(terms)
        if loss < self.best_loss:
            self.best_loss = loss
            self.best_values = list(values)
            # Kept alongside the best values because nothing else can recover
            # it afterwards, and because it is the one number a fit can move
            # freely: the level term scores the spread around the grid's median
            # offset, so a candidate that bought its shape by making the voice
            # quieter scores exactly as if it had not.
            self.best_level_offset_db = (
                None if terms is None else terms.get("level_offset_db")
            )
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
        print(f"  the held-out {axis} produced no analyzable oracle rows — skipped",
              file=sys.stderr)
        return None

    resolved = cli_weights(args)
    weights = LossWeights(resolved)
    # From the resolved weight, not from the flag: `--w-mss` defaults to None so
    # that an unset weight and an explicit zero stay distinguishable.
    want_audio = resolved.get("mss", 0.0) > 0.0
    corpus = resolve_corpus(holdout)

    def score(values: list[float]) -> float:
        rows, audio = render_model_rows_subprocess(
            build_dir, args.program, args.pattern, holdout.notes,
            velocities_csv=holdout.velocities,
            overrides=tunable_overrides(knobs, values),
            want_audio=want_audio, room_ir=room_ir, corpus=corpus,
            gate_ms=getattr(holdout, "drum_gate_ms", 0), bank=getattr(args, "bank", 0),
            band_edge_hz=band_edge,
        )
        mss = mss_distance(audio, oracle_audio) if want_audio and audio is not None else 0.0
        terms = score_terms(rows, oracle_rows, n_harm=args.n_harm, mss=mss,
                            percussive=percussive,
                            groups=dict(getattr(corpus, "groups", None) or {}),
                            audibility=not getattr(args, "flat_partial_weighting", False))
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
        print(f"--grid enumerates a product, and {len(knobs)} knobs is not a surface "
              f"anyone can read. Narrow the spec to at most three.", file=sys.stderr)
        return 2
    points = max(2, args.grid)
    total = points ** len(knobs)
    if total > GRID_MAX_POINTS:
        print(f"--grid {points} over {len(knobs)} knobs is {total} points, past the "
              f"{GRID_MAX_POINTS} this will enumerate. Use fewer points or fewer knobs.",
              file=sys.stderr)
        return 2

    axes = [np.linspace(k.lo, k.hi, points).tolist() for k in knobs]
    combos = [list(c) for c in itertools.product(*axes)]
    print(f"grid: {total} points over {', '.join(k.label for k in knobs)}",
          file=sys.stderr)

    evaluator.quiet = True
    fit = evaluator.evaluate_batch(combos)
    evaluator.quiet = False

    held = holdout_scorer(args, build_dir, knobs, room_ir)
    hold_scores = [held[0](c) for c in combos] if held else [math.nan] * len(combos)

    labels = [k.label for k in knobs]
    width = max(len(lbl) for lbl in labels)
    print("\n" + "  ".join(f"{lbl:>{width}}" for lbl in labels)
          + f"  {'fit':>10}  {'hold-out':>10}")
    order = sorted(range(len(combos)), key=lambda i: fit[i])
    for i in order:
        values = "  ".join(f"{v:>{width}.4g}" for v in combos[i])
        hold = f"{hold_scores[i]:10.4f}" if math.isfinite(hold_scores[i]) else " " * 6 + "n/a"
        print(f"{values}  {fit[i]:10.4f}  {hold}")
    if held:
        print(f"\n  hold-out on {held[1]} {held[2]}. Read the two columns together: a "
              f"point that wins the fit and does nothing on the hold-out is a feature "
              f"of the fit set, and a broad region that is good on both is worth more "
              f"than a better isolated point.")
    else:
        print("\n  No hold-out was asked for, so every number here is the fit set "
              "scoring itself. Add --validate-notes or --validate-velocities.")
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
    p.add_argument("--dump-audio", default="", dest="dump_audio",
                   help="also write the normalized mono render to this .npy path")
    p.add_argument("--room-ir", default="", dest="room_ir",
                   help="convolve the render with this .npy impulse response first")
    p.add_argument("--corpus", default="", help="capture manifest laying out the probe")
    p.add_argument("--corpus-timbre", default="", dest="corpus_timbre")
    p.add_argument("--drum-gate-ms", type=int, default=0, dest="drum_gate_ms")
    p.add_argument("--mono-mode", default="mean", dest="mono_mode",
                   choices=list(MONO_MODES))
    p.add_argument("--band-edge-hz", type=float, default=0.0, dest="band_edge_hz",
                   help="the reference's own measurable ceiling; bands above it "
                        "are excluded from the profile and from its normalisation")
    a = p.parse_args(argv)

    corpus = load_corpus(a.corpus, a.corpus_timbre) if a.corpus else None
    pattern, total, _ = _score(a.program, a.pattern, a.notes, a.velocities, corpus=corpus,
                               gate_ms=a.drum_gate_ms)
    smf_bytes = write_smf(
        pattern.notes, program=a.program, bank=a.bank, channel=pattern.channel,
        end_pad=pattern.tail
    )
    audio = np.asarray(render_model(smf_bytes, total, SR), dtype=np.float32)
    if a.room_ir:
        # Applied here rather than in the parent so the per-note metrics and the
        # multi-scale term both see the same roomed signal.
        audio = apply_room(audio, np.load(a.room_ir))
    raw = to_mono(audio, a.mono_mode)
    mono = normalize_rms(raw)
    rows = probe_rows(mono, pattern, SR, raw=raw,
                      max_band_hz=a.band_edge_hz or None)
    if a.dump_audio:
        np.save(a.dump_audio, mono.astype(np.float32))
    print(json.dumps(rows))
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def apply_spec_weights(args, argv: list[str]) -> None:
    """Let a spec set the term weights it needs, unless the command line said otherwise.

    Most of the `--w-*` flags default to zero, which means a fit run without
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
            sr=SR, notes=args.notes, bank=args.bank,
        )
        print(f"catalogue: {len(catalogue.defaults)} knobs across "
              f"{len({p for p, _ in catalogue.programs})} programs "
              f"({len(catalogue.programs)} with their variation banks), "
              f"{len(catalogue.bounds)} clamp bounds",
              file=sys.stderr)

    if args.dump_knobs:
        key = (drum_patch_key(args.drum_note) if args.drum_note is not None
               else catalogue.patch_for(args.program, args.bank) or "")
        rows = sorted(
            (k, v) for k, v in catalogue.defaults.items()
            if not args.program_only or (key and k.startswith(key + "."))
        )
        if args.drum_note is not None:
            print(f"# drum note {args.drum_note} is voiced by patch {key!r}")
        else:
            print(f"# program {args.program} bank {args.bank} is voiced by patch {key!r}")
            print(f"# program {args.program} has variation banks "
                  f"{catalogue.banks_for(args.program)}")
        print("# key\tdefault\tmin\tmax")
        for k, v in rows:
            bound = catalogue.bound_for(k)
            span = f"\t{bound[0]:g}\t{bound[1]:g}" if bound else "\t-\t-"
            print(f"{k}\t{v:g}{span}")
        return 0

    if auto:
        spec = auto_spec(args.program, catalogue, drum_note=args.drum_note, bank=args.bank)
        subject = (f"drum note {args.drum_note}" if args.drum_note is not None
                   else f"program {args.program} bank {args.bank}")
        patch = (drum_patch_key(args.drum_note) if args.drum_note is not None
                 else catalogue.patch_for(args.program, args.bank))
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
                build_dir, args.program, args.pattern, args.notes,
                velocities_csv=args.velocities, want_audio=True, corpus=corpus,
                gate_ms=getattr(args, "drum_gate_ms", 0), bank=getattr(args, "bank", 0),
                band_edge_hz=band_edge,
            )
            if dry_model is None:
                raise RuntimeError("room correction needs the model render, which came back empty")
            pattern, _, _ = _score(args.program, args.pattern, args.notes, args.velocities,
                                   corpus=corpus, gate_ms=getattr(args, "drum_gate_ms", 0))
            spans = [(n.start, n.start + n.dur) for n in pattern.notes]
            ir = fit_room_ir(dry_model[:, None], SR, spans, room)
            ir_path = Path(tmp) / "room.npy"
            np.save(ir_path, ir)
        evaluator = Evaluator(
            knobs, pristine, oracle, oracle_audio, args, build_dir, room_ir=ir_path,
            corpus=corpus, band_edge_hz=band_edge,
        )
        if evaluator.workers > 1:
            print(f"rendering up to {evaluator.workers} candidates concurrently",
                  file=sys.stderr)
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
    parser.add_argument("--bank", type=int, default=0,
                        help="GS variation bank of --program (default 0, the capital tone). "
                             "A variation is its own patch with its own knobs, so this "
                             "selects both what is rendered and what --spec auto offers: "
                             "program 19 is a six-rank principal chorus at 0, three flute "
                             "ranks at 8 and a full organ with reeds at 16. "
                             "--dump-knobs lists the banks a program has")
    parser.add_argument("--drum-note", type=int, default=None, dest="drum_note",
                        help="fit a percussion instrument instead of a GM program: the "
                             "probe moves to the drum channel, where this note number "
                             "selects the instrument (38 acoustic snare, 36 kick, 46 open "
                             "hi-hat) and --program selects the kit. Scored with the "
                             "percussion metric set, and --spec auto offers that note's "
                             "own patch fields")
    parser.add_argument("--drum-gate-ms", type=int, default=0, dest="drum_gate_ms",
                        help=DRUM_GATE_HELP)
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
    parser.add_argument("--allow-rigged-oracle", action="store_true",
                        dest="allow_rigged_oracle",
                        help="fit against a reference that carries a rig — an amplifier, a "
                             "cabinet, a rotary speaker — or one nobody has classified on a "
                             "family that could carry one. The instrument's knobs then "
                             "absorb the amplifier: every metric improves and the values "
                             "transfer to nothing once the rig is a stage of its own. "
                             "Answer the capture's `rig` field instead wherever that is "
                             "possible")
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
    parser.add_argument("--diagnose", action="store_true",
                        help="instead of fitting, report what the residual is made of and "
                             "which parts of it no knob reaches — the difference between "
                             "constants still slightly off and a mechanism the voice does "
                             "not have. Costs 2 renders per knob and writes nothing; run it "
                             "after a fit has written back, since it describes the tree as "
                             "it stands. --out records the verdict as JSON")
    parser.add_argument("--grid", type=int, default=0, metavar="POINTS",
                        help="instead of fitting, enumerate POINTS values of every knob "
                             "in the spec (at most three) and print the fit and hold-out "
                             "loss at each point of the product. What a search cannot "
                             "show: whether its winner is a spike or a plateau, and "
                             "whether a coarse grid's best point survives being scored "
                             "on notes it never saw. Run it before trusting a search "
                             "over knobs that interact")
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
    parser.add_argument("--w-harm", type=float, default=None, dest="w_harm",
                        help="weight on the harmonic-profile L1 term")
    parser.add_argument("--w-cents", type=float, default=None, dest="w_cents",
                        help="weight on the intonation (cents) term")
    parser.add_argument("--w-tnr", type=float, default=None, dest="w_tnr",
                        help="weight on the noise-floor (TNR shortfall) term")
    parser.add_argument("--w-env", type=float, default=None, dest="w_env",
                        help="weight on the temporal-envelope term (sustain slope / release / "
                             "attack; attack / decay / crest for a drum). Unset, the weight "
                             "comes from the tone class: half for a sustained voice, where the "
                             "spectrum is the identity, and 1 for a struck, plucked or modal "
                             "one and for a drum, where the gesture is")
    parser.add_argument("--w-band", type=float, default=None, dest="w_band",
                        help="drum fits: weight on the 1/3-octave level-profile term, the "
                             "percussion analogue of the harmonic ladder")
    parser.add_argument("--w-bdecay", type=float, default=None, dest="w_bdecay",
                        help="drum fits: weight on the per-octave-band decay-slope term")
    parser.add_argument("--w-kit", type=float, default=None, dest="w_kit",
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
                             "to a single note by itself, so name the family with --notes")
    parser.add_argument("--w-init", type=float, default=None, dest="w_init",
                        help="weight on the per-harmonic ONSET-ladder term (excitation evidence)")
    parser.add_argument("--w-slope", type=float, default=None, dest="w_slope",
                        help="weight on the per-harmonic decay-slope term (loop evidence)")
    parser.add_argument("--w-tail", type=float, default=None, dest="w_tail",
                        help="weight on the per-harmonic decay slope 2-6 s in, which only has "
                             "frames to fit when the probe holds a note that long. This is "
                             "the aftersound: on a piano it is most of the note, and no "
                             "two-second probe can reach it")
    parser.add_argument("--w-hf", type=float, default=None, dest="w_hf",
                        help="weight on the attack's high-band balance, measured in 20 ms "
                             "slices over the first 120 ms. Catches a strike-noise or "
                             "excitation path whose top end is wrong for a few tens of "
                             "milliseconds — a tick, which the ear finds instantly and a "
                             "whole-timeline spectral distance averages away")
    parser.add_argument("--w-lf", type=float, default=None, dest="w_lf",
                        help="weight on the attack's low- and mid-band balance, 20 Hz to "
                             "4 kHz over one 50 ms window. The bass counterpart of --w-hf: "
                             "catches an excitation that dumps its energy below where the "
                             "instrument radiates, which reads as a note with no onset and "
                             "which every sustain-window term here scores as correct")
    parser.add_argument("--w-stiff", type=float, default=None, dest="w_stiff",
                        help="weight on STRING STIFFNESS: how far the model stretches its "
                             "twelfth partial against how far the reference does, in cents. "
                             "The ladder is now measured along each string's own partial "
                             "series, which is what makes it correct and also what leaves "
                             "the series itself unpriced — a voice twice as stiff as its "
                             "reference otherwise scores a clean sheet")
    parser.add_argument("--w-dyn", type=float, default=None, dest="w_dyn",
                        help="weight on the DYNAMICS CURVE: how brightness tracks velocity, "
                             "fitted per pitch so register is held fixed. The only term "
                             "that lives in the relation between notes rather than inside "
                             "one — a model can match every note of a grid one at a time "
                             "and still get the trend between them wrong. Needs a probe "
                             "with a velocity axis (--pattern velocity, or a drum probe)")
    parser.add_argument("--w-level", type=float, default=None, dest="w_level",
                        help="weight on the level BALANCE across the probe grid: how loud each "
                             "note is relative to the others, with the grid's own median "
                             "offset removed so an output-gain difference is not fitted. "
                             "Needs a probe with more than one note to mean anything")
    parser.add_argument("--max-level-drift-db", type=float, default=6.0,
                        dest="max_level_drift_db",
                        help="how far the winner may move the voice's whole-grid level away "
                             "from the start point before the loss charges it (default 6 dB; "
                             "0 disables). A fence rather than a term: inside it the score is "
                             "unchanged. Every other term is level-normalised, so without it a "
                             "candidate can buy a better spectrum by quietening the voice - "
                             "measured at 31 dB down with a bit-identical band profile")
    parser.add_argument("--w-crest", type=float, default=None, dest="w_crest",
                        help="weight on peak-minus-held-RMS per note. Gain-invariant, and the "
                             "one term that sees a note whose envelope never falls after its "
                             "attack — every other term here is normalised past it")
    parser.add_argument("--w-mss", type=float, default=None, dest="w_mss",
                        help="weight on the multi-scale STFT distance over the whole render "
                             "(sees what the per-note metric set does not model)")
    parser.add_argument("--w-modes", type=float, default=None, dest="w_modes",
                        help="weight on the MEASURED PARTIAL SERIES: the partials as found, "
                             "paired against the reference's by frequency, priced in cents "
                             "and dB. The harmonic ladder searches n*f0*sqrt(1+B*n^2), which "
                             "describes a stiff string and nothing else, so on a bar, a bell, "
                             "a plate or a membrane every bin above the fundamental reads the "
                             "render's own noise floor - on BOTH sides. That covers GM 8-14, "
                             "47, 55 and 112-118, and every drum note with a definite pitch. "
                             "Weighted by default for those, and available for any voice")
    parser.add_argument("--w-mod", type=float, default=None, dest="w_mod",
                        help="weight on MOVEMENT: vibrato depth and rate, tremolo, the slow "
                             "beat of an ensemble or a unison pair, and how wide the "
                             "fundamental is. A sampled reference is a recording of a player "
                             "and carries all of it; a physical model renders a still note "
                             "unless told otherwise, and every other term here reads that "
                             "stillness as cleanliness - --w-tnr charges the model only for "
                             "being NOISIER, so nothing could ever ask for vibrato")
    parser.add_argument("--flat-partial-weighting", action="store_true",
                        dest="flat_partial_weighting",
                        help="score every partial of the harmonic term equally, as this "
                             "harness did before audibility weighting. By default a partial's "
                             "vote is scaled by an A-weighting at its own frequency and by how "
                             "far it sits under the loudest partial of the same note - so A0's "
                             "27.5 Hz fundamental no longer outvotes the partials that carry "
                             "its timbre, and a 10 dB error on something 50 dB down is no "
                             "longer charged in full")
    parser.add_argument("--raw-loss", action="store_true", dest="raw_loss",
                        help="weight the terms in their own units instead of normalising "
                             "each to its value at the start point. The weights then mean "
                             "whatever the units make them mean")
    parser.add_argument("--mono-mode", default="mean", dest="mono_mode",
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
                             "without the plugin it came from")
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
