"""The sustained-note metric set, its absolute level, and per-note deltas."""

from __future__ import annotations

from dataclasses import asdict, dataclass

import numpy as np
from metrics_hit import HIT_ENVELOPE_HOP_MS, HIT_ENVELOPE_WIN_MS, _hit_onset
from metrics_modal import modal_profile
from metrics_modulation import f0_width_cents, modulation_note
from metrics_partials import estimate_inharmonicity_b, ladder_present, partial_hz
from metrics_signal import (
    DB_FLOOR,
    N_HARMONICS,
    _db,
    _peak_near,
    _rms_envelope,
    _spectrum,
    midi_to_hz,
)
from smf import Note

MIN_SUSTAIN_SEC = 0.15
# Furthest the sustain window may sit from the onset, in seconds. These are
# exactly where the 0.3/0.9 fractions land on the two-second probe patterns, so
# every existing probe measures the identical window; the cap only bites on a
# gate longer than that, where the fraction would otherwise walk off the end of
# the note. See `analyze_note`.
SUSTAIN_WINDOW_S = (0.6, 1.8)
#: How far under the note's own peak an analysis window may sit before it is
#: read as having landed past the end of the sound rather than on its tail.
#:
#: Measured across the captured corpora rather than chosen, on both windows it
#: guards: the woodblock's are 215 dB down, which is digital silence, and the
#: next quietest corpus is 55 dB down on the sustain window and 29 on the
#: partial-fit one. Anywhere in that gap separates them. Falling back gives the
#: analysis MORE signal, so the direction is safe where the threshold is not.
#:
#: An emptiness test rather than a length one, because length is not the failure:
#: a woodblock's sustain window is a full 300 ms and starts 115 ms after the
#: sound ended. And a positive test rather than a failed-measurement one,
#: because the measurements do not fail on silence — `find_partials` returns an
#: f0 from a -124 dBFS noise floor, which is then reported as the voice.
SILENT_WINDOW_DB = -80.0


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
    #: None where the sustain window sat on the dB clamp: a note that reached
    #: digital zero fits a flat line through `DB_FLOOR` and comes out at 0.0,
    #: which is also what a note held perfectly gets. Absent rather than 0.0 so
    #: a reader charges for it instead of reading it as the best case.
    sustain_slope_db_s: float | None
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


def _under_peak_db(window: np.ndarray, note: np.ndarray) -> float:
    """The window's RMS against the note's peak, in dB. `-inf` for an empty one."""
    if window.size == 0 or note.size == 0:
        return float("-inf")
    peak = float(np.abs(note).max())
    if peak <= 0.0:
        return 0.0
    rms = float(np.sqrt(np.mean(window.astype(np.float64) ** 2)))
    return 20.0 * np.log10(max(rms, 1e-30) / peak)


def analyze_note(
    mono: np.ndarray, sr: int, note: Note, render_end: float, *, onset: float | None = None
) -> NoteMetrics:
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
    # The cap above is in seconds, so it cannot see a window that is long enough
    # and lands after the sound: a woodblock is over in 25 ms and its window is
    # 300 ms of silence starting at 150. See `SILENT_WINDOW_DB`.
    if _under_peak_db(sustain, mono[on:off]) < SILENT_WINDOW_DB:
        sustain = mono[on:off]

    freqs, mag = _spectrum(sustain, sr)

    # Fundamental: strongest peak within ±80 cents of equal temperament.
    f0, h1_mag = _peak_near(freqs, mag, expected_f0, 80.0)
    cents_err = 1200.0 * np.log2(f0 / expected_f0) if f0 > 0 else 0.0

    # How far this string's partials have been stretched by its own stiffness.
    # Zero for anything that is not a stiff string, which is what makes every
    # such voice read exactly what it read before this was measured at all.
    inharmonicity_b, inharmonicity_partials = estimate_inharmonicity_b(freqs, mag, f0, h1_mag, sr)

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
    slope: float | None = None
    sustain_rms_db = float(_db(np.sqrt(np.mean(sustain**2))))
    if np.count_nonzero(sus_mask) >= 4:
        t_s = times[sus_mask]
        e = env[sus_mask]
        # The same four frames, but of signal: under the clamp every frame
        # converts to -240, so the fit returns 0.0 and a note that died reads
        # as one that never moved.
        if np.count_nonzero(e > DB_FLOOR) >= 4:
            e_db = np.asarray(_db(e), dtype=np.float64)
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
        f_times, f_env = _rms_envelope(
            mono[fine_start:fine_end], sr, hop_ms=HIT_ENVELOPE_HOP_MS, win_ms=HIT_ENVELOPE_WIN_MS
        )
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
        sustain_slope_db_s=None if slope is None else round(slope, 2),
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
        return {"peak_dbfs": round(peak_db, 2), "held_rms_dbfs": None, "held_crest_db": None}
    return {
        "peak_dbfs": round(peak_db, 2),
        "held_rms_dbfs": round(held_db, 2),
        # Named apart from the percussion set's own `crest_db`, which is a
        # different measurement of a different window on a normalised signal —
        # and which these fields are merged alongside on a drum probe.
        "held_crest_db": round(peak_db - held_db, 2),
    }


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
        "sustain_slope_delta_db_s": (
            None
            if model.sustain_slope_db_s is None or oracle.sustain_slope_db_s is None
            else round(model.sustain_slope_db_s - oracle.sustain_slope_db_s, 2)
        ),
        "release_delta_ms": round(model.release_ms - oracle.release_ms, 1),
        "level_delta_db": round(model.sustain_rms_db - oracle.sustain_rms_db, 2),
    }
