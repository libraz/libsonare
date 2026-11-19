"""Tests for the flat POD-config path of RealtimeVoiceChanger.

These tests exercise the ctypes-direct path that skips the JSON round-trip:
``realtime_voice_changer_preset_config`` + ``RealtimeVoiceChanger.config`` /
``set_config`` with a record. The slow JSON path is covered separately by
``test_editing.py``.
"""

from __future__ import annotations

import ctypes
import dataclasses
import json
import math
import re
from pathlib import Path
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import ErrorCode, SonareError, SonareValueError
from libsonare._ffi import SonareRealtimeVoiceChangerConfig

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


def test_pod_struct_size_matches_c_abi() -> None:
    """sonare_c.h pins sizeof == 72 * sizeof(float) (struct_version 2)."""
    assert ctypes.sizeof(SonareRealtimeVoiceChangerConfig) == 72 * ctypes.sizeof(ctypes.c_float)


def test_pod_dataclass_has_one_field_per_pod_field() -> None:
    # struct_version is the layout tag, not a setting.
    pod_field_names = {
        name for name, _ in SonareRealtimeVoiceChangerConfig._fields_ if name != "struct_version"
    }
    dc_field_names = {f.name for f in dataclasses.fields(libsonare.RealtimeVoiceChangerConfig)}
    assert pod_field_names == dc_field_names


def test_preset_config_returns_dataclass_for_known_name() -> None:
    cfg = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    assert isinstance(cfg, libsonare.RealtimeVoiceChangerConfig)
    # Neutral monitor is a no-op-ish preset: wet_mix should be 1.0 and the
    # formant factor should be 1.0 (no pitch shift).
    assert cfg.wet_mix == pytest.approx(1.0, abs=1e-6)
    assert cfg.formant_factor == pytest.approx(1.0, abs=1e-6)


def test_default_constructed_config_matches_neutral_monitor_preset() -> None:
    """A default-constructed config is the neutralMonitor preset (single source).

    Regression guard: previously the dataclass carried hand-written literals that
    matched neither the C POD defaults nor any preset, so
    ``RealtimeVoiceChangerConfig()`` -> ``set_config`` produced a
    bypassed-gate / flat-EQ voice instead of a usable neutral one.
    """
    default = libsonare.RealtimeVoiceChangerConfig()
    preset = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    assert default == preset


def test_preset_config_distinguishes_presets() -> None:
    neutral = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    bright = libsonare.realtime_voice_changer_preset_config("brightIdol")
    # The "bright" preset cannot be identical to the neutral one -- at least one
    # field must differ.
    assert dataclasses.astuple(neutral) != dataclasses.astuple(bright)


def test_preset_config_rejects_an_unknown_name_listing_the_ids() -> None:
    with pytest.raises(SonareValueError, match="invalid voice character preset") as caught:
        libsonare.realtime_voice_changer_preset_config("does-not-exist")  # type: ignore[arg-type]
    assert "'radioVoice'" in str(caught.value)


@pytest.mark.parametrize("value", [0, 1, 2**32, True, False, 1.0])
def test_preset_config_refuses_a_number_rather_than_reading_it_as_an_ordinal(value) -> None:
    """A preset is named by its id; an ordinal (or a bool posing as one) is a TypeError.

    The positive control: the ids these ordinals would have named are three
    distinct live presets, so a resolver that read the number would return one
    of them, indistinguishable from having asked for it.
    """
    ids = ("neutralMonitor", "brightIdol", "softWhisper")
    folded = [dataclasses.astuple(libsonare.realtime_voice_changer_preset_config(i)) for i in ids]
    assert len(set(folded)) == 3
    with pytest.raises(TypeError, match="voice character preset"):
        libsonare.realtime_voice_changer_preset_config(value)


def _build_changer() -> libsonare.RealtimeVoiceChanger:
    return libsonare.RealtimeVoiceChanger(
        sample_rate=48000, max_block_size=128, num_channels=1, config="neutralMonitor"
    )


