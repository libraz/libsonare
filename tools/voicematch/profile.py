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

A capture whose timbres declare MIDI channel 10 is measured with none of that.
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
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from capture import (  # noqa: E402
    DEFAULT_CONFIG, PERCUSSION_CHANNEL, RIG_UNCLASSIFIED, load_config, model_rig, note_groups,
    note_map, out_root, tail_seconds,
)
from loss import KIT_MIN_MEMBERS, kit_report  # noqa: E402
from metrics import (  # noqa: E402
    INHARMONICITY_TOLERANCES, MAX_FIT_PARTIALS, MIN_PARTIALS_FOR_B,
    THIRD_OCTAVE_CENTERS, _db, _peak_near, _rms_envelope, _spectrum, analyze_hit,
    band_edges_by_timbre, fit_partial_series, midi_to_hz, partial_hz, shared_band_edge, to_mono,
)
from phrases import TAKE_SETS, build_takes  # noqa: E402
from _repo import REPO_ROOT  # noqa: E402
from render_model import render_model  # noqa: E402
from render_oracle import render_oracle_fluidsynth  # noqa: E402
from room import Room, match_sends, measurable_room, place_model_in  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import read_wav, write_wav  # noqa: E402

REFERENCE_DIR = Path(__file__).resolve().parent / "reference"
#: Every committed capture definition lives here. Globbed rather than listed:
#: the failure worth catching is an instrument added without being added to a
#: list, which a list cannot catch.
CAPTURE_DIR = Path(__file__).resolve().parent / "capture"
# The partial count, the floor and the reliability gate all live with the fit in
# `metrics`, so the reference profile and the model-vs-oracle comparison read a
# string with one ruler. They were the same numbers when they were written twice;
# a quantity defined in two places only stays equal until one of them is tuned.
MAX_PARTIALS = MAX_FIT_PARTIALS
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

    What CAN disagree with it is an address. A rack selects its slots by channel
    too, and a slot number written into this field claims a meaning it was never
    about: five melodic instruments that happened to sit in slot 10 were measured
    as drum maps, and the profiles came back with a band tilt and a crest for
    every note and a fundamental for none. So the slot has its own name,
    `capture.slot_channel`, and this field stays the semantic one.

    All timbres or none: a capture holding a kit on channel 10 and a melodic
    slot on channel 1 has no single answer, and guessing one would measure half
    of it with the wrong metric set.

    Tolerant of a channel outside `capture.NOTE_CHANNELS`, which `load_config`
    refuses — a reference profile carries the timbre block it was measured with,
    so this also reads capture definitions written before the slot had a name.
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


#: How far under its own peak an envelope is still the instrument. Past this a
#: held note is the recording's floor, the beating between two nearly-dead
#: strings, or -- on a trimmed sample -- digital silence, and a line fitted
#: through any of the three describes the file rather than the string. Measured
#: on this corpus, C7's reference envelope reaches -240 dBFS inside the gate,
#: and the aftersound rate fitted past that point was the slope of the silence:
#: -1.1 dB/s, which read as a reference that rings and a model that does not.
DECAY_RANGE_DB = 60.0

#: How close two decay spans have to be before the rates fitted over them are
#: worth differencing. Loose, because the two sides reach the range bound at
#: their own moments and a decay is not a straight line: what this excludes is
#: the pair where one side sustained for the whole gate and the other was over
#: in a fraction of it, which is a comparison of a late rate against an early one.
DECAY_SPAN_AGREEMENT = 0.6


#: Where the body reading is taken, in seconds from the onset. After the strike,
#: so the hammer's own broadband noise is not counted as the instrument's body,
#: and a full second wide, because what is being measured is a resonance that
#: outlives the string rather than a transient.
BODY_WINDOW_S = (0.2, 1.2)
#: The lowest fundamental this reading means anything for. Below it the band
#: under the note is narrower than an octave and is measuring the note's own
#: skirt rather than anything under it.
BODY_MIN_F0_HZ = 120.0


def body_below_f0_db(x: np.ndarray, sr: int, f0: float) -> float | None:
    """Energy under the note's own fundamental, relative to the fundamental.

    A grand's radiated sound is not only its string. The blow drives the bridge,
    the bridge drives the board, and the board's low modes ring for seconds under
    a note that has none of that frequency in it. How much of the sound that is
    depends steeply on register, because the string's radiated fundamental falls
    away toward the treble while the body's answer to a blow does not: measured
    on three concert grands, this reading rises about forty decibels from C3 to
    C8, and at C8 the energy under the note outweighs the note.

    Taken as a ratio rather than as a level, so it says nothing about how loud
    the render was and cannot be answered by a gain. A model that is a string and
    only a string sits flat across the whole keyboard here, which no amount of
    body LEVEL corrects -- the register dependence is the measurement.
    """
    if f0 < BODY_MIN_F0_HZ:
        return None
    a, b = int(BODY_WINDOW_S[0] * sr), int(BODY_WINDOW_S[1] * sr)
    seg = x[a:b]
    if seg.size < sr // 8:
        return None
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)

    def band(lo: float, hi: float) -> float:
        sel = (freqs >= lo) & (freqs < hi)
        return float(np.sqrt(np.sum(spec[sel] ** 2))) if sel.any() else 0.0

    under, fundamental = band(40.0, f0 * 0.7), band(f0 * 0.8, f0 * 1.25)
    if fundamental <= 0.0 or under <= 0.0:
        return None
    return round(float(20.0 * np.log10(under / fundamental)), 2)


def double_decay_gap(row: dict) -> float | None:
    """How much faster the prompt stage falls than the aftersound, in dB/s.

    A struck string loses energy fast while the strings of its unison and the
    two polarizations of each move together, and far more slowly once they have
    decohered. The size of that mechanism is the GAP between the two rates, and
    neither rate on its own carries it: a voice can sit inside both the held-note
    bound and the aftersound bound while having no double decay whatsoever, by
    putting the same rate in both. Reported as its own dimension for that reason
    -- a mechanism with no dimension can never be the largest error.
    """
    if "decay_early_db_s" not in row or "decay_late_db_s" not in row:
        return None
    return row["decay_early_db_s"] - row["decay_late_db_s"]


#: How many notes a velocity has to contribute before its register profile is
#: worth taking a median of. Below this the median is one or two notes, so the
#: normalization it performs is a comparison of those notes with themselves.
REGISTER_MIN_NOTES = 4


def register_deltas(model: dict[int, dict[int, float]],
                    ref: dict[int, dict[int, float]]) -> list[float]:
    """How the two keyboards' own loudness curves differ, note by note, in dB.

    Every other dimension here is a ratio, a rate, a time or a range, so not one
    of them can see whether a note is the right LOUDNESS for its place on the
    keyboard -- and register balance is most of what voicing a piano is. It was
    invisible for exactly as long as it had no dimension: the model's top octave
    sat twelve decibels under its own mid-keyboard relative to three concert
    grands that agree with each other to within three, and every gated dimension
    read green through it.

    Absolute level cannot be compared -- three instruments recorded at three
    gains say nothing to each other -- so each side is normalized by its OWN
    median across the notes played at that velocity, which removes a constant
    offset and privileges no note. Per velocity, because a grand's register
    profile genuinely changes with how hard it is struck: the top octave falls
    away further at pianissimo than at forte, and pooling the velocities would
    average that out and call it noise.

    @param model Level in dB per note per velocity, one side.
    @param ref   The same for the other side.
    @return One (note, velocity, delta) per pair shared by both sides.
    """
    out: list[tuple[int, int, float]] = []
    velocities = {v for per_vel in model.values() for v in per_vel}
    velocities &= {v for per_vel in ref.values() for v in per_vel}
    for vel in sorted(velocities):
        notes = sorted(n for n in set(model) & set(ref)
                       if vel in model[n] and vel in ref[n])
        if len(notes) < REGISTER_MIN_NOTES:
            continue
        m_mid = float(np.median([model[n][vel] for n in notes]))
        r_mid = float(np.median([ref[n][vel] for n in notes]))
        out.extend((n, vel, (model[n][vel] - m_mid) - (ref[n][vel] - r_mid))
                   for n in notes)
    return out


