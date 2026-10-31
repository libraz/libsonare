"""``Section.group``: the per-section material index, from both entry points."""

from __future__ import annotations

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_SR = 22050
_SPAN_SEC = 8.0


def _span(frequency: float, partials: int) -> np.ndarray:
    t = np.arange(int(_SR * _SPAN_SEC)) / _SR
    wave = sum(np.sin(2.0 * np.pi * frequency * k * t) / k for k in range(1, partials + 1))
    return 0.3 * wave / partials**0.5


@pytest.fixture(scope="module")
def abab() -> list[float]:
    """A-B-A-B: two materials of 8 s each, alternating."""
    a = _span(220.0, 1)
    b = _span(311.13, 6)
    return [float(x) for x in np.concatenate([a, b, a, b])]


def _assert_first_appearance_numbering(groups: list[int]) -> None:
    assert groups[0] == 0
    highest = 0
    for g in groups:
        assert 0 <= g <= highest + 1
        highest = max(highest, g)


def test_analyze_sections_exposes_group_as_int(abab) -> None:
    sections = libsonare.analyze_sections(abab, sample_rate=_SR, min_section_sec=4.0).sections

    assert sections
    assert all(type(s.group) is int for s in sections)
    _assert_first_appearance_numbering([s.group for s in sections])


def test_analyze_exposes_group_as_int(abab) -> None:
    sections = libsonare.analyze(abab, sample_rate=_SR).sections

    assert sections
    assert all(type(s.group) is int for s in sections)
    _assert_first_appearance_numbering([s.group for s in sections])


def test_alternating_material_yields_distinct_groups(abab) -> None:
    sections = libsonare.analyze_sections(abab, sample_rate=_SR, min_section_sec=4.0).sections
    groups = [s.group for s in sections]

    assert len(set(groups)) >= 2
    # A section that sits wholly inside one span shares its group with every
    # section wholly inside a span of the same material.
    by_material: dict[int, set[int]] = {0: set(), 1: set()}
    for s in sections:
        first, last = int(s.start // _SPAN_SEC), int((s.end - 1e-3) // _SPAN_SEC)
        if first == last:
            by_material[first % 2].add(s.group)
    assert not by_material[0] & by_material[1]
