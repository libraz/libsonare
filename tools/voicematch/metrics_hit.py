"""The percussion metric set: strike, tone, pitch drop, and per-hit deltas."""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass

import numpy as np
from metrics_bands import (
    BAND_FLOOR_DB,
    OCTAVE_CENTERS,
    OCTAVE_RATIO,
    THIRD_OCTAVE_CENTERS,
    THIRD_OCTAVE_RATIO,
    band_edge_index,
)
from metrics_decay import _band_decay, _band_power
from metrics_modal import measure_modes
from metrics_signal import _db, _rms_envelope, _spectrum, channel_width
from smf import Note

# Longest stretch of one hit that is analyzed.
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

# A hit's envelope is read on a far finer grid than a sustained note's. Time to
# peak is single-digit milliseconds for most of the kit, so the 5 ms hop that
# resolves a bowed attack quantises a snare's to 5, 10 or 15 and reports the
# quantisation rather than the attack.
HIT_ENVELOPE_HOP_MS = 0.5
HIT_ENVELOPE_WIN_MS = 2.0

#: Widest the centroid is ever integrated over, when the capture set no ceiling
#: of its own. Above it a kit carries no content a listener places the sound by.
CENTROID_MAX_HZ = 16000.0

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

# Below this the attack measures the ruler. An RMS window half full of a step is
# already 3.01 dB down, so the first frame that can clear the tolerance sits half
# a window in whatever the hit does before it: a step, a single-sample impulse
# and ramps of 0.1 and 0.5 ms all measure 1.00 ms. Above the floor the reading is
# monotone but reads low -- a true 5 ms rise comes back 4, a 10 comes back 9 and
# a 30 comes back 25 -- so a model pinned here against a reference well above it
# understates the gap and never invents one.
ATTACK_FLOOR_MS = HIT_ENVELOPE_WIN_MS / 2.0


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


HIT_TONE_WINDOW_S = 0.30
#: Bottom of the band a hit's tonality is read over. Under it is the capture's
#: own rumble, and a kick's fundamental is the one thing in a kit whose peakiness
#: nobody is in doubt about.
FLATNESS_LOW_HZ = 50.0
#: How far under the window's loudest bin a null is taken to be the transform's
#: rather than the hit's. A geometric mean is decided by its smallest terms, so
#: without a floor one cancelled bin carries the whole reading.
FLATNESS_FLOOR_DB = 120.0
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


def spectral_flatness_db(freqs: np.ndarray, mag: np.ndarray,
                         max_band_hz: float | None) -> float | None:
    """Geometric over arithmetic mean of the power spectrum, in dB.

    How much of a hit stands in lines rather than lying in a continuum: a struck
    bar or a cowbell runs tens of dB under a shaker, and 0 dB is a flat spectrum.
    The band profile cannot see it at all — a tone per 1/3-octave band carrying
    that band's energy has the same `bands_db` as the noise it was built from, to
    a hundredth of a decibel, with the same tilt and the same centroid. What it
    does NOT resolve is where inside one band the energy sits: a line and a
    filled quarter-octave over the same floor come back about a decibel apart,
    which is under the references' own disagreement, so narrowness within a band
    stays unmeasured.

    The percussion counterpart of `tnr_db`, which cannot be computed here: that
    one is the power inside windows around a harmonic ladder, and a drum hit has
    no fundamental to build a ladder on. A measure that needs no target
    frequencies at all is the only one available, and on the metal it is also the
    more trustworthy — `measure_modes` returns twelve peaks for a cymbal, and
    those peaks move by 14 % of their frequency between velocities of the same
    instrument, so a mask built from them would be measuring the extractor.

    Cut at the capture's own ceiling, for the reason the band profile is: above
    it the spectrum is the recording chain's roll-off, which is smooth and would
    read as tone.
    """
    ceiling = min(CENTROID_MAX_HZ,
                  float("inf") if max_band_hz is None
                  else max_band_hz * THIRD_OCTAVE_RATIO)
    band = (freqs >= FLATNESS_LOW_HZ) & (freqs <= ceiling)
    power = np.asarray(mag[band], dtype=np.float64) ** 2
    if power.size < 16:
        return None
    top = float(power.max())
    if top <= 0.0:
        return None
    power = np.maximum(power, top * 10.0 ** (-FLATNESS_FLOOR_DB / 10.0))
    geometric = float(np.exp(np.mean(np.log(power))))
    arithmetic = float(np.mean(power))
    return round(10.0 * math.log10(geometric / arithmetic), 2)


