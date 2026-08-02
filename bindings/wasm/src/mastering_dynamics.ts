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

/** Canonical request form for the offline compressor. */
export interface MasteringDynamicsCompressorRequest extends CompressorOptions {
  samples: Float32Array;
  sampleRate: number;
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

/** Canonical request form for the offline gate. */
export interface MasteringDynamicsGateRequest extends GateOptions {
  samples: Float32Array;
  sampleRate: number;
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

/** Canonical request form for the offline transient shaper. */
export interface MasteringDynamicsTransientShaperRequest extends TransientShaperOptions {
  samples: Float32Array;
  sampleRate: number;
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
  request: MasteringDynamicsCompressorRequest,
): DynamicsResult;
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate: number,
  options?: CompressorOptions,
): DynamicsResult;
export function masteringDynamicsCompressor(
  samples: Float32Array | MasteringDynamicsCompressorRequest,
  sampleRate?: number,
  options: CompressorOptions = {},
): DynamicsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  assertSamples('masteringDynamicsCompressor', request.samples, request.validate !== false);
  const detector =
    typeof request.detector === 'string'
      ? COMPRESSOR_DETECTOR_MAP[request.detector]
      : request.detector;
  // Preserve inherited option values as well as own properties. The embind
  // boundary's hasProperty() intentionally follows the prototype chain, so a
  // spread copy here would otherwise erase values before C++ can read them.
  const opts: Record<string, unknown> = Object.assign(
    Object.create(Object.getPrototypeOf(request)) as Record<string, unknown>,
    request,
  );
  if (detector !== undefined) {
    opts.detector = detector;
  }
  return requireModule().masteringDynamicsCompressor(request.samples, request.sampleRate, opts);
}

/** Offline noise gate (hysteresis, hold, optional key HPF). */
export function masteringDynamicsGate(request: MasteringDynamicsGateRequest): DynamicsResult;
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate: number,
  options?: GateOptions,
): DynamicsResult;
export function masteringDynamicsGate(
  samples: Float32Array | MasteringDynamicsGateRequest,
  sampleRate?: number,
  options: GateOptions = {},
): DynamicsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  assertSamples('masteringDynamicsGate', request.samples, request.validate !== false);
  return requireModule().masteringDynamicsGate(request.samples, request.sampleRate, request);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  request: MasteringDynamicsTransientShaperRequest,
): DynamicsResult;
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate: number,
  options?: TransientShaperOptions,
): DynamicsResult;
export function masteringDynamicsTransientShaper(
  samples: Float32Array | MasteringDynamicsTransientShaperRequest,
  sampleRate?: number,
  options: TransientShaperOptions = {},
): DynamicsResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  assertSamples('masteringDynamicsTransientShaper', request.samples, request.validate !== false);
  return requireModule().masteringDynamicsTransientShaper(
    request.samples,
    request.sampleRate,
    request,
  );
}
