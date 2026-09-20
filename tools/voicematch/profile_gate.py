"""The gate file: the bounds a run is held to, and the reference spread they are read against."""

from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from profile_measure import double_decay_gap, register_deltas
from profile_summary import a4_offset_cents, partial_balance_db

DELTA_LABELS = {"stretch": "tuning vs the reference (cents)",
                "decay": "held-note decay (dB/s)",
                "damper": "damper release (ms)",
                "balance": "partial stack h2-h6 vs h1 (dB)",
                "centroid_pct": "brightness (% of the reference centroid)",
                "tnr": "tone-to-noise, + = model is cleaner (dB)",
                "vel_range": "softest-to-hardest level range (dB)",
                "register": "register profile, each side vs its own median (dB)",
                "stereo": "board width, + = model radiates wider (0 = mono)",
                "aftersound": "aftersound, the decay AFTER the knee (dB/s)",
                "doubling": "double decay, prompt minus aftersound (dB/s)",
                "body": "body under the note, below-f0 minus f0 (dB)",
                "band_tilt": "band tilt, + = model is brighter (dB)",
                "band_shape": "band profile error, magnitude only (dB)",
                "band_decay": "per-octave decay rate (dB/s)",
                # Time to the envelope's peak, which is not the same event on
                # the two paths this label serves: on a drum the peak IS the
                # strike, on a struck string the hammer is over milliseconds
                # before the soundboard reaches full level, so the number is
                # the bloom after it. Naming the peak rather than the cause is
                # the only wording true of both.
                "attack": "time to the note's arrival (ms)",
                "crest": "peak over RMS of the hit (dB)",
                "level": "how loud the hit is vs the reference (dBFS)",
                "ring": "ring length, + = model rings longer (doublings)",
                "tonality": "spectral flatness, + = model is noisier (dB)"}


def register_levels(rows: list[dict], timbre: str) -> dict[int, dict[int, float]]:
    """One timbre's per-note, per-velocity body level, in the shape @ref register_deltas wants."""
    out: dict[int, dict[int, float]] = {}
    for r in rows:
        if r["timbre"] != timbre or r.get("held_peak_dbfs") is None:
            continue
        out.setdefault(r["note"], {})[r["velocity"]] = r["held_peak_dbfs"]
    return out


def register_spread_by_note(profile: dict) -> dict[int, float]:
    """How far the references sit from each other on the register profile, per NOTE.

    The pooled spread the summary prints is a median over the whole keyboard, and
    a register profile is the one dimension where that is least useful: three
    grands agree on their top octave to within a couple of decibels and disagree
    about the note below it by eight, so the same model error means opposite
    things at two adjacent notes. Per note is the only reading that separates a
    finding from a place these instruments simply differ.
    """
    rows = profile.get("rows", [])
    timbres = sorted({r["timbre"] for r in rows})
    pooled: dict[int, list[float]] = {}
    for i, a in enumerate(timbres):
        for b in timbres[i + 1:]:
            for note, _vel, delta in register_deltas(register_levels(rows, a),
                                                     register_levels(rows, b)):
                pooled.setdefault(note, []).append(abs(delta))
    return {n: float(np.median(v)) for n, v in pooled.items() if v}


def print_register_profile(register: list[tuple[int, int, float]],
                           spread: dict[int, float]) -> None:
    """Where on the keyboard the level profile parts company, note by note.

    A median cannot carry this one at all. A register error is by definition
    confined to a register, so it is a handful of notes at one end against a
    keyboard's worth that are fine, and both summary columns divide it down to
    something that looks like drift. The row that matters is the note, and it is
    always at an edge.
    """
    by_note: dict[int, list[float]] = {}
    for note, _vel, delta in register:
        by_note.setdefault(note, []).append(delta)
    notes = sorted(by_note)
    print("\nregister profile, model minus reference, dB, over the references' own "
          "disagreement\n  (each side taken against its own median across the keyboard):")
    for i in range(0, len(notes), 5):
        chunk = notes[i:i + 5]
        cells = []
        for n in chunk:
            err = float(np.median(by_note[n]))
            s = spread.get(n)
            ratio = f"{abs(err) / s:4.1f}x" if s and s > 0.0 else "    -"
            cells.append(f"n{n:<3d}{err:+6.1f} /{(s if s else 0.0):5.1f} ={ratio}")
        print("  " + "  ".join(cells))


