"""Analysis result shapes: key, beats, chords, spectra, metering and stems."""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal, NamedTuple

import numpy as np
import numpy.typing as npt

from ._types_enums import KeyMode, PitchClass

if TYPE_CHECKING:
    from numpy.typing import NDArray

    from ._types_engine import MelodyResult, Section


def _empty_float32() -> np.ndarray:
    return np.empty(0, dtype=np.float32)


def _empty_int32() -> np.ndarray:
    return np.empty(0, dtype=np.int32)


@dataclass(frozen=True, slots=True)
class Key:
    """Detected musical key.

    Attributes:
        root: Tonic pitch class.
        mode: Major, minor or one of the five modes.
        confidence: Share of the model's belief that this key is the answer, in
            ``[0, 1)``. A softmax over the profile correlations of every
            candidate that was scored, so it falls as the runner-up closes in
            and two keys that split the evidence -- a relative major and minor,
            typically -- each report about half.

            This is the model's own belief, **not** a measured accuracy. It says
            how decisively the chroma picked this key out of the candidate set;
            it does not say how often that pick is right, and nothing here has
            been fitted against annotated recordings. A confident wrong answer
            is entirely possible, so a pipeline that branches on it must choose
            its own threshold against its own material.
    """

    root: PitchClass
    mode: KeyMode
    confidence: float

    @property
    def name(self) -> str:
        """Key spelled out, e.g. ``"C major"``."""
        return f"{self.root} {self.mode}"

    @property
    def short_name(self) -> str:
        """Compact key spelling: ``"C"`` for major, ``"Cm"`` for minor, the long form otherwise."""
        if self.mode == "major":
            return f"{self.root}"
        if self.mode == "minor":
            return f"{self.root}m"
        return f"{self.root} {self.mode}"

    def __str__(self) -> str:
        return self.name


@dataclass(frozen=True, slots=True, eq=False)
class AutoTuneResult:
    """Result of :func:`auto_tune`.

    Attributes:
        samples: The corrected audio, the length of the input.
        key: The key the audio was tuned to: the detected one (with the model's
            own ``confidence``, see :class:`Key`) or the one the call named
            (``confidence`` 1).
    """

    samples: npt.NDArray[np.float32]
    key: Key


@dataclass(frozen=True, slots=True)
class KeyCandidate:
    """Key candidate with raw profile correlation."""

    key: Key
    """The candidate key."""
    correlation: float
    """Correlation of the chroma with this key's profile."""


@dataclass(frozen=True, slots=True)
class TimeSignature:
    """Detected time signature."""

    numerator: int
    """Beats per bar."""
    denominator: int
    """Note value of one beat (4 = quarter note)."""
    confidence: float
    """Belief in this meter over the other candidates, in [0, 1]."""

    def __str__(self) -> str:
        return f"{self.numerator}/{self.denominator}"


@dataclass(frozen=True, slots=True)
class Beat:
    """Beat event.

    ``strength`` is a single raw frame of the onset envelope, sampled at the
    beat's own frame. It is not normalized, its scale depends on the material,
    and it moves with beat-position jitter, so it is not a relative salience
    across beats. For accent scoring use
    :attr:`AnalysisResult.beat_observations` ``.onset_strength``, which is the
    windowed aggregate the library's own downbeat pass scores.
    """

    time_sec: float
    """Beat time, in seconds."""
    strength: float
    """Raw onset-envelope value at the beat's frame; unnormalized (see above)."""


@dataclass(frozen=True, slots=True)
class AnalysisDynamics:
    """Dynamics summary embedded in :class:`AnalysisResult`."""

    dynamic_range_db: float | None
    """Dynamic range, in dB; None for a silent input."""
    peak_db: float
    """Peak level, in dB; -inf for a silent input."""
    rms_db: float
    """RMS level, in dB; -inf for a silent input."""
    crest_factor: float | None
    """Peak-to-RMS ratio of the signal; None for a silent input."""
    loudness_range_lu: float | None
    """Loudness range, in LU; None for a silent input."""
    is_compressed: bool
    """True when the signal is flagged as compressed."""


@dataclass(frozen=True, slots=True)
class AnalysisRhythm:
    """Rhythm summary embedded in :class:`AnalysisResult`."""

    time_signature: TimeSignature
    """Estimated time signature."""
    syncopation: float | None
    """Syncopation level, in ``[0, 1]``.

    ``None`` below four tracked beats, with silent accents, or without both a
    strong- and a weak-position beat observed.
    """
    groove_type: str
    """Rhythmic feel of the track, as a groove token."""
    pattern_regularity: float | None
    """Regularity of the beat intervals, in ``[0, 1]``; ``None`` below two intervals."""
    tempo_stability: float | None
    """Tempo stability over time, in ``[0, 1]``; ``None`` below two positive intervals."""


