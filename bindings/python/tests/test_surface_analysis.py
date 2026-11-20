"""Public analysis surface: sample-rate spelling, mirror arguments and segmentation names."""

from __future__ import annotations

import inspect
import re
from collections.abc import Callable

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
    "ebur128_loudness_range_lu",
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
    "mastering_dynamics_compressor",
    "mastering_dynamics_gate",
    "mastering_dynamics_transient_shaper",
    "mastering_analyze_pair",
    "mastering_apply_pair_processor",
    "mastering_apply_pair_processor_stereo",
    "mastering_apply_processor",
    "mastering_apply_processor_stereo",
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
    "mastering_analyze_stereo",
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
    "metering_true_peak_dbtp",
    "metering_vectorscope",
    "metering_vectorscope_decimated",
    "mix_stereo",
    "momentary_lufs",
    "nnls_chroma",
    "normalize",
    "normalize_stereo",
    "note_move",
    "note_stretch",
    "onset_strength_multi",
    "percussive",
    "phase_vocoder",
    "pitch_correct_timevarying",
    "pitch_correct_to_midi",
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
    "num_channels",
    "source",
    "reference",
    "source_left",
    "source_right",
    "reference_left",
    "reference_right",
    "strips",
    "tracks",
    "onset_envelope",
    "magnitude",
    "notes",
    "name",
    "params",
    "processor",
    "analysis",
    "n_fft",
}
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
    for convert in (libsonare.frames_to_samples, libsonare.samples_to_frames):
        name = convert.__name__
        with pytest.raises(
            SonareValueError, match=f"^{name}: n_fft must not be 0; omit it for no centring$"
        ):
            convert(3, hop_length=512, n_fft=0)
        with pytest.raises(
            SonareValueError, match=rf"^{name}: n_fft must be an integer in \[1, 2147483647\]$"
        ):
            convert(3, hop_length=512, n_fft=-5)
        for hop in (0, -5):
            with pytest.raises(
                SonareValueError,
                match=rf"^{name}: hop_length must be an integer in \[1, 2147483647\]$",
            ):
                convert(4, hop_length=hop)
    with pytest.raises(SonareValueError):
        libsonare.clicks([0.0], sr=SR, length=0)
    y = _tone()
    libsonare.mfcc(y, sr=SR, fmax=None)
    with pytest.raises(SonareValueError):
        libsonare.mfcc(y, sr=SR, fmax=0.0)


def test_resample_names_both_rates() -> None:
    out = np.asarray(libsonare.resample(_tone(), sample_rate=SR, target_sample_rate=2 * SR))
    assert abs(out.size - 2 * int(SR * 0.5)) <= 2
    audio = libsonare.Audio.from_buffer(_tone(), SR)
    assert np.asarray(audio.resample(target_sample_rate=2 * SR)).size == out.size


def test_remix_aligns_to_zero_crossings_by_default() -> None:
    y = _tone()
    intervals = [[100, 3000], [5000, 9000]]
    default = np.asarray(libsonare.remix(y, intervals, sr=SR))
    aligned = np.asarray(libsonare.remix(y, intervals, sr=SR, align_zeros=True))
    np.testing.assert_array_equal(default, aligned)
    assert inspect.signature(libsonare.remix).parameters["align_zeros"].default is True


# --- arrays and matrices -----------------------------------------------------------------


def _signal() -> np.ndarray:
    return _tone(1.0)


def _chain() -> dict[str, np.ndarray]:
    y = _signal()
    return {
        "y": y,
        "env": libsonare.onset_strength(y, sr=SR),
        "mel": libsonare.mel_spectrogram(y, SR).power,
        "mfcc": libsonare.mfcc(y, sr=SR).coefficients,
        "chroma": libsonare.chroma(y, SR).features,
    }


ONE_DIMENSIONAL_RESULTS = {
    "detect_beats": lambda y: libsonare.detect_beats(y, SR),
    "detect_downbeats": lambda y: libsonare.detect_downbeats(y, SR),
    "detect_onsets": lambda y: libsonare.detect_onsets(y, SR),
    "spectral_centroid": lambda y: libsonare.spectral_centroid(y, SR),
    "spectral_bandwidth": lambda y: libsonare.spectral_bandwidth(y, SR),
    "spectral_flux": lambda y: libsonare.spectral_flux(y, SR),
    "spectral_rolloff": lambda y: libsonare.spectral_rolloff(y, SR),
    "spectral_flatness": lambda y: libsonare.spectral_flatness(y, SR),
    "zero_crossing_rate": lambda y: libsonare.zero_crossing_rate(y, SR),
    "rms_energy": lambda y: libsonare.rms_energy(y, SR),
    "tone": lambda y: libsonare.tone(sr=SR, duration=0.1),
    "chirp": lambda y: libsonare.chirp(sr=SR, duration=0.1),
    "clicks": lambda y: libsonare.clicks([0.0, 0.1], sr=SR),
    "power_to_db": lambda y: libsonare.power_to_db(y**2 + 1e-3),
    "amplitude_to_db": lambda y: libsonare.amplitude_to_db(np.abs(y) + 1e-3),
    "db_to_power": lambda y: libsonare.db_to_power(y),
    "db_to_amplitude": lambda y: libsonare.db_to_amplitude(y),
    "preemphasis": lambda y: libsonare.preemphasis(y),
    "deemphasis": lambda y: libsonare.deemphasis(y),
    "pad_center": lambda y: libsonare.pad_center(y, y.size + 10),
    "fix_length": lambda y: libsonare.fix_length(y, y.size - 10),
    "vector_normalize": lambda y: libsonare.vector_normalize(y),
    "plp": lambda y: libsonare.plp(libsonare.onset_strength(y, sr=SR), sr=SR),
    "onset_strength": lambda y: libsonare.onset_strength(y, sr=SR),
    "momentary_lufs": lambda y: libsonare.momentary_lufs(y, SR),
    "short_term_lufs": lambda y: libsonare.short_term_lufs(y, SR),
    "resample": lambda y: libsonare.resample(y, SR, 2 * SR),
}


