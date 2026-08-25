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

A capture whose timbres sit on MIDI channel 10 is measured with none of that.
There its note numbers select instruments rather than pitches, so there is no
fundamental for any of the six to be about, and what is measured instead is the
1/3-octave band profile, the per-octave decay of it, the attack, the crest and
the velocity response. That path is `measure_hit` and `compare_percussion`, and
it is separate code rather than a branch: the columns that replace the pitched
ones are not the same columns under different names.

`compare` renders the same note-and-velocity grid through libsonare's GM
fallback — on the program the capture names, not a fixed one, and on the channel
it names — measures it the same way, and prints the differences. That is the
part which says what to change. `--gate` holds those differences to bounds
recorded earlier, which is what catches the change that improves one dimension
by breaking another.
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

from capture import (  # noqa: E402
    DEFAULT_CONFIG, load_config, note_groups, note_map, out_root,
)
from loss import KIT_MIN_MEMBERS, kit_report  # noqa: E402
from metrics import (  # noqa: E402
    INHARMONICITY_TOLERANCES, MAX_FIT_PARTIALS, MIN_PARTIALS_FOR_B,
    THIRD_OCTAVE_CENTERS, _db, _peak_near, _rms_envelope, _spectrum, analyze_hit,
    fit_partial_series, measure_band_edge, midi_to_hz, partial_hz, to_mono,
)
from render_model import render_model  # noqa: E402
from render_oracle import render_oracle_fluidsynth  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import read_wav, write_wav  # noqa: E402

REFERENCE_DIR = Path(__file__).resolve().parent / "reference"
# The partial count, the floor and the reliability gate all live with the fit in
# `metrics`, so the reference profile and the model-vs-oracle comparison read a
# string with one ruler. They were the same numbers when they were written twice;
# a quantity defined in two places only stays equal until one of them is tuned.
MAX_PARTIALS = MAX_FIT_PARTIALS
# One-based MIDI channel 10, on which a note number selects an instrument
# instead of a pitch. `write_smf` counts channels from zero.
PERCUSSION_CHANNEL = 10
# The 1/3-octave band centres `analyze_hit` reports, split into the two ends the
# tilt is taken between. The middle is left out on purpose: a kit's body lives
# there and every piece of it puts energy there, so including it averages the
# two ends towards each other and the tilt stops separating a dull snare from a
# bright one.
TILT_LOW_HZ = 500.0
TILT_HIGH_HZ = 2000.0
# A model render whose loudest sample is under this is a kit piece the GM
# fallback does not voice, not a quiet one. `analyze_hit` normalises the band
# profile to its own loudest band, so silence comes back as a flat spectrum and
# scores as a plausible instrument rather than as an absence.
SILENT_HIT_DBFS = -80.0
# Past this much delay between a note-on and the strike, the host scheduled the
# note rather than the instrument being slow to speak. Well above any real
# attack on a kit and well under the shortest delay a retried note came back at.
LATE_ONSET_MS = 20.0


# --------------------------------------------------------------------------
# which kind of instrument a capture is


def is_percussion(cap: dict) -> bool:
    """Whether this capture's note numbers select instruments rather than pitches.

    Read off the MIDI channel rather than from a flag of its own. The channel is
    where the distinction already lives — it is what makes a note number select
    an instrument, on the reference as much as in libsonare — and a second field
    saying the same thing is a field that can disagree with it.

    All timbres or none: a capture holding a kit on channel 10 and a melodic
    slot on channel 1 has no single answer, and guessing one would measure half
    of it with the wrong metric set.
    """
    timbres = cap.get("timbres") or []
    if not timbres:
        return False
    on_ten = [int(t.get("channel", 1)) == PERCUSSION_CHANNEL for t in timbres]
    if any(on_ten) and not all(on_ten):
        raise ValueError(
            "capture mixes percussion and melodic timbres: "
            f"{[t.get('id') for t, p in zip(timbres, on_ten) if p]} sit on MIDI "
            f"channel {PERCUSSION_CHANNEL} and the rest do not. Split them into "
            f"two capture definitions — one profile cannot be measured both ways"
        )
    return all(on_ten)


# --------------------------------------------------------------------------
# per-note measurement


