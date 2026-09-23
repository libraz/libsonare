"""Tests for the construction-key catalog entries and the timing query."""

from __future__ import annotations

import math

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def test_soft_clipper_param_info_publishes_the_aliasing_enum() -> None:
    """A construction-only enum key carries type, choices and a null id."""
    import libsonare

    params = {
        param["name"]: param
        for param in libsonare.mastering_insert_param_info("saturation.softClipper")
    }
    aliasing = params["aliasing"]
    assert aliasing["type"] == "enum"
    assert aliasing["id"] is None
    assert aliasing["rtSafe"] is False
    assert aliasing["choices"] is not None
    assert {"name": "oversample4x", "value": 3} in aliasing["choices"]
    assert "ceiling" in params


def test_insert_timing_reports_positive_latency_for_oversampling() -> None:
    """Selecting the 4x-oversampling choice reports a non-zero latency."""
    import libsonare

    timing = libsonare.mastering_insert_timing("saturation.softClipper", {"aliasing": 3}, 48000)
    assert timing["latencySamples"] > 0
    assert timing["tailSamples"] >= 0


def test_insert_timing_at_defaults_matches_the_capability_catalog() -> None:
    """An empty configuration answers the same as the catalog's default probe."""
    import libsonare

    timing = libsonare.mastering_insert_timing("saturation.softClipper", {}, 48000)
    catalog = libsonare.capability_catalog()
    entry = next(
        processor
        for processor in catalog["processors"]
        if processor["id"] == "saturation.softClipper"
    )
    assert timing["latencySamples"] == entry["latencySamples"]
    assert timing["tailSamples"] == entry["tailSamples"]


def test_insert_timing_rejects_an_unknown_key() -> None:
    """A key the insert does not read is refused rather than silently ignored."""
    import libsonare

    with pytest.raises(libsonare.SonareError, match="does not read parameter\\(s\\).*bogus"):
        libsonare.mastering_insert_timing("saturation.softClipper", {"bogus": 1.0}, 48000)


def test_insert_timing_rejects_non_finite_and_non_numeric_values() -> None:
    """NaN and a string value are rejected client-side, naming the offending key."""
    import libsonare

    with pytest.raises(libsonare.SonareValueError, match="aliasing"):
        libsonare.mastering_insert_timing(
            "saturation.softClipper", {"aliasing": float("nan")}, 48000
        )
    with pytest.raises(libsonare.SonareValueError, match="aliasing"):
        libsonare.mastering_insert_timing("saturation.softClipper", {"aliasing": "x"}, 48000)  # type: ignore[dict-item]


def test_insert_timing_rejects_an_unknown_insert() -> None:
    """An unknown processor name is rejected rather than answering zero."""
    import libsonare

    with pytest.raises(libsonare.SonareError, match="unknown insert processor"):
        libsonare.mastering_insert_timing("nope.nope", {}, 48000)


def test_insert_timing_accepts_boolean_values() -> None:
    """A boolean value travels as a JSON boolean rather than 0/1."""
    import libsonare

    params = {
        param["name"]: param
        for param in libsonare.mastering_insert_param_info("dynamics.parallelComp")
    }
    assert params["linkedDetection"]["type"] == "boolean"
    timing = libsonare.mastering_insert_timing(
        "dynamics.parallelComp", {"linkedDetection": True}, 48000
    )
    assert math.isfinite(timing["latencySamples"])
