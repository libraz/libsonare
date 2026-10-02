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
  either direction. Near zero means no independently varied knob exposed by
  this probe moved that measurement. That is scoped evidence for a structural
  gap; coupled context points are checked before the report makes the claim.
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
from collections.abc import Iterable
from dataclasses import dataclass, field

from fit_quality import aggregate_term_residuals
from loss import LOSS_TERMS, TERM_UNITS, measured_terms, unmeasurable_terms

# The numerical target uses the same declared scoring units as the quality
# report. These units are a fixed ruler, not a calibrated listening threshold.
MATCHED_UNITS = 1.0

# Count a knob as connected when its sampled response exceeds this fraction
# of a declared unit. Smaller effects remain unresolved at this probe resolution.
CONNECTED_UNITS = 0.1

# What one knob has to buy, as a share of the gap above one unit, for the gap
# to count as reachable by fitting alone. A single knob closing the whole gap
# by itself is a strong statement and a rare one; most real headroom is spread
# over several, which is why the middle band exists rather than a threshold.
REACHABLE_SHARE = 0.5
PARTIAL_SHARE = 0.1

VERDICT_ORDER = (
    "measurement-limited",
    "interaction",
    "unreachable",
    "unscored",
    "spent",
    "partial",
    "reachable",
    "matched",
    "not computed",
)

MEASUREMENT_LIMITED = "measurement-limited"
# Public JSON/report spelling. The note explains that this is a joint-only
# response, while the concise verdict stays stable for downstream consumers.
JOINT_ONLY = "interaction"

