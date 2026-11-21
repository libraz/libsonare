"""Pitch-correction, harmony, vocoder, character and MIDI surface of RealtimeVoiceChanger."""

from __future__ import annotations

import ctypes
import dataclasses
import math
from typing import get_args

import numpy as np
import pytest

import libsonare
from libsonare import ErrorCode, SonareError, SonareValueError
from libsonare._ffi import SonareRealtimeVoiceChangerConfig

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SAMPLE_RATE = 48000
NEW_PRESETS = [
    "hardTune",
    "robotMonotone",
    "choirHarmony",
    "vocoderStrings",
    "megaphone",
    "radioVoice",
]


def _tone(count: int = 4800, hz: float = 220.0) -> np.ndarray:
    return (0.2 * np.sin(2 * math.pi * hz * np.arange(count) / SAMPLE_RATE)).astype(np.float32)


def _neutral() -> libsonare.RealtimeVoiceChangerConfig:
    return libsonare.realtime_voice_changer_preset_config("neutralMonitor")


def _flat(config: libsonare.RealtimeVoiceChangerConfig) -> dict[str, object]:
    out: dict[str, object] = {}
    for f in dataclasses.fields(config):
        camel = "".join(
            part if i == 0 else part.capitalize() for i, part in enumerate(f.name.split("_"))
        )
        value = getattr(config, f.name)
        if value is None:  # an automatic grain is spelled by omission
            continue
        if f.name == "harmony_steps":
            out["harmonySteps"] = list(value)
        elif camel in ("limiterEnableIspLimiter", "vocoderEnabled"):
            out[camel] = bool(value)
        else:
            out[camel] = value
    return out


def _changer(
    config: libsonare.RealtimeVoiceChangerConfig, num_channels: int = 1
) -> libsonare.RealtimeVoiceChanger:
    return libsonare.RealtimeVoiceChanger(
        SAMPLE_RATE, num_channels=num_channels, config=_flat(config)
    )


def test_neutral_preset_selects_the_psola_engine() -> None:
    cfg = _neutral()
    assert cfg.voice_engine == "psola"
    assert cfg.pitch_correction_mode == "off"
    assert cfg.vocoder_enabled is False
    assert len(cfg.harmony_steps) == 3


@pytest.mark.parametrize("name", NEW_PRESETS)
def test_new_presets_resolve_by_name_and_id(name: str) -> None:
    names = libsonare.realtime_voice_changer_preset_names()
    assert name in names
    ordinal = names.index(name)
    assert isinstance(
        libsonare.realtime_voice_changer_preset_config(name), libsonare.RealtimeVoiceChangerConfig
    )
    assert libsonare.voice_character_preset_id(ordinal) == name
    with libsonare.RealtimeVoiceChanger(
        SAMPLE_RATE, max_block_size=128, num_channels=1, config=name
    ) as changer:
        out = changer.process_mono(_tone())
        assert np.isfinite(out).all()


def test_preset_list_has_twelve_ordered_entries() -> None:
    names = libsonare.realtime_voice_changer_preset_names()
    assert len(names) == 12
    assert names[6:] == NEW_PRESETS
    # The Literal lists the ids in the core's ordinal order.
    assert tuple(names) == get_args(libsonare.VoicePresetId)


def test_new_fields_round_trip_through_the_pod() -> None:
    base = _neutral()
    cfg = dataclasses.replace(
        base,
        pitch_correction_mode="auto",
        pitch_correction_scale="naturalMinor",
        pitch_correction_key_root=9,
        pitch_correction_amount=0.75,
        harmony_voices=2,
        harmony_steps=(5, -2, -7),
        character_type="bbdChorus",
        reverb_variation="tempoDelay",
        reverb_variation_division="dottedEighth",
        vocoder_carrier_wave="ensemble",
        midi_release="input",
    )
    pod = cfg._to_pod()
    assert pod.struct_size == ctypes.sizeof(SonareRealtimeVoiceChangerConfig)
    assert list(pod.harmony_steps) == [5, -2, -7]
    assert pod.pitch_correction_scale == 1
    assert pod.reverb_variation_division == 2
    assert libsonare.RealtimeVoiceChangerConfig._from_pod(pod) == cfg
    with _changer(base) as changer:
        changer.set_config(cfg)
        live = changer.config
        assert live.harmony_steps == (5, -2, -7)
        assert live.pitch_correction_scale == "naturalMinor"
        assert live.character_type == "bbdChorus"
        assert live.midi_release == "input"
        assert live.pitch_correction_amount == pytest.approx(0.75, abs=1e-6)


