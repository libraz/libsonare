"""Tests for the one repair analysis and the one repair application.

``mastering_repair_analyze`` and ``mastering_repair_apply`` take any number of
channels. The application runs its stages in the chain's fixed order whatever
order the list gives, reports per channel for the channel-scoped stages and
once for the linked ones, and must agree with the per-stage entries it is
built from.
"""

from __future__ import annotations

import dataclasses

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

        assert analysis.channels is not None and len(analysis.channels) == 2
        assert analysis.channels[0].click.count == 0
        assert analysis.channels[1].click.count > 0
        assert isinstance(analysis.declip_threshold_safe, bool)
        assert isinstance(analysis.recommended[0], libsonare.DeclickStage)
        assert analysis.recommended[0].lpc_order is not None
        assert all(stage.stage != "dereverb" for stage in analysis.recommended)

        applied = libsonare.mastering_repair_apply(channels, SR, stages=analysis.recommended)
        assert [entry.stage for entry in applied.reports] == [
            stage.stage for stage in analysis.recommended
        ]

    def test_refuses_an_empty_channel_set(self) -> None:
        with pytest.raises(libsonare.SonareValueError):
            libsonare.mastering_repair_analyze([], SR)


class TestApply:
    def test_runs_the_fixed_order_whatever_the_list_order(self) -> None:
        channels = [_clicked(_material(440.0, 3), 1500, 4000), _material(660.0, 4)]
        forward = libsonare.mastering_repair_apply(
            channels, SR, stages=[libsonare.DecrackleStage(), libsonare.DeclickStage()]
        )
        reversed_ = libsonare.mastering_repair_apply(
            channels, SR, stages=[libsonare.DeclickStage(), libsonare.DecrackleStage()]
        )
        assert [entry.stage for entry in forward.reports] == ["declick", "decrackle"]
        for a, b in zip(forward.channels, reversed_.channels, strict=True):
            np.testing.assert_array_equal(a, b)
        assert forward.reports == reversed_.reports

    def test_a_declip_report_with_no_flat_run_has_no_flat_level(self) -> None:
        clean = [_material(440.0, 1)]
        report = libsonare.mastering_repair_apply(clean, SR, stages=[libsonare.DeclipStage()])
        detected = report.reports[0].reports[0].detected
        assert detected.flat_run_count == 0 and detected.flat_level is None
        clipped = [np.clip(_material(440.0, 1) * 8.0, -0.5, 0.5).astype(np.float32)]
        report = libsonare.mastering_repair_apply(clipped, SR, stages=[libsonare.DeclipStage()])
        detected = report.reports[0].reports[0].detected
        assert detected.flat_run_count > 0 and detected.flat_level == pytest.approx(0.5, abs=0.01)

    def test_matches_the_per_stage_entries(self) -> None:
        left = _clicked(_material(440.0, 5), 1500, 4000)
        right = _material(660.0, 6)
        applied = libsonare.mastering_repair_apply(
            [left, right], SR, stages=[libsonare.DeclickStage(lpc_order=16)]
        )
        pair = libsonare.mastering_repair_declick_stereo(left, right, SR, lpc_order=16)
        np.testing.assert_array_equal(applied.channels[0], np.asarray(pair.left, dtype=np.float32))
        np.testing.assert_array_equal(applied.channels[1], np.asarray(pair.right, dtype=np.float32))
        assert applied.reports[0].reports[0] == pair.left_report

        three = [left, right, left]
        denoised = libsonare.mastering_repair_apply(
            three, SR, stages=[libsonare.DenoiseStage(mode="mmseStsa", reduction_db=18)]
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
            channels, SR, stages=[libsonare.DereverbStage(), libsonare.DehumStage(adaptive=True)]
        )
        assert [(e.stage, e.scope, len(e.reports)) for e in result.reports] == [
            ("dehum", "channel", 3),
            ("dereverb", "linked", 1),
        ]
        assert isinstance(result.reports[0].reports[0], libsonare.DehumReport)

    @pytest.mark.parametrize(
        "stages",
        [
            [libsonare.DeclickStage(), libsonare.DeclickStage()],
            [libsonare.DenoiseStage(mode="bogus")],
        ],
    )
    def test_refuses_a_bad_stage_list(self, stages: list[libsonare.RepairStage]) -> None:
        with pytest.raises(libsonare.SonareError) as excinfo:
            libsonare.mastering_repair_apply([_material(440.0, 10)], SR, stages=stages)
        assert not isinstance(excinfo.value, libsonare.SonareValueError)
        assert excinfo.value.code == libsonare.ErrorCode.INVALID_FORMAT

    @pytest.mark.parametrize("stage", ["declick", {"stage": "declick"}])
    def test_refuses_stages_that_are_not_stage_records(self, stage: object) -> None:
        with pytest.raises(TypeError, match="stage records"):
            libsonare.mastering_repair_apply(
                [_material(440.0, 11)],
                SR,
                stages=[stage],  # type: ignore[list-item]
            )

    def test_a_misspelt_setting_is_refused_where_the_stage_is_built(self) -> None:
        with pytest.raises(TypeError):
            libsonare.DeclickStage(lpc_odrer=8)  # type: ignore[call-arg]

    def test_a_recommended_declip_level_is_given_in_dbfs(self) -> None:
        clipped = [np.clip(_material(440.0, 1) * 8.0, -0.5, 0.5).astype(np.float32)]
        analysis = libsonare.mastering_repair_analyze(clipped, SR)
        declip = [s for s in analysis.recommended if isinstance(s, libsonare.DeclipStage)]
        assert declip and declip[0].clip_threshold_db == pytest.approx(-6.02, abs=0.1)
        by_hand = libsonare.mastering_repair_apply(
            clipped,
            SR,
            stages=[libsonare.DeclipStage(clip_threshold_db=declip[0].clip_threshold_db)],
        )
        recommended = libsonare.mastering_repair_apply(clipped, SR, stages=declip)
        np.testing.assert_array_equal(by_hand.channels[0], recommended.channels[0])

    def test_reports_progress_and_stops_when_cancelled(self) -> None:
        channels = [_material(440.0, 12), _material(550.0, 13)]
        seen: list[tuple[float, str]] = []
        libsonare.mastering_repair_apply(
            channels,
            SR,
            stages=[libsonare.DecrackleStage(), libsonare.DeclickStage()],
            on_progress=lambda progress, stage: seen.append((progress, stage)),
        )
        assert seen == [(0.5, "repair.declick"), (1.0, "repair.decrackle")]
        with pytest.raises(libsonare.SonareError) as excinfo:
            libsonare.mastering_repair_apply(
                channels, SR, stages=[libsonare.DeclickStage()], cancel=lambda: True
            )
        assert excinfo.value.code == libsonare.ErrorCode.CANCELLED


