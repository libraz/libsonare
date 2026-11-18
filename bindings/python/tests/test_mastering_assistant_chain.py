"""Tests for the mastering-assistant chain-config extractors.

The referent for every assertion here is the core entry point exposed through
:func:`mastering_assistant_suggest` (the full profile+chain suggestion); nothing
below hardcodes an expected value.
"""

from __future__ import annotations

import json

import pytest

from libsonare import (
    SonareValueError,
    master_audio,
    master_audio_stereo,
    mastering_assistant_suggest,
    mastering_assistant_suggest_chain,
    mastering_assistant_suggest_chain_stereo,
    mastering_assistant_suggest_stereo,
)
from libsonare._mastering_offline import _flatten_chain_config
from libsonare._mastering_pair import _chain_config_from_document

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 44100
SAMPLES = sine(220.0, 0.4, sr=SR, amp=0.2)
LEFT = SAMPLES
RIGHT = sine(220.0, 0.4, sr=SR, amp=0.15)
PARAMS = {"targetLufs": -13.0, "ceilingDb": -0.8}


def test_chain_matches_the_suggestion_chain_config() -> None:
    """The chain-only extractor agrees with the chain config the full suggestion carries."""
    suggestion = mastering_assistant_suggest(SAMPLES, sample_rate=SR, params=PARAMS)
    chain = mastering_assistant_suggest_chain(SAMPLES, sample_rate=SR, params=PARAMS)
    assert chain == suggestion.chain_config
    assert not any("." in key for key in chain)


def test_chain_stereo_matches_the_suggestion_chain_config() -> None:
    """The stereo chain-only extractor agrees with the stereo suggestion's chain config."""
    suggestion = mastering_assistant_suggest_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    chain = mastering_assistant_suggest_chain_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    assert chain == suggestion.chain_config


def test_chain_overrides_master_audio() -> None:
    """The nested chain round-trips: as overrides it renders like the suggestion's own chain."""
    suggestion = mastering_assistant_suggest(SAMPLES, sample_rate=SR, params=PARAMS)
    chain = mastering_assistant_suggest_chain(SAMPLES, sample_rate=SR, params=PARAMS)
    assert _flatten_chain_config(chain) == _flatten_chain_config(suggestion.chain_config)
    result = master_audio(SAMPLES, sample_rate=SR, preset="pop", overrides=chain)
    by_hand = master_audio(
        SAMPLES,
        sample_rate=SR,
        preset="pop",
        overrides=suggestion.chain_config,
    )
    assert result.report is not None
    assert result.samples == by_hand.samples


def test_chain_stereo_overrides_master_audio_stereo() -> None:
    """The extracted stereo chain is accepted as master_audio_stereo overrides as-is."""
    chain = mastering_assistant_suggest_chain_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    result = master_audio_stereo(LEFT, RIGHT, sample_rate=SR, preset="pop", overrides=chain)
    assert result.report is not None


def test_nested_chain_param_merges_into_the_nested_config() -> None:
    """A schema v2 structured block merges at its path, lists staying lists.

    The assistant cannot emit schema v2 today, so there is no C entry point
    that produces a nested value to exercise this through; the unwrap helper
    is exercised directly instead.
    """
    document = json.dumps(
        {
            "version": 2,
            "params": {
                "dynamics.multibandComp": {
                    "crossover": {"cutoffsHz": [200.0, 2000.0]},
                    "bands": [{"ratio": 2.0}, {"ratio": 3.0}, {"ratio": 4.0}],
                },
                "loudness.targetLufs": -14.0,
            },
        }
    )
    assert _chain_config_from_document(document) == {
        "dynamics": {
            "multibandComp": {
                "crossover": {"cutoffsHz": [200.0, 2000.0]},
                "bands": [{"ratio": 2.0}, {"ratio": 3.0}, {"ratio": 4.0}],
            }
        },
        "loudness": {"targetLufs": -14.0},
    }


def test_non_numeric_chain_param_leaf_is_rejected_by_name() -> None:
    """A leaf that is neither a number nor a boolean is named rather than dropped."""
    document = json.dumps({"version": 2, "params": {"dynamics.multibandComp": {"slope": None}}})
    with pytest.raises(SonareValueError, match="dynamics.multibandComp.slope"):
        _chain_config_from_document(document)


def test_a_config_without_params_is_refused() -> None:
    """An empty overrides mapping applies the preset unchanged, so it is not a result."""
    with pytest.raises(SonareValueError, match="no params block"):
        _chain_config_from_document(json.dumps({"version": 1, "params": {}}))
