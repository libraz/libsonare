"""How many things are ringing: modal density, and why it is not a knob.

A resonator bank answers a strike with as many ringing frequencies as it has
resonators. A real radiating structure answers with as many as it has modes, and
a wooden plate the size of a piano's has hundreds below a kilohertz -- enough
that above roughly the same frequency the individual modes overlap and the
response stops being a set of pitches and becomes a diffuse floor.

That difference is audible and it has a name in both directions. Sparse, long
ringing resonances at fixed frequencies under every note is what a bell is.
Dense overlapping ones are what a room, a body, a soundboard is. No value of any
gain, decay or corner frequency turns one into the other: the count is fixed at
compile time, and a bank that rings as a chord cannot be tuned into a bank that
rings as air.

So this counts. The aftersound is transformed at a resolution fine enough to
separate individual resonances, the played note's own partials are removed, and
what is left is counted as peaks per octave. Two renders that differ by an order
of magnitude here differ structurally and not by calibration.

The envelope statistic is the same question asked the other way. A diffuse field
is a sum of many independent contributions and its envelope is Rayleigh; a
handful of sinusoids beat against each other instead and swing much wider. It
needs no peak-picking threshold, which makes it the check on the count rather
than a second version of it.

What it must NOT be read against is Rayleigh's own 0.523. That is the spread of
an unsmoothed envelope, and the one measured here is smoothed over a few cycles
of the band's bottom to get the carrier out of it -- which averages independent
samples together and pulls a genuinely diffuse field down to somewhere between
0.2 and 0.45 depending on the band. Band-limited noise put through this function
measures 0.29 at 60-125 Hz and 0.36 at 2-4 kHz, not 0.52. So `diffuse_floor`
measures the bottom of the scale by generating noise and running it through the
identical path, and a reading is meaningful against that and not against the
textbook figure. Getting this wrong understates how far from diffuse everything
is, including the instrument.
"""

from __future__ import annotations

import numpy as np

#: Octave bands the count is reported in.
DENSITY_BANDS = ((60, 125), (125, 250), (250, 500), (500, 1000),
                 (1000, 2000), (2000, 4000), (4000, 8000))
#: dB a peak must stand over the local median to count as a resonance.
PEAK_PROMINENCE_DB = 6.0
#: Rayleigh's coefficient of variation, for the unsmoothed envelope only. This
#: is NOT what `envelope_diffuseness` returns for a diffuse field -- see the
#: module docstring and `diffuse_floor`, which measures it instead.
RAYLEIGH_CV = 0.5227
#: dB a band must stand over the same recording's own floor before its texture
#: is a reading rather than a description of that floor. See `band_snr_db`.
TEXTURE_SNR_DB = 10.0


def _fine_spectrum(sig, window, sr):
    seg = np.asarray(sig[int(window[0] * sr):int(window[1] * sr)], dtype=np.float64)
    if len(seg) < 4096:
        return None, None
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    return np.fft.rfftfreq(len(seg), 1.0 / sr), sp