def usable_decay_end(env_db: np.ndarray, peak_i: int,
                     range_db: float = DECAY_RANGE_DB) -> int:
    """Index one past the last envelope point still within @p range_db of the peak.

    The LAST point inside the range, not the first one outside it: a decaying
    unison beats by ten decibels and more, and dips below any line long before
    it has stopped sounding.

    This bound catches a floor that lies BELOW it and cannot catch one that
    lies above. A capture whose floor is the second kind has to say so: the
    piano's `_late_top` records what that costs its top octave, what was tried,
    and why the reading is flagged rather than corrected.
    """
    live = np.where(env_db[peak_i:] >= env_db[peak_i] - range_db)[0]
    return peak_i + int(live[-1]) + 1 if live.size else peak_i + 1


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

    body = body_below_f0_db(held, sr, float(row.get("f0_hz") or 0.0))
    if body is not None:
        row["body_below_f0_db"] = body

    t_env, env = _rms_envelope(held, sr)
    env_db = np.asarray(_db(env))
    peak_i = int(np.argmax(env))
    row["peak_dbfs"] = round(float(_db(np.abs(mono).max())), 2)
    row["rms_dbfs"] = round(float(_db(np.sqrt(np.mean(held.astype(np.float64) ** 2)))), 2)
    # The loudest the note's body gets: the maximum of the same 10 ms windowed
    # RMS every decay measurement here is read off. It is the one level in this
    # row that is independent of how long the note lasts -- `rms_dbfs` averages
    # over the whole gate, so a voice ringing for eight seconds against a
    # reference that died in two reads as louder without being louder anywhere.
    # It is not hammer-free, and does not need to be: a transient reaches a
    # windowed RMS only in proportion to its share of the window's energy, so a
    # click that puts 14 dB on `peak_dbfs` puts under 3 on this.
    row["held_peak_dbfs"] = round(float(env_db[peak_i]), 2)
    row["attack_ms"] = round(float(t_env[peak_i] * 1000.0), 1)

    tail = slice(peak_i, usable_decay_end(env_db, peak_i))
    # How much of the held note the two rates were fitted over. Both are slopes,
    # so they only compare against a reference fitted over a comparable span --
    # a two-stage decay read to 4 s and one read to 8 is two different questions.
    row["decay_span_s"] = round(float(t_env[tail][-1] - t_env[peak_i]), 3) \
        if t_env[tail].size else 0.0
    if t_env[tail].size > 8:
        row["decay_db_s"] = round(float(np.polyfit(t_env[tail], env_db[tail], 1)[0]), 2)
        row.update(double_decay(env_db[tail], t_env[tail]))
    if row.get("partials_hz"):
        row["partial_decay_db_s"] = partial_decay(held[a:], sr, row["partials_hz"])

    # Damper: note-off to 40 dB below the level it was still holding. Skipped
    # when the string had already stopped — a held C7 is 45 dB down long before
    # the key comes up, and "how fast the damper stopped it" then measures the
    # noise floor and reports it as a very fast damper.
    held_peak_db = float(env_db[peak_i])
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


def profile_body(profile: dict) -> str:
    """The measurement itself — the profile minus its stamp, serialized.

    Re-serializing rather than comparing the dicts is what makes a profile read
    back from disk comparable with one just built: the file's floats, lists and
    nulls come back through the same writer they went out through, so a list
    that was a tuple in memory or a float printed at a different width cannot
    read as a change when nothing changed.
    """
    return json.dumps({k: v for k, v in profile.items() if k != "measured_utc"},
                      indent=1, ensure_ascii=False)


def measurement_stamp(profile: dict, out_path: Path) -> str:
    """When this measurement last *changed*, not when it was last taken.

    A profile is a pure function of the captured WAVs and the analysis code —
    the capture writes audio to disk and every number here is read back from it,
    with nothing measured live — so re-running `measure` over an unchanged
    corpus with unchanged code must produce the identical file. A wall-clock
    stamp written on every run defeats that, and it defeats more than a diff:

    - Nothing can then say mechanically whether an analysis edit moved a
      committed reference, which is the one question that decides whether the
      edit is safe to land.
    - A gate records the stamp of the reference its bounds were read against
      (`reference_measured_utc`), so a re-measure that changed nothing would
      still report every gate as predating its own reference.

    So the stamp is carried forward whenever everything else is unchanged, and
    moves only when a number does.
    """
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    if not out_path.exists():
        return now
    try:
        previous = json.loads(out_path.read_text())
    except (OSError, ValueError):
        return now
    kept = previous.get("measured_utc", "")
    if kept and profile_body(previous) == profile_body(profile):
        return kept
    return now


def measure(cfg: dict, corpus_dir: Path, out_path: Path) -> int:
    manifest_path = corpus_dir / "manifest.json"
    if not manifest_path.exists():
        print(f"no corpus at {manifest_path} — run `capture.py corpus` first", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text())
    preroll_s = manifest["preroll_ms"] / 1000.0
    gate_s = manifest["gate_ms"] / 1000.0
    percussion = is_percussion(cfg)

    # What this profile is allowed to measure: the timbres the capture DECLARES,
    # and nothing else the corpus happens to hold.
    #
    # Two things end up in a manifest that are not the reference. `render-grid`
    # adds the model's own grid on purpose, so that every tool reading a corpus
    # reads the model with no special case. And a re-capture keeps whatever the
    # previous definition rendered, because `corpus --resume` preserves timbres
    # it does not recognise rather than discarding an expensive render — so an
    # instrument re-captured from a different product still has the old one on
    # disk under the old timbre id.
    #
    # Measuring either is silent: `committed_capture` already intersects with
    # the tracked definition, so the profile would declare one timbre and hold
    # the statistics of two. The second case is the dangerous one, since the
    # retired reference is usually retired for being wrong — a bass replaced
    # because its rig was baked in would have gone on contributing to the DI
    # profile that replaced it.
    declared = {t["id"] for t in cfg.get("timbres", [])}

    def sweep(max_band_hz: float | None) -> list[dict]:
        rows: list[dict] = []
        for i, rec in enumerate(manifest["renders"], 1):
            if rec["timbre"] not in declared:
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
    band_edge = shared_band_edge(rows) if percussion else None
    if percussion:
        # Named per reference rather than as one number: when they differ, the
        # capture is held to the narrowest, and a reader who is not told which
        # one set the ceiling cannot tell a wide reference from a wasted one.
        per_timbre = band_edges_by_timbre(rows)
        if len(per_timbre) > 1:
            for tid, edge in per_timbre.items():
                print(f"  {tid}: bandwidth "
                      f"{'no measurable ceiling' if edge is None else f'{edge / 1000.0:.1f} kHz'}",
                      file=sys.stderr)
    if band_edge is not None:
        print(f"\ncapture bandwidth: {band_edge / 1000.0:.1f} kHz — re-measuring so "
              f"the band profile is normalised over what this capture carries",
              file=sys.stderr)
        rows = sweep(band_edge)

    tracked = json.loads(Path(cfg["_path"]).read_text())
    profile = {
        "id": cfg["id"],
        "label": tracked["label"],
        # Filled in below, once there is a body to compare against what is
        # already on disk. Declared here so the key keeps its place in the file.
        "measured_utc": "",
        "capture": {**committed_capture(cfg, tracked, manifest),
                    # Part of the method, not of the instrument: it says what
                    # this recording chain could hear, so a later comparison can
                    # decline to score the model where the reference is blind.
                    "band_edge_hz": band_edge},
        "rows": rows,
        "summary": summarize_percussion(rows) if percussion else summarize(rows),
    }
    before = out_path.read_text() if out_path.exists() else ""
    profile["measured_utc"] = measurement_stamp(profile, out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    written = json.dumps(profile, indent=1, ensure_ascii=False) + "\n"
    out_path.write_text(written)
    print(f"\n{len(rows)} measured notes -> {out_path}", file=sys.stderr)
    print(f"unchanged since {profile['measured_utc']} — the file is byte-identical"
          if written == before else
          f"measurement stamped {profile['measured_utc']}", file=sys.stderr)
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
    # On a kit the channel is not a transport detail: it is what makes the note
    # number select an instrument. Rendering the model's grid on channel 1 would
    # play 47 pitches of whatever program 0 is and write them into the corpus
    # under drum note names.
    channel = PERCUSSION_CHANNEL - 1 if is_percussion(cfg) else 0

    out_dir = corpus_dir / timbre
    out_dir.mkdir(parents=True, exist_ok=True)
    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    rows = []
    total = len(notes) * len(velocities)
    for i, (note, vel) in enumerate(
        ((n, v) for n in notes for v in velocities), start=1
    ):
        tail_s = tail_seconds(cfg, note)
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program,
                        channel=channel, end_pad=tail_s)
        audio = render_model(smf, preroll_s + gate_s + tail_s, sr, rig=rig)
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

    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    swing: dict[str, list[float]] = {}
    for note in notes:
        got = {}
        tail_s = tail_seconds(cfg, note)
        for v in (lo_v, hi_v):
            if (note, v) not in ref:
                break
            smf = write_smf([Note(note, v, preroll_s, gate_s)], program=program,
                            end_pad=tail_s)
            audio = render_model(smf, preroll_s + gate_s + tail_s, cap["sample_rate"],
                                 rig=rig)
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