def find_partials(seg: np.ndarray, sr: int, note: int) -> dict:
    """Locate the partials of one struck note and fit the stiff-string law.

    Returns the measured f0, the inharmonicity coefficient B, and the partial
    frequencies and levels that were actually found.

    The fit itself is `metrics.fit_partial_series`, which is also what the
    model-vs-oracle comparison reads a ladder with. That shared ruler is the
    point: a reference profile measured one way and a model scored another was
    the state this replaced, and the two disagreed by up to 1.8x on the same
    three-note piano probe. What stays here is the reporting — which partials
    were found at the fitted series, how strong each was, and whether there
    were enough of them for B to mean anything.
    """
    freqs, mag = _spectrum(seg, sr)
    et = midi_to_hz(note)
    f0_seed, a0 = _peak_near(freqs, mag, et, 120.0)
    if a0 <= 0.0:
        return {}
    f0, b, fitted_on = fit_partial_series(freqs, mag, f0_seed, a0, sr)
    found: list[tuple[int, float, float]] = []
    for n in range(1, MAX_PARTIALS + 1):
        predicted = partial_hz(f0, n, b)
        if predicted > 0.45 * sr:
            break
        fn, an = _peak_near(freqs, mag, predicted, INHARMONICITY_TOLERANCES[-1])
        if an > 0.0:
            found.append((n, fn, an))

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


def measure_hit(audio: np.ndarray, sr: int, note: int, velocity: int, *,
                preroll_s: float, gate_s: float,
                max_band_hz: float | None = None) -> dict:
    """One percussion hit, as a profile row.

    A struck instrument has no fundamental, so none of the measurements above it
    apply: there is no partial to be inharmonic, no temperament to be stretched
    against, and nothing for a tone-to-noise ratio to be a ratio of. What is
    left is the shape of the spectrum, how each band of it decays, and how hard
    the strike was — which is what `analyze_hit` reports.

    `peak_dbfs` is carried alongside so the velocity response is measured for a
    drum by exactly the code that measures it for a piano. It is also the only
    axis a drum note has: a different note is a different instrument, not the
    same one played higher.
    """
    mono = to_mono(audio)
    hit = analyze_hit(mono, sr, Note(note, velocity, preroll_s, gate_s), len(mono) / sr,
                      max_band_hz=max_band_hz)
    row = hit.to_dict()
    strike = mono[int(preroll_s * sr):]
    row["peak_dbfs"] = round(float(_db(float(np.max(np.abs(strike))))), 2) if len(strike) else None
    return row


def committed_capture(cfg: dict, tracked: dict, manifest: dict) -> dict:
    """The capture block a reference profile records: the method, never the identity.

    The manifest is written from the merged configuration, so it carries the
    untracked overlay's half — the plugin's component triple, and the preset
    each slot was loaded from. Copying it through would put a product name into
    a committed file, which is the one thing the split of a capture definition
    into a tracked half and a `.local.json` half exists to prevent.

    So the descriptive fields come from the tracked definition and only the
    grid comes from the manifest — which is also what makes the manifest worth
    reading here at all: it records what was *rendered*, and a resumed capture
    can have covered less than the definition asks for.

    Intersecting the two also drops the model's own grid. `render-grid` adds it
    to the corpus as one more timbre on purpose, so that every tool reading a
    corpus reads the model with no special case; a profile of the *reference*
    is the one place that is wrong, since it would describe the thing being
    measured as one of the measurements.
    """
    by_id = {t["id"]: t for t in tracked.get("timbres", [])}
    manifest_timbres = [t for t in manifest["timbres"] if t["id"] in by_id]
    return {
        # The GM program the model answers this reference with. Recorded here
        # rather than taken from whichever config a later `compare` is handed,
        # so a profile cannot be diffed against a different instrument.
        "program": int(cfg.get("program", 0)),
        "params": manifest["params"],
        "sample_rate": manifest["sample_rate"],
        "gate_ms": manifest["gate_ms"],
        "tail": manifest["tail"],
        "preroll_ms": manifest["preroll_ms"],
        "timbres": [by_id[t["id"]] for t in manifest_timbres],
        "notes": manifest["notes"],
        "velocities": manifest["velocities"],
    }


