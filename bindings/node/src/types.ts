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

export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain';

export interface RealtimeVoiceChangerPreset {
  schemaVersion: 1;
  id?: string;
  name?: string;
  description?: string;
  macros?: Record<string, number>;
  dsp?: Record<string, unknown>;
}

export type RealtimeVoiceChangerConfigInput = VoicePresetId | RealtimeVoiceChangerPreset;

export interface RealtimeVoiceChangerOptions {
  sampleRate: number;
  maxBlockSize?: number;
  channels?: 1 | 2;
  preset?: RealtimeVoiceChangerConfigInput;
}

/**
 * Flat (normalized) realtime-voice-changer configuration, mirroring the
 * `SonareRealtimeVoiceChangerConfig` POD returned by
 * `realtimeVoiceChangerPresetConfig`.
 */
export interface RealtimeVoiceChangerConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retuneSemitones: number;
  retuneMix: number;
  retuneGrainSize: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  eqHighpassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  gateAttackMs: number;
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  compressorAttackMs: number;
  compressorReleaseMs: number;
  compressorMakeupGainDb: number;
  deesserFrequencyHz: number;
  deesserThresholdDb: number;
  deesserRatio: number;
  deesserRangeDb: number;
  reverbMix: number;
  reverbTimeMs: number;
  reverbDamping: number;
  reverbSeed: number;
  limiterCeilingDb: number;
  limiterReleaseMs: number;
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
}

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
  key: Key;
  timeSignature: TimeSignature;
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

/** Progress callback for {@link analyzeWithProgress}. */
export type AnalysisProgressCallback = (progress: number, stage: string) => void;

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
   * When pYIN is active, reflect-pad by `frameLength / 2` so frame `i` is
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

/** Options for the high-level {@link mastering} one-shot. All fields are optional. */
export interface MasteringOptions {
  /** Integrated-loudness target in LUFS. Default -14. */
  targetLufs?: number;
  /** True-peak ceiling in dBTP. Default -1. */
  ceilingDb?: number;
  /** True-peak oversampling factor. Default 4. */
  truePeakOversample?: number;
}

