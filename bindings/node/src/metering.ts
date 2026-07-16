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

/** Sliding-window dynamic range report (mirrors C SonareDynamicRangeResult). */
export interface DynamicRangeReport {
  dynamicRangeDb: number;
  lowPercentileDb: number;
  highPercentileDb: number;
  windowRmsDb: Float32Array;
}

/** Common input for one-shot mono metering functions. */
export interface MeteringSamplesRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface MeteringTruePeakRequest extends MeteringSamplesRequest {
  oversampleFactor?: number;
}

export interface MeteringDetectClippingRequest
  extends MeteringSamplesRequest,
    MeteringDetectClippingOptions {}

export interface MeteringDynamicRangeRequest
  extends MeteringSamplesRequest,
    MeteringDynamicRangeOptions {}

export function meteringPeakDb(request: MeteringSamplesRequest): number;
export function meteringPeakDb(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): number;
export function meteringPeakDb(
  samples: Float32Array | MeteringSamplesRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringPeakDb', request.samples, request.validate !== false);
  return addon.meteringPeakDb(request.samples, request.sampleRate ?? 22050);
}

export function meteringRmsDb(request: MeteringSamplesRequest): number;
export function meteringRmsDb(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): number;
export function meteringRmsDb(
  samples: Float32Array | MeteringSamplesRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringRmsDb', request.samples, request.validate !== false);
  return addon.meteringRmsDb(request.samples, request.sampleRate ?? 22050);
}

export function meteringCrestFactorDb(request: MeteringSamplesRequest): number;
export function meteringCrestFactorDb(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): number;
export function meteringCrestFactorDb(
  samples: Float32Array | MeteringSamplesRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringCrestFactorDb', request.samples, request.validate !== false);
  return addon.meteringCrestFactorDb(request.samples, request.sampleRate ?? 22050);
}

export function meteringDcOffset(request: MeteringSamplesRequest): number;
export function meteringDcOffset(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): number;
export function meteringDcOffset(
  samples: Float32Array | MeteringSamplesRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringDcOffset', request.samples, request.validate !== false);
  return addon.meteringDcOffset(request.samples, request.sampleRate ?? 22050);
}

/**
 * Inter-sample (true) peak in dBFS. `oversampleFactor` must be a power of two
 * in [1, 16]; pass 0 to use the library default (4).
 */
export function meteringTruePeakDb(request: MeteringTruePeakRequest): number;
export function meteringTruePeakDb(
  samples: Float32Array,
  sampleRate?: number,
  oversampleFactor?: number,
  options?: ValidateOptions,
): number;
export function meteringTruePeakDb(
  samples: Float32Array | MeteringTruePeakRequest,
  sampleRate = 22050,
  oversampleFactor = 4,
  options: ValidateOptions = {},
): number {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, oversampleFactor, ...options }
      : samples;
  assertSamples('meteringTruePeakDb', request.samples, request.validate !== false);
  return addon.meteringTruePeakDb(
    request.samples,
    request.sampleRate ?? 22050,
    request.oversampleFactor ?? 4,
  );
}

/**
 * Detect contiguous runs of clipped samples.
 *
 * @param threshold Linear absolute threshold (default 0.999).
 * @param minRegionSamples Minimum run length to report (default 1).
 */
export function meteringDetectClipping(request: MeteringDetectClippingRequest): ClippingReport;
export function meteringDetectClipping(
  samples: Float32Array,
  sampleRate?: number,
  options?: MeteringDetectClippingOptions,
): ClippingReport;
export function meteringDetectClipping(
  samples: Float32Array | MeteringDetectClippingRequest,
  sampleRate = 22050,
  options: MeteringDetectClippingOptions = {},
): ClippingReport {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringDetectClipping', request.samples, request.validate !== false);
  return addon.meteringDetectClipping(
    request.samples,
    request.sampleRate ?? 22050,
    request.threshold ?? 0.999,
    request.minRegionSamples ?? 1,
  );
}

/**
 * Sliding-window dynamic range (high_percentile_db - low_percentile_db).
 * Pass 0 for window/hop to use the library default (window=3 s, hop=1 s). The
 * percentiles use a NEGATIVE sentinel for "use the library default" (low=0.10,
 * high=0.95) because 0 is a literal 0th percentile; omitted percentiles default
 * to -1.
 */
