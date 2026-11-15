"""What one captured note or hit is reduced to, measurement by measurement."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from capture import PERCUSSION_CHANNEL
from loop_record import (
    VERDICT_LOOPED,
    VERDICT_UNRECORDED,
    body_loop_lines,
    detect_loop,
    exclusions,
    held_ladder,
    read_to_join_s,
)
from metrics import (
    ATTACK_BANDS_HZ,
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
    arrival_index,
    attack_bands,
    body_below_f0_db,
    channel_width,
    fit_partial_series,
    held_pitch,
    measure_tnr_mask,
    midi_to_hz,
    onset_shape,
    partial_hz,
    partial_level,
    pitch_motion,
    sound_onset_s,
    tnr_mask_parts,
    to_mono,
    tone_to_noise_db,
)
from metrics_floor import body_floor_state, partial_clearance, recording_floor, valley_profile
from metrics_lines import (
    BODY_LINE_MISSED_REASON,
    BODY_LINES_REASON,
    IDLE_AFTER_RELEASE_S,
    body_lines_in,
    held_lines,
    line_amplitude_dbfs,
    line_presence,
    lines_off_the_series,
    mask_lines,
    missed_body_lines,
    recurring_body_line_groups,
    stationary_lines,
)
from metrics_modal import modal_profile
from metrics_motion import CoherenceState, partial_coherence, warp_reading
from metrics_note import (
    BODY_MIN_F0_HZ,
    BODY_WINDOW_S,
    band_power_db,
    body_bands,
)
from metrics_partials import series_scatter_cents
from metrics_warp import LineStructure, line_structure
from profile_window import row_window
from smf import Note
from toneclass import LADDER_DIMENSIONS
from wavio import read_wav

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


#: RMS distance, in cents, of the located partials from the fitted series from which a ladder
#: whose lines the warp left broad is read as hopping between voices. A clean string reads 0.3-3
#: (a grand's treble over few partials to 15), unison and ensemble voices 6-13.
B_SCATTER_CENTS = 6.0
#: Fewest located partials a scatter is read over.
B_SCATTER_MIN_PARTIALS = 5


#: What a row whose motion is undecided takes no reading of: the stiffness (and the partial count it
#: rests on), the tone-to-noise ratio and the valleys.
UNRESOLVED_WITHHELD = ("inharmonicity_b", "tnr_db", "valley_db")


def find_partials(
    seg: np.ndarray,
    sr: int,
    note: int,
    *,
    modal: bool = False,
    stationary_lines_hz: list[float] | tuple[float, ...] = (),
) -> dict:
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

    Under a pitch motion the fundamental is `held_pitch`'s demodulated mean. Every
    level is `partial_level`'s power over the partial's lobe (its FM extent under a
    motion), which does not scallop with the partial's offset from its bin.
    """
    return read_partials(seg, sr, note, modal=modal, stationary_lines_hz=stationary_lines_hz)[0]


