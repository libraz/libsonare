"""Mono reduction, level and spectrum primitives, and audibility weighting."""

from __future__ import annotations

import numpy as np

N_HARMONICS = 12


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


def channel_width(audio: np.ndarray | None, lo: int = 0, hi: int | None = None) -> float | None:
    """How wide a render's image is over `[lo, hi)`: 0 is mono, 1 is decorrelated.

    `channel_correlation` read as a width, which is the direction a comparison
    wants — a model that radiates wider than its reference reads positive. The
    window is the caller's because the answer depends on it: a correlation taken
    across a recording's tail padding reports the silence as a source in the
    middle, and every capture here is mostly tail.
    """
    if audio is None or audio.ndim != 2:
        return None
    corr = channel_correlation(audio[lo:hi])
    return None if corr is None else round(1.0 - abs(corr), 3)


def normalize_rms(audio: np.ndarray, target_rms: float = 0.05) -> np.ndarray:
    """Scale the whole render to a common RMS so level metrics compare balance."""
    rms = float(np.sqrt(np.mean(audio**2)))
    if rms < 1e-8:
        return audio
    return audio * (target_rms / rms)


#: Where `_db` clamps. Named because a reader has to be able to ask whether a
#: value came from the signal or from this: everything at or under it converts
#: to the same -240 dB, so any shape fitted across it is the clamp's and not the
#: voice's.
DB_FLOOR = 1e-12


def _db(x: np.ndarray | float, floor: float = DB_FLOOR) -> np.ndarray | float:
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


# Where a render is taken to start sounding, and on what grid that is decided.
#
# A hosted plugin does not always sound a note in the buffer it was delivered
# in — measured on a sampled kit, the same key at six velocities started
# anywhere between 0 and 750 ms after the note-on — and a capture's own guard
# only refuses a render that is LATE, so the offset that survives is bounded on
# one side and systematic. A window anchored on the note-on rather than on the
# sound reports that offset as the instrument's attack time.
#
# The floor is a fraction of the search window's own peak rather than an
# absolute level, so a preroll carrying the library's room tone does not read
# as the note having already started. Half a millisecond because the offset
# being located is single-digit milliseconds, which a 5 ms hop quantises away.
ONSET_FLOOR_DB = -50.0
ONSET_SEARCH_S = 1.0
ONSET_HOP_MS = 0.5
ONSET_WIN_MS = 2.0
#: How long the envelope has to stay over the floor before the crossing counts
#: as the sound starting. Ten frames of the grid above, so no single-frame
#: ripple can pass for a beginning. It costs nothing in accuracy: what is
#: returned is the frame the run STARTS at, so a longer hold only raises the
#: shortest sound that can satisfy it, and @ref sound_onset_s falls back for
#: those rather than refusing them.
ONSET_HOLD_MS = 5.0
#: How long a stretch under the floor has to be before it is a gap in the sound
#: rather than a trough in its waveform. One period of 40 Hz, which is the
#: lowest fundamental this corpus captures — a tuba's E1 at 41 Hz — and the
#: point below which a 2 ms RMS window stops smoothing a waveform into a level
#: at all. Measured on `lead_square`'s bottom octave, a 65 Hz pulse read
#: through that window swings between -9 and -64 dB of its own peak every
#: 15 ms, so its first 5 ms unbroken stretch over the floor is 38 ms after it
#: began sounding.
ONSET_GAP_MS = 25.0


def _first_sustained(over: np.ndarray, need: int) -> int | None:
    """First index of the earliest run of `need` consecutive true values."""
    if need <= 1:
        return int(np.argmax(over)) if bool(over.any()) else None
    if over.size < need:
        return None
    runs = np.convolve(over.astype(np.int32), np.ones(need, dtype=np.int32), mode="valid")
    hit = np.nonzero(runs == need)[0]
    return int(hit[0]) if hit.size else None


def _close_gaps(over: np.ndarray, span: int) -> np.ndarray:
    """Fill every false stretch shorter than `span`, leaving the longer ones.

    What separates a waveform's own trough from a silence between two sounds,
    and there is nothing but duration to separate them by — both are the
    envelope under the floor. See @ref ONSET_GAP_MS for where the line sits.
    """
    idx = np.flatnonzero(over)
    if span <= 1 or idx.size < 2:
        return over
    out = over.copy()
    lo, hi = idx[:-1], idx[1:]
    for g in np.flatnonzero((hi - lo > 1) & (hi - lo - 1 < span)):
        out[lo[g] + 1 : hi[g]] = True
    return out


