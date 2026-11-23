/** Pitch class, C = 0 through B = 11. */
export type PitchClass = 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11;

/** A detected musical key. */
export interface Key {
  /** Pitch class of the tonic (C = 0). */
  root: PitchClass;
  /** Mode name (`'major'`, `'minor'`, `'dorian'`, ...). */
  mode: KeyMode;
  /**
   * Share of the model's belief that this key is the answer, in `[0, 1)`.
   *
   * A softmax over the profile correlations of every candidate that was scored,
   * so it falls as the runner-up closes in and two keys that split the evidence
   * -- a relative major and minor, typically -- each report about half.
   *
   * This is the model's own belief, **not** a measured accuracy. It says how
   * decisively the chroma picked this key out of the candidate set; it does not
   * say how often that pick is right, and nothing here has been fitted against
   * annotated recordings. A confident wrong answer is entirely possible, so a
   * pipeline that branches on it must choose its own threshold against its own
   * material.
   */
  confidence: number;
  /** Full key name, e.g. `'C major'`. */
  name: string;
  /** Short key name: the tonic for major, with an `m` suffix for minor, e.g. `'C'`, `'Am'`. */
  shortName: string;
}

/** A key mode by name. */
export type KeyMode =
  | 'major'
  | 'minor'
  | 'dorian'
  | 'phrygian'
  | 'lydian'
  | 'mixolydian'
  | 'locrian';

/** Autocorrelation or cosine similarity for a tempogram. */
export type TempogramMode = 'autocorrelation' | 'cosine';

/** Normalisation applied to a vector: by its maximum, its L1 or L2 norm, or its power. */
export type NormType = 'max' | 'l1' | 'l2' | 'power';

/** Rhythmic feel reported by {@link analyzeRhythm}. */
export type GrooveType = 'straight' | 'shuffle' | 'swing';

/** How a tempo candidate relates to the primary tempo. */
export type BpmCandidateRelation = 'primary' | 'half' | 'double' | 'other';

/** Genre hint that picks the key-profile family; an unknown value is a `RangeError`. */
export type GenreHint = 'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz';

/** Key-profile family; a closed set of lowerCamelCase names. */
export type KeyProfile =
  | 'krumhanslSchmuckler'
  | 'temperley'
  | 'shaath'
  | 'faraldoEdmt'
  | 'faraldoEdma'
  | 'faraldoEdmm'
  | 'bellmanBudge';

/**
 * Settings shared by {@link detectKey} and {@link detectKeyCandidates}, as the
 * request fields and as the positional `options`.
 */
export interface KeyDetectionOptions {
  /** Analysis window in samples at 22050 Hz, rescaled to the input rate. Default `2048`. */
  nFft?: number;
  /** Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default `512`. */
  hopLength?: number;
  /** Separate the harmonic part before the chroma. Default `false`. */
  useHpss?: boolean;
  /** Weight chroma frames by the RMS of the input. Default `false`. */
  loudnessWeighted?: boolean;
  /** Chroma high-pass corner in Hz, as in {@link analyze}. Absent = 80; 0 = no filter. */
  chromaHighPassHz?: number;
  /** Candidate modes: an explicit list, `'majorMinor'`, or `'all'`/`'modal'` for all seven. */
  modes?: KeyMode[] | 'majorMinor' | 'all' | 'modal';
  /**
   * Pins the key-profile family; a pinned profile outranks `genreHint`. Omitted, `genreHint`
   * chooses it, or the chroma does under the default `'auto'` hint.
   */
  profile?: KeyProfile;
  /** Picks the profile family when `profile` is absent. Unknown values are a `RangeError`. */
  genreHint?: GenreHint;
  /**
   * Tuning offset of the recording in cents, in `[-50, 50)`; 0 is concert A440.
   * Omitted or `'auto'` measures it from the audio. The value used is reported as
   * {@link KeyDetection.tuningCents}. {@link referenceHzToTuning} converts a reference
   * pitch to this unit. Pass 0 to read the chroma at A440.
   */
  tuningCents?: number | 'auto';
}

/** Result of {@link detectKey}: the key, and the tuning its chroma was built with. */
export interface KeyDetection extends Key {
  /** Tuning used, in cents: the given value, or the measured one under `'auto'`. */
  tuningCents: number;
}

/** A candidate key with the correlation that ranked it. */
export interface KeyCandidate {
  /** The candidate key. */
  key: Key;
  /** Correlation of the chroma with this key profile. */
  correlation: number;
}

/** A time signature with the support the analysis found for it. */
export interface TimeSignature {
  /** Beats per bar. */
  numerator: number;
  /** Note value of one beat: `4` is a quarter note. */
  denominator: number;
  /**
   * Support for this signature in `[0, 1]`.
   *
   * What it measures depends on which field the signature arrived in. On
   * {@link MeterEstimate.timeSignature} it is derived from the margin over the
   * runner-up; on a {@link MeterEstimate.candidates} entry it is that
   * candidate's share of the summed support. The two are not comparable, so
   * read the value from the field you meant rather than from whichever one is
   * to hand.
   */
  confidence: number;
}

/** A single detected beat in {@link AnalysisResult.beats}. */
export interface Beat {
  /** Beat time in seconds. */
  timeSec: number;
  /**
   * The onset envelope sampled at the single frame nearest this beat. It is a
   * raw, unnormalized value: its scale depends on the material, and it shifts
   * with beat-position jitter because nothing is averaged around the beat. For
   * accent scoring use {@link BeatObservations.onsetStrength}, the windowed
   * aggregate the library's own downbeat and meter passes score.
   */
  strength: number;
}

/**
 * Beat-level evidence the library's own downbeat and meter decisions score —
 * the input to those decisions, not their output. Each stream is parallel to
 * {@link AnalysisResult.beats}, one value per beat. An empty stream means the
 * analysis could not produce it, NOT that every beat scored zero.
 */