@dataclass(frozen=True, slots=True, eq=False)
class BeatObservations:
    """Beat-level evidence behind the downbeat and meter decisions.

    These are the inputs the library's own downbeat and meter pass scores, not
    outputs of it. Each stream is parallel to ``AnalysisResult.beats``, one
    value per beat. An empty stream means the analysis could not produce it,
    *not* that every beat scored zero: ``low_frequency_energy`` is empty when
    the analysis ran without audio, and ``chord_change`` is empty until chords
    have been analyzed.

    ``onset_strength`` is the windowed onset aggregate around each beat -- the
    accent value the downbeat pass scores, and the strength source
    :func:`libsonare.estimate_meter` expects. It is distinct from
    ``Beat.strength``, which is a single raw unwindowed envelope frame at the
    beat's frame.
    """

    onset_strength: npt.NDArray[np.float32] = dataclasses.field(default_factory=_empty_float32)
    """Windowed onset aggregate around each beat; the accent value the downbeat pass scores."""
    low_frequency_energy: npt.NDArray[np.float32] = dataclasses.field(
        default_factory=_empty_float32
    )
    """Low-frequency energy at each beat; empty when the analysis ran without audio."""
    chord_change: npt.NDArray[np.float32] = dataclasses.field(default_factory=_empty_float32)
    """Chord-change evidence at each beat; empty until chords are analyzed."""


@dataclass(frozen=True, slots=True)
class KeyDetection(Key):
    """A detected key and the tuning its chroma was built with.

    Attributes:
        tuning_cents: Tuning used, in cents: the given value, or the
            measured one under ``tuning_cents="auto"``.
    """

    tuning_cents: float = 0.0


@dataclass(frozen=True, slots=True, eq=False)
class AnalysisResult:
    """Full audio analysis result.

    ``bpm_confidence`` is the winning tempo cluster's vote share within 60-200 BPM,
    searched at 22050 Hz. :func:`analyze_bpm` searches the same range at the input
    rate, so the two can differ on an input at another rate.
    """

    bpm: float | None
    """Global tempo, in beats per minute; ``None`` when no tempo was found."""
    bpm_confidence: float
    """Winning tempo cluster's vote share, in ``[0, 1]`` (see the class note)."""
    key: Key
    """Detected musical key."""
    time_signature: TimeSignature
    """Estimated time signature."""
    timbre: TimbreFrame
    """Aggregate timbre summary."""
    dynamics: AnalysisDynamics
    """Aggregate dynamics summary."""
    rhythm: AnalysisRhythm
    """Aggregate rhythm summary."""
    melody: MelodyResult
    """Melody-contour summary."""
    beats: list[Beat] = dataclasses.field(default_factory=list)
    """Detected beats, in time order."""
    # The bar starts, drawn from ``beats``: each is the beat that opens a bar.
    downbeats: list[Beat] = dataclasses.field(default_factory=list)
    """The beats that open a bar; not the same length as ``beats``."""
    # Which beat of the first bar the analysis starts on. The meter estimator's
    # own phase, so it can disagree with the first downbeat once downbeats are
    # refined from chord and low-frequency evidence.
    downbeat_phase: int = 0
    """Beat index the first bar starts on."""
    bpm_candidates: list[AnalysisBpmCandidate] = dataclasses.field(default_factory=list)
    """Tempo hypotheses, the chosen tempo first."""
    time_signature_candidates: list[TimeSignature] = dataclasses.field(default_factory=list)
    """Time-signature hypotheses ordered by descending support."""
    chords: list[AnalysisChord] = dataclasses.field(default_factory=list)
    """Detected chord progression, each labelled with its Roman numeral in ``key``."""
    sections: list[Section] = dataclasses.field(default_factory=list)
    """Detected song-structure sections."""
    beat_observations: BeatObservations | None = None
    """Per-beat evidence behind the downbeat and meter decisions; ``None`` when not produced."""
    # Smoothed local tempo at each beat, in BPM, parallel to ``beats``.
    # Empty unless ``compute_tempo_curve`` was set, and empty regardless when
    # fewer than two beats were detected, since a tempo is a property of the
    # interval between two beats. The last entry repeats the tempo of the
    # interval leading into the final beat, which opens no interval of its own.
    # Values are continuous, not points on a tempo grid: each is a weighted
    # average, in log tempo, of the beat intervals around it. This is the local
    # tempo rather than ``bpm`` resampled: on material whose tempo moves it
    # departs from ``bpm``.
    beat_local_bpm: npt.NDArray[np.float32] = dataclasses.field(default_factory=_empty_float32)
    """Smoothed local tempo at each beat, in BPM, parallel to ``beats``; may be empty."""
    form: str = ""
    """Musical form label, e.g. ``"AABA"``; empty when none was derived."""
    # Tuning the chromagrams were built with, in cents: the
    # given value, or the measured one under ``tuning_cents="auto"``.
    tuning_cents: float = 0.0
    """Tuning the chromagrams were built with, in cents."""


