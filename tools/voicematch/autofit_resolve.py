"""What a run is settled to fit against, before anything is built.

The probe and the corpus behind it, the oracle combinations that are refused
outright, and the term weights the spec carries.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

import bank
import oracle_cache
from corpus import (
    PERCUSSION_CHANNEL as CORPUS_PERCUSSION_CHANNEL,
)
from corpus import (
    Corpus,
    check_against_slot_table,
    check_rig,
    corpus_from_descriptor,
    corpus_pattern,
    describe,
    load_corpus,
)
from knobs import load_spec_weights
from loop_record import committed_loop_cells
from loss import (
    DYN_MIN_VELOCITY_SPREAD,
    KIT_MIN_MEMBERS,
    LOSS_TERMS,
    SKELETON_BANDS,
    SKELETON_MAX_S,
    band_min_note_s,
    band_min_note_samples_for,
    cli_weights,
)
from metrics import measure_band_edge
from metrics_skeleton import sample_count
from patterns import build_pattern, pattern_length
from render_oracle import check_oracle_rig
from toneclass import PERCUSSION_WEIGHTS, default_weights
from writeback import adoption_refusal

HERE = Path(__file__).resolve().parent

#: Reference median tnr_db at or under which `bright` takes a default weight. It came from
#: the bank's distribution of reference median tnr (28 captures at or under it on 2026-11-13);
#: it is a trigger for a weight, not a classification of the voice.
NOISE_LED_TNR_DB = 6.0


def _score(
    program: int,
    pattern_name: str,
    notes_csv: str,
    velocities_csv: str = "",
    corpus: Corpus | None = None,
    gate_ms: int = 0,
):
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
    pattern = (
        corpus_pattern(corpus, **kwargs)
        if corpus is not None
        else build_pattern(pattern_name, program, **kwargs)
    )
    return pattern, pattern_length(pattern), pattern.analysis_notes


@dataclass(frozen=True)
class CaptureSpec:
    """One `--corpus` of a fit and the cuts that belong to it alone.

    A cut left empty falls back to the run-wide flag of the same name, so a fit
    against one capture reads exactly as it always did.
    """

    path: str
    timbre: str = ""
    notes: str = ""
    velocities: str = ""
    validate_notes: str = ""
    validate_velocities: str = ""


#: The per-capture flags, each aligned by position with `--corpus`: attribute on
#: the args, the field of `CaptureSpec` it fills, and the flag's spelling.
_ALIGNED = (
    ("corpus_timbre", "timbre", "--corpus-timbre"),
    ("corpus_notes", "notes", "--corpus-notes"),
    ("corpus_velocities", "velocities", "--corpus-velocities"),
    ("corpus_validate_notes", "validate_notes", "--corpus-validate-notes"),
    ("corpus_validate_velocities", "validate_velocities", "--corpus-validate-velocities"),
)


def _given(value) -> list[str]:
    """A flag's values as a list, whether it arrived as one string, a list or not at all."""
    if value is None:
        return []
    return [value] if isinstance(value, str) else list(value)


def capture_specs(args) -> list[CaptureSpec]:
    """The captures a run fits against, in the order they were named.

    `--corpus` may be repeated. Every per-capture flag is aligned with it by
    position and is either absent or given once per `--corpus` (`''` for "the
    default"), because a flag given for only some of them would leave it open
    which ones.
    """
    paths = [p for p in _given(getattr(args, "corpus", "")) if p]
    if not paths:
        return []
    columns: dict[str, list[str]] = {}
    for attr, field, flag in _ALIGNED:
        got = _given(getattr(args, attr, None))
        if got and len(got) != len(paths):
            raise ValueError(
                f"{flag} is given {len(got)} time(s) for {len(paths)} --corpus; give it once "
                f"per --corpus (an empty value for the default) or not at all"
            )
        columns[field] = got or [""] * len(paths)
    return [
        CaptureSpec(path, **{field: column[i] for field, column in columns.items()})
        for i, path in enumerate(paths)
    ]


def manifest_of(path: str) -> Path:
    """The manifest file a `--corpus` names: the path itself, or `manifest.json` beside its WAVs."""
    p = Path(path).expanduser()
    return p if p.suffix == ".json" else p / "manifest.json"


def corpus_present(path: str) -> bool:
    """Whether the capture manifest a `--corpus` names is on this host."""
    return manifest_of(path).exists()