def modal_density(sig, note, window=(2.5, 6.0), sr=48000, bands=DENSITY_BANDS,
                  B=0.0, notch_cents=35.0, notch=True):
    """Peaks per octave in the aftersound, with the played note's partials removed.

    The note's own partials are notched out rather than left in, because a
    string is not what is under question and its series would otherwise be
    counted as the structure's modes. The notch is a third of a semitone, which
    is wide enough for the analysis window's own resolution and narrow enough
    to leave a resonance sitting between two partials.

    `notch=False` is for a struck piece that has no played partials to remove,
    where the whole spectrum is the structure. It is not an optimisation: the
    notch is uncapped, so on a low note the mask fuses into a continuous band
    above roughly the sixteenth partial and covers the entire upper spectrum —
    every band comes back empty and the count reads zero for a signal full of
    resonances. Passing an unpitched piece a nominal note number is therefore
    not a way to get an unnotched count.
    """
    from .partials import note_hz, partial_hz
    fr, sp = _fine_spectrum(sig, window, sr)
    if fr is None:
        return {b: (0, 0.0) for b in bands}
    keep = np.ones(len(fr), dtype=bool)
    if notch:
        f0 = note_hz(note)
        half = 2.0 ** (notch_cents / 1200.0)
        for k in range(1, 200):
            f = partial_hz(f0, B, k)
            if f > fr[-1]:
                break
            keep &= ~((fr > f / half) & (fr < f * half))
    out = {}
    for lo, hi in bands:
        m = keep & (fr >= lo) & (fr < hi)
        if m.sum() < 32:
            out[(lo, hi)] = (0, 0.0)
            continue
        band = 10 * np.log10(np.maximum(sp[m], 1e-30))
        f_in = fr[m]
        # A local median rather than a global one: the band's own tilt would
        # otherwise decide how many peaks its quiet end is allowed to have.
        w = max(9, (len(band) // 24) | 1)
        pad = np.pad(band, w // 2, mode="edge")
        local = np.array([np.median(pad[i:i + w]) for i in range(len(band))])
        above = band > local + PEAK_PROMINENCE_DB
        # One peak per contiguous run, so a resonance a few bins wide is one
        # resonance and not the width of its own skirt.
        runs = int(np.sum(above[1:] & ~above[:-1])) + int(above[0])
        octaves = np.log2(hi / lo)
        out[(lo, hi)] = (runs, runs / octaves)
        del f_in
    return out


def envelope_diffuseness(sig, band, window=(2.5, 6.0), sr=48000):
    """Spread of the detrended envelope in one band: low is diffuse, high is few.

    The scale runs from `diffuse_floor` at the bottom, through about 0.48 for
    two equal partials beating to a full null, upward as the number of things
    ringing falls further. Reported as measured rather than as a verdict,
    because a band that holds almost no energy at all will produce a number and
    it will mean nothing -- check the band's level against the floor first.
    """
    x = np.asarray(sig[int(window[0] * sr):int(window[1] * sr)], dtype=np.float64)
    if len(x) < 4096:
        return float("nan")
    S = np.fft.rfft(x)
    fr = np.fft.rfftfreq(len(x), 1.0 / sr)
    y = np.fft.irfft(np.where((fr >= band[0]) & (fr < band[1]), S, 0.0), len(x))
    # Hilbert envelope via the analytic signal, smoothed over a few cycles of
    # the band's own bottom so the carrier is gone and the beating is not.
    n = len(y)
    Y = np.fft.fft(y)
    h = np.zeros(n)
    h[0] = 1.0
    h[1:(n + 1) // 2] = 2.0
    if n % 2 == 0:
        h[n // 2] = 1.0
    env = np.abs(np.fft.ifft(Y * h))
    w = max(16, int(4 * sr / max(band[0], 20.0)))
    env = np.convolve(env, np.ones(w) / w, mode="valid")
    if len(env) < 16 or not np.all(np.isfinite(env)) or env.max() <= 0.0:
        return float("nan")
    # Take the decay out first. An exponential tail has a large envelope spread
    # entirely on its own -- over a window holding two time constants it reaches
    # 0.6, which is more than a pair of tones beating to a full null produces --
    # so without this the statistic reports a fast-decaying band as though it
    # were a sparse one. That is not a subtlety at the margin: it inverts the
    # comparison whenever the two things being compared decay at different
    # rates, which is exactly when it is being asked. A straight line through
    # the log envelope is the right detrend because the quantity removed is an
    # exponential decay, and it leaves the beating alone.
    tiny = float(env.max()) * 1e-9
    t = np.arange(len(env), dtype=np.float64)
    logenv = np.log(np.maximum(env, tiny))
    slope, intercept = np.polyfit(t, logenv, 1)
    env = env / np.exp(intercept + slope * t)
    mu = float(env.mean())
    if mu <= 0:
        return float("nan")
    return float(env.std() / mu)


def recurrence(spectro, signals, notes, velocity, window=(2.5, 6.0),
               prominence_db=PEAK_PROMINENCE_DB, scale: int = 0):
    """Per spectral row, the fraction of notes whose aftersound peaks there.

    What makes a bell is not a level, it is a coincidence: the same frequencies
    answer whatever you strike. The loss states that as a row-wise minimum
    across notes of each note's own normalised residue, and a minimum prices
    invariant energy by its WEAKEST relative appearance -- so a bank that is a
    small fraction of a loud note and a large fraction of a quiet one has no
    weak appearance to be found by, and passes. One did: twenty fixed resonators
    ringing five seconds under every note improved the minimum-based term while
    turning the voice into a chime, and the ear caught it after the score did
    not.

    Counting instead of measuring is what fixes that. A row that peaks in nearly
    every note is a property of the instrument's body -- or of a bank standing
    in for one -- and a row that peaks in one or two is that note's own. The
    played note's partials are notched out first, for the same reason they are
    in `modal_density`, or every note would report its own series as recurrent.

    The notch has a cost worth knowing: a resonance that happens to sit on a
    note's own partial is invisible FOR THAT NOTE, so a bank tuned to the note
    grid -- which the undamped treble population is, because those pitches are
    played strings -- is undercounted by however many notes it coincides with.
    It reads as recurrent in the notes it misses and silent in the notes it hits,
    which is the opposite of alarming and the wrong way round. Read the profile,
    not only the scalar.

    Returns (rows_hz, fraction), fraction in [0, 1].
    """
    from .partials import (
        RESOLVABLE_PARTIAL,
        fit_inharmonicity,
        harmonic_rows,
        note_hz,
    )
    hz = spectro.rows_hz(scale)
    hits = np.zeros(len(hz))
    seen = 0
    for n in notes:
        sig = signals.get((n, velocity))
        if sig is None:
            continue
        S = spectro(sig)[scale]
        c = spectro.columns(scale, S.shape[1], window[0], window[1])
        if not c.any():
            continue
        col = S[:, c].mean(axis=1)
        f0 = note_hz(n)
        hm = harmonic_rows(hz, f0, fit_inharmonicity(sig, f0, spectro.sample_rate),
                           max_partial=RESOLVABLE_PARTIAL)
        w = max(9, (len(col) // 24) | 1)
        pad = np.pad(col, w // 2, mode="edge")
        local = np.array([np.median(pad[i:i + w]) for i in range(len(col))])
        hits += ((col > local + prominence_db) & ~hm).astype(float)
        seen += 1
    return hz, (hits / seen if seen else hits)


def bell_score(spectro, signals, notes, velocity, threshold: float = 0.6, **kw):
    """How much of the spectrum answers nearly every note. Lower is an instrument.

    One number for the recurrence profile: the share of rows that peak in at
    least `threshold` of the notes. Compare a model's against the reference's
    rather than against zero -- a real instrument's body does recur, and the
    question is whether the model recurs more.
    """
    _hz, frac = recurrence(spectro, signals, notes, velocity, **kw)
    return float(np.mean(frac >= threshold))


def band_snr_db(sig, band, window=(2.5, 6.0), floor_window=(9.0, 10.0), sr=48000):
    """How far a band stands over the same recording's own floor in that band.

    The gate every texture reading needs, and the one whose absence produced the
    largest wrong answer this package has recorded. A sampled instrument's top
    octave stops decaying partway through the note and sits flat at the session
    floor -- measured here, four kilohertz up flattens 55 to 59 dB under its own
    peak at about three and a half seconds and does not move again. A model that
    keeps decaying past that point is then compared against noise, and it is the
    NOISE that reads as the diffuse, dense, instrument-like thing.

    The floor is taken from the same note after its release rather than from a
    separate quiet capture, because a sampler gates its recorded floor with the
    same envelope it gates the note with -- so the floor under a loud note is
    not the floor under a soft one, and a single figure for the corpus would
    understate it exactly where it does the most damage.
    """
    def p(win):
        x = np.asarray(sig[int(win[0] * sr):int(win[1] * sr)], dtype=np.float64)
        if len(x) < 256:
            return 0.0
        S = np.fft.rfft(x)
        fr = np.fft.rfftfreq(len(x), 1.0 / sr)
        m = (fr >= band[0]) & (fr < band[1])
        return float(np.sum(np.abs(S[m]) ** 2)) / len(x) ** 2

    a, b = p(window), p(floor_window)
    if a <= 0.0 or b <= 0.0:
        return float("inf")
    return 10 * np.log10(a / b)


def diffuse_floor(band, window=(2.5, 6.0), sr=48000, seed=20240517):
    """What a fully diffuse field measures HERE, through the identical path.

    The theoretical Rayleigh figure does not apply to a smoothed envelope, and
    the smoothing is not optional -- without it the statistic reads the carrier
    rather than the beating. So the bottom of the scale is generated rather than
    quoted: white noise, the same band, the same window length, the same
    envelope and the same smoothing. Seeded, so a comparison is repeatable.

    Averaged over a few draws because one draw of a random process is itself
    random, and the spread between draws is a few percent of the value.
    """
    rng = np.random.default_rng(seed)
    n = int((window[1] - window[0]) * sr)
    vals = [envelope_diffuseness(rng.standard_normal(n), band, (0.0, n / sr), sr)
            for _ in range(4)]
    return float(np.nanmean(vals))
