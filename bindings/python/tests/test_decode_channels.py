"""decode_channels and downmix: channel-preserving decode and the BS.775 fold."""

from __future__ import annotations

import math
import struct
from pathlib import Path

import numpy as np
import pytest

from libsonare import Audio, ChannelLayout, SonareError, SonareValueError, decode_channels, downmix

SAMPLE_RATE = 22050
FRAMES = 64


def _pcm16_wav(channels: int) -> bytes:
    """Build a frame-interleaved 16-bit PCM WAV with a distinct tone per channel."""
    data = bytearray()
    for frame in range(FRAMES):
        for channel in range(channels):
            value = round(2000 * (channel + 1) * math.sin(0.1 * frame * (channel + 1)))
            data += struct.pack("<h", value)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
    header += struct.pack(
        "<IHHIIHH",
        16,
        1,
        channels,
        SAMPLE_RATE,
        SAMPLE_RATE * channels * 2,
        channels * 2,
        16,
    )
    return header + b"data" + struct.pack("<I", len(data)) + bytes(data)


@pytest.mark.parametrize("channel_count", [1, 2, 6])
def test_decode_channels_matches_the_core_decoder(channel_count: int, tmp_path: Path) -> None:
    """Every channel and sample equals what the core path loader reads."""
    wav = _pcm16_wav(channel_count)
    channels, sample_rate = decode_channels(wav)
    assert sample_rate == SAMPLE_RATE
    assert channels.dtype == np.float32
    assert channels.shape == (channel_count, FRAMES)

    path = tmp_path / "in.wav"
    path.write_bytes(wav)
    assert Audio.file_channel_count(str(path)) == channel_count
    for channel in range(channel_count):
        with Audio.from_file_channel(str(path), channel) as plane:
            np.testing.assert_array_equal(channels[channel], plane.data)


@pytest.mark.parametrize("channel_count", [2, 6])
def test_downmix_to_mono_equals_the_mono_decoder(channel_count: int) -> None:
    """Folding the decoded channels lands on the samples Audio.from_memory returns."""
    wav = _pcm16_wav(channel_count)
    channels, _ = decode_channels(wav)
    mono = downmix(channels, ChannelLayout.MONO)
    assert mono.shape == (1, FRAMES)
    with Audio.from_memory(wav) as audio:
        np.testing.assert_array_equal(mono[0], audio.data)


def test_decode_channels_refuses_unusable_bytes() -> None:
    with pytest.raises(SonareError):
        decode_channels(b"")
    with pytest.raises(SonareError):
        decode_channels(b"garbage")


def test_downmix_applies_bs775_coefficients() -> None:
    inv = math.sqrt(0.5)
    surround = np.stack([np.full(8, v, dtype=np.float32) for v in (0.1, 0.2, 0.3, 0.4, 0.05, 0.15)])
    stereo = downmix(surround, ChannelLayout.STEREO)
    assert stereo.shape == (2, 8)
    left = 0.1 + inv * 0.3 + inv * 0.05
    right = 0.2 + inv * 0.3 + inv * 0.15
    np.testing.assert_allclose(stereo[0], left, atol=1e-6)
    np.testing.assert_allclose(stereo[1], right, atol=1e-6)
    np.testing.assert_allclose(downmix(surround, 0)[0], 0.5 * (left + right), atol=1e-6)


def test_downmix_copies_on_identity_and_means_an_unmodelled_count() -> None:
    stereo = [np.full(8, 0.25, dtype=np.float32), np.full(8, -0.5, dtype=np.float32)]
    np.testing.assert_array_equal(downmix(stereo, ChannelLayout.STEREO), np.stack(stereo))
    three = [np.full(8, v, dtype=np.float32) for v in (0.3, 0.6, 0.9)]
    np.testing.assert_allclose(downmix(three, ChannelLayout.MONO)[0], 0.6, atol=1e-6)
    with pytest.raises(SonareError):
        downmix(three, ChannelLayout.STEREO)


def test_downmix_refuses_an_upmix_an_unknown_layout_and_malformed_channels() -> None:
    pair = [np.zeros(8, dtype=np.float32), np.zeros(8, dtype=np.float32)]
    with pytest.raises(SonareError):
        downmix(pair, ChannelLayout.FIVE_POINT_ONE)
    with pytest.raises(ValueError):
        downmix(pair, 4)
    with pytest.raises(SonareValueError):
        downmix([], ChannelLayout.MONO)
    with pytest.raises(SonareValueError):
        downmix([np.zeros(8, dtype=np.float32), np.zeros(3, dtype=np.float32)], 0)
