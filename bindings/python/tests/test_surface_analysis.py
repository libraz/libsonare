"""Public analysis surface: sample-rate spelling, mirror arguments and segmentation names."""

from __future__ import annotations

import inspect

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SR = 22050

# libsonare operations: sample_rate is required, positional-or-keyword.
OPERATIONS = (
    "analyze",
    "analyze_bpm",
    "analyze_dynamics",
    "analyze_impulse_response",
    "analyze_melody",
    "analyze_rhythm",
    "analyze_sections",
    "analyze_timbre",
    "analyze_with_progress",
    "auto_tune",
    "bass_chroma",
    "chord_functional_analysis",
    "chroma",
    "cqt_to_audio",
    "cyclic_tempogram",
    "decompose_stems",
    "decompose_stems_linked",
    "detect_acoustic",
    "detect_beats",
    "detect_boundaries",
    "detect_bpm",
    "detect_chords",
    "detect_downbeats",
    "detect_key",
    "detect_key_candidates",
    "detect_onsets",
    "ebur128_loudness_range",
    "estimate_room",
    "griffin_lim",
    "harmonic",
    "hpss",
    "hpss_with_residual",
    "lufs",
    "lufs_interleaved",
    "lufs_series_interleaved",
    "master_audio",
    "master_audio_stereo",
    "mastering",
    "mastering_ab_match_loudness",
    "mastering_ab_match_loudness_stereo",
    "mastering_assistant_suggest",
    "mastering_assistant_suggest_chain",
    "mastering_assistant_suggest_chain_stereo",
    "mastering_assistant_suggest_stereo",
    "mastering_audio_profile",
    "mastering_audio_profile_stereo",
    "mastering_chain",
    "mastering_chain_stereo",
    "mastering_dynamics_compressor",
    "mastering_dynamics_gate",
    "mastering_dynamics_transient_shaper",
    "mastering_pair_analyze",
    "mastering_pair_process",
    "mastering_pair_process_stereo",
    "mastering_process",
    "mastering_process_stereo",
    "mastering_repair_analyze",
    "mastering_repair_apply",
    "mastering_repair_declick",
    "mastering_repair_declick_stereo",
    "mastering_repair_declip",
    "mastering_repair_declip_stereo",
    "mastering_repair_decrackle",
    "mastering_repair_decrackle_stereo",
    "mastering_repair_dehum",
    "mastering_repair_dehum_stereo",
    "mastering_repair_denoise_classical",
    "mastering_repair_denoise_classical_linked",
    "mastering_repair_denoise_classical_stereo",
    "mastering_repair_dereverb_classical",
    "mastering_repair_dereverb_classical_linked",
    "mastering_repair_dereverb_classical_stereo",
    "mastering_repair_detect_clicks",
    "mastering_repair_detect_clipping",
    "mastering_repair_detect_crackle",
    "mastering_repair_detect_hum",
    "mastering_repair_detect_noise_floor",
    "mastering_repair_detect_reverb",
    "mastering_repair_detect_trim_range",
    "mastering_repair_detect_trim_range_stereo",
    "mastering_repair_noise_band_bins",
    "mastering_repair_trim_silence",
    "mastering_repair_trim_silence_stereo",
    "mastering_stereo_analyze",
    "mastering_streaming_preview",
    "mastering_streaming_preview_stereo",
    "mel_spectrogram",
    "metering_crest_factor_db",
    "metering_crest_factor_db_stereo",
    "metering_dc_offset",
    "metering_detect_clipping",
    "metering_dynamic_range",
    "metering_peak_db",
    "metering_phase_scope",
    "metering_phase_scope_decimated",
    "metering_rms_db",
    "metering_silence_ratio",
    "metering_spectrum",
    "metering_spectrum_frame",
    "metering_stereo_correlation",
    "metering_stereo_width",
    "metering_true_peak_db",
    "metering_vectorscope",
    "metering_vectorscope_decimated",
    "mix_stereo",
    "momentary_lufs",
    "nnls_chroma",
    "normalize",
    "normalize_rms",
    "normalize_rms_stereo",
    "normalize_stereo",
    "note_move",
    "note_stretch",
    "onset_strength_multi",
    "percussive",
    "phase_vocoder",
    "pitch_correct_timevarying",
    "pitch_correct_to_midi",
    "pitch_correct_to_midi_timevarying",
    "pitch_pyin",
    "pitch_shift",
    "pitch_yin",
    "remix_aligned_intervals",
    "rms_energy",
    "short_term_lufs",
    "spectral_flux",
    "stft_db",
    "streaming_loudness_gain",
    "streaming_loudness_gain_stereo",
    "synthesize_rir",
    "time_stretch",
    "trim",
    "voice_change",
    "voice_change_realtime",
    "vqt_to_audio",
)

