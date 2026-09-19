"""What one captured note or hit is reduced to, measurement by measurement."""

from __future__ import annotations

import numpy as np
from capture import PERCUSSION_CHANNEL
from metrics import (
    INHARMONICITY_TOLERANCES,
    MAX_FIT_PARTIALS,
    MIN_PARTIALS_FOR_B,
    SILENT_WINDOW_DB,
    _db,
    _peak_near,
    _rms_envelope,
    _spectrum,
    _under_peak_db,
    analyze_hit,
    channel_width,
    fit_partial_series,
    midi_to_hz,
    partial_hz,
    sound_onset_s,
    to_mono,
)
from smf import Note

# The partial count, the floor and the reliability gate all live with the fit in
# `metrics`, so the reference profile and the model-vs-oracle comparison read a
# string with one ruler. They were the same numbers when they were written twice;
# a quantity defined in two places only stays equal until one of them is tuned.
MAX_PARTIALS = MAX_FIT_PARTIALS


# --------------------------------------------------------------------------
# which kind of instrument a capture is


def is_percussion(cap: dict) -> bool:
    """Whether this capture's note numbers select instruments rather than pitches.

    Read off the MIDI channel rather than from a flag of its own. The channel is
    where the distinction already lives — it is what makes a note number select
    an instrument, on the reference as much as in libsonare — and a second field
    saying the same thing is a field that can disagree with it.

    What CAN disagree with it is an address. A rack selects its slots by channel
    too, and a slot number written into this field claims a meaning it was never
    about: five melodic instruments that happened to sit in slot 10 were measured
    as drum maps, and the profiles came back with a band tilt and a crest for
    every note and a fundamental for none. So the slot has its own name,
    `capture.slot_channel`, and this field stays the semantic one.

    All timbres or none: a capture holding a kit on channel 10 and a melodic
    slot on channel 1 has no single answer, and guessing one would measure half
    of it with the wrong metric set.

    Tolerant of a channel outside `capture.NOTE_CHANNELS`, which `load_config`
    refuses — a reference profile carries the timbre block it was measured with,
    so this also reads capture definitions written before the slot had a name.
    """
    timbres = cap.get("timbres") or []
    if not timbres:
        return False
    on_ten = [int(t.get("channel", 1)) == PERCUSSION_CHANNEL for t in timbres]
    if any(on_ten) and not all(on_ten):
        raise ValueError(
            "capture mixes percussion and melodic timbres: "
            f"{[t.get('id') for t, p in zip(timbres, on_ten) if p]} sit on MIDI "
            f"channel {PERCUSSION_CHANNEL} and the rest do not. Split them into "
            f"two capture definitions — one profile cannot be measured both ways"
        )
    return all(on_ten)


# --------------------------------------------------------------------------
# per-note measurement


def find_partials(seg: np.ndarray, sr: int, note: int) -> dict:
    """Locate the partials of one struck note and fit the stiff-string law.

    Returns the measured f0, the inharmonicity coefficient B, and the partial
    frequencies and levels that were actually found.

    The fit itself is `metrics.fit_partial_series`, which is also what the
    model-vs-oracle comparison reads a ladder with. That shared ruler is the
    point: a reference profile measured one way and a model scored another was
    the state this replaced, and the two disagreed by up to 1.8x on the same
    three-note piano probe. What stays here is the reporting — which partials
    were found at the fitted series, how strong each was, and whether there
    were enough of them for B to mean anything.
    """
    freqs, mag = _spectrum(seg, sr)
    et = midi_to_hz(note)
    f0_seed, a0 = _peak_near(freqs, mag, et, 120.0)
    if a0 <= 0.0:
        return {}
    f0, b, fitted_on = fit_partial_series(freqs, mag, f0_seed, a0, sr)
    found: list[tuple[int, float, float]] = []
    for n in range(1, MAX_PARTIALS + 1):
        predicted = partial_hz(f0, n, b)
        if predicted > 0.45 * sr:
            break
        fn, an = _peak_near(freqs, mag, predicted, INHARMONICITY_TOLERANCES[-1])
        if an > 0.0:
            found.append((n, fn, an))

    if not found:
        return {}
    # The ladder is reported against its own fundamental, which is the anchor
    # `harmonics_db` uses on the live path and the one every consumer of this
    # field re-references to before reading it (`partial_balance_db`, `band_db`
    # both subtract `partials_db[0]`). Anchoring on the loudest partial instead
    # left the two halves of the harness holding the same ladder at two
    # different zeroes, and the loudest partial is not a stable choice of zero:
    # measured over the committed references it sits above h1 on 30 % of rows
    # and within 1 dB of the runner-up on 9 %, so which partial it is can turn
    # over between two takes of one note. Falls back to the loudest where the
    # series has no fundamental at all, there being nothing else to be relative
    # to.
    anchor = next((a for n, _, a in found if n == 1), 0.0) or max(a for _, _, a in found)
    return {
        "f0_hz": round(f0, 3),
        "cents_vs_et": round(float(1200.0 * np.log2(f0 / et)), 2),
        "inharmonicity_b": float(f"{b:.3e}"),
        # How many partials the stiffness fit actually had. At the top of the
        # keyboard there is barely a series left to fit — C8 puts only five
        # partials under the Nyquist frequency — and a B from four of them is a
        # number rather than a measurement. Reported so that whoever reads the
        # profile can tell which is which, and used by `summarize` to keep the
        # unreliable ones out of the curve a voice would be fitted to.
        "partials_fit": fitted_on,
        "inharmonicity_reliable": fitted_on >= MIN_PARTIALS_FOR_B,
        "partials_hz": [round(fn, 2) for _, fn, _ in found],
        "partials_db": [round(float(20 * np.log10(max(an, 1e-12) / anchor)), 2)
                        for _, _, an in found],
    }


