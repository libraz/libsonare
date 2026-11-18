"""An optional count, rate or id is ``None`` when absent, and an explicit 0 is refused.

Each field below reaches a C slot whose 0 means "not given". The cases pair the
omitted spelling with a given value that changes the outcome, and refuse 0 by
name, so the test fails if 0 drifts back to meaning absence.
"""

from __future__ import annotations

import pytest

from libsonare import (
    EngineBounceOptions,
    EngineClip,
    EngineMetronomeConfig,
    EngineMidiClipSchedule,
    Project,
    RealtimeEngine,
    SonareValueError,
)

SR = 48000


def test_move_clip_keeps_the_track_when_none_and_refuses_zero() -> None:
    with Project() as project:
        first = project.add_track("audio", "a")
        second = project.add_track("audio", "b")
        clip = project.add_clip(
            first, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=SR
        )

        def track_of() -> int:
            return {c.id: c.track_id for c in project.clips()}[clip]

        project.move_clip(clip, 240.0)
        assert track_of() == first
        project.move_clip(clip, 240.0, second)
        assert track_of() == second
        with pytest.raises(
            SonareValueError, match=r"^new_track_id must be an integer (?:in|within) \[1, "
        ):
            project.move_clip(clip, 240.0, 0)


def test_metronome_click_length_reads_back_none_when_omitted() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.set_metronome(EngineMetronomeConfig(enabled=True))
        assert engine.metronome().click_samples is None
        engine.set_metronome(EngineMetronomeConfig(enabled=True, click_samples=64))
        assert engine.metronome().click_samples == 64


def test_engine_clip_track_is_none_when_unset_and_zero_is_refused() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        clip = EngineClip(id=1, channels=[[0.5] * 128], start_ppq=0.0)
        assert clip.track_id is None
        engine.set_clips([clip])
        with pytest.raises(
            SonareValueError, match=r"^clip\.track_id must be an integer (?:in|within) \[1, "
        ):
            engine.set_clips([EngineClip(id=1, channels=[[0.5] * 128], start_ppq=0.0, track_id=0)])


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("id", 0),
        ("track_id", 0),
        ("length_samples", 0),
        ("length_samples", -1),
        ("loop_length_samples", 0),
        ("loop_length_samples", -1),
    ],
)
def test_midi_schedule_refuses_zero_where_none_is_absence(field: str, value: int) -> None:
    with (
        RealtimeEngine(sample_rate=SR, max_block_size=128) as engine,
        pytest.raises(
            SonareValueError, match=rf"^clip\.{field} must be an integer (?:in|within) \[1, "
        ),
    ):
        engine.set_midi_clips([EngineMidiClipSchedule(events=[], **{field: value})])


def test_midi_schedule_omitted_fields_are_accepted() -> None:
    schedule = EngineMidiClipSchedule(events=[])
    assert (schedule.id, schedule.track_id, schedule.length_samples) == (None, None, None)
    assert schedule.loop_length_samples is None
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.set_midi_clips([schedule])
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    events=[], id=3, length_samples=512, loop=True, loop_length_samples=256
                )
            ]
        )


def test_bounce_rates_are_none_when_omitted_and_zero_is_refused() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.play()
        kept = engine.bounce_offline(EngineBounceOptions(total_frames=128, num_channels=2))
        assert kept.sample_rate == SR
        halved = engine.bounce_offline(
            EngineBounceOptions(total_frames=128, num_channels=2, target_sample_rate=SR // 2)
        )
        assert halved.sample_rate == SR // 2
        for field in ("target_sample_rate", "source_sample_rate"):
            with pytest.raises(SonareValueError, match=rf"^options\.{field} must be an integer"):
                engine.bounce_offline(
                    EngineBounceOptions(total_frames=128, num_channels=2, **{field: 0})
                )
