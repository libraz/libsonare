"""The number a fit minimises: from a rendered probe to a single scalar.

Three layers. `probe_rows` measures a render with whichever metric set the probe
pattern calls for — the harmonic one for a pitched voice, the percussion one for
a drum hit. `loss_terms` / `percussion_terms` reduce a model/oracle pair of those
to the named terms a weight can be put on. `LossWeights` combines them, dividing
each by a fixed perceptual unit so a weight means the same thing across terms
whose raw units differ by orders of magnitude, and scaling the total so the
start point reads exactly 1.0.

`skeleton_note` lives here rather than in `metrics` because it exists for the
fit: it separates the excitation spectrum from the loop decay, which is what
lets a staged fit score those two against different evidence.
"""

from __future__ import annotations

import math
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

import numpy as np

# The split modules below hold what this file used to define inline. Every
# importer reads this module by name and one of them patches attributes on it,
# so the whole surface is re-exported here.
# ruff: noqa: F401
from loss_dimensions import (
    _SHARED_TERMS,
    BAND_DELTA_CAP_DB,
    BAND_REFERENCE_FLOOR_DB,
    BDECAY_MIN_RATE_DB_S,
    BDECAY_OCTAVE_CAP,
    DYN_DELTA_CAP_DB,
    DYN_MIN_VELOCITY_SPREAD,
    DYN_VELOCITY_SPAN,
    HARM_REACH,
    HF_DELTA_CAP_DB,
    LEVEL_DELTA_CAP_DB,
    LF_DELTA_CAP_DB,
    LOSS_TERMS,
    MOD_CENTS_CAP,
    MOD_DEPTH_CAP,
    MOD_RATE_CAP_HZ,
    MOD_RATE_MIN_CENTS,
    MOD_RATE_MIN_DB,
    MOD_VIB_CENTS_PER_UNIT,
    MOD_WIDTH_CAP,
    MOD_WIDTH_CENTS_PER_UNIT,
    MODE_CENTS_CAP,
    MODE_CENTS_PER_DB,
    MODE_DB_CAP,
    MODE_PAIR_CENTS,
    MODE_UNMATCHED_DB,
    PERC_LF_MAX_HZ,
    PERCUSSION_TERMS,
    PITCHED_TERMS,
    STIFF_DELTA_CENTS_CAP,
    SUSTAIN_SLOPE_CAP_DB_S,
    TAIL_DELTA_CAP_DB_S,
    CellCount,
    _absent_or,
    _attack_delta_ms,
    _brightness,
    _dyn_terms,
    _fell_silent,
    _level_terms,
    _lf_balance_db,
    _mod_terms,
    _modes_terms,
    _pair_modes,
    _perc_lf_terms,
    _rows_comparable,
    _stiff_terms,
    measured_terms,
)
from loss_kit import (
    KIT_DB_PER_DOUBLING,
    KIT_DOUBLING_CAP,
    KIT_MIN_MEMBERS,
    KIT_MIN_SPREAD,
    KIT_RELATIONS,
    _kit_relation,
    _kit_scored,
    _kit_terms,
    _kit_value,
    kit_report,
)
from loss_mss import (
    MSS_FFT_SIZES,
    MSS_LOG_WEIGHTING,
    MSS_MIN_HZ,
    _log_bin_weights,
    _stft_mag,
    mss_distance,
)
from loss_weights import (
    TERM_COUNT_KEYS,
    TERM_UNITS,
    UNMEASURABLE_PENALTY,
    LossWeights,
    cli_weights,
    dropped_weights,
    refused_weights,
    unmeasurable_terms,
)
from metrics import (
    MIN_PARTIALS_FOR_B,
    THIRD_OCTAVE_CENTERS,
    _peak_near,
    _spectrum,
    analyze_hit,
    analyze_note,
    attack_bands,
    attack_low_bands,
    attack_peaks,
    audibility_weights,
    band_tilt_db,
    estimate_inharmonicity_b,
    ladder_present,
    level_of,
    midi_to_hz,
    note_onset,
    partial_hz,
    partial_offset,
    stretch_cents,
)
from patterns import analysis_window_end
from toneclass import default_weights

# Longest analysis window per note. The probe patterns hold a note for two
# seconds and this used to match them, which made the cap invisible — and made
# the harness structurally unable to see a decay that goes wrong later than
# that. A grand's aftersound runs 9 to 50 seconds from A0 to the top of the
# fitted range, and a model whose C6 fell to nothing in 1.6 s of an 8 s hold
# scored exactly the same as one that held for eleven, because both were
# measured over the same first two seconds. The cap now sits past the capture's
# own eight-second gate; a shorter note still measures only as far as it lasts.
SKELETON_MAX_S = 8.0

# Where the three decay bands are fitted, in seconds from the onset. The first
# two are the prompt sound and the start of the aftersound; the third only has
# frames to fit when the probe holds the note that long, and is what a
# two-second window could never reach.
SKELETON_BANDS = {"early_db_s": (0.08, 0.40), "late_db_s": (0.80, 1.80),
                  "tail_db_s": (2.00, 6.00)}

