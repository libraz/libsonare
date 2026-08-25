"""Prompt decay as a function of frequency, and whether the notes agree on one.

A piano partial does not decay at one rate. It falls quickly while the component
that moves the bridge is still there and slowly once only the component that
does not is left, and the two rates are separate quantities with separate
causes. Every scalar decay measure in this harness reports something between
them, so a voice can match the aftersound exactly and still be wrong about the
first half second -- which is the half second a listener uses to decide what the
instrument is.

The claim this module tests is that the prompt rate is a function of ABSOLUTE
frequency and not of note. That is what a termination-side loss looks like: the
bridge does not know which string is driving it, so every partial of every note
that lands at the same frequency should lose energy at the same rate. It is
falsifiable and `collapse` is the test -- bin every partial of every note by
frequency, and report how far apart the notes land inside a bin. A spread of the
same order as the curve's own range means there is no curve, and the honest
reading of that is that the prompt rate is a per-note quantity here.

Where it does collapse, the curve is a design target: a model whose prompt decay
comes from a note-relative filter cannot produce a fixed-frequency one at more
than one note, however it is calibrated.

What this module does NOT do is convert the rate into a bridge admittance. The
conversion is available -- a partial makes f0 traversals a second, so a
per-traversal loss of 2Ny becomes 17.372 * N * f0 * y dB/s -- and it is not used,
because the prompt window's rate is the sum of the bridge term and the string's
own internal and air losses, and nothing measurable here separates them. Quoting
a y would state a mechanism the measurement cannot see.
"""

from __future__ import annotations

import numpy as np

from . import partials
from .partials import Track

#: Prompt window: past the strike transient, inside the coupled component's
#: life. Opening at the attack instead would fit the hammer's own envelope.
PROMPT = (0.12, 0.5)
#: Aftersound window. Kept short at the far end because the reference's upper
#: partials plateau onto the recorded floor within a few seconds, and a rate
#: fitted across that plateau is a rate belonging to the recording.
AFTER = (1.2, 3.0)


def rates(track: Track, sig: np.ndarray, prompt=PROMPT, after=AFTER,
          *, keep_unfittable: bool = False):
    """[(frequency, prompt_db_s, after_db_s)] for every measurable partial.

    A partial is measurable when the REFERENCE still stands clear of its own
    local floor at the end of the late window; the model is read on the same
    partials whether or not its own level would qualify, so that the two sides
    are compared over the same set rather than over each side's best ones.

    That intent needs `keep_unfittable` to be honoured, and it is what the MODEL
    side must pass. `decay_db_s` refuses a window whose envelope dips under the
    floor, and the floor here is the REFERENCE's — so a None on the model side
    is not a measurement failure, it is the model having gone quiet where the
    reference is still sounding. Dropping the partial then removes it from the
    comparison instead of charging it: a voice that dies in its top two octaves
    produces a shorter table with no worse numbers in it, which reads as a
    narrower measurement rather than as the defect it is. The reference side
    keeps the drop, because there a None is a property of the recording and
    there is nothing to compare against.
    """
    out = []
    for k, f in track.ks:
        if not track.clear(k, after[1] - 0.3):
            continue
        floor = partials.level_db(track.floor(k), after[1] - 0.3) + partials.BED_CLEAR_DB
        env = track.envelope(sig, k)
        a = partials.decay_db_s(env, prompt[0], prompt[1], floor, track.sr)
        b = partials.decay_db_s(env, after[0], after[1], floor, track.sr)
        if (a is None or b is None) and not keep_unfittable:
            continue
        out.append((float(f),
                    None if a is None else float(a),
                    None if b is None else float(b)))
    return out


def bins(f_lo: float = 40.0, f_hi: float = 6000.0, per_octave: int = 2):
    n = int(round(np.log2(f_hi / f_lo) * per_octave))
    return f_lo * 2.0 ** (np.arange(n + 1) / per_octave)


