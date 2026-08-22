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

from metrics import analyze_hit, analyze_note
from patterns import analysis_window_end

SKELETON_MAX_S = 2.0  # analysis window per note (matches the sustain pattern)
MSS_FFT_SIZES = (512, 1024, 2048, 4096)


def skeleton_note(mono: np.ndarray, sr: int, note, n_harm: int = 12) -> dict:
    """Per-harmonic envelope skeleton of one note.

    Separates the two things the time-averaged spectrum conflates:
      - init_db: per-harmonic level extrapolated to the onset (dB rel h1) —
        the EXCITATION spectrum evidence;
      - early_db_s / late_db_s: per-harmonic decay slopes (0.08-0.40 s and
        0.8-1.8 s linear fits) — the LOOP decay evidence.
    Harmonics above Nyquist are None.
    """
    f0_nominal = 440.0 * 2.0 ** ((note.note - 69) / 12.0)
    start = int(note.start * sr)
    seg = np.asarray(
        mono[start : start + int(min(note.dur, SKELETON_MAX_S) * sr)], dtype=np.float64
    )
    win_n = int(0.05 * sr)
    hop = int(0.01 * sr)
    if len(seg) < win_n + hop:
        return {"init_db": [None] * n_harm, "early_db_s": [None] * n_harm,
                "late_db_s": [None] * n_harm}

    # Refine each partial's frequency (inharmonicity-aware) with a zoomed DFT
    # over the early sustain.
    ref = seg[int(0.05 * sr) : int(0.55 * sr)]
    ref_w = ref * np.hanning(len(ref))
    t_ref = np.arange(len(ref)) / sr
    freqs: list[float | None] = []
    for h in range(1, n_harm + 1):
        guess = f0_nominal * h
        if guess > sr / 2 - 500.0:
            freqs.append(None)
            continue
        cand = np.linspace(guess * 0.985, guess * 1.015, 41)
        amps = np.abs(np.exp(-2j * np.pi * np.outer(cand, t_ref)) @ ref_w)
        freqs.append(float(cand[int(np.argmax(amps))]))

    n_frames = (len(seg) - win_n) // hop
    frames = np.lib.stride_tricks.sliding_window_view(seg, win_n)[:: hop][:n_frames]
    frames = frames * np.hanning(win_n)
    t_win = np.arange(win_n) / sr
    active = [f for f in freqs if f is not None]
    basis = np.exp(-2j * np.pi * np.outer(t_win, np.array(active)))
    env = np.abs(frames @ basis)  # (n_frames, n_active)
    env_db = 20.0 * np.log10(env + 1e-12)
    t_frame = (np.arange(n_frames) * hop + win_n / 2) / sr

    def fit(lo: float, hi: float, col: np.ndarray) -> tuple[float, float] | None:
        mask = (t_frame >= lo) & (t_frame <= hi)
        if mask.sum() < 4:
            return None
        m, b = np.polyfit(t_frame[mask], col[mask], 1)
        return float(m), float(b)

    init_db: list[float | None] = []
    early: list[float | None] = []
    late: list[float | None] = []
    col_i = 0
    for f in freqs:
        if f is None:
            init_db.append(None)
            early.append(None)
            late.append(None)
            continue
        col = env_db[:, col_i]
        col_i += 1
        fe = fit(0.08, 0.40, col)
        fl = fit(0.80, 1.80, col)
        init_db.append(fe[1] if fe else None)
        early.append(fe[0] if fe else None)
        late.append(fl[0] if fl else None)
    if init_db[0] is None:
        return {"init_db": [None] * n_harm, "early_db_s": [None] * n_harm,
                "late_db_s": [None] * n_harm}
    ref_db = init_db[0]
    init_db = [None if v is None else v - ref_db for v in init_db]
    return {"init_db": init_db, "early_db_s": early, "late_db_s": late}

# The spectral centroid is deliberately excluded from the loss: it depends on
# the probe note set (register weighting) and has been an unreliable, noisy
# signal in this harness. Match harmonic profile, intonation, and noise floor
# instead.


