"""A measurement that was not taken comes back as ``None``, never as a stand-in value.

Each case drives a real input that leaves the quantity unmeasured and checks the
field is ``None`` -- and, beside it, that an input which does measure it still
returns a number, so the mapping cannot have turned every value into ``None``.
"""

from __future__ import annotations

import ctypes
import math
from dataclasses import fields

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SR = 22050


def _tone(seconds: float, hz: float = 220.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    return (0.5 * np.sin(2.0 * np.pi * hz * t)).astype(np.float32)


def _reverberant_noise() -> np.ndarray:
    rng = np.random.default_rng(1)
    tail = np.exp(-np.arange(SR) / SR * 8.0) * rng.standard_normal(SR)
    dry = rng.standard_normal(SR * 2).astype(np.float32)
    return np.convolve(dry, tail)[: SR * 3].astype(np.float32)


def test_blind_acoustics_leave_the_direct_sound_measures_absent() -> None:
    result = libsonare.detect_acoustic(_reverberant_noise(), sample_rate=SR)
    assert result.is_blind
    assert (result.edt, result.c50, result.c80, result.d50) == (None, None, None, None)
    assert (result.edt_bands, result.c50_bands, result.c80_bands) == (None, None, None)
    assert result.rt60 > 0.0
    assert isinstance(result.rt60_bands, np.ndarray) and result.rt60_bands.size > 0


def test_a_silent_recording_leaves_the_reverberation_time_absent_and_the_bands_full_length() -> (
    None
):
    result = libsonare.detect_acoustic(np.zeros(SR, dtype=np.float32), sample_rate=SR)
    assert result.rt60 is None
    assert result.rt60_bands.size > 0
    assert np.isnan(result.rt60_bands).all()


def test_an_impulse_response_measures_the_direct_sound_quantities() -> None:
    rng = np.random.default_rng(3)
    ir = (np.exp(-np.arange(SR) / SR * 8.0) * rng.standard_normal(SR) * 0.3).astype(np.float32)
    ir[0] = 1.0
    result = libsonare.analyze_impulse_response(ir, SR)
    assert not result.is_blind
    assert all(isinstance(v, float) for v in (result.edt, result.c50, result.c80, result.d50))
    assert isinstance(result.edt_bands, np.ndarray)


def test_an_unvoiced_melody_point_has_no_frequency() -> None:
    signal = np.concatenate([_tone(1.0), np.zeros(SR, dtype=np.float32)])
    points = libsonare.analyze_melody(signal, SR).points
    frequencies = [point.frequency_hz for point in points]
    assert any(f is None for f in frequencies)
    voiced = [f for f in frequencies if f is not None]
    assert voiced and all(f == pytest.approx(220.0, rel=0.02) for f in voiced[:5])


def test_silence_has_no_novelty_peak() -> None:
    result = libsonare.detect_boundaries(np.zeros(SR * 4, dtype=np.float32), sample_rate=SR)
    assert result.novelty_peak is None


def test_clipping_without_a_flat_run_has_no_flat_level() -> None:
    clean = libsonare.mastering_repair_detect_clipping(_tone(0.5) * 0.5, SR)
    assert clean.flat_run_count == 0
    assert clean.flat_level is None


def test_a_meter_that_has_seen_no_audio_has_zero_channels() -> None:
    from libsonare._ffi_types_analysis import SonareMixMeterSnapshot
    from libsonare._runtime import _mix_meter_from_c

    raw = SonareMixMeterSnapshot()
    snapshot = _mix_meter_from_c(raw)
    assert snapshot.num_channels == 0
    assert snapshot.peak_db == snapshot.rms_db == snapshot.true_peak_dbtp == ()


def test_mixer_meter_requires_its_channel_count_and_planes() -> None:
    from dataclasses import MISSING

    by_name = {f.name: f for f in fields(libsonare.MixMeterSnapshot)}
    for name in ("num_channels", "peak_db", "rms_db", "true_peak_dbtp"):
        assert by_name[name].default is MISSING, name
        assert by_name[name].default_factory is MISSING, name


@pytest.mark.parametrize("num_channels", [1, 2, 6])
def test_mixer_meter_arrays_are_channel_sized_and_convert_nan_to_none(num_channels: int) -> None:
    from libsonare._ffi_types_analysis import SonareMixMeterSnapshot
    from libsonare._runtime import _mix_meter_from_c

    raw = SonareMixMeterSnapshot()
    raw.num_channels = num_channels
    for name in ("peak_db", "rms_db", "true_peak_dbtp"):
        plane = getattr(raw, name)
        for index in range(len(plane)):
            plane[index] = math.nan
        plane[0] = -math.inf
        if num_channels > 1:
            plane[1] = math.inf
        if num_channels > 2:
            plane[num_channels - 1] = -3.5
    unmeasured_scalars = (
        "correlation",
        "mono_compat_width",
        "momentary_lufs",
        "short_term_lufs",
        "integrated_lufs",
        "max_true_peak_dbtp",
    )
    for name in unmeasured_scalars:
        setattr(raw, name, ctypes.c_float(math.nan))

    snapshot = _mix_meter_from_c(raw)
    for name in unmeasured_scalars:
        assert getattr(snapshot, name) is None
    assert snapshot.num_channels == num_channels
    assert (
        len(snapshot.peak_db)
        == len(snapshot.rms_db)
        == len(snapshot.true_peak_dbtp)
        == num_channels
    )
    for name in ("peak_db", "rms_db", "true_peak_dbtp"):
        values = getattr(snapshot, name)
        assert values[0] == -math.inf
        if num_channels > 1:
            assert values[1] == math.inf
        if num_channels > 2:
            assert values[-1] == -3.5
            assert all(value is None for value in values[2:-1])
        assert all(value is None or not math.isnan(value) for value in values)
    for field in fields(snapshot):
        value = getattr(snapshot, field.name)
        if isinstance(value, float):
            assert not math.isnan(value)
        elif isinstance(value, tuple):
            assert all(item is None or not math.isnan(item) for item in value)
    for name in (
        "peak_db_l",
        "peak_db_r",
        "rms_db_l",
        "rms_db_r",
        "true_peak_dbtp_l",
        "true_peak_dbtp_r",
    ):
        assert not hasattr(snapshot, name)


def test_a_refused_inharmonicity_fit_is_nan_and_a_fitted_one_is_not() -> None:
    # A plain sine has no partial series to fit a stretch to, so the fit is refused.
    with libsonare.PolyphonicAnalysis.analyze(_tone(1.0), SR, estimate_inharmonicity=True) as sine:
        refused = sine.note_inharmonicity()
    assert refused.size == 1 and np.isnan(refused[0])
    t = np.arange(SR) / SR
    harmonic = sum(0.3 / h * np.sin(2.0 * np.pi * 220.0 * h * t) for h in range(1, 13))
    rich = np.asarray(harmonic, dtype=np.float32)
    with libsonare.PolyphonicAnalysis.analyze(rich, SR, estimate_inharmonicity=True) as fit:
        fitted = fit.note_inharmonicity()
    # 0 is a fitted result (the harmonic series), not the refusal.
    assert fitted.size == 1 and fitted[0] == pytest.approx(0.0, abs=1e-5)


def test_a_project_diagnostic_about_no_single_target_has_no_target_id() -> None:
    from libsonare._ffi_types_mastering_project import (
        SonareProjectCompileResult,
        SonareProjectDiagnostic,
    )
    from libsonare._project_render import _compile_diagnostics

    rows = (SonareProjectDiagnostic * 2)()
    rows[0].target_id = 0
    rows[1].target_id = 7
    result = SonareProjectCompileResult()
    result.diagnostics = rows
    result.diagnostic_count = 2
    diagnostics = _compile_diagnostics(result)
    assert [d.target_id for d in diagnostics] == [None, 7]


def test_a_pattern_position_with_no_voted_chord_is_none() -> None:
    from libsonare._ffi_types_streaming import SonareStreamBarChord
    from libsonare.streaming import _voted_pattern

    rows = (SonareStreamBarChord * 2)()
    rows[0].bar_index, rows[0].root, rows[0].quality = 0, -1, 0
    rows[1].bar_index, rows[1].root, rows[1].quality = 1, 9, 0
    pattern = _voted_pattern(rows, 2)
    assert pattern[0] is None
    assert pattern[1] is not None and int(pattern[1].root) == 9


def test_silent_loudness_reads_are_minus_infinity_and_its_gain_is_absent() -> None:
    silence = np.zeros(SR, dtype=np.float32)
    gain = libsonare.streaming_loudness_gain(silence, SR)
    assert (gain.integrated_lufs, gain.true_peak_dbtp) == (-math.inf, -math.inf)
    loud = libsonare.streaming_loudness_gain(_tone(1.0), SR)
    assert isinstance(loud.integrated_lufs, float) and isinstance(loud.true_peak_dbtp, float)
    assert math.isfinite(loud.integrated_lufs) and math.isfinite(loud.true_peak_dbtp)
