from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from typing import Any, Literal, TypeAlias

import numpy as np
from numpy.typing import NDArray

from ._errors import SonareError

VocalTargetMode: TypeAlias = Literal["none", "center", "curve"]
VocalFormantMode: TypeAlias = Literal["preserve", "shift"]
VocalMergePolicy: TypeAlias = Literal["preserve", "reset"]
VocalReason: TypeAlias = Literal[
    "none",
    "invalidInput",
    "revisionConflict",
    "sourceMismatch",
    "unsupported",
    "cancelled",
    "counterExhausted",
    "invalidState",
]

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
    target_mode: VocalTargetMode
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
    formant_mode: VocalFormantMode
    formant_shift_semitones: float
    amplitude_envelope: tuple[float, ...]
    def __init__(
        self,
        target_mode: VocalTargetMode = ...,
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
        formant_mode: VocalFormantMode = ...,
        formant_shift_semitones: float = ...,
        amplitude_envelope: Sequence[float] = ...,
    ) -> None: ...

class VocalNote:
    id: int
    source_start_sample: int
    source_end_sample: int
    analysis_frame_start: int
    analysis_frame_end: int
    median_hz: float | None
    center_midi: float | None
    f0_stability: float | None
    amplitude: NDArray[np.float32]
    edit: VocalNoteEdit
    def __init__(
        self,
        id: int,
        source_start_sample: int,
        source_end_sample: int,
        analysis_frame_start: int,
        analysis_frame_end: int,
        median_hz: float | None,
        center_midi: float | None,
        f0_stability: float | None,
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

class VocalNotesResult:
    notes: tuple[VocalNote, ...]
    transitions: tuple[VocalTransition, ...]
    def __init__(
        self,
        notes: tuple[VocalNote, ...],
        transitions: tuple[VocalTransition, ...],
    ) -> None: ...

class VocalHistory:
    can_undo: bool
    can_redo: bool
    def __init__(self, can_undo: bool, can_redo: bool) -> None: ...

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
    abi_version: int
    profile_id: int
    monophonic_only: bool
    analysis_cancellable: bool
    minimum_formant_shift_semitones: float
    maximum_formant_shift_semitones: float
    def __init__(
        self,
        abi_version: int,
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

class VocalSetNoteEdit:
    note_id: int
    edit: VocalNoteEdit
    def __init__(self, note_id: int, edit: VocalNoteEdit) -> None: ...

class VocalSetNoteSourceSpan:
    note_id: int
    source_start_sample: int | None
    source_end_sample: int | None
    destination_start_sample: int | None
    destination_length_samples: int | None
    source_start_sec: float | None
    source_end_sec: float | None
    destination_start_sec: float | None
    destination_length_sec: float | None
    def __init__(
        self,
        note_id: int,
        source_start_sample: int | None = ...,
        source_end_sample: int | None = ...,
        destination_start_sample: int | None = ...,
        destination_length_samples: int | None = ...,
        source_start_sec: float | None = ...,
        source_end_sec: float | None = ...,
        destination_start_sec: float | None = ...,
        destination_length_sec: float | None = ...,
    ) -> None: ...

class VocalSplitNote:
    note_id: int
    cut_source_sample: int | None
    cut_source_sec: float | None
    def __init__(
        self,
        note_id: int,
        cut_source_sample: int | None = ...,
        cut_source_sec: float | None = ...,
    ) -> None: ...

class VocalMergeNotes:
    note_ids: tuple[int, ...]
    merge_policy: VocalMergePolicy
    def __init__(self, note_ids: Sequence[int], merge_policy: VocalMergePolicy = ...) -> None: ...

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
    reason: VocalReason
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
        analysis: VocalAnalysis | None = ...,
        output_length_samples: int | None = ...,
        edge_fade_ms: float | None = ...,
        vibrato_cutoff_hz: float | None = ...,
        segmentation_threshold_cents: float | None = ...,
        min_note_ms: float | None = ...,
        frame_length_samples: int | None = ...,
        hop_length_samples: int | None = ...,
        fmin_hz: float | None = ...,
        fmax_hz: float | None = ...,
        yin_threshold: float | None = ...,
        voiced_threshold: float | None = ...,
        centered: bool | None = ...,
        reference_hz: float | None = ...,
        max_history_bytes: int | None = ...,
        max_cache_bytes: int | None = ...,
        max_undo_depth: int | None = ...,
        max_render_jobs: int | None = ...,
    ) -> None: ...
    @classmethod
    def restore(
        cls,
        samples: Sequence[float] | NDArray[Any],
        sample_rate: int,
        state: bytes | bytearray | memoryview,
        *,
        max_history_bytes: int | None = ...,
        max_cache_bytes: int | None = ...,
        max_undo_depth: int | None = ...,
        max_render_jobs: int | None = ...,
    ) -> VocalEditSession: ...
    def __enter__(self) -> VocalEditSession: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None:
        """Release the native session; safe to call twice."""
    def notes(self) -> VocalNotesResult:
        """Notes and transitions of the committed state; SonareError (InvalidState) once closed."""
    def analysis(self) -> VocalAnalysisResult:
        """The pitch analysis the session was built from."""
    def capabilities(self) -> VocalCapabilities:
        """What the loaded engine supports."""
    def token(self) -> VocalStateToken:
        """State token of the committed state."""
    @property
    def revision(self) -> int:
        """Counter of committed edits."""
    @property
    def output_length_samples(self) -> int:
        """Length of the output in samples."""
    def history(self) -> VocalHistory:
        """Whether an undo or a redo is available."""
    @property
    def can_undo(self) -> bool:
        """True when an edit can be undone."""
    @property
    def can_redo(self) -> bool:
        """True when an undone edit can be redone."""
    def begin_edit(self, expected_revision: int | None = ...) -> VocalEditDraft:
        """Open a draft; ``expected_revision`` None means the current revision.

        Raises VocalEditError on a stale revision.
        """
    def undo(self, expected_revision: int | None = ...) -> VocalEditResult:
        """Undo the last committed edit; ``expected_revision`` None means the current revision.

        Raises VocalEditError when refused.
        """
    def redo(self, expected_revision: int | None = ...) -> VocalEditResult:
        """Redo the last undone edit; ``expected_revision`` None means the current revision.

        Raises VocalEditError when refused.
        """
    def evaluate_pitch(self, note_id: int) -> VocalPitchResult:
        """Measured, target and effective pitch per frame of one note."""
    def source_sample_to_destination_sample(self, note_id: int, source_sample: float) -> float:
        """Map a source position of a note to the output, in samples."""
    def destination_sample_to_source_sample(self, note_id: int, destination_sample: float) -> float:
        """Map an output position of a note back to the source, in samples."""
    def capture_render_snapshot(self) -> VocalRenderSnapshot:
        """Capture the committed state for rendering."""
    def export_state(self) -> bytes:
        """Serialise the session state to bytes that ``restore`` accepts."""
    def render(
        self,
        range: VocalRange | Sequence[int | None] | None = ...,
        *,
        start_sec: float | None = ...,
        end_sec: float | None = ...,
        request_id: int = ...,
        cancel: Callable[[], bool] | None = ...,
    ) -> VocalRenderResult: ...

class VocalEditDraft:
    def __enter__(self) -> VocalEditDraft: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None:
        """Release the native draft; safe to call twice."""
    def notes(self) -> VocalNotesResult:
        """Notes and transitions of the draft; SonareError (InvalidState) once closed."""
    def token(self) -> VocalStateToken:
        """State token of the draft."""
    def apply(
        self,
        operations: Iterable[VocalOperation],
        expected_generation: int,
    ) -> VocalEditResult:
        """Apply operations in order; ``expected_generation`` must be the draft's generation.

        Raises VocalEditError when refused.
        """
    def commit(self, expected_revision: int | None = ...) -> VocalEditResult:
        """Commit the draft; ``expected_revision`` None means the session's current revision.

        Raises VocalEditError on a stale revision.
        """
    def cancel(self) -> None:
        """Discard the draft."""
    def evaluate_pitch(self, note_id: int) -> VocalPitchResult:
        """Measured, target and effective pitch per frame of one note in the draft."""
    def source_sample_to_destination_sample(self, note_id: int, source_sample: float) -> float:
        """Map a note's source position in the draft to the output, in samples."""
    def destination_sample_to_source_sample(self, note_id: int, destination_sample: float) -> float:
        """Map a note's output position in the draft to the source, in samples."""
    def capture_render_snapshot(self) -> VocalRenderSnapshot:
        """Capture the draft state for rendering."""

class VocalRenderSnapshot:
    def __enter__(self) -> VocalRenderSnapshot: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None:
        """Release the native snapshot; safe to call twice."""
    @property
    def output_length_samples(self) -> int:
        """Length of the output in samples."""
    def render(
        self,
        range: VocalRange | Sequence[int | None] | None = ...,
        *,
        start_sec: float | None = ...,
        end_sec: float | None = ...,
        request_id: int = ...,
        cancel: Callable[[], bool] | None = ...,
    ) -> VocalRenderResult: ...
    def begin_render_job(
        self,
        range: VocalRange | Sequence[int | None] | None = ...,
        *,
        start_sec: float | None = ...,
        end_sec: float | None = ...,
        request_id: int = ...,
    ) -> VocalRenderJob:
        """Start an incremental render of a destination range, in samples or seconds."""

class VocalRenderJob:
    def __enter__(self) -> VocalRenderJob: ...
    def __exit__(self, exc_type: object, exc: object, tb: object) -> None: ...
    def close(self) -> None:
        """Release the native job; safe to call twice."""
    def abort(self) -> None:
        """Abandon the render."""
    def next(self, cancel: Callable[[], bool] | None = ...) -> bool:
        """Run one step; True once the render is complete; ``cancel`` is polled."""
    def finalize(self, cancel: Callable[[], bool] | None = ...) -> VocalRenderResult:
        """Return the rendered result and release the job; ``cancel`` is polled."""

def vocal_edit_abi_version() -> int | None: ...
