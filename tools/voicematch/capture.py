#!/usr/bin/env python3
"""Capture a reference corpus from an AudioUnit instrument, and calibrate the rig.

Four commands, in the order they are used:

    capture.py identify  --config capture/drums.json --channels 1-16
    capture.py calibrate --config capture/piano.json
    capture.py corpus    --config capture/piano.json
    capture.py verify    --config capture/piano.json

`identify` is the one that only a multitimbral rack needs, and it comes first
because a capture cannot name a slot it has not found. A rack answers on sixteen
channels and publishes no slot names, so the only way to learn what is loaded in
one is to play it: the command asks how much of the slot's answer to a key sits
on that key's own harmonic series (near 1 and it is an instrument, not a kit)
and, where it does not, how far the slot's diagnostic hits sit from the kit this
capture already measured. It reports and never edits the definition.

`calibrate` measures what the *host* has to do to record this plugin
faithfully, and refuses to guess. A disk-streaming sampler has two settings
that silently ruin a recording — how long it is given to load, and whether it
is driven in real time — and neither one announces itself: too little settling
gives a file of the right length holding a peak three orders of magnitude too
small, and too much speed gives one that starts correctly and goes silent in
the middle. Both look fine in a file browser. The calibration bisects the
settle time, renders the same note with and without real time, renders one note
twice to check the plugin is identical to itself, and writes the recipe out.

`corpus` renders the note x velocity x timbre grid one note per process, which
is what keeps a sympathetic-resonance model from letting the previous note bleed
into this one's measurement. Each finished render is appended to the manifest
before the next starts, so an interrupted run leaves an exact record and
re-running resumes from it.

`verify` re-reads what is on disk and reports the failures that leave a
plausible file behind — silence, a clipped peak, a velocity that went nowhere.

The audio this writes stays out of the repository: a sample library's licence
covers what you render from it. `profile.py` turns the corpus into measurements,
and it is the measurements that are committed.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import replace
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT  # noqa: E402
from au_oracle import (  # noqa: E402
    AuRenderError,
    AuSource,
    dry_params,
    find_aubounce,
    resolve_preset,
)
from metrics import analyze_hit, harmonic_share, midi_to_hz, to_mono  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import read_wav  # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE / "capture" / "piano.json"
# Captured audio is licensed by whoever made the instrument, so it never enters
# the tree. SONARE_VOICEMATCH_ROOT names where a corpus lives; without it the
# renders go to an untracked scratch directory inside the checkout, so a fresh
# clone captures somewhere sane instead of failing on a path only one machine
# has.
CORPUS_ROOT = Path(
    os.environ.get("SONARE_VOICEMATCH_ROOT") or REPO_ROOT / ".cache" / "voicematch"
).expanduser()
DEFAULT_OUT_ROOT = CORPUS_ROOT / "capture"

#: One-based MIDI channel 10, on which a note number selects an instrument
#: instead of a pitch. `write_smf` counts channels from zero.
PERCUSSION_CHANNEL = 10

#: What a timbre's `channel` may say, and the whole of it. MIDI answers "what
#: does a note number mean here" two ways and no more, so a `channel` of 7 is an
#: address that has been written into the field deciding which metric set
#: measures the capture. A rack slot is addressed by `slot_channel`.
NOTE_CHANNELS = (1, PERCUSSION_CHANNEL)

#: What a capture's `rig` may answer. `dry` asks about a room and cannot stand in
#: for it: dryness is looked for as a tail and a cabinet has none, so a close-mic'd
#: amplified guitar reads dry with the whole rig inside it. Absent is
#: `unclassified` and never "no rig" — a capture that does not say is one nobody
#: has answered for.
RIG_UNCLASSIFIED = "unclassified"
RIG_NONE = "none"
RIG_BAKED = "baked"
RIG_VALUES = (RIG_UNCLASSIFIED, RIG_NONE, RIG_BAKED)

#: The GM programs whose reference may have an amplifier, a cabinet or a rotary
#: speaker inside it. An unanswered capture is a fit hazard here and nowhere else;
#: a wind or a piano is not waiting on anyone. A building is deliberately absent —
#: it is a room, and a room is measured and convolved onto the model rather than
#: refused (`src/midi/synth/docs/voicing.md`).
RIG_CAPABLE_PROGRAMS = frozenset(
    {4, 5, 7}              # electric pianos and the clavinet, played through an amp
    | {16, 17, 18}         # drawbar, percussive and rock organ, through a rotary cabinet
    | set(range(26, 32))   # electric guitars, jazz through harmonics
    | set(range(33, 38))   # electric basses, fingered through slap
)


# --------------------------------------------------------------------------
# config


def local_overlay(path: Path) -> dict:
    """The untracked half of a capture definition: which product it was.

    A capture definition answers two questions at once — *how* an instrument was
    measured, and *which product* was measured. The first is the method and
    belongs in the repository: the grid, the gate length, the GM program the
    model answers with, and why each is what it is. The second names a
    commercial sample library, and a public repository that carries it is a
    public statement about where the reference data came from.

    So the tracked file describes the instrument by category and the sibling
    `<id>.local.json` supplies the identity: the plugin's component triple and
    each timbre's preset. It is gitignored, it is only needed to *re-capture*,
    and its absence is not an error — everything downstream reads the committed
    `reference/<id>.json`, so scoring, fitting and diagnosing work without it.
    """
    local = path.with_suffix(".local.json")
    if not local.exists():
        return {}
    return json.loads(local.read_text())


def merge_overlay(cfg: dict, overlay: dict) -> dict:
    """Fold the identity overlay into a capture definition, matching timbres by id."""
    if not overlay:
        return cfg
    timbres = {t["id"]: dict(t) for t in overlay.get("timbres", [])}
    merged = {**cfg, **{k: v for k, v in overlay.items() if k != "timbres"}}
    merged["timbres"] = [
        {**t, **timbres.get(t["id"], {})} for t in cfg.get("timbres", [])
    ]
    return merged


def load_config(path: Path) -> dict:
    """Read a capture definition and fill in the defaults it left out."""
    cfg = merge_overlay(json.loads(path.read_text()), local_overlay(path))
    cfg.setdefault("sample_rate", 48000)
    cfg.setdefault("settle_ms", 4000)
    cfg.setdefault("realtime", True)
    cfg.setdefault("gate_ms", 8000)
    cfg.setdefault("tail", "2s")
    # How long is recorded after note-off, for the notes that need more than the
    # rest. A cymbal's wash runs four to ten seconds; measured on the captured
    # kit, every cymbal's `decay_ms` — the time to fall 20 dB — sits between
    # 1170 and 1760 ms against an analysis ceiling of 1800, which means those
    # numbers are the window rather than the instrument, and what happens after
    # it was never recorded. A flat `tail` long enough for a ride would triple
    # the whole grid's render time for the sake of ten notes.
    cfg.setdefault("tail_by_note", {})
    cfg.setdefault("preroll_ms", 100)
    cfg.setdefault("dry", True)
    # Whether the reference this capture produces carries a rig between the
    # instrument and the microphone. `dry` cannot answer it (see RIG_VALUES), and
    # the answer is what gates fitting: `corpus.check_rig`.
    cfg.setdefault("rig", RIG_UNCLASSIFIED)
    if cfg["rig"] not in RIG_VALUES:
        raise ValueError(
            f"{cfg.get('id', path.name)}: rig is one of {', '.join(RIG_VALUES)}, "
            f"not {cfg['rig']!r}"
        )
    cfg.setdefault("params", [])
    # Which instrument this capture is a reference for. The capture is the only
    # place that knows: the plugin, the phrase set, the GM program the model
    # answers with and the dimensions worth scoring are one decision, and
    # splitting them across a config, a command line and a hardcoded constant is
    # how a harpsichord ends up measured against program 0.
    cfg.setdefault("program", 0)
    cfg.setdefault("takes", "")
    cfg.setdefault("dimensions", [])
    for timbre in cfg.get("timbres") or []:
        channel = int(timbre.get("channel", 1))
        if channel not in NOTE_CHANNELS:
            raise ValueError(
                f"{cfg.get('id', path.name)}: timbre {timbre.get('id', '?')!r} declares "
                f"channel {channel}, which is a rack slot in the field that says what a "
                f"note number MEANS. Write it as `slot_channel`; `channel` answers "
                f"{PERCUSSION_CHANNEL} (a note selects an instrument) or 1 (a note "
                f"selects a pitch)"
            )
    cfg["_path"] = str(path)
    return cfg


def config_params(cfg: dict) -> tuple[str, ...]:
    """The `--param` list: the config's own, plus every effect section switched off."""
    params = tuple(cfg.get("params", ()))
    if cfg.get("dry", True):
        params = tuple(dict.fromkeys(dry_params(cfg["plugin"]) + params))
    return params


