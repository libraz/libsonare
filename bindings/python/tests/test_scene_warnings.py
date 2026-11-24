"""Unknown mixing scene keys are refused by path, for a whole scene and a strip fragment."""

from __future__ import annotations

import json
import re
from types import SimpleNamespace

import pytest

from libsonare import (
    ErrorCode,
    Mixer,
    Project,
    RealtimeEngine,
    SonareError,
    SonareValueError,
    _mixing,
    _project_inspection,
    _runtime,
)

from ._helpers import raises_coded, scene_strip

_TYPO_SCENE = (
    '{"version":2,"$schema":"s","strips":[{"id":"lead","faderDB":-3,"x-note":1}],"buses":[]}'
)
_TYPO_STRIP = '{"version":2,"strips":[{"id":"s","faderDB":-3}],"buses":[]}'


def test_unstructured_scene_failure_keeps_the_library_error(monkeypatch) -> None:
    message = "GS EFX requires both bytes of the type"
    fake = SimpleNamespace(
        sonare_project_set_mixer_scene_json=lambda *_: ErrorCode.INVALID_FORMAT,
        sonare_last_error_message=lambda: message.encode(),
        sonare_last_error_reason=lambda: None,
        sonare_last_refusal_subject_count=lambda: 0,
    )
    monkeypatch.setattr(_project_inspection, "_get_lib", lambda: fake)
    monkeypatch.setattr(_runtime, "_get_lib", lambda: fake)
    project = Project.__new__(Project)
    monkeypatch.setattr(project, "_require_handle", lambda: object())

    with pytest.raises(SonareError) as refused:
        project.set_mixer_scene({"version": 2})
    assert type(refused.value) is SonareError
    assert refused.value.code == ErrorCode.INVALID_FORMAT
    assert str(refused.value).endswith(message)
    assert f"scene.{message}" not in str(refused.value)


def test_raw_scene_failure_keeps_literal_key_from_library(monkeypatch) -> None:
    message = "unknown key strips[0].inserts[0].params.drive_db (expected driveDb)"

    def unexpected_render() -> int:
        pytest.fail("document refusal was sent through the facade subject renderer")

    fake = SimpleNamespace(
        sonare_mixer_from_scene_json=lambda *_: None,
        sonare_last_error_code=lambda: ErrorCode.INVALID_FORMAT,
        sonare_last_error_message=lambda: message.encode(),
        sonare_last_error_reason=lambda: None,
        sonare_last_refusal_subject_count=unexpected_render,
    )
    monkeypatch.setattr(_mixing, "_get_lib", lambda: fake)
    monkeypatch.setattr(_runtime, "_get_lib", lambda: fake)

    with raises_coded(ErrorCode.INVALID_FORMAT, match=re.escape(message) + "$"):
        Mixer.from_json('{"version":2}', sample_rate=48000)


def test_project_set_mixer_scene_refuses_an_unknown_key_by_path_and_clears_with_none() -> None:
    with Project() as project:
        assert not hasattr(project, "set_mixer_scene_json")
        with pytest.raises(SonareValueError) as refused:
            project.set_mixer_scene(json.loads(_TYPO_SCENE))
        assert "unknown key scene.strips[0].faderDB" in str(refused.value)

        annotated = json.loads(
            '{"version":2,"$schema":"s","strips":[{"id":"lead","x-note":1}],"buses":[]}'
        )
        project.set_mixer_scene(annotated)
        assert project.mixer_scene() is not None
        project.set_mixer_scene(None)
        assert project.mixer_scene() is None

        with pytest.raises(TypeError, match="scene"):
            project.set_mixer_scene(_TYPO_SCENE)  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="scene must be a mapping"):
            project.set_mixer_scene([])  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match=r"scene\.version must be 2"):
            project.set_mixer_scene({"version": 1})


def test_project_and_engine_insert_parameter_refusals_name_the_scene_path() -> None:
    insert = {"processor": "saturation.softClipper", "params": {"drive": 1.0}}
    with Project() as project, pytest.raises(SonareValueError) as project_refusal:
        project.set_mixer_scene(
            {"version": 2, "strips": [{"id": "lead", "inserts": [insert]}], "buses": []}
        )
    expected = "(expected aliasing, ceiling, driveDb, mix)"
    assert f"unknown key scene.strips[0].inserts[0].params.drive {expected}" in str(
        project_refusal.value
    )

    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_track_buses([{"bus_id": 1}])
        for call in (
            lambda: engine.set_strip(10, {"id": "s", "inserts": [insert]}),
            lambda: engine.set_strip("master", {"id": "s", "inserts": [insert]}),
            lambda: engine.set_bus_strip(1, {"id": "s", "inserts": [insert]}),
        ):
            with pytest.raises(SonareValueError) as engine_refusal:
                call()
            assert str(engine_refusal.value).endswith(
                f"unknown key settings.inserts[0].params.drive {expected}"
            )


def test_loaded_mixer_scene_keeps_unread_key_spelling() -> None:
    scene = {
        "version": 2,
        "strips": [
            {
                "id": "lead",
                "inserts": [{"processor": "saturation.softClipper", "params": {"drive_db": 1.0}}],
            },
        ],
    }
    with raises_coded(
        ErrorCode.INVALID_FORMAT, match=r"unknown key strips\[0\]\.inserts\[0\]\.params\.drive_db"
    ):
        Mixer.from_json(json.dumps(scene), sample_rate=48000)


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