export interface BeatObservations {
  /**
   * Onset-envelope aggregate over a window around each beat. This is the
   * accent evidence the library's own meter pass scores;
   * {@link Beat.strength} is a single unwindowed frame of the same envelope.
   */
  onsetStrength: number[];
  /** Low-frequency energy at each beat. Empty when the analysis ran without audio. */
  lowFrequencyEnergy: number[];
  /** Chord-change evidence at each beat. Empty until chords are analyzed. */
  chordChange: number[];
}

/**
 * One chord in {@link AnalysisResult.chords}: a {@link Chord} that also carries its
 * Roman numeral in the analysis key.
 */
export interface AnalysisChord extends Chord {
  /**
   * Roman numeral of the chord relative to the analysis key
   * ({@link AnalysisResult.key}), e.g. `'V7'`, `'vi'`, `'bVII'`. The same
   * spelling `chordFunctionalAnalysis` reports as `romanNumeral` for that chord and key. Empty
   * for a chord that is N.C.
   */
  romanNumeral: string;
}

/** Aggregate dynamics summary in {@link AnalysisResult.dynamics}. */
export interface AnalysisDynamics {
  /** Dynamic range in dB. `+Infinity` when the low percentile window is silent; absent when not measured. */
  dynamicRangeDb?: number;
  /** Peak level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  peakDb: number;
  /** RMS level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  rmsDb: number;
  /** Peak-to-RMS ratio of the signal. Absent when not measured. */
  crestFactor?: number;
  /**
   * Loudness range in LU. Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
  /** True when the signal is flagged as compressed. */
  isCompressed: boolean;
}

/** Aggregate rhythm summary in {@link AnalysisResult.rhythm}. */
export interface AnalysisRhythm {
  /** Estimated time signature. */
  timeSignature: TimeSignature;
  /** Syncopation level in `[0, 1]`. */
  syncopation: number;
  /** Rhythmic feel of the track. */
  grooveType: GrooveType;
  /** Regularity of the beat intervals in `[0, 1]`; 0 with no beat interval. */
  patternRegularity: number;
  /** Tempo stability over time in `[0, 1]`; 0 below two beat intervals. */
  tempoStability: number;
}

/** Melody-contour summary in {@link AnalysisResult.melody}. */
export interface AnalysisMelody {
  /** Span between the lowest and highest voiced pitch, in octaves. */
  pitchRangeOctaves: number;
  /** Pitch stability in `[0, 1]`; 1 is very stable. */
  pitchStability: number;
  /** Mean fundamental frequency of the voiced frames, in Hz. */
  meanFrequencyHz: number;
  /** Vibrato rate in Hz; 0 when there is no vibrato. */
  vibratoRateHz: number;
  /** Pitch contour, one point per analysis frame. */
  pitches: MelodyPoint[];
}

/** Complete result of the unified {@link analyze} call. */
export interface AnalysisResult {
  /** Global tempo in beats per minute. */
  bpm: number;
  /**
   * Winning tempo cluster's vote share within the searched range: 60–200 BPM at 22050 Hz.
   * `analyzeBpm()` searches the same 60–200 BPM by default at the input rate, so the two can
   * still differ on the same input.
   */
  bpmConfidence: number;
  /** Tempo hypotheses, the chosen tempo first. */
  bpmCandidates: AnalysisBpmCandidate[];
  /** Detected musical key. */
  key: Key;
  /** Estimated time signature. */
  timeSignature: TimeSignature;
  /** Time-signature hypotheses ordered by descending support. */
  timeSignatureCandidates: TimeSignature[];
  /** Detected beats, in time order. */
  beats: Beat[];
  /** Per-beat evidence behind the downbeat and meter decisions. */
  beatObservations: BeatObservations;
  /**
   * Smoothed local tempo at each beat, in BPM, indexing in parallel with
   * `beats`.
   *
   * Empty unless `computeTempoCurve` was set, and empty regardless when fewer
   * than two beats were detected, since a tempo is a property of the interval
   * between two beats. The last entry repeats the tempo of the interval leading
   * into the final beat, which opens no interval of its own.
   *
   * Values are continuous, not points on a tempo grid: each is a weighted
   * average, in log tempo, of the beat intervals around it. A steady tempo
   * reads within about 1% at every beat, and a tempo that moves is followed a
   * few beats late.
   *
   * This is the local tempo rather than `bpm` resampled: on material whose
   * tempo moves it departs from `bpm`, and reading a single number out of it is
   * not how to get the global tempo.
   */
  beatLocalBpm: number[];
  /** The beats that fall on a measure start. Not the same length as `beats`. */
  downbeats: Beat[];
  /** Beat index the first measure starts on. */
  downbeatPhase: number;
  /** Detected chord progression. */
  chords: AnalysisChord[];
  /** Detected song-structure sections. */
  sections: Section[];
  /** Aggregate timbre summary. */
  timbre: TimbreFrame;
  /** Aggregate dynamics summary. */
  dynamics: AnalysisDynamics;
  /** Aggregate rhythm summary. */
  rhythm: AnalysisRhythm;
  /** Melody-contour summary. */
  melody: AnalysisMelody;
  /** Human-readable musical form label (e.g. `'AABA'`). */
  form: string;
  /** Tuning the chromagrams were built with, in cents: the given value, or the measured one under `tuningCents: 'auto'`. */
  tuningCents: number;
}

