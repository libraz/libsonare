export interface Key {
  root: string;
  mode: string;
  confidence: number;
  name: string;
  shortName: string;
}

export type KeyMode =
  | 'major'
  | 'minor'
  | 'dorian'
  | 'phrygian'
  | 'lydian'
  | 'mixolydian'
  | 'locrian';

export type TempogramMode = 'autocorrelation' | 'auto' | 'ac' | 'cosine' | 0 | 1;

export type KeyProfile =
  | 'ks'
  | 'krumhansl'
  | 'krumhansl-schmuckler'
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
  | 'bellman'
  | 0
  | 1
  | 2
  | 3
  | 4
  | 5
  | 6;

export interface KeyDetectionOptions {
  nFft?: number;
  hopLength?: number;
  useHpss?: boolean;
  loudnessWeighted?: boolean;
  highPassHz?: number;
  modes?: KeyMode[] | 'major-minor' | 'all' | 'modal';
  profile?: KeyProfile;
  genreHint?: 'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz' | string;
}

export interface KeyCandidate {
  key: Key;
  correlation: number;
}

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

/** A single detected beat in {@link AnalysisResult.beats}. */
export interface AnalysisBeat {
  /** Beat time in seconds. */
  time: number;
  /** Relative beat strength / salience. */
  strength: number;
}

/**
 * One chord in {@link AnalysisResult.chords}. Mirrors the camelCase JSON emitted
 * by the full-analysis pipeline. Unlike the standalone {@link Chord} (whose
 * `root`/`bass`/`quality` are string labels), the full-analysis chord encodes
 * `root`/`bass` as pitch-class ordinals (0..11, C=0) and `quality` as a
 * ChordQuality ordinal, and carries a human-readable `name` (e.g. `'Cmaj7'`).
 */
export interface AnalysisChord {
  /** Root pitch class ordinal (0..11, C=0). */
  root: number;
  /** Bass pitch class ordinal (0..11, C=0). */
  bass: number;
  /** ChordQuality ordinal. */
  quality: number;
  start: number;
  end: number;
  confidence: number;
  /** Human-readable chord name (e.g. `'Cmaj7'`). */
  name: string;
}

/** One song-structure section in {@link AnalysisResult.sections}. */
export interface AnalysisSection {
  /** Section type ordinal (0=Intro, 1=Verse, ... 7=Unknown). */
  type: SectionTypeOrdinal;
  start: number;
  end: number;
  /** Relative energy level in `[0, 1]`. */
  energyLevel: number;
  /** Detection confidence in `[0, 1]`. */
  confidence: number;
  /** Human-readable section name (e.g. `'Chorus'`). */
  name: string;
}