def percussion_row_deltas(m: dict, r: dict) -> dict[str, float | None]:
    """One hit against another, on the dimensions a kit is judged on.

    Factored out so the model-against-reference comparison and the
    reference-against-reference spread run the identical arithmetic. A spread
    computed any other way would be a different ruler, and the ratio between
    them is the whole point of having one.
    """
    tilt_m, tilt_r = band_tilt_db(m.get("bands_db")), band_tilt_db(r.get("bands_db"))
    return {
        "band_tilt": None if tilt_m is None or tilt_r is None else tilt_m - tilt_r,
        "band_shape": band_shape_error_db(m.get("bands_db"), r.get("bands_db")),
        "band_decay": mean_band_decay_delta(m.get("band_decay_db_s"),
                                            r.get("band_decay_db_s")),
        "attack": m["attack_ms"] - r["attack_ms"],
        "crest": m["crest_db"] - r["crest_db"],
        "centroid_pct": (100.0 * (m["centroid_hz"] / r["centroid_hz"] - 1.0)
                         if r.get("centroid_hz") else None),
        # How loud the hit actually is. Every other column here is normalised —
        # a band profile against its own loudest band, a crest against its own
        # RMS, a decay against its own peak — which is what makes them measure
        # timbre, and which also makes all of them blind to gain. Rewriting
        # eighteen of the kit's output levels moved not one of them by a digit.
        # `vel_range` is a span and cancels an offset by construction, so it is
        # not this either.
        "level": (m["peak_dbfs"] - r["peak_dbfs"]
                  if m.get("peak_dbfs") is not None
                  and r.get("peak_dbfs") is not None else None),
    }


def percussion_reference_spread(profile: dict,
                                dimensions: list[str] | None = None) -> dict[str, float]:
    """How far a kit's references sit from EACH OTHER, dimension by dimension.

    The percussion counterpart of `reference_spread`, which computes the pitched
    dimensions only — stretch, inharmonicity, a partial stack — none of which a
    kit has. Without this a percussion gate carries no `reference_spread` at
    all, so `status.coverage`'s agreement step has nothing to judge against and
    every dimension reads unjudgeable however many kits were captured. Capturing
    a second reference is necessary for a kit to be scored and was never
    sufficient on its own.

    Runs `percussion_row_deltas` over the (note, velocity) keys two references
    share, pooled over every pair of them, taking the median absolute value —
    the same reduction the model's own error goes through.
    """
    rows = profile.get("rows", [])
    timbres = sorted({r["timbre"] for r in rows})
    if len(timbres) < 2:
        return {}
    by_key: dict[str, dict[tuple[int, int], dict]] = {}
    for r in rows:
        by_key.setdefault(r["timbre"], {})[(r["note"], r["velocity"])] = r

    pooled: dict[str, list[float]] = {}
    for i, a in enumerate(timbres):
        for b in timbres[i + 1:]:
            peaks: dict[str, dict[int, dict[int, float]]] = {}
            for key in sorted(set(by_key[a]) & set(by_key[b])):
                x, y = by_key[a][key], by_key[b][key]
                for k, v in percussion_row_deltas(x, y).items():
                    if v is not None and np.isfinite(v):
                        pooled.setdefault(k, []).append(abs(float(v)))
                for side, src in ((a, x), (b, y)):
                    if src.get("peak_dbfs") is not None:
                        peaks.setdefault(side, {}).setdefault(key[0], {})[key[1]] = \
                            src["peak_dbfs"]
            for note, pa in sorted(peaks.get(a, {}).items()):
                pb = peaks.get(b, {}).get(note, {})
                both = sorted(set(pa) & set(pb))
                if len(both) >= 2:
                    pooled.setdefault("vel_range", []).append(abs(
                        (max(pa[v] for v in both) - min(pa[v] for v in both))
                        - (max(pb[v] for v in both) - min(pb[v] for v in both))))
    spread = {k: float(np.median(v)) for k, v in pooled.items() if v}
    return {k: v for k, v in spread.items() if not dimensions or k in dimensions}


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
    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    kit_rows: list[tuple[dict, dict]] = []
    for note, vel in pairs:
        played = mapping.get(note, note)
        # The captured note, not the one the map makes the model play: the window
        # has to be the one the reference row was measured over, and `tail_by_note`
        # is written in the capture's own numbering.
        tail_s = tail_seconds(cfg, note)
        smf = write_smf([Note(played, vel, preroll_s, gate_s)], program=program,
                        channel=PERCUSSION_CHANNEL - 1, end_pad=tail_s)
        audio = render_model(smf, preroll_s + gate_s + tail_s, sr, rig=rig)
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

        row = percussion_row_deltas(m, r)
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
    spread = percussion_reference_spread(profile, list(summary))
    print("\n" + f"{'':46s} {'median':>9} {'|median|':>9} {'spread':>8} "
          f"{'x spread':>9} {'rows':>5}")
    for k, row in summary.items():
        s_k = spread.get(k)
        ratio = (row["abs_median"] / s_k) if s_k and s_k > 0 else None
        print(f"  {DELTA_LABELS.get(k, k):46s} {row['median']:+9.2f} "
              f"{row['abs_median']:9.2f} "
              f"{(f'{s_k:8.2f}' if s_k is not None else '       -')} "
              f"{(f'{ratio:8.1f}x' if ratio is not None else '        -')} {row['n']:5d}")
    print("\n  A kit has no register to average along: every row is a different "
          "instrument,\n  so the signed median says only how the kit leans as a whole and the "
          "absolute\n  one is the column to read. `band shape` is a magnitude already and its "
          "two\n  columns are the same number by construction.")
    if spread:
        print("\n  `spread` is how far the REFERENCE KITS sit from each other on the same "
              "dimension,\n  by the same arithmetic, so `x spread` reads the same in dB, "
              "milliseconds and\n  percent: 1.0x is as close to them as they are to one "
              "another, which is as close\n  as this corpus can define. A gate cannot say this "
              "— its bounds come from what\n  the voice measured the day they were written. "
              "The largest ratio is where the\n  next round belongs.")
    else:
        print("\n  No `spread`: this capture has one reference kit, so no dimension can be "
              "judged\n  against anything but itself. A second kit is what makes the column "
              "exist.")

    print_kit_relations(kit_rows, note_groups(cfg))

    if gate_path:
        return check_gate(summary, Path(gate_path), timbre, profile.get("measured_utc", ""))
    if write_gate:
        return write_gate_file(summary, Path(write_gate), timbre, margin,
                               profile.get("measured_utc", ""), spread)
    return 0


# --------------------------------------------------------------------------
# readiness: what an instrument has, and what the next round needs