@pytest.mark.parametrize("name", sorted(ONE_DIMENSIONAL_RESULTS))
def test_one_dimensional_results_are_float32_arrays(name: str) -> None:
    out = ONE_DIMENSIONAL_RESULTS[name](_signal())
    assert isinstance(out, np.ndarray)
    assert out.dtype == np.float32
    assert out.ndim == 1


def test_per_frame_features_have_one_value_per_frame() -> None:
    y = _signal()
    n_frames = libsonare.stft(y, SR).magnitude.shape[1]
    for name in ("spectral_centroid", "spectral_bandwidth", "spectral_rolloff", "onset_strength"):
        assert ONE_DIMENSIONAL_RESULTS[name](y).shape == (n_frames,), name


def test_db_conversions_keep_values_and_shape() -> None:
    power = np.asarray([1.0, 0.1, 0.01], dtype=np.float32)
    db = libsonare.power_to_db(power, top_db=80.0)
    assert db.shape == power.shape
    np.testing.assert_allclose(db, [0.0, -10.0, -20.0], atol=1e-4)
    np.testing.assert_allclose(libsonare.db_to_power(db), power, rtol=1e-4)


def test_index_results_are_int32_arrays() -> None:
    c = _chain()
    peaks = libsonare.peak_pick(c["env"], 3, 3, 3, 5, 0.05, 5)
    fixed = libsonare.fix_frames([5, 20, 500], 0, 100)
    labels = libsonare.segment_agglomerative(c["chroma"], 3)
    refined = libsonare.segment_subsegment(c["chroma"], [0, 10, 20], n_segments=2)
    for out in (peaks, fixed, labels, refined):
        assert isinstance(out, np.ndarray)
        assert out.dtype == np.int32
        assert out.ndim == 1
    np.testing.assert_array_equal(fixed, [0, 5, 20, 100])
    assert labels.shape == (c["chroma"].shape[1],)


def test_matrix_results_are_two_dimensional() -> None:
    c = _chain()
    y, env = c["y"], c["env"]
    n_bins, n_frames = libsonare.stft(y, SR).magnitude.shape
    assert n_bins == 2048 // 2 + 1
    db = libsonare.stft_db(y, SR)
    assert db.dtype == np.float32 and db.shape == (n_bins, n_frames)
    assert libsonare.Audio.from_buffer(y, SR).stft_db().shape == (n_bins, n_frames)
    assert libsonare.tempogram(env, sr=SR, win_length=64).shape == (64, env.size)
    assert libsonare.cyclic_tempogram(env, SR, n_bins=30).shape == (30, env.size)
    assert libsonare.fourier_tempogram(env, sr=SR, win_length=64).shape == (33, env.size)
    assert libsonare.onset_strength_multi(y, SR, n_bands=3).shape == (3, env.size)
    assert libsonare.nnls_chroma(y, SR).shape[0] == 12
    assert libsonare.Audio.from_buffer(y, SR).nnls_chroma().shape[0] == 12
    assert libsonare.tonnetz(c["chroma"]).shape == (6, c["chroma"].shape[1])


def test_result_classes_hold_two_dimensional_arrays() -> None:
    y = _signal()
    stft = libsonare.stft(y, SR)
    n_bins, n_frames = stft.magnitude.shape
    assert stft.power.shape == (n_bins, n_frames)
    assert stft.magnitude.dtype == np.float32
    mel = libsonare.mel_spectrogram(y, SR, n_mels=40)
    assert mel.power.shape == mel.db.shape == (40, n_frames)
    mfcc = libsonare.mfcc(y, sr=SR, n_mfcc=13)
    assert mfcc.coefficients.shape == (13, n_frames)
    for fn in (libsonare.chroma, libsonare.bass_chroma):
        chroma = fn(y, SR)
        assert chroma.features.shape == (12, n_frames)
        assert chroma.mean_energy.shape == (12,)
    for fn in (libsonare.chroma_cens, libsonare.chroma_cqt):
        assert fn(y, sr=SR).features.shape[0] == 12
    for fn in (libsonare.cqt, libsonare.pseudo_cqt, libsonare.hybrid_cqt, libsonare.vqt):
        cqt = fn(y, sr=SR, n_bins=48)
        assert cqt.magnitude.shape[0] == 48
        assert cqt.frequencies.shape == (48,)
    for fn in (libsonare.pitch_yin, libsonare.pitch_pyin):
        pitch = fn(y, SR)
        assert pitch.f0.shape == pitch.voiced_prob.shape == pitch.voiced_flag.shape
        assert pitch.voiced_flag.dtype == np.bool_
    track = libsonare.piptrack(y, sr=SR)
    assert track.pitches.shape == track.magnitudes.shape == (n_bins, n_frames)
    reassigned = libsonare.reassigned_spectrogram(y, sr=SR)
    assert reassigned.magnitude.shape == reassigned.times.shape == (n_bins, n_frames)
    assert reassigned.frequencies.shape == (n_bins, n_frames)


