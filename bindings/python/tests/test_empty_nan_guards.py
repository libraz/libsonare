"""Empty / NaN-Inf input guards for metering / dynamics / voice-changer wrappers.

Source-side helpers (`_validate_samples` in `_runtime.py`) raise typed
`ValueError`s with `{fn_name}: {arg_name}` prefixes so callers can pattern-match
on the wrapper rather than on a generic C error string. These tests pin the
behavior across all in-scope public wrappers.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import (
    ebur128_loudness_range,
    harmonic,
    lufs,
    lufs_interleaved,
    mastering_dynamics_compressor,
    mastering_dynamics_gate,
    mastering_dynamics_transient_shaper,
    metering_detect_clipping,
    metering_dynamic_range,
    metering_peak_db,
    metering_rms_db,
    metering_spectrum,
    metering_spectrum_frame,
    metering_stereo_correlation,
    metering_true_peak_db,
    momentary_lufs,
    normalize,
    percussive,
    phase_vocoder,
    pitch_pyin,
    pitch_shift,
    pitch_yin,
    short_term_lufs,
    time_stretch,
    waveform_peak_pyramid,
    waveform_peaks,
)
from libsonare._effects import voice_change, voice_change_realtime
from libsonare._runtime import SonareError

SR = 22050

# Every public wrapper that routes its primary buffer through
# ``_validate_samples`` (arg ``samples``). Keeping this list aligned with the
# Node/WASM guard suites ensures a Python-only regression in the finite/empty
# preflight fails a test rather than propagating NaN downstream. Each entry maps
# a wrapper name to an invoker taking the validated buffer as its first argument.
_SAMPLE_WRAPPERS = {
    "lufs": lambda buf: lufs(buf, SR),
    "momentary_lufs": lambda buf: momentary_lufs(buf, SR),
    "short_term_lufs": lambda buf: short_term_lufs(buf, SR),
    "lufs_interleaved": lambda buf: lufs_interleaved(buf, 1, SR),
    "ebur128_loudness_range": lambda buf: ebur128_loudness_range(buf, SR),
    "metering_true_peak_db": lambda buf: metering_true_peak_db(buf, SR),
    "metering_detect_clipping": lambda buf: metering_detect_clipping(buf, SR),
    "metering_dynamic_range": lambda buf: metering_dynamic_range(buf, SR),
    "metering_spectrum": lambda buf: metering_spectrum(buf, SR),
    "metering_spectrum_frame": lambda buf: metering_spectrum_frame(buf, SR),
    "waveform_peaks": lambda buf: waveform_peaks(buf, 1),
    "waveform_peak_pyramid": lambda buf: waveform_peak_pyramid(buf, 1),
    "harmonic": lambda buf: harmonic(buf, SR),
    "percussive": lambda buf: percussive(buf, SR),
    "normalize": lambda buf: normalize(buf, SR),
    "time_stretch": lambda buf: time_stretch(buf, SR),
    "pitch_shift": lambda buf: pitch_shift(buf, SR),
    "phase_vocoder": lambda buf: phase_vocoder(buf, SR),
    "pitch_pyin": lambda buf: pitch_pyin(buf, SR),
    "pitch_yin": lambda buf: pitch_yin(buf, SR),
    "voice_change": lambda buf: voice_change(buf),
    "voice_change_realtime": lambda buf: voice_change_realtime(buf),
    "mastering_dynamics_compressor": lambda buf: mastering_dynamics_compressor(buf, SR),
    "mastering_dynamics_gate": lambda buf: mastering_dynamics_gate(buf, SR),
    "mastering_dynamics_transient_shaper": lambda buf: mastering_dynamics_transient_shaper(buf, SR),
}


def _sine(n: int = 1024, freq: float = 440.0, amp: float = 0.5) -> np.ndarray:
    t = np.arange(n, dtype=np.float32) / SR
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _with_nan(n: int = 1024, index: int = 100) -> np.ndarray:
    buf = _sine(n)
    buf[index] = math.nan
    return buf


def _with_inf(n: int = 1024, index: int = 200) -> np.ndarray:
    buf = _sine(n)
    buf[index] = math.inf
    return buf


def _with_neg_inf(n: int = 1024, index: int = 321) -> np.ndarray:
    buf = _sine(n)
    buf[index] = -math.inf
    return buf


class TestEmptyGuards:
    def test_lufs_rejects_empty(self):
        with pytest.raises(ValueError, match=r"lufs: samples must not be empty"):
            lufs(np.empty(0, dtype=np.float32), SR)

    def test_metering_peak_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_peak_db: samples must not be empty"):
            metering_peak_db(np.empty(0, dtype=np.float32))

    def test_metering_rms_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_rms_db: samples must not be empty"):
            metering_rms_db(np.empty(0, dtype=np.float32))

    def test_metering_true_peak_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_true_peak_db: samples must not be empty"):
            metering_true_peak_db(np.empty(0, dtype=np.float32), SR)

    def test_metering_stereo_correlation_rejects_empty_left(self):
        with pytest.raises(
            ValueError,
            match=r"metering_stereo_correlation: left must not be empty",
        ):
            metering_stereo_correlation(np.empty(0, dtype=np.float32), _sine())

    def test_mastering_dynamics_compressor_rejects_empty(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_compressor: samples must not be empty",
        ):
            mastering_dynamics_compressor(np.empty(0, dtype=np.float32), SR)

    def test_mastering_dynamics_gate_rejects_empty(self):
        with pytest.raises(ValueError, match=r"mastering_dynamics_gate: samples must not be empty"):
            mastering_dynamics_gate(np.empty(0, dtype=np.float32), SR)

    def test_mastering_dynamics_transient_shaper_rejects_empty(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_transient_shaper: samples must not be empty",
        ):
            mastering_dynamics_transient_shaper(np.empty(0, dtype=np.float32), SR)

    def test_voice_change_rejects_empty(self):
        with pytest.raises(ValueError, match=r"voice_change: samples must not be empty"):
            voice_change(np.empty(0, dtype=np.float32))

    def test_voice_change_realtime_rejects_empty(self):
        with pytest.raises(ValueError, match=r"voice_change_realtime: samples must not be empty"):
            voice_change_realtime(np.empty(0, dtype=np.float32))


class TestNanInfGuards:
    def test_lufs_rejects_nan_with_index(self):
        with pytest.raises(ValueError, match=r"lufs: samples contains NaN or Inf at index 100"):
            lufs(_with_nan(), SR)

    def test_lufs_rejects_inf_with_index(self):
        with pytest.raises(ValueError, match=r"lufs: samples contains NaN or Inf at index 200"):
            lufs(_with_inf(), SR)

    def test_mastering_dynamics_compressor_rejects_nan(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_compressor: samples contains NaN or Inf at index 100",
        ):
            mastering_dynamics_compressor(_with_nan(), SR)

    def test_mastering_dynamics_gate_rejects_inf(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_gate: samples contains NaN or Inf at index 200",
        ):
            mastering_dynamics_gate(_with_inf(), SR)

    def test_metering_peak_db_rejects_nan(self):
        with pytest.raises(
            ValueError, match=r"metering_peak_db: samples contains NaN or Inf at index 100"
        ):
            metering_peak_db(_with_nan())


class TestValidateFalseStillHasCAbiBackstop:
    def test_lufs_validate_false(self):
        # validate=False skips only the Python preflight; C-ABI validation is
        # mandatory and must still reject NaN input.
        with pytest.raises(SonareError) as exc_info:
            lufs(_with_nan(), SR, validate=False)
        assert exc_info.value.code == 4

    def test_mastering_dynamics_compressor_validate_false(self):
        with pytest.raises(SonareError) as exc_info:
            mastering_dynamics_compressor(_with_nan(), SR, validate=False)
        assert exc_info.value.code == 4


class TestPitchCAbiValidation:
    @pytest.mark.parametrize(
        "pitch_name, pitch", [("pitch_yin", pitch_yin), ("pitch_pyin", pitch_pyin)]
    )
    def test_rejects_negative_inf_with_index(self, pitch_name, pitch):
        with pytest.raises(
            ValueError,
            match=rf"{pitch_name}: samples contains NaN or Inf at index 321",
        ):
            pitch(_with_neg_inf(), sample_rate=SR)

    @pytest.mark.parametrize("pitch", [pitch_yin, pitch_pyin], ids=["yin", "pyin"])
    def test_invalid_sample_rate_keeps_sonare_error_code(self, pitch):
        with pytest.raises(SonareError) as exc_info:
            pitch(_sine(SR), sample_rate=0)
        assert exc_info.value.code == 4

    @pytest.mark.parametrize("pitch", [pitch_yin, pitch_pyin], ids=["yin", "pyin"])
    def test_normal_audio_returns_pitch_result(self, pitch):
        result = pitch(_sine(SR), sample_rate=SR)
        assert result.n_frames > 0
        assert len(result.f0) == result.n_frames
        assert len(result.voiced_prob) == result.n_frames
        assert len(result.voiced_flag) == result.n_frames


class TestSampleWrapperGuardCoverage:
    """Parametrized empty / NaN / Inf coverage across every ``samples`` wrapper."""

    @pytest.mark.parametrize("name", sorted(_SAMPLE_WRAPPERS))
    def test_rejects_empty(self, name):
        with pytest.raises(ValueError, match=rf"{name}: samples must not be empty"):
            _SAMPLE_WRAPPERS[name](np.empty(0, dtype=np.float32))

    @pytest.mark.parametrize("name", sorted(_SAMPLE_WRAPPERS))
    def test_rejects_nan_with_index(self, name):
        with pytest.raises(ValueError, match=rf"{name}: samples contains NaN or Inf at index 100"):
            _SAMPLE_WRAPPERS[name](_with_nan())

    @pytest.mark.parametrize("name", sorted(_SAMPLE_WRAPPERS))
    def test_rejects_inf_with_index(self, name):
        with pytest.raises(ValueError, match=rf"{name}: samples contains NaN or Inf at index 200"):
            _SAMPLE_WRAPPERS[name](_with_inf())


class TestPositiveSmoke:
    def test_metering_rms_db_finite_on_sine(self):
        v = metering_rms_db(_sine(SR))
        assert math.isfinite(v)

    def test_mastering_dynamics_compressor_returns_same_length(self):
        sig = _sine(SR)
        out, latency = mastering_dynamics_compressor(sig, SR)
        assert len(out) == len(sig)
        assert isinstance(latency, int)
