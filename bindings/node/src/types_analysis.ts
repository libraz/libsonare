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
   * strength {@link estimateMeter} is meant to be fed;
   * {@link AnalysisBeat.strength} is a single unwindowed frame of the same
   * envelope.
   */
  onsetStrength: number[];
  /** Low-frequency energy at each beat. Empty when the analysis ran without audio. */
  lowFrequencyEnergy: number[];
  /** Chord-change evidence at each beat. Empty until chords are analyzed. */
  chordChange: number[];
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
   * This is the local tempo rather than `bpm` resampled: on material whose
   * tempo moves it departs from `bpm`, and reading a single number out of it is
   * not how to get the global tempo.
   */
  beatLocalBpm: number[];
  /** Indices into `beats` that fall on a measure start. Not the same length as `beats`. */
  downbeatIndices: number[];
  /** Beat index the first measure starts on. */
  downbeatPhase: number;
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

/** Result of {@link estimateMeter}. */
export interface MeterEstimate {
  /** The best-supported meter over the scored beat series. */
  timeSignature: TimeSignature;
  /** Beat index the first measure starts on; always in `[0, timeSignature.numerator)`. */
  downbeatPhase: number;
  /**
   * How the bar divides, in beats per accent group: `[3, 2, 2]` is the 7/8 an
   * aksak meter notates as 3+2+2, and `[2, 2]` an ordinary four. Always sums to
   * `timeSignature.numerator`.
   *
   * A single entry means no internal division was resolved — the numerator has
   * none to find, it was too wide to search, or the span was too short to
   * search at all.
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

/**
 * Progress callback for {@link analyzeWithProgress}. Its return value is
 * ignored; use the request object's `cancel` callback to cancel cooperatively.
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

/**
 * Song-structure section type ordinal (mirrors the C `SonareSectionType`).
 *
 * Ordinal 2 (PreChorus) is never produced by the analyzer: it has no detection
 * branch, so filtering sections on it always yields an empty result. Every
 * other ordinal is reachable. 7 (Unknown) means the analyzer did not identify
 * the segment -- no boundary was detected, or the segment matched none of the
 * positive branches -- and comes with `confidence` 0.
 */
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
  /**
   * Uniform wall absorption, clamped to [0, 0.999] (the back-compat scalar).
   * Defaults to 0.2. Higher absorption shortens RT60, so it changes both the
   * rendered content and the tail length rather than only the timbre.
   */
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
  /**
   * Add the ISO 9613-1 atmospheric-absorption term to the late tail's per-band
   * RT60. Off by default so the RIR is unchanged; it mainly shortens the high
   * bands of a large room.
   */
  airAbsorptionEnabled?: boolean;
  /**
   * Air temperature in degrees Celsius; 0 (or omitted) selects the ISO
   * reference climate's 20 degC. A literal 0 degC is therefore not
   * distinguishable from unset -- use 0.01 for a freezing room, which absorbs
   * identically.
   */
  airTemperatureC?: number;
  /**
   * Relative humidity in percent; 0 (or omitted) selects the ISO reference
   * climate's 50 %. Both climate values are read only while
   * `airAbsorptionEnabled` is set.
   */
  airHumidityPercent?: number;
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

/** One diagnostic reported by the RIR synthesizer. */
export interface RirDiagnostic {
  /** Stable machine-readable id, e.g. `acoustic.source_outside_room`. */
  code: string;
  message: string;
  severity: 'info' | 'warning' | 'error';
}

export interface RirResult {
  rir: Float32Array;
  sampleRate: number;
  hasError: boolean;
  /**
   * First error diagnostic as `code: message`, empty when `hasError` is false.
   * Matches the string the C ABI leaves in `sonare_last_error_message()`.
   */
  errorMessage: string;
  /**
   * Every diagnostic the synthesizer reported, in order. Warnings appear here on
   * successful calls too — a `maxSeconds` clamp that cut the tail is a warning,
   * not an error, and is otherwise indistinguishable from an untruncated RIR.
   */
  diagnostics: RirDiagnostic[];
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
  /**
   * Per-octave-band mean absorption and RT60 (s). The two are independent
   * estimates and either can fail to converge on its own; both arrays are always
   * the same length, with the failed side NaN-filled rather than truncating both
   * to the shorter (possibly empty) one.
   */
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
  /** Canonical core chord symbol (e.g. `'Cmaj7'`, `'Am/C'`, `'N.C.'`). */
  name: string;
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