/** Result of {@link estimateMeter}. */
export interface MeterEstimate {
  /**
   * The best-supported meter over the scored beat series. Its `confidence` is
   * the margin over the runner-up — how separated the winner is — not the
   * share-of-support a {@link candidates} entry carries under the same name.
   */
  timeSignature: TimeSignature;
  /** Beat index the first measure starts on; always in `[0, timeSignature.numerator)`. */
  downbeatPhase: number;
  /**
   * Whether a search ran, as opposed to the fixed fallback being reported.
   *
   * `false` means the beat series was too short to score any candidate, and
   * every other field then carries that fallback rather than a measurement —
   * `timeSignature.confidence` included, which is 0, so an unchecked read
   * degrades toward "no idea" rather than toward a middling detection. Read
   * this before treating a short span's answer as a detection.
   */
  searched: boolean;
  /**
   * How the bar divides, in beats per accent group: `[3, 2, 2]` is the 7/8 an
   * aksak meter notates as 3+2+2, and `[2, 2]` an ordinary four. Always sums to
   * `timeSignature.numerator`.
   *
   * A single entry means no internal division was resolved — the numerator has
   * none to find, it was too wide to search, or the span was too short to
   * search at all.
   *
   * This is also what tells a compound bar from a simple one: per-beat accents
   * cannot say how a beat subdivides, so a six accented 3+3 keeps the requested
   * denominator and reports `[3, 3]` rather than being promoted to 6/8.
   */
  grouping: number[];
  /**
   * Support per *requested* candidate numerator, in the order the request
   * listed them.
   *
   * Standardized and signed: zero is the level a numerator reaches on beats
   * carrying no meter, so a negative entry means less support than noise would
   * produce. Only the ordering and the gaps between entries carry meaning —
   * one entry read on its own says nothing.
   *
   * Comparable only within one result. A score grows with the square root of
   * how many beats were scored, so the same meter over twice the beats scores
   * about 1.41 times as high; a segmentation search comparing spans of
   * different lengths has to normalize for length first.
   */
  candidateScores: number[];
  /**
   * The scored meters ordered by descending support, with `confidence` as each
   * one's share of the total score. This ordering is not the request order, so
   * `candidates[i]` is not the candidate `candidateScores[i]` scores — match
   * the two on `numerator` rather than by index.
   */
  candidates: TimeSignature[];
}

/** Options for {@link analyzeMelody}. All fields are optional. */
export interface MelodyOptions {
  /** Lowest f0 (Hz) the tracker will consider. Default 65 (≈ C2). */
  f0MinHz?: number;
  /** Highest f0 (Hz) the tracker will consider. Default 2093 (≈ C7). */
  f0MaxHz?: number;
  /** Analysis frame length in samples at 22050 Hz, rescaled to the input rate. Default 2048. */
  frameLength?: number;
  /** Hop length between frames in samples of the input buffer. Default 256. */
  hopLength?: number;
  /** Voicing confidence threshold in [0,1]; frames below are unvoiced. Default 0.1. */
  threshold?: number;
  /** Select the Viterbi-smoothed pYIN tracker instead of plain YIN. Default false. */
  usePyin?: boolean;
  /**
   * When pYIN is active, zero-pad by `frameLength / 2` so frame `i` is
   * centered at `i * hopLength` (matches `librosa.pyin(center=True)`). Ignored
   * for plain YIN. Default true.
   */
  center?: boolean;
}

/** Options for {@link analyzeBpm}. All fields are optional. */
export interface AnalyzeBpmOptions {
  /** Lowest BPM to consider. Default 60. */
  bpmMin?: number;
  /**
   * Highest BPM to consider. Default 200. A range other than the default is searched as given, and
   * the unconstrained estimate is preferred when it lies inside it.
   */
  bpmMax?: number;
  /** Tempo prior the tracker is biased toward. Default 120. */
  startBpm?: number;
  /** Analysis window length in samples at 22050 Hz, rescaled to the input sample rate. Default 2048. */
  nFft?: number;
  /** Hop length in samples of the input buffer. Default 512. */
  hopLength?: number;
  /** Number of tempo candidates to return. Default 5. */
  maxCandidates?: number;
}

/** Options for {@link analyzeRhythm}. All fields are optional. */
export interface AnalyzeRhythmOptions {
  /** Lowest BPM to consider. Default 60. */
  bpmMin?: number;
  /**
   * Highest BPM to consider. Default 200. A range other than the default is searched as given, and
   * the unconstrained estimate is preferred when it lies inside it.
   */
  bpmMax?: number;
  /** Tempo prior the tracker is biased toward. Default 120. */
  startBpm?: number;
  /** Analysis window length in samples at 22050 Hz, rescaled to the input sample rate. Default 2048. */
  nFft?: number;
  /** Hop length in samples of the input buffer. Default 512. */
  hopLength?: number;
}

/** Options for {@link analyzeDynamics}. All fields are optional. */
export interface AnalyzeDynamicsOptions {
  /** Analysis window length in seconds. Default 0.4. */
  windowSec?: number;
  /** Hop in samples of the caller's buffer, at the sample rate. Default 512. */
  hopLength?: number;
  /** Threshold (dB) above which compression is flagged. Default 6. */
  compressionThresholdDb?: number;
}

/** Options for {@link detectAcoustic}. All fields are optional. */
export interface AcousticOptions {
  /** Number of octave bands. Default 6. */
  nOctaveBands?: number;
  /** Number of third-octave sub-bands. Default 24. */
  nThirdOctaveSubbands?: number;
  /** Minimum decay range (dB) for a valid RT estimate. Default 30. */
  minDecayDb?: number;
  /** Noise-floor margin (dB) above the measured floor. Default 10. */
  noiseFloorMarginDb?: number;
}

/** Options for {@link analyzeTimbre}. All fields are optional. */
export interface AnalyzeTimbreOptions {
  /** Analysis window length in samples at 22050 Hz, rescaled to the input sample rate. Default 2048. */
  nFft?: number;
  /** Hop length in samples of the input buffer. Default 512. */
  hopLength?: number;
  /** Number of mel bands. Default 128. */
  nMels?: number;
  /** Number of MFCC coefficients. Default 13. */
  nMfcc?: number;
  /** Per-frame statistics window in seconds. Default 0.5. */
  windowSec?: number;
}

/** Options for {@link analyzeSections}. All fields are optional. */
export interface AnalyzeSectionsOptions {
  /** Analysis window in samples at 22050 Hz, rescaled to the analysis rate. Default 2048. */
  nFft?: number;
  /** Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default 512. */
  hopLength?: number;
  /** Minimum section length in seconds. Default 4. */
  minSectionSec?: number;
  /**
   * Section boundary threshold: the relative threshold a structural boundary must reach
   * to start a new section, as {@link BoundaryOptions.threshold}. A finite value of 0 or
   * more; a negative one is a RangeError. Omit it for the library default 0.22, which sits
   * below {@link detectBoundaries}' own default of 0.3 because sections also snap to
   * downbeats and keep a minimum length.
   */
  threshold?: number;
}

