"""Shared runtime helpers for the libsonare Python binding."""

from __future__ import annotations

import contextlib
import ctypes
import functools
import inspect
from collections.abc import Callable, Iterator, Mapping, Sequence
from enum import IntEnum
from numbers import Integral
from typing import Any, TypeVar, cast

import numpy as np

# _runtime is the shared re-export hub: feature submodules do
# `from ._runtime import *`, so forward the full C-struct and public type
# surfaces here instead of maintaining a partial hand-written list (an
# incomplete list silently breaks submodules at runtime with NameError).
from ._ffi import *  # noqa: F403
from .types import *  # noqa: F403

# Pan-law aliases are normalized case-insensitively and with underscores folded
# to hyphens. A finite Literal would incorrectly reject valid spellings such as
# ``CONST_4.5DB`` before the runtime resolver can apply that contract; malformed
# strings still fail in _pan_law_value below.
PanLawName = str
PanLawInput = PanLawName | PanLaw | int

PAN_MODE_BALANCE = 0
PAN_MODE_STEREO_PAN = 1
PAN_MODE_DUAL_PAN = 2

_lib: ctypes.CDLL | None = None


class ErrorCode(IntEnum):
    """Public C-ABI error codes carried by :class:`SonareError`."""

    OK = 0
    FILE_NOT_FOUND = 1
    INVALID_FORMAT = 2
    DECODE_FAILED = 3
    INVALID_PARAMETER = 4
    OUT_OF_MEMORY = 5
    NOT_SUPPORTED = 6
    INVALID_STATE = 7
    CANCELLED = 8
    ENCODE_FAILED = 9
    UNKNOWN = 99


class SonareError(RuntimeError):
    """Exception raised for non-OK Sonare C API return codes."""

    def __init__(self, code: int, message: str) -> None:
        self.code = int(code)
        super().__init__(f"[{self.code}] {message}")

    @property
    def code_name(self) -> str:
        """Canonical cross-binding name of :attr:`code`."""
        names = {
            ErrorCode.OK: "Ok",
            ErrorCode.FILE_NOT_FOUND: "FileNotFound",
            ErrorCode.INVALID_FORMAT: "InvalidFormat",
            ErrorCode.DECODE_FAILED: "DecodeFailed",
            ErrorCode.INVALID_PARAMETER: "InvalidParameter",
            ErrorCode.OUT_OF_MEMORY: "OutOfMemory",
            ErrorCode.NOT_SUPPORTED: "NotSupported",
            ErrorCode.INVALID_STATE: "InvalidState",
            ErrorCode.CANCELLED: "Cancelled",
            ErrorCode.ENCODE_FAILED: "EncodeFailed",
            ErrorCode.UNKNOWN: "Unknown",
        }
        try:
            return names[ErrorCode(self.code)]
        except ValueError:
            return names[ErrorCode.UNKNOWN]