@dataclass(frozen=True, slots=True, eq=False)
class MeterEstimate:
    """Meter estimated over a caller-supplied beat series.

    ``candidate_scores`` and ``candidates`` do not index alike:
    ``candidate_scores`` is parallel to the numerators that were *requested*,
    in the order they were requested, while ``candidates`` is ordered by
    descending support. Pair a score with a numerator through the request list,
    never through ``candidates``.

    A ``candidate_scores`` entry is standardized and signed: zero is the level a
    numerator reaches on beats carrying no meter, so a negative entry means less
    support than noise would produce. Only the ordering and the gaps between
    entries carry meaning — one entry read on its own says nothing.

    Scores are comparable only within one result. A score grows with the square
    root of how many beats were scored, so the same meter over twice the beats
    scores about 1.41 times as high. Scores taken from two calls over spans of
    different lengths rank the spans by length as much as by meter; a
    segmentation search that compares them across candidate span boundaries has
    to normalize for length first.

    ``grouping`` is an int32 array of how the bar divides, in beats per accent group: ``[3, 2, 2]``
    is the 7/8 an aksak meter notates as 3+2+2, and ``[2, 2]`` an ordinary four.
    It always sums to ``time_signature.numerator``. A single entry means no
    internal division was resolved — the numerator has none to find, it was too
    wide to search, or the span was too short to search at all.

    ``searched`` separates a measurement from the fixed fallback: it is False
    when the beat series was too short to score any candidate, and then every
    other field carries that fallback rather than a result. Read it before
    treating a short span's answer as a detection — the confidence reported
    alongside it is 0, so an unchecked read degrades toward "no idea" rather
    than toward a middling detection.

    Two different quantities share the name ``confidence``:
    ``time_signature.confidence`` is derived from the margin over the
    runner-up, while a ``candidates`` entry's is that candidate's share of the
    summed support. They are not comparable — read the one belonging to the
    field you meant.
    """

    time_signature: TimeSignature
    """Best-supported meter over the scored beat series."""
    downbeat_phase: int
    """Beat index the first bar starts on, in ``[0, time_signature.numerator)``."""
    searched: bool
    """False when the beats were too few to score; every other field is then the fallback."""
    grouping: npt.NDArray[np.int32] = dataclasses.field(default_factory=_empty_int32)
    """How the bar divides, in beats per accent group; sums to ``time_signature.numerator``."""
    candidate_scores: npt.NDArray[np.float32] = dataclasses.field(default_factory=_empty_float32)
    """Standardized support per requested numerator, in request order (see above)."""
    candidates: list[TimeSignature] = dataclasses.field(default_factory=list)
    """Scored meters by descending support; each ``confidence`` is its share of the total."""


@dataclass(frozen=True, slots=True)
class BpmCandidate:
    """A tempo candidate with its confidence."""

    bpm: float
    """Tempo, in beats per minute."""
    confidence: float
    """Winning tempo cluster's vote share, in ``[0, 1]``."""


BpmCandidateRelation = Literal["primary", "half", "double", "other"]
"""How a tempo candidate relates to the primary: the primary, half of it, double, or other."""


@dataclass(frozen=True, slots=True)
class AnalysisBpmCandidate(BpmCandidate):
    """A tempo candidate of :func:`analyze`, with its relation to the chosen tempo."""

    relation: BpmCandidateRelation
    """Relation to the chosen tempo: ``primary``, ``half``, ``double`` or ``other``."""


@dataclass(frozen=True, slots=True, eq=False)
class BpmAnalysisResult:
    """Detailed BPM analysis result.

    ``confidence`` is the winning tempo cluster's vote share within
    ``bpm_min``-``bpm_max`` (60-200 by default), searched at the input rate;
    :func:`analyze` searches at 22050 Hz, so its ``bpm_confidence`` can differ.
    ``candidates`` opens with the chosen tempo
    (``candidates[0].bpm == bpm``); the other histogram bins follow by
    descending vote share.
    """

    bpm: float | None
    """Chosen tempo, in beats per minute.

    ``None`` when no tempo was found, and then ``candidates`` is empty.
    """
    confidence: float
    """Winning tempo cluster's vote share, in ``[0, 1]`` (see the class note)."""
    candidates: list[BpmCandidate]
    """Tempo candidates: the chosen tempo first, then the other bins by descending vote share."""
    autocorrelation: npt.NDArray[np.float32]
    """Autocorrelation of the onset envelope over tempo lags."""
    tempogram: npt.NDArray[np.float32]
    """Tempogram of the onset envelope, flattened."""


@dataclass(frozen=True, slots=True)
class LufsResult:
    """ITU-R BS.1770 / EBU R128 loudness metrics.

    ``momentary_lufs`` and ``short_term_lufs`` are the final complete windows;
    the corresponding ``max_*`` fields report EBU R128 Max-M and Max-S.
    """

    integrated_lufs: float
    """Integrated loudness over the whole signal, in LUFS; -inf when nothing passes the gate."""
    momentary_lufs: float | None
    """Loudness of the final complete 400 ms window, in LUFS; not Max-M. -inf for zero energy,
    None when the signal is shorter than one window."""
    short_term_lufs: float | None
    """Loudness of the final complete 3 s window, in LUFS; not Max-S. -inf for zero energy,
    None when the signal is shorter than one window."""
    max_momentary_lufs: float | None
    """Maximum 400 ms window loudness (EBU R128 Max-M), in LUFS; -inf for zero energy, None when
    the signal is shorter than one window."""
    max_short_term_lufs: float | None
    """Maximum 3 s window loudness (EBU R128 Max-S), in LUFS; -inf for zero energy, None when
    the signal is shorter than one window."""
    loudness_range_lu: float | None
    """Loudness range, in LU; None when not measured."""


