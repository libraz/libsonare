from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from enum import IntEnum
from typing import Any, TypeAlias

import numpy as np
from numpy.typing import NDArray

from ._errors import SonareError

class VocalTargetMode(IntEnum):
    NONE: int
    CENTER: int
    CURVE: int

class VocalFormantMode(IntEnum):
    PRESERVE: int
    SHIFT: int

class VocalOperationKind(IntEnum):
    SET_EDIT: int
    SET_SOURCE_SPAN: int
    SPLIT: int
    MERGE: int
    SET_TRANSITION: int
    REMOVE_TRANSITION: int
    RESET: int

class VocalMergePolicy(IntEnum):
    PRESERVE: int
    RESET: int

class VocalRange:
    start_sample: int
    end_sample: int
    def __init__(self, start_sample: int, end_sample: int) -> None: ...
    @property
    def length(self) -> int: ...

class VocalPitchPoint:
    source_sample: float
    target_midi: float
    def __init__(self, source_sample: float, target_midi: float) -> None: ...
    @property
    def midi(self) -> float: ...

class VocalNoteEdit:
    target_mode: VocalTargetMode | int
    target_midi: float
    target_points: tuple[VocalPitchPoint, ...]
    amount: float
    speed_ms: float
    max_correction_semitones: float
    transpose_semitones: float
    drift_scale: float
    vibrato_scale: float
    destination_start_sample: int
    destination_length_samples: int
    gain_db: float
    muted: bool
    formant_mode: VocalFormantMode | int
    formant_shift_semitones: float
    amplitude_envelope: tuple[float, ...]
    def __init__(
        self,
        target_mode: VocalTargetMode | int = ...,
        target_midi: float = ...,
        target_points: Sequence[VocalPitchPoint] = ...,
        amount: float = ...,
        speed_ms: float = ...,
        max_correction_semitones: float = ...,
        transpose_semitones: float = ...,
        drift_scale: float = ...,
        vibrato_scale: float = ...,
        destination_start_sample: int = ...,
        destination_length_samples: int = ...,
        gain_db: float = ...,
        muted: bool = ...,
        formant_mode: VocalFormantMode | int = ...,
        formant_shift_semitones: float = ...,
        amplitude_envelope: Sequence[float] = ...,
    ) -> None: ...

class VocalNote:
    id: int
    source_start_sample: int
    source_end_sample: int
    analysis_frame_start: int
    analysis_frame_end: int
    has_pitch: bool
    median_hz: float
    center_midi: float
    f0_stability: float
    amplitude: NDArray[np.float32]
    edit: VocalNoteEdit
    def __init__(
        self,
        id: int,
        source_start_sample: int,
        source_end_sample: int,
        analysis_frame_start: int,
        analysis_frame_end: int,
        has_pitch: bool,
        median_hz: float,
        center_midi: float,
        f0_stability: float,
        amplitude: NDArray[np.float32],
        edit: VocalNoteEdit,
    ) -> None: ...
    @property
    def source_range(self) -> VocalRange: ...

class VocalTransition:
    left_note_id: int
    right_note_id: int
    left_window_samples: int
    right_window_samples: int
    strength: float
    def __init__(
        self,
        left_note_id: int,
        right_note_id: int,
        left_window_samples: int,
        right_window_samples: int,
        strength: float,
    ) -> None: ...

class VocalStateToken:
    session_epoch: int
    revision: int
    draft_id: int
    generation: int
    request_id: int
    profile_id: int
    def __init__(
        self,
        session_epoch: int,
        revision: int,
        draft_id: int,
        generation: int,
        request_id: int = ...,
        profile_id: int = ...,
    ) -> None: ...
    @property
    def committed_revision(self) -> int: ...

class VocalAnalysis:
    frame_origin_sample: float
    samples_per_frame: float
    frame_length_samples: int
    f0_hz: NDArray[np.float32]
    voiced: NDArray[np.uint8]
    algorithm_id: str
    algorithm_version: int
    fmin_hz: float
    fmax_hz: float
    yin_threshold: float
    voiced_threshold: float
    centered: bool
    segmentation_threshold_cents: float
    min_note_ms: float
    reference_hz: float
    def __init__(
        self,
        frame_origin_sample: float,
        samples_per_frame: float,
        frame_length_samples: int,
        f0_hz: NDArray[np.float32],
        voiced: NDArray[np.uint8],
        algorithm_id: str,
        algorithm_version: int,
        fmin_hz: float = ...,
        fmax_hz: float = ...,
        yin_threshold: float = ...,
        voiced_threshold: float = ...,
        centered: bool = ...,
        segmentation_threshold_cents: float = ...,
        min_note_ms: float = ...,
        reference_hz: float = ...,
    ) -> None: ...

class VocalAnalysisResult:
    analysis: VocalAnalysis
    source_sha256: str
    analysis_sha256: str
    source_length_samples: int
    sample_rate: int
    def __init__(
        self,
        analysis: VocalAnalysis,
        source_sha256: str,
        analysis_sha256: str,
        source_length_samples: int,
        sample_rate: int,
    ) -> None: ...

