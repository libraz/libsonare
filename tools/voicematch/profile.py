#!/usr/bin/env python3
"""Turn a captured corpus into a reference profile, and score a model against it.

    profile.py measure                 # corpus WAVs -> reference/<id>.json
    profile.py compare                 # render the same grid through libsonare
                                       # and report where it diverges

The audio a capture produces cannot be committed — a sample library's licence
covers what is rendered from it — and would be the wrong thing to commit even
if it could: what a physical model has to reproduce is a handful of measured
properties, not four hundred megabytes of one particular piano. `measure`
extracts those properties and they are what lives in the repository.

Four of them are piano-specific and none are in `metrics.py`, whose per-note
analysis assumes a harmonic series:

- **Inharmonicity.** A piano string is stiff, so its nth partial sits at
  `n*f0*sqrt(1 + B*n^2)` rather than at `n*f0`. B runs from about 1e-4 in the
  middle to over 1e-2 at the top, and it is most of what separates a piano
  from a harp. Measured by fitting that law to the partials that are actually
  there, not assumed.
- **Stretch tuning.** Because of inharmonicity a piano is deliberately tuned
  away from equal temperament — flat in the bass, sharp in the treble, by tens
  of cents at the extremes. A model tuned to exact equal temperament beats
  against a real recording, and the beating is the first thing anyone hears.
- **Double decay.** A struck string does not decay along one line: the two or
  three strings of a unison fall out of phase, and the fast initial decay gives
  way to a slower aftersound at a knee a second or two in. A model with one
  decay rate is either too short or too dull.
- **Damper release.** Note-off on a piano is a felt damper landing on a moving
  string, which is neither instant nor an exponential release.

`compare` renders the same note-and-velocity grid through libsonare's GM
fallback, measures it the same way, and prints the differences. That is the
part which says what to change.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from capture import DEFAULT_CONFIG, load_config, out_root  # noqa: E402
from metrics import _db, _peak_near, _rms_envelope, _spectrum, midi_to_hz, to_mono  # noqa: E402
from render_model import render_model  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import read_wav  # noqa: E402

REFERENCE_DIR = Path(__file__).resolve().parent / "reference"
MAX_PARTIALS = 20
# Below this, relative to the strongest partial, a "peak" is as likely to be the
# noise floor as a partial, and feeding it to the inharmonicity fit moves B by
# more than the fit is worth.
PARTIAL_FLOOR_DB = -60.0
# Fewer than this many partials and the two-parameter stiffness fit has no room
# left over to be checked by; the value it returns is an interpolation.
MIN_PARTIALS_FOR_B = 6


# --------------------------------------------------------------------------
# per-note measurement


def find_partials(seg: np.ndarray, sr: int, note: int) -> dict:
    """Locate the partials of one struck note and fit the stiff-string law.

    Returns the measured f0, the inharmonicity coefficient B, and the partial
    frequencies and levels that were actually found.

    The fit is iterative because the two unknowns depend on each other: where
    to look for partial n depends on B, and B is estimated from where the
    partials turned out to be. Starting from B = 0 and re-searching three times
    converges well inside the tolerance for every note in the corpus — the
    search window narrows as the prediction improves, which is what stops a
    high partial from locking onto its neighbour.
    """
    freqs, mag = _spectrum(seg, sr)
    et = midi_to_hz(note)
    f0, a0 = _peak_near(freqs, mag, et, 120.0)
    if a0 <= 0.0:
        return {}
    b = 0.0
    fitted_on = 0
    found: list[tuple[int, float, float]] = []
    for it in range(3):
        found = []
        tol = (90.0, 45.0, 25.0)[it]
        for n in range(1, MAX_PARTIALS + 1):
            predicted = n * f0 * float(np.sqrt(1.0 + b * n * n))
            if predicted > 0.45 * sr:
                break
            fn, an = _peak_near(freqs, mag, predicted, tol)
            if an > 0.0:
                found.append((n, fn, an))
        strong = [(n, fn, an) for n, fn, an in found
                  if 20 * np.log10(max(an, 1e-12) / a0) > PARTIAL_FLOOR_DB]
        if len(strong) < 4:
            break
        # (f_n / n)^2 = f0^2 + f0^2 * B * n^2 is linear in n^2, weighted by how
        # strongly each partial was actually present.
        x = np.array([n * n for n, _, _ in strong], dtype=float)
        y = np.array([(fn / n) ** 2 for n, fn, _ in strong], dtype=float)
        w = np.array([an for _, _, an in strong], dtype=float)
        w = w / w.sum()
        xm, ym = float((w * x).sum()), float((w * y).sum())
        denom = float((w * (x - xm) ** 2).sum())
        if denom <= 0:
            break
        slope = float((w * (x - xm) * (y - ym)).sum()) / denom
        intercept = ym - slope * xm
        if intercept <= 0:
            break
        f0 = float(np.sqrt(intercept))
        b = max(0.0, slope / intercept)
        fitted_on = len(strong)

    if not found:
        return {}
    peak_a = max(a for _, _, a in found)
    return {
        "f0_hz": round(f0, 3),
        "cents_vs_et": round(float(1200.0 * np.log2(f0 / et)), 2),
        "inharmonicity_b": float(f"{b:.3e}"),
        # How many partials the stiffness fit actually had. At the top of the
        # keyboard there is barely a series left to fit — C8 puts only five
        # partials under the Nyquist frequency — and a B from four of them is a
        # number rather than a measurement. Reported so that whoever reads the
        # profile can tell which is which, and used by `summarize` to keep the
        # unreliable ones out of the curve a voice would be fitted to.
        "partials_fit": fitted_on,
        "inharmonicity_reliable": fitted_on >= MIN_PARTIALS_FOR_B,
        "partials_hz": [round(fn, 2) for _, fn, _ in found],
        "partials_db": [round(float(20 * np.log10(max(an, 1e-12) / peak_a)), 2)
                        for _, _, an in found],
    }


def double_decay(env_db: np.ndarray, t: np.ndarray) -> dict:
    """Split the decay at the knee where the fast fall gives way to the aftersound.

    A single slope over the whole held note is the average of two very
    different things and matches neither. The breakpoint is searched rather
    than assumed, because where it falls is itself a property worth reporting:
    it is set by how fast the strings of a unison drift apart, which a model
    reproduces only if it has more than one string.
    """
    if len(t) < 12:
        return {}
    best = None
    lo, hi = max(3, len(t) // 10), min(len(t) - 4, int(len(t) * 0.75))
    for k in range(lo, hi):
        e = 0.0
        for a, b in ((0, k), (k, len(t))):
            if b - a < 3:
                e = np.inf
                break
            p = np.polyfit(t[a:b], env_db[a:b], 1)
            e += float(np.sum((np.polyval(p, t[a:b]) - env_db[a:b]) ** 2))
        if best is None or e < best[0]:
            best = (e, k)
    if best is None:
        return {}
    k = best[1]
    early = float(np.polyfit(t[:k], env_db[:k], 1)[0])
    late = float(np.polyfit(t[k:], env_db[k:], 1)[0])
    return {
        "decay_early_db_s": round(early, 2),
        "decay_late_db_s": round(late, 2),
        "decay_knee_s": round(float(t[k] - t[0]), 3),
    }


def partial_decay(seg: np.ndarray, sr: int, partials_hz: list[float],
                  n_partials: int = 8) -> list[float]:
    """Decay rate in dB/s of each of the first partials, each from its own peak.

    The top of a piano's spectrum dies far faster than the bottom, and that
    difference over time — not the spectrum at any one instant — is why a
    sustained piano note darkens as it rings. A model matched on one spectral
    snapshot can have every partial in the right place and still get this wrong.
    """
    out: list[float] = []
    win = max(1024, int(0.093 * sr))
    hop = win // 2
    n_frames = max(1, (len(seg) - win) // hop + 1)
    if n_frames < 6:
        return out
    frames = np.lib.stride_tricks.sliding_window_view(seg, win)[::hop][:n_frames]
    window = np.hanning(win)
    spec = np.abs(np.fft.rfft(frames * window, axis=1))
    fr = np.fft.rfftfreq(win, 1.0 / sr)
    t = np.arange(n_frames) * hop / sr
    for f in partials_hz[:n_partials]:
        k = int(np.argmin(np.abs(fr - f)))
        band = spec[:, max(0, k - 1):k + 2].max(axis=1)
        db = _db(band / max(band.max(), 1e-12))
        keep = db > -55.0
        if keep.sum() < 5:
            out.append(float("nan"))
            continue
        out.append(round(float(np.polyfit(t[keep], db[keep], 1)[0]), 2))
    return out


def _above_fundamental(seg: np.ndarray, sr: int, f0_hz: float) -> np.ndarray:
    """Drop everything below the note, so a release measures the string.

    A close-miked grand keeps ringing after every note-off at frequencies that
    have nothing to do with what was played: the same 21, 35 and 82 Hz appear in
    the tail of A0, C3 and C4 alike, at 25 to 40 dB below the held level. Those
    are the case and the soundboard, not the string the damper just landed on,
    and they sit above a broadband envelope's threshold for a long time —
    measured that way a C4 damper reads as taking over two seconds, which is the
    box ringing, not the felt.

    Cutting below the fundamental leaves the note and removes the box. It cannot
    do so at the bottom of the keyboard, where those modes are the note's own
    first partials; a bass row is contaminated whatever this does, and the
    velocities where it matters are also the ones the capture tail truncates.
    """
    n = len(seg)
    if n < 16:
        return seg.astype(np.float64)
    fc = max(20.0, 0.6 * f0_hz)
    spec = np.fft.rfft(seg.astype(np.float64))
    spec[np.fft.rfftfreq(n, 1.0 / sr) < fc] = 0.0
    return np.fft.irfft(spec, n)


def measure_note(audio: np.ndarray, sr: int, note: int, *,
                 preroll_s: float, gate_s: float) -> dict:
    """Every measurement this profile carries, for one captured note."""
    mono = to_mono(audio)
    on = int(preroll_s * sr)
    off = int((preroll_s + gate_s) * sr)
    held = mono[on:off]
    if held.size < sr // 4:
        return {}

    # The spectrum for the partial fit comes from a window that starts after the
    # strike: the hammer noise is broadband and would blur every peak the fit
    # depends on, while a window too late has lost the high partials entirely.
    a = int(0.12 * sr)
    b = min(len(held), a + int(1.5 * sr))
    row: dict = dict(find_partials(held[a:b], sr, note))
    if not row:
        # No fundamental means this is not a struck note, and every measurement
        # below would still produce a number for it — a decay slope of 0 dB/s
        # and a damper release the length of the window. A profile is worse for
        # holding those than for being short a row.
        return {}

    t_env, env = _rms_envelope(held, sr)
    env_db = np.asarray(_db(env))
    peak_i = int(np.argmax(env))
    row["peak_dbfs"] = round(float(_db(np.abs(mono).max())), 2)
    row["rms_dbfs"] = round(float(_db(np.sqrt(np.mean(held.astype(np.float64) ** 2)))), 2)
    row["attack_ms"] = round(float(t_env[peak_i] * 1000.0), 1)

    tail = slice(peak_i, len(t_env))
    if t_env[tail].size > 8:
        row["decay_db_s"] = round(float(np.polyfit(t_env[tail], env_db[tail], 1)[0]), 2)
        row.update(double_decay(env_db[tail], t_env[tail]))
    if row.get("partials_hz"):
        row["partial_decay_db_s"] = partial_decay(held[a:], sr, row["partials_hz"])

    # Damper: note-off to 40 dB below the level it was still holding. Skipped
    # when the string had already stopped — a held C7 is 45 dB down long before
    # the key comes up, and "how fast the damper stopped it" then measures the
    # noise floor and reports it as a very fast damper.
    held_peak_db = float(_db(env.max()))
    off_db = float(_db(env[-1])) if env.size else -240.0
    row["decayed_before_note_off"] = bool(off_db < held_peak_db - 45.0)
    rel = mono[off:]
    if rel.size > sr // 20 and not row["decayed_before_note_off"]:
        t_rel, env_rel = _rms_envelope(_above_fundamental(rel, sr, row["f0_hz"]), sr)
        start_db = float(_db(env_rel[0]))
        under = np.where(_db(env_rel) < start_db - 40.0)[0]
        row["damper_release_ms"] = round(float(t_rel[under[0]] * 1000.0), 1) if under.size \
            else round(float(t_rel[-1] * 1000.0), 1)
        row["damper_capped"] = not bool(under.size)

    freqs, mag = _spectrum(held[a:b], sr)
    p = mag ** 2
    row["centroid_hz"] = round(float((freqs * p).sum() / max(p.sum(), 1e-20)), 1)
    if audio.ndim > 1 and audio.shape[1] == 2:
        left, right = audio[on:off, 0], audio[on:off, 1]
        denom = float(np.std(left) * np.std(right))
        corr = float(np.mean((left - left.mean()) * (right - right.mean())) / denom) if denom > 0 else 1.0
        row["stereo_width"] = round(1.0 - abs(corr), 3)
    return row


# --------------------------------------------------------------------------
# measure


def measure(cfg: dict, corpus_dir: Path, out_path: Path) -> int:
    manifest_path = corpus_dir / "manifest.json"
    if not manifest_path.exists():
        print(f"no corpus at {manifest_path} — run `capture.py corpus` first", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text())
    preroll_s = manifest["preroll_ms"] / 1000.0
    gate_s = manifest["gate_ms"] / 1000.0

    rows = []
    for i, rec in enumerate(manifest["renders"], 1):
        path = corpus_dir / rec["path"]
        if not path.exists():
            continue
        audio, sr = read_wav(path)
        m = measure_note(audio, sr, rec["note"], preroll_s=preroll_s, gate_s=gate_s)
        if not m:
            print(f"  {rec['id']}: nothing measurable", file=sys.stderr)
            continue
        rows.append({"timbre": rec["timbre"], "note": rec["note"],
                     "velocity": rec["velocity"], **m})
        if i % 20 == 0:
            print(f"  {i}/{len(manifest['renders'])}", file=sys.stderr)

    profile = {
        "id": cfg["id"],
        "label": cfg["label"],
        "measured_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "capture": {
            "plugin": manifest["plugin"],
            "params": manifest["params"],
            "sample_rate": manifest["sample_rate"],
            "gate_ms": manifest["gate_ms"],
            "tail": manifest["tail"],
            "preroll_ms": manifest["preroll_ms"],
            "timbres": manifest["timbres"],
            "notes": manifest["notes"],
            "velocities": manifest["velocities"],
        },
        "rows": rows,
        "summary": summarize(rows),
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(profile, indent=1, ensure_ascii=False) + "\n")
    print(f"\n{len(rows)} measured notes -> {out_path}", file=sys.stderr)
    print_summary(profile["summary"])
    return 0


def summarize(rows: list[dict]) -> dict:
    """The per-timbre curves: what a voice is fitted to rather than a table of takes."""
    out: dict = {}
    for row in rows:
        t = out.setdefault(row["timbre"], {"stretch_cents": {}, "inharmonicity_b": {},
                                           "level_dbfs": {}, "centroid_hz": {},
                                           "decay_db_s": {}, "damper_release_ms": {}})
        n = str(row["note"])
        v = str(row["velocity"])
        # The tuning and the stiffness belong to the string, not to how hard it
        # was hit, so they are averaged over velocity rather than tabulated by it.
        if "cents_vs_et" in row:
            t["stretch_cents"].setdefault(n, []).append(row["cents_vs_et"])
        if "inharmonicity_b" in row and row.get("inharmonicity_reliable"):
            t["inharmonicity_b"].setdefault(n, []).append(row["inharmonicity_b"])
        for key, dest in (("rms_dbfs", "level_dbfs"), ("centroid_hz", "centroid_hz"),
                          ("decay_db_s", "decay_db_s"), ("damper_release_ms", "damper_release_ms")):
            if key in row:
                t[dest].setdefault(n, {})[v] = row[key]
    for t in out.values():
        for key in ("stretch_cents", "inharmonicity_b"):
            t[key] = {n: round(float(np.median(vals)), 4 if key == "stretch_cents" else 8)
                      for n, vals in sorted(t[key].items(), key=lambda kv: int(kv[0]))}
    return out


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


# --------------------------------------------------------------------------
# compare


def partial_balance_db(partials_db: list[float] | None) -> float | None:
    """Mean level of partials 2-6 relative to the fundamental.

    The one number that says whether a note has a piano's partial stack at all.
    Centroid does not: broadband strike noise carries a centroid perfectly well
    while the tonal spectrum underneath has collapsed to a sine, which is what
    an over-long hammer contact does to the treble. Inharmonicity does not
    either — it needs six partials before it reports anything, so a note this
    broken reads as "unmeasurable" rather than as wrong.
    """
    if not partials_db or len(partials_db) < 6:
        return None
    ref = partials_db[0]
    upper = [d for d in partials_db[1:6] if d > -200.0]
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

    Wider than partial_balance_db and parameterized, because the way a piano
    responds to velocity is not one number: a real hammer moves the partials
    ABOVE the felt's cutoff and leaves the ones below it alone, so the h2-h7 and
    the h8-h16 bands answer different questions and averaging them together
    hides which of the two the model gets wrong.
    """
    if not partials_db or len(partials_db) < lo:
        return None
    ref = partials_db[0]
    band = [d for d in partials_db[lo - 1:hi] if d > -200.0]
    if not band:
        return None
    return float(np.mean(band) - ref)