/** Options for {@link detectBoundaries}. All fields are optional. */
export interface BoundaryOptions {
  /** Analysis window in samples at 22050 Hz, rescaled to the analysis rate. Default 2048. */
  nFft?: number;
  /** Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default 512. */
  hopLength?: number;
  /** Checkerboard kernel size in frames. Default 64. */
  kernelSize?: number;
  /**
   * Relative threshold: a peak must reach this fraction of the largest raw novelty
   * event within `referenceWindowSec` of it, a lone dominant event (one no other event
   * in the window comes within 30% of) set aside. Default 0.3.
   */
  threshold?: number;
  /**
   * Absolute threshold, applied to the raw novelty response before it is scaled.
   * This is the floor that asks whether the features changed at all; at 0 a
   * stationary input segments anyway, because the novelty curve is scaled by its
   * own maximum. Default 0.005.
   */
  absoluteThreshold?: number;
  /** Number of MFCC coefficients. Default 13. */
  nMfcc?: number;
  /** Number of chroma bins. Default 12. */
  nChroma?: number;
  /** Minimum spacing between peaks in seconds. Default 2. */
  peakDistanceSec?: number;
  /**
   * One-sided span in seconds over which the local novelty maximum that
   * `threshold` is measured against is taken. Must be finite and non-negative;
   * 0 makes each frame its own reference, which disables the relative threshold.
   * A value at least the track length makes the reference whole-track, still with
   * a lone dominant event set aside. Default 60.
   */
  referenceWindowSec?: number;
  /** Include MFCC in the similarity features. Default true. */
  useMfcc?: boolean;
  /** Include chroma in the similarity features. Default true. */
  useChroma?: boolean;
}

/** One detected structural transition. */
export interface Boundary {
  /** Boundary time in seconds; the authoritative output. */
  timeSec: number;
  /**
   * Index into the ANALYSIS grid, which is not the source's STFT grid: input
   * above 22.05 kHz is resampled before any feature is computed, and a long
   * input is additionally mean-pooled (see `frameStride` on the result). Use
   * `timeSec` for any sample or second mapping.
   */
  frame: number;
  /** Novelty score at the boundary. */
  strength: number;
}

/** Result of {@link detectBoundaries}. */
export interface BoundaryResult {
  /** Detected structural transitions, in time order. */
  boundaries: Boundary[];
  /**
   * The novelty curve scaled by its own maximum, so values are in `[0, 1]` and
   * a peak of 1 means "the most novel frame here" rather than "a large change".
   * Multiply by `noveltyPeak` to recover the raw response `absoluteThreshold`
   * is compared against.
   */
  noveltyCurve: Float32Array;
  /** Maximum of the raw novelty response; 0 when nothing rose above the numerical floor. */
  noveltyPeak: number;
  /** The rate the ANALYSIS ran at, not the input's. */
  sampleRate: number;
  /** The hop length the analysis ran at. */
  hopLength: number;
  /** Raw frames averaged per analysis frame; 1 unless pooled. */
  frameStride: number;
}

/**
 * Song-structure section type.
 *
 * `'unknown'` means no labelling rule matched the segment; it always comes
 * with `confidence` 0.
 */
export type SectionType =
  | 'intro'
  | 'verse'
  | 'preChorus'
  | 'chorus'
  | 'bridge'
  | 'instrumental'
  | 'outro'
  | 'unknown';

/** One detected song-structure section. */
export interface Section {
  /** Section type. */
  type: SectionType;
  /** Human-readable section name (e.g. `'Chorus'`). */
  name: string;
  /** Section start time in seconds. */
  startSec: number;
  /** Section end time in seconds. */
  endSec: number;
  /** Median frame RMS in the section over the loudest section's, in `[0, 1]`. */
  energyLevel: number;
  /** Detection confidence in `[0, 1]`; 0 for `'unknown'` (no labelling rule matched). */
  confidence: number;
  /** Sections sharing a value repeat the same material, numbered 0, 1, ... by first appearance. */
  group: number;
}

/** The sections {@link analyzeSections} detected, with the form they spell. */
export interface SectionAnalysis {
  /** Detected sections, in time order. */
  sections: Section[];
  /** One letter per section in time order (e.g. `'IABABCBO'`); `''` when there are no sections. */
  form: string;
}

/** One frame of a melody contour. */
export interface MelodyPoint {
  /** Frame time in seconds. */
  timeSec: number;
  /** Estimated fundamental frequency in Hz; absent when the frame is unvoiced. */
  frequencyHz?: number;
  /** Voicing confidence in `[0, 1]`. */
  confidence: number;
}

/** Result of {@link analyzeMelody}. */
export interface MelodyResult {
  /** Melody contour, one point per analysis frame. */
  points: MelodyPoint[];
  /** Span between the lowest and highest voiced pitch, in octaves. */
  pitchRangeOctaves: number;
  /** Pitch stability in `[0, 1]`; 1 is very stable. */
  pitchStability: number;
  /** Mean fundamental frequency of the voiced frames, in Hz. */
  meanFrequencyHz: number;
  /** Vibrato rate in Hz; 0 when there is no vibrato. */
  vibratoRateHz: number;
}

/** A tempo hypothesis with its confidence. */
export interface BpmCandidate {
  /** Tempo in beats per minute. */
  bpm: number;
  /** Winning tempo cluster's vote share, in `[0, 1]`. */
  confidence: number;
}

/** One tempo hypothesis in {@link AnalysisResult.bpmCandidates}: a {@link BpmCandidate} with its relation to the chosen tempo. */
export interface AnalysisBpmCandidate extends BpmCandidate {
  /** Relation to the chosen tempo. */
  relation: BpmCandidateRelation;
}