def cache_role(args) -> str:
    return getattr(args, "oracle_cache_role", "fit")


def cache_file_for(args, spec: CaptureSpec) -> Path:
    """The cache `--oracle-cache` holds for one capture, timbre and role (fit or holdout)."""
    return oracle_cache.cache_file(
        args.oracle_cache, oracle_cache.capture_key(spec.path), spec.timbre, cache_role(args)
    )


def open_corpus(args, spec: CaptureSpec) -> Corpus:
    """One capture's `Corpus`: from its manifest, or from the cache's descriptor without one.

    The descriptor is checked against the whitelist and the tracked slot table
    here; whether the cache may stand in for the oracle is `oracle_reference`'s.
    """
    if not getattr(args, "oracle_cache", "") or corpus_present(spec.path):
        return load_corpus(spec.path, spec.timbre)
    path = cache_file_for(args, spec)
    desc = oracle_cache.read(path).descriptor
    check_against_slot_table(desc, desc["capture_id"])
    return corpus_from_descriptor(desc, spec.path, cache_file=path)


def resolve_corpus(args) -> Corpus | None:
    """Load the capture manifest a corpus run scores against, if there is one.

    A run against several captures has no single corpus to answer with; its
    per-capture namespaces (`capture_namespace`) each have one.
    """
    specs = capture_specs(args)
    if not specs:
        return None
    if len(specs) > 1:
        raise ValueError("this fit names several captures; resolve each through its own namespace")
    return open_corpus(args, specs[0])


def primary_corpus(args) -> Corpus | None:
    """The first capture's manifest: what a value one render shares across a joint fit is read from.

    The knob catalogue is dumped once for the whole run, at the primary
    capture's render rate; a single capture is its own primary.
    """
    specs = capture_specs(args)
    return open_corpus(args, specs[0]) if specs else None


def oracle_cache_plan(args, corpus: Corpus | None) -> dict | None:
    """What `--oracle-cache` asks of this oracle: its file, identity and direction, or None.

    The cache is read when the manifest is not on this host or `--oracle-cache-only`
    forces it, and written (replacing any earlier one) when it is.
    """
    directory = getattr(args, "oracle_cache", "")
    only = getattr(args, "oracle_cache_only", False)
    if not directory:
        for flag, given in (("only", only), ("write", getattr(args, "oracle_cache_write", False))):
            if given:
                raise ValueError(f"--oracle-cache-{flag} needs --oracle-cache DIR")
        return None
    specs = capture_specs(args)
    if corpus is None or len(specs) != 1:
        raise ValueError("--oracle-cache applies to a corpus oracle, one capture at a time")
    spec = specs[0]
    return {
        "file": cache_file_for(args, spec),
        "identity": oracle_cache.identity_of(
            args, oracle_cache.capture_key(spec.path), spec.timbre, cache_role(args)
        ),
        "read": only or corpus.cache_file is not None,
        "present": corpus_present(spec.path),
        "manifest": manifest_of(spec.path),
    }


def capture_program(corpus: Corpus) -> tuple[int, int] | None:
    """The (program, bank) a capture's definition says its recording answers, if it has one."""
    for cap in bank.captures():
        if corpus.capture_id and cap.id == corpus.capture_id:
            return cap.program, cap.bank
    return None


def capture_namespace(args, spec: CaptureSpec, *, primary: bool) -> argparse.Namespace:
    """The args one capture is fitted and held out under: the run's, with its own cuts.

    The first capture renders the run's `--program` / `--bank`. Every other one
    renders the program its own capture definition names, because a capture of a
    struct copy (a ukulele on the nylon patch) is sounded by the copy's program,
    not the fitted one's.
    """
    ns = argparse.Namespace(**vars(args))
    ns.corpus = spec.path
    ns.corpus_timbre = spec.timbre
    for attr, _, _ in _ALIGNED[1:]:
        setattr(ns, attr, None)  # folded into the fields below
    ns.notes = spec.notes or getattr(args, "notes", "")
    ns.velocities = spec.velocities or getattr(args, "velocities", "")
    ns.validate_notes = spec.validate_notes or getattr(args, "validate_notes", "")
    ns.validate_velocities = spec.validate_velocities or getattr(args, "validate_velocities", "")
    if not primary:
        named = capture_program(open_corpus(args, spec))
        if named is None:
            print(
                f"capture {spec.path}: its definition names no program, so it is rendered "
                f"as program {ns.program} bank {getattr(ns, 'bank', 0)}",
                file=sys.stderr,
            )
        else:
            ns.program, ns.bank = named
    return ns


