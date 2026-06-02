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
from collections.abc import Sequence

from ._runtime import (
    SonareRirSynthConfig,
    SonareRirSynthResult,
    SonareRoomEstimate,
    SonareRoomEstimateConfig,
    SonareRoomMorphConfig,
    _check,
    _float_array_result,
    _get_lib,
    _optional_float_array_result,
    _to_c_float_array,
)
from .types import RirResult, RoomEstimate


def synthesize_rir(
    length_m: float,
    width_m: float,
    height_m: float,
    *,
    source: tuple[float, float, float] = (1.0, 1.0, 1.2),
    listener: tuple[float, float, float] = (5.0, 4.0, 1.7),
    absorption: float = 0.2,
    sample_rate: int = 48000,
    ism_order: int = 3,
    seed: int = 1,
    max_seconds: float = 0.0,
) -> RirResult:
    """Synthesize a room impulse response from shoebox geometry.

    Args:
        length_m, width_m, height_m: Room dimensions in metres.
        source, listener: (x, y, z) positions inside the room, in metres.
        absorption: Uniform wall absorption, clamped to [0, 0.999].
        sample_rate: Output sample rate in Hz.
        ism_order: Image-source reflection order.
        seed: Deterministic late-tail seed.
        max_seconds: Hard RIR length cap (0 = natural length).

    Returns:
        A :class:`RirResult`; ``has_error`` is True when the geometry is invalid
        (the listener/source falls outside the room), in which case ``rir`` is
        empty.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_synthesize_rir"):
        raise RuntimeError("libsonare was built without acoustic-simulation support")
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
        ism_order=ism_order,
        seed=seed,
    )
    out = SonareRirSynthResult()
    rc = lib.sonare_synthesize_rir(
        ctypes.byref(config),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return RirResult(
            rir=_float_array_result(out.rir, out.length),
            sample_rate=int(out.sample_rate),
            has_error=bool(out.has_error),
        )
    finally:
        lib.sonare_free_rir_synth_result(ctypes.byref(out))


def estimate_room(
    samples: Sequence[float] | list[float],
    sample_rate: int = 48000,
    *,
    aspect_hint_lw: float = 1.0,
    aspect_hint_lh: float = 1.0,
    reference_absorption: float = 0.15,
    prefer_eyring: bool = True,
    n_octave_bands: int = 0,
) -> RoomEstimate:
    """Estimate an equivalent room from a recording (or impulse response).

    The volume scale is anchored by ``reference_absorption`` (the inverse
    problem is rank-deficient by one) and the shape by the aspect hints; the
    returned ``confidence`` reports how well the data support the estimate.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_estimate_room"):
        raise RuntimeError("libsonare was built without acoustic-simulation support")
    c_array, length = _to_c_float_array(samples)
    config = SonareRoomEstimateConfig(
        aspect_hint_lw=aspect_hint_lw,
        aspect_hint_lh=aspect_hint_lh,
        reference_absorption=reference_absorption,
        prefer_eyring=1 if prefer_eyring else 0,
        n_octave_bands=n_octave_bands,
    )
    out = SonareRoomEstimate()
    rc = lib.sonare_estimate_room(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = out.band_count
        return RoomEstimate(
            volume=float(out.volume),
            length=float(out.length_m),
            width=float(out.width_m),
            height=float(out.height_m),
            drr_db=float(out.drr_db),
            confidence=float(out.confidence),
            absorption_bands=_optional_float_array_result(out.absorption_bands, count),
            rt60_bands=_optional_float_array_result(out.rt60_bands, count),
        )
    finally:
        lib.sonare_free_room_estimate(ctypes.byref(out))


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
    source_tail_suppression: float = 0.5,
    wet: float = 0.5,
    ism_order: int = 3,
    seed: int = 1,
    max_seconds: float = 0.0,
) -> list[float]:
    """Morph a recording's reverberation toward a target room (creative FX).

    Returns the morphed mono samples (the input length plus the target room's
    reverb tail). This is not dereverberation: the source reverb is only gently
    suppressed before the target room is added.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_room_morph"):
        raise RuntimeError("libsonare was built without acoustic-simulation support")
    c_array, length = _to_c_float_array(samples)
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
        ism_order=ism_order,
        seed=seed,
    )
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_room_morph(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)