def rig_capable(program: int) -> bool:
    """Whether a reference for this GM program could have a rig recorded into it."""
    return int(program) in RIG_CAPABLE_PROGRAMS


def tail_for(cfg: dict, note: int) -> str:
    """How long to record after note-off for one note of the grid.

    `tail_by_note` maps a MIDI note (as a string, since it comes from JSON) to
    its own tail; everything unnamed keeps the capture's flat `tail`. A range is
    written as "49-59" and covers both ends.
    """
    table = cfg.get("tail_by_note") or {}
    for key, value in table.items():
        for part in str(key).split(","):
            part = part.strip()
            if "-" in part:
                lo, hi = part.split("-", 1)
                if int(lo) <= note <= int(hi):
                    return str(value)
            elif part and int(part) == note:
                return str(value)
    return str(cfg["tail"])


def tail_seconds(cfg: dict, note: int) -> float:
    """`tail_for` as a number of seconds, for the sides that render rather than record.

    The capture records each note for its own tail; the model has to be rendered
    over the same window or the two are not comparable. Reading it from the same
    table rather than from a constant is what keeps them together: a flat 2 s
    model render against an 8 s captured cymbal compares two seconds of model
    against two seconds of reference and eight seconds of decay against nothing,
    which reads as a model whose every band dies too fast.
    """
    text = tail_for(cfg, note).strip().lower()
    return float(text[:-1]) if text.endswith("s") else float(text)


def note_map(cfg: dict) -> dict[int, int]:
    """Which model note answers each captured note, where they disagree.

    A drum note number names an instrument, and a sampled kit is under no
    obligation to lay its instruments out the way GM does. The kit measured for
    `reference/drums.json` does not: its six toms ascend in the order 45, 47,
    48, 50, 41, 43, so a note-for-note comparison scores the low floor tom
    against the high one and reports a tuning error that is really a mapping.

    Applied to the ORACLE side only — the model is rendered on GM's layout,
    which is the layout it ships with and the one a user's MIDI file is written
    against. Correcting the model would be calibrating out the reference's
    idiosyncrasy into the product.

    Keys arrive from JSON as strings and are returned as ints. An empty map is
    the normal case: only a capture whose layout was measured and found to
    differ carries one.
    """
    return {int(k): int(v) for k, v in (cfg.get("note_map") or {}).items()}


def note_groups(cfg: dict) -> dict[str, tuple[int, ...]]:
    """Which of the capture's notes are members of one family, by family name.

    A percussion capture's note numbers name instruments, and some of those
    instruments are one thing: six toms are one series of sizes, three hi-hats
    are one mechanism at three openings, a mute and an open cuica are one drum
    played two ways. `loss._kit_terms` scores the relations inside each family,
    which is the only part of a kit no per-hit measurement can reach.

    A family is a SET here and never a sequence — the term compares the sorted
    contrasts within it — so a capture whose layout disagrees with GM's needs no
    order and no map to be grouped correctly.

    Names are for the reader; nothing keys off them. An absent block is the
    normal case, and gives a fit with no kit relations rather than an error:
    only a capture whose families were identified in its own rows carries one.
    """
    return {str(name): tuple(int(n) for n in notes)
            for name, notes in (cfg.get("groups") or {}).items()}


def slot_channel(timbre: dict) -> int:
    """Which MIDI channel this timbre's rack slot answers on.

    An address, and nothing but one. The timbre's `channel` is the other
    quantity: what its note numbers MEAN, which `profile.is_percussion` reads to
    choose the metric set for the whole capture. One name carried both until a
    rack slot number reached that field, and then a banjo in slot 10 was measured
    as a drum map — every note reported a band profile and none reported a pitch.

    They part company in both directions: addressing a kit's slot on 10 when the
    rack keeps it on 15 plays whatever else lives there, and declaring the kit on
    15 sends a drum map through the pitched measurements. So a rack capture
    declares `slot_channel` and leaves `channel` alone.

    Absent, the semantic channel addresses the slot. That is safe rather than
    historical: `load_config` holds `channel` to `NOTE_CHANNELS`, so the fallback
    can only ever reach slot 1 or slot 10 — the plugin that is not a rack, and
    the rack that does keep its kit where GM puts it.
    """
    return int(timbre.get("slot_channel", timbre.get("channel", 1)))


