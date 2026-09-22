"""Generate the mastering/restoration evaluation corpus and its manifest.

One corpus serves both families (see ``docs/objective.md``). It has three
halves, and they differ in what a measurement may conclude from them:

``synthetic``
    Sine, chord and speech beds crossed with clicks, hum, clipping, noise and
    reverberation. Each item ships a clean reference beside it and every planted
    defect's quantity in the manifest, so a restoration metric has something to be
    measured against and a detector has a known answer to be scored on.

``listening``
    The four long fixtures ``tools/mastering_generate_listening_corpus.py``
    already produces, imported rather than re-derived so there is one generator
    for them. They carry no clean reference and no recorded defect quantity, so
    they support chain measurement and listening only -- never a detector's
    acceptance.

``recorded``
    Real captures, for the cases synthesis does not reach. They are not
    committed; this script only picks up whatever sits in ``recorded/`` with a
    manifest beside it.

Quantities are measured after rounding to the file's bit depth, on the pair of
stages the defect sits between, so the manifest describes what landed on disk
rather than the intermediate the generator held. On an item carrying several
defects the later stages round once more than the measurement did, which moves
a level by under one 24-bit step.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

_HERE = Path(__file__).resolve().parent
_TOOLS = _HERE.parent
# This directory goes in last so it ends up first. `tools/voicematch` has its own
# `corpus.py`, and with it ahead of us on the path an `import corpus` from
# anywhere in this harness silently resolves to the voice-tuning one.
sys.path.insert(0, str(_TOOLS / "voicematch"))
sys.path.insert(0, str(_TOOLS))
sys.path.insert(0, str(_HERE))

from _repo import REPO_ROOT
from wavio import read_wav, write_wav

SAMPLE_RATE = 48_000
BIT_DEPTH = 24
DEFAULT_SEED = 20_250_915

MANIFEST_SCHEMA = 1
MANIFEST_NAME = "manifest.json"

SYNTHETIC_DIR = "synthetic"
DYNAMICS_DIR = "dynamics"
LISTENING_DIR = "listening"
RECORDED_DIR = "recorded"

# Short-term loudness is a 3 s window with no partial emitted, so a feed under
# this reports no spread at all rather than a small one.
SHORT_TERM_WINDOW_SECONDS = 3.0

# How many noise draws a noise item carries. Noise is the one planted defect
# whose metric movement is smaller than the spread between draws -- a denoiser's
# STOI delta at 0 dB averages +0.002 with a standard deviation of 0.003 -- so a
# single draw supports "did not get worse" but not "got better". The other
# defects are deterministic or move far above that spread and stay single.
#
# Fixed at five before any of it was measured. Choosing the count by seeing
# which count produces the sign one wants is the same defect as choosing the
# seed that way.
NOISE_REALIZATIONS = 5

# How long the synthetic impulse response runs, in multiples of its own T60. At
# one T60 the tail is already 60 dB down, so the half beyond it is what keeps the
# truncation from landing inside the decay the fit reads.
RIR_SPAN_IN_T60 = 1.5

# The band the reverberation time is fitted over: ISO 3382's T30, which starts
# below the direct sound and stops above the truncation.
T60_FIT_UPPER_DB = -5.0
T60_FIT_LOWER_DB = -35.0

# How wet the reverberant items are, and where the tail starts. Equal direct and
# reverberant energy is a distant source in a live room; the predelay sits under
# the dereverberator's own 50 ms late-delay default so that the knob has both an
# early and a late region to move between.
REVERB_DRR_DB = 0.0
REVERB_PREDELAY_MS = 20.0


# ---------------------------------------------------------------- quantization


def quantize(audio: np.ndarray) -> np.ndarray:
    """Round to the grid the WAV writer will use, in float.

    Every planted quantity is measured after this, so the clipped-sample count
    and the achieved SNR in the manifest are the file's, not the generator's.
    """
    full = float((1 << (BIT_DEPTH - 1)) - 1)
    return np.rint(np.clip(audio, -1.0, 1.0) * full) / full


# ------------------------------------------------------------------------ beds


def _time_axis(seconds: float) -> np.ndarray:
    return np.arange(round(SAMPLE_RATE * seconds), dtype=np.float64) / SAMPLE_RATE


def sine_bed(seconds: float, freq: float = 440.0, amp: float = 0.5) -> np.ndarray:
    """A steady sine, the two channels offset in phase so a downmix is not a copy."""
    t = _time_axis(seconds)
    left = amp * np.sin(2.0 * np.pi * freq * t)
    right = amp * np.sin(2.0 * np.pi * freq * t + 0.35)
    return np.stack([left, right], axis=1)


def chord_bed(seconds: float, root: float = 220.0, amp: float = 0.28) -> np.ndarray:
    """A major triad under a slow envelope.

    The envelope is what keeps short-term loudness spread from being degenerate
    on a bed that is otherwise stationary.
    """
    t = _time_axis(seconds)
    env = 0.7 + 0.3 * np.sin(2.0 * np.pi * 0.5 * t)
    partials = (root, root * 2 ** (4 / 12), root * 2 ** (7 / 12))
    left = sum(amp * np.sin(2.0 * np.pi * f * t + 0.11 * i) for i, f in enumerate(partials))
    right = sum(amp * np.sin(2.0 * np.pi * f * t - 0.19 * i) for i, f in enumerate(partials))
    return np.stack([left * env, right * env], axis=1)


def _peak_normalize(audio: np.ndarray, peak: float) -> np.ndarray:
    current = float(np.max(np.abs(audio)))
    if current <= 0.0:
        return audio
    return audio * (peak / current)


# ------------------------------------------------- beds that carry dynamics
#
# Short-term loudness is a 3 s window, so a bed under about 3.1 s produces no
# blocks at all and a steady one produces blocks that are all the same. Neither
# leaves the spread anywhere to move, which is what a check for over-compression
# needs. These beds run long and vary slowly on purpose.


def _section_gain(seconds: float, section_seconds: float, levels_db: list[float]) -> np.ndarray:
    """A piecewise-constant gain over repeating sections, with short fades.

    The fade is not cosmetic: a step between two levels is a click, and a click
    is a transient the chain's limiter reacts to rather than the level change.
    """
    t = _time_axis(seconds)
    gain = np.zeros_like(t)
    fade = 0.05
    for index, level_db in enumerate(levels_db):
        start = index * section_seconds
        inside = (t >= start) & (t < start + section_seconds)
        gain[inside] = 10.0 ** (level_db / 20.0)
    if len(levels_db) * section_seconds < seconds:
        gain[t >= len(levels_db) * section_seconds] = 10.0 ** (levels_db[-1] / 20.0)
    smoothing = max(1, round(fade * SAMPLE_RATE))
    window = np.ones(smoothing) / smoothing
    return np.convolve(gain, window, mode="same")


def apply_sections(
    bed: np.ndarray, section_seconds: float, levels_db: list[float], *, peak: float = 0.9
) -> np.ndarray:
    """Drive any bed through the stated section levels.

    Normalized after the levels are applied, so the stated levels set the
    distribution's shape and the peak sets its headroom independently.
    """
    seconds = bed.shape[0] / SAMPLE_RATE
    return _peak_normalize(bed * _section_gain(seconds, section_seconds, levels_db)[:, None], peak)


def section_bed(
    seconds: float, section_seconds: float, levels_db: list[float], *, peak: float = 0.9
) -> np.ndarray:
    """A chord alternating between stated levels, section by section."""
    return apply_sections(chord_bed(seconds), section_seconds, levels_db, peak=peak)


def swell_bed(
    seconds: float, *, period_seconds: float, depth_db: float, peak: float = 0.9
) -> np.ndarray:
    """A chord under one slow sinusoidal swell in dB.

    The period runs well past the 3 s window: at a period near the window the
    measurement averages most of the swing away and the spread reads far under
    the depth that was applied.
    """
    t = _time_axis(seconds)
    gain_db = depth_db * np.sin(2.0 * np.pi * t / period_seconds)
    shaped = chord_bed(seconds) * (10.0 ** (gain_db / 20.0))[:, None]
    return _peak_normalize(shaped, peak)


# ------------------------------------------------------------------ speech bed
#
# STOI correlates short-time band envelopes against a model fitted to speech, so
# it says nothing about a sine and has already called a correct restoration a
# regression on one. These items are what STOI is read on.


def _speech_like(n: int, fs: int) -> np.ndarray:
    """Pitch pulses through three formant resonators, amplitude-gated like syllables.

    This is the same generator the STOI implementation was checked against the
    reference implementation with, kept identical on purpose: a speech-like
    signal written twice is two signals, and the agreement figures would then
    have been measured on material the corpus does not contain. The per-sample
    resonator loop is part of what is being reused -- rewriting it as a
    convolution would be a second implementation of the same filter.
    """
    t = np.arange(n) / fs
    f0 = 110.0 + 25.0 * np.sin(2 * np.pi * 0.7 * t)
    phase = np.cumsum(2 * np.pi * f0 / fs)
    excitation = np.where(np.diff(np.floor(phase / (2 * np.pi)), prepend=0) > 0, 1.0, 0.0)
    out = np.zeros(n)
    for formant, bandwidth, gain in (
        (730.0, 90.0, 1.0),
        (1090.0, 110.0, 0.5),
        (2440.0, 170.0, 0.25),
    ):
        r = np.exp(-np.pi * bandwidth / fs)
        theta = 2 * np.pi * formant / fs
        a1, a2 = -2 * r * np.cos(theta), r * r
        y = np.zeros(n)
        for i in range(2, n):
            y[i] = excitation[i] - a1 * y[i - 1] - a2 * y[i - 2]
        out += gain * y
    syllables = (0.5 + 0.5 * np.sin(2 * np.pi * 2.3 * t)) ** 3
    out *= syllables
    return out / np.max(np.abs(out)) * 0.7


_SPEECH_CACHE: dict[int, np.ndarray] = {}


def speech_bed(seconds: float, *, peak: float = 0.5) -> np.ndarray:
    """One speech-like signal on both channels, the right a shade lower.

    The channels are not decorrelated by a delay, which is how the tonal beds
    get their stereo. A delay comb-filters the downmix, and the comb lands on
    exactly the band envelopes STOI reads, so the downmix feed would score worse
    than either channel for a reason that has nothing to do with the processing.
    A level difference alone keeps the downmix speech.

    The synthesis is cached: every speech item shares one bed, and the loop in
    :func:`_speech_like` is the slowest thing in this file.
    """
    frames = round(SAMPLE_RATE * seconds)
    if frames not in _SPEECH_CACHE:
        _SPEECH_CACHE[frames] = _speech_like(frames, SAMPLE_RATE)
    mono = _SPEECH_CACHE[frames]
    stereo = np.stack([mono, mono * 10.0 ** (-0.5 / 20.0)], axis=1)
    return _peak_normalize(stereo, peak)


def program_bed(seconds: float) -> np.ndarray:
    """A kick/hat pattern over bass and pad, building slowly.

    The one bed here with real transients, so the chain's limiter and
    compressor have something to act on rather than a stationary tone.
    """
    t = _time_axis(seconds)

    def pulse(period: float, attack: float, release: float) -> np.ndarray:
        beat = np.mod(t, period)
        return np.where(beat < attack, beat / attack, np.exp(-(beat - attack) / release))

    # Integrated, not multiplied by t: a swept frequency times the global t has
    # instantaneous frequency f + t*df/dt, which aliases once t grows.
    kick_f = 52.0 + 80.0 * np.exp(-np.mod(t, 0.5) * 40.0)
    kick = pulse(0.5, 0.002, 0.08) * np.sin(np.cumsum(2.0 * np.pi * kick_f / SAMPLE_RATE))
    hat = 0.12 * pulse(0.25, 0.001, 0.018) * np.sin(2.0 * np.pi * 8400.0 * t)
    bass = 0.3 * np.sin(2.0 * np.pi * 74.0 * t)
    pad_l = 0.16 * np.sin(2.0 * np.pi * 330.0 * t)
    pad_r = 0.16 * np.sin(2.0 * np.pi * 333.0 * t)

    build = 10.0 ** ((-10.0 + 10.0 * np.sin(2.0 * np.pi * t / 8.0 - np.pi / 2.0)) / 20.0)
    left = (0.8 * kick + hat + bass + pad_l) * build
    right = (0.78 * kick - hat + bass + pad_r) * build
    return _peak_normalize(np.stack([left, right], axis=1), 0.9)


# ------------------------------------------------------------------- defects


def plant_clicks(
    bed: np.ndarray,
    rng: np.random.Generator,
    *,
    count: int,
    amp_range: tuple[float, float] = (0.30, 0.85),
    width_samples: int = 2,
    edge_guard: int = 480,
) -> tuple[np.ndarray, dict]:
    """Add impulsive clicks at random positions, identical in both channels.

    Both channels get the same click because that is what a defect in the source
    looks like; a click present in one channel only would vanish from a downmix
    at a rate no metric row records.
    """
    frames = bed.shape[0]
    usable = frames - 2 * edge_guard - width_samples
    min_gap = max(width_samples * 4, usable // (count * 2))
    positions: list[int] = []
    while len(positions) < count:
        candidate = int(rng.integers(edge_guard, edge_guard + usable))
        if all(abs(candidate - p) >= min_gap for p in positions):
            positions.append(candidate)
    positions.sort()

    amplitudes = rng.uniform(amp_range[0], amp_range[1], size=count)
    signs = rng.choice(np.array([-1.0, 1.0]), size=count)
    out = bed.copy()
    for pos, amp, sign in zip(positions, amplitudes, signs, strict=True):
        out[pos : pos + width_samples, :] += sign * amp
    record = {
        "count": count,
        "positions_samples": [int(p) for p in positions],
        "amplitudes": [round(float(a * s), 6) for a, s in zip(amplitudes, signs, strict=True)],
        "width_samples": width_samples,
        "channels": "both",
    }
    return out, record


def plant_hum(
    bed: np.ndarray,
    *,
    fundamental_hz: float,
    harmonic_db: list[float],
) -> tuple[np.ndarray, dict]:
    """Add a hum fundamental and its harmonics at stated levels in dBFS."""
    seconds = bed.shape[0] / SAMPLE_RATE
    t = _time_axis(seconds)
    hum = np.zeros_like(t)
    harmonic_hz: list[float] = []
    for index, level_db in enumerate(harmonic_db, start=1):
        freq = fundamental_hz * index
        harmonic_hz.append(freq)
        amp = 10.0 ** (level_db / 20.0)
        hum += amp * np.sin(2.0 * np.pi * freq * t + 0.23 * index)
    out = bed + hum[:, None]
    record = {
        "fundamental_hz": fundamental_hz,
        "harmonic_count": len(harmonic_db),
        "harmonic_hz": [round(f, 4) for f in harmonic_hz],
        "harmonic_db": [round(float(d), 4) for d in harmonic_db],
        "channels": "both",
    }
    return out, record


def _longest_run(mask: np.ndarray) -> int:
    """Length of the longest True run in a 1-D boolean mask."""
    if not mask.any():
        return 0
    padded = np.concatenate(([False], mask, [False]))
    edges = np.flatnonzero(padded[1:] != padded[:-1])
    return int(np.max(edges[1::2] - edges[0::2]))


def plant_clip(bed: np.ndarray, *, threshold: float) -> tuple[np.ndarray, dict]:
    """Hard-clip the bed, and count what the clipping actually took.

    The clean reference is the bed itself rather than a louder pre-drive signal,
    so both sides of the comparison are storable in a WAV without clipping.
    """
    return np.clip(bed, -threshold, threshold), {"threshold": threshold}


def _clip_record(clean: np.ndarray, dirty: np.ndarray, threshold: float) -> dict:
    """Measure the clipping on the pair as it reads back from disk.

    ``threshold_in_file`` is not the requested threshold: the WAV writer scales
    by ``2**23 - 1`` and the reader divides by ``2**23``, so a plateau written at
    the requested value reads back a hundred-nanosecond-scale step below it. The
    declipper's detector is an inclusive ``>=`` against a level, so handing it
    the requested number finds nothing at all. The file's own plateau is the
    number a caller wants, and it is the one recorded.
    """
    step = 2.0 ** -(BIT_DEPTH - 1)
    per_channel = []
    total = 0
    longest = 0
    for ch in range(clean.shape[1]):
        mask = np.abs(clean[:, ch]) > np.abs(dirty[:, ch]) + 2.0 * step
        count = int(mask.sum())
        per_channel.append(count)
        total += count
        longest = max(longest, _longest_run(mask))
    return {
        "threshold": threshold,
        "threshold_in_file": round(float(np.max(np.abs(dirty))), 9),
        "clipped_samples": total,
        "clipped_samples_per_channel": per_channel,
        "clipped_ratio": round(total / float(clean.size), 8),
        "max_run_samples": longest,
    }


def _pink_noise(rng: np.random.Generator, frames: int) -> np.ndarray:
    """1/f noise by spectral shaping of white noise (unit RMS)."""
    white = rng.standard_normal(frames)
    spectrum = np.fft.rfft(white)
    freqs = np.arange(spectrum.size, dtype=np.float64)
    freqs[0] = 1.0
    shaped = np.fft.irfft(spectrum / np.sqrt(freqs), n=frames)
    rms = float(np.sqrt(np.mean(shaped**2)))
    return shaped / rms if rms > 0.0 else shaped


def plant_noise(
    bed: np.ndarray,
    rng: np.random.Generator,
    *,
    kind: str,
    snr_db: float,
) -> tuple[np.ndarray, dict]:
    """Add white or pink noise at a stated SNR against the bed's own power.

    The two channels get independent noise, which is what a real noise floor
    does and what makes a downmix row differ from a per-channel row.
    """
    frames = bed.shape[0]
    if kind == "white":
        noise = rng.standard_normal((frames, 2))
        noise /= np.sqrt(np.mean(noise**2, axis=0, keepdims=True))
    elif kind == "pink":
        noise = np.stack([_pink_noise(rng, frames) for _ in range(2)], axis=1)
    else:
        raise ValueError(f"unknown noise kind: {kind}")

    signal_power = float(np.mean(bed**2))
    target_power = signal_power / (10.0 ** (snr_db / 10.0))
    noise *= np.sqrt(target_power)

    before, after = quantize(bed), quantize(bed + noise)
    residual = after - before
    quantized_signal = float(np.mean(before**2))
    quantized_noise = float(np.mean(residual**2))
    record = {
        "kind": kind,
        "requested_snr_db": snr_db,
        "achieved_snr_db": round(10.0 * np.log10(quantized_signal / quantized_noise), 4),
        "noise_floor_dbfs": round(10.0 * np.log10(quantized_noise), 4),
        "channels": "independent",
    }
    return bed + noise, record


def _schroeder_t60(rir: np.ndarray, sample_rate: int) -> float:
    """Reverberation time from the impulse response's backward-integrated energy.

    A T30 fit -- the slope between -5 and -35 dB on the decay curve, extrapolated
    to 60 dB -- which is what ISO 3382 quotes when the curve does not stay
    straight for the full 60. It is measured rather than assumed because the
    planted number has to be the file's: the response is truncated, its first
    milliseconds carry the direct sound rather than the tail, and neither is
    visible in the requested value.
    """
    decay = np.cumsum((rir**2)[::-1])[::-1]
    decay_db = 10.0 * np.log10(np.maximum(decay / decay[0], 1e-20))
    seconds = np.arange(decay.size, dtype=np.float64) / sample_rate
    inside = (decay_db <= T60_FIT_UPPER_DB) & (decay_db >= T60_FIT_LOWER_DB)
    if int(inside.sum()) < 2:
        return float("nan")
    slope = float(np.polyfit(seconds[inside], decay_db[inside], 1)[0])
    return -60.0 / slope


def _synthetic_rir(
    rng: np.random.Generator, *, t60_sec: float, drr_db: float, predelay_ms: float
) -> np.ndarray:
    """A direct impulse plus an exponentially decaying noise tail.

    The envelope is ``exp(-3 ln10 t / T60)``, so the tail's *energy* is 60 dB
    down at T60. That is the same decay law ``dereverb_classical`` assumes when
    it turns its ``t60_sec`` knob into a late-power estimate, which is what makes
    the planted quantity and the knob the same quantity rather than two numbers
    that share a name.
    """
    frames = round(SAMPLE_RATE * t60_sec * RIR_SPAN_IN_T60)
    seconds = np.arange(frames, dtype=np.float64) / SAMPLE_RATE
    tail = rng.standard_normal(frames) * np.exp(-3.0 * np.log(10.0) * seconds / t60_sec)
    tail[: round(SAMPLE_RATE * predelay_ms * 1.0e-3)] = 0.0
    # The direct impulse carries unit energy, so the tail's total energy is the
    # direct-to-reverberant ratio outright and the ratio is set rather than read.
    tail *= np.sqrt(10.0 ** (-drr_db / 10.0) / float(np.sum(tail**2)))
    tail[0] += 1.0
    return tail


def plant_reverb(
    bed: np.ndarray,
    rng: np.random.Generator,
    *,
    t60_sec: float,
    drr_db: float,
    predelay_ms: float,
    peak: float = 0.9,
) -> tuple[np.ndarray, np.ndarray, dict]:
    """Convolve the bed with a synthetic room, and return the pair on one scale.

    Both channels get the same response, as the click and hum planters do. A
    per-channel tail would be decorrelated, and the downmix's
    direct-to-reverberant ratio would then sit below the recorded one by the
    decorrelation gain -- a difference no row records.

    This is the only planter that returns the clean side too. The reverberant sum
    peaks above the bed, so it has to be brought back inside full scale, and the
    factor belongs to the pair: applied to the dirty side alone it would be a
    level change, which segmental SNR reads as damage by construction.

    The tail is truncated at the bed's end rather than extending it, so both
    sides keep the same frame count and the item ends mid-decay.

    Two direct-to-reverberant ratios are recorded and they are different
    quantities, not a request and its error. ``rir_drr_db`` belongs to the
    impulse response and is exact; ``pair_drr_db`` is what the written pair
    carries, and it sits lower on sustained material because a held note keeps
    feeding the tail while the direct sound stays where it is.
    """
    rir = _synthetic_rir(rng, t60_sec=t60_sec, drr_db=drr_db, predelay_ms=predelay_ms)
    frames = bed.shape[0]
    wet = np.stack([np.convolve(bed[:, ch], rir)[:frames] for ch in range(bed.shape[1])], axis=1)
    scale = peak / max(float(np.max(np.abs(wet))), float(np.max(np.abs(bed))))
    clean, dirty = bed * scale, wet * scale

    before, after = quantize(clean), quantize(dirty)
    reverberant = after - before
    record = {
        "t60_requested_sec": t60_sec,
        "t60_measured_sec": round(_schroeder_t60(rir, SAMPLE_RATE), 6),
        "t60_measure": f"schroeder T30 ({T60_FIT_UPPER_DB:g} to {T60_FIT_LOWER_DB:g} dB) x2",
        "rir_drr_db": drr_db,
        "pair_drr_db": round(
            10.0 * np.log10(float(np.mean(before**2)) / float(np.mean(reverberant**2))), 4
        ),
        "predelay_ms": predelay_ms,
        "rir_seconds": round(rir.size / SAMPLE_RATE, 6),
        "tail_dropped_samples": int(rir.size - 1),
        "pair_scale": round(float(scale), 9),
        "channels": "both",
    }
    return clean, dirty, record


# -------------------------------------------------------------- item builders


def _write_pair(
    out_dir: Path, item_id: str, clean_q: np.ndarray, dirty_q: np.ndarray
) -> tuple[str, str]:
    sub = out_dir / SYNTHETIC_DIR
    sub.mkdir(parents=True, exist_ok=True)
    dirty_rel = f"{SYNTHETIC_DIR}/{item_id}.wav"
    clean_rel = f"{SYNTHETIC_DIR}/{item_id}_clean.wav"
    write_wav(out_dir / dirty_rel, dirty_q.astype(np.float32), SAMPLE_RATE, bits=BIT_DEPTH)
    write_wav(out_dir / clean_rel, clean_q.astype(np.float32), SAMPLE_RATE, bits=BIT_DEPTH)
    return dirty_rel, clean_rel


def _synthetic_entry(
    out_dir: Path,
    item_id: str,
    bed_name: str,
    seconds: float,
    clean: np.ndarray,
    dirty: np.ndarray,
    defects: dict,
    seed_pair: list[int],
    speech_bearing: bool = False,
) -> dict:
    peak = float(np.max(np.abs(dirty)))
    if peak > 1.0:
        raise ValueError(
            f"{item_id}: the planted signal peaks at {peak:.4f}; writing it would clip it "
            f"and plant a defect the manifest does not record"
        )
    clean_q = quantize(clean)
    dirty_q = quantize(dirty)
    dirty_rel, clean_rel = _write_pair(out_dir, item_id, clean_q, dirty_q)
    if "clip" in defects:
        written, _ = read_wav(out_dir / dirty_rel)
        reference, _ = read_wav(out_dir / clean_rel)
        defects["clip"] = _clip_record(
            np.asarray(reference, dtype=np.float64),
            np.asarray(written, dtype=np.float64),
            defects["clip"]["threshold"],
        )
    return {
        "id": item_id,
        "role": "synthetic",
        "bed": bed_name,
        "audio": dirty_rel,
        "clean_reference": clean_rel,
        "sample_rate": SAMPLE_RATE,
        "channels": 2,
        "frames": int(clean_q.shape[0]),
        "duration_seconds": round(seconds, 6),
        "bit_depth": BIT_DEPTH,
        "seed": seed_pair,
        "defects": defects,
        "usable_for": ["restoration", "mastering"],
        "speech_bearing": speech_bearing,
    }


def _noise_ensemble_entry(
    out_dir: Path,
    item_id: str,
    bed_name: str,
    seconds: float,
    clean: np.ndarray,
    *,
    kind: str,
    snr_db: float,
    seed: int,
    index: int,
    speech_bearing: bool = False,
) -> dict:
    """One clean reference and several independent draws of the same noise condition.

    The draws differ only in the noise realization: same bed, same kind, same
    requested SNR. That is what makes averaging across them meaningful, and it
    is why every other planted quantity stays out of the loop.

    The item's own ``audio`` and ``defects`` are the first draw's, so anything
    reading an item as a single file still reads a coherent one.
    """
    sub = out_dir / SYNTHETIC_DIR
    sub.mkdir(parents=True, exist_ok=True)
    clean_q = quantize(clean)
    clean_rel = f"{SYNTHETIC_DIR}/{item_id}_clean.wav"
    write_wav(out_dir / clean_rel, clean_q.astype(np.float32), SAMPLE_RATE, bits=BIT_DEPTH)

    draws: list[dict] = []
    for draw in range(NOISE_REALIZATIONS):
        seed_triple = [seed, index, draw]
        rng = np.random.default_rng(seed_triple)
        dirty, noise = plant_noise(clean, rng, kind=kind, snr_db=snr_db)
        peak = float(np.max(np.abs(dirty)))
        if peak > 1.0:
            raise ValueError(
                f"{item_id} draw {draw}: the planted signal peaks at {peak:.4f}; writing it "
                f"would clip it and plant a defect the manifest does not record"
            )
        rel = f"{SYNTHETIC_DIR}/{item_id}_r{draw}.wav"
        write_wav(out_dir / rel, quantize(dirty).astype(np.float32), SAMPLE_RATE, bits=BIT_DEPTH)
        draws.append(
            {"index": draw, "seed": seed_triple, "audio": rel, "defects": {"noise": noise}}
        )

    return {
        "id": item_id,
        "role": "synthetic",
        "bed": bed_name,
        "audio": draws[0]["audio"],
        "clean_reference": clean_rel,
        "sample_rate": SAMPLE_RATE,
        "channels": 2,
        "frames": int(clean_q.shape[0]),
        "duration_seconds": round(seconds, 6),
        "bit_depth": BIT_DEPTH,
        "seed": [seed, index],
        "defects": draws[0]["defects"],
        "usable_for": ["restoration", "mastering"],
        "speech_bearing": speech_bearing,
        "realizations": draws,
    }


def build_synthetic(out_dir: Path, seed: int) -> list[dict]:
    """Build every synthetic item, each from its own derived generator."""
    entries: list[dict] = []

    def rng_for(index: int) -> np.random.Generator:
        return np.random.default_rng([seed, index])

    # 0: clicks on a sine bed -- the easiest case for a declicker to get right.
    # The bed sits low so a click is impulsive against it rather than merely loud,
    # and so the sum stays inside full scale: a click the file clipped would be a
    # second defect at an amplitude the manifest does not carry.
    seconds = 1.0
    clean = sine_bed(seconds, amp=0.15)
    dirty, clicks = plant_clicks(clean, rng_for(0), count=12, amp_range=(0.45, 0.80))
    entries.append(
        _synthetic_entry(
            out_dir, "sine_click", "sine", seconds, clean, dirty, {"click": clicks}, [seed, 0]
        )
    )

    # 1: denser clicks over harmonic content, where an interpolator has more to lose.
    seconds = 1.0
    clean = _peak_normalize(chord_bed(seconds), 0.15)
    dirty, clicks = plant_clicks(
        clean, rng_for(1), count=40, amp_range=(0.45, 0.80), width_samples=3
    )
    entries.append(
        _synthetic_entry(
            out_dir, "chord_click", "chord", seconds, clean, dirty, {"click": clicks}, [seed, 1]
        )
    )

    # 2/3: the two mains frequencies, with different harmonic counts.
    seconds = 1.0
    clean = sine_bed(seconds)
    dirty, hum = plant_hum(clean, fundamental_hz=50.0, harmonic_db=[-26.0, -32.0, -38.0, -44.0])
    entries.append(
        _synthetic_entry(
            out_dir, "sine_hum50", "sine", seconds, clean, dirty, {"hum": hum}, [seed, 2]
        )
    )

    seconds = 1.0
    clean = chord_bed(seconds)
    dirty, hum = plant_hum(clean, fundamental_hz=60.0, harmonic_db=[-24.0, -30.0, -36.0])
    entries.append(
        _synthetic_entry(
            out_dir, "chord_hum60", "chord", seconds, clean, dirty, {"hum": hum}, [seed, 3]
        )
    )

    # 4/5: many short clipped runs, then few long ones. A steady sine dwells near
    # its peak, so its threshold sits far higher than the chord's for a lighter cut.
    seconds = 0.5
    clean = _peak_normalize(sine_bed(seconds), 0.95)
    dirty, clip = plant_clip(clean, threshold=0.945)
    entries.append(
        _synthetic_entry(
            out_dir, "sine_clip", "sine", seconds, clean, dirty, {"clip": clip}, [seed, 4]
        )
    )

    seconds = 0.5
    clean = _peak_normalize(chord_bed(seconds), 0.95)
    dirty, clip = plant_clip(clean, threshold=0.62)
    entries.append(
        _synthetic_entry(
            out_dir, "chord_clip", "chord", seconds, clean, dirty, {"clip": clip}, [seed, 5]
        )
    )

    # 6/7: white and pink at the same SNR, so a denoiser's spectral assumption
    # shows. These two carry a draw ensemble; see NOISE_REALIZATIONS.
    seconds = 1.0
    entries.append(
        _noise_ensemble_entry(
            out_dir,
            "sine_white_noise",
            "sine",
            seconds,
            sine_bed(seconds),
            kind="white",
            snr_db=12.0,
            seed=seed,
            index=6,
        )
    )

    # The bed sits lower than the chord's natural peak so that every draw fits,
    # not just the first: the loudest draw decides the headroom, and scaling the
    # whole item leaves the SNR and every metric that matters here unchanged.
    seconds = 1.0
    entries.append(
        _noise_ensemble_entry(
            out_dir,
            "chord_pink_noise",
            "chord",
            seconds,
            _peak_normalize(chord_bed(seconds), 0.6),
            kind="pink",
            snr_db=12.0,
            seed=seed,
            index=7,
        )
    )

    # 8: three defects at once, for the chained case a single-defect item cannot show.
    seconds = 2.0
    clean = _peak_normalize(chord_bed(seconds), 0.30)
    dirty, noise = plant_noise(clean, rng_for(8), kind="white", snr_db=18.0)
    dirty, hum = plant_hum(dirty, fundamental_hz=50.0, harmonic_db=[-28.0, -34.0])
    dirty, clicks = plant_clicks(dirty, rng_for(9), count=16, amp_range=(0.40, 0.55))
    entries.append(
        _synthetic_entry(
            out_dir,
            "chord_noise_hum_click",
            "chord",
            seconds,
            clean,
            dirty,
            {"noise": noise, "hum": hum, "click": clicks},
            [seed, 8, 9],
        )
    )
    return entries


def build_speech(out_dir: Path, seed: int) -> list[dict]:
    """The same five defects again, on speech-bearing material.

    These are the only items STOI is read on. They run longer than the tonal
    ones because STOI's silent-frame removal drops roughly a quarter of the
    signal and the measure returns NaN under about a second: a short speech item
    would be speech-bearing and still unreadable.
    """
    seconds = 2.5
    entries: list[dict] = []

    def rng_for(index: int) -> np.random.Generator:
        return np.random.default_rng([seed, 100 + index])

    def entry(item_id: str, clean: np.ndarray, dirty: np.ndarray, defects: dict, i: int) -> dict:
        return _synthetic_entry(
            out_dir, item_id, "speech", seconds, clean, dirty, defects, [seed, 100 + i], True
        )

    clean = speech_bed(seconds, peak=0.15)
    dirty, clicks = plant_clicks(clean, rng_for(0), count=14, amp_range=(0.45, 0.80))
    entries.append(entry("speech_click", clean, dirty, {"click": clicks}, 0))

    clean = speech_bed(seconds, peak=0.95)
    dirty, clip = plant_clip(clean, threshold=0.45)
    entries.append(entry("speech_clip", clean, dirty, {"clip": clip}, 1))

    clean = speech_bed(seconds, peak=0.5)
    dirty, hum = plant_hum(clean, fundamental_hz=50.0, harmonic_db=[-26.0, -32.0, -38.0, -44.0])
    entries.append(entry("speech_hum50", clean, dirty, {"hum": hum}, 2))

    # 0 dB rather than the tonal items' 12 dB. STOI on this bed reads 0.98 at
    # 12 dB, which leaves the denoiser nothing to win and only damage to lose:
    # measured across the bed, the denoiser's STOI delta is negative at 18 and
    # 12 dB, positive at 6 and 0, and negative again at -6. A row placed where
    # the metric cannot improve tests nothing in the improving direction.
    entries.append(
        _noise_ensemble_entry(
            out_dir,
            "speech_white_noise",
            "speech",
            seconds,
            speech_bed(seconds, peak=0.5),
            kind="white",
            snr_db=0.0,
            seed=seed,
            index=103,
            speech_bearing=True,
        )
    )
    entries.append(
        _noise_ensemble_entry(
            out_dir,
            "speech_pink_noise",
            "speech",
            seconds,
            speech_bed(seconds, peak=0.5),
            kind="pink",
            snr_db=0.0,
            seed=seed,
            index=104,
            speech_bearing=True,
        )
    )

    # The one item both families can read at once. STOI needs speech, a clean
    # reference and a planted defect; short-term loudness spread needs length and
    # slow level variation. Nothing else in the corpus has all four, so the pair
    # of metrics could not be crossed on a single item at all.
    #
    # The defect is hum because it is deterministic -- a noise defect here would
    # pull a 16-second item into the draw ensemble -- and because STOI moves
    # clearly on it. The hum sits at a fixed level while the speech steps
    # between section levels, which is what hum does and which leaves the local
    # signal-to-hum ratio different in every section.
    # The sections step further than the chord item's do, and the hum sits lower,
    # because both effects narrow the spread here. Speech is syllable-gated at
    # 2.3 Hz, which the 3 s window averages away, so the same steps that give the
    # chord 4.62 LU give speech 4.52; and hum at a fixed level fills the quiet
    # sections' floor, which took the first attempt down to 2.94 LU.
    long_seconds = 16.0
    clean = apply_sections(
        speech_bed(long_seconds, peak=1.0), 4.0, [-3.0, -21.0, -9.0, -27.0], peak=0.5
    )
    dirty, hum = plant_hum(clean, fundamental_hz=50.0, harmonic_db=[-34.0, -40.0, -46.0, -52.0])
    entries.append(
        _synthetic_entry(
            out_dir,
            "speech_dynamic_hum50",
            "speech_sections",
            long_seconds,
            clean,
            dirty,
            {"hum": hum},
            [seed, 105],
            True,
        )
    )

    return entries


def build_reverb(out_dir: Path, seed: int) -> list[dict]:
    """Reverberant material, for the defect the other builders do not plant.

    A dereverberator's knobs are only reachable on a signal that has a tail. Swept
    over dry material every one of them reads as inert, which makes a knob that is
    wired indistinguishable from one that is not -- the reading is empty rather
    than red, and it is the failure this half of the corpus exists to prevent.

    Three items. Two speech beds differing in nothing but reverberation time, so a
    response can be read against the planted quantity rather than against a single
    condition; and one tonal bed, so that response is not read off one talker. The
    two speech items are the only reverberant rows STOI may be read on.

    All three run past the 3 s short-term window, which is what leaves every
    metric column live on the speech pair instead of null.
    """
    seconds = 4.0
    entries: list[dict] = []

    conditions = (
        ("speech_reverb_short", "speech", True, 0.35),
        ("speech_reverb_long", "speech", True, 1.20),
        ("chord_reverb_long", "chord", False, 1.20),
    )
    for index, (item_id, bed_name, speech, t60_sec) in enumerate(conditions):
        bed = speech_bed(seconds, peak=0.5) if speech else _peak_normalize(chord_bed(seconds), 0.5)
        clean, dirty, reverb = plant_reverb(
            bed,
            np.random.default_rng([seed, 200 + index]),
            t60_sec=t60_sec,
            drr_db=REVERB_DRR_DB,
            predelay_ms=REVERB_PREDELAY_MS,
        )
        entries.append(
            _synthetic_entry(
                out_dir,
                item_id,
                bed_name,
                seconds,
                clean,
                dirty,
                {"reverb": reverb},
                [seed, 200 + index],
                speech,
            )
        )
    return entries


def _dynamics_entry(
    out_dir: Path, item_id: str, bed_name: str, seconds: float, audio: np.ndarray
) -> dict:
    """Write one mastering-only item: no defect, no reference, long enough to measure."""
    peak = float(np.max(np.abs(audio)))
    if peak > 1.0:
        raise ValueError(f"{item_id}: the bed peaks at {peak:.4f} and would be clipped on write")
    quantized = quantize(audio)
    sub = out_dir / DYNAMICS_DIR
    sub.mkdir(parents=True, exist_ok=True)
    rel = f"{DYNAMICS_DIR}/{item_id}.wav"
    write_wav(out_dir / rel, quantized.astype(np.float32), SAMPLE_RATE, bits=BIT_DEPTH)
    return {
        "id": item_id,
        "role": "synthetic",
        "bed": bed_name,
        "audio": rel,
        "clean_reference": None,
        "sample_rate": SAMPLE_RATE,
        "channels": 2,
        "frames": int(quantized.shape[0]),
        "duration_seconds": round(seconds, 6),
        "bit_depth": BIT_DEPTH,
        "seed": None,
        "defects": None,
        "usable_for": ["mastering"],
        "speech_bearing": False,
    }


def build_dynamics(out_dir: Path) -> list[dict]:
    """The long beds the chain metrics need, and the restoration metrics do not.

    Restoration metrics are frame-averaged and read fine off half a second, so
    nothing here is a restoration item: these exist because short-term loudness
    spread has no value at all under 3.1 s and no *range* on a steady signal.
    """
    entries = []

    # Four sections at stated levels: a distribution with two clear modes, which
    # is the shape a compressor narrows most visibly.
    seconds = 16.0
    entries.append(
        _dynamics_entry(
            out_dir,
            "dynamic_sections",
            "chord_sections",
            seconds,
            section_bed(seconds, 4.0, [-6.0, -18.0, -10.0, -22.0]),
        )
    )

    # One slow swell, period well past the measurement window.
    seconds = 16.0
    entries.append(
        _dynamics_entry(
            out_dir,
            "slow_swell",
            "chord_swell",
            seconds,
            swell_bed(seconds, period_seconds=8.0, depth_db=9.0),
        )
    )

    # Program-like material: transients under a slow build.
    seconds = 12.0
    entries.append(
        _dynamics_entry(out_dir, "program_build", "program", seconds, program_bed(seconds))
    )
    return entries


def build_listening(out_dir: Path) -> list[dict]:
    """Import the long listening fixtures rather than re-deriving them.

    These are slower than everything else here -- they are generated one sample
    at a time by their own script, and they are four seconds each.
    """
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    import mastering_generate_listening_corpus as legacy

    fixtures = {
        "transients_dry": legacy.transient_fixture,
        "clipped_tone_dry": legacy.clipped_fixture,
        "noise_hum_dry": legacy.noise_hum_fixture,
        "stereo_mix_dry": legacy.stereo_mix_fixture,
    }
    sub = out_dir / LISTENING_DIR
    sub.mkdir(parents=True, exist_ok=True)

    entries: list[dict] = []
    for item_id, build in fixtures.items():
        started = time.perf_counter()
        audio = quantize(np.asarray(build(), dtype=np.float64))
        rel = f"{LISTENING_DIR}/{item_id}.wav"
        write_wav(out_dir / rel, audio.astype(np.float32), legacy.SAMPLE_RATE, bits=BIT_DEPTH)
        entries.append(
            {
                "id": item_id,
                "role": "listening",
                "bed": "legacy_fixture",
                "audio": rel,
                "clean_reference": None,
                "sample_rate": legacy.SAMPLE_RATE,
                "channels": 2,
                "frames": int(audio.shape[0]),
                "duration_seconds": round(audio.shape[0] / legacy.SAMPLE_RATE, 6),
                "bit_depth": BIT_DEPTH,
                "seed": None,
                "defects": None,
                "usable_for": ["mastering"],
                "speech_bearing": False,
                "source": "tools/mastering_generate_listening_corpus.py",
            }
        )
        print(f"  {item_id:<24} {time.perf_counter() - started:6.2f}s")
    return entries


def collect_recorded(out_dir: Path) -> list[dict]:
    """Fold in whatever captures sit under ``recorded/`` with a manifest.

    Captures are never committed, so this half is empty in a clean checkout and
    the absence is normal rather than an error.
    """
    index = out_dir / RECORDED_DIR / MANIFEST_NAME
    if not index.exists():
        return []
    declared = json.loads(index.read_text())
    entries: list[dict] = []
    for item in declared.get("items", []):
        rel = f"{RECORDED_DIR}/{item['audio']}"
        path = out_dir / rel
        if not path.exists():
            raise FileNotFoundError(f"{index} declares {item['audio']}, which is not there")
        audio, sample_rate = read_wav(path)
        clean_rel = None
        if item.get("clean_reference"):
            clean_rel = f"{RECORDED_DIR}/{item['clean_reference']}"
            if not (out_dir / clean_rel).exists():
                raise FileNotFoundError(f"{index} declares a clean reference that is not there")
        usable = ["mastering"] if clean_rel is None else ["restoration", "mastering"]
        entries.append(
            {
                "id": item["id"],
                "role": "recorded",
                "bed": item.get("source", "recorded"),
                "audio": rel,
                "clean_reference": clean_rel,
                "sample_rate": int(sample_rate),
                "channels": int(audio.shape[1]) if audio.ndim > 1 else 1,
                "frames": int(audio.shape[0]),
                "duration_seconds": round(audio.shape[0] / float(sample_rate), 6),
                "bit_depth": None,
                "seed": None,
                "defects": item.get("defects"),
                "usable_for": usable,
                "speech_bearing": bool(item.get("speech_bearing", False)),
                "notes": item.get("notes", ""),
            }
        )
    return entries


# ------------------------------------------------------------------------ main


def measure_chain_readiness(out_dir: Path, items: list[dict]) -> None:
    """Record, per item, whether the chain's dynamics metric can say anything about it.

    A row whose spread is NaN is not a row that scored badly; it is a row the
    metric could not be computed on. Recording the measured value here lets a
    later comparison reject such a row by reading the manifest, instead of
    discovering it as a NaN in a ledger that a pass criterion then has to guess
    the meaning of.

    Measured on the item's own audio, before any processing, and on the same
    stereo shape the runner feeds the chain.
    """
    try:
        import metrics_chain
    except (ImportError, OSError) as exc:
        # The chain metrics reach the C library, which this script otherwise does
        # not need. Corpus generation is not held hostage to a built dylib; the
        # manifest says the spread was not measured rather than claiming a value.
        print(f"chain readiness: not measured ({exc}); spread recorded as null")
        metrics_chain = None  # type: ignore[assignment]

    for item in items:
        duration = float(item["duration_seconds"])
        spread = None
        if metrics_chain is not None:
            audio, sample_rate = read_wav(out_dir / item["audio"])
            value = metrics_chain.short_term_spread(
                np.asarray(audio, dtype=np.float64), sample_rate
            )
            spread = None if not np.isfinite(value) else round(float(value), 4)
        item["chain_readiness"] = {
            "duration_seconds": round(duration, 6),
            "reaches_short_term_window": duration > SHORT_TERM_WINDOW_SECONDS,
            "short_term_spread_lu": spread,
            "measured_with": None if metrics_chain is None else "metrics_chain.short_term_spread",
        }


def generate(out_dir: Path, *, seed: int, listening: bool) -> dict:
    """Write every item and return the manifest that describes them."""
    out_dir.mkdir(parents=True, exist_ok=True)

    started = time.perf_counter()
    print("synthetic:")
    items = build_synthetic(out_dir, seed)
    print(f"  {len(items)} items in {time.perf_counter() - started:.2f}s")

    started = time.perf_counter()
    print("speech:")
    speech = build_speech(out_dir, seed)
    items += speech
    print(f"  {len(speech)} items in {time.perf_counter() - started:.2f}s")

    started = time.perf_counter()
    print("reverb:")
    reverb = build_reverb(out_dir, seed)
    items += reverb
    print(f"  {len(reverb)} items in {time.perf_counter() - started:.2f}s")

    started = time.perf_counter()
    print("dynamics:")
    items += build_dynamics(out_dir)
    print(f"  3 items in {time.perf_counter() - started:.2f}s")

    if listening:
        print("listening:")
        items += build_listening(out_dir)

    recorded = collect_recorded(out_dir)
    if recorded:
        print(f"recorded:\n  {len(recorded)} items picked up")
    items += recorded

    measure_chain_readiness(out_dir, items)

    manifest = {
        "schema": MANIFEST_SCHEMA,
        "generator": "tools/mastering-eval/corpus.py",
        "contract": "tools/mastering-eval/docs/objective.md",
        "sample_rate": SAMPLE_RATE,
        "bit_depth": BIT_DEPTH,
        "seed": seed,
        "items": items,
    }
    (out_dir / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def default_output_dir() -> Path:
    return Path(__file__).resolve().parent / "audio"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=default_output_dir(),
        help="where the audio and the manifest go (regenerable; not committed)",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument(
        "--skip-listening",
        action="store_true",
        help="leave out the four four-second fixtures, which dominate the run time",
    )
    args = parser.parse_args()

    started = time.perf_counter()
    manifest = generate(args.output_dir, seed=args.seed, listening=not args.skip_listening)
    # Counted per draw, not per item: a noise item writes one file per draw and
    # a total that ignored them would understate what was generated.
    total = sum(
        item["duration_seconds"] * max(1, len(item.get("realizations", [])))
        for item in manifest["items"]
    )
    print(
        f"{len(manifest['items'])} items, {total:.1f}s of audio, "
        f"{time.perf_counter() - started:.2f}s wall -> {args.output_dir / MANIFEST_NAME}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