class VocalCapabilities:
    api_version: int
    profile_id: int
    monophonic_only: bool
    analysis_cancellable: bool
    minimum_formant_shift_semitones: float
    maximum_formant_shift_semitones: float
    def __init__(
        self,
        api_version: int,
        profile_id: int,
        monophonic_only: bool,
        analysis_cancellable: bool,
        minimum_formant_shift_semitones: float,
        maximum_formant_shift_semitones: float,
    ) -> None: ...

class VocalIdChange:
    operation_index: int
    retired_id: int
    new_ids: tuple[int, ...]
    def __init__(self, operation_index: int, retired_id: int, new_ids: Sequence[int]) -> None: ...

class VocalEditResult:
    token: VocalStateToken
    dirty_ranges: tuple[VocalRange, ...]
    id_changes: tuple[VocalIdChange, ...]
    def __init__(
        self,
        token: VocalStateToken,
        dirty_ranges: Sequence[VocalRange],
        id_changes: Sequence[VocalIdChange] = ...,
    ) -> None: ...

class VocalPitchResult:
    source_samples: NDArray[np.float64]
    measured_midi: NDArray[np.float64]
    target_midi: NDArray[np.float64]
    effective_midi: NDArray[np.float64]
    voiced: NDArray[np.uint8]
    has_target: NDArray[np.uint8]
    def __init__(
        self,
        source_samples: NDArray[np.float64],
        measured_midi: NDArray[np.float64],
        target_midi: NDArray[np.float64],
        effective_midi: NDArray[np.float64],
        voiced: NDArray[np.uint8],
        has_target: NDArray[np.uint8],
    ) -> None: ...

class VocalRenderResult:
    samples: NDArray[np.float32]
    start_sample: int
    token: VocalStateToken
    processed_ranges: tuple[VocalRange, ...]
    cache_hit_units: int
    dry_passed_frames: int
    limited_correction_frames: int
    def __init__(
        self,
        samples: NDArray[np.float32],
        start_sample: int,
        token: VocalStateToken,
        processed_ranges: Sequence[VocalRange],
        cache_hit_units: int,
        dry_passed_frames: int,
        limited_correction_frames: int,
    ) -> None: ...

class VocalCreateOptions:
    output_length_samples: int | None
    edge_fade_ms: float | None
    vibrato_cutoff_hz: float | None
    segmentation_threshold_cents: float | None
    min_note_ms: float | None
    frame_length_samples: int | None
    hop_length_samples: int | None
    fmin_hz: float | None
    fmax_hz: float | None
    max_history_bytes: int | None
    max_cache_bytes: int | None
    max_undo_depth: int | None
    max_render_jobs: int | None
    analysis: VocalAnalysis | None
    yin_threshold: float | None
    voiced_threshold: float | None
    centered: bool | None
    reference_hz: float | None
    def __init__(
        self,
        output_length_samples: int | None = ...,
        edge_fade_ms: float | None = ...,
        vibrato_cutoff_hz: float | None = ...,
        segmentation_threshold_cents: float | None = ...,
        min_note_ms: float | None = ...,
        frame_length_samples: int | None = ...,
        hop_length_samples: int | None = ...,
        fmin_hz: float | None = ...,
        fmax_hz: float | None = ...,
        max_history_bytes: int | None = ...,
        max_cache_bytes: int | None = ...,
        max_undo_depth: int | None = ...,
        max_render_jobs: int | None = ...,
        analysis: VocalAnalysis | None = ...,
        yin_threshold: float | None = ...,
        voiced_threshold: float | None = ...,
        centered: bool | None = ...,
        reference_hz: float | None = ...,
    ) -> None: ...

class VocalSetNoteEdit:
    note_id: int
    edit: VocalNoteEdit
    def __init__(self, note_id: int, edit: VocalNoteEdit) -> None: ...

class VocalSetNoteSourceSpan:
    note_id: int
    source_start_sample: int
    source_end_sample: int
    destination_start_sample: int
    destination_length_samples: int
    def __init__(
        self,
        note_id: int,
        source_start_sample: int,
        source_end_sample: int,
        destination_start_sample: int = ...,
        destination_length_samples: int = ...,
    ) -> None: ...

class VocalSplitNote:
    note_id: int
    cut_source_sample: int
    def __init__(self, note_id: int, cut_source_sample: int) -> None: ...

class VocalMergeNotes:
    note_ids: tuple[int, ...]
    merge_policy: VocalMergePolicy | int
    def __init__(
        self, note_ids: Sequence[int], merge_policy: VocalMergePolicy | int = ...
    ) -> None: ...

class VocalSetTransition:
    transition: VocalTransition
    def __init__(self, transition: VocalTransition) -> None: ...

class VocalRemoveTransition:
    left_note_id: int
    right_note_id: int
    def __init__(self, left_note_id: int, right_note_id: int) -> None: ...

