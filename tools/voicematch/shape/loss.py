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

Cost. A note's render and everything read off it are cached against the part of
the override string that can reach that note, so a candidate touching one piece
of a kit re-renders one piece. See `_models`.
"""

from __future__ import annotations

import threading
from collections import OrderedDict
from dataclasses import dataclass, field

import numpy as np

from . import struck, terms
from .bed import MARGIN_DB
from .partials import (
    RESOLVABLE_PARTIAL,
    fit_inharmonicity,
    harmonic_rows,
    note_hz,
)
from .render import scope_overrides
from .spectro import Spectro

FLOOR_DB = 85.0
CELL_CLIP = 40.0
#: dB below a note's own peak at which a cell's weight bottoms out.
WEIGHT_RANGE = 70.0
WEIGHT_FLOOR = 0.03

DEFAULT_WEIGHTS = {"spectrum": 1.0, "onset": 1.0, "residue": 0.7,
                   "invariance": 1.0, "release": 1.0, "balance": 1.0,
                   "recurrence": 1.0}

#: What a struck, unpitched piece is scored on. Two of the seven above are gone
#: and two others take their place; the four that stay need no partial series
#: and are unchanged.
#:
#: `residue` asks how much of a render is NOT the played string, which on a
#: piece with no played partials is the whole render — the ratio's denominator
#: is empty and the question has no percussion form. `release` straddles a
#: note-off that a one-shot voice does not have. `invariance` and `recurrence`
#: both survive with an empty mask and mean MORE here than they do on a
#: keyboard: a kit whose pieces all answer at the same frequencies is one plate
#: wearing several names, which is the exact failure a shared resonator causes.
STRUCK_WEIGHTS = {"spectrum": 1.0, "onset": 1.0, "invariance": 1.0,
                  "balance": 1.0, "recurrence": 1.0, "density": 1.0,
                  "prompt": 1.0}


@dataclass
class Terms:
    """One score, its parts, and the gain that was removed to reach it."""

    total: float
    parts: dict[str, float]
    per_note: dict
    gain_db: float

    #: Terms this instrument is scored on that nothing could be read for. They
    #: are out of the total rather than in it at zero, and named here because a
    #: score built from fewer terms is not the same number as one built from all
    #: of them and must not read as one.
    unscored: tuple = ()

    def __str__(self) -> str:
        out = f"{self.total:.3f}  " + "  ".join(
            f"{k} {v:.2f}" for k, v in self.parts.items())
        return out + ("  [unscored: " + ", ".join(self.unscored) + "]"
                      if self.unscored else "")


@dataclass
class ShapeLoss:
    """A comparison bound to one corpus, one bed, and one analysis geometry."""

    signals: object
    spectro: Spectro = field(default_factory=Spectro)
    bed: object = None
    #: Note-off time in the capture, from its gate. The release term straddles it.
    note_off_s: float = 8.1
    weights: dict = None
    velocities: tuple = (56, 88, 120)
    floor_db: float = FLOOR_DB
    #: Whether the note number names a pitch. False for a kit, where it names an
    #: instrument: the partial mask has nothing to mask and every window is set
    #: by the piece's own decay rather than by a keyboard's note length.
    pitched: bool = True
    #: Megabytes of analysed model notes to keep. One entry is a spectrogram at
    #: three scales, a couple of megabytes on this geometry, so the budget is
    #: what decides how many candidates' worth of grid the cache spans. Zero
    #: switches it off, which is what the equivalence test scores against.
    #:
    #: The floor below is not optional: with room for less than two grids the
    #: cache evicts the notes the NEXT candidate needs, which costs the analysis
    #: and buys nothing. A grid too large for the budget therefore takes the
    #: memory anyway rather than thrashing quietly -- the reference side already
    #: keeps the whole grid analysed with no cap at all, so this is proportional
    #: to a cost the comparison was paying before the cache existed.
    cache_mb: float = 384.0

    def __post_init__(self) -> None:
        self._ref: dict = {}
        self._packs: OrderedDict = OrderedDict()
        self._pack_lock = threading.Lock()
        self._entry_bytes = 0
        #: Renders asked of `signals`, for the tests that price the cache.
        self.rendered = 0
        if self.weights is None:
            self.weights = dict(DEFAULT_WEIGHTS if self.pitched else STRUCK_WEIGHTS)
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
            sr = self.spectro.sample_rate
            if self.pitched:
                f0 = note_hz(k[0])
                B = fit_inharmonicity(sig, f0, sr)
                hm = harmonic_rows(self.spectro.rows_hz(0), f0, B)
                # Capped at a resolvable partial, unlike the mask above it: this
                # one is asking what is up there BESIDES the string, and the
                # uncapped mask covers a bass note's whole upper spectrum.
                pm = harmonic_rows(self.spectro.rows_hz(0), f0, B,
                                   max_partial=RESOLVABLE_PARTIAL)
                bal_windows = terms.BALANCE_WINDOWS
                late = terms.RECUR_WINDOW
                held_window = (0.4, 1.4)
            else:
                # Nothing to notch, and every window from the piece's own decay.
                # A kit's pieces are an order of magnitude apart in length, so a
                # fixed aftersound window reads a ride's tail and a hi-hat's
                # silence and reports the two as the same kind of measurement.
                hm = np.zeros(self.spectro.rows_hz(0).shape, dtype=bool)
                pm = hm
                body, late = struck.windows(sig, sr)
                bal_windows = (body, late)
                held_window = body
            pack[k] = {
                "clean": clean, "bed": beds, "harmonic": hm,
                "windows": bal_windows, "late": late, "held_window": held_window,
                "held": terms.held_db(sig, window=held_window, sr=sr),
                "onset": terms.onset_stats(sig, sr),
                "curve": terms.residue_curve(self.spectro, clean[0], hm,
                                             window=late),
                "balance": [terms.band_balance(self.spectro, clean[0], w)
                            for w in bal_windows],
                "peaks": terms.peak_rows(self.spectro, clean[0], pm, window=late),
                "peak_mask": pm,
            }
            if self.pitched:
                pack[k].update({
                    "residue": terms.residue_ratio(self.spectro, clean[0], hm),
                    "valid": terms.residue_valid(self.spectro, clean[0], beds[0], hm),
                    "release": terms.release_stats(sig, self.release_pre,
                                                   self.release_post, sr),
                })
            else:
                d, dok = struck.mode_count(sig, late, sr)
                p, pok = struck.prompt_late(sig, bal_windows[0], late, sr)
                pack[k].update({"density": (d, dok), "prompt": (p, pok)})
        self._ref[key] = pack
        return pack

    def _model(self, k, sig, r) -> dict:
        """Everything read off one model render that the gain frame cannot move.

        Separated so it can be kept: the gain is a mean over the whole note set
        and changes whenever any note in the set does, so nothing measured
        against it survives a neighbouring note moving. Everything here is
        measured against the reference alone, which is fixed.
        """
        sr = self.spectro.sample_rate
        M = self.spectro(sig)
        pack = {
            "M": M,
            "held": terms.held_db(sig, window=r["held_window"], sr=sr),
            "onset": terms.onset_stats(sig, sr),
            "balance": [terms.band_balance(self.spectro, M[0], w)
                        for w in r["windows"]],
            "curve": terms.residue_curve(self.spectro, M[0], r["harmonic"],
                                         window=r["late"]),
            "peaks": terms.peak_rows(self.spectro, M[0], r["peak_mask"],
                                     window=r["late"]),
        }
        if self.pitched:
            pack["release"] = terms.release_stats(sig, self.release_pre,
                                                  self.release_post, sr)
            pack["residue"] = terms.residue_ratio(self.spectro, M[0], r["harmonic"])
        else:
            pack["density"] = struck.mode_count(sig, r["late"], sr)
            pack["prompt"] = struck.prompt_late(sig, r["windows"][0], r["late"], sr)
        return pack

    def _models(self, keys, ref, overrides: str) -> dict:
        """One analysed pack per note, rendering only the notes that moved.

        Keyed on the note, the layer, and `scope_overrides` -- the part of the
        candidate a note can read. A coordinate descent over a kit changes one
        piece at a time, so every other piece's key is the one the previous
        evaluation already stored and the render is skipped.

        The notes that do have to be rendered go out in a single call, under the
        WHOLE override string rather than the scoped one. That is the same render
        by the identity the key rests on, and it keeps the batch to one
        subprocess; splitting it per scope would trade the process launches back.

        Duplicate work under concurrency is possible and not worth locking
        against: two threads that want the same cold note both compute it and one
        result is discarded. It stays rare because the callers score the current
        state serially before fanning out -- `Descent.run` for its starting
        score, `ablate` for `f0` -- which is what puts the shared notes in the
        cache before any pool starts.
        """
        want = {k: (k, scope_overrides(overrides, k[0])) for k in keys}
        out, missing = {}, []
        if self.cache_mb:
            with self._pack_lock:
                for k in keys:
                    p = self._packs.get(want[k])
                    if p is None:
                        missing.append(k)
                    else:
                        self._packs.move_to_end(want[k])
                        out[k] = p
        else:
            missing = list(keys)
        if missing:
            with self._pack_lock:
                self.rendered += len(missing)
            sigs = self.signals(missing, ov=overrides)
            for k in missing:
                out[k] = self._model(k, sigs[k], ref[k])
            if self.cache_mb:
                with self._pack_lock:
                    for k in missing:
                        self._packs[want[k]] = out[k]
                        self._packs.move_to_end(want[k])
                    cap = self.cache_limit(len(keys))
                    while len(self._packs) > cap:
                        self._packs.popitem(last=False)
        return out

    def cache_limit(self, grid: int) -> int:
        """Entries the budget allows, but never fewer than two grids."""
        if not self._entry_bytes and self._packs:
            p = next(reversed(self._packs.values()))
            self._entry_bytes = sum(a.nbytes for a in p["M"])
        by_budget = int(self.cache_mb * 1e6 / self._entry_bytes) \
            if self._entry_bytes else 0
        return max(2 * grid, by_budget)

    def score(self, overrides: str = "", notes=(), detail: bool = False) -> Terms:
        ref = self.reference(notes)
        keys = self.pairs(notes)
        mod = self._models(keys, ref, overrides)
        g = float(np.mean([mod[k]["held"] - ref[k]["held"] for k in keys]))

        per_note, cells, grids = {}, [], {}
        onset_err, res_err, rel_err, bal_err = [], [], [], []
        den_err, pro_err = [], []
        mcurves, rcurves = {}, {}
        mpeaks, rpeaks = {}, {}
        for k in keys:
            r, mo = ref[k], mod[k]
            ml, mr = mo["onset"]
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
            if self.pitched and r["release"][0] > -70.0:
                rel_err.append(min(max(mo["release"][1] - r["release"][1], 0.0),
                                   terms.RELEASE_CLIP))

            M = mo["M"]
            # Two-sided and gain-free: a band the model under-fills costs
            # exactly what a band it over-fills costs. Bands the reference
            # itself does not occupy are not asked about, so a model is not
            # charged for failing to reproduce a floor.
            for mb, rb in zip(mo["balance"], r["balance"]):
                live = rb > terms.BALANCE_LIVE_DB
                if live.any():
                    bal_err.append(np.clip(mb[live] - rb[live],
                                           -terms.BALANCE_CLIP, terms.BALANCE_CLIP))
            if self.pitched:
                res_err.append(np.clip(
                    mo["residue"] - r["residue"],
                    -terms.RESIDUE_CLIP, terms.RESIDUE_CLIP)[r["valid"]])
            else:
                # Both are two-sided. Too sparse is a tuned bar and too diffuse
                # is a hiss, and a strike that keeps its top and one that loses
                # it are different pieces -- neither direction is free.
                md, mok = mo["density"]
                rd, rok = r["density"]
                ok = mok & rok
                if ok.any():
                    den_err.append(np.clip(md[ok] - rd[ok],
                                           -struck.DENSITY_CLIP, struck.DENSITY_CLIP))
                # The REFERENCE decides which bands are asked about. A model
                # silent where the instrument is not is the finding, so its own
                # mask must not be allowed to withdraw the question.
                mp, _ = mo["prompt"]
                rp, rpok = r["prompt"]
                ok = rpok
                if ok.any():
                    pro_err.append(np.clip(mp[ok] - rp[ok],
                                           -struck.PROMPT_CLIP, struck.PROMPT_CLIP))
            mcurves.setdefault(k[1], []).append(mo["curve"])
            rcurves.setdefault(k[1], []).append(r["curve"])
            mpeaks.setdefault(k[1], []).append(mo["peaks"])
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
        if self.pitched:
            re_ = [x for x in res_err if x.size]
            parts["residue"] = float(np.sqrt(np.mean(np.concatenate(re_) ** 2))) \
                if re_ else 0.0
        else:
            # Omitted rather than zeroed when nothing could be read. Both are
            # gated -- a band under its own recording's floor is not asked about
            # -- and a gate that rejects everything leaves an empty average,
            # which scores as a perfect match for a measurement that never ran.
            # Leaving the key out keeps it out of the weighted mean as well, and
            # `Terms` prints what was not scored so a shorter list is visible
            # rather than silently better.
            if den_err:
                parts["density"] = float(np.sqrt(np.mean(np.concatenate(den_err) ** 2)))
            if pro_err:
                parts["prompt"] = float(np.sqrt(np.mean(np.concatenate(pro_err) ** 2)))
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
        if self.pitched:
            parts["release"] = float(np.sqrt(np.mean(np.array(rel_err) ** 2))) \
                if rel_err else 0.0
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
        missing = tuple(k for k in self.weights if k not in parts)
        out = Terms(total=total, parts=parts, per_note=per_note, gain_db=g,
                    unscored=missing)
        return (out, grids) if detail else out