#: How far under its own peak an envelope is still the instrument. Past this a
#: held note is the recording's floor, the beating between two nearly-dead
#: strings, or -- on a trimmed sample -- digital silence, and a line fitted
#: through any of the three describes the file rather than the string. Measured
#: on this corpus, C7's reference envelope reaches -240 dBFS inside the gate,
#: and the aftersound rate fitted past that point was the slope of the silence:
#: -1.1 dB/s, which read as a reference that rings and a model that does not.
DECAY_RANGE_DB = 60.0

#: How close two decay spans have to be before the rates fitted over them are
#: worth differencing. Loose, because the two sides reach the range bound at
#: their own moments and a decay is not a straight line: what this excludes is
#: the pair where one side sustained for the whole gate and the other was over
#: in a fraction of it, which is a comparison of a late rate against an early one.
DECAY_SPAN_AGREEMENT = 0.6

#: How far under the envelope's maximum still counts as the note having arrived.
#: A struck note's maximum IS its onset, but a held one has no maximum worth the
#: name: measured on this corpus a clarinet's envelope crests 0.20 dB over its
#: own mean, so its loudest 10 ms window falls wherever the vibrato happened to
#: peak -- 1270 ms in, against 25 ms for the first arrival within this bound. The
#: value is the one `metrics` already quotes an attack against on the percussion
#: path, and it moves a decaying note's onset by under 15 ms.
ONSET_SLACK_DB = 3.0

#: How long after arriving a note may still be climbing to the peak its decay is
#: fitted from. Measured on this corpus the longest any voice with a real peak
#: takes is 185 ms -- a vibraphone bar blooming -- while the earliest a plateau
#: puts its highest sample is 375 ms, so the two populations do not overlap.
RISE_WINDOW_S = 0.25


def arrival_index(env_db: np.ndarray) -> int:
    """First envelope point within @ref ONSET_SLACK_DB of the loudest one.

    Where the note has ARRIVED at its level, which is the far end of the rise
    and not its beginning — the beginning is `sound_onset_s`, the one detector
    every window in this harness is placed by, and this was called `onset_index`
    for long enough to read as a second one.

    The attack time is read off this rather than off `argmax`, which is only an
    onset on an instrument that decays. On a plateau `argmax` is the largest
    sample of a flat line: a clarinet's envelope crests 0.13 dB over its own
    median, so its loudest window lands wherever the vibrato happened to.
    """
    if env_db.size == 0:
        return 0
    return int(np.argmax(env_db >= float(np.max(env_db)) - ONSET_SLACK_DB))


