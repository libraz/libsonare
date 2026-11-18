"""Tests for :func:`libsonare.mastering_preset_params`.

The referent for the key-set assertion is
:func:`libsonare.mastering_assistant_suggest_chain`, which walks the same
``chain_config_to_json`` document shape from a different C entry point.
"""

from __future__ import annotations

import pytest

from libsonare import (
    SonareError,
    mastering_assistant_suggest_chain,
    mastering_preset_names,
    mastering_preset_params,
)
from libsonare._mastering_offline import _chain_config_paths

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
SAMPLES = sine(220.0, 0.2, sr=SR, amp=0.2)


def test_every_preset_key_set_matches_the_assistant_suggested_chain() -> None:
    """Each preset's params carry the same leaves the assistant would suggest."""
    suggested = _chain_config_paths(mastering_assistant_suggest_chain(SAMPLES, sample_rate=SR))
    for preset in mastering_preset_names():
        params = _chain_config_paths(mastering_preset_params(preset))
        assert set(params) == set(suggested), preset


def test_preset_params_are_the_document_params_as_the_core_wrote_them() -> None:
    """Keys are dotted paths, enum leaves are tokens, and no leaf is a container."""
    for preset in mastering_preset_names():
        params = mastering_preset_params(preset)
        assert "loudness.targetLufs" in params, preset
        for path, value in _chain_config_paths(params).items():
            assert isinstance(value, (float, bool, int, str)), (preset, path)
        assert isinstance(params["dynamics.multibandComp"]["crossover"]["slope"], str), preset


def test_unknown_preset_raises() -> None:
    with pytest.raises(SonareError):
        mastering_preset_params("not-a-real-preset")