def select_dimensions(summary: dict[str, dict], wanted: list[str],
                      excused: dict[str, str] | None = None) -> dict[str, dict]:
    """Narrow the summary to the dimensions this instrument is judged on.

    Every dimension is measured for every instrument, because measuring is
    cheap and a number nobody looks at costs nothing. What is not free is
    GATING on one that does not apply: a harpsichord's top octave has no
    dampers at all, so its damper column compares one arbitrary tail against
    another and a bound recorded from it fails on whichever way the noise fell.
    A capture that lists no dimensions is judged on all of them, which is the
    right default — an instrument earns an exclusion by having a reason.

    `dimensions_na` is that reason written down, and it is subtracted here as
    well as in the coverage report: an entry saying a measurement is invalid,
    next to a bound recorded from that same measurement, asserts both at once.

    A named dimension that was not measured is reported rather than dropped:
    silence there reads as "that dimension was fine".
    """
    if excused:
        summary = {k: v for k, v in summary.items() if k not in excused}
    if not wanted:
        return summary
    missing = [d for d in wanted if d not in summary and d not in (excused or {})]
    if missing:
        print(f"\nthese dimensions are named by the capture and were not measured in this "
              f"run: {', '.join(missing)}", file=sys.stderr)
    return {k: v for k, v in summary.items() if k in wanted}


def reference_spread(profile: dict, dimensions: list[str] | None = None) -> dict[str, float]:
    """How far the references are from EACH OTHER, dimension by dimension.

    The number a model's error has to be read against, and the one a gate cannot
    supply. A gate's bounds are recorded from whatever the voice measured on the
    day, so a green gate says "no worse than when this was written" and never
    "as close as two of these instruments are to each other" -- which is the only
    definition of finished this harness can state. Dividing one by the other
    gives a figure that means the same thing in cents, dB, milliseconds and
    percent: at 1.0 the model sits inside the family, at 5.0 it is five times
    further out than its own references disagree, and the largest one is where
    the next round belongs.

    Computed by running every reference timbre against every other through the
    same per-row arithmetic the model goes through, so a dimension cannot be
    compared one way here and another way there.
    """
    rows = profile.get("rows", [])
    timbres = sorted({r["timbre"] for r in rows})
    if len(timbres) < 2:
        return {}
    a4 = {t: a4_offset_cents(rows, t) for t in timbres}
    by_key: dict[str, dict[tuple[int, int], dict]] = {}
    for r in rows:
        by_key.setdefault(r["timbre"], {})[(r["note"], r["velocity"])] = r

    def pair_deltas(a: str, b: str) -> dict[str, list[float]]:
        out: dict[str, list[float]] = {}
        shared = sorted(set(by_key.get(a, {})) & set(by_key.get(b, {})))
        peaks: dict[str, dict[int, dict[int, float]]] = {}
        for key in shared:
            x, y = by_key[a][key], by_key[b][key]

            def diff(field, sx=x, sy=y):
                return (sx[field] - sy[field]) if field in sx and field in sy else None

            row = {
                "stretch": (None if diff("cents_vs_et") is None
                            else diff("cents_vs_et") + a4[a] - a4[b]),
                "decay": diff("decay_db_s"),
                "tnr": diff("tnr_db"),
                "aftersound": diff("decay_late_db_s"),
                "doubling": (None if double_decay_gap(x) is None or double_decay_gap(y) is None
                             else double_decay_gap(x) - double_decay_gap(y)),
                "body": diff("body_below_f0_db"),
                "attack": diff("attack_ms"),
                "stereo": diff("stereo_width"),
                "damper": (None if x.get("damper_capped") or y.get("damper_capped")
                           else diff("damper_release_ms")),
            }
            bal_x, bal_y = partial_balance_db(x.get("partials_db")), partial_balance_db(
                y.get("partials_db"))
            row["balance"] = None if bal_x is None or bal_y is None else bal_x - bal_y
            if x.get("centroid_hz") and y.get("centroid_hz"):
                row["centroid_pct"] = 100.0 * (x["centroid_hz"] / y["centroid_hz"] - 1.0)
            for k, v in row.items():
                if v is not None and np.isfinite(v):
                    out.setdefault(k, []).append(float(v))
            for side, src in ((a, x), (b, y)):
                if "peak_dbfs" in src:
                    peaks.setdefault(side, {}).setdefault(key[0], {})[key[1]] = src["peak_dbfs"]
        for note, pa in sorted(peaks.get(a, {}).items()):
            pb = peaks.get(b, {}).get(note, {})
            both = sorted(set(pa) & set(pb))
            if len(both) >= 2:
                out.setdefault("vel_range", []).append(
                    (max(pa[v] for v in both) - min(pa[v] for v in both))
                    - (max(pb[v] for v in both) - min(pb[v] for v in both)))
        register = register_deltas(register_levels(rows, a), register_levels(rows, b))
        if register:
            out["register"] = [d for _, _, d in register]
        return out

    pooled: dict[str, list[float]] = {}
    for i, a in enumerate(timbres):
        for b in timbres[i + 1:]:
            for k, v in pair_deltas(a, b).items():
                pooled.setdefault(k, []).extend(np.abs(v).tolist())
    spread = {k: float(np.median(v)) for k, v in pooled.items() if v}
    return {k: v for k, v in spread.items() if not dimensions or k in dimensions}