def readiness(cfg: dict, *, archive: Path, reference_dir: Path) -> dict:
    """What exists for one instrument, and what the absence of each piece costs.

    Written because the answer was previously assembled by hand out of four
    directories and a `git log`, which is slow, easy to get wrong, and exactly
    the sort of thing that stops a loop between rounds. Every field here is a
    file that either exists or does not; nothing is inferred from a timestamp on
    disk, because a checkout reorders those.
    """
    ident = cfg["id"]
    out: dict = {"id": ident, "program": int(cfg.get("program", 0)),
                 "takes_set": cfg.get("takes") or "", "blocking": [], "next": []}

    profile_path = reference_dir / f"{ident}.json"
    profile = json.loads(profile_path.read_text()) if profile_path.exists() else None
    out["profile_rows"] = len(profile["rows"]) if profile else 0
    out["measured_utc"] = (profile or {}).get("measured_utc", "")
    if profile is None:
        out["blocking"].append(
            "no reference profile: capture.py calibrate/corpus/verify then profile.py measure. "
            "Needs the plugin, so it cannot be done from a plain clone")
        return out

    # Which dimensions the reference can actually adjudicate. A column missing
    # from every row is not a dimension this instrument is short on -- it is one
    # the profile predates, and nothing downstream will ever say so.
    present = {k for r in profile["rows"] for k in r}
    wanted = list(cfg.get("dimensions") or [])
    needs = {"stretch": "cents_vs_et", "decay": "decay_db_s", "aftersound": "decay_late_db_s",
             "doubling": "decay_early_db_s", "body": "body_below_f0_db",
             "attack": "attack_ms", "stereo": "stereo_width", "damper": "damper_release_ms",
             "balance": "partials_db", "centroid_pct": "centroid_hz", "tnr": "tnr_db",
             "vel_range": "peak_dbfs", "register": "held_peak_dbfs"}
    out["dimensions"] = wanted or ["(all measured)"]
    out["unbacked"] = sorted(d for d in wanted if needs.get(d) and needs[d] not in present)
    if out["unbacked"]:
        out["next"].append(
            f"the profile carries no {', '.join(needs[d] for d in out['unbacked'])}: "
            f"re-run profile.py measure over the corpus, or drop those from `dimensions`")

    gate_path = reference_dir / f"{ident}_gate.json"
    gate = json.loads(gate_path.read_text()) if gate_path.exists() else None
    out["gate_bounds"] = len(gate.get("bounds", {})) if gate else 0
    out["gate_timbre"] = (gate or {}).get("timbre", "")
    if gate is None:
        out["next"].append(
            "no gate: nothing holds this voice to anything. profile.py compare "
            "--write-gate reference/<id>_gate.json once the numbers are worth holding")
    else:
        against = gate.get("reference_measured_utc")
        if against is None:
            out["gate_stale"] = "unknown"
            out["next"].append(
                "the gate predates the field recording which reference it was measured "
                "against, so its staleness cannot be checked: re-record it once")
        elif against != out["measured_utc"]:
            out["gate_stale"] = "stale"
            out["next"].append(
                f"the gate was recorded against a reference measured {against} and the "
                f"profile now reads {out['measured_utc']}: its bounds compare against an "
                f"instrument no longer in the file. Re-record before trusting a failure")
        else:
            out["gate_stale"] = "current"
        missing_bounds = [d for d in wanted if d not in gate.get("bounds", {})]
        if missing_bounds:
            out["next"].append(
                f"gated dimensions with no bound recorded: {', '.join(missing_bounds)} — "
                f"listed but unchecked, which reads as passing. Re-record the gate")

    index = archive / "index.json"
    held = set(json.loads(index.read_text()).get(ident, {})) if index.exists() else set()
    out["takes_archived"] = len(held)
    if cfg.get("takes"):
        try:
            wanted_takes = {t.id for t in build_takes(cfg["takes"], out["program"])}
        except KeyError:
            wanted_takes = set()
        out["takes_total"] = len(wanted_takes)
        if wanted_takes - held:
            out["next"].append(
                f"{len(wanted_takes - held)} of {len(wanted_takes)} phrase takes have no "
                f"archived reference, so `profile.py takes` is blind to them: run "
                f"make_audition.py --archive-references once. Needs the plugin")
    else:
        out["takes_total"] = 0
        out["next"].append(
            "the capture names no phrase set, so nothing measures what happens BETWEEN "
            "notes — every coupling in this voice is unmeasured. Add one to the capture")
    return out


def status(cfg: dict, *, archive: Path, reference_dir: Path, every: bool) -> int:
    """Print the readiness of one instrument, or of every shipped capture."""
    configs = []
    if every:
        for path in sorted(CAPTURE_DIR.glob("*.json")):
            if path.name.endswith(".local.json"):
                continue
            configs.append(load_config(path))
    else:
        configs = [cfg]

    rows = [readiness(c, archive=archive, reference_dir=reference_dir) for c in configs]
    print(f"  {'instrument':<14}{'prog':>5}{'rows':>6}{'gate':>7}{'gate vs ref':>13}"
          f"{'takes':>8}{'unbacked dims':>16}")
    for r in rows:
        gate = f"{r['gate_bounds']}" if r["gate_bounds"] else "-"
        stale = r.get("gate_stale", "-") if r["gate_bounds"] else "-"
        took = (f"{r.get('takes_archived', 0)}/{r.get('takes_total', 0)}"
                if r.get("takes_total") else "-")
        print(f"  {r['id']:<14}{r['program']:>5}{r['profile_rows']:>6}{gate:>7}{stale:>13}"
              f"{took:>8}{','.join(r['unbacked']) or '-':>16}")
    for r in rows:
        if not (r["blocking"] or r["next"]):
            continue
        print(f"\n  {r['id']}:")
        for line in r["blocking"] + r["next"]:
            print(f"    - {line}")
    if not any(r["blocking"] or r["next"] for r in rows):
        print("\n  every instrument is ready to measure.")
    # Never non-zero on an incomplete instrument: this reports, it does not gate.
    # A loop reads it to decide where to start, and a status command that exits
    # 1 on "there is work to do" cannot be used for that.
    return 0


# --------------------------------------------------------------------------
# phrase takes: the half of an instrument that only exists between notes

#: Skipped after the last note-off before the tail window opens, so the release
#: transient of the final note is not measured as part of what follows it.
TAKE_TAIL_LEAD_S = 0.08
SUSTAIN_CC = 64
#: Two decades either side of the middle. Wide bands on purpose: this is a
#: balance reading over a whole phrase, and a narrow one would report which
#: pitches the phrase happens to contain.
TAKE_BANDS = ((40.0, 160.0), (160.0, 640.0), (640.0, 2560.0), (2560.0, 10240.0))

#: Where a broadband layer added to fill a decay tail is heard instead as a
#: veil over the note. Deliberately not the tail's own band: the two are
#: different jobs and a single layer serving both is the shape a missing
#: mechanism takes here -- during the note the high end should be the string's
#: partials, after it the board's own field.
SUSTAIN_TONALITY_BAND = (2000.0, 8000.0)

#: Below this, nothing in this harness radiates and whatever a source shows is
#: its own recording chain. Removed before any tail number is taken, because the
#: tail is where it stops being negligible: a sampled instrument's floor is
#: recorded INTO the samples and plays back with them, so it is a fixed level
#: under a decaying signal and its share grows as the note dies. Measured on the
#: piano take references it is worth 1.4 to 2.5 dB at the top of the tail and up
#: to 10 by the end of it, which is most of the slope being fitted through it.
#: The band readings below already start at 40 Hz and were never affected; the
#: broadband ones had no such bound.
TAKE_SUBSONIC_HZ = 40.0
#: Amplitude under which a source is not quiet but absent. These renders finish
#: before their files do -- every piano take reference reaches exact digital
#: silence about a second before the take's nominal end -- and a tail slope
#: fitted across that edge is a measurement of the edge.
TAKE_SILENCE = 1.0e-7

TAKE_LABELS = {
    "sustain_tonality": "2-8 kHz during the note, + = partials over a veil (dB)",
    "s2560": "2560-10240 Hz during the note (dB of the note)",
    "tail": "what is left after the last note (dB under the take's peak)",
    "tail_slope": "how fast that falls (dB/s)",
    "tail_tonality": "tail tonality, + = resolvable modes rather than a smear (dB)",
    "damped": "left 200 ms after the sustain pedal releases (dB of the tail)",
    "floor_rise": "floor before the last onset vs the first (dB)",
    "b40": "tail 40-160 Hz (dB of the tail)",
    "b160": "tail 160-640 Hz",
    "b640": "tail 640-2560 Hz",
    "b2560": "tail 2560-10240 Hz",
}