def measure(cfg: dict, corpus_dir: Path, out_path: Path) -> int:
    manifest_path = corpus_dir / "manifest.json"
    if not manifest_path.exists():
        print(f"no corpus at {manifest_path} — run `capture.py corpus` first", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text())
    preroll_s = manifest["preroll_ms"] / 1000.0
    gate_s = manifest["gate_ms"] / 1000.0
    percussion = is_percussion(cfg)

    # A reference profile is the calibration target, so the model's own grid is
    # not part of it even when `render-grid` has put it in the same corpus: it
    # would double the committed file with a snapshot of the thing being
    # calibrated, which is stale the next time the voice is touched.
    modelled = {t["id"] for t in manifest.get("timbres", []) if t.get("model")}

    def sweep(max_band_hz: float | None) -> list[dict]:
        rows: list[dict] = []
        for i, rec in enumerate(manifest["renders"], 1):
            if rec["timbre"] in modelled:
                continue
            path = corpus_dir / rec["path"]
            if not path.exists():
                continue
            audio, sr = read_wav(path)
            m = (measure_hit(audio, sr, rec["note"], rec["velocity"],
                             preroll_s=preroll_s, gate_s=gate_s,
                             max_band_hz=max_band_hz)
                 if percussion else
                 measure_note(audio, sr, rec["note"], preroll_s=preroll_s, gate_s=gate_s))
            if not m:
                print(f"  {rec['id']}: nothing measurable", file=sys.stderr)
                continue
            rows.append({"timbre": rec["timbre"], "note": rec["note"],
                         "velocity": rec["velocity"], **m})
            if i % 20 == 0:
                print(f"  {i}/{len(manifest['renders'])}", file=sys.stderr)
        return rows

    rows = sweep(None)
    # What this capture can actually measure, before anything is committed as
    # what the instrument does. A band profile is normalised to its own loudest
    # band, so the edge cannot be applied to a measured profile after the fact —
    # the whole sweep is taken again against it. See `measure_band_edge`.
    band_edge = measure_band_edge(rows) if percussion else None
    if band_edge is not None:
        print(f"\ncapture bandwidth: {band_edge / 1000.0:.1f} kHz — re-measuring so "
              f"the band profile is normalised over what this capture carries",
              file=sys.stderr)
        rows = sweep(band_edge)

    tracked = json.loads(Path(cfg["_path"]).read_text())
    profile = {
        "id": cfg["id"],
        "label": tracked["label"],
        "measured_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "capture": {**committed_capture(cfg, tracked, manifest),
                    # Part of the method, not of the instrument: it says what
                    # this recording chain could hear, so a later comparison can
                    # decline to score the model where the reference is blind.
                    "band_edge_hz": band_edge},
        "rows": rows,
        "summary": summarize_percussion(rows) if percussion else summarize(rows),
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(profile, indent=1, ensure_ascii=False) + "\n")
    print(f"\n{len(rows)} measured notes -> {out_path}", file=sys.stderr)
    if percussion:
        print_percussion_summary(profile["summary"])
    else:
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
    # On a kit the channel is not a transport detail: it is what makes the note
    # number select an instrument. Rendering the model's grid on channel 1 would
    # play 47 pitches of whatever program 0 is and write them into the corpus
    # under drum note names.
    channel = PERCUSSION_CHANNEL - 1 if is_percussion(cfg) else 0

    out_dir = corpus_dir / timbre
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    total = len(notes) * len(velocities)
    for i, (note, vel) in enumerate(
        ((n, v) for n in notes for v in velocities), start=1
    ):
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program,
                        channel=channel, end_pad=tail_s)
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


def band_tilt_db(bands_db: list[float] | None) -> float | None:
    """How much of a hit sits above 2 kHz rather than below 500 Hz.

    `analyze_hit` normalises the band profile to its own loudest band, so a
    whole-spectrum level offset has already been divided out and the only thing
    left to compare is the shape. This is the one number of that shape a listener
    would name first: a kit piece is dull or bright before it is anything else.
    """
    if not bands_db:
        return None
    centres = np.asarray(THIRD_OCTAVE_CENTERS[:len(bands_db)], dtype=np.float64)
    vals = np.asarray(bands_db[:len(centres)], dtype=np.float64)
    low, high = vals[centres <= TILT_LOW_HZ], vals[centres >= TILT_HIGH_HZ]
    if not len(low) or not len(high):
        return None
    return float(np.mean(high) - np.mean(low))


