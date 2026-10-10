"""Typed multiband (crossover list + bands list) on the chain facades.

A nested config with ``crossover.cutoffsHz`` and ``bands`` lists flattens to the
ordinal keys the core accepts, so it must render exactly like the same config
written as those flat keys. The referent for every assertion is the flat
spelling run through the same entry point in the same build.
"""

from __future__ import annotations

import json

import numpy as np
import pytest

from libsonare import (
    SonareValueError,
    StreamingMasteringChain,
    master_audio,
    mastering_chain,
    mastering_chain_stereo,
)
from libsonare._mastering_offline import _enum_value, _flatten_chain_config
from libsonare._mastering_pair import _unwrap_chain_params

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
    assert flat == _flatten_chain_config(FLAT)


def test_enum_names_resolve_on_nested_and_band_paths() -> None:
    """Slope and a band detector name resolve to the numbers the core reports."""
    assert (
        _enum_value(None, f"{MB}.crossover.slope", "lr8")
        == _flatten_chain_config(TYPED)[f"{MB}.crossover.slope"]
    )
    detector = _flatten_chain_config(TYPED)[f"{MB}.bands.2.detector"]
    assert detector == _enum_value(None, f"{MB}.band2.detector", "logRms")
    assert detector == _enum_value(None, "dynamics.compressor.detector", "logRms")


def test_typed_config_renders_like_flat_keys_mono() -> None:
    nested = mastering_chain(LEFT, SR, TYPED)
    flat = mastering_chain(LEFT, SR, FLAT)
    assert nested.samples == flat.samples
    assert MB in " ".join(nested.stages)


def test_typed_config_renders_like_flat_keys_stereo() -> None:
    nested = mastering_chain_stereo(LEFT, RIGHT, SR, TYPED)
    flat = mastering_chain_stereo(LEFT, RIGHT, SR, FLAT)
    assert nested.left == flat.left
    assert nested.right == flat.right


def test_typed_config_changes_the_render() -> None:
    """The typed bands are read: a different band count renders differently."""
    three = {f"{MB}.enabled": True, f"{MB}.lowCutoffHz": 200.0, f"{MB}.highCutoffHz": 5000.0}
    assert mastering_chain(LEFT, SR, TYPED).samples != mastering_chain(LEFT, SR, three).samples


def test_typed_overrides_on_a_preset_render_like_flat_keys() -> None:
    nested = master_audio(LEFT, SR, "pop", TYPED)
    flat = master_audio(LEFT, SR, "pop", FLAT)
    assert nested.samples == flat.samples


def test_streaming_chain_typed_matches_flat_keys() -> None:
    block = 512
    outputs = []
    for config in (TYPED, FLAT):
        with StreamingMasteringChain(config) as chain:
            chain.prepare(SR, block, 1)
            rendered: list[float] = []
            for start in range(0, len(LEFT) - block + 1, block):
                rendered.extend(chain.process_mono(LEFT[start : start + block].tolist()))
        outputs.append(rendered)
    assert outputs[0] == outputs[1]
    assert np.any(np.asarray(outputs[0]) != 0.0)


@pytest.mark.parametrize(
    "config, path",
    [
        ({f"{MB}.crossover.cutoffsHz": []}, f"{MB}.crossover.cutoffsHz"),
        ({"dynamics": {"multibandComp": {"crossover": {"cutoffsHz": []}}}}, f"{MB}.crossover"),
        ({"dynamics": {"multibandComp": {"bands": []}}}, f"{MB}.bands"),
    ],
)
def test_empty_list_is_refused_by_path(config: dict, path: str) -> None:
    with pytest.raises(SonareValueError, match="is an empty list, which has no flat spelling"):
        _flatten_chain_config(config)
    with pytest.raises(SonareValueError, match=path.replace(".", r"\.")):
        mastering_chain(LEFT, SR, config)


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


def test_v2_document_unwraps_to_array_spelling_and_round_trips() -> None:
    slope = int(_enum_value(None, f"{MB}.crossover.slope", "lr8"))
    flat = _unwrap_chain_params(_v2_document(slope))
    assert flat["loudness.targetLufs"] == -14.0
    assert flat[f"{MB}.enabled"] is True
    assert [flat[f"{MB}.crossover.cutoffsHz.{i}"] for i in range(3)] == [200.0, 1200.0, 5000.0]
    assert flat[f"{MB}.crossover.slope"] == slope
    assert flat[f"{MB}.bands.3.ratio"] == 4.0
    assert not any(key == MB or key.endswith("bands") for key in flat)
    # The unwrapped keys are accepted back and reproduce the nested form.
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
    assert _flatten_chain_config(flat) == _flatten_chain_config(nested)
    assert mastering_chain(LEFT, SR, flat).samples == mastering_chain(LEFT, SR, nested).samples


def test_v1_representable_config_keeps_the_shorthand() -> None:
    """A suggestion that fits the three-band shorthand is not rewritten into lists."""
    from libsonare import mastering_assistant_suggest_chain

    chain = mastering_assistant_suggest_chain(LEFT, SR)
    assert not any(".bands." in key or "cutoffsHz." in key for key in chain)
    document = json.dumps({"version": 1, "params": {f"{MB}.lowCutoffHz": 200.0}})
    assert _unwrap_chain_params(document) == {f"{MB}.lowCutoffHz": 200.0}


def test_unwrap_refuses_a_non_scalar_leaf_by_name() -> None:
    document = json.dumps({"version": 2, "params": {MB: {"crossover": {"slope": "lr8"}}}})
    with pytest.raises(SonareValueError, match=r"dynamics\.multibandComp\.crossover\.slope"):
        _unwrap_chain_params(document)
    with pytest.raises(SonareValueError, match=r"is an empty list"):
        _unwrap_chain_params(json.dumps({"version": 2, "params": {MB: {"bands": []}}}))