def dynamics(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int]) -> int:
    """How much the timbre changes from the softest blow to the hardest.

    `compare` scores one velocity at a time, so a model whose every velocity is
    individually plausible can still have the wrong DYNAMICS -- the axis a
    pianist actually plays. This tabulates the pp->ff swing instead of the
    absolute value: level in dB, and the two partial bands relative to the
    fundamental so a swing that is merely "louder" is separated from one that is
    "brighter". A physical hammer model gets this from its contact solver, so a
    mismatch here is structural and no amount of per-note voicing reaches it.
    """
    if not profile_path.exists():
        print(f"no profile at {profile_path} — run `profile.py measure` first")
        return 2
    profile = json.loads(profile_path.read_text())
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    tail_s = 2.0

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}")
        return 2
    notes = sorted({n for n, _ in ref if not notes_filter or n in notes_filter})
    vels = sorted({v for _, v in ref})
    if len(vels) < 2:
        print("the profile has a single velocity — nothing to compare across")
        return 2
    lo_v, hi_v = vels[0], vels[-1]

    print(f"model vs {timbre}: velocity {lo_v} -> {hi_v} swing, {len(notes)} notes")
    print("(each column is the change ACROSS velocity, not the absolute value)\n")
    print(f"{'note':>5} | {'level model':>11} {'level ref':>9} | {'h2-7 model':>10} {'h2-7 ref':>8} "
          f"| {'h8-16 model':>11} {'h8-16 ref':>9}")
    print("-" * 82)

    swing: dict[str, list[float]] = {}
    for note in notes:
        got = {}
        for v in (lo_v, hi_v):
            if (note, v) not in ref:
                break
            smf = write_smf([Note(note, v, preroll_s, gate_s)], program=0, end_pad=tail_s)
            audio = render_model(smf, preroll_s + gate_s + tail_s, cap["sample_rate"])
            m = measure_note(audio, cap["sample_rate"], note, preroll_s=preroll_s, gate_s=gate_s)
            if not m:
                break
            got[v] = (m, ref[(note, v)])
        if len(got) < 2:
            print(f"{note:5d} | not measurable at both velocities")
            continue
        (m_lo, r_lo), (m_hi, r_hi) = got[lo_v], got[hi_v]

        def delta(a, b, key):
            if key not in a or key not in b:
                return None
            return a[key] - b[key]

        def band_delta(a, b, lo, hi):
            x = band_db(a.get("partials_db"), lo, hi)
            y = band_db(b.get("partials_db"), lo, hi)
            return None if x is None or y is None else x - y

        cols = {
            "level_m": delta(m_hi, m_lo, "peak_dbfs"), "level_r": delta(r_hi, r_lo, "peak_dbfs"),
            "low_m": band_delta(m_hi, m_lo, 2, 7), "low_r": band_delta(r_hi, r_lo, 2, 7),
            "high_m": band_delta(m_hi, m_lo, 8, 16), "high_r": band_delta(r_hi, r_lo, 8, 16),
        }
        for k, v in cols.items():
            if v is not None and np.isfinite(v):
                swing.setdefault(k, []).append(float(v))

        def fmt(v, width):
            return format(v, f"+{width}.1f") if v is not None and np.isfinite(v) else "n/a".rjust(width)

        print(f"{note:5d} | {fmt(cols['level_m'], 11)} {fmt(cols['level_r'], 9)} "
              f"| {fmt(cols['low_m'], 10)} {fmt(cols['low_r'], 8)} "
              f"| {fmt(cols['high_m'], 11)} {fmt(cols['high_r'], 9)}")

    print("\nmedian swing (model vs reference, and the error between them):")
    for label, mk, rk in (("level (dB)", "level_m", "level_r"),
                          ("h2-h7 vs h1 (dB)", "low_m", "low_r"),
                          ("h8-h16 vs h1 (dB)", "high_m", "high_r")):
        if mk not in swing or rk not in swing:
            continue
        mm, rr = float(np.median(swing[mk])), float(np.median(swing[rk]))
        print(f"  {label:22s} model {mm:+7.2f}   ref {rr:+7.2f}   error {mm - rr:+7.2f}")
    return 0


