"""Decide whether a reference was recorded through a rig, from the reference itself.

A capture's `rig` field gates fitting, and `dry` cannot stand in for it: dryness
is looked for as a tail and a cabinet has none. So the question has to be put to
the audio directly, and what the audio can answer is narrower than it looks.

**A rig can be shown present and cannot be shown absent.** Every signature here
is something a rig adds — a speaker's skirt, a rotor's modulation — so finding
one is evidence and finding none is not. A dull instrument recorded flat and a
bright one recorded through a cabinet both come back dull, and no single-sided
measurement separates them. `rig: "none"` therefore needs an A/B the plugin can
supply, which is how the fingered bass got its answer: its amplifier stage
switched off and what left the signal was measured. A rack that publishes no
such switch cannot produce a `none` for any of its slots, whatever the spectrum
says, and the honest outcome there is that the answer stays unclassified.

Two signatures, one per rig kind:

- **A cabinet is a steep skirt with a knee**, and the slope is the part a merely
  dark source cannot imitate. A speaker in a box falls at 30-50 dB per octave
  above 4-5 kHz; a pickup, a string or a mic'd body falls at 6-20. Better still
  when several timbres are available: a cabinet is *one filter after everything*,
  so instruments through it share a high-frequency shape while instruments that
  are each dull for their own reasons do not. `shared_skirt` is that comparison
  and it is the strongest evidence this module produces.
- **A rotary speaker is anti-correlated across the mics.** When the horn faces
  one it is turned away from the other, so the two channels' envelopes modulate
  out of phase. In-phase modulation at a few dB is a tonewheel beating against
  its neighbour, which every drawbar organ does and no rig is responsible for.

The measurement's own blind spot is worth naming: a rig's signature lives where
the source has energy. A drawbar organ generates nine harmonics and nothing
above them, so a cabinet after it has no band to shape and leaves no skirt to
find — the null there says the instrument is unmeasurable this way, not that it
is clean.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from corpus import Corpus
from wavio import read_wav

#: Where a guitar cabinet's knee sits, and the octaves the slope is fitted over.
#: Below 4 kHz the curve is the instrument's own; above 12 kHz a 48 kHz capture
#: is close enough to Nyquist that the anti-alias filter joins in.
SKIRT_LO_HZ = 4000.0
SKIRT_HI_HZ = 12000.0

#: Third-octave resolution: fine enough to place a knee, coarse enough that a
#: single partial landing in a band does not become the shape.
BANDS_PER_OCTAVE = 3

#: The attack is where a plucked or struck instrument puts its high frequencies,
#: so the skirt is measured there rather than over the sustain, which on a dark
#: instrument has nothing above 4 kHz to filter either way.
ATTACK_MS = 120.0

#: A rotary runs between a slow chorale and a fast tremolo; outside this the
#: modulation is a tremolo pedal or a note's own envelope rather than a rotor.
MOD_LO_HZ = 0.4
MOD_HI_HZ = 10.0

#: Below this the two channels are the same signal and the rotary test has
#: nothing to read. Well under the several dB a rotor swings between the mics,
#: and above the -96 dB a dual-mono 16-bit render differs by.
DUAL_MONO_DB = -40.0


@dataclass(frozen=True)
class Skirt:
    """One timbre's high-frequency fall-off, as a slope and the knee it starts at."""

    #: dB per octave over `SKIRT_LO_HZ`..`SKIRT_HI_HZ`, negative for a fall.
    slope_db_per_octave: float
    #: The highest third-octave still within 10 dB of the 1-2 kHz plateau.
    knee_hz: float
    #: Note-to-note standard deviation of the slope. A filter after the
    #: instrument applies equally to every note, so a shared rig reads steadier
    #: than a rolloff belonging to the instrument's own strings.
    slope_spread: float
    #: Third-octave centres and the median curve, normalised at 1-2 kHz.
    centres: tuple[float, ...]
    curve: tuple[float, ...]


@dataclass(frozen=True)
class Rotary:
    """Evidence for or against a rotating speaker, from the modulation's stereo phase."""

    #: Dominant envelope modulation rate, Hz, and its depth in dB.
    rate_hz: float
    depth_db: float
    #: Correlation of the two channels' band-limited envelopes. A rotor drives
    #: this negative; a tonewheel beat leaves it positive.
    interchannel_correlation: float
    #: L-R against L, in dB. A rotor swings several dB between the mics, so a
    #: file rendered dual mono has nowhere to put one and the correlation above
    #: is +1 by construction rather than by measurement.
    channel_separation_db: float
    #: Whether the correlation was measured on channels that actually differ.
    #: False for a mono or dual-mono capture, where the question is vacuous —
    #: which is not the same answer as a rotor being absent.
    stereo: bool


