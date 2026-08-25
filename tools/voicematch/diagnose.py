"""Separate what calibration can still reach from what it never will.

A fit reports one number, and that number cannot answer the question the number
raises. A loss of 0.62 says the values improved; it does not say whether the
remaining 0.62 is a few constants still slightly off, or a mechanism the voice
does not have. Those two call for opposite work — more fitting, or a physical
change to the model — and told apart by eye they are routinely mistaken for
each other, because a residual looks the same either way.

**What this measures.** Each knob is moved to both ends of its range with
everything else where it is, and the *per-term* mismatch is recorded rather
than the combined loss. That is the same 2n+1 probe `--screen` already runs;
the difference is only that screening collapses each render to one number and
throws the terms away. From those terms two independent things are read for
every measurement the metric set produces:

- **Connectivity** — the largest change any single knob makes to the term, in
  either direction. Near zero means nothing this program exposes is wired to
  that measurement. That is the structural claim, and it is the sharpest one
  available: no amount of fitting reaches a term nothing moves.
- **Improvement** — the largest *reduction* any single knob makes. A term that
  moves but does not improve is a term the fit has already spent, or one whose
  improvement costs another term. That is a trade-off, not a missing mechanism,
  and treating the two alike is the mistake this exists to stop.

**What it cannot see, and says so in its own output.** A one-at-a-time probe is
blind to a knob that is inert alone and effective in combination — the same
limitation `screen_knobs` documents, and the reason `unreachable` is reported
as a hypothesis to test rather than as a fact. It is also only as good as the
range each knob was searched over: a heuristic range around the default is
narrow on purpose, so "no effect over it" is weak evidence. A range that came
from `clamp_synth_patch` is the engine's own accepted interval, and no effect
over the WHOLE of that is strong evidence — strong enough to be worth acting on.
The report separates the two rather than averaging them into one verdict.

Terms carrying no weight are diagnosed too, and deliberately: a fit is blind to
them by construction, so a defect that lives in one has never been looked at.
"""

from __future__ import annotations

import json
import math
import sys
from dataclasses import dataclass, field

from loss import LOSS_TERMS, TERM_FLOORS, measured_terms

# Below this many multiples of its own floor, a term is as matched as the probe
# can tell. The floors are already "one unit of this term that anyone would
# notice" — 1 dB of harmonic error, 1 cent, 0.1 dB/s of slope.
MATCHED_UNITS = 1.0

# A knob counts as connected to a term when moving it over its range shifts the
# term by at least this fraction of the term's floor. A tenth of the smallest
# difference anyone would notice: generous, because the cost of calling a live
# knob dead is a mechanism removed from a model that had it, and the cost of
# calling a dead knob live is one more thing to check.
CONNECTED_UNITS = 0.1

# What one knob has to buy, as a share of the gap above the floor, for the gap
# to count as reachable by fitting alone. A single knob closing the whole gap
# by itself is a strong statement and a rare one; most real headroom is spread
# over several, which is why the middle band exists rather than a threshold.
REACHABLE_SHARE = 0.5
PARTIAL_SHARE = 0.1

VERDICT_ORDER = ("unreachable", "unscored", "spent", "partial", "reachable",
                 "matched", "not computed")

TERM_UNITS = {
    "harm": "dB", "cents": "cents", "tnr": "dB", "env": "composite",
    "init": "dB", "slope": "dB/s ÷10", "tail": "dB/s ÷10", "hf": "dB",
    "lf": "dB", "dyn": "dB per 64 velocity", "stiff": "cents",
    "level": "dB", "crest": "dB", "mss": "ratio", "band": "dB",
    "bdecay": "octaves of decay rate", "modes": "dB + cents÷25", "mod": "composite",
    "kit": "doublings",
}

