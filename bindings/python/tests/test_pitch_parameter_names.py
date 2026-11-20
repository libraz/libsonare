"""Pitch and speed quantities carry one name each, and a call that would do
nothing without them requires them.

``pitch_semitones`` is the shift, ``speed_ratio`` the playback speed,
``target_mode`` the pitch-correction target and ``scale_reference_midi`` the
anchor of a scale grid. The fixed-MIDI contour corrector is
``pitch_correct_timevarying`` with its default ``target_mode``.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from typing import Any, get_args

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError
from libsonare.types import PitchTargetMode

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
HOP = 512


def _tone() -> np.ndarray:
    return sine(220.0, 0.5, amp=0.5)


def _contour(samples: np.ndarray, hz: float = 220.0) -> list[float]:
    return [hz] * (len(samples) // HOP + 1)


SHIFTS: dict[str, tuple[Callable[..., Any], str, str, float]] = {
    "pitch_shift": (
        lambda **kw: libsonare.pitch_shift(_tone(), SR, **kw),
        "pitch_semitones",
        "semitones",
        3.0,
    ),
    "Audio.pitch_shift": (
        lambda **kw: libsonare.Audio.from_buffer(_tone(), SR).pitch_shift(**kw),
        "pitch_semitones",
        "semitones",
        3.0,
    ),
    "time_stretch": (
        lambda **kw: libsonare.time_stretch(_tone(), SR, **kw),
        "speed_ratio",
        "rate",
        1.5,
    ),
    "Audio.time_stretch": (
        lambda **kw: libsonare.Audio.from_buffer(_tone(), SR).time_stretch(**kw),
        "speed_ratio",
        "rate",
        1.5,
    ),
    "phase_vocoder": (
        lambda **kw: libsonare.phase_vocoder(_tone(), SR, **kw),
        "speed_ratio",
        "rate",
        1.5,
    ),
}


@pytest.mark.parametrize("name", sorted(SHIFTS))
def test_the_defining_quantity_is_required(name: str) -> None:
    call, _, _, _ = SHIFTS[name]
    with pytest.raises(TypeError):
        call()


@pytest.mark.parametrize("name", sorted(SHIFTS))
def test_the_named_quantity_reaches_the_core(name: str) -> None:
    call, new, _, value = SHIFTS[name]
    moved = np.asarray(call(**{new: value}), dtype=np.float32)
    neutral = np.asarray(call(**{new: 0.0 if new == "pitch_semitones" else 1.0}), np.float32)
    assert moved.shape != neutral.shape or not np.allclose(moved, neutral)


@pytest.mark.parametrize("name", sorted(SHIFTS))
def test_the_old_spelling_is_refused(name: str) -> None:
    call, _, old, value = SHIFTS[name]
    with pytest.raises(TypeError):
        call(**{old: value})


def test_the_fixed_midi_contour_corrector_is_the_default_target_mode() -> None:
    assert not hasattr(libsonare, "pitch_correct_to_midi_timevarying")
    assert get_args(PitchTargetMode) == ("midi", "scale")
    samples = _tone()
    f0 = _contour(samples)
    implicit = libsonare.pitch_correct_timevarying(
        samples, SR, f0, HOP, target_midi=60.0, voiced=[1] * len(f0)
    )
    explicit = libsonare.pitch_correct_timevarying(
        samples, SR, f0, HOP, target_mode="midi", target_midi=60.0, voiced=[1] * len(f0)
    )
    np.testing.assert_array_equal(implicit, explicit)
    scaled = libsonare.pitch_correct_timevarying(
        samples, SR, f0, HOP, target_mode="scale", voiced=[1] * len(f0)
    )
    assert not np.array_equal(scaled, implicit)


@pytest.mark.parametrize("value", ["Scale", "fixed", ""])
def test_an_unknown_target_mode_names_the_tokens(value: str) -> None:
    samples = _tone()
    with pytest.raises(
        SonareValueError,
        match="^pitch_correct_timevarying: target_mode must be one of midi, scale$",
    ):
        libsonare.pitch_correct_timevarying(samples, SR, _contour(samples), HOP, target_mode=value)  # type: ignore[arg-type]


@pytest.mark.parametrize("value", [0, 1, None])
def test_a_non_string_target_mode_is_a_type_error(value: object) -> None:
    samples = _tone()
    with pytest.raises(TypeError):
        libsonare.pitch_correct_timevarying(samples, SR, _contour(samples), HOP, target_mode=value)  # type: ignore[arg-type]


def test_the_removed_mode_keyword_is_refused() -> None:
    samples = _tone()
    with pytest.raises(TypeError):
        libsonare.pitch_correct_timevarying(samples, SR, _contour(samples), HOP, mode="scale")  # type: ignore[call-arg]


def test_scale_reference_midi_moves_the_scale_grid() -> None:
    # A fractional anchor shifts the grid; an integer one is the default grid.
    samples = _tone()
    f0 = _contour(samples, 225.0)
    kw: dict[str, Any] = {"target_mode": "scale", "voiced": [1] * len(f0)}
    default = libsonare.pitch_correct_timevarying(samples, SR, f0, HOP, **kw)
    detuned = libsonare.pitch_correct_timevarying(
        samples, SR, f0, HOP, scale_reference_midi=69.5, **kw
    )
    assert not np.array_equal(default, detuned)
    with pytest.raises(TypeError):
        libsonare.pitch_correct_timevarying(samples, SR, f0, HOP, reference_midi=69.5, **kw)  # type: ignore[call-arg]


def test_auto_tune_takes_scale_reference_midi() -> None:
    samples = sine(225.0, 0.5, amp=0.5)
    key = (libsonare.PitchClass.C, "major")
    default = libsonare.auto_tune(samples, SR, key=key)
    detuned = libsonare.auto_tune(samples, SR, key=key, scale_reference_midi=69.5)
    assert not np.array_equal(default.samples, detuned.samples)
    with pytest.raises(TypeError):
        libsonare.auto_tune(samples, SR, key=key, reference_midi=69.5)  # type: ignore[call-arg]


@pytest.mark.parametrize(
    "fn", [libsonare.scale_quantize_midi, libsonare.scale_correction_semitones]
)
def test_the_scale_helpers_take_scale_reference_midi(fn: Callable[..., float]) -> None:
    mask = libsonare.scale_mask_for_mode(0, "major")
    absent = fn(0, mask, 60.4)
    assert fn(0, mask, 60.4, scale_reference_midi=None) == absent
    assert fn(0, mask, 60.4, scale_reference_midi=69.0) == absent
    assert fn(0, mask, 60.4, scale_reference_midi=69.5) != absent
    assert fn(0, mask, 60.4, scale_reference_midi=0.0) == absent
    with pytest.raises(SonareValueError, match="scale_reference_midi"):
        fn(0, mask, 60.4, scale_reference_midi=-1.0)
    with pytest.raises(TypeError):
        fn(0, mask, 60.4, reference_midi=69.5)
    assert math.isfinite(absent)
