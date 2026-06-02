"""Shared runtime helpers for the libsonare Python binding."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np

# _runtime is the shared re-export hub: feature submodules do
# `from ._runtime import *`, so forward the full C-struct and public type
# surfaces here instead of maintaining a partial hand-written list (an
# incomplete list silently breaks submodules at runtime with NameError).
from ._ffi import *  # noqa: F403
from .types import *  # noqa: F403

PAN_MODE_BALANCE = 0
PAN_MODE_STEREO_PAN = 1
PAN_MODE_DUAL_PAN = 2

_lib: ctypes.CDLL | None = None


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
            raise RuntimeError(detail_str)
        msg = lib.sonare_error_message(rc)
        raise RuntimeError(msg.decode("utf-8") if msg else f"sonare error {rc}")


def _validate_samples(
    fn_name: str,
    samples: object,
    *,
    validate: bool = True,
    arg_name: str = "samples",
) -> np.ndarray:
    """Coerce ``samples`` to a contiguous float32 buffer and apply input guards.

    Always rejects empty buffers with ``ValueError``. When ``validate`` is True
    (the default), additionally scans for NaN / Inf and raises ``ValueError``
    on the first offending index. Hot paths may pass ``validate=False`` to
    skip the O(n) scan.
    """
    buf = _as_float32_buffer(samples)
    if int(buf.shape[0]) == 0:
        raise ValueError(f"{fn_name}: {arg_name} must not be empty")
    if validate:
        # `np.isfinite` is vectorised C, so this stays cheap relative to the
        # actual DSP call but lets us surface the *index* of the bad value.
        finite = np.isfinite(buf)
        if not bool(finite.all()):
            bad = int(np.argmin(finite))
            raise ValueError(f"{fn_name}: {arg_name} contains NaN or Inf at index {bad}")
    return buf


def _validate_scalar(fn_name: str, value: float, arg_name: str) -> float:
    """Reject NaN / Inf scalar inputs with ``ValueError``."""
    v = float(value)
    if not np.isfinite(v):
        raise ValueError(f"{fn_name}: {arg_name} must be a finite number")
    return v


def _as_float32_buffer(samples: object) -> np.ndarray:
    """Coerce ``samples`` to a contiguous ``float32`` 1-D numpy buffer.

    Zero-copy when the input is already a contiguous ``float32`` ndarray; one
    bulk C-level copy otherwise (``np.ascontiguousarray`` for non-contig
    float32 input, ``np.asarray`` for lists/tuples/array.array).
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
    # list / tuple / array.array / generator → bulk-convert via NumPy's
    # vectorised C path (orders of magnitude faster than `(c_float*N)(*seq)`).
    return np.ascontiguousarray(np.asarray(samples, dtype=np.float32)).reshape(-1)


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
    c_array._np_backing = buf  # type: ignore[attr-defined]
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
        return np.frombuffer(array, dtype=np.float32, count=count).copy()
    # POINTER(c_float) path: materialise a fixed-size view at the same address.
    arr_type = ctypes.c_float * count
    view = arr_type.from_address(ctypes.addressof(array.contents))  # type: ignore[union-attr]
    return np.frombuffer(view, dtype=np.float32, count=count).copy()


def _from_c_int_array(array: object, count: int) -> np.ndarray:
    """Copy a C ``int32*`` (or fixed-length ``c_int32 * N`` array) into numpy.

    Integer mirror of :func:`_from_c_float_array`: accepts a ``ctypes.Array``
    or ``POINTER(c_int32)`` and returns an independent ``int32`` ndarray so the
    C-side allocation may be freed immediately afterwards.
    """
    if count <= 0:
        return np.empty(0, dtype=np.int32)
    if isinstance(array, ctypes.Array):
        return np.frombuffer(array, dtype=np.int32, count=count).copy()
    arr_type = ctypes.c_int32 * count
    view = arr_type.from_address(ctypes.addressof(array.contents))  # type: ignore[union-attr]
    return np.frombuffer(view, dtype=np.int32, count=count).copy()


def _to_c_int_array(values: Sequence[int] | list[int]) -> tuple[ctypes.Array[ctypes.c_int32], int]:
    # Bulk-marshal via NumPy's vectorised C path instead of `(c_int32*N)(*seq)`,
    # which unpacks every element through Python varargs (mirrors the
    # zero-copy rewrite of `_to_c_float_array`).
    buf = np.ascontiguousarray(np.asarray(values, dtype=np.int32)).reshape(-1)
    length = int(buf.shape[0])
    if length == 0:  # noqa: SIM108
        c_array = (ctypes.c_int32 * 0)()
    else:
        c_array = (ctypes.c_int32 * length).from_buffer(buf)
    # Pin the numpy buffer so the backing memory outlives the C call.
    c_array._np_backing = buf  # type: ignore[attr-defined]
    return c_array, length


def _pan_mode_value(value: str | int) -> int:
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    if key == "balance":
        return PAN_MODE_BALANCE
    if key in ("stereo-pan", "stereopan", "pan"):
        return PAN_MODE_STEREO_PAN
    if key in ("dual-pan", "dualpan"):
        return PAN_MODE_DUAL_PAN
    raise ValueError(f"unknown pan mode: {value}")