TERM_MEANS = {
    "harm": "the harmonic ladder — each partial's level against the fundamental",
    "cents": "intonation",
    "tnr": "how much noise sits between the partials",
    "env": "the gesture: attack, sustain slope and release",
    "init": "the excitation spectrum, extrapolated back to the onset",
    "slope": "how fast each harmonic decays over the first two seconds",
    "tail": "the aftersound, from two seconds on",
    "hf": "the high-frequency content of the first 120 ms",
    "lf": "the low and mid bands of the first 50 ms — the attack's weight",
    "dyn": "the dynamics curve: how brightness tracks velocity, per pitch",
    "stiff": "string stiffness — how far it stretches its twelfth partial",
    "level": "how the level is distributed across the probe",
    "crest": "peak against RMS",
    "mss": "the whole timeline, at four spectral resolutions",
    "band": "the third-octave level profile",
    "bdecay": "how fast each octave band dies, as a ratio to the reference's rate",
    "modes": "the partials as measured — where they actually are, not where a "
             "harmonic series predicts. The only pitched reading a bar, a bell "
             "or a membrane has",
    "mod": "movement: vibrato, tremolo, and the beat of an ensemble",
    "kit": "the relations inside the kit's own families — how far the six toms "
           "spread apart, how the three hi-hats stand against each other. The "
           "one percussion reading that lives between instruments rather than "
           "inside one",
}


def scorable(terms: dict[str, float] | None) -> bool:
    """Whether a render produced anything the terms were measured from.

    A probe that pushed a knob to an end where the voice falls silent has no
    analysable notes, and `score_terms` reports that as every term at zero —
    which is the best score obtainable. Read literally it says the knob fixed
    everything, and since the probe deliberately visits both extremes of every
    range, that is not a rare case: it is what silencing a gain looks like.
    """
    return bool(terms) and terms.get("comparable", 1.0) > 0.0


@dataclass(frozen=True)
class KnobReach:
    """What one knob does to one term over the whole of its search range."""

    knob: str
    #: Largest change in either direction. This is the connectivity evidence.
    swing: float
    #: Largest reduction, floored at zero. This is the headroom evidence.
    gain: float
    #: Which end of the range produced `gain`, or "" when neither did.
    at: str
    #: Where the searched range came from, which is what decides how much a
    #: null result is worth: "clamp" is the whole interval the engine accepts
    #: and a null over it is strong evidence; "spec" was chosen by hand for this
    #: fit; "auto" is a heuristic window around the default and is narrow on
    #: purpose, so a null over it means very little.
    source: str


@dataclass
class TermVerdict:
    term: str
    weight: float
    residual: float
    units: float
    verdict: str
    movers: int
    probed: int
    best: KnobReach | None = None
    strongest: KnobReach | None = None
    note: str = ""

    def to_dict(self) -> dict:
        out = {k: v for k, v in self.__dict__.items() if k not in ("best", "strongest")}
        for name in ("best", "strongest"):
            reach = getattr(self, name)
            out[name] = reach.__dict__ if reach else None
        return out


@dataclass
class Diagnosis:
    terms: list[TermVerdict] = field(default_factory=list)
    #: Knobs that move nothing measurable at all, which is a different finding
    #: from a term nothing moves: it is a knob the spec should not have offered.
    inert_knobs: list[str] = field(default_factory=list)
    #: Probes whose render had nothing to measure, usually because that end of
    #: the range silences the voice. Excluded from every verdict and counted
    #: here, since a knob probed at one end carries half the evidence.
    unscorable: list[str] = field(default_factory=list)
    #: Which axes the probe varied. A knob whose effect only shows along an axis
    #: the probe holds fixed reads as inert and is not.
    axes: str = ""

    def to_dict(self) -> dict:
        return {"terms": [t.to_dict() for t in self.terms],
                "inert_knobs": list(self.inert_knobs),
                "unscorable": list(self.unscorable),
                "axes": self.axes}

    def structural(self) -> list[TermVerdict]:
        """The terms whose residual no knob reaches. The reason this exists."""
        return [t for t in self.terms if t.verdict == "unreachable"]


