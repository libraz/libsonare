"""Broadband and tonal noise repair: denoise and dehum."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._effects_repair_common import (
    _linked_channel_planes,
    _linked_output_planes,
    _run_detection,
    _run_repair,
)
from ._ffi import (
    SONARE_DEHUM_MODE_NOTCH,
    SONARE_DEHUM_MODE_SUBTRACT,
    SONARE_DENOISE_MODE_LOG_MMSE,
    SONARE_DENOISE_MODE_MMSE_STSA,
    SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,
    SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA,
    SONARE_DENOISE_NOISE_ESTIMATOR_MCRA,
    SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,
    SONARE_DENOISE_NOISE_ESTIMATOR_SPP,
    SONARE_REPAIR_NOISE_BAND_COUNT,
    SonareDehumConfig,
    SonareDehumStereoResult,
    SonareDenoiseClassicalConfig,
    SonareDenoiseReport,
    SonareDenoiseStereoResult,
    SonareHumDetection,
    SonareNoiseDetection,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    SonareValueError,
    _as_float32_buffer,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _planar_channel_arrays,
    _require_power_of_two,
    _resolve_enum,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _unsupported_effect_symbol,
)
from .types import (
    DehumReport,
    DehumStereoResult,
    DenoiseLinkedResult,
    DenoiseReport,
    DenoiseStereoResult,
    HumDetection,
    NoiseDetection,
)

_DENOISE_MODE_NAMES = {
    "logmmse": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "log_mmse": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "lsa": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "mmsestsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "mmse_stsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "stsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "spectralsubtraction": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
    "spectral_subtraction": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
    "ss": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
}


_DENOISE_ESTIMATOR_NAMES = {
    "quantile": SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,  # noqa: F405
    "mcra": SONARE_DENOISE_NOISE_ESTIMATOR_MCRA,  # noqa: F405
    "imcra": SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA,  # noqa: F405
    "spp": SONARE_DENOISE_NOISE_ESTIMATOR_SPP,  # noqa: F405
}


def _coerce_denoise_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DENOISE_MODE_NAMES,
        "denoise mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


def _coerce_denoise_estimator(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DENOISE_ESTIMATOR_NAMES,
        "denoise noise estimator",
        underscore=True,
        strip=True,
        validate_int=True,
    )


@_guard_buffer("samples")
def mastering_repair_denoise_classical(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> np.ndarray:
    """Offline STFT-domain classical denoiser.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        mode: ``"logMmse"`` (default), ``"mmseStsa"``, or ``"spectralSubtraction"``;
              an integer in ``SONARE_DENOISE_MODE_*`` is also accepted.
        noise_estimator: ``"quantile"`` (default), ``"mcra"``, ``"imcra"``, or ``"spp"``.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples (default 256).
        dd_alpha: Decision-directed a priori SNR smoothing (default 0.98).
        reduction_db: Deepest attenuation the mask may apply, in dB, >= 0
            (default 26.0).
        over_subtraction: Berouti alpha; SpectralSubtraction only (default 2.0).
        spectral_floor: Berouti beta; SpectralSubtraction only (default 0.05).
        noise_estimation_quantile: Fraction of frames assumed noise-only (default 0.1).
        speech_presence_gain: Apply speech-presence probability gating (default True).
        gain_smoothing: Smooth gains across time (default True).

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``mode`` / ``noise_estimator`` cannot be resolved.
        RuntimeError: If the C call rejects the request (e.g. non-power-of-two
        ``n_fft`` or non-positive ``hop_length``).
    """
    # The core requires a power of two here (denoise_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    # Narrowed ahead of the sign check: int() takes 256.7 as 256, a hop nobody asked for.
    hop_length_value = _narrow_int(
        hop_length, "mastering_repair_denoise_classical: hop_length", _C_INT_MIN, _C_INT_MAX
    )
    if hop_length_value <= 0:
        raise SonareValueError("hop_length must be positive")

    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    config = SonareDenoiseClassicalConfig(  # noqa: F405
        mode=_coerce_denoise_mode(mode),
        noise_estimator=_coerce_denoise_estimator(noise_estimator),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        dd_alpha=float(dd_alpha),
        reduction_db=float(reduction_db),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        noise_estimation_quantile=float(noise_estimation_quantile),
        speech_presence_gain=1 if speech_presence_gain else 0,
        gain_smoothing=1 if gain_smoothing else 0,
    )
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_mastering_repair_denoise_classical(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


def _extract_noise_detection(raw: Any) -> NoiseDetection:
    return NoiseDetection(
        floor_dbfs=float(raw.floor_dbfs),
        band_floor_dbfs=[float(v) for v in raw.band_floor_dbfs],
    )


@_guard_buffer("samples")
def mastering_repair_detect_noise_floor(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> NoiseDetection:
    """Measure the noise floor without denoising.

    Runs the STFT and the configured noise estimator -- the two stages
    :func:`mastering_repair_denoise_classical` runs -- and stops before the gain
    mask, which is why the attenuation figures live on the denoise report rather
    than here::

        floor = libsonare.mastering_repair_detect_noise_floor(samples, 44100)
        print(floor.floor_dbfs, floor.band_floor_dbfs[0])

    Needs at least ``n_fft`` samples and REFUSES a shorter buffer, unlike
    :func:`mastering_repair_detect_reverb`, which pads one.

    ``band_floor_dbfs`` has 32 entries on a geometric grid from 20 Hz to Nyquist,
    low to high -- the same axis the mastering report's band energy deltas use.
    Every level is absolute, so compare a figure only against one measured over
    the same channel count.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        mode: ``"logMmse"`` (default), ``"mmseStsa"``, or ``"spectralSubtraction"``;
              an integer in ``SONARE_DENOISE_MODE_*`` is also accepted.
        noise_estimator: ``"quantile"`` (default), ``"mcra"``, ``"imcra"``, or ``"spp"``.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples (default 256).
        dd_alpha: Decision-directed a priori SNR smoothing (default 0.98).
        reduction_db: Deepest attenuation the mask may apply, in dB (default 26.0).
        over_subtraction: Berouti alpha; SpectralSubtraction only (default 2.0).
        spectral_floor: Berouti beta; SpectralSubtraction only (default 0.05).
        noise_estimation_quantile: Fraction of frames assumed noise-only (default 0.1).
        speech_presence_gain: Apply speech-presence probability gating (default True).
        gain_smoothing: Smooth gains across time (default True).

    Returns:
        :class:`~libsonare.types.NoiseDetection`.

    Raises:
        SonareValueError: If ``mode`` / ``noise_estimator`` cannot be resolved,
            if ``n_fft`` is not a power of two, if ``hop_length`` is not
            positive, or if the buffer is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request, which includes a buffer
            shorter than ``n_fft``.
    """
    # The core requires a power of two here (denoise_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    hop_length_value = _narrow_int(
        hop_length, "mastering_repair_detect_noise_floor: hop_length", _C_INT_MIN, _C_INT_MAX
    )
    if hop_length_value <= 0:
        raise SonareValueError("hop_length must be positive")
    config = SonareDenoiseClassicalConfig(  # noqa: F405
        mode=_coerce_denoise_mode(mode),
        noise_estimator=_coerce_denoise_estimator(noise_estimator),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        dd_alpha=float(dd_alpha),
        reduction_db=float(reduction_db),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        noise_estimation_quantile=float(noise_estimation_quantile),
        speech_presence_gain=1 if speech_presence_gain else 0,
        gain_smoothing=1 if gain_smoothing else 0,
    )
    return _extract_noise_detection(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_noise_floor,
            samples,
            sample_rate,
            config,
            SonareNoiseDetection,  # noqa: F405
        )
    )


# One more than the band count: the first bin of every band plus the
# one-past-the-end bin of the last.
_NOISE_BAND_EDGE_COUNT = SONARE_REPAIR_NOISE_BAND_COUNT + 1


def mastering_repair_noise_band_bins(n_fft: int = 1024, sample_rate: int = 22050) -> list[int]:
    """Bin boundaries of the grid a denoise noise floor is reported on.

    Band ``k`` of :attr:`~libsonare.types.NoiseDetection.band_floor_dbfs` covers
    the one-sided STFT bins ``[bins[k], bins[k + 1])``, and bin ``b`` sits at
    ``b * sample_rate / n_fft`` Hz::

        bins = libsonare.mastering_repair_noise_band_bins(n_fft=1024, sample_rate=48000)
        floor = libsonare.mastering_repair_detect_noise_floor(samples, 48000, n_fft=1024)
        for k, level in enumerate(floor.band_floor_dbfs):
            if bins[k] == bins[k + 1]:
                continue  # empty band: `level` is the sentinel, not a measurement
            print(bins[k] * 48000 / 1024, level)

    The geometric band edges are rounded to bins, so a band narrower than the
    bin spacing comes out EMPTY -- ``bins[k] == bins[k + 1]`` -- and its level
    reads as the floor sentinel because no bin landed in it, not because that
    region was quiet. Telling those two apart is what this grid is for, and it
    is why the rounding cannot be recovered from the band count alone.

    Nothing but the analysis geometry decides the grid, so no denoise config is
    taken: one call describes every floor measured at that ``n_fft`` and
    ``sample_rate``, whatever mode or estimator produced it.

    Args:
        n_fft: STFT size the bins belong to; a positive power of two, the same
            rule :func:`mastering_repair_detect_noise_floor` applies to its
            config, so every grid returned here is one that entry can report on
            (default 1024).
        sample_rate: Sample rate the bins belong to, in Hz; positive
            (default 22050).

    Returns:
        33 bin indices, low to high -- one more than the 32 bands: the first bin
        of every band plus the one-past-the-end bin of the last, which is
        ``n_fft // 2 + 1``. The sequence is non-decreasing.

    Raises:
        SonareValueError: If ``n_fft`` is not a positive power of two.
        SonareError: If the C call rejects the request, which includes a
            non-positive ``sample_rate``.
    """
    # The same power-of-two rule the denoise config validator applies; check it
    # eagerly so the message names the argument.
    _require_power_of_two(n_fft, "n_fft")
    out_bins = (ctypes.c_int * _NOISE_BAND_EDGE_COUNT)()
    rc = _get_lib().sonare_mastering_repair_noise_band_bins(
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(sample_rate, "sample_rate"),
        out_bins,
    )
    _check(rc)
    return [int(v) for v in out_bins]


def _extract_denoise_report(raw: Any) -> DenoiseReport:
    return DenoiseReport(
        detected=_extract_noise_detection(raw.detected),
        mean_reduction_db=float(raw.mean_reduction_db),
        max_reduction_db=float(raw.max_reduction_db),
        floor_limited_fraction=float(raw.floor_limited_fraction),
    )


@_guard_buffer("left", "right")
def mastering_repair_denoise_classical_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> DenoiseStereoResult:
    """Denoises a stereo pair with one channel-linked gain mask.

    The mask is built from the channel-summed power and applied unchanged to
    both channels, so the pass cannot move an interchannel level or phase
    difference. That is also why the result carries one ``report`` rather
    than one per channel: a pair would be two copies of one measurement and
    would read as though the two could differ.

    ``report.detected`` is the one part that depends on the channel count.
    Its levels are absolute dBFS measured on the channel-summed power, so two
    identical channels read about 3 dB above the same material through
    :func:`mastering_repair_denoise_classical`; compare a stereo floor only
    against another stereo floor.

    Needs at least ``n_fft`` samples and rejects a shorter input, unlike
    :func:`mastering_repair_dereverb_classical_stereo`, which pads one.

    Which config fields are live depends on ``mode``: ``over_subtraction``
    and ``spectral_floor`` are read only by ``"spectralSubtraction"``, and
    ``speech_presence_gain`` and ``gain_smoothing`` only by the other two, so
    at the default mode the first pair does nothing.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        mode: ``"logMmse"`` (default), ``"mmseStsa"``, or ``"spectralSubtraction"``;
              an integer in ``SONARE_DENOISE_MODE_*`` is also accepted.
        noise_estimator: ``"quantile"`` (default), ``"mcra"``, ``"imcra"``, or ``"spp"``.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples (default 256).
        dd_alpha: Decision-directed a priori SNR smoothing (default 0.98).
        reduction_db: Deepest attenuation the mask may apply, in dB, >= 0
            (default 26.0).
        over_subtraction: Berouti alpha; SpectralSubtraction only (default 2.0).
        spectral_floor: Berouti beta; SpectralSubtraction only (default 0.05).
        noise_estimation_quantile: Fraction of frames assumed noise-only (default 0.1).
        speech_presence_gain: Apply speech-presence probability gating (default True).
        gain_smoothing: Smooth gains across time (default True).

    Returns:
        :class:`DenoiseStereoResult` with the denoised channels and the one
        report the shared mask produced.

    Raises:
        SonareValueError: If ``mode`` / ``noise_estimator`` cannot be resolved,
            if ``n_fft`` is not a power of two, if ``hop_length`` is not
            positive, or if the two channels differ in length.
        SonareError: If the C call rejects the request, which includes an input
            shorter than ``n_fft``.
    """
    # The core requires a power of two here (denoise_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    hop_length_value = _narrow_int(
        hop_length,
        "mastering_repair_denoise_classical_stereo: hop_length",
        _C_INT_MIN,
        _C_INT_MAX,
    )
    if hop_length_value <= 0:
        raise SonareValueError("hop_length must be positive")

    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDenoiseClassicalConfig(  # noqa: F405
        mode=_coerce_denoise_mode(mode),
        noise_estimator=_coerce_denoise_estimator(noise_estimator),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        dd_alpha=float(dd_alpha),
        reduction_db=float(reduction_db),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        noise_estimation_quantile=float(noise_estimation_quantile),
        speech_presence_gain=1 if speech_presence_gain else 0,
        gain_smoothing=1 if gain_smoothing else 0,
    )
    out = SonareDenoiseStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_denoise_classical_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        n = int(out.length)
        return DenoiseStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            report=_extract_denoise_report(out.report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDenoiseStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)


def mastering_repair_denoise_classical_linked(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    reduction_db: float = 26.0,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> DenoiseLinkedResult:
    """Denoises any number of channels with one channel-linked gain mask.

    The N-channel form of
    :func:`mastering_repair_denoise_classical_stereo`, carrying the same
    guarantee for the whole set: one mask built from the channel-summed power
    and applied unchanged to every channel, so no interchannel level or phase
    difference moves however many channels there are. That is also why the
    result carries one ``report`` rather than one per channel.

    A single channel reproduces :func:`mastering_repair_denoise_classical`
    bit for bit, and two channels reproduce
    :func:`mastering_repair_denoise_classical_stereo` plane for plane, with
    ``channels[0]`` the left and ``channels[1]`` the right::

        result = libsonare.mastering_repair_denoise_classical_linked(
            [left, right, centre], 44100
        )
        assert len(result.channels) == 3

    Needs at least ``n_fft`` samples and rejects a shorter input, unlike
    :func:`mastering_repair_dereverb_classical_linked`, which pads one.

    ``report.detected`` is the one part that moves with the channel count. Its
    levels are absolute dBFS referred to the summed mean square of every
    channel, so N identical channels read ``10*log10(N)`` dB above one of
    them -- about 3.01 dB for a pair and 4.77 dB for three. Compare a floor
    only against one measured over the same number of channels. The
    attenuation figures on the report are fractions and do not move.

    Which config fields are live depends on ``mode``: ``over_subtraction``
    and ``spectral_floor`` are read only by ``"spectralSubtraction"``, and
    ``speech_presence_gain`` and ``gain_smoothing`` only by the other two, so
    at the default mode the first pair does nothing.

    Args:
        channels: One input buffer per channel, in the order the outputs come
            back, or a 2-D ``(channels, frames)`` array. All channels must be
            the same length; the C form has one length for the set.
        sample_rate: Sample rate in Hz, shared by every channel (default 22050).
        mode: ``"logMmse"`` (default), ``"mmseStsa"``, or ``"spectralSubtraction"``;
              an integer in ``SONARE_DENOISE_MODE_*`` is also accepted.
        noise_estimator: ``"quantile"`` (default), ``"mcra"``, ``"imcra"``, or ``"spp"``.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples (default 256).
        dd_alpha: Decision-directed a priori SNR smoothing (default 0.98).
        reduction_db: Deepest attenuation the mask may apply, in dB, >= 0
            (default 26.0).
        over_subtraction: Berouti alpha; SpectralSubtraction only (default 2.0).
        spectral_floor: Berouti beta; SpectralSubtraction only (default 0.05).
        noise_estimation_quantile: Fraction of frames assumed noise-only (default 0.1).
        speech_presence_gain: Apply speech-presence probability gating (default True).
        gain_smoothing: Smooth gains across time (default True).

    Returns:
        :class:`DenoiseLinkedResult` with one output buffer per input channel
        and the one report the shared mask produced.

    Raises:
        SonareValueError: If ``mode`` / ``noise_estimator`` cannot be resolved,
            if ``n_fft`` is not a power of two, if ``hop_length`` is not
            positive, if ``channels`` is empty, if the channels disagree in
            length, or if any channel is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request, which includes an input
            shorter than ``n_fft``.
    """
    # The core requires a power of two here (denoise_classical.cpp), narrower
    # than the shared even-size rule; check it eagerly so the message names it.
    n_fft_value = _require_power_of_two(n_fft, "n_fft")
    hop_length_value = _narrow_int(
        hop_length,
        "mastering_repair_denoise_classical_linked: hop_length",
        _C_INT_MIN,
        _C_INT_MAX,
    )
    if hop_length_value <= 0:
        raise SonareValueError("hop_length must be positive")

    lib = _get_lib()
    symbol = "sonare_mastering_repair_denoise_classical_linked"
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    planes = _linked_channel_planes("mastering_repair_denoise_classical_linked", channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    config = SonareDenoiseClassicalConfig(  # noqa: F405
        mode=_coerce_denoise_mode(mode),
        noise_estimator=_coerce_denoise_estimator(noise_estimator),
        n_fft=n_fft_value,
        hop_length=hop_length_value,
        dd_alpha=float(dd_alpha),
        reduction_db=float(reduction_db),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        noise_estimation_quantile=float(noise_estimation_quantile),
        speech_presence_gain=1 if speech_presence_gain else 0,
        gain_smoothing=1 if gain_smoothing else 0,
    )
    out_buffers, out_ptrs = _linked_output_planes(len(arrays), frame_count)
    report = SonareDenoiseReport()  # noqa: F405
    _check(
        lib.sonare_mastering_repair_denoise_classical_linked(
            ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            _to_c_size_t(len(arrays), "channel_count"),
            _to_c_size_t(frame_count, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.cast(out_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            ctypes.byref(report),
        )
    )
    return DenoiseLinkedResult(
        channels=out_buffers,
        report=_extract_denoise_report(report),
    )


_DEHUM_MODE_NAMES = {
    "subtract": SONARE_DEHUM_MODE_SUBTRACT,  # noqa: F405
    "notch": SONARE_DEHUM_MODE_NOTCH,  # noqa: F405
}


def _coerce_dehum_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _DEHUM_MODE_NAMES,
        "dehum mode",
        underscore=True,
        strip=True,
        validate_int=True,
    )


@_guard_buffer("samples")
def mastering_repair_dehum(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
    mode: int | str = "subtract",
) -> np.ndarray:
    """Offline mains-hum remover.

    Args:
        mode: ``"subtract"`` (default) or ``"notch"``; an integer in
            ``SONARE_DEHUM_MODE_*`` is also accepted.
    """
    config = SonareDehumConfig(  # noqa: F405
        fundamental_hz=float(fundamental_hz),
        # Narrowed rather than coerced: int() takes 4.7 as 4, a notch count nobody asked for.
        harmonics=_narrow_int(
            harmonics, "mastering_repair_dehum: harmonics", _C_INT_MIN, _C_INT_MAX
        ),
        q=float(q),
        adaptive=1 if adaptive else 0,
        search_range_hz=float(search_range_hz),
        adaptation=float(adaptation),
        frame_size=_narrow_int(
            frame_size, "mastering_repair_dehum: frame_size", _C_INT_MIN, _C_INT_MAX
        ),
        pll_bandwidth=float(pll_bandwidth),
        mode=_coerce_dehum_mode(mode),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_dehum, samples, sample_rate, config)


def _extract_hum_detection(raw: Any) -> HumDetection:
    return HumDetection(
        fundamental_hz=float(raw.fundamental_hz),
        fundamental_prominence=float(raw.fundamental_prominence),
        harmonics=int(raw.harmonics),
        harmonic_dbfs=[float(v) for v in raw.harmonic_dbfs],
    )


@_guard_buffer("samples")
def mastering_repair_detect_hum(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
    mode: int | str = "subtract",
) -> HumDetection:
    """Measure mains hum without filtering it.

    ALWAYS measured through the estimation path, whatever ``adaptive`` says: the
    fixed path notches the configured frequency without ever looking for hum, so
    a detector following the flag would hand back its own input. That makes this
    the way to find out whether a recording has hum at all, and at which
    frequency, before deciding to filter it::

        detected = libsonare.mastering_repair_detect_hum(samples, 44100)
        if detected.fundamental_prominence > 2.0:
            samples = libsonare.mastering_repair_dehum(
                samples, 44100, fundamental_hz=detected.fundamental_hz
            )

    ``fundamental_prominence`` is the winning candidate's projected energy over
    the median candidate; 1.0 means no peak was found at all. It is not a lock
    flag. ``harmonic_dbfs`` has 16 entries measured at every ``k * f0`` the
    sample rate carries, not only the notched ones, so an entry at or past
    Nyquist reads the dB floor because nothing is there to measure.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        fundamental_hz: Frequency the search is centred on (default 50.0).
        harmonics: Notch count the repair would use (default 4).
        q: Notch Q the repair would use (default 20.0).
        adaptive: Ignored here -- the estimation path always runs (default False).
        search_range_hz: Tracking search range in Hz (default 2.0).
        adaptation: Tracking step size (default 0.25).
        frame_size: Analysis frame size, must be >= 16 (default 2048).
        pll_bandwidth: PLL bandwidth (default 0.01).
        mode: ``"subtract"`` (default) or ``"notch"``; an integer in
            ``SONARE_DEHUM_MODE_*`` is also accepted.

    Returns:
        :class:`~libsonare.types.HumDetection`.

    Raises:
        SonareValueError: If the buffer is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    config = SonareDehumConfig(  # noqa: F405
        fundamental_hz=float(fundamental_hz),
        harmonics=_narrow_int(
            harmonics, "mastering_repair_detect_hum: harmonics", _C_INT_MIN, _C_INT_MAX
        ),
        q=float(q),
        adaptive=1 if adaptive else 0,
        search_range_hz=float(search_range_hz),
        adaptation=float(adaptation),
        frame_size=_narrow_int(
            frame_size, "mastering_repair_detect_hum: frame_size", _C_INT_MIN, _C_INT_MAX
        ),
        pll_bandwidth=float(pll_bandwidth),
        mode=_coerce_dehum_mode(mode),
    )
    return _extract_hum_detection(
        _run_detection(
            _get_lib().sonare_mastering_repair_detect_hum,
            samples,
            sample_rate,
            config,
            SonareHumDetection,  # noqa: F405
        )
    )