/** Result of {@link analyzeBpm}. */
export interface BpmAnalysisResult {
  /** Chosen tempo in beats per minute. */
  bpm: number;
  /**
   * Winning tempo cluster's vote share within `bpmMin`–`bpmMax` (60–200 by default), searched at
   * the input rate. `analyze()` searches at 22050 Hz, so its `bpmConfidence` can differ.
   */
  confidence: number;
  /**
   * Chosen tempo first (`candidates[0].bpm === bpm`), then the other histogram
   * bins by descending vote share.
   */
  candidates: BpmCandidate[];
  /** Autocorrelation of the onset envelope over tempo lags. */
  autocorrelation: Float32Array;
  /** Tempogram of the onset envelope, flattened. */
  tempogram: Float32Array;
}

/**
 * Room acoustic parameters from a blind recording or a measured impulse
 * response (`isBlind` distinguishes the two).
 *
 * Only `rt60` (and `rt60Bands`) is estimated in both modes. `c50`/`c80`/`d50`
 * and `edt` require a known direct-sound arrival time, which only a measured
 * impulse response provides, so they and their band arrays are absent when
 * `isBlind` is true -- `edt` measures the 0 to -10 dB decay and the blind
 * estimator only fits the late decay `rt60` comes from.
 *
 * A value that was not measured is an absent key, never NaN. A band array that
 * was measured keeps its full length, indexed like `rt60Bands`, with NaN for
 * each band that could not be fitted; one with no band measured is absent.
 */
export interface AcousticResult {
  /** Broadband reverberation time (RT60) in seconds; absent when no decay could be fitted. */
  rt60?: number;
  /** Early decay time in seconds; absent when `isBlind` or not fitted. */
  edt?: number;
  /** Clarity C50 in dB; absent when `isBlind` or not measurable. */
  c50?: number;
  /** Clarity C80 in dB; absent when `isBlind` or not measurable. */
  c80?: number;
  /** Definition D50, the early-energy fraction in `[0, 1]`; absent when `isBlind` or not measurable. */
  d50?: number;
  /** RT60 per octave band, in seconds; NaN for a band not fitted, absent when no band was. */
  rt60Bands?: Float32Array;
  /** Early decay time per octave band, in seconds; absent when `isBlind` or no band was fitted. */
  edtBands?: Float32Array;
  /** Clarity C50 per octave band, in dB; absent when `isBlind` or no band was measured. */
  c50Bands?: Float32Array;
  /** Clarity C80 per octave band, in dB; absent when `isBlind` or no band was measured. */
  c80Bands?: Float32Array;
  /** Confidence of the estimate, in `[0, 1]`. */
  confidence: number;
  /** True when the result was estimated from a recording rather than a measured impulse response. */
  isBlind: boolean;
}

/** Names accepted for `materialPreset`. */
export type MaterialPresetName = 'none' | 'concrete' | 'wood' | 'curtain' | 'carpet' | 'glass';

/** Names accepted for the room-estimate `mode`. */
export type AcousticModeName = 'auto' | 'blind' | 'impulseResponse';

/** Shoebox geometry and placement: what {@link roomGeometryFromEstimate} returns, and what RIR synthesis and the room morph extend. */
export interface RoomGeometry {
  /** Room length in metres. Default `7`. */
  lengthM?: number;
  /** Room width in metres. Default `5`. */
  widthM?: number;
  /** Room height in metres. Default `3`. */
  heightM?: number;
  /**
   * Uniform wall absorption in [0, 1], used when no band absorption or preset
   * is given; a value outside is refused, and the model holds a wall at most
   * 0.999 absorbent. Defaults to 0.2. Higher absorption shortens RT60, so it changes both the
   * rendered content and the tail length rather than only the timbre.
   */
  absorption?: number;
  /**
   * Optional per-octave-band wall absorption (125/250/500/1k/2k/4k.. Hz). When
   * provided it overrides `absorption` unless `materialPreset` is set.
   *
   * The late tail's decay time runs continuously between octave centres, so
   * where absorption (a preset's included) changes steeply from one octave to
   * the next, the octave-band RT60 measured back from the result leans toward
   * the slower neighbour, as it does for a real room. The design value holds
   * at the octave centre.
   *
   * Every entry must be a finite number within [0, 1]. In a plain number array
   * a non-numeric entry is refused by index rather than read as 0, which is a
   * legal coefficient and would have been indistinguishable from a rigid wall
   * the caller asked for.
   */
  bandAbsorption?: Float32Array | number[];
  /**
   * Optional per-band wall scattering; missing bands default to 0. Independent
   * of `bandAbsorption` and `materialPreset` — it applies to whichever material
   * the absorption precedence selected, so a preset or a scalar-absorption room
   * can still be given rough walls. Entries follow `bandAbsorption`'s domain
   * and its per-index refusal.
   */
  bandScattering?: Float32Array | number[];
  /**
   * Wall-material preset, by name (`'none'`, `'concrete'`, `'wood'`,
   * `'curtain'`, `'carpet'`, `'glass'`). Any preset but `'none'` wins over
   * `bandAbsorption`/`absorption`. An unknown name throws a `RangeError` and a
   * number a `TypeError`, both listing the valid names.
   */
  materialPreset?: MaterialPresetName;
  /** Source position in metres, with finite `x`, `y` and `z`. Omitted: the default placement scaled to the room. */
  source?: RoomPlacement;
  /** Listener position in metres, with finite `x`, `y` and `z`. Omitted: the default placement scaled to the room. */
  listener?: RoomPlacement;
  /** Image-source reflection order; clamped to the safe maximum. */
  ismOrder?: number;
  /**
   * Seed of the diffuse tail, an integer. Omit for the default seed 1; 0 is a seed
   * like any other.
   */
  seed?: number;
  /** Maximum RIR length in seconds. Omit for the natural length (no cap); an explicit 0 is refused. */
  maxSec?: number;
  /**
   * Add the ISO 9613-1 atmospheric-absorption term to the late tail's per-band
   * RT60. On by default; it mainly shortens the high bands of a large room.
   * `false` gives the geometry-only RT60.
   */
  airAbsorptionEnabled?: boolean;
  /**
   * Air temperature in degrees Celsius. Omit for the ISO reference climate's
   * 20 degC; 0 is 0 degC.
   */
  airTemperatureC?: number;
  /**
   * Relative humidity in percent. Omit for the ISO reference climate's 50 %; 0 is
   * dry air. Both climate values are read only while `airAbsorptionEnabled` is set.
   */
  airHumidityPercent?: number;
}

