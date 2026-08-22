"""Round-trip tests for the WAV reader.

An externally rendered oracle arrives in whatever format the host wrote, and a
misread one does not fail loudly — it produces a plausible signal at the wrong
scale, or with the byte order of a 24-bit sample scrambled, and the fit chases
it. These build each format by hand and check what comes back.

    rye run --pyproject bindings/python/pyproject.toml \
        python -m pytest tools/voicematch/test_wavio.py
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wavio import read_wav, write_wav  # noqa: E402

SR = 48000
# Everything comes back as float32, so no format can be checked finer than this.
FLOAT32_RESOLUTION = 1.2e-7
KSDATAFORMAT_TAIL = b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"


def _riff(fmt_tag: int, bits: int, channels: int, payload: bytes, *, extensible: bool) -> bytes:
    """Assemble a WAV file, optionally in WAVE_FORMAT_EXTENSIBLE form.

    A LIST chunk sits between `fmt ` and `data` so the reader's chunk walk has
    something it must skip rather than misparse.
    """
    block = channels * bits // 8
    if extensible:
        fmt_body = struct.pack("<HHIIHH", 0xFFFE, channels, SR, SR * block, block, bits)
        fmt_body += struct.pack("<HHI", 22, bits, 0)  # cbSize, valid bits, channel mask
        fmt_body += struct.pack("<H", fmt_tag) + KSDATAFORMAT_TAIL
    else:
        fmt_body = struct.pack("<HHIIHH", fmt_tag, channels, SR, SR * block, block, bits)
    body = b"fmt " + struct.pack("<I", len(fmt_body)) + fmt_body
    body += b"LIST" + struct.pack("<I", 4) + b"INFO"
    body += b"data" + struct.pack("<I", len(payload)) + payload
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


def _reference() -> np.ndarray:
    t = np.arange(4800) / float(SR)
    return (0.7 * np.sin(2.0 * np.pi * 440.0 * t)).astype(np.float64)


def _pack_24(x: np.ndarray) -> bytes:
    v = np.round(x * (2**23 - 1)).astype(np.int32)
    return np.stack([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF], axis=1).astype("u1").tobytes()


FORMATS = [
    ("float32", 3, 32, lambda x: x.astype("<f4").tobytes(), FLOAT32_RESOLUTION),
    ("float64", 3, 64, lambda x: x.astype("<f8").tobytes(), FLOAT32_RESOLUTION),
    ("pcm8", 1, 8, lambda x: np.round(x * 127 + 128).astype("u1").tobytes(), 1e-2),
    ("pcm16", 1, 16, lambda x: np.round(x * 32767).astype("<i2").tobytes(), 4e-5),
    ("pcm24", 1, 24, _pack_24, 2e-7),
    ("pcm32", 1, 32, lambda x: np.round(x * (2**31 - 1)).astype("<i4").tobytes(), FLOAT32_RESOLUTION),
]


@pytest.mark.parametrize("extensible", [False, True], ids=["plain", "extensible"])
@pytest.mark.parametrize("name,tag,bits,pack,tol", FORMATS, ids=[f[0] for f in FORMATS])
def test_read_wav_decodes_format(tmp_path, name, tag, bits, pack, tol, extensible):
    x = _reference()
    path = tmp_path / f"{name}.wav"
    path.write_bytes(_riff(tag, bits, 1, pack(x), extensible=extensible))

    audio, sr = read_wav(path)

    assert sr == SR
    assert audio.shape == (len(x), 1)
    assert audio.dtype == np.float32
    assert np.max(np.abs(audio[:, 0].astype(np.float64) - x)) < tol


def test_read_wav_keeps_channels_interleaved(tmp_path):
    x = _reference()
    stereo = np.stack([x, -x], axis=1).astype("<f4")
    path = tmp_path / "stereo.wav"
    path.write_bytes(_riff(3, 32, 2, stereo.tobytes(), extensible=False))

    audio, _ = read_wav(path)

    assert audio.shape == (len(x), 2)
    # Right is the exact negation of left: a de-interleaving slip would not hold this.
    assert np.max(np.abs(audio[:, 0] + audio[:, 1])) < FLOAT32_RESOLUTION


def test_write_then_read_round_trips(tmp_path):
    x = _reference()
    path = tmp_path / "written.wav"
    write_wav(path, x.astype(np.float32), SR)

    audio, sr = read_wav(path)

    assert sr == SR
    assert audio.shape == (len(x), 1)
    # A full LSB (the write truncates toward zero rather than rounding) plus the
    # 32767-out / 32768-in scale asymmetry, over a 0.7 peak.
    assert np.max(np.abs(audio[:, 0].astype(np.float64) - x)) < 7e-5


def test_read_wav_rejects_a_non_wave_file(tmp_path):
    path = tmp_path / "not.wav"
    path.write_bytes(b"ID3\x04\x00\x00\x00\x00\x00\x00\x00\x00")

    with pytest.raises(ValueError, match="not a RIFF/WAVE file"):
        read_wav(path)
