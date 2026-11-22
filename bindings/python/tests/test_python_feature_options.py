"""Option-routing tests for the extended C feature APIs."""

from __future__ import annotations

import re
from typing import Any, cast

import pytest

from libsonare import _analysis_reports, _conversions


class _FakeNnlsLibrary:
    """Small ctypes-compatible fake recording the NNLS chroma call."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []

    def sonare_nnls_chroma(self, *args: object) -> int:
        self.calls.append(("nnls", args))
        return 0


class _FakeImpulseLibrary:
    """Small fake for the impulse-response entry point."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[object, ...]]] = []

    def sonare_analyze_impulse_response(self, *args: object) -> int:
        self.calls.append(("impulse", args))
        return 0

    def sonare_free_acoustic_result(self, _result: object) -> None:
        pass


def test_nnls_chroma_routes_default_and_custom_hop_through_options(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The options struct carries both the default hop and an explicit one."""
    lib = _FakeNnlsLibrary()
    monkeypatch.setattr(_conversions, "_get_lib", lambda: lib)

    _conversions.nnls_chroma([0.0], hop_length=512, sample_rate=22050)
    _conversions.nnls_chroma([0.0], hop_length=256, sample_rate=22050)

    assert [name for name, _args in lib.calls] == ["nnls", "nnls"]
    assert [cast(Any, args[3])._obj.hop_length for _name, args in lib.calls] == [512, 256]


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        # Every refusal states the member's whole domain, whichever property the value breaks.
        ({"hop_length": 0}, "hop_length must be an integer in [1, 2147483647]"),
        ({"hop_length": 1.5}, "hop_length must be an integer in [1, 2147483647]"),
        ({"stft_blend_weight": float("nan")}, "stft_blend_weight must be finite"),
        ({"stft_blend_n_fft": 3}, "stft_blend_n_fft must be an even integer in [2, 524288]"),
        ({"stft_blend_n_fft": 0}, "stft_blend_n_fft must be an even integer in [2, 524288]"),
        (
            {"stft_blend_n_fft": 2**31},
            "stft_blend_n_fft must be an even integer in [2, 524288]",
        ),
    ],
)
def test_nnls_chroma_validates_parameters(kwargs: dict[str, Any], message: str) -> None:
    """Python rejects values that the C contract rejects before FFI."""
    with pytest.raises(ValueError, match=re.escape(message)):
        _conversions.nnls_chroma([0.0], sample_rate=22050, **kwargs)


def test_analyze_impulse_response_routes_default_and_custom_decay_through_options(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The options struct carries the default decay range and an explicit one."""
    lib = _FakeImpulseLibrary()
    monkeypatch.setattr(_analysis_reports, "_get_lib", lambda: lib)

    _analysis_reports.analyze_impulse_response([0.0], min_decay_db=30.0, sample_rate=48000)
    _analysis_reports.analyze_impulse_response([0.0], min_decay_db=45.0, sample_rate=48000)

    assert [name for name, _args in lib.calls] == ["impulse", "impulse"]
    assert [cast(Any, args[4])._obj.min_decay_db for _name, args in lib.calls] == [30.0, 45.0]


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        ({"min_decay_db": 0.0}, "min_decay_db must be finite and greater than 0"),
        ({"min_decay_db": float("inf")}, "min_decay_db must be finite and greater than 0"),
        ({"n_octave_bands": -1}, r"n_octave_bands must be an integer in \[0, "),
    ],
)
def test_analyze_impulse_response_validates_parameters(
    kwargs: dict[str, Any], message: str
) -> None:
    """Python rejects values that the C contract rejects."""
    with pytest.raises(ValueError, match=message):
        _analysis_reports.analyze_impulse_response([0.0], sample_rate=48000, **kwargs)