/** Request for {@link synthesizeRir}. */
export interface SynthesizeRirRequest extends RoomGeometry {
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Use the Eyring statistical late-tail model (default true); false = Sabine. */
  preferEyring?: boolean;
  /** Early/late crossover in ms. Omit for auto (~sqrt(V) ms); an explicit 0 is refused. */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms. Omit for 5 ms; 0 is a hard switch. */
  crossfadeMs?: number;
}

/** One diagnostic reported by the RIR synthesizer. */
export interface RirDiagnostic {
  /** Stable machine-readable id, e.g. `acoustic.source_outside_room`. */
  code: string;
  /** Human-readable description of the condition. */
  message: string;
  /** How serious the condition is. */
  severity: 'info' | 'warning' | 'error';
}

/** A synthesized room impulse response and the diagnostics the synthesis reported. */
export interface RirResult {
  /** Impulse response samples. */
  rir: Float32Array;
  /** Sample rate of `rir` in Hz. */
  sampleRate: number;
  /**
   * Every diagnostic the synthesizer reported, in order. A refused synthesis
   * throws a coded `SonareError` instead, so every entry is a warning — a
   * `maxSec` clamp that cut the tail is one, and is otherwise
   * indistinguishable from an untruncated RIR.
   */
  diagnostics: RirDiagnostic[];
}

/** Settings of {@link estimateRoom}. */
export interface RoomEstimateOptions {
  /** Length/width ratio hint of the room. Omit for no hint; an explicit 0 is refused. */
  aspectHintLw?: number;
  /** Length/height ratio hint of the room. Omit for no hint; an explicit 0 is refused. */
  aspectHintLh?: number;
  /**
   * Mean-absorption prior anchoring the volume scale, in `[0.01, 0.99]`. Omit
   * for the library default, 0.15; a value outside that range, 0 included, is
   * refused with a `RangeError`. The reported volume scales with the cube of the prior.
   */
  referenceAbsorption?: number;
  /** Use the Eyring statistical late-tail model (default true); false = Sabine. */
  preferEyring?: boolean;
  /** Octave bands analysed. Omit for the library default; an explicit 0 is refused. */
  nOctaveBands?: number;
  /**
   * Analyzer routing, by name: `'auto'`, `'blind'`, `'impulseResponse'`. An
   * unknown name throws a `RangeError` and a number a `TypeError`, both listing
   * the valid names.
   */
  mode?: AcousticModeName;
  /** Analyzer decay-fit span in dB. Omit for the library default; an explicit 0 is refused. */
  minDecayDb?: number;
  /** Analyzer noise-floor margin in dB. Omit for the library default; an explicit 0 is refused. */
  noiseFloorMarginDb?: number;
}

/** Result of {@link estimateRoom}: the estimated room and its decay. */
export interface RoomEstimateResult {
  /**
   * Equivalent volume (m^3) and representative dimensions (m), under the names
   * {@link RoomGeometry} takes. Absent, with `confidence` 0, when the recording
   * has no measurable broadband decay (silence, or an RT60 the analyzer could
   * not fit).
   */
  volume?: number;
  /** Estimated room length in metres; absent when no decay is measurable. */
  lengthM?: number;
  /** Estimated room width in metres; absent when no decay is measurable. */
  widthM?: number;
  /** Estimated room height in metres; absent when no decay is measurable. */
  heightM?: number;
  /**
   * Direct-to-reverberant ratio in dB; `Infinity` when the direct sound has no energy outside its
   * window, absent when the recording is silent or empty.
   */
  drrDb?: number;
  /** Confidence of the estimate, in `[0, 1]`; 0 when nothing was measurable. */
  confidence: number;
  /**
   * Per-octave-band mean absorption. Absorption and `rt60Bands` are independent
   * estimates and either can fail on its own: a failed one is absent, and when
   * both are present they have the same length, the shorter padded with NaN.
   */
  bandAbsorption?: Float32Array;
  /** RT60 per octave band, in seconds; NaN for a band not fitted, absent when no band was. */
  rt60Bands?: Float32Array;
}

/** A position inside a room, in metres. */
export interface RoomPlacement {
  /** Position along the room length, in metres. */
  x: number;
  /** Position along the room width, in metres. */
  y: number;
  /** Height above the floor, in metres. */
  z: number;
}

/** Settings of {@link roomMorph} and {@link roomMorphStereo}. */
export interface RoomMorphOptions extends RoomGeometry {
  /** Wet level of the target room. */
  wet?: number;
  /** How far the source recording's own room tail is suppressed. */
  sourceTailSuppression?: number;
  /**
   * Use the Eyring statistical late-tail model for the target room (default
   * true); false = Sabine. Matches {@link SynthesizeRirRequest.preferEyring}.
   */
  preferEyring?: boolean;
  /** Early/late crossover in ms. Omit for auto (~sqrt(V) ms); an explicit 0 is refused. */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms. Omit for 5 ms; 0 is a hard switch. */
  crossfadeMs?: number;
}

/**
 * Morphed audio and what the target-room synthesis had to change to make it.
 *
 * Shaped like {@link RirResult} because the same synthesis runs underneath.
 * An unusable morph throws, so every entry in {@link RoomMorphResult.diagnostics}
 * is a warning.
 */
export interface RoomMorphResult {
  /** Morphed samples: input length plus the target room's reverb tail. */
  audio: Float32Array;
  /** Sample rate of `audio` in Hz. */
  sampleRate: number;
  /**
   * Every diagnostic the target-room synthesis reported, in order. Each says the
   * morph went through a room other than the one requested — an image-source
   * order reduced to the safe maximum (`acoustic.ism_order_clamped`), a tail cut
   * against `maxSec` (`acoustic.rir_length_clamped`), a `maxSec` shorter
   * than the direct sound's flight time and extended to fit it
   * (`acoustic.rir_length_floored`), a `maxSec` shorter than the longest
   * band's RT60, which cuts that band before it decays by 60 dB so its
   * reverberation time cannot be measured from the RIR
   * (`acoustic.rir_tail_truncated`), a request that produced no diffuse tail
   * (`acoustic.no_late_tail`) — and is otherwise invisible.
   *
   * These are the five codes the synthesis can emit here, so a `switch` over
   * them needs no fall-through case. `roomMorph` forwards `maxSec`
   * unchanged, which is why the floored one reaches a morph at all.
   */
  diagnostics: RirDiagnostic[];
}