def test_config_returns_the_live_normalized_record() -> None:
    changer = _build_changer()
    try:
        cfg = changer.config
        assert isinstance(cfg, libsonare.RealtimeVoiceChangerConfig)
        # The handle was created from the neutral preset, so its live config
        # agrees with realtime_voice_changer_preset_config("neutralMonitor") on
        # every field, the retune grain included: config reads the grain as
        # configured, and the preset leaves it automatic.
        preset = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
        for f in dataclasses.fields(libsonare.RealtimeVoiceChangerConfig):
            assert getattr(cfg, f.name) == pytest.approx(getattr(preset, f.name), abs=1e-6), (
                f"live config drift on {f.name}"
            )
        assert preset.retune_grain_size_samples is None
        assert libsonare.RealtimeVoiceChangerConfig().retune_grain_size_samples is None
        with pytest.raises(
            libsonare.SonareValueError,
            match="^RealtimeVoiceChanger.set_config: retune_grain_size_samples must not be 0; "
            "omit it for the automatic grain$",
        ):
            changer.set_config(dataclasses.replace(cfg, retune_grain_size_samples=0))
    finally:
        changer.close()


def test_set_config_with_a_record_round_trips_through_config() -> None:
    changer = _build_changer()
    try:
        cfg = changer.config
        cfg = dataclasses.replace(
            cfg,
            input_gain_db=-3.5,
            output_gain_db=1.25,
            wet_mix=0.7,
            compressor_ratio=4.0,
            limiter_release_ms=80.0,
            limiter_enable_isp_limiter=False,
            limiter_isp_ceiling_dbtp=-2.5,
        )
        changer.set_config(cfg)
        round_tripped = changer.config
        assert round_tripped.input_gain_db == pytest.approx(-3.5, abs=1e-6)
        assert round_tripped.output_gain_db == pytest.approx(1.25, abs=1e-6)
        assert round_tripped.wet_mix == pytest.approx(0.7, abs=1e-6)
        assert round_tripped.compressor_ratio == pytest.approx(4.0, abs=1e-6)
        assert round_tripped.limiter_release_ms == pytest.approx(80.0, abs=1e-6)
        assert round_tripped.limiter_enable_isp_limiter is False
        assert round_tripped.limiter_isp_ceiling_dbtp == pytest.approx(-2.5, abs=1e-6)
    finally:
        changer.close()


@pytest.mark.parametrize("field", ["limiter_enable_isp_limiter", "vocoder_enabled"])
def test_on_off_fields_are_bools_and_an_int_is_refused(field: str) -> None:
    cfg = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    assert isinstance(getattr(cfg, field), bool)
    flipped = dataclasses.replace(cfg, **{field: not getattr(cfg, field)})
    assert getattr(libsonare.RealtimeVoiceChangerConfig._from_pod(flipped._to_pod()), field) is (
        not getattr(cfg, field)
    )
    for wrong in (1, 0, 1.0):
        with pytest.raises(TypeError, match=f"^{field} must be a bool$"):
            dataclasses.replace(cfg, **{field: wrong})._to_pod()


