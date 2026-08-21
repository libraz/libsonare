import { ErrorCode, SonareError } from './errors';
import { getSonareModule } from './module_state';
import type { ValidateOptions } from './validation';
import { assertSamples } from './validation';

/**
 * Validates a true-peak oversample factor: `0` (meaning "use the default 4") or
 * a power of two in `[1, 16]`. The native layer applies the same `0 -> 4`
 * normalization (see `metering::true_peak_db`), so the raw factor can be passed
 * through unchanged after this check. Kept module-local (not exported) so it does
 * not surface as a WASM-only symbol with no C-API counterpart.
 */
function assertOversampleFactor(fnName: string, factor: number): void {
  const normalized = factor === 0 ? 4 : factor;
  if (
    !Number.isInteger(normalized) ||
    normalized < 1 ||
    normalized > 16 ||
    (normalized & (normalized - 1)) !== 0
  ) {
    throw new SonareError(
      ErrorCode.InvalidParameter,
      'InvalidParameter',
      `${fnName}: oversampleFactor must be 0 or a power of two from 1 to 16`,
    );
  }
}

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

/** Canonical request form for single-channel meter readings. */
export interface MeteringSamplesRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for true-peak analysis. */
export interface MeteringTruePeakRequest extends MeteringSamplesRequest {
  oversampleFactor?: number;
}

/** Canonical request form for clipping analysis. */
export interface MeteringDetectClippingRequest extends MeteringDetectClippingOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for dynamic-range analysis. */
export interface MeteringDynamicRangeRequest extends MeteringDynamicRangeOptions {
  samples: Float32Array;
  sampleRate?: number;
}

function requireModule() {
  return getSonareModule();
}

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
  return requireModule().meteringPeakDb(request.samples, request.sampleRate ?? 22050);
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
  return requireModule().meteringRmsDb(request.samples, request.sampleRate ?? 22050);
}

export interface MeteringSilenceRatioRequest extends MeteringSamplesRequest {
  thresholdDb?: number;
  frameLength?: number;
  hopLength?: number;
}

export function meteringSilenceRatio(request: MeteringSilenceRatioRequest): number;
export function meteringSilenceRatio(
  samples: Float32Array,
  sampleRate?: number,
  thresholdDb?: number,
  frameLength?: number,
  hopLength?: number,
  options?: ValidateOptions,
): number;
export function meteringSilenceRatio(
  samples: Float32Array | MeteringSilenceRatioRequest,
  sampleRate = 22050,
  thresholdDb = -45,
  frameLength = 1024,
  hopLength = 256,
  options: ValidateOptions = {},
): number {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, thresholdDb, frameLength, hopLength, ...options }
      : samples;
  assertSamples('meteringSilenceRatio', request.samples, request.validate !== false);
  return requireModule().meteringSilenceRatio(
    request.samples,
    request.sampleRate ?? 22050,
    request.thresholdDb ?? -45,
    request.frameLength ?? 1024,
    request.hopLength ?? 256,
  );
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
  return requireModule().meteringCrestFactorDb(request.samples, request.sampleRate ?? 22050);
}

/**
 * Crest factor in dB across both channels of a stereo pair.
 *
 * Takes the peak across both channels and the RMS over both together. An
 * out-of-phase pair cancels in the `0.5 * (left + right)` downmix
 * {@link meteringCrestFactorDb} would need, which understates its RMS and so
 * overstates the crest factor.
 */
export function meteringCrestFactorDbStereo(request: MeteringStereoRequest): number {
  assertSamples('meteringCrestFactorDbStereo', request.left, request.validate !== false);
  assertSamples('meteringCrestFactorDbStereo', request.right, request.validate !== false);
  return requireModule().meteringCrestFactorDbStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
  );
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
  return requireModule().meteringDcOffset(request.samples, request.sampleRate ?? 22050);
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
  const factor = request.oversampleFactor ?? 4;
  assertOversampleFactor('meteringTruePeakDb', factor);
  return requireModule().meteringTruePeakDb(request.samples, request.sampleRate ?? 22050, factor);
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
  const minRegionSamples = request.minRegionSamples ?? 1;
  if (!Number.isInteger(minRegionSamples) || minRegionSamples < 0) {
    throw new RangeError('meteringDetectClipping: minRegionSamples must be a non-negative integer');
  }
  return requireModule().meteringDetectClipping(
    request.samples,
    request.sampleRate ?? 22050,
    request.threshold ?? 0.999,
    minRegionSamples,
  );
}

