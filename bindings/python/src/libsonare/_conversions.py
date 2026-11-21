"""Core conversion and utility wrappers for libsonare."""

from __future__ import annotations

import ctypes
import math
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

import numpy as np
import numpy.typing as npt

from ._analysis_detection import _DEFAULT_BPM_MAX, _DEFAULT_BPM_MIN
from ._errors import _BARE_TEXT, _entry, _refuse
from ._ffi import SonareNnlsChromaOptions, SonareSilenceCommonReport
from ._narrowing import _narrow_double
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    ErrorCode,
    SonareValueError,
    _call_float_array_transform,
    _call_string,
    _check,
    _from_c_float_array,
    _from_c_float_columns,
    _from_c_float_matrix,
    _from_c_int_array,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _out_int_array,
    _resolve_optional_non_negative,
    _strip_c_entry,
    _to_c_float,
    _to_c_float_array,
    _to_c_float_matrix,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _token_ordinal,
    _utf8_arg,
    _validate_matrix,
    _validate_samples,
)
from ._types_enums import _NORM_TYPES, _TEMPOGRAM_MODES, NormType, TempogramMode


@_entry("hz_to_mel")
def hz_to_mel(hz: float) -> float:
    """Convert frequency in Hz to Mel scale."""
    lib = _get_lib()
    return float(lib.sonare_hz_to_mel(_to_c_float(hz, "hz")))


@_entry("mel_to_hz")
def mel_to_hz(mel: float) -> float:
    """Convert Mel scale value to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_mel_to_hz(_to_c_float(mel, "mel")))


@_entry("hz_to_midi")
def hz_to_midi(hz: float) -> float:
    """Convert frequency in Hz to MIDI note number."""
    lib = _get_lib()
    return float(lib.sonare_hz_to_midi(_to_c_float(hz, "hz")))


@_entry("midi_to_hz")
def midi_to_hz(midi: float) -> float:
    """Convert MIDI note number to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_midi_to_hz(_to_c_float(midi, "midi")))


def _refused_argument(fallback_message: str) -> SonareValueError:
    """Build the error for a call the core refused, from the library's last error."""
    lib = _get_lib()
    code = int(lib.sonare_last_error_code())
    raw = lib.sonare_last_error_message()
    if not raw:
        return _refuse(fallback_message)
    detail = _strip_c_entry(raw.decode("utf-8", errors="replace"))
    exc = SonareValueError(detail, code or int(ErrorCode.INVALID_PARAMETER))
    setattr(exc, _BARE_TEXT, detail)
    return exc


@_entry("hz_to_note")
def hz_to_note(hz: float) -> str:
    """Convert frequency in Hz to note name (e.g. 'A4').

    Raises:
        SonareValueError: If ``hz`` is not finite and greater than 0.
    """
    lib = _get_lib()
    return _call_string(lib.sonare_hz_to_note, _to_c_float(hz, "hz"))


@_entry("note_to_hz")
def note_to_hz(note: str) -> float:
    """Convert note name (e.g. 'A4') to frequency in Hz.

    Raises:
        SonareValueError: If ``note`` does not parse or its octave is out of range.
    """
    lib = _get_lib()
    hz = float(lib.sonare_note_to_hz(_utf8_arg(note, "note")))
    if math.isnan(hz):
        raise _refused_argument("note must be a note name such as A4")
    return hz


@_entry("frames_to_time")
def frames_to_time(frames: int, sr: int = 22050, hop_length: int = 512) -> float:
    """Convert frame count to time in seconds.

    Raises:
        SonareValueError: If ``sr`` or ``hop_length`` is not an integer in ``[1, 2147483647]``.
    """
    lib = _get_lib()
    return float(
        lib.sonare_frames_to_time(
            _to_c_int(frames, "frames"),
            ctypes.c_int(_narrow_int(sr, "sr", 1, _C_INT_MAX)),
            _narrow_int(hop_length, "hop_length", 1, _C_INT_MAX),
        )
    )


