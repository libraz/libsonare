"""Every tempo entry point searches 60-200 BPM unless told otherwise, so the
same recording gets the same tempo range whichever function reads it.
"""

from __future__ import annotations

import inspect
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _clicks(bpm: float, seconds: float = 8.0) -> np.ndarray:
    out = np.zeros(int(SR * seconds), dtype=np.float32)
    step = int(round(SR * 60.0 / bpm))
    for start in range(0, out.size - 64, step):
        out[start : start + 64] = np.hanning(64).astype(np.float32)
    return out


ANALYZERS: dict[str, Callable[..., Any]] = {
    "analyze_bpm": lambda x, **kw: libsonare.analyze_bpm(x, SR, **kw),
    "Audio.analyze_bpm": lambda x, **kw: libsonare.Audio.from_buffer(x, SR).analyze_bpm(**kw),
}


def test_bpm_only_entries_no_longer_accept_start_bpm() -> None:
    assert "start_bpm" not in inspect.signature(libsonare.analyze_bpm).parameters
    assert "start_bpm" not in inspect.signature(libsonare.Audio.analyze_bpm).parameters


@pytest.mark.parametrize("name", sorted(ANALYZERS))
def test_the_default_range_is_60_to_200(name: str) -> None:
    signal = _clicks(120.0)
    implicit = ANALYZERS[name](signal)
    explicit = ANALYZERS[name](signal, bpm_min=60.0, bpm_max=200.0)
    assert implicit.bpm == explicit.bpm
    assert [c.bpm for c in implicit.candidates] == [c.bpm for c in explicit.candidates]


@pytest.mark.parametrize("name", sorted(ANALYZERS))
def test_no_default_candidate_falls_outside_60_to_200(name: str) -> None:
    # A slow pulse a wider search would report as itself.
    signal = _clicks(45.0)
    result = ANALYZERS[name](signal)
    assert 60.0 <= result.bpm <= 200.0
    assert all(60.0 <= c.bpm <= 200.0 for c in result.candidates)
    wide = ANALYZERS[name](signal, bpm_min=30.0, bpm_max=300.0)
    assert [c.bpm for c in wide.candidates] != [c.bpm for c in result.candidates]
