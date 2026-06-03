"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_features_signatures(lib: ctypes.CDLL) -> None:
    # --- Features - Spectrogram ---

    # sonare_stft
    lib.sonare_stft.restype = ctypes.c_int32
    lib.sonare_stft.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareStftResult),
    ]

    # sonare_stft_db
    lib.sonare_stft_db.restype = ctypes.c_int32
    lib.sonare_stft_db.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ]

    # --- Features - Mel ---

    # sonare_mel_spectrogram
    lib.sonare_mel_spectrogram.restype = ctypes.c_int32
    lib.sonare_mel_spectrogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMelResult),
    ]

    # sonare_mfcc
    lib.sonare_mfcc.restype = ctypes.c_int32
    lib.sonare_mfcc.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMfccResult),
    ]

    # --- Features - Chroma ---

    # sonare_chroma
    lib.sonare_chroma.restype = ctypes.c_int32
    lib.sonare_chroma.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareChromaResult),
    ]

    # --- Features - Spectral ---

    # sonare_spectral_centroid
    lib.sonare_spectral_centroid.restype = ctypes.c_int32
    lib.sonare_spectral_centroid.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_bandwidth
    lib.sonare_spectral_bandwidth.restype = ctypes.c_int32
    lib.sonare_spectral_bandwidth.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_rolloff
    lib.sonare_spectral_rolloff.restype = ctypes.c_int32
    lib.sonare_spectral_rolloff.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_flatness
    lib.sonare_spectral_flatness.restype = ctypes.c_int32
    lib.sonare_spectral_flatness.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_zero_crossing_rate
    lib.sonare_zero_crossing_rate.restype = ctypes.c_int32
    lib.sonare_zero_crossing_rate.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_rms_energy
    lib.sonare_rms_energy.restype = ctypes.c_int32
    lib.sonare_rms_energy.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Features - Pitch ---

    # sonare_pitch_yin
    lib.sonare_pitch_yin.restype = ctypes.c_int32
    lib.sonare_pitch_yin.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(SonarePitchResult),
    ]

    # sonare_pitch_pyin
    lib.sonare_pitch_pyin.restype = ctypes.c_int32
    lib.sonare_pitch_pyin.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(SonarePitchResult),
    ]

    # --- Core - Conversion ---

    lib.sonare_hz_to_mel.restype = ctypes.c_float
    lib.sonare_hz_to_mel.argtypes = [ctypes.c_float]

    lib.sonare_mel_to_hz.restype = ctypes.c_float
    lib.sonare_mel_to_hz.argtypes = [ctypes.c_float]

    lib.sonare_hz_to_midi.restype = ctypes.c_float
    lib.sonare_hz_to_midi.argtypes = [ctypes.c_float]

    lib.sonare_midi_to_hz.restype = ctypes.c_float
    lib.sonare_midi_to_hz.argtypes = [ctypes.c_float]

    lib.sonare_hz_to_note.restype = ctypes.c_char_p
    lib.sonare_hz_to_note.argtypes = [ctypes.c_float]

    lib.sonare_note_to_hz.restype = ctypes.c_float
    lib.sonare_note_to_hz.argtypes = [ctypes.c_char_p]

    lib.sonare_frames_to_time.restype = ctypes.c_float
    lib.sonare_frames_to_time.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    lib.sonare_time_to_frames.restype = ctypes.c_int32
    lib.sonare_time_to_frames.argtypes = [ctypes.c_float, ctypes.c_int, ctypes.c_int]

    lib.sonare_frames_to_samples.restype = ctypes.c_int32
    lib.sonare_frames_to_samples.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    lib.sonare_samples_to_frames.restype = ctypes.c_int32
    lib.sonare_samples_to_frames.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    for name in (
        "sonare_power_to_db",
        "sonare_amplitude_to_db",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for name in (
        "sonare_db_to_power",
        "sonare_db_to_amplitude",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for name in ("sonare_preemphasis", "sonare_deemphasis"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.sonare_trim_silence.restype = ctypes.c_int32
    lib.sonare_trim_silence.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_split_silence.restype = ctypes.c_int32
    lib.sonare_split_silence.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_frame_signal.restype = ctypes.c_int32
    lib.sonare_frame_signal.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    for name in ("sonare_pad_center", "sonare_fix_length"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.sonare_fix_frames.restype = ctypes.c_int32
    lib.sonare_fix_frames.argtypes = [
        ctypes.POINTER(ctypes.c_int),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_peak_pick.restype = ctypes.c_int32
    lib.sonare_peak_pick.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_vector_normalize.restype = ctypes.c_int32
    lib.sonare_vector_normalize.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_pcen.restype = ctypes.c_int32
    lib.sonare_pcen.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_tonnetz.restype = ctypes.c_int32
    lib.sonare_tonnetz.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_tempogram.restype = ctypes.c_int32
    lib.sonare_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_tempogram_with_mode.restype = ctypes.c_int32
    lib.sonare_tempogram_with_mode.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_cyclic_tempogram.restype = ctypes.c_int32
    lib.sonare_cyclic_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_plp.restype = ctypes.c_int32
    lib.sonare_plp.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_onset_strength
    lib.sonare_onset_strength.restype = ctypes.c_int32
    lib.sonare_onset_strength.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_fourier_tempogram
    lib.sonare_fourier_tempogram.restype = ctypes.c_int32
    lib.sonare_fourier_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_tempogram_ratio
    lib.sonare_tempogram_ratio.restype = ctypes.c_int32
    lib.sonare_tempogram_ratio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_nnls_chroma
    lib.sonare_nnls_chroma.restype = ctypes.c_int32
    lib.sonare_nnls_chroma.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_lufs
    lib.sonare_lufs.restype = ctypes.c_int32
    lib.sonare_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareLufsResult),
    ]

    # sonare_momentary_lufs
    lib.sonare_momentary_lufs.restype = ctypes.c_int32
    lib.sonare_momentary_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_short_term_lufs
    lib.sonare_short_term_lufs.restype = ctypes.c_int32
    lib.sonare_short_term_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Metering (offline) — basic / true peak / clipping / dynamic range ---

    for _name in (
        "sonare_metering_peak_db",
        "sonare_metering_rms_db",
        "sonare_metering_crest_factor_db",
        "sonare_metering_dc_offset",
    ):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
        ]

    lib.sonare_metering_true_peak_db.restype = ctypes.c_int32
    lib.sonare_metering_true_peak_db.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_metering_detect_clipping.restype = ctypes.c_int32
    lib.sonare_metering_detect_clipping.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_size_t,
        ctypes.POINTER(SonareClippingResult),
    ]

    lib.sonare_free_clipping_result.restype = None
    lib.sonare_free_clipping_result.argtypes = [ctypes.POINTER(SonareClippingResult)]

    lib.sonare_metering_dynamic_range.restype = ctypes.c_int32
    lib.sonare_metering_dynamic_range.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareDynamicRangeResult),
    ]

    lib.sonare_free_dynamic_range_result.restype = None
    lib.sonare_free_dynamic_range_result.argtypes = [ctypes.POINTER(SonareDynamicRangeResult)]

    # --- Metering (offline) — stereo / phase-scope / spectrum ---

    for _name in ("sonare_metering_stereo_correlation", "sonare_metering_stereo_width"):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
        ]

    lib.sonare_metering_vectorscope.restype = ctypes.c_int32
    lib.sonare_metering_vectorscope.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareVectorscopeResult),
    ]

    lib.sonare_free_vectorscope_result.restype = None
    lib.sonare_free_vectorscope_result.argtypes = [ctypes.POINTER(SonareVectorscopeResult)]

    lib.sonare_metering_phase_scope.restype = ctypes.c_int32
    lib.sonare_metering_phase_scope.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonarePhaseScopeResult),
    ]

    lib.sonare_free_phase_scope_result.restype = None
    lib.sonare_free_phase_scope_result.argtypes = [ctypes.POINTER(SonarePhaseScopeResult)]

    lib.sonare_metering_spectrum.restype = ctypes.c_int32
    lib.sonare_metering_spectrum.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareSpectrumResult),
    ]

    lib.sonare_free_spectrum_result.restype = None
    lib.sonare_free_spectrum_result.argtypes = [ctypes.POINTER(SonareSpectrumResult)]

    # --- Editing - Scale quantizer ---

    for _name in ("sonare_scale_quantize_midi", "sonare_scale_correction_semitones"):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.c_int,
            ctypes.c_uint16,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(ctypes.c_float),
        ]

    lib.sonare_scale_pitch_class_enabled.restype = ctypes.c_int32
    lib.sonare_scale_pitch_class_enabled.argtypes = [
        ctypes.c_int,
        ctypes.c_uint16,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]

    # --- Core - Resample ---

    lib.sonare_resample.restype = ctypes.c_int32
    lib.sonare_resample.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
