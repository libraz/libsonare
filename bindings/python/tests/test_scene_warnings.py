"""Unknown mixing scene keys are refused by path, for a whole scene and a strip fragment."""

from __future__ import annotations

import pytest

from libsonare import ErrorCode, Project, RealtimeEngine, SonareError, SonareValueError

from ._helpers import scene_strip

_TYPO_SCENE = (
    '{"version":2,"$schema":"s","strips":[{"id":"lead","faderDB":-3,"x-note":1}],"buses":[]}'
)
_TYPO_STRIP = '{"version":2,"strips":[{"id":"s","faderDB":-3}],"buses":[]}'


def test_project_set_mixer_scene_json_refuses_an_unknown_key_by_path() -> None:
    with Project() as project:
        with pytest.raises(SonareError) as refused:
            project.set_mixer_scene_json(_TYPO_SCENE)
        assert not isinstance(refused.value, SonareValueError)
        assert refused.value.code == int(ErrorCode.INVALID_FORMAT)
        assert "unknown key strips[0].faderDB" in str(refused.value)
        annotated = '{"version":2,"$schema":"s","strips":[{"id":"lead","x-note":1}],"buses":[]}'
        assert project.set_mixer_scene_json(annotated) is None


def test_engine_strip_setters_refuse_an_unknown_key() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        for call in (
            lambda: engine.set_strip(10, scene_strip(_TYPO_STRIP)),
            lambda: engine.set_strip("master", scene_strip(_TYPO_STRIP)),
        ):
            with pytest.raises(SonareValueError) as refused:
                call()
            assert refused.value.code == int(ErrorCode.INVALID_PARAMETER)
            assert "unknown key settings.faderDB" in str(refused.value)
        annotated = '{"$schema":"s","version":2,"strips":[{"id":"s","faderDb":-3,"x-t":1}]}'
        engine.set_strip(10, scene_strip(annotated))
