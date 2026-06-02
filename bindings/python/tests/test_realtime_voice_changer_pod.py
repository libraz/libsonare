"""Tests for the flat POD-config path of RealtimeVoiceChanger.

These tests exercise the ctypes-direct path that skips the JSON round-trip:
``realtime_voice_changer_preset_pod`` + ``RealtimeVoiceChanger.config_pod`` /
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
from libsonare._ffi import SonareRealtimeVoiceChangerConfig

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


def test_pod_struct_size_matches_c_abi() -> None:
    """sonare_c.h pins sizeof == 36 * sizeof(float) (ABI v2)."""
    assert ctypes.sizeof(SonareRealtimeVoiceChangerConfig) == 36 * ctypes.sizeof(ctypes.c_float)


def test_pod_dataclass_has_one_field_per_pod_field() -> None:
    pod_field_names = {name for name, _ in SonareRealtimeVoiceChangerConfig._fields_}
    dc_field_names = {f.name for f in dataclasses.fields(libsonare.RealtimeVoiceChangerConfig)}
    assert pod_field_names == dc_field_names


def test_preset_pod_returns_dataclass_for_known_name() -> None:
    cfg = libsonare.realtime_voice_changer_preset_pod("neutral-monitor")
    assert isinstance(cfg, libsonare.RealtimeVoiceChangerConfig)
    # Neutral monitor is a no-op-ish preset: wet_mix should be 1.0 and the
    # formant factor should be 1.0 (no pitch shift).
    assert cfg.wet_mix == pytest.approx(1.0, abs=1e-6)
    assert cfg.formant_factor == pytest.approx(1.0, abs=1e-6)


def test_preset_pod_accepts_int_ordinal() -> None:
    by_name = libsonare.realtime_voice_changer_preset_pod("bright-idol")
    by_ord = libsonare.realtime_voice_changer_preset_pod(
        libsonare._ffi.SONARE_VC_PRESET_BRIGHT_IDOL
    )
    # All fields should match between the two access paths.
    for f in dataclasses.fields(libsonare.RealtimeVoiceChangerConfig):
        assert getattr(by_name, f.name) == getattr(by_ord, f.name)


def test_preset_pod_distinguishes_presets() -> None:
    neutral = libsonare.realtime_voice_changer_preset_pod("neutral-monitor")
    bright = libsonare.realtime_voice_changer_preset_pod("bright-idol")
    # The "bright" preset cannot be identical to the neutral one — at least one
    # field must differ.
    assert dataclasses.astuple(neutral) != dataclasses.astuple(bright)


def test_preset_pod_rejects_unknown_name() -> None:
    with pytest.raises(ValueError, match="unknown voice character preset"):
        libsonare.realtime_voice_changer_preset_pod("does-not-exist")


def _build_changer() -> libsonare.RealtimeVoiceChanger:
    return libsonare.RealtimeVoiceChanger(
        sample_rate=48000, preset="neutral-monitor", max_block_size=128
    )


def test_config_pod_returns_live_normalized_config() -> None:
    changer = _build_changer()
    try:
        cfg = changer.config_pod()
        assert isinstance(cfg, libsonare.RealtimeVoiceChangerConfig)
        # The handle was created from the neutral preset, so its live config
        # must agree with realtime_voice_changer_preset_pod("neutral-monitor").
        preset = libsonare.realtime_voice_changer_preset_pod("neutral-monitor")
        for f in dataclasses.fields(libsonare.RealtimeVoiceChangerConfig):
            assert getattr(cfg, f.name) == pytest.approx(getattr(preset, f.name), abs=1e-6), (
                f"live config drift on {f.name}"
            )
    finally:
        changer.close()


def test_set_config_pod_round_trips_through_get_config() -> None:
    changer = _build_changer()
    try:
        cfg = changer.config_pod()
        cfg.input_gain_db = -3.5
        cfg.output_gain_db = 1.25
        cfg.wet_mix = 0.7
        cfg.compressor_ratio = 4.0
        cfg.limiter_release_ms = 80.0
        cfg.limiter_enable_isp_limiter = 0
        cfg.limiter_isp_ceiling_dbtp = -2.5
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
        cfg = libsonare.realtime_voice_changer_preset_pod("bright-idol")
        changer.set_config_pod(cfg)
        out = changer.process_mono(samples)
        assert out.shape == samples.shape
        assert out.dtype == np.float32
        assert np.isfinite(out).all()
    finally:
        changer.close()


def test_to_pod_and_from_pod_are_inverses() -> None:
    cfg = libsonare.realtime_voice_changer_preset_pod("dark-villain")
    cycled = libsonare.RealtimeVoiceChangerConfig.from_pod(cfg.to_pod())
    assert dataclasses.astuple(cycled) == dataclasses.astuple(cfg)
