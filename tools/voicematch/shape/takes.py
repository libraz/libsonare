"""Phrase takes: the material a single-note corpus cannot contain.

Every other module here measures one note struck alone. That is the right shape
for timbre and it is the wrong shape for everything an instrument does when more
than one string is moving -- the bloom of a held chord, what a pedal adds, what
happens when the same string is struck again before it has stopped. A corpus of
single notes cannot show any of it, and a fit against that corpus cannot produce
it: not because the search failed, but because the question was never put.

The audition renderer already plays those phrases through both the model and the
reference plugin. This reads what it wrote. Three measures, each shaped by a
mistake made while taking it by hand first:

`band_error` puts one gain on a take, measured over a body window, and reports
the remaining per-band difference in a later one. Without the gain the table is
a restatement of loudness; with it, "the total is right and the contents are
tilted" becomes readable, which is what thinness is.

`window_for` exists because a window placed by eye lands in the wrong place. The
pedal take's late window was first put after the pedal lifts, which measures the
damper rather than the resonance -- the opposite mechanism, at the opposite end
of the same take.

`relative_to` compares each side against ITS OWN rendering of a simpler take, so
whatever a side gets wrong about how one note decays cancels and what is left is
what the phrase added. That is the only way to read a repeated-note take: the
raw level difference cannot separate "eight strikes stacked up" from "one strike
decays wrong", and the ratio can.

Nothing here knows what a piano is. The takes, their windows and the reference's
name come from the audition manifest.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np

#: Octave-ish bands, matching the loss's balance term so the two are readable
#: against each other.
BANDS = ((30, 60), (60, 125), (125, 250), (250, 500), (500, 1000),
         (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000))
#: dB a band must stand over the take's own tail floor to be worth reading.
SNR_DB = 10.0


def load(directory) -> dict:
    """`{take_id: {source: (samples, sample_rate)}}` plus `_reference`.

    The reference source is whichever one is not `model`; a model-only page has
    none, and callers that need one should say so rather than compare a render
    with itself.
    """
    from wavio import read_wav

    d = Path(directory)
    man = json.load(open(d / "manifest.json"))
    others = [s for s in man["sources"] if s != "model"]
    out: dict = {"_reference": others[0] if others else None,
                 "_manifest": man}
    for item in man["items"]:
        tid = item["id"]
        tracks = {}
        for src in man["sources"]:
            p = d / tid / f"{src}.wav"
            if not p.exists():
                continue
            x, sr = read_wav(p)
            a = np.asarray(x, dtype=np.float64)
            tracks[src] = ((a.mean(axis=1) if a.ndim > 1 else a), sr)
        if tracks:
            out[tid] = tracks
    return out


def rms_db(x, sr, window) -> float:
    seg = np.asarray(x[int(window[0] * sr):int(window[1] * sr)], dtype=np.float64)
    if len(seg) == 0:
        return -300.0
    return 10 * np.log10(max(float(np.mean(seg * seg)), 1e-30))


def band_db(x, sr, band, window) -> float:
    seg = np.asarray(x[int(window[0] * sr):int(window[1] * sr)], dtype=np.float64)
    if len(seg) < 1024:
        return -300.0
    S = np.fft.rfft(seg)
    fr = np.fft.rfftfreq(len(seg), 1.0 / sr)
    m = (fr >= band[0]) & (fr < band[1])
    return 10 * np.log10(max(float(np.sum(np.abs(S[m]) ** 2)) / len(seg) ** 2, 1e-30))


def window_for(item, what: str) -> tuple[float, float]:
    """A window derived from the take's own events rather than placed by eye.

    `what` is one of:
      `body`     from just after the first note-on, while everything is sounding
      `ringing`  after the last note-off, before any pedal lifts
      `floor`    the very end, where the take has nothing left in it

    The pedal case is the one that matters and the one that was got wrong: the
    resonance a held pedal produces lives between the last note-off and the
    pedal-up event, and a window after the pedal lifts measures the dampers
    landing, which is the opposite mechanism.
    """
    meta = item.get("meta", item)
    notes = meta.get("notes")
    if not notes:
        raise ValueError(
            f"take {item.get('id', '?')} carries no schedule; re-render it with a "
            "make_audition that writes meta.notes / meta.cc")
    starts = [n["start"] for n in notes]
    ends = [n["start"] + n["duration"] for n in notes]
    total = float(meta["seconds"])
    first, last = min(starts), max(ends)
    # The pedal-up that ends the ringing is the first one AFTER the last note
    # stops, not the first one in the take. A phrase that changes pedal on every
    # bass note has several, and taking the earliest collapses the window onto a
    # moment in the middle of the phrase where notes are still being played.
    ups = sorted(float(t) for t, cc, value in meta.get("cc", ())
                 if cc == 64 and value < 64)
    after = [t for t in ups if t > last]
    stop = after[0] if after else total - 0.6
    if what == "body":
        return (first + 0.1, min(first + 0.1 + 1.6, last))
    if what == "ringing":
        # A take whose last note is still sounding when it ends has no window in
        # which nothing is driven; say so rather than return an inverted one.
        lo = last + 0.1
        if stop - lo < 0.3:
            raise ValueError(
                f"take {item.get('id', '?')} has no window between its last "
                f"note-off ({last:.2f} s) and {stop:.2f} s")
        return (lo, stop)
    if what == "floor":
        return (max(total - 0.6, 0.0), total)
    raise ValueError(what)


def band_error(tracks, source, reference, body, window, bands=BANDS,
               snr_db: float = SNR_DB):
    """Per-band dB difference in `window`, with one gain removed on `body`.

    Bands the reference does not hold clear of its own tail floor come back as
    None rather than as a number, for the reason every other module here now
    carries a floor gate: a recording that has stopped decaying reads as the
    dense, instrument-like thing a model is being asked to match.
    """
    m, sr = tracks[source]
    r, _ = tracks[reference]
    floor = (max(len(r) / sr - 0.6, 0.0), len(r) / sr)
    g = rms_db(m, sr, body) - rms_db(r, sr, body)
    out = []
    for b in bands:
        rb = band_db(r, sr, b, window)
        if rb - band_db(r, sr, b, floor) <= snr_db:
            out.append(None)
            continue
        out.append(band_db(m, sr, b, window) - rb - g)
    return out, g


def relative_to(tracks, simple_tracks, source, window, simple_window) -> float:
    """This take's level in `window` against the same source's simpler take.

    Both sides measured inside one render, so the number is what the phrase
    added over one note and carries none of that render's own errors about how a
    note decays. Compare the model's figure with the reference's: the difference
    is the phrase behaviour, isolated.
    """
    x, sr = tracks[source]
    s, ssr = simple_tracks[source]
    return rms_db(x, sr, window) - rms_db(s, ssr, simple_window)


def report(rows, bands=BANDS) -> str:
    """`rows` is a sequence of (label, per-band list) as `band_error` returns."""
    hdr = "".join(f"{f'{lo // 1000}k' if lo >= 1000 else lo:>7}" for lo, _ in bands)
    out = [f"{'':<26}{hdr}{'mean|e|':>9}"]
    for label, vals in rows:
        live = [v for v in vals if v is not None]
        out.append(f"{label:<26}" + "".join(
            f"{v:>7.1f}" if v is not None else f"{'--':>7}" for v in vals) +
            (f"{np.mean(np.abs(live)):>9.1f}" if live else f"{'--':>9}"))
    return "\n".join(out)
