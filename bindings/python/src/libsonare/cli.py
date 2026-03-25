"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import sys

PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MODE_NAMES = ["major", "minor"]


def _load_audio(path: str) -> tuple[list[float], int]:
    """Load audio from file via the Audio class."""
    from .audio import Audio

    audio = Audio.from_file(path)
    return audio.data, audio.sample_rate


def _format_time(seconds: float) -> str:
    """Format seconds as mm:ss."""
    mm = int(seconds) // 60
    ss = int(seconds) % 60
    return f"{mm}:{ss:02d}"


def cmd_version(args: argparse.Namespace) -> int:
    from . import version

    v = version()
    if args.json:
        print(json.dumps({"lib_version": v, "cli": "python"}))
    else:
        print(f"libsonare {v} (Python CLI)")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    from .audio import Audio

    audio = Audio.from_file(args.file)
    sr = audio.sample_rate
    n = audio.length
    dur = audio.duration

    if args.json:
        print(
            json.dumps(
                {
                    "duration": round(dur, 3),
                    "sample_rate": sr,
                    "samples": n,
                }
            )
        )
    else:
        print(f"  Duration:    {_format_time(dur)} ({dur:.1f}s)")
        print(f"  Sample Rate: {sr} Hz")
        print(f"  Samples:     {n}")
    return 0


def cmd_bpm(args: argparse.Namespace) -> int:
    from . import detect_bpm

    samples, sr = _load_audio(args.file)
    bpm = detect_bpm(samples, sample_rate=sr)
    if args.json:
        print(json.dumps({"bpm": round(bpm, 2)}))
    else:
        print(f"  BPM: {bpm:.2f}")
    return 0


def cmd_key(args: argparse.Namespace) -> int:
    from . import detect_key

    samples, sr = _load_audio(args.file)
    key = detect_key(samples, sample_rate=sr)
    name = f"{PITCH_NAMES[key.root.value]} {MODE_NAMES[key.mode.value]}"
    if args.json:
        print(
            json.dumps(
                {
                    "root": key.root.value,
                    "mode": key.mode.value,
                    "confidence": round(key.confidence, 4),
                    "name": name,
                }
            )
        )
    else:
        print(f"  Key: {name} (confidence: {key.confidence:.1%})")
    return 0


def cmd_beats(args: argparse.Namespace) -> int:
    from . import detect_beats

    samples, sr = _load_audio(args.file)
    beats = detect_beats(samples, sample_rate=sr)
    if args.json:
        print(json.dumps([round(b, 4) for b in beats]))
    else:
        print(f"  Beat times ({len(beats)} beats):")
        for i, b in enumerate(beats[:20]):
            print(f"    {i + 1:3d}. {b:.3f}s")
        if len(beats) > 20:
            print(f"    ... ({len(beats) - 20} more)")
    return 0


def cmd_onsets(args: argparse.Namespace) -> int:
    from . import detect_onsets

    samples, sr = _load_audio(args.file)
    onsets = detect_onsets(samples, sample_rate=sr)
    if args.json:
        print(json.dumps([round(o, 4) for o in onsets]))
    else:
        print(f"  Onset times ({len(onsets)} onsets):")
        for i, o in enumerate(onsets[:20]):
            print(f"    {i + 1:3d}. {o:.3f}s")
        if len(onsets) > 20:
            print(f"    ... ({len(onsets) - 20} more)")
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    from . import analyze

    samples, sr = _load_audio(args.file)
    r = analyze(samples, sample_rate=sr)
    key_name = f"{PITCH_NAMES[r.key.root.value]} {MODE_NAMES[r.key.mode.value]}"

    if args.json:
        print(
            json.dumps(
                {
                    "bpm": round(r.bpm, 2),
                    "bpm_confidence": round(r.bpm_confidence, 4),
                    "key": {
                        "root": r.key.root.value,
                        "mode": r.key.mode.value,
                        "confidence": round(r.key.confidence, 4),
                        "name": key_name,
                    },
                    "time_signature": {
                        "numerator": r.time_signature.numerator,
                        "denominator": r.time_signature.denominator,
                    },
                    "beats": len(r.beat_times),
                }
            )
        )
    else:
        print(
            f"\n  \033[32m\033[1m> Estimated BPM : {r.bpm:.2f} BPM  "
            f"(conf {r.bpm_confidence * 100:.1f}%)\033[0m"
        )
        print(
            f"  \033[35m\033[1m> Estimated Key : {key_name}  "
            f"(conf {r.key.confidence * 100:.1f}%)\033[0m"
        )
        print(f"  > Time Signature: {r.time_signature.numerator}/{r.time_signature.denominator}")
        print(f"  > Beats: {len(r.beat_times)}")
    return 0