class SonareValueError(SonareError, ValueError):
    """Exception raised when the binding rejects a caller-supplied argument.

    Deriving from both :class:`SonareError` and :class:`ValueError` is the
    compatibility contract, not an implementation detail: every
    argument-validation failure the binding raises must be caught by
    ``except ValueError`` (the plain type argument validation has always used)
    and by ``except SonareError`` (the binding's own error hierarchy), so
    neither style of caller needs to know which one a given entry point picks.

    Unlike :class:`SonareError`, the message is the plain validation text with
    no ``[code]`` prefix, since the failure never reached the C ABI.
    :attr:`code` defaults to :attr:`ErrorCode.INVALID_PARAMETER` so error-class
    and exit-code mapping treat it exactly like the C-ABI rejection it stands
    in for.
    """

    def __init__(self, message: str, code: int = int(ErrorCode.INVALID_PARAMETER)) -> None:
        self.code = int(code)
        ValueError.__init__(self, message)


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure.

    When the C layer recorded a detailed thread-local message
    (``sonare_last_error_message``), it is preferred over the generic
    ``sonare_error_message(rc)`` fallback so users see the underlying cause.
    """
    if rc != SONARE_OK:
        lib = _get_lib()
        detail = lib.sonare_last_error_message()
        detail_str = detail.decode("utf-8") if detail else ""
        if detail_str:
            raise SonareError(rc, detail_str)
        msg = lib.sonare_error_message(rc)
        raise SonareError(rc, msg.decode("utf-8") if msg else f"sonare error {rc}")


def _validate_samples(
    fn_name: str,
    samples: object,
    *,
    validate: bool = True,
    arg_name: str = "samples",
    allow_empty: bool = False,
) -> np.ndarray:
    """Coerce ``samples`` to a contiguous float32 buffer and apply input guards.

    Rejects empty buffers with :class:`SonareValueError`. When ``validate`` is
    True (the default), additionally scans for NaN / Inf and raises
    :class:`SonareValueError` on the first offending index. Hot paths may pass
    ``validate=False`` to skip the O(n) scan.

    ``allow_empty`` skips only the emptiness check, for a caller that inspects
    several buffers together and reports "nothing to work on" once rather than
    per buffer. The non-finite scan is unaffected — it is a no-op on an empty
    buffer — so the NaN / Inf message stays defined in this one place.
    """
    buf = _as_float32_buffer(samples, fn_name=fn_name, arg_name=arg_name)
    if not allow_empty and int(buf.shape[0]) == 0:
        raise SonareValueError(f"{fn_name}: {arg_name} must not be empty")
    if validate:
        # `np.isfinite` is vectorised C, so this stays cheap relative to the
        # actual DSP call but lets us surface the *index* of the bad value.
        finite = np.isfinite(buf)
        if not bool(finite.all()):
            bad = int(np.argmin(finite))
            raise SonareValueError(f"{fn_name}: {arg_name} contains NaN or Inf at index {bad}")
    return buf


_GuardedFn = TypeVar("_GuardedFn", bound=Callable[..., Any])


def _guard_buffer(*arg_names: str) -> Callable[[_GuardedFn], _GuardedFn]:
    """Preflight the named sample-buffer arguments of a facade function.

    Runs :func:`_validate_samples` on each named argument before the wrapped
    call, so an empty or non-finite buffer raises :class:`SonareValueError`
    naming both the facade function and the offending argument instead of the
    bare ``[4] Invalid parameter`` the C ABI reports for the same input.

    Arguments are resolved through the wrapped function's signature, so both
    call styles keep working — the librosa mirrors and the realtime event
    packers are positional by design. A name the call never supplied (an
    optional buffer left at its default) is skipped. When the wrapped function
    exposes a ``validate`` flag, the value in effect for the call decides
    whether the O(n) non-finite scan runs.

    The wrapper is built with :func:`functools.wraps`, so ``__doc__``,
    ``__module__`` and the introspected signature survive decoration; the
    ``.pyi`` stubs and :func:`libsonare._facade.rebind_facade_exports` both
    depend on that.
    """

    def decorate(fn: _GuardedFn) -> _GuardedFn:
        signature = inspect.signature(fn)
        validate_param = signature.parameters.get("validate")

        @functools.wraps(fn)
        def guarded(*args: Any, **kwargs: Any) -> Any:
            bound = signature.bind_partial(*args, **kwargs)
            validate = True
            if validate_param is not None:
                validate = bool(bound.arguments.get("validate", validate_param.default))
            for arg_name in arg_names:
                if arg_name in bound.arguments:
                    _validate_samples(
                        fn.__name__,
                        bound.arguments[arg_name],
                        validate=validate,
                        arg_name=arg_name,
                    )
            return fn(*args, **kwargs)

        return cast(_GuardedFn, guarded)

    return decorate


def _validate_scalar(fn_name: str, value: float, arg_name: str) -> float:
    """Reject NaN / Inf scalar inputs with :class:`SonareValueError`."""
    v = float(value)
    if not np.isfinite(v):
        raise SonareValueError(f"{fn_name}: {arg_name} must be a finite number")
    return v


def _as_float32_buffer(
    samples: object, *, fn_name: str = "", arg_name: str = "samples"
) -> np.ndarray:
    """Coerce ``samples`` to a contiguous ``float32`` 1-D numpy buffer.

    Zero-copy when the input is already a contiguous ``float32`` ndarray; one
    bulk C-level copy otherwise (``np.ascontiguousarray`` for non-contig
    float32 input, ``np.asarray`` for a sized sequence — list, tuple,
    ``array.array``, ``range``, ``memoryview``).

    An input NumPy cannot turn into a numeric buffer is rejected here with
    :class:`SonareValueError`, so no caller sees a bare NumPy ``TypeError``
    from inside the binding. A generator is the case worth naming: it is not a
    sequence, ``np.asarray`` wraps it in a 0-d object array, and the float cast
    then fails — it has never been convertible, whatever a reader might assume
    from the iterable-sounding parameter name.

    ``fn_name`` / ``arg_name`` only shape that message, so a rejection names the
    facade the caller invoked exactly as :func:`_validate_samples` does.
    """
    if isinstance(samples, np.ndarray):
        if (
            samples.dtype == np.float32
            and samples.flags["C_CONTIGUOUS"]
            and samples.flags["WRITEABLE"]
            and samples.ndim == 1
        ):
            return samples
        # Read-only float32 arrays (e.g. from ``np.frombuffer``, mmap, or
        # ``setflags(write=False)``) are harmless to the C library (samples are
        # taken as ``const``) but ``ctypes.from_buffer`` requires a *writable*
        # buffer. ``np.ascontiguousarray`` returns a read-only array unchanged,
        # so force a fresh writable copy in that case; otherwise take the cheap
        # single-pass cast/flatten path.
        buf = np.ascontiguousarray(samples, dtype=np.float32).reshape(-1)
        if not buf.flags["WRITEABLE"]:
            buf = np.array(buf, dtype=np.float32, copy=True, order="C").reshape(-1)
        return buf
    # list / tuple / array.array / range / memoryview → bulk-convert via NumPy's
    # vectorised C path (orders of magnitude faster than `(c_float*N)(*seq)`).
    try:
        return np.ascontiguousarray(np.asarray(samples, dtype=np.float32)).reshape(-1)
    except (TypeError, ValueError) as exc:
        prefix = f"{fn_name}: " if fn_name else ""
        raise SonareValueError(
            f"{prefix}{arg_name} must be a sequence of numbers or a numpy array, "
            f"not {type(samples).__name__}"
        ) from exc


def _to_c_float_array(
    samples: Sequence[float] | list[float] | np.ndarray,
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Convert a sample sequence to a ctypes float array (zero-copy when possible).

    The returned ctypes array shares memory with an internal numpy buffer when
    the input is already a contiguous ``float32`` ndarray, eliminating the
    per-element Python→C marshalling that used to dominate hot paths like
    :class:`RealtimeVoiceChanger.process_mono` (128 samples / 2.9 ms at 44.1 kHz).

    A reference to the backing buffer is attached to the returned ctypes
    array via ``_np_backing`` so it cannot be collected while the C call is
    in flight.
    """
    buf = _as_float32_buffer(samples)
    length = int(buf.shape[0])
    if length == 0:  # noqa: SIM108
        # `from_buffer` rejects zero-length buffers on some platforms; fall
        # back to a freshly allocated empty array.
        c_array = (ctypes.c_float * 0)()
    else:
        c_array = (ctypes.c_float * length).from_buffer(buf)
    # Defensive: pin the numpy buffer to the ctypes object so callers that
    # only retain ``c_array`` cannot accidentally drop the underlying memory.
    setattr(c_array, "_np_backing", buf)  # noqa: B010 -- ctypes arrays allow dynamic pinning.
    return c_array, length


