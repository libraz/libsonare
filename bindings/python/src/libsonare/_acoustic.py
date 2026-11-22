"""Geometric room-acoustics wrappers for libsonare.

These wrap the offline acoustic C ABI (RIR synthesis from room geometry, blind
equivalent-room estimation, and the room-character morph). They are available
only when libsonare was built with acoustic-simulation support; each raises a
clear ``RuntimeError`` otherwise. Feature-off shared libraries may still export
the C symbols as stubs that return ``SONARE_ERROR_NOT_SUPPORTED``. The
streaming engines (RoomReverb, RoomMorph) remain reachable through the insert
API by name ("effects.reverb.room", "effects.acoustic.roomMorph").
"""

from __future__ import annotations

import ctypes
import math
from collections.abc import Sequence
from typing import TypeVar

from ._errors import _entry, _refuse, _refuse_type
from ._ffi_types_analysis import SonareRoomPlacement
from ._narrowing import _domain_float, _float_data, _narrow_double, _narrow_float
from ._runtime import (
    SonareRirSynthConfig,
    SonareRirSynthResult,
    SonareRoomEstimate,
    SonareRoomEstimateConfig,
    SonareRoomMorphConfig,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _last_diagnostics,
    _optional_from_c_float_array,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _to_c_uint,
    _validate_c_int_field,
)
from .types import (
    AcousticModeName,
    MaterialPresetName,
    RirDiagnostic,
    RirResult,
    RoomEstimate,
    RoomGeometry,
    RoomMorphResult,
    RoomMorphStereoResult,
)

# SonareDiagnosticSeverity ordinals (sonare_c_types_enums.h), mapped to the
# strings Node and WASM put on RirDiagnostic.severity.
_DIAGNOSTIC_SEVERITIES = ("info", "warning", "error")


def _read_diagnostics() -> list[RirDiagnostic]:
    """Reads the structured diagnostic channel the last C call published.

    Must run before any later C ABI call can replace the thread-local list, the
    same contract the error and warning strings carry.
    """
    return [
        RirDiagnostic(
            code=code,
            message=message,
            severity=_DIAGNOSTIC_SEVERITIES[severity]
            if 0 <= severity < len(_DIAGNOSTIC_SEVERITIES)
            else "info",
        )
        for code, message, severity in _last_diagnostics()
    ]


def _named_selector(value: str, argument: str, prefix: str) -> int:
    """Resolves a selector given by name; anything but a string is a ``TypeError``.

    ``prefix`` names the C ABI's ``sonare_<prefix>_name`` getter, whose values
    are contiguous from 0, so a name's position is its selector value.
    """
    name_of = getattr(_get_lib(), f"sonare_{prefix}_name")
    valid: list[str] = []
    while (name := name_of(len(valid))) is not None:
        valid.append(name.decode())
    if not isinstance(value, str):
        raise _refuse_type(f"{argument} must be one of {', '.join(valid)}")
    if value not in valid:
        raise _refuse(f"{argument} must be one of {', '.join(valid)}")
    return valid.index(value)


def _material_preset(value: MaterialPresetName) -> int:
    return _named_selector(value, "material_preset", "material_preset")


def _acoustic_mode(value: AcousticModeName) -> int:
    return _named_selector(value, "mode", "acoustic_mode")


def _band_array_args(
    bands: Sequence[float] | None,
    *,
    arg_name: str = "bands",
) -> tuple[object, int, object]:
    """Build the (pointer, count, owner) tuple for an optional per-band array.

    The third element keeps the backing ctypes buffer alive for the duration of
    the FFI call (the config only stores a borrowed pointer).

    ``None`` and an empty array are both absent (the library default applies).
    Emptiness is read from the coerced element count, never from truthiness: a
    numpy array answers ``bool()`` by value at one element and raises at any
    other length, so only the count separates absent from a present zero band.

    ``arg_name`` names the caller's own parameter in a rejection, since every
    entry point reaching here spells its bands differently.
    """
    if bands is None:
        return None, 0, None
    # Bulk numpy marshalling like every other float buffer that crosses the C
    # ABI. A band list is short enough that the per-element varargs form cost
    # nothing measurable, but keeping one path means the rule holds without an
    # exception list -- which is what let the two clip paths drift.
    buf, count = _to_c_float_array(bands, arg_name=arg_name)
    if count == 0:
        return None, 0, None
    return ctypes.cast(buf, ctypes.POINTER(ctypes.c_float)), count, buf