def band_shape_error_db(model: list[float] | None, ref: list[float] | None) -> float | None:
    """RMS distance between two normalised band profiles.

    A magnitude and never a direction, which is the point: the tilt above says
    which way a hit is wrong and this says how much of the error the tilt failed
    to account for. A piece can match on tilt and still be a different
    instrument, with a resonance in the wrong band and a hole beside it.
    """
    if not model or not ref:
        return None
    n = min(len(model), len(ref))
    if not n:
        return None
    diff = np.asarray(model[:n], dtype=np.float64) - np.asarray(ref[:n], dtype=np.float64)
    return float(np.sqrt(np.mean(diff**2)))


def mean_band_decay_delta(model: list, ref: list) -> float | None:
    """Model-minus-reference decay rate, averaged over the octaves both resolved.

    A band that decayed in neither render, or in only one of them, is left out
    rather than counted as agreement: `analyze_hit` reports `None` there, and
    a hit with no energy in a band has no decay rate to be right about.
    """
    pairs = [(m, r) for m, r in zip(model or [], ref or [])
             if m is not None and r is not None]
    return float(np.mean([m - r for m, r in pairs])) if pairs else None


def print_kit_relations(kit_rows: list[tuple[dict, dict]],
                        groups: dict[str, tuple[int, ...]]) -> None:
    """What the kit's own families do, against what the reference's do.

    Every column of the table above is one instrument against its own reference
    row. A kit is not 47 independent instruments, and the relation between its
    members — the tom series, the hi-hat trio — is where a kit stops sounding
    like a kit long before any single row looks wrong.

    Printed rather than gated, because the gate is a per-dimension median across
    every hit and these are per family: folding them in would average a six-tom
    finding into 282 rows and lose it. `--w-kit` is where the same measurement
    drives a search.
    """
    if not groups or not kit_rows:
        return
    rows = kit_report([m for m, _ in kit_rows], [r for _, r in kit_rows], groups)
    if not rows:
        print("\n  no kit relation could be read: no family the capture declares has "
              f"{KIT_MIN_MEMBERS} members in this run's note filter.")
        return
    print(f"\n{'family':>12} {'relation':>9} | {'reference':>10} {'model':>8} "
          f"{'charge':>8} {'members':>8}")
    print("-" * 62)
    for row in rows:
        print(f"{row['family']:>12} {row['relation']:>9} | {row['spread']:10.2f} "
              f"{row['model_spread']:8.2f} {row['charge']:8.2f} {row['members']:8d}")
    print("\n  In doublings — a factor of two in frequency, in milliseconds or in "
          "amplitude.\n  `reference` and `model` are how far the family's members "
          "spread apart on each\n  side; a model spread well under the reference's is a "
          "family collapsed towards\n  one instrument. A relation the reference does not "
          "itself hold is not listed:\n  which relations a family has is read off the "
          "rows, never declared.")


