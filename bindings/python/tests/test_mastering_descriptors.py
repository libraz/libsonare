"""Parameter descriptors, the EQ ceiling, streaming denoise and enum names in overrides."""

from __future__ import annotations

import numpy as np
import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 48000


def _noise(n: int) -> np.ndarray:
    rng = np.random.default_rng(12345)
    return (0.1 * rng.uniform(-1.0, 1.0, n)).astype(np.float32)


def test_every_logarithmic_parameter_publishes_a_positive_floor_and_a_finite_top() -> None:
    import libsonare

    defects: list[str] = []
    logarithmic = 0
    for entry in libsonare.capability_catalog()["processors"]:
        for param in entry["params"]:
            if param["scale"] != "log":
                continue
            logarithmic += 1
            low = param["uiMin"] if param["uiMin"] is not None else param["min"]
            high = param["uiMax"] if param["uiMax"] is not None else param["max"]
            label = f"{entry['id']}.{param['name']}"
            if low is None or not low > 0:
                defects.append(f"{label} has no positive lower end")
            elif high is None:
                defects.append(f"{label} has no finite upper end")
            elif not low < high:
                defects.append(f"{label} has an empty display range")
    assert logarithmic > 800
    assert defects == []


@pytest.mark.parametrize("stage", ["repair.denoiseClassical", "repair.dereverbClassical"])
def test_transform_size_is_powers_of_two_and_the_hop_has_no_stale_maximum(stage: str) -> None:
    import libsonare

    info = {param["name"]: param for param in libsonare.mastering_insert_param_info(stage)}
    n_fft, hop = info["nFft"], info["hopLength"]
    assert n_fft["min"] is None and n_fft["max"] is None
    sizes = [choice["value"] for choice in n_fft["choices"]]
    assert len(sizes) > 1
    assert all(size & (size - 1) == 0 for size in sizes)
    assert [choice["name"] for choice in n_fft["choices"]] == [str(size) for size in sizes]
    assert sizes[-1] == 524288
    assert hop["choices"] is None
    assert hop["min"] == 1
    assert hop["max"] is None
    assert hop["dependsOn"] == [{"key": "nFft", "relation": "le", "factor": 0.5}]


def test_param_info_serves_the_catalog_rows_of_an_offline_repair_stage() -> None:
    import libsonare

    catalog = {entry["id"]: entry for entry in libsonare.mastering_processor_catalog()}
    for stage in ("repair.declick", "repair.declip", "repair.trimSilence"):
        info = libsonare.mastering_insert_param_info(stage)
        assert info
        assert info == catalog[stage]["params"]
        assert all(param["id"] is None for param in info)
    assert libsonare.mastering_insert_param_info("repair.noSuchStage") == []


def test_eq_ceiling_follows_the_rate_the_insert_is_built_for() -> None:
    import libsonare
    from libsonare import SonareError

    def ceiling(rate: int | None) -> float:
        info = libsonare.mastering_insert_param_info("eq.parametric", rate)
        return next(p for p in info if p["name"] == "band0.frequencyHz")["max"]

    assert ceiling(None) == 24000
    assert ceiling(96000) == 48000

    x = _noise(2048)
    params = {"band0.frequencyHz": 30000.0, "band0.gainDb": 3.0}
    libsonare.mastering_process("eq.parametric", x, 96000, params)
    with pytest.raises(SonareError, match=r"below 24000 Hz \(Nyquist at 48000 Hz\)"):
        libsonare.mastering_process("eq.parametric", x, SR, params)


def test_streaming_denoise_prepares_at_its_default_estimator() -> None:
    import libsonare

    with libsonare.StreamingMasteringChain({"repair": {"denoise": {"enabled": True}}}) as chain:
        chain.prepare(SR, 512, 1)
        assert chain.stage_names() == ["repair.denoise"]
        assert len(chain.process_mono(_noise(512))) == 512