def source_for(cfg: dict, timbre: dict, **overrides) -> AuSource:
    """The `AuSource` that records one timbre of a capture definition."""
    src = AuSource(
        plugin=cfg["plugin"],
        preset=timbre.get("preset", ""),
        # The other route to a timbre, for a plugin whose audio-unit build keeps
        # its state under its own keys rather than Steinberg's: a `.vstpreset`
        # reaches those two keys only, so such a plugin takes one without
        # complaint and ignores it. A saved class-info dictionary reaches it.
        state=timbre.get("state", ""),
        # A rack selects its timbres by channel rather than by preset: one file
        # is loaded and each channel plays a different slot of it.
        channel=slot_channel(timbre),
        params=config_params(cfg),
        settle_ms=int(cfg["settle_ms"]),
        realtime=bool(cfg["realtime"]),
        preroll_ms=int(cfg["preroll_ms"]),
        tail=str(cfg["tail"]),
        sample_rate=int(cfg["sample_rate"]),
    )
    return replace(src, **overrides) if overrides else src


def out_root(cfg: dict, cli_out: str) -> Path:
    if cli_out:
        return Path(cli_out).expanduser().resolve()
    return DEFAULT_OUT_ROOT / cfg["id"]


# --------------------------------------------------------------------------
# calibrate


def config_sends(cfg: dict) -> tuple[int, int, int] | None:
    """The capture's `sends`, or None where it declares none.

    Present, the grid is rendered from a generated score carrying those CC91 /
    CC93 / CC94 values; absent, from aubounce's own note arguments, which write
    no controllers at all. The opt-in exists because it changes what a render
    IS: a plugin whose effect sections are not advertised as parameters cannot
    be dried by `dry`, and a GM-compatible one can still be dried by sending it
    a zero reverb. Every reference measured before this field existed was taken
    through the note arguments, and those files are the ground truth rather
    than something regenerable, so the default has to leave that path alone.
    """
    sends = cfg.get("sends")
    if sends is None:
        return None
    if len(sends) != 3:
        raise ValueError(f"{cfg['id']}: sends takes three values (reverb, chorus, delay)")
    return tuple(int(v) for v in sends)  # type: ignore[return-value]


def _note_argv(source: AuSource, out: Path, note: int, velocity: int, gate_ms: int,
               *, sends: tuple[int, int, int] | None = None) -> list[str]:
    if sends is not None:
        # A file supplies its own channel, and aubounce refuses `--channel`
        # beside `--midi` rather than dropping it, so the slot goes in here.
        midi = out.with_suffix(".mid")
        midi.parent.mkdir(parents=True, exist_ok=True)
        midi.write_bytes(write_smf(
            [Note(note, velocity, 0.0, gate_ms / 1000.0)],
            program=-1, channel=source.channel - 1, sends=sends,
        ))
        return source.argv(out, midi=midi)
    argv = source.argv(out)
    # `argv` builds a render with no notes in it; a single note is what a
    # calibration probe needs and what the corpus grid is made of.
    return argv[:3] + ["--note", str(note), "--velocity", str(velocity), "--gate-ms", str(gate_ms)] + argv[3:]


def _probe(source: AuSource, out: Path, note: int, velocity: int, gate_ms: int,
           *, sends: tuple[int, int, int] | None = None) -> dict:
    """One calibration render. Returns aubounce's summary, or the refusal as data."""
    argv = _note_argv(source, out, note, velocity, gate_ms, sends=sends)
    started = time.monotonic()
    proc = subprocess.run(argv, capture_output=True, text=True)
    wall = time.monotonic() - started
    if proc.returncode != 0:
        return {"error": proc.stderr.strip()[:400], "wall_s": wall}
    try:
        summary = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {"error": f"no JSON: {proc.stdout[:200]}", "wall_s": wall}
    summary["wall_s"] = round(wall, 2)
    return summary