def collapse(profiles: dict, which: int = 1, edges=None):
    """Do the notes agree on one curve?

    `profiles` is {key: rates(...)} over as many notes and velocities as are
    available; `which` selects the prompt rate (1) or the aftersound rate (2).
    Returns [(centre_hz, median, iqr, n_keys, n_points, n_missing)].

    A bin is dropped unless at least two different NOTES reached it. One note's
    partial ladder walking up the frequency axis would otherwise fill the whole
    table on its own and every bin would agree with itself perfectly, which is
    the one answer the test must not be able to return.

    `n_missing` counts the partials in the bin whose rate came back None — a
    model that stopped sounding there. It is reported rather than folded in
    because a median over the survivors is silent about how many there were,
    and half a bin going quiet moves that median by nothing while being the
    largest thing that happened in it.
    """
    if edges is None:
        edges = bins()
    rows = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        vals, notes, missing = [], set(), 0
        for key, prof in profiles.items():
            note = key[0] if isinstance(key, tuple) else key
            got = [p[which] for p in prof if lo <= p[0] < hi]
            if not got:
                continue
            notes.add(note)
            vals.extend(v for v in got if v is not None)
            missing += sum(1 for v in got if v is None)
        if len(vals) < 4 or len(notes) < 2:
            continue
        v = np.asarray(vals)
        q1, q3 = np.percentile(v, [25, 75])
        rows.append((float(np.sqrt(lo * hi)), float(np.median(v)), float(q3 - q1),
                     len(notes), len(vals), missing))
    return rows


def report(ref: dict, model: dict, sr: int = 48000, prompt=PROMPT, after=AFTER) -> str:
    """Both sides' prompt and aftersound curves, with the collapse test on each.

    `ref` and `model` are {(note, velocity): signal} over the same keys. The
    reference's IQR column is the one that decides whether the table means
    anything; the model/ref difference column is the design error, and it is a
    difference of rates in dB per second rather than a ratio, because a rate is
    already logarithmic.

    **The reference decides which bands appear.** Every band the reference
    answered is printed whether or not the model could answer it, and one it
    could not reads `n/a` rather than being left out. Intersecting the two sides
    was the same defect this harness has been caught in before, one level up: a
    model that went quiet across the top of its range dropped those bands from
    both the model's `collapse` and the intersection, and what printed was a
    shorter table whose every row still agreed. Absence is the finding here, and
    a comparison narrowed by the thing being measured cannot report it.
    """
    tracks = {}
    rp, mp = {}, {}
    for key in sorted(ref):
        note = key[0]
        if note not in tracks:
            tracks[note] = Track(ref[key], note, sr)
        rp[key] = rates(tracks[note], ref[key], prompt, after)
        # See `rates`: the model keeps the partials it could not sustain, so a
        # band it went quiet in is countable instead of absent.
        mp[key] = rates(tracks[note], model[key], prompt, after, keep_unfittable=True)
    lines = [f"prompt {prompt}  after {after}",
             f"{'band Hz':>9}{'notes':>6}{'pts':>5}"
             f"{'ref prompt':>12}{'iqr':>7}{'mdl prompt':>12}{'iqr':>7}{'err':>7}"
             f"{'ref after':>11}{'mdl after':>11}{'err':>7}{'quiet':>7}"]
    rc = {round(c): r for c, *r in collapse(rp, 1)}
    mc = {round(c): r for c, *r in collapse(mp, 1)}
    ra = {round(c): r for c, *r in collapse(rp, 2)}
    ma = {round(c): r for c, *r in collapse(mp, 2)}
    unanswered = []
    for c in sorted(rc):
        r = rc[c]
        cells = [f"{c:>9}{r[2]:>6}{r[3]:>5}", f"{r[0]:>12.1f}{r[1]:>7.1f}"]
        m = mc.get(c)
        if m is None:
            unanswered.append(c)
            lines.append("".join(cells) + f"{'n/a':>12}{'':>7}{'':>7}"
                                          f"{'':>11}{'':>11}{'':>7}{'all':>7}")
            continue
        cells.append(f"{m[0]:>12.1f}{m[1]:>7.1f}{m[0] - r[0]:>7.1f}")
        if c in ra and c in ma:
            cells.append(f"{ra[c][0]:>11.1f}{ma[c][0]:>11.1f}{ma[c][0] - ra[c][0]:>7.1f}")
        else:
            cells.append(f"{'':>11}{'':>11}{'':>7}")
        cells.append(f"{m[4]:>7}")
        lines.append("".join(cells))
    quiet = sum(1 for prof in mp.values() for p in prof if p[1] is None or p[2] is None)
    total = sum(len(prof) for prof in mp.values())
    lines.append("")
    lines.append(f"`quiet` is that band's partials the model let fall under the "
                 f"reference's own floor, `all` its whole width; {quiet} of {total} "
                 f"partials across the run.")
    if unanswered:
        lines.append(f"{len(unanswered)} bands the reference answered and the model did "
                     f"not at all: {', '.join(str(c) for c in unanswered)} Hz. A band "
                     f"missing from this table would have been a band nobody could see.")
    return "\n".join(lines)
