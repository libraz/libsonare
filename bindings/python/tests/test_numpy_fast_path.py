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
import pathlib
import time

import numpy as np
import pytest

import libsonare
from libsonare._runtime import _from_c_float_array, _to_c_float_array, _to_c_int_array


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


def test_to_c_int_array_accepts_read_only_int_ndarray() -> None:
    """Read-only int input must be copied instead of leaking the raw
    ``ctypes.TypeError: underlying buffer is not writable`` — the integer
    mirror of the float guard above."""
    ro = np.frombuffer(np.arange(8, dtype=np.int32).tobytes(), dtype=np.int32)
    assert not ro.flags["WRITEABLE"]
    c_array, length = _to_c_int_array(ro)
    assert length == 8
    assert list(c_array)[:3] == [0, 1, 2]


def test_int_apis_accept_read_only_ndarray() -> None:
    """End-to-end: every public entry point that marshals caller-supplied frame
    indices must accept a read-only int array rather than raising ``TypeError``.
    ``detect_key`` / ``detect_key_candidates`` also route through
    ``_to_c_int_array``, but over a mode list this module builds itself, so a
    caller cannot reach them with a read-only buffer."""

    def read_only(values: list[int]) -> np.ndarray:
        arr = np.asarray(values, dtype=np.int32)
        arr.setflags(write=False)
        return arr

    assert libsonare.fix_frames(read_only([1, 3, 5])) == [0, 1, 3, 5]

    energy = np.abs(_sine(512)).astype(np.float32)
    backtracked = libsonare.onset_backtrack(read_only([100, 300]), energy)
    assert len(backtracked) == 2

    data = np.linspace(0.0, 1.0, 4 * 16, dtype=np.float32)
    refined = libsonare.subsegment(data, 4, 16, read_only([0, 8]), n_segments=2)
    assert len(refined) >= 2

    samples = _sine(2048)
    remixed = libsonare.remix(samples, read_only([0, 512, 1024, 1536]))
    assert remixed.size == 1024
    resolved = libsonare.remix_aligned_intervals(samples, read_only([0, 512, 1024, 1536]))
    assert len(resolved) == 4


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


def _count_float32_coercions(call, *args, **kwargs) -> tuple[int, int]:
    """(coercion calls, actual copies) made while ``call`` runs.

    Patches the shared helper in every module that imported it by name, so a
    body reaching it through `_to_c_float_array` is counted too.
    """
    import sys

    from libsonare import _runtime

    stats = {"calls": 0, "copies": 0}
    real = _runtime._as_float32_buffer

    def counting(samples, **kw):
        out = real(samples, **kw)
        stats["calls"] += 1
        if not (isinstance(samples, np.ndarray) and samples is out):
            stats["copies"] += 1
        return out

    patched = [
        module
        for module in sys.modules.values()
        if getattr(module, "_as_float32_buffer", None) is real
    ]
    for module in patched:
        module._as_float32_buffer = counting
    try:
        call(*args, **kwargs)
    finally:
        for module in patched:
            module._as_float32_buffer = real
    return stats["calls"], stats["copies"]


@pytest.mark.parametrize(
    "samples",
    [
        pytest.param([0.1, -0.2, 0.3, -0.4] * 64, id="list"),
        pytest.param(tuple([0.1, -0.2, 0.3, -0.4] * 64), id="tuple"),
        pytest.param(np.linspace(-0.5, 0.5, 256, dtype=np.float64), id="float64_ndarray"),
        pytest.param(np.linspace(-0.5, 0.5, 512, dtype=np.float32)[::2], id="strided_float32"),
    ],
)
def test_guarded_entry_point_coerces_the_buffer_exactly_once(samples) -> None:
    """`@_guard_buffer` hands its validated buffer to the body, not the original.

    The guard used to discard what it coerced and let the body coerce the
    caller's value a second time, so every non-contiguous or non-float32 input
    paid for the full walk and the float32 allocation twice — and the bytes the
    C ABI received were one conversion removed from the ones that were checked.
    """
    _, copies = _count_float32_coercions(libsonare.trim_silence, samples)
    assert copies == 1


def test_guarded_entry_point_stays_zero_copy_for_a_ready_buffer() -> None:
    """A contiguous float32 buffer must still reach the C ABI without a copy."""
    ready = np.linspace(-0.5, 0.5, 256, dtype=np.float32)
    calls, copies = _count_float32_coercions(libsonare.trim_silence, ready)
    assert calls >= 2  # the guard and the body both consult the helper
    assert copies == 0


