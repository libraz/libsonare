import { addon } from './native.js';
import type { ValidateOptions } from './validation.js';
import { assertAudioInput, requestObject } from './validation.js';

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
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
  latencySamples: number;
}

/** Canonical request form for the offline compressor. */
export interface MasteringDynamicsCompressorRequest extends CompressorOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for the offline noise gate. */
export interface MasteringDynamicsGateRequest extends GateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for the offline transient shaper. */
export interface MasteringDynamicsTransientShaperRequest extends TransientShaperOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Offline feed-forward compressor (soft-knee, optional makeup, sidechain HPF, PDR). */
export function masteringDynamicsCompressor(
  request: MasteringDynamicsCompressorRequest,
): DynamicsProcessorResult;
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate?: number,
  options?: CompressorOptions,
): DynamicsProcessorResult;
export function masteringDynamicsCompressor(
  samples: Float32Array | MasteringDynamicsCompressorRequest,
  sampleRate = 22050,
  options: CompressorOptions = {},
): DynamicsProcessorResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('masteringDynamicsCompressor', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringDynamicsCompressor', request.samples, resolvedSampleRate, request);
  return addon.masteringDynamicsCompressor(request.samples, resolvedSampleRate, request);
}

/** Offline noise gate with hysteresis, hold, and optional key HPF. */
export function masteringDynamicsGate(
  request: MasteringDynamicsGateRequest,
): DynamicsProcessorResult;
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate?: number,
  options?: GateOptions,
): DynamicsProcessorResult;
export function masteringDynamicsGate(
  samples: Float32Array | MasteringDynamicsGateRequest,
  sampleRate = 22050,
  options: GateOptions = {},
): DynamicsProcessorResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('masteringDynamicsGate', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringDynamicsGate', request.samples, resolvedSampleRate, request);
  return addon.masteringDynamicsGate(request.samples, resolvedSampleRate, request);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  request: MasteringDynamicsTransientShaperRequest,
): DynamicsProcessorResult;
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate?: number,
  options?: TransientShaperOptions,
): DynamicsProcessorResult;
export function masteringDynamicsTransientShaper(
  samples: Float32Array | MasteringDynamicsTransientShaperRequest,
  sampleRate = 22050,
  options: TransientShaperOptions = {},
): DynamicsProcessorResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('masteringDynamicsTransientShaper', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringDynamicsTransientShaper',
    request.samples,
    resolvedSampleRate,
    request,
  );
  return addon.masteringDynamicsTransientShaper(request.samples, resolvedSampleRate, request);
}
