"""The mono repair entry points return the processed buffer beside the report
of what the pass did, the same pairing the stereo entry points already use.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _signal() -> NDArray[np.float32]:
    return sine(440.0, 0.3, amp=0.5)


CASES: dict[str, tuple[Callable[..., Any], type, type]] = {
    "declick": (
        libsonare.mastering_repair_declick,
        libsonare.DeclickResult,
        libsonare.DeclickReport,
    ),
    "declip": (
        libsonare.mastering_repair_declip,
        libsonare.DeclipResult,
        libsonare.DeclipReport,
    ),
    "decrackle": (
        libsonare.mastering_repair_decrackle,
        libsonare.DecrackleResult,
        libsonare.DecrackleReport,
    ),
    "dehum": (
        libsonare.mastering_repair_dehum,
        libsonare.DehumResult,
        libsonare.DehumReport,
    ),
    "denoise_classical": (
        libsonare.mastering_repair_denoise_classical,
        libsonare.DenoiseResult,
        libsonare.DenoiseReport,
    ),
    "dereverb_classical": (
        libsonare.mastering_repair_dereverb_classical,
        libsonare.DereverbResult,
        libsonare.DereverbReport,
    ),
    "trim_silence": (
        libsonare.mastering_repair_trim_silence,
        libsonare.TrimSilenceResult,
        libsonare.TrimReport,
    ),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_a_mono_repair_returns_float32_samples_beside_its_report(name: str) -> None:
    fn, result_type, report_type = CASES[name]
    result = fn(_signal(), SR)

    assert type(result) is result_type
    assert isinstance(result.samples, np.ndarray)
    assert result.samples.dtype == np.float32
    assert result.samples.ndim == 1
    assert isinstance(result.report, report_type)
    with pytest.raises(AttributeError):
        result.samples = result.samples  # type: ignore[misc]


def test_the_declick_report_counts_the_clicks_it_repaired() -> None:
    clean = _signal()
    clicked = clean.copy()
    for i in range(5):
        clicked[1000 + i * 1200] = 1.0

    assert libsonare.mastering_repair_declick(clean, SR).report.repaired_runs == 0
    repaired = libsonare.mastering_repair_declick(clicked, SR)
    assert repaired.report.repaired_runs > 0
    assert repaired.report.linked_runs == 0
    assert repaired.samples.shape == clicked.shape


def test_the_declip_report_counts_the_samples_it_rebuilt() -> None:
    clipped = np.clip(sine(440.0, 0.3, amp=1.0) * 2.0, -0.9, 0.9).astype(np.float32)

    result = libsonare.mastering_repair_declip(
        clipped, SR, clip_threshold_db=20.0 * math.log10(0.85)
    )
    assert result.report.repaired_samples > 0
    clean = libsonare.mastering_repair_declip(
        _signal(), SR, clip_threshold_db=20.0 * math.log10(0.85)
    )
    assert clean.report.repaired_samples == 0


def test_a_trim_reports_what_it_removed_from_each_end() -> None:
    pad = np.zeros(1200, dtype=np.float32)
    samples = np.concatenate([pad, _signal(), pad]).astype(np.float32)

    result = libsonare.mastering_repair_trim_silence(samples, SR)
    report = result.report

    assert report.removed_head_samples > 0
    assert report.removed_tail_samples > 0
    assert (
        report.removed_head_samples + len(result.samples) + report.removed_tail_samples
        == samples.shape[0]
    )
    assert np.array_equal(result.samples, samples[report.range.first : report.range.last_exclusive])


def test_a_trim_that_keeps_nothing_returns_an_empty_float32_buffer() -> None:
    silence = np.zeros(4410, dtype=np.float32)

    result = libsonare.mastering_repair_trim_silence(silence, SR)

    assert isinstance(result.samples, np.ndarray)
    assert result.samples.dtype == np.float32
    assert result.samples.shape == (0,)
    assert (result.report.range.first, result.report.range.last_exclusive) == (4410, 4410)
    assert result.report.removed_head_samples == 4410
    assert result.report.removed_tail_samples == 0