def _curve_value(value: AutomationCurve | str | int) -> int:
    """Resolve an automation curve to its C enum value."""
    if isinstance(value, AutomationCurve):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.lower()
    if key in ("linear", "lin"):
        return int(AutomationCurve.LINEAR)
    if key in ("exponential", "exp"):
        return int(AutomationCurve.EXPONENTIAL)
    if key in ("hold", "step"):
        return int(AutomationCurve.HOLD)
    if key in ("s-curve", "s_curve", "scurve", "smooth"):
        return int(AutomationCurve.S_CURVE)
    raise ValueError(f"unknown automation curve: {value}")


def _pan_law_value(value: PanLaw | str | int) -> int:
    """Resolve a pan law to its C enum value (0=-3dB, 1=-4.5dB, 2=-6dB, 3=linear)."""
    if isinstance(value, PanLaw):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    mapping = {
        "const-3db": PanLaw.CONST_3DB,
        "-3db": PanLaw.CONST_3DB,
        "const-4.5db": PanLaw.CONST_4_5DB,
        "-4.5db": PanLaw.CONST_4_5DB,
        "const-6db": PanLaw.CONST_6DB,
        "-6db": PanLaw.CONST_6DB,
        "linear": PanLaw.LINEAR_0DB,
        "linear-0db": PanLaw.LINEAR_0DB,
        "0db": PanLaw.LINEAR_0DB,
    }
    if key not in mapping:
        raise ValueError(f"unknown pan law: {value}")
    return int(mapping[key])


def _meter_tap_value(value: MeterTap | str | int) -> int:
    """Resolve a meter tap point to its C enum value (0 pre-fader, 1 post-fader)."""
    if isinstance(value, MeterTap):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    if key in ("pre-fader", "pre", "prefader"):
        return int(MeterTap.PRE_FADER)
    if key in ("post-fader", "post", "postfader"):
        return int(MeterTap.POST_FADER)
    raise ValueError(f"unknown meter tap: {value}")


def _send_timing_value(value: SendTiming | str | int) -> int:
    """Resolve a send timing to its C enum value (0 pre-fader, 1 post-fader)."""
    if isinstance(value, SendTiming):
        return int(value)
    if isinstance(value, int):
        return value
    key = value.replace("_", "-").lower()
    if key in ("pre-fader", "pre", "prefader"):
        return int(SendTiming.PRE_FADER)
    if key in ("post-fader", "post", "postfader"):
        return int(SendTiming.POST_FADER)
    raise ValueError(f"unknown send timing: {value}")


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
    names = {
        "major": Mode.MAJOR,
        "maj": Mode.MAJOR,
        "minor": Mode.MINOR,
        "min": Mode.MINOR,
        "m": Mode.MINOR,
        "dorian": Mode.DORIAN,
        "phrygian": Mode.PHRYGIAN,
        "lydian": Mode.LYDIAN,
        "mixolydian": Mode.MIXOLYDIAN,
        "locrian": Mode.LOCRIAN,
    }
    out: list[int] = []
    for mode in modes:
        if isinstance(mode, str):
            key = mode.lower()
            if key not in names:
                raise ValueError(f"invalid mode: {mode}")
            out.append(int(names[key]))
        else:
            out.append(int(Mode(mode)))
    return out


def _profile_value(profile: KeyProfile | str | None) -> int:
    if profile is None:
        return int(KeyProfile.KRUMHANSL_SCHMUCKLER)
    if isinstance(profile, str):
        names = {
            "ks": KeyProfile.KRUMHANSL_SCHMUCKLER,
            "krumhansl": KeyProfile.KRUMHANSL_SCHMUCKLER,
            "krumhansl-schmuckler": KeyProfile.KRUMHANSL_SCHMUCKLER,
            "temperley": KeyProfile.TEMPERLEY,
            "shaath": KeyProfile.SHAATH,
            "keyfinder": KeyProfile.SHAATH,
            "faraldo-edmt": KeyProfile.FARALDO_EDMT,
            "edmt": KeyProfile.FARALDO_EDMT,
            "faraldo-edma": KeyProfile.FARALDO_EDMA,
            "edma": KeyProfile.FARALDO_EDMA,
            "faraldo-edmm": KeyProfile.FARALDO_EDMM,
            "edmm": KeyProfile.FARALDO_EDMM,
            "bellman-budge": KeyProfile.BELLMAN_BUDGE,
            "bellman": KeyProfile.BELLMAN_BUDGE,
        }
        key = profile.lower()
        if key not in names:
            raise ValueError(f"invalid key profile: {profile}")
        return int(names[key])
    return int(KeyProfile(profile))


def _float_array_result(out: ctypes.POINTER(ctypes.c_float), count: int) -> list[float]:
    # Bulk C-side copy via `_from_c_float_array`, then `.tolist()` to honour the
    # documented `list[float]` return contract (callers index/iterate as lists).
    return _from_c_float_array(out, count).tolist()


def _optional_float_array_result(out: ctypes.POINTER(ctypes.c_float), count: int) -> list[float]:
    # A null pointer means the array was not computed (e.g. clarity bands in
    # blind mode); represent that as an empty list rather than crashing.
    if not out:
        return []
    return _from_c_float_array(out, count).tolist()


def _int_array_result(out: ctypes.POINTER(ctypes.c_int), count: int) -> list[int]:
    return _from_c_int_array(out, count).tolist()


def _call_float_transform(
    fn_name: str, values: Sequence[float] | list[float], *args: object
) -> list[float]:
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = getattr(lib, fn_name)(
        c_array,
        ctypes.c_size_t(length),
        *args,
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


__all__ = [name for name in globals() if not name.startswith("__")]
