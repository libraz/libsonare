"""Metering options left out take the library default; an explicit 0 is refused.

True peak, spectrum and dynamic range read ``None`` as "use the default" and
never read a 0 as that, matching the other surfaces. Dynamic-range percentiles
are ranks in [0, 1] with the low one at most the high one.
"""

from __future__ import annotations

import math
import re
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000


def _signal() -> np.ndarray:
    t = np.arange(SR * 4) / SR
    envelope = np.where((t % 1.0) < 0.5, 1.0, 0.1)
    return (0.5 * envelope * np.sin(2 * math.pi * 440.0 * t)).astype(np.float32)


def _true_peak(**kw: Any) -> object:
    return libsonare.metering_true_peak_dbtp(_signal(), SR, **kw)


def _spectrum(**kw: Any) -> object:
    return libsonare.metering_spectrum(_signal(), SR, **kw)


def _spectrum_frame(**kw: Any) -> object:
    return libsonare.metering_spectrum_frame(_signal(), SR, **kw)


def _dynamic_range(**kw: Any) -> object:
    return libsonare.metering_dynamic_range(_signal(), SR, **kw)


def _audio() -> libsonare.Audio:
    return libsonare.Audio.from_buffer(_signal(), SR)


def _audio_true_peak(**kw: Any) -> object:
    return _audio().true_peak_dbtp(**kw)


def _audio_spectrum(**kw: Any) -> object:
    return _audio().spectrum(**kw)


def _audio_spectrum_frame(**kw: Any) -> object:
    return _audio().spectrum_frame(**kw)


def _audio_dynamic_range(**kw: Any) -> object:
    return _audio().dynamic_range(**kw)


CASES: list[tuple[str, Callable[..., object], str, object]] = [
    ("true_peak", _true_peak, "oversample_factor", 4),
    ("spectrum", _spectrum, "n_fft", 2048),
    ("spectrum", _spectrum, "octave_fraction", 3),
    ("spectrum", _spectrum, "db_ref", 1.0),
    ("spectrum_frame", _spectrum_frame, "n_fft", 2048),
    ("spectrum_frame", _spectrum_frame, "db_amin", 1e-10),
    ("dynamic_range", _dynamic_range, "window_sec", 3.0),
    ("dynamic_range", _dynamic_range, "hop_sec", 1.0),
    ("Audio.true_peak_db", _audio_true_peak, "oversample_factor", 4),
    ("Audio.spectrum", _audio_spectrum, "db_amin", 1e-10),
    ("Audio.spectrum_frame", _audio_spectrum_frame, "octave_fraction", 3),
    ("Audio.dynamic_range", _audio_dynamic_range, "window_sec", 3.0),
]


def _same(a: object, b: object) -> bool:
    if isinstance(a, float):
        return a == b
    fields = getattr(a, "__dataclass_fields__", {})
    return all(np.array_equal(getattr(a, f), getattr(b, f)) for f in fields)


@pytest.mark.parametrize(("label", "call", "name", "default"), CASES)
def test_an_omitted_option_is_the_library_default(
    label: str, call: Callable[..., object], name: str, default: object
) -> None:
    assert _same(call(), call(**{name: default}))
    assert _same(call(), call(**{name: None}))


@pytest.mark.parametrize(("label", "call", "name", "default"), CASES)
def test_an_explicit_zero_is_refused(
    label: str, call: Callable[..., object], name: str, default: object
) -> None:
    with pytest.raises(SonareValueError, match=name):
        call(**{name: 0})


@pytest.mark.parametrize("name", ["low_percentile", "high_percentile"])
@pytest.mark.parametrize("value", [-0.1, 1.5, 95.0])
@pytest.mark.parametrize("call", [_dynamic_range, _audio_dynamic_range])
def test_a_percentile_outside_the_unit_interval_is_refused(
    call: Callable[..., object], name: str, value: float
) -> None:
    with pytest.raises(SonareValueError, match=name):
        call(**{name: value})


@pytest.mark.parametrize("call", [_dynamic_range, _audio_dynamic_range])
def test_a_low_percentile_above_the_high_one_is_refused(call: Callable[..., object]) -> None:
    with pytest.raises(SonareValueError, match="low_percentile"):
        call(low_percentile=0.9, high_percentile=0.2)


def test_the_unit_interval_ends_are_real_requests() -> None:
    # A rising ramp, so every window sits at a different level.
    t = np.arange(SR * 4) / SR
    ramp = (np.linspace(0.01, 1.0, t.size) * np.sin(2 * math.pi * 440.0 * t)).astype(np.float32)
    windows = {"window_sec": 0.25, "hop_sec": 0.125}
    default = libsonare.metering_dynamic_range(ramp, SR, **windows)
    full = libsonare.metering_dynamic_range(
        ramp, SR, low_percentile=0.0, high_percentile=1.0, **windows
    )
    assert full.dynamic_range_db > default.dynamic_range_db


@pytest.mark.parametrize(
    ("entry", "call"),
    [
        ("metering_spectrum", _spectrum),
        ("metering_spectrum_frame", _spectrum_frame),
        ("Audio.spectrum", _audio_spectrum),
        ("Audio.spectrum_frame", _audio_spectrum_frame),
    ],
)
def test_a_power_of_two_outside_the_fft_range_is_refused(
    entry: str, call: Callable[..., object]
) -> None:
    text = rf"^{re.escape(entry)}: n_fft must be a power of two in \[2, 4194304\]$"
    for n_fft in (1, 8388608):
        with pytest.raises(SonareValueError, match=text):
            call(n_fft=n_fft)
    for n_fft in (2, 4194304):
        call(n_fft=n_fft)