def decay_origin_index(env_db: np.ndarray, hop_s: float) -> int:
    """Where the fall starts: the loudest point inside the note's own rise.

    A decay is fitted from the peak, and on anything that decays `argmax` finds
    it. What `argmax` cannot do is refuse -- a held note has no peak, so it
    returns the wandering level's best moment and the rate is fitted from where
    noise put it, over whatever fraction of the note followed. Bounding the
    search to @ref RISE_WINDOW_S past the arrival keeps the real peak on every
    voice that has one and stops a plateau's late crest from claiming to be one.

    Not the arrival itself: that sits on the rising edge, and a fast envelope
    fitted from there is fitted partly over its own attack. Measured on a synth
    drum whose peak is 30 ms past its arrival, the early rate came back at
    +60.8 dB/s -- a decay reported as a climb.
    """
    if env_db.size == 0:
        return 0
    start = arrival_index(env_db)
    span = max(1, round(RISE_WINDOW_S / max(hop_s, 1e-9)))
    return start + int(np.argmax(env_db[start:min(env_db.size, start + span)]))


#: Where the body reading is taken, in seconds from the onset. After the strike,
#: so the hammer's own broadband noise is not counted as the instrument's body,
#: and a full second wide, because what is being measured is a resonance that
#: outlives the string rather than a transient.
BODY_WINDOW_S = (0.2, 1.2)
#: The lowest fundamental this reading means anything for. Below it the band
#: under the note is narrower than an octave and is measuring the note's own
#: skirt rather than anything under it.
BODY_MIN_F0_HZ = 120.0


def body_below_f0_db(x: np.ndarray, sr: int, f0: float) -> float | None:
    """Energy under the note's own fundamental, relative to the fundamental.

    A grand's radiated sound is not only its string. The blow drives the bridge,
    the bridge drives the board, and the board's low modes ring for seconds under
    a note that has none of that frequency in it. How much of the sound that is
    depends steeply on register, because the string's radiated fundamental falls
    away toward the treble while the body's answer to a blow does not: measured
    on three concert grands, this reading rises about forty decibels from C3 to
    C8, and at C8 the energy under the note outweighs the note.

    Taken as a ratio rather than as a level, so it says nothing about how loud
    the render was and cannot be answered by a gain. A model that is a string and
    only a string sits flat across the whole keyboard here, which no amount of
    body LEVEL corrects -- the register dependence is the measurement.
    """
    if f0 < BODY_MIN_F0_HZ:
        return None
    a, b = int(BODY_WINDOW_S[0] * sr), int(BODY_WINDOW_S[1] * sr)
    seg = x[a:b]
    if seg.size < sr // 8:
        return None
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)

    def band(lo: float, hi: float) -> float:
        sel = (freqs >= lo) & (freqs < hi)
        return float(np.sqrt(np.sum(spec[sel] ** 2))) if sel.any() else 0.0

    under, fundamental = band(40.0, f0 * 0.7), band(f0 * 0.8, f0 * 1.25)
    if fundamental <= 0.0 or under <= 0.0:
        return None
    return round(float(20.0 * np.log10(under / fundamental)), 2)


def double_decay_gap(row: dict) -> float | None:
    """How much faster the prompt stage falls than the aftersound, in dB/s.

    A struck string loses energy fast while the strings of its unison and the
    two polarizations of each move together, and far more slowly once they have
    decohered. The size of that mechanism is the GAP between the two rates, and
    neither rate on its own carries it: a voice can sit inside both the held-note
    bound and the aftersound bound while having no double decay whatsoever, by
    putting the same rate in both. Reported as its own dimension for that reason
    -- a mechanism with no dimension can never be the largest error.
    """
    if "decay_early_db_s" not in row or "decay_late_db_s" not in row:
        return None
    return row["decay_early_db_s"] - row["decay_late_db_s"]


#: How many notes a velocity has to contribute before its register profile is
#: worth taking a median of. Below this the median is one or two notes, so the
#: normalization it performs is a comparison of those notes with themselves.
REGISTER_MIN_NOTES = 4