def test_a_declip_level_survives_the_round_trip_from_analysis_to_apply() -> None:
    from libsonare._effects_repair_session import _stage_from_wire, _stage_to_wire

    wire = {"stage": "declip", "clipThresholdDb": -1.9382, "lpcOrder": 36}
    stage = _stage_from_wire(wire)
    assert isinstance(stage, libsonare.DeclipStage)
    assert stage.clip_threshold_db == -1.9382
    back = _stage_to_wire(stage)
    assert back["clipThresholdDb"] == wire["clipThresholdDb"]
    assert back["lpcOrder"] == 36


def test_declip_link_channels_survives_the_round_trip() -> None:
    from libsonare._effects_repair_session import _stage_from_wire, _stage_to_wire

    wire = {"stage": "declip", "clipThresholdDb": -6.0, "linkChannels": True}
    stage = _stage_from_wire(wire)
    assert isinstance(stage, libsonare.DeclipStage)
    assert stage.link_channels is True
    assert _stage_to_wire(stage) == wire
    assert "linkChannels" not in _stage_to_wire(libsonare.DeclipStage())


def test_a_recommended_stereo_declip_applies_and_linking_is_opt_in() -> None:
    # The same tone driven harder on the left: the right channel's runs sit inside the left's.
    left = np.clip(_material(440.0, 1) * 8.0, -0.5, 0.5).astype(np.float32)
    right = np.clip(_material(440.0, 1) * 4.0, -0.5, 0.5).astype(np.float32)
    analysis = libsonare.mastering_repair_analyze([left, right], SR)
    declip = [s for s in analysis.recommended if isinstance(s, libsonare.DeclipStage)]
    assert declip
    recommended = libsonare.mastering_repair_apply([left, right], SR, stages=declip)
    own = libsonare.mastering_repair_apply(
        [left, right], SR, stages=[dataclasses.replace(declip[0], link_channels=False)]
    )
    linked = libsonare.mastering_repair_apply(
        [left, right], SR, stages=[dataclasses.replace(declip[0], link_channels=True)]
    )
    # Independent repair leaves every sample a channel did not clip as it was; linking widens
    # the right channel's runs to the left's and re-estimates those samples too.
    unclipped = np.abs(right) < 0.5
    np.testing.assert_array_equal(own.channels[1][unclipped], right[unclipped])
    assert not np.array_equal(linked.channels[1][unclipped], right[unclipped])
    expected = linked if declip[0].link_channels else own
    for channel in range(2):
        np.testing.assert_array_equal(recommended.channels[channel], expected.channels[channel])