@_entry("time_to_frames")
def time_to_frames(time_sec: float, sr: int = 22050, hop_length: int = 512) -> int:
    """Convert time in seconds to frame count.

    Raises:
        SonareValueError: If ``sr`` or ``hop_length`` is not an integer in ``[1, 2147483647]``.
    """
    lib = _get_lib()
    return int(
        lib.sonare_time_to_frames(
            _to_c_float(time_sec, "time_sec"),
            ctypes.c_int(_narrow_int(sr, "sr", 1, _C_INT_MAX)),
            _narrow_int(hop_length, "hop_length", 1, _C_INT_MAX),
        )
    )


def _centring_n_fft(n_fft: int | None) -> int:
    """The C field's ``n_fft``: 0 when omitted (no centring), else a positive frame size."""
    if n_fft is None:
        return 0
    if isinstance(n_fft, int) and not isinstance(n_fft, bool) and n_fft == 0:
        raise _refuse("n_fft must not be 0; omit it for no centring")
    return _narrow_int(n_fft, "n_fft", 1, _C_INT_MAX)


@_entry("frames_to_samples")
def frames_to_samples(frames: int, hop_length: int = 512, n_fft: int | None = None) -> int:
    """Convert frame count to sample index.

    With ``n_fft`` the result is offset by ``n_fft // 2`` (librosa's centring).

    Raises:
        SonareValueError: If ``hop_length`` or a given ``n_fft`` is not an integer in
            ``[1, 2147483647]``.
    """
    lib = _get_lib()
    return int(
        lib.sonare_frames_to_samples(
            _to_c_int(frames, "frames"),
            _narrow_int(hop_length, "hop_length", 1, _C_INT_MAX),
            _centring_n_fft(n_fft),
        )
    )


@_entry("samples_to_frames")
def samples_to_frames(samples: int, hop_length: int = 512, n_fft: int | None = None) -> int:
    """Convert sample index to frame count.

    With ``n_fft``, ``n_fft // 2`` is subtracted from the sample first (librosa's centring).

    Raises:
        SonareValueError: If ``hop_length`` or a given ``n_fft`` is not an integer in
            ``[1, 2147483647]``.
    """
    lib = _get_lib()
    return int(
        lib.sonare_samples_to_frames(
            _to_c_int(samples, "samples"),
            _narrow_int(hop_length, "hop_length", 1, _C_INT_MAX),
            _centring_n_fft(n_fft),
        )
    )


@_entry("power_to_db")
def power_to_db(
    values: Sequence[float] | list[float],
    ref: float = 1.0,
    amin: float = 1e-10,
    top_db: float = 80.0,
) -> np.ndarray:
    """Convert power values to dB; returns a float32 array shaped like ``values``."""
    return _call_float_array_transform(
        "sonare_power_to_db",
        values,
        _to_c_float(ref, "ref"),
        _to_c_float(amin, "amin"),
        _to_c_float(top_db, "top_db"),
        arg_name="values",
    )


@_entry("amplitude_to_db")
def amplitude_to_db(
    values: Sequence[float] | list[float],
    ref: float = 1.0,
    amin: float = 1e-5,
    top_db: float = 80.0,
) -> np.ndarray:
    """Convert amplitude values to dB; returns a float32 array shaped like ``values``."""
    return _call_float_array_transform(
        "sonare_amplitude_to_db",
        values,
        _to_c_float(ref, "ref"),
        _to_c_float(amin, "amin"),
        _to_c_float(top_db, "top_db"),
        arg_name="values",
    )


@_entry("db_to_power")
def db_to_power(values: Sequence[float] | list[float], ref: float = 1.0) -> np.ndarray:
    """Convert dB values back to power; returns a float32 array shaped like ``values``."""
    return _call_float_array_transform(
        "sonare_db_to_power", values, _to_c_float(ref, "ref"), arg_name="values"
    )