# The envelope grid every band is fitted on, and the fewest frames a fit is
# allowed to run on. Module constants rather than locals because the shortest
# note that reaches a band is derived from them below, and a hand-copied mirror
# of a number that is right here is how a gate comes to describe a window the
# code no longer uses.
SKELETON_FRAME_WIN_S = 0.05
SKELETON_FRAME_HOP_S = 0.01
SKELETON_MIN_FRAMES = 4


def band_min_note_s(band: str) -> float:
    """Shortest note that puts `SKELETON_MIN_FRAMES` frames inside `band`.

    A note under this reaches the band with nothing in it, and every cell of the
    term reading that band is then skipped — which scores exactly 0.0, the
    term's best value, from no comparison at all. `tail` is the one this
    matters for: its band opens at 2.0 s and the default `sustain` probe holds
    2.0 s, so it misses by 70 ms and the term has never once been measured on
    that probe.
    """
    start = SKELETON_BANDS[band][0]
    # A frame is timed at its own centre, so the first one inside the band sits
    # half a window before it rather than on it.
    first = math.ceil((start - SKELETON_FRAME_WIN_S / 2.0) / SKELETON_FRAME_HOP_S)
    return (first + SKELETON_MIN_FRAMES) * SKELETON_FRAME_HOP_S + SKELETON_FRAME_WIN_S


# How far under the note's loudest frame a band may sit and still be treated as
# a measurement rather than as the floor. Generous, since a high partial 70 dB
# down is still a partial; what it excludes is a band with nothing in it at all.
SKELETON_FLOOR_DB = 90.0

# The zoom grid a partial's frequency is refined over: 41 points spanning ±1.5%
# of the guess, or ±26 cents. Shared by both refiners so the value they return
# comes from the same set and only the way it is chosen can differ.
REFINE_SPAN = 0.015
REFINE_POINTS = 41


def _refine_grid(guess: float) -> np.ndarray:
    return np.linspace(
        guess * (1.0 - REFINE_SPAN), guess * (1.0 + REFINE_SPAN), REFINE_POINTS
    )


def _refine_partial_direct(ref_w: np.ndarray, t_ref: np.ndarray, guess: float) -> float:
    """Zoomed DFT over the candidate grid, evaluated one candidate at a time.

    The straightforward form, kept because it is the one that is obviously
    correct: `_refine_partial` is the same quantity computed cheaply, and this
    is what the test holds it to.
    """
    cand = _refine_grid(guess)
    amps = np.abs(np.exp(-2j * np.pi * np.outer(cand, t_ref)) @ ref_w)
    return float(cand[int(np.argmax(amps))])


def _refine_partial(ref_w: np.ndarray, t_ref: np.ndarray, guess: float) -> float:
    """The same zoomed DFT, stepped along the grid instead of rebuilt per point.

    The grid is equally spaced, so each candidate's phasor is the previous
    one's times a fixed step. The direct form evaluates `exp` at all 41xN
    points; this evaluates it twice and buys the other 39 candidates with a
    complex multiply each. Folding the windowed segment into the phasor up
    front makes each step a multiply and a sum rather than a multiply and a dot
    product.

    It is worth the second implementation because this is the hot spot of the
    whole harness, by a wide margin and not where it looks: on a three-note
    sustain probe the refiner was 86% of the time spent turning a render into
    rows, and that reduction was itself four times the C++ render it measures.

    The recurrence accumulates phase error the direct form does not, which is
    of no consequence where the answer is: over the 40 steps it reaches ~2e-13
    of the peak, and it grows only in the nulls between partials, where summing
    24000 cancelling terms is what limits both forms anyway. What the caller
    consumes is the argmax, and that sits on a peak.
    """
    cand = _refine_grid(guess)
    step = np.exp(-2j * np.pi * (cand[1] - cand[0]) * t_ref)
    y = ref_w * np.exp(-2j * np.pi * cand[0] * t_ref)
    amps = np.empty(cand.size)
    for k in range(cand.size):
        amps[k] = abs(y.sum())
        if k + 1 < cand.size:
            y *= step
    return float(cand[int(np.argmax(amps))])


