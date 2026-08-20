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
export interface MasteringRepairDeclickRequest extends DeclickOptions {
  samples: Float32Array;
  sampleRate: number;
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
export interface MasteringRepairDenoiseClassicalRequest extends DenoiseClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(request: MasteringRepairDeclickRequest): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclickOptions,
): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array | MasteringRepairDeclickRequest,
  sampleRate?: number,
  options: DeclickOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDeclick(request.samples, request.sampleRate, request);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  request: MasteringRepairDenoiseClassicalRequest,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DenoiseClassicalOptions,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array | MasteringRepairDenoiseClassicalRequest,
  sampleRate?: number,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDenoiseClassical(
    request.samples,
    request.sampleRate,
    request,
  );
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}
export interface MasteringRepairDeclipRequest extends DeclipOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}
export interface MasteringRepairDecrackleRequest extends DecrackleOptions {
  samples: Float32Array;
  sampleRate: number;
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
export interface MasteringRepairDehumRequest extends DehumOptions {
  samples: Float32Array;
  sampleRate: number;
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
export interface MasteringRepairDereverbClassicalRequest extends DereverbClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
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
export interface MasteringRepairTrimSilenceRequest extends TrimSilenceOptions {
  samples: Float32Array;
  sampleRate: number;
}

/**
 * Offline LPC-based declipper.
 *
 * Only clipped runs of at most 512 consecutive samples are reconstructed with the LPC solver.
 * The cap is a fixed sample count: it is not derived from `lpcOrder`, from `sampleRate`, or from
 * any other option, so its duration depends on the rate (~10.7 ms at 48 kHz). A longer run is
 * filled with cubic / linear interpolation instead, which keeps the solver's dense matrices
 * bounded by the cap rather than by the input. Exceeding the cap silently changes the
 * reconstruction method rather than throwing: `lpcOrder`, `iterations` and `lpcBlend` have no
 * effect on the interpolated run.
 */
export function masteringRepairDeclip(request: MasteringRepairDeclipRequest): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclipOptions,
): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array | MasteringRepairDeclipRequest,
  sampleRate?: number,
  options: DeclipOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDeclip(request.samples, request.sampleRate, request);
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(request: MasteringRepairDecrackleRequest): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate: number,
  options?: DecrackleOptions,
): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array | MasteringRepairDecrackleRequest,
  sampleRate?: number,
  options: DecrackleOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDecrackle(request.samples, request.sampleRate, request);
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options?: DehumOptions,
): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array | MasteringRepairDehumRequest,
  sampleRate?: number,
  options: DehumOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDehum(request.samples, request.sampleRate, request);
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  request: MasteringRepairDereverbClassicalRequest,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array | MasteringRepairDereverbClassicalRequest,
  sampleRate?: number,
  options: DereverbClassicalOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDereverbClassical(
    request.samples,
    request.sampleRate,
    request,
  );
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array | MasteringRepairTrimSilenceRequest,
  sampleRate?: number,
  options: TrimSilenceOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairTrimSilence(request.samples, request.sampleRate, request);
}