def _extract_dehum_report(raw: Any) -> DehumReport:
    return DehumReport(
        detected=_extract_hum_detection(raw.detected),
        notched_harmonics=int(raw.notched_harmonics),
        applied_fundamental_hz=float(raw.applied_fundamental_hz),
        fundamental_drift_hz=float(raw.fundamental_drift_hz),
    )


@_guard_buffer("left", "right")
def mastering_repair_dehum_stereo(
    left: Sequence[float] | list[float] | np.ndarray,
    right: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
    mode: int | str = "subtract",
) -> DehumStereoResult:
    """Dehums a stereo pair, sharing the tracked fundamental when tracking is on.

    Mains hum is one physical source, so with ``adaptive`` set the tracker
    reads the channel mean and both cascades follow the one frequency it
    finds: tracking the channels apart would put the notches at two
    frequencies differing by whatever each channel's programme material
    pulled its own search to, an image shift the hum itself never had. Only
    the frequency is shared -- each channel keeps its own filter state, so
    neither channel's transient rings through the other, and each report's
    ``detected`` measures that channel's own input. With ``adaptive`` clear,
    which is the default, nothing is shared and the two channels are
    filtered independently at the configured frequency.

    Args:
        left: Left channel input buffer (any sequence convertible to float32).
        right: Right channel input buffer, same length as ``left``.
        sample_rate: Sample rate in Hz (default 22050).
        fundamental_hz: Mains-hum fundamental (default 50 Hz).
        harmonics: Notch count including fundamental (default 4).
        q: Notch Q (default 20).
        adaptive: Enable adaptive tracking, shared between channels (default False).
        search_range_hz: Tracking search range in Hz (default 2).
        adaptation: Tracking step size (default 0.25).
        frame_size: Analysis frame size, must be >= 16 (default 2048).
        pll_bandwidth: PLL bandwidth (default 0.01).
        mode: ``"subtract"`` (default) or ``"notch"``; an integer in
            ``SONARE_DEHUM_MODE_*`` is also accepted.

    Returns:
        :class:`DehumStereoResult` with the dehummed channels and each
        channel's own detection/repair report.
    """
    lib = _get_lib()
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise SonareValueError("left and right channel lengths must match")
    config = SonareDehumConfig(  # noqa: F405
        fundamental_hz=float(fundamental_hz),
        harmonics=_narrow_int(
            harmonics, "mastering_repair_dehum_stereo: harmonics", _C_INT_MIN, _C_INT_MAX
        ),
        q=float(q),
        adaptive=1 if adaptive else 0,
        search_range_hz=float(search_range_hz),
        adaptation=float(adaptation),
        frame_size=_narrow_int(
            frame_size, "mastering_repair_dehum_stereo: frame_size", _C_INT_MIN, _C_INT_MAX
        ),
        pll_bandwidth=float(pll_bandwidth),
        mode=_coerce_dehum_mode(mode),
    )
    out = SonareDehumStereoResult()  # noqa: F405
    rc = lib.sonare_mastering_repair_dehum_stereo(
        left_array,
        right_array,
        _to_c_size_t(left_length, "left_length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    try:
        _check(rc)
        n = int(out.length)
        return DehumStereoResult(
            left=[float(out.left[i]) for i in range(n)],
            right=[float(out.right[i]) for i in range(n)],
            length=n,
            left_report=_extract_dehum_report(out.left_report),
            right_report=_extract_dehum_report(out.right_report),
        )
    finally:
        # No dedicated free function for this result: `left`/`right` are each
        # released with sonare_free_floats (see SonareDehumStereoResult in
        # sonare_c_mastering.h). A refused call leaves `out` at its
        # zero-initialized default, so both pointers are still NULL here.
        if out.left:
            lib.sonare_free_floats(out.left)
        if out.right:
            lib.sonare_free_floats(out.right)
