"""Does the loss move when the sound changes? A synthetic positive control.

Nobody has measured whether the objective in `loss.py` responds to the
differences a listener names first. A listening ranking is not available, so
this substitutes a weaker but mechanical question: perturb a REFERENCE render
by a known amount and ask whether the loss charges for it.

Two design rules make the answer readable.

**The perturbation is applied to audio, never to a knob.** A knob answers a
different question — whether the knob reaches the defect — and a null through
one cannot be attributed to the loss.

**The yardstick is the reference's own spread.** A capture with two or more
timbres supplies `loss(A, B)`: how far two recordings of the same instrument
sit from each other. `LossWeights.calibrate` is given that pair, so it scores
exactly 1.0, and the perturbation's score is then read directly as a multiple
of it. Below 1.0 the perturbation is buried in the spread and the loss is not
seeing it. The scaling is the harness's own, not arithmetic invented here.

Every perturbation carries an amplitude, and the run measures the whole family
at amplitude zero as well. A zero-amplitude perturbation that does not score
exactly 0.0 is plumbing, not signal.

Read-only: nothing under `capture/` or `reference/` is written or modified, no
model is built, and the exit code is always 0.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from autofit_resolve import reference_band_edge
from corpus import corpus_oracle, corpus_pattern, load_corpus
from loss import (
    HARM_REACH,
    LOSS_TERMS,
    LossWeights,
    mss_distance,
    probe_rows,
    score_terms,
)
from loss_dimensions import _brightness
from loss_weights import TERM_UNITS
from metrics import midi_to_hz, normalize_rms, note_onset, partial_hz, to_mono
from toneclass import default_weights, tone_class

SR = 48000


@dataclass(frozen=True)
class Subject:
    """One capture, the slice of its grid to probe, and what it is."""

    capture: str
    notes: tuple[int, ...]
    velocities: tuple[int, ...]
    program: int
    percussive: bool = False


# The population. Every entry is a capture with two or more reference timbres,
# because only those supply a yardstick; the note and velocity slices keep one
# probe inside a few minutes of audio while holding a velocity axis (which
# `dyn` needs) and, for the kit, whole families (which `kit` needs).
SUBJECTS = (
    Subject("piano", (36, 48, 60), (24, 56, 88, 120), 0),
    Subject("harpsichord", (36, 48, 60), (24, 56, 88, 120), 6),
    Subject("drawbar_organ", (48, 60, 72), (48, 127), 16),
    Subject("electric_guitar_di", (48, 56, 64), (32, 56, 104), 27),
    Subject(
        "drums", (36, 38, 41, 43, 45, 47, 48, 50, 42, 44, 46), (64, 100, 127), 0, percussive=True
    ),
)


# --------------------------------------------------------------------------- #
# Slot arithmetic
# --------------------------------------------------------------------------- #
def _slot_bounds(pattern, n: int) -> list[tuple[int, int]]:
    """Sample range each analysis note owns, up to the next onset."""
    starts = [round(note.start * SR) for note in pattern.notes]
    ends = [*starts[1:], n]
    return list(zip(starts, [min(e, n) for e in ends]))


def _filter_slot(seg: np.ndarray, gain: np.ndarray, freqs: np.ndarray) -> np.ndarray:
    """Apply a time-invariant spectral gain to one slot.

    One transform over the whole slot rather than an STFT: the gain is constant
    in time, so this is a linear filter and the long transform is both exact and
    cheaper. Zero-padded to twice the slot so the implied circular convolution
    cannot wrap the slot's tail onto its attack.
    """
    n = len(seg)
    spec = np.fft.rfft(seg, n=2 * n)
    del freqs  # the caller built `gain` on the padded grid
    return np.fft.irfft(spec * gain, n=2 * n)[:n]


def _pad_freqs(n: int) -> np.ndarray:
    return np.fft.rfftfreq(2 * n, 1.0 / SR)


# --------------------------------------------------------------------------- #
# The perturbations
# --------------------------------------------------------------------------- #
# Each takes the unnormalised mono render, the pattern, the rows measured from
# the UNPERTURBED render (which is where every frequency and every trend it
# needs comes from), and a scalar amplitude. Amplitude 0.0 must be the identity.


def p_high_partials(raw, pattern, rows, amp: float) -> tuple[np.ndarray, str]:
    """Raise partials 15-30 by `amp` dB, leaving 1-14 alone.

    Each partial is placed at `partial_hz(f0, k, B)` using the f0 and the
    stiffness the harness itself measured on this render, so the boost lands
    where the ladder and the tone mask look rather than on an integer multiple
    they would already have walked off. The region is +/-0.6 % of the partial's
    own frequency, raised-cosine at the edges; above Nyquist it is skipped and
    the skip is counted.
    """
    out = np.array(raw, dtype=np.float64)
    placed = skipped = 0
    for (a, b), row in zip(_slot_bounds(pattern, len(raw)), rows):
        f0 = row.get("f0_hz") or (midi_to_hz(row["note"]) if row.get("note") else 0.0)
        if f0 <= 0.0:
            continue
        stiff = row.get("inharmonicity_b") or 0.0
        seg = out[a:b]
        freqs = _pad_freqs(len(seg))
        gain = np.ones_like(freqs)
        for k in range(15, 31):
            hz = partial_hz(f0, k, stiff)
            if hz >= SR / 2 - 200.0:
                skipped += 1
                continue
            half = hz * 0.006
            d = np.abs(freqs - hz) / half
            shape = np.where(d < 1.0, 0.5 * (1.0 + np.cos(np.pi * d)), 0.0)
            gain += (10.0 ** (amp / 20.0) - 1.0) * shape
            placed += 1
        out[a:b] = _filter_slot(seg, gain, freqs)
    return out, f"partials 15-30 +{amp:g} dB: {placed} placed, {skipped} over Nyquist"


def p_slow_attack(raw, pattern, rows, amp: float) -> tuple[np.ndarray, str]:
    """Replace each note's rise with a raised-cosine ramp `amp` ms long.

    From the onset the harness detects, not the scheduled one. It does not need
    to know the original attack: a 0->1 ramp over 8 ms leaves a note that
    reaches full level in 8 ms whatever it did before, so a 2 ms rise becomes an
    8 ms one. `attack_fine_ms` on the rows is what says whether it landed --
    the 0.5 ms grid `_attack_delta_ms` prefers, not the 5 ms one `attack_ms`
    quantises to, on which a rise moving from 14.6 ms to 15 reads unchanged.
    """
    out = np.array(raw, dtype=np.float64)
    if amp <= 0.0:
        return out, "attack ramp 0 ms (identity)"
    mono = normalize_rms(raw)
    ramped = 0
    for (a, b), note in zip(_slot_bounds(pattern, len(raw)), pattern.notes):
        onset = note_onset(mono, SR, note, b / SR)
        start = round(onset * SR)
        n = int(SR * amp / 1000.0)
        if start + n >= b:
            continue
        t = np.arange(n) / n
        out[start : start + n] *= 0.5 * (1.0 - np.cos(np.pi * t))
        out[a:start] = 0.0
        ramped += 1
    return out, f"attack ramp {amp:g} ms on {ramped} notes"


def p_tremolo(raw, pattern, rows, amp: float) -> tuple[np.ndarray, str]:
    """Amplitude-modulate each note at 3 Hz to a depth of `amp` dB peak-to-peak.

    Stands in for "remove the unison beat", which cannot be done to a recording
    — separating two strings that were captured as one mix is not an operation
    this harness has. What it does instead is exact rather than approximate: it
    ADDS movement of the kind `mod` reads (`trem_db`, `trem_rate_hz`, and the
    beat pair, which are measured from the same amplitude modulation), so the
    term under test is the same one, and `trem_db` on the rows says it landed.
    """
    out = np.array(raw, dtype=np.float64)
    if amp <= 0.0:
        return out, "tremolo depth 0 dB (identity)"
    depth = (10.0 ** (amp / 20.0) - 1.0) / (10.0 ** (amp / 20.0) + 1.0)
    for a, b in _slot_bounds(pattern, len(raw)):
        t = np.arange(b - a) / SR
        out[a:b] *= 1.0 + depth * np.sin(2.0 * np.pi * TREMOLO_HZ * t)
    return out, f"{TREMOLO_HZ:g} Hz tremolo, {amp:g} dB peak-to-peak"


TREMOLO_HZ = 3.0


def p_invert_dynamics(raw, pattern, rows, amp: float) -> tuple[np.ndarray, str]:
    """Negate how brightness tracks velocity, leaving each pitch's mean intact.

    `dyn` fits `_brightness` against velocity per pitch. `_brightness` on a
    pitched row is the mean of h4-h10 against h1, so the trend is inverted by
    giving slot i a gain of `-2 * s * (v_i - mean v)` dB on exactly those
    partials, where `s` is the slope this render itself shows. That leaves the
    group's mean brightness where it was and takes its slope to `-s`.

    `amp` scales the inversion: 1.0 inverts, 0.0 is the identity, 0.5 flattens.
    On a percussion row `_brightness` reads the top third of the band profile
    instead, so the gain is a shelf over that region.

    The gain is capped at `INVERT_CAP_DB`, and the cap is load-bearing rather
    than defensive. `_brightness` drops a ladder bin at the -120 dB sentinel
    instead of averaging it in, so a grid whose upper partials fall under the
    floor at low velocity and clear it at high velocity reports a slope of tens
    of dB per velocity span; uncapped, negating it asks for a +60 dB boost and
    the perturbation stops being the one named. What is capped is reported, so
    a partial inversion reads as one.
    """
    out = np.array(raw, dtype=np.float64)
    if amp <= 0.0:
        return out, "dynamics inversion 0x (identity)"
    bounds = _slot_bounds(pattern, len(raw))
    groups: dict[int, list[int]] = {}
    for i, row in enumerate(rows):
        if row.get("note") is not None and row.get("velocity") is not None:
            groups.setdefault(row["note"], []).append(i)
    moved = capped = 0
    for idx in groups.values():
        pairs = [(float(rows[i]["velocity"]), _brightness(rows[i])) for i in idx]
        pairs = [(v, br) for v, br in pairs if br is not None]
        if len(pairs) < 2 or max(v for v, _ in pairs) - min(v for v, _ in pairs) < 16.0:
            continue
        vel = np.asarray([v for v, _ in pairs])
        slope = float(np.polyfit(vel, [br for _, br in pairs], 1)[0])
        centre = float(vel.mean())
        for i in idx:
            row = rows[i]
            if row.get("velocity") is None:
                continue
            want = -2.0 * amp * slope * (float(row["velocity"]) - centre)
            db = max(-INVERT_CAP_DB, min(INVERT_CAP_DB, want))
            capped += abs(want) > INVERT_CAP_DB
            a, b = bounds[i]
            seg = out[a:b]
            freqs = _pad_freqs(len(seg))
            out[a:b] = _filter_slot(seg, _brightness_gain(row, freqs, db), freqs)
            moved += 1
    return out, (
        f"dynamics slope negated on {moved} slots ({len(groups)} pitches), "
        f"{capped} clipped at +/-{INVERT_CAP_DB:g} dB"
    )


#: How far the inversion may push one slot. Level with `DYN_DELTA_CAP_DB`, which
#: is what `dyn` itself charges at most, so the perturbation cannot ask for more
#: than the term it is testing could ever price.
INVERT_CAP_DB = 12.0


def _brightness_gain(row: dict, freqs: np.ndarray, db: float) -> np.ndarray:
    """A gain of `db` over exactly what `_brightness` reads on this row."""
    gain = np.ones_like(freqs)
    factor = 10.0 ** (db / 20.0)
    if row.get("harmonics_db") is not None:
        f0 = row.get("f0_hz") or (midi_to_hz(row["note"]) if row.get("note") else 0.0)
        if f0 <= 0.0:
            return gain
        stiff = row.get("inharmonicity_b") or 0.0
        for k in range(4, 11):
            hz = partial_hz(f0, k, stiff)
            if hz >= SR / 2 - 200.0:
                continue
            half = hz * 0.02
            d = np.abs(freqs - hz) / half
            gain += (factor - 1.0) * np.where(d < 1.0, 0.5 * (1.0 + np.cos(np.pi * d)), 0.0)
        return gain
    # The percussion branch of `_brightness`: the top third of a 1/3-octave
    # profile that starts at 50 Hz and runs 25 bands, so about 2 kHz up.
    edge = 2000.0
    ramp = np.clip((np.log2(np.maximum(freqs, 1.0) / edge) + 1.0), 0.0, 1.0)
    return 1.0 + (factor - 1.0) * ramp


def p_hiss(raw, pattern, rows, amp: float) -> tuple[np.ndarray, str]:
    """Add band-limited white noise `amp` dB under each slot's own RMS.

    300 Hz to 16 kHz, which is between the partials rather than under the
    fundamental. Deterministic: one seeded generator per run, so two runs of
    this instrument compare the same signal.
    """
    out = np.array(raw, dtype=np.float64)
    if not math.isfinite(amp):
        return out, "hiss off (identity)"
    rng = np.random.default_rng(20260920)
    for a, b in _slot_bounds(pattern, len(raw)):
        seg = out[a:b]
        rms = float(np.sqrt(np.mean(seg**2)))
        if rms <= 0.0:
            continue
        noise = rng.standard_normal(len(seg))
        freqs = _pad_freqs(len(seg))
        band = ((freqs >= 300.0) & (freqs <= 16000.0)).astype(np.float64)
        noise = _filter_slot(noise, band, freqs)
        nrms = float(np.sqrt(np.mean(noise**2)))
        if nrms <= 0.0:
            continue
        out[a:b] = seg + noise * (rms * 10.0 ** (amp / 20.0) / nrms)
    return out, f"white noise 300 Hz-16 kHz at {amp:g} dB re slot RMS"


def p_gain(raw, pattern, rows, amp: float) -> tuple[np.ndarray, str]:
    """Scale the whole render by `amp` dB."""
    return np.asarray(raw, dtype=np.float64) * 10.0 ** (amp / 20.0), f"output {amp:+g} dB"


@dataclass(frozen=True)
class Perturbation:
    key: str
    label: str
    fn: object
    amp: float
    zero: float
    #: Row field whose change is the proof the perturbation landed. Empty where
    #: the evidence is a loss term instead.
    witness: str = ""
    pitched_only: bool = False


PERTURBATIONS = (
    Perturbation(
        "p1_high_partials", "partials 15-30 +12 dB", p_high_partials, 12.0, 0.0, pitched_only=True
    ),
    Perturbation(
        "p2_slow_attack",
        "attack -> 8 ms",
        p_slow_attack,
        8.0,
        0.0,
        witness="attack_fine_ms,attack_ms",
    ),
    # Two lengths, because 8 ms turned out to be shorter than what these
    # references already do: the captured grands and harpsichords measure 14-15
    # ms to peak and the drawbar organ 62, so an 8 ms ramp leaves `attack_ms`
    # exactly where it was and the row above measures nothing on them. 30 ms
    # clears every melodic capture here and is what actually asks the question.
    Perturbation(
        "p2b_slow_attack_30",
        "attack -> 30 ms",
        p_slow_attack,
        30.0,
        0.0,
        witness="attack_fine_ms,attack_ms",
    ),
    Perturbation(
        "p3_tremolo", "3 Hz tremolo, 3 dB", p_tremolo, 3.0, 0.0, witness="trem_db,crest_db"
    ),
    Perturbation("p4_invert_dyn", "velocity response inverted", p_invert_dynamics, 1.0, 0.0),
    Perturbation(
        "p5_hiss", "hiss at -30 dB", p_hiss, -30.0, -math.inf, witness="tnr_db,centroid_hz"
    ),
    Perturbation("p6_gain", "output -6 dB", p_gain, -6.0, 0.0),
)


# --------------------------------------------------------------------------- #
# Running one subject
# --------------------------------------------------------------------------- #
def _measure(raw: np.ndarray, pattern, threads: int, band_edge: float | None):
    mono = normalize_rms(raw)
    rows = probe_rows(mono, pattern, SR, raw=raw, max_band_hz=band_edge, threads=threads)
    return rows, mono


def _terms(
    model_rows, oracle_rows, model_mono, oracle_mono, subject: Subject, groups
) -> dict[str, float] | None:
    return score_terms(
        model_rows,
        oracle_rows,
        n_harm=HARM_REACH,
        mss=mss_distance(model_mono, oracle_mono),
        percussive=subject.percussive,
        groups=groups,
    )


def _witness(rows_a, rows_p, fields: str) -> str:
    """Mean of one row field before and after, so a null can be attributed.

    Several field names, first present wins: the two metric sets name the same
    evidence differently and a percussion row has no `trem_db` at all, so a
    single name reads "not measured" on half the population.
    """
    for field in fields.split(","):
        pairs = [(a.get(field), p.get(field)) for a, p in zip(rows_a, rows_p)]
        pairs = [
            (x, y) for x, y in pairs if isinstance(x, (int, float)) and isinstance(y, (int, float))
        ]
        if pairs:
            before = sum(x for x, _ in pairs) / len(pairs)
            after = sum(y for _, y in pairs) / len(pairs)
            return f"{field}: {before:.2f} -> {after:.2f} (mean of {len(pairs)})"
    return f"{fields}: not measured on either side"


def _delta_db(raw_a: np.ndarray, raw_p: np.ndarray) -> float:
    """How big the perturbation is, as the difference signal against the original.

    The one witness every perturbation gets. A loss that does not move is only
    a finding once this says the audio did: a null here and a null in the loss
    is a broken perturbation, and the two read identically in a results table.
    """
    n = min(len(raw_a), len(raw_p))
    base = float(np.sqrt(np.mean(np.asarray(raw_a[:n], dtype=np.float64) ** 2)))
    diff = float(
        np.sqrt(
            np.mean(
                (np.asarray(raw_p[:n], dtype=np.float64) - np.asarray(raw_a[:n], dtype=np.float64))
                ** 2
            )
        )
    )
    if base <= 0.0:
        return float("-inf")
    return 20.0 * math.log10(max(diff, 1e-18) / base)


def _partial_band_db(raw: np.ndarray, pattern, rows, lo_k: float, hi_k: float) -> float | None:
    """Mean energy between partials `lo_k` and `hi_k`, in dB re the slot's total.

    The witness `p1` needs: `attack_ms` and friends cannot see a boost that
    lands above the tenth harmonic, so the proof that it landed has to be taken
    from the spectrum directly.
    """
    vals = []
    for (a, b), row in zip(_slot_bounds(pattern, len(raw)), rows):
        f0 = row.get("f0_hz") or (midi_to_hz(row["note"]) if row.get("note") else 0.0)
        if f0 <= 0.0:
            continue
        seg = np.asarray(raw[a:b], dtype=np.float64)
        if len(seg) < 4096:
            continue
        freqs = np.fft.rfftfreq(len(seg), 1.0 / SR)
        power = np.abs(np.fft.rfft(seg)) ** 2
        mask = (freqs >= lo_k * f0) & (freqs <= min(hi_k * f0, SR / 2))
        total = float(power.sum())
        if total <= 0.0 or not mask.any():
            continue
        vals.append(10.0 * math.log10(max(float(power[mask].sum()), 1e-30) / total))
    return sum(vals) / len(vals) if vals else None


def _brightness_slopes(rows) -> dict[int, float]:
    """`dyn`'s own quantity: brightness against velocity, per pitch."""
    groups: dict[int, list[tuple[float, float]]] = {}
    for row in rows:
        br = _brightness(row)
        if row.get("note") is None or row.get("velocity") is None or br is None:
            continue
        groups.setdefault(row["note"], []).append((float(row["velocity"]), br))
    out = {}
    for note, pairs in groups.items():
        vel = [v for v, _ in pairs]
        if len(pairs) < 2 or max(vel) - min(vel) < 16.0:
            continue
        out[note] = float(np.polyfit(vel, [b for _, b in pairs], 1)[0]) * 64.0
    return out


