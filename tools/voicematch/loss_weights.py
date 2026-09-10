"""The term weight vocabulary and the single number a fit minimises."""

from __future__ import annotations

import math
from dataclasses import dataclass

from loss_dimensions import LOSS_TERMS, measured_terms
from toneclass import default_weights


# Smallest value a term is normalised against, in that term's own units: 1 dB of
# harmonic-profile error, 1 cent, 1 dB of excess noise, and so on. Without a
# floor a term that happens to start near zero — the noise penalty of a model
# already cleaner than its oracle is exactly zero — would divide by nothing and
# swamp every other term the moment it moved at all.
TERM_FLOORS = {
    "harm": 1.0, "modes": 1.0, "cents": 1.0, "tnr": 1.0, "mod": 0.5, "env": 0.1,
    "init": 1.0, "slope": 0.1,
    "tail": 0.1, "hf": 1.0, "lf": 1.0, "stiff": 1.0,
    "level": 0.5, "crest": 0.5, "dyn": 0.5,
    "mss": 0.01, "band": 1.0, "bdecay": 0.1,
    # A decibel of tilt and five per cent of centroid: both are quantities a
    # listener names before anything else about a kit piece, and both are well
    # inside what two takes of the same drum differ by.
    "tilt": 1.0, "bright": 5.0,
    # A tenth of a doubling: about a semitone and a half of pitch, 7 % of a
    # decay, or 0.6 dB of level. Below that the relation is inside the
    # reference's own strike-to-strike variation.
    "kit": 0.1,
}


def cli_weights(args) -> dict[str, float]:
    """The term weights a run resolves to: the instrument's defaults, then the CLI.

    Every `--w-*` flag defaults to None rather than to a number, so "not given"
    is distinguishable from "given as zero". What fills the gaps is
    `toneclass.default_weights`, which answers by what the instrument IS.

    It used to be one set of numbers for everything: `harm`, `cents` and `tnr`
    at one, and every other weight at zero. That scores a time-averaged
    spectrum, an intonation error and a noise floor — no envelope, no decay, no
    level, no attack — which is defensible for a bowed note, where the spectrum
    genuinely is most of the identity, and close to useless for a struck one,
    where the identity is the decay and the strike. A modal voice fared worse
    still: `harm` measures its noise floor (see `_modes_terms`), so the default
    weighting scored a bell almost entirely on a quantity it does not have.

    A spec's `weights` block sits between the two — the class supplies what
    neither names. `run` prints what it resolved, since a default that depends
    on the instrument is otherwise invisible.
    """
    percussive = getattr(args, "percussive", False)
    drum_note = getattr(args, "drum_note", None)
    weights = default_weights(getattr(args, "program", 0), drum_note=drum_note,
                              percussive=percussive)
    if not getattr(args, "has_analysis_notes", True):
        # A probe with nothing to analyse a note at a time — `scale`,
        # `room-probe`. Only the whole-timeline term can say anything, so the
        # class defaults are dropped rather than supplied and then refused.
        weights = {t: w for t, w in weights.items() if t == "mss"}
    if not getattr(args, "has_kit_groups", True):
        # A drum probe whose capture named no families, or one narrowed below a
        # single family's worth of notes. Same treatment as above: the class
        # default is dropped rather than supplied and then refused, because a
        # one-drum fit is an ordinary thing to run. An explicit `--w-kit` gets
        # refused by the caller instead.
        weights.pop("kit", None)
    group = set(measured_terms(percussive))
    for term in LOSS_TERMS:
        given = getattr(args, f"w_{term}", None)
        if given is not None:
            weights[term] = given
    # A term the probe's metric set does not produce is dropped rather than
    # carried at zero: `dyn` on a percussion probe is shared and stays, but a
    # `harm` weight on a drum fit would be a weight on a term that is
    # structurally 0.0, which is that term's best score.
    return {t: w for t, w in weights.items() if t in group and w > 0.0}


def refused_weights(args) -> list[str]:
    """Terms asked for on the command line that this probe cannot produce.

    `cli_weights` drops them, correctly and without a word, which is how a drum
    fit spent a round under `--w-hf 1`: `percussion_terms` never computes `hf`,
    so the weight multiplied a constant 0.0 and two runs differing only in that
    flag came back byte-identical. The flag was accepted, echoed nowhere and
    changed nothing. Only what was explicitly given is reported — the class
    defaults are filtered by the same rule on purpose.
    """
    group = set(measured_terms(getattr(args, "percussive", False)))
    return [t for t in LOSS_TERMS
            if t not in group and (getattr(args, f"w_{t}", None) or 0.0) > 0.0]


