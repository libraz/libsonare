"""Cutting a fit down before spending the budget on samples.

`--spec auto` offers every knob a program's patch and engine expose, which for
most programs is more than a search can use well. Two reductions apply, in this
order:

`screen_knobs` probes each knob at both ends and drops the ones that do not move
the loss, naming every one it drops. `run_stages` fits the excitation knobs
against the onset evidence and the decay knobs against the decay evidence before
fitting everything together, which breaks the ridge where a brighter excitation
and a faster decay trade against each other.

Both hand the optimisers a `SubEvaluator` — a view of the real evaluator
restricted to a subset of the knobs, so budget, cache and logging stay with the
parent and a staged fit accounts for its evaluations exactly as a plain one does.
"""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import replace

from knobs import Knob
from loss import cli_weights


class SubEvaluator:
    """A view of an `Evaluator` restricted to a subset of its knobs.

    A stage fits some of the knobs and leaves the rest at the values the
    previous stage settled on; the optimisers are written against the full knob
    vector, so the restriction lives here rather than in each of them. Budget,
    cache and logging all belong to the parent, so a staged fit accounts for its
    evaluations exactly as an unstaged one does.
    """

    def __init__(self, parent, indices: list[int], base: list[float]):
        self.parent = parent
        self.indices = indices
        self.base = list(base)

    def _expand(self, sub_values) -> list[float]:
        full = list(self.base)
        for slot, value in zip(self.indices, sub_values):
            full[slot] = value
        return full

    def _shrink(self, full: list[float]) -> list[float]:
        return [full[i] for i in self.indices]

    def __call__(self, sub_values) -> float:
        return self.parent(self._expand(sub_values))

    def evaluate_batch(self, batch) -> list[float]:
        return self.parent.evaluate_batch([self._expand(v) for v in batch])

    def restage(self, weights: dict[str, float], name: str = "fit") -> None:
        self.parent.restage(weights, name)

    @property
    def trajectory(self):
        return self.parent.trajectory

    @property
    def best_loss(self) -> float:
        return self.parent.best_loss

    @property
    def best_values(self):
        best = self.parent.best_values
        return None if best is None else self._shrink(best)

    @property
    def workers(self) -> int:
        return self.parent.workers

    @property
    def quiet(self) -> bool:
        return self.parent.quiet

    @quiet.setter
    def quiet(self, value: bool) -> None:
        self.parent.quiet = value


