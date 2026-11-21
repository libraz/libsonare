"""Public type definitions for libsonare.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray

    from ._types_analysis import Key
    from ._types_enums import PitchClass


@dataclass(frozen=True, slots=True, eq=False)
class CqtResult:
    """Constant-Q / Variable-Q transform magnitude result.

    ``magnitude`` is a float32 array of shape ``(n_bins, n_frames)`` and
    ``frequencies`` holds the ``n_bins`` center frequencies in Hz.
    """

    hop_length: int
    """Hop between frames, in samples."""
    sample_rate: int
    """Sample rate of the analyzed signal in Hz."""
    magnitude: NDArray[np.float32]
    """Magnitude per bin and frame, shape ``(n_bins, n_frames)``."""
    frequencies: NDArray[np.float32]
    """Center frequency of each bin in Hz."""
    gamma: float
    """Bandwidth offset in Hz the transform ran with: the resolved value for :func:`vqt`, 0 for
    the constant-Q transforms."""


@dataclass(frozen=True, slots=True)
class StreamQuantizeConfig:
    """Quantization ranges for the u8/i16 bandwidth-reduction read paths.

    Defaults mirror ``sonare_stream_quantize_config_init``. Widen any range
    whose source values exceed the defaults: the quantizers clamp normalized
    values to ``[0, 1]``, so a stream louder or quieter than these ranges
    otherwise saturates silently to the endpoints.
    """

    mel_db_min: float = -80.0
    """Mel floor in dB for quantization."""
    mel_db_max: float = 0.0
    """Mel ceiling in dB for quantization."""
    onset_max: float = 50.0
    """Largest expected onset strength."""
    rms_max: float = 1.0
    """Largest expected RMS energy."""
    centroid_max: float = 11025.0
    """Largest expected spectral centroid in Hz."""


@dataclass(frozen=True, slots=True, eq=False)
class StreamFrames:
    """A batch of analyzed frames from :class:`StreamAnalyzer`.

    Matrices are band-major ``(bands, n_frames)`` arrays, the layout of the batch
    :func:`mel_spectrogram`. A feature that is not computed is ``None``; the chord
    arrays follow ``compute_chroma``.

    ``mel`` is LINEAR mel power (not dB). The quantized read paths
    (:class:`StreamFramesU8` / :class:`StreamFramesI16`) convert to dB before
    packing.

    ``chord_root`` is the pitch class of each frame's chord, or -1 where none was
    detected; ``chord_quality`` is an ordinal into :data:`CHORD_QUALITIES`.
    """

    n_frames: int
    """Number of frames in the batch."""
    timestamps_sec: NDArray[np.float32]
    """Frame timestamps in seconds, shape ``(n_frames,)``."""
    mel: NDArray[np.float32] | None
    """Linear mel power, shape ``(n_mels, n_frames)``; None when not computed."""
    chroma: NDArray[np.float32] | None
    """Chroma per frame, shape ``(n_chroma, n_frames)``; None when not computed."""
    onset_strength: NDArray[np.float32] | None
    """Onset strength per frame; None when not computed."""
    rms_energy: NDArray[np.float32]
    """RMS energy per frame."""
    spectral_centroid: NDArray[np.float32] | None
    """Spectral centroid per frame in Hz; None when not computed."""
    spectral_flatness: NDArray[np.float32] | None
    """Spectral flatness per frame; None when not computed."""
    chord_root: NDArray[np.int32] | None
    """Per-frame chord root pitch class, -1 where none; None when chroma is not computed."""
    chord_quality: NDArray[np.int32] | None
    """Per-frame chord quality ordinal; None when chroma is not computed."""
    chord_confidence: NDArray[np.float32] | None
    """Per-frame chord confidence; None when chroma is not computed."""


@dataclass(frozen=True, slots=True, eq=False)
class StreamFramesU8:
    """Quantized (uint8) frame batch, shaped as :class:`StreamFrames`.

    ``mel`` is in dB, quantized over ``[mel_db_min, mel_db_max]`` (unlike the
    linear power of :class:`StreamFrames`). A feature not computed is ``None``.
    """

    n_frames: int
    """Number of frames in the batch."""
    timestamps_sec: NDArray[np.float32]
    """Frame timestamps in seconds, shape ``(n_frames,)``."""
    mel: NDArray[np.uint8] | None
    """Mel in dB over ``[mel_db_min, mel_db_max]``; None when not computed."""
    chroma: NDArray[np.uint8] | None
    """Quantized chroma per frame, shape ``(n_chroma, n_frames)``; None when not computed."""
    onset_strength: NDArray[np.uint8] | None
    """Quantized onset strength per frame; None when not computed."""
    rms_energy: NDArray[np.uint8]
    """Quantized RMS energy per frame."""
    spectral_centroid: NDArray[np.uint8] | None
    """Quantized spectral centroid per frame; None when not computed."""
    spectral_flatness: NDArray[np.uint8] | None
    """Quantized spectral flatness per frame; None when not computed."""
    chord_root: NDArray[np.int8] | None
    """Per-frame chord root pitch class, -1 where none; None when chroma is not computed."""
    chord_quality: NDArray[np.int8] | None
    """Per-frame chord quality ordinal; None when chroma is not computed."""
    chord_confidence: NDArray[np.uint8] | None
    """Chord confidence quantized over ``[0, 1]``; None when chroma is not computed."""


@dataclass(frozen=True, slots=True, eq=False)
class StreamFramesI16:
    """Quantized (int16) frame batch, shaped as :class:`StreamFrames`.

    ``mel`` is in dB, quantized over ``[mel_db_min, mel_db_max]`` (unlike the
    linear power of :class:`StreamFrames`). A feature not computed is ``None``.
    """

    n_frames: int
    """Number of frames in the batch."""
    timestamps_sec: NDArray[np.float32]
    """Frame timestamps in seconds, shape ``(n_frames,)``."""
    mel: NDArray[np.int16] | None
    """Mel in dB over ``[mel_db_min, mel_db_max]``; None when not computed."""
    chroma: NDArray[np.int16] | None
    """Quantized chroma per frame, shape ``(n_chroma, n_frames)``; None when not computed."""
    onset_strength: NDArray[np.int16] | None
    """Quantized onset strength per frame; None when not computed."""
    rms_energy: NDArray[np.int16]
    """Quantized RMS energy per frame."""
    spectral_centroid: NDArray[np.int16] | None
    """Quantized spectral centroid per frame; None when not computed."""
    spectral_flatness: NDArray[np.int16] | None
    """Quantized spectral flatness per frame; None when not computed."""
    chord_root: NDArray[np.int8] | None
    """Per-frame chord root pitch class, -1 where none; None when chroma is not computed."""
    chord_quality: NDArray[np.int8] | None
    """Per-frame chord quality ordinal; None when chroma is not computed."""
    chord_confidence: NDArray[np.int16] | None
    """Chord confidence quantized over ``[0, 1]``; None when chroma is not computed."""


@dataclass(frozen=True, slots=True)
class StreamChordChange:
    """A chord change in the progression; ``name`` is spelled as the batch
    analysis spells a chord, and ``quality`` is a :data:`CHORD_QUALITIES` token."""

    root: PitchClass
    """Pitch class of the chord root."""
    quality: str
    """Chord quality token from :data:`CHORD_QUALITIES`."""
    name: str
    """Chord name, spelled as the batch analysis spells a chord."""
    start_sec: float
    """Time the chord starts, in seconds."""
    confidence: float
    """Chord confidence in ``[0, 1]``."""


@dataclass(frozen=True, slots=True)
class StreamBarChord:
    """One bar of the beat-synchronized chord progression.

    ``bar_index`` is the bar number, not this entry's position in the list: bars
    with no confident chord are not recorded and the oldest entries are dropped
    at the history cap. Group bars by pattern position with ``bar_index``, never
    with the list index.

    ``start_sec`` is on the same timeline as :attr:`StreamFrames.timestamps_sec`
    (including a ``start_sample`` anchor), and consecutive bars are
    ``bar_duration_sec`` apart rather than snapped to the analysis frame grid.

    In ``voted_pattern`` the fields shift meaning: ``bar_index`` is the pattern
    position and ``start_sec`` is unused; a position no chord was voted for is
    ``None`` in that list.
    """

    bar_index: int
    """Bar number; in ``voted_pattern`` the pattern position."""
    root: PitchClass
    """Pitch class of the chord root."""
    quality: str
    """Chord quality token from :data:`CHORD_QUALITIES`."""
    name: str
    """Chord name, spelled as the batch analysis spells a chord."""
    start_sec: float
    """Bar start in seconds; unused in ``voted_pattern``."""
    confidence: float
    """Chord confidence in ``[0, 1]``."""


@dataclass(frozen=True, slots=True)
class StreamPatternScore:
    """Match score of one chord pattern in a streaming analysis."""

    name: str
    """Chord pattern name."""
    score: float
    """Match score of the pattern."""


@dataclass(frozen=True, slots=True)
class StreamProgressiveEstimate:
    """Progressive BPM, key and chord estimate. A field is ``None`` until the
    analyzer has an estimate for it.

    ``bpm_candidate_count`` is the number of tempo candidates the most recent
    BPM estimate chose from, 0 until one has run — the same quantity as the
    batch analysis result's field of the same name.

    ``updated`` is true when the key or BPM was re-estimated since the previous
    snapshot. One change sets it on exactly one snapshot however the caller
    chunks its input, and a call that produced no frame does not repeat it.
    """

    bpm: float | None
    """Estimated tempo in BPM."""
    bpm_confidence: float | None
    """Confidence of the BPM estimate in ``[0, 1]``."""
    bpm_candidate_count: int
    """Tempo candidates the latest BPM estimate chose from."""
    key: Key | None
    """Estimated key."""
    chord: StreamChordChange | None
    """Current chord."""
    chord_progression: list[StreamChordChange]
    """Chord changes recorded so far."""
    bar_chord_progression: list[StreamBarChord]
    """Beat-synchronized chord progression, one entry per recorded bar."""
    current_bar: int | None
    """Current bar index; None until the BPM is stable."""
    bar_duration_sec: float | None
    """Duration of one bar in seconds; None until the BPM is stable."""
    voted_pattern: list[StreamBarChord | None]
    """Voted chord at each pattern position; None where no chord was voted."""
    pattern_length: int
    """Length of the voted chord pattern."""
    detected_pattern_name: str | None
    """Name of the detected chord pattern."""
    detected_pattern_score: float | None
    """Match score of the detected chord pattern."""
    all_pattern_scores: list[StreamPatternScore]
    """Score of every candidate chord pattern."""
    accumulated_sec: float
    """Total audio processed for estimation, in seconds."""
    used_frames: int
    """Number of frames used for estimation."""
    updated: bool
    """True when the key or BPM was re-estimated since the previous snapshot."""


@dataclass(frozen=True, slots=True)
class StreamStats:
    """Counters and the progressive estimate snapshot from :class:`StreamAnalyzer`."""

    total_frames: int
    """Total frames processed."""
    total_samples: int
    """Total samples processed."""
    duration_sec: float
    """Total audio processed, in seconds."""
    pending_frames: int
    """Unread frames currently retained."""
    dropped_output_frames: int
    """New output frames dropped at the pending-frame cap."""
    dropped_chord_progression_entries: int
    """Oldest chord changes dropped at the progression cap."""
    dropped_bar_progression_entries: int
    """Oldest bar chords dropped at the progression cap."""
    non_finite_discard_blocks: int
    """Blocks in which a non-finite input sample was replaced before it could
    reach the analyzer's recursive state.

    Unlike the drop counts above, nothing is missing from the output: every
    estimate is produced as usual and simply stops describing the input, so this
    is the only report that the stream was degraded. The unit is one
    :meth:`StreamAnalyzer.process` call, never a sample, so a block carrying a
    thousand NaNs adds one. Cleared by :meth:`StreamAnalyzer.reset` alongside
    the drop counts, because that call rebuilds the timeline and the count
    describes a segment rather than the analyzer.
    """

    estimate: StreamProgressiveEstimate
    """Progressive estimate snapshot."""