@dataclass(frozen=True, slots=True, eq=False)
class LufsSeriesResult:
    """Momentary and short-term loudness series; a window the signal is too short for is empty."""

    momentary_lufs: npt.NDArray[np.float32]
    """400 ms window loudness in LUFS as float32."""
    short_term_lufs: npt.NDArray[np.float32]
    """3 s window loudness in LUFS as float32."""


@dataclass(frozen=True, slots=True)
class ClippingRegion:
    """One contiguous run of clipped samples reported by detect_clipping."""

    start_sample: int
    """Index of the first clipped sample."""
    end_sample: int
    """Index one past the last clipped sample."""
    length: int
    """Run length, in samples."""
    peak: float
    """Largest absolute sample value in the run (linear)."""


@dataclass(frozen=True, slots=True)
class ClippingReport:
    """Aggregated clipping detection result (mirrors SonareClippingResult)."""

    clipped_samples: int
    """Number of samples at or past the threshold."""
    clipping_ratio: float
    """``clipped_samples`` divided by the input length."""
    max_clipped_peak: float
    """Largest absolute sample value over all reported regions (linear)."""
    regions: list[ClippingRegion]
    """The clipped runs, in order."""


@dataclass(frozen=True, slots=True, eq=False)
class DynamicRangeReport:
    """Sliding-window dynamic range report (mirrors SonareDynamicRangeResult)."""

    dynamic_range_db: float | None
    """High percentile minus low percentile of the window levels, in dB.

    ``inf`` when the low percentile lands on a silent window; ``None`` when the
    high percentile is silent too (silent input) or the input is empty.
    """
    low_percentile_db: float | None
    """Window RMS level at the low percentile, in dBFS.

    ``-inf`` on a silent window, ``None`` for empty input.
    """
    high_percentile_db: float | None
    """Window RMS level at the high percentile, in dBFS.

    ``-inf`` on a silent window, ``None`` for empty input.
    """
    window_rms_db: npt.NDArray[np.float32]
    """RMS level of each analysis window, in dBFS; a silent window is ``-inf``."""


@dataclass(frozen=True, slots=True)
class VectorscopeReport:
    """Mid/side vectorscope point series for a (left, right) stereo pair."""

    mid: NDArray[np.float32]
    """Mid component of each point."""
    side: NDArray[np.float32]
    """Side component of each point."""


@dataclass(frozen=True, slots=True)
class PhaseScopeReport:
    """Phase-scope (Lissajous) point series plus summary stats."""

    mid: NDArray[np.float32]
    """Mid component of each point."""
    side: NDArray[np.float32]
    """Side component of each point."""
    radius: NDArray[np.float32]
    """Distance of each point from the origin."""
    angle_rad: NDArray[np.float32]
    """Angle of each point, ``atan2(side, mid)``, in radians; ``NaN`` at a silent point, which has
    no angle. The array keeps full length: a typed array cannot hold absence, and dropping the
    element would break index alignment with ``mid``, ``side`` and ``radius``."""
    correlation: float | None
    """Stereo correlation over the full-resolution signal; ``None`` when either channel has zero
    energy."""
    average_abs_angle_rad: float | None
    """Mean absolute point angle over the non-silent points of the full-resolution signal, in
    radians; ``None`` when every point is silent."""
    max_radius: float
    """Largest point radius over the full-resolution signal."""


@dataclass(frozen=True, slots=True)
class SpectrumReport:
    """Single-frame magnitude / power / dB spectrum (mirrors SonareSpectrumResult)."""

    frequencies: NDArray[np.float32]
    """Center frequency of each bin, in Hz."""
    magnitude: NDArray[np.float32]
    """Linear magnitude of each bin."""
    power: NDArray[np.float32]
    """Power of each bin."""
    db: NDArray[np.float32]
    """Level of each bin, in dB."""
    n_fft: int
    """FFT size the spectrum was computed with."""
    sample_rate: int
    """Sample rate of the analyzed audio, in Hz."""


@dataclass(frozen=True, slots=True)
class WaveformPeaksReport:
    """Per-channel min/max waveform buckets. Arrays are channel-major."""

    min: NDArray[np.float32]
    """Per-bucket minima, channel-major: index ``channel * bucket_count + bucket``."""
    max: NDArray[np.float32]
    """Per-bucket maxima, channel-major: index ``channel * bucket_count + bucket``."""
    num_channels: int
    """Number of channels."""
    bucket_count: int
    """Number of buckets per channel."""
    samples_per_bucket: int
    """Bucket width, in samples."""