/** Options for {@link noteStretch}. All fields are optional. */
export interface NoteStretchOptions {
  /** First sample of the note to stretch. Default 0. */
  onsetSample?: number;
  /** Last sample of the note to stretch. Default 0. */
  offsetSample?: number;
  /** Stretch ratio (1 = unchanged). Default 1. */
  stretchRatio?: number;
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

/** Constant-Q / Variable-Q transform magnitude result. */
export interface CqtResult {
  /** Number of frequency bins. */
  nBins: number;
  /** Number of time frames. */
  nFrames: number;
  /** Hop length in samples. */
  hopLength: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Row-major `[nBins x nFrames]` magnitude matrix. */
  magnitude: Float32Array;
  /** Center frequency (Hz) of each of the `nBins` bins. */
  frequencies: Float32Array;
}

/** Reconstructed linear-magnitude STFT from a mel spectrogram (`melToStft`). */
export interface InverseStftResult {
  /** Number of STFT frequency bins (`nFft / 2 + 1`). */
  nBins: number;
  /** Number of time frames. */
  nFrames: number;
  /** Row-major `[nBins x nFrames]` magnitude matrix. */
  power: Float32Array;
}

/** Reconstructed mel spectrogram from MFCCs (`mfccToMel`). */
export interface InverseMelResult {
  /** Number of mel bands. */
  nMels: number;
  /** Number of time frames. */
  nFrames: number;
  /** Row-major `[nMels x nFrames]` mel power matrix. */
  power: Float32Array;
}

/** Construction options for {@link StreamAnalyzer}. Mirrors `sonare::StreamConfig`. */
export interface StreamAnalyzerConfig {
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  nMels?: number;
  fmin?: number;
  fmax?: number;
  tuningRefHz?: number;
  /** Compute the per-frame magnitude spectrum. Defaults to false: no read path
   *  surfaces it, so enabling it only burns realtime CPU with no readable result. */
  computeMagnitude?: boolean;
  computeMel?: boolean;
  computeChroma?: boolean;
  computeOnset?: boolean;
  computeSpectral?: boolean;
  emitEveryNFrames?: number;
  magnitudeDownsample?: number;
  keyUpdateIntervalSec?: number;
  bpmUpdateIntervalSec?: number;
  /** Window type: 0 Hann, 1 Hamming, 2 Blackman, 3 Rectangular. */
  window?: number;
  /** Output format: 0 Float32, 1 Int16, 2 Uint8. */
  outputFormat?: number;
}

/** Structure-of-arrays frame buffer (`StreamAnalyzer.readFramesSoa`). */
export interface StreamFramesSoa {
  nFrames: number;
  nMels: number;
  timestamps: Float32Array;
  mel: Float32Array;
  chroma: Float32Array;
  onsetStrength: Float32Array;
  rmsEnergy: Float32Array;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  chordRoot: Int32Array;
  chordQuality: Int32Array;
  chordConfidence: Float32Array;
}

/**
 * Quantization ranges for the uint8/int16 bandwidth-reduction read paths
 * (`StreamAnalyzer.readFramesU8` / `readFramesI16`). Omitted fields fall back to
 * the library defaults shown below; widen any range whose source values exceed
 * the defaults, otherwise a louder/quieter stream saturates to the endpoints.
 */
export interface StreamQuantizeConfig {
  /** dB floor for mel quantization (default -80). */
  melDbMin?: number;
  /** dB ceiling for mel quantization (default 0). */
  melDbMax?: number;
  /** Max expected onset strength (default 50). */
  onsetMax?: number;
  /** Max expected RMS energy (default 1). */
  rmsMax?: number;
  /** Max expected spectral centroid in Hz (default 11025). */
  centroidMax?: number;
}

/** Quantized (uint8) frame buffer (`StreamAnalyzer.readFramesU8`). */
export interface StreamFramesU8 {
  nFrames: number;
  nMels: number;
  timestamps: Float32Array;
  mel: Uint8Array;
  chroma: Uint8Array;
  onsetStrength: Uint8Array;
  rmsEnergy: Uint8Array;
  spectralCentroid: Uint8Array;
  spectralFlatness: Uint8Array;
}

/** Quantized (int16) frame buffer (`StreamAnalyzer.readFramesI16`). */
export interface StreamFramesI16 {
  nFrames: number;
  nMels: number;
  timestamps: Float32Array;
  mel: Int16Array;
  chroma: Int16Array;
  onsetStrength: Int16Array;
  rmsEnergy: Int16Array;
  spectralCentroid: Int16Array;
  spectralFlatness: Int16Array;
}

/** A chord change in a progressive estimate (semitone root, quality ordinal). */
export interface StreamChordChange {
  root: number;
  quality: number;
  startTime: number;
  confidence: number;
}

/** A per-bar chord in a progressive estimate. */
export interface StreamBarChord {
  barIndex: number;
  root: number;
  quality: number;
  startTime: number;
  confidence: number;
}

/** A named chord-pattern match score. */
export interface StreamPatternScore {
  name: string;
  score: number;
}

/** Progressive (incremental) musical estimate from {@link StreamAnalyzer.stats}. */
export interface StreamProgressiveEstimate {
  bpm: number;
  bpmConfidence: number;
  bpmCandidateCount: number;
  key: number;
  keyMinor: boolean;
  keyConfidence: number;
  chordRoot: number;
  chordQuality: number;
  chordConfidence: number;
  chordStartTime: number;
  chordProgression: StreamChordChange[];
  barChordProgression: StreamBarChord[];
  currentBar: number;
  barDuration: number;
  votedPattern: StreamBarChord[];
  patternLength: number;
  detectedPatternName: string;
  detectedPatternScore: number;
  allPatternScores: StreamPatternScore[];
  accumulatedSeconds: number;
  usedFrames: number;
  updated: boolean;
}

/** Snapshot returned by {@link StreamAnalyzer.stats}. */
export interface StreamAnalyzerStats {
  totalFrames: number;
  totalSamples: number;
  durationSeconds: number;
  estimate: StreamProgressiveEstimate;
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

export interface LufsResult {
  integratedLufs: number;
  momentaryLufs: number;
  shortTermLufs: number;
  loudnessRange: number;
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

export type MasteringPreset =
  | 'pop'
  | 'edm'
  | 'acoustic'
  | 'hipHop'
  | 'aiMusic'
  | 'speech'
  | 'streaming'
  | 'youtube'
  | 'broadcast'
  | 'podcast'
  | 'audiobook'
  | 'cinema'
  | 'jpop'
  | 'ambient'
  | 'lofi'
  | 'classical'
  | 'drumAndBass'
  | 'techno'
  | 'metal'
  | 'trap'
  | 'rnb'
  | 'jazz'
  | 'kpop'
  | 'trance'
  | 'gameOst';

export interface StreamingPlatform {
  name: string;
  targetLufs: number;
  ceilingDb: number;
}

export type SoloProcessor =
  | 'dynamics.brickwallLimiter'
  | 'dynamics.compressor'
  | 'dynamics.deesser'
  | 'dynamics.expander'
  | 'dynamics.gate'
  | 'dynamics.limiter'
  | 'dynamics.parallelComp'
  | 'dynamics.sidechainRouter'
  | 'dynamics.duckingProcessor'
  | 'dynamics.transientShaper'
  | 'dynamics.upwardCompressor'
  | 'dynamics.upwardExpander'
  | 'dynamics.vocalRider'
  | 'eq.apiStyle'
  | 'eq.bandPass'
  | 'eq.cutFilter'
  | 'eq.dynamic'
  | 'eq.equalizer'
  | 'eq.graphic'
  | 'eq.linearPhase'
  | 'eq.midSide'
  | 'eq.minimumPhase'
  | 'eq.parametric'
  | 'eq.pultec'
  | 'eq.shelving'
  | 'eq.tilt'
  | 'final.bitDepth'
  | 'final.dither'
  | 'final.outputChain'
  | 'maximizer.adaptiveRelease'
  | 'maximizer.loudnessOptimize'
  | 'maximizer.maximizer'
  | 'maximizer.softKneeMax'
  | 'maximizer.truePeakLimiter'
  | 'multiband.compressor'
  | 'multiband.dynamicEq'
  | 'multiband.expander'
  | 'multiband.imager'
  | 'multiband.limiter'
  | 'multiband.saturation'
  | 'repair.declick'
  | 'repair.declip'
  | 'repair.decrackle'
  | 'repair.dehum'
  | 'repair.denoiseClassical'
  | 'repair.dereverbClassical'
  | 'repair.trimSilence'
  | 'saturation.bitcrusher'
  | 'saturation.exciter'
  | 'saturation.hardClipper'
  | 'saturation.multibandExciter'
  | 'saturation.ampSim'
  | 'saturation.softClipper'
  | 'saturation.tape'
  | 'saturation.transformer'
  | 'saturation.tube'
  | 'saturation.waveshaper'
  | 'spectral.airBand'
  | 'spectral.lowEndFocus'
  | 'spectral.presenceEnhancer'
  | 'spectral.spectralShaper'
  | 'stereo.autoPan'
  | 'stereo.haasEnhancer'
  | 'stereo.imager'
  | 'stereo.monoMaker'
  | 'stereo.phaseAlign'
  | 'stereo.stereoBalance';

export type PairProcessor =
  | 'match.applyMatchEq'
  | 'match.alignReferenceToSource'
  | 'match.abSwitch'
  | 'match.abCrossfade';

export type PairAnalysis =
  | 'match.referenceLoudness'
  | 'match.tonalBalance'
  | 'match.tonalBalanceLogBands'
  | 'match.matchEqCurve'
  | 'match.estimateReferenceDelaySamples';

export type StereoAnalysis = 'stereo.monoCompatCheck' | 'stereo.monoCompatCheckLogBands';

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

export interface MasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples?: number;
}

export interface MasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
}

/**
 * A nested processor / parameter sub-tree of a {@link MasteringChainConfig}.
 * Leaf values are numbers or booleans; nest deeper for processor parameters.
 */
export interface MasteringChainSection {
  [key: string]: number | boolean | MasteringChainSection;
}

/**
 * Nested mastering-chain configuration. Top-level keys are the processing
 * modules; nest processor and parameter names beneath them, e.g.
 *
 * ```ts
 * masteringChain(samples, sr, {
 *   dynamics: { compressor: { thresholdDb: -24 } },
 *   loudness: { targetLufs: -14 },
 * });
 * ```
 *
 * A boolean toggles a module/processor's `enabled` flag; setting any field
 * implicitly enables its module unless `enabled: false` is also given. Unknown
 * keys throw at apply time. (`stereo.*` modules apply on the stereo path only.)
 */
export interface MasteringChainConfig {
  repair?: MasteringChainSection;
  eq?: MasteringChainSection;
  dynamics?: MasteringChainSection;
  saturation?: MasteringChainSection;
  spectral?: MasteringChainSection;
  stereo?: MasteringChainSection;
  maximizer?: MasteringChainSection;
  loudness?: MasteringChainSection;
}

export interface MasteringChainResult {
  /** Latency-compensated offline output; no separate latency field is reported. */
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
}

export interface MasteringChainStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
}