/**
 * Sliding-window dynamic range for mono audio. Pass 0 for window/hop to use the library
 * default (window=3 s, hop=1 s). The percentiles use a NEGATIVE sentinel for
 * "use the library default" (low=0.10, high=0.95) because 0 is a literal 0th
 * percentile; omitted percentiles therefore default to -1.
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
  return requireModule().meteringDynamicRange(
    request.samples,
    request.sampleRate ?? 22050,
    request.windowSec ?? 0,
    request.hopSec ?? 0,
    request.lowPercentile ?? -1,
    request.highPercentile ?? -1,
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

/** Canonical request form for stereo meter readings. */
export interface MeteringStereoRequest extends ValidateOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for display-decimated stereo scopes. */
export interface MeteringStereoDecimatedRequest extends MeteringStereoRequest {
  maxPoints?: number;
}

/** Options for the scope functions (mirrors the Node `ScopeOptions`). */
export interface ScopeOptions extends ValidateOptions {
  /**
   * Upper bound on the returned point count. Omit / `0` (or a value `>= length`)
   * yields one point per input sample; otherwise the point cloud is
   * deterministically decimated to at most `maxPoints` points for display.
   */
  maxPoints?: number;
}

/** Canonical request form for whole-signal spectrum analysis. */
export interface MeteringSpectrumRequest extends SpectrumOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for a single spectrum frame. */
export interface MeteringSpectrumFrameRequest extends MeteringSpectrumRequest {
  frameOffset?: number;
}

/** Canonical request form for waveform bucket generation. */
export interface WaveformPeaksRequest extends WaveformPeaksOptions {
  samples: Float32Array;
  channels: number;
}

/** Canonical request form for multi-resolution waveform bucket generation. */
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

/** Uncentered correlation (cosine similarity) in [-1, 1] between equal-length channels. */
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
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringStereoCorrelation', request.left, validate, 'left');
  assertSamples('meteringStereoCorrelation', request.right, validate, 'right');
  return requireModule().meteringStereoCorrelation(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
  );
}

/**
 * Stereo width as `sqrt(side_energy / mid_energy)` in `[0, +Infinity)`: the
 * side/mid RMS *amplitude* ratio, not the energy ratio. 0 = pure mono, ~1 =
 * wide stereo, larger = increasingly decorrelated / out-of-phase. The value is
 * unbounded and returns `Infinity` when the mid channel is silent (a
 * mono-collapsed / fully out-of-phase signal).
 *
 * Convert to dB with `20 * Math.log10(value)`; `10 * Math.log10` would
 * understate the true energy ratio by half.
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
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringStereoWidth', request.left, validate, 'left');
  assertSamples('meteringStereoWidth', request.right, validate, 'right');
  return requireModule().meteringStereoWidth(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
  );
}

/**
 * Mid/side vectorscope point series. By default emits one point per input
 * sample; pass `maxPoints` to get a display-sized decimated point set (matching
 * the Node `meteringVectorscope` shape).
 */
export function meteringVectorscope(request: MeteringStereoDecimatedRequest): VectorscopeReport;
export function meteringVectorscope(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  options?: ScopeOptions,
): VectorscopeReport;
export function meteringVectorscope(
  left: Float32Array | MeteringStereoDecimatedRequest,
  right?: Float32Array,
  sampleRate = 22050,
  options: ScopeOptions = {},
): VectorscopeReport {
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringVectorscope', request.left, validate, 'left');
  assertSamples('meteringVectorscope', request.right, validate, 'right');
  return requireModule().meteringVectorscopeDecimated(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.maxPoints ?? 0,
  );
}

/**
 * Display-sized mid/side vectorscope.
 *
 * @deprecated Pass `maxPoints` to {@link meteringVectorscope} instead; it now
 * folds `maxPoints` into the request, matching the Node surface. This alias is
 * kept for backward compatibility and simply delegates.
 */
export function meteringVectorscopeDecimated(
  request: MeteringStereoDecimatedRequest,
): VectorscopeReport;
export function meteringVectorscopeDecimated(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  maxPoints?: number,
  options?: ValidateOptions,
): VectorscopeReport;
export function meteringVectorscopeDecimated(
  left: Float32Array | MeteringStereoDecimatedRequest,
  right?: Float32Array,
  sampleRate = 22050,
  maxPoints = 0,
  options: ValidateOptions = {},
): VectorscopeReport {
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, maxPoints, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringVectorscopeDecimated', request.left, validate, 'left');
  assertSamples('meteringVectorscopeDecimated', request.right, validate, 'right');
  return requireModule().meteringVectorscopeDecimated(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.maxPoints ?? 0,
  );
}

