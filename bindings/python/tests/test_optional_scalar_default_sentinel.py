"""The documented "0 selects the library default" scalars on the C ABI.

Every one of these parameters documents 0 as the sentinel that requests the
library default. The convention is only safe when a value that is *not* the
sentinel is either applied or refused -- never replaced. A value quietly
swapped for the default is indistinguishable from the call the caller meant to
make, so each case asserts the outcome (the refusal, or the sample content)
rather than only that the call returned.

The zero cases are the positive control: 0 has to produce the same content as
omitting the parameter, and a nearby non-sentinel value has to produce
*different* content, or "0 selects the default" cannot be told apart from "0 is
applied as 0".
"""

from __future__ import annotations

import ctypes
import math

import numpy as np
import pytest

from libsonare import SonareError
from libsonare._features_core import mel_spectrogram
from libsonare._features_metering import scale_quantize_midi

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
C_MAJOR_MASK = 0b101010110101

# The 12-TET grid anchor the quantizer falls back to (A4).
DEFAULT_REFERENCE_MIDI = 69.0

REFUSED_VALUES = [
    pytest.param(-5.0, id="negative"),
    pytest.param(math.nan, id="nan"),
    pytest.param(math.inf, id="positive-infinity"),
    pytest.param(-math.inf, id="negative-infinity"),
]


def _mel(**kwargs: float) -> np.ndarray:
    result = mel_spectrogram(
        sine(440.0, 0.5, SR), SR, n_fft=512, hop_length=256, n_mels=16, **kwargs
    )
    return np.asarray(result.power, dtype=np.float32)


class TestMelFrequencyBounds:
    """sonare_mel_spectrogram_ex fmin / fmax."""

    @pytest.mark.parametrize("field", ["fmin", "fmax"])
    @pytest.mark.parametrize("value", REFUSED_VALUES)
    def test_refuses_a_negative_or_non_finite_bound(self, field: str, value: float) -> None:
        with pytest.raises(SonareError) as excinfo:
            _mel(**{field: value})
        # The diagnostic has to name the field, so a caller passing several
        # optional bounds knows which one was refused.
        assert field in str(excinfo.value)
        # Which layer answers is part of the contract. A negative bound is
        # representable, so it reaches the core and the core's domain check
        # refuses it. A non-finite one never leaves the binding: the argument
        # reader refuses it there, because the conversion that would carry it
        # is the same one that folds a saturating value onto it.
        wording = (
            "finite non-negative" if math.isfinite(value) else "must be a finite number within"
        )
        assert wording in str(excinfo.value)

    @pytest.mark.parametrize("field", ["fmin", "fmax"])
    def test_zero_selects_the_librosa_default(self, field: str) -> None:
        assert np.array_equal(_mel(**{field: 0.0}), _mel())

    def test_a_non_sentinel_bound_changes_the_spectrogram(self) -> None:
        # Without this the equality above would also hold if 0 were applied as 0.
        assert not np.array_equal(_mel(fmin=300.0), _mel())
        assert not np.array_equal(_mel(fmax=4000.0), _mel())


class TestScaleQuantizerReference:
    """sonare_scale_quantize_midi reference_midi."""

    QUANTIZED = [60.4, 61.6, 66.3, 70.2]

    def test_refuses_a_negative_reference(self) -> None:
        with pytest.raises(SonareError) as excinfo:
            scale_quantize_midi(0, C_MAJOR_MASK, 60.4, reference_midi=-5.0)
        assert "reference_midi" in str(excinfo.value)

    def test_refuses_a_reference_above_the_midi_range(self) -> None:
        with pytest.raises(SonareError) as excinfo:
            scale_quantize_midi(0, C_MAJOR_MASK, 60.4, reference_midi=127.5)
        assert "reference_midi" in str(excinfo.value)

    @pytest.mark.parametrize("value", [math.nan, math.inf, -math.inf])
    def test_the_c_abi_refuses_a_non_finite_reference(self, value: float) -> None:
        # Driven through ctypes rather than the facade: the Python layer rejects
        # a non-finite scalar of its own, which would hide whether the C ABI
        # underneath refuses the value or promotes it to the default.
        from libsonare._runtime import _get_lib

        out = ctypes.c_float(0.0)
        rc = _get_lib().sonare_scale_quantize_midi(
            ctypes.c_int(0),
            ctypes.c_uint16(C_MAJOR_MASK),
            ctypes.c_float(value),
            ctypes.c_float(60.4),
            ctypes.byref(out),
        )
        assert rc != 0

    @pytest.mark.parametrize("midi", QUANTIZED)
    def test_zero_selects_the_default_anchor(self, midi: float) -> None:
        assert scale_quantize_midi(0, C_MAJOR_MASK, midi) == scale_quantize_midi(
            0, C_MAJOR_MASK, midi, reference_midi=0.0
        )
        assert scale_quantize_midi(0, C_MAJOR_MASK, midi, reference_midi=0.0) == (
            scale_quantize_midi(0, C_MAJOR_MASK, midi, reference_midi=DEFAULT_REFERENCE_MIDI)
        )

    def test_a_detuned_reference_moves_the_grid(self) -> None:
        # The anchor only shifts the grid by its fractional part, so the control
        # against "0 is applied as 0" has to be a fractional reference: an
        # integer one is the same grid as the default whatever its value.
        detuned = scale_quantize_midi(0, C_MAJOR_MASK, 60.4, reference_midi=69.5)
        assert detuned != scale_quantize_midi(0, C_MAJOR_MASK, 60.4)
