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
from collections.abc import Callable

import numpy as np
import pytest

import libsonare
from libsonare import SonareError, SonareValueError
from libsonare._features_core import mel_spectrogram
from libsonare._features_metering import scale_correction_semitones, scale_quantize_midi

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


def _mel(**kwargs: float | None) -> np.ndarray:
    result = mel_spectrogram(
        sine(440.0, 0.5, SR), SR, n_fft=512, hop_length=256, n_mels=16, **kwargs
    )
    return np.asarray(result.power, dtype=np.float32)


class TestMelFrequencyBounds:
    """sonare_mel_spectrogram fmin / fmax, reached as fmin_hz / fmax_hz."""

    @pytest.mark.parametrize("value", REFUSED_VALUES)
    def test_refuses_a_negative_or_non_finite_lower_edge(self, value: float) -> None:
        with pytest.raises(SonareError) as excinfo:
            _mel(fmin_hz=value)
        # The diagnostic has to name the field, so a caller passing several
        # optional bounds knows which one was refused.
        assert "fmin" in str(excinfo.value)
        # A negative edge is representable, so it reaches the core and the
        # core's domain check refuses it; a non-finite one never leaves the
        # binding, whose argument reader refuses it.
        wording = (
            "must be finite and at least 0" if math.isfinite(value) else "must be finite and in"
        )
        assert wording in str(excinfo.value)

    @pytest.mark.parametrize("value", [0.0, *REFUSED_VALUES])
    def test_refuses_an_upper_edge_that_is_not_positive_and_finite(self, value: float) -> None:
        # 0 is the core's "Nyquist" sentinel; the binding spells that as None,
        # so an explicit 0 is refused rather than read as the default.
        with pytest.raises(SonareError) as excinfo:
            _mel(fmax_hz=value)
        assert "fmax_hz" in str(excinfo.value)

    def test_a_zero_lower_edge_is_zero_hz(self) -> None:
        assert np.array_equal(_mel(fmin_hz=0.0), _mel())

    def test_an_absent_upper_edge_is_the_nyquist(self) -> None:
        assert np.array_equal(_mel(fmax_hz=None), _mel(fmax_hz=SR / 2))

    def test_a_given_bound_changes_the_spectrogram(self) -> None:
        # Without this the equalities above would also hold if the bounds were ignored.
        assert not np.array_equal(_mel(fmin_hz=300.0), _mel())
        assert not np.array_equal(_mel(fmax_hz=4000.0), _mel())


