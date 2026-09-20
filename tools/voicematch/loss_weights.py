"""The term weight vocabulary and the single number a fit minimises."""

from __future__ import annotations

import math
from dataclasses import dataclass

from loss_dimensions import HARM_REACH, LOSS_TERMS, measured_terms
from toneclass import default_weights

# One perceptual unit, in each term's own raw units: 1 dB of harmonic-profile
# error, 1 cent, 1 dB of excess noise, and so on. This is the ruler every term
# is divided by, so a term's contribution says how far the model is from the
# reference in units anyone can name, and the same number means the same thing
# on every voice in the bank.
#
# `diagnose.py` has always read this table exactly that way — it divides a
# residual by the entry and calls the quotient "units" — and the objective now
# reads it the same way, which is the point: a term the diagnosis calls matched
# at one unit is a term the fit charges one unit for.
#
# **A term that sums k cells carries the k.** Half of these are per-note means
# of one number and half are sums over a ladder, a band profile or a slice
# grid, and a unit written per cell would hand every summing term an influence
# proportional to how many cells it happens to aggregate: measured across the
# bank's rendered probes, the summing terms came out 40 to 400 units where the
# averaging ones came out 1 to 15, which is the same defect as scaling by the
# start value wearing different clothes. So one unit is one step on EVERY cell
# of the aggregate, and each entry below carries the cell count it was built
# from.
TERM_UNITS = {
    # The ladder, and the same ladder extrapolated back to the onset: one dB on
    # each of `HARM_REACH` bins. `harm` weights its bins by audibility, so a
    # uniform dB of error reads under one unit there and exactly one in `init`.
    "harm": 1.0 * HARM_REACH,
    "init": 1.0 * HARM_REACH,
    "modes": 1.0, "cents": 1.0, "tnr": 1.0, "mod": 0.5,
    # Three parts — sustain slope, release, attack — each already divided by
    # roughly the amount of it that is audible, so one unit is one such step on
    # all three. A decibel per second over the probe's two-second hold is two
    # decibels of drift across the note.
    "env": 3.0,
    # Twelve cells (two decay bands over six harmonics) and six, each carrying
    # a dB/s already divided by ten, so one unit is one dB/s on every cell.
    "slope": 0.1 * 12, "tail": 0.1 * 6,
    # Thirty cells for the attack's high bands, five bands over six slices, at a
    # dB each. `lf` carries no count on purpose and is the one exception to the
    # rule above: it AVERAGES its bands rather than summing them, so that its
    # harmonic and percussion branches can share one name — see `loss.py`.
    "hf": 1.0 * 30, "lf": 1.0,
    "stiff": 1.0,
    "level": 0.5, "crest": 0.5, "dyn": 0.5,
    # The third-octave profile's twenty-five bands at a dB each, and its eight
    # octave decay rates at a tenth of a doubling each.
    "mss": 0.01, "band": 1.0 * 25, "bdecay": 0.1 * 8,
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
    if not getattr(args, "has_tail_window", True):
        # And once more on the time axis. `tail` reads a decay band that opens
        # two seconds after the onset, so a probe whose notes are shorter than
        # that reaches it with no frames in it — every cell skipped, the term
        # scoring exactly 0.0, which is its best. Dropped rather than refused
        # because the default probe is one of those; an explicit `--w-tail` gets
        # refused by the caller instead.
        weights.pop("tail", None)
    if not getattr(args, "has_velocity_spread", True):
        # The same on the other between-note axis. `dyn` fits brightness against
        # velocity per pitch, and the default `sustain` probe sounds each pitch
        # once, so there is no curve — and an unfittable `dyn` is 0.0, which is
        # also its best score, diluting every candidate equally. Dropped rather
        # than refused because fitting a `sustain` probe is an ordinary thing to
        # run; an explicit `--w-dyn` gets refused by the caller instead.
        weights.pop("dyn", None)
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


#: Which probe-shape flag each class default depends on, and what is missing
#: when it is false. A term here is dropped silently by `cli_weights` because a
#: probe that cannot fit it is an ordinary thing to run — and silence is how a
#: weight the instrument's class asked for goes missing with nothing to say so.
DROP_REASONS = {
    "kit": ("has_kit_groups", "the probe covers no whole family of the capture's"),
    "dyn": ("has_velocity_spread", "the probe sounds each pitch at one velocity"),
    "tail": ("has_tail_window", "no note is held long enough to reach the 2-6 s band"),
}


def unmeasurable_terms(args) -> list[tuple[str, str]]:
    """Terms this probe's shape produces no cell for, with why — weight aside.

    `dropped_weights` answers a question about the fit: which of the class's
    own defaults had to go. This answers one about the measurement, and the
    difference matters to a reader rather than to a run. A term nobody weighted
    still reports a residual, and a residual over no cells is 0.0, which is
    also the term's best possible value — so a report that trusts the number
    says the voice matches where nothing was compared.
    """
    return [(t, why) for t, (flag, why) in DROP_REASONS.items()
            if not getattr(args, flag, True)]


def dropped_weights(args) -> list[tuple[str, str]]:
    """Class defaults this probe cannot fit, with why — the other half of `refused_weights`.

    That one reports a weight the caller *named* and the metric set cannot
    produce. This reports one the caller never named: the instrument's class
    asked for it and the probe's shape means there is nothing to fit, so it was
    dropped rather than carried at a value that is unmeasurable and best at the
    same time. The two never overlap, since a named weight is refused instead.
    """
    if not getattr(args, "has_analysis_notes", True):
        return []       # every per-note default went at once; naming them is noise
    asked = default_weights(getattr(args, "program", 0),
                            drum_note=getattr(args, "drum_note", None),
                            percussive=getattr(args, "percussive", False))
    return [(t, why) for t, (flag, why) in DROP_REASONS.items()
            if asked.get(t, 0.0) > 0.0 and getattr(args, f"w_{t}", None) is None
            and not getattr(args, flag, True)]


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

# What a term costs once it can no longer be measured, in multiples of its own
# start value. Above 1.0 on purpose: equal to the start would make going blind
# exactly as good as changing nothing, which leaves a flat direction for a
# search to drift along, and there is no upper bound on how wrong an
# unmeasurable term might be. Not infinite either — the point is to make the
# trade unattractive, not to reject a candidate that is better everywhere else.
UNMEASURABLE_PENALTY = 2.0
#: The smallest start value the penalty is taken against, in perceptual units.
#: A term that starts already matched has a start contribution near zero, and
#: twice nothing is nothing — so going blind on it would be free.
UNMEASURABLE_MIN_UNITS = 1.0


@dataclass
class LossWeights:
    """Turns the per-term mismatch into the single number the optimiser minimises.

    `scales` is what makes the weights mean what they say. The raw terms are in
    incomparable units — the harmonic term is an L1 sum over the ladder in dB
    and runs to tens, the intonation term is a handful of cents, the multi-scale
    term is a fraction — so weighting them directly gives whichever term happens
    to be numerically largest an influence nobody chose. Each is divided by
    `TERM_UNITS`, one perceptual unit of that term, so a term's contribution is
    how far from the reference the model is in units anyone can name.

    **The scale is fixed and never the start point's own value.** Dividing a
    term by what it happened to measure at the start makes the pull of one raw
    unit `w / start`, so the dimension that starts furthest out is the one the
    objective is flattest along and the dimension already nearly right is the
    one it is steepest along — the ear reacts to the largest error and the
    objective polishes the smallest. It also leaves the reported number with no
    absolute meaning, since two voices scoring 0.85 can be arbitrarily different
    distances from their references.

    `reference` is a separate layer and a display one: the whole weighted sum is
    divided by what the start point scored, so the start reads exactly 1.0 and
    0.85 is 15 % better than the compiled-in values. It is one positive constant
    over every term, so it cannot move a term's pull relative to another's —
    which is what makes it safe to keep now that the per-term scale is fixed.

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
    #: What each term scored at the start point, in perceptual units. Read only
    #: by the unmeasurable penalty, which is defined as a multiple of the start.
    baseline_units: dict[str, float] | None = None

    def active(self) -> tuple[str, ...]:
        return tuple(t for t in LOSS_TERMS if self.weights.get(t, 0.0) > 0.0)

    def unreached(self, terms: dict[str, float]) -> list[tuple[str, float, float]]:
        """Weighted terms whose cells held no comparison, worst share first.

        Both ends of the range are in here and they look nothing alike in the
        numbers. A term whose cells were all skipped reads exactly 0.0 — its
        best score — because the reference had nothing to compare against: a
        two-second probe never reaches the `tail` band, so `tail` scores a
        perfect nothing on a voice whose class weights it. A term whose cells
        all hit the cap reads its WORST, which no empty-set guard looks for,
        and its size is the cap rather than the distance.

        Neither is a measurement, and the objective charges both as though it
        were. Reported rather than reweighted, for the reason `_went_unmeasurable`
        gives: dropping a cell nothing could compare is how an empty set comes to
        score as a match. Each entry is the term, how much of the loss it
        carries, and how many cells it had — zero meaning the reference offered
        nothing, and any other number meaning that many caps and no comparison.
        """
        out: list[tuple[str, float, float]] = []
        for name in self.active():
            cells = terms.get(f"{name}_cells")
            if cells is None or cells > terms.get(f"{name}_capped", 0.0):
                continue
            share = (self.weights[name] * terms.get(name, 0.0) / TERM_UNITS[name]
                     / (self.reference or 1.0))
            out.append((name, share, cells))
        return sorted(out, key=lambda e: -e[1])

    def calibrate(self, terms: dict[str, float]) -> None:
        """Adopt `terms` as the reference point every term is measured against."""
        self.scales = dict(TERM_UNITS)
        self.baseline_counts = {t: float(terms.get(key, 0.0))
                                for t, key in TERM_COUNT_KEYS.items()}
        self.baseline_units = {t: terms.get(t, 0.0) / TERM_UNITS[t]
                               for t in LOSS_TERMS}
        # Divide by the reference point's own score rather than by the sum of
        # the weights: the terms no longer start at one unit each, so the weight
        # sum is not what the start actually scores, and a loss whose start is
        # 37.2 rather than 1.00 is a number nobody can read a percentage off.
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
                #
                # Charged against the term's own start value, so the trade stays
                # as unattractive as it was when every term started at one unit.
                start = (self.baseline_units or {}).get(name, 0.0)
                total += w * UNMEASURABLE_PENALTY * max(start, UNMEASURABLE_MIN_UNITS)
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