def summarize_deltas(deltas: dict[str, list[float]]) -> dict[str, dict]:
    """Reduce each dimension's per-row deltas to the numbers a gate reads.

    Three rather than one, because a median hides a defect in two different
    ways and each column answers one of them. The signed median alone cannot
    fail on a defect that is symmetric across the keyboard: the brightness
    column once read +0.16 % of the reference centroid while individual notes
    were between 26 and 660 % out — the bass was dark by as much as the treble
    was bright. The absolute median is what that summary was missing.

    But an absolute median is still a median, so it only moves once half the
    grid is out, and the same brightness example describes a TAIL rather than a
    sign cancellation. Measured on the drum kit, every scalar dimension read
    between 0.6x and 0.9x of the references' own spread — inside, on both
    median columns — while 28 to 46 % of rows sat outside that spread and the
    worst row ran to 78x it. `p90` is the column that fails on a minority of
    bad rows, which is the shape a per-note defect actually takes.
    """
    return {
        k: {"median": float(np.median(v)), "abs_median": float(np.median(np.abs(v))),
            "p90": float(np.percentile(np.abs(v), 90)), "n": len(v)}
        for k, v in deltas.items()
    }


#: Dimensions whose row is a NOTE rather than a grid cell — each one is built by
#: aggregating that note's velocities, so a count below the grid's is their shape
#: and not evidence a censor took away. Reporting them as thin would put a line
#: on every gate and teach the reader to skip the one that means something.
PER_NOTE_DIMENSIONS = ("vel_range", "register")

#: Floor for a dimension with neither a measured spread nor a guess. Nobody
#: chose it for the dimension it lands on, in whatever unit that is: 101 of 119
#: `register` bounds are this number. Named so a gate can record it as its own.
GENERIC_FLOOR = 1.0


