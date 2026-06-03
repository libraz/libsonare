import { getSonareModule } from './module_state';
import { assertSamples } from './validation';
import type { ValidateOptions } from './validation';

// ============================================================================
// Metering — basic / true-peak / clipping / dynamic range
// ============================================================================

/** One contiguous run of clipped samples reported by `meteringDetectClipping`. */
export interface ClippingRegion {
  startSample: number;
  endSample: number;
  length: number;
  peak: number;
}

/** Aggregated clipping report. */
export interface ClippingReport {
  clippedSamples: number;
  clippingRatio: number;
  maxClippedPeak: number;
  regions: ClippingRegion[];
}

/** Sliding-window dynamic range report. */
export interface DynamicRangeReport {
  dynamicRangeDb: number;
  lowPercentileDb: number;
  highPercentileDb: number;
  windowRmsDb: Float32Array;
}

function requireModule() {
  return getSonareModule();
}

export function meteringPeakDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringPeakDb', samples, options.validate !== false);
  return requireModule().meteringPeakDb(samples, sampleRate);
}

export function meteringRmsDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringRmsDb', samples, options.validate !== false);
  return requireModule().meteringRmsDb(samples, sampleRate);
}

export function meteringCrestFactorDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringCrestFactorDb', samples, options.validate !== false);
  return requireModule().meteringCrestFactorDb(samples, sampleRate);
}

export function meteringDcOffset(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringDcOffset', samples, options.validate !== false);
  return requireModule().meteringDcOffset(samples, sampleRate);
}

/**
 * Inter-sample (true) peak in dBFS. `oversampleFactor` must be a power of two
 * in [1, 16]; pass 0 to use the library default (4).
 */
export function meteringTruePeakDb(
  samples: Float32Array,
  sampleRate = 22050,
  oversampleFactor = 4,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringTruePeakDb', samples, options.validate !== false);
  return requireModule().meteringTruePeakDb(samples, sampleRate, oversampleFactor);
}

/**
 * Detect contiguous runs of clipped samples.
 *
 * @param threshold Linear absolute threshold (default 0.999).
 * @param minRegionSamples Minimum run length to report (default 1).
 */
export function meteringDetectClipping(
  samples: Float32Array,
  sampleRate = 22050,
  threshold = 0.999,
  minRegionSamples = 1,
  options: ValidateOptions = {},
): ClippingReport {
  assertSamples('meteringDetectClipping', samples, options.validate !== false);
  return requireModule().meteringDetectClipping(samples, sampleRate, threshold, minRegionSamples);
}

/**
 * Sliding-window dynamic range. Pass 0 for any parameter to use the library
 * default (window=3 s, hop=1 s, low=0.10, high=0.95).
 */
export function meteringDynamicRange(
  samples: Float32Array,
  sampleRate = 22050,
  windowSec = 0,
  hopSec = 0,
  lowPercentile = 0,
  highPercentile = 0,
  options: ValidateOptions = {},
): DynamicRangeReport {
  assertSamples('meteringDynamicRange', samples, options.validate !== false);
  return requireModule().meteringDynamicRange(
    samples,
    sampleRate,
    windowSec,
    hopSec,
    lowPercentile,
    highPercentile,
  );
}

// ============================================================================
// Metering — stereo / phase-scope / spectrum
// ============================================================================

/** Mid/side vectorscope point series for a (left, right) stereo pair. */
export interface VectorscopeReport {
  mid: Float32Array;
  side: Float32Array;
}

/** Phase-scope (Lissajous) point series plus summary stats. */
export interface PhaseScopeReport {
  mid: Float32Array;
  side: Float32Array;
  radius: Float32Array;
  angleRad: Float32Array;
  correlation: number;
  averageAbsAngleRad: number;
  maxRadius: number;
}

/** Options for `meteringSpectrum`. */
export interface SpectrumOptions {
  /** FFT size. Pass 0 / omit for the library default (2048). */
  nFft?: number;
  /** Apply fractional-octave smoothing to magnitude. */
  applyOctaveSmoothing?: boolean;
  /** Smoothing fraction (e.g. 3 = 1/3-octave). 0 / omit = library default (3). */
  octaveFraction?: number;
  /** Linear reference for the dB conversion. 0 / omit = 1.0. */
  dbRef?: number;
  /** Linear floor used to avoid log(0). 0 / omit = library default. */
  dbAmin?: number;
}

/** Single-frame magnitude / power / dB spectrum returned by `meteringSpectrum`. */
export interface SpectrumReport {
  frequencies: Float32Array;
  magnitude: Float32Array;
  power: Float32Array;
  db: Float32Array;
  nFft: number;
  sampleRate: number;
}

/** Pearson correlation in [-1, 1] between two equal-length channels. */
export function meteringStereoCorrelation(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const validate = options.validate !== false;
  assertSamples('meteringStereoCorrelation', left, validate, 'left');
  assertSamples('meteringStereoCorrelation', right, validate, 'right');
  return requireModule().meteringStereoCorrelation(left, right, sampleRate);
}

/** Side / mid energy ratio: 0 = pure mono, ~1 = wide stereo. */
export function meteringStereoWidth(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const validate = options.validate !== false;
  assertSamples('meteringStereoWidth', left, validate, 'left');
  assertSamples('meteringStereoWidth', right, validate, 'right');
  return requireModule().meteringStereoWidth(left, right, sampleRate);
}

/** Per-sample mid/side point series (one entry per input frame). */
export function meteringVectorscope(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): VectorscopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringVectorscope', left, validate, 'left');
  assertSamples('meteringVectorscope', right, validate, 'right');
  return requireModule().meteringVectorscope(left, right, sampleRate);
}

/** Phase-scope point series plus summary stats. */
export function meteringPhaseScope(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): PhaseScopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringPhaseScope', left, validate, 'left');
  assertSamples('meteringPhaseScope', right, validate, 'right');
  return requireModule().meteringPhaseScope(left, right, sampleRate);
}

/** Single-frame spectrum view (uses the first `nFft` samples of `samples`). */
export function meteringSpectrum(
  samples: Float32Array,
  sampleRate = 22050,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const validate = options?.validate !== false;
  assertSamples('meteringSpectrum', samples, validate);
  return requireModule().meteringSpectrum(samples, sampleRate, options ?? {});
}
