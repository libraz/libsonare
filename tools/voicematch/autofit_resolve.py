"""What a run is settled to fit against, before anything is built.

The probe and the corpus behind it, the oracle combinations that are refused
outright, and the term weights the spec carries.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from corpus import (
    PERCUSSION_CHANNEL as CORPUS_PERCUSSION_CHANNEL,
)
from corpus import (
    Corpus,
    check_rig,
    corpus_pattern,
    describe,
    load_corpus,
)
from knobs import load_spec_weights
from loss import (
    KIT_MIN_MEMBERS,
    LOSS_TERMS,
    SKELETON_BANDS,
    SKELETON_MAX_S,
    band_min_note_s,
    cli_weights,
)
from metrics import measure_band_edge
from patterns import build_pattern, pattern_length
from render_oracle import check_oracle_rig

HERE = Path(__file__).resolve().parent


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


def _committed_band_edge(corpus: Corpus | None) -> float | None:
    """What the capture's measured profile settled its ceiling at, if it has one."""
    if corpus is None or not corpus.capture_id:
        return None
    path = HERE / "reference" / f"{corpus.capture_id}.json"
    if not path.exists():
        return None
    profile = json.loads(path.read_text())
    return (profile.get("capture") or {}).get("band_edge_hz")


def reference_band_edge(corpus: Corpus | None, rows: list[dict]) -> float | None:
    """The highest 1/3-octave band this fit may be scored over, in Hz.

    A fit run scores one oracle, so the only ceiling it can measure for itself is
    `measure_band_edge` — whether that reference still tells its instruments
    apart up there. The gate scores the same voice against `shared_band_edge`,
    which also asks whether the capture's references AGREE about where the
    instruments are, and needs two recordings to ask it. The drum capture answers
    8 kHz to the first question and 5 kHz to the second, so a fit left on its own
    measurement optimises three bands the gate does not read, and closes the
    voice's filter to match a roll-off nothing holds it to.

    So the committed profile's ceiling is taken as well, and the lower wins. Not
    the profile's alone: a corpus re-captured since it was measured can be the
    narrower of the two, and a ceiling that rises because a file on disk is older
    than the audio is the failure this is here to prevent, arriving the other way
    round.
    """
    own = measure_band_edge(rows)
    committed = _committed_band_edge(corpus)
    known = [e for e in (own, committed) if e is not None]
    if not known:
        return None
    edge = min(known)
    if committed is not None and edge == committed and edge != own:
        print(f"oracle bandwidth: "
              f"{'no measurable ceiling' if own is None else f'{own / 1000.0:.1f} kHz'} "
              f"on its own, held to {edge / 1000.0:.1f} kHz — the ceiling "
              f"{corpus.capture_id} measured across its references, which is the "
              f"one the gate scores against", file=sys.stderr)
    return edge


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
    # Whether any pitch is sounded at two velocities, which is the whole of what
    # the dynamics term fits. Same treatment again, and for the same reason the
    # kit one gets it: fitting a `sustain` probe is an ordinary thing to run, so
    # the class default is dropped rather than supplied and then refused, while
    # an explicit --w-dyn is still refused below.
    velocities: dict[int, set[int]] = {}
    for n in pattern.analysis_notes:
        velocities.setdefault(n.note, set()).add(n.velocity)
    args.has_velocity_spread = any(len(v) >= 2 for v in velocities.values())
    # Whether any note is held long enough for the aftersound band to have
    # frames in it. Same treatment for the fourth time, and this one is the
    # quietest of the four: `tail` reads a band that opens at 2.0 s, the default
    # probe holds 2.0 s, and a band with no frames is skipped rather than
    # charged — so the term scored exactly 0.0, its BEST value, on every
    # candidate of every fit that ever weighted it.
    tail_min = band_min_note_s("tail_db_s")
    args.has_tail_window = any(min(n.dur, SKELETON_MAX_S) >= tail_min
                               for n in pattern.analysis_notes)
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
    # Only what was asked for explicitly reaches here: a class default is already
    # gone, dropped by `cli_weights` off `has_velocity_spread`.
    if getattr(args, "w_dyn", None) and not args.has_velocity_spread:
        raise ValueError(
            f"--w-dyn fits brightness against velocity per pitch, and pattern "
            f"{args.pattern!r} sounds each note at a single velocity, so there is no "
            f"curve to fit. Use --pattern velocity, a drum probe, or drop --w-dyn."
        )
    # The same again on the time axis, and the reason it is worth a refusal: an
    # unreachable `tail` is 0.0, which is also its best score, so a run
    # weighting it would report a perfect aftersound on every candidate.
    if getattr(args, "w_tail", None) and not args.has_tail_window:
        longest = max((n.dur for n in pattern.analysis_notes), default=0.0)
        raise ValueError(
            f"--w-tail reads the {SKELETON_BANDS['tail_db_s'][0]:g}-"
            f"{SKELETON_BANDS['tail_db_s'][1]:g} s decay band, and the longest note "
            f"pattern {args.pattern!r} holds is {longest:g} s, so the band has no frames "
            f"in it on any note. Hold the notes past {band_min_note_s('tail_db_s'):g} s "
            f"(--dur), fit against a corpus whose gate is longer, or drop --w-tail."
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

    A term the spec omits is not zero: `cli_weights` fills it from the
    instrument's class, so this block says what the class got wrong rather than
    what the run scores. Reading it as the whole vector is how a class default
    reaches a fit unargued, including one a spec's own prose argues against.

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
