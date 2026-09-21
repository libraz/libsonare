"""Analysis result shapes: key, beats, chords, spectra, metering and stems.

camelCase property aliases mirror the JS binding's public API so users moving
between languages see the same names. They intentionally violate PEP8 N802.
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from ._types_enums import Mode, PitchClass

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray

    from ._types_engine import MelodyPoint, Section


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
    mode: Mode
    confidence: float

    @property
    def name(self) -> str:
        return f"{self.root} {self.mode}"

    @property
    def short_name(self) -> str:
        if self.mode == Mode.MAJOR:
            return f"{self.root}"
        if self.mode == Mode.MINOR:
            return f"{self.root}m"
        return f"{self.root} {self.mode}"

    @property
    def shortName(self) -> str:  # noqa: N802
        return self.short_name

    def __str__(self) -> str:
        return self.name


@dataclass(frozen=True, slots=True)
class KeyCandidate:
    """Key candidate with raw profile correlation."""

    key: Key
    correlation: float


@dataclass(frozen=True, slots=True)
class TimeSignature:
    """Detected time signature."""

    numerator: int
    denominator: int
    confidence: float

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

    time: float
    strength: float | None = None


@dataclass(frozen=True, slots=True)
class BpmHypothesis:
    """A tempo hypothesis retained by the unified music analysis."""

    value: float
    confidence: float
    relation: Literal["primary", "half", "double", "other"]


@dataclass(frozen=True, slots=True)
class AnalysisDynamics:
    """Dynamics summary embedded in :class:`AnalysisResult`."""

    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_db: float
    is_compressed: bool

    @property
    def dynamicRangeDb(self) -> float:  # noqa: N802
        return self.dynamic_range_db

    @property
    def peakDb(self) -> float:  # noqa: N802
        return self.peak_db

    @property
    def rmsDb(self) -> float:  # noqa: N802
        return self.rms_db

    @property
    def crestFactor(self) -> float:  # noqa: N802
        return self.crest_factor

    @property
    def loudnessRangeDb(self) -> float:  # noqa: N802
        return self.loudness_range_db

    @property
    def isCompressed(self) -> bool:  # noqa: N802
        return self.is_compressed


@dataclass(frozen=True, slots=True)
class AnalysisTimbre:
    """Timbre summary embedded in :class:`AnalysisResult`."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float


@dataclass(frozen=True, slots=True)
class AnalysisRhythm:
    """Rhythm summary embedded in :class:`AnalysisResult`."""

    time_signature: TimeSignature
    syncopation: float
    groove_type: str
    pattern_regularity: float
    tempo_stability: float

    @property
    def timeSignature(self) -> TimeSignature:  # noqa: N802
        return self.time_signature

    @property
    def grooveType(self) -> str:  # noqa: N802
        return self.groove_type

    @property
    def patternRegularity(self) -> float:  # noqa: N802
        return self.pattern_regularity

    @property
    def tempoStability(self) -> float:  # noqa: N802
        return self.tempo_stability


@dataclass(frozen=True, slots=True)
class AnalysisMelody:
    """Melody summary embedded in :class:`AnalysisResult`."""

    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
    pitches: list[MelodyPoint]

    @property
    def pitchRangeOctaves(self) -> float:  # noqa: N802
        return self.pitch_range_octaves

    @property
    def pitchStability(self) -> float:  # noqa: N802
        return self.pitch_stability

    @property
    def meanFrequency(self) -> float:  # noqa: N802
        return self.mean_frequency

    @property
    def vibratoRate(self) -> float:  # noqa: N802
        return self.vibrato_rate


@dataclass(frozen=True, slots=True)
class AnalysisBeatObservations:
    """Beat-level evidence behind the downbeat and meter decisions.

    These are the inputs the library's own downbeat and meter pass scores, not
    outputs of it. Each stream is parallel to ``AnalysisResult.beat_times``,
    one value per beat. An empty stream means the analysis could not produce
    it, *not* that every beat scored zero: ``low_frequency_energy`` is empty
    when the analysis ran without audio, and ``chord_change`` is empty until
    chords have been analyzed.
    """

    # Windowed onset aggregate around each beat — the accent value the downbeat
    # pass scores, and the strength source :func:`libsonare.estimate_meter`
    # expects. Distinct from ``AnalysisResult.beat_strengths``, which is a
    # single raw unwindowed envelope frame at the beat's frame.
    onset_strength: list[float] = dataclasses.field(default_factory=list)
    low_frequency_energy: list[float] = dataclasses.field(default_factory=list)
    chord_change: list[float] = dataclasses.field(default_factory=list)

    @property
    def onsetStrength(self) -> list[float]:  # noqa: N802
        return self.onset_strength

    @property
    def lowFrequencyEnergy(self) -> list[float]:  # noqa: N802
        return self.low_frequency_energy

    @property
    def chordChange(self) -> list[float]:  # noqa: N802
        return self.chord_change


