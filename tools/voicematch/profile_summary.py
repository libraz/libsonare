"""Measured rows into the per-timbre curves a voice is fitted to, and the printed report."""

from __future__ import annotations

import sys

import numpy as np

from metrics import ladder_present


# Past this much delay between a note-on and the strike, the host scheduled the
# note rather than the instrument being slow to speak. Well above any real
# attack on a kit and well under the shortest delay a retried note came back at.
LATE_ONSET_MS = 20.0


def velocity_response(rows: list[dict]) -> dict:
    """Per note, how far the level moves from the softest blow to the hardest.

    A dimension in its own right rather than a slice of the level table, because
    on some instruments it IS the instrument. A harpsichord's plectrum releases
    at nearly the same displacement however fast the key is pressed, so the
    literature puts its whole dynamic range within 3 to 6 dB and its amplitude
    is not reliably monotonic in key speed; a piano's runs to 30 dB and is
    monotonic by construction. A model given the sampler velocity curve of the
    wrong one of those is wrong by 20 dB in a way no timbre metric reports,
    since every one of them normalises the note by its own fundamental.

    `monotonic` is reported rather than assumed for the same reason: on an
    instrument that genuinely is not, a model that is reads as correct on the
    range alone.
    """
    by_note: dict[str, dict[int, float]] = {}
    for row in rows:
        if "peak_dbfs" in row:
            by_note.setdefault(str(row["note"]), {})[int(row["velocity"])] = row["peak_dbfs"]
    out: dict = {}
    for note, peaks in sorted(by_note.items(), key=lambda kv: int(kv[0])):
        if len(peaks) < 2:
            continue
        ordered = [peaks[v] for v in sorted(peaks)]
        out[note] = {
            "range_db": round(max(ordered) - min(ordered), 2),
            "monotonic": ordered == sorted(ordered),
        }
    return out


def summarize(rows: list[dict]) -> dict:
    """The per-timbre curves: what a voice is fitted to rather than a table of takes."""
    out: dict = {}
    for row in rows:
        t = out.setdefault(row["timbre"], {"stretch_cents": {}, "inharmonicity_b": {},
                                           "level_dbfs": {}, "centroid_hz": {},
                                           "decay_db_s": {}, "damper_release_ms": {},
                                           "tnr_db": {}})
        n = str(row["note"])
        v = str(row["velocity"])
        # The tuning and the stiffness belong to the string, not to how hard it
        # was hit, so they are averaged over velocity rather than tabulated by it.
        if "cents_vs_et" in row:
            t["stretch_cents"].setdefault(n, []).append(row["cents_vs_et"])
        if "inharmonicity_b" in row and row.get("inharmonicity_reliable"):
            t["inharmonicity_b"].setdefault(n, []).append(row["inharmonicity_b"])
        for key, dest in (("rms_dbfs", "level_dbfs"), ("centroid_hz", "centroid_hz"),
                          ("decay_db_s", "decay_db_s"), ("damper_release_ms", "damper_release_ms"),
                          ("tnr_db", "tnr_db")):
            if key in row:
                t[dest].setdefault(n, {})[v] = row[key]
    for timbre, t in out.items():
        for key in ("stretch_cents", "inharmonicity_b"):
            t[key] = {n: round(float(np.median(vals)), 4 if key == "stretch_cents" else 8)
                      for n, vals in sorted(t[key].items(), key=lambda kv: int(kv[0]))}
        t["velocity_response"] = velocity_response(
            [r for r in rows if r["timbre"] == timbre]
        )
    return out