class VocalResetNotes:
    note_ids: tuple[int, ...]
    def __init__(self, note_ids: Sequence[int]) -> None: ...

VocalOperation: TypeAlias = (
    VocalSetNoteEdit
    | VocalSetNoteSourceSpan
    | VocalSplitNote
    | VocalMergeNotes
    | VocalSetTransition
    | VocalRemoveTransition
    | VocalResetNotes
)

class VocalEditError(SonareError):
    reason: int
    field: str
    expected: int
    actual: int
    expected_text: str
    actual_text: str

class VocalEditSession:
    def __init__(
        self,
        samples: Sequence[float] | NDArray[Any],
        sample_rate: int,
        *,
        options: VocalCreateOptions | None = ...,
        analysis: VocalAnalysis | None = ...,
        yin_threshold: float | None = ...,
        voiced_threshold: float | None = ...,
        centered: bool | None = ...,
        reference_hz: float | None = ...,
        output_length_samples: int | None = ...,
        **option_overrides: Any,
    ) -> None: ...
    @classmethod
    def restore(
        cls,
        samples: Sequence[float] | NDArray[Any],
        sample_rate: int,
        state: bytes | bytearray | memoryview,
    ) -> VocalEditSession: ...
    def __enter__(self) -> VocalEditSession: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None: ...
    def notes(self) -> tuple[tuple[VocalNote, ...], tuple[VocalTransition, ...]]: ...
    def analysis(self) -> VocalAnalysisResult: ...
    def capabilities(self) -> VocalCapabilities: ...
    def token(self) -> VocalStateToken: ...
    @property
    def revision(self) -> int: ...
    @property
    def output_length_samples(self) -> int: ...
    def history(self) -> tuple[bool, bool]: ...
    @property
    def can_undo(self) -> bool: ...
    @property
    def can_redo(self) -> bool: ...
    def begin_edit(self, expected_revision: int | None = ...) -> VocalEditDraft: ...
    def undo(self, expected_revision: int | None = ...) -> VocalEditResult: ...
    def redo(self, expected_revision: int | None = ...) -> VocalEditResult: ...
    def evaluate_pitch(self, note_id: int) -> VocalPitchResult: ...
    def source_sample_to_destination_sample(self, note_id: int, source_sample: float) -> float: ...
    def destination_sample_to_source_sample(
        self, note_id: int, destination_sample: float
    ) -> float: ...
    def capture_render_snapshot(self) -> VocalRenderSnapshot: ...
    def export_state(self) -> bytes: ...
    def render(
        self,
        range: VocalRange | Sequence[int] | None = ...,
        *,
        request_id: int = ...,
        cancel: Callable[[], bool] | None = ...,
    ) -> VocalRenderResult: ...

class VocalEditDraft:
    def __enter__(self) -> VocalEditDraft: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None: ...
    def notes(self) -> tuple[tuple[VocalNote, ...], tuple[VocalTransition, ...]]: ...
    def token(self) -> VocalStateToken: ...
    def apply(
        self,
        operations: Iterable[VocalOperation],
        expected_generation: int | None = ...,
    ) -> VocalEditResult: ...
    def commit(self, expected_revision: int | None = ...) -> VocalEditResult: ...
    def cancel(self) -> None: ...
    def evaluate_pitch(self, note_id: int) -> VocalPitchResult: ...
    def source_sample_to_destination_sample(self, note_id: int, source_sample: float) -> float: ...
    def destination_sample_to_source_sample(
        self, note_id: int, destination_sample: float
    ) -> float: ...
    def capture_render_snapshot(self) -> VocalRenderSnapshot: ...

class VocalRenderSnapshot:
    def __enter__(self) -> VocalRenderSnapshot: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None: ...
    @property
    def output_length_samples(self) -> int: ...
    def render(
        self,
        range: VocalRange | Sequence[int] | None = ...,
        *,
        request_id: int = ...,
        cancel: Callable[[], bool] | None = ...,
    ) -> VocalRenderResult: ...
    def begin_render_job(
        self,
        range: VocalRange | Sequence[int] | None = ...,
        *,
        request_id: int = ...,
    ) -> VocalRenderJob: ...

class VocalRenderJob:
    def __enter__(self) -> VocalRenderJob: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None: ...
    def abort(self) -> None: ...
    def next(self, cancel: Callable[[], bool] | None = ...) -> bool: ...
    def finalize(self, cancel: Callable[[], bool] | None = ...) -> VocalRenderResult: ...

def vocal_edit_available() -> bool: ...
def create_vocal_edit_session(
    samples: Sequence[float] | NDArray[Any],
    sample_rate: int,
    *,
    options: VocalCreateOptions | None = ...,
    analysis: VocalAnalysis | None = ...,
    output_length_samples: int | None = ...,
    **option_overrides: Any,
) -> VocalEditSession: ...
def restore_vocal_edit_session(
    samples: Sequence[float] | NDArray[Any],
    sample_rate: int,
    state: bytes | bytearray | memoryview,
) -> VocalEditSession: ...
