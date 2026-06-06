import { getSonareModule } from './module_state';
import type { ValidateOptions } from './validation';
import { assertSamples } from './validation';

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

/** Options for {@link meteringDetectClipping}. All fields are optional. */
export interface MeteringDetectClippingOptions extends ValidateOptions {
  /** Linear absolute threshold. Default 0.999. */
  threshold?: number;
  /** Minimum run length to report. Default 1. */
  minRegionSamples?: number;
}

/** Options for {@link meteringDynamicRange}. All fields are optional. */
export interface MeteringDynamicRangeOptions extends ValidateOptions {
  /** Window length in seconds (0 = library default, 3 s). Default 0. */
  windowSec?: number;
  /** Hop length in seconds (0 = library default, 1 s). Default 0. */
  hopSec?: number;
  /** Low percentile in [0,1] (negative = library default, 0.10). Default -1. */
  lowPercentile?: number;
  /** High percentile in [0,1] (negative = library default, 0.95). Default -1. */
  highPercentile?: number;
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
  const factor = oversampleFactor === 0 ? 4 : oversampleFactor;
  if (factor < 1 || factor > 16 || (factor & (factor - 1)) !== 0) {
    throw new RangeError(
      'meteringTruePeakDb: oversampleFactor must be 0 or a power of two from 1 to 16',
    );
  }
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
  options: MeteringDetectClippingOptions = {},
): ClippingReport {
  assertSamples('meteringDetectClipping', samples, options.validate !== false);
  return requireModule().meteringDetectClipping(
    samples,
    sampleRate,
    options.threshold ?? 0.999,
    options.minRegionSamples ?? 1,
  );
}

/**
 * Sliding-window dynamic range. Pass 0 for window/hop to use the library
 * default (window=3 s, hop=1 s). The percentiles use a NEGATIVE sentinel for
 * "use the library default" (low=0.10, high=0.95) because 0 is a literal 0th
 * percentile; omitted percentiles therefore default to -1.
 */
export function meteringDynamicRange(
  samples: Float32Array,
  sampleRate = 22050,
  options: MeteringDynamicRangeOptions = {},
): DynamicRangeReport {
  assertSamples('meteringDynamicRange', samples, options.validate !== false);
  return requireModule().meteringDynamicRange(
    samples,
    sampleRate,
    options.windowSec ?? 0,
    options.hopSec ?? 0,
    options.lowPercentile ?? -1,
    options.highPercentile ?? -1,
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

/** Options for {@link waveformPeaks}. All fields are optional. */
export interface WaveformPeaksOptions extends ValidateOptions {
  /** Bucket width in frames. Default 512. */
  samplesPerBucket?: number;
}

/** Options for {@link waveformPeakPyramid}. All fields are optional. */
export interface WaveformPeakPyramidOptions extends ValidateOptions {
  /** Bucket widths in frames, one per zoom level. Default [512, 1024, 2048, 4096]. */
  samplesPerBucketLevels?: number[];
}

/** Per-channel min/max waveform buckets. Arrays are channel-major. */
export interface WaveformPeaksReport {
  min: Float32Array;
  max: Float32Array;
  channels: number;
  bucketCount: number;
  samplesPerBucket: number;
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

/**
 * Display-sized mid/side vectorscope. Like {@link meteringVectorscope} but the
 * point series is deterministically decimated to at most `maxPoints` points
 * (`0`, or a value `>= length`, yields one point per input sample). Mirrors the
 * Node/Python decimated vectorscope.
 */
export function meteringVectorscopeDecimated(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  maxPoints = 0,
  options: ValidateOptions = {},
): VectorscopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringVectorscopeDecimated', left, validate, 'left');
  assertSamples('meteringVectorscopeDecimated', right, validate, 'right');
  return requireModule().meteringVectorscopeDecimated(left, right, sampleRate, maxPoints);
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

/**
 * Display-sized phase scope. Like {@link meteringPhaseScope} but the point
 * series is deterministically decimated to at most `maxPoints` points (`0`, or
 * a value `>= length`, yields one point per input sample). The summary stats are
 * always computed over the full-resolution signal. Mirrors the Node/Python
 * decimated phase scope.
 */
export function meteringPhaseScopeDecimated(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  maxPoints = 0,
  options: ValidateOptions = {},
): PhaseScopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringPhaseScopeDecimated', left, validate, 'left');
  assertSamples('meteringPhaseScopeDecimated', right, validate, 'right');
  return requireModule().meteringPhaseScopeDecimated(left, right, sampleRate, maxPoints);
}

/**
 * Welch-averaged magnitude / power / dB spectrum over the WHOLE signal (split
 * into Hann-windowed, 50%-overlapping `nFft`-length frames whose power spectra
 * are averaged). For a true single-frame snapshot, use
 * {@link meteringSpectrumFrame}.
 */
export function meteringSpectrum(
  samples: Float32Array,
  sampleRate = 22050,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const validate = options?.validate !== false;
  assertSamples('meteringSpectrum', samples, validate);
  return requireModule().meteringSpectrum(samples, sampleRate, options ?? {});
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
  return requireModule().meteringSpectrumFrame(samples, sampleRate, frameOffset, options ?? {});
}

/** Compute per-channel min/max waveform buckets from interleaved audio. */
export function waveformPeaks(
  samples: Float32Array,
  channels: number,
  options: WaveformPeaksOptions = {},
): WaveformPeaksReport {
  assertSamples('waveformPeaks', samples, options.validate !== false);
  if (channels <= 0 || samples.length % channels !== 0) {
    throw new RangeError('waveformPeaks: samples length must be a multiple of channels');
  }
  const samplesPerBucket = options.samplesPerBucket ?? 512;
  if (samplesPerBucket <= 0) {
    throw new RangeError('waveformPeaks: samplesPerBucket must be > 0');
  }
  return requireModule().waveformPeaks(samples, channels, samplesPerBucket);
}

/** Compute waveform peak buckets for several zoom levels. */
export function waveformPeakPyramid(
  samples: Float32Array,
  channels: number,
  options: WaveformPeakPyramidOptions = {},
): WaveformPeaksReport[] {
  assertSamples('waveformPeakPyramid', samples, options.validate !== false);
  if (channels <= 0 || samples.length % channels !== 0) {
    throw new RangeError('waveformPeakPyramid: samples length must be a multiple of channels');
  }
  const levels = options.samplesPerBucketLevels ?? [512, 1024, 2048, 4096];
  if (levels.length === 0 || levels.some((level) => level <= 0)) {
    throw new RangeError('waveformPeakPyramid: samplesPerBucketLevels must be non-empty and > 0');
  }
  return requireModule().waveformPeakPyramid(samples, channels, levels);
}