# Arguments that may precede sample_rate: the audio (or primary data) arguments.
PRIMARY_ARGS = {
    "samples",
    "left",
    "right",
    "channels",
    "source",
    "reference",
    "source_left",
    "source_right",
    "reference_left",
    "reference_right",
    "strips",
    "tracks",
    "onset_envelope",
    "notes",
    "name",
    "params",
    "processor_name",
    "analysis_name",
    "n_fft",
}
# Flat-matrix inverses whose dimension arguments still precede sample_rate.
MATRIX_INPUTS = {"cqt_to_audio", "vqt_to_audio", "griffin_lim"}

# librosa mirrors (tools/api/librosa_mirrors.toml) that take a rate: sr=22050, never sample_rate.
RATE_MIRRORS = (
    "chirp",
    "chroma_cens",
    "chroma_cqt",
    "clicks",
    "cqt",
    "estimate_tuning",
    "fourier_tempogram",
    "hybrid_cqt",
    "mel_to_audio",
    "mel_to_stft",
    "mfcc",
    "mfcc_to_audio",
    "pcen",
    "piptrack",
    "plp",
    "poly_features",
    "pseudo_cqt",
    "reassigned_spectrogram",
    "remix",
    "spectral_bandwidth",
    "spectral_centroid",
    "spectral_contrast",
    "spectral_flatness",
    "spectral_rolloff",
    "stft",
    "tempogram",
    "tempogram_ratio",
    "tone",
    "vqt",
    "zero_crossing_rate",
    "onset_strength",
)

SEGMENT_FUNCTIONS = (
    "cross_similarity",
    "recurrence_matrix",
    "recurrence_to_lag",
    "lag_to_recurrence",
    "subsegment",
    "agglomerative",
    "path_enhance",
)


def _tone(seconds: float = 0.5) -> np.ndarray:
    t = np.arange(int(SR * seconds), dtype=np.float32) / SR
    return (0.5 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)


@pytest.mark.parametrize("name", OPERATIONS)
def test_operation_requires_sample_rate(name: str) -> None:
    params = inspect.signature(getattr(libsonare, name)).parameters
    param = params["sample_rate"]
    assert param.default is inspect.Parameter.empty
    if param.kind is inspect.Parameter.KEYWORD_ONLY:
        return
    assert param.kind is inspect.Parameter.POSITIONAL_OR_KEYWORD
    before = list(params)[: list(params).index("sample_rate")]
    if name not in MATRIX_INPUTS:
        assert set(before) <= PRIMARY_ARGS, before


def test_omitted_sample_rate_is_a_type_error() -> None:
    with pytest.raises(TypeError):
        libsonare.detect_bpm(_tone())  # type: ignore[call-arg]


@pytest.mark.parametrize("bad", [22050.0, 44100.5], ids=["integral-float", "fraction"])
def test_float_sample_rate_is_refused(bad: float) -> None:
    with pytest.raises(SonareValueError):
        libsonare.detect_bpm(_tone(), bad)  # type: ignore[arg-type]
    with pytest.raises(SonareValueError):
        libsonare.stft(_tone(), sr=bad)  # type: ignore[arg-type]


@pytest.mark.parametrize("bad", [True, "22050"], ids=["bool", "str"])
def test_non_number_sample_rate_is_a_type_error(bad: object) -> None:
    with pytest.raises(TypeError):
        libsonare.detect_bpm(_tone(), bad)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        libsonare.stft(_tone(), sr=bad)  # type: ignore[arg-type]