_RoomConfig = TypeVar("_RoomConfig", SonareRirSynthConfig, SonareRoomMorphConfig)


def _place(placement: SonareRoomPlacement, point: tuple[float, float, float], name: str) -> None:
    """Write ``point`` into ``placement``, refusing a component by its index in ``name``."""
    placement.x, placement.y, placement.z = (
        _narrow_float(value, f"{name}[{i}]") for i, value in enumerate(point)
    )


def _fill_room_config(
    config: _RoomConfig,
    *,
    source: tuple[float, float, float] | None,
    listener: tuple[float, float, float] | None,
    band_ptr: object,
    band_count: int,
    scatter_ptr: object,
    scatter_count: int,
    material_preset: MaterialPresetName,
    prefer_eyring: bool,
    air_absorption_enabled: bool,
    seed: int | None,
    ism_order: int,
    **fields: float | ctypes.c_float | None,
) -> _RoomConfig:
    """Overwrite an ``_init``-filled config with every field the caller gave.

    A ``None`` keyword leaves the ``_init`` value; every other value is written
    literally, so a 0 seed, a 0 ms crossfade and a 0 degC climate are honoured.
    A ``None`` placement stays all-NaN, which the library resolves to the default
    placement scaled to the room.
    """
    if source is not None:
        _place(config.source, source, "source")
    if listener is not None:
        _place(config.listener, listener, "listener")
    config.band_absorption = band_ptr
    config.band_absorption_count = band_count
    config.band_scattering = scatter_ptr
    config.band_scattering_count = scatter_count
    config.material_preset = _material_preset(material_preset)
    config.prefer_eyring = int(prefer_eyring)
    config.air_absorption_enabled = int(air_absorption_enabled)
    config.ism_order = _to_c_int(ism_order, "ism_order").value
    if seed is not None:
        config.seed = _to_c_uint(seed, "seed").value
    for name in ("max_sec", "mixing_time_ms"):
        if fields.get(name) == 0:
            raise _refuse(f"{name} must not be 0; omit it for the automatic value")
    for name, value in fields.items():
        if value is not None:
            setattr(config, name, value)
    return config