/**
 * Stereo morph result: each channel convolved with the target room's impulse
 * response for its own receiver of a spaced pair.
 *
 * Shaped like {@link RoomMorphResult} with `left` / `right` in place of `audio`;
 * both channels have the input length plus the target room's reverb tail. An
 * unusable morph throws, so every entry in {@link RoomMorphStereoResult.diagnostics}
 * is a warning.
 */
export interface RoomMorphStereoResult {
  /** Left channel: input length plus the target room's reverb tail. */
  left: Float32Array;
  /** Right channel, the same length as `left`. */
  right: Float32Array;
  /** Sample rate of both channels in Hz. */
  sampleRate: number;
  /**
   * Every diagnostic the target-room synthesis reported, in order. Each says the
   * morph went through a room other than the one requested — an image-source
   * order reduced to the safe maximum (`acoustic.ism_order_clamped`), a tail cut
   * against `maxSec` (`acoustic.rir_length_clamped`), a `maxSec` shorter
   * than the direct sound's flight time and extended to fit it
   * (`acoustic.rir_length_floored`), a `maxSec` shorter than the longest
   * band's RT60, which cuts that band before it decays by 60 dB so its
   * reverberation time cannot be measured from the RIR
   * (`acoustic.rir_tail_truncated`), a request that produced no diffuse tail
   * (`acoustic.no_late_tail`) — and is otherwise invisible.
   *
   * These are the five codes the synthesis can emit here, so a `switch` over
   * them needs no fall-through case.
   */
  diagnostics: RirDiagnostic[];
}

/**
 * Row-major 2-D matrix: `data[r * cols + c]` holds row `r`, column `c`. Each
 * function returning one states what its rows and columns are.
 */
export interface Matrix2D {
  /** Row-major `[rows x cols]` data buffer. */
  data: Float32Array;
  /** Number of rows. */
  rows: number;
  /** Number of columns. */
  cols: number;
}

/** Result of {@link analyzeRhythm}. */
export interface RhythmResult {
  /**
   * The beat tracker's own tempo, refined from the local beat period. It is a
   * third figure rather than either tempo entry point's: measured against
   * synthesized click trains it differs from both at every sample rate, and
   * lands closer to the known tempo than either. Analysed at the sample rate
   * you pass: `nFft` is a window length in samples at 22050 Hz, rescaled to
   * that rate, and `hopLength` is in samples of your buffer.
   */
  bpm: number;
  /** Estimated time signature. */
  timeSignature: TimeSignature;
  /** Rhythmic feel of the track. */
  grooveType: GrooveType;
  /** Syncopation level in `[0, 1]`. */
  syncopation: number;
  /** Regularity of the beat intervals in `[0, 1]`; 0 with no beat interval. */
  patternRegularity: number;
  /** Tempo stability over time in `[0, 1]`; 0 below two beat intervals. */
  tempoStability: number;
  /** Intervals between consecutive beats, in seconds. */
  beatIntervals: Float32Array;
}

/**
 * Dynamics metrics returned by {@link analyzeDynamics}.
 *
 * The WASM package declares the same shape under the same name; the two are
 * pinned to one field list by `tests/conformance/shared_type_shapes.json`. The
 * processor envelope is `DynamicsProcessorResult`, which is a different shape.
 */
export interface DynamicsResult {
  /** Dynamic range in dB. `+Infinity` when the low percentile window is silent; absent when not measured. */
  dynamicRangeDb?: number;
  /** Peak level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  peakDb: number;
  /** RMS level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  rmsDb: number;
  /** Peak-to-RMS ratio of the signal. Absent when not measured. */
  crestFactor?: number;
  /**
   * Loudness range in LU. Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
  /** True when the signal is flagged as compressed. */
  isCompressed: boolean;
  /** Loudness curve timestamps in seconds, parallel to {@link loudnessRmsDb}. */
  loudnessTimesSec: Float32Array;
  /** Loudness curve RMS values (dB), parallel to {@link loudnessTimesSec}. `-Infinity` for an all-zero window. */
  loudnessRmsDb: Float32Array;
}

/** Timbre metrics for one analysis window. Entries are ordered by time in {@link TimbreResult.timbreOverTime}. */
export interface TimbreFrame {
  /** Spectral brightness in `[0, 1]`; high is bright. */
  brightness: number;
  /** Spectral warmth in `[0, 1]`; high is warm. */
  warmth: number;
  /** Spectral density in `[0, 1]`; high is rich. */
  density: number;
  /** Spectral roughness in `[0, 1]`; high is rough. */
  roughness: number;
  /** Harmonic complexity in `[0, 1]`; high is complex. */
  complexity: number;
}

/** Result of {@link analyzeTimbre}: overall timbre metrics and per-frame spectral curves. */
export interface TimbreResult {
  /** Spectral brightness in `[0, 1]`; high is bright. */
  brightness: number;
  /** Spectral warmth in `[0, 1]`; high is warm. */
  warmth: number;
  /** Spectral density in `[0, 1]`; high is rich. */
  density: number;
  /** Spectral roughness in `[0, 1]`; high is rough. */
  roughness: number;
  /** Harmonic complexity in `[0, 1]`; high is complex. */
  complexity: number;
  /** Spectral centroid per analysis frame, in Hz. */
  spectralCentroid: Float32Array;
  /** Spectral flatness per analysis frame, in `[0, 1]`. */
  spectralFlatness: Float32Array;
  /** Spectral roll-off frequency per analysis frame, in Hz. */
  spectralRolloff: Float32Array;
  /** Time-varying timbre metrics, one entry per analysis window. */
  timbreOverTime: TimbreFrame[];
}