@pytest.mark.parametrize(
    ("cls", "removed"),
    [
        ("StftResult", ("n_bins", "n_frames")),
        ("CqtResult", ("n_bins", "n_frames")),
        ("MelSpectrogramResult", ("n_mels", "n_frames", "is_db")),
        ("MfccResult", ("n_mfcc", "n_frames", "is_db")),
        ("ChromaResult", ("n_chroma", "n_frames")),
        ("PitchResult", ("n_frames",)),
        ("PiptrackResult", ("n_bins", "n_frames")),
        ("ReassignedSpectrogramResult", ("n_bins", "n_frames")),
    ],
)
def test_matrix_results_drop_their_dimension_fields(cls: str, removed: tuple[str, ...]) -> None:
    fields = {f.name for f in __import__("dataclasses").fields(getattr(libsonare, cls))}
    assert not fields & set(removed)


def test_matrix_results_are_frozen_and_compare_by_identity() -> None:
    result = libsonare.stft(_signal(), SR)
    with pytest.raises(AttributeError):
        result.n_fft = 1  # type: ignore[misc]
    assert result != libsonare.stft(_signal(), SR)
    assert result == result  # noqa: PLR0124


def test_frame_signal_columns_are_the_frames() -> None:
    y = _signal()
    frame_length, hop = 1024, 256
    frames = libsonare.frame_signal(y, frame_length, hop)
    assert frames.dtype == np.float32
    assert frames.shape == (frame_length, 1 + (y.size - frame_length) // hop)
    assert frames.flags["C_CONTIGUOUS"]
    for j in (0, 1, frames.shape[1] - 1):
        np.testing.assert_array_equal(frames[:, j], y[j * hop : j * hop + frame_length])


def test_silence_results_are_arrays_and_typed_results() -> None:
    gap = np.zeros(SR // 2, dtype=np.float32)
    y = np.concatenate([gap, _signal(), gap, _signal(), gap])
    trimmed = libsonare.trim_silence(y)
    assert isinstance(trimmed, libsonare.TrimResult)
    assert trimmed.audio.dtype == np.float32
    assert trimmed.audio.size == trimmed.end_sample - trimmed.start_sample
    assert trimmed.start_sample > 0 and trimmed.end_sample < y.size
    intervals = libsonare.split_silence(y)
    assert intervals.dtype == np.int64 and intervals.shape == (2, 2)
    assert np.all(intervals[:, 0] < intervals[:, 1])
    common = libsonare.split_silence_common([y, y])
    assert common.dtype == np.int64 and common.shape[1] == 2
    with_report = libsonare.split_silence_common_with_report([y, y])
    assert isinstance(with_report, libsonare.SplitSilenceCommonWithReportResult)
    np.testing.assert_array_equal(with_report.intervals, common)
    assert isinstance(with_report.report, libsonare.SilenceCommonReport)


def test_matrix_inputs_take_one_two_dimensional_array() -> None:
    c = _chain()
    mel, mfcc, chroma = c["mel"], c["mfcc"], c["chroma"]
    delta = libsonare.mel_delta(mel)
    assert delta.dtype == np.float32 and delta.shape == mel.shape
    assert libsonare.pcen(mel, sr=SR).shape == mel.shape
    assert libsonare.tonnetz(chroma).shape == (6, chroma.shape[1])
    recurrence = libsonare.segment_recurrence_matrix(chroma)
    n = chroma.shape[1]
    assert recurrence.shape == (n, n)
    assert libsonare.segment_path_enhance(recurrence, 3).shape == (n, n)
    lag = libsonare.segment_recurrence_to_lag(recurrence)
    assert lag.shape == (2 * n, n)
    assert libsonare.segment_lag_to_recurrence(lag).shape == (n, n)
    assert libsonare.segment_cross_similarity(chroma, chroma).ndim == 2
    stft = libsonare.mel_to_stft(mel, sr=SR)
    assert stft.dtype == np.float32 and stft.shape == (2048 // 2 + 1, mel.shape[1])
    assert libsonare.mfcc_to_mel(mfcc, n_mels=64).shape == (64, mfcc.shape[1])
    ratio = libsonare.tempogram_ratio(libsonare.tempogram(c["env"], sr=SR, win_length=64), sr=SR)
    assert ratio.dtype == np.float32 and ratio.ndim == 1


def test_matrix_inputs_accept_nested_lists() -> None:
    mel = _chain()["mel"]
    np.testing.assert_allclose(
        libsonare.mel_delta(mel.tolist()), libsonare.mel_delta(mel), rtol=1e-6, atol=1e-7
    )


def test_inverse_transforms_return_audio_arrays() -> None:
    c = _chain()
    y = c["y"]
    magnitude = libsonare.stft(y, SR).magnitude
    cqt = libsonare.cqt(y, sr=SR).magnitude
    for audio in (
        libsonare.mel_to_audio(c["mel"], sr=SR, n_iter=2),
        libsonare.mfcc_to_audio(c["mfcc"], sr=SR, n_iter=2),
        libsonare.griffin_lim(magnitude, SR, n_iter=2),
        libsonare.cqt_to_audio(cqt, SR, n_iter=2),
        libsonare.vqt_to_audio(cqt, SR, n_iter=2),
    ):
        assert audio.dtype == np.float32 and audio.ndim == 1 and audio.size > 0


MATRIX_INPUT_CALLS = {
    "mel_delta": (lambda m: libsonare.mel_delta(m), "features"),
    "pcen": (lambda m: libsonare.pcen(m), "values"),
    "tonnetz": (lambda m: libsonare.tonnetz(m), "chromagram"),
    "segment_recurrence_matrix": (lambda m: libsonare.segment_recurrence_matrix(m), "data"),
    "segment_agglomerative": (lambda m: libsonare.segment_agglomerative(m, 2), "data"),
    "segment_subsegment": (lambda m: libsonare.segment_subsegment(m, [0, 2]), "data"),
    "segment_cross_similarity": (lambda m: libsonare.segment_cross_similarity(m, m), "x"),
    "segment_recurrence_to_lag": (lambda m: libsonare.segment_recurrence_to_lag(m), "recurrence"),
    "segment_lag_to_recurrence": (lambda m: libsonare.segment_lag_to_recurrence(m), "lag"),
    "segment_path_enhance": (lambda m: libsonare.segment_path_enhance(m, 3), "recurrence"),
    "tempogram_ratio": (lambda m: libsonare.tempogram_ratio(m), "tempogram"),
    "mel_to_stft": (lambda m: libsonare.mel_to_stft(m), "mel"),
    "mel_to_audio": (lambda m: libsonare.mel_to_audio(m, n_iter=1), "mel"),
    "mfcc_to_mel": (lambda m: libsonare.mfcc_to_mel(m), "coefficients"),
    "mfcc_to_audio": (lambda m: libsonare.mfcc_to_audio(m, n_iter=1), "coefficients"),
    "griffin_lim": (lambda m: libsonare.griffin_lim(m, SR, n_iter=1), "magnitude"),
    "cqt_to_audio": (lambda m: libsonare.cqt_to_audio(m, SR, n_iter=1), "magnitude"),
    "vqt_to_audio": (lambda m: libsonare.vqt_to_audio(m, SR, n_iter=1), "magnitude"),
}


@pytest.mark.parametrize("name", sorted(MATRIX_INPUT_CALLS))
@pytest.mark.parametrize("shape", [(8,), (2, 4, 3)], ids=["1-D", "3-D"])
def test_matrix_inputs_refuse_a_wrong_rank_by_name(name: str, shape: tuple[int, ...]) -> None:
    call, parameter = MATRIX_INPUT_CALLS[name]
    with pytest.raises(SonareValueError, match=rf"{name}: {parameter} must be a 2-D array"):
        call(np.ones(shape, dtype=np.float32))


@pytest.mark.parametrize("name", ["mel_delta", "pcen", "griffin_lim"])
def test_matrix_inputs_refuse_non_finite_values(name: str) -> None:
    call, parameter = MATRIX_INPUT_CALLS[name]
    bad = np.ones((4, 6), dtype=np.float32)
    bad[1, 2] = np.nan
    with pytest.raises(SonareValueError, match=parameter):
        call(bad)


def test_segment_inputs_that_must_be_square_are_refused() -> None:
    with pytest.raises(SonareValueError, match="recurrence must be square"):
        libsonare.segment_path_enhance(np.ones((4, 5), dtype=np.float32), 3)


@pytest.mark.parametrize(
    "name",
    [
        "SegmentMatrix",
        "StftMagnitudeResult",
        "MelPowerResult",
        "mel_result_to_stft",
        "mel_result_to_audio",
        "mfcc_result_to_mel",
        "mfcc_result_to_audio",
    ],
)
def test_removed_result_types_and_helpers_are_absent(name: str) -> None:
    assert not hasattr(libsonare, name)
    assert name not in libsonare.__all__
    assert not hasattr(libsonare.types, name)


@pytest.mark.parametrize(
    "name",
    [
        "TrimResult",
        "SplitSilenceCommonWithReportResult",
        "PiptrackResult",
        "ReassignedSpectrogramResult",
    ],
)
def test_result_types_are_importable(name: str) -> None:
    assert getattr(libsonare, name) is not None
    assert name in libsonare.__all__


def test_audio_methods_return_arrays() -> None:
    audio = libsonare.Audio.from_buffer(_signal(), SR)
    for method in (
        "detect_beats",
        "detect_downbeats",
        "detect_onsets",
        "onset_strength",
        "momentary_lufs",
        "short_term_lufs",
        "spectral_centroid",
        "spectral_bandwidth",
        "spectral_rolloff",
        "spectral_flatness",
        "zero_crossing_rate",
        "rms_energy",
    ):
        out = getattr(audio, method)()
        assert isinstance(out, np.ndarray) and out.dtype == np.float32, method
    assert audio.resample(2 * SR).dtype == np.float32
    assert audio.mel_spectrogram().power.ndim == 2
    assert audio.pitch_yin().f0.ndim == 1


# ---------------------------------------------------------------------------
# Units in names: cents, seconds, hertz and the tempo range
# ---------------------------------------------------------------------------


def test_tuning_cents_round_trips_through_the_converters() -> None:
    hz = libsonare.tuning_to_reference_hz(10.0)
    assert hz == pytest.approx(440.0 * 2.0 ** (10.0 / 1200.0), rel=1e-5)
    assert libsonare.reference_hz_to_tuning(hz) == pytest.approx(10.0, abs=1e-2)
    assert libsonare.tuning_to_reference_hz(tuning_cents=0.0, a4_hz=432.0) == pytest.approx(432.0)
    assert libsonare.reference_hz_to_tuning(reference_hz=432.0, a4_hz=432.0) == pytest.approx(0.0)


@pytest.mark.parametrize(
    "name", ["detect_key", "detect_key_candidates", "detect_chords", "analyze"]
)
def test_tuning_is_named_tuning_cents(name: str) -> None:
    parameters = inspect.signature(getattr(libsonare, name)).parameters
    assert "tuning_cents" in parameters
    assert "tuning" not in parameters


def test_given_tuning_cents_is_reported_on_the_results() -> None:
    samples = _signal()
    assert libsonare.detect_key(samples, SR, tuning_cents=12.5).tuning_cents == pytest.approx(12.5)
    assert libsonare.detect_chords(samples, SR, tuning_cents=-7.0).tuning_cents == pytest.approx(
        -7.0
    )
    with pytest.raises(SonareValueError):
        libsonare.detect_key(samples, SR, tuning_cents=50.0)


def test_time_to_frames_takes_seconds() -> None:
    assert libsonare.time_to_frames(time_sec=1.0, sr=22050, hop_length=512) == 43


def test_note_segment_and_transcribe_names_carry_their_unit() -> None:
    fields = {f.name for f in libsonare.NoteSegment.__dataclass_fields__.values()}
    assert {"start_sec", "end_sec"} <= fields
    assert not {"start_seconds", "end_seconds"} & fields
    parameters = inspect.signature(libsonare.transcribe).parameters
    assert {"f0_min_hz", "f0_max_hz"} <= set(parameters)
    assert not {"fmin", "fmax"} & set(parameters)


@pytest.mark.parametrize("name", ["detect_bpm", "detect_beats"])
def test_tempo_search_range_is_keyword_only_and_validated(name: str) -> None:
    samples = _signal()
    function = getattr(libsonare, name)
    parameters = inspect.signature(function).parameters
    for bound in ("bpm_min", "bpm_max"):
        assert parameters[bound].kind is inspect.Parameter.KEYWORD_ONLY
        assert parameters[bound].default is None
    function(samples, SR)
    function(samples, SR, bpm_min=80.0, bpm_max=180.0)
    with pytest.raises(SonareValueError):
        function(samples, SR, bpm_min=180.0, bpm_max=80.0)
    with pytest.raises(SonareValueError):
        function(samples, SR, bpm_min=-1.0)


@pytest.mark.parametrize("name", ["detect_bpm", "detect_beats"])
def test_audio_tempo_search_range_matches_the_free_function(name: str) -> None:
    samples = _signal()
    audio = libsonare.Audio.from_buffer(samples, SR)
    method = getattr(audio, name)
    parameters = inspect.signature(method).parameters
    for bound in ("bpm_min", "bpm_max"):
        assert parameters[bound].kind is inspect.Parameter.KEYWORD_ONLY
        assert parameters[bound].default is None
    np.testing.assert_allclose(method(), getattr(libsonare, name)(samples, SR))
    np.testing.assert_allclose(
        method(bpm_min=80.0, bpm_max=180.0),
        getattr(libsonare, name)(samples, SR, bpm_min=80.0, bpm_max=180.0),
    )
    with pytest.raises(SonareValueError):
        method(bpm_min=180.0, bpm_max=80.0)
    with pytest.raises(SonareValueError):
        method(bpm_min=-1.0)


def test_reattack_ratio_none_is_no_split_and_zero_is_refused() -> None:
    samples = _signal()
    libsonare.transcribe(samples, SR, polyphonic=True, tempo_bpm=120.0, reattack_ratio=None)
    libsonare.transcribe(samples, SR, polyphonic=True, tempo_bpm=120.0, reattack_ratio=2.0)
    for value in (0.0, -1.0, 1.0):
        with pytest.raises(SonareValueError, match="reattack_ratio"):
            libsonare.transcribe(
                samples, SR, polyphonic=True, tempo_bpm=120.0, reattack_ratio=value
            )
        with pytest.raises(SonareValueError, match="reattack_ratio"):
            libsonare.PolyphonicAnalysis.analyze(samples, SR, reattack_ratio=value)


def test_note_segments_takes_voiced_or_voiced_prob_by_keyword() -> None:
    f0 = [440.0, 441.0, 439.0, 440.0, 0.0, 0.0, 660.0, 661.0, 659.0, 660.0]
    probability = [1.0] * 4 + [0.0] * 2 + [1.0] * 4
    flags = [1, 1, 1, 1, 0, 0, 1, 1, 1, 1]
    spans = [(0, 4), (6, 10)]
    by_probability = libsonare.note_segments(f0, 100.0, voiced_prob=probability)
    by_flags = libsonare.note_segments(f0, 100.0, voiced=flags)
    by_bools = libsonare.note_segments(f0, 100.0, voiced=np.array(flags, dtype=bool))
    for found in (by_probability, by_flags, by_bools):
        assert [(s.frame_start, s.frame_end) for s in found] == spans
    assert [s.start_sec for s in by_flags] == pytest.approx([0.0, 0.06])
    assert [s.end_sec for s in by_flags] == pytest.approx([0.04, 0.10])

    with pytest.raises(TypeError, match="voiced"):
        libsonare.note_segments(f0, 100.0)
    with pytest.raises(SonareValueError, match="f0_hz and voiced must have the same length$"):
        libsonare.note_segments(f0, 100.0, voiced=flags[:-1])
    with pytest.raises(TypeError):
        libsonare.note_segments(f0, probability, 100.0)  # type: ignore[misc]
    with pytest.raises(TypeError, match="voiced_flags"):
        libsonare.note_segments(f0, 100.0, voiced_flags=flags)  # type: ignore[call-arg]


def test_note_segments_voiced_wins_over_voiced_prob() -> None:
    f0 = [440.0, 441.0, 439.0, 440.0, 0.0, 0.0, 660.0, 661.0, 659.0, 660.0]
    flags = [1, 1, 1, 1, 0, 0, 1, 1, 1, 1]
    silent = [0.0] * len(f0)
    by_flags = libsonare.note_segments(f0, 100.0, voiced=flags)
    both = libsonare.note_segments(f0, 100.0, voiced=flags, voiced_prob=silent)
    assert libsonare.note_segments(f0, 100.0, voiced_prob=silent) == []
    assert [(s.frame_start, s.frame_end) for s in both] == [(0, 4), (6, 10)]
    assert both == by_flags


def test_stream_names_carry_their_unit() -> None:
    analyzer = libsonare.StreamAnalyzer(sample_rate=SR, reference_hz=432.0, fmin_hz=50.0)
    try:
        assert analyzer.current_time_sec == 0.0
        analyzer.set_reference_hz(440.0)
        analyzer.set_expected_duration(duration_sec=10.0)
        analyzer.process(_signal())
        frames = analyzer.read_frames(8)
        assert len(frames.timestamps_sec) == frames.n_frames > 0
        assert not hasattr(frames, "timestamps")
        stats = analyzer.stats()
        assert hasattr(stats, "duration_sec")
        for name in ("bar_duration_sec", "accumulated_sec"):
            assert hasattr(stats.estimate, name), name
    finally:
        analyzer.close()


# ---------------------------------------------------------------------------
# Analysis result classes: typed beats, arrays, names and the progress option
# ---------------------------------------------------------------------------


def _clicks(seconds: float = 8.0, bpm: float = 120.0) -> np.ndarray:
    """An accented click track: a decaying 1 kHz burst on every beat, louder every fourth."""
    out = np.zeros(int(SR * seconds), dtype=np.float32)
    burst = np.exp(-np.arange(1500) / 200.0) * np.sin(2 * np.pi * 1000.0 * np.arange(1500) / SR)
    step = int(SR * 60.0 / bpm)
    for index, start in enumerate(range(0, len(out) - len(burst), step)):
        out[start : start + len(burst)] += (0.9 if index % 4 == 0 else 0.5) * burst.astype(
            np.float32
        )
    return out


@pytest.fixture(scope="module")
def analysis() -> libsonare.AnalysisResult:
    return libsonare.analyze(_clicks(), SR, compute_tempo_curve=True)


def test_beats_and_downbeats_are_beat_objects(analysis: libsonare.AnalysisResult) -> None:
    assert analysis.beats
    for beat in (*analysis.beats, *analysis.downbeats):
        assert isinstance(beat, libsonare.Beat)
        assert isinstance(beat.time_sec, float)
        assert isinstance(beat.strength, float)
    beat_times = [beat.time_sec for beat in analysis.beats]
    assert beat_times == sorted(beat_times)
    times = set(beat_times)
    assert all(beat.time_sec in times for beat in analysis.downbeats)
    assert all(isinstance(chord, libsonare.AnalysisChord) for chord in analysis.chords)
    assert all(isinstance(c, libsonare.AnalysisBpmCandidate) for c in analysis.bpm_candidates)
    assert {c.relation for c in analysis.bpm_candidates} <= {"primary", "half", "double", "other"}
    assert isinstance(analysis.timbre, libsonare.TimbreFrame)
    assert isinstance(analysis.dynamics, libsonare.AnalysisDynamics)
    assert isinstance(analysis.rhythm, libsonare.AnalysisRhythm)
    assert isinstance(analysis.melody, libsonare.MelodyResult)
    assert isinstance(analysis.beat_observations, libsonare.BeatObservations)


def test_bpm_analysis_candidates_carry_no_relation() -> None:
    """A field whose presence depends on the returning function is a second record."""
    result = libsonare.analyze_bpm(_clicks(), SR)
    assert result.candidates
    assert all(type(c) is libsonare.BpmCandidate for c in result.candidates)
    assert not any(hasattr(c, "relation") for c in result.candidates)


def test_removed_result_attributes_are_absent(analysis: libsonare.AnalysisResult) -> None:
    for name in (
        "beat_times",
        "beat_strengths",
        "downbeat_indices",
        "beatTimes",
        "bpmConfidence",
        "timeSignature",
        "beatObservations",
        "beatLocalBpm",
    ):
        assert not hasattr(analysis, name), name
    assert not hasattr(libsonare, "analyze_with_progress")
    for removed in ("AnalysisTimbre", "SectionResult", "BpmHypothesis", "AnalysisBeatObservations"):
        assert not hasattr(libsonare, removed), removed
    key = libsonare.detect_key(_signal(), SR)
    assert not hasattr(key, "shortName")
    chord = analysis.chords[0] if analysis.chords else None
    if chord is not None:
        assert not any(hasattr(chord, name) for name in ("rootName", "bassName", "canonical_name"))
    assert not hasattr(analysis.dynamics, "dynamicRangeDb")


def test_analysis_arrays_are_float32_ndarrays(analysis: libsonare.AnalysisResult) -> None:
    assert isinstance(analysis.beat_local_bpm, np.ndarray)
    assert analysis.beat_local_bpm.dtype == np.float32
    observations = analysis.beat_observations
    assert observations is not None
    for name in ("onset_strength", "low_frequency_energy", "chord_change"):
        values = getattr(observations, name)
        assert isinstance(values, np.ndarray) and values.dtype == np.float32, name

    signal = _signal()
    rhythm = libsonare.analyze_rhythm(signal, SR)
    dynamics = libsonare.analyze_dynamics(signal, SR)
    timbre = libsonare.analyze_timbre(signal, SR)
    bpm = libsonare.analyze_bpm(signal, SR)
    acoustic = libsonare.analyze_impulse_response(signal, SR)
    boundaries = libsonare.detect_boundaries(signal, SR)
    meter = libsonare.estimate_meter(analysis.beats)
    arrays = {
        "RhythmResult.beat_intervals": rhythm.beat_intervals,
        "DynamicsResult.loudness_times_sec": dynamics.loudness_times_sec,
        "DynamicsResult.loudness_rms_db": dynamics.loudness_rms_db,
        "TimbreResult.spectral_centroid": timbre.spectral_centroid,
        "TimbreResult.spectral_flatness": timbre.spectral_flatness,
        "TimbreResult.spectral_rolloff": timbre.spectral_rolloff,
        "BpmAnalysisResult.autocorrelation": bpm.autocorrelation,
        "BpmAnalysisResult.tempogram": bpm.tempogram,
        "AcousticResult.rt60_bands": acoustic.rt60_bands,
        "AcousticResult.edt_bands": acoustic.edt_bands,
        "AcousticResult.c50_bands": acoustic.c50_bands,
        "AcousticResult.c80_bands": acoustic.c80_bands,
        "BoundaryResult.novelty_curve": boundaries.novelty_curve,
        "MeterEstimate.candidate_scores": meter.candidate_scores,
    }
    for label, values in arrays.items():
        assert isinstance(values, np.ndarray) and values.dtype == np.float32, label
    assert isinstance(meter.grouping, np.ndarray) and meter.grouping.dtype == np.int32
    assert not hasattr(boundaries, "n_frames")
    assert not hasattr(dynamics, "loudness_times")


def test_boundary_and_beat_types_are_importable() -> None:
    for name in (
        "Beat",
        "AnalysisChord",
        "BeatObservations",
        "Boundary",
        "BoundaryResult",
        "BpmCandidate",
        "AnalysisBpmCandidate",
        "BpmCandidateRelation",
        "CHORD_QUALITIES",
        "ProgressCallback",
        "CancelCallback",
    ):
        assert getattr(libsonare, name) is not None
        assert name in libsonare.__all__


def test_analyze_reports_monotone_progress_and_cancels() -> None:
    calls: list[tuple[float, str]] = []
    result = libsonare.analyze(
        _tone(2.0), SR, on_progress=lambda progress, stage: calls.append((progress, stage))
    )
    assert isinstance(result, libsonare.AnalysisResult)
    assert calls
    fractions = [progress for progress, _ in calls]
    assert fractions == sorted(fractions)
    assert all(0.0 <= value <= 1.0 for value in fractions)
    assert all(isinstance(stage, str) for _, stage in calls)

    with pytest.raises(libsonare.SonareError) as caught:
        libsonare.analyze(_tone(2.0), SR, cancel=lambda: True)
    assert caught.value.code == libsonare.ErrorCode.CANCELLED


def test_estimate_meter_takes_the_beats_of_an_analysis(analysis: libsonare.AnalysisResult) -> None:
    meter = libsonare.estimate_meter(analysis.beats)
    assert meter.searched
    assert isinstance(meter.time_signature, libsonare.TimeSignature)
    assert "beat_times" not in inspect.signature(libsonare.estimate_meter).parameters
    with pytest.raises(TypeError):
        libsonare.estimate_meter([0.5, 1.0, 1.5])  # type: ignore[list-item]
    with pytest.raises(SonareValueError):
        libsonare.estimate_meter([])


def test_chord_qualities_index_the_c_ordinals() -> None:
    assert len(libsonare.CHORD_QUALITIES) == 25
    assert libsonare.CHORD_QUALITIES[0] == "major"
    assert libsonare.CHORD_QUALITIES[9] == "unknown"
    assert libsonare.CHORD_QUALITIES[24] == "dominant7Sharp9"
    # A known A-minor-seventh detection reports the token its ordinal indexes.
    minor7 = libsonare.CHORD_QUALITIES.index("minor7")
    assert minor7 == 6
    names = {
        quality: libsonare.Chord(libsonare.PitchClass.A, quality, 0.0, 1.0, 1.0).name
        for quality in libsonare.CHORD_QUALITIES
    }
    assert set(names) == set(libsonare.CHORD_QUALITIES)
    detected = libsonare.detect_chords(_signal(), SR, use_beat_sync=False).chords
    assert all(chord.quality in libsonare.CHORD_QUALITIES for chord in detected)
    assert all(chord.name for chord in detected)


def test_chord_names_and_roman_numerals_are_fields() -> None:
    chord = libsonare.Chord(libsonare.PitchClass.C, "major", 0.0, 1.0, 1.0, name="C")
    assert chord.name == "C"
    assert not hasattr(chord, "roman_numeral")
    functional = libsonare.chord_functions([chord], (libsonare.PitchClass.C, "major"))
    assert functional[0].roman_numeral == "I"
    assert functional[0].name == "C"
    assert not hasattr(functional[0], "roman")


def test_sections_are_a_named_list() -> None:
    result = libsonare.analyze_sections(_tone(8.0), SR, min_section_sec=2.0)
    assert isinstance(result.sections, list)
    for section in result.sections:
        assert isinstance(section, libsonare.Section)
        assert section.name
    assert "canonical_name" not in {f.name for f in libsonare.Section.__dataclass_fields__.values()}


def test_key_detection_names_the_chroma_high_pass() -> None:
    for name in ("detect_key", "detect_key_candidates"):
        parameters = inspect.signature(getattr(libsonare, name)).parameters
        assert "chroma_high_pass_hz" in parameters
        assert "high_pass_hz" not in parameters
    assert "chroma_high_pass_hz" in inspect.signature(libsonare.analyze).parameters
    assert "chroma_highpass_hz" not in inspect.signature(libsonare.analyze).parameters
    libsonare.detect_key(_signal(), SR, chroma_high_pass_hz=120.0)


def test_transcribe_result_is_frozen() -> None:
    result = libsonare.transcribe(_signal(), SR, tempo_bpm=120.0)
    with pytest.raises(Exception, match="frozen|assign"):
        result.note_count = 0  # type: ignore[misc]


@pytest.mark.parametrize("name", ["pad_center", "fix_length"])
def test_target_size_is_refused_by_cause(name: str) -> None:
    fn = getattr(libsonare, name)
    values = np.ones(8, dtype=np.float32)
    for bad in (-1, 1.5):
        with pytest.raises(
            SonareValueError, match=rf"^{name}: target_size must be an integer in \[0, "
        ):
            fn(values, bad)
    for bad in ("8", True):
        with pytest.raises(TypeError, match=f"^{name}: target_size must be an integer"):
            fn(values, bad)


_CQT_HOP_ENTRIES = (
    "cqt",
    "vqt",
    "pseudo_cqt",
    "hybrid_cqt",
    "chroma_cqt",
    "chroma_cens",
    "bass_chroma",
    "cqt_to_audio",
    "vqt_to_audio",
)


@pytest.mark.parametrize("name", _CQT_HOP_ENTRIES)
@pytest.mark.parametrize("hop", [0, 1.5, 65537, 2**31])
def test_constant_q_hop_is_refused_by_the_core_bound(name: str, hop: object) -> None:
    first = np.ones((12, 4), dtype=np.float32) if name.endswith("_to_audio") else _tone()
    with pytest.raises(
        SonareValueError, match=rf"^{name}: hop_length must be an integer in \[1, 65536\]$"
    ):
        getattr(libsonare, name)(first, SR, hop_length=hop)


def test_constant_q_hop_takes_the_bound_itself() -> None:
    libsonare.cqt(_tone(), SR, hop_length=65536)


_MEL = np.ones((16, 4), dtype=np.float32)
_MFCC = np.ones((13, 4), dtype=np.float32)
_STFT_MAGNITUDE = np.ones((257, 4), dtype=np.float32)
_INT_MAX = "2147483647"


@pytest.mark.parametrize(
    ("call", "text"),
    [
        (
            lambda: libsonare.mel_to_stft(_MEL, SR, n_fft=0),
            f"mel_to_stft: n_fft must be an integer in [1, {_INT_MAX}]",
        ),
        (
            lambda: libsonare.mel_to_stft(_MEL, 0),
            f"mel_to_stft: sr must be an integer in [1, {_INT_MAX}]",
        ),
        (
            lambda: libsonare.mel_to_audio(_MEL, SR, n_fft=0, n_iter=1),
            f"mel_to_audio: n_fft must be an integer in [1, {_INT_MAX}]",
        ),
        (
            lambda: libsonare.mfcc_to_audio(_MFCC, n_mels=16, sr=SR, n_fft=0, n_iter=1),
            f"mfcc_to_audio: n_fft must be an integer in [1, {_INT_MAX}]",
        ),
        (
            lambda: libsonare.griffin_lim(_STFT_MAGNITUDE, 1000, n_iter=1),
            "griffin_lim: sample_rate must be an integer in [8000, 384000]",
        ),
        (
            lambda: libsonare.griffin_lim(_STFT_MAGNITUDE, SR, n_iter=0),
            "griffin_lim: n_iter must be an integer in [1, 256]",
        ),
        (
            lambda: libsonare.griffin_lim(_STFT_MAGNITUDE, SR, n_iter=1, momentum=1.0),
            "griffin_lim: momentum must be finite and in [0, 1)",
        ),
        (
            lambda: libsonare.spectral_bandwidth(_tone(), SR, p=0.0),
            "spectral_bandwidth: p must be finite and greater than 0",
        ),
        (
            lambda: libsonare.spectral_flux(_tone(), SR, lag=0),
            f"spectral_flux: lag must be an integer in [1, {_INT_MAX}]",
        ),
        (
            lambda: libsonare.zero_crossings(_tone(), threshold=-1.0),
            "zero_crossings: threshold must be finite and at least 0",
        ),
        (
            lambda: libsonare.onset_backtrack([2], np.array([1.0, np.nan, 1.0], np.float32)),
            "onset_backtrack: energy contains a non-finite sample at index 1",
        ),
        (
            lambda: libsonare.tuning_to_reference_hz(0.0, a4_hz=0.0),
            "tuning_to_reference_hz: a4_hz must be finite and greater than 0",
        ),
        (
            lambda: libsonare.tuning_to_reference_hz(1e9),
            "tuning_to_reference_hz: "
            "a4_hz * 2^(tuning_cents / 1200) must be finite and greater than 0",
        ),
        (
            lambda: libsonare.reference_hz_to_tuning(0.0),
            "reference_hz_to_tuning: reference_hz must be finite and greater than 0",
        ),
        (
            lambda: libsonare.note_segments(
                np.full(4, 220.0, np.float32), 100.0, voiced_prob=np.full(4, 2.0, np.float32)
            ),
            "note_segments: voiced_prob[0] must be finite and in [0, 1]",
        ),
        (
            lambda: libsonare.note_segments(
                np.full(4, 220.0, np.float32), 0.0, voiced_prob=np.full(4, 0.5, np.float32)
            ),
            "note_segments: frame_rate must be finite and greater than 0",
        ),
    ],
)
def test_a_library_refusal_states_its_cause(call: Callable[[], object], text: str) -> None:
    with pytest.raises(SonareValueError, match=f"^{re.escape(text)}$"):
        call()