def _reach(term: str, base: float, probes: list[tuple[str, str, dict | None, str]]
           ) -> tuple[KnobReach | None, KnobReach | None, set[str], int]:
    """Per-knob effect on one term: the best improver, the strongest mover, the count.

    Two winners rather than one, because they answer different questions and
    routinely disagree. Once a fit has converged, every knob sits at its own
    optimum and nothing improves anything — reading only the improver then
    reports the whole model as structurally deficient, which is the single way
    this measurement can lie.
    """
    by_knob: dict[str, dict[str, float]] = {}
    source: dict[str, str] = {}
    for label, end, terms, range_source in probes:
        source[label] = range_source
        if terms is None or not scorable(terms):
            continue
        by_knob.setdefault(label, {})[end] = terms.get(term, base)

    best: KnobReach | None = None
    strongest: KnobReach | None = None
    movers: set[str] = set()
    floor = TERM_FLOORS[term]
    for label, ends in by_knob.items():
        swing = max((abs(v - base) for v in ends.values()), default=0.0)
        gains = {end: base - v for end, v in ends.items()}
        at, gain = max(gains.items(), key=lambda kv: kv[1], default=("", 0.0))
        gain = max(0.0, gain)
        reach = KnobReach(knob=label, swing=swing, gain=gain,
                          at=at if gain > 0.0 else "", source=source.get(label, "auto"))
        if swing >= CONNECTED_UNITS * floor:
            movers.add(label)
        if best is None or gain > best.gain:
            best = reach
        if strongest is None or swing > strongest.swing:
            strongest = reach
    return best, strongest, movers, len(by_knob)


def _classify(term: str, weight: float, residual: float, best: KnobReach | None,
              strongest: KnobReach | None, movers: int) -> tuple[str, str]:
    """The verdict for one term, and the sentence a reader is meant to act on."""
    floor = TERM_FLOORS[term]
    units = residual / floor
    unit = TERM_UNITS.get(term, "")
    if units <= MATCHED_UNITS:
        return "matched", (f"{residual:.3g} {unit} — at or under the smallest difference "
                           f"this term resolves.")
    gap = residual - floor
    if movers == 0:
        source = strongest.source if strongest else "auto"
        where = {
            "clamp": "over the whole interval the engine accepts",
            "spec": "over the range the spec gave them",
        }.get(source, "over the range each was searched")
        detail = (f" The largest effect any of them had was {strongest.swing:.3g} {unit}."
                  if strongest else "")
        weak = {
            "clamp": "",
            "spec": " Those ranges were chosen by hand; check they are wide enough before "
                    "concluding the mechanism is absent.",
        }.get(source,
              " Those ranges are heuristic windows around each default and narrow on "
              "purpose, so widen them before concluding the mechanism is absent.")
        return "unreachable", (
            f"{residual:.3g} {unit} off, and no knob moves it {where}.{detail} "
            f"Nothing in this program's voicing or its engine's calibration is wired to "
            f"this measurement — a fit cannot reach it, whatever budget it is given.{weak}"
        )
    if weight <= 0.0:
        return "unscored", (
            f"{residual:.3g} {unit} off, and this term carries no weight, so no fit has "
            f"ever tried to close it. {movers} knobs move it. Weight it before calling it "
            f"structural."
        )
    if best is None or best.gain <= 0.0:
        return "spent", (
            f"{residual:.3g} {unit} off. {movers} knobs move it and none reduces it from "
            f"here: either the fit has already taken what there was, or every direction "
            f"that helps costs another term. That is a trade-off to price, not a missing "
            f"mechanism."
        )
    share = best.gain / gap if gap > 0.0 else 1.0
    at_end = f" at its {best.at}" if best.at else ""
    pinned = ""
    if best.at and best.source == "clamp":
        pinned = (" That is the end of the interval the engine accepts, so this knob has "
                  "nothing more to give.")
    elif best.at:
        pinned = " That is the end of a searched range; widen it and re-probe."
    if share >= REACHABLE_SHARE:
        verdict = "reachable"
        lead = "most of this is still on the table"
    elif share >= PARTIAL_SHARE:
        verdict = "partial"
        lead = "some of this is on the table"
    else:
        verdict = "spent"
        lead = "almost none of this is on the table"
    return verdict, (
        f"{residual:.3g} {unit} off; {lead} — `{best.knob}`{at_end} buys "
        f"{best.gain:.3g} {unit} on its own, {100.0 * share:.0f} % of the gap.{pinned}"
    )


