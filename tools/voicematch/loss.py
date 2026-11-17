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

import json
import math
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import numpy as np

# The split modules below hold what this file used to define inline. Every
# importer reads this module by name and one of them patches attributes on it,
# so the whole surface is re-exported here.
# ruff: noqa: F401
from loss_dimensions import (
    _SHARED_TERMS,
    ARRIVAL_ABSENT_CAP,
    BAND_DELTA_CAP_DB,
    BAND_REFERENCE_FLOOR_DB,
    BDECAY_MIN_RATE_DB_S,
    BDECAY_OCTAVE_CAP,
    BODY_DELTA_CAP_DB,
    BRIGHT_ABSENT_CAP,
    CENTS_ABSENT_CAP,
    CREST_FIELDS,
    DYN_DELTA_CAP_DB,
    DYN_MIN_VELOCITY_SPREAD,
    DYN_VELOCITY_SPAN,
    HARM_REACH,
    HF_DELTA_CAP_DB,
    LEVEL_DELTA_CAP_DB,
    LEVEL_FIELDS,
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
    MODAL_UPPER_RATIO,
    MODE_CENTS_CAP,
    MODE_CENTS_PER_DB,
    MODE_DB_CAP,
    MODE_PAIR_CENTS,
    MODE_UNMATCHED_DB,
    PERC_LF_MAX_HZ,
    PERCUSSION_TERMS,
    PITCHED_TERMS,
    RISE_ABSENT_CAP_DB,
    STIFF_DELTA_CENTS_CAP,
    STRIKE_ABSENT_CAP_DB,
    SUSTAIN_SLOPE_CAP_DB_S,
    TAIL_DELTA_CAP_DB_S,
    TILT_ABSENT_CAP_DB,
    TNR_ABSENT_CAP_DB,
    TONAL_ABSENT_CAP_DB,
    CellCount,
    _absent_or,
    _attack_delta_ms,
    _attack_hf_share,
    _brightness,
    _delta_or_cap,
    _dyn_terms,
    _fell_silent,
    _finite_or_none,
    _level_terms,
    _lf_balance_db,
    _mod_terms,
    _modal_brightness,
    _modes_terms,
    _onset_terms,
    _onsetvel_terms,
    _pair_modes,
    _perc_lf_terms,
    _rows_comparable,
    _stiff_terms,
    _swing_terms,
    _velcurve_terms,
    _velspan_terms,
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
    body_below_f0_db,
    estimate_inharmonicity_b,
    ladder_present,
    level_of,
    midi_to_hz,
    note_onset,
    onset_shape,
    partial_hz,
    partial_offset,
    stretch_cents,
    to_mono,
)
from metrics_hit import ring_doublings
from metrics_skeleton import (
    SKELETON_FRAME_HOP_S as _SKELETON_FRAME_HOP_S,
)
from metrics_skeleton import (
    SKELETON_FRAME_MIN_S as _SKELETON_FRAME_MIN_S,
)
from metrics_skeleton import (
    partial_envelopes,
    sample_count,
    skeleton_frame_s,
    skeleton_frame_samples,
)
from metrics_texture import (
    DENSITY_CLIP,
    EVOLUTION_CLIP_DB,
    PROMPT_CLIP,
    WINDOW_FLATNESS_CLIP_DB,
)
from patterns import analysis_window_end
from profile_measure import (
    attack_hf_share_of,
    band_shape_error_db,
    fit_window_readings,
    held_peak_dbfs,
    held_readable,
    peak_dbfs,
)
from toneclass import default_weights, tone_class

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
SKELETON_BANDS = {"early_db_s": (0.08, 0.40), "late_db_s": (0.80, 1.80), "tail_db_s": (2.00, 6.00)}

# The envelope grid every band is fitted on (the shortest window; a low note's
# is longer, see `skeleton_frame_s`), and the fewest frames a fit is
# allowed to run on. Module constants rather than locals because the shortest
# note that reaches a band is derived from them below, and a hand-copied mirror
# of a number that is right here is how a gate comes to describe a window the
# code no longer uses.
SKELETON_FRAME_WIN_S = _SKELETON_FRAME_MIN_S
SKELETON_FRAME_HOP_S = _SKELETON_FRAME_HOP_S
SKELETON_MIN_FRAMES = 4


def band_min_note_samples(band: str, *, frame_s: float, sample_rate: int) -> int:
    """Minimum integer note length whose exclusive frame grid reaches a band."""
    win_n, hop = skeleton_frame_samples(frame_s, sample_rate)
    start_n = sample_count(SKELETON_BANDS[band][0], sample_rate)
    first = max(0, math.ceil((start_n - (win_n - 1) / 2.0) / hop))
    return win_n + (first + SKELETON_MIN_FRAMES) * hop


def band_min_note_s(
    band: str,
    *,
    frame_s: float = SKELETON_FRAME_WIN_S,
    sample_rate: int | None = None,
) -> float:
    """Shortest note that puts `SKELETON_MIN_FRAMES` frames inside `band`.

    A note under this reaches the band with nothing in it, and every cell of the
    term reading that band is then skipped — which scores exactly 0.0, the
    term's best value, from no comparison at all. `tail` is the one this
    matters for: its band opens at 2.0 s and the default `sustain` probe holds
    2.0 s, so it misses by 70 ms and the term has never once been measured on
    that probe.
    """
    if sample_rate is not None:
        return band_min_note_samples(band, frame_s=frame_s, sample_rate=sample_rate) / sample_rate
    start = SKELETON_BANDS[band][0]
    # A frame is timed at its own centre, so the first one inside the band sits
    # half a window before it rather than on it.
    first = math.ceil((start - frame_s / 2.0) / SKELETON_FRAME_HOP_S)
    return (first + SKELETON_MIN_FRAMES) * SKELETON_FRAME_HOP_S + frame_s


def band_min_note_for(
    band: str, note, *, percussive: bool = False, sample_rate: int | None = None
) -> float:
    """Minimum duration for a note's frame grid to reach one decay band."""
    frame_s = band_frame_s_for_note(note, percussive=percussive)
    return band_min_note_s(band, frame_s=frame_s, sample_rate=sample_rate)


def band_frame_s_for_note(note, *, percussive: bool = False) -> float:
    """Frame duration used by the skeleton for this pitched note or hit."""
    return SKELETON_FRAME_WIN_S if percussive else skeleton_frame_s(midi_to_hz(note.note))