def test_pod_mirror_places_harmony_steps_as_a_three_element_int_array() -> None:
    fields = dict((name, ctype) for name, ctype in SonareRealtimeVoiceChangerConfig._fields_)
    assert fields["harmony_steps"]._length_ == 3
    assert fields["harmony_steps"]._type_ is fields["harmony_voices"]


@pytest.mark.parametrize(
    "field",
    [
        "voice_engine",
        "pitch_correction_mode",
        "pitch_correction_scale",
        "vocoder_carrier_source",
        "vocoder_carrier_wave",
        "character_type",
        "reverb_variation",
        "reverb_variation_division",
        "midi_release",
    ],
)
def test_unknown_enum_spelling_is_refused(field: str) -> None:
    with pytest.raises(SonareValueError, match=field):
        dataclasses.replace(_neutral(), **{field: "sideways"})._to_pod()


def test_harmony_steps_must_hold_exactly_three_integers() -> None:
    base = _neutral()
    for steps in [(1, 2), (1, 2, 3, 4), ()]:
        with pytest.raises(SonareValueError, match="harmony_steps"):
            dataclasses.replace(base, harmony_steps=steps)._to_pod()
    with pytest.raises(TypeError, match=r"harmony_steps\[1\]"):
        dataclasses.replace(base, harmony_steps=(0, True, 0))._to_pod()
    with pytest.raises(SonareValueError, match=r"harmony_steps\[2\]"):
        dataclasses.replace(base, harmony_steps=(0, 0, 2**31))._to_pod()
    with pytest.raises(SonareValueError, match=r"harmony_steps\[0\]"):
        dataclasses.replace(base, harmony_steps=(1.5, 0, 0))._to_pod()
    with pytest.raises(TypeError, match="harmony_steps"):
        dataclasses.replace(base, harmony_steps=3)._to_pod()  # type: ignore[arg-type]


def test_numeric_fields_refuse_what_a_c_type_would_change() -> None:
    base = _neutral()
    with pytest.raises(TypeError):
        dataclasses.replace(base, vocoder_bands=True)._to_pod()
    with pytest.raises(TypeError):
        dataclasses.replace(base, harmony_level=True)._to_pod()
    with pytest.raises(SonareValueError):
        dataclasses.replace(base, vocoder_bands=20.5)._to_pod()
    with pytest.raises(SonareValueError):
        dataclasses.replace(base, vocoder_bands=2**32)._to_pod()
    with pytest.raises(SonareValueError):
        dataclasses.replace(base, pitch_correction_key_root=2**31)._to_pod()
    with pytest.raises(SonareValueError):
        dataclasses.replace(base, character_amount=float("nan"))._to_pod()


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("pitch_correction_amount", 1.5),
        ("pitch_correction_key_root", 12),
        ("pitch_correction_robot_octave", 2),
        ("harmony_voices", 4),
        ("vocoder_bands", 7),
        ("vocoder_bands", 33),
        ("character_tone", -1.5),
        ("reverb_variation_feedback", 0.96),
        ("reverb_variation_tempo_bpm", 29.0),
        ("voice_engine_f0_min_hz", 49.0),
    ],
)
def test_out_of_range_new_field_is_refused_and_the_previous_config_kept(field, value) -> None:
    base = _neutral()
    with _changer(base) as changer:
        before = changer.config
        with pytest.raises(SonareValueError):
            changer.set_config(dataclasses.replace(base, **{field: value}))
        assert changer.config == before


def test_range_edges_are_accepted() -> None:
    base = _neutral()
    with _changer(base) as changer:
        edge = dataclasses.replace(
            base,
            pitch_correction_amount=1.0,
            pitch_correction_key_root=11,
            pitch_correction_robot_octave=-1,
            harmony_voices=3,
            harmony_steps=(-7, 0, 7),
            character_tone=-1.0,
            reverb_variation_feedback=0.95,
            reverb_variation_tempo_bpm=300.0,
        )
        changer.set_config(edge)
        assert changer.config.harmony_steps == (-7, 0, 7)


def test_grain_engine_refuses_pitch_correction_harmony_and_vocoder() -> None:
    grain = dataclasses.replace(_neutral(), voice_engine="grain")
    for change in (
        {"pitch_correction_mode": "auto"},
        {"pitch_correction_mode": "robot"},
        {"harmony_voices": 1},
        {"vocoder_enabled": True},
    ):
        refused = dataclasses.replace(grain, **change)
        # Through the creation document the refusal is a content defect of that document.
        with pytest.raises(SonareError) as from_document:
            _changer(refused).close()
        assert not isinstance(from_document.value, SonareValueError)
        assert from_document.value.code == int(ErrorCode.INVALID_FORMAT)
        # Through the POD it is a refused argument.
        with _changer(grain) as changer, pytest.raises(SonareValueError):
            changer.set_config(refused)


