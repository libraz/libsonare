"""Tests for the flat POD-config path of RealtimeVoiceChanger.

These tests exercise the ctypes-direct path that skips the JSON round-trip:
``realtime_voice_changer_preset_config`` + ``RealtimeVoiceChanger.config_pod`` /
``set_config_pod``. The slow JSON path is covered separately by
``test_editing.py``.
"""

from __future__ import annotations

import ctypes
import dataclasses
import math

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
    ``RealtimeVoiceChangerConfig()`` -> ``set_config_pod`` produced a
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


def test_config_pod_returns_live_normalized_config() -> None:
    changer = _build_changer()
    try:
        cfg = changer.config_pod()
        assert isinstance(cfg, libsonare.RealtimeVoiceChangerConfig)
        # The handle was created from the neutral preset, so its live config
        # must agree with realtime_voice_changer_preset_config("neutralMonitor")
        # on every field except the retune grain size. Grain size is structural
        # and resolved at prepare() time, so config() reports the effective grain
        # rather than the requested value (0 = "library default") in the preset.
        preset = libsonare.realtime_voice_changer_preset_config("neutralMonitor")
        for f in dataclasses.fields(libsonare.RealtimeVoiceChangerConfig):
            if f.name == "retune_grain_size_samples":
                continue
            assert getattr(cfg, f.name) == pytest.approx(getattr(preset, f.name), abs=1e-6), (
                f"live config drift on {f.name}"
            )
        # The preset requests the library default (0); the live config reports
        # the resolved, non-zero grain that fixes latency for the session.
        assert preset.retune_grain_size_samples == 0
        assert cfg.retune_grain_size_samples > 0
    finally:
        changer.close()


def test_set_config_pod_round_trips_through_get_config() -> None:
    changer = _build_changer()
    try:
        cfg = changer.config_pod()
        cfg = dataclasses.replace(
            cfg,
            input_gain_db=-3.5,
            output_gain_db=1.25,
            wet_mix=0.7,
            compressor_ratio=4.0,
            limiter_release_ms=80.0,
            limiter_enable_isp_limiter=0,
            limiter_isp_ceiling_dbtp=-2.5,
        )
        changer.set_config_pod(cfg)
        round_tripped = changer.config_pod()
        assert round_tripped.input_gain_db == pytest.approx(-3.5, abs=1e-6)
        assert round_tripped.output_gain_db == pytest.approx(1.25, abs=1e-6)
        assert round_tripped.wet_mix == pytest.approx(0.7, abs=1e-6)
        assert round_tripped.compressor_ratio == pytest.approx(4.0, abs=1e-6)
        assert round_tripped.limiter_release_ms == pytest.approx(80.0, abs=1e-6)
        assert round_tripped.limiter_enable_isp_limiter == 0
        assert round_tripped.limiter_isp_ceiling_dbtp == pytest.approx(-2.5, abs=1e-6)
    finally:
        changer.close()


def test_set_config_pod_does_not_break_processing() -> None:
    """After a POD config swap, the handle must still produce finite audio."""
    sr = 48000
    samples = 0.2 * np.sin(2 * math.pi * 220.0 * np.arange(sr // 4) / sr).astype(np.float32)
    changer = _build_changer()
    try:
        cfg = libsonare.realtime_voice_changer_preset_config("brightIdol")
        changer.set_config_pod(cfg)
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
        assert warped.config_pod().formant_mode == "absolute"
        warped.set_config_pod(dataclasses.replace(absolute, formant_factor=1.2))
        with pytest.raises(ValueError, match="formant mode"):
            warped.set_config_pod(base)
        with pytest.raises(ValueError, match="formant mode"):
            relative.set_config_pod(absolute)


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
        before = changer.config_pod()
        factor_range = r"formant\.factor out of range \[0\.55, 1\.65\]"
        with pytest.raises(SonareValueError, match=factor_range):
            changer.set_config_pod(dataclasses.replace(base, formant_factor=3.0))
        with pytest.raises(SonareValueError, match=r"limiter\.ceilingDb out of range"):
            changer.set_config_pod(dataclasses.replace(base, limiter_ceiling_db=0.5))
        assert changer.config_pod() == before
    with pytest.raises(SonareError, match="out of range") as created:
        libsonare.RealtimeVoiceChanger(
            48000, num_channels=1, config=_flat(dataclasses.replace(base, formant_factor=3.0))
        )
    assert not isinstance(created.value, SonareValueError)
    assert created.value.code == int(ErrorCode.INVALID_FORMAT)