class StereoBlock(NamedTuple):
    """One processed stereo block; unpacks as ``left, right``."""

    left: NDArray[np.float32]
    """Processed left channel of the block."""
    right: NDArray[np.float32]
    """Processed right channel of the block."""


@dataclass(frozen=True, slots=True, eq=False)
class EqSpectrumSnapshot:
    """Realtime equalizer snapshot.

    ``pre_left``/``pre_right`` and ``post_left``/``post_right`` are the pre- and
    post-EQ waveform streams (uniformly decimated time-domain samples), so they
    are a scope feed rather than a spectral estimate. ``profile_db`` is the
    frequency-domain view: the post-EQ signal is Hann-windowed, transformed and
    its bin powers summed into 16 geometrically spaced bands covering 20 Hz to
    20 kHz, in amplitude decibels relative to full scale, rising immediately and
    falling smoothly.
    """

    pre_left: NDArray[np.float32]
    """Pre-EQ left waveform scope feed."""
    pre_right: NDArray[np.float32]
    """Pre-EQ right waveform scope feed."""
    post_left: NDArray[np.float32]
    """Post-EQ left waveform scope feed."""
    post_right: NDArray[np.float32]
    """Post-EQ right waveform scope feed."""
    band_gain_db: NDArray[np.float32]
    """Current gain of each band, in dB."""
    profile_db: NDArray[np.float32]
    """Post-EQ level of the 16 bands from 20 Hz to 20 kHz, in dB relative to full scale."""
    last_auto_gain_db: float
    """Most recent automatic gain, in dB."""
    seq: int
    """Snapshot sequence number."""


@dataclass(frozen=True, slots=True, eq=False)
class RhythmResult:
    """Rhythm analysis primitives."""

    bpm: float | None
    """The beat tracker's own tempo, in beats per minute; ``None`` when no tempo was found."""
    time_signature: TimeSignature
    """Estimated time signature."""
    groove_type: str
    """Rhythmic feel of the track, as a groove token."""
    syncopation: float | None
    """Syncopation level, in ``[0, 1]``.

    ``None`` below four tracked beats, with silent accents, or without both a
    strong- and a weak-position beat observed.
    """
    pattern_regularity: float | None
    """Regularity of the beat intervals, in ``[0, 1]``; ``None`` below two intervals."""
    tempo_stability: float | None
    """Tempo stability over time, in ``[0, 1]``; ``None`` below two positive intervals."""
    beat_intervals: npt.NDArray[np.float32]
    """Intervals between consecutive beats, in seconds."""


@dataclass(frozen=True, slots=True, eq=False)
class DynamicsResult:
    """Dynamics and loudness analysis primitives."""

    dynamic_range_db: float | None
    """Dynamic range, in dB; +inf when the low percentile window is silent.

    None when not measured.
    """
    peak_db: float
    """Peak level, in dB; -inf for silence."""
    rms_db: float
    """RMS level, in dB; -inf for silence."""
    crest_factor: float | None
    """Peak-to-RMS ratio of the signal; None when not measured."""
    loudness_range_lu: float | None
    """Loudness range, in LU; None when not measured."""
    is_compressed: bool
    """True when the signal is flagged as compressed."""
    loudness_times_sec: npt.NDArray[np.float32]
    """Loudness curve timestamps, in seconds, parallel to ``loudness_rms_db``."""
    loudness_rms_db: npt.NDArray[np.float32]
    """Loudness curve RMS values, in dB, parallel to ``loudness_times_sec``; -inf for an all-zero
    window."""


@dataclass(frozen=True, slots=True)
class TimbreFrame:
    """Timbre metrics for one analysis window in TimbreResult.timbre_over_time."""

    brightness: float | None
    """Spectral brightness, in ``[0, 1]``; high is bright."""
    warmth: float | None
    """Spectral warmth, in ``[0, 1]``; high is warm."""
    density: float | None
    """Spectral density, in ``[0, 1]``; high is rich."""
    roughness: float | None
    """Spectral roughness, in ``[0, 1]``; high is rough."""
    complexity: float | None
    """Harmonic complexity, in ``[0, 1]``; high is complex. Every timbre field is ``None`` where
    no frame carries spectral energy."""


@dataclass(frozen=True, slots=True, eq=False)
class TimbreResult:
    """Timbre and spectral-shape analysis primitives."""

    brightness: float | None
    """Spectral brightness, in ``[0, 1]``; high is bright."""
    warmth: float | None
    """Spectral warmth, in ``[0, 1]``; high is warm."""
    density: float | None
    """Spectral density, in ``[0, 1]``; high is rich."""
    roughness: float | None
    """Spectral roughness, in ``[0, 1]``; high is rough."""
    complexity: float | None
    """Harmonic complexity, in ``[0, 1]``; high is complex. Every timbre field is ``None`` where
    no frame carries spectral energy."""
    spectral_centroid: npt.NDArray[np.float32]
    """Spectral centroid per frame."""
    spectral_flatness: npt.NDArray[np.float32]
    """Spectral flatness per frame."""
    spectral_rolloff: npt.NDArray[np.float32]
    """Spectral rolloff per frame."""
    timbre_over_time: list[TimbreFrame]
    """Time-varying timbre metrics, one entry per analysis window."""