def skeleton_note(mono: np.ndarray, sr: int, note, n_harm: int = HARM_REACH) -> dict:
    """Per-harmonic envelope skeleton of one note.

    Separates the two things the time-averaged spectrum conflates:
      - init_db: per-harmonic level extrapolated to the onset (dB rel h1) —
        the EXCITATION spectrum evidence;
      - early_db_s / late_db_s / tail_db_s: per-harmonic decay slopes over the
        three bands in `SKELETON_BANDS` — the LOOP decay evidence.
    Harmonics above Nyquist are None, and so is a band the note is too short to
    reach.
    """
    f0_nominal = 440.0 * 2.0 ** ((note.note - 69) / 12.0)
    start = int(note.start * sr)
    seg = np.asarray(
        mono[start : start + int(min(note.dur, SKELETON_MAX_S) * sr)], dtype=np.float64
    )
    win_n = int(SKELETON_FRAME_WIN_S * sr)
    hop = int(SKELETON_FRAME_HOP_S * sr)
    empty = {"init_db": [None] * n_harm, **{b: [None] * n_harm for b in SKELETON_BANDS}}
    if len(seg) < win_n + hop:
        return empty

    # Refine each partial's frequency with a zoomed DFT over the early sustain.
    #
    # The grid is ±1.5%, or ±26 cents — tighter than the ±40 cents the spectral
    # ladder searches, so a stiff string walks out of it sooner. Centring it on
    # an integer multiple therefore tracks a grand's bass only to about the
    # sixth partial and then follows whatever sits between the real ones, which
    # is the render's floor: the decay slope fitted to that is noise with a
    # direction, and the excitation level extrapolated from it is invented.
    # Centring it on the string's own partial series costs one pass over the
    # low partials and keeps the grid where it was for anything harmonic.
    ref = seg[int(0.05 * sr) : int(0.55 * sr)]
    ref_w = ref * np.hanning(len(ref))
    t_ref = np.arange(len(ref)) / sr

    def _refine(guess: float) -> float:
        return _refine_partial(ref_w, t_ref, guess)

    # Stiffness comes from the shared fit rather than from a second estimate
    # made here. Two estimators of one physical quantity drift, and this one
    # would drift against the ruler the ladder and the reference profile are
    # both read with.
    spec_f, spec_m = _spectrum(ref, sr)
    _, h1_mag = _peak_near(spec_f, spec_m, f0_nominal, 80.0)
    b, _ = estimate_inharmonicity_b(spec_f, spec_m, f0_nominal, h1_mag, sr)
    f0 = _refine(f0_nominal) if f0_nominal <= sr / 2 - 500.0 else f0_nominal

    freqs: list[float | None] = []
    for h in range(1, n_harm + 1):
        guess = partial_hz(f0, h, b)
        if guess > sr / 2 - 500.0:
            freqs.append(None)
            continue
        freqs.append(_refine(guess))

    n_frames = (len(seg) - win_n) // hop
    frames = np.lib.stride_tricks.sliding_window_view(seg, win_n)[:: hop][:n_frames]
    frames = frames * np.hanning(win_n)
    t_win = np.arange(win_n) / sr
    active = [f for f in freqs if f is not None]
    basis = np.exp(-2j * np.pi * np.outer(t_win, np.array(active)))
    env = np.abs(frames @ basis)  # (n_frames, n_active)
    env_db = 20.0 * np.log10(env + 1e-12)
    t_frame = (np.arange(n_frames) * hop + win_n / 2) / sr

    # Where a partial has stopped being a partial. A band whose frames all sit
    # this far under the loudest thing in the note is measuring the render's
    # floor, and a slope fitted to that is noise with a direction: the top two
    # octaves are gone well before the 2-6 s band on any real grand, so without
    # this the aftersound term compares one silence with another and reports a
    # confident number that no knob can move.
    floor_db = float(np.max(env_db)) - SKELETON_FLOOR_DB

    def fit(lo: float, hi: float, col: np.ndarray) -> tuple[float, float] | None:
        mask = (t_frame >= lo) & (t_frame <= hi)
        if mask.sum() < SKELETON_MIN_FRAMES or float(np.max(col[mask])) < floor_db:
            return None
        m, b = np.polyfit(t_frame[mask], col[mask], 1)
        return float(m), float(b)

    init_db: list[float | None] = []
    slopes: dict[str, list[float | None]] = {band: [] for band in SKELETON_BANDS}
    col_i = 0
    for f in freqs:
        if f is None:
            init_db.append(None)
            for band in SKELETON_BANDS:
                slopes[band].append(None)
            continue
        col = env_db[:, col_i]
        col_i += 1
        fits = {band: fit(lo, hi, col) for band, (lo, hi) in SKELETON_BANDS.items()}
        early = fits["early_db_s"]
        init_db.append(early[1] if early else None)
        for band, got in fits.items():
            slopes[band].append(got[0] if got else None)
    if init_db[0] is None:
        return empty
    ref_db = init_db[0]
    init_db = [None if v is None else v - ref_db for v in init_db]
    return {"init_db": init_db, **slopes}

# The spectral centroid is deliberately excluded from the loss: it depends on
# the probe note set (register weighting) and has been an unreliable, noisy
# signal in this harness. Match harmonic profile, intonation, and noise floor
# instead.