def summarize_percussion(rows: list[dict]) -> dict:
    """The per-timbre curves of a kit: one instrument per note, not one curve.

    Nothing here is interpolated across notes the way a keyboard's is. A kit's
    note 38 and note 40 are two snares that happen to be adjacent, so a summary
    that averaged them would be describing an instrument nobody owns; every
    table below stays keyed by note, and velocity is the only axis reduced over.
    """
    out: dict = {}
    for row in rows:
        t = out.setdefault(row["timbre"], {"bands_db": {}, "band_decay_db_s": {},
                                           "centroid_hz": {}, "onset_ms": {},
                                           "attack_ms": {},
                                           "decay_ms": {}, "crest_db": {},
                                           "level_dbfs": {}})
        n, v = str(row["note"]), str(row["velocity"])
        for key, dest in (("bands_db", "bands_db"), ("band_decay_db_s", "band_decay_db_s"),
                          ("centroid_hz", "centroid_hz"), ("onset_ms", "onset_ms"),
                          ("attack_ms", "attack_ms"),
                          ("decay_ms", "decay_ms"), ("crest_db", "crest_db"),
                          ("level_db", "level_dbfs")):
            if key in row:
                t[dest].setdefault(n, {})[v] = row[key]
    for timbre, t in out.items():
        t["velocity_response"] = velocity_response(
            [r for r in rows if r["timbre"] == timbre]
        )
    return out


def print_percussion_summary(summary: dict) -> None:
    for timbre, s in summary.items():
        print(f"\n== {timbre} ==", file=sys.stderr)
        notes = sorted(s["centroid_hz"], key=int)
        if not notes:
            continue
        print("  note   centroid(Hz)   attack(ms)   decay(ms)   crest(dB)", file=sys.stderr)
        for n in notes:
            def loudest(table):
                by_vel = table.get(n) or {}
                return by_vel[max(by_vel, key=int)] if by_vel else float("nan")
            print(f"  {int(n):4d}   {loudest(s['centroid_hz']):12.0f}   "
                  f"{loudest(s['attack_ms']):10.1f}   {loudest(s['decay_ms']):9.0f}   "
                  f"{loudest(s['crest_db']):9.1f}", file=sys.stderr)
        vel = s.get("velocity_response") or {}
        if vel:
            ranges = [row["range_db"] for row in vel.values()]
            non_mono = [int(n) for n, row in vel.items() if not row["monotonic"]]
            print(f"  velocity range {min(ranges):.1f} to {max(ranges):.1f} dB "
                  f"across {len(vel)} notes", file=sys.stderr)
            if non_mono:
                # On a kit this is usually a sample-layer boundary rather than
                # an instrument that genuinely plays quieter when hit harder.
                print(f"  (level is not monotonic in velocity at {non_mono})", file=sys.stderr)
        late = [(int(n), v, ms) for n, by_vel in (s.get("onset_ms") or {}).items()
                for v, ms in by_vel.items() if ms > LATE_ONSET_MS]
        if late:
            # Every measurement here is taken from the strike rather than from
            # the note-on, so a late one costs nothing but its own tail. Said
            # out loud anyway: it is the host's scheduling, it means those rows
            # were retried during capture, and a hit late enough to fall outside
            # the window would be lost silently.
            worst = max(ms for _, _, ms in late)
            print(f"  ({len(late)} of {sum(len(x) for x in s['onset_ms'].values())} rows "
                  f"sounded up to {worst:.0f} ms after their note-on; measured from the "
                  f"strike)", file=sys.stderr)


def print_summary(summary: dict) -> None:
    for timbre, s in summary.items():
        print(f"\n== {timbre} ==", file=sys.stderr)
        notes = sorted(s["stretch_cents"], key=int)
        if not notes:
            continue
        print("  note   stretch(c)   inharmonicity B", file=sys.stderr)
        dropped = []
        for n in notes:
            b = s["inharmonicity_b"].get(n)
            shown = f"{b:.3e}" if b is not None else "  too few partials"
            if b is None:
                dropped.append(int(n))
            print(f"  {int(n):4d}   {s['stretch_cents'][n]:+8.1f}   {shown}", file=sys.stderr)
        if dropped:
            # Named rather than silently absent: a curve that quietly stops
            # short reads afterwards as a curve that covered the keyboard.
            print(f"  (stiffness not fitted at {dropped} — too few partials under "
                  f"the Nyquist frequency to fit two parameters)", file=sys.stderr)
        vel = s.get("velocity_response") or {}
        if vel:
            ranges = [row["range_db"] for row in vel.values()]
            non_mono = [int(n) for n, row in vel.items() if not row["monotonic"]]
            print(f"  velocity range {min(ranges):.1f} to {max(ranges):.1f} dB "
                  f"across {len(vel)} notes", file=sys.stderr)
            if non_mono:
                print(f"  (level is not monotonic in velocity at {non_mono} — a property "
                      f"of the instrument, not a fault, on anything plucked)", file=sys.stderr)


