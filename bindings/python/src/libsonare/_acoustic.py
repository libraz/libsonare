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

from ._errors import ErrorCode, SonareError, SonareValueError, _not_supported
from ._runtime import (
    SonareRirSynthConfig,
    SonareRirSynthResult,
    SonareRoomEstimate,
    SonareRoomEstimateConfig,
    SonareRoomMorphConfig,
    SonareRoomMorphStereoResult,
    _check,
    _float_array_result,
    _get_lib,
    _guard_buffer,
    _optional_float_array_result,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
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

# SONARE_REVERB_MODEL_* selectors (sonare_c_acoustic.h). DEFAULT (0) resolves to
# the library default (Eyring); only SABINE selects Sabine explicitly.
_REVERB_MODEL_DEFAULT = 0
_REVERB_MODEL_SABINE = 1
_REVERB_MODEL_EYRING = 2

# SonareDiagnosticSeverity ordinals (sonare_c_types_enums.h), mapped to the
# strings Node and WASM put on RirDiagnostic.severity.
_DIAGNOSTIC_SEVERITIES = ("info", "warning", "error")


def _read_diagnostics() -> list[RirDiagnostic]:
    """Reads the structured diagnostic channel the last C call published.

    Must run before any later C ABI call can replace the thread-local list, the
    same contract the error and warning strings carry.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_last_diagnostic_count"):
        return []
    out: list[RirDiagnostic] = []
    for position in range(lib.sonare_last_diagnostic_count()):
        index = _to_c_size_t(position, "index")
        code = lib.sonare_last_diagnostic_code(index) or b""
        message = lib.sonare_last_diagnostic_message(index) or b""
        severity = lib.sonare_last_diagnostic_severity(index)
        out.append(
            RirDiagnostic(
                code=code.decode("utf-8"),
                message=message.decode("utf-8"),
                severity=_DIAGNOSTIC_SEVERITIES[severity]
                if 0 <= severity < len(_DIAGNOSTIC_SEVERITIES)
                else "info",
            )
        )
    return out


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
        raise TypeError(f"{argument} must be one of {valid} (a name, not a number), got {value!r}")
    if value not in valid:
        raise SonareValueError(f"{argument} must be one of {valid}, got {value!r}")
    return valid.index(value)


def _material_preset(value: MaterialPresetName) -> int:
    return _named_selector(value, "material_preset", "material_preset")


def _acoustic_mode(value: AcousticModeName) -> int:
    return _named_selector(value, "mode", "acoustic_mode")


def _late_model(prefer_eyring: bool) -> int:
    return _REVERB_MODEL_EYRING if prefer_eyring else _REVERB_MODEL_SABINE


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


def _checked_seed(seed: int) -> int:
    """Refuse a seed the C field cannot express.

    The C ``seed`` is a ``uint32``, and Node and WASM both reject a value outside
    its range rather than folding one in. Folding here instead made ``-1`` and the
    library default indistinguishable under a successful call, so two different
    requests returned the same tail with nothing to tell them apart.
    """
    if not 0 <= seed <= 0xFFFFFFFF:
        raise SonareValueError("seed must be within [0, 4294967295]")
    return seed


def synthesize_rir(
    length_m: float = 7.0,
    width_m: float = 5.0,
    height_m: float = 3.0,
    *,
    source: tuple[float, float, float] = (1.0, 1.0, 1.2),
    listener: tuple[float, float, float] = (5.0, 4.0, 1.7),
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    sample_rate: int = 48000,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int = 1,
    max_seconds: float = 0.0,
    mixing_time_ms: float = 0.0,
    crossfade_ms: float = 0.0,
    air_absorption_enabled: bool = False,
    air_temperature_c: float = 0.0,
    air_humidity_percent: float = 0.0,
) -> RirResult:
    """Synthesize a room impulse response from shoebox geometry.

    Args:
        length_m, width_m, height_m: Room dimensions in metres (default
            7 x 5 x 3, matching the other bindings).
        source, listener: (x, y, z) positions inside the room, in metres.
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
        seed: Deterministic late-tail seed, in [0, 4294967295]; 0 keeps the
            library default, as it does on every other surface.
        max_seconds: Hard RIR length cap (0 = natural length).
        mixing_time_ms: Early/late crossover in ms (0 = auto, ~sqrt(V) ms).
        crossfade_ms: Equal-power crossfade width around the mixing time in ms
            (0 = library default).
        air_absorption_enabled: Add the ISO 9613-1 atmospheric-absorption term
            to the late tail's per-band RT60. Off by default so the RIR is
            unchanged; it mainly shortens the high bands of a large room.
        air_temperature_c: Air temperature in degrees Celsius (0 = the ISO
            reference climate's 20 degC). A literal 0 degC is not
            distinguishable from unset here; use 0.01 for a freezing room.
        air_humidity_percent: Relative humidity in percent (0 = the ISO
            reference climate's 50 %). Both climate values are read only while
            ``air_absorption_enabled`` is set, and an implausible pair raises
            ``SonareError`` (``acoustic.invalid_air_absorption``).

    Returns:
        A :class:`RirResult`; its ``diagnostics`` are warnings.

    Raises:
        SonareError: The geometry, placement or timing is unusable (for example
            a source outside the room); the message leads with the diagnostic
            code, such as ``acoustic.source_outside_room``.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_synthesize_rir"):
        raise _not_supported("libsonare was built without acoustic-simulation support")
    bands_ptr, bands_count, _bands_owner = _band_array_args(
        band_absorption, arg_name="band_absorption"
    )
    scatter_ptr, scatter_count, _scatter_owner = _band_array_args(
        band_scattering, arg_name="band_scattering"
    )
    config = SonareRirSynthConfig(
        length_m=length_m,
        width_m=width_m,
        height_m=height_m,
        source_x=source[0],
        source_y=source[1],
        source_z=source[2],
        listener_x=listener[0],
        listener_y=listener[1],
        listener_z=listener[2],
        absorption=absorption,
        max_seconds=max_seconds,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        ism_order=ism_order,
        late_model=_late_model(prefer_eyring),
        seed=_checked_seed(seed),
        air_absorption_enabled=1 if air_absorption_enabled else 0,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
        absorption_bands=bands_ptr,
        absorption_band_count=bands_count,
        scattering_bands=scatter_ptr,
        scattering_band_count=scatter_count,
        material_preset=_material_preset(material_preset),
    )
    out = SonareRirSynthResult()
    rc = lib.sonare_synthesize_rir(
        ctypes.byref(config),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        if out.has_error:
            detail = lib.sonare_last_error_message()
            raise SonareError(
                ErrorCode.INVALID_PARAMETER,
                detail.decode("utf-8") if detail else "invalid room geometry",
            )
        # Non-fatal diagnostics are recorded on success too -- a max_seconds clamp
        # that cut the tail, or a request degraded to early reflections only.
        # Read before any later C ABI call can overwrite the thread-local list.
        return RirResult(
            rir=_float_array_result(out.rir, out.length),
            sample_rate=int(out.sample_rate),
            diagnostics=_read_diagnostics(),
        )
    finally:
        lib.sonare_free_rir_synth_result(ctypes.byref(out))


@_guard_buffer("samples")
def estimate_room(
    samples: Sequence[float] | list[float],
    sample_rate: int = 48000,
    *,
    aspect_hint_lw: float = 1.0,
    aspect_hint_lh: float = 1.0,
    reference_absorption: float = 0.15,
    prefer_eyring: bool = True,
    n_octave_bands: int = 0,
    mode: AcousticModeName = "auto",
    min_decay_db: float = 0.0,
    noise_floor_margin_db: float = 0.0,
) -> RoomEstimate:
    """Estimate an equivalent room from a recording (or impulse response).

    The volume scale is anchored by ``reference_absorption`` (the inverse
    problem is rank-deficient by one) and the shape by the aspect hints; the
    returned ``confidence`` reports how well the data support the estimate.

    Args:
        reference_absorption: Mean-absorption prior anchoring the volume scale
            (0 = library default, 0.15). Clamped into ``[0.01, 0.99]`` rather
            than refused: a value outside that range still returns a successful
            estimate, computed from the clamped prior. The reported volume
            scales with the cube of the prior, so the substitution is worth
            three orders of magnitude at the low end.
        mode: Analyzer routing, by name -- ``"auto"`` (impulse-like inputs route
            to IR analysis), ``"blind"`` or ``"impulse_response"``. An unknown
            name raises ``SonareValueError`` and a number ``TypeError``.
        min_decay_db: Analyzer decay-fit span in dB (0 = library default).
        noise_floor_margin_db: Analyzer noise-floor margin in dB (0 = library
            default).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_estimate_room"):
        raise _not_supported("libsonare was built without acoustic-simulation support")
    c_array, length = _to_c_float_array(samples)
    config = SonareRoomEstimateConfig(
        aspect_hint_lw=aspect_hint_lw,
        aspect_hint_lh=aspect_hint_lh,
        reference_absorption=reference_absorption,
        min_decay_db=min_decay_db,
        noise_floor_margin_db=noise_floor_margin_db,
        prefer_eyring=1 if prefer_eyring else 0,
        n_octave_bands=n_octave_bands,
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
    _check(rc)
    try:
        count = out.band_count
        return RoomEstimate(
            volume=float(out.volume),
            length_m=float(out.length_m),
            width_m=float(out.width_m),
            height_m=float(out.height_m),
            drr_db=float(out.drr_db),
            confidence=float(out.confidence),
            band_absorption=_optional_float_array_result(out.absorption_bands, count),
            rt60_bands=_optional_float_array_result(out.rt60_bands, count),
        )
    finally:
        lib.sonare_free_room_estimate(ctypes.byref(out))


def _placement(value: Sequence[float], argument: str) -> tuple[float, float, float]:
    if len(value) != 3:
        raise SonareValueError(f"{argument} must be an (x, y, z) triple")
    return (float(value[0]), float(value[1]), float(value[2]))


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

        geometry = libsonare.room_geometry_from_estimate(estimate, source=(1, 1, 1.2),
                                                         listener=(3, 2, 1.7))
        rir = libsonare.synthesize_rir(**geometry)

    ``source`` and ``listener`` are (x, y, z) positions in metres; an estimate
    carries no placement, so an omitted one is left out of the result and
    :func:`synthesize_rir` applies its own default, which may fall outside a
    small estimated room. Absorption bands that did not converge are left out
    too, so the scalar ``absorption`` applies.

    Raises:
        SonareValueError: The estimate has no measurable dimensions (NaN).
    """
    lib = _get_lib()
    bands_ptr, bands_count, _bands_owner = _band_array_args(
        estimate.band_absorption, arg_name="band_absorption"
    )
    c_estimate = SonareRoomEstimate(
        volume=estimate.volume,
        length_m=estimate.length_m,
        width_m=estimate.width_m,
        height_m=estimate.height_m,
        drr_db=estimate.drr_db,
        confidence=estimate.confidence,
        absorption_bands=bands_ptr,
        rt60_bands=None,
        band_count=bands_count,
    )
    config = SonareRirSynthConfig()
    _check(lib.sonare_room_geometry_from_estimate(ctypes.byref(c_estimate), ctypes.byref(config)))
    geometry: RoomGeometry = {
        "length_m": float(config.length_m),
        "width_m": float(config.width_m),
        "height_m": float(config.height_m),
    }
    if config.absorption_band_count > 0:
        geometry["band_absorption"] = _optional_float_array_result(
            config.absorption_bands, config.absorption_band_count
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
    source: tuple[float, float, float],
    listener: tuple[float, float, float],
    absorption: float,
    band_absorption: Sequence[float] | None,
    band_scattering: Sequence[float] | None,
    material_preset: MaterialPresetName,
    source_tail_suppression: float,
    wet: float,
    ism_order: int,
    prefer_eyring: bool,
    seed: int,
    max_seconds: float,
    mixing_time_ms: float,
    crossfade_ms: float,
    air_absorption_enabled: bool,
    air_temperature_c: float,
    air_humidity_percent: float,
    receiver_spacing_m: float = 0.0,
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
    config = SonareRoomMorphConfig(
        length_m=length_m,
        width_m=width_m,
        height_m=height_m,
        source_x=source[0],
        source_y=source[1],
        source_z=source[2],
        listener_x=listener[0],
        listener_y=listener[1],
        listener_z=listener[2],
        absorption=absorption,
        source_tail_suppression=source_tail_suppression,
        wet=wet,
        max_seconds=max_seconds,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        ism_order=ism_order,
        late_model=_late_model(prefer_eyring),
        seed=_checked_seed(seed),
        air_absorption_enabled=1 if air_absorption_enabled else 0,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
        absorption_bands=bands_ptr,
        absorption_band_count=bands_count,
        scattering_bands=scatter_ptr,
        scattering_band_count=scatter_count,
        material_preset=_material_preset(material_preset),
        receiver_spacing_m=receiver_spacing_m,
    )
    return config, (bands_owner, scatter_owner)


@_guard_buffer("samples")
def room_morph(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    source: tuple[float, float, float] = (1.0, 1.0, 1.2),
    listener: tuple[float, float, float] = (5.0, 4.0, 1.7),
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int = 1,
    max_seconds: float = 0.0,
    mixing_time_ms: float = 0.0,
    crossfade_ms: float = 0.0,
    air_absorption_enabled: bool = False,
    air_temperature_c: float = 0.0,
    air_humidity_percent: float = 0.0,
) -> RoomMorphResult:
    """Morph a recording's reverberation toward a target room (creative FX).

    Returns the morphed mono samples (the input length plus the target room's
    reverb tail) together with the target-RIR synthesis warnings, the way
    :func:`synthesize_rir` returns the RIR with its own. This is not
    dereverberation: the source reverb is only gently suppressed before the
    target room is added.

    Args:
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
        mixing_time_ms: Early/late crossover in ms (0 = auto, ~sqrt(V) ms).
        crossfade_ms: Equal-power crossfade width around the mixing time in ms
            (0 = library default).
        air_absorption_enabled: Add the ISO 9613-1 atmospheric-absorption term
            to the target room's late tail (off by default).
        air_temperature_c: Air temperature in degrees Celsius (0 = the ISO
            reference 20 degC); see :func:`synthesize_rir`.
        air_humidity_percent: Relative humidity in percent (0 = the ISO
            reference 50 %). An implausible climate raises here rather than
            reporting a diagnostic, because the morph validates its config.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_room_morph"):
        raise _not_supported("libsonare was built without acoustic-simulation support")
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
        max_seconds=max_seconds,
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
    _check(rc)
    try:
        # Read before any later C ABI call can overwrite the thread-local list,
        # exactly as synthesize_rir does.
        diagnostics = _read_diagnostics()
        return RoomMorphResult(
            audio=_float_array_result(out, out_length.value),
            sample_rate=sample_rate,
            diagnostics=diagnostics,
        )
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


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
    source: tuple[float, float, float] = (1.0, 1.0, 1.2),
    listener: tuple[float, float, float] = (5.0, 4.0, 1.7),
    absorption: float = 0.2,
    band_absorption: Sequence[float] | None = None,
    band_scattering: Sequence[float] | None = None,
    material_preset: MaterialPresetName = "none",
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    prefer_eyring: bool = True,
    seed: int = 1,
    max_seconds: float = 0.0,
    mixing_time_ms: float = 0.0,
    crossfade_ms: float = 0.0,
    air_absorption_enabled: bool = False,
    air_temperature_c: float = 0.0,
    air_humidity_percent: float = 0.0,
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
        receiver_spacing_m: Receiver spacing in metres, in (0, 4]; None = 0.5.
            Both receivers must lie inside the room, so a ``listener`` closer to
            a wall than half the spacing raises :class:`SonareError`.

    The remaining keywords are those of :func:`room_morph`.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_room_morph_stereo"):
        raise _not_supported("libsonare was built without acoustic-simulation support")
    if receiver_spacing_m is None:
        spacing = 0.0
    else:
        spacing = float(receiver_spacing_m)
        if not math.isfinite(spacing) or not 0.0 < spacing <= 4.0:
            raise SonareValueError(
                f"receiver_spacing_m must be finite and in (0, 4], got {receiver_spacing_m!r}"
            )
    c_left, length = _to_c_float_array(left)
    c_right, right_length = _to_c_float_array(right)
    if right_length != length:
        raise SonareValueError(
            f"left and right must have the same length, got {length} and {right_length}"
        )
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
        max_seconds=max_seconds,
        mixing_time_ms=mixing_time_ms,
        crossfade_ms=crossfade_ms,
        air_absorption_enabled=air_absorption_enabled,
        air_temperature_c=air_temperature_c,
        air_humidity_percent=air_humidity_percent,
        receiver_spacing_m=spacing,
    )
    out = SonareRoomMorphStereoResult()
    rc = lib.sonare_room_morph_stereo(
        c_left,
        c_right,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        # Read before any later C ABI call can overwrite the thread-local list.
        diagnostics = _read_diagnostics()
        return RoomMorphStereoResult(
            left=_float_array_result(out.left, out.length),
            right=_float_array_result(out.right, out.length),
            sample_rate=sample_rate,
            diagnostics=diagnostics,
        )
    finally:
        lib.sonare_free_room_morph_stereo_result(ctypes.byref(out))