def diagnose(base_terms: dict[str, float],
             probes: list[tuple[str, str, dict | None, str]],
             weights: dict[str, float], *, percussive: bool = False,
             axes: str = "") -> Diagnosis:
    """Reduce a base render and its 2n probe renders to a verdict per term.

    Pure: `base_terms` and each probe's terms are what `score_terms` returns, and
    `probes` is one entry per (knob, range end) as
    `(knob label, "lo"|"hi", terms or None, "clamp"|"spec"|"auto")`.
    """
    out = Diagnosis(axes=axes)
    out.unscorable = sorted(f"{label}:{end}" for label, end, terms, _ in probes
                            if not scorable(terms))
    group = set(measured_terms(percussive))
    moved_something: set[str] = set()
    for term in LOSS_TERMS:
        if term not in group:
            continue
        weight = float(weights.get(term, 0.0))
        residual = float(base_terms.get(term, 0.0))
        if not math.isfinite(residual):
            continue
        if term == "mss" and weight <= 0.0:
            # Unlike every other term, this one is not computed unless it is
            # weighted: the renders it needs the audio of are not kept. Its zero
            # is an absence, and reporting it as a match would be a lie the
            # dict's own shape invites.
            out.terms.append(TermVerdict(
                term=term, weight=0.0, residual=0.0, units=0.0,
                verdict="not computed", movers=0, probed=0,
                note="not computed — the multi-scale term needs --w-mss above zero "
                     "before the renders it compares are kept.",
            ))
            continue
        best, strongest, movers, probed = _reach(term, residual, probes)
        moved_something |= movers
        verdict, note = _classify(term, weight, residual, best, strongest, len(movers))
        out.terms.append(TermVerdict(
            term=term, weight=weight, residual=residual,
            units=residual / TERM_FLOORS[term],
            verdict=verdict, movers=len(movers), probed=probed,
            best=best, strongest=strongest, note=note,
        ))
    out.inert_knobs = sorted({p[0] for p in probes} - moved_something)
    return out


def print_report(diag: Diagnosis, *, out_path: str = "") -> None:
    """The verdict, worst first, with what to do about each."""
    order = {name: i for i, name in enumerate(VERDICT_ORDER)}
    rows = sorted(diag.terms, key=lambda t: (order.get(t.verdict, 99), -t.units))

    print("\n== what the residual is made of ==")
    print(f"{'term':>7} {'residual':>10} {'units':>7} {'w':>5} {'movers':>7}  verdict")
    print("-" * 78)
    for t in rows:
        movers = f"{t.movers}/{t.probed}" if t.probed else "-"
        print(f"{t.term:>7} {t.residual:>10.4g} {t.units:>7.1f} {t.weight:>5.2g} "
              f"{movers:>7}  {t.verdict}")
    print("\n  'units' is the residual in multiples of the smallest difference the term "
          "resolves,\n  and 'movers' is how many knobs shift it at all over the range they "
          "were searched.")

    for t in rows:
        if t.verdict == "matched":
            continue
        print(f"\n  [{t.verdict}] {t.term} — {TERM_MEANS.get(t.term, t.term)}")
        print(f"    {t.note}")

    structural = diag.structural()
    if structural:
        print(f"\n== {len(structural)} measurement"
              f"{'s' if len(structural) > 1 else ''} nothing reaches ==")
        for t in structural:
            print(f"  {t.term}: {TERM_MEANS.get(t.term, t.term)}")
        print("\n  This is a hypothesis to test, not a finding. A one-at-a-time probe "
              "cannot see\n  a knob that does nothing alone and something in combination, "
              "so confirm it by\n  adding the mechanism and watching the term move — and "
              "if it does not, the\n  mechanism was not the missing one either.")
    else:
        unscored = [t.term for t in rows if t.verdict == "unscored"]
        print("\n  Every measurement is reachable by something. Whatever is left is a "
              "matter of\n  values, weights or budget rather than of physics.")
        if unscored:
            print(f"  Start with {', '.join(unscored)}: those carry no weight, so the "
                  f"residual in them\n  is headroom no fit has ever been pointed at.")

    if diag.unscorable:
        n = len(diag.unscorable)
        print(f"\n== {n} probe{'s' if n > 1 else ''} had nothing to measure ==")
        print("  " + ", ".join(diag.unscorable))
        print("  That end of the range leaves the voice with no analysable note — usually a "
              "gain or\n  a level taken to zero. They are left out of every verdict above "
              "rather than\n  scored, since a silent render matches nothing and would "
              "otherwise score as a\n  perfect match. A knob probed at only one end carries "
              "half the evidence.")

    if diag.inert_knobs:
        print(f"\n== {len(diag.inert_knobs)} knobs move no measurement at all ==")
        for label in diag.inert_knobs:
            print(f"  {label}")
        print("  A knob offered by the catalogue that this voice never reads — a rank that "
              "is off\n  in this bank, a field an engine reads only in a mode this patch "
              "does not use — or\n  one whose effect is below the probe's resolution. "
              "Dropping them from the spec\n  makes the next fit cheaper and its covariance "
              "better conditioned.")
        if diag.axes:
            print(f"\n  Before dropping any: this probe varied {diag.axes}. A knob whose "
                  f"effect only\n  appears along an axis the probe holds fixed is inert "
                  f"HERE and nowhere else —\n  a velocity-curve control cannot move a "
                  f"single-velocity probe, and reads exactly\n  like a dead one.")

    if out_path:
        from pathlib import Path
        Path(out_path).write_text(json.dumps(diag.to_dict(), indent=2) + "\n")
        print(f"\nDiagnosis written to {out_path}")