export type PanMode = 'balance' | 'stereoPan' | 'stereo-pan' | 'dualPan' | 'dual-pan' | number;

export interface MixOptions {
  inputTrimDb?: number | number[];
  faderDb?: number | number[];
  pan?: number | number[];
  panMode?: PanMode | PanMode[];
  width?: number | number[];
  muted?: boolean | boolean[];
}

export interface MixMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  monoCompatWidth: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  truePeakDbL: number;
  truePeakDbR: number;
  maxTruePeakDb: number;
  seq: number;
}

export interface MixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: MixMeterSnapshot[];
}

/** Mixed stereo master returned by {@link Mixer.processStereo}. */
export interface MixerProcessResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
}

/**
 * Interpolation curve for scheduled automation events
 * (see {@link Mixer.scheduleInsertAutomation}).
 */
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 's-curve';

/**
 * Pan law applied by a strip's panner. Mapped to the C enum ints
 * `0=const3dB`, `1=const4.5dB`, `2=const6dB`, `3=linear0dB`.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/**
 * Meter tap point on a strip. Mapped to the C enum ints
 * `0=preFader`, `1=postFader`.
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/**
 * A reference to a strip in the {@link Mixer}: either a 0-based strip index or
 * the strip's string id.
 */
export type StripRef = number | string;

/** Single goniometer sample from {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
}

export type EngineTelemetryType = 0 | 1;

export type EngineTelemetryError = 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13;

export interface EngineTelemetry {
  type: EngineTelemetryType;
  error: EngineTelemetryError;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  graphLatencySamplesQ8: number;
  value: number;
}

/** Meter telemetry record drained from {@link RealtimeEngine.drainMeterTelemetry}. */
export interface EngineMeterTelemetry {
  /** Meter tap target id (e.g. master/bus identifier). */
  targetId: number;
  /** Render-frame timestamp of the snapshot. */
  renderFrame: number;
  /** Monotonic sequence number. */
  seq: number;
  /** Per-channel peak level in dB `[left, right]`. */
  peakDb: [number, number];
  /** Per-channel RMS level in dB `[left, right]`. */
  rmsDb: [number, number];
  /** Per-channel true-peak level in dB `[left, right]`. */
  truePeakDb: [number, number];
  /** Maximum true-peak across channels in dB. */
  maxTruePeakDb: number;
  /** Stereo correlation in `[-1, 1]`. */
  correlation: number;
  /** Mono-compatibility width metric. */
  monoCompatWidth: number;
  /** Momentary loudness (LUFS). */
  momentaryLufs: number;
  /** Short-term loudness (LUFS). */
  shortTermLufs: number;
  /** Integrated loudness (LUFS). */
  integratedLufs: number;
  /** Gain reduction in dB. */
  gainReductionDb: number;
  /** Number of records dropped before this snapshot. */
  droppedRecords: number;
}