def test_construction_time_fields_are_fixed_by_a_record_set_config() -> None:
    base = _neutral()
    with _changer(base) as changer:
        for change in (
            {"voice_engine": "grain"},
            {"voice_engine_f0_min_hz": 80.0},
            {"vocoder_bands": 16},
        ):
            with pytest.raises(SonareValueError):
                changer.set_config(dataclasses.replace(base, **change))


def test_tone_through_hard_tune_style_correction_stays_finite() -> None:
    cfg = dataclasses.replace(
        _neutral(),
        pitch_correction_mode="auto",
        pitch_correction_scale="major",
        pitch_correction_speed_ms=0.0,
    )
    with _changer(cfg) as changer:
        out = changer.process_mono(_tone(9600))
        assert out.shape == (9600,)
        assert np.isfinite(out).all()


def test_note_on_off_hold_notes_on_a_psola_handle() -> None:
    with libsonare.RealtimeVoiceChanger(
        SAMPLE_RATE, max_block_size=128, num_channels=1, config="hardTune"
    ) as changer:
        changer.note_on(60)
        changer.note_on(64, velocity=0.5)
        changer.note_on(64)
        changer.note_off(60)
        changer.note_off(100)
        changer.all_notes_off()
        assert np.isfinite(changer.process_mono(_tone())).all()


@pytest.mark.parametrize("note", [0, 127])
def test_note_range_edges_are_accepted(note: int) -> None:
    with libsonare.RealtimeVoiceChanger(SAMPLE_RATE, num_channels=1, config="hardTune") as changer:
        changer.note_on(note)
        changer.note_off(note)


@pytest.mark.parametrize("note", [-1, 128, 2**31, 2**32])
def test_note_outside_the_midi_range_is_refused(note: int) -> None:
    with libsonare.RealtimeVoiceChanger(SAMPLE_RATE, num_channels=1, config="hardTune") as changer:
        with pytest.raises(SonareValueError):
            changer.note_on(note)
        with pytest.raises(SonareValueError):
            changer.note_off(note)


def test_note_refuses_a_fraction_and_a_bool() -> None:
    with libsonare.RealtimeVoiceChanger(SAMPLE_RATE, num_channels=1, config="hardTune") as changer:
        with pytest.raises(SonareValueError):
            changer.note_on(60.5)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            changer.note_on(True)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            changer.note_off(False)  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            changer.note_on(60, velocity=True)  # type: ignore[arg-type]


@pytest.mark.parametrize("velocity", [0.0, -0.5, 1.5, float("nan"), float("inf")])
def test_velocity_outside_zero_exclusive_to_one_is_refused(velocity: float) -> None:
    with (
        libsonare.RealtimeVoiceChanger(SAMPLE_RATE, num_channels=1, config="hardTune") as changer,
        pytest.raises(SonareValueError),
    ):
        changer.note_on(60, velocity)


def test_velocity_defaults_to_one_and_accepts_the_upper_edge() -> None:
    with libsonare.RealtimeVoiceChanger(SAMPLE_RATE, num_channels=1, config="hardTune") as changer:
        changer.note_on(60)
        changer.note_on(61, velocity=1.0)
        changer.note_on(62, velocity=1e-3)


def test_note_api_on_a_grain_handle_is_invalid_state() -> None:
    grain = dataclasses.replace(_neutral(), voice_engine="grain")
    with _changer(grain) as changer:
        with pytest.raises(SonareError) as raised:
            changer.note_on(60)
        assert not isinstance(raised.value, SonareValueError)
        assert raised.value.code == int(ErrorCode.INVALID_STATE)


def test_note_api_on_a_closed_handle_is_invalid_state() -> None:
    changer = libsonare.RealtimeVoiceChanger(SAMPLE_RATE, num_channels=1, config="hardTune")
    changer.close()
    for call in (lambda: changer.note_on(60), lambda: changer.note_off(60), changer.all_notes_off):
        with pytest.raises(SonareError) as raised:
            call()
        assert raised.value.code == int(ErrorCode.INVALID_STATE)


def _vocoder(source: str) -> libsonare.RealtimeVoiceChangerConfig:
    return dataclasses.replace(
        _neutral(), vocoder_enabled=True, vocoder_carrier_source=source, vocoder_level=1.0
    )


