#!/usr/bin/env python3
"""Master an input file to -14 LUFS and write a PCM WAV file."""

from __future__ import annotations

import struct
import sys
import wave

import libsonare


def write_wav(path: str, samples: list[float], sample_rate: int) -> None:
    pcm = [max(-1.0, min(1.0, sample)) for sample in samples]
    frames = struct.pack(f"<{len(pcm)}h", *(round(sample * 32767) for sample in pcm))
    with wave.open(path, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(frames)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT_AUDIO OUTPUT_WAV", file=sys.stderr)
        return 2

    with libsonare.Audio.from_file(sys.argv[1]) as audio:
        result = audio.mastering(target_lufs=-14.0)
    write_wav(sys.argv[2], result.samples, result.sample_rate)
    print(f"LUFS: {result.input_lufs:.1f} -> {result.output_lufs:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