/** Read-only engine transport snapshot from {@link RealtimeEngine.getTransportState}. */
export interface EngineTransportState {
  /** Whether the transport is currently playing. */
  isPlaying: boolean;
  /** Whether looping is enabled. */
  looping: boolean;
  /** Current render-frame counter. */
  renderFrame: number;
  /** Current timeline position in samples. */
  samplePosition: number;
  /** Current position in pulses-per-quarter-note. */
  ppq: number;
  /** Current tempo in beats per minute. */
  bpm: number;
  /** Loop start in PPQ. */
  loopStartPpq: number;
  /** Loop end in PPQ. */
  loopEndPpq: number;
  /** Engine sample rate in Hz. */
  sampleRate: number;
  /** PPQ of the current bar's downbeat (derived from the tempo map). */
  barStartPpq: number;
  /** Zero-based index of the current bar. */
  barCount: number;
  /** Time signature in effect at the current PPQ. */
  timeSignature: TimeSignature;
}

/**
 * Engine automation breakpoint curve as an integer code.
 * Canonical ordinals (matches mixer `AutomationCurve`):
 *   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve.
 */
export type EngineAutomationPointCurve = 0 | 1 | 2 | 3;

export interface EngineParameterInfo {
  id: number;
  name: string;
  unit: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  rtSafe: boolean;
  defaultCurve: EngineAutomationPointCurve;
}

export interface EngineAutomationPoint {
  ppq: number;
  value: number;
  curveToNext?: EngineAutomationPointCurve;
}

export interface EngineMarker {
  id: number;
  ppq: number;
  name?: string;
}

export interface EngineMetronomeConfig {
  enabled: boolean;
  beatGain?: number;
  accentGain?: number;
  clickSamples?: number;
  /** Click duration in seconds; used when clickSamples is 0 to derive the click length from the sample rate. */
  clickSeconds?: number;
}

export interface EngineClip {
  id: number;
  channels: Float32Array[];
  startPpq: number;
  lengthSamples?: number;
  clipOffsetSamples?: number;
  loop?: boolean;
  gain?: number;
  fadeInSamples?: number;
  fadeOutSamples?: number;
}

export interface EngineCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
}

export interface EngineBounceOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  targetSampleRate?: number;
  sourceSampleRate?: number;
  normalizeLufs?: boolean;
  targetLufs?: number;
  dither?: 0 | 1 | 2 | 3;
  ditherBits?: number;
  ditherSeed?: number;
}

export interface EngineBounceResult {
  interleaved: Float32Array;
  frames: number;
  numChannels: number;
  sampleRate: number;
  integratedLufs: number;
}

export interface EngineFreezeOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  clipId?: number;
  startPpq?: number;
  gain?: number;
}

export interface EngineFreezeResult {
  clipId: number;
  frames: number;
  numChannels: number;
}

export type EngineGraphNodeType = 0 | 1;

export type EngineGraphMix = 0 | 1;

export interface EngineGraphNode {
  id: string;
  type?: EngineGraphNodeType;
  gainDb?: number;
  numPorts?: number;
}

export interface EngineGraphConnection {
  sourceNode: string;
  sourcePort: number;
  destNode: string;
  destPort: number;
  mix?: EngineGraphMix;
}

export interface EngineGraphParameterBinding {
  paramId: number;
  nodeId: string;
}

export interface EngineGraphSpec {
  nodes: EngineGraphNode[];
  connections: EngineGraphConnection[];
  inputNode: string;
  outputNode: string;
  numChannels?: number;
  parameterBindings?: EngineGraphParameterBinding[];
}

export interface StftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

export interface StftDbResult {
  nBins: number;
  nFrames: number;
  db: Float32Array;
}

export interface MelSpectrogramResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

export interface MfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

export interface ChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

export interface PitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

/** Phase processing mode for the streaming equalizer. */
export type EqPhaseMode = 'zero' | 'natural' | 'linear';

/**
 * Single equalizer band passed to {@link StreamingEqualizer.setBand}.
 *
 * Only `type`/`frequencyHz`/`gainDb`/`enabled` are commonly needed; the
 * remaining fields fall back to processor defaults.
 */
export interface EqBandInput {
  type?:
    | 'Peak'
    | 'LowShelf'
    | 'HighShelf'
    | 'LowPass'
    | 'HighPass'
    | 'BandPass'
    | 'Notch'
    | 'TiltShelf'
    | 'FlatTilt';
  frequencyHz?: number;
  gainDb?: number;
  q?: number;
  enabled?: boolean;
  coeffMode?: 'Rbj' | 'Vicanek';
  slopeDbOct?: number;
  placement?: 'Stereo' | 'Left' | 'Right' | 'Mid' | 'Side';
  phase?: 'Inherit' | 'ZeroLatency' | 'NaturalPhase' | 'LinearPhase';
  soloed?: boolean;
  bypassed?: boolean;
  proportionalQ?: boolean;
  proportionalQStrength?: number;
  dynamic?: boolean;
  thresholdDb?: number;
  autoThreshold?: boolean;
  ratio?: number;
  rangeDb?: number;
  attackMs?: number;
  releaseMs?: number;
  lookaheadMs?: number;
  externalSidechain?: boolean;
  sidechainFreqHz?: number;
  sidechainQ?: number;
}

