"""Shared plumbing for the mastering repair wrappers."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._runtime import (
    SonareValueError,
    _as_float32_buffer,
    _check,
    _from_c_float_array,
    _get_lib,
    _not_planar_channels,
    _out_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_samples,
)


def _run_detection(
    lib_fn: Any,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config: Any,
    detection_type: Any,
) -> Any:
    """Call one measure-only repair entry and hand back the filled C struct.

    These allocate nothing -- the result is a small POD struct the caller
    supplies -- so unlike :func:`_run_repair` there is nothing to release and no
    pointer to read. A refused call zeroes the struct before it returns, which
    the ``_check`` below turns into an exception anyway.
    """
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    out = detection_type()
    rc = lib_fn(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    return out


def _run_repair(
    lib_fn: Any,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config: Any,
) -> np.ndarray:
    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    with _out_float_array(lib) as (out, out_length):
        rc = lib_fn(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        _check(rc)
        return _from_c_float_array(out, out_length.value)


def _linked_channel_planes(
    fn_name: str,
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
) -> list[np.ndarray]:
    """Coerce and preflight the channel set of an N-channel linked repair.

    The rank and emptiness rejections are the ones
    :func:`_planar_channel_arrays` raises; what is added here is a per-channel
    non-finite scan that names WHICH channel. The C entry validates every
    plane rather than only the first, and an index in the message is the
    difference between a caller checking one buffer and checking all of them.
    """
    if isinstance(channels, np.ndarray):
        if channels.ndim != 2:
            raise _not_planar_channels(channels, "channels")
        planes: list[Any] = list(channels)
    else:
        try:
            planes = list(channels)
        except TypeError as exc:
            raise _not_planar_channels(channels, "channels") from exc
    if not planes:
        raise SonareValueError(f"{fn_name}: channels must not be empty")
    return [
        _validate_samples(fn_name, plane, arg_name=f"channels[{index}]")
        for index, plane in enumerate(planes)
    ]


def _linked_output_planes(
    channel_count: int, frame_count: int
) -> tuple[list[np.ndarray], ctypes.Array[Any]]:
    """Allocate the caller-owned output planes an N-channel linked repair writes.

    Nothing is heap-allocated by these C entries, so there is no result struct
    to free: the planes handed in are the ones that come back, which is why the
    numpy buffers are returned alongside the pointer table.
    """
    buffers = [np.zeros(frame_count, dtype=np.float32) for _ in range(channel_count)]
    arrays = [(ctypes.c_float * frame_count).from_buffer(buffer) for buffer in buffers]
    ptr_type = ctypes.POINTER(ctypes.c_float) * channel_count
    ptrs = ptr_type(*[ctypes.cast(array, ctypes.POINTER(ctypes.c_float)) for array in arrays])
    setattr(ptrs, "_plane_arrays", arrays)  # noqa: B010 -- pin the views for the call's duration.
    return buffers, ptrs