def _mono(audio: np.ndarray) -> np.ndarray:
    return audio if audio.ndim == 1 else audio.mean(axis=1)


def _attack(x: np.ndarray, sample_rate: int) -> np.ndarray:
    """The first `ATTACK_MS` from the onset, found as the first 5 % of the peak."""
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    start = int(np.argmax(np.abs(x) > peak * 0.05)) if peak > 0 else 0
    return x[start : start + int(sample_rate * ATTACK_MS / 1000.0)]


def _third_octave(x: np.ndarray, sample_rate: int) -> tuple[np.ndarray, np.ndarray]:
    """Third-octave band levels of one segment, in dB, from 250 Hz up."""
    if x.size < 1024:
        return np.zeros(0), np.zeros(0)
    n_fft = 1 << 15
    mag = np.abs(np.fft.rfft(x * np.hanning(x.size), n_fft))
    freq = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)
    db = 20.0 * np.log10(np.maximum(mag, 1e-12))
    top = min(20000.0, sample_rate / 2.0)
    count = int(np.log2(top / 250.0) * BANDS_PER_OCTAVE)
    centres = 250.0 * 2.0 ** (np.arange(count) / BANDS_PER_OCTAVE)
    edge = 2.0 ** (0.5 / BANDS_PER_OCTAVE)
    levels = []
    for centre in centres:
        band = (freq >= centre / edge) & (freq < centre * edge)
        levels.append(10.0 * np.log10(np.mean(10.0 ** (db[band] / 10.0))) if band.any() else np.nan)
    return centres, np.asarray(levels)


def measure_skirt(corpus: Corpus, velocity: int = 0) -> Skirt:
    """Fit the high-frequency fall-off over every note of one corpus.

    Normalising each note at 1-2 kHz before the median is what makes the curves
    comparable across instruments: what is being compared is the *shape* above
    the plateau, not how loud or how bright the instrument is.
    """
    velocity = velocity or corpus.velocities[-1]
    slopes: list[float] = []
    curves: list[np.ndarray] = []
    centres = np.zeros(0)
    for note in corpus.notes:
        path = corpus.renders.get((note, velocity))
        if path is None:
            continue
        audio, sample_rate = read_wav(path)
        x = _mono(audio)[int(sample_rate * corpus.preroll_s) :]
        centres, curve = _third_octave(_attack(x, sample_rate), sample_rate)
        if centres.size == 0:
            continue
        plateau = np.nanmedian(curve[(centres >= 1000.0) & (centres <= 2000.0)])
        curve = curve - plateau
        curves.append(curve)
        band = (centres >= SKIRT_LO_HZ) & (centres <= SKIRT_HI_HZ) & np.isfinite(curve)
        if band.sum() >= 3:
            slopes.append(float(np.polyfit(np.log2(centres[band]), curve[band], 1)[0]))
    if not curves:
        raise ValueError(f"{corpus.timbre}: no renders to measure a skirt on")
    median = np.nanmedian(np.asarray(curves), axis=0)
    above = centres[(centres >= 1000.0) & np.isfinite(median) & (median >= -10.0)]
    return Skirt(
        slope_db_per_octave=float(np.nanmedian(slopes)) if slopes else float("nan"),
        knee_hz=float(above.max()) if above.size else float("nan"),
        slope_spread=float(np.nanstd(slopes)) if slopes else float("nan"),
        centres=tuple(float(f) for f in centres),
        curve=tuple(float(v) for v in median),
    )


def curve_distance(a: Skirt, b: Skirt, lo_hz: float = 2000.0) -> float:
    """RMS difference between two normalised curves above `lo_hz`, in dB.

    Two timbres recorded through one cabinet agree here within a few dB while
    differing from anything not behind it by ten or more, which is what turns a
    dark spectrum into evidence of a shared filter.
    """
    centres = np.asarray(a.centres)
    band = (centres >= lo_hz) & (centres <= 16000.0)
    diff = np.asarray(a.curve)[band] - np.asarray(b.curve)[band]
    return float(np.sqrt(np.nanmean(diff**2)))


