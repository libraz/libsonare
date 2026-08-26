"""Coordinate descent over the whole voice, and the two passes that follow it.

Grids are multiplicative around the value the code currently ships, which keeps
a knob's own scale out of the search's business -- a cutoff in hertz and a level
ratio get the same relative steps. A multiplicative grid cannot leave zero, and
several switches and taper depths ship at exactly zero, so a zero-valued
coordinate gets an additive ladder instead. Without it the one switch that
decides whether a string loop's upper-partial decay is designed or inherited is
unreachable, and it turned out to be the largest single move in the fit.

Split by note, not by knob. The descent sees one set of notes and never sees the
other, so a move that only works on the notes it was fitted to shows up as a
hold-out that fails to follow. A scalar fit on this corpus was caught doing
exactly that twice and both times the move was an artefact.

`ablate` and `prune` are what make the result reviewable. Descent accepts a move
against the state at the moment it was tried, which is not the state it ends in:
by the end a move may be carrying nothing, or may be compensating for a move
made after it. Reverting each one alone in the final state prices it honestly,
and keeping only those that still pay on the held-out notes has consistently
improved the hold-out while roughly halving the number of constants a change
would have to justify.
"""

from __future__ import annotations

import concurrent.futures as cf
from dataclasses import dataclass, field

from .render import write_overrides

COARSE_STEPS = (0.4, 0.6, 0.8, 1.25, 1.6, 2.5)
FINE_STEPS = (0.72, 0.85, 0.93, 1.08, 1.18, 1.4)
#: A multiplicative grid cannot leave zero, so a coordinate that ships at
#: exactly zero gets this ladder instead. It spans decades rather than the unit
#: interval because the coordinates that ship at zero are of two kinds and only
#: one of them is a switch. The other is a gain, and a gain's useful range is
#: wherever its own physics puts it: the soundboard's air-noise coefficient
#: multiplies noise at the envelope's own level, so its whole plausible range
#: lies below 0.01, and an earlier ladder starting at 0.15 tried nothing but
#: values forty decibels past the top of it. Every one was correctly rejected
#: and the rejection said nothing about the mechanism -- which, swept on its own
#: scale, closed the largest single residual in the voice at no cost.
ZERO_LADDER = (0.0, 0.001, 0.01, 0.1, 0.4, 1.0)
#: dB of cell RMS below which a move is noise rather than a fit.
ACCEPT_DB = 0.02
KEEP_DB = 0.02
#: Hold-out contributions `prune` will drop a move for, strictest first, ending
#: in `None` for "keep every move". Tried in order and each one scored, because
#: individual contributions do not add and the strictest rung routinely loses.
PRUNE_LADDER = (KEEP_DB, 0.01, 0.005, 0.0, None)


@dataclass
class Descent:
    """Parallel coordinate descent against a `ShapeLoss`."""

    loss: object
    base: dict
    fit_notes: tuple
    hold_notes: tuple
    #: Coordinates the search must not touch. Register boundaries, reference
    #: pitches and mode switches are topology, and sweeping a note number by
    #: two and a half asks the voice a question a loss should not be voting on.
    deny: set = field(default_factory=set)
    fixed: dict = field(default_factory=dict)
    steps: tuple = COARSE_STEPS
    workers: int = 7
    passes: int = 4
    log: object = print
    #: Where the hold-out is scored, when that is not the same comparison as the
    #: fit. On a keyboard the axis is the note and one loss serves both. On a kit
    #: it cannot be: every piece is its own patch, so a note the descent never
    #: saw is a note whose knobs it never touched, and the hold-out is frozen at
    #: its starting value however well the fit is going. `prune` then reads a
    #: hold-out contribution of zero for every move and drops all of them. The
    #: axis that means something there is velocity -- the same patch asked a
    #: question the fit did not pose -- which is a second loss over the same
    #: notes at the layers held back.
    hold_loss: object = None

    def evaluate(self, ov: str, notes, hold: bool = False) -> float:
        loss = (self.hold_loss or self.loss) if hold else self.loss
        try:
            return float(loss.score(ov, notes=notes).total)
        except Exception:
            return float("inf")

    def run(self, start: dict | None = None) -> dict:
        state = dict(self.base)
        state.update(start or {})
        state.update(self.fixed)
        coords = [k for k in sorted(self.base)
                  if k not in self.deny and k not in self.fixed]
        best = self.evaluate(write_overrides(state, self.base), self.fit_notes)
        h = self.evaluate(write_overrides(state, self.base), self.hold_notes, hold=True)
        self.log(f"start  fit {best:.3f}  hold {h:.3f}  ({len(coords)} coordinates)")
        pool = cf.ThreadPoolExecutor(max_workers=self.workers)
        try:
            for p in range(self.passes):
                moved = 0
                for name in coords:
                    cur = state[name]
                    cands = ([c for c in ZERO_LADDER if c != cur] if cur == 0.0
                             else [cur * s for s in self.steps])
                    futs = {pool.submit(self.evaluate,
                                        write_overrides({**state, name: c}, self.base),
                                        self.fit_notes): c for c in cands}
                    res = sorted((f.result(), futs[f]) for f in cf.as_completed(futs))
                    t, c = res[0]
                    if t < best - ACCEPT_DB:
                        state[name] = c
                        self.log(f"  p{p} {name:<44} {cur:<12.6g} -> {c:<12.6g} "
                                 f"fit {t:.3f}")
                        best, moved = t, moved + 1
                h = self.evaluate(write_overrides(state, self.base), self.hold_notes,
                                  hold=True)
                self.log(f"pass {p}: fit {best:.3f}  hold {h:.3f}  ({moved} moves)")
                if moved == 0:
                    break
        finally:
            pool.shutdown()
        return {k: v for k, v in state.items() if v != self.base.get(k)}


