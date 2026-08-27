"""Per-note timbre metrics and model-vs-oracle deltas.

All metrics are computed on a mono mix, per note event, from windows derived
from the score (the harness knows every onset/offset exactly). Level-dependent
metrics are taken after both renders are normalized to equal overall RMS, so
what remains measures timbre and balance, not master gain.

Metric set (per note):
  f0_hz / f0_cents_err   measured fundamental and deviation from equal temperament
  harmonics_db           first 12 harmonic magnitudes in dB relative to h1
  centroid_hz            amplitude-weighted spectral centroid of the sustain
  odd_even_db            mean(h3,h5,h7,h9) - mean(h2,h4,h6,h8)
  tnr_db                 tonal-to-noise ratio in the sustain window
  attack_ms              10% -> 90% rise time of the amplitude envelope
  sustain_slope_db_s     linear dB/s fit over the sustain window
  release_ms             time after note-off for the envelope to fall 40 dB
  sustain_rms_db         sustain-window RMS (post global normalization)

Percussion metric set (per hit, `analyze_hit`):
  bands_db               1/3-octave levels, dB relative to the loudest band
  band_decay_db_s        per-octave-band decay slope after the peak
  onset_ms               note-on to the strike the rest of the set is measured from
  attack_ms              strike to first arrival within 3 dB of the peak
  decay_ms               end of the attack to the last moment within 20 dB of the peak
  crest_db               peak-to-RMS ratio over the hit
  centroid_hz            broadband spectral centroid of the hit
  level_db               hit RMS (post global normalization)
A drum note has no fundamental, so every metric above that is anchored on one —
the harmonic ladder, the intonation error, the tonal-to-noise ratio — measures a
frequency the sound does not contain. What is left of a percussion hit is its
level *profile* and how fast each part of that profile dies, which is what these
measure instead.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass

import numpy as np

from smf import Note

MIN_SUSTAIN_SEC = 0.15
# Furthest the sustain window may sit from the onset, in seconds. These are
# exactly where the 0.3/0.9 fractions land on the two-second probe patterns, so
# every existing probe measures the identical window; the cap only bites on a
# gate longer than that, where the fraction would otherwise walk off the end of
# the note. See `analyze_note`.
SUSTAIN_WINDOW_S = (0.6, 1.8)
N_HARMONICS = 12

# ISO 1/3-octave centres, 50 Hz to 12.5 kHz: the resolution a percussion hit's
# spectrum is worth reporting at. Finer would resolve individual modes, which
# move with every knob and are not what a fit should chase; coarser would merge
# a snare's shell into its wires.
THIRD_OCTAVE_CENTERS = (
    50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0,
    800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0,
    8000.0, 10000.0, 12500.0,
)
THIRD_OCTAVE_RATIO = 2.0 ** (1.0 / 6.0)

# Decay is fit per octave band rather than per third-octave: a third-octave
# band of a noisy hit carries too few modes for a slope fit to be stable.
OCTAVE_CENTERS = (63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0)
OCTAVE_RATIO = 2.0 ** 0.5

# Longest stretch of one hit that is analyzed, and how far below the loudest
# band a band still counts as present. The floor keeps bands with no content in
# either render from reading as a large difference between two noise floors.
# The kit's long pieces, named once and read by three sides of the same
# decision: the analysis ceiling below, the gap `patterns.drum_gap_for`
# leaves after a hit, and the per-note `tail` a capture records them for.
# Measured on `reference/drums.json`, the median `decay_ms` of every cymbal
# in the kit sat between 1170 and 1760 ms against a ceiling of 1800 — those
# numbers were the window and not the cymbals. `decay_ms` is the time to
# fall 20 dB, and a ride that takes 1758 ms to fall that far has four or
# five seconds of wash after it, which is most of what makes it a cymbal.
#
# It lives here rather than with the probe patterns because it is a fact
# about the instruments, and the probe layout is one of its consumers.
LONG_DECAY_DRUM_NOTES = frozenset({
    49,  # crash cymbal 1
    51,  # ride cymbal 1
    52,  # chinese cymbal
    53,  # ride bell
    55,  # splash cymbal
    57,  # crash cymbal 2
    59,  # ride cymbal 2
    81,  # open triangle
    80,  # mute triangle - the same instrument, and its mute group's other half
    84,  # belltree, the longest and quietest thing in the kit
})

HIT_MAX_SEC = 1.8
#: What a hit is analysed over when it is one of the kit's long-decay notes.
#: `LONG_DECAY_DRUM_NOTES` gives those notes an eight-second gap in the
#: probe and `tail_by_note` records them for eight seconds instead of two; this
#: is the third side of the same decision, and without it the longer probe and
#: the longer capture both arrive at a window that stops at 1.8 s. A ceiling is
#: still wanted — past the wash there is only the room — but it belongs where
#: the instrument is, not where the shortest instrument is.
HIT_LONG_MAX_SEC = 10.0
BAND_FLOOR_DB = -60.0

# A hit's envelope is read on a far finer grid than a sustained note's. Time to
# peak is single-digit milliseconds for most of the kit, so the 5 ms hop that
# resolves a bowed attack quantises a snare's to 5, 10 or 15 and reports the
# quantisation rather than the attack.
HIT_ENVELOPE_HOP_MS = 0.5
HIT_ENVELOPE_WIN_MS = 2.0

# How far below the hit's own peak the strike is considered to have begun, and
# how long after the note-on one may still be looked for. A hosted plugin does
# not always sound a note in the buffer it was delivered in — measured on a
# sampled kit, the same key at six velocities started anywhere between 0 and
# 750 ms after the note-on — and a window anchored on the note-on rather than on
# the strike reports that scheduling jitter as the instrument's attack time.
HIT_ONSET_FLOOR_DB = -50.0
HIT_ONSET_SEARCH_SEC = 1.0

# How close to its own peak a hit counts as having arrived. Time to the peak
# itself is not a usable statistic for anything that washes: a crash holds
# within a couple of dB of its maximum for hundreds of milliseconds, so which
# frame carries the maximum is decided by ripple, and the same cymbal at six
# velocities reported 34, 204, 174, 164, 197 and 197 ms. First arrival within a
# tolerance is stable and is what a rise time means in any case.
HIT_ATTACK_TOLERANCE_DB = -3.0


def midi_to_hz(note: int) -> float:
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


# How a stereo render is reduced to the one channel every metric reads, and why
# the default is the one with a known defect.
#
# `mean` sums the channels, which comb-filters whatever is decorrelated between
# them. A reference captured through two spaced close mics is decorrelated by
# construction — that is what spacing is for — so the sum has notches at the
# frequencies where the path difference is half a wavelength, and a notch that
# lands on a partial reads as several dB of harmonic error the model is then
# asked to reproduce. A mono model has no such notches and cannot.
#
# It is still the default, and the reason is not that it is right. Every
# committed profile in `reference/` was measured through it, and those files
# cannot be re-measured without the plugin they came from. Changing the
# reduction would silently redefine what they contain. So the fix is offered
# rather than imposed: `--mono-mode left` (or `loudest`) takes one channel and
# has no sum in it at all, and `channel_correlation` reports how much of a
# difference that could make for a given render.
MONO_MODES = ("mean", "left", "loudest")


def to_mono(audio: np.ndarray, mode: str = "mean") -> np.ndarray:
    """Reduce (frames, channels) to one channel of float64.

    `mean` sums; `left` takes the first channel; `loudest` takes whichever
    channel carries the most energy. See `MONO_MODES` for why a sum is the
    default despite comb-filtering a spaced-pair capture.
    """
    if audio.ndim == 1:
        return audio.astype(np.float64)
    a = audio.astype(np.float64)
    if a.shape[1] == 1:
        return a[:, 0]
    if mode == "left":
        return a[:, 0]
    if mode == "loudest":
        return a[:, int(np.argmax((a**2).mean(axis=0)))]
    if mode != "mean":
        raise ValueError(f"unknown mono mode {mode!r} (choose from {MONO_MODES})")
    return a.mean(axis=1)


def channel_correlation(audio: np.ndarray) -> float | None:
    """How alike a stereo render's two channels are, as a correlation in [-1, 1].

    None for anything that is not two-channel. Near 1 means summing them is
    harmless; well below it means the sum is comb-filtered and a per-partial
    level read off that sum is partly the microphone spacing rather than the
    instrument. Reported rather than acted on — see `MONO_MODES`.
    """
    if audio.ndim != 2 or audio.shape[1] != 2:
        return None
    a = audio.astype(np.float64)
    left, right = a[:, 0] - a[:, 0].mean(), a[:, 1] - a[:, 1].mean()
    denom = float(np.std(left) * np.std(right))
    if denom <= 0.0:
        return None
    return float(np.mean(left * right) / denom)


def normalize_rms(audio: np.ndarray, target_rms: float = 0.05) -> np.ndarray:
    """Scale the whole render to a common RMS so level metrics compare balance."""
    rms = float(np.sqrt(np.mean(audio**2)))
    if rms < 1e-8:
        return audio
    return audio * (target_rms / rms)


def _db(x: np.ndarray | float, floor: float = 1e-12) -> np.ndarray | float:
    return 20.0 * np.log10(np.maximum(x, floor))


def _rms_envelope(y: np.ndarray, sr: int, hop_ms: float = 5.0, win_ms: float = 10.0):
    """Frame RMS envelope; returns (times_sec, rms) arrays."""
    hop = max(1, int(sr * hop_ms / 1000.0))
    win = max(hop, int(sr * win_ms / 1000.0))
    n = max(0, (len(y) - win) // hop + 1)
    if n == 0:
        return np.zeros(1), np.array([float(np.sqrt(np.mean(y**2)))])
    frames = np.lib.stride_tricks.sliding_window_view(y, win)[::hop][:n]
    rms = np.sqrt(np.mean(frames**2, axis=1))
    times = (np.arange(n) * hop + win / 2) / sr
    return times, rms


def _spectrum(seg: np.ndarray, sr: int):
    """Hann-windowed magnitude spectrum; returns (freqs, magnitude)."""
    win = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg * win))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return freqs, mag


def _peak_near(freqs: np.ndarray, mag: np.ndarray, center_hz: float, tolerance_cents: float) -> tuple[float, float]:
    """Strongest bin within ±tolerance_cents of center_hz -> (freq, magnitude).

    Refines the peak frequency by parabolic interpolation over log-magnitude.
    """
    lo = center_hz * 2.0 ** (-tolerance_cents / 1200.0)
    hi = center_hz * 2.0 ** (tolerance_cents / 1200.0)
    idx = np.where((freqs >= lo) & (freqs <= hi))[0]
    if idx.size == 0:
        return center_hz, 0.0
    k = idx[np.argmax(mag[idx])]
    if 0 < k < len(mag) - 1 and mag[k] > 0:
        with np.errstate(divide="ignore"):
            a, b, c = np.log(np.maximum(mag[k - 1 : k + 2], 1e-12))
        denom = a - 2 * b + c
        delta = 0.5 * (a - c) / denom if abs(denom) > 1e-12 else 0.0
        delta = float(np.clip(delta, -0.5, 0.5))
        return float(freqs[k] + delta * (freqs[1] - freqs[0])), float(mag[k])
    return float(freqs[k]), float(mag[k])


# A stiff string's partials are not at integer multiples: the nth sits at
# n·f0·sqrt(1 + B·n²). B is small — a concert grand's bass runs around 4e-4,
# an upright's closer to 8e-4 — but it compounds with n², and a ±40 cent search
# window around n·f0 stops containing the partial it is looking for well before
# the twelfth. At B=4e-4 the twelfth is 48 cents sharp and reads 49 dB dark; at
# 8e-4 the ninth through twelfth read 66 to 86 dB dark. None of that is timbre.
#
# The failure is a cliff rather than a gradient — the partial is either inside
# the window or it is not — so a model and a reference whose stiffness differs
# slightly can land on opposite sides of it and produce a fabricated mismatch
# of tens of decibels, in the term that carries weight 1.0 by default. Which is
# to say the fit then spends real knobs correcting an artefact of the ruler.
#
# Fitted rather than assumed, and fitted the way the reference profiler has
# always fitted it — this is the one estimator, not a second one that happens to
# agree. Three passes with a narrowing window, because the two unknowns depend on
# each other: where to look for partial n depends on B, and B comes from where
# the partials turned out to be. A single pass over a fixed window is measurably
# worse on real audio, not just in principle: against this fit it disagreed by up
# to 1.8x across a three-note piano probe and returned exactly zero on one note
# where the series was plainly stretched.
INHARMONICITY_TOLERANCES = (90.0, 45.0, 25.0)
MAX_FIT_PARTIALS = 20
# Highest partial used to seed the fit. Its predicted deviation at B=0 has to
# stay inside the widest search window for any stiffness a real string has: at
# 8e-4, an upright's bass, the eighth partial is 44 cents sharp and the twelfth
# is 94, so eight is the last one a zero-stiffness guess still finds.
INHARMONICITY_SEED_PARTIALS = 8
# Below this a partial's peak location is the noise floor's, not the partial's,
# so it is not evidence about stiffness.
INHARMONICITY_FLOOR_DB = 60.0
# Fewest partials that make the fit a measurement rather than a number. At the
# top of the keyboard there is barely a series left — C8 puts five partials under
# Nyquist — and a B from four of them says more about the noise than the string.
MIN_PARTIALS_FOR_B = 6
# Under this much predicted deviation at the top of the ladder, the ladder is
# harmonic for measurement purposes and B is taken as exactly zero. This is
# what keeps an organ, a brass voice or any other integer-partial instrument
# reading bit-for-bit what it read before stiffness was modelled at all.
INHARMONICITY_MIN_CENTS = 5.0
MAX_INHARMONICITY_B = 0.01


def fit_partial_series(freqs: np.ndarray, mag: np.ndarray, f0_seed: float,
                       h1_mag: float, sr: int) -> tuple[float, float, int]:
    """Fit the stiff-string law to a measured spectrum -> (f0, B, partials_fit).

    `(f_n / n)² = f0² + f0²·B·n²` is linear in `n²`, so both unknowns come out
    of one weighted least-squares pass — weighted by how strongly each partial
    was actually present, since a partial near the floor locates itself badly
    and should not steer the fit. Iterated three times from B=0 with the search
    window narrowing each pass, which is what stops a high partial from locking
    onto its neighbour once the prediction is good enough to be trusted.

    `partials_fit` is how many partials the last pass actually had. It is the
    difference between a measurement and a number, and callers gate on it
    rather than on the value alone.
    """
    if f0_seed <= 0.0 or h1_mag <= 0.0:
        return f0_seed, 0.0, 0
    floor = h1_mag * 10.0 ** (-INHARMONICITY_FLOOR_DB / 20.0)
    f0, b, fitted_on = f0_seed, 0.0, 0

    # Seed from the low partials before the first weighted pass, because a pass
    # that starts at B=0 and admits the whole series mistracks the top of it and
    # cannot recover: a high partial found at its integer multiple contributes
    # (f_n/n)² = f0² exactly, which is the value that says B is zero, and enough
    # of those hold the slope down however many times the fit is iterated. Below
    # the eighth partial a B=0 prediction is still inside the window for any real
    # stiffness, so those are the ones that can bootstrap it. The failure is not
    # hypothetical — without this the fit reported 6.3e-5 for a synthetic series
    # built at 8e-4, having locked onto the wrong peaks on its first pass.
    seeds: list[float] = []
    for n in range(2, INHARMONICITY_SEED_PARTIALS + 1):
        target = n * f0_seed
        if target >= 0.45 * sr:
            break
        fn, an = _peak_near(freqs, mag, target, INHARMONICITY_TOLERANCES[0])
        if an > floor and fn > 0.0:
            seeds.append(((fn / target) ** 2 - 1.0) / (n * n))
    if seeds:
        b = min(max(float(np.median(seeds)), 0.0), MAX_INHARMONICITY_B)

    for tol in INHARMONICITY_TOLERANCES:
        strong: list[tuple[int, float, float]] = []
        for n in range(1, MAX_FIT_PARTIALS + 1):
            predicted = partial_hz(f0, n, b)
            if predicted >= 0.45 * sr:
                break
            fn, an = _peak_near(freqs, mag, predicted, tol)
            if an > floor and fn > 0.0:
                strong.append((n, fn, an))
        if len(strong) < 4:
            break
        x = np.array([n * n for n, _, _ in strong], dtype=np.float64)
        y = np.array([(fn / n) ** 2 for n, fn, _ in strong], dtype=np.float64)
        w = np.array([an for _, _, an in strong], dtype=np.float64)
        w = w / w.sum()
        xm, ym = float((w * x).sum()), float((w * y).sum())
        denom = float((w * (x - xm) ** 2).sum())
        if denom <= 0.0:
            break
        slope = float((w * (x - xm) * (y - ym)).sum()) / denom
        intercept = ym - slope * xm
        if intercept <= 0.0:
            break
        f0 = float(np.sqrt(intercept))
        b = min(max(0.0, slope / intercept), MAX_INHARMONICITY_B)
        fitted_on = len(strong)
    return f0, b, fitted_on


def estimate_inharmonicity_b(freqs: np.ndarray, mag: np.ndarray, f0: float,
                             h1_mag: float, sr: int) -> tuple[float, int]:
    """Stiffness of the series, or exactly 0.0 when it is harmonic.

    The zero is deliberate and is what keeps an organ, a brass voice or any
    other integer-partial instrument reading bit-for-bit what it read before
    stiffness was measured at all: under `INHARMONICITY_MIN_CENTS` of predicted
    deviation at the top of the ladder there is nothing here a search window
    cares about, so the ladder is searched exactly where it always was.
    """
    _, b, fitted_on = fit_partial_series(freqs, mag, f0, h1_mag, sr)
    if b <= 0.0:
        return 0.0, fitted_on
    top = N_HARMONICS
    if 1200.0 * np.log2(np.sqrt(1.0 + b * top * top)) < INHARMONICITY_MIN_CENTS:
        return 0.0, fitted_on
    return b, fitted_on


def partial_hz(f0: float, n: int, b: float) -> float:
    """Where the nth partial of a string with stiffness `b` actually sits."""
    return n * f0 * float(np.sqrt(1.0 + b * n * n))


# How far out the partial series is worth extrapolating. The stiffness fit reads
# the low partials, where a string has any, and B enters the frequency as n², so
# the error in a predicted partial grows as the square of how far past the fit
# it is asked to reach. Beyond this the prediction is not accurate enough to say
# whether something sits on a partial or between two — which is the only thing
# it is used for here — so callers get told the answer is unknown instead.
MAX_EXTRAPOLATED_PARTIAL = 60


def partial_offset(freq: float, f0: float, b: float) -> float | None:
    """How far `freq` sits from the nearest partial, as a fraction of the gap.

    0.0 means exactly on a partial and 0.5 means exactly midway between two, so
    the value answers one question: could this peak BE a partial of the note
    that played it. A peak that cannot is the interesting case — a free
    resonance rung by the strike rather than driven by the string.

    None where the series cannot answer: no fundamental, or a frequency further
    out than `MAX_EXTRAPOLATED_PARTIAL`. An unanswerable question is not the
    same as an answer of zero, and reporting it as zero would quietly file every
    frequency above the fitted range as an ordinary partial.
    """
    if f0 <= 0.0 or freq <= 0.0:
        return None
    ns = np.arange(1, MAX_EXTRAPOLATED_PARTIAL + 2, dtype=float)
    fs = ns * f0 * np.sqrt(1.0 + b * ns * ns)
    if freq > fs[MAX_EXTRAPOLATED_PARTIAL - 1]:
        return None
    k = int(np.argmin(np.abs(fs - freq)))
    lo = fs[k] - fs[k - 1] if k > 0 else f0
    hi = fs[k + 1] - fs[k] if k + 1 < len(fs) else f0
    gap = (lo + hi) / 2.0
    if gap <= 0.0:
        return None
    return float(abs(fs[k] - freq) / gap)


def stretch_cents(b: float, n: int = N_HARMONICS) -> float:
    """How far stiffness `b` has pushed the nth partial above its harmonic.

    B itself spans an order of magnitude between a grand's treble and an
    upright's bass, so a difference in B is not a comparable quantity across
    the keyboard and a term built on one would be decided by whichever note had
    the stiffest string. The stretch it produces at a fixed partial is
    comparable, is what the ear and the ladder both actually meet, and arrives
    in cents — the same unit the intonation term already uses.
    """
    return 1200.0 * float(np.log2(np.sqrt(1.0 + b * n * n)))


# --------------------------------------------------------------------------- #
# Modal profile — the partial series of everything that is not a string
# --------------------------------------------------------------------------- #
# Why the harmonic ladder is not enough, and why this is not another window
# around a predicted place.
#
# Every pitched measurement above searches for partial n at n*f0*sqrt(1+B*n^2).
# That law describes a stiff string and nothing else. A bar, a bell, a plate or
# a membrane puts its partials at ratios no formula predicts and the model says
# so out loud: libsonare voices GM 8 at 1 : 2.756 : 5.404 : 8.933, GM 10 at
# 1 : 1.004 : 6.267 : 6.29 : 17.5, GM 14 at 0.5 : 1 : 2.76 : 5.4 : 8.9, and a
# timpano at 1 : 1.5 : 2 : 2.44. Not one of those ratios lands inside the
# ladder's +/-40 cent window around an integer multiple.
#
# What that produced was not an error. Measured on a synthesised celesta
# against a slightly differently tuned one, ten of the twelve ladder bins read
# the render's own noise floor on BOTH sides (-103 to -110 dB), the floor guard
# passed them because it tests for -120 rather than for the floor, and the
# harmonic term returned 15.09 — a confident number made almost entirely of the
# difference between two noise floors. A fit minimising it is shaping noise.
#
# So the partials are FOUND rather than predicted. The measurement is the same
# for a bell and for a violin; what changes is only that a violin's answer comes
# out at integer multiples.
#
# Frequencies are kept ABSOLUTE rather than as ratios to a detected anchor.
# Anchoring needs a mode chosen to anchor on, and the two obvious choices both
# fail: the lowest peak moves with the noise floor, and the strongest peak can
# be a different mode on the two sides, which shifts every ratio at once and
# reports one selection difference as a wholly retuned instrument. Both renders
# played the same MIDI note, so absolute cents is already the comparable
# quantity — and it prices mistuning, which a ratio cannot see at all.
MODAL_MAX_MODES = 12
#: How far under the strongest peak a peak still counts. Wide, because a bell's
#: upper modes are genuinely quiet and still audible; what it excludes is the
#: floor.
MODAL_FLOOR_DB = 55.0
#: How far above the locally smoothed spectrum a bin has to stand to be a mode
#: rather than a shoulder of one, and the span the baseline is smoothed over.
MODAL_PROMINENCE_DB = 8.0
MODAL_BASELINE_HZ = 400.0
#: Two peaks closer than this are one mode seen twice. A quarter tone: closer
#: than anything a bar's modes come, and wide enough to swallow the skirt a
#: Hann window puts either side of a partial.
MODAL_MERGE_CENTS = 50.0
#: Highest frequency worth reporting a mode at. Above this a sampled reference
#: is usually reporting its own capture bandwidth rather than the instrument.
MODAL_MAX_HZ = 12000.0


def measure_modes(freqs: np.ndarray, mag: np.ndarray, *,
                  n_max: int = MODAL_MAX_MODES,
                  floor_db: float = MODAL_FLOOR_DB) -> list[tuple[float, float]]:
    """The strongest isolated partials of a spectrum -> [(hz, db rel strongest)].

    Prominence against a locally smoothed baseline rather than a plain local
    maximum, so the skirt of a strong mode does not report as several weak ones
    and a dense low register does not report every ripple.

    Sorted by frequency, not by level, because the caller pairs them against
    another render's by frequency and a level ordering would only have to be
    undone. Returns fewer than `n_max` freely — a woodblock has two modes and
    padding the list would invent ten.
    """
    if len(freqs) < 8 or float(mag.max()) <= 0.0:
        return []
    db = 20.0 * np.log10(np.maximum(mag, 1e-12))
    bin_hz = float(freqs[1] - freqs[0])
    half = max(1, int(round(MODAL_BASELINE_HZ / bin_hz / 2.0)))
    kernel = np.ones(2 * half + 1) / (2 * half + 1)
    baseline = np.convolve(db, kernel, mode="same")
    top = float(db.max())
    cand: list[tuple[float, float]] = []
    hi = min(len(freqs) - 1, int(np.searchsorted(freqs, MODAL_MAX_HZ)))
    for i in range(1, hi):
        if db[i] < top - floor_db or db[i] - baseline[i] < MODAL_PROMINENCE_DB:
            continue
        if db[i] < db[i - 1] or db[i] < db[i + 1]:
            continue
        # Parabolic interpolation over log magnitude, the same refinement
        # `_peak_near` uses, so a mode is located to a fraction of a bin.
        a, b, c = db[i - 1], db[i], db[i + 1]
        denom = a - 2 * b + c
        delta = float(np.clip(0.5 * (a - c) / denom, -0.5, 0.5)) if abs(denom) > 1e-12 else 0.0
        cand.append((float(freqs[i] + delta * bin_hz), float(b)))
    if not cand:
        return []
    # Merge anything within a quarter tone, keeping the louder, then keep the
    # `n_max` loudest and hand them back in frequency order.
    cand.sort(key=lambda p: -p[1])
    kept: list[tuple[float, float]] = []
    for hz, level in cand:
        if any(abs(1200.0 * np.log2(hz / k[0])) < MODAL_MERGE_CENTS for k in kept):
            continue
        kept.append((hz, level))
        if len(kept) >= n_max:
            break
    kept.sort()
    peak = max(level for _, level in kept)
    return [(round(hz, 2), round(level - peak, 2)) for hz, level in kept]


def modal_profile(freqs: np.ndarray, mag: np.ndarray, expected_f0: float,
                  **kwargs) -> dict:
    """`measure_modes` as the fields a row carries.

    `modal_ratio` is against the note's nominal equal-tempered frequency rather
    than against a detected anchor: a fixed, known divisor cannot be chosen
    differently on the two sides, so the column is readable without being the
    thing the term compares.
    """
    modes = measure_modes(freqs, mag, **kwargs)
    return {
        "modal_hz": [hz for hz, _ in modes],
        "modal_db": [db for _, db in modes],
        "modal_ratio": [round(hz / expected_f0, 4) for hz, _ in modes]
        if expected_f0 > 0.0 else [],
    }


# How far under a note's OWN loudest partial the ladder has to sit before the
# bin is the render's floor rather than a partial. The -120 dB sentinel the
# ladder already writes catches only a bin above Nyquist; a bin that found
# nothing writes whatever the noise floor happened to be, which on a real render
# is -100 dB or so and passes every `> -120` test there is.
LADDER_FLOOR_MARGIN_DB = 80.0


def ladder_present(harmonics_db: list[float],
                   margin: float = LADDER_FLOOR_MARGIN_DB) -> list[bool]:
    """Which ladder bins found a partial rather than the noise floor.

    Read against the loudest bin of that note's own ladder, so it needs no
    absolute calibration and no knowledge of what the render's floor happens to
    be. A voice whose partials are where the ladder looks is entirely True; a
    bar or a bell is True at h1 and almost nowhere else, which is the finding.
    """
    if not harmonics_db:
        return []
    top = max(harmonics_db)
    return [v > -120.0 and v > top - margin for v in harmonics_db]


# --------------------------------------------------------------------------- #
# Audibility weighting
# --------------------------------------------------------------------------- #
# Two reasons an L1 over partials in dB is not what a listener hears, and both
# of them bite hardest exactly where this harness is used.
#
# Absolute frequency: A0's fundamental at 27.5 Hz and its eighth partial at
# 220 Hz are more than 30 dB apart in how loud they are for the same level, and
# an unweighted sum gives them the same vote. A bass note's timbre lives in the
# partials, not in the fundamental, and the unweighted term says otherwise.
#
# Level within the note: a partial 50 dB under the loudest one in the same note
# is masked by it. An error of 10 dB there is inaudible and is charged in full,
# so a fit can spend real knobs moving something no one can hear while a 3 dB
# error on the loudest partial waits.
#
# Neither is a model of hearing. A-weighting is a 40-phon curve applied at every
# level, and the masking weight is a slope rather than a spreading function.
# They are corrections in the right direction, and both are switchable, because
# a term whose value changes meaning between releases is worse than one that is
# merely crude.


def a_weight_db(freq_hz: np.ndarray | float) -> np.ndarray | float:
    """IEC 61672 A-weighting in dB, 0 dB at 1 kHz."""
    f = np.asarray(freq_hz, dtype=np.float64)
    f2 = np.maximum(f, 1e-6) ** 2
    num = (12194.0**2) * f2**2
    den = ((f2 + 20.6**2)
           * np.sqrt((f2 + 107.7**2) * (f2 + 737.9**2))
           * (f2 + 12194.0**2))
    with np.errstate(divide="ignore", invalid="ignore"):
        ra = np.where(den > 0.0, num / den, 0.0)
        out = 20.0 * np.log10(np.maximum(ra, 1e-30)) + 2.00

    return float(out) if np.isscalar(freq_hz) or np.ndim(freq_hz) == 0 else out


#: Under this much below the note's loudest partial, a partial's weight has
#: fallen to `AUDIBILITY_MIN_WEIGHT`. Chosen as a plausible masking span rather
#: than measured: within a critical band the slope is steeper and across the
#: spectrum it is shallower, so this is one number standing in for a surface.
AUDIBILITY_MASK_SPAN_DB = 45.0
#: What a fully masked partial still counts for. Not zero, because a partial
#: nobody can hear in the reference can still be the one the model is 40 dB LOUD
#: on, and a zero weight would license that.
AUDIBILITY_MIN_WEIGHT = 0.1
#: Range the A-weighting is allowed to span, in dB. Uncapped it runs to -50 dB
#: at 20 Hz, which does not down-weight the bottom octave so much as delete it.
AUDIBILITY_A_FLOOR_DB = -20.0


def audibility_weights(freqs_hz, levels_db, *, a_weight: bool = True,
                       mask: bool = True) -> np.ndarray:
    """Per-partial (or per-band) weights in [AUDIBILITY_MIN_WEIGHT, 1].

    `levels_db` are relative levels within one note — the h1-normalised ladder
    or the peak-normalised band profile — so the masking half needs no absolute
    calibration. `freqs_hz` may contain None for a bin that has no frequency,
    which is weighted by level alone.

    Both halves are multiplicative and each is switchable, because a term that
    silently changes meaning is worse than a crude one that does not.
    """
    n = len(levels_db)
    w = np.ones(n, dtype=np.float64)
    if mask:
        finite = [v for v in levels_db if v is not None and np.isfinite(v)]
        if finite:
            top = max(finite)
            for i, v in enumerate(levels_db):
                if v is None or not np.isfinite(v):
                    w[i] = AUDIBILITY_MIN_WEIGHT
                    continue
                frac = 1.0 - (top - v) / AUDIBILITY_MASK_SPAN_DB
                w[i] *= float(np.clip(frac, AUDIBILITY_MIN_WEIGHT, 1.0))
    if a_weight:
        for i, f in enumerate(freqs_hz):
            if f is None or not np.isfinite(f) or f <= 0.0:
                continue
            aw = max(float(a_weight_db(f)), AUDIBILITY_A_FLOOR_DB)
            # Referenced to 0 dB at 1 kHz and expressed as a gain in [0, 1], so
            # the weight can only ever reduce a partial's vote.
            w[i] *= float(10.0 ** (min(aw, 0.0) / 20.0))
    return np.maximum(w, AUDIBILITY_MIN_WEIGHT * AUDIBILITY_MIN_WEIGHT)


# --------------------------------------------------------------------------- #
# Movement: vibrato, tremolo, and the beat of an ensemble
# --------------------------------------------------------------------------- #
# Nothing in this harness measured whether a note MOVES, and movement is most of
# what separates a played instrument from a synthesised one. The README has said
# so for as long as it has existed —
#
#   "A model reading 'cleaner, flatter' than the oracle often means 'add
#    vibrato/breath movement', not 'the oracle is worse'."
#
# — while the objective had no term that could ever say it. `tnr` is the closest
# and it is one-sided by construction: it charges the model for being NOISIER
# than the reference and is silent when the model is dead. So the one reading
# that reliably indicates a missing vibrato was the one reading the loss could
# not act on.
#
# What is measured is the fundamental's instantaneous frequency and level over
# the held part of the note, separated into three bands because they are three
# different mechanisms with three different repairs:
#
#   vibrato   3-9 Hz frequency modulation — the player's hand or embouchure
#   tremolo   3-9 Hz amplitude modulation — bowing pressure, breath
#   beat      0.3-3 Hz amplitude modulation — two or more sources slightly
#             detuned against each other, which is what a section, a unison
#             string pair and a chorused pad all are
#
# The beat band is why this also answers the ensemble question. A section
# patch's identity is how far apart its voices are tuned, and that is not
# reachable by any single-note spectral measure — but it is exactly a slow
# amplitude modulation of every partial, and one note is enough to read it.
MOD_FRAME_HOP_S = 0.01
MOD_FRAME_WIN_S = 0.05
#: Where the movement is read, in seconds from the detected onset. It starts
#: after the attack, since a rise is not a modulation, and it stops before a
#: struck note has decayed into its own floor.
MOD_WINDOW_S = (0.15, 2.5)
#: Fewest frames that make a modulation spectrum a measurement. At a 10 ms hop
#: this is 0.4 s, which is two and a half cycles of the slowest beat the band
#: covers — under that the peak found is the window rather than the note.
MOD_MIN_FRAMES = 40
VIBRATO_BAND_HZ = (3.0, 9.0)
BEAT_BAND_HZ = (0.3, 3.0)
#: How far either side of the nominal pitch the tracker looks. Wider than the
#: deepest vibrato any instrument uses, so the track cannot be clipped by its
#: own search range and report a shallow one.
MOD_TRACK_CENTS = 120.0
MOD_TRACK_POINTS = 49


def _band_peak(spectrum: np.ndarray, rate_hz: float, band: tuple[float, float],
               n: int) -> tuple[float, float]:
    """Strongest component of a modulation spectrum inside `band`.

    Returns (peak-to-peak amplitude in the input's own units, rate in Hz).
    The amplitude conversion assumes the Hann window the caller applied: a
    sinusoid of amplitude A lands at |X| = A*n/4, so peak-to-peak is 8|X|/n.
    """
    freqs = np.fft.rfftfreq(n, 1.0 / rate_hz)
    mask = (freqs >= band[0]) & (freqs <= band[1])
    if not mask.any():
        return 0.0, 0.0
    idx = np.where(mask)[0]
    k = idx[int(np.argmax(spectrum[idx]))]
    return float(8.0 * spectrum[k] / n), float(freqs[k])


def modulation_note(mono: np.ndarray, sr: int, note: Note, f0: float,
                    onset: float | None = None) -> dict:
    """Vibrato, tremolo and beat of one note's fundamental.

    The fundamental is tracked rather than assumed: a zoomed DFT over a bank of
    candidate frequencies per frame, which resolves a few cents at a 50 ms
    window where an FFT bin is 20 Hz. Both the frequency track and the level
    track are linearly detrended before the modulation spectrum is taken, so a
    note that is decaying (every level track) or drifting (a wind instrument
    warming) does not report its trend as a very slow modulation.

    Every field is None when the note is too short, too quiet or has no
    fundamental to track. A zero would be a claim that the note is dead still,
    which is a different finding from not having looked.
    """
    empty = {"vib_cents": None, "vib_rate_hz": None, "trem_db": None,
             "trem_rate_hz": None, "beat_db": None, "beat_rate_hz": None}
    if f0 <= 0.0 or f0 > sr / 2.0:
        return empty
    start = (note.start if onset is None else onset) + MOD_WINDOW_S[0]
    end = min((note.start if onset is None else onset) + MOD_WINDOW_S[1],
              note.start + note.dur)
    a, b = int(start * sr), int(end * sr)
    seg = np.asarray(mono[a:b], dtype=np.float64)
    win_n = int(MOD_FRAME_WIN_S * sr)
    hop = int(MOD_FRAME_HOP_S * sr)
    n_frames = (len(seg) - win_n) // hop + 1 if len(seg) >= win_n else 0
    if n_frames < MOD_MIN_FRAMES:
        return empty

    frames = np.lib.stride_tricks.sliding_window_view(seg, win_n)[::hop][:n_frames]
    frames = frames * np.hanning(win_n)
    span = 2.0 ** (MOD_TRACK_CENTS / 1200.0)
    cand = np.linspace(f0 / span, f0 * span, MOD_TRACK_POINTS)
    t = np.arange(win_n) / sr
    basis = np.exp(-2j * np.pi * np.outer(cand, t))       # (points, win_n)
    amps = np.abs(frames @ basis.T)                        # (frames, points)
    k = np.argmax(amps, axis=1)
    level = amps[np.arange(n_frames), k]
    if float(level.max()) <= 0.0:
        return empty

    # Parabolic refinement over the candidate grid, so the track is not
    # quantised to the grid spacing (5 cents at the default point count).
    log_cand = np.log2(cand)
    track = np.empty(n_frames)
    for i, ki in enumerate(k):
        if 0 < ki < MOD_TRACK_POINTS - 1:
            y0, y1, y2 = amps[i, ki - 1], amps[i, ki], amps[i, ki + 1]
            denom = y0 - 2 * y1 + y2
            d = float(np.clip(0.5 * (y0 - y2) / denom, -0.5, 0.5)) if abs(denom) > 1e-12 else 0.0
        else:
            d = 0.0
        step = log_cand[1] - log_cand[0]
        track[i] = log_cand[ki] + d * step
    cents = 1200.0 * (track - float(np.median(track)))
    level_db = 20.0 * np.log10(np.maximum(level, 1e-12))

    idx = np.arange(n_frames, dtype=np.float64)
    cents = cents - np.polyval(np.polyfit(idx, cents, 1), idx)
    level_db = level_db - np.polyval(np.polyfit(idx, level_db, 1), idx)

    window = np.hanning(n_frames)
    rate = 1.0 / MOD_FRAME_HOP_S
    fm = np.abs(np.fft.rfft(cents * window))
    am = np.abs(np.fft.rfft(level_db * window))
    vib_cents, vib_rate = _band_peak(fm, rate, VIBRATO_BAND_HZ, n_frames)
    trem_db, trem_rate = _band_peak(am, rate, VIBRATO_BAND_HZ, n_frames)
    beat_db, beat_rate = _band_peak(am, rate, BEAT_BAND_HZ, n_frames)
    return {
        "vib_cents": round(vib_cents, 2), "vib_rate_hz": round(vib_rate, 2),
        "trem_db": round(trem_db, 2), "trem_rate_hz": round(trem_rate, 2),
        "beat_db": round(beat_db, 2), "beat_rate_hz": round(beat_rate, 2),
    }


def f0_width_cents(freqs: np.ndarray, mag: np.ndarray, f0: float) -> float | None:
    """Width of the fundamental's spectral peak at -3 dB, in cents.

    A single string radiates one frequency and the width measured here is the
    analysis window's. Several sources a few cents apart — a piano's unison, a
    string section, a chorused pad — radiate a band, and the width is how far
    apart they are. That is the one property of an ensemble patch a single note
    can carry, and it is why this is taken alongside the beat rate rather than
    instead of it: the beat says how fast, this says how wide.

    None when the peak runs off the end of the analysed span, which is what a
    note with no stable fundamental gives.
    """
    if f0 <= 0.0 or len(freqs) < 4:
        return None
    lo = f0 * 2.0 ** (-200.0 / 1200.0)
    hi = f0 * 2.0 ** (200.0 / 1200.0)
    idx = np.where((freqs >= lo) & (freqs <= hi))[0]
    if idx.size < 4:
        return None
    local = mag[idx]
    k = int(np.argmax(local))
    top = float(local[k])
    if top <= 0.0:
        return None
    half = top * 10.0 ** (-3.0 / 20.0)
    left = k
    while left > 0 and local[left] > half:
        left -= 1
    right = k
    while right < len(local) - 1 and local[right] > half:
        right += 1
    if left == 0 or right == len(local) - 1:
        return None
    f_lo, f_hi = float(freqs[idx[left]]), float(freqs[idx[right]])
    if f_lo <= 0.0 or f_hi <= f_lo:
        return None
    return round(1200.0 * float(np.log2(f_hi / f_lo)), 1)


@dataclass
class NoteMetrics:
    note: int
    velocity: int
    f0_hz: float
    f0_cents_err: float
    harmonics_db: list[float]  # h1..h12, dB relative to h1 (h1 == 0)
    centroid_hz: float
    odd_even_db: float
    tnr_db: float
    attack_ms: float
    sustain_slope_db_s: float
    release_ms: float
    release_capped: bool
    sustain_rms_db: float
    # Reported rather than scored. It is the ruler the ladder above was read
    # with, and a model whose stiffness is far from its reference's is a real
    # difference worth seeing — but one that belongs to the string's physics,
    # not to the timbre terms, which are now measured independently of it.
    inharmonicity_b: float
    #: How many partials the stiffness fit had. Under `MIN_PARTIALS_FOR_B` the
    #: value is a number rather than a measurement — the top of the keyboard
    #: barely has a series left to fit — so a reader gates on this, not on B.
    inharmonicity_partials: int
    #: The partials as FOUND rather than as predicted — see `measure_modes`.
    #: Empty for a render with nothing in it. This is the only pitched
    #: measurement a bar, a bell or a membrane has, and it reduces to the
    #: harmonic ladder for anything whose partials are integer multiples.
    modal_hz: list[float]
    modal_db: list[float]
    modal_ratio: list[float]
    #: How many ladder bins found a partial rather than the note's own noise
    #: floor. A stiff string reports close to `N_HARMONICS`; a glockenspiel
    #: reports 1, which is what says the ladder is the wrong ruler for it.
    ladder_partials: int
    #: The attack again, on the grid the percussion path uses. `attack_ms`
    #: keeps its 5 ms hop and 10 ms window because every committed profile in
    #: `reference/` was measured with it and cannot be re-measured without the
    #: plugin it came from; this one resolves what that grid quantises. A
    #: struck or plucked string reaches its peak in single-digit milliseconds,
    #: so measured on the coarse grid a 0.5 ms rise and a 5 ms rise report the
    #: same number.
    attack_fine_ms: float
    #: How wide the fundamental is, in cents — one string or several. See
    #: `f0_width_cents`.
    f0_width_cents: float | None
    #: Vibrato, tremolo and beat. See `modulation_note`.
    vib_cents: float | None
    vib_rate_hz: float | None
    trem_db: float | None
    trem_rate_hz: float | None
    beat_db: float | None
    beat_rate_hz: float | None

    def to_dict(self) -> dict:
        return asdict(self)


def analyze_note(mono: np.ndarray, sr: int, note: Note, render_end: float,
                 *, onset: float | None = None) -> NoteMetrics:
    """Compute all per-note metrics from the mono render.

    `onset` is where the note actually starts sounding, which is not always
    where it was scheduled (see `note_onset`). It is used only by the
    measurements added after the committed profiles were taken — the fine
    attack and the modulation set — so that passing it cannot change a number
    one of those profiles carries. `attack_ms`, the ladder and the envelope
    metrics stay anchored on the score exactly as they were.
    """
    expected_f0 = midi_to_hz(note.note)
    on = int(note.start * sr)
    off = int((note.start + note.dur) * sr)

    # Sustain window: the settled middle of the note (falls back to the whole
    # note when it is too short for a stable window).
    #
    # Capped in seconds as well as taken as a fraction, because the gate is a
    # property of the probe and not of the instrument. On the two-second probes
    # the cap is exactly where the fraction already lands, so nothing moves. On
    # a corpus probe holding eight seconds the fraction alone would read 2.4 to
    # 7.2 s in, which on the top two octaves is entirely after the note has
    # stopped — measured on three concert grands, C8 is 40 dB down by half a
    # second and C7 inside one. The harmonic ladder
    # then compares one render's noise floor with another's and reports the
    # model 120 dB dark, unmoved by every knob, which is what silence looks like
    # when it is mistaken for a measurement.
    sus_a = int((note.start + min(0.3 * note.dur, SUSTAIN_WINDOW_S[0])) * sr)
    sus_b = int((note.start + min(0.9 * note.dur, SUSTAIN_WINDOW_S[1])) * sr)
    if sus_b - sus_a < int(MIN_SUSTAIN_SEC * sr):
        sus_a, sus_b = on, off
    sustain = mono[sus_a:sus_b]
    if len(sustain) < 256:
        sustain = mono[on : on + max(256, off - on)]

    freqs, mag = _spectrum(sustain, sr)

    # Fundamental: strongest peak within ±80 cents of equal temperament.
    f0, h1_mag = _peak_near(freqs, mag, expected_f0, 80.0)
    cents_err = 1200.0 * np.log2(f0 / expected_f0) if f0 > 0 else 0.0

    # How far this string's partials have been stretched by its own stiffness.
    # Zero for anything that is not a stiff string, which is what makes every
    # such voice read exactly what it read before this was measured at all.
    inharmonicity_b, inharmonicity_partials = estimate_inharmonicity_b(
        freqs, mag, f0, h1_mag, sr)

    # Harmonic profile relative to h1, each partial searched where the string
    # actually puts it rather than at an integer multiple.
    harmonics_db: list[float] = [0.0]
    h_mags = [h1_mag]
    for k in range(2, N_HARMONICS + 1):
        target = partial_hz(f0, k, inharmonicity_b)
        if target >= sr / 2:
            harmonics_db.append(-120.0)
            h_mags.append(0.0)
            continue
        _, hk = _peak_near(freqs, mag, target, 40.0)
        h_mags.append(hk)
        harmonics_db.append(float(_db(hk) - _db(h1_mag)) if h1_mag > 0 else -120.0)

    odd = [harmonics_db[k - 1] for k in (3, 5, 7, 9) if harmonics_db[k - 1] > -120.0]
    even = [harmonics_db[k - 1] for k in (2, 4, 6, 8) if harmonics_db[k - 1] > -120.0]
    odd_even = (float(np.mean(odd)) - float(np.mean(even))) if odd and even else 0.0

    # Spectral centroid (amplitude-weighted, 0-12 kHz).
    band = freqs <= 12000.0
    centroid = float(np.sum(freqs[band] * mag[band]) / max(np.sum(mag[band]), 1e-12))

    # Tonal-to-noise: power in ±50-cent bands around every harmonic vs the rest
    # of the 80 Hz - 16 kHz band.
    # The mask runs to the fortieth partial, where stiffness has moved the
    # partial far further than it moved the twelfth: at B=4e-4 the fortieth is
    # 427 cents sharp, four times outside its own ±50 cent band. Left on
    # integer multiples the mask files a stiff string's upper partials as noise
    # and reports the voice as noisier than it is — in a term that also carries
    # weight 1.0 by default, and in the one direction `tnr` penalises.
    power = mag**2
    harmonic_mask = np.zeros_like(freqs, dtype=bool)
    k = 1
    while k <= 40:
        target = partial_hz(f0, k, inharmonicity_b)
        if target >= min(16000.0, sr / 2):
            break
        lo = target * 2.0 ** (-50.0 / 1200.0)
        hi = target * 2.0 ** (50.0 / 1200.0)
        harmonic_mask |= (freqs >= lo) & (freqs <= hi)
        k += 1
    band = (freqs >= 80.0) & (freqs <= 16000.0)
    p_harm = float(np.sum(power[band & harmonic_mask]))
    p_noise = float(np.sum(power[band & ~harmonic_mask]))
    tnr = 10.0 * np.log10(max(p_harm, 1e-12) / max(p_noise, 1e-12))

    # Envelope metrics.
    seg_end = min(int(render_end * sr), len(mono))
    times, env = _rms_envelope(mono[on:seg_end], sr)

    note_dur = note.dur
    attack_region = env[times <= min(0.5, note_dur)]
    peak = float(np.max(attack_region)) if attack_region.size else float(np.max(env))
    attack_ms = 0.0
    if peak > 0:
        above10 = np.where(env >= 0.1 * peak)[0]
        above90 = np.where(env >= 0.9 * peak)[0]
        if above10.size and above90.size:
            attack_ms = max(0.0, (times[above90[0]] - times[above10[0]]) * 1000.0)

    sus_mask = (times >= 0.3 * note_dur) & (times <= 0.9 * note_dur)
    slope = 0.0
    sustain_rms_db = float(_db(np.sqrt(np.mean(sustain**2))))
    if np.count_nonzero(sus_mask) >= 4:
        t_s = times[sus_mask]
        e_db = np.asarray(_db(env[sus_mask]), dtype=np.float64)
        slope = float(np.polyfit(t_s, e_db, 1)[0])

    # Release: from the level just before note-off, time to fall 40 dB.
    off_t = note_dur
    pre_off = env[(times >= off_t - 0.1) & (times <= off_t)]
    release_ms = 0.0
    capped = False
    if pre_off.size:
        level_db = float(_db(np.median(pre_off)))
        after = times > off_t
        fallen = after & (np.asarray(_db(env), dtype=np.float64) <= level_db - 40.0)
        if np.any(fallen):
            release_ms = (times[np.argmax(fallen)] - off_t) * 1000.0
        else:
            release_ms = (times[-1] - off_t) * 1000.0
            capped = True

    # The measurements added after the committed profiles were taken. All of
    # them anchor on the detected onset when one is supplied, and none of them
    # touches a field above.
    at = note.start if onset is None else onset
    fine_start = int(at * sr)
    fine_end = min(int((at + min(0.5, note_dur)) * sr), len(mono))
    attack_fine_ms = 0.0
    if fine_end - fine_start > int(0.004 * sr):
        f_times, f_env = _rms_envelope(mono[fine_start:fine_end], sr,
                                       hop_ms=HIT_ENVELOPE_HOP_MS,
                                       win_ms=HIT_ENVELOPE_WIN_MS)
        f_peak = float(np.max(f_env)) if f_env.size else 0.0
        if f_peak > 0:
            a10 = np.where(f_env >= 0.1 * f_peak)[0]
            a90 = np.where(f_env >= 0.9 * f_peak)[0]
            if a10.size and a90.size:
                attack_fine_ms = max(0.0, (f_times[a90[0]] - f_times[a10[0]]) * 1000.0)

    modal = modal_profile(freqs, mag, expected_f0)
    movement = modulation_note(mono, sr, note, float(f0), onset=onset)

    return NoteMetrics(
        note=note.note,
        velocity=note.velocity,
        f0_hz=float(f0),
        f0_cents_err=float(cents_err),
        harmonics_db=[round(h, 2) for h in harmonics_db],
        centroid_hz=round(centroid, 1),
        odd_even_db=round(odd_even, 2),
        tnr_db=round(float(tnr), 2),
        attack_ms=round(attack_ms, 1),
        sustain_slope_db_s=round(slope, 2),
        release_ms=round(release_ms, 1),
        release_capped=capped,
        sustain_rms_db=round(sustain_rms_db, 2),
        inharmonicity_b=round(inharmonicity_b, 7),
        inharmonicity_partials=inharmonicity_partials,
        modal_hz=modal["modal_hz"],
        modal_db=modal["modal_db"],
        modal_ratio=modal["modal_ratio"],
        ladder_partials=sum(ladder_present(harmonics_db)),
        attack_fine_ms=round(attack_fine_ms, 2),
        f0_width_cents=f0_width_cents(freqs, mag, float(f0)),
        **movement,
    )


# --------------------------------------------------------------------------- #
# Level, and the attack's high end
# --------------------------------------------------------------------------- #
# Where the held level is read, in seconds from the onset. Fixed rather than a
# fraction of the note, because the gate is a property of the probe and not of
# the instrument: on a corpus probe holding eight seconds, a 0.3-0.9 fraction
# reads 2.4 to 7.2 s in, which on the top octave is entirely after the note has
# stopped — the reference's own C8 is down 40 dB by half a second. Both sides
# then measure silence and agree perfectly about it.
HELD_WINDOW_S = (0.20, 1.20)
# Below this much under the note's own peak there is no note left in the window,
# so its level carries no information and is reported as absent rather than as a
# number near the arithmetic floor. Two renders whose held windows are both
# empty otherwise score as a perfect level match.
HELD_FLOOR_DB = 80.0


def level_of(raw: np.ndarray, sr: int, note: Note, window_end: float) -> dict:
    """Absolute level of one note, measured on audio no normalisation has touched.

    Every other metric here is deliberately level-blind — the harmonic ladder is
    h1-normalised, the band profile is normalised to its own loudest band, and
    both renders are scaled to a common RMS before any of it — because a timbre
    match is a question about shape. The consequence is that a voice can satisfy
    all of them and still be the wrong loudness in a register, or hold a note far
    too long after an attack of the right height, with nothing in the objective
    able to say so.

    Three numbers, deliberately including a difference that survives an unknown
    output-gain offset: peak, the RMS of the held part of the note, and the crest
    between them. Crest is the one that needs no calibration at all — a model
    whose peak is 3.9 dB over a reference while its held RMS is 8.7 dB over is
    not loud, it is a note that never falls after its attack, and that reads
    identically whatever gain either side was captured at.
    """
    on = int(note.start * sr)
    end = min(int(window_end * sr), len(raw))
    seg = raw[on:end]
    if len(seg) < 256:
        return {"peak_dbfs": None, "held_rms_dbfs": None, "held_crest_db": None}
    peak = float(np.max(np.abs(seg)))
    lo, hi = HELD_WINDOW_S
    held_a = int((note.start + lo) * sr)
    held_b = min(int((note.start + min(hi, note.dur)) * sr), end)
    if held_b - held_a < 256:
        # Too short to hold: fall back to the settled middle of whatever there
        # is, which is what the sustain metrics read on a staccato probe.
        held_a = int((note.start + 0.3 * note.dur) * sr)
        held_b = min(int((note.start + 0.9 * note.dur) * sr), end)
    held = raw[held_a:held_b] if held_b - held_a >= 256 else seg
    held_rms = float(np.sqrt(np.mean(held**2)))
    peak_db = float(_db(peak))
    held_db = float(_db(held_rms))
    if peak_db - held_db > HELD_FLOOR_DB:
        return {"peak_dbfs": round(peak_db, 2), "held_rms_dbfs": None,
                "held_crest_db": None}
    return {
        "peak_dbfs": round(peak_db, 2),
        "held_rms_dbfs": round(held_db, 2),
        # Named apart from the percussion set's own `crest_db`, which is a
        # different measurement of a different window on a normalised signal —
        # and which these fields are merged alongside on a drum probe.
        "held_crest_db": round(peak_db - held_db, 2),
    }


# The attack window, and how it is cut up. Six 20 ms slices covering the first
# 120 ms: long enough to reach past the hammer contact and the bloom, short
# enough that a burst confined to the first frame is not averaged away. A
# whole-timeline spectral distance cannot see one of these — a 43 ms excess of
# +53 dB at 20-24 kHz on a note that matches within half a dB everywhere else
# dilutes into the multi-scale term's average and never moves it.
ATTACK_WINDOW_MS = 20.0
ATTACK_WINDOWS = 6
# Bands above the last harmonic anyone models. This is where a strike-noise
# path with the wrong filter order announces itself: a single pole falls at
# 6 dB/octave, which no radiating mechanism does, and leaves a burst still
# 23 dB up at 16 kHz that the ear reads as a tick rather than as brightness.
ATTACK_BANDS_HZ = ((4000.0, 8000.0), (8000.0, 12000.0), (12000.0, 16000.0),
                   (16000.0, 20000.0), (20000.0, 24000.0))

# What the low attack bands are measured against, and why it is not the
# window's own broadband level.
#
# A share of the total is compositional: the bands sum to unity, so whichever
# band is loudest sets the denominator for all of them and a defect confined to
# one band fabricates a delta in every other. Measured on a band-limited 40 Hz
# excess with nothing else wrong, the three bands from 200 Hz up each reported
# 4.92 dB of difference that was not there; against this anchor they report
# exactly zero, and the band the defect is actually in reports 48.
#
# 200 Hz to 4 kHz holds real signal for every note on the keyboard — the eighth
# through hundred-and-forty-fifth partial of an A0, the first and second of a
# C7 — and sits above the bands a bass defect lives in, so that defect cannot
# move its own reference. It is a partition point rather than an exclusion: the
# three bands inside it are read as a share OF it, which is meaningful, since
# only a band identical to the anchor would be degenerate.
#
# `attack_bands` deliberately does not use it — see the note there. Its 20 ms
# slice is shorter than one cycle of the frequencies this anchor is meant to be
# independent of, so the leakage lands in the anchor and the fix inverts.
ATTACK_ANCHOR_HZ = (200.0, 4000.0)


def _anchor_power(freqs: np.ndarray, power: np.ndarray, sr: int) -> float:
    """Power in the reference band every attack measure is expressed against."""
    lo, hi = ATTACK_ANCHOR_HZ
    if lo >= sr / 2:
        return 0.0
    mask = (freqs >= lo) & (freqs < min(hi, sr / 2))
    return float(power[mask].sum())


def _bands_against_anchor(freqs: np.ndarray, power: np.ndarray, sr: int,
                          bands, anchor: float) -> list[float | None]:
    """Each band's level relative to `anchor`, in dB; None above Nyquist."""
    out: list[float | None] = []
    for lo, hi in bands:
        if lo >= sr / 2:
            out.append(None)
            continue
        mask = (freqs >= lo) & (freqs < min(hi, sr / 2))
        ratio = float(power[mask].sum()) / anchor
        out.append(round(float(10.0 * np.log10(max(ratio, 1e-12))), 2))
    return out


def attack_bands(mono: np.ndarray, sr: int, note: Note,
                 onset: float) -> list[float | None]:
    """High-band balance through the attack: one value per band per time slice.

    Each value is the band's level relative to that slice's own broadband level,
    so the measure says nothing about how loud the attack was and everything
    about its spectral tilt. That is what makes it usable on the RMS-normalised
    signal the rest of the metric set reads, and what makes it comparable
    between a model and a reference captured at different gains.

    **Not the shared anchor `attack_low_bands` uses, and the difference is the
    window rather than a preference.** A share of the total is compositional, so
    a defect in one band does move the others, and an anchor outside both band
    sets is what fixes that — at 50 ms. It does not survive a 20 ms slice: a
    slice is shorter than one cycle of the frequencies the low measure is
    watching, so a 40 Hz excess leaks straight into a 200 Hz - 4 kHz anchor and
    contaminates the reference it was supposed to be independent of. Measured on
    a band-limited low-frequency defect, anchoring made this term's worst band
    move 6.83 dB where the share of the total moved 1.18. The anchor is kept
    where it works and not carried here for symmetry.

    `onset` is where the attack actually begins, which is not necessarily where
    the note was scheduled — see `note_onset`. A slice is 20 ms, so a model that
    speaks 30 ms after its note-on would otherwise have its first two slices
    compared against a reference's silence.

    Bands above Nyquist and slices past the end of the render come back None
    rather than as a floor value, so a shorter render contributes nothing to the
    term instead of contributing a fabricated match.
    """
    win = int(sr * ATTACK_WINDOW_MS / 1000.0)
    on = int(onset * sr)
    out: list[float | None] = []
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    window = np.hanning(win)
    for i in range(ATTACK_WINDOWS):
        a = on + i * win
        seg = mono[a : a + win]
        if len(seg) < win:
            out.extend([None] * len(ATTACK_BANDS_HZ))
            continue
        power = np.abs(np.fft.rfft(seg * window)) ** 2
        total = float(power.sum())
        if total <= 0.0:
            out.extend([None] * len(ATTACK_BANDS_HZ))
            continue
        out.extend(_bands_against_anchor(freqs, power, sr, ATTACK_BANDS_HZ, total))
    return out


# The attack's low end, and why it is one window rather than six slices. The
# high-band measure above trades frequency resolution for time resolution,
# which is the right trade from 4 kHz up: a tick is a burst confined to one
# slice, and 50 Hz bins are ample there. At the bottom of the range that trade
# inverts. The two bands that carry a bass note's attack — 20-60 and 60-200 Hz
# — are one bin and three at a 20 ms slice's resolution, so they cannot be told
# apart at all. A single 50 ms window resolves 20 Hz instead, and is still
# short enough that a 30 ms strike event dominates it rather than averaging
# into the sustain behind it.
ATTACK_LF_WINDOW_MS = 50.0
# Bands from the bottom of the audible range up to where ATTACK_BANDS_HZ takes
# over, so the two measures tile the spectrum without either one paying for the
# same error twice. DC sits outside the lowest band deliberately: a constant
# offset is a property of whatever captured the signal, not of the instrument.
#
# The lowest band contains the fundamental on the bottom octave — A0 is 27.5 Hz
# — and that is not an oversight. This is the attack's band balance, not a
# sub-fundamental measure, and the bottom octave is exactly where the defect it
# exists to catch was found: a bass note carrying 43 dB more 20-60 Hz than its
# reference while sitting 15-20 dB under it above 200 Hz, which is a note that
# is felt and never heard. Nothing else here could see it. The harmonic ladder
# is h1-normalised, so an excess at h1 is invisible by construction; every
# other pitched metric reads the sustain window, which a strike event is over
# before it opens; and a whole-timeline spectral distance averages a 50 ms
# event away.
# The two bands below the anchor are the measurement; the three inside it are a
# partition of the anchor itself, which is not degenerate — a band is only
# self-referential if it IS the anchor — and says how the mid weight is
# distributed. Only a band that equals ATTACK_ANCHOR_HZ would have to go.
ATTACK_LF_BANDS_HZ = ((20.0, 60.0), (60.0, 200.0), (200.0, 800.0),
                      (800.0, 2000.0), (2000.0, 4000.0))


def attack_low_bands(mono: np.ndarray, sr: int, note: Note,
                     onset: float) -> list[float | None]:
    """Low- and mid-band balance through the attack: one value per band.

    Each value is the band's level relative to `ATTACK_ANCHOR_HZ`, exactly as
    `attack_bands` reports the top end and against the same reference, so the
    two measures are commensurate and neither prices the other's defect. The
    measure says nothing about how loud the attack was and everything about its
    spectral tilt, which is what makes it usable on the RMS-normalised signal
    the rest of the metric set reads.

    `onset` is where the attack actually begins — see `note_onset`.

    A render too short to fill the window, one with nothing in the anchor band,
    and a band above Nyquist all come back None rather than as a floor value,
    so they contribute nothing to the term instead of contributing a fabricated
    match.
    """
    win = int(sr * ATTACK_LF_WINDOW_MS / 1000.0)
    on = int(onset * sr)
    seg = mono[on : on + win]
    if len(seg) < win:
        return [None] * len(ATTACK_LF_BANDS_HZ)
    power = np.abs(np.fft.rfft(seg * np.hanning(win))) ** 2
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    anchor = _anchor_power(freqs, power, sr)
    if anchor <= 0.0:
        return [None] * len(ATTACK_LF_BANDS_HZ)
    return _bands_against_anchor(freqs, power, sr, ATTACK_LF_BANDS_HZ, anchor)


# What a narrowband ring in the attack is, and why it needs its own measure
# rather than another band.
#
# `attack_bands` prices the top end four kilohertz at a time, which is the right
# width for a tilt and the wrong one for a mode: a single lightly-damped
# resonance rung by the strike is a spike a few tens of hertz wide, and a band
# average reports it as a few decibels of extra brightness spread over the whole
# band. The term still charges for it — on this piano it charges the cap — but
# the number it produces says "the 8-12 kHz band is hot", which is not something
# anyone can act on.
#
# So this is a diagnostic and deliberately NOT a loss term. The energy is
# already priced by `hf`; pricing it twice would let one ring outvote the rest
# of the attack, and a peak list is a jumpy thing to hand an optimiser besides.
# What it adds is attribution: which frequency, how far above its neighbours,
# and — via `fixed_resonances` in `loss.py` — whether it sits at the same place
# on every note, which is what separates a resonator mode from a partial.
#
# Set at 40 Hz rather than at the top of the modelled harmonic range, because
# the ring this exists to find is not always up there. A piano's case and frame
# modes live between 40 Hz and 5 kHz, so a 4 kHz floor put the whole of the
# instrument's own structure out of range and left the diagnostic able to see
# only fold-back and undamped filters. Lowering it does not cost precision:
# against a sampled reference the notes below 4 kHz that survive every other
# gate came back four in number and on the reference side only.
ATTACK_PEAK_FLOOR_HZ = 40.0
# The baseline each peak is measured against is the spectrum smoothed over this
# span. Wide enough that a mode cannot lift its own baseline, narrow enough to
# follow the instrument's real rolloff rather than averaging across it.
ATTACK_PEAK_BASELINE_HZ = 2000.0
# How far above that baseline a bin has to stand to be called a peak. A partial
# on a dense low note clears 10 dB routinely; at 15 dB what survives on a piano
# is a partial that dominates its neighbours or a mode that has no business
# being there at all, and the two are told apart by pitch-independence.
ATTACK_PEAK_PROMINENCE_DB = 15.0
# How far below the window's own loudest bin a peak may sit and still be one.
# Prominence is a ratio against the neighbourhood, which says nothing about
# whether either has any signal in it: in a band that is numerically empty the
# smoothed baseline is round-off and anything above it reports tens of decibels
# of prominence over nothing. Measured on a synthetic string whose spectrum is
# genuinely silent between partials, that produced peaks at 80 dB prominence in
# bands 250 dB below the fundamental. Real renders carry a noise floor that
# hides this, which is exactly why it needed a guard rather than a test signal.
ATTACK_PEAK_FLOOR_DB = 80.0


def attack_peaks(mono: np.ndarray, sr: int, note: Note,
                 onset: float) -> list[tuple[float, float]]:
    """Isolated narrowband peaks in the attack, as (frequency Hz, prominence dB).

    Measured over the same span `attack_bands` covers, from the same detected
    `onset`, so a peak found here is a peak that measure is also charging for.
    Prominence is the excess over the locally smoothed spectrum, which makes it
    blind to the overall level and to the instrument's own rolloff — a peak is
    only a peak relative to what sits beside it.

    Returns an empty list when the window cannot be filled, rather than a
    fabricated one: no data and no peaks are different findings, and the caller
    counts notes so it can tell them apart.
    """
    win = int(sr * ATTACK_WINDOW_MS / 1000.0) * ATTACK_WINDOWS
    on = int(onset * sr)
    seg = mono[on : on + win]
    if len(seg) < win:
        return []
    power = np.abs(np.fft.rfft(seg * np.hanning(win))) ** 2
    if float(power.sum()) <= 0.0:
        return []
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    db = 10.0 * np.log10(power + 1e-30)
    bin_hz = float(freqs[1] - freqs[0])
    half = max(1, int(round(ATTACK_PEAK_BASELINE_HZ / bin_hz / 2.0)))
    kernel = np.ones(2 * half + 1)
    # Divided by how many bins each output actually saw, rather than by the
    # kernel width. A plain smoothing pads the ends with zeros, and these are dB
    # — every real value is negative — so the padding pulls the baseline UP
    # towards 0 and the excess down with it. The span is 2 kHz wide, so at the
    # bottom of the spectrum that is most of the window: with the floor at 4 kHz
    # nothing was measured there and it did not matter, and at 40 Hz the first
    # 240 bins would each be judged against a baseline made mostly of padding.
    counts = np.convolve(np.ones_like(db), kernel, mode="same")
    baseline = np.convolve(db, kernel, mode="same") / counts
    excess = db - baseline
    # A bin in an empty band is not a peak however far it stands over its
    # neighbours; see ATTACK_PEAK_FLOOR_DB.
    excess[db < float(db.max()) - ATTACK_PEAK_FLOOR_DB] = 0.0
    lo = int(np.searchsorted(freqs, ATTACK_PEAK_FLOOR_HZ))
    hi = len(freqs) - half            # the smoothing tapers at the very top
    out: list[tuple[float, float]] = []
    i = lo
    while i < hi:
        if excess[i] < ATTACK_PEAK_PROMINENCE_DB:
            i += 1
            continue
        # Walk out to where the peak falls back to two thirds of the threshold,
        # so one resonance is reported once rather than once per bin over it.
        start = i
        while i < hi and excess[i] > ATTACK_PEAK_PROMINENCE_DB * (2.0 / 3.0):
            i += 1
        top = start + int(np.argmax(excess[start:i]))
        out.append((float(freqs[top]), float(excess[top])))
    return out


# --------------------------------------------------------------------------- #
# Percussion
# --------------------------------------------------------------------------- #
def _band_power(freqs: np.ndarray, power: np.ndarray, centers, ratio: float) -> np.ndarray:
    """Power summed into each band around `centers`, half-band width `ratio`."""
    out = np.empty(len(centers))
    for i, c in enumerate(centers):
        mask = (freqs >= c / ratio) & (freqs < c * ratio)
        out[i] = float(power[mask].sum())
    return out


# How a band's decay rate is estimated, and why not by fitting the log envelope.
#
# The previous estimator regressed the raw log-magnitude series from its peak
# frame down to 25 dB below it. On a noisy transient that is dominated by which
# frame the peak happened to land in, and the reference says so: measured across
# `reference/drums.json`, the SAME physical instrument struck at six velocities
# gives band decay rates spanning a median of 43 to 249 dB/s depending on the
# band, and a 90th percentile over 1200 dB/s. The loss caps the model-vs-
# reference difference at 60 dB/s. So the reference's own strike-to-strike
# variation already exceeded the cap in most bands: `bdecay` was a saturated
# constant with no gradient in it, not a measurement of anything.
#
# A Schroeder backward integration is the standard answer and it is already in
# this tree — `room.py` uses it to measure reverberation. Integrating the
# remaining energy from each moment forward removes exactly the frame-to-frame
# ripple that decided the old fit, and the -5..-25 dB span is the T20 convention
# for the same reason: the first 5 dB is the strike rather than the decay, and
# the bottom of the range is where the recording's floor takes over.
#
# The estimate also has to be able to refuse. A band with no decay in it, or one
# whose energy curve is not a line, returns None rather than a slope, which the
# loss already treats as an absent reference value.
EDC_FIT_RANGE_DB = (-5.0, -25.0)
#: How far the curve has to fall before a slope through it is a measurement.
#: Under this the fit spans too little to separate a rate from an offset.
EDC_MIN_DROP_DB = 12.0
#: How straight the energy decay curve has to be. A real decay is close to a
#: line in dB; a band that is being re-excited, or that is mostly floor, is not,
#: and the value fitted to it is a number rather than a rate.
EDC_MIN_R2 = 0.90
EDC_MIN_FRAMES = 6


def _edc_slope(series_power: np.ndarray, times: np.ndarray) -> tuple[float, float] | None:
    """Decay rate in dB/s from a Schroeder backward integration -> (slope, r2).

    `series_power` is a band's power per frame, already trimmed to start at that
    band's own peak. None when the curve does not fall far enough, has too few
    frames, or is not straight enough to have a rate.
    """
    if len(series_power) < EDC_MIN_FRAMES:
        return None
    edc = np.cumsum(series_power[::-1])[::-1]
    if edc[0] <= 0.0:
        return None
    edc_db = 10.0 * np.log10(np.maximum(edc / edc[0], 1e-30))
    total_drop = edc_db[0] - edc_db[-1]
    if total_drop < EDC_MIN_DROP_DB:
        return None
    hi, lo = EDC_FIT_RANGE_DB
    # Where the curve is genuinely shallower than the nominal span, fit whatever
    # it does cover rather than refusing: a short window on a fast band is the
    # common case and the -25 dB point simply is not in it.
    lo = max(lo, edc_db[-1] + 1.0)
    mask = (edc_db <= hi) & (edc_db >= lo)
    if np.count_nonzero(mask) < 4:
        return None
    t, y = times[mask], edc_db[mask]
    slope, intercept = np.polyfit(t, y, 1)
    resid = y - (slope * t + intercept)
    ss_res = float(np.sum(resid**2))
    ss_tot = float(np.sum((y - y.mean()) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0.0 else 0.0
    if r2 < EDC_MIN_R2:
        return None
    return float(slope), float(r2)


def _band_decay(
    seg: np.ndarray, sr: int, centers, ratio: float,
    *, n_fft: int = 1024, hop: int = 256,
) -> list[float | None]:
    """Post-peak decay rate in dB/s for each band, or None where unfittable.

    A percussion hit is a decay from the first sample, so each band is measured
    from its own peak frame downward — bands do not peak together, and a wire
    buzz that arrives after the shell would read as a rising slope if they were
    all anchored on the broadband onset.

    The rate itself comes from a Schroeder backward integration rather than from
    a regression on the log envelope; see `_edc_slope` for the measurement that
    made that necessary.
    """
    if len(seg) < n_fft:
        return [None] * len(centers)
    n_frames = (len(seg) - n_fft) // hop + 1
    frames = np.lib.stride_tricks.sliding_window_view(seg, n_fft)[::hop][:n_frames]
    power = np.abs(np.fft.rfft(frames * np.hanning(n_fft), axis=1)) ** 2
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    times = (np.arange(n_frames) * hop + n_fft / 2) / sr

    out: list[float | None] = []
    for c in centers:
        mask = (freqs >= c / ratio) & (freqs < c * ratio)
        if not mask.any():
            out.append(None)
            continue
        band = np.asarray(power[:, mask].sum(axis=1), dtype=np.float64)
        peak = int(np.argmax(band))
        got = _edc_slope(band[peak:], times[peak:] - times[peak])
        out.append(None if got is None else round(got[0], 2))
    return out


# --------------------------------------------------------------------------- #
# What a drum's pitch is, and why the band profile cannot see it
# --------------------------------------------------------------------------- #
# "A drum note has no fundamental" is true of a cymbal, a shaker and a snare,
# and false of most of a kit. Six toms, two congas, two bongos, two timbales,
# an agogo pair, a cowbell, a woodblock, a triangle and a taiko all have a
# definite pitch, and the kit's tuning — whether those six toms make an
# ascending series — is the first thing a drummer hears.
#
# The 1/3-octave profile cannot report it. A band is four semitones wide, so a
# tom two semitones out of tune moves `bands_db` barely at all. Meanwhile the
# model exposes the pitch directly: `base_freq_hz`, `mode_ratios[6]` and
# `pitch_drop` are all patch fields `--spec auto` offers, and until this existed
# a fit could move any of them and see almost nothing come back.
#
# The capture already knew. `capture/drums.json` records measured tom
# fundamentals (note 45 at 67-73 Hz, 47 at 73-83, 48 at 87-93, 50 at 93-97,
# 41 at 163-170, 43 at 183-190) in a note to the reader, because there was
# nowhere in the measurement path to put them.
#: Window and tolerance for `harmonic_share`. Half a second resolves a bin
#: every two hertz, which is a fifth of the tolerance at the bottom of the
#: ladder it is used on; sixty cents is wide enough for a patch tuned a few
#: cents off and far narrower than the gap between neighbouring partials.
HARMONIC_SHARE_WINDOW_S = 0.50
HARMONIC_SHARE_TOLERANCE_CENTS = 60.0


def harmonic_share(seg: np.ndarray, sr: int, f0_hz: float, *,
                   partials: int = N_HARMONICS) -> float | None:
    """How much of a segment's energy sits on the harmonic series of `f0_hz`.

    The question this answers is not what pitch a sound has but whether it has
    the pitch it was SENT, which is what separates an instrument from a drum
    kit on a rack slot nobody can see into: a melodic slot answers a note number
    with that note's series whatever its patch layers on top, and a kit answers
    it with an instrument whose partials have no relation to the key.

    Asking it this way rather than by estimating a pitch and comparing is what
    makes it usable. A pitch estimator has to decide WHICH peak is the
    fundamental, and on a bright patch the strongest one is routinely the
    second or third partial — an ambiguity that reads as "does not track" on an
    instrument that tracks perfectly. A share needs no such decision.

    Reported against the power below the highest partial searched, so a dark
    instrument and a bright one are comparable; `None` when the segment is too
    short to resolve the tolerance.
    """
    n = min(len(seg), int(HARMONIC_SHARE_WINDOW_S * sr))
    if n < 4096 or f0_hz <= 0.0:
        return None
    freqs, mag = _spectrum(np.asarray(seg[:n], dtype=np.float64), sr)
    power = mag**2
    ceiling = f0_hz * (partials + 0.5)
    band = (freqs > 0.0) & (freqs <= min(ceiling, sr / 2.0))
    total = float(np.sum(power[band]))
    if total <= 0.0:
        return None
    span = 2.0 ** (HARMONIC_SHARE_TOLERANCE_CENTS / 1200.0)
    on = np.zeros_like(band)
    for k in range(1, partials + 1):
        centre = f0_hz * k
        if centre > sr / 2.0:
            break
        on |= (freqs >= centre / span) & (freqs <= centre * span)
    return float(np.sum(power[on & band]) / total)


HIT_TONE_WINDOW_S = 0.30
#: How far the strike's pitch overshoot is tracked, and on what grid. A membrane
#: released from the strike falls back through a time constant of a few tens of
#: milliseconds, so the window has to be short and the hop finer than it.
PITCH_DROP_WINDOW_S = 0.30
PITCH_DROP_FRAME_S = 0.030
PITCH_DROP_HOP_S = 0.005
#: How far above and below the settled pitch the tracker looks. A kick's
#: overshoot is commonly half an octave, so a narrow window would clip it and
#: report a drum with no drop at all.
PITCH_DROP_SPAN = (0.6, 2.6)
PITCH_DROP_POINTS = 61
#: How far under the tone's own peak a frame may sit and still steer the track.
PITCH_DROP_FLOOR_DB = 24.0


def hit_tone(seg: np.ndarray, sr: int) -> dict:
    """The tonal part of a percussion hit: its modes and its pitch.

    Measured over the first `HIT_TONE_WINDOW_S` rather than the whole hit,
    because a membrane's modes are clearest before the noise layer has decayed
    past them and long after that the band is the room.

    Two different pitches, because the two questions have different answers on
    a real kit:

    `tone_f0_hz` is the STRONGEST mode — the pitch a listener assigns. Taking
    the lowest instead was tried and is wrong on exactly the drums it matters
    for. Measured against `capture/drums.json`, whose `_toms` note records the
    six tom fundamentals by hand: the lowest-mode rule reproduces four of them
    and reports the two smallest toms at 55.8 and 61.7 Hz against hand
    measurements of 163-170 and 183-190, which inverts the kit's pitch order.
    Those two notes carry a low component within 2 dB of the head's tuned mode
    and at about a third of its frequency — a shell or air resonance, or the
    floor tom in the same room — and it is not what anyone hears as the pitch.
    The strongest-mode rule reproduces all six.

    `tone_lowest_hz` is the lowest mode, kept because it is a different fact
    and one of them was going to be needed.

    `modal_ratio` stays against the LOWEST mode, since that is the column that
    reads directly against the patch's `mode_ratios` — an ideal circular head
    is 1 : 1.59 : 2.14 : 2.30 : 2.65, and those ratios are to the fundamental.
    """
    empty = {"modal_hz": [], "modal_db": [], "modal_ratio": [],
             "tone_f0_hz": None, "tone_lowest_hz": None}
    n = min(len(seg), int(HIT_TONE_WINDOW_S * sr))
    if n < 512:
        return empty
    freqs, mag = _spectrum(np.asarray(seg[:n], dtype=np.float64), sr)
    modes = measure_modes(freqs, mag)
    if not modes:
        return empty
    lowest = modes[0][0]
    strongest = max(modes, key=lambda m: m[1])[0]
    return {
        "modal_hz": [hz for hz, _ in modes],
        "modal_db": [db for _, db in modes],
        "modal_ratio": [round(hz / lowest, 4) for hz, _ in modes],
        "tone_f0_hz": round(strongest, 2),
        "tone_lowest_hz": round(lowest, 2),
    }


def pitch_drop(seg: np.ndarray, sr: int, f0: float | None) -> dict:
    """The strike's pitch overshoot: how far it starts sharp and how fast it falls.

    A struck head is stretched by the strike and relaxes, so the tone starts
    above its settled pitch and falls back — which is the difference between a
    kick drum and a sine blip, and which `PercussionPatchParams::pitch_drop`
    exists to produce. Nothing in this harness measured it, so the knob was
    classified into the excitation stage of a staged fit and scored only through
    whatever the 1/3-octave profile happened to notice.

    Returns the ratio of the starting frequency to the settled one and the time
    constant of the fall. Both None when there is no trackable tone, which is
    every cymbal and every shaker — an absence, not a drum with a static pitch.
    """
    empty = {"pitch_drop_ratio": None, "pitch_drop_ms": None}
    if not f0 or f0 <= 0.0 or f0 > sr / 2.5:
        return empty
    win_n = int(PITCH_DROP_FRAME_S * sr)
    hop = int(PITCH_DROP_HOP_S * sr)
    end = min(len(seg), int(PITCH_DROP_WINDOW_S * sr))
    n_frames = (end - win_n) // hop + 1 if end >= win_n else 0
    if n_frames < 8:
        return empty
    frames = np.lib.stride_tricks.sliding_window_view(
        np.asarray(seg[:end], dtype=np.float64), win_n)[::hop][:n_frames]
    frames = frames * np.hanning(win_n)
    cand = np.linspace(f0 * PITCH_DROP_SPAN[0], f0 * PITCH_DROP_SPAN[1],
                       PITCH_DROP_POINTS)
    t = np.arange(win_n) / sr
    amps = np.abs(frames @ np.exp(-2j * np.pi * np.outer(cand, t)).T)
    k = np.argmax(amps, axis=1)
    level = amps[np.arange(n_frames), k]
    if float(level.max()) <= 0.0:
        return empty
    keep = level >= float(level.max()) * 10.0 ** (-PITCH_DROP_FLOOR_DB / 20.0)
    if np.count_nonzero(keep) < 6:
        return empty
    track = cand[k]
    idx = np.where(keep)[0]
    settled = float(np.median(track[idx[len(idx) // 2:]]))
    start = float(track[idx[0]])
    if settled <= 0.0:
        return empty
    ratio = start / settled
    # Time for the excess over the settled pitch to fall to 1/e of what it
    # started at. Undefined when there is no excess to fall.
    ms = None
    excess = track[idx] - settled
    if excess[0] > settled * 0.01:
        target = excess[0] / math.e
        below = np.where(excess <= target)[0]
        if below.size:
            ms = float((idx[below[0]] - idx[0]) * PITCH_DROP_HOP_S * 1000.0)
    return {"pitch_drop_ratio": round(ratio, 4),
            "pitch_drop_ms": None if ms is None else round(ms, 1)}


@dataclass
class HitMetrics:
    """One percussion hit, measured with and without reference to a pitch."""

    note: int
    velocity: int
    bands_db: list[float]                # 1/3-octave, dB relative to the loudest band
    peak_band_hz: float
    band_decay_db_s: list[float | None]  # per octave band
    centroid_hz: float
    onset_ms: float                      # strike, relative to the note-on
    attack_ms: float
    decay_ms: float
    decay_capped: bool
    crest_db: float
    level_db: float
    #: The tonal part, for the two thirds of a kit that has one. Empty for a
    #: cymbal or a shaker, which is an absence rather than a pitch of zero.
    modal_hz: list[float]
    modal_db: list[float]
    modal_ratio: list[float]
    tone_f0_hz: float | None
    tone_lowest_hz: float | None
    pitch_drop_ratio: float | None
    pitch_drop_ms: float | None

    def to_dict(self) -> dict:
        return asdict(self)


def _hit_onset(mono: np.ndarray, sr: int, start: float, limit: float) -> float:
    """Where the strike actually begins, in seconds, at or after `start`.

    Located by walking back from the loudest moment to the last frame under
    `HIT_ONSET_FLOOR_DB`, so a piece whose envelope genuinely swells (a crash, a
    vibraslap's rattle) keeps its real onset rather than being cut to its peak.
    Only the first `HIT_ONSET_SEARCH_SEC` is searched: past that the loudest
    thing in the window is more likely to be the next event than this one.

    Falls back to `start` when nothing rises above the floor, which is what a
    silent render gives and what the caller already handles.
    """
    scan_end = int(min(limit, start + HIT_ONSET_SEARCH_SEC) * sr)
    scan = np.asarray(mono[int(start * sr):min(scan_end, len(mono))], dtype=np.float64)
    if len(scan) < 2:
        return start
    times, env = _rms_envelope(scan, sr, hop_ms=HIT_ENVELOPE_HOP_MS,
                               win_ms=HIT_ENVELOPE_WIN_MS)
    peak_i = int(np.argmax(env))
    floor = float(env[peak_i]) * 10.0 ** (HIT_ONSET_FLOOR_DB / 20.0)
    if floor <= 0.0:
        return start
    below = np.where(env[: peak_i + 1] <= floor)[0]
    return start + float(times[int(below[-1])]) if below.size else start


def note_onset(mono: np.ndarray, sr: int, note: Note, window_end: float) -> float:
    """Where a pitched note actually starts sounding, in seconds.

    The same search the percussion set has always run, applied to the pitched
    one, and for the same reason: the attack measures cut the first 120 ms into
    20 ms slices, so a model that speaks 30 ms after its note-on has its first
    two slices compared against a reference's silence and reports a spectral
    difference that is really a timing one.

    This deliberately does NOT hide the timing difference. `analyze_note`'s
    `attack_ms` is a rise time — a duration between two envelope crossings, not
    a delay from the note-on — and stays what it was, while the delay itself is
    carried on the row as `onset_ms`. Aligning the windows and reporting the
    offset separates two things the scheduled anchor conflated; anchoring on
    the score alone measured neither of them cleanly.
    """
    return _hit_onset(mono, sr, note.start, window_end)


#: How far a band's across-instrument spread may fall below the capture's own
#: typical spread and still be called informative. Half: a band that separates
#: the kit half as well as the capture does on average is degraded but is still
#: answering the question, and one that separates it less than that is mostly
#: reporting a shared transfer function.
BAND_EDGE_MIN_SPREAD_FRACTION = 0.5
#: Fewest distinct rows a spread can be read from. Two instruments differ or
#: they do not; it takes a handful before "how much do they differ" is a
#: measurement rather than a pair.
BAND_EDGE_MIN_ROWS = 8


def measure_band_edge(rows: list[dict]) -> float | None:
    """The highest 1/3-octave band a capture still tells instruments apart in, in Hz.

    A sampled reference has a bandwidth, and it is not the analysis range. What
    marks the end of it is not where the energy stops — a capture rolling off
    still has energy above its edge — but where the band stops DISCRIMINATING.
    Below the edge a crash, a cowbell, a closed hi-hat and a cabasa read tens of
    dB apart because they are different objects; at and above it they converge,
    because what is left is one shared transfer function rather than four
    instruments. Measured on `reference/drums.json`, the across-instrument
    spread holds between 21 and 30 dB from 63 Hz to 8 kHz, falls to 9 dB at
    10 kHz and to nothing at 12.5 kHz.

    An energy test cannot find that boundary. 57 % of that capture's rows are
    still above the band floor at 8 kHz and 36 % at 10 kHz, so a floor count
    puts the edge wherever the threshold was chosen; the spread collapses at one
    place and says so.

    Nothing above the edge is evidence about the instrument, so nothing above it
    should be scored or normalised against: a model with a real cymbal wash is
    charged the cap in bands the reference cannot resolve, and if the model's
    own loudest band lands up there, the profile it is normalised against shifts
    and every OTHER band's reading moves with it. The second one is why this
    cannot be left to the loss to skip.

    This reads a set of DIFFERENT instruments, which is what a percussion
    capture is. A pitched capture is one instrument at many notes, where a
    collapsing spread is a property of the register rather than of the chain, so
    the caller applies this to the percussion metric set only.

    Returned as a band centre so it can be compared against
    `THIRD_OCTAVE_CENTERS` without a tolerance. `None` when the capture carries
    its whole range, or when there are too few rows to read a spread from.
    """
    profiles = [r.get("bands_db") for r in rows or []]
    profiles = [p for p in profiles if p and len(p) == len(THIRD_OCTAVE_CENTERS)]
    if len(profiles) < BAND_EDGE_MIN_ROWS:
        return None
    columns = np.asarray(profiles, dtype=np.float64)
    spread = np.percentile(columns, 75, axis=0) - np.percentile(columns, 25, axis=0)
    # The capture's own typical separation, taken over every band rather than
    # over a hand-picked "good" region: picking the region would be choosing the
    # answer, since the region is what the edge is being measured against.
    floor = float(np.median(spread)) * BAND_EDGE_MIN_SPREAD_FRACTION
    if floor <= 0.0:
        return None
    # Walked from the top down and stopped at the first band that discriminates.
    # The edge is where a roll-off begins, so it is the highest CONTIGUOUS
    # informative band; a lone wide band above a collapsed one is a resonance in
    # the capture path, not a return of bandwidth.
    for i in range(len(THIRD_OCTAVE_CENTERS) - 1, -1, -1):
        if spread[i] >= floor:
            if i == len(THIRD_OCTAVE_CENTERS) - 1:
                return None
            return float(THIRD_OCTAVE_CENTERS[i])
    return None


def band_edge_index(max_band_hz: float | None) -> int:
    """How many 1/3-octave bands sit at or below `max_band_hz`."""
    if max_band_hz is None:
        return len(THIRD_OCTAVE_CENTERS)
    return sum(1 for c in THIRD_OCTAVE_CENTERS if c <= max_band_hz + 1e-6)


def analyze_hit(mono: np.ndarray, sr: int, note: Note, window_end: float, *,
                max_band_hz: float | None = None) -> HitMetrics:
    """Compute the percussion metric set for one hit.

    The window runs from the strike to `window_end` (the next hit, or the end of
    the render), capped at `HIT_MAX_SEC`. The note's own duration is ignored:
    a drum is a one-shot and its note-off carries no information.

    The strike is located rather than assumed to be at the note-on, because a
    hosted plugin's is not: leading silence inside the window inflates time to
    peak by exactly its own length, dilutes the RMS the crest and level are
    measured against, and tilts every per-band decay fit.

    `max_band_hz` is the reference's own measurable ceiling (`measure_band_edge`).
    Bands above it are reported at the floor and, more importantly, are excluded
    from the normalisation, so the profile below the edge does not move when the
    model has content the capture could not have recorded. Both sides of a
    comparison must be measured with the same value or the profiles are
    normalised against different things; `peak_band_hz` deliberately stays
    full-range, because a wash that peaks above the reference's ceiling is
    exactly what that field exists to show.
    """
    onset = _hit_onset(mono, sr, note.start, window_end)
    on = int(onset * sr)
    ceiling = HIT_LONG_MAX_SEC if note.note in LONG_DECAY_DRUM_NOTES else HIT_MAX_SEC
    end = int(min(window_end, onset + ceiling) * sr)
    seg = np.asarray(mono[on:min(end, len(mono))], dtype=np.float64)
    if len(seg) < 256:
        seg = np.asarray(mono[on : on + 256], dtype=np.float64)

    freqs, mag = _spectrum(seg, sr)
    power = mag**2
    bands = _band_power(freqs, power, THIRD_OCTAVE_CENTERS, THIRD_OCTAVE_RATIO)
    bands_db = np.asarray(_db(np.sqrt(bands)), dtype=np.float64)
    keep = band_edge_index(max_band_hz)
    top = float(bands_db[:keep].max())
    bands_db = np.maximum(bands_db - top, BAND_FLOOR_DB)
    if keep < len(bands_db):
        bands_db[keep:] = BAND_FLOOR_DB
    peak_band = THIRD_OCTAVE_CENTERS[int(np.argmax(bands))]

    in_range = freqs <= 16000.0
    centroid = float(
        np.sum(freqs[in_range] * mag[in_range]) / max(np.sum(mag[in_range]), 1e-12)
    )

    times, env = _rms_envelope(
        seg, sr, hop_ms=HIT_ENVELOPE_HOP_MS, win_ms=HIT_ENVELOPE_WIN_MS
    )
    peak = float(np.max(env))
    reached = np.where(env >= peak * 10.0 ** (HIT_ATTACK_TOLERANCE_DB / 20.0))[0]
    attack_i = int(reached[0]) if reached.size else int(np.argmax(env))
    attack_ms = float(times[attack_i] * 1000.0)

    # Decay runs from the same moment the attack ended, not from wherever the
    # maximum happened to land: on a hit that plateaus, the maximum sits in the
    # middle of the plateau and the time it takes to fall is measured short by
    # however much of the plateau preceded it.
    #
    # Read as the last moment the hit was still above the threshold rather than
    # the first moment it dipped below. A 2 ms window on a noise wash crosses
    # -20 dB and comes back within one frame, and the open hi-hat that rings for
    # half a second reported 14 ms on three of its six velocities for that
    # reason alone.
    over = np.where(env[attack_i:] > peak * 10.0 ** (-20.0 / 20.0))[0]
    last_i = attack_i + int(over[-1]) if over.size else attack_i
    capped = last_i >= len(env) - 1
    decay_ms = float((times[last_i] - times[attack_i]) * 1000.0)

    rms = float(np.sqrt(np.mean(seg**2)))
    crest_db = float(_db(np.max(np.abs(seg))) - _db(rms))

    tone = hit_tone(seg, sr)
    drop = pitch_drop(seg, sr, tone["tone_f0_hz"])

    return HitMetrics(
        note=note.note,
        velocity=note.velocity,
        bands_db=[round(float(v), 2) for v in bands_db],
        peak_band_hz=peak_band,
        **tone,
        **drop,
        band_decay_db_s=[None if v is None else round(v, 2)
                         for v in _band_decay(seg, sr, OCTAVE_CENTERS, OCTAVE_RATIO)],
        centroid_hz=round(centroid, 1),
        onset_ms=round((onset - note.start) * 1000.0, 2),
        attack_ms=round(attack_ms, 2),
        decay_ms=round(decay_ms, 1),
        decay_capped=capped,
        crest_db=round(crest_db, 2),
        level_db=round(float(_db(rms)), 2),
    )


def compare_hit(model: HitMetrics, oracle: HitMetrics) -> dict:
    """Model-minus-oracle deltas for one percussion hit."""
    band_delta = [round(m - o, 2) for m, o in zip(model.bands_db, oracle.bands_db)]
    decay_delta = [
        round(m - o, 2) if m is not None and o is not None else None
        for m, o in zip(model.band_decay_db_s, oracle.band_decay_db_s)
    ]
    return {
        "note": model.note,
        "velocity": model.velocity,
        "bands_delta_db": band_delta,
        "band_decay_delta_db_s": decay_delta,
        "peak_band_ratio": round(model.peak_band_hz / max(oracle.peak_band_hz, 1e-9), 3),
        "centroid_delta_hz": round(model.centroid_hz - oracle.centroid_hz, 1),
        "attack_delta_ms": round(model.attack_ms - oracle.attack_ms, 2),
        "decay_delta_ms": round(model.decay_ms - oracle.decay_ms, 1),
        "crest_delta_db": round(model.crest_db - oracle.crest_db, 2),
        "level_delta_db": round(model.level_db - oracle.level_db, 2),
    }


def compare_note(model: NoteMetrics, oracle: NoteMetrics) -> dict:
    """Model-minus-oracle deltas for one note."""
    harm_delta = [
        round(m - o, 2) if m > -120.0 and o > -120.0 else None
        for m, o in zip(model.harmonics_db, oracle.harmonics_db)
    ]
    return {
        "note": model.note,
        "velocity": model.velocity,
        "f0_cents_delta": round(model.f0_cents_err - oracle.f0_cents_err, 1),
        "harmonics_delta_db": harm_delta,
        "centroid_delta_hz": round(model.centroid_hz - oracle.centroid_hz, 1),
        "odd_even_delta_db": round(model.odd_even_db - oracle.odd_even_db, 2),
        "tnr_delta_db": round(model.tnr_db - oracle.tnr_db, 2),
        "attack_delta_ms": round(model.attack_ms - oracle.attack_ms, 1),
        "sustain_slope_delta_db_s": round(model.sustain_slope_db_s - oracle.sustain_slope_db_s, 2),
        "release_delta_ms": round(model.release_ms - oracle.release_ms, 1),
        "level_delta_db": round(model.sustain_rms_db - oracle.sustain_rms_db, 2),
    }
