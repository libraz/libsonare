"""The five terms combined into one number a search can minimise.

Floor discipline. Cells where both sides sit below the reference note's peak
minus `floor_db` carry no comparison and are dropped rather than counted as a
confident zero -- otherwise silence against silence fills most of the plane and
every real error is divided by it. Cells where exactly one side is above the
floor are the important ones, a missing partial or one the model invented, and
they are kept with the quiet side held at the floor so the term stays bounded.

Weighting is by the louder of the two sides, not by the reference. A cell where
the reference is 40 dB down and the model is 15 dB up is a partial the model
invented, and weighting it by the quiet side files the loudest defect in the
voice under noise. What is heard is the maximum of the two.

Gain frame. Exactly one scalar is removed across the whole note set: the mean
held-level offset. Per-note level error is a real error and stays in, and so
does the model's own loudness-versus-velocity curve, which normalising each
layer separately would grant for free.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import terms
from .bed import MARGIN_DB
from .partials import (
    RESOLVABLE_PARTIAL,
    fit_inharmonicity,
    harmonic_rows,
    note_hz,
)
from .spectro import Spectro

FLOOR_DB = 85.0
CELL_CLIP = 40.0
#: dB below a note's own peak at which a cell's weight bottoms out.
WEIGHT_RANGE = 70.0
WEIGHT_FLOOR = 0.03

DEFAULT_WEIGHTS = {"spectrum": 1.0, "onset": 1.0, "residue": 0.7,
                   "invariance": 1.0, "release": 1.0, "balance": 1.0,
                   "recurrence": 1.0}


@dataclass
class Terms:
    """One score, its parts, and the gain that was removed to reach it."""

    total: float
    parts: dict[str, float]
    per_note: dict
    gain_db: float

    def __str__(self) -> str:
        return f"{self.total:.3f}  " + "  ".join(
            f"{k} {v:.2f}" for k, v in self.parts.items())


@dataclass
class ShapeLoss:
    """A comparison bound to one corpus, one bed, and one analysis geometry."""

    signals: object
    spectro: Spectro = field(default_factory=Spectro)
    bed: object = None
    #: Note-off time in the capture, from its gate. The release term straddles it.
    note_off_s: float = 8.1
    weights: dict = field(default_factory=lambda: dict(DEFAULT_WEIGHTS))
    velocities: tuple = (56, 88, 120)
    floor_db: float = FLOOR_DB

    def __post_init__(self) -> None:
        self._ref: dict = {}
        end = self.spectro.seconds
        self.release_pre = (self.note_off_s - 0.5, self.note_off_s - 0.1)
        self.release_post = (min(self.note_off_s + 1.1, end - 0.4), end)

    def pairs(self, notes):
        return [(n, v) for n in notes for v in self.velocities]

    def reference(self, notes):
        """Everything measured off the reference once, cached by note set."""
        key = tuple(sorted(notes))
        if key in self._ref:
            return self._ref[key]
        sigs = self.signals(self.pairs(notes), ref=True)
        pack = {}
        for k, sig in sigs.items():
            R = self.spectro(sig)
            clean, beds = [], []
            for s in range(len(self.spectro.scales)):
                if self.bed is None:
                    clean.append(R[s])
                    beds.append(np.full_like(R[s], -400.0))
                else:
                    c, b = self.bed.clean(self.spectro, R[s], s)
                    clean.append(c)
                    beds.append(b)
            f0 = note_hz(k[0])
            B = fit_inharmonicity(sig, f0, self.spectro.sample_rate)
            hm = harmonic_rows(self.spectro.rows_hz(0), f0, B)
            pack[k] = {
                "clean": clean, "bed": beds, "harmonic": hm,
                "held": terms.held_db(sig, sr=self.spectro.sample_rate),
                "onset": terms.onset_stats(sig, self.spectro.sample_rate),
                "residue": terms.residue_ratio(self.spectro, clean[0], hm),
                "valid": terms.residue_valid(self.spectro, clean[0], beds[0], hm),
                "curve": terms.residue_curve(self.spectro, clean[0], hm),
                "release": terms.release_stats(sig, self.release_pre,
                                               self.release_post,
                                               self.spectro.sample_rate),
                "balance": [terms.band_balance(self.spectro, clean[0], w)
                            for w in terms.BALANCE_WINDOWS],
                # Capped at a resolvable partial, unlike the mask above it: this
                # one is asking what is up there BESIDES the string, and the
                # uncapped mask covers a bass note's whole upper spectrum.
                "peaks": terms.peak_rows(
                    self.spectro, clean[0],
                    harmonic_rows(self.spectro.rows_hz(0), f0, B,
                                  max_partial=RESOLVABLE_PARTIAL)),
                "peak_mask": harmonic_rows(self.spectro.rows_hz(0), f0, B,
                                           max_partial=RESOLVABLE_PARTIAL),
            }
        self._ref[key] = pack
        return pack

    def score(self, overrides: str = "", notes=(), detail: bool = False) -> Terms:
        ref = self.reference(notes)
        keys = self.pairs(notes)
        mod = self.signals(keys, ov=overrides)
        sr = self.spectro.sample_rate
        g = float(np.mean([terms.held_db(mod[k], sr=sr) - ref[k]["held"] for k in keys]))

        per_note, cells, grids = {}, [], {}
        onset_err, res_err, rel_err, bal_err = [], [], [], []
        mcurves, rcurves = {}, {}
        mpeaks, rpeaks = {}, {}
        for k in keys:
            r = ref[k]
            ml, mr = terms.onset_stats(mod[k], sr)
            rl, rr = r["onset"]
            onset_err.append(np.clip(ml - rl - g, -terms.ONSET_CLIP, terms.ONSET_CLIP))
            # Time-to-rise as a ratio in decibels, not a difference in
            # milliseconds: eight against eighty-five is the same kind of error
            # as eighty against eight hundred and fifty.
            onset_err.append(np.clip(
                20.0 * np.log10(np.maximum(mr, 0.5) / np.maximum(rr, 0.5)),
                -terms.ONSET_CLIP, terms.ONSET_CLIP))

            # One-sided, and scored only while the reference still had a note to
            # damp: once it has decayed past seventy decibels under its own peak
            # the ratio is about its floor and not about its felt.
            mrel = terms.release_stats(mod[k], self.release_pre, self.release_post, sr)
            if r["release"][0] > -70.0:
                rel_err.append(min(max(mrel[1] - r["release"][1], 0.0), terms.RELEASE_CLIP))

            M = self.spectro(mod[k])
            # Two-sided and gain-free: a band the model under-fills costs
            # exactly what a band it over-fills costs. Bands the reference
            # itself does not occupy are not asked about, so a model is not
            # charged for failing to reproduce a floor.
            for w, rb in zip(terms.BALANCE_WINDOWS, r["balance"]):
                mb = terms.band_balance(self.spectro, M[0], w)
                live = rb > terms.BALANCE_LIVE_DB
                if live.any():
                    bal_err.append(np.clip(mb[live] - rb[live],
                                           -terms.BALANCE_CLIP, terms.BALANCE_CLIP))
            res_err.append(np.clip(
                terms.residue_ratio(self.spectro, M[0], r["harmonic"]) - r["residue"],
                -terms.RESIDUE_CLIP, terms.RESIDUE_CLIP)[r["valid"]])
            mcurves.setdefault(k[1], []).append(
                terms.residue_curve(self.spectro, M[0], r["harmonic"]))
            rcurves.setdefault(k[1], []).append(r["curve"])
            mpeaks.setdefault(k[1], []).append(
                terms.peak_rows(self.spectro, M[0], r["peak_mask"]))
            rpeaks.setdefault(k[1], []).append(r["peaks"])

            note_cells = []
            for s in range(len(self.spectro.scales)):
                R, B = r["clean"][s], r["bed"][s]
                # The floor is per note and per scale: a short window holds less
                # energy per frame, so one absolute number would floor the whole
                # short-window plane and score nothing there.
                fl = max(float(R.max()), float((M[s] - g).max())) - self.floor_db
                m = np.maximum(M[s] - g, fl)
                rr_ = np.maximum(R, fl)
                live = (R > B + MARGIN_DB) & (R > fl)
                plain = np.clip(m - rr_, -CELL_CLIP, CELL_CLIP)
                # Where a floor was measured, a cell the reference cannot speak
                # for charges the model's excess over that floor and nothing
                # else. Where none was -- an unsampled reference, or a corpus
                # whose notes did not agree on one -- there is no such level,
                # and charging against the sentinel would score every quiet cell
                # in the plane at the clip. The ordinary floored comparison is
                # what is left, and it is zero when both sides are silent.
                known = B > -300.0
                excess = np.clip(np.maximum(m - (B + MARGIN_DB), 0.0), 0.0, CELL_CLIP)
                d = np.where(live, plain, np.where(known, excess, plain))
                keep = live | np.where(known, m > B + MARGIN_DB, m > fl)
                pk = max(float(R.max()), float(m.max()))
                w = np.clip((np.maximum(rr_, m) - (pk - WEIGHT_RANGE)) / WEIGHT_RANGE,
                            WEIGHT_FLOOR, 1.0)
                note_cells.append((np.abs(d[keep]), w[keep]))
                if detail:
                    grids[(k, s)] = (rr_, m, d, live)
            dn = np.concatenate([a for a, _ in note_cells])
            wn = np.concatenate([b for _, b in note_cells])
            per_note[k] = float(np.sqrt(np.sum(wn * dn ** 2) / np.sum(wn)))
            cells.append((dn, wn))

        da = np.concatenate([a for a, _ in cells])
        wa = np.concatenate([b for _, b in cells])
        parts = {"spectrum": float(np.sqrt(np.sum(wa * da ** 2) / np.sum(wa)))}
        oe = np.concatenate(onset_err)
        parts["onset"] = float(np.sqrt(np.mean(oe ** 2)))
        re_ = [x for x in res_err if x.size]
        parts["residue"] = float(np.sqrt(np.mean(np.concatenate(re_) ** 2))) if re_ else 0.0
        # One-sided: a model that rings less at fixed pitches than the reference
        # does is not thereby wrong, and the reference's own figure carries the
        # residual of its bed subtraction, which is a floor and not a target.
        inv = []
        for v in mcurves:
            mi = terms.invariant_floor(mcurves[v])
            ri = terms.invariant_floor(rcurves[v])
            live = ri > -300.0
            inv.append(np.clip(np.maximum(mi[live] - ri[live], 0.0),
                               0.0, terms.INVARIANCE_CLIP))
        parts["invariance"] = float(np.sqrt(np.mean(np.concatenate(inv) ** 2))) if inv else 0.0
        parts["release"] = float(np.sqrt(np.mean(np.array(rel_err) ** 2))) if rel_err else 0.0
        parts["balance"] = float(np.sqrt(np.mean(np.concatenate(bal_err) ** 2))) \
            if bal_err else 0.0
        # Recurrence: per row, how much more often the model's aftersound peaks
        # there than the instrument's, in percentage points of the note set.
        #
        # This is the term the other six could not be. Every one of them prices
        # a LEVEL, and a bank of resonators at fixed frequencies is not a level
        # -- it is a coincidence, the same pitches answering whatever is struck,
        # and it is what the ear calls a bell. `invariance` was written for it
        # and cannot see it: a row-wise minimum across notes prices invariant
        # energy by its weakest relative appearance, so a bank that is a small
        # fraction of a loud note and a large fraction of a quiet one has no
        # weak appearance to be found by. Three separate changes improved every
        # score available while making the voice audibly a chime, and the ear
        # caught all three after the numbers had not.
        #
        # Counting instead of measuring is what fixes it, and it needs the whole
        # note set at once -- which is why it lives here rather than in a probe.
        # One-sided: a real instrument's body does recur, and recurring LESS
        # than it does is a different complaint that `residue` already carries.
        rec = []
        for v in mpeaks:
            if not mpeaks[v]:
                continue
            mf = np.mean(np.stack(mpeaks[v]), axis=0) * 100.0
            rf = np.mean(np.stack(rpeaks[v]), axis=0) * 100.0
            rec.append(np.clip(np.maximum(mf - rf, 0.0), 0.0, terms.RECUR_CLIP))
        parts["recurrence"] = float(np.sqrt(np.mean(np.concatenate(rec) ** 2))) \
            if rec else 0.0

        num = sum(self.weights[k] * parts[k] ** 2 for k in parts)
        total = float(np.sqrt(num / sum(self.weights[k] for k in parts)))
        out = Terms(total=total, parts=parts, per_note=per_note, gain_db=g)
        return (out, grids) if detail else out
