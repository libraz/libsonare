"""Tests for the geometric room-acoustics bindings."""

from __future__ import annotations

import ctypes
import math

import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")


def _acoustic_available() -> bool:
    from libsonare._runtime import (
        SONARE_ERROR_NOT_SUPPORTED,
        SonareRirSynthConfig,
        SonareRirSynthResult,
        _get_lib,
    )

    lib = _get_lib()
    if not hasattr(lib, "sonare_synthesize_rir"):
        return False
    config = SonareRirSynthConfig(
        length_m=7.0,
        width_m=5.0,
        height_m=3.0,
        source_x=1.0,
        source_y=1.0,
        source_z=1.2,
        listener_x=5.0,
        listener_y=4.0,
        listener_z=1.7,
        absorption=0.2,
        max_seconds=0.05,
        ism_order=1,
        seed=1,
    )
    out = SonareRirSynthResult()
    rc = lib.sonare_synthesize_rir(ctypes.byref(config), 48000, ctypes.byref(out))
    if rc == 0:
        lib.sonare_free_rir_synth_result(ctypes.byref(out))
    return rc != SONARE_ERROR_NOT_SUPPORTED


acoustic = pytest.mark.skipif(
    not (LIB_AVAILABLE and _acoustic_available()),
    reason="libsonare built without acoustic-simulation support",
)


@acoustic
def test_synthesize_rir_produces_decaying_response() -> None:
    result = libsonare.synthesize_rir(
        7.0, 5.0, 3.0, source=(1.5, 1.0, 1.2), listener=(5.0, 4.0, 1.7), absorption=0.15
    )
    assert result.has_error is False
    assert result.sample_rate == 48000
    assert len(result.rir) > 0
    assert any(abs(s) > 0.0 for s in result.rir)


@acoustic
def test_synthesize_rir_flags_invalid_geometry() -> None:
    # Source outside the room => geometry validation error => empty RIR.
    result = libsonare.synthesize_rir(7.0, 5.0, 3.0, source=(99.0, 1.0, 1.2))
    assert result.has_error is True
    assert len(result.rir) == 0
    assert "acoustic.source_outside_room" in result.error_message


@acoustic
def test_synthesize_rir_surfaces_non_fatal_diagnostics() -> None:
    # Warnings accompany SUCCESSFUL calls, so gating the diagnostic read on
    # has_error (which this facade used to do) leaves a truncated RIR
    # indistinguishable from a complete one. Node and WASM already exposed them.
    clamped = libsonare.synthesize_rir(12.0, 9.0, 5.0, absorption=0.05, max_seconds=0.3)
    assert clamped.has_error is False
    assert clamped.error_message == ""
    assert "acoustic.rir_length_clamped" in clamped.warning_message
    assert clamped.warningMessage == clamped.warning_message

    # ... and a request that needed no clamping reports nothing, so the field is
    # a real signal rather than always-populated noise.
    clean = libsonare.synthesize_rir(5.0, 4.0, 3.0, absorption=0.3, max_seconds=3.0)
    assert clean.has_error is False
    assert clean.warning_message == ""


@acoustic
def test_estimate_room_round_trips_a_known_shoebox() -> None:
    rir = libsonare.synthesize_rir(
        7.0, 5.0, 3.0, source=(1.5, 1.0, 1.2), listener=(5.0, 4.0, 1.7), absorption=0.15
    )
    est = libsonare.estimate_room(
        rir.rir,
        sample_rate=48000,
        aspect_hint_lw=7.0 / 5.0,
        aspect_hint_lh=7.0 / 3.0,
        reference_absorption=0.15,
        prefer_eyring=True,
    )
    true_volume = 7.0 * 5.0 * 3.0
    assert math.isclose(est.volume, true_volume, rel_tol=0.20)
    assert est.confidence > 0.0
    assert len(est.rt60_bands) >= 4
    assert len(est.absorption_bands) == len(est.rt60_bands)
    assert math.isfinite(est.drr_db)
    # camelCase aliases mirror the other bindings.
    assert est.drrDb == est.drr_db


@acoustic
@pytest.mark.slow  # two full RIR syntheses (~10 s); run via `make test-python-slow`
def test_room_morph_adds_a_target_tail_and_is_deterministic() -> None:
    # A short impulse-like recording morphed toward a live target room.
    samples = [0.0] * 4000
    samples[0] = 1.0
    out_a = libsonare.room_morph(samples, 48000, 12.0, 9.0, 5.0, absorption=0.08, wet=0.7)
    out_b = libsonare.room_morph(samples, 48000, 12.0, 9.0, 5.0, absorption=0.08, wet=0.7)
    assert len(out_a) > len(samples)  # target reverb tail appended
    assert out_a == out_b  # deterministic for a fixed seed
    assert all(math.isfinite(s) for s in out_a)


@acoustic
def test_estimate_room_zero_confidence_for_silence() -> None:
    est = libsonare.estimate_room([0.0] * 48000, sample_rate=48000)
    assert est.confidence == 0.0
    assert est.volume == 0.0


@acoustic
def test_room_estimate_and_morph_reject_invalid_configuration() -> None:
    samples = [1.0] + [0.0] * 1999
    with pytest.raises(libsonare.SonareError):
        libsonare.estimate_room(samples, sample_rate=48000, reference_absorption=float("nan"))
    with pytest.raises(libsonare.SonareError):
        libsonare.estimate_room(samples, sample_rate=48000, aspect_hint_lw=float("inf"))
    with pytest.raises(libsonare.SonareError):
        libsonare.room_morph(samples, 48000, -1.0, 4.0, 3.0)
    with pytest.raises(libsonare.SonareError):
        libsonare.room_morph(samples, 48000, 5.0, 4.0, 3.0, wet=float("nan"))