def probe_rows(mono: np.ndarray, pattern, sr: int, raw: np.ndarray | None = None,
               max_band_hz: float | None = None, threads: int = 1) -> list[dict]:
    """Measure every analysis note of `pattern` with the metric set it calls for.

    A drum hit and a bowed note are both "one note of the probe", but nothing
    downstream of this point can score them the same way: one has a harmonic
    ladder and an intonation error, the other has neither. The pattern decides,
    once, and the rows carry the shape the loss then reads.

    Both metric sets read the same window — up to the next onset, never past it
    (`analysis_window_end`), so no note's release is measured through the next
    note's attack.

    `raw` is the same render before RMS normalisation. Everything measured from
    `mono` is a shape, and shapes are what normalisation is for; the level
    fields are the one thing that cannot survive it, so they are measured from
    the untouched signal instead. Omitting `raw` leaves those fields off the
    rows, and the level terms then have nothing to score — which is what a
    caller that has only a normalised render should get.

    `max_band_hz` is the reference capture's measurable ceiling and applies to
    the percussion set only. It has to be the SAME value on both sides — see
    `analyze_hit` — so it is resolved once from the oracle and handed to every
    model render of the run rather than re-derived per render.

    `threads` measures that many notes at once. Every note reads its own window
    of a signal nothing here writes to, so the rows are the same rows in the
    same order whatever this is set to — the only thing it changes is the wall
    clock. It is worth having because the partial refinement `skeleton_note`
    runs is the harness's hot spot and numpy drops the interpreter lock for
    most of it. The caller decides the number: a render already running
    alongside its siblings wants 1, since the concurrency is being spent a
    level up.
    """
    if pattern.percussive:
        def measure(note) -> dict:
            return analyze_hit(mono, sr, note, analysis_window_end(pattern, note),
                               max_band_hz=max_band_hz).to_dict()
    else:
        def measure(note) -> dict:
            end = analysis_window_end(pattern, note)
            # Found once and shared, so nothing that reads the attack can
            # disagree about where the note began.
            onset = note_onset(mono, sr, note, end)
            row = analyze_note(mono, sr, note, end, onset=onset).to_dict()
            row["skeleton"] = skeleton_note(mono, sr, note)
            row["onset_ms"] = round((onset - note.start) * 1000.0, 2)
            row["attack_hf_db"] = attack_bands(mono, sr, note, onset)
            # The same window's other end. Kept as its own field rather than
            # appended to `attack_hf_db`, because the two are measured over
            # different spans — six 20 ms slices against one 50 ms window — and
            # a single field would invite a reader to treat them as one grid.
            row["attack_lf_db"] = attack_low_bands(mono, sr, note, onset)
            # Attribution for what the two above price. Not a term and not read
            # by `loss_terms` — see `attack_peaks` and `fixed_resonances`.
            row["attack_peaks"] = attack_peaks(mono, sr, note, onset)
            return row

    notes = list(pattern.analysis_notes)
    if threads > 1 and len(notes) > 1:
        with ThreadPoolExecutor(max_workers=min(threads, len(notes))) as pool:
            rows = list(pool.map(measure, notes))
    else:
        rows = [measure(note) for note in notes]
    if raw is not None:
        for row, note in zip(rows, pattern.analysis_notes):
            row.update(level_of(raw, sr, note, analysis_window_end(pattern, note)))
    return rows


# The two conditions a peak has to meet to be called a fixed resonance, and why
# neither one alone is enough.
#
# Measured on this probe against a sampled reference, taking them one at a time:
# recurrence alone reported fifteen resonances on the REFERENCE, which has none
# — with twenty peaks per note over eighteen kilohertz, two notes coinciding
# somewhere is not evidence of anything. Off-partial alone is per-note and so
# cannot be corroborated at all. Together they gave three on the model and zero
# on the reference, and the model's strongest was found on every note of the
# grid.
#
# How far from the nearest partial a peak has to sit, as a fraction of the gap
# between neighbouring partials — 0.5 being exactly midway. A driven partial can
# be pushed off its predicted place by the analysis window's resolution and by
# the stiffness fit's own error, so the criterion needs real margin; below about
# a third of the gap the two populations overlap.
RESONANCE_OFF_PARTIAL = 0.35
# How far a peak may move between notes and still be the same resonance. A mode
# is excited from a different phase by each strike and read through a 120 ms
# window, so a little wander is expected; this is narrow enough that two
# unrelated peaks landing inside it is rare.
RESONANCE_GROUP_HZ = 150.0
# A frequency has to appear on at least this many of the probe's notes. Two is
# the minimum that can corroborate anything: one note on its own cannot tell a
# resonance from a coincidence in that note's own spectrum.
RESONANCE_MIN_NOTES = 2
# What `recurrence_only=True` asks for instead of the off-partial test, as a
# fraction of the notes that contributed peaks at all.
#
# The off-partial test is what makes two-note corroboration mean anything: drop
# it at RESONANCE_MIN_NOTES and this reports fifteen resonances on a reference
# that has none, because with twenty peaks per note over eighteen kilohertz two
# notes coinciding somewhere is not evidence. The evidence has to come from
# somewhere else, and the only other thing a fixed pitch does is appear on
# EVERY note — not two of them. Requiring nearly all of the grid is a far
# stronger claim than either original condition and is what the mode is for.
RESONANCE_RECURRENCE_FRACTION = 0.8


