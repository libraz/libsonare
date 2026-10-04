"""ctypes mirrors for the versioned monophonic vocal-edit C ABI.

The C header deliberately keeps all arrays as borrowed pointers.  The public
facade therefore owns the temporary ctypes/NumPy buffers used for input and
copies every returned buffer before calling one of the C ``free_*`` functions.
"""

from __future__ import annotations

import ctypes

from ._cstruct import CStruct

SONARE_VOCAL_EDIT_API_VERSION = 1


class SonareVocalPitchPoint(CStruct):
    _fields_ = [
        ("source_sample", ctypes.c_double),
        ("midi", ctypes.c_double),
    ]


class SonareVocalRange(CStruct):
    _fields_ = [
        ("start_sample", ctypes.c_int64),
        ("end_sample", ctypes.c_int64),
    ]


class SonareVocalAnalysis(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("frame_origin_sample", ctypes.c_double),
        ("samples_per_frame", ctypes.c_double),
        ("frame_length_samples", ctypes.c_uint32),
        ("f0_hz", ctypes.POINTER(ctypes.c_float)),
        ("voiced", ctypes.POINTER(ctypes.c_uint8)),
        ("frame_count", ctypes.c_uint64),
        ("algorithm_id", ctypes.c_char_p),
        ("algorithm_version", ctypes.c_uint32),
        ("fmin_hz", ctypes.c_double),
        ("fmax_hz", ctypes.c_double),
        ("yin_threshold", ctypes.c_double),
        ("voiced_threshold", ctypes.c_double),
        ("centered", ctypes.c_uint32),
        ("segmentation_threshold_cents", ctypes.c_double),
        ("min_note_ms", ctypes.c_double),
        ("reference_hz", ctypes.c_double),
    ]


class SonareVocalCreateOptions(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("output_length_samples", ctypes.c_int64),
        ("edge_fade_ms", ctypes.c_double),
        ("vibrato_cutoff_hz", ctypes.c_double),
        ("segmentation_threshold_cents", ctypes.c_double),
        ("min_note_ms", ctypes.c_double),
        ("frame_length_samples", ctypes.c_uint32),
        ("hop_length_samples", ctypes.c_uint32),
        ("fmin_hz", ctypes.c_double),
        ("fmax_hz", ctypes.c_double),
        ("max_history_bytes", ctypes.c_uint64),
        ("max_cache_bytes", ctypes.c_uint64),
        ("max_undo_depth", ctypes.c_uint32),
        ("max_render_jobs", ctypes.c_uint32),
        ("analysis", ctypes.POINTER(SonareVocalAnalysis)),
        ("yin_threshold", ctypes.c_double),
        ("voiced_threshold", ctypes.c_double),
        ("centered", ctypes.c_uint32),
        ("reference_hz", ctypes.c_double),
    ]


class SonareVocalNoteEdit(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("target_mode", ctypes.c_uint32),
        ("target_midi", ctypes.c_double),
        ("target_points", ctypes.POINTER(SonareVocalPitchPoint)),
        ("target_point_count", ctypes.c_uint64),
        ("amount", ctypes.c_double),
        ("speed_ms", ctypes.c_double),
        ("max_correction_semitones", ctypes.c_double),
        ("transpose_semitones", ctypes.c_double),
        ("drift_scale", ctypes.c_double),
        ("vibrato_scale", ctypes.c_double),
        ("destination_start_sample", ctypes.c_int64),
        ("destination_length_samples", ctypes.c_int64),
        ("gain_db", ctypes.c_double),
        ("muted", ctypes.c_uint32),
        ("formant_mode", ctypes.c_uint32),
        ("formant_shift_semitones", ctypes.c_double),
        ("amplitude_envelope", ctypes.POINTER(ctypes.c_float)),
        ("amplitude_envelope_count", ctypes.c_uint64),
    ]


class SonareVocalNote(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("id", ctypes.c_uint32),
        ("source_start_sample", ctypes.c_int64),
        ("source_end_sample", ctypes.c_int64),
        ("analysis_frame_start", ctypes.c_uint64),
        ("analysis_frame_end", ctypes.c_uint64),
        ("has_pitch", ctypes.c_uint32),
        ("median_hz", ctypes.c_double),
        ("center_midi", ctypes.c_double),
        ("f0_stability", ctypes.c_double),
        ("amplitude", ctypes.POINTER(ctypes.c_float)),
        ("amplitude_count", ctypes.c_uint64),
        ("edit", SonareVocalNoteEdit),
    ]


