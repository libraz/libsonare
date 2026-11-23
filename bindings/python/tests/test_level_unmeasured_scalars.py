"""A level the core did not measure is ``None``; silence stays an infinity.

Each case drives an input that leaves a scalar unmeasured and checks it is
``None``, and beside it an input that measures it still reads a finite number,
so a reader cannot pass by turning every value into ``None`` or into an infinity.
"""

from __future__ import annotations

import ctypes
import math

import numpy as np
import pytest

import libsonare
from libsonare._engine_conversions import _meter_telemetry_from_c, _scope_telemetry_from_c
from libsonare._ffi_types_core import SonareMeterTelemetryRecord, SonareScopeTelemetryRecord

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SR = 48000
SILENCE = np.zeros(SR, dtype=np.float32)


def _tone(seconds: float = 1.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    return (0.1 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)


def test_dynamics_result_reads_silence_as_minus_infinity_and_unmeasured_ratios_as_none() -> None:
    silent = libsonare.analyze_dynamics(SILENCE, sample_rate=SR)
    assert (silent.peak_db, silent.rms_db) == (-math.inf, -math.inf)
    assert (silent.dynamic_range_db, silent.crest_factor, silent.loudness_range_lu) == (
        None,
        None,
        None,
    )
    assert np.all(silent.loudness_rms_db == -math.inf)
    sounding = libsonare.analyze_dynamics(_tone(), sample_rate=SR)
    assert sounding.crest_factor is not None and math.isfinite(sounding.crest_factor)
    assert sounding.dynamic_range_db is not None and math.isfinite(sounding.dynamic_range_db)


def test_lufs_reads_an_unfilled_window_as_none_and_zero_energy_as_minus_infinity() -> None:
    one_second = libsonare.lufs(_tone(), SR)
    assert math.isfinite(one_second.integrated_lufs)
    assert one_second.momentary_lufs is not None and math.isfinite(one_second.momentary_lufs)
    assert one_second.short_term_lufs is None
    assert one_second.max_short_term_lufs is None
    assert one_second.loudness_range_lu is None

    silent = libsonare.lufs(SILENCE, SR)
    assert silent.integrated_lufs == -math.inf
    assert silent.momentary_lufs == -math.inf
    assert silent.max_momentary_lufs == -math.inf
    assert silent.short_term_lufs is None


def test_ebur128_loudness_range_is_none_without_a_gated_window() -> None:
    assert libsonare.ebur128_loudness_range_lu(SILENCE, SR) is None
    long_range = libsonare.ebur128_loudness_range_lu(_tone(5.0), SR)
    assert long_range is not None and math.isfinite(long_range)


def test_metering_dynamic_range_reads_a_silent_low_end_as_infinite_and_silence_as_none() -> None:
    gapped = np.concatenate([_tone(2.0), np.zeros(SR * 2, dtype=np.float32)])
    report = libsonare.metering_dynamic_range(gapped, SR, window_sec=0.4, hop_sec=0.2)
    assert report.low_percentile_db == -math.inf
    assert report.high_percentile_db is not None and math.isfinite(report.high_percentile_db)
    assert report.dynamic_range_db == math.inf

    silent = libsonare.metering_dynamic_range(np.zeros(SR * 2, dtype=np.float32), SR)
    assert silent.dynamic_range_db is None
    assert silent.low_percentile_db == silent.high_percentile_db == -math.inf

    sounding = libsonare.metering_dynamic_range(_tone(2.0), SR)
    assert sounding.dynamic_range_db is not None and math.isfinite(sounding.dynamic_range_db)


def test_metering_crest_factor_is_none_for_silence() -> None:
    assert libsonare.metering_crest_factor_db(SILENCE, SR) is None
    assert libsonare.metering_crest_factor_db_stereo(SILENCE, SILENCE, SR) is None
    tone = _tone()
    assert math.isfinite(libsonare.metering_crest_factor_db(tone, SR))
    assert math.isfinite(libsonare.metering_crest_factor_db_stereo(tone, tone, SR))


def test_stereo_correlation_and_width_are_none_without_energy_and_infinite_for_anti_phase() -> None:
    tone = _tone()
    assert libsonare.metering_stereo_correlation(SILENCE, SILENCE, SR) is None
    assert libsonare.metering_stereo_correlation(tone, SILENCE, SR) is None
    assert libsonare.metering_stereo_width(SILENCE, SILENCE, SR) is None
    assert libsonare.metering_stereo_width(tone, -tone, SR) == math.inf
    assert libsonare.metering_stereo_correlation(tone, tone, SR) == pytest.approx(1.0)
    assert libsonare.metering_stereo_width(tone, tone, SR) == pytest.approx(0.0)


def test_mix_meter_snapshot_reads_silence_as_minus_infinity_and_unmeasured_as_none() -> None:
    silent = libsonare.mix_stereo([(SILENCE[:1024], SILENCE[:1024])], sample_rate=SR).meters[0]
    assert silent.num_channels == 2
    assert silent.peak_db == (-math.inf, -math.inf)
    assert silent.rms_db == (-math.inf, -math.inf)
    assert silent.true_peak_dbtp == (-math.inf, -math.inf)
    assert silent.max_true_peak_dbtp == -math.inf
    assert (silent.correlation, silent.mono_compat_width) == (None, None)
    assert (silent.momentary_lufs, silent.short_term_lufs, silent.integrated_lufs) == (None,) * 3

    tone = _tone()[:1024]
    sounding = libsonare.mix_stereo([(tone, tone)], sample_rate=SR).meters[0]
    assert sounding.correlation == pytest.approx(1.0)
    assert sounding.mono_compat_width == pytest.approx(0.0)
    assert math.isfinite(sounding.peak_db[0])


def test_mastering_report_reads_a_short_take_as_unmeasured_and_silence_as_minus_infinity() -> None:
    config = {"loudness": {"targetLufs": -14.0}}
    short = libsonare.master_audio(_tone(), SR, overrides=config)
    assert short.report is not None
    assert short.report.after.max_short_term_lufs is None
    assert short.report.after.loudness_range_lu is None
    assert short.output_loudness_range_lu is None
    assert math.isfinite(short.report.after.max_momentary_lufs)

    silent = libsonare.master_audio(SILENCE, SR, overrides=config)
    assert silent.report is not None
    assert silent.report.before.max_momentary_lufs == -math.inf
    assert silent.report.before.true_peak_dbtp == -math.inf
    assert silent.output_true_peak_dbtp == -math.inf


def test_chain_loudness_gain_is_none_when_the_stage_is_off_or_the_input_is_silent() -> None:
    enabled = {"loudness": {"enabled": True, "targetLufs": -14.0}}
    disabled = {"loudness": {"enabled": False}}
    tone = _tone(2.0)
    for run in (
        lambda x, cfg: libsonare.master_audio(x, SR, overrides=cfg),
        lambda x, cfg: libsonare.master_audio_stereo(x, x, SR, overrides=cfg),
    ):
        applied = run(tone, enabled)
        assert applied.report is not None
        assert applied.loudness_gain_db is not None
        assert math.isfinite(applied.loudness_gain_db)
        assert applied.loudness_gain_db != 0.0
        assert applied.report.loudness_gain_db == applied.loudness_gain_db
        for result in (run(tone, disabled), run(SILENCE, enabled)):
            assert result.report is not None
            assert result.loudness_gain_db is None
            assert result.report.loudness_gain_db is None


def test_meter_telemetry_reads_nan_scalars_as_none_and_keeps_plane_arrays() -> None:
    raw = SonareMeterTelemetryRecord()
    raw.num_channels = 2
    nan = math.nan
    # The struct's own setters refuse a non-finite value, so write the bytes the core would.
    for field, value in (
        ("max_true_peak_dbtp", nan),
        ("correlation", nan),
        ("mono_compat_width", nan),
        ("momentary_lufs", nan),
        ("short_term_lufs", nan),
        ("integrated_lufs", -math.inf),
    ):
        offset = getattr(SonareMeterTelemetryRecord, field).offset
        ctypes.c_float.from_buffer(raw, offset).value = value
    raw.peak_db[0] = -math.inf
    raw.peak_db[1] = nan
    record = _meter_telemetry_from_c(raw)
    assert (record.max_true_peak_dbtp, record.correlation, record.mono_compat_width) == (None,) * 3
    assert (record.momentary_lufs, record.short_term_lufs) == (None, None)
    assert record.integrated_lufs == -math.inf
    assert record.peak_db[0] == -math.inf
    assert math.isnan(record.peak_db[1])


def test_scope_telemetry_bands_keep_minus_infinity_and_nan() -> None:
    raw = SonareScopeTelemetryRecord()
    raw.band_count = 3
    raw.bands[0] = -math.inf
    raw.bands[1] = -40.0
    raw.bands[2] = math.nan
    bands = _scope_telemetry_from_c(raw).bands
    assert bands[0] == -math.inf
    assert bands[1] == pytest.approx(-40.0)
    assert math.isnan(bands[2])
