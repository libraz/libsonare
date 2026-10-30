"""Vocal-edit enums, value dataclasses, and operation types."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import TypeAlias

import numpy as np
from numpy.typing import NDArray

FloatArray: TypeAlias = NDArray[np.float32]


class VocalTargetMode(IntEnum):
    NONE = 0
    CENTER = 1
    CURVE = 2


class VocalFormantMode(IntEnum):
    PRESERVE = 0
    SHIFT = 1


class VocalOperationKind(IntEnum):
    SET_EDIT = 0
    SET_SOURCE_SPAN = 1
    SPLIT = 2
    MERGE = 3
    SET_TRANSITION = 4
    REMOVE_TRANSITION = 5
    RESET = 6


class VocalMergePolicy(IntEnum):
    PRESERVE = 0
    RESET = 1


@dataclass(frozen=True, slots=True)
class VocalRange:
    start_sample: int
    end_sample: int

    @property
    def length(self) -> int:
        return max(0, self.end_sample - self.start_sample)


@dataclass(frozen=True, slots=True)
class VocalPitchPoint:
    source_sample: float
    target_midi: float

    @property
    def midi(self) -> float:
        return self.target_midi


@dataclass(frozen=True, slots=True)
class VocalNoteEdit:
    target_mode: VocalTargetMode | int = VocalTargetMode.NONE
    target_midi: float = 0.0
    target_points: tuple[VocalPitchPoint, ...] = ()
    amount: float = 0.0
    speed_ms: float = 0.0
    max_correction_semitones: float = 12.0
    transpose_semitones: float = 0.0
    drift_scale: float = 1.0
    vibrato_scale: float = 1.0
    destination_start_sample: int = 0
    destination_length_samples: int = 0
    gain_db: float = 0.0
    muted: bool = False
    formant_mode: VocalFormantMode | int = VocalFormantMode.PRESERVE
    formant_shift_semitones: float = 0.0
    amplitude_envelope: tuple[float, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "target_points", tuple(self.target_points))
        object.__setattr__(self, "amplitude_envelope", tuple(self.amplitude_envelope))


@dataclass(frozen=True, slots=True)
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
    amplitude: FloatArray
    edit: VocalNoteEdit

    @property
    def source_range(self) -> VocalRange:
        return VocalRange(self.source_start_sample, self.source_end_sample)


@dataclass(frozen=True, slots=True)
class VocalTransition:
    left_note_id: int
    right_note_id: int
    left_window_samples: int
    right_window_samples: int
    strength: float


@dataclass(frozen=True, slots=True)
class VocalStateToken:
    session_epoch: int
    revision: int
    draft_id: int
    generation: int
    request_id: int = 0
    profile_id: int = 0

    @property
    def committed_revision(self) -> int:
        return self.revision


@dataclass(frozen=True, slots=True)
class VocalAnalysis:
    frame_origin_sample: float
    samples_per_frame: float
    frame_length_samples: int
    f0_hz: FloatArray
    voiced: NDArray[np.uint8]
    algorithm_id: str
    algorithm_version: int
    fmin_hz: float = 65.0
    fmax_hz: float = 2093.0
    yin_threshold: float = 0.1
    voiced_threshold: float = 0.5
    centered: bool = True
    segmentation_threshold_cents: float = 50.0
    min_note_ms: float = 30.0
    reference_hz: float = 440.0


@dataclass(frozen=True, slots=True)
class VocalAnalysisResult:
    analysis: VocalAnalysis
    source_sha256: str
    analysis_sha256: str
    source_length_samples: int
    sample_rate: int


@dataclass(frozen=True, slots=True)
class VocalCapabilities:
    api_version: int
    profile_id: int
    monophonic_only: bool
    analysis_cancellable: bool
    minimum_formant_shift_semitones: float
    maximum_formant_shift_semitones: float


@dataclass(frozen=True, slots=True)
class VocalIdChange:
    operation_index: int
    retired_id: int
    new_ids: tuple[int, ...]


@dataclass(frozen=True, slots=True)
class VocalEditResult:
    token: VocalStateToken
    dirty_ranges: tuple[VocalRange, ...]
    id_changes: tuple[VocalIdChange, ...] = ()


@dataclass(frozen=True, slots=True)
class VocalPitchResult:
    source_samples: NDArray[np.float64]
    measured_midi: NDArray[np.float64]
    target_midi: NDArray[np.float64]
    effective_midi: NDArray[np.float64]
    voiced: NDArray[np.uint8]
    has_target: NDArray[np.uint8]


@dataclass(frozen=True, slots=True)
class VocalRenderResult:
    samples: FloatArray
    start_sample: int
    token: VocalStateToken
    processed_ranges: tuple[VocalRange, ...]
    cache_hit_units: int
    dry_passed_frames: int
    limited_correction_frames: int


@dataclass(frozen=True, slots=True)
class VocalCreateOptions:
    output_length_samples: int | None = None
    edge_fade_ms: float | None = None
    vibrato_cutoff_hz: float | None = None
    segmentation_threshold_cents: float | None = None
    min_note_ms: float | None = None
    frame_length_samples: int | None = None
    hop_length_samples: int | None = None
    fmin_hz: float | None = None
    fmax_hz: float | None = None
    max_history_bytes: int | None = None
    max_cache_bytes: int | None = None
    max_undo_depth: int | None = None
    max_render_jobs: int | None = None
    analysis: VocalAnalysis | None = None
    yin_threshold: float | None = None
    voiced_threshold: float | None = None
    centered: bool | None = None
    reference_hz: float | None = None


@dataclass(frozen=True, slots=True)
class VocalSetNoteEdit:
    note_id: int
    edit: VocalNoteEdit


@dataclass(frozen=True, slots=True)
class VocalSetNoteSourceSpan:
    """Set a note's source span and its destination placement.

    Each position is given in samples or in seconds (``*_sec``, rounded to the
    nearest sample at the session's rate), one spelling per position. Every
    position is required and has no default: an omitted one is refused by name,
    and 0 is the sample position 0. Giving both spellings of one position is
    refused.
    """

    note_id: int
    source_start_sample: int | None = None
    source_end_sample: int | None = None
    destination_start_sample: int | None = None
    destination_length_samples: int | None = None
    source_start_sec: float | None = None
    source_end_sec: float | None = None
    destination_start_sec: float | None = None
    destination_length_sec: float | None = None


@dataclass(frozen=True, slots=True)
class VocalSplitNote:
    """Cut a note at ``cut_source_sample`` or at ``cut_source_sec`` seconds."""

    note_id: int
    cut_source_sample: int | None = None
    cut_source_sec: float | None = None


@dataclass(frozen=True, slots=True)
class VocalMergeNotes:
    note_ids: tuple[int, ...]
    merge_policy: VocalMergePolicy | int = VocalMergePolicy.PRESERVE

    def __post_init__(self) -> None:
        object.__setattr__(self, "note_ids", tuple(self.note_ids))


@dataclass(frozen=True, slots=True)
class VocalSetTransition:
    transition: VocalTransition


@dataclass(frozen=True, slots=True)
class VocalRemoveTransition:
    left_note_id: int
    right_note_id: int


@dataclass(frozen=True, slots=True)
class VocalResetNotes:
    note_ids: tuple[int, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "note_ids", tuple(self.note_ids))


VocalOperation: TypeAlias = (
    VocalSetNoteEdit
    | VocalSetNoteSourceSpan
    | VocalSplitNote
    | VocalMergeNotes
    | VocalSetTransition
    | VocalRemoveTransition
    | VocalResetNotes
)