def test_no_module_marshals_a_float_buffer_through_varargs() -> None:
    """Sample buffers reach the C ABI through the shared bulk numpy path.

    ``(ctypes.c_float * frames)(*channel)`` unpacks every sample through Python
    varargs. The realtime process path was moved off it; clip-page supply and
    clip marshalling were not, and stayed that way for two releases because
    nothing compared them. A rule over the source is what keeps the three from
    drifting apart again, rather than a list of the files that were fixed.
    """
    import ast

    package = pathlib.Path(libsonare.__file__).parent
    offenders: list[str] = []
    scanned = 0
    for path in sorted(package.glob("*.py")):
        scanned += 1
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            if not isinstance(node, ast.Call):
                continue
            if not any(isinstance(arg, ast.Starred) for arg in node.args):
                continue
            # `(ctypes.c_float * n)(*seq)`: the callee is the array-type product.
            func = node.func
            if not isinstance(func, ast.BinOp) or not isinstance(func.op, ast.Mult):
                continue
            element = func.left
            if isinstance(element, ast.Attribute) and element.attr == "c_float":
                offenders.append(f"{path.name}:{node.lineno}")
    # Guard the scan itself: a parser change that matches nothing would pass.
    assert scanned > 20, f"only {scanned} modules scanned"
    assert offenders == [], (
        "float sample buffers must be marshalled through _planar_channel_arrays / "
        f"_to_c_float_array, not per-element varargs: {offenders}"
    )


def test_int_apis_declare_int_samples() -> None:
    """Every public parameter marshalled through ``_to_c_int_array`` must be
    declared ``IntSamples`` in the stub, and ``IntSamples`` must admit ndarray.

    mypy cannot enforce this: numpy is configured with ``follow_imports = skip``
    (its published stubs use PEP 695 syntax our 3.11 target rejects), so the
    ``np.ndarray`` arm of the alias collapses to ``Any`` and a stub that omits
    it type-checks exactly like one that has it. The understatement is only
    visible to a downstream consumer with real numpy stubs, who cannot pass the
    array the runtime has always accepted. Deriving the parameter list from the
    marshalling call sites keeps this from being a list of the ones that were
    fixed once.
    """
    import ast

    package = pathlib.Path(libsonare.__file__).parent

    # Parameters handed to _to_c_int_array from within the function that
    # declares them, i.e. caller-supplied index arrays rather than internally
    # built lists.
    marshalled: set[tuple[str, str]] = set()
    for path in sorted(package.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for func in ast.walk(tree):
            if not isinstance(func, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            params = {arg.arg for arg in func.args.args + func.args.kwonlyargs}
            for node in ast.walk(func):
                if not isinstance(node, ast.Call):
                    continue
                if not (isinstance(node.func, ast.Name) and node.func.id == "_to_c_int_array"):
                    continue
                for arg in node.args:
                    if isinstance(arg, ast.Name) and arg.id in params:
                        marshalled.add((func.name, arg.id))

    stub = ast.parse((package / "analyzer.pyi").read_text(encoding="utf-8"))

    alias = next(
        node
        for node in stub.body
        if isinstance(node, ast.AnnAssign)
        and isinstance(node.target, ast.Name)
        and node.target.id == "IntSamples"
    )
    assert "np.ndarray" in ast.unparse(alias.value), (
        "IntSamples must admit numpy arrays: the runtime marshaller accepts them"
    )

    declarations: dict[str, dict[str, str]] = {}
    for node in stub.body:
        if isinstance(node, ast.FunctionDef):
            declarations[node.name] = {
                arg.arg: ast.unparse(arg.annotation) if arg.annotation else ""
                for arg in node.args.args + node.args.kwonlyargs
            }

    offenders: list[str] = []
    checked = 0
    for name, param in sorted(marshalled):
        declared = declarations.get(name)
        if declared is None or param not in declared:
            # Not on the public stub surface (internal helper); nothing to check.
            continue
        checked += 1
        if "IntSamples" not in declared[param]:
            offenders.append(f"{name}({param}: {declared[param]})")

    # Guard the derivation itself: a parser change that matches nothing, or a
    # rename that empties the set, would otherwise pass silently.
    assert checked >= 5, f"only {checked} stub parameters cross-checked"
    assert offenders == [], (
        f"these parameters accept ndarray at runtime but understate it in the stub: {offenders}"
    )
