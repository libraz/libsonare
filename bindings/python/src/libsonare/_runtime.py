"""Shared runtime helpers for the libsonare Python binding."""

from __future__ import annotations

import contextlib
import ctypes
import functools
import inspect
import operator
from collections.abc import Callable, Iterator, Mapping, Sequence
from typing import Any, SupportsIndex, TypeVar, cast

import numpy as np

# _runtime is the shared re-export hub: feature submodules do
# `from ._runtime import *`, so forward the full C-struct and public type
# surfaces here instead of maintaining a partial hand-written list (an
# incomplete list silently breaks submodules at runtime with NameError).
# The leaf modules below are forwarded the same way; the redundant aliases are
# what marks a re-export to mypy's strict mode.
from ._errors import ErrorCode as ErrorCode
from ._errors import SonareError as SonareError
from ._errors import SonareValueError as SonareValueError
from ._ffi import *  # noqa: F403
from ._narrowing import _FLOAT32_MAX as _FLOAT32_MAX
from ._narrowing import _float_narrowing_error as _float_narrowing_error
from ._narrowing import _narrow_double as _narrow_double
from ._narrowing import _narrow_float as _narrow_float
from ._narrowing import _narrow_int as _narrow_int
from ._narrowing import _narrowing_error as _narrowing_error
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


def _get_lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = load_library()
    return _lib


def _generic_error(rc: int) -> SonareError:
    """Build a :class:`SonareError` from the code's own generic string."""
    msg = _get_lib().sonare_error_message(_to_c_int32(rc, "rc"))
    return SonareError(rc, msg.decode("utf-8") if msg else f"sonare error {rc}")


def _check(rc: int) -> None:
    """Check a SonareError return code and raise on failure.

    When the C layer recorded a detailed thread-local message
    (``sonare_last_error_message``), it is preferred over the generic
    ``sonare_error_message(rc)`` fallback so users see the underlying cause.

    This is the single definition of the return-code-to-exception mapping for
    the whole binding; every submodule imports it from here. Audio-thread entry
    points are the one exception and use :func:`_check_realtime` instead.
    """
    if rc != SONARE_OK:
        lib = _get_lib()
        detail = lib.sonare_last_error_message()
        detail_str = detail.decode("utf-8") if detail else ""
        if detail_str:
            raise SonareError(rc, detail_str)
        raise _generic_error(rc)


def _check_realtime(rc: int) -> None:
    """Check a return code from an audio-thread C entry point.

    Audio-thread entry points -- the engine block-render calls and the realtime
    voice changer's process / latency calls -- neither clear nor record the
    thread-local detail message, because first-touch TLS setup would break
    their no-allocation contract. After one of them fails, that slot still
    holds whatever an earlier control-thread call left behind, which
    ``sonare_c_types_functions.h`` states must not be read as belonging to the
    failed call.

    So this maps the code through ``sonare_error_message`` only. Reporting the
    stale detail instead described an unrelated call: a channel-count mismatch
    in the voice changer surfaced as a previous STFT's error text, which is
    both wrong and intermittent, since it depends on the call history rather
    than on the failure.
    """
    if rc != SONARE_OK:
        raise _generic_error(rc)