@_entry("synthesize_rir")
def synthesize_rir(
    length_m: float = 7.0,
    width_m: float = 5.0,
    height_m: float = 3.0,
    *,
    source: tuple[float, float, float] | None = None,
    listener: tuple[float, float, float] | None = None,
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    sample_rate: int,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int | None = None,
    max_sec: float | None = None,
    mixing_time_ms: float | None = None,
    crossfade_ms: float | None = None,
    air_absorption_enabled: bool = True,
    air_temperature_c: float | None = None,
    air_humidity_percent: float | None = None,
) -> RirResult:
    """Synthesize a room impulse response from shoebox geometry.

    Args:
        length_m, width_m, height_m: Room dimensions in metres (default
            7 x 5 x 3, matching the other bindings).
        source, listener: (x, y, z) positions inside the room, in metres;
            None = the default placement scaled to the room.
        absorption: Uniform wall absorption, clamped to [0, 0.999].
        band_absorption: Optional per-octave-band wall absorption
            (125/250/500/1k/2k/4k.. Hz). When given it overrides ``absorption``
            (unless ``material_preset`` selects a named preset). The late
            tail's decay time runs continuously between octave centres, so
            where absorption (a preset's included) changes steeply from one
            octave to the next, the octave-band RT60 measured back from the
            result leans toward the slower neighbour, as it does for a real
            room; the design value holds at the octave centre.
        band_scattering: Optional per-octave-band wall scattering; missing
            bands read as 0. Independent of ``band_absorption`` and
            ``material_preset`` -- it applies to whichever material the
            absorption precedence selected.
        material_preset: Named wall-material preset, by name (``"none"``,
            ``"concrete"``, ``"wood"``, ``"curtain"``, ``"carpet"``, ``"glass"``).
            Any preset but ``"none"`` wins over ``band_absorption`` and
            ``absorption``. An unknown name raises ``SonareValueError`` and a
            number ``TypeError``.
        sample_rate: Output sample rate in Hz.
        ism_order: Image-source reflection order.
        prefer_eyring: Use the Eyring statistical late-tail model (default);
            False selects Sabine.
        seed: Deterministic late-tail seed, in [0, 4294967295]; None = 1.
            Every value, 0 included, is a seed.
        max_sec: Hard RIR length cap; None keeps the natural length.
        mixing_time_ms: Early/late crossover in ms; None derives it (~sqrt(V) ms).
        crossfade_ms: Equal-power crossfade width around the mixing time in ms;
            None = 5 ms, and 0 is a hard switch.
        air_absorption_enabled: Add the ISO 9613-1 atmospheric-absorption term
            to the late tail's per-band RT60. On by default; it mainly shortens
            the high bands of a large room. ``False`` gives the geometry-only
            RT60.
        air_temperature_c: Air temperature in degrees Celsius; None = the ISO
            reference climate's 20 degC, and 0 is 0 degC.
        air_humidity_percent: Relative humidity in percent; None = the ISO
            reference climate's 50 %, and 0 is 0 %. Both climate values are read only while
            ``air_absorption_enabled`` is set, and an implausible pair raises
            ``SonareError`` naming the value.

    Returns:
        A :class:`RirResult`; its ``diagnostics`` are warnings.

    Raises:
        SonareError: The geometry, placement or timing is unusable (for example
            a source outside the room); the message names the refused value
            and the range it must lie in.
    """
    lib = _get_lib()
    bands_ptr, bands_count, _bands_owner = _band_array_args(
        band_absorption, arg_name="band_absorption"
    )
    scatter_ptr, scatter_count, _scatter_owner = _band_array_args(
        band_scattering, arg_name="band_scattering"
    )
    config = SonareRirSynthConfig()
    _fill_room_config(
        config,
        source=source,
        listener=listener,
        band_ptr=bands_ptr,
        band_count=bands_count,
        scatter_ptr=scatter_ptr,
        scatter_count=scatter_count,
        material_preset=material_preset,
        prefer_eyring=prefer_eyring,
        air_absorption_enabled=air_absorption_enabled,
        seed=seed,
        length_m=length_m,
        width_m=width_m,
        height_m=height_m,
        absorption=absorption,
        max_sec=max_sec,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        ism_order=ism_order,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
    )
    out = SonareRirSynthResult()
    rc = lib.sonare_synthesize_rir(
        ctypes.byref(config),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        # Non-fatal diagnostics are recorded on success too -- a max_sec clamp
        # that cut the tail, or a request degraded to early reflections only.
        # Read before any later C ABI call can overwrite the thread-local list.
        return RirResult(
            rir=_from_c_float_array(out.rir, out.length),
            sample_rate=int(out.sample_rate),
            diagnostics=_read_diagnostics(),
        )
    finally:
        lib.sonare_free_rir_synth_result(ctypes.byref(out))


@_entry("estimate_room")
@_guard_buffer("samples")
def estimate_room(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    aspect_hint_lw: float = 1.0,
    aspect_hint_lh: float = 1.0,
    reference_absorption: float | None = None,
    prefer_eyring: bool = True,
    n_octave_bands: int | None = None,
    mode: AcousticModeName = "auto",
    min_decay_db: float | None = None,
    noise_floor_margin_db: float | None = None,
) -> RoomEstimate:
    """Estimate an equivalent room from a recording (or impulse response).

    The volume scale is anchored by ``reference_absorption`` (the inverse
    problem is rank-deficient by one) and the shape by the aspect hints; the
    returned ``confidence`` reports how well the data support the estimate.

    Args:
        reference_absorption: Mean-absorption prior anchoring the volume scale;
            ``None`` keeps the library default (0.15); a given value must lie in
            ``[0.01, 0.99]``. The reported volume scales with the cube of the
            prior.
        mode: Analyzer routing, by name -- ``"auto"`` (impulse-like inputs route
            to IR analysis), ``"blind"`` or ``"impulseResponse"``. An unknown
            name raises ``SonareValueError`` and a number ``TypeError``.
        n_octave_bands: Analyzer octave-band count, at least 1; ``None`` keeps
            the library default (6).
        min_decay_db: Analyzer decay-fit span in dB, above 0; ``None`` keeps the
            library default (30).
        noise_floor_margin_db: Analyzer noise-floor margin in dB, at least 0;
            ``None`` keeps the library default (10).

    Raises:
        SonareValueError: If a given value is outside its domain: an aspect hint
            or ``min_decay_db`` not above 0, ``reference_absorption`` outside
            ``[0.01, 0.99]``, ``n_octave_bands`` below 1, or a negative
            ``noise_floor_margin_db``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareRoomEstimateConfig._given(
        aspect_hint_lw=aspect_hint_lw,
        aspect_hint_lh=aspect_hint_lh,
        reference_absorption=reference_absorption,
        min_decay_db=min_decay_db,
        noise_floor_margin_db=noise_floor_margin_db,
        prefer_eyring=1 if prefer_eyring else 0,
        n_octave_bands=(
            None
            if n_octave_bands is None
            else _validate_c_int_field(n_octave_bands, "n_octave_bands")
        ),
        mode=_acoustic_mode(mode),
    )
    out = SonareRoomEstimate()
    rc = lib.sonare_estimate_room(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc, flattened="config")
        count = out.band_count
        return RoomEstimate(
            volume=float(out.volume),
            length_m=float(out.length_m),
            width_m=float(out.width_m),
            height_m=float(out.height_m),
            drr_db=float(out.drr_db),
            confidence=float(out.confidence),
            band_absorption=_optional_from_c_float_array(out.band_absorption, count),
            rt60_bands=_optional_from_c_float_array(out.rt60_bands, count),
        )
    finally:
        lib.sonare_free_room_estimate(ctypes.byref(out))


def _placement(value: Sequence[float], argument: str) -> tuple[float, float, float]:
    if len(value) != 3:
        raise _refuse(f"{argument} must be an (x, y, z) triple")
    return (float(value[0]), float(value[1]), float(value[2]))


@_entry("room_geometry_from_estimate")
def room_geometry_from_estimate(
    estimate: RoomEstimate,
    *,
    source: Sequence[float] | None = None,
    listener: Sequence[float] | None = None,
) -> RoomGeometry:
    """Turn a room estimate into the geometry :func:`synthesize_rir` takes.

    The pair to :func:`estimate_room`: the estimate's ``length_m``, ``width_m``,
    ``height_m`` and ``band_absorption`` are already :func:`synthesize_rir`'s
    names, so this merges the placement in and drops what the estimate did not
    converge on::

        geometry = libsonare.room_geometry_from_estimate(estimate, source=(2, 1, 1.2),
                                                         listener=(3, 2, 1.7))
        rir = libsonare.synthesize_rir(**geometry)

    The result also carries a placement, the default source and listener
    positions scaled per axis into the estimated room (the default 7 x 5 x 3
    room maps to the default placement exactly), and ``absorption`` as the mean
    of the finite bands. ``source`` and ``listener`` are (x, y, z) positions in
    metres that replace the scaled placement when given. Absorption bands that
    did not converge are left out of ``band_absorption``.

    Raises:
        SonareValueError: The estimate has no measurable dimensions (NaN).
    """
    lib = _get_lib()
    bands_ptr, bands_count, _bands_owner = _band_array_args(
        estimate.band_absorption, arg_name="band_absorption"
    )
    # Each field mirrors the RoomEstimate field of the same name, which reads an
    # unmeasured value as NaN, so the fields are read as data rather than as arguments.
    c_estimate = SonareRoomEstimate(
        volume=_float_data(estimate.volume, "estimate.volume"),
        length_m=_float_data(estimate.length_m, "estimate.length_m"),
        width_m=_float_data(estimate.width_m, "estimate.width_m"),
        height_m=_float_data(estimate.height_m, "estimate.height_m"),
        drr_db=_float_data(estimate.drr_db, "estimate.drr_db"),
        confidence=_float_data(estimate.confidence, "estimate.confidence"),
        band_absorption=bands_ptr,
        rt60_bands=None,
        band_count=bands_count,
    )
    config = SonareRirSynthConfig()
    _check(lib.sonare_room_geometry_from_estimate(ctypes.byref(c_estimate), ctypes.byref(config)))
    geometry: RoomGeometry = {
        "length_m": _narrow_double(config.length_m, "config.length_m"),
        "width_m": _narrow_double(config.width_m, "config.width_m"),
        "height_m": _narrow_double(config.height_m, "config.height_m"),
        "source": (
            _narrow_double(config.source.x, "config.source.x"),
            _narrow_double(config.source.y, "config.source.y"),
            _narrow_double(config.source.z, "config.source.z"),
        ),
        "listener": (
            _narrow_double(config.listener.x, "config.listener.x"),
            _narrow_double(config.listener.y, "config.listener.y"),
            _narrow_double(config.listener.z, "config.listener.z"),
        ),
    }
    if any(math.isfinite(band) for band in estimate.band_absorption):
        geometry["absorption"] = _narrow_double(config.absorption, "config.absorption")
    if config.band_absorption_count > 0:
        geometry["band_absorption"] = _optional_from_c_float_array(
            config.band_absorption, config.band_absorption_count
        )
    if source is not None:
        geometry["source"] = _placement(source, "source")
    if listener is not None:
        geometry["listener"] = _placement(listener, "listener")
    return geometry


def _room_morph_config(
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    source: tuple[float, float, float] | None,
    listener: tuple[float, float, float] | None,
    absorption: float,
    band_absorption: Sequence[float] | None,
    band_scattering: Sequence[float] | None,
    material_preset: MaterialPresetName,
    source_tail_suppression: float,
    wet: float,
    ism_order: int,
    prefer_eyring: bool,
    seed: int | None,
    max_sec: float | None,
    mixing_time_ms: float | None,
    crossfade_ms: float | None,
    air_absorption_enabled: bool,
    air_temperature_c: float | None,
    air_humidity_percent: float | None,
    receiver_spacing_m: ctypes.c_float | None = None,
) -> tuple[SonareRoomMorphConfig, tuple[object, object]]:
    """Builds the morph config shared by the mono and stereo entry points.

    Returns the config with the band-array owners its pointers borrow from; the
    caller keeps them alive across the C call.
    """
    bands_ptr, bands_count, bands_owner = _band_array_args(
        band_absorption, arg_name="band_absorption"
    )
    scatter_ptr, scatter_count, scatter_owner = _band_array_args(
        band_scattering, arg_name="band_scattering"
    )
    config = SonareRoomMorphConfig()
    _fill_room_config(
        config,
        source=source,
        listener=listener,
        band_ptr=bands_ptr,
        band_count=bands_count,
        scatter_ptr=scatter_ptr,
        scatter_count=scatter_count,
        material_preset=material_preset,
        prefer_eyring=prefer_eyring,
        air_absorption_enabled=air_absorption_enabled,
        seed=seed,
        length_m=length_m,
        width_m=width_m,
        height_m=height_m,
        absorption=absorption,
        source_tail_suppression=source_tail_suppression,
        wet=wet,
        max_sec=max_sec,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        ism_order=ism_order,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
        receiver_spacing_m=receiver_spacing_m,
    )
    return config, (bands_owner, scatter_owner)


@_entry("room_morph")
@_guard_buffer("samples")
def room_morph(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    source: tuple[float, float, float] | None = None,
    listener: tuple[float, float, float] | None = None,
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int | None = None,
    max_sec: float | None = None,
    mixing_time_ms: float | None = None,
    crossfade_ms: float | None = None,
    air_absorption_enabled: bool = True,
    air_temperature_c: float | None = None,
    air_humidity_percent: float | None = None,
) -> RoomMorphResult:
    """Morph a recording's reverberation toward a target room (creative FX).

    Returns the morphed mono samples (the input length plus the target room's
    reverb tail) together with the target-RIR synthesis warnings, the way
    :func:`synthesize_rir` returns the RIR with its own. This is not
    dereverberation: the source reverb is only gently suppressed before the
    target room is added.

    Args:
        source, listener: (x, y, z) positions inside the target room, in
            metres; None = the default placement scaled to the target room.
        band_absorption: Optional per-octave-band target-wall absorption; when
            given it overrides ``absorption`` unless ``material_preset`` is set.
        band_scattering: Optional per-octave-band target-wall scattering;
            missing bands read as 0. Independent of ``band_absorption`` and
            ``material_preset`` -- it applies to whichever material the
            absorption precedence selected.
        material_preset: Named target-wall material preset (``"none"``; see
            :func:`synthesize_rir`). A non-zero preset wins over the bands/scalar.
        prefer_eyring: Use the Eyring statistical late-tail model for the target
            room (default); False selects Sabine.
        mixing_time_ms: Early/late crossover in ms; None derives it (~sqrt(V) ms).
        crossfade_ms: Equal-power crossfade width around the mixing time in ms;
            None = 5 ms, and 0 is a hard switch.
        air_absorption_enabled: Add the ISO 9613-1 atmospheric-absorption term
            to the target room's late tail (on by default; ``False`` gives the
            geometry-only RT60).
        seed: Deterministic late-tail seed; None = 1, and 0 is a seed.
        air_temperature_c: Air temperature in degrees Celsius; None = the ISO
            reference 20 degC, and 0 is 0 degC; see :func:`synthesize_rir`.
        air_humidity_percent: Relative humidity in percent; None = the ISO
            reference 50 %, and 0 is 0 %. An implausible climate raises here rather than
            reporting a diagnostic, because the morph validates its config.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config, _owners = _room_morph_config(
        length_m,
        width_m,
        height_m,
        source=source,
        listener=listener,
        absorption=absorption,
        band_absorption=band_absorption,
        band_scattering=band_scattering,
        material_preset=material_preset,
        source_tail_suppression=source_tail_suppression,
        wet=wet,
        ism_order=ism_order,
        prefer_eyring=prefer_eyring,
        seed=seed,
        max_sec=max_sec,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        air_absorption_enabled=air_absorption_enabled,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
    )
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_room_morph(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc, flattened="config")
    try:
        # Read before any later C ABI call can overwrite the thread-local list,
        # exactly as synthesize_rir does.
        diagnostics = _read_diagnostics()
        return RoomMorphResult(
            audio=_from_c_float_array(out, out_length.value),
            sample_rate=sample_rate,
            diagnostics=diagnostics,
        )
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