def probe_rows(mono: np.ndarray, pattern, sr: int) -> list[dict]:
    """Measure every analysis note of `pattern` with the metric set it calls for.

    A drum hit and a bowed note are both "one note of the probe", but nothing
    downstream of this point can score them the same way: one has a harmonic
    ladder and an intonation error, the other has neither. The pattern decides,
    once, and the rows carry the shape the loss then reads.

    Both metric sets read the same window — up to the next onset, never past it
    (`analysis_window_end`), so no note's release is measured through the next
    note's attack.
    """
    rows = []
    if pattern.percussive:
        for note in pattern.analysis_notes:
            rows.append(analyze_hit(mono, sr, note, analysis_window_end(pattern, note)).to_dict())
        return rows
    for note in pattern.analysis_notes:
        row = analyze_note(mono, sr, note, analysis_window_end(pattern, note)).to_dict()
        row["skeleton"] = skeleton_note(mono, sr, note)
        rows.append(row)
    return rows


def _stft_mag(x: np.ndarray, n_fft: int, hop: int) -> np.ndarray:
    """Magnitude STFT of a mono signal; (frames, bins)."""
    if len(x) < n_fft:
        x = np.concatenate([x, np.zeros(n_fft - len(x))])
    n_frames = (len(x) - n_fft) // hop + 1
    frames = np.lib.stride_tricks.sliding_window_view(x, n_fft)[::hop][:n_frames]
    return np.abs(np.fft.rfft(frames * np.hanning(n_fft), axis=1))


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
        scale = max(float(np.mean(sb)), 1e-9)
        total += float(np.mean(np.abs(sa - sb))) / scale
        total += float(np.mean(np.abs(np.log(sa + 1e-5) - np.log(sb + 1e-5))))
    return total / len(MSS_FFT_SIZES)


# The terms the loss is built from, in report order. `harm`/`cents`/`tnr`/`init`/
# `slope` come from the harmonic metric set and `band`/`bdecay` from the
# percussion one; `env` and `mss` are computed for both. A run uses one group or
# the other — which one the probe pattern decides — and the unused terms stay at
# zero weight.
LOSS_TERMS = ("harm", "cents", "tnr", "env", "init", "slope", "mss", "band", "bdecay")

# Smallest value a term is normalised against, in that term's own units: 1 dB of
# harmonic-profile error, 1 cent, 1 dB of excess noise, and so on. Without a
# floor a term that happens to start near zero — the noise penalty of a model
# already cleaner than its oracle is exactly zero — would divide by nothing and
# swamp every other term the moment it moved at all.
TERM_FLOORS = {
    "harm": 1.0, "cents": 1.0, "tnr": 1.0, "env": 0.1, "init": 1.0, "slope": 0.1,
    "mss": 0.01, "band": 1.0, "bdecay": 0.1,
}


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


