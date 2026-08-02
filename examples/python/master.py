#!/usr/bin/env python3
"""Master an input file to -14 LUFS, write a PCM WAV, and print its report."""

from __future__ import annotations

import sys
import wave
from array import array
from dataclasses import asdict
import json
import math
from typing import Sequence

import libsonare


def write_wav(path: str, samples: Sequence[float], sample_rate: int) -> None:
    """Write PCM in bounded chunks instead of materializing a full second copy."""
    with wave.open(path, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        for offset in range(0, len(samples), 8192):
            pcm = array(
                "h",
                (
                    round(max(-1.0, min(1.0, sample)) * 32767)
                    for sample in samples[offset : offset + 8192]
                ),
            )
            output.writeframes(pcm.tobytes())


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
