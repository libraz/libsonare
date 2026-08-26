"""What the parameters can reach, and what they cannot.

A residual is not evidence of a missing mechanism until something has failed to
reach it. Those are two different questions and they are routinely confused:

    connectivity   does ANY parameter move this measurement at all
    improvement    does any parameter move it toward the instrument

Only the first is a structural claim. Reading improvement alone inverts the
answer after a fit has been written back: every knob then sits at its own
optimum, so every measurement reports that nothing improves it, and a
well-modelled voice reads as deficient in every dimension at once.

So this decomposes the error into buckets that name a mechanism -- when in the
note, which band, and whether the energy sits on the played string's partials or
away from them -- and then asks both questions of each bucket by sweeping every
coordinate the voice declares.

The loss's own terms are buckets here too, `term.`-prefixed. The level buckets
are a decomposition of one question and there are several the arrangement of
band levels cannot state at all -- how many things are ringing, how the strike
separates from the sustain -- so without a row of their own those have no reach
column and get judged by whether the fit improved them, which is the reading
that inverts. The movement threshold is in the bucket's own units, decibels for
a level and counted resonances for a density.

A bucket no coordinate can move is the finding. It says the model has no term
for whatever produces that part of the sound, and no amount of calibration will
produce one. A bucket that many coordinates move but none improves is the
opposite finding: the mechanism is there and the fit has already spent it, or it
is being held by something else in the loss.

Two nulls that are not findings, both of which this tool can produce and neither
of which is its fault. A coordinate whose axis the corpus holds fixed reads dead
because nothing varies it, not because it does nothing. And a sweep over a
hand-chosen multiplicative range is a statement about that range, not about the
interval the code will actually accept.
"""

from __future__ import annotations

import concurrent.futures as cf

import numpy as np

from .partials import fit_inharmonicity, harmonic_rows, note_hz
from .render import write_overrides
from .terms import held_db

#: When in the note. The attack is read through the spectrogram's short scale,
#: which is the only one whose first frame is inside it.
SEGMENTS = (("attack", 0.10, 0.17, 1), ("body", 0.2, 0.8, 0),
            ("sustain", 0.8, 3.0, 0), ("tail", 3.0, 7.0, 0))
#: Bands wide enough that every bucket holds something on every note.
BANDS = (("lo", 30.0, 250.0), ("mid", 250.0, 2000.0), ("hi", 2000.0, 16000.0))
#: dB of bucket movement below which a coordinate has not touched it.
DEAD_DB = 0.3


def buckets(spectro, S, f0, B, gain_db=0.0):
    """Bucket levels for one render, in dB, keyed `segment.band.on|off`.

    The partial mask is recomputed per scale rather than resampled from one:
    the two scales have different row counts, and an interpolated mask smears a
    quarter-tone notch into whatever the ratio happens to be.
    """
    out = {}
    for seg, t0, t1, scale in SEGMENTS:
        G = S[scale]
        hz = spectro.rows_hz(scale)
        hm = harmonic_rows(hz, f0, B)
        c = spectro.columns(scale, G.shape[1], t0, t1)
        if not c.any():
            for band, _, _ in BANDS:
                out[f"{seg}.{band}.on"] = -200.0
                out[f"{seg}.{band}.off"] = -200.0
            continue
        col = (10.0 ** (G / 10.0))[:, c].mean(axis=1)
        for band, lo, hi in BANDS:
            m = (hz >= lo) & (hz < hi)
            for tag, sel in (("on", m & hm), ("off", m & ~hm)):
                out[f"{seg}.{band}.{tag}"] = \
                    10 * np.log10(max(float(col[sel].sum()), 1e-30)) - gain_db
    return out


def bucket_error(loss, overrides, notes, velocity):
    """Model minus reference per bucket, in dB, pooled over notes.

    One gain for the whole set, the same convention the loss uses, so a bucket
    error is a balance error and not a restatement of the model's loudness.
    """
    pairs = [(n, velocity) for n in notes]
    ref_sigs = loss.signals(pairs, ref=True)
    mod_sigs = loss.signals(pairs, ov=overrides)
    sr = loss.spectro.sample_rate
    g = float(np.mean([held_db(mod_sigs[k], sr=sr) - held_db(ref_sigs[k], sr=sr)
                       for k in pairs]))
    acc: dict[str, list] = {}
    for k in pairs:
        f0 = note_hz(k[0])
        B = fit_inharmonicity(ref_sigs[k], f0, sr)
        rb = buckets(loss.spectro, loss.spectro(ref_sigs[k]), f0, B)
        mb = buckets(loss.spectro, loss.spectro(mod_sigs[k]), f0, B, gain_db=g)
        for name in rb:
            # A bucket the reference itself does not occupy carries no target.
            if rb[name] > -190.0:
                acc.setdefault(name, []).append(mb[name] - rb[name])
    return {k: float(np.mean(v)) for k, v in acc.items() if v}