CHORD_QUALITIES: tuple[str, ...] = (
    "major",
    "minor",
    "diminished",
    "augmented",
    "dominant7",
    "major7",
    "minor7",
    "sus2",
    "sus4",
    "unknown",
    "add9",
    "minorAdd9",
    "dim7",
    "halfDim7",
    "major9",
    "dominant9",
    "sus2Add4",
    "major6",
    "minor6",
    "minorMajor7",
    "dominant7Sus4",
    "dominant11",
    "dominant13",
    "dominant7Flat9",
    "dominant7Sharp9",
)
"""Chord quality tokens, indexed by the C ``SonareChordQuality`` ordinal."""


def _chord_symbol(root: object, quality: str, bass: object | None = None) -> str:
    """Spell a chord symbol: ``"N.C."`` for ``unknown``, else root, suffix and slash bass."""
    if quality == "unknown":
        return "N.C."
    suffixes: dict[str, str] = {
        "major": "",
        "minor": "m",
        "diminished": "dim",
        "augmented": "aug",
        "dominant7": "7",
        "major7": "maj7",
        "minor7": "m7",
        "sus2": "sus2",
        "sus4": "sus4",
        "unknown": "",
        "add9": "add9",
        "minorAdd9": "madd9",
        "dim7": "dim7",
        "halfDim7": "m7b5",
        "major9": "maj9",
        "dominant9": "9",
        "sus2Add4": "sus2add4",
        "major6": "6",
        "minor6": "m6",
        "minorMajor7": "mM7",
        "dominant7Sus4": "7sus4",
        "dominant11": "11",
        "dominant13": "13",
        "dominant7Flat9": "7b9",
        "dominant7Sharp9": "7#9",
    }
    slash = "" if bass is None or bass == root else f"/{bass}"
    return f"{root}{suffixes.get(quality, '')}{slash}"


@dataclass(frozen=True, slots=True)
class Chord:
    """Detected chord with timing and confidence.

    ``name`` is the chord symbol (``"Am7"``, ``"C/E"``; ``"N.C."`` for no chord).
    The analysis functions fill it; a chord built by hand carries whatever
    ``name`` it is given.
    """

    root: PitchClass
    """Root pitch class."""
    quality: str
    """Chord quality token from :data:`CHORD_QUALITIES`; ``"unknown"`` if not named."""
    start_sec: float
    """Chord start time, in seconds."""
    end_sec: float
    """Chord end time, in seconds."""
    confidence: float
    """Correlation of the chroma with the chosen chord template, in ``[0, 1]``."""
    bass: PitchClass | None = None
    """Bass pitch class; ``None`` when no bass was estimated."""
    name: str = ""
    """Chord symbol, e.g. ``"Am7"``, ``"C/E"``, ``"N.C."``."""

    @property
    def duration_sec(self) -> float:
        """Chord length in seconds, ``end_sec - start_sec``."""
        return self.end_sec - self.start_sec

    @property
    def root_name(self) -> str:
        """Canonical core spelling, stable across all language bindings."""
        return str(self.root)

    @property
    def bass_name(self) -> str:
        """Canonical core spelling, stable across all language bindings."""
        return str(self.root if self.bass is None else self.bass)


@dataclass(frozen=True, slots=True)
class AnalysisChord(Chord):
    """A chord of :func:`analyze`, labelled with a Roman numeral in the analysis key.

    ``roman_numeral`` is empty for a chord that is N.C.
    """

    roman_numeral: str = ""
    """Roman numeral of the chord relative to the analysis key, e.g. ``"V7"``; empty for N.C."""


ChordFunctionName = Literal["tonic", "subdominant", "dominant", "chromatic", "none"]
"""Harmonic function of a chord relative to the key."""


@dataclass(frozen=True, slots=True)
class FunctionalChord(Chord):
    """A timed chord labelled relative to a key by :func:`chord_functions`.

    Every :class:`Chord` field is carried over unchanged. ``roman_numeral`` is
    the Roman numeral (``"I"``, ``"V7"``, ``"vi"``; ``"N.C."`` for an unknown
    chord) and ``function`` its harmonic function: ``"tonic"``,
    ``"subdominant"``, ``"dominant"``, ``"chromatic"`` for a root outside the
    key's scale, or ``"none"`` for an unknown chord.
    """

    roman_numeral: str = ""
    """Roman numeral relative to the key, e.g. ``"V7"``; ``"N.C."`` for an unknown chord."""
    function: ChordFunctionName = "none"


@dataclass(frozen=True, slots=True)
class ChordAnalysisResult:
    """Chord detection primitives.

    Attributes:
        chords: The detected chord timeline.
        tuning_cents: Tuning the chroma was built with, in cents:
            the given value, or the measured one under ``tuning_cents="auto"``.
    """

    chords: list[Chord]
    tuning_cents: float = 0.0