def fixed_resonances(rows: list[dict], *,
                     recurrence_only: bool = False) -> list[dict]:
    """Attack peaks that recur across the probe's notes at a fixed frequency.

    A free resonance rung by the strike — an undamped filter, a fold-back, a
    body mode excited off-pitch — sits where the string has no partial and stays
    there when the pitch changes. Both halves of that are normally needed, and
    the second reads the relation BETWEEN notes, which is why this lives here
    with `_dyn_terms` rather than inside the per-note metrics.

    Returns one entry per surviving frequency, most prominent first, carrying
    the notes it was found on. A note whose stiffness fit was too thin to place
    its partials contributes nothing rather than contributing peaks judged
    against a guessed series — the same gate `stiff` uses, for the same reason.

    Deliberately not a loss term: `hf` already charges for the energy these
    carry, and a second term over the same evidence would let one ring outvote
    the rest of the attack. What this adds is which frequency.

    `recurrence_only` drops the off-partial test and asks for near-total
    recurrence instead. The blind spot it opens up is the one the default has:
    a resonance DRIVEN at a fixed pitch lands near a partial on some notes of a
    grid and is rejected there, so a ring that is genuinely present on every
    note can be filtered out on the notes that would have proved it. What the
    off-partial test buys — the corroboration that makes two notes enough — is
    replaced by requiring the frequency on nearly the whole grid; the count that
    coincided with a partial is reported as `on_partial_notes` rather than
    used as a filter, so a caller can still see which finding is which.
    """
    peaks: list[tuple[float, float, int, bool]] = []
    contributing: set[int] = set()
    for row in rows:
        note = row.get("note")
        if note is None:
            continue
        f0 = row.get("f0_hz") or 0.0
        if row.get("inharmonicity_partials", 0) < MIN_PARTIALS_FOR_B:
            continue
        b = row.get("inharmonicity_b") or 0.0
        contributing.add(note)
        for freq, prom in row.get("attack_peaks", []):
            off = partial_offset(float(freq), float(f0), float(b))
            clear = off is not None and off > RESONANCE_OFF_PARTIAL
            if clear or recurrence_only:
                peaks.append((float(freq), float(prom), note, not clear))
    if recurrence_only:
        need = max(RESONANCE_MIN_NOTES,
                   round(len(contributing) * RESONANCE_RECURRENCE_FRACTION))
    else:
        need = RESONANCE_MIN_NOTES
    peaks.sort()
    groups: list[list[tuple[float, float, int, bool]]] = []
    for entry in peaks:
        if groups and entry[0] - groups[-1][0][0] <= RESONANCE_GROUP_HZ:
            groups[-1].append(entry)
        else:
            groups.append([entry])
    out: list[dict] = []
    for group in groups:
        notes = sorted({n for _, _, n, _ in group})
        if len(notes) < need:
            continue
        # One note may contribute several bins of the same ring; the frequency
        # is the prominence-weighted centre so a broad shoulder does not drag it.
        weight = sum(p for _, p, _, _ in group)
        out.append({
            "hz": round(sum(f * p for f, p, _, _ in group) / weight, 1),
            "prominence_db": round(weight / len(group), 1),
            "notes": notes,
            # How many of those notes it landed on a partial for. Under the
            # default that is zero by construction; under `recurrence_only` it
            # is the note to read the finding with.
            "on_partial_notes": len({n for _, _, n, on in group if on}),
        })
    out.sort(key=lambda d: -d["prominence_db"])
    return out


