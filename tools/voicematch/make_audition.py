#!/usr/bin/env python3
"""Render the same piano phrases through libsonare and through a real plugin.

    rye run --pyproject bindings/python/pyproject.toml \
        python tools/voicematch/make_audition.py

Writes `<take>/model.wav` and one WAV per reference timbre into a directory
outside the repository's tracked tree, plus the `manifest.json` that
`tools/audition/serve.py` reads. Then:

    python tools/audition/serve.py <audition-dir>

The takes are chosen for what they can catch by ear that a metric will not
report on its own. A harmonic ladder can be matched note by note while the
instrument still sounds wrong the moment two notes overlap, or the moment a
note is struck again before it has stopped, or the moment the pedal comes up —
because those are couplings between strings rather than properties of one, and
the per-note analysis in `metrics.py` never sees them.

All versions of a take are written at one shared gain, so their level
difference survives into the comparison; the listening page has its own
loudness match for when that difference is in the way.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from au_oracle import AuRenderError, AuSource, render_oracle_au  # noqa: E402
from capture import config_params, load_config, CORPUS_ROOT, DEFAULT_CONFIG  # noqa: E402
from render_model import render_model  # noqa: E402
from smf import Note, write_smf  # noqa: E402
from wavio import write_wav  # noqa: E402

SR = 48000
PIANO_PROGRAM = 0  # GM Acoustic Grand Piano
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

    def duration(self) -> float:
        end = max((n.start + n.dur) for n in self.notes)
        return end + self.tail_s


def takes() -> list[Take]:
    """The audition set: one phrase per thing that is hard to hear any other way."""
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
    args = ap.parse_args()

    cfg = load_config(Path(args.config))
    out = Path(args.out).expanduser().resolve()
    wanted = [t.strip() for t in args.timbres.split(",") if t.strip()]
    timbres = [t for t in cfg["timbres"] if not wanted or t["id"] in wanted]
    if args.model_only:
        timbres = []
    only = {t.strip() for t in args.only.split(",") if t.strip()}

    selected = [t for t in takes() if not only or t.id in only]
    sources = {"model": {"label": "libsonare NativeSynth (GM fallback)"}}
    for t in timbres:
        sources[t["id"]] = {"label": f"{cfg['label'].split(',')[0]} — {t['label']}"}

    items = []
    for take in selected:
        total = take.duration()
        smf = write_smf(
            take.notes, program=PIANO_PROGRAM, end_pad=take.tail_s,
            cc_events=take.cc_events,
        )
        print(f"== {take.id} ({total:.1f}s) ==", file=sys.stderr)

        renders: dict[str, np.ndarray] = {}
        renders["model"] = render_model(smf, total, SR)
        print("  model", file=sys.stderr)

        for timbre in timbres:
            source = AuSource(
                plugin=cfg["plugin"],
                preset=timbre["preset"],
                params=config_params(cfg),
                settle_ms=int(cfg["settle_ms"]),
                realtime=bool(cfg["realtime"]),
                preroll_ms=int(cfg["preroll_ms"]),
                tail=f"{take.tail_s:.0f}s",
                sample_rate=SR,
            )
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
                "program": PIANO_PROGRAM,
                "pedal": bool(take.cc_events),
            },
        })

    manifest = {
        "title": "libsonare piano vs The Grand 3",
        "notes": (
            "Every version of a take is written at one shared gain, so the level "
            "difference between them is real. The reference is captured dry — every "
            "effect section of the plugin is switched off — so what is being compared "
            "is the instrument and not a room."
        ),
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "sources": sources,
        "items": items,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\n{len(items)} takes -> {out}", file=sys.stderr)
    print(f"listen:  python tools/audition/serve.py {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
