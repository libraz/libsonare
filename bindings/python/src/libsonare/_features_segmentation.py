"""Self-similarity, recurrence and segmentation matrices for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import SonareSegmentIndices, SonareSegmentMatrix
from ._runtime import (
    SonareValueError,
    _check,
    _float_array_result,
    _from_c_int_array,
    _get_lib,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _validate_samples,
)
from .types import SegmentMatrix


def _segment_input(
    name: str,
    data: Sequence[float] | list[float],
    rows: int,
    cols: int,
    *,
    arg_name: str = "data",
    dims_prefix: str = "",
) -> tuple[ctypes.Array[ctypes.c_float], int]:
    # ``arg_name`` / ``dims_prefix`` name the caller's own parameters, so a
    # two-matrix entry point such as `cross_similarity` reports `x` / `x_rows`
    # rather than the helper's internal `data` / `rows` spelling.
    rows_name, cols_name = f"{dims_prefix}rows", f"{dims_prefix}cols"
    if rows <= 0 or cols <= 0:
        raise SonareValueError(f"{name}: {rows_name} and {cols_name} must be positive")
    values = _validate_samples(name, data, arg_name=arg_name)
    if len(values) != rows * cols:
        raise SonareValueError(f"{name}: {arg_name} length must equal {rows_name} * {cols_name}")
    return _to_c_float_array(values)


def _segment_matrix_result(lib: ctypes.CDLL, out: SonareSegmentMatrix) -> SegmentMatrix:
    try:
        values = _float_array_result(out.values, out.rows * out.cols)
        return SegmentMatrix(out.rows, out.cols, values)
    finally:
        lib.sonare_free_segment_matrix(ctypes.byref(out))


def _segment_indices_result(lib: ctypes.CDLL, out: SonareSegmentIndices) -> list[int]:
    try:
        return [int(v) for v in _from_c_int_array(out.values, out.count)]
    finally:
        lib.sonare_free_segment_indices(ctypes.byref(out))


def cross_similarity(
    x: Sequence[float] | list[float],
    x_rows: int,
    x_cols: int,
    y: Sequence[float] | list[float],
    y_rows: int,
    y_cols: int,
    k: int = 0,
    metric: str = "cosine",
    mode: str = "connectivity",
) -> SegmentMatrix:
    """Return column-wise cross-similarity (``librosa.segment.cross_similarity``)."""
    x_array, _ = _segment_input(
        "cross_similarity", x, x_rows, x_cols, arg_name="x", dims_prefix="x_"
    )
    y_array, _ = _segment_input(
        "cross_similarity", y, y_rows, y_cols, arg_name="y", dims_prefix="y_"
    )
    out = SonareSegmentMatrix()
    rc = _get_lib().sonare_segment_cross_similarity(
        x_array,
        _to_c_int(x_rows, "x_rows"),
        _to_c_int(x_cols, "x_cols"),
        y_array,
        _to_c_int(y_rows, "y_rows"),
        _to_c_int(y_cols, "y_cols"),
        _to_c_int(k, "k"),
        metric.encode(),
        mode.encode(),
        ctypes.byref(out),
    )
    _check(rc)
    return _segment_matrix_result(_get_lib(), out)


def recurrence_matrix(
    data: Sequence[float] | list[float],
    rows: int,
    cols: int,
    k: int = 0,
    width: int = 1,
    sym: bool = False,
    metric: str = "euclidean",
    mode: str = "connectivity",
) -> SegmentMatrix:
    """Return a self-similarity recurrence matrix (``librosa.segment.recurrence_matrix``)."""
    c_data, _ = _segment_input("recurrence_matrix", data, rows, cols)
    lib = _get_lib()
    out = SonareSegmentMatrix()
    rc = lib.sonare_segment_recurrence_matrix(
        c_data,
        _to_c_int(rows, "rows"),
        _to_c_int(cols, "cols"),
        _to_c_int(k, "k"),
        _to_c_int(width, "width"),
        1 if sym else 0,
        metric.encode(),
        mode.encode(),
        ctypes.byref(out),
    )
    _check(rc)
    return _segment_matrix_result(lib, out)


def recurrence_to_lag(
    recurrence: Sequence[float] | list[float],
    n: int,
    pad: bool = False,
) -> SegmentMatrix:
    """Convert an ``n × n`` recurrence matrix to a lag matrix."""
    c_recurrence, _ = _segment_input("recurrence_to_lag", recurrence, n, n, arg_name="recurrence")
    lib = _get_lib()
    out = SonareSegmentMatrix()
    _check(
        lib.sonare_segment_recurrence_to_lag(
            c_recurrence, _to_c_int(n, "n"), 1 if pad else 0, ctypes.byref(out)
        )
    )
    return _segment_matrix_result(lib, out)


def lag_to_recurrence(
    lag: Sequence[float] | list[float],
    n_rows: int,
    n_lags: int,
) -> SegmentMatrix:
    """Convert a lag matrix back to an ``n_rows × n_rows`` recurrence matrix."""
    c_lag, _ = _segment_input("lag_to_recurrence", lag, n_rows, n_lags, arg_name="lag")
    lib = _get_lib()
    out = SonareSegmentMatrix()
    _check(
        lib.sonare_segment_lag_to_recurrence(
            c_lag, _to_c_int(n_rows, "n_rows"), _to_c_int(n_lags, "n_lags"), ctypes.byref(out)
        )
    )
    return _segment_matrix_result(lib, out)


def subsegment(
    data: Sequence[float] | list[float],
    rows: int,
    cols: int,
    boundaries: Sequence[int] | list[int],
    n_segments: int = 4,
) -> list[int]:
    """Refine frame boundaries by clustering each parent segment."""
    c_data, _ = _segment_input("subsegment", data, rows, cols)
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


def agglomerative(
    data: Sequence[float] | list[float],
    rows: int,
    cols: int,
    k: int,
    linkage: str = "average",
) -> list[int]:
    """Cluster feature columns and return one label per column."""
    c_data, _ = _segment_input("agglomerative", data, rows, cols)
    lib = _get_lib()
    out = SonareSegmentIndices()
    _check(
        lib.sonare_segment_agglomerative(
            c_data,
            _to_c_int(rows, "rows"),
            _to_c_int(cols, "cols"),
            _to_c_int(k, "k"),
            linkage.encode(),
            ctypes.byref(out),
        )
    )
    return _segment_indices_result(lib, out)


def path_enhance(
    recurrence: Sequence[float] | list[float],
    n: int,
    win: int,
    max_ratio: int = 2,
    min_ratio: int = 0,
    n_filters: int = 7,
) -> SegmentMatrix:
    """Enhance diagonal paths in a recurrence matrix."""
    c_recurrence, _ = _segment_input("path_enhance", recurrence, n, n, arg_name="recurrence")
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