@acoustic
def test_synthesize_rir_uses_default_room_dimensions() -> None:
    # Room dimensions default to 7 x 5 x 3 to match the Node/WASM/CLI bindings.
    result = libsonare.synthesize_rir()
    assert result.has_error is False
    assert result.sample_rate == 48000
    assert len(result.rir) > 0


@acoustic
def test_synthesize_rir_late_model_is_honored() -> None:
    # A more absorptive room makes Sabine and Eyring diverge; selecting the model
    # via prefer_eyring must change the synthesized tail.
    sabine = libsonare.synthesize_rir(
        7.0, 5.0, 3.0, absorption=0.4, max_seconds=0.3, prefer_eyring=False
    )
    eyring = libsonare.synthesize_rir(
        7.0, 5.0, 3.0, absorption=0.4, max_seconds=0.3, prefer_eyring=True
    )
    assert sabine.rir != eyring.rir


@acoustic
def test_synthesize_rir_honors_per_band_scattering() -> None:
    base = dict(
        length_m=7.0,
        width_m=5.0,
        height_m=3.0,
        absorption_bands=[0.2, 0.22, 0.24, 0.26],
        max_seconds=0.3,
        seed=123,
    )
    mirror = libsonare.synthesize_rir(**base, scattering_bands=[0.0, 0.0, 0.0, 0.0])
    diffuse = libsonare.synthesize_rir(**base, scattering_bands=[0.8, 0.8, 0.8, 0.8])
    assert diffuse.rir != mirror.rir


@acoustic
def test_estimate_room_band_arrays_share_length() -> None:
    rir = libsonare.synthesize_rir(7.0, 5.0, 3.0, absorption=0.15)
    est = libsonare.estimate_room(rir.rir, sample_rate=48000, mode=2, min_decay_db=25.0)
    assert len(est.absorption_bands) == len(est.rt60_bands)


@acoustic
def test_synthesize_rir_routes_air_absorption_through_the_c_abi() -> None:
    # The ISO 9613-1 atmospheric term used to be reachable only through the
    # streaming insert; the offline entry point had no way to ask for it. Each
    # assertion pins one of the three ctypes fields, so dropping any single one
    # fails here instead of hiding behind the other two.
    hall = dict(
        length_m=30.0,
        width_m=24.0,
        height_m=15.0,
        source=(3.0, 3.0, 1.5),
        listener=(10.0, 8.0, 1.7),
        absorption=0.2,
        max_seconds=0.5,
        ism_order=2,
        seed=3,
    )
    off = libsonare.synthesize_rir(**hall)
    iso = libsonare.synthesize_rir(
        **hall,
        air_absorption_enabled=True,
        air_temperature_c=20.0,
        air_humidity_percent=50.0,
    )
    assert not off.has_error
    assert not iso.has_error
    assert iso.rir != off.rir
    # Air absorption can only take energy out of the statistical tail; sample
    # 9600 (200 ms) is past the ~100 ms crossover for this room.
    assert sum(v * v for v in iso.rir[9600:]) < sum(v * v for v in off.rir[9600:])

    # A zeroed climate resolves to the ISO reference, matching the seed and
    # crossfade_ms convention the rest of this config already follows.
    implicit = libsonare.synthesize_rir(**hall, air_absorption_enabled=True)
    assert implicit.rir == iso.rir

    # Both climate values are read individually.
    warm = libsonare.synthesize_rir(
        **hall, air_absorption_enabled=True, air_temperature_c=35.0, air_humidity_percent=50.0
    )
    humid = libsonare.synthesize_rir(
        **hall, air_absorption_enabled=True, air_temperature_c=20.0, air_humidity_percent=90.0
    )
    assert warm.rir != iso.rir
    assert humid.rir != iso.rir
    assert warm.rir != humid.rir


@acoustic
def test_synthesize_rir_reports_an_implausible_air_climate() -> None:
    hall = dict(length_m=30.0, width_m=24.0, height_m=15.0, absorption=0.2, max_seconds=0.2)
    bad = libsonare.synthesize_rir(**hall, air_absorption_enabled=True, air_temperature_c=-500.0)
    assert bad.has_error
    assert "acoustic.invalid_air_absorption" in bad.error_message
    assert bad.rir == []
    # The same implausible value is ignored while the flag is off.
    ignored = libsonare.synthesize_rir(**hall, air_temperature_c=-500.0)
    assert not ignored.has_error


@acoustic
def test_room_morph_routes_air_absorption_and_rejects_a_bad_climate() -> None:
    samples = [0.0] * 4000
    samples[0] = 1.0
    target = dict(
        source=(3.0, 3.0, 1.5),
        listener=(10.0, 8.0, 1.7),
        absorption=0.2,
        wet=1.0,
        max_seconds=0.3,
        ism_order=2,
        seed=3,
    )
    off = libsonare.room_morph(samples, 48000, 30.0, 24.0, 15.0, **target)
    on = libsonare.room_morph(
        samples,
        48000,
        30.0,
        24.0,
        15.0,
        **target,
        air_absorption_enabled=True,
        air_temperature_c=20.0,
        air_humidity_percent=50.0,
    )
    assert len(off) == len(on)
    assert off != on
    # The morph validates its config rather than diagnosing, so this raises.
    with pytest.raises(libsonare.SonareError):
        libsonare.room_morph(
            samples,
            48000,
            30.0,
            24.0,
            15.0,
            **target,
            air_absorption_enabled=True,
            air_humidity_percent=150.0,
        )