@dataclass(frozen=True, slots=True, eq=False)
class StftResult:
    """Short-time Fourier transform result.

    ``magnitude`` and ``power`` are float32 arrays of shape ``(n_bins, n_frames)``;
    ``db`` is the magnitude in dB with the same shape, or ``None`` unless the
    transform was asked for it.
    """

    n_fft: int
    """FFT size, in samples."""
    hop_length: int
    """Hop length between frames, in samples."""
    sample_rate: int
    """Sample rate of the analyzed audio, in Hz."""
    magnitude: NDArray[np.float32]
    """Magnitude spectrogram."""
    power: NDArray[np.float32]
    """Power spectrogram."""
    db: NDArray[np.float32] | None = None
    """Magnitude in dB, or ``None`` unless requested."""


@dataclass(frozen=True, slots=True, eq=False)
class MelSpectrogramResult:
    """Mel spectrogram result.

    Carries the parameters of the forward transform. ``power`` and ``db`` are
    float32 arrays of shape ``(n_mels, n_frames)``; ``power`` is linear power and
    ``db`` is its dB form.

    Attributes:
        n_fft: FFT size of the source STFT.
        fmin_hz: Lower Mel band edge in Hz as applied.
        fmax_hz: Upper Mel band edge in Hz as applied: the Nyquist when the
            request left it at 0.
        htk: ``True`` for the HTK Mel formula, ``False`` for Slaney.
    """

    sample_rate: int
    """Sample rate of the analyzed audio, in Hz."""
    hop_length: int
    """Hop length between frames, in samples."""
    power: NDArray[np.float32]
    """Linear-power Mel spectrogram, shape ``(n_mels, n_frames)``."""
    db: NDArray[np.float32]
    """Mel spectrogram in dB, shape ``(n_mels, n_frames)``."""
    n_fft: int
    fmin_hz: float
    fmax_hz: float
    htk: bool


@dataclass(frozen=True, slots=True, eq=False)
class MfccResult:
    """MFCC (Mel-frequency cepstral coefficients) result.

    Carries the Mel parameters the coefficients were derived with and the lifter.
    ``coefficients`` is a float32 array of shape ``(n_mfcc, n_frames)``.

    Attributes:
        n_mels: Mel bands the coefficients were taken from.
        fmax_hz: Upper Mel band edge in Hz as applied: the Nyquist when the request
            left it at 0.
        lifter: Cepstral lifter applied to the coefficients; 0 means none.
    """

    coefficients: NDArray[np.float32]
    """MFCC matrix, shape ``(n_mfcc, n_frames)``."""
    sample_rate: int
    """Sample rate of the analyzed audio, in Hz."""
    hop_length: int
    """Hop length between frames, in samples."""
    n_fft: int
    """FFT size of the source STFT."""
    n_mels: int
    fmin_hz: float
    """Lower Mel band edge, in Hz."""
    fmax_hz: float
    htk: bool
    """``True`` for the HTK Mel formula, ``False`` for Slaney."""
    lifter: float


@dataclass(frozen=True, slots=True, eq=False)
class ChromaResult:
    """Chroma feature result.

    ``features`` is a float32 array of shape ``(n_chroma, n_frames)`` and
    ``mean_energy`` holds the mean energy of each of the ``n_chroma`` classes.
    """

    sample_rate: int
    """Sample rate of the analyzed audio, in Hz."""
    hop_length: int
    """Hop length between frames, in samples."""
    features: NDArray[np.float32]
    """Chroma matrix, shape ``(n_chroma, n_frames)``."""
    mean_energy: NDArray[np.float32]
    """Mean energy of each chroma class, length ``n_chroma``."""


@dataclass(frozen=True, slots=True, eq=False)
class PitchResult:
    """Pitch detection result.

    ``voiced_flag`` is the voicing decision and the value every consumer should
    gate on.

    ``voiced_prob`` is pYIN's per-frame voiced *observation mass* (the same
    quantity librosa returns), NOT a signal-quality confidence and NOT a
    correction weight. The mass depends on how many periods of the pitch fit
    inside ``frame_length``, so for a fixed frame length it rises with F0 even
    when the signal is unchanged: a steady three-harmonic tone at 2048 samples /
    48 kHz averages well under 0.1 at C2 and about 0.5 at C5, with every frame
    flagged voiced throughout. Thresholding it at a fixed 0.5 (the default in
    :func:`note_segments`) therefore drops entire low registers.
    """

    f0: NDArray[np.float32]
    """Per-frame fundamental frequency, in Hz; unvoiced frames are NaN."""
    voiced_prob: NDArray[np.float32]
    """Per-frame voiced observation mass (see above)."""
    voiced_flag: NDArray[np.bool_]
    """Per-frame voicing decision."""
    median_f0: float | None
    """Median fundamental frequency of the voiced frames, in Hz; ``None`` when none is voiced."""
    mean_f0: float | None
    """Mean fundamental frequency of the voiced frames, in Hz; ``None`` when none is voiced."""


