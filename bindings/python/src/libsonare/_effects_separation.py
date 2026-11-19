"""Source separation: NMF decomposition, stems, and the HPSS family."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from dataclasses import dataclass
from typing import get_args

import numpy as np
import numpy.typing as npt

from ._effects_repair_common import _linked_channel_planes
from ._errors import _entry, _not_supported, _refuse, _refuse_type
from ._ffi import SonareDecomposeStemsConfig, SonareHpssResult
from ._narrowing import (
    _narrow_double,
    _narrow_int,
    _resolve_absent_positive,
    _resolve_optional_non_negative,
    _resolve_zero_default,
)
from ._runtime import (
    _C_INT_MAX,
    _DEFAULT_EFFECT_N_FFT,
    _call_float_array_transform,
    _check,
    _from_c_float_array,
    _from_c_int_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _out_int_array,
    _planar_channel_arrays,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _token_arg,
    _unsupported_effect_symbol,
    _utf8_arg,
    _validate_effect_hop,
    _validate_hpss_kernel,
    _validate_samples,
    _validate_stft_n_fft,
)
from .types import HpssResult, NmfInit, NnFilterAggregate

_NMF_INITS: tuple[str, ...] = get_args(NmfInit)
_NN_FILTER_AGGREGATES: tuple[str, ...] = get_args(NnFilterAggregate)

# ============================================================================
# Effects - decomposition / separation
# ============================================================================


@dataclass(frozen=True, slots=True, eq=False)
class DecomposeResult:
    """Result of :func:`decompose` and :func:`decompose_with_init`."""

    w: np.ndarray
    """Component matrix of shape ``(n_features, n_components)`` as float32."""
    h: np.ndarray
    """Activation matrix of shape ``(n_components, n_frames)`` as float32."""


@_entry("decompose")
@_guard_buffer("s")
def decompose(
    s: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    n_components: int,
    n_iter: int | None = None,
    beta: float = 2.0,
) -> DecomposeResult:
    """Non-negative matrix factorisation (librosa.decompose.decompose).

    Args:
        s: Input spectrogram, flattened row-major ``[n_features x n_frames]``.
        n_features: Feature dimension (rows). Must be greater than 0.
        n_frames: Number of time frames. Must be greater than 0.
        n_components: Target number of components (k). Must be greater than 0.
        n_iter: Number of multiplicative-update iterations; None takes the
            library default (50). Must be greater than 0 when given.
        beta: Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).

    Returns:
        A :class:`DecomposeResult` of float32 arrays: ``w`` is the component
        matrix of shape ``(n_features, n_components)`` and ``h`` is the
        activation matrix of shape ``(n_components, n_frames)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(s)
    if length != n_features * n_frames:
        raise _refuse("s length must equal n_features * n_frames")
    with (
        _out_float_array(lib) as (out_w, out_w_length),
        _out_float_array(lib) as (out_h, out_h_length),
    ):
        _check(
            lib.sonare_decompose(
                c_array,
                _to_c_int(n_features, "n_features"),
                _to_c_int(n_frames, "n_frames"),
                _to_c_int(n_components, "n_components"),
                _to_c_int(_resolve_zero_default(n_iter, "n_iter"), "n_iter"),
                _to_c_float(beta, "beta"),
                ctypes.byref(out_w),
                ctypes.byref(out_w_length),
                ctypes.byref(out_h),
                ctypes.byref(out_h_length),
            )
        )
        w = _from_c_float_array(out_w, out_w_length.value).reshape(n_features, n_components)
        h = _from_c_float_array(out_h, out_h_length.value).reshape(n_components, n_frames)
        return DecomposeResult(w=w, h=h)


