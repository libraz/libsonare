#!/usr/bin/env python3
"""Turn a captured corpus into a reference profile, and score a model against it.

    profile.py measure                 # corpus WAVs -> reference/<id>.json
    profile.py compare                 # render the same grid through libsonare
                                       # and report where it diverges

The audio a capture produces cannot be committed — a sample library's licence
covers what is rendered from it — and would be the wrong thing to commit even
if it could: what a physical model has to reproduce is a handful of measured
properties, not four hundred megabytes of one particular instrument. `measure`
extracts those properties and they are what lives in the repository, under
`reference/<capture id>.json`.

What it measures is everything a stiff, plucked or struck string can be wrong
about. None of it is in `metrics.py`, whose per-note analysis assumes an exact
harmonic series and a probe rendered by both sides:

- **Inharmonicity.** A stiff string puts its nth partial at
  `n*f0*sqrt(1 + B*n^2)` rather than at `n*f0`. B is measured by fitting that
  law to the partials that are actually there, not assumed — which is what
  makes the same measurement carry a piano (1e-4 in the middle to over 1e-2 at
  the top, and most of what separates a piano from a harp) and a harpsichord
  (near zero on the speaking length, and non-zero only through the segment
  behind the bridge).
- **Stretch tuning.** An instrument with inharmonic strings is deliberately
  tuned away from equal temperament, and by how much is a property of the
  instrument. A model tuned to exact equal temperament beats against the
  recording, and the beating is the first thing anyone hears.
- **Double decay.** A string does not decay along one line: the strings of a
  unison fall out of phase, and the fast initial decay gives way to a slower
  aftersound at a knee the fit locates rather than assumes. Where that knee
  falls is itself the evidence for how many strings are sounding.
- **Damper release.** Note-off is a damper landing on a moving string, which is
  neither instant nor an exponential release — and on some instruments part of
  the compass has no damper at all, which this reports rather than averages in.
- **Tone-to-noise.** The mechanism against the string. A model with the partial
  stack right and no action noise reads cleaner than any recording, which every
  spectral metric scores as an improvement.
- **Velocity response.** How far the level moves from the softest blow to the
  hardest, and whether it moves monotonically. On a plucked instrument this is
  most of the instrument's identity and no timbre metric can see it.

`compare` renders the same note-and-velocity grid through libsonare's GM
fallback — on the program the capture names, not a fixed one — measures it the
same way, and prints the differences. That is the part which says what to
change. `--gate` holds those differences to bounds recorded earlier, which is
what catches the change that improves one dimension by breaking another.
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
from wavio import read_wav, write_wav  # noqa: E402

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


def tone_to_noise_db(freqs: np.ndarray, mag: np.ndarray, f0_hz: float,
                     n_partials: int = 16) -> float:
    """Energy in the partials against everything else in the audible band.

    What it separates is the string from the mechanism: a plucked or struck
    instrument's noise floor is its own action — a plectrum scraping, a hammer's
    felt, a jack falling back — and a model that has the partial stack right and
    no mechanism noise reads far cleaner than any recording of the real thing.
    That reads as an improvement on every spectral metric and is the single most
    audible thing still missing.

    The partial window is the wider of ±2 % and three FFT bins. A relative-only
    window collapses below one bin at a low fundamental — at 44 Hz it asks for
    ±0.9 Hz out of a 1.7 Hz grid — so a bass note would score as pure noise for
    a reason that is entirely the analysis window's.
    """
    if freqs.size < 2 or f0_hz <= 0.0:
        return float("nan")
    power = mag.astype(np.float64) ** 2
    bin_hz = float(freqs[1] - freqs[0])
    tonal = np.zeros(freqs.shape, dtype=bool)
    for n in range(1, n_partials + 1):
        target = n * f0_hz
        if target > freqs[-1]:
            break
        half = max(0.02 * target, 3.0 * bin_hz)
        tonal |= (freqs >= target - half) & (freqs <= target + half)
    band = (freqs >= 40.0) & (freqs <= 12000.0)
    tone = float(power[tonal & band].sum())
    noise = float(power[~tonal & band].sum())
    return float(10.0 * np.log10((tone + 1e-30) / (noise + 1e-30)))


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
    tnr = tone_to_noise_db(freqs, mag, row["f0_hz"])
    if np.isfinite(tnr):
        row["tnr_db"] = round(tnr, 2)
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
            # The GM program the model answers this reference with. Recorded here
            # rather than taken from whichever config a later `compare` is handed,
            # so a profile cannot be diffed against a different instrument.
            "program": int(cfg.get("program", 0)),
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


def render_grid(cfg: dict, corpus_dir: Path, *, timbre: str, program: int) -> int:
    """Render the model over the capture's own grid, as one more timbre of it.

    The oracle corpus is one WAV per (note, velocity), and the model has to be
    measured the same way before a difference between them means anything.
    Writing it into the same directory under the same names, and registering it
    in the same manifest, means every tool that reads a corpus reads the model
    with no special case: `measure` turns it into a profile the reference can be
    diffed against, and `load_corpus` will serve it as a timbre.

    The grid comes from the capture definition, not from constants matched to it
    by hand. A hand-copied grid drifts the first time a note is added to the
    capture, and what it produces then is a model measured on notes the
    reference does not have — which reads as a reference that is missing rows.
    """
    manifest_path = corpus_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    preroll_s = manifest.get("preroll_ms", cfg["preroll_ms"]) / 1000.0
    gate_s = manifest.get("gate_ms", cfg["gate_ms"]) / 1000.0
    sr = int(manifest.get("sample_rate", cfg["sample_rate"]))
    notes = manifest.get("notes") or cfg["notes"]
    velocities = manifest.get("velocities") or cfg["velocities"]
    tail_s = 2.0

    out_dir = corpus_dir / timbre
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    total = len(notes) * len(velocities)
    for i, (note, vel) in enumerate(
        ((n, v) for n in notes for v in velocities), start=1
    ):
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program,
                        end_pad=tail_s)
        audio = render_model(smf, preroll_s + gate_s + tail_s, sr)
        rel = Path(timbre) / f"n{note:03d}_v{vel:03d}.wav"
        write_wav(corpus_dir / rel, np.asarray(audio), sr)
        peak = float(np.abs(audio).max())
        rows.append({"id": f"{timbre}/n{note:03d}_v{vel:03d}", "timbre": timbre,
                     "note": note, "velocity": vel, "path": str(rel),
                     "peak": peak, "seconds": round(preroll_s + gate_s + tail_s, 2)})
        print(f"[{i}/{total}] {rel} peak {peak:.4f}", file=sys.stderr)

    header = manifest or {
        "id": cfg["id"], "config": cfg.get("_path", ""), "plugin": "libsonare",
        "sample_rate": sr, "gate_ms": cfg["gate_ms"], "tail": cfg["tail"],
        "preroll_ms": cfg["preroll_ms"], "settle_ms": cfg.get("settle_ms", 0),
        "realtime": False, "params": [], "timbres": [], "notes": notes,
        "velocities": velocities,
    }
    # Replace this timbre's rows rather than appending to them, so re-rendering
    # after a voice edit leaves one grid behind and not two generations of one.
    kept = [r for r in header.get("renders", []) if r.get("timbre") != timbre]
    timbres = [t for t in header.get("timbres", []) if t.get("id") != timbre]
    timbres.append({"id": timbre, "label": f"libsonare, GM program {program}",
                    "model": True})
    header["timbres"] = timbres
    header["renders"] = sorted(kept + rows, key=lambda r: r["id"])
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(header, indent=1) + "\n")
    print(f"\n{len(rows)} renders in {out_dir}, registered as timbre {timbre!r}",
          file=sys.stderr)
    return 0


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


# --------------------------------------------------------------------------
# compare


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


def dynamics(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int]) -> int:
    """How much the timbre changes from the softest blow to the hardest.

    `compare` scores one velocity at a time, so a model whose every velocity is
    individually plausible can still have the wrong DYNAMICS -- the axis a
    pianist actually plays. This tabulates the pp->ff swing instead of the
    absolute value: level in dB, and the two partial bands relative to the
    fundamental so a swing that is merely "louder" is separated from one that is
    "brighter". A physical model gets this out of its excitation solver rather
    than out of a curve, so a mismatch here is structural: no amount of per-note
    voicing reaches it, and `diagnose.py` is where to take it.
    """
    if not profile_path.exists():
        print(f"no profile at {profile_path} — run `profile.py measure` first")
        return 2
    profile = json.loads(profile_path.read_text())
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    tail_s = 2.0

    program = profile_program(profile, cfg)
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
            smf = write_smf([Note(note, v, preroll_s, gate_s)], program=program,
                            end_pad=tail_s)
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


def compare(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int],
            gate_path: str = "", write_gate: str = "", margin: float = 1.25) -> int:
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

    program = profile_program(profile, cfg)
    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    a4_off = a4_offset_cents(profile["rows"], timbre)
    print(f"model vs {timbre}: {len(pairs)} notes, model on GM program {program}")
    print(f"reference A4 sits {a4_off:+.2f} c from 440 Hz; "
          f"that offset is removed from the stretch column\n")
    print(f"{'note':>5} {'vel':>4} | {'stretch Δc':>10} {'B model':>10} {'B ref':>10} "
          f"{'decay Δdb/s':>12} {'h2-6 model':>10} {'h2-6 ref':>9} "
          f"{'centroid Δ%':>12} {'TNR Δdb':>9} {'damper Δms':>11}")
    print("-" * 114)

    deltas: dict[str, list[float]] = {}
    damper_censored: list[tuple[int, int]] = []
    # Peak level per side, per note, per velocity. The dynamic range is the one
    # dimension no single (note, velocity) row can carry: it is the difference
    # between two of them, so it is accumulated here and reduced after the loop.
    peaks: dict[str, dict[int, dict[int, float]]] = {}
    for note, vel in pairs:
        smf = write_smf([Note(note, vel, preroll_s, gate_s)],
                        program=program, end_pad=tail_s)
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
            # Positive means the model is the cleaner of the two, which is the
            # direction this almost always fails in: the mechanism noise a real
            # action makes is the part a physical model most often has no
            # mechanism for at all.
            "tnr": d("tnr_db"),
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
        for side, src in (("m", m), ("r", r)):
            if "peak_dbfs" in src:
                peaks.setdefault(side, {}).setdefault(note, {})[vel] = src["peak_dbfs"]

        def fmt(v, spec):
            return format(v, spec) if v is not None and np.isfinite(v) else "n/a".rjust(len(format(0, spec)))

        damper_col = fmt(d("damper_release_ms"), '+10.1f') + ("*" if damper_capped else " ")
        print(f"{note:5d} {vel:4d} | {fmt(row['stretch'], '+10.1f')} "
              f"{m.get('inharmonicity_b', float('nan')):10.3e} {r.get('inharmonicity_b', float('nan')):10.3e} "
              f"{fmt(row['decay'], '+12.2f')} {fmt(bal_m, '+10.1f')} {fmt(bal_r, '+9.1f')} "
              f"{fmt(centroid_pct, '+12.1f')} {fmt(row['tnr'], '+9.1f')} {damper_col}")

    if damper_censored:
        print(f"\n* {len(damper_censored)} of {len(pairs)} rows never fell 40 dB inside the "
              f"{tail_s:.0f} s tail on one side or the other; shown, not counted:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v in damper_censored))
    for note, model_peaks in sorted(peaks.get("m", {}).items()):
        ref_peaks = peaks.get("r", {}).get(note, {})
        shared = sorted(set(model_peaks) & set(ref_peaks))
        if len(shared) < 2:
            continue
        span = ([model_peaks[v] for v in shared], [ref_peaks[v] for v in shared])
        deltas.setdefault("vel_range", []).append(
            (max(span[0]) - min(span[0])) - (max(span[1]) - min(span[1]))
        )
    summary = select_dimensions(summarize_deltas(deltas), cfg.get("dimensions") or [])
    print("\n" + f"{'':46s} {'median':>9} {'|median|':>9} {'rows':>5}")
    for k, row in summary.items():
        print(f"  {DELTA_LABELS.get(k, k):46s} {row['median']:+9.2f} "
              f"{row['abs_median']:9.2f} {row['n']:5d}")
    print("\n  The signed median is what the voice is doing on average and the absolute one "
          "is\n  how far any given note is from the reference. They part company exactly where "
          "a\n  summary is least trustworthy: errors of opposite sign in different registers "
          "cancel\n  in the first column and do not in the second.")

    if gate_path:
        return check_gate(summary, Path(gate_path), timbre)
    if write_gate:
        return write_gate_file(summary, Path(write_gate), timbre, margin)
    return 0


DELTA_LABELS = {"stretch": "tuning vs the reference (cents)",
                "decay": "held-note decay (dB/s)",
                "damper": "damper release (ms)",
                "balance": "partial stack h2-h6 vs h1 (dB)",
                "centroid_pct": "brightness (% of the reference centroid)",
                "tnr": "tone-to-noise, + = model is cleaner (dB)",
                "vel_range": "softest-to-hardest level range (dB)"}


def select_dimensions(summary: dict[str, dict], wanted: list[str]) -> dict[str, dict]:
    """Narrow the summary to the dimensions this instrument is judged on.

    Every dimension is measured for every instrument, because measuring is
    cheap and a number nobody looks at costs nothing. What is not free is
    GATING on one that does not apply: a harpsichord's top octave has no
    dampers at all, so its damper column compares one arbitrary tail against
    another and a bound recorded from it fails on whichever way the noise fell.
    A capture that lists no dimensions is judged on all of them, which is the
    right default — an instrument earns an exclusion by having a reason.

    A named dimension that was not measured is reported rather than dropped:
    silence there reads as "that dimension was fine".
    """
    if not wanted:
        return summary
    missing = [d for d in wanted if d not in summary]
    if missing:
        print(f"\nthese dimensions are named by the capture and were not measured in this "
              f"run: {', '.join(missing)}", file=sys.stderr)
    return {k: v for k, v in summary.items() if k in wanted}


def summarize_deltas(deltas: dict[str, list[float]]) -> dict[str, dict]:
    """Reduce each dimension's per-row deltas to the pair of numbers a gate reads.

    Two numbers rather than one, because the signed median alone cannot fail on
    a defect that is symmetric across the keyboard. The brightness column once
    read +0.16 % of the reference centroid while individual notes were between
    26 and 660 % out — the bass was dark by as much as the treble was bright,
    and the median of the signed errors said the voice was correct to a sixth of
    a percent. The absolute median is what that summary was missing.
    """
    return {
        k: {"median": float(np.median(v)), "abs_median": float(np.median(np.abs(v))),
            "n": len(v)}
        for k, v in deltas.items()
    }


def check_gate(summary: dict[str, dict], gate_path: Path, timbre: str) -> int:
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
    bounds = gate.get("bounds", {})
    failures = []
    for key, bound in bounds.items():
        row = summary.get(key)
        if row is None:
            failures.append(f"{DELTA_LABELS.get(key, key)}: not measured in this run")
            continue
        for stat in ("median", "abs_median"):
            limit = bound.get(stat)
            if limit is None:
                continue
            value = abs(row[stat]) if stat == "median" else row[stat]
            if value > limit:
                failures.append(
                    f"{DELTA_LABELS.get(key, key)}: {stat} {value:.2f} over its bound {limit:.2f}"
                )
    print(f"\ngate: {gate_path.name}, {len(bounds)} dimensions")
    if failures:
        for line in failures:
            print(f"  FAIL  {line}")
        print(f"  {len(failures)} of the recorded bounds were exceeded. If the change is "
              f"deliberate, re-record with --write-gate in the same commit as the change "
              f"that justifies it.")
        return 1
    print("  every recorded bound held")
    return 0


def write_gate_file(summary: dict[str, dict], gate_path: Path, timbre: str,
                    margin: float) -> int:
    """Record the current numbers as the bounds a later run is held to.

    The margin is multiplicative and deliberately not tight: a bound that fails
    on measurement noise gets switched off, and a switched-off gate catches
    nothing. There is also a floor under each bound, since a dimension that
    happens to be near zero today would otherwise be held to a tolerance no
    change could stay inside.
    """
    floors = {"stretch": 1.0, "decay": 0.5, "damper": 5.0, "balance": 0.5,
              "centroid_pct": 1.0, "tnr": 1.0, "vel_range": 1.0}
    bounds = {}
    for key, row in summary.items():
        floor = floors.get(key, 1.0)
        bounds[key] = {
            "median": round(max(abs(row["median"]) * margin, floor), 3),
            "abs_median": round(max(row["abs_median"] * margin, floor), 3),
        }
    gate_path.parent.mkdir(parents=True, exist_ok=True)
    gate_path.write_text(json.dumps({
        "_": "Bounds the compare table is held to. Both are absolute limits: 'median' caps "
             "the magnitude of the signed median and 'abs_median' caps the median absolute "
             "error, which is the one that can fail when errors of opposite sign cancel. "
             "Re-record only in the same change as the behaviour that justifies it.",
        "timbre": timbre,
        "margin": margin,
        "bounds": bounds,
    }, indent=2) + "\n")
    print(f"\nwrote {gate_path} — {len(bounds)} bounds at {margin:g}x the measured values")
    return 0


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, help_text in (("measure", "corpus WAVs -> reference profile JSON"),
                            ("render-grid", "render the model over the capture's own grid, "
                                            "as one more timbre of the corpus"),
                            ("compare", "render the same grid through libsonare and diff it"),
                            ("dynamics", "diff the pp->ff swing rather than one velocity at a time")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--config", default=str(DEFAULT_CONFIG))
        p.add_argument("--corpus", default="", help="corpus directory (default: the capture output)")
        p.add_argument("--profile", default="", help="profile JSON (default: reference/<id>.json)")
        p.add_argument("--program", type=int, default=None,
                       help="GM program the model answers with (default: the one the "
                            "capture definition names, or the one the profile recorded)")
    for name in ("compare", "dynamics"):
        p = sub.choices[name]
        p.add_argument("--timbre", default="", help="which captured timbre to compare against")
        p.add_argument("--notes", default="", help="restrict to these MIDI notes, comma-separated")
    sub.choices["render-grid"].add_argument(
        "--timbre", default="model",
        help="name the model's grid takes in the corpus (default: model)")
    gate_group = sub.choices["compare"]
    gate_group.add_argument(
        "--gate", default="",
        help="hold the summary to the bounds in this JSON and exit 1 when one is exceeded. "
             "Catches the change that improves one dimension by breaking another, which is "
             "the shape most voice work takes and the one a listening test finds last")
    gate_group.add_argument(
        "--write-gate", default="", dest="write_gate",
        help="record the current summary as bounds at --margin times its values. Do this in "
             "the same change as the behaviour that justifies the new numbers")
    gate_group.add_argument(
        "--margin", type=float, default=1.25,
        help="slack in a written bound (default: 1.25x). Loose on purpose — a bound that "
             "fails on measurement noise gets switched off, and then it catches nothing")

    args = ap.parse_args()
    cfg = load_config(Path(args.config))
    if args.program is not None:
        cfg["program"] = args.program
        cfg["_program_override"] = True
    corpus_dir = Path(args.corpus).resolve() if args.corpus else out_root(cfg, "")
    profile_path = Path(args.profile) if args.profile else REFERENCE_DIR / f"{cfg['id']}.json"

    if args.cmd == "measure":
        return measure(cfg, corpus_dir, profile_path)
    if args.cmd == "render-grid":
        return render_grid(cfg, corpus_dir, timbre=args.timbre,
                           program=int(cfg.get("program", 0)))
    timbre = args.timbre or cfg["timbres"][0]["id"]
    notes_filter = {int(x) for x in args.notes.split(",") if x.strip()}
    if args.cmd == "dynamics":
        return dynamics(cfg, profile_path, timbre=timbre, notes_filter=notes_filter)
    if args.gate and args.write_gate:
        print("--gate and --write-gate are alternatives: one checks the bounds and the "
              "other replaces them", file=sys.stderr)
        return 2
    return compare(cfg, profile_path, timbre=timbre, notes_filter=notes_filter,
                   gate_path=args.gate, write_gate=args.write_gate, margin=args.margin)


if __name__ == "__main__":
    sys.exit(main())