def compare_percussion(cfg: dict, profile: dict, *, timbre: str, notes_filter: set[int],
                       gate_path: str, write_gate: str, margin: float) -> int:
    """Score the model against a captured kit, one instrument per note.

    Separate from `compare` rather than a branch inside it, because almost
    nothing carries over: there is no stretch, no inharmonicity, no damper and
    no partial stack, and the columns that replace them are not the same columns
    under different names. What is shared is the shape of the answer — a signed
    and an absolute median per dimension — and the gate that holds it.
    """
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    tail_s = 2.0
    sr = cap["sample_rate"]
    # The reference's own ceiling, recorded when it was measured. Absent from a
    # profile measured before the edge was, and then the model is measured
    # full-range exactly as that profile was — a comparison is only fair if both
    # sides normalised over the same bands.
    band_edge = cap.get("band_edge_hz")
    mapping = note_map(cfg)

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}; it has "
              f"{sorted({r['timbre'] for r in profile['rows']})}")
        return 2

    program = profile_program(profile, cfg)
    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    print(f"model vs {timbre}: {len(pairs)} hits, model on GM kit {program}, "
          f"MIDI channel {PERCUSSION_CHANNEL}")
    if band_edge:
        print(f"reference bandwidth {band_edge / 1000.0:.1f} kHz — the band columns "
              f"are read over what the capture carries")
    if mapping:
        print("capture note -> model note: "
              + ", ".join(f"{k}->{v}" for k, v in sorted(mapping.items())))
    print()
    print(f"{'note':>5} {'vel':>4} | {'tilt Δdb':>9} {'shape db':>9} "
          f"{'decay Δdb/s':>12} {'centroid Δ%':>12} {'attack Δms':>11} "
          f"{'crest Δdb':>10} {'level Δdb':>10}")
    print("-" * 89)

    deltas: dict[str, list[float]] = {}
    peaks: dict[str, dict[int, dict[int, float]]] = {}
    silent: list[tuple[int, int]] = []
    # Kept side by side and index-aligned, for the relations no single row can
    # carry — see `loss.kit_report`. The reference row is stored under the note
    # it was CAPTURED on, since the families are declared in the capture's own
    # numbering, while the model row is whatever the map made it play.
    kit_rows: list[tuple[dict, dict]] = []
    for note, vel in pairs:
        played = mapping.get(note, note)
        smf = write_smf([Note(played, vel, preroll_s, gate_s)], program=program,
                        channel=PERCUSSION_CHANNEL - 1, end_pad=tail_s)
        audio = render_model(smf, preroll_s + gate_s + tail_s, sr)
        m = measure_hit(audio, sr, played, vel, preroll_s=preroll_s, gate_s=gate_s,
                        max_band_hz=band_edge)
        r = ref[(note, vel)]
        # A kit piece the model does not voice at all renders silence, and every
        # normalised metric scores silence as a flat spectrum rather than as an
        # absence. Named and left out, in the same way a capped release is.
        if m.get("peak_dbfs") is None or m["peak_dbfs"] <= SILENT_HIT_DBFS:
            silent.append((note, vel))
            print(f"{note:5d} {vel:4d} | model renders nothing above "
                  f"{SILENT_HIT_DBFS:.0f} dBFS")
            continue

        tilt_m, tilt_r = band_tilt_db(m.get("bands_db")), band_tilt_db(r.get("bands_db"))
        row = {
            "band_tilt": None if tilt_m is None or tilt_r is None else tilt_m - tilt_r,
            "band_shape": band_shape_error_db(m.get("bands_db"), r.get("bands_db")),
            "band_decay": mean_band_decay_delta(m.get("band_decay_db_s"),
                                                r.get("band_decay_db_s")),
            "attack": m["attack_ms"] - r["attack_ms"],
            "crest": m["crest_db"] - r["crest_db"],
            "centroid_pct": (100.0 * (m["centroid_hz"] / r["centroid_hz"] - 1.0)
                             if r.get("centroid_hz") else None),
            # How loud the hit actually is. Every other column here is
            # normalised — a band profile against its own loudest band, a crest
            # against its own RMS, a decay against its own peak — which is what
            # makes them measure timbre, and which also makes all of them blind
            # to gain. Rewriting eighteen of the kit's output levels moved not
            # one of them by a digit. `vel_range` is a span and cancels an
            # offset by construction, so it is not this either.
            "level": (m["peak_dbfs"] - r["peak_dbfs"]
                      if m.get("peak_dbfs") is not None
                      and r.get("peak_dbfs") is not None else None),
        }
        for k, v in row.items():
            if v is not None and np.isfinite(v):
                deltas.setdefault(k, []).append(float(v))
        for side, src in (("m", m), ("r", r)):
            if src.get("peak_dbfs") is not None:
                peaks.setdefault(side, {}).setdefault(note, {})[vel] = src["peak_dbfs"]
        kit_rows.append((m, {**r, "note": note, "velocity": vel}))

        def fmt(v, spec):
            return (format(v, spec) if v is not None and np.isfinite(v)
                    else "n/a".rjust(len(format(0, spec))))

        label = f"{note:5d}" if played == note else f"{note:3d}>{played:<2d}"
        print(f"{label} {vel:4d} | {fmt(row['band_tilt'], '+9.1f')} "
              f"{fmt(row['band_shape'], '9.1f')} {fmt(row['band_decay'], '+12.2f')} "
              f"{fmt(row['centroid_pct'], '+12.1f')} {fmt(row['attack'], '+11.1f')} "
              f"{fmt(row['crest'], '+10.1f')} {fmt(row['level'], '+10.1f')}")

    if silent:
        print(f"\n* {len(silent)} of {len(pairs)} hits are silent on the model side; "
              f"shown, not counted:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v in silent))
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
    print("\n  A kit has no register to average along: every row is a different "
          "instrument,\n  so the signed median says only how the kit leans as a whole and the "
          "absolute\n  one is the column to read. `band shape` is a magnitude already and its "
          "two\n  columns are the same number by construction.")

    print_kit_relations(kit_rows, note_groups(cfg))

    if gate_path:
        return check_gate(summary, Path(gate_path), timbre)
    if write_gate:
        return write_gate_file(summary, Path(write_gate), timbre, margin)
    return 0


def compare(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int],
            gate_path: str = "", write_gate: str = "", margin: float = 1.25) -> int:
    """Measure libsonare the same way and print the difference, dimension by dimension."""
    if not profile_path.exists():
        print(f"no profile at {profile_path} — run `profile.py measure` first")
        return 2
    profile = json.loads(profile_path.read_text())
    if is_percussion(profile["capture"]):
        return compare_percussion(cfg, profile, timbre=timbre, notes_filter=notes_filter,
                                  gate_path=gate_path, write_gate=write_gate, margin=margin)
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
                "vel_range": "softest-to-hardest level range (dB)",
                "band_tilt": "band tilt, + = model is brighter (dB)",
                "band_shape": "band profile error, magnitude only (dB)",
                "band_decay": "per-octave decay rate (dB/s)",
                "attack": "time to the peak of the strike (ms)",
                "crest": "peak over RMS of the hit (dB)",
                "level": "how loud the hit is vs the reference (dBFS)"}


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
# Two references, and where they agree