def band_min_note_samples_for(
    band: str, note, sample_rate: int, *, percussive: bool = False
) -> int:
    """Reachability threshold in the renderer's integer sample domain."""
    return band_min_note_samples(
        band,
        frame_s=band_frame_s_for_note(note, percussive=percussive),
        sample_rate=sample_rate,
    )


# Maximum usable decay range below the note's loudest partial. This keeps
# missing partials and noise-only bins from becoming measurements.
SKELETON_FLOOR_DB = 90.0
# Maximum usable decay range below a partial's own peak. This stops a real
# partial's decay at the recording floor, before that floor can set its slope.
SKELETON_PARTIAL_FLOOR_DB = 60.0

# The zoom grid a partial's frequency is refined over: 41 points spanning ±1.5%
# of the guess, or ±26 cents. Shared by both refiners so the value they return
# comes from the same set and only the way it is chosen can differ.
REFINE_SPAN = 0.015
REFINE_POINTS = 41


def _refine_grid(guess: float) -> np.ndarray:
    return np.linspace(guess * (1.0 - REFINE_SPAN), guess * (1.0 + REFINE_SPAN), REFINE_POINTS)


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
    start = sample_count(note.start, sr)
    seg = np.asarray(
        mono[start : start + sample_count(min(note.dur, SKELETON_MAX_S), sr)], dtype=np.float64
    )
    frame_s = skeleton_frame_s(f0_nominal)
    win_n, hop = skeleton_frame_samples(frame_s, sr)
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

    active = [f for f in freqs if f is not None]
    t_frame, env = partial_envelopes(seg, sr, active, frame_s=frame_s)
    if env.size == 0:
        return empty
    env_db = 20.0 * np.log10(env + 1e-12)

    # Per partial, the higher of the note-wide floor and the partial's own floor.
    note_floor_db = float(np.max(env_db)) - SKELETON_FLOOR_DB
    partial_floor_db = np.max(env_db, axis=0) - SKELETON_PARTIAL_FLOOR_DB
    floor_db = np.maximum(note_floor_db, partial_floor_db)

    def fit(lo: float, hi: float, col: np.ndarray, floor: float) -> tuple[float, float] | None:
        mask = (t_frame >= lo) & (t_frame <= hi)
        in_band = np.flatnonzero(mask)
        if in_band.size < SKELETON_MIN_FRAMES:
            return None
        # Fit only the first contiguous run above the floor, so the floor never sets the slope.
        valid = in_band[col[in_band] > floor]
        if valid.size < SKELETON_MIN_FRAMES:
            return None
        run_end = (
            int(np.flatnonzero(np.diff(valid) != 1)[0]) + 1
            if np.any(np.diff(valid) != 1)
            else len(valid)
        )
        valid = valid[:run_end]
        if valid.size < SKELETON_MIN_FRAMES:
            return None
        m, b = np.polyfit(t_frame[valid], col[valid], 1)
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
        col_floor = float(floor_db[col_i])
        col_i += 1
        fits = {band: fit(lo, hi, col, col_floor) for band, (lo, hi) in SKELETON_BANDS.items()}
        early = fits["early_db_s"]
        init_db.append(early[1] if early else None)
        for band, got in fits.items():
            slopes[band].append(got[0] if got else None)
    if init_db[0] is None:
        return empty
    ref_db = init_db[0]
    init_db = [None if v is None else v - ref_db for v in init_db]
    return {"init_db": init_db, **slopes}


#: The `fit_window_readings` keys a pitched row carries, under the fields the loss reads.
FIT_WINDOW_FIELDS = {
    "cents_vs_et": "cents_vs_et",
    "centroid_hz": "fit_centroid_hz",
    "tnr_db": "fit_tnr_db",
    "tnr_mask": "fit_tnr_mask",
    "body_below_f0_db": "body_below_f0_db",
}
#: The key an oracle mask carries its row's `fit_tnr_mask` under, for `probe_rows` to split off.
FIT_TNR_MASK_KEY = "fit"
#: The key it carries the seconds its row was read to (`read_to_join_s`) under, likewise.
READ_END_KEY = "read_to_join_s"

#: Per own term (`GATE_OWN_TERMS`), the gate's function its per-note reading comes from.
OWN_TERM_READERS = {
    "cents": fit_window_readings,
    "tnr": fit_window_readings,
    "bright": fit_window_readings,
    "velspan": peak_dbfs,
    "level": held_peak_dbfs,
    "onset": onset_shape,
    "hfshare": attack_hf_share_of,
    "hfdyn": attack_hf_share_of,
    "body": body_below_f0_db,
}
#: The same for `PERCUSSION_GATE_OWN_TERMS`: `measure_hit` reads a hit through `analyze_hit`.
PERCUSSION_OWN_TERM_READERS = {
    "velspan": peak_dbfs,
    "crest": analyze_hit,
    "bandrms": band_shape_error_db,
    "bright": analyze_hit,
    "tonal": analyze_hit,
    "rise": analyze_hit,
}


def oracle_tnr_masks(oracle_rows: list[dict]) -> list[dict] | None:
    """The oracle rows' `tnr_mask`s, for `probe_rows` to read the model through.

    A mask carries the rest of how its row was read where the row says so (`modal`,
    `stationary_lines_hz`; see `analyze_note`), so the model is read as its reference was, the
    row's fit-window mask under `FIT_TNR_MASK_KEY`, which `tnr` reads both sides through, and
    the seconds a looped row was read to under `READ_END_KEY`.
    None for a percussive probe, whose rows carry no tone-to-noise ratio.
    """
    masks = [row.get("tnr_mask") for row in oracle_rows]
    if any(m is None for m in masks):
        return None
    carried = {FIT_TNR_MASK_KEY: "fit_tnr_mask", READ_END_KEY: "read_to_join_s"}
    return [
        {**m, **{key: row[field] for key, field in carried.items() if row.get(field) is not None}}
        for m, row in zip(masks, oracle_rows)
    ]


def corpus_reading(corpus, reference_dir) -> dict:
    """How the capture's reference rows are read, from its committed profile: `modal` and the lines.

    Empty where the capture has no profile or reads as a plain string. The oracle's probe is read
    this way so its rows, and every model row read through them, match the reference profile's.
    """
    path = Path(reference_dir) / f"{corpus.capture_id}.json"
    if not corpus.capture_id or not path.exists():
        return {}
    profile = json.loads(path.read_text())
    out: dict = {}
    if not tone_class(int(profile.get("capture", {}).get("program", 0))).stiffness_fit:
        out["modal"] = True
    lines = profile.get("stationary_lines", {}).get("timbres", {}).get(corpus.timbre, {})
    if lines.get("lines"):
        out["stationary_lines_hz"] = [ln["hz"] for ln in lines["lines"]]
    body = profile.get("body_lines", {}).get("timbres", {}).get(corpus.timbre, {})
    if body.get("lines"):
        out["body_lines_hz"] = [ln["hz"] for ln in body["lines"]]
    return out


