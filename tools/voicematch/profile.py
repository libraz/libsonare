"""Turn a captured corpus into a reference profile, and score a model against it.

    profile.py measure                 # corpus WAVs -> reference/<id>.json
    profile.py compare                 # render the same grid through libsonare
                                       # and report where it diverges

The audio a capture produces cannot be committed — a sample library's licence
covers what is rendered from it — and would be the wrong thing to commit even
if it could: what a physical model has to reproduce is a handful of measured
properties, not four hundred megabytes of one particular instrument. `measure`
extracts those properties and they are what lives in the repository, under
`reference/<capture id>.json`.

What it measures is everything a stiff, plucked or struck string can be wrong
about. None of it is in `metrics.py`, whose per-note analysis assumes an exact
harmonic series and a probe rendered by both sides:

- **Inharmonicity.** A stiff string puts its nth partial at
  `n*f0*sqrt(1 + B*n^2)` rather than at `n*f0`. B is measured by fitting that
  law to the partials that are actually there, not assumed — which is what
  makes the same measurement carry a piano (1e-4 in the middle to over 1e-2 at
  the top, and most of what separates a piano from a harp) and a harpsichord
  (near zero on the speaking length, and non-zero only through the segment
  behind the bridge).
- **Stretch tuning.** An instrument with inharmonic strings is deliberately
  tuned away from equal temperament, and by how much is a property of the
  instrument. A model tuned to exact equal temperament beats against the
  recording, and the beating is the first thing anyone hears.
- **Double decay.** A string does not decay along one line: the strings of a
  unison fall out of phase, and the fast initial decay gives way to a slower
  aftersound at a knee the fit locates rather than assumes. Where that knee
  falls is itself the evidence for how many strings are sounding.
- **Damper release.** Note-off is a damper landing on a moving string, which is
  neither instant nor an exponential release — and on some instruments part of
  the compass has no damper at all, which this reports rather than averages in.
- **Tone-to-noise.** The mechanism against the string. A model with the partial
  stack right and no action noise reads cleaner than any recording, which every
  spectral metric scores as an improvement.
- **Velocity response.** How far the level moves from the softest blow to the
  hardest, and whether it moves monotonically. On a plucked instrument this is
  most of the instrument's identity and no timbre metric can see it.

A capture whose timbres declare MIDI channel 10 is measured with none of that.
There its note numbers select instruments rather than pitches, so there is no
fundamental for any of the six to be about, and what is measured instead is the
1/3-octave band profile, the per-octave decay of it, the attack, the crest and
the velocity response. That path is `measure_hit` and `compare_percussion`, and
it is separate code rather than a branch: the columns that replace the pitched
ones are not the same columns under different names.

`compare` renders the same note-and-velocity grid through libsonare's GM
fallback — on the program the capture names, not a fixed one, and on the channel
it names — measures it the same way, and prints the differences. That is the
part which says what to change. `--gate` holds those differences to bounds
recorded earlier, which is what catches the change that improves one dimension
by breaking another.
"""

# Every importer reads this module by name, and the tests set attributes on it —
# so the whole surface is re-exported here, private names included.
# ruff: noqa: F401

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT
from capture import (
    DEFAULT_CONFIG,
    PERCUSSION_CHANNEL,
    RIG_UNCLASSIFIED,
    ROOM_NONE,
    ROOM_UNCLASSIFIED,
    load_config,
    model_rig,
    note_groups,
    note_map,
    out_root,
    tail_seconds,
)
from corpus import load_corpus
from loss import KIT_MIN_MEMBERS, kit_report
from metrics import (
    ATTACK_FLOOR_MS,
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
    band_edges_by_timbre,
    band_tilt_db,
    fit_partial_series,
    ladder_present,
    measure_agreement_edge,
    midi_to_hz,
    partial_hz,
    shared_band_edge,
    to_mono,
)
from phrases import TAKE_SETS, build_takes
from profile_gate import (
    DELTA_LABELS,
    PER_NOTE_DIMENSIONS,
    check_gate,
    print_register_profile,
    print_summary_table,
    print_vanished_dimensions,
    reference_spread,
    register_levels,
    register_spread_by_note,
    select_dimensions,
    summarize_deltas,
    write_gate_file,
)
from profile_measure import (
    BODY_MIN_F0_HZ,
    BODY_WINDOW_S,
    DECAY_RANGE_DB,
    DECAY_SPAN_AGREEMENT,
    MAX_PARTIALS,
    ONSET_SLACK_DB,
    REGISTER_MIN_NOTES,
    RISE_WINDOW_S,
    SHORT_RING_FLOOR_DB,
    _above_fundamental,
    _short_ring_window,
    arrival_index,
    body_below_f0_db,
    decay_origin_index,
    double_decay,
    double_decay_gap,
    find_partials,
    is_percussion,
    measure_hit,
    measure_note,
    partial_decay,
    register_deltas,
    tone_to_noise_db,
    usable_decay_end,
)
from profile_percussion import (
    band_decay_reach,
    band_shape_error_db,
    mean_band_decay_delta,
    percussion_reference_spread,
    percussion_row_deltas,
    print_kit_relations,
    ring_collapsed,
)
from profile_status import (
    CAPTURE_DIR,
    committed_capture,
    measurement_stamp,
    profile_body,
    readiness,
    status,
)
from profile_summary import (
    LATE_ONSET_MS,
    a4_offset_cents,
    band_db,
    partial_balance_db,
    print_percussion_summary,
    print_summary,
    profile_bank,
    profile_program,
    summarize,
    summarize_percussion,
    velocity_response,
)
from profile_take import (
    SUSTAIN_CC,
    SUSTAIN_TONALITY_BAND,
    TAKE_BANDS,
    TAKE_LABELS,
    TAKE_SILENCE,
    TAKE_SUBSONIC_HZ,
    TAKE_TAIL_LEAD_S,
    archived_take_ids,
    archived_take_references,
    highpass,
    measure_take,
    room_match,
    signal_end_s,
    take_windows,
    usable_tail,
)
from render_model import render_model
from render_oracle import render_oracle_fluidsynth
from room import Room, match_sends, measurable_room, place_model_in
from smf import Note, write_smf
from wavio import read_wav, write_wav

REFERENCE_DIR = Path(__file__).resolve().parent / "reference"
# A model render whose loudest sample is under this is a kit piece the GM
# fallback does not voice, not a quiet one. `analyze_hit` normalises the band
# profile to its own loudest band, so silence comes back as a flat spectrum and
# scores as a plausible instrument rather than as an absence.
SILENT_HIT_DBFS = -80.0
# What a recorded room carries back into a `Room`. The entry beside them says how
# many notes agreed and over what range, which is provenance rather than the space.
ROOM_FIELDS = tuple(f.name for f in dataclasses.fields(Room))


def reference_window(cfg: dict, corpus_dir: Path | None, timbre: str, *,
                     preroll_s: float, gate_s: float):
    """How long to render one slot for: the span its reference row was read over.

    A reference row is measured over the WHOLE recording the corpus holds for it,
    and a recording read out of a module's own sample data is as long as the
    instrument rang rather than as long as the gate — 0.42 s to 2.09 s across one
    seventeen-key kit captured at a flat 400 ms gate. Rendering the model for
    `preroll + gate + tail` then differences a decay fitted over two seconds
    against one fitted over four hundred milliseconds, and what the model's
    column reports is the length of its own buffer.

    The manifest records each slot's own length, so the window is read from
    there. Where it records none, and for a grid the manifest does not cover, the
    fallback is the declared `preroll + gate + tail` — which is also exactly what
    a rendered oracle's manifest records, so on those grids this is the same
    number and nothing moves.
    """
    slots: dict[tuple[int, int], float] = {}
    if corpus_dir is not None:
        try:
            slots = load_corpus(corpus_dir, timbre).slots
        except (OSError, ValueError, KeyError):
            slots = {}

    def window(note: int, velocity: int) -> float:
        recorded = slots.get((note, velocity))
        if recorded is None:
            return preroll_s + gate_s + tail_seconds(cfg, note)
        return preroll_s + recorded

    return window


# --------------------------------------------------------------------------
# measure


