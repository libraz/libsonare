"""Harmonic-series fitting: stiffness, partial placement, ladder presence."""

from __future__ import annotations

import numpy as np
from metrics_modulation import (
    FM_LOWPASS_F0_FRACTION,
    FM_STOP_F0_FRACTION,
    Tremolo,
    Vibrato,
    demodulated_pitch,
    pitch_excursion,
    vibrato_extent,
)
from metrics_signal import N_HARMONICS, _peak_near, _spectrum

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


def fit_partial_series(
    freqs: np.ndarray, mag: np.ndarray, f0_seed: float, h1_mag: float, sr: int
) -> tuple[float, float, int]:
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


def estimate_inharmonicity_b(
    freqs: np.ndarray, mag: np.ndarray, f0: float, h1_mag: float, sr: int
) -> tuple[float, int]:
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


#: Top of the tone-to-noise band, and of the partials masked inside it.
TNR_TOP_HZ = 12000.0
#: Weakest FM sideband, relative to the unmodulated partial, that the mask keeps
#: as tone. Measured on synthetic 30-cent vibrato: at -40 dB TNR still read 4-6 dB low.
TNR_SIDEBAND_DB = -60.0
_BESSEL_TAU = np.linspace(0.0, np.pi, 513)


def fm_sideband_reach(beta: float, max_order: int) -> int:
    """Highest order k <= `max_order` whose |J_k(beta)| reaches `TNR_SIDEBAND_DB`, or 0."""
    if beta <= 0.0 or max_order < 1:
        return 0
    k = np.arange(1, max_order + 1)[:, None]
    jk = (
        np.trapezoid(np.cos(k * _BESSEL_TAU - beta * np.sin(_BESSEL_TAU)), _BESSEL_TAU, axis=1)
        / np.pi
    )
    above = np.nonzero(np.abs(jk) >= 10.0 ** (TNR_SIDEBAND_DB / 20.0))[0]
    return int(above[-1]) + 1 if above.size else 0


def am_sideband_reach(index: float, max_order: int) -> int:
    """Highest order k <= `max_order` whose I_k(index)/I_0(index) reaches `TNR_SIDEBAND_DB`, or 0.

    A level that swings sinusoidally in dB is exp(index·sin) in amplitude, whose
    sidebands are the modified Bessel functions' ratios to the carrier.
    """
    if index <= 0.0 or max_order < 1:
        return 0
    k = np.arange(0, max_order + 1)[:, None]
    ik = np.trapezoid(
        np.exp(index * (np.cos(_BESSEL_TAU) - 1.0)) * np.cos(k * _BESSEL_TAU), _BESSEL_TAU, axis=1
    )
    above = np.nonzero(np.abs(ik[1:]) >= ik[0] * 10.0 ** (TNR_SIDEBAND_DB / 20.0))[0]
    return int(above[-1]) + 1 if above.size else 0


def am_margin_hz(f0_hz: float, bin_hz: float, tremolo: Tremolo) -> float:
    """How far either side of every partial a tremolo's sidebands reach, in Hz.

    The same for every partial, since the level swings the whole note: three bins
    for the main lobe plus the rate times the highest order `am_sideband_reach`
    keeps, counted up to a quarter of f0. `tremolo.db` peak-to-peak is an index of
    db·ln(10)/40 nepers.
    """
    index = tremolo.db * np.log(10.0) / 40.0
    order = am_sideband_reach(index, int(np.ceil(0.25 * f0_hz / tremolo.rate_hz)))
    return 3.0 * bin_hz + tremolo.rate_hz * order


def pitch_motion(seg: np.ndarray, sr: int, f0: float) -> Vibrato | None:
    """The fundamental's motion over `seg` as one reading, or None when it holds still.

    `vibrato_extent`'s line or band where the track holds a vibrato, whose
    sidebands it places at the rates it resolves, and `pitch_excursion`'s
    spread where it does not, as a section's or a choir's aperiodic churn. A
    vibrato reads as it did before the excursion was measured; motion beside
    an accepted vibrato is not added to it.
    """
    vibrato = vibrato_extent(seg, sr, f0)
    return vibrato if vibrato is not None else pitch_excursion(seg, sr, f0)


