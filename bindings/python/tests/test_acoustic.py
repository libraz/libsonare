"""Tests for the geometric room-acoustics bindings."""

from __future__ import annotations

import ctypes
import math
from collections.abc import Callable

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE


class _ForeignEstimateError(Exception):
    """Stands for a failure raised by code outside the binding."""


pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")


def _acoustic_available() -> bool:
    from libsonare._runtime import (
        SONARE_ERROR_NOT_SUPPORTED,
        SonareRirSynthConfig,
        SonareRirSynthResult,
        SonareRoomPlacement,
        _get_lib,
    )

    lib = _get_lib()
    config = SonareRirSynthConfig(
        length_m=7.0,
        width_m=5.0,
        height_m=3.0,
        source=SonareRoomPlacement(1.0, 1.0, 1.2),
        listener=SonareRoomPlacement(5.0, 4.0, 1.7),
        absorption=0.2,
        max_sec=0.05,
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
        7.0,
        5.0,
        3.0,
        source=(1.5, 1.0, 1.2),
        listener=(5.0, 4.0, 1.7),
        absorption=0.15,
        sample_rate=48000,
    )
    assert result.sample_rate == 48000
    assert len(result.rir) > 0
    assert any(abs(s) > 0.0 for s in result.rir)


@acoustic
def test_synthesize_rir_raises_for_invalid_geometry() -> None:
    # Source outside the room => geometry validation error => coded SonareError.
    with pytest.raises(libsonare.SonareValueError, match="acoustic.source_outside_room") as caught:
        libsonare.synthesize_rir(7.0, 5.0, 3.0, source=(99.0, 1.0, 1.2), sample_rate=48000)
    assert caught.value.code == libsonare.ErrorCode.INVALID_PARAMETER


@acoustic
def test_synthesize_rir_result_has_no_error_fields() -> None:
    result = libsonare.synthesize_rir(5.0, 4.0, 3.0, absorption=0.3, max_sec=0.2, sample_rate=48000)
    assert not hasattr(result, "has_error")
    assert not hasattr(result, "error_message")


@acoustic
def test_synthesize_rir_surfaces_non_fatal_diagnostics() -> None:
    # Warnings accompany SUCCESSFUL calls; without them a truncated RIR is
    # indistinguishable from a complete one.
    clamped = libsonare.synthesize_rir(
        12.0, 9.0, 5.0, absorption=0.05, max_sec=0.3, sample_rate=48000
    )
    clamp = next(d for d in clamped.diagnostics if d.code == "acoustic.rir_length_clamped")
    # Each entry keeps its own severity and message rather than arriving as one
    # flattened line, so the set matches what Node and WASM hand back.
    assert clamp.severity == "warning"
    assert clamp.message != ""

    # ... and a request that needed no clamping reports nothing, so the field is
    # a real signal rather than always-populated noise.
    clean = libsonare.synthesize_rir(5.0, 4.0, 3.0, absorption=0.3, max_sec=3.0, sample_rate=48000)
    assert clean.diagnostics == []