/**
 * Phase-scope point series plus summary stats. By default emits one point per
 * input sample; pass `maxPoints` to decimate the point cloud for display
 * (matching the Node `meteringPhaseScope` shape). The summary stats are always
 * computed over the full-resolution signal.
 */
export function meteringPhaseScope(request: MeteringStereoDecimatedRequest): PhaseScopeReport;
export function meteringPhaseScope(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  options?: ScopeOptions,
): PhaseScopeReport;
export function meteringPhaseScope(
  left: Float32Array | MeteringStereoDecimatedRequest,
  right?: Float32Array,
  sampleRate = 22050,
  options: ScopeOptions = {},
): PhaseScopeReport {
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringPhaseScope', request.left, validate, 'left');
  assertSamples('meteringPhaseScope', request.right, validate, 'right');
  return requireModule().meteringPhaseScopeDecimated(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.maxPoints ?? 0,
  );
}

/**
 * Display-sized phase scope.
 *
 * @deprecated Pass `maxPoints` to {@link meteringPhaseScope} instead; it now
 * folds `maxPoints` into the request, matching the Node surface. This alias is
 * kept for backward compatibility and simply delegates.
 */
export function meteringPhaseScopeDecimated(
  request: MeteringStereoDecimatedRequest,
): PhaseScopeReport;
export function meteringPhaseScopeDecimated(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  maxPoints?: number,
  options?: ValidateOptions,
): PhaseScopeReport;
export function meteringPhaseScopeDecimated(
  left: Float32Array | MeteringStereoDecimatedRequest,
  right?: Float32Array,
  sampleRate = 22050,
  maxPoints = 0,
  options: ValidateOptions = {},
): PhaseScopeReport {
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, maxPoints, ...options }
      : left;
  const validate = request.validate !== false;
  assertSamples('meteringPhaseScopeDecimated', request.left, validate, 'left');
  assertSamples('meteringPhaseScopeDecimated', request.right, validate, 'right');
  return requireModule().meteringPhaseScopeDecimated(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.maxPoints ?? 0,
  );
}

/**
 * Welch-averaged magnitude / power / dB spectrum over the WHOLE mono signal (split
 * into Hann-windowed, 50%-overlapping `nFft`-length frames whose power spectra
 * are averaged). For a true single-frame snapshot, use
 * {@link meteringSpectrumFrame}.
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
  options: SpectrumOptions & ValidateOptions = {},
): SpectrumReport {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('meteringSpectrum', request.samples, request.validate !== false);
  return requireModule().meteringSpectrum(request.samples, request.sampleRate ?? 22050, request);
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
  options: SpectrumOptions & ValidateOptions = {},
): SpectrumReport {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, frameOffset, ...options } : samples;
  assertSamples('meteringSpectrumFrame', request.samples, request.validate !== false);
  return requireModule().meteringSpectrumFrame(
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
  channels?: number,
  options: WaveformPeaksOptions = {},
): WaveformPeaksReport {
  const request =
    samples instanceof Float32Array
      ? { samples, channels: channels as number, ...options }
      : samples;
  assertSamples('waveformPeaks', request.samples, request.validate !== false);
  if (request.channels <= 0 || request.samples.length % request.channels !== 0) {
    throw new RangeError('waveformPeaks: samples length must be a multiple of channels');
  }
  const samplesPerBucket = request.samplesPerBucket ?? 512;
  if (samplesPerBucket <= 0) {
    throw new RangeError('waveformPeaks: samplesPerBucket must be > 0');
  }
  return requireModule().waveformPeaks(request.samples, request.channels, samplesPerBucket);
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
  channels?: number,
  options: WaveformPeakPyramidOptions = {},
): WaveformPeaksReport[] {
  const request =
    samples instanceof Float32Array
      ? { samples, channels: channels as number, ...options }
      : samples;
  assertSamples('waveformPeakPyramid', request.samples, request.validate !== false);
  if (request.channels <= 0 || request.samples.length % request.channels !== 0) {
    throw new RangeError('waveformPeakPyramid: samples length must be a multiple of channels');
  }
  const levels = request.samplesPerBucketLevels ?? [512, 1024, 2048, 4096];
  if (levels.length === 0 || levels.some((level) => level <= 0)) {
    throw new RangeError('waveformPeakPyramid: samplesPerBucketLevels must be non-empty and > 0');
  }
  return requireModule().waveformPeakPyramid(request.samples, request.channels, levels);
}