def calibrate(cfg: dict, out: Path, *, note: int, velocity: int, verbose: bool) -> dict:
    """Measure the host settings this plugin needs, and prove they are stable.

    Reports rather than asserts. The numbers are the point: a future plugin
    update that moves the minimum settle time shows up here as a changed number
    rather than as a corpus that is quietly missing its first note.
    """
    timbre = cfg["timbres"][0]
    base = source_for(cfg, timbre, tail="2s")
    sends = config_sends(cfg)
    scratch = out / "_calibration"
    scratch.mkdir(parents=True, exist_ok=True)
    gate_ms = 2000
    report: dict = {
        "config": cfg["_path"],
        "plugin": cfg["plugin"],
        "aubounce": str(find_aubounce()),
        "timbre": timbre.get("id", ""),
        "preset": str(resolve_preset(timbre["preset"])) if timbre.get("preset") else "",
        "params": list(base.params),
        "sends": list(sends) if sends else None,
        "probe": {"note": note, "velocity": velocity, "gate_ms": gate_ms},
    }

    # 1. Real time or not. A sampler driven at full speed reports the gap it
    #    left in the middle of the note; there is nothing else to notice.
    print("== real time ==", file=sys.stderr)
    rt_rows = []
    for realtime in (True, False):
        s = _probe(replace(base, realtime=realtime, settle_ms=max(4000, base.settle_ms)),
                   scratch / f"rt_{realtime}.wav", note, velocity, gate_ms, sends=sends)
        rt_rows.append({"realtime": realtime, **{k: s.get(k) for k in
                                                 ("peak", "dropout_ms", "seconds", "wall_s", "error")}})
        print(f"  realtime={str(realtime):5s} peak={s.get('peak', 0):.4f} "
              f"dropout={s.get('dropout_ms', '?')}ms wall={s.get('wall_s', 0):.1f}s", file=sys.stderr)
    report["realtime"] = rt_rows
    realtime_required = bool(next(r for r in rt_rows if not r["realtime"])["dropout_ms"])
    report["realtime_required"] = realtime_required

    # 2. Settle time, by bisection on "does the note come out INTACT".
    #
    # Not "at all": an absolute floor only catches the failure at its extreme.
    # Under-settling degrades before it silences, and on one rack here the
    # degraded render peaks 0.0209 against a correct 0.1733 -- 18 dB down, three
    # times louder than the floor, and reported ok. The recipe that came out of
    # it was a settle at which the rack renders silence or a note arriving
    # nearly three seconds late, so every grid built on it would have been built
    # on broken audio.
    #
    # So a settle is judged against the render step 1 already took at a generous
    # settle, on the two ways it goes wrong: the note comes out quiet, or it
    # comes out late. Both are compared rather than thresholded absolutely.
    print("== settle ==", file=sys.stderr)
    src = replace(base, realtime=True)
    settle_rows = []
    # What this plugin reaches when it has had time. Step 1 renders at
    # `max(4000, base.settle_ms)`, so it is the intact reading to measure against.
    reference_peak = max((float(r["peak"] or 0.0) for r in rt_rows), default=0.0)

    def sounds(ms: int) -> bool:
        wav = scratch / f"settle_{ms}.wav"
        s = _probe(replace(src, settle_ms=ms), wav, note, velocity, gate_ms, sends=sends)
        peak, drop = float(s.get("peak", 0.0)), s.get("dropout_ms", 0)
        onset = _onset_ms(wav, int(cfg["sample_rate"])) if wav.exists() else None
        quiet = reference_peak > 0.0 and peak < reference_peak * SETTLE_PEAK_RATIO
        late = onset is not None and onset > float(src.preroll_ms) + ONSET_SLACK_MS
        ok = peak >= src.min_peak and not drop and not quiet and not late
        why = ("ok" if ok else
               "SILENT" if peak < src.min_peak else
               "DROPOUT" if drop else
               f"QUIET ({peak / reference_peak:.2f}x the settled peak)" if quiet else
               f"LATE ({onset:.0f} ms)" if late else "not ok")
        settle_rows.append({"settle_ms": ms, "peak": peak, "dropout_ms": drop,
                            "onset_ms": None if onset is None else round(onset, 2),
                            "seconds": s.get("seconds"), "wall_s": s.get("wall_s"), "ok": ok})
        print(f"  settle={ms:6d} peak={peak:.4f} dropout={drop}ms -> {why}", file=sys.stderr)
        return ok

    lo, hi = 0, 500
    while hi <= 32000 and not sounds(hi):
        lo, hi = hi, hi * 2
    if hi > 32000:
        report["settle"] = settle_rows
        report["settle_min_ms"] = None
        report["error"] = "no settle time up to 32 s produced a note"
        _write(out / "calibration.json", report)
        return report
    while hi - lo > 250:
        mid = (lo + hi) // 2
        if sounds(mid):
            hi = mid
        else:
            lo = mid
    report["settle"] = settle_rows
    report["settle_min_ms"] = hi
    # Twice the measured minimum, because the minimum is what this machine
    # needed while otherwise idle and a loaded machine needs more.
    report["settle_recommended_ms"] = max(2 * hi, 2000)

    # 3. The plugin against itself. Everything downstream assumes a render is a
    #    property of the settings rather than of the moment.
    print("== determinism ==", file=sys.stderr)
    rec = report["settle_recommended_ms"]
    det = replace(src, settle_ms=rec)
    a = _probe(det, scratch / "det_a.wav", note, velocity, gate_ms, sends=sends)
    b = _probe(det, scratch / "det_b.wav", note, velocity, gate_ms, sends=sends)
    same = False
    if "error" not in a and "error" not in b:
        wa, _ = read_wav(scratch / "det_a.wav")
        wb, _ = read_wav(scratch / "det_b.wav")
        same = wa.shape == wb.shape and bool(np.array_equal(wa, wb))
    report["deterministic"] = same
    report["determinism"] = {"a": a.get("peak"), "b": b.get("peak"),
                             "identical_samples": same}
    print(f"  two renders identical: {same}", file=sys.stderr)

    # 4. Dryness. A reference with a room in it makes every timbre metric lie in
    #    the same direction, so the capture has to prove there is none.
    print("== dryness ==", file=sys.stderr)
    try:
        from room import estimate_room

        audio, sr = read_wav(scratch / "det_a.wav")
        # The note occupies preroll .. preroll+gate; what follows it is the
        # damper falling plus whatever space the capture picked up.
        on = det.preroll_ms / 1000.0
        room = estimate_room(audio, sr, [(on, on + gate_ms / 1000.0)])
        rt60 = float(getattr(room, "rt60_s", 0.0) or 0.0)
        report["room"] = {"rt60_s": rt60, "hf_ratio": float(getattr(room, "hf_ratio", 0.0) or 0.0)}
        print(f"  measured RT60 {rt60:.2f}s "
              f"({'dry' if rt60 < 0.35 else 'A ROOM IS IN THE CAPTURE'})", file=sys.stderr)
    except Exception as exc:  # room measurement is a report, never a gate
        report["room"] = {"error": f"{type(exc).__name__}: {exc}"}

    # 5. Level at the reference velocity, so a later capture can be compared to
    #    this one rather than to whatever the master volume happened to be.
    audio, sr = read_wav(scratch / "det_a.wav")
    mono = audio.mean(axis=1)
    report["reference_level"] = {
        "note": note, "velocity": velocity,
        "peak_dbfs": round(float(20 * np.log10(max(np.abs(mono).max(), 1e-12))), 2),
        "rms_dbfs": round(float(20 * np.log10(max(np.sqrt((mono ** 2).mean()), 1e-12))), 2),
    }

    _write(out / "calibration.json", report)
    print(f"\nrecipe: --realtime={'required' if realtime_required else 'optional'} "
          f"--settle-ms {report['settle_recommended_ms']} "
          f"(minimum measured {report['settle_min_ms']})", file=sys.stderr)
    print(f"-> {out / 'calibration.json'}", file=sys.stderr)
    return report


# --------------------------------------------------------------------------
# corpus


