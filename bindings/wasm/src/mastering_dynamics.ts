import { getSonareModule } from './module_state.js';
import type { ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertNonNegativeFloatValue,
  assertPositiveFloatValue,
  requireToken,
  sampleOneShot,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Mastering — offline dynamics processors (compressor / gate / transient_shaper)
// ============================================================================

/** Compressor sidechain detector mode. */
export type CompressorDetector = 'peak' | 'rms' | 'logRms';

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
  /** Sidechain high-pass corner in Hz, above 0. Absent = no sidechain high-pass. */
  sidechainHighPassHz?: number;
  pdrTimeMs?: number;
  pdrReleaseScale?: number;
}

/** Canonical request form for the offline compressor. */
export interface MasteringDynamicsCompressorRequest extends CompressorOptions {
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
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
  /** Key high-pass corner in Hz. Absent = off; 0 Hz passes everything and is accepted. */
  keyHighPassHz?: number;
}

/** Canonical request form for the offline gate. */
export interface MasteringDynamicsGateRequest extends GateOptions {
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
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
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/**
 * Result envelope returned by offline mastering dynamics processors.
 *
 * Named for what it is rather than for the module it lives in. {@link
 * DynamicsResult} is the *analysis* shape on every binding, so having that one
 * identifier mean two disjoint field lists across the Node and WASM packages
 * made a shared TypeScript module type-check against one and fail against the
 * other — with the identifier resolving either way, so only the member list
 * gave it away.
 */
export interface DynamicsProcessorResult {
  samples: Float32Array;
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
  latencySamples: number;
}

const COMPRESSOR_DETECTOR_TOKENS = ['peak', 'rms', 'logRms'] as const;

// Native reads the detector as an ordinal.
const COMPRESSOR_DETECTOR_ORDINAL: Record<CompressorDetector, number> = {
  peak: 0,
  rms: 1,
  logRms: 2,
};

/**
 * The options object a native dynamics call reads. Inherited option values are preserved
 * as well as own properties: the embind boundary's hasProperty() intentionally follows the
 * prototype chain, so a spread copy would erase values before C++ can read them.
 */
function nativeOptions(request: object): Record<string, unknown> {
  return Object.assign(
    Object.create(Object.getPrototypeOf(request)) as Record<string, unknown>,
    request,
  );
}

/** Offline feed-forward compressor (soft knee, optional auto-makeup / sidechain high-pass). */
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
  const opts = nativeOptions(r);
  if (r.detector !== undefined && r.detector !== null) {
    const token = requireToken(
      'masteringDynamicsCompressor',
      r.detector,
      'detector',
      COMPRESSOR_DETECTOR_TOKENS,
      'peak',
    );
    opts.detector = COMPRESSOR_DETECTOR_ORDINAL[token];
  }
  // Native still reads an enable flag plus a corner for the sidechain high-pass.
  delete opts.sidechainHighPassHz;
  if (r.sidechainHighPassHz !== undefined && r.sidechainHighPassHz !== null) {
    assertPositiveFloatValue(
      'masteringDynamicsCompressor',
      r.sidechainHighPassHz,
      'sidechainHighPassHz',
    );
    opts.sidechainHpfEnabled = true;
    opts.sidechainHpfHz = r.sidechainHighPassHz;
  }
  return requireModule().masteringDynamicsCompressor(r.samples, r.sampleRate, opts);
}

/** Offline noise gate (hysteresis, hold, optional key high-pass). */
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
  const opts = nativeOptions(r);
  // Native still spells the key high-pass `keyHpfHz`.
  delete opts.keyHighPassHz;
  if (r.keyHighPassHz !== undefined && r.keyHighPassHz !== null) {
    assertNonNegativeFloatValue('masteringDynamicsGate', r.keyHighPassHz, 'keyHighPassHz');
    opts.keyHpfHz = r.keyHighPassHz;
  }
  return requireModule().masteringDynamicsGate(r.samples, r.sampleRate, opts);
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
  return requireModule().masteringDynamicsTransientShaper(r.samples, r.sampleRate, r);
}