@dataclass(frozen=True, slots=True)
class AnalysisResult:
    """Full audio analysis result."""

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beat_times: list[float]
    # Extended fields (populated when sonare_analyze_json is available).
    # timbre/dynamics/rhythm/melody are intentionally Optional here, unlike the
    # Node/WASM surfaces which type them as required: Python keeps a legacy
    # fallback to the older flat sonare_analyze struct (for builds without
    # sonare_analyze_json) that returns only bpm/key/time-signature/beats and
    # leaves these four unset. The compiled bindings always ship the JSON path,
    # so they can guarantee presence; Python cannot without dropping the
    # fallback. Guard with `is not None` before use.
    # One raw onset-envelope frame per beat, sampled at the beat's own frame.
    # Not normalized and not comparable across material, and it moves with
    # beat-position jitter — ``beat_observations.onset_strength`` is the
    # windowed aggregate to score accents with.
    beat_strengths: list[float] = dataclasses.field(default_factory=list)
    # Positions of the bar starts within ``beat_times``, so
    # ``beat_times[downbeat_indices[k]]`` is the k-th downbeat. This is an
    # index list, not a parallel array: it is shorter than ``beat_times``, and
    # testing a beat for downbeat status is a membership check on it rather
    # than a time comparison against a separate downbeat series.
    downbeat_indices: list[int] = dataclasses.field(default_factory=list)
    # Which beat of the first bar the analysis starts on. The meter estimator's
    # own phase, so it can disagree with ``downbeat_indices[0]`` once downbeats
    # are refined from chord and low-frequency evidence.
    downbeat_phase: int = 0
    bpm_candidates: list[BpmHypothesis] = dataclasses.field(default_factory=list)
    time_signature_candidates: list[TimeSignature] = dataclasses.field(default_factory=list)
    chords: list[Chord] = dataclasses.field(default_factory=list)
    sections: list[Section] = dataclasses.field(default_factory=list)
    timbre: AnalysisTimbre | None = None
    dynamics: AnalysisDynamics | None = None
    rhythm: AnalysisRhythm | None = None
    melody: AnalysisMelody | None = None
    beat_observations: AnalysisBeatObservations | None = None
    # Smoothed local tempo at each beat, in BPM, parallel to ``beat_times``.
    # Empty unless ``compute_tempo_curve`` was set, and empty regardless when
    # fewer than two beats were detected, since a tempo is a property of the
    # interval between two beats. The last entry repeats the tempo of the
    # interval leading into the final beat, which opens no interval of its own.
    # This is the local tempo rather than ``bpm`` resampled: on material whose
    # tempo moves it departs from ``bpm``, and reading a single number out of it
    # is not how to get the global tempo.
    beat_local_bpm: list[float] = dataclasses.field(default_factory=list)
    form: str = ""

    @property
    def bpmConfidence(self) -> float:  # noqa: N802
        return self.bpm_confidence

    @property
    def timeSignature(self) -> TimeSignature:  # noqa: N802
        return self.time_signature

    @property
    def bpmCandidates(self) -> list[BpmHypothesis]:  # noqa: N802
        return self.bpm_candidates

    @property
    def timeSignatureCandidates(self) -> list[TimeSignature]:  # noqa: N802
        return self.time_signature_candidates

    @property
    def beatTimes(self) -> list[float]:  # noqa: N802
        return self.beat_times

    @property
    def beatStrengths(self) -> list[float]:  # noqa: N802
        return self.beat_strengths

    @property
    def downbeatIndices(self) -> list[int]:  # noqa: N802
        return self.downbeat_indices

    @property
    def downbeatPhase(self) -> int:  # noqa: N802
        return self.downbeat_phase

    @property
    def beatObservations(self) -> AnalysisBeatObservations | None:  # noqa: N802
        return self.beat_observations

    @property
    def beatLocalBpm(self) -> list[float]:  # noqa: N802
        return self.beat_local_bpm

    @property
    def beats(self) -> list[Beat]:
        if self.beat_strengths:
            return [
                Beat(time=t, strength=s)
                for t, s in zip(self.beat_times, self.beat_strengths, strict=False)
            ]
        return [Beat(time=t) for t in self.beat_times]