def hit_tone(seg: np.ndarray, sr: int, *, max_band_hz: float | None = None) -> dict:
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
             "tone_f0_hz": None, "tone_lowest_hz": None, "flatness_db": None}
    n = min(len(seg), int(HIT_TONE_WINDOW_S * sr))
    if n < 512:
        return empty
    freqs, mag = _spectrum(np.asarray(seg[:n], dtype=np.float64), sr)
    # Flatness is read here rather than off the hit's whole spectrum because
    # this window is a fixed length and that one is not: a geometric mean moves
    # with the bin width, and two renders whose strikes land at different
    # moments would be compared at different resolutions.
    flatness = spectral_flatness_db(freqs, mag, max_band_hz)
    modes = measure_modes(freqs, mag)
    if not modes:
        return {**empty, "flatness_db": flatness}
    lowest = modes[0][0]
    strongest = max(modes, key=lambda m: m[1])[0]
    return {
        "flatness_db": flatness,
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
    #: The attack sat at or under what the envelope window can resolve, so the
    #: number is the ruler and not the hit -- the counterpart of `decay_capped`
    #: at the other end. A step, an impulse and a 0.5 ms ramp all measure the
    #: same, because an RMS window half full of a step is already within the
    #: -3 dB the attack is read at.
    attack_floored: bool
    decay_ms: float
    decay_capped: bool
    crest_db: float
    level_db: float
    #: How peaky the hit's spectrum is, and how wide its image. Both are `None`
    #: rather than 0.0 where they could not be read — a hit too short to
    #: transform, and a mono render, are absences and not a noise floor or a
    #: centred source.
    flatness_db: float | None
    stereo_width: float | None
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


def analyze_hit(mono: np.ndarray, sr: int, note: Note, window_end: float, *,
                max_band_hz: float | None = None,
                stereo: np.ndarray | None = None) -> HitMetrics:
    """Compute the percussion metric set for one hit.

    The window runs from the strike to `window_end` (the next hit, or the end of
    the render), capped at `HIT_MAX_SEC`. The note's own duration is ignored:
    a drum is a one-shot and its note-off carries no information.

    The strike is located rather than assumed to be at the note-on, because a
    hosted plugin's is not: leading silence inside the window inflates time to
    peak by exactly its own length, dilutes the RMS the crest and level are
    measured against, and tilts every per-band decay fit.

    `max_band_hz` is the reference's own measurable ceiling (`shared_band_edge`).
    Bands above it are reported at the floor and, more importantly, are excluded
    from the normalisation, so the profile below the edge does not move when the
    model has content the capture could not have recorded. Both sides of a
    comparison must be measured with the same value or the profiles are
    normalised against different things.

    It reaches every field a comparison reads, which is four: the 1/3-octave
    profile, the per-octave decay, the centroid's integration range and the
    flatness band. They were not all cut at first, and a partial cut is the worst
    of the three states — the profile stops at the edge while the centroid keeps
    integrating a chain's roll-off, so one gated dimension charges the model for
    the top octave the other has already agreed is unmeasurable. `peak_band_hz`
    deliberately stays full-range, because a wash that peaks above the
    reference's ceiling is exactly what that field exists to show.

    `stereo` is the same audio with its channels, when the render has two. It is
    passed in rather than measured by the caller so the image is read over the
    window the rest of the hit is read over: a kit's recording is mostly tail
    padding, and a correlation taken across that reports the silence as a source
    in the middle.
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
    keep_octaves = band_edge_index(max_band_hz, OCTAVE_CENTERS)
    top = float(bands_db[:keep].max())
    bands_db = np.maximum(bands_db - top, BAND_FLOOR_DB)
    if keep < len(bands_db):
        bands_db[keep:] = BAND_FLOOR_DB
    peak_band = THIRD_OCTAVE_CENTERS[int(np.argmax(bands))]

    # Cut at whatever the kept bands cover, for the reason the profile is:
    # a capture that cannot hear its own cymbals reports a centroid the chain
    # decided, and a model with a real wash is charged the difference.
    ceiling = CENTROID_MAX_HZ if max_band_hz is None else min(
        CENTROID_MAX_HZ, max_band_hz * THIRD_OCTAVE_RATIO)
    in_range = freqs <= ceiling
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

    tone = hit_tone(seg, sr, max_band_hz=max_band_hz)
    drop = pitch_drop(seg, sr, tone["tone_f0_hz"])

    return HitMetrics(
        stereo_width=channel_width(stereo, on, min(end, len(mono))),
        note=note.note,
        velocity=note.velocity,
        bands_db=[round(float(v), 2) for v in bands_db],
        peak_band_hz=peak_band,
        **tone,
        **drop,
        band_decay_db_s=[
            None if v is None or i >= keep_octaves else round(v, 2)
            for i, v in enumerate(_band_decay(seg, sr, OCTAVE_CENTERS, OCTAVE_RATIO))],
        centroid_hz=round(centroid, 1),
        onset_ms=round((onset - note.start) * 1000.0, 2),
        attack_ms=round(attack_ms, 2),
        attack_floored=attack_ms <= ATTACK_FLOOR_MS,
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
        "flatness_delta_db": _delta(model.flatness_db, oracle.flatness_db, 2),
        "stereo_delta": _delta(model.stereo_width, oracle.stereo_width, 3),
    }


def _delta(model: float | None, oracle: float | None, digits: int) -> float | None:
    """A difference, or `None` where either side had no reading to difference."""
    if model is None or oracle is None:
        return None
    return round(model - oracle, digits)