def _validate_samples(
    fn_name: str,
    samples: object,
    *,
    validate: bool = True,
    arg_name: str = "samples",
    allow_empty: bool = False,
    window: tuple[int, int] | None = None,
) -> np.ndarray:
    """Coerce ``samples`` to a contiguous float32 buffer and apply input guards.

    Rejects empty buffers with :class:`SonareValueError`. When ``validate`` is
    True (the default), additionally scans for NaN / Inf and raises
    :class:`SonareValueError` on the first offending index. Hot paths may pass
    ``validate=False`` to skip the O(n) scan.

    The coercion runs first, so a bad type or a rank other than 1 is named as
    such by :func:`_as_float32_buffer` and never reaches the scan. That
    ordering is what keeps the NaN / Inf message true: it is only ever raised
    for a buffer that really holds a non-finite sample.

    ``allow_empty`` skips only the emptiness check, for a caller that inspects
    several buffers together and reports "nothing to work on" once rather than
    per buffer. The non-finite scan is unaffected — it is a no-op on an empty
    buffer — so the NaN / Inf message stays defined in this one place.

    ``window`` is ``(start, length)`` for an entry point that reads one span of
    the buffer rather than all of it. It narrows the non-finite scan to that
    span, clamped to the buffer, and leaves the emptiness check covering the
    whole thing — the contract the C ABI states for its windowed calls, and the
    cost model they promise: per call the scan is bounded by the window, not by
    the buffer an analyzer is polling. The reported index stays absolute.
    """
    buf = _as_float32_buffer(samples, fn_name=fn_name, arg_name=arg_name)
    length = int(buf.shape[0])
    if not allow_empty and length == 0:
        raise SonareValueError(f"{fn_name}: {arg_name} must not be empty")
    if validate:
        start, stop = 0, length
        if window is not None:
            start = min(max(window[0], 0), length)
            stop = min(start + max(window[1], 0), length)
        # `np.isfinite` is vectorised C, so this stays cheap relative to the
        # actual DSP call but lets us surface the *index* of the bad value.
        finite = np.isfinite(buf[start:stop])
        if not bool(finite.all()):
            bad = start + int(np.argmin(finite))
            raise SonareValueError(f"{fn_name}: {arg_name} contains NaN or Inf at index {bad}")
    return buf


_GuardedFn = TypeVar("_GuardedFn", bound=Callable[..., Any])