/** Realtime-safe spectrum snapshot from {@link StreamingEqualizer.spectrum}. */
export interface EqSpectrumSnapshot {
  preLeft: Float32Array;
  preRight: Float32Array;
  postLeft: Float32Array;
  postRight: Float32Array;
  bandGainDb: Float32Array;
  profileDb: Float32Array;
  lastAutoGainDb: number;
  seq: number;
}

// ============================================================================
// Headless arrangement / DAW project (sonare_project_* C ABI)
// ============================================================================

/**
 * Expected project ABI version, mirroring `SONARE_PROJECT_ABI_VERSION`
 * (src/sonare_c_project.h) and the other bindings' constant. A `projectAbiVersion()`
 * that differs means the native binary lays out the flat project PODs
 * differently than this binding expects (0 = arrangement support compiled out).
 */
export const EXPECTED_PROJECT_ABI_VERSION = 1;

/** Track kind for {@link ProjectTrackDesc} (mirrors SonareProjectTrackKind). */
export type ProjectTrackKind = 'audio' | 'midi' | 'aux' | 0 | 1 | 2;

/** Track-kind ordinals (mirror SonareProjectTrackKind). */
export const PROJECT_TRACK_AUDIO = 0;
export const PROJECT_TRACK_MIDI = 1;
export const PROJECT_TRACK_AUX = 2;

/** Descriptor for {@link Project.addTrack}. */
export interface ProjectTrackDesc {
  /** Track kind: `'audio'` | `'midi'` | `'aux'` or the ordinal 0/1/2. */
  kind?: ProjectTrackKind;
  /** Optional track name. */
  name?: string;
}

/** One first-class warp-map anchor. Sample positions must be finite and monotonic. */
export interface ProjectWarpAnchor {
  warpSample: number;
  sourceSample: number;
}

/** First-class project warp map referenced by clip `warpRefId`. */
export interface ProjectWarpMapDesc {
  id: number;
  name?: string;
  anchors: ProjectWarpAnchor[];
}

/**
 * Descriptor for {@link Project.addClip}. All musical positions are PPQ
 * (quarter notes); `lengthPpq` must be > 0.
 */
export interface ProjectClipDesc {
  /** Owning track id (from {@link Project.addTrack}). */
  trackId: number;
  /** `true` for a MIDI clip, `false`/omitted for an audio clip. */
  isMidi?: boolean;
  /** Clip start position in PPQ (default 0). */
  startPpq?: number;
  /** Clip length in PPQ (must be > 0). */
  lengthPpq: number;
  /** Offset into the source content in PPQ (default 0). */
  sourceOffsetPpq?: number;
  /** Linear clip gain (default 1). */
  gain?: number;
  /**
   * Decoded interleaved audio for an audio clip. When provided, the clip is
   * bound to a fresh renderable audio source; omit for a metadata-only source.
   */
  audio?: Float32Array;
  /** Channel count of `audio` (default 1). */
  audioChannels?: number;
  /** Sample rate of `audio` in Hz (default 0 = the project's). */
  audioSampleRate?: number;
  /** Optional host-local source reference for a metadata-only audio source. */
  sourceUri?: string;
}

/** `(trackId, clipId)` returned by {@link Project.addMidiClip}. */
export interface ProjectMidiClipResult {
  trackId: number;
  clipId: number;
}

/**
 * A flat MIDI event accepted by {@link Project.setMidiEvents}. `data0` / `data1`
 * are the first two UMP words of a channel-voice message (stored opaquely).
 * `data1` defaults to 0. SysEx imported from SMF is preserved by the native
 * project side store; it is not constructible through this flat event object.
 * The tuple form `[ppq, data0, data1]` is also accepted.
 */
export interface ProjectMidiEvent {
  ppq: number;
  data0: number;
  data1?: number;
}

/** Options for {@link Project.midiRouteEvents}. `null`/omitted filter fields mean any/no remap. */
export interface ProjectMidiRouteConfig {
  filterGroup?: number | null;
  filterChannel?: number | null;
  remapChannel?: number | null;
  thru?: boolean;
}

/** Result of {@link Project.midiRouteEvents}. */
export interface ProjectMidiRouteResult {
  events: ProjectMidiEvent[];
  overflowed: boolean;
  overflowCount: number;
}

export type ProjectMidiCcBindingKind = 0 | 1 | 2 | 3;

/** Options for {@link Project.midiCcLearn}. All fields are optional. */
export interface MidiCcLearnOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
  /** Minimum normalized CC movement required to learn a binding. Default `0`. */
  minMovement?: number;
}

/** Options for {@link RealtimeEngine.bindMidiCc}. All fields are optional. */
export interface MidiCcBindOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
}

