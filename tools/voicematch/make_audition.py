#!/usr/bin/env python3
"""Render the same phrases through libsonare and through a real plugin.

    rye run --pyproject bindings/python/pyproject.toml \
        python tools/voicematch/make_audition.py

Writes `<take>/model.wav` and one WAV per reference timbre into a directory
outside the repository's tracked tree, plus the `manifest.json` that
`tools/audition/serve.py` reads. Then:

    python tools/audition/serve.py <audition-dir>

Which instrument is auditioned comes from the capture definition: it names the
reference plugin and its timbres already, and `program` and `takes` say which GM
program the model should answer with and which phrase set to play. A phrase set
belongs to an instrument rather than to the tool — a harpsichord has no pedal to
lift and a piano has no stops to draw — so each is written for what its own
instrument is hard to get right.

The takes are chosen for what they can catch by ear that a metric will not
report on its own. A harmonic ladder can be matched note by note while the
instrument still sounds wrong the moment two notes overlap, or the moment a
note is struck again before it has stopped, or the moment the pedal comes up —
because those are couplings between strings rather than properties of one, and
the per-note analysis in `metrics.py` never sees them.

All versions of a take are written at one shared gain, so their level
difference survives into the comparison; the listening page has its own
loudness match for when that difference is in the way.

`--variant` adds candidate settings of the voice as further versions of every
take, each rendered under its own `SONARE_TUNING_OVERRIDES`. That is the form a
listening question usually arrives in — "is this constant better at 0 or at 4"
is not a question the metrics can settle, and the answer has to be heard against
the same phrase and the same reference. It needs a library built with
`-DBUILD_TUNING=ON`; without one the override layer is compiled out and every
variant renders identically, which the tool checks for and reports rather than
producing a page of indistinguishable versions.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT  # noqa: E402
from au_oracle import AuRenderError, render_oracle_au  # noqa: E402
from capture import load_config, source_for, CORPUS_ROOT, DEFAULT_CONFIG  # noqa: E402
from patterns import registers_for_program  # noqa: E402
from render_model import render_model  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import write_wav  # noqa: E402

SR = 48000
DEFAULT_OUT = CORPUS_ROOT / "audition"
PEDAL = 64


@dataclass
class Take:
    """One phrase, rendered once per version."""

    id: str
    label: str
    group: str
    sub: str
    notes: list[Note]
    tail_s: float = 4.0
    cc_events: tuple[tuple[float, int, int], ...] = field(default=())
    #: MIDI channel. 9 is the GM drum channel, on which a note number selects a
    #: percussion instrument rather than a pitch — the kit set needs it and
    #: nothing else does.
    channel: int = 0

    def duration(self) -> float:
        end = max((n.start + n.dur) for n in self.notes)
        return end + self.tail_s


def piano_takes(program: int = 0) -> list[Take]:
    """The piano set: one phrase per thing that is hard to hear any other way."""
    out: list[Take] = []

    out.append(Take(
        "single-c4", "Single note — C4, mf", "one note at a time",
        "attack, free decay, damper",
        [Note(60, 96, 0.3, 4.0)], tail_s=4.0,
    ))

    out.append(Take(
        "dynamics-c4", "Dynamics — C4 at five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127",
        [Note(60, v, 0.3 + i * 2.2, 1.6) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=3.5,
    ))

    out.append(Take(
        "register-sweep", "Register — A0 to C8", "one note at a time",
        "A0 C2 C4 C6 C8, vel 96",
        [Note(n, 96, 0.3 + i * 2.6, 2.0) for i, n in enumerate((21, 36, 60, 84, 108))],
        tail_s=4.0,
    ))

    # Two strings a third apart beat against each other in a way that depends on
    # every partial being in the right place, not just the fundamental. A model
    # with the right harmonic ladder and the wrong inharmonicity passes note by
    # note and falls apart here.
    out.append(Take(
        "chord-sustained", "Chord — C major triad held", "notes together",
        "C3 E3 G3, vel 88, 6 s",
        [Note(n, 88, 0.3, 6.0) for n in (48, 52, 55)], tail_s=4.0,
    ))

    out.append(Take(
        "arpeggio-legato", "Arpeggio — overlapping, no pedal", "notes together",
        "C3 E3 G3 C4 E4 G4 C5, each held past the next",
        [Note(n, 84, 0.3 + i * 0.42, 1.9)
         for i, n in enumerate((48, 52, 55, 60, 64, 67, 72))],
        tail_s=4.0,
    ))

    # The pedal is the piano behaviour a physical model is most likely to be
    # missing entirely: it lifts every damper, so the strings that were never
    # struck ring in sympathy with the ones that were.
    out.append(Take(
        "pedal-resonance", "Pedal — staccato notes under a held pedal", "the pedal",
        "CC64 down at 0.2 s, up at 7.0 s",
        [Note(n, 92, 0.5 + i * 0.8, 0.12)
         for i, n in enumerate((36, 43, 48, 52, 55, 60, 64, 67))],
        tail_s=4.0,
        cc_events=((0.2, PEDAL, 127), (7.0, PEDAL, 0)),
    ))

    out.append(Take(
        "repeated-note", "Repeated note — eight strikes on a ringing string", "the pedal",
        "C4 vel 100, 190 ms apart",
        [Note(60, 100, 0.3 + i * 0.19, 0.14) for i in range(8)], tail_s=4.0,
    ))

    # Something to just listen to. Everything above is a probe; this is the one
    # that says whether the result is an instrument.
    phrase = [
        (60, 100, 0.00, 0.85), (64, 88, 0.85, 0.85), (67, 92, 1.70, 0.85),
        (72, 104, 2.55, 1.30), (71, 84, 3.85, 0.60), (67, 80, 4.45, 0.60),
        (64, 76, 5.05, 1.60),
        (36, 78, 0.00, 1.70), (43, 70, 1.70, 1.70), (41, 72, 3.40, 1.70),
        (36, 74, 5.05, 1.60),
    ]
    out.append(Take(
        "phrase-ballad", "Phrase — melody over a bass line, pedalled", "a phrase",
        "with the pedal changed on each bass note",
        [Note(n, v, 0.4 + s, d) for n, v, s, d in phrase], tail_s=5.0,
        cc_events=(
            (0.45, PEDAL, 127), (2.05, PEDAL, 0), (2.15, PEDAL, 127),
            (3.75, PEDAL, 0), (3.85, PEDAL, 127), (5.40, PEDAL, 0),
            (5.50, PEDAL, 127), (7.20, PEDAL, 0),
        ),
    ))
    return out


def harpsichord_takes(program: int = 6) -> list[Take]:
    """The harpsichord set.

    Almost nothing the piano set probes transfers. There is no pedal to lift and
    no una corda; what there is instead is a mechanism that refuses to obey the
    key, and a top octave that used to stop sounding long before it should.
    """
    out: list[Take] = []

    out.append(Take(
        "single-c4", "Single note - C4, mf", "one note at a time",
        "pluck, free decay, damper on release",
        [Note(60, 96, 0.3, 4.0)], tail_s=3.0,
    ))

    # The instrument's defining trait, and the one a listener catches in a
    # second: five velocities across the whole MIDI range should arrive at very
    # nearly the same loudness. A model that spends a dynamic range here is not
    # a harpsichord however good its single note sounds.
    out.append(Take(
        "dynamics-c4", "Dynamics - C4 at five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127, and they should barely differ",
        [Note(60, v, 0.3 + i * 1.7, 1.2) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=3.0,
    ))

    # A harpsichord's peak level is set by the plectrum rather than by the
    # string, so the compass should be flat. A register-balance error is
    # inaudible note by note and obvious the moment the notes are played in a
    # row.
    out.append(Take(
        "register-sweep", "Register - the whole compass", "one note at a time",
        "F1 C2 C3 C4 C5 C6 F6, vel 96",
        [Note(n, 96, 0.3 + i * 1.5, 1.2)
         for i, n in enumerate((29, 36, 48, 60, 72, 84, 89))],
        tail_s=3.5,
    ))

    # The treble is where the engine this replaced fell apart: the note died in
    # a fifth of a second. Held four seconds, alone, with nothing to hide it.
    out.append(Take(
        "treble-sustain", "Treble - the top note held four seconds", "one note at a time",
        "the top of the compass has to keep sounding",
        [Note(89, 96, 0.3, 4.0)], tail_s=3.0,
    ))

    out.append(Take(
        "chord-sustained", "Chord - C major triad held", "notes together",
        "C3 E3 G3, vel 88, 5 s",
        [Note(n, 88, 0.3, 5.0) for n in (48, 52, 55)], tail_s=3.5,
    ))

    # Baroque keyboard writing is mostly this: overlapping lines on an
    # instrument with no pedal, where every note stops when its key does.
    out.append(Take(
        "arpeggio-legato", "Arpeggio - overlapping, held past each other", "notes together",
        "C3 E3 G3 C4 E4 G4 C5, each held past the next",
        [Note(n, 84, 0.3 + i * 0.34, 1.5)
         for i, n in enumerate((48, 52, 55, 60, 64, 67, 72))],
        tail_s=3.5,
    ))

    # The ornament the instrument is played with, fast enough that the damper's
    # speed and the jack's reset decide whether it reads as notes at all.
    out.append(Take(
        "trill", "Trill - C4 to D4, sixteen notes", "the mechanism",
        "each key released before the next, 90 ms apart",
        [Note(60 if i % 2 == 0 else 62, 92, 0.3 + i * 0.09, 0.075) for i in range(16)],
        tail_s=3.0,
    ))

    # Release is a mechanism, not a fade: the jack drops, the tongue pivots past
    # the string without plucking it again, and the felt lands.
    out.append(Take(
        "staccato-release", "Staccato - eight short notes", "the mechanism",
        "the jack and the damper are the sound between the notes",
        [Note(n, 96, 0.3 + i * 0.55, 0.10)
         for i, n in enumerate((48, 55, 60, 64, 67, 64, 60, 55))],
        tail_s=3.0,
    ))

    # Something to just listen to, as the piano set has.
    out.append(Take(
        "phrase-baroque", "Phrase - two voices", "a phrase",
        "a right hand over a walking bass, no pedal anywhere",
        [Note(n, v, 0.4 + st, d) for n, v, st, d in (
            (72, 96, 0.00, 0.38), (71, 88, 0.38, 0.38), (69, 90, 0.76, 0.38),
            (67, 88, 1.14, 0.38), (65, 92, 1.52, 0.38), (64, 88, 1.90, 0.38),
            (62, 90, 2.28, 0.38), (60, 96, 2.66, 0.90),
            (64, 88, 3.60, 0.38), (65, 86, 3.98, 0.38), (67, 92, 4.36, 0.76),
            (72, 98, 5.12, 1.20),
            (48, 88, 0.00, 0.72), (55, 82, 0.76, 0.72), (53, 84, 1.52, 0.72),
            (52, 82, 2.28, 0.72), (48, 86, 3.04, 0.72), (50, 82, 3.80, 0.72),
            (52, 84, 4.56, 0.52), (48, 90, 5.12, 1.20),
        )], tail_s=3.5,
    ))
    return out


def drum_takes(program: int = 0) -> list[Take]:
    """The kit set: what a drum part is, which single hits are not.

    The single-hit probe strikes one instrument every two seconds, which is the
    right stimulus for measuring one drum and the wrong one for hearing a kit.
    Everything below lives in the relation between hits, and a fit that never
    plays two hits close together cannot produce any of it — not because the
    search failed but because the question was never put.

    Written on the drum channel, so a note number selects an instrument. The
    take set is `analysis_notes`-free by nature: every per-hit measurement
    assumes an isolated strike.
    """
    out: list[Take] = []

    # The mute group. 42, 44 and 46 share a GM exclusive class, so an open hat
    # left ringing is cut the instant a closed one or the pedal arrives. That is
    # the pedal, it is most of what a hi-hat part sounds like, and no probe in
    # this harness has ever fired it.
    out.append(Take(
        "hh-choke", "Hi-hat — open, choked, open, pedal", "the mute group",
        "46 open / 42 closed / 46 open / 44 pedal",
        [Note(46, 110, 0.3, 0.05), Note(42, 100, 0.8, 0.05),
         Note(46, 110, 1.8, 0.05), Note(44, 90, 2.3, 0.05)], tail_s=4.0, channel=9,
    ))

    # At speed each strike lands on the last one's ring. A voice that sounds
    # right alone and identical every time reads as a machine here, which is
    # what a fixed one-shot with no round robin and no choke sounds like.
    out.append(Take(
        "hh-sixteenths", "Hi-hat — sixteenths at 120", "at speed",
        "42 closed, accented on the beat",
        [Note(42, 112 if i % 4 == 0 else 96, 0.3 + i * 0.125, 0.05)
         for i in range(16)], tail_s=2.5, channel=9,
    ))

    # Two strikes close enough to fuse into one event. The ear hears a flam as
    # thickness rather than as two notes, and whether it does depends entirely
    # on what the second strike finds the first one doing.
    out.append(Take(
        "snare-flam", "Snare — flams and a roll", "at speed",
        "grace note 28 ms ahead, then a 12-stroke roll",
        [Note(38, 60, 0.3, 0.05), Note(38, 110, 0.328, 0.05),
         Note(38, 60, 1.3, 0.05), Note(38, 110, 1.328, 0.05)]
        + [Note(38, 90, 2.4 + i * 0.06, 0.05) for i in range(12)], tail_s=3.0, channel=9,
    ))

    # The tom fill is where the kit's tuning is audible as a kit rather than as
    # six separate instruments. The order is the MEASURED pitch order of the
    # capture, which is not the order General MIDI names the keys in.
    out.append(Take(
        "tom-fill", "Toms — a descending fill", "the kit as a kit",
        "43 41 50 48 47 45, the capture's measured pitch order, high to low",
        [Note(n, 104, 0.3 + i * 0.22, 0.05)
         for i, n in enumerate((43, 41, 50, 48, 47, 45))], tail_s=3.5, channel=9,
    ))

    # A cymbal's wash is four to ten seconds and every measurement in this
    # harness stops at 1.8. This is the take that hears what none of them reach.
    out.append(Take(
        "cymbal-wash", "Cymbals — crash and ride, let ring", "the long tail",
        "49 crash, then 51 ride, ten seconds",
        [Note(49, 120, 0.3, 0.05), Note(51, 100, 3.0, 0.05)], tail_s=10.0, channel=9,
    ))

    out.append(Take(
        "groove", "Groove — a bar of eights", "the kit as a kit",
        "kick, snare, closed hat, 100 bpm",
        [Note(42, 92 if i % 2 else 104, 0.3 + i * 0.3, 0.05) for i in range(8)]
        + [Note(36, 110, 0.3, 0.05), Note(36, 100, 1.5, 0.05)]
        + [Note(38, 108, 0.9, 0.05), Note(38, 108, 2.1, 0.05)], tail_s=3.5, channel=9,
    ))
    return out


def sustained_takes(program: int = 40) -> list[Take]:
    """The set for anything bowed or blown: what happens BETWEEN notes.

    The whole harness measures isolated held notes, and on a wind or a bowed
    string that is the least characteristic thing the instrument does. Tonguing,
    a bow change, the join in a slur and the shape of a swell are where the
    playing is, and none of them exists inside one note.

    Generic across the family rather than per instrument: the register comes
    from the capture's own program, so this set is a shape and the notes are
    filled in against `registers_for_program`.
    """
    lo, mid, hi = registers_for_program(program)
    out: list[Take] = []

    out.append(Take(
        "single-long", "Single note — held six seconds", "one note at a time",
        "the vibrato, if there is one, and how it starts",
        [Note(mid, 96, 0.3, 6.0)], tail_s=2.5,
    ))

    # A slur is one continuous sound with the pitch changed inside it. A model
    # that retriggers its excitation on every note-on produces a series of
    # separate notes, which is audible immediately and invisible to every
    # measurement here.
    out.append(Take(
        "legato-scale", "Legato — a scale, slurred", "between notes",
        "overlapping note-ons, no gap anywhere",
        [Note(mid + d, 92, 0.3 + i * 0.38, 0.44)
         for i, d in enumerate((0, 2, 4, 5, 7, 9, 11, 12))], tail_s=2.5,
    ))

    out.append(Take(
        "tongued", "Repeated notes — separated", "between notes",
        "the same pitch eight times, each articulated",
        [Note(mid, 100, 0.3 + i * 0.35, 0.22) for i in range(8)], tail_s=2.5,
    ))

    out.append(Take(
        "swell", "Swell — one note from nothing and back", "one note at a time",
        "expression from 0 to 127 and back over 6 s",
        [Note(mid, 100, 0.3, 6.0)], tail_s=2.5,
        cc_events=tuple((0.3 + i * 0.25, 11, v) for i, v in enumerate(
            list(range(0, 128, 11)) + list(range(127, -1, -11)))),
    ))

    out.append(Take(
        "leap", "Leaps — across the compass", "between notes",
        "low, high, low, slurred",
        [Note(n, 96, 0.3 + i * 0.6, 0.7)
         for i, n in enumerate((lo, hi, mid, hi, lo))], tail_s=3.0,
    ))
    return out


#: Phrase sets by name. A capture definition names the one its instrument needs.
TAKE_SETS = {
    "piano": piano_takes,
    "harpsichord": harpsichord_takes,
    "drums": drum_takes,
    "sustained": sustained_takes,
}


#: Renders one SMF in a fresh interpreter. The tuning override table is read
#: when the library loads, so two settings of the same constant cannot be
#: rendered by one process -- the second would silently get the first's values.
_VARIANT_WORKER = r'''
import sys
import numpy as np
sys.path.insert(0, "tools"); sys.path.insert(0, "tools/voicematch")
from render_model import render_model
smf, out, seconds, sr = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
with open(smf, "rb") as fh:
    a = np.asarray(render_model(fh.read(), seconds, sr), dtype=np.float32)
np.save(out, a.mean(axis=1) if a.ndim > 1 else a)
'''


def parse_variants(specs: list[str]) -> list[tuple[str, str]]:
    """`name=overrides` pairs, in the order given.

    The name is the source key the page shows and the file stem on disk, so it
    is restricted to what is safe in both; the overrides are passed through
    untouched, since the library is the only thing that can say whether a key
    exists.
    """
    out: list[tuple[str, str]] = []
    for spec in specs:
        name, sep, overrides = spec.partition("=")
        name = name.strip()
        if not sep or not name:
            raise SystemExit(f"--variant wants name=overrides, got {spec!r}")
        if not all(c.isalnum() or c in "._-" for c in name):
            raise SystemExit(f"--variant name may only hold letters, digits, . _ - : {name!r}")
        if name == "model":
            raise SystemExit("--variant name 'model' is taken by the unmodified voice")
        out.append((name, overrides.strip()))
    return out


def render_variant(smf: bytes, seconds: float, sr: int, overrides: str,
                   lib_path: str = "") -> np.ndarray:
    """One take under one override set, in its own interpreter."""
    env = dict(os.environ)
    if lib_path:
        env["SONARE_LIB_PATH"] = lib_path
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    else:
        env.pop("SONARE_TUNING_OVERRIDES", None)
    with tempfile.TemporaryDirectory() as tmp:
        smf_path = Path(tmp) / "take.mid"
        smf_path.write_bytes(smf)
        out_path = Path(tmp) / "render.npy"
        proc = subprocess.run(
            [sys.executable, "-c", _VARIANT_WORKER, str(smf_path), str(out_path),
             str(seconds), str(sr)],
            capture_output=True, text=True, env=env, cwd=str(REPO_ROOT))
        if proc.returncode:
            raise RuntimeError(proc.stderr[-4000:])
        return np.load(out_path)


def shared_gain(renders: dict[str, np.ndarray], headroom_db: float = -1.0) -> float:
    """One gain for every version of a take, so their level difference survives.

    Normalising each version on its own would erase exactly the thing a
    register-balance or velocity-curve problem shows up as.
    """
    peak = max((float(np.abs(a).max()) for a in renders.values() if a.size), default=0.0)
    if peak <= 0.0:
        return 1.0
    return float(10.0 ** (headroom_db / 20.0) / peak)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=str(DEFAULT_CONFIG),
                    help="capture definition naming the reference plugin and its timbres")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--timbres", default="",
                    help="comma-separated timbre ids to render (default: all in the config)")
    ap.add_argument("--model-only", action="store_true",
                    help="skip the reference renders and audition the model against itself")
    ap.add_argument("--only", default="", help="comma-separated take ids")
    ap.add_argument("--takes", default="",
                    help=f"phrase set: {'/'.join(TAKE_SETS)} (default: the config's)")
    ap.add_argument("--program", type=int, default=None,
                    help="GM program the model answers with (default: the config's)")
    ap.add_argument("--variant", action="append", default=[], metavar="NAME=OVERRIDES",
                    help="an extra version of every take, rendered under this "
                         "SONARE_TUNING_OVERRIDES string; repeatable")
    ap.add_argument("--lib", default="",
                    help="library the variants load (a -DBUILD_TUNING=ON build); "
                         "sets SONARE_LIB_PATH for them")
    args = ap.parse_args()
    variants = parse_variants(args.variant)

    cfg = load_config(Path(args.config))
    out = Path(args.out).expanduser().resolve()
    wanted = [t.strip() for t in args.timbres.split(",") if t.strip()]
    timbres = [t for t in cfg["timbres"] if not wanted or t["id"] in wanted]
    if args.model_only:
        timbres = []
    only = {t.strip() for t in args.only.split(",") if t.strip()}

    # Which instrument is being auditioned belongs to the capture definition,
    # which already names the reference plugin it is being compared against.
    program = args.program if args.program is not None else int(cfg.get("program", 0))
    # `or` rather than a get() default at each step: `load_config` declares
    # `takes` and fills it with an empty string, so a key-missing default would
    # never fire anyway. There is deliberately no fallback set — an instrument
    # with no phrases of its own would otherwise be auditioned on the piano's,
    # which renders and plays and looks like a successful comparison of the
    # wrong music.
    set_name = args.takes or cfg.get("takes")
    if set_name not in TAKE_SETS:
        named = f"{set_name!r}" if set_name else "no phrase set"
        print(f"the capture names {named}; have {', '.join(TAKE_SETS)}", file=sys.stderr)
        return 2

    selected = [t for t in TAKE_SETS[set_name](program) if not only or t.id in only]
    # `role` splits the page's version switch into a model row and a reference
    # row. Seven versions of a take is an ordinary number once a couple of
    # candidate settings are in play, and as one undifferentiated strip of
    # buttons it takes reading every label to find which side of the comparison
    # a version is on -- which is the one thing the page should never make
    # anybody work out.
    sources = {"model": {
        "label": "libsonare NativeSynth (GM fallback)",
        "role": "model",
        "detail": "the library as it stands, no overrides",
    }}
    for name, overrides in variants:
        sources[name] = {
            "label": f"libsonare NativeSynth (GM fallback), {name}",
            "role": "model",
            "detail": overrides or "no overrides",
        }
    for t in timbres:
        sources[t["id"]] = {
            "label": t["label"],
            "role": "reference",
            "detail": cfg["label"].split(",")[0],
        }

    items = []
    variant_digests: dict[str, set[str]] = {}
    for take in selected:
        total = take.duration()
        smf = write_smf(
            take.notes, program=program, end_pad=take.tail_s,
            cc_events=take.cc_events, channel=take.channel,
        )
        print(f"== {take.id} ({total:.1f}s) ==", file=sys.stderr)

        renders: dict[str, np.ndarray] = {}
        # `--lib` has to reach the unmodified voice as well as the variants.
        # Rendering it in-process instead would take whichever library the
        # loader prefers, so a page meant to compare four settings of one
        # constant would be comparing two builds -- and the difference between
        # two build trees is invisible on a listening page and reads as tuning.
        renders["model"] = (render_variant(smf, total, SR, "", args.lib) if args.lib
                            else render_model(smf, total, SR))
        print("  model", file=sys.stderr)

        for name, overrides in variants:
            audio = render_variant(smf, total, SR, overrides, args.lib)
            renders[name] = audio
            # Hashed rather than compared pairwise: what has to be caught is a
            # library with the override layer compiled out, where EVERY variant
            # is the same render, and a set of digests says that in one look.
            variant_digests.setdefault(take.id, set()).add(
                hashlib.sha256(np.ascontiguousarray(audio, dtype=np.float32)
                               .tobytes()).hexdigest())
            print(f"  {name}", file=sys.stderr)

        for timbre in timbres:
            # Built through the same helper the capture path uses, so a timbre
            # selected by channel rather than by preset -- a slot of a
            # multitimbral rack -- reaches the plugin here too.
            source = source_for(cfg, timbre, tail=f"{take.tail_s:.0f}s", sample_rate=SR)
            try:
                renders[timbre["id"]] = render_oracle_au(smf, total, SR, source=source)
                print(f"  {timbre['id']}", file=sys.stderr)
            except (AuRenderError, FileNotFoundError) as exc:
                print(f"  {timbre['id']}: SKIPPED — {exc}", file=sys.stderr)

        gain = shared_gain(renders)
        tracks = {}
        for key, audio in renders.items():
            rel = Path(take.id) / f"{key}.wav"
            (out / rel).parent.mkdir(parents=True, exist_ok=True)
            write_wav(out / rel, np.clip(audio * gain, -1.0, 1.0), SR)
            tracks[key] = str(rel)

        items.append({
            "id": take.id,
            "label": take.label,
            "sub": take.sub,
            "group": take.group,
            "tracks": tracks,
            "meta": {
                "seconds": round(total, 2),
                "shared_gain_db": round(float(20 * np.log10(max(gain, 1e-9))), 2),
                "program": program,
                "pedal": bool(take.cc_events),
                # The schedule, so a take can say where its own measurement
                # windows are. Anything reading these files otherwise has to
                # place a window by eye against a phrase it cannot see, and a
                # window placed by eye lands in the wrong place: the pedal
                # take's resonance lives between the last note-off and the
                # pedal lifting, and a window a little further on measures the
                # dampers landing instead -- the opposite mechanism, at the
                # other end of the same take.
                "notes": [{"note": n.note, "velocity": n.velocity,
                           "start": round(n.start, 4), "duration": round(n.dur, 4)}
                          for n in take.notes],
                "cc": [[round(t, 4), int(cc), int(v)] for t, cc, v in take.cc_events],
            },
        })

    dry = bool(cfg.get("dry", True))
    manifest = {
        "title": cfg.get("audition_title", f"libsonare vs {cfg.get('label', 'reference')}"),
        "notes": (
            "Every version of a take is written at one shared gain, so the level "
            "difference between them is real. "
            + ("The reference is captured dry — every effect section of the plugin is "
               "switched off — so what is being compared is the instrument and not a room."
               if dry else
               "The reference is NOT captured dry: this one carries effects of its own "
               "that cannot be switched off per slot, so part of what is heard on the "
               "reference side is its room.")
        ),
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "sources": sources,
        "items": items,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n{len(items)} takes -> {out}", file=sys.stderr)
    # A page whose variants are all the same render looks exactly like a page
    # whose variants are subtly different, and the difference is a build flag
    # nobody sees. Say it here rather than let it be listened to.
    if len(variants) > 1:
        identical = [tid for tid, d in variant_digests.items() if len(d) == 1]
        if len(identical) == len(variant_digests):
            print(f"WARNING: all {len(variants)} variants rendered identically on every "
                  f"take.\n         The library has no tuning override layer -- rebuild "
                  f"it with -DBUILD_TUNING=ON,\n         or point --lib at one that has.",
                  file=sys.stderr)
        elif identical:
            print(f"note: {len(identical)} take(s) render identically across the "
                  f"variants: {', '.join(sorted(identical))}", file=sys.stderr)
    if out.is_relative_to(CORPUS_ROOT):
        print("listen:  python tools/audition/serve.py"
              "   (this set is under the scratch root, so it is found with no argument)",
              file=sys.stderr)
    else:
        print(f"listen:  python tools/audition/serve.py {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