def register_deltas(model: dict[int, dict[int, float]],
                    ref: dict[int, dict[int, float]]) -> list[float]:
    """How the two keyboards' own loudness curves differ, note by note, in dB.

    Every other dimension here is a ratio, a rate, a time or a range, so not one
    of them can see whether a note is the right LOUDNESS for its place on the
    keyboard -- and register balance is most of what voicing a piano is. It was
    invisible for exactly as long as it had no dimension: the model's top octave
    sat twelve decibels under its own mid-keyboard relative to three concert
    grands that agree with each other to within three, and every gated dimension
    read green through it.

    Absolute level cannot be compared -- three instruments recorded at three
    gains say nothing to each other -- so each side is normalized by its OWN
    median across the notes played at that velocity, which removes a constant
    offset and privileges no note. Per velocity, because a grand's register
    profile genuinely changes with how hard it is struck: the top octave falls
    away further at pianissimo than at forte, and pooling the velocities would
    average that out and call it noise.

    @param model Level in dB per note per velocity, one side.
    @param ref   The same for the other side.
    @return One (note, velocity, delta) per pair shared by both sides.
    """
    out: list[tuple[int, int, float]] = []
    velocities = {v for per_vel in model.values() for v in per_vel}
    velocities &= {v for per_vel in ref.values() for v in per_vel}
    for vel in sorted(velocities):
        notes = sorted(n for n in set(model) & set(ref)
                       if vel in model[n] and vel in ref[n])
        if len(notes) < REGISTER_MIN_NOTES:
            continue
        m_mid = float(np.median([model[n][vel] for n in notes]))
        r_mid = float(np.median([ref[n][vel] for n in notes]))
        out.extend((n, vel, (model[n][vel] - m_mid) - (ref[n][vel] - r_mid))
                   for n in notes)
    return out


def usable_decay_end(env_db: np.ndarray, start_i: int,
                     range_db: float = DECAY_RANGE_DB) -> int:
    """Index one past the last envelope point still within @p range_db of @p start_i.

    The LAST point inside the range, not the first one outside it: a decaying
    unison beats by ten decibels and more, and dips below any line long before
    it has stopped sounding.

    This bound catches a floor that lies BELOW it and cannot catch one that
    lies above. A capture whose floor is the second kind has to say so: the
    piano's `_late_top` records what that costs its top octave, what was tried,
    and why the reading is flagged rather than corrected.
    """
    live = np.where(env_db[start_i:] >= env_db[start_i] - range_db)[0]
    return start_i + int(live[-1]) + 1 if live.size else start_i + 1


