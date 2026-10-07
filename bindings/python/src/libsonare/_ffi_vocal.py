"""ctypes function signatures for the monophonic vocal-edit ABI."""

from __future__ import annotations

import ctypes

from ._errors import ErrorCode, SonareError
from ._ffi_types_vocal import (
    SONARE_VOCAL_EDIT_API_VERSION,
    SonareVocalAnalysis,
    SonareVocalAnalysisResult,
    SonareVocalCancelCallback,
    SonareVocalCapabilities,
    SonareVocalCreateOptions,
    SonareVocalEditResult,
    SonareVocalErrorDetail,
    SonareVocalNoteEdit,
    SonareVocalNotesResult,
    SonareVocalOperation,
    SonareVocalPitchResult,
    SonareVocalRange,
    SonareVocalRenderResult,
    SonareVocalRestoreOptions,
    SonareVocalStateBytes,
    SonareVocalStateToken,
)


def configure_vocal_signatures(lib: ctypes.CDLL) -> None:
    """Attach argument and result types for all vocal-edit entry points.

    The aggregate ``_ffi.load_library`` routine calls this once after the
    general ABI check.  Keeping the declarations in a separate module makes
    the ctypes mirrors importable by the ABI-layout generator without loading
    a native library.
    """

    u32 = ctypes.c_uint32
    u64 = ctypes.c_uint64
    i64 = ctypes.c_int64
    error = ctypes.c_int32
    handle = ctypes.c_void_p

    lib.sonare_vocal_edit_api_version.restype = u32
    lib.sonare_vocal_edit_api_version.argtypes = []
    version = int(lib.sonare_vocal_edit_api_version())
    if version != SONARE_VOCAL_EDIT_API_VERSION:
        raise SonareError(
            int(ErrorCode.ABI_MISMATCH),
            f"libsonare ABI mismatch: native vocal-edit API reports {version}, "
            f"expected {SONARE_VOCAL_EDIT_API_VERSION}. The installed shared library is "
            "incompatible with this Python binding.",
        )

    for name, cls in (
        ("sonare_vocal_analysis_init", SonareVocalAnalysis),
        ("sonare_vocal_create_options_init", SonareVocalCreateOptions),
        ("sonare_vocal_restore_options_init", SonareVocalRestoreOptions),
        ("sonare_vocal_note_edit_init", SonareVocalNoteEdit),
        ("sonare_vocal_operation_init", SonareVocalOperation),
        ("sonare_vocal_notes_result_init", SonareVocalNotesResult),
        ("sonare_vocal_analysis_result_init", SonareVocalAnalysisResult),
        ("sonare_vocal_edit_result_init", SonareVocalEditResult),
        ("sonare_vocal_pitch_result_init", SonareVocalPitchResult),
        ("sonare_vocal_render_result_init", SonareVocalRenderResult),
        ("sonare_vocal_state_bytes_init", SonareVocalStateBytes),
        ("sonare_vocal_capabilities_init", SonareVocalCapabilities),
        ("sonare_vocal_error_detail_init", SonareVocalErrorDetail),
    ):
        fn = getattr(lib, name)
        fn.restype = None
        fn.argtypes = [ctypes.POINTER(cls)]

    lib.sonare_vocal_available.restype = ctypes.c_int
    lib.sonare_vocal_available.argtypes = []
    lib.sonare_vocal_last_error_detail.restype = None
    lib.sonare_vocal_last_error_detail.argtypes = [ctypes.POINTER(SonareVocalErrorDetail)]

    lib.sonare_vocal_session_create.restype = error
    lib.sonare_vocal_session_create.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        i64,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareVocalCreateOptions),
        ctypes.POINTER(handle),
    ]
    lib.sonare_vocal_session_restore.restype = error
    lib.sonare_vocal_session_restore.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        i64,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_uint8),
        u64,
        ctypes.POINTER(SonareVocalRestoreOptions),
        ctypes.POINTER(handle),
    ]
    lib.sonare_vocal_session_destroy.restype = None
    lib.sonare_vocal_session_destroy.argtypes = [handle]

    lib.sonare_vocal_session_notes.restype = error
    lib.sonare_vocal_session_notes.argtypes = [handle, ctypes.POINTER(SonareVocalNotesResult)]
    lib.sonare_vocal_draft_notes.restype = error
    lib.sonare_vocal_draft_notes.argtypes = [handle, ctypes.POINTER(SonareVocalNotesResult)]
    lib.sonare_vocal_session_token.restype = error
    lib.sonare_vocal_session_token.argtypes = [handle, ctypes.POINTER(SonareVocalStateToken)]
    lib.sonare_vocal_draft_token.restype = error
    lib.sonare_vocal_draft_token.argtypes = [handle, ctypes.POINTER(SonareVocalStateToken)]
    lib.sonare_vocal_session_analysis.restype = error
    lib.sonare_vocal_session_analysis.argtypes = [
        handle,
        ctypes.POINTER(SonareVocalAnalysisResult),
    ]
    lib.sonare_vocal_session_capabilities.restype = error
    lib.sonare_vocal_session_capabilities.argtypes = [
        handle,
        ctypes.POINTER(SonareVocalCapabilities),
    ]
    lib.sonare_vocal_session_revision.restype = error
    lib.sonare_vocal_session_revision.argtypes = [handle, ctypes.POINTER(u64)]
    lib.sonare_vocal_session_output_length.restype = error
    lib.sonare_vocal_session_output_length.argtypes = [handle, ctypes.POINTER(i64)]
    lib.sonare_vocal_session_history.restype = error
    lib.sonare_vocal_session_history.argtypes = [
        handle,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_vocal_session_begin_edit.restype = error
    lib.sonare_vocal_session_begin_edit.argtypes = [handle, u64, ctypes.POINTER(handle)]

    lib.sonare_vocal_draft_apply.restype = error
    lib.sonare_vocal_draft_apply.argtypes = [
        handle,
        u64,
        ctypes.POINTER(SonareVocalOperation),
        u64,
        ctypes.POINTER(SonareVocalEditResult),
    ]
    lib.sonare_vocal_draft_commit.restype = error
    lib.sonare_vocal_draft_commit.argtypes = [
        handle,
        u64,
        ctypes.POINTER(SonareVocalEditResult),
    ]
    lib.sonare_vocal_draft_cancel.restype = error
    lib.sonare_vocal_draft_cancel.argtypes = [handle]
    lib.sonare_vocal_draft_destroy.restype = None
    lib.sonare_vocal_draft_destroy.argtypes = [handle]

    for name, _fn_name in (
        ("sonare_vocal_session_undo", "session_undo"),
        ("sonare_vocal_session_redo", "session_redo"),
    ):
        fn = getattr(lib, name)
        fn.restype = error
        fn.argtypes = [handle, u64, ctypes.POINTER(SonareVocalEditResult)]

    for name in ("sonare_vocal_session_evaluate_pitch", "sonare_vocal_draft_evaluate_pitch"):
        fn = getattr(lib, name)
        fn.restype = error
        fn.argtypes = [handle, u32, ctypes.POINTER(SonareVocalPitchResult)]

    for name in (
        "sonare_vocal_session_source_to_destination",
        "sonare_vocal_session_destination_to_source",
        "sonare_vocal_draft_source_to_destination",
        "sonare_vocal_draft_destination_to_source",
    ):
        fn = getattr(lib, name)
        fn.restype = error
        fn.argtypes = [handle, u32, ctypes.c_double, ctypes.POINTER(ctypes.c_double)]

    lib.sonare_vocal_session_capture_snapshot.restype = error
    lib.sonare_vocal_session_capture_snapshot.argtypes = [handle, ctypes.POINTER(handle)]
    lib.sonare_vocal_draft_capture_snapshot.restype = error
    lib.sonare_vocal_draft_capture_snapshot.argtypes = [handle, ctypes.POINTER(handle)]
    lib.sonare_vocal_snapshot_destroy.restype = None
    lib.sonare_vocal_snapshot_destroy.argtypes = [handle]
    lib.sonare_vocal_snapshot_output_length.restype = error
    lib.sonare_vocal_snapshot_output_length.argtypes = [handle, ctypes.POINTER(i64)]

    cancel = SonareVocalCancelCallback
    lib.sonare_vocal_snapshot_render.restype = error
    lib.sonare_vocal_snapshot_render.argtypes = [
        handle,
        SonareVocalRange,
        u64,
        cancel,
        ctypes.c_void_p,
        ctypes.POINTER(SonareVocalRenderResult),
    ]
    lib.sonare_vocal_render_job_begin.restype = error
    lib.sonare_vocal_render_job_begin.argtypes = [
        handle,
        SonareVocalRange,
        u64,
        ctypes.POINTER(handle),
    ]
    lib.sonare_vocal_render_job_next.restype = error
    lib.sonare_vocal_render_job_next.argtypes = [
        handle,
        cancel,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_vocal_render_job_finalize.restype = error
    lib.sonare_vocal_render_job_finalize.argtypes = [
        handle,
        cancel,
        ctypes.c_void_p,
        ctypes.POINTER(SonareVocalRenderResult),
    ]
    lib.sonare_vocal_render_job_abort.restype = None
    lib.sonare_vocal_render_job_abort.argtypes = [handle]
    lib.sonare_vocal_render_job_destroy.restype = None
    lib.sonare_vocal_render_job_destroy.argtypes = [handle]

    lib.sonare_vocal_session_export_state.restype = error
    lib.sonare_vocal_session_export_state.argtypes = [
        handle,
        ctypes.POINTER(SonareVocalStateBytes),
    ]

    for name, cls in (
        ("sonare_vocal_free_notes", SonareVocalNotesResult),
        ("sonare_vocal_free_analysis", SonareVocalAnalysisResult),
        ("sonare_vocal_free_edit_result", SonareVocalEditResult),
        ("sonare_vocal_free_pitch_result", SonareVocalPitchResult),
        ("sonare_vocal_free_render_result", SonareVocalRenderResult),
        ("sonare_vocal_free_state_bytes", SonareVocalStateBytes),
    ):
        fn = getattr(lib, name)
        fn.restype = None
        fn.argtypes = [ctypes.POINTER(cls)]


__all__ = [
    "configure_vocal_signatures",
    *[name for name in globals() if name.startswith("SonareVocal")],
]
