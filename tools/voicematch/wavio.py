"""Tiny stdlib WAV I/O (16-bit PCM) for the voice-match harness.

Both renderers' outputs go through the same 16-bit quantization so the
comparison sees identical noise floors (~-96 dBFS, far below any timbre
metric of interest).
"""

from __future__ import annotations

import wave
from pathlib import Path

import numpy as np


def write_wav(path: Path | str, audio: np.ndarray, sr: int) -> None:
    """Write a (frames,) or (frames, channels) float array as 16-bit PCM."""
    if audio.ndim == 1:
        audio = audio[:, None]
    clipped = np.clip(audio, -1.0, 1.0)
    pcm = (clipped * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def read_wav(path: Path | str) -> tuple[np.ndarray, int]:
    """Read a 16-bit PCM WAV into a float32 (frames, channels) array."""
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: only 16-bit PCM WAV supported (got {8 * w.getsampwidth()}-bit)")
        sr = w.getframerate()
        channels = w.getnchannels()
        raw = w.readframes(w.getnframes())
    pcm = np.frombuffer(raw, dtype="<i2").reshape(-1, channels)
    return pcm.astype(np.float32) / 32768.0, sr