def _guard_buffer(
    *arg_names: str, shape_only: tuple[str, ...] = ()
) -> Callable[[_GuardedFn], _GuardedFn]:
    """Preflight the named sample-buffer arguments of a facade function.

    Names given in ``shape_only`` are coerced and checked for emptiness like the
    rest, but their values are not scanned. That is for an argument whose own
    encoding uses a non-finite value to mean something -- an F0 contour spells a
    frame with no pitch that way -- where refusing it would refuse a measurement
    rather than a mistake.

    Runs :func:`_validate_samples` on each named argument before the wrapped
    call, so an empty or non-finite buffer raises :class:`SonareValueError`
    naming both the facade function and the offending argument instead of the
    bare ``[4] Invalid parameter`` the C ABI reports for the same input.

    The validated buffer replaces the caller's argument before the body runs, so
    the bytes the C ABI receives are the ones that were checked and each call
    coerces at most once. It is a contiguous float32 1-D ndarray; every body
    already funnels its buffer through the same helper, for which that is the
    zero-copy case.

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
            coerced = False
            for arg_name in (*arg_names, *shape_only):
                if arg_name in bound.arguments:
                    # Hand the body the buffer that was just validated, not the
                    # caller's original. Discarding it made every non-contiguous
                    # or non-float32 input pay for the full `np.asarray` walk and
                    # the float32 allocation twice — once here and once in the
                    # body's own `_to_c_float_array` — and left the bytes the C
                    # ABI actually receives one conversion removed from the ones
                    # that were checked.
                    bound.arguments[arg_name] = _validate_samples(
                        fn.__name__,
                        bound.arguments[arg_name],
                        validate=validate and arg_name not in shape_only,
                        arg_name=arg_name,
                    )
                    coerced = True
            if not coerced:
                return fn(*args, **kwargs)
            return fn(*bound.args, **bound.kwargs)

        return cast(_GuardedFn, guarded)

    return decorate


def _validate_scalar(fn_name: str, value: float, arg_name: str) -> float:
    """Reject NaN / Inf scalar inputs with :class:`SonareValueError`.

    The float32 range is not checked here -- :func:`_narrow_float` does that
    where the value is converted. What this owes the caller either way is a
    refusal naming the argument, so an integer too large for a double is
    refused rather than escaping as a bare ``OverflowError``.
    """
    refusal = f"{fn_name}: {arg_name} must be a finite number"
    try:
        v = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise SonareValueError(refusal) from exc
    if not np.isfinite(v):
        raise SonareValueError(refusal)
    return v


def _not_a_buffer(samples: object, fn_name: str, arg_name: str) -> SonareValueError:
    """Build the rejection for an input that is not a numeric buffer at all."""
    prefix = f"{fn_name}: " if fn_name else ""
    return SonareValueError(
        f"{prefix}{arg_name} must be a sequence of numbers or a numpy array, "
        f"not {type(samples).__name__}"
    )


def _past_float32_range(samples: object, fn_name: str, arg_name: str) -> SonareValueError:
    """Build the rejection for an element the float32 cast turned into an infinity.

    Reported through the same builder the scalar conversions use, because it is
    the same refusal: a finite number the target type cannot hold. Letting the
    cast stand instead named the result rather than the cause -- the caller's
    buffer held no infinity until this conversion produced one, and the
    finiteness check downstream then reported one at the caller's index.

    Only reached once the cast has already overflowed, so locating the element
    costs a pass on the failing path rather than on every call.
    """
    prefix = f"{fn_name}: " if fn_name else ""
    element = arg_name
    try:
        wide = np.asarray(samples, dtype=np.float64).reshape(-1)
    except (OverflowError, TypeError, ValueError):
        # Too wide for a double either, so there is no index to read it at.
        return SonareValueError(f"{prefix}{_float_narrowing_error(element)}")
    outside = np.isfinite(wide) & (np.abs(wide) > _FLOAT32_MAX)
    if outside.any():
        element = f"{arg_name}[{int(np.argmax(outside))}]"
    return SonareValueError(f"{prefix}{_float_narrowing_error(element)}")


def _not_one_dimensional(array: np.ndarray, fn_name: str, arg_name: str) -> SonareValueError:
    """Build the rejection for a buffer of the wrong rank, naming its shape."""
    prefix = f"{fn_name}: " if fn_name else ""
    return SonareValueError(
        f"{prefix}{arg_name} must be a 1-D buffer, not ndim={array.ndim} "
        f"shape={tuple(array.shape)}: downmix a (frames, channels) read along axis 1, "
        f"or flatten a row-major matrix argument, before passing it"
    )


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

    Anything that does not coerce to rank 1 is rejected here too, and this is
    the only place in the binding that decides it. Flattening instead is silent
    and wrong in both directions: a ``(frames, 2)`` stereo read — what
    ``soundfile`` hands back — becomes an interleaved mono buffer of twice the
    frame count, which analyses as an octave-low pitch, a halved tempo and a
    doubled loudness with nothing to indicate it; and a matrix argument
    supplied as a transposed 2-D array flattens in the wrong order while still
    passing the entry point's ``rows * columns`` length check. A rank the
    caller did not intend must therefore be named, not repaired.

    A rank-0 result is the ``None`` case: NumPy turns ``None`` into
    ``array(nan)``, so without this it reached the non-finite scan and was
    reported as a NaN sample at index 0 — sending the caller to look at audio
    data for a bug in whatever produced the ``None``.

    ``fn_name`` / ``arg_name`` only shape those messages, so a rejection names
    the facade the caller invoked exactly as :func:`_validate_samples` does.
    """
    if isinstance(samples, np.ndarray):
        if (
            samples.dtype == np.float32
            and samples.flags["C_CONTIGUOUS"]
            and samples.flags["WRITEABLE"]
            and samples.ndim == 1
        ):
            return samples
        if samples.ndim != 1:
            raise _not_one_dimensional(samples, fn_name, arg_name)
        # Read-only float32 arrays (e.g. from ``np.frombuffer``, mmap, or
        # ``setflags(write=False)``) are harmless to the C library (samples are
        # taken as ``const``) but ``ctypes.from_buffer`` requires a *writable*
        # buffer. ``np.ascontiguousarray`` returns a read-only array unchanged,
        # so force a fresh writable copy in that case; otherwise take the cheap
        # single-pass cast path.
        # Raised rather than warned: the cast folds a finite value past the
        # float32 ceiling onto an infinity, which reads downstream as one the
        # caller passed.
        try:
            with np.errstate(over="raise"):
                buf = np.ascontiguousarray(samples, dtype=np.float32)
        except FloatingPointError as exc:
            raise _past_float32_range(samples, fn_name, arg_name) from exc
        if not buf.flags["WRITEABLE"]:
            buf = np.array(buf, dtype=np.float32, copy=True, order="C")
        return buf
    # list / tuple / array.array / range / memoryview → bulk-convert via NumPy's
    # vectorised C path (orders of magnitude faster than `(c_float*N)(*seq)`).
    try:
        with np.errstate(over="raise"):
            converted = np.asarray(samples, dtype=np.float32)
    except (FloatingPointError, OverflowError) as exc:
        # An int too wide for a double raises rather than overflowing, and it is
        # the same refusal: a number this buffer's element type cannot hold.
        raise _past_float32_range(samples, fn_name, arg_name) from exc
    except (TypeError, ValueError) as exc:
        raise _not_a_buffer(samples, fn_name, arg_name) from exc
    if converted.ndim == 0:
        # ``None`` and bare scalars land here; report the type that was passed
        # rather than the rank, which says nothing about what to fix.
        raise _not_a_buffer(samples, fn_name, arg_name)
    if converted.ndim != 1:
        raise _not_one_dimensional(converted, fn_name, arg_name)
    return np.ascontiguousarray(converted)