# Which key carries the count of data points each averaged term was measured
# over. Every one of these terms skips the points it cannot use and divides by
# the survivors, and every one of them already reported its count — this is the
# table that makes the counts readable by name instead of by convention.
#
# A term absent from this table is measured over a fixed set (`mss` compares
# four fixed transform sizes, `level` reads the untouched signal) and has no
# count to go to zero.
#
# `lf` is two different measurements under one name and only the percussion one
# is listed here: for a drum it is the low end of the band profile and skips the
# bands the reference floored, and for a pitched voice it is the attack's low
# bands over a fixed set, with an absent value already priced by `_absent_or`.
# The pitched set never emits `lf_notes`, so the guard reads a zero baseline
# there and correctly stays out of the way.
TERM_COUNT_KEYS = {
    "harm": "harm_bins",
    "modes": "modes_notes",
    "mod": "mod_notes",
    "stiff": "stiff_notes",
    "dyn": "dyn_groups",
    "band": "band_bins",
    "bdecay": "bdecay_bins",
    "tilt": "tilt_hits",
    "bright": "bright_hits",
    "lf": "lf_notes",
    # The one most likely to go blind of any of them: a relation is dropped
    # whenever the reference stops holding it or a member stops supplying a
    # value, so a render whose toms lost their pitch takes every tom relation
    # with it and the term would otherwise report the best score it has.
    "kit": "kit_notes",
}

# What a term costs once it can no longer be measured, in units of its own start
# value. Above 1.0 on purpose: equal to the start would make going blind exactly
# as good as changing nothing, which leaves a flat direction for a search to
# drift along, and there is no upper bound on how wrong an unmeasurable term
# might be. Not infinite either — the point is to make the trade unattractive,
# not to reject a candidate that is better everywhere else.
UNMEASURABLE_PENALTY = 2.0


@dataclass
class LossWeights:
    """Turns the per-term mismatch into the single number the optimiser minimises.

    `scales` is what makes the weights mean what they say. The raw terms are in
    incomparable units — the harmonic term is an L1 sum over ten harmonics in dB
    and runs to tens, the intonation term is a handful of cents, the multi-scale
    term is a fraction — so weighting them directly gives whichever term happens
    to be numerically largest an influence nobody chose. Dividing each by its
    value at the fit's start point makes every term start at 1, so `--w-env 2`
    genuinely means twice the pull of a unit-weighted term, and the total is
    scaled so the start point scores exactly 1.0: a reported 0.85 is 15 % better
    than the compiled-in values, whatever the units were.

    `scales` is None until the baseline has been measured (and stays None under
    `--raw-loss`), in which case the raw weighted sum is used instead.
    """

    weights: dict[str, float]
    scales: dict[str, float] | None = None
    reference: float = 1.0
    #: How many data points each averaged term was measured over at the start
    #: point. See `_weighted` — this is what lets a term that has stopped being
    #: measurable be told from one that has become perfect.
    baseline_counts: dict[str, float] | None = None

    def active(self) -> tuple[str, ...]:
        return tuple(t for t in LOSS_TERMS if self.weights.get(t, 0.0) > 0.0)

    def calibrate(self, terms: dict[str, float]) -> None:
        """Adopt `terms` as the reference point every term is measured against."""
        self.scales = {t: max(terms.get(t, 0.0), TERM_FLOORS[t]) for t in LOSS_TERMS}
        self.baseline_counts = {t: float(terms.get(key, 0.0))
                                for t, key in TERM_COUNT_KEYS.items()}
        # Divide by the reference point's own score rather than by the sum of
        # the weights: a term that starts below its unit floor contributes less
        # than 1, so the weight sum is not what the start actually scores, and a
        # loss whose start is 0.84 rather than 1.00 is a number nobody can read
        # a percentage off.
        self.reference = 1.0
        scored = self._weighted(terms)
        self.reference = scored if scored > 0.0 else 1.0

    def _weighted(self, terms: dict[str, float]) -> float:
        total = 0.0
        for name in LOSS_TERMS:
            w = self.weights.get(name, 0.0)
            if w <= 0.0:
                continue
            value = terms.get(name, 0.0)
            if self._went_unmeasurable(name, terms):
                # A term that could be measured at the start point and cannot be
                # measured here is charged its worst rather than credited its
                # best. Every averaged term skips the points it cannot use and
                # divides by the survivors, which is right until there are none:
                # then the sum is 0.0 over 0 points and 0.0 is that term's best
                # possible score. A render whose modes vanished, whose vibrato
                # stopped being detectable or whose partials fell under the
                # floor therefore reads as the render that fixed the term, and
                # a search will take that trade every time it is offered.
                #
                # Each helper already reports its own count for exactly this
                # reason; nothing read them. `loss_terms` handles the harmonic
                # ladder's version of this by refusing the whole comparison,
                # which is right when the ladder is the objective and too blunt
                # when one term of ten has gone quiet.
                total += w * UNMEASURABLE_PENALTY
                continue
            total += w * (value / self.scales[name] if self.scales else value)
        return total

    def _went_unmeasurable(self, name: str, terms: dict[str, float]) -> bool:
        """Did this term have data at the start point and have none now?"""
        if not self.baseline_counts:
            return False
        key = TERM_COUNT_KEYS.get(name)
        if key is None:
            return False
        return self.baseline_counts.get(name, 0.0) > 0.0 and terms.get(key, 0.0) <= 0.0

    def combine(self, terms: dict[str, float] | None) -> float:
        if terms is None:
            return math.inf
        total = self._weighted(terms)
        if self.scales:
            total /= self.reference
        return total if math.isfinite(total) else math.inf
