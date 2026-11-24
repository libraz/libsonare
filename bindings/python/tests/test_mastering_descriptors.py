"""Parameter descriptors, the EQ ceiling, streaming denoise and enum names in overrides."""

from __future__ import annotations

import numpy as np
import pytest

from ._helpers import LIB_AVAILABLE, raises_coded

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 48000


def _noise(n: int) -> np.ndarray:
    rng = np.random.default_rng(12345)
    return (0.1 * rng.uniform(-1.0, 1.0, n)).astype(np.float32)


def test_every_logarithmic_parameter_publishes_a_positive_floor_and_a_finite_top() -> None:
    import libsonare

    defects: list[str] = []
    logarithmic = 0
    for entry in libsonare.capability_catalog().processors:
        for param in entry.params:
            if param.scale != "log":
                continue
            logarithmic += 1
            low = param.ui_min if param.ui_min is not None else param.min
            high = param.ui_max if param.ui_max is not None else param.max
            label = f"{entry.id}.{param.name}"
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

    info = {param.name: param for param in libsonare.mastering_insert_param_info(stage)}
    n_fft, hop = info["nFft"], info["hopLength"]
    assert n_fft.min is None and n_fft.max is None
    sizes = [choice.value for choice in n_fft.choices]
    assert len(sizes) > 1
    assert all(size & (size - 1) == 0 for size in sizes)
    assert [choice.name for choice in n_fft.choices] == [str(size) for size in sizes]
    assert sizes[-1] == 524288
    assert hop.choices is None
    assert hop.min == 1
    assert hop.max is None
    assert hop.depends_on == (
        libsonare.MasteringInsertParamDependency(key="nFft", relation="le", factor=0.5),
    )


def test_param_info_serves_the_catalog_rows_of_an_offline_repair_stage() -> None:
    import libsonare

    catalog = {entry.id: entry for entry in libsonare.mastering_processor_catalog()}
    for stage in ("repair.declick", "repair.declip", "repair.trimSilence"):
        info = libsonare.mastering_insert_param_info(stage)
        assert info
        assert tuple(info) == catalog[stage].params
        assert all(param.id is None for param in info)
    with pytest.raises(
        libsonare.SonareValueError,
        match=r"^mastering_insert_param_info: unknown insert processor: repair\.noSuchStage$",
    ):
        libsonare.mastering_insert_param_info("repair.noSuchStage")


@pytest.mark.parametrize("call", ["mastering_insert_param_info", "mastering_insert_param_names"])
def test_a_retired_insert_id_is_refused_with_its_current_spelling(call: str) -> None:
    import libsonare

    with pytest.raises(
        libsonare.SonareValueError,
        match=rf"^{call}: unknown insert processor: eq\.pultec \(renamed to eq\.passiveTube\)$",
    ):
        getattr(libsonare, call)("eq.pultec")


@pytest.mark.parametrize("stage", ["effects.reverb.room", "effects.acoustic.roomMorph"])
def test_receiver_spacing_names_the_room_geometry_it_is_accepted_against(stage: str) -> None:
    import libsonare

    info = libsonare.mastering_insert_param_info(stage)
    spacing = {param.name: param for param in info}["receiverSpacingM"]
    assert spacing.max == pytest.approx(2.5, abs=1e-3)
    assert spacing.depends_on == ()
    assert {"lengthM", "widthM", "listenerX", "listenerY"} <= set(spacing.coupled_with)
    assert all(isinstance(param.coupled_with, tuple) for param in info)


def test_eq_ceiling_follows_the_rate_the_insert_is_built_for() -> None:
    import libsonare
    from libsonare import SonareValueError

    def ceiling(rate: int | None) -> float:
        info = libsonare.mastering_insert_param_info("eq.parametric", rate)
        return next(p for p in info if p.name == "band0.frequencyHz").max

    assert ceiling(None) == 24000
    assert ceiling(96000) == 48000

    x = _noise(2048)
    params = {"band0.frequencyHz": 30000.0, "band0.gainDb": 3.0}
    libsonare.mastering_apply_processor("eq.parametric", x, 96000, params)
    with pytest.raises(
        SonareValueError,
        match=(
            r"^mastering_apply_processor: params\.band0\.frequencyHz "
            r"must be finite and in \(0, 24000\)$"
        ),
    ):
        libsonare.mastering_apply_processor("eq.parametric", x, SR, params)