@_entry("room_morph_stereo")
@_guard_buffer("left", "right")
def room_morph_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    receiver_spacing_m: float | None = None,
    source: tuple[float, float, float] | None = None,
    listener: tuple[float, float, float] | None = None,
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int | None = None,
    max_sec: float | None = None,
    mixing_time_ms: float | None = None,
    crossfade_ms: float | None = None,
    air_absorption_enabled: bool = True,
    air_temperature_c: float | None = None,
    air_humidity_percent: float | None = None,
) -> RoomMorphStereoResult:
    """Morph a stereo recording toward a target room heard by two receivers.

    Like :func:`room_morph`, but the target room is heard by two omnidirectional
    receivers placed ``receiver_spacing_m`` apart about ``listener``, so the two
    channels carry different reverberation rather than one centred tail. The
    source-reverb suppression gain is shared across the channels so the image
    does not move. Returns the left and right channels (each the input length
    plus the target room's tail) with the target-RIR synthesis warnings.

    Args:
        left: Left channel samples.
        right: Right channel samples, same length as ``left``.
        receiver_spacing_m: Receiver spacing in metres, in (0, 4]; None fits it to the
            room: 0.5 where that pair fits around ``listener``, else the largest that fits.
            Both receivers must lie inside the room, so a ``listener`` closer to
            a wall than half the spacing raises :class:`SonareError`.

    The remaining keywords are those of :func:`room_morph`.
    """
    lib = _get_lib()
    spacing = (
        None
        if receiver_spacing_m is None
        else _domain_float(receiver_spacing_m, "receiver_spacing_m")
    )
    # The C struct reads NaN as not given; a given NaN is refused as the infinity it also is not.
    if spacing is not None and math.isnan(spacing.value):
        spacing = ctypes.c_float(math.inf)
    c_left, length = _to_c_float_array(left)
    c_right, right_length = _to_c_float_array(right)
    if right_length != length:
        raise _refuse("left and right must have the same length")
    config, _owners = _room_morph_config(
        length_m,
        width_m,
        height_m,
        source=source,
        listener=listener,
        absorption=absorption,
        band_absorption=band_absorption,
        band_scattering=band_scattering,
        material_preset=material_preset,
        source_tail_suppression=source_tail_suppression,
        wet=wet,
        ism_order=ism_order,
        prefer_eyring=prefer_eyring,
        seed=seed,
        max_sec=max_sec,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        air_absorption_enabled=air_absorption_enabled,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
        receiver_spacing_m=spacing,
    )
    out_left = ctypes.POINTER(ctypes.c_float)()
    out_right = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_room_morph_stereo(
        c_left,
        c_right,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out_left),
        ctypes.byref(out_right),
        ctypes.byref(out_length),
    )
    _check(rc, flattened="config")
    try:
        # Read before any later C ABI call can overwrite the thread-local list.
        diagnostics = _read_diagnostics()
        return RoomMorphStereoResult(
            left=_from_c_float_array(out_left, out_length.value),
            right=_from_c_float_array(out_right, out_length.value),
            sample_rate=sample_rate,
            diagnostics=diagnostics,
        )
    finally:
        if out_left:
            lib.sonare_free_floats(out_left)
        if out_right:
            lib.sonare_free_floats(out_right)