@_entry("db_to_amplitude")
def db_to_amplitude(values: Sequence[float] | list[float], ref: float = 1.0) -> np.ndarray:
    """Convert dB values back to amplitude; returns a float32 array shaped like ``values``."""
    return _call_float_array_transform(
        "sonare_db_to_amplitude", values, _to_c_float(ref, "ref"), arg_name="values"
    )


@_entry("preemphasis")
def preemphasis(
    samples: Sequence[float] | list[float],
    coef: float = 0.97,
    zi: float | None = None,
) -> np.ndarray:
    """Apply librosa-compatible pre-emphasis; returns a float32 array."""
    return _call_float_array_transform(
        "sonare_preemphasis",
        samples,
        _to_c_float(coef, "coef"),
        _to_c_float(0.0 if zi is None else zi, "zi"),
        ctypes.c_int(0 if zi is None else 1),
    )


@_entry("deemphasis")
def deemphasis(
    samples: Sequence[float] | list[float],
    coef: float = 0.97,
    zi: float | None = None,
) -> np.ndarray:
    """Apply inverse pre-emphasis; returns a float32 array."""
    return _call_float_array_transform(
        "sonare_deemphasis",
        samples,
        _to_c_float(coef, "coef"),
        _to_c_float(0.0 if zi is None else zi, "zi"),
        ctypes.c_int(0 if zi is None else 1),
    )


