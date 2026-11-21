"""Voice-changer, retune, room and repair names match the other surfaces.

Preset ids and closed-set tokens are lowerCamel, quantities carry their unit
(``pitch_semitones``, ``grain_size_samples``, ``max_sec``, ``*_db`` in dBFS),
and each retired spelling is refused rather than silently accepted.
"""

from __future__ import annotations

import dataclasses
import json
import math

import numpy as np
import pytest

import libsonare
from libsonare import RealtimeVoiceChangerConfig, SonareValueError, StreamingRetune

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000

LOWER_CAMEL_PRESETS = (
    "neutralMonitor",
    "brightIdol",
    "softWhisper",
    "deepNarrator",
    "robotMascot",
    "darkVillain",
    "hardTune",
    "robotMonotone",
    "choirHarmony",
    "vocoderStrings",
    "radioVoice",
    "megaphone",
)


def test_every_preset_id_is_lower_camel() -> None:
    assert set(libsonare.realtime_voice_changer_preset_names()) == set(LOWER_CAMEL_PRESETS)
    for name in LOWER_CAMEL_PRESETS:
        assert isinstance(
            libsonare.realtime_voice_changer_preset_config(name), RealtimeVoiceChangerConfig
        )


@pytest.mark.parametrize("old", ["neutral-monitor", "bright-idol", "hard-tune", "radio-voice"])
def test_a_kebab_preset_id_is_refused(old: str) -> None:
    with pytest.raises(SonareValueError):
        libsonare.realtime_voice_changer_preset_config(old)
    with pytest.raises(libsonare.SonareError):
        libsonare.RealtimeVoiceChanger(SR, num_channels=1, config=old)


def test_the_config_fields_carry_their_unit_and_no_doubled_word() -> None:
    names = {field.name for field in dataclasses.fields(RealtimeVoiceChangerConfig)}
    for new in (
        "retune_pitch_semitones",
        "retune_grain_size_samples",
        "eq_high_pass_hz",
        "voice_engine",
        "reverb_variation",
    ):
        assert new in names
    for old in (
        "retune_semitones",
        "retune_grain_size",
        "eq_highpass_hz",
        "voice_engine_engine",
        "reverb_variation_variation",
    ):
        assert old not in names


@pytest.mark.parametrize(
    ("field", "new", "old"),
    [
        ("pitch_correction_scale", "naturalMinor", "natural_minor"),
        ("character_type", "bbdChorus", "bbd_chorus"),
        ("reverb_variation", "tempoDelay", "tempo_delay"),
        ("reverb_variation", "dubEcho", "dub_echo"),
        ("reverb_variation_division", "dottedEighth", "dotted_eighth"),
    ],
)
def test_config_tokens_are_lower_camel(field: str, new: str, old: str) -> None:
    base = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    with libsonare.RealtimeVoiceChanger(SR, num_channels=1) as changer:
        changer.set_config(dataclasses.replace(base, **{field: new}))
        assert getattr(changer.config, field) == new
        with pytest.raises(SonareValueError, match="expected one of|must be one of"):
            changer.set_config(dataclasses.replace(base, **{field: old}))


def _flat(config: RealtimeVoiceChangerConfig) -> dict[str, object]:
    out: dict[str, object] = {}
    for f in dataclasses.fields(config):
        head, *rest = f.name.split("_")
        value = getattr(config, f.name)
        if value is None:  # an automatic grain is spelled by omission
            continue
        if isinstance(value, tuple):
            value = list(value)
        elif f.name in ("limiter_enable_isp_limiter", "vocoder_enabled"):
            value = bool(value)
        out[head + "".join(part.capitalize() for part in rest)] = value
    return out


def test_the_flat_config_takes_harmony_steps_as_one_array() -> None:
    base = libsonare.realtime_voice_changer_preset_config("choirHarmony")
    flat = _flat(dataclasses.replace(base, harmony_steps=(2, 5, -3)))
    assert "harmonySteps" in flat
    with libsonare.RealtimeVoiceChanger(SR, num_channels=1, config=flat) as changer:
        assert json.loads(changer.config_json())["dsp"]["harmony"]["steps"] == [2, 5, -3]
    split = {k: v for k, v in flat.items() if k != "harmonySteps"}
    split.update(harmonyStep1=2, harmonyStep2=5, harmonyStep3=-3)
    with pytest.raises(libsonare.SonareError, match="harmonyStep1"):
        libsonare.RealtimeVoiceChanger(SR, num_channels=1, config=split)


def test_streaming_retune_names_its_quantities_with_units() -> None:
    with StreamingRetune(pitch_semitones=3.0, grain_size_samples=512) as retune:
        assert retune.config() == {"pitch_semitones": 3.0, "mix": 1.0, "grain_size_samples": 512}
        assert not hasattr(retune, "grain_size")
    with pytest.raises(TypeError):
        StreamingRetune(semitones=3.0)  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        StreamingRetune(grain_size=512)  # type: ignore[call-arg]


def test_the_rir_length_cap_is_max_sec() -> None:
    capped = libsonare.synthesize_rir(sample_rate=SR, max_sec=0.05)
    assert len(capped.rir) <= int(0.05 * SR) + 1
    with pytest.raises(TypeError):
        libsonare.synthesize_rir(sample_rate=SR, max_seconds=0.05)  # type: ignore[call-arg]