def measure_rooms(manifest: dict, corpus_dir: Path, declared: set[str],
                  preroll_s: float, gate_s: float,
                  answer: str = ROOM_UNCLASSIFIED) -> dict:
    """The space each timbre was recorded in, where its own grid can say.

    Recorded here rather than measured at comparison time because `compare` reads
    the committed profile and never the audio, so a room that lived only in the
    corpus would be unavailable from a clean clone — and because the room is a
    property of the reference, which is what this file holds.

    **A room is common to every note and an instrument's own decay is not**, so
    the estimate is taken per note and kept only where most of the compass agrees
    there is one. Measured over the whole grid at once instead, a single note
    carries the answer: the electric guitar's DI reports a 1.17 s space that way,
    and note by note it is 3.56 s on one note of ten and nothing on the other
    nine. `room.py`'s own guards do not catch that one — the length is credible
    and the notes are held for seconds — so the agreement is the guard.

    The rest of the guards are `measurable_room`'s: a small space, a dry render
    and a probe whose windows are gates rather than notes all come back as no
    room. A timbre with no entry is compared as rendered, which on all but a few
    captures is the right answer and the one this records.

    **None of that reaches a source whose own release lasts as long as a small
    hall**, because every guard here separates a room from *one note's* decay and
    a release belongs to every note exactly as a room does — it is one envelope
    generator, so the agreement test above passes on it perfectly. The capture is
    the only thing that can answer, which is what `room: none` is for, and it is
    the same answer `takes` has always asked the capture for rather than guessing.
    """
    if answer == ROOM_NONE:
        print("  the capture answers `room: none`, so no space is measured — "
              "a note-tail estimate cannot tell a room from a long release, and "
              "the one it would record is convolved onto every model render "
              "before any figure is taken", file=sys.stderr)
        return {}
    by_timbre: dict[str, dict[int, tuple[int, Path]]] = {}
    for rec in manifest["renders"]:
        if rec["timbre"] not in declared:
            continue
        path = corpus_dir / rec["path"]
        if not path.exists():
            continue
        # The loudest take of each note: a T20 fit needs the tail 25 dB clear of
        # the floor, which the softest blow of a decaying instrument is not.
        slot = by_timbre.setdefault(rec["timbre"], {})
        if rec["velocity"] >= slot.get(rec["note"], (-1, None))[0]:
            slot[rec["note"]] = (rec["velocity"], path)
    rooms: dict[str, dict] = {}
    for tid, by_note in sorted(by_timbre.items()):
        measured: list[Room] = []
        for note in sorted(by_note):
            audio, sr = read_wav(by_note[note][1])
            mono = audio.mean(axis=1) if audio.ndim > 1 else audio
            room = measurable_room(np.asarray(mono, dtype=np.float64), sr,
                                   [(preroll_s, preroll_s + gate_s)])
            if room is not None:
                measured.append(room)
        probed = len(by_note)
        if len(measured) * 2 <= probed:
            if measured:
                print(f"  {tid}: {len(measured)} of {probed} notes measure a space and the "
                      f"rest measure none, so what one note decays like is not a room — "
                      f"compared as rendered", file=sys.stderr)
            continue
        rt60 = sorted(r.rt60_s for r in measured)
        # Rounded like every other measurement in the file. A room quoted to the
        # last bit would also re-stamp the profile — and date every gate against
        # it — on an arithmetic drift far below what the estimate can resolve.
        entry = {k: round(float(np.median([getattr(r, k) for r in measured])), 4)
                 for k in ROOM_FIELDS}
        entry.update({"notes_measured": len(measured), "notes_probed": probed,
                      "rt60_min_s": round(rt60[0], 4), "rt60_max_s": round(rt60[-1], 4)})
        rooms[tid] = entry
        print(f"  {tid}: recorded in a space of RT60 {entry['rt60_s']:.2f}s "
              f"({rt60[0]:.2f}-{rt60[-1]:.2f} over {len(measured)} of {probed} notes), tail "
              f"level {entry['tail_db']:+.1f}dB, HF ratio {entry['hf_ratio']:.2f} — a "
              f"comparison places the model in it before measuring", file=sys.stderr)
    return rooms


def measure(cfg: dict, corpus_dir: Path, out_path: Path) -> int:
    manifest_path = corpus_dir / "manifest.json"
    if not manifest_path.exists():
        print(f"no corpus at {manifest_path} — run `capture.py corpus` first", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text())
    preroll_s = manifest["preroll_ms"] / 1000.0
    gate_s = manifest["gate_ms"] / 1000.0
    percussion = is_percussion(cfg)

    # What this profile is allowed to measure: the timbres the capture DECLARES,
    # and nothing else the corpus happens to hold.
    #
    # Two things end up in a manifest that are not the reference. `render-grid`
    # adds the model's own grid on purpose, so that every tool reading a corpus
    # reads the model with no special case. And a re-capture keeps whatever the
    # previous definition rendered, because `corpus --resume` preserves timbres
    # it does not recognise rather than discarding an expensive render — so an
    # instrument re-captured from a different product still has the old one on
    # disk under the old timbre id.
    #
    # Measuring either is silent: `committed_capture` already intersects with
    # the tracked definition, so the profile would declare one timbre and hold
    # the statistics of two. The second case is the dangerous one, since the
    # retired reference is usually retired for being wrong — a bass replaced
    # because its rig was baked in would have gone on contributing to the DI
    # profile that replaced it.
    declared = {t["id"] for t in cfg.get("timbres", [])}

    def sweep(max_band_hz: float | None) -> list[dict]:
        rows: list[dict] = []
        for i, rec in enumerate(manifest["renders"], 1):
            if rec["timbre"] not in declared:
                continue
            path = corpus_dir / rec["path"]
            if not path.exists():
                continue
            audio, sr = read_wav(path)
            m = (measure_hit(audio, sr, rec["note"], rec["velocity"],
                             preroll_s=preroll_s, gate_s=gate_s,
                             max_band_hz=max_band_hz)
                 if percussion else
                 measure_note(audio, sr, rec["note"], preroll_s=preroll_s, gate_s=gate_s))
            if not m:
                print(f"  {rec['id']}: nothing measurable", file=sys.stderr)
                continue
            rows.append({"timbre": rec["timbre"], "note": rec["note"],
                         "velocity": rec["velocity"], **m})
            if i % 20 == 0:
                print(f"  {i}/{len(manifest['renders'])}", file=sys.stderr)
        return rows

    rows = sweep(None)
    # What this capture can actually measure, before anything is committed as
    # what the instrument does. A band profile is normalised to its own loudest
    # band, so the edge cannot be applied to a measured profile after the fact —
    # the whole sweep is taken again against it. See `measure_band_edge`.
    band_edge = shared_band_edge(rows) if percussion else None
    if percussion:
        # Named per reference rather than as one number: when they differ, the
        # capture is held to the narrowest, and a reader who is not told which
        # one set the ceiling cannot tell a wide reference from a wasted one.
        per_timbre = band_edges_by_timbre(rows)
        if len(per_timbre) > 1:
            for tid, edge in per_timbre.items():
                print(f"  {tid}: bandwidth "
                      f"{'no measurable ceiling' if edge is None else f'{edge / 1000.0:.1f} kHz'}",
                      file=sys.stderr)
            # Printed beside them because it is a different question with the
            # same answer type, and a ceiling nobody can attribute reads as one
            # reference being narrow when it is the two of them disagreeing.
            agreed = measure_agreement_edge(rows)
            print(f"  references agree to "
                  f"{'their whole range' if agreed is None else f'{agreed / 1000.0:.1f} kHz'}",
                  file=sys.stderr)
    if band_edge is not None:
        print(f"\ncapture bandwidth: {band_edge / 1000.0:.1f} kHz — re-measuring so "
              f"the band profile is normalised over what this capture carries",
              file=sys.stderr)
        rows = sweep(band_edge)

    rooms = measure_rooms(manifest, corpus_dir, declared, preroll_s, gate_s,
                          str(cfg.get("room", ROOM_UNCLASSIFIED)))

    tracked = json.loads(Path(cfg["_path"]).read_text())
    profile = {
        "id": cfg["id"],
        "label": tracked["label"],
        # Filled in below, once there is a body to compare against what is
        # already on disk. Declared here so the key keeps its place in the file.
        "measured_utc": "",
        "capture": {**committed_capture(cfg, tracked, manifest),
                    # Part of the method, not of the instrument: it says what
                    # this recording chain could hear, so a later comparison can
                    # decline to score the model where the reference is blind.
                    "band_edge_hz": band_edge},
        "rows": rows,
        "summary": summarize_percussion(rows) if percussion else summarize(rows),
    }
    # Absent rather than empty on a reference with no room, so the profile of a
    # dry capture is byte-identical to one measured before this was recorded and
    # its stamp — and every gate dated against it — stays where it is.
    if rooms:
        profile["rooms"] = rooms
    before = out_path.read_text() if out_path.exists() else ""
    profile["measured_utc"] = measurement_stamp(profile, out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    written = json.dumps(profile, indent=1, ensure_ascii=False) + "\n"
    out_path.write_text(written)
    print(f"\n{len(rows)} measured notes -> {out_path}", file=sys.stderr)
    print(f"unchanged since {profile['measured_utc']} — the file is byte-identical"
          if written == before else
          f"measurement stamped {profile['measured_utc']}", file=sys.stderr)
    if percussion:
        print_percussion_summary(profile["summary"])
    else:
        print_summary(profile["summary"])
    return 0


def render_grid(cfg: dict, corpus_dir: Path, *, timbre: str, program: int,
                bank: int = 0, rig: bool | None = None) -> int:
    """Render the model over the capture's own grid, as one more timbre of it.

    The oracle corpus is one WAV per (note, velocity), and the model has to be
    measured the same way before a difference between them means anything.
    Writing it into the same directory under the same names, and registering it
    in the same manifest, means every tool that reads a corpus reads the model
    with no special case: `measure` turns it into a profile the reference can be
    diffed against, and `load_corpus` will serve it as a timbre.

    The grid comes from the capture definition, not from constants matched to it
    by hand. A hand-copied grid drifts the first time a note is added to the
    capture, and what it produces then is a model measured on notes the
    reference does not have — which reads as a reference that is missing rows.
    """
    manifest_path = corpus_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    preroll_s = manifest.get("preroll_ms", cfg["preroll_ms"]) / 1000.0
    gate_s = manifest.get("gate_ms", cfg["gate_ms"]) / 1000.0
    sr = int(manifest.get("sample_rate", cfg["sample_rate"]))
    notes = manifest.get("notes") or cfg["notes"]
    velocities = manifest.get("velocities") or cfg["velocities"]
    # On a kit the channel is not a transport detail: it is what makes the note
    # number select an instrument. Rendering the model's grid on channel 1 would
    # play 47 pitches of whatever program 0 is and write them into the corpus
    # under drum note names.
    channel = PERCUSSION_CHANNEL - 1 if is_percussion(cfg) else 0

    out_dir = corpus_dir / timbre
    out_dir.mkdir(parents=True, exist_ok=True)
    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    # Overriding it renders the same grid at the other boundary, which is how the
    # rig's own transfer function is obtained — the difference of the two, and the
    # only thing that separates the amplifier from the voice behind it.
    if rig is None:
        rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    # The reference's own slot, not this grid's: a model WAV is measured over its
    # whole length, so a slot shorter than the row it will be diffed against
    # reports its buffer where the reference reports the instrument.
    reference_timbre = next((t["id"] for t in cfg.get("timbres", []) if t.get("id")), "")
    window = reference_window(cfg, corpus_dir, reference_timbre,
                              preroll_s=preroll_s, gate_s=gate_s)
    rows = []
    total = len(notes) * len(velocities)
    for i, (note, vel) in enumerate(
        ((n, v) for n in notes for v in velocities), start=1
    ):
        window_s = window(note, vel)
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program, bank=bank,
                        channel=channel, end_pad=max(0.0, window_s - preroll_s - gate_s))
        audio = render_model(smf, window_s, sr, rig=rig)
        rel = Path(timbre) / f"n{note:03d}_v{vel:03d}.wav"
        write_wav(corpus_dir / rel, np.asarray(audio), sr)
        peak = float(np.abs(audio).max())
        rows.append({"id": f"{timbre}/n{note:03d}_v{vel:03d}", "timbre": timbre,
                     "note": note, "velocity": vel, "path": str(rel),
                     "peak": peak, "seconds": round(window_s, 4)})
        print(f"[{i}/{total}] {rel} peak {peak:.4f}", file=sys.stderr)

    header = manifest or {
        "id": cfg["id"], "config": cfg.get("_path", ""), "plugin": "libsonare",
        "sample_rate": sr, "gate_ms": cfg["gate_ms"], "tail": cfg["tail"],
        "preroll_ms": cfg["preroll_ms"], "settle_ms": cfg.get("settle_ms", 0),
        "realtime": False, "params": [], "timbres": [], "notes": notes,
        "velocities": velocities,
    }
    # Replace this timbre's rows rather than appending to them, so re-rendering
    # after a voice edit leaves one grid behind and not two generations of one.
    kept = [r for r in header.get("renders", []) if r.get("timbre") != timbre]
    timbres = [t for t in header.get("timbres", []) if t.get("id") != timbre]
    timbres.append({"id": timbre, "label": f"libsonare, GM program {program}",
                    "model": True})
    header["timbres"] = timbres
    header["renders"] = sorted(kept + rows, key=lambda r: r["id"])
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(header, indent=1) + "\n")
    print(f"\n{len(rows)} renders in {out_dir}, registered as timbre {timbre!r}",
          file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# compare


def dynamics(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int],
             corpus_dir: Path | None = None) -> int:
    """How much the timbre changes from the softest blow to the hardest.

    `compare` scores one velocity at a time, so a model whose every velocity is
    individually plausible can still have the wrong DYNAMICS -- the axis a
    pianist actually plays. This tabulates the pp->ff swing instead of the
    absolute value: level in dB, and the two partial bands relative to the
    fundamental so a swing that is merely "louder" is separated from one that is
    "brighter". A physical model gets this out of its excitation solver rather
    than out of a curve, so a mismatch here is structural: no amount of per-note
    voicing reaches it, and `diagnose.py` is where to take it.
    """
    if not profile_path.exists():
        print(f"no profile at {profile_path} — run `profile.py measure` first")
        return 2
    profile = json.loads(profile_path.read_text())
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0

    program = profile_program(profile, cfg)
    bank = profile_bank(profile, cfg)
    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}")
        return 2
    notes = sorted({n for n, _ in ref if not notes_filter or n in notes_filter})
    vels = sorted({v for _, v in ref})
    if len(vels) < 2:
        print("the profile has a single velocity — nothing to compare across")
        return 2
    lo_v, hi_v = vels[0], vels[-1]

    print(f"model vs {timbre}: velocity {lo_v} -> {hi_v} swing, {len(notes)} notes")
    print("(each column is the change ACROSS velocity, not the absolute value)\n")
    print(f"{'note':>5} | {'level model':>11} {'level ref':>9} | {'h2-7 model':>10} {'h2-7 ref':>8} "
          f"| {'h8-16 model':>11} {'h8-16 ref':>9}")
    print("-" * 82)

    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    window = reference_window(cfg, corpus_dir, timbre,
                              preroll_s=preroll_s, gate_s=gate_s)
    swing: dict[str, list[float]] = {}
    for note in notes:
        got = {}
        for v in (lo_v, hi_v):
            if (note, v) not in ref:
                break
            window_s = window(note, v)
            smf = write_smf([Note(note, v, preroll_s, gate_s)], program=program, bank=bank,
                            end_pad=max(0.0, window_s - preroll_s - gate_s))
            audio = render_model(smf, window_s, cap["sample_rate"], rig=rig)
            m = measure_note(audio, cap["sample_rate"], note, preroll_s=preroll_s, gate_s=gate_s)
            if not m:
                break
            got[v] = (m, ref[(note, v)])
        if len(got) < 2:
            print(f"{note:5d} | not measurable at both velocities")
            continue
        (m_lo, r_lo), (m_hi, r_hi) = got[lo_v], got[hi_v]

        def delta(a, b, key):
            if key not in a or key not in b:
                return None
            return a[key] - b[key]

        def band_delta(a, b, lo, hi):
            x = band_db(a.get("partials_db"), lo, hi)
            y = band_db(b.get("partials_db"), lo, hi)
            return None if x is None or y is None else x - y

        cols = {
            "level_m": delta(m_hi, m_lo, "peak_dbfs"), "level_r": delta(r_hi, r_lo, "peak_dbfs"),
            "low_m": band_delta(m_hi, m_lo, 2, 7), "low_r": band_delta(r_hi, r_lo, 2, 7),
            "high_m": band_delta(m_hi, m_lo, 8, 16), "high_r": band_delta(r_hi, r_lo, 8, 16),
        }
        for k, v in cols.items():
            if v is not None and np.isfinite(v):
                swing.setdefault(k, []).append(float(v))

        def fmt(v, width):
            return format(v, f"+{width}.1f") if v is not None and np.isfinite(v) else "n/a".rjust(width)

        print(f"{note:5d} | {fmt(cols['level_m'], 11)} {fmt(cols['level_r'], 9)} "
              f"| {fmt(cols['low_m'], 10)} {fmt(cols['low_r'], 8)} "
              f"| {fmt(cols['high_m'], 11)} {fmt(cols['high_r'], 9)}")

    print("\nmedian swing (model vs reference, and the error between them):")
    for label, mk, rk in (("level (dB)", "level_m", "level_r"),
                          ("h2-h7 vs h1 (dB)", "low_m", "low_r"),
                          ("h8-h16 vs h1 (dB)", "high_m", "high_r")):
        if mk not in swing or rk not in swing:
            continue
        mm, rr = float(np.median(swing[mk])), float(np.median(swing[rk]))
        # Over how many notes, because these medians are taken over the notes
        # that produced a number and a note that stopped producing one leaves
        # the median instead of moving it. Both sides, since they can differ.
        print(f"  {label:22s} model {mm:+7.2f}   ref {rr:+7.2f}   error {mm - rr:+7.2f}"
              f"   over {len(swing[mk])}/{len(swing[rk])} of {len(notes)} notes")
    return 0