def test_set_config_with_a_record_does_not_break_processing() -> None:
    """After a POD config swap, the handle must still produce finite audio."""
    sr = 48000
    samples = 0.2 * np.sin(2 * math.pi * 220.0 * np.arange(sr // 4) / sr).astype(np.float32)
    changer = _build_changer()
    try:
        cfg = libsonare.realtime_voice_changer_preset_config("brightIdol")
        changer.set_config(cfg)
        out = changer.process_mono(samples)
        assert out.shape == samples.shape
        assert out.dtype == np.float32
        assert np.isfinite(out).all()
    finally:
        changer.close()


def test_to_pod_and_from_pod_are_inverses() -> None:
    cfg = libsonare.realtime_voice_changer_preset_config("darkVillain")
    cycled = libsonare.RealtimeVoiceChangerConfig._from_pod(cfg._to_pod())
    assert dataclasses.astuple(cycled) == dataclasses.astuple(cfg)


def _flat(config: libsonare.RealtimeVoiceChangerConfig) -> dict[str, object]:
    """The camelCase flat document the JSON entry points accept for a POD."""
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


def test_formant_mode_is_a_string_field_defaulting_to_relative() -> None:
    cfg = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    assert cfg.formant_mode == "relative"
    pod = dataclasses.replace(cfg, formant_mode="absolute")._to_pod()
    assert pod.struct_version == 2
    assert pod.formant_mode == 1
    assert libsonare.RealtimeVoiceChangerConfig._from_pod(pod).formant_mode == "absolute"
    with pytest.raises(ValueError, match="formant_mode"):
        dataclasses.replace(cfg, formant_mode="sideways")._to_pod()


def test_formant_mode_is_fixed_when_the_changer_is_created() -> None:
    # The grain engine is selected explicitly: it is the one whose absolute mode adds
    # exactly one analysis frame of latency.
    base = dataclasses.replace(
        libsonare.realtime_voice_changer_preset_config("neutralMonitor"),
        voice_engine="grain",
    )
    absolute = dataclasses.replace(base, formant_mode="absolute", formant_factor=1.1)
    with (
        libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=_flat(base)) as relative,
        libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=_flat(absolute)) as warped,
    ):
        # One analysis frame (1024 samples at 48 kHz) more than the relative chain.
        assert warped.latency_samples == relative.latency_samples + 1024
        assert warped.config.formant_mode == "absolute"
        warped.set_config(dataclasses.replace(absolute, formant_factor=1.2))
        with pytest.raises(ValueError, match="formant mode"):
            warped.set_config(base)
        with pytest.raises(ValueError, match="formant mode"):
            relative.set_config(absolute)


def test_unreachable_absolute_warp_is_refused_with_the_reachable_range() -> None:
    base = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    unreachable = dataclasses.replace(
        base, voice_engine="grain", formant_mode="absolute", retune_pitch_semitones=-9.0
    )
    # The creation document is a JSON entry point: its content defect is InvalidFormat.
    with pytest.raises(SonareError, match=r"\[0\.55, 0\.9811\]") as refused:
        libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=_flat(unreachable))
    assert not isinstance(refused.value, SonareValueError)
    assert refused.value.code == int(ErrorCode.INVALID_FORMAT)


def test_out_of_range_field_is_refused_and_the_previous_config_kept() -> None:
    base = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    with libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=_flat(base)) as changer:
        before = changer.config
        factor_range = r"formant\.factor must be finite and in \[0\.55, 1\.65\]"
        with pytest.raises(SonareValueError, match=factor_range):
            changer.set_config(dataclasses.replace(base, formant_factor=3.0))
        with pytest.raises(
            SonareValueError, match=r"limiter\.ceilingDb must be finite and in \[-12, -1\]"
        ):
            changer.set_config(dataclasses.replace(base, limiter_ceiling_db=0.5))
        assert changer.config == before
    with pytest.raises(
        SonareError, match=r"formantFactor must be finite and in \[0\.55, 1\.65\]"
    ) as created:
        libsonare.RealtimeVoiceChanger(
            48000, num_channels=1, config=_flat(dataclasses.replace(base, formant_factor=3.0))
        )
    assert not isinstance(created.value, SonareValueError)
    assert created.value.code == int(ErrorCode.INVALID_FORMAT)


def test_set_config_takes_a_record_and_config_is_a_property() -> None:
    """One setter for every config form; the record read is a property."""
    base = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
    with libsonare.RealtimeVoiceChanger(48000, num_channels=1) as changer:
        changer.set_config(dataclasses.replace(base, formant_factor=1.2))
        assert changer.config.formant_factor == pytest.approx(1.2)
        changer.set_config("neutralMonitor")
        assert changer.config.formant_factor == pytest.approx(base.formant_factor)
        assert not hasattr(changer, "set_config_pod")
        assert not hasattr(changer, "config_pod")