def compare(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int]) -> int:
    """Measure libsonare the same way and print the difference, dimension by dimension."""
    if not profile_path.exists():
        print(f"no profile at {profile_path} — run `profile.py measure` first")
        return 2
    profile = json.loads(profile_path.read_text())
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    tail_s = 2.0

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}; it has "
              f"{sorted({r['timbre'] for r in profile['rows']})}")
        return 2

    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    a4_off = a4_offset_cents(profile["rows"], timbre)
    print(f"model vs {timbre}: {len(pairs)} notes")
    print(f"reference A4 sits {a4_off:+.2f} c from 440 Hz; "
          f"that offset is removed from the stretch column\n")
    print(f"{'note':>5} {'vel':>4} | {'stretch Δc':>10} {'B model':>10} {'B ref':>10} "
          f"{'decay Δdb/s':>12} {'h2-6 model':>10} {'h2-6 ref':>9} "
          f"{'centroid Δ%':>12} {'damper Δms':>11}")
    print("-" * 104)

    deltas: dict[str, list[float]] = {}
    damper_censored: list[tuple[int, int]] = []
    for note, vel in pairs:
        smf = write_smf([Note(note, vel, preroll_s, gate_s)],
                        program=0, end_pad=tail_s)
        audio = render_model(smf, preroll_s + gate_s + tail_s, cap["sample_rate"])
        m = measure_note(audio, cap["sample_rate"], note, preroll_s=preroll_s, gate_s=gate_s)
        r = ref[(note, vel)]
        if not m:
            print(f"{note:5d} {vel:4d} | model rendered nothing measurable")
            continue

        def d(key, scale=1.0):
            if key not in m or key not in r:
                return None
            return (m[key] - r[key]) * scale

        stretch = d("cents_vs_et")
        # A capped release never reached -40 dB inside the capture's tail, so
        # the number recorded for it is the length of the window and not a
        # measurement. Differencing two of those, or one against a real one,
        # yields a delta that moves when the tail length changes and never when
        # the voice does; the row is shown and left out of the median instead.
        damper_capped = bool(m.get("damper_capped") or r.get("damper_capped"))
        if damper_capped:
            damper_censored.append((note, vel))
        row = {
            "stretch": None if stretch is None else stretch + a4_off,
            "decay": d("decay_db_s"),
            "damper": None if damper_capped else d("damper_release_ms"),
        }
        centroid_pct = None
        if "centroid_hz" in m and r.get("centroid_hz"):
            centroid_pct = 100.0 * (m["centroid_hz"] / r["centroid_hz"] - 1.0)
        bal_m = partial_balance_db(m.get("partials_db"))
        bal_r = partial_balance_db(r.get("partials_db"))
        row["balance"] = None if bal_m is None or bal_r is None else bal_m - bal_r
        for k, v in list(row.items()) + [("centroid_pct", centroid_pct)]:
            if v is not None and np.isfinite(v):
                deltas.setdefault(k, []).append(float(v))

        def fmt(v, spec):
            return format(v, spec) if v is not None and np.isfinite(v) else "n/a".rjust(len(format(0, spec)))

        damper_col = fmt(d("damper_release_ms"), '+10.1f') + ("*" if damper_capped else " ")
        print(f"{note:5d} {vel:4d} | {fmt(row['stretch'], '+10.1f')} "
              f"{m.get('inharmonicity_b', float('nan')):10.3e} {r.get('inharmonicity_b', float('nan')):10.3e} "
              f"{fmt(row['decay'], '+12.2f')} {fmt(bal_m, '+10.1f')} {fmt(bal_r, '+9.1f')} "
              f"{fmt(centroid_pct, '+12.1f')} {damper_col}")

    if damper_censored:
        print(f"\n* {len(damper_censored)} of {len(pairs)} rows never fell 40 dB inside the "
              f"{tail_s:.0f} s tail on one side or the other; shown, not counted:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v in damper_censored))
    print("\nmedian delta:")
    labels = {"stretch": "tuning vs the reference (cents)",
              "decay": "held-note decay (dB/s)",
              "damper": "damper release (ms)",
              "balance": "partial stack h2-h6 vs h1 (dB)",
              "centroid_pct": "brightness (% of the reference centroid)"}
    for k, vals in deltas.items():
        print(f"  {labels.get(k, k):46s} {np.median(vals):+9.2f}")
    return 0


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, help_text in (("measure", "corpus WAVs -> reference profile JSON"),
                            ("compare", "render the same grid through libsonare and diff it"),
                            ("dynamics", "diff the pp->ff swing rather than one velocity at a time")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--config", default=str(DEFAULT_CONFIG))
        p.add_argument("--corpus", default="", help="corpus directory (default: the capture output)")
        p.add_argument("--profile", default="", help="profile JSON (default: reference/<id>.json)")
    for name in ("compare", "dynamics"):
        p = sub.choices[name]
        p.add_argument("--timbre", default="", help="which captured timbre to compare against")
        p.add_argument("--notes", default="", help="restrict to these MIDI notes, comma-separated")

    args = ap.parse_args()
    cfg = load_config(Path(args.config))
    corpus_dir = Path(args.corpus).resolve() if args.corpus else out_root(cfg, "")
    profile_path = Path(args.profile) if args.profile else REFERENCE_DIR / f"{cfg['id']}.json"

    if args.cmd == "measure":
        return measure(cfg, corpus_dir, profile_path)
    timbre = args.timbre or cfg["timbres"][0]["id"]
    notes_filter = {int(x) for x in args.notes.split(",") if x.strip()}
    if args.cmd == "dynamics":
        return dynamics(cfg, profile_path, timbre=timbre, notes_filter=notes_filter)
    return compare(cfg, profile_path, timbre=timbre, notes_filter=notes_filter)


if __name__ == "__main__":
    sys.exit(main())