def compare_percussion(cfg: dict, profile: dict, *, timbre: str, notes_filter: set[int],
                       gate_path: str, write_gate: str, margin: float,
                       corpus_dir: Path | None = None) -> int:
    """Score the model against a captured kit, one instrument per note.

    Separate from `compare` rather than a branch inside it, because almost
    nothing carries over: there is no stretch, no inharmonicity, no damper and
    no partial stack, and the columns that replace them are not the same columns
    under different names. What is shared is the shape of the answer — a signed
    and an absolute median per dimension — and the gate that holds it.
    """
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    sr = cap["sample_rate"]
    # The reference's own ceiling, recorded when it was measured. Absent from a
    # profile measured before the edge was, and then the model is measured
    # full-range exactly as that profile was — a comparison is only fair if both
    # sides normalised over the same bands.
    band_edge = cap.get("band_edge_hz")
    mapping = note_map(cfg)

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}; it has "
              f"{sorted({r['timbre'] for r in profile['rows']})}")
        return 2

    program = profile_program(profile, cfg)
    bank = profile_bank(profile, cfg)
    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    print(f"model vs {timbre}: {len(pairs)} hits, model on GM kit {program}, "
          f"MIDI channel {PERCUSSION_CHANNEL}")
    if band_edge:
        print(f"reference bandwidth {band_edge / 1000.0:.1f} kHz — the band columns "
              f"are read over what the capture carries")
    if mapping:
        print("capture note -> model note: "
              + ", ".join(f"{k}->{v}" for k, v in sorted(mapping.items())))
    print()
    # Wired for symmetry rather than because a kit needs it: `Room.gated` refuses
    # a probe whose windows are the gate, which every percussion grid's are, so
    # no captured kit records a room and this is a no-op on all of them today.
    place = ModelPlacer(profile, timbre, [(preroll_s, preroll_s + gate_s)])
    place.announce()
    print(f"{'note':>5} {'vel':>4} | {'tilt Δdb':>9} {'shape db':>9} "
          f"{'decay Δdb/s':>12} {'centroid Δ%':>12} {'attack Δms':>11} "
          f"{'crest Δdb':>10} {'level Δdb':>10} {'ring Δ2x':>9} {'tonal Δdb':>10}")
    print("-" * 110)

    deltas: dict[str, list[float]] = {}
    peaks: dict[str, dict[int, dict[int, float]]] = {}
    silent: list[tuple[int, int]] = []
    # Octave-cells the `band_decay` column was actually read over, against the
    # cells the reference offered it. The dimension averages the octaves both
    # sides resolved, which is a per-row number that cannot say how many rows it
    # came from -- see `band_decay_reach`.
    reach = [0, 0]
    # Kept side by side and index-aligned, for the relations no single row can
    # carry — see `loss.kit_report`. The reference row is stored under the note
    # it was CAPTURED on, since the families are declared in the capture's own
    # numbering, while the model row is whatever the map made it play.
    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    # The captured note, not the one the map makes the model play: the window has
    # to be the one the reference row was measured over, and both the corpus slot
    # and `tail_by_note` are written in the capture's own numbering.
    window = reference_window(cfg, corpus_dir, timbre,
                              preroll_s=preroll_s, gate_s=gate_s)
    kit_rows: list[tuple[dict, dict]] = []
    decay_censored: list[tuple[int, int, bool, bool]] = []
    ring_gone: list[tuple[int, int, str]] = []
    # Rows that reached the per-row arithmetic at all, which is what every
    # dimension below was OFFERED. A dimension's own count cannot be read
    # without it: 11 rows is a full grid on one capture and a third of one here.
    attempted = 0
    offered_keys: set[str] = set()
    for note, vel in pairs:
        played = mapping.get(note, note)
        window_s = window(note, vel)
        smf = write_smf([Note(played, vel, preroll_s, gate_s)], program=program, bank=bank,
                        channel=PERCUSSION_CHANNEL - 1,
                        end_pad=max(0.0, window_s - preroll_s - gate_s))
        audio = render_model(smf, window_s, sr, rig=rig)
        audio = place(audio, sr)
        m = measure_hit(audio, sr, played, vel, preroll_s=preroll_s, gate_s=gate_s,
                        max_band_hz=band_edge)
        r = ref[(note, vel)]
        # A kit piece the model does not voice at all renders silence, and every
        # normalised metric scores silence as a flat spectrum rather than as an
        # absence. Named and left out, in the same way a capped release is.
        if m.get("peak_dbfs") is None or m["peak_dbfs"] <= SILENT_HIT_DBFS:
            silent.append((note, vel))
            print(f"{note:5d} {vel:4d} | model renders nothing above "
                  f"{SILENT_HIT_DBFS:.0f} dBFS")
            continue

        attempted += 1
        row = percussion_row_deltas(m, r)
        # A hit that stopped ringing altogether. `ring_doublings` has no ratio to
        # return for a zero and refuses it, and a refusal reduced with a skip is
        # how a collapsed piece leaves the median rather than failing it — the
        # family's worst member stops being counted and the family reads better.
        collapsed = ring_collapsed(m, r)
        if collapsed:
            ring_gone.append((note, vel, collapsed))
        # A capped reading never fell 20 dB inside the window, so what it records
        # is the window and not the hit — the percussion counterpart of a capped
        # damper release. `ring` already refuses one (`ring_doublings`), and the
        # per-octave rates are slopes fitted over that same truncated span, so
        # they are the second column the cap reaches. Shown and left out.
        decay_capped = bool(m.get("decay_capped") or r.get("decay_capped"))
        uncensored_decay = row["band_decay"]
        if decay_capped:
            decay_censored.append((note, vel, bool(m.get("decay_capped")),
                                   bool(r.get("decay_capped"))))
            row["band_decay"] = None
        if not decay_capped:
            # Counted only where the column it describes counted the row, or it
            # would speak for cells no average was taken over.
            got, offered = band_decay_reach(m, r)
            reach[0] += got
            reach[1] += offered
        offered_keys |= set(row)
        for k, v in row.items():
            if v is not None and np.isfinite(v):
                deltas.setdefault(k, []).append(float(v))
        for side, src in (("m", m), ("r", r)):
            if src.get("peak_dbfs") is not None:
                peaks.setdefault(side, {}).setdefault(note, {})[vel] = src["peak_dbfs"]
        kit_rows.append((m, {**r, "note": note, "velocity": vel}))

        def fmt(v, spec):
            return (format(v, spec) if v is not None and np.isfinite(v)
                    else "n/a".rjust(len(format(0, spec))))

        label = f"{note:5d}" if played == note else f"{note:3d}>{played:<2d}"
        # A censored row still prints its rate, marked, because which side ran
        # out first is the whole content of the finding.
        decay_col = fmt(uncensored_decay, '+11.2f') + ("~" if decay_capped else " ")
        ring_col = fmt(row['ring'], '+8.2f') + ("!" if collapsed else " ")
        print(f"{label} {vel:4d} | {fmt(row['band_tilt'], '+9.1f')} "
              f"{fmt(row['band_shape'], '9.1f')} {decay_col} "
              f"{fmt(row['centroid_pct'], '+12.1f')} {fmt(row['attack'], '+11.1f')} "
              f"{fmt(row['crest'], '+10.1f')} {fmt(row['level'], '+10.1f')} "
              f"{ring_col} {fmt(row['tonality'], '+10.2f')}")

    if ring_gone:
        # Loud and separate from the censors below, because this one is a
        # finding about the voice rather than about the measurement: the piece
        # no longer rings. It is UNSCORABLE rather than a large error, since no
        # ratio of zero exists, and the row it would otherwise vacate is the
        # worst row the median had.
        sides = sorted({s for _n, _v, s in ring_gone})
        print(f"\n! {len(ring_gone)} of {attempted} scored rows report NO ring at all "
              f"({', '.join(sides)} side): `decay_ms` is 0, so the hit is a click and "
              f"`ring` has no ratio to take. Counted here and unscorable, never dropped "
              f"— a collapsed piece leaving the median reads as the family improving:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v, _s in ring_gone))
    if decay_censored:
        # WHICH side hit its ceiling, because that is the finding the censor
        # hides: a row drops out exactly when one hit was still loud where the
        # other had already gone, which is the same thing as saying it rings
        # much longer. Left unnamed it reads as missing data.
        model_only = sum(1 for _n, _v, mc, rc in decay_censored if mc and not rc)
        ref_only = sum(1 for _n, _v, mc, rc in decay_censored if rc and not mc)
        both = len(decay_censored) - model_only - ref_only
        parts = [f"{model_only} on the model side" if model_only else "",
                 f"{ref_only} on the reference side" if ref_only else "",
                 f"{both} on both" if both else ""]
        print(f"\n~ {len(decay_censored)} of {len(pairs)} rows never fell 20 dB inside "
              f"the window the reference was recorded over, so their decay covers a "
              f"different part of the fall on the two sides; shown, not counted, and "
              f"{', '.join(p for p in parts if p)}:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v, _m, _r in decay_censored))
    if silent:
        print(f"\n* {len(silent)} of {len(pairs)} hits are silent on the model side; "
              f"shown, not counted:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v in silent))
    for note, model_peaks in sorted(peaks.get("m", {}).items()):
        ref_peaks = peaks.get("r", {}).get(note, {})
        shared = sorted(set(model_peaks) & set(ref_peaks))
        if len(shared) < 2:
            continue
        span = ([model_peaks[v] for v in shared], [ref_peaks[v] for v in shared])
        deltas.setdefault("vel_range", []).append(
            (max(span[0]) - min(span[0])) - (max(span[1]) - min(span[1]))
        )
    summary = select_dimensions(summarize_deltas(deltas), cfg.get("dimensions") or [],
                                cfg.get("dimensions_na") or {})
    spread = percussion_reference_spread(profile, list(summary))
    print_summary_table(summary, spread, attempted)
    print_vanished_dimensions(offered_keys, summary, cfg.get("dimensions") or [],
                              cfg.get("dimensions_na") or {})
    print("\n  A kit has no register to average along: every row is a different "
          "instrument,\n  so the signed median says only how the kit leans as a whole and the "
          "absolute\n  one is the column to read. `band shape` is a magnitude already and its "
          "two\n  columns are the same number by construction.")
    print("\n  `p90` is the 90th percentile of the same absolute error, and it is the column "
          "that\n  sees a handful of bad instruments: a kit can sit inside its spread on both "
          "median\n  columns while nearly half its rows are outside it. Read `p90` against "
          "`spread`\n  the same way, and where the two medians and `p90` disagree, the "
          "disagreement is\n  the finding — a few instruments are wrong rather than the kit.")
    if spread:
        print("\n  `spread` is how far the REFERENCE KITS sit from each other on the same "
              "dimension,\n  by the same arithmetic, so `x spread` reads the same in dB, "
              "milliseconds and\n  percent: 1.0x is as close to them as they are to one "
              "another, which is as close\n  as this corpus can define. A gate cannot say this "
              "— its bounds come from what\n  the voice measured the day they were written. "
              "The largest ratio is where the\n  next round belongs.")
    else:
        print("\n  No `spread`: this capture has one reference kit, so no dimension can be "
              "judged\n  against anything but itself. A second kit is what makes the column "
              "exist.")

    # An attack at the envelope window's floor is the ruler rather than the hit,
    # and the `attack` column above cannot say so: it is a difference, and a
    # difference between a floored reading and a real one looks like any other.
    # Derived from the recorded number rather than read from `attack_floored`,
    # so a profile measured before that field existed reports the same.
    floored = {side: sum(1 for m, r in kit_rows
                         if (m if side == "model" else r).get("attack_ms", 1e9)
                         <= ATTACK_FLOOR_MS)
               for side in ("model", "reference")}
    if kit_rows and floored["model"]:
        total = len(kit_rows)
        print(f"\n  {floored['model']} of {total} model rows and {floored['reference']} of "
              f"{total} reference rows sit at\n  the attack floor "
              f"({ATTACK_FLOOR_MS:g} ms), where a step, an impulse and a "
              f"{ATTACK_FLOOR_MS:g} ms ramp all\n  measure the same. Those rows say "
              f"the attack is no slower than that and nothing\n  more, so an `attack` delta "
              f"taken against a reference above the floor is a LOWER\n  bound on the gap. It "
              f"understates and never invents.")

    if reach[1]:
        print(f"\n  per-octave decay read over {reach[0]} of the {reach[1]} octave-cells "
              f"the reference\n  resolved a rate in ({100.0 * reach[0] / reach[1]:.0f} %). "
              f"The rest are octaves the model gave no\n  measurable rate, and they leave "
              f"that row's average rather than being charged\n  to it — so a low figure "
              f"here says the column is speaking for less of the kit\n  than its row count "
              f"suggests. Two reference kits reach 94 and 87 % of each other.")

    print_kit_relations(kit_rows, note_groups(cfg))

    if gate_path:
        return check_gate(summary, Path(gate_path), timbre, profile.get("measured_utc", ""))
    if write_gate:
        return write_gate_file(summary, Path(write_gate), timbre, margin,
                               profile.get("measured_utc", ""), spread)
    return 0


def takes(cfg: dict, *, archive: Path, only: set[str], program: int,
          bank: int = 0) -> int:
    """Measure the phrase takes, which is where the couplings live.

    A per-note grid answers what one string does. Everything an instrument does
    because two of its parts are connected -- a pedalled wash accumulating, a
    hi-hat landing on its own ring, a chord's ranks beating -- happens only
    between notes, and a corpus of single notes cannot excite it at all. That is
    why constants on those paths sit unexamined however many fits have run: the
    search never had a measurement of them to minimise.
    """
    # The capture's rate, not a constant of this file: the archive was written
    # at whatever the capture renders at, and a second copy of that number here
    # is a mirror that only stays equal until one of them is changed.
    sr = int(cfg.get("sample_rate", 48000))
    set_name = cfg.get("takes")
    if set_name not in TAKE_SETS:
        named = f"{set_name!r}" if set_name else "no phrase set"
        print(f"the capture names {named}; have {', '.join(TAKE_SETS)}", file=sys.stderr)
        return 2
    selected = [t for t in build_takes(set_name, program) if not only or t.id in only]
    # The capture says whether its reference carries a room; nothing here can
    # tell one from an instrument's own long release, and guessing the wrong way
    # invents a building and convolves it onto every figure below. `room` is the
    # field that answers it. `dry` stands in where nobody has: it is an
    # instruction to the host rather than a reading of the recording, so it says
    # wet for a rack that merely advertises no effect section to switch off.
    room_answer = str(cfg.get("room", ROOM_UNCLASSIFIED))
    wet = False if room_answer == ROOM_NONE else not cfg.get("dry", True)
    # And whether it carries a rig, which is a different question a dryness test
    # cannot answer: a cabinet is a filter rather than a space.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    # One IR per reference for the whole run: the room is a property of the
    # session that recorded it, and the first take able to measure one hands it
    # to every take that cannot.
    room_irs: dict[str, tuple] = {}
    print(f"phrase takes, model on GM program {program}, against the archived references.")
    print("* marks a model value outside the range those references themselves span.\n")
    measured = 0
    for take in selected:
        refs = archived_take_references(archive, cfg["id"], take.id, sr)
        if not refs:
            print(f"  {take.id}: no archived reference — build a page for it first "
                  f"(make_audition.py --archive-references)", file=sys.stderr)
            continue
        windows = take_windows(take)
        spans = [(n.start, n.start + n.dur) for n in take.notes]
        smf = write_smf(take.notes, program=program, bank=bank, end_pad=take.tail_s,
                        cc_events=take.cc_events, channel=take.channel)
        # The tail window ends where the references do, not where the phrase
        # nominally does. Clipped for both sides at once, before either is
        # measured, so the model is never read over a span the instrument it is
        # being compared against had already left.
        nominal = windows.get("tail")
        if nominal:
            clipped = usable_tail([to_mono(np.asarray(a, dtype=np.float64))
                                   for a in refs.values()], sr, nominal)
            windows["tail"] = clipped
        dry_model = render_model(smf, take.duration(), sr, rig=rig)
        # The model renders dry — `write_smf` writes CC91 0 — so on a capture
        # whose reference carries its building, every tail figure below would be
        # a dry signal read against a wet one, and each of them would be outside
        # the references' spread for that reason alone. Measured on the church
        # organ: the tail fell at 78 dB/s against their 15 to 17, and its four
        # band levels missed by 11 to 25 dB. One room per reference, because two
        # references are two buildings and placing the model in one of them
        # would score it against the other's.
        model_rows, unplaced = [], []
        if not wet:
            model_rows.append(measure_take(dry_model, sr, windows))
        for name, audio in (refs.items() if wet else ()):
            if name not in room_irs:
                room = measurable_room(audio, sr, spans)
                if room is not None:
                    room_irs[name] = (room, place_model_in(dry_model, sr, spans, room)[1])
            if name not in room_irs:
                unplaced.append(name)
                continue
            room, ir = room_irs[name]
            model_rows.append(
                measure_take(place_model_in(dry_model, sr, spans, room, ir)[0], sr, windows))
        model = model_rows[0] if model_rows else {}
        ref_rows = [measure_take(a, sr, windows) for a in refs.values()]
        if wet and not model_rows:
            # Every figure below is a tail figure or is read across one, so with
            # no room to put the model in there is nothing here to report rather
            # than a set of numbers that all say the same thing about the
            # building. A phrase of short notes cannot measure a room itself
            # (`Room.gated`); one long-note take in the same set gives every
            # other take its IR, so this is a set without one.
            print(f"  {take.id}: skipped — the reference carries a room and no take in this "
                  f"run measured one to put the model in (run without --only, or add a "
                  f"phrase that holds a note)", file=sys.stderr)
            continue
        if not model or not any(ref_rows):
            # Almost always a phrase with no tail to measure rather than a
            # broken render: a take whose pedal lifts on the last note-off, or
            # whose notes run to the end, leaves no window in which nothing is
            # being played. Named rather than counted, because the same line
            # would otherwise cover a silent model.
            reason = ("no window between the last note and the end of the phrase"
                      if windows.get("tail") is None
                      else "the render carried no measurable tail")
            print(f"  {take.id}: skipped — {reason}", file=sys.stderr)
            continue
        measured += 1
        print(f"  {take.id} — {take.label}")
        clipped = windows.get("tail")
        if nominal and clipped is None:
            print("    (no tail reading: the references stop within a fraction of a second "
                  "of the last note, so there is no span in which they are still sounding)")
        elif nominal and clipped[1] < nominal[1] - 0.05:
            print(f"    (tail read {clipped[0]:.1f}-{clipped[1]:.1f} s: the references run "
                  f"out {nominal[1] - clipped[1]:.1f} s before the phrase does)")
        if wet:
            placed = ", ".join(f"RT60 {room_irs[n][0].rt60_s:.1f}s"
                               for n in refs if n in room_irs)
            missing = (f"; {len(unplaced)} reference(s) left out, no room measured"
                       if unplaced else "")
            print(f"    (model placed in {placed}{missing})")
        for key, label in TAKE_LABELS.items():
            vals = [r[key] for r in ref_rows if key in r]
            got = [r[key] for r in model_rows if key in r]
            if not got or not vals:
                continue
            lo, hi = min(vals), max(vals)
            # Outside their range in EVERY room it was placed in, or it is not
            # the model that is outside — it is which building it was read in.
            flag = "" if any(lo <= g <= hi for g in got) else "*"
            shown = (f"{got[0]:>9.1f}" if max(got) - min(got) < 0.05
                     else f"{min(got):>9.1f}..{max(got):<.1f}")
            print(f"    {label:<58}{lo:>8.1f}..{hi:<8.1f}{shown}{flag}")
        print()
    if not measured:
        print("nothing measured — see the reason printed against each take above",
              file=sys.stderr)
        return 2
    return 0


class ModelPlacer:
    """Puts every model render in the space the reference was recorded in.

    A dry model scored against a reference carrying its building reads short in
    decay, fast in release and thin under the note, and all three are the room.
    The fitter has always corrected for this on the same corpus (`autofit
    --room auto`); this is the comparator's half of it, off the room the profile
    recorded, so the two halves judge one signal.

    One impulse response for the whole grid, fitted on the first render: the
    space belongs to the recording session rather than to a note, and a fit per
    note would make each row's correction depend on that row's own decay.
    """

    def __init__(self, profile: dict, timbre: str, spans: list[tuple[float, float]]):
        recorded = (profile.get("rooms") or {}).get(timbre)
        self.room = (Room(**{k: v for k, v in recorded.items() if k in ROOM_FIELDS})
                     if recorded else None)
        self.spans = spans
        self.ir: np.ndarray | None = None

    def announce(self) -> None:
        if self.room is None:
            return
        print(f"the reference carries a room — RT60 {self.room.rt60_s:.2f}s, tail level "
              f"{self.room.tail_db:+.1f}dB, HF ratio {self.room.hf_ratio:.2f} — and the "
              f"model is placed in a matching space before every measurement below\n")

    def __call__(self, audio: np.ndarray, sr: int) -> np.ndarray:
        if self.room is None:
            return audio
        placed, self.ir = place_model_in(np.asarray(audio, dtype=np.float64), sr,
                                         self.spans, self.room, self.ir)
        return placed.astype(np.float32)


def compare(cfg: dict, profile_path: Path, *, timbre: str, notes_filter: set[int],
            gate_path: str = "", write_gate: str = "", margin: float = 1.25,
            corpus_dir: Path | None = None) -> int:
    """Measure libsonare the same way and print the difference, dimension by dimension."""
    if not profile_path.exists():
        print(f"no profile at {profile_path} — run `profile.py measure` first")
        return 2
    profile = json.loads(profile_path.read_text())
    if is_percussion(profile["capture"]):
        return compare_percussion(cfg, profile, timbre=timbre, notes_filter=notes_filter,
                                  gate_path=gate_path, write_gate=write_gate, margin=margin,
                                  corpus_dir=corpus_dir)
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}; it has "
              f"{sorted({r['timbre'] for r in profile['rows']})}")
        return 2

    program = profile_program(profile, cfg)
    bank = profile_bank(profile, cfg)
    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    a4_off = a4_offset_cents(profile["rows"], timbre)
    print(f"model vs {timbre}: {len(pairs)} notes, model on GM program {program}")
    print(f"reference A4 sits {a4_off:+.2f} c from 440 Hz; "
          f"that offset is removed from the stretch column\n")
    place = ModelPlacer(profile, timbre, [(preroll_s, preroll_s + gate_s)])
    place.announce()
    # Every dimension the summary gates has a column here, so a bound that moves
    # can be traced to the notes that moved it. The aftersound is the one that
    # most needs it: it is the slowest thing the voice does, so it is the least
    # uniform across the keyboard, and a median hides a register that is wrong in
    # the opposite direction from the rest.
    header = (f"{'note':>5} {'vel':>4} | {'stretch Δc':>10} {'B model':>10} {'B ref':>10} "
              f"{'decay Δdb/s':>12} {'after Δdb/s':>12} {'h2-6 model':>10} {'h2-6 ref':>9} "
              f"{'centroid Δ%':>12} {'TNR Δdb':>9} {'damper Δms':>11}"
              f"{'attack Δms':>11} {'width Δ':>8}")
    print(header)
    print("-" * len(header))

    deltas: dict[str, list[float]] = {}
    damper_censored: list[tuple[int, int]] = []
    span_censored: list[tuple[int, int]] = []
    # Rows that reached the per-row arithmetic at all, which is what every
    # dimension below was OFFERED — see `print_summary_table`.
    attempted = 0
    offered_keys: set[str] = set()
    # Peak level per side, per note, per velocity. The dynamic range is the one
    # dimension no single (note, velocity) row can carry: it is the difference
    # between two of them, so it is accumulated here and reduced after the loop.
    peaks: dict[str, dict[int, dict[int, float]]] = {}
    # The loudest the note's body gets, per side, per note, per velocity, for
    # the register profile. Not the strike peak `vel_range` reads -- that is the
    # hammer -- and not the gate-wide RMS, which counts a long note as a loud
    # one and would report this voice's oversustained treble as a level.
    # Which side of the instrument's boundary the model stops at, from the
    # capture's own answer: a direct reference is compared against the direct
    # signal, one recorded through an amplifier against the model plus its rig.
    rig = model_rig(str(cfg.get("rig", RIG_UNCLASSIFIED)))
    window = reference_window(cfg, corpus_dir, timbre,
                              preroll_s=preroll_s, gate_s=gate_s)
    levels: dict[str, dict[int, dict[int, float]]] = {}
    for note, vel in pairs:
        window_s = window(note, vel)
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program, bank=bank,
                        end_pad=max(0.0, window_s - preroll_s - gate_s))
        audio = render_model(smf, window_s, cap["sample_rate"], rig=rig)
        audio = place(audio, cap["sample_rate"])
        m = measure_note(audio, cap["sample_rate"], note, preroll_s=preroll_s, gate_s=gate_s)
        r = ref[(note, vel)]
        if not m:
            print(f"{note:5d} {vel:4d} | model rendered nothing measurable")
            continue
        attempted += 1

        def d(key, scale=1.0, m=m, r=r):
            if key not in m or key not in r:
                return None
            return (m[key] - r[key]) * scale

        stretch = d("cents_vs_et")
        # A capped release never reached -40 dB inside the capture's tail, so
        # the number recorded for it is the length of the window and not a
        # measurement. Differencing two of those, or one against a real one,
        # yields a delta that moves when the tail length changes and never when
        # the voice does; the row is shown and left out of the median instead.
        damper_capped = bool(m.get("damper_capped") or r.get("damper_capped"))
        if damper_capped:
            damper_censored.append((note, vel))
        # A decay rate is a slope, and two slopes only difference into a
        # comparison when they were fitted over comparable spans. The span is
        # each side's own -- it ends where that render stopped being audible --
        # so a note the model sustains twice as long as the reference produces
        # two numbers about two different parts of a two-stage decay. Shown and
        # left out, the same as a capped damper.
        gap_m, gap_r = double_decay_gap(m), double_decay_gap(r)
        m_span, r_span = m.get("decay_span_s", 0.0), r.get("decay_span_s", 0.0)
        span_mismatch = (min(m_span, r_span) < DECAY_SPAN_AGREEMENT * max(m_span, r_span, 1e-9))
        if span_mismatch:
            span_censored.append((note, vel, m_span, r_span))
        row = {
            "stretch": None if stretch is None else stretch + a4_off,
            "decay": None if span_mismatch else d("decay_db_s"),
            # Positive means the model is the cleaner of the two, which is the
            # direction this almost always fails in: the mechanism noise a real
            # action makes is the part a physical model most often has no
            # mechanism for at all.
            "tnr": d("tnr_db"),
            "damper": None if damper_capped else d("damper_release_ms"),
            # The rise from the sound's own onset to its loudest point -- the
            # bloom, not the strike. A model whose envelope peaks on the strike
            # and falls from there reads as a thump rather than as a note that
            # sinks in. How late either side sounded is `onset_ms` and is not
            # in here: that is the capture chain and the plugin, not the voice.
            "attack": d("attack_ms"),
            # The SECOND decay rate. A piano string loses energy fast while the
            # strings of its unison move together and far more slowly once they
            # have drifted apart and are trading it through the bridge rather
            # than radiating it. That second rate is what a pedalled note is
            # still doing seconds later -- it is the afterglow -- and `decay`
            # above cannot see it: one straight line through both regimes lands
            # between them, so a voice with no aftersound at all can hold that
            # line. Measured against three concert grands, this voice sat inside
            # the decay bound while its late rate at C5 was three times theirs.
            "aftersound": None if span_mismatch else d("decay_late_db_s"),
            "doubling": None if span_mismatch or gap_m is None or gap_r is None
            else gap_m - gap_r,
            "body": d("body_below_f0_db"),
            # The two radiation paths of a real board arrive decorrelated. A
            # model that folds one signal into both legs scores exactly 0.0
            # here and is inaudibly mono however wide the reverb around it is,
            # which no mono-summed measurement in this file can see.
            "stereo": d("stereo_width"),
        }
        centroid_pct = None
        if "centroid_hz" in m and r.get("centroid_hz"):
            centroid_pct = 100.0 * (m["centroid_hz"] / r["centroid_hz"] - 1.0)
        bal_m = partial_balance_db(m.get("partials_db"))
        bal_r = partial_balance_db(r.get("partials_db"))
        row["balance"] = None if bal_m is None or bal_r is None else bal_m - bal_r
        scored = list(row.items()) + [("centroid_pct", centroid_pct)]
        offered_keys |= {k for k, _v in scored}
        for k, v in scored:
            if v is not None and np.isfinite(v):
                deltas.setdefault(k, []).append(float(v))
        for side, src in (("m", m), ("r", r)):
            if "peak_dbfs" in src:
                peaks.setdefault(side, {}).setdefault(note, {})[vel] = src["peak_dbfs"]
            if src.get("held_peak_dbfs") is not None and np.isfinite(src["held_peak_dbfs"]):
                levels.setdefault(side, {}).setdefault(note, {})[vel] = src["held_peak_dbfs"]

        def fmt(v, spec):
            return format(v, spec) if v is not None and np.isfinite(v) else "n/a".rjust(len(format(0, spec)))

        damper_col = fmt(d("damper_release_ms"), '+10.1f') + ("*" if damper_capped else " ")
        # A censored pair still prints its two rates, marked, because seeing
        # which side ran out first is the whole content of the finding.
        decay_col = (fmt(d("decay_db_s"), '+11.2f') + ("~" if span_mismatch else " "))
        after_col = (fmt(d("decay_late_db_s"), '+11.2f') + ("~" if span_mismatch else " "))
        print(f"{note:5d} {vel:4d} | {fmt(row['stretch'], '+10.1f')} "
              f"{m.get('inharmonicity_b', float('nan')):10.3e} {r.get('inharmonicity_b', float('nan')):10.3e} "
              f"{decay_col} {after_col} "
              f"{fmt(bal_m, '+10.1f')} {fmt(bal_r, '+9.1f')} "
              f"{fmt(centroid_pct, '+12.1f')} {fmt(row['tnr'], '+9.1f')} {damper_col}"
              f"{fmt(row['attack'], '+11.1f')} {fmt(row['stereo'], '+8.3f')}")

    if damper_censored:
        # Each censored row was rendered over its own slot's tail, so the
        # sentence names the span rather than one number it no longer has.
        tails = sorted({round(window(n, v) - preroll_s - gate_s, 2)
                        for n, v in damper_censored})
        span = (f"{tails[0]:.2g} s" if len(tails) == 1
                else f"{tails[0]:.2g}-{tails[-1]:.2g} s")
        print(f"\n* {len(damper_censored)} of {len(pairs)} rows never fell 40 dB inside the "
              f"{span} tail on one side or the other; shown, not counted:")
        print("  " + ", ".join(f"n{n}v{v}" for n, v in damper_censored))
    if span_censored:
        # WHICH side outlasted the other, because that is the finding and the
        # censor is what hides it: a row is dropped exactly when the two decays
        # are too different to difference, which is the same thing as saying the
        # difference is large. Left unnamed it reads as missing data.
        longer = sum(1 for _n, _v, ms, rs in span_censored if ms > rs)
        side = ("the model outlasted the reference on all of them" if longer == len(span_censored)
                else "the reference outlasted the model on all of them" if longer == 0
                else f"the model outlasted the reference on {longer} of them")
        print(f"\n~ {len(span_censored)} of {len(pairs)} rows had one side stay audible "
              f"well past the other, so their decay rates cover different parts of the "
              f"decay; shown, not counted, and {side}:")
        print("  " + ", ".join(f"n{n}v{v} ({ms:.1f}s vs {rs:.1f}s)"
                               for n, v, ms, rs in span_censored))
    for note, model_peaks in sorted(peaks.get("m", {}).items()):
        ref_peaks = peaks.get("r", {}).get(note, {})
        shared = sorted(set(model_peaks) & set(ref_peaks))
        if len(shared) < 2:
            continue
        span = ([model_peaks[v] for v in shared], [ref_peaks[v] for v in shared])
        deltas.setdefault("vel_range", []).append(
            (max(span[0]) - min(span[0])) - (max(span[1]) - min(span[1]))
        )
    register = register_deltas(levels.get("m", {}), levels.get("r", {}))
    if register:
        deltas["register"] = [d for _, _, d in register]
        print_register_profile(register, register_spread_by_note(profile))
    summary = select_dimensions(summarize_deltas(deltas), cfg.get("dimensions") or [],
                                cfg.get("dimensions_na") or {})
    spread = reference_spread(profile, list(summary))
    print_summary_table(summary, spread, attempted)
    print_vanished_dimensions(offered_keys, summary, cfg.get("dimensions") or [],
                              cfg.get("dimensions_na") or {})
    print("\n  The signed median is what the voice is doing on average and the absolute one "
          "is\n  how far any given note is from the reference. They part company exactly where "
          "a\n  summary is least trustworthy: errors of opposite sign in different registers "
          "cancel\n  in the first column and do not in the second. `p90` is the 90th "
          "percentile of\n  that absolute error, and it parts company with both wherever the "
          "defect is a few\n  notes rather than the register: a minority of bad rows cannot "
          "move a median.")
    print("\n  `spread` is how far the REFERENCES sit from each other on the same dimension, "
          "by\n  the same arithmetic, so `x spread` reads the same in cents, dB, milliseconds "
          "and\n  percent: 1.0x is as close to them as they are to one another, which is as "
          "close\n  as this corpus can define. A gate cannot say this — its bounds come from "
          "what\n  the voice measured the day they were written, so green means "
          "\"no worse than\n  then\", never \"finished\". The largest ratio is where the "
          "next round belongs.")

    if gate_path:
        return check_gate(summary, Path(gate_path), timbre, profile.get("measured_utc", ""))
    if write_gate:
        return write_gate_file(summary, Path(write_gate), timbre, margin,
                               profile.get("measured_utc", ""), spread)
    return 0


