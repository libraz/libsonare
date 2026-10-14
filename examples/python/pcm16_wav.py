"""Shared PCM16 WAV writer for the Python examples."""

from __future__ import annotations

import math
import struct
import wave
from collections.abc import Sequence

_SAMPLES_PER_CHUNK = 8192


def _narrow_float32(sample: float) -> float:
    """Apply the native writer's float32 narrowing and non-finite policy."""
    value = float(sample)
    try:
        narrowed = struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return 0.0
    return narrowed if math.isfinite(narrowed) else 0.0


def float_to_pcm16(sample: float) -> int:
    """Convert one normalized sample to a signed PCM16 code."""
    narrowed = _narrow_float32(sample)
    clamped = max(-1.0, min(1.0, narrowed))
    scaled = clamped * 32768.0
    rounded = math.floor(scaled + 0.5) if scaled >= 0.0 else math.ceil(scaled - 0.5)
    return max(-32768, min(32767, rounded))


def write_pcm16_wav(path: str, samples: Sequence[float], sample_rate: int, channels: int) -> None:
    """Write interleaved float samples as bounded little-endian PCM16 chunks."""
    with wave.open(path, "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        for offset in range(0, len(samples), _SAMPLES_PER_CHUNK):
            count = min(_SAMPLES_PER_CHUNK, len(samples) - offset)
            pcm = bytearray(count * 2)
            for index in range(count):
                struct.pack_into("<h", pcm, index * 2, float_to_pcm16(samples[offset + index]))
            output.writeframes(pcm)
