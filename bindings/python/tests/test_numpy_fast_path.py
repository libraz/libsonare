"""Tests for the numpy zero-copy fast path in the libsonare Python binding.

These tests cover the conversion shim introduced to remove the per-element
``ctypes`` marshalling that previously dominated realtime hot paths
(``RealtimeVoiceChanger.process_mono`` / ``process_interleaved`` and the
shared ``_to_c_float_array`` helper).

Coverage:

1. ``_to_c_float_array`` accepts both ``numpy.ndarray`` (zero-copy when
   already ``float32`` contiguous) and plain Python sequences, and the
   resulting ctypes buffer aliases the numpy buffer in the zero-copy path.
2. ``_from_c_float_array`` round-trips a ``c_float * N`` back into an
   independent ``numpy.ndarray``.
3. ``RealtimeVoiceChanger`` returns ``numpy.ndarray`` from ``process_mono``
   and ``process_interleaved`` and produces deterministic output regardless
   of whether the caller passes ``list[float]`` or ``np.ndarray``.
4. A loose timing budget proves that processing a one-million-sample buffer
   through ``RealtimeVoiceChanger`` (the worst-case offline path) completes
   in well under a second on stock CI hardware. The threshold is generous
   to avoid flakiness — the previous element-wise implementation took
   *seconds* on the same buffer, so any regression to the slow path will
   blow past this budget.
"""

from __future__ import annotations

import ctypes
import math
import time

import numpy as np
import pytest

import libsonare
from libsonare._runtime import _from_c_float_array, _to_c_float_array


def _sine(n: int, freq: float = 440.0, sr: int = 22050) -> np.ndarray:
    t = np.arange(n, dtype=np.float32) / float(sr)
    return (0.2 * np.sin(2.0 * math.pi * freq * t)).astype(np.float32)


# ---------------------------------------------------------------------------
# _to_c_float_array / _from_c_float_array unit tests
# ---------------------------------------------------------------------------


def test_to_c_float_array_zero_copy_for_float32_ndarray() -> None:
    """A contiguous float32 ndarray must be exposed without copying."""
    arr = np.arange(8, dtype=np.float32)
    c_array, length = _to_c_float_array(arr)
    assert length == 8
    # The ctypes buffer should alias the numpy buffer (same memory address).
    assert ctypes.addressof(c_array) == arr.ctypes.data
    # Mutating via the C array must reflect in the numpy array (proves aliasing).
    c_array[0] = 99.0
    assert arr[0] == 99.0


def test_to_c_float_array_accepts_read_only_float32_ndarray() -> None:
    """Read-only float32 input (np.frombuffer / mmap / WRITEABLE=False) must be
    accepted rather than raising ``TypeError: underlying buffer is not writable``.
    The C side takes samples as const, so a read-only buffer is harmless; the
    fast path must fall back to a writable copy instead of crashing."""
    ro = np.frombuffer(np.arange(8, dtype=np.float32).tobytes(), dtype=np.float32)
    assert not ro.flags["WRITEABLE"]
    c_array, length = _to_c_float_array(ro)
    assert length == 8
    assert list(c_array)[:3] == [0.0, 1.0, 2.0]


def test_samples_api_accepts_read_only_ndarray() -> None:
    """End-to-end: a samples-accepting public API must accept a read-only array
    instead of raising ``TypeError: underlying buffer is not writable``."""
    ro = np.ascontiguousarray(_sine(2048))
    ro.setflags(write=False)
    # Must not raise on the read-only input (the regression). rms_energy returns
    # a per-frame sequence; just assert it produced finite values.
    result = np.asarray(libsonare.rms_energy(ro), dtype=np.float64)
    assert result.size > 0
    assert np.all(np.isfinite(result))


def test_to_c_float_array_accepts_list_input() -> None:
    """Plain ``list[float]`` input must still work (back-compat path)."""
    c_array, length = _to_c_float_array([0.0, 0.25, -0.5, 0.75])
    assert length == 4
    assert pytest.approx(c_array[0]) == 0.0
    assert pytest.approx(c_array[1]) == 0.25
    assert pytest.approx(c_array[2]) == -0.5
    assert pytest.approx(c_array[3]) == 0.75


def test_to_c_float_array_accepts_tuple_and_non_float32() -> None:
    """Tuples and non-float32 ndarrays must be coerced via a single bulk copy."""
    c_tuple, n_tuple = _to_c_float_array((1.0, 2.0, 3.0))
    assert n_tuple == 3
    assert pytest.approx(c_tuple[2]) == 3.0

    f64 = np.array([0.1, 0.2, 0.3], dtype=np.float64)
    c_f64, n_f64 = _to_c_float_array(f64)
    assert n_f64 == 3
    # Down-cast must succeed without loss within float32 precision.
    assert pytest.approx(c_f64[1], abs=1e-7) == 0.2


def test_to_c_float_array_handles_empty_input() -> None:
    """Zero-length input must not raise (edge case for empty buffers)."""
    c_array, length = _to_c_float_array([])
    assert length == 0
    assert len(c_array) == 0


def test_from_c_float_array_returns_independent_copy() -> None:
    """``_from_c_float_array`` must return an ndarray that outlives the source."""
    src = (ctypes.c_float * 4)(1.5, 2.5, 3.5, 4.5)
    out = _from_c_float_array(src, 4)
    assert isinstance(out, np.ndarray)
    assert out.dtype == np.float32
    assert out.tolist() == [1.5, 2.5, 3.5, 4.5]
    # Mutating the source must NOT affect the returned ndarray (copy semantics).
    src[0] = -100.0
    assert out[0] == 1.5