def loss_terms(
    model_rows: list[dict], oracle_rows_: list[dict], *,
    n_harm: int = HARM_REACH, mss: float = 0.0, audibility: bool = True,
) -> dict[str, float] | None:
    """Per-term mean mismatch between the model and the oracle, unweighted.

    Per note, from the same fields report.json carries:
      - `harm`: L1 distance of h1-normalized harmonics_db over the first
        `n_harm` harmonics (the most directly actionable timbre signal);
      - `cents`: absolute f0 cents difference from the oracle;
      - `tnr`: noise-floor shortfall, counted only when the model is noisier
        than the oracle (a model cleaner than the oracle is not penalised, since
        the sampled oracle carries natural vibrato/breath noise);
      - `env`: sustain-slope (dB/s), release (per 100 ms) and attack (per 10 ms)
        differences — the double-decay / ring-down signature a purely spectral
        match is blind to;
      - `init` / `slope`: per-harmonic ONSET ladder and decay slopes from
        `skeleton_note`, which separate the excitation spectrum from the loop
        decay (the time-averaged harmonic term conflates them). Per-bin deltas
        are capped (12 dB / 30 dB/s) so single-sample oracle quirks cannot
        dominate the objective.
    The spectral centroid is intentionally not part of the loss.

    `mss` is carried through unchanged: it is already a whole-timeline measure
    (see `mss_distance`), so it is not averaged over notes.

    Returns None when the two renders do not line up, which the caller reports
    as an infinite loss.
    """
    comparable = _rows_comparable(model_rows, oracle_rows_)
    if comparable is None:
        return None
    if not comparable:
        # Flagged, not just zeroed. A dict of zeros is the best score there is,
        # and "there was nothing to compare" reaching a caller as a perfect
        # match is how a render that fell silent gets read as the render that
        # fixed everything.
        return {**{name: 0.0 for name in LOSS_TERMS}, "mss": mss, "comparable": 0.0}
    totals = {name: 0.0 for name in LOSS_TERMS}
    # How many partials above the fundamental were present on both sides
    # anywhere in the probe. Zero is not a good score, it is no measurement: a
    # render that fell silent reports every harmonic at the -120 dB floor and
    # h1 at 0 by definition, so the guard below skips all of them, h1 matches h1
    # exactly, and the harmonic term comes out at 0.0 — the best value it has.
    # Left unguarded, silencing any gain is the cheapest way to win this term.
    compared = available = 0
    # How many notes the one-sided noise term actually charged for. `tnr` counts
    # only where the model is NOISIER, so its zero has two meanings that read
    # identically in a fit log: every note matched, or the model is cleaner than
    # the reference everywhere and the term has run out of anything to say.
    # Those are opposite findings — on a sampled oracle a model measurably
    # cleaner than the reference is usually one missing the reference's own
    # movement — and on a normalised objective the second is worth a full unit
    # of loss to whichever candidate reaches it first. Reported so a reader can
    # tell those apart, and deliberately NOT in `TERM_COUNT_KEYS`: unlike
    # `stiff_notes` and `dyn_groups`, this one moves with the candidate, so
    # `_went_unmeasurable` would charge a model for getting clean rather than
    # for going unmeasurable. Starting at 0 is not unusual — a physical model is
    # cleaner than a sampled recording by default — and on those voices the term
    # says nothing about any candidate. `loss_cells.py` counts which ones.
    tnr_notes = 0
    # How many cells of each capped aggregate were a comparison rather than a
    # cap standing in for one. See `CellCount`: the raw value of a term whose
    # cells all hit the cap is its worst, not its best, so none of the empty-set
    # guards above can see it and a reader cannot tell it from a real distance.
    cells = {t: CellCount() for t in ("init", "slope", "tail", "hf", "lf",
                                     "mod", "modes")}
    for m, o in zip(model_rows, oracle_rows_):
        pairs = list(zip(m["harmonics_db"][:n_harm], o["harmonics_db"][:n_harm]))
        available += max(0, len(pairs) - 1)
        # Which bins hold a partial rather than the note's own noise floor, read
        # against each side's loudest bin. The -120 dB sentinel this replaces
        # marks a bin above Nyquist and nothing else, so a bin that found
        # nothing passed it carrying whatever the floor happened to be — which
        # is how a voice with no harmonic series at all scored a confident
        # harmonic error made of two noise floors.
        present_m = ladder_present(m["harmonics_db"])[:n_harm]
        present_o = ladder_present(o["harmonics_db"])[:n_harm]
        # A partial's vote, weighted by whether anyone can hear it. Equal
        # weighting gives A0's 27.5 Hz fundamental the same say as its eighth
        # partial 30 dB above it in loudness, and charges an error on a partial
        # 50 dB under the loudest one in the same note in full.
        # A row that names neither a measured f0 nor a pitch has no frequency to
        # weight by, so it is weighted by level alone rather than by a guess.
        f0_hz = m.get("f0_hz") or (midi_to_hz(m["note"]) if m.get("note") is not None
                                   else 0.0)
        b = m.get("inharmonicity_b") or 0.0
        weights = (audibility_weights(
            [partial_hz(f0_hz, k + 1, b) if f0_hz > 0.0 else None
             for k in range(len(pairs))],
            [max(mh, oh) for mh, oh in pairs])
            if audibility else np.ones(len(pairs)))
        for i, (mh, oh) in enumerate(pairs):
            if mh <= -120.0 or oh <= -120.0:
                continue
            if not (present_m[i] or present_o[i]):
                # Neither side has a partial here. Charging the difference
                # between two noise floors is what this skip exists to stop; a
                # bin where only ONE side has a partial is a real difference and
                # is still charged.
                continue
            totals["harm"] += float(weights[i]) * abs(mh - oh)
            compared += i > 0 and present_m[i] and present_o[i]
        totals["cents"] += abs(m["f0_cents_err"] - o["f0_cents_err"])
        shortfall = max(0.0, o["tnr_db"] - m["tnr_db"])  # only when the model is noisier
        totals["tnr"] += shortfall
        tnr_notes += shortfall > 0.0
        totals["env"] += _absent_or(m["sustain_slope_db_s"], o["sustain_slope_db_s"],
                                    SUSTAIN_SLOPE_CAP_DB_S)
        totals["env"] += abs(m["release_ms"] - o["release_ms"]) / 100.0
        totals["env"] += _attack_delta_ms(m, o) / 10.0
        if "skeleton" in m and "skeleton" in o:
            sm, so = m["skeleton"], o["skeleton"]
            for a, b in zip(sm["init_db"], so["init_db"]):
                totals["init"] += _absent_or(a, b, 12.0, cells["init"])
            for key in ("early_db_s", "late_db_s"):
                for a, b in zip(sm[key][:6], so[key][:6]):
                    totals["slope"] += _absent_or(a, b, 30.0, cells["slope"]) / 10.0
            for a, b in zip(sm.get("tail_db_s", [])[:6], so.get("tail_db_s", [])[:6]):
                totals["tail"] += _absent_or(a, b, TAIL_DELTA_CAP_DB_S,
                                             cells["tail"]) / 10.0
        for a, b in zip(m.get("attack_hf_db", []), o.get("attack_hf_db", [])):
            totals["hf"] += _absent_or(a, b, HF_DELTA_CAP_DB, cells["hf"])
        # Averaged over the bands the reference offered, not summed over them,
        # because the percussion branch of this same term is a mean of one
        # number per hit: summed, one name would carry two aggregates five
        # times apart and no single unit could scale both. The denominator is
        # the ORACLE's band count, so a candidate cannot shrink it.
        lf_parts = [_absent_or(a, b, LF_DELTA_CAP_DB, cells["lf"])
                    for a, b in zip(m.get("attack_lf_db", []),
                                    o.get("attack_lf_db", [])) if b is not None]
        if lf_parts:
            totals["lf"] += sum(lf_parts) / len(lf_parts)
    if _fell_silent(model_rows, oracle_rows_):
        # The reference has partials and the model produced none anywhere. That
        # is a render that stopped sounding, and every normalised term scores
        # silence as a flat spectrum rather than as an absence.
        return None
    if available and not compared and not any(m.get("modal_hz") for m in model_rows):
        # The pre-modal form of the same guard, for rows that carry no measured
        # partial list: nothing above the fundamental survived on both sides
        # anywhere in the probe. Where `modal_hz` IS present this is no longer a
        # silence test — a bell legitimately has an empty harmonic ladder — and
        # `harm_bins` below is what a caller reads instead.
        return None
    n = len(model_rows)
    out = {name: totals[name] / n for name in LOSS_TERMS}
    out["mss"] = mss
    out["mod"], mod_notes = _mod_terms(model_rows, oracle_rows_, cells["mod"])
    out["modes"], modes_notes = _modes_terms(model_rows, oracle_rows_, cells["modes"])
    out["level"], out["crest"], offset = _level_terms(model_rows, oracle_rows_)
    # Reported alongside the value because a probe with no velocity axis can
    # only score zero here, and zero is this term's best possible value.
    out["dyn"], dyn_groups = _dyn_terms(model_rows, oracle_rows_)
    out["stiff"], stiff_notes = _stiff_terms(model_rows, oracle_rows_)
    if any(not math.isfinite(v) for v in out.values()):
        return None
    out["dyn_groups"] = float(dyn_groups)
    out["stiff_notes"] = float(stiff_notes)
    out["tnr_notes"] = float(tnr_notes)
    out["mod_notes"] = float(mod_notes)
    out["modes_notes"] = float(modes_notes)
    # How many ladder bins above the fundamental held a partial on both sides.
    # Zero means this voice has no harmonic series where the ladder looks — a
    # bar, a bell, a membrane — and `harm` is then measuring nothing, in which
    # case its zero is not a match. Read it exactly as `dyn_groups` is read.
    out["harm_bins"] = float(compared)
    for term, tally in cells.items():
        out.update(tally.out(term))
    out["level_offset_db"] = offset
    out["comparable"] = 1.0
    return out


