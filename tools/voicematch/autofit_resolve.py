"""What a run is settled to fit against, before anything is built.

The probe and the corpus behind it, the oracle combinations that are refused
outright, and the term weights the spec carries.
"""

from __future__ import annotations

import sys
from pathlib import Path

from corpus import (
    PERCUSSION_CHANNEL as CORPUS_PERCUSSION_CHANNEL,
    Corpus, check_rig, corpus_pattern, describe, load_corpus,
)
from knobs import load_spec_weights
from loss import KIT_MIN_MEMBERS, LOSS_TERMS, cli_weights
from patterns import build_pattern, pattern_length
from render_oracle import check_oracle_rig


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