def double_decay(env_db: np.ndarray, t: np.ndarray) -> dict:
    """Split the decay at the knee where the fast fall gives way to the aftersound.

    A single slope over the whole held note is the average of two very
    different things and matches neither. The breakpoint is searched rather
    than assumed, because where it falls is itself a property worth reporting:
    it is set by how fast the strings of a unison drift apart, which a model
    reproduces only if it has more than one string.
    """
    if len(t) < 12:
        return {}
    best = None
    lo, hi = max(3, len(t) // 10), min(len(t) - 4, int(len(t) * 0.75))
    for k in range(lo, hi):
        e = 0.0
        for a, b in ((0, k), (k, len(t))):
            if b - a < 3:
                e = np.inf
                break
            p = np.polyfit(t[a:b], env_db[a:b], 1)
            e += float(np.sum((np.polyval(p, t[a:b]) - env_db[a:b]) ** 2))
        if best is None or e < best[0]:
            best = (e, k)
    if best is None:
        return {}
    k = best[1]
    early = float(np.polyfit(t[:k], env_db[:k], 1)[0])
    late = float(np.polyfit(t[k:], env_db[k:], 1)[0])
    return {
        "decay_early_db_s": round(early, 2),
        "decay_late_db_s": round(late, 2),
        "decay_knee_s": round(float(t[k] - t[0]), 3),
    }


def partial_decay(seg: np.ndarray, sr: int, partials_hz: list[float],
                  n_partials: int = 8) -> list[float]:
    """Decay rate in dB/s of each of the first partials, each from its own peak.

    The top of a piano's spectrum dies far faster than the bottom, and that
    difference over time — not the spectrum at any one instant — is why a
    sustained piano note darkens as it rings. A model matched on one spectral
    snapshot can have every partial in the right place and still get this wrong.
    """
    out: list[float] = []
    win = max(1024, int(0.093 * sr))
    hop = win // 2
    n_frames = max(1, (len(seg) - win) // hop + 1)
    if n_frames < 6:
        return out
    frames = np.lib.stride_tricks.sliding_window_view(seg, win)[::hop][:n_frames]
    window = np.hanning(win)
    spec = np.abs(np.fft.rfft(frames * window, axis=1))
    fr = np.fft.rfftfreq(win, 1.0 / sr)
    t = np.arange(n_frames) * hop / sr
    for f in partials_hz[:n_partials]:
        k = int(np.argmin(np.abs(fr - f)))
        band = spec[:, max(0, k - 1):k + 2].max(axis=1)
        db = _db(band / max(band.max(), 1e-12))
        keep = db > -55.0
        if keep.sum() < 5:
            out.append(float("nan"))
            continue
        out.append(round(float(np.polyfit(t[keep], db[keep], 1)[0]), 2))
    return out


def _above_fundamental(seg: np.ndarray, sr: int, f0_hz: float) -> np.ndarray:
    """Drop everything below the note, so a release measures the string.

    A close-miked grand keeps ringing after every note-off at frequencies that
    have nothing to do with what was played: the same 21, 35 and 82 Hz appear in
    the tail of A0, C3 and C4 alike, at 25 to 40 dB below the held level. Those
    are the case and the soundboard, not the string the damper just landed on,
    and they sit above a broadband envelope's threshold for a long time —
    measured that way a C4 damper reads as taking over two seconds, which is the
    box ringing, not the felt.

    Cutting below the fundamental leaves the note and removes the box. It cannot
    do so at the bottom of the keyboard, where those modes are the note's own
    first partials; a bass row is contaminated whatever this does, and the
    velocities where it matters are also the ones the capture tail truncates.
    """
    n = len(seg)
    if n < 16:
        return seg.astype(np.float64)
    fc = max(20.0, 0.6 * f0_hz)
    spec = np.fft.rfft(seg.astype(np.float64))
    spec[np.fft.rfftfreq(n, 1.0 / sr) < fc] = 0.0
    return np.fft.irfft(spec, n)


def tone_to_noise_db(freqs: np.ndarray, mag: np.ndarray, f0_hz: float,
                     n_partials: int = 16) -> float:
    """Energy in the partials against everything else in the audible band.

    What it separates is the string from the mechanism: a plucked or struck
    instrument's noise floor is its own action — a plectrum scraping, a hammer's
    felt, a jack falling back — and a model that has the partial stack right and
    no mechanism noise reads far cleaner than any recording of the real thing.
    That reads as an improvement on every spectral metric and is the single most
    audible thing still missing.

    The partial window is the wider of ±2 % and three FFT bins. A relative-only
    window collapses below one bin at a low fundamental — at 44 Hz it asks for
    ±0.9 Hz out of a 1.7 Hz grid — so a bass note would score as pure noise for
    a reason that is entirely the analysis window's.
    """
    if freqs.size < 2 or f0_hz <= 0.0:
        return float("nan")
    power = mag.astype(np.float64) ** 2
    bin_hz = float(freqs[1] - freqs[0])
    tonal = np.zeros(freqs.shape, dtype=bool)
    for n in range(1, n_partials + 1):
        target = n * f0_hz
        if target > freqs[-1]:
            break
        half = max(0.02 * target, 3.0 * bin_hz)
        tonal |= (freqs >= target - half) & (freqs <= target + half)
    band = (freqs >= 40.0) & (freqs <= 12000.0)
    tone = float(power[tonal & band].sum())
    noise = float(power[~tonal & band].sum())
    return float(10.0 * np.log10((tone + 1e-30) / (noise + 1e-30)))


#: How far under its own peak a note's ring is still worth fitting partials in.
#: The strike is what puts a bar's modes there, so unlike a sustained voice
#: there is no later window to prefer — what there is has to be taken whole.
SHORT_RING_FLOOR_DB = 40.0


def _short_ring_window(held: np.ndarray, sr: int) -> tuple[int, int]:
    """Where a note too short for the fixed window keeps its partials.

    From the envelope peak, which is the strike, to where the ring has fallen
    `SHORT_RING_FLOOR_DB` under it. Both ends are this note's own: a woodblock
    is 35 ms at the bottom of its range and 17 at the top, so no fixed span
    covers it either.
    """
    times, env = _rms_envelope(held, sr)
    if env.size == 0:
        return 0, 0
    peak_i = int(np.argmax(env))
    floor = float(env[peak_i]) * 10.0 ** (-SHORT_RING_FLOOR_DB / 20.0)
    over = np.nonzero(env[peak_i:] > floor)[0]
    end_i = peak_i + (int(over[-1]) if over.size else 0)
    hop = float(times[1] - times[0]) if times.size > 1 else 0.005
    a = int(float(times[peak_i]) * sr)
    b = min(len(held), int((float(times[end_i]) + hop) * sr))
    return (a, b) if b - a >= sr // 200 else (0, 0)


def measure_note(audio: np.ndarray, sr: int, note: int, *,
                 preroll_s: float, gate_s: float) -> dict:
    """Every measurement this profile carries, for one captured note.

    The gate window is placed on the onset the render actually has rather than
    on the one the score asked for. A capture's own guard refuses a render that
    sounds LATE by more than `capture.ONSET_SLACK_MS` and cannot refuse one that
    is early, so what survives it is a one-sided offset of up to that slack —
    the same size as the 5 ms grid the envelope is read on. Measured on the
    committed references, 40 % of the rows carried an attack inside two of those
    quanta, so the window's own placement error was a large fraction of the
    quantity it was being used to measure. The percussion path has anchored on
    the located strike since it was written; this is the same anchor, from the
    same detector.
    """
    mono = to_mono(audio)
    # From the note-on, the same origin `analyze_hit` and `note_onset` give it:
    # the preroll is there to absorb the plugin's first buffer, so a scan that
    # began at the file would answer with whatever that buffer left behind.
    onset_s = sound_onset_s(mono, sr, preroll_s, len(mono) / sr)
    on = int(onset_s * sr)
    off = on + int(gate_s * sr)
    held = mono[on:off]
    if held.size < sr // 4:
        return {}

    # The spectrum for the partial fit comes from a window that starts after the
    # strike: the hammer noise is broadband and would blur every peak the fit
    # depends on, while a window too late has lost the high partials entirely.
    a = int(0.12 * sr)
    b = min(len(held), a + int(1.5 * sr))
    # 120 ms is after the end of a note that is over in 35, and everything read
    # off this window — the partials, the centroid, the tone-to-noise — is then
    # taken on the floor. So it is checked for signal BEFORE it is used rather
    # than after it has failed, because it does not fail: `find_partials`
    # returns an f0 from a -124 dBFS floor, which is then reported as the voice.
    # Where it is empty the same fit is placed against the note's own ring, from
    # its envelope peak to where the fall leaves the floor. See
    # `SILENT_WINDOW_DB` for the separation this rests on.
    if _under_peak_db(held[a:b], held) < SILENT_WINDOW_DB:
        ring_a, ring_b = _short_ring_window(held, sr)
        if ring_b > ring_a:
            a, b = ring_a, ring_b
    row: dict = dict(find_partials(held[a:b], sr, note))
    if not row:
        # No fundamental means this is not a struck note, and every measurement
        # below would still produce a number for it — a decay slope of 0 dB/s
        # and a damper release the length of the window. A profile is worse for
        # holding those than for being short a row.
        return {}

    body = body_below_f0_db(held, sr, float(row.get("f0_hz") or 0.0))
    if body is not None:
        row["body_below_f0_db"] = body

    t_env, env = _rms_envelope(held, sr)
    env_db = np.asarray(_db(env))
    peak_i = int(np.argmax(env))
    row["peak_dbfs"] = round(float(_db(np.abs(mono).max())), 2)
    row["rms_dbfs"] = round(float(_db(np.sqrt(np.mean(held.astype(np.float64) ** 2)))), 2)
    # The loudest the note's body gets: the maximum of the same 10 ms windowed
    # RMS every decay measurement here is read off. It is the one level in this
    # row that is independent of how long the note lasts -- `rms_dbfs` averages
    # over the whole gate, so a voice ringing for eight seconds against a
    # reference that died in two reads as louder without being louder anywhere.
    # It is not hammer-free, and does not need to be: a transient reaches a
    # windowed RMS only in proportion to its share of the window's energy, so a
    # click that puts 14 dB on `peak_dbfs` puts under 3 on this.
    row["held_peak_dbfs"] = round(float(env_db[peak_i]), 2)
    # Three indices, because `argmax` answers only the first of the three
    # questions on an envelope that does not decay: how loud the note got, when
    # it arrived, and where its fall begins.
    arrival_i = arrival_index(env_db)
    origin_i = decay_origin_index(env_db, float(t_env[1] - t_env[0]) if t_env.size > 1 else 0.005)
    # How late the render sounded against the note-on it was sent. Carried
    # rather than discarded: it is a property of the capture chain and of the
    # plugin, and it is the offset the window above was moved by, so a reader
    # can see what the alignment removed. The same field `analyze_hit` reports,
    # with the same sign — positive means the sound arrived after the note-on.
    row["onset_ms"] = round((onset_s - preroll_s) * 1000.0, 2)
    # The rise, now that the delay above is no longer inside it: from the sound's
    # own onset to the first moment it is within `ONSET_SLACK_DB` of its peak.
    # On a piano this is not the strike — the hammer is over in a couple of
    # milliseconds — it is the bloom the soundboard adds after it.
    row["attack_ms"] = round(float(t_env[arrival_i] * 1000.0), 1)

    tail = slice(origin_i, usable_decay_end(env_db, origin_i))
    # How much of the held note the two rates were fitted over. Both are slopes,
    # so they only compare against a reference fitted over a comparable span --
    # a two-stage decay read to 4 s and one read to 8 is two different questions.
    row["decay_span_s"] = round(float(t_env[tail][-1] - t_env[origin_i]), 3) \
        if t_env[tail].size else 0.0
    if t_env[tail].size > 8:
        row["decay_db_s"] = round(float(np.polyfit(t_env[tail], env_db[tail], 1)[0]), 2)
        row.update(double_decay(env_db[tail], t_env[tail]))
    if row.get("partials_hz"):
        row["partial_decay_db_s"] = partial_decay(held[a:], sr, row["partials_hz"])

    # Damper: note-off to 40 dB below the level it was still holding. Skipped
    # when the string had already stopped — a held C7 is 45 dB down long before
    # the key comes up, and "how fast the damper stopped it" then measures the
    # noise floor and reports it as a very fast damper.
    held_peak_db = float(env_db[peak_i])
    off_db = float(_db(env[-1])) if env.size else -240.0
    row["decayed_before_note_off"] = bool(off_db < held_peak_db - 45.0)
    rel = mono[off:]
    if rel.size > sr // 20 and not row["decayed_before_note_off"]:
        t_rel, env_rel = _rms_envelope(_above_fundamental(rel, sr, row["f0_hz"]), sr)
        start_db = float(_db(env_rel[0]))
        under = np.where(_db(env_rel) < start_db - 40.0)[0]
        row["damper_release_ms"] = round(float(t_rel[under[0]] * 1000.0), 1) if under.size \
            else round(float(t_rel[-1] * 1000.0), 1)
        row["damper_capped"] = not bool(under.size)

    freqs, mag = _spectrum(held[a:b], sr)
    p = mag ** 2
    row["centroid_hz"] = round(float((freqs * p).sum() / max(p.sum(), 1e-20)), 1)
    tnr = tone_to_noise_db(freqs, mag, row["f0_hz"])
    if np.isfinite(tnr):
        row["tnr_db"] = round(tnr, 2)
    width = channel_width(audio, on, off)
    if width is not None:
        row["stereo_width"] = width
    return row


def measure_hit(audio: np.ndarray, sr: int, note: int, velocity: int, *,
                preroll_s: float, gate_s: float,
                max_band_hz: float | None = None) -> dict:
    """One percussion hit, as a profile row.

    A struck instrument has no fundamental, so none of the measurements above it
    apply: there is no partial to be inharmonic, no temperament to be stretched
    against, and no ladder for a tone-to-noise ratio to mask around. What is
    left is the shape of the spectrum, how each band of it decays, how hard the
    strike was, how much of the spectrum stands in peaks rather than lying in a
    continuum, and how wide the image is — which is what `analyze_hit` reports.
    The last two are what the pitched set gets from `tnr_db` and `stereo_width`;
    the image is the same measurement, and the tonality is a different one
    answering the same question, because this one needs no target frequencies.

    `peak_dbfs` is carried alongside so the velocity response is measured for a
    drum by exactly the code that measures it for a piano. It is also the only
    axis a drum note has: a different note is a different instrument, not the
    same one played higher.
    """
    mono = to_mono(audio)
    hit = analyze_hit(mono, sr, Note(note, velocity, preroll_s, gate_s), len(mono) / sr,
                      max_band_hz=max_band_hz,
                      stereo=audio if audio.ndim == 2 and audio.shape[1] == 2 else None)
    row = hit.to_dict()
    strike = mono[int(preroll_s * sr):]
    row["peak_dbfs"] = round(float(_db(float(np.max(np.abs(strike))))), 2) if len(strike) else None
    return row