# --------------------------------------------------------------------------
# Two references, and where they agree


# How far apart two independent references may sit on a dimension and still be
# said to agree about it, in that dimension's own units.
#
# These are not tolerances on the model. They are the width of "what this
# instrument is", measured the only way it can be: by asking twice. Two
# libraries voicing the same GM instrument differ by their voicing decisions —
# a brighter ride, a longer crash — and inside that width there is no target to
# hit, so a model sitting anywhere in it is right and a fit that moves it is
# chasing whichever reference it was handed.
#
# Set at roughly the difference a listener would call a different choice rather
# than a different instrument. Level is the widest and the most important: a
# 6 dB disagreement about how loud a tambourine is says the kit balance is a mix
# decision, and a fit given one reference will happily take it 31 dB down.
#
# `band_decay` and `ring` are the two a percussion row carries and a melodic one
# does not; both come back `None` where the fields are absent, which prints as
# `n/a` and counts nothing. Their widths are the two references' own measured
# disagreement over the kit grid, recorded in `capture/drums.json`. They are wide
# — 0.69 doublings is a factor of 1.6 — and a wide bound is the point: it catches
# a hit that is several times wrong and passes anything a second real kit could
# have been. Without them nothing in the tree could fail a ring regression on any
# kit note at all, which is how an engine change that moved every sub-unity mode's
# damping reached the bank with no instrument able to see it.
AGREEMENT_TOLERANCE = {
    "level": 6.0,
    "band_tilt": 6.0,
    "band_shape": 6.0,
    "centroid_pct": 50.0,
    "attack": 5.0,
    "crest": 3.0,
    "band_decay": 41.7,
    "ring": 0.69,
}