def print_summary_table(summary: dict[str, dict], spread: dict[str, float],
                        attempted: int) -> None:
    """The per-dimension summary, with the population each median was taken over.

    `unscored` is the column the table was missing. Every dimension here reduces
    a list built by skipping the rows it could not produce a number for, so a
    change that makes a row unscorable removes it from the population instead of
    failing it — and a median over a shrunk population is free to improve for
    exactly that reason. `rows` alone cannot say so: it is a count with nothing
    to read it against, and both comparators print it beside dimensions whose
    populations differ by design.

    Shared by the pitched and percussion comparators so the two cannot drift;
    the prose under it is each caller's, since what the columns mean for a kit
    and for a keyboard is not the same sentence.
    """
    print("\n" + f"{'':46s} {'median':>9} {'|median|':>9} {'p90':>8} {'spread':>8} "
          f"{'x spread':>9} {'rows':>5} {'unscored':>9}")
    for k, row in summary.items():
        s_k = spread.get(k)
        ratio = (row["abs_median"] / s_k) if s_k and s_k > 0 else None
        # A per-note dimension's unit is not the row, so a difference against the
        # row count is its shape rather than a censor — the same exemption the
        # gate's thin-grid check makes.
        missing = ("        -" if k in PER_NOTE_DIMENSIONS
                   else f"{max(attempted - row['n'], 0):9d}")
        print(f"  {DELTA_LABELS.get(k, k):46s} {row['median']:+9.2f} "
              f"{row['abs_median']:9.2f} {row['p90']:8.2f} "
              f"{(f'{s_k:8.2f}' if s_k is not None else '       -')} "
              f"{(f'{ratio:8.1f}x' if ratio is not None else '        -')} "
              f"{row['n']:5d} {missing}")
    thin = [k for k, row in summary.items()
            if k not in PER_NOTE_DIMENSIONS and row["n"] < attempted]
    if thin:
        print(f"\n  `unscored` is how many of the {attempted} scored rows the dimension "
              f"could not\n  produce a number for, and those rows LEFT the median rather "
              f"than failing it.\n  A change that makes a row unscorable therefore improves "
              f"the column it broke:\n  read every median above against this count, on "
              f"{', '.join(DELTA_LABELS.get(k, k) for k in thin)}.")


def print_vanished_dimensions(offered: set[str], summary: dict[str, dict],
                              wanted: list[str], excused: dict[str, str]) -> None:
    """Name the dimensions that scored no row at all, and so have no line.

    The limit case of `unscored`: at zero the dimension does not appear in the
    table, which is the same silence as a dimension that agreed. An empty column
    is the one a metric scores best of all, and the reader has no count to read
    against it because there is no row to carry one.

    `select_dimensions` reports this only for a capture that declares its
    dimensions, and a capture that declares none is judged on all of them — so
    without this those are exactly the runs where a dimension can disappear with
    nothing said.
    """
    gone = sorted(k for k in offered
                  if k not in summary and k not in (excused or {})
                  and (not wanted or k in wanted))
    if not gone:
        return
    print(f"\n  {len(gone)} dimension(s) scored no row at all and so have no line above: "
          f"{', '.join(DELTA_LABELS.get(k, k) for k in gone)}.\n  Not excused and not "
          f"bounded anywhere — an empty column is not a column that agreed.")


