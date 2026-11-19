"""Offline mixing assistant: analyse tracks and suggest a mixer scene.

The assistant only ever returns parameters. It does not process audio and it
does not apply anything: feeding a suggestion to :class:`~libsonare.Mixer` is
the caller's separate, explicit step through
``Mixer(scene=...)``, and there is deliberately no entry
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

from ._errors import _entry, _not_supported, _refuse, _refuse_type
from ._mastering_offline import _mastering_params
from ._narrowing import _narrow_double
from ._result_records import _fields
from ._runtime import (
    _C_INT_MAX,
    _as_float32_buffer,
    _check,
    _get_lib,
    _joined_names,
    _narrow_int,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _utf8_arg,
    _validate_c_int_field,
)
from ._types_mixing_assistant import (
    MixAssistantMixProfile,
    MixAssistantResult,
    MixAssistantTrackProfile,
    MixBandDominance,
    MixCrowdedBand,
    MixMonoRisk,
    MixTrackAlignment,
)

# Keyword argument -> C-ABI param key. The C side accepts both spellings, but
# the camelCase key is the cross-binding canonical one, so that is what travels.
_PARAM_KEYS = {
    "target_track_lufs": "targetTrackLufs",
    "suggestion_strength": "suggestionStrength",
    "eq_max_cut_db": "eqMaxCutDb",
    "mix_bus_headroom_dbtp": "mixBusHeadroomDbtp",
    "tempo_bpm": "tempoBpm",
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

# The options whose C++ field is an `int`. A param value travels as a double, so
# without this the two integral options are the only named integer arguments on
# this surface not narrowed before the call, and a fraction would reach the core
# to be refused there under the camelCase key rather than the one a caller typed.
_INTEGER_PARAMS = frozenset({"n_fft", "hop_length"})


class MixAssistantTrack(NamedTuple):
    """One track handed to the mixing assistant.

    Attributes:
        id: Strip identifier, non-empty and unique within the call. It is the
            id the suggested scene addresses this track by.
        channels: The track's planes as a list or tuple: one plane for a mono
            track, or two of equal length (left, right) for a stereo one.
            Tracks may mix mono and stereo freely within one call.
        name: Optional display name used as a classification hint. Naming the
            measured class raises its confidence. Naming another class switches
            to it when the measurement does not contradict it: a measurable
            class needs its own feature rule satisfied by the track, and one the
            classifier cannot measure needs the track not to have been measured
            as a drum. For the six it cannot separate by measurement —
            ``keys``, ``strings``, ``lead``, ``vocal``, ``backing`` and ``fx``
            — the name is the only thing that can supply the class, so an
            unnamed voice, pad or lead line comes back ``unknown``. A compound
            name states its last hint word (``"Lead Vox"`` is ``vocal``);
            hint words joined by anything else state none.
        sample_rate: Rate of this track, in Hz, or ``None`` for the call's
            ``sample_rate``. A track at another rate is resampled onto the
            call's rate before analysis.

    Tracks may differ in length: truncating every track to the shortest would
    delete a part that only enters late in the song.
    """

    id: str
    channels: Sequence[Sequence[float] | np.ndarray]
    name: str | None = None
    sample_rate: int | None = None


class _TrackArrays(NamedTuple):
    """Marshalled track arrays, held together so nothing is collected mid-call."""

    left_ptrs: Any
    right_ptrs: Any
    ids: Any
    names: Any
    lengths: Any
    sample_rates: Any
    count: int
    pins: list[Any]


def _build_track_arrays(tracks: Sequence[MixAssistantTrack], sample_rate: int) -> _TrackArrays:
    """Validate ``tracks`` and marshal them into the parallel C arrays.

    Every allocated buffer is pinned in the returned tuple, so the caller keeps
    one reference alive for the duration of the C call.
    """
    count = len(tracks)
    if count == 0:
        return _TrackArrays(None, None, None, None, None, None, 0, [])

    seen: set[str] = set()
    left_arrays: list[Any] = []
    right_arrays: list[Any] = []
    lengths: list[int] = []
    id_buffers: list[bytes] = []
    name_buffers: list[bytes | None] = []
    sample_rates: list[int] = []
    any_rate = False
    any_right = False

    for index, track in enumerate(tracks):
        track_id = track.id
        if not isinstance(track_id, str) or not track_id:
            raise _refuse(f"tracks[{index}].id must be a non-empty string")
        if track_id in seen:
            raise _refuse(f"tracks[{index}].id must be unique among tracks")
        seen.add(track_id)

        if track.sample_rate is None:
            sample_rates.append(sample_rate)
        else:
            # Refused like the call's own rate: narrowed first, then the sign.
            track_rate = _narrow_int(
                track.sample_rate,
                f"tracks[{index}].sample_rate",
                1,
                _C_INT_MAX,
            )
            sample_rates.append(track_rate)
            any_rate = True

        # Coerce only: a non-finite sample is degenerate audio here, not a
        # malformed call. The core classifies it per track and returns
        # `usable: false` with an exclusion reason, so preflighting it would
        # turn a partial result into a failed call and make that reason
        # unreachable. Rank and type are still named by the coercion.
        channels = track.channels
        if not isinstance(channels, (list, tuple)):
            raise _refuse_type(f"tracks[{index}].channels must be a list or tuple of planes")
        if len(channels) not in (1, 2):
            raise _refuse(f"tracks[{index}].channels must hold 1 or 2 planes")
        left = _as_float32_buffer(channels[0], arg_name=f"tracks[{index}].channels[0]")
        left_array, left_length = _to_c_float_array(left)
        if len(channels) == 1:
            right_array = None
            right_length = left_length
        else:
            right = _as_float32_buffer(channels[1], arg_name=f"tracks[{index}].channels[1]")
            right_array, right_length = _to_c_float_array(right)
            any_right = True
        if right_length != left_length:
            raise _refuse(f"tracks[{index}].channels planes must have equal lengths")

        left_arrays.append(left_array)
        right_arrays.append(right_array)
        lengths.append(left_length)
        id_buffers.append(_utf8_arg(track_id, f"tracks[{index}].id"))
        name_buffers.append(_utf8_arg(track.name, "name") if track.name is not None else None)

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
    # NULL when no track names its own rate: every track is at the call's rate.
    rate_array = _to_c_int_array(sample_rates, "sample_rates")[0] if any_rate else None

    pins: list[Any] = [left_arrays, right_arrays, id_buffers, name_buffers]
    return _TrackArrays(
        left_ptrs, right_ptrs, id_array, name_array, length_array, rate_array, count, pins
    )


def _assistant_params(options: dict[str, float | int | bool | None]) -> tuple[Any, int]:
    """Marshal the supplied options, leaving unset ones to the core defaults.

    An option left at ``None`` is not placed in the param array at all, so the
    value the C++ config declares stays in force instead of being overwritten
    with a default duplicated on this side.
    """
    params: dict[str, float | int | bool] = {}
    for name, value in options.items():
        if value is None:
            continue
        if name in _INTEGER_PARAMS:
            # Ahead of the bool branch, so `True` is refused rather than read as 1.
            params[_PARAM_KEYS[name]] = _validate_c_int_field(value, name)
            continue
        if isinstance(value, bool):
            params[_PARAM_KEYS[name]] = value
            continue
        number = _narrow_double(value, f"{name}")
        if not np.isfinite(number):
            raise _refuse(f"{name} must be finite")
        params[_PARAM_KEYS[name]] = number
    return _mastering_params(params)


def _suggest(
    tracks: Sequence[MixAssistantTrack],
    sample_rate: int,
    options: dict[str, float | int | bool | None],
) -> Any:
    """Run the suggest entry point and return its parsed document."""
    # The symbol is exported unconditionally -- a build without the assistant
    # keeps it and answers SONARE_ERROR_NOT_SUPPORTED -- so there is nothing to
    # probe for here. _check turns that answer into the same error class every
    # other unsupported call raises.
    lib = _get_lib()
    # Narrowed, not int()-coerced: int() takes 44100.7 as 44100, a rate the
    # whole suggestion would then be computed at.
    sample_rate_value = _narrow_int(sample_rate, "sample_rate", 1, _C_INT_MAX)
    arrays = _build_track_arrays(tracks, sample_rate_value)
    param_array, param_count = _assistant_params(options)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mixing_assistant_suggest(
        arrays.left_ptrs,
        arrays.right_ptrs,
        arrays.ids,
        arrays.names,
        arrays.lengths,
        arrays.sample_rates,
        ctypes.c_size_t(arrays.count),
        _to_c_int(sample_rate_value, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return json.loads(ctypes.string_at(json_ptr).decode("utf-8")) if json_ptr.value else {}
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def _track_profile_from_wire(wire: dict[str, Any]) -> MixAssistantTrackProfile:
    fields = _fields(wire)
    fields["name"] = fields["name"] or None
    fields["exclusion_reason"] = fields["exclusion_reason"] or None
    for key in (
        "source_confidence",
        "duration_sec",
        "true_peak_dbtp",
        "crest_factor_db",
        "spectral_centroid_hz",
        "spectral_flatness",
        "attack_per_second",
        "sustain_ratio",
    ):
        fields[key] = float(fields[key])
    if fields["integrated_lufs"] is not None:
        fields["integrated_lufs"] = float(fields["integrated_lufs"])
    fields["band_occupancy"] = {band: float(share) for band, share in wire["bandOccupancy"].items()}
    return MixAssistantTrackProfile(**fields)


def _mix_profile_from_wire(wire: dict[str, Any]) -> MixAssistantMixProfile:
    return MixAssistantMixProfile(
        track_count=int(wire["trackCount"]),
        band_dominance=[
            MixBandDominance(
                masker=row["masker"],
                maskee=row["maskee"],
                band=row["band"],
                ratio=float(row["ratio"]),
                valid_frames=int(row["validFrames"]),
            )
            for row in wire["bandDominance"]
        ],
        alignment=[
            MixTrackAlignment(
                reference=row["reference"],
                target=row["target"],
                lag_samples=int(row["lagSamples"]),
                correlation=float(row["correlation"]),
                polarity_opposed=bool(row["polarityOpposed"]),
            )
            for row in wire["alignment"]
        ],
        crowded_bands=[
            MixCrowdedBand(band=row["band"], crowding=float(row["crowding"]))
            for row in wire["crowdedBands"]
        ],
        mono_risks=[
            MixMonoRisk(
                strip_id=row["stripId"],
                correlation=float(row["correlation"]),
                width=float(row["width"]),
                wide_low_end=bool(row["wideLowEnd"]),
            )
            for row in wire["monoRisks"]
        ],
    )


def _mix_assistant_result_from_wire(wire: dict[str, Any]) -> MixAssistantResult:
    return MixAssistantResult(
        scene=wire["scene"],
        tracks=[_track_profile_from_wire(track) for track in wire["tracks"]],
        mix=_mix_profile_from_wire(wire["mix"]),
        explanation=[str(line) for line in wire["explanation"]],
    )


@_entry("mixing_assistant_suggest")
def mixing_assistant_suggest(
    tracks: Sequence[MixAssistantTrack],
    *,
    sample_rate: int,
    target_track_lufs: float | None = None,
    suggestion_strength: float | None = None,
    eq_max_cut_db: float | None = None,
    mix_bus_headroom_dbtp: float | None = None,
    tempo_bpm: float | None = None,
    enable_structure: bool | None = None,
    enable_gain: bool | None = None,
    enable_balance: bool | None = None,
    enable_eq: bool | None = None,
    enable_dynamics: bool | None = None,
    enable_image: bool | None = None,
    enable_high_pass: bool | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
) -> MixAssistantResult:
    """Analyse ``tracks`` and return a suggested scene with the reasoning behind it.

    Args:
        tracks: Tracks to analyse, each a :class:`MixAssistantTrack`. Lengths may
            differ between tracks. An empty sequence yields an empty suggestion.
            A track whose audio is degenerate — empty, at a non-positive sample
            rate, holding a NaN or Inf sample, too short for a gated loudness,
            silent, or without energy in the analysis bands — is excluded and
            reported with ``usable`` False and an ``exclusion_reason``
            naming which, rather than failing the call. A non-finite sample is
            reported as itself rather than as silence, so the reason describes
            the buffer instead of the material.
        sample_rate: Analysis rate, in Hz, and the rate of every track that
            does not state its own; a track at another rate is resampled onto it.
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
        tempo_bpm: Tempo the suggested delay times are voiced against, in BPM.
            ``None`` (the default) selects the transport's own fallback tempo: the
            assistant is handed bare stems and cannot measure a tempo the set as
            a whole agrees on. Pass the song's tempo and the delay lands on the
            beat. Zero, a negative, or a value outside 20-400 BPM is rejected
            rather than clamped.
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
        A :class:`MixAssistantResult`: ``scene`` (the document
        :class:`~libsonare.Mixer` takes as ``scene``), ``tracks`` and ``mix``
        with the measurements behind the suggestion, and ``explanation``, a list
        of human-readable lines in application order.
    """
    # None is the fallback; 0 is the C entry's spelling of it and is not a tempo here.
    if tempo_bpm is not None and tempo_bpm <= 0:
        raise _refuse("tempo_bpm must be greater than 0")
    wire = _suggest(
        tracks,
        sample_rate,
        {
            "target_track_lufs": target_track_lufs,
            "suggestion_strength": suggestion_strength,
            "eq_max_cut_db": eq_max_cut_db,
            "mix_bus_headroom_dbtp": mix_bus_headroom_dbtp,
            "tempo_bpm": tempo_bpm,
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
    return _mix_assistant_result_from_wire(wire)


@_entry("mixing_assistant_source_class_names")
def mixing_assistant_source_class_names() -> list[str]:
    """Return the source-class identifiers the assistant can report.

    Every name returned is a class some shipped entry point can actually put on
    a track: most come from the classifier's measured decision table, and
    ``keys``, ``strings``, ``lead``, ``vocal``, ``backing`` and ``fx`` from a
    track's ``name``.

    Raises:
        RuntimeError: If the library was built without the mixing assistant.
            The entry point exists in such a build and answers with an empty
            list, which is the one answer that cannot be a real taxonomy.
    """
    lib = _get_lib()
    names = _joined_names(
        lib.sonare_mixing_assistant_source_class_names(), "mixing assistant source class names"
    )
    if not names:
        raise _not_supported("libsonare was built without mixing assistant support")
    return names


@_entry("mixing_assistant_source_class_from_name")
def mixing_assistant_source_class_from_name(name: str) -> int:
    """Resolve a source-class identifier to its enum value.

    Raises:
        TypeError: If ``name`` is not a string.
        SonareValueError: If ``name`` is not one of :func:`mixing_assistant_source_class_names`.
    """
    refusal = f"name must be one of {', '.join(mixing_assistant_source_class_names())}"
    if not isinstance(name, str):
        raise _refuse_type(refusal)
    lib = _get_lib()
    value = int(lib.sonare_mixing_assistant_source_class_from_name(_utf8_arg(name, "name")))
    if value < 0:
        raise _refuse(refusal)
    return value
