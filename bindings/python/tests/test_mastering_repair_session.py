"""Tests for the one repair analysis and the one repair application.

``mastering_repair_analyze`` and ``mastering_repair_apply`` take any number of
channels. The application runs its stages in the chain's fixed order whatever
order the list gives, reports per channel for the channel-scoped stages and
once for the linked ones, and must agree with the per-stage entries it is
built from.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
LENGTH = SR // 2


def _material(freq: float, seed: int) -> NDArray[np.float32]:
    rng = np.random.default_rng(seed)
    t = np.arange(LENGTH) / SR
    tone = 0.2 * np.sin(2.0 * np.pi * freq * t)
    return (tone + rng.standard_normal(LENGTH) * 0.003).astype(np.float32)


def _clicked(signal: NDArray[np.float32], first: int, stride: int) -> NDArray[np.float32]:
    out = signal.copy()
    out[first::stride] = 0.95
    return out


class TestAnalyze:
    def test_measures_every_channel_and_recommends_declick(self) -> None:
        channels = [_material(440.0, 1), _clicked(_material(550.0, 2), 2000, 3500)]
        analysis = libsonare.mastering_repair_analyze(channels, SR)

        assert len(analysis["channels"]) == 2
        assert analysis["channels"][0]["clickCount"] == 0
        assert analysis["channels"][1]["clickCount"] > 0
        assert isinstance(analysis["declipThresholdSafe"], bool)
        assert analysis["recommended"][0]["stage"] == "declick"
        assert all(stage["stage"] != "dereverb" for stage in analysis["recommended"])

        applied = libsonare.mastering_repair_apply(channels, SR, stages=analysis["recommended"])
        assert [entry.stage for entry in applied.reports] == [
            stage["stage"] for stage in analysis["recommended"]
        ]

    def test_refuses_an_empty_channel_set(self) -> None:
        with pytest.raises(libsonare.SonareValueError):
            libsonare.mastering_repair_analyze([], SR)


class TestApply:
    def test_runs_the_fixed_order_whatever_the_list_order(self) -> None:
        channels = [_clicked(_material(440.0, 3), 1500, 4000), _material(660.0, 4)]
        forward = libsonare.mastering_repair_apply(
            channels, SR, stages=[{"stage": "decrackle"}, {"stage": "declick"}]
        )
        reversed_ = libsonare.mastering_repair_apply(
            channels, SR, stages=[{"stage": "declick"}, {"stage": "decrackle"}]
        )
        assert [entry.stage for entry in forward.reports] == ["declick", "decrackle"]
        for a, b in zip(forward.channels, reversed_.channels, strict=True):
            np.testing.assert_array_equal(a, b)
        assert forward.reports == reversed_.reports

    def test_matches_the_per_stage_entries(self) -> None:
        left = _clicked(_material(440.0, 5), 1500, 4000)
        right = _material(660.0, 6)
        applied = libsonare.mastering_repair_apply(
            [left, right], SR, stages=[{"stage": "declick", "lpcOrder": 16}]
        )
        pair = libsonare.mastering_repair_declick_stereo(left, right, SR, lpc_order=16)
        np.testing.assert_array_equal(applied.channels[0], np.asarray(pair.left, dtype=np.float32))
        np.testing.assert_array_equal(applied.channels[1], np.asarray(pair.right, dtype=np.float32))
        assert applied.reports[0].reports[0] == pair.left_report

        three = [left, right, left]
        denoised = libsonare.mastering_repair_apply(
            three, SR, stages=[{"stage": "denoise", "mode": "mmseStsa", "reductionDb": 18}]
        )
        linked = libsonare.mastering_repair_denoise_classical_linked(
            three, SR, mode="mmseStsa", reduction_db=18
        )
        for a, b in zip(denoised.channels, linked.channels, strict=True):
            np.testing.assert_array_equal(a, b)
        assert denoised.reports[0].scope == "linked"
        report = denoised.reports[0].reports[0]
        assert isinstance(report, libsonare.DenoiseReport)
        assert report.mean_reduction_db == pytest.approx(linked.report.mean_reduction_db)
        assert report.detected.band_floor_dbfs == pytest.approx(
            linked.report.detected.band_floor_dbfs
        )

    def test_reports_per_channel_and_once_for_linked_stages(self) -> None:
        channels = [_material(440.0, 7), _material(550.0, 8), _material(660.0, 9)]
        result = libsonare.mastering_repair_apply(
            channels, SR, stages=[{"stage": "dereverb"}, {"stage": "dehum", "adaptive": True}]
        )
        assert [(e.stage, e.scope, len(e.reports)) for e in result.reports] == [
            ("dehum", "channel", 3),
            ("dereverb", "linked", 1),
        ]
        assert isinstance(result.reports[0].reports[0], libsonare.DehumReport)

    @pytest.mark.parametrize(
        "stages",
        [
            [{"stage": "declick"}, {"stage": "declick"}],
            [{"stage": "trimSilence"}],
            [{"stage": "declick", "lpcOdrer": 8}],
            [{"stage": "denoise", "mode": "bogus"}],
        ],
    )
    def test_refuses_a_bad_stage_list(self, stages: list[dict[str, object]]) -> None:
        with pytest.raises(libsonare.SonareError) as excinfo:
            libsonare.mastering_repair_apply([_material(440.0, 10)], SR, stages=stages)
        assert excinfo.value.code == libsonare.ErrorCode.INVALID_PARAMETER

    def test_refuses_stages_that_are_not_mappings(self) -> None:
        with pytest.raises(libsonare.SonareValueError):
            libsonare.mastering_repair_apply(
                [_material(440.0, 11)],
                SR,
                stages=["declick"],  # type: ignore[list-item]
            )

    def test_reports_progress_and_stops_when_cancelled(self) -> None:
        channels = [_material(440.0, 12), _material(550.0, 13)]
        seen: list[tuple[float, str]] = []
        libsonare.mastering_repair_apply(
            channels,
            SR,
            stages=[{"stage": "decrackle"}, {"stage": "declick"}],
            on_progress=lambda progress, stage: seen.append((progress, stage)),
        )
        assert seen == [(0.5, "repair.declick"), (1.0, "repair.decrackle")]
        with pytest.raises(libsonare.SonareError) as excinfo:
            libsonare.mastering_repair_apply(
                channels, SR, stages=[{"stage": "declick"}], cancel=lambda: True
            )
        assert excinfo.value.code == libsonare.ErrorCode.CANCELLED