class SonareVocalTransition(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("left_note_id", ctypes.c_uint32),
        ("right_note_id", ctypes.c_uint32),
        ("left_window_samples", ctypes.c_int64),
        ("right_window_samples", ctypes.c_int64),
        ("strength", ctypes.c_double),
    ]


class SonareVocalOperation(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("kind", ctypes.c_uint32),
        ("note_id", ctypes.c_uint32),
        ("edit", SonareVocalNoteEdit),
        ("source_start_sample", ctypes.c_int64),
        ("source_end_sample", ctypes.c_int64),
        ("destination_start_sample", ctypes.c_int64),
        ("destination_length_samples", ctypes.c_int64),
        ("cut_source_sample", ctypes.c_int64),
        ("note_ids", ctypes.POINTER(ctypes.c_uint32)),
        ("note_id_count", ctypes.c_uint64),
        ("merge_policy", ctypes.c_uint32),
        ("transition", SonareVocalTransition),
    ]


class SonareVocalStateToken(CStruct):
    _fields_ = [
        ("session_epoch", ctypes.c_uint64),
        ("revision", ctypes.c_uint64),
        ("draft_id", ctypes.c_uint64),
        ("generation", ctypes.c_uint64),
        ("request_id", ctypes.c_uint64),
        ("profile_id", ctypes.c_uint32),
    ]


class SonareVocalNotesResult(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("notes", ctypes.POINTER(SonareVocalNote)),
        ("note_count", ctypes.c_uint64),
        ("transitions", ctypes.POINTER(SonareVocalTransition)),
        ("transition_count", ctypes.c_uint64),
    ]


class SonareVocalAnalysisResult(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("analysis", SonareVocalAnalysis),
        ("source_sha256", ctypes.c_char * 65),
        ("analysis_sha256", ctypes.c_char * 65),
        ("source_length_samples", ctypes.c_int64),
        ("sample_rate", ctypes.c_uint32),
    ]


class SonareVocalIdChange(CStruct):
    _fields_ = [
        ("operation_index", ctypes.c_uint32),
        ("retired_id", ctypes.c_uint32),
        ("first_new_id", ctypes.c_uint32),
        ("second_new_id", ctypes.c_uint32),
    ]


class SonareVocalEditResult(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("token", SonareVocalStateToken),
        ("dirty_ranges", ctypes.POINTER(SonareVocalRange)),
        ("dirty_range_count", ctypes.c_uint64),
        ("id_changes", ctypes.POINTER(SonareVocalIdChange)),
        ("id_change_count", ctypes.c_uint64),
    ]


class SonareVocalPitchResult(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("source_samples", ctypes.POINTER(ctypes.c_double)),
        ("measured_midi", ctypes.POINTER(ctypes.c_double)),
        ("target_midi", ctypes.POINTER(ctypes.c_double)),
        ("effective_midi", ctypes.POINTER(ctypes.c_double)),
        ("voiced", ctypes.POINTER(ctypes.c_uint8)),
        ("has_target", ctypes.POINTER(ctypes.c_uint8)),
        ("frame_count", ctypes.c_uint64),
    ]


class SonareVocalRenderResult(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("samples", ctypes.POINTER(ctypes.c_float)),
        ("sample_count", ctypes.c_int64),
        ("start_sample", ctypes.c_int64),
        ("token", SonareVocalStateToken),
        ("processed_ranges", ctypes.POINTER(SonareVocalRange)),
        ("processed_range_count", ctypes.c_uint64),
        ("cache_hit_units", ctypes.c_uint64),
        ("dry_passed_frames", ctypes.c_uint64),
        ("limited_correction_frames", ctypes.c_uint64),
    ]


class SonareVocalStateBytes(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_uint64),
    ]


class SonareVocalCapabilities(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("profile_id", ctypes.c_uint32),
        ("monophonic_only", ctypes.c_uint32),
        ("analysis_cancellable", ctypes.c_uint32),
        ("minimum_formant_shift_semitones", ctypes.c_double),
        ("maximum_formant_shift_semitones", ctypes.c_double),
    ]


class SonareVocalErrorDetail(CStruct):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("reason", ctypes.c_uint32),
        ("field", ctypes.c_char * 96),
        ("expected", ctypes.c_uint64),
        ("actual", ctypes.c_uint64),
        ("expected_text", ctypes.c_char * 96),
        ("actual_text", ctypes.c_char * 96),
    ]


SonareVocalCancelCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


__all__ = [name for name in globals() if name.startswith("SonareVocal")]
