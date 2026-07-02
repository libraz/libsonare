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

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gm_names import gm_name
from metrics import analyze_note, compare_note, normalize_rms, to_mono
from patterns import PATTERN_BUILDERS, build_pattern, pattern_length
from render_model import ensure_lib_path, render_model
from render_oracle import render_oracle_fluidsynth
from smf import write_smf
from wavio import write_wav

OUT_DIR = Path(__file__).resolve().parent / "out"
SR = 48000


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
        kwargs = {}
        if args.notes:
            kwargs["notes"] = tuple(int(n) for n in args.notes.split(","))
        pattern = build_pattern(args.pattern, program, **kwargs)
        total = pattern_length(pattern)
        smf_bytes = write_smf(pattern.notes, program=program, end_pad=pattern.tail)

        out = OUT_DIR / f"p{program:03d}_{pattern.name}"
        out.mkdir(parents=True, exist_ok=True)
        (out / "notes.mid").write_bytes(smf_bytes)

        model = render_model(smf_bytes, total, SR)
        oracle = render_oracle_fluidsynth(
            smf_bytes, total, SR,
            soundfont=Path(args.sf2) if args.sf2 else None,
        )
        write_wav(out / "model.wav", model, SR)
        write_wav(out / "oracle.wav", oracle, SR)

        if args.render_only:
            print(f"rendered {out}/model.wav + oracle.wav ({total:.1f}s)")
            continue

        model_mono = normalize_rms(to_mono(model))
        oracle_mono = normalize_rms(to_mono(oracle))

        rows = []
        for note in pattern.analysis_notes:
            end = note.start + note.dur + pattern.tail
            nm = analyze_note(model_mono, SR, note, end)
            no = analyze_note(oracle_mono, SR, note, end)
            rows.append({
                "model": nm.to_dict(),
                "oracle": no.to_dict(),
                "delta": compare_note(nm, no),
            })

        report = format_report(program, pattern.name, rows)
        (out / "report.txt").write_text(report + "\n")
        (out / "report.json").write_text(json.dumps({
            "program": program,
            "gm_name": gm_name(program),
            "pattern": pattern.name,
            "sample_rate": SR,
            "notes": rows,
        }, indent=2) + "\n")
        print(report)
        print(f"-> {out}/  (model.wav / oracle.wav / report.json)")
        print()
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    cmp_p = sub.add_parser("compare", help="render both synths and report timbre deltas")
    cmp_p.add_argument("--programs", required=True, help="GM programs: '40', '40-43', or '40,42,71'")
    cmp_p.add_argument("--pattern", default="sustain", choices=sorted(PATTERN_BUILDERS))
    cmp_p.add_argument("--notes", default="", help="override probe notes, e.g. '55,67,79'")
    cmp_p.add_argument("--sf2", default="", help="oracle SoundFont path (default: assets/MuseScore_General.sf3)")
    cmp_p.add_argument("--render-only", action="store_true", help="write WAVs but skip analysis")
    cmp_p.set_defaults(func=run_compare)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
