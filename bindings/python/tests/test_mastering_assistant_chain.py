"""Tests for the mastering-assistant chain-config extractors.

The referent for every assertion here is the core entry point exposed through
:func:`mastering_assistant_suggest` (the full profile+chain document); nothing
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
from libsonare._mastering_pair import _unwrap_chain_params

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 44100
SAMPLES = sine(220.0, 0.4, sr=SR, amp=0.2)
LEFT = SAMPLES
RIGHT = sine(220.0, 0.4, sr=SR, amp=0.15)
PARAMS = {"targetLufs": -13.0, "ceilingDb": -0.8}


def test_chain_matches_the_document_chain_config() -> None:
    """The chain-only extractor agrees with the chain config nested in the document."""
    document = json.loads(mastering_assistant_suggest(SAMPLES, sample_rate=SR, params=PARAMS))
    chain = mastering_assistant_suggest_chain(SAMPLES, sample_rate=SR, params=PARAMS)
    assert chain == document["chainConfig"]["params"]


def test_chain_stereo_matches_the_document_chain_config() -> None:
    """The stereo chain-only extractor agrees with the stereo document's chain config."""
    document = json.loads(
        mastering_assistant_suggest_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    )
    chain = mastering_assistant_suggest_chain_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    assert chain == document["chainConfig"]["params"]


def test_chain_overrides_master_audio() -> None:
    """The extracted chain is accepted as master_audio overrides as-is."""
    chain = mastering_assistant_suggest_chain(SAMPLES, sample_rate=SR, params=PARAMS)
    result = master_audio(SAMPLES, sample_rate=SR, preset_name="pop", overrides=chain)
    assert result.report is not None


def test_chain_stereo_overrides_master_audio_stereo() -> None:
    """The extracted stereo chain is accepted as master_audio_stereo overrides as-is."""
    chain = mastering_assistant_suggest_chain_stereo(LEFT, RIGHT, sample_rate=SR, params=PARAMS)
    result = master_audio_stereo(LEFT, RIGHT, sample_rate=SR, preset_name="pop", overrides=chain)
    assert result.report is not None


def test_non_scalar_chain_param_is_rejected_by_name() -> None:
    """A nested object value is named in the error rather than dropped silently.

    The assistant cannot emit schema v2 today, so there is no C entry point
    that produces a nested value to exercise this through; the unwrap helper
    is exercised directly instead.
    """
    document = json.dumps(
        {
            "version": 2,
            "params": {"dynamics.multibandComp": {"bands": 3}, "loudness.targetLufs": -14.0},
        }
    )
    with pytest.raises(SonareValueError, match="dynamics.multibandComp"):
        _unwrap_chain_params(document)


def test_a_config_without_params_is_refused() -> None:
    """An empty overrides mapping applies the preset unchanged, so it is not a result."""
    with pytest.raises(SonareValueError, match="no params block"):
        _unwrap_chain_params(json.dumps({"version": 1, "params": {}}))
