import type { ValidateOptions } from './validation';

/**
 * Pitch class enum (C=0, C#=1, ..., B=11)
 */
export const PitchClass = {
  C: 0,
  Cs: 1,
  D: 2,
  Ds: 3,
  E: 4,
  F: 5,
  Fs: 6,
  G: 7,
  Gs: 8,
  A: 9,
  As: 10,
  B: 11,
} as const;

export type PitchClass = (typeof PitchClass)[keyof typeof PitchClass];

/**
 * Musical mode
 */
export const Mode = {
  Major: 0,
  Minor: 1,
  Dorian: 2,
  Phrygian: 3,
  Lydian: 4,
  Mixolydian: 5,
  Locrian: 6,
} as const;

export type Mode = (typeof Mode)[keyof typeof Mode];

export type TempogramMode = 'autocorrelation' | 'auto' | 'ac' | 'cosine' | 0 | 1;

export const KeyProfile = {
  KrumhanslSchmuckler: 0,
  Temperley: 1,
  Shaath: 2,
  FaraldoEDMT: 3,
  FaraldoEDMA: 4,
  FaraldoEDMM: 5,
  BellmanBudge: 6,
} as const;

export type KeyProfile = (typeof KeyProfile)[keyof typeof KeyProfile];

export type KeyProfileName =
  | 'ks'
  | 'krumhansl'
  | 'temperley'
  | 'shaath'
  | 'keyfinder'
  | 'faraldo-edmt'
  | 'edmt'
  | 'faraldo-edma'
  | 'edma'
  | 'faraldo-edmm'
  | 'edmm'
  | 'bellman-budge'
  | 'bellman';

/**
 * Chord quality
 */
export const ChordQuality = {
  Major: 0,
  Minor: 1,
  Diminished: 2,
  Augmented: 3,
  Dominant7: 4,
  Major7: 5,
  Minor7: 6,
  Sus2: 7,
  Sus4: 8,
  Unknown: 9,
  Add9: 10,
  MinorAdd9: 11,
  Dim7: 12,
  HalfDim7: 13,
  Major9: 14,
  Dominant9: 15,
  Sus2Add4: 16,
  Major6: 17,
  Minor6: 18,
  MinorMajor7: 19,
  Dominant7Sus4: 20,
  Dominant11: 21,
  Dominant13: 22,
  Dominant7Flat9: 23,
  Dominant7Sharp9: 24,
} as const;

export type ChordQuality = (typeof ChordQuality)[keyof typeof ChordQuality];

/**
 * Section type.
 *
 * `PreChorus` is never produced by `analyze()`: it has no detection branch, so
 * filtering sections on it always yields an empty result. Every other value is
 * reachable. `Unknown` means the analyzer did not name the segment: no
 * boundary was detected, the segment matched none of the positive branches, or
 * the evidence for a musical function was too weak to assert one. The first
 * case comes with `confidence` 0; the last keeps the sub-threshold score, so a
 * caller can see how close the segment came to a label.
 */
export const SectionType = {
  Intro: 0,
  Verse: 1,
  PreChorus: 2,
  Chorus: 3,
  Bridge: 4,
  Instrumental: 5,
  Outro: 6,
  Unknown: 7,
} as const;

export type SectionType = (typeof SectionType)[keyof typeof SectionType];

/**
 * Detected musical key
 */
export interface Key {
  root: PitchClass;
  mode: Mode;
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
  name: string;
  shortName: string;
}

export interface KeyDetectionOptions extends ValidateOptions {
  nFft?: number;
  hopLength?: number;
  useHpss?: boolean;
  loudnessWeighted?: boolean;
  highPassHz?: number;
  modes?:
    | Mode[]
    | ('major' | 'minor' | 'dorian' | 'phrygian' | 'lydian' | 'mixolydian' | 'locrian')[]
    | 'major-minor'
    | 'all'
    | 'modal';
  profile?: KeyProfile | KeyProfileName;
  genreHint?: 'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz' | string;
}

export interface KeyCandidate {
  key: Key;
  correlation: number;
}

export interface ChordDetectionOptions extends ValidateOptions {
  minDuration?: number;
  smoothingWindow?: number;
  /** Final-template correlation threshold in [0, 1]; below it emits Unknown / N.C. */
  threshold?: number;
  useTriadsOnly?: boolean;
  nFft?: number;
  hopLength?: number;
  useBeatSync?: boolean;
  useHmm?: boolean;
  hmmBeamWidth?: number;
  useKeyContext?: boolean;
  keyRoot?: PitchClass;
  keyMode?: Mode;
  detectInversions?: boolean;
  chromaMethod?: 'stft' | 'nnls';
}