def check_gate(summary: dict[str, dict], gate_path: Path, timbre: str,
               measured_utc: str = "") -> int:
    """Fail the run when a dimension has moved outside its recorded bound.

    What this exists to catch is not a bad voice — it is a change that improves
    one dimension by breaking another, which is the shape most of this work
    takes. Widening the prompt-decay profile fixes the level on every note
    between F#2 and F#5 and brightens the same notes by 35 to 60 points of
    centroid; both are real, and whoever makes that trade should be the one to
    decide it rather than discovering it later in a listening test.
    """
    if not gate_path.exists():
        print(f"\nno gate at {gate_path} — write one with --write-gate once the current "
              f"numbers are ones worth holding", file=sys.stderr)
        return 2
    gate = json.loads(gate_path.read_text())
    if gate.get("timbre") and gate["timbre"] != timbre:
        print(f"\ngate was recorded against timbre {gate['timbre']!r}, not {timbre!r}; "
              f"a bound is only meaningful against the reference it was measured from",
              file=sys.stderr)
        return 2
    # A gate is invalidated by its REFERENCE moving as much as by its voice.
    # Re-measuring the profile moves the numbers every bound was recorded from,
    # so a gate recorded before that re-measure is holding the voice to a
    # comparison against an instrument that is no longer in the file. Reported
    # rather than failed: which of the two moved is the question the reader has
    # to answer, and a red line that does not say which is worse than a warning
    # that does.
    against = gate.get("reference_measured_utc")
    if measured_utc and against and against != measured_utc:
        print(f"\nthis gate was recorded against a reference measured {against}, and the "
              f"profile now reads {measured_utc}. The bounds predate their own reference; "
              f"re-record before reading a failure as the voice's.", file=sys.stderr)
    bounds = gate.get("bounds", {})
    failures = []
    evidence: list[tuple[str, int, int | None]] = []
    for key, bound in bounds.items():
        row = summary.get(key)
        if row is None:
            failures.append(f"{DELTA_LABELS.get(key, key)}: not measured in this run")
            continue
        # How many of the grid's rows survived censoring into this median. A
        # censor drops a row it cannot compare and the remaining rows are
        # averaged, so a dimension can be held to a bound while most of the
        # keyboard contributed nothing -- and neither the bound nor the verdict
        # says which rows those were. Recorded so the comparison is against the
        # same population, in both directions: evidence returning is as much a
        # different measurement as evidence leaving.
        was, now = bound.get("rows"), int(row.get("n", 0))
        evidence.append((key, now, was))
        if was and now and (2 * now < was or 2 * was < now):
            failures.append(
                f"{DELTA_LABELS.get(key, key)}: median over {now} rows, bound recorded "
                f"from {was} — not the same population")
        for stat in ("median", "abs_median", "p90"):
            limit = bound.get(stat)
            if limit is None:
                continue
            value = abs(row[stat]) if stat == "median" else row[stat]
            if value > limit:
                failures.append(
                    f"{DELTA_LABELS.get(key, key)}: {stat} {value:.2f} over its bound {limit:.2f}"
                )
    print(f"\ngate: {gate_path.name}, {len(bounds)} dimensions")
    # A dimension this run measured and this gate has no bound for. The loop
    # above walks the gate, so such a dimension is held to nothing and says so
    # nowhere -- which is the same silence a capture naming an unmeasured
    # dimension produces, and reads the same way: as a column that was fine.
    ungated = [k for k in summary if k not in bounds]
    if ungated:
        print(f"  {', '.join(DELTA_LABELS.get(k, k) for k in ungated)}: measured, no bound "
              f"recorded — nothing here holds it. Re-record with --write-gate.")
    # A gate written before `p90` existed carries only its two median columns,
    # and the loop above skips a stat with no bound. That is the same silence as
    # an ungated dimension and reads the same way -- as a tail that was fine --
    # so it is reported rather than left to the reader to notice was missing.
    tailless = [k for k, b in bounds.items() if "p90" not in b]
    if tailless:
        print(f"  no tail bound on {len(tailless)} of {len(bounds)} dimensions: these predate "
              f"the p90 column, so a minority of bad rows passes them. Re-record with "
              f"--write-gate.")
    widest = max((now for _k, now, _was in evidence), default=0)
    thin = [(k, now) for k, now, _was in evidence
            if widest and 2 * now < widest and k not in PER_NOTE_DIMENSIONS]
    if thin:
        print(f"  held on part of the grid: "
              f"{', '.join(f'{DELTA_LABELS.get(k, k)} {n}/{widest}' for k, n in thin)}")
    # The same question asked of the BOUND rather than of the run. `--write-gate`
    # records a bound from whatever population survived censoring and imposes no
    # minimum, so a dimension reaching one row of thirty-five is written as
    # confidently as one reaching all of them -- and a median over one row is
    # that row. Measured across the bank: six of the nine `damper` bounds came
    # from one to five rows of a thirty-five-row grid. Reported rather than
    # failed, and reported whether or not the bound was exceeded, because a
    # passing bound taken from one row says as little as a failing one.
    frail = [(k, was) for k, _now, was in evidence
             if was and widest and 2 * was < widest and k not in PER_NOTE_DIMENSIONS]
    if frail:
        print(f"  recorded from part of the grid: "
              f"{', '.join(f'{DELTA_LABELS.get(k, k)} {w}/{widest}' for k, w in frail)}\n"
              f"  — these bounds are that many rows, not the voice. A pass and a failure "
              f"both\n  speak for the notes that survived censoring and for no others.")
    if any(was is None for _k, _now, was in evidence):
        print("  this gate records no row counts, so a bound cannot be compared against the "
              "evidence it was set from. The next --write-gate records them.")
    # A bound resting on `GENERIC_FLOOR` is the weakest kind there is: its
    # dimension has no measured spread and no guess written for it, so the
    # number came from an argument default in whatever unit the dimension uses.
    # Named only when one of them is what failed, since a gate carries many.
    from_default = sorted({DELTA_LABELS.get(k, k) for k, b in bounds.items()
                           if isinstance(b, dict) and b.get("floor_from") == "default"
                           and any(f.startswith(DELTA_LABELS.get(k, k) + ":") for f in failures)})
    if from_default:
        print(f"  the bound that failed rests on the generic {GENERIC_FLOOR} floor on "
              f"{', '.join(from_default)}:\n  its dimension has neither a measured spread nor "
              f"a guess, so that number was\n  chosen for no dimension in particular and is "
              f"in this one's units by accident.")
    if failures:
        for line in failures:
            print(f"  FAIL  {line}")
        print(f"  {len(failures)} of the recorded bounds were exceeded. If the change is "
              f"deliberate, re-record with --write-gate in the same commit as the change "
              f"that justifies it.")
        return 1
    print("  every recorded bound held")
    return 0


