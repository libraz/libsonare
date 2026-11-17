"""The loaded library's ``masteringPresets`` catalog entry against the tracked file."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from libsonare import MasteringPresetCatalogEntry, capability_catalog

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

ROOT = Path(__file__).resolve().parents[3]


def _tracked_mastering_presets() -> tuple[MasteringPresetCatalogEntry, ...]:
    catalog = json.loads((ROOT / "tools/capability-catalog.json").read_text(encoding="utf-8"))
    return tuple(
        MasteringPresetCatalogEntry(
            name=row["name"],
            kind=row["kind"],
            target_lufs=row["targetLufs"],
            true_peak_ceiling_db=row["truePeakCeilingDb"],
            max_limiter_gain_reduction_db=row["maxLimiterGainReductionDb"],
        )
        for row in catalog["masteringPresets"]
    )


def test_mastering_presets_matches_the_tracked_catalog() -> None:
    """The loaded library's catalog and the checked-in generated file agree exactly."""
    assert capability_catalog().mastering_presets == _tracked_mastering_presets()


def test_mastering_presets_carries_restoration_entries_with_null_loudness() -> None:
    entries = capability_catalog().mastering_presets
    restoration = [entry for entry in entries if entry.kind == "restoration"]
    assert restoration
    for entry in restoration:
        assert entry.target_lufs is None
        assert entry.true_peak_ceiling_db is None
        assert entry.max_limiter_gain_reduction_db is None


def test_mastering_presets_carries_mastering_entries_with_numeric_loudness() -> None:
    entries = capability_catalog().mastering_presets
    mastering = [entry for entry in entries if entry.kind == "mastering"]
    assert mastering
    for entry in mastering:
        assert isinstance(entry.target_lufs, (int, float))
        assert isinstance(entry.true_peak_ceiling_db, (int, float))
        assert isinstance(entry.max_limiter_gain_reduction_db, (int, float))
