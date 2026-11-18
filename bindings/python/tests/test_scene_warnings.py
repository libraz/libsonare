"""Unknown mixing scene keys are refused by path, for a whole scene and a strip fragment."""

from __future__ import annotations

import pytest

from libsonare import ErrorCode, Project, RealtimeEngine, SonareError, SonareValueError

_TYPO_SCENE = (
    '{"version":1,"$schema":"s","strips":[{"id":"lead","faderDB":-3,"x-note":1}],"buses":[]}'
)
_TYPO_STRIP = '{"version":1,"strips":[{"id":"s","faderDB":-3}],"buses":[]}'


def test_project_set_mixer_scene_json_refuses_an_unknown_key_by_path() -> None:
    with Project() as project:
        with pytest.raises(SonareError) as refused:
            project.set_mixer_scene_json(_TYPO_SCENE)
        assert not isinstance(refused.value, SonareValueError)
        assert refused.value.code == int(ErrorCode.INVALID_FORMAT)
        assert "unknown scene key 'strips[0].faderDB'" in str(refused.value)
        annotated = '{"version":1,"$schema":"s","strips":[{"id":"lead","x-note":1}],"buses":[]}'
        assert project.set_mixer_scene_json(annotated) is None


def test_engine_strip_setters_refuse_an_unknown_key() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        for call in (
            lambda: engine.set_track_strip_json(10, _TYPO_STRIP),
            lambda: engine.set_master_strip_json(_TYPO_STRIP),
        ):
            with pytest.raises(SonareError) as refused:
                call()
            assert not isinstance(refused.value, SonareValueError)
            assert refused.value.code == int(ErrorCode.INVALID_FORMAT)
            assert "unknown scene key 'strips[0].faderDB'" in str(refused.value)
        annotated = '{"$schema":"s","version":1,"strips":[{"id":"s","faderDb":-3,"x-t":1}]}'
        engine.set_track_strip_json(10, annotated)