def term_error(loss, overrides, notes):
    """Each term of the loss as a bucket in its own right.

    The level buckets above decompose ONE question -- where the energy sits --
    and several terms ask something no arrangement of band levels can represent.
    Mode count is the sharp case: a plate with half the reference's resonances
    can hold every band level exactly, so the buckets read clean while the piece
    sounds like a tuned bar. Without a row of its own, a deficit like that has no
    reach column at all, and "no coordinate improves it" gets read off the fit
    instead -- which after a write-back is true of every knob in the voice.

    A term is already a distance from the reference, so it enters as its own
    value: zero is the target and the sign is fixed.
    """
    return {f"term.{k}": v
            for k, v in loss.score(overrides, notes=notes).parts.items()}


def measure_all(loss, overrides, notes, velocity):
    """Level buckets and loss terms together, which is what a sweep asks about.

    The level buckets are dropped for a kit. Their `on`/`off` split is a notch
    around `note_hz(n)`, and on channel 10 the note number selects an instrument
    rather than a pitch -- so the notch lands on a frequency that is not in the
    signal, and every row it produces is a reading of an arbitrary comb. The
    terms already carry a band decomposition that was written for a struck piece.
    """
    out = {} if not loss.pitched else bucket_error(loss, overrides, notes, velocity)
    out.update(term_error(loss, overrides, notes))
    return out


def reach(loss, base, moves, notes, velocity, steps=(0.8, 1.25),
          zero_ladder=(0.15, 0.4), workers=7, log=print, measure=measure_all):
    """Per bucket: how far the best coordinate moves it, and how far toward zero.

    Returns (errors, movement, best, mover) -- the error at the starting point,
    the largest absolute movement any single coordinate produced, the smallest
    absolute error any single coordinate reached, and which coordinate that was.
    """
    start = {**base, **moves}
    err0 = measure(loss, write_overrides(start, base), notes, velocity)
    names = sorted(err0)
    log(f"{len(names)} buckets, {len(base)} coordinates")

    def probe(coord):
        cur = start[coord]
        cands = list(zero_ladder) if cur == 0.0 else [cur * s for s in steps]
        out = []
        for c in cands:
            try:
                out.append(measure(
                    loss, write_overrides({**start, coord: c}, base), notes, velocity))
            except Exception:
                continue
        return coord, out

    movement = dict.fromkeys(names, 0.0)
    best = {n: abs(err0[n]) for n in names}
    mover = dict.fromkeys(names, "")
    done = 0
    with cf.ThreadPoolExecutor(max_workers=workers) as pool:
        for coord, results in pool.map(probe, sorted(base)):
            done += 1
            if done % 20 == 0:
                log(f"  {done}/{len(base)} coordinates probed")
            for e in results:
                for n in names:
                    if n not in e:
                        continue
                    d = abs(e[n] - err0[n])
                    if d > movement[n]:
                        movement[n] = d
                        mover[n] = coord
                    if abs(e[n]) < best[n]:
                        best[n] = abs(e[n])
    return err0, movement, best, mover


def report(err0, movement, best, mover):
    """The table, worst unreachable bucket first."""
    rows = []
    for n in sorted(err0):
        rows.append((movement[n] < DEAD_DB, -abs(err0[n]), n))
    rows.sort()
    out = [f"{'bucket':<18}{'error':>9}{'best knob moves it':>20}"
           f"{'best reaches':>14}   verdict / largest mover"]
    for dead, _, n in rows:
        verdict = "UNREACHABLE" if dead else (
            "reachable" if best[n] < abs(err0[n]) - DEAD_DB else "moves, no gain")
        out.append(f"{n:<18}{err0[n]:>+9.1f}{movement[n]:>20.1f}{best[n]:>14.1f}"
                   f"   {verdict:<15} {mover[n].split('.')[-1]}")
    return "\n".join(out)
