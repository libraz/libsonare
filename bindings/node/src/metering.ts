import { addon } from './native.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

/** One contiguous run of clipped samples reported by `meteringDetectClipping`. */
export interface ClippingRegion {
  startSample: number;
  endSample: number;
  length: number;
  peak: number;
}

/** Aggregated clipping report (mirrors C SonareClippingResult). */
export interface ClippingReport {
  clippedSamples: number;
  clippingRatio: number;
  maxClippedPeak: number;
  regions: ClippingRegion[];
}

/** Sliding-window dynamic range report (mirrors C SonareDynamicRangeResult). */
export interface DynamicRangeReport {
  dynamicRangeDb: number;
  lowPercentileDb: number;
  highPercentileDb: number;
  windowRmsDb: Float32Array;
}

export function meteringPeakDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringPeakDb', samples, options.validate !== false);
  return addon.meteringPeakDb(samples, sampleRate);
}

export function meteringRmsDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringRmsDb', samples, options.validate !== false);
  return addon.meteringRmsDb(samples, sampleRate);
}

export function meteringCrestFactorDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringCrestFactorDb', samples, options.validate !== false);
  return addon.meteringCrestFactorDb(samples, sampleRate);
}

export function meteringDcOffset(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringDcOffset', samples, options.validate !== false);
  return addon.meteringDcOffset(samples, sampleRate);
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
  return addon.meteringTruePeakDb(samples, sampleRate, oversampleFactor);
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
  return addon.meteringDetectClipping(samples, sampleRate, threshold, minRegionSamples);
}

/**
 * Sliding-window dynamic range (high_percentile_db - low_percentile_db).
 * Pass 0 for window/hop to use the library default (window=3 s, hop=1 s). The
 * percentiles use a NEGATIVE sentinel for "use the library default" (low=0.10,
 * high=0.95) because 0 is a literal 0th percentile; omitted percentiles default
 * to -1.
 */
export function meteringDynamicRange(
  samples: Float32Array,
  sampleRate = 22050,
  windowSec = 0,
  hopSec = 0,
  lowPercentile = -1,
  highPercentile = -1,
  options: ValidateOptions = {},
): DynamicRangeReport {
  assertSamples('meteringDynamicRange', samples, options.validate !== false);
  return addon.meteringDynamicRange(
    samples,
    sampleRate,
    windowSec,
    hopSec,
    lowPercentile,
    highPercentile,
  );
}

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

/** Magnitude / power / dB spectrum returned by the metering spectrum functions. */
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
  return addon.meteringStereoCorrelation(left, right, sampleRate);
}

/**
 * Side / mid energy ratio, clamped to `[0, 2]`: 0 = pure mono, ~1 = wide
 * stereo, 2 = fully decorrelated / out-of-phase.
 */
export function meteringStereoWidth(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const validate = options.validate !== false;
  assertSamples('meteringStereoWidth', left, validate, 'left');
  assertSamples('meteringStereoWidth', right, validate, 'right');
  return addon.meteringStereoWidth(left, right, sampleRate);
}

/** Options for the decimated scope functions. */
export interface ScopeOptions extends ValidateOptions {
  /**
   * Upper bound on the returned point count. Omit / `0` (or a value `>= length`)
   * yields one point per input sample; otherwise the point cloud is
   * deterministically decimated to at most `maxPoints` points (keeping the
   * largest-radius sample per bucket) for display-sized output.
   */
  maxPoints?: number;
}

/**
 * Mid/side vectorscope point series. By default emits one point per input
 * sample; pass `maxPoints` to get a display-sized decimated point set.
 */
export function meteringVectorscope(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ScopeOptions = {},
): VectorscopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringVectorscope', left, validate, 'left');
  assertSamples('meteringVectorscope', right, validate, 'right');
  return addon.meteringVectorscope(left, right, sampleRate, options.maxPoints ?? 0);
}

/**
 * Phase-scope point series plus summary stats. By default emits one point per
 * input sample; pass `maxPoints` to decimate the point cloud for display. The
 * summary stats are always computed over the full-resolution signal.
 */
export function meteringPhaseScope(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ScopeOptions = {},
): PhaseScopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringPhaseScope', left, validate, 'left');
  assertSamples('meteringPhaseScope', right, validate, 'right');
  return addon.meteringPhaseScope(left, right, sampleRate, options.maxPoints ?? 0);
}

/**
 * Welch-averaged magnitude / power / dB spectrum over the WHOLE signal. This is
 * NOT a single-frame snapshot: the signal is split into Hann-windowed,
 * 50%-overlapping `nFft`-length frames whose power spectra are averaged across
 * the entire input, so transients are smeared by the averaging. For a true
 * single-frame FFT of one window, use {@link meteringSpectrumFrame}.
 */
export function meteringSpectrum(
  samples: Float32Array,
  sampleRate = 22050,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const validate = options?.validate !== false;
  assertSamples('meteringSpectrum', samples, validate);
  return addon.meteringSpectrum(samples, sampleRate, options ?? {});
}

/**
 * True single-frame magnitude / power / dB spectrum (one Hann-windowed
 * `nFft`-length FFT), for spectrum-analyzer "moment" snapshots that must not be
 * time-averaged like {@link meteringSpectrum}. The analysis frame spans
 * `[frameOffset, frameOffset + nFft)`; samples past the end are zero-padded.
 */
export function meteringSpectrumFrame(
  samples: Float32Array,
  sampleRate = 22050,
  frameOffset = 0,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const validate = options?.validate !== false;
  assertSamples('meteringSpectrumFrame', samples, validate);
  return addon.meteringSpectrumFrame(samples, sampleRate, frameOffset, options ?? {});
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `referenceMidi` defaults to A4 (69) when passed as 0.
 */