@_entry("decompose_with_init")
@_guard_buffer("s")
def decompose_with_init(
    s: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    n_components: int,
    n_iter: int | None = None,
    beta: float = 2.0,
    init: NmfInit | None = None,
) -> DecomposeResult:
    """NMF with a selectable initialiser (librosa.decompose.decompose, ``init``).

    Identical to :func:`decompose` but exposes the initialisation strategy:
    ``"random"`` or ``"nndsvd"`` (the SVD-based warm start, which tends to
    converge in fewer iterations). Default: the library's (``"random"``). Any
    other string, the empty one included, is refused.

    Returns:
        A :class:`DecomposeResult` of float32 arrays: ``w`` of shape
        ``(n_features, n_components)`` and ``h`` of shape
        ``(n_components, n_frames)``.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_decompose_with_init"):
        raise _not_supported("libsonare was built without sonare_decompose_with_init")
    c_array, length = _to_c_float_array(s)
    if length != n_features * n_frames:
        raise _refuse("s length must equal n_features * n_frames")
    with (
        _out_float_array(lib) as (out_w, out_w_length),
        _out_float_array(lib) as (out_h, out_h_length),
    ):
        _check(
            lib.sonare_decompose_with_init(
                c_array,
                _to_c_int(n_features, "n_features"),
                _to_c_int(n_frames, "n_frames"),
                _to_c_int(n_components, "n_components"),
                _to_c_int(_resolve_zero_default(n_iter, "n_iter"), "n_iter"),
                _to_c_float(beta, "beta"),
                None if init is None else _utf8_arg(_token_arg(init, _NMF_INITS, "init"), "init"),
                ctypes.byref(out_w),
                ctypes.byref(out_w_length),
                ctypes.byref(out_h),
                ctypes.byref(out_h_length),
            )
        )
        w = _from_c_float_array(out_w, out_w_length.value).reshape(n_features, n_components)
        h = _from_c_float_array(out_h, out_h_length.value).reshape(n_components, n_frames)
        return DecomposeResult(w=w, h=h)


def _stems_config(
    n_components: int | None,
    n_fft: int | None,
    hop_length: int | None,
    n_iter: int | None,
    beta: float | None,
    init: NmfInit | None,
    mask_power: float | None,
) -> SonareDecomposeStemsConfig:
    """Build a stems config; an omitted value is the C field's 0 / NULL, the core's default.

    A given count is narrowed before its positivity check: int(0.5) is the 0
    sentinel, so a fraction would run at the default and report success. The C
    reader treats "" as NULL, so the init token is checked here.
    """
    counts = {"n_components": n_components, "n_iter": n_iter}
    c_counts = {name: 0 for name in counts}
    for name, value in counts.items():
        if value is not None:
            c_counts[name] = _narrow_int(value, name, 1, _C_INT_MAX)
    c_mask_power = 0.0
    if mask_power is not None:
        c_mask_power = _narrow_double(mask_power, "mask_power")
        if c_mask_power < 1.0:
            raise _refuse("mask_power must be at least 1")
    return SonareDecomposeStemsConfig(
        struct_version=1,
        n_components=c_counts["n_components"],
        n_fft=0 if n_fft is None else _validate_stft_n_fft(n_fft),
        hop_length=0 if hop_length is None else _validate_effect_hop(hop_length),
        n_iter=c_counts["n_iter"],
        beta=0.0 if beta is None else _narrow_double(beta, "beta"),
        init=None if init is None else _utf8_arg(_token_arg(init, _NMF_INITS, "init"), "init"),
        mask_power=c_mask_power,
    )


@_entry("decompose_stems")
def decompose_stems(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_components: int | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
    n_iter: int | None = None,
    beta: float | None = None,
    init: NmfInit | None = None,
    mask_power: float | None = None,
) -> dict[str, object]:
    """NMF separation that carries the original phase, so components are listenable.

    :func:`decompose` returns the W / H factors of a magnitude spectrogram,
    which have no phase; reconstructing from them needs a phase estimator, and
    an estimated phase does not hold up as a stem. This builds a per-component
    soft mask from the factorisation and applies it to the original complex
    spectrogram. The masks sum to one wherever the model has energy and the
    inverse STFT is linear, so the components sum back to the input.

    Args:
        samples: Input signal (mono).
        sample_rate: Sample rate in Hz.
        n_components: Number of NMF components. Default: the library's (4).
        n_fft: STFT size. Default: the library's (2048).
        hop_length: STFT hop. Default: ``n_fft / 4``, derived by the library.
        n_iter: NMF multiplicative-update iterations. Default: the library's (100).
        beta: Beta divergence (2 = Frobenius, 1 = Kullback-Leibler). Default:
            the library's (2.0).
        init: NMF initialisation, ``"random"`` or ``"nndsvd"``; any other
            string, the empty one included, is refused. Default: the library's
            (``"random"``).
        mask_power: Soft-mask exponent, at least 1. Default: the library's
            (1.0). 1 keeps the magnitude ratio; 2 is the Wiener-style power
            ratio, which separates harder at the cost of more artefacts on
            overlapping partials.

    Returns:
        Dict with ``components`` (list of 1-D float32 arrays, each the length of
        the input), ``w`` (``(n_bins, n_components)``), ``h``
        (``(n_components, n_frames)``) and ``sample_rate``.

    Raises:
        SonareValueError: If a given ``n_components``, ``n_iter``, ``n_fft`` or
            ``hop_length`` is below 1, ``mask_power`` is below 1, or
            ``init`` is not a known token. Leaving a value out is spelled
            ``None``, so 0 is refused rather than read as the C ABI's default.
        SonareError: If the library rejects a value that reaches it, such as a
            non-finite ``beta`` or ``mask_power``.
    """
    _validate_samples(samples)
    config = _stems_config(n_components, n_fft, hop_length, n_iter, beta, init, mask_power)
    lib = _get_lib()
    if not hasattr(lib, "sonare_decompose_stems"):
        raise _unsupported_effect_symbol("sonare_decompose_stems")
    c_array, length = _to_c_float_array(samples)
    component_count = ctypes.c_size_t()
    with (
        _out_float_array(lib) as (out, component_length),
        _out_float_array(lib) as (out_w, out_w_length),
        _out_float_array(lib) as (out_h, out_h_length),
    ):
        _check(
            lib.sonare_decompose_stems(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(component_count),
                ctypes.byref(component_length),
                ctypes.byref(out_w),
                ctypes.byref(out_w_length),
                ctypes.byref(out_h),
                ctypes.byref(out_h_length),
            )
        )
        count = component_count.value
        per = component_length.value
        flat = _from_c_float_array(out, count * per)
        # Non-overlapping views of one owned copy, as the linked form returns.
        components = [flat[i * per : (i + 1) * per] for i in range(count)]
        w = _from_c_float_array(out_w, out_w_length.value)
        h = _from_c_float_array(out_h, out_h_length.value)
        return {
            "components": components,
            "w": w.reshape(-1, count) if count else w,
            "h": h.reshape(count, -1) if count else h,
            "sample_rate": sample_rate,
        }


@_entry("decompose_stems_linked")
def decompose_stems_linked(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int,
    n_components: int | None = None,
    n_fft: int | None = None,
    hop_length: int | None = None,
    n_iter: int | None = None,
    beta: float | None = None,
    init: NmfInit | None = None,
    mask_power: float | None = None,
) -> dict[str, object]:
    """N-channel form of :func:`decompose_stems`: one NMF model, one soft mask.

    Each channel's STFT is averaged into a single magnitude plane before the
    factorisation, and the resulting per-component soft mask is applied
    UNCHANGED to every channel's own complex spectrum, so no interchannel
    level or phase difference moves -- the same guarantee
    :func:`mastering_repair_denoise_classical_linked` gives its channel set.

    A single channel reproduces :func:`decompose_stems` bit for bit: the
    average over one channel is a division by 1, which does not change the
    bits.

    Args:
        channels: One input buffer per channel, or a 2-D ``(channels, frames)``
            array. All channels must be the same length; the C form has one
            length for the set. At least one and at most 64 channels.
        sample_rate: Sample rate in Hz, shared by every channel.
        n_components: Number of NMF components. Default: the library's (4).
        n_fft: STFT size. Default: the library's (2048).
        hop_length: STFT hop. Default: ``n_fft / 4``, derived by the library.
        n_iter: NMF multiplicative-update iterations. Default: the library's (100).
        beta: Beta divergence (2 = Frobenius, 1 = Kullback-Leibler). Default:
            the library's (2.0).
        init: NMF initialisation, ``"random"`` or ``"nndsvd"``; any other
            string, the empty one included, is refused. Default: the library's
            (``"random"``).
        mask_power: Soft-mask exponent, at least 1. Default: the library's
            (1.0). 1 keeps the magnitude ratio; 2 is the Wiener-style power
            ratio.

    Returns:
        Dict with the same keys as :func:`decompose_stems` -- ``components``,
        ``w``, ``h`` and ``sample_rate`` -- but each ``components`` element is
        channel-planar: a 2-D float32 array of shape ``(num_channels, n)``
        rather than :func:`decompose_stems`'s 1-D array, component c channel
        ch at row ch.

    Raises:
        SonareValueError: If ``channels`` is empty or its planes disagree in
            length, if a given ``n_components``, ``n_iter``, ``n_fft`` or
            ``hop_length`` is below 1, ``mask_power`` is below 1, or
            ``init`` is not a known token.
        SonareError: If the library rejects a value that reaches it, such as a
            non-finite ``beta`` or ``mask_power``, or more than 64 channels.
    """
    config = _stems_config(n_components, n_fft, hop_length, n_iter, beta, init, mask_power)
    lib = _get_lib()
    if not hasattr(lib, "sonare_decompose_stems_linked"):
        raise _unsupported_effect_symbol("sonare_decompose_stems_linked")
    planes = _linked_channel_planes(channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    channel_count = len(arrays)
    component_count = ctypes.c_size_t()
    out_channel_count = ctypes.c_size_t()
    with (
        _out_float_array(lib) as (out, component_length),
        _out_float_array(lib) as (out_w, out_w_length),
        _out_float_array(lib) as (out_h, out_h_length),
    ):
        _check(
            lib.sonare_decompose_stems_linked(
                ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
                _to_c_size_t(channel_count, "channel_count"),
                _to_c_size_t(frame_count, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(component_count),
                ctypes.byref(out_channel_count),
                ctypes.byref(component_length),
                ctypes.byref(out_w),
                ctypes.byref(out_w_length),
                ctypes.byref(out_h),
                ctypes.byref(out_h_length),
            )
        )
        count = component_count.value
        ch = out_channel_count.value
        per = component_length.value
        flat = _from_c_float_array(out, count * ch * per)
        components = [
            flat[i * ch * per : (i + 1) * ch * per].reshape(ch, per) for i in range(count)
        ]
        w = _from_c_float_array(out_w, out_w_length.value)
        h = _from_c_float_array(out_h, out_h_length.value)
        return {
            "components": components,
            "w": w.reshape(-1, count) if count else w,
            "h": h.reshape(count, -1) if count else h,
            "sample_rate": sample_rate,
        }


@_entry("nn_filter")
@_guard_buffer("s")
def nn_filter(
    s: Sequence[float] | list[float],
    n_features: int,
    n_frames: int,
    aggregate: NnFilterAggregate | None = None,
    k: int | None = None,
    width: int | None = None,
) -> np.ndarray:
    """Nearest-neighbour spectrogram filter (librosa.decompose.nn_filter).

    Args:
        s: Input spectrogram, flattened row-major ``[n_features x n_frames]``.
        n_features: Feature dimension (rows). Must be greater than 0.
        n_frames: Number of time frames. Must be greater than 0.
        aggregate: Aggregator: ``"mean"``, ``"median"``, ``"min"`` or ``"max"``;
            any other string, the empty one included, is refused. Default:
            ``"mean"`` (the library's).
        k: Number of nearest neighbours; None takes the library default (7).
            Must be greater than 0 when given.
        width: Time exclusion half-width; None takes the library default (1).
            Must be at least 0 when given.

    Returns:
        The smoothed spectrogram as a float32 array of shape
        ``(n_features, n_frames)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(s)
    if length != n_features * n_frames:
        raise _refuse("s length must equal n_features * n_frames")
    aggregate_bytes = (
        None
        if aggregate is None
        else _utf8_arg(_token_arg(aggregate, _NN_FILTER_AGGREGATES, "aggregate"), "aggregate")
    )
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_nn_filter(
                c_array,
                _to_c_int(n_features, "n_features"),
                _to_c_int(n_frames, "n_frames"),
                aggregate_bytes,
                _to_c_int(_resolve_absent_positive(k, "k"), "k"),
                _to_c_int(_resolve_optional_non_negative(width, "width"), "width"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value).reshape(n_features, n_frames)


def _remix_interval_array(
    intervals: npt.ArrayLike,
) -> tuple[ctypes.Array[ctypes.c_int32], int]:
    """Marshal an ``(n, 2)`` array of ``(start, end)`` rows, refusing any other shape."""
    shape = np.shape(intervals)
    if len(shape) != 2 or shape[1] != 2:
        raise _refuse("intervals must have shape (n, 2): one (start, end) row per slice")
    array, n_ints = _to_c_int_array(intervals, "intervals")
    return array, n_ints // 2


@_entry("remix")
@_guard_buffer("samples")
def remix(
    samples: Sequence[float] | list[float],
    intervals: npt.ArrayLike,
    sr: int = 22050,
    align_zeros: bool = True,
) -> np.ndarray:
    """Reorder / concatenate a signal by interval slices (librosa.effects.remix).

    With ``align_zeros`` the boundaries snap to the signal's zero-crossings,
    which is a per-signal decision: calling this per channel snaps each channel
    to a different frame and drifts a stereo take apart. Resolve one cut set
    with :func:`remix_aligned_intervals` and apply it to every channel instead.

    Args:
        samples: Input signal.
        intervals: ``(n, 2)`` array of ``(start, end)`` sample rows.
        sr: Sample rate in Hz (default 22050).
        align_zeros: Snap slice boundaries to zero-crossings (default True).

    Returns:
        The remixed signal as a 1-D float32 array.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    intervals_array, n_pairs = _remix_interval_array(intervals)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_remix(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sr, "sr"),
                intervals_array,
                ctypes.c_size_t(n_pairs),
                ctypes.c_int(1 if align_zeros else 0),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("remix_aligned_intervals")
@_guard_buffer("samples")
def remix_aligned_intervals(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    intervals: npt.ArrayLike,
    align_zeros: bool = True,
) -> np.ndarray:
    """Resolve the cut points :func:`remix` would use, without cutting.

    Returns one clamped ``(start, end)`` row per input interval.
    With ``align_zeros`` each boundary snaps to the nearest zero-crossing, with
    two guards that stop a slice from vanishing: a signal with no sign change at
    all (silence, a DC offset, any constant) is not snapped, and a slice that
    had content but collapses to empty after snapping keeps its unsnapped
    boundaries.

    Use this to cut a multichannel take on one common frame set: resolve once
    from one channel, then slice every channel with the returned pairs.

    Args:
        samples: Input signal.
        intervals: ``(n, 2)`` array of ``(start, end)`` sample rows.
        sample_rate: Sample rate in Hz.
        align_zeros: Snap slice boundaries to zero-crossings (default True).

    Returns:
        ``int32`` array of shape ``(n, 2)``, one resolved ``(start, end)`` row
        per input interval.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_remix_aligned_intervals"):
        raise _not_supported("loaded libsonare does not expose sonare_remix_aligned_intervals")
    c_array, length = _to_c_float_array(samples)
    intervals_array, n_pairs = _remix_interval_array(intervals)
    with _out_int_array(lib) as (out, out_count):
        _check(
            lib.sonare_remix_aligned_intervals(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                intervals_array,
                ctypes.c_size_t(n_pairs),
                ctypes.c_int(1 if align_zeros else 0),
                ctypes.byref(out),
                ctypes.byref(out_count),
            )
        )
        return _from_c_int_array(out, out_count.value * 2).reshape(-1, 2)


@_entry("hpss")
def hpss(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int | None = None,
    hard_mask: bool = False,
) -> HpssResult:
    """Perform harmonic-percussive source separation.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        kernel_harmonic: Harmonic median filter kernel size, in STFT frames: a
            positive odd integer at most 524287. The ceiling is 524288 and an
            even kernel is refused, so 524287 is the largest legal value.
        kernel_percussive: Percussive median filter kernel size, in STFT bins,
            under the same rule.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 4
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window. Default: ``n_fft / 4``,
            derived by the library.
        hard_mask: Use binary harmonic/percussive masks (default ``False``).

    Returns:
        HpssResult with harmonic and percussive components.
    """
    _validate_samples(samples)
    n_fft = _validate_stft_n_fft(n_fft)
    hop_length = 0 if hop_length is None else _validate_effect_hop(hop_length)
    kernel_harmonic = _validate_hpss_kernel(kernel_harmonic, "kernel_harmonic")
    kernel_percussive = _validate_hpss_kernel(kernel_percussive, "kernel_percussive")
    if not isinstance(hard_mask, bool):
        raise _refuse_type("hard_mask must be a bool")

    lib = _get_lib()
    use_soft_mask = 0 if hard_mask else 1
    if not hasattr(lib, "sonare_hpss_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != 0 or hard_mask:
            raise _unsupported_effect_symbol("sonare_hpss_ex")
        if not hasattr(lib, "sonare_hpss"):
            raise _unsupported_effect_symbol("sonare_hpss")
        return _hpss_legacy(lib, samples, sample_rate, kernel_harmonic, kernel_percussive)

    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss_ex(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(use_soft_mask, "use_soft_mask"),
        ctypes.c_int(0),
        ctypes.byref(out),
        None,
    )
    _check(rc)
    try:
        n = out.length
        return HpssResult(
            harmonic=_from_c_float_array(out.harmonic, n),
            percussive=_from_c_float_array(out.percussive, n),
            length=int(n),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))


def _hpss_legacy(
    lib: ctypes.CDLL,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int,
    kernel_percussive: int,
) -> HpssResult:
    """Call the pre-extended HPSS entry point for legacy-default requests."""
    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.length
        return HpssResult(
            harmonic=_from_c_float_array(out.harmonic, n),
            percussive=_from_c_float_array(out.percussive, n),
            length=int(n),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))


@_entry("harmonic")
def harmonic(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> np.ndarray:
    """Extract the harmonic component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.

    Returns:
        ``float32`` array of harmonic component samples.
    """
    _validate_samples(samples)
    return _call_float_array_transform(
        "sonare_harmonic", samples, _to_c_int(sample_rate, "sample_rate")
    )


@_entry("percussive")
def percussive(
    samples: Sequence[float] | list[float],
    sample_rate: int,
) -> np.ndarray:
    """Extract the percussive component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.

    Returns:
        ``float32`` array of percussive component samples.
    """
    _validate_samples(samples)
    return _call_float_array_transform(
        "sonare_percussive", samples, _to_c_int(sample_rate, "sample_rate")
    )


@_entry("hpss_with_residual")
def hpss_with_residual(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int | None = None,
    hard_mask: bool = False,
) -> dict[str, object]:
    """HPSS into harmonic / percussive / residual signals.

    The three outputs always add back up to the input. ``residual`` is silent
    under the default soft mask, whose two masks sum to one, so ``harmonic`` and
    ``percussive`` already carry everything; it is returned anyway so the result
    shape does not change with the mask. Pass ``hard_mask=True`` for a residual
    that holds signal -- its thresholded masks leave the band where neither
    component dominates, measured at 3% of the input energy on a voice-plus-kick
    signal.

    Example:
        >>> soft = hpss_with_residual(samples, sample_rate)
        >>> # soft["residual"] is silence
        >>> hard = hpss_with_residual(samples, sample_rate, hard_mask=True)
        >>> # hard["residual"] carries what neither component claimed

    Args:
        samples: Input audio.
        sample_rate: Sample rate in Hz.
        kernel_harmonic: Horizontal median filter size, in STFT frames: a
            positive odd integer at most 524287. The ceiling is 524288 and an
            even kernel is refused, so 524287 is the largest legal value.
        kernel_percussive: Vertical median filter size, in STFT bins, under the
            same rule.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 4
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window. Default: ``n_fft / 4``,
            derived by the library.
        hard_mask: Use binary harmonic/percussive masks (default ``False``).

    Returns:
        A dict with ``harmonic`` / ``percussive`` / ``residual`` float32 arrays
        (all the same length) plus ``sampleRate``.
    """
    _validate_samples(samples)
    n_fft = _validate_stft_n_fft(n_fft)
    hop_length = 0 if hop_length is None else _validate_effect_hop(hop_length)
    kernel_harmonic = _validate_hpss_kernel(kernel_harmonic, "kernel_harmonic")
    kernel_percussive = _validate_hpss_kernel(kernel_percussive, "kernel_percussive")
    if not isinstance(hard_mask, bool):
        raise _refuse_type("hard_mask must be a bool")

    lib = _get_lib()
    use_soft_mask = 0 if hard_mask else 1
    if not hasattr(lib, "sonare_hpss_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != 0 or hard_mask:
            raise _unsupported_effect_symbol("sonare_hpss_ex")
        if not hasattr(lib, "sonare_hpss_with_residual"):
            raise _unsupported_effect_symbol("sonare_hpss_with_residual")
        return _hpss_with_residual_legacy(
            lib, samples, sample_rate, kernel_harmonic, kernel_percussive
        )

    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    residual = ctypes.POINTER(ctypes.c_float)()
    rc = lib.sonare_hpss_ex(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(use_soft_mask, "use_soft_mask"),
        ctypes.c_int(1),
        ctypes.byref(out),
        ctypes.byref(residual),
    )
    try:
        _check(rc)
        n = int(out.length)
        return {
            "harmonic": _from_c_float_array(out.harmonic, n),
            "percussive": _from_c_float_array(out.percussive, n),
            "residual": _from_c_float_array(residual, n),
            "sampleRate": int(out.sample_rate),
        }
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))
        if residual:
            lib.sonare_free_floats(residual)


def _hpss_with_residual_legacy(
    lib: ctypes.CDLL,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int,
    kernel_percussive: int,
) -> dict[str, object]:
    """Call the pre-extended three-way HPSS entry point."""
    c_array, length = _to_c_float_array(samples)
    with (
        _out_float_array(lib) as (out_harmonic, out_length),
        _out_float_array(lib) as (out_percussive, _),
        _out_float_array(lib) as (out_residual, _),
    ):
        out_sample_rate = ctypes.c_int()
        _check(
            lib.sonare_hpss_with_residual(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_int(kernel_harmonic, "kernel_harmonic"),
                _to_c_int(kernel_percussive, "kernel_percussive"),
                ctypes.byref(out_harmonic),
                ctypes.byref(out_percussive),
                ctypes.byref(out_residual),
                ctypes.byref(out_length),
                ctypes.byref(out_sample_rate),
            )
        )
        n = out_length.value
        return {
            "harmonic": _from_c_float_array(out_harmonic, n),
            "percussive": _from_c_float_array(out_percussive, n),
            "residual": _from_c_float_array(out_residual, n),
            "sampleRate": int(out_sample_rate.value),
        }


@_entry("phase_vocoder")
def phase_vocoder(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    speed_ratio: float,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Phase-vocoder time-scale modification (STFT -> phase_vocoder -> iSTFT).

    Args:
        samples: Input audio.
        sample_rate: Sample rate in Hz.
        speed_ratio: Playback speed (< 1.0 slower and longer, > 1.0 faster and
            shorter); required, greater than 0. The opposite sense of :func:`note_stretch`'s
            ``stretch_ratio``, a duration ratio.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 4
            (default 2048).
        hop_length: Hop length used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window (default 512).

    Returns:
        The time-stretched audio as a 1-D float32 array.
    """
    _validate_samples(samples)
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_phase_vocoder(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _to_c_float(speed_ratio, "speed_ratio"),
                _to_c_int(n_fft, "n_fft"),
                _to_c_int(hop_length, "hop_length"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)