def _not_planar_channels(channels: object, subject: str) -> SonareValueError:
    """Build the rejection for a channels argument that is not a planar buffer."""
    detail = (
        f"ndim={channels.ndim} shape={tuple(channels.shape)}"
        if isinstance(channels, np.ndarray)
        else f"a {type(channels).__name__}"
    )
    return SonareValueError(
        f"{subject} must be a sequence of 1-D channel buffers or a 2-D "
        f"(channels, frames) array, not {detail}"
    )


def _planar_channel_arrays(
    channels: Sequence[Sequence[float]] | np.ndarray,
    *,
    subject: str = "channels",
) -> tuple[list[ctypes.Array[ctypes.c_float]], ctypes.Array[Any], int]:
    """Marshal equal-length planar channels into ctypes arrays and a pointer table.

    One shared implementation for every planar-channel entry point: the realtime
    process path, clip-page supply, and clip marshalling each used to build
    ``(c_float * frames)(*channel)``, which unpacks every sample through Python
    varargs -- roughly 8192 conversions and a tuple build per 4096-frame stereo
    page, against one bulk copy here. Two of the three kept doing that after the
    third was fixed, which is why this lives in one place now.

    Each channel gets a private writable buffer, so a C call that writes its
    output back into these planes cannot reach the caller's array, and the numpy
    backing is pinned to the ctypes object so it outlives the call.

    A sequence of channel buffers and a 2-D ``(channels, frames)`` ndarray are
    both accepted -- the array is exactly the planar shape this API models, and
    it iterates as its channel rows. Every emptiness test here is an explicit
    length test: truthiness of the outer object raised NumPy's own ambiguous
    error for an array, and truthiness of a ``(1, 1)`` array split on the sample
    value it stored rather than on any size.

    ``subject`` names the argument in every rejection raised here, including the
    per-channel rank check, so a 2-D channel reports the name the caller used
    rather than the coercion helper's default.
    """
    if isinstance(channels, np.ndarray):
        if channels.ndim != 2:
            raise _not_planar_channels(channels, subject)
        channel_count = int(channels.shape[0])
    else:
        try:
            channel_count = len(channels)
        except TypeError as exc:
            raise _not_planar_channels(channels, subject) from exc
    if channel_count == 0:
        raise SonareValueError(f"{subject} must not be empty")
    arrays: list[ctypes.Array[ctypes.c_float]] = []
    frame_count = -1
    for channel in channels:
        buf = np.array(
            _as_float32_buffer(channel, arg_name=subject),
            dtype=np.float32,
            copy=True,
            order="C",
        )
        if frame_count < 0:
            # The frame count comes from the first coerced plane, so the rank
            # check runs before anything reads a length off a non-buffer.
            frame_count = int(buf.shape[0])
            if frame_count == 0:
                raise SonareValueError(f"{subject} must not be empty")
        elif int(buf.shape[0]) != frame_count:
            raise SonareValueError(f"all {subject} must have the same length")
        c_array = (ctypes.c_float * frame_count).from_buffer(buf)
        c_array._np_backing = buf  # type: ignore[attr-defined]
        arrays.append(c_array)
    ptr_type = ctypes.POINTER(ctypes.c_float) * len(arrays)
    ptrs = ptr_type(*[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in arrays])
    return arrays, ptrs, frame_count