def fm_margin_hz(target: float, f0_hz: float, bin_hz: float, motion: Vibrato | None) -> float:
    """How far either side of a partial at `target` its energy reaches, in Hz.

    Three bins hold a still partial's Hann main lobe. A pitch motion spreads
    partial n into sidebands at multiples of its rate, with index beta_n = n's
    peak deviation over the rate, and adds the rate times the highest sideband
    order still above `TNR_SIDEBAND_DB`, counted up to a quarter of f0. A rate
    that wanders puts the order its slowest rate reaches at its fastest rate's
    spacing.
    """
    margin = 3.0 * bin_hz
    if motion is None:
        return margin
    slow = motion.rate_hz
    beta = target * (2.0 ** (0.5 * motion.cents / 1200.0) - 1.0) / slow
    return margin + motion.rate_hi_hz * fm_sideband_reach(beta, int(np.ceil(0.25 * f0_hz / slow)))


#: A Hann window's noise bandwidth in bins: a still partial's power summed over its
#: lobe, over its peak bin's.
HANN_POWER_BINS = 1.5


def partial_level(
    freqs: np.ndarray, mag: np.ndarray, target: float, f0_hz: float, motion: Vibrato | None
) -> float:
    """A partial's magnitude as its power summed over its FM extent, on a still peak's scale.

    Under a pitch motion the peak bin is whichever Bessel line won, and it falls as
    the index rises, so upper partials read dark by the depth of the motion. The
    sidebands hold the partial's power between them, so it is summed over
    `fm_margin_hz` either side (under the quarter-f0 cap the tone-to-noise mask
    uses) and scaled by `HANN_POWER_BINS`, which returns a still on-bin partial's
    peak magnitude.
    """
    if freqs.size < 2:
        return 0.0
    bin_hz = float(freqs[1] - freqs[0])
    half = min(fm_margin_hz(target, f0_hz, bin_hz, motion), 0.25 * f0_hz)
    inside = (freqs >= target - half) & (freqs <= target + half)
    return float(np.sqrt(np.sum(mag[inside].astype(np.float64) ** 2) / HANN_POWER_BINS))


#: Passes `held_pitch` demodulates the fundamental for, each centred on the last
#: one's mean, so a start a sideband off the centre still ends on it.
HELD_PITCH_PASSES = 2


def held_pitch(seg: np.ndarray, sr: int, f0: float) -> tuple[float, Vibrato | None]:
    """The pitch `seg` holds, and its `pitch_motion`.

    Under a vibrato a spectral peak is whichever FM line won, so the pitch is
    the Hann-weighted mean of the fundamental's demodulated track. The
    demodulation passes only the fundamental's own FM extent (`fm_margin_hz`,
    under the quarter-f0 cap): over `vibrato_extent`'s wider passband the track
    follows a second component near the fundamental, and a lead whose detuned
    oscillators beat under one a fourth below read 22.8 cents flat. An
    excursion leaves the pitch at `f0`: its motion has no line a peak could
    have picked off-centre, and where it is two components beating the
    demodulated mean moves off the stronger one's pitch. It still returns as the
    motion, which widens the partial windows. Where the pitch holds still,
    `f0` is returned unchanged, so a still tone reads exactly as its peak did.
    """
    vibrato = vibrato_extent(seg, sr, f0)
    if vibrato is None:
        return f0, pitch_excursion(seg, sr, f0)
    bin_hz = sr / len(seg)
    centre = f0
    for _ in range(HELD_PITCH_PASSES):
        cutoff = min(fm_margin_hz(centre, centre, bin_hz, vibrato), 0.25 * centre)
        stop = cutoff * FM_STOP_F0_FRACTION / FM_LOWPASS_F0_FRACTION
        cents = demodulated_pitch(seg, sr, centre, cutoff, stop)
        if cents is None:
            break
        taper = np.hanning(len(cents))
        centre *= 2.0 ** (float(np.sum(cents * taper) / np.sum(taper)) / 1200.0)
    return centre, vibrato


