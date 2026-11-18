"""Typed multiband (crossover list + bands list) on the chain facades.

A nested config with ``crossover.cutoffsHz`` and ``bands`` lists flattens to the
ordinal keys the core accepts. The referent is ``FLAT``, the native spelling
written out by hand: the flattener must produce it, and nesting it back must
render exactly like the typed config through the same entry point.
"""

from __future__ import annotations

import json

import numpy as np
import pytest

from libsonare import (
    SonareValueError,
    StreamingMasteringChain,
    master_audio,
    master_audio_stereo,
)
from libsonare._mastering_offline import _enum_value, _flatten_chain_config, _nest_chain_config
from libsonare._mastering_pair import _chain_config_from_document

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
MB = "dynamics.multibandComp"
LEFT = sine(220.0, 0.4, sr=SR, amp=0.4) + sine(3100.0, 0.4, sr=SR, amp=0.15)
RIGHT = sine(330.0, 0.4, sr=SR, amp=0.3) + sine(5200.0, 0.4, sr=SR, amp=0.1)

TYPED = {
    "dynamics": {
        "multibandComp": {
            "enabled": True,
            "crossover": {"cutoffsHz": [200.0, 1200.0, 5000.0], "slope": "lr8"},
            "bands": [
                {"thresholdDb": -30.0, "ratio": 3.0},
                {"thresholdDb": -26.0, "ratio": 2.0, "attackMs": 5.0},
                {"thresholdDb": -24.0, "ratio": 2.5, "detector": "logRms"},
                {"thresholdDb": -20.0, "ratio": 4.0, "makeupGainDb": 1.5},
            ],
        }
    }
}

FLAT = {
    f"{MB}.enabled": True,
    f"{MB}.crossover.cutoffsHz.0": 200.0,
    f"{MB}.crossover.cutoffsHz.1": 1200.0,
    f"{MB}.crossover.cutoffsHz.2": 5000.0,
    f"{MB}.crossover.slope": "lr8",
    f"{MB}.bands.0.thresholdDb": -30.0,
    f"{MB}.bands.0.ratio": 3.0,
    f"{MB}.bands.1.thresholdDb": -26.0,
    f"{MB}.bands.1.ratio": 2.0,
    f"{MB}.bands.1.attackMs": 5.0,
    f"{MB}.bands.2.thresholdDb": -24.0,
    f"{MB}.bands.2.ratio": 2.5,
    f"{MB}.bands.2.detector": "logRms",
    f"{MB}.bands.3.thresholdDb": -20.0,
    f"{MB}.bands.3.ratio": 4.0,
    f"{MB}.bands.3.makeupGainDb": 1.5,
}


def test_nested_lists_flatten_to_ordinal_keys() -> None:
    """The flattener spells a list element as ``<path>.<index>`` and keeps enum names resolved."""
    flat = _flatten_chain_config(TYPED)
    assert flat[f"{MB}.crossover.cutoffsHz.2"] == 5000.0
    assert flat[f"{MB}.bands.1.attackMs"] == 5.0
    assert flat[f"{MB}.enabled"] == 1.0
    assert flat.keys() == FLAT.keys()
    assert _nest_chain_config(FLAT) == TYPED


def test_enum_names_resolve_on_nested_and_band_paths() -> None:
    """Slope and a band detector name resolve to the numbers the core reports."""
    assert (
        _enum_value(None, f"{MB}.crossover.slope", "lr8")
        == _flatten_chain_config(TYPED)[f"{MB}.crossover.slope"]
    )
    detector = _flatten_chain_config(TYPED)[f"{MB}.bands.2.detector"]
    assert detector == _enum_value(None, f"{MB}.bands.2.detector", "logRms")
    assert detector == _enum_value(None, "dynamics.compressor.detector", "logRms")


def test_dotted_key_is_refused_by_name() -> None:
    with pytest.raises(SonareValueError, match=r"dotted keys are not accepted; nest the object"):
        master_audio(LEFT, SR, overrides=FLAT)
    with pytest.raises(SonareValueError, match=r"dynamics\.multibandComp"):
        master_audio(LEFT, SR, preset="pop", overrides={MB: {"enabled": True}})


def test_typed_config_renders_like_flat_keys_mono() -> None:
    nested = master_audio(LEFT, SR, overrides=TYPED)
    flat = master_audio(LEFT, SR, overrides=_nest_chain_config(FLAT))
    np.testing.assert_array_equal(nested.samples, flat.samples)
    assert MB in " ".join(nested.stages)


def test_typed_config_renders_like_flat_keys_stereo() -> None:
    nested = master_audio_stereo(LEFT, RIGHT, SR, overrides=TYPED)
    flat = master_audio_stereo(LEFT, RIGHT, SR, overrides=_nest_chain_config(FLAT))
    np.testing.assert_array_equal(nested.left, flat.left)
    np.testing.assert_array_equal(nested.right, flat.right)


