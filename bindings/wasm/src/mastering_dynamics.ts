import { getSonareModule } from './module_state';
import type { ValidateOptions } from './validation';
import { assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Mastering — offline dynamics processors (compressor / gate / transient_shaper)
// ============================================================================

/** Compressor sidechain detector mode. */
export type CompressorDetector = 'peak' | 'rms' | 'log_rms';

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions extends ValidateOptions {
  thresholdDb?: number;
  ratio?: number;
  attackMs?: number;
  releaseMs?: number;
  kneeDb?: number;
  makeupGainDb?: number;
  autoMakeup?: boolean;
  detector?: CompressorDetector | number;
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

/** Result envelope returned by offline mastering dynamics processors. */
export interface DynamicsResult {
  samples: Float32Array;
  latencySamples: number;
}

const COMPRESSOR_DETECTOR_MAP: Record<CompressorDetector, number> = {
  peak: 0,
  rms: 1,
  log_rms: 2,
};

/** Offline feed-forward compressor (soft knee, optional auto-makeup / sidechain HPF). */
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate: number,
  options: CompressorOptions = {},
): DynamicsResult {
  assertSamples('masteringDynamicsCompressor', samples, options.validate !== false);
  const detector =
    typeof options.detector === 'string'
      ? COMPRESSOR_DETECTOR_MAP[options.detector]
      : options.detector;
  const opts: Record<string, unknown> = { ...options };
  if (detector !== undefined) {
    opts.detector = detector;
  }
  return requireModule().masteringDynamicsCompressor(samples, sampleRate, opts);
}

/** Offline noise gate (hysteresis, hold, optional key HPF). */
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate: number,
  options: GateOptions = {},
): DynamicsResult {
  assertSamples('masteringDynamicsGate', samples, options.validate !== false);
  return requireModule().masteringDynamicsGate(samples, sampleRate, options);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate: number,
  options: TransientShaperOptions = {},
): DynamicsResult {
  assertSamples('masteringDynamicsTransientShaper', samples, options.validate !== false);
  return requireModule().masteringDynamicsTransientShaper(samples, sampleRate, options);
}
