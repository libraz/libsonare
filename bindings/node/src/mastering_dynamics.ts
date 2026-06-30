import { addon } from './native.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

/** Detector mode for `masteringDynamicsCompressor`. */
export type CompressorDetector = 'peak' | 'rms' | 'log_rms' | 'logRms' | 0 | 1 | 2;

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions extends ValidateOptions {
  thresholdDb?: number;
  ratio?: number;
  attackMs?: number;
  releaseMs?: number;
  kneeDb?: number;
  makeupGainDb?: number;
  autoMakeup?: boolean;
  detector?: CompressorDetector;
  sidechainHpfEnabled?: boolean;
  sidechainHpfHz?: number;
  pdrTimeMs?: number;
  pdrReleaseScale?: number;
}

/** Options for `masteringDynamicsGate`. */
export interface GateOptions extends ValidateOptions {
  thresholdDb?: number;
  attackMs?: number;
  releaseMs?: number;
  rangeDb?: number;
  holdMs?: number;
  closeThresholdDb?: number;
  keyHpfHz?: number;
}

/** Options for `masteringDynamicsTransientShaper`. */
export interface TransientShaperOptions extends ValidateOptions {
  attackGainDb?: number;
  sustainGainDb?: number;
  fastAttackMs?: number;
  fastReleaseMs?: number;
  slowAttackMs?: number;
  slowReleaseMs?: number;
  sensitivity?: number;
  maxGainDb?: number;
  gainSmoothingMs?: number;
  lookaheadMs?: number;
}

/** Result of an offline dynamics processor call. */
export interface DynamicsProcessorResult {
  samples: Float32Array;
  latencySamples: number;
}

/** Offline feed-forward compressor (soft-knee, optional makeup, sidechain HPF, PDR). */
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate = 22050,
  options: CompressorOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsCompressor', samples, options.validate !== false);
  return addon.masteringDynamicsCompressor(samples, sampleRate, options);
}

/** Offline noise gate with hysteresis, hold, and optional key HPF. */
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate = 22050,
  options: GateOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsGate', samples, options.validate !== false);
  return addon.masteringDynamicsGate(samples, sampleRate, options);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate = 22050,
  options: TransientShaperOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsTransientShaper', samples, options.validate !== false);
  return addon.masteringDynamicsTransientShaper(samples, sampleRate, options);
}
