"""ctypes signatures for the Project vocal-edit C ABI."""

from __future__ import annotations

import ctypes

from ._ffi_types_vocal import SonareVocalCancelCallback
from ._ffi_types_vocal_project import (
    SonareProjectVocalEditApplyDesc,
    SonareProjectVocalEditApplyResult,
    SonareProjectVocalEditDependenciesResult,
    SonareProjectVocalOriginalSource,
    SonareProjectVocalRehydrateResult,
)


def configure_vocal_project_signatures(lib: ctypes.CDLL) -> None:
    """Attach argument and result types for all Project vocal-edit symbols."""

    error = ctypes.c_int32
    handle = ctypes.c_void_p

    for name, cls in (
        (
            "sonare_project_vocal_edit_apply_desc_init",
            SonareProjectVocalEditApplyDesc,
        ),
        (
            "sonare_project_vocal_edit_apply_result_init",
            SonareProjectVocalEditApplyResult,
        ),
        (
            "sonare_project_vocal_edit_dependencies_result_init",
            SonareProjectVocalEditDependenciesResult,
        ),
        ("sonare_project_vocal_original_source_init", SonareProjectVocalOriginalSource),
        (
            "sonare_project_vocal_rehydrate_result_init",
            SonareProjectVocalRehydrateResult,
        ),
    ):
        fn = getattr(lib, name)
        fn.restype = None
        fn.argtypes = [ctypes.POINTER(cls)]

    lib.sonare_project_apply_vocal_edit.restype = error
    lib.sonare_project_apply_vocal_edit.argtypes = [
        handle,
        ctypes.POINTER(SonareProjectVocalEditApplyDesc),
        ctypes.POINTER(SonareProjectVocalEditApplyResult),
    ]
    lib.sonare_project_get_vocal_edit_dependencies.restype = error
    lib.sonare_project_get_vocal_edit_dependencies.argtypes = [
        handle,
        ctypes.POINTER(SonareProjectVocalEditDependenciesResult),
    ]
    lib.sonare_project_rehydrate_vocal_edits.restype = error
    lib.sonare_project_rehydrate_vocal_edits.argtypes = [
        handle,
        ctypes.POINTER(SonareProjectVocalOriginalSource),
        ctypes.c_uint64,
        SonareVocalCancelCallback,
        ctypes.c_void_p,
        ctypes.POINTER(SonareProjectVocalRehydrateResult),
    ]

    lib.sonare_project_free_vocal_edit_dependencies.restype = None
    lib.sonare_project_free_vocal_edit_dependencies.argtypes = [
        ctypes.POINTER(SonareProjectVocalEditDependenciesResult)
    ]
    lib.sonare_project_free_vocal_rehydrate_result.restype = None
    lib.sonare_project_free_vocal_rehydrate_result.argtypes = [
        ctypes.POINTER(SonareProjectVocalRehydrateResult)
    ]


__all__ = ["configure_vocal_project_signatures"]