def corpus(cfg: dict, out: Path, *, resume: bool, limit: int, verbose: bool) -> int:
    """Render the note x velocity x timbre grid, one note per process."""
    manifest_path = out / "manifest.json"
    done: dict[str, dict] = {}
    # Timbres somebody else registered in this corpus — `profile.py render-grid`
    # adds the model's own grid as one, flagged `model: true`, and that flag is
    # what `profile.py measure` excludes it by. The renders survive a re-capture
    # because they are in `done`; without this the registration does not, so a
    # corpus re-rendered after a render-grid keeps the model's audio and loses
    # the only thing that says it is the model. `measure` then folds the voice
    # being calibrated into the reference it is calibrated against, which is
    # silent, self-confirming, and cost this capture its measured band edge:
    # `measure_band_edge` reads across-instrument separation, and the model's
    # rows are not one of the instruments.
    foreign: list[dict] = []
    if resume and manifest_path.exists():
        prior = json.loads(manifest_path.read_text())
        done = {r["id"]: r for r in prior.get("renders", [])}
        ours = {t["id"] for t in cfg["timbres"]}
        foreign = [t for t in prior.get("timbres", [])
                   if isinstance(t, dict) and t.get("id") not in ours]

    # Loudest velocity of a note first, so every quieter render of that note can
    # be checked against a level that is known to have loaded.
    jobs = [
        (t, n, v)
        for t in cfg["timbres"]
        for n in cfg["notes"]
        for v in sorted(cfg["velocities"], reverse=True)
    ]
    if limit:
        jobs = jobs[:limit]

    header = {
        "id": cfg["id"],
        "config": cfg["_path"],
        "plugin": cfg["plugin"],
        "sample_rate": cfg["sample_rate"],
        "gate_ms": cfg["gate_ms"],
        "tail": cfg["tail"],
        "preroll_ms": cfg["preroll_ms"],
        "settle_ms": cfg["settle_ms"],
        "realtime": cfg["realtime"],
        "params": list(config_params(cfg)),
        "sends": list(config_sends(cfg)) if config_sends(cfg) else None,
        "timbres": list(cfg["timbres"]) + foreign,
        "notes": cfg["notes"],
        "velocities": cfg["velocities"],
        # Carried into the manifest so a corpus is self-describing, the way the
        # note list and the velocities are. `corpus.py` falls back to the config
        # this header names for a manifest written before the block existed.
        "groups": cfg.get("groups", {}),
        "rig": cfg["rig"],
    }

    todo = [j for j in jobs if _job_id(*j) not in done or not (out / done[_job_id(*j)]["path"]).exists()]
    per = (cfg["settle_ms"] / 1000.0) + (cfg["gate_ms"] / 1000.0) + 2.5
    print(f"{len(todo)} renders to go of {len(jobs)} "
          f"(~{per:.0f}s each, ~{len(todo) * per / 60:.0f} min)", file=sys.stderr)

    # The reference level per (timbre, note): its loudest velocity, from this run
    # or from a previous one that is already in the manifest.
    loudest: dict[tuple[str, int], float] = {}
    for row in done.values():
        k = (row["timbre"], row["note"])
        loudest[k] = max(loudest.get(k, 0.0), float(row.get("peak", 0.0)))

    failures = 0
    started = time.monotonic()
    for i, (timbre, note, vel) in enumerate(todo, 1):
        jid = _job_id(timbre, note, vel)
        rel = Path(timbre["id"]) / f"n{note:03d}_v{vel:03d}.wav"
        src = source_for(cfg, timbre, tail=tail_for(cfg, note))
        try:
            summary = _render_note(
                src, out / rel, note, vel, int(cfg["gate_ms"]), sends=config_sends(cfg),
                floor_peak=loudest.get((timbre["id"], note), 0.0),
                preroll_ms=float(cfg["preroll_ms"]),
                sample_rate=int(cfg["sample_rate"]),
            )
            loudest[(timbre["id"], note)] = max(
                loudest.get((timbre["id"], note), 0.0), float(summary["peak"])
            )
            done[jid] = {
                "id": jid, "timbre": timbre["id"], "note": note, "velocity": vel,
                "path": str(rel), "peak": summary["peak"], "seconds": summary["seconds"],
                "preroll_peak": summary.get("preroll_peak", 0.0),
                "onset_ms": summary.get("onset_ms"),
                "attempts": summary.get("attempts", 1),
            }
        except (AuRenderError, json.JSONDecodeError) as exc:
            failures += 1
            done.pop(jid, None)
            print(f"[{i}/{len(todo)}] FAIL {jid}: {exc}", file=sys.stderr)
        else:
            elapsed = time.monotonic() - started
            eta = elapsed / i * (len(todo) - i)
            print(f"[{i}/{len(todo)}] {jid} peak {summary['peak']:.4f} "
                  f"({summary['seconds']:.1f}s)  eta {eta / 60:.0f}m", file=sys.stderr)
        # Written every time, so an interrupted run is an exact record rather
        # than an approximate one.
        _write(manifest_path, {**header, "renders": sorted(done.values(), key=lambda r: r["id"])})

    print(f"\n{len(done)} renders in {out}, {failures} failed", file=sys.stderr)
    return 1 if failures else 0


def _job_id(timbre: dict, note: int, vel: int) -> str:
    return f"{timbre['id']}/n{note:03d}_v{vel:03d}"


# A render that loaded is never this far below the loudest velocity of its own
# note. Measured on the piano sampler: the real ratio from velocity 24 to 120 is
# around -24 dB, and a render whose samples did not arrive sits at -42 dB.
QUIET_RATIO = 1.0 / 40.0  # -32 dB

# The ratio above is a piano's dynamic range, and an instrument with a wider one
# runs off the bottom of it. A clarinet reads 49 dB from velocity 16 to 127 while
# its centroid moves 574 Hz to 1625, so velocity is a change of timbre rather
# than of level -- and its softest layer sits below -32 dB at every note from 62
# up, which rejected seven legitimate cells five attempts each at a stable value.
#
# Level cannot separate the two populations there, so this asks the other
# question: not how loud the render is, but whether it is the note. A quiet real
# note still puts all of its energy on the series of the key that was pressed,
# and a render whose samples never arrived has no series to put it on.
#
# Measured on both populations rather than chosen. Across six slots of one rack
# -- including the breathy ones, flute and piccolo -- and 40 dB of level, a real
# note reads 0.9996 to 1.0000. Broadband noise reads 0.43 at both -42 and -60
# dBFS, which is the fraction a flat spectrum puts inside the partial bands by
# construction and so does not drift with level; digital silence and a lone click
# report nothing at all. The threshold sits between the two with room for an
# instrument noisier than any measured here.
#
# It needs no percussion exemption, because it cannot fire for percussion: a kit
# answers a key with an instrument whose partials have no relation to it, reads
# far below this, and keeps exactly today's behaviour.
QUIET_TONE_SHARE = 0.8

# The other failure, and the one the ratio above is structurally unable to see:
# the note arrives LATE, and louder than it should be.
#
# Measured on the drum kit by re-rendering the grid and comparing. Every render
# that reproduced sits at exactly the preroll — 100.1 to 100.6 ms on a 100 ms
# preroll — while every render that came back a different signal begins 150 to
# 843 ms in, after a stretch of digital silence, and is 4 to 25 dB LOUDER than
# the note really is. Their correlation with the correct render is 0.000: it is
# not the note shifted, it is a different event. Whole instruments came out that
# way — all six velocities of the closed hi-hat, all six of the mute triangle —
# and their velocity ramps were nonsense (-7.99, -8.03, -8.04, -0.57, -0.79,
# -0.32 dBFS) against clean ones that ascend smoothly.
#
# Being loud is exactly why nothing caught it. `QUIET_RATIO` asks whether the
# samples arrived; this asks whether they arrived on time, which is the only
# part of the failure that shows. A slow-attack instrument does not defeat it:
# the test is the first sample over an absolute -80 dBFS, not the peak.
#
# The width is measured rather than chosen, because the first value chosen was
# too wide to catch the failure it was written for. Across a 282-render drum
# grid every correct render onsets between 100.00 and 100.62 ms against a 100 ms
# preroll, and nothing legitimately lands between there and the failure -- while
# one render of the open hi-hat came back 31 ms late, 6.6 dB over a velocity
# ramp whose neighbouring step is 0.16 dB, and was recorded because 31 sits
# under a 40 ms slack. Re-rendered four times it returned the same correct take
# every time, so the failure is a one-off the guard has to see, not a property
# of the slot.
#
# One blind spot to know: this cannot fire for a source whose samples carry
# their own pre-attack noise. Fifty of that grid's renders read an onset of 0
# because the library's room tone is already sounding before the strike, and a
# late render inside one of those is invisible here. `capture.py verify` names
# them as energy before the note.
ONSET_SLACK_MS = 10.0
#: Absolute level that counts as the render having begun. Well under anything an
#: instrument radiates and well over a silent preroll, which is exactly zero.
ONSET_FLOOR_DBFS = -80.0