def test_external_carrier_is_required_by_the_carrierless_entries() -> None:
    with _changer(_vocoder("external"), num_channels=2) as changer:
        samples = _tone()
        for call in (
            lambda: changer.process_mono(samples),
            lambda: changer.process_interleaved(np.tile(samples, 2)),
            lambda: changer.process_planar([samples, samples]),
        ):
            with pytest.raises(SonareError) as raised:
                call()
            assert not isinstance(raised.value, SonareValueError)
            assert raised.value.code == int(ErrorCode.INVALID_STATE)


def test_process_mono_with_a_carrier_keeps_the_output_shape_and_validates_its_length() -> None:
    samples = _tone()
    with _changer(_vocoder("external")) as changer:
        out = changer.process_mono(samples, carrier=_tone(hz=110.0))
        assert out.shape == samples.shape and out.dtype == np.float32
        assert np.isfinite(out).all()
        with pytest.raises(SonareValueError, match="carrier"):
            changer.process_mono(samples, carrier=samples[:-1])
        with pytest.raises(SonareValueError, match="carrier"):
            changer.process_mono(samples, carrier=np.concatenate([samples, samples[:1]]))


def test_carrier_entries_render_with_an_external_carrier() -> None:
    samples = _tone()
    carrier = _tone(hz=110.0)
    with _changer(_vocoder("external"), num_channels=2) as changer:
        mono = changer.process_mono(samples, carrier=carrier)
        assert mono.shape == samples.shape and mono.dtype == np.float32
        assert np.isfinite(mono).all()
        changer.reset()
        interleaved = changer.process_interleaved(np.tile(samples, 2), carrier=carrier)
        assert interleaved.shape == (2 * samples.shape[0],)
        assert np.isfinite(interleaved).all()
        changer.reset()
        left, right = changer.process_planar([samples, samples], carrier=carrier)
        assert left.shape == right.shape == samples.shape
        assert np.isfinite(left).all() and np.isfinite(right).all()


def test_carrier_is_ignored_by_an_internal_carrier_config() -> None:
    samples = _tone()
    with _changer(_vocoder("internal")) as with_carrier, _changer(_vocoder("internal")) as plain:
        a = with_carrier.process_mono(samples, carrier=_tone(hz=300.0))
        b = plain.process_mono(samples)
        np.testing.assert_array_equal(a, b)


def test_carrier_length_must_match_the_input() -> None:
    samples = _tone()
    with _changer(_vocoder("external"), num_channels=2) as changer:
        with pytest.raises(SonareValueError, match="carrier"):
            changer.process_mono(samples, carrier=samples[:-1])
        with pytest.raises(SonareValueError, match="carrier"):
            changer.process_interleaved(np.tile(samples, 2), carrier=samples[:-1])
        with pytest.raises(SonareValueError, match="carrier"):
            changer.process_planar([samples, samples], carrier=samples[:-1])


def test_realtime_voice_changer_render_renders_a_new_preset() -> None:
    out = libsonare.realtime_voice_changer_render(_tone(9600), SAMPLE_RATE, config="choirHarmony")
    assert out.shape == (9600,)
    assert np.isfinite(out).all()


def test_one_planar_channel_takes_the_mono_path() -> None:
    samples = _tone()
    with (
        _changer(_neutral(), num_channels=1) as planar,
        _changer(_neutral(), num_channels=1) as mono,
    ):
        (out,) = planar.process_planar([samples])
        np.testing.assert_array_equal(out, mono.process_mono(samples))


def test_two_planar_channels_leave_the_inputs_untouched() -> None:
    left, right = _tone(), _tone(hz=330.0)
    before = (left.copy(), right.copy())
    with _changer(_neutral(), num_channels=2) as changer:
        out = changer.process_planar(np.stack([left, right]))
    assert len(out) == 2 and out[0].shape == left.shape
    np.testing.assert_array_equal(left, before[0])
    np.testing.assert_array_equal(right, before[1])


def test_planar_channel_count_and_lengths_are_refused() -> None:
    samples = _tone()
    with _changer(_neutral(), num_channels=2) as changer:
        with pytest.raises(SonareValueError, match="1 or 2 planes"):
            changer.process_planar([samples, samples, samples])
        with pytest.raises(SonareValueError, match="same length"):
            changer.process_planar([samples, samples[:-1]])
        with pytest.raises(SonareValueError, match="1 or 2 planes"):
            changer.process_planar([])