def read_partials(
    seg: np.ndarray,
    sr: int,
    note: int,
    *,
    modal: bool = False,
    stationary_lines_hz: list[float] | tuple[float, ...] = (),
) -> tuple[dict, LineStructure | None, list[float]]:
    """`find_partials`'s row, the note's line structure where it was read, and the lines masked.

    B and the fit's partial count come from the segment on its fundamental's clock
    where the warp collapses the lines (`metrics_warp`), and from the raw segment
    otherwise; the located partials and their levels are always the raw
    spectrum's. `modal` fits no B: the row carries the measured partials as
    `modal_profile`. Lines in `stationary_lines_hz` are masked from the spectra
    unless one is also a partial of the note's own series.
    A ladder whose lines stay broad and whose partials scatter off the series
    carries `b_unreliable`.
    """
    freqs, mag = _spectrum(seg, sr)
    et = midi_to_hz(note)
    f0_seed, a0 = _peak_near(freqs, mag, et, 120.0)
    if a0 <= 0.0:
        return {}, None, []
    lines = lines_off_the_series(freqs, mag, f0_seed, a0, sr, modal, list(stationary_lines_hz))
    mag = mask_lines(freqs, mag, lines)
    structure: LineStructure | None = None
    if modal:
        f0, b, fitted_on = f0_seed, 0.0, 0
    else:
        structure = line_structure(seg, sr, f0_seed, pitch_motion(seg, sr, f0_seed))
        if structure.state is CoherenceState.UNDECIDED:
            f0, b, fitted_on = f0_seed, 0.0, 0
        elif structure.unwarped is None:
            f0, b, fitted_on = fit_partial_series(freqs, mag, f0_seed, a0, sr)
        else:
            wf, wm = _spectrum(structure.unwarped, sr)
            wm = mask_lines(wf, wm, lines)
            wf0, wa0 = _peak_near(wf, wm, f0_seed, 120.0)
            f0, b, fitted_on = fit_partial_series(wf, wm, wf0, wa0, sr)
    # The fit's f0 is the stiffness law's, whose h1 sits above it; without a fit it is h1's own.
    h1_ratio = partial_hz(1.0, 1, b) if fitted_on else 1.0
    h1, motion = held_pitch(seg, sr, f0 * h1_ratio, b)
    f0 = h1 / h1_ratio
    found: list[tuple[int, float, float]] = []
    for n in range(1, MAX_PARTIALS + 1):
        predicted = partial_hz(f0, n, b)
        if predicted > 0.45 * sr:
            break
        fn, an = _peak_near(freqs, mag, predicted, INHARMONICITY_TOLERANCES[-1])
        if an > 0.0:
            # The lobe's power, not the bare peak, which scallops with the bin offset. A still
            # lobe centres on the peak found; a moving one on the series, its carrier.
            centre = predicted if motion is not None else fn
            found.append((n, fn, partial_level(freqs, mag, centre, f0, motion)))

    if not found:
        return {}, structure, lines
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
    row = {
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
        "partials_db": [
            round(float(20 * np.log10(max(an, 1e-12) / anchor)), 2) for _, _, an in found
        ],
    }
    if modal:
        row["modal_profile"] = modal_profile(freqs, mag, et)
    if structure is not None:
        if structure.state is not None:
            row["unwarp"] = {
                "state": structure.state.value,
                "coherence": round(structure.coherence, 3),
                "reason": structure.reason,
                "kept": structure.unwarped is not None,
                "width_ratio": None
                if structure.width_ratio is None
                else round(structure.width_ratio, 3),
            }
        if structure.state is CoherenceState.UNDECIDED:
            row["unresolved"] = {"reason": structure.reason, "withheld": list(UNRESOLVED_WITHHELD)}
            del row["inharmonicity_b"]
        if structure.broad:
            row["broad_lines"] = {
                "broad_ratio": round(structure.broad_ratio, 2),
                "width_ratio": None
                if structure.width_ratio is None
                else round(structure.width_ratio, 3),
            }
            scatter, counted = series_scatter_cents(freqs, mag, f0, b, a0, sr)
            if counted >= B_SCATTER_MIN_PARTIALS and scatter >= B_SCATTER_CENTS:
                row["b_unreliable"] = {
                    "reason": (
                        f"unison or ensemble: lines {structure.broad_ratio:.1f}x a still tone's "
                        f"and not read warped, partials {scatter:.1f} c rms off "
                        "one series; stretch, balance, partial_tilt and tnr read through B or "
                        "through partials located on it"
                    ),
                    "dimensions": list(LADDER_DIMENSIONS),
                }
    return row, structure, lines


#: Share of a timbre's rows with broad lines from which its tone-to-noise ratio is unreliable:
#: the mask follows one pitch track, and a chorus or a unison has several.
TNR_BROAD_SHARE = 0.5
TNR_UNRELIABLE_REASON = (
    "chorus or ensemble: most rows have lines the pitch warp does not collapse, "
    "and the tone mask follows one pitch track"
)


