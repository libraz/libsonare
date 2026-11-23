from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from typing import Any, Literal, TypeAlias

from numpy.typing import NDArray

from ._vocal_edit_marshal import VocalEditError as VocalEditError
from ._vocal_edit_marshal import VocalErrorDetail as VocalErrorDetail
from ._vocal_edit_model import VocalAnalysis as VocalAnalysis
from ._vocal_edit_model import VocalAnalysisResult as VocalAnalysisResult
from ._vocal_edit_model import VocalCapabilities as VocalCapabilities
from ._vocal_edit_model import VocalEditResult as VocalEditResult
from ._vocal_edit_model import VocalHistory as VocalHistory
from ._vocal_edit_model import VocalIdChange as VocalIdChange
from ._vocal_edit_model import VocalMergeNotes as VocalMergeNotes
from ._vocal_edit_model import VocalNote as VocalNote
from ._vocal_edit_model import VocalNoteEdit as VocalNoteEdit
from ._vocal_edit_model import VocalNotesResult as VocalNotesResult
from ._vocal_edit_model import VocalPitchPoint as VocalPitchPoint
from ._vocal_edit_model import VocalPitchResult as VocalPitchResult
from ._vocal_edit_model import VocalRange as VocalRange
from ._vocal_edit_model import VocalRemoveTransition as VocalRemoveTransition
from ._vocal_edit_model import VocalRenderResult as VocalRenderResult
from ._vocal_edit_model import VocalResetNotes as VocalResetNotes
from ._vocal_edit_model import VocalSetNoteEdit as VocalSetNoteEdit
from ._vocal_edit_model import VocalSetNoteSourceSpan as VocalSetNoteSourceSpan
from ._vocal_edit_model import VocalSetTransition as VocalSetTransition
from ._vocal_edit_model import VocalSplitNote as VocalSplitNote
from ._vocal_edit_model import VocalStateToken as VocalStateToken
from ._vocal_edit_model import VocalTransition as VocalTransition

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

VocalOperation: TypeAlias = (
    VocalSetNoteEdit
    | VocalSetNoteSourceSpan
    | VocalSplitNote
    | VocalMergeNotes
    | VocalSetTransition
    | VocalRemoveTransition
    | VocalResetNotes
)

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