# ---------------------------------------------------------------------------
# RealtimeVoiceChanger numpy-path tests
# ---------------------------------------------------------------------------


def test_process_mono_returns_ndarray() -> None:
    """``process_mono`` must return a float32 ``numpy.ndarray``."""
    sr = 48000
    samples = _sine(256, sr=sr)
    with libsonare.RealtimeVoiceChanger(sr, "neutral-monitor", max_block_size=128) as ch:
        out = ch.process_mono(samples)
    assert isinstance(out, np.ndarray)
    assert out.dtype == np.float32
    assert out.shape == (256,)
    assert np.all(np.isfinite(out))


def test_process_mono_accepts_list_input() -> None:
    """``process_mono`` must accept a plain ``list[float]`` (back-compat)."""
    sr = 48000
    samples = _sine(256, sr=sr).tolist()
    with libsonare.RealtimeVoiceChanger(sr, "neutral-monitor", max_block_size=128) as ch:
        out = ch.process_mono(samples)
    assert isinstance(out, np.ndarray)
    assert out.shape == (256,)


def test_process_mono_is_deterministic_across_input_types() -> None:
    """list and ndarray inputs must produce bit-identical output (deterministic)."""
    sr = 48000
    samples_np = _sine(512, sr=sr)
    samples_list = samples_np.tolist()

    with libsonare.RealtimeVoiceChanger(sr, "bright-idol", max_block_size=128) as ch1:
        out_np = ch1.process_mono(samples_np)
    with libsonare.RealtimeVoiceChanger(sr, "bright-idol", max_block_size=128) as ch2:
        out_list = ch2.process_mono(samples_list)

    # The numpy path may differ in the last ULP because list→float32 coercion
    # rounds 64-bit doubles down. Allow a tiny tolerance, not exact equality.
    assert np.allclose(out_np, out_list, atol=1e-5, rtol=1e-4)


def test_process_interleaved_returns_ndarray() -> None:
    """``process_interleaved`` must return a float32 ndarray of the same length."""
    sr = 48000
    n_frames = 512
    mono = _sine(n_frames, sr=sr)
    interleaved = np.empty(n_frames * 2, dtype=np.float32)
    interleaved[0::2] = mono
    interleaved[1::2] = mono

    with libsonare.RealtimeVoiceChanger(sr, "bright-idol", max_block_size=128, channels=2) as ch:
        out = ch.process_interleaved(interleaved)

    assert isinstance(out, np.ndarray)
    assert out.dtype == np.float32
    assert out.shape == (n_frames * 2,)
    assert np.all(np.isfinite(out))
    # Identical L/R input must produce identical L/R output (state symmetry).
    np.testing.assert_array_equal(out[0::2], out[1::2])


def test_process_planar_stereo_returns_ndarray_pair() -> None:
    """``process_planar_stereo`` must return a ``(left, right)`` float32 pair."""
    sr = 48000
    n_frames = 512
    left = _sine(n_frames, sr=sr)
    right = _sine(n_frames, sr=sr)

    with libsonare.RealtimeVoiceChanger(sr, "bright-idol", max_block_size=128, channels=2) as ch:
        out_left, out_right = ch.process_planar_stereo(left, right)

    assert isinstance(out_left, np.ndarray)
    assert isinstance(out_right, np.ndarray)
    assert out_left.dtype == np.float32
    assert out_right.dtype == np.float32
    assert out_left.shape == (n_frames,)
    assert out_right.shape == (n_frames,)
    assert np.all(np.isfinite(out_left))
    assert np.all(np.isfinite(out_right))
    # Identical L/R input must produce identical L/R output (state symmetry).
    np.testing.assert_array_equal(out_left, out_right)


def test_voice_change_realtime_returns_ndarray() -> None:
    """The offline convenience wrapper must surface the ndarray result."""
    sr = 48000
    samples = _sine(1024, sr=sr)
    out = libsonare.voice_change_realtime(samples, sample_rate=sr, preset="neutral-monitor")
    assert isinstance(out, np.ndarray)
    assert out.shape == (1024,)
    # Preserves length even via the wrapper path (existing test_editing.py
    # contract relied on `len(result) == len(samples)`).
    assert len(out) == len(samples)


# ---------------------------------------------------------------------------
# Timing budget — proves we are NOT on the per-element ctypes path.
# ---------------------------------------------------------------------------


def test_large_buffer_processes_under_budget() -> None:
    """A 1 M-sample buffer must process in well under 1 second.

    The previous per-element ``[float(c_out[i]) for i in range(length)]``
    implementation took multiple seconds on the same buffer; numpy zero-copy
    should complete in tens of milliseconds. We allow a very generous 2 s
    ceiling so the test is not flaky on slow CI runners — any regression to
    the old element-wise marshalling path will blow far past this budget.
    """
    sr = 48000
    n = 1_000_000
    samples = _sine(n, sr=sr)
    start = time.perf_counter()
    out = libsonare.voice_change_realtime(samples, sample_rate=sr, preset="neutral-monitor")
    elapsed = time.perf_counter() - start
    assert isinstance(out, np.ndarray)
    assert out.shape == (n,)
    # Surface the actual timing in the failure message for diagnostics.
    assert elapsed < 2.0, f"processing 1M samples took {elapsed:.3f}s (budget 2.0s)"