def _to_c_float_array_owned(
    samples: Sequence[float] | list[float] | np.ndarray,
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Like :func:`_to_c_float_array`, but always over a fresh writable copy.

    Use this for C entry points that mutate the buffer in place: unlike
    :func:`_to_c_float_array` (which shares memory with a contiguous ``float32``
    ndarray for speed), this never aliases the caller's array, so processing
    cannot overwrite the input. The single bulk copy is negligible next to the
    DSP work these offline/streaming calls perform.
    """
    buf = np.array(_as_float32_buffer(samples), dtype=np.float32, copy=True, order="C").reshape(-1)
    length = int(buf.shape[0])
    if length == 0:  # noqa: SIM108
        c_array = (ctypes.c_float * 0)()
    else:
        c_array = (ctypes.c_float * length).from_buffer(buf)
    setattr(c_array, "_np_backing", buf)  # noqa: B010 -- ctypes arrays allow dynamic pinning.
    return c_array, length


def _from_c_float_array(array: object, count: int) -> np.ndarray:
    """Copy a C ``float*`` (or fixed-length ``c_float * N`` array) into numpy.

    Accepts either a ``ctypes.Array`` (e.g. ``(c_float * N)``) or a
    ``POINTER(c_float)`` and returns an independent ``float32`` ndarray
    (``copy=True`` semantics) so callers may safely free the C-side
    allocation immediately afterwards.
    """
    if count <= 0:
        return np.empty(0, dtype=np.float32)
    if isinstance(array, ctypes.Array):
        # `np.frombuffer` on a `(c_float * N)` shares memory; `.copy()` makes
        # the returned array safe to outlive the source ctypes buffer.
        floats = np.frombuffer(memoryview(array), dtype=np.float32, count=count)
        return cast(np.ndarray, floats.copy())
    # POINTER(c_float) path: materialise a fixed-size view at the same address.
    ptr = cast(Any, array)
    arr_type = ctypes.c_float * count
    view = arr_type.from_address(ctypes.addressof(ptr.contents))
    return cast(np.ndarray, np.frombuffer(memoryview(view), dtype=np.float32, count=count).copy())


def _from_c_int_array(array: object, count: int) -> np.ndarray:
    """Copy a C ``int32*`` (or fixed-length ``c_int32 * N`` array) into numpy.

    Integer mirror of :func:`_from_c_float_array`: accepts a ``ctypes.Array``
    or ``POINTER(c_int32)`` and returns an independent ``int32`` ndarray so the
    C-side allocation may be freed immediately afterwards.
    """
    if count <= 0:
        return np.empty(0, dtype=np.int32)
    if isinstance(array, ctypes.Array):
        ints = np.frombuffer(memoryview(array), dtype=np.int32, count=count)
        return cast(np.ndarray, ints.copy())
    ptr = cast(Any, array)
    arr_type = ctypes.c_int32 * count
    view = arr_type.from_address(ctypes.addressof(ptr.contents))
    return cast(np.ndarray, np.frombuffer(memoryview(view), dtype=np.int32, count=count).copy())


def _to_c_int_array(values: Sequence[int] | list[int]) -> tuple[ctypes.Array[ctypes.c_int32], int]:
    # Bulk-marshal via NumPy's vectorised C path instead of `(c_int32*N)(*seq)`,
    # which unpacks every element through Python varargs (mirrors the
    # zero-copy rewrite of `_to_c_float_array`).
    buf = np.ascontiguousarray(np.asarray(values, dtype=np.int32)).reshape(-1)
    # `ctypes.from_buffer` needs a *writable* buffer, and `ascontiguousarray`
    # hands a read-only int array back unchanged (`np.frombuffer`, mmap,
    # `setflags(write=False)`), so force a fresh writable copy in that case —
    # mirrors the float path in `_as_float32_buffer`.
    if not buf.flags["WRITEABLE"]:
        buf = np.array(buf, dtype=np.int32, copy=True, order="C").reshape(-1)
    length = int(buf.shape[0])
    if length == 0:  # noqa: SIM108
        c_array = (ctypes.c_int32 * 0)()
    else:
        c_array = (ctypes.c_int32 * length).from_buffer(buf)
    # Pin the numpy buffer so the backing memory outlives the C call.
    setattr(c_array, "_np_backing", buf)  # noqa: B010 -- ctypes arrays allow dynamic pinning.
    return c_array, length


_PAN_MODE_NAMES = {
    "balance": PAN_MODE_BALANCE,
    "stereo-pan": PAN_MODE_STEREO_PAN,
    "stereopan": PAN_MODE_STEREO_PAN,
    "pan": PAN_MODE_STEREO_PAN,
    "dual-pan": PAN_MODE_DUAL_PAN,
    "dualpan": PAN_MODE_DUAL_PAN,
}


def _enum_error(
    value: object,
    names: Mapping[str, int],
    what: str,
    verb: str,
    expected: bool,
    quote_value: bool,
) -> str:
    if expected:
        return f"{verb} {what}: {value!r} (expected one of {sorted(names)})"
    rendered = repr(value) if quote_value else str(value)
    return f"{verb} {what}: {rendered}"


def _resolve_enum(
    value: object,
    names: Mapping[str, int],
    what: str,
    *,
    enum_cls: type | None = None,
    dash: bool = False,
    underscore: bool = False,
    strip: bool = False,
    validate_int: bool = False,
    reject_bool: bool = False,
    verb: str = "unknown",
    expected: bool = False,
    quote_value: bool = False,
) -> int:
    """Resolve a string / int / enum ``value`` to its C enum ordinal.

    ``names`` maps accepted lowercase spellings (with underscores folded to
    dashes when ``dash`` is set) to ordinals. Integers pass through unchanged;
    an ``enum_cls`` instance is coerced with ``int()``. Unknown inputs raise
    :class:`SonareValueError` built from ``verb`` / ``what`` (and, when
    ``expected`` is set, the sorted accepted names).
    """
    if enum_cls is not None and isinstance(value, enum_cls):
        return cast(int, value)
    if isinstance(value, int):
        if (reject_bool and isinstance(value, bool)) or (
            validate_int and value not in names.values()
        ):
            raise SonareValueError(_enum_error(value, names, what, verb, expected, quote_value))
        return value
    if not isinstance(value, str):
        raise SonareValueError(_enum_error(value, names, what, verb, expected, quote_value))
    key = value.strip() if strip else value
    if dash:
        key = key.replace("_", "-")
    elif underscore:
        key = key.replace("-", "_")
    key = key.lower()
    if key not in names:
        raise SonareValueError(_enum_error(value, names, what, verb, expected, quote_value))
    return names[key]


_C_INT_MAX = 2**31 - 1


def _require_power_of_two(value: int, name: str) -> None:
    """Validate a positive power-of-two integer with a consistent error."""
    if value <= 0 or (value & (value - 1)) != 0:
        raise SonareValueError(f"{name} must be a positive power of two")


def _validate_stft_n_fft(fn_name: str, n_fft: int) -> int:
    """Validate an STFT size against the domain the core accepts.

    The core FFT is mixed-radix, so any even size transforms exactly; only the
    real one-sided spectrum's ``n_fft / 2 + 1`` bin layout needs the evenness. A
    power-of-two restriction here would reject sizes the C ABI and the native
    CLI accept, which makes the facade diverge rather than merely be stricter.

    This is the single definition of that domain for the Python binding. Every
    STFT-framed entry point routes through it, so the accepted range cannot
    drift apart between them.

    Args:
        fn_name: Caller name, used to prefix the error message.
        n_fft: Requested STFT size.

    Returns:
        The validated size as a plain ``int``.

    Raises:
        SonareValueError: If ``n_fft`` is not an even integer in
            ``[2, 2**31 - 1]``.
    """
    if isinstance(n_fft, bool) or not isinstance(n_fft, Integral):
        raise SonareValueError(f"{fn_name}: n_fft must be an integer")
    n_fft = int(n_fft)
    if n_fft < 2 or n_fft > _C_INT_MAX or n_fft % 2 != 0:
        raise SonareValueError(f"{fn_name}: n_fft must be an even signed 32-bit integer >= 2")
    return n_fft


def _validate_effect_fft_options(fn_name: str, n_fft: int, hop_length: int) -> tuple[int, int]:
    """Validate and normalize the FFT options shared by spectral effects.

    Entry points that additionally constrain the hop (the constant-overlap-add
    rule, for instance) apply that on top of what this returns.

    Args:
        fn_name: Caller name, used to prefix the error messages.
        n_fft: Requested STFT size; see :func:`_validate_stft_n_fft`.
        hop_length: Requested hop size in samples.

    Returns:
        The validated ``(n_fft, hop_length)`` pair as plain ``int`` values.

    Raises:
        SonareValueError: If either value falls outside the accepted domain.
    """
    n_fft = _validate_stft_n_fft(fn_name, n_fft)
    if isinstance(hop_length, bool) or not isinstance(hop_length, Integral):
        raise SonareValueError(f"{fn_name}: hop_length must be an integer")
    hop_length = int(hop_length)
    if hop_length <= 0 or hop_length > _C_INT_MAX:
        raise SonareValueError(
            f"{fn_name}: hop_length must fit in a positive signed 32-bit integer"
        )
    return n_fft, hop_length


def _synth_enum_value(value: str | int, names: Mapping[str, int], what: str) -> int:
    """Resolve a NativeSynth patch enum spelling to its C ordinal."""
    return _resolve_enum(value, names, what, expected=True)


def _pan_mode_value(value: str | int) -> int:
    return _resolve_enum(value, _PAN_MODE_NAMES, "pan mode", dash=True)


_AUTOMATION_CURVE_NAMES = {
    "linear": int(AutomationCurve.LINEAR),
    "lin": int(AutomationCurve.LINEAR),
    "exponential": int(AutomationCurve.EXPONENTIAL),
    "exp": int(AutomationCurve.EXPONENTIAL),
    "hold": int(AutomationCurve.HOLD),
    "step": int(AutomationCurve.HOLD),
    "s-curve": int(AutomationCurve.S_CURVE),
    "s_curve": int(AutomationCurve.S_CURVE),
    "scurve": int(AutomationCurve.S_CURVE),
    "smooth": int(AutomationCurve.S_CURVE),
}


def _curve_value(value: AutomationCurve | str | int) -> int:
    """Resolve an automation curve to its C enum value."""
    return _resolve_enum(
        value, _AUTOMATION_CURVE_NAMES, "automation curve", enum_cls=AutomationCurve
    )


_PAN_LAW_NAMES = {
    "const3db": int(PanLaw.CONST_3DB),
    "const-3db": int(PanLaw.CONST_3DB),
    "-3db": int(PanLaw.CONST_3DB),
    "const4.5db": int(PanLaw.CONST_4_5DB),
    "const-4.5db": int(PanLaw.CONST_4_5DB),
    "-4.5db": int(PanLaw.CONST_4_5DB),
    "const6db": int(PanLaw.CONST_6DB),
    "const-6db": int(PanLaw.CONST_6DB),
    "-6db": int(PanLaw.CONST_6DB),
    "linear0db": int(PanLaw.LINEAR_0DB),
    "linear": int(PanLaw.LINEAR_0DB),
    "linear-0db": int(PanLaw.LINEAR_0DB),
    "0db": int(PanLaw.LINEAR_0DB),
}


def _pan_law_value(value: PanLawInput) -> int:
    """Resolve a pan law to its C enum value (0=-3dB, 1=-4.5dB, 2=-6dB, 3=linear)."""
    return _resolve_enum(value, _PAN_LAW_NAMES, "pan law", enum_cls=PanLaw, dash=True)


_METER_TAP_NAMES = {
    "pre-fader": int(MeterTap.PRE_FADER),
    "pre": int(MeterTap.PRE_FADER),
    "prefader": int(MeterTap.PRE_FADER),
    "post-fader": int(MeterTap.POST_FADER),
    "post": int(MeterTap.POST_FADER),
    "postfader": int(MeterTap.POST_FADER),
}


def _meter_tap_value(value: MeterTap | str | int) -> int:
    """Resolve a meter tap point to its C enum value (0 pre-fader, 1 post-fader)."""
    return _resolve_enum(value, _METER_TAP_NAMES, "meter tap", enum_cls=MeterTap, dash=True)


_SEND_TIMING_NAMES = {
    "pre-fader": int(SendTiming.PRE_FADER),
    "pre": int(SendTiming.PRE_FADER),
    "prefader": int(SendTiming.PRE_FADER),
    "post-fader": int(SendTiming.POST_FADER),
    "post": int(SendTiming.POST_FADER),
    "postfader": int(SendTiming.POST_FADER),
}


def _send_timing_value(value: SendTiming | str | int) -> int:
    """Resolve a send timing to its C enum value (0 post-fader, 1 pre-fader)."""
    return _resolve_enum(value, _SEND_TIMING_NAMES, "send timing", enum_cls=SendTiming, dash=True)


_WARP_MODE_NAMES = {"off": 0, "repitch": 1, "tempo-sync": 2, "time-stretch": 3}


def _warp_mode_value(mode: str | int) -> int:
    """Resolve a warp mode to its C enum value (0 off, 1 repitch, 2 tempo-sync, 3 time-stretch)."""
    return _resolve_enum(mode, _WARP_MODE_NAMES, "warp mode", reject_bool=True)


def _mix_meter_from_c(snapshot: SonareMixMeterSnapshot) -> MixMeterSnapshot:
    return MixMeterSnapshot(
        peak_db_l=float(snapshot.peak_db_l),
        peak_db_r=float(snapshot.peak_db_r),
        rms_db_l=float(snapshot.rms_db_l),
        rms_db_r=float(snapshot.rms_db_r),
        correlation=float(snapshot.correlation),
        mono_compat_width=float(snapshot.mono_compat_width),
        mono_compat_peak=float(snapshot.mono_compat_peak),
        mono_compat_side_rms=float(snapshot.mono_compat_side_rms),
        likely_mono_compatible=bool(snapshot.likely_mono_compatible),
        momentary_lufs=float(snapshot.momentary_lufs),
        short_term_lufs=float(snapshot.short_term_lufs),
        integrated_lufs=float(snapshot.integrated_lufs),
        gain_reduction_db=float(snapshot.gain_reduction_db),
        true_peak_db_l=float(snapshot.true_peak_db_l),
        true_peak_db_r=float(snapshot.true_peak_db_r),
        max_true_peak_db=float(snapshot.max_true_peak_db),
        seq=int(snapshot.seq),
        channel_count=int(snapshot.channel_count),
        peak_db=tuple(float(snapshot.peak_db[i]) for i in range(snapshot.channel_count)),
        rms_db=tuple(float(snapshot.rms_db[i]) for i in range(snapshot.channel_count)),
        true_peak_db=tuple(float(snapshot.true_peak_db[i]) for i in range(snapshot.channel_count)),
    )


def _mode_values(modes: Sequence[Mode | str] | str | None) -> list[int]:
    if modes is None:
        return []
    if isinstance(modes, str):
        key = modes.lower()
        if key in ("major-minor", "majmin", "diatonic"):
            return [int(Mode.MAJOR), int(Mode.MINOR)]
        if key in ("all", "modal"):
            return [
                int(Mode.MAJOR),
                int(Mode.MINOR),
                int(Mode.DORIAN),
                int(Mode.PHRYGIAN),
                int(Mode.LYDIAN),
                int(Mode.MIXOLYDIAN),
                int(Mode.LOCRIAN),
            ]
        modes = [modes]
    out: list[int] = []
    for mode in modes:
        if isinstance(mode, str):
            out.append(_resolve_enum(mode, _MODE_NAMES, "mode", verb="invalid"))
        else:
            out.append(int(Mode(mode)))
    return out


_MODE_NAMES = {
    "major": int(Mode.MAJOR),
    "maj": int(Mode.MAJOR),
    "minor": int(Mode.MINOR),
    "min": int(Mode.MINOR),
    "m": int(Mode.MINOR),
    "dorian": int(Mode.DORIAN),
    "phrygian": int(Mode.PHRYGIAN),
    "lydian": int(Mode.LYDIAN),
    "mixolydian": int(Mode.MIXOLYDIAN),
    "locrian": int(Mode.LOCRIAN),
}

_KEY_PROFILE_NAMES = {
    "ks": int(KeyProfile.KRUMHANSL_SCHMUCKLER),
    "krumhansl": int(KeyProfile.KRUMHANSL_SCHMUCKLER),
    "krumhansl-schmuckler": int(KeyProfile.KRUMHANSL_SCHMUCKLER),
    "temperley": int(KeyProfile.TEMPERLEY),
    "shaath": int(KeyProfile.SHAATH),
    "keyfinder": int(KeyProfile.SHAATH),
    "faraldo-edmt": int(KeyProfile.FARALDO_EDMT),
    "edmt": int(KeyProfile.FARALDO_EDMT),
    "faraldo-edma": int(KeyProfile.FARALDO_EDMA),
    "edma": int(KeyProfile.FARALDO_EDMA),
    "faraldo-edmm": int(KeyProfile.FARALDO_EDMM),
    "edmm": int(KeyProfile.FARALDO_EDMM),
    "bellman-budge": int(KeyProfile.BELLMAN_BUDGE),
    "bellman": int(KeyProfile.BELLMAN_BUDGE),
}


def _profile_value(profile: KeyProfile | str | None) -> int:
    if profile is None:
        return int(KeyProfile.KRUMHANSL_SCHMUCKLER)
    if not isinstance(profile, str):
        return int(KeyProfile(profile))
    return _resolve_enum(profile, _KEY_PROFILE_NAMES, "key profile", verb="invalid")


def _float_array_result(out: object, count: int) -> list[float]:
    # Bulk C-side copy via `_from_c_float_array`, then `.tolist()` to honour the
    # documented `list[float]` return contract (callers index/iterate as lists).
    return cast(list[float], _from_c_float_array(out, count).tolist())


def _optional_float_array_result(out: object, count: int) -> list[float]:
    # A null pointer means the array was not computed (e.g. clarity bands in
    # blind mode); represent that as an empty list rather than crashing.
    if not out:
        return []
    return cast(list[float], _from_c_float_array(out, count).tolist())


def _int_array_result(out: object, count: int) -> list[int]:
    return cast(list[int], _from_c_int_array(out, count).tolist())


@contextlib.contextmanager
def _out_float_array(
    lib: ctypes.CDLL,
) -> Iterator[tuple[ctypes._Pointer[ctypes.c_float], ctypes.c_size_t]]:
    """Manage a C ``float*`` out-parameter, freeing it on exit.

    Yields ``(out, out_length)`` to pass by reference into a C call. The heap
    buffer is released with ``sonare_free_floats`` on both the success and the
    exception paths whenever the pointer is non-null, including zero-length
    sentinel allocations.
    """
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    try:
        yield out, out_length
    finally:
        if out:
            lib.sonare_free_floats(out)


@contextlib.contextmanager
def _out_int_array(
    lib: ctypes.CDLL,
) -> Iterator[tuple[ctypes._Pointer[ctypes.c_int], ctypes.c_size_t]]:
    """Manage a C ``int*`` out-parameter, freeing it on exit.

    Integer sibling of :func:`_out_float_array`; releases with
    ``sonare_free_ints``.
    """
    out = ctypes.POINTER(ctypes.c_int)()
    out_length = ctypes.c_size_t()
    try:
        yield out, out_length
    finally:
        if out:
            lib.sonare_free_ints(out)


def _call_float_transform(
    fn_name: str, values: Sequence[float] | list[float] | np.ndarray, *args: object
) -> list[float]:
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    with _out_float_array(lib) as (out, out_length):
        rc = getattr(lib, fn_name)(
            c_array,
            ctypes.c_size_t(length),
            *args,
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _float_array_result(out, out_length.value)


__all__ = [name for name in globals() if not name.startswith("__")]
