"""Shared builders for the Project test suites."""

from __future__ import annotations

import math

import numpy as np

from libsonare import (
    Project,
)


def _make_stereo_sine(frames: int, sample_rate: float = 48000.0) -> np.ndarray:
    """Build interleaved stereo samples mirroring the C parity fixture."""
    t = np.arange(frames, dtype=np.float64) / sample_rate
    left = 0.25 * np.sin(2.0 * math.pi * 220.0 * t)
    right = 0.18 * np.sin(2.0 * math.pi * 330.0 * t)
    interleaved = np.empty(frames * 2, dtype=np.float32)
    interleaved[0::2] = left.astype(np.float32)
    interleaved[1::2] = right.astype(np.float32)
    return interleaved


def _build_project() -> tuple[Project, int, int, int]:
    """Build a small non-trivial project: audio track+clip and MIDI track+clip."""
    project = Project()
    project.set_sample_rate(48000.0)

    audio_track = project.add_track("audio", "audio")
    assert audio_track != 0

    audio = _make_stereo_sine(48000)
    audio_clip = project.add_clip(
        audio_track,
        start_ppq=0.0,
        length_ppq=2.0,
        audio=audio,
        audio_channels=2,
        audio_sample_rate=48000,
    )
    assert audio_clip != 0

    midi_track, midi_clip = project.add_midi_clip(0.0, 4.0)
    assert midi_clip != 0

    project.set_midi_events(
        midi_clip,
        [(0.0, 0x20903C40, 0), (1.0, 0x20803C00, 0)],
    )
    return project, audio_clip, midi_track, midi_clip