# How far apart two independent references may sit on a dimension and still be
# said to agree about it, in that dimension's own units.
#
# These are not tolerances on the model. They are the width of "what this
# instrument is", measured the only way it can be: by asking twice. Two
# libraries voicing the same GM instrument differ by their voicing decisions —
# a brighter ride, a longer crash — and inside that width there is no target to
# hit, so a model sitting anywhere in it is right and a fit that moves it is
# chasing whichever reference it was handed.
#
# Set at roughly the difference a listener would call a different choice rather
# than a different instrument. Level is the widest and the most important: a
# 6 dB disagreement about how loud a tambourine is says the kit balance is a mix
# decision, and a fit given one reference will happily take it 31 dB down.
AGREEMENT_TOLERANCE = {
    "level": 6.0,
    "band_tilt": 6.0,
    "band_shape": 6.0,
    "centroid_pct": 50.0,
    "attack": 5.0,
    "crest": 3.0,
}


def agree(cfg: dict, profile: dict, *, timbre: str, notes_filter: set[int]) -> int:
    """Measure a second reference over the same grid and report where the two agree.

    Every bound in `reference/*_gate.json` was decided by hand from one
    reference, and one reference cannot say which of its numbers are the
    instrument and which are its own voicing. That is not a documentation gap:
    working from a single reference, a tambourine was on its way to being pushed
    31 dB down — inaudible — because that is where the one reference put it.

    The second opinion is fluidsynth over the bundled SoundFont, which is
    already wired for the fit's oracle. It is not a better reference than the
    captured one and is not used as a target; it is an independent one, and the
    only question asked of it is where it and the capture say the same thing.

    Nothing is written. The output is the map a gate should be recorded from:
    a dimension the two agree on is worth bounding, and one they do not is worth
    measuring and leaving alone.
    """
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    tail_s = 2.0
    sr = cap["sample_rate"]
    band_edge = cap.get("band_edge_hz")
    program = profile_program(profile, cfg)
    if not is_percussion(cap):
        print("`agree` measures the percussion metric set; the pitched one has no "
              "second reference wired to it yet", file=sys.stderr)
        return 2

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}", file=sys.stderr)
        return 2
    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    print(f"capture {timbre!r} vs fluidsynth, {len(pairs)} hits, GM kit {program}\n")
    print(f"{'note':>5} {'vel':>4} | " + " ".join(f"{k:>12}" for k in AGREEMENT_TOLERANCE))
    print("-" * (12 + 13 * len(AGREEMENT_TOLERANCE)))

    agreed: dict[str, int] = {k: 0 for k in AGREEMENT_TOLERANCE}
    counted: dict[str, int] = {k: 0 for k in AGREEMENT_TOLERANCE}
    deltas: dict[str, list[float]] = {}
    for note, vel in pairs:
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program,
                        channel=PERCUSSION_CHANNEL - 1, end_pad=tail_s)
        try:
            audio = render_oracle_fluidsynth(smf, preroll_s + gate_s + tail_s, sr)
        except (FileNotFoundError, RuntimeError) as exc:
            print(f"second reference unavailable: {exc}", file=sys.stderr)
            return 2
        second = measure_hit(audio, sr, note, vel, preroll_s=preroll_s, gate_s=gate_s,
                             max_band_hz=band_edge)
        r = ref[(note, vel)]
        if second.get("peak_dbfs") is None or second["peak_dbfs"] <= SILENT_HIT_DBFS:
            print(f"{note:5d} {vel:4d} | the second reference does not voice this note")
            continue

        tilt_a, tilt_b = band_tilt_db(r.get("bands_db")), band_tilt_db(second.get("bands_db"))
        row = {
            "level": (second["peak_dbfs"] - r["peak_dbfs"]
                      if r.get("peak_dbfs") is not None else None),
            "band_tilt": None if tilt_a is None or tilt_b is None else tilt_b - tilt_a,
            "band_shape": band_shape_error_db(second.get("bands_db"), r.get("bands_db")),
            "centroid_pct": (100.0 * (second["centroid_hz"] / r["centroid_hz"] - 1.0)
                             if r.get("centroid_hz") else None),
            "attack": second["attack_ms"] - r["attack_ms"],
            "crest": second["crest_db"] - r["crest_db"],
        }
        cells = []
        for key, tol in AGREEMENT_TOLERANCE.items():
            v = row[key]
            if v is None or not np.isfinite(v):
                cells.append(f"{'n/a':>12}")
                continue
            counted[key] += 1
            deltas.setdefault(key, []).append(float(v))
            ok = abs(v) <= tol
            agreed[key] += 1 if ok else 0
            cells.append(f"{v:>11.1f}{'' if ok else '!'}")
        print(f"{note:5d} {vel:4d} | " + " ".join(cells))

    print(f"\n{'':16s} {'agree':>8} {'of':>5} {'median Δ':>10} {'tolerance':>10}")
    for key, tol in AGREEMENT_TOLERANCE.items():
        n = counted[key]
        med = float(np.median(deltas[key])) if deltas.get(key) else float("nan")
        print(f"  {key:14s} {agreed[key]:8d} {n:5d} {med:10.1f} {tol:10.1f}")
    print("\n  A dimension the two references agree on has a target in it and is worth "
          "gating.\n  One they do not is a voicing decision: measure it, print it, and do "
          "not fit to it —\n  the number a single reference gives there is that library's "
          "opinion, and a fit\n  handed it will follow it as far as it goes.")
    return 0


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, help_text in (("measure", "corpus WAVs -> reference profile JSON"),
                            ("render-grid", "render the model over the capture's own grid, "
                                            "as one more timbre of the corpus"),
                            ("compare", "render the same grid through libsonare and diff it"),
                            ("agree", "measure a second, independent reference over the same "
                                      "grid and report which dimensions the two agree on"),
                            ("dynamics", "diff the pp->ff swing rather than one velocity at a time")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--config", default=str(DEFAULT_CONFIG))
        p.add_argument("--corpus", default="", help="corpus directory (default: the capture output)")
        p.add_argument("--profile", default="", help="profile JSON (default: reference/<id>.json)")
        p.add_argument("--program", type=int, default=None,
                       help="GM program the model answers with (default: the one the "
                            "capture definition names, or the one the profile recorded)")
    for name in ("compare", "agree", "dynamics"):
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
    if args.cmd == "agree":
        if not profile_path.exists():
            print(f"no profile at {profile_path} — run `profile.py measure` first")
            return 2
        return agree(cfg, json.loads(profile_path.read_text()), timbre=timbre,
                     notes_filter=notes_filter)
    if args.gate and args.write_gate:
        print("--gate and --write-gate are alternatives: one checks the bounds and the "
              "other replaces them", file=sys.stderr)
        return 2
    return compare(cfg, profile_path, timbre=timbre, notes_filter=notes_filter,
                   gate_path=args.gate, write_gate=args.write_gate, margin=args.margin)


if __name__ == "__main__":
    sys.exit(main())
