"""Tiny stdlib WAV I/O for the voice-match harness.

Writing goes through 16-bit PCM, so both renderers' outputs carry identical
quantization noise (~-96 dBFS, far below any timbre metric of interest).

Reading accepts what a DAW or plugin host actually writes — PCM 8/16/24/32 and
IEEE float 32/64, including WAVE_FORMAT_EXTENSIBLE — because an externally
rendered oracle is rarely 16-bit. Every format is returned as float32 in
[-1, 1], so the caller cannot tell which one it got.
"""

from __future__ import annotations

import struct
import wave
from pathlib import Path

import numpy as np

WAVE_FORMAT_PCM = 0x0001
WAVE_FORMAT_IEEE_FLOAT = 0x0003
WAVE_FORMAT_EXTENSIBLE = 0xFFFE


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


def _decode(raw: bytes, fmt: int, bits: int) -> np.ndarray:
    """Decode one channel-interleaved sample block to float32 in [-1, 1]."""
    if fmt == WAVE_FORMAT_IEEE_FLOAT:
        if bits == 32:
            return np.frombuffer(raw, dtype="<f4").astype(np.float32)
        if bits == 64:
            return np.frombuffer(raw, dtype="<f8").astype(np.float32)
        raise ValueError(f"unsupported float width: {bits}-bit")
    if fmt != WAVE_FORMAT_PCM:
        raise ValueError(f"unsupported WAV format tag: 0x{fmt:04X}")
    if bits == 8:  # 8-bit PCM is unsigned by the RIFF spec
        return (np.frombuffer(raw, dtype="u1").astype(np.float32) - 128.0) / 128.0
    if bits == 16:
        return np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if bits == 24:
        b = np.frombuffer(raw, dtype="u1").reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        return v.astype(np.float32) / float(1 << 23)
    if bits == 32:
        return np.frombuffer(raw, dtype="<i4").astype(np.float32) / float(1 << 31)
    raise ValueError(f"unsupported PCM width: {bits}-bit")


def read_wav(path: Path | str) -> tuple[np.ndarray, int]:
    """Read a WAV file into a float32 (frames, channels) array plus its rate.

    Walks the RIFF chunks directly rather than going through `wave`, which
    rejects IEEE-float and extensible files that hosts routinely write.
    """
    path = Path(path)
    fmt_tag = bits = channels = sr = None
    data: np.ndarray | None = None
    with path.open("rb") as f:
        header = f.read(12)
        if len(header) < 12 or header[:4] != b"RIFF" or header[8:12] != b"WAVE":
            raise ValueError(f"{path}: not a RIFF/WAVE file")
        while True:
            head = f.read(8)
            if len(head) < 8:
                break
            cid, size = struct.unpack("<4sI", head)
            if cid == b"fmt ":
                body = f.read(size)
                fmt_tag, channels, sr, _, _, bits = struct.unpack("<HHIIHH", body[:16])
                if fmt_tag == WAVE_FORMAT_EXTENSIBLE and len(body) >= 26:
                    # The real format tag is the first field of the SubFormat GUID.
                    (fmt_tag,) = struct.unpack("<H", body[24:26])
            elif cid == b"data":
                if fmt_tag is None:
                    raise ValueError(f"{path}: data chunk precedes fmt chunk")
                data = _decode(f.read(size), fmt_tag, bits)
            else:
                f.seek(size, 1)
            if size % 2:  # RIFF chunks are word-aligned
                f.seek(1, 1)
    if data is None or channels is None or sr is None:
        raise ValueError(f"{path}: no decodable data chunk")
    usable = (len(data) // channels) * channels
    return data[:usable].reshape(-1, channels), int(sr)