#: Bound keys a re-record ratchets. `rows` sits beside them and is a population
#: count, so taking the smaller of two would quietly narrow what the gate claims
#: to have measured rather than tighten what it holds.
RATCHETED_BOUND_KEYS = ("median", "abs_median", "p90")


def _carry_the_unbounded(bounds: dict[str, dict], gate_path: Path) -> dict[str, str]:
    """Carry forward the recorded reasons a dimension holds no bound.

    `_unbounded` is hand-written — it says why a dimension the capture asks for
    could not be measured into one, which nothing computes — and this writer
    emits a fixed payload, so until this existed every re-record silently
    deleted it. The kit gates are where the convention lives and they were the
    ones it would have been deleted from.

    An entry whose dimension now HAS a bound is dropped rather than carried: the
    reason has been answered, and a stale one would explain away a bound that is
    sitting right beside it.
    """
    if not gate_path.exists():
        return {}
    recorded = json.loads(gate_path.read_text()).get("_unbounded") or {}
    return {k: v for k, v in recorded.items() if k not in bounds}


def _keep_the_tighter(bounds: dict[str, dict], gate_path: Path) -> list[str]:
    """Hold each bound to the tighter of the recorded one and the new one.

    A re-record may correct a bound and may never loosen one, and until this
    existed that was a rule kept by hand — which is the kind that holds until
    the run where the number moves for a reason that sounds good. It moves for
    reasons that sound good routinely: sharpening the measurement shrinks the
    reference spread under a bound, the floor stops winning, and the bound
    silently becomes the voice's own error times the margin. Nothing regressed
    and the gate got weaker anyway.

    Mutates `bounds` and returns one line per bound it refused to loosen, for
    the caller to print — a ratchet that acts silently is a second way to lose
    track of what the gate is holding.
    """
    if not gate_path.exists():
        return []
    recorded = json.loads(gate_path.read_text()).get("bounds", {})
    declined = []
    for dimension, row in bounds.items():
        was = recorded.get(dimension)
        if not was:
            continue
        for key in RATCHETED_BOUND_KEYS:
            if key in was and was[key] < row[key]:
                declined.append(f"{dimension}.{key} {row[key]:g} -> {was[key]:g}")
                row[key] = was[key]
    return declined