def _delta(a: float | None, b: float | None) -> float | None:
    """`a - b`, or None where either side did not measure."""
    return None if a is None or b is None else float(a) - float(b)


def agreement_row(second: dict, r: dict) -> dict:
    """One grid cell's second-reference-minus-reference delta, per dimension.

    Its keys are exactly `AGREEMENT_TOLERANCE`'s, and `agree` indexes it with
    `row[key]` rather than `.get` deliberately: a tolerance with no delta behind
    it would otherwise read as a dimension the two references never disagreed
    on, which is the empty set scoring as perfect agreement.
    """
    tilt_a, tilt_b = band_tilt_db(r.get("bands_db")), band_tilt_db(second.get("bands_db"))
    return {
        "level": (second["peak_dbfs"] - r["peak_dbfs"]
                  if r.get("peak_dbfs") is not None else None),
        "band_tilt": None if tilt_a is None or tilt_b is None else tilt_b - tilt_a,
        "band_shape": band_shape_error_db(second.get("bands_db"), r.get("bands_db")),
        "centroid_pct": (100.0 * (second["centroid_hz"] / r["centroid_hz"] - 1.0)
                         if r.get("centroid_hz") else None),
        "attack": second["attack_ms"] - r["attack_ms"],
        "crest": second["crest_db"] - r["crest_db"],
        "band_decay": _delta(second.get("band_decay_db_s"), r.get("band_decay_db_s")),
        # A capped decay is the analysis window, not the hit, so a ratio taken
        # against one compares a window with an instrument.
        "ring": (None if second.get("decay_capped") or r.get("decay_capped")
                 else _ratio_doublings(second.get("decay_ms"), r.get("decay_ms"))),
    }


