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
"""

from __future__ import annotations

from dataclasses import asdict, dataclass

import numpy as np

from smf import Note

MIN_SUSTAIN_SEC = 0.15
N_HARMONICS = 12


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
    sus_a = int((note.start + 0.3 * note.dur) * sr)
    sus_b = int((note.start + 0.9 * note.dur) * sr)
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