/** Options for `analyzeBpm`. All fields are optional. */
export interface AnalyzeBpmOptions extends ValidateOptions {
  /** Lowest BPM to consider. Default 30. */
  bpmMin?: number;
  /** Highest BPM to consider. Default 300. */
  bpmMax?: number;
  /** Tempo prior the tracker is biased toward. Default 120. */
  startBpm?: number;
  /** FFT size for the onset envelope. Default 2048. */
  nFft?: number;
  /** Hop length for the onset envelope. Default 512. */
  hopLength?: number;
  /** Number of tempo candidates to return. Default 5. */
  maxCandidates?: number;
}

/** Options for `analyzeRhythm`. All fields are optional. */
export interface AnalyzeRhythmOptions extends ValidateOptions {
  /** Lowest BPM to consider. Default 60. */
  bpmMin?: number;
  /** Highest BPM to consider. Default 200. */
  bpmMax?: number;
  /** Tempo prior the tracker is biased toward. Default 120. */
  startBpm?: number;
  /** FFT size for the onset envelope. Default 2048. */
  nFft?: number;
  /** Hop length for the onset envelope. Default 512. */
  hopLength?: number;
}

/** Options for `analyzeDynamics`. All fields are optional. */
export interface AnalyzeDynamicsOptions extends ValidateOptions {
  /** Loudness-curve window length in seconds. Default 0.4. */
  windowSec?: number;
  /** Hop length for the loudness curve. Default 512. */
  hopLength?: number;
  /** Crest-factor (dB) below which the signal is flagged as compressed. Default 6. */
  compressionThreshold?: number;
}

/** Options for `analyzeTimbre`. All fields are optional. */
export interface AnalyzeTimbreOptions extends ValidateOptions {
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop length. Default 512. */
  hopLength?: number;
  /** Number of Mel bands. Default 128. */
  nMels?: number;
  /** Number of MFCCs. Default 13. */
  nMfcc?: number;
  /** Per-window analysis length in seconds. Default 0.5. */
  windowSec?: number;
}

/** Options for `analyzeSections`. All fields are optional. */
export interface AnalyzeSectionsOptions {
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop length. Default 512. */
  hopLength?: number;
  /** Minimum section duration in seconds. Default 4. */
  minSectionSec?: number;
}

/**
 * Detected beat
 */
export interface Beat {
  /** Beat position in seconds. */
  time: number;
  /**
   * Onset-envelope value sampled at the beat's frame.
   *
   * @remarks
   * A single raw frame of the onset envelope, not a normalized or relative
   * salience: it is unbounded, its scale depends on the material, and it moves
   * with any jitter in the beat position. For accent scoring use
   * {@link BeatObservations.onsetStrength}, which is the windowed aggregate the
   * library's own downbeat pass scores.
   */
  strength: number;
}

/**
 * Beat-level evidence behind the downbeat and meter decisions.
 *
 * @remarks
 * These are inputs to the library's decision, not outputs of it, exposed so a
 * caller running its own meter work scores the same evidence instead of
 * reconstructing a weaker approximation from the frame-level onset envelope.
 * Each non-empty stream holds one value per entry of
 * {@link AnalysisResult.beats} and indexes in parallel with it. An empty stream
 * means the analysis could not produce it, which is not the same as every beat
 * having scored zero.
 */
export interface BeatObservations {
  /**
   * Beat-local onset-strength window.
   *
   * @remarks
   * Unlike {@link Beat.strength} this is aggregated over a window around the
   * beat rather than sampled at a single frame, so it is the stream to score
   * accents with.
   */
  onsetStrength: number[];
  /**
   * Beat-local low-frequency energy — the accent evidence a log-spectral
   * difference discards. Empty when the analysis ran without audio.
   */
  lowFrequencyEnergy: number[];
  /** Per-beat chord-change evidence. Empty until chords are analyzed. */
  chordChange: number[];
}

/**
 * Detected chord
 */
export interface Chord {
  root: PitchClass;
  bass: PitchClass;
  /** Canonical core spelling; stable across all language bindings. */
  rootName: string;
  /** Canonical core spelling; stable across all language bindings. */
  bassName: string;
  quality: ChordQuality;
  start: number;
  end: number;
  /** Derived from `end - start`; the core carries only the two endpoints. */
  duration: number;
  confidence: number;
  name: string;
}

export interface ChordAnalysisResult {
  chords: Chord[];
}

/**
 * Detected section
 */
export interface Section {
  type: SectionType;
  start: number;
  end: number;
  energyLevel: number;
  confidence: number;
  name: string;
}

/**
 * A single melody contour point (mirrors the C `SonareMelodyPoint`).
 */
export interface MelodyPoint {
  /** Frame time in seconds. */
  time: number;
  /** Estimated fundamental frequency in Hz (0 when unvoiced). */
  frequency: number;
  /** Voicing confidence in `[0, 1]`. */
  confidence: number;
}

/**
 * Melody analysis result (mirrors the C `SonareMelodyResult`).
 */
export interface MelodyResult {
  points: MelodyPoint[];
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
}

/**
 * Timbre characteristics
 */