def probe_rows(
    mono: np.ndarray,
    pattern,
    sr: int,
    raw: np.ndarray | None = None,
    max_band_hz: float | None = None,
    threads: int = 1,
    tnr_masks: list[dict] | None = None,
    reading: dict | None = None,
    a4_offset_c: float | None = None,
    read_ends: dict[tuple[int, int], float] | None = None,
    leads: dict[tuple[int, int], float] | None = None,
) -> list[dict]:
    """Measure every analysis note of `pattern` with the metric set it calls for.

    A drum hit and a bowed note are both "one note of the probe", but nothing
    downstream of this point can score them the same way: one has a harmonic
    ladder and an intonation error, the other has neither. The pattern decides,
    once, and the rows carry the shape the loss then reads.

    Both metric sets read the same window — up to the next onset, never past it
    (`analysis_window_end`), so no note's release is measured through the next
    note's attack.

    `raw` is the same render before RMS normalisation, and before any downmix:
    (frames,) or (frames, channels), read as equal power by `level_of`. Everything
    measured from `mono` is a shape, and shapes are what normalisation is for;
    the level fields are the one thing that cannot survive it, so they are
    measured from the untouched signal instead. Omitting `raw` leaves those fields off the
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

    `tnr_masks` is `oracle_tnr_masks` of the oracle rows, one per analysis note, given
    when the render is the model's so both sides read tone-to-noise through
    the oracle's mask. `reading` is the oracle's own (`corpus_reading`), carried into its rows' masks.
    `a4_offset_c` is the oracle's `reference_a4_offset_c`, recorded on each of its rows for `cents`.
    `read_ends` is the oracle's `committed_read_ends`, the seconds a looped reference row is read
    to, recorded on its rows; the model reads to the same time through its mask. `leads` is the
    oracle's `committed_leads`: the lead-in its recordings had, which the timeline drops and the
    gate's onset search began inside.

    Everything the gate's dimensions read is read as the gate reads it, off the mean downmix of
    `raw` whatever the mono mode (`note_gate`), so a term and its dimension share one input.
    """
    notes = list(pattern.analysis_notes)
    masks = [None] * len(notes) if tnr_masks is None else list(tnr_masks)
    if len(masks) != len(notes):
        raise ValueError(f"{len(masks)} tone-to-noise masks for {len(notes)} analysis notes")
    raw_mono = None if raw is None else to_mono(np.asarray(raw))
    shape = mono if raw_mono is None else raw_mono

    def lead_of(note) -> float:
        return (leads or {}).get((note.note, note.velocity), 0.0)

    def gate_peak(row: dict, note, end: float) -> None:
        # The gate's `vel_range` reading: the largest sample of the note's slot, un-normalised.
        if raw_mono is not None:
            row["mono_peak_dbfs"] = peak_dbfs(raw_mono[int(note.start * sr) : int(end * sr)])

    if pattern.percussive:

        def measure(note, _mask) -> dict:
            end = analysis_window_end(pattern, note)
            row = analyze_hit(
                shape, sr, note, end, max_band_hz=max_band_hz, lead_s=lead_of(note)
            ).to_dict()
            gate_peak(row, note, end)
            return row
    else:

        def measure(note, mask) -> dict:
            end = analysis_window_end(pattern, note)
            # Found once and shared, so nothing that reads the attack can
            # disagree about where the note began.
            onset = note_onset(shape, sr, note, end, lead_of(note))
            fit_mask = None if mask is None else mask.get(FIT_TNR_MASK_KEY)
            until = (
                (read_ends or {}).get((note.note, note.velocity))
                if mask is None
                else mask.get(READ_END_KEY)
            )
            if mask is not None:
                mask = {k: v for k, v in mask.items() if k not in (FIT_TNR_MASK_KEY, READ_END_KEY)}
            row = analyze_note(
                mono, sr, note, end, onset=onset, tnr_mask=mask, reading=reading
            ).to_dict()
            # The gate's readings, on its onset-anchored gate and through the oracle's
            # fit-window mask; the oracle reads through its own.
            on = int(onset * sr)
            off = min(on + int(note.dur * sr), int(end * sr))
            given = row["tnr_mask"] if mask is None else mask
            # A reference row with no fit-window mask had no fit-window read, so the gate has no
            # cell here; the model's is absent too, never read through a mask of its own.
            fit = (
                {}
                if mask is not None and fit_mask is None
                else fit_window_readings(
                    shape[on:off],
                    sr,
                    note.note,
                    fit_mask,
                    modal=bool(given.get("modal")),
                    stationary_lines_hz=given.get("stationary_lines_hz", ()),
                    body_lines_hz=given.get("body_lines_hz", ()),
                    read_end=None if until is None else int(until * sr),
                )
            )
            # Absent where the gate reads none, so `analyze_note`'s own reading never stands in.
            row.update({field: fit.get(key) for key, field in FIT_WINDOW_FIELDS.items()})
            if row["fit_tnr_mask"] is None:
                del row["fit_tnr_mask"]
            if until is not None:
                row["read_to_join_s"] = until
            # The gate's onset shape is read at the fit window's f0, over the gate's own length.
            row.update(
                onset_shape(shape[: int(end * sr)], sr, onset, note.dur, fit["f0_hz"])
                if fit
                else {"onset_overshoot_db": None, "onset_settle_ms": None}
            )
            gate_peak(row, note, end)
            if a4_offset_c is not None:
                row["a4_offset_c"] = a4_offset_c
            if raw_mono is not None and held_readable(raw_mono[on:off], sr):
                row["held_peak_dbfs"] = held_peak_dbfs(raw_mono[on:off], sr)
            row["skeleton"] = skeleton_note(mono, sr, note)
            row["onset_ms"] = round((onset - note.start) * 1000.0, 2)
            row["attack_hf_db"] = attack_bands(shape, sr, note, onset)
            # The same window's other end. Kept as its own field rather than
            # appended to `attack_hf_db`, because the two are measured over
            # different spans — six 20 ms slices against one 50 ms window — and
            # a single field would invite a reader to treat them as one grid.
            row["attack_lf_db"] = attack_low_bands(mono, sr, note, onset)
            # Attribution for what the two above price. Not a term and not read
            # by `loss_terms` — see `attack_peaks` and `fixed_resonances`.
            row["attack_peaks"] = attack_peaks(mono, sr, note, onset)
            return row

    if threads > 1 and len(notes) > 1:
        with ThreadPoolExecutor(max_workers=min(threads, len(notes))) as pool:
            rows = list(pool.map(measure, notes, masks))
    else:
        rows = [measure(note, mask) for note, mask in zip(notes, masks)]
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


