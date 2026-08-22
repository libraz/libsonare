"""Offline mixing assistant: analyse tracks and suggest a mixer scene.

The assistant only ever returns parameters. It does not process audio and it
does not apply anything: feeding a suggestion to :class:`~libsonare.Mixer` is
the caller's separate, explicit step through
:meth:`~libsonare.Mixer.from_scene_json`, and there is deliberately no entry
point that performs both halves at once.

Everything here is offline work. It allocates, runs an STFT per track and
evaluates every track pair, so it must not run on a realtime thread.
"""

from __future__ import annotations

import ctypes
import json
from collections.abc import Sequence
from typing import Any, NamedTuple

import numpy as np

from ._mastering_offline import _mastering_params
from ._runtime import SonareValueError, _check, _get_lib, _to_c_float_array, _validate_samples

# Keyword argument -> C-ABI param key. The C side accepts both spellings, but
# the camelCase key is the cross-binding canonical one, so that is what travels.
_PARAM_KEYS = {
    "target_track_lufs": "targetTrackLufs",
    "suggestion_strength": "suggestionStrength",
    "eq_max_cut_db": "eqMaxCutDb",
    "mix_bus_headroom_dbtp": "mixBusHeadroomDbtp",
    "enable_structure": "enableStructure",
    "enable_gain": "enableGain",
    "enable_balance": "enableBalance",
    "enable_eq": "enableEq",
    "enable_dynamics": "enableDynamics",
    "enable_image": "enableImage",
    "enable_high_pass": "enableHighPass",
    "n_fft": "nFft",
    "hop_length": "hopLength",
}


class MixTrackInput(NamedTuple):
    """One track handed to the mixing assistant.

    Attributes:
        track_id: Strip identifier, non-empty and unique within the call. It is
            the id the suggested scene addresses this track by.
        left: Left channel, or the whole signal for a mono track.
        right: Right channel, or ``None`` for a mono track. Tracks may mix mono
            and stereo freely within one call.
        name: Optional display name used as a classification hint (a track
            called ``"kick"`` is more readily read as a kick drum). For a class
            the classifier can measure it only adjusts confidence and never
            selects the class on its own. For the four it cannot separate by
            measurement — ``keys``, ``strings``, ``backing`` and ``fx`` — the
            name is the only thing that can supply the class at all, and only
            when the measurement produced no answer.

    Tracks may differ in length: truncating every track to the shortest would
    delete a part that only enters late in the song.
    """

    track_id: str
    left: Sequence[float] | np.ndarray
    right: Sequence[float] | np.ndarray | None = None
    name: str | None = None


class _TrackArrays(NamedTuple):
    """Marshalled track arrays, held together so nothing is collected mid-call."""

    left_ptrs: Any
    right_ptrs: Any
    ids: Any
    names: Any
    lengths: Any
    count: int
    pins: list[Any]


def _build_track_arrays(fn_name: str, tracks: Sequence[MixTrackInput]) -> _TrackArrays:
    """Validate ``tracks`` and marshal them into the parallel C arrays.

    Every allocated buffer is pinned in the returned tuple, so the caller keeps
    one reference alive for the duration of the C call.
    """
    count = len(tracks)
    if count == 0:
        return _TrackArrays(None, None, None, None, None, 0, [])

    seen: set[str] = set()
    left_arrays: list[Any] = []
    right_arrays: list[Any] = []
    lengths: list[int] = []
    id_buffers: list[bytes] = []
    name_buffers: list[bytes | None] = []
    any_right = False

    for index, track in enumerate(tracks):
        track_id = track.track_id
        if not isinstance(track_id, str) or not track_id:
            raise SonareValueError(
                f"{fn_name}: tracks[{index}].track_id must be a non-empty string"
            )
        if track_id in seen:
            raise SonareValueError(f"{fn_name}: duplicate track_id {track_id!r}")
        seen.add(track_id)

        # A NaN or Inf sample would propagate silently through the measurements
        # into a plausible-looking suggestion, so it is rejected here with the
        # offending index rather than at the C ABI as a bare invalid-parameter.
        left = _validate_samples(
            fn_name, track.left, arg_name=f"tracks[{index}].left", allow_empty=True
        )
        left_array, left_length = _to_c_float_array(left)
        if track.right is None:
            right_array = None
            right_length = left_length
        else:
            right = _validate_samples(
                fn_name, track.right, arg_name=f"tracks[{index}].right", allow_empty=True
            )
            right_array, right_length = _to_c_float_array(right)
            any_right = True
        if right_length != left_length:
            raise SonareValueError(
                f"{fn_name}: tracks[{index}] left and right channel lengths must match"
            )

        left_arrays.append(left_array)
        right_arrays.append(right_array)
        lengths.append(left_length)
        id_buffers.append(track_id.encode("utf-8"))
        name_buffers.append(track.name.encode("utf-8") if track.name is not None else None)

    float_ptr = ctypes.POINTER(ctypes.c_float)
    left_ptrs = (float_ptr * count)(*[ctypes.cast(arr, float_ptr) for arr in left_arrays])
    # NULL for the whole array when every track is mono, and NULL per entry for
    # a mono track inside a mixed set -- both spellings the C ABI accepts.
    right_ptrs = (
        (float_ptr * count)(
            *[None if arr is None else ctypes.cast(arr, float_ptr) for arr in right_arrays]
        )
        if any_right
        else None
    )
    id_array = (ctypes.c_char_p * count)(*id_buffers)
    name_array = (
        (ctypes.c_char_p * count)(*name_buffers)
        if any(buffer is not None for buffer in name_buffers)
        else None
    )
    length_array = (ctypes.c_size_t * count)(*lengths)

    pins: list[Any] = [left_arrays, right_arrays, id_buffers, name_buffers]
    return _TrackArrays(left_ptrs, right_ptrs, id_array, name_array, length_array, count, pins)