def loss_terms(
    model_rows: list[dict], oracle_rows_: list[dict], *, n_harm: int, mss: float = 0.0
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
        return {**{name: 0.0 for name in LOSS_TERMS}, "mss": mss}
    totals = {name: 0.0 for name in LOSS_TERMS}
    for m, o in zip(model_rows, oracle_rows_):
        for mh, oh in zip(m["harmonics_db"][:n_harm], o["harmonics_db"][:n_harm]):
            if mh > -120.0 and oh > -120.0:
                totals["harm"] += abs(mh - oh)
        totals["cents"] += abs(m["f0_cents_err"] - o["f0_cents_err"])
        totals["tnr"] += max(0.0, o["tnr_db"] - m["tnr_db"])  # only when the model is noisier
        totals["env"] += abs(m["sustain_slope_db_s"] - o["sustain_slope_db_s"])
        totals["env"] += abs(m["release_ms"] - o["release_ms"]) / 100.0
        totals["env"] += abs(m["attack_ms"] - o["attack_ms"]) / 10.0
        if "skeleton" in m and "skeleton" in o:
            sm, so = m["skeleton"], o["skeleton"]
            for a, b in zip(sm["init_db"], so["init_db"]):
                if a is not None and b is not None:
                    totals["init"] += min(abs(a - b), 12.0)
            for key in ("early_db_s", "late_db_s"):
                for a, b in zip(sm[key][:6], so[key][:6]):
                    if a is not None and b is not None:
                        totals["slope"] += min(abs(a - b), 30.0) / 10.0
    n = len(model_rows)
    out = {name: totals[name] / n for name in LOSS_TERMS}
    out["mss"] = mss
    if any(not math.isfinite(v) for v in out.values()):
        return None
    return out


# Per-band deltas are capped for the same reason the harmonic ones are: a band
# the oracle happens to have almost nothing in produces a huge dB difference
# from a model that has slightly less, and one such band would otherwise decide
# the whole objective.
BAND_DELTA_CAP_DB = 24.0
BAND_DECAY_DELTA_CAP_DB_S = 60.0


def percussion_terms(
    model_rows: list[dict], oracle_rows_: list[dict], *, mss: float = 0.0
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
        return {**{name: 0.0 for name in LOSS_TERMS}, "mss": mss}
    totals = {name: 0.0 for name in LOSS_TERMS}
    for m, o in zip(model_rows, oracle_rows_):
        for a, b in zip(m["bands_db"], o["bands_db"]):
            totals["band"] += min(abs(a - b), BAND_DELTA_CAP_DB)
        for a, b in zip(m["band_decay_db_s"], o["band_decay_db_s"]):
            if a is not None and b is not None:
                totals["bdecay"] += min(abs(a - b), BAND_DECAY_DELTA_CAP_DB_S) / 10.0
        totals["env"] += abs(m["attack_ms"] - o["attack_ms"]) / 5.0
        totals["env"] += abs(m["decay_ms"] - o["decay_ms"]) / 100.0
        totals["env"] += abs(m["crest_db"] - o["crest_db"]) / 3.0
    n = len(model_rows)
    out = {name: totals[name] / n for name in LOSS_TERMS}
    out["mss"] = mss
    if any(not math.isfinite(v) for v in out.values()):
        return None
    return out


def score_terms(
    model_rows: list[dict], oracle_rows_: list[dict],
    *, n_harm: int, mss: float = 0.0, percussive: bool = False,
) -> dict[str, float] | None:
    """Reduce a rendered probe to raw loss terms, by the metric set it carries."""
    if percussive:
        return percussion_terms(model_rows, oracle_rows_, mss=mss)
    return loss_terms(model_rows, oracle_rows_, n_harm=n_harm, mss=mss)


def cli_weights(args) -> dict[str, float]:
    """The term weights as given on the command line.

    A percussion probe carries none of the harmonic terms, so a drum fit weights
    the percussion group instead; `--w-env` and `--w-mss` mean the same thing in
    both. `--w-env` defaults per group rather than to a constant: the temporal
    envelope is an optional refinement for a sustained voice and most of what
    distinguishes one drum from another, so it starts at 0 for one and 1 for the
    other. `run` prints the resolved weights, since a default that depends on
    the probe is otherwise invisible.
    """
    percussive = getattr(args, "percussive", False)
    env = args.w_env if args.w_env is not None else (1.0 if percussive else 0.0)
    if percussive:
        return {"band": args.w_band, "bdecay": args.w_bdecay, "env": env, "mss": args.w_mss}
    return {
        "harm": args.w_harm, "cents": args.w_cents, "tnr": args.w_tnr,
        "env": env, "init": args.w_init, "slope": args.w_slope, "mss": args.w_mss,
    }


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

    def active(self) -> tuple[str, ...]:
        return tuple(t for t in LOSS_TERMS if self.weights.get(t, 0.0) > 0.0)

    def calibrate(self, terms: dict[str, float]) -> None:
        """Adopt `terms` as the reference point every term is measured against."""
        self.scales = {t: max(terms.get(t, 0.0), TERM_FLOORS[t]) for t in LOSS_TERMS}
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
            total += w * (value / self.scales[name] if self.scales else value)
        return total

    def combine(self, terms: dict[str, float] | None) -> float:
        if terms is None:
            return math.inf
        total = self._weighted(terms)
        if self.scales:
            total /= self.reference
        return total if math.isfinite(total) else math.inf