/** Aggregate timbre summary in {@link AnalysisResult.timbre}. */
export interface AnalysisTimbre {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

/** Aggregate dynamics summary in {@link AnalysisResult.dynamics}. */
export interface AnalysisDynamics {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  crestFactor: number;
  loudnessRangeDb: number;
  isCompressed: boolean;
}

/** Aggregate rhythm summary in {@link AnalysisResult.rhythm}. */
export interface AnalysisRhythm {
  timeSignature: TimeSignature;
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
}

/** One melody pitch sample in {@link AnalysisMelody.pitches}. */
export interface AnalysisPitchPoint {
  /** Frame time in seconds. */
  time: number;
  /** Estimated fundamental frequency in Hz (0 when unvoiced). */
  frequency: number;
  /** Voicing confidence in `[0, 1]`. */
  confidence: number;
}

/** Melody-contour summary in {@link AnalysisResult.melody}. */
export interface AnalysisMelody {
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
  pitches: AnalysisPitchPoint[];
}

export interface AnalysisResult {
  bpm: number;
  bpmConfidence: number;
  bpmCandidates: BpmHypothesis[];
  key: Key;
  timeSignature: TimeSignature;
  timeSignatureCandidates: TimeSignature[];
  /**
   * Beat times as a `Float32Array` for backward compatibility. Derived from
   * `beats[].time`.
   */
  beatTimes: Float32Array;
  beats: AnalysisBeat[];
  /** Detected chord progression. */
  chords: AnalysisChord[];
  /** Detected song-structure sections. */
  sections: AnalysisSection[];
  /** Aggregate timbre summary. */
  timbre: AnalysisTimbre;
  /** Aggregate dynamics summary. */
  dynamics: AnalysisDynamics;
  /** Aggregate rhythm summary. */
  rhythm: AnalysisRhythm;
  /** Melody-contour summary. */
  melody: AnalysisMelody;
  /** Human-readable musical form label (e.g. `'AABA'`). */
  form: string;
}

/**
 * Progress callback for {@link analyzeWithProgress}. Return exactly `false` to
 * request cooperative cancellation; returning `undefined` continues normally.
 */
export type AnalysisProgressCallback = import('./types.js').ProgressCallback;

/** Options for {@link analyzeMelody}. All fields are optional. */
export interface MelodyOptions {
  /** Lowest f0 (Hz) the tracker will consider. Default 65 (≈ C2). */
  fmin?: number;
  /** Highest f0 (Hz) the tracker will consider. Default 2093 (≈ C7). */
  fmax?: number;
  /** Analysis frame length in samples. Default 2048. */
  frameLength?: number;
  /** Hop length between frames in samples. Default 256. */
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

/** Options for {@link analyzeRhythm}. All fields are optional. */
export interface AnalyzeRhythmOptions {
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

/** Options for {@link analyzeDynamics}. All fields are optional. */
export interface AnalyzeDynamicsOptions {
  /** Analysis window length in seconds. Default 0.4. */
  windowSec?: number;
  /** Hop length in samples. Default 512. */
  hopLength?: number;
  /** Threshold (dB) above which compression is flagged. Default 6. */
  compressionThreshold?: number;
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
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop length in samples. Default 512. */
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
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop length in samples. Default 512. */
  hopLength?: number;
  /** Minimum section length in seconds. Default 4. */
  minSectionSec?: number;
}

/** Song-structure section type ordinal (mirrors the C `SonareSectionType`). */
export type SectionTypeOrdinal = 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7;

export interface Section {
  /** Section type ordinal (0=Intro, 1=Verse, ... 7=Unknown). */
  type: SectionTypeOrdinal;
  /** Human-readable section name (e.g. `'Chorus'`). */
  name: string;
  /** Section start time in seconds. */
  start: number;
  /** Section end time in seconds. */
  end: number;
  /** Relative energy level in `[0, 1]`. */
  energyLevel: number;
  /** Detection confidence in `[0, 1]`. */
  confidence: number;
}

export interface MelodyPoint {
  /** Frame time in seconds. */
  time: number;
  /** Estimated fundamental frequency in Hz (0 when unvoiced). */
  frequency: number;
  /** Voicing confidence in `[0, 1]`. */
  confidence: number;
}

export interface MelodyResult {
  points: MelodyPoint[];
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
}

export interface BpmCandidate {
  bpm: number;
  confidence: number;
}

export interface BpmAnalysisResult {
  bpm: number;
  confidence: number;
  candidates: BpmCandidate[];
  autocorrelation: Float32Array;
  tempogram: Float32Array;
}

export interface AcousticResult {
  rt60: number;
  edt: number;
  c50: number;
  c80: number;
  d50: number;
  rt60Bands: Float32Array;
  edtBands: Float32Array;
  c50Bands: Float32Array;
  c80Bands: Float32Array;
  confidence: number;
  isBlind: boolean;
}

/** Shoebox geometry + placement shared by RIR synthesis and the room morph. */
export interface RoomGeometryOptions {
  lengthM?: number;
  widthM?: number;
  heightM?: number;
  /** Uniform wall absorption, clamped to [0, 0.999] (the back-compat scalar). */
  absorption?: number;
  /**
   * Optional per-octave-band wall absorption (125/250/500/1k/2k/4k.. Hz). When
   * provided it overrides `absorption` unless `materialPreset` is set.
   */
  bandAbsorption?: Float32Array | number[];
  /** Optional per-band wall scattering; missing bands default to 0. */
  bandScattering?: Float32Array | number[];
  /**
   * Named wall-material preset (0 none; 1 concrete, 2 wood, 3 curtain,
   * 4 carpet, 5 glass). A non-zero preset wins over `bandAbsorption`/`absorption`.
   */
  materialPreset?: number;
  sourceX?: number;
  sourceY?: number;
  sourceZ?: number;
  listenerX?: number;
  listenerY?: number;
  listenerZ?: number;
  ismOrder?: number;
  seed?: number;
  maxSeconds?: number;
}

export interface RirSynthOptions extends RoomGeometryOptions {
  sampleRate?: number;
  /** Use the Eyring statistical late-tail model (default true); false = Sabine. */
  preferEyring?: boolean;
  /** Early/late crossover in ms (0 = auto, ~sqrt(V) ms). */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms (0 = default). */
  crossfadeMs?: number;
}

export interface RirResult {
  rir: Float32Array;
  sampleRate: number;
  hasError: boolean;
}

export interface RoomEstimateOptions {
  aspectHintLw?: number;
  aspectHintLh?: number;
  referenceAbsorption?: number;
  preferEyring?: boolean;
  nOctaveBands?: number;
  /** Analyzer routing: 0 = auto, 1 = blind, 2 = impulse-response. */
  mode?: number;
  /** Analyzer decay-fit span in dB (0 = library default). */
  minDecayDb?: number;
  /** Analyzer noise-floor margin in dB (0 = library default). */
  noiseFloorMarginDb?: number;
}

export interface RoomEstimateResult {
  volume: number;
  length: number;
  width: number;
  height: number;
  drrDb: number;
  confidence: number;
  absorptionBands: Float32Array;
  rt60Bands: Float32Array;
}

export interface RoomMorphOptions extends RoomGeometryOptions {
  wet?: number;
  sourceTailSuppression?: number;
  /**
   * Use the Eyring statistical late-tail model for the target room (default
   * true); false = Sabine. Matches {@link RirSynthOptions.preferEyring}.
   */
  preferEyring?: boolean;
  /** Early/late crossover in ms (0 = auto, ~sqrt(V) ms). */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms (0 = default). */
  crossfadeMs?: number;
}

/** Row-major dense matrix returned by feature/decompose helpers. */
export interface Matrix2D {
  /** Number of rows. */
  rows: number;
  /** Number of columns. */
  cols: number;
  /** Row-major `[rows x cols]` data buffer. */
  data: Float32Array;
}

/** Non-negative matrix factorisation result (`decompose`). */
export interface DecomposeResult {
  /** Component matrix `[nFeatures x nComponents]`. */
  w: Matrix2D;
  /** Activation matrix `[nComponents x nFrames]`. */
  h: Matrix2D;
}

/** Harmonic/percussive/residual separation result (`hpssWithResidual`). */
export interface HpssResidualResult {
  /** Harmonic component signal. */
  harmonic: Float32Array;
  /** Percussive component signal. */
  percussive: Float32Array;
  /** Residual (`original - harmonic - percussive`) signal. */
  residual: Float32Array;
  /** Shared sample rate of all three signals. */
  sampleRate: number;
}

export interface RhythmResult {
  bpm: number;
  timeSignature: TimeSignature;
  grooveType: 'straight' | 'shuffle' | 'swing';
  syncopation: number;
  patternRegularity: number;
  tempoStability: number;
  beatIntervals: Float32Array;
}

export interface DynamicsResult {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  crestFactor: number;
  loudnessRangeDb: number;
  isCompressed: boolean;
  loudnessTimes: Float32Array;
  loudnessRmsDb: Float32Array;
}

/** Timbre metrics for one analysis window. Entries are ordered by time in {@link TimbreResult.timbreOverTime}. */
export interface TimbreFrame {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

export interface TimbreResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  spectralRolloff: Float32Array;
  /** Time-varying timbre metrics, one entry per analysis window. */
  timbreOverTime: TimbreFrame[];
}

export interface Chord {
  root: string;
  bass: string;
  /** Canonical core spelling; stable across all language bindings. */
  rootName: string;
  /** Canonical core spelling; stable across all language bindings. */
  bassName: string;
  quality:
    | 'major'
    | 'minor'
    | 'diminished'
    | 'augmented'
    | 'dominant7'
    | 'major7'
    | 'minor7'
    | 'sus2'
    | 'sus4'
    | 'add9'
    | 'minorAdd9'
    | 'dim7'
    | 'halfDim7'
    | 'major9'
    | 'dominant9'
    | 'sus2Add4'
    | 'unknown';
  start: number;
  end: number;
  duration: number;
  confidence: number;
}

export interface ChordAnalysisResult {
  chords: Chord[];
}

export type ChordChromaMethod = 'stft' | 'nnls';

/**
 * Options-object form of the chord-detection parameters, mirroring the WASM
 * `ChordDetectionOptions`. Accepted by the standalone {@link detectChords} /
 * {@link chordFunctionalAnalysis} functions as an alternative to the positional
 * argument form.
 */
export interface ChordDetectionOptions {
  minDuration?: number;
  smoothingWindow?: number;
  /** Final-template correlation threshold in [0, 1]; below it emits `unknown` / N.C. */
  threshold?: number;
  useTriadsOnly?: boolean;
  nFft?: number;
  hopLength?: number;
  useBeatSync?: boolean;
  useHmm?: boolean;
  hmmBeamWidth?: number;
  useKeyContext?: boolean;
  keyRoot?: number;
  keyMode?: number;
  detectInversions?: boolean;
  chromaMethod?: ChordChromaMethod;
}

export interface HpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}
