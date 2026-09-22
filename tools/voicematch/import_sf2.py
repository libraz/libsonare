"""Import a SoundFont's recordings as a voicematch corpus.

`capture.py corpus` records a plugin by playing it; this records a SoundFont by
reading it. Everything after that is the same — the same manifest, the same
directory of one WAV per (note, velocity), so `profile`, `compare`, `autofit`
and `diagnose` read the result without knowing which produced it.

The capture definition is the same two files as well: the tracked
`capture/<id>.json` holds the method, and the untracked `capture/<id>.local.json`
names the SoundFont and which of its presets answers each timbre. A font full of
one product's recordings is that product, so its name belongs in the half that
is not published — the same rule, for the same reason, as a plugin triple.

**Nothing here resamples, retimes, or invents a velocity.** Three refusals carry
that, and each exists because the thing it prevents is silent:

- a font that needs a player is refused by `sf2.require_corpus`;
- a grid asking for more velocities than the font has layers is refused, because
  writing one recording out under five velocity labels manufactures the axis a
  dynamics fit reads and every metric would still compute;
- a grid asking for a longer gate than the font recorded is refused, naming the
  longest it can serve, because a short window silently truncates a decay.

Usage:

    python3 tools/voicematch/import_sf2.py tools/voicematch/capture/<id>.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))

from capture import (
    DEFAULT_OUT_ROOT,
    ROOM_UNCLASSIFIED,
    load_config,
    note_groups,
    note_map,
)
from sf2 import SoundFont, UnplayableAsCorpus
from wavio import write_wav


class ImportRefused(ValueError):
    """The grid and the font disagree about something that cannot be papered over."""


def _resolve_font(cfg: dict, timbre: dict) -> tuple[Path, str]:
    """The SoundFont file and preset name that answer one timbre.

    Both come from the untracked overlay. Their absence is the ordinary state of
    a fresh clone and is reported as such rather than as a corruption.
    """
    name = timbre.get("preset")
    file = timbre.get("sf2") or cfg.get("sf2")
    root = cfg.get("soundfont_dir", "")
    if not name or not file:
        raise ImportRefused(
            f"timbre {timbre.get('id', '?')!r} names no SoundFont preset. The font and "
            f"the preset live in the untracked {Path(cfg['_path']).stem}.local.json — "
            f'a timbre block of {{"id": ..., "sf2": "FAMILY.sf2", "preset": "Tone Name"}}, '
            f"with `soundfont_dir` beside it"
        )
    path = Path(root).expanduser() / file if root else Path(file).expanduser()
    if not path.exists():
        raise ImportRefused(f"{path} does not exist")
    return path, name


def _preset(font: SoundFont, name: str) -> object:
    """The preset called `name`, refusing an ambiguous or absent one by name.

    Resolved by name rather than by (bank, program) because a font whose presets
    were flattened into one bank carries the tone list in its names and nothing
    else: the numbers there are positions, and a position silently means a
    different instrument in the next font of the set.
    """
    hits = [p for p in font.presets if p.name == name]
    if not hits:
        near = [p.name for p in font.presets if name.lower() in p.name.lower()][:6]
        raise ImportRefused(
            f"{font.path.name} has no preset called {name!r}"
            + (f" — did you mean {near}?" if near else f" (it has {len(font.presets)} presets)")
        )
    if len(hits) > 1:
        raise ImportRefused(
            f"{font.path.name} has {len(hits)} presets called {name!r}, so a name "
            f"cannot select one of them"
        )
    return hits[0]


def _check_velocity_axis(cfg: dict, font: SoundFont) -> int:
    """The one velocity a layer-less font may be written under.

    A SoundFont with no `velRange` holds one recording per note, taken at a
    velocity it does not record. Writing it out under several velocity labels
    would put identical audio in every row of the velocity axis, which reads as
    a perfectly flat dynamic response rather than as a missing measurement.
    """
    velocities = list(cfg.get("velocities") or ())
    if len(velocities) != 1:
        raise ImportRefused(
            f"{cfg['id']}: the font declares no velocity layers, so it holds one "
            f"recording per note at a velocity it does not state. This grid asks for "
            f"{len(velocities)} velocities ({velocities}), which would write the same "
            f"audio into every one of them. Declare a single velocity, and excuse "
            f"`vel_range` and `dynamics` in `dimensions_na` with that as the reason"
        )
    return int(velocities[0])


def _check_gate(cfg: dict, found: dict[str, dict[int, float]]) -> None:
    """Refuse a gate longer than the shortest recording it would be read from."""
    gate_s = float(cfg["gate_ms"]) / 1000.0
    short = [
        (tid, note, seconds)
        for tid, notes in found.items()
        for note, seconds in sorted(notes.items())
        if seconds < gate_s
    ]
    if short:
        worst = min(s for _t, _n, s in short)
        listing = ", ".join(f"{t}/n{n:03d} {s:.2f}s" for t, n, s in short[:6])
        raise ImportRefused(
            f"{cfg['id']}: the grid declares a {gate_s:.1f} s gate and "
            f"{len(short)} of the recordings are shorter — {listing}"
            + (" ..." if len(short) > 6 else "")
            + f". A gate is what the analysis window is promised to be, so lower "
            f"`gate_ms` to at most {int(worst * 1000)} or drop the notes that cannot "
            f"hold it. Nothing here pads a recording to a gate it does not fill"
        )


def import_corpus(cfg: dict, out: Path, *, verbose: bool = False) -> dict:
    """Write one capture definition's grid out of its SoundFont(s)."""
    velocity = None
    notes = [int(n) for n in cfg["notes"]]
    renders: list[dict] = []
    found: dict[str, dict[int, float]] = {}
    missing: dict[str, list[int]] = {}
    rates: set[int] = set()
    fonts: dict[Path, SoundFont] = {}

    try:
        for timbre in cfg["timbres"]:
            tid = timbre["id"]
            path, preset_name = _resolve_font(cfg, timbre)
            if path not in fonts:
                font = SoundFont(path)
                font.require_corpus()
                fonts[path] = font
            font = fonts[path]
            if velocity is None:
                velocity = _check_velocity_axis(cfg, font)

            available = font.note_map(_preset(font, preset_name))
            found[tid], missing[tid] = {}, []
            for note in notes:
                sample = available.get(note)
                if sample is None:
                    missing[tid].append(note)
                    continue
                audio, rate = font.extract(sample)
                rates.add(rate)
                rel = Path(tid) / f"n{note:03d}_v{velocity:03d}.wav"
                (out / rel).parent.mkdir(parents=True, exist_ok=True)
                write_wav(out / rel, audio, rate)
                found[tid][note] = sample.seconds
                renders.append(
                    {
                        "id": f"{tid}/n{note:03d}_v{velocity:03d}",
                        "timbre": tid,
                        "note": note,
                        "velocity": velocity,
                        "path": str(rel),
                        "peak": float(np.max(np.abs(audio))) if audio.size else 0.0,
                        "seconds": sample.seconds,
                        # There is no lead-in: an extracted recording begins at its
                        # own first frame, which is why `preroll_ms` is held at 0.
                        "preroll_peak": 0.0,
                        "onset_ms": 0.0,
                        "attempts": 1,
                        "sf2_sample": sample.name,
                    }
                )
                if verbose:
                    print(
                        f"  {tid}/n{note:03d} <- {sample.name!r} ({sample.seconds:.2f}s @ {rate})",
                        file=sys.stderr,
                    )

        if not renders:
            raise ImportRefused(f"{cfg['id']}: no note of the grid has a recording")
        if len(rates) > 1:
            raise ImportRefused(
                f"{cfg['id']}: the recordings are at {sorted(rates)} Hz. One corpus "
                f"carries one rate, and resampling here would make the reference this "
                f"importer's output rather than the source's"
            )
        if int(cfg["preroll_ms"]) != 0:
            raise ImportRefused(
                f"{cfg['id']}: an extracted recording begins at its own first frame, "
                f"so the capture has to declare `preroll_ms: 0`. It declares "
                f"{cfg['preroll_ms']} — every consumer that strips a preroll would cut "
                f"{cfg['preroll_ms']} ms off the attack"
            )
        _check_gate(cfg, found)
    finally:
        for font in fonts.values():
            font.close()

    manifest = {
        "id": cfg["id"],
        "config": cfg["_path"],
        # Deliberately no `plugin`: nothing played this. A reader that wants one
        # should fail loudly here rather than read a placeholder as a product.
        "source": {"kind": "sf2", "class": cfg.get("source_class")},
        "sample_rate": rates.pop(),
        "gate_ms": cfg["gate_ms"],
        "tail": "0s",
        "preroll_ms": 0,
        "onset_slack_ms": cfg["onset_slack_ms"],
        "keyswitch_lead_ms": 0,
        "settle_ms": 0,
        "realtime": False,
        "warmup": False,
        "params": [],
        "sends": None,
        "timbres": list(cfg["timbres"]),
        "notes": notes,
        "velocities": [velocity],
        "groups": note_groups(cfg),
        "note_map": note_map(cfg),
        "rig": cfg["rig"],
        "room": cfg["room"],
        # Reach, as an output. A note the font does not hold at its own pitch is
        # not extracted, so "no divergence found" on a program has to be readable
        # against how much of the grid was ever compared.
        "reach": {
            "requested": len(cfg["timbres"]) * len(notes),
            "extracted": len(renders),
            "missing": {t: v for t, v in missing.items() if v},
        },
        "renders": sorted(renders, key=lambda r: r["id"]),
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def check_module_is_dry(cfg: dict, manifest: dict, out: Path) -> int:
    """The measured half of `source_class: module` + `room: present`.

    A module is recorded with reverb, chorus, delay and the insertion effect all
    off, so a module corpus that measures a space is one of two things — a module
    left with its effects on, or a capture from something else labelled as one.
    Neither is importable, and neither is visible to the declared half of the
    rule ([capture.md](docs/capture.md)), which can only catch a capture that
    admits to a room in the same file where it claims to be the module.
    """
    if cfg.get("source_class") != "module":
        return 0
    from profile import (
        measure_rooms,
    )

    # Measured against `unclassified` rather than against what the capture
    # declared, because the capture's own answer is the thing under test. Passing
    # `room: none` through would make `measure_rooms` return early and print that
    # no space was found — a pass that means the measurement never ran, which is
    # the assertion this check exists to stop standing in for.
    rooms = measure_rooms(
        manifest,
        out,
        {t["id"] for t in cfg["timbres"]},
        preroll_s=0.0,
        gate_s=float(cfg["gate_ms"]) / 1000.0,
        answer=ROOM_UNCLASSIFIED,
    )
    if not rooms:
        print("  module capture measures no space, as a module capture must", file=sys.stderr)
        return 0
    for tid, room in sorted(rooms.items()):
        print(f"  {tid}: RT60 {room.get('rt60_s', 0.0):.2f} s", file=sys.stderr)
    print(
        f"\n{cfg['id']}: this is declared `source_class: module` and its own audio "
        f"measures a space on {len(rooms)} timbre(s). A module is captured with every "
        f"effect off, so either the effects were on or this is not the module. "
        f"A space here drives room.py into matching a tank rather than a building.",
        file=sys.stderr,
    )
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("config", type=Path, help="tools/voicematch/capture/<id>.json")
    ap.add_argument("--out", default="", help="corpus directory (default: the scratch root)")
    ap.add_argument(
        "--report", action="store_true", help="print what each font sets, and import nothing"
    )
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    cfg = load_config(args.config)
    out = Path(args.out).expanduser().resolve() if args.out else DEFAULT_OUT_ROOT / cfg["id"]

    try:
        if args.report:
            for timbre in cfg["timbres"]:
                path, name = _resolve_font(cfg, timbre)
                with SoundFont(path) as font:
                    print(f"{path.name} [{timbre['id']} -> {name!r}]")
                    print(json.dumps(font.extraction_report(), indent=2))
            return 0
        manifest = import_corpus(cfg, out, verbose=args.verbose)
    except (ImportRefused, UnplayableAsCorpus) as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 1

    reach = manifest["reach"]
    print(f"{reach['extracted']} of {reach['requested']} cells in {out}", file=sys.stderr)
    for tid, gaps in sorted(reach["missing"].items()):
        print(f"  {tid}: no recording at {gaps}", file=sys.stderr)
    return check_module_is_dry(cfg, manifest, out)


if __name__ == "__main__":
    raise SystemExit(main())
