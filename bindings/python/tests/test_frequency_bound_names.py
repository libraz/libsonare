"""Frequency bounds outside the librosa mirrors carry their unit in the name.

Pitch searches take ``f0_min_hz`` / ``f0_max_hz`` and spectral bands take
``fmin_hz`` / ``fmax_hz``. Each case shows the new name reaches the core (a
narrower search changes the answer) and that the old spelling is gone.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _tone() -> np.ndarray:
    return sine(440.0, 0.5, amp=0.5)


def _pitch_yin(**kw: Any) -> np.ndarray:
    return np.asarray(libsonare.pitch_yin(_tone(), SR, **kw).f0)


def _pitch_pyin(**kw: Any) -> np.ndarray:
    return np.asarray(libsonare.pitch_pyin(_tone(), SR, **kw).f0)


def _audio_pitch_yin(**kw: Any) -> np.ndarray:
    return np.asarray(libsonare.Audio.from_buffer(_tone(), SR).pitch_yin(**kw).f0)


def _melody(**kw: Any) -> np.ndarray:
    result = libsonare.analyze_melody(_tone(), SR, **kw)
    return np.asarray([point.frequency for point in result.points])


PITCH_SEARCHES: dict[str, Callable[..., np.ndarray]] = {
    "pitch_yin": _pitch_yin,
    "pitch_pyin": _pitch_pyin,
    "Audio.pitch_yin": _audio_pitch_yin,
    "analyze_melody": _melody,
}


@pytest.mark.parametrize("name", sorted(PITCH_SEARCHES))
def test_a_pitch_search_range_excluding_the_tone_changes_the_track(name: str) -> None:
    search = PITCH_SEARCHES[name]
    found = search()
    above = search(f0_min_hz=600.0, f0_max_hz=1200.0)
    assert not np.array_equal(found, above)


@pytest.mark.parametrize("name", sorted(PITCH_SEARCHES))
@pytest.mark.parametrize("old", ["fmin", "fmax"])
def test_a_pitch_search_refuses_the_unitless_name(name: str, old: str) -> None:
    with pytest.raises(TypeError):
        PITCH_SEARCHES[name](**{old: 100.0})


def test_mel_spectrogram_band_edges_are_named_in_hz() -> None:
    mel = libsonare.mel_spectrogram(_tone(), SR, n_mels=32, fmin_hz=200.0, fmax_hz=4000.0)
    assert (mel.fmin_hz, mel.fmax_hz) == (200.0, 4000.0)
    with pytest.raises(TypeError):
        libsonare.mel_spectrogram(_tone(), SR, fmin=200.0)  # type: ignore[call-arg]


def test_mfcc_reports_its_band_edges_in_hz() -> None:
    result = libsonare.mfcc(_tone(), SR, n_mels=32, n_mfcc=13, fmin=200.0, fmax=4000.0)
    assert (result.fmin_hz, result.fmax_hz) == (200.0, 4000.0)


@pytest.mark.parametrize("fn", [libsonare.cqt_to_audio, libsonare.vqt_to_audio])
def test_inverse_constant_q_takes_fmin_hz(fn: Callable[..., np.ndarray]) -> None:
    magnitude = np.abs(
        libsonare.cqt(_tone(), sr=SR, hop_length=128, fmin=130.8128, n_bins=12).magnitude
    )
    audio = fn(magnitude, SR, hop_length=128, fmin_hz=130.8128, n_iter=2)
    assert audio.dtype == np.float32 and audio.size > 0
    with pytest.raises(TypeError):
        fn(magnitude, SR, hop_length=128, fmin=130.8128, n_iter=2)