#: How far under the settled peak a calibration render may sit and still count as
#: that plugin loaded. An under-settled render is not silent, it is weak: on the
#: rack that forced this, the same note reads 0.0209 at a half-second settle and
#: 0.1733 at eight, and reads the latter three times running once it is there.
#: Half leaves room for a plugin less repeatable than these while rejecting a
#: miss of that size, which is 0.12 of the settled peak.
SETTLE_PEAK_RATIO = 0.5


def _onset_ms(path: Path, sr: int) -> float | None:
    """Where the written render's audio actually begins, in ms, or None if silent."""
    audio, file_sr = read_wav(path)
    if audio.size == 0:
        return None
    level = np.abs(audio).max(axis=1) if audio.ndim > 1 else np.abs(audio)
    over = level > 10.0 ** (ONSET_FLOOR_DBFS / 20.0)
    if not over.any():
        return None
    return float(np.argmax(over)) / float(file_sr or sr) * 1000.0


def _tone_share(path: Path, note: int, sr: int, preroll_ms: float) -> float | None:
    """How much of the written render sits on the series of the key that was pressed.

    `None` where there is nothing to measure — a silent render, or one too short
    to resolve the tolerance.
    """
    audio, file_sr = read_wav(path)
    if audio.size == 0:
        return None
    rate = int(file_sr or sr)
    mono = to_mono(audio)[int(rate * preroll_ms / 1000.0):]
    return harmonic_share(mono, rate, midi_to_hz(note))


def _render_note(src: AuSource, out: Path, note: int, vel: int, gate_ms: int,
                 *, floor_peak: float, preroll_ms: float = 0.0,
                 sample_rate: int = 48000, attempts: int = 5,
                 sends: tuple[int, int, int] | None = None) -> dict:
    """One corpus render, retried while it comes back too quiet to be the note.

    The failure this catches is a race inside the plugin rather than a setting:
    the same note at the same velocity renders correctly, then near-silently,
    then correctly again, at roughly one failure in three, and *more* settling
    time does not reduce it — 4 s and 16 s both produced the real 0.0122 while
    8 s in between produced 0.0010. So the answer is to notice and try again,
    not to slow the capture down.

    What decides "too quiet" is the loudest velocity already recorded for this
    same note, not an absolute floor: the real notes at the top of the keyboard
    at velocity 24 are quiet enough that any fixed threshold either lets a
    failure through down there or rejects real notes.

    A relative floor is still a level test, and on an instrument whose dynamic
    range is wider than the one it was measured on it cuts into real notes. So
    falling under it is a suspicion rather than a verdict: what settles it is
    whether the render carries the series of the key that was pressed. See
    QUIET_TONE_SHARE.
    """
    out.parent.mkdir(parents=True, exist_ok=True)
    floor = max(src.min_peak, floor_peak * QUIET_RATIO)
    last = ""
    for attempt in range(attempts):
        proc = subprocess.run(
            _note_argv(src, out, note, vel, gate_ms, sends=sends),
            capture_output=True, text=True,
        )
        if proc.returncode != 0:
            last = proc.stderr.strip()[:400]
            continue
        try:
            summary = json.loads(proc.stdout)
        except json.JSONDecodeError:
            last = f"no JSON: {proc.stdout[:160]}"
            continue
        if summary.get("dropout_ms"):
            last = (
                f"dropout {summary['dropout_ms']} ms inside the note. On a treble note held for "
                f"seconds this can be the note simply finishing while the key is still down, "
                f"which an aubounce from before it told the two apart reports as a dropout — "
                f"check `aubounce --version` before reading it as a streaming failure."
            )
            continue
        peak = float(summary.get("peak", 0.0))
        if peak < floor:
            # Quiet is not the same as absent. See QUIET_TONE_SHARE.
            share = _tone_share(out, note, sample_rate, preroll_ms)
            if share is None or share < QUIET_TONE_SHARE:
                heard = "no tone at all" if share is None else f"a tone share of {share:.3f}"
                last = (f"peak {peak:.5f} against a floor of {floor:.5f} and {heard}: "
                        f"the samples did not arrive")
                continue
            summary["quiet_tone_share"] = round(share, 4)
        onset = _onset_ms(out, sample_rate)
        summary["onset_ms"] = None if onset is None else round(onset, 2)
        if onset is not None and onset > preroll_ms + ONSET_SLACK_MS:
            # See ONSET_SLACK_MS. Loud enough to pass the ratio above and not
            # the note: retried rather than recorded.
            last = (f"the render begins {onset:.0f} ms in against a {preroll_ms:.0f} ms "
                    f"preroll: this is not the note")
            continue
        if attempt:
            print(f"       (took {attempt + 1} attempts)", file=sys.stderr)
        summary["attempts"] = attempt + 1
        return summary
    raise AuRenderError(f"{last} (after {attempts} attempts)")


# --------------------------------------------------------------------------
# identify

#: The octave ladder stage one plays. Three notes rather than one because a
#: patch that is silent or unvoiced on a single key would otherwise read as a
#: kit, and two octaves is wide enough that no melodic patch covers only part
#: of it.
PITCH_PROBE_NOTES = (48, 60, 72)
#: Where a slot stops being an instrument. Measured on both sides rather than
#: chosen: the captured concert grand reads 0.95 to 1.00 across its whole
#: compass, and the captured kit's slot reads 0.05 to 0.15 on the same three
#: keys. Anything between those is a slot worth listening to rather than a
#: verdict, which is what the reported number is for.
MELODIC_SHARE = 0.5