/** MIDI CC <-> automation binding descriptor used by CC learn/conversion helpers. */
export interface ProjectMidiCcBinding {
  ccNumber: number;
  /** MIDI channel 0..15, or 255 for any channel. */
  channel: number;
  /** 0 = 7-bit CC, 1 = 14-bit CC, 2 = RPN, 3 = NRPN. */
  kind: ProjectMidiCcBindingKind;
  ccLsbNumber?: number;
  selectorMsb?: number;
  selectorLsb?: number;
  paramId: number;
  minValue: number;
  maxValue: number;
}

/** One compile diagnostic (mirrors SonareProjectDiagnostic). */
export interface ProjectDiagnostic {
  code: number;
  /** 0 = error, 1 = warning. */
  severity: number;
  /** Affected clip / track / source id (0 = n/a). */
  targetId: number;
  /** Human-readable message for this diagnostic. */
  message: string;
}

/** Result of {@link Project.compile}. */
export interface ProjectCompileResult {
  /** `true` when compilation produced a renderable timeline (no error diagnostics). */
  hasTimeline: boolean;
  /** Newline-joined human-readable diagnostic detail. */
  messages: string;
  diagnostics: ProjectDiagnostic[];
}

/** One tempo segment for {@link Project.setTempoSegments}. */
export interface ProjectTempoSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Tempo in BPM at the segment start. */
  bpm: number;
  /** Segment start in absolute samples. Default `0`. */
  startSample?: number;
  /** Tempo in BPM at the segment end for a ramp; `0` / omitted = constant tempo. */
  endBpm?: number;
}

/** One time-signature segment for {@link Project.setTimeSignatures}. */
export interface ProjectTimeSignatureSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Beats per bar. */
  numerator: number;
  /** Beat unit (e.g. `4` for quarter note). */
  denominator: number;
}

/** Options for {@link Project.bounce}. Zero / omitted fields take native defaults. */
export interface ProjectBounceOptions {
  /**
   * Render length in frames at the output sample rate. Omit / `<= 0` lets the
   * native side auto-derive the length from the arrangement (musical end plus
   * any instrument release tail). It does NOT produce an empty render.
   */
  totalFrames?: number;
  /** Render block size; <= 0 / omit => 128. */
  blockSize?: number;
  /** Output channel count; <= 0 / omit => 2. */
  numChannels?: number;
  /** Output sample rate; <= 0 / omit => the project's. */
  sampleRate?: number;
  /** Host-instrument PDC (samples) fed to the compiler. */
  instrumentLatencySamples?: number;
}

/** Oscillator waveform for the {@link BuiltinInstrumentConfig built-in synth}. */
export type SynthWaveform = 'sine' | 'saw' | 'sawtooth' | 'square' | 'triangle';

/**
 * Patch for the built-in minimal polyphonic oscillator synth used by
 * {@link Project.bounceWithBuiltinInstrument} /
 * {@link Project.bounceWithBuiltinInstruments}. Every numeric field uses
 * "0 / omit => sensible default", so an empty object is the default sine patch
 * and callers override only what they need.
 */
export interface BuiltinInstrumentConfig {
  /**
   * MIDI destination id this patch renders (the value set by
   * {@link Project.setTrackMidiDestination}). Defaults to `0`.
   */
  destinationId?: number;
  /** Oscillator waveform: a {@link SynthWaveform} name or numeric enum (0=sine). */
  waveform?: SynthWaveform | number;
  /** Master output gain (linear); 0 / omit => 0.2. */
  gain?: number;
  /** ADSR attack in ms; 0 / omit => 5. */
  attackMs?: number;
  /** ADSR decay in ms; 0 / omit => 60. */
  decayMs?: number;
  /** ADSR sustain level [0, 1]; 0 / omit => 0.7. */
  sustain?: number;
  /** ADSR release in ms; 0 / omit => 120. */
  releaseMs?: number;
  /** Max simultaneous voices; 0 / omit => 16, clamped to [1, 64]. */
  polyphony?: number;
}

/**
 * Cross-binding alias of {@link BuiltinInstrumentConfig}. The same built-in-synth
 * patch concept is named `BuiltinSynthConfig` in the Python binding; this alias
 * lets portable code use that shared name on the Node surface too.
 */
export type BuiltinSynthConfig = BuiltinInstrumentConfig;

/**
 * Patch for the GS-compatible SoundFont player used by
 * {@link Project.bounceWithSf2Instrument} /
 * {@link Project.bounceWithSf2Instruments} and
 * {@link RealtimeEngine.setSf2Instrument}. Every field uses
 * "0 / omit => sensible default".
 */
export interface Sf2InstrumentConfig {
  /**
   * MIDI destination id this player renders (the value set by
   * {@link Project.setTrackMidiDestination}). Defaults to `0`.
   */
  destinationId?: number;
  /** Master output gain (linear); 0 / omit => 0.5. */
  gain?: number;
  /** Max simultaneous voices; 0 / omit => 48, clamped to [1, 64]. */
  polyphony?: number;
}