def run_subject(subject: Subject, root: Path, threads: int) -> dict | None:
    manifest = root / "capture" / subject.capture / "manifest.json"
    if not manifest.exists():
        print(f"  {subject.capture}: no corpus at {manifest} — skipped", file=sys.stderr)
        return None
    base = load_corpus(manifest)
    others = [t for t in _reference_timbres(manifest) if t != base.timbre]
    if not others:
        print(
            f"  {subject.capture}: one reference timbre, so no yardstick — skipped", file=sys.stderr
        )
        return None

    pattern = corpus_pattern(base, notes=subject.notes, velocities=subject.velocities)
    weights = default_weights(subject.program, percussive=subject.percussive)
    groups = base.groups if subject.percussive else None

    raw_a = to_mono(corpus_oracle(base, pattern, SR))
    rows_a, mono_a = _measure(raw_a, pattern, threads, None)
    # The capture's own measurable ceiling, resolved once from the base render
    # and used on every side of every comparison — `analyze_hit` requires the
    # same value on both, and a value re-derived per render would move with the
    # perturbation it is meant to be independent of.
    band_edge = reference_band_edge(base, rows_a) if subject.percussive else None
    if band_edge is not None:
        rows_a, mono_a = _measure(raw_a, pattern, threads, band_edge)
        out_band = round(band_edge / 1000.0, 1)
    else:
        out_band = None

    out = {
        "capture": subject.capture,
        "timbre": base.timbre,
        "tone_class": ("percussion" if subject.percussive else tone_class(subject.program).value),
        "weights": weights,
        "slots": len(pattern.notes),
        "notes": list(subject.notes),
        "velocities": list(subject.velocities),
        "seconds": round(len(raw_a) / SR, 1),
        "band_edge_khz": out_band,
        "spread": [],
        "perturbations": [],
    }

    # The yardstick, one entry per other reference timbre.
    scales: list[tuple[str, LossWeights, LossWeights, dict]] = []
    for timbre in others:
        other = load_corpus(manifest, timbre)
        raw_b = to_mono(corpus_oracle(other, pattern, SR))
        rows_b, mono_b = _measure(raw_b, pattern, threads, band_edge)
        fwd = _terms(rows_b, rows_a, mono_b, mono_a, subject, groups)
        rev = _terms(rows_a, rows_b, mono_a, mono_b, subject, groups)
        if fwd is None or rev is None:
            print(
                f"  {subject.capture}/{timbre}: the pair did not compare — skipped", file=sys.stderr
            )
            continue
        lw_f, lw_r = LossWeights(dict(weights)), LossWeights(dict(weights))
        lw_f.calibrate(fwd)
        lw_r.calibrate(rev)
        scales.append((timbre, lw_f, lw_r, fwd))
        out["spread"].append(
            {
                "against": timbre,
                "raw": {t: round(fwd.get(t, 0.0), 4) for t in LOSS_TERMS},
                "counts": {
                    k: fwd.get(k)
                    for k in (
                        "harm_bins",
                        "modes_notes",
                        "mod_notes",
                        "tnr_notes",
                        "dyn_groups",
                        "stiff_notes",
                        "band_bins",
                        "bdecay_bins",
                        "lf_notes",
                        "kit_notes",
                    )
                    if fwd.get(k) is not None
                },
            }
        )
    if not scales:
        return out

    # Every perturbation, at its amplitude and at zero.
    for pert in PERTURBATIONS:
        if pert.pitched_only and subject.percussive:
            out["perturbations"].append({"key": pert.key, "skipped": "no harmonic ladder on a kit"})
            continue
        for tag, amp in (("", pert.amp), ("_zero", pert.zero)):
            raw_p, how = pert.fn(raw_a, pattern, rows_a, amp)
            rows_p, mono_p = _measure(raw_p, pattern, threads, band_edge)
            fwd = _terms(rows_p, rows_a, mono_p, mono_a, subject, groups)
            rev = _terms(rows_a, rows_p, mono_a, mono_p, subject, groups)
            entry = {
                "key": pert.key + tag,
                "label": pert.label if not tag else pert.label + " [amplitude 0]",
                "how": how,
                "amp": amp,
                "delta_db": _round(_delta_db(raw_a, raw_p)),
                "raw": None if fwd is None else {t: round(fwd.get(t, 0.0), 4) for t in LOSS_TERMS},
                "ratio": {},
                "ratio_reversed": {},
                "term_ratio": {},
            }
            witnesses = []
            if pert.witness:
                witnesses.append(_witness(rows_a, rows_p, pert.witness))
            if pert.key == "p1_high_partials":
                before = _partial_band_db(raw_a, pattern, rows_a, 14.5, 30.5)
                after = _partial_band_db(raw_p, pattern, rows_p, 14.5, 30.5)
                witnesses.append(
                    "h15-h30 share of slot energy: "
                    + (
                        "not measurable"
                        if before is None or after is None
                        else f"{before:.2f} -> {after:.2f} dB"
                    )
                )
            if pert.key == "p4_invert_dyn":
                sa, sp = _brightness_slopes(rows_a), _brightness_slopes(rows_p)
                witnesses.append(
                    "brightness slope per 64 velocity steps: "
                    + ", ".join(
                        f"n{n} {sa[n]:+.2f} -> {sp.get(n, float('nan')):+.2f}" for n in sorted(sa)
                    )
                    or "no velocity axis"
                )
            if pert.key == "p6_gain" and fwd is not None:
                witnesses.append(f"level_offset_db: {fwd.get('level_offset_db', 0.0):+.2f}")
            entry["witness"] = witnesses
            for timbre, lw_f, lw_r, spread in scales:
                entry["ratio"][timbre] = _round(lw_f.combine(fwd))
                entry["ratio_reversed"][timbre] = _round(lw_r.combine(rev))
            if fwd is not None:
                ref = scales[0][3]
                entry["term_ratio"] = {
                    t: _round(fwd.get(t, 0.0) / max(ref.get(t, 0.0), TERM_UNITS[t]))
                    for t in LOSS_TERMS
                }
            out["perturbations"].append(entry)
    return out