def test_streaming_denoise_prepares_at_its_default_estimator() -> None:
    import libsonare

    with libsonare.StreamingMasteringChain(
        {"repair": {"denoise": {"enabled": True}}},
        sample_rate=SR,
        max_block_size=512,
        num_channels=1,
    ) as chain:
        assert chain.stage_names() == ["repair.denoise"]
        assert len(chain.process_mono(_noise(512))) == 512


def test_streaming_denoise_takes_an_estimator_by_name_and_still_refuses_quantile() -> None:
    import libsonare
    from libsonare import SonareError

    for name in ("mcra", "imcra", "spp"):
        config = {"repair": {"denoise": {"enabled": True, "noiseEstimator": name}}}
        libsonare.StreamingMasteringChain(config, sample_rate=SR, num_channels=1).close()
    with pytest.raises(
        SonareError, match=r"noiseEstimator quantile is not supported for a streaming"
    ):
        libsonare.StreamingMasteringChain(
            {"repair": {"denoise": {"enabled": True, "noiseEstimator": "quantile"}}},
            sample_rate=SR,
            num_channels=1,
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


def test_an_enum_leaf_takes_its_name_and_refuses_the_number() -> None:
    import libsonare
    from libsonare import ErrorCode

    x = _noise(SR)
    by_name = libsonare.master_audio(
        x,
        SR,
        preset="pop",
        overrides={
            "repair": {"denoise": {"enabled": True, "noiseEstimator": "mcra", "mode": "mmseStsa"}}
        },
    )
    assert by_name.report is not None
    with raises_coded(
        ErrorCode.INVALID_PARAMETER,
        match=r"overrides\.repair\.denoise\.mode must be one of logMmse, mmseStsa, "
        r"spectralSubtraction$",
    ):
        libsonare.master_audio(
            x,
            SR,
            preset="pop",
            overrides={"repair": {"denoise": {"enabled": True, "mode": 1}}},
        )


def test_an_unknown_enum_name_is_refused_with_the_key_and_the_valid_names() -> None:
    import libsonare
    from libsonare import SonareError

    config = {"repair": {"denoise": {"enabled": True, "noiseEstimator": "nope"}}}
    with pytest.raises(
        SonareError,
        match=r"overrides\.repair\.denoise\.noiseEstimator must be one of quantile, mcra, imcra, "
        r"spp$",
    ):
        libsonare.master_audio(_noise(SR), SR, preset="pop", overrides=config)
    with pytest.raises(
        SonareError, match=r"config\.repair\.denoise\.noiseEstimator must be one of quantile"
    ):
        libsonare.StreamingMasteringChain(config, sample_rate=SR, num_channels=1)


def test_per_processor_enum_names_resolve_to_the_numbers() -> None:
    import libsonare

    x = _noise(2048)
    by_name = libsonare.mastering_apply_processor(
        "saturation.softClipper", x, SR, {"aliasing": "oversample4x"}
    )
    by_number = libsonare.mastering_apply_processor(
        "saturation.softClipper", x, SR, {"aliasing": 3}
    )
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
        match=r"aliasing must be one of none, adaa1, adaa2, oversample4x$",
    ):
        libsonare.mastering_apply_processor("saturation.softClipper", x, SR, {"aliasing": "nope"})
    with pytest.raises(SonareError, match=r"aliasing must be one of none, adaa1, adaa2"):
        libsonare.mastering_insert_timing("saturation.softClipper", {"aliasing": "nope"}, SR)
    with pytest.raises(ValueError):
        libsonare.mastering_apply_processor("saturation.softClipper", x, SR, {"ceiling": "loud"})