def write_gate_file(summary: dict[str, dict], gate_path: Path, timbre: str,
                    margin: float, measured_utc: str = "",
                    spread: dict[str, float] | None = None) -> int:
    """Record the current numbers as the bounds a later run is held to.

    The margin is multiplicative and deliberately not tight: a bound that fails
    on measurement noise gets switched off, and a switched-off gate catches
    nothing. There is also a floor under each bound, since a dimension that
    happens to be near zero today would otherwise be held to a tolerance no
    change could stay inside.
    """
    # A floor is the dimension's own noise, and it is MEASURED: where the corpus
    # holds several instruments of one kind, the median disagreement between
    # them is the tightest bound any model can be held to without failing on
    # which one it was compared against. Hand-written floors are the fallback
    # for a corpus with a single reference, and they were the whole story until
    # a measured one showed how far off a guess can be -- the guess under the
    # tuning column was 1.0 cent where three concert grands disagree by 3.74, so
    # a voice sitting at half their spread failed a bound it should never have
    # been held to. Nothing here loosens a bound below what was measured: the
    # recorded value times the margin still wins whenever it is larger.
    guesses = {"stretch": 1.0, "decay": 0.5, "damper": 5.0, "balance": 0.5,
               "centroid_pct": 1.0, "tnr": 1.0, "vel_range": 1.0,
               "stereo": 0.27, "attack": 40.0, "aftersound": 1.81,
               # A sixth of a doubling and a decibel: the smallest change in
               # ring length and in tonality a listener would call a different
               # instrument. Both are fallbacks — a capture with two references
               # measures its own floor and that one wins.
               "ring": 0.17, "tonality": 1.0}
    measured = {k: v for k, v in (spread or {}).items() if v > 0.0}
    floors = {**guesses, **measured}
    bounds = {}
    for key, row in summary.items():
        floor = floors.get(key, GENERIC_FLOOR)
        bounds[key] = {
            # Which of the three decided this floor, recorded because the three
            # carry different weight and the number alone cannot say which it
            # was. `default` is the weakest: nobody chose it for this dimension.
            "floor": round(floor, 4),
            "floor_from": ("measured spread" if key in measured
                           else "guess" if key in guesses else "default"),
            "median": round(max(abs(row["median"]) * margin, floor), 3),
            "abs_median": round(max(row["abs_median"] * margin, floor), 3),
            # The tail, held to the same floor as the medians: a p90 tighter
            # than the references' own disagreement would fail on which of them
            # the model was compared against rather than on the model.
            "p90": round(max(row["p90"] * margin, floor), 3),
            # The grid rows that survived censoring into this median. One voice
            # records its decay bound from 18 of 50, and without this a later
            # run compares against it as though the whole grid had spoken.
            "rows": int(row.get("n", 0)),
        }
    declined = _keep_the_tighter(bounds, gate_path)
    carried = _carry_the_unbounded(bounds, gate_path)
    gate_path.parent.mkdir(parents=True, exist_ok=True)
    gate_path.write_text(json.dumps({
        "_": "Bounds the compare table is held to. All three are absolute limits: 'median' "
             "caps the magnitude of the signed median, 'abs_median' caps the median absolute "
             "error, which is the one that can fail when errors of opposite sign cancel, and "
             "'p90' caps the 90th percentile of that absolute error, which is the one that "
             "can fail when a minority of rows is far out and the median is not. A dimension "
             "can sit inside both medians while nearly half its rows are outside the "
             "references' own spread. 'rows' is how many of the grid's rows survived "
             "censoring into that median, so a later run can tell whether it is comparing "
             "against the same population. Re-record only in the same change as the "
             "behaviour that justifies it.",
        "timbre": timbre,
        "margin": margin,
        # Which reference generation these were measured against, and when. Both
        # are here so staleness is a comparison rather than an excavation: the
        # alternative is reading the file's history to find out whether the
        # profile moved under it, which nobody does until a gate is already red.
        "reference_measured_utc": measured_utc,
        "recorded_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        # What the references disagree with each other by, recorded beside the
        # bounds so a later reader can see which of them are the measured floor
        # rather than the voice's own number times the margin.
        "reference_spread": {k: round(v, 4) for k, v in sorted((spread or {}).items())},
        # Why a dimension this capture asks for carries no bound. Hand-written
        # and carried across a re-record, since nothing measures it.
        **({"_unbounded": carried} if carried else {}),
        "bounds": bounds,
    }, indent=2) + "\n")
    print(f"\nwrote {gate_path} — {len(bounds)} bounds at {margin:g}x the measured values")
    for line in declined:
        print(f"  kept the recorded bound: {line}")
    return 0