@pytest.mark.parametrize(
    "call",
    [
        lambda **kw: libsonare.synthesize_rir(sample_rate=SR, **kw),
        lambda **kw: libsonare.room_morph([0.0] * 100, SR, 7.0, 5.0, 3.0, **kw),
        lambda **kw: libsonare.room_morph_stereo([0.0] * 100, [0.0] * 100, SR, 7.0, 5.0, 3.0, **kw),
    ],
)
@pytest.mark.parametrize("name", ["max_sec", "mixing_time_ms"])
def test_an_automatic_length_or_mixing_time_is_absence_not_zero(call, name: str) -> None:
    call(**{name: None})
    with pytest.raises(SonareValueError, match=name):
        call(**{name: 0.0})


def test_an_absent_cap_is_the_natural_length() -> None:
    natural = libsonare.synthesize_rir(sample_rate=SR, seed=1)
    omitted = libsonare.synthesize_rir(sample_rate=SR, seed=1, max_sec=None)
    np.testing.assert_array_equal(natural.rir, omitted.rir)
    assert len(libsonare.synthesize_rir(sample_rate=SR, seed=1, max_sec=0.05).rir) < len(
        natural.rir
    )


def test_the_room_estimate_mode_is_impulse_response_in_lower_camel() -> None:
    rir = libsonare.synthesize_rir(sample_rate=SR, max_sec=0.5)
    libsonare.estimate_room(rir.rir, sample_rate=SR, mode="impulseResponse", min_decay_db=25.0)
    with pytest.raises(SonareValueError, match="impulseResponse"):
        libsonare.estimate_room(rir.rir, sample_rate=SR, mode="impulse_response")


def _clipped() -> np.ndarray:
    t = np.arange(SR // 4) / SR
    return np.clip(np.sin(2 * math.pi * 220.0 * t), -0.6, 0.6).astype(np.float32)


def test_the_clip_threshold_is_in_dbfs() -> None:
    samples = _clipped()
    below = libsonare.mastering_repair_detect_clipping(
        samples, SR, clip_threshold_db=20.0 * math.log10(0.5)
    )
    above = libsonare.mastering_repair_detect_clipping(
        samples, SR, clip_threshold_db=20.0 * math.log10(0.9)
    )
    assert below.clipped_samples > above.clipped_samples
    with pytest.raises(TypeError):
        libsonare.mastering_repair_detect_clipping(samples, SR, clip_threshold=0.5)  # type: ignore[call-arg]


def test_the_trim_threshold_is_in_dbfs() -> None:
    with pytest.raises(TypeError):
        libsonare.mastering_repair_trim_silence(_clipped(), SR, threshold=0.001)  # type: ignore[call-arg]


@pytest.mark.parametrize("old", ["equal_power", "equal-power", "s_curve", "exp"])
def test_a_fade_curve_outside_the_four_tokens_is_refused(old: str) -> None:
    project = libsonare.Project()
    track = project.add_track("audio", "gtr")
    clip = project.add_clip(
        track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=SR
    )
    for token in ("linear", "equalPower", "exponential", "logarithmic"):
        project.set_clip_fade(clip, 4.0, 4.0, fade_in_curve=token, fade_out_curve=token)
    with pytest.raises(libsonare.SonareError):
        project.set_clip_fade(clip, 4.0, 4.0, fade_in_curve=old)


def _preset_document() -> dict[str, object]:
    document: dict[str, object] = json.loads(
        libsonare.realtime_voice_changer_preset_json("neutralMonitor")
    )
    return document


def test_a_preset_document_is_read_at_schema_version_4_only() -> None:
    document = _preset_document()
    assert document["schemaVersion"] == 4
    libsonare.voice_change_realtime(np.zeros(4800, np.float32), SR, config=document)
    with pytest.raises(libsonare.SonareError, match=r"schemaVersion must be 4") as refused:
        libsonare.voice_change_realtime(
            np.zeros(4800, np.float32), SR, config={**document, "schemaVersion": 2}
        )
    assert refused.value.code_name == "InvalidFormat"


def test_a_preset_document_names_the_grain_in_samples() -> None:
    document = _preset_document()
    dsp = document["dsp"]
    assert isinstance(dsp, dict)
    retune = dsp["retune"]
    old = {**document, "dsp": {**dsp, "retune": {**retune, "grainSize": 64}}}
    with pytest.raises(libsonare.SonareError, match=r"unknown key dsp\.retune\.grainSize$"):
        libsonare.voice_change_realtime(np.zeros(4800, np.float32), SR, config=old)
    zero = {**document, "dsp": {**dsp, "retune": {**retune, "grainSizeSamples": 0}}}
    with pytest.raises(libsonare.SonareError, match=r"dsp\.retune\.grainSizeSamples must not be 0"):
        libsonare.voice_change_realtime(np.zeros(4800, np.float32), SR, config=zero)
    given = {**document, "dsp": {**dsp, "retune": {**retune, "grainSizeSamples": 64}}}
    libsonare.voice_change_realtime(np.zeros(4800, np.float32), SR, config=given)
