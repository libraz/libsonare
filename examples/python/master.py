"""Master an input file to -14 LUFS, write a PCM WAV, and print its report."""

from __future__ import annotations

import json
import math
import sys
from collections.abc import Sequence
from dataclasses import asdict

import libsonare
from pcm16_wav import write_pcm16_wav


def write_wav(path: str, samples: Sequence[float], sample_rate: int) -> None:
    """Write mono PCM using the shared bounded PCM16 conversion."""
    write_pcm16_wav(path, samples, sample_rate, 1)


def json_safe(value: object) -> object:
    """Map unavailable short-window metrics (±inf/NaN) to JSON null."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {key: json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_safe(item) for item in value]
    if isinstance(value, tuple):
        return [json_safe(item) for item in value]
    return value


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT_AUDIO OUTPUT_WAV", file=sys.stderr)
        return 2

    with libsonare.Audio.from_file(sys.argv[1]) as audio:
        result = audio.mastering_chain(
            {"loudness": {"enabled": True, "targetLufs": -14.0, "ceilingDb": -1.0}}
        )
    write_wav(sys.argv[2], result.samples, result.sample_rate)
    print(f"LUFS: {result.input_lufs:.1f} -> {result.output_lufs:.1f}")
    print(json.dumps(json_safe(asdict(result.report)), indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
