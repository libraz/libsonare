"""Vocal-edit enums, value dataclasses, and operation types."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from typing import Literal, TypeAlias, get_args

import numpy as np
from numpy.typing import NDArray

FloatArray: TypeAlias = NDArray[np.float32]


VocalTargetMode: TypeAlias = Literal["none", "center", "curve"]
"""How a note's pitch target is given: untouched, one MIDI pitch, or a curve."""

VocalFormantMode: TypeAlias = Literal["preserve", "shift"]
"""Whether a note keeps its formants or shifts them by ``formant_shift_semitones``."""

VocalMergePolicy: TypeAlias = Literal["preserve", "reset"]
"""Whether merged notes keep the first note's edit or start from defaults."""

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
"""Why a vocal-edit call was refused; ``"none"`` on a record that carries no failure."""

_VOCAL_TARGET_MODES: tuple[VocalTargetMode, ...] = get_args(VocalTargetMode)
_VOCAL_FORMANT_MODES: tuple[VocalFormantMode, ...] = get_args(VocalFormantMode)
_VOCAL_MERGE_POLICIES: tuple[VocalMergePolicy, ...] = get_args(VocalMergePolicy)
_VOCAL_REASONS: tuple[VocalReason, ...] = get_args(VocalReason)


@dataclass(frozen=True, slots=True)
class VocalRange:
    """A half-open range of source or output samples."""

    start_sample: int
    """First sample of the range."""
    end_sample: int
    """One past the last sample of the range."""

    @property
    def length(self) -> int:
        """Number of samples in the range; 0 when empty or inverted."""
        return max(0, self.end_sample - self.start_sample)


@dataclass(frozen=True, slots=True)
class VocalPitchPoint:
    """One point of a pitch curve: a source position and its target pitch."""

    source_sample: float
    """Position in the source, in samples."""
    target_midi: float
    """Target pitch as a (fractional) MIDI note number."""

    @property
    def midi(self) -> float:
        """Alias of ``target_midi``."""
        return self.target_midi


@dataclass(frozen=True, slots=True)
class VocalNoteEdit:
    """The edit applied to one note: pitch target, correction, timing, gain and formant settings."""

    target_mode: VocalTargetMode = "none"
    """How the pitch target is given (``VocalTargetMode``): ``"none"``, ``"center"`` or
    ``"curve"``."""
    target_midi: float = 0.0
    """Target pitch as a (fractional) MIDI note number, used when ``target_mode`` is
    ``"center"``."""
    target_points: Sequence[VocalPitchPoint] = ()
    """Pitch curve over the source, used when ``target_mode`` is ``"curve"``."""
    amount: float = 0.0
    """Correction strength, 0 (none) to 1 (full)."""
    speed_ms: float = 0.0
    """Correction speed in milliseconds."""
    max_correction_semitones: float = 12.0
    """Largest correction applied, in semitones."""
    transpose_semitones: float = 0.0
    """Transposition applied on top of the correction, in semitones."""
    drift_scale: float = 1.0
    """Scale of the natural pitch drift kept: 1 keeps it, 0 removes it."""
    vibrato_scale: float = 1.0
    """Scale of the vibrato kept: 1 keeps it, 0 removes it."""
    destination_start_sample: int = 0
    """Where the note starts in the output, in samples."""
    destination_length_samples: int = 0
    """Length of the note in the output, in samples."""
    gain_db: float = 0.0
    """Gain in decibels."""
    muted: bool = False
    """True to silence the note."""
    formant_mode: VocalFormantMode = "preserve"
    """Formant handling (``VocalFormantMode``): ``"preserve"`` or ``"shift"``."""
    formant_shift_semitones: float = 0.0
    """Formant shift in semitones, used when ``formant_mode`` is ``"shift"``."""
    amplitude_envelope: Sequence[float] = ()
    """Amplitude envelope multipliers over the note; empty leaves the amplitude untouched."""

    def __post_init__(self) -> None:
        object.__setattr__(self, "target_points", tuple(self.target_points))
        object.__setattr__(self, "amplitude_envelope", tuple(self.amplitude_envelope))


