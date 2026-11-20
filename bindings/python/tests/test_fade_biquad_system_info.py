"""Tests for fade, biquad_filter and system_info, the facades over three C entries."""

from __future__ import annotations

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


class TestFade:
    def test_zero_lengths_are_the_identity(self) -> None:
        tone = sine(440.0, 0.25, SR)
        np.testing.assert_array_equal(libsonare.fade(tone, SR), tone)

    def test_fade_in_ramp_starts_at_zero_and_leaves_the_tail(self) -> None:
        ones = np.ones(SR // 2, dtype=np.float32)
        out = libsonare.fade(ones, SR, fade_in_sec=0.1)
        assert out[0] == 0.0
        assert 0.0 < out[SR // 20] < 1.0
        np.testing.assert_array_equal(out[SR // 5 :], ones[SR // 5 :])

    def test_fade_out_ramp_ends_at_zero(self) -> None:
        ones = np.ones(SR // 2, dtype=np.float32)
        out = libsonare.fade(ones, SR, fade_out_sec=0.1)
        assert abs(out[-1]) < 1e-3
        assert out[0] == 1.0

    def test_a_negative_length_is_refused_by_name(self) -> None:
        with pytest.raises(
            SonareValueError, match=r"^fade: fade_in_sec must be finite and at least 0$"
        ):
            libsonare.fade(sine(440.0, 0.1, SR), SR, fade_in_sec=-1.0)


class TestBiquadFilter:
    def test_high_pass_removes_dc(self) -> None:
        dc = np.full(SR, 0.5, dtype=np.float32)
        out = libsonare.biquad_filter(dc, SR, type="highPass", cutoff_hz=100.0)
        assert abs(float(np.mean(out[SR // 2 :]))) < 1e-3

    def test_band_pass_reads_center_and_bandwidth(self) -> None:
        tone = sine(2000.0, 0.25, SR)
        out = libsonare.biquad_filter(
            tone, SR, type="bandPass", center_hz=2000.0, bandwidth_hz=500.0, zero_phase=True
        )
        assert out.shape == tone.shape
        assert out.dtype == np.float32

    def test_band_pass_without_center_is_refused(self) -> None:
        with pytest.raises(
            SonareValueError,
            match=r"^biquad_filter: center_hz must be finite and in \(0, 11025\)$",
        ):
            libsonare.biquad_filter(sine(440.0, 0.1, SR), SR, type="bandPass", bandwidth_hz=100.0)

    def test_a_cutoff_on_a_band_pass_is_refused_with_the_c_text(self) -> None:
        with pytest.raises(
            SonareValueError,
            match=(r"^biquad_filter: cutoff_hz must be 0 when type is band-pass or notch$"),
        ):
            libsonare.biquad_filter(
                sine(440.0, 0.1, SR),
                SR,
                type="bandPass",
                cutoff_hz=1000.0,
                center_hz=2000.0,
                bandwidth_hz=100.0,
            )

    def test_an_unknown_type_names_the_tokens(self) -> None:
        with pytest.raises(
            SonareValueError,
            match=r"^biquad_filter: type must be one of highPass, lowPass, bandPass, notch$",
        ):
            libsonare.biquad_filter(sine(440.0, 0.1, SR), SR, type="peak", cutoff_hz=100.0)  # type: ignore[arg-type]


class TestSystemInfo:
    def test_reports_the_three_sections(self) -> None:
        info = libsonare.system_info()
        assert isinstance(info, dict)
        assert {"cpu", "memory", "parallel"} <= set(info)
        assert isinstance(info["parallel"]["enabled"], bool)
