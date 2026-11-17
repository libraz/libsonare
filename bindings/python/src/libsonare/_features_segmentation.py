"""Self-similarity, recurrence and segmentation matrices for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

import numpy as np
import numpy.typing as npt

from ._ffi import SonareSegmentIndices, SonareSegmentMatrix
from ._runtime import (
    SonareValueError,
    _check,
    _from_c_float_matrix,
    _from_c_int_array,
    _get_lib,
    _to_c_float_matrix,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _utf8_arg,
)


def _square_input(
    name: str, matrix: npt.ArrayLike, *, arg_name: str
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    c_matrix, rows, cols = _to_c_float_matrix(name, matrix, arg_name=arg_name)
    if rows != cols:
        raise SonareValueError(f"{name}: {arg_name} must be square, got shape ({rows}, {cols})")
    return c_matrix, rows


def _segment_matrix_result(lib: ctypes.CDLL, out: SonareSegmentMatrix) -> np.ndarray:
    try:
        return _from_c_float_matrix(out.values, out.rows, out.cols)
    finally:
        lib.sonare_free_segment_matrix(ctypes.byref(out))


def _segment_indices_result(lib: ctypes.CDLL, out: SonareSegmentIndices) -> np.ndarray:
    try:
        return _from_c_int_array(out.values, out.count)
    finally:
        lib.sonare_free_segment_indices(ctypes.byref(out))


def segment_cross_similarity(
    x: npt.ArrayLike,
    y: npt.ArrayLike,
    k: int = 0,
    metric: str = "cosine",
    mode: str = "connectivity",
) -> np.ndarray:
    """Return column-wise cross-similarity (``librosa.segment.cross_similarity``).

    ``x`` and ``y`` are 2-D feature arrays of shape ``(features, frames)``. The
    result is a float32 array of shape ``(frames_y, frames_x)``.
    """
    x_array, x_rows, x_cols = _to_c_float_matrix("segment_cross_similarity", x, arg_name="x")
    y_array, y_rows, y_cols = _to_c_float_matrix("segment_cross_similarity", y, arg_name="y")
    out = SonareSegmentMatrix()
    rc = _get_lib().sonare_segment_cross_similarity(
        x_array,
        _to_c_int(x_rows, "x_rows"),
        _to_c_int(x_cols, "x_cols"),
        y_array,
        _to_c_int(y_rows, "y_rows"),
        _to_c_int(y_cols, "y_cols"),
        _to_c_int(k, "k"),
        _utf8_arg(metric, "metric"),
        _utf8_arg(mode, "mode"),
        ctypes.byref(out),
    )
    _check(rc)
    return _segment_matrix_result(_get_lib(), out)


def segment_recurrence_matrix(
    data: npt.ArrayLike,
    k: int = 0,
    width: int = 1,
    sym: bool = False,
    metric: str = "euclidean",
    mode: str = "connectivity",
) -> np.ndarray:
    """Return a self-similarity recurrence matrix (``librosa.segment.recurrence_matrix``).

    ``data`` is a 2-D feature array of shape ``(features, frames)``. The result is
    a float32 array of shape ``(frames, frames)``.
    """
    c_data, rows, cols = _to_c_float_matrix("segment_recurrence_matrix", data, arg_name="data")
    lib = _get_lib()
    out = SonareSegmentMatrix()
    rc = lib.sonare_segment_recurrence_matrix(
        c_data,
        _to_c_int(rows, "rows"),
        _to_c_int(cols, "cols"),
        _to_c_int(k, "k"),
        _to_c_int(width, "width"),
        1 if sym else 0,
        _utf8_arg(metric, "metric"),
        _utf8_arg(mode, "mode"),
        ctypes.byref(out),
    )
    _check(rc)
    return _segment_matrix_result(lib, out)


def segment_recurrence_to_lag(
    recurrence: npt.ArrayLike,
    pad: bool = True,
) -> np.ndarray:
    """Convert a square recurrence matrix to a lag-by-time matrix.

    Matches ``librosa.segment.recurrence_to_lag``: ``lag[l, i] = rec[(i + l) % n_lags, i]``
    with ``n_lags = 2n`` when ``pad`` (negative lags wrapped into the lower half) and ``n``
    otherwise. ``recurrence`` is a 2-D array of shape ``(n, n)``.
    """
    c_recurrence, n = _square_input("segment_recurrence_to_lag", recurrence, arg_name="recurrence")
    lib = _get_lib()
    out = SonareSegmentMatrix()
    _check(
        lib.sonare_segment_recurrence_to_lag(
            c_recurrence, _to_c_int(n, "n"), 1 if pad else 0, ctypes.byref(out)
        )
    )
    return _segment_matrix_result(lib, out)


def segment_lag_to_recurrence(lag: npt.ArrayLike) -> np.ndarray:
    """Convert a lag matrix back to an ``(n, n)`` recurrence matrix.

    ``lag`` is a 2-D array of shape ``(n_lags, n)``, where ``n_lags`` is ``n`` for the
    unpadded layout and ``2 * n`` for the padded one.
    """
    c_lag, n_lags, n_rows = _to_c_float_matrix("segment_lag_to_recurrence", lag, arg_name="lag")
    lib = _get_lib()
    out = SonareSegmentMatrix()
    _check(
        lib.sonare_segment_lag_to_recurrence(
            c_lag, _to_c_int(n_rows, "n_rows"), _to_c_int(n_lags, "n_lags"), ctypes.byref(out)
        )
    )
    return _segment_matrix_result(lib, out)


def segment_subsegment(
    data: npt.ArrayLike,
    boundaries: Sequence[int] | list[int],
    n_segments: int = 4,
) -> np.ndarray:
    """Refine frame boundaries by clustering each parent segment.

    ``data`` is a 2-D feature array of shape ``(features, frames)``. Returns an
    int32 array of frame indices.
    """
    c_data, rows, cols = _to_c_float_matrix("segment_subsegment", data, arg_name="data")
    c_boundaries, count = _to_c_int_array(boundaries, "boundaries")
    lib = _get_lib()
    out = SonareSegmentIndices()
    _check(
        lib.sonare_segment_subsegment(
            c_data,
            _to_c_int(rows, "rows"),
            _to_c_int(cols, "cols"),
            c_boundaries,
            _to_c_size_t(count, "boundaries length"),
            _to_c_int(n_segments, "n_segments"),
            ctypes.byref(out),
        )
    )
    return _segment_indices_result(lib, out)


def segment_agglomerative(
    data: npt.ArrayLike,
    k: int,
    linkage: str = "average",
) -> np.ndarray:
    """Cluster feature columns and return one label per column as an int32 array.

    ``data`` is a 2-D feature array of shape ``(features, frames)``.
    """
    c_data, rows, cols = _to_c_float_matrix("segment_agglomerative", data, arg_name="data")
    lib = _get_lib()
    out = SonareSegmentIndices()
    _check(
        lib.sonare_segment_agglomerative(
            c_data,
            _to_c_int(rows, "rows"),
            _to_c_int(cols, "cols"),
            _to_c_int(k, "k"),
            _utf8_arg(linkage, "linkage"),
            ctypes.byref(out),
        )
    )
    return _segment_indices_result(lib, out)


def segment_path_enhance(
    recurrence: npt.ArrayLike,
    win: int,
    max_ratio: int = 2,
    min_ratio: int = 0,
    n_filters: int = 7,
) -> np.ndarray:
    """Enhance diagonal paths in a square recurrence matrix of shape ``(n, n)``."""
    c_recurrence, n = _square_input("segment_path_enhance", recurrence, arg_name="recurrence")
    lib = _get_lib()
    out = SonareSegmentMatrix()
    _check(
        lib.sonare_segment_path_enhance(
            c_recurrence,
            _to_c_int(n, "n"),
            _to_c_int(win, "win"),
            _to_c_int(max_ratio, "max_ratio"),
            _to_c_int(min_ratio, "min_ratio"),
            _to_c_int(n_filters, "n_filters"),
            ctypes.byref(out),
        )
    )
    return _segment_matrix_result(lib, out)