def measure_rotary(corpus: Corpus, velocity: int = 0) -> Rotary:
    """Look for a rotor: envelope modulation that the two mics disagree about."""
    velocity = velocity or corpus.velocities[-1]
    rates: list[float] = []
    depths: list[float] = []
    correlations: list[float] = []
    separations: list[float] = []
    for note in corpus.notes:
        path = corpus.renders.get((note, velocity))
        if path is None:
            continue
        audio, sample_rate = read_wav(path)
        if audio.ndim < 2 or audio.shape[1] < 2:
            continue
        side = float(np.sqrt(np.mean((audio[:, 0] - audio[:, 1]) ** 2)))
        mid = float(np.sqrt(np.mean(audio[:, 0] ** 2)))
        separations.append(20.0 * np.log10(max(side, 1e-12) / max(mid, 1e-12)))
        # Inside the gate, and never past it. Note-off is one large event both
        # channels share, so a window that reaches it correlates them whatever
        # the rotor was doing — on the drawbar organ that is the difference
        # between +0.50 and +0.999.
        head = corpus.preroll_s + 0.5
        tail = corpus.preroll_s + min(corpus.gate_s, 5.5)
        seg = _sounding(audio[int(sample_rate * head) : int(sample_rate * tail)])
        if seg.shape[0] < sample_rate:
            continue
        left, env_rate = _envelope_db(seg[:, 0], sample_rate)
        right, _ = _envelope_db(seg[:, 1], sample_rate)
        rate, depth = _modulation_peak(left, env_rate)
        rates.append(rate)
        depths.append(depth)
        correlations.append(_band_correlation(left, right, env_rate))
    separation = float(np.median(separations)) if separations else float("-inf")
    if not rates:
        return Rotary(float("nan"), float("nan"), float("nan"), separation, False)
    return Rotary(
        rate_hz=float(np.median(rates)),
        depth_db=float(np.median(depths)),
        interchannel_correlation=float(np.median(correlations)),
        channel_separation_db=separation,
        stereo=separation > DUAL_MONO_DB,
    )


#: How far below its own peak the envelope may fall before the window stops
#: being the note. A muted guitar dies inside a second under a six-second gate,
#: and the silence after it is a shape that reports as tens of dB of modulation.
SOUNDING_FLOOR_DB = 40.0


def _sounding(seg: np.ndarray, hop: int = 512) -> np.ndarray:
    """Trim a window to the span where the note is still within `SOUNDING_FLOOR_DB`."""
    x = _mono(seg)
    frames = x[: (x.size // hop) * hop].reshape(-1, hop)
    level = np.sqrt((frames**2).mean(axis=1))
    if level.size == 0 or level.max() <= 0:
        return seg
    above = np.flatnonzero(level > level.max() * 10.0 ** (-SOUNDING_FLOOR_DB / 20.0))
    return seg[: int(above[-1] + 1) * hop] if above.size else seg


def _envelope_db(x: np.ndarray, sample_rate: int, hop: int = 64) -> tuple[np.ndarray, float]:
    """Log envelope with its decay taken out, and the rate it is sampled at.

    A note's own fall is a straight line in dB, and a line leaks into the lowest
    modulation bins — which is where a slow rotary lives. Removing the trend
    rather than the mean is what keeps a decay from reading as a rotor.
    """
    frames = x[: (x.size // hop) * hop].reshape(-1, hop)
    env = 20.0 * np.log10(np.maximum(np.sqrt((frames**2).mean(axis=1)), 1e-12))
    index = np.arange(env.size, dtype=float)
    return env - np.polyval(np.polyfit(index, env, 1), index), sample_rate / hop


def _modulation_peak(env: np.ndarray, env_rate: float) -> tuple[float, float]:
    """Strongest modulation in the rotary band, as a rate and a peak-to-peak depth.

    The band starts at two cycles in the window rather than at `MOD_LO_HZ`: one
    cycle is a shape and cannot be told from a note that decays to the floor,
    which on a short gate otherwise reports tens of dB of modulation at the band
    edge. A gate too short to hold two turns of a slow rotor cannot see one, and
    saying so is better than reporting the envelope as a rotor.
    """
    windowed = env * np.hanning(env.size)
    mag = np.abs(np.fft.rfft(windowed)) / (env.size * 0.25)
    freq = np.fft.rfftfreq(env.size, 1.0 / env_rate)
    band = (freq >= max(MOD_LO_HZ, 2.0 * env_rate / env.size)) & (freq <= MOD_HI_HZ)
    if not band.any():
        return float("nan"), float("nan")
    peak = int(np.argmax(mag[band]))
    return float(freq[band][peak]), float(2.0 * mag[band][peak])


def _band_correlation(left: np.ndarray, right: np.ndarray, env_rate: float) -> float:
    """Correlation of two envelopes restricted to the rotary band."""

    def restrict(env: np.ndarray) -> np.ndarray:
        spectrum = np.fft.rfft(env)
        freq = np.fft.rfftfreq(env.size, 1.0 / env_rate)
        spectrum[(freq < MOD_LO_HZ) | (freq > MOD_HI_HZ)] = 0.0
        return np.fft.irfft(spectrum, env.size)

    a, b = restrict(left), restrict(right)
    norm = float(np.linalg.norm(a) * np.linalg.norm(b))
    return float(np.dot(a, b) / norm) if norm > 0 else float("nan")