@dataclass(frozen=True, slots=True)
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

    ``grouping`` is how the bar divides, in beats per accent group: ``[3, 2, 2]``
    is the 7/8 an aksak meter notates as 3+2+2, and ``[2, 2]`` an ordinary four.
    It always sums to ``time_signature.numerator``. A single entry means no
    internal division was resolved — the numerator has none to find, it was too
    wide to search, or the span was too short to search at all.

    ``searched`` separates a measurement from the fixed fallback: it is False
    when the beat series was too short to score any candidate, and then every
    other field carries that fallback rather than a result. Read it before
    treating a short span's answer as a detection — the confidence reported
    alongside it is the fallback's fixed value, not a measurement.
    """

    time_signature: TimeSignature
    downbeat_phase: int
    searched: bool
    grouping: list[int] = dataclasses.field(default_factory=list)
    candidate_scores: list[float] = dataclasses.field(default_factory=list)
    candidates: list[TimeSignature] = dataclasses.field(default_factory=list)

    @property
    def timeSignature(self) -> TimeSignature:  # noqa: N802
        return self.time_signature

    @property
    def downbeatPhase(self) -> int:  # noqa: N802
        return self.downbeat_phase

    @property
    def candidateScores(self) -> list[float]:  # noqa: N802
        return self.candidate_scores


@dataclass(frozen=True, slots=True)
class BpmCandidate:
    """BPM candidate with confidence."""

    bpm: float
    confidence: float


@dataclass(frozen=True, slots=True)
class BpmAnalysisResult:
    """Detailed BPM analysis result."""

    bpm: float
    confidence: float
    candidates: list[BpmCandidate]
    autocorrelation: list[float]
    tempogram: list[float]


@dataclass(frozen=True, slots=True)
class LufsResult:
    """ITU-R BS.1770 / EBU R128 loudness metrics.

    ``momentary_lufs`` and ``short_term_lufs`` are the final complete windows;
    the corresponding ``max_*`` fields report EBU R128 Max-M and Max-S.
    """

    integrated_lufs: float
    momentary_lufs: float
    short_term_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    loudness_range: float

    @property
    def integratedLufs(self) -> float:  # noqa: N802
        return self.integrated_lufs

    @property
    def momentaryLufs(self) -> float:  # noqa: N802
        return self.momentary_lufs

    @property
    def shortTermLufs(self) -> float:  # noqa: N802
        return self.short_term_lufs

    @property
    def maxMomentaryLufs(self) -> float:  # noqa: N802
        return self.max_momentary_lufs

    @property
    def maxShortTermLufs(self) -> float:  # noqa: N802
        return self.max_short_term_lufs

    @property
    def loudnessRange(self) -> float:  # noqa: N802
        return self.loudness_range


@dataclass(frozen=True, slots=True)
class ClippingRegion:
    """One contiguous run of clipped samples reported by detect_clipping."""

    start_sample: int
    end_sample: int
    length: int
    peak: float


@dataclass(frozen=True, slots=True)
class ClippingReport:
    """Aggregated clipping detection result (mirrors SonareClippingResult)."""

    clipped_samples: int
    clipping_ratio: float
    max_clipped_peak: float
    regions: list[ClippingRegion]


@dataclass(frozen=True, slots=True)
class DynamicRangeReport:
    """Sliding-window dynamic range report (mirrors SonareDynamicRangeResult)."""

    dynamic_range_db: float
    low_percentile_db: float
    high_percentile_db: float
    window_rms_db: list[float]


@dataclass(frozen=True, slots=True)
class VectorscopeReport:
    """Mid/side vectorscope point series for a (left, right) stereo pair."""

    mid: NDArray[np.float32]
    side: NDArray[np.float32]


@dataclass(frozen=True, slots=True)
class PhaseScopeReport:
    """Phase-scope (Lissajous) point series plus summary stats."""

    mid: NDArray[np.float32]
    side: NDArray[np.float32]
    radius: NDArray[np.float32]
    angle_rad: NDArray[np.float32]
    correlation: float
    average_abs_angle_rad: float
    max_radius: float


@dataclass(frozen=True, slots=True)
class SpectrumReport:
    """Single-frame magnitude / power / dB spectrum (mirrors SonareSpectrumResult)."""

    frequencies: NDArray[np.float32]
    magnitude: NDArray[np.float32]
    power: NDArray[np.float32]
    db: NDArray[np.float32]
    n_fft: int
    sample_rate: int


@dataclass(frozen=True, slots=True)
class WaveformPeaksReport:
    """Per-channel min/max waveform buckets. Arrays are channel-major."""

    min: NDArray[np.float32]
    max: NDArray[np.float32]
    channels: int
    bucket_count: int
    samples_per_bucket: int


@dataclass(frozen=True, slots=True)
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

    pre_left: list[float]
    pre_right: list[float]
    post_left: list[float]
    post_right: list[float]
    band_gain_db: list[float]
    profile_db: list[float]
    last_auto_gain_db: float
    seq: int

    @property
    def preLeft(self) -> list[float]:  # noqa: N802
        return self.pre_left

    @property
    def preRight(self) -> list[float]:  # noqa: N802
        return self.pre_right

    @property
    def postLeft(self) -> list[float]:  # noqa: N802
        return self.post_left

    @property
    def postRight(self) -> list[float]:  # noqa: N802
        return self.post_right

    @property
    def bandGainDb(self) -> list[float]:  # noqa: N802
        return self.band_gain_db

    @property
    def profileDb(self) -> list[float]:  # noqa: N802
        return self.profile_db

    @property
    def lastAutoGainDb(self) -> float:  # noqa: N802
        return self.last_auto_gain_db


@dataclass(frozen=True, slots=True)
class RhythmResult:
    """Rhythm analysis primitives."""

    bpm: float
    time_signature: TimeSignature
    groove_type: str
    syncopation: float
    pattern_regularity: float
    tempo_stability: float
    beat_intervals: list[float]

    @property
    def timeSignature(self) -> TimeSignature:  # noqa: N802
        return self.time_signature

    @property
    def grooveType(self) -> str:  # noqa: N802
        return self.groove_type

    @property
    def patternRegularity(self) -> float:  # noqa: N802
        return self.pattern_regularity

    @property
    def tempoStability(self) -> float:  # noqa: N802
        return self.tempo_stability

    @property
    def beatIntervals(self) -> list[float]:  # noqa: N802
        return self.beat_intervals


@dataclass(frozen=True, slots=True)
class DynamicsResult:
    """Dynamics and loudness analysis primitives."""

    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_db: float
    is_compressed: bool
    loudness_times: list[float]
    loudness_rms_db: list[float]

    @property
    def dynamicRangeDb(self) -> float:  # noqa: N802
        return self.dynamic_range_db

    @property
    def peakDb(self) -> float:  # noqa: N802
        return self.peak_db

    @property
    def rmsDb(self) -> float:  # noqa: N802
        return self.rms_db

    @property
    def crestFactor(self) -> float:  # noqa: N802
        return self.crest_factor

    @property
    def loudnessRangeDb(self) -> float:  # noqa: N802
        return self.loudness_range_db

    @property
    def isCompressed(self) -> bool:  # noqa: N802
        return self.is_compressed

    @property
    def loudnessTimes(self) -> list[float]:  # noqa: N802
        return self.loudness_times

    @property
    def loudnessRmsDb(self) -> list[float]:  # noqa: N802
        return self.loudness_rms_db


@dataclass(frozen=True, slots=True)
class TimbreFrame:
    """Timbre metrics for one analysis window in TimbreResult.timbre_over_time."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float


