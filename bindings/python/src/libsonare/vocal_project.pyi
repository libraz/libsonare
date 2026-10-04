from __future__ import annotations

from collections.abc import Callable, Sequence
from enum import IntEnum
from typing import Any

from numpy.typing import NDArray

from ._project import Project
from .vocal_edit import VocalStateToken

class ProjectVocalRehydrateStatus(IntEnum):
    REHYDRATED: int
    ALREADY_READY: int
    UNRESOLVED: int

class ProjectVocalReason(IntEnum):
    NONE: int
    INVALID_INPUT: int
    REVISION_CONFLICT: int
    SOURCE_MISMATCH: int
    UNSUPPORTED: int
    CANCELLED: int
    COUNTER_EXHAUSTED: int
    INVALID_STATE: int

class ProjectVocalOriginalSource:
    source_id: int
    mono: Sequence[float] | NDArray[Any]
    sample_rate: int
    def __init__(
        self,
        source_id: int,
        mono: Sequence[float] | NDArray[Any],
        sample_rate: int,
    ) -> None: ...

class ProjectVocalEditApplyResult:
    clip_id: int
    take_id: int
    original_source_id: int
    derived_source_id: int
    committed_revision: int
    profile_id: int
    derived_source_sha256: bytes
    sidecar_key: str
    def __init__(
        self,
        clip_id: int,
        take_id: int,
        original_source_id: int,
        derived_source_id: int,
        committed_revision: int,
        profile_id: int,
        derived_source_sha256: bytes,
        sidecar_key: str,
    ) -> None: ...

class ProjectVocalEditDependency:
    clip_id: int
    take_id: int
    original_source_id: int
    derived_source_id: int
    source_sample_rate: int
    profile_id: int
    source_sample_count: int
    committed_revision: int
    original_source_sha256: bytes
    derived_source_sha256: bytes
    original_pcm_available: bool
    derived_pcm_available: bool
    reason: ProjectVocalReason | int
    sidecar_key: str
    def __init__(
        self,
        clip_id: int,
        take_id: int,
        original_source_id: int,
        derived_source_id: int,
        source_sample_rate: int,
        profile_id: int,
        source_sample_count: int,
        committed_revision: int,
        original_source_sha256: bytes,
        derived_source_sha256: bytes,
        original_pcm_available: bool,
        derived_pcm_available: bool,
        reason: ProjectVocalReason | int,
        sidecar_key: str,
    ) -> None: ...

class ProjectVocalRehydrateItem:
    clip_id: int
    take_id: int
    derived_source_id: int
    status: ProjectVocalRehydrateStatus | int
    reason: ProjectVocalReason | int
    def __init__(
        self,
        clip_id: int,
        take_id: int,
        derived_source_id: int,
        status: ProjectVocalRehydrateStatus | int,
        reason: ProjectVocalReason | int,
    ) -> None: ...

def apply_project_vocal_edit(
    project: Project,
    *,
    clip_id: int,
    expected_source_id: int,
    expected_source_sample_rate: int,
    expected_source_sample_count: int,
    expected_source_sha256: str,
    expected_clip_length_ppq: float,
    expected_source_offset_ppq: float,
    rendered_mono: Sequence[float] | NDArray[Any],
    rendered_sample_rate: int,
    render_token: VocalStateToken,
    sve1: bytes | bytearray | memoryview,
    take_id: int = ...,
    rendered_start_sample: int = ...,
) -> ProjectVocalEditApplyResult: ...
def get_project_vocal_edit_dependencies(
    project: Project,
) -> tuple[ProjectVocalEditDependency, ...]: ...
def rehydrate_project_vocal_edits(
    project: Project,
    original_sources: Sequence[ProjectVocalOriginalSource],
    *,
    cancel: Callable[[], bool] | None = ...,
) -> tuple[ProjectVocalRehydrateItem, ...]: ...