TERM_UNIT_NAMES = {
    "harm": "dB",
    "cents": "cents",
    "tnr": "dB",
    "env": "composite",
    "init": "dB",
    "slope": "dB/s ÷10",
    "tail": "dB/s ÷10",
    "hf": "dB",
    "lf": "dB",
    "dyn": "dB per 64 velocity",
    "hfdyn": "dB per 64 velocity",
    "stiff": "cents",
    "level": "dB",
    "crest": "dB",
    "mss": "ratio",
    "band": "dB",
    "bdecay": "octaves of decay rate",
    "modes": "dB + cents÷25",
    "mod": "composite",
    "tilt": "dB",
    "bright": "% of the reference centroid",
    "tonal": "dB of spectral flatness",
    "rise": "dB of tilt change, strike to body",
    "strike": "dB of tilt over the first 15 ms",
    "ring": "doublings of the time to fall 20 dB",
    "kit": "doublings",
    "density": "resonances per octave band",
    "prompt": "dB of band-share change, strike to aftersound",
    "evolve": "dB of band power evolution on a common hit-power ruler",
    "diffuse": "dB of short-window spectral flatness",
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
    "hfdyn": "how the attack's 4-8 kHz share tracks velocity, per pitch",
    "stiff": "string stiffness — how far it stretches its twelfth partial",
    "level": "how the level is distributed across the probe",
    "crest": "peak against RMS",
    "mss": "the whole timeline, at four spectral resolutions",
    "band": "the third-octave level profile",
    "bdecay": "how fast each octave band dies, as a ratio to the reference's rate",
    "tilt": "which way the spectrum leans — the level above 2 kHz against the "
    "level below 500 Hz, which `band` measures the magnitude of and "
    "never the direction",
    "bright": "where the hit's energy sits, as a percentage of the reference's own centroid",
    "tonal": "whether the hit stands in lines or lies in a continuum — its spectral flatness",
    "density": "how densely resonances fill each octave of the aftersound",
    "prompt": "how each octave band's share changes from the strike to the aftersound",
    "ring": "how long the hit rings — the time it takes to fall 20 dB, in doublings",
    "rise": "whether the hit's colour moves after the strike — its tilt 30-60 ms in "
    "against its first 15 ms",
    "strike": "the colour of the strike itself — the tilt of the first 15 ms, which "
    "`rise` differences away",
    "modes": "the partials as measured — where they actually are, not where a "
    "harmonic series predicts. The only pitched reading a bar, a bell "
    "or a membrane has",
    "mod": "movement: vibrato, tremolo, and the beat of an ensemble",
    "kit": "the relations inside the kit's own families — how far the six toms "
    "spread apart, how the three hi-hats stand against each other. The "
    "one percussion reading that lives between instruments rather than "
    "inside one",
    "evolve": "how band power moves from the stick contact through body and late hit",
    "diffuse": "whether each short hit window is a sparse set of lines or a diffuse field",
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
    worst_units: float | None = None
    worst_condition: dict[str, float | None] | None = None
    measurement_reasons: tuple[str, ...] = ()
    endpoint_valid: int = 0
    endpoint_expected: int = 0
    endpoint_complete: bool = True
    joint_movers: int = 0
    measurement_limited: bool = False

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
    sampling: dict[str, object] = field(default_factory=dict)

    def to_dict(self) -> dict:
        return {
            "terms": [t.to_dict() for t in self.terms],
            "inert_knobs": list(self.inert_knobs),
            "unscorable": list(self.unscorable),
            "axes": self.axes,
            "sampling": dict(self.sampling),
        }

    def structural(self) -> list[TermVerdict]:
        """The terms whose residual no knob reaches. The reason this exists."""
        return [
            t
            for t in self.terms
            if t.verdict == "unreachable"
            and not t.measurement_limited
            and t.endpoint_complete
            and t.endpoint_expected > 0
        ]


def _unpack_probe(probe) -> tuple[str, str, dict | None, str, str]:
    """Read the legacy four-field shape and the optional context kind."""
    label, end, terms, range_source, *rest = probe
    kind = rest[0] if rest else ("joint" if str(label).startswith("context:") else "knob")
    return str(label), str(end), terms, str(range_source), kind


def _reach(
    term: str,
    base: float,
    probes: list[tuple[str, str, dict | None, str]],
    *,
    strict: bool = False,
) -> tuple[KnobReach | None, KnobReach | None, set[str], int, int, int, set[str]]:
    """Per-knob effect on one term: the best improver, the strongest mover, the count.

    Two winners rather than one, because they answer different questions and
    routinely disagree. Once a fit has converged, every knob sits at its own
    optimum and nothing improves anything — reading only the improver then
    reports the whole model as structurally deficient, which is the single way
    this measurement can lie.
    """
    by_knob: dict[str, dict[str, float]] = {}
    source: dict[str, str] = {}
    endpoint_labels: set[str] = set()
    valid_by_label: dict[str, int] = {}
    endpoint_kinds = {"lo", "hi"}
    valid_endpoints = 0
    for raw_probe in probes:
        label, end, terms, range_source, kind = _unpack_probe(raw_probe)
        if kind == "joint":
            continue
        if end in endpoint_kinds:
            endpoint_labels.add(label)
        source[label] = range_source
        if terms is None or not scorable(terms):
            continue
        value = _term_value(term, terms, base, strict=strict)
        if value is None:
            continue
        if end in endpoint_kinds:
            valid_endpoints += 1
            valid_by_label[label] = valid_by_label.get(label, 0) + 1
        by_knob.setdefault(label, {})[end] = value

    best: KnobReach | None = None
    strongest: KnobReach | None = None
    movers: set[str] = set()
    unit_size = TERM_UNITS[term]
    for label, ends in by_knob.items():
        swing = max((abs(v - base) for v in ends.values()), default=0.0)
        gains = {end: base - v for end, v in ends.items()}
        at, gain = max(gains.items(), key=lambda kv: kv[1], default=("", 0.0))
        gain = max(0.0, gain)
        reach = KnobReach(
            knob=label,
            swing=swing,
            gain=gain,
            at=at if gain > 0.0 else "",
            source=source.get(label, "auto"),
        )
        if swing >= CONNECTED_UNITS * unit_size:
            movers.add(label)
        if best is None or gain > best.gain:
            best = reach
        if strongest is None or swing > strongest.swing:
            strongest = reach
    complete_labels = {label for label, count in valid_by_label.items() if count >= 2}
    return (
        best,
        strongest,
        movers,
        len(by_knob),
        valid_endpoints,
        2 * len(endpoint_labels),
        complete_labels,
    )


def _joint_reach(
    term: str,
    base: float,
    probes: Iterable,
    *,
    strict: bool = False,
) -> tuple[float, int]:
    """Return movement from context points without counting a live knob."""
    swing = 0.0
    valid = 0
    for raw_probe in probes:
        _label, _end, terms, _source, kind = _unpack_probe(raw_probe)
        if kind != "joint" or terms is None or not scorable(terms):
            continue
        value = _term_value(term, terms, base, strict=strict)
        if value is None:
            continue
        valid += 1
        swing = max(swing, abs(value - base))
    return swing, valid


def _positive(value) -> bool:
    try:
        return math.isfinite(float(value)) and float(value) > 0.0
    except (TypeError, ValueError):
        return False


def _worst_raw(terms: dict | None, term: str) -> float | None:
    if not isinstance(terms, dict) or f"{term}_worst" not in terms:
        return None
    try:
        value = float(terms[f"{term}_worst"])
    except (TypeError, ValueError):
        return None
    return abs(value) if math.isfinite(value) else None


def _term_value(term: str, terms: dict | None, fallback: float, *, strict: bool) -> float | None:
    if terms is None or (strict and term not in terms):
        return None
    try:
        value = float(terms.get(term, fallback))
    except (TypeError, ValueError):
        return None
    if not math.isfinite(value):
        return None
    worst = _worst_raw(terms, term)
    return max(abs(value), worst or 0.0)


def _bare_measurement_reasons(terms: dict | None, term: str) -> set[str]:
    """Find explicit coverage markers without changing legacy bare terms."""
    if not isinstance(terms, dict):
        return set()
    reasons: set[str] = set()
    for suffix, reason in (
        ("capped", "one or more cells reached the comparison cap"),
        ("absent", "the reference has cells the candidate did not measure"),
        ("skipped", "one or more reference cells were skipped"),
    ):
        if _positive(terms.get(f"{term}_{suffix}")):
            reasons.add(reason)
    for suffix in ("cells", "notes", "groups", "hits", "bins"):
        key = f"{term}_{suffix}"
        if key in terms and terms.get(key) is not None and not _positive(terms.get(key)):
            reasons.add("no valid comparison cells were recorded")
    if "comparable" in terms and not _positive(terms.get("comparable")):
        reasons.add("the render produced no comparable note")
    return reasons


def _objective_measurement(
    objective,
    terms: dict | None,
    term: str,
) -> tuple[set[str], float | None, dict[str, float | None] | None]:
    """Read coverage and worst-note evidence through the quality API."""
    if objective is None:
        return set(), None, None
    try:
        rows = aggregate_term_residuals(terms or {}, objective, active=(term,), baseline_terms=None)
    except (AttributeError, KeyError, TypeError, ValueError):
        # A weight-like compatibility object has no coverage evidence.
        return set(), None, None
    row = rows.get(term)
    if not row:
        return {"the objective did not produce a coverage record"}, None, None
    reasons: set[str] = set()
    if row.get("unmeasured"):
        reasons.add("no valid comparison cells were recorded")
    if row.get("coverage_complete") is False:
        reasons.add("the objective has incomplete reference coverage")
    for key, reason in (
        ("capped", "one or more cells reached the comparison cap"),
        ("absent", "the reference has cells the candidate did not measure"),
        ("skipped", "one or more reference cells were skipped"),
    ):
        if _positive(row.get(key)):
            reasons.add(reason)
    coverage = row.get("coverage")
    if coverage is not None:
        try:
            if float(coverage) < 1.0:
                reasons.add("the candidate covers only part of the reference cells")
        except (TypeError, ValueError):
            reasons.add("the objective coverage is not finite")
    worst_units = row.get("worst_residual_units")
    try:
        worst_units = float(worst_units) if worst_units is not None else None
    except (TypeError, ValueError):
        worst_units = None
    condition = row.get("worst_condition")
    if isinstance(condition, dict):
        clean_condition = {}
        for key, value in condition.items():
            try:
                converted = float(value) if value is not None else None
            except (TypeError, ValueError):
                converted = None
            clean_condition[key] = (
                converted if converted is not None and math.isfinite(converted) else None
            )
        condition = clean_condition
    else:
        condition = None
    return reasons, worst_units, condition


def _classify(
    term: str,
    weight: float,
    residual: float,
    best: KnobReach | None,
    strongest: KnobReach | None,
    movers: int,
    *,
    effective_units: float | None = None,
    measurement_limited: bool = False,
    measurement_note: str = "",
    joint_swing: float = 0.0,
) -> tuple[str, str]:
    """The verdict for one term, and the sentence a reader is meant to act on."""
    unit_size = TERM_UNITS[term]
    units = residual / unit_size if effective_units is None else effective_units
    unit = TERM_UNIT_NAMES.get(term, "")
    if measurement_limited:
        detail = f" {measurement_note}" if measurement_note else ""
        return MEASUREMENT_LIMITED, (
            f"{residual:.3g} {unit} off, but the measurement is incomplete.{detail} "
            "Collect a complete uncapped endpoint before attributing this residual "
            "to the physical model."
        )
    if units <= MATCHED_UNITS:
        return "matched", (f"{residual:.3g} {unit} — at or under the declared numerical target.")
    gap = max(0.0, (units - MATCHED_UNITS) * unit_size)
    if weight <= 0.0:
        return "unscored", (
            f"{residual:.3g} {unit} off, and this term carries no weight, so no fit has "
            f"ever tried to close it. {movers} knobs move it. Weight it before calling it "
            f"structural."
        )
    if movers == 0 and joint_swing >= CONNECTED_UNITS * unit_size:
        return JOINT_ONLY, (
            f"{residual:.3g} {unit} off. No single knob moves it, but the sampled "
            f"joint contexts move it by {joint_swing:.3g} {unit}. Probe a coupled "
            "change before treating this as a missing mechanism."
        )
    if movers == 0:
        source = strongest.source if strongest else "auto"
        if strongest:
            where = {
                "clamp": f"over `{strongest.knob}`'s whole interval the engine accepts",
                "spec": f"over `{strongest.knob}`'s range from the spec",
            }.get(source, f"over `{strongest.knob}`'s searched range")
        else:
            where = "over the searched range"
        detail = (
            f" The largest effect any of them had was {strongest.swing:.3g} {unit}."
            if strongest
            else ""
        )
        weak = {
            "clamp": "",
            "spec": " That range was chosen by hand; check it is wide enough before "
            "concluding the mechanism is absent.",
        }.get(
            source,
            " This is a heuristic window around one default and narrow on purpose, so "
            "widen it before concluding the mechanism is absent.",
        )
        return "unreachable", (
            f"{residual:.3g} {unit} off, and no knob moves it {where}.{detail} "
            f"No independently varied knob in this probe is wired to this measurement. "
            f"Treat that as a scoped hypothesis about the model, not a guarantee.{weak}"
        )
    if best is None or best.gain <= 0.0:
        return "spent", (
            f"{residual:.3g} {unit} off. {movers} knobs move it and none reduces it from "
            f"here: either the fit has already taken what there was, or every direction "
            f"that helps costs another term. That is a trade-off to price, not a missing "
            f"mechanism."
        )
    share = (best.gain / unit_size) / (units - MATCHED_UNITS) if gap > 0.0 else 1.0
    endpoint = best.at in {"lo", "hi"}
    at_end = f" at its {best.at}" if endpoint else (f" at sampled {best.at}" if best.at else "")
    pinned = ""
    if endpoint and best.source == "clamp":
        pinned = (
            " That is the end of the interval the engine accepts, so this knob has "
            "nothing more to give."
        )
    elif endpoint:
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


def diagnose(
    base_terms: dict[str, float],
    probes: list[tuple[str, str, dict | None, str]],
    weights: dict[str, float],
    *,
    percussive: bool = False,
    axes: str = "",
    unmeasurable: Iterable[tuple[str, str]] = (),
    objective=None,
    context_probes: Iterable = (),
    sampling: dict[str, object] | None = None,
) -> Diagnosis:
    """Reduce a base render and its 2n probe renders to a verdict per term.

    Pure: `base_terms` and each probe's terms are what `score_terms` returns, and
    `probes` is one entry per (knob, range end) as
    `(knob label, "lo"|"hi", terms or None, "clamp"|"spec"|"auto")`.

    `unmeasurable` is `(term, why)` for anything this probe's shape produced no
    cell for. Those arrive as a 0.0 residual, which is also the best value a
    term has, so without it a term that compared nothing reads as matched.
    ``objective`` is optional for compatibility with old hand-built term
    dictionaries. Production runs pass ``LossWeights`` here, which lets the
    diagnosis use the same coverage, cap and worst-note evidence as the quality
    report. ``context_probes`` contains bounded all-low/all-high samples and
    can turn a one-at-a-time null into the explicit ``interaction`` verdict.
    """
    absent = dict(unmeasurable)
    out = Diagnosis(axes=axes, sampling=dict(sampling or {}))
    strict = objective is not None
    context_probes = list(context_probes)
    out.unscorable = sorted(
        f"{label}:{end}"
        for raw_probe in probes
        for label, end, terms, _source, _kind in (_unpack_probe(raw_probe),)
        if not scorable(terms)
    )
    group = set(measured_terms(percussive))
    moved_something: set[str] = set()
    valid_knobs: set[str] = set()
    uncertain_knobs: set[str] = set()
    for term in LOSS_TERMS:
        if term not in group:
            continue
        weight = float(weights.get(term, 0.0))
        residual = float(base_terms.get(term, 0.0))
        if not math.isfinite(residual):
            continue
        base_reasons = _bare_measurement_reasons(base_terms, term)
        objective_reasons, worst_units, worst_condition = _objective_measurement(
            objective, base_terms, term
        )
        reasons = base_reasons | objective_reasons
        baseline_value = max(residual, (worst_units or 0.0) * TERM_UNITS[term])
        if worst_units is None:
            baseline_value = max(baseline_value, _worst_raw(base_terms, term) or 0.0)
        if term in absent:
            out.terms.append(
                TermVerdict(
                    term=term,
                    weight=weight,
                    residual=0.0,
                    units=0.0,
                    verdict="not computed",
                    movers=0,
                    probed=0,
                    note=f"not computed — {absent[term]}, so no cell was compared "
                    f"and the 0.0 below is an absence rather than a match.",
                    worst_units=worst_units,
                    worst_condition=worst_condition,
                    measurement_reasons=tuple(sorted(reasons | {absent[term]})),
                    measurement_limited=bool(strict or reasons or term in absent),
                )
            )
            continue
        if term == "mss" and weight <= 0.0:
            # Unlike every other term, this one is not computed unless it is
            # weighted: the renders it needs the audio of are not kept. Its zero
            # is an absence, and reporting it as a match would be a lie the
            # dict's own shape invites.
            out.terms.append(
                TermVerdict(
                    term=term,
                    weight=0.0,
                    residual=0.0,
                    units=0.0,
                    verdict="not computed",
                    movers=0,
                    probed=0,
                    note="not computed — the multi-scale term needs --w-mss above zero "
                    "before the renders it compares are kept.",
                    worst_units=worst_units,
                    worst_condition=worst_condition,
                    measurement_reasons=tuple(
                        sorted(reasons | {"multi-scale audio was not retained"})
                    ),
                    measurement_limited=bool(strict or reasons),
                )
            )
            continue
        endpoint_reasons = set(reasons)
        term_probes = []
        for raw_probe in probes:
            label, end, endpoint_terms, source, kind = _unpack_probe(raw_probe)
            invalid_reasons = _bare_measurement_reasons(endpoint_terms, term)
            objective_reasons, _ignored_worst, _ignored_condition = _objective_measurement(
                objective, endpoint_terms, term
            )
            invalid_reasons |= objective_reasons
            endpoint_reasons |= invalid_reasons
            term_probes.append(
                (label, end, None if invalid_reasons else endpoint_terms, source, kind)
            )
        term_context_probes = []
        for raw_probe in context_probes:
            label, end, context_terms, source, kind = _unpack_probe(raw_probe)
            invalid_reasons = _bare_measurement_reasons(context_terms, term)
            objective_reasons, _ignored_worst, _ignored_condition = _objective_measurement(
                objective, context_terms, term
            )
            invalid_reasons |= objective_reasons
            endpoint_reasons |= invalid_reasons
            term_context_probes.append(
                (label, end, None if invalid_reasons else context_terms, source, kind)
            )
        (
            best,
            strongest,
            movers,
            probed,
            valid_endpoints,
            expected_endpoints,
            complete_labels,
        ) = _reach(term, baseline_value, term_probes, strict=strict)
        moved_something |= movers
        valid_knobs |= complete_labels
        endpoint_complete = expected_endpoints == 0 or valid_endpoints >= expected_endpoints
        if weight > 0.0:
            term_labels = {
                _unpack_probe(raw_probe)[0]
                for raw_probe in term_probes
                if _unpack_probe(raw_probe)[4] != "joint"
            }
            invalid_labels = {
                _unpack_probe(raw_probe)[0]
                for raw_probe in term_probes
                if _unpack_probe(raw_probe)[2] is None
            }
            uncertain_knobs |= invalid_labels
            if base_reasons:
                uncertain_knobs |= term_labels
            if not endpoint_complete:
                uncertain_knobs |= term_labels - complete_labels
        if strict and not endpoint_complete:
            endpoint_reasons.add(
                f"only {valid_endpoints}/{expected_endpoints} knob endpoints produced a valid term"
            )
        joint_swing, joint_valid = _joint_reach(
            term, baseline_value, term_context_probes, strict=strict
        )
        if strict and joint_valid == 0 and context_probes:
            endpoint_reasons.add("joint context samples were not scorable")
        effective_units = baseline_value / TERM_UNITS[term]
        valid_response = bool(movers) or joint_swing >= CONNECTED_UNITS * TERM_UNITS[term]
        # An invalid exploratory point cannot erase a valid baseline or response.
        measurement_limited = bool(reasons) and (strict or base_reasons)
        probe_reasons = endpoint_reasons - reasons
        if (
            not measurement_limited
            and effective_units > MATCHED_UNITS
            and probe_reasons
            and not valid_response
        ):
            measurement_limited = True
        verdict, note = _classify(
            term,
            weight,
            residual,
            best,
            strongest,
            len(movers),
            effective_units=effective_units,
            measurement_limited=measurement_limited,
            measurement_note="; ".join(sorted(endpoint_reasons)),
            joint_swing=joint_swing,
        )
        out.terms.append(
            TermVerdict(
                term=term,
                weight=weight,
                residual=residual,
                units=effective_units,
                verdict=verdict,
                movers=len(movers),
                probed=probed,
                best=best,
                strongest=strongest,
                note=note,
                worst_units=worst_units,
                worst_condition=worst_condition,
                measurement_reasons=tuple(sorted(endpoint_reasons)),
                endpoint_valid=valid_endpoints,
                endpoint_expected=expected_endpoints,
                endpoint_complete=endpoint_complete,
                joint_movers=1 if joint_swing >= CONNECTED_UNITS * TERM_UNITS[term] else 0,
                measurement_limited=measurement_limited,
            )
        )
    all_knobs = {
        label
        for raw_probe in probes
        for label, _end, _terms, _source, kind in (_unpack_probe(raw_probe),)
        if kind != "joint"
    }
    if any(term.verdict == JOINT_ONLY for term in out.terms):
        uncertain_knobs |= all_knobs
    out.inert_knobs = sorted((all_knobs & valid_knobs) - moved_something - uncertain_knobs)
    if not out.sampling:
        endpoint_count = sum(1 for raw_probe in probes if _unpack_probe(raw_probe)[4] != "joint")
        context_labels = sorted(
            {
                _unpack_probe(raw_probe)[0]
                for raw_probe in context_probes
                if _unpack_probe(raw_probe)[4] == "joint"
            }
        )
        out.sampling = {
            "initial_candidates": endpoint_count + 1 if endpoint_count else 0,
            "adaptive_candidates": 0,
            "total_candidates": endpoint_count + 1 if endpoint_count else 0,
            "contexts": context_labels,
            "adaptive": bool(context_labels),
        }
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
        print(
            f"{t.term:>7} {t.residual:>10.4g} {t.units:>7.1f} {t.weight:>5.2g} "
            f"{movers:>7}  {t.verdict}"
        )
    print(
        "\n  'units' uses the declared TERM_UNITS ruler, not a calibrated listening "
        "threshold.\n  'movers' counts knobs with a measurable response in the sampled range."
    )

    for t in rows:
        if t.verdict == "matched":
            continue
        print(f"\n  [{t.verdict}] {t.term} — {TERM_MEANS.get(t.term, t.term)}")
        print(f"    {t.note}")

    structural = diag.structural()
    measurement_limited = [t for t in rows if t.verdict == MEASUREMENT_LIMITED]
    interactions = [t for t in rows if t.verdict == JOINT_ONLY]
    unscored = [t for t in rows if t.verdict == "unscored"]
    no_probe_evidence = [
        t for t in rows if t.weight > 0.0 and (t.endpoint_expected == 0 or not t.endpoint_complete)
    ]
    if structural:
        print(
            f"\n== {len(structural)} measurement"
            f"{'s' if len(structural) > 1 else ''} nothing reaches =="
        )
        for t in structural:
            print(f"  {t.term}: {TERM_MEANS.get(t.term, t.term)}")
        print(
            "\n  This is a hypothesis to test, not a finding. A one-at-a-time probe "
            "and the bounded joint samples do not cover every combination. Check "
            "untested interactions,\n  ranges and probe axes before changing the physical model."
        )
    elif (
        measurement_limited
        or interactions
        or no_probe_evidence
        or unscored
        or any(t.verdict == "not computed" for t in rows)
    ):
        print(
            "\n  No universal reachability claim is made: some terms still need "
            "measurement evidence"
            " or a coupled probe before the residual can be attributed to physics."
        )
        if measurement_limited:
            print("  Measurement-limited: " + ", ".join(t.term for t in measurement_limited) + ".")
        if interactions:
            print("  Interaction evidence: " + ", ".join(t.term for t in interactions) + ".")
        if unscored:
            print("  Unscored terms: " + ", ".join(t.term for t in unscored) + ".")
    else:
        print(
            "\n  No structural deficiency was established by these sampled conditions. "
            "The remaining\n  residuals still need fitting and validation."
        )

    if diag.sampling:
        print(
            "\n== probe effort ==\n"
            f"  {diag.sampling.get('total_candidates', '?')} unique candidates "
            f"({diag.sampling.get('initial_candidates', '?')} initial, "
            f"{diag.sampling.get('adaptive_candidates', 0)} adaptive)"
        )
        contexts = diag.sampling.get("contexts", ())
        if contexts:
            print("  contexts: " + ", ".join(str(value) for value in contexts))

    if diag.unscorable:
        n = len(diag.unscorable)
        print(f"\n== {n} probe{'s' if n > 1 else ''} had nothing to measure ==")
        print("  " + ", ".join(diag.unscorable))
        print(
            "  That end of the range leaves the voice with no analysable note — usually a "
            "gain or\n  a level taken to zero. They are left out of every verdict above "
            "rather than\n  scored, since a silent render matches nothing and would "
            "otherwise score as a\n  perfect match. A knob probed at only one end carries "
            "half the evidence."
        )

    if diag.inert_knobs:
        print(f"\n== {len(diag.inert_knobs)} knobs move no measurement at all ==")
        for label in diag.inert_knobs:
            print(f"  {label}")
        print(
            "  A knob offered by the catalogue that this voice never reads — a rank that "
            "is off\n  in this bank, a field an engine reads only in a mode this patch "
            "does not use — or\n  one whose effect is below the probe's resolution. "
            "Dropping them from the spec\n  makes the next fit cheaper and its covariance "
            "better conditioned."
        )
        if diag.axes:
            print(
                f"\n  Before dropping any: this probe varied {diag.axes}. A knob whose "
                f"effect only\n  appears along an axis the probe holds fixed is inert "
                f"HERE and nowhere else —\n  a velocity-curve control cannot move a "
                f"single-velocity probe, and reads exactly\n  like a dead one."
            )
        print(
            "\n  These are bounded samples, including interior and joint conditions when "
            "the diagnostic\n  needed them. An untested interior value, interaction or probe "
            "axis can still\n  reveal an effect; inspect those before removing a knob."
        )

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
    return where + " and ".join(
        (count(len(notes), "one note", "notes"), count(len(vels), "one velocity", "velocities"))
    )


def run_diagnosis(evaluator, knobs, args, catalogue=None, *, out_path: str = "") -> Diagnosis:
    """Render the base point and both ends of every knob, then diagnose the terms.

    The first pass costs `2n+1` candidates, the same price as `--screen`.
    Only an incomplete or unreachable first pass earns a bounded second pass:
    the two start-to-end midpoints for each knob and two all-at-once contexts.
    That is at most `4n+3` unique candidates, so a diagnosis cannot turn into
    an unbounded pair search. Nothing is written to the source: the state being
    diagnosed is whatever the tree currently holds, so the way to diagnose a
    fitted voice is to let the fit write back and run this afterwards.
    """
    base_values = [k.start_value for k in knobs]
    max_candidates = 4 * len(knobs) + 3
    sampling: dict[str, object] = {
        "initial_candidates": 0,
        "adaptive_candidates": 0,
        "total_candidates": 0,
        "max_candidates": max_candidates,
        "contexts": [],
        "adaptive": False,
        "cache_hits": 0,
    }
    print(
        f"probing {len(knobs)} knobs at both ends ({2 * len(knobs) + 1} renders, "
        f"up to {max_candidates} with the adaptive pass)...",
        file=sys.stderr,
    )

    def key(values: list[float]):
        key_fn = getattr(evaluator, "key", None)
        if callable(key_fn):
            return key_fn(values)
        return tuple(float(value) for value in values)

    def cached(values: list[float]):
        cache = getattr(evaluator, "cache", {})
        point_key = key(values)
        if point_key in cache:
            return cache[point_key]
        return None

    sampled_keys = {key(base_values)}
    base_result = evaluator(base_values)
    base_terms = cached(base_values)
    if base_terms is None and isinstance(base_result, dict):
        # Small callers may return raw terms; production returns scalar loss.
        base_terms = base_result
    sampling["initial_candidates"] = 1
    sampling["total_candidates"] = 1
    if not scorable(base_terms):
        print(
            "the base render produced nothing measurable against the oracle; there is "
            "nothing to diagnose",
            file=sys.stderr,
        )
        return Diagnosis(sampling=sampling)

    def evaluate_unique(points: list[list[float]]) -> int:
        """Evaluate uncached points once and restore the caller's quiet flag."""

        pending: list[list[float]] = []
        pending_keys = set()
        cache = getattr(evaluator, "cache", {})
        for values in points:
            point_key = key(values)
            if point_key in cache or point_key in sampled_keys or point_key in pending_keys:
                sampling["cache_hits"] = int(sampling["cache_hits"]) + 1
                sampled_keys.add(point_key)
                continue
            pending.append(values)
            pending_keys.add(point_key)
        if not pending:
            return 0
        old_quiet = getattr(evaluator, "quiet", None)
        try:
            if old_quiet is not None:
                evaluator.quiet = True
            evaluate_batch = getattr(evaluator, "evaluate_batch", None)
            if callable(evaluate_batch):
                evaluate_batch(pending)
            else:
                for values in pending:
                    evaluator(values)
        finally:
            if old_quiet is not None:
                evaluator.quiet = old_quiet
        sampled_keys.update(pending_keys)
        return len(pending)

    trials: list[tuple[str, str, list[float]]] = []
    for i, knob in enumerate(knobs):
        for end, value in (("lo", knob.lo), ("hi", knob.hi)):
            candidate = list(base_values)
            candidate[i] = value
            trials.append((knob.label, end, candidate))
    initial_points = [trial[2] for trial in trials]
    initial_added = evaluate_unique(initial_points)
    sampling["initial_candidates"] = 1 + initial_added
    sampling["total_candidates"] = 1 + initial_added

    hand_written = getattr(args, "spec", "auto") not in ("auto", "")

    knob_by_label = {knob.label: knob for knob in knobs}

    def range_source(label: str) -> str:
        knob = knob_by_label.get(label)
        bound = catalogue.bound_for(label) if catalogue else None
        if bound is not None and knob is not None:
            blo, bhi = bound
            tolerance = 1e-9 * max(1.0, abs(blo), abs(bhi))
            if knob.lo <= blo + tolerance and knob.hi >= bhi - tolerance:
                return "clamp"
        return "spec" if hand_written else "auto"

    def trial_probes(rows):
        return [(label, end, cached(values), range_source(label)) for label, end, values in rows]

    initial_probes = trial_probes(trials)
    objective = getattr(evaluator, "loss", None)
    resolved_weights = getattr(objective, "weights", objective or {})
    base_kwargs = {
        "percussive": bool(getattr(args, "percussive", False)),
        "axes": probe_axes(getattr(evaluator, "oracle", []), getattr(args, "pattern", "")),
        "unmeasurable": unmeasurable_terms(args),
        "objective": objective,
    }
    initial_diag = diagnose(
        base_terms,
        initial_probes,
        resolved_weights,
        sampling=dict(sampling),
        **base_kwargs,
    )
    needs_adaptive = any(
        term.weight > 0.0
        and (
            term.verdict in ("unreachable", MEASUREMENT_LIMITED)
            or term.measurement_limited
            or (not term.endpoint_complete and term.units > MATCHED_UNITS)
        )
        for term in initial_diag.terms
    )
    adaptive_trials: list[tuple[str, str, list[float]]] = []
    context_trials: list[tuple[str, str, list[float]]] = []
    if needs_adaptive and knobs:
        for i, knob in enumerate(knobs):
            for end, endpoint in (("mid-lo", knob.lo), ("mid-hi", knob.hi)):
                if knob.log and knob.start_value > 0.0 and endpoint > 0.0:
                    midpoint = math.sqrt(knob.start_value * endpoint)
                else:
                    midpoint = 0.5 * (knob.start_value + endpoint)
                candidate = list(base_values)
                candidate[i] = midpoint
                adaptive_trials.append((knob.label, end, candidate))
        context_trials = [
            ("context:all-low", "joint", [knob.lo for knob in knobs]),
            ("context:all-high", "joint", [knob.hi for knob in knobs]),
        ]
        adaptive_points = [trial[2] for trial in adaptive_trials + context_trials]
        sampled_before_adaptive = set(sampled_keys)
        adaptive_added = evaluate_unique(adaptive_points)
        sampling["adaptive_candidates"] = adaptive_added
        sampling["total_candidates"] = int(sampling["total_candidates"]) + adaptive_added
        sampling["adaptive"] = bool(adaptive_added)
        new_keys = sampled_keys - sampled_before_adaptive
        context_keys = set()
        sampling["contexts"] = []
        for trial in context_trials:
            point_key = key(trial[2])
            if point_key in new_keys and point_key not in context_keys:
                sampling["contexts"].append(trial[0])
                context_keys.add(point_key)

    probes = initial_probes + trial_probes(adaptive_trials)
    context_probes = [
        (label, end, cached(values), "context", "joint") for label, end, values in context_trials
    ]
    diag = diagnose(
        base_terms,
        probes,
        resolved_weights,
        context_probes=context_probes,
        sampling=sampling,
        **base_kwargs,
    )
    print_report(diag, out_path=out_path)
    return diag
