"""A caller's number is refused, not folded into a legal one.

ctypes applies the C conversion on both routes into the library -- an argument
constructor and a struct field assignment -- so an out-of-range value arrives as
a different, legal number rather than as an error. The value that matters is not
the implausible one: ``2**31`` wraps negative and the core's own guards catch it,
while ``2**32 + 8`` lands on 8, which is a setting a caller could have asked for.

Each case drives one field behind one reader, and each opens with a positive
control -- two legitimate values whose results differ. Without it an accepted
out-of-range value cannot be told from a field the entry point never reads.
"""

from __future__ import annotations

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError

SAMPLE_RATE = 22050


@pytest.fixture(scope="module")
def tone() -> np.ndarray:
    n = SAMPLE_RATE // 2
    return (0.3 * np.sin(2 * np.pi * 220 * np.arange(n) / SAMPLE_RATE)).astype(np.float32)


def _mel(tone: np.ndarray, n_mels: int):
    return ls.mel_spectrogram(
        tone, sample_rate=SAMPLE_RATE, n_fft=512, hop_length=256, n_mels=n_mels
    )


def test_a_wrapped_count_is_refused_rather_than_read_as_a_smaller_one(tone) -> None:
    """The signed-int argument reader, driven where the wrap lands in domain."""
    assert _mel(tone, 8).n_mels != _mel(tone, 16).n_mels  # positive control
    for value in (2**32 + 8, 2**32 + 16, 2**32, 2**31, -(2**31) - 1):
        with pytest.raises(SonareValueError, match="n_mels"):
            _mel(tone, value)


def test_a_wrapped_struct_field_is_refused_rather_than_read_as_a_smaller_one() -> None:
    """The struct-field route: assignment applies the same conversion."""

    def frames(n_fft: int) -> int:
        analyzer = ls.StreamAnalyzer(
            ls.StreamConfig(sample_rate=8000, n_fft=n_fft, hop_length=16, n_mels=8)
        )
        analyzer.process([0.01 * i for i in range(512)])
        return analyzer.stats().total_frames

    assert frames(32) != frames(64)  # positive control
    for value in (2**32 + 32, 2**32 + 64, 2**32):
        with pytest.raises(SonareValueError, match="n_fft"):
            frames(value)


def test_a_negative_size_is_refused_rather_than_read_as_the_largest_one() -> None:
    """The unsigned readers: a negative wraps to the top of the range, not to zero."""
    with ls.StreamAnalyzer(
        ls.StreamConfig(sample_rate=8000, n_fft=32, hop_length=32, n_mels=8)
    ) as analyzer:
        analyzer.process([0.01 * i for i in range(512)])
        assert analyzer.read_frames(1).n_frames != analyzer.read_frames(3).n_frames
        for value in (-1, -2, 2**64, 2**64 + 2):
            with pytest.raises(SonareValueError, match="max_frames"):
                analyzer.read_frames(value)


def test_a_wrapped_bitmask_is_refused_rather_than_read_as_a_different_scale() -> None:
    """The 16-bit reader: 2**16 + mask is the same mask with a different meaning."""
    assert ls.scale_quantize_midi(0, 0b101010110101, 61.4) != ls.scale_quantize_midi(0, 1, 61.4)
    for value in (-1, 2**16, 2**16 + 0b101010110101):
        with pytest.raises(SonareValueError, match="mode_mask"):
            ls.scale_quantize_midi(0, value, 61.4)


def test_an_out_of_range_sample_position_is_refused(mixer_scene) -> None:
    """The 64-bit reader."""
    mixer_scene.schedule_fader_automation("vocal", 0, -6.0, 0)
    for value in (2**63, 2**63 + 2, -(2**63) - 1):
        with pytest.raises(SonareValueError, match="sample_pos"):
            mixer_scene.schedule_fader_automation("vocal", value, -6.0, 0)


def test_an_out_of_range_index_is_refused(mixer_scene) -> None:
    """The unsigned 32-bit readers behind an insert automation target."""
    mixer_scene.schedule_insert_automation(0, 0, 0, 0, 0.5, 0)
    for value in (-1, 2**32, 2**32 + 2):
        with pytest.raises(SonareValueError, match="insert_index"):
            mixer_scene.schedule_insert_automation(0, value, 0, 0, 0.5, 0)
        with pytest.raises(SonareValueError, match="param_id"):
            mixer_scene.schedule_insert_automation(0, 0, value, 0, 0.5, 0)


@pytest.fixture
def mixer_scene():
    scene = ls.mixing_scene_preset_json(ls.mixing_scene_preset_names()[0])
    mixer = ls.Mixer.from_scene_json(scene, sample_rate=48000, block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()