def screen_knobs(evaluator, knobs: list[Knob], args) -> list[int]:
    """Keep only the knobs that measurably move the loss; report the rest.

    `--spec auto` offers every knob the program's patch and engine expose, and a
    good many of them do nothing for a given voice — a feature switched off at
    its default, a field the engine reads only in a mode this patch does not
    use, a constant whose effect is below the probe's resolution. They are not
    free: CMA-ES learns a covariance whose cost grows with the square of the
    dimension, so carrying dead knobs spends the budget on modelling noise.

    Each knob is probed at both ends of its range with everything else at its
    start value, which is two renders per knob and parallelises completely. A
    knob is kept when either end moves the loss by at least
    `--screen-threshold`.

    What this cannot see: a knob that is inert on its own but not in
    combination with another. Screening therefore biases towards keeping — the
    default threshold is small — and the dropped knobs are always named, since
    a silently narrowed search reads afterwards as a search that covered
    everything.
    """
    cost = 2 * len(knobs) + 1
    if cost > args.max_evals // 3:
        print(f"screening: {cost} of {args.max_evals} evaluations go on the probe itself. "
              f"It pays for itself only when the budget is several times the knob count — "
              f"raise --max-evals or drop --screen.", file=sys.stderr)
    print(f"screening {len(knobs)} knobs ({cost} renders)...", file=sys.stderr)
    start = [k.start_value for k in knobs]
    baseline = evaluator(start)
    probes: list[list[float]] = []
    for i, knob in enumerate(knobs):
        for end in (knob.lo, knob.hi):
            trial = list(start)
            trial[i] = end
            probes.append(trial)
    evaluator.quiet = True
    losses = evaluator.evaluate_batch(probes)
    evaluator.quiet = False

    keep: list[int] = []
    dropped: list[tuple[str, float]] = []
    for i, knob in enumerate(knobs):
        pair = losses[2 * i : 2 * i + 2]
        effect = max(abs(v - baseline) for v in pair if math.isfinite(v)) if any(
            math.isfinite(v) for v in pair
        ) else 0.0
        if effect >= args.screen_threshold:
            keep.append(i)
        else:
            dropped.append((knob.label, effect))

    print(f"screening: {len(keep)}/{len(knobs)} knobs move the loss by at least "
          f"{args.screen_threshold} over their range", file=sys.stderr)
    if dropped:
        print("  dropped (largest effect first):", file=sys.stderr)
        for label, effect in sorted(dropped, key=lambda kv: -kv[1]):
            print(f"    {label}  effect={effect:.5f}", file=sys.stderr)
    if not keep:
        # Not the same event as "most knobs are inert", and it used to be
        # reported as one: the fallback below quietly restored the whole list,
        # so a spec that moved NOTHING and a spec that moved everything both
        # continued into the fit with the same knob count and the same
        # one-line message. Zero is the signature of a probe that did not
        # reach what it was aimed at — an engine switched off underneath the
        # fields being swept, or a value swept over a range the clamp rejects
        # — and it is worth more than the fit that follows it.
        biggest = max((e for _, e in dropped), default=0.0)
        raise RuntimeError(
            f"screening: 0 of {len(knobs)} knobs move the loss at all "
            f"(largest effect {biggest:.2e}, threshold {args.screen_threshold}). "
            f"The probe is not reaching these fields. Check that the engine they "
            f"belong to is switched on for this program, that their ranges are "
            f"the ones clamp_synth_patch accepts, and that the probe's pattern "
            f"exercises the axis they act on."
        )
    return keep


# Which stage a knob belongs to, by substring of its *leaf* — the field's own
# name, not the whole key. Matching the whole key would classify by the section
# it sits in: `bowed_string.bow_force` contains "ring", which would put every
# string engine's knobs in the decay stage whatever the field does.
#
# The split follows the one `skeleton_note` measures: what the excitation puts
# into the string or the air, and what the loop does with it afterwards.
# Excitation is tested first, so `click_decay_ms` — the decay of an excitation
# transient, not of the loop — lands with the excitation. A knob matching
# neither list is fitted only in the final stage, and the final stage takes
# every knob, so a misclassification costs efficiency and never reach.
STAGE_TOKENS = {
    "excitation": (
        "attack", "chiff", "strike", "pick", "pluck", "hammer", "exc_", "click", "slap",
        "breath_pressure", "breath_noise", "jet_ratio", "jet_turbulence", "lip_tension",
        "bow_force", "bow_speed", "bow_position", "vel_to", "nail", "wind_sag", "phisem",
        "noise_", "pitch_drop",
    ),
    "decay": (
        "decay", "release", "damp", "t60", "sustain", "stretch", "reflection", "shimmer",
        "ring_s", "wire",
    ),
}

# The weights each stage scores with. The final stage uses the weights given on
# the command line, so the CLI still controls the objective that decides the
# answer; the earlier stages only decide where the search starts from.
STAGE_WEIGHTS = {
    "excitation": {"init": 1.0, "harm": 1.0},
    "decay": {"slope": 1.0, "env": 1.0, "harm": 0.5},
}

# The same split against the percussion evidence. A drum has no onset ladder to
# separate the excitation with, so the excitation stage is scored on the band
# profile — which a hit's first tens of milliseconds dominate — plus the attack
# gesture, and the decay stage on the per-band slopes.
PERCUSSION_STAGE_WEIGHTS = {
    "excitation": {"band": 1.0, "env": 0.5},
    "decay": {"bdecay": 1.0, "env": 1.0, "band": 0.5},
}