def _ratio_doublings(a: float | None, b: float | None) -> float | None:
    """How many doublings `a` is above `b`, or None where either is unusable.

    A length is compared as a ratio rather than a difference because the kit
    spans 24x on it — 60 ms of woodblock against 1428 of cymbal — so a bound in
    milliseconds would be the cymbals and nothing else.
    """
    if a is None or b is None or a <= 0.0 or b <= 0.0:
        return None
    return float(np.log2(float(a) / float(b)))


def agree(cfg: dict, profile: dict, *, timbre: str, notes_filter: set[int],
          corpus_dir: Path | None = None) -> int:
    """Measure a second reference over the same grid and report where the two agree.

    Every bound in `reference/*_gate.json` was decided by hand from one
    reference, and one reference cannot say which of its numbers are the
    instrument and which are its own voicing. That is not a documentation gap:
    working from a single reference, a tambourine was on its way to being pushed
    31 dB down — inaudible — because that is where the one reference put it.

    The second opinion is fluidsynth over the bundled SoundFont, which is
    already wired for the fit's oracle. It is not a better reference than the
    captured one and is not used as a target; it is an independent one, and the
    only question asked of it is where it and the capture say the same thing.

    Nothing is written. The output is the map a gate should be recorded from:
    a dimension the two agree on is worth bounding, and one they do not is worth
    measuring and leaving alone.
    """
    cap = profile["capture"]
    preroll_s = cap["preroll_ms"] / 1000.0
    gate_s = cap["gate_ms"] / 1000.0
    sr = cap["sample_rate"]
    band_edge = cap.get("band_edge_hz")
    program = profile_program(profile, cfg)
    bank = profile_bank(profile, cfg)
    if not is_percussion(cap):
        print("`agree` measures the percussion metric set; the pitched one has no "
              "second reference wired to it yet", file=sys.stderr)
        return 2

    ref = {(r["note"], r["velocity"]): r for r in profile["rows"] if r["timbre"] == timbre}
    if not ref:
        print(f"profile has no timbre {timbre!r}", file=sys.stderr)
        return 2
    pairs = sorted({k for k in ref if not notes_filter or k[0] in notes_filter})
    print(f"capture {timbre!r} vs fluidsynth, {len(pairs)} hits, GM kit {program}\n")
    print(f"{'note':>5} {'vel':>4} | " + " ".join(f"{k:>12}" for k in AGREEMENT_TOLERANCE))
    print("-" * (12 + 13 * len(AGREEMENT_TOLERANCE)))

    agreed: dict[str, int] = {k: 0 for k in AGREEMENT_TOLERANCE}
    counted: dict[str, int] = {k: 0 for k in AGREEMENT_TOLERANCE}
    deltas: dict[str, list[float]] = {}
    window = reference_window(cfg, corpus_dir, timbre,
                              preroll_s=preroll_s, gate_s=gate_s)
    for note, vel in pairs:
        window_s = window(note, vel)
        smf = write_smf([Note(note, vel, preroll_s, gate_s)], program=program, bank=bank,
                        channel=PERCUSSION_CHANNEL - 1,
                        end_pad=max(0.0, window_s - preroll_s - gate_s))
        try:
            audio = render_oracle_fluidsynth(smf, window_s, sr)
        except (FileNotFoundError, RuntimeError) as exc:
            print(f"second reference unavailable: {exc}", file=sys.stderr)
            return 2
        second = measure_hit(audio, sr, note, vel, preroll_s=preroll_s, gate_s=gate_s,
                             max_band_hz=band_edge)
        r = ref[(note, vel)]
        if second.get("peak_dbfs") is None or second["peak_dbfs"] <= SILENT_HIT_DBFS:
            print(f"{note:5d} {vel:4d} | the second reference does not voice this note")
            continue

        row = agreement_row(second, r)
        cells = []
        for key, tol in AGREEMENT_TOLERANCE.items():
            v = row[key]
            if v is None or not np.isfinite(v):
                cells.append(f"{'n/a':>12}")
                continue
            counted[key] += 1
            deltas.setdefault(key, []).append(float(v))
            ok = abs(v) <= tol
            agreed[key] += 1 if ok else 0
            cells.append(f"{v:>11.1f}{'' if ok else '!'}")
        print(f"{note:5d} {vel:4d} | " + " ".join(cells))

    print(f"\n{'':16s} {'agree':>8} {'of':>5} {'median Δ':>10} {'tolerance':>10}")
    for key, tol in AGREEMENT_TOLERANCE.items():
        n = counted[key]
        med = float(np.median(deltas[key])) if deltas.get(key) else float("nan")
        print(f"  {key:14s} {agreed[key]:8d} {n:5d} {med:10.1f} {tol:10.1f}")
    print("\n  A dimension the two references agree on has a target in it and is worth "
          "gating.\n  One they do not is a voicing decision: measure it, print it, and do "
          "not fit to it —\n  the number a single reference gives there is that library's "
          "opinion, and a fit\n  handed it will follow it as far as it goes.")
    return 0