@dataclass(frozen=True, slots=True)
class TimbreResult:
    """Timbre and spectral-shape analysis primitives."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    spectral_centroid: list[float]
    spectral_flatness: list[float]
    spectral_rolloff: list[float]
    timbre_over_time: list[TimbreFrame]

    @property
    def spectralCentroid(self) -> list[float]:  # noqa: N802
        return self.spectral_centroid

    @property
    def spectralFlatness(self) -> list[float]:  # noqa: N802
        return self.spectral_flatness

    @property
    def spectralRolloff(self) -> list[float]:  # noqa: N802
        return self.spectral_rolloff

    @property
    def timbreOverTime(self) -> list[TimbreFrame]:  # noqa: N802
        return self.timbre_over_time


@dataclass(frozen=True, slots=True)
class Chord:
    """Detected chord with timing and confidence."""

    root: PitchClass
    quality: str
    start: float
    end: float
    confidence: float
    bass: PitchClass | None = None
    canonical_name: str = ""

    @property
    def duration(self) -> float:
        return self.end - self.start

    @property
    def root_name(self) -> str:
        """Canonical core spelling, stable across all language bindings."""
        return str(self.root)

    @property
    def bass_name(self) -> str:
        """Canonical core spelling, stable across all language bindings."""
        return str(self.root if self.bass is None else self.bass)

    @property
    def rootName(self) -> str:  # noqa: N802
        return self.root_name

    @property
    def bassName(self) -> str:  # noqa: N802
        return self.bass_name

    @property
    def name(self) -> str:
        if self.canonical_name:
            return self.canonical_name
        if self.quality == "unknown":
            return "N.C."
        suffixes = {
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
        bass = self.root if self.bass is None else self.bass
        slash = "" if bass == self.root else f"/{bass}"
        return f"{self.root}{suffixes.get(self.quality, '')}{slash}"


@dataclass(frozen=True, slots=True)
class ChordAnalysisResult:
    """Chord detection primitives."""

    chords: list[Chord]


@dataclass(frozen=True, slots=True)
class StftResult:
    """Short-time Fourier transform result."""

    n_bins: int
    n_frames: int
    n_fft: int
    hop_length: int
    sample_rate: int
    magnitude: list[float]
    power: list[float]


@dataclass(frozen=True, slots=True)
class MelSpectrogramResult:
    """Mel spectrogram result."""

    n_mels: int
    n_frames: int
    sample_rate: int
    hop_length: int
    power: list[float]
    db: list[float]


@dataclass(frozen=True, slots=True)
class MfccResult:
    """MFCC (Mel-frequency cepstral coefficients) result."""

    n_mfcc: int
    n_frames: int
    coefficients: list[float]


@dataclass(frozen=True, slots=True)
class ChromaResult:
    """Chroma feature result."""

    n_chroma: int
    n_frames: int
    sample_rate: int
    hop_length: int
    features: list[float]
    mean_energy: list[float]


@dataclass(frozen=True, slots=True)
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

    n_frames: int
    f0: list[float]
    voiced_prob: list[float]
    voiced_flag: list[bool]
    median_f0: float
    mean_f0: float


@dataclass(frozen=True, slots=True)
class PiptrackResult:
    """Per-bin pitch candidates and peak magnitudes from spectral piptrack."""

    n_bins: int
    n_frames: int
    pitches: list[float]
    magnitudes: list[float]


@dataclass(frozen=True, slots=True)
class ReassignedSpectrogramResult:
    """Magnitude and reassigned coordinates for a row-major STFT matrix."""

    n_bins: int
    n_frames: int
    magnitude: list[float]
    times: list[float]
    frequencies: list[float]


@dataclass(frozen=True, slots=True)
class SegmentMatrix:
    """A row-major matrix returned by a ``librosa.segment``-compatible API."""

    rows: int
    cols: int
    values: list[float]


@dataclass(frozen=True, slots=True)
class NoteSegment:
    """One stable monophonic note region segmented from an F0 track."""

    frame_start: int
    frame_end: int
    start_seconds: float
    end_seconds: float
    median_cents: float


@dataclass(frozen=True, slots=True)
class HpssResult:
    """Harmonic-percussive source separation result."""

    harmonic: list[float]
    percussive: list[float]
    length: int
    sample_rate: int


@dataclass(frozen=True, slots=True)
class TrimRange:
    """One half-open sample range, in input-buffer coordinates.

    ``first == last_exclusive`` is an empty range, which is what a channel
    carrying nothing reports.
    """

    first: int
    last_exclusive: int


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
    removed_head_samples: int
    removed_tail_samples: int


@dataclass(frozen=True, slots=True)
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

    left: list[float]
    right: list[float]
    length: int
    report: TrimReport
    left_range: TrimRange
    right_range: TrimRange


@dataclass(frozen=True, slots=True)
class NormalizeStereoResult:
    """A normalized channel pair and the one gain that produced it.

    ``applied_gain_db`` is a single figure rather than one per channel: the
    level is measured across both channels and the gain goes to both, which is
    what keeps the stereo balance intact. A pair that is already silent comes
    back untouched with the gain at 0.
    """

    left: list[float]
    right: list[float]
    length: int
    applied_gain_db: float
