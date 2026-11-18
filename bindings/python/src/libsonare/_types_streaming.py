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
    from ._types_enums import PitchClass, WindowType


@dataclass(frozen=True, slots=True, eq=False)
class CqtResult:
    """Constant-Q / Variable-Q transform magnitude result.

    ``magnitude`` is a float32 array of shape ``(n_bins, n_frames)`` and
    ``frequencies`` holds the ``n_bins`` center frequencies in Hz.
    """

    hop_length: int
    sample_rate: int
    magnitude: NDArray[np.float32]
    frequencies: NDArray[np.float32]


@dataclass(frozen=True, slots=True)
class StreamQuantizeConfig:
    """Quantization ranges for the u8/i16 bandwidth-reduction read paths.

    Defaults mirror ``sonare_stream_quantize_config_default``. Widen any range
    whose source values exceed the defaults: the quantizers clamp normalized
    values to ``[0, 1]``, so a stream louder or quieter than these ranges
    otherwise saturates silently to the endpoints.
    """

    mel_db_min: float = -80.0
    mel_db_max: float = 0.0
    onset_max: float = 50.0
    rms_max: float = 1.0
    centroid_max: float = 11025.0


@dataclass(frozen=True, slots=True)
class StreamConfig:
    """Construction config for :class:`StreamAnalyzer`.

    Field defaults mirror the C ``sonare_stream_analyzer_config_default`` values.

    ``n_fft`` is the analysis window as a duration, in samples at 44100 Hz. Below
    that rate the window is rescaled to the analysis rate, never shorter than
    ``hop_length``; ``hop_length`` itself is in samples at the analysis rate.
    The emitted bin count is fixed by the FFT length the window is zero-padded
    to, not by ``n_fft // 2 + 1``.

    ``fmax_hz`` is ``None`` for Nyquist; an explicit value must be positive.
    ``reference_hz`` is the A4 reference and must be within 220..880, the same
    range :meth:`StreamAnalyzer.set_reference_hz` accepts.
    """

    sample_rate: int
    n_fft: int = 2048
    hop_length: int = 512
    n_mels: int = 128
    fmin_hz: float = 0.0
    fmax_hz: float | None = None
    reference_hz: float = 440.0
    compute_mel: bool = True
    compute_chroma: bool = True
    compute_onset: bool = True
    compute_spectral: bool = True
    emit_every_n_frames: int = 1
    magnitude_downsample: int = 1
    max_pending_frames: int = 4096  # Overflow drops the newly produced frame.
    max_progression_entries: int = 4096  # Overflow drops the oldest entry.
    key_update_interval_sec: float = 5.0
    bpm_update_interval_sec: float = 10.0
    window: WindowType = "hann"


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
    timestamps_sec: NDArray[np.float32]
    mel: NDArray[np.float32] | None
    chroma: NDArray[np.float32] | None
    onset_strength: NDArray[np.float32] | None
    rms_energy: NDArray[np.float32]
    spectral_centroid: NDArray[np.float32] | None
    spectral_flatness: NDArray[np.float32] | None
    chord_root: NDArray[np.int32] | None
    chord_quality: NDArray[np.int32] | None
    chord_confidence: NDArray[np.float32] | None


@dataclass(frozen=True, slots=True, eq=False)
class StreamFramesU8:
    """Quantized (uint8) frame batch, shaped as :class:`StreamFrames`.

    ``mel`` is in dB, quantized over ``[mel_db_min, mel_db_max]`` (unlike the
    linear power of :class:`StreamFrames`). A feature not computed is ``None``.
    """

    n_frames: int
    timestamps_sec: NDArray[np.float32]
    mel: NDArray[np.uint8] | None
    chroma: NDArray[np.uint8] | None
    onset_strength: NDArray[np.uint8] | None
    rms_energy: NDArray[np.uint8]
    spectral_centroid: NDArray[np.uint8] | None
    spectral_flatness: NDArray[np.uint8] | None


@dataclass(frozen=True, slots=True, eq=False)
class StreamFramesI16:
    """Quantized (int16) frame batch, shaped as :class:`StreamFrames`.

    ``mel`` is in dB, quantized over ``[mel_db_min, mel_db_max]`` (unlike the
    linear power of :class:`StreamFrames`). A feature not computed is ``None``.
    """

    n_frames: int
    timestamps_sec: NDArray[np.float32]
    mel: NDArray[np.int16] | None
    chroma: NDArray[np.int16] | None
    onset_strength: NDArray[np.int16] | None
    rms_energy: NDArray[np.int16]
    spectral_centroid: NDArray[np.int16] | None
    spectral_flatness: NDArray[np.int16] | None


@dataclass(frozen=True, slots=True)
class StreamChordChange:
    """A chord change in the progression; ``name`` is spelled as the batch
    analysis spells a chord, and ``quality`` is a :data:`CHORD_QUALITIES` token."""

    root: PitchClass
    quality: str
    name: str
    start_sec: float
    confidence: float


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
    root: PitchClass
    quality: str
    name: str
    start_sec: float
    confidence: float


@dataclass(frozen=True, slots=True)
class StreamPatternScore:
    name: str
    score: float


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
    bpm_confidence: float | None
    bpm_candidate_count: int
    key: Key | None
    chord: StreamChordChange | None
    chord_progression: list[StreamChordChange]
    bar_chord_progression: list[StreamBarChord]
    current_bar: int | None
    bar_duration_sec: float | None
    voted_pattern: list[StreamBarChord | None]
    pattern_length: int
    detected_pattern_name: str | None
    detected_pattern_score: float | None
    all_pattern_scores: list[StreamPatternScore]
    accumulated_sec: float
    used_frames: int
    updated: bool


@dataclass(frozen=True, slots=True)
class StreamStats:
    """Counters and the progressive estimate snapshot from :class:`StreamAnalyzer`."""

    total_frames: int
    total_samples: int
    duration_sec: float
    pending_frames: int
    dropped_output_frames: int
    dropped_chord_progression_entries: int
    dropped_bar_progression_entries: int
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
