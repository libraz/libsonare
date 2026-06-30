import type { ValidateOptions } from './validation';

/** Options for `detectAcoustic`. All fields are optional. */
export interface AcousticOptions extends ValidateOptions {
  /** Number of octave bands. Default 6. */
  nOctaveBands?: number;
  /** Number of 1/3-octave sub-bands. Default 24. */
  nThirdOctaveSubbands?: number;
  /** Minimum decay range (dB) for a valid RT60 fit. Default 30. */
  minDecayDb?: number;
  /** Margin (dB) above the noise floor for the decay fit. Default 10. */
  noiseFloorMarginDb?: number;
}

/**
 * Room acoustic parameters from an impulse response
 */
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
