#!/usr/bin/env python3
"""Voice-match harness: render the same MIDI through libsonare's GM fallback
(physical models under tuning) and a reference GM synth, then report per-note
timbre deltas.

Run from the repo root through the bindings' rye environment:

    rye run --pyproject bindings/python/pyproject.toml \
        python tools/voicematch/voicematch.py compare --programs 40

Outputs land under tools/voicematch/out/p<NNN>_<pattern>/ as model.wav,
oracle.wav, notes.mid, report.txt and report.json. Rebuild the dylib first
when the synth code changed:

    cmake --build build-python-shared --target sonare_shared -j8
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gm_names import gm_name
from metrics import (
    OCTAVE_CENTERS,
    THIRD_OCTAVE_CENTERS,
    analyze_hit,
    analyze_note,
    compare_hit,
    compare_note,
    normalize_rms,
    to_mono,
)
from patterns import PATTERN_BUILDERS, build_pattern, pattern_length
from render_model import ensure_lib_path, render_model
from render_oracle import add_oracle_args, obtain_oracle
from room import apply_room, estimate_room, fit_room_ir, match_sends
from smf import write_smf
from wavio import write_wav

OUT_DIR = Path(__file__).resolve().parent / "out"
SR = 48000


def parse_sends(spec: str) -> tuple[int | None, int | None, int | None]:
    """Parse '0,0,0' / 'gs' / 'none' into the write_smf sends triple."""
    if spec == "gs":
        return (None, None, None)  # leave the module at its power-on values
    parts = [p.strip() for p in spec.split(",")]
    if len(parts) != 3:
        raise ValueError(f"--model-sends wants three values or 'gs', got {spec!r}")
    return tuple(None if p in ("", "gs") else int(p) for p in parts)  # type: ignore[return-value]


def match_model_room(model, oracle, sr, notes, mode: str, external: bool):
    """Put the model in the oracle's space so the timbre metrics compare like for like.

    Returns (model_audio, room). An oracle rendered by an external host carries
    its room baked in; without this the comparison reads that room as the
    instrument's release, noise floor and sustain slope.

    Skipped unless the oracle came from a WAV: the built-in fluidsynth oracle is
    rendered with its effect units off, so anything measured there is the
    instrument's own decay and correcting for it would invent a room.
    """
    if mode == "none" or not external:
        return model, None
    room = estimate_room(oracle, sr, notes)
    if room.is_dry():
        return model, room
    return apply_room(model, fit_room_ir(model, sr, notes, room)), room


def parse_programs(spec: str) -> list[int]:
    """Parse '40', '40-43', or '40,42,71' into a program list."""
    programs: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            lo, hi = part.split("-", 1)
            programs.extend(range(int(lo), int(hi) + 1))
        elif part:
            programs.append(int(part))
    for p in programs:
        if not 0 <= p < 128:
            raise ValueError(f"program out of range: {p}")
    return programs


def note_label(midi_note: int) -> str:
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    return f"{names[midi_note % 12]}{midi_note // 12 - 1}"


def probe_kwargs(args) -> dict:
    """The note/velocity overrides a pattern builder takes, from the CLI."""
    kwargs: dict = {}
    if getattr(args, "drum_note", None) is not None:
        kwargs["notes"] = (args.drum_note,)
    if getattr(args, "notes", ""):
        kwargs["notes"] = tuple(int(n) for n in args.notes.split(","))
    if getattr(args, "velocities", ""):
        kwargs["velocities"] = tuple(int(v) for v in args.velocities.split(","))
    return kwargs


def probe_pattern(args, program: int):
    """Build the probe, defaulting to the drum pattern when a drum note is named."""
    name = args.pattern
    if getattr(args, "drum_note", None) is not None and name == "sustain":
        name = "drum"
    return build_pattern(name, program, **probe_kwargs(args))


def out_dir_for(program: int, pattern) -> Path:
    """Where a probe's renders and report land.

    A drum probe is keyed by its note rather than by the program: the program is
    only the kit, so every drum note of a kit would otherwise share one directory
    and overwrite the previous note's report.
    """
    if pattern.percussive and pattern.notes:
        return OUT_DIR / f"d{pattern.notes[0].note:03d}_{pattern.name}"
    return OUT_DIR / f"p{program:03d}_{pattern.name}"


# GM percussion key map, the part of it a probe is likely to name.
_DRUM_NAMES = {
    35: "Acoustic Bass Drum", 36: "Bass Drum 1", 37: "Side Stick", 38: "Acoustic Snare",
    39: "Hand Clap", 40: "Electric Snare", 41: "Low Floor Tom", 42: "Closed Hi-Hat",
    43: "High Floor Tom", 44: "Pedal Hi-Hat", 45: "Low Tom", 46: "Open Hi-Hat",
    47: "Low-Mid Tom", 48: "Hi-Mid Tom", 49: "Crash Cymbal 1", 50: "High Tom",
    51: "Ride Cymbal 1", 52: "Chinese Cymbal", 53: "Ride Bell", 54: "Tambourine",
    55: "Splash Cymbal", 56: "Cowbell", 57: "Crash Cymbal 2", 59: "Ride Cymbal 2",
    60: "Hi Bongo", 61: "Low Bongo", 62: "Mute Hi Conga", 63: "Open Hi Conga",
    64: "Low Conga", 65: "High Timbale", 66: "Low Timbale", 69: "Cabasa",
    70: "Maracas", 75: "Claves", 76: "Hi Wood Block", 77: "Low Wood Block",
}


def drum_name(note: int) -> str:
    return _DRUM_NAMES.get(note, f"drum note {note}")


def format_drum_report(pattern_name: str, rows: list[dict]) -> str:
    """The percussion metric table: level profile, per-band decay, gesture."""
    if not rows:
        return f"== pattern '{pattern_name}' ==\n(no analyzable hits)"
    note = rows[0]["model"]["note"]
    lines = [f"== drum note {note} ({drum_name(note)}) — pattern '{pattern_name}' =="]
    for row in rows:
        m, o, d = row["model"], row["oracle"], row["delta"]
        lines.append(f"velocity {m['velocity']}")
        lines.append(
            f"  peak band {m['peak_band_hz']:8.0f} Hz  vs {o['peak_band_hz']:8.0f} Hz"
            f"   x{d['peak_band_ratio']:.2f}"
        )
        lines.append(
            f"  centroid  {m['centroid_hz']:8.1f} Hz  vs {o['centroid_hz']:8.1f} Hz"
            f"   Δ {d['centroid_delta_hz']:+8.1f} Hz"
            f" ({'brighter' if d['centroid_delta_hz'] > 0 else 'darker'})"
        )
        lines.append(
            f"  attack    {m['attack_ms']:8.2f} ms  vs {o['attack_ms']:8.2f} ms"
            f"   Δ {d['attack_delta_ms']:+.2f} ms"
        )
        dec_m = f"{m['decay_ms']:.0f}{'+' if m['decay_capped'] else ''}"
        dec_o = f"{o['decay_ms']:.0f}{'+' if o['decay_capped'] else ''}"
        lines.append(
            f"  -20 dB in {dec_m:>8} ms  vs {dec_o:>8} ms   Δ {d['decay_delta_ms']:+.1f} ms"
        )
        lines.append(
            f"  crest     {m['crest_db']:8.2f} dB  vs {o['crest_db']:8.2f} dB"
            f"   Δ {d['crest_delta_db']:+.2f} dB"
        )
        lines.append(
            f"  level     {m['level_db']:8.2f} dB  vs {o['level_db']:8.2f} dB"
            f"   Δ {d['level_delta_db']:+.2f} dB (post RMS-align)"
        )
        bands = "  ".join(
            f"{c / 1000:g}k:{v:+.0f}" if c >= 1000 else f"{c:g}:{v:+.0f}"
            for c, v in zip(THIRD_OCTAVE_CENTERS, d["bands_delta_db"])
        )
        lines.append(f"  1/3-oct Δ dB vs peak-normalized oracle: {bands}")
        decay = "  ".join(
            f"{c / 1000:g}k:{v:+.0f}" if c >= 1000 else f"{c:g}:{v:+.0f}"
            for c, v in zip(OCTAVE_CENTERS, d["band_decay_delta_db_s"])
            if v is not None
        )
        lines.append(f"  band decay Δ dB/s: {decay or '(no band fittable)'}")
        lines.append("")
    return "\n".join(lines)


def format_report(program: int, pattern_name: str, rows: list[dict]) -> str:
    lines = [f"== program {program} ({gm_name(program)}) — pattern '{pattern_name}' =="]
    if not rows:
        lines.append("(no analyzable notes in this pattern — listening/waveform check only)")
        return "\n".join(lines)
    for row in rows:
        m, o, d = row["model"], row["oracle"], row["delta"]
        n = m["note"]
        lines.append(f"note {n} ({note_label(n)}) vel {m['velocity']}")
        lines.append(
            f"  f0        {m['f0_hz']:8.2f} Hz ({m['f0_cents_err']:+5.1f}c)"
            f"  vs {o['f0_hz']:8.2f} Hz ({o['f0_cents_err']:+5.1f}c)"
            f"   Δ {d['f0_cents_delta']:+.1f}c"
        )
        lines.append(
            f"  centroid  {m['centroid_hz']:8.1f} Hz  vs {o['centroid_hz']:8.1f} Hz"
            f"   Δ {d['centroid_delta_hz']:+8.1f} Hz"
            f" ({'brighter' if d['centroid_delta_hz'] > 0 else 'darker'})"
        )
        lines.append(
            f"  odd/even  {m['odd_even_db']:+7.2f} dB  vs {o['odd_even_db']:+7.2f} dB"
            f"   Δ {d['odd_even_delta_db']:+.2f} dB"
        )
        lines.append(
            f"  TNR       {m['tnr_db']:7.2f} dB  vs {o['tnr_db']:7.2f} dB"
            f"   Δ {d['tnr_delta_db']:+.2f} dB ({'cleaner' if d['tnr_delta_db'] > 0 else 'noisier'})"
        )
        lines.append(
            f"  attack    {m['attack_ms']:7.1f} ms  vs {o['attack_ms']:7.1f} ms"
            f"   Δ {d['attack_delta_ms']:+.1f} ms"
        )
        lines.append(
            f"  sus slope {m['sustain_slope_db_s']:+7.2f} dB/s vs {o['sustain_slope_db_s']:+7.2f} dB/s"
            f"   Δ {d['sustain_slope_delta_db_s']:+.2f}"
        )
        rel_m = f"{m['release_ms']:.0f}{'+' if m['release_capped'] else ''}"
        rel_o = f"{o['release_ms']:.0f}{'+' if o['release_capped'] else ''}"
        lines.append(f"  release   {rel_m:>7} ms  vs {rel_o:>7} ms   Δ {d['release_delta_ms']:+.1f} ms")
        lines.append(
            f"  level     {m['sustain_rms_db']:7.2f} dB  vs {o['sustain_rms_db']:7.2f} dB"
            f"   Δ {d['level_delta_db']:+.2f} dB (post RMS-align)"
        )
        harm = "  ".join(
            f"h{k + 2}:{v:+.1f}" if v is not None else f"h{k + 2}:n/a"
            for k, v in enumerate(d["harmonics_delta_db"][1:])
        )
        lines.append(f"  harmonics Δ dB vs h1-normalized oracle: {harm}")
        lines.append("")
    return "\n".join(lines)


def run_compare(args: argparse.Namespace) -> int:
    programs = parse_programs(args.programs)
    ensure_lib_path()
    exit_code = 0
    for program in programs:
        pattern = probe_pattern(args, program)
        total = pattern_length(pattern)
        smf_bytes = write_smf(
            pattern.notes, program=program, channel=pattern.channel, end_pad=pattern.tail,
            sends=parse_sends(args.model_sends),
        )

        out = out_dir_for(program, pattern)
        out.mkdir(parents=True, exist_ok=True)
        (out / "notes.mid").write_bytes(smf_bytes)

        model = render_model(smf_bytes, total, SR)
        oracle = obtain_oracle(
            args, smf_bytes, total, SR, [n.start for n in pattern.notes],
        )
        model, room = match_model_room(
            model, oracle, SR, [(n.start, n.start + n.dur) for n in pattern.notes], args.room,
            external=bool(getattr(args, "oracle_wav", "")),
        )
        write_wav(out / "model.wav", model, SR)
        write_wav(out / "oracle.wav", oracle, SR)

        if args.render_only:
            print(f"rendered {out}/model.wav + oracle.wav ({total:.1f}s)")
            continue

        model_mono = normalize_rms(to_mono(model))
        oracle_mono = normalize_rms(to_mono(oracle))

        rows = []
        if pattern.percussive:
            # A hit's window ends where the next one begins; the last one gets
            # the tail. `analyze_hit` caps it, so a long gap costs nothing.
            starts = [n.start for n in pattern.notes]
            for note in pattern.analysis_notes:
                end = next((s for s in starts if s > note.start), total)
                hm = analyze_hit(model_mono, SR, note, end)
                ho = analyze_hit(oracle_mono, SR, note, end)
                rows.append({
                    "model": hm.to_dict(),
                    "oracle": ho.to_dict(),
                    "delta": compare_hit(hm, ho),
                })
        else:
            for note in pattern.analysis_notes:
                end = note.start + note.dur + pattern.tail
                nm = analyze_note(model_mono, SR, note, end)
                no = analyze_note(oracle_mono, SR, note, end)
                rows.append({
                    "model": nm.to_dict(),
                    "oracle": no.to_dict(),
                    "delta": compare_note(nm, no),
                })

        report = (format_drum_report(pattern.name, rows) if pattern.percussive
                  else format_report(program, pattern.name, rows))
        if room is not None and not room.is_dry():
            report = (
                f"oracle room: RT60 {room.rt60_s:.2f}s, tail level {room.tail_db:+.1f}dB, "
                f"HF ratio {room.hf_ratio:.2f} — the model was convolved with a matching\n"
                f"             space before analysis, so the deltas below are timbre, not room.\n"
                f"             `room-match` converts this into libsonare's own send settings.\n\n"
            ) + report
        (out / "report.txt").write_text(report + "\n")
        (out / "report.json").write_text(json.dumps({
            "program": program,
            "gm_name": gm_name(program),
            "drum_note": pattern.notes[0].note if pattern.percussive else None,
            "pattern": pattern.name,
            "percussive": pattern.percussive,
            "sample_rate": SR,
            "oracle_room": room.to_dict() if room is not None else None,
            "notes": rows,
        }, indent=2) + "\n")
        print(report)
        print(f"-> {out}/  (model.wav / oracle.wav / report.json)")
        print()
    return exit_code


def run_export_probe(args: argparse.Namespace) -> int:
    """Write the probe SMF (plus its timeline) for rendering in an external host."""
    programs = parse_programs(args.programs)
    for program in programs:
        pattern = probe_pattern(args, program)
        total = pattern_length(pattern)
        smf_bytes = write_smf(
            pattern.notes, program=program, channel=pattern.channel, end_pad=pattern.tail
        )

        out = out_dir_for(program, pattern)
        out.mkdir(parents=True, exist_ok=True)
        mid = out / "probe.mid"
        mid.write_bytes(smf_bytes)
        (out / "probe.json").write_text(json.dumps({
            "program": program,
            "gm_name": gm_name(program),
            "drum_note": pattern.notes[0].note if pattern.percussive else None,
            "channel": pattern.channel,
            "pattern": pattern.name,
            "sample_rate": SR,
            "total_seconds": round(total, 4),
            "notes": [
                {"note": n.note, "velocity": n.velocity,
                 "start": round(n.start, 4), "dur": round(n.dur, 4)}
                for n in pattern.notes
            ],
        }, indent=2) + "\n")
        print(f"-> {mid}")
        if pattern.percussive:
            note = pattern.notes[0].note
            print(f"   drum note {note} ({drum_name(note)}) on MIDI channel 10, kit "
                  f"{program}, pattern '{pattern.name}', {total:.2f}s, "
                  f"{len(pattern.notes)} hits")
            print(f"   Render it at {SR} Hz with the reference kit, then pass the WAV back:")
            print(f"     voicematch.py compare --programs {program} --drum-note {note} "
                  f"--pattern {pattern.name} --oracle-wav <rendered.wav>")
            continue
        print(f"   program {program} ({gm_name(program)}), pattern '{pattern.name}', "
              f"{total:.2f}s, {len(pattern.notes)} notes")
        print(f"   Render it at {SR} Hz with the reference instrument, then pass the WAV back:")
        print(f"     voicematch.py compare --programs {program} --pattern {pattern.name} "
              f"--oracle-wav <rendered.wav>")
    return 0


def run_room_match(args: argparse.Namespace) -> int:
    """Measure the oracle's space and report the libsonare settings that match it.

    This is the half of ambience that lives in the shipped library rather than
    in the harness. `compare` puts the model in the oracle's room so the timbre
    fit is not confounded by it; this asks the complementary question — what
    would libsonare have to do to be in that room by itself, at the GS power-on
    controller values a plain GM file arrives with.
    """
    import os
    import subprocess

    program = parse_programs(args.programs)[0]
    pattern = build_pattern(args.pattern, program)
    total = pattern_length(pattern)
    probe = write_smf(pattern.notes, program=program, channel=pattern.channel,
                      end_pad=pattern.tail, sends=(0, 0, 0))
    spans = [(n.start, n.start + n.dur) for n in pattern.notes]

    oracle = obtain_oracle(args, probe, total, SR, [n.start for n in pattern.notes])
    target = estimate_room(oracle, SR, spans)
    print(f"oracle room: RT60 {target.rt60_s:.2f}s  tail level {target.tail_db:+.1f}dB  "
          f"HF ratio {target.hf_ratio:.2f}")
    if target.is_dry():
        print("-> the reference is dry; no ambience to reproduce.")
        return 0

    child = (
        "import sys; sys.path.insert(0, %r)\n"
        "from patterns import build_pattern, pattern_length\n"
        "from smf import write_smf\n"
        "from room import estimate_room\n"
        "import render_model\n"
        "prog, cc91 = int(sys.argv[1]), int(sys.argv[2])\n"
        "pat = build_pattern(%r, prog)\n"
        "smf = write_smf(pat.notes, program=prog, channel=pat.channel, "
        "end_pad=pat.tail, sends=(cc91, 0, 0))\n"
        "a = render_model.render_model(smf, pattern_length(pat), 48000)\n"
        "r = estimate_room(a, 48000, [(n.start, n.start + n.dur) for n in pat.notes])\n"
        "print(f'{r.rt60_s} {r.tail_db} {r.hf_ratio}')\n"
    ) % (str(Path(__file__).resolve().parent), args.pattern)

    def measure(cc91: int, decay_scale: float):
        from room import Room
        env = dict(os.environ)
        env["SONARE_TUNING_OVERRIDES"] = f"gs_effects.kReverbDecayScale={decay_scale}"
        proc = subprocess.run(
            [sys.executable, "-c", child, str(program), str(cc91)],
            env=env, capture_output=True, text=True,
        )
        if proc.returncode != 0:
            raise SystemExit(proc.stderr[-1200:])
        rt, tail, hf = (float(v) for v in proc.stdout.strip().split()[-3:])
        return Room(rt60_s=rt, hf_ratio=hf, tail_db=tail, predelay_ms=15.0)

    print("searching libsonare's ambience controls (one render per point)...")
    result = match_sends(target, measure, log=print if args.verbose else None)
    print()
    print(f"closest match: CC91 {result['cc91']}, reverb_decay {result['reverb_decay']} "
          f"(gs_effects.kReverbDecayScale={result['decay_scale']})")
    print(f"  reached RT60 {result['measured']['rt60_s']:.2f}s  "
          f"tail {result['measured']['tail_db']:+.1f}dB   residual {result['residual']}")
    print(f"  -> gm_fallback_sends reverb_scale for program {program}: "
          f"{result['reverb_scale']} (currently weighting the GS power-on CC91 of 40)")
    if result["residual"] > 1.5:
        print("  NOTE: residual above 1.5 — libsonare's tank cannot reach this space; "
              "the reference room is outside its range.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    cmp_p = sub.add_parser("compare", help="render both synths and report timbre deltas")
    cmp_p.add_argument("--programs", required=True, help="GM programs: '40', '40-43', or '40,42,71'")
    cmp_p.add_argument("--pattern", default="sustain", choices=sorted(PATTERN_BUILDERS))
    cmp_p.add_argument("--notes", default="", help="override probe notes, e.g. '55,67,79'")
    cmp_p.add_argument("--velocities", default="",
                       help="override probe velocities, e.g. '64,100,127'")
    cmp_p.add_argument("--drum-note", type=int, default=None, dest="drum_note",
                       help="compare a percussion instrument: the probe moves to the drum "
                            "channel, where this note selects the instrument and --programs "
                            "selects the kit, and the report switches to the percussion "
                            "metric set (band profile, per-band decay, attack)")
    add_oracle_args(cmp_p)
    cmp_p.add_argument(
        "--room", default="auto", choices=("auto", "none"),
        help="auto (default): measure the oracle's reverberation and convolve a matching "
             "space onto the model before analysis, so a wet reference does not read as "
             "timbre; none: compare as rendered",
    )
    cmp_p.add_argument(
        "--model-sends", default="0,0,0", metavar="R,C,D",
        help="CC91/93/94 written into the probe for the model render (default 0,0,0 = dry; "
             "'gs' keeps libsonare's power-on ambience)",
    )
    cmp_p.add_argument("--render-only", action="store_true", help="write WAVs but skip analysis")
    cmp_p.set_defaults(func=run_compare)

    room_p = sub.add_parser(
        "room-match",
        help="measure the oracle's room and report the libsonare sends that reproduce it",
    )
    room_p.add_argument("--programs", required=True, help="GM program (only the first is used)")
    room_p.add_argument("--pattern", default="sustain", choices=sorted(PATTERN_BUILDERS))
    room_p.add_argument("--verbose", action="store_true", help="print every search point")
    add_oracle_args(room_p)
    room_p.set_defaults(func=run_room_match)

    exp_p = sub.add_parser(
        "export-probe",
        help="write the probe SMF so an external synth can render the oracle side",
    )
    exp_p.add_argument("--programs", required=True, help="GM programs: '40', '40-43', or '40,42,71'")
    exp_p.add_argument("--pattern", default="sustain", choices=sorted(PATTERN_BUILDERS))
    exp_p.add_argument("--notes", default="", help="override probe notes, e.g. '55,67,79'")
    exp_p.add_argument("--velocities", default="",
                       help="override probe velocities, e.g. '64,100,127'")
    exp_p.add_argument("--drum-note", type=int, default=None, dest="drum_note",
                       help="export a drum-channel probe of this percussion instrument "
                            "instead of a melodic one")
    exp_p.set_defaults(func=run_export_probe)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
