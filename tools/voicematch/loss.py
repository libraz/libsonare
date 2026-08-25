"""The number a fit minimises: from a rendered probe to a single scalar.

Three layers. `probe_rows` measures a render with whichever metric set the probe
pattern calls for — the harmonic one for a pitched voice, the percussion one for
a drum hit. `loss_terms` / `percussion_terms` reduce a model/oracle pair of those
to the named terms a weight can be put on. `LossWeights` combines them, dividing
each by its value at the fit's start point so the start scores exactly 1.0 and a
weight means the same thing across terms whose raw units differ by orders of
magnitude.

`skeleton_note` lives here rather than in `metrics` because it exists for the
fit: it separates the excitation spectrum from the loop decay, which is what
lets a staged fit score those two against different evidence.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from metrics import (  # noqa: E402
    THIRD_OCTAVE_CENTERS,
    _peak_near,
    _spectrum,
    analyze_hit,
    analyze_note,
    attack_bands,
    attack_low_bands,
    attack_peaks,
    audibility_weights,
    MIN_PARTIALS_FOR_B,
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
MSS_FFT_SIZES = (512, 1024, 2048, 4096)

# Where the three decay bands are fitted, in seconds from the onset. The first
# two are the prompt sound and the start of the aftersound; the third only has
# frames to fit when the probe holds the note that long, and is what a
# two-second window could never reach.
SKELETON_BANDS = {"early_db_s": (0.08, 0.40), "late_db_s": (0.80, 1.80),
                  "tail_db_s": (2.00, 6.00)}

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


def skeleton_note(mono: np.ndarray, sr: int, note, n_harm: int = 12) -> dict:
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
    win_n = int(0.05 * sr)
    hop = int(0.01 * sr)
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
        if mask.sum() < 4 or float(np.max(col[mask])) < floor_db:
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
               max_band_hz: float | None = None) -> list[dict]:
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
    """
    rows = []
    if pattern.percussive:
        for note in pattern.analysis_notes:
            rows.append(analyze_hit(mono, sr, note, analysis_window_end(pattern, note),
                                    max_band_hz=max_band_hz).to_dict())
    else:
        for note in pattern.analysis_notes:
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
            rows.append(row)
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
                   int(round(len(contributing) * RESONANCE_RECURRENCE_FRACTION)))
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


def _stft_mag(x: np.ndarray, n_fft: int, hop: int) -> np.ndarray:
    """Magnitude STFT of a mono signal; (frames, bins)."""
    if len(x) < n_fft:
        x = np.concatenate([x, np.zeros(n_fft - len(x))])
    n_frames = (len(x) - n_fft) // hop + 1
    frames = np.lib.stride_tricks.sliding_window_view(x, n_fft)[::hop][:n_frames]
    return np.abs(np.fft.rfft(frames * np.hanning(n_fft), axis=1))


# How the multi-scale distance weights the spectrum, and why not equally.
#
# A linear-frequency bin grid puts half its bins above a quarter of the sample
# rate. At 48 kHz that is 12 kHz up, where a sampled reference is usually
# reporting its own capture bandwidth, and only a handful of bins cover the
# octave from 100 to 200 Hz where a bass note's whole identity lives. So an
# unweighted mean over bins is a mean over the wrong measure: it is dominated by
# the top two octaves, which carry the least musical information per bin and the
# most measurement artefact.
#
# Weighting each bin by 1/f makes every octave contribute equally, which is the
# scale pitch is heard on and the scale `shape/spectro.py` already compares its
# spectrograms on. The two halves of this harness were reading the same renders
# through different frequency scales.
MSS_LOG_WEIGHTING = True
#: Below this a bin is weighted as if it sat here. Without the clamp the DC and
#: first few bins take weights running to infinity.
MSS_MIN_HZ = 30.0


def _log_bin_weights(n_fft: int, sr: float = 48000.0) -> np.ndarray:
    """Per-bin weights that make each octave contribute equally, summing to 1."""
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    w = 1.0 / np.maximum(freqs, MSS_MIN_HZ)
    return w / w.sum()


