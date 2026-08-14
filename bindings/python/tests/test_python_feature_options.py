"""Compatibility and option-routing tests for newly extended C feature APIs."""

from __future__ import annotations

import ctypes
from typing import Any, cast

import pytest

from libsonare import ErrorCode, SonareError, _analysis_reports, _conversions


class _FakeNnlsLibrary:
    """Small ctypes-compatible fake exposing selected generations of the ABI."""

    def __init__(self, *, has_ex2: bool = False, has_ex: bool = False) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []
        if has_ex2:
            self.sonare_nnls_chroma_ex2 = self._ex2
        if has_ex:
            self.sonare_nnls_chroma_ex = self._ex

    def _ex2(self, *args: object) -> int:
        self.calls.append(("ex2", args))
        return 0

    def _ex(self, *args: object) -> int:
        self.calls.append(("ex", args))
        return 0

    def sonare_nnls_chroma(self, *args: object) -> int:
        self.calls.append(("legacy", args))
        return 0


class _FakeImpulseLibrary:
    """Small fake for the legacy and extended impulse-response entry points."""

    def __init__(self, *, has_ex: bool = False) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []
        if has_ex:
            self.sonare_analyze_impulse_response_ex = self._ex

    def _ex(self, *args: object) -> int:
        self.calls.append(("ex", args))
        return 0

    def sonare_analyze_impulse_response(self, *args: object) -> int:
        self.calls.append(("legacy", args))
        return 0

    def sonare_free_acoustic_result(self, _result: object) -> None:
        pass


def test_nnls_chroma_routes_default_and_custom_hop_to_ex2(monkeypatch: pytest.MonkeyPatch) -> None:
    """The new symbol receives both the legacy default and an explicit hop."""
    lib = _FakeNnlsLibrary(has_ex2=True)
    monkeypatch.setattr(_conversions, "_get_lib", lambda: lib)

    _conversions.nnls_chroma([0.0], hop_length=512)
    _conversions.nnls_chroma([0.0], hop_length=256)

    assert [name for name, _args in lib.calls] == ["ex2", "ex2"]
    assert [cast(ctypes.c_int, args[6]).value for _name, args in lib.calls] == [512, 256]


def test_nnls_chroma_uses_legacy_ex_when_ex2_is_missing(monkeypatch: pytest.MonkeyPatch) -> None:
    """A library with the prior extended symbol retains the old default path."""
    lib = _FakeNnlsLibrary(has_ex=True)
    monkeypatch.setattr(_conversions, "_get_lib", lambda: lib)

    _conversions.nnls_chroma([0.0])

    assert [name for name, _args in lib.calls] == ["ex"]


def test_nnls_chroma_uses_base_symbol_when_all_extended_symbols_are_missing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The original default API remains usable on the oldest supported ABI."""
    lib = _FakeNnlsLibrary()
    monkeypatch.setattr(_conversions, "_get_lib", lambda: lib)

    _conversions.nnls_chroma([0.0])

    assert [name for name, _args in lib.calls] == ["legacy"]


def test_nnls_chroma_rejects_custom_hop_without_ex2(monkeypatch: pytest.MonkeyPatch) -> None:
    """A legacy library must not silently discard a requested hop length."""
    lib = _FakeNnlsLibrary(has_ex=True)
    monkeypatch.setattr(_conversions, "_get_lib", lambda: lib)

    with pytest.raises(SonareError, match="sonare_nnls_chroma_ex2") as error:
        _conversions.nnls_chroma([0.0], hop_length=256)
    assert error.value.code == ErrorCode.NOT_SUPPORTED
    assert not lib.calls


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        ({"hop_length": 0}, "hop_length must be a positive integer"),
        ({"hop_length": 1.5}, "hop_length must be a positive integer"),
        ({"stft_blend_weight": float("nan")}, "stft_blend_weight must be finite"),
        ({"stft_blend_n_fft": 3}, "stft_blend_n_fft must be an even integer"),
    ],
)
def test_nnls_chroma_validates_ex2_parameters(kwargs: dict[str, Any], message: str) -> None:
    """Python rejects values that the ex2 C contract rejects before FFI."""
    with pytest.raises(ValueError, match=message):
        _conversions.nnls_chroma([0.0], **kwargs)


def test_analyze_impulse_response_routes_default_and_custom_decay_to_ex(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The extended symbol receives the legacy default and explicit decay range."""
    lib = _FakeImpulseLibrary(has_ex=True)
    monkeypatch.setattr(_analysis_reports, "_get_lib", lambda: lib)

    _analysis_reports.analyze_impulse_response([0.0], min_decay_db=30.0)
    _analysis_reports.analyze_impulse_response([0.0], min_decay_db=45.0)

    assert [name for name, _args in lib.calls] == ["ex", "ex"]
    assert [cast(ctypes.c_float, args[4]).value for _name, args in lib.calls] == [30.0, 45.0]


def test_analyze_impulse_response_uses_legacy_symbol_for_default(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A library without the extension remains usable with the default range."""
    lib = _FakeImpulseLibrary()
    monkeypatch.setattr(_analysis_reports, "_get_lib", lambda: lib)

    _analysis_reports.analyze_impulse_response([0.0])

    assert [name for name, _args in lib.calls] == ["legacy"]


def test_analyze_impulse_response_rejects_custom_decay_without_ex(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A legacy library must not silently discard a requested decay range."""
    lib = _FakeImpulseLibrary()
    monkeypatch.setattr(_analysis_reports, "_get_lib", lambda: lib)

    with pytest.raises(SonareError, match="sonare_analyze_impulse_response_ex") as error:
        _analysis_reports.analyze_impulse_response([0.0], min_decay_db=45.0)
    assert error.value.code == ErrorCode.NOT_SUPPORTED
    assert not lib.calls


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        ({"min_decay_db": 0.0}, "min_decay_db must be finite and positive"),
        ({"min_decay_db": float("inf")}, "min_decay_db must be finite and positive"),
        ({"n_octave_bands": -1}, "n_octave_bands must be a non-negative integer"),
    ],
)
def test_analyze_impulse_response_validates_ex_parameters(
    kwargs: dict[str, Any], message: str
) -> None:
    """Python rejects values that the extended C contract rejects."""
    with pytest.raises(ValueError, match=message):
        _analysis_reports.analyze_impulse_response([0.0], **kwargs)
