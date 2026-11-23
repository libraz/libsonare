"""A level of silence is an infinite end; a measurement not taken is ``None``.

Each reader restores the end ``schemas/result-null-means.json`` names for a null,
and reads an absent key as ``None``. Every case beside a silent input checks that a
sounding one still reads a finite number, so a reader cannot pass by turning every
value into an infinity.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare
from libsonare import SonareError
from libsonare._mastering_pair import _audio_profile_from_wire

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SR = 48000
SILENCE = np.zeros(SR, dtype=np.float32)


def _music() -> np.ndarray:
    t = np.arange(SR) / SR
    return (0.12 * np.sin(2 * np.pi * 220 * t) + 0.08 * np.sin(2 * np.pi * 1760 * t)).astype(
        np.float32
    )


def test_a_silent_profile_reads_minus_infinity_and_leaves_the_ratios_unmeasured() -> None:
    loudness = libsonare.mastering_audio_profile(SILENCE, SR).loudness
    assert loudness.integrated_lufs == -math.inf
    assert loudness.true_peak_dbtp == -math.inf
    assert (loudness.loudness_range_lu, loudness.crest_factor_db) == (None, None)
    spectral = libsonare.mastering_audio_profile(SILENCE, SR).spectral
    assert spectral.mid_rms_db == -math.inf
    sounding = libsonare.mastering_audio_profile(_music(), SR).loudness
    assert math.isfinite(sounding.integrated_lufs) and math.isfinite(sounding.true_peak_dbtp)
    assert sounding.crest_factor_db is not None


def test_a_silent_assistant_profile_reads_minus_infinity() -> None:
    profile = libsonare.mastering_assistant_suggest(SILENCE, SR).profile
    assert (profile.integrated_lufs, profile.true_peak_dbtp) == (-math.inf, -math.inf)
    assert (profile.loudness_range_lu, profile.crest_factor_db) == (None, None)
    sounding = libsonare.mastering_assistant_suggest(_music(), SR).profile
    assert math.isfinite(sounding.integrated_lufs)


def test_a_silent_streaming_preview_has_no_normalization_gain() -> None:
    for platform in libsonare.mastering_streaming_preview(SILENCE, SR).platforms:
        assert (platform.integrated_lufs, platform.true_peak_dbtp) == (-math.inf, -math.inf)
        assert platform.normalization_gain_db is None
    for platform in libsonare.mastering_streaming_preview(_music(), SR).platforms:
        assert platform.normalization_gain_db is not None


def test_a_silent_side_leaves_no_gain_to_match() -> None:
    music = _music()
    result = libsonare.mastering_analyze_pair("match.referenceLoudness", SILENCE, music, SR)
    assert result.source_lufs == -math.inf and math.isfinite(result.reference_lufs)
    assert result.gain_to_match_db is None
    matched = libsonare.mastering_analyze_pair("match.referenceLoudness", music, music, SR)
    assert matched.gain_to_match_db is not None


def test_an_empty_reference_band_leaves_no_deviation() -> None:
    music = _music()
    quiet_source = libsonare.mastering_analyze_pair("match.tonalBalance", SILENCE, music, SR)
    assert all(band.source_db == -math.inf for band in quiet_source.bands)
    assert all(band.deviation_db == -math.inf for band in quiet_source.bands)
    quiet_reference = libsonare.mastering_analyze_pair("match.tonalBalance", music, SILENCE, SR)
    assert all(band.reference_db == -math.inf for band in quiet_reference.bands)
    assert all(band.deviation_db is None for band in quiet_reference.bands)


def test_an_anti_phase_pair_is_infinitely_wide_and_a_silent_pair_unmeasured() -> None:
    music = _music()
    wide = libsonare.mastering_analyze_stereo("stereo.monoCompatCheck", music, -music, SR)
    assert wide.width == math.inf
    silent = libsonare.mastering_analyze_stereo("stereo.monoCompatCheck", SILENCE, SILENCE, SR)
    assert (silent.correlation, silent.width) == (None, None)
    narrow = libsonare.mastering_analyze_stereo("stereo.monoCompatCheck", music, music, SR)
    assert narrow.width is not None and math.isfinite(narrow.width)


def test_silent_analysis_dynamics_read_minus_infinity() -> None:
    dynamics = libsonare.analyze(SILENCE, SR).dynamics
    assert (dynamics.peak_db, dynamics.rms_db) == (-math.inf, -math.inf)
    assert (dynamics.dynamic_range_db, dynamics.crest_factor, dynamics.loudness_range_lu) == (
        None,
        None,
        None,
    )
    sounding = libsonare.analyze(_music(), SR).dynamics
    assert math.isfinite(sounding.peak_db)


def test_a_silent_repair_analysis_reads_minus_infinity_floors() -> None:
    analysis = libsonare.mastering_repair_analyze([SILENCE], SR)
    assert analysis.integrated_lufs == -math.inf
    assert analysis.defects is not None
    noise = analysis.defects.noise
    assert noise.floor_dbfs == -math.inf
    assert np.all(noise.band_floor_dbfs == -math.inf)
    assert all(level == -math.inf for level in analysis.defects.hum.harmonic_dbfs)


def test_a_noise_floor_lists_only_the_bands_a_bin_lands_in() -> None:
    bins = libsonare.mastering_repair_noise_band_bins(SR, n_fft=1024)
    detected = libsonare.mastering_repair_detect_noise_floor(_music(), SR)
    assert detected.band_floor_band.dtype == np.int32
    assert detected.band_floor_dbfs.shape == detected.band_floor_band.shape
    assert np.all(np.diff(detected.band_floor_band) > 0)
    empty = {band for band in range(32) if bins[band] == bins[band + 1]}
    assert empty and not empty & set(detected.band_floor_band.tolist())
    assert len(detected.band_floor_band) == 32 - len(empty)


def test_a_null_at_a_path_the_table_does_not_name_is_refused() -> None:
    wire = {
        "durationSec": 1,
        "bpm": None,
        "bpmConfidence": 0,
        "loudness": {"integratedLufs": None, "truePeakDbtp": None},
        "spectral": {
            **{f"{band}RmsDb": None for band in ("sub", "low", "lowMid", "mid", "highMid")},
            **{f"{band}RmsDb": None for band in ("high", "air")},
            "centroidHz": 0,
            "flatness": 1,
            "rolloffHz": 0,
        },
        "dynamics": {"attackPerSecond": 0, "shortTermLufsStd": 0, "sustainRatio": 0},
    }
    named = {**wire, "bpm": 120}
    assert _audio_profile_from_wire(dict(named)).spectral.air_rms_db == -math.inf
    with pytest.raises(SonareError, match="bpm is null, which names no infinite end"):
        _audio_profile_from_wire(wire)


def test_a_measurement_is_absent_or_present_and_a_left_null_is_refused() -> None:
    from libsonare._mastering_pair import MasteringAudioProfileDynamics
    from libsonare._result_records import _measured_float, _record

    assert _measured_float({}, "width") is None
    assert _measured_float({"width": 1}, "width") == 1.0
    with pytest.raises(SonareError, match="width is null, which names no infinite end") as refused:
        _measured_float({"width": None}, "width")
    assert refused.value.code == int(libsonare.ErrorCode.INVALID_FORMAT)
    fields = {"attackPerSecond": 0, "shortTermLufsStd": 0, "sustainRatio": 0}
    assert _record(MasteringAudioProfileDynamics, fields).sustain_ratio == 0.0
    with pytest.raises(SonareError, match="sustainRatio is null"):
        _record(MasteringAudioProfileDynamics, {**fields, "sustainRatio": None})