def cmd_mel(args: argparse.Namespace) -> int:
    from . import mel_spectrogram

    samples, sr = _load_audio(args.file)
    result = mel_spectrogram(
        samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length, n_mels=args.n_mels
    )
    if args.json:
        print(
            json.dumps(
                {
                    "n_mels": result.n_mels,
                    "n_frames": result.n_frames,
                    "sample_rate": result.sample_rate,
                    "hop_length": result.hop_length,
                }
            )
        )
    else:
        print("  Mel Spectrogram:")
        print(f"    Shape: {result.n_mels} mels x {result.n_frames} frames")
    return 0


def cmd_chroma(args: argparse.Namespace) -> int:
    from . import chroma

    samples, sr = _load_audio(args.file)
    result = chroma(samples, sample_rate=sr, n_fft=args.n_fft, hop_length=args.hop_length)
    if args.json:
        print(
            json.dumps(
                {
                    "n_chroma": result.n_chroma,
                    "n_frames": result.n_frames,
                    "mean_energy": [round(e, 6) for e in result.mean_energy],
                }
            )
        )
    else:
        print(f"  Chromagram: {result.n_chroma} bins x {result.n_frames} frames")
        print("  Mean energy per pitch class:")
        max_energy = max(result.mean_energy) if result.mean_energy else 0
        for i, e in enumerate(result.mean_energy):
            bar = "#" * int(e * 50 / max_energy) if max_energy > 0 else ""
            print(f"    {PITCH_NAMES[i]:2s} {e:.4f} {bar}")
    return 0