def tone_to_noise_db(
    freqs: np.ndarray,
    mag: np.ndarray,
    f0_hz: float,
    b: float = 0.0,
    motion: Vibrato | None = None,
    tremolo: Tremolo | None = None,
) -> float:
    """Energy in the partials against everything else in the audible band.

    What it separates is the string from the mechanism: a plucked or struck
    instrument's noise floor is its own action — a plectrum scraping, a hammer's
    felt, a jack falling back — and a model that has the partial stack right and
    no mechanism noise reads far cleaner than any recording of the real thing.
    That reads as an improvement on every spectral metric and is the single most
    audible thing still missing.

    Every partial up to the band's top is masked, placed by the fitted stiffness
    `b`: a plucked string's spectrum above its sixteenth partial is still almost
    all partial, and counted as noise it read the string as its own mechanism.
    The band starts just under the fundamental, because the energy below it is
    the body, which `body_below_f0_db` already measures.

    The partial window is the wider of ±2 % and three FFT bins. A relative-only
    window collapses below one bin at a low fundamental — at 44 Hz it asks for
    ±0.9 Hz out of a 1.7 Hz grid — so a bass note would score as pure noise for
    a reason that is entirely the analysis window's. It is capped at a quarter of
    the fundamental: from the 13th partial up ±2 % would cover more than half of
    each gap between partials, and from the 25th the windows tile the spectrum
    and broadband noise there would all count as tone.

    `motion` is measured over the same segment, as `pitch_motion` gives it, so
    a section's or a choir's aperiodic spread widens the mask as a soloist's
    vibrato does. On a low note a vibrato's first sidebands lie outside the
    still window and were read as noise, so each window's bin margin is
    `fm_margin_hz`, under the same quarter-f0 cap, and noise between partials
    is still counted. Past that cap -- a deep vibrato on the upper partials of
    a bright low note -- the excursion fills the gap and part of it still reads
    as noise. None leaves the mask exactly as for a still tone.

    `tremolo`, as `tremolo_extent` reads it over the same segment, puts sidebands
    at multiples of its rate around every partial: a vibraphone's motor puts its
    first pair 4 Hz out at -16 dB, outside the still window below 200 Hz. The
    window reaches the farther of `am_margin_hz` and the FM margin: a level swing
    riding a vibrato is mostly the vibrato's own through the body's resonances,
    at its rate, and adds no lines past it. None leaves the mask exactly as
    without it.
    """
    if freqs.size < 2 or f0_hz <= 0.0:
        return float("nan")
    power = mag.astype(np.float64) ** 2
    bin_hz = float(freqs[1] - freqs[0])
    top = min(TNR_TOP_HZ, float(freqs[-1]))
    tonal = np.zeros(freqs.shape, dtype=bool)
    am = 0.0 if tremolo is None else am_margin_hz(f0_hz, bin_hz, tremolo)
    n = 1
    while True:
        target = partial_hz(f0_hz, n, b)
        margin = max(fm_margin_hz(target, f0_hz, bin_hz, motion), am)
        half = min(max(0.02 * target, margin), 0.25 * f0_hz)
        if target - half > top:
            break
        tonal |= (freqs >= target - half) & (freqs <= target + half)
        n += 1
    band = (freqs >= max(40.0, 0.8 * f0_hz)) & (freqs <= top)
    tone = float(power[tonal & band].sum())
    noise = float(power[~tonal & band].sum())
    return float(10.0 * np.log10((tone + 1e-30) / (noise + 1e-30)))


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


# How far under a note's OWN loudest partial the ladder has to sit before the
# bin is the render's floor rather than a partial. The -120 dB sentinel the
# ladder already writes catches only a bin above Nyquist; a bin that found
# nothing writes whatever the noise floor happened to be, which on a real render
# is -100 dB or so and passes every `> -120` test there is.
LADDER_FLOOR_MARGIN_DB = 80.0


def ladder_present(harmonics_db: list[float], margin: float = LADDER_FLOOR_MARGIN_DB) -> list[bool]:
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


#: Window and tolerance for `harmonic_share`. Half a second resolves a bin
#: every two hertz, which is a fifth of the tolerance at the bottom of the
#: ladder it is used on; sixty cents is wide enough for a patch tuned a few
#: cents off and far narrower than the gap between neighbouring partials.
HARMONIC_SHARE_WINDOW_S = 0.50
HARMONIC_SHARE_TOLERANCE_CENTS = 60.0


def harmonic_share(
    seg: np.ndarray, sr: int, f0_hz: float, *, partials: int = N_HARMONICS
) -> float | None:
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
    instrument and a bright one are comparable; `None` for a segment under
    4096 samples.

    **That floor is a sample count and not the tolerance it reads as.** The
    tolerance is a ratio, so the window it opens is `f0 * 0.0693` wide and
    narrows with pitch, while the bin spacing is `sr / n`: at the floor, 85 ms
    at 48 kHz, one bin is 11.7 Hz against a 9.07 Hz window at the lowest note
    `capture.py` probes, so the fundamental's own window holds no bin of its
    own. Resolving two bins there needs 0.22 s, which is inside the 0.5 s this
    reads and so costs the callers nothing — but a caller handing it a short
    segment gets a share biased down by its bottom partials rather than a None.
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
