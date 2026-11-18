"""Tests for the mastering-assistant chain-config extractors.

The referent for every assertion here is the core entry point exposed through
:func:`mastering_assistant_suggest` (the full profile+chain suggestion); nothing
below hardcodes an expected value.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    master_audio,
    master_audio_stereo,
    mastering_assistant_suggest,
    mastering_assistant_suggest_chain,
    mastering_assistant_suggest_chain_stereo,
    mastering_assistant_suggest_stereo,
)
from libsonare._mastering_offline import _flatten_chain_config

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
    # The document's params as the core wrote them: dotted paths, not a re-nested tree.
    assert chain["loudness.targetLufs"] == pytest.approx(-13.0)


def test_chain_stereo_matches_the_suggestion_chain_config() -> None:
    """The stereo chain-only extractor agrees with the stereo suggestion's chain config."""
    suggestion = mastering_assistant_suggest_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    chain = mastering_assistant_suggest_chain_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    assert chain == suggestion.chain_config


def test_chain_overrides_master_audio() -> None:
    """The chain round-trips: as overrides it renders like the suggestion's own chain."""
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
    np.testing.assert_array_equal(result.samples, by_hand.samples)


def test_chain_stereo_overrides_master_audio_stereo() -> None:
    """The extracted stereo chain is accepted as master_audio_stereo overrides as-is."""
    chain = mastering_assistant_suggest_chain_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    result = master_audio_stereo(LEFT, RIGHT, sample_rate=SR, preset="pop", overrides=chain)
    assert result.report is not None