def fixed_resonances(rows: list[dict], *, recurrence_only: bool = False) -> list[dict]:
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
        need = max(RESONANCE_MIN_NOTES, round(len(contributing) * RESONANCE_RECURRENCE_FRACTION))
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
        out.append(
            {
                "hz": round(sum(f * p for f, p, _, _ in group) / weight, 1),
                "prominence_db": round(weight / len(group), 1),
                "notes": notes,
                # How many of those notes it landed on a partial for. Under the
                # default that is zero by construction; under `recurrence_only` it
                # is the note to read the finding with.
                "on_partial_notes": len({n for _, _, n, on in group if on}),
            }
        )
    out.sort(key=lambda d: -d["prominence_db"])
    return out


def _cell_sequence(value):
    """Return an array-like metric cell sequence, or ``None`` if it is absent."""
    if isinstance(value, (list, tuple, np.ndarray)):
        return value
    return None


def _limited_cells(value, limit: int | None = None):
    cells = _cell_sequence(value)
    return None if cells is None else cells[:limit]


def _reference_indexed_pairs(model_values, oracle_values):
    """Yield candidate/reference cells using the reference as the denominator.

    Metric arrays are evidence grids, not positional records. The reference
    decides which cells exist; an absent or truncated candidate therefore
    yields ``None`` for each finite oracle cell and is charged by
    :func:`_absent_or`. A missing reference field yields no cells and remains
    unknown, which keeps older cached profiles compatible with new metrics.
    """
    oracle = _cell_sequence(oracle_values)
    if oracle is None:
        return
    model = _cell_sequence(model_values)
    for index, oracle_value in enumerate(oracle):
        candidate = model[index] if model is not None and index < len(model) else None
        yield candidate, oracle_value