@pytest.mark.parametrize("name", RATE_MIRRORS)
def test_mirror_spells_rate_sr(name: str) -> None:
    params = inspect.signature(getattr(libsonare, name)).parameters
    assert "sample_rate" not in params
    assert params["sr"].default == 22050


@pytest.mark.parametrize("name", SEGMENT_FUNCTIONS)
def test_segment_functions_are_prefixed(name: str) -> None:
    assert callable(getattr(libsonare, f"segment_{name}"))
    assert not hasattr(libsonare, name)
    assert f"segment_{name}" in libsonare.__all__


def test_onset_strength_replaces_onset_envelope() -> None:
    assert not hasattr(libsonare, "onset_envelope")
    envelope = np.asarray(libsonare.onset_strength(_tone(), sr=SR))
    assert envelope.ndim == 1 and envelope.size > 0
    audio = libsonare.Audio.from_buffer(_tone(), SR)
    assert not hasattr(audio, "onset_envelope")
    np.testing.assert_allclose(np.asarray(audio.onset_strength()), envelope, rtol=1e-5, atol=1e-6)


def test_synthesis_mirrors_take_librosa_argument_names() -> None:
    a = np.asarray(libsonare.tone(440.0, sr=SR, duration=0.1, phi=0.5))
    assert a.size == int(SR * 0.1)
    c = np.asarray(libsonare.clicks([0.0], sr=SR, click_freq=2000.0, length=SR // 10))
    assert c.size == SR // 10
    for name, old in (("tone", "phase"), ("clicks", "frequency")):
        assert old not in inspect.signature(getattr(libsonare, name)).parameters


def test_chirp_defaults_to_exponential_sweep() -> None:
    default = np.asarray(libsonare.chirp(fmin=110.0, fmax=880.0, sr=SR, duration=0.5))
    exponential = np.asarray(
        libsonare.chirp(fmin=110.0, fmax=880.0, sr=SR, duration=0.5, linear=False)
    )
    linear = np.asarray(libsonare.chirp(fmin=110.0, fmax=880.0, sr=SR, duration=0.5, linear=True))
    np.testing.assert_allclose(default, exponential, atol=1e-6)
    assert np.max(np.abs(default - linear)) > 0.1


def test_mirror_absent_is_none_not_zero() -> None:
    assert libsonare.frames_to_samples(3, hop_length=512, n_fft=None) == 3 * 512
    assert libsonare.frames_to_samples(3, hop_length=512, n_fft=2048) == 3 * 512 + 1024
    assert libsonare.samples_to_frames(3 * 512, hop_length=512, n_fft=None) == 3
    with pytest.raises(SonareValueError):
        libsonare.frames_to_samples(3, hop_length=512, n_fft=0)
    with pytest.raises(SonareValueError):
        libsonare.samples_to_frames(3 * 512, hop_length=512, n_fft=0)
    with pytest.raises(SonareValueError):
        libsonare.clicks([0.0], sr=SR, length=0)
    y = _tone()
    libsonare.mfcc(y, sr=SR, fmax=None)
    with pytest.raises(SonareValueError):
        libsonare.mfcc(y, sr=SR, fmax=0.0)


def test_resample_names_both_rates() -> None:
    out = np.asarray(libsonare.resample(_tone(), source_sample_rate=SR, target_sample_rate=2 * SR))
    assert abs(out.size - 2 * int(SR * 0.5)) <= 2
    audio = libsonare.Audio.from_buffer(_tone(), SR)
    assert np.asarray(audio.resample(target_sample_rate=2 * SR)).size == out.size


def test_remix_aligns_to_zero_crossings_by_default() -> None:
    y = _tone()
    intervals = [100, 3000, 5000, 9000]
    default = np.asarray(libsonare.remix(y, intervals, sr=SR))
    aligned = np.asarray(libsonare.remix(y, intervals, sr=SR, align_zeros=True))
    np.testing.assert_array_equal(default, aligned)
    assert inspect.signature(libsonare.remix).parameters["align_zeros"].default is True