def _round(x: float) -> float | None:
    return None if x is None or not math.isfinite(x) else round(float(x), 4)


def _reference_timbres(manifest: Path) -> list[str]:
    """Timbre ids the manifest carries that are references rather than models."""
    data = json.loads(manifest.read_text())
    return [t["id"] for t in data.get("timbres", []) if isinstance(t, dict) and not t.get("model")]


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #
def _print(result: dict) -> None:
    print(f"\n=== {result['capture']} — {result['tone_class']}, base timbre {result['timbre']!r}")
    print(
        f"    reach: {result['slots']} slots "
        f"({len(result['notes'])} notes x {len(result['velocities'])} velocities), "
        f"{result['seconds']} s per render"
    )
    print("    weights: " + " ".join(f"{t}={w:g}" for t, w in sorted(result["weights"].items())))
    weighted = {t for t, w in result["weights"].items() if w > 0.0}
    for entry in result["spread"]:
        shown = " ".join(f"{t}={v:g}" for t, v in entry["raw"].items() if t in weighted)
        print(f"    yardstick vs {entry['against']!r}: raw {shown}")
        print("      counts: " + " ".join(f"{k}={v:g}" for k, v in entry["counts"].items()))
    if not result["perturbations"]:
        return
    names = [e["against"] for e in result["spread"]]
    head = "  ".join(f"{n[:12]:>12}" for n in names)
    print(f"\n    {'perturbation':<30} {head}   (1.0 = the reference spread)")
    for pert in result["perturbations"]:
        if "skipped" in pert:
            print(f"    {pert['key']:<30} skipped: {pert['skipped']}")
            continue
        cells = []
        for name in names:
            got = pert["ratio"].get(name)
            cells.append(f"{'n/a' if got is None else format(got, '.3f'):>12}")
        print(f"    {pert['key']:<30} " + "  ".join(cells))
        rev = [
            f"{n}={format(v, '.3f') if v is not None else 'n/a'}"
            for n, v in pert["ratio_reversed"].items()
        ]
        print("      reversed (perturbed side as the oracle): " + " ".join(rev))
        print(f"      perturbation size: {pert['delta_db']} dB re the render; {pert['how']}")
        for line in pert.get("witness", []):
            print(f"      {line}")
        moved = {t: r for t, r in pert["term_ratio"].items() if r is not None and r >= 0.05}
        if moved:
            print(
                "      per-term, against the same spread: "
                + " ".join(
                    f"{t}{'' if t in weighted else '(w0)'}={r:.2f}"
                    for t, r in sorted(moved.items(), key=lambda kv: -kv[1])
                )
            )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--root",
        default="",
        help="scratch root (default: $SONARE_VOICEMATCH_ROOT, else .cache/voicematch)",
    )
    ap.add_argument("--subject", default="", help="comma-separated capture ids")
    ap.add_argument(
        "--threads", type=int, default=2, help="notes measured at once (this machine caps at 2-3)"
    )
    ap.add_argument("--json", default="", help="write the full result here")
    args = ap.parse_args(argv)

    import os

    root = (
        Path(args.root or os.environ.get("SONARE_VOICEMATCH_ROOT") or ".cache/voicematch")
        .expanduser()
        .resolve()
    )
    want = {s.strip() for s in args.subject.split(",") if s.strip()}
    subjects = [s for s in SUBJECTS if not want or s.capture in want]

    print(f"loss sensitivity — scratch root {root}")
    print(
        f"{len(subjects)} subject(s), {len(PERTURBATIONS)} perturbations, "
        f"each also run at amplitude 0"
    )
    results = []
    for subject in subjects:
        try:
            got = run_subject(subject, root, max(1, args.threads))
        except Exception as exc:  # noqa: BLE001 - a subject that cannot be
            # measured is one row of the result, never the end of the run
            print(f"  {subject.capture}: {type(exc).__name__}: {exc}", file=sys.stderr)
            continue
        if got is not None:
            results.append(got)
            _print(got)
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1))
        print(f"\nwrote {args.json}")
    if not results:
        print("\nNOTHING WAS COMPARED — no subject produced a yardstick.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