def sound_onset_s(
    mono: np.ndarray,
    sr: int,
    start: float,
    limit: float,
    *,
    floor_db: float = ONSET_FLOOR_DB,
    search_s: float = ONSET_SEARCH_S,
    hop_ms: float = ONSET_HOP_MS,
    win_ms: float = ONSET_WIN_MS,
    hold_ms: float = ONSET_HOLD_MS,
    gap_ms: float = ONSET_GAP_MS,
) -> float:
    """Where the sound begins, in seconds, at or after `start`.

    The earliest moment the envelope goes over `floor_db` of the search
    window's peak and stays over it for `hold_ms`, counting a stretch under the
    floor shorter than `gap_ms` as part of the sound rather than as a break in
    it; the frame before that run is the answer. A voice that genuinely swells
    (a crash, a vibraslap's rattle, a bowed entry) keeps its real onset rather
    than being cut to its peak, because a swell is over the floor from its own
    beginning. Only `search_s` past `start` is looked at: further on, the
    loudest thing in the window is more likely to be the next event than this
    one.

    Asked forwards, because the sub-floor frames of a render are not all
    lead-in. Read backwards from the peak — the last frame under the floor —
    any single frame the envelope dips through resets the answer to itself:
    measured on the cached corpus, three frames of ripple 730 ms into a
    `lead_square` that had been sounding since its preroll put the window
    630 ms inside the note, and the 2 ms of silence between two of
    `bird_tweet`'s chirps put it 755 ms in, at the head of the loudest chirp
    rather than the first one. Across the pitched corpus that rule misplaced
    49 rows.

    `start` is where the caller knows the sound cannot have begun before, and
    every caller passes the note-on. It is not a detail: a preroll exists to
    absorb whatever the plugin does in its first buffer, so what sits in one is
    by construction not the note — `electric_grand` puts a click in the first
    70 ms of its file and `english_horn` a previous note's tail decaying from
    -43 dB, and a scan that started at the file would answer with either.

    This is the one onset every path uses — the percussion window, the live
    probe's per-note anchor and the profile's captured-note window — so that a
    model and the reference it is scored against are read from the same kind of
    instant. Falls back to the first frame over the floor for a sound too short
    to hold, and to `start` when nothing rises above it at all, which is what a
    silent render gives and what every caller already handles.

    A lead under the `win_ms` the envelope is read through cannot be resolved
    and comes back as zero; past that it reads `win_ms / 2` early, because
    `_rms_envelope` times a frame at its centre and the frame this returns is
    the last one lying wholly before the sound. Measured against synthetic
    leads of 0, 2, 5 and 10 ms the error is 0, 0, -1.00 and -1.00 ms — a
    constant once resolvable, so it cancels in every difference two sides of
    one comparison take, and only an absolute reading of the delay —
    `onset_ms` — carries it.
    """
    scan_end = int(min(limit, start + search_s) * sr)
    scan = np.asarray(mono[int(start * sr) : min(scan_end, len(mono))], dtype=np.float64)
    if len(scan) < 2:
        return start
    times, env = _rms_envelope(scan, sr, hop_ms=hop_ms, win_ms=win_ms)
    floor = float(env[int(np.argmax(env))]) * 10.0 ** (floor_db / 20.0)
    if floor <= 0.0:
        return start
    over = _close_gaps(env > floor, max(1, round(gap_ms / hop_ms)))
    i = _first_sustained(over, max(1, round(hold_ms / hop_ms)))
    if i is None:
        i = _first_sustained(over, 1)
    return start + float(times[i - 1]) if i else start


def _spectrum(seg: np.ndarray, sr: int):
    """Hann-windowed magnitude spectrum; returns (freqs, magnitude)."""
    win = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg * win))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return freqs, mag


def _peak_near(
    freqs: np.ndarray, mag: np.ndarray, center_hz: float, tolerance_cents: float
) -> tuple[float, float]:
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
    den = (f2 + 20.6**2) * np.sqrt((f2 + 107.7**2) * (f2 + 737.9**2)) * (f2 + 12194.0**2)
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


def audibility_weights(
    freqs_hz, levels_db, *, a_weight: bool = True, mask: bool = True
) -> np.ndarray:
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