/**
 * Chord quality tokens, indexed by the native ordinal. A chord crosses the
 * native boundary as the ordinal; the position in this tuple is its token.
 */
export const CHORD_QUALITIES = [
  'major',
  'minor',
  'diminished',
  'augmented',
  'dominant7',
  'major7',
  'minor7',
  'sus2',
  'sus4',
  'unknown',
  'add9',
  'minorAdd9',
  'dim7',
  'halfDim7',
  'major9',
  'dominant9',
  'sus2Add4',
  'major6',
  'minor6',
  'minorMajor7',
  'dominant7Sus4',
  'dominant11',
  'dominant13',
  'dominant7Flat9',
  'dominant7Sharp9',
] as const;

/** Chord quality token; see {@link CHORD_QUALITIES}. Numbers are refused. */
export type ChordQuality = (typeof CHORD_QUALITIES)[number];

/** One detected chord. */
export interface Chord {
  /** Root pitch class (C = 0). */
  root: PitchClass;
  /** Bass pitch class (C = 0). */
  bass: PitchClass;
  /** Canonical core spelling of `root`; stable across all language bindings. */
  rootName: string;
  /** Canonical core spelling of `bass`; stable across all language bindings. */
  bassName: string;
  /** Chord quality token; `'unknown'` for a chord the analyzer did not name. */
  quality: ChordQuality;
  /** Canonical core chord symbol (e.g. `'Cmaj7'`, `'Am/C'`, `'N.C.'`). */
  name: string;
  /** Chord start time in seconds. */
  startSec: number;
  /** Chord end time in seconds. */
  endSec: number;
  /** Chord length in seconds, `endSec - startSec`. */
  durationSec: number;
  /** Correlation of the chroma with the chosen chord template, in `[0, 1]`. */
  confidence: number;
}

/**
 * Harmonic function of a chord in a key: `'chromatic'` for a root outside the
 * key's scale and `'none'` for an unknown chord (`'N.C.'`).
 */
export type ChordFunction = 'tonic' | 'subdominant' | 'dominant' | 'chromatic' | 'none';

/** A timed chord with its Roman numeral and harmonic function in a key. */
export type FunctionalChord<T extends ChordFunctionsInput = Chord> = T & {
  /** Roman numeral relative to the key (`'I'`, `'V7'`, `'vi'`; `'N.C.'` for an unknown chord). */
  romanNumeral: string;
  /** Harmonic function of the chord in the key. */
  function: ChordFunction;
};

/** The fields {@link chordFunctions} reads from a chord; everything else is carried through. */
export interface ChordFunctionsInput {
  /** Root pitch class (C = 0). */
  root: PitchClass;
  /** Chord quality token. */
  quality: ChordQuality;
}

/** A key as `detectKey` reports it. */
export interface ChordFunctionsKey {
  /** Pitch class of the tonic (C = 0). */
  root: PitchClass;
  /** Mode of the key. */
  mode: KeyMode;
}

/** Result of {@link detectChords}. */
export interface ChordAnalysisResult {
  /** Detected chords, in time order. */
  chords: Chord[];
  /** Tuning the chroma was built with, in cents: the given value, or the measured one under `tuningCents: 'auto'`. */
  tuningCents: number;
}

/** Chroma front end for chord detection; `'cqt'` is the default. */
export type ChordChromaMethod = 'stft' | 'nnls' | 'cqt';

/**
 * Settings shared by {@link detectChords} and {@link chordFunctionalAnalysis}, as
 * the request fields and as the positional `options`.
 */
export interface ChordDetectionOptions {
  /** Shortest chord kept, in seconds. */
  minDurationSec?: number;
  /** Smoothing window, in seconds. */
  smoothingWindowSec?: number;
  /** Final-template correlation threshold in [0, 1]; below it emits `unknown` / N.C. */
  threshold?: number;
  /** Use only triad templates, no seventh chords. Default `true`. */
  useTriadsOnly?: boolean;
  /** STFT chroma window in samples at 22050 Hz, rescaled to the input rate so its duration is the same at every rate. */
  nFft?: number;
  /** Hop in samples of the caller's buffer, at the sample rate. Default `512`. */
  hopLength?: number;
  /** Detect chords on beat boundaries. Default `true`. */
  useBeatSync?: boolean;
  /** Smooth chord candidates with a Viterbi HMM. Default `true`. */
  useHmm?: boolean;
  /** Candidate beam width of the HMM smoothing. Default `24`. */
  hmmBeamWidth?: number;
  /** Bias the HMM transitions by the key given in `keyRoot` and `keyMode`. Default `false`. */
  useKeyContext?: boolean;
  /** Pitch class of the key, e.g. {@link detectKey}'s `root`. */
  keyRoot?: PitchClass;
  /** Mode of the key, e.g. {@link detectKey}'s `mode`. */
  keyMode?: KeyMode;
  /** Estimate the bass pitch class and emit slash chords. Default `false`. */
  detectInversions?: boolean;
  /** Chroma front end. Default `'cqt'`. */
  chromaMethod?: ChordChromaMethod;
  /**
   * Tuning offset of the recording in cents; must be in `[-50, 50)`. Omitted or
   * `'auto'` measures it from the audio; the value used is reported as
   * {@link ChordAnalysisResult.tuningCents}. {@link referenceHzToTuning} converts a
   * reference pitch to this unit. Pass 0 for concert A440.
   */
  tuningCents?: number | 'auto';
}

/** Harmonic and percussive components from harmonic-percussive separation. */
export interface HpssResult {
  /** Harmonic component, the same length as the input. */
  harmonic: Float32Array;
  /** Percussive component, the same length as the input. */
  percussive: Float32Array;
  /**
   * What neither component claimed (`input - harmonic - percussive`), the same
   * length as the input. Present only when the call set `withResidual: true`.
   */
  residual?: Float32Array;
  /** Sample rate of every returned signal in Hz. */
  sampleRate: number;
}