@dataclass(frozen=True, slots=True)
class VocalNote:
    """One detected note with its source span, measured pitch and current edit."""

    id: int
    """Note id, stable until an operation retires it."""
    source_start_sample: int
    """First source sample of the note."""
    source_end_sample: int
    """One past the last source sample of the note."""
    analysis_frame_start: int
    """First analysis frame of the note."""
    analysis_frame_end: int
    """One past the last analysis frame of the note."""
    # The three pitch fields are None on a note no pitch was measured on.
    median_hz: float | None
    """Median fundamental frequency in Hz; None when no pitch was measured."""
    center_midi: float | None
    """Centre pitch as a MIDI note number; None when no pitch was measured."""
    f0_stability: float | None
    """Stability of the fundamental over the note; None when no pitch was measured."""
    amplitude: NDArray[np.float32]
    """Amplitude envelope of the note."""
    edit: VocalNoteEdit
    """The note's current edit."""

    @property
    def source_range(self) -> VocalRange:
        """The note's source span as a ``VocalRange``."""
        return VocalRange(self.source_start_sample, self.source_end_sample)


@dataclass(frozen=True, slots=True)
class VocalTransition:
    """The crossfade between two adjacent notes."""

    left_note_id: int
    """Id of the earlier note."""
    right_note_id: int
    """Id of the later note."""
    left_window_samples: int
    """Window length taken from the earlier note, in samples."""
    right_window_samples: int
    """Window length taken from the later note, in samples."""
    strength: float
    """Blend strength."""


@dataclass(frozen=True, slots=True)
class VocalNotesResult:
    """The notes of a vocal session or draft and the transitions between them."""

    notes: tuple[VocalNote, ...]
    """Every note, in order."""
    transitions: tuple[VocalTransition, ...]
    """Every transition between adjacent notes."""


@dataclass(frozen=True, slots=True)
class VocalHistory:
    """Whether the session has an edit to undo or redo."""

    can_undo: bool
    """True when an edit can be undone."""
    can_redo: bool
    """True when an undone edit can be redone."""


@dataclass(frozen=True, slots=True)
class VocalStateToken:
    """Identifies a session or draft state by epoch, revision, draft and generation counters."""

    session_epoch: int
    """Counter naming the session instance."""
    revision: int
    """Counter of committed edits."""
    draft_id: int
    """Id of the draft; 0 for a committed state."""
    generation: int
    """Counter of operations applied to the draft; 0 for a committed state."""
    request_id: int = 0
    """Id of the render request that produced the state; 0 when none."""
    profile_id: int = 0
    """Engine profile the state was produced under."""

    @property
    def committed_revision(self) -> int:
        """Alias of ``revision``."""
        return self.revision


@dataclass(frozen=True, slots=True)
class VocalAnalysis:
    """Frame-wise pitch analysis of the source and the settings it was made with."""

    frame_origin_sample: float
    """Source sample of the first analysis frame."""
    samples_per_frame: float
    """Hop between analysis frames, in samples."""
    frame_length_samples: int
    """Analysis window length, in samples."""
    f0_hz: NDArray[np.float32]
    """Fundamental frequency per frame, in Hz."""
    voiced: NDArray[np.uint8]
    """Voicing flag per frame (0 or 1)."""
    algorithm_id: str
    """Identifier of the pitch algorithm that produced the analysis."""
    algorithm_version: int
    """Version of the pitch algorithm."""
    fmin_hz: float = 65.0
    """Lowest frequency searched, in Hz."""
    fmax_hz: float = 2093.0
    """Highest frequency searched, in Hz."""
    yin_threshold: float = 0.1
    """Cutoff on the normalized YIN function, in (0, 1]."""
    voiced_threshold: float = 0.5
    """Voicing decision threshold, a probability in [0, 1]."""
    centered: bool = True
    """True when frames are centred on their position."""
    segmentation_threshold_cents: float = 50.0
    """Pitch change in cents that starts a new note."""
    min_note_ms: float = 30.0
    """Shortest note kept, in milliseconds."""
    reference_hz: float = 440.0
    """Reference tuning (A4) in Hz."""


@dataclass(frozen=True, slots=True)
class VocalAnalysisResult:
    """A pitch analysis with the digests and format of the source it describes."""

    analysis: VocalAnalysis
    """The pitch analysis the session was built from."""
    source_sha256: str
    """SHA-256 of the source, hexadecimal."""
    analysis_sha256: str
    """SHA-256 of the analysis, hexadecimal."""
    source_length_samples: int
    """Length of the analysed source, in samples."""
    sample_rate: int
    """Sample rate of the analysed source in Hz."""


@dataclass(frozen=True, slots=True)
class VocalCapabilities:
    """What the loaded vocal-edit engine supports."""

    abi_version: int
    """Vocal-edit ABI version of the loaded library."""
    profile_id: int
    """Engine profile id."""
    monophonic_only: bool
    """True when the engine edits one voice at a time."""
    analysis_cancellable: bool
    """True when analysis can be cancelled."""
    minimum_formant_shift_semitones: float
    """Lowest formant shift accepted, in semitones."""
    maximum_formant_shift_semitones: float
    """Highest formant shift accepted, in semitones."""


