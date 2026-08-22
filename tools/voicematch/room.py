"""Ambience: measure the room an oracle was recorded in, and put the model in it.

Some instruments are never heard dry. A church organ is voiced *for* its
building, an orchestral harp for a hall, and a sampled or VST rendering of
either carries that space baked into the samples — it cannot be switched off.
Comparing a dry model render against such an oracle makes every timbre metric
lie in the same direction: the release looks far too long, the tone-to-noise
ratio far too low, the sustain slope far too flat. A fitter handed that loss
spends its knobs chasing the building.

So the room is treated as a nuisance parameter. `estimate_room` measures the
oracle's reverberation from its own note tails, `synth_room_ir` builds an
impulse response with those properties, and `apply_room` convolves it onto the
*model* before any metric is computed. What remains in the loss is the
instrument.

This is deliberately harness-side rather than a libsonare render option: the
room is a linear post-filter, so one model render can be auditioned in many
candidate rooms without re-rendering, which is what makes fitting the room
alongside the voice affordable. `match_sends` then translates a measured room
back into libsonare's own ambience controls, which is what a fitted result has
to be expressed in to be usable.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict

import numpy as np

# Octave-band centres for the band-wise decay measurement. Below ~125 Hz a
# note-tail estimate is dominated by the fundamental itself rather than by the
# room, and above ~8 kHz air absorption makes the tail too short to measure
# from music; both ends are extrapolated from their neighbour.
BANDS_HZ = (125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0)

# Schroeder backward-integration is fitted over this dB span below the decay's
# onset. -5..-25 dB (T20) rather than the full -60 dB, because a music tail is
# only ever a few tens of dB above the noise floor before the next note.
FIT_DB_HI = -5.0
FIT_DB_LO = -25.0


@dataclass
class Room:
    """A measured (or fitted) acoustic space."""

    rt60_s: float
    """Broadband reverberation time in seconds."""
    hf_ratio: float
    """RT60 at 4 kHz divided by RT60 at 500 Hz. <1 = an absorbent, damped room."""
    tail_db: float
    """Energy in the notes over energy in the tails after note-off, in dB.

    Not the textbook direct-to-reverberant ratio, which needs an impulse
    response nobody has here. This is the quantity music can actually give up:
    how loud what remains after the player stops is, relative to what they
    played. Large = close and dry, small = distant and wet, and it is the
    measure both the estimate and the send search use, so they agree.
    """
    predelay_ms: float
    """Gap between the direct sound and the onset of the reverberant field."""

    tail_window_s: float = 0.0
    """Shortest silence the estimate had to measure a decay in, in seconds.

    Zero on a room that was constructed rather than measured. See `truncated`.
    """

    def to_dict(self) -> dict:
        return asdict(self)

    def truncated(self) -> bool:
        """True when the probe's silences were too short to hold the decay.

        A T20 fit needs the tail to fall 25 dB, which takes RT60 * 25/60 of a
        second. When the next note arrives before that, the slope is fitted to
        the part of the decay that fits in the window and the estimate is biased
        — usually short, since what remains is the steepest early part. The
        number is still the best available and is used, but a caller that can
        choose its probe should choose a longer silence instead (the
        `room-probe` pattern exists for this).
        """
        return self.rt60_s > 0.0 and self.tail_window_s < self.rt60_s * 25.0 / 60.0

    def is_dry(self) -> bool:
        """True when the space is too small or too quiet to be worth modelling.

        The floor is 0.35 s rather than something smaller because a decay that
        short cannot be told apart from the instrument's own release by a
        note-tail measurement — a dry reference organ render measures 0.49 s
        from its pipes alone. Small rooms are therefore reported as no room,
        which is the cheap direction to be wrong in: a room that is missed
        leaves a small residual, a room that is invented is convolved onto
        every model render and corrupts every metric it was added to protect.

        The tail-level bound is a backstop, set loose because `tail_db` depends
        on how long the notes are relative to the decay (a room measuring 31 dB
        under one-second notes measures far less under short ones) and a tight
        threshold would reject real rooms.
        """
        return self.rt60_s < 0.35 or self.tail_db > 35.0


DRY = Room(rt60_s=0.0, hf_ratio=1.0, tail_db=99.0, predelay_ms=0.0)


def _octave_band(x: np.ndarray, sr: int, centre_hz: float) -> np.ndarray:
    """One octave band of `x`, via a zero-phase FFT brick-wall.

    Zero-phase because the decay slope is the only thing measured here and a
    causal filter's own ring-down would be added to the room's.
    """
    lo = centre_hz / np.sqrt(2.0)
    hi = centre_hz * np.sqrt(2.0)
    n = int(2 ** np.ceil(np.log2(max(len(x), 2))))
    spec = np.fft.rfft(x, n)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    spec[(freqs < lo) | (freqs > hi)] = 0.0
    return np.fft.irfft(spec, n)[: len(x)]


def _t20_from_tail(tail: np.ndarray, sr: int) -> float:
    """RT60 (s) extrapolated from the -5..-25 dB slope of a Schroeder curve.

    Returns 0 when the tail never spans the fit window — a short or noisy
    segment gives a slope fitted to noise, and a wrong number here is worse
    than an absent one because it would silently define the model's room.
    """
    if len(tail) < sr // 50:
        return 0.0
    energy = tail.astype(np.float64) ** 2
    # Schroeder backward integration: E(t) = integral from t to end.
    sch = np.cumsum(energy[::-1])[::-1]
    if sch[0] <= 0.0:
        return 0.0
    curve = 10.0 * np.log10(np.maximum(sch / sch[0], 1e-12))
    hi_idx = np.argmax(curve <= FIT_DB_HI)
    lo_idx = np.argmax(curve <= FIT_DB_LO)
    # argmax on an all-False array returns 0, i.e. the level was never reached.
    if hi_idx == 0 or lo_idx <= hi_idx:
        return 0.0
    t = np.arange(hi_idx, lo_idx) / sr
    y = curve[hi_idx:lo_idx]
    slope = np.polyfit(t, y, 1)[0]  # dB per second, negative
    if slope >= -1e-6:
        return 0.0
    return float(-60.0 / slope)


def estimate_room(audio: np.ndarray, sr: int, notes: list[tuple[float, float]]) -> Room:
    """Measure the space from the tails between the notes of `audio`.

    `notes` are (start, end) second pairs. The tail runs from one note's end to
    the *next note's start* — not to the next note-off, which would put a whole
    note inside the window being measured for decay and read the instrument as
    a huge room.

    What is measured is the room's decay plus whatever the instrument keeps
    ringing. That confound is real and unavoidable from music alone: a
    long-sustaining instrument reads as a slightly longer room. It is still far
    better than assuming dry — the error is a fraction of a second where the
    confound being corrected is seconds — and `_t20_from_tail` plus the
    high-frequency plausibility gate below reject the cases where the ring
    dominates outright.
    """
    mono = audio.mean(axis=1) if audio.ndim > 1 else audio
    mono = np.asarray(mono, dtype=np.float64)
    if not notes:
        return DRY

    spans = sorted(notes)
    tails: list[np.ndarray] = []
    note_energy = 0.0
    for i, (on, off) in enumerate(spans):
        note_energy += float(np.sum(mono[int(on * sr) : int(off * sr)] ** 2))
        start = int(off * sr)
        limit = spans[i + 1][0] if i + 1 < len(spans) else off + 3.0
        end = min(int(min(limit, off + 3.0) * sr), len(mono))
        if end - start > sr // 10:
            tails.append(mono[start:end])
    if not tails:
        return DRY

    band_rt: dict[float, list[float]] = {b: [] for b in BANDS_HZ}
    for tail in tails:
        for b in BANDS_HZ:
            if b >= 0.45 * sr:
                continue
            rt = _t20_from_tail(_octave_band(tail, sr, b), sr)
            if rt > 0.0:
                band_rt[b].append(rt)

    def median_rt(b: float) -> float:
        vals = band_rt.get(b, [])
        return float(np.median(vals)) if vals else 0.0

    mid = [median_rt(b) for b in (500.0, 1000.0) if median_rt(b) > 0.0]
    rt60 = float(np.mean(mid)) if mid else 0.0
    if rt60 <= 0.0:
        # No band gave a usable slope: report dry rather than invent a room.
        return DRY
    hf = median_rt(4000.0)
    ref = median_rt(500.0) or rt60
    hf_ratio = float(np.clip(hf / ref, 0.15, 1.5)) if hf > 0.0 else 0.6

    # Physical plausibility gate. Air absorption and every real absorber damp
    # high frequencies faster than low, so a room's 4 kHz RT60 is well under its
    # 500 Hz one — 0.4 to 0.8 in occupied spaces. A measurement near or above
    # unity is not measuring a room; it is measuring the instrument's own ring,
    # which on a bowed string or a pipe is brightest where it lasts longest. A
    # dry reference organ render measures 0.98 here, which is why the bound sits
    # at 0.9 rather than at 1.
    if hf_ratio >= 0.9:
        return DRY

    tail_energy = sum(float(np.sum(t**2)) for t in tails)
    tail_db = 10.0 * np.log10(max(note_energy, 1e-20) / max(tail_energy, 1e-20))

    return Room(
        rt60_s=rt60, hf_ratio=hf_ratio, tail_db=float(tail_db), predelay_ms=15.0,
        tail_window_s=min(len(t) for t in tails) / sr,
    )


def synth_room_ir(room: Room, sr: int, seed: int = 0x5011D) -> np.ndarray:
    """A stereo impulse response with `room`'s decay, ratio and predelay.

    Exponentially-decaying band-filtered noise — the statistical late field,
    with no attempt at early reflections. Early reflections carry the room's
    *identity* (which wall, how far) and none of its *decay*; only the decay
    confounds the timbre metrics, so modelling them would add free parameters
    that the loss cannot constrain. Seeded, never platform RNG, so a fit is
    reproducible.
    """
    if room.is_dry():
        return np.zeros((1, 2), dtype=np.float32)
    length = int(min(max(room.rt60_s * 1.2, 0.15), 6.0) * sr)
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal((length, 2))

    t = np.arange(length) / sr
    out = np.zeros((length, 2), dtype=np.float64)
    # Per-band decay: RT60 slides log-linearly from the 500 Hz reference to the
    # 4 kHz measurement, so hf_ratio sets the tilt across the whole spectrum.
    for b in BANDS_HZ:
        if b >= 0.45 * sr:
            continue
        octaves = np.log2(b / 500.0)
        rt = room.rt60_s * float(room.hf_ratio) ** (octaves / 3.0)  # 500 Hz -> 4 kHz is 3 octaves
        rt = float(np.clip(rt, 0.03, 8.0))
        env = np.exp(-6.907755279 * t / rt)[:, None]
        for ch in range(2):
            out[:, ch] += _octave_band(noise[:, ch] * env[:, 0], sr, b)

    # Scale the tail so that convolving it in at unit gain lands the note /
    # tail balance on the measured `tail_db`. Approximate by construction — the
    # exact ratio depends on how long the notes are relative to the decay — but
    # the estimate and this synthesis use the same measure, so a room measured
    # here and rebuilt here round-trips.
    tail_energy = float(np.sum(out**2))
    if tail_energy > 0.0:
        out *= np.sqrt(10.0 ** (-room.tail_db / 10.0) / tail_energy)

    pre = int(room.predelay_ms * 0.001 * sr)
    ir = np.zeros((pre + length + 1, 2), dtype=np.float64)
    ir[0, :] = 1.0  # the direct sound
    ir[pre + 1 :, :] += out
    return ir.astype(np.float32)


def apply_room(audio: np.ndarray, ir: np.ndarray) -> np.ndarray:
    """Convolve `audio` (frames, ch) with `ir` (frames, ch), keeping the length.

    Truncated to the input length rather than extended: every downstream metric
    windows by note position, so a longer array would shift nothing but would
    make the model and oracle disagree on where the render ends.
    """
    if ir.shape[0] <= 1:
        return audio
    frames = audio.shape[0]
    n = int(2 ** np.ceil(np.log2(frames + ir.shape[0])))
    out = np.empty_like(audio)
    for ch in range(audio.shape[1]):
        a = np.fft.rfft(audio[:, ch].astype(np.float64), n)
        h = np.fft.rfft(ir[:, min(ch, ir.shape[1] - 1)].astype(np.float64), n)
        out[:, ch] = np.fft.irfft(a * h, n)[:frames]
    return out


def fit_room_ir(
    dry: np.ndarray, sr: int, notes: list[tuple[float, float]], target: Room, iters: int = 5
) -> np.ndarray:
    """Build the impulse response that puts `dry` in `target`'s space.

    `synth_room_ir` is parameterised by the room's own properties, but what has
    to match is the *measurement*: after convolution the model must measure the
    same RT60 and tail level as the oracle did, through the same estimator. The
    two differ by a factor that depends on how long the notes are relative to
    the decay — most of a long note's reverberant energy falls inside the note
    window, not the tail — so the IR is fitted rather than computed.

    A handful of iterations; each is one convolution plus one measurement of an
    already-rendered buffer, and it runs once for the whole fit.
    """
    if target.is_dry():
        return synth_room_ir(DRY, sr)

    best_ir = synth_room_ir(target, sr)
    best_err = float("inf")

    def measure(rt: float, wet: float) -> tuple[np.ndarray, Room, float]:
        nonlocal best_ir, best_err
        ir = synth_room_ir(Room(rt, target.hf_ratio, wet, target.predelay_ms), sr)
        got = estimate_room(apply_room(dry, ir), sr, notes)
        err = room_distance(got, target) if not got.is_dry() else float("inf")
        if err < best_err:
            best_err, best_ir = err, ir
        return ir, got, err

    # Stage 1: reverberation time, which is close to independent of wetness and
    # converges in two or three multiplicative steps.
    rt = target.rt60_s
    wet = target.tail_db
    for _ in range(iters):
        _, got, err = measure(rt, wet)
        if err < 0.15:
            return best_ir
        if got.is_dry():
            wet -= 8.0
            continue
        rt = float(np.clip(rt * target.rt60_s / max(got.rt60_s, 1e-3), 0.05, 8.0))

    # Stage 2: wetness, by bisection rather than a step, because the measured
    # tail level saturates. Past a certain point the reverberant field fills the
    # note window as much as the tail, so the ratio stops responding — for
    # one-second notes in a 1.6 s room it bottoms out around +19 dB however wet
    # the space gets. A target below that floor is not reachable, and bisection
    # settles on the wettest room that is, which is the right way to miss.
    lo, hi = -30.0, 40.0
    for _ in range(8):
        mid = 0.5 * (lo + hi)
        _, got, err = measure(rt, mid)
        if err < 0.15:
            break
        if got.is_dry() or got.tail_db > target.tail_db:
            hi = mid  # too dry: more reverb
        else:
            lo = mid
    return best_ir


GS_DEFAULT_REVERB_DECAY = 0.7  # GsEffectsConfig::reverb_decay
GS_POWER_ON_CC91 = 40  # what gs_reset()/gm_reset() leave CC91 at

# Search grid for `match_sends`. The tank decay is swept as a multiplier on the
# shipped GsEffectsConfig value rather than in seconds because that is the knob
# libsonare actually exposes (`gs_effects.kReverbDecayScale`), and the mapping
# to RT60 is neither linear nor instrument-independent: measured on program 19,
# the same sweep gives 1.12 s at tank decay 0.28 and 6.45 s at 0.98, while the
# measured RT60 also drifts with the send because the instrument's own release
# sits inside the window. That coupling is why this is a search over real
# renders rather than a closed-form inversion.
_DECAY_SCALES = (0.4, 0.55, 0.7, 0.85, 1.0, 1.15, 1.3, 1.4)
_CC91_STEPS = (0, 10, 20, 30, 40, 55, 70, 90, 110, 127)


def room_distance(a: Room, b: Room) -> float:
    """How far apart two spaces are, in units where 1.0 is clearly audible.

    A 20 % RT60 error and a 3 dB tail-level error are weighted as comparable:
    both are around the threshold at which a listener calls it a different
    room.
    """
    rt_err = abs(np.log(max(a.rt60_s, 1e-3) / max(b.rt60_s, 1e-3))) / 0.2
    tail_err = abs(a.tail_db - b.tail_db) / 3.0
    return float(np.hypot(rt_err, tail_err))


def match_sends(target: Room, measure, log=None) -> dict:
    """Find the libsonare ambience controls that land closest to `target`.

    A fit is only useful if its result can be written back, and libsonare does
    not take an RT60 — it takes a per-channel CC91 send, a per-program send
    weighting (`gm_fallback_sends`) and a tank decay (`GsEffectsConfig`, via
    `gs_effects.kReverbDecayScale`). This searches those controls directly.

    `measure(cc91, decay_scale) -> Room` renders the probe through libsonare at
    those settings and measures the result; it is called once per grid point,
    so it must be a subprocess render — the override table is read once per
    process and a sweep inside one interpreter would measure the first value
    every time.

    Returns the best controls, the residual distance, and the `reverb_scale`
    that would put this program at that send when the channel sits at the GS
    power-on CC91 — which is the number to write into `gm_fallback_sends`.
    """
    if target.is_dry():
        return {
            "cc91": 0, "decay_scale": 1.0, "reverb_scale": 0.0,
            "residual": 0.0, "measured": DRY.to_dict(), "dry": True,
        }

    # Staged rather than a full 8x10 grid: the measured response separates
    # cleanly, with RT60 driven by the tank decay and DRR by the send. Sweep
    # each against the quantity it owns, then sweep the decay once more at the
    # chosen send to pick up the residual coupling (a longer tail also holds
    # more energy, so the send shifts RT60 a little and the decay shifts DRR).
    best: tuple[float, int, float, Room] | None = None

    def consider(cc91: int, decay_scale: float) -> float:
        nonlocal best
        got = measure(cc91, decay_scale)
        d = room_distance(got, target)
        if log is not None:
            log(f"  cc91={cc91:3d} decay_scale={decay_scale:.2f} -> "
                f"rt60={got.rt60_s:.2f}s tail={got.tail_db:+.1f}dB  dist={d:.2f}")
        if best is None or d < best[0]:
            best = (d, cc91, decay_scale, got)
        return d

    for decay_scale in _DECAY_SCALES:
        consider(GS_POWER_ON_CC91, decay_scale)
    assert best is not None
    coarse_decay = best[2]
    for cc91 in _CC91_STEPS:
        consider(cc91, coarse_decay)
    chosen_cc91 = best[1]
    for decay_scale in _DECAY_SCALES:
        if decay_scale != coarse_decay:
            consider(chosen_cc91, decay_scale)

    residual, cc91, decay_scale, got = best
    return {
        "cc91": cc91,
        "decay_scale": round(decay_scale, 3),
        "reverb_decay": round(GS_DEFAULT_REVERB_DECAY * decay_scale, 3),
        "reverb_scale": round(cc91 / GS_POWER_ON_CC91, 2),
        "residual": round(residual, 3),
        "measured": got.to_dict(),
        "dry": False,
    }
