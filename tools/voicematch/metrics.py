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
HIT_MAX_SEC = 1.8
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


def to_mono(audio: np.ndarray) -> np.ndarray:
    """Mix (frames, channels) down to mono float64."""
    if audio.ndim == 1:
        return audio.astype(np.float64)
    return audio.astype(np.float64).mean(axis=1)


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

    def to_dict(self) -> dict:
        return asdict(self)


def analyze_note(mono: np.ndarray, sr: int, note: Note, render_end: float) -> NoteMetrics:
    """Compute all per-note metrics from the mono render."""
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
    # stopped — a real grand's C8 is 40 dB down by 2.2 s. The harmonic ladder
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

    # Harmonic profile relative to h1.
    harmonics_db: list[float] = [0.0]
    h_mags = [h1_mag]
    for k in range(2, N_HARMONICS + 1):
        target = k * f0
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
    power = mag**2
    harmonic_mask = np.zeros_like(freqs, dtype=bool)
    k = 1
    while k * f0 < min(16000.0, sr / 2) and k <= 40:
        target = k * f0
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
    )


# --------------------------------------------------------------------------- #
# Level, and the attack's high end
# --------------------------------------------------------------------------- #
# Where the held level is read, in seconds from the onset. Fixed rather than a
# fraction of the note, because the gate is a property of the probe and not of
# the instrument: on a corpus probe holding eight seconds, a 0.3-0.9 fraction
# reads 2.4 to 7.2 s in, which on the top octave is entirely after the note has
# stopped — the reference's own C8 is down 40 dB by 2.2 s. Both sides then
# measure silence and agree perfectly about it.
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


def attack_bands(mono: np.ndarray, sr: int, note: Note) -> list[float | None]:
    """High-band balance through the attack: one value per band per time slice.

    Each value is the band's level relative to that slice's own broadband level,
    so the measure says nothing about how loud the attack was and everything
    about its spectral tilt. That is what makes it usable on the RMS-normalised
    signal the rest of the metric set reads, and what makes it comparable
    between a model and a reference captured at different gains.

    Bands above Nyquist and slices past the end of the render come back None
    rather than as a floor value, so a shorter render contributes nothing to the
    term instead of contributing a fabricated match.
    """
    win = int(sr * ATTACK_WINDOW_MS / 1000.0)
    on = int(note.start * sr)
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
        for lo, hi in ATTACK_BANDS_HZ:
            if lo >= sr / 2:
                out.append(None)
                continue
            mask = (freqs >= lo) & (freqs < min(hi, sr / 2))
            share = float(power[mask].sum()) / total
            out.append(round(float(10.0 * np.log10(max(share, 1e-12))), 2))
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


def _band_decay(
    seg: np.ndarray, sr: int, centers, ratio: float,
    *, n_fft: int = 1024, hop: int = 256, drop_db: float = 25.0,
) -> list[float | None]:
    """Post-peak decay slope in dB/s for each band, or None where unfittable.

    A percussion hit is a decay from the first sample, so each band is fit from
    its own peak frame downward — bands do not peak together, and a wire buzz
    that arrives after the shell would read as a rising slope if they were all
    anchored on the broadband onset.

    The fit stops `drop_db` below the peak so it measures the decay rather than
    where the band reaches the render's noise floor and flattens out.
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
        series = np.asarray(_db(np.sqrt(power[:, mask].sum(axis=1))), dtype=np.float64)
        peak = int(np.argmax(series))
        top = series[peak]
        window = np.arange(len(series)) >= peak
        window &= series >= top - drop_db
        # Stop at the first frame that falls through the floor, so a band that
        # rings, dies and is then re-excited by the next hit is not fit across
        # the gap.
        below = np.where((np.arange(len(series)) > peak) & (series < top - drop_db))[0]
        if below.size:
            window &= np.arange(len(series)) < below[0]
        if np.count_nonzero(window) < 4:
            out.append(None)
            continue
        out.append(float(np.polyfit(times[window], series[window], 1)[0]))
    return out


@dataclass
class HitMetrics:
    """One percussion hit, measured without reference to a fundamental."""

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


def analyze_hit(mono: np.ndarray, sr: int, note: Note, window_end: float) -> HitMetrics:
    """Compute the percussion metric set for one hit.

    The window runs from the strike to `window_end` (the next hit, or the end of
    the render), capped at `HIT_MAX_SEC`. The note's own duration is ignored:
    a drum is a one-shot and its note-off carries no information.

    The strike is located rather than assumed to be at the note-on, because a
    hosted plugin's is not: leading silence inside the window inflates time to
    peak by exactly its own length, dilutes the RMS the crest and level are
    measured against, and tilts every per-band decay fit.
    """
    onset = _hit_onset(mono, sr, note.start, window_end)
    on = int(onset * sr)
    end = int(min(window_end, onset + HIT_MAX_SEC) * sr)
    seg = np.asarray(mono[on:min(end, len(mono))], dtype=np.float64)
    if len(seg) < 256:
        seg = np.asarray(mono[on : on + 256], dtype=np.float64)

    freqs, mag = _spectrum(seg, sr)
    power = mag**2
    bands = _band_power(freqs, power, THIRD_OCTAVE_CENTERS, THIRD_OCTAVE_RATIO)
    bands_db = np.asarray(_db(np.sqrt(bands)), dtype=np.float64)
    top = float(bands_db.max())
    bands_db = np.maximum(bands_db - top, BAND_FLOOR_DB)
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

    return HitMetrics(
        note=note.note,
        velocity=note.velocity,
        bands_db=[round(float(v), 2) for v in bands_db],
        peak_band_hz=peak_band,
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