def _committed_band_edge(corpus: Corpus | None) -> float | None:
    """What the capture's measured profile settled its ceiling at, if it has one."""
    if corpus is None or not corpus.capture_id:
        return None
    path = HERE / "reference" / f"{corpus.capture_id}.json"
    if not path.exists():
        return None
    profile = json.loads(path.read_text())
    return (profile.get("capture") or {}).get("band_edge_hz")


def window_line(corpus: Corpus, pattern) -> str:
    """The held window of one capture: how long the model sounds a note against how long the reference did."""
    model = max(n.dur for n in pattern.notes)
    return f"window: model {model:g} s / reference {corpus.gate_s:g} s"


def has_onset_reference(corpus: Corpus | None, pattern) -> bool:
    """Whether the committed profile has an onset-eligible cell on this probe's grid.

    True where there is no committed profile to ask: the term then reports its
    own compared count rather than being dropped on a guess.
    """
    if corpus is None or not corpus.capture_id:
        return True
    path = HERE / "reference" / f"{corpus.capture_id}.json"
    if not path.exists():
        return True
    cells = {(n.note, n.velocity) for n in pattern.analysis_notes}
    return any(
        r.get("timbre") == corpus.timbre
        and (r.get("note"), r.get("velocity")) in cells
        and "onset_overshoot_db" in r
        and "onset_settle_ms" in r
        for r in json.loads(path.read_text()).get("rows") or []
    )


