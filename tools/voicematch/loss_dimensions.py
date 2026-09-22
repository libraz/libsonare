"""The per-dimension loss terms: a model/oracle pair of row lists in, a scalar out."""

from __future__ import annotations

import math

import numpy as np
from metrics import (
    MIN_PARTIALS_FOR_B,
    N_HARMONICS,
    THIRD_OCTAVE_CENTERS,
    stretch_cents,
)

#: How far up the ladder `harm` compares. `analyze_note` measures exactly
#: `N_HARMONICS` bins, so this is the whole of what the measurement offers
#: rather than a preference — a caller asking for fewer leaves the bins above
#: its number charged by nothing, since `tnr`'s harmonic mask treats them as
#: partials and excludes them from the noise it prices.
HARM_REACH = N_HARMONICS

# The terms the loss is built from, in report order. `harm`/`cents`/`tnr`/`init`/
# `slope`/`tail`/`hf`/`lf` come from the harmonic metric set and `band`/`bdecay`
# from the percussion one; `env`, `mss`, `level` and `crest` are computed for
# both. A run uses one group or the other — which one the probe pattern decides
# — and the unused terms stay at zero weight.
LOSS_TERMS = (
    "harm",
    "modes",
    "cents",
    "tnr",
    "mod",
    "env",
    "init",
    "slope",
    "tail",
    "hf",
    "lf",
    "stiff",
    "level",
    "crest",
    "dyn",
    "mss",
    "band",
    "bdecay",
    "tilt",
    "bright",
    "kit",
)

# Per-note caps, in each term's own units. A reference row can be measuring
# almost nothing — a partial 58 dB under the fundamental, a band the capture has
# no energy in — and an uncapped delta against one of those decides the whole
# objective on its own.
TAIL_DELTA_CAP_DB_S = 20.0

# What a model note whose sustain window sat on the dB clamp costs `env`. Same
# 30 dB/s the skeleton's early/late bands are capped at, because it is the same
# quantity; the absence it charges for is a note that stopped being there.
SUSTAIN_SLOPE_CAP_DB_S = 30.0
HF_DELTA_CAP_DB = 24.0
# Same cap as the high bands, and it binds far more often. A reference's 20-60
# Hz share on a treble note is genuinely tiny, so an uncapped delta there would
# let the top octave — where the band carries nothing anyone can hear — outvote
# the bass, which is the only register the band was added to watch.
LF_DELTA_CAP_DB = 24.0
LEVEL_DELTA_CAP_DB = 18.0

# Which terms a metric set actually produces. Both reducers fill every entry of
# LOSS_TERMS so the dict has one shape, which means a term belonging to the
# other set reads as exactly 0.0 — indistinguishable, in the dict alone, from a
# term the model matched perfectly. Anything that reasons about a residual
# rather than about the combined loss has to know the difference.
_SHARED_TERMS = ("env", "mss", "level", "crest", "dyn", "modes")
PITCHED_TERMS = (
    "harm",
    "cents",
    "tnr",
    "mod",
    "init",
    "slope",
    "tail",
    "hf",
    "lf",
    "stiff",
) + _SHARED_TERMS
#: `modes` is shared because a drum has one too. A tom, a conga, a timbale, a
#: woodblock and a cowbell all have a definite pitch, and the 1/3-octave band
#: profile cannot see it: a band is four semitones wide, so a tom two semitones
#: out of tune barely moves `band` at all. The model says the pitch is there —
#: `base_freq_hz`, `mode_ratios`, `pitch_drop` are all patch fields a fit can
#: move — and until this term existed nothing scored any of them.
#:
#: `lf` is shared for the reason it is a separate term at all. `band` is a mean
#: over 25 bands, so a kick 27 dB hot at 50 Hz moves it by about one unit —
#: nothing, against a term whose typical value is in the tens — while being the
#: single most audible error the kit can have. The low end is one region rather
#: than six independent bands, and `_perc_lf_terms` charges the balance between
#: it and everything above it as one number.
#:
#: `kit` is percussion-only because it needs a declared set of families to read
#: relations across, and only a percussion capture has one — a pitched voice's
#: between-note relations are `dyn` and `level`, which are the whole grid rather
#: than a family inside it.
#: `tilt` and `bright` are percussion-only, and they exist because `band` cannot
#: stand in for them. `band` is an L1 over 25 bands with each side normalised to
#: its OWN loudest band, so it charges a magnitude per band and never a
#: direction, and when a candidate's loudest band moves the anchor moves with
#: it: a whole-kit fit brought `band` down from 16.4 to 15.5 dB while taking the
#: tilt from 6.8 to 9.4 dB and the centroid from 39 % to 75 % over its
#: reference — the one dimension of eight that had been inside the reference
#: kits' own spread, pushed outside it. These two are the gate's own arithmetic,
#: read from the same `band_tilt_db` and the same centroid ratio, so a fit
#: optimises the quantity the kit is judged on rather than a proxy for it.
PERCUSSION_TERMS = ("band", "bdecay", "tilt", "bright", "lf", "kit") + _SHARED_TERMS