def tnr_unreliable(rows: list[dict]) -> dict | None:
    """A capture's `tnr_unreliable` record by timbre, or None where no timbre's lines are mostly broad.

    Read from the rows' own `broad_lines`, per timbre because a gate reads one: an organ
    whose bright registration is clean and whose dark one is not is only half-diluted
    into a capture-wide share. A timbre whose notes mostly carry lines the pitch warp
    cannot collapse is a chorus or an ensemble, whose tone-to-noise ratio the mask
    (one pitch track) cannot separate from noise.
    """
    by_timbre: dict[str, list[dict]] = {}
    for r in rows:
        by_timbre.setdefault(str(r.get("timbre")), []).append(r)
    flagged = {}
    for timbre, group in by_timbre.items():
        broad = sum("broad_lines" in r for r in group)
        if broad >= TNR_BROAD_SHARE * len(group):
            flagged[timbre] = {"rows": broad, "of": len(group)}
    if not flagged:
        return None
    return {"reason": TNR_UNRELIABLE_REASON, "timbres": flagged}


def body_lines_masked(
    held: np.ndarray, sr: int, f0: float, lines_hz: list[float] | tuple[float, ...]
) -> dict:
    """The body lines masked on one row and the share of the band's power they held, or {}."""
    inside = [hz for hz in lines_hz if 40.0 <= hz < f0 * 0.7]
    seg = held[int(BODY_WINDOW_S[0] * sr) : int(BODY_WINDOW_S[1] * sr)]
    if not inside or f0 < BODY_MIN_F0_HZ:
        return {}
    kept = band_power_db(seg, sr, 40.0, f0 * 0.7, inside)
    whole = band_power_db(seg, sr, 40.0, f0 * 0.7)
    if kept is None or whole is None:
        return {}
    return {
        "hz": [round(hz, 1) for hz in inside],
        "share_removed": round(1.0 - 10.0 ** ((kept - whole) / 10.0), 4),
        "reason": BODY_LINES_REASON,
    }


def measure_body_lines(
    manifest: dict, corpus_dir: Path, declared: set[str], preroll_s: float, gate_s: float
) -> dict[str, dict]:
    """The lines each declared timbre's body band carries on notes of unrelated pitch.

    Read from one velocity per note (the one nearest 80), in the body window, relative to
    each note's own spectrum, so a line that follows the note's level is found at any velocity.
    Each masked line records its `line_presence`; a sustained line on one note too few is
    recorded under `missed` with the notes it stands on, and is not masked.
    -> {timbre: {"of": notes, "lines": [{"hz", "notes", "presence", ...}], "missed": [...]}};
    absent where nothing recurs.
    """
    chosen: dict[str, dict[int, dict]] = {}
    for rec in manifest["renders"]:
        if rec["timbre"] in declared and midi_to_hz(rec["note"]) >= BODY_MIN_F0_HZ:
            held = chosen.setdefault(rec["timbre"], {})
            if rec["note"] not in held or abs(rec["velocity"] - 80) < abs(
                held[rec["note"]]["velocity"] - 80
            ):
                held[rec["note"]] = rec
    out: dict[str, dict] = {}
    for timbre, notes in chosen.items():
        per_note = []
        # Per entry of `per_note`: the note, its body window, and its preroll and post-release tail.
        reads: list[tuple[int, np.ndarray, list[np.ndarray], int]] = []
        for note, rec in notes.items():
            path = corpus_dir / rec["path"]
            if not path.exists():
                continue
            audio, sr = read_wav(path)
            mono = to_mono(audio)
            on = int(sound_onset_s(mono, sr, preroll_s, len(mono) / sr) * sr)
            off = on + int(gate_s * sr)
            seg = mono[on:off][int(BODY_WINDOW_S[0] * sr) : int(BODY_WINDOW_S[1] * sr)]
            if seg.size < sr // 8:
                continue
            f0 = midi_to_hz(note)
            per_note.append((f0, body_lines_in(seg, sr, f0)))
            idle = [mono[:on], mono[off + int(IDLE_AFTER_RELEASE_S * sr) :]]
            reads.append((note, seg, idle, sr))
        masked = []
        for hz, idx in recurring_body_line_groups(per_note):
            note_db = [line_amplitude_dbfs(reads[i][1], reads[i][3], hz) for i in idx]
            masked.append(
                {
                    "hz": hz,
                    "notes": len(idx),
                    **line_presence(
                        [db for db in note_db if db is not None],
                        [seg for i in idx for seg in reads[i][2]],
                        reads[idx[0]][3],
                        hz,
                    ),
                }
            )
        missed = [
            {
                "hz": hz,
                "notes": len(idx),
                "on_notes": [reads[i][0] for i in idx],
                "reason": BODY_LINE_MISSED_REASON,
            }
            for hz, idx in missed_body_lines(per_note)
        ]
        if masked or missed:
            out[timbre] = {"of": len(per_note), "lines": masked}
            if missed:
                out[timbre]["missed"] = missed
    return out


