"""The repair CLI's defect report: the profile's detector groups, snake_cased."""

from __future__ import annotations

import pytest

from libsonare._cli_repair import _print_repair_detection_report, _repair_detection_report


def _wire_defects() -> dict[str, dict[str, object]]:
    return {
        "click": {"count": 0, "rejected": 3, "longestRunSamples": 0, "perSecond": 0.0},
        "clip": {
            "clippedSamples": 12,
            "clippedRatio": 0.001,
            "runCount": 2,
            "longestRunSamples": 7,
            "flatRunCount": 0,
            "longestFlatRunSamples": 0,
            "flatSampleCount": 0,
            "flatLevel": 0.0,
        },
        "crackle": {"sampleCount": 0, "sampleFraction": 0.0, "perSecond": 0.0},
        "noise": {"floorDbfs": -72.0, "bandFloorDbfs": [-80.0, -61.5, -90.0]},
        "hum": {
            "fundamentalHz": 60.0,
            "fundamentalProminence": 4.0,
            "harmonics": 2,
            "harmonicDbfs": [-50.0, -44.0, -120.0],
        },
        "reverb": {"lateDecayRatioDb": -18.0},
    }


def test_report_keeps_each_detector_group_in_snake_case() -> None:
    report = _repair_detection_report(_wire_defects())
    assert report is not None
    assert list(report) == ["click", "clip", "crackle", "noise", "hum", "reverb"]
    assert report["click"] == {
        "count": 0,
        "rejected": 3,
        "longest_run_samples": 0,
        "per_second": 0.0,
    }
    assert report["clip"]["clipped_samples"] == 12
    assert report["clip"]["clipped_ratio"] == 0.001
    # No flat run was found, so no flat-top level was measured.
    assert "flat_level" not in report["clip"]
    assert report["noise"]["band_floor_dbfs"] == [-80.0, -61.5, -90.0]
    assert report["hum"]["harmonic_dbfs"] == [-50.0, -44.0, -120.0]
    assert report["reverb"] == {"late_decay_ratio_db": -18.0}


def test_an_unmeasured_profile_has_no_report(capsys: pytest.CaptureFixture[str]) -> None:
    assert _repair_detection_report(None) is None
    _print_repair_detection_report(None)
    assert "Measured:     no" in capsys.readouterr().out


def test_text_report_derives_the_loudest_band_and_harmonic(
    capsys: pytest.CaptureFixture[str],
) -> None:
    _print_repair_detection_report(_repair_detection_report(_wire_defects()))
    out = capsys.readouterr().out
    assert "loudest band -61.5 dBFS (band 1)" in out
    assert "fundamental -50.0 dBFS, loudest harmonic -44.0 dBFS" in out


def test_an_incomplete_group_is_refused() -> None:
    defects = _wire_defects()
    del defects["hum"]["harmonicDbfs"]
    with pytest.raises(ValueError, match="'hum'"):
        _repair_detection_report(defects)