def stage_weights(args) -> dict[str, dict[str, float]]:
    return PERCUSSION_STAGE_WEIGHTS if getattr(args, "percussive", False) else STAGE_WEIGHTS


def stage_of(label: str) -> str | None:
    """Which stage fits this knob first, or None for the final stage only."""
    leaf = label.rsplit(".", 1)[-1]
    for stage, tokens in STAGE_TOKENS.items():
        if any(token in leaf for token in tokens):
            return stage
    return None


def staged_indices(knobs: list[Knob], stage: str) -> list[int]:
    return [i for i, k in enumerate(knobs) if stage_of(k.label) == stage]


def run_stages(evaluator, knobs: list[Knob], args, optimizer) -> list[float]:
    """Fit the excitation, then the loop decay, then everything together.

    Fitting all of a physical voice at once asks the optimiser to separate two
    things the time-averaged spectrum conflates: how much energy the excitation
    puts into each harmonic, and how fast the loop takes it back out. A brighter
    excitation with a faster decay and a duller one with a slower decay produce
    nearly the same average spectrum, so the two trade against each other and
    the search wanders along that ridge.

    `skeleton_note` already separates the evidence — the onset ladder is the
    excitation, the decay slopes are the loop. Scoring the first group against
    only the onset evidence and the second against only the decay evidence
    breaks the trade, and the final stage then refines everything under the
    weights actually asked for, from a start point that is already in the right
    region rather than somewhere along the ridge.

    Each stage gets a share of `--max-evals` in proportion to how many knobs it
    fits, with the final stage never smaller than half the budget.
    """
    plan = [(name, staged_indices(knobs, name)) for name in ("excitation", "decay")]
    plan = [(name, idx) for name, idx in plan if len(idx) >= 2]
    if not plan:
        print("stages: nothing classified as excitation or decay — fitting in one stage",
              file=sys.stderr)
        return optimizer(evaluator, knobs, args)

    total_evals = args.max_evals
    # Half of what is *left*, not half of the total: screening may already have
    # spent most of the budget, and the final stage is the one that scores under
    # the weights the answer is judged by, so it must never be squeezed to zero.
    early_budget = max(0, total_evals - len(evaluator.trajectory)) // 2
    weighted = sum(len(idx) for _, idx in plan)
    base = [k.start_value for k in knobs]

    if early_budget < 2 * len(plan):
        print(f"stages: only {early_budget * 2} evaluations left after screening — "
              f"fitting in one stage instead", file=sys.stderr)
        return optimizer(evaluator, knobs, args)

    weights_for = stage_weights(args)
    for name, indices in plan:
        share = max(2, int(early_budget * len(indices) / weighted))
        stage_args = argparse.Namespace(**vars(args))
        stage_args.max_evals = min(total_evals, len(evaluator.trajectory) + share)
        evaluator.restage(weights_for[name], name)
        # Restarted from where the previous stage left off, via copies: the
        # originals carry the compiled-in defaults that the final report diffs
        # against, and a stage must not rewrite what "start" meant.
        sub_knobs = [replace(knobs[i], start_value=base[i]) for i in indices]
        print(f"\n== stage '{name}': {len(indices)} knobs, "
              f"{share} evaluations, weights {weights_for[name]} ==", file=sys.stderr)
        optimizer(SubEvaluator(evaluator, indices, base), sub_knobs, stage_args)
        if evaluator.best_values is not None:
            base = list(evaluator.best_values)

    print(f"\n== stage 'all': {len(knobs)} knobs, "
          f"{total_evals - len(evaluator.trajectory)} evaluations, CLI weights ==",
          file=sys.stderr)
    evaluator.restage(cli_weights(args), "all")
    return optimizer(evaluator, [replace(k, start_value=v) for k, v in zip(knobs, base)], args)
