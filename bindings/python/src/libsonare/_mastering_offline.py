"""Mastering wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
import math
from collections.abc import Callable, Iterable, Mapping, Sequence
from typing import Any, cast

from ._cancellation import (
    CancellationState,
    make_cancel_trampoline,
    make_progress_trampoline,
    raise_callback_error,
)
from ._errors import ErrorCode, _entry, _refuse, _refuse_type
from ._ffi import (
    SonareMasteringChainResult,
    SonareMasteringChainStereoResult,
    SonareMasteringConfig,
    SonareMasteringParam,
    SonareMasteringProgressCallback,
    SonareMasteringResult,
    SonareMasteringStereoResult,
)
from ._narrowing import _narrow_double
from ._runtime import (
    _call_string,
    _check,
    _coded_error,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _last_error,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
)
from ._types_capabilities import _from_wire
from .types import (
    CapabilityCatalog,
    MasteringAmpPresetCatalogEntry,
    MasteringChainConfig,
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringInsertParamInfo,
    MasteringInsertTiming,
    MasteringLoudnessSummary,
    MasteringProcessorCatalogEntry,
    MasteringReport,
    MasteringResult,
    MasteringStereoResult,
    StageGainReduction,
)


@_entry("mastering")
@_guard_buffer("samples")
def mastering(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    target_lufs: float = -14.0,
    ceiling_dbtp: float = -1.0,
    true_peak_oversample: int | None = None,
    release_ms: float | None = None,
    apply_gain_at_input_rate: bool = False,
) -> MasteringResult:
    """Apply mastering loudness normalization with a true-peak ceiling.

    ``release_ms`` tunes the post true-peak limiter release, one time constant
    (about 63% of the way at the set time); ``None`` keeps the library default
    (50 ms), and an explicit zero or negative value raises ``SonareValueError``.
    ``apply_gain_at_input_rate`` applies the static loudness gain at the input
    (pre-oversample) rate.

    ``ceiling_dbtp`` is held on the true peak read at ``true_peak_oversample``,
    one of 1, 2, 4, 8 or 16. Default: 4 (the library's), the BS.1770 measure; a
    finer meter can read up to ~0.15 dB higher. An explicit 0 raises
    ``SonareValueError``.
    """
    lib = _get_lib()

    c_array, length = _to_c_float_array(samples)
    config = SonareMasteringConfig(
        target_lufs=target_lufs,
        ceiling_dbtp=ceiling_dbtp,
        apply_gain_at_input_rate=1 if apply_gain_at_input_rate else 0,
    )
    # An omitted knob keeps the default sonare_mastering_config_init wrote.
    if true_peak_oversample is not None:
        config.true_peak_oversample = true_peak_oversample
    if release_ms is not None:
        config.release_ms = release_ms
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_process(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc, flattened="config")
    try:
        processed = _from_c_float_array(out.samples, out.length)
        return MasteringResult(
            samples=processed,
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
            loudness_target_limited=bool(out.loudness_target_limited),
            non_finite_substitution_count=int(out.non_finite_substitution_count),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


def _processor_params(
    processor: str, params: Mapping[str, float | int | bool | str] | None
) -> dict[str, float | int | bool] | None:
    """Resolve the enum names in a solo processor's ``params`` to their numbers."""
    if not params:
        return None
    return {
        key: _enum_value(processor, key, value) if isinstance(value, str) else value
        for key, value in params.items()
    }


def _insert_params(
    processor: str, params: Mapping[str, float | int | bool | str | Sequence[float]]
) -> dict[str, float | bool | str | list[float]]:
    """Check an insert's ``params`` against the ``type`` each descriptor declares.

    A ``string`` key takes a ``str``, an ``array`` key a sequence of finite
    numbers; every other key takes a finite number or bool, or an enum name
    resolved as :func:`_processor_params` does. A wrong type is a ``TypeError``,
    a non-finite number a ``SonareValueError``. A key the processor does not
    declare is left to the core, which refuses it by name.
    """
    kinds = {info.name: info.type for info in mastering_insert_param_info(processor)}
    out: dict[str, float | bool | str | list[float]] = {}
    for key, value in params.items():
        kind = kinds.get(key)
        if kind == "string":
            if not isinstance(value, str):
                raise _refuse_type(f"params.{key} must be a str")
            out[key] = value
        elif kind == "array":
            if isinstance(value, str | bytes | Mapping) or not isinstance(value, Iterable):
                raise _refuse_type(f"params.{key} must be a sequence of numbers")
            items = list(value)
            if any(isinstance(item, bool) or not isinstance(item, int | float) for item in items):
                raise _refuse_type(f"params.{key} must be a sequence of numbers")
            if not all(math.isfinite(item) for item in items):
                raise _refuse(f"params.{key} must contain only finite numbers")
            out[key] = [float(item) for item in items]
        elif isinstance(value, str):
            if kind != "enum":
                raise _refuse_type(f"params.{key} must be a number or bool")
            out[key] = _enum_value(processor, key, value)
        elif isinstance(value, bool):
            out[key] = value
        elif isinstance(value, int | float):
            if not math.isfinite(value):
                raise _refuse(f"params.{key} must be finite")
            out[key] = float(value)
        else:
            raise _refuse_type(f"params.{key} must be a number, bool or enum name")
    return out


def _mastering_params(params: dict[str, float | int | bool] | None) -> tuple[Any, int]:
    items = list((params or {}).items())
    array_type = SonareMasteringParam * len(items)
    key_buffers = [_utf8_arg(str(key), "key") for key, _ in items]
    array = array_type(
        *[
            SonareMasteringParam(key=key_buffers[index], value=float(value))
            for index, (_, value) in enumerate(items)
        ]
    )
    return array, len(items)


def _assistant_params(
    params: dict[str, float | int | bool | str] | None,
) -> tuple[Any, int]:
    """Marshal assistant params, resolving ``targetPlatform`` and ``preset`` to indices.

    The param transport carries numbers, so each name travels as the index the
    library resolves for it. The name is looked up through the library rather
    than mapped here, so the accepted set cannot drift out of step with the core.
    """
    if not params:
        return _mastering_params(None)
    resolved: dict[str, float | int | bool] = {}
    for key, value in params.items():
        if key == "targetPlatform":
            resolved[key] = _resolve_name(
                f"params.{key}",
                value,
                "sonare_mastering_platform_from_name",
                mastering_platform_names,
            )
            continue
        if key == "preset":
            resolved[key] = _resolve_name(
                f"params.{key}",
                value,
                "sonare_mastering_preset_from_name",
                mastering_preset_names,
            )
            continue
        if isinstance(value, str):
            raise _refuse(
                f"params.{key} must be a number; only targetPlatform and preset take a name"
            )
        resolved[key] = value
    return _mastering_params(resolved)


def _resolve_name(
    key: str,
    value: object,
    symbol: str,
    names: Callable[[], list[str]],
) -> int:
    """Resolve one named assistant param to the index its C-ABI lookup returns."""
    message = f"{key} must be one of {', '.join(names())}"
    if not isinstance(value, str):
        raise _refuse(message)
    lib = _get_lib()
    index = getattr(lib, symbol)(_utf8_arg(value, "value"))
    if index < 0:
        raise _refuse(message)
    return int(index)


@_entry("mastering_processor_names")
def mastering_processor_names() -> list[str]:
    """Return supported mastering processor names shared by CLI/Node/WASM/Python."""
    lib = _get_lib()
    raw = lib.sonare_mastering_processor_names()
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_pair_processor_names")
def mastering_pair_processor_names() -> list[str]:
    """Return supported two-input mastering processor names."""
    lib = _get_lib()
    raw = lib.sonare_mastering_pair_processor_names()
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_pair_analysis_names")
def mastering_pair_analysis_names() -> list[str]:
    """Return supported two-input mastering analysis names."""
    lib = _get_lib()
    raw = lib.sonare_mastering_pair_analysis_names()
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_stereo_analysis_names")
def mastering_stereo_analysis_names() -> list[str]:
    """Return supported stereo mastering analysis names."""
    lib = _get_lib()
    raw = lib.sonare_mastering_stereo_analysis_names()
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_insert_names")
def mastering_insert_names() -> list[str]:
    """Return the mastering insert (FX) names shared by CLI/Node/WASM/Python.

    The native layer returns a lifetime-owned, newline-joined static string the
    caller must NOT free (same convention as the other ``*_names`` getters).
    """
    lib = _get_lib()
    raw = lib.sonare_mastering_insert_names()
    if raw is None:
        raise _last_error(int(ErrorCode.UNKNOWN), "failed to list mastering inserts")
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_insert_param_names")
def mastering_insert_param_names(insert: str) -> list[str]:
    """Return the camelCase parameter names a given insert/FX processor reads.

    For tools/UIs that want to validate a scene insert's params before loading
    it: any supplied key NOT in this list is silently ignored by the processor
    (and would be reported via :meth:`Mixer.scene_warnings` on a scene load).
    Band/sub-band processors enumerate their indexed ``band{i}.<field>`` keys.
    Returns an empty list for an unknown ``insert`` (or one whose insert needs an
    unavailable build feature, e.g. FX).

    The native layer returns a thread-local, newline-joined string the caller
    must NOT free (same convention as the other ``*_names`` getters).
    """
    lib = _get_lib()
    raw = lib.sonare_mastering_insert_param_names(_utf8_arg(insert, "insert"))
    if raw is None:
        raise _last_error(int(ErrorCode.UNKNOWN), "failed to list mastering insert params")
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_insert_param_info")
def mastering_insert_param_info(
    insert: str, sample_rate: int | None = None
) -> list[MasteringInsertParamInfo]:
    """Return parameter metadata for a given insert/FX processor.

    Each :class:`MasteringInsertParamInfo` describes one parameter the insert
    reads, with ``name`` (camelCase parameter name), ``id`` (stable numeric
    parameter id) and ``rt_safe`` (whether the parameter can be changed on the
    realtime audio thread), and optional ``unit`` metadata. Any id of
    :func:`mastering_processor_catalog` is served, an offline repair stage
    (``repair.declick``, ``repair.declip``, ``repair.trimSilence``) included,
    with the rows its catalog entry's ``params`` carries; those rows have a
    ``None`` ``id``. Returns an empty list for an unknown ``insert`` (or one whose
    insert needs an unavailable build feature, e.g. FX).

    With ``sample_rate``, a key whose ``max_relative_to`` is ``"nyquist"`` reports
    as ``max`` / ``max_exclusive`` the bound accepted when the insert is built and
    prepared at that rate (an EQ band frequency reaches 48000 for a 96000 Hz
    host). Without it the rate-less answer is returned, with the 24000 cap of
    the 48 kHz probe. A rate outside the supported range raises.
    """
    lib = _get_lib()
    if sample_rate is None:
        raw = _call_string(lib.sonare_mastering_insert_param_info, _utf8_arg(insert, "insert"))
    else:
        raw = _call_string(
            lib.sonare_mastering_insert_param_info_at_rate,
            _utf8_arg(insert, "insert"),
            _to_c_int(sample_rate, "sample_rate"),
        )
    return [_from_wire(MasteringInsertParamInfo, entry) for entry in json.loads(raw)]


@_entry("mastering_insert_timing")
def mastering_insert_timing(
    insert: str, params: Mapping[str, float | bool | str | Sequence[float]], sample_rate: int
) -> MasteringInsertTiming:
    """Return the latency and tail of one insert built from ``params`` at ``sample_rate``.

    What a host needs for delay compensation of a slot whose configuration
    changes the processor's delay (an oversampled saturation path, a
    linear-phase crossover, a lookahead). The insert is built exactly as a
    scene or strip would build it and asked after ``prepare``, so the answer
    is the one that instance will report. :func:`capability_catalog`'s
    ``latency_samples`` and ``tail_samples`` are this query at default
    parameters and 48 kHz.

    ``params`` is keyed as in :func:`mastering_insert_param_info` and shaped as
    the document a scene or strip insert is built from; each value matches its
    key's declared ``type``: a bool or finite number, the ``choices`` name of an
    ``enum`` key, a ``str`` for a ``string`` key, a sequence of finite numbers
    for an ``array`` key. A wrong type is a ``TypeError``. A key the insert does
    not read is refused rather than ignored, because an ignored key would answer
    for a configuration the caller did not ask for.
    """
    lib = _get_lib()
    payload = _insert_params(insert, params)
    params_json = _utf8_arg(json.dumps(payload, allow_nan=False), "params")
    out_latency = ctypes.c_int(0)
    out_tail = ctypes.c_int(0)
    rc = lib.sonare_mastering_insert_timing(
        _utf8_arg(insert, "insert"),
        params_json,
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(out_latency),
        ctypes.byref(out_tail),
    )
    _check(rc)
    return MasteringInsertTiming(
        latency_samples=int(out_latency.value), tail_samples=int(out_tail.value)
    )


@_entry("mastering_processor_catalog")
def mastering_processor_catalog() -> list[MasteringProcessorCatalogEntry]:
    """Return the full catalog of mastering processors.

    Each :class:`MasteringProcessorCatalogEntry` describes one processor with
    ``id`` (camelCase processor id, e.g. ``dynamics.compressor``), ``kind``
    (one of ``"realtime"``, ``"offline"`` or ``"pair"``), ``realtime_insertable``
    (whether the processor can run as a realtime insert), ``stereo_only``
    (whether it requires a stereo signal), ``latency_samples`` and
    ``tail_samples`` (reported processing latency and audible decay for one
    default 48 kHz / 512-sample prepared probe; 0 for offline processors and
    representative values for configuration-dependent processors), and
    ``channel_policy`` (how the mixer wraps the processor on a >2-channel
    surround bus insert:
    ``"multichannel"`` for one full-buffer call, ``"stereoPairOnly"`` for
    front-L/R-only with surround planes passed through dry). ``kind`` follows
    the precedence pair > realtime > offline:
    a processor exposed as a pair processor is reported as ``"pair"``, an
    otherwise realtime-capable processor as ``"realtime"``, and the remainder
    as ``"offline"``. Raises NOT_SUPPORTED when built without mastering.
    """
    lib = _get_lib()
    raw = _call_string(lib.sonare_mastering_processor_catalog)
    return [_from_wire(MasteringProcessorCatalogEntry, entry) for entry in json.loads(raw)]


@_entry("mastering_amp_preset_catalog")
def mastering_amp_preset_catalog() -> list[MasteringAmpPresetCatalogEntry]:
    """Return the built-in amp-sim rig catalog.

    One entry per rig, in ``saturation.ampSim`` ``presetIndex`` order, each
    with ``index``, ``name``, and ``params`` (the rig's resolved starting
    values under the insert's camelCase keys, e.g. ``drive``, ``bassDb``,
    ``ampModel``, with enum-valued keys as integer ordinals). Raises
    NOT_SUPPORTED when built without mastering.
    """
    lib = _get_lib()
    raw = _call_string(lib.sonare_mastering_amp_preset_catalog)
    return [_from_wire(MasteringAmpPresetCatalogEntry, entry) for entry in json.loads(raw)]


@_entry("capability_catalog")
def capability_catalog() -> CapabilityCatalog:
    """Return the loaded library's complete processor and preset catalog.

    Parameter bounds and defaults are ``None`` where the underlying processor
    has no published descriptor, rather than guessed by this binding.
    """
    lib = _get_lib()
    return _from_wire(
        CapabilityCatalog, json.loads(_call_string(lib.sonare_capability_catalog_json))
    )


@_entry("mastering_apply_processor")
@_guard_buffer("samples")
def mastering_apply_processor(
    processor: str,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool | str] | None = None,
) -> MasteringResult:
    """Apply a named mastering processor using the shared cross-language API.

    An enum-valued key of ``params`` also takes its ``choices`` name
    (``{"noiseEstimator": "mcra"}``), resolved by the core.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _mastering_params(_processor_params(processor, params))
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_apply_processor(
        _utf8_arg(processor, "processor"),
        c_array,
        _to_c_size_t(length, "length"),
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
            loudness_target_limited=bool(out.loudness_target_limited),
            non_finite_substitution_count=int(out.non_finite_substitution_count),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


@_entry("mastering_apply_processor_stereo")
@_guard_buffer("left", "right")
def mastering_apply_processor_stereo(
    processor: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    params: dict[str, float | int | bool | str] | None = None,
) -> MasteringStereoResult:
    """Apply a named stereo mastering processor using the shared cross-language API.

    ``params`` takes enum names as :func:`mastering_apply_processor` does.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise _refuse("left and right must have the same length")
    param_array, param_count = _mastering_params(_processor_params(processor, params))
    out = SonareMasteringStereoResult()
    rc = lib.sonare_mastering_apply_processor_stereo(
        _utf8_arg(processor, "processor"),
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        param_array,
        _to_c_size_t(param_count, "param_count"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
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


def _enum_value(processor: str | None, key: str, name: str) -> float:
    """Resolve ``name`` for the enum key ``key``; a non-enum key keeps ``float(name)``.

    ``processor`` is the processor id whose parameter ``key`` is, or ``None`` when
    ``key`` is a flat mastering chain key.
    """
    lib = _get_lib()
    is_enum = ctypes.c_int(0)
    value = ctypes.c_double(0.0)
    rc = lib.sonare_mastering_enum_value(
        None if processor is None else _utf8_arg(processor, "processor"),
        _utf8_arg(key, "key"),
        _utf8_arg(name, "name"),
        ctypes.byref(is_enum),
        ctypes.byref(value),
    )
    if rc == int(ErrorCode.INVALID_PARAMETER):
        # An unknown name is an out-of-domain argument: the core's text, as a SonareValueError.
        detail = lib.sonare_last_error_message()
        if not detail:
            raise _refuse(f"{key} must name a declared value")
        raise _coded_error(rc, detail.decode("utf-8"))
    _check(rc)
    return value.value if is_enum.value else float(name)


def _chain_config_paths(config: Mapping[str, Any] | None) -> dict[str, Any]:
    """Walk a chain config into ``{"module.processor.param": leaf}``.

    Leaves are returned unconverted. A dotted key and a nested object name the
    same path, and a list recurses as ``<path>.<index>`` (``crossover.cutoffsHz``
    -> ``crossover.cutoffsHz.0``). Content is not screened here: an input config
    has already been read by the core's document reader.
    """
    paths: dict[str, Any] = {}
    if not config:
        return paths

    def walk(value: Any, path: str) -> None:
        if isinstance(value, Mapping):
            for key, child in value.items():
                name = str(key)
                walk(child, f"{path}.{name}" if path else name)
        elif isinstance(value, (list, tuple)):
            for index, item in enumerate(value):
                walk(item, f"{path}.{index}")
        else:
            paths[path] = value

    walk(config, "")
    return paths


def _flatten_chain_config(config: Mapping[str, Any] | None) -> dict[str, float]:
    """Flatten a chain config into the dotted-key map the native call reads.

    Booleans become 0.0/1.0 and a token is resolved by the core to the number
    every flat parameter list carries.
    """
    flat: dict[str, float] = {}
    for path, value in _chain_config_paths(config).items():
        if isinstance(value, bool):
            flat[path] = 1.0 if value else 0.0
        elif isinstance(value, str):
            flat[path] = _enum_value(None, path, value)
        else:
            flat[path] = _narrow_double(value, path)
    return flat


def _nest_chain_config(flat: Mapping[str, Any]) -> dict[str, Any]:
    """Nest a native ``{"module.param": value}`` map into a chain config.

    The inverse of :func:`_flatten_chain_config`'s path walk: each dot opens an
    object, and an object whose keys are exactly ``0..n-1`` becomes a list. A
    value that is itself an object (a version 2 document's multiband stage)
    merges at its path.
    """
    root: dict[str, Any] = {}
    for path, value in flat.items():
        node = root
        parts = str(path).split(".")
        for part in parts[:-1]:
            child = node.setdefault(part, {})
            if not isinstance(child, dict):
                raise _refuse(f"{path} must not nest under a value")
            node = child
        leaf = parts[-1]
        if isinstance(value, Mapping):
            child = node.setdefault(leaf, {})
            if not isinstance(child, dict):
                raise _refuse(f"{path} must not nest under a value")
            for key, member in _nest_chain_config(value).items():
                child[key] = member
        elif isinstance(value, list):
            node[leaf] = [
                _nest_chain_config(item) if isinstance(item, Mapping) else item for item in value
            ]
        else:
            node[leaf] = value
    return cast(dict[str, Any], _lists_from_index_objects(root))


def _lists_from_index_objects(node: Any) -> Any:
    if isinstance(node, list):
        return [_lists_from_index_objects(item) for item in node]
    if not isinstance(node, dict):
        return node
    converted = {key: _lists_from_index_objects(value) for key, value in node.items()}
    if converted and all(key.isdigit() for key in converted):
        indices = sorted(int(key) for key in converted)
        if indices == list(range(len(indices))):
            return [converted[str(index)] for index in indices]
    return converted


def _check_chain_document(config: Mapping[str, Any]) -> None:
    """Have the core read ``config`` as a version 2 chain document.

    Every content refusal (unknown or earlier key, a path given twice, a null, a
    number for a token, an out-of-domain value) is the core's ``InvalidFormat``,
    naming the path.
    """
    lib = _get_lib()
    document = json.dumps({"version": 2, "params": dict(config)})
    json_ptr = ctypes.c_char_p()
    _check(
        lib.sonare_mastering_chain_config_from_json(
            _utf8_arg(document, "config"), ctypes.byref(json_ptr)
        )
    )
    if json_ptr.value:
        lib.sonare_free_string(json_ptr)


def _chain_config_json(config: Mapping[str, Any] | None, name: str) -> bytes | None:
    """Serialize a chain config for the C entries that take it as JSON; None is absent.

    The core reads it rooted at ``name``, so every content refusal names the
    caller's own argument.
    """
    if config is None:
        return None
    if not isinstance(config, Mapping):
        raise _refuse_type(f"{name} must be a mapping (parse JSON text with json.loads first)")
    return _utf8_arg(json.dumps(dict(config)), name)


def _extract_stages(stages_ptr: object, count: int) -> list[str]:
    if not stages_ptr or count == 0:
        return []
    raw_ptr = cast(Any, stages_ptr)
    result: list[str] = []
    for i in range(count):
        raw = raw_ptr[i]
        result.append(raw.decode("utf-8") if raw else "")
    return result


def _extract_stage_gain_reductions(
    stages_ptr: object, values_ptr: object, count: int
) -> list[StageGainReduction]:
    if not stages_ptr or not values_ptr or count == 0:
        return []
    raw_stages = cast(Any, stages_ptr)
    values = _from_c_float_array(values_ptr, count)
    result: list[StageGainReduction] = []
    for i in range(count):
        raw = raw_stages[i]
        result.append(
            StageGainReduction(
                stage=raw.decode("utf-8") if raw else "",
                gain_reduction_db=values[i],
            )
        )
    return result


def _extract_mastering_loudness_summary(raw: object) -> MasteringLoudnessSummary:
    summary = cast(Any, raw)
    return MasteringLoudnessSummary(
        integrated_lufs=float(summary.integrated_lufs),
        max_momentary_lufs=float(summary.max_momentary_lufs),
        max_short_term_lufs=float(summary.max_short_term_lufs),
        true_peak_dbtp=float(summary.true_peak_dbtp),
        loudness_range_lu=float(summary.loudness_range_lu),
    )


def _extract_mastering_report(raw: object) -> MasteringReport:
    report = cast(Any, raw)
    return MasteringReport(
        before=_extract_mastering_loudness_summary(report.before),
        after=_extract_mastering_loudness_summary(report.after),
        applied_gain_db=float(report.applied_gain_db),
        max_gain_reduction_db=float(report.max_gain_reduction_db),
        loudness_target_limited=bool(report.loudness_target_limited),
        band_energy_delta_db=_from_c_float_array(
            report.band_energy_delta_db, len(report.band_energy_delta_db)
        ),
    )


def _make_progress_trampoline(
    on_progress: Callable[[float, str], None], state: CancellationState
) -> Any:
    """Wrap a Python callback for use as a C SonareMasteringProgressCallback."""
    return make_progress_trampoline(SonareMasteringProgressCallback, on_progress, state)


def _run_mastering(
    lib: Any,
    symbol: str,
    head: tuple[Any, ...],
    overrides: MasteringChainConfig | None,
    on_progress: Callable[[float, str], None] | None,
    cancel: Callable[[], bool] | None,
    out: Any,
    release: Callable[[Any], None],
) -> None:
    """Call ``symbol`` or its progress variant with ``head`` plus the overrides into ``out``."""
    tail = (_chain_config_json(overrides, "overrides"),)
    state = CancellationState(cancel)
    if on_progress is None and cancel is None:
        rc = getattr(lib, symbol)(*head, *tail, ctypes.byref(out))
    else:
        cb = (
            _make_progress_trampoline(on_progress, state)
            if on_progress is not None
            else SonareMasteringProgressCallback(0)
        )
        cancel_cb = make_cancel_trampoline(state)
        rc = getattr(lib, f"{symbol}_with_progress_ex")(
            *head, *tail, cb, None, ctypes.byref(out), cancel_cb, None
        )
    raise_callback_error(state, rc, lambda: release(ctypes.byref(out)))
    _check(rc)


def _chain_result_fields(out: Any) -> dict[str, Any]:
    return {
        "sample_rate": int(out.sample_rate),
        "input_lufs": float(out.input_lufs),
        "output_lufs": float(out.output_lufs),
        "applied_gain_db": float(out.applied_gain_db),
        "stages": _extract_stages(out.stages, int(out.stages_count)),
        "output_true_peak_dbtp": float(out.output_true_peak_dbtp),
        "output_loudness_range_lu": float(out.output_loudness_range_lu),
        "loudness_target_limited": bool(out.loudness_target_limited),
        "stage_gain_reductions": _extract_stage_gain_reductions(
            out.stage_gain_reduction_stages,
            out.stage_gain_reduction_values,
            int(out.stage_gain_reductions_count),
        ),
        "report": _extract_mastering_report(out.report),
        "non_finite_substitution_count": int(out.non_finite_substitution_count),
    }


@_entry("mastering_preset_names")
def mastering_preset_names() -> list[str]:
    """Return built-in mastering preset identifiers (e.g. ``"pop"``, ``"aiMusic"``)."""
    lib = _get_lib()
    raw = lib.sonare_mastering_preset_names()
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("mastering_platform_names")
def mastering_platform_names() -> list[str]:
    """Return the delivery targets the mastering assistant accepts.

    These are the values ``targetPlatform`` selects between (e.g. ``"broadcast"``,
    ``"club"``). The list comes from the library rather than from a table kept
    here, so a target added in the core is visible without a Python change.
    """
    lib = _get_lib()
    raw = lib.sonare_mastering_platform_names()
    return raw.decode("utf-8").splitlines() if raw else []


@_entry("master_audio")
@_guard_buffer("samples")
def master_audio(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    preset: str | None = None,
    overrides: MasteringChainConfig | None = None,
    on_progress: Callable[[float, str], None] | None = None,
    cancel: Callable[[], bool] | None = None,
) -> MasteringChainResult:
    """Master mono audio with a named preset, a chain of overrides, or both.

    The chain composes (in fixed order) repair, EQ, dynamics, saturation,
    spectral, maximizer, and loudness stages. A call with neither ``preset`` nor
    ``overrides`` raises SonareValueError: the caller chooses the chain. An
    explicit ``overrides`` without a preset, even an empty mapping, runs as the
    whole chain, so ``overrides={}`` returns the input.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz.
        preset: Preset identifier from :func:`mastering_preset_names`; None runs
            ``overrides`` alone.
        overrides: Chain document params (:data:`MasteringChainConfig`), e.g.
            ``{"dynamics": {"compressor": {"thresholdDb": -24}},
            "loudness.targetLufs": -14}``, applied on top of the preset defaults
            when a preset is given. A boolean toggles a module/processor's
            ``enabled`` flag. The core reads it rooted at ``overrides``, so an
            unknown key, a path given twice or a null raises SonareValueError
            naming ``overrides.<path>``. The color stages ``saturation.tape`` and
            ``saturation.exciter`` are engaged from an override only when you
            pass ``enabled=True`` explicitly: on a preset where they are off,
            ``{"saturation": {"tape": {"driveDb": 6}}}`` alone has no audible
            effect; use ``{"saturation": {"tape": {"enabled": True, "driveDb": 6}}}``.
        on_progress: Optional callback ``(progress, stage)`` invoked after each
            enabled stage completes. ``progress`` is in ``[0.0, 1.0]`` and
            ``stage`` is the stage identifier (e.g. ``"dynamics.compressor"``).
            Its return value is ignored. An exception raised inside the callback
            stops the chain at its next cancellation point and is raised from
            this call.
        cancel: Optional zero-argument callable. A true return value requests
            cancellation at the next native cancellation point.

    Returns:
        :class:`MasteringChainResult` with processed samples, LUFS info, and the
        ordered list of stages that ran.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    audio = (c_array, _to_c_size_t(length, "length"), _to_c_int(sample_rate, "sample_rate"))
    if preset is None and overrides is None:
        raise _refuse("give preset or overrides")
    symbol = "sonare_master_audio"
    head = (None if preset is None else _utf8_arg(preset, "preset"), *audio)
    out = SonareMasteringChainResult()
    release = lib.sonare_free_mastering_chain_result
    _run_mastering(lib, symbol, head, overrides, on_progress, cancel, out, release)
    try:
        return MasteringChainResult(
            samples=_from_c_float_array(out.samples, out.length), **_chain_result_fields(out)
        )
    finally:
        release(ctypes.byref(out))


@_entry("master_audio_stereo")
@_guard_buffer("left", "right")
def master_audio_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int,
    *,
    preset: str | None = None,
    overrides: MasteringChainConfig | None = None,
    on_progress: Callable[[float, str], None] | None = None,
    cancel: Callable[[], bool] | None = None,
) -> MasteringChainStereoResult:
    """Master stereo audio with a named preset, a chain of overrides, or both.

    See :func:`master_audio` for the ``preset``, ``overrides``,
    ``on_progress``, and ``cancel`` semantics; neither given raises SonareValueError.
    The stereo chain also recognises the ``stereo.imager`` and
    ``stereo.monoMaker`` stages.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise _refuse("left and right must have the same length")
    audio = (
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
    )
    if preset is None and overrides is None:
        raise _refuse("give preset or overrides")
    symbol = "sonare_master_audio_stereo"
    head = (None if preset is None else _utf8_arg(preset, "preset"), *audio)
    out = SonareMasteringChainStereoResult()
    release = lib.sonare_free_mastering_chain_stereo_result
    _run_mastering(lib, symbol, head, overrides, on_progress, cancel, out, release)
    try:
        return MasteringChainStereoResult(
            left=_from_c_float_array(out.left, out.length),
            right=_from_c_float_array(out.right, out.length),
            **_chain_result_fields(out),
        )
    finally:
        release(ctypes.byref(out))
