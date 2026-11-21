"""Tests for the chain config a mastering-assistant suggestion carries.

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
    mastering_assistant_suggest_stereo,
)
from libsonare._mastering_offline import _flatten_chain_config

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 44100
SAMPLES = sine(220.0, 0.4, sr=SR, amp=0.2)
LEFT = SAMPLES
RIGHT = sine(220.0, 0.4, sr=SR, amp=0.15)
PARAMS = {"targetLufs": -13.0, "ceilingDbtp": -0.8}


def test_suggestion_chain_config_carries_the_requested_target() -> None:
    """The suggestion's chain config is the document's params, keyed by dotted path."""
    chain = mastering_assistant_suggest(SAMPLES, sample_rate=SR, params=PARAMS).chain_config
    # The document's params as the core wrote them: dotted paths, not a re-nested tree.
    assert chain["loudness.targetLufs"] == pytest.approx(-13.0)


def test_stereo_suggestion_chain_config_carries_the_requested_target() -> None:
    """The stereo suggestion's chain config carries the requested target as well."""
    chain = mastering_assistant_suggest_stereo(
        LEFT, RIGHT, sample_rate=SR, params=PARAMS
    ).chain_config
    assert chain["loudness.targetLufs"] == pytest.approx(-13.0)


def test_chain_overrides_master_audio() -> None:
    """The chain round-trips: as overrides it renders like the suggestion's own chain."""
    suggestion = mastering_assistant_suggest(SAMPLES, sample_rate=SR, params=PARAMS)
    chain = suggestion.chain_config
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
    chain = mastering_assistant_suggest_stereo(
        LEFT, RIGHT, sample_rate=SR, params=PARAMS
    ).chain_config
    result = master_audio_stereo(LEFT, RIGHT, sample_rate=SR, preset="pop", overrides=chain)
    assert result.report is not None