def test_streaming_denoise_takes_an_estimator_by_name_and_still_refuses_quantile() -> None:
    import libsonare
    from libsonare import SonareError

    for name in ("mcra", "imcra", "spp"):
        config = {"repair": {"denoise": {"enabled": True, "noiseEstimator": name}}}
        with libsonare.StreamingMasteringChain(config) as chain:
            chain.prepare(SR, 512, 1)
    with pytest.raises(SonareError, match="quantile"):
        libsonare.StreamingMasteringChain(
            {"repair": {"denoise": {"enabled": True, "noiseEstimator": "quantile"}}}
        )


def test_streaming_loudness_gain_measures_through_the_streaming_default() -> None:
    import math

    import libsonare

    gain = libsonare.streaming_loudness_gain(
        _noise(SR),
        SR,
        {"loudness": {"targetLufs": -20.0}, "repair": {"denoise": {"enabled": True}}},
    )
    assert math.isfinite(gain.loudness_static_gain_db)


def test_enum_names_resolve_to_the_numbers_the_flat_list_carries() -> None:
    import libsonare

    x = _noise(SR)
    by_number = libsonare.master_audio(
        x, SR, "pop", {"repair": {"denoise": {"enabled": True, "noiseEstimator": 1, "mode": 1}}}
    )
    by_name = libsonare.master_audio(
        x,
        SR,
        "pop",
        {"repair": {"denoise": {"enabled": True, "noiseEstimator": "mcra", "mode": "mmseStsa"}}},
    )
    flat = libsonare.master_audio(
        x,
        SR,
        "pop",
        {
            "repair.denoise.enabled": True,
            "repair.denoise.noiseEstimator": "mcra",
            "repair.denoise.mode": "mmseStsa",
        },
    )
    assert list(by_name.samples) == list(by_number.samples)
    assert list(flat.samples) == list(by_number.samples)


def test_an_unknown_enum_name_is_refused_with_the_key_and_the_valid_names() -> None:
    import libsonare
    from libsonare import SonareError

    config = {"repair": {"denoise": {"enabled": True, "noiseEstimator": "nope"}}}
    with pytest.raises(
        SonareError,
        match=r"repair\.denoise\.noiseEstimator: unknown name 'nope' \(valid names: quantile, mcra",
    ):
        libsonare.master_audio(_noise(SR), SR, "pop", config)
    with pytest.raises(SonareError, match="unknown name 'nope'"):
        libsonare.StreamingMasteringChain(config)


def test_per_processor_enum_names_resolve_to_the_numbers() -> None:
    import libsonare

    x = _noise(2048)
    by_name = libsonare.mastering_process(
        "saturation.softClipper", x, SR, {"aliasing": "oversample4x"}
    )
    by_number = libsonare.mastering_process("saturation.softClipper", x, SR, {"aliasing": 3})
    assert list(by_name.samples) == list(by_number.samples)
    assert by_name.latency_samples == by_number.latency_samples > 0
    assert libsonare.mastering_insert_timing(
        "saturation.softClipper", {"aliasing": "oversample4x"}, SR
    ) == libsonare.mastering_insert_timing("saturation.softClipper", {"aliasing": 3}, SR)


def test_per_processor_unknown_enum_name_is_refused_with_the_valid_names() -> None:
    import libsonare
    from libsonare import SonareError

    x = _noise(2048)
    with pytest.raises(
        SonareError,
        match=r"aliasing: unknown name 'nope' \(valid names: none, adaa1, adaa2, oversample4x\)",
    ):
        libsonare.mastering_process("saturation.softClipper", x, SR, {"aliasing": "nope"})
    with pytest.raises(SonareError, match="unknown name 'nope'"):
        libsonare.mastering_insert_timing("saturation.softClipper", {"aliasing": "nope"}, SR)
    with pytest.raises(ValueError):
        libsonare.mastering_process("saturation.softClipper", x, SR, {"ceiling": "loud"})