def percussion_terms(
    model_rows: list[dict], oracle_rows_: list[dict], *, mss: float = 0.0,
    groups: dict[str, list[int]] | None = None,
) -> dict[str, float] | None:
    """Per-term mismatch for a drum probe, from the percussion metric set.

    Two terms carry the timbre, mirroring what `harm` and `slope` carry for a
    pitched voice:
      - `band`: L1 distance of the 1/3-octave level profile, each side
        normalised to its own loudest band. This is the percussion analogue of
        the h1-normalized harmonic ladder — level-blind, so it measures the
        shape of the spectrum rather than how loud the hit was;
      - `bdecay`: L1 distance of the per-octave-band decay slopes. A snare and a
        rimshot can have nearly the same spectrum at the onset and be told apart
        entirely by how fast the top of it dies.
    `env` carries the gestural difference — time to peak, time to fall 20 dB,
    and crest factor — each divided by roughly the amount that is audible, so
    the three are comparable before the term weights see them.

    `mss` is carried through unchanged, as in `loss_terms`.
    """
    comparable = _rows_comparable(model_rows, oracle_rows_)
    if comparable is None:
        return None
    if not comparable:
        # Flagged, not just zeroed. A dict of zeros is the best score there is,
        # and "there was nothing to compare" reaching a caller as a perfect
        # match is how a render that fell silent gets read as the render that
        # fixed everything.
        return {**{name: 0.0 for name in LOSS_TERMS}, "mss": mss, "comparable": 0.0}
    totals = {name: 0.0 for name in LOSS_TERMS}
    band_bins = bdecay_bins = tilt_hits = bright_hits = 0
    # See `CellCount` and the pitched reducer: how much of each capped aggregate
    # is a comparison rather than a cap standing in for one.
    cells = {t: CellCount() for t in ("band", "bdecay", "modes")}
    for m, o in zip(model_rows, oracle_rows_):
        tilt_m, tilt_o = band_tilt_db(m.get("bands_db")), band_tilt_db(o.get("bands_db"))
        if tilt_m is not None and tilt_o is not None:
            totals["tilt"] += abs(tilt_m - tilt_o)
            tilt_hits += 1
        if o.get("centroid_hz") and m.get("centroid_hz"):
            totals["bright"] += abs(100.0 * (m["centroid_hz"] / o["centroid_hz"] - 1.0))
            bright_hits += 1
        for a, b in zip(m["bands_db"], o["bands_db"]):
            if b <= BAND_REFERENCE_FLOOR_DB:
                # The reference has floored this band. See
                # BAND_REFERENCE_FLOOR_DB — the capture cannot resolve it, so
                # neither can any charge levied here.
                cells["band"].skipped += 1
                continue
            delta = abs(a - b)
            totals["band"] += min(delta, BAND_DELTA_CAP_DB)
            band_bins += 1
            cells["band"].clipped += delta >= BAND_DELTA_CAP_DB
            cells["band"].compared += delta < BAND_DELTA_CAP_DB
        for a, b in zip(m["band_decay_db_s"], o["band_decay_db_s"]):
            if b is None or abs(b) < BDECAY_MIN_RATE_DB_S:
                cells["bdecay"].skipped += 1
                continue
            bdecay_bins += 1
            if a is None or abs(a) < BDECAY_MIN_RATE_DB_S:
                # Charged its worst rather than skipped. This sum is divided by
                # the row count and not by the bins that survived it, so a skip
                # here is a straight discount and a shorter hit buys one: taking
                # the kick's `amp_env.decay_ms` to its floor leaves 12 of its 48
                # bands with a measurable rate, and the term reads as improved
                # while the hit is four times too short. The reference measured
                # this band, so a model giving it no rate at all disagrees by at
                # least the cap.
                totals["bdecay"] += BDECAY_OCTAVE_CAP
                cells["bdecay"].absent += 1
                continue
            octaves = abs(math.log2(abs(a) / abs(b)))
            totals["bdecay"] += min(octaves, BDECAY_OCTAVE_CAP)
            cells["bdecay"].clipped += octaves >= BDECAY_OCTAVE_CAP
            cells["bdecay"].compared += octaves < BDECAY_OCTAVE_CAP
        totals["env"] += abs(m["attack_ms"] - o["attack_ms"]) / 5.0
        totals["env"] += abs(m["decay_ms"] - o["decay_ms"]) / 100.0
        totals["env"] += abs(m["crest_db"] - o["crest_db"]) / 3.0
    n = len(model_rows)
    out = {name: totals[name] / n for name in LOSS_TERMS}
    out["mss"] = mss
    out["level"], out["crest"], offset = _level_terms(model_rows, oracle_rows_)
    # Reported alongside the value because a probe with no velocity axis can
    # only score zero here, and zero is this term's best possible value.
    out["dyn"], dyn_groups = _dyn_terms(model_rows, oracle_rows_)
    # The pitch of everything in a kit that has one — see `_modes_terms`. Empty
    # for a cymbal or a shaker, whose rows carry no modes, so the term costs
    # those nothing and prices a mistuned tom.
    out["modes"], modes_notes = _modes_terms(model_rows, oracle_rows_, cells["modes"])
    # The low end as one region rather than as six of twenty-five bands — see
    # `_perc_lf_terms`. Scored on the same profiles `band` reads, so it is a
    # re-weighting of evidence already in hand and not a second measurement.
    out["lf"], lf_notes = _perc_lf_terms(model_rows, oracle_rows_)
    # The relations between the kit's own members — see `_kit_terms`. Empty
    # without a capture that declares its families, which is what a probe of one
    # drum note gets and is the right answer there.
    out["kit"], kit_notes = _kit_terms(model_rows, oracle_rows_, groups)
    if any(not math.isfinite(v) for v in out.values()):
        return None
    out["dyn_groups"] = float(dyn_groups)
    out["modes_notes"] = float(modes_notes)
    out["lf_notes"] = float(lf_notes)
    out["kit_notes"] = float(kit_notes)
    # How many band cells the comparison actually charged for: with the
    # reference's floored bands skipped, a low count means the capture is
    # narrower than the analysis range and the term is speaking for fewer bands
    # than it looks like. This one counts what the REFERENCE offered, so it does
    # not move with the candidate and `_went_unmeasurable` cannot fire on it —
    # it is kept in `TERM_COUNT_KEYS` anyway so that a skip reintroduced on the
    # model side is caught rather than silently unguarded. `tnr_notes` is the
    # opposite case and is out of that table on purpose; see its own comment.
    out["band_bins"] = float(band_bins)
    out["bdecay_bins"] = float(bdecay_bins)
    # Both skip when either side has no profile or no centroid to read, which a
    # candidate can cause by rendering silence, so both are counted and guarded.
    out["tilt_hits"] = float(tilt_hits)
    out["bright_hits"] = float(bright_hits)
    for term, tally in cells.items():
        out.update(tally.out(term))
    out["level_offset_db"] = offset
    out["comparable"] = 1.0
    return out


def score_terms(
    model_rows: list[dict], oracle_rows_: list[dict],
    *, n_harm: int = HARM_REACH, mss: float = 0.0, percussive: bool = False,
    audibility: bool = True, groups: dict[str, list[int]] | None = None,
) -> dict[str, float] | None:
    """Reduce a rendered probe to raw loss terms, by the metric set it carries."""
    if percussive:
        return percussion_terms(model_rows, oracle_rows_, mss=mss, groups=groups)
    return loss_terms(model_rows, oracle_rows_, n_harm=n_harm, mss=mss,
                      audibility=audibility)
