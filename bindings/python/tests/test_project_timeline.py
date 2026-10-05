"""Compiled project timelines applied to a realtime engine.

Python counterpart of ``tests/api/sonare_c_project_timeline_test.cpp``: an
engine that applied a compiled timeline renders exactly what the project bounce
renders.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    BuiltinSynthConfig,
    EngineBounceOptions,
    Project,
    ProjectTimeline,
    RealtimeEngine,
    SonareError,
)

from ._project_helpers import _make_stereo_sine

_RATE = 48000
_BLOCK = 128
_FRAMES = _BLOCK * 180  # 0.48 s
_DESTINATION = 9
_SYNTH = BuiltinSynthConfig(waveform="saw", gain=0.3, attack_ms=1.0, release_ms=20.0, polyphony=4)


def _build_project() -> tuple[Project, int]:
    """One audio clip and one MIDI clip routed to ``_DESTINATION`` at 120 BPM."""
    project = Project()
    project.set_sample_rate(float(_RATE))
    audio_track = project.add_track("audio", "audio")
    audio_clip = project.add_clip(
        audio_track,
        start_ppq=0.0,
        length_ppq=0.5,
        audio=_make_stereo_sine(_RATE // 4),
        audio_channels=2,
        audio_sample_rate=_RATE,
    )
    midi_track, midi_clip = project.add_midi_clip(0.0, 0.9)
    project.set_midi_events(
        midi_clip,
        [
            (0.1, 0x20903C60, 0),
            (0.5, 0x20803C00, 0),
            (0.5, 0x20904060, 0),
            (0.85, 0x20804000, 0),
        ],
    )
    project.set_track_midi_destination(midi_track, _DESTINATION)
    return project, audio_clip


def _project_bounce(project: Project) -> np.ndarray:
    return project.bounce_with_builtin_instrument(
        _SYNTH,
        destination_id=_DESTINATION,
        total_frames=_FRAMES,
        block_size=_BLOCK,
        num_channels=2,
        sample_rate=_RATE,
    )


def _prepared_engine() -> RealtimeEngine:
    return RealtimeEngine(sample_rate=float(_RATE), max_block_size=_BLOCK)


def _silent_block() -> list[list[float]]:
    return [[0.0] * _BLOCK, [0.0] * _BLOCK]


def _stop_and_rewind(engine: RealtimeEngine) -> None:
    engine.stop()
    engine.seek_sample(0)
    engine.process(_silent_block())
    assert not engine.transport_state().playing


def _engine_bounce(engine: RealtimeEngine) -> np.ndarray:
    engine.play()
    result = engine.bounce_offline(
        EngineBounceOptions(
            total_frames=_FRAMES,
            block_size=_BLOCK,
            num_channels=2,
            source_sample_rate=_RATE,
            target_sample_rate=_RATE,
        )
    )
    _stop_and_rewind(engine)
    assert result.frames == _FRAMES
    return np.asarray(result.interleaved, dtype=np.float32).reshape(-1, 2)


def _compile(project: Project) -> ProjectTimeline:
    result = project.compile_timeline()
    assert result.has_timeline is True
    assert result.timeline is not None
    return result.timeline


def _apply(engine: RealtimeEngine, project: Project, *, close_early: bool = False) -> None:
    # A fresh instrument per apply so voice state from a prior render cannot leak.
    engine.set_builtin_instrument(_SYNTH, _DESTINATION)
    timeline = _compile(project)
    try:
        engine.apply_project_timeline(timeline)
        if close_early:
            timeline.close()
    finally:
        timeline.close()


def test_applied_timeline_renders_exactly_like_project_bounce_across_edits() -> None:
    project, audio_clip = _build_project()
    with project, _prepared_engine() as engine:

        def require_match() -> np.ndarray:
            _apply(engine, project)
            live = _engine_bounce(engine)
            bounced = _project_bounce(project)
            assert float(np.max(np.abs(bounced))) > 0.01
            assert np.array_equal(live, bounced)
            return bounced

        initial = require_match()
        project.move_clip(audio_clip, 0.25)
        moved = require_match()
        assert not np.array_equal(moved, initial)


def test_timeline_survives_close_right_after_apply() -> None:
    project, _ = _build_project()
    with project, _prepared_engine() as engine:
        _apply(engine, project, close_early=True)
        assert np.array_equal(_engine_bounce(engine), _project_bounce(project))


def test_timeline_close_is_idempotent_and_context_managed() -> None:
    project, _ = _build_project()
    with project:
        with _compile(project) as timeline:
            assert isinstance(timeline, ProjectTimeline)
        timeline.close()
        with _prepared_engine() as engine, pytest.raises(RuntimeError, match="closed"):
            engine.apply_project_timeline(timeline)


def test_apply_while_playing_raises_and_leaves_engine_unchanged() -> None:
    project, _ = _build_project()
    with project, _prepared_engine() as engine:
        engine.set_builtin_instrument(_SYNTH, _DESTINATION)
        engine.play()
        engine.process(_silent_block())
        assert engine.transport_state().playing
        with _compile(project) as timeline, pytest.raises(SonareError):
            engine.apply_project_timeline(timeline)
        assert engine.clip_count() == 0


def test_compile_failure_has_no_timeline_and_reports_diagnostics() -> None:
    # A clip naming a source the project does not hold is a compile error.
    json = (
        '{"version":1,"sample_rate":48000,"tracks":[{"id":1,"name":"audio","kind":0,'
        '"channel_strip_ref":"","output_target":"","midi_destination_id":0,'
        '"automation_lanes":[]}],"clips":[{"id":1,"track_id":1,"source_id":99,'
        '"start_ppq":0,"length_ppq":1,"source_offset_ppq":0,"gain":1,'
        '"fade_in":{"length_ppq":0,"curve":0},"fade_out":{"length_ppq":0,"curve":0},'
        '"loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0}]}'
    )
    with Project.from_json(json) as project:
        result = project.compile_timeline()
        expected = project.compile()
        assert result.timeline is None
        assert result.has_timeline is False
        assert result.diagnostic_count > 0
        assert result.diagnostics == expected.diagnostics
        assert result.messages == expected.messages