def _pitch_probe(src: AuSource, scratch: Path, channel: int, velocity: int,
                 gate_ms: int, sample_rate: int) -> dict:
    """Stage one: does this slot answer a note number with that note's pitch?

    The discriminator is deliberately not a fact about drums. `harmonic_share`
    asks how much of the render sits on the series of the key that was pressed,
    which a melodic slot answers near 1 whatever its patch layers on top and a
    kit answers near 0 because each key is a different instrument. Nothing here
    knows what a drum sounds like — the tells recorded for the kit already
    captured describe THAT kit, and a rule written from them would find only
    more of it.

    Reported as the median over the ladder: one key can fall on a patch's split
    point, and three keys do not.
    """
    shares: list[float] = []
    for note in PITCH_PROBE_NOTES:
        wav = scratch / f"ch{channel:02d}-pitch-{note}.wav"
        try:
            _render_note(replace(src, channel=channel), wav, note, velocity, gate_ms,
                         floor_peak=0.0, preroll_ms=float(src.preroll_ms),
                         sample_rate=sample_rate)
        except AuRenderError as exc:
            return {"error": str(exc)[:200]}
        audio, sr = read_wav(wav)
        mono = to_mono(audio)
        onset = int(float(src.preroll_ms) / 1000.0 * sr)
        share = harmonic_share(mono[onset:], sr, midi_to_hz(note))
        if share is not None:
            shares.append(share)
    if not shares:
        # Nothing measurable is not a kit; it is unmeasured, and stage two
        # decides.
        return {"share": [], "median_share": None, "melodic": False}
    median = float(np.median(shares))
    return {"share": [round(s, 3) for s in shares], "median_share": round(median, 3),
            "melodic": median >= MELODIC_SHARE}


def _reference_bands(ident: str) -> tuple[dict[tuple[int, int], list[float]], float | None]:
    """The committed reference's own band profiles, keyed by (note, velocity).

    Stage two compares against these rather than against a written-down list of
    what a kick looks like. The capture already holds one measured kit; what a
    second one has to resemble is that measurement.
    """
    path = HERE / "reference" / f"{ident}.json"
    if not path.exists():
        return {}, None
    profile = json.loads(path.read_text())
    rows = {(int(r["note"]), int(r["velocity"])): r.get("bands_db") or []
            for r in profile.get("rows") or []}
    return rows, (profile.get("capture") or {}).get("band_edge_hz")


