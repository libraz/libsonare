"""Pan law, automation curve, clip fade and loop, and voice-changer enum fields are exact tokens.

Each takes only its lowerCamel tokens: another spelling is a
``SonareValueError`` naming the accepted ones, and a number (an old ordinal,
or a bool) is a ``TypeError``.
"""

from __future__ import annotations

import dataclasses
from typing import get_args

import pytest

import libsonare
from libsonare import SonareValueError
from libsonare._runtime import _curve_value, _pan_law_value

from ._helpers import LIB_AVAILABLE

SR = 48000

PAN_LAWS = ("const3Db", "const4.5Db", "const6Db", "linear0Db")
CURVES = ("linear", "exponential", "hold", "sCurve")


def test_the_closed_sets_are_literal_types_in_ordinal_order() -> None:
    assert get_args(libsonare.PanLaw) == PAN_LAWS
    assert get_args(libsonare.AutomationCurve) == CURVES
    assert [_pan_law_value(token) for token in PAN_LAWS] == [0, 1, 2, 3]
    assert [_curve_value(token) for token in CURVES] == [0, 1, 2, 3]


@pytest.mark.parametrize(
    ("resolve", "old", "accepted"),
    [
        (_pan_law_value, "const3dB", "const3Db"),
        (_pan_law_value, "linear", "const3Db"),
        (_pan_law_value, "CONST6DB", "const3Db"),
        (_curve_value, "s-curve", "sCurve"),
        (_curve_value, "exp", "sCurve"),
        (_curve_value, "Linear", "sCurve"),
    ],
)
def test_another_spelling_is_refused_and_names_the_tokens(resolve, old: str, accepted: str) -> None:
    with pytest.raises(SonareValueError, match=accepted):
        resolve(old)


@pytest.mark.parametrize("resolve", [_pan_law_value, _curve_value])
@pytest.mark.parametrize("value", [0, 3, True, 1.0])
def test_a_number_is_a_type_error(resolve, value: object) -> None:
    with pytest.raises(TypeError):
        resolve(value)


@pytest.fixture
def clip_project():
    if not LIB_AVAILABLE:
        pytest.skip("libsonare shared library missing")
    project = libsonare.Project()
    track = project.add_track("audio", "gtr")
    clip = project.add_clip(track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=SR)
    return project, clip


def test_a_fade_curve_ordinal_is_a_type_error(clip_project) -> None:
    project, clip = clip_project
    project.set_clip_fade(clip, 4.0, 4.0, fade_in_curve="equalPower")
    for value in (1, True):
        with pytest.raises(TypeError):
            project.set_clip_fade(clip, 4.0, 4.0, fade_in_curve=value)  # type: ignore[arg-type]
    with pytest.raises(SonareValueError, match="equalPower"):
        project.set_clip_fade(clip, 4.0, 4.0, fade_out_curve="EqualPower")  # type: ignore[arg-type]


def test_the_loop_mode_is_off_or_loop(clip_project) -> None:
    project, clip = clip_project
    project.set_clip_loop(clip, "loop", 240.0)
    project.set_clip_loop(clip, "off")
    for old in ("on", "none", "Loop"):
        with pytest.raises(SonareValueError, match="loop"):
            project.set_clip_loop(clip, old, 240.0)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        project.set_clip_loop(clip, 1, 240.0)  # type: ignore[arg-type]


@pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")
@pytest.mark.parametrize("field", ["character_type", "voice_engine", "midi_release"])
def test_a_voice_changer_enum_ordinal_is_a_type_error(field: str) -> None:
    base = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    with libsonare.RealtimeVoiceChanger(SR) as changer, pytest.raises(TypeError, match=field):
        changer.set_config_pod(dataclasses.replace(base, **{field: 1}))
