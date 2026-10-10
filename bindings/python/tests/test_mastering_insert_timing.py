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


def test_param_info_flags_exclusive_bounds_and_nyquist_ceilings() -> None:
    """A bound the processor rejects itself is flagged; an EQ ceiling follows Nyquist."""
    import libsonare

    compressor = {
        param["name"]: param
        for param in libsonare.mastering_insert_param_info("dynamics.compressor")
    }
    hpf = compressor["sidechainHpfHz"]
    assert hpf["min"] == 0
    assert isinstance(hpf["minExclusive"], bool)
    assert hpf["minExclusive"] is True
    assert hpf["maxExclusive"] is False
    assert hpf["maxRelativeTo"] is None
    eq = {param["name"]: param for param in libsonare.mastering_insert_param_info("eq.parametric")}
    assert eq["band0.frequencyHz"]["maxRelativeTo"] == "nyquist"


def test_param_info_sample_rate_resolves_the_nyquist_ceiling() -> None:
    """A host rate changes an EQ frequency ceiling and leaves a non-rate key alone."""
    import libsonare

    def frequency(rate: int | None) -> dict:
        info = libsonare.mastering_insert_param_info("eq.parametric", sample_rate=rate)
        return {param["name"]: param for param in info}["band0.frequencyHz"]

    assert frequency(None)["max"] == 24000
    assert frequency(44100)["max"] == 22050
    assert frequency(44100)["maxExclusive"] is True
    # An insert built for a known rate reaches that rate's Nyquist frequency.
    assert frequency(96000)["max"] == 48000

    def ratio(rate: int | None) -> dict:
        info = libsonare.mastering_insert_param_info("dynamics.compressor", sample_rate=rate)
        return {param["name"]: param for param in info}["ratio"]

    assert ratio(44100) == ratio(None)
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_insert_param_info("eq.parametric", sample_rate=0)


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


def test_causal_repair_stages_reach_the_generic_insert_path() -> None:
    """The repair inserts answer the timing and descriptor queries every insert answers."""
    import libsonare

    def latency(name: str, params: dict[str, float | bool]) -> int:
        return int(libsonare.mastering_insert_timing(name, params, 48000)["latencySamples"])

    assert latency("repair.decrackle", {}) == 1
    assert latency("repair.dehum", {}) == 0
    assert latency("repair.dehum", {"adaptive": True, "frameSize": 1024}) == 1024
    assert latency("repair.dereverbClassical", {"nFft": 512}) == 511
    # n_fft - 1 plus the hop the gain smoothing lags by.
    assert latency("repair.denoiseClassical", {}) == 1279

    mode = {p["name"]: p for p in libsonare.mastering_insert_param_info("repair.decrackle")}["mode"]
    assert [choice["name"] for choice in mode["choices"] or []] == ["median"]
    with pytest.raises(libsonare.SonareError) as excinfo:
        libsonare.mastering_insert_timing("repair.dereverbClassical", {"wpeEnabled": True}, 48000)
    assert excinfo.value.code == libsonare.ErrorCode.INVALID_PARAMETER
    # The quantile estimator (0) ranks a whole signal; the insert defaults to spp.
    with pytest.raises(libsonare.SonareError):
        libsonare.mastering_insert_timing("repair.denoiseClassical", {"noiseEstimator": 0}, 48000)
    params = {
        p["name"]: p for p in libsonare.mastering_insert_param_info("repair.denoiseClassical")
    }
    assert "quantile" not in [
        choice["name"] for choice in params["noiseEstimator"]["choices"] or []
    ]


def _scene_timing(processor: str, params: dict) -> tuple[int, int]:
    """Latency and tail of the insert a scene builds from ``params``."""
    import json

    import libsonare

    scene = {
        "version": 1,
        "strips": [
            {"id": "a", "inserts": [{"slot": "pre", "processor": processor, "params": params}]}
        ],
        "buses": [{"id": "master", "role": "master", "inserts": []}],
        "connections": [{"source": "a", "destination": "master"}],
    }
    with libsonare.Mixer.from_scene_json(json.dumps(scene), 48000) as mixer:
        return mixer.latency_samples(), mixer.tail_samples()


def _timing_pair(processor: str, params: dict) -> tuple[int, int]:
    import libsonare

    timing = libsonare.mastering_insert_timing(processor, params, 48000)
    return timing["latencySamples"], timing["tailSamples"]


def test_insert_timing_matches_the_constructed_insert_for_an_array_key() -> None:
    """An array-typed key travels as an array and reproduces the scene-built insert."""
    params = {"bandAbsorption": [0.1] * 6}
    pair = _timing_pair("effects.acoustic.roomMorph", params)
    assert pair == _scene_timing("effects.acoustic.roomMorph", params)
    assert pair[1] != _timing_pair("effects.acoustic.roomMorph", {})[1]


def test_insert_timing_matches_the_constructed_insert_for_a_string_key() -> None:
    """A string-typed key travels as a string and reproduces the scene-built insert."""
    import base64
    import struct

    cab_ir = base64.b64encode(struct.pack("<2000f", 0.5, *([0.0] * 1999))).decode()
    params = {"cabIrF32Base64": cab_ir, "cabIrSampleRate": 48000}
    pair = _timing_pair("saturation.ampSim", params)
    assert pair == _scene_timing("saturation.ampSim", params)
    assert pair[1] != _timing_pair("saturation.ampSim", {})[1]
    named = {"preset": "britStack"}
    assert _timing_pair("saturation.ampSim", named) == _scene_timing("saturation.ampSim", named)


def test_insert_timing_refuses_a_value_that_does_not_match_the_key_type() -> None:
    """A wrong type is a TypeError, a non-finite number a SonareValueError."""
    import libsonare

    def timing(name: str, params: dict) -> None:
        libsonare.mastering_insert_timing(name, params, 48000)

    with pytest.raises(TypeError, match="bandAbsorption"):
        timing("effects.acoustic.roomMorph", {"bandAbsorption": 0.1})
    with pytest.raises(TypeError, match="bandAbsorption"):
        timing("effects.acoustic.roomMorph", {"bandAbsorption": ["a"]})
    with pytest.raises(TypeError, match="bandAbsorption"):
        timing("effects.acoustic.roomMorph", {"bandAbsorption": "0.1"})
    with pytest.raises(TypeError, match="preset"):
        timing("saturation.ampSim", {"preset": 3})
    with pytest.raises(TypeError, match="driveDb"):
        timing("saturation.softClipper", {"driveDb": [1.0]})
    with pytest.raises(TypeError, match="driveDb"):
        timing("saturation.softClipper", {"driveDb": "loud"})
    with pytest.raises(libsonare.SonareValueError, match="bandAbsorption"):
        timing("effects.acoustic.roomMorph", {"bandAbsorption": [float("nan")]})
    with pytest.raises(libsonare.SonareError):
        timing("saturation.ampSim", {"preset": "nope"})
