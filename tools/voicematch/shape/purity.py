"""How much of a render is the played string, and how much is everything else.

"Muddy". "The attack is not clean". "There is a cymbal in the tail". Those are
one complaint measured in three places, and no term in `loss.py` states it: the
nearest, `residue`, is pooled over the whole note behind a validity gate, so a
render that is clean where the gate is open and filthy where it is shut scores
well and sounds wrong.

Here it is one number per window. The played note's partial rows are located
with its own fitted inharmonicity, energy on them is summed against energy off
them, and the ratio is reported as it stands. Nothing is subtracted and no gain
is removed, because a ratio taken inside a single render needs neither -- the
question is what fraction of THIS sound is the string, and each side answers it
for itself. That property is what makes the measure usable against material the
corpus does not contain, where there is no aligned reference to subtract.

Three things about reading it, each of which has already produced a wrong answer
here:

Average per note, not per unit of power. Pooling the powers across notes and
dividing weights the loudest notes, and the treble -- where this voice's worst
defect lives -- weighs nothing. The two conventions gave opposite signs on the
same data. `profile` returns the notes so the reader can see them disagree
instead of choosing an average that hides it.

A recorded floor is non-harmonic in its entirety, so a reference whose tail has
settled onto its session noise reports a low ratio, and a model tuned to match
the figure has been tuned to match hiss. `floor_share` prices that directly by
measuring the same note after its damper has landed. On the corpus this package
was built against the answer came back at zero to one percent, which cleared the
reference -- but it is cheap and the failure it guards against is silent.

The band matters and has to be stated. Above the top partial the mask is "off"
everywhere by construction, so a ratio taken to Nyquist is a statement about
bandwidth rather than about purity, and it will move whenever anything changes
the render's top octave.

And the number has no absolute zero. How much of the band the partial mask
covers depends on the note -- on a log-frequency axis a bass note's partials
crowd the upper rows and a treble note's do not -- so white noise put through
this does not read as 0 dB, it reads as whatever fraction of the band the mask
happens to occupy, which measured +5 dB for a middle C. Every comparison here is
therefore between two renders OF THE SAME NOTE over the same band, where that
offset is common to both sides and cancels. Comparing the figure for one note
against the figure for another says nothing.
"""

from __future__ import annotations

import numpy as np

from .partials import fit_inharmonicity, harmonic_rows, note_hz

#: Windows the complaint is usually made about. The attack is read on the short
#: scale by callers that need it; these are the coarse scale's.
WINDOWS = (("attack", 0.10, 0.25), ("body", 0.3, 1.0),
           ("sustain", 1.5, 3.0), ("tail", 3.5, 6.5))
#: Partials above this multiple of the fundamental are outside the comparison.
#: A fixed 16 kHz ceiling as well, so the top of the keyboard does not have its
#: ratio decided by the anti-alias filter.
TOP_PARTIAL = 40.0
TOP_HZ = 16000.0
#: Percent of a reference's off-partial energy that may come from its own floor
#: before the reading is a description of that floor.
FLOOR_SHARE_LIMIT = 25.0


def _split(spectro, S, note, B, window, top_partial=TOP_PARTIAL, top_hz=TOP_HZ,
           scale: int = 0):
    hz = spectro.rows_hz(scale)
    f0 = note_hz(note)
    hm = harmonic_rows(hz, f0, B)
    c = spectro.columns(scale, S.shape[1], window[0], window[1])
    if not c.any():
        return 0.0, 0.0
    col = (10.0 ** (S / 10.0))[:, c].mean(axis=1)
    band = hz < min(f0 * top_partial, top_hz)
    return float(col[hm & band].sum()), float(col[~hm & band].sum())


def purity_db(spectro, sig, note, window, B=None, **kw) -> float:
    """dB of on-partial energy over off-partial energy in one window.

    Positive is the string dominating. The inharmonicity is fitted from the
    signal itself unless supplied, so a model whose stretch is wrong is not
    additionally penalised for its partials landing off a grid fitted to the
    other side -- pass the reference's B explicitly to ask that question.
    """
    if B is None:
        B = fit_inharmonicity(sig, note_hz(note), spectro.sample_rate)
    on, off = _split(spectro, spectro(sig)[0], note, B, window, **kw)
    return 10 * np.log10(max(on, 1e-30) / max(off, 1e-30))


def profile(spectro, signals, notes, velocity, windows=WINDOWS, **kw):
    """`{window: {note: purity_db}}` -- the notes kept, not averaged.

    Deliberately not reduced. The pooled figure for this measure changed sign
    with its averaging convention on real data, which is the signature of notes
    that disagree, and the disagreement was the finding.
    """
    out: dict[str, dict[int, float]] = {}
    for name, t0, t1 in windows:
        row = {}
        for n in notes:
            sig = signals.get((n, velocity))
            if sig is None:
                continue
            row[n] = purity_db(spectro, sig, n, (t0, t1), **kw)
        out[name] = row
    return out


def floor_share(spectro, sig, note, window, floor_window, **kw) -> float:
    """Percent of the off-partial energy in `window` that the floor accounts for.

    `floor_window` is a stretch of the same recording holding nothing but its
    floor -- after the damper has landed, for a corpus note. Above
    `FLOOR_SHARE_LIMIT` the purity reading in that window is about the recording
    and not about the instrument.
    """
    B = fit_inharmonicity(sig, note_hz(note), spectro.sample_rate)
    S = spectro(sig)[0]
    _, off = _split(spectro, S, note, B, window, **kw)
    _, foff = _split(spectro, S, note, B, floor_window, **kw)
    if off <= 0.0:
        return 100.0
    return 100.0 * foff / off


def report(model_profile, ref_profile) -> str:
    """Model minus reference per note, worst window first."""
    rows = []
    for w, mrow in model_profile.items():
        rrow = ref_profile.get(w, {})
        notes = sorted(set(mrow) & set(rrow))
        if not notes:
            continue
        d = {n: mrow[n] - rrow[n] for n in notes}
        rows.append((max(abs(v) for v in d.values()), w, notes, d))
    rows.sort(reverse=True)
    if not rows:
        return "no window had notes on both sides"
    notes = rows[0][2]
    out = [f"{'window':<12}" + "".join(f"{n:>7}" for n in notes) + "   worst"]
    for worst, w, ns, d in rows:
        out.append(f"{w:<12}" + "".join(f"{d.get(n, float('nan')):>7.1f}"
                                        for n in notes) + f"{worst:>8.1f}")
    return "\n".join(out)
