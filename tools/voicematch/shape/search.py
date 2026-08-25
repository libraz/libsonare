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

    def evaluate(self, ov: str, notes) -> float:
        try:
            return float(self.loss.score(ov, notes=notes).total)
        except Exception:
            return float("inf")

    def run(self, start: dict | None = None) -> dict:
        state = dict(self.base)
        state.update(start or {})
        state.update(self.fixed)
        coords = [k for k in sorted(self.base)
                  if k not in self.deny and k not in self.fixed]
        best = self.evaluate(write_overrides(state, self.base), self.fit_notes)
        h = self.evaluate(write_overrides(state, self.base), self.hold_notes)
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
                h = self.evaluate(write_overrides(state, self.base), self.hold_notes)
                self.log(f"pass {p}: fit {best:.3f}  hold {h:.3f}  ({moved} moves)")
                if moved == 0:
                    break
        finally:
            pool.shutdown()
        return {k: v for k, v in state.items() if v != self.base.get(k)}


def ablate(loss, base: dict, moves: dict, fit_notes, hold_notes, workers: int = 7):
    """Price each accepted move by reverting it alone in the final state.

    Returned per move as (change in fit, change in hold-out). Positive means the
    score got worse without it, which is the move earning its place.
    """
    def one(name):
        ov = write_overrides({**base, **moves, name: base[name]}, base)
        return (loss.score(ov, notes=fit_notes).total,
                loss.score(ov, notes=hold_notes).total)

    full = write_overrides({**base, **moves}, base)
    f0 = loss.score(full, notes=fit_notes).total
    h0 = loss.score(full, notes=hold_notes).total
    out = {}
    with cf.ThreadPoolExecutor(max_workers=workers) as pool:
        futs = {pool.submit(one, k): k for k in moves if k in base}
        for fut in cf.as_completed(futs):
            f, h = fut.result()
            out[futs[fut]] = (f - f0, h - h0)
    return out, (f0, h0)


def prune(loss, base: dict, moves: dict, fit_notes, hold_notes,
          keep_db: float = KEEP_DB, workers: int = 7):
    """Keep only the moves that still pay on notes the descent never saw.

    The hold-out and not the fit is the criterion, and the difference matters:
    a move that improves the fit while leaving the hold-out alone has learnt the
    eight notes it was shown. Verified end to end afterwards rather than assumed
    from the sum of the individual contributions, which do not add.
    """
    scores, (f0, h0) = ablate(loss, base, moves, fit_notes, hold_notes, workers)
    kept = {k: v for k, v in moves.items()
            if k not in scores or scores[k][1] > keep_db}
    ov = write_overrides({**base, **kept}, base)
    return kept, {
        "before": {"fit": f0, "hold": h0},
        "after": {"fit": loss.score(ov, notes=fit_notes).total,
                  "hold": loss.score(ov, notes=hold_notes).total},
        "dropped": sorted(set(moves) - set(kept)),
        "contributions": scores,
    }


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