def take_windows(take) -> dict:
    """Where to measure this phrase, read off the phrase's own schedule.

    Never a fixed time. A take is an arbitrary phrase and the interesting
    windows are defined by its events: the tail is whatever is still sounding
    after the last note-off, and on an instrument with a sustain pedal it ends
    when the pedal lifts rather than when the render does -- measuring past that
    point averages the ring together with the dampers landing on it, which are
    opposite mechanisms at the two ends of one window.
    """
    last_off = max(n.start + n.dur for n in take.notes)
    release = next((t for t, cc, v in sorted(take.cc_events)
                    if cc == SUSTAIN_CC and v < 64 and t > last_off), None)
    tail0 = last_off + TAKE_TAIL_LEAD_S
    tail1 = release if release is not None else last_off + take.tail_s
    windows = {"tail": (tail0, tail1)} if tail1 - tail0 > 0.15 else {}
    if release is not None:
        windows["damped"] = (release + 0.20, release + 0.40)
    # Where the phrase leaves a gap, the level just before the next onset is
    # what the notes before it left behind. On a coupled instrument that floor
    # RISES through a phrase; on a model whose voices do not interact it falls,
    # and no per-note measurement can tell the two apart.
    gaps = []
    onsets = sorted({n.start for n in take.notes})
    for onset in onsets[1:]:
        prev_off = max((n.start + n.dur for n in take.notes if n.start + n.dur <= onset),
                       default=None)
        if prev_off is not None and onset - prev_off > 0.15:
            gaps.append((onset - 0.06, onset - 0.005))
    windows["_gaps"] = gaps
    # The held part of the take's longest note. A layer added to fill the decay
    # tail is audible over the NOTE as well, and measuring only the tail cannot
    # tell an improvement from a veil; this is the other side of that trade, on
    # the same phrase and the same render. Staccato takes have no such window
    # and are simply not measured on it.
    longest = max(take.notes, key=lambda n: n.dur)
    if longest.dur > 0.5:
        windows["sustain"] = (longest.start + 0.15, longest.start + longest.dur)
    return windows


def signal_end_s(x: np.ndarray, sr: int, threshold: float = TAKE_SILENCE) -> float:
    """When this source stops having anything in it at all, in seconds.

    Not a quietness test. A tail sixty decibels under the body is an ordinary
    decay and cutting there ends the window on whichever voice decays fastest;
    what has to be excluded is a render that has RUN OUT -- a sampled instrument
    reaching the end of its sample and being faded to nothing, and then a file
    that carries on in exact silence.
    """
    live = np.flatnonzero(np.abs(np.asarray(x, dtype=np.float64)) > threshold)
    return float(live[-1] + 1) / sr if live.size else 0.0


def usable_tail(sources: list[np.ndarray], sr: int,
                window: tuple[float, float]) -> tuple[float, float] | None:
    """@p window clipped to where every source still has signal.

    Taken across the REFERENCES and applied to the model too, so both sides are
    read over one span. Per-source would be worse than useless here: a synthetic
    render runs to the end of its buffer, so it would keep a window the
    instrument it is being compared against left two seconds earlier.
    """
    ends = [signal_end_s(x, sr) for x in sources]
    hi = min([window[1]] + ends)
    return (window[0], hi) if hi - window[0] > 0.2 else None


