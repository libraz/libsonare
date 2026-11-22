"""The repair CLI's defect report: the profile's detector groups, snake_cased."""

from __future__ import annotations

import pytest

from libsonare._cli_repair import _print_repair_detection_report, _repair_detection_report
from libsonare._types_repair import (
    AudioDefects,
    ClickDetection,
    ClipDetection,
    CrackleDetection,
    HumDetection,
    NoiseDetection,
    ReverbDetection,
)


def _defects() -> AudioDefects:
    return AudioDefects(
        click=ClickDetection(count=0, rejected=3, longest_run_samples=0, per_second=0.0),
        clip=ClipDetection(
            clipped_samples=12,
            clipped_ratio=0.001,
            run_count=2,
            longest_run_samples=7,
            flat_run_count=0,
            longest_flat_run_samples=0,
            flat_sample_count=0,
            flat_level=None,
        ),
        crackle=CrackleDetection(sample_count=0, sample_fraction=0.0, per_second=0.0),
        noise=NoiseDetection(
            floor_dbfs=-72.0, band_floor_dbfs=[-80.0, -61.5, -90.0], band_floor_band=[0, 2, 3]
        ),
        hum=HumDetection(
            fundamental_hz=60.0,
            fundamental_prominence=4.0,
            harmonics=2,
            harmonic_dbfs=[-50.0, -44.0, -120.0],
        ),
        reverb=ReverbDetection(late_decay_ratio_db=-18.0, late_predictability=None),
    )


def test_report_keeps_each_detector_group_in_snake_case() -> None:
    report = _repair_detection_report(_defects())
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
    assert report["noise"]["band_floor_band"] == [0, 2, 3]
    assert report["hum"]["harmonic_dbfs"] == [-50.0, -44.0, -120.0]
    assert report["reverb"] == {"late_decay_ratio_db": -18.0}


def test_an_unmeasured_profile_has_no_report(capsys: pytest.CaptureFixture[str]) -> None:
    assert _repair_detection_report(None) is None
    _print_repair_detection_report(None)
    assert "Measured:     no" in capsys.readouterr().out


def test_text_report_derives_the_loudest_band_and_harmonic(
    capsys: pytest.CaptureFixture[str],
) -> None:
    _print_repair_detection_report(_repair_detection_report(_defects()))
    out = capsys.readouterr().out
    assert "loudest band -61.5 dBFS (band 2)" in out
    assert "fundamental -50.0 dBFS, loudest harmonic -44.0 dBFS" in out