def test_typed_config_changes_the_render() -> None:
    """The typed bands are read: a different band count renders differently."""
    three = {
        "dynamics": {
            "multibandComp": {"enabled": True, "crossover": {"cutoffsHz": [200.0, 5000.0]}}
        }
    }
    assert not np.array_equal(
        master_audio(LEFT, SR, overrides=TYPED).samples,
        master_audio(LEFT, SR, overrides=three).samples,
    )


def test_typed_overrides_on_a_preset_render_like_flat_keys() -> None:
    nested = master_audio(LEFT, SR, preset="pop", overrides=TYPED)
    flat = master_audio(LEFT, SR, preset="pop", overrides=_nest_chain_config(FLAT))
    np.testing.assert_array_equal(nested.samples, flat.samples)


def test_streaming_chain_typed_matches_flat_keys() -> None:
    block = 512
    outputs = []
    for config in (TYPED, _nest_chain_config(FLAT)):
        with StreamingMasteringChain(
            config, sample_rate=SR, max_block_size=block, num_channels=1
        ) as chain:
            rendered: list[float] = []
            for start in range(0, len(LEFT) - block + 1, block):
                rendered.extend(chain.process_mono(LEFT[start : start + block].tolist()))
        outputs.append(rendered)
    assert outputs[0] == outputs[1]
    assert np.any(np.asarray(outputs[0]) != 0.0)


@pytest.mark.parametrize(
    "config, path",
    [
        (
            {"dynamics": {"multibandComp": {"crossover": {"cutoffsHz": []}}}},
            f"{MB}.crossover.cutoffsHz",
        ),
        ({"dynamics": {"multibandComp": {"bands": []}}}, f"{MB}.bands"),
    ],
)
def test_empty_list_is_refused_by_path(config: dict, path: str) -> None:
    with pytest.raises(SonareValueError, match="is an empty list, which has no flat spelling"):
        _flatten_chain_config(config)
    with pytest.raises(SonareValueError, match=path.replace(".", r"\.")):
        master_audio(LEFT, SR, overrides=config)


def _v2_document(slope: int) -> str:
    return json.dumps(
        {
            "version": 2,
            "params": {
                "loudness.targetLufs": -14.0,
                MB: {
                    "enabled": True,
                    "crossover": {
                        "cutoffsHz": [200.0, 1200.0, 5000.0],
                        "slope": slope,
                        "mode": 0,
                        "firKernelSize": 1024,
                    },
                    "bands": [
                        {"thresholdDb": -30.0, "ratio": 3.0},
                        {"thresholdDb": -26.0, "ratio": 2.0},
                        {"thresholdDb": -24.0, "ratio": 2.5},
                        {"thresholdDb": -20.0, "ratio": 4.0},
                    ],
                },
            },
        }
    )


def test_v2_document_nests_and_round_trips() -> None:
    slope = int(_enum_value(None, f"{MB}.crossover.slope", "lr8"))
    config = _chain_config_from_document(_v2_document(slope))
    nested = {
        "loudness": {"targetLufs": -14.0},
        "dynamics": {
            "multibandComp": {
                "enabled": True,
                "crossover": {
                    "cutoffsHz": [200.0, 1200.0, 5000.0],
                    "slope": slope,
                    "mode": 0,
                    "firKernelSize": 1024,
                },
                "bands": [
                    {"thresholdDb": -30.0, "ratio": 3.0},
                    {"thresholdDb": -26.0, "ratio": 2.0},
                    {"thresholdDb": -24.0, "ratio": 2.5},
                    {"thresholdDb": -20.0, "ratio": 4.0},
                ],
            }
        },
    }
    assert config == nested
    assert _flatten_chain_config(config)[f"{MB}.crossover.cutoffsHz.2"] == 5000.0


@pytest.mark.parametrize(
    ("config", "current"),
    [
        ({"lowCutoffHz": 200.0}, "cutoff0Hz"),
        ({"highCutoffHz": 5000.0}, "cutoff1Hz"),
        ({"band2": {"detector": "rms"}}, "bands.2.detector"),
    ],
)
def test_multiband_shorthand_keys_are_refused_naming_the_current_key(
    config: dict, current: str
) -> None:
    """The three-band shorthand is gone; the refusal names the list spelling."""
    with pytest.raises(SonareValueError, match=current.replace(".", r"\.")):
        master_audio(LEFT, SR, overrides={"dynamics": {"multibandComp": config}})


def test_document_refuses_a_non_scalar_leaf_by_name() -> None:
    document = json.dumps({"version": 2, "params": {MB: {"crossover": {"slope": "lr8"}}}})
    with pytest.raises(SonareValueError, match=r"dynamics\.multibandComp\.crossover\.slope"):
        _chain_config_from_document(document)
    with pytest.raises(SonareValueError, match=r"is an empty list"):
        _chain_config_from_document(json.dumps({"version": 2, "params": {MB: {"bands": []}}}))
