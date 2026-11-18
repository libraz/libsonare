"""Analysis result shapes: key, beats, chords, spectra, metering and stems."""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal, NamedTuple

import numpy as np

from ._types_enums import KeyMode, PitchClass

if TYPE_CHECKING:
    from numpy.typing import NDArray

    from ._types_engine import MelodyPoint, Section


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
        return f"{self.root} {self.mode}"

    @property
    def short_name(self) -> str:
        if self.mode == "major":
            return f"{self.root}"
        if self.mode == "minor":
            return f"{self.root}m"
        return f"{self.root} {self.mode}"

    def __str__(self) -> str:
        return self.name


@dataclass(frozen=True, slots=True)
class AutoTuneResult:
    """Result of :func:`auto_tune`.

    Attributes:
        samples: The corrected audio, the length of the input.
        key: The key the audio was tuned to: the detected one (with the model's
            own ``confidence``, see :class:`Key`) or the one the call named
            (``confidence`` 1).
    """

    samples: list[float]
    key: Key


@dataclass(frozen=True, slots=True)
class KeyCandidate:
    """Key candidate with raw profile correlation."""

    key: Key
    correlation: float


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
    strength: float


@dataclass(frozen=True, slots=True)
class AnalysisDynamics:
    """Dynamics summary embedded in :class:`AnalysisResult`."""

    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_lu: float
    is_compressed: bool


@dataclass(frozen=True, slots=True)
class AnalysisRhythm:
    """Rhythm summary embedded in :class:`AnalysisResult`."""

    time_signature: TimeSignature
    syncopation: float
    groove_type: str
    pattern_regularity: float
    tempo_stability: float


@dataclass(frozen=True, slots=True)
class AnalysisMelody:
    """Melody summary embedded in :class:`AnalysisResult`."""

    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency_hz: float
    vibrato_rate_hz: float
    pitches: list[MelodyPoint]


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

    onset_strength: np.ndarray = dataclasses.field(default_factory=_empty_float32)
    low_frequency_energy: np.ndarray = dataclasses.field(default_factory=_empty_float32)
    chord_change: np.ndarray = dataclasses.field(default_factory=_empty_float32)


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

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beats: list[Beat] = dataclasses.field(default_factory=list)
    # The bar starts, drawn from ``beats``: each is the beat that opens a bar.
    downbeats: list[Beat] = dataclasses.field(default_factory=list)
    # Which beat of the first bar the analysis starts on. The meter estimator's
    # own phase, so it can disagree with the first downbeat once downbeats are
    # refined from chord and low-frequency evidence.
    downbeat_phase: int = 0
    bpm_candidates: list[BpmCandidate] = dataclasses.field(default_factory=list)
    time_signature_candidates: list[TimeSignature] = dataclasses.field(default_factory=list)
    chords: list[AnalysisChord] = dataclasses.field(default_factory=list)
    sections: list[Section] = dataclasses.field(default_factory=list)
    # timbre/dynamics/rhythm/melody are intentionally Optional here, unlike the
    # Node/WASM surfaces which type them as required: Python keeps a legacy
    # fallback to the older flat sonare_analyze struct (for builds without
    # sonare_analyze_json) that returns only bpm/key/time-signature/beats and
    # leaves these four unset. Guard with `is not None` before use.
    timbre: TimbreFrame | None = None
    dynamics: AnalysisDynamics | None = None
    rhythm: AnalysisRhythm | None = None
    melody: AnalysisMelody | None = None
    beat_observations: BeatObservations | None = None
    # Smoothed local tempo at each beat, in BPM, parallel to ``beats``.
    # Empty unless ``compute_tempo_curve`` was set, and empty regardless when
    # fewer than two beats were detected, since a tempo is a property of the
    # interval between two beats. The last entry repeats the tempo of the
    # interval leading into the final beat, which opens no interval of its own.
    # Values are continuous, not points on a tempo grid: each is a weighted
    # average, in log tempo, of the beat intervals around it. This is the local
    # tempo rather than ``bpm`` resampled: on material whose tempo moves it
    # departs from ``bpm``.
    beat_local_bpm: np.ndarray = dataclasses.field(default_factory=_empty_float32)
    form: str = ""
    # Tuning the chromagrams were built with, in cents: the
    # given value, or the measured one under ``tuning_cents="auto"``.
    tuning_cents: float = 0.0


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
    downbeat_phase: int
    searched: bool
    grouping: np.ndarray = dataclasses.field(default_factory=_empty_int32)
    candidate_scores: np.ndarray = dataclasses.field(default_factory=_empty_float32)
    candidates: list[TimeSignature] = dataclasses.field(default_factory=list)