export const SYNTH_ENGINE_MODES = [
  'default',
  'subtractive',
  'fm',
  'karplus-strong',
  'modal',
  'additive',
  'percussion',
  'piano',
] as const;
export const SYNTH_OSC_WAVEFORMS = [
  'default',
  'sine',
  'saw',
  'square',
  'triangle',
  'noise',
] as const;
export const SYNTH_FILTER_MODELS = [
  'default',
  'svf',
  'moog-ladder',
  'diode-ladder',
  'sallen-key',
] as const;
export const SYNTH_FILTER_OUTPUTS = ['default', 'lowpass', 'bandpass', 'highpass'] as const;
export const SYNTH_BODY_TYPES = ['default', 'none', 'guitar', 'violin', 'wood-tube'] as const;
export const SYNTH_MOD_SOURCES = [
  'none',
  'amp-env',
  'filter-env',
  'lfo1',
  'lfo2',
  'velocity',
  'key-track',
  'mod-wheel',
  'random',
] as const;
export const SYNTH_MOD_DESTINATIONS = [
  'none',
  'pitch-cents',
  'cutoff-cents',
  'amp-gain',
  'pan-units',
] as const;

export interface SynthEnumTables {
  engineModes: string[];
  waveforms: string[];
  filterModels: string[];
  filterOutputs: string[];
  bodyTypes: string[];
  modSources: string[];
  modDestinations: string[];
}

/** NativeSynth engine selector ({@link SynthPatch}; `'default'` keeps the base patch's). */
export type SynthEngineMode = (typeof SYNTH_ENGINE_MODES)[number];

/** NativeSynth oscillator waveform (`'default'` keeps the base patch's). */
export type SynthOscWaveform = (typeof SYNTH_OSC_WAVEFORMS)[number];

/** NativeSynth filter model — the character core (`'default'` keeps the base patch's). */
export type SynthFilterModel = (typeof SYNTH_FILTER_MODELS)[number];

/** NativeSynth filter output (SVF only; `'default'` keeps the base patch's). */
export type SynthFilterOutput = (typeof SYNTH_FILTER_OUTPUTS)[number];

/** NativeSynth body/formant resonance voicing (`'default'` keeps the base patch's). */
export type SynthBodyType = (typeof SYNTH_BODY_TYPES)[number];

/** {@link SynthPatch} mod-matrix source. */
export type SynthModSource = (typeof SYNTH_MOD_SOURCES)[number];

/** {@link SynthPatch} mod-matrix destination. */
export type SynthModDestination = (typeof SYNTH_MOD_DESTINATIONS)[number];

/** One {@link SynthPatch} mod-matrix routing (name or C ordinal per field). */
export interface SynthModRouting {
  source: SynthModSource | number;
  destination: SynthModDestination | number;
  /** Destination units at full source deflection. */
  depth: number;
}

/**
 * Versioned NativeSynth patch for {@link Project.bounceWithSynthInstrument} /
 * {@link Project.bounceWithSynthInstruments} and
 * {@link RealtimeEngine.setSynthInstrument}.
 *
 * The patch starts from a BASE — the named `preset` (see
 * {@link synthPresetNames}; a `"va:"` routing prefix is accepted) or, when
 * `preset` is omitted, the default subtractive patch. Every numeric field then
 * uses "0 / omit => keep the base value" (non-zero values override, clamped to
 * their audible ranges) and the enum fields reserve `'default'` as keep. The
 * frozen C ABI has no per-field presence bits, so explicit zero numeric
 * overrides (for example `ampSustain: 0`) cannot be represented; they keep the
 * base value. A non-empty `modRoutings` REPLACES the base mod matrix.
 *
 * Mode-specific deep parameters (FM operator stacks, modal mode tables,
 * drawbar registrations, kit pieces, piano strings) travel inside the named
 * presets; the patch exposes the wrapper sections every engine shares.
 */
export interface SynthPatch {
  /**
   * Optional binding convenience for JS realtime/offline helpers. It is not
   * part of the NativeSynth patch itself; Python uses explicit
   * `(destination_id, patch)` bindings instead. Defaults to `0`.
   */
  destinationId?: number;
  /** Base preset name (see {@link synthPresetNames}); omit for the init patch. */
  preset?: string;
  engineMode?: SynthEngineMode | number;
  // --- oscillator section (subtractive mode) ---
  waveform?: SynthOscWaveform | number;
  /** Detuned-stack width [1, 7]. */
  unison?: number;
  detuneCents?: number;
  /** Per-voice slow pitch drift depth (cents). */
  driftCents?: number;
  /** Pre-filter drive [0, 1]. */
  drive?: number;
  // --- filter section ---
  filterModel?: SynthFilterModel | number;
  filterOutput?: SynthFilterOutput | number;
  cutoffHz?: number;
  resonanceQ?: number;
  /** Cutoff keyboard tracking [0, 1]. */
  keyTrack?: number;
  envToCutoffCents?: number;
  velToCutoffCents?: number;
  // --- envelopes (ms / sustain in [0, 1]) ---
  ampAttackMs?: number;
  ampDecayMs?: number;
  /** 0 / omit keeps the base value; explicit zero sustain is not representable. */
  ampSustain?: number;
  ampReleaseMs?: number;
  filterAttackMs?: number;
  filterDecayMs?: number;
  /** 0 / omit keeps the base value; explicit zero sustain is not representable. */
  filterSustain?: number;
  filterReleaseMs?: number;
  // --- LFOs / glide ---
  lfoRateHz?: number;
  lfoToPitchCents?: number;
  lfo2RateHz?: number;
  glideMs?: number;
  // --- realism polish ---
  body?: SynthBodyType | number;
  /** Body resonance mix [0, 1]. */
  bodyMix?: number;
  /** Seeded per-voice pan scatter [0, 1]. */
  stereoSpread?: number;
  /** Mod matrix (at most 8 routings; REPLACES the base matrix when non-empty). */
  modRoutings?: SynthModRouting[];
  // --- voice pool / bus ---
  /** Master output gain (linear). */
  gain?: number;
  /** Max simultaneous voices [1, 64]. */
  polyphony?: number;
  /** Gain-neutral bus saturation [0, 1]. */
  busDrive?: number;
}