def _assistant_params(
    fn_name: str, options: dict[str, float | int | bool | None]
) -> tuple[Any, int]:
    """Marshal the supplied options, leaving unset ones to the core defaults.

    An option left at ``None`` is not placed in the param array at all, so the
    value the C++ config declares stays in force instead of being overwritten
    with a default duplicated on this side.
    """
    params: dict[str, float | int | bool] = {}
    for name, value in options.items():
        if value is None:
            continue
        if isinstance(value, bool):
            params[_PARAM_KEYS[name]] = value
            continue
        number = float(value)
        if not np.isfinite(number):
            raise SonareValueError(f"{fn_name}: {name} must be a finite number")
        params[_PARAM_KEYS[name]] = number
    return _mastering_params(params)


def _suggest(
    symbol: str,
    fn_name: str,
    tracks: Sequence[MixTrackInput],
    sample_rate: int,
    options: dict[str, float | int | bool | None],
) -> str:
    """Run one of the two suggest entry points and return its JSON string."""
    # The symbol is exported unconditionally -- a build without the assistant
    # keeps it and answers SONARE_ERROR_NOT_SUPPORTED -- so there is nothing to
    # probe for here. _check turns that answer into the same error class every
    # other unsupported call raises.
    lib = _get_lib()
    if int(sample_rate) <= 0:
        raise SonareValueError(f"{fn_name}: sample_rate must be positive")
    arrays = _build_track_arrays(fn_name, tracks)
    param_array, param_count = _assistant_params(fn_name, options)
    json_ptr = ctypes.c_char_p()
    rc = getattr(lib, symbol)(
        arrays.left_ptrs,
        arrays.right_ptrs,
        arrays.ids,
        arrays.names,
        arrays.lengths,
        ctypes.c_size_t(arrays.count),
        ctypes.c_int(int(sample_rate)),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def suggest_mix_scene(
    tracks: Sequence[MixTrackInput],
    *,
    sample_rate: int,
    target_track_lufs: float | None = None,
    suggestion_strength: float | None = None,
    eq_max_cut_db: float | None = None,
    mix_bus_headroom_dbtp: float | None = None,
    enable_structure: bool | None = None,
    enable_gain: bool | None = None,
    enable_balance: bool | None = None,
    enable_eq: bool | None = None,
    enable_dynamics: bool | None = None,
    enable_image: bool | None = None,
    enable_high_pass: bool | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
) -> dict[str, Any]:
    """Analyse ``tracks`` and return a suggested scene with the reasoning behind it.

    Args:
        tracks: Tracks to analyse, each a :class:`MixTrackInput`. Lengths may
            differ between tracks. An empty sequence yields an empty suggestion.
        sample_rate: Shared sample rate of every track, in Hz.
        target_track_lufs: Absolute integrated-loudness target each track is
            staged towards, in LUFS.
        suggestion_strength: Overall strength in ``[0, 1]``, scaling every
            level-like decision: trims, fader offsets, send levels, EQ cut
            depths, compression ratios and ranges, and how far a track is
            spread from the centre. ``0`` is not an empty suggestion — it is
            every one of those taken and set to zero, plus the decisions that
            are not levels and so do not scale: the bus topology and routing,
            and the physical corrections for a measured cancellation (polarity,
            alignment delay, low-end mono fold). To suggest nothing, switch the
            domains off instead; that also skips the work.
        eq_max_cut_db: Largest cut a single suggested EQ band may apply, in dB.
        mix_bus_headroom_dbtp: Headroom the summed mix is left with on the
            master bus, in dBTP.
        enable_structure: Suggest bus structure and routing.
        enable_gain: Suggest per-track gain staging.
        enable_balance: Suggest fader balance between tracks.
        enable_eq: Suggest corrective EQ.
        enable_dynamics: Suggest dynamics processing.
        enable_image: Suggest stereo placement and width.
        enable_high_pass: Suggest a high-pass filter on tracks carrying residue
            below their register. Off by default: a survey of mixing best
            practices found the rule that every track without low-frequency
            content should be high-passed to be seldom used in studio mixing and
            unsupported by subjective testing. Switched on, the filter is
            proposed from the track's measured low-frequency content rather than
            from its source class, so a part playing below its class's usual
            register keeps what it plays.
        n_fft: Analysis FFT size.
        hop_length: Analysis hop length in samples.

    Every option left at ``None`` keeps the library's own default; a disabled
    domain is not evaluated at all rather than evaluated and discarded, and the
    cross-track measurement it reads is skipped with it. The per-track profiling
    behind ``tracks`` is not a domain's cost and runs either way, so it is the
    floor that switching every domain off leaves.

    Returns:
        The parsed result document: ``scene`` (in the schema
        :meth:`~libsonare.Mixer.from_scene_json` reads), ``tracks`` and ``mix``
        with the measurements behind the suggestion, and ``explanation``, a list
        of human-readable lines in application order.
    """
    document = _suggest(
        "sonare_mixing_assistant_suggest",
        "suggest_mix_scene",
        tracks,
        sample_rate,
        {
            "target_track_lufs": target_track_lufs,
            "suggestion_strength": suggestion_strength,
            "eq_max_cut_db": eq_max_cut_db,
            "mix_bus_headroom_dbtp": mix_bus_headroom_dbtp,
            "enable_structure": enable_structure,
            "enable_gain": enable_gain,
            "enable_balance": enable_balance,
            "enable_eq": enable_eq,
            "enable_dynamics": enable_dynamics,
            "enable_image": enable_image,
            "enable_high_pass": enable_high_pass,
            "n_fft": n_fft,
            "hop_length": hop_length,
        },
    )
    return json.loads(document) if document else {}


def suggest_mix_scene_json(
    tracks: Sequence[MixTrackInput],
    *,
    sample_rate: int,
    target_track_lufs: float | None = None,
    suggestion_strength: float | None = None,
    eq_max_cut_db: float | None = None,
    mix_bus_headroom_dbtp: float | None = None,
    enable_structure: bool | None = None,
    enable_gain: bool | None = None,
    enable_balance: bool | None = None,
    enable_eq: bool | None = None,
    enable_dynamics: bool | None = None,
    enable_image: bool | None = None,
    enable_high_pass: bool | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
) -> str:
    """Suggest a scene for ``tracks`` and return only the scene, as JSON.

    Takes the same arguments as :func:`suggest_mix_scene` and reaches the same
    suggestion, but serialises only the scene, in the schema
    :meth:`~libsonare.Mixer.from_scene_json` reads. Use it when the suggestion
    is going straight to a mixer, so the scene need not be dug out of the fuller
    result document and re-serialised.
    """
    return _suggest(
        "sonare_mixing_assistant_suggest_scene_json",
        "suggest_mix_scene_json",
        tracks,
        sample_rate,
        {
            "target_track_lufs": target_track_lufs,
            "suggestion_strength": suggestion_strength,
            "eq_max_cut_db": eq_max_cut_db,
            "mix_bus_headroom_dbtp": mix_bus_headroom_dbtp,
            "enable_structure": enable_structure,
            "enable_gain": enable_gain,
            "enable_balance": enable_balance,
            "enable_eq": enable_eq,
            "enable_dynamics": enable_dynamics,
            "enable_image": enable_image,
            "enable_high_pass": enable_high_pass,
            "n_fft": n_fft,
            "hop_length": hop_length,
        },
    )


def mix_source_class_names() -> list[str]:
    """Return the source-class identifiers the assistant can report.

    Every name returned is a class some shipped entry point can actually put on
    a track: most come from the classifier's measured decision table, and
    ``keys``, ``strings``, ``backing`` and ``fx`` from a track's ``name``.

    Raises:
        RuntimeError: If the library was built without the mixing assistant.
            The entry point exists in such a build and answers with an empty
            list, which is the one answer that cannot be a real taxonomy.
    """
    lib = _get_lib()
    raw = lib.sonare_mixing_assistant_source_class_names()
    names = raw.decode("utf-8").splitlines() if raw else []
    if not names:
        raise RuntimeError("libsonare was built without mixing assistant support")
    return names


def mix_source_class_from_name(name: str) -> int:
    """Resolve a source-class identifier to its enum value, or ``-1`` if unknown."""
    lib = _get_lib()
    return int(lib.sonare_mixing_assistant_source_class_from_name(name.encode("utf-8")))