@dataclass(frozen=True, slots=True)
class BpmCandidate:
    """A tempo candidate with its confidence.

    ``relation`` is the candidate's relation to the chosen tempo --
    ``"primary"``, ``"half"``, ``"double"`` or ``"other"`` -- on the candidates
    of :class:`AnalysisResult`, and ``None`` on those of :func:`analyze_bpm`.
    """

    bpm: float
    confidence: float
    relation: str | None = None


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

    bpm: float
    confidence: float
    candidates: list[BpmCandidate]
    autocorrelation: np.ndarray
    tempogram: np.ndarray


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
    loudness_range_lu: float


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


class StereoBlock(NamedTuple):
    """One processed stereo block; unpacks as ``left, right``."""

    left: NDArray[np.float32]
    right: NDArray[np.float32]


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
    pre_right: NDArray[np.float32]
    post_left: NDArray[np.float32]
    post_right: NDArray[np.float32]
    band_gain_db: NDArray[np.float32]
    profile_db: NDArray[np.float32]
    last_auto_gain_db: float
    seq: int


@dataclass(frozen=True, slots=True, eq=False)
class RhythmResult:
    """Rhythm analysis primitives."""

    bpm: float
    time_signature: TimeSignature
    groove_type: str
    syncopation: float
    pattern_regularity: float
    tempo_stability: float
    beat_intervals: np.ndarray


@dataclass(frozen=True, slots=True, eq=False)
class DynamicsResult:
    """Dynamics and loudness analysis primitives."""

    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_lu: float
    is_compressed: bool
    loudness_times_sec: np.ndarray
    loudness_rms_db: np.ndarray


@dataclass(frozen=True, slots=True)
class TimbreFrame:
    """Timbre metrics for one analysis window in TimbreResult.timbre_over_time."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float


@dataclass(frozen=True, slots=True, eq=False)
class TimbreResult:
    """Timbre and spectral-shape analysis primitives."""

    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    spectral_centroid: np.ndarray
    spectral_flatness: np.ndarray
    spectral_rolloff: np.ndarray
    timbre_over_time: list[TimbreFrame]


#: Chord quality tokens, indexed by the C ``SonareChordQuality`` ordinal.
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
    quality: str
    start_sec: float
    end_sec: float
    confidence: float
    bass: PitchClass | None = None
    name: str = ""

    @property
    def duration_sec(self) -> float:
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


ChordFunctionName = Literal["tonic", "subdominant", "dominant", "chromatic", "none"]


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

    ``magnitude`` and ``power`` are float32 arrays of shape ``(n_bins, n_frames)``.
    """

    n_fft: int
    hop_length: int
    sample_rate: int
    magnitude: NDArray[np.float32]
    power: NDArray[np.float32]


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
    hop_length: int
    power: NDArray[np.float32]
    db: NDArray[np.float32]
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
    sample_rate: int
    hop_length: int
    n_fft: int
    n_mels: int
    fmin_hz: float
    fmax_hz: float
    htk: bool
    lifter: float


@dataclass(frozen=True, slots=True, eq=False)
class ChromaResult:
    """Chroma feature result.

    ``features`` is a float32 array of shape ``(n_chroma, n_frames)`` and
    ``mean_energy`` holds the mean energy of each of the ``n_chroma`` classes.
    """

    sample_rate: int
    hop_length: int
    features: NDArray[np.float32]
    mean_energy: NDArray[np.float32]


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
    voiced_prob: NDArray[np.float32]
    voiced_flag: NDArray[np.bool_]
    median_f0: float
    mean_f0: float


@dataclass(frozen=True, slots=True, eq=False)
class PiptrackResult:
    """Per-bin pitch candidates and peak magnitudes from spectral piptrack.

    Both are float32 arrays of shape ``(n_bins, n_frames)``.
    """

    pitches: NDArray[np.float32]
    magnitudes: NDArray[np.float32]


@dataclass(frozen=True, slots=True, eq=False)
class ReassignedSpectrogramResult:
    """Magnitude and reassigned coordinates of an STFT.

    All three are float32 arrays of shape ``(n_bins, n_frames)``.
    """

    magnitude: NDArray[np.float32]
    times: NDArray[np.float32]
    frequencies: NDArray[np.float32]


@dataclass(frozen=True, slots=True)
class NoteSegment:
    """One stable monophonic note region segmented from an F0 track."""

    frame_start: int
    frame_end: int
    start_sec: float
    end_sec: float
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


@dataclass(frozen=True, slots=True, eq=False)
class TrimSilenceResult:
    """A trimmed mono buffer and the range it was cut to.

    A pass that kept nothing returns an empty ``samples`` -- a success, not a
    refusal; ``report`` says where the cut fell."""

    samples: NDArray[np.float32]
    report: TrimReport


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