# --------------------------------------------------------------------------
# rig


def rig_evidence(cfg: dict, corpus_dir: Path, *, against: list[str], timbre: str) -> int:
    """Put the `rig` question to the reference itself, and say what the answer can be.

    The comparison captures are the argument: a cabinet is one filter after
    several instruments, so what makes a dark spectrum into evidence is that
    siblings share its shape while everything not behind it does not. Siblings
    from the same rack are worth more than any absolute threshold, since they
    came through the same recording chain in the same session.
    """
    from corpus import load_corpus
    from rig import curve_distance, measure_rotary, measure_skirt

    subject = load_corpus(corpus_dir, timbre or cfg["timbres"][0]["id"])
    # Keyed by timbre as well as capture, so naming the subject's own capture as a
    # sibling compares the model's grid against the reference it was rendered
    # beside rather than overwriting the row it was going to be compared with.
    subject_key = f"{cfg['id']}:{subject.timbre}"
    skirts = {subject_key: measure_skirt(subject)}
    for name in against:
        other_cfg = load_config(CAPTURE_DIR / f"{name}.json")
        other = load_corpus(out_root(other_cfg, ""))
        skirts[f"{name}:{other.timbre}"] = measure_skirt(other)

    first = skirts[subject_key]
    centres = [f for f in first.centres if 2000.0 <= f <= 16000.0]
    print(f"\nthird-octave level re 1-2 kHz, dB — {subject_key} against "
          f"{len(against)} sibling(s)")
    print(f"{'':24}" + "".join(f"{f:8.0f}" for f in centres))
    for name, skirt in skirts.items():
        row = [v for f, v in zip(skirt.centres, skirt.curve) if 2000.0 <= f <= 16000.0]
        print(f"{name[:24]:24}" + "".join(f"{v:8.1f}" for v in row))

    print(f"\n{'':24}{'skirt':>12}{'knee':>10}{'note spread':>14}{'vs subject':>12}")
    for name, skirt in skirts.items():
        distance = "" if name == subject_key else f"{curve_distance(first, skirt):12.2f}"
        print(f"{name[:24]:24}{skirt.slope_db_per_octave:9.1f}/oct{skirt.knee_hz:9.0f}Hz"
              f"{skirt.slope_spread:13.1f} {distance}")

    rotary = measure_rotary(subject)
    if rotary.stereo:
        print(f"\nmodulation {rotary.rate_hz:.2f} Hz at {rotary.depth_db:.2f} dB, "
              f"inter-channel correlation {rotary.interchannel_correlation:+.3f} "
              f"(channels {rotary.channel_separation_db:.1f} dB apart)")
        print("  A rotor turns away from one mic as it faces the other, so it drives the\n"
              "  correlation negative. In-phase modulation is the instrument beating "
              "against itself.")
    else:
        print(f"\nrendered dual mono ({rotary.channel_separation_db:.1f} dB apart): the "
              f"rotary test has\n  nothing to read here, which is not the same answer as "
              f"a rotor being absent.")

    print(f"\n  `rig` is currently {subject.rig!r}.")
    print("  This measurement can show a rig present and cannot show one absent: a dull\n"
          "  instrument recorded flat and a bright one recorded through a cabinet both come\n"
          "  back dull. `none` needs an A/B the product can supply — its amplifier stage\n"
          "  switched off, and what that removed measured. Without one the answer stays\n"
          "  unclassified however clean the spectrum looks.")
    return 0


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, help_text in (("measure", "corpus WAVs -> reference profile JSON"),
                            ("render-grid", ("render the model over the capture's own grid, "
                                            "as one more timbre of the corpus")),
                            ("compare", "render the same grid through libsonare and diff it"),
                            ("agree", ("measure a second, independent reference over the same "
                                      "grid and report which dimensions the two agree on")),
                            ("dynamics", "diff the pp->ff swing rather than one velocity at a time"),
                            ("takes", ("measure the phrase takes, which is where the couplings "
                                      "between notes live and where a note grid is blind")),
                            ("status", ("what an instrument has, what it is missing, and what "
                                       "the next round needs — the entry point of a loop")),
                            ("rig", ("measure whether this reference was recorded through an "
                                    "amplifier or a rotary, and say what the answer can be")),
                            ("room-match", ("what libsonare's own CC91 send and GS tank would "
                                           "have to be to sit in the reference's room"))):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--config", default=str(DEFAULT_CONFIG))
        p.add_argument("--corpus", default="", help="corpus directory (default: the capture output)")
        p.add_argument("--profile", default="", help="profile JSON (default: reference/<id>.json)")
        p.add_argument("--program", type=int, default=None,
                       help="GM program the model answers with (default: the one the "
                            "capture definition names, or the one the profile recorded)")
    rig_group = sub.choices["rig"]
    rig_group.add_argument(
        "--against", default="",
        help="comma-separated capture ids to compare the high-frequency shape against. "
             "Siblings from the same rack are what turn a dark spectrum into evidence of "
             "one filter, since they came through the same chain in the same session")
    rig_group.add_argument("--timbre", default="", help="which captured timbre to measure")
    for name in ("compare", "agree", "dynamics"):
        p = sub.choices[name]
        p.add_argument("--timbre", default="", help="which captured timbre to compare against")
        p.add_argument("--notes", default="", help="restrict to these MIDI notes, comma-separated")
    grid_group = sub.choices["render-grid"]
    grid_group.add_argument(
        "--timbre", default="model",
        help="name the model's grid takes in the corpus (default: model)")
    grid_group.add_argument(
        "--no-rig", action="store_true", dest="no_rig",
        help="render at the instrument's own boundary whatever the capture answers, "
             "clearing the amplifier the bank binds after an electric guitar's voice. "
             "Against the same grid rendered normally, the difference is the rig's own "
             "transfer function — the only measurement that separates it from the voice")
    status_group = sub.choices["status"]
    status_group.add_argument("--all", action="store_true", dest="every",
                              help="every shipped capture rather than one")
    status_group.add_argument("--archive", default="", dest="status_archive",
                              help="phrase-take reference archive (default: the audition one)")
    room_group = sub.choices["room-match"]
    room_group.add_argument("--take", default="", help="phrase to measure the room on "
                            "(default: the first the archive holds a reference for)")
    room_group.add_argument("--archive", default="", help="reference archive (default: the "
                            "audition one)")
    room_group.add_argument("--verbose", action="store_true", help="print every search point")
    takes_group = sub.choices["takes"]
    takes_group.add_argument("--only", default="", help="comma-separated take ids")
    takes_group.add_argument(
        "--archive", default="",
        help="reference renders of the phrases (default: the audition archive). These are "
             "not in the note corpus, which is one note at a time and so cannot hold a phrase")
    gate_group = sub.choices["compare"]
    gate_group.add_argument(
        "--gate", default="",
        help="hold the summary to the bounds in this JSON and exit 1 when one is exceeded. "
             "Catches the change that improves one dimension by breaking another, which is "
             "the shape most voice work takes and the one a listening test finds last")
    gate_group.add_argument(
        "--write-gate", default="", dest="write_gate",
        help="record the current summary as bounds at --margin times its values. Do this in "
             "the same change as the behaviour that justifies the new numbers")
    gate_group.add_argument(
        "--margin", type=float, default=1.25,
        help="slack in a written bound (default: 1.25x). Loose on purpose — a bound that "
             "fails on measurement noise gets switched off, and then it catches nothing")

    args = ap.parse_args()
    cfg = load_config(Path(args.config))
    if args.program is not None:
        cfg["program"] = args.program
        cfg["_program_override"] = True
    corpus_dir = Path(args.corpus).resolve() if args.corpus else out_root(cfg, "")
    profile_path = Path(args.profile) if args.profile else REFERENCE_DIR / f"{cfg['id']}.json"

    if args.cmd == "measure":
        return measure(cfg, corpus_dir, profile_path)
    if args.cmd == "render-grid":
        return render_grid(cfg, corpus_dir, timbre=args.timbre,
                           program=int(cfg.get("program", 0)),
                           bank=int(cfg.get("bank", 0) or 0),
                           rig=False if args.no_rig else None)
    if args.cmd == "status":
        from make_audition import DEFAULT_REFERENCE_ARCHIVE
        return status(cfg,
                      archive=Path(args.status_archive).expanduser().resolve()
                      if args.status_archive else DEFAULT_REFERENCE_ARCHIVE,
                      reference_dir=REFERENCE_DIR, every=bool(args.every))
    if args.cmd == "rig":
        return rig_evidence(cfg, corpus_dir, timbre=args.timbre,
                            against=[n.strip() for n in args.against.split(",") if n.strip()])
    if args.cmd == "room-match":
        from make_audition import DEFAULT_REFERENCE_ARCHIVE
        archive = (Path(args.archive).expanduser().resolve() if args.archive
                   else DEFAULT_REFERENCE_ARCHIVE)
        program = int(cfg.get("program", 0))
        take_id = args.take
        if not take_id:
            held = archived_take_ids(archive, cfg["id"])
            ordered = [t.id for t in build_takes(cfg.get("takes") or "", program)
                       ] if cfg.get("takes") in TAKE_SETS else []
            take_id = next((t for t in ordered if t in held), "")
            if not take_id:
                print(f"no archived phrase reference for {cfg['id']}", file=sys.stderr)
                return 2
        return room_match(cfg, archive=archive, take_id=take_id, program=program,
                          verbose=bool(args.verbose))
    if args.cmd == "takes":
        from make_audition import DEFAULT_REFERENCE_ARCHIVE
        return takes(cfg,
                     archive=Path(args.archive).expanduser().resolve() if args.archive
                     else DEFAULT_REFERENCE_ARCHIVE,
                     only={t.strip() for t in args.only.split(",") if t.strip()},
                     program=int(cfg.get("program", 0)),
                     bank=int(cfg.get("bank", 0) or 0))
    timbre = args.timbre or cfg["timbres"][0]["id"]
    notes_filter = {int(x) for x in args.notes.split(",") if x.strip()}
    if args.cmd == "dynamics":
        return dynamics(cfg, profile_path, timbre=timbre, notes_filter=notes_filter,
                        corpus_dir=corpus_dir)
    if args.cmd == "agree":
        if not profile_path.exists():
            print(f"no profile at {profile_path} — run `profile.py measure` first")
            return 2
        return agree(cfg, json.loads(profile_path.read_text()), timbre=timbre,
                     notes_filter=notes_filter, corpus_dir=corpus_dir)
    if args.gate and args.write_gate:
        print("--gate and --write-gate are alternatives: one checks the bounds and the "
              "other replaces them", file=sys.stderr)
        return 2
    return compare(cfg, profile_path, timbre=timbre, notes_filter=notes_filter,
                   gate_path=args.gate, write_gate=args.write_gate, margin=args.margin,
                   corpus_dir=corpus_dir)


if __name__ == "__main__":
    sys.exit(main())