class TestScaleQuantizerReference:
    """sonare_scale_quantize_midi scale_reference_midi: None is A4, an explicit 0 is MIDI note 0."""

    QUANTIZED = [60.4, 61.6, 66.3, 70.2]

    def test_refuses_a_negative_reference(self) -> None:
        with pytest.raises(SonareError) as excinfo:
            scale_quantize_midi(0, C_MAJOR_MASK, 60.4, scale_reference_midi=-5.0)
        assert "reference_midi" in str(excinfo.value)

    def test_refuses_a_reference_above_the_midi_range(self) -> None:
        with pytest.raises(SonareError) as excinfo:
            scale_quantize_midi(0, C_MAJOR_MASK, 60.4, scale_reference_midi=127.5)
        assert "reference_midi" in str(excinfo.value)

    @pytest.mark.parametrize("value", [math.inf, -math.inf])
    def test_the_c_abi_refuses_an_infinite_reference(self, value: float) -> None:
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
    def test_an_absent_reference_is_a4(self, midi: float) -> None:
        assert scale_quantize_midi(0, C_MAJOR_MASK, midi) == scale_quantize_midi(
            0, C_MAJOR_MASK, midi, scale_reference_midi=None
        )
        assert scale_quantize_midi(0, C_MAJOR_MASK, midi) == (
            scale_quantize_midi(0, C_MAJOR_MASK, midi, scale_reference_midi=DEFAULT_REFERENCE_MIDI)
        )

    @pytest.mark.parametrize("fn", [scale_quantize_midi, scale_correction_semitones])
    def test_an_explicit_zero_reference_is_accepted(self, fn: Callable[..., float]) -> None:
        # An integer anchor is the default grid whatever its value, so 0 lands on A4's.
        assert fn(0, C_MAJOR_MASK, 60.4, scale_reference_midi=0.0) == fn(0, C_MAJOR_MASK, 60.4)

    def test_the_c_abi_reads_nan_as_a4_and_zero_literally(self) -> None:
        from libsonare._runtime import _get_lib

        def quantize(reference: float) -> float:
            out = ctypes.c_float(0.0)
            rc = _get_lib().sonare_scale_quantize_midi(
                ctypes.c_int(0),
                ctypes.c_uint16(C_MAJOR_MASK),
                ctypes.c_float(reference),
                ctypes.c_float(60.4),
                ctypes.byref(out),
            )
            assert rc == 0
            return float(out.value)

        assert quantize(math.nan) == quantize(DEFAULT_REFERENCE_MIDI)
        # A fractional anchor near 0 separates "0 read literally" from a 0 promoted to A4.
        assert quantize(0.5) != quantize(DEFAULT_REFERENCE_MIDI)
        assert quantize(0.5) == quantize(DEFAULT_REFERENCE_MIDI + 0.5)

    def test_a_detuned_reference_moves_the_grid(self) -> None:
        # The anchor only shifts the grid by its fractional part, so the control
        # against "0 is applied as 0" has to be a fractional reference: an
        # integer one is the same grid as the default whatever its value.
        detuned = scale_quantize_midi(0, C_MAJOR_MASK, 60.4, scale_reference_midi=69.5)
        assert detuned != scale_quantize_midi(0, C_MAJOR_MASK, 60.4)


def _features() -> np.ndarray:
    return np.random.default_rng(3).standard_normal((6, 40)).astype(np.float32)


SEGMENT_NEIGHBOURS = {
    "segment_recurrence_matrix": lambda **kw: libsonare.segment_recurrence_matrix(
        _features(), **kw
    ),
    "segment_cross_similarity": lambda **kw: libsonare.segment_cross_similarity(
        _features(), _features()[:, ::-1].copy(), **kw
    ),
}


class TestSegmentNeighbourCount:
    """The recurrence ``k``: None derives it from the frame count; 0 is not a count."""

    @pytest.mark.parametrize("name", sorted(SEGMENT_NEIGHBOURS))
    def test_an_absent_k_is_the_derived_count(self, name: str) -> None:
        call = SEGMENT_NEIGHBOURS[name]
        assert np.array_equal(call(), call(k=None))
        assert not np.array_equal(call(), call(k=2))

    @pytest.mark.parametrize("name", sorted(SEGMENT_NEIGHBOURS))
    @pytest.mark.parametrize("value", [0, -1])
    def test_a_count_below_one_is_refused(self, name: str, value: int) -> None:
        with pytest.raises(SonareValueError, match=r"k must be an integer in \[1, 2147483647\]"):
            SEGMENT_NEIGHBOURS[name](k=value)


class TestPathEnhanceMinRatio:
    """``min_ratio``: None is ``1 / max_ratio``; 0 is not a ratio."""

    @staticmethod
    def _enhance(**kw: object) -> np.ndarray:
        rec = libsonare.segment_recurrence_matrix(_features(), mode="affinity")
        return libsonare.segment_path_enhance(rec, 5, **kw)

    def test_an_absent_min_ratio_is_the_reciprocal_of_max_ratio(self) -> None:
        assert np.array_equal(self._enhance(), self._enhance(min_ratio=None))
        assert not np.array_equal(self._enhance(), self._enhance(min_ratio=1))

    @pytest.mark.parametrize("value", [0, -1])
    def test_a_ratio_below_one_is_refused(self, value: int) -> None:
        with pytest.raises(SonareValueError, match="min_ratio must be greater than 0"):
            self._enhance(min_ratio=value)