@_entry("trim_silence")
@_guard_buffer("samples")
def trim_silence(
    samples: Sequence[float] | list[float],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> TrimResult:
    """Trim leading/trailing silence and return a :class:`TrimResult`."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    start = ctypes.c_int64()
    end = ctypes.c_int64()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_trim_silence(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_float(top_db, "top_db"),
            _to_c_size_t(frame_length, "frame_length"),
            _to_c_size_t(hop_length, "hop_length"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(start),
            ctypes.byref(end),
        )
        _check(rc)
        return TrimResult(
            audio=_from_c_float_array(out, out_length.value),
            start_sample=int(start.value),
            end_sample=int(end.value),
        )


@_entry("split_silence")
@_guard_buffer("samples")
def split_silence(
    samples: Sequence[float] | list[float],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Return non-silent intervals as an int64 array of shape ``(n_intervals, 2)``.

    Each row is ``[start_sample, end_sample)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_int_array(lib) as (out, out_count):
        rc = lib.sonare_split_silence(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_float(top_db, "top_db"),
            _to_c_size_t(frame_length, "frame_length"),
            _to_c_size_t(hop_length, "hop_length"),
            ctypes.byref(out),
            ctypes.byref(out_count),
        )
        _check(rc)
        return _intervals_result(out, out_count.value)


def _intervals_result(out: object, count: int) -> np.ndarray:
    """Copy a flat ``[start, end, ...]`` C int array into an int64 ``(n, 2)`` array."""
    return _from_c_int_array(out, count).astype(np.int64).reshape(-1, 2)


@dataclass(frozen=True, slots=True, eq=False)
class TrimResult:
    """Audio kept by :func:`trim_silence` and the span it came from.

    ``audio`` is a float32 array; ``start_sample`` and ``end_sample`` bound it
    in the input as ``[start_sample, end_sample)``.
    """

    audio: npt.NDArray[np.float32]
    """The kept audio."""
    start_sample: int
    """Start of the kept span in the input, in samples."""
    end_sample: int
    """End of the kept span in the input, exclusive, in samples."""


@dataclass(frozen=True, slots=True)
class SilenceCommonReport:
    """Why :func:`split_silence_common_with_report` found the gaps it did.

    One interval covering everything is the answer to three different
    situations and the interval list separates none of them: no take has a
    quiet moment at all, the takes each have one but not in the same place, or
    ``top_db`` was set too loose to see the ones they have.

    Read ``silence_ceiling_db`` -- the largest ``top_db`` at which every signal
    still shows silence -- against the ``top_db`` that was passed, which is the
    whole decision. Near 0, a take is sounding continuously and no threshold
    helps. Below ``top_db``, the threshold was too loose to see the quiet these
    takes do have, and one under the reported ceiling finds it. At or above
    ``top_db`` with a single interval, every take shows silence at this setting
    and they do not share any of it -- the alignment case.

    ``max_signal_intervals`` / ``min_signal_intervals`` count what the most and
    least fragmented signal produced alone, before the union merged anything.
    They measure shape rather than cause: a take that sounds once and stops
    counts 1, exactly as a take with no silence does, so use the ceiling to
    tell those apart and these to see whether any take has an interior gap
    (>= 2) and whether the takes differ in how broken up they are.
    """

    silence_ceiling_db: float
    """Largest ``top_db`` at which every signal still shows silence."""
    max_signal_intervals: int
    """Interval count of the most fragmented signal on its own, before the union."""
    min_signal_intervals: int
    """Interval count of the least fragmented signal on its own, before the union."""


def _common_silence_signals(
    signals: Sequence[Sequence[float]],
) -> tuple[list[ctypes.Array[ctypes.c_float]], ctypes.Array[Any], ctypes.Array[ctypes.c_size_t]]:
    """Validate and marshal the signal table both common-silence splitters take.

    The returned per-signal arrays are what the pointer table points at, so the
    caller must hold them for as long as the call runs.
    """
    if len(signals) == 0:
        raise _refuse("signals must not be empty")
    c_arrays: list[ctypes.Array[ctypes.c_float]] = []
    lengths: list[int] = []
    for index, signal in enumerate(signals):
        coerced = _validate_samples(signal, arg_name=f"signals[{index}]")
        c_array, length = _to_c_float_array(coerced, arg_name=f"signals[{index}]")
        c_arrays.append(c_array)
        lengths.append(length)
    float_ptr = ctypes.POINTER(ctypes.c_float)
    signal_ptrs = (float_ptr * len(c_arrays))(
        *[ctypes.cast(array, float_ptr) for array in c_arrays]
    )
    length_array = (ctypes.c_size_t * len(lengths))(*lengths)
    return c_arrays, signal_ptrs, length_array


@_entry("split_silence_common")
def split_silence_common(
    signals: Sequence[Sequence[float]],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> np.ndarray:
    """Return the intervals where any of several takes of one part is sounding.

    The result is an int64 array of shape ``(n_intervals, 2)`` of
    ``[start_sample, end_sample)`` rows.

    What several takes have in common is the silence, not the sound: the
    result is the union of what :func:`split_silence` reports for each
    signal (merged where intervals touch), so a cut always falls where every
    signal is quiet, never mid-phrase in one of them. A single signal returns
    exactly what :func:`split_silence` does.
    """
    c_arrays, signal_ptrs, length_array = _common_silence_signals(signals)
    lib = _get_lib()
    with _out_int_array(lib) as (out, out_count):
        rc = lib.sonare_split_silence_common(
            signal_ptrs,
            _to_c_size_t(len(c_arrays), "signal_count"),
            length_array,
            _to_c_float(top_db, "top_db"),
            _to_c_size_t(frame_length, "frame_length"),
            _to_c_size_t(hop_length, "hop_length"),
            ctypes.byref(out),
            ctypes.byref(out_count),
            None,
        )
        _check(rc)
        return _intervals_result(out, out_count.value)


@dataclass(frozen=True, slots=True, eq=False)
class SplitSilenceCommonWithReportResult:
    """Intervals from :func:`split_silence_common_with_report` and why they came out so.

    ``intervals`` is an int64 array of shape ``(n_intervals, 2)`` of
    ``[start_sample, end_sample)`` rows.
    """

    intervals: npt.NDArray[np.int64]
    """Shared non-silent ``[start_sample, end_sample)`` rows, shape ``(n_intervals, 2)``."""
    report: SilenceCommonReport
    """Why the intervals came out as they did."""


@_entry("split_silence_common_with_report")
def split_silence_common_with_report(
    signals: Sequence[Sequence[float]],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> SplitSilenceCommonWithReportResult:
    """Return what :func:`split_silence_common` returns, plus why.

    Identical intervals and identical refusals; the difference is the
    :class:`SilenceCommonReport`, whose figures come from the same RMS pass the
    intervals do and so can never describe a different measurement. The plain
    entry point stays because a caller cutting takes has no use for the
    diagnosis.
    """
    c_arrays, signal_ptrs, length_array = _common_silence_signals(signals)
    lib = _get_lib()
    report = SonareSilenceCommonReport()
    with _out_int_array(lib) as (out, out_count):
        rc = lib.sonare_split_silence_common(
            signal_ptrs,
            _to_c_size_t(len(c_arrays), "signal_count"),
            length_array,
            _to_c_float(top_db, "top_db"),
            _to_c_size_t(frame_length, "frame_length"),
            _to_c_size_t(hop_length, "hop_length"),
            ctypes.byref(out),
            ctypes.byref(out_count),
            ctypes.byref(report),
        )
        _check(rc)
        intervals = _intervals_result(out, out_count.value)
    return SplitSilenceCommonWithReportResult(
        intervals=intervals,
        report=SilenceCommonReport(
            silence_ceiling_db=float(report.silence_ceiling_db),
            max_signal_intervals=int(report.max_signal_intervals),
            min_signal_intervals=int(report.min_signal_intervals),
        ),
    )


@_entry("frame_signal")
def frame_signal(
    samples: Sequence[float] | list[float],
    frame_length: int,
    hop_length: int,
) -> np.ndarray:
    """Slice a signal into frames.

    Returns a float32 array of shape ``(frame_length, n_frames)``: column ``j`` is
    ``samples[j * hop_length : j * hop_length + frame_length]``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_frame_signal(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_size_t(frame_length, "frame_length"),
            _to_c_size_t(hop_length, "hop_length"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(n_frames),
        )
        _check(rc)
        frames = int(n_frames.value)
        flat = _from_c_float_array(out, out_length.value)
        # The C buffer holds one frame after another, so its rows are frames.
        frame_size = flat.size // frames if frames else int(frame_length)
        return np.ascontiguousarray(flat.reshape(frames, frame_size).T)


@_entry("pad_center")
def pad_center(
    values: Sequence[float] | list[float],
    target_size: int,
    pad_value: float = 0.0,
) -> np.ndarray:
    """Pad an array by centering it within target size; returns a float32 array."""
    return _call_float_array_transform(
        "sonare_pad_center",
        values,
        _to_c_size_t(target_size, "target_size"),
        _to_c_float(pad_value, "pad_value"),
        arg_name="values",
    )


@_entry("fix_length")
def fix_length(
    values: Sequence[float] | list[float],
    target_size: int,
    pad_value: float = 0.0,
) -> np.ndarray:
    """Crop or pad an array to exact length; returns a float32 array."""
    return _call_float_array_transform(
        "sonare_fix_length",
        values,
        _to_c_size_t(target_size, "target_size"),
        _to_c_float(pad_value, "pad_value"),
        arg_name="values",
    )


@_entry("fix_frames")
def fix_frames(
    frames: Sequence[int] | list[int],
    x_min: int = 0,
    x_max: int | None = None,
    pad: bool = True,
) -> np.ndarray:
    """Adjust frame indices to fit within bounds; returns an int32 array.

    ``x_max`` is the inclusive upper bound; ``None`` (the default) means none, and a
    negative value is refused.
    """
    lib = _get_lib()
    # Frame indices take the int path, which keeps an integer dtype exact.
    c_array, length = _to_c_int_array(frames, "frames")
    if length == 0:
        raise _refuse("frames must not be empty")
    with _out_int_array(lib) as (out, out_length):
        rc = lib.sonare_fix_frames(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(x_min, "x_min"),
            _to_c_int(_resolve_optional_non_negative(x_max, "x_max"), "x_max"),
            ctypes.c_int(1 if pad else 0),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_int_array(out, out_length.value)


@_entry("peak_pick")
def peak_pick(
    values: Sequence[float] | list[float],
    pre_max: int,
    post_max: int,
    pre_avg: int,
    post_avg: int,
    delta: float,
    wait: int,
) -> np.ndarray:
    """Pick peaks using librosa.util.peak_pick-compatible parameters.

    Returns an int32 array of peak indices.

    ``delta`` is a threshold above the local mean, in the units of ``values``
    itself. The envelope is not normalized -- :func:`onset_strength` returns
    half-wave rectified Mel-spectrogram flux, whose scale depends on the input
    level and on ``n_mels`` -- so a delta that works for one signal need not
    work for another. Read a starting value off your own envelope (a small
    fraction of ``max(values)`` is a reasonable first try) rather than carrying
    one over from another source.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(values, arg_name="values")
    with _out_int_array(lib) as (out, out_length):
        rc = lib.sonare_peak_pick(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(pre_max, "pre_max"),
            _to_c_int(post_max, "post_max"),
            _to_c_int(pre_avg, "pre_avg"),
            _to_c_int(post_avg, "post_avg"),
            _to_c_float(delta, "delta"),
            _to_c_int(wait, "wait"),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_int_array(out, out_length.value)


@_entry("vector_normalize")
def vector_normalize(
    values: Sequence[float] | list[float],
    norm: NormType = "max",
    threshold: float = 0.0,
) -> np.ndarray:
    """Normalize a vector (float32 array) by its maximum magnitude, L1, L2 or power norm."""
    return _call_float_array_transform(
        "sonare_vector_normalize",
        values,
        _to_c_int(_token_ordinal(norm, _NORM_TYPES, "norm"), "norm"),
        _to_c_float(threshold, "threshold"),
        arg_name="values",
    )


@_entry("pcen")
def pcen(
    values: npt.ArrayLike,
    sr: int = 22050,
    hop_length: int = 512,
    time_constant: float = 0.4,
    gain: float = 0.98,
    bias: float = 2.0,
    power: float = 0.5,
    eps: float = 1e-6,
) -> np.ndarray:
    """Apply per-channel energy normalization to a spectrogram.

    ``values`` is a 2-D array of shape ``(n_bins, n_frames)``; the result is a
    float32 array of the same shape.
    """
    lib = _get_lib()
    c_array, n_bins, n_frames = _to_c_float_matrix(values, arg_name="values")
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_pcen(
            c_array,
            _to_c_int(n_bins, "n_bins"),
            _to_c_int(n_frames, "n_frames"),
            _to_c_int(sr, "sr"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_float(time_constant, "time_constant"),
            _to_c_float(gain, "gain"),
            _to_c_float(bias, "bias"),
            _to_c_float(power, "power"),
            _to_c_float(eps, "eps"),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_matrix(out, n_bins, n_frames)


@_entry("tonnetz")
def tonnetz(chromagram: npt.ArrayLike) -> np.ndarray:
    """Compute Tonnetz from a chromagram.

    ``chromagram`` is a 2-D array of shape ``(n_chroma, n_frames)``; the result is
    a float32 array of shape ``(6, n_frames)``.
    """
    lib = _get_lib()
    c_array, n_chroma, n_frames = _to_c_float_matrix(chromagram, arg_name="chromagram")
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_tonnetz(
            c_array,
            _to_c_int(n_chroma, "n_chroma"),
            _to_c_int(n_frames, "n_frames"),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_columns(out, out_length.value, n_frames)


@_entry("tempogram")
def tempogram(
    onset_envelope: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
    mode: TempogramMode = "autocorrelation",
) -> np.ndarray:
    """Compute a tempogram as a float32 array of shape ``(win_length, n_frames)``."""
    mode_id = _token_ordinal(mode, _TEMPOGRAM_MODES, "mode")
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope, arg_name="onset_envelope")
    n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_tempogram(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sr, "sr"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_size_t(win_length, "win_length"),
            ctypes.c_int(1 if center else 0),
            ctypes.c_int(1 if norm else 0),
            _to_c_int(mode_id, "mode_id"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(n_frames),
        )
        _check(rc)
        return _from_c_float_columns(out, out_length.value, int(n_frames.value))


@_entry("cyclic_tempogram")
def cyclic_tempogram(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int,
    hop_length: int = 512,
    win_length: int = 384,
    bpm_min: float = 60.0,
    n_bins: int = 60,
) -> np.ndarray:
    """Compute a cyclic tempogram as a float32 array of shape ``(n_bins, n_frames)``."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope, arg_name="onset_envelope")
    n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_cyclic_tempogram(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_size_t(win_length, "win_length"),
            _to_c_float(bpm_min, "bpm_min"),
            _to_c_int(n_bins, "n_bins"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(n_frames),
        )
        _check(rc)
        return _from_c_float_columns(out, out_length.value, int(n_frames.value))


@_entry("plp")
def plp(
    onset_envelope: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    tempo_min: float | None = None,
    tempo_max: float | None = None,
    win_length: int = 384,
) -> np.ndarray:
    """Compute predominant local pulse from an onset envelope (float32, one value per frame).

    The pulse is sought between ``tempo_min`` and ``tempo_max`` BPM; an omitted bound
    is the library default, 60-200 BPM as on every other tempo entry point.
    """
    return _call_float_array_transform(
        "sonare_plp",
        onset_envelope,
        _to_c_int(sr, "sr"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_float(_DEFAULT_BPM_MIN if tempo_min is None else tempo_min, "tempo_min"),
        _to_c_float(_DEFAULT_BPM_MAX if tempo_max is None else tempo_max, "tempo_max"),
        _to_c_size_t(win_length, "win_length"),
        arg_name="onset_envelope",
    )


@_entry("onset_strength")
@_guard_buffer("samples")
def onset_strength(
    samples: Sequence[float] | list[float],
    sr: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
) -> np.ndarray:
    """Compute the onset strength envelope (librosa.onset.onset_strength).

    Returns a float32 array with one value per frame (half-wave rectified
    Mel-spectrogram flux).
    """
    return _call_float_array_transform(
        "sonare_onset_strength",
        samples,
        _to_c_int(sr, "sr"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_int(n_mels, "n_mels"),
    )


@_entry("onset_strength_multi")
@_guard_buffer("samples")
def onset_strength_multi(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_bands: int = 3,
) -> np.ndarray:
    """Compute multi-band onset strength as a float32 array of shape ``(n_bands, n_frames)``."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_onset_strength_multi(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_int(n_fft, "n_fft"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_int(n_mels, "n_mels"),
            _to_c_int(n_bands, "n_bands"),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(n_frames),
        )
        _check(rc)
        return _from_c_float_columns(out, out_length.value, int(n_frames.value))


@_entry("fourier_tempogram")
def fourier_tempogram(
    onset_envelope: Sequence[float] | list[float],
    sr: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
) -> np.ndarray:
    """Compute the Fourier (FFT-based) tempogram magnitude.

    Returns a float32 array of shape ``(n_bins, n_frames)``.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope, arg_name="onset_envelope")
    n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_fourier_tempogram(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sr, "sr"),
            _to_c_size_t(hop_length, "hop_length"),
            _to_c_size_t(win_length, "win_length"),
            ctypes.c_int(1 if center else 0),
            # The C norm flag stays set, as on every other surface.
            ctypes.c_int(1),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(n_frames),
        )
        _check(rc)
        return _from_c_float_columns(out, out_length.value, int(n_frames.value))


@_entry("tempogram_ratio")
def tempogram_ratio(
    tempogram: npt.ArrayLike,
    sr: int = 22050,
    hop_length: int = 512,
    factors: Sequence[float] | list[float] | None = None,
) -> np.ndarray:
    """Aggregate tempogram magnitudes at integer ratios of a reference tempo.

    ``tempogram`` is a 2-D array of shape ``(win_length, n_frames)``. When
    ``factors`` is None the library default factors are used. Returns a float32
    array with one value per factor.
    """
    matrix = _validate_matrix(tempogram, arg_name="tempogram")
    win_length = int(matrix.shape[0])
    if factors is None:
        factors_ptr: ctypes.Array[ctypes.c_float] | None = None
        n_factors = 0
    else:
        # Every factor scales the reference lag, so it must be finite and
        # positive: a NaN reaches an undefined float-to-int cast in the core
        # and an infinity silently degenerates to the DC lag. The list is a
        # handful of tempo ratios, so a plain scan costs nothing.
        factor_values = [_narrow_double(value, "factors") for value in factors]
        for index, factor in enumerate(factor_values):
            if not math.isfinite(factor) or factor <= 0.0:
                raise _refuse(f"factors[{index}] must be finite and greater than 0")
        factors_ptr, n_factors = _to_c_float_array(factor_values, arg_name="factors")
    return _call_float_array_transform(
        "sonare_tempogram_ratio",
        matrix.reshape(-1),
        _to_c_size_t(win_length, "win_length"),
        _to_c_int(sr, "sr"),
        _to_c_size_t(hop_length, "hop_length"),
        factors_ptr,
        _to_c_size_t(n_factors, "n_factors"),
        arg_name="tempogram",
    )


@_entry("nnls_chroma")
@_guard_buffer("samples")
def nnls_chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    enable_stft_blend: bool = True,
    stft_blend_weight: float = 0.55,
    stft_blend_n_fft: int = 4096,
    hop_length: int = 512,
) -> np.ndarray:
    """Compute NNLS chroma.

    Returns a float32 array of shape ``(12, n_frames)``.  ``hop_length``
    controls the CQT hop used by the NNLS analysis and defaults to the legacy
    512-sample hop.
    """
    # Narrowed, not int()-coerced; the core refuses an odd or too small stft_blend_n_fft.
    hop_length_value = _narrow_int(hop_length, "hop_length", 1, _C_INT_MAX)
    stft_blend_n_fft_value = _narrow_int(
        stft_blend_n_fft, "stft_blend_n_fft", _C_INT_MIN, _C_INT_MAX
    )
    try:
        blend_weight = _narrow_double(stft_blend_weight, "stft_blend_weight")
        blend_weight_c = ctypes.c_float(blend_weight).value
    except (TypeError, ValueError, OverflowError) as exc:
        raise _refuse("stft_blend_weight must be finite in [0, 1]") from exc
    if not math.isfinite(blend_weight_c) or not 0.0 <= blend_weight_c <= 1.0:
        raise _refuse("stft_blend_weight must be finite in [0, 1]")

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    n_frames = ctypes.c_int()
    with _out_float_array(lib) as (out, out_length):
        options = SonareNnlsChromaOptions._given(
            enable_stft_blend=int(enable_stft_blend),
            stft_blend_weight=blend_weight_c,
            stft_blend_n_fft=stft_blend_n_fft_value,
            hop_length=hop_length_value,
        )
        rc = lib.sonare_nnls_chroma(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(options),
            ctypes.byref(out),
            ctypes.byref(out_length),
            ctypes.byref(n_frames),
        )
        _check(rc)
        return _from_c_float_columns(out, out_length.value, int(n_frames.value))


def _resample_float32(
    samples: Sequence[float] | np.ndarray, src_sr: int, target_sr: int
) -> np.ndarray:
    """Resample to a float32 ndarray."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_resample(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(src_sr, "src_sr"),
            _to_c_int(target_sr, "target_sr"),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


@_entry("resample")
@_guard_buffer("samples")
def resample(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    target_sample_rate: int,
) -> np.ndarray:
    """Resample audio to a different sample rate.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate of ``samples`` in Hz.
        target_sample_rate: Target sample rate in Hz.

    Returns:
        A float32 array of resampled audio samples.
    """
    return _resample_float32(samples, sample_rate, target_sample_rate)
