"""Mastering wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
from collections.abc import Mapping, Sequence
from typing import Any, cast

import numpy as np

from ._effects_repair_session import _audio_defects_from_wire
from ._errors import _entry, _not_supported, _refuse, _refuse_type
from ._ffi import (
    SonareLoudnessMatch,
    SonareMasteringResult,
    SonareMasteringStereoResult,
    SonareStreamingPlatform,
)
from ._mastering_offline import (
    _assistant_params,
    _mastering_params,
)
from ._narrowing import _narrow_double
from ._result_records import _record
from ._runtime import (
    _check,
    _finite_or_none,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _require_instance,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
)
from .types import (
    LoudnessMatchResult,
    LoudnessMatchStereoResult,
    MasteringAssistantProfile,
    MasteringAssistantResult,
    MasteringAudioProfile,
    MasteringAudioProfileDynamics,
    MasteringAudioProfileLoudness,
    MasteringAudioProfileSpectral,
    MasteringChainConfig,
    MasteringResult,
    MasteringStereoResult,
    MasteringStreamingPreviewPlatform,
    MasteringStreamingPreviewResult,
    MatchEqCurveResult,
    MatchEstimateReferenceDelaySamplesResult,
    MatchReferenceLoudnessResult,
    MatchTonalBalanceBand,
    MatchTonalBalanceResult,
    StereoMonoCompatBand,
    StereoMonoCompatCheckLogBandsResult,
    StereoMonoCompatCheckResult,
    StreamingPlatform,
)


def _reject_nul(value: str, argument: str) -> None:
    """Reject an embedded NUL that the C string would silently truncate."""
    if "\x00" in value:
        raise _refuse(f"{argument} must not contain NUL")


def _stereo_pair_params(params: dict[str, float | int | bool] | None) -> tuple[Any, int]:
    """Marshal stereo pair params after checking their C-string keys."""
    for key in params or {}:
        _reject_nul(str(key), "parameter key")
    return _mastering_params(params)


@_entry("mastering_apply_pair_processor")
@_guard_buffer("source", "reference")
def mastering_apply_pair_processor(
    processor: str,
    source: Sequence[float] | list[float],
    reference: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool] | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MasteringResult:
    """Apply a named two-input mastering processor.

    ``reference`` may be at its own ``reference_sample_rate`` (default: ``sample_rate``);
    the library resamples it onto ``sample_rate`` first, so the output is at
    ``sample_rate``.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_pair_processor"):
        raise _not_supported("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source)
    reference_array, reference_length = _to_c_float_array(reference)
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringResult()
    # Reference masters are commonly a different length than the source; the _ex
    # variant takes independent source/reference lengths (the pair primitives
    # consume each buffer at its own length).
    rc = lib.sonare_mastering_apply_pair_processor_ex(
        _utf8_arg(processor, "processor"),
        source_array,
        _to_c_size_t(source_length, "source_length"),
        reference_array,
        _to_c_size_t(reference_length, "reference_length"),
        _to_c_int(
            sample_rate if reference_sample_rate is None else reference_sample_rate,
            "reference_sample_rate",
        ),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringResult(
            samples=_from_c_float_array(out.samples, out.length),
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
            non_finite_substitution_count=int(out.non_finite_substitution_count),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


@_entry("mastering_apply_pair_processor_stereo")
@_guard_buffer("source_left", "source_right", "reference_left", "reference_right")
def mastering_apply_pair_processor_stereo(
    processor: str,
    source_left: Sequence[float] | list[float],
    source_right: Sequence[float] | list[float],
    reference_left: Sequence[float] | list[float],
    reference_right: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool] | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> MasteringStereoResult:
    """Apply the stereo ``match.abCrossfade`` pair processor.

    Source and reference may have different lengths and rates; the left/right
    planes in each pair must have the same length. A reference at
    ``reference_sample_rate`` (default: ``sample_rate``) is resampled onto
    ``sample_rate`` first. The native processor returns the shorter pair length
    and applies one mix to both channels.
    """
    if not isinstance(processor, str):
        raise _refuse_type("processor must be a string")
    _reject_nul(processor, "processor")
    param_array, param_count = _stereo_pair_params(params)
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_pair_processor_stereo_ex"):
        raise _not_supported("libsonare was built without stereo pair mastering support")
    source_left_array, source_left_length = _to_c_float_array(source_left, arg_name="source_left")
    source_right_array, source_right_length = _to_c_float_array(
        source_right, arg_name="source_right"
    )
    reference_left_array, reference_left_length = _to_c_float_array(
        reference_left, arg_name="reference_left"
    )
    reference_right_array, reference_right_length = _to_c_float_array(
        reference_right, arg_name="reference_right"
    )
    if source_left_length != source_right_length:
        raise _refuse("source_left and source_right channel lengths must match")
    if reference_left_length != reference_right_length:
        raise _refuse("reference_left and reference_right channel lengths must match")
    out = SonareMasteringStereoResult()
    try:
        rc = lib.sonare_mastering_apply_pair_processor_stereo_ex(
            _utf8_arg(processor, "processor"),
            source_left_array,
            source_right_array,
            _to_c_size_t(source_left_length, "source_length"),
            reference_left_array,
            reference_right_array,
            _to_c_size_t(reference_left_length, "reference_length"),
            _to_c_int(
                sample_rate if reference_sample_rate is None else reference_sample_rate,
                "reference_sample_rate",
            ),
            _to_c_int(sample_rate, "sample_rate"),
            param_array,
            _to_c_size_t(param_count, "param_count"),
            ctypes.byref(out),
        )
        _check(rc)
        return MasteringStereoResult(
            left=_from_c_float_array(out.left, out.length),
            right=_from_c_float_array(out.right, out.length),
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
            loudness_target_limited=bool(out.loudness_target_limited),
            non_finite_substitution_count=int(out.non_finite_substitution_count),
        )
    finally:
        lib.sonare_free_mastering_stereo_result(ctypes.byref(out))


@_entry("mastering_analyze_pair")
@_guard_buffer("source", "reference")
def mastering_analyze_pair(
    analysis: str,
    source: Sequence[float] | list[float],
    reference: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool] | None = None,
    *,
    reference_sample_rate: int | None = None,
) -> (
    MatchReferenceLoudnessResult
    | MatchTonalBalanceResult
    | MatchEqCurveResult
    | MatchEstimateReferenceDelaySamplesResult
):
    """Run a named two-input mastering analysis and return its record.

    ``reference`` may be at its own ``reference_sample_rate`` (default:
    ``sample_rate``); it is resampled onto ``sample_rate`` first, so a delay
    estimate is in source-rate samples.

    ``match.referenceLoudness`` returns :class:`MatchReferenceLoudnessResult`,
    ``match.tonalBalance`` and ``match.tonalBalanceLogBands``
    :class:`MatchTonalBalanceResult`, ``match.matchEqCurve``
    :class:`MatchEqCurveResult` and ``match.estimateReferenceDelaySamples``
    :class:`MatchEstimateReferenceDelaySamplesResult`.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_analyze_pair"):
        raise _not_supported("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source)
    reference_array, reference_length = _to_c_float_array(reference)
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    # Independent source/reference lengths (see mastering_apply_pair_processor).
    rc = lib.sonare_mastering_analyze_pair_ex(
        _utf8_arg(analysis, "analysis"),
        source_array,
        _to_c_size_t(source_length, "source_length"),
        reference_array,
        _to_c_size_t(reference_length, "reference_length"),
        _to_c_int(
            sample_rate if reference_sample_rate is None else reference_sample_rate,
            "reference_sample_rate",
        ),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    return _pair_analysis_from_wire(analysis, _take_json(lib, json_ptr))


@_entry("mastering_ab_match_loudness")
@_guard_buffer("source", "reference")
def mastering_ab_match_loudness(
    source: Sequence[float] | list[float] | np.ndarray,
    reference: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    reference_sample_rate: int | None = None,
) -> LoudnessMatchResult:
    """Gain-match ``source`` to ``reference``'s integrated loudness.

    An A/B between two takes is otherwise decided by whichever is louder. The
    gain is applied with no upper bound and :attr:`LoudnessMatchResult.
    matched_true_peak_dbtp` reports where that left the peak, rather than the
    call capping it: a headroom clamp would return ``source`` at its own
    loudness whenever it started near full scale, which is the one thing a
    match must not do. Clamp afterwards if the result has to stay under a
    ceiling.

    The two takes need not be the same length or rate; each is measured over its
    own buffer, a reference at another rate after it is resampled onto
    ``sample_rate``. Only ``source`` is returned -- ``reference`` is read as the
    loudness target and is not itself modified.

    Args:
        source: Mono take to be matched (any sequence convertible to float32).
        reference: Mono take whose loudness ``source`` is brought to.
        sample_rate: Sample rate of ``source`` and of the result, in Hz.
        reference_sample_rate: Sample rate of ``reference``, in Hz; omitted
            means ``sample_rate``.

    Returns:
        A :class:`LoudnessMatchResult`: ``samples`` is a ``float32``
        ``numpy.ndarray`` as long as ``source`` at ``sample_rate``, beside the
        measured loudnesses, the applied gain and the matched true peak.

    Example:
        >>> result = mastering_ab_match_loudness(take, ref, 48000)
        >>> result.applied_gain_db  # doctest: +SKIP
        -3.2
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_ab_match_loudness"):
        raise _not_supported("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source, arg_name="source")
    reference_array, reference_length = _to_c_float_array(reference, arg_name="reference")
    match = SonareLoudnessMatch()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_mastering_ab_match_loudness(
            source_array,
            _to_c_size_t(source_length, "source_length"),
            reference_array,
            _to_c_size_t(reference_length, "reference_length"),
            _to_c_int(
                sample_rate if reference_sample_rate is None else reference_sample_rate,
                "reference_sample_rate",
            ),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(match),
        )
        _check(rc)
        matched = _from_c_float_array(out, out_length.value)
    return LoudnessMatchResult(
        samples=matched,
        sample_rate=sample_rate,
        reference_lufs=_finite_or_none(match.reference_lufs),
        source_lufs=_finite_or_none(match.source_lufs),
        applied_gain_db=float(match.applied_gain_db),
        matched_true_peak_dbtp=float(match.matched_true_peak_dbtp),
    )


@_entry("mastering_ab_match_loudness_stereo")
@_guard_buffer("source_left", "source_right", "reference_left", "reference_right")
def mastering_ab_match_loudness_stereo(
    source_left: Sequence[float] | list[float] | np.ndarray,
    source_right: Sequence[float] | list[float] | np.ndarray,
    reference_left: Sequence[float] | list[float] | np.ndarray,
    reference_right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    reference_sample_rate: int | None = None,
) -> LoudnessMatchStereoResult:
    """Gain-match a stereo source to a stereo reference with one shared gain.

    Each source/reference pair must have equal left/right lengths, while the
    source and reference may have different lengths and rates; a reference at
    ``reference_sample_rate`` (default: ``sample_rate``) is resampled onto
    ``sample_rate`` before it is measured. Returns a
    :class:`LoudnessMatchStereoResult` whose ``left`` and ``right`` planes are
    ``float32`` arrays.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_ab_match_loudness_stereo"):
        raise _not_supported("libsonare was built without stereo A/B loudness-match support")
    source_left_array, source_left_length = _to_c_float_array(source_left, arg_name="source_left")
    source_right_array, source_right_length = _to_c_float_array(
        source_right, arg_name="source_right"
    )
    reference_left_array, reference_left_length = _to_c_float_array(
        reference_left, arg_name="reference_left"
    )
    reference_right_array, reference_right_length = _to_c_float_array(
        reference_right, arg_name="reference_right"
    )
    if source_left_length != source_right_length:
        raise _refuse("source_left and source_right channel lengths must match")
    if reference_left_length != reference_right_length:
        raise _refuse("reference_left and reference_right channel lengths must match")
    out = SonareMasteringStereoResult()
    match = SonareLoudnessMatch()
    try:
        rc = lib.sonare_mastering_ab_match_loudness_stereo(
            source_left_array,
            source_right_array,
            _to_c_size_t(source_left_length, "source_length"),
            reference_left_array,
            reference_right_array,
            _to_c_size_t(reference_left_length, "reference_length"),
            _to_c_int(
                sample_rate if reference_sample_rate is None else reference_sample_rate,
                "reference_sample_rate",
            ),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(out),
            ctypes.byref(match),
        )
        _check(rc)
        matched = (
            _from_c_float_array(out.left, out.length),
            _from_c_float_array(out.right, out.length),
        )
    finally:
        lib.sonare_free_mastering_stereo_result(ctypes.byref(out))
    return LoudnessMatchStereoResult(
        left=matched[0],
        right=matched[1],
        sample_rate=sample_rate,
        reference_lufs=_finite_or_none(match.reference_lufs),
        source_lufs=_finite_or_none(match.source_lufs),
        applied_gain_db=float(match.applied_gain_db),
        matched_true_peak_dbtp=float(match.matched_true_peak_dbtp),
    )


@_entry("mastering_analyze_stereo")
@_guard_buffer("left", "right")
def mastering_analyze_stereo(
    analysis: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool] | None = None,
) -> StereoMonoCompatCheckResult | StereoMonoCompatCheckLogBandsResult:
    """Run a named stereo mastering analysis and return its record.

    ``stereo.monoCompatCheck`` returns :class:`StereoMonoCompatCheckResult` and
    ``stereo.monoCompatCheckLogBands`` :class:`StereoMonoCompatCheckLogBandsResult`.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_analyze_stereo"):
        raise _not_supported("libsonare was built without mastering support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise _refuse("left and right channel lengths must match")
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_analyze_stereo(
        _utf8_arg(analysis, "analysis"),
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    return _stereo_analysis_from_wire(analysis, _take_json(lib, json_ptr))


_PLATFORM_KEYS = ("name", "target_lufs", "ceiling_dbtp")
# The camelCase spellings a platform record does not take, with the key that replaces each.
_PLATFORM_RENAMED = {
    "targetLufs": "target_lufs",
    "ceilingDb": "ceiling_dbtp",
    "ceilingDbtp": "ceiling_dbtp",
    "ceiling_db": "ceiling_dbtp",
}


def _platform_fields(platform: Mapping[str, Any]) -> tuple[str, float, float]:
    """Read one :class:`StreamingPlatform` record, refusing any other key by name."""
    if not isinstance(platform, Mapping):
        raise _refuse_type("each platform must be a mapping")
    for key in platform:
        if key in _PLATFORM_RENAMED:
            raise _refuse(f"platform has unknown key {key!r}; use {_PLATFORM_RENAMED[key]!r}")
        if key not in _PLATFORM_KEYS:
            raise _refuse(f"platform has unknown key {key!r}")
    missing = [key for key in _PLATFORM_KEYS if key not in platform]
    if missing:
        raise _refuse(f"platform is missing {', '.join(missing)}")
    name = platform["name"]
    if not isinstance(name, str):
        raise _refuse_type("platform name must be a string")
    return (
        name,
        _narrow_double(platform["target_lufs"], "target_lufs"),
        _narrow_double(platform["ceiling_dbtp"], "ceiling_dbtp"),
    )


def _streaming_platforms(
    platforms: Sequence[StreamingPlatform] | None,
) -> tuple[Any, int, list[bytes]]:
    """Build the C platform array, keeping the name buffers alive for the call.

    ``None`` is the library's built-in platform set; an empty list is refused.
    """
    if platforms is None:
        return None, 0, []
    fields = [_platform_fields(platform) for platform in platforms]
    if not fields:
        raise _refuse("platforms must not be empty")
    array_type = SonareStreamingPlatform * len(fields)
    platform_buffers = [_utf8_arg(name, "name") for name, _, _ in fields]
    platform_array = array_type(
        *[
            SonareStreamingPlatform(
                name=platform_buffers[index], target_lufs=target, ceiling_dbtp=ceiling
            )
            for index, (_, target, ceiling) in enumerate(fields)
        ]
    )
    return platform_array, len(fields), platform_buffers


def _take_json(lib: Any, json_ptr: ctypes.c_char_p) -> Any:
    """Parse and free a native JSON result string."""
    try:
        return json.loads(ctypes.string_at(json_ptr).decode("utf-8")) if json_ptr.value else {}
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def _optional_float(value: Any) -> float | None:
    return None if value is None else float(value)


def _pair_analysis_from_wire(
    name: str, wire: Mapping[str, Any]
) -> (
    MatchReferenceLoudnessResult
    | MatchTonalBalanceResult
    | MatchEqCurveResult
    | MatchEstimateReferenceDelaySamplesResult
):
    if name == "match.referenceLoudness":
        return MatchReferenceLoudnessResult(
            source_lufs=_optional_float(wire["sourceLufs"]),
            reference_lufs=_optional_float(wire["referenceLufs"]),
            gain_to_match_db=_optional_float(wire["gainToMatchDb"]),
        )
    if name in ("match.tonalBalance", "match.tonalBalanceLogBands"):
        return MatchTonalBalanceResult(
            bands=[
                MatchTonalBalanceBand(
                    low_hz=float(band["lowHz"]),
                    high_hz=float(band["highHz"]),
                    source_db=float(band["sourceDb"]),
                    reference_db=float(band["referenceDb"]),
                    deviation_db=float(band["deviationDb"]),
                )
                for band in wire["bands"]
            ]
        )
    if name == "match.matchEqCurve":
        return MatchEqCurveResult(
            frequencies=np.asarray(wire["frequencies"], dtype=np.float32),
            gain_db=np.asarray(wire["gainDb"], dtype=np.float32),
        )
    return MatchEstimateReferenceDelaySamplesResult(delay_samples=int(wire["delaySamples"]))


def _stereo_analysis_from_wire(
    name: str, wire: Mapping[str, Any]
) -> StereoMonoCompatCheckResult | StereoMonoCompatCheckLogBandsResult:
    if name == "stereo.monoCompatCheck":
        return StereoMonoCompatCheckResult(
            correlation=float(wire["correlation"]),
            width=_optional_float(wire["width"]),
            mono_peak=float(wire["monoPeak"]),
            side_rms=float(wire["sideRms"]),
            likely_mono_compatible=bool(wire["likelyMonoCompatible"]),
        )
    return StereoMonoCompatCheckLogBandsResult(
        bands=[
            StereoMonoCompatBand(
                low_hz=float(band["lowHz"]),
                high_hz=float(band["highHz"]),
                correlation=float(band["correlation"]),
                side_rms=float(band["sideRms"]),
            )
            for band in wire["bands"]
        ]
    )


def _streaming_preview_from_wire(wire: Mapping[str, Any]) -> MasteringStreamingPreviewResult:
    return MasteringStreamingPreviewResult(
        platforms=[
            MasteringStreamingPreviewPlatform(
                name=str(platform["name"]),
                integrated_lufs=(
                    None
                    if platform["integratedLufs"] is None
                    else float(platform["integratedLufs"])
                ),
                true_peak_dbtp=float(platform["truePeakDbtp"]),
                normalization_gain_db=float(platform["normalizationGainDb"]),
                ceiling_risk=bool(platform["ceilingRisk"]),
            )
            for platform in wire["platforms"]
        ]
    )


def _assistant_result_from_wire(wire: Mapping[str, Any]) -> MasteringAssistantResult:
    return MasteringAssistantResult(
        chain_config=_document_params(wire["chainConfig"]),
        explanation=[str(line) for line in wire["explanation"]],
        profile=_record(MasteringAssistantProfile, wire["profile"]),
    )


def _audio_profile_from_wire(wire: Mapping[str, Any]) -> MasteringAudioProfile:
    defects = wire.get("defects")
    return MasteringAudioProfile(
        duration_sec=float(wire["durationSec"]),
        bpm=float(wire["bpm"]),
        bpm_confidence=float(wire["bpmConfidence"]),
        loudness=_record(MasteringAudioProfileLoudness, wire["loudness"]),
        spectral=_record(MasteringAudioProfileSpectral, wire["spectral"]),
        dynamics=_record(MasteringAudioProfileDynamics, wire["dynamics"]),
        defects=None if defects is None else _audio_defects_from_wire(defects),
    )


@_entry("mastering_streaming_preview")
@_guard_buffer("samples")
def mastering_streaming_preview(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    platforms: Sequence[StreamingPlatform] | None = None,
) -> MasteringStreamingPreviewResult:
    """Preview streaming-platform normalization and ceiling risk.

    ``platforms`` lists the targets to preview; ``None`` takes the library's
    built-in set and an empty list is refused.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_streaming_preview"):
        raise _not_supported("libsonare was built without mastering streaming preview support")
    c_array, length = _to_c_float_array(samples)
    platform_array, platform_count, _buffers = _streaming_platforms(platforms)

    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_streaming_preview(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        platform_array,
        _to_c_size_t(platform_count, "platform_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    return _streaming_preview_from_wire(_take_json(lib, json_ptr))


@_entry("mastering_assistant_suggest")
@_guard_buffer("samples")
def mastering_assistant_suggest(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool | str] | None = None,
) -> MasteringAssistantResult:
    """Analyze audio and suggest a mastering chain with the reasoning behind it.

    ``params`` accepts ``targetPlatform`` as a delivery-target name (see
    :func:`mastering_platform_names`) and ``preset`` as the mastering preset the
    suggestion starts from (default ``"streaming"``; restoration presets are
    refused); every other key is numeric. ``chain_config`` is the chain document's params
    :func:`mastering_assistant_suggest_chain` returns.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_assistant_suggest"):
        raise _not_supported("libsonare was built without mastering assistant support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _assistant_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_assistant_suggest(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    return _assistant_result_from_wire(_take_json(lib, json_ptr))


def _document_params(document: Mapping[str, Any]) -> MasteringChainConfig:
    """The ``params`` of a version 2 chain document, as the core wrote it."""
    return cast(MasteringChainConfig, document["params"])


def _chain_config_from_document(json_text: str) -> MasteringChainConfig:
    """The ``params`` of a version 2 chain document's text."""
    return _document_params(json.loads(json_text))


@_entry("mastering_assistant_suggest_chain")
@_guard_buffer("samples")
def mastering_assistant_suggest_chain(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool | str] | None = None,
) -> MasteringChainConfig:
    """Suggest a mastering chain as the chain document's params, ready as ``overrides``.

    Skips parsing the full profile+chain document :func:`mastering_assistant_suggest`
    returns down to the chain config it already carries.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_assistant_suggest_chain_json"):
        raise _not_supported("libsonare was built without mastering assistant support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _assistant_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_assistant_suggest_chain_json(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        json_text = ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)
    return _chain_config_from_document(json_text)


@_entry("mastering_audio_profile")
@_guard_buffer("samples")
def mastering_audio_profile(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringAudioProfile:
    """Analyze audio and return the mastering assistant profile.

    The ``nFft`` and ``hopLength`` params are the window length and hop in
    samples at 48000 Hz, rescaled to the input sample rate.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_audio_profile"):
        raise _not_supported("libsonare was built without mastering audio profile support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_audio_profile(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    return _audio_profile_from_wire(_take_json(lib, json_ptr))


def _stereo_channels(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
) -> tuple[Any, Any, int]:
    left_array, left_length = _to_c_float_array(left, arg_name="left")
    right_array, right_length = _to_c_float_array(right, arg_name="right")
    if left_length != right_length:
        raise _refuse("left and right channel lengths must match")
    return left_array, right_array, left_length


def _stereo_analysis_json(
    symbol: str,
    missing_message: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    param_array: Any,
    param_count: int,
) -> str:
    lib = _get_lib()
    if not hasattr(lib, symbol):
        raise _not_supported(missing_message)
    left_array, right_array, length = _stereo_channels(left, right)
    json_ptr = ctypes.c_char_p()
    rc = getattr(lib, symbol)(
        left_array,
        right_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


@_entry("mastering_streaming_preview_stereo")
@_guard_buffer("left", "right")
def mastering_streaming_preview_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    platforms: Sequence[StreamingPlatform] | None = None,
) -> MasteringStreamingPreviewResult:
    """Preview streaming normalization for a stereo pair.

    Measures the integrated loudness with BS.1770 channel summing and reports
    the larger of the two channel true peaks. Passing a ``0.5 * (left + right)``
    downmix to :func:`mastering_streaming_preview` instead reads roughly 6 dB
    low on decorrelated material, and both the normalization gain and the
    ceiling-risk flag follow from that measurement. ``platforms`` is as for
    :func:`mastering_streaming_preview`.
    """
    platform_array, platform_count, _buffers = _streaming_platforms(platforms)
    json_text = _stereo_analysis_json(
        "sonare_mastering_streaming_preview_stereo",
        "libsonare was built without mastering streaming preview support",
        left,
        right,
        sample_rate,
        platform_array,
        platform_count,
    )
    return _streaming_preview_from_wire(json.loads(json_text))


@_entry("mastering_assistant_suggest_stereo")
@_guard_buffer("left", "right")
def mastering_assistant_suggest_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool | str] | None = None,
) -> MasteringAssistantResult:
    """Suggest a mastering chain for a stereo pair.

    Profiles through :func:`mastering_audio_profile_stereo`, so the loudness
    stage of the suggestion is built on the channel-summed program. ``params``
    accepts ``targetPlatform`` and ``preset`` by name, as
    :func:`mastering_assistant_suggest` does.
    """
    param_array, param_count = _assistant_params(params)
    json_text = _stereo_analysis_json(
        "sonare_mastering_assistant_suggest_stereo",
        "libsonare was built without mastering assistant support",
        left,
        right,
        sample_rate,
        param_array,
        param_count,
    )
    return _assistant_result_from_wire(json.loads(json_text))


@_entry("mastering_assistant_suggest_chain_stereo")
@_guard_buffer("left", "right")
def mastering_assistant_suggest_chain_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool | str] | None = None,
) -> MasteringChainConfig:
    """Stereo counterpart of :func:`mastering_assistant_suggest_chain`."""
    param_array, param_count = _assistant_params(params)
    json_text = _stereo_analysis_json(
        "sonare_mastering_assistant_suggest_chain_json_stereo",
        "libsonare was built without mastering assistant support",
        left,
        right,
        sample_rate,
        param_array,
        param_count,
    )
    return _chain_config_from_document(json_text)


@_entry("mastering_preset_params")
def mastering_preset_params(preset: str) -> MasteringChainConfig:
    """Return a built-in preset's chain configuration document's params.

    The same shape :func:`mastering_assistant_suggest_chain` returns, so
    the result's parameter set matches what the assistant would suggest and
    applies to :func:`master_audio` as overrides unchanged.

    Args:
        preset: Preset identifier from :func:`mastering_preset_names`.

    Raises:
        SonareValueError: If ``preset`` is not a known preset name.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_preset_params_json"):
        raise _not_supported("libsonare was built without mastering preset support")
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_preset_params_json(
        _utf8_arg(preset, "preset"),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        json_text = ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)
    return _chain_config_from_document(json_text)


@_entry("mastering_chain_config_from_json")
def mastering_chain_config_from_json(document: str) -> MasteringChainConfig:
    """Validate a chain configuration document and return its params.

    Reads ``document`` as the native CLI's ``--chain-config`` reads it and returns
    the chain it describes in the shape :func:`mastering_preset_params`
    returns, ready for :func:`master_audio` as ``overrides``. Every chain
    parameter is present in the result, at its document value or its default.

    Args:
        document: Chain configuration JSON text, version 2 (``version`` may be left out).

    Raises:
        TypeError: If ``document`` is not a ``str``.
        SonareError: Code ``INVALID_FORMAT`` when the document is malformed, lacks
            ``params``, carries another version, an unknown or earlier key
            spelling, a path given twice, a null, a number for a token or an
            unknown token, or a value outside its domain; the message names the
            path and, for an earlier spelling, the current one.
    """
    _require_instance(document, str, "document")
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_chain_config_params_json"):
        raise _not_supported("libsonare was built without mastering chain config support")
    json_ptr = ctypes.c_char_p()
    _check(
        lib.sonare_mastering_chain_config_params_json(
            _utf8_arg(document, "document"), ctypes.byref(json_ptr)
        )
    )
    return _document_params(_take_json(lib, json_ptr))


@_entry("mastering_audio_profile_stereo")
@_guard_buffer("left", "right")
def mastering_audio_profile_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringAudioProfile:
    """Return the mastering assistant profile of a stereo pair.

    The ``loudness`` group is measured from the two channels; the
    spectral, dynamics and tempo fields describe shape and timing rather than
    absolute level and are measured on the downmix, which keeps them comparable
    with :func:`mastering_audio_profile`, and its ``nFft`` / ``hopLength``
    params are read the same way: samples at 48000 Hz, rescaled to the input
    sample rate. Defect detectors run on each channel
    and their results are aggregated.
    """
    param_array, param_count = _mastering_params(params)
    json_text = _stereo_analysis_json(
        "sonare_mastering_audio_profile_stereo",
        "libsonare was built without mastering audio profile support",
        left,
        right,
        sample_rate,
        param_array,
        param_count,
    )
    return _audio_profile_from_wire(json.loads(json_text))