def measure_stationary_lines(
    manifest: dict, corpus_dir: Path, declared: set[str], preroll_s: float, gate_s: float
) -> dict[str, dict]:
    """The lines each declared timbre latches on, from its softest take of every note.

    -> {timbre: {"of": takes, "lines": [{"hz": ..., "takes": ...}]}}; a timbre with
    none is absent, so a capture without them reads exactly as before.
    """
    softest: dict[str, dict[int, dict]] = {}
    for rec in manifest["renders"]:
        if rec["timbre"] not in declared:
            continue
        held = softest.setdefault(rec["timbre"], {})
        if rec["note"] not in held or rec["velocity"] < held[rec["note"]]["velocity"]:
            held[rec["note"]] = rec
    out: dict[str, dict] = {}
    for timbre, notes in softest.items():
        takes = []
        for rec in notes.values():
            path = corpus_dir / rec["path"]
            if not path.exists():
                continue
            audio, sr = read_wav(path)
            got = held_lines(to_mono(audio), sr, gate_s, preroll_s)
            if got is not None:
                takes.append(got[0])
        lines = stationary_lines(takes)
        if lines:
            out[timbre] = {
                "of": len(takes),
                "lines": [{"hz": ln.hz, "takes": ln.takes} for ln in lines],
            }
    return out


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

#: How long after arriving a note may still be climbing to the peak its decay is
#: fitted from. Measured on this corpus the longest any voice with a real peak
#: takes is 185 ms -- a vibraphone bar blooming -- while the earliest a plateau
#: puts its highest sample is 375 ms, so the two populations do not overlap.
RISE_WINDOW_S = 0.25


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
    return start + int(np.argmax(env_db[start : min(env_db.size, start + span)]))


#: `metrics_attack.ATTACK_BANDS_HZ` band the gate reads: 4-8 kHz, where a felt
#: lowpass's cutoff or width first shows; every other read here starts at 120 ms.
ATTACK_HF_BAND_INDEX = 0


def attack_hf_share_db(mono: np.ndarray, sr: int, note: int, onset_s: float) -> float | None:
    """4-8 kHz share of the strike's own broadband level, through the first 120 ms.

    Reuses `attack_bands` rather than a second spectral pass. Each 20 ms slice
    is its own band-over-total-power ratio, so a gain difference between two
    captures cancels; the six are collapsed to their median so one bad slice does not swing the row.
    """
    bands = attack_bands(mono, sr, Note(note, 0, 0.0, 0.0), onset_s)
    n_bands = len(ATTACK_BANDS_HZ)
    vals = [v for v in bands[ATTACK_HF_BAND_INDEX::n_bands] if v is not None]
    return round(float(np.median(vals)), 2) if vals else None


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


def register_deltas(
    model: dict[int, dict[int, float]], ref: dict[int, dict[int, float]]
) -> list[float]:
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
        notes = sorted(n for n in set(model) & set(ref) if vel in model[n] and vel in ref[n])
        if len(notes) < REGISTER_MIN_NOTES:
            continue
        m_mid = float(np.median([model[n][vel] for n in notes]))
        r_mid = float(np.median([ref[n][vel] for n in notes]))
        out.extend((n, vel, (model[n][vel] - m_mid) - (ref[n][vel] - r_mid)) for n in notes)
    return out


def usable_decay_end(env_db: np.ndarray, start_i: int, range_db: float = DECAY_RANGE_DB) -> int:
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


