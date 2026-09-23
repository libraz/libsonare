"""The loaded library's ``masteringPresets`` catalog entry against the tracked file."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from libsonare import capability_catalog

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

ROOT = Path(__file__).resolve().parents[3]


def _tracked_mastering_presets() -> list[dict[str, object]]:
    catalog = json.loads((ROOT / "tools/capability-catalog.json").read_text(encoding="utf-8"))
    return catalog["masteringPresets"]


def test_mastering_presets_matches_the_tracked_catalog() -> None:
    """The loaded library's catalog and the checked-in generated file agree exactly."""
    assert capability_catalog()["masteringPresets"] == _tracked_mastering_presets()


def test_mastering_presets_carries_restoration_entries_with_null_loudness() -> None:
    entries = capability_catalog()["masteringPresets"]
    restoration = [entry for entry in entries if entry["kind"] == "restoration"]
    assert restoration
    for entry in restoration:
        assert entry["targetLufs"] is None
        assert entry["truePeakCeilingDb"] is None
        assert entry["maxLimiterGainReductionDb"] is None


def test_mastering_presets_carries_mastering_entries_with_numeric_loudness() -> None:
    entries = capability_catalog()["masteringPresets"]
    mastering = [entry for entry in entries if entry["kind"] == "mastering"]
    assert mastering
    for entry in mastering:
        assert isinstance(entry["targetLufs"], (int, float))
        assert isinstance(entry["truePeakCeilingDb"], (int, float))
        assert isinstance(entry["maxLimiterGainReductionDb"], (int, float))
