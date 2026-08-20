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
} as const;

export type ChordQuality = (typeof ChordQuality)[keyof typeof ChordQuality];

/**
 * Section type.
 *
 * `PreChorus` is never produced by `analyze()`: it has no detection branch, so
 * filtering sections on it always yields an empty result. Every other value is
 * reachable. `Unknown` means the analyzer did not identify the segment -- no
 * boundary was detected, or the segment matched none of the positive branches
 * -- and comes with `confidence` 0.
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
  time: number;
  strength: number;
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
  chords: Chord[];
  sections: Section[];
  timbre: Timbre;
  dynamics: Dynamics;
  rhythm: RhythmFeatures;
  melody: MelodyContour;
  form: string;
}
