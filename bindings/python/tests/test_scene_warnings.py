"""Unknown mixing scene keys: a whole scene reports them, a strip fragment refuses them."""

from __future__ import annotations

import pytest

from libsonare import Project, RealtimeEngine, SonareError

_TYPO_SCENE = (
    '{"version":1,"$schema":"s","strips":[{"id":"lead","faderDB":-3,"x-note":1}],"buses":[]}'
)
_TYPO_STRIP = '{"version":1,"strips":[{"id":"s","faderDB":-3}],"buses":[]}'


def test_project_set_mixer_scene_json_returns_unknown_key_warnings() -> None:
    project = Project()
    assert project.set_mixer_scene_json(_TYPO_SCENE) == ["unknown scene key 'strips[0].faderDB'"]
    assert project.set_mixer_scene_json('{"version":1,"strips":[{"id":"lead"}]}') == []


def test_engine_strip_setters_refuse_an_unknown_key() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_lanes([10])
        for call in (
            lambda: engine.set_track_strip_json(10, _TYPO_STRIP),
            lambda: engine.set_master_strip_json(_TYPO_STRIP),
        ):
            with pytest.raises(SonareError) as refused:
                call()
            assert refused.value.code == 4
            assert "unknown strip key 'strips[0].faderDB'" in str(refused.value)
        annotated = '{"$schema":"s","version":1,"strips":[{"id":"s","faderDb":-3,"x-t":1}]}'
        engine.set_track_strip_json(10, annotated)
