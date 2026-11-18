import { addon } from './native.js';
import type { ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertNonNegativeFloatValue,
  assertPositiveFloatValue,
  sampleOneShot,
} from './validation.js';

/** Detector mode for `masteringDynamicsCompressor`. */
export type CompressorDetector = 'peak' | 'rms' | 'logRms';

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions extends ValidateOptions {
  thresholdDb?: number;
  ratio?: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  releaseMs?: number;
  kneeDb?: number;
  makeupGainDb?: number;
  autoMakeup?: boolean;
  detector?: CompressorDetector;
  /** Sidechain high-pass corner in Hz, above 0. Absent = no sidechain high-pass. */
  sidechainHighPassHz?: number;
  pdrTimeMs?: number;
  pdrReleaseScale?: number;
}

/** Options for `masteringDynamicsGate`. */
export interface GateOptions extends ValidateOptions {
  thresholdDb?: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  releaseMs?: number;
  rangeDb?: number;
  holdMs?: number;
  closeThresholdDb?: number;
  /** Key high-pass corner in Hz. Absent = off; 0 Hz passes everything and is accepted. */
  keyHighPassHz?: number;
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
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Canonical request form for the offline noise gate. */
export interface MasteringDynamicsGateRequest extends GateOptions {
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Canonical request form for the offline transient shaper. */
export interface MasteringDynamicsTransientShaperRequest extends TransientShaperOptions {
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

const COMPRESSOR_DETECTORS = ['peak', 'rms', 'logRms'] as const;

// The addon reads the detector as an ordinal.
const COMPRESSOR_DETECTOR_ORDINAL: Record<CompressorDetector, number> = {
  peak: 0,
  rms: 1,
  logRms: 2,
};

/** Offline feed-forward compressor (soft-knee, optional makeup, sidechain high-pass, PDR). */
export function masteringDynamicsCompressor(
  request: MasteringDynamicsCompressorRequest,
): DynamicsProcessorResult;
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate: number,
  options?: CompressorOptions,
): DynamicsProcessorResult;
export function masteringDynamicsCompressor(...args: unknown[]): DynamicsProcessorResult {
  const r = sampleOneShot<MasteringDynamicsCompressorRequest>('masteringDynamicsCompressor', args);
  assertAudioInput('masteringDynamicsCompressor', r.samples, r.sampleRate, r);
  const { sidechainHighPassHz, ...opts } = r as MasteringDynamicsCompressorRequest &
    Record<string, unknown>;
  if (r.detector !== undefined && r.detector !== null) {
    assertClosedToken('masteringDynamicsCompressor', r.detector, COMPRESSOR_DETECTORS, 'detector');
    opts.detector = COMPRESSOR_DETECTOR_ORDINAL[r.detector] as never;
  }
  // The addon still reads an enable flag plus a corner for the sidechain high-pass.
  if (sidechainHighPassHz !== undefined && sidechainHighPassHz !== null) {
    assertPositiveFloatValue(
      'masteringDynamicsCompressor',
      sidechainHighPassHz,
      'sidechainHighPassHz',
    );
    opts.sidechainHpfEnabled = true;
    opts.sidechainHpfHz = sidechainHighPassHz;
  }
  return addon.masteringDynamicsCompressor(r.samples, r.sampleRate, opts);
}

/** Offline noise gate with hysteresis, hold, and optional key high-pass. */
export function masteringDynamicsGate(
  request: MasteringDynamicsGateRequest,
): DynamicsProcessorResult;
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate: number,
  options?: GateOptions,
): DynamicsProcessorResult;
export function masteringDynamicsGate(...args: unknown[]): DynamicsProcessorResult {
  const r = sampleOneShot<MasteringDynamicsGateRequest>('masteringDynamicsGate', args);
  assertAudioInput('masteringDynamicsGate', r.samples, r.sampleRate, r);
  const { keyHighPassHz, ...opts } = r as MasteringDynamicsGateRequest & Record<string, unknown>;
  // The addon still spells the key high-pass `keyHpfHz`.
  if (keyHighPassHz !== undefined && keyHighPassHz !== null) {
    assertNonNegativeFloatValue('masteringDynamicsGate', keyHighPassHz, 'keyHighPassHz');
    opts.keyHpfHz = keyHighPassHz;
  }
  return addon.masteringDynamicsGate(r.samples, r.sampleRate, opts);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  request: MasteringDynamicsTransientShaperRequest,
): DynamicsProcessorResult;
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate: number,
  options?: TransientShaperOptions,
): DynamicsProcessorResult;
export function masteringDynamicsTransientShaper(...args: unknown[]): DynamicsProcessorResult {
  const r = sampleOneShot<MasteringDynamicsTransientShaperRequest>(
    'masteringDynamicsTransientShaper',
    args,
  );
  assertAudioInput('masteringDynamicsTransientShaper', r.samples, r.sampleRate, r);
  return addon.masteringDynamicsTransientShaper(r.samples, r.sampleRate, r);
}
