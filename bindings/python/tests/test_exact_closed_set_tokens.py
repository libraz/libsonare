"""Mixer, automation, clip fade and loop, and voice-changer closed sets are exact tokens.

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
from libsonare._project_instruments import _synth_waveform_value
from libsonare._project_synth import (
    _articulation_value,
    _controller_axis_value,
    _controller_input_value,
    _mpe_dimension_value,
    _note_tracking_value,
    _part_rig_mode_value,
    _sample_desc_loop_value,
    _sample_key_track_value,
    _sample_loop_value,
)
from libsonare._runtime import (
    _channel_layout_value,
    _curve_value,
    _meter_tap_value,
    _pan_law_value,
    _pan_mode_value,
    _send_timing_value,
    _sidechain_source_kind_value,
)

from ._helpers import LIB_AVAILABLE

SR = 48000

PAN_LAWS = ("const3Db", "const4.5Db", "const6Db", "linear0Db")
CURVES = ("linear", "exponential", "hold", "sCurve")


def test_the_closed_sets_are_literal_types_in_ordinal_order() -> None:
    assert get_args(libsonare.PanLaw) == PAN_LAWS
    assert get_args(libsonare.AutomationCurve) == CURVES
    assert [_pan_law_value(token) for token in PAN_LAWS] == [0, 1, 2, 3]
    assert [_curve_value(token) for token in CURVES] == [0, 1, 2, 3]


# Each Literal in ordinal order, except SendTiming, whose C ordinals put postFader first.
MIXER_SETS = [
    ("PanMode", _pan_mode_value, ("balance", "stereoPan", "dualPan")),
    ("MeterTap", _meter_tap_value, ("preFader", "postFader")),
    ("SendTiming", _send_timing_value, ("postFader", "preFader")),
    ("SidechainSourceKind", _sidechain_source_kind_value, ("track", "bus")),
    ("ChannelLayout", _channel_layout_value, ("mono", "stereo", "5.1", "7.1")),
]


@pytest.mark.parametrize(("name", "resolve", "tokens"), MIXER_SETS)
def test_the_mixer_closed_sets_are_literal_types(name: str, resolve, tokens: tuple) -> None:
    assert set(get_args(getattr(libsonare, name))) == set(tokens)
    assert [resolve(token) for token in tokens] == list(range(len(tokens)))
    with pytest.raises(SonareValueError, match=tokens[-1]):
        resolve(tokens[-1] + " ")
    for number in (0, True):
        with pytest.raises(TypeError):
            resolve(number)


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
    clip = project.add_clip(
        track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=SR
    )
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
    with (
        libsonare.RealtimeVoiceChanger(SR, num_channels=1) as changer,
        pytest.raises(TypeError, match=field),
    ):
        changer.set_config(dataclasses.replace(base, **{field: 1}))


# (public Literal, resolver, {token: C value}); the SF2 loop modes skip 2 and "sawtooth" is "saw".
SYNTH_SETS = [
    (
        "SampleLoopMode",
        _sample_loop_value,
        {"default": 0, "none": 1, "continuous": 2, "keyDown": 3},
    ),
    ("SampleKeyTrack", _sample_key_track_value, {"default": 0, "on": 1, "off": 2}),
    ("SampleDescLoopMode", _sample_desc_loop_value, {"none": 0, "continuous": 1, "keyDown": 3}),
    (
        "SynthWaveform",
        _synth_waveform_value,
        {"sine": 0, "saw": 1, "sawtooth": 1, "square": 2, "triangle": 3},
    ),
    (
        "ControllerInput",
        _controller_input_value,
        {
            "controlChange": 0,
            "channelPressure": 1,
            "polyPressure": 2,
            "pitchBend": 3,
            "velocity": 4,
        },
    ),
    ("ControllerAxis", _controller_axis_value, None),
    ("Articulation", _articulation_value, {"poly": 0, "monoRetrigger": 1, "monoLegato": 2}),
    ("MpeDimension", _mpe_dimension_value, {"bend": 0, "pressure": 1, "timbre": 2}),
    ("NoteTracking", _note_tracking_value, {"last": 0, "lowest": 1, "highest": 2, "all": 3}),
    ("PartRigMode", _part_rig_mode_value, {"bank": 0, "none": 1, "chain": 2}),
]


@pytest.mark.parametrize(("name", "resolve", "values"), SYNTH_SETS, ids=[s[0] for s in SYNTH_SETS])
def test_the_synth_closed_sets_take_exact_tokens(name: str, resolve, values: dict | None) -> None:
    tokens = get_args(getattr(libsonare, name))
    if values is None:
        values = {token: ordinal for ordinal, token in enumerate(tokens)}
    assert tokens == tuple(values)
    assert {token: resolve(token) for token in tokens} == values
    with pytest.raises(SonareValueError, match=tokens[-1]):
        resolve(tokens[-1].upper())
    for number in (0, 1, True):
        with pytest.raises(TypeError):
            resolve(number)