def partial_decay(
    seg: np.ndarray, sr: int, partials_hz: list[float], n_partials: int = 8
) -> list[float]:
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
        band = spec[:, max(0, k - 1) : k + 2].max(axis=1)
        db = _db(band / max(band.max(), 1e-12))
        keep = db > -55.0
        if keep.sum() < 5:
            out.append(float("nan"))
            continue
        out.append(round(float(np.polyfit(t[keep], db[keep], 1)[0]), 2))
    return out


#: Pole Q of the two biquads that cascade into a 4th-order Butterworth high-pass.
_BUTTERWORTH4_Q = (0.5411961001461970, 1.3065629648763766)

#: History before the key lift the release filter runs over before it is read.
DAMPER_SETTLE_S = 0.5


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

    The filter is causal, so the caller hands it history before the key lift to
    settle on. A brick-wall cut in the frequency domain spreads its truncation
    error across the whole segment, and a release that falls fast then stalls
    on that leakage 33 to 38 dB down instead of reaching the 40 dB it is timed to.
    """
    y = seg.astype(np.float64)
    if len(y) < 16:
        return y
    w0 = 2.0 * np.pi * max(20.0, 0.6 * f0_hz) / sr
    cw, sw = float(np.cos(w0)), float(np.sin(w0))
    for q in _BUTTERWORTH4_Q:
        alpha = sw / (2.0 * q)
        a0 = 1.0 + alpha
        b0 = (1.0 + cw) / (2.0 * a0)
        b1, a1, a2 = -2.0 * b0, -2.0 * cw / a0, (1.0 - alpha) / a0
        out: list[float] = []
        x1 = x2 = y1 = y2 = 0.0
        for x0 in y.tolist():
            o = b0 * (x0 + x2) + b1 * x1 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x0, y1, o
            out.append(o)
        y = np.asarray(out)
    return y


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


def reference_tnr_mask(row: dict) -> dict:
    """The mask a reference row's tone-to-noise ratio was read through, for its model note.

    A row measured before the mask was recorded is refused rather than read as a
    still tone, which would put the model back on its own mask.
    """
    if "tnr_mask" not in row:
        raise SystemExit(
            f"reference row {row.get('note')}/{row.get('velocity')} records no tnr_mask; "
            "re-measure the reference with `profile.py measure`"
        )
    return row["tnr_mask"]


def one_shot_window(
    audio: np.ndarray, sr: int, note: int, *, preroll_s: float, recorded_s: float
) -> dict:
    """The window of one recording of a module one-shot capture; see `profile_window.row_window`.

    The partials the floor reading needs are found the way `measure_note` finds them, on
    the same segment of the recording, so the window is decided before the row is read.
    """
    mono = to_mono(audio)
    on = int(sound_onset_s(mono, sr, preroll_s, len(mono) / sr) * sr)
    held = mono[on:]
    a = int(0.12 * sr)
    partials: list[float] = []
    clear_end_s = None
    if held.size > a + sr // 4:
        partials = (
            find_partials(held[a : min(len(held), a + int(1.5 * sr))], sr, note).get("partials_hz")
            or []
        )
        clear = partial_clearance(held[a:], sr, partials).get("partial_clear_s") if partials else []
        # The frame `partial_clearance` read ends a window after the time it reports.
        last = max((c for c in clear or [] if c is not None), default=None)
        if last is not None:
            clear_end_s = a / sr + last + max(1024, int(0.093 * sr)) / sr
    t_env, env = _rms_envelope(held, sr)
    return row_window(
        held, sr, int(recorded_s * 1000.0), partials, t_env, np.asarray(_db(env)), clear_end_s
    )


def note_gate(
    audio: np.ndarray, sr: int, preroll_s: float, gate_s: float
) -> tuple[np.ndarray, np.ndarray, float, int, int] | None:
    """(mono, held, onset_s, on, off): the onset-anchored gate of a captured note, or None."""
    mono = to_mono(audio)
    # From the note-on, the same origin `analyze_hit` and `note_onset` give it:
    # the preroll is there to absorb the plugin's first buffer, so a scan that
    # began at the file would answer with whatever that buffer left behind.
    onset_s = sound_onset_s(mono, sr, preroll_s, len(mono) / sr)
    on = int(onset_s * sr)
    off = on + int(gate_s * sr)
    held = mono[on:off]
    if held.size < sr // 4:
        return None
    return mono, held, onset_s, on, off


def fit_window(held: np.ndarray, sr: int, read_end: int | None = None) -> tuple[int, int]:
    """(a, b): the window of `held` the partial fit reads, ending by `read_end` when given."""
    # The spectrum for the partial fit comes from a window that starts after the
    # strike: the hammer noise is broadband and would blur every peak the fit
    # depends on, while a window too late has lost the high partials entirely.
    a = int(0.12 * sr)
    b = min(len(held), a + int(1.5 * sr))
    if read_end is not None:
        b = min(b, read_end)
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
    return a, b


def note_windows(
    audio: np.ndarray, sr: int, preroll_s: float, gate_s: float
) -> tuple[np.ndarray, np.ndarray, int, int, float, int, int] | None:
    """(mono, held, a, b, onset_s, on, off): where a captured note's gate and fit window sit, or None.

    `held` is the onset-anchored gate and `held[a:b]` the window the partial fit
    reads; `measure_note` and the recording-floor pass place them the same way.
    """
    gate = note_gate(audio, sr, preroll_s, gate_s)
    if gate is None:
        return None
    mono, held, onset_s, on, off = gate
    a, b = fit_window(held, sr)
    return mono, held, a, b, onset_s, on, off


def reference_loop(row: dict) -> dict:
    """The loop record a reference row was read through, for its model note.

    A row measured before the record existed reads as unrecorded, so the model is
    measured whole as it always was rather than put through a window nobody gave it.
    """
    return row.get("loop") or {"verdict": VERDICT_UNRECORDED}


def measure_note(
    audio: np.ndarray,
    sr: int,
    note: int,
    *,
    preroll_s: float,
    gate_s: float,
    tnr_mask: dict | None = None,
    modal: bool = False,
    stationary_lines_hz: list[float] | tuple[float, ...] = (),
    body_lines_hz: list[float] | tuple[float, ...] = (),
    body_lines_missed_hz: list[float] | tuple[float, ...] = (),
    loop: dict | None = None,
    capture_id: str | None = None,
    window: dict | None = None,
) -> dict:
    """Every measurement this profile carries, for one captured note.

    `tnr_mask` is the reference row's `tnr_mask`, given when the note is the
    model's, so the tone-to-noise ratio is read through the reference's mask;
    without it the note is measured through its own. Either way the row records
    the mask it was read through. `modal` and `stationary_lines_hz` are the
    reference's too (`find_partials`), so a model row is read as its reference was. `body_lines_hz` are
    masked from the body band; `body_lines_missed_hz`, the lines on one note too few, are
    only recorded under `body_lines.missed`, each with the share of the band it holds.

    `loop` is the reference row's `loop` record, given when the note is the
    model's, so a spectrum the reference read only up to its loop join is read
    to the same time; without it the note is tested for a loop and records the
    verdict, `capture_id` deciding whether the spectral reads stop at the join.

    `window` is `one_shot_window`'s answer for a capture read per note; its end is then
    the `gate_s` given, and the row records it with the rule that chose it. Every row
    records the window the model is read through beside the span the recording gave.

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
    gate = note_gate(audio, sr, preroll_s, gate_s)
    if gate is None:
        return {}
    mono, held, onset_s, on, off = gate
    measured_loop = loop is None
    loop_rec = detect_loop(held, sr) if measured_loop else dict(loop)
    if measured_loop:
        until_s = read_to_join_s(loop_rec, capture_id)
        if until_s is not None:
            loop_rec["read_to_join_s"] = until_s
    until_s = loop_rec.get("read_to_join_s")
    read_end = None if until_s is None else int(until_s * sr)
    a, b = fit_window(held, sr, read_end)
    partials, structure, masked = read_partials(
        held[a:b], sr, note, modal=modal, stationary_lines_hz=stationary_lines_hz
    )
    row: dict = dict(partials)
    # Tone-to-noise and the valleys read the segment on its fundamental's clock where the warp kept it.
    warped = structure is not None and structure.unwarped is not None
    read_seg = structure.unwarped if warped else held[a:b]
    if not row:
        # No fundamental means this is not a struck note, and every measurement
        # below would still produce a number for it — a decay slope of 0 dB/s
        # and a damper release the length of the window. A profile is worse for
        # holding those than for being short a row.
        return {}
    if stationary_lines_hz:
        row["stationary_lines_hz"] = list(stationary_lines_hz)

    body_src = held if read_end is None else held[:read_end]
    body = body_below_f0_db(body_src, sr, float(row.get("f0_hz") or 0.0), body_lines_hz)
    if body is not None:
        row["body_below_f0_db"] = body
        bands = body_bands(body_src, sr, float(row["f0_hz"]), body_lines_hz)
        if bands is not None:
            row["body_under_db"], row["body_f0_db"] = round(bands[0], 2), round(bands[1], 2)
        masked_body = body_lines_masked(body_src, sr, float(row["f0_hz"]), body_lines_hz)
        if masked_body:
            row["body_lines"] = masked_body
        missed = [
            body_lines_masked(body_src, sr, float(row["f0_hz"]), [hz])
            for hz in body_lines_missed_hz
        ]
        missed = [{"hz": m["hz"][0], "share": m["share_removed"]} for m in missed if m]
        if missed:
            row.setdefault("body_lines", {})["missed"] = {
                "lines": missed,
                "reason": BODY_LINE_MISSED_REASON,
            }

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
    hf = attack_hf_share_db(mono, sr, note, onset_s)
    if hf is not None:
        row["attack_hf_db"] = hf
    # Absent rather than null where the note is not held, as every unmeasured field here is.
    row.update(
        {
            k: v
            for k, v in onset_shape(mono, sr, onset_s, gate_s, row["f0_hz"]).items()
            if v is not None
        }
    )

    tail = slice(origin_i, usable_decay_end(env_db, origin_i))
    # How much of the held note the two rates were fitted over. Both are slopes,
    # so they only compare against a reference fitted over a comparable span --
    # a two-stage decay read to 4 s and one read to 8 is two different questions.
    row["decay_span_s"] = (
        round(float(t_env[tail][-1] - t_env[origin_i]), 3) if t_env[tail].size else 0.0
    )
    if t_env[tail].size > 8:
        row["decay_db_s"] = round(float(np.polyfit(t_env[tail], env_db[tail], 1)[0]), 2)
        row.update(double_decay(env_db[tail], t_env[tail]))
    if row.get("partials_hz"):
        row["partial_decay_db_s"] = partial_decay(held[a:], sr, row["partials_hz"])
        row.update(partial_clearance(held[a:], sr, row["partials_hz"]))
        if "unresolved" not in row:
            row.update(valley_profile(read_seg, sr, row["partials_hz"], row["partials_db"]))
    row.update(recording_floor(mono, sr, preroll_s, float(env_db[peak_i]), f0=row["f0_hz"]))

    # Damper: note-off to 40 dB below the level it was still holding. Skipped
    # when the string had already stopped — a held C7 is 45 dB down long before
    # the key comes up, and "how fast the damper stopped it" then measures the
    # noise floor and reports it as a very fast damper.
    # Read from the key lift, the score's note-off: `off` is the onset-anchored window end.
    lift = int((preroll_s + gate_s) * sr)
    held_peak_db = float(env_db[peak_i])
    lift_i = min(int(np.searchsorted(t_env, (lift - on) / sr)), env.size - 1)
    off_db = float(_db(env[lift_i])) if env.size else -240.0
    row["decayed_before_note_off"] = bool(off_db < held_peak_db - 45.0)
    rel = mono[lift:]
    if rel.size > sr // 20 and not row["decayed_before_note_off"]:
        settle = min(lift - on, int(DAMPER_SETTLE_S * sr))
        filtered = _above_fundamental(mono[lift - settle :], sr, row["f0_hz"])[settle:]
        t_rel, env_rel = _rms_envelope(filtered, sr)
        start_db = float(_db(env_rel[0]))
        under = np.where(_db(env_rel) < start_db - 40.0)[0]
        row["damper_release_ms"] = (
            round(float(t_rel[under[0]] * 1000.0), 1)
            if under.size
            else round(float(t_rel[-1] * 1000.0), 1)
        )
        row["damper_capped"] = not bool(under.size)

    freqs, mag = _spectrum(held[a:b], sr)
    p = mask_lines(freqs, mag, masked) ** 2
    row["centroid_hz"] = round(float((freqs * p).sum() / max(p.sum(), 1e-20)), 1)
    if warped:
        freqs, mag = _spectrum(read_seg, sr)
    stiffness = row["inharmonicity_b"] if row.get("inharmonicity_reliable") else 0.0
    own_mask = measure_tnr_mask(held[a:b], sr, row["f0_hz"])
    # The note's own pitch motion, beside the mask it is read through, since the model's
    # mask is its reference's. Where the reference is measured, the coherence of the
    # partials' tracks says whether one source made it.
    if "fm" in own_mask:
        row["fm"] = own_mask["fm"]
        if tnr_mask is None:
            row["fm_warp"] = warp_reading(held[a:b], sr, row["f0_hz"])
            row["fm_coherence"] = round(partial_coherence(held[a:b], sr, row["f0_hz"]), 3)
    if tnr_mask is None:
        tnr_mask = own_mask
    row["tnr_mask"] = tnr_mask
    tnr = (
        float("nan")
        if "unresolved" in row
        else tone_to_noise_db(freqs, mag, row["f0_hz"], stiffness, *tnr_mask_parts(tnr_mask))
    )
    if np.isfinite(tnr):
        row["tnr_db"] = round(tnr, 2)
    width = channel_width(audio, on, off)
    if width is not None:
        row["stereo_width"] = width
    if measured_loop and loop_rec.get("verdict") == VERDICT_LOOPED:
        loop_rec["period_pitch_periods"] = round(
            loop_rec["period_ms"] * float(row["f0_hz"]) / 1000.0, 1
        )
        lines = (
            body_loop_lines(float(row["f0_hz"]), loop_rec["period_ms"])
            if body is not None
            else None
        )
        if lines is not None:
            loop_rec["body_lines"] = lines
        loop_rec["excluded"], loop_rec["snapshot"] = exclusions(
            loop_rec,
            body_on_lines=bool(lines and lines["on_lines"]),
            attack_ms=row["attack_ms"],
            body_floor=body_floor_state(row),
        )
    if measured_loop:
        ladder = held_ladder(held, sr, midi_to_hz(note))
        if ladder is not None:
            loop_rec["ladder_db"] = ladder
    row["loop"] = loop_rec
    row["window"] = {"model_s": round(gate_s, 4), "reference_s": round(held.size / sr, 4)}
    if window is not None:
        row["window_end_ms"] = window["end_ms"]
        row["window_rule"] = window["rule"]
        row["window_recorded_ms"] = window["recorded_ms"]
        for key in ("end_clearance_db", "sound_end_ms", "marks"):
            if key in window:
                row[f"window_{key}"] = window[key]
    return row


def measure_hit(
    audio: np.ndarray,
    sr: int,
    note: int,
    velocity: int,
    *,
    preroll_s: float,
    gate_s: float,
    max_band_hz: float | None = None,
) -> dict:
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
    hit = analyze_hit(
        mono,
        sr,
        Note(note, velocity, preroll_s, gate_s),
        len(mono) / sr,
        max_band_hz=max_band_hz,
        stereo=audio if audio.ndim == 2 and audio.shape[1] == 2 else None,
    )
    row = hit.to_dict()
    strike = mono[int(preroll_s * sr) :]
    row["peak_dbfs"] = round(float(_db(float(np.max(np.abs(strike))))), 2) if len(strike) else None
    return row