export interface Timbre {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

/**
 * Dynamics characteristics
 */
export interface Dynamics {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  loudnessRangeDb: number;
  crestFactor: number;
  isCompressed: boolean;
}

/**
 * Time signature
 */
export interface TimeSignature {
  numerator: number;
  denominator: number;
  confidence: number;
}

/**
 * Meter estimated over a caller-supplied beat series.
 */
export interface MeterEstimate {
  /** Selected time signature. */
  timeSignature: TimeSignature;
  /** Beat index the first measure starts on, in `[0, timeSignature.numerator)`. */
  downbeatPhase: number;
  /**
   * Whether a search ran, as opposed to the fixed fallback being reported.
   *
   * @remarks
   * `false` means the beat series was too short to score any candidate, and
   * every other field then carries that fallback rather than a measurement —
   * including `timeSignature.confidence`, which is the fallback's own fixed
   * value. Read this before treating a short span's answer as a detection.
   */
  searched: boolean;
  /**
   * How the bar divides, in beats per accent group.
   *
   * @remarks
   * `[3, 2, 2]` is the 7/8 an aksak meter notates as 3+2+2, and `[2, 2]` an
   * ordinary four. Always sums to `timeSignature.numerator`.
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
   * Multi-comb score per requested candidate numerator.
   *
   * @remarks
   * Parallel to the `candidateNumerators` that were requested, in the order
   * they were given. This does NOT index alike with {@link candidates}, which
   * is ordered by descending support.
   *
   * Standardized and signed: zero is the level a numerator reaches on beats
   * carrying no meter, so a negative entry means less support than noise would
   * produce. Only the ordering and the gaps between entries carry meaning.
   *
   * Comparable only within one result. A score grows with the square root of
   * how many beats were scored, so the same meter over twice the beats scores
   * about 1.41 times as high; a segmentation search comparing spans of
   * different lengths has to normalize for length first.
   */
  candidateScores: number[];
  /**
   * Candidate signatures ordered by descending support.
   *
   * @remarks
   * A ranking, so entry `k` is the k-th best hypothesis — not the k-th
   * requested numerator. Use {@link candidateScores} to read the score of a
   * specific requested numerator.
   */
  candidates: TimeSignature[];
}

/** Existing tempo-estimator hypothesis retained by unified analysis. */
export interface BpmHypothesis {
  value: number;
  confidence: number;
  relation: 'primary' | 'half' | 'double' | 'other';
}

/**
 * Rhythm features
 */
export interface RhythmFeatures {
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
  timeSignature: TimeSignature;
}

/**
 * Melody contour from the unified analysis (pitch trajectory + summary stats).
 */
export interface MelodyContour {
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
  pitches: MelodyPoint[];
}

/**
 * Complete analysis result
 */
export interface AnalysisResult {
  bpm: number;
  bpmConfidence: number;
  bpmCandidates: BpmHypothesis[];
  key: Key;
  timeSignature: TimeSignature;
  timeSignatureCandidates: TimeSignature[];
  beatTimes: Float32Array;
  beats: Beat[];
  /**
   * Indices into {@link AnalysisResult.beats} that fall on a measure start.
   *
   * @remarks
   * Not the same length as `beats` — it holds one entry per detected downbeat,
   * and each entry indexes `beats`, so `beats[downbeatIndices[k]]` is the k-th
   * downbeat. Testing a beat for downbeat status is a membership check on this
   * list rather than a time comparison against a separate downbeat series.
   */
  downbeatIndices: number[];
  /**
   * Beat index the first measure starts on, in `[0, timeSignature.numerator)`.
   *
   * @remarks
   * The meter estimator's phase, so `downbeatIndices` normally begins at this
   * value. It is not re-derived when downbeats are refined from chord and
   * low-frequency-energy evidence, so the two can disagree when the refinement
   * moves the first measure start.
   */
  downbeatPhase: number;
  /**
   * Beat-level evidence behind the downbeat and meter decisions, parallel to
   * {@link AnalysisResult.beats}.
   */
  beatObservations: BeatObservations;
  /**
   * Smoothed local tempo at each beat, in BPM, parallel to
   * {@link AnalysisResult.beats}.
   *
   * @remarks
   * Empty unless `computeTempoCurve` was set, and empty regardless when fewer
   * than two beats were detected, since a tempo is a property of the interval
   * between two beats. The last entry repeats the tempo of the interval leading
   * into the final beat, which opens no interval of its own.
   *
   * This is the local tempo rather than {@link AnalysisResult.bpm} resampled:
   * on material whose tempo moves it departs from `bpm`, and reading a single
   * number out of it is not how to get the global tempo.
   */
  beatLocalBpm: number[];
  chords: Chord[];
  sections: Section[];
  timbre: Timbre;
  dynamics: Dynamics;
  rhythm: RhythmFeatures;
  melody: MelodyContour;
  form: string;
}