def probe_axes(oracle_rows: list[dict], pattern: str = "") -> str:
    """What the probe actually varies, counted off the rows that were scored.

    Reported because an inert knob and a knob the probe cannot exercise produce
    the identical measurement. The `sustain` pattern holds velocity fixed, so
    every dynamics control on the instrument reads dead against it — and on a
    harpsichord, whose whole identity is what happens across velocity, that is
    the one axis worth probing.

    Counted from the oracle's own rows rather than from the command line: the
    note list is usually left to the pattern to choose, so the flags are empty
    on exactly the runs where this matters.
    """
    notes = {r.get("note") for r in oracle_rows if r.get("note") is not None}
    vels = {r.get("velocity") for r in oracle_rows if r.get("velocity") is not None}

    def count(n: int, one: str, many: str) -> str:
        return one if n == 1 else f"{n} {many}"

    where = f"the {pattern} pattern over " if pattern else ""
    return where + " and ".join((count(len(notes), "one note", "notes"),
                                 count(len(vels), "one velocity", "velocities")))


def run_diagnosis(evaluator, knobs, args, catalogue=None, *, out_path: str = "") -> Diagnosis:
    """Render the base point and both ends of every knob, then diagnose the terms.

    Costs `2n+1` renders and parallelises completely, which is the same price as
    `--screen`. Nothing is written to the source: the state being diagnosed is
    whatever the tree currently holds, so the way to diagnose a fitted voice is
    to let the fit write back and run this afterwards.
    """
    base_values = [k.start_value for k in knobs]
    print(f"probing {len(knobs)} knobs at both ends ({2 * len(knobs) + 1} renders)...",
          file=sys.stderr)
    evaluator(base_values)
    base_terms = evaluator.cache.get(evaluator.key(base_values))
    if not scorable(base_terms):
        print("the base render produced nothing measurable against the oracle; there is "
              "nothing to diagnose", file=sys.stderr)
        return Diagnosis()

    trials: list[tuple[str, str, list[float]]] = []
    for i, knob in enumerate(knobs):
        for end, value in (("lo", knob.lo), ("hi", knob.hi)):
            candidate = list(base_values)
            candidate[i] = value
            trials.append((knob.label, end, candidate))
    evaluator.quiet = True
    evaluator.evaluate_batch([t[2] for t in trials])
    evaluator.quiet = False

    # Where each knob's range came from decides what a null result over it is
    # worth, so it is carried rather than assumed. The clamp bound wins wherever
    # the library reported one, since that is the interval the engine will
    # actually accept whatever the spec asked for.
    hand_written = getattr(args, "spec", "auto") not in ("auto", "")

    def range_source(label: str) -> str:
        if catalogue and catalogue.bound_for(label):
            return "clamp"
        return "spec" if hand_written else "auto"

    probes = [(label, end, evaluator.cache.get(evaluator.key(values)), range_source(label))
              for label, end, values in trials]
    diag = diagnose(base_terms, probes, evaluator.loss.weights,
                    percussive=bool(getattr(args, "percussive", False)),
                    axes=probe_axes(evaluator.oracle, getattr(args, "pattern", "")))
    print_report(diag, out_path=out_path)
    return diag
