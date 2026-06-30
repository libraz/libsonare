import { getSonareModule } from './module_state';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Mastering repair (declick, denoise_classical, declip, decrackle, dehum,
// dereverb_classical, trim_silence) — hand-written bindings.
// ============================================================================

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions {
  mode?: DenoiseClassicalMode;
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  nFft?: number;
  hopLength?: number;
  ddAlpha?: number;
  gainFloor?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  noiseEstimationQuantile?: number;
  speechPresenceGain?: boolean;
  gainSmoothing?: boolean;
}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate: number,
  options: DeclickOptions = {},
): Float32Array {
  return requireModule().masteringRepairDeclick(samples, sampleRate, options);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate: number,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  return requireModule().masteringRepairDenoiseClassical(samples, sampleRate, options);
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}

/** Options for `masteringRepairDehum`. */
export interface DehumOptions {
  fundamentalHz?: number;
  harmonics?: number;
  q?: number;
  adaptive?: boolean;
  searchRangeHz?: number;
  adaptation?: number;
  frameSize?: number;
  pllBandwidth?: number;
}

/** Options for `masteringRepairDereverbClassical`. */
export interface DereverbClassicalOptions {
  threshold?: number;
  attenuation?: number;
  nFft?: number;
  hopLength?: number;
  t60Sec?: number;
  lateDelayMs?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  wpeEnabled?: boolean;
  wpeIterations?: number;
  wpeTaps?: number;
  wpeStrength?: number;
}

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  threshold?: number;
  paddingSamples?: number;
  mode?: TrimSilenceMode;
  gateLufs?: number;
  windowMs?: number;
}

/** Offline LPC-based declipper. */
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate: number,
  options: DeclipOptions = {},
): Float32Array {
  return requireModule().masteringRepairDeclip(samples, sampleRate, options);
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate: number,
  options: DecrackleOptions = {},
): Float32Array {
  return requireModule().masteringRepairDecrackle(samples, sampleRate, options);
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options: DehumOptions = {},
): Float32Array {
  return requireModule().masteringRepairDehum(samples, sampleRate, options);
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options: DereverbClassicalOptions = {},
): Float32Array {
  return requireModule().masteringRepairDereverbClassical(samples, sampleRate, options);
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options: TrimSilenceOptions = {},
): Float32Array {
  return requireModule().masteringRepairTrimSilence(samples, sampleRate, options);
}