@acoustic
def test_estimate_room_round_trips_a_known_shoebox() -> None:
    rir = libsonare.synthesize_rir(
        7.0,
        5.0,
        3.0,
        source=(1.5, 1.0, 1.2),
        listener=(5.0, 4.0, 1.7),
        absorption=0.15,
        sample_rate=48000,
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
    assert len(est.band_absorption) == len(est.rt60_bands)
    assert math.isfinite(est.drr_db)
    assert not hasattr(est, "drrDb")


@acoustic
@pytest.mark.slow  # two full RIR syntheses (~10 s); run via `make test-python-slow`
def test_room_morph_adds_a_target_tail_and_is_deterministic() -> None:
    # A short impulse-like recording morphed toward a live target room.
    samples = [0.0] * 4000
    samples[0] = 1.0
    out_a = libsonare.room_morph(samples, 48000, 12.0, 9.0, 5.0, absorption=0.08, wet=0.7).audio
    out_b = libsonare.room_morph(samples, 48000, 12.0, 9.0, 5.0, absorption=0.08, wet=0.7).audio
    assert len(out_a) > len(samples)  # target reverb tail appended
    assert out_a == out_b  # deterministic for a fixed seed
    assert all(math.isfinite(s) for s in out_a)


@acoustic
def test_estimate_room_zero_confidence_for_silence() -> None:
    est = libsonare.estimate_room([0.0] * 48000, sample_rate=48000)
    assert est.confidence == 0.0
    assert all(math.isnan(v) for v in (est.volume, est.length_m, est.width_m, est.height_m))


@acoustic
def test_room_estimate_and_morph_reject_invalid_configuration() -> None:
    samples = [1.0] + [0.0] * 1999
    with pytest.raises(libsonare.SonareError):
        libsonare.estimate_room(samples, sample_rate=48000, reference_absorption=float("nan"))
    with pytest.raises(libsonare.SonareError):
        libsonare.estimate_room(samples, sample_rate=48000, aspect_hint_lw=float("inf"))
    # A given value is read literally: 0 is outside these domains, and a negative margin too.
    for keyword, outside in (
        ("reference_absorption", 0),
        ("n_octave_bands", 0),
        ("min_decay_db", 0),
        ("noise_floor_margin_db", -1),
    ):
        with pytest.raises(libsonare.SonareValueError, match=keyword):
            libsonare.estimate_room(samples, sample_rate=48000, **{keyword: outside})
    libsonare.estimate_room(samples, sample_rate=48000, noise_floor_margin_db=0)
    with pytest.raises(libsonare.SonareError):
        libsonare.room_morph(samples, 48000, -1.0, 4.0, 3.0)
    with pytest.raises(libsonare.SonareError):
        libsonare.room_morph(samples, 48000, 5.0, 4.0, 3.0, wet=float("nan"))


@acoustic
def test_synthesize_rir_uses_default_room_dimensions() -> None:
    # Room dimensions default to 7 x 5 x 3 to match the Node/WASM/CLI bindings.
    result = libsonare.synthesize_rir(sample_rate=48000)
    assert result.sample_rate == 48000
    assert len(result.rir) > 0


@acoustic
def test_synthesize_rir_late_model_is_honored() -> None:
    # A more absorptive room makes Sabine and Eyring diverge; selecting the model
    # via prefer_eyring must change the synthesized tail.
    sabine = libsonare.synthesize_rir(
        7.0, 5.0, 3.0, absorption=0.4, max_sec=0.3, prefer_eyring=False, sample_rate=48000
    )
    eyring = libsonare.synthesize_rir(
        7.0, 5.0, 3.0, absorption=0.4, max_sec=0.3, prefer_eyring=True, sample_rate=48000
    )
    assert not np.array_equal(sabine.rir, eyring.rir)


@acoustic
def test_synthesize_rir_honors_per_band_scattering() -> None:
    base = dict(
        sample_rate=48000,
        length_m=7.0,
        width_m=5.0,
        height_m=3.0,
        band_absorption=[0.2, 0.22, 0.24, 0.26],
        max_sec=0.3,
        seed=123,
    )
    mirror = libsonare.synthesize_rir(**base, band_scattering=[0.0, 0.0, 0.0, 0.0])
    diffuse = libsonare.synthesize_rir(**base, band_scattering=[0.8, 0.8, 0.8, 0.8])
    assert not np.array_equal(diffuse.rir, mirror.rir)


@acoustic
def test_band_arguments_use_the_node_and_wasm_spelling() -> None:
    room = dict(length_m=7.0, width_m=5.0, height_m=3.0, max_sec=0.2)
    with pytest.raises(TypeError, match="absorption_bands"):
        libsonare.synthesize_rir(**room, absorption_bands=[0.2])
    with pytest.raises(TypeError, match="scattering_bands"):
        libsonare.synthesize_rir(**room, scattering_bands=[0.2])
    rir = libsonare.synthesize_rir(7.0, 5.0, 3.0, absorption=0.15, sample_rate=48000)
    est = libsonare.estimate_room(
        rir.rir, sample_rate=48000, mode="impulseResponse", min_decay_db=25.0
    )
    assert not hasattr(est, "absorption_bands")
    assert not hasattr(est, "bandAbsorption")


@acoustic
def test_estimate_room_band_arrays_share_length() -> None:
    rir = libsonare.synthesize_rir(7.0, 5.0, 3.0, absorption=0.15, sample_rate=48000)
    est = libsonare.estimate_room(
        rir.rir, sample_rate=48000, mode="impulseResponse", min_decay_db=25.0
    )
    assert len(est.band_absorption) == len(est.rt60_bands)


def test_estimate_room_frees_a_partial_foreign_result_before_check(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An error result from an older library still gets its owned buffers freed."""
    import libsonare._acoustic as acoustic_module

    real_library = acoustic_module._get_lib()

    class ForeignLibrary:
        def __init__(self) -> None:
            self.absorption = (ctypes.c_float * 1)(0.25)
            self.rt60 = (ctypes.c_float * 1)(0.5)
            self.estimate_calls = 0
            self.free_calls = 0

        def __getattr__(self, name: str) -> object:
            # Keep selector metadata and every other ABI accessor on the real
            # current library. Only the estimate/free pair is foreign here.
            return getattr(real_library, name)

        def sonare_estimate_room(self, *_args: object) -> int:
            self.estimate_calls += 1
            out = _args[-1]._obj  # type: ignore[attr-defined]
            out.band_absorption = ctypes.cast(self.absorption, ctypes.POINTER(ctypes.c_float))
            out.rt60_bands = ctypes.cast(self.rt60, ctypes.POINTER(ctypes.c_float))
            out.band_count = 1
            return int(libsonare.ErrorCode.INVALID_PARAMETER)

        def sonare_free_room_estimate(self, out_arg: object) -> None:
            out = out_arg._obj  # type: ignore[attr-defined]
            assert bool(out.band_absorption)
            assert bool(out.rt60_bands)
            self.free_calls += 1

    foreign = ForeignLibrary()
    monkeypatch.setattr(acoustic_module, "_get_lib", lambda: foreign)

    def raise_from_check(_rc: int, *, flattened: str | None = None) -> None:
        raise _ForeignEstimateError("foreign estimate failure")

    monkeypatch.setattr(acoustic_module, "_check", raise_from_check)
    with pytest.raises(_ForeignEstimateError, match="foreign estimate failure"):
        acoustic_module.estimate_room([0.0, 1.0], sample_rate=48000)
    assert foreign.estimate_calls == 1
    assert foreign.free_calls == 1


@acoustic
def test_synthesize_rir_routes_air_absorption_through_the_c_abi() -> None:
    # The ISO 9613-1 atmospheric term used to be reachable only through the
    # streaming insert; the offline entry point had no way to ask for it. Each
    # assertion pins one of the three ctypes fields, so dropping any single one
    # fails here instead of hiding behind the other two.
    hall = dict(
        sample_rate=48000,
        length_m=30.0,
        width_m=24.0,
        height_m=15.0,
        source=(3.0, 3.0, 1.5),
        listener=(10.0, 8.0, 1.7),
        absorption=0.2,
        max_sec=0.5,
        ism_order=2,
        seed=3,
    )
    off = libsonare.synthesize_rir(**hall, air_absorption_enabled=False)
    iso = libsonare.synthesize_rir(
        **hall,
        air_absorption_enabled=True,
        air_temperature_c=20.0,
        air_humidity_percent=50.0,
    )
    assert not np.array_equal(iso.rir, off.rir)
    # Air absorption can only take energy out of the statistical tail; sample
    # 9600 (200 ms) is past the ~100 ms crossover for this room.
    assert sum(v * v for v in iso.rir[9600:]) < sum(v * v for v in off.rir[9600:])

    # An absent climate is the ISO reference; an explicit 0 degC / 0 % is a climate.
    implicit = libsonare.synthesize_rir(**hall, air_absorption_enabled=True)
    np.testing.assert_array_equal(implicit.rir, iso.rir)
    freezing = libsonare.synthesize_rir(**hall, air_absorption_enabled=True, air_temperature_c=0.0)
    arid = libsonare.synthesize_rir(**hall, air_absorption_enabled=True, air_humidity_percent=0.0)
    assert not np.array_equal(freezing.rir, iso.rir)
    assert not np.array_equal(arid.rir, iso.rir)

    # Air absorption is on by default; False is the geometry-only RIR.
    np.testing.assert_array_equal(libsonare.synthesize_rir(**hall).rir, iso.rir)

    # Both climate values are read individually.
    warm = libsonare.synthesize_rir(
        **hall, air_absorption_enabled=True, air_temperature_c=35.0, air_humidity_percent=50.0
    )
    humid = libsonare.synthesize_rir(
        **hall, air_absorption_enabled=True, air_temperature_c=20.0, air_humidity_percent=90.0
    )
    assert not np.array_equal(warm.rir, iso.rir)
    assert not np.array_equal(humid.rir, iso.rir)
    assert not np.array_equal(warm.rir, humid.rir)


@acoustic
def test_synthesize_rir_raises_on_an_implausible_air_climate() -> None:
    hall = dict(
        length_m=30.0,
        width_m=24.0,
        height_m=15.0,
        absorption=0.2,
        max_sec=0.2,
        sample_rate=48000,
    )
    with pytest.raises(libsonare.SonareError, match="acoustic.invalid_air_absorption"):
        libsonare.synthesize_rir(**hall, air_absorption_enabled=True, air_temperature_c=-500.0)
    # The same implausible value is ignored while the flag is off.
    libsonare.synthesize_rir(**hall, air_absorption_enabled=False, air_temperature_c=-500.0)


@acoustic
def test_room_morph_routes_air_absorption_and_rejects_a_bad_climate() -> None:
    samples = [0.0] * 4000
    samples[0] = 1.0
    target = dict(
        source=(3.0, 3.0, 1.5),
        listener=(10.0, 8.0, 1.7),
        absorption=0.2,
        wet=1.0,
        max_sec=0.3,
        ism_order=2,
        seed=3,
    )
    off = libsonare.room_morph(
        samples, 48000, 30.0, 24.0, 15.0, **target, air_absorption_enabled=False
    ).audio
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
    ).audio
    assert len(off) == len(on)
    assert not np.array_equal(off, on)
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


# Band arrays reach the C ABI through one optional-array helper shared by
# `synthesize_rir` and `room_morph`, for both the absorption and the scattering
# bands. The rows below drive a numpy input at each of the lengths whose
# `bool()` behaviour differs: empty and 2+ raise, and a single element answers
# by value -- so a zero band used to be indistinguishable from no bands at all.
_BAND_ROOM = dict(sample_rate=22050, max_sec=0.2)
_BAND_SHAPES = ([], [0.0], [0.9], [0.1, 0.2, 0.3, 0.4, 0.5, 0.6])


@acoustic
def test_synthesize_rir_accepts_a_numpy_band_array_of_any_length() -> None:
    default = libsonare.synthesize_rir(**_BAND_ROOM)
    # The comparison quantity must be able to move: a silent RIR would make
    # every assertion below pass vacuously.
    assert max(abs(s) for s in default.rir) > 1e-4

    for shape in _BAND_SHAPES:
        bands = np.array(shape, dtype=np.float32)
        result = libsonare.synthesize_rir(band_absorption=bands, **_BAND_ROOM)
        scattering = libsonare.synthesize_rir(band_scattering=bands, **_BAND_ROOM)
        assert max(abs(s) for s in result.rir) > 1e-4
        assert max(abs(s) for s in scattering.rir) > 1e-4


@acoustic
def test_a_single_zero_band_array_is_not_read_as_absent() -> None:
    # The quiet half of the defect: `bool(np.array([0.0]))` is False, so this
    # request silently rendered the library default instead of the zero band.
    # An absence-of-exception check cannot see it -- the outcome has to move.
    default = libsonare.synthesize_rir(**_BAND_ROOM)
    zero_band = libsonare.synthesize_rir(
        band_absorption=np.array([0.0], dtype=np.float32), **_BAND_ROOM
    )
    assert max(abs(s) for s in zero_band.rir) > 1e-4
    assert not np.array_equal(zero_band.rir, default.rir)


@acoustic
def test_band_arrays_agree_between_a_list_and_a_numpy_array() -> None:
    # The positive control: without it, a fix that rejected every numpy input
    # would satisfy every rejection test above.
    for shape in _BAND_SHAPES:
        from_list = libsonare.synthesize_rir(band_absorption=shape, **_BAND_ROOM)
        from_array = libsonare.synthesize_rir(
            band_absorption=np.array(shape, dtype=np.float32), **_BAND_ROOM
        )
        np.testing.assert_array_equal(from_array.rir, from_list.rir)
        assert from_array.sample_rate == from_list.sample_rate


@acoustic
def test_an_empty_band_array_reads_as_absent() -> None:
    # Empty means absent for a list and must mean the same for a numpy array,
    # whose `bool()` raises rather than answering.
    default = libsonare.synthesize_rir(**_BAND_ROOM)
    np.testing.assert_array_equal(
        libsonare.synthesize_rir(band_absorption=[], **_BAND_ROOM).rir, default.rir
    )
    empty = libsonare.synthesize_rir(band_absorption=np.array([], dtype=np.float32), **_BAND_ROOM)
    np.testing.assert_array_equal(empty.rir, default.rir)


@acoustic
def test_a_malformed_band_array_is_rejected_naming_the_parameter() -> None:
    # Rejection is the binding's own, so it carries the caller's parameter name
    # rather than a bare numpy message about an ambiguous truth value.
    with pytest.raises(libsonare.SonareValueError, match="band_absorption"):
        libsonare.synthesize_rir(band_absorption=np.zeros((2, 3), dtype=np.float32), **_BAND_ROOM)
    with pytest.raises(libsonare.SonareValueError, match="band_scattering"):
        libsonare.synthesize_rir(band_scattering="not a buffer", **_BAND_ROOM)


@acoustic
def test_room_morph_accepts_numpy_band_arrays() -> None:
    samples = [math.sin(2.0 * math.pi * 220.0 * i / 22050.0) for i in range(4410)]
    room = dict(sample_rate=22050, length_m=7.0, width_m=5.0, height_m=3.0, max_sec=0.2)
    default = libsonare.room_morph(samples, **room).audio
    assert max(abs(s) for s in default) > 1e-4

    zero_band = libsonare.room_morph(
        samples, band_absorption=np.array([0.0], dtype=np.float32), **room
    ).audio
    assert not np.array_equal(zero_band, default)
    np.testing.assert_array_equal(
        libsonare.room_morph(
            samples, band_absorption=np.array([0.0], dtype=np.float32), **room
        ).audio,
        libsonare.room_morph(samples, band_absorption=[0.0], **room).audio,
    )
    scattered = libsonare.room_morph(
        samples, band_scattering=np.array([0.3, 0.4, 0.5], dtype=np.float32), **room
    ).audio
    np.testing.assert_array_equal(
        scattered, libsonare.room_morph(samples, band_scattering=[0.3, 0.4, 0.5], **room).audio
    )


@acoustic
def test_a_seed_the_c_field_cannot_express_is_rejected_rather_than_folded() -> None:
    # Node and WASM both refuse one outside the C field's uint32 range. Folding a
    # negative to 0 here made -1 and the library default indistinguishable under a
    # successful call, so two different requests returned the same tail.
    room = dict(sample_rate=22050, length_m=7.0, width_m=5.0, height_m=3.0, max_sec=0.2)
    for invalid in (-1, 0x1_0000_0000):
        with pytest.raises(libsonare.SonareValueError, match="seed"):
            libsonare.synthesize_rir(seed=invalid, **room)
        with pytest.raises(libsonare.SonareValueError, match="seed"):
            libsonare.room_morph([0.0] * 2205, seed=invalid, **room)

    # The whole uint32 range stays reachable, and 0 is a seed of its own.
    assert len(libsonare.synthesize_rir(seed=0xFFFF_FFFF, **room).rir) > 0
    assert not np.array_equal(
        libsonare.synthesize_rir(seed=0, **room).rir, libsonare.synthesize_rir(**room).rir
    )


_ROOM = dict(sample_rate=22050, length_m=7.0, width_m=5.0, height_m=3.0, max_sec=0.2)


def _room_calls(**kwargs: object) -> list[Callable[[], object]]:
    mono = [0.0] * 2205
    return [
        lambda: libsonare.synthesize_rir(**_ROOM, **kwargs),
        lambda: libsonare.room_morph(mono, **_ROOM, **kwargs),
        lambda: libsonare.room_morph_stereo(mono, mono, **_ROOM, **kwargs),
    ]


@acoustic
@pytest.mark.parametrize(
    ("name", "invalid"),
    [
        (name, invalid)
        for name in ("ism_order", "seed")
        for invalid in (2.5, True, 2**31 if name == "ism_order" else 2**32, -1)
    ],
)
def test_a_room_integer_the_c_field_cannot_hold_is_refused_by_name(
    name: str, invalid: object
) -> None:
    # A bool is the wrong type; every other value is out of the field's domain.
    expected = TypeError if isinstance(invalid, bool) else libsonare.SonareValueError
    for call in _room_calls(**{name: invalid}):
        with pytest.raises(expected, match=name):
            call()


@acoustic
def test_every_room_integer_in_range_is_accepted() -> None:
    for kwargs in ({"ism_order": 0}, {"ism_order": 2}, {"seed": 0}, {"seed": 2**32 - 1}):
        for call in _room_calls(**kwargs):
            call()


@acoustic
@pytest.mark.parametrize(
    ("keyword", "default", "literal"),
    [
        ("seed", 1, 0),
        ("crossfade_ms", 5.0, 0.0),
        ("air_temperature_c", 20.0, 0.0),
        ("air_humidity_percent", 50.0, 0.0),
    ],
)
def test_an_absent_room_keyword_is_the_documented_default_and_zero_is_literal(
    keyword: str, default: float, literal: float
) -> None:
    room = dict(sample_rate=22050, length_m=7.0, width_m=5.0, height_m=3.0, max_sec=0.2)
    absent = libsonare.synthesize_rir(**room).rir
    np.testing.assert_array_equal(
        libsonare.synthesize_rir(**room, **{keyword: default}).rir, absent
    )
    assert not np.array_equal(libsonare.synthesize_rir(**room, **{keyword: literal}).rir, absent)

    samples = [0.0] * 2205
    samples[0] = 1.0
    morph_absent = libsonare.room_morph(samples, **room).audio
    np.testing.assert_array_equal(
        libsonare.room_morph(samples, **room, **{keyword: default}).audio, morph_absent
    )
    assert not np.array_equal(
        libsonare.room_morph(samples, **room, **{keyword: literal}).audio, morph_absent
    )

    stereo_absent = libsonare.room_morph_stereo(samples, samples, **room).left
    stereo_default = libsonare.room_morph_stereo(samples, samples, **room, **{keyword: default})
    stereo_literal = libsonare.room_morph_stereo(samples, samples, **room, **{keyword: literal})
    np.testing.assert_array_equal(stereo_default.left, stereo_absent)
    assert not np.array_equal(stereo_literal.left, stereo_absent)


@acoustic
def test_room_morph_reports_the_target_synthesis_warnings() -> None:
    # The morph synthesizes its target RIR with the code synthesize_rir uses, so
    # the same clamp fires. It used to be dropped, leaving a morph through a room
    # the caller did not ask for indistinguishable from one through the room they
    # did.
    samples = [0.0] * 4000
    samples[0] = 1.0
    room = dict(sample_rate=48000, length_m=12.0, width_m=9.0, height_m=5.0, max_sec=2.0)

    clamped = libsonare.room_morph(samples, ism_order=99, **room)
    codes = [d.code for d in clamped.diagnostics]
    assert "acoustic.ism_order_clamped" in codes
    # An unusable morph raises rather than reporting, so nothing here is an error.
    assert {d.severity for d in clamped.diagnostics} == {"warning"}
    assert len(clamped.audio) > len(samples)

    # An order the synthesizer honours leaves the channel clear, so the entry
    # above is that run's rather than a slot nothing ever resets.
    quiet = libsonare.room_morph(samples, ism_order=2, **room)
    assert "acoustic.ism_order_clamped" not in [d.code for d in quiet.diagnostics]


@acoustic
def test_each_material_preset_name_builds_its_own_rir() -> None:
    room = dict(max_sec=0.1, sample_rate=22050, ism_order=2)
    names = ["none", "concrete", "wood", "curtain", "carpet", "glass"]
    rirs = [tuple(libsonare.synthesize_rir(material_preset=name, **room).rir) for name in names]
    assert len(set(rirs)) == len(names)


@acoustic
def test_a_number_for_a_preset_or_mode_is_a_type_error_naming_the_names() -> None:
    with pytest.raises(
        TypeError,
        match=(
            "^synthesize_rir: material_preset must be one of "
            "none, concrete, wood, curtain, carpet, glass$"
        ),
    ):
        libsonare.synthesize_rir(material_preset=2, sample_rate=48000)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="material_preset"):
        libsonare.room_morph([0.0] * 100, 48000, 7.0, 5.0, 3.0, material_preset=1)  # type: ignore[arg-type]
    with pytest.raises(
        TypeError, match="^estimate_room: mode must be one of auto, blind, impulseResponse$"
    ):
        libsonare.estimate_room([0.0] * 1000, mode=2, sample_rate=48000)  # type: ignore[arg-type]


@acoustic
def test_an_unknown_preset_or_mode_name_is_refused_naming_the_valid_set() -> None:
    with pytest.raises(
        libsonare.SonareValueError,
        match=(
            "^synthesize_rir: material_preset must be one of "
            "none, concrete, wood, curtain, carpet, glass$"
        ),
    ):
        libsonare.synthesize_rir(material_preset="marble", sample_rate=48000)
    with pytest.raises(libsonare.SonareValueError, match="material_preset"):
        libsonare.room_morph([0.0] * 100, 48000, 7.0, 5.0, 3.0, material_preset="Concrete")
    with pytest.raises(
        libsonare.SonareValueError,
        match="^estimate_room: mode must be one of auto, blind, impulseResponse$",
    ):
        libsonare.estimate_room([0.0] * 1000, mode="impulse_response", sample_rate=48000)


@acoustic
def test_room_geometry_from_estimate_feeds_synthesize_rir() -> None:
    rir = libsonare.synthesize_rir(
        7.0,
        5.0,
        3.0,
        source=(1.5, 1.0, 1.2),
        listener=(5.0, 4.0, 1.7),
        absorption=0.15,
        sample_rate=48000,
    )
    estimate = libsonare.estimate_room(
        rir.rir,
        sample_rate=48000,
        aspect_hint_lw=7.0 / 5.0,
        aspect_hint_lh=7.0 / 3.0,
        reference_absorption=0.15,
    )
    geometry = libsonare.room_geometry_from_estimate(
        estimate, source=(1.0, 1.0, 1.2), listener=(3.0, 2.0, 1.7)
    )
    assert set(geometry) == {
        "length_m",
        "width_m",
        "height_m",
        "absorption",
        "band_absorption",
        "source",
        "listener",
    }
    assert geometry["length_m"] == pytest.approx(estimate.length_m, rel=1e-6)
    assert geometry["band_absorption"] == pytest.approx(estimate.band_absorption, rel=1e-6)
    options = dict(max_sec=0.1, ism_order=2, sample_rate=48000)
    mapped = libsonare.synthesize_rir(**geometry, **options)
    by_hand = libsonare.synthesize_rir(
        estimate.length_m,
        estimate.width_m,
        estimate.height_m,
        source=(1.0, 1.0, 1.2),
        listener=(3.0, 2.0, 1.7),
        band_absorption=estimate.band_absorption,
        **options,
    )
    np.testing.assert_array_equal(mapped.rir, by_hand.rir)
    # Omitted positions take the default placement scaled into the estimated room.
    scaled = libsonare.room_geometry_from_estimate(estimate)
    assert len(scaled["source"]) == 3 and len(scaled["listener"]) == 3
    assert scaled["absorption"] == pytest.approx(
        sum(estimate.band_absorption) / len(estimate.band_absorption), rel=1e-5
    )


@acoustic
def test_room_geometry_from_estimate_places_a_small_room_inside_itself() -> None:
    estimate = libsonare.RoomEstimate(
        volume=2.35,
        length_m=1.33,
        width_m=1.33,
        height_m=1.33,
        drr_db=0.0,
        confidence=1.0,
        band_absorption=[0.2, 0.3],
        rt60_bands=[0.5, 0.5],
    )
    geometry = libsonare.room_geometry_from_estimate(estimate)
    for point in (geometry["source"], geometry["listener"]):
        assert all(0.0 < v < 1.33 for v in point)
    assert geometry["absorption"] == pytest.approx(0.25, rel=1e-6)
    assert (
        libsonare.synthesize_rir(**geometry, max_sec=0.1, ism_order=2, sample_rate=48000).rir.size
        > 0
    )
    full = libsonare.RoomEstimate(
        volume=105.0,
        length_m=7.0,
        width_m=5.0,
        height_m=3.0,
        drr_db=0.0,
        confidence=1.0,
        band_absorption=[float("nan")],
        rt60_bands=[0.5],
    )
    placed = libsonare.room_geometry_from_estimate(full)
    assert placed["source"] == pytest.approx((1.0, 1.0, 1.2))
    assert placed["listener"] == pytest.approx((5.0, 4.0, 1.7))
    assert "absorption" not in placed


@acoustic
def test_room_geometry_from_estimate_refuses_an_unmeasured_room() -> None:
    silent = libsonare.estimate_room([0.0] * 48000, sample_rate=48000)
    assert math.isnan(silent.length_m)
    # The estimate's own NaN reaches the library, which names the cause.
    with pytest.raises(
        libsonare.SonareValueError,
        match=r"^room_geometry_from_estimate: room estimate has no measurable dimensions",
    ):
        libsonare.room_geometry_from_estimate(silent)
    with pytest.raises(libsonare.SonareValueError, match="triple"):
        libsonare.room_geometry_from_estimate(
            libsonare.RoomEstimate(100.0, 6.0, 5.0, 3.0, 1.0, 0.5, [], []), source=(1.0, 2.0)
        )


@acoustic
def test_an_unmeasured_estimate_leaves_the_dereverb_delay_as_given() -> None:
    silent = libsonare.estimate_room([0.0] * 48000, sample_rate=48000)
    assert math.isnan(silent.volume)
    config = libsonare.mastering_repair_dereverb_config_for_room(silent, late_delay_ms=33.0)
    assert config["late_delay_ms"] == pytest.approx(33.0)


_STEREO_ROOM = dict(
    sample_rate=48000, length_m=12.0, width_m=9.0, height_m=5.0, max_sec=0.5, ism_order=2
)


def _impulse(n: int = 2000) -> list[float]:
    samples = [0.0] * n
    samples[0] = 1.0
    return samples


@acoustic
def test_room_morph_stereo_returns_two_different_longer_channels() -> None:
    samples = _impulse()
    result = libsonare.room_morph_stereo(samples, samples, **_STEREO_ROOM)
    assert len(result.left) == len(result.right) > len(samples)
    assert result.sample_rate == 48000
    assert not hasattr(result, "sampleRate")
    assert not np.array_equal(result.left, result.right)
    assert all(math.isfinite(s) for s in result.left + result.right)


@acoustic
def test_room_morph_stereo_default_spacing_equals_explicit_half_metre() -> None:
    samples = _impulse()
    default = libsonare.room_morph_stereo(samples, samples, **_STEREO_ROOM)
    explicit = libsonare.room_morph_stereo(samples, samples, receiver_spacing_m=0.5, **_STEREO_ROOM)
    np.testing.assert_array_equal(default.left, explicit.left)
    np.testing.assert_array_equal(default.right, explicit.right)


@acoustic
@pytest.mark.parametrize("spacing", [0.0, -1.0, 4.5, 1e40, float("nan"), float("inf")])
def test_room_morph_stereo_refuses_an_out_of_range_spacing(spacing: float) -> None:
    samples = _impulse(100)
    libsonare.room_morph_stereo(samples, samples, receiver_spacing_m=4.0, **_STEREO_ROOM)
    with pytest.raises(
        libsonare.SonareValueError, match=r": receiver_spacing_m must be finite and in \(0, 4\]$"
    ):
        libsonare.room_morph_stereo(samples, samples, receiver_spacing_m=spacing, **_STEREO_ROOM)


@acoustic
def test_room_morph_stereo_refuses_mismatched_channel_lengths() -> None:
    with pytest.raises(libsonare.SonareValueError, match="same length"):
        libsonare.room_morph_stereo(_impulse(100), _impulse(99), **_STEREO_ROOM)


@acoustic
def test_room_morph_stereo_reports_the_target_synthesis_warnings() -> None:
    samples = _impulse()
    room = {**_STEREO_ROOM, "max_sec": 0.05}
    result = libsonare.room_morph_stereo(samples, samples, **room)
    assert "acoustic.rir_length_clamped" in [d.code for d in result.diagnostics]


@acoustic
def test_room_morph_stereo_refuses_a_listener_too_close_to_a_wall() -> None:
    samples = _impulse(100)
    with pytest.raises(libsonare.SonareError):
        libsonare.room_morph_stereo(samples, samples, listener=(0.1, 4.0, 1.7), **_STEREO_ROOM)