def reference_loop_note(corpus: Corpus | None, pattern) -> str | None:
    """How many of this probe's reference rows loop and which terms they lose, or None.

    The weight is withdrawn per row (`loop_record.mark_loop_rows`), so a capture that is
    only partly looped keeps the terms on its unlooped rows; this is the line that says so.
    None where there is no committed profile or no probe row carries an exclusion.
    """
    if corpus is None or not corpus.capture_id:
        return None
    cells = committed_loop_cells(corpus.capture_id, corpus.timbre)
    probe = {(n.note, n.velocity) for n in pattern.analysis_notes}
    hit = {c: cells[c] for c in probe if cells.get(c)}
    if not hit:
        return None
    terms = sorted({t for v in hit.values() for t in v})
    return (
        f"{', '.join(terms)} carry no weight on the {len(hit)} of {len(probe)} reference rows "
        f"of {corpus.capture_id} whose loop makes them read the loop; the other rows keep them"
    )


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
        print(
            f"oracle bandwidth: "
            f"{'no measurable ceiling' if own is None else f'{own / 1000.0:.1f} kHz'} "
            f"on its own, held to {edge / 1000.0:.1f} kHz — the ceiling "
            f"{corpus.capture_id} measured across its references, which is the "
            f"one the gate scores against",
            file=sys.stderr,
        )
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
    held = (
        getattr(args, "validate_velocities", "")
        if percussive
        else getattr(args, "validate_notes", "")
    )
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
            args.fit_assessment = check_rig(
                corpus, args.program, allow=getattr(args, "allow_rigged_oracle", False)
            )
        args.pattern = "corpus"
    elif not getattr(args, "diagnose", False):
        # No capture, so no record of a rig — and the hazard is the same size.
        # What the route means by carrying no record is not the same for all
        # three of them, which is what `check_oracle_rig` sorts out.
        args.fit_assessment = check_oracle_rig(
            args, args.program, allow=getattr(args, "allow_rigged_oracle", False)
        )
    pattern, _, analysis_notes = _score(
        args.program,
        args.pattern,
        args.notes,
        args.velocities,
        corpus=corpus,
        gate_ms=getattr(args, "drum_gate_ms", 0),
    )
    # Import at call time: render_rows depends on this module's _score, while
    # the reachability check must use the same rate the renderer will use.
    from render_rows import render_rate

    sample_rate = render_rate(corpus)
    args.sample_rate = sample_rate
    if corpus is not None:
        print(describe(corpus, pattern), file=sys.stderr)
        print(window_line(corpus, pattern), file=sys.stderr)
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
    args.has_velocity_spread = any(
        len(v) >= 2 and max(v) - min(v) >= DYN_MIN_VELOCITY_SPREAD for v in velocities.values()
    )
    args.has_level_contrasts = len({(n.note, n.velocity) for n in pattern.analysis_notes}) >= 2
    args.has_onset_reference = has_onset_reference(corpus, pattern)
    args.loop_note = reference_loop_note(corpus, pattern)
    # Whether any note is held long enough for the aftersound band to have
    # frames in it. Same treatment for the fourth time, and this one is the
    # quietest of the four: `tail` reads a band that opens at 2.0 s, the default
    # probe holds 2.0 s, and a band with no frames is skipped rather than
    # charged — so the term scored exactly 0.0, its BEST value, on every
    # candidate of every fit that ever weighted it.
    tail_min_samples_by_note = [
        band_min_note_samples_for("tail_db_s", note, sample_rate, percussive=pattern.percussive)
        for note in pattern.analysis_notes
    ]
    tail_min = (
        min(tail_min_samples_by_note) / sample_rate
        if tail_min_samples_by_note
        else band_min_note_s("tail_db_s")
    )
    args.has_tail_window = any(
        min(sample_count(n.dur, sample_rate), sample_count(SKELETON_MAX_S, sample_rate)) >= required
        for n, required in zip(pattern.analysis_notes, tail_min_samples_by_note)
    )
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
    if getattr(args, "w_level", None) and not args.has_level_contrasts:
        raise ValueError(
            "--w-level needs at least two note/velocity conditions for a level contrast; "
            "removing common gain makes a single condition score zero at any level."
        )
    # The dynamics term is fitted per pitch across velocity, so a probe that
    # sounds every note once has nothing for it to fit. Refused rather than
    # scored, because its unmeasurable value is 0.0 and 0.0 is also its best
    # possible score: weighted on a `sustain` probe it would report a perfect
    # dynamics match on every candidate and quietly dilute the whole objective.
    # Only what was asked for explicitly reaches here: a class default is already
    # gone, dropped by `cli_weights` off `has_velocity_spread`.
    for flag in ("dyn", "hfdyn", "modedyn", "onsetvel", "velspan", "velcurve"):
        if getattr(args, f"w_{flag}", None) and not args.has_velocity_spread:
            spans = [max(v) - min(v) for v in velocities.values() if len(v) >= 2]
            axis_problem = (
                f"the widest per-note velocity span is {max(spans):g} steps, below the "
                f"required {DYN_MIN_VELOCITY_SPREAD:g}"
                if spans
                else "each note is sounded at a single velocity"
            )
            raise ValueError(
                f"--w-{flag} fits against velocity per pitch, and pattern "
                f"{args.pattern!r} has {axis_problem}, so there is no "
                f"curve to fit. Use --pattern velocity, a drum probe, or drop --w-{flag}."
            )
    if getattr(args, "w_onset", None) and not args.has_onset_reference:
        raise ValueError(
            "--w-onset compares each cell's onset shape, and no cell of this probe is "
            "onset-eligible in the committed reference, so there is nothing to compare. "
            "Drop --w-onset."
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
            f"in it on any note. Hold the notes past {tail_min:g} s "
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


#: What a probe says about the measurements it can take. A weight is refused or
#: dropped on the strength of these, so across several captures a measurement
#: counts only where every one of them can take it.
_PROBE_FLAGS = (
    "has_analysis_notes",
    "has_velocity_spread",
    "has_level_contrasts",
    "has_onset_reference",
    "has_tail_window",
    "has_kit_groups",
)


def resolve_probes(args, resolver=None) -> list[argparse.Namespace]:
    """Settle the probe of every capture the run names; the namespaces they fit under.

    One capture is the run itself: `args` is resolved in place and returned, so
    nothing about a single-capture fit moves. Several each get a namespace of
    their own, resolved by the same `resolve_probe`, and the run-wide `args`
    then carries what they agree on. Every capture is scored under one weight
    vector, so a term one of them cannot measure is dropped from the class
    defaults, and an explicitly asked-for one is refused, for all of them.

    `resolver` is the per-capture `resolve_probe` (a caller's seam to replace it).
    """
    resolver = resolver or resolve_probe
    specs = capture_specs(args)
    if len(specs) < 2:
        if specs:
            # Folded back to the plain single-capture shape every reader expects.
            only = specs[0]
            args.corpus, args.corpus_timbre = only.path, only.timbre
            args.notes = only.notes or getattr(args, "notes", "")
            args.velocities = only.velocities or getattr(args, "velocities", "")
            args.validate_notes = only.validate_notes or getattr(args, "validate_notes", "")
            args.validate_velocities = only.validate_velocities or getattr(
                args, "validate_velocities", ""
            )
        resolver(args)
        return [args]
    if getattr(args, "oracle_wav", ""):
        raise ValueError(
            "--corpus and --oracle-wav both name the reference; a corpus run assembles "
            "its oracle from the capture, so drop one of them"
        )
    caps = [capture_namespace(args, spec, primary=i == 0) for i, spec in enumerate(specs)]
    for ns in caps:
        resolver(ns)
    if len({ns.percussive for ns in caps}) > 1:
        raise ValueError(
            "the captures mix a kit with pitched notes; they are scored by different "
            "metric sets and cannot share one fit"
        )
    for flag in _PROBE_FLAGS:
        agreed = all(bool(getattr(ns, flag)) for ns in caps)
        for ns in (args, *caps):
            setattr(ns, flag, agreed)
    args.loop_note = "; ".join(ns.loop_note for ns in caps if getattr(ns, "loop_note", None))
    args.pattern = "corpus"
    args.percussive = caps[0].percussive
    args.sample_rate = caps[0].sample_rate
    # One assessment stands for the run, and a refusal anywhere is the one that counts.
    assessments = [getattr(ns, "fit_assessment", None) for ns in caps]
    refused = [a for a in assessments if a is not None and adoption_refusal(a) is not None]
    args.fit_assessment = (refused or assessments)[0]
    return caps


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
                f"spec {args.spec}: {term!r} is not a loss term (they are {', '.join(LOSS_TERMS)})"
            )
        flag = f"--w-{term}"
        if flag in explicit:
            continue
        setattr(args, f"w_{term}", value)
        applied.append(f"{term}={value:g}")
    if applied:
        print(f"spec weights: {' '.join(applied)}", file=sys.stderr)