def _to_c_float_array(
    samples: Sequence[float] | list[float] | np.ndarray,
    *,
    fn_name: str = "",
    arg_name: str = "samples",
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Convert a sample sequence to a ctypes float array (zero-copy when possible).

    The returned ctypes array shares memory with an internal numpy buffer when
    the input is already a contiguous ``float32`` ndarray, eliminating the
    per-element Python→C marshalling that used to dominate hot paths like
    :class:`RealtimeVoiceChanger.process_mono` (128 samples / 2.9 ms at 44.1 kHz).

    A reference to the backing buffer is attached to the returned ctypes
    array via ``_np_backing`` so it cannot be collected while the C call is
    in flight.

    ``fn_name`` / ``arg_name`` are forwarded to :func:`_as_float32_buffer`, so a
    call site coercing an unvalidated buffer under another name reports that
    name instead of this helper's ``samples`` default.
    """
    buf = _as_float32_buffer(samples, fn_name=fn_name, arg_name=arg_name)
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
    *,
    fn_name: str = "",
    arg_name: str = "samples",
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    """Like :func:`_to_c_float_array`, but always over a fresh writable copy.

    Use this for C entry points that mutate the buffer in place: unlike
    :func:`_to_c_float_array` (which shares memory with a contiguous ``float32``
    ndarray for speed), this never aliases the caller's array, so processing
    cannot overwrite the input. The single bulk copy is negligible next to the
    DSP work these offline/streaming calls perform.
    """
    buf = np.array(
        _as_float32_buffer(samples, fn_name=fn_name, arg_name=arg_name),
        dtype=np.float32,
        copy=True,
        order="C",
    )
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


def _reject_unrepresentable_int32(values: np.ndarray, name: str) -> None:
    """Refuse an element the ``int32`` cast would change into another legal one.

    The array counterpart of :func:`_narrow_int`, vectorised so the bulk path it
    guards stays a bulk path. It exists because the cast is silent in both
    directions a caller can reach: ``1000.7`` becomes the sample index ``1000``
    and ``2**31`` becomes ``-2**31``, and the C ABI takes ``const int*``, so
    nothing downstream can tell either from a value the caller meant.

    Each refusal names the index and the property that element actually lacks,
    rather than reporting the whole array under one name.
    """
    if values.size == 0:
        return
    if values.dtype.kind == "f":
        unusable = ~np.isfinite(values)
        if unusable.any():
            raise SonareValueError(f"{name}[{int(np.argmax(unusable))}] must be a finite integer")
        fractional = values != np.trunc(values)
        if fractional.any():
            raise SonareValueError(f"{name}[{int(np.argmax(fractional))}] must be an integer")
    elif values.dtype.kind not in "iub":
        # A Python int too wide for int64 arrives as object dtype. It IS an
        # integer, so the range refusal below has to be the one that reports it.
        try:
            values = np.array([operator.index(v) for v in values.ravel()], dtype=object)
        except TypeError as exc:
            raise SonareValueError(f"{name} must hold integers") from exc
    outside = (values < _C_INT_MIN) | (values > _C_INT_MAX)
    if outside.any():
        raise SonareValueError(
            f"{name}[{int(np.argmax(outside))}] must be an integer within "
            f"[{_C_INT_MIN}, {_C_INT_MAX}]"
        )


def _to_c_int_array(
    values: Sequence[int] | list[int], name: str = "values"
) -> tuple[ctypes.Array[ctypes.c_int32], int]:
    # Bulk-marshal via NumPy's vectorised C path instead of `(c_int32*N)(*seq)`,
    # which unpacks every element through Python varargs (mirrors the
    # zero-copy rewrite of `_to_c_float_array`).
    source = np.asarray(values)
    # Checked before the cast rather than after, which is where the fold happens.
    _reject_unrepresentable_int32(source, name)
    buf = np.ascontiguousarray(source.astype(np.int32, copy=False)).reshape(-1)
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


# Bounds are measured from the host's ctypes rather than assumed, so the size_t
# ceiling is the one the loaded library actually uses.
_SIZE_T_MAX = (1 << (ctypes.sizeof(ctypes.c_size_t) * 8)) - 1
_UINT_MAX = (1 << (ctypes.sizeof(ctypes.c_uint) * 8)) - 1
# Fixed-width, unlike the two above: a uint32_t field is 32 bits on every host.
_UINT8_MAX = 2**8 - 1
_UINT16_MAX = 2**16 - 1
_UINT32_MAX = 2**32 - 1
_INT64_MIN = -(2**63)
_INT64_MAX = 2**63 - 1


def _to_c_int(value: object, name: str) -> ctypes.c_int:
    """Narrow a caller-supplied integer onto a C ``int``; see :func:`_narrow_int`."""
    return ctypes.c_int(_narrow_int(value, name, _C_INT_MIN, _C_INT_MAX))


def _to_c_int32(value: object, name: str) -> ctypes.c_int32:
    """Narrow a caller-supplied integer onto ``int32_t``; see :func:`_narrow_int`."""
    return ctypes.c_int32(_narrow_int(value, name, _C_INT_MIN, _C_INT_MAX))


def _to_c_int64(value: object, name: str) -> ctypes.c_int64:
    """Narrow a caller-supplied integer onto ``int64_t``; see :func:`_narrow_int`."""
    return ctypes.c_int64(_narrow_int(value, name, _INT64_MIN, _INT64_MAX))


def _to_c_uint(value: object, name: str) -> ctypes.c_uint:
    """Narrow a caller-supplied integer onto ``unsigned``; see :func:`_narrow_int`."""
    return ctypes.c_uint(_narrow_int(value, name, 0, _UINT_MAX))


def _to_c_uint32(value: object, name: str) -> ctypes.c_uint32:
    """Narrow a caller-supplied integer onto ``uint32_t``; see :func:`_narrow_int`."""
    return ctypes.c_uint32(_narrow_int(value, name, 0, _UINT32_MAX))


def _to_c_uint16(value: object, name: str) -> ctypes.c_uint16:
    """Narrow a caller-supplied integer onto ``uint16_t``; see :func:`_narrow_int`."""
    return ctypes.c_uint16(_narrow_int(value, name, 0, _UINT16_MAX))


def _to_c_uint8(value: object, name: str) -> ctypes.c_uint8:
    """Narrow a caller-supplied integer onto ``uint8_t``; see :func:`_narrow_int`."""
    return ctypes.c_uint8(_narrow_int(value, name, 0, _UINT8_MAX))


def _to_c_size_t(value: object, name: str) -> ctypes.c_size_t:
    """Narrow a caller-supplied integer onto ``size_t``; see :func:`_narrow_int`."""
    return ctypes.c_size_t(_narrow_int(value, name, 0, _SIZE_T_MAX))


def _to_c_float(value: object, name: str) -> ctypes.c_float:
    """Narrow a caller-supplied number onto a C ``float``; see :func:`_narrow_float`."""
    return ctypes.c_float(_narrow_float(value, name))


def _to_c_double(value: object, name: str) -> ctypes.c_double:
    """Narrow a caller-supplied number onto a C ``double``; see :func:`_narrow_double`."""
    return ctypes.c_double(_narrow_double(value, name))


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
) -> str:
    if expected:
        return f"{verb} {what}: {value!r} (expected one of {sorted(names)})"
    return f"{verb} {what}: {value}"


def _resolve_enum(
    value: object,
    names: Mapping[str, int],
    what: str,
    *,
    enum_cls: type | None = None,
    dash: bool = False,
    underscore: bool = False,
    strip: bool = False,
    validate_int: bool = True,
    reject_bool: bool = True,
    verb: str = "unknown",
    expected: bool = True,
) -> int:
    """Resolve a string / int / enum ``value`` to its C enum ordinal.

    ``names`` maps accepted lowercase spellings (with underscores folded to
    dashes when ``dash`` is set) to ordinals. An integer must name one of those
    ordinals, and a ``bool`` is not an ordinal; an ``enum_cls`` instance is
    coerced with ``int()``. Unknown inputs raise :class:`SonareValueError` built
    from ``verb`` / ``what``, listing the accepted names: a refusal that names
    none of them leaves the caller with nothing to try next.

    A table that does not spell every ordinal its field accepts is bounded at
    the call site instead; ``validate_int=False`` is not that tool, because it
    removes the bound rather than widening it.
    """
    if enum_cls is not None and isinstance(value, enum_cls):
        return cast(int, value)
    if isinstance(value, int):
        if (reject_bool and isinstance(value, bool)) or (
            validate_int and value not in names.values()
        ):
            raise SonareValueError(_enum_error(value, names, what, verb, expected))
        return value
    if not isinstance(value, str):
        raise SonareValueError(_enum_error(value, names, what, verb, expected))
    key = value.strip() if strip else value
    if dash:
        key = key.replace("_", "-")
    elif underscore:
        key = key.replace("-", "_")
    key = key.lower()
    if key not in names:
        raise SonareValueError(_enum_error(value, names, what, verb, expected))
    return names[key]


_C_INT_MAX = 2**31 - 1
_C_INT_MIN = -(2**31)


def _int_refusal(fn_name: str, value: object, arg_name: str, domain: str) -> SonareValueError:
    """Word a refusal :func:`_narrow_int` raised, keeping its two halves apart.

    The shared predicate words a bad type and a value out of range identically.
    The validators below name a non-integer as such and name everything else
    against ``domain``, which states the field's whole accepted range rather
    than only the C type's.
    """
    if isinstance(value, bool) or not isinstance(value, SupportsIndex):
        return SonareValueError(f"{fn_name}: {arg_name} must be an integer")
    return SonareValueError(f"{fn_name}: {arg_name} {domain}")


def _validate_c_int_field(fn_name: str, value: int, arg_name: str) -> int:
    """Narrow a config field onto a C ``int``, refusing anything that would wrap.

    ctypes truncates on assignment to a ``c_int32`` field, so a value past the
    signed range reaches the core as a different, legal number instead of being
    refused: ``2**32`` arrives as 0, which every field of the versioned configs
    reads as "keep the default", and ``2**32 + 1`` arrives as 1. Either way the
    call succeeds having used a setting the caller never asked for.

    The check is :func:`_narrow_int`, the one the ``_to_c_*`` readers run, so
    this cannot come to accept a different set of values than they do; only the
    wording is this field's own.

    Args:
        fn_name: Caller name, used to prefix the error message.
        value: Requested field value.
        arg_name: Keyword name, named in the error message.

    Returns:
        The value as a plain ``int``.

    Raises:
        SonareValueError: If the value is not an integer or does not fit.
    """
    try:
        return _narrow_int(value, arg_name, _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise _int_refusal(fn_name, value, arg_name, "must fit in a signed 32-bit integer") from exc


def _validate_hpss_kernel(fn_name: str, value: int, arg_name: str) -> int:
    """Validate an HPSS median-filter kernel against the domain the core accepts.

    One definition for the whole binding: both HPSS entry points narrow the
    kernel into a C ``int``, where a value past the signed range wraps into a
    different kernel the core then happily separates on. The range half is
    :func:`_narrow_int`; odd-and-positive is this domain's own.

    Args:
        fn_name: Caller name, used to prefix the error message.
        value: Requested kernel size.
        arg_name: Keyword name, named in the error message.

    Returns:
        The kernel size as a plain ``int``.

    Raises:
        SonareValueError: If the value is not a positive odd signed 32-bit int.
    """
    domain = "must be a positive odd signed 32-bit integer"
    try:
        kernel = _narrow_int(value, arg_name, _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise _int_refusal(fn_name, value, arg_name, domain) from exc
    if kernel <= 0 or kernel % 2 == 0:
        raise SonareValueError(f"{fn_name}: {arg_name} {domain}")
    return kernel


def _require_power_of_two(value: object, name: str) -> int:
    """Validate a positive power-of-two integer with a consistent error.

    Narrowed before the bit test, which raises a bare :class:`TypeError` on a
    float rather than this family's refusal, and which a caller would otherwise
    reach only after ``int()`` had already truncated the value.
    """
    narrowed = _narrow_int(value, name, _C_INT_MIN, _C_INT_MAX)
    if narrowed <= 0 or (narrowed & (narrowed - 1)) != 0:
        raise SonareValueError(f"{name} must be a positive power of two")
    return narrowed


def _validate_stft_n_fft(fn_name: str, n_fft: int) -> int:
    """Validate an STFT size against the domain the core accepts.

    The core FFT is mixed-radix, so any even size transforms exactly; only the
    real one-sided spectrum's ``n_fft / 2 + 1`` bin layout needs the evenness. A
    power-of-two restriction here would reject sizes the C ABI and the native
    CLI accept, which makes the facade diverge rather than merely be stricter.

    This is the single definition of the even-and-at-least-two domain, reached
    through :func:`_validate_effect_fft_options`, so the entry points that hold
    it cannot drift apart. It is not the binding's only STFT size domain: other
    STFT-framed entry points deliberately require a power of two, and they are
    not stricter versions of this one -- adopt this domain for a new entry point
    only after checking which of the two the C ABI behind it accepts.

    Args:
        fn_name: Caller name, used to prefix the error message.
        n_fft: Requested STFT size.

    Returns:
        The validated size as a plain ``int``.

    Raises:
        SonareValueError: If ``n_fft`` is not an even integer in
            ``[2, 2**31 - 1]``.
    """
    domain = "must be an even signed 32-bit integer >= 2"
    try:
        size = _narrow_int(n_fft, "n_fft", _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise _int_refusal(fn_name, n_fft, "n_fft", domain) from exc
    if size < 2 or size % 2 != 0:
        raise SonareValueError(f"{fn_name}: n_fft {domain}")
    return size


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
    domain = "must fit in a positive signed 32-bit integer"
    try:
        hop = _narrow_int(hop_length, "hop_length", _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise _int_refusal(fn_name, hop_length, "hop_length", domain) from exc
    if hop <= 0:
        raise SonareValueError(f"{fn_name}: hop_length {domain}")
    return n_fft, hop


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
    fn_name: str,
    values: Sequence[float] | list[float] | np.ndarray,
    *args: object,
    arg_name: str = "samples",
) -> list[float]:
    lib = _get_lib()
    # `fn_name` is the C symbol, not the facade, so only the argument name goes on.
    c_array, length = _to_c_float_array(values, arg_name=arg_name)
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
