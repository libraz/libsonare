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

`drawn` is the one measurement here that keeps the gain rather than removing it,
because the gain is a finding. Every version of a take is written at one shared
gain set by the loudest of them, so a model that plays hot draws as a solid slab
filling its pane while the reference is pushed down it and draws as a thin core
with the transients standing clear -- and that reads to the eye as a difference
in DENSITY, which is a statement about the envelope, when it is a difference in
LEVEL, which is a statement about one number. The two are told apart by
measuring peak, body and floor separately: an offset moves all three together
and leaves the intervals between them alone. Nothing else in this harness would
have caught it. The note metrics are h1-normalised and level-blind by
construction, `band_error` divides the offset out on purpose, and the audition
page draws from the raw buffer, so the difference was visible on screen, absent
from every table, and shaped exactly like a defect it is not.

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
#: Seconds per column of the drawn envelope. A waveform pane takes one peak per
#: pixel, which makes its column a function of how wide the window happens to be
#: and how long the take is; a fixed span keeps two takes comparable and is what
#: a measurement needs. Short enough to keep a strike in one column, long enough
#: that a single sample of a period does not become the column.
DRAWN_COL_S = 0.015


def pick_references(manifest, wanted: str = "") -> list[str]:
    """Which sources everything else is measured against.

    A list, not one, wherever the page holds more than one reference. Three
    concert grands were captured because no single instrument is the target, and
    on a phrase they are far apart: on the arpeggio take one of the three sits
    7 dB above the other two, and reading the model against that one alone puts
    its transient-to-body figure 12.8 dB out where the median puts it 1.2 dB
    out. Which of those two numbers gets reported is then decided by which
    reference happened to be first in the manifest, and the report says nothing
    about having chosen.
    """
    return _references(manifest, wanted)


def pick_reference(manifest, wanted: str = "") -> str | None:
    """The single reference, for callers that can only take one.

    Read `pick_references` first: on a phrase take the choice between one
    reference and the median of three is worth more than 10 dB.
    """
    refs = _references(manifest, wanted)
    return refs[0] if refs else None


def _references(manifest, wanted: str = "") -> list[str]:
    """Resolve `--reference`, or work out what the page's references are.

    A page is not two renders any more. `make_audition --variant` puts candidate
    settings of the model beside it, so "whichever source is not `model`" -- what
    this used to answer -- resolves to a variant, and every table then reads as a
    comparison against a reference when it is a comparison against the model
    under different constants. The reference is the row every other row is judged
    by; naming the wrong one inverts the finding rather than blurring it, and
    nothing downstream can tell.

    So: declared `role: reference` sources decide it. Failing that, a page with
    one non-model source is unambiguous and that source is it. A page with
    several and no roles is refused, because there is no rule that picks between
    a variant and a reference from a key alone.
    """
    sources = manifest.get("sources", {})
    if wanted:
        names = [w.strip() for w in wanted.split(",") if w.strip()]
        missing = [w for w in names if w not in sources]
        if missing:
            raise SystemExit(f"no source {', '.join(missing)} on this page; have "
                             f"{', '.join(sources) or '(none)'}")
        return names
    declared = [k for k, v in sources.items()
                if isinstance(v, dict) and v.get("role") == "reference"]
    if declared:
        return declared
    others = [s for s in sources if s != "model"]
    if len(others) <= 1:
        return others
    raise SystemExit(
        f"this page has {len(others)} sources beside `model` and none of them "
        f"declares role=reference: {', '.join(others)}.\n"
        f"Name them with --reference (comma-separated), or re-render the page "
        f"with a make_audition that writes roles.")


def load(directory, reference: str = "") -> dict:
    """`{take_id: {source: (samples, sample_rate)}}` plus `_reference`.

    A model-only page has no reference; callers that need one should say so
    rather than compare a render with itself.
    """
    from wavio import read_wav

    d = Path(directory)
    man = json.load(open(d / "manifest.json"))
    out: dict = {"_reference": pick_reference(man, reference), "_manifest": man}
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
      `phrase`   the whole of it, from the first note-on to the end

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
    if what == "phrase":
        return (first, total)
    raise ValueError(what)


