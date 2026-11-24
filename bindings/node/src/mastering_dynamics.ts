import { withEntry } from './internal/entry.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertNonNegativeFloatValue,
  sampleOneShot,
} from './validation.js';

/** Detector mode for `masteringDynamicsCompressor`. */
export type CompressorDetector = 'peak' | 'rms' | 'logRms';

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions {
  /** Level above which compression starts, in dBFS. Default `-18`. */
  thresholdDb?: number;
  /** Compression ratio, at least 1; a smaller one is refused. Default `2`. */
  ratio?: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  releaseMs?: number;
  /** Soft-knee width in dB. Default `0`. */
  kneeDb?: number;
  /** Makeup gain in dB. Default `0`. */
  makeupGainDb?: number;
  /** Derive the makeup gain automatically. Default `false`. */
  autoMakeup?: boolean;
  /** Level detector: `'peak'`, `'rms'` or `'logRms'`. Default `'rms'`. */
  detector?: CompressorDetector;
  /** Sidechain high-pass corner in Hz. Use a finite positive value; omission or 0 disables the filter. */
  sidechainHighPassHz?: number;
  /** Program-dependent release time in ms. Default `0`. */
  pdrTimeMs?: number;
  /** Program-dependent release multiplier. Default `1`. */
  pdrReleaseScale?: number;
}

/** Options for `masteringDynamicsGate`. */
export interface GateOptions {
  /** Level above which the gate opens, in dBFS. Default `-50`. */
  thresholdDb?: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  releaseMs?: number;
  /** Attenuation applied while the gate is closed, in dB. Default `-80`. */
  rangeDb?: number;
  /** Minimum time the gate stays open, in ms. Default `0`. */
  holdMs?: number;
  /** Level in dBFS below which an open gate closes (hysteresis), at most `thresholdDb`. Default `-50`. */
  closeThresholdDb?: number;
  /** Key high-pass corner in Hz. Absent = off; 0 Hz passes everything and is accepted. */
  keyHighPassHz?: number;
}

/** Options for `masteringDynamicsTransientShaper`. */
export interface TransientShaperOptions {
  /** Gain applied to attacks, in dB. Default `3`. */
  attackGainDb?: number;
  /** Gain applied to sustain, in dB. Default `0`. */
  sustainGainDb?: number;
  /** Attack of the fast envelope follower, in ms. Default `0`. */
  fastAttackMs?: number;
  /** Release of the fast envelope follower, in ms. Default `20`. */
  fastReleaseMs?: number;
  /** Attack of the slow envelope follower, in ms. Default `15`. */
  slowAttackMs?: number;
  /** Release of the slow envelope follower, in ms. Default `200`. */
  slowReleaseMs?: number;
  /** Detection sensitivity, at least 0; a negative one is refused. Default `1`. */
  sensitivity?: number;
  /** Safety clamp on the applied gain, in dB. Default `12`. */
  maxGainDb?: number;
  /** Smoothing of the gain curve in ms; 0 disables it. Default `0`. */
  gainSmoothingMs?: number;
  /** Lookahead in ms; 0 disables it. Default `0`. */
  lookaheadMs?: number;
}

/** Result of an offline dynamics processor call. */
export interface DynamicsProcessorResult {
  /** Processed audio, latency already compensated. */
  samples: Float32Array;
  /**
   * The processor's own latency, already compensated in the returned audio;
   * do not trim the returned audio by it.
   */
  latencySamples: number;
}

/** Canonical request form for the offline compressor. */
export interface MasteringDynamicsCompressorRequest extends CompressorOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Canonical request form for the offline noise gate. */
export interface MasteringDynamicsGateRequest extends GateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Canonical request form for the offline transient shaper. */
export interface MasteringDynamicsTransientShaperRequest extends TransientShaperOptions {
  /** Mono input samples. */
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
  return withEntry('masteringDynamicsCompressor', () => {
    const r = sampleOneShot<MasteringDynamicsCompressorRequest>(
      'masteringDynamicsCompressor',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    const opts = { ...r } as MasteringDynamicsCompressorRequest & Record<string, unknown>;
    if (!isAbsent(r.detector)) {
      assertClosedToken(r.detector, COMPRESSOR_DETECTORS, 'detector');
      opts.detector = COMPRESSOR_DETECTOR_ORDINAL[r.detector] as never;
    }
    return addon.masteringDynamicsCompressor(r.samples, r.sampleRate, opts);
  });
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
  return withEntry('masteringDynamicsGate', () => {
    const r = sampleOneShot<MasteringDynamicsGateRequest>('masteringDynamicsGate', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    if (!isAbsent(r.keyHighPassHz)) {
      assertNonNegativeFloatValue(r.keyHighPassHz, 'keyHighPassHz');
    }
    return addon.masteringDynamicsGate(r.samples, r.sampleRate, r);
  });
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
  return withEntry('masteringDynamicsTransientShaper', () => {
    const r = sampleOneShot<MasteringDynamicsTransientShaperRequest>(
      'masteringDynamicsTransientShaper',
      args,
    );
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.masteringDynamicsTransientShaper(r.samples, r.sampleRate, r);
  });
}