def holds_a_whole_kit(distinct_peak_bands: int, notes_measured: int) -> bool:
    """Whether a slot's diagnostic hits are a kit rather than one instrument.

    Stage one has already excluded anything that plays pitches, so what is left
    to separate is a rack slot holding a whole General MIDI map from one holding
    a single drum that answers every key with the same sound. Half the notes
    landing on their own peak band is a kit; a floor of three keeps a capture
    whose families are few from reading as a kit on two notes alone.
    """
    if notes_measured <= 0:
        return False
    return distinct_peak_bands >= max(3, notes_measured // 2)


def _kit_likeness(src: AuSource, scratch: Path, channel: int, notes: tuple[int, ...],
                  velocity: int, gate_ms: int, sample_rate: int,
                  reference: dict[tuple[int, int], list[float]],
                  band_edge: float | None) -> dict:
    """Stage two: how far this slot's diagnostic hits sit from the known kit's.

    Reported as the median absolute band-profile difference in decibels, over
    one note per declared family. The capture's own timbre channel is measured
    through this same path as the control: without a number for a slot that IS
    the kit, a table of distances says nothing about what a small one means.

    Also counts how many DISTINCT peak bands the slot's notes land on, which is
    the difference between a kit and one instrument mapped across the keys. A
    distance alone cannot say: a slot holding nothing but congas sits some
    number of decibels from the kit exactly as a rival kit does, and taking it
    as a second reference would score a whole map against one drum. Stage one
    has already excluded anything that plays pitches, so a slot answering the
    diagnostic notes with one or two peak bands is a single percussion
    instrument and a slot answering with most of them is a kit.
    """
    per_note, distances = {}, []
    for note in notes:
        ref = reference.get((note, velocity))
        if not ref:
            continue
        wav = scratch / f"ch{channel:02d}-kit-{note}.wav"
        try:
            _render_note(replace(src, channel=channel), wav, note, velocity, gate_ms,
                         floor_peak=0.0, preroll_ms=float(src.preroll_ms),
                         sample_rate=sample_rate)
        except AuRenderError as exc:
            per_note[note] = {"error": str(exc)[:120]}
            continue
        audio, sr = read_wav(wav)
        mono = to_mono(audio)
        hit = analyze_hit(mono, sr, Note(note, velocity, float(src.preroll_ms) / 1000.0,
                                         gate_ms / 1000.0),
                          len(mono) / sr, max_band_hz=band_edge)
        delta = float(np.median(np.abs(np.asarray(hit.bands_db) - np.asarray(ref))))
        distances.append(delta)
        per_note[note] = {"peak_band_hz": hit.peak_band_hz,
                          "attack_ms": round(hit.attack_ms, 2),
                          "band_delta_db": round(delta, 2)}
    bands = [e["peak_band_hz"] for e in per_note.values() if e.get("peak_band_hz")]
    return {"notes": per_note,
            "band_delta_db": round(float(np.median(distances)), 2) if distances else None,
            "distinct_peak_bands": len(set(bands)),
            "notes_measured": len(bands)}


def parse_channels(spec: str) -> tuple[int, ...]:
    """`1-8,11-13,15,16` -> the channels it names, in order, without repeats."""
    out: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = (int(x) for x in part.split("-", 1))
            out.extend(range(lo, hi + 1))
        else:
            out.append(int(part))
    seen, ordered = set(), []
    for ch in out:
        if 1 <= ch <= 16 and ch not in seen:
            seen.add(ch)
            ordered.append(ch)
    return tuple(ordered)


def identify(cfg: dict, out: Path, *, channels: tuple[int, ...], velocity: int,
             verbose: bool) -> int:
    """Say what is loaded in each slot of a multitimbral rack.

    A rack answers on sixteen channels and publishes no slot names, so the only
    way to learn what is in one is to play it. This asks two questions in the
    order that makes the second cheap: does the slot play pitches (then it is
    not a kit, and no drum render is spent on it), and if it does not, how close
    are its diagnostic hits to the kit this capture already measured.

    Reports; never edits the capture definition. Which slot to add as a second
    timbre is a decision, and the evidence for it belongs in front of a person.
    """
    timbre = cfg["timbres"][0]
    base = source_for(cfg, timbre, tail="2s")
    claimed = {slot_channel(t) for t in cfg["timbres"]}
    probe = tuple(channels) or tuple(sorted(claimed))
    scratch = out / "_identify"
    scratch.mkdir(parents=True, exist_ok=True)
    gate_ms = int(cfg.get("gate_ms", 50))
    sample_rate = int(cfg["sample_rate"])
    reference, band_edge = _reference_bands(cfg["id"])
    groups = note_groups(cfg)
    # One note per declared family: a kit differs from a melodic slot across the
    # whole layout rather than on any single key, and the families are where the
    # capture already recorded which keys are different instruments.
    diagnostic = tuple(sorted({notes[0] for notes in groups.values() if notes}))

    report: dict = {"config": cfg["_path"], "plugin": cfg["plugin"],
                    "velocity": velocity, "band_edge_hz": band_edge,
                    "pitch_probe": list(PITCH_PROBE_NOTES),
                    "diagnostic_notes": list(diagnostic), "channels": {}}
    for channel in probe:
        control = channel in claimed
        if verbose:
            print(f"  ch {channel:2d}{' (control)' if control else ''}", file=sys.stderr)
        entry: dict = {"control": control, "pitch": _pitch_probe(
            base, scratch, channel, velocity, gate_ms, sample_rate)}
        if not entry["pitch"].get("melodic") and diagnostic and reference:
            entry["kit"] = _kit_likeness(base, scratch, channel, diagnostic, velocity,
                                         gate_ms, sample_rate, reference, band_edge)
        report["channels"][str(channel)] = entry

    _write(scratch / "report.json", report)
    print(f"\n{cfg['id']}: {len(probe)} slot(s), velocity {velocity}\n")
    print("  ch   pitch share   verdict        band delta   layout")
    for channel in probe:
        e = report["channels"][str(channel)]
        pitch, kit = e["pitch"], e.get("kit") or {}
        share = pitch.get("median_share")
        if pitch.get("error"):
            verdict = "did not render"
        elif pitch.get("melodic"):
            verdict = "plays pitches"
        elif kit.get("band_delta_db") is None:
            verdict = "no pitch, unscored"
        else:
            verdict = "no pitch"
        delta = kit.get("band_delta_db")
        measured = kit.get("notes_measured") or 0
        distinct = kit.get("distinct_peak_bands") or 0
        layout = "-" if not measured else (
            f"{distinct}/{measured} bands"
            f"{'  a kit' if holds_a_whole_kit(distinct, measured) else '  ONE INSTRUMENT'}")
        print(f"  {channel:2d}   {'-' if share is None else f'{share:10.3f}'}"
              f"   {verdict:<14} {'-' if delta is None else f'{delta:6.2f} dB'}"
              f"   {layout}"
              f"{'   <- the kit already captured' if e['control'] else ''}")
    print(f"\n  {scratch / 'report.json'}")
    return 0


# --------------------------------------------------------------------------
# verify


def verify(out: Path) -> int:
    """Re-read the corpus and report what a plausible file can still be wrong about."""
    manifest_path = out / "manifest.json"
    if not manifest_path.exists():
        print(f"no manifest at {manifest_path}", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text())
    renders = manifest["renders"]
    faults, doubts = [], []
    levels: dict[tuple[str, int], list[tuple[int, float]]] = {}

    for row in renders:
        path = out / row["path"]
        if not path.exists():
            faults.append(f"{row['id']}: missing")
            continue
        audio, sr = read_wav(path)
        mono = audio.mean(axis=1) if audio.ndim > 1 else audio
        peak = float(np.abs(mono).max())
        if peak <= 0.0:
            faults.append(f"{row['id']}: digital silence")
            continue
        if peak >= 0.999:
            doubts.append(f"{row['id']}: clips at {peak:.4f}")
        pre = int(manifest["preroll_ms"] * sr / 1000)
        if pre and float(np.abs(mono[:pre]).max()) > 1e-4:
            doubts.append(f"{row['id']}: energy before the note")
        # The late-render failure, auditable without re-rendering anything. See
        # ONSET_SLACK_MS: a render that starts well past the preroll is a
        # different event from the one that was asked for, and it is loud, so
        # nothing above this line can see it. A FAULT rather than a doubt —
        # the file is not the note, and a corpus carrying one produces a
        # reference profile that looks exactly like a good one.
        onset = _onset_ms(path, sr)
        if onset is not None and onset > manifest["preroll_ms"] + ONSET_SLACK_MS:
            faults.append(f"{row['id']}: begins {onset:.0f} ms in, against a "
                          f"{manifest['preroll_ms']:.0f} ms preroll — not the note")
            continue
        rms = float(np.sqrt((mono[pre:] ** 2).mean()))
        levels.setdefault((row["timbre"], row["note"]), []).append((row["velocity"], rms))

    for (timbre, note), pts in sorted(levels.items()):
        pts.sort()
        for (v0, r0), (v1, r1) in zip(pts, pts[1:]):
            if r1 < r0 * 0.98:
                doubts.append(
                    f"{timbre} n{note}: velocity {v1} is quieter than {v0} "
                    f"({20 * np.log10(max(r1, 1e-12) / max(r0, 1e-12)):+.1f} dB)"
                )

    for line in faults:
        print(f"FAULT  {line}")
    for line in doubts:
        print(f"doubt  {line}")
    print(f"\n{len(renders)} renders: {len(faults)} faults, {len(doubts)} doubts")
    return 1 if faults else 0


# --------------------------------------------------------------------------


def _write(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name, help_text in (
        ("calibrate", "measure the host settings this plugin needs"),
        ("corpus", "render the note x velocity x timbre grid"),
        ("identify", "say what is loaded in each slot of a multitimbral rack"),
        ("verify", "re-read the corpus and report what is wrong with it"),
    ):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--config", default=str(DEFAULT_CONFIG), help="capture definition JSON")
        p.add_argument("--out", default="", help=f"output root (default: {DEFAULT_OUT_ROOT}/<id>)")
        p.add_argument("--verbose", action="store_true")

    cal = sub.choices["calibrate"]
    cal.add_argument("--note", type=int, default=60, help="probe note (default: middle C)")
    cal.add_argument("--velocity", type=int, default=100)

    cor = sub.choices["corpus"]
    cor.add_argument("--no-resume", action="store_true", help="re-render everything")
    cor.add_argument("--limit", type=int, default=0, help="stop after this many renders")

    idf = sub.choices["identify"]
    idf.add_argument("--channels", default="1-16",
                     help="channels to probe, as `1-8,11-13,15,16` (default: all sixteen). "
                          "The capture's own timbre channels are the control and are "
                          "worth leaving in")
    idf.add_argument("--velocity", type=int, default=100)

    args = ap.parse_args()
    cfg = load_config(Path(args.config))
    out = out_root(cfg, args.out)

    if args.cmd == "calibrate":
        calibrate(cfg, out, note=args.note, velocity=args.velocity, verbose=args.verbose)
        return 0
    if args.cmd == "corpus":
        return corpus(cfg, out, resume=not args.no_resume, limit=args.limit, verbose=args.verbose)
    if args.cmd == "identify":
        return identify(cfg, out, channels=parse_channels(args.channels),
                        velocity=args.velocity, verbose=args.verbose)
    return verify(out)


if __name__ == "__main__":
    sys.exit(main())