def drawn(x, sr, window, col_s: float = DRAWN_COL_S):
    """Peak, body and floor of the wave as a waveform pane draws it.

    One absolute peak per short column, then three percentiles of those columns:
    the 95th is what the transients reach, the median is the body they sit on,
    the 10th is the quietest the take gets. Percentiles rather than the extremes
    because one clipped sample and one gap of silence would otherwise decide two
    of the three numbers.

    Levels stay absolute. That is the whole point -- see the module docstring.
    """
    a, b = int(window[0] * sr), min(int(window[1] * sr), len(x))
    seg = np.abs(np.asarray(x[a:b], dtype=np.float64))
    n = max(1, int(round(col_s * sr)))
    if seg.size < n:
        return (float("nan"),) * 3
    cols = seg[:(seg.size // n) * n].reshape(-1, n).max(axis=1)
    # A column of digital silence is not a level. Two renders that both end in
    # it would otherwise agree at whatever sentinel stood in for zero, and their
    # difference -- a difference of two sentinels -- would print as a number.
    def to_db(v):
        return 20 * np.log10(float(v)) if v > 0.0 else float("nan")
    return tuple(to_db(v) for v in np.percentile(cols, [95, 50, 10]))


def envelope_report(tracks, references, window, col_s: float = DRAWN_COL_S) -> str:
    """Every source's drawn envelope, against the median of the references.

    `spike-body` is the interval a level offset cannot move, so it is the column
    that says whether the envelope itself differs. A row that is uniformly high
    with `spike-body` near zero is a version that is louder; a row whose
    `spike-body` has collapsed is a version whose transients no longer stand out
    of what it is sustaining, which is the thing "the wave is filled in" means.

    The references keep their own rows above the median, and reading them is not
    optional: where they disagree with each other by more than the model
    disagrees with them, the model's number is inside the spread of real
    instruments and there is nothing there to fix.
    """
    def fmt(v, width, sign=""):
        # `v == v` is the NaN test: a percentile that landed on digital silence.
        return format(v, f">{sign}{width}.1f") if v == v else format("--", f">{width}")

    def cell(v, sign=""):
        return fmt(v, 8, sign)

    def wide(v, sign=""):
        return fmt(v, 12, sign)

    if isinstance(references, str):
        references = [references]
    rows = {src: drawn(x, sr, window, col_s) for src, (x, sr) in tracks.items()}
    present = [r for r in references if r in rows]
    if not present:
        return "   no reference render for this take"
    ref = tuple(np.median([rows[r][i] for r in present]) for i in range(3))

    out = [f"   {'':<22}{'spikes':>8}{'body':>8}{'floor':>8}{'spike-body':>12}",
           f"   {'':<22}{'p95':>8}{'p50':>8}{'p10':>8}{'dB':>12}"]
    for src, (hi, mid, lo) in rows.items():
        mark = "  (reference)" if src in present else ""
        out.append(f"   {src:<22}{cell(hi)}{cell(mid)}{cell(lo)}{wide(hi - mid)}{mark}")
    if len(present) > 1:
        out.append(f"   {'reference median':<22}{cell(ref[0])}{cell(ref[1])}"
                   f"{cell(ref[2])}{wide(ref[0] - ref[1])}")
    out.append(f"   {'':<22}{'-' * 36:>36}")
    for src, (hi, mid, lo) in rows.items():
        if src in present:
            continue
        out.append(f"   {src + ' - ref':<22}{cell(hi - ref[0], '+')}"
                   f"{cell(mid - ref[1], '+')}{cell(lo - ref[2], '+')}"
                   f"{wide((hi - mid) - (ref[0] - ref[1]), '+')}")
    # The spread of the references themselves, which is the only thing that says
    # whether a delta above is large. Reported as a range rather than left to be
    # worked out from the rows, because it routinely is not worked out.
    if len(present) > 1:
        spread = [max(rows[r][i] for r in present) - min(rows[r][i] for r in present)
                  for i in range(3)]
        sb = [rows[r][0] - rows[r][1] for r in present]
        out.append(f"   {'(reference spread)':<22}{cell(spread[0])}{cell(spread[1])}"
                   f"{cell(spread[2])}{wide(max(sb) - min(sb))}")
    return "\n".join(out)


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