def profile_program(profile: dict, cfg: dict) -> int:
    """The GM program the model answers this profile with.

    From the profile first: it records what the capture was a reference FOR, and
    that is the only place the two are tied together. A profile measured before
    the field existed falls back to the config, which is the same value for
    every capture in the tree; the fallback is here so an old profile still
    compares rather than silently comparing against program 0.

    `cfg["program"]` wins when the command line set it, since asking to compare
    this reference against a different program is a real thing to want — the
    same harpsichord capture judges programs 6 and 7.
    """
    if cfg.get("_program_override"):
        return int(cfg["program"])
    recorded = profile.get("capture", {}).get("program")
    return int(recorded if recorded is not None else cfg.get("program", 0))


def partial_balance_db(partials_db: list[float] | None) -> float | None:
    """Mean level of partials 2-6 relative to the fundamental.

    The one number that says whether a note has a partial stack at all. Centroid
    does not: broadband excitation noise carries a centroid perfectly well while
    the tonal spectrum underneath has collapsed to a sine, which is what an
    over-long hammer contact or an over-soft plectrum does to the treble.
    Inharmonicity does not either — it needs six partials before it reports
    anything, so a note this broken reads as "unmeasurable" rather than as wrong.

    A bin the ladder found nothing in carries the render's own noise floor, not a
    sentinel, so it has to be excluded by the same `ladder_present` rule the fit
    uses rather than by a magnitude test. Averaging the floor bins in is how a
    bar or a bell scores a partial stack it does not have: a glockenspiel's h2-h6
    sit 55 to 91 dB under h1 and read as -80 dB of stack, which is the mean of
    four noise floors. Nothing above the fundamental surviving means unscorable.
    """
    if not partials_db or len(partials_db) < 6:
        return None
    ref = partials_db[0]
    present = ladder_present(partials_db[:6])
    upper = [d for d, ok in zip(partials_db[1:6], present[1:6]) if ok]
    if not upper:
        return None
    return float(np.mean(upper) - ref)


def a4_offset_cents(rows: list[dict], timbre: str) -> float:
    """How far the reference instrument's A4 itself sits from 440 Hz.

    This is master tune, not stretch: an instrument tuned to A439.4 reads two
    cents flat at every note, and subtracting it is the difference between
    asking "is the stretch curve right" and asking "did we copy this plugin's
    tuning knob". A model that answers yes to the second is detuned against
    every other instrument in a mix, so the offset is reported and removed
    rather than fitted.
    """
    at = {}
    for r in rows:
        if r["timbre"] == timbre and "cents_vs_et" in r:
            at.setdefault(r["note"], []).append(r["cents_vs_et"])
    if not at:
        return 0.0
    notes = sorted(at)
    med = {n: float(np.median(at[n])) for n in notes}
    below = [n for n in notes if n <= 69]
    above = [n for n in notes if n >= 69]
    if not below or not above:
        return med[notes[0] if not below else notes[-1]]
    lo, hi = below[-1], above[0]
    if lo == hi:
        return med[lo]
    return med[lo] + (med[hi] - med[lo]) * (69.0 - lo) / (hi - lo)


def band_db(partials_db: list[float] | None, lo: int, hi: int) -> float | None:
    """Mean level of partials [lo, hi] (1-based) relative to the fundamental.

    Wider than partial_balance_db and parameterized, because the way an
    instrument responds to velocity is not one number: a real hammer moves the
    partials ABOVE the felt's cutoff and leaves the ones below it alone, so the
    h2-h7 and the h8-h16 bands answer different questions and averaging them
    together hides which of the two the model gets wrong.
    """
    if not partials_db or len(partials_db) < lo:
        return None
    ref = partials_db[0]
    band = [d for d in partials_db[lo - 1:hi] if d > -200.0]
    if not band:
        return None
    return float(np.mean(band) - ref)