def loss_terms(
    model_rows: list[dict],
    oracle_rows_: list[dict],
    *,
    n_harm: int = HARM_REACH,
    mss: float = 0.0,
    audibility: bool = True,
) -> dict[str, float] | None:
    """Per-term mean mismatch between the model and the oracle, unweighted.

    Per note, from the same fields report.json carries:
      - `harm`: L1 distance of h1-normalized harmonics_db over the first
        `n_harm` harmonics (the most directly actionable timbre signal);
      - `cents`: |model - oracle| of the gate's `cents_vs_et`, the fit window's
        fundamental against equal temperament;
      - `tnr`: |model - oracle| of the gate's tone-to-noise ratio, read off the
        fit window through the oracle's mask. Two-sided, as the gate's bound is;
        the one-sided form's reason (a sampled oracle's breath noise is not the
        model's to match) is superseded, since a model cleaner than the
        reference is usually one missing the reference's own movement;
      - `env`: sustain-slope (dB/s), release (per 100 ms) and attack (per 10 ms)
        differences — the double-decay / ring-down signature a purely spectral
        match is blind to;
      - `init` / `slope`: per-harmonic ONSET ladder and decay slopes from
        `skeleton_note`, which separate the excitation spectrum from the loop
        decay (the time-averaged harmonic term conflates them). Per-bin deltas
        are capped (12 dB / 30 dB/s) so single-sample oracle quirks cannot
        dominate the objective.
      - `bright`: the gate's `centroid_pct`, |100 (model / oracle - 1)| per note of
        the fit window's line-masked |X|^2 centroid;
      - `hfshare`: the gate's `attack_hf`, |model - oracle| dB per note of the
        4-8 kHz share of the first 120 ms;
      - `arrival`: the gate's arrival time, |model - oracle| ms per note.
      - `body`: the gate's energy under the fundamental, |model - oracle| dB per note.

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
    # Cells charged at an absent cap: the reference held a reading and the model had none.
    absent_n = [0]

    def charge(delta: float | None, cap: float) -> float:
        absent_n[0] += delta is None
        return _delta_or_cap(delta, cap)

    # How many partials above the fundamental were present on both sides
    # anywhere in the probe. Zero is not a good score, it is no measurement: a
    # render that fell silent reports every harmonic at the -120 dB floor and
    # h1 at 0 by definition, so the guard below skips all of them, h1 matches h1
    # exactly, and the harmonic term comes out at 0.0 — the best value it has.
    # Left unguarded, silencing any gain is the cheapest way to win this term.
    compared = available = 0
    # How many notes `tnr` compared: the reference read a finite ratio and no loop took the note off it.
    tnr_notes = 0
    # How many reference readings of `tnr` and `body` the recording's floor took off the gate.
    tnr_floor_dropped = body_floor_dropped = 0
    # How many cells of each capped aggregate were a comparison rather than a
    # cap standing in for one. See `CellCount`: the raw value of a term whose
    # cells all hit the cap is its worst, not its best, so none of the empty-set
    # guards above can see it and a reader cannot tell it from a real distance.
    cells = {t: CellCount() for t in ("env", "init", "slope", "tail", "hf", "lf", "mod", "modes")}
    bright_hits = arrival_hits = body_hits = hfshare_notes = 0
    for m, o in zip(model_rows, oracle_rows_):
        # A reading the reference has and the model lacks costs the cap and still counts as a hit.
        if o.get("fit_centroid_hz"):
            totals["bright"] += charge(
                100.0 * (m["fit_centroid_hz"] / o["fit_centroid_hz"] - 1.0)
                if m.get("fit_centroid_hz")
                else None,
                BRIGHT_ABSENT_CAP,
            )
            bright_hits += 1
        o_hf = attack_hf_share_of(o.get("attack_hf_db"))
        if o_hf is not None:
            m_hf = attack_hf_share_of(m.get("attack_hf_db"))
            totals["hfshare"] += charge(None if m_hf is None else m_hf - o_hf, HF_DELTA_CAP_DB)
            hfshare_notes += 1
        if o.get("arrival_ms") is not None:
            totals["arrival"] += charge(
                None if m.get("arrival_ms") is None else m["arrival_ms"] - o["arrival_ms"],
                ARRIVAL_ABSENT_CAP,
            )
            arrival_hits += 1
        # Terms the loop or the recording's floor take off the gate on this reference row are skipped on it alone.
        floor = o.get("floor_censored") or ()
        off = [*(o.get("loop_unweighted") or ()), *floor]
        if o.get("body_below_f0_db") is not None:
            if "body" in floor:
                body_floor_dropped += 1
            if "body" not in off:
                absent_n[0] += _finite_or_none(m.get("body_below_f0_db")) is None
                totals["body"] += _absent_or(
                    m.get("body_below_f0_db"), o["body_below_f0_db"], BODY_DELTA_CAP_DB
                )
                body_hits += 1
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
        f0_hz = m.get("f0_hz") or (midi_to_hz(m["note"]) if m.get("note") is not None else 0.0)
        b = m.get("inharmonicity_b") or 0.0
        weights = (
            audibility_weights(
                [partial_hz(f0_hz, k + 1, b) if f0_hz > 0.0 else None for k in range(len(pairs))],
                [max(mh, oh) for mh, oh in pairs],
            )
            if audibility
            else np.ones(len(pairs))
        )
        # A model whose every partial above h1 sits on the sentinel where the
        # reference has some is silent, not above Nyquist: its floor is charged.
        silent_model = not any(mh > -120.0 for mh, _ in pairs[1:]) and any(
            oh > -120.0 for _, oh in pairs[1:]
        )
        for i, (mh, oh) in enumerate(pairs):
            if oh <= -120.0 or (mh <= -120.0 and not (silent_model and i > 0)):
                continue
            if not (present_m[i] or present_o[i]):
                # Neither side has a partial here. Charging the difference
                # between two noise floors is what this skip exists to stop; a
                # bin where only ONE side has a partial is a real difference and
                # is still charged.
                continue
            totals["harm"] += float(weights[i]) * abs(mh - oh)
            compared += i > 0 and present_m[i] and present_o[i]
        # A reading the reference has and the model lacks is charged the cap; one it lacks is skipped.
        o_cents = _finite_or_none(o.get("cents_vs_et"))
        if "cents" not in off and o_cents is not None:
            m_cents = _finite_or_none(m.get("cents_vs_et"))
            # The gate's `stretch`: model minus reference, the reference's own A4 offset removed.
            stretch = None if m_cents is None else m_cents - o_cents + o.get("a4_offset_c", 0.0)
            totals["cents"] += charge(stretch, CENTS_ABSENT_CAP)
        o_tnr = _finite_or_none(o.get("fit_tnr_db"))
        if "tnr" in floor and o_tnr is not None:
            tnr_floor_dropped += 1
        if "tnr" not in off and o_tnr is not None:
            m_tnr = _finite_or_none(m.get("fit_tnr_db"))
            totals["tnr"] += charge(None if m_tnr is None else m_tnr - o_tnr, TNR_ABSENT_CAP_DB)
            tnr_notes += 1
        totals["env"] += _absent_or(
            m["sustain_slope_db_s"], o["sustain_slope_db_s"], SUSTAIN_SLOPE_CAP_DB_S, cells["env"]
        )
        # A reference that never fell 40 dB after note-off read its floor, not a release.
        if o.get("release_capped"):
            cells["env"].skipped += 1
        else:
            totals["env"] += abs(m["release_ms"] - o["release_ms"]) / 100.0
        totals["env"] += _attack_delta_ms(m, o) / 10.0
        oracle_skeleton = o.get("skeleton")
        if isinstance(oracle_skeleton, dict):
            model_skeleton = m.get("skeleton") if isinstance(m.get("skeleton"), dict) else {}
            for a, b in _reference_indexed_pairs(
                model_skeleton.get("init_db"), oracle_skeleton.get("init_db")
            ):
                totals["init"] += _absent_or(a, b, 12.0, cells["init"])
            for key in () if "slope" in off else ("early_db_s", "late_db_s"):
                for a, b in _reference_indexed_pairs(
                    _limited_cells(model_skeleton.get(key), HARM_REACH),
                    _limited_cells(oracle_skeleton.get(key), HARM_REACH),
                ):
                    totals["slope"] += _absent_or(a, b, 30.0, cells["slope"], 0.1) / 10.0
            for a, b in (
                ()
                if "tail" in off
                else _reference_indexed_pairs(
                    _limited_cells(model_skeleton.get("tail_db_s"), HARM_REACH),
                    _limited_cells(oracle_skeleton.get("tail_db_s"), HARM_REACH),
                )
            ):
                totals["tail"] += _absent_or(a, b, TAIL_DELTA_CAP_DB_S, cells["tail"], 0.1) / 10.0
        for a, b in _reference_indexed_pairs(m.get("attack_hf_db"), o.get("attack_hf_db")):
            totals["hf"] += _absent_or(a, b, HF_DELTA_CAP_DB, cells["hf"])
        # Averaged over the bands the reference offered, not summed over them,
        # because the percussion branch of this same term is a mean of one
        # number per hit: summed, one name would carry two aggregates five
        # times apart and no single unit could scale both. The denominator is
        # the ORACLE's band count, so a candidate cannot shrink it.
        lf_parts = []
        lf_pairs = list(_reference_indexed_pairs(m.get("attack_lf_db"), o.get("attack_lf_db")))
        lf_bands = max(1, sum(_finite_or_none(b) is not None for _a, b in lf_pairs))
        for a, b in lf_pairs:
            value = _absent_or(a, b, LF_DELTA_CAP_DB, cells["lf"], 1.0 / lf_bands)
            if _finite_or_none(b) is not None:
                lf_parts.append(value)
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
    mod_pairs = [
        (m, o)
        for m, o in zip(model_rows, oracle_rows_, strict=True)
        if "mod" not in (o.get("loop_unweighted") or ())
    ]
    out["mod"], mod_notes = _mod_terms(
        [m for m, _ in mod_pairs], [o for _, o in mod_pairs], cells["mod"]
    )
    out["modes"], modes_notes = _modes_terms(model_rows, oracle_rows_, cells["modes"])
    level_counts: dict[str, float] = {}
    out["level"], out["crest"], offset = _level_terms(
        model_rows, oracle_rows_, counts=level_counts, level_field=LEVEL_FIELDS[False]
    )
    # Reported alongside the value because a probe with no velocity axis can
    # only score zero here, and zero is this term's best possible value.
    dyn_counts: dict[str, float] = {}
    out["dyn"], dyn_groups = _dyn_terms(model_rows, oracle_rows_, counts=dyn_counts)
    hfdyn_counts: dict[str, float] = {}
    out["hfdyn"], hfdyn_groups = _swing_terms(
        model_rows, oracle_rows_, _attack_hf_share, HF_DELTA_CAP_DB, counts=hfdyn_counts
    )
    modedyn_counts: dict[str, float] = {}
    out["modedyn"], modedyn_groups = _dyn_terms(
        model_rows, oracle_rows_, read=_modal_brightness, counts=modedyn_counts
    )
    onsetvel_counts: dict[str, float] = {}
    out["onsetvel"], onsetvel_groups = _onsetvel_terms(
        model_rows, oracle_rows_, counts=onsetvel_counts
    )
    onset_counts: dict[str, float] = {}
    out["onset"], onset_compared = _onset_terms(model_rows, oracle_rows_, counts=onset_counts)
    velspan_counts: dict[str, float] = {}
    out["velspan"], velspan_groups = _velspan_terms(model_rows, oracle_rows_, counts=velspan_counts)
    velcurve_counts: dict[str, float] = {}
    out["velcurve"], _ = _velcurve_terms(model_rows, oracle_rows_, counts=velcurve_counts)
    stiff_counts: dict[str, float] = {}
    out["stiff"], stiff_notes = _stiff_terms(model_rows, oracle_rows_, counts=stiff_counts)
    out.update(stiff_counts)
    if any(not math.isfinite(v) for v in out.values()):
        return None
    out["dyn_groups"] = float(dyn_groups)
    out["hfdyn_groups"] = float(hfdyn_groups)
    out["modedyn_groups"] = float(modedyn_groups)
    out["onsetvel_groups"] = float(onsetvel_groups)
    out["velspan_groups"] = float(velspan_groups)
    out.update({f"velcurve_{key}": value for key, value in velcurve_counts.items()})
    out["onset_compared"] = float(onset_compared)
    out["stiff_notes"] = float(stiff_notes)
    out["tnr_notes"] = float(tnr_notes)
    out["tnr_floor_dropped"] = float(tnr_floor_dropped)
    out["body_floor_dropped"] = float(body_floor_dropped)
    out["mod_notes"] = float(mod_notes)
    out["modes_notes"] = float(modes_notes)
    # How many ladder bins above the fundamental held a partial on both sides.
    # Zero means this voice has no harmonic series where the ladder looks — a
    # bar, a bell, a membrane — and `harm` is then measuring nothing, in which
    # case its zero is not a match. Read it exactly as `dyn_groups` is read.
    out["harm_bins"] = float(compared)
    out["bright_hits"] = float(bright_hits)
    out["arrival_hits"] = float(arrival_hits)
    out["body_hits"] = float(body_hits)
    out["hfshare_notes"] = float(hfshare_notes)
    denominators = {"mod": mod_notes, "modes": modes_notes}
    for term, tally in cells.items():
        out.update(tally.out(term))
        out[f"{term}_uncapped"] = tally.uncapped(out[term], denominators.get(term, n))
    out.update(level_counts)
    out["absent_charged"] = float(
        absent_n[0]
        + sum(out[f"{term}_absent"] for term in cells)
        + sum(
            c.get("absent", 0.0)
            for c in (dyn_counts, hfdyn_counts, modedyn_counts, onsetvel_counts, velspan_counts)
        )
        + level_counts["level_expected"]
        - level_counts["level_notes"]
        + level_counts.get("crest_absent", 0.0)
        + stiff_counts.get("stiff_absent", 0.0)
    )
    out.update({f"dyn_{key}": value for key, value in dyn_counts.items()})
    out.update({f"hfdyn_{key}": value for key, value in hfdyn_counts.items()})
    out.update({f"modedyn_{key}": value for key, value in modedyn_counts.items()})
    out.update({f"onsetvel_{key}": value for key, value in onsetvel_counts.items()})
    out.update({f"velspan_{key}": value for key, value in velspan_counts.items()})
    out.update({f"onset_{key}": value for key, value in onset_counts.items()})
    out["level_offset_db"] = offset
    out["comparable"] = 1.0
    return out


def _focal_cell_mean(values: list[float]) -> tuple[float, float]:
    """Return the mean and the worst-quarter mean of the cells.

    The worst quarter (at least one cell) keeps one focal defect from vanishing
    in the plain mean.
    """
    if not values:
        return 0.0, 0.0
    ordered = sorted(values, reverse=True)
    mean = math.fsum(ordered) / len(ordered)
    upper = ordered[: max(1, math.ceil(len(ordered) * 0.25))]
    return mean, math.fsum(upper) / len(upper)


def percussion_terms(
    model_rows: list[dict],
    oracle_rows_: list[dict],
    *,
    mss: float = 0.0,
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
    `bandrms` is the gate's `band_shape`: the RMS distance of the two band
    profiles over every band, uncapped. `crest` reads the gate's `crest_db`.

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
    # Cells charged at an absent cap: the reference held a reading and the model had none.
    absent_n = [0]

    def charge(delta: float | None, cap: float) -> float:
        absent_n[0] += delta is None
        return _delta_or_cap(delta, cap)

    band_bins = bdecay_bins = tilt_hits = bright_hits = tonal_hits = rise_hits = ring_hits = 0
    strike_hits = bandrms_hits = 0
    # See `CellCount` and the pitched reducer: how much of each capped aggregate
    # is a comparison rather than a cap standing in for one.
    cells = {
        t: CellCount()
        for t in ("band", "bdecay", "modes", "density", "prompt", "evolve", "diffuse")
    }
    density_bins = prompt_bins = 0
    evolution_cells = window_flatness_cells = 0
    evolution_reference_unavailable = 0
    window_flatness_reference_unavailable = 0
    evolution_deltas: list[float] = []
    window_flatness_deltas: list[float] = []
    # The same cells with a clip charged its full distance; an absence stays the cap.
    uncapped_deltas: dict[str, list[float]] = {"evolve": [], "diffuse": []}
    for m, o in zip(model_rows, oracle_rows_):
        tilt_m, tilt_o = band_tilt_db(m.get("bands_db")), band_tilt_db(o.get("bands_db"))
        if tilt_o is not None:
            totals["tilt"] += charge(
                None if tilt_m is None else tilt_m - tilt_o, TILT_ABSENT_CAP_DB
            )
            tilt_hits += 1
        if o.get("centroid_hz"):
            totals["bright"] += charge(
                100.0 * (m["centroid_hz"] / o["centroid_hz"] - 1.0)
                if m.get("centroid_hz")
                else None,
                BRIGHT_ABSENT_CAP,
            )
            bright_hits += 1
        flat_m, flat_o = m.get("flatness_db"), o.get("flatness_db")
        if flat_o is not None:
            totals["tonal"] += charge(
                None if flat_m is None else flat_m - flat_o, TONAL_ABSENT_CAP_DB
            )
            tonal_hits += 1
        rise_m, rise_o = m.get("tilt_rise_db"), o.get("tilt_rise_db")
        if rise_o is not None:
            totals["rise"] += charge(
                None if rise_m is None else rise_m - rise_o, RISE_ABSENT_CAP_DB
            )
            rise_hits += 1
        strike_m, strike_o = m.get("tilt_strike_db"), o.get("tilt_strike_db")
        if strike_o is not None:
            totals["strike"] += charge(
                None if strike_m is None else strike_m - strike_o, STRIKE_ABSENT_CAP_DB
            )
            strike_hits += 1
        # A capped model decay is a lower bound, kept as `loss_kit` keeps it; a capped reference is still refused.
        ring = ring_doublings({**m, "decay_capped": False}, o)
        if ring is not None:
            totals["ring"] += abs(ring)
            ring_hits += 1
        # A reference cell the model lacks costs the cap; absent reference fields are unmeasured.
        for value_key, valid_key, term, cap, counter_name in (
            ("modal_density", "modal_density_valid", "density", DENSITY_CLIP, "density_bins"),
            ("prompt_late_db", "prompt_late_valid", "prompt", PROMPT_CLIP, "prompt_bins"),
        ):
            model_values, model_valid = m.get(value_key), m.get(valid_key)
            oracle_values, oracle_valid = o.get(value_key), o.get(valid_key)
            sequence = (list, tuple, np.ndarray)
            if not isinstance(oracle_values, sequence) or not isinstance(oracle_valid, sequence):
                # Legacy profiles have no answer to this new question.
                continue
            for i, oracle_ok in enumerate(oracle_valid):
                if not oracle_ok or i >= len(oracle_values):
                    cells[term].skipped += 1
                    continue
                oracle_value = oracle_values[i]
                if not np.isscalar(oracle_value) or not math.isfinite(float(oracle_value)):
                    cells[term].skipped += 1
                    continue
                if counter_name == "density_bins":
                    density_bins += 1
                else:
                    prompt_bins += 1
                model_ok = (
                    isinstance(model_values, sequence)
                    and isinstance(model_valid, sequence)
                    and i < len(model_values)
                    and i < len(model_valid)
                    and bool(model_valid[i])
                    and np.isscalar(model_values[i])
                    and math.isfinite(float(model_values[i]))
                )
                if not model_ok:
                    totals[term] += cap
                    cells[term].absent += 1
                    continue
                delta = abs(float(model_values[i]) - float(oracle_value))
                totals[term] += min(delta, cap)
                if delta >= cap:
                    cells[term].clipped += 1
                    cells[term].excess += delta - cap
                else:
                    cells[term].compared += 1
        # The reference's eligibility mask defines the temporal cells; a missing candidate cell is charged the cap.
        for value_key, valid_key, measured_key, term, cap, counter_name in (
            (
                "evolution_db",
                "evolution_valid",
                "evolution_measured",
                "evolve",
                EVOLUTION_CLIP_DB,
                "evolution_cells",
            ),
            (
                "window_flatness_db",
                "window_flatness_valid",
                "window_flatness_measured",
                "diffuse",
                WINDOW_FLATNESS_CLIP_DB,
                "window_flatness_cells",
            ),
        ):
            model_values, model_valid = m.get(value_key), m.get(valid_key)
            model_measured = m.get(measured_key)
            oracle_values, oracle_valid = o.get(value_key), o.get(valid_key)

            def _cells(values, valid):
                def _sequence(value):
                    if isinstance(value, np.ndarray):
                        return [value.item()] if value.ndim == 0 else value
                    return value if isinstance(value, (list, tuple)) else None

                values = _sequence(values)
                valid = _sequence(valid)
                if values is None or valid is None:
                    return {}

                def _valid_flag(value):
                    # A malformed multi-cell ndarray makes that coordinate unknown.
                    try:
                        return bool(value)
                    except (TypeError, ValueError):
                        return False

                output = {}
                for row_index, value in enumerate(values):
                    if isinstance(value, (list, tuple)) or (
                        isinstance(value, np.ndarray) and value.ndim > 0
                    ):
                        valid_row = (
                            valid[row_index]
                            if row_index < len(valid)
                            and (
                                isinstance(valid[row_index], (list, tuple))
                                or (
                                    isinstance(valid[row_index], np.ndarray)
                                    and valid[row_index].ndim > 0
                                )
                            )
                            else []
                        )
                        for cell_index, cell in enumerate(value):
                            output[(row_index, cell_index)] = (
                                cell,
                                cell_index < len(valid_row) and _valid_flag(valid_row[cell_index]),
                            )
                    else:
                        output[(row_index, 0)] = (
                            value,
                            row_index < len(valid) and _valid_flag(valid[row_index]),
                        )
                return output

            oracle_cells = _cells(oracle_values, oracle_valid)
            model_cells = _cells(model_values, model_valid)
            model_measured_cells = _cells(model_values, model_measured)
            for coordinate, (oracle_value, oracle_ok) in oracle_cells.items():
                if not bool(oracle_ok) or _finite_or_none(oracle_value) is None:
                    # A reference-invalid coordinate was never evidence, so it is counted apart from `skipped`.
                    if term == "evolve":
                        evolution_reference_unavailable += 1
                    else:
                        window_flatness_reference_unavailable += 1
                    continue
                if counter_name == "evolution_cells":
                    evolution_cells += 1
                else:
                    window_flatness_cells += 1
                model_value, model_valid_ok = model_cells.get(coordinate, (None, False))
                measured_value, model_measured_ok = model_measured_cells.get(
                    coordinate, (None, False)
                )
                if coordinate not in model_cells:
                    model_value = measured_value
                model_ok = bool(model_valid_ok) or bool(model_measured_ok)
                if not model_ok or _finite_or_none(model_value) is None:
                    value = distance = cap
                    cells[term].absent += 1
                else:
                    distance = abs(float(model_value) - float(oracle_value))
                    value = min(distance, cap)
                    if value >= cap:
                        cells[term].clipped += 1
                    else:
                        cells[term].compared += 1
                uncapped_deltas[term].append(distance)
                if term == "evolve":
                    evolution_deltas.append(value)
                else:
                    window_flatness_deltas.append(value)
        # The gate's `band_shape`, every band; the reference's floored bands included.
        if o.get("bands_db"):
            shape = band_shape_error_db(m.get("bands_db"), o["bands_db"])
            totals["bandrms"] += charge(shape, BAND_DELTA_CAP_DB)
            bandrms_hits += 1
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
            cells["band"].excess += max(0.0, delta - BAND_DELTA_CAP_DB)
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
            cells["bdecay"].excess += max(0.0, octaves - BDECAY_OCTAVE_CAP)
        totals["env"] += abs(m["attack_ms"] - o["attack_ms"]) / 5.0
        totals["env"] += abs(m["decay_ms"] - o["decay_ms"]) / 100.0
        totals["env"] += abs(m["crest_db"] - o["crest_db"]) / 3.0
    n = len(model_rows)
    out = {name: totals[name] / n for name in LOSS_TERMS}
    evolution_mean, evolution_upper = _focal_cell_mean(evolution_deltas)
    flatness_mean, flatness_upper = _focal_cell_mean(window_flatness_deltas)
    out["evolve"] = max(evolution_mean, evolution_upper) if evolution_deltas else 0.0
    out["diffuse"] = max(flatness_mean, flatness_upper) if window_flatness_deltas else 0.0
    out["mss"] = mss
    level_counts: dict[str, float] = {}
    out["level"], out["crest"], offset = _level_terms(
        model_rows,
        oracle_rows_,
        counts=level_counts,
        level_field=LEVEL_FIELDS[True],
        crest_field=CREST_FIELDS[True],
    )
    # Reported alongside the value because a probe with no velocity axis can
    # only score zero here, and zero is this term's best possible value.
    dyn_counts: dict[str, float] = {}
    out["dyn"], dyn_groups = _dyn_terms(model_rows, oracle_rows_, counts=dyn_counts)
    velspan_counts: dict[str, float] = {}
    out["velspan"], velspan_groups = _velspan_terms(model_rows, oracle_rows_, counts=velspan_counts)
    velcurve_counts: dict[str, float] = {}
    out["velcurve"], _ = _velcurve_terms(model_rows, oracle_rows_, counts=velcurve_counts)
    # The pitch of everything in a kit that has one — see `_modes_terms`. Empty
    # for a cymbal or a shaker, whose rows carry no modes, so the term costs
    # those nothing and prices a mistuned tom.
    out["modes"], modes_notes = _modes_terms(model_rows, oracle_rows_, cells["modes"])
    # The low end as one region rather than as six of twenty-five bands — see
    # `_perc_lf_terms`. Scored on the same profiles `band` reads, so it is a
    # re-weighting of evidence already in hand and not a second measurement.
    lf_counts: dict[str, float] = {}
    out["lf"], lf_notes = _perc_lf_terms(model_rows, oracle_rows_, counts=lf_counts)
    # The relations between the kit's own members — see `_kit_terms`. Empty
    # without a capture that declares its families, which is what a probe of one
    # drum note gets and is the right answer there.
    kit_counts: dict[str, float] = {}
    out["kit"], kit_notes = _kit_terms(model_rows, oracle_rows_, groups, counts=kit_counts)
    if any(not math.isfinite(v) for v in out.values()):
        return None
    out["dyn_groups"] = float(dyn_groups)
    out["velspan_groups"] = float(velspan_groups)
    out.update({f"velcurve_{key}": value for key, value in velcurve_counts.items()})
    out["modes_notes"] = float(modes_notes)
    out["lf_notes"] = float(lf_notes)
    out["kit_notes"] = float(kit_notes)
    # How many band cells the comparison actually charged for: with the
    # reference's floored bands skipped, a low count means the capture is
    # narrower than the analysis range and the term is speaking for fewer bands
    # than it looks like. This one counts what the REFERENCE offered, so it does
    # not move with the candidate and `_went_unmeasurable` cannot fire on it —
    # it is kept in `TERM_COUNT_KEYS` anyway so that a skip reintroduced on the
    # model side is caught rather than silently unguarded.
    out["band_bins"] = float(band_bins)
    out["bdecay_bins"] = float(bdecay_bins)
    out["density_bins"] = float(density_bins)
    out["prompt_bins"] = float(prompt_bins)
    out["evolution_cells"] = float(evolution_cells)
    out["window_flatness_cells"] = float(window_flatness_cells)
    out["evolution_reference_unavailable"] = float(evolution_reference_unavailable)
    out["window_flatness_reference_unavailable"] = float(window_flatness_reference_unavailable)
    out["evolution_cell_mean"] = float(evolution_mean)
    out["window_flatness_cell_mean"] = float(flatness_mean)
    # Both skip when either side has no profile or no centroid to read, which a
    # candidate can cause by rendering silence, so both are counted and guarded.
    out["tilt_hits"] = float(tilt_hits)
    out["bright_hits"] = float(bright_hits)
    out["tonal_hits"] = float(tonal_hits)
    out["rise_hits"] = float(rise_hits)
    out["strike_hits"] = float(strike_hits)
    out["ring_hits"] = float(ring_hits)
    out["bandrms_hits"] = float(bandrms_hits)
    for term, tally in cells.items():
        out.update(tally.out(term))
        if term in uncapped_deltas:
            deltas = uncapped_deltas[term]
            out[f"{term}_uncapped"] = max(_focal_cell_mean(deltas)) if deltas else 0.0
        else:
            denominator = modes_notes if term == "modes" else n
            out[f"{term}_uncapped"] = tally.uncapped(out[term], denominator)
    out.update(level_counts)
    out["absent_charged"] = float(
        absent_n[0]
        + sum(out[f"{term}_absent"] for term in cells)
        + sum(c.get("absent", 0.0) for c in (dyn_counts, velspan_counts))
        + level_counts["level_expected"]
        - level_counts["level_notes"]
        + level_counts.get("crest_absent", 0.0)
        + kit_counts.get("kit_absent", 0.0)
    )
    out.update({f"dyn_{key}": value for key, value in dyn_counts.items()})
    out.update({f"velspan_{key}": value for key, value in velspan_counts.items()})
    out.update(lf_counts)
    out.update(kit_counts)
    out["level_offset_db"] = offset
    out["comparable"] = 1.0
    return out


def score_terms(
    model_rows: list[dict],
    oracle_rows_: list[dict],
    *,
    n_harm: int = HARM_REACH,
    mss: float = 0.0,
    percussive: bool = False,
    audibility: bool = True,
    groups: dict[str, list[int]] | None = None,
) -> dict[str, float] | None:
    """Reduce a rendered probe to raw loss terms, by the metric set it carries."""
    if percussive:
        return percussion_terms(model_rows, oracle_rows_, mss=mss, groups=groups)
    return loss_terms(model_rows, oracle_rows_, n_harm=n_harm, mss=mss, audibility=audibility)