def ablate(loss, base: dict, moves: dict, fit_notes, hold_notes, workers: int = 7,
           hold_loss=None):
    """Price each accepted move by reverting it alone in the final state.

    Returned per move as (change in fit, change in hold-out). Positive means the
    score got worse without it, which is the move earning its place.

    `hold_loss` is where the second number comes from when the hold-out is not a
    different set of notes -- see `Descent.hold_loss`.
    """
    hl = hold_loss or loss

    def one(name):
        ov = write_overrides({**base, **moves, name: base[name]}, base)
        return (loss.score(ov, notes=fit_notes).total,
                hl.score(ov, notes=hold_notes).total)

    full = write_overrides({**base, **moves}, base)
    f0 = loss.score(full, notes=fit_notes).total
    h0 = hl.score(full, notes=hold_notes).total
    out = {}
    with cf.ThreadPoolExecutor(max_workers=workers) as pool:
        futs = {pool.submit(one, k): k for k in moves if k in base}
        for fut in cf.as_completed(futs):
            f, h = fut.result()
            out[futs[fut]] = (f - f0, h - h0)
    return out, (f0, h0)


def prune(loss, base: dict, moves: dict, fit_notes, hold_notes,
          keep_db: float = KEEP_DB, workers: int = 7, hold_loss=None):
    """Keep only the moves that still pay on notes the descent never saw.

    The hold-out and not the fit is the criterion, and the difference matters:
    a move that improves the fit while leaving the hold-out alone has learnt the
    eight notes it was shown. Verified end to end afterwards rather than assumed
    from the sum of the individual contributions, which do not add.

    That last clause is not a caveat, it is the reason for the ladder. Each move
    is priced alone in the final state, so a set of moves that each carry almost
    nothing on their own can carry a great deal together: a hi-hat fit dropped
    eighteen moves whose individual hold-out contributions were all inside two
    hundredths of a decibel and lost a quarter of a decibel of hold-out doing
    it. One threshold therefore cannot be trusted on the strength of the
    contributions it was computed from -- it has to be rendered and scored, and
    when it loses, loosened.

    So the thresholds are tried in order from the strictest, each one scored end
    to end, and the first whose hold-out is no worse than the full set's is the
    answer. The last rung keeps everything, which is always available and is the
    honest result when no smaller set holds: a smaller set of constants is worth
    having, but not at the cost of the thing they were fitted to.
    """
    scores, (f0, h0) = ablate(loss, base, moves, fit_notes, hold_notes, workers,
                              hold_loss=hold_loss)
    hl = hold_loss or loss
    attempts = []
    for thresh in PRUNE_LADDER:
        kept = (dict(moves) if thresh is None else
                {k: v for k, v in moves.items()
                 if k not in scores or scores[k][1] > thresh})
        ov = write_overrides({**base, **kept}, base)
        f = loss.score(ov, notes=fit_notes).total
        h = hl.score(ov, notes=hold_notes).total
        attempts.append({"keep_db": thresh, "kept": len(kept), "fit": f, "hold": h})
        if h <= h0 + keep_db or thresh is None:
            return kept, {
                "before": {"fit": f0, "hold": h0},
                "after": {"fit": f, "hold": h},
                "keep_db": thresh,
                "attempts": attempts,
                "dropped": sorted(set(moves) - set(kept)),
                "contributions": scores,
            }
    raise AssertionError("PRUNE_LADDER must end in None")


def split_velocities(velocities, stride: int = 2):
    """A fit set and a hold-out set of velocity layers, alternating.

    The generalisation axis for a struck kit. A held-out NOTE there is a
    different patch, so no move the descent made could reach it and the hold-out
    reports nothing; a held-out LAYER is the same patch at a dynamic the fit was
    not shown, which is the question a hold-out is for.
    """
    vs = sorted(velocities)
    if len(vs) < 2:
        return tuple(vs), tuple(vs)
    return tuple(vs[::stride]), tuple(v for i, v in enumerate(vs) if i % stride)


def split_notes(notes, stride: int = 2):
    """A fit set and a hold-out set that both span the whole range.

    Alternating rather than splitting the range in half, so neither set is a
    register. A model fitted only on the bass and held out on the treble tests
    extrapolation, which is a different and much harder question than the one
    the hold-out is asked -- whether a move generalises to notes between the
    ones it was shown.
    """
    ns = sorted(notes)
    return tuple(ns[::stride]), tuple(n for i, n in enumerate(ns) if i % stride)


def summarise(contributions, base, moves, limit: int = 0):
    """Ablation table text, worst-carrying move first."""
    rows = sorted(contributions.items(), key=lambda kv: -kv[1][1])
    if limit:
        rows = rows[:limit]
    out = [f"{'move':<46}{'dFit':>8}{'dHold':>8}   value"]
    for k, (df, dh) in rows:
        out.append(f"{k.split('.')[-1]:<46}{df:>+8.3f}{dh:>+8.3f}   "
                   f"{base[k]:g} -> {moves[k]:g}")
    return "\n".join(out)