def test_a_changer_built_from_a_record_reproduces_the_source() -> None:
    """The constructor takes the same three config kinds set_config does."""
    base = dataclasses.replace(
        libsonare.realtime_voice_changer_preset_config("neutralMonitor"),
        voice_engine="grain",
        formant_mode="absolute",
        formant_factor=1.1,
    )
    with (
        libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=_flat(base)) as source,
        libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=source.config) as copy,
    ):
        assert copy.config_json() == source.config_json()
        assert copy.latency_samples == source.latency_samples


@pytest.mark.parametrize("grain", [1, 3, 6, 1.5, -4, 8196])
def test_set_config_refuses_a_retune_grain_outside_its_domain(grain: float) -> None:
    changer = _build_changer()
    try:
        refused = dataclasses.replace(changer.config, retune_grain_size_samples=grain)
        with pytest.raises(SonareValueError) as caught:
            changer.set_config(refused)
        assert str(caught.value) == (
            "RealtimeVoiceChanger.set_config: retune_grain_size_samples "
            "must be a multiple of 4 in [4, 8192]"
        )
    finally:
        changer.close()


@pytest.mark.parametrize("grain", [1, 3, 2230, -4, 8196])
def test_a_document_refuses_a_retune_grain_outside_its_domain_by_path(grain: int) -> None:
    """The preset and flat documents read the grain against the stage's own domain."""
    preset = json.loads(libsonare.realtime_voice_changer_preset_json("deepNarrator"))
    preset["dsp"]["retune"]["grainSizeSamples"] = grain
    flat = _flat(libsonare.realtime_voice_changer_preset_config("neutralMonitor"))
    flat["retuneGrainSizeSamples"] = grain
    for document, path in (
        (preset, "dsp.retune.grainSizeSamples"),
        (flat, "retuneGrainSizeSamples"),
    ):
        with pytest.raises(SonareError) as refused:
            libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=document)
        assert refused.value.code == int(ErrorCode.INVALID_FORMAT)
        assert str(refused.value).endswith(f"{path} must be a multiple of 4 in [4, 8192]")


@pytest.mark.parametrize("grain", [4, 8192])
def test_set_config_keeps_a_retune_grain_inside_its_domain(grain: int) -> None:
    changer = _build_changer()
    try:
        changer.set_config(dataclasses.replace(changer.config, retune_grain_size_samples=grain))
        assert changer.config.retune_grain_size_samples == grain
    finally:
        changer.close()


# The key list the Node and WASM suites read: the record config returns is one shape everywhere.
_SHARED_KEYS = json.loads(
    (
        Path(__file__).resolve().parents[3]
        / "tests/conformance/realtime_voice_changer_config_keys.json"
    ).read_text()
)


def _snake(key: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", key).lower()


def test_record_fields_are_the_shared_config_keys() -> None:
    # The facade copies the record to and from the struct mirror by its field names.
    struct_fields = {
        name for name, *_ in SonareRealtimeVoiceChangerConfig._fields_ if name != "struct_version"
    }
    shared = _SHARED_KEYS["keys"] + _SHARED_KEYS["optional_keys"]
    assert len(shared) == len(set(shared))
    assert struct_fields == {_snake(key) for key in shared}


@pytest.mark.parametrize("name", libsonare.realtime_voice_changer_preset_names())
def test_a_preset_reads_back_the_optional_keys_as_none(name: Any) -> None:
    changer = libsonare.RealtimeVoiceChanger(44100, num_channels=1, config=name)
    try:
        record = changer.config
        for optional in _SHARED_KEYS["optional_keys"]:
            assert getattr(record, _snake(optional)) is None
        for key in _SHARED_KEYS["keys"]:
            assert getattr(record, _snake(key)) is not None, key
    finally:
        changer.close()


def test_an_explicit_grain_reads_back_as_given() -> None:
    changer = libsonare.RealtimeVoiceChanger(44100, num_channels=1, config="brightIdol")
    try:
        changer.set_config(dataclasses.replace(changer.config, retune_grain_size_samples=1024))
        assert changer.config.retune_grain_size_samples == 1024
    finally:
        changer.close()