def cmd_spectral(args: argparse.Namespace) -> int:
    import statistics

    from . import (
        rms_energy,
        spectral_bandwidth,
        spectral_centroid,
        spectral_flatness,
        spectral_rolloff,
        zero_crossing_rate,
    )

    samples, sr = _load_audio(args.file)
    nf = args.n_fft
    hl = args.hop_length

    def _stats(vals: list[float]) -> dict:
        if not vals:
            return {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
        return {
            "mean": round(statistics.mean(vals), 4),
            "std": round(statistics.stdev(vals), 4) if len(vals) > 1 else 0.0,
            "min": round(min(vals), 4),
            "max": round(max(vals), 4),
        }

    features = {
        "centroid": _stats(spectral_centroid(samples, sr, nf, hl)),
        "bandwidth": _stats(spectral_bandwidth(samples, sr, nf, hl)),
        "rolloff": _stats(spectral_rolloff(samples, sr, nf, hl)),
        "flatness": _stats(spectral_flatness(samples, sr, nf, hl)),
        "zcr": _stats(zero_crossing_rate(samples, sr, nf, hl)),
        "rms": _stats(rms_energy(samples, sr, nf, hl)),
    }

    if args.json:
        print(json.dumps({"features": features}))
    else:
        print("  Spectral Features:")
        print(f"  {'Feature':<15s} {'Mean':>10s} {'Std':>10s} {'Min':>10s} {'Max':>10s}")
        for name, s in features.items():
            fmt = ".1f" if name in ("centroid", "bandwidth", "rolloff") else ".4f"
            print(
                f"  {name:<15s} {s['mean']:>10{fmt}} {s['std']:>10{fmt}} "
                f"{s['min']:>10{fmt}} {s['max']:>10{fmt}}"
            )
    return 0


def cmd_pitch(args: argparse.Namespace) -> int:
    from . import pitch_pyin, pitch_yin

    samples, sr = _load_audio(args.file)
    algo = getattr(args, "algorithm", "pyin")
    if algo == "yin":
        result = pitch_yin(samples, sample_rate=sr)
    else:
        result = pitch_pyin(samples, sample_rate=sr)

    if args.json:
        print(
            json.dumps(
                {
                    "algorithm": algo,
                    "n_frames": result.n_frames,
                    "median_f0": round(result.median_f0, 2),
                    "mean_f0": round(result.mean_f0, 2),
                }
            )
        )
    else:
        print(f"  Pitch Tracking ({algo}):")
        print(f"    Frames:    {result.n_frames}")
        print(f"    Median F0: {result.median_f0:.1f} Hz")
        print(f"    Mean F0:   {result.mean_f0:.1f} Hz")
    return 0


def cmd_hpss(args: argparse.Namespace) -> int:
    from . import hpss

    samples, sr = _load_audio(args.file)
    result = hpss(samples, sample_rate=sr)

    h_energy = sum(abs(x) for x in result.harmonic) / len(result.harmonic)
    p_energy = sum(abs(x) for x in result.percussive) / len(result.percussive)

    if args.json:
        print(
            json.dumps(
                {
                    "length": result.length,
                    "sample_rate": result.sample_rate,
                    "harmonic_energy": round(h_energy, 6),
                    "percussive_energy": round(p_energy, 6),
                }
            )
        )
    else:
        print(f"  HPSS: {result.length} samples")
        print(f"  Harmonic energy:   {h_energy:.6f}")
        print(f"  Percussive energy: {p_energy:.6f}")
    return 0


def main() -> None:
    """CLI entry point."""
    # Common arguments shared by all subcommands
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", help="Output JSON")
    common.add_argument("--n-fft", type=int, default=2048, help="FFT size (default: 2048)")
    common.add_argument("--hop-length", type=int, default=512, help="Hop length (default: 512)")
    common.add_argument(
        "--n-mels", type=int, default=128, help="Number of mel bands (default: 128)"
    )
    common.add_argument("-o", "--output", type=str, default="", help="Output file path")

    parser = argparse.ArgumentParser(
        prog="sonare",
        description="libsonare - Fast audio analysis (Python CLI)",
    )
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("version", parents=[common], help="Show version")
    sub.add_parser("info", parents=[common], help="Show audio file information")
    sub.add_parser("bpm", parents=[common], help="Detect BPM")
    sub.add_parser("key", parents=[common], help="Detect musical key")
    sub.add_parser("beats", parents=[common], help="Detect beat times")
    sub.add_parser("onsets", parents=[common], help="Detect onset times")
    sub.add_parser("analyze", parents=[common], help="Full music analysis")
    sub.add_parser("mel", parents=[common], help="Compute mel spectrogram")
    sub.add_parser("chroma", parents=[common], help="Compute chromagram")
    sub.add_parser("spectral", parents=[common], help="Compute spectral features")
    pitch_p = sub.add_parser("pitch", parents=[common], help="Track pitch")
    pitch_p.add_argument("--algorithm", choices=["yin", "pyin"], default="pyin")
    sub.add_parser("hpss", parents=[common], help="Harmonic-percussive separation")

    # Add file argument to all subcommands that need it
    for name in [
        "info",
        "bpm",
        "key",
        "beats",
        "onsets",
        "analyze",
        "mel",
        "chroma",
        "spectral",
        "pitch",
        "hpss",
    ]:
        sub.choices[name].add_argument("file", help="Audio file path")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    commands = {
        "version": cmd_version,
        "info": cmd_info,
        "bpm": cmd_bpm,
        "key": cmd_key,
        "beats": cmd_beats,
        "onsets": cmd_onsets,
        "analyze": cmd_analyze,
        "mel": cmd_mel,
        "chroma": cmd_chroma,
        "spectral": cmd_spectral,
        "pitch": cmd_pitch,
        "hpss": cmd_hpss,
    }

    handler = commands.get(args.command)
    if not handler:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1)

    try:
        sys.exit(handler(args))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
