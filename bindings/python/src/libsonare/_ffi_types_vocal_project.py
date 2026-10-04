"""ctypes mirrors for the Project vocal-edit C ABI."""

from __future__ import annotations

import ctypes

from ._cstruct import CStruct
from ._ffi_types_vocal import SonareVocalStateToken

SONARE_VOCAL_PROJECT_API_VERSION = 1
SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY = 96

SONARE_VOCAL_REHYDRATE_REHYDRATED = 0
SONARE_VOCAL_REHYDRATE_ALREADY_READY = 1
SONARE_VOCAL_REHYDRATE_UNRESOLVED = 2


class SonareProjectVocalEditApplyDesc(CStruct):
    """Maps to ``SonareProjectVocalEditApplyDesc``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("clip_id", ctypes.c_uint32),
        ("take_id", ctypes.c_uint32),
        ("expected_source_id", ctypes.c_uint32),
        ("expected_source_sample_rate", ctypes.c_uint32),
        ("expected_source_sample_count", ctypes.c_int64),
        ("expected_source_sha256", ctypes.c_uint8 * 32),
        ("expected_clip_length_ppq", ctypes.c_double),
        ("expected_source_offset_ppq", ctypes.c_double),
        ("rendered_mono", ctypes.POINTER(ctypes.c_float)),
        ("rendered_sample_count", ctypes.c_int64),
        ("rendered_sample_rate", ctypes.c_uint32),
        ("rendered_start_sample", ctypes.c_int64),
        ("render_token", SonareVocalStateToken),
        ("sve1", ctypes.POINTER(ctypes.c_uint8)),
        ("sve1_size", ctypes.c_uint64),
    ]


class SonareProjectVocalEditApplyResult(CStruct):
    """Maps to ``SonareProjectVocalEditApplyResult``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("clip_id", ctypes.c_uint32),
        ("take_id", ctypes.c_uint32),
        ("original_source_id", ctypes.c_uint32),
        ("derived_source_id", ctypes.c_uint32),
        ("committed_revision", ctypes.c_uint64),
        ("profile_id", ctypes.c_uint32),
        ("derived_source_sha256", ctypes.c_uint8 * 32),
        ("sidecar_key", ctypes.c_char * SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY),
    ]


class SonareProjectVocalEditDependency(CStruct):
    """Maps to ``SonareProjectVocalEditDependency``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("clip_id", ctypes.c_uint32),
        ("take_id", ctypes.c_uint32),
        ("original_source_id", ctypes.c_uint32),
        ("derived_source_id", ctypes.c_uint32),
        ("source_sample_rate", ctypes.c_uint32),
        ("profile_id", ctypes.c_uint32),
        ("source_sample_count", ctypes.c_int64),
        ("committed_revision", ctypes.c_uint64),
        ("original_source_sha256", ctypes.c_uint8 * 32),
        ("derived_source_sha256", ctypes.c_uint8 * 32),
        ("original_pcm_available", ctypes.c_uint32),
        ("derived_pcm_available", ctypes.c_uint32),
        ("reason", ctypes.c_uint32),
        ("sidecar_key", ctypes.c_char * SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY),
    ]


class SonareProjectVocalEditDependenciesResult(CStruct):
    """Maps to ``SonareProjectVocalEditDependenciesResult``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("dependencies", ctypes.POINTER(SonareProjectVocalEditDependency)),
        ("dependency_count", ctypes.c_uint64),
    ]


class SonareProjectVocalOriginalSource(CStruct):
    """Maps to ``SonareProjectVocalOriginalSource``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("source_id", ctypes.c_uint32),
        ("mono", ctypes.POINTER(ctypes.c_float)),
        ("sample_count", ctypes.c_int64),
        ("sample_rate", ctypes.c_uint32),
    ]


class SonareProjectVocalRehydrateItem(CStruct):
    """Maps to ``SonareProjectVocalRehydrateItem``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("clip_id", ctypes.c_uint32),
        ("take_id", ctypes.c_uint32),
        ("derived_source_id", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("reason", ctypes.c_uint32),
    ]


class SonareProjectVocalRehydrateResult(CStruct):
    """Maps to ``SonareProjectVocalRehydrateResult``."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("items", ctypes.POINTER(SonareProjectVocalRehydrateItem)),
        ("item_count", ctypes.c_uint64),
    ]


__all__ = [
    "SONARE_VOCAL_PROJECT_API_VERSION",
    "SONARE_VOCAL_PROJECT_SIDECAR_KEY_CAPACITY",
    "SONARE_VOCAL_REHYDRATE_REHYDRATED",
    "SONARE_VOCAL_REHYDRATE_ALREADY_READY",
    "SONARE_VOCAL_REHYDRATE_UNRESOLVED",
    "SonareProjectVocalEditApplyDesc",
    "SonareProjectVocalEditApplyResult",
    "SonareProjectVocalEditDependency",
    "SonareProjectVocalEditDependenciesResult",
    "SonareProjectVocalOriginalSource",
    "SonareProjectVocalRehydrateItem",
    "SonareProjectVocalRehydrateResult",
]