def measured_terms(percussive: bool) -> tuple[str, ...]:
    """The terms the probe's metric set produces, in LOSS_TERMS order."""
    group = PERCUSSION_TERMS if percussive else PITCHED_TERMS
    return tuple(t for t in LOSS_TERMS if t in group)


def _level_terms(model_rows: list[dict], oracle_rows_: list[dict]) -> tuple[float, float, float]:
    """The two level terms, plus the whole-grid offset they are measured against.

    Absolute dBFS is not comparable between a model rendered here and a
    reference captured through somebody else's output stage, and a term that
    treated it as comparable would spend the fit's budget on an output gain. So
    the grid's own median offset is removed first and what is scored is the
    residual — how the level is distributed across register and velocity, which
    is a property of the instrument — while the offset itself is returned for
    the report, since a fit that silently corrects a 9 dB calibration error is
    not something to discover later.

    Crest needs no such treatment: it is a difference of two levels from the
    same render, so any gain common to both cancels. It is also the sharper of
    the two here, because the defect it catches — a note whose envelope never
    falls after its attack — is invisible to every shape metric and to the level
    residual alike.

    Returns zeros when the rows carry no level fields, which is what a probe
    measured from a normalised render should score: nothing, rather than a
    match.
    """
    offsets: list[float] = []
    crests: list[float] = []
    # A note the oracle holds and the model does not is charged the cap rather
    # than dropped, for the reason `_absent_or` gives: dropping it lets a voice
    # that stopped sounding score better than one that sounds slightly wrong.
    # The level residual is measured around the grid's median offset, so an
    # absent row cannot join `offsets` without inventing a level for it; it is
    # counted in `crests`, which is where an envelope that never falls is
    # already the defect being caught.
    for m, o in zip(model_rows, oracle_rows_):
        mo, oo = m.get("held_rms_dbfs"), o.get("held_rms_dbfs")
        if mo is not None and oo is not None:
            offsets.append(mo - oo)
        mc, oc = m.get("held_crest_db"), o.get("held_crest_db")
        if oc is not None:
            crests.append(
                LEVEL_DELTA_CAP_DB if mc is None else min(abs(mc - oc), LEVEL_DELTA_CAP_DB)
            )
    if not offsets:
        return 0.0, (sum(crests) / len(crests) if crests else 0.0), 0.0
    median = sorted(offsets)[len(offsets) // 2]
    balance = sum(min(abs(d - median), LEVEL_DELTA_CAP_DB) for d in offsets) / len(offsets)
    crest = sum(crests) / len(crests) if crests else 0.0
    return balance, crest, median


# How brightness is read off whichever metric set the rows carry, and how far
# apart two velocities have to be before the pair says anything about a curve.
DYN_MIN_VELOCITY_SPREAD = 16.0
DYN_DELTA_CAP_DB = 12.0
# Slope is reported per this many velocity steps rather than per step, so the
# term arrives in the same order of magnitude as the dB terms beside it.
DYN_VELOCITY_SPAN = 64.0


def _brightness(row: dict) -> float | None:
    """How bright one note sounded, on the scale its own metric set provides.

    Both sources are already normalised to something inside the same note — the
    harmonic ladder to h1, the band profile to its loudest band — so this is a
    tilt and not a level, and it survives the RMS normalisation and the unknown
    output gain exactly as the terms it is derived from do.

    **The pitched branch is the one this term was shown to earn.** Two
    candidates with identical `harm` and opposite dynamics curves are told apart
    only here. The percussion branch works but has no such demonstration: it
    reads the upper third of the band profile, which is most of what `band`
    already compares, and on a synthetic kit it moved 0.89 where `band` moved
    22. Weight it on a drum fit expecting a refinement, not a new signal.
    """
    ladder = row.get("harmonics_db")
    if ladder is not None:
        upper = [v for v in ladder[3:10] if v > -120.0]
        return sum(upper) / len(upper) if upper else None
    bands = row.get("bands_db")
    if bands:
        # The upper third of the band profile, minus whatever the reference
        # floored. On the captured kit that is the 10 and 12.5 kHz bands on most
        # rows, so without this filter a third of what the drum branch reads is
        # the capture's own bottom rather than the hit's brightness — a term
        # nominally about how tone tracks force, partly measuring a constant.
        upper = [v for v in bands[len(bands) * 2 // 3 :] if v > BAND_REFERENCE_FLOOR_DB]
        return sum(upper) / len(upper) if upper else None
    return None


def _dyn_terms(model_rows: list[dict], oracle_rows_: list[dict]) -> tuple[float, int]:
    """How differently brightness tracks velocity, and how many notes said so.

    Every other term is a per-note comparison averaged over the probe, so the
    objective is blind to anything that lives in the RELATION between notes. A
    dynamics curve is exactly that: a struck or blown instrument gets brighter
    with force in a way that is characteristic of the mechanism, and a model can
    match every note of a grid one at a time while getting the trend between
    them wrong. It is also the axis a physical model should win on, since a
    sampled reference has only as many curves as it has velocity layers.

    Fitted per pitch, so register is held fixed and what is left is the response
    to force. Returns the count as well as the value because a probe with no
    velocity axis can only report zero here, and zero is this term's best score:
    the caller needs to be able to tell "matched" from "never measured".
    """
    # A row that does not name its pitch and velocity carries no dynamics
    # evidence — it is skipped rather than assumed, the same way a band the
    # oracle has nothing in is skipped, so a caller holding partial rows gets
    # "never measured" instead of a crash or an invented curve.
    groups: dict[int, list[int]] = {}
    for i, r in enumerate(model_rows):
        if r.get("note") is not None and r.get("velocity") is not None:
            groups.setdefault(r["note"], []).append(i)
    total = 0.0
    used = 0
    for idx in groups.values():
        pairs = []
        for i in idx:
            m, o = _brightness(model_rows[i]), _brightness(oracle_rows_[i])
            if m is not None and o is not None:
                pairs.append((float(model_rows[i]["velocity"]), m, o))
        if len(pairs) < 2:
            continue
        vel = [p[0] for p in pairs]
        if max(vel) - min(vel) < DYN_MIN_VELOCITY_SPREAD:
            continue
        v = np.asarray(vel, dtype=np.float64)
        sm = float(np.polyfit(v, [p[1] for p in pairs], 1)[0]) * DYN_VELOCITY_SPAN
        so = float(np.polyfit(v, [p[2] for p in pairs], 1)[0]) * DYN_VELOCITY_SPAN
        total += min(abs(sm - so), DYN_DELTA_CAP_DB)
        used += 1
    return (total / used if used else 0.0), used


# --------------------------------------------------------------------------- #
# The measured partial series
# --------------------------------------------------------------------------- #
# How far apart two peaks may sit and still be the same mode. Wide, because the
# whole point is that a model's mode can be substantially mistuned and still be
# that mode; past this it is a different mode and the pair is charged as two
# absences instead, which is the larger penalty and the right one.
MODE_PAIR_CENTS = 250.0
MODE_CENTS_CAP = 200.0
MODE_DB_CAP = 24.0
#: What a mode present on one side and absent on the other costs. Deliberately
#: larger than the worst paired error: a missing partial and a spurious one are
#: both structural, and a term that priced them below a mistuning would let a
#: fit delete a mode it could not place.
MODE_UNMATCHED_DB = 18.0
#: How many cents of mistuning weigh as much as one dB of level error. A quarter
#: tone against 2 dB — mistuning is the more audible defect on anything with a
#: definite pitch, and this is where that judgement is written down.
MODE_CENTS_PER_DB = 25.0


def _pair_modes(m_hz, o_hz) -> list[tuple[int, int, float]]:
    """Match a model's modes to a reference's, nearest first.

    Greedy over the whole cost matrix rather than in frequency order, so one
    badly placed mode cannot cascade into every pair after it. Each mode is
    used at most once; what is left over on either side is an absence.
    """
    cand: list[tuple[float, int, int]] = []
    for i, mh in enumerate(m_hz):
        if not mh or mh <= 0.0:
            continue
        for j, oh in enumerate(o_hz):
            if not oh or oh <= 0.0:
                continue
            cents = abs(1200.0 * math.log2(mh / oh))
            if cents <= MODE_PAIR_CENTS:
                cand.append((cents, i, j))
    cand.sort()
    used_m: set[int] = set()
    used_o: set[int] = set()
    pairs: list[tuple[int, int, float]] = []
    for cents, i, j in cand:
        if i in used_m or j in used_o:
            continue
        used_m.add(i)
        used_o.add(j)
        pairs.append((i, j, cents))
    return pairs


def _modes_terms(
    model_rows: list[dict], oracle_rows_: list[dict], tally: CellCount | None = None
) -> tuple[float, int]:
    """How differently the two instruments place their partials, and on how many notes.

    The harmonic ladder searches for partial n at `n*f0*sqrt(1+B*n^2)`, which
    describes a stiff string and nothing else. A bar, a bell, a plate or a
    membrane puts its partials where no formula predicts, so on those voices
    every ladder bin above the fundamental reads the render's own noise floor —
    on BOTH sides, which means the harmonic term returns a confident number made
    entirely of the difference between two noise floors. Measured on a
    synthesised celesta: ten of twelve bins at the floor and `harm` = 15.09.
    That covers GM 8-14 and 47, 55, 112-118, plus every drum note with a
    definite pitch.
    """
    total = 0.0
    used = 0
    for m, o in zip(model_rows, oracle_rows_):
        o_hz, o_db = o.get("modal_hz") or [], o.get("modal_db") or []
        m_hz, m_db = m.get("modal_hz") or [], m.get("modal_db") or []
        if not o_hz:
            # The reference has no partials to place. Skipped rather than
            # scored, exactly as an absent oracle band is: it is a property of
            # the reference, and a model charged for it would be charged for
            # something it cannot fix.
            continue
        pairs = _pair_modes(m_hz, o_hz)
        cost = 0.0
        for i, j, cents in pairs:
            cost += min(cents, MODE_CENTS_CAP) / MODE_CENTS_PER_DB
            db = abs(m_db[i] - o_db[j]) if i < len(m_db) and j < len(o_db) else 0.0
            cost += min(db, MODE_DB_CAP)
            if tally is not None:
                capped = cents >= MODE_CENTS_CAP or db >= MODE_DB_CAP
                tally.clipped += capped
                tally.compared += not capped
        # A mode present on one side only. It is not a comparison — there was
        # nothing to compare it with — so it counts as a cap the same way an
        # absent value does in `_absent_or`.
        unmatched = (len(m_hz) - len(pairs)) + (len(o_hz) - len(pairs))
        cost += MODE_UNMATCHED_DB * unmatched
        if tally is not None:
            tally.absent += unmatched
        total += cost / len(o_hz)
        used += 1
    return (total / used if used else 0.0), used


# --------------------------------------------------------------------------- #
# Movement
# --------------------------------------------------------------------------- #
# Scales chosen so each part of the term arrives in roughly "one unit is one
# audible step": 5 cents of vibrato depth, 1 dB of tremolo or beat depth, and
# 5 cents of fundamental width. The rates are charged only where both sides
# actually have depth, since the rate of a modulation that is not there is
# whichever bin the noise floor peaked in.
MOD_VIB_CENTS_PER_UNIT = 5.0
MOD_WIDTH_CENTS_PER_UNIT = 5.0
MOD_DEPTH_CAP = 12.0
MOD_CENTS_CAP = 60.0
MOD_WIDTH_CAP = 60.0
MOD_RATE_CAP_HZ = 4.0
#: Below this much depth a modulation is not present, so its rate says nothing.
MOD_RATE_MIN_CENTS = 5.0
MOD_RATE_MIN_DB = 1.0


def _mod_terms(
    model_rows: list[dict], oracle_rows_: list[dict], tally: CellCount | None = None
) -> tuple[float, int]:
    """How differently the two voices move, and how many notes said so.

    A sampled reference is a recording of a player, so it carries vibrato,
    breath movement and — on anything with more than one string or more than one
    voice — a beat. A physical model renders a mathematically still note unless
    it is told otherwise, and every other term here reads that stillness as
    cleanliness: `tnr` charges the model only for being NOISIER, the ladder is a
    time average, and the multi-scale term ignores phase. So the harness could
    see the difference in every render and had no way to say it.

    An absent measurement on the MODEL side where the reference has one is
    charged the cap rather than skipped, for the reason `_absent_or` gives: a
    note too dead to track is the defect, not the absence of evidence about one.

    **A rate is skipped on the reference's depth alone and never on the model's.**
    Both were once required to clear the floor, which handed a model a discount
    for going still: the parts are summed, so dropping one is a straight
    subtraction. Against a reference at 20 cents and 2 dB, a model at 5.1 cents
    and 1.1 dB scored 10.88 and one at 4.9 and 0.9 — dimmer on both axes —
    scored 4.12, because falling under the floor dropped two rate parts worth
    7.0 between them. The term added to charge a model for being too still had a
    cliff in it rewarding exactly that, and a decaying note reads as still by
    construction, since `measure_modulation` detrends the level track.
    """
    total = 0.0
    used = 0
    for m, o in zip(model_rows, oracle_rows_):
        parts: list[float] = []
        scored = False
        for field, scale, cap in (
            ("vib_cents", MOD_VIB_CENTS_PER_UNIT, MOD_CENTS_CAP),
            ("trem_db", 1.0, MOD_DEPTH_CAP),
            ("beat_db", 1.0, MOD_DEPTH_CAP),
            ("f0_width_cents", MOD_WIDTH_CENTS_PER_UNIT, MOD_WIDTH_CAP),
        ):
            mv, ov = m.get(field), o.get(field)
            if ov is None:
                continue
            scored = True
            parts.append(_absent_or(mv, ov, cap, tally) / scale)
        for depth, rate, floor in (
            ("vib_cents", "vib_rate_hz", MOD_RATE_MIN_CENTS),
            ("trem_db", "trem_rate_hz", MOD_RATE_MIN_DB),
            ("beat_db", "beat_rate_hz", MOD_RATE_MIN_DB),
        ):
            md, od = m.get(depth), o.get(depth)
            mr, orr = m.get(rate), o.get(rate)
            if od is None or orr is None or od < floor:
                # The reference does not have this modulation, so the rate it
                # reports is whichever bin its noise floor peaked in and there
                # is nothing here to compare against.
                continue
            if md is None or mr is None or md < floor:
                # The reference moves and the model does not, so the model has
                # no rate to be right or wrong about. Charged the cap, exactly
                # as an absent value is above — never skipped, because the parts
                # are summed and a skip is a discount for the defect itself.
                parts.append(MOD_RATE_CAP_HZ)
                if tally is not None:
                    tally.absent += 1
                continue
            offset = abs(mr - orr)
            parts.append(min(offset, MOD_RATE_CAP_HZ))
            if tally is not None:
                tally.clipped += offset >= MOD_RATE_CAP_HZ
                tally.compared += offset < MOD_RATE_CAP_HZ
        if scored:
            total += sum(parts)
            used += 1
    return (total / used if used else 0.0), used


# A quarter tone of stretch difference at the twelfth partial is already far
# past anything a string does; past that the note is telling us the fit failed
# rather than that the model is wrong.
STIFF_DELTA_CENTS_CAP = 50.0


def _stiff_terms(model_rows: list[dict], oracle_rows_: list[dict]) -> tuple[float, int]:
    """How differently the two strings stretch their partials, and on how many notes.

    This exists because making the ladder correct removed the only thing that
    was pricing stiffness at all. Before the ladder tracked the partial series,
    a model stiffer than its reference showed up as tens of decibels of
    fabricated harmonic error, which is the wrong quantity in the wrong term but
    was at least a pressure in the right direction. With the ladder measured
    independently of the series, nothing charged for the series itself — so a
    voice whose strings are twice as stiff as the reference's could score a
    clean sheet. That is a real property of a real string and it belongs in a
    term of its own rather than as a contaminant of the timbre ones.

    Counted only where both sides fitted enough partials to mean it. Unlike the
    dynamics term, a zero here is a legitimate match rather than a missing
    measurement: two harmonic instruments genuinely agree that neither stretches.
    """
    total = 0.0
    used = 0
    for m, o in zip(model_rows, oracle_rows_):
        mb, ob = m.get("inharmonicity_b"), o.get("inharmonicity_b")
        if mb is None or ob is None:
            continue
        if (
            m.get("inharmonicity_partials", 0) < MIN_PARTIALS_FOR_B
            or o.get("inharmonicity_partials", 0) < MIN_PARTIALS_FOR_B
        ):
            continue
        delta = abs(stretch_cents(mb) - stretch_cents(ob))
        total += min(delta, STIFF_DELTA_CENTS_CAP)
        used += 1
    return (total / used if used else 0.0), used


def _rows_comparable(model_rows: list[dict], oracle_rows_: list[dict]) -> bool | None:
    """True when the rows line up, False when there are none, None when they clash.

    "None at all" is not the same failure as "a different number on each side".
    A pattern with no analysis notes (`scale`, `room-probe`) has nothing per-note
    to score and is legal as long as the only weighted term is the whole-timeline
    multi-scale one; a count mismatch means one render did not sound the probe,
    which is a broken evaluation and scores infinite.
    """
    if len(model_rows) != len(oracle_rows_):
        return None
    return bool(model_rows)


class CellCount:
    """What happened to each cell of a summed term — four outcomes, not two.

    A capped aggregate's raw value cannot tell them apart, and they mean
    opposite things:

    - **compared** — both sides had a value and the difference was inside the
      cap. The only one of the four that is a measurement.
    - **clipped** — both sides had a value and the difference reached the cap.
      A comparison, but its magnitude is the cap rather than the distance.
    - **absent** — the reference had a value and the model did not. No
      comparison happened; the cap stands in for one, which is right (a model
      that stopped sounding is the defect) and is not evidence about how far
      apart they are.
    - **skipped** — the reference had no value, so there was nothing to compare
      against and nothing is charged. Scores 0.0, which is the term's BEST.

    Both ends are invisible in the raw value and they fail in opposite
    directions. A term made of caps reads as its worst, which no empty-set
    guard looks for: two voices' `slope` came to 24 cells with not one
    comparison among them, and one kit's `modes` to 72. A term made of skips
    reads as its best: `tail` was 546 cells of 546 skipped across the bank's
    rendered probes, because its band opens at 2.0 s and the default probe
    holds 2.0 s.

    Reported as `<term>_cells` (what the reference offered), `<term>_capped`,
    `<term>_absent` and `<term>_skipped`, read exactly as `harm_bins` is.
    Nothing in the objective divides by any of them: the cap is still charged
    and the skip is still free.
    """

    __slots__ = ("absent", "clipped", "compared", "skipped")

    def __init__(self) -> None:
        self.compared = 0
        self.clipped = 0
        self.absent = 0
        self.skipped = 0

    @property
    def capped(self) -> int:
        """Cells whose value is the cap — a clip or a stand-in for an absence."""
        return self.clipped + self.absent

    def out(self, term: str) -> dict[str, float]:
        return {
            f"{term}_cells": float(self.compared + self.capped),
            f"{term}_capped": float(self.capped),
            f"{term}_absent": float(self.absent),
            f"{term}_skipped": float(self.skipped),
        }


def _absent_or(model, oracle, cap: float, tally: CellCount | None = None) -> float:
    """Compare one measured value with its reference, charging for an absence.

    A band the *model* has nothing in, where the oracle has something, is the
    model failing to sound — and skipping it, which is what comparing only the
    pairs that are both present does, scores that failure at zero. Silence the
    voice completely and every band goes that way at once, so the decay terms
    come out at exactly 0.0, their best value. It is the same shape as the
    harmonic ladder's floor guard and it survived that fix: a metric that skips
    unusable points and averages the rest scores an empty set as perfect.

    So an absent model value costs the cap. An absent *oracle* value is a
    different thing and still skipped: it means the reference has nothing there
    to match, which on a short probe is most of the aftersound band and is a
    property of the probe rather than of the voice — and it is not a cell of
    this term at all, so `tally` does not count it either way.
    """
    if oracle is None:
        if tally is not None:
            tally.skipped += 1
        return 0.0
    if model is None:
        if tally is not None:
            tally.absent += 1
        return cap
    delta = abs(model - oracle)
    if tally is not None:
        if delta >= cap:
            tally.clipped += 1
        else:
            tally.compared += 1
    return min(delta, cap)


def _fell_silent(model_rows: list[dict], oracle_rows_: list[dict]) -> bool:
    """Whether the model produced no partials anywhere while the reference did.

    The guard this replaces counted ladder bins above the -120 dB sentinel,
    which only ever marks a bin above Nyquist. A render that fell silent writes
    whatever its noise floor was — around -100 dB — into every bin, so every bin
    passed, h1 matched h1 by definition, and the harmonic term came out at its
    best possible value. Silencing a gain was the cheapest way to win it.

    `measure_modes` finds partials wherever they are rather than where a series
    predicts, so "did this render produce any partial at all" is a question it
    can answer for a bell as well as for a string, and answering it needs no
    floor constant.
    """
    oracle_modes = sum(len(o.get("modal_hz") or []) for o in oracle_rows_)
    if not oracle_modes:
        return False
    return sum(len(m.get("modal_hz") or []) for m in model_rows) == 0


def _attack_delta_ms(m: dict, o: dict) -> float:
    """Attack difference in ms, on the finest grid both sides carry.

    `attack_fine_ms` resolves 0.5 ms where `attack_ms` quantises to 5 and
    smears over a 10 ms window — measured on synthetic rises, 0.5 ms and 1 ms
    both report 5.0, and 2, 3 and 5 ms all report 10.0. That grid is kept on
    `attack_ms` because every committed profile in `reference/` was measured
    through it and cannot be re-measured without the plugin it came from, so
    the fine one is preferred where present and the coarse one is the fallback.
    A comparison that measures both sides live — `compare`, `autofit`, every
    oracle route — always has the fine one.
    """
    mf, of = m.get("attack_fine_ms"), o.get("attack_fine_ms")
    if mf is not None and of is not None:
        return abs(mf - of)
    return abs(m["attack_ms"] - o["attack_ms"])


# Per-band deltas are capped for the same reason the harmonic ones are: a band
# the oracle happens to have almost nothing in produces a huge dB difference
# from a model that has slightly less, and one such band would otherwise decide
# the whole objective.
BAND_DELTA_CAP_DB = 24.0

# Where the reference stops carrying information, and what the model is charged
# for having content there.
#
# `analyze_hit` floors a band at -60 dB under the hit's loudest one, and the
# comment on that floor says it keeps two noise floors from reading as a
# difference — which it does, when BOTH sides are at it. Measured across
# `reference/drums.json`, the 12.5 kHz band is at the floor on 98 % of rows and
# the 10 kHz band on 60 %: the captured kit is dark above 8 kHz, every
# instrument in it at the same rate, which four different objects cannot be.
#
# The floor did not protect anything there, because a model with a real cymbal
# wash is NOT at the floor. Its 12.5 kHz band sits perhaps -15 dB under its own
# peak, the reference reads -60, the delta is 45 dB and is charged at the 24 dB
# cap. `band` is a sum over 25 bands, so those two carried up to 48 units of a
# term whose whole typical value is around a hundred — a quarter to a third of
# the objective, permanently, reducible only by removing the model's high end.
#
# So a band the reference has floored is skipped, exactly as an absent oracle
# value is skipped everywhere else here: the capture cannot resolve it, and a
# charge levied on evidence that does not exist is fabricated whichever way it
# points. The count is reported (`band_bins`) so a narrowed comparison is never
# silent.
#
# What this does NOT reopen is the defect the model's radiated upper bound was
# added to fix — an open-topped noise wash peaking at 12.5 kHz where the
# reference peaks at 2.5 kHz. That is visible in `peak_band_hz` and in the
# 4-8 kHz bands, which are well inside the reference's range and still scored.
BAND_REFERENCE_FLOOR_DB = -59.0

# Decay rates are compared as a RATIO rather than as a difference in dB/s.
#
# A difference cannot be scaled: the same estimator returns -20 dB/s for a tom's
# shell and -800 dB/s for the stick click on top of it, both correctly, so a cap
# wide enough to express the second saturates on everything and one narrow
# enough for the first saturates on the second. Measured on the reference, the
# same instrument struck at six velocities spans a median of 43 to 249 dB/s per
# band — the reference's own strike-to-strike variation already exceeded the old
# 60 dB/s cap, which made `bdecay` a saturated constant with no gradient in it.
#
# A ratio has none of that. One unit is a factor of two — a band that dies twice
# as fast as the reference's — which means the same thing at every rate and is
# roughly what the ear grades a decay by.
BDECAY_OCTAVE_CAP = 3.0
#: Under this the band is not decaying and a ratio against it is arithmetic
#: rather than a measurement.
BDECAY_MIN_RATE_DB_S = 2.0

# The top of the region `lf` reads as one thing. 160 Hz is where a kit stops
# being the kick and the floor tom: above it the snare's body, the toms' heads
# and every stick click start arriving, and averaging those in is exactly the
# dilution this term exists to undo.
PERC_LF_MAX_HZ = 160.0


def _lf_balance_db(bands_db, valid) -> float | None:
    """How much bottom a hit has, in dB, relative to the rest of its spectrum.

    A balance rather than a level, and that is the whole design. Both sides'
    profiles are normalised to their own loudest band, so on a kick — where the
    loudest band IS the bottom — comparing the low bands directly compares each
    side's anchor against itself and can only ever return zero. Measured against
    the bands above the boundary instead, the anchor cancels and what is left is
    the quantity the ear actually grades: how far this drum's bottom stands out
    of its own body.
    """
    low = [bands_db[i] for i in valid if THIRD_OCTAVE_CENTERS[i] <= PERC_LF_MAX_HZ]
    rest = [bands_db[i] for i in valid if THIRD_OCTAVE_CENTERS[i] > PERC_LF_MAX_HZ]
    if not low or not rest:
        return None
    return sum(low) / len(low) - sum(rest) / len(rest)


def _perc_lf_terms(model_rows: list[dict], oracle_rows_: list[dict]) -> tuple[float, int]:
    """How far the model's low end sits from the reference's, as one region.

    Bands the reference floored are left out for the reason they are left out of
    `band`: see BAND_REFERENCE_FLOOR_DB. Both sides are read over the same set
    of bands, since a balance taken over two different sets is not a comparison.
    """
    total = 0.0
    scored = 0
    for m, o in zip(model_rows, oracle_rows_):
        mb, ob = m.get("bands_db") or [], o.get("bands_db") or []
        if len(mb) != len(THIRD_OCTAVE_CENTERS) or len(ob) != len(THIRD_OCTAVE_CENTERS):
            continue
        valid = [i for i in range(len(THIRD_OCTAVE_CENTERS)) if ob[i] > BAND_REFERENCE_FLOOR_DB]
        m_lf, o_lf = _lf_balance_db(mb, valid), _lf_balance_db(ob, valid)
        if m_lf is None or o_lf is None:
            continue
        total += min(abs(m_lf - o_lf), LF_DELTA_CAP_DB)
        scored += 1
    return (total / scored, scored) if scored else (0.0, 0)
