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

/**
 * Construction options for {@link StreamAnalyzer}. Mirrors `sonare::StreamConfig`.
 *
 * Every count-valued field (`sampleRate`, `nFft`, `hopLength`, `nMels`,
 * `emitEveryNFrames`, `magnitudeDownsample`, `maxPendingFrames`,
 * `maxProgressionEntries`, `window`, `outputFormat`) must be an integer the
 * native type can hold; a fractional or out-of-range value is rejected rather
 * than truncated.
 */
export interface StreamAnalyzerConfig {
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  nMels?: number;
  fmin?: number;
  fmax?: number;
  /** A4 tuning reference in Hz. Defaults to 440; must be within 220..880, the
   *  same range `setTuningRefHz` accepts live. */
  tuningRefHz?: number;
  /** Compute the per-frame magnitude spectrum. Must be false (the default): no
   *  read path on this surface returns magnitude frames, so enabling it would
   *  only burn realtime CPU with no readable result. `true` is rejected, the
   *  same way the C ABI, WASM and Python reject it. */
  computeMagnitude?: boolean;
  computeMel?: boolean;
  computeChroma?: boolean;
  computeOnset?: boolean;
  computeSpectral?: boolean;
  emitEveryNFrames?: number;
  magnitudeDownsample?: number;
  /** Maximum unread frames; overflow drops the newly produced frame. */
  maxPendingFrames?: number;
  /** Maximum retained chord and bar progression entries; overflow drops oldest. */
  maxProgressionEntries?: number;
  keyUpdateIntervalSec?: number;
  bpmUpdateIntervalSec?: number;
  /** Window type: 0 Hann, 1 Hamming, 2 Blackman, 3 Rectangular. */
  window?: number;
  /** @deprecated Must be 0 (Float32). Use readFramesU8/readFramesI16 explicitly. */
  outputFormat?: number;
}

/** Structure-of-arrays frame buffer (`StreamAnalyzer.readFramesSoa`). */
export interface StreamFramesSoa {
  nFrames: number;
  nMels: number;
  /** Chroma stride: 12 when enabled, otherwise 0. */
  nChroma: number;
  /** MEL=1, CHROMA=2, ONSET=4, SPECTRAL=8. */
  featureFlags: number;
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
  nChroma: number;
  /** MEL=1, CHROMA=2, ONSET=4, SPECTRAL=8. */
  featureFlags: number;
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
  nChroma: number;
  /** MEL=1, CHROMA=2, ONSET=4, SPECTRAL=8. */
  featureFlags: number;
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
  /**
   * Bar number, not the index of this entry in the array: bars with no
   * confident chord are not recorded and the oldest entries are dropped at the
   * history cap. Group bars by pattern position with this, never with the array
   * index. In `votedPattern` it is the pattern position instead.
   */
  barIndex: number;
  root: number;
  quality: number;
  /**
   * Start of the bar, on the same timeline as `StreamFrame.timestamp`
   * (including a `sampleOffset` anchor). Consecutive bars are `barDuration`
   * apart rather than snapped to the analysis frame grid. Unused in
   * `votedPattern`.
   */
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
  /**
   * Tempo candidates the most recent BPM estimate chose from; 0 until an
   * estimate has run. Same quantity as the batch analysis result's field of the
   * same name.
   */
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
  /**
   * True when the key or BPM was re-estimated since the previous stats
   * snapshot. One change sets it on exactly one snapshot however the caller
   * chunks its input, and a call that produced no frame does not repeat it.
   */
  updated: boolean;
}

/** Snapshot returned by {@link StreamAnalyzer.stats}. */
export interface StreamAnalyzerStats {
  totalFrames: number;
  totalSamples: number;
  durationSeconds: number;
  pendingFrames: number;
  droppedOutputFrames: number;
  droppedChordProgressionEntries: number;
  droppedBarProgressionEntries: number;
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
  /**
   * pYIN's per-frame voiced **observation mass**, exactly as librosa returns
   * it: the summed probability of the frame's voiced pitch hypotheses.
   *
   * This is NOT a signal-quality confidence and NOT a correction weight. The
   * mass depends on how many periods of the pitch fit inside `frameLength`,
   * because the CMNDF troughs of a long period measured over a short frame are
   * shallower. For a fixed `frameLength` it therefore rises with F0 even when
   * the signal is unchanged: a steady three-harmonic tone at 2048 samples /
   * 48 kHz averages well under 0.1 at C2 and about 0.5 at C5, with every frame
   * flagged voiced throughout.
   *
   * Use {@link voicedFlag} for any voicing decision. In particular, thresholding
   * this value at a fixed 0.5 (`noteSegments`' default) drops entire low
   * registers.
   */
  voicedProb: Float32Array;
  /** Per-frame voicing decision from the Viterbi path — the voicing oracle. */
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

/** Per-bin pitch candidates and peak magnitudes from spectral piptrack. */
export interface PiptrackResult {
  nBins: number;
  nFrames: number;
  pitches: Float32Array;
  magnitudes: Float32Array;
}

/** Magnitude and reassigned time/frequency coordinates for each STFT bin. */
export interface ReassignedSpectrogramResult {
  nBins: number;
  nFrames: number;
  magnitude: Float32Array;
  times: Float32Array;
  frequencies: Float32Array;
}

/** One stable monophonic note region segmented from an F0 track. */
export interface NoteSegment {
  frameStart: number;
  frameEnd: number;
  startSeconds: number;
  endSeconds: number;
  medianCents: number;
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
  /**
   * Resonance/slope. Ignored for a LowShelf/HighShelf band once `phase:
   * 'NaturalPhase'` forces Vicanek coefficients (the Vicanek matched-Z shelf
   * design has no Q/S parameter) -- the value is still stored and read back
   * verbatim, so a reflected `q` on such a band does not describe the applied
   * response. Set `coeffMode: 'Rbj'` for a Q-controllable shelf.
   */
  q?: number;
  enabled?: boolean;
  coeffMode?: 'Rbj' | 'Vicanek';
  slopeDbOct?: number;
  placement?: 'Stereo' | 'Left' | 'Right' | 'Mid' | 'Side';
  /**
   * `'NaturalPhase'` forces `coeffMode: 'Vicanek'` for this band, which
   * ignores `q` for LowShelf/HighShelf (see `q`'s doc).
   */
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
  /**
   * Delays the detector's view of the signal by this many ms; a larger value
   * makes the band react LATER, not earlier -- this is a detector delay, not
   * true look-ahead, and adds no latency to the audio path.
   */
  detectorDelayMs?: number;
  /** Former (misleading) spelling of `detectorDelayMs`, still accepted; `detectorDelayMs` wins if both are set. */
  lookaheadMs?: number;
  externalSidechain?: boolean;
  sidechainFreqHz?: number;
  sidechainQ?: number;
}

/**
 * Realtime-safe snapshot from {@link StreamingEqualizer.spectrum}.
 *
 * `preLeft`/`preRight` and `postLeft`/`postRight` are the pre- and post-EQ
 * waveform streams (uniformly decimated time-domain samples), so they are a
 * scope feed rather than a spectral estimate. `profileDb` is the frequency-domain
 * view: the post-EQ signal is Hann-windowed, transformed and its bin powers
 * summed into 16 geometrically spaced bands covering 20 Hz to 20 kHz, in
 * amplitude decibels relative to full scale, rising immediately and falling
 * smoothly.
 */
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