def reference_median_tnr(corpus: Corpus | None) -> float | None:
    """The median `tnr_db` of the capture's committed reference rows for its timbre, if any."""
    if corpus is None or not corpus.capture_id:
        return None
    path = HERE / "reference" / f"{corpus.capture_id}.json"
    if not path.exists():
        return None
    values = [
        r["tnr_db"]
        for r in json.loads(path.read_text()).get("rows") or []
        if r.get("timbre") == corpus.timbre and r.get("tnr_db") is not None
    ]
    return statistics.median(values) if values else None


def noise_led_tnr(corpora: list[Corpus | None]) -> tuple[str, float] | None:
    """The noise-led capture with the lowest reference median tnr, and that tnr, if any is at or under the trigger.

    One weight vector scores every capture of a joint fit, so one noise-led capture
    is enough to weigh `bright` for all of them. A capture with no committed
    reference is skipped, and a kit anywhere in the list leaves the answer to its
    class defaults, which already weigh `bright`.
    """
    if not corpora or any(
        c is not None and c.channel == CORPUS_PERCUSSION_CHANNEL for c in corpora
    ):
        return None
    found = []
    for c in corpora:
        tnr = reference_median_tnr(c)
        if tnr is not None and tnr <= NOISE_LED_TNR_DB:
            found.append((tnr, c.capture_id))
    if not found:
        return None
    tnr, capture = min(found)
    return capture, tnr


def apply_noise_led_bright(args) -> None:
    """Weigh `bright` by default when any capture's reference is noise-led.

    Runs after `apply_spec_weights`, so a weight the command line or the spec gave
    (including 0) is never replaced; only an unset `bright` takes the percussion
    profile's weight. The trigger reads the reference's median tnr and says nothing
    about what the voice is, so it licenses no noise mechanism.
    """
    if getattr(args, "w_bright", None) is not None:
        return
    specs = capture_specs(args)
    if default_weights(getattr(args, "program", 0)).get("bright", 0.0) > 0.0:
        return
    led = noise_led_tnr([open_corpus(args, s) for s in specs])
    if led is None:
        return
    capture, tnr = led
    args.w_bright = PERCUSSION_WEIGHTS["bright"]
    print(
        f"bright weighted: {capture} reference tnr {tnr:.1f} dB ≤ {NOISE_LED_TNR_DB:g} "
        f"(noise-led trigger)",
        file=sys.stderr,
    )