export function meteringDynamicRange(request: MeteringDynamicRangeRequest): DynamicRangeReport;
export function meteringDynamicRange(
  samples: Float32Array,
  sampleRate?: number,
  options?: MeteringDynamicRangeOptions,
): DynamicRangeReport;
export function meteringDynamicRange(
  samples: Float32Array | MeteringDynamicRangeRequest,
  sampleRate = 22050,
  options: MeteringDynamicRangeOptions = {},
): DynamicRangeReport {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringDynamicRange', request.samples, request.validate !== false);
  return addon.meteringDynamicRange(
    request.samples,
    request.sampleRate ?? 22050,
    request.windowSec ?? 0,
    request.hopSec ?? 0,
    request.lowPercentile ?? -1,
    request.highPercentile ?? -1,
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

/** Common input for one-shot stereo metering functions. */
export interface MeteringStereoRequest extends ValidateOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

export interface MeteringScopeRequest extends MeteringStereoRequest, ScopeOptions {}
export interface MeteringSpectrumRequest extends MeteringSamplesRequest, SpectrumOptions {}
export interface MeteringSpectrumFrameRequest extends MeteringSpectrumRequest {
  frameOffset?: number;
}
export interface WaveformPeaksRequest extends WaveformPeaksOptions {
  samples: Float32Array;
  channels: number;
}
export interface WaveformPeakPyramidRequest extends WaveformPeakPyramidOptions {
  samples: Float32Array;
  channels: number;
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
export function meteringStereoCorrelation(request: MeteringStereoRequest): number;
export function meteringStereoCorrelation(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): number;
export function meteringStereoCorrelation(
  left: Float32Array | MeteringStereoRequest,
  right?: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const request: MeteringStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringStereoCorrelation', request.left, validate, 'left');
  assertSamples('meteringStereoCorrelation', request.right, validate, 'right');
  return addon.meteringStereoCorrelation(request.left, request.right, request.sampleRate ?? 22050);
}

/**
 * Side / mid energy ratio, clamped to `[0, 2]`: 0 = pure mono, ~1 = wide
 * stereo, 2 = fully decorrelated / out-of-phase.
 */
export function meteringStereoWidth(request: MeteringStereoRequest): number;
export function meteringStereoWidth(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): number;
export function meteringStereoWidth(
  left: Float32Array | MeteringStereoRequest,
  right?: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const request: MeteringStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringStereoWidth', request.left, validate, 'left');
  assertSamples('meteringStereoWidth', request.right, validate, 'right');
  return addon.meteringStereoWidth(request.left, request.right, request.sampleRate ?? 22050);
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
export function meteringVectorscope(request: MeteringScopeRequest): VectorscopeReport;
export function meteringVectorscope(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  options?: ScopeOptions,
): VectorscopeReport;
export function meteringVectorscope(
  left: Float32Array | MeteringScopeRequest,
  right?: Float32Array,
  sampleRate = 22050,
  options: ScopeOptions = {},
): VectorscopeReport {
  const request: MeteringScopeRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringVectorscope', request.left, validate, 'left');
  assertSamples('meteringVectorscope', request.right, validate, 'right');
  return addon.meteringVectorscope(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.maxPoints ?? 0,
  );
}

/**
 * Phase-scope point series plus summary stats. By default emits one point per
 * input sample; pass `maxPoints` to decimate the point cloud for display. The
 * summary stats are always computed over the full-resolution signal.
 */
export function meteringPhaseScope(request: MeteringScopeRequest): PhaseScopeReport;
export function meteringPhaseScope(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  options?: ScopeOptions,
): PhaseScopeReport;
export function meteringPhaseScope(
  left: Float32Array | MeteringScopeRequest,
  right?: Float32Array,
  sampleRate = 22050,
  options: ScopeOptions = {},
): PhaseScopeReport {
  const request: MeteringScopeRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringPhaseScope', request.left, validate, 'left');
  assertSamples('meteringPhaseScope', request.right, validate, 'right');
  return addon.meteringPhaseScope(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.maxPoints ?? 0,
  );
}

/**
 * Welch-averaged magnitude / power / dB spectrum over the WHOLE signal. This is
 * NOT a single-frame snapshot: the signal is split into Hann-windowed,
 * 50%-overlapping `nFft`-length frames whose power spectra are averaged across
 * the entire input, so transients are smeared by the averaging. For a true
 * single-frame FFT of one window, use {@link meteringSpectrumFrame}.
 */
export function meteringSpectrum(request: MeteringSpectrumRequest): SpectrumReport;
export function meteringSpectrum(
  samples: Float32Array,
  sampleRate?: number,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport;
export function meteringSpectrum(
  samples: Float32Array | MeteringSpectrumRequest,
  sampleRate = 22050,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const validate = request.validate !== false;
  assertSamples('meteringSpectrum', request.samples, validate);
  return addon.meteringSpectrum(request.samples, request.sampleRate ?? 22050, request);
}

/**
 * True single-frame magnitude / power / dB spectrum (one Hann-windowed
 * `nFft`-length FFT), for spectrum-analyzer "moment" snapshots that must not be
 * time-averaged like {@link meteringSpectrum}. The analysis frame spans
 * `[frameOffset, frameOffset + nFft)`; samples past the end are zero-padded.
 */
export function meteringSpectrumFrame(request: MeteringSpectrumFrameRequest): SpectrumReport;
export function meteringSpectrumFrame(
  samples: Float32Array,
  sampleRate?: number,
  frameOffset?: number,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport;
export function meteringSpectrumFrame(
  samples: Float32Array | MeteringSpectrumFrameRequest,
  sampleRate = 22050,
  frameOffset = 0,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, frameOffset, ...options } : samples;
  const validate = request.validate !== false;
  assertSamples('meteringSpectrumFrame', request.samples, validate);
  return addon.meteringSpectrumFrame(
    request.samples,
    request.sampleRate ?? 22050,
    request.frameOffset ?? 0,
    request,
  );
}

/** Compute per-channel min/max waveform buckets from interleaved audio. */
export function waveformPeaks(request: WaveformPeaksRequest): WaveformPeaksReport;
export function waveformPeaks(
  samples: Float32Array,
  channels: number,
  options?: WaveformPeaksOptions,
): WaveformPeaksReport;
export function waveformPeaks(
  samples: Float32Array | WaveformPeaksRequest,
  channels = 0,
  options: WaveformPeaksOptions = {},
): WaveformPeaksReport {
  const request = samples instanceof Float32Array ? { samples, channels, ...options } : samples;
  assertSamples('waveformPeaks', request.samples, request.validate !== false);
  if (request.channels <= 0 || request.samples.length % request.channels !== 0) {
    throw new RangeError('waveformPeaks: samples length must be a multiple of channels');
  }
  const samplesPerBucket = request.samplesPerBucket ?? 512;
  if (samplesPerBucket <= 0) {
    throw new RangeError('waveformPeaks: samplesPerBucket must be > 0');
  }
  return addon.waveformPeaks(request.samples, request.channels, samplesPerBucket);
}

/** Compute waveform peak buckets for several zoom levels. */
export function waveformPeakPyramid(request: WaveformPeakPyramidRequest): WaveformPeaksReport[];
export function waveformPeakPyramid(
  samples: Float32Array,
  channels: number,
  options?: WaveformPeakPyramidOptions,
): WaveformPeaksReport[];
export function waveformPeakPyramid(
  samples: Float32Array | WaveformPeakPyramidRequest,
  channels = 0,
  options: WaveformPeakPyramidOptions = {},
): WaveformPeaksReport[] {
  const request = samples instanceof Float32Array ? { samples, channels, ...options } : samples;
  assertSamples('waveformPeakPyramid', request.samples, request.validate !== false);
  if (request.channels <= 0 || request.samples.length % request.channels !== 0) {
    throw new RangeError('waveformPeakPyramid: samples length must be a multiple of channels');
  }
  const levels = request.samplesPerBucketLevels ?? [512, 1024, 2048, 4096];
  if (levels.length === 0 || levels.some((level) => level <= 0)) {
    throw new RangeError('waveformPeakPyramid: samplesPerBucketLevels must be non-empty and > 0');
  }
  return addon.waveformPeakPyramid(request.samples, request.channels, levels);
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `referenceMidi` defaults to A4 (69) when passed as 0.
 */
