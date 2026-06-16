"""Property tests for the Project serialize/deserialize round-trip.

``test_project.py`` pins byte-stable serialization for a single curated
arrangement. This module complements that golden by fuzzing the *content* value
space: it builds many randomized-but-valid projects (varying sample rate, tempo
map, audio/MIDI tracks, clips, and events) with a seeded RNG and asserts the
invariant that serialization is a faithful, stable round-trip --

    from_json(to_json_bytes(p)).to_json_bytes() == to_json_bytes(p)

across all of them. A serializer that drops, reorders, or non-deterministically
formats a field only for certain content shapes is invisible to a single golden
but fails here. Seeds are fixed so any failure reproduces deterministically.
"""

from __future__ import annotations

import random

import numpy as np
import pytest

from libsonare import Project

SAMPLE_RATES = (44100.0, 48000.0, 96000.0)
TRACK_NAMES = ("lead", "bass", "drums", "pad", "fx", "vox-é", "ベース", "")


def _random_name(rng: random.Random) -> str | None:
    choice = rng.choice((*TRACK_NAMES, None))
    return choice


def _random_audio(rng: random.Random) -> tuple[np.ndarray, int]:
    """A small interleaved buffer plus its channel count (kept tiny: no DSP runs)."""
    channels = rng.choice((1, 2))
    frames = rng.randint(1, 64)
    data = np.asarray([rng.uniform(-1.0, 1.0) for _ in range(frames * channels)], dtype=np.float32)
    return data, channels


def _add_random_audio_track(project: Project, rng: random.Random) -> None:
    track = project.add_track("audio", _random_name(rng))
    # Place clips sequentially with a gap: the default overlap policy rejects
    # overlapping clips on a track, so a cursor keeps every clip valid.
    cursor = round(rng.uniform(0.0, 4.0), 3)
    for _ in range(rng.randint(0, 3)):
        length = round(rng.uniform(0.25, 16.0), 3)
        audio, channels = _random_audio(rng)
        project.add_clip(
            track,
            start_ppq=cursor,
            length_ppq=length,
            gain=round(rng.uniform(0.0, 1.5), 3),
            audio=audio,
            audio_channels=channels,
            audio_sample_rate=int(rng.choice(SAMPLE_RATES)),
        )
        cursor = round(cursor + length + rng.uniform(0.1, 4.0), 3)


def _add_random_midi_track(project: Project, rng: random.Random) -> None:
    length = round(rng.uniform(1.0, 16.0), 3)
    _track, clip = project.add_midi_clip(round(rng.uniform(0.0, 8.0), 3), length)
    events: list[tuple[float, int, int]] = []
    for _ in range(rng.randint(0, 12)):
        ppq = round(rng.uniform(0.0, length), 4)
        group = rng.randint(0, 15)
        channel = rng.randint(0, 15)
        kind = rng.random()
        if kind < 0.4:
            note, vel = rng.randint(0, 127), rng.randint(1, 127)
            events.append(Project.midi_note_on(ppq, group, channel, note, vel))
            events.append(Project.midi_note_off(min(ppq + 0.5, length), group, channel, note, 0))
        elif kind < 0.7:
            events.append(
                Project.midi_cc(ppq, group, channel, rng.randint(0, 127), rng.randint(0, 127))
            )
        else:
            events.append(Project.midi_program(ppq, group, channel, rng.randint(0, 127)))
    if events:
        project.set_midi_events(clip, events)


def _random_tempo_map(rng: random.Random) -> list[tuple[float, float, float]]:
    starts = sorted({0.0, *(round(rng.uniform(1.0, 64.0), 2) for _ in range(rng.randint(0, 3)))})
    return [(s, round(rng.uniform(40.0, 240.0), 3), 0.0) for s in starts]


def build_random_project(seed: int) -> Project:
    """Construct a randomized-but-valid project from a fixed seed."""
    rng = random.Random(seed)
    project = Project()
    project.set_sample_rate(rng.choice(SAMPLE_RATES))
    project.set_tempo_segments(_random_tempo_map(rng))
    for _ in range(rng.randint(0, 3)):
        _add_random_audio_track(project, rng)
    for _ in range(rng.randint(0, 3)):
        _add_random_midi_track(project, rng)
    return project


def _assert_round_trips(project: Project) -> None:
    """serialize -> deserialize -> serialize must be byte-identical."""
    first = project.to_json_bytes()
    assert first
    restored = Project.from_json(first)
    try:
        assert restored.to_json_bytes() == first
    finally:
        restored.close()


@pytest.mark.parametrize("seed", range(40))
def test_random_project_round_trips_byte_identically(seed: int) -> None:
    project = build_random_project(seed)
    try:
        _assert_round_trips(project)
    finally:
        project.close()


def test_empty_project_round_trips() -> None:
    project = Project()
    try:
        _assert_round_trips(project)
    finally:
        project.close()


def test_boundary_values_round_trip() -> None:
    """Extreme-but-valid content (dense events, long ppq, unicode names) is stable."""
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        project.set_tempo_segments([(0.0, 40.0, 0.0), (1000.0, 240.0, 0.0)])
        track = project.add_track("audio", "ユニコード-name-é")
        project.add_clip(
            track,
            start_ppq=0.0,
            length_ppq=1_000_000.0,
            audio=np.zeros(4, dtype=np.float32),
            audio_channels=2,
            audio_sample_rate=48000,
        )
        _midi_track, clip = project.add_midi_clip(0.0, 256.0)
        events: list[tuple[float, int, int]] = []
        for i in range(256):
            events.append(Project.midi_note_on(float(i), 0, i % 16, i % 128, 1 + (i % 127)))
            events.append(Project.midi_note_off(float(i) + 0.25, 0, i % 16, i % 128, 0))
        project.set_midi_events(clip, events)
        _assert_round_trips(project)
    finally:
        project.close()