def mss_distance(model: np.ndarray, oracle: np.ndarray) -> float:
    """Multi-scale STFT distance between two mono renders.

    The per-note metric set models what is known to matter for a physical
    voice — harmonic ladder, intonation, noise floor, envelope. This term sees
    the rest: inharmonicity, formant structure between the harmonics, attack
    detail, anything the sustain window misses. Log and linear magnitude are
    both summed at each scale, the linear term normalised by the oracle's own
    mean so the scales stay comparable. Phase is deliberately ignored — two
    renders of the same note are never phase-aligned.
    """
    n = min(len(model), len(oracle))
    if n < MSS_FFT_SIZES[-1]:
        return 0.0
    a = np.asarray(model[:n], dtype=np.float64)
    b = np.asarray(oracle[:n], dtype=np.float64)
    total = 0.0
    for n_fft in MSS_FFT_SIZES:
        sa = _stft_mag(a, n_fft, n_fft // 4)
        sb = _stft_mag(b, n_fft, n_fft // 4)
        m = min(len(sa), len(sb))
        sa, sb = sa[:m], sb[:m]
        # Each octave weighted equally rather than each bin — see
        # MSS_LOG_WEIGHTING. `np.average` over the bin axis, then a plain mean
        # over frames, so the weighting changes which frequencies count and not
        # which moments do.
        w = _log_bin_weights(n_fft) if MSS_LOG_WEIGHTING else None
        scale = max(float(np.average(sb, weights=w) if w is not None
                          else np.mean(sb)), 1e-9)
        lin = np.abs(sa - sb)
        log = np.abs(np.log(sa + 1e-5) - np.log(sb + 1e-5))
        if w is None:
            total += float(np.mean(lin)) / scale + float(np.mean(log))
        else:
            total += float(np.mean(lin @ w)) / scale + float(np.mean(log @ w))
    return total / len(MSS_FFT_SIZES)


# The terms the loss is built from, in report order. `harm`/`cents`/`tnr`/`init`/
# `slope`/`tail`/`hf`/`lf` come from the harmonic metric set and `band`/`bdecay`
# from the percussion one; `env`, `mss`, `level` and `crest` are computed for
# both. A run uses one group or the other — which one the probe pattern decides
# — and the unused terms stay at zero weight.
LOSS_TERMS = ("harm", "modes", "cents", "tnr", "mod", "env", "init", "slope",
              "tail", "hf", "lf", "stiff", "level", "crest", "dyn", "mss",
              "band", "bdecay", "kit")

# Smallest value a term is normalised against, in that term's own units: 1 dB of
# harmonic-profile error, 1 cent, 1 dB of excess noise, and so on. Without a
# floor a term that happens to start near zero — the noise penalty of a model
# already cleaner than its oracle is exactly zero — would divide by nothing and
# swamp every other term the moment it moved at all.
TERM_FLOORS = {
    "harm": 1.0, "modes": 1.0, "cents": 1.0, "tnr": 1.0, "mod": 0.5, "env": 0.1,
    "init": 1.0, "slope": 0.1,
    "tail": 0.1, "hf": 1.0, "lf": 1.0, "stiff": 1.0,
    "level": 0.5, "crest": 0.5, "dyn": 0.5,
    "mss": 0.01, "band": 1.0, "bdecay": 0.1,
    # A tenth of a doubling: about a semitone and a half of pitch, 7 % of a
    # decay, or 0.6 dB of level. Below that the relation is inside the
    # reference's own strike-to-strike variation.
    "kit": 0.1,
}

# Per-note caps, in each term's own units. A reference row can be measuring
# almost nothing — a partial 58 dB under the fundamental, a band the capture has
# no energy in — and an uncapped delta against one of those decides the whole
# objective on its own.
TAIL_DELTA_CAP_DB_S = 20.0
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
PITCHED_TERMS = ("harm", "cents", "tnr", "mod", "init", "slope", "tail", "hf",
                 "lf", "stiff") + _SHARED_TERMS
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
PERCUSSION_TERMS = ("band", "bdecay", "lf", "kit") + _SHARED_TERMS


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
            crests.append(LEVEL_DELTA_CAP_DB if mc is None
                          else min(abs(mc - oc), LEVEL_DELTA_CAP_DB))
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
        upper = [v for v in bands[len(bands) * 2 // 3:]
                 if v > BAND_REFERENCE_FLOOR_DB]
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


def _modes_terms(model_rows: list[dict], oracle_rows_: list[dict]) -> tuple[float, int]:
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
            if i < len(m_db) and j < len(o_db):
                cost += min(abs(m_db[i] - o_db[j]), MODE_DB_CAP)
        cost += MODE_UNMATCHED_DB * (len(m_hz) - len(pairs))   # spurious modes
        cost += MODE_UNMATCHED_DB * (len(o_hz) - len(pairs))   # missing modes
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


def _mod_terms(model_rows: list[dict], oracle_rows_: list[dict]) -> tuple[float, int]:
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
    """
    total = 0.0
    used = 0
    for m, o in zip(model_rows, oracle_rows_):
        parts: list[float] = []
        scored = False
        for field, scale, cap in (("vib_cents", MOD_VIB_CENTS_PER_UNIT, MOD_CENTS_CAP),
                                  ("trem_db", 1.0, MOD_DEPTH_CAP),
                                  ("beat_db", 1.0, MOD_DEPTH_CAP),
                                  ("f0_width_cents", MOD_WIDTH_CENTS_PER_UNIT,
                                   MOD_WIDTH_CAP)):
            mv, ov = m.get(field), o.get(field)
            if ov is None:
                continue
            scored = True
            parts.append(_absent_or(mv, ov, cap) / scale)
        for depth, rate, floor in (("vib_cents", "vib_rate_hz", MOD_RATE_MIN_CENTS),
                                   ("trem_db", "trem_rate_hz", MOD_RATE_MIN_DB),
                                   ("beat_db", "beat_rate_hz", MOD_RATE_MIN_DB)):
            md, od = m.get(depth), o.get(depth)
            mr, orr = m.get(rate), o.get(rate)
            if None in (md, od, mr, orr) or md < floor or od < floor:
                continue
            parts.append(min(abs(mr - orr), MOD_RATE_CAP_HZ))
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
        if (m.get("inharmonicity_partials", 0) < MIN_PARTIALS_FOR_B
                or o.get("inharmonicity_partials", 0) < MIN_PARTIALS_FOR_B):
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


def _absent_or(model, oracle, cap: float) -> float:
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
    property of the probe rather than of the voice.
    """
    if oracle is None:
        return 0.0
    return cap if model is None else min(abs(model - oracle), cap)


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


def loss_terms(
    model_rows: list[dict], oracle_rows_: list[dict], *, n_harm: int, mss: float = 0.0,
    audibility: bool = True,
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
    # of loss to whichever candidate reaches it first. Counted for the same
    # reason `stiff_notes` and `dyn_groups` are.
    tnr_notes = 0
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
        totals["env"] += abs(m["sustain_slope_db_s"] - o["sustain_slope_db_s"])
        totals["env"] += abs(m["release_ms"] - o["release_ms"]) / 100.0
        totals["env"] += _attack_delta_ms(m, o) / 10.0
        if "skeleton" in m and "skeleton" in o:
            sm, so = m["skeleton"], o["skeleton"]
            for a, b in zip(sm["init_db"], so["init_db"]):
                totals["init"] += _absent_or(a, b, 12.0)
            for key in ("early_db_s", "late_db_s"):
                for a, b in zip(sm[key][:6], so[key][:6]):
                    totals["slope"] += _absent_or(a, b, 30.0) / 10.0
            for a, b in zip(sm.get("tail_db_s", [])[:6], so.get("tail_db_s", [])[:6]):
                totals["tail"] += _absent_or(a, b, TAIL_DELTA_CAP_DB_S) / 10.0
        for a, b in zip(m.get("attack_hf_db", []), o.get("attack_hf_db", [])):
            totals["hf"] += _absent_or(a, b, HF_DELTA_CAP_DB)
        for a, b in zip(m.get("attack_lf_db", []), o.get("attack_lf_db", [])):
            totals["lf"] += _absent_or(a, b, LF_DELTA_CAP_DB)
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
    out["mod"], mod_notes = _mod_terms(model_rows, oracle_rows_)
    out["modes"], modes_notes = _modes_terms(model_rows, oracle_rows_)
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
    out["level_offset_db"] = offset
    out["comparable"] = 1.0
    return out


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


def _perc_lf_terms(model_rows: list[dict],
                   oracle_rows_: list[dict]) -> tuple[float, int]:
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
        valid = [i for i in range(len(THIRD_OCTAVE_CENTERS))
                 if ob[i] > BAND_REFERENCE_FLOOR_DB]
        m_lf, o_lf = _lf_balance_db(mb, valid), _lf_balance_db(ob, valid)
        if m_lf is None or o_lf is None:
            continue
        total += min(abs(m_lf - o_lf), LF_DELTA_CAP_DB)
        scored += 1
    return (total / scored, scored) if scored else (0.0, 0)


# --------------------------------------------------------------------------
# What a kit is, as opposed to what is in it.
#
# Every term above reduces one hit to numbers taken against that hit's own
# reference row. A kit is not a collection of independent instruments: six toms
# are one series of sizes, three hi-hats are one mechanism at three openings,
# and what a listener notices wrong first is the relation rather than the
# member. Nothing measured a note at a time can hold a relation.
#
# The gap is not merely one of emphasis, because the per-row terms are capped.
# Measured on this kit: the reference's six toms run 71 to 184 Hz and
# libsonare's key-track from 100 to 167, so every one of the six is more than
# MODE_CENTS_CAP out and `modes` returns its cap on all six. Widening the series
# — the one repair that would fix it — changes that term by nothing at all until
# the members come back under the cap, so the objective has no gradient in the
# direction of the fix. A saturated term is not a weak signal; it is no signal.
#
# A relation term does not saturate that way, because it measures CONTRAST:
# each member against its own group, so wherever the group as a whole sits
# cancels — that part is already priced by `modes`, `env` and `level` — and what
# is left is only the shape of the set.
#
# The comparison is between SORTED contrast vectors, which is what makes this a
# statement about the group rather than about its members. Two consequences,
# both wanted. It is immune to the layout question a drum capture always has:
# this kit lays its six toms out as 45, 47, 48, 50, 41, 43 against GM's order,
# and a set has no order to disagree about, so the term needs no note map and
# cannot charge for one. And it is blind to a permuted group — a model with the
# open hi-hat's decay on the closed key scores here exactly as one with them the
# right way round. That is deliberate: placement is what a per-note term is good
# at, and `env` charges a swapped hi-hat pair some eight units per row.
#
# Every relation is expressed in doublings — a factor of two in frequency, in
# milliseconds or in amplitude — so one cap serves all four and a unit means
# roughly the same amount of audible wrongness whichever one produced it.

#: Members that have to survive on both sides before a relation is scored. Two
#: is the floor by construction: one member has no interior, and its contrast
#: against itself is zero on both sides whatever either side did.
KIT_MIN_MEMBERS = 2

#: How far apart the REFERENCE's own members have to sit, in doublings, before
#: a relation counts as one.
#:
#: Measured rather than declared, for the reason `measure_band_edge` measures
#: its own edge: a group holds some relations and not others, and which is a
#: property of the instrument. This kit's six toms are level-matched to within
#: 1.2 dB — 0.19 doublings — so there is no level relation among them to
#: reproduce, and scoring one would charge the model for the reference's own
#: strike-to-strike variation. The same group's pitch spans 1.37 doublings and
#: its decay 0.98, which are relations. A capture whose groups were declared
#: with the relations they hold would drift from what the rows say the day
#: either changed; this cannot.
KIT_MIN_SPREAD = 0.25

#: Per-member cap, in doublings. Wide on purpose — the whole point of the term
#: is to keep a gradient where the per-row terms have none, so a cap tight
#: enough to bind on an ordinary error would reintroduce exactly the saturation
#: this was written to escape. Three doublings is past any relation a kit holds.
KIT_DOUBLING_CAP = 3.0

#: dB per doubling of amplitude, so a level relation lands in the same unit as
#: a frequency or a duration ratio.
KIT_DB_PER_DOUBLING = 20.0 * math.log10(2.0)

#: (name, row field, the field is already in dB, oracle field that disqualifies
#: a member). Four relations, each naming a different repair.
#:
#: `level` reads `peak_dbfs` rather than the row's own `level_db`, for the
#: reason `_level_terms` does: that field is measured on audio no normalisation
#: has touched, and it is also the one field of the four that a probe can be
#: missing. A run that measured only a normalised render then scores no level
#: relation, which is the right answer rather than a match.
#:
#: `decay` drops a member whose ORACLE row hit the analysis ceiling: that row's
#: number is the window rather than the instrument, and a contrast taken against
#: it measures how long the capture ran. The model side is NOT dropped for the
#: same reason, and the asymmetry is the honest direction — a capped model
#: decay is a lower bound on a render that rang past the window, and a charge
#: computed from a lower bound can only understate what is really there, where
#: dropping it would hide a model that rings too long altogether.
KIT_RELATIONS = (
    ("pitch", "tone_f0_hz", False, None),
    ("decay", "decay_ms", False, "decay_capped"),
    ("colour", "centroid_hz", False, None),
    ("level", "peak_dbfs", True, None),
)


def _kit_value(row: dict, field: str, in_db: bool) -> float | None:
    """One member's value in doublings, or None where the row has no usable one."""
    value = row.get(field)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    value = float(value)
    if not math.isfinite(value):
        return None
    if in_db:
        return value / KIT_DB_PER_DOUBLING
    return math.log2(value) if value > 0.0 else None


def _kit_relation(model_rows: list[dict], oracle_rows_: list[dict],
                  indices: list[int], field: str, in_db: bool,
                  guard: str | None) -> tuple[list[float], list[float]] | None:
    """(model contrasts, oracle contrasts) for one relation of one group.

    Both sides are read over exactly the same members: a contrast taken over two
    different sets is not a comparison of anything. Each side is then centred on
    its OWN median and sorted, so what survives is the spread and the spacing of
    the set and not where the set sits or which member holds which place.

    None when the members do not support the relation, or when the REFERENCE
    does not itself hold it — see KIT_MIN_SPREAD.
    """
    pairs = []
    for i in indices:
        if guard is not None and oracle_rows_[i].get(guard):
            continue
        m = _kit_value(model_rows[i], field, in_db)
        o = _kit_value(oracle_rows_[i], field, in_db)
        if m is not None and o is not None:
            pairs.append((m, o))
    if len(pairs) < KIT_MIN_MEMBERS:
        return None
    oracle_c = sorted(v - float(np.median([b for _, b in pairs])) for _, v in pairs)
    if oracle_c[-1] - oracle_c[0] < KIT_MIN_SPREAD:
        return None
    model_c = sorted(v - float(np.median([a for a, _ in pairs])) for v, _ in pairs)
    return model_c, oracle_c


def _kit_scored(model_rows: list[dict], oracle_rows_: list[dict],
                groups: dict[str, list[int]] | None):
    """Yield (family, relation, velocity, model contrasts, oracle contrasts).

    Groups arrive as ORACLE note numbers, since a family is a fact about the
    captured kit, and are resolved to row indices — so whatever pairing the
    caller established between the two sides stands here unchanged.

    One relation per velocity, because two members of a family struck at
    different velocities are not in a relation; they are two measurements.
    """
    if not groups:
        return
    index: dict[tuple, int] = {}
    for i, row in enumerate(oracle_rows_):
        index.setdefault((row.get("note"), row.get("velocity")), i)
    velocities = sorted({row.get("velocity") for row in oracle_rows_},
                        key=lambda v: (v is None, v))
    for family, notes in groups.items():
        for velocity in velocities:
            members = [index[(n, velocity)] for n in notes if (n, velocity) in index]
            if len(members) < KIT_MIN_MEMBERS:
                continue
            for name, field, in_db, guard in KIT_RELATIONS:
                found = _kit_relation(model_rows, oracle_rows_, members,
                                      field, in_db, guard)
                if found is not None:
                    yield family, name, velocity, found[0], found[1]


def _kit_terms(model_rows: list[dict], oracle_rows_: list[dict],
               groups: dict[str, list[int]] | None) -> tuple[float, int]:
    """How far the model's kit-internal relations sit from the reference's.

    A mean, so it dilutes: the shipped kit resolves to 790 member-relations
    across thirteen families, and one family badly wrong moves the number by a
    fortieth of its own charge. That is the same arithmetic `band` has over its
    twenty-five bands and it is left alone here rather than patched, because the
    two things that read this already answer it. A fit names the family it is
    working on (`--notes 41,43,45,47,48,50`), and then the term is that family
    and dilutes by nothing; a report calls `kit_report`, which never pools
    families together at all.
    """
    total = 0.0
    scored = 0
    for _family, _name, _v, model_c, oracle_c in _kit_scored(model_rows, oracle_rows_,
                                                             groups):
        total += sum(min(abs(a - b), KIT_DOUBLING_CAP)
                     for a, b in zip(model_c, oracle_c))
        scored += len(oracle_c)
    return (total / scored, scored) if scored else (0.0, 0)


def kit_report(model_rows: list[dict], oracle_rows_: list[dict],
               groups: dict[str, list[int]] | None) -> list[dict]:
    """One readable line per family and relation, pooled over the velocities.

    The scalar term answers whether the kit's relations are right; this answers
    which of them are not, which is what someone reading a comparison table
    needs and what a single number can never say. Same measurements — a family
    absent here is one no relation could be taken over, not one that passed.

    `spread` is the reference's own range in doublings and `model_spread` the
    model's over the same members; `charge` is what the term is being fed. A
    model spread far under the reference's is a family collapsed towards one
    instrument, which is the failure this was written for.
    """
    pooled: dict[tuple[str, str], dict[str, list[float]]] = {}
    for family, name, _v, model_c, oracle_c in _kit_scored(model_rows, oracle_rows_,
                                                           groups):
        acc = pooled.setdefault((family, name),
                                {"spread": [], "model_spread": [], "charge": [],
                                 "members": []})
        acc["spread"].append(oracle_c[-1] - oracle_c[0])
        acc["model_spread"].append(model_c[-1] - model_c[0])
        acc["charge"].append(sum(min(abs(a - b), KIT_DOUBLING_CAP)
                                 for a, b in zip(model_c, oracle_c)) / len(oracle_c))
        acc["members"].append(float(len(oracle_c)))
    out = []
    for (family, name), acc in pooled.items():
        out.append({
            "family": family, "relation": name,
            "spread": float(np.median(acc["spread"])),
            "model_spread": float(np.median(acc["model_spread"])),
            "charge": float(np.median(acc["charge"])),
            "members": int(max(acc["members"])),
            "velocities": len(acc["charge"]),
        })
    return sorted(out, key=lambda r: -r["charge"])


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
    band_bins = bdecay_bins = 0
    for m, o in zip(model_rows, oracle_rows_):
        for a, b in zip(m["bands_db"], o["bands_db"]):
            if b <= BAND_REFERENCE_FLOOR_DB:
                # The reference has floored this band. See
                # BAND_REFERENCE_FLOOR_DB — the capture cannot resolve it, so
                # neither can any charge levied here.
                continue
            totals["band"] += min(abs(a - b), BAND_DELTA_CAP_DB)
            band_bins += 1
        for a, b in zip(m["band_decay_db_s"], o["band_decay_db_s"]):
            if b is None or abs(b) < BDECAY_MIN_RATE_DB_S:
                continue
            if a is None or abs(a) < BDECAY_MIN_RATE_DB_S:
                # Skipped rather than charged at the cap, and this is the one
                # place the `_absent_or` doctrine does NOT apply. There, an
                # absent model value means the model failed to sound. Here it
                # means the estimator refused: the energy curve did not fall far
                # enough, or was not straight enough to have a rate, or the
                # window ended first. Charging a measurement failure would make
                # the term noisiest exactly where it is least certain.
                #
                # Nothing is lost by the skip, because a model that genuinely
                # stopped sounding is already caught elsewhere: `band` compares
                # a profile normalised to each side's own loudest band, `level`
                # and `crest` read the untouched signal, and `_fell_silent`
                # rejects the render outright.
                continue
            totals["bdecay"] += min(abs(math.log2(abs(a) / abs(b))),
                                    BDECAY_OCTAVE_CAP)
            bdecay_bins += 1
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
    out["modes"], modes_notes = _modes_terms(model_rows, oracle_rows_)
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
    # How many band cells the comparison actually charged for. Reported for the
    # same reason `tnr_notes` is: with the reference's floored bands skipped,
    # a low count means the capture is narrower than the analysis range and the
    # term is speaking for fewer bands than it looks like.
    out["band_bins"] = float(band_bins)
    out["bdecay_bins"] = float(bdecay_bins)
    out["level_offset_db"] = offset
    out["comparable"] = 1.0
    return out


def score_terms(
    model_rows: list[dict], oracle_rows_: list[dict],
    *, n_harm: int, mss: float = 0.0, percussive: bool = False,
    audibility: bool = True, groups: dict[str, list[int]] | None = None,
) -> dict[str, float] | None:
    """Reduce a rendered probe to raw loss terms, by the metric set it carries."""
    if percussive:
        return percussion_terms(model_rows, oracle_rows_, mss=mss, groups=groups)
    return loss_terms(model_rows, oracle_rows_, n_harm=n_harm, mss=mss,
                      audibility=audibility)


def cli_weights(args) -> dict[str, float]:
    """The term weights a run resolves to: the instrument's defaults, then the CLI.

    Every `--w-*` flag defaults to None rather than to a number, so "not given"
    is distinguishable from "given as zero". What fills the gaps is
    `toneclass.default_weights`, which answers by what the instrument IS.

    It used to be one set of numbers for everything: `harm`, `cents` and `tnr`
    at one, and every other weight at zero. That scores a time-averaged
    spectrum, an intonation error and a noise floor — no envelope, no decay, no
    level, no attack — which is defensible for a bowed note, where the spectrum
    genuinely is most of the identity, and close to useless for a struck one,
    where the identity is the decay and the strike. A modal voice fared worse
    still: `harm` measures its noise floor (see `_modes_terms`), so the default
    weighting scored a bell almost entirely on a quantity it does not have.

    A spec's `weights` block sits between the two — the class supplies what
    neither names. `run` prints what it resolved, since a default that depends
    on the instrument is otherwise invisible.
    """
    percussive = getattr(args, "percussive", False)
    drum_note = getattr(args, "drum_note", None)
    weights = default_weights(getattr(args, "program", 0), drum_note=drum_note,
                              percussive=percussive)
    if not getattr(args, "has_analysis_notes", True):
        # A probe with nothing to analyse a note at a time — `scale`,
        # `room-probe`. Only the whole-timeline term can say anything, so the
        # class defaults are dropped rather than supplied and then refused.
        weights = {t: w for t, w in weights.items() if t == "mss"}
    if not getattr(args, "has_kit_groups", True):
        # A drum probe whose capture named no families, or one narrowed below a
        # single family's worth of notes. Same treatment as above: the class
        # default is dropped rather than supplied and then refused, because a
        # one-drum fit is an ordinary thing to run. An explicit `--w-kit` gets
        # refused by the caller instead.
        weights.pop("kit", None)
    group = set(measured_terms(percussive))
    for term in LOSS_TERMS:
        given = getattr(args, f"w_{term}", None)
        if given is not None:
            weights[term] = given
    # A term the probe's metric set does not produce is dropped rather than
    # carried at zero: `dyn` on a percussion probe is shared and stays, but a
    # `harm` weight on a drum fit would be a weight on a term that is
    # structurally 0.0, which is that term's best score.
    return {t: w for t, w in weights.items() if t in group and w > 0.0}


# Which key carries the count of data points each averaged term was measured
# over. Every one of these terms skips the points it cannot use and divides by
# the survivors, and every one of them already reported its count — this is the
# table that makes the counts readable by name instead of by convention.
#
# A term absent from this table is measured over a fixed set (`mss` compares
# four fixed transform sizes, `level` reads the untouched signal) and has no
# count to go to zero.
#
# `lf` is two different measurements under one name and only the percussion one
# is listed here: for a drum it is the low end of the band profile and skips the
# bands the reference floored, and for a pitched voice it is the attack's low
# bands over a fixed set, with an absent value already priced by `_absent_or`.
# The pitched set never emits `lf_notes`, so the guard reads a zero baseline
# there and correctly stays out of the way.
TERM_COUNT_KEYS = {
    "harm": "harm_bins",
    "modes": "modes_notes",
    "mod": "mod_notes",
    "stiff": "stiff_notes",
    "dyn": "dyn_groups",
    "band": "band_bins",
    "bdecay": "bdecay_bins",
    "lf": "lf_notes",
    # The one most likely to go blind of any of them: a relation is dropped
    # whenever the reference stops holding it or a member stops supplying a
    # value, so a render whose toms lost their pitch takes every tom relation
    # with it and the term would otherwise report the best score it has.
    "kit": "kit_notes",
}

# What a term costs once it can no longer be measured, in units of its own start
# value. Above 1.0 on purpose: equal to the start would make going blind exactly
# as good as changing nothing, which leaves a flat direction for a search to
# drift along, and there is no upper bound on how wrong an unmeasurable term
# might be. Not infinite either — the point is to make the trade unattractive,
# not to reject a candidate that is better everywhere else.
UNMEASURABLE_PENALTY = 2.0


@dataclass
class LossWeights:
    """Turns the per-term mismatch into the single number the optimiser minimises.

    `scales` is what makes the weights mean what they say. The raw terms are in
    incomparable units — the harmonic term is an L1 sum over ten harmonics in dB
    and runs to tens, the intonation term is a handful of cents, the multi-scale
    term is a fraction — so weighting them directly gives whichever term happens
    to be numerically largest an influence nobody chose. Dividing each by its
    value at the fit's start point makes every term start at 1, so `--w-env 2`
    genuinely means twice the pull of a unit-weighted term, and the total is
    scaled so the start point scores exactly 1.0: a reported 0.85 is 15 % better
    than the compiled-in values, whatever the units were.

    `scales` is None until the baseline has been measured (and stays None under
    `--raw-loss`), in which case the raw weighted sum is used instead.
    """

    weights: dict[str, float]
    scales: dict[str, float] | None = None
    reference: float = 1.0
    #: How many data points each averaged term was measured over at the start
    #: point. See `_weighted` — this is what lets a term that has stopped being
    #: measurable be told from one that has become perfect.
    baseline_counts: dict[str, float] | None = None

    def active(self) -> tuple[str, ...]:
        return tuple(t for t in LOSS_TERMS if self.weights.get(t, 0.0) > 0.0)

    def calibrate(self, terms: dict[str, float]) -> None:
        """Adopt `terms` as the reference point every term is measured against."""
        self.scales = {t: max(terms.get(t, 0.0), TERM_FLOORS[t]) for t in LOSS_TERMS}
        self.baseline_counts = {t: float(terms.get(key, 0.0))
                                for t, key in TERM_COUNT_KEYS.items()}
        # Divide by the reference point's own score rather than by the sum of
        # the weights: a term that starts below its unit floor contributes less
        # than 1, so the weight sum is not what the start actually scores, and a
        # loss whose start is 0.84 rather than 1.00 is a number nobody can read
        # a percentage off.
        self.reference = 1.0
        scored = self._weighted(terms)
        self.reference = scored if scored > 0.0 else 1.0

    def _weighted(self, terms: dict[str, float]) -> float:
        total = 0.0
        for name in LOSS_TERMS:
            w = self.weights.get(name, 0.0)
            if w <= 0.0:
                continue
            value = terms.get(name, 0.0)
            if self._went_unmeasurable(name, terms):
                # A term that could be measured at the start point and cannot be
                # measured here is charged its worst rather than credited its
                # best. Every averaged term skips the points it cannot use and
                # divides by the survivors, which is right until there are none:
                # then the sum is 0.0 over 0 points and 0.0 is that term's best
                # possible score. A render whose modes vanished, whose vibrato
                # stopped being detectable or whose partials fell under the
                # floor therefore reads as the render that fixed the term, and
                # a search will take that trade every time it is offered.
                #
                # Each helper already reports its own count for exactly this
                # reason; nothing read them. `loss_terms` handles the harmonic
                # ladder's version of this by refusing the whole comparison,
                # which is right when the ladder is the objective and too blunt
                # when one term of ten has gone quiet.
                total += w * UNMEASURABLE_PENALTY
                continue
            total += w * (value / self.scales[name] if self.scales else value)
        return total

    def _went_unmeasurable(self, name: str, terms: dict[str, float]) -> bool:
        """Did this term have data at the start point and have none now?"""
        if not self.baseline_counts:
            return False
        key = TERM_COUNT_KEYS.get(name)
        if key is None:
            return False
        return self.baseline_counts.get(name, 0.0) > 0.0 and terms.get(key, 0.0) <= 0.0

    def combine(self, terms: dict[str, float] | None) -> float:
        if terms is None:
            return math.inf
        total = self._weighted(terms)
        if self.scales:
            total /= self.reference
        return total if math.isfinite(total) else math.inf