@dataclass(frozen=True, slots=True)
class VocalIdChange:
    """A note id retired by an operation and the ids that replace it."""

    operation_index: int
    """Index of the operation in the request that replaced the id."""
    retired_id: int
    """Note id that no longer exists."""
    new_ids: tuple[int, ...]
    """Note ids that replace it."""


@dataclass(frozen=True, slots=True)
class VocalEditResult:
    """Outcome of an applied edit: the new state, changed output ranges and id replacements."""

    token: VocalStateToken
    """State after the call."""
    dirty_ranges: tuple[VocalRange, ...]
    """Output ranges whose audio changed."""
    id_changes: tuple[VocalIdChange, ...] = ()
    """Note ids replaced by the call."""


@dataclass(frozen=True, slots=True)
class VocalPitchResult:
    """Per-frame measured, target and effective pitch over a range."""

    source_samples: NDArray[np.float64]
    """Source positions of the frames, in samples."""
    measured_midi: NDArray[np.float64]
    """Measured pitch per frame as MIDI note numbers."""
    target_midi: NDArray[np.float64]
    """Target pitch per frame as MIDI note numbers."""
    effective_midi: NDArray[np.float64]
    """Pitch per frame after correction, as MIDI note numbers."""
    voiced: NDArray[np.uint8]
    """Voicing flag per frame (0 or 1)."""
    has_target: NDArray[np.uint8]
    """1 per frame where a target exists, else 0."""


@dataclass(frozen=True, slots=True)
class VocalRenderResult:
    """Rendered audio with the state it came from and render statistics."""

    samples: NDArray[np.float32]
    """Rendered mono samples."""
    start_sample: int
    """Output sample at which ``samples`` starts."""
    token: VocalStateToken
    """State the audio was rendered from."""
    processed_ranges: tuple[VocalRange, ...]
    """Ranges that were actually recomputed."""
    cache_hit_units: int
    """Work units served from the cache."""
    dry_passed_frames: int
    """Frames passed through unprocessed."""
    limited_correction_frames: int
    """Frames whose correction was limited by ``max_correction_semitones``."""


@dataclass(frozen=True, slots=True)
class VocalSetNoteEdit:
    """Operation: replace a note's edit."""

    note_id: int
    """Id of the note to edit."""
    edit: VocalNoteEdit
    """The note's new edit, replacing the whole previous one."""


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
    """Id of the note to change."""
    source_start_sample: int | None = None
    """Start of the note in the source, in samples."""
    source_end_sample: int | None = None
    """End of the note in the source, in samples."""
    destination_start_sample: int | None = None
    """Start of the note in the output, in samples."""
    destination_length_samples: int | None = None
    """Length of the note in the output, in samples."""
    source_start_sec: float | None = None
    """Start of the note in the source, in seconds."""
    source_end_sec: float | None = None
    """End of the note in the source, in seconds."""
    destination_start_sec: float | None = None
    """Start of the note in the output, in seconds."""
    destination_length_sec: float | None = None
    """Length of the note in the output, in seconds."""


@dataclass(frozen=True, slots=True)
class VocalSplitNote:
    """Cut a note at ``cut_source_sample`` or at ``cut_source_sec`` seconds."""

    note_id: int
    """Id of the note to split."""
    cut_source_sample: int | None = None
    """Cut position in the source, in samples."""
    cut_source_sec: float | None = None
    """Cut position in the source, in seconds."""


@dataclass(frozen=True, slots=True)
class VocalMergeNotes:
    """Operation: merge adjacent notes into one."""

    note_ids: Sequence[int]
    """Ids of the adjacent notes to merge, in order."""
    merge_policy: VocalMergePolicy = "preserve"
    """Whether the merged note keeps the first note's edit or starts from defaults
    (``VocalMergePolicy``)."""

    def __post_init__(self) -> None:
        object.__setattr__(self, "note_ids", tuple(self.note_ids))


@dataclass(frozen=True, slots=True)
class VocalSetTransition:
    """Operation: set the crossfade between two adjacent notes."""

    transition: VocalTransition
    """The crossfade to set between two adjacent notes."""


@dataclass(frozen=True, slots=True)
class VocalRemoveTransition:
    """Operation: remove the crossfade between two adjacent notes."""

    left_note_id: int
    """Id of the earlier note."""
    right_note_id: int
    """Id of the later note."""


@dataclass(frozen=True, slots=True)
class VocalResetNotes:
    """Operation: restore notes to their analysed state."""

    note_ids: Sequence[int]
    """Ids of the notes to restore to their analysed state."""

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
"""One edit operation accepted by a vocal session or draft."""