@dataclass(frozen=True, slots=True, eq=False)
class PiptrackResult:
    """Per-bin pitch candidates and peak magnitudes from spectral piptrack.

    Both are float32 arrays of shape ``(n_bins, n_frames)``.
    """

    pitches: NDArray[np.float32]
    """Pitch candidate per bin and frame."""
    magnitudes: NDArray[np.float32]
    """Peak magnitude per bin and frame."""


@dataclass(frozen=True, slots=True, eq=False)
class ReassignedSpectrogramResult:
    """Magnitude and reassigned coordinates of an STFT.

    All three are float32 arrays of shape ``(n_bins, n_frames)``.
    """

    magnitude: NDArray[np.float32]
    """STFT magnitude per bin and frame."""
    times: NDArray[np.float32]
    """Reassigned time of each bin and frame, in seconds."""
    frequencies: NDArray[np.float32]
    """Reassigned frequency of each bin and frame, in Hz."""


@dataclass(frozen=True, slots=True)
class NoteSegment:
    """One stable monophonic note region segmented from an F0 track."""

    frame_start: int
    """First F0 frame of the note."""
    frame_end: int
    """End of the note in F0 frames (exclusive)."""
    start_sec: float
    """Note start, in seconds."""
    end_sec: float
    """Note end, in seconds."""
    median_cents: float
    """Median pitch of the note, in cents."""


@dataclass(frozen=True, slots=True, eq=False)
class HpssResult:
    """Harmonic-percussive source separation result."""

    harmonic: npt.NDArray[np.float32]
    """Harmonic component, ``length`` samples."""
    percussive: npt.NDArray[np.float32]
    """Percussive component, ``length`` samples."""
    length: int
    """Length of each component, in samples."""
    sample_rate: int
    """Sample rate of both components, in Hz."""
    residual: npt.NDArray[np.float32] | None = None
    """What neither component claimed, ``length`` samples; ``None`` unless requested."""


@dataclass(frozen=True, slots=True)
class TrimRange:
    """One half-open sample range, in input-buffer coordinates.

    ``first == last_exclusive`` is an empty range, which is what a channel
    carrying nothing reports.
    """

    first: int
    """First kept sample."""
    last_exclusive: int
    """One past the last kept sample."""


@dataclass(frozen=True, slots=True)
class TrimReport:
    """What a trim pass kept and what it dropped.

    A pass that kept nothing reports the range ``(length, length)``, which
    counts the whole buffer as removed head and leaves ``removed_tail_samples``
    at 0. The two still sum to the input length, so a caller reporting how much
    went reads the right total; only the split between the ends is arbitrary
    there.
    """

    range: TrimRange
    """The kept range, padding included."""
    removed_head_samples: int
    """Samples dropped before ``range.first``."""
    removed_tail_samples: int
    """Samples dropped after ``range.last_exclusive``."""


@dataclass(frozen=True, slots=True, eq=False)
class TrimSilenceResult:
    """A trimmed mono buffer and the range it was cut to.

    A pass that kept nothing returns an empty ``samples`` -- a success, not a
    refusal; ``report`` says where the cut fell."""

    samples: NDArray[np.float32]
    """The kept audio, ``report.range.last_exclusive - report.range.first`` samples long."""
    report: TrimReport
    """What the pass kept and dropped."""


@dataclass(frozen=True, slots=True, eq=False)
class TrimSilenceStereoResult:
    """A trimmed stereo pair, the range both channels were cut to, and the two
    per-channel scans that range is the union of.

    ``length`` is the OUTPUT length, not the input length every other stereo
    repair result echoes: trimming shortens the pair, so this field is the only
    thing that says how much came back. When neither channel carries signal both
    lists are empty and ``length`` is 0 -- a success, not a refusal.

    ``report.range`` is the union that was applied to BOTH channels.
    ``left_range`` and ``right_range`` are the per-channel scans it was formed
    from, so a caller can see which channel decided each edge.
    """

    left: npt.NDArray[np.float32]
    """Left channel, cut to the shared range."""
    right: npt.NDArray[np.float32]
    """Right channel, cut to the shared range."""
    length: int
    """Output length of each channel, in samples."""
    report: TrimReport
    """What the pass kept and dropped; its range is the union applied to both channels."""
    left_range: TrimRange
    """Range the left channel's own scan found."""
    right_range: TrimRange
    """Range the right channel's own scan found."""


@dataclass(frozen=True, slots=True, eq=False)
class NormalizeStereoResult:
    """A normalized channel pair.

    The level is measured across both channels and one gain goes to both, which
    keeps the stereo balance intact. A pair that is already silent comes back
    untouched.
    """

    left: npt.NDArray[np.float32]
    """Normalized left channel."""
    right: npt.NDArray[np.float32]
    """Normalized right channel."""
    length: int
    """Length of each channel, in samples."""
