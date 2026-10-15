"""voice_change refuses an unusable formant factor as an argument error in both modes."""

from __future__ import annotations

import math

import pytest

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_BAD_FACTORS = [0.0, -2.0, 10.0, math.nan]


@pytest.mark.parametrize("mode", ["relative", "absolute"])
@pytest.mark.parametrize("factor", _BAD_FACTORS, ids=["zero", "negative", "large", "nan"])
def test_unusable_formant_factor_raises_sonare_value_error(mode: str, factor: float) -> None:
    from libsonare import SonareValueError, voice_change

    samples = sine(440.0, 0.25, 22050)
    with pytest.raises(SonareValueError):
        voice_change(samples, 22050, 0.0, factor, mode)


@pytest.mark.parametrize("mode", ["relative", "absolute"])
def test_in_range_formant_factor_is_accepted(mode: str) -> None:
    from libsonare import voice_change

    samples = sine(440.0, 0.25, 22050)
    assert len(voice_change(samples, 22050, 0.0, 1.0, mode)) > 0