/** Source backend a resolved MIDI program renders through. */
export type SourceBackend = 'sf2' | 'synth';

/**
 * One {@link Project.soundFontManifest} entry: a (channel, bank, program)
 * combination the arrangement plays, with the backend it resolves to.
 */
export interface Sf2ProgramStatus {
  /** MIDI channel (0-15). */
  channel: number;
  /** Effective SF2 bank (drum channels report 128). */
  bank: number;
  /** Program number (0-127). */
  program: number;
  /** `'sf2'` when the loaded SoundFont covers the program, else `'synth'`. */
  backend: SourceBackend;
  /** Resolved SF2 preset name (GS fallback included); empty for `'synth'`. */
  presetName: string;
}

/** Clip fade-curve ordinals (mirror SonareProjectFadeCurve). */
export type ProjectFadeCurve = 0 | 1 | 2 | 3;
export const PROJECT_FADE_CURVE_LINEAR = 0;
export const PROJECT_FADE_CURVE_EQUAL_POWER = 1;
export const PROJECT_FADE_CURVE_EXPONENTIAL = 2;
export const PROJECT_FADE_CURVE_LOGARITHMIC = 3;

/** Clip loop-mode ordinals (mirror SonareProjectLoopMode). */
export type ProjectLoopMode = 0 | 1;
export const PROJECT_LOOP_MODE_OFF = 0;
export const PROJECT_LOOP_MODE_LOOP = 1;

/** One clip fade region for {@link Project.setClipFade}. */
export interface ProjectClipFade {
  /** Fade length in PPQ; finite and >= 0 (0 = no fade). */
  lengthPpq: number;
  /** Interpolation curve ({@link ProjectFadeCurve}); default linear (0). */
  curve?: ProjectFadeCurve;
}

/**
 * One automation breakpoint for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}. `curve` (alias `curveToNext`) is the
 * PPQ-domain curve to the next breakpoint (0 = Linear default, 1 = Exponential,
 * 2 = Hold, 3 = SCurve).
 */
export interface ProjectAutomationPoint {
  ppq: number;
  value: number;
  curve?: EngineAutomationPointCurve;
  curveToNext?: EngineAutomationPointCurve;
}

/**
 * Descriptor for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}.
 */
export interface ProjectAutomationLaneDesc {
  /** Host-defined target parameter id the lane drives. */
  targetParamId: number;
  /** Breakpoints (stored verbatim; need not be pre-sorted). */
  points: ProjectAutomationPoint[];
}

/** One key segment for {@link Project.annotateKeys}. */
export interface ProjectKeySegment {
  startPpq: number;
  endPpq: number;
  /** Tonic pitch class 0..11 (C=0) or 255 for unknown. Default 255. */
  tonicPc?: number;
  /**
   * KeyMode ordinal: 0 unknown, 1 major, 2 minor, 3 dorian, 4 phrygian,
   * 5 lydian, 6 mixolydian, 7 locrian. Default 0.
   */
  mode?: number;
}

/** One chord symbol for {@link Project.annotateChords}. */
export interface ProjectChordSymbol {
  startPpq: number;
  endPpq: number;
  /** Root pitch class 0..11 (C=0) or 255 for unknown. Default 255. */
  rootPc?: number;
  /**
   * ChordQuality ordinal: 0 unknown, 1 major, 2 minor, 3 diminished,
   * 4 augmented, 5 dominant, 6 half-diminished, 7 suspended. Default 0.
   */
  quality?: number;
  /** Extension semitone offsets (up to 8). */
  extensions?: number[];
  /** Slash-bass pitch class 0..11 or 255 for none. Default 255. */
  slashBassPc?: number;
  /** Optional roman-numeral label. */
  romanNumeral?: string;
  /** Marks a modulation boundary. Default false. */
  modulationBoundary?: boolean;
}

/** Descriptor for {@link Project.setAssistSidecar}. */
export interface ProjectAssistSidecarInput {
  /** Non-empty module id key. */
  moduleId: string;
  /** Module-defined schema version. Default 0. */
  schemaVersion?: number;
  /** Target track id (0 = project scope). Default 0. */
  targetTrackId?: number;
  /** Region start in PPQ. Default 0. */
  regionStartPpq?: number;
  /** Region end in PPQ. Default 0. */
  regionEndPpq?: number;
  /** Opaque module-owned payload bytes. */
  payload?: Uint8Array;
}

/** A stored assist sidecar returned by {@link Project.getAssistSidecar}. */
export interface ProjectAssistSidecar {
  moduleId: string;
  schemaVersion: number;
  targetTrackId: number;
  regionStartPpq: number;
  regionEndPpq: number;
  payload: Uint8Array;
}