def highpass(x: np.ndarray, sr: int, hz: float) -> np.ndarray:
    """Everything above @p hz, through a filter rather than through the spectrum.

    Emphatically NOT a brick wall on the rfft, which is what this was first. A
    rectangular window in frequency is a sinc in time, so zeroing a band spreads
    its truncation error across the whole buffer -- and on a take whose loudest
    moment is seventy decibels over its tail, that error IS the tail. Measured:
    the model's post-damper tail decays cleanly from -72 to -130 dB, and the
    same tail behind a brick wall sits flat at -81 and then RISES toward the end
    as the circular wrap brings the phrase's opening back round. It reads as a
    voice with a non-decaying tail, which is a finding, and it is the filter.

    A windowed sinc instead, convolved LINEARLY (the transform is zero-padded to
    the full convolution length, so nothing wraps). The window is Blackman-
    Harris, whose sidelobes are 92 dB down -- the dynamic range this has to
    survive is the seventy between a take's peak and its tail, and a Blackman's
    58 would not.
    """
    if hz <= 0.0:
        return x
    x = np.asarray(x, dtype=np.float64)
    taps = 8193
    n = np.arange(taps) - (taps - 1) // 2
    fc = hz / sr
    lp = np.sinc(2.0 * fc * n) * 2.0 * fc
    a = (0.35875, 0.48829, 0.14128, 0.01168)
    k = 2.0 * np.pi * np.arange(taps) / (taps - 1)
    lp *= a[0] - a[1] * np.cos(k) + a[2] * np.cos(2 * k) - a[3] * np.cos(3 * k)
    lp /= lp.sum()
    h = -lp
    h[(taps - 1) // 2] += 1.0
    size = len(x) + taps - 1
    y = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(h, size), size)
    return y[(taps - 1) // 2:(taps - 1) // 2 + len(x)]


def measure_take(audio: np.ndarray, sr: int, windows: dict) -> dict:
    """One phrase reduced to the numbers a per-note grid cannot carry.

    Everything under `TAKE_SUBSONIC_HZ` is removed first, on both sides, since
    no instrument here radiates there and a recording that does would otherwise
    be compared against a render's silence.
    """
    x = highpass(to_mono(np.asarray(audio, dtype=np.float64)), sr, TAKE_SUBSONIC_HZ)
    peak = float(np.abs(x).max())
    if peak <= 0.0:
        return {}

    def seg(window):
        a, b = int(window[0] * sr), int(window[1] * sr)
        return x[max(0, a):min(len(x), b)]

    def rms(window):
        s = seg(window)
        return float(np.sqrt(np.mean(s ** 2))) if s.size else 0.0

    out: dict = {}
    # What the note-and-gap windows carry is measured first, because it does not
    # depend on there being a tail. A harpsichord's every reference stops within
    # a tenth of a second of its last damper, so the tail block below drops out
    # for that whole instrument -- and taking the rest of the take with it would
    # lose the two readings about the NOTE that are the reason those takes exist.
    if "sustain" in windows:
        held = seg(windows["sustain"])
        if held.size > sr // 8:
            sf, smag = _spectrum(held, sr)
            sp = np.asarray(smag, dtype=np.float64) ** 2
            lo, hi = SUSTAIN_TONALITY_BAND
            sq = np.maximum(sp[(sf >= lo) & (sf < hi)], 1e-30)
            if sq.size:
                out["sustain_tonality"] = round(
                    float(10 * np.log10(sq.mean() / np.exp(np.log(sq).mean()))), 2)
            stotal = max(float(sp.sum()), 1e-30)
            share = float(sp[(sf >= 2560.0) & (sf < 10240.0)].sum()) / stotal
            out["s2560"] = round(float(10 * np.log10(max(share, 1e-12))), 2)
    gaps = windows.get("_gaps") or []
    if len(gaps) >= 2:
        first, last = _db(rms(gaps[0]) / peak), _db(rms(gaps[-1]) / peak)
        out["floor_rise"] = round(float(last - first), 2)

    tail = windows.get("tail")
    if tail is None or rms(tail) <= 0.0:
        return out
    out["tail"] = round(float(_db(rms(tail) / peak)), 2)
    mid = 0.5 * (tail[0] + tail[1])
    span = 0.5 * (tail[1] - tail[0])
    if span > 0.05:
        out["tail_slope"] = round(
            float((_db(rms((mid, tail[1]))) - _db(rms((tail[0], mid)))) / span), 2)
    if "damped" in windows:
        out["damped"] = round(float(_db(rms(windows["damped"]) / max(rms(tail), 1e-12))), 2)
    body = seg(tail)
    freqs, mag = _spectrum(body, sr)
    p = np.asarray(mag, dtype=np.float64) ** 2
    band = (freqs >= 100.0) & (freqs < 5000.0)
    q = np.maximum(p[band], 1e-30)
    if q.size:
        out["tail_tonality"] = round(float(10 * np.log10(q.mean() / np.exp(np.log(q).mean()))), 2)
    total = max(float(p.sum()), 1e-30)
    for lo, hi in TAKE_BANDS:
        share = float(p[(freqs >= lo) & (freqs < hi)].sum()) / total
        out[f"b{int(lo)}"] = round(float(10 * np.log10(max(share, 1e-12))), 2)
    return out


def archived_take_references(archive: Path, capture_id: str, take_id: str, sr: int) -> dict:
    """The reference renders of one phrase, back at the level they were made at.

    These come from the audition archive rather than from the note corpus,
    because a phrase is not in the note corpus: the corpus is one note at a
    time, which is exactly the condition under which everything measured here
    is inactive.
    """
    index = archive / "index.json"
    if not index.exists():
        return {}
    meta = json.loads(index.read_text()).get(capture_id, {}).get(take_id)
    if not meta:
        return {}
    gain = 10.0 ** (float(meta["gain_db"]) / 20.0)
    out = {}
    for path in sorted((archive / capture_id / take_id).glob("*.wav")):
        audio, file_sr = read_wav(path)
        if file_sr == sr:
            out[path.stem] = np.asarray(audio, dtype=np.float64) / gain
    return out


def archived_take_ids(archive: Path, capture_id: str) -> set[str]:
    """Which phrases the archive holds a reference for, by take id."""
    index = archive / "index.json"
    if not index.exists():
        return set()
    return set(json.loads(index.read_text()).get(capture_id, {}))


def room_match(cfg: dict, *, archive: Path, take_id: str, program: int, verbose: bool) -> int:
    """What libsonare's OWN ambience controls can do about the reference's room.

    The complementary question to everything else here. `compare` and `takes`
    put the model in the reference's space so the timbre is read without the
    building; this asks what the shipped library would have to be told to be in
    that building by itself, which it answers in the only terms it takes — a
    CC91 send and the GS tank's decay, not an RT60. The send is what a program's
    `gm_fallback_sends` weight scales, so the answer converts straight into that
    table.

    Driven off a phrase rather than off a synthesised probe, unlike
    `voicematch.py room-match`: the reference for a phrase is already in the
    archive, so this needs neither the plugin nor the machine it runs on, and
    the room is measured on the same audio the listening page plays.
    """
    from voicematch import DECAY_SCALE_KEY

    sr = int(cfg.get("sample_rate", 48000))
    if cfg.get("dry", True):
        print("the capture is dry: there is no room to reproduce", file=sys.stderr)
        return 0
    wanted = [t for t in build_takes(cfg["takes"], program) if t.id == take_id]
    if not wanted:
        print(f"no take {take_id!r} in the {cfg['takes']!r} set", file=sys.stderr)
        return 2
    take = wanted[0]
    refs = archived_take_references(archive, cfg["id"], take.id, sr)
    if not refs:
        print(f"{take.id}: no archived reference", file=sys.stderr)
        return 2
    spans = [(n.start, n.start + n.dur) for n in take.notes]

    # Every reference, not the first: two recordings of one instrument disagree
    # about their building by more than this search can resolve, and aiming at
    # whichever came first picks a tank the other contradicts.
    targets: list[Room] = []
    for name, audio in refs.items():
        room = measurable_room(np.asarray(audio, dtype=np.float64), sr, spans)
        if room is None:
            print(f"{take.id}: {name} measures no room this phrase can support",
                  file=sys.stderr)
            continue
        targets.append(room)
        print(f"target ({name} on {take.id}): RT60 {room.rt60_s:.2f}s  "
              f"tail level {room.tail_db:+.1f}dB  HF ratio {room.hf_ratio:.2f}")
    if not targets:
        return 2
    if len(targets) > 1:
        print(f"  span: RT60 {min(t.rt60_s for t in targets):.2f}-"
              f"{max(t.rt60_s for t in targets):.2f}s  tail level "
              f"{min(t.tail_db for t in targets):+.1f} to "
              f"{max(t.tail_db for t in targets):+.1f}dB  "
              f"(anything inside it scores zero)")

    # One render per grid point, each in its own interpreter: the override table
    # is read once when the library loads, so a sweep inside one process would
    # measure the first tank setting at every point.
    child = (
        "import sys; sys.path.insert(0, %r)\n"
        "import json\n"
        "from phrases import build_takes\n"
        "from smf import write_smf\n"
        "from room import estimate_room\n"
        "from render_model import render_model\n"
        "prog, cc91, tid, tset = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]\n"
        "t = [x for x in build_takes(tset, prog) if x.id == tid][0]\n"
        "smf = write_smf(t.notes, program=prog, end_pad=t.tail_s, "
        "cc_events=t.cc_events, channel=t.channel, sends=(cc91, 0, 0))\n"
        "a = render_model(smf, t.duration(), 48000)\n"
        "r = estimate_room(a, 48000, [(n.start, n.start + n.dur) for n in t.notes])\n"
        "print(f'{r.rt60_s} {r.tail_db} {r.hf_ratio}')\n"
    ) % (str(Path(__file__).resolve().parent),)

    def measure(cc91: int, decay_scale: float):
        env = dict(os.environ)
        env["SONARE_TUNING_OVERRIDES"] = f"{DECAY_SCALE_KEY}={decay_scale}"
        proc = subprocess.run(
            [sys.executable, "-c", child, str(program), str(cc91), take.id, cfg["takes"]],
            env=env, capture_output=True, text=True, cwd=str(REPO_ROOT))
        if proc.returncode:
            raise SystemExit(proc.stderr[-1200:])
        rt, tail, hf = (float(v) for v in proc.stdout.strip().split()[-3:])
        return Room(rt60_s=rt, hf_ratio=hf, tail_db=tail, predelay_ms=15.0)

    print("searching libsonare's ambience controls (one render per point)...")
    result = match_sends(targets, measure, log=print if verbose else None)
    print(f"\nclosest: CC91 {result['cc91']}, reverb_decay {result['reverb_decay']} "
          f"({DECAY_SCALE_KEY}={result['decay_scale']})")
    print(f"  reached RT60 {result['measured']['rt60_s']:.2f}s  "
          f"tail {result['measured']['tail_db']:+.1f}dB   residual {result['residual']}")
    print(f"  -> MULTIPLY program {program}'s gm_fallback_sends reverb weight "
          f"by {result['send_factor']} (the shipped weight is already in the "
          f"measurement; this is not the weight to write)")
    if result["residual"] > 1.5:
        print("  the tank cannot reach this space: the reference's room is outside "
              "the range libsonare's own reverb spans, so no send weight fixes it")
    return 0


def takes(cfg: dict, *, archive: Path, only: set[str], program: int) -> int:
    """Measure the phrase takes, which is where the couplings live.

    A per-note grid answers what one string does. Everything an instrument does
    because two of its parts are connected -- a pedalled wash accumulating, a
    hi-hat landing on its own ring, a chord's ranks beating -- happens only
    between notes, and a corpus of single notes cannot excite it at all. That is
    why constants on those paths sit unexamined however many fits have run: the
    search never had a measurement of them to minimise.
    """
    # The capture's rate, not a constant of this file: the archive was written
    # at whatever the capture renders at, and a second copy of that number here
    # is a mirror that only stays equal until one of them is changed.
    sr = int(cfg.get("sample_rate", 48000))
    set_name = cfg.get("takes")
    if set_name not in TAKE_SETS:
        named = f"{set_name!r}" if set_name else "no phrase set"
        print(f"the capture names {named}; have {', '.join(TAKE_SETS)}", file=sys.stderr)
        return 2
    selected = [t for t in build_takes(set_name, program) if not only or t.id in only]
    # The capture says whether its reference carries a room; nothing here can
    # tell one from an instrument's own long release, and guessing the wrong way
    # invents a building and convolves it onto every figure below.
    wet = not cfg.get("dry", True)
    # And whether it carries a rig, which is a different question a dryness test
    # cannot answer: a cabinet is a filter rather than a space.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    # One IR per reference for the whole run: the room is a property of the
    # session that recorded it, and the first take able to measure one hands it
    # to every take that cannot.
    room_irs: dict[str, tuple] = {}
    print(f"phrase takes, model on GM program {program}, against the archived references.")
    print("* marks a model value outside the range those references themselves span.\n")
    measured = 0
    for take in selected:
        refs = archived_take_references(archive, cfg["id"], take.id, sr)
        if not refs:
            print(f"  {take.id}: no archived reference — build a page for it first "
                  f"(make_audition.py --archive-references)", file=sys.stderr)
            continue
        windows = take_windows(take)
        spans = [(n.start, n.start + n.dur) for n in take.notes]
        smf = write_smf(take.notes, program=program, end_pad=take.tail_s,
                        cc_events=take.cc_events, channel=take.channel)
        # The tail window ends where the references do, not where the phrase
        # nominally does. Clipped for both sides at once, before either is
        # measured, so the model is never read over a span the instrument it is
        # being compared against had already left.
        nominal = windows.get("tail")
        if nominal:
            clipped = usable_tail([to_mono(np.asarray(a, dtype=np.float64))
                                   for a in refs.values()], sr, nominal)
            windows["tail"] = clipped
        dry_model = render_model(smf, take.duration(), sr, rig=rig)
        # The model renders dry — `write_smf` writes CC91 0 — so on a capture
        # whose reference carries its building, every tail figure below would be
        # a dry signal read against a wet one, and each of them would be outside
        # the references' spread for that reason alone. Measured on the church
        # organ: the tail fell at 78 dB/s against their 15 to 17, and its four
        # band levels missed by 11 to 25 dB. One room per reference, because two
        # references are two buildings and placing the model in one of them
        # would score it against the other's.
        model_rows, unplaced = [], []
        if not wet:
            model_rows.append(measure_take(dry_model, sr, windows))
        for name, audio in (refs.items() if wet else ()):
            if name not in room_irs:
                room = measurable_room(audio, sr, spans)
                if room is not None:
                    room_irs[name] = (room, place_model_in(dry_model, sr, spans, room)[1])
            if name not in room_irs:
                unplaced.append(name)
                continue
            room, ir = room_irs[name]
            model_rows.append(
                measure_take(place_model_in(dry_model, sr, spans, room, ir)[0], sr, windows))
        model = model_rows[0] if model_rows else {}
        ref_rows = [measure_take(a, sr, windows) for a in refs.values()]
        if wet and not model_rows:
            # Every figure below is a tail figure or is read across one, so with
            # no room to put the model in there is nothing here to report rather
            # than a set of numbers that all say the same thing about the
            # building. A phrase of short notes cannot measure a room itself
            # (`Room.gated`); one long-note take in the same set gives every
            # other take its IR, so this is a set without one.
            print(f"  {take.id}: skipped — the reference carries a room and no take in this "
                  f"run measured one to put the model in (run without --only, or add a "
                  f"phrase that holds a note)", file=sys.stderr)
            continue
        if not model or not any(ref_rows):
            # Almost always a phrase with no tail to measure rather than a
            # broken render: a take whose pedal lifts on the last note-off, or
            # whose notes run to the end, leaves no window in which nothing is
            # being played. Named rather than counted, because the same line
            # would otherwise cover a silent model.
            reason = ("no window between the last note and the end of the phrase"
                      if windows.get("tail") is None
                      else "the render carried no measurable tail")
            print(f"  {take.id}: skipped — {reason}", file=sys.stderr)
            continue
        measured += 1
        print(f"  {take.id} — {take.label}")
        clipped = windows.get("tail")
        if nominal and clipped is None:
            print("    (no tail reading: the references stop within a fraction of a second "
                  "of the last note, so there is no span in which they are still sounding)")
        elif nominal and clipped[1] < nominal[1] - 0.05:
            print(f"    (tail read {clipped[0]:.1f}-{clipped[1]:.1f} s: the references run "
                  f"out {nominal[1] - clipped[1]:.1f} s before the phrase does)")
        if wet:
            placed = ", ".join(f"RT60 {room_irs[n][0].rt60_s:.1f}s"
                               for n in refs if n in room_irs)
            missing = (f"; {len(unplaced)} reference(s) left out, no room measured"
                       if unplaced else "")
            print(f"    (model placed in {placed}{missing})")
        for key, label in TAKE_LABELS.items():
            vals = [r[key] for r in ref_rows if key in r]
            got = [r[key] for r in model_rows if key in r]
            if not got or not vals:
                continue
            lo, hi = min(vals), max(vals)
            # Outside their range in EVERY room it was placed in, or it is not
            # the model that is outside — it is which building it was read in.
            flag = "" if any(lo <= g <= hi for g in got) else "*"
            shown = (f"{got[0]:>9.1f}" if max(got) - min(got) < 0.05
                     else f"{min(got):>9.1f}..{max(got):<.1f}")
            print(f"    {label:<58}{lo:>8.1f}..{hi:<8.1f}{shown}{flag}")
        print()
    if not measured:
        print("nothing measured — see the reason printed against each take above",
              file=sys.stderr)
        return 2
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
    # Every dimension the summary gates has a column here, so a bound that moves
    # can be traced to the notes that moved it. The aftersound is the one that
    # most needs it: it is the slowest thing the voice does, so it is the least
    # uniform across the keyboard, and a median hides a register that is wrong in
    # the opposite direction from the rest.
    header = (f"{'note':>5} {'vel':>4} | {'stretch Δc':>10} {'B model':>10} {'B ref':>10} "
              f"{'decay Δdb/s':>12} {'after Δdb/s':>12} {'h2-6 model':>10} {'h2-6 ref':>9} "
              f"{'centroid Δ%':>12} {'TNR Δdb':>9} {'damper Δms':>11}"
              f"{'attack Δms':>11} {'width Δ':>8}")
    print(header)
    print("-" * len(header))

    deltas: dict[str, list[float]] = {}
    damper_censored: list[tuple[int, int]] = []
    span_censored: list[tuple[int, int]] = []
    # Peak level per side, per note, per velocity. The dynamic range is the one
    # dimension no single (note, velocity) row can carry: it is the difference
    # between two of them, so it is accumulated here and reduced after the loop.
    peaks: dict[str, dict[int, dict[int, float]]] = {}
    # The loudest the note's body gets, per side, per note, per velocity, for
    # the register profile. Not the strike peak `vel_range` reads -- that is the
    # hammer -- and not the gate-wide RMS, which counts a long note as a loud
    # one and would report this voice's oversustained treble as a level.
    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    levels: dict[str, dict[int, dict[int, float]]] = {}
    for note, vel in pairs:
        tail_s = tail_seconds(cfg, note)
        smf = write_smf([Note(note, vel, preroll_s, gate_s)],
                        program=program, end_pad=tail_s)
        audio = render_model(smf, preroll_s + gate_s + tail_s, cap["sample_rate"], rig=rig)
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
        # A decay rate is a slope, and two slopes only difference into a
        # comparison when they were fitted over comparable spans. The span is
        # each side's own -- it ends where that render stopped being audible --
        # so a note the model sustains twice as long as the reference produces
        # two numbers about two different parts of a two-stage decay. Shown and
        # left out, the same as a capped damper.
        gap_m, gap_r = double_decay_gap(m), double_decay_gap(r)
        m_span, r_span = m.get("decay_span_s", 0.0), r.get("decay_span_s", 0.0)
        span_mismatch = (min(m_span, r_span) < DECAY_SPAN_AGREEMENT * max(m_span, r_span, 1e-9))
        if span_mismatch:
            span_censored.append((note, vel, m_span, r_span))
        row = {
            "stretch": None if stretch is None else stretch + a4_off,
            "decay": None if span_mismatch else d("decay_db_s"),
            # Positive means the model is the cleaner of the two, which is the
            # direction this almost always fails in: the mechanism noise a real
            # action makes is the part a physical model most often has no
            # mechanism for at all.
            "tnr": d("tnr_db"),
            "damper": None if damper_capped else d("damper_release_ms"),
            # How long the note takes to reach its loudest point. On a piano
            # this is not the strike -- the hammer is over in a couple of
            # milliseconds -- it is the bloom the soundboard adds after it, and
            # a model whose envelope peaks on the strike and falls from there
            # reads as a thump rather than as a note that sinks in.
            "attack": d("attack_ms"),
            # The SECOND decay rate. A piano string loses energy fast while the
            # strings of its unison move together and far more slowly once they
            # have drifted apart and are trading it through the bridge rather
            # than radiating it. That second rate is what a pedalled note is
            # still doing seconds later -- it is the afterglow -- and `decay`
            # above cannot see it: one straight line through both regimes lands
            # between them, so a voice with no aftersound at all can hold that
            # line. Measured against three concert grands, this voice sat inside
            # the decay bound while its late rate at C5 was three times theirs.
            "aftersound": None if span_mismatch else d("decay_late_db_s"),
            "doubling": None if span_mismatch or gap_m is None or gap_r is None
            else gap_m - gap_r,
            "body": d("body_below_f0_db"),
            # The two radiation paths of a real board arrive decorrelated. A
            # model that folds one signal into both legs scores exactly 0.0
            # here and is inaudibly mono however wide the reverb around it is,
            # which no mono-summed measurement in this file can see.
            "stereo": d("stereo_width"),
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
            if src.get("held_peak_dbfs") is not None and np.isfinite(src["held_peak_dbfs"]):
                levels.setdefault(side, {}).setdefault(note, {})[vel] = src["held_peak_dbfs"]

        def fmt(v, spec):
            return format(v, spec) if v is not None and np.isfinite(v) else "n/a".rjust(len(format(0, spec)))

        damper_col = fmt(d("damper_release_ms"), '+10.1f') + ("*" if damper_capped else " ")
        # A censored pair still prints its two rates, marked, because seeing
        # which side ran out first is the whole content of the finding.
        decay_col = (fmt(d("decay_db_s"), '+11.2f') + ("~" if span_mismatch else " "))
        after_col = (fmt(d("decay_late_db_s"), '+11.2f') + ("~" if span_mismatch else " "))
        print(f"{note:5d} {vel:4d} | {fmt(row['stretch'], '+10.1f')} "
              f"{m.get('inharmonicity_b', float('nan')):10.3e} {r.get('inharmonicity_b', float('nan')):10.3e} "
              f"{decay_col} {after_col} "
              f"{fmt(bal_m, '+10.1f')} {fmt(bal_r, '+9.1f')} "
              f"{fmt(centroid_pct, '+12.1f')} {fmt(row['tnr'], '+9.1f')} {damper_col}"
              f"{fmt(row['attack'], '+11.1f')} {fmt(row['stereo'], '+8.3f')}")

    if damper_censored:
        # Each censored row was rendered over its own note's tail, so the
        # sentence names the span rather than one number it no longer has.
        tails = sorted({tail_seconds(cfg, n) for n, _ in damper_censored})
        span = (f"{tails[0]:.0f} s" if len(tails) == 1
                else f"{tails[0]:.0f}-{tails[-1]:.0f} s")
        print(f"\n* {len(damper_censored)} of {len(pairs)} rows never fell 40 dB inside the "
              f"{span} tail on one side or the other; shown, not counted:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v in damper_censored))
    if span_censored:
        # WHICH side outlasted the other, because that is the finding and the
        # censor is what hides it: a row is dropped exactly when the two decays
        # are too different to difference, which is the same thing as saying the
        # difference is large. Left unnamed it reads as missing data.
        longer = sum(1 for _n, _v, ms, rs in span_censored if ms > rs)
        side = ("the model outlasted the reference on all of them" if longer == len(span_censored)
                else "the reference outlasted the model on all of them" if longer == 0
                else f"the model outlasted the reference on {longer} of them")
        print(f"\n~ {len(span_censored)} of {len(pairs)} rows had one side stay audible "
              f"well past the other, so their decay rates cover different parts of the "
              f"decay; shown, not counted, and {side}:")
        print("  " + ", ".join(f"n{n}v{v} ({ms:.1f}s vs {rs:.1f}s)"
                               for n, v, ms, rs in span_censored))
    for note, model_peaks in sorted(peaks.get("m", {}).items()):
        ref_peaks = peaks.get("r", {}).get(note, {})
        shared = sorted(set(model_peaks) & set(ref_peaks))
        if len(shared) < 2:
            continue
        span = ([model_peaks[v] for v in shared], [ref_peaks[v] for v in shared])
        deltas.setdefault("vel_range", []).append(
            (max(span[0]) - min(span[0])) - (max(span[1]) - min(span[1]))
        )
    register = register_deltas(levels.get("m", {}), levels.get("r", {}))
    if register:
        deltas["register"] = [d for _, _, d in register]
        print_register_profile(register, register_spread_by_note(profile))
    summary = select_dimensions(summarize_deltas(deltas), cfg.get("dimensions") or [])
    spread = reference_spread(profile, list(summary))
    print("\n" + f"{'':46s} {'median':>9} {'|median|':>9} {'spread':>8} {'x spread':>9} {'rows':>5}")
    for k, row in summary.items():
        s_k = spread.get(k)
        ratio = (row["abs_median"] / s_k) if s_k and s_k > 0 else None
        print(f"  {DELTA_LABELS.get(k, k):46s} {row['median']:+9.2f} "
              f"{row['abs_median']:9.2f} "
              f"{(f'{s_k:8.2f}' if s_k is not None else '       -')} "
              f"{(f'{ratio:8.1f}x' if ratio is not None else '        -')} {row['n']:5d}")
    print("\n  The signed median is what the voice is doing on average and the absolute one "
          "is\n  how far any given note is from the reference. They part company exactly where "
          "a\n  summary is least trustworthy: errors of opposite sign in different registers "
          "cancel\n  in the first column and do not in the second.")
    print("\n  `spread` is how far the REFERENCES sit from each other on the same dimension, "
          "by\n  the same arithmetic, so `x spread` reads the same in cents, dB, milliseconds "
          "and\n  percent: 1.0x is as close to them as they are to one another, which is as "
          "close\n  as this corpus can define. A gate cannot say this — its bounds come from "
          "what\n  the voice measured the day they were written, so green means "
          "\"no worse than\n  then\", never \"finished\". The largest ratio is where the "
          "next round belongs.")

    if gate_path:
        return check_gate(summary, Path(gate_path), timbre, profile.get("measured_utc", ""))
    if write_gate:
        return write_gate_file(summary, Path(write_gate), timbre, margin,
                               profile.get("measured_utc", ""), spread)
    return 0


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
                "attack": "time to the envelope peak (ms)",
                "crest": "peak over RMS of the hit (dB)",
                "level": "how loud the hit is vs the reference (dBFS)"}


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
    # A dimension this run measured and this gate has no bound for. The loop
    # above walks the gate, so such a dimension is held to nothing and says so
    # nowhere -- which is the same silence a capture naming an unmeasured
    # dimension produces, and reads the same way: as a column that was fine.
    ungated = [k for k in summary if k not in bounds]
    if ungated:
        print(f"  {', '.join(DELTA_LABELS.get(k, k) for k in ungated)}: measured, no bound "
              f"recorded — nothing here holds it. Re-record with --write-gate.")
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
               "stereo": 0.27, "attack": 40.0, "aftersound": 1.81}
    floors = {**guesses, **{k: v for k, v in (spread or {}).items() if v > 0.0}}
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
        tail_s = tail_seconds(cfg, note)
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
                            ("dynamics", "diff the pp->ff swing rather than one velocity at a time"),
                            ("takes", "measure the phrase takes, which is where the couplings "
                                      "between notes live and where a note grid is blind"),
                            ("status", "what an instrument has, what it is missing, and what "
                                       "the next round needs — the entry point of a loop"),
                            ("room-match", "what libsonare's own CC91 send and GS tank would "
                                           "have to be to sit in the reference's room")):
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
    status_group = sub.choices["status"]
    status_group.add_argument("--all", action="store_true", dest="every",
                              help="every shipped capture rather than one")
    status_group.add_argument("--archive", default="", dest="status_archive",
                              help="phrase-take reference archive (default: the audition one)")
    room_group = sub.choices["room-match"]
    room_group.add_argument("--take", default="", help="phrase to measure the room on "
                            "(default: the first the archive holds a reference for)")
    room_group.add_argument("--archive", default="", help="reference archive (default: the "
                            "audition one)")
    room_group.add_argument("--verbose", action="store_true", help="print every search point")
    takes_group = sub.choices["takes"]
    takes_group.add_argument("--only", default="", help="comma-separated take ids")
    takes_group.add_argument(
        "--archive", default="",
        help="reference renders of the phrases (default: the audition archive). These are "
             "not in the note corpus, which is one note at a time and so cannot hold a phrase")
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
    if args.cmd == "status":
        from make_audition import DEFAULT_REFERENCE_ARCHIVE
        return status(cfg,
                      archive=Path(args.status_archive).expanduser().resolve()
                      if args.status_archive else DEFAULT_REFERENCE_ARCHIVE,
                      reference_dir=REFERENCE_DIR, every=bool(args.every))
    if args.cmd == "room-match":
        from make_audition import DEFAULT_REFERENCE_ARCHIVE
        archive = (Path(args.archive).expanduser().resolve() if args.archive
                   else DEFAULT_REFERENCE_ARCHIVE)
        program = int(cfg.get("program", 0))
        take_id = args.take
        if not take_id:
            held = archived_take_ids(archive, cfg["id"])
            ordered = [t.id for t in build_takes(cfg.get("takes") or "", program)
                       ] if cfg.get("takes") in TAKE_SETS else []
            take_id = next((t for t in ordered if t in held), "")
            if not take_id:
                print(f"no archived phrase reference for {cfg['id']}", file=sys.stderr)
                return 2
        return room_match(cfg, archive=archive, take_id=take_id, program=program,
                          verbose=bool(args.verbose))
    if args.cmd == "takes":
        from make_audition import DEFAULT_REFERENCE_ARCHIVE
        return takes(cfg,
                     archive=Path(args.archive).expanduser().resolve() if args.archive
                     else DEFAULT_REFERENCE_ARCHIVE,
                     only={t.strip() for t in args.only.split(",") if t.strip()},
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
