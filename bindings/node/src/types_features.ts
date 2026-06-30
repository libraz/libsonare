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

/** Reconstructed STFT power from a mel spectrogram (`melToStft`). */
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
  /**
   * Row-major `[nFrames * nMels]` mel spectrogram in LINEAR power (not dB) — the
   * raw per-frame mel energies. The quantized read paths (`readFramesU8` /
   * `readFramesI16`) convert to dB before packing, so their `mel` is dB-scaled.
   */
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
  /** Row-major `[nFrames * nMels]` mel in dB, quantized over `[melDbMin, melDbMax]`. */
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
  /** Row-major `[nFrames * nMels]` mel in dB, quantized over `[melDbMin, melDbMax]`. */
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
